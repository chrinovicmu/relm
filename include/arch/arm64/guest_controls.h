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


