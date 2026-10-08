#ifndef RELM_ARCH_ARM64_VCPU_ARCH_H
#define RELM_ARCH_ARM64_VCPU_ARCH_H

#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>

#define RELM_ARM64_NR_GPRS          31U
#define RELM_ARM64_NR_FP_REGS       32U

struct relm_vgic_vcpu;
struct vcpu_arch_ops;

enum relm_arm64_exit_kind {
    /* No guest exit has been captured, or the previous exit was consumed. */
    RELM_ARM64_EXIT_NONE = 0,
    /* A synchronous trap: for example HVC, trapped MRS/MSR, or stage-2 abort. */
    RELM_ARM64_EXIT_SYNC,
    /* A physical IRQ routed to EL2; not automatically an IRQ for this guest. */
    RELM_ARM64_EXIT_IRQ,
    /* A physical FIQ routed to EL2, requiring its own routing/handling policy. */
    RELM_ARM64_EXIT_FIQ,
    /* An asynchronous system error; syndrome validity needs separate checks. */
    RELM_ARM64_EXIT_SERROR,
}
struct guest_regs {
    u64 x0, x1, x2, x3, x4, x5, x6, x7;
    u64 x8, x9, x10, x11, x12, x13, x14, x15;
    u64 x16, x17, x18, x19, x20, x21, x22, x23;
    u64 x24, x25, x26, x27, x28, x29, x30;
    u64 sp_el0;
    u64 sp_el1;
    u64 pc; 
    u64 pstate;
    u64 esr_el2; 
    u64 far_el2; 
    u64 hpfar_el2; 
}__attribute__((aligned(16))); 


struct relm_arch64_el1_state{
    u64 sctlr_el1;
    u64 actlr_el1; 
    u64 cpacr_el1; 
    u64 ttbr0_el1; 
    u64 ttbr1_el1; 
    u64 tcr_el1; 
    u64 mair_el1;
    u64 amair_el1;
    u64 vbar_el1; 
    u64 elr_el1;; 
    u64 spsr_el1;
    u64 esr_el1;
    u64 far_el1;
    u64 afsr0_el1;
    u64 par_el1;
    u64 contextidr_el1;
    u64 tpidr_el0;
    u64 tpidrro_el0;
    u64 tpidr_el1;
    u64 cntkctl_el1;
    u64 csselr_el1;
    u64 mdscr_el1;
}; 

struct relm_arm64_timer_state {
    u64 cntv_cval;
    u32 cntv_ctl;
    u32 reserved_virtual;
    u64 cntp_cval;
    /* CNTP_CTL_EL0: physical-timer ENABLE/IMASK; ISTATUS is likewise derived. */
    u32 cntp_ctl;
    u32 reserved_physical;
};

struct relm_arm64_fp_state {
    /*
     * V0..V31 / Q0..Q31, two u64s each: qN_lo is bits [63:0] (also Dn),
     * qN_hi is bits [127:64]. Raw register bits, not C float values.
     * Linux task/signal switches and RELM guest/host switches must keep
     * every bit; an EL2 exit may not drop a register or its upper half.
     * lo/hi are numeric bit positions. Save/restore assembly must match
     * that for its endianness; this is not a migration image.
     */
    u64 q0_lo, q0_hi, q1_lo, q1_hi, q2_lo, q2_hi, q3_lo, q3_hi;
    u64 q4_lo, q4_hi, q5_lo, q5_hi, q6_lo, q6_hi, q7_lo, q7_hi;
    u64 q8_lo, q8_hi, q9_lo, q9_hi, q10_lo, q10_hi, q11_lo, q11_hi;
    u64 q12_lo, q12_hi, q13_lo, q13_hi, q14_lo, q14_hi, q15_lo, q15_hi;
    u64 q16_lo, q16_hi, q17_lo, q17_hi, q18_lo, q18_hi, q19_lo, q19_hi;
    u64 q20_lo, q20_hi, q21_lo, q21_hi, q22_lo, q22_hi, q23_lo, q23_hi;
    u64 q24_lo, q24_hi, q25_lo, q25_hi, q26_lo, q26_hi, q27_lo, q27_hi;
    u64 q28_lo, q28_hi, q29_lo, q29_hi, q30_lo, q30_hi, q31_lo, q31_hi;

