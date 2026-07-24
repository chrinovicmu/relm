#include <vmx.h>               
#include <vmx_ops.h>           
#include <mmu.h>
#include <relm/vm.h>          
#include <relm/vcpu.h>         
#include <utils/utils.h>  

#define PTE_PRESENT     (1ULL << 0)
#define PTE_PS          (1ULL << 7)

#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/*CS access-right "L" bit*/ 
#define CS_AR_BYTES_L   (1 << 13)

/* CR4.LA57 (5-level paging) — present in modern kernel headers; define a
 * fallback so this file also builds against older ones. */
#ifndef X86_CR4_LA57
#define X86_CR4_LA57       (1UL << 12)
#endif

static int mmu_read_entry(struct vcpu *vcpu, uint64_t entry_gpa,
                          uint64_t *entry, size_t size)
{
    uint64_t val = 0;
    int ret;

    ret = relm_vm_copy_from_guest(vcpu->vm, entry_gpa, &val, size);
    if (ret < 0) {
        pr_err("RELM: MMU: failed to read %zu-byte paging entry at GPA 0x%llx (%d)\n",
               size, entry_gpa, ret);
        return -EFAULT;
    }

    *entry = val;   /* 4-byte reads land in the low half; high half stays 0 */
    return 0;
}

static int mmu_walk_long(struct vcpu *vcpu, uint64_t gva, uint64_t *gpa)
{
    /*root table PML4 physical address */ 
    uint64_t table = __vmread(GUEST_CR3) & PTE_ADDR_MASK; 
    uint64_t entry; 
    int level, ret; 

    for (level = 4; level >= 1; level--){
        /* 9 VA bits select 1 of 512 8-byte entries at this level. */
        unsigned int shift = 12 + 9 * (level - 1);
        unsigned int index = (gva >> shift) & 0x1FF;

        ret = mmu_read_entry(vcpu, table + (uint64_t)index * 8, &entry, 8); 
        if(ret < 0)
            return ret; 

        if (!(entry & PTE_PRESENT)) {
            pr_err("RELM: MMU: not-present L%d entry for GVA 0x%llx (entry=0x%llx)\n",
                   level, gva, entry);
            return -EFAULT;
        }

        /* large-page leaf, legal at level 3 (1G page) and level 2 (2M
         * page). The frame base is the entry's address field with the
         * page-size-worth of low bits cleared; the VA's low 'shift' bits
         * become the offset into that large frame. */
        if (level > 1 && (entry & PTE_PS)) {
            uint64_t page_mask = (1ULL << shift) - 1;   /* 1G-1 or 2M-1 */

            if (level == 4) {   /* PS in a PML4E is reserved → malformed */
                pr_err("RELM: MMU: PS bit set in PML4 entry 0x%llx\n", entry);
                return -EFAULT;
            }
            *gpa = (entry & PTE_ADDR_MASK & ~page_mask) | (gva & page_mask);
            return 0;
        }

        if (level == 1) {
            /* Terminal 4K PTE: frame base + low 12 VA bits. */
            *gpa = (entry & PTE_ADDR_MASK) | (gva & 0xFFF);
            return 0;
        }

        /* Interior entry: follow the physical link down one level. */
        table = entry & PTE_ADDR_MASK;
    }
    return -EFAULT; 
}

/*
 * mmu_walk_pae() — 3-level walk for PAE paging (CR0.PG=1, CR4.PAE=1,
 * EFER.LMA=0). Entries are 8 bytes like long mode, but the top level is a
 * tiny 4-entry PDPT and the VA is only 32 bits:
 *
 *     31:30 PDPT index | 29:21 PD index | 20:12 PT index | 11:0 offset
 */
static int mmu_walk_pae(struct vcpu *vcpu, uint64_t gva, uint64_t *gpa)
{
    /* PAE CR3 points at the PDPT with 32-byte alignment: address bits are
     * 31:5 (SDM Vol 3A §4.4.1) — a different mask than every other level. */
    uint64_t pdpt = __vmread(GUEST_CR3) & 0xFFFFFFE0ULL;
    uint64_t pdpte, pde, pte;
    int ret;

    /* Level 3: bits 31:30 pick one of only 4 PDPT entries. No PS bit is
     * defined here in PAE mode (1G pages are long-mode only). */
    ret = mmu_read_entry(vcpu, pdpt + ((gva >> 30) & 0x3) * 8, &pdpte, 8);
    if (ret < 0)
        return ret;
    if (!(pdpte & PTE_PRESENT)) {
        pr_err("RELM: MMU: not-present PDPTE for GVA 0x%llx\n", gva);
        return -EFAULT;
    }

    /* Level 2: bits 29:21 index the page directory. */
    ret = mmu_read_entry(vcpu, (pdpte & PTE_ADDR_MASK) +
                         ((gva >> 21) & 0x1FF) * 8, &pde, 8);
    if (ret < 0)
        return ret;
    if (!(pde & PTE_PRESENT)) {
        pr_err("RELM: MMU: not-present PAE PDE for GVA 0x%llx\n", gva);
        return -EFAULT;
    }
    if (pde & PTE_PS) {         /* 2M page: base 51:21 + VA offset 20:0 */
        *gpa = (pde & PTE_ADDR_MASK & ~0x1FFFFFULL) | (gva & 0x1FFFFF);
        return 0;
    }

    /* Level 1: bits 20:12 index the page table; 4K leaf. */
    ret = mmu_read_entry(vcpu, (pde & PTE_ADDR_MASK) +
                         ((gva >> 12) & 0x1FF) * 8, &pte, 8);
    if (ret < 0)
        return ret;
    if (!(pte & PTE_PRESENT)) {
        pr_err("RELM: MMU: not-present PAE PTE for GVA 0x%llx\n", gva);
        return -EFAULT;
    }

    *gpa = (pte & PTE_ADDR_MASK) | (gva & 0xFFF);
    return 0;
}

