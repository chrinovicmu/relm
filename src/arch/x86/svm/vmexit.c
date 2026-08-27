#include <linux/printk.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <asm/processor.h>  
#include <relm/vcpu.h>
#include <relm/vm.h>
#include <svm.h>
#include <vmexit.h>
#include <npt.h>        
#include <include/firmware/fw_cfg.h>
#include <include/firmware/seabios.h>
#include <include/boot/arch/x86/loader.h>
#include <include/debug/insn_dump.h>
#include <utils/utils.h> 

static void svm_advance_rip(struct vcpu *vcpu, unsigned int fallback_len)
{
    struct vmcb_save_area *save = &vcpu->arch.vmcb->save; 
    uint64_t next_rip = vcpu->arch.vmcb->controls.next_rip; 
    save->rip = next_rip ? next_rip : (save->rip + fallback_len); 
    vcpu->arch.regs.rip = save.rip; 
}

static void svm_emulate_cpuid(struct vcpu *vcpu)
{
    uint32_t leaf, subleaf;
    uint32_t eax, ebx, ecx, edx;

    leaf    = (uint32_t)vcpu->arch.regs.rax;
    subleaf = (uint32_t)vcpu->arch.regs.rcx;

    asm volatile(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf)
        : "memory"
    );

    PDEBUG("RELM: cpuid leaf=0x%x subleaf=0x%x -> eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
           leaf, subleaf, eax, ebx, ecx, edx);

    vcpu->arch.regs.rax = eax;
    vcpu->arch.regs.rbx = ebx;
    vcpu->arch.regs.rcx = ecx;
    vcpu->arch.regs.rdx = edx;
}

