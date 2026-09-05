#include <linux/printk.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <relm/vcpu.h>
#include <relm/vm.h>
#include <vmx.h>
#include <ept.h>
#include <vmx_ops.h>
#include <vmexit.h>
#include <vmcs_state.h>
#include <include/virtio/mmio.h>
#include <include/firmware/fw_cfg.h>
#include <include/firmware/seabios.h> 
#include <include/boot/arch/x86/loader.h>
#include <include/debug/insn_dump.h>

#include <utils/utils.h>

#define CREATE_TRACE_POINTS 
#include <include/trace/events/relm.h> 

static void relm_dump_fault_regs(struct vcpu *vcpu, uint64_t guest_rsp)
{
    pr_err("relm: [VPID=%u] RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx\n",
           vcpu->vpid, vcpu->arch.regs.rax, vcpu->arch.regs.rbx,
           vcpu->arch.regs.rcx, vcpu->arch.regs.rdx);
    pr_err("relm: [VPID=%u] RSI=0x%llx RDI=0x%llx RBP=0x%llx RSP=0x%llx\n",
           vcpu->vpid, vcpu->arch.regs.rsi, vcpu->arch.regs.rdi,
           vcpu->arch.regs.rbp, guest_rsp);
    pr_err("relm: [VPID=%u] R8=0x%llx R9=0x%llx R10=0x%llx R11=0x%llx\n",
           vcpu->vpid, vcpu->arch.regs.r8, vcpu->arch.regs.r9,
           vcpu->arch.regs.r10, vcpu->arch.regs.r11);
    pr_err("relm: [VPID=%u] R12=0x%llx R13=0x%llx R14=0x%llx R15=0x%llx\n",
           vcpu->vpid, vcpu->arch.regs.r12, vcpu->arch.regs.r13,
           vcpu->arch.regs.r14, vcpu->arch.regs.r15);
    pr_err("relm: [VPID=%u] CR0=0x%llx CR3=0x%llx CR4=0x%llx EFER=0x%llx RFLAGS=0x%llx\n",
           vcpu->vpid,
           (unsigned long long)__vmread(GUEST_CR0),
           (unsigned long long)__vmread(GUEST_CR3),
           (unsigned long long)__vmread(GUEST_CR4),
           (unsigned long long)__vmread(GUEST_IA32_EFER),
           (unsigned long long)__vmread(GUEST_RFLAGS));
}

/*
 * relm_vmentry_save_rsp() — called from relm_vmentry_asm (vmx_asm.S) just
 * before VMLAUNCH/VMRESUME, with the kthread RSP *after* the callee-saved
 * pushes. Stores it in vcpu->arch.vmentry_host_rsp so the VM-exit stub's
 * stop path can find the frame again: .Lvmexit_return_zero reloads this
 * value into RSP and finishes relm_vmentry_asm's epilogue on its behalf.
 * Loss of this value = no way back to the vCPU kthread (the stub parks
 * the CPU in .Lvmexit_fatal).
 */
void relm_vmentry_save_rsp(uint64_t rsp)
{
    struct vcpu *vcpu = relm_get_current_vcpu(); 

    if(unlikely(!vcpu))
    {
        pr_err("RELM: relm_vmentry_save_rsp: no current VCPU on CPU%d\n",
               smp_processor_id());
        return; 
    }

    vcpu->arch.vmentry_host_rsp = rsp; 
    PDEBUG("RELM: VCPU%d: kthread RSP 0x%llx saved before VM-entry on CPU%d\n",
           vcpu->vpid, rsp, smp_processor_id());
}
/*
 * emulate_cpuid() — handle a guest CPUID (unconditionally exiting
 * instruction). Passthrough model: execute the real CPUID on the host
 * with the guest's leaf (RAX) and subleaf (RCX) and hand the host's
 * answer straight back. No filtering yet — the guest sees host CPU
 * features verbatim, including ones we do not virtualise (TODO: mask
 * feature bits such as VMX itself, x2APIC, etc. as emulation grows).
 */
