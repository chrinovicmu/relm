#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <asm/processor.h>   
#include <asm/msr.h>         

#include <svm.h>
#include <utils/utils.h>

bool relm_svm_check_support(void)
{
    uint32_t ecx, edx;
    uint64_t vm_cr;
    int cpu = smp_processor_id();

    /* Gate 1: does the silicon have SVM at all. */
    ecx = cpuid_ecx(SVM_CPUID_EXT_FEATURES);
    if (!(ecx & SVM_CPUID_FEATURE_BIT)) {
        pr_err("RELM: CPU%d: SVM not supported by CPUID (0x80000001 ECX=0x%x)\n",
               cpu, ecx);
        return false;
    }

    edx = cpuid_edx(SVM_CPUID_SVM_FEATURES);

    vm_cr = 0;
    rdmsrl(MSR_VM_CR, vm_cr);

    if (vm_cr & MSR_VM_CR_SVMDIS) {
        if (vm_cr & MSR_VM_CR_LOCK)
            pr_err("RELM: CPU%d: SVM disabled AND locked by BIOS "
                   "(VM_CR=0x%llx) — cannot be re-enabled until reboot\n",
                   cpu, vm_cr);
        else
            pr_err("RELM: CPU%d: SVM currently disabled by BIOS "
                   "(VM_CR=0x%llx)\n", cpu, vm_cr);
        return false;
    }

    PDEBUG("RELM: CPU%d: SVM support verified (ext_features_edx=0x%x, VM_CR=0x%llx)\n",
           cpu, edx, vm_cr);

    return true;
}

void relm_enable_svm_operation(void)
{
    uint64_t efer = 0;
    int cpu = smp_processor_id();

    rdmsrl(MSR_EFER, efer);

    if (!(efer & EFER_SVME)) {
        efer |= EFER_SVME;
        wrmsrl(MSR_EFER, efer);
    }

    /* Verify — mirrors relm_enable_vmx_operation()'s CR4 re-read. */
    efer = 0;
    rdmsrl(MSR_EFER, efer);
    if (!(efer & EFER_SVME)) {
        pr_err("RELM: CPU%d: EFER.SVME did not stick (EFER=0x%llx)\n",
               cpu, efer);
        return;
    }

    pr_info("RELM: CPU%d: SVM operation enabled\n", cpu);
}

struct svm_enable_work {
    atomic_t failed_cpus;
};

static void relm_svm_enable_cpu(void *arg)
{
    struct svm_enable_work *work = arg;
    int cpu = smp_processor_id();

    if (!relm_svm_check_support()) {
        pr_err("RELM: CPU%d: SVM support/permission check failed, "
               "cannot enable\n", cpu);
        atomic_inc(&work->failed_cpus);
        return;
    }

    relm_enable_svm_operation();
}

static void relm_svm_disable_cpu(void *unused)
{
    uint64_t efer = 0;
    int cpu = smp_processor_id();

    (void)unused;

    rdmsrl(MSR_EFER, efer);

    if (efer & EFER_SVME) {
        efer &= ~EFER_SVME;
        wrmsrl(MSR_EFER, efer);
    }

    PDEBUG("RELM: CPU%d: SVM operation disabled\n", cpu);
}

int relm_svm_enable_on_all_cpus(void)
{
    struct svm_enable_work work;

    atomic_set(&work.failed_cpus, 0);

    pr_info("RELM: Enabling SVM on all %d online CPUs\n",
            num_online_cpus());

    on_each_cpu(relm_svm_enable_cpu, &work, 1);

    if (atomic_read(&work.failed_cpus) > 0) {
        pr_err("RELM: SVM enable failed on %d CPU(s)\n",
               atomic_read(&work.failed_cpus));
        relm_svm_disable_on_all_cpus();
        return -EIO;
    }

    pr_info("RELM: SVM enabled on all CPUs\n");

    return 0;
}

void relm_svm_disable_on_all_cpus(void)
{
    pr_info("RELM: Disabling SVM on all CPUs\n");

    on_each_cpu(relm_svm_disable_cpu, NULL, 1);

    pr_info("RELM: SVM disabled on all CPUs\n");
}

