#include <linux/slab.h>      
#include <linux/gfp.h>       
#include <linux/mm.h>        
#include <linux/spinlock.h>  
#include <linux/errno.h>     
#include <asm/io.h>          
#include <asm/processor.h>   
#include <relm/vm.h>         
#include <relm/vcpu.h>       
#include <npt.h>             
#include <vmcb.h>           
#include <utils/utils.h>

static int svm_mem_setup(struct relm_vm *vm)
{
    return relm_setup_npt(vm);
}

static int svm_mem_map_page(struct relm_vm *vm,
                            uint64_t gpa, uint64_t hpa, uint64_t flags)
{
    uint64_t npt_flags = NPT_PRESENT | NPT_USER;

    if (flags & RELM_MEM_F_WRITE)
        npt_flags |= NPT_WRITABLE;

    if (!(flags & RELM_MEM_F_EXEC))
        npt_flags |= NPT_NX;

    if (flags & RELM_MEM_F_MMIO)
        npt_flags |= NPT_MEMTYPE_UC;
    else
        npt_flags |= NPT_MEMTYPE_WB;    /* 0 — spelled out for symmetry */

    return relm_npt_map_page(vm->arch.npt, gpa, hpa, npt_flags);
}

static void svm_mem_unmap_page(struct relm_vm *vm, uint64_t gpa)
{
    relm_npt_unmap_page(vm->arch.npt, gpa);
}

static int svm_mem_create_guest_page_tables(struct relm_vm *vm)
{
    return relm_npt_create_guest_page_tables(vm);
}

static void svm_mem_invalidate(struct relm_vm *vm)
{
    relm_npt_invalidate(vm);
}
static void svm_mem_destroy(struct relm_vm *vm)
{
    if (vm->arch.npt) {
        relm_npt_context_destroy(vm->arch.npt);
        vm->arch.npt = NULL;    /* dangling-pointer hygiene, as in ept.c */
    }
}

const struct relm_mem_ops svm_mem_ops = {
    .setup                    = svm_mem_setup,
    .map_page                 = svm_mem_map_page,
    .unmap_page               = svm_mem_unmap_page,
    .create_guest_page_tables = svm_mem_create_guest_page_tables,
    .invalidate               = svm_mem_invalidate,
    .destroy                  = svm_mem_destroy,
}

bool relm_npt_check_support(void)
{
    uint32_t ecx;   /* 0x8000_0001 ECX — extended feature flags */
    uint32_t edx;   /* 0x8000_000A EDX — SVM sub-feature flags  */

    /* No SVM => leaf 0x8000_000A is undefined; must gate on this first. */
    ecx = cpuid_ecx(SVM_CPUID_EXT_FEATURES);
    if (!(ecx & SVM_CPUID_FEATURE_BIT)) {
        pr_err("RELM: SVM not supported by this CPU\n");
        return false;
    }

    /* SVM feature leaf: the hard requirement is the NP bit. */
    edx = cpuid_edx(SVM_CPUID_SVM_FEATURES);
    if (!(edx & SVM_FEAT_NPT)) {
        pr_err("RELM: SVM nested paging (NPT) not supported\n");
        return false;
    }

    pr_info("RELM: NPT support verified\n");

    if (edx & SVM_FEAT_NRIPS)
        pr_info("RELM: SVM next-RIP save supported\n");
    if (edx & SVM_FEAT_DECODE_ASSIST)
        pr_info("RELM: SVM decode assists supported\n");
    if (edx & SVM_FEAT_FLUSH_ASID)
        pr_info("RELM: SVM flush-by-ASID supported\n");

    return true;
}

int relm_setup_npt(struct relm_vm *vm)
{
    if (!vm)
        return -EINVAL;

    if (!relm_npt_check_support()) {
        pr_err("RELM: NPT not supported on this CPU\n");
        return -ENOTSUPP;
    }

    vm->arch.npt = relm_npt_context_create();
    if (IS_ERR(vm->arch.npt)) {
        int err = PTR_ERR(vm->arch.npt);
        vm->arch.npt = NULL;    /* don't leave an ERR_PTR posing as valid */
        pr_err("RELM: Failed to create NPT context: %d\n", err);
        return err;
    }

    return 0;
}


struct npt_context *relm_npt_context_create(void)
{
    struct npt_context *npt;

