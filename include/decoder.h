/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_X86_DECODE_H
#define RELM_X86_DECODE_H

#include <linux/types.h>
#include <stdint.h>
 
struct vcpu;


#define X86_MAX_INS_LEN 15 

#define X86_OP_MOV_RM8_R8       0x88    /* MOV r/m8,  r8          */
#define X86_OP_MOV_RM_R         0x89    /* MOV r/m16/32/64, r     ← main case */
#define X86_OP_MOV_RM8_IMM8     0xC6    /* MOV r/m8,  imm8        */
#define X86_OP_MOV_RM_IMM       0xC7    /* MOV r/m16/32/64, imm   */

#define X86_REX_BASE            0x40    
#define X86_REX_MASK            0xF0    
#define X86_REX_W               0x08    
#define X86_REX_R               0x04        
#define X86_REX_X               0x02    
#define X86_REX_B               0x01    

#define X86_MODRM_MOD_MASK      0xC0    
#define X86_MODRM_MOD_SHIFT     6
#define X86_MODRM_REG_MASK      0x38
#define X86_MODRM_REG_SHIFT     3
#define X86_MODRM_RM_MASK       0x07    
 
#define X86_MODRM_MOD_NODISP    0x00    /* mod=00: no displacement          */
#define X86_MODRM_MOD_DISP8     0x40    /* mod=01: 8-bit displacement       */
#define X86_MODRM_MOD_DISP32    0x80    /* mod=10: 32-bit displacement      */
#define X86_MODRM_MOD_REG       0xC0    /* mod=11: register direct          */
 
#define X86_MODRM_RM_SIB        4    /* r/m=100: SIB byte follows            */
#define X86_MODRM_RM_RIPREL     5    /* r/m=101 with mod=00: RIP-relative    */

#define X86_PREFIX_OPERAND_SIZE  0x66   /* switch 16/32-bit operand size    */
#define X86_PREFIX_ADDR_SIZE     0x67   /* switch 32/64-bit address size    */
#define X86_PREFIX_LOCK          0xF0
#define X86_PREFIX_REPNE         0xF2
#define X86_PREFIX_REP           0xF3
#define X86_PREFIX_CS            0x2E
#define X86_PREFIX_SS            0x36
#define X86_PREFIX_DS            0x3E
#define X86_PREFIX_ES            0x26
#define X86_PREFIX_FS            0x64
#define X86_PREFIX_GS            0x65

enum x86_decode_result
{
    X86_DECODE_REG = 0, 
    X86_DECODE_IMM = 1, 
    X86_DECODE_FAIL = -1, 
}; 

struct x86_decode_write
{
    enum x86_decode_result; 
    uint8_t reg_num; 
    uint32_t value; 
    uint8_t insn_len; 
}; 


int relm_x86_decode_apic_write(struct vcpu *vcpu, 
                               struct x86_decoded_write *out);
uint32_t relm_x86_gpr_value(const struct vcpu *vcpu,
                            uint8_t reg_num);
const char *relm_x86_gpr_name(uint8_t reg_num);

#endif 