/* relm_vcpu_alloc_init() — Phase 1 vCPU bring-up.
 *
 * Called by relm_vcpu_create() (src/core/vcpu.c) as vcpu->ops->vcpu_alloc,
 * AFTER it has already kzalloc'd *vcpu, set vcpu->vm/vpid/state, and
 * initialised the lock/waitqueue/host_stack — all arch-agnostic and
 * already done for us. Our only job here is everything SVM-specific that
 * can happen on ANY cpu (no VMRUN, no MSR_VM_HSAVE_PA write — that needs
 * the vCPU pinned to its target CPU first, a later Phase-2 step, the SVM
 * analogue of VMX's VMPTRLD-requires-the-pinned-CPU rule).
 *
 * Five allocations, each a physically-contiguous, zeroed block:
 *
 *   vmcb        1 page  (4 KiB)  - struct vmcb, 4 KiB aligned by construction
 *                                  (__get_free_page always returns a page-
 *                                  aligned address, and PAGE_SIZE == 4 KiB
 *                                  satisfies the VMCB's alignment rule)
 *   hsave       1 page  (4 KiB)  - opaque host-save area, see vcpu_arch.h
 *   host_vmcb   1 page  (4 KiB)  - scratch VMSAVE target for HOST state
 *   msrpm       2 pages (8 KiB)  - order 1
 *   iopm        3 pages (12 KiB) - rounded to order 2 (4 pages, 16 KiB):
 *                                  the buddy allocator only hands out
 *                                  power-of-two page counts, and 3 isn't
 *                                  one. The 4th page is inert padding —
 *                                  VMCB.iopm_base_pa always points at the
 *                                  base of a 3-page-wide region the CPU
 *                                  reads, so the extra page is simply
 *                                  never consulted. (VMX sidesteps this
 *                                  by using two SEPARATE 4 KiB bitmaps
 *                                  instead of one 3-page one.)
 */
int relm_vcpu_alloc_init(struct vcpu *vcpu)
{
    if (!vcpu)
        return -EINVAL;

    /* vmcb — control area + save area, one 4 KiB page. */
    vcpu->arch.vmcb = (struct vmcb *)__get_free_page(
        GFP_KERNEL | __GFP_ZERO);
    if (!vcpu->arch.vmcb) {
        pr_err("RELM: VCPU%d: VMCB alloc failed\n", vcpu->vpid);
        return -ENOMEM;
    }
    vcpu->arch.vmcb_pa = virt_to_phys(vcpu->arch.vmcb);

    vcpu->arch.hsave = (void *)__get_free_page(
        GFP_KERNEL | __GFP_ZERO);
    if (!vcpu->arch.hsave) {
        pr_err("RELM: VCPU%d: host-save-area alloc failed\n", 
               vcpu->vpid);
        goto _out_free_vmcb;
    }
    vcpu->arch.hsave_pa = virt_to_phys(vcpu->arch.hsave);

    /* host_vmcb — scratch VMSAVE target for the host's FS/GS/TR/LDTR
     * hidden state + syscall/SYSENTER MSRs around every VMRUN */
    vcpu->arch.host_vmcb = (struct vmcb *)__get_free_page(
        GFP_KERNEL | __GFP_ZERO);
    if (!vcpu->arch.host_vmcb) {
        pr_err("RELM: VCPU%d: host scratch VMCB alloc failed\n", 
               vcpu->vpid);
        goto _out_free_hsave;
    }
    vcpu->arch.host_vmcb_pa = virt_to_phys(vcpu->arch.host_vmcb);

    /* msrpm — 8 KiB, 2 pages, order 1. Zero = every MSR passes through*/ 
    vcpu->arch.msrpm = (void *)__get_free_pages(
        GFP_KERNEL | __GFP_ZERO, get_order(2 * PAGE_SIZE));
    if (!vcpu->arch.msrpm) {
        pr_err("RELM: VCPU%d: MSRPM alloc failed\n",
               vcpu->vpid);
        goto _out_free_host_vmcb;
    }
    vcpu->arch.msrpm_pa = virt_to_phys(vcpu->arch.msrpm);

    /* iopm — 12 KiB logically, order-2 (4 pages / 16 KiB) physically;
     * every io passesthrough*/
    vcpu->arch.iopm = (uint8_t *)__get_free_pages(
        GFP_KERNEL| __GFP_ZERO, get_order(3 * PAGE_SIZE));
    if (!vcpu->arch.iopm) {
        pr_err("RELM: VCPU%d: IOPM alloc failed\n", 
               vcpu->vpid);
        goto _out_free_msrpm;
    }
    vcpu->arch.iopm_pa = virt_to_phys(vcpu->arch.iopm);

    vcpu->arch.asid = SVM_ASID_FROM_VPID(vcpu->vpid);

    pr_info("RELM: VCPU%d: SVM Phase 1 complete "
            "(vmcb_pa=0x%llx hsave_pa=0x%llx host_vmcb_pa=0x%llx "
            "msrpm_pa=0x%llx iopm_pa=0x%llx asid=%u)\n",
            vcpu->vpid, vcpu->arch.vmcb_pa, vcpu->arch.hsave_pa,
            vcpu->arch.host_vmcb_pa, vcpu->arch.msrpm_pa,
            vcpu->arch.iopm_pa, vcpu->arch.asid);

    return 0;

_out_free_msrpm:
    free_pages((unsigned long)vcpu->arch.msrpm, 
               get_order(2 * PAGE_SIZE));
_out_free_host_vmcb:
    free_page((unsigned long)vcpu->arch.host_vmcb);
_out_free_hsave:
    free_page((unsigned long)vcpu->arch.hsave);
_out_free_vmcb:
    free_page((unsigned long)vcpu->arch.vmcb);
    return -ENOMEM;
}