    /* Context bookkeeping struct: ordinary kernel heap, zeroed. */
    npt = kzalloc(sizeof(*npt), GFP_KERNEL);
    if (!npt) {
        pr_err("RELM: Failed to allocate NPT context\n");
        return ERR_PTR(-ENOMEM);
    }

    npt->pml4 = (npt_pml4_t *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
    if (!npt->pml4) {
        pr_err("RELM: Failed to allocate NPT PML4 table\n");
        kfree(npt);
        return ERR_PTR(-ENOMEM);
    }

    npt->pml4_pa = virt_to_phys(npt->pml4);

    /* Fresh census. */
    memset(&npt->stats, 0, sizeof(npt->stats));

    spin_lock_init(&npt->lock);

    return npt;
}

static void relm_npt_free_table(void *table_va, int level)
{
    npt_entry_t *entries = (npt_entry_t *)table_va;
    int i;

    if (level == 1) {
        free_page((unsigned long)table_va);
        return;
    }

    for (i = 0; i < NPT_ENTRIES_PER_TABLE; i++) {
        npt_entry_t entry = entries[i];

        if (!(entry & NPT_PRESENT))
            continue;

        if (entry & NPT_PAGE_SIZE)
            continue;
       
        uint64_t child_pa = entry & NPT_ADDR_MASK;

        void *child_va = phys_to_virt(child_pa);

        relm_npt_free_table(child_va, level - 1);
    }

    free_page((unsigned long)table_va);
}

void relm_npt_context_destroy(struct npt_context *npt)
{
    if (!npt)
        return;

    if (npt->pml4) {
        relm_npt_free_table(npt->pml4, NPT_LEVELS);
        npt->pml4 = NULL;
        npt->pml4_pa = 0;
    }

    kfree(npt);
}
static inline void *relm_npt_alloc_table(void)
{
    void *table = (void *)__get_free_page(GFP_ATOMIC | __GFP_ZERO);

    if (unlikely(!table)) {
        pr_err("RELM: Failed to alloc NPT table\n");
        return NULL;
    }

    return table;
}

static void *relm_npt_get_or_create_table(npt_entry_t *entry_ptr, int level)
{
    npt_entry_t entry = *entry_ptr;
    void *table_va;
    uint64_t table_pa;

    if (entry & NPT_PRESENT)
        return phys_to_virt(entry & NPT_ADDR_MASK);

    table_va = relm_npt_alloc_table();
    if (!table_va)
        return NULL;

    table_pa = virt_to_phys(table_va);
    *entry_ptr = (table_pa & NPT_ADDR_MASK) | NPT_RWX;

    return table_va;
}

int relm_npt_map_page(struct npt_context *npt, uint64_t gpa,
                      uint64_t hpa, uint64_t flags)
{
    npt_pdpt_t *pdpt;
    npt_pd_t *pd;
    npt_pt_t *pt;

    unsigned long irq_flags;

    if (!npt || !npt->pml4) {
        pr_err("RELM: Invalid NPT context\n");
        return -EINVAL;
    }

    /*
     * 4 KiB alignment on both sides: bits 11:0 of a leaf entry are flag
     * bits, so an unaligned PA cannot even be represented — and an
     * unaligned GPA would silently map the containing page instead.
     */
    if ((gpa & 0xFFF) || (hpa & 0xFFF)) {
        pr_err("RELM: Addresses must be 4KB aligned (GPA=0x%llx, HPA=0x%llx)\n",
               gpa, hpa);
        return -EINVAL;
    }

    if ((flags & (NPT_PRESENT | NPT_USER)) != (NPT_PRESENT | NPT_USER)) {
        pr_err("RELM: NPT mapping must be present + user (flags=0x%llx)\n",
               flags);
        return -EINVAL;
    }

    /* One writer at a time; irqsave since exits can reach this path. */
    spin_lock_irqsave(&npt->lock, irq_flags);

    /* Level 4: GPA bits 47:39 select the PML4 slot. */
    uint32_t pml4_index = NPT_PML4_INDEX(gpa);

    /* Descend/create the PDPT this slot points at. */
    pdpt = (npt_pdpt_t *)relm_npt_get_or_create_table(
        &npt->pml4->entries[pml4_index], 3);
    if (!pdpt) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOMEM;
    }