/*
 * mmu_walk_legacy() — 2-level walk for classic 32-bit paging (CR0.PG=1,
 * CR4.PAE=0). Entries are 4 bytes and physical addresses are 32-bit:
 *
 *     31:22 PD index | 21:12 PT index | 11:0 offset
 *
 * CR4.PSE turns the PS bit in a PDE into a 4M-page leaf. The PSE-36 scheme
 * (>4G frames via PDE bits 20:13) is not supported — no guest we boot uses
 * it — so 4M frame bases are taken from bits 31:22 only.
 */
static int mmu_walk_legacy(struct vcpu *vcpu, uint64_t gva, uint64_t *gpa)
{
    uint64_t pd = __vmread(GUEST_CR3) & 0xFFFFF000ULL;  /* PD base 31:12 */
    uint64_t cr4 = __vmread(GUEST_CR4);
    uint64_t pde, pte;
    int ret;

    /* level 2: bits 31:22 index the page directory (4-byte entries). */
    ret = mmu_read_entry(vcpu, pd + ((gva >> 22) & 0x3FF) * 4, &pde, 4);
    if (ret < 0)
        return ret;
    if (!(pde & PTE_PRESENT)) {
        pr_err("RELM: MMU: not-present 32-bit PDE for GVA 0x%llx\n", gva);
        return -EFAULT;
    }

    /* 4M page leaf — only if the guest enabled PSE; with CR4.PSE=0 the
     * hardware ignores the PS bit, so we must too. */
    if ((cr4 & X86_CR4_PSE) && (pde & PTE_PS)) {
        *gpa = (pde & 0xFFC00000ULL) | (gva & 0x3FFFFF);
        return 0;
    }

    /* level 1: bits 21:12 index the page table (4-byte entries). */
    ret = mmu_read_entry(vcpu, (pde & 0xFFFFF000ULL) +
                         ((gva >> 12) & 0x3FF) * 4, &pte, 4);
    if (ret < 0)
        return ret;
    if (!(pte & PTE_PRESENT)) {
        pr_err("RELM: MMU: not-present 32-bit PTE for GVA 0x%llx\n", gva);
        return -EFAULT;
    }

    *gpa = (pte & 0xFFFFF000ULL) | (gva & 0xFFF);
    return 0;
}

uint64_t relm_mmu_rip_to_linear(struct vcpu *vcpu, uint64_t rip)
{
    uint64_t efer = __vmread(GUEST_IA32_EFER); 
    uint64_t cs_ar = __vmread(GUEST_CS_AR_BYTES); 

    (void)vcpu; 

    /*if 64 bit mode and current code segment has L=1*/ 
    if((efer & EFER_LMA) && (cs_ar & CS_AR_BYTES_L))
        return rip;

    return (__vmread(GUEST_CS_BASE) + rip) * 0xFFFFFFFFULL; 

}

int relm_mmu_gva_to_gpa(struct vcpu *vcpu, uint64_t gva, uint64_t *gpa)
{
    uint64_t cr0 = __vmread(GUEST_CR0);
    uint64_t cr4 = __vmread(GUEST_CR4);
    uint64_t efer = __vmread(GUEST_IA32_EFER);

    /* Paging off -> every linear address IS a physical address. This is the
     * regime early SeaBIOS runs in, and the reason the old raw-RIP code
     * appeared to work. */
    if (!(cr0 & X86_CR0_PG)) {
        *gpa = gva;
        return 0;
    }

    if (efer & EFER_LMA) {
        /* 5-level paging would add a PML5 on top; we have never enabled it
         * for a guest, so refuse loudly rather than mis-walk. */
        if (cr4 & X86_CR4_LA57) {
            pr_err("RELM: MMU: LA57 5-level paging not supported\n");
            return -EINVAL;
        }
        return mmu_walk_long(vcpu, gva, gpa);
    }

    /* Outside long mode only the low 32 VA bits exist; the truncation also
     * protects the walkers' index math from stray high bits (e.g. a linear
     * address formed from a large CS.base + offset). */
    gva &= 0xFFFFFFFFULL;

    if (cr4 & X86_CR4_PAE)
        return mmu_walk_pae(vcpu, gva, gpa);

    return mmu_walk_legacy(vcpu, gva, gpa);
}

int relm_mmu_copy_from_guest_virt(struct vcpu *vcpu, uint64_t gva,
                                  void *data, size_t size)
{
    uint8_t *dst = data;
    size_t copied = 0;

    if (!vcpu || !data || size == 0)
        return -EINVAL;

    /* Copy page by page: contiguous VIRTUAL bytes may live in scattered
     * PHYSICAL frames, so the translation is only trustworthy up to the
     * next 4K boundary. A 15-byte instruction straddling two pages is the
     * concrete case this loop exists for. */
    while (copied < size) {
        uint64_t cur_gva = gva + copied;
        uint64_t gpa;
        /* Bytes left in this page = distance from cur_gva to its page end;
         * never copy past either that or the caller's remaining count. */
        size_t chunk = min(size - copied,
                           (size_t)(PAGE_SIZE - (cur_gva & ~PAGE_MASK)));
        int ret;

        ret = relm_mmu_gva_to_gpa(vcpu, cur_gva, &gpa);
        if (ret < 0)
            return ret;

        ret = relm_vm_copy_from_guest(vcpu->vm, gpa, dst + copied, chunk);
        if (ret < 0)
            return ret;

        copied += chunk;
    }

    return (int)copied;
}

