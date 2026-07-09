#include <relm/decoder.h>

int relm_decode_instruction(const uint8_t *insn_buf, int len, 
                            struct relm_decoded_insn *out)
{
#if defined(CONFIG_X86)
    return relm_decode_x86(insn_buf, len, out);
#elif defined(CONFIG_RISCV)
    return relm_decode_riscv(insn_buf, len, out);
#elif defined(CONFIG_ARM64)
    return relm_decode_arm64(insn_buf, len, out);
#else
    return -ENOSYS;
#endif
}
