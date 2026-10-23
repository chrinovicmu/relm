#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/printk.h>

#include <asm/sysreg.h>
#include <asm/barrier.h>

#include <relm/vcpu.h>
#include <relm/vm.h>
#include <entry.h>
#include <stage2.h>

static int arm64_vcpu_alloc(struct vcpu *vcpu)
{
    if (!vcpu || !vcpu->vm)
        return -EINVAL;

    return 0;
}

static void arm64_vcpu_free(struct vcpu *vcpu)
{
    (void)vcpu;
}

static int relm_setup_guest_ctrls_vhe(struct vcpu *vcpu)
{
    struct relm_arm_guest_ctrls ctrls = {0}; 
    u64 host_hcr; 
    u64 pfr0; 
    u64 dfr0; 
    u64 mmfr1; 
    u64 pmuver; 

    if(read_sysreg(CurrentEl) != CurrentEl2){
        pr_err("RELM: VCPU%u: arm64: VHE requires the host kernel at EL2\n",
               vcpu->vpid);
        return -EOPNOTSUPP;
    }

    host_hcr = read_sysreg(hcr_el2); 
    if((host_hcr & (HCR_E2H | HCR_TGE)) != (HCR_E2H | HCR_TGE)){
        pr_err("RELM: VCPU%u: arm64: expected host E2H=1/TGE=1, HCR_EL2=%#llx\n",
               vcpu->vpid, host_hcr);
        return -EOPNOTSUPP;
    }

    pfr0 = read_sysreg(id_aa64pfr0_el1);
    dfr0 = read_sysreg(id_aa64dfr0_el1);
    mmfr1 = read_sysreg(id_aa64mmfr1_el1); 

    relm_arm64_enable_vhe(&ctrls);
    relm_arm64_clear_trap_general_exceptions(&ctrls);
    relm_arm64_set_execution_state_64bit(&ctrls);
    relm_arm64_enable_stage2_mmu(&ctrls);

    relm_arm64_route_physical_serror_to_el2(&ctrls);
    relm_arm64_route_physical_irq_to_el2(&ctrls);
    relm_arm64_route_physical_fiq_to_el2(&ctrls);

    relm_arm64_set_trap_wfi(&ctrls);
    relm_arm64_set_trap_wfe(&ctrls);
    relm_arm64_set_trap_smc(&ctrls);

    relm_arm64_set_trap_id_group3(&ctrls);
    relm_arm64_set_trap_id_group2(&ctrls);
    relm_arm64_set_trap_id_group1(&ctrls);

    relm_arm64_set_trap_auxiliary_control(&ctrls);
    relm_arm64_set_trap_implementation_defined(&ctrls);

    relm_arm64_set_trap_cache_set_way(&ctrls);
    relm_arm64_enable_set_way_invalidation_override(&ctrls);
    relm_arm64_set_trap_virtual_memory_writes(&ctrls);

    relm_arm64_enable_page_table_walk_protection(&ctrls);
    relm_arm64_set_barrier_upgrade_inner_shareable(&ctrls);
    relm_arm64_enable_force_broadcast(&ctrls);

    if (FIELD_GET(ID_AA64MMFR1_EL1_LO, mmfr1))
        relm_arm64_set_trap_lor(&controls);
    if (FIELD_GET(ID_AA64PFR0_EL1_RAS, pfr0))
        relm_arm64_set_trap_ras_error_records(&controls);

    relm_arm64_enable_fp_simd(&controls);

    /* TTA[28] traps trace-register access; TAM[30] traps AMU registers. */
    if (FIELD_GET(ID_AA64DFR0_EL1_TraceVer, dfr0))
        relm_arm64_set_trap_trace_access(&controls);
    if (FIELD_GET(ID_AA64PFR0_EL1_AMU, pfr0))
        relm_arm64_set_trap_activity_monitors(&controls);

    /*
     * MDCR_EL2 protects debug/performance registers that we do not yet
     * virtualize. TDA[9], TDRA[11], TDOSA[10] trap their respective debug
     * register groups. TDE[8]=0 leaves ordinary guest debug exceptions
     * (e.g. Linux userspace BRK -> SIGTRAP) to guest EL1. Register traps
     * alone do not save/disable the host's active breakpoint/watchpoint
     * banks; a proper host debug-state switch is still required.
     */
    relm_arm64_set_trap_debug_access(&controls);
    relm_arm64_set_trap_debug_rom_address(&controls);
    relm_arm64_set_trap_debug_os_access(&controls);
    pmuver = FIELD_GET(ID_AA64DFR0_EL1_PMUVer, dfr0);

    vcpu->arch.ctrls = ctrls;
    return 0; 

}
static int relm_setup_guest_state_aarch64(struct vcpu *vcpu)
{
    u64 pstate;

    if (!vcpu->vm->arch.kernel_entry_ipa) {
        pr_err("RELM: VCPU%u: arm64: vm->arch.kernel_entry_ipa not set "
               "(ARM64 image loader not run?)\n", vcpu->vpid);
        return -EINVAL;
    }
    if (!vcpu->vm->arch.dtb_ipa) {
        pr_err("RELM: VCPU%u: arm64: vm->arch.dtb_ipa not set "
               "(ARM64 DTB loader not run?)\n", vcpu->vpid);
        return -EINVAL;
    }

    pstate = PSR_MODE_EL1h | PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT;

    vcpu->arch.regs.pc     = vcpu->vm->arch.kernel_entry_ipa;
    vcpu->arch.regs.x0     = vcpu->vm->arch.dtb_ipa;
    vcpu->arch.regs.x1     = 0;
    vcpu->arch.regs.x2     = 0;
    vcpu->arch.regs.x3     = 0;
    vcpu->arch.regs.pstate = pstate;

    pr_info("RELM: VCPU%u: arm64: seeded pc=%#llx x0(dtb)=%#llx pstate=%#llx\n",
            vcpu->vpid, vcpu->arch.regs.pc, vcpu->arch.regs.x0,
            vcpu->arch.regs.pstate);

    return 0;
}