    /* Cumulative FP exception flags and the SIMD saturation QC bit. */
    u32 fpsr;
    /* Rounding mode, exception traps, default-NaN and flush-to-zero. */
    u32 fpcr;
    u64 reserved;
} __attribute__((aligned(16)));

struct relm_arm64_guest_controls{
    u64 hcr_el2; 
    u64 cptr_el2; 
    u64 mdcr_el2; 
    u64 cnthctl_el2; 
}; 

struct vcpu_arch{
    struct guest_regs regs; 
    struct relm_arch64_el1_state el1; 
    struct relm_arm64_timer_state; 
    struct relm_arm64_guest_controls; 
    enum relm_arm64_exit_kind exit_kind; 
    bool initialized; 
}

static inline u64
relm_arm64_read_xreg(const struct guest_regs *regs, unsigned int reg)
{
    switch (reg) {
    case 0:  return regs->x0;
    case 1:  return regs->x1;
    case 2:  return regs->x2;
    case 3:  return regs->x3;
    case 4:  return regs->x4;
    case 5:  return regs->x5;
    case 6:  return regs->x6;
    case 7:  return regs->x7;
    case 8:  return regs->x8;
    case 9:  return regs->x9;
    case 10: return regs->x10;
    case 11: return regs->x11;
    case 12: return regs->x12;
    case 13: return regs->x13;
    case 14: return regs->x14;
    case 15: return regs->x15;
    case 16: return regs->x16;
    case 17: return regs->x17;
    case 18: return regs->x18;
    case 19: return regs->x19;
    case 20: return regs->x20;
    case 21: return regs->x21;
    case 22: return regs->x22;
    case 23: return regs->x23;
    case 24: return regs->x24;
    case 25: return regs->x25;
    case 26: return regs->x26;
    case 27: return regs->x27;
    case 28: return regs->x28;
    case 29: return regs->x29;
    case 30: return regs->x30;
    default: return 0; /* XZR or an invalid register number. */
    }
}

static inline void
relm_arm64_write_xreg(struct guest_regs *regs, unsigned int reg,
                     u64 value)
{
    switch (reg) {
    case 0:  regs->x0 = value;  break;
    case 1:  regs->x1 = value;  break;
    case 2:  regs->x2 = value;  break;
    case 3:  regs->x3 = value;  break;
    case 4:  regs->x4 = value;  break;
    case 5:  regs->x5 = value;  break;
    case 6:  regs->x6 = value;  break;
    case 7:  regs->x7 = value;  break;
    case 8:  regs->x8 = value;  break;
    case 9:  regs->x9 = value;  break;
    case 10: regs->x10 = value; break;
    case 11: regs->x11 = value; break;
    case 12: regs->x12 = value; break;
    case 13: regs->x13 = value; break;
    case 14: regs->x14 = value; break;
    case 15: regs->x15 = value; break;
    case 16: regs->x16 = value; break;
    case 17: regs->x17 = value; break;
    case 18: regs->x18 = value; break;
    case 19: regs->x19 = value; break;
    case 20: regs->x20 = value; break;
    case 21: regs->x21 = value; break;
    case 22: regs->x22 = value; break;
    case 23: regs->x23 = value; break;
    case 24: regs->x24 = value; break;
    case 25: regs->x25 = value; break;
    case 26: regs->x26 = value; break;
    case 27: regs->x27 = value; break;
    case 28: regs->x28 = value; break;
    case 29: regs->x29 = value; break;
    case 30: regs->x30 = value; break;
    default: break; /* XZR discards writes; invalid register numbers do too. */
    }
}

/* An emulated 32-bit result replaces the entire xN with zero-extended wN. */
static inline void
relm_arm64_write_wreg(struct guest_regs *regs, unsigned int reg,
                     u32 value)
{
    relm_arm64_write_xreg(regs, reg, (u64)value);
}

/*
 * Compile-time ABI checks for the blocks consumed by assembly. These describe
 * RELM's chosen software layout, not an Arm-mandated structure or guest ABI.
 * Keep a generated asm-offsets source as the consumer when assembly is added.
 */
