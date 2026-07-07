#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
 
#include <include/relm/vm.h>
#include <include/virtio/virtio.h>
#include <include/virtio/mmio.h>
#include <utils/utils.h>

int relm_cm_reserve_mmio_region(struct relm_vm *vm, 
                                uint64_t gpa_start, 
                                uint64_t size, 
                                const char *name)
{
    struct guest_mem_region *ram_region; 
    unsigned int i; 
    uint64_t this_end; 

    if (!vm || !name || size == 0)
        return -EINVAL;

    if(!PAGE_ALIGNED(gpa_start) || !PAGE_ALIGNED(size)) 
    {
        pr_err("RELM: mmio: reserve '%s' rejected — gpa_start=0x%llx "
               "size=0x%llx not page-aligned\n", name, gpa_start, size);
        return -EINVAL;
    }

    this_end = gpa_start + size; 

    for(ram_region = vm->memory.mem_regions; ram_region; 
        ram_region = ram_region->next){

        uint64_t ram_end = ram_region->gpa_start + ram_region->size; 
        bool overlap = gpa_start < ram_end && 
            ram_region->gpa_start < this_end; 

        if(overlap) {
            pr_err("RELM: mmio: reserve '%s' (0x%llx-0x%llx) overlaps "
                   "guest RAM region 0x%llx-0x%llx\n",
                   name, gpa_start, this_end - 1,
                   ram_region->gpa_start, ram_end - 1);
            return -EBUSY;
        }
    }

     for(i = 0; i < vm->memory.mem_regions_count; i++){
        struct relm_mmio_region *existing = &vm->memory.mmio_region[i]; 
        uint64_t existing_end = existing->gpa_start + existing->size; 

        bool overlaps = gpa_start < existing_end 
            && existing->gpa_start < this_end; 


        if (overlap) {
            pr_err("RELM: mmio: reserve '%s' (0x%llx-0x%llx) overlaps "
                   "already-reserved '%s' at 0x%llx-0x%llx\n",
                   name, gpa_start, this_end - 1,
                   existing->name, existing->gpa_start, existing_end - 1);
            return -EBUSY;
        }
    }

    if (vm->memory.mmio_region_count >= RELM_MAX_MMIO_REGIONS) {
        pr_err("RELM: mmio: registry full (max %d), cannot reserve "
               "'%s'\n", RELM_MAX_MMIO_REGIONS, name);
        return -ENOSPC;
    }

    struct relm_mmio_region *slot = 
        &vm->memory.mmio_regions[vm->mem_region_count]; 
    slot->gpa_start = gpa_start; 
    slot->size = size; 
    strscpy(slot->name, name, sizeof(slot->name)); 

    vm->memory.mem_region_count++; 

    PDEBUG("RELM: mmio: reserved '%s' at GPA 0x%llx-0x%llx (%llu "
            "bytes) — deliberately UNMAPPED in EPT\n",
            name, gpa_start, this_end - 1, size);
 
    return 0;

}

void relm_vm_release_mmio_region(struct relm_vm *vm, uint64_t gpa_start)
{
    unsigned int i;
 
    if (!vm)
        return;
 
    for (i = 0; i < vm->memory.mmio_region_count; i++) {
        if (vm->memory.mmio_regions[i].gpa_start == gpa_start) {
            unsigned int j;
 
            pr_info("RELM: mmio: released '%s' at GPA 0x%llx\n",
                    vm->memory.mmio_regions[i].name, gpa_start);
 
            for (j = i; j + 1 < vm->memory.mmio_region_count; j++) {
                vm->memory.mmio_regions[j] = vm->memory.mmio_regions[j + 1];
            }
            vm->memory.mmio_region_count--;
            return;
        }
    }
 
    pr_warn("RELM: mmio: release called for GPA 0x%llx, which was "
            "never reserved\n", gpa_start);
}
 
bool relm_vm_gpa_is_mmio_region(struct relm_vm *vm, uint64_t fault_gpa)
{
    unsigned int i;
 
    if (!vm)
        return false;
 
    for (i = 0; i < vm->memory.mmio_region_count; i++) {
        struct relm_mmio_region *region = &vm->memory.mmio_regions[i];
        uint64_t end = region->gpa_start + region->size;
 
        if (fault_gpa >= region->gpa_start && fault_gpa < end) {
            return true;
        }
    }
    return false;
}
 
unsigned int relm_vm_mmio_region_count(struct relm_vm *vm)
{
    if (!vm)
        return 0;
    return vm->memory.mmio_region_count;
}
 
int relm_vm_mmio_region_at(struct relm_vm *vm, unsigned int index,
                           struct relm_mmio_region *out)
{
    if (!vm || !out)
        return -EINVAL;
    if (index >= vm->memory.mmio_region_count)
        return -ENOENT;
 
    *out = vm->memory.mmio_regions[index];
    return 0;
}
 


int relm_virtio_mmio_register_device(struct relm_virtio_device *dev, 
                                     uint64_t base_gpa, 
                                     unsigned int irq)
{

}

