
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <relm/decoder.h>

static void expect_gpr_transfer(const uint8_t *bytes, int byte_count,
                                enum relm_memory_access access,
                                unsigned int width, int gpr_index,
                                unsigned int gpr_bit_offset,
                                enum relm_gpr_write_semantics write_semantics)
{
    struct relm_decoded_insn decoded;
    int ret;

    ret = relm_decode_instruction(bytes, byte_count, &decoded);
    assert(ret == byte_count);
    assert(decoded.length == (unsigned int)byte_count);
    assert(decoded.memory_access == access);
    assert(decoded.memory_width == width);
    assert(decoded.operand_kind == RELM_OPERAND_GPR);
    assert(decoded.gpr_index == gpr_index);
    assert(decoded.gpr_width == width);
    assert(decoded.gpr_bit_offset == gpr_bit_offset);
    assert(decoded.gpr_write_semantics == write_semantics);
    assert(relm_decoded_has_valid_gpr(&decoded));
}

static void test_basic_mov(void)
{
    static const uint8_t store_ecx[] = { 0x89, 0x08 }; /* mov [rax], ecx */
    static const uint8_t load_ebx[]  = { 0x8B, 0x18 }; /* mov ebx, [rax] */

    expect_gpr_transfer(store_ecx, sizeof(store_ecx),
                        RELM_MEMORY_ACCESS_WRITE, 4, 1, 0,
                        RELM_GPR_WRITE_NONE);
    expect_gpr_transfer(load_ebx, sizeof(load_ebx),
                        RELM_MEMORY_ACCESS_READ, 4, 3, 0,
                        RELM_GPR_WRITE_ZERO_EXTEND);
}

static void test_rex_register_extension(void)
{
    static const uint8_t store_r9d[] = {
        0x44, 0x89, 0x48, 0x10, /* mov dword ptr [rax + 0x10], r9d */
    };
    static const uint8_t load_r9[] = {
        0x4C, 0x8B, 0x08,       /* mov r9, qword ptr [rax] */
    };

    expect_gpr_transfer(store_r9d, sizeof(store_r9d),
                        RELM_MEMORY_ACCESS_WRITE, 4, 9, 0,
                        RELM_GPR_WRITE_NONE);
    expect_gpr_transfer(load_r9, sizeof(load_r9),
                        RELM_MEMORY_ACCESS_READ, 8, 9, 0,
                        RELM_GPR_WRITE_REPLACE);
}

/*
 * Operand-size prefixes and byte-register encodings are the cases the old
 * wrapper handled incorrectly. AX/CX modify only their low 16 bits, while AH
 * selects bits 15:8 rather than the low byte of RAX.
 */
static void test_partial_register_lanes(void)
{
    static const uint8_t load_cx[] = {
        0x66, 0x8B, 0x08,       /* mov cx, word ptr [rax] */
    };
    static const uint8_t load_ah[] = {
        0x8A, 0x20,             /* mov ah, byte ptr [rax] */
    };
    static const uint8_t store_ah[] = {
        0x88, 0x20,             /* mov byte ptr [rax], ah */
    };
    static const uint8_t load_spl[] = {
        0x40, 0x8A, 0x20,       /* REX changes the same reg field to SPL */
    };
    static const uint8_t prefixed_load_cx[] = {
        0x67, 0x66, 0x8B, 0x08, /* mov cx, word ptr [eax] */
    };
    struct relm_decoded_insn decoded;
    uint64_t old_value = UINT64_C(0x1122334455667788);
    uint64_t committed;
    int ret;

    expect_gpr_transfer(load_cx, sizeof(load_cx),
                        RELM_MEMORY_ACCESS_READ, 2, 1, 0,
                        RELM_GPR_WRITE_MERGE);
    expect_gpr_transfer(load_ah, sizeof(load_ah),
                        RELM_MEMORY_ACCESS_READ, 1, 0, 8,
                        RELM_GPR_WRITE_MERGE);
    expect_gpr_transfer(store_ah, sizeof(store_ah),
                        RELM_MEMORY_ACCESS_WRITE, 1, 0, 8,
                        RELM_GPR_WRITE_NONE);
    expect_gpr_transfer(load_spl, sizeof(load_spl),
                        RELM_MEMORY_ACCESS_READ, 1, 4, 0,
                        RELM_GPR_WRITE_MERGE);
    expect_gpr_transfer(prefixed_load_cx, sizeof(prefixed_load_cx),
                        RELM_MEMORY_ACCESS_READ, 2, 1, 0,
                        RELM_GPR_WRITE_MERGE);

    ret = relm_decode_instruction(store_ah, sizeof(store_ah), &decoded);
    assert(ret == (int)sizeof(store_ah));
    assert(relm_decoded_gpr_extract(&decoded, old_value) == 0x77);

    ret = relm_decode_instruction(load_ah, sizeof(load_ah), &decoded);
    assert(ret == (int)sizeof(load_ah));
    committed = relm_decoded_gpr_commit(&decoded, old_value, 0xAA);
    assert(committed == UINT64_C(0x112233445566AA88));
}

