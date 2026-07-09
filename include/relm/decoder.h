#ifndef RELM_DECODER_H
#define RELM_DECODER_H

#include <linux/types.h>

struct relm_decoded_insn {
    bool is_write;
    unsigned int op_size; 

    bool is_immediate; 
    uint64_t immediate;

    int src_reg; 
    int dst_reg; 
};

int relm_decode_instruction(const uint8_t *insn_buf, int len, 
                            struct relm_decoded_insn *out);

#if defined(CONFIG_X86)
int relm_decode_x86(const uint8_t *insn_buf, int len, struct relm_decoded_insn *out);
#elif defined(CONFIG_RISCV)
int relm_decode_riscv(const uint8_t *insn_buf, int len, struct relm_decoded_insn *out);
#elif defined(CONFIG_ARM64)
int relm_decode_arm64(const uint8_t *insn_buf, int len, struct relm_decoded_insn *out);
#endif

#endif /* RELM_DECODER_H */