    /* Level 3: bits 38:30 select the PDPT slot (1 GiB granule). */
    uint32_t pdpt_idx = NPT_PDPT_INDEX(gpa);

    pd = (npt_pd_t *)relm_npt_get_or_create_table(
        &pdpt->entries[pdpt_idx], 2);
    if (!pd) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOMEM;
    }

    /* Level 2: bits 29:21 select the PD slot (2 MiB granule). */
    uint32_t pd_idx = NPT_PD_INDEX(gpa);

    pt = (npt_pt_t *)relm_npt_get_or_create_table(
        &pd->entries[pd_idx], 1);
    if (!pt) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOMEM;
    }

    /* Level 1: bits 20:12 select the final PTE. */
    uint32_t pt_idx = NPT_PT_INDEX(gpa);

    npt_entry_t *leaf_entry = &pt->entries[pt_idx];

    if (*leaf_entry & NPT_PRESENT) {
        pr_warn("RELM: GPA 0x%llx already NPT-mapped, overwriting\n", gpa);
    } else {
        npt->stats.pages_4kb++;
        npt->stats.total_mapped += NPT_PAGE_SIZE_4KB;
    }

    *leaf_entry = (hpa & NPT_ADDR_MASK) | flags;

    spin_unlock_irqrestore(&npt->lock, irq_flags);

    return 0;
}

/* relm_npt_map_range — page-by-page loop, byte-identical logic to EPT's. */
int relm_npt_map_range(struct npt_context *npt, uint64_t gpa_start,
                       uint64_t hpa_start, uint64_t size, uint64_t flags)
{
    uint64_t gpa;
    uint64_t hpa;
    uint64_t num_pages;
    uint64_t i;
    int ret;

    if (!npt)
        return -EINVAL;

    size = PAGE_ALIGN(size);
    num_pages = size / NPT_PAGE_SIZE_4KB;

    for (i = 0; i < num_pages; i++) {
        gpa = gpa_start + (i * NPT_PAGE_SIZE_4KB);
        hpa = hpa_start + (i * NPT_PAGE_SIZE_4KB);

        ret = relm_npt_map_page(npt, gpa, hpa, flags);
        if (ret < 0) {
            pr_err("RELM: Failed to NPT-map page %llu/%llu (GPA=0x%llx)\n",
                   i + 1, num_pages, gpa);
            return ret;
        }
    }

    pr_info("RELM: Successfully NPT-mapped %llu pages\n", num_pages);

    return 0;
}

int relm_npt_map_huge_page(struct npt_context *npt, uint64_t gpa, 
                           uint64_t hpa, uint64_t flags)
{
    npt_pdpt_t *pdpt;
    npt_pd_t *pd;
    npt_entry_t *leaf_entry;
    unsigned long irq_flags;
    uint32_t pml4_index;
    uint32_t pdpt_idx;
    uint32_t pd_idx;

    if (!npt || !npt->pml4) {
        pr_err("RELM: Invalid NPT context\n");
        return -EINVAL;
    }

    if(!npt_is_2mb_aligned(gpa) || !npt_is_2mb_aligned(hpa)){
        pr_err("RELM: Huge NPT mapping must be 2MB aligned (GPA=0x%llx, HPA=0x%llx)\n",
               gpa, hpa);
        return -EINVAL;
    }

    if ((flags & (NPT_PRESENT | NPT_USER)) != (NPT_PRESENT | NPT_USER)) {
        pr_err("RELM: NPT mapping must be present + user (flags=0x%llx)\n",
               flags);
        return -EINVAL;
    }

    spin_lock_irqsave(&npt->lock, irq_flags);

    pml4_index = NPT_PML4_INDEX(gpa);

    pdpt = (npt_pdpt_t *)relm_npt_get_or_create_table(
        &npt->pml4->entries[pml4_index], 3);
    if (!pdpt) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOMEM;
    }

    pdpt_idx = NPT_PDPT_INDEX(gpa);

    pd = (npt_pd_t *)relm_npt_get_or_create_table(
        &pdpt->entries[pdpt_idx], 2);
    if (!pd) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOMEM;
    }

    pd_idx = NPT_PD_INDEX(gpa);

    leaf_entry = &pd->entries[pd_idx];

    if (*leaf_entry & NPT_PRESENT) {
        pr_warn("RELM: GPA 0x%llx already NPT-mapped (huge), overwriting\n", gpa);
    } else {
        npt->stats.pages_2mb++;
        npt->stats.total_mapped += NPT_PAGE_SIZE_2MB;
    }

    *leaf_entry = (hpa & NPT_ADDR_MASK) | flags | NPT_PAGE_SIZE;

    spin_unlock_irqrestore(&npt->lock, irq_flags);

    return 0;
}

