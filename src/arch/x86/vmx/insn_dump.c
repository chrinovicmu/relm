#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <vmx.h>
#include <vmx_ops.h>
#include <mmu.h>
#include <include/debug/insn_dump.h>
#include <relm/vcpu.h>

#include <Zydis/Zydis.h>

#define CS_AR_BYTES_L   (1UL << 13)   /* 1 = 64-bit code segment (long mode) */
#define CS_AR_BYTES_D   (1UL << 14)   /* 1 = default 32-bit operands/addr    */
#ifndef X86_CR0_PE
#define X86_CR0_PE      (1UL << 0)
#endif

#define INSN_DUMP_MAX_BYTES     ZYDIS_MAX_INSTRUCTION_LENGTH   /* = 15 */
#define INSN_DUMP_TEXT_MAX      160

/*
 * insn_dump_pick_mode() — read live GUEST_CR0/CR4/EFER/CS-access-rights out
 * of the current VMCS and translate them into the (ZydisMachineMode,
 * ZydisStackWidth) pair Zydis needs to decode correctly. This is the
 * VMX-specific half of step [1]/[2] in insn_dump.h's pipeline diagram —
 * the generic layer never sees any of this, it only calls
 * relm_arch_dump_guest_insn() (Section 6 below) and gets back text.
 *
 * The decision tree mirrors the x86 architectural definition of "what mode
 * is the CPU in" (SDM Vol 3A, chapter 2 "System Architecture Overview" and
 * chapter 9 "Processor Management and Initialization"):
 *
 *   EFER.LMA=1 (long mode is ACTIVE, i.e. paging is also on)
 *     CS.L=1  -> 64-bit mode                  (LONG_64,        stack 64)
 *     CS.L=0, CS.D=1 -> 32-bit compatibility   (LONG_COMPAT_32, stack 32)
 *     CS.L=0, CS.D=0 -> 16-bit compatibility   (LONG_COMPAT_16, stack 16)
 *   EFER.LMA=0
 *     CR0.PE=1 (protected mode, not long)
 *       CS.D=1 -> 32-bit protected             (LEGACY_32,      stack 32)
 *       CS.D=0 -> 16-bit protected              (LEGACY_16,      stack 16)
 *     CR0.PE=0 (real mode — this is where SeaBIOS starts)
 *       -> 16-bit real mode                     (REAL_16,        stack 16)
 *
 * CS.L and CS.D can never both be 1 at once for a legal descriptor (SDM:
 * setting L=1 with D=1 is reserved/undefined), so the two checks above
 * never race each other.
 */
static void insn_dump_pick_mode(ZydisMachineMode *mode,
                                ZydisStackWidth *stack_width)
{
    uint64_t cr0    = __vmread(GUEST_CR0);
    uint64_t efer   = __vmread(GUEST_IA32_EFER);
    uint64_t cs_ar  = __vmread(GUEST_CS_AR_BYTES);

    bool long_mode_active = (efer & EFER_LMA) != 0;
    bool cs_is_64bit      = (cs_ar & CS_AR_BYTES_L) != 0;
    bool cs_is_default32  = (cs_ar & CS_AR_BYTES_D) != 0;
    bool protected_mode   = (cr0 & X86_CR0_PE) != 0;

    if (long_mode_active) {
        if (cs_is_64bit) {
            *mode = ZYDIS_MACHINE_MODE_LONG_64;
            *stack_width = ZYDIS_STACK_WIDTH_64;
        } else if (cs_is_default32) {
            *mode = ZYDIS_MACHINE_MODE_LONG_COMPAT_32;
            *stack_width = ZYDIS_STACK_WIDTH_32;
        } else {
            *mode = ZYDIS_MACHINE_MODE_LONG_COMPAT_16;
            *stack_width = ZYDIS_STACK_WIDTH_16;
        }
        return;
    }

    if (protected_mode) {
        if (cs_is_default32) {
            *mode = ZYDIS_MACHINE_MODE_LEGACY_32;
            *stack_width = ZYDIS_STACK_WIDTH_32;
        } else {
            *mode = ZYDIS_MACHINE_MODE_LEGACY_16;
            *stack_width = ZYDIS_STACK_WIDTH_16;
        }
        return;
    }

    /* Neither long mode nor protected mode is active -> real mode. This
     * is the mode the guest is in from the very first instruction we
     * hand it (SeaBIOS reset vector), before it ever touches CR0. */
    *mode = ZYDIS_MACHINE_MODE_REAL_16;
    *stack_width = ZYDIS_STACK_WIDTH_16;
}

