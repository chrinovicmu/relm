#include <linux/kernel.h>
#include <include/debug/page_fault.h>
#include <relm/vcpu.h>

void relm_dump_page_fault(struct vcpu *vcpu, uint64_t guest_rip)
{
    uint64_t cr2;
    uint32_t err;
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
}

