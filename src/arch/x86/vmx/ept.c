#include <asm-generic/errno.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <asm/io.h>
#include <asm/msr.h>

#include <vmx.h>
#include <relm/vm.h>
#include <ept.h>
#include <vmx_ops.h>
#include <vmexit.h>
#include <vmcs_state.h>
#include <virtio/virtio.h>
#include <virtio/mmio.h>
#include <utils/utils.h>


static int vmx_mem_setup(struct relm_vm *vm)
{
    return relm_setup_ept(vm); 
}

static int vmx_mem_map_page(struct relm_vm *vm, 
                            uint64_t gpa, uint64_t hpa, uint64_t flags)
{
    /*Translate generic RELM_MEM_F_ *flags to EPT bits*/ 
    uint64_t ept_flags = 0; 

    if (flags & RELM_MEM_F_READ) 
        ept_flags |= EPT_ACCESS_READ;
    
    if (flags & RELM_MEM_F_WRITE) 
        ept_flags |= EPT_ACCESS_WRITE;
    
    if (flags & RELM_MEM_F_EXEC)  
        ept_flags |= EPT_ACCESS_EXEC;

    if (flags & RELM_MEM_F_MMIO)
        ept_flags |= EPT_MEMTYPE_UC | EPT_IGNORE_PAT;
    else
        ept_flags |= EPT_MEMTYPE_WB;

    return relm_ept_map_page(vm->arch.ept, gpa, hpa, ept_flags);
}

static void vmx_mem_unmap_page(struct relm_vm *vm, uint64_t gpa)
{
    relm_ept_unmap_page(vm->arch.ept, gpa);
}

static int vmx_mem_create_guest_page_tables(struct relm_vm *vm)
{
    return relm_ept_create_guest_page_tables(vm);
}

static void vmx_mem_invalidate(struct relm_vm *vm)
{
    relm_ept_invalidate_context(vm->arch.ept);
}

static void vmx_mem_destroy(struct relm_vm *vm)
{
    if (vm->arch.ept) {
        relm_ept_context_destroy(vm->arch.ept);
        vm->arch.ept = NULL;
    }
}

/*vmx_mem_ops : table assigned to vm->mem_ops for every VMX guest*/ 
const struct relm_mem_ops vmx_mem_ops = {
    .setup                    = vmx_mem_setup,
    .map_page                 = vmx_mem_map_page,
    .unmap_page               = vmx_mem_unmap_page,
    .create_guest_page_tables = vmx_mem_create_guest_page_tables,
    .invalidate               = vmx_mem_invalidate,
    .destroy                  = vmx_mem_destroy,
};

static inline void _invept(uint64_t type, uint64_t eptp)
{
    struct{
        uint64_t eptp; 
        uint64_t reserved; 
    }__attribute__((packed)) descriptor = {
        .eptp = eptp, 
        .reserved = 0,  
    }; 

    __asm__ __volatile__ (
        "invept %0, %1\n"
        : 
        :"m" (descriptor), 
         "r" (type)
        :"memory", "cc"
    ); 
}

bool relm_ept_check_support(void)
{
    uint64_t ept_vpid_cap; 

    ept_vpid_cap = __rdmsr1(MSR_IA32_VMX_EPT_VPID_CAP); 

    if(!(ept_vpid_cap & EPT_CAP_PAGE_WALK_4))
    {
        pr_err("RELM: EPT 4-level page walk not supported\n"); 
        return false; 
    }

    if(!(ept_vpid_cap & EPT_CAP_MEMTYPE_WB))
    {
        pr_err("RELM: EPT write-back memory type not supported\n"); 
        return false; 
    }

    if(!(ept_vpid_cap & EPT_CAP_INVEPT))
    {
        pr_err("RELM: INVEPT instruction not supported\n"); 
        return false; 
    }

    pr_info("RELM: EPT support verified\n");

    RELM_EPT_PRINT_CAP(EPT_CAP_2MB_PAGES,
                   "EPT 2MB large pages supported");

    RELM_EPT_PRINT_CAP(EPT_CAP_1GB_PAGES,
                   "EPT 1GB large pages supported");

    RELM_EPT_PRINT_CAP(EPT_CAP_AD_FLAGS,
                   "EPT accessed/dirty flags supported");  
    return true;
}