/*VMCB Constrol Area*/ 
int relm_vcpu_vmcb_setup(struct vcpu *vcpu)
{
    struct vmcb_control_area *ctrl ;
    struct svm_intercepts *icpt; 

    if (!vcpu || !vcpu->vm || !vcpu->arch.vmcb)
        return -EINVAL;

    if (!vcpu->vm->arch.npt) {
        pr_err("RELM: VCPU%d: no NPT context — nested_cr3 unavailable, "
               "cannot arm this VMCB\n", vcpu->vpid);
        return -ENODEV;
    }

    /* relm does not need to intercept any R/W to CR/DR*/ 
    ctrl->cr_intercepts = 0; 
    ctrl->dr_intercepts = 0; 

    /*
     * Exceptions: #UD(6) so the decoder/emulator path can identify
     * instructions RELM emulates by trapping their fetch, #PF(14) for
     * diagnostic visibility into guest page faults
     */
    icpt->exceptions = INTERCEPT_EXCEPTION(6) | INTERCEPT_EXCEPTION(14);

    icpt->intercepts_1 = INTERCEPT_HLT | INTERCEPT_CPUID |
                         INTERCEPT_IOIO_PROT | INTERCEPT_MSR_PROT |
                         INTERCEPT_SHUTDOWN;

    icpt->intercepts_2 = INTERCEPT_VMRUN | INTERCEPT_VMMCALL;

    icpt->intercepts_3 = 0;
    ctrl->intercept_cr = icpt->cr_intercepts;
    ctrl->intercept_dr = icpt->dr_intercepts;
    ctrl->intercept_exceptions = icpt->exceptions;
    ctrl->intercept_1 = icpt->intercepts_1;
    ctrl->intercept_2 = icpt->intercepts_2;
    ctrl->intercept_3 = icpt->intercepts_3;

    ctrl->asid = vcpu->arch.asid;

    /*TLB flush for this vCPU's first VMRUN */ 
    ctrl->tlb_ctl = TLB_CONTROL_FLUSH_ASID;

    ctrl->iopm_base_pa  = vcpu->arch.iopm_pa;
    ctrl->msrpm_base_pa = vcpu->arch.msrpm_pa;

    ctrl->nested_ctl = NESTED_CTL_NP_ENABLE;
    ctrl->nested_cr3 = vcpu->vm->arch.npt->pml4_pa;

    PDEBUG("RELM: VCPU%d: VMCB control area armed "
           "(asid=%u nested_cr3=0x%llx iopm_pa=0x%llx msrpm_pa=0x%llx)\n",
           vcpu->vpid, ctrl->asid, ctrl->nested_cr3,
           ctrl->iopm_base_pa, ctrl->msrpm_base_pa);

    return 0;
} 

/*
 * svm_init_guest_state_firmware() — 16-bit real mode, SeaBIOS reset
 * vector. Segment values are byte-for-byte the same architectural facts
 * VMX's relm_setup_guest_state_firmware() writes (both backends reuse
 * the exact same SEABIOS_INITIAL_* constants from firmware/seabios.h —
 * these describe the x86 RESET STATE, not anything VMX- or SVM-specific).
 * Only the mechanism of writing them differs: vmcb->save.<field> = value
 * here, versus a VMWRITE there.
 */