static void emulate_cpuid(struct vcpu *vcpu)
{
    uint32_t leaf, subleaf;
    uint32_t eax, ebx, ecx, edx;

    /* guest inputs */
    leaf    = (uint32_t)vcpu->arch.regs.rax;
    subleaf = (uint32_t)vcpu->arch.regs.rcx;

    asm volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf)
        : "memory"
    );

    /* log BEFORE state mutation (avoids confusion during tracing) */
    PDEBUG("cpuid leaf=0x%x subleaf=0x%x -> eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
           leaf, subleaf, eax, ebx, ecx, edx);

    /* vcpu->arch.regs is the single place handlers write guest registers;
     * handle_vmexit copies it back into the on-stack GPR save block that
     * the VM-exit stub pops before VMRESUME. */
    vcpu->arch.regs.rax = eax;
    vcpu->arch.regs.rbx = ebx;
    vcpu->arch.regs.rcx = ecx;
    vcpu->arch.regs.rdx = edx;
}
/*
 * relm_vmentry_get_rsp() — counterpart of relm_vmentry_save_rsp(): called
 * from the VM-exit stub's stop path (.Lvmexit_return_zero in vmx_asm.S)
 * to retrieve the kthread RSP saved before VM-entry. Returns 0 when there
 * is no current vCPU or nothing was saved; the asm treats 0 as "no stack
 * to unwind to" and halts the CPU instead of jumping to a garbage RSP.
 */
uint64_t relm_vmentry_get_rsp(void)
{
    struct vcpu *vcpu = relm_get_current_vcpu(); 

    if(unlikely(!vcpu))
    {
        pr_err("RELM: relm_vmentry_get_rsp: no current VCPU on CPU%d\n", 
               smp_processor_id());
        return 0; 
    }

    if(unlikely(vcpu->arch.vmentry_host_rsp == 0))
    {
        pr_err("RELM: VCPU%d: vmentry_host_rsp is 0\n",
               vcpu->vpid); 
        return 0; 
    }

    PDEBUG("RELM: VCPU%d: returning kthread RSP 0x%llx to vmexit_handler on CPU%d\n",
           vcpu->vpid, vcpu->arch.vmentry_host_rsp, smp_processor_id());
 
    return vcpu->arch.vmentry_host_rsp;
}

/*
 * handle_vmexit() — the C half of VM-exit handling, called by
 * relm_vmexit_handler (vmx_asm.S) with a pointer to the guest GPRs the
 * stub just pushed onto the kthread stack. Everything the hardware knows
 * about the exit is read from the VMCS here (exit reason, qualification,
 * guest RIP/RSP, instruction length).
 *
 * Register contract (the reason this function is bracketed by two big
 * copy blocks):
 *   entry:  guest_gprs -> vcpu->arch.regs, so every handler sees guest
 *           state in one uniform place and generic code never touches the
 *           raw stack block;
 *   exit:   vcpu->arch.regs -> guest_gprs, because the stack block — not
 *           arch.regs — is what the stub pops into the CPU before
 *           VMRESUME. A handler's write that skipped this sync would be
 *           silently lost.
 * RSP/RIP flow through the VMCS instead: handlers that advance the guest
 * (past HLT, CPUID, an emulated MMIO access...) _vmwrite GUEST_RIP.
 *
 * Return value drives the asm stub:
 *   1 -> pop guest GPRs and VMRESUME straight back into the guest;
 *   0 -> unwind to relm_vcpu_loop. NOT necessarily fatal: HLT uses 0 so
 *        the loop can sleep until an interrupt is injected; fatal paths
 *        additionally set vcpu->state to STOPPED/ERROR before returning 0.
 */
