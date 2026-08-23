#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
 
#include <include/relm/vm.h>
#include <include/virtio/virtio.h>
#include <include/virtio/mmio.h>
#include <include/arch/x86/vmx/vm_arch.h>
#include <include/relm/decoder.h>
#include <include/arch/x86/vmx/vcpu_arch.h>
#include <include/arch/x86/vmx/mmu.h>
#include <relm/vcpu.h>
#include <utils/utils.h>

/*
 * relm_vm_reserve_mmio_region() — claim a guest-physical address range as
 * an MMIO window. The trap mechanism is simply *absence*: the range is
 * recorded in vm->memory.mmio_regions[] and deliberately NEVER mapped in
 * EPT, so any guest access raises an EPT violation, which the exit handler
 * routes to relm_virtio_mmio_handle_ept_violation() below.
 *
 * Validates before committing:
 *   - gpa_start and size must be page-aligned (EPT maps whole pages, so a
 *     sub-page region would drag neighbouring addresses into trapping);
 *   - the range must not overlap guest RAM (walk of mem_regions list) —
 *     RAM is EPT-mapped, so an overlap would make part of the window
 *     silently NOT trap;
 *   - the range must not overlap an already-reserved MMIO region;
 *   - the fixed registry (RELM_MAX_MMIO_REGIONS slots) must have room.
 *
 * Returns 0, -EINVAL (bad args/alignment), -EBUSY (overlap), or -ENOSPC
 * (registry full).
 */
