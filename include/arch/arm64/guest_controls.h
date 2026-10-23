/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_ARCH_ARM64_GUEST_ctrls_H
#define RELM_ARCH_ARM64_GUEST_ctrls_H

#include <hyp_regs.h>
#include <vcpu_arch.h>


static inline void relm_arm64_enable_vhe(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_E2H;
}

static inline void relm_arm64_set_trap_general_exceptions(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TGE;
}

static inline void relm_arm64_clear_trap_general_exceptions(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TGE;
}

static inline void relm_arm64_set_execution_state_64bit(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_RW;
}
static inline void relm_arm64_set_execution_state_32bit(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_RW;
}

static struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_VM;
}

static inline void relm_arm64_disable_stage2_mmu(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_VM;
}

/*
 * AMO[5] - physical SError (System Error) routing to EL2 with TGE=0,
 * subject to higher-level routing. The host owns physical error handling.
 * Clear removes this routing request; neither helper injects a virtual
 * SError into guest Linux or supplies an error-recovery policy.
 */
static inline void relm_arm64_route_physical_serror_to_el2(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_AMO;
}

static inline void relm_arm64_clear_physical_serror_routing(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_AMO;
}

/*
 * IMO[4] - physical IRQ (Interrupt Request) routing to EL2 with TGE=0.
 * This lets the host service physical devices when an interrupt arrives
 * during guest execution, subject to higher-level routing. Clear removes
 * this request. Delivering an IRQ to guest Linux needs the virtual GIC.
 */
static inline void relm_arm64_route_physical_irq_to_el2(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_IMO;
}

static inline void relm_arm64_clear_physical_irq_routing(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_IMO;
}

/*
 * FMO[3] - physical FIQ (Fast Interrupt Request) routing to EL2 with
 * TGE=0, subject to higher-level routing. This keeps host interrupt work
 * under host control. Clear removes this request; it does not make a
 * virtual FIQ pending in the guest.
 */
static inline void relm_arm64_route_physical_fiq_to_el2(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_FMO;
}

static inline void relm_arm64_clear_physical_fiq_routing(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_FMO;
}

/*
 * TWI[13] - Trap Wait For Interrupt. Guest Linux can use WFI in its idle
 * loop; trapping a wait lets RELM block/yield the vCPU. A WFI that completes
 * immediately need not trap. Clear removes this interception; the set
 * helper does not itself implement the idle exit handler.
 */
static inline void relm_arm64_set_trap_wfi(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TWI;
}

static inline void relm_arm64_clear_trap_wfi(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TWI;
}


/*
 * TWE[14] - Trap Wait For Event. Linux synchronization code may wait with
 * WFE. Set requests an exit for an eligible wait; an already-pending event
 * can let WFE complete without trapping. Clear removes this trap request.
 */
static inline void relm_arm64_set_trap_wfe(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TWE;
}

static inline void relm_arm64_clear_trap_wfe(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TWE;
}

/*
 * TSC[19] - Trap Secure Monitor Call. Linux may use SMC for firmware/PSCI
 * services. Set requests interception at EL2 so RELM can handle the VM's
 * request; clear removes this trap. Neither supplies PSCI emulation or
 * changes the separate HVC enable policy.
 */
static inline void relm_arm64_set_trap_smc(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TSC;
}

static inline void relm_arm64_clear_trap_smc(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TSC;
}

/*
 * TID3[18] - Trap identification-register group 3, including feature
 * reads such as ID_AA64PFR0_EL1. Linux uses these to select instructions
 * and alternatives. Set lets handlers return the VM's feature_regs;
 * clear removes this group's trap, without changing the advertised values.
 */
static inline void relm_arm64_set_trap_id_group3(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TID3;
}

static inline void relm_arm64_clear_trap_id_group3(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TID3;
}

/*
 * TID2[17] - Trap identification-register group 2: covered cache ID and
 * selection accesses, including CTR/CLIDR/CCSIDR/CSSELR. Linux probes
 * these for cache geometry and maintenance. Set requires a virtual cache
 * model in the handler; clear removes this group's interception.
 */
static inline void relm_arm64_set_trap_id_group2(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TID2;
}

static inline void relm_arm64_clear_trap_id_group2(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TID2;
}

/*
 * TID1[16] - Trap identification-register group 1, including revision
 * information such as REVIDR_EL1. Set keeps these Linux CPU-identification
 * reads under VM policy; clear removes the group trap. These ID groups
 * do not replace VPIDR_EL2/VMPIDR_EL2 for the guest's MIDR/MPIDR identity.
 */
static inline void relm_arm64_set_trap_id_group1(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TID1;
}

static inline void relm_arm64_clear_trap_id_group1(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TID1;
}

/*
 * TACR[21] - Trap Auxiliary Control Register accesses, ACTLR_EL1.
 * ACTLR contains CPU-specific ctrls. Set intercepts guest access so
 * guest Linux cannot directly reconfigure host settings; clear removes
 * this interception. It does not initialize the guest's ACTLR value.
 */