static const char *insn_dump_size_keyword(uint16_t size_bits)
{
    switch (size_bits) {
    case 8:   return "byte ptr";
    case 16:  return "word ptr";
    case 32:  return "dword ptr";
    case 64:  return "qword ptr";
    case 80:  return "tbyte ptr";     /* x87 80-bit extended precision */
    case 128: return "xmmword ptr";
    case 256: return "ymmword ptr";
    case 512: return "zmmword ptr";
    default:  return "ptr";           /* unusual/unhandled width; still
                                        * prints something rather than
                                        * silently dropping the operand. */
    }
}

static void insn_dump_format_operand(char *buf, size_t buf_size,
                                     const ZydisDecodedOperand *op)
{
    switch (op->type) {

    /*  REGISTER operand: "eax", "r15", "xmm0", */
    case ZYDIS_OPERAND_TYPE_REGISTER:
        /* ZydisRegisterGetString() already returns the exact lowercase
         * name a human disassembler expects (Zydis's own internal
         * register name table — Register.c, which IS compiled into this
         * build, see the ZYDIS_C_NAMES list in the top-level Makefile). */
        scnprintf(buf, buf_size, "%s",
                 ZydisRegisterGetString(op->reg.value));
        break;

    /* ---- IMMEDIATE operand: "0x1", "0xffffffffffffffff", ... ------ */
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        /*
         * Print immediates as unsigned hex unconditionally. This mirrors
         * what objdump/gdb do for the common case (e.g. "mov eax, 0x1")
         * and sidesteps having to decide, per width, how to sign-extend
         * a *signed* immediate into readable decimal — hex is unambiguous
         * regardless of is_signed, and is what a human debugging raw
         * guest bytes actually wants to compare against a hex dump.
         */
        scnprintf(buf, buf_size, "0x%llx",
                 op->imm.is_signed
                     ? (unsigned long long)op->imm.value.s
                     : (unsigned long long)op->imm.value.u);
        break;

    /* -MEMORY operand: "dword ptr [rax+rbx*4+0x10]", 
     *      "qword ptr gs:[0x0]", "byte ptr [rip+0x2000]", ...        */
    case ZYDIS_OPERAND_TYPE_MEMORY: {
        /* Small local cursor so we can build up "[base+index*scale+disp]"
         * piece by piece without a chain of separate scnprintf() calls
         * each needing their own bounds-checked offset math. */
        char inner[96];
        size_t pos = 0;
        bool wrote_term = false; /* has anything gone inside [...] yet? */

        /*
         * RIP-relative addressing (base == RIP) is extremely common in
         * modern x86-64 code (PIE binaries, the Linux kernel's own
         * position-independent decompressor stub — exactly the code this
         * infra was built to debug) and reads awkwardly if treated as a
         * generic base register, so it gets its own explicit branch: the
         * disp value IS the resolved offset already, print it directly.
         */
        if (op->mem.base == ZYDIS_REGISTER_RIP) {
            pos += scnprintf(inner + pos, sizeof(inner) - pos,
                             "rip+0x%llx",
                             (unsigned long long)op->mem.disp.value);
            wrote_term = true;
        } else {
            if (op->mem.base != ZYDIS_REGISTER_NONE) {
                pos += scnprintf(inner + pos, sizeof(inner) - pos, "%s",
                                 ZydisRegisterGetString(op->mem.base));
                wrote_term = true;
            }

            if (op->mem.index != ZYDIS_REGISTER_NONE) {
                pos += scnprintf(inner + pos, sizeof(inner) - pos,
                                 "%s%s*%u",
                                 wrote_term ? "+" : "",
                                 ZydisRegisterGetString(op->mem.index),
                                 (unsigned int)op->mem.scale);
                wrote_term = true;
            }

            /* Displacement: only printed if Zydis says one is actually
             * encoded (has_displacement) — a base-only operand like
             * "[rax]" must not grow a spurious "+0x0". */
            if (op->mem.disp.has_displacement && op->mem.disp.value != 0) {
                long long disp = op->mem.disp.value;

                if (disp < 0)
                    pos += scnprintf(inner + pos, sizeof(inner) - pos,
                                     "-0x%llx", (unsigned long long)-disp);
                else
                    pos += scnprintf(inner + pos, sizeof(inner) - pos,
                                     "%s0x%llx", wrote_term ? "+" : "",
                                     (unsigned long long)disp);
                wrote_term = true;
            }

            /* Absolute-addressing edge case: no base, no index, disp==0
             * is legal (e.g. a MOV with a bare 32-bit moffs operand) —
             * make sure "[]" never prints empty. */
            if (!wrote_term)
                pos += scnprintf(inner + pos, sizeof(inner) - pos, "0x0");
        }

        /*
         * Segment override prefix: only worth printing for the two
         * segments x86-64 code actually uses non-default (FS/GS — Linux
         * per-CPU data, TLS, stack canaries all go through one of these).
         * CS/DS/ES/SS overrides are architectural leftovers from 16/32-bit
         * mode that Zydis still reports but that clutter a 64-bit dump
         * with no informational value, so they're deliberately skipped.
         */
        if (op->mem.segment == ZYDIS_REGISTER_FS ||
            op->mem.segment == ZYDIS_REGISTER_GS) {
            scnprintf(buf, buf_size, "%s %s:[%s]",
                     insn_dump_size_keyword(op->size),
                     ZydisRegisterGetString(op->mem.segment), inner);
        } else {
            scnprintf(buf, buf_size, "%s [%s]",
                     insn_dump_size_keyword(op->size), inner);
        }
        break;
    }

    /* Far-pointer operand (JMP/CALL FAR, LDS/LES-style). -- */
    case ZYDIS_OPERAND_TYPE_POINTER:
        scnprintf(buf, buf_size, "0x%x:0x%x",
                 (unsigned int)op->ptr.segment, (unsigned int)op->ptr.offset);
        break;

    default:
        scnprintf(buf, buf_size, "<op-type-%d>", (int)op->type);
        break;
    }
}