int relm_svm_handle_vmexit(struct vcpu *vcpu)
{
    struct vmcb_control_area *ctrl;
    uint64_t exit_code;
    int ret;

    if (!vcpu || !vcpu->arch.vmcb) {
        pr_err("RELM: relm_svm_handle_vmexit: no vcpu/vmcb\n");
        return 0;
    }

    ctrl = &vcpu->arch.vmcb->control;

    /* Exit-information cache */ 
    vcpu->arch.exit_code     = ctrl->exit_code;
    vcpu->arch.exit_info_1   = ctrl->exit_info_1;
    vcpu->arch.exit_info_2   = ctrl->exit_info_2;
    vcpu->arch.exit_int_info = ((uint64_t)ctrl->exit_int_info_err << 32) |
                               ctrl->exit_int_info;

    exit_code = vcpu->arch.exit_code;
    vcpu->stats.total_exits++;

    PDEBUG("RELM: [VPID=%u] SVM exit #%llu: code=0x%llx RIP=0x%llx\n",
           vcpu->vpid, vcpu->stats.total_exits, exit_code,
           vcpu->arch.vmcb->save.rip);

    /*
    * VMRUN instruction failed die to invalid VMCB or fields*/ 
    if (exit_code == SVM_EXITCODE_INVALID) {
        pr_err("RELM: [VPID=%u] VMRUN failed — VMCB consistency check "
               "(VMEXIT_INVALID)\n", vcpu->vpid);
        relm_dump_vcpu(vcpu);
        vcpu->state = VCPU_STATE_ERROR;
        return 0;
    }

    /*
     * Any intercepted exception vector. 
     * only #UD(6) and #PF(14) are actually gated on right
     */
    if (exit_code >= SVM_EXITCODE_EXCP_BASE &&
        exit_code <  SVM_EXITCODE_EXCP_BASE + 32) {
        uint32_t vector = (uint32_t)(exit_code - SVM_EXITCODE_EXCP_BASE);

        pr_err("RELM: [VPID=%u] Guest exception vector=%u at RIP=0x%llx\n",
               vcpu->vpid, vector, vcpu->arch.vmcb->save.rip);

        /* Treat all exceptions as fatal for now */ 
        vcpu->state = VCPU_STATE_STOPPED;
        return 0;
    }

    switch (exit_code){

        case SVM_EXITCODE_INTR:
            /* Physical maskable interrupt arrived while the guest was
            * running; just re-enter — same as VMX's
            * EXIT_REASON_EXTERNAL_INTERRUPT. */
            PDEBUG("RELM: [VPID=%u] External interrupt\n", vcpu->vpid);
            ret = 1;
            break;

         ase SVM_EXITCODE_SHUTDOWN:
            /* Guest faulted while delivering a double fault — on bare metal
            * the machine would reset. Unrecoverable; stop*/ 
            pr_err("RELM: [VPID=%u] Guest shutdown (triple fault) at RIP=0x%llx\n",
                vcpu->vpid, vcpu->arch.vmcb->save.rip);
            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0;
            break;

        case SVM_EXITCODE_INIT:
            /* INIT arriving at a running vCPU. Proper handling would put it
            * in wait-for-SIPI; until then treat as a stop request, same as
            pr_info("RELM: [VPID=%u] INIT signal received\n", vcpu->vpid);
            */ 
            vcpu->state = VCPU_STATE_STOPPED;
            ret = 0;
            break;

        case SVM_EXITCODE_HLT:
            PDEBUG("RELM: [VPID=%u] Guest executed HLT at RIP=0x%llx\n",
               vcpu->vpid, vcpu->arch.vmcb->save.rip);

            vcpu->halted = true;
            svm_advance_rip(vcpu, 1);   /* HLT is a fixed 1-byte encoding (0xF4) */
            ret = 0;
            break;

        case SVM_EXITCODE_CPUID:
            svm_emulate_cpuid(vcpu);
            svm_advance_rip(vcpu, 2);   /* CPUID is a fixed 2-byte encoding (0F A2) */
            ret = 1;
            break;

        case SVM_EXITCODE_IOIO: 
        {
             */ 
            uint64_t info1 = vcpu->arch.exit_info_1;
            uint32_t size = (info1 & (1ULL << 3)) ? 1 :
                        (info1 & (1ULL << 4)) ? 2 : 4;
            bool is_in  = (info1 & (1ULL << 0)) != 0;
            bool is_str = (info1 & (1ULL << 1)) != 0;
            bool is_rep = (info1 & (1ULL << 2)) != 0;
            uint16_t port = (uint16_t)((info1 >> 16) & 0xFFFFULL);
            uint32_t io_val = 0;

            PDEBUG("RELM: [VPID=%u] IOIO exit: %s%s%s port=0x%03x size=%u RIP=0x%llx\n",
               vcpu->vpid, is_in ? "IN" : "OUT", is_str ? " STRING" : "",
               is_rep ? " REP" : "", port, size, vcpu->arch.vmcb->save.rip);

            if((port == FW_CFG_PORT_SEL || port == FW_CF_PORT_DATA) && 
               !is_str && !is_rep){
                
                struct relm_vm *vm == vcpu->vm;
                
                if (!is_in) {
                    uint32_t size_mask = (size == 1) ? 0xFFU :
                            (size == 2) ? 0xFFFFU : 0xFFFFFFFFU;

                    io_val = (uint32_t)(vcpu->arch.regs.rax & size_mask);
                    PDEBUG("RELM: fw_cfg OUT port=0x%03x val=0x%08x (size=%u)\n",
                           port, io_val, size);
                }

                relm_fw_cfg_handle_io(&vm->fw_data->fw_cfg, port, !is_in,
                                  size, &io_val);
            
                if (is_in) {
                    vcpu->arch.regs.rax = (unsigned long)(io_val & 0xFFFFFFFFUL);
                    PDEBUG("RELM: fw_cfg IN  port=0x%03x -> val=0x%08x (size=%u) -> RAX=0x%lx\n",
                           port, io_val, size, vcpu->arch.regs.rax);
                }
            }else{
                 /* Any other port: NOP the access — OUTs swallowed, INs
                 * leave RAX untouched. */
                PDEBUG("RELM: [VPID=%u] unhandled port 0x%03x %s size=%u — NOP\n",
                       vcpu->vpid, port, is_in ? "IN" : "OUT", size);
            }
            /*EXITTINFO2 already holds the address of the instruction
            * following the IN/OUT*/ 
            vcpu->arch.vmcb->save.rip = vcpu->arch.exit_info_2;
            vcpu->arch.regs.rip = vcpu->arch.vmcb->save.rip;

            ret = 1;
            break;
        }



    }





}

