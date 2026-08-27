#ifndef RELM_INSN_DUMP_H
#define RELM_INSN_DUMP_H

#include <linux/types.h>

struct vcpu;

int relm_dump_guest_insn(struct vcpu *vcpu, uint64_t gva);
int relm_dump_guest_insn_range(struct vcpu *vcpu, uint64_t gva_start,
                               uint64_t gva_end);

int relm_arch_dump_guest_insn(struct vcpu *vcpu, uint64_t gva,
                              unsigned int *out_length);

#ifndef RELM_INSN_DUMP_DEBUG
#define RELM_INSN_DUMP_DEBUG 1
#endif

#if RELM_INSN_DUMP_DEBUG

#define RELM_DUMP_GUEST_INSN(vcpu, gva) \
    relm_dump_guest_insn((vcpu), (gva))

#define RELM_DUMP_GUEST_INSN_RANGE(vcpu, start, end) \
    relm_dump_guest_insn_range((vcpu), (start), (end))

#else /* !RELM_INSN_DUMP_DEBUG */


#define RELM_DUMP_GUEST_INSN(vcpu, gva) \
    do { (void)(vcpu); (void)(gva); } while (0)

#define RELM_DUMP_GUEST_INSN_RANGE(vcpu, start, end) \
    do { (void)(vcpu); (void)(start); (void)(end); } while (0)

#endif /* RELM_INSN_DUMP_DEBUG */

#endif /* RELM_INSN_DUMP_H */ 

