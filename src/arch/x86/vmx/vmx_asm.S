
# src/arch/x86/vmx/vmx_asm.S — the VMX world-switch code.
#
# This file owns the only two moments where the CPU crosses between host
# and guest execution:
#
#   relm_vmentry_asm    host -> guest. Loads the guest's general-purpose
#                       registers from a 'struct guest_regs' and executes
#                       VMLAUNCH (first entry) or VMRESUME (later entries).
#   relm_vmexit_handler guest -> host. The address programmed into VMCS
#                       HOST_RIP: the CPU jumps here on every VM-exit.
#                       Captures the guest GPRs, calls the C exit handler,
#                       then either VMRESUMEs straight back into the guest
#                       or unwinds to the vCPU kthread.
#
# Why assembly is required at all: the hardware saves/restores only RIP,
# RSP and RFLAGS (plus VMCS-managed state) across entry/exit. The 15
# general-purpose registers are OUR problem — at the vmlaunch instruction
# every GPR must already hold the guest's value, and at HOST_RIP every GPR
# still holds whatever the guest left in it. No C code can run in either
# window (it would clobber the very registers being carried), so the
# load/save must be done here.
#
# This code runs in kernel mode, with interrupts disabled or controlled,
# and must obey the System V AMD64 ABI where applicable.

#include <asm/linkage.h>

.globl relm_vmentry_asm
.globl relm_vmexit_handler

.extern relm_vmentry_save_rsp
.extern relm_vmentry_get_rsp
.extern handle_vmexit

# Offsets into the on-stack GPR save block built by relm_vmexit_handler's
# push sequence below (15 pushes, RAX first). Pushes grow the stack down,
# so the LAST push (r15) sits at offset 0 and the FIRST (rax) at 112.
# This layout is what C sees as 'struct stack_guest_gprs' — the pointer
# handed to handle_vmexit(). Keep the two in sync.
.set VCPU_R15, 0
.set VCPU_R14, 8
.set VCPU_R13, 16
.set VCPU_R12, 24
.set VCPU_R11, 32
.set VCPU_R10, 40
.set VCPU_R9,  48
.set VCPU_R8,  56
.set VCPU_RBP, 64
.set VCPU_RDI, 72
.set VCPU_RSI, 80
.set VCPU_RDX, 88
.set VCPU_RCX, 96
.set VCPU_RBX, 104
.set VCPU_RAX, 112

# Field offsets into 'struct guest_regs' (include/arch/x86/vmx/vcpu_arch.h,
# packed) — the source relm_vmentry_asm loads guest registers from.
.set GREGS_RAX,   0
.set GREGS_RBX,   8
.set GREGS_RCX,  16
.set GREGS_RDX,  24
.set GREGS_RSI,  32
.set GREGS_RDI,  40
.set GREGS_RBP,  48
# GREGS_RSP = 56 - NOT loaded; CPU reads GUEST_RSP from the VMCS.
.set GREGS_R8,   64
.set GREGS_R9,   72
.set GREGS_R10,  80
.set GREGS_R11,  88
.set GREGS_R12,  96
.set GREGS_R13, 104
.set GREGS_R14, 112
.set GREGS_R15, 120
# GREGS_RIP    = 128 - NOT loaded; CPU reads GUEST_RIP from the VMCS.
# GREGS_RFLAGS = 136 - NOT loaded; CPU reads GUEST_RFLAGS from the VMCS.

# Size of the 15-register save block pushed on VM-exit (15 * 8 bytes).
.set VMEXIT_SAVE_SIZE, 120


# relm_vmentry_asm(struct guest_regs *regs /* RDI */, int launched /* RSI */)
#
# Enter the guest. Called from relm_vcpu_loop as an ordinary C function,
# but it does NOT return the ordinary way on success: a successful
# VMLAUNCH/VMRESUME switches to the guest, and control only comes back at
# relm_vmexit_handler (via VMCS HOST_RIP) on the next VM-exit. The path
# back to our caller runs through .Lvmexit_return_zero below, which
# restores the RSP saved here and pops the same six callee-saved registers
# — the two routines share one stack frame protocol.
#
# Falling THROUGH vmlaunch/vmresume means the instruction itself failed
# (bad VMCS state, wrong launch flag, ...); then we unwind normally and
# return -1 so the C loop can report the error.
relm_vmentry_asm:
    #RDI : guest_regs pointer
    #RSI : lanched flag

    # Save the callee-saved registers the ABI makes us preserve. The
    # matching pops are in vmx_error (failure) and .Lvmexit_return_zero
    # (normal exit path) — all three sequences must stay mirror images.
    push    %rbp
    push    %rbx
    push    %r12
    push    %r13
    push    %r14
    push    %r15

    # Stash the arguments in callee-saved registers: the helper call below
    # is allowed to clobber RDI/RSI (caller-saved), and we still need both
    # values after it returns.
    mov     %rdi, %r12
    movl    %esi, %r13d

    # Record the current kthread RSP (post-push) in vcpu->arch via a C
    # helper. relm_vmentry_get_rsp() returns this exact value on the exit
    # path so .Lvmexit_return_zero can find the frame we just built.
    movq    %rsp, %rdi
    callq   relm_vmentry_save_rsp

    # Load the guest GPRs from the guest_regs struct (base pointer in
    # r12). RSP/RIP/RFLAGS are deliberately absent — the CPU takes those
    # from the VMCS guest-state area on entry.
    movq    GREGS_RAX(%r12), %rax
    movq    GREGS_RBX(%r12), %rbx
    movq    GREGS_RCX(%r12), %rcx
    movq    GREGS_RDX(%r12), %rdx
    movq    GREGS_RSI(%r12), %rsi
    movq    GREGS_RBP(%r12), %rbp
    movq    GREGS_R8 (%r12), %r8
    movq    GREGS_R9 (%r12), %r9
    movq    GREGS_R10(%r12), %r10
    movq    GREGS_R11(%r12), %r11
    movq    GREGS_R14(%r12), %r14
    movq    GREGS_R15(%r12), %r15

    # Test the launched flag NOW, while r13 still holds it — the next
    # three loads overwrite r13 and r12 with guest values. MOV does not
    # touch RFLAGS, so the ZF result survives until the jnz.
    test %r13d, %r13d

    # Last three loads: rdi (freed after the helper call), then r13 and
    # r12 themselves — pointer and flag are dead from here on.
    movq    GREGS_RDI(%r12), %rdi
    movq    GREGS_R13(%r12), %r13
    movq    GREGS_R12(%r12), %r12

    jnz     do_resume

