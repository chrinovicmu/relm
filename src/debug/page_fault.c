/*
 * src/debug/page_fault.c — generic-layer guest #PF diagnostic dump.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <include/debug/page_fault.h>
#include <include/debug/insn_dump.h>
#include <relm/vcpu.h>
#include <relm/vm.h>

#define RELM_PAGE_FAULT_INSN_DUMP_WINDOW   0x20

bool relm_page_fault_addr_in_guest_ram(struct vcpu *vcpu, uint64_t gpa,
                                       uint64_t *out_region_start,
                                       uint64_t *out_region_size)
{
    struct guest_mem_region *region;

    for (region = vcpu->vm->memory.mem_regions; region; region = region->next) {
        if (gpa >= region->gpa_start &&
            gpa < region->gpa_start + region->size) {
            *out_region_start = region->gpa_start;
            *out_region_size = region->size;
            return true;
        }
    }

    return false;
}

static void relm_page_fault_dump_insn_window(struct vcpu *vcpu, uint64_t guest_rip)
{
    if (guest_rip < RELM_PAGE_FAULT_INSN_DUMP_WINDOW)
        return;

    pr_err("relm: [VPID=%u] #PF: disassembling %u bytes before RIP=0x%llx:\n",
           vcpu->vpid, RELM_PAGE_FAULT_INSN_DUMP_WINDOW, guest_rip);
    RELM_DUMP_GUEST_INSN_RANGE(vcpu,
                               guest_rip - RELM_PAGE_FAULT_INSN_DUMP_WINDOW,
                               guest_rip);
}

void relm_dump_page_fault(struct vcpu *vcpu, uint64_t guest_rip)
{
    uint64_t cr2;
    uint32_t err;
    uint64_t gpa;
    uint64_t region_start, region_size;
    int ret;

    ret = relm_arch_get_page_fault_info(vcpu, &cr2, &err);
    if (ret < 0) {
        pr_err("relm: [VPID=%u] #PF at RIP=0x%llx: failed to read fault "
               "info (%d)\n", vcpu->vpid, guest_rip, ret);
        return;
    }

    pr_err("relm: [VPID=%u] #PF at RIP=0x%llx: CR2=0x%llx err=0x%x "
           "(%s %s %s%s)\n",
           vcpu->vpid, guest_rip, cr2, err,
           (err & 1) ? "protection" : "not-present",
           (err & 2) ? "write" : "read",
           (err & 4) ? "user" : "supervisor",
           (err & 0x10) ? " instr-fetch" : "");

    relm_arch_dump_page_fault_regs(vcpu);

    ret = relm_arch_translate_gva_to_gpa(vcpu, cr2, &gpa);
    if (ret == -EFAULT) {
        pr_err("relm: [VPID=%u] #PF: guest's own page tables have NO "
               "mapping for CR2=0x%llx (%s with the error code's %s bit) — "
               "no GPA exists yet to check against guest RAM\n",
               vcpu->vpid, cr2,
               (err & 1) ? "INCONSISTENT" : "consistent",
               (err & 1) ? "protection" : "not-present");
        relm_page_fault_dump_insn_window(vcpu, guest_rip);
        return;
    } else if (ret < 0) {
        pr_err("relm: [VPID=%u] #PF: CR2=0x%llx uses a guest paging mode "
               "this backend's walker doesn't support (%d) — cannot "
               "translate to a GPA\n", vcpu->vpid, cr2, ret);
        relm_page_fault_dump_insn_window(vcpu, guest_rip);
        return;
    }

    if (relm_page_fault_addr_in_guest_ram(vcpu, gpa, &region_start, &region_size)) {
        pr_err("relm: [VPID=%u] #PF CR2=0x%llx -> GPA=0x%llx is INSIDE "
               "backed guest RAM region [0x%llx-0x%llx) — guest itself "
               "faulted on mapped memory (check guest page tables / EPT "
               "permissions)\n",
               vcpu->vpid, cr2, gpa, region_start, region_start + region_size);
    } else {
        pr_err("relm: [VPID=%u] #PF CR2=0x%llx -> GPA=0x%llx is OUTSIDE "
               "all backed guest RAM (total mapped: %llu MB) — the guest's "
               "own page tables point at a physical page RELM never backed "
               "(EPT/memory-map bug, or a guest bug that built a garbage "
               "mapping)\n",
               vcpu->vpid, cr2, gpa,
               vcpu->vm->memory.total_guest_ram / (1024 * 1024));
        relm_page_fault_dump_insn_window(vcpu, guest_rip);
    }
}