static int arm64_vcpu_init(struct vcpu *vcpu)
{
    int ret;

    if (!vcpu || !vcpu->vm)
        return -EINVAL;

    ret = relm_setup_guest_ctrls_vhe(vcpu); 
    if(ret)
        return ret; 

    ret = relm_setup_guest_state_aarch64(vcpu);
    if (ret)
        return ret;

    vcpu->arch.power_state = RELM_ARM64_POWER_ON;
    vcpu->arch.initialized = true;

    return 0;
}

struct relm_arm64_host_state {
    u64 hcr_el2;
    u64 vbar_el2;

    u64 sctlr_el1, cpacr_el1, ttbr0_el1, ttbr1_el1, tcr_el1;
    u64 mair_el1, amair_el1, vbar_el1, contextidr_el1, cntkctl_el1;
    u64 elr_el1, spsr_el1, esr_el1, afsr0_el1, afsr1_el1, far_el1;

    u64 cntv_cval, cntp_cval;
    u32 cntv_ctl, cntp_ctl;
};
static DEFINE_PER_CPU(struct relm_arm64_host_state, relm_arm64_host_state);

static int relm_arm64_guest_entry(struct vcpu *vcpu)
{
    struct relm_arm64_host_state *host = this_cpu_ptr(&relm_arm64_host_state); 
    struct relm_arm64_host_state *eli = &vcpu->arch.eli;
    struct relm_arm64_timer_state *time = &vcpu->arch.timer; 
    struct relm_stage2 *stage2 = vcpu->vm->arch.stage2; 
    u64 vttbr;
    int ret;
    
    if (!stage2)
        return -EINVAL;

    ret = relm_stage2_encode_vttbr(&stage2->geometry, stage2->root_pa,
                                   stage2->vmid, &vttbr);
    if (ret)
        return ret;


    /*Save host context*/ 
    host->hcr_el2 = read_sysreg(hcr_el2);
    host->vbar_el2 = read_sysreg(vbar_el2);
    host->sctlr_el1 = read_sysreg(sctlr_el1);
    host->cpacr_el1 = read_sysreg(cpacr_el1);
    host->ttbr0_el1 = read_sysreg(ttbr0_el1);
    host->ttbr1_el1 = read_sysreg(ttbr1_el1);
    host->tcr_el1 = read_sysreg(tcr_el1);
    host->mair_el1 = read_sysreg(mair_el1);
    host->amair_el1 = read_sysreg(amair_el1);
    host->vbar_el1 = read_sysreg(vbar_el1);
    host->contextidr_el1 = read_sysreg(contextidr_el1);
    host->cntkctl_el1 = read_sysreg(cntkctl_el1);
    host->elr_el1 = read_sysreg(elr_el1);
    host->spsr_el1 = read_sysreg(spsr_el1);
    host->esr_el1 = read_sysreg(esr_el1);
    host->afsr0_el1 = read_sysreg(afsr0_el1);
    host->afsr1_el1 = read_sysreg(afsr1_el1);
    host->far_el1 = read_sysreg(far_el1);
    host->cntv_cval = read_sysreg(cntv_cval_el0);
    host->cntv_ctl = (u32)read_sysreg(cntv_ctl_el0);
    host->cntp_cval = read_sysreg(cntp_cval_el0);
    host->cntp_ctl = (u32)read_sysreg(cntp_ctl_el0);

    write_sysreg(stage2->vtcr_el2, vtcr_el2);
    write_sysreg(vttbr, vttbr_el2); 
    isb();

    write_sysreg_el1(el1->sctlr_el1, SYS_SCTLR);
    write_sysreg_el1(el1->cpacr_el1, SYS_CPACR);
    write_sysreg_el1(el1->ttbr0_el1, SYS_TTBR0);
    write_sysreg_el1(el1->ttbr1_el1, SYS_TTBR1);
    write_sysreg_el1(el1->tcr_el1, SYS_TCR);
    write_sysreg_el1(el1->mair_el1, SYS_MAIR);
    write_sysreg_el1(el1->amair_el1, SYS_AMAIR);
    write_sysreg_el1(el1->vbar_el1, SYS_VBAR);
    write_sysreg_el1(el1->contextidr_el1, SYS_CONTEXTIDR);
    write_sysreg_el1(el1->cntkctl_el1, SYS_CNTKCTL);
    write_sysreg_el1(el1->elr_el1, SYS_ELR);
    write_sysreg_el1(el1->spsr_el1, SYS_SPSR);
    write_sysreg_el1(el1->esr_el1, SYS_ESR);
    write_sysreg_el1(el1->afsr0_el1, SYS_AFSR0);
    write_sysreg_el1(el1->afsr1_el1, SYS_AFSR1);
    write_sysreg_el1(el1->far_el1, SYS_FAR);

    write_sysreg(el1->csselr_el1, csselr_el1);
    write_sysreg(el1->tpidr_el1, tpidr_el1);
    write_sysreg(el1->mdscr_el1, mdscr_el1);
    write_sysreg(el1->tpidr_el0, tpidr_el0);
    write_sysreg(el1->tpidrro_el0, tpidrro_el0);
    write_sysreg(el1->par_el1, par_el1);
    write_sysreg(vcpu->arch.regs.sp_el1, sp_el1);

    write_sysreg_el0(timer->cntv_cval, SYS_CNTV_CVAL);
    write_sysreg_el0((u64)timer->cntv_ctl, SYS_CNTV_CTL);
    write_sysreg_el0(timer->cntp_cval, SYS_CNTP_CVAL);
    write_sysreg_el0((u64)timer->cntp_ctl, SYS_CNTP_CTL);

    write_sysreg(vcpu->arch.virtual_midr, vpidr_el2);
    write_sysreg(vcpu->arch.virtual_mpidr, vmpidr_el2);

    write_sysreg(vcpu->arch.ctrls.cptr_el2, cptr_el2);
    write_sysreg(vcpu->arch.ctrls.mdcr_el2, mdcr_el2);
    write_sysreg(vcpu->arch.ctrls.cnthctl_el2, cnthctl_el2);

    write_sysreg((u64)relm_hyp_vectors, vbar_el2);
    /* HCR_EL2 last: writing it is what clears TGE and starts routing
     * EL0/EL1 exceptions to relm_hyp_vectors instead of the host. */
    write_sysreg(vcpu->arch.ctrls.hcr_el2, hcr_el2);
    isb();

    return 0;
}