static int svm_init_guest_state_firmware(struct vcpu *vcpu)
{
    struct vmcb_save_area *save;
    uint64_t host_pat = 0;

    if (!vcpu || !vcpu->arch.vmcb)
        return -EINVAL;

    save = &vcpu->arch.vmcb->save;

    /* CS: selector 0xF000, base 0xFFFF0000 — linear base+RIP lands on
     * the reset vector at 0xFFFFFFF0, same as the VMX backend's
     * GUEST_CS_BASE programming (relm_mmu_rip_to_linear()'s reasoning
     * applies unchanged, per svm.h's original design note). */
    save->cs.selector = SEABIOS_INITIAL_CS_SELECTOR;      /* 0xF000     */
    save->cs.base = SEABIOS_INITIAL_CS_BASE;          /* 0xFFFF0000 */
    save->cs.limit = SEABIOS_INITIAL_CS_LIMIT;         /* 0xFFFF     */
    save->cs.attrib = SEABIOS_INITIAL_CS_ACCESS;        /* 0x9B: type=B
                                                            * (exec/read),
                                                            * S=1,DPL=0,P=1;
                                                            * high nibble 0
                                                            * (16-bit seg,
                                                            * no G/DB/L)   */

    save->ds = save->es = save->fs = save->gs = save->ss = (struct vmcb_seg){
        .selector = SEABIOS_INITIAL_DS_SELECTOR,
        .base = SEABIOS_INITIAL_DS_BASE,
        .limit = SEABIOS_INITIAL_DS_LIMIT,
        .attrib = SEABIOS_INITIAL_DS_ACCESS,   /* 0x93: type=3 (r/w
                                                   * data), S=1,DPL=0,P=1 */
    };

    /* LDTR: present-but-unused 16-bit LDT descriptor (P=1, no separate
     * "unusable" bit on SVM the way VMX has bit 16 of AR_BYTES — a
     * segment is simply "unusable" here by having P=0, but the real
     * reset-state LDTR descriptor IS marked present, so we carry that
     * faithfully rather than forcing P=0). */
    save->ldtr.selector = 0;
    save->ldtr.base = 0;
    save->ldtr.limit = 0xFFFF;
    save->ldtr.attrib = SEABIOS_INITIAL_LDTR_ACCESS;  /* 0x82 */

    /* TR: minimal present 16-bit TSS, busy. */
    save->tr.selector = 0;
    save->tr.base = 0;
    save->tr.limit = 0x67;   /* 104 bytes: minimum TSS */
    save->tr.attrib = SEABIOS_INITIAL_TR_ACCESS;      /* 0x8B */

    /* GDTR/IDTR: base/limit only — selector/attrib are meaningless for
     * these two and read as zero (struct vmcb_seg comment, vmcb.h). */
    save->gdtr.base  = SEABIOS_INITIAL_GDTR_BASE;   /* 0      */
    save->gdtr.limit = SEABIOS_INITIAL_GDTR_LIMIT;  /* 0xFFFF */
    save->idtr.base  = SEABIOS_INITIAL_IDTR_BASE;   /* 0      */
    save->idtr.limit = 0x3FF;   /* real-mode IVT: 256 x 4 bytes */

    save->cr0 = SEABIOS_INITIAL_CR0;   /* 0x60000010: CD|NW|ET, PE=0, PG=0 */
    save->cr4 = SEABIOS_INITIAL_CR4;   /* 0                                */
    save->cr3 = 0;                     /* no paging yet                   */

    save->efer = SEABIOS_INITIAL_EFER | EFER_SVME;   /* 0 | SVME */

    save->rip = SEABIOS_INITIAL_RIP;   /* 0xFFF0 */
    save->rsp = 0;
    save->rflags = SEABIOS_INITIAL_RFLAGS; /* 0x0002 */
    save->cpl = 0;

    /* Host PAT, or NPT's memory-type combining treats an all-zero guest
     * PAT as "everything Uncacheable" */
    rdmsrl(MSR_IA32_CR_PAT, host_pat);
    save->g_pat = host_pat;

    vcpu->arch.regs.rip    = save->rip;
    vcpu->arch.regs.rsp    = save->rsp;
    vcpu->arch.regs.rflags = save->rflags;
    vcpu->arch.cr0 = save->cr0;
    vcpu->arch.cr3 = save->cr3;
    vcpu->arch.cr4 = save->cr4;
    vcpu->arch.efer = save->efer;

    pr_info("RELM: VCPU%d: guest state = 16-bit real mode (SVM), "
            "CS:IP = 0xF000:0xFFF0 -> linear 0xFFFFFFF0\n", vcpu->vpid);

    return 0;
}