int relm_setup_ept(struct relm_vm *vm)
{
    if(!vm)
        return -EINVAL; 

    if(!relm_ept_check_support())
    {
        pr_err("RELM: EPT not supported on this CPU\n"); 
        return -ENOTSUPP; 
    }

    /*
    if(!relm_ept_enabled(vcpu))
    {
        pr_err("RELM: EPT not enabled in execution controls\n"); 
        return -EINVAL; 
    }
*/ 
    vm->arch.ept = relm_ept_context_create(); 
    if(IS_ERR(vm->arch.ept))
    {
        int err = PTR_ERR(vm->arch.ept); 
        vm->arch.ept = NULL; 
        pr_err("RELM: Failed to create EPT context: %d\n", err);
        return err; 
    }

   // pr_info("RELM: EPT setup complete for VM %d (EPTP=0x%llx)\n", 
    //        vm->vm_id, vm->arch.ept->eptp); 

    return 0; 
}

int relm_handle_ept_violation(struct relm_vm *vm)
{
    if(!vm || !vm->arch.ept)
        return -ENAVAIL;

    uint64_t exit_qualification;
    uint64_t gpa; 
    bool data_read; 
    bool data_write; 
    bool instr_fetch;
    bool ept_present; 

    exit_qualification = __vmread(VM_EXIT_QUALIFICATION); 
    gpa = __vmread(GUEST_PHYSICAL_ADDRESS); 

    data_read = exit_qualification & (1ULL << 0);
    data_write = exit_qualification & (1ULL << 1);
    instr_fetch = exit_qualification & (1ULL << 2);
    ept_present = exit_qualification & (1ULL << 3);
    
    pr_err("RELM: EPT violation at GPA 0x%llx\n", gpa);
    pr_err("  Access type: %s%s%s\n",
           data_read ? "Read " : "",
           data_write ? "Write " : "",
           instr_fetch ? "Exec " : "");
    pr_err("  EPT entry: %s\n",
           ept_present ? "Present (permission violation)" : "Not present");
    pr_err("  Guest RIP: 0x%llx\n", __vmread(GUEST_RIP));
   
    /*treat EPT violations as fatat for now */ 
    return -EFAULT; 
}

int relm_vcpu_handle_ept_misconfig(struct relm_vm *vm)
{
    uint64_t gpa; 
    gpa = __vmread(GUEST_PHYSICAL_ADDRESS); 

    pr_err("RELM: EPT misconfiguration at GPA 0x%llx\n", gpa);
    pr_err("  Guest RIP: 0x%llx\n", __vmread(GUEST_RIP));
    pr_err("  This indicates a bug in EPT setup code!\n");
    
    if(vm->arch.ept)
        relm_ept_dump_tables(vm->arch.ept); 

    return -EFAULT; 

}


struct ept_context *relm_ept_context_create(void)
{
    struct ept_context *ept; 

    ept = kzalloc(sizeof(*ept), GFP_KERNEL); 
    if(!ept)
    {
        pr_err("RELM: Failed to allocate EPT context\n"); 
        return ERR_PTR(-ENOMEM); 
    }

    /*allocate root PML4 , 4kb page-aligned, zeroed*/ 
    ept->pml4 = (ept_pml4_t *)__get_free_page(GFP_KERNEL | __GFP_ZERO); 
    if(!ept->pml4)
    {
        pr_err("RELM: Failed to allocate EPT PML4 table\n"); 
        kfree(ept); 
        return ERR_PTR(-ENOMEM); 
    }
    ept->pml4_pa = virt_to_phys(ept->pml4); 

