
#define REGS_X0    0
#define REGS_X1    8
#define REGS_X2    16
#define REGS_X3    24
#define REGS_X4    32
#define REGS_X5    40
#define REGS_X6    48
#define REGS_X7    56
#define REGS_X8    64
#define REGS_X9    72
#define REGS_X10   80
#define REGS_X11   88
#define REGS_X12   96
#define REGS_X13   104
#define REGS_X14   112
#define REGS_X15   120
#define REGS_X16   128
#define REGS_X17   136
#define REGS_X18   144
#define REGS_X19   152
#define REGS_X20   160
#define REGS_X21   168
#define REGS_X22   176
#define REGS_X23   184
#define REGS_X24   192
#define REGS_X25   200
#define REGS_X26   208
#define REGS_X27   216
#define REGS_X28   224
#define REGS_X29   232
#define REGS_X30   240
#define REGS_SP_EL0   248
#define REGS_PC       264
#define REGS_PSTATE   272
#define REGS_ESR_EL2  280
#define REGS_FAR_EL2  288
#define REGS_HPFAR_EL2 296

/* Offset of 'fp' within struct vcpu_arch; pinned by its own static_assert
 * in vcpu_arch.h specifically because entry.S needs the literal number. */
#define VCPU_FP    528

#define FP_Q0   0
#define FP_Q2   32
#define FP_Q4   64
#define FP_Q6   96
#define FP_Q8   128
#define FP_Q10  160
#define FP_Q12  192
#define FP_Q14  224
#define FP_Q16  256
#define FP_Q18  288
#define FP_Q20  320
#define FP_Q22  352
#define FP_Q24  384
#define FP_Q26  416
#define FP_Q28  448
#define FP_Q30  480
#define FP_FPSR 512
#define FP_FPCR 516


/* enum relm_arm64_exit_kind values (vcpu_arch.h) — entry.S returns one of
 * these via x0; keep in sync with the enum. */
#define EXIT_SYNC    1
#define EXIT_IRQ     2
#define EXIT_FIQ     3
#define EXIT_SERROR  4
 
/*
 * relm_arm64_vcpu_run_asm()'s own stack frame. FR_CTXPTR holds &vcpu_arch
 * across the entire guest run — no GPR survives an eret/trap-back
 * untouched, so this is the only place that pointer can live. The vector
 * table reads it back out at FR_CTXPTR to recover the context, then
 * reuses the same slot to stash the exit code briefly (see
 * relm_arm64_guest_exit_common). FR_GX0_GX1 is where a vector stub parks
 * the guest's real x0/x1 before it can safely overwrite x0 with an exit
 * code.
 */
#define FR_X18_X19   0
#define FR_X20_X21   16
#define FR_X22_X23   32
#define FR_X24_X25   48
#define FR_X26_X27   64
#define FR_X28_X29   80
#define FR_LR_SPEL0  96
#define FR_CTXPTR    112
#define FR_GX0_GX1   128
#define FRAME_SIZE   144 


.text 
.globl relm_arm64_vcpu_run_asm 
.globl relm_hyp_vectors 