static void relm_arm64_guest_exit(struct vcpu *vcpu)
{
    struct relm_arm64_host_state *host = this_cpu_ptr(&relm_arm64_host_state);
    struct relm_arm64_el1_state *el1 = &vcpu->arch.el1;
    struct relm_arm64_timer_state *timer = &vcpu->arch.timer;

    /* HCR_EL2 first: restoring the host's own value re-enables TGE,
     * routing exceptions back to the host before anything else changes. */
    write_sysreg(host->hcr_el2, hcr_el2);
    isb();

    el1->sctlr_el1 = read_sysreg_el1(SYS_SCTLR);
    el1->cpacr_el1 = read_sysreg_el1(SYS_CPACR);
    el1->ttbr0_el1 = read_sysreg_el1(SYS_TTBR0);
    el1->ttbr1_el1 = read_sysreg_el1(SYS_TTBR1);
    el1->tcr_el1 = read_sysreg_el1(SYS_TCR);
    el1->mair_el1 = read_sysreg_el1(SYS_MAIR);
    el1->amair_el1 = read_sysreg_el1(SYS_AMAIR);
    el1->vbar_el1 = read_sysreg_el1(SYS_VBAR);
    el1->contextidr_el1 = read_sysreg_el1(SYS_CONTEXTIDR);
    el1->cntkctl_el1 = read_sysreg_el1(SYS_CNTKCTL);
    el1->elr_el1 = read_sysreg_el1(SYS_ELR);
    el1->spsr_el1 = read_sysreg_el1(SYS_SPSR);
    el1->esr_el1 = read_sysreg_el1(SYS_ESR);
    el1->afsr0_el1 = read_sysreg_el1(SYS_AFSR0);
    el1->afsr1_el1 = read_sysreg_el1(SYS_AFSR1);
    el1->far_el1 = read_sysreg_el1(SYS_FAR);

    el1->csselr_el1 = read_sysreg(csselr_el1);
    el1->tpidr_el1 = read_sysreg(tpidr_el1);
    el1->mdscr_el1 = read_sysreg(mdscr_el1);
    el1->tpidr_el0 = read_sysreg(tpidr_el0);
    el1->tpidrro_el0 = read_sysreg(tpidrro_el0);
    el1->par_el1 = read_sysreg(par_el1);
    vcpu->arch.regs.sp_el1 = read_sysreg(sp_el1);

    timer->cntv_cval = read_sysreg_el0(SYS_CNTV_CVAL);
    timer->cntv_ctl = (u32)read_sysreg_el0(SYS_CNTV_CTL);
    timer->cntp_cval = read_sysreg_el0(SYS_CNTP_CVAL);
    timer->cntp_ctl = (u32)read_sysreg_el0(SYS_CNTP_CTL);
    /* Mask both timers before leaving the guest context: their ENABLE
     * bits are still live in the real hardware comparators above, and
     * the host has no business receiving an interrupt sourced from a
     * deadline the GUEST programmed. */
    write_sysreg_el0(0, SYS_CNTV_CTL);
    write_sysreg_el0(0, SYS_CNTP_CTL);

    write_sysreg(host->vbar_el2, vbar_el2);
    write_sysreg(host->sctlr_el1, sctlr_el1);
    write_sysreg(host->cpacr_el1, cpacr_el1);
    write_sysreg(host->ttbr0_el1, ttbr0_el1);
    write_sysreg(host->ttbr1_el1, ttbr1_el1);
    write_sysreg(host->tcr_el1, tcr_el1);
    write_sysreg(host->mair_el1, mair_el1);
    write_sysreg(host->amair_el1, amair_el1);
    write_sysreg(host->vbar_el1, vbar_el1);
    write_sysreg(host->contextidr_el1, contextidr_el1);
    write_sysreg(host->cntkctl_el1, cntkctl_el1);
    write_sysreg(host->elr_el1, elr_el1);
    write_sysreg(host->spsr_el1, spsr_el1);
    write_sysreg(host->esr_el1, esr_el1);
    write_sysreg(host->afsr0_el1, afsr0_el1);
    write_sysreg(host->afsr1_el1, afsr1_el1);
    write_sysreg(host->far_el1, far_el1);
    write_sysreg(host->cntv_cval, cntv_cval_el0);
    write_sysreg((u64)host->cntv_ctl, cntv_ctl_el0);
    write_sysreg(host->cntp_cval, cntp_cval_el0);
    write_sysreg((u64)host->cntp_ctl, cntp_ctl_el0);
    isb();
}

