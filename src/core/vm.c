#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <include/relm/vm.h>
#include <include/relm/vcpu.h>
#include <utils/utils.h>


static inline void relm_set_current_vcpu(struct vcpu *vcpu)
{
    this_cpu_write(current_vcpu, vcpu);
}

struct vcpu *relm_get_current_vcpu(void)
{
    return this_cpu_read(current_vcpu);
}

static const char * const vm_state_names[] = {
    [VM_STATE_CREATED]      = "CREATED",
    [VM_STATE_INITIALIZED]  = "INITIALIZED", 
    [VM_STATE_RUNNING]      = "RUNNING",
    [VM_STATE_SUSPENDED]    = "SUSPENDED",
    [VM_STATE_STOPPED]      = "STOPPED",
};

static inline const char *vm_state_to_string(enum vm_state state)
{
    if ((unsigned int)state >= ARRAY_SIZE(vm_state_names))
        return "UNKNOWN";
    return vm_state_names[state] ? vm_state_names[state] : "???";
}

static uint64_t relm_op_get_uptime(struct relm_vm *vm)
{
    if (!vm) return 0;
    return ktime_to_ns(ktime_get()) - vm->stats.start_time_ns;
}

static uint64_t relm_op_get_cpu_utilization(struct relm_vm *vm)
{
    return 0;
}

static void relm_op_print_stats(struct relm_vm *vm)
{
    if (!vm) return;
    pr_info("RELM [%s] Stats: Exits=%llu, CPUID=%llu, HLT=%llu\n",
            vm->vm_name, vm->stats.total_exits,
            vm->stats.cpuid_exits, vm->stats.hlt_exits);
}

static void relm_op_dump_regs(struct relm_vm *vm, int vpid)
{
    struct vcpu *vcpu;
    int index;

    if (!vm) return;
    index = VPID_TO_INDEX(vpid);
    vcpu = vm->vcpus[index];
    if (!vcpu) return;

    if (vcpu->ops && vcpu->ops->dump_regs)
        vcpu->ops->dump_regs(vcpu);
}


static const struct relm_vm_operations relm_generic_vm_ops = {
    .get_uptime = relm_op_get_uptime,
    .get_cpu_utilization = relm_op_get_cpu_utilization; 
    .print_stats = relm_op_print_stats,
    .dump_regs = relm_op_dump_regs,
};

/*build final ops table for a VM
 * arch provides vm_init and destroy_vm, generic provides everything else*/ 
static void relm_vm_ops_merg(struct relm_vm *vm, 
                             const struct relm_vm_operations *arch_ops)
{
    struct relm_vm_operations *merged = &vm->ops_storage;
    *merged = *arch_ops; 

    /*patch generic defaults for any NULL introspection slots*/
    if(!merged->get_uptime)
        merged->get_uptime = relm_generic_vm_ops.get_uptime; 
    if(!merged->get_cpu_utilization)
        merged->get_cpu_utilization = relm_generic_vm_ops->get_cpu_utilization; 
    if (!merged->print_stats)
        merged->print_stats = relm_generic_vm_ops.print_stats;
    if (!merged->dump_regs)
        merged->dump_regs = relm_generic_vm_ops.dump_regs;

    vm->ops = merged; 
}

