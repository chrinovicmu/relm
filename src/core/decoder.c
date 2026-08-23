/*
 * src/core/decoder.c — generic-layer dispatcher for the instruction decoder.
 *
 * The core (arch-independent) emulation paths — virtio-MMIO EPT-violation
 * handling, APIC-access emulation — need to decode faulting guest
 * instructions, but must not contain arch-specific decode logic. This file
 * is the seam: it forwards to whichever per-arch decoder was compiled in.
 * The actual x86 decoding lives in src/arch/x86/decoder/utils.c
 * (relm_decode_x86), built on the kernel's insn_decode() machinery.
 */

#include <linux/errno.h>
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
