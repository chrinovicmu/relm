#ifndef RELM_DECODER_H
#define RELM_DECODER_H

/*
 * include/relm/decoder.h — generic-layer instruction-decoder interface.
 *
 * When the guest touches an address we emulate (virtio-MMIO window, the
 * APIC page), the access traps to the hypervisor (EPT violation or
 * APIC-access exit) but the hardware does NOT tell us *what* the guest was
 * trying to do — only the faulting address. To emulate the access we must
 * fetch the instruction bytes at the guest's RIP and decode them ourselves
 * to learn: read or write? how wide? which register (or immediate) is the
 * source/destination?
 *
 * This header is arch-agnostic: callers (virtio/mmio.c, vmx/apic.c) only
 * see 'struct relm_decoded_insn' and relm_decode_instruction(); the
 * per-arch decoders (relm_decode_x86, ...) live under src/arch/.
 */

#include <linux/types.h>

/*
 * struct relm_decoded_insn — the decoded summary of one guest instruction
 * that faulted on an emulated memory region. Filled by
 * relm_decode_instruction(); consumed by the MMIO/APIC emulation paths.
 *
 * is_write:     true  = guest stores TO the emulated region (mov reg/imm -> mem)
 *               false = guest loads FROM it (mov mem -> reg)
 * op_size:      operand width in bytes (2/4/8) from prefixes: REX.W = 8,
 *               0x66 = 2, default = 4. Decides masking / zero-extension.
 * access_size:  width of the memory access itself (currently mirrors op_size).
 * is_immediate: true when the store carries an immediate operand (e.g.
 *               0xC7 MOV imm32 -> mem) — there is NO source GPR in that case,
 *               so src_reg must not be consulted.
 * immediate:    the immediate value when is_immediate is set.
 * src_reg:      GPR index of the store's source register (writes only).
 * dst_reg:      GPR index of the load's destination register (reads only).
 *
 * src_reg/dst_reg use the x86 instruction-encoding numbering
 * (0=RAX 1=RCX 2=RDX 3=RBX 4=RSP 5=RBP 6=RSI 7=RDI 8-15=R8-R15) — the
 * same order as guest_reg_ptr() in vcpu_arch.h and the gpr_table in
 * virtio/mmio.c, so the index can be used directly to resolve the register.
 */
struct relm_decoded_insn {
    bool is_write;
    unsigned int op_size;
    unsigned int access_size; //width of mmeory access
    bool is_immediate;
    uint64_t immediate;

    int src_reg;
    int dst_reg;
};

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