static inline void relm_arm64_set_trap_auxiliary_control(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TACR;
}

static inline void relm_arm64_clear_trap_auxiliary_control(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TACR;
}

/*
 * TIDCP[20] - Trap the implementation-defined accesses covered by this
 * field. Set keeps those CPU-specific operations under RELM's control;
 * clear removes this trap. Coverage depends on the architecture/CPU and
 * is not a universal trap for every implementation-defined register.
 */
static inline void relm_arm64_set_trap_implementation_defined(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TIDCP;
}

static inline void relm_arm64_clear_trap_implementation_defined(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TIDCP;
}


/*
 * TSW[22] - Trap cache maintenance by Set/Way. Such instructions select
 * physical cache entries, not a guest address. Set intercepts guest Linux
 * maintenance for host coordination; clear removes this trap. Neither
 * executes the maintenance operation or emulates it.
 */
static inline void relm_arm64_set_trap_cache_set_way(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TSW;
}

static inline void relm_arm64_clear_trap_cache_set_way(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TSW;
}


/*
 * SWIO[1] - Set/Way Invalidation Override. Enable upgrades applicable
 * executed invalidations to clean+invalidate, preserving dirty data;
 * disable removes that override. If TSW traps the instruction, software
 * still has to handle it: SWIO does not perform the trapped operation.
 */
static inline void relm_arm64_enable_set_way_invalidation_override(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_SWIO;
}

static inline void relm_arm64_disable_set_way_invalidation_override(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_SWIO;
}

/*
 * TVM[26] - Trap Virtual Memory control writes, including the covered
 * SCTLR_EL1, TTBR0/1_EL1 and TCR_EL1 writes. Linux uses them to configure
 * stage 1 and enable its MMU/caches. Set lets RELM enforce its cache policy.
 * Clear only after the handler has made native operation safe; clearing
 * is not a substitute for handling the MMU/cache-off boot transition.
 */
static inline void relm_arm64_set_trap_virtual_memory_writes(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_TVM;
}

static inline void relm_arm64_clear_trap_virtual_memory_writes(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_TVM;
}

/*
 * PTW[2] - protection for stage-1 Page Table Walks through stage 2.
 * Enable faults a walk into memory classified as Device by stage 2;
 * Linux's page tables belong in RAM. Disable removes this protection,
 * without changing the tables or the separate stage-2 enable.
 */
static inline void relm_arm64_enable_page_table_walk_protection(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_PTW;
}

static inline void relm_arm64_disable_page_table_walk_protection(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_PTW;
}


/*
 * BSU[11:10] - Barrier Shareability Upgrade. Set 01 so applicable guest
 * barriers cover at least the Inner Shareable domain; clear selects 00
 * (no upgrade). Replace the whole field so a previous bit 11 cannot turn
 * the requested 01 into 11. This does not execute a barrier itself.
 */
static inline void relm_arm64_set_barrier_upgrade_inner_shareable(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 = (ctrls->hcr_el2 & ~HCR_BSU_MASK) | HCR_BSU_IS;
}

static inline void relm_arm64_clear_barrier_upgrade(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_BSU_MASK;
}

/*
 * FB[9] - Force Broadcast for the maintenance operations covered by this
 * field. Enable broadens their visibility when guest Linux performs them;
 * disable removes the override. RELM still needs its own DSB/ISB/TLBI
 * sequences when changing translation state.
 */
static inline void relm_arm64_enable_force_broadcast(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 |= HCR_FB;
}

static inline void relm_arm64_disable_force_broadcast(
    struct relm_arm64_guest_ctrls *ctrls)
{
    ctrls->hcr_el2 &= ~HCR_FB;
} 

/*
 * TLOR[35] - Trap Limited Ordering Region register accesses. LOR controls
 * memory ordering in configured regions. Set protects host LOR state
 * from guest access; clear removes this interception. The caller must
 * establish FEAT_LOR support before using this optional control.
 */
static inline void relm_arm64_set_trap_lor(
    struct relm_arm64_guest_controls *controls)
{
    controls->hcr_el2 |= HCR_TLOR;
}

static inline void relm_arm64_clear_trap_lor(
    struct relm_arm64_guest_controls *controls)
{
    controls->hcr_el2 &= ~HCR_TLOR;
}

/*
 * TERR[36] - Trap RAS error-record accesses. Reliability, Availability
 * and Serviceability registers describe hardware errors. Set intercepts
 * covered guest accesses; clear removes the trap. Requires FEAT_RAS and
 * is separate from AMO's routing of a physical SError exception.
 */
static inline void relm_arm64_set_trap_ras_error_records(
    struct relm_arm64_guest_controls *controls)
{
    controls->hcr_el2 |= HCR_TERR;
}

