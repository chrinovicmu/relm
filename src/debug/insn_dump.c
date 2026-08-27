#include <linux/errno.h>
#include <linux/kernel.h>
#include <debug/insn_dump.h>

#define RELM_INSN_DUMP_RANGE_MAX      256

int relm_dump_guest_insn(struct vcpu *vcpu, uint64_t gva)
{
    unsigned int length; 
    return relm_arch_dump_guest_insn(vcpu, gva, &length);
}

int relm_dump_guest_insn_range(struct vcpu *vcpu, uint64_t gva_start,
                               uint64_t gva_end)
{
    uint64_t cursor;
    unsigned int printed = 0;
    int ret;

    if (!vcpu || gva_end <= gva_start)
        return -EINVAL;

    cursor = gva_start;

    while (cursor < gva_end && printed < RELM_INSN_DUMP_RANGE_MAX) {
        unsigned int length = 0;

        ret = relm_arch_dump_guest_insn(vcpu, cursor, &length);
        if (ret < 0) {
            return ret;
        }

        cursor += length;
        printed++;
    }
    if (cursor < gva_end) {
        pr_info("relm decoder: range dump truncated at %u instructions "
               "(reached 0x%llx, requested up to 0x%llx)\n",
               RELM_INSN_DUMP_RANGE_MAX, (unsigned long long)cursor,
               (unsigned long long)gva_end);
    }

    return 0;
}


