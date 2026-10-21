/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_ARCH_ARM64_VMEXIT_H
#define RELM_ARCH_ARM64_VMEXIT_H

struct vcpu;


#define RELM_ARM64_ESR_EC_SHIFT          26U
#define RELM_ARM64_ESR_EC_MASK           (0x3fULL << RELM_ARM64_ESR_EC_SHIFT)

/*
 * Unshifted EC values for the AArch64 classes classified below.
 * Defining a class does not enable or advertise its optional CPU feature.
 * AArch32 classes and other unlisted encodings use the unhandled-sync hook.
 */

/* Unknown reason, including instructions reported as undefined. */
#define RELM_ARM64_EC_UNKNOWN            0x00U

/* Trapped Wait For Interrupt/Event: WFI/WFE and implemented timed forms. */
#define RELM_ARM64_EC_WFX                0x01U

/* Access trap for floating-point/Advanced SIMD, not an FP arithmetic fault. */
#define RELM_ARM64_EC_FP_ASIMD           0x07U

/* Trapped pointer-authentication instruction (for example by HCR_EL2.API). */
#define RELM_ARM64_EC_PAUTH              0x09U

/* Branch Target Identification exception: a guarded branch-target violation. */
#define RELM_ARM64_EC_BTI                0x0dU

/* Illegal execution state, such as an illegal exception-return state. */
#define RELM_ARM64_EC_ILLEGAL_STATE      0x0eU

/*
 * Supervisor, Hypervisor and Secure Monitor Calls from AArch64.
 * SVC normally reaches guest Linux at EL1, not RELM with guest TGE=0.
 */
#define RELM_ARM64_EC_SVC64              0x15U
#define RELM_ARM64_EC_HVC64              0x16U
#define RELM_ARM64_EC_SMC64              0x17U

/* Trapped A64 system-register or system-instruction access (MRS/MSR/SYS). */
#define RELM_ARM64_EC_SYS64              0x18U

/* Scalable Vector Extension access trap; RELM currently saves basic FP only. */
#define RELM_ARM64_EC_SVE                0x19U

/* Trapped ERET/ERETAA/ERETAB instruction; distinct from illegal state. */
#define RELM_ARM64_EC_ERET               0x1aU

/* Pointer-authentication failure, distinct from a disabled-instruction trap. */
#define RELM_ARM64_EC_FPAC               0x1cU

/* Scalable Matrix Extension exception; no SME guest state is implemented yet. */
#define RELM_ARM64_EC_SME                0x1dU

/*
 * Instruction/Data Abort: an instruction fetch or data access failed.
 * LOWER means the fault came from EL0/EL1 relative to EL2. CURRENT means
 * EL2's own execution faulted; it must not enter the guest-abort handler.
 * The EC alone does not establish stage 2, MMIO, or a valid FAR/HPFAR.
 */
#define RELM_ARM64_EC_IABT_LOWER         0x20U
#define RELM_ARM64_EC_IABT_CURRENT       0x21U
#define RELM_ARM64_EC_PC_ALIGNMENT       0x22U /* Misaligned instruction address. */
#define RELM_ARM64_EC_DABT_LOWER         0x24U
#define RELM_ARM64_EC_DABT_CURRENT       0x25U
#define RELM_ARM64_EC_SP_ALIGNMENT       0x26U /* Stack-pointer alignment check. */

/* Floating-point arithmetic exception from AArch64; not EC_FP_ASIMD. */
#define RELM_ARM64_EC_FP_EXCEPTION64     0x2cU

/* System Error syndrome class: dispatch SError by vector, not this value. */
#define RELM_ARM64_EC_SERROR             0x2fU

/*
 * Hardware debug exceptions: breakpoint, software single-step, watchpoint.
 * LOWER describes guest execution. CURRENT describes EL2 itself.
 * A Linux userspace BRK instruction has the separate BRK64 class below.
 */
#define RELM_ARM64_EC_BREAKPOINT_LOWER   0x30U
#define RELM_ARM64_EC_BREAKPOINT_CURRENT 0x31U
#define RELM_ARM64_EC_SINGLE_STEP_LOWER  0x32U
#define RELM_ARM64_EC_SINGLE_STEP_CURRENT 0x33U
#define RELM_ARM64_EC_WATCHPOINT_LOWER   0x34U
#define RELM_ARM64_EC_WATCHPOINT_CURRENT 0x35U
#define RELM_ARM64_EC_BRK64              0x3cU /* A64 software breakpoint. */

int relm_arm64_handle_exit(struct vcpu *vcpu);
int relm_arm64_handle_irq(struct vcpu *vcpu);
int relm_arm64_handle_fiq(struct vcpu *vcpu);
int relm_arm64_handle_serror(struct vcpu *vcpu);
int relm_arm64_handle_wfx(struct vcpu *vcpu);
int relm_arm64_handle_hvc(struct vcpu *vcpu);
int relm_arm64_handle_smc(struct vcpu *vcpu);
int relm_arm64_handle_sysreg(struct vcpu *vcpu);
int relm_arm64_handle_instruction_abort(struct vcpu *vcpu);
int relm_arm64_handle_data_abort(struct vcpu *vcpu);
int relm_arm64_handle_unknown_exception(struct vcpu *vcpu);
int relm_arm64_handle_illegal_execution_state(struct vcpu *vcpu);
int relm_arm64_handle_pc_alignment(struct vcpu *vcpu);
int relm_arm64_handle_sp_alignment(struct vcpu *vcpu);
int relm_arm64_handle_fp_simd_trap(struct vcpu *vcpu);
int relm_arm64_handle_fp_exception(struct vcpu *vcpu);
int relm_arm64_handle_sve_trap(struct vcpu *vcpu);
int relm_arm64_handle_sme_trap(struct vcpu *vcpu);
int relm_arm64_handle_pauth_trap(struct vcpu *vcpu);
int relm_arm64_handle_pauth_fault(struct vcpu *vcpu);
int relm_arm64_handle_bti_exception(struct vcpu *vcpu);
int relm_arm64_handle_eret_trap(struct vcpu *vcpu);
int relm_arm64_handle_breakpoint(struct vcpu *vcpu);
int relm_arm64_handle_single_step(struct vcpu *vcpu);
int relm_arm64_handle_watchpoint(struct vcpu *vcpu);
int relm_arm64_handle_brk(struct vcpu *vcpu);

/*
 * Unexpected: a recognized class inconsistent with this guest exit path.
 * Unhandled: an unlisted EC, including unsupported AArch32/newer classes.
 * Both retain the full saved ESR for a later explicit failure policy.
 */
int relm_arm64_handle_unexpected_sync(struct vcpu *vcpu);
int relm_arm64_handle_unhandled_sync(struct vcpu *vcpu);

#endif /* RELM_ARCH_ARM64_VMEXIT_H */
