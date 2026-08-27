
/* mmu.c — software guest page-table walker for the SVM backend.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>       
#include <asm/processor.h> 
#include <asm/msr.h>           
#include <vmcb.h>               
#include <mmu.h>
#include <relm/vm.h>        
#include <relm/vcpu.h>          
#include <utils/utils.h>                                    

#define PTE_PRESENT        (1ULL << 0)
#define PTE_PS             (1ULL << 7)

#define PTE_ADDR_MASK      0x000FFFFFFFFFF000ULL

#ifndef X86_CR4_LA57
#define X86_CR4_LA57       (1UL << 12)
#endif

/*
 * mmu_read_entry() — fetch one paging-structure entry from guest RAM.
 */
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

/* mmu_walk_long() — 4-level walk for long-mode paging (EFER.LMA=1).*/
static int mmu_walk_long(struct vcpu *vcpu, uint64_t cr3, uint64_t gva,
                         uint64_t *gpa)
{
    uint64_t table = cr3 & PTE_ADDR_MASK;
    uint64_t entry;
    int level, ret;

    for (level = 4; level >= 1; level--) {
        unsigned int shift = 12 + 9 * (level - 1);
        unsigned int index = (gva >> shift) & 0x1FF;

        ret = mmu_read_entry(vcpu, table + (uint64_t)index * 8, &entry, 8);
        if (ret < 0)
            return ret;

        if (!(entry & PTE_PRESENT)) {
            pr_err("RELM: MMU: not-present L%d entry for GVA 0x%llx (entry=0x%llx)\n",
                   level, gva, entry);
            return -EFAULT;
        }

        if (level > 1 && (entry & PTE_PS)) {
            uint64_t page_mask = (1ULL << shift) - 1;

            if (level == 4) {
                pr_err("RELM: MMU: PS bit set in PML4 entry 0x%llx\n", entry);
                return -EFAULT;
            }
            *gpa = (entry & PTE_ADDR_MASK & ~page_mask) | (gva & page_mask);
            return 0;
        }

        if (level == 1) {
            *gpa = (entry & PTE_ADDR_MASK) | (gva & 0xFFF);
            return 0;
        }

        table = entry & PTE_ADDR_MASK;
    }

    return -EFAULT;     
}

/*mmu_walk_pae() — 3-level walk for PAE paging (CR0.PG=1, CR4.PAE=1,*/

static int mmu_walk_pae(struct vcpu *vcpu, uint64_t pdpt, uint64_t gva,
                        uint64_t *gpa)
{
    uint64_t pdpte, pde, pte;
    int ret;

    ret = mmu_read_entry(vcpu, pdpt + ((gva >> 30) & 0x3) * 8, &pdpte, 8);
    if (ret < 0)
        return ret;
    if (!(pdpte & PTE_PRESENT)) {
        pr_err("RELM: MMU: not-present PDPTE for GVA 0x%llx\n", gva);
        return -EFAULT;
    }

    ret = mmu_read_entry(vcpu, (pdpte & PTE_ADDR_MASK) +
                         ((gva >> 21) & 0x1FF) * 8, &pde, 8);
    if (ret < 0)
        return ret;
    if (!(pde & PTE_PRESENT)) {
        pr_err("RELM: MMU: not-present PAE PDE for GVA 0x%llx\n", gva);
        return -EFAULT;
    }
    if (pde & PTE_PS) {
        *gpa = (pde & PTE_ADDR_MASK & ~0x1FFFFFULL) | (gva & 0x1FFFFF);
        return 0;
    }

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

/* mmu_walk_legacy() — 2-level walk for classic 32-bit paging (CR0.PG=1,*/ 
static int mmu_walk_legacy(struct vcpu *vcpu, uint64_t pd, uint64_t cr4,
                           uint64_t gva, uint64_t *gpa)
{
    uint64_t pde, pte;
    int ret;

    ret = mmu_read_entry(vcpu, pd + ((gva >> 22) & 0x3FF) * 4, &pde, 4);
    if (ret < 0)
        return ret;
    if (!(pde & PTE_PRESENT)) {
        pr_err("RELM: MMU: not-present 32-bit PDE for GVA 0x%llx\n", gva);
        return -EFAULT;
    }

    if ((cr4 & X86_CR4_PSE) && (pde & PTE_PS)) {
        *gpa = (pde & 0xFFC00000ULL) | (gva & 0x3FFFFF);
        return 0;
    }

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
    uint64_t efer  = vcpu->arch.vmcb->save.efer;
    uint16_t cs_ar = vcpu->arch.vmcb->save.cs.attrib;

    /* 64-bit mode = long mode active AND the current code segment has L=1
     * (SVM_SEG_ATTRIB_L, vmcb.h — the compressed-attribute counterpart of
     * VMX's CS_AR_BYTES bit 13). There the CPU treats CS.base as 0
     * regardless of the descriptor, so RIP already IS the linear address. */
    if ((efer & EFER_LMA) && (cs_ar & SVM_SEG_ATTRIB_L))
        return rip;

    /* Real / protected / compat mode: linear = CS.base + offset, wrapped
     * at 32 bits — same as the VMX version, sourced from
     * vmcb->save.cs.base instead of GUEST_CS_BASE. */
    return (vcpu->arch.vmcb->save.cs.base + rip) & 0xFFFFFFFFULL;
}

int relm_mmu_gva_to_gpa(struct vcpu *vcpu, uint64_t gva, uint64_t *gpa)
{
    struct vmcb_save_area *save = &vcpu->arch.vmcb->save;
    uint64_t cr0  = save->cr0;
    uint64_t cr3  = save->cr3;
    uint64_t cr4  = save->cr4;
    uint64_t efer = save->efer;

    /* Paging off → every linear address IS a physical address. */
    if (!(cr0 & X86_CR0_PG)) {
        *gpa = gva;
        return 0;
    }

    if (efer & EFER_LMA) {
        if (cr4 & X86_CR4_LA57) {
            pr_err("RELM: MMU: LA57 5-level paging not supported\n");
            return -EINVAL;
        }
        return mmu_walk_long(vcpu, cr3, gva, gpa);
    }

    /* Outside long mode only the low 32 VA bits exist; the truncation also
     * protects the walkers' index math from stray high bits. */
    gva &= 0xFFFFFFFFULL;

    if (cr4 & X86_CR4_PAE)
        return mmu_walk_pae(vcpu, cr3 & 0xFFFFFFE0ULL, gva, gpa);

    return mmu_walk_legacy(vcpu, cr3 & 0xFFFFF000ULL, cr4, gva, gpa);
}

int relm_mmu_copy_from_guest_virt(struct vcpu *vcpu, uint64_t gva,
                                  void *data, size_t size)
{
    uint8_t *dst = data;
    size_t copied = 0;

    if (!vcpu || !data || size == 0)
        return -EINVAL;

    while (copied < size) {
        uint64_t cur_gva = gva + copied;
        uint64_t gpa;
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