int relm_vm_allocate_guest_ram(struct relm_vm *vm, uint64_t size, uint64_t gpa_start)
{
    struct guest_mem_region *region;
    uint64_t num_pages;
    uint64_t i;
    struct page *page;
    uint64_t gpa;
    uint64_t hpa;
    int ret;

    if(!vm)
        return -EINVAL;

    /*align size to page boundary */ 
    size = PAGE_ALIGN(size);
    num_pages = size / PAGE_SIZE;

    pr_info("RELM: Allocating %llu pages (%llu MB) of guest RAM at GPA 0x%llx\n",
            num_pages, size / (1024 * 1024), gpa_start);

    region = kzalloc(sizeof(*region), GFP_KERNEL);
    if(!region)
        return -ENOMEM;

    region->pages = kzalloc(num_pages * sizeof(struct page*), GFP_KERNEL);
    if(!region->pages)
    {
        kfree(region);
        return -ENOMEM;
    }

    region->gpa_start = gpa_start;
    region->size = size;
    region->nr_pages = num_pages;

    region->flags = RELM_MEM_F_READ |
        RELM_MEM_F_WRITE | 
        RELM_MEM_F_EXEC;

    for(i = 0; i < num_pages; i++ )
    {
        page = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if(!page)
        {
            pr_err("RELM: Failed to allocate page %llu/%llu\n",
                   i + 1, num_pages);
            ret = -ENOMEM;
            goto _cleanup;
        }

        region->pages[i] = page;
        gpa = gpa_start + (i * PAGE_SIZE);
        hpa = PFN_PHYS(page_to_pfn(page));

        ret = vm->mem_ops->map_page(vm, gpa, hpa, region->flags);
        if(ret < 0)
        {
            pr_err("RELM: Failed to map page at GPA 0x%llx\n", gpa);
            __free_page(page);
            goto _cleanup;
        }
        
        /*progress indicator for every 256MB*/
        if (i > 0 && (i % (256 * 1024 * 1024 / PAGE_SIZE)) == 0)
        {
            pr_info("RELM: Mapped %llu MB...\n",
                    (i * PAGE_SIZE) / (1024 * 1024));
        }
    }

    region->next = vm->memory.mem_regions;
    vm->memory.mem_regions = region;
    vm->memory.total_guest_ram += size;

    pr_info("RELM: Successfully allocated and mapped guest RAM\n");
   
  //  relm_ept_invalidate_context(vm->arch.ept);
   
    return 0;

_cleanup:
    while (i--) {
        if (region->pages[i])
            __free_page(region->pages[i]);
    }
    kfree(region->pages);
    kfree(region);
    return ret;
}

int relm_vm_map_mmio_region(struct relm_vm *vm, uint64_t gpa, 
                            uint64_t hpa, uint64_t size)
{
    struct guest_mem_region *region; 
    uint64_t num_pages; 
    uint64_t i; 
    uint64_t mmio_flags; 
    uint64_t current_gpa, current_hpa; 
    int ret; 

    if(!vm || !vm->arch.ept)
        return -EINVAL; 

    if(size == 0)
        return -EINVAL; 

    if(gpa & ~PAGE_MASK || (hpa & ~PAGE_MASK) || (size & ~PAGE_MASK)){
        pr_err("RELM: MMIO region not page-aligned "
               "(gpa=0x%llx hpa=0x%llx size=0x%llx)\n", 
               gpa, hpa, size); 
        return -EINVAL; 
    }

    num_pages = size / PAGE_SIZE;

    mmio_flags = RELM_MEM_F_READ | 
        RELM_MEM_F_WRITE | 
        RELM_MEM_F_MMIO;  


    pr_info("RELM: Mapping MMIO region GPA 0x%llx -> HPA 0x%llx (%llu pages)\n", 
            gpa, hpa, num_pages); 
    region = kzalloc(sizeof(*region), GFP_KERNEL); 
    if(!region)
        return -ENOMEM; 

    region->gpa_start = gpa; 
    region->size = size; 
    region->pages = NULL; 
    region->nr_pages = num_pages; 
    region->flags = mmio_flags; 

    for(i =0; i < num_pages; i++)
    {
        current_gpa = gpa + (i * PAGE_SIZE); 
        current_hpa = hpa + (i * PAGE_SIZE);

        ret = vm->mem_ops->map_page(vm, current_gpa, current_hpa, mmio_flags); 
        if(ret < 0){
            pr_err("RELM: Failed to map MMIO page GPA 0x%llx -> HPA 0x%llx (%d)\n", 
                   current_gpa, current_hpa, ret); 
            goto _cleanup; 
        }
    }

    region->next = vm->memory.mem_regions; 
    vm->memory.mem_regions = region; 
    pr_info("RELM: MMIO region mapped successfully\n"); 
    
    return 0; 

_cleanup:
    while(i--)
        relm_ept_unmap_page(vm->arch.ept, gpa + (i * PAGE_SIZE)); 
    kfree(region); 
    return ret; 
}
int relm_vm_create_guest_page_tables(struct relm_vm *vm)
{
    if (!vm || !vm->mem_ops || !vm->mem_ops->create_guest_page_tables)
        return -EINVAL;

    return vm->mem_ops->create_guest_page_tables(vm);
}