    ept->memtype = EPTP_MEMTYPE_WB;

    /*A/D flags disabled for now */ 
    ept->ad_enabled = false; 

    ept->eptp = CONSTRUCT_EPTP(ept->pml4_pa, ept->memtype, ept->ad_enabled); 

    memset(&ept->stats, 0, sizeof(ept->stats)); 

    spin_lock_init(&ept->lock); 

//    pr_info("RELM : EPT context created, PML4 PA=0x%llx, EPTP=0x%llx\n", 
  //          ept->pml4_pa, ept->eptp); 

    return ept; 
}

static void relm_ept_free_table(void *table_va, int level)
{
    ept_entry_t *entries = (ept_entry_t *)table_va; 
    int i; 

    if(level == 1)
    {
        free_page((unsigned long)table_va); 
        return; 
    }

    for(i = 0; i < EPT_ENTRIES_PER_TABLE; i++)
    {
        ept_entry_t entry = entries[i]; 

        /*skip empty entries */ 
        if(!(entry & EPT_ACCESS_READ))
            continue; 

        /*check if this is a large page */ 
        if(entry & EPT_PAGE_SIZE)
            continue; 

        uint64_t child_pa = entry & EPT_ADDR_MASK; 

        void *child_va = phys_to_virt(child_pa); 

        relm_ept_free_table(child_va, level - 1); 
    }

    free_page((unsigned long)table_va);
}

void relm_ept_context_destroy(struct ept_context *ept)
{
    if(!ept)
        return; 

    if(ept->pml4)
    {
        relm_ept_free_table(ept->pml4, 4); 
        ept->pml4 = NULL; 
        ept->pml4_pa = 0; 
    }

  //  pr_info("RELM: EPT context destroyed (mapped %llu bytes)\n", 
    //        ept->stats.total_mapped); 

    kfree(ept); 
}

static inline void *relm_ept_alloc_table(void)
{
    void *table = (void *)__get_free_page(GFP_ATOMIC | __GFP_ZERO); 
    if(unlikely(!table))
    {
        pr_err("RELM: Failed to alloc EPT table\n");
        return NULL; 
    }

    return table; 
}

static void *relm_ept_get_or_create_table(ept_entry_t *entry_ptr, int level)
{
    ept_entry_t entry = *entry_ptr; 
    void *table_va; 
    uint64_t table_pa; 

    if(entry & EPT_ACCESS_READ)
    {
        return phys_to_virt(entry & EPT_ADDR_MASK);  
    }

    table_va = relm_ept_alloc_table(); 
    if(!table_va)
        return NULL; 

    table_pa = virt_to_phys(table_va); 

    /*create entry pointing to new table 
     * for non-leaf entries , we give RWX permissions*/ 
    *entry_ptr = (table_pa & EPT_ADDR_MASK) | EPT_RWX; 

    return table_va; 
}

