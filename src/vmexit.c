#include <linux/printk.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <include/vmx.h>
#include <include/vm.h>
#include <include/ept.h>
#include <include/vmx_ops.h>
#include <include/vmexit.h>
#include <include/vmcs_state.h>
#include <include/firmware/fw_cfg.h> 
#include <utils/utils.h>

#define CREATE_TRACE_POINTS 
#include <include/trace/events/relm.h> 

void relm_vmentry_save_rsp(uint64_t rsp)
{
    struct vcpu *vcpu = relm_get_current_vcpu(); 

    if(unlikely(!vcpu))
    {
        pr_err("RELM: relm_vmentry_save_rsp: no current VCPU on CPU%d", 
               smp_processor_id());
        return; 
    }

    vcpu->vmentry_host_rsp = rsp; 
    PDEBUG("RELM: VCPU%d: kthread RSP 0x%llx saved before VM-entry on CPU%d\n",
           vcpu->vpid, rsp, smp_processor_id());
}
static void emulate_cpuid(struct vcpu *vcpu)
{
    uint32_t leaf, subleaf;
    uint32_t eax, ebx, ecx, edx;

    /* guest inputs */
    leaf    = (uint32_t)vcpu->regs.rax;
    subleaf = (uint32_t)vcpu->regs.rcx;

    asm volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf)
        : "memory"
    );

    /* log BEFORE state mutation (avoids confusion during tracing) */
    PDEBUG("cpuid leaf=0x%x subleaf=0x%x -> eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
           leaf, subleaf, eax, ebx, ecx, edx);

    /* write results back to guest register state */
    vcpu->regs.rax = eax;
    vcpu->regs.rbx = ebx;
    vcpu->regs.rcx = ecx;
    vcpu->regs.rdx = edx;
}
uint64_t relm_vmentry_get_rsp(void)
{
    struct vcpu *vcpu = relm_get_current_vcpu(); 

    if(unlikely(!vcpu))
    {
        pr_err("RELM: relm_vmentry_get_rsp: no current VCPU on CPU%d\n", 
               smp_processor_id());
        return 0; 
    }

    if(unlikely(vcpu->vmentry_host_rsp == 0))
    {
        pr_err("RELM: VCPU%d: vmentry_host_rsp is 0",
               vcpu->vpid); 
        return 0; 
    }

    PDEBUG("RELM: VCPU%d: returning kthread RSP 0x%llx to vmexit_handler on CPU%d\n",
           vcpu->vpid, vcpu->vmentry_host_rsp, smp_processor_id());
 
    return vcpu->vmentry_host_rsp;
}

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

    /*check if VM-entry failure */
    if(exit_reason & (1U << 32))
    {
        pr_err("relm: [VPID=%u] VM-entry failure in exit handler\n",
               vcpu->vpid);
        return 0;
    }
    
    exit_reason = exit_reason & 0xFFFF;

    exit_qualification = __vmread(VM_EXIT_QUALIFICATION);
    guest_rip = __vmread(GUEST_RIP);
    guest_rsp = __vmread(GUEST_RSP);

    vcpu->stats.total_exits++;

    if(!vcpu->launched)
    {
        vcpu->launched = 1;
        pr_info("relm: [VPID=%u] First VM-exit (exit #%llu), now using VMRESUME\n",
                vcpu->vpid, vcpu->stats.total_exits);
    }

    vcpu->regs.rax = guest_gprs->rax;
    vcpu->regs.rbx = guest_gprs->rbx;
    vcpu->regs.rcx = guest_gprs->rcx;
    vcpu->regs.rdx = guest_gprs->rdx;
    vcpu->regs.rsi = guest_gprs->rsi;
    vcpu->regs.rdi = guest_gprs->rdi;
    vcpu->regs.rbp = guest_gprs->rbp;
    vcpu->regs.r8  = guest_gprs->r8;
    vcpu->regs.r9  = guest_gprs->r9;
    vcpu->regs.r10 = guest_gprs->r10;
    vcpu->regs.r11 = guest_gprs->r11;
    vcpu->regs.r12 = guest_gprs->r12;
    vcpu->regs.r13 = guest_gprs->r13;
    vcpu->regs.r14 = guest_gprs->r14;
    vcpu->regs.r15 = guest_gprs->r15;

    vcpu->regs.rsp = guest_rsp;
    vcpu->regs.rip = guest_rip;

    PDEBUG("relm: [VPID=%u] Exit #%llu: reason=%llu RIP=0x%llx\n",
           vcpu->vpid, vcpu->stats.total_exits, exit_reason, guest_rip);

    switch(exit_reason)
    {
        case EXIT_REASON_EXCEPTION_NMI:
        {
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

            pr_err("relm: [VPID=%u] Guest triple fault at RIP=0x%llx\n",
                   vcpu->vpid, guest_rip);
            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0;
            break; 

        case EXIT_REASON_APIC_ACCESS:
            ret = relm_apic_handle_access(vcpu);
            break;  

        case EXIT_REASON_INIT_SIGNAL:

            pr_info("relm: [VPID=%u] INIT signal received\n", vcpu->vpid);
            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0; 
            break; 

        case EXIT_REASON_HLT:

            pr_info("relm: [VPID=%u] Guest executed HLT at RIP=0x%llx\n",
                    vcpu->vpid, guest_rip);
            vcpu->halted = true;
            vcpu->state = VCPU_STATE_HALTED;

            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);

            /* stop execution on HLT */
            ret = 0; 
            break; 

        case EXIT_REASON_CPUID:
        {
            emulate_cpuid(vcpu);
            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);
            ret = 1; 
            break; 
        }


        case EXIT_REASON_IO_INSTRUCTION:
        {
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
                        
                    io_val = (uint32_t)(vcpu->regs.rax & size_mask); 
                    PDEBUG("RELM: fw_cfg OUT port=0x%03x val=0x%08x (size=%u)",
                           port, io_val, size);
                }

                ret = relm_fw_cfg_handle_io(&vm->fw_data->fw_cfg, port, !is_in, size, &io_val);

                /*guest is reading from fw_cfg*/  
                if(is_in)
                {
                /*zero exend io_val to 64 bits and write to guest RAX*/ 
                    vcpu->regs.rax = (unsigned long)(io_val & 0xFFFFFFFFUL); 
                    _vmwrite(0x6818, vcpu->regs.rax); 

                    PDEBUG("RELM: fw_cfg IN  port=0x%03x → val=0x%08x "
                               "(size=%u) → RAX=0x%lx",
                               port, io_val, size, vcpu->regs.rax);
                }

                instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
                _vmwrite(GUEST_RIP, guest_rip + instr_len);
                break; 
            }

            PDEBUG("RELM: [VPID=%u] unhandled port 0x%03x %s size=%u — NOP",
                   vcpu->vpid, port, is_in ? "IN" : "OUT", size);
 
            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);
            ret = 1;
            break;
        }

        case EXIT_REASON_VMCALL:

            pr_info("relm: [VPID=%u] VMCALL hypercall at RIP=0x%llx\n",
                    vcpu->vpid, guest_rip);

            /*TODO: implement hypercall
            * advance RIP for now*/
            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);
            ret = 1;
            break; 

        case EXIT_REASON_MSR_READ:
        {
            uint32_t msr = vcpu->regs.rcx & 0xFFFFFFFF;
            pr_info("relm: [VPID=%u] RDMSR 0x%x at RIP=0x%llx\n",
                    vcpu->vpid, msr, guest_rip);

            /*TODO: emulate MSR_READ
             * pass 0 for now*/
            guest_gprs->rax = 0;
            guest_gprs->rdx = 0;
            vcpu->regs.rax = 0;
            vcpu->regs.rdx = 0;

            instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
            _vmwrite(GUEST_RIP, guest_rip + instr_len);
            ret = 1;
            break; 

        }

        case EXIT_REASON_MSR_WRITE:
        {
            uint32_t msr = vcpu->regs.rcx & 0xFFFFFFFFULL;
            uint64_t val = ((uint64_t)vcpu->regs.rdx << 32) | 
                    (uint64_t)(vcpu->regs.rax & 0xFFFFFFFF);

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
                bool pg = (guest_cr0 & (1ULL << 32)) != 0; 
                bool lma = lma && pg; 

                if(lma)
                    val != (1ULL << 10); 

                _vmwrite(VMCS_GUEST_IA32_EFER, val); 
                vcpu->efer = val; 


                PDEBUG("RELM: [VPID=%u] WRMSR EFER: LME=%u PG=%u → "
                        "LMA=%u EFER=0x%llx",
                        vcpu->vpid, lme ? 1:0, pg ? 1:0, lma ? 1:0, val);

                /*sync IA32_MODE_GUEST in vm-entry controls 
                 * we update it now, so the very next VMRESUME is in 64 bit long mode.*/
                uint32_t entry_ctrl = (uint32_t)__vmread(VMCS_ENTRY_CONTROLS); 
                if(lma)
                {
                    /*long mode active: set IA32_MODE_GUEST*/  
                    entry_ctrl |= VMCS_ENTRY_IA32E_MODE;
                    PDEBUG("RELM: [VPID=%u] IA32E_MODE_GUEST → 1 "
                            "(guest entered 64-bit long mode)", vcpu->vpid);
                }else{
                    /*long mode not active : clear IA32E_MODE_GUEST*/ 
                    entry_ctrl = &= ~(uint32_t)VMCS_ENTRY_IA32E_MODE; 
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
            uint64_t gpa = __vmread(GUEST_PHYSICAL_ADDRESS);
            bool data_read = exit_qualification & (1ULL << 0);
            bool data_write = exit_qualification & (1ULL << 1);
            bool instr_fetch = exit_qualification & (1ULL << 2);
            bool ept_readable = exit_qualification & (1ULL << 3);
            bool ept_writable = exit_qualification & (1ULL << 4);
            bool ept_executable = exit_qualification & (1ULL << 5);

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
               acc_type == CR_ACCESS_TYPE_WRITE)
            {
                /* CR3 write: delegate to the CR3 shadow cache handler.
                 * It records the new CR3 value, updates VMCS GUEST_CR3,
                 * advances GUEST_RIP, and potentially promotes hot values
                 * to suppress future exits. */
                ret = relm_cr3_cache_handle_exit(vcpu, exit_qualification);
                break; 
            }
            else
            {
                /* CR0, CR4, CR8 write, or CR read — not yet handled.
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
       
        default:

            pr_err("relm: [VPID=%u] Unhandled VM-exit reason %llu\n",
                   vcpu->vpid, exit_reason);
            pr_err(" Guest RIP: 0x%llx\n", guest_rip);
            pr_err(" Exit qualification: 0x%llx\n", exit_qualification);

            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0; 
            break;  
    }

    end_time = ktime_get_ns(); 
    duration = end_time - start_time; 

    trace_relm_vm_exit(vcpu->vpid, 
                       exit_reason, 
                       vcpu->regs.rip, 
                       exit_qualification, 
                       duration); 
    return ret; 
}

