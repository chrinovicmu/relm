#ifndef RELM_ARCH_X86_VMX_VCPU_ARCH_H
#define RELM_ARCH_X86_VMX_VCPU_ARCH_H

/*
 * include/arch/x86/vmx/vcpu_arch.h
 * Defines 'struct vcpu_arch' for the x86 VMX backend.
 */

#include <include/arch/x86/vmx/vmcs.h>
#include <include/arch/x86/vmx/vmx.h>
#include <include/arch/x86/vmx/apic.h>

/* Maximum number of MSRs managed in the VM-entry/exit MSR load/store lists. */
#define RELM_MAX_MANAGED_MSRS   16

/*
 * 'struct cr3_shadow_cache' is defined in <arch/x86/vmx/vmx.h> (included above),
 * which carries the full CR3-target caching implementation used by the
 * relm_cr3_cache_* API. A second, minimal definition previously lived here and
 * collided with it (redefinition with different members); it has been removed.
 */

struct guest_regs {
    unsigned long rax, rbx, rcx, rdx;
    unsigned long rsi, rdi, rbp, rsp;
    unsigned long r8,  r9,  r10, r11;
    unsigned long r12, r13, r14, r15;
    unsigned long rip, rflags;
    unsigned long cs, ds, es, fs, gs, ss;
    unsigned long fs_base, gs_base;
} __attribute__((packed));

/*
 * GPR accessors indexed by the x86 instruction-encoding register number
 * (0=RAX 1=RCX 2=RDX 3=RBX 4=RSP 5=RBP 6=RSI 7=RDI 8-15=R8-R15), which is
 * what the decoder reports in src_reg/dst_reg. Exit handlers may write
 * through these: handle_vmexit() syncs vcpu->arch.regs back into the
 * on-stack GPR block before VMRESUME.
 */
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

struct vcpu_arch {

    struct vmcs_region *vmcs;
    uint64_t vmcs_pa;

    /*
     * host_rsp:          VMCS HOST_RSP value — top of the vCPU kthread stack,
     *                    written into the VMCS during Phase 2 setup.
     * vmentry_host_rsp:  the kthread RSP captured by relm_vmentry_save_rsp()
     *                    just before VMLAUNCH/VMRESUME, so the VM-exit stub can
     *                    restore the kthread stack and return to relm_vcpu_loop.
     * Both are referenced as vcpu->arch.* by vmx.c and vmexit.c but were missing
     * from this struct after the refactor.
     */
    uint64_t host_rsp;
    uint64_t vmentry_host_rsp;

    struct vmx_exec_ctrls controls; /* pin/proc/exit/entry control bitmasks  */ 

    /*
     * msr_bitmap: 4 KiB bitmap controlling which MSR accesses exit.
     *   bit = 0 → passthrough, bit = 1 -> VM-exit.
     * io_bitmap: two 4 KiB bitmaps (A + B) for I/O port interception.
     *   Both allocated as contiguous pages; io_bitmap points to bitmap A.
     */
    void    *msr_bitmap;
    uint64_t msr_bitmap_pa;

    uint8_t *io_bitmap;
    uint64_t io_bitmap_pa;

    /* Exception bitmap: bit N set -> exception vector N causes a VM-exit.  */
    uint32_t exception_bitmap;

    /*
     * VMX can automatically save/restore a list of MSRs on VM-exit and
     * VM-entry.  We maintain three lists:
     *   vmexit_store:  MSRs the hardware writes on exit  (guest values)
     *   vmexit_load:   MSRs the hardware loads on exit   (host values)
     *   vmentry_load:  MSRs the hardware loads on entry  (guest values)
     */
    struct msr_entry *vmexit_store_area;
    uint64_t          vmexit_store_pa;

    struct msr_entry *vmexit_load_area;
    uint64_t          vmexit_load_pa;

    struct msr_entry *vmentry_load_area;
    uint64_t          vmentry_load_pa;

    size_t vmexit_count;
    size_t vmentry_count; 

    uint32_t msr_indices[RELM_MAX_MANAGED_MSRS];
    uint32_t msr_count;

    /*
     *shadow CR0/CR3/CR4/CR8 and EFER shadow.
     * On VM-exit the hardware gives us the *guest* values; we keep copies
     * so generic code can read them without doing a VMCS read.
     */
    unsigned long cr0, cr3, cr4, cr8;
    unsigned long efer;

    struct guest_regs regs; 

    /* CR3 shadow cache — avoids redundant VMCS accesses.                  */
    struct cr3_shadow_cache cr3_cache;

    uint64_t gdtr_base;
    u16      gdtr_limit;
    uint64_t idtr_base;
    u16      idtr_limit;

    uint64_t exit_reason;
    uint64_t exit_qualification;

    struct virt_apic apic;
};

extern const struct vcpu_arch_ops vmx_vcpu_ops;

#endif /* RELM_ARCH_X86_VMX_VCPU_ARCH_H */
