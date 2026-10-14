#ifndef RELM_ARCH_ARM64_ENTRY_H
#define RELM_ARCH_ARM64_ENTRY_H

#include <linux/types.h>

struct vcpu_arch;


extern int relm_arm64_vcpu_run_asm(struct vcpu_arch *arch);


extern char relm_hyp_vectors[];

#endif /* RELM_ARCH_ARM64_ENTRY_H */