static_assert(offsetof(struct guest_regs, x0) == 0);
static_assert(offsetof(struct guest_regs, x1) == 8);
static_assert(offsetof(struct guest_regs, x2) == 16);
static_assert(offsetof(struct guest_regs, x3) == 24);
static_assert(offsetof(struct guest_regs, x4) == 32);
static_assert(offsetof(struct guest_regs, x5) == 40);
static_assert(offsetof(struct guest_regs, x6) == 48);
static_assert(offsetof(struct guest_regs, x7) == 56);
static_assert(offsetof(struct guest_regs, x8) == 64);
static_assert(offsetof(struct guest_regs, x9) == 72);
static_assert(offsetof(struct guest_regs, x10) == 80);
static_assert(offsetof(struct guest_regs, x11) == 88);
static_assert(offsetof(struct guest_regs, x12) == 96);
static_assert(offsetof(struct guest_regs, x13) == 104);
static_assert(offsetof(struct guest_regs, x14) == 112);
static_assert(offsetof(struct guest_regs, x15) == 120);
static_assert(offsetof(struct guest_regs, x16) == 128);
static_assert(offsetof(struct guest_regs, x17) == 136);
static_assert(offsetof(struct guest_regs, x18) == 144);
static_assert(offsetof(struct guest_regs, x19) == 152);
static_assert(offsetof(struct guest_regs, x20) == 160);
static_assert(offsetof(struct guest_regs, x21) == 168);
static_assert(offsetof(struct guest_regs, x22) == 176);
static_assert(offsetof(struct guest_regs, x23) == 184);
static_assert(offsetof(struct guest_regs, x24) == 192);
static_assert(offsetof(struct guest_regs, x25) == 200);
static_assert(offsetof(struct guest_regs, x26) == 208);
static_assert(offsetof(struct guest_regs, x27) == 216);
static_assert(offsetof(struct guest_regs, x28) == 224);
static_assert(offsetof(struct guest_regs, x29) == 232);
static_assert(offsetof(struct guest_regs, x30) == 240);
static_assert(offsetof(struct guest_regs, sp_el0) == 248);
static_assert(offsetof(struct guest_regs, sp_el1) == 256);
static_assert(offsetof(struct guest_regs, pc) == 264);
static_assert(offsetof(struct guest_regs, pstate) == 272);
static_assert(offsetof(struct guest_regs, esr_el2) == 280);
static_assert(offsetof(struct guest_regs, far_el2) == 288);
static_assert(offsetof(struct guest_regs, hpfar_el2) == 296);
static_assert(sizeof(struct guest_regs) == 304);
static_assert(__alignof__(struct guest_regs) == 16);