relm_arm64_vcpu_run_asm:
    sub     sp, sp, #FRAME_SIZE
    stp	    x18, x19, [sp, #FR_X18_X19]
	stp	    x20, x21, [sp, #FR_X20_X21]
	stp	    x22, x23, [sp, #FR_X22_X23]
	stp	    x24, x25, [sp, #FR_X24_X25]
	stp	    x26, x27, [sp, #FR_X26_X27]
	stp	    x28, x29, [sp, #FR_X28_X29] 
    mrs     x9, sp_el0 
    stp	    x30, x9,  [sp, #FR_LR_SPEL0]

    /*vcpu_arch struct */ 
    str     x0, [sp, #FR_CTXPTR]

    /*x29 now holds pointer ro vcpu_arch struct */ 
    mov	    x29, x0 

    /*set up guest user-mode stack pointer*/ 
    ldr     x1, [x29, #REGS_SP_EL0]
    msr     sp_el0;

    ldr     x1, [x29 #REGS_PC]
    msr     elr_el2, x1 
    ldr     x1, [x29, #REGS_PSTATE]
    msr     spsr_el2, x1 

    /*load floating point and SIMD register bank */ 
    add     x2, x29, #VCPU_FP
    ldp	    q0,  q1,  [x2, #FP_Q0]
	ldp	    q2,  q3,  [x2, #FP_Q2]
	ldp	    q4,  q5,  [x2, #FP_Q4]
	ldp	    q6,  q7,  [x2, #FP_Q6]
	ldp	    q8,  q9,  [x2, #FP_Q8]
	ldp	    q10, q11, [x2, #FP_Q10]
	ldp	    q12, q13, [x2, #FP_Q12]
	ldp	    q14, q15, [x2, #FP_Q14]
	ldp	    q16, q17, [x2, #FP_Q16]
	ldp	    q18, q19, [x2, #FP_Q18]
	ldp	    q20, q21, [x2, #FP_Q20]
	ldp	    q22, q23, [x2, #FP_Q22]
	ldp	    q24, q25, [x2, #FP_Q24]
	ldp	    q26, q27, [x2, #FP_Q26]
	ldp	    q28, q29, [x2, #FP_Q28]
	ldp	    q30, q31, [x2, #FP_Q30]
	ldr	    w3, [x2, #FP_FPSR]
	msr	    fpsr, x3
	ldr	    w3, [x2, #FP_FPCR]
	msr	    fpcr, x3


    isb

    /*load general purpose register bank*/ 
    ldp	    x0,  x1,  [x29, #REGS_X0]
	ldp	    x2,  x3,  [x29, #REGS_X2]
	ldp	    x4,  x5,  [x29, #REGS_X4]
	ldp	    x6,  x7,  [x29, #REGS_X6]
	ldp	    x8,  x9,  [x29, #REGS_X8]
	ldp	    x10, x11, [x29, #REGS_X10]
	ldp	    x12, x13, [x29, #REGS_X12]
	ldp	    x14, x15, [x29, #REGS_X14]
	ldp	    x16, x17, [x29, #REGS_X16]
	ldp	    x18, x19, [x29, #REGS_X18]
	ldp	    x20, x21, [x29, #REGS_X20]
	ldp	    x22, x23, [x29, #REGS_X22]
	ldp	    x24, x25, [x29, #REGS_X24]
	ldp	    x26, x27, [x29, #REGS_X26]
	ldr	    x28,      [x29, #REGS_X28]
	ldp	    x29, x30, [x29, #REGS_X29]

    /*eret transfers control to guest */ 
    eret

    /*prevent straight line speculation */ 
	sb

relm_arm64_guest_exit_common:

    stp	    x2,  x3,  [x1, #REGS_X2]
	stp	    x4,  x5,  [x1, #REGS_X4]
	stp	    x6,  x7,  [x1, #REGS_X6]
	stp	    x8,  x9,  [x1, #REGS_X8]
	stp	    x10, x11, [x1, #REGS_X10]
	stp	    x12, x13, [x1, #REGS_X12]
	stp	    x14, x15, [x1, #REGS_X14]
	stp	    x16, x17, [x1, #REGS_X16]
	stp	    x18, x19, [x1, #REGS_X18]
	stp	    x20, x21, [x1, #REGS_X20]
	stp	    x22, x23, [x1, #REGS_X22]
	stp	    x24, x25, [x1, #REGS_X24]
	stp	    x26, x27, [x1, #REGS_X26]
	stp	    x28, x29, [x1, #REGS_X28]
	str	    x30,      [x1, #REGS_X30]

    ldp	    x2, x3, [sp, #FR_GX0_GX1]
	stp	    x2, x3, [x1, #REGS_X0]

    mrs	    x2, sp_el0
	str	    x2, [x1, #REGS_SP_EL0]
	mrs	    x2, elr_el2
	str	    x2, [x1, #REGS_PC]
	mrs	    x2, spsr_el2
	str	    x2, [x1, #REGS_PSTATE]
	mrs	    x2, esr_el2
	str	    x2, [x1, #REGS_ESR_EL2]
	mrs	    x2, far_el2
	str	    x2, [x1, #REGS_FAR_EL2]
	mrs	    x2, hpfar_el2
	str	    x2, [x1, #REGS_HPFAR_EL2]

    add	x2, x1, #VCPU_FP
	stp	q0,  q1,  [x2, #FP_Q0]
	stp	q2,  q3,  [x2, #FP_Q2]
	stp	q4,  q5,  [x2, #FP_Q4]
	stp	q6,  q7,  [x2, #FP_Q6]
	stp	q8,  q9,  [x2, #FP_Q8]
	stp	q10, q11, [x2, #FP_Q10]
	stp	q12, q13, [x2, #FP_Q12]
	stp	q14, q15, [x2, #FP_Q14]
	stp	q16, q17, [x2, #FP_Q16]
	stp	q18, q19, [x2, #FP_Q18]
	stp	q20, q21, [x2, #FP_Q20]
	stp	q22, q23, [x2, #FP_Q22]
	stp	q24, q25, [x2, #FP_Q24]
	stp	q26, q27, [x2, #FP_Q26]
	stp	q28, q29, [x2, #FP_Q28]
	stp	q30, q31, [x2, #FP_Q30]
	mrs	x3, fpsr
	str	w3, [x2, #FP_FPSR]
	mrs	x3, fpcr
	str	w3, [x2, #FP_FPCR]

    /*restore host registers*/ 
    ldp	x18, x19, [sp, #FR_X18_X19]
	ldp	x20, x21, [sp, #FR_X20_X21]
	ldp	x22, x23, [sp, #FR_X22_X23]
	ldp	x24, x25, [sp, #FR_X24_X25]
	ldp	x26, x27, [sp, #FR_X26_X27]
	ldp	x28, x29, [sp, #FR_X28_X29]
	ldp	x30, x9,  [sp, #FR_LR_SPEL0]
	msr	sp_el0, x9
	add	sp, sp, #FRAME_SIZE
	ret

.balign 2048
relm_hyp_vectors:

	.balign 128
curr_el_sp0_sync:
	b	curr_el_sp0_sync

	.balign 128
curr_el_sp0_irq:
	b	curr_el_sp0_irq

	.balign 128
curr_el_sp0_fiq:
	b	curr_el_sp0_fiq

	.balign 128
curr_el_sp0_serror:
	b	curr_el_sp0_serror

	.balign 128
curr_el_spx_sync:
	b	curr_el_spx_sync

	.balign 128
curr_el_spx_irq:
	b	curr_el_spx_irq

	.balign 128
curr_el_spx_fiq:
	b	curr_el_spx_fiq

	.balign 128
curr_el_spx_serror:
	b	curr_el_spx_serror

	.balign 128
lower_el_aarch64_sync:
	stp	x0, x1, [sp, #FR_GX0_GX1]
	ldr	x1, [sp, #FR_CTXPTR]
	mov	x0, #EXIT_SYNC
	b	relm_arm64_guest_exit_common

	.balign 128
lower_el_aarch64_irq:
	stp	x0, x1, [sp, #FR_GX0_GX1]
	ldr	x1, [sp, #FR_CTXPTR]
	mov	x0, #EXIT_IRQ
	b	relm_arm64_guest_exit_common

	.balign 128
lower_el_aarch64_fiq:
	stp	x0, x1, [sp, #FR_GX0_GX1]
	ldr	x1, [sp, #FR_CTXPTR]
	mov	x0, #EXIT_FIQ
	b	relm_arm64_guest_exit_common

	.balign 128
lower_el_aarch64_serror:
	stp	x0, x1, [sp, #FR_GX0_GX1]
	ldr	x1, [sp, #FR_CTXPTR]
	mov	x0, #EXIT_SERROR
	b	relm_arm64_guest_exit_common

	.balign 128
lower_el_aarch32_sync:
	b	lower_el_aarch32_sync
	.balign 128
lower_el_aarch32_irq:
	b	lower_el_aarch32_irq
	.balign 128
lower_el_aarch32_fiq:
	b	lower_el_aarch32_fiq
	.balign 128
lower_el_aarch32_serror:
	b	lower_el_aarch32_serror
	.balign 128