int relm_ept_map_page(struct ept_context *ept, uint64_t gpa, 
                     uint64_t hpa, uint64_t flags) 
{
    ept_pdpt_t *pdpt; 
    ept_pd_t *pd; 
    ept_pt_t *pt; 

    unsigned long irq_flags ; 

    if(!ept || !ept->pml4)
    {
        pr_err("RELM: Invalid EPT context\n"); 
        return -EINVAL; 
    }

    if(!ept_is_4kb_aligned(gpa) || !ept_is_4kb_aligned(hpa))
    {
        pr_err("RELM: Addresses must be 4KB aligned (GPA=0x%llx, HPA=0x%llx)\n",
               gpa, hpa); 
        return -EINVAL; 
    }

    /*ensure at least read permission is set */ 
    if(!(flags & EPT_ACCESS_READ))
    {
        pr_err("RELM: EPT mapping must have at least read access\n");
        return -EINVAL; 
    }

    spin_lock_irqsave(&ept->lock, irq_flags); 

    uint32_t pml4_index = EPT_PML4_INDEX(gpa); 

    /*get PDPT table*/ 
    pdpt = (ept_pdpt_t *)relm_ept_get_or_create_table(
        &ept->pml4->entries[pml4_index], 3);
    if(!pdpt)
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOMEM; 
    }

    uint32_t pdpt_idx = EPT_PDPT_INDEX(gpa); 

    pd = (ept_pd_t *)relm_ept_get_or_create_table(
        &pdpt->entries[pdpt_idx], 2);
    if(!pd)
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOMEM; 
    }

    uint32_t pd_idx = EPT_PD_INDEX(gpa); 

    pt = (ept_pt_t *)relm_ept_get_or_create_table(
        &pd->entries[pd_idx], 1); 
    if(!pt)
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOMEM; 
    }

    uint32_t pt_idx = EPT_PT_INDEX(gpa); 

    ept_entry_t *leaf_entry = &pt->entries[pt_idx]; 

    if(*leaf_entry & EPT_ACCESS_READ)
    {
        pr_warn("RELM: GPA 0x%llx already mapped, overwriting\n", gpa); 
    }
    else{
        ept->stats.pages_4kb++; 
        ept->stats.total_mapped += EPT_PAGE_SIZE_4KB; 
    }
    
    *leaf_entry = (hpa & EPT_ADDR_MASK) | flags;

    spin_unlock_irqrestore(&ept->lock, irq_flags); 

//    PDEBUG("RELM: Mapped GPA 0x%llx -> HPA 0x%llx (flags=0x%llx)\n",
    //       gpa, hpa, flags);
    
   // relm_ept_invalidate_context(ept); 
    return 0; 
}

/*map multiple pages in a loop */ 
int relm_ept_map_range(struct ept_context *ept, uint64_t gpa_start, 
                      uint64_t hpa_start, uint64_t size, uint64_t flags)
{
    uint64_t gpa; 
    uint64_t hpa; 
    uint64_t num_pages; 
    uint64_t i; 
    int ret; 

    if(!ept)
        return -EINVAL; 

    /*round size to 4KB page boundary */ 
    size = PAGE_ALIGN(size); 
    num_pages = size / EPT_PAGE_SIZE_4KB; 

    // PDEBUG("RELM: Mapping EPT range GPA 0x%llx -> HPA 0x%llx (%llu pages)\n",

    /*map each page in the range */ 
    for(i = 0; i < num_pages; i++)
    {
        gpa = gpa_start + (i * EPT_PAGE_SIZE_4KB); 
        hpa = hpa_start + (i * EPT_PAGE_SIZE_4KB); 

        ret = relm_ept_map_page(ept, gpa, hpa, flags);
        if(ret < 0)
        {
            pr_err("RELM: Failed to map page %llu/%llu (GPA=0x%llx)\n", 
                   i + 1, num_pages, gpa); 
            return ret; 
        }
    }

    pr_info("RELM: Successfully mapped %llu pages\n", num_pages);

    return 0;
}