int handle_vmexit(struct stack_guest_gprs *guest_gprs)
{
    struct vcpu *vcpu;
    uint64_t exit_reason;
    uint64_t exit_qualification;
    uint64_t guest_rip;
    uint64_t guest_rsp;
    uint64_t instr_len;
    u64 start_time, end_time, duration; 

    int ret; 

    start_time = ktime_get_ns(); 
    

    vcpu = relm_get_current_vcpu();
    if(!vcpu)
    {
        pr_err("relm: handle_vmexit called but no current VCPU!\n");
        return 0;
    }

    exit_reason = __vmread(VM_EXIT_REASON);

    /* Check the VM-entry-failure flag: bit 31 of the exit-reason field.
     * The old test `exit_reason & (1U << 32)` shifted a 32-bit int by 32
     * (undefined behavior) and never examined bit 31, so entry failures went
     * undetected. Must be checked on the full field before masking to 16 bits. */
    if(exit_reason & (1ULL << 31))
    {
        pr_err("relm: [VPID=%u] VM-entry failure in exit handler (reason=0x%llx)\n",
               vcpu->vpid, exit_reason & 0xFFFF);
        return 0;
    }
    
    /* Bits 15:0 carry the basic exit reason; the rest are flag bits. */
    exit_reason = exit_reason & 0xFFFF;

    exit_qualification = __vmread(VM_EXIT_QUALIFICATION);
    guest_rip = __vmread(GUEST_RIP);
    guest_rsp = __vmread(GUEST_RSP);

    vcpu->stats.total_exits++;

    /* A VM-exit proves the VMLAUNCH succeeded: from now on the VMCS is in
     * "launched" state and re-entry must use VMRESUME (relm_vmentry_asm
     * picks the instruction from this flag). */
    if(!vcpu->launched)
    {
        vcpu->launched = 1;
        pr_info("relm: [VPID=%u] First VM-exit (exit #%llu), now using VMRESUME\n",
                vcpu->vpid, vcpu->stats.total_exits);
    }

    /* Import the guest register state: on-stack block (pushed by the asm
     * stub) -> vcpu->arch.regs, the uniform model all handlers read and
     * write. Mirrored back at the bottom of this function. RSP/RIP come
     * from the VMCS, not the stack — hardware saves those itself. */
    vcpu->arch.regs.rax = guest_gprs->rax;
    vcpu->arch.regs.rbx = guest_gprs->rbx;
    vcpu->arch.regs.rcx = guest_gprs->rcx;
    vcpu->arch.regs.rdx = guest_gprs->rdx;
    vcpu->arch.regs.rsi = guest_gprs->rsi;
    vcpu->arch.regs.rdi = guest_gprs->rdi;
    vcpu->arch.regs.rbp = guest_gprs->rbp;
    vcpu->arch.regs.r8  = guest_gprs->r8;
    vcpu->arch.regs.r9  = guest_gprs->r9;
    vcpu->arch.regs.r10 = guest_gprs->r10;
    vcpu->arch.regs.r11 = guest_gprs->r11;
    vcpu->arch.regs.r12 = guest_gprs->r12;
    vcpu->arch.regs.r13 = guest_gprs->r13;
    vcpu->arch.regs.r14 = guest_gprs->r14;
    vcpu->arch.regs.r15 = guest_gprs->r15;

    vcpu->arch.regs.rsp = guest_rsp;
    vcpu->arch.regs.rip = guest_rip;

    PDEBUG("relm: [VPID=%u] Exit #%llu: reason=%llu RIP=0x%llx\n",
           vcpu->vpid, vcpu->stats.total_exits, exit_reason, guest_rip);

    RELM_DUMP_GUEST_INSN(vcpu, guest_rip);

    switch(exit_reason)
    {
        case EXIT_REASON_EXCEPTION_NMI:
        {
            /* A guest exception whose vector is set in our exception
             * bitmap (or an NMI). Interruption info encodes what fired:
             * bits 7:0 = vector, bits 10:8 = type (3 = hardware
             * exception, 2 = NMI, ...). */
            uint32_t intr_info = __vmread(VM_EXIT_INTR_INFO);
            uint32_t vector = intr_info & 0xFF;
            uint32_t intr_type = (intr_info >> 8) & 0x7;

            pr_err("relm: [VPID=%u] Guest exception: vector=%u type=%u at RIP=0x%llx\n",
                   vcpu->vpid, vector, intr_type, guest_rip);

            /*treat all exceptions as fatal */
            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0;
            break; 
        }

        case EXIT_REASON_EXTERNAL_INTERRUPT:

            /* external interrupt arrived while guest was running
            * just re-enter the guest */
            PDEBUG("relm: [VPID=%u] External interrupt\n", vcpu->vpid);
            ret = 1;
            break; 

        case EXIT_REASON_TRIPLE_FAULT:

            /* Guest took a fault while delivering a double fault — on
             * bare metal the machine would reset. Unrecoverable; stop. */
            pr_err("relm: [VPID=%u] Guest triple fault at RIP=0x%llx\n",
                   vcpu->vpid, guest_rip);
            relm_dump_fault_regs(vcpu, guest_rsp);
            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0;
            break; 

        case EXIT_REASON_APIC_ACCESS:
            /* Guest touched the APIC page at 0xFEE00000. Full fetch/
             * decode/emulate flow lives in vmx/apic.c; it also advances
             * the guest RIP itself. */
            ret = relm_apic_handle_access(vcpu);
            break;

        case EXIT_REASON_INIT_SIGNAL:

            /* INIT arriving at a running vCPU. Proper handling would put
             * it in wait-for-SIPI (the SMP bring-up dance in apic.c's IPI
             * TODOs); until then treat as a stop request. */
            pr_info("relm: [VPID=%u] INIT signal received\n", vcpu->vpid);
            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0; 
            break; 

        case EXIT_REASON_HLT:

            PDEBUG("relm: [VPID=%u] Guest executed HLT at RIP=0x%llx\n",
                    vcpu->vpid, guest_rip);

            /*
             * HLT is the guest idle path: it expects to resume at the next
             * instruction once an interrupt is delivered. We advance RIP past
             * the HLT and mark the vCPU halted, but KEEP state == RUNNING and
             * return 0 so control unwinds to relm_vcpu_loop, which then sleeps
             * on the wait queue until an interrupt is injected (or the vCPU is
             * stopped) and re-enters the guest.
             *
             * The old code set state = VCPU_STATE_HALTED, which the generic loop
             * treats as "state != RUNNING" and breaks out of — permanently
             * stopping the vCPU on the very first HLT and halting boot progress.
             */
            vcpu->halted = true;

            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);

            ret = 0;   /* return to relm_vcpu_loop, which sleeps until woken */
            break;

        case EXIT_REASON_CPUID:
        {
            /* Fill RAX/RBX/RCX/RDX in vcpu->arch.regs (synced to the
             * stack block at the bottom), then step past the 2-byte
             * CPUID using the hardware-reported instruction length. */
            emulate_cpuid(vcpu);
            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);
            ret = 1;
            break;
        }


        case EXIT_REASON_IO_INSTRUCTION:
        {
            /* IN/OUT trapped via the I/O bitmaps. The exit qualification
             * (SDM Vol 3C table 27-5) tells us everything — no decoding
             * needed: bits 2:0 = access size minus one, bit 3 =
             * direction (1 = IN), bit 4 = string op (INS/OUTS), bit 5 =
             * REP prefix, bits 31:16 = port number. */
            uint32_t size = (uint32_t)(exit_qualification & 0x7ULL) + 1;
            bool is_in = (exit_qualification & (1ULL << 3)) != 0;
            bool is_str = (exit_qualification & (1ULL << 4)) != 0;

            /*REP prefix : repets ECX times */
            bool is_rep = (exit_qualification & (1ULL << 5)) != 0;


            uint16_t port = (uint16_t)((exit_qualification >> 16) & 0xFFFFULL); 
            uint32_t io_val = 0; 

            PDEBUG("RELM: [VPID=%u] IO_EXIT: %s%s%s port=0x%03x size=%u "
                   "RIP=0x%llx",
                   vcpu->vpid,
                   is_in  ? "IN"     : "OUT",
                   is_str ? " STRING" : "",
                   is_rep ? " REP"    : "",
                   port, size, guest_rip);

            /* Only the QEMU fw_cfg ports (0x510 selector / 0x511 data,
             * used by SeaBIOS to fetch boot configuration) are emulated,
             * and only their simple non-string non-REP forms — which is
             * all SeaBIOS uses. */
            if((port == FW_CFG_PORT_SEL || port == FW_CFG_PORT_DATA)
                && !is_str
                && !is_rep)
            {
                struct relm_vm *vm = vcpu->vm; 

                /*for OUT (guest writing to fw_cfg)*/ 
                if(!is_in)
                {
                    uint32_t size_mask = (size == 1) ? 0xFFU 
                        : (size == 2) ? 0xFFFFU 
                        : 0xFFFFFFFFU; 
                        
                    io_val = (uint32_t)(vcpu->arch.regs.rax & size_mask); 
                    PDEBUG("RELM: fw_cfg OUT port=0x%03x val=0x%08x (size=%u)",
                           port, io_val, size);
                }

                ret = relm_fw_cfg_handle_io(&vm->fw_data->fw_cfg, port, !is_in, size, &io_val);

                /*guest is reading from fw_cfg*/
                if(is_in)
                {
                    /* Zero-extend io_val to 64 bits and deliver it to the
                     * guest RAX via vcpu->arch.regs (synced back to the
                     * on-stack GPR block by handle_vmexit). RAX is NOT a
                     * VMCS field: the old code wrote it to encoding 0x6818
                     * — GUEST_IDTR_BASE — corrupting the guest IDT base. */
                    unsigned long rax_val = (unsigned long)(io_val & 0xFFFFFFFFUL);
                    vcpu->arch.regs.rax = rax_val;

                    PDEBUG("RELM: fw_cfg IN  port=0x%03x → val=0x%08x "
                               "(size=%u) → RAX=0x%lx",
                               port, io_val, size, rax_val);
                }

                instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
                _vmwrite(GUEST_RIP, guest_rip + instr_len);
                break; 
            }

            /* Any other port: NOP the access. OUTs are swallowed; INs
             * leave RAX untouched. The guest keeps running — legacy
             * device probes (PIC, PIT, serial...) just see nothing. */
            PDEBUG("RELM: [VPID=%u] unhandled port 0x%03x %s size=%u — NOP",
                   vcpu->vpid, port, is_in ? "IN" : "OUT", size);

            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);
            ret = 1;
            break;
        }

        case EXIT_REASON_VMCALL:

            /* VMCALL always exits. Two users share it: the diagnostic
             * IDT stubs we plant in the guest (each exception vector's
             * stub does VMCALL with the vector in RAX, so early-boot
             * faults reach the host log even with no working guest
             * console), and — eventually — real hypercalls. */
            pr_info("relm: [VPID=%u] VMCALL hypercall at RIP=0x%llx\n",
                    vcpu->vpid, guest_rip);


            /*diagnostic IDT trampoline 
             * to distinguish a real hypercall from a diagnostic trap, 
             * we additionally check that guest_rip lands inside the 
             * stubs page and that rax is less that 256*/ 

            uint64_t rax = vcpu->arch.regs.rax; 
            uint64_t stubs_lo = RELM_GUEST_IDT_STUBS_GPA; 
            uint64_t stubs_hi = stubs_lo + RELM_GUEST_IDT_STUBS_SIZE; 

            bool from_diag_stub = (guest_rip >= stubs_lo) && 
                (guest_rip < stubs_hi) && 
                (rax < 256ULL); 
            
            if(from_diag_stub)
            {
                uint64_t cr2 = _read_cr2(); 
                uint64_t exit_qual = exit_qualification; 

                uint64_t stub_base = guest_rip - 8ULL; 
                uint64_t expected_stub = stubs_lo + rax *
                    RELM_GUEST_IDT_STUBS_STRIDE; 

                const char *vec_name = "unknown"; 
                switch(rax)
                {
                    case 0:  vec_name = "#DE (divide error)";          break;
                    case 1:  vec_name = "#DB (debug)";                 break;
                    case 2:  vec_name = "NMI";                         break;
                    case 3:  vec_name = "#BP (breakpoint)";            break;
                    case 4:  vec_name = "#OF (overflow)";              break;
                    case 5:  vec_name = "#BR (bound range)";           break;
                    case 6:  vec_name = "#UD (invalid opcode)";        break;
                    case 7:  vec_name = "#NM (no FPU)";                break;
                    case 8:  vec_name = "#DF (double fault)";          break;
                    case 10: vec_name = "#TS (invalid TSS)";           break;
                    case 11: vec_name = "#NP (segment not present)";   break;
                    case 12: vec_name = "#SS (stack-segment fault)";   break;
                    case 13: vec_name = "#GP (general protection)";    break;
                    case 14: vec_name = "#PF (page fault)";            break;
                    case 16: vec_name = "#MF (x87 FPE)";               break;
                    case 17: vec_name = "#AC (alignment check)";       break;
                    case 18: vec_name = "#MC (machine check)";         break;
                    case 19: vec_name = "#XM (SIMD FPE)";              break;
                    case 21: vec_name = "#CP (control protection)";    break;
                    default: break;
                }

                pr_err("RELM: [VPID=%u] *** DIAG-IDT TRAP vector=%llu (%s)\n",
                       vcpu->vpid, rax, vec_name);
                pr_err("RELM:        RIP-at-vmcall=0x%llx  (stub_base=0x%llx, "
                       "expected=0x%llx%s)\n",
                       guest_rip - 3ULL, stub_base, expected_stub,
                       (stub_base == expected_stub) ? "" :
                                                      " — MISMATCH!");
                pr_err("RELM:        CR2=0x%llx  (meaningful only for #PF)\n",
                       cr2);
                pr_err("RELM:        EXIT_QUAL=0x%llx  RSP=0x%llx  RFLAGS=0x%llx\n",
                       exit_qual,
                       (uint64_t)__vmread(GUEST_RSP),
                       (uint64_t)__vmread(GUEST_RFLAGS)); 

                /*stop guest */ 
               ret = 0; 
                break; 
            }
            /*TODO : 
             * Non-diagnostic VMCALL: real hypercall path. */
            pr_info("relm: [VPID=%u] VMCALL hypercall at RIP=0x%llx RAX=0x%llx\n",
                    vcpu->vpid, guest_rip, rax);

            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);
            ret = 1;
            break;

        case EXIT_REASON_MSR_READ:
        {
            /* RDMSR trapped by the MSR bitmap. ECX holds the MSR index;
             * the result is returned split across EDX:EAX. */
            uint32_t msr = vcpu->arch.regs.rcx & 0xFFFFFFFF;
            pr_info("relm: [VPID=%u] RDMSR 0x%x at RIP=0x%llx\n",
                    vcpu->vpid, msr, guest_rip);

            /*TODO: emulate MSR_READ
             * pass 0 for now*/
            vcpu->arch.regs.rax = 0;
            vcpu->arch.regs.rdx = 0;

            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);
            ret = 1;
            break; 

        }

        case EXIT_REASON_MSR_WRITE:
        {
            /* WRMSR: index in ECX, 64-bit value assembled from EDX:EAX.
             * Only IA32_EFER is genuinely emulated (it gates the switch
             * into 64-bit long mode); everything else is logged and
             * dropped. */
            uint32_t msr = vcpu->arch.regs.rcx & 0xFFFFFFFFULL;
            uint64_t val = ((uint64_t)vcpu->arch.regs.rdx << 32) |
                    (uint64_t)(vcpu->arch.regs.rax & 0xFFFFFFFF);

            PDEBUG("relm: [VPID=%u] WRMSR 0x%x = 0x%llx at RIP=0x%llx\n",
                    vcpu->vpid, msr, val, guest_rip);


            /*handle IA32_EFER MSR index 0xC0000080*/ 
            if(msr == MSR_IA32_EFER)
            {
                /*sanitize bit mask. clear reserved bits and 
                 * only allow gust writes for valid bits 
                 * SCE, LME, LMA, NXE*/ 
                const uint64_t EFER_VALID_MASK = (1ULL << 0)  |  /* SCE */
                                                  (1ULL << 8)  |  /* LME */
                                                  (1ULL << 10) |  /* LMA */
                                                  (1ULL << 11);   /* NXE */
                val &= EFER_VALID_MASK;

                /*guest must not directly set the LMA bit, it 
                 * is set by the CPU whem LME+PG is activated. 
                 * so we clear it*/ 
                val &= ~(1ULL << 10);

                /*LMA = LME AND CR0.PG*/
                uint64_t guest_cr0 = __vmread(GUEST_CR0);
                bool lme = (val & (1ULL << 8)) != 0;
                bool pg = (guest_cr0 & (1ULL << 31)) != 0;   /* CR0.PG is bit 31, not 32 */
                bool lma = lme && pg;                        /* was `lma && pg` — used itself uninitialized */

                if(lma)
                    val |= (1ULL << 10);                     /* was `val != (1ULL<<10)` — a discarded comparison; set LMA */

                _vmwrite(GUEST_IA32_EFER, val); 
                vcpu->arch.efer = val;


                PDEBUG("RELM: [VPID=%u] WRMSR EFER: LME=%u PG=%u → "
                        "LMA=%u EFER=0x%llx",
                        vcpu->vpid, lme ? 1:0, pg ? 1:0, lma ? 1:0, val);

                /*sync IA32_MODE_GUEST in vm-entry controls 
                 * we update it now, so the very next VMRESUME is in 64 bit long mode.*/
                uint32_t entry_ctrl = (uint32_t)__vmread(VMCS_ENTRY_CONTROLS); 
                if(lma)
                {
                    /*long mode active: set IA32_MODE_GUEST*/  
                    entry_ctrl |= VM_ENTRY_IA32E_MODE;
                    PDEBUG("RELM: [VPID=%u] IA32E_MODE_GUEST → 1 "
                            "(guest entered 64-bit long mode)", vcpu->vpid);
                }else{
                    /*long mode not active : clear IA32E_MODE_GUEST*/ 
                    entry_ctrl &= ~(uint32_t)VM_ENTRY_IA32E_MODE; 
                }
                _vmwrite(VMCS_ENTRY_CONTROLS, entry_ctrl); 
            }
            else{

                /*TODO
                 * Emulate Other MSRs writes 
                 *  add cases for:
                 *  MSR_STAR / MSR_LSTAR / MSR_CSTAR: SYSCALL targets
                 *  MSR_FS_BASE / MSR_GS_BASE: segment bases
                 *  MSR_IA32_APIC_BASE: APIC relocation */

            PDEBUG("RELM: [VPID=%u] WRMSR MSR=0x%08x ignored "
                       "(not emulated)", vcpu->vpid, msr);
            }

            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);

            ret = 1;
            break; 
        }


        case EXIT_REASON_EPT_VIOLATION:
        {
            /* Guest access to a GPA the EPT tables reject. Qualification
             * bits (SDM Vol 3C table 27-7): 2:0 = what the ACCESS was
             * (read/write/fetch), 5:3 = what the EPT ENTRY allowed.
             * Entry bits all clear = the GPA is not mapped at all — which
             * is either our deliberate MMIO trap or a real guest bug. */
            uint64_t gpa = __vmread(GUEST_PHYSICAL_ADDRESS);
            bool data_read = exit_qualification & (1ULL << 0);
            bool data_write = exit_qualification & (1ULL << 1);
            bool instr_fetch = exit_qualification & (1ULL << 2);
            bool ept_readable = exit_qualification & (1ULL << 3);
            bool ept_writable = exit_qualification & (1ULL << 4);
            bool ept_executable = exit_qualification & (1ULL << 5);

            bool was_present = ept_readable || ept_writable || ept_executable;

            /* Unmapped GPA inside a reserved MMIO window = virtio device
             * access (MMIO regions are trap-by-absence: reserved in the
             * registry, never mapped in EPT — see virtio/mmio.c). */
            if (!was_present)
            {
            /*if GPA is in the range the VM has reserved for MMIO */
                if (relm_vm_gpa_is_mmio_region(vcpu->vm, gpa)) {

                    if (relm_virtio_mmio_handle_ept_violation(vcpu, gpa)) {
                        /* MMIO access emulated (the handler advanced RIP past
                         * the faulting instruction). Resume the guest via
                         * VMRESUME. The old code returned 0 = stop, killing the
                         * guest on its first virtio MMIO access. */
                        ret = 1;
                        break;
                    }

                    /* Reserved range, but the access could not be emulated:
                     * either no registered device claimed the GPA (an
                     * initialization-ordering bug) or the handler failed to
                     * fetch/decode the faulting instruction (it logged the
                     * specific cause). Resuming would just re-fault at the
                     * same RIP forever, so stop the vCPU. */
                    pr_err("relm: [VPID=%u] unemulatable MMIO access at GPA "
                           "0x%llx in a reserved MMIO range\n",
                           vcpu->vpid, gpa);
                    vcpu->state = VCPU_STATE_ERROR;
                    ret = 0;
                    break;
                }
            }
            /* Not an MMIO window: the guest touched memory it has no
             * business touching (or our EPT setup is missing a mapping).
             * Dump access-vs-permissions and stop. */
            pr_err("relm: [VPID=%u] EPT violation at GPA 0x%llx\n",
                   vcpu->vpid, gpa);
            pr_err(" Access: %s%s%s at RIP=0x%llx\n",
                   data_read ? "R" : "",
                   data_write ? "W" : "",
                   instr_fetch ? "X" : "",
                   guest_rip);
            pr_err(" EPT entry: %s%s%s\n",
                   ept_readable ? "R" : "-",
                   ept_writable ? "W" : "-",
                  ept_executable ? "X" : "-");
            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0;
            break;
        }
        
        case EXIT_REASON_CR_ACCESS:
        {
            uint32_t cr_num    = (uint32_t)(exit_qualification & CR_ACCESS_CR_NUMBER_MASK);
            uint32_t acc_type  = (uint32_t)(exit_qualification & CR_ACCESS_TYPE_MASK);
 
            if(cr_num == CR_ACCESS_CR_NUMBER_CR3 &&
               (acc_type == CR_ACCESS_TYPE_WRITE || acc_type == CR_ACCESS_TYPE_READ))
            {
                ret = relm_cr3_passthrough_handle_exit(vcpu, exit_qualification, acc_type);
                break;
            }
            else if(cr_num == CR_ACCESS_CR_NUMBER_CR4 &&
                    acc_type == CR_ACCESS_TYPE_WRITE)
            {
                ret = relm_cr4_write_handle_exit(vcpu, exit_qualification);
                break;
            }
            else
            {
                /* CR0, CR8 write, or CR read not yet handled.
                 * Log and stop the guest. Implement as needed. */
                pr_err("relm: [VPID=%u] Unhandled CR access "
                       "cr=%u type=%u at RIP=0x%llx\n",
                       vcpu->vpid, cr_num, acc_type >> 4, guest_rip);
                vcpu->state = VCPU_STATE_STOPPED;
                ret = 0;
                break;
            }
        }
        case EXIT_REASON_INVALID_GUEST_STATE:

            /* VM-entry itself failed */ 
            pr_err("relm: [VPID=%u] Invalid guest state\n", vcpu->vpid);
            pr_err(" Guest RIP: 0x%llx\n", guest_rip);
            pr_err(" Guest RSP: 0x%llx\n", guest_rsp);

            relm_dump_vcpu(vcpu);

            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0;
            break; 

        case EXIT_REASON_VMX_PREEMPTION_TIMER_EXPIRED:
       
            pr_warn("relm: [VPID=%u] Unexpected VMX preemption timer expired "
                    "at RIP=0x%llx — timer was not configured\n",
                    vcpu->vpid, guest_rip);
            
            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0; 
            break; 
       
        case EXIT_REASON_EPT_MISCONFIG:
        {
            uint64_t gpa = __vmread(GUEST_PHYSICAL_ADDRESS);

            /* An EPT misconfiguration means an EPT paging-structure entry has
             * an illegal configuration (e.g. a reserved bit set, or a bad
             * memory type on a leaf). Previously this reason had no case and
             * fell through to the generic "unhandled" path, hiding the cause.
             * Dispatch to the dedicated diagnostic dumper, then stop. */
            pr_err("relm: [VPID=%u] EPT misconfiguration at GPA 0x%llx RIP=0x%llx\n",
                   vcpu->vpid, gpa, guest_rip);
            relm_vcpu_handle_ept_misconfig(vcpu->vm);
            vcpu->state = VCPU_STATE_ERROR;
            ret = 0;
            break;
        }

        default:

            pr_err("relm: [VPID=%u] Unhandled VM-exit reason %llu\n",
                   vcpu->vpid, exit_reason);
            pr_err(" Guest RIP: 0x%llx\n", guest_rip);
            pr_err(" Exit qualification: 0x%llx\n", exit_qualification);

            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0; 
            break;  
    }

    /* Copy emulated register state back into the on-stack save block: it is
     * what the VM-exit stub pops into the CPU before VMRESUME (see
     * vmx_asm.S). Handlers write only vcpu->arch.regs; without this sync any
     * emulated result (CPUID, IN, MMIO reads, ...) would be silently
     * discarded and the guest would resume with stale registers. */
    guest_gprs->rax = vcpu->arch.regs.rax;
    guest_gprs->rbx = vcpu->arch.regs.rbx;
    guest_gprs->rcx = vcpu->arch.regs.rcx;
    guest_gprs->rdx = vcpu->arch.regs.rdx;
    guest_gprs->rsi = vcpu->arch.regs.rsi;
    guest_gprs->rdi = vcpu->arch.regs.rdi;
    guest_gprs->rbp = vcpu->arch.regs.rbp;
    guest_gprs->r8  = vcpu->arch.regs.r8;
    guest_gprs->r9  = vcpu->arch.regs.r9;
    guest_gprs->r10 = vcpu->arch.regs.r10;
    guest_gprs->r11 = vcpu->arch.regs.r11;
    guest_gprs->r12 = vcpu->arch.regs.r12;
    guest_gprs->r13 = vcpu->arch.regs.r13;
    guest_gprs->r14 = vcpu->arch.regs.r14;
    guest_gprs->r15 = vcpu->arch.regs.r15;

    end_time = ktime_get_ns();
    duration = end_time - start_time;

    trace_relm_vm_exit(vcpu->vpid,
                       exit_reason,
                       vcpu->arch.regs.rip,
                       exit_qualification,
                       duration);
    return ret;
}

/*
 * vmx_handle_exit — vcpu_arch_ops.handle_exit hook for the generic run loop.
 *
 * VM-exits are fully handled by handle_vmexit(), invoked directly from the
 * VM-exit asm stub before the stack unwinds back into vcpu_run. By the time
 * the generic loop calls this hook the exit is already resolved; report
 * fatality via the state the handler left behind.
 */
int vmx_handle_exit(struct vcpu *vcpu)
{
    return (vcpu->state == VCPU_STATE_ERROR) ? -EFAULT : 0;
}