void relm_vm_free_guest_mem(struct relm_vm *vm)
{
    struct guest_mem_region *region;
    struct guest_mem_region *next;
    uint64_t i;

    if(!vm)
        return;

    region = vm->memory.mem_regions;
    while(region)
    {
        next = region->next;
        if(region->pages)
        {
            for(i = 0; i < region->nr_pages; i++)
            {
                if(region->pages[i])
                    __free_page(region->pages[i]);
            }
            kfree(region->pages);
        }
        kfree(region);
        region = next;
    }

    vm->memory.mem_regions = NULL;
    vm->memory.total_guest_ram = 0;
}

struct relm_vm * relm_create_vm(int vm_id, const char *vm_name,
                              uint64_t ram_size)
{
    struct relm_vm *vm;
    int ret = 0;

    vm = kzalloc(sizeof(struct relm_vm), GFP_KERNEL);
    if(!vm)
        return NULL;

    vm->vm_id = vm_id;
    vm->state = VM_STATE_CREATED;
    vm->max_vcpus = RELM_MAX_VCPUS;
    spin_lock_init(&vm->lock); 

    if(vm_name)
        strscpy(vm->vm_name, vm_name, sizeof(vm->vm_name));
    else
        snprintf(vm->vm_name, sizeof(vm->vm_name), "vm-%d", vm_id);


    /*assign the arch mem_ops tables */ 
    vm->mem_ops = &RELM_ARCH_MEM_OPS;
    relm_vm_ops_merg(vm, &RELM_ARCH_VM_OPS); 

    if(vm-<mem_ops->setup(vm) < 0){

        pr_err("RELM: mem_ops->setup failed\n"); 
        goto _out_free_vm; 
    }

    if(ram_size > 0)
    {
        ret = relm_vm_allocate_guest_ram(vm, ram_size, 0x0);
        if(ret < 0)
        {
            pr_err("RELM: Failed to allocate guest RAM\n");
            goto _out_free_ept;
            return NULL; 
        }
        PDEBUG("RELM: Allocated %llu MB guest RAM\n", ram_size >> 20);

        ret = relm_vm_create_guest_page_tables(vm); 
        if(ret < 0)
        {
            pr_err("Failed to create map Guest page tables\n"); 
            goto _out_free_mmu; 
            return NULL; 
        }
    }

    vm->vcpus = kcalloc(vm->max_vcpus, sizeof(struct vcpu*), GFP_KERNEL);
    if(!vm->vcpus)
    {
        pr_err("RELM: Failed to allocate VCPU array\n");
        goto _out_free_memory;
    }

    vm->state = VM_STATE_INITIALIZED;

    pr_info("RELM: VM '%s' (ID: %d) created with %llu MB RAM\n",
            vm->vm_name, vm->vm_id, (ram_size >> 20));

    return vm;

_out_free_memory:
    relm_vm_free_guest_mem(vm);
_out_free_mmu:
    if(vm->mem_ops && vm->mem_ops->destroy)
        vm->mem_ops->destroy(vm); 
_out_free_vm:
    kfree(vm);
    return NULL;
    
}

