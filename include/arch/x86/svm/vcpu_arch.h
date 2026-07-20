#ifndef RELM_ARCH_X86_SVM_VCPU_ARCH_H
#define RELM_ARCH_X86_SVM_VCPU_ARCH_H

#include <linux/types.h>

struct vmcb; 

struct guest_regs {
    unsigned long rax, rbx, rcx, rdx;
    unsigned long rsi, rdi, rbp, rsp;
    unsigned long r8,  r9,  r10, r11;
    unsigned long r12, r13, r14, r15;
    unsigned long rip, rflags;
    unsigned long cs, ds, es, fs, gs, ss;
    unsigned long fs_base, gs_base;
} __attribute__((packed));


/* Field mapping to the VMCB control area 
 *   cr_intercepts : offset 0x000 — bits  0-15 = CR0..CR15 read intercepts,
 *                                  bits 16-31 = CR0..CR15 write intercepts.
 *   dr_intercepts : offset 0x004 — same read/write split for DR0..DR15.
 *   exceptions    : offset 0x008 — bit N intercepts exception vector N;
 *                   direct equivalent of the VMX exception bitmap
 *                   (vcpu->arch.exception_bitmap in the VMX backend).
 *   intercepts_1  : offset 0x00C — instruction/event intercepts word 1
 *                   (INTR, NMI, SMI, INIT, HLT, INVLPG, IOIO_PROT,
 *                    MSR_PROT, CPUID, RDTSC, ...).
 *   intercepts_2  : offset 0x010 — word 2 (VMRUN, VMMCALL, VMLOAD, VMSAVE,
 *                   STGI, CLGI, SKINIT, MONITOR, MWAIT, XSETBV, ...).
 *                   Note: the VMRUN intercept bit MUST be set — a VMCB that
 *                   lets the guest execute VMRUN un-intercepted is
 *                   architecturally invalid and VMRUN fails with exit code
 *                   VMEXIT_INVALID (-1).
 *   intercepts_3  : offset 0x014 — word 3 (INVLPGB, INVPCID, MCOMMIT, ...);
 *                   only meaningful on newer cores, kept for completeness.
 */
struct svm_intercepts {
    u32 cr_intercepts;
    u32 dr_intercepts;
    u32 exceptions;
    u32 intercepts_1;
    u32 intercepts_2;
    u32 intercepts_3;
};

struct vcpu_arch{
    
    struct vmcb *vmcb; 
    uint64_t vmcb_pa; 

    /*host save area
     * need to move it to a per-cpu struct if we plan on 
     * running multiple vcpus in the future */ 
    void *hsave; 
    uint64_t hsave_pa; 

    /*scratch VMCB used as the VMSAVE target for host second-teir state*/ 
    struct vmcb *host_vmcb; 
    uint64_t host_vmcb_pa; 

    struct svm_intercepts intercepts; 

    /*msr permissons mapL 8 KiB*/ 
    void *msrpm;
    void msrpm_pa; 

    /*I/O permissons map : 12 KiB*/ 
    uint8_t *iopm; 
    uint64_t iopm_pa; 

    /*address space indentifier tagging this vCPU's TLB entries */ 
    uint32_t asid; 

    unsigned long cr0, cr3, cr4, cr8;
    unsigned long efer;

    struct guest_regs regs;

    uint64_t gdtr_base;
    u16 gdtr_limit;
    uint64_t idtr_base;
    u16 idtr_limit; 

    uint64_t exit_code;
    uint64_t exit_info_1;
    uint64_t exit_info_2;

    /*event that was mid-delivery when exit occured */ 
    uint64_t exit_int_info;
}; 

extern const struct vcpu_arch_ops svm_vcpu_ops; 

static inline unsigned long *guest_reg_ptr(struct guest_regs *regs, int index)
{
    switch (index) {
    case 0:  return &regs->rax;
    case 1:  return &regs->rcx;
    case 2:  return &regs->rdx;
    case 3:  return &regs->rbx;
    case 4:  return &regs->rsp;
    case 5:  return &regs->rbp;
    case 6:  return &regs->rsi;
    case 7:  return &regs->rdi;
    case 8:  return &regs->r8;
    case 9:  return &regs->r9;
    case 10: return &regs->r10;
    case 11: return &regs->r11;
    case 12: return &regs->r12;
    case 13: return &regs->r13;
    case 14: return &regs->r14;
    case 15: return &regs->r15;
    default: return NULL;
    }
}

/* Out-of-range indices read as 0; writes to them are dropped. */
static inline unsigned long guest_reg_read(struct guest_regs *regs, int index)
{
    unsigned long *reg = guest_reg_ptr(regs, index);

    return reg ? *reg : 0;
}

static inline void guest_reg_write(struct guest_regs *regs, int index,
                                   unsigned long value)
{
    unsigned long *reg = guest_reg_ptr(regs, index);

    if (reg)
        *reg = value;
}