static bool npt_range_overlaps_mmio(struct relm_vm *vm, uint64_t gpa, uint64_t size)
{
    unsigned int i, count;
    struct relm_mmio_region region;
    uint64_t range_end = gpa + size;

    if (!vm)
        return false;

    count = relm_vm_mmio_region_count(vm);
    for (i = 0; i < count; i++) {
        if (relm_vm_mmio_region_at(vm, i, &region) < 0)
            continue;

        if (gpa < region.gpa_start + region.size && region.gpa_start < range_end)
            return true;
    }

    return false;
}

int relm_npt_map_guest_ram_4kb(struct npt_context *npt, struct relm_vm *vm,
                               uint64_t gpa_start, uint64_t hpa_start,
                               uint64_t size, uint64_t flags)
{
    uint64_t gpa;
    uint64_t hpa;
    uint64_t num_pages;
    uint64_t i;
    int ret;

    if (!npt)
        return -EINVAL;

    size = PAGE_ALIGN(size);
    num_pages = size / NPT_PAGE_SIZE_4KB;

    for (i = 0; i < num_pages; i++) {
        gpa = gpa_start + (i * NPT_PAGE_SIZE_4KB);
        hpa = hpa_start + (i * NPT_PAGE_SIZE_4KB);

        if (npt_range_overlaps_mmio(vm, gpa, NPT_PAGE_SIZE_4KB)) {
            PDEBUG("RELM: skipping GPA 0x%llx in guest RAM map — reserved MMIO\n",
                   gpa);
            continue;
        }

        ret = relm_npt_map_page(npt, gpa, hpa, flags);
        if (ret < 0) {
            pr_err("RELM: Failed to 4KB-map guest RAM page %llu/%llu (GPA=0x%llx)\n",
                   i + 1, num_pages, gpa);
            return ret;
        }
    }

    pr_info("RELM: Guest RAM mapped 4KB-only: %llu pages (GPA 0x%llx, size 0x%llx)\n",
            num_pages, gpa_start, size);

    return 0;
}

int relm_npt_map_guest_ram_huge(struct npt_context *npt, struct relm_vm *vm,
                                uint64_t gpa_start, uint64_t hpa_start,
                                uint64_t size, uint64_t flags)
{
    uint64_t gpa;
    uint64_t hpa;
    uint64_t end;
    int ret;

    if (!npt)
        return -EINVAL;

    size = PAGE_ALIGN(size);
    end = gpa_start + size;

    gpa = gpa_start;
    hpa = hpa_start;

    while (gpa < end) {
        uint64_t remaining = end - gpa;
        uint64_t offset_in_2mb = gpa & (NPT_PAGE_SIZE_2MB - 1);
        bool aligned_2mb = !offset_in_2mb && !(hpa & (NPT_PAGE_SIZE_2MB - 1));
        bool whole_2mb_fits = remaining >= NPT_PAGE_SIZE_2MB;

        if (aligned_2mb && whole_2mb_fits &&
            !npt_range_overlaps_mmio(vm, gpa, NPT_PAGE_SIZE_2MB)) {
            if (ret < 0) {
                pr_err("RELM: Failed to huge-map GPA 0x%llx\n", gpa);
                return ret;
            }

            gpa += NPT_PAGE_SIZE_2MB;
            hpa += NPT_PAGE_SIZE_2MB;
        } else {
            uint64_t to_boundary = NPT_PAGE_SIZE_2MB - offset_in_2mb;
            uint64_t chunk = (remaining < to_boundary) ? remaining : to_boundary;

            ret = relm_npt_map_guest_ram_4kb(npt, vm, gpa, hpa, chunk, flags);
            if (ret < 0)
                return ret;

            gpa += chunk;
            hpa += chunk;
        }
    }

    pr_info("RELM: Guest RAM mapped (huge-preferred): 4KB=%llu 2MB=%llu pages "
            "(GPA 0x%llx, size 0x%llx)\n",
            npt->stats.pages_4kb, npt->stats.pages_2mb, gpa_start, size);

    return 0;
}