int relm_ept_map_huge_page(struct ept_context *ept, uint64_t gpa, 
                           uint64_t hpa, uint64_t flags)
{
    ept_pdpt_t *pdpt; 
    ept_pd_t *pd; 
    ept_entry_t *leaf_entry; 
    unsigned long irq_flags; 
    uint32_t pml4_index; 
    uint32_t pdpt_idx; 
    uint32_t pd_idx; 

    if (!ept || !ept->pml4) {
        pr_err("RELM: Invalid EPT context\n");
        return -EINVAL;
    }

    if(!ept_is_2mb_aligned(gpa) || !ept_is_2mb_aligned(hpa)){
        pr_err("RELM: Huge EPT mapping must be 2MB aligned (GPA=0x%llx, HPA=0x%llx)\n",
               gpa, hpa);
        return -EINVAL;
    }

    if (!(flags & EPT_ACCESS_READ)) {
        pr_err("RELM: EPT mapping must have at least read access\n");
        return -EINVAL;
    }

    spin_lock_irqsave(&ept->lock, irq_flags); 

    pml4_index = EPT_PML4_INDEX(gpa); 

    pdpt = (ept_pdpt_t *)relm_ept_get_or_create_table(
        &ept->pml4->entries[pml4_index], 3); 
    if(!pdpt){
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOMEM; 
    }

    pdpt_idx = EPT_PDPT_INDEX(gpa); 
    pd = (ept_pd_t *)relm_ept_get_or_create_table(
        &pdpt->entries[pdpt_idx], 2);
    if(!pd){
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOMEM; 
    }

    pd_idx = EPT_PD_INDEX(gpa); 
    leaf_entry = &pd->entries[pd_idx]; 

    if (*leaf_entry & EPT_ACCESS_READ) {
        pr_warn("RELM: GPA 0x%llx already mapped (huge), overwriting\n", gpa);
    } else {
        ept->stats.pages_2mb++;
        ept->stats.total_mapped += EPT_PAGE_SIZE_2MB;
    }
    
    /*EPT_PAGE_SIZE(PS): 2 MiB leaf*/ 
    *leaf_entry = (hpa * EPT_ADDR_MASK) | flags | EPT_PAGE_SIZE;

    spin_unlock_irqrestore(&ept->lock, irq_flags); 

    return 0; 
}

/*does gpa gpa, gpa + size touch reserved mmio region*/ 
static bool ept_range_overlaps_mmio(struct relm_vm *vm, uint64_t gpa, uint64_t size)
{
    unsigned int i, count; 
    struct relm_mmio_region region; 
    uint64_t range_end = gpa + size; 

    if(!vm)
        return false; 

    count = relm_vm_mmio_region_count(vm);
    for(i = 0; i < count; ++i){
        if(relm_vm_mmio_region_at(vm, i, &region) < 0)
            continue; 

        if(gpa < region.gpa_start + region.size && region.gpa_start < range_end)
            return true; 
    }

    return false; 
}

