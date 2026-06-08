#ifndef RELM_ARCH_X86_VMX_VCPU_ARCH_H
#define RELM_ARCH_X86_VMX_VCPU_ARCH_H

/*
 * include/arch/x86/vmx/vcpu_arch.h
 *
 * Defines 'struct vcpu_arch' for the x86 VMX backend.
 *
 * This file is selected by the Makefile via:
 *   ARCH_INCLUDE_DIR = include/arch/x86/vmx
 *   CFLAGS += -I$(ARCH_INCLUDE_DIR) -I<parent_so_relm_arch_resolves>
 *
 * The generic struct vcpu embeds this as:
 *   struct vcpu_arch arch;
 *
 * Everything that was previously scattered in vmx.h and vmcs_state.h
 * that is *per-vCPU* lives here.  Shared per-host-CPU state (VMXON
 * region) lives in struct host_cpu, unchanged.
 */

#include <include/arch/x86/vmx/vmcs.h>
#include <include/arch/x86/vmx/vmx.h>
#include <include/arch/x86/apic.h>

/* Maximum number of MSRs managed in the VM-entry/exit MSR load/store lists. */
#define RELM_MAX_MANAGED_MSRS   16

/* CR3 shadow cache — avoids expensive VMCS reads on every CR3 access exit.*/ 
struct cr3_shadow_cache {
    uint64_t guest_cr3;
    uint64_t shadow_cr3;
    bool     valid;
};


struct guest_regs {
    unsigned long rax, rbx, rcx, rdx;
    unsigned long rsi, rdi, rbp, rsp;
    unsigned long r8,  r9,  r10, r11;
    unsigned long r12, r13, r14, r15;
    unsigned long rip, rflags;
    unsigned long cs, ds, es, fs, gs, ss;
    unsigned long fs_base, gs_base;
} __attribute__((packed));

struct vcpu_arch {

    struct vmcs_region *vmcs;   
    uint64_t vmcs_pa;    

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