int relm_npt_unmap_page(struct npt_context *npt, uint64_t gpa)
{
    npt_pdpt_t *pdpt;
    npt_pd_t *pd;
    npt_pt_t *pt;
    npt_entry_t *leaf_entry;
    unsigned long irq_flags;
    uint32_t pml4_idx;
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;

    if (!npt || !npt->pml4)
        return -EINVAL;

    spin_lock_irqsave(&npt->lock, irq_flags);

    /* Level 4 slot; non-present => whole 512 GiB region is unmapped. */
    pml4_idx = NPT_PML4_INDEX(gpa);

    if (!(npt->pml4->entries[pml4_idx] & NPT_PRESENT)) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOENT;
    }

    /* Follow the PA in bits 51:12 down to the PDPT. */
    pdpt = (npt_pdpt_t *)phys_to_virt(
        npt->pml4->entries[pml4_idx] & NPT_ADDR_MASK);

    pdpt_idx = NPT_PDPT_INDEX(gpa);

    if (!(pdpt->entries[pdpt_idx] & NPT_PRESENT)) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOENT;
    }

    /* PDPT -> PD. */
    pd = (npt_pd_t *)phys_to_virt(
        pdpt->entries[pdpt_idx] & NPT_ADDR_MASK);

    pd_idx = NPT_PD_INDEX(gpa);

    if (!(pd->entries[pd_idx] & NPT_PRESENT)) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOENT;
    }

    /* PD -> PT. */
    pt = (npt_pt_t *)phys_to_virt(
        pd->entries[pd_idx] & NPT_ADDR_MASK);

    pt_idx = NPT_PT_INDEX(gpa);

    leaf_entry = &pt->entries[pt_idx];

    if (!(*leaf_entry & NPT_PRESENT)) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOENT;
    }

    /*
     * Zero the leaf: not-present at level 1 => accesses to this GPA now
     * raise #NPF with NPF_ERR_PRESENT clear — the MMIO-trap state.
     */
    *leaf_entry = 0;

    npt->stats.pages_4kb--;
    npt->stats.total_mapped -= NPT_PAGE_SIZE_4KB;

    spin_unlock_irqrestore(&npt->lock, irq_flags);

    PDEBUG("RELM: NPT-unmapped GPA 0x%llx\n", gpa);

    return 0;
}

int relm_npt_get_mapping(struct npt_context *npt, uint64_t gpa, uint64_t *hpa)
{
    npt_pdpt_t *pdpt;
    npt_pd_t *pd;
    npt_pt_t *pt;
    npt_entry_t leaf_entry;
    unsigned long irq_flags;

    uint32_t pml4_idx;
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;

    if (!npt || !npt->pml4)
        return -EINVAL;

    /* Lock so a racing unmap can't free/clear tables mid-walk. */
    spin_lock_irqsave(&npt->lock, irq_flags);

    /* Level 4. */
    pml4_idx = NPT_PML4_INDEX(gpa);

    if (!(npt->pml4->entries[pml4_idx] & NPT_PRESENT)) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOENT;
    }

    pdpt = (npt_pdpt_t *)phys_to_virt(
        npt->pml4->entries[pml4_idx] & NPT_ADDR_MASK);

    /* Level 3. */
    pdpt_idx = NPT_PDPT_INDEX(gpa);

    if (!(pdpt->entries[pdpt_idx] & NPT_PRESENT)) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOENT;
    }

    pd = (npt_pd_t *)phys_to_virt(
        pdpt->entries[pdpt_idx] & NPT_ADDR_MASK);

    /* Level 2. */
    pd_idx = NPT_PD_INDEX(gpa);

    if (!(pd->entries[pd_idx] & NPT_PRESENT)) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOENT;
    }

    pt = (npt_pt_t *)phys_to_virt(
        pd->entries[pd_idx] & NPT_ADDR_MASK);

    /* Level 1: the PTE itself. */
    pt_idx = NPT_PT_INDEX(gpa);

    leaf_entry = pt->entries[pt_idx];

    if (!(leaf_entry & NPT_PRESENT)) {
        spin_unlock_irqrestore(&npt->lock, irq_flags);
        return -ENOENT;
    }

    /* Frame from the entry, offset from the original address. */
    *hpa = (leaf_entry & NPT_ADDR_MASK) | NPT_PAGE_OFFSET(gpa);

    spin_unlock_irqrestore(&npt->lock, irq_flags);

    return 0;
}