int relm_ept_map_guest_ram_4kb(struct ept_context *ept, struct relm_vm *vm, 
                               uint64_t gpa_start, uint64_t hpa_start, 
                               uint64_t size, uint64_t flags)
{
    uint64_t gpa; 
    uint64_t hpa; 
    uint64_t num_pages; 
    uint64_t i; 
    int ret; 

    if(!ept)
        return -EINVAL; 

    size = PAGE_ALIGN(size); 
    num_pages = size / EPT_PAGE_SIZE_4KB; 

    for(i = 0; i < num_pages; ++i){
        gpa = gpa_start + (i * EPT_PAGE_SIZE_4KB); 
        hpa = hpa_start + (i * EPT_PAGE_SIZE_4KB); 

        if(ept_range_overlaps_mmio(vm, gpa, EPT_PAGE_SIZE_4KB)){
            PDEBUG("RELM: skipping GPA 0x%llx in guest RAM map — reserved MMIO\n",
                   gpa);
            continue;
        }

        ret = relm_ept_map_page(ept, gpa, hpa, flags);
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

int relm_ept_map_guest_ram_huge(struct ept_context *ept, struct relm_vm *vm,
                                uint64_t gpa_start, uint64_t hpa_start,
                                uint64_t size, uint64_t flags)
{
    uint64_t gpa;
    uint64_t hpa;
    uint64_t end;
    int ret;

    if (!ept)
        return -EINVAL;

    size = PAGE_ALIGN(size);
    end = gpa_start + size;

    gpa = gpa_start;
    hpa = hpa_start;

    while (gpa < end) {
        uint64_t remaining = end - gpa;
        uint64_t offset_in_2mb = gpa & (EPT_PAGE_SIZE_2MB - 1);
        bool aligned_2mb = !offset_in_2mb && !(hpa & (EPT_PAGE_SIZE_2MB - 1));
        bool whole_2mb_fits = remaining >= EPT_PAGE_SIZE_2MB;

        if (aligned_2mb && whole_2mb_fits &&
            !ept_range_overlaps_mmio(vm, gpa, EPT_PAGE_SIZE_2MB)) {
            ret = relm_ept_map_huge_page(ept, gpa, hpa, flags);
            if (ret < 0) {
                pr_err("RELM: Failed to huge-map GPA 0x%llx\n", gpa);
                return ret;
            }

            gpa += EPT_PAGE_SIZE_2MB;
            hpa += EPT_PAGE_SIZE_2MB;
        } else {
            uint64_t to_boundary = EPT_PAGE_SIZE_2MB - offset_in_2mb;
            uint64_t chunk = (remaining < to_boundary) ? remaining : to_boundary;

            ret = relm_ept_map_guest_ram_4kb(ept, vm, gpa, hpa, chunk, flags);
            if (ret < 0)
                return ret;

            gpa += chunk;
            hpa += chunk;
        }
    }

    pr_info("RELM: Guest RAM mapped (huge-preferred): 4KB=%llu 2MB=%llu pages "
            "(GPA 0x%llx, size 0x%llx)\n",
            ept->stats.pages_4kb, ept->stats.pages_2mb, gpa_start, size);

    return 0;
}

/*walks EPT table to find the leaf entry and clear it */ 
int relm_ept_unmap_page(struct ept_context *ept, uint64_t gpa)
{
    ept_pdpt_t *pdpt; 
    ept_pd_t *pd; 
    ept_pt_t *pt; 
    ept_entry_t *leaf_entry; 
    unsigned long irq_flags; 
    uint32_t pml4_idx; 
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;

    if (!ept || !ept->pml4)
        return -EINVAL; 

    spin_lock_irqsave(&ept->lock, irq_flags); 

    pml4_idx = EPT_PML4_INDEX(gpa); 

    if(!(ept->pml4->entries[pml4_idx] & EPT_ACCESS_READ))
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOENT; 
    }

    pdpt = (ept_pdpt_t *)phys_to_virt(
        ept->pml4->entries[pml4_idx] & EPT_ADDR_MASK);

    pdpt_idx = EPT_PDPT_INDEX(gpa); 

    if(!(pdpt->entries[pdpt_idx] & EPT_ACCESS_READ))
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOENT; 
    }

    pd = (ept_pd_t*)phys_to_virt(
        pdpt->entries[pdpt_idx] & EPT_ADDR_MASK); 

    pd_idx = EPT_PD_INDEX(gpa); 

    if(!(pd->entries[pd_idx] & EPT_ACCESS_READ))
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOENT; 
    }

    pt = (ept_pt_t*)phys_to_virt(
        pd->entries[pd_idx] & EPT_ADDR_MASK); 

    pt_idx = EPT_PT_INDEX(gpa); 

    leaf_entry = &pt->entries[pt_idx]; 

    if(!(*leaf_entry & EPT_ACCESS_READ))
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOENT; 
    }

    /*clear leaf entry */ 
    *leaf_entry = 0; 

    ept->stats.pages_4kb--; 
    ept->stats.total_mapped -= EPT_PAGE_SIZE_4KB; 

    spin_unlock_irqrestore(&ept->lock, irq_flags);

    /*invalidate EPT TLB entries for the entir context to ensure consistency */ 
//    relm_ept_invalidate_context(ept); 

    PDEBUG("RELM: Unmapped GPA 0x%llx\n", gpa);

    return 0; 
}