void relm_destroy_vm(struct relm_vm *vm)
{
    int i;
    if(!vm)
        return;

    pr_info("RELM: Destroying VM '%s' (ID: %d)\n", vm->vm_name, vm->vm_id);

    /*stop and free all vcpus */
    if(vm->vcpus)
    {
        for(i = 0; i < vm->max_vcpus; i++)
        {
            if(vm->vcpus[i])
            {
                /*if VCPU has runnning thread, stop it first.*/
                if(vm->vcpus[i]->host_task)
                    kthread_stop(vm->vcpus[i]->host_task);
                relm_vcpu_destroy(vm->vcpus[i]);
                vm->vcpus[i] = NULL;
            }
        }
        kfree(vm->vcpus);
    }

    relm_vm_free_guest_mem(vm);

    if(vm->mem_ops && vm->mem_ops->destroy)
        vm->mem_ops->destroy(vm);

    kfree(vm);
    pr_info("RELM: VM destruction complete.\n");
}

/*copy data from host kernel memory into guest's physical address space.*/ 
int relm_vm_copy_to_guest(struct relm_vm *vm, uint64_t gpa,
                         const void *data, size_t size)
{
    struct guest_mem_region *region = NULL;
    uint64_t region_offset;
    uint64_t page_index;
    uint64_t page_offset;
    uint64_t bytes_to_copy;

    const uint8_t *src = (const uint8_t *)data;
    uint8_t *page_va;
    size_t copied = 0;
    uint64_t current_gpa;
    struct page *page; 

    if(!vm || !data || size == 0)
    {
        pr_err("RELM: Invalid parameters to copy_to_guest\n"); 
        return -EINVAL;
    }

    current_gpa = gpa;

    pr_info("RELM: Copying %zu bytes to guest at GPA 0x%llx\n", 
            size, gpa);

    while(copied < size)
    {
        if(!region || current_gpa < region->gpa_start || 
            current_gpa >= (region->gpa_start + region->size))
        {
            region = vm->memory.mem_regions; 
            while(region)
            {
                /*check if current_gpa falls into this region's address space range */ 
                if(current_gpa >= region->gpa_start &&
                    current_gpa < (region->gpa_start + region->size)){
                    break; 
                }
                region = region->next; 
            }
        }

        if(!region)
        {
            pr_err("RELM: GPA %0xllx not mapped in any guest memory region\n", 
                   current_gpa); 
            return copied > 0 ? copied : -EFAULT; 
        }

        /*the offset within the region */ 
        region_offset = current_gpa - region->gpa_start;

        /*page within in region */ 
        page_index = region_offset / PAGE_SIZE; 

        page_offset = region_offset % PAGE_SIZE; 

        /*page boundary 
         * kmap_local_page only maps one 4KB page*/ 
        bytes_to_copy = PAGE_SIZE - page_offset; 

        /*total data boundary
         * ensure we don't copy more that what remains in the source buffer*/ 
        if(bytes_to_copy > (size - copied)){
            bytes_to_copy = size - copied; 
        }

        /*region boundary
         * ensure we don't overflow the guest memory itself*/ 
        if(bytes_to_copy > (region->size - region_offset)){
            bytes_to_copy = region->size - region_offset; 
        }

        /*get physical page corresponding to this guest page*/ 
        page = region->pages[page_index]; 
        if(!page)
        {
            pr_err("RELM: NULL page at index %luu in region at GPA 0x%llx\n", 
                   page_index, region->gpa_start); 
            return copied > 0 ? copied : -EFAULT; 
        }

        /*map guest page into kernel virtual address space */ 
        page_va = kmap_local_page(page); 
        if(!page_va)
        {
            pr_err("RELM: Failed to map guest at GPA 0x%llx\n", 
                   current_gpa); 
        }

        /*perform actaul memory copy*/ 
        memcpy(page_va + page_offset, src + copied, bytes_to_copy); 

        /*host maps guest-backed struct page and writes through 
         * temoparay kernel VA. 
         * these stores populate the CPU's D-cache and are not immediatley
         * visible to guest memory consumers.
         * flush data-cache to ensure DRAM visibility*/ 

        flush_dcache_page(page); 

        kunmap_local(page_va); 

        copied += bytes_to_copy; 
        current_gpa += bytes_to_copy; 

        if (size > (10 * 1024 * 1024) && (copied % (1024 * 1024)) == 0) {
            pr_info("RELM: Copied %zu / %zu bytes...\n", copied, size);
        }     
    }

    pr_info("RELM: Successfully copied %zu bytes to guest memory\n",
            copied);
    return (int)copied; 
}

