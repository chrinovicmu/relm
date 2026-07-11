#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
 
#include <include/relm/vm.h>
#include <include/virtio/virtio.h>
#include <include/virtio/mmio.h>
#include <include/arch/x86/apic.h> 
#include <include/arch/x86/vmx/vm_arch.h> 
#include <include/relm/decoder.h>
#include <include/arch/x86/vmx/vcpu_arch.h> 
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
 
bool relm_virtio_mmio_handle_ept_violation(struct vcpu *vcpu, 
                                           uint64_t fault_gpa)
{
    struct relm_virto_device *dev; 
    uint8_t insn_buf[16];
    struct relm_decoded_insn decoded;
    uint64_t guest_rip; 
    unsigned int offset; 
    uint32_t value = 0; 
    bool needs_irq = false; 
    int n, ret; 

    dev = find_device_for_gpa(fault_gpa); 
    if(!dev)
        return false; 

    offset = (unsigned int)(fault_gpa - dev->mmio_base_gpa); 
    guest_rip = vcpu->arch.regs.rip; 

    n = relm_vm_copy_from_guest(vcpu->vm, guest_rip, 
                                insn_buf, sizeof(insn_buf)); 

    if (n < 0) {
        pr_err("RELM: virtio-mmio: VCPU%d: failed to fetch instruction "
               "bytes at RIP 0x%llx (n=%d)\n", vcpu->vpid, guest_rip, n);
        return true;
    }

    ret = relm_decode_instruction(insn_bytes, n, &decoded);
    if (ret < 0) {
        pr_err("RELM: virtio-mmio: VCPU%d: failed to decode instruction "
               "at RIP 0x%llx for device '%s' (ret=%d)\n",
               vcpu->vpid, guest_rip, dev->name, ret);
        return true;
    }

    /* GPR table */
    uint64_t *gpr_table[16] = {
        &vcpu->arch.regs.rax, &vcpu->arch.regs.rcx,
        &vcpu->arch.regs.rdx, &vcpu->arch.regs.rbx,
        &vcpu->arch.regs.rsp, &vcpu->arch.regs.rbp,
        &vcpu->arch.regs.rsi, &vcpu->arch.regs.rdi,
        &vcpu->arch.regs.r8,  &vcpu->arch.regs.r9,
        &vcpu->arch.regs.r10, &vcpu->arch.regs.r11,
        &vcpu->arch.regs.r12, &vcpu->arch.regs.r13,
        &vcpu->arch.regs.r14, &vcpu->arch.regs.r15,
    };

    int gpr_index = decoded.is_write ? decoded.src_reg : decoded.dst_reg; 

    if (gpr_index < 0 || gpr_index >= 16) {
        pr_warn("RELM: virtio-mmio: VCPU%d: invalid GPR index %d\n",
                vcpu->vpid, gpr_index);
        return true;
    }

    if (decoded.is_write) 
    {
        uint64_t mask = (decoded.op_size == 4) ? 0xFFFFFFFFULL
                      : (decoded.op_size == 2) ? 0xFFFFULL
                      : 0xFFULL;

        value = (uint32_t)(*gpr_table[gpr_index] & mask);

        relm_virtio_mmio_register_access(dev, offset, true, &value, &needs_irq); 
    }else{
        relm_virtio_mmio_register_access(dev, offset, false, &value, &needs_irq);

        /* write back with proper zero-extension */
        if (decoded.op_size == 4) {
            *gpr_table[gpr_index] = (uint64_t)value;
        } else if (decoded.op_size == 2) {
            *gpr_table[gpr_index] = (*gpr_table[gpr_index] & ~0xFFFFULL) | (value & 0xFFFFULL);
        } else {
            *gpr_table[gpr_index] = (*gpr_table[gpr_index] & ~0xFFULL) | (value & 0xFFULL);
        }
    }

    1if (needs_irq) {
        PDEBUG("RELM: virtio-mmio: VCPU%d: injecting IRQ %u for device '%s'\n",
               vcpu->vpid, dev->irq, dev->name);
        relm_apic_inject_irq(dev->vm, dev->irq);
    }

    /* advance RIP */
    vcpu->arch.regs.rip = guest_rip + (unsigned int)ret;
    CHECK_VMWRITE(GUEST_RIP, vcpu->arch.regs.rip);

    PDEBUG("RELM: virtio-mmio: VCPU%d: '%s' %s offset 0x%x size=%u "
           "gpr=%d value=0x%x, RIP 0x%llx -> 0x%llx\n",
           vcpu->vpid, dev->name, decoded.is_write ? "write" : "read",
           offset, decoded.op_size, gpr_index, value,
           guest_rip, vcpu->arch.regs.rip);

    return true;


}