/*
 * relm_npt_create_guest_page_tables — build the tables the GUEST will
 * load into its own CR3: identity-map (GVA == GPA) of the first 1 GiB
 * using 2 MiB large pages, so the boot code can enable long mode.
 */

int relm_npt_create_guest_page_tables(struct relm_vm *vm)
{
    uint64_t *pml4;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t pml4_gpa, pdpt_gpa, pd_gpa;
    uint64_t pml4_hpa, pdpt_hpa, pd_hpa;
    int i;

    if (!vm || !vm->arch.npt)
        return -EINVAL;

    uint64_t pt_base_gpa = vm->memory.total_guest_ram - (3 * PAGE_SIZE);

    /* Host pages that will back those three guest-physical pages. */
    struct page *pml4_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    struct page *pdpt_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    struct page *pd_page = alloc_page(GFP_KERNEL | __GFP_ZERO);

    if (!pml4_page || !pdpt_page || !pd_page) {
        if (pml4_page) __free_page(pml4_page);
        if (pdpt_page) __free_page(pdpt_page);
        if (pd_page) __free_page(pd_page);
        return -ENOMEM;
    }

    /* Their host-physical addresses — the NPT side of the mapping. */
    pml4_hpa = PFN_PHYS(page_to_pfn(pml4_page));
    pdpt_hpa = PFN_PHYS(page_to_pfn(pdpt_page));
    pd_hpa = PFN_PHYS(page_to_pfn(pd_page));

    /* Their guest-physical addresses — what the guest's entries hold. */
    pml4_gpa = pt_base_gpa;
    pdpt_gpa = pt_base_gpa + PAGE_SIZE;
    pd_gpa = pt_base_gpa + (2 * PAGE_SIZE);
    
    /* NPT-map each table page so the guest */
    relm_npt_map_page(vm->arch.npt, pml4_gpa, pml4_hpa, NPT_RWX);
    relm_npt_map_page(vm->arch.npt, pdpt_gpa, pdpt_hpa, NPT_RWX);
    relm_npt_map_page(vm->arch.npt, pd_gpa, pd_hpa, NPT_RWX);

    /* Host-side VAs to fill in the guest's entries. */
    pml4 = page_address(pml4_page);
    pdpt = page_address(pdpt_page);
    pd = page_address(pd_page);

    /*
     * The guest's entries hold GPAs (its whole address world is
     * guest-physical).  0x7 = Present | R/W | User — classic PTE bits,
     * which for once are literally the same constants as our NPT_* set.
     */

    /* PML4[0] -> PDPT: covers GVA 0 .. 512 GiB. */
    pml4[0] = pdpt_gpa | 0x7;

    /* PDPT[0] -> PD: covers GVA 0 .. 1 GiB. */
    pdpt[0] = pd_gpa | 0x7;

    /*
     * PD: 512 entries x 2 MiB = the full 1 GiB, identity-mapped.
     * 0x87 = Present | R/W | User | PS — PS (bit 7) makes each entry a
     * 2 MiB leaf, so no PT level exists and the entry's address IS the
     * target 2 MiB frame (i * 2 MiB, hence identity).
     */
    for (i = 0; i < 512; i++)
        pd[i] = (i * 0x200000ULL) | 0x87;

    /* Publish the root for boot code to stuff into the guest's CR3
     * (on SVM: vmcb->save.cr3). */
    vm->arch.pml4_gpa = pml4_gpa;

    pr_info("RELM: Guest page tables created - PML4_GPA = 0x%llx\n", pml4_gpa);

    return 0;
}