/*copy data from guest phsyical memory to host */ 
int relm_vm_copy_from_guest(struct relm_vm *vm, const uint64_t gpa,
                            void *data, size_t size)
{
    struct guest_mem_region *region = NULL; 
    uint64_t region_offset; 
    uint64_t page_index; 
    uint64_t page_offset; 
    uint64_t bytes_to_copy; 
    uint8_t *dst = (uint8_t*)data; 
    uint8_t *page_va; 
    size_t copied = 0; 
    uint64_t current_gpa; 
    struct page *page; 

    if(!vm || !data || size == 0)
    {
        pr_err("RELM: Invalid parameters to copy_from_guest\n"); 
        return -EINVAL; 
    }

    current_gpa = gpa; 

    pr_info("RELM: Copying %zu bytes from guest at GPA 0x%llx\n",
            size, gpa);

    while(copied < size)
    {
        if(!region || current_gpa < region->gpa_start || 
            current_gpa >= (region->gpa_start + region->size)){
        
            region = vm->memory.mem_regions; 
            while(region)
            {
            if(current_gpa >= region->gpa_start &&
               current_gpa < (region->gpa_start + region->size)){
                break;
                }
            region = region->next; 
            }
        }

        if(!region)
        {
            pr_err("RELM: GPA 0x%llx not mapped in any guest memory region\n", 
                   current_gpa); 
            return copied > 0 ? copied : -EFAULT; 
        }

        region_offset = current_gpa - region->gpa_start; 
        page_index = region_offset / PAGE_SIZE; 
        page_offset = region_offset % PAGE_SIZE; 

        bytes_to_copy = PAGE_SIZE - page_offset; 
        if(bytes_to_copy > (size - copied)){
            bytes_to_copy = size - copied; 
        }
        if(bytes_to_copy > (region->size - region_offset)){
            bytes_to_copy = region->size - region_offset; 
        }

        page = region->pages[page_index]; 
        if(!page)
        {
            pr_err("RELM: NULL page at index %llu\n", page_index); 
            return copied > 0 ? copied : -EFAULT; 
        }

        page_va = kmap_local_page(page); 
        if(!page_va)
        {
            pr_err("RELM: Failed to map guest paged\n"); 
            return copied > 0 ? copied : -EFAULT; 
        }

        memcpy(dst + copied, page_va + page_offset, bytes_to_copy);

        /*flushing might be unnnecesay here 
        flush_dcache_page(page); 
        */ 

        kunmap_local(page_va); 

        copied += bytes_to_copy; 
        current_gpa += bytes_to_copy; 
    }

    pr_info("RELM: Successfully copied %zu bytes from guest memory\n", 
            copied);

    return (int)copied;

}

