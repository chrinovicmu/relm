// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>

#include <relm/vcpu.h>
#include <vmexit.h> 


/*
 * Called ONLY for the synchronous vector. ESR_EL2.EC is bits [31:26];
 */
static int relm_arm64_dispatch_sync(struct vcpu *vcpu)
{
    u64 esr = vcpu->arch.regs.esr_el2;
    u32 ec = (u32)((esr & RELM_ARM64_ESR_EC_MASK) >>
                   RELM_ARM64_ESR_EC_SHIFT);

    switch (ec) {
    case RELM_ARM64_EC_UNKNOWN:
        return relm_arm64_handle_unknown_exception(vcpu);

    case RELM_ARM64_EC_WFX:
        return relm_arm64_handle_wfx(vcpu);

    case RELM_ARM64_EC_FP_ASIMD:
        return relm_arm64_handle_fp_simd_trap(vcpu);
    case RELM_ARM64_EC_PAUTH:
        return relm_arm64_handle_pauth_trap(vcpu);
    case RELM_ARM64_EC_SVE:
        return relm_arm64_handle_sve_trap(vcpu);
    case RELM_ARM64_EC_SME:
        return relm_arm64_handle_sme_trap(vcpu);

    case RELM_ARM64_EC_BTI:
        return relm_arm64_handle_bti_exception(vcpu);
    case RELM_ARM64_EC_ILLEGAL_STATE:
        return relm_arm64_handle_illegal_execution_state(vcpu);
    case RELM_ARM64_EC_ERET:
        return relm_arm64_handle_eret_trap(vcpu);
    case RELM_ARM64_EC_FPAC:
        return relm_arm64_handle_pauth_fault(vcpu);

    case RELM_ARM64_EC_HVC64:
        return relm_arm64_handle_hvc(vcpu);
    case RELM_ARM64_EC_SMC64:
        return relm_arm64_handle_smc(vcpu);

    case RELM_ARM64_EC_SYS64:
        return relm_arm64_handle_sysreg(vcpu);

    case RELM_ARM64_EC_IABT_LOWER:
        return relm_arm64_handle_instruction_abort(vcpu);
    case RELM_ARM64_EC_DABT_LOWER:
        return relm_arm64_handle_data_abort(vcpu);

    case RELM_ARM64_EC_PC_ALIGNMENT:
        return relm_arm64_handle_pc_alignment(vcpu);
    case RELM_ARM64_EC_SP_ALIGNMENT:
        return relm_arm64_handle_sp_alignment(vcpu);
    case RELM_ARM64_EC_FP_EXCEPTION64:
        return relm_arm64_handle_fp_exception(vcpu);

    /* Debug exceptions from guest execution, not debug register accesses. */
    case RELM_ARM64_EC_BREAKPOINT_LOWER:
        return relm_arm64_handle_breakpoint(vcpu);
    case RELM_ARM64_EC_SINGLE_STEP_LOWER:
        return relm_arm64_handle_single_step(vcpu);
    case RELM_ARM64_EC_WATCHPOINT_LOWER:
        return relm_arm64_handle_watchpoint(vcpu);
    case RELM_ARM64_EC_BRK64:
        return relm_arm64_handle_brk(vcpu);

    /*
     * These are not normal guest synchronous exits in this backend.
     * With TGE=0, a guest userspace SVC belongs to guest Linux at EL1.
     * CURRENT classes describe EL2 faults, whose vectors do not use the
     * guest unwind path. SError must arrive through EXIT_SERROR instead.
     * Keep such inconsistent snapshots out of guest emulation handlers.
     */
    case RELM_ARM64_EC_SVC64:
    case RELM_ARM64_EC_IABT_CURRENT:
    case RELM_ARM64_EC_DABT_CURRENT:
    case RELM_ARM64_EC_BREAKPOINT_CURRENT:
    case RELM_ARM64_EC_SINGLE_STEP_CURRENT:
    case RELM_ARM64_EC_WATCHPOINT_CURRENT:
    case RELM_ARM64_EC_SERROR:
        return relm_arm64_handle_unexpected_sync(vcpu);

    /* Includes AArch32 classes and optional/newer classes not listed here. */
    default:
        return relm_arm64_handle_unhandled_sync(vcpu);
    }
}
