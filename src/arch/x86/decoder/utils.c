#include <linux/kernel.h>
#include <include/relm/decoder.h>
#include "include/arch/x86/decoder/insn.h" 

static inline int get_reg_index(struct insn *insn) {
    int reg = X86_MODRM_REG(insn->modrm.value);
    /* If REX prefix is present and REX.R bit is set, it maps to extended registers (r8-r15) */
    if (insn->rex_prefix.nbytes && (insn->rex_prefix.value & 0x04)) {
        reg += 8;
    }
    return reg;
}

int relm_decode_x86(const uint8_t *insn_buf, int len, struct relm_decoded_insn *out)
{
    struct insn insn;
    int ret;

    ret = insn_decode(&insn, insn_buf, len, INSN_MODE_64);
    if (ret < 0) {
        return ret;
    }

    uint8_t op = insn.opcode.bytes[0];

    if (insn.rex_prefix.nbytes && (insn.rex_prefix.value & 0x08)) {
        out->op_size = 8; /* REX.W bit set = 64-bit operation */
    } else if (insn.prefixes.nbytes && (insn.prefixes.bytes[0] == 0x66)) {
        out->op_size = 2; /* 0x66 Prefix = 16-bit operation */
    } else {
        out->op_size = 4; /* Default x86 Protected/Long Mode size = 32-bit (Standard for APIC/VirtIO) */
    }

    switch (op) {
        
        /* Case A: MOV Reg -> Mem (Guest Write) 
         * Opcode 0x89: mov r/m32, r32  or  mov r/m64, r64 */
        case 0x89:
            out->is_write = true;
            out->src_reg = get_reg_index(&insn); 
            out->is_immediate = false;
            break;

        /* Case B: MOV Mem -> Reg (Guest Read)
         * Opcode 0x8B: mov r32, r/m32  or  mov r64, r/m64 */
        case 0x8B:
            out->is_write = false;
            out->dst_reg = get_reg_index(&insn);
            out->is_immediate = false;
            break;

        /* Case C: MOV Immediate -> Mem (Guest Write)
         * Opcode 0xC7: mov r/m32, imm32 */
        case 0xC7:
            out->is_write = true;
            out->is_immediate = true;
            if (insn.immediate.nbytes > 0) {
                out->immediate = insn.immediate.value;
            }
            break;

        default:
            pr_warn("RELM: x86 decoder encountered unhandled hardware trap opcode: 0x%02x\n", op);
            return -ENOSYS;
    }

    return insn.length;
}