/*
 * svm_init_guest_state_protected() — 32-bit flat protected mode, no
 * paging (CR0.PG=0)
 */
static int svm_init_guest_state_protected(struct vcpu *vcpu)
{
    struct vmcb_save_area *save;
    uint64_t host_pat = 0;

    if (!vcpu || !vcpu->arch.vmcb)
        return -EINVAL;

    save = &vcpu->arch.vmcb->save;

    save->cs = (struct vmcb_seg){
        .selector = 0x08, .base = 0, .limit = 0xFFFFFFFFU,
        .attrib = 0x0C9B,  /* type=B,S=1,DPL=0,P=1 | G=1,D/B=1,L=0 */
    };
    save->ds = save->es = save->ss = save->fs = save->gs = (struct vmcb_seg){
        .selector = 0x10, .base = 0, .limit = 0xFFFFFFFFU,
        .attrib = 0x0C93,  /* type=3,S=1,DPL=0,P=1 | G=1,D/B=1,L=0 */
    };

    save->ldtr = (struct vmcb_seg){ 0 };   /* P=0: unused, matches VMX  */
    save->tr.selector = 0;
    save->tr.base = 0;
    save->tr.limit = 0x67;
    save->tr.attrib = 0x08B;             /* 32-bit TSS, busy, P=1     */

    save->gdtr.base = vcpu->arch.gdtr_base;
    save->gdtr.limit = vcpu->arch.gdtr_limit;
    save->idtr.base = vcpu->arch.idtr_base;
    save->idtr.limit = vcpu->arch.idtr_limit;

    /* CR0/CR4/CR3: caller-supplied — see function comment. NE forced on
     * regardless (x87 error reporting via #MF, not the legacy #FERR/IRQ13
     * path) since nothing upstream should be relying on the legacy path. */
    save->cr0 = vcpu->arch.cr0 | X86_CR0_PE | X86_CR0_NE;
    save->cr4 = vcpu->arch.cr4;
    save->cr3 = vcpu->arch.cr3;

    save->efer = EFER_SVME;   /* no LME/LMA — not in long mode */

    save->rip  = vcpu->arch.regs.rip;
    save->rsp  = vcpu->arch.regs.rsp;
    save->rflags = 0x2;
    save->cpl = 0;

    rdmsrl(MSR_IA32_CR_PAT, host_pat);
    save->g_pat = host_pat;

    vcpu->arch.cr0  = save->cr0;
    vcpu->arch.efer = save->efer;
    vcpu->arch.regs.rflags = save->rflags;

    pr_info("RELM: VCPU%d: guest state = 32-bit flat protected mode (SVM), "
            "no paging, RIP=0x%llx\n", vcpu->vpid, save->rip);

    return 0;
}

/*
 * svm_init_guest_state_longmode() — direct 64-bit kernel entry (Linux
 * x86 boot protocol startup_64), the SVM counterpart of VMX's
 * relm_setup_guest_state_longmode() and RELM's actually-active boot
 * path on both backends. RSI = boot_params_gpa is the one register the
 * Linux 64-bit entry convention requires beyond RIP/RSP/segments —
 * carried here in vcpu->arch.regs.rsi, spilled/reloaded by the SVM
 * entry asm around VMRUN exactly like every other non-RAX/RSP/RIP GPR
 * (vcpu_arch.h point 4), NOT a VMCB save-area field the way RIP/RSP are.
 *
 * No CR0/CR4 FIXED0/FIXED1 capability-MSR sanitization pass, unlike
 * VMX: SVM has no such mandatory-bit MSR family for CR0/CR4 (svm.h's
 * "Gone, and why" already covers the parallel case for VM_CR/EFER) — a
 * plain literal is architecturally complete here.
 */