/*zero out a range og guest memory */ 
int relm_vm_zero_guest_memory(struct relm_vm *vm, uint64_t gpa, size_t size)
{
    struct guest_mem_region *region;
    uint64_t region_offset;
    uint64_t page_index;
    uint64_t page_offset;
    uint64_t bytes_to_zero;
    uint8_t *page_va;
    size_t zeroed = 0;
    uint64_t current_gpa;
    struct page *page;

    if (!vm || size == 0) {
        return -EINVAL;
    }

    current_gpa = gpa;

    while (zeroed < size) 
    {
        if(!region || current_gpa < region->gpa_start || 
            current_gpa >= (region->gpa_start + region->size))
        {
            region = vm->memory.mem_regions;
            while (region) 
            {
                if (current_gpa >= region->gpa_start &&
                    current_gpa < (region->gpa_start + region->size)) {
                    break;
                }

                region = region->next;
            }
        }

        if (!region) {
            return zeroed > 0 ? zeroed : -EFAULT;
        }

        region_offset = current_gpa - region->gpa_start;
        page_index = region_offset / PAGE_SIZE;
        page_offset = region_offset % PAGE_SIZE;

        bytes_to_zero = PAGE_SIZE - page_offset;
        if (bytes_to_zero > (size - zeroed)) {
            bytes_to_zero = size - zeroed;
        }

        page = region->pages[page_index];
        if (!page) {
            return zeroed > 0 ? zeroed : -EFAULT;
        }

        page_va = kmap_local_page(page);
        if (!page_va) {
            return zeroed > 0 ? zeroed : -ENOMEM;
        }

        memset(page_va + page_offset, 0, bytes_to_zero);

        /*mark page as dirty*/ 
        set_page_dirty(page); 

        flush_dcache_page(page);

        kunmap_local(page_va);

        zeroed += bytes_to_zero;
        current_gpa += bytes_to_zero;
    }

    return (int)zeroed;
}

/*
 * relm_vm_add_vcpu — generic entry point for adding a vCPU to a VM.
 *
 * Validates the vpid, calls vm->ops->add_vcpu() which does the actual
 * allocation through the arch vtable, then handles the vm-level bookkeeping.
 *
 */
int relm_vm_add_vcpu(struct relm_vm *vm, int vpid)
{
    int index;

    if (!vm) {
        pr_err("RELM: relm_vm_add_vcpu: NULL vm\n");
        return -EINVAL;
    }

    if (!VPID_IS_VALID(vpid, vm->max_vcpus)) {
        pr_err("RELM: Invalid VPID %d (max=%d)\n", vpid, vm->max_vcpus);
        return -EINVAL;
    }

    index = VPID_TO_INDEX(vpid);
    if (vm->vcpus[index]) {
        pr_err("RELM: VCPU VPID=%d already exists\n", vpid);
        return -EEXIST;
    }

    if (!vm->ops || !vm->ops->add_vcpu) {
        pr_err("RELM: VM has no add_vcpu op\n");
        return -ENOTSUP;
    }

    /*
     * Delegate entirely to the arch. The arch implementation:
     *   1. Calls relm_vcpu_create(vm, vpid, &vmx_vcpu_ops)
     *   2. Does any arch per-vm-vcpu wiring
     *   3. Stores the vcpu in vm->vcpus[index]
     *   4. Increments vm->online_vcpus
     */
    return vm->ops->add_vcpu(vm, vpid);
}

struct vcpu *relm_vm_get_vcpu(struct relm_vm *vm, uint16_t vpid)
{
    if(!vm || vpid == 0)
        return NULL;

    struct vcpu *vcpu = NULL;
    int index;

    if(!vm || !vm->vcpus)
        return NULL;

    if(!VPID_IS_VALID(vpid, vm->max_vcpus))
        return NULL;

    index = VPID_TO_INDEX(vpid);

    spin_lock(&vm->lock);
    vcpu = vm->vcpus[index];
    spin_unlock(&vm->lock);

    return vcpu;
}



