/*
 * src/arch/x86/decoder/utils.c — Zydis-backed x86 instruction adapteri 
 * */ 

#include <linux/errno.h>
#include <linux/string.h>
#include <relm/decoder.h>
#include <Zydis/Zydis.h>


static int relm_zydis_decode_gpr(const ZydisDecodedOperand *operand,
                                 struct relm_decoded_insn *out)
{
    ZydisRegisterClass reg_class;
    ZydisRegister enclosing;

    if (operand->type != ZYDIS_OPERAND_TYPE_REGISTER)
        return -EOPNOTSUPP;

    reg_class = ZydisRegisterGetClass(operand->reg.value);
    switch (reg_class) {
    case ZYDIS_REGCLASS_GPR8:
    case ZYDIS_REGCLASS_GPR16:
    case ZYDIS_REGCLASS_GPR32:
    case ZYDIS_REGCLASS_GPR64:
        break;
    default:
        /* Segment, control, vector, mask, and x87 registers are not GPRs. */
        return -EOPNOTSUPP;
    }

    if (!operand->size || (operand->size & 7))
        return -EOPNOTSUPP;

    out->gpr_width = operand->size / 8;
    if (!relm_width_mask(out->gpr_width))
        return -EOPNOTSUPP;

    enclosing = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,
                                                  operand->reg.value);
    if (enclosing < ZYDIS_REGISTER_RAX || enclosing > ZYDIS_REGISTER_R15)
        return -EOPNOTSUPP;

    out->gpr_index = (int)(enclosing - ZYDIS_REGISTER_RAX);

    /*
     * The legacy high-byte registers are the only scalar GPR operands whose
     * value does not start at bit zero of the enclosing register. A REX prefix
     * makes these registers unencodable; Zydis has already enforced that rule.
     */
    switch (operand->reg.value) {
    case ZYDIS_REGISTER_AH:
    case ZYDIS_REGISTER_CH:
    case ZYDIS_REGISTER_DH:
    case ZYDIS_REGISTER_BH:
        out->gpr_bit_offset = 8;
        break;
    default:
        out->gpr_bit_offset = 0;
        break;
    }

    return relm_decoded_has_valid_gpr(out) ? 0 : -EOPNOTSUPP;
}


static int relm_zydis_set_load_semantics(struct relm_decoded_insn *out)
{
    switch (out->gpr_width) {
    case 1:
    case 2:
        out->gpr_write_semantics = RELM_GPR_WRITE_MERGE;
        return 0;
    case 4:
        out->gpr_write_semantics = RELM_GPR_WRITE_ZERO_EXTEND;
        return 0;
    case 8:
        out->gpr_write_semantics = RELM_GPR_WRITE_REPLACE;
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}


/*
 * relm_decode_x86() — decode and validate one 64-bit guest instruction.
 *
 * Returns the positive instruction length and fills *out on success.
 * -EINVAL means invalid arguments or undecodable/truncated bytes;
 * -EOPNOTSUPP means Zydis decoded a real instruction, but RELM's MMIO
 * emulator intentionally does not implement its semantics.
 */
int relm_decode_x86(const uint8_t *insn_buf, int len,
                    struct relm_decoded_insn *out)
{
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const ZydisDecodedOperand *memory = NULL;
    const ZydisDecodedOperand *value_operand = NULL;
    ZydisDecoder decoder;
    ZyanStatus status;
    unsigned int i;
    int ret;

    if (!insn_buf || !out || len <= 0)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->gpr_index = -1;

    /*
     * RELM currently runs x86 guests in long mode, so both the machine mode
     * and architectural stack width are 64 bits.*/ 
    status = ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                              ZYDIS_STACK_WIDTH_64);
    if (!ZYAN_SUCCESS(status))
        return -EINVAL;

    status = ZydisDecoderDecodeFull(&decoder, insn_buf, (ZyanUSize)len,
                                    &instruction, operands);
    if (!ZYAN_SUCCESS(status))
        return -EINVAL;

    if (instruction.mnemonic != ZYDIS_MNEMONIC_MOV ||
        (instruction.attributes & ZYDIS_ATTRIB_HAS_LOCK))
        return -EOPNOTSUPP;

    if (instruction.operand_count_visible != 2)
        return -EOPNOTSUPP;

    for (i = 0; i < instruction.operand_count_visible; ++i) {
        const ZydisDecodedOperand *operand = &operands[i];

        if (operand->visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT)
            return -EOPNOTSUPP;

        if (operand->type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (memory)
                return -EOPNOTSUPP;
            memory = operand;
        } else {
            if (value_operand)
                return -EOPNOTSUPP;
            value_operand = operand;
        }
    }

    if (!memory || !value_operand || memory->mem.type != ZYDIS_MEMOP_TYPE_MEM)
        return -EOPNOTSUPP;

    if (!memory->size || (memory->size & 7))
        return -EOPNOTSUPP;

    out->memory_width = memory->size / 8;
    if (!relm_width_mask(out->memory_width))
        return -EOPNOTSUPP;

    if (memory->actions == ZYDIS_OPERAND_ACTION_READ) {
        out->memory_access = RELM_MEMORY_ACCESS_READ;
    } else if (memory->actions == ZYDIS_OPERAND_ACTION_WRITE) {
        out->memory_access = RELM_MEMORY_ACCESS_WRITE;
    } else {
        return -EOPNOTSUPP;
    }

    if (out->memory_access == RELM_MEMORY_ACCESS_WRITE &&
        (!(value_operand->actions & ZYDIS_OPERAND_ACTION_MASK_READ) ||
         (value_operand->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)))
        return -EOPNOTSUPP;

    if (out->memory_access == RELM_MEMORY_ACCESS_READ &&
        (!(value_operand->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) ||
         (value_operand->actions & ZYDIS_OPERAND_ACTION_MASK_READ)))
        return -EOPNOTSUPP;

    switch (value_operand->type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        out->operand_kind = RELM_OPERAND_GPR;
        ret = relm_zydis_decode_gpr(value_operand, out);
        if (ret < 0)
            return ret;

        /* Plain scalar MOV transfers equally-sized register and memory data. */
        if (out->gpr_width != out->memory_width)
            return -EOPNOTSUPP;

        if (out->memory_access == RELM_MEMORY_ACCESS_READ) {
            ret = relm_zydis_set_load_semantics(out);
            if (ret < 0)
                return ret;
        } else {
            out->gpr_write_semantics = RELM_GPR_WRITE_NONE;
        }
        break;

    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        /* An immediate can only be the source of a memory write. */
        if (out->memory_access != RELM_MEMORY_ACCESS_WRITE ||
            value_operand->imm.is_relative)
            return -EOPNOTSUPP;

        out->operand_kind = RELM_OPERAND_IMMEDIATE;
        out->immediate = value_operand->imm.is_signed
                       ? (uint64_t)value_operand->imm.value.s
                       : (uint64_t)value_operand->imm.value.u;
        break;

    default:
        return -EOPNOTSUPP;
    }

    if (!instruction.length || instruction.length > (unsigned int)len)
        return -EINVAL;

    out->length = instruction.length;
    return (int)instruction.length;
}