int relm_npt_handle_fault(struct vcpu *vcpu, uint64_t gpa, uint64_t error_code)
{
    if (!vcpu || !vcpu->vm || !vcpu->vm->arch.npt)
        return -ENAVAIL;

    bool present = error_code & NPF_ERR_PRESENT;
    bool write = error_code & NPF_ERR_WRITE;
    bool fetch = error_code & NPF_ERR_FETCH;
    bool rsvd = error_code & NPF_ERR_RSVD;
    bool gpt_walk = error_code & NPF_ERR_GPT_WALK;

    pr_err("RELM: #NPF at GPA 0x%llx (error=0x%llx)\n", gpa, error_code);
    pr_err("  Access type: %s%s\n",
           write ? "Write " : "Read ",
           fetch ? "(instruction fetch) " : "");
    pr_err("  NPT entry: %s\n",
           present ? "Present (permission violation)" : "Not present");

    if (rsvd) {
        pr_err("  Reserved bit set in NPT entry - bug in NPT setup code!\n");
        relm_npt_dump_tables(vcpu->vm->arch.npt);
    }
   
    if (gpt_walk)
        pr_err("  Fault occurred during guest page-table walk\n");

    if (vcpu->arch.vmcb)
        pr_err("  Guest RIP: 0x%llx\n", vcpu->arch.vmcb->save.rip);

    return -EFAULT;
}

void relm_npt_invalidate(struct relm_vm *vm)
{
    int i;

    if (!vm || !vm->vcpus)
        return;

    for (i = 0; i < vm->max_vcpus; i++) {
        struct vcpu *vcpu = vm->vcpus[i];

        if (!vcpu || !vcpu->arch.vmcb)
            continue;
        vcpu->arch.vmcb->control.tlb_ctl = TLB_CONTROL_FLUSH_ASID;
    }

    PDEBUG("RELM: NPT flush requested (nCR3=0x%llx)\n",
           vm->arch.npt ? vm->arch.npt->pml4_pa : 0);
}

void relm_npt_dump_tables(struct npt_context *npt)
{
    int pml4_idx;
    int pdpt_idx;
    int pd_idx;
    int pt_idx;
    npt_pdpt_t *pdpt;
    npt_pd_t *pd;
    npt_pt_t *pt;

    if (!npt || !npt->pml4)
        return;

    pr_info("=== NPT Table Dump ===\n");
    pr_info("nCR3 (PML4 PA): 0x%llx\n", npt->pml4_pa);
    pr_info("Stats: %llu x 4KB, %llu x 2MB, %llu x 1GB pages\n",
            npt->stats.pages_4kb, npt->stats.pages_2mb, npt->stats.pages_1gb);
    pr_info("Total mapped: %llu bytes\n", npt->stats.total_mapped);

    for (pml4_idx = 0; pml4_idx < NPT_ENTRIES_PER_TABLE; pml4_idx++) {
        if (!(npt->pml4->entries[pml4_idx] & NPT_PRESENT))
            continue;

        pdpt = (npt_pdpt_t *)phys_to_virt(
            npt->pml4->entries[pml4_idx] & NPT_ADDR_MASK);

        for (pdpt_idx = 0; pdpt_idx < NPT_ENTRIES_PER_TABLE; pdpt_idx++) {
            if (!(pdpt->entries[pdpt_idx] & NPT_PRESENT))
                continue;

            pd = (npt_pd_t *)phys_to_virt(
                pdpt->entries[pdpt_idx] & NPT_ADDR_MASK);

            for (pd_idx = 0; pd_idx < NPT_ENTRIES_PER_TABLE; pd_idx++) {
                if (!(pd->entries[pd_idx] & NPT_PRESENT))
                    continue;

                pt = (npt_pt_t *)phys_to_virt(
                    pd->entries[pd_idx] & NPT_ADDR_MASK);

                for (pt_idx = 0; pt_idx < NPT_ENTRIES_PER_TABLE; pt_idx++) {
                    npt_entry_t entry = pt->entries[pt_idx];

                    if (!(entry & NPT_PRESENT))
                        continue;

                    /* Reassemble the GPA from the four indices. */
                    uint64_t gpa = ((uint64_t)pml4_idx << 39) |
                                   ((uint64_t)pdpt_idx << 30) |
                                   ((uint64_t)pd_idx << 21) |
                                   ((uint64_t)pt_idx << 12);
                    uint64_t hpa = entry & NPT_ADDR_MASK;
                    char perms[4] = {
                        'R',                                /* P => readable */
                        (entry & NPT_WRITABLE) ? 'W' : '-',
                        (entry & NPT_NX) ? '-' : 'X',       /* NX inverted   */
                        '\0'
                    };

                    pr_info("  GPA 0x%llx -> HPA 0x%llx [%s]\n",
                            gpa, hpa, perms);
                }
            }
        }
    }

    pr_info("=== End NPT Dump ===\n");
}