static void insn_dump_format_instruction(char *buf, size_t buf_size,
                                         const ZydisDecodedInstruction *insn,
                                         const ZydisDecodedOperand *operands)
{
    size_t pos = 0;
    unsigned int i;

    if (insn->attributes & ZYDIS_ATTRIB_HAS_LOCK)
        pos += scnprintf(buf + pos, buf_size - pos, "lock ");
    if (insn->attributes & ZYDIS_ATTRIB_HAS_REP)
        pos += scnprintf(buf + pos, buf_size - pos, "rep ");
    if (insn->attributes & ZYDIS_ATTRIB_HAS_REPE)
        pos += scnprintf(buf + pos, buf_size - pos, "repe ");
    if (insn->attributes & ZYDIS_ATTRIB_HAS_REPNE)
        pos += scnprintf(buf + pos, buf_size - pos, "repne ");

    pos += scnprintf(buf + pos, buf_size - pos, "%s",
                     ZydisMnemonicGetString(insn->mnemonic));

    for (i = 0; i < insn->operand_count_visible; i++) {
        char operand_text[96];

        insn_dump_format_operand(operand_text, sizeof(operand_text),
                                 &operands[i]);

        pos += scnprintf(buf + pos, buf_size - pos, "%s%s",
                         (i == 0) ? " " : ", ", operand_text);
    }

    (void)pos; 
}

int relm_arch_dump_guest_insn(struct vcpu *vcpu, uint64_t gva,
                              unsigned int *out_length)
{
    uint8_t raw[INSN_DUMP_MAX_BYTES];

    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZydisDecoder decoder;
    ZydisMachineMode mode;
    ZydisStackWidth stack_width;
    ZyanStatus status;

    char text[INSN_DUMP_TEXT_MAX];

    int ret;

    if (!vcpu)
        return -EINVAL;

    ret = relm_mmu_copy_from_guest_virt(vcpu, gva, raw, sizeof(raw));
    if (ret < 0) {
        pr_err("relm decoder: [VPID=%u] 0x%llx: failed to fetch guest "
              "instruction bytes (%d)\n",
              vcpu->vpid, (unsigned long long)gva, ret);
        return ret;
    }

    insn_dump_pick_mode(&mode, &stack_width);

    status = ZydisDecoderInit(&decoder, mode, stack_width);
    if (!ZYAN_SUCCESS(status)) {
        pr_err("relm decoder: [VPID=%u] 0x%llx: ZydisDecoderInit failed "
              "(mode=%d, status=0x%x)\n",
              vcpu->vpid, (unsigned long long)gva, (int)mode,
              (unsigned int)status);
        return -EINVAL;
    }

    status = ZydisDecoderDecodeFull(&decoder, raw, sizeof(raw),
                                    &instruction, operands);
    if (!ZYAN_SUCCESS(status)) {
        pr_err("relm decoder: [VPID=%u] 0x%llx: Zydis decode failed "
              "(mode=%d, status=0x%x) raw bytes: "
              "%02x %02x %02x %02x %02x %02x %02x %02x\n",
              vcpu->vpid, (unsigned long long)gva, (int)mode,
              (unsigned int)status,
              raw[0], raw[1], raw[2], raw[3],
              raw[4], raw[5], raw[6], raw[7]);
        return -EINVAL;
    }

    insn_dump_format_instruction(text, sizeof(text), &instruction, operands);

    pr_info("relm decoder: [VPID=%u] 0x%llx: %s\n",
           vcpu->vpid, (unsigned long long)gva, text);

    *out_length = instruction.length;

    return 0;
}