int relm_vm_reserve_mmio_region(struct relm_vm *vm,
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

     for(i = 0; i < vm->memory.mmio_region_count; i++){
        struct relm_mmio_region *existing = &vm->memory.mmio_regions[i];
        uint64_t existing_end = existing->gpa_start + existing->size; 

        bool overlaps = gpa_start < existing_end 
            && existing->gpa_start < this_end; 


        if (overlaps) {
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
        &vm->memory.mmio_regions[vm->memory.mmio_region_count];
    slot->gpa_start = gpa_start;
    slot->size = size;
    strscpy(slot->name, name, sizeof(slot->name));

    vm->memory.mmio_region_count++;

    PDEBUG("RELM: mmio: reserved '%s' at GPA 0x%llx-0x%llx (%llu "
            "bytes) — deliberately UNMAPPED in EPT\n",
            name, gpa_start, this_end - 1, size);
 
    return 0;

}

/*
 * relm_vm_release_mmio_region() — remove the reservation whose start GPA
 * matches exactly (start address is the lookup key, not any address inside
 * the range). The registry is a dense array, so later entries are shifted
 * down one slot to fill the hole. Releasing a never-reserved GPA only
 * warns — callers on teardown paths may release unconditionally.
 */
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
 
/*
 * relm_vm_gpa_is_mmio_region() — does this guest-physical address fall
 * inside any reserved MMIO window? Used by the EPT-violation exit handler
 * to distinguish "guest touched an emulated device" (dispatch to MMIO
 * emulation) from "guest touched genuinely unmapped memory" (a fatal
 * guest bug). Range check is [gpa_start, gpa_start + size).
 */
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
 
/*
 * relm_vm_mmio_region_count() — number of currently-reserved MMIO regions.
 * Pairs with relm_vm_mmio_region_at() for iteration without exposing the
 * registry's storage to callers.
 */
unsigned int relm_vm_mmio_region_count(struct relm_vm *vm)
{
    if (!vm)
        return 0;
    return vm->memory.mmio_region_count;
}
 
/*
 * relm_vm_mmio_region_at() — copy the region descriptor at 'index' into
 * *out (copy, not pointer: entries move when a region is released, so a
 * borrowed pointer could go stale under the caller). Returns 0, -EINVAL
 * on NULL args, -ENOENT past the end.
 */
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
    struct relm_virtio_device *dev;
    uint8_t insn_buf[16];
    struct relm_decoded_insn decoded;
    uint64_t guest_rip, guest_linear;
    unsigned int offset;
    uint32_t value = 0; 
    unsigned long old_gpr;
    bool needs_irq = false; 
    int gpr_index = -1;
    int n, ret;

    /* Which virtio device owns this GPA? None → not our fault to handle.
     * fault_gpa is the hardware-translated DATA address (see header
     * comment) — already physical, already the address space the device
     * registry is keyed in, so it is compared against mmio_base_gpa
     * ranges directly. */
    dev = relm_virtio_find_device_for_gpa(vcpu->vm, fault_gpa);
    if(!dev)
        return false;

    /* Byte offset into the device's register window = which virtio-mmio
     * register (MagicValue, DeviceID, QueueNotify, ...) was touched.
     * Plain GPA arithmetic — both operands are physical. */
    offset = (unsigned int)(fault_gpa - dev->mmio_base_gpa);

    guest_rip = vcpu->arch.regs.rip;

    guest_linear = relm_mmu_rip_to_linear(vcpu, guest_rip);
    {
        size_t first = min(sizeof(insn_buf),
                           (size_t)(PAGE_SIZE - (guest_linear & ~PAGE_MASK)));

        n = relm_mmu_copy_from_guest_virt(vcpu, guest_linear,
                                          insn_buf, first);
        if (n > 0 && first < sizeof(insn_buf)) {
            int n2 = relm_mmu_copy_from_guest_virt(vcpu,
                                                   guest_linear + first,
                                                   insn_buf + first,
                                                   sizeof(insn_buf) - first);
            if (n2 > 0)
                n += n2;
        }
    }

    if (n < 0) {
        pr_err("RELM: virtio-mmio: VCPU%d: failed to fetch instruction "
               "bytes at RIP 0x%llx (linear 0x%llx, n=%d)\n",
               vcpu->vpid, guest_rip, guest_linear, n);
        /* Returning true here would VMRESUME at the same RIP and re-fault
         * forever; report unhandled so the caller stops the vCPU. */
        return false;
    }

    ret = relm_decode_instruction(insn_buf, n, &decoded);
    if (ret < 0) {
        pr_err("RELM: virtio-mmio: VCPU%d: failed to decode instruction "
               "at RIP 0x%llx for device '%s' (ret=%d)\n",
               vcpu->vpid, guest_rip, dev->name, ret);
        return false;
    }

    /*
     * Virtio-MMIO registers are 32-bit and naturally aligned. The device
     * model below exchanges a uint32_t and has no byte-enable interface, so a
     * byte/word/qword instruction could not be emulated precisely. Reject it
     * instead of silently widening, truncating, or addressing the wrong
     * register. Normal Linux readl()/writel() accesses satisfy both checks.
     */
    if (decoded.memory_width != sizeof(value) || (offset & 3U)) {
        pr_warn("RELM: virtio-mmio: VCPU%d: unsupported access width=%u "
                "or unaligned offset=0x%x for device '%s'\n",
                vcpu->vpid, decoded.memory_width, offset, dev->name);
        return false;
    }

    if (decoded.operand_kind == RELM_OPERAND_GPR) {
        if (!relm_decoded_has_valid_gpr(&decoded)) {
            pr_warn("RELM: virtio-mmio: VCPU%d: invalid decoded GPR %d\n",
                    vcpu->vpid, decoded.gpr_index);
            return false;
        }
        gpr_index = decoded.gpr_index;
    }

    if (decoded.memory_access == RELM_MEMORY_ACCESS_WRITE) {
        /*
         * A store's value is either embedded in the instruction or extracted
         * from the exact decoded register lane. The helper matters even though
         * virtio currently accepts only dword transfers: it keeps register
         * semantics centralized and makes future device widths safe by
         * construction.
         */
        if (decoded.operand_kind == RELM_OPERAND_IMMEDIATE) {
            value = (uint32_t)(decoded.immediate &
                               relm_width_mask(decoded.memory_width));
        } else if (decoded.operand_kind == RELM_OPERAND_GPR) {
            old_gpr = guest_reg_read(&vcpu->arch.regs, gpr_index);
            value = (uint32_t)relm_decoded_gpr_extract(&decoded, old_gpr);
        } else {
            return false;
        }

        relm_virtio_mmio_register_access(dev, offset, true, &value, &needs_irq);
    } else if (decoded.memory_access == RELM_MEMORY_ACCESS_READ) {
        if (decoded.operand_kind != RELM_OPERAND_GPR)
            return false;

        relm_virtio_mmio_register_access(dev, offset, false, &value, &needs_irq);
        old_gpr = guest_reg_read(&vcpu->arch.regs, gpr_index);
        guest_reg_write(&vcpu->arch.regs, gpr_index,
                        (unsigned long)relm_decoded_gpr_commit(
                            &decoded, old_gpr, value));
    } else {
        return false;
    }

    if (needs_irq) {
        PDEBUG("RELM: virtio-mmio: VCPU%d: injecting IRQ %u for device '%s'\n",
               vcpu->vpid, dev->irq, dev->name);
        /* Route through the arch ops so this generic layer never touches
         * the x86 APIC directly; the backend translates line -> vector.
         * TODO: route to the vCPU the guest programmed, not the one that
         * happened to take this fault (needs IOAPIC/GSI routing). */
        int irq_ret = vcpu->ops->inject_irq(vcpu, dev->irq);
        if (irq_ret < 0)
            pr_warn("RELM: virtio-mmio: VCPU%d: IRQ %u injection failed "
                    "(%d) for device '%s'\n",
                    vcpu->vpid, dev->irq, irq_ret, dev->name);
    }

    /* Step the guest past the emulated instruction using the length stored in
     * the validated decoder contract. Both copies of RIP are updated: the
     * VMCS field is what the CPU actually resumes from; vcpu->arch.regs.rip
     * keeps the software model consistent for tracing and later handlers. */
    vcpu->arch.regs.rip = guest_rip + decoded.length;
    CHECK_VMWRITE(GUEST_RIP, vcpu->arch.regs.rip);

    PDEBUG("RELM: virtio-mmio: VCPU%d: '%s' %s offset 0x%x size=%u "
           "gpr=%d value=0x%x, RIP 0x%llx -> 0x%llx\n",
           vcpu->vpid, dev->name,
           decoded.memory_access == RELM_MEMORY_ACCESS_WRITE ? "write" : "read",
           offset, decoded.memory_width, gpr_index, value,
           guest_rip, vcpu->arch.regs.rip);

    return true;


}