do_launch:
    # First entry on this VMCS (launched == 0): VMLAUNCH is required.
    # On success this instruction does not fall through — the next host
    # code to run is relm_vmexit_handler.
    vmlaunch
    jmp     vmx_error

do_resume:
    # Subsequent entries (launched != 0): VMRESUME. Same no-fall-through
    # contract as vmlaunch; falling into vmx_error means it failed.
    vmresume

vmx_error:
    # VMLAUNCH/VMRESUME failed (VMfailValid/VMfailInvalid). Unwind the
    # frame built at function entry and return -1 to relm_vcpu_loop, which
    # reads the VMCS error field for diagnostics.
    movl    $-1, %eax  #return val
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbx
    pop     %rbp
    ret

# relm_vmexit_handler — VMCS HOST_RIP target; entered on every VM-exit.
#
# On arrival: RSP = VMCS HOST_RSP (top of this vCPU's kthread stack,
# programmed during VMCS setup) and every GPR still holds its GUEST value.
# Nothing may be clobbered before it is saved.
#
# Return paths, decided by handle_vmexit()'s return value:
#   1 (continue) -> pop the (possibly modified) guest GPRs, VMRESUME.
#   0 (stop)     -> discard the GPR block, restore the kthread RSP saved
#                   by relm_vmentry_asm, return 0 to the C run loop.
relm_vmexit_handler:
    #CPU has already reloaded HOST_RSP from VMCS.
    # Capture all guest GPRs onto the host stack. Push order defines the
    # struct stack_guest_gprs layout (see VCPU_* offsets above): after the
    # last push, RSP points at r15 == offset 0 == the struct's first field.
    push    %rax
    push    %rbx
    push    %rcx
    push    %rdx
    push    %rsi
    push    %rdi
    push    %rbp
    push    %r8
    push    %r9
    push    %r10
    push    %r11
    push    %r12
    push    %r13
    push    %r14
    push    %r15

    # First argument to handle_vmexit(): pointer to the save block just
    # built — RSP itself. The C handler reads guest registers from it and
    # writes emulation results back into it (via the vcpu->arch.regs
    # round-trip; see the sync comment at the bottom of handle_vmexit).
    movq    %rsp, %rdi
    # align stack to 16 bytes for C ABI
    sub     $8, %rsp
    call    handle_vmexit
    #restore alignment
    add     $8, %rsp

    # handle_vmexit returned: 0 = unwind to the kthread, nonzero = re-enter
    # the guest.
    testl   %eax, %eax
    jz      .Lvmexit_stop

    # Re-enter path: reload the guest GPRs from the save block. Pops
    # mirror the pushes above, and pick up any values the C handler
    # modified (CPUID results, MMIO read data, ...).
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %r11
    pop     %r10
    pop     %r9
    pop     %r8
    pop     %rbp
    pop     %rdi
    pop     %rsi
    pop     %rdx
    pop     %rcx
    pop     %rbx
    pop     %rax

    # Straight back into the guest. RSP is back at HOST_RSP top, which is
    # exactly what the next exit expects. No fall-through on success.
    vmresume

    /* VMRESUME failed. NOTE: the save block was already consumed by the
     pops above, so this addq skips PAST the kthread stack top — it only
     avoids harm because .Lvmexit_return_zero reloads RSP from the saved
     kthread RSP two instructions later (the intervening call briefly
     pushes above the stack top; suspicious but currently unexercised).

    */ 

    addq    $VMEXIT_SAVE_SIZE, %rsp
    jmp     .Lvmexit_return_zero

.Lvmexit_stop:
    # Stop path: the guest GPRs are already captured in vcpu->arch.regs by
    # handle_vmexit, so the on-stack block is dead weight — drop it.
    addq    $VMEXIT_SAVE_SIZE, %rsp

.Lvmexit_return_zero:
    # Fetch the kthread RSP that relm_vmentry_asm recorded just before
    # entering the guest. 0 means no current vCPU / never saved — there is
    # no stack to return on, so park the CPU (.Lvmexit_fatal).
    call    relm_vmentry_get_rsp
    testq   %rax, %rax
    jz      .Lvmexit_fatal

    # Abandon the exit-time stack and jump back onto relm_vmentry_asm's
    # frame, then finish that function's epilogue on its behalf: pop the
    # six callee-saved registers it pushed and return 0 to relm_vcpu_loop
    # — as if the original relm_vmentry_asm call had returned normally.
    movq    %rax, %rsp
    xorl    %eax, %eax

    pop     %r15                       # restore callee-saved (vmentry_asm step 1)
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbx
    pop     %rbp
    RET                            # return 0 to relm_vcpu_loop

.Lvmexit_fatal:
    # No saved RSP to unwind to. Halt this CPU in a loop rather than run
    # C code on a corrupt stack. (Interrupts can wake HLT, hence the jmp.)
    hlt
    jmp     .Lvmexit_fatal

