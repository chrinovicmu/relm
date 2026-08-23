#ifndef RELM_DECODER_H
#define RELM_DECODER_H

#if defined(__KERNEL__)
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#endif


enum relm_memory_access {
    RELM_MEMORY_ACCESS_INVALID = 0,
    RELM_MEMORY_ACCESS_READ,
    RELM_MEMORY_ACCESS_WRITE,
};

enum relm_operand_kind {
    RELM_OPERAND_INVALID = 0,
    RELM_OPERAND_GPR,
    RELM_OPERAND_IMMEDIATE,
};

enum relm_gpr_write_semantics {
    RELM_GPR_WRITE_NONE = 0,
    RELM_GPR_WRITE_MERGE,
    RELM_GPR_WRITE_ZERO_EXTEND,
    RELM_GPR_WRITE_REPLACE,
};


struct relm_decoded_insn {
    unsigned int length;
    enum relm_memory_access memory_access;
    unsigned int memory_width;

    enum relm_operand_kind operand_kind;
    uint64_t immediate;

    int gpr_index;
    unsigned int gpr_width;
    unsigned int gpr_bit_offset;
    enum relm_gpr_write_semantics gpr_write_semantics;
};

static inline uint64_t relm_width_mask(unsigned int width)
{
    switch (width) {
    case 1: return (uint64_t)0xFFULL;
    case 2: return (uint64_t)0xFFFFULL;
    case 4: return (uint64_t)0xFFFFFFFFULL;
    case 8: return (uint64_t)~0ULL;
    default: return 0;
    }
}
static inline bool
relm_decoded_has_valid_gpr(const struct relm_decoded_insn *decoded)
{
    if (!decoded || decoded->operand_kind != RELM_OPERAND_GPR)
        return false;

    if (decoded->gpr_index < 0 || decoded->gpr_index >= 16)
        return false;

    if (!relm_width_mask(decoded->gpr_width))
        return false;

    if (decoded->gpr_bit_offset == 0)
        return true;

    return decoded->gpr_width == 1 && decoded->gpr_bit_offset == 8;
}

static inline uint64_t
relm_decoded_gpr_extract(const struct relm_decoded_insn *decoded,
                         uint64_t enclosing_value)
{
    uint64_t mask;

    if (!relm_decoded_has_valid_gpr(decoded))
        return 0;

    mask = relm_width_mask(decoded->gpr_width);
    return (enclosing_value >> decoded->gpr_bit_offset) & mask;
}

static inline uint64_t
relm_decoded_gpr_commit(const struct relm_decoded_insn *decoded,
                        uint64_t old_value, uint64_t transferred_value)
{
    uint64_t mask;

    if (!relm_decoded_has_valid_gpr(decoded))
        return old_value;

    mask = relm_width_mask(decoded->gpr_width);

    switch (decoded->gpr_write_semantics) {
    case RELM_GPR_WRITE_MERGE: {
        uint64_t lane_mask = mask << decoded->gpr_bit_offset;

        return (old_value & ~lane_mask) |
               ((transferred_value & mask) << decoded->gpr_bit_offset);
    }
    case RELM_GPR_WRITE_ZERO_EXTEND:
    case RELM_GPR_WRITE_REPLACE:
        return transferred_value & mask;
    case RELM_GPR_WRITE_NONE:
    default:
        return old_value;
    }
}


/*
 * relm_decode_instruction() — decode one guest instruction (arch-agnostic
 * entry point). insn_buf holds up to 'len' raw bytes copied from guest
 * memory at the faulting RIP. On success fills *out and returns the
 * instruction length in bytes (callers use it to advance the guest RIP
 * past the emulated instruction); negative errno on failure
 * (-ENOSYS for opcodes the decoder does not handle).
  */
int relm_decode_instruction(const uint8_t *insn_buf, int len,
                            struct relm_decoded_insn *out);

/* Per-arch backends relm_decode_instruction() dispatches to; exactly one
 * is compiled in, selected by the kernel CONFIG_* of the build host. */
#if defined(CONFIG_X86)
int relm_decode_x86(const uint8_t *insn_buf, int len, struct relm_decoded_insn *out);
#elif defined(CONFIG_RISCV)
int relm_decode_riscv(const uint8_t *insn_buf, int len, struct relm_decoded_insn *out);
#elif defined(CONFIG_ARM64)
int relm_decode_arm64(const uint8_t *insn_buf, int len, struct relm_decoded_insn *out);
#endif

#endif /* RELM_DECODER_H */