int relm_ept_create_guest_page_tables(struct relm_vm *vm)
{
    uint64_t *buf; 
    uint64_t *pml4; 
    uint64_t *pdpt;      
    uint64_t *pd;       
    uint64_t pml4_gpa, pdpt_gpa, pd_gpa;
    uint64_t pml4_hpa, pdpt_hpa, pd_hpa;
    int i;
    int ret; 

    if (!vm || !vm->arch.ept)
        return -EINVAL;

    /* allocate 3 pages for page tables (PML4, PDPT, PD)
    * from guest RAM at a high address to avoid conflicts */ 
    
    uint64_t pt_base_gpa = vm->memory.total_guest_ram - (3 * PAGE_SIZE);
    
    pr_info("RELM: Creating guest page tables at GPA 0x%llx\n", pt_base_gpa);

    pml4_gpa = pt_base_gpa; 
    pdpt_gpa = pt_base_gpa + PAGE_SIZE; 
    pd_gpa = pt_base_gpa + (2 * PAGE_SIZE);

    buf = kzalloc(PAGE_SIZE, GFP_KERNEL); 
    if(!buf)
        return -ENOMEM; 

     /* PD entries: Identity map first 1GB using 2MB pages
     * each PD entry covers 2MB */
    for (i = 0; i < 512; i++)
        buf[i] = (i * 0x200000ULL) | 0x87;  // Present, R/W, User, PS
    ret = relm_vm_copy_to_guest(vm, pd_gpa, buf, PAGE_SIZE);
    if (ret < 0)
        goto _out;

     // PDPT[0] -> PD
    memset(buf, 0, PAGE_SIZE);
    buf[0] = pd_gpa | 0x7;  // Present, R/W, User
    ret = relm_vm_copy_to_guest(vm, pdpt_gpa, buf, PAGE_SIZE);
    if (ret < 0)
        goto _out; 

    // PML4[0] -> PDPT
    memset(buf, 0, PAGE_SIZE);
    buf[0] = pdpt_gpa | 0x7;  // Present, R/W, User
    ret = relm_vm_copy_to_guest(vm, pml4_gpa, buf, PAGE_SIZE);
    if (ret < 0)
        goto _out;

    vm->arch.pml4_gpa = pml4_gpa;

    pr_info("RELM: Guest page tables created - PML4_GPA = 0x%llx\n", pml4_gpa);

    ret = 0;
_out:
    kfree(buf);
    return ret;
}

/*walk EPT tables to find the HPA of given GPA */  
int relm_ept_get_mapping(struct ept_context *ept, uint64_t gpa, uint64_t *hpa)
{
    ept_pdpt_t *pdpt; 
    ept_pd_t *pd; 
    ept_pt_t *pt; 
    ept_entry_t leaf_entry; 
    unsigned long irq_flags; 

    uint32_t pml4_idx; 
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;

    if(!ept || !ept->pml4)
        return -EINVAL; 

    spin_lock_irqsave(&ept->lock, irq_flags); 

    pml4_idx = EPT_PML4_INDEX(gpa); 

    if(!(ept->pml4->entries[pml4_idx] & EPT_ACCESS_READ))
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOENT; 
    }

    pdpt = (ept_pdpt_t*)phys_to_virt(
        ept->pml4->entries[pml4_idx] & EPT_ADDR_MASK); 

    pdpt_idx = EPT_PDPT_INDEX(gpa); 

    if(!(pdpt->entries[pdpt_idx] & EPT_ACCESS_READ))
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOENT; 
    }

    pd = (ept_pd_t *)phys_to_virt(
        pdpt->entries[pdpt_idx] & EPT_ADDR_MASK);

    pd_idx = EPT_PD_INDEX(gpa); 

    if(!(pd->entries[pd_idx] & EPT_ACCESS_READ))
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOENT; 
    }

    pt = (ept_pt_t *)phys_to_virt(
        pd->entries[pd_idx] & EPT_ADDR_MASK); 

    pt_idx = EPT_PT_INDEX(gpa); 

    leaf_entry = pt->entries[pt_idx]; 

    if(!(leaf_entry & EPT_ACCESS_READ))
    {
        spin_unlock_irqrestore(&ept->lock, irq_flags); 
        return -ENOENT; 
    }

    /*extract host physical address*/ 
    *hpa = (leaf_entry & EPT_ADDR_MASK) |
        EPT_PAGE_OFFSET(gpa); 

    spin_unlock_irqrestore(&ept->lock, irq_flags); 

    return 0; 
}