static_assert(offsetof(struct relm_arm64_fp_state, q0_lo) == 0);
static_assert(offsetof(struct relm_arm64_fp_state, q0_hi) == 8);
static_assert(offsetof(struct relm_arm64_fp_state, q1_lo) == 16);
static_assert(offsetof(struct relm_arm64_fp_state, q1_hi) == 24);
static_assert(offsetof(struct relm_arm64_fp_state, q2_lo) == 32);
static_assert(offsetof(struct relm_arm64_fp_state, q2_hi) == 40);
static_assert(offsetof(struct relm_arm64_fp_state, q3_lo) == 48);
static_assert(offsetof(struct relm_arm64_fp_state, q3_hi) == 56);
static_assert(offsetof(struct relm_arm64_fp_state, q4_lo) == 64);
static_assert(offsetof(struct relm_arm64_fp_state, q4_hi) == 72);
static_assert(offsetof(struct relm_arm64_fp_state, q5_lo) == 80);
static_assert(offsetof(struct relm_arm64_fp_state, q5_hi) == 88);
static_assert(offsetof(struct relm_arm64_fp_state, q6_lo) == 96);
static_assert(offsetof(struct relm_arm64_fp_state, q6_hi) == 104);
static_assert(offsetof(struct relm_arm64_fp_state, q7_lo) == 112);
static_assert(offsetof(struct relm_arm64_fp_state, q7_hi) == 120);
static_assert(offsetof(struct relm_arm64_fp_state, q8_lo) == 128);
static_assert(offsetof(struct relm_arm64_fp_state, q8_hi) == 136);
static_assert(offsetof(struct relm_arm64_fp_state, q9_lo) == 144);
static_assert(offsetof(struct relm_arm64_fp_state, q9_hi) == 152);
static_assert(offsetof(struct relm_arm64_fp_state, q10_lo) == 160);
static_assert(offsetof(struct relm_arm64_fp_state, q10_hi) == 168);
static_assert(offsetof(struct relm_arm64_fp_state, q11_lo) == 176);
static_assert(offsetof(struct relm_arm64_fp_state, q11_hi) == 184);
static_assert(offsetof(struct relm_arm64_fp_state, q12_lo) == 192);
static_assert(offsetof(struct relm_arm64_fp_state, q12_hi) == 200);
static_assert(offsetof(struct relm_arm64_fp_state, q13_lo) == 208);
static_assert(offsetof(struct relm_arm64_fp_state, q13_hi) == 216);
static_assert(offsetof(struct relm_arm64_fp_state, q14_lo) == 224);
static_assert(offsetof(struct relm_arm64_fp_state, q14_hi) == 232);
static_assert(offsetof(struct relm_arm64_fp_state, q15_lo) == 240);
static_assert(offsetof(struct relm_arm64_fp_state, q15_hi) == 248);
static_assert(offsetof(struct relm_arm64_fp_state, q16_lo) == 256);
static_assert(offsetof(struct relm_arm64_fp_state, q16_hi) == 264);
static_assert(offsetof(struct relm_arm64_fp_state, q17_lo) == 272);
static_assert(offsetof(struct relm_arm64_fp_state, q17_hi) == 280);
static_assert(offsetof(struct relm_arm64_fp_state, q18_lo) == 288);
static_assert(offsetof(struct relm_arm64_fp_state, q18_hi) == 296);
static_assert(offsetof(struct relm_arm64_fp_state, q19_lo) == 304);
static_assert(offsetof(struct relm_arm64_fp_state, q19_hi) == 312);
static_assert(offsetof(struct relm_arm64_fp_state, q20_lo) == 320);
static_assert(offsetof(struct relm_arm64_fp_state, q20_hi) == 328);
static_assert(offsetof(struct relm_arm64_fp_state, q21_lo) == 336);
static_assert(offsetof(struct relm_arm64_fp_state, q21_hi) == 344);
static_assert(offsetof(struct relm_arm64_fp_state, q22_lo) == 352);
static_assert(offsetof(struct relm_arm64_fp_state, q22_hi) == 360);
static_assert(offsetof(struct relm_arm64_fp_state, q23_lo) == 368);
static_assert(offsetof(struct relm_arm64_fp_state, q23_hi) == 376);
static_assert(offsetof(struct relm_arm64_fp_state, q24_lo) == 384);
static_assert(offsetof(struct relm_arm64_fp_state, q24_hi) == 392);
static_assert(offsetof(struct relm_arm64_fp_state, q25_lo) == 400);
static_assert(offsetof(struct relm_arm64_fp_state, q25_hi) == 408);
static_assert(offsetof(struct relm_arm64_fp_state, q26_lo) == 416);
static_assert(offsetof(struct relm_arm64_fp_state, q26_hi) == 424);
static_assert(offsetof(struct relm_arm64_fp_state, q27_lo) == 432);
static_assert(offsetof(struct relm_arm64_fp_state, q27_hi) == 440);
static_assert(offsetof(struct relm_arm64_fp_state, q28_lo) == 448);
static_assert(offsetof(struct relm_arm64_fp_state, q28_hi) == 456);
static_assert(offsetof(struct relm_arm64_fp_state, q29_lo) == 464);
static_assert(offsetof(struct relm_arm64_fp_state, q29_hi) == 472);
static_assert(offsetof(struct relm_arm64_fp_state, q30_lo) == 480);
static_assert(offsetof(struct relm_arm64_fp_state, q30_hi) == 488);
static_assert(offsetof(struct relm_arm64_fp_state, q31_lo) == 496);
static_assert(offsetof(struct relm_arm64_fp_state, q31_hi) == 504);
static_assert(offsetof(struct relm_arm64_fp_state, fpsr) == 512);
static_assert(offsetof(struct relm_arm64_fp_state, fpcr) == 516);
static_assert(offsetof(struct relm_arm64_fp_state, reserved) == 520);
static_assert(sizeof(struct relm_arm64_fp_state) == 528);
static_assert(__alignof__(struct relm_arm64_fp_state) == 16);
static_assert(sizeof(struct relm_arm64_timer_state) == 32);
static_assert(offsetof(struct vcpu_arch, regs) == 0);
static_assert(offsetof(struct vcpu_arch, fp) % 16 == 0);
static_assert(__alignof__(struct vcpu_arch) == 16);

/* Supplied by the ARM64 backend when its allocation/run/exit code is added. */
extern const struct vcpu_arch_ops arm64_vcpu_ops;

#endif /* RELM_ARCH_ARM64_VCPU_ARCH_H */