// Note: This function appears to be legacy/incomplete.
// Consider removing it or reimplementing properly.
int relm_run_vm(struct relm_vm *vm)
{
    int i; 
    int ret = 0; 
    int started_vcpus = 0; 
    struct vcpu *vcpu; 

    if(!vm)
    {
        pr_err("RELM: Cannot run NULL VM\n"); 
        return -EINVAL; 
    }

    if(vm->state != VM_STATE_INITIALIZED && vm->state != VM_STATE_STOPPED) 
    {
        pr_err("RELM: VM '%s' is not in runnable state (current state : %s)\n",
               vm->vm_name, 
               vm_state_to_string(vm->state));
        return -EINVAL; 
    }

    if (vm->online_vcpus == 0) {
        pr_err("RELM: VM '%s' has no VCPUs configured\n", vm->vm_name);
        return -ENOENT;
    }

    pr_info("RELM: Starting VM '%s' (ID: %d) with %d VCPU(s)\n",
            vm->vm_name, vm->vm_id, vm->online_vcpus);


    vm->stats.start_time_ns = ktime_to_ns(ktime_get());

    spin_lock(&vm->lock);
    vm->state = VM_STATE_RUNNING;
    spin_unlock(&vm->lock);

    for(i = 0; i < vm->max_vcpus; i++)
    {
        vcpu = vm->vcpus[i]; 

        if(!vcpu){
            continue;
        }

        if (vcpu->host_task)
        {
            pr_warn("RELM: VCPU VPID=%u already running, skipping\n",
                    vcpu->vpid);
            started_vcpus++;
            continue;
        }

        pr_info("RELM: Launching VCPU VPID=%u (target CPU: %d)\n",
                vcpu->vpid, vcpu->target_cpu_id);

        ret = relm_run_vcpu(vm); 
        if(ret < 0)
        {
            pr_err("RELM: Failed to start VCPU VPID=%u: %d\n", 
                   vcpu->vpid, ret); 

            goto _stop_all_vcpus; 
        }

        started_vcpus++; 
    }


    if (started_vcpus == 0) 
    {
        pr_err("RELM: Failed to start any VCPUs for VM '%s'\n",
               vm->vm_name);
        spin_lock(&vm->lock); 
        vm->state = VM_STATE_STOPPED;
        spin_unlock(&vm->lock); 
        return -EIO;
    }

    pr_info("RELM: VM '%s' successfully started with %d/%d VCPUs running\n",
            vm->vm_name, started_vcpus, vm->online_vcpus);

    return 0;

_stop_all_vcpus:

    pr_err("RELM: Stopping all VCPUs due to launch failure\n");

    for (i = 0; i < vm->max_vcpus; i++) 
    {
        vcpu = vm->vcpus[i];
        if (!vcpu || !vcpu->host_task) {
            continue;
        }

        pr_info("RELM: Stopping VCPU VPID=%u\n", vcpu->vpid);
        relm_stop_vcpu(vcpu);
    }

    spin_lock(&vm->lock);
    vm->state = VM_STATE_INITIALIZED;
    spin_unlock(&vm->lock);

    return ret;
}

int relm_stop_vm(struct relm_vm *vm)
{
    int i;
    int ret;
    int stopped_vcpus = 0;
    struct vcpu *vcpu;

    if (!vm) 
    {
        pr_err("RELM: Cannot stop NULL VM\n");
        return -EINVAL;
    }

    if (!vm->vcpus)
    {
        pr_warn("RELM: VM '%s' has no VCPU array\n", vm->vm_name);
        return -EINVAL;
    }

    pr_info("RELM: Stopping VM '%s' (ID: %d)\n",
            vm->vm_name, vm->vm_id);

    for (i = 0; i < vm->max_vcpus; i++) {
        vcpu = vm->vcpus[i];

        vcpu = vm->vcpus[i];
        if(!vcpu || !vcpu->host_task)
                continue;

        pr_info("RELM: Stopping VCPU VPID=%u\n", vcpu->vpid);

        ret = relm_stop_vcpu(vm, vcpu->vpid);
        if (ret < 0)
        {
            pr_err("RELM: Failed to stop VCPU VPID=%u: %d\n",
                   vcpu->vpid, ret);
        } else {
            stopped_vcpus++;
        }
    }

    spin_lock(&vm->lock);
    vm->state = VM_STATE_STOPPED;
    spin_unlock(&vm->lock);

    pr_info("RELM: VM '%s' stopped (%d VCPUs stopped)\n",
            vm->vm_name, stopped_vcpus);

    if (vm->ops && vm->ops->print_stats) {
        vm->ops->print_stats(vm);
    }

    return 0;
}