static int arm64_vcpu_run(struct vcpu *vcpu)
{
    int code;
    int ret;

    ret = relm_arm64_guest_entry(vcpu);
    if (ret)
        return ret;

    code = relm_arm64_vcpu_run_asm(&vcpu->arch);

    relm_arm64_guest_exit(vcpu);

    switch (code) {
    case RELM_ARM64_EXIT_SYNC:
    case RELM_ARM64_EXIT_IRQ:
    case RELM_ARM64_EXIT_FIQ:
    case RELM_ARM64_EXIT_SERROR:
        vcpu->arch.exit_kind = code;
        return 0;
    default:
        pr_err("RELM: VCPU%u: entry.S returned unknown exit code %d\n",
               vcpu->vpid, code);
        return -EIO;
    }
}

static int arm64_handle_exit(struct vcpu *vcpu)
{
    return (vcpu->state == VCPU_STATE_ERROR) ? -EFAULT : 0;
}

static void arm64_vcpu_destroy(struct vcpu *vcpu)
{
    (void)vcpu;
}

static int arm64_setup_mmu(struct vcpu *vcpu)
{
    (void)vcpu;
    return 0;
}

static int arm64_inject_irq(struct vcpu *vcpu, unsigned int irq)
{
    (void)vcpu;
    (void)irq;
    return -EINVAL;
}

