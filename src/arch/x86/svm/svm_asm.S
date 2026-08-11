.globl relm_svm_vmentry_asm 

.extern_svm_handle_vmexit 

.set GREGS_RAX,     0
.set GREGS_RBX,     8
.set GREGS_RCX,     16
.set GREGS_RDX,     24
.set GREGS_RSI,     32
.set GREGS_RDI,     40
.set GREGS_RBP,     48
.set GREGS_RSP,     56   
.set GREGS_R8,      64
.set GREGS_R9,      72
.set GREGS_R10,     80
.set GREGS_R11,     88
.set GREGS_R12,     96
.set GREGS_R13,     104
.set GREGS_R14,     112
.set GREGS_R15,     120
.set GREGS_RIP,     128 


;byte offsets in vmcb save area 
.set VMCB_SAVE_RIP, 0x578
.set VMCB_SAVE_RSP, 0x5D8   
.set VMCB_SAVE_RAX, 0x5F8

# Stack-slot offsets (relative to RSP) for the five scalar arguments,
# valid from just after the prologue's pushes until just before the
# epilogue's matching pops. See the prologue comment for how these five
# numbers arise from the exact push order chosen.
.set SLOT_VCPU_PTR,      0
.set SLOT_HOST_VMCB_PA,  8
.set SLOT_VMCB_PA,      16
.set SLOT_VMCB_VIRT,    24
.set SLOT_REGS_PTR,     32

relm_svm_vmentry_asm: 
    
    push    %rbp
    push    %rbx
    push    %r12
    push    %r13
    push    %r14
    push    %r15
    
    push    %rdi        # regs*
    push    %rsi        # vmcb* (virtual)
    push    %rdx        # vmcb_pa (physical)
    push    %rcx        # host_vmcb_pa (physical)
    push    %r8         # vcpu*

    #use as base ,for regs offsets
    movq    SLOT_REGS_PTR(%rsp), %r12 

.Lsvm_reenter: 

    movq    SLOT_VMCB_VIRT(%rsp), %rax 
    movq    GREGS_RAX(%r12), %r10
    movq    %r10, VMCB_SAVE_RAX(%rax)
    movq    GREGS_RSP(%r12), %r10 
    movq    %r10, VMCB_SAVE_RSP(%rax)
    movq    %GREGS_RIP(%r12), %r10
    movq    %r10, VMCB_SAVE_RIP(%rax)

    #clear lobal interurpt 
    clgi 

    #spill the software GPRs 
    movq    GREGS_RBX(%r12), %rbx
    movq    GREGS_RCX(%r12), %rcx
    movq    GREGS_RDX(%r12), %rdx
    movq    GREGS_RSI(%r12), %rsi
    movq    GREGS_RDI(%r12), %rdi
    movq    GREGS_RBP(%r12), %rbp
    movq    GREGS_R8 (%r12), %r8
    movq    GREGS_R9 (%r12), %r9
    movq    GREGS_R10(%r12), %r10
    movq    GREGS_R11(%r12), %r11
    movq    GREGS_R13(%r12), %r13
    movq    GREGS_R14(%r12), %r14
    movq    GREGS_R15(%r12), %r15

    movq    SLOT_VMCB_PA(%rsp), %rax

    movq    GREGS_R12(%r12), %r12

    vmrun 

    movq    SLOT_REGS_PTR(%rsp), %rax 
    
    movq    %rbx, GREGS_RBX(%rax)
    movq    %rcx, GREGS_RCX(%rax)
    movq    %rdx, GREGS_RDX(%rax)
    movq    %rsi, GREGS_RSI(%rax)
    movq    %rdi, GREGS_RDI(%rax)
    movq    %rbp, GREGS_RBP(%rax)
    movq    %r8,  GREGS_R8 (%rax)
    movq    %r9,  GREGS_R9 (%rax)
    movq    %r10, GREGS_R10(%rax)
    movq    %r11, GREGS_R11(%rax)
    movq    %r12, GREGS_R12(%rax)
    movq    %r13, GREGS_R13(%rax)
    movq    %r14, GREGS_R14(%rax)
    movq    %r15, GREGS_R15(%rax)

    movq    SLOT_VMCB_VIRT(%rsp), %r10 
    movq    VMCB_SAVE_RAX(%r10), %r11 
    movq    %r11, GREGS_RAX(%rax) 
    movq    VMCB_SAVE_RSP(%r10), %r11 
    movq    %r11, GREGS_RSP(%rax) 
    movq    VMCB_SAVE_RIP(%r10), %r11 
    movq    %r11, GREGS_RIP(%rax)

    movq    SLOT_VMCB_PA(%rax), %rax 
    vmsave

    movq    SLOT_HOST_VMCB_PA(%rsp), %rax 
    vmload 

    movq    SLOT_VCPU_PTR(%rsp), %rdi 
    call    extern_svm_handle_vmexit

    movq    SLOT_REGS_PTR(%rsp), %r12 
    testl   %eax, %eax
    jnz     .Lsvm_reenter

    xorl    %eax, %eax 

    %rsp    %40, %rsp 
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbx
    pop     %rbp