static int svm_init_guest_state_longmode(struct vcpu *vcpu)
{
    struct vmcb_save_area *save;
    uint64_t host_pat = 0;
    uint64_t entry_rip, entry_rsi, entry_rsp;

    if (!vcpu || !vcpu->vm)
        return -EINVAL;

    if (!vcpu->vm->arch.pml4_gpa) {
        pr_err("RELM: VCPU%d: longmode: vm->arch.pml4_gpa not set "
               "(relm_npt_create_guest_page_tables not run?)\n", vcpu->vpid);
        return -EINVAL;
    }

    entry_rip = vcpu->vm->arch.kernel_entry_gpa;
    if (!entry_rip) {
        pr_err("RELM: VCPU%d: longmode: vm->arch.kernel_entry_gpa not set "
               "(linux_loader_setup not run?)\n", vcpu->vpid);
        return -EINVAL;
    }

    entry_rsi = vcpu->vm->arch.boot_params_gpa;
    if (!entry_rsi) {
        pr_err("RELM: VCPU%d: longmode: vm->arch.boot_params_gpa not set\n",
               vcpu->vpid);
        return -EINVAL;
    }

    save = &vcpu->arch.vmcb->save;

    save->cs = (struct vmcb_seg){
        .selector = 0x08, .base = 0, .limit = 0xFFFFFFFFU,
        .attrib = 0x0A9B,  /* type=B,S=1,DPL=0,P=1 | G=1,D/B=0,L=1
                              * function's 0x0C9B (L=0,D/B=1) above     */
    };
    save->ds = save->es = save->ss = save->fs = save->gs = (struct vmcb_seg){
        .selector = 0x10, .base = 0, .limit = 0xFFFFFFFFU,
        .attrib = 0x0C93,
    };

    save->ldtr = (struct vmcb_seg){ 0 };
    save->tr.selector = 0;
    save->tr.base = 0;
    save->tr.limit = 0x67;
    save->tr.attrib = 0x08B;
    
    save->gdtr.base = RELM_GUEST_GDT_GPA;
    save->gdtr.limit = RELM_GUEST_GDT_SIZE - 1U;
    save->idtr.base = RELM_GUEST_IDT_GPA;
    save->idtr.limit = RELM_GUEST_IDT_SIZE - 1U;

    save->cr3 = vcpu->vm->arch.pml4_gpa;

    save->cr0 = X86_CR0_PG | X86_CR0_PE | X86_CR0_NE | X86_CR0_WP;
    save->cr4 = X86_CR4_PAE;

    save->efer = EFER_LME | EFER_LMA | EFER_SCE | EFER_SVME;
    save->rip = entry_rip;

    /* Stack: top of guest RAM, below the 3 reserved pages
     * relm_npt_create_guest_page_tables() carved out for the guest's own
     * page tables (same 3-page reservation npt.c documents), 16-byte
     * aligned per the x86-64 SysV stack-alignment ABI the kernel entry
     * expects. */
    entry_rsp = (vcpu->vm->memory.total_guest_ram - (3 * PAGE_SIZE)) & ~0xFULL;
    save->rsp = entry_rsp;

    save->rflags = 0x2;
    save->cpl = 0;

    rdmsrl(MSR_IA32_CR_PAT, host_pat);
    save->g_pat = host_pat;

    vcpu->arch.cr0 = save->cr0;
    vcpu->arch.cr3 = save->cr3;
    vcpu->arch.cr4 = save->cr4;
    vcpu->arch.efer = save->efer;

    vcpu->arch.regs.rip = entry_rip;
    vcpu->arch.regs.rsi = entry_rsi;
    vcpu->arch.regs.rsp = entry_rsp;
    vcpu->arch.regs.rflags = save->rflags;

    pr_info("RELM: VCPU%d: guest state = 64-bit long mode (SVM), "
            "RIP=0x%llx RSI(boot_params)=0x%llx RSP=0x%llx nCR3(guest)=0x%llx\n",
            vcpu->vpid, entry_rip, entry_rsi, entry_rsp, save->cr3);

    return 0;
}