static inline void relm_arm64_clear_trap_ras_error_records(
    struct relm_arm64_guest_controls *controls)
{
    controls->hcr_el2 &= ~HCR_TERR;


/*
 * FPEN[21:20] - Floating-point/Advanced SIMD access control. Enable
 * selects 11, permitting the Q-register/FPSR/FPCR switch in entry.S.
 * Disable selects 00, trapping covered accesses at EL2 and below; do not
 * install it while RELM's unconditional FP save/restore needs access.
 * Guest Linux separately manages its EL1/EL0 policy through CPACR_EL1.
 * These helpers do not save the host's FP bank or acquire its ownership.
 */
static inline void relm_arm64_enable_fp_simd(
    struct relm_arm64_guest_controls *controls)
{
    controls->cptr_el2 =
        (controls->cptr_el2 & ~CPTR_EL2_VHE_FPEN_MASK) |
        CPTR_EL2_VHE_FPEN_MASK;
}

static inline void relm_arm64_disable_fp_simd(
    struct relm_arm64_guest_controls *controls)
{
    controls->cptr_el2 &= ~CPTR_EL2_VHE_FPEN_MASK;
}


/*
 * TTA[28, VHE] - Trap Trace Access. Set intercepts the implemented trace
 * system-register interface so guest software cannot configure host
 * tracing; clear removes this interception. Requires the trace interface.
 * The non-VHE TTA field is at a different bit and is not used here.
 */
static inline void relm_arm64_set_trap_trace_access(
    struct relm_arm64_guest_controls *controls)
{
    controls->cptr_el2 |= CPTR_EL2_VHE_TTA;
}

static inline void relm_arm64_clear_trap_trace_access(
    struct relm_arm64_guest_controls *controls)
{
    controls->cptr_el2 &= ~CPTR_EL2_VHE_TTA;
}


/*
 * TAM[30] - Trap Activity Monitors Unit accesses. When AMU exists, set
 * intercepts guest activity-counter/control accesses; clear removes this
 * trap. The physical activity counters remain a host resource unless
 * RELM implements a guest model and the associated state handling.
 */
static inline void relm_arm64_set_trap_activity_monitors(
    struct relm_arm64_guest_controls *controls)
{
    controls->cptr_el2 |= CPTR_EL2_TAM;
}

static inline void relm_arm64_clear_trap_activity_monitors(
    struct relm_arm64_guest_controls *controls)
{
    controls->cptr_el2 &= ~CPTR_EL2_TAM;
}

/* MDCR_EL2 - Monitor Debug Configuration Register, EL2. */

/*
 * TDA[9] - Trap Debug register Accesses, including covered breakpoint and
 * watchpoint controls. Set retains host control; clear removes this trap.
 * Linux debug-exception delivery is separate (TDE). These helpers do not
 * save/disable any already-active host breakpoint or watchpoint.
 */
static inline void relm_arm64_set_trap_debug_access(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 |= MDCR_EL2_TDA;
}

static inline void relm_arm64_clear_trap_debug_access(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 &= ~MDCR_EL2_TDA;
}

/*
 * TDRA[11] - Trap Debug ROM Address access, MDRAR_EL1 in AArch64.
 * Debug ROM tables describe discoverable debug components. Set keeps
 * this guest discovery read under RELM policy; clear removes the trap.
 */
static inline void relm_arm64_set_trap_debug_rom_address(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 |= MDCR_EL2_TDRA;
}

static inline void relm_arm64_clear_trap_debug_rom_address(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 &= ~MDCR_EL2_TDRA;
}

/*
 * TDOSA[10] - Trap Debug OS-related accesses: the applicable debug lock,
 * status and power interfaces. Set intercepts guest debug management;
 * clear removes this trap. Exact coverage depends on the implemented
 * debug architecture, including any fine-grained trap controls.
 */
static inline void relm_arm64_set_trap_debug_os_access(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 |= MDCR_EL2_TDOSA;
}

static inline void relm_arm64_clear_trap_debug_os_access(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 &= ~MDCR_EL2_TDOSA;
}

/*
 * TPM[6] - Trap Performance Monitor accesses. When a PMU exists, set
 * intercepts the covered guest accesses (for example from Linux perf);
 * clear removes this trap. Allowing access does not virtualize counters.
 */
static inline void relm_arm64_set_trap_pmu_access(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 |= MDCR_EL2_TPM;
}

static inline void relm_arm64_clear_trap_pmu_access(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 &= ~MDCR_EL2_TPM;
}

/*
 * TPMCR[5] - Trap Performance Monitors Control Register, PMCR_EL0.
 * Set intercepts guest PMU reset/enable controls; clear removes this
 * separate trap. Call only when PMU support is established.
 */
static inline void relm_arm64_set_trap_pmu_control(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 |= MDCR_EL2_TPMCR;
}

static inline void relm_arm64_clear_trap_pmu_control(
    struct relm_arm64_guest_controls *controls)
{
    controls->mdcr_el2 &= ~MDCR_EL2_TPMCR;
}