static int arm64_read_msr(struct vcpu *vcpu, uint32_t msr, uint64_t *val)
{
    (void)vcpu;
    (void)msr;
    (void)val;
    return -EINVAL;
}

static int arm64_write_msr(struct vcpu *vcpu, uint32_t msr, uint64_t val)
{
    (void)vcpu;
    (void)msr;
    (void)val;
    return -EINVAL;
}

static void arm64_dump_regs(struct vcpu *vcpu)
{
    const struct guest_regs *r;
    const struct relm_arm64_el1_state *el1;

    if (!vcpu)
        return;
    r = &vcpu->arch.regs;
    el1 = &vcpu->arch.el1;

    pr_info("RELM: VCPU%u (arm64) state=%d power_state=%d exit_kind=%d\n",
            vcpu->vpid, vcpu->state, vcpu->arch.power_state,
            vcpu->arch.exit_kind);

    pr_info("RELM: VCPU%u x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n",
            vcpu->vpid, r->x0, r->x1, r->x2, r->x3);
    pr_info("RELM: VCPU%u x4=%#llx x5=%#llx x6=%#llx x7=%#llx\n",
            vcpu->vpid, r->x4, r->x5, r->x6, r->x7);
    pr_info("RELM: VCPU%u x8=%#llx x9=%#llx x10=%#llx x11=%#llx\n",
            vcpu->vpid, r->x8, r->x9, r->x10, r->x11);
    pr_info("RELM: VCPU%u x12=%#llx x13=%#llx x14=%#llx x15=%#llx\n",
            vcpu->vpid, r->x12, r->x13, r->x14, r->x15);
    pr_info("RELM: VCPU%u x16=%#llx x17=%#llx x18=%#llx x19=%#llx\n",
            vcpu->vpid, r->x16, r->x17, r->x18, r->x19);
    pr_info("RELM: VCPU%u x20=%#llx x21=%#llx x22=%#llx x23=%#llx\n",
            vcpu->vpid, r->x20, r->x21, r->x22, r->x23);
    pr_info("RELM: VCPU%u x24=%#llx x25=%#llx x26=%#llx x27=%#llx\n",
            vcpu->vpid, r->x24, r->x25, r->x26, r->x27);
    pr_info("RELM: VCPU%u x28=%#llx x29=%#llx x30=%#llx\n",
            vcpu->vpid, r->x28, r->x29, r->x30);
    pr_info("RELM: VCPU%u sp_el0=%#llx sp_el1=%#llx pc=%#llx pstate=%#llx\n",
            vcpu->vpid, r->sp_el0, r->sp_el1, r->pc, r->pstate);
    pr_info("RELM: VCPU%u esr_el2=%#llx far_el2=%#llx hpfar_el2=%#llx\n",
            vcpu->vpid, r->esr_el2, r->far_el2, r->hpfar_el2);

    pr_info("RELM: VCPU%u sctlr_el1=%#llx tcr_el1=%#llx mair_el1=%#llx\n",
            vcpu->vpid, el1->sctlr_el1, el1->tcr_el1, el1->mair_el1);
    pr_info("RELM: VCPU%u ttbr0_el1=%#llx ttbr1_el1=%#llx vbar_el1=%#llx\n",
            vcpu->vpid, el1->ttbr0_el1, el1->ttbr1_el1, el1->vbar_el1);
    pr_info("RELM: VCPU%u elr_el1=%#llx spsr_el1=%#llx esr_el1=%#llx far_el1=%#llx\n",
            vcpu->vpid, el1->elr_el1, el1->spsr_el1, el1->esr_el1, el1->far_el1);

    pr_info("RELM: VCPU%u cntv_cval=%#llx cntv_ctl=%#x cntp_cval=%#llx cntp_ctl=%#x\n",
            vcpu->vpid, vcpu->arch.timer.cntv_cval, vcpu->arch.timer.cntv_ctl,
            vcpu->arch.timer.cntp_cval, vcpu->arch.timer.cntp_ctl);

    pr_info("RELM: VCPU%u hcr_el2=%#llx cptr_el2=%#llx mdcr_el2=%#llx cnthctl_el2=%#llx\n",
            vcpu->vpid, vcpu->arch.ctrls.hcr_el2, vcpu->arch.ctrls.cptr_el2,
            vcpu->arch.ctrls.mdcr_el2, vcpu->arch.ctrls.cnthctl_el2);

    pr_info("RELM: VCPU%u virtual_midr=%#llx virtual_mpidr=%#llx\n",
            vcpu->vpid, vcpu->arch.virtual_midr, vcpu->arch.virtual_mpidr);
}

const struct vcpu_arch_ops arm64_vcpu_ops = {
    .vcpu_alloc   = arm64_vcpu_alloc,
    .vcpu_init    = arm64_vcpu_init,
    .vcpu_run     = arm64_vcpu_run,
    .handle_exit  = arm64_handle_exit,
    .vcpu_free    = arm64_vcpu_free,
    .vcpu_destroy = arm64_vcpu_destroy,
    .setup_mmu    = arm64_setup_mmu,
    .inject_irq   = arm64_inject_irq,
    .read_msr     = arm64_read_msr,
    .write_msr    = arm64_write_msr,
    .dump_regs    = arm64_dump_regs,
};