/* A 32-bit destination clears the high half of its enclosing 64-bit GPR. */
static void test_zero_extension(void)
{
    static const uint8_t load_eax[] = { 0x8B, 0x00 }; /* mov eax, [rax] */
    struct relm_decoded_insn decoded;
    uint64_t committed;
    int ret;

    ret = relm_decode_instruction(load_eax, sizeof(load_eax), &decoded);
    assert(ret == (int)sizeof(load_eax));

    committed = relm_decoded_gpr_commit(
        &decoded, UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0x12345678));
    assert(committed == UINT64_C(0x0000000012345678));
}

/*
 * C7 encodes an immediate store. In the REX.W form the physical immediate is
 * 32 bits and the architectural value is sign-extended to the 64-bit memory
 * width; Zydis performs that normalization for the adapter.
 */
static void test_immediate_stores(void)
{
    static const uint8_t store_imm32[] = {
        0xC7, 0x00, 0x78, 0x56, 0x34, 0x12,
    }; /* mov dword ptr [rax], 0x12345678 */
    static const uint8_t store_sign_extended[] = {
        0x48, 0xC7, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    }; /* mov qword ptr [rax], -1 */
    struct relm_decoded_insn decoded;
    int ret;

    ret = relm_decode_instruction(store_imm32, sizeof(store_imm32), &decoded);
    assert(ret == (int)sizeof(store_imm32));
    assert(decoded.memory_access == RELM_MEMORY_ACCESS_WRITE);
    assert(decoded.memory_width == 4);
    assert(decoded.operand_kind == RELM_OPERAND_IMMEDIATE);
    assert(decoded.immediate == UINT64_C(0x12345678));
    assert(decoded.gpr_index == -1);

    ret = relm_decode_instruction(store_sign_extended,
                                  sizeof(store_sign_extended), &decoded);
    assert(ret == (int)sizeof(store_sign_extended));
    assert(decoded.memory_width == 8);
    assert(decoded.operand_kind == RELM_OPERAND_IMMEDIATE);
    assert(decoded.immediate == UINT64_MAX);
}

static void test_fail_closed(void)
{
    static const uint8_t xchg_memory[] = { 0x87, 0x08 }; /* xchg [rax], ecx */
    static const uint8_t register_mov[] = { 0x89, 0xC8 }; /* mov eax, ecx */
    static const uint8_t truncated[] = { 0x48, 0x8B };
    struct relm_decoded_insn decoded;

    assert(relm_decode_instruction(xchg_memory, sizeof(xchg_memory),
                                   &decoded) == -EOPNOTSUPP);
    assert(relm_decode_instruction(register_mov, sizeof(register_mov),
                                   &decoded) == -EOPNOTSUPP);
    assert(relm_decode_instruction(truncated, sizeof(truncated), &decoded) < 0);
}

int main(void)
{
    test_basic_mov();
    test_rex_register_extension();
    test_partial_register_lanes();
    test_zero_extension();
    test_immediate_stores();
    test_fail_closed();

    puts("decoder vectors: PASS");
    return 0;
}