/*flush all cached ept translations */ 
void relm_ept_invalidate_context(struct ept_context *ept)
{
    if(!ept)
        return; 

    _invept(1, ept->eptp);

    PDEBUG("RELM: Invalidated EPT context (EPTP=0x%llx)\n", ept->eptp);
}


void relm_ept_dump_tables(struct ept_context *ept)
{
    int pml4_idx;
    int pdpt_idx;
    int pd_idx; 
    int pt_idx;
    ept_pdpt_t *pdpt;
    ept_pd_t *pd;
    ept_pt_t *pt;
    
    if (!ept || !ept->pml4)
        return;
    
    pr_info("=== EPT Table Dump ===\n");
    pr_info("EPTP: 0x%llx\n", ept->eptp);
    pr_info("PML4 PA: 0x%llx\n", ept->pml4_pa);
    pr_info("Stats: %llu x 4KB, %llu x 2MB, %llu x 1GB pages\n",
            ept->stats.pages_4kb, ept->stats.pages_2mb, ept->stats.pages_1gb);
    pr_info("Total mapped: %llu bytes\n", ept->stats.total_mapped);
    
    for (pml4_idx = 0; pml4_idx < EPT_ENTRIES_PER_TABLE; pml4_idx++) 
    {
        if (!(ept->pml4->entries[pml4_idx] & EPT_ACCESS_READ))
            continue;
        
        pdpt = (ept_pdpt_t *)phys_to_virt(
            ept->pml4->entries[pml4_idx] & EPT_ADDR_MASK);
        
        for (pdpt_idx = 0; pdpt_idx < EPT_ENTRIES_PER_TABLE; pdpt_idx++) 
        {
            if (!(pdpt->entries[pdpt_idx] & EPT_ACCESS_READ))
                continue;
            
            pd = (ept_pd_t *)phys_to_virt(
                pdpt->entries[pdpt_idx] & EPT_ADDR_MASK);
            
            for (pd_idx = 0; pd_idx < EPT_ENTRIES_PER_TABLE; pd_idx++)
            {
                if (!(pd->entries[pd_idx] & EPT_ACCESS_READ))
                    continue;
                
                pt = (ept_pt_t *)phys_to_virt(
                    pd->entries[pd_idx] & EPT_ADDR_MASK);
                
                for (pt_idx = 0; pt_idx < EPT_ENTRIES_PER_TABLE; pt_idx++) 
                {
                    ept_entry_t entry = pt->entries[pt_idx];
                    
                    if (!(entry & EPT_ACCESS_READ))
                        continue;
                    
                    uint64_t gpa = ((uint64_t)pml4_idx << 39) |
                                   ((uint64_t)pdpt_idx << 30) |
                                   ((uint64_t)pd_idx << 21) |
                                   ((uint64_t)pt_idx << 12);
                    uint64_t hpa = entry & EPT_ADDR_MASK;
                    char perms[4] = {
                        (entry & EPT_ACCESS_READ) ? 'R' : '-',
                        (entry & EPT_ACCESS_WRITE) ? 'W' : '-',
                        (entry & EPT_ACCESS_EXEC) ? 'X' : '-',
                        '\0'
                    };
                    
                    pr_info("  GPA 0x%llx -> HPA 0x%llx [%s]\n",
                            gpa, hpa, perms);
                }
            }
        }
    }
    
    pr_info("=== End EPT Dump ===\n");
}
