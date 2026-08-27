/*
 * src/arch/x86/svm/insn_dump.c — AMD SVM backend for the generic-layer
 * "live disassembler" debug infra (include/relm/debug/insn_dump.h).*/ 

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <vmcb.h>
#include <mmu.h>
#include <include/debug/insn_dump.h>
#include <relm/vcpu.h>
#include <Zydis/Zydis.h>


#ifndef X86_CR0_PE
#define X86_CR0_PE      (1UL << 0)
#endif

#define INSN_DUMP_MAX_BYTES     ZYDIS_MAX_INSTRUCTION_LENGTH   /* = 15 */
#define INSN_DUMP_TEXT_MAX      160

static void insn_dump_pick_mode(struct vcpu *vcpu, ZydisMachineMode *mode,
                                ZydisStackWidth *stack_width)
{
    struct vmcb_save_area *save = &vcpu->arch.vmcb->save;

    bool long_mode_active = (save->efer & EFER_LMA) != 0;
    bool cs_is_64bit      = (save->cs.attrib & SVM_SEG_ATTRIB_L) != 0;
    bool cs_is_default32  = (save->cs.attrib & SVM_SEG_ATTRIB_DB) != 0;
    bool protected_mode   = (save->cr0 & X86_CR0_PE) != 0;

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
    default:  return "ptr";           
    }
}


static void insn_dump_format_operand(char *buf, size_t buf_size,
                                     const ZydisDecodedOperand *op)
{
    switch (op->type) {

    /*  REGISTER operand: "eax", "r15", "xmm0",  */
    case ZYDIS_OPERAND_TYPE_REGISTER:
        scnprintf(buf, buf_size, "%s",
                 ZydisRegisterGetString(op->reg.value));
        break;

    /* IMMEDIATE operand: "0x1", "0xffffffffffffffff",*/
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        scnprintf(buf, buf_size, "0x%llx",
                 op->imm.is_signed
                     ? (unsigned long long)op->imm.value.s
                     : (unsigned long long)op->imm.value.u);
        break;

    /* MEMORY operand: "dword ptr [rax+rbx*4+0x10]",
     *      "qword ptr gs:[0x0]", "byte ptr [rip+0x2000]", ...        */
    case ZYDIS_OPERAND_TYPE_MEMORY: {
        char inner[96];
        size_t pos = 0;
        bool wrote_term = false;

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

            if (!wrote_term)
                pos += scnprintf(inner + pos, sizeof(inner) - pos, "0x0");
        }

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

    /* ---- Far-pointer operand (JMP/CALL FAR, LDS/LES-style). -- */
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

    insn_dump_pick_mode(vcpu, &mode, &stack_width);

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

