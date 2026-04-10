/* SPDX-License-Identifier: GPL-2.0
 *
 * x86_decode.c — x86-64 instruction decoder for APIC write emulation
 *
 * Decodes the memory-write instruction at GUEST_RIP to determine which
 * guest general-purpose register contains the value being written to an
 * APIC MMIO register, or whether the instruction uses an immediate value.
 *
 */
 
#include <linux/printk.h>
#include <linux/string.h>    
#include <linux/highmem.h>   
#include <linux/mm.h>       
 
#include <include/vmx.h>         
#include <include/vmx_ops.h>     
#include <include/ept.h>         
#include <include/x86_decode.h>
#include <utils/utils.h>        

static const char *const gpr_names[16] = {
    "RAX",  /* 0 */
    "RCX",  /* 1 */
    "RDX",  /* 2 */
    "RBX",  /* 3 */
    "RSP",  /* 4 */
    "RBP",  /* 5 */
    "RSI",  /* 6 */
    "RDI",  /* 7 */
    "R8",   /* 8  — requires REX.R=1 to address */
    "R9",   /* 9  — requires REX.R=1 */
    "R10",  /* 10 — requires REX.R=1 */
    "R11",  /* 11 — requires REX.R=1 */
    "R12",  /* 12 — requires REX.R=1 */
    "R13",  /* 13 — requires REX.R=1 */
    "R14",  /* 14 — requires REX.R=1 */
    "R15",  /* 15 — requires REX.R=1 */
};

const char *relm_x86_gpr_name(uint8_t reg_num)
{
    if(reg_num > 15)
        return "invlaid reg num";
    return gpr_names[reg_num];
}


uint32_t relm_x86_gpr_value(const struct vcpu *vcpu, uint8_t reg_num)
{
    switch(reg_num)
    {
        case  0:
            return (uint32_t)(vcpu->regs.rax & 0xFFFFFFFFUL);
        case  1:
            return (uint32_t)(vcpu->regs.rcx & 0xFFFFFFFFUL);
        case  2:
            return (uint32_t)(vcpu->regs.rdx & 0xFFFFFFFFUL);
        case  3: 
            return (uint32_t)(vcpu->regs.rbx & 0xFFFFFFFFUL);
        case  4:
            return (uint32_t)(vcpu->regs.rsp & 0xFFFFFFFFUL); /* guest RSP */
        case  5:
            return (uint32_t)(vcpu->regs.rbp & 0xFFFFFFFFUL);
        case  6:
            return (uint32_t)(vcpu->regs.rsi & 0xFFFFFFFFUL);
        case  7:
            return (uint32_t)(vcpu->regs.rdi & 0xFFFFFFFFUL);
        case  8:
            return (uint32_t)(vcpu->regs.r8  & 0xFFFFFFFFUL);
        case  9:
            return (uint32_t)(vcpu->regs.r9  & 0xFFFFFFFFUL);
        case 10:
            return (uint32_t)(vcpu->regs.r10 & 0xFFFFFFFFUL);
        case 11:
            return (uint32_t)(vcpu->regs.r11 & 0xFFFFFFFFUL);
        case 12:
            return (uint32_t)(vcpu->regs.r12 & 0xFFFFFFFFUL);
        case 13:
            return (uint32_t)(vcpu->regs.r13 & 0xFFFFFFFFUL);
        case 14:
            return (uint32_t)(vcpu->regs.r14 & 0xFFFFFFFFUL);
        case 15:
            return (uint32_t)(vcpu->regs.r15 & 0xFFFFFFFFUL);
        default:
            pr_warn("RELM: x86_decode: invalid register number %u\n", reg_num);
            return 0;
    }
}


