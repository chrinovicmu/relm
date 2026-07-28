#ifndef SVM_VMEXIT_H
#define SVM_VMEXIT_H 

#include <relm/vm.h>
#include <utils/utils.h>
#include <svm.h>

#define SVM_EXITCODE_CR_READ_BASE    0x00
#define SVM_EXITCODE_CR_WRITE_BASE   0x10
#define SVM_EXITCODE_CR_READ(n)      (SVM_EXITCODE_CR_READ_BASE  + (n))
#define SVM_EXITCODE_CR_WRITE(n)     (SVM_EXITCODE_CR_WRITE_BASE + (n))

#define SVM_EXITCODE_DR_READ_BASE    0x20
#define SVM_EXITCODE_DR_WRITE_BASE   0x30
#define SVM_EXITCODE_DR_READ(n)      (SVM_EXITCODE_DR_READ_BASE  + (n))
#define SVM_EXITCODE_DR_WRITE(n)     (SVM_EXITCODE_DR_WRITE_BASE + (n))

#define SVM_EXITCODE_EXCP_BASE       0x40
#define SVM_EXITCODE_EXCP(vec)       (SVM_EXITCODE_EXCP_BASE + (vec))

#define SVM_EXITCODE_EXCP_DE   SVM_EXITCODE_EXCP(0)   /* divide error      */
#define SVM_EXITCODE_EXCP_DB   SVM_EXITCODE_EXCP(1)   /* debug             */
#define SVM_EXITCODE_EXCP_BP   SVM_EXITCODE_EXCP(3)   /* breakpoint        */
#define SVM_EXITCODE_EXCP_OF   SVM_EXITCODE_EXCP(4)   /* overflow          */
#define SVM_EXITCODE_EXCP_UD   SVM_EXITCODE_EXCP(6)   /* invalid opcode    */
#define SVM_EXITCODE_EXCP_NM   SVM_EXITCODE_EXCP(7)   /* device not avail  */
#define SVM_EXITCODE_EXCP_DF   SVM_EXITCODE_EXCP(8)   /* double fault      */
#define SVM_EXITCODE_EXCP_TS   SVM_EXITCODE_EXCP(10)  /* invalid TSS       */
#define SVM_EXITCODE_EXCP_NP   SVM_EXITCODE_EXCP(11)  /* segment not present */
#define SVM_EXITCODE_EXCP_SS   SVM_EXITCODE_EXCP(12)  /* stack fault       */
#define SVM_EXITCODE_EXCP_GP   SVM_EXITCODE_EXCP(13)  /* general protection */
#define SVM_EXITCODE_EXCP_PF   SVM_EXITCODE_EXCP(14)  /* page fault        */
#define SVM_EXITCODE_EXCP_MF   SVM_EXITCODE_EXCP(16)  /* x87 FP error      */
#define SVM_EXITCODE_EXCP_AC   SVM_EXITCODE_EXCP(17)  /* alignment check   */
#define SVM_EXITCODE_EXCP_MC   SVM_EXITCODE_EXCP(18)  /* machine check     */
#define SVM_EXITCODE_EXCP_XM   SVM_EXITCODE_EXCP(19)  /* SIMD FP exception */

#define SVM_EXITCODE_SWINT_BASE      0x75

#define SVM_EXITCODE_INTR                0x60  
#define SVM_EXITCODE_NMI                 0x61
#define SVM_EXITCODE_SMI                 0x62
#define SVM_EXITCODE_INIT                0x63  
#define SVM_EXITCODE_VINTR               0x64  
#define SVM_EXITCODE_CR0_SEL_WRITE       0x65  
#define SVM_EXITCODE_IDTR_READ           0x66
#define SVM_EXITCODE_GDTR_READ           0x67
#define SVM_EXITCODE_LDTR_READ           0x68
#define SVM_EXITCODE_TR_READ             0x69
#define SVM_EXITCODE_IDTR_WRITE          0x6A
#define SVM_EXITCODE_GDTR_WRITE          0x6B
#define SVM_EXITCODE_LDTR_WRITE          0x6C
#define SVM_EXITCODE_TR_WRITE            0x6D
#define SVM_EXITCODE_RDTSC               0x6E
#define SVM_EXITCODE_RDPMC               0x6F
#define SVM_EXITCODE_PUSHF               0x70
#define SVM_EXITCODE_POPF                0x71
#define SVM_EXITCODE_CPUID               0x72  
#define SVM_EXITCODE_RSM                 0x73
#define SVM_EXITCODE_IRET                0x74
#define SVM_EXITCODE_INVD                0x76  
#define SVM_EXITCODE_PAUSE               0x77  
#define SVM_EXITCODE_HLT                 0x78  
#define SVM_EXITCODE_INVLPG              0x79  
#define SVM_EXITCODE_INVLPGA             0x7A  
#define SVM_EXITCODE_IOIO                0x7B  
#define SVM_EXITCODE_MSR                 0x7C  
#define SVM_EXITCODE_TASK_SWITCH         0x7D  
#define SVM_EXITCODE_FERR_FREEZE         0x7E
#define SVM_EXITCODE_SHUTDOWN            0x7F
#define SVM_EXITCODE_VMRUN               0x80  
#define SVM_EXITCODE_VMMCALL             0x81  
#define SVM_EXITCODE_VMLOAD              0x82
#define SVM_EXITCODE_VMSAVE              0x83
#define SVM_EXITCODE_STGI                0x84
#define SVM_EXITCODE_CLGI                0x85
#define SVM_EXITCODE_SKINIT              0x86
#define SVM_EXITCODE_RDTSCP              0x87  
#define SVM_EXITCODE_ICEBP               0x88
#define SVM_EXITCODE_WBINVD              0x89
#define SVM_EXITCODE_MONITOR             0x8A  
#define SVM_EXITCODE_MWAIT               0x8B  
#define SVM_EXITCODE_MWAIT_CONDITIONAL   0x8C
#define SVM_EXITCODE_XSETBV              0x8D  
#define SVM_EXITCODE_NPF                 0x400

/* AVIC exits — present for completeness; unreachable until AVIC (SVM's
 * APICv analogue) is wired up, per the "no virt_apic member yet" note in
 * vcpu_arch.h. */
#define SVM_EXITCODE_AVIC_INCOMPLETE_IPI 0x401
#define SVM_EXITCODE_AVIC_NOACCEL        0x402
#define SVM_EXITCODE_INVALID              ((uint64_t)-1)


extern struct vcpu *relm_get_current_vcpu(void);
int relm_svm_handle_vmexit(struct vcpu *vcpu);

/*
 * relm_svm_handle_exit() — vcpu_arch_ops.handle_exit hook for the
 * generic run loop; the SVM analogue of vmx_handle_exit(). By the time
 * the generic loop calls this, relm_svm_handle_vmexit() has already
 * fully resolved the exit; this just translates vcpu->state into the
 * errno the generic loop expects.
 */
int relm_svm_handle_exit(struct vcpu *vcpu);

#endif /* SVM_VMEXIT_H */