int relm_init_vmcb_state(struct vcpu *vcpu)
{
    return svm_init_guest_state_longmode(vcpu);
}
void relm_free_vcpu(struct vcpu *vcpu)
{
    if (!vcpu)
        return;

    if (vcpu->arch.iopm) {
        free_pages((unsigned long)vcpu->arch.iopm, get_order(3 * PAGE_SIZE));
        vcpu->arch.iopm = NULL;
        vcpu->arch.iopm_pa = 0;
    }

    if (vcpu->arch.msrpm) {
        free_pages((unsigned long)vcpu->arch.msrpm, get_order(2 * PAGE_SIZE));
        vcpu->arch.msrpm = NULL;
        vcpu->arch.msrpm_pa = 0;
    }

    if (vcpu->arch.host_vmcb) {
        free_page((unsigned long)vcpu->arch.host_vmcb);
        vcpu->arch.host_vmcb = NULL;
        vcpu->arch.host_vmcb_pa = 0;
    }

    if (vcpu->arch.hsave) {
        free_page((unsigned long)vcpu->arch.hsave);
        vcpu->arch.hsave = NULL;
        vcpu->arch.hsave_pa = 0;
    }

    if (vcpu->arch.vmcb) {
        free_page((unsigned long)vcpu->arch.vmcb);
        vcpu->arch.vmcb = NULL;
        vcpu->arch.vmcb_pa = 0;
    }

    PDEBUG("RELM: VCPU%d: SVM allocations freed\n", vcpu->vpid);
}

void relm_dump_vcpu(struct vcpu *vcpu)
{
    struct vmcb_control_area *ctrl;
    struct vmcb_save_area *save;

    if (!vcpu || !vcpu->arch.vmcb) {
        pr_info("RELM: VCPU?: relm_dump_vcpu: no VMCB\n");
        return;
    }

    ctrl = &vcpu->arch.vmcb->control;
    save = &vcpu->arch.vmcb->save;

    pr_info("\n*** SVM VCPU%d Control Area ***\n\n", vcpu->vpid);

    pr_info("intercepts: cr=0x%08x dr=0x%08x exceptions=0x%08x\n",
            ctrl->intercept_cr, ctrl->intercept_dr, ctrl->intercept_exceptions);
    pr_info("intercepts: word1=0x%08x word2=0x%08x word3=0x%08x\n",
            ctrl->intercept_1, ctrl->intercept_2, ctrl->intercept_3);

    pr_info("asid=%u tlb_ctl=0x%02x\n", ctrl->asid, ctrl->tlb_ctl);

    pr_info("nested_ctl=0x%llx nested_cr3=0x%llx\n",
            ctrl->nested_ctl, ctrl->nested_cr3);

    pr_info("iopm_base_pa=0x%llx msrpm_base_pa=0x%llx\n",
            ctrl->iopm_base_pa, ctrl->msrpm_base_pa);

    pr_info("exit_code=0x%llx exit_info_1=0x%llx exit_info_2=0x%llx\n",
            ctrl->exit_code, ctrl->exit_info_1, ctrl->exit_info_2);

    pr_info("event_inj=0x%08x event_inj_err=0x%08x\n",
            ctrl->event_inj, ctrl->event_inj_err);

    pr_info("\n*** SVM VCPU%d Save Area ***\n\n", vcpu->vpid);

    pr_info("CS: sel=0x%04x base=0x%llx limit=0x%08x attrib=0x%04x\n",
            save->cs.selector, save->cs.base, save->cs.limit, save->cs.attrib);
    pr_info("SS: sel=0x%04x base=0x%llx limit=0x%08x attrib=0x%04x\n",
            save->ss.selector, save->ss.base, save->ss.limit, save->ss.attrib);
    pr_info("DS: sel=0x%04x base=0x%llx limit=0x%08x attrib=0x%04x\n",
            save->ds.selector, save->ds.base, save->ds.limit, save->ds.attrib);

    pr_info("GDTR base=0x%llx limit=0x%08x  IDTR base=0x%llx limit=0x%08x\n",
            save->gdtr.base, save->gdtr.limit,
            save->idtr.base, save->idtr.limit);

    pr_info("CR0=0x%llx CR2=0x%llx CR3=0x%llx CR4=0x%llx CPL=%u\n",
            save->cr0, save->cr2, save->cr3, save->cr4, save->cpl);

    pr_info("EFER=0x%llx G_PAT=0x%llx\n", save->efer, save->g_pat);

    pr_info("RIP=0x%llx (regs.rip=0x%lx)\n", 
            save->rip, vcpu->arch.regs.rip);
    pr_info("RSP=0x%llx (regs.rsp=0x%lx)\n", 
            save->rsp, vcpu->arch.regs.rsp);
    pr_info("RAX=0x%llx (regs.rax=0x%lx)\n",
            save->rax, vcpu->arch.regs.rax);
    pr_info("RFLAGS=0x%llx DR6=0x%llx DR7=0x%llx\n",
            save->rflags, save->dr6, save->dr7);
}
