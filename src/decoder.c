/* SPDX-License-Identifier: GPL-2.0
 *
 * x86_decode.c — x86-64 instruction decoder for APIC write emulation
 *
 * Decodes the memory-write instruction at GUEST_RIP to determine which
 * guest general-purpose register contains the value being written to an
 * APIC MMIO register, or whether the instruction uses an immediate value.
 *
 */ 
#include <cerrno>
#include <linux/printk.h>
#include <linux/string.h>    
#include <linux/highmem.h>   
#include <linux/mm.h>       
 
#include <include/vmx.h>         
#include <include/vmx_ops.h>     
#include <include/ept.h>         
#include <include/x86_decode.h>
#include <stddef.h>
#include <stdint.h>
#include <utils/utils.h>        

/* VMCS field encodings needed for the GVA→GPA walk */
#define VMCS_GUEST_CR0   0x6800   /* contains PG bit (bit 31) */
#define VMCS_GUEST_CR3   0x6802   /* PML4 base GPA */
#define VMCS_GUEST_RIP   0x681E

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

static int relm_read_hpa(uint64_t hpa, void *buf, size_t len)
{
    void *va = phys_to_virt((phys_addr_t)hpa);
    if(!va)
        return -EFAULT;
 
    memcpy(buf, va, len);
    return 0;
}


static int relm_gva_to_gpa(const struct vcpu *vcpu, uint64_t gva, 
                           uint64_t *gpa_out)
{
    uint64_t cr0 = __vmread(VMCS_GUEST_CR0); 

    if(!(cr0 & (1ULL << 31)))
    {
        *gpa_out = gva;
        PDEBUG("RELM: x86_decode: CR0.PG=0 → GVA=0x%llx is GPA (no walk)\n",
               gva);
        return 0;
    }

    uint64_t cr3 = __vmread(VMCS_GUEST_CR3); 
    uint64_t table_gpa = cr3 & 0x000FFFFFFFFFF000ULL;

    uint64_t index; 
    uint64_t entry_gpa; 
    uint64_t entry_hpa; 
    uint64_t entry; 
    int ret; 

    /*level 1 PML4*/ 
    index = (gva >> 39) & 0x1FFULL; 
    entry_gpa = table_gpa + (index * 8); 

    ret = relm_ept_translate_gpa(vcpu->vm->ept, entry_gpa, &entry_hpa); 
    if(ret)
    {
        pr_warn("RELM: x86_decode: PML4 entry GPA=0x%llx not in EPT "
                "(GVA=0x%llx)\n", entry_gpa, gva);
        return -EFAULT;
    }

    ret = relm_read_hpa(entry_hpa, &entry, sizeof(entry));
    if(ret) 
        return ret;

    if(!(entry & 1ULL))
    {
        pr_warn("RELM: x86_decode: PML4E[%llu] not present "
                "(entry=0x%llx GVA=0x%llx)\n", index, entry, gva);
        return -EFAULT;
    }


    table_gpa = entry_gpa & 0x000FFFFFC0000000ULL;
    
    /*level 2 PDPT*/ 
    index = (gva >> 30) & 0x1FFULL;
    entry_gpa = table_gpa + (index * 8);
 
    ret = relm_ept_translate_gpa(vcpu->vm->ept, entry_gpa, &entry_hpa);
    if(ret)
    {
        pr_warn("RELM: x86_decode: PDPT entry GPA=0x%llx not in EPT\n",
                entry_gpa);
        return -EFAULT;
    }
 
    ret = relm_read_hpa(entry_hpa, &entry, sizeof(entry));
    if(ret) return ret;
 
    if(!(entry & 1ULL))
    {
        pr_warn("RELM: x86_decode: PDPTE[%llu] not present (GVA=0x%llx)\n",
                index, gva);
        return -EFAULT;
    }
 
    /* 1 GB page */ 
    if(entry & (1ULL << 7))
    {
        *gpa_out = (entry & 0x000FFFFFC0000000ULL) | (gva & 0x3FFFFFFFULL);
        PDEBUG("RELM: x86_decode: 1 GB page: GVA=0x%llx → GPA=0x%llx\n",
               gva, *gpa_out);
        return 0;
    }
 
    table_gpa = entry & 0x000FFFFFFFFFF000ULL;
 
    /*level 3 PD*/ 
    index = (gva >> 21) & 0x1FFULL; 
    entry_gpa = table_gpa + (index * 8); 

    ret = relm_ept_translate_gpa(vcpu->vm->ept, entry_gpa, &entry_hpa); 

    ret = relm_read_hpa(entry_hpa, &entry, sizeof(entry)); 
    if(ret)
        return ret; 

    if(!(entry & 1ULL))
    {
        pr_warn("RELM: x86_decode: PDE[%llu] not present (GVA=0x%llx)\n",
                index, gva);
        return -EFAULT;
    }

      /* 2 MB page: PDE.PS (bit 7) = 1.
     * Final GPA = PDE[51:21] | GVA[20:0] */
    if(entry & (1ULL << 7))
    {
        *gpa_out = (entry & 0x000FFFFFFFE00000ULL) | (gva & 0x1FFFFFULL);
        PDEBUG("RELM: x86_decode: 2 MB page: GVA=0x%llx → GPA=0x%llx\n",
               gva, *gpa_out);
        return 0;
    }
 
    table_gpa = entry & 0x000FFFFFFFFFF000ULL;

    /*LEVEL 4 PT*/
    index = (gva >> 12) & 0x1FFULL;
    entry_gpa = table_gpa + (index * 8);
 
    ret = relm_ept_translate_gpa(vcpu->vm->ept, entry_gpa, &entry_hpa);
    if(ret)
    {
        pr_warn("RELM: x86_decode: PT entry GPA=0x%llx not in EPT\n",
                entry_gpa);
        return -EFAULT;
    }
 
    ret = relm_read_hpa(entry_hpa, &entry, sizeof(entry));
    if(ret) 
        return ret;
 
    if(!(entry & 1ULL))
    {
        pr_warn("RELM: x86_decode: PTE[%llu] not present (GVA=0x%llx)\n",
                index, gva);
        return -EFAULT;
    }

    /*final 4 KB page */ 
    *gpa_out = (entry & 0x000FFFFFFFFFF000ULL) | (gva & 0xFFFULL);
 
    PDEBUG("RELM: x86_decode: 4 KB page: GVA=0x%llx → GPA=0x%llx\n",
           gva, *gpa_out);
    return 0;

}

static int relm_guest_read_bytes(const struct vcpu *vcpu, 
                                 uint64_t gva, uint8_t *buf, 
                                 uint32_t count)
{
    uint32_t remaining =  count; 
    uint32_t offset = 0; 

    while (remaining > 0)
    {
        uint64_t cur_gva = gva + offset; 
        uint64_t cur_gpa; 
        int ret = relm_gva_to_gpa(vcpu, cur_gva, cur_gpa); 
        if(ret)
        {
            pr_warn("RELM: x86_decode: GVA=0x%llx not mapped in guest "
                    "page tables\n", cur_gva);
            return -EFAULT;
        }

        uint64_t cur_hpa; 
        ret = relm_ept_translate_gpa(vcpu->vm-?ept, cur_gpa, &cur_hpa); 
        if(ret)
        {
            pr_warn("RELM: x86_decode: GPA=0x%llx not mapped in EPT\n",
                    cur_gpa);
            return -EFAULT;
        }

        /* cross-page boundary calculation.
         *
         * cur_hpa includes the byte offset within the host page.
         * We can safely read from cur_hpa up to the end of the 4 KB page
         * without re-translating. The number of bytes until the page
         * boundary is: PAGE_SIZE - (cur_hpa & (PAGE_SIZE - 1)). */ 

        uint32_t offset_in_page = (uint32_t)(cur_hpa & (PAGE_SIZE -1)); 
        uint32_t bytes_in_page = (uint32_t)(PAGE_SIZE - offset_in_page); 
        uint32_t to_copy = (remaining < bytes_in_page) ? remaining : bytes_in_page; 

        ret = relm_read_hpa(cur_hpa, buf + offset, to_copy); 
         if(ret)
        {
            pr_warn("RELM: x86_decode: failed to read %u bytes from "
                    "HPA=0x%llx\n", to_copy, cur_hpa);
            return -EFAULT;
        }

        offset += to_copy;
        remaining -= to_copy;
    }

    return 0; 
}

static bool is_legacy_prefix(uint8_t byte)
{
    switch(byte)
    {
        case X86_PREFIX_LOCK:
        case X86_PREFIX_REPNE:
        case X86_PREFIX_REP:
        case X86_PREFIX_CS:
        case X86_PREFIX_SS:
        case X86_PREFIX_DS:
        case X86_PREFIX_ES:
        case X86_PREFIX_FS:
        case X86_PREFIX_GS:
        case X86_PREFIX_OPERAND_SIZE:
        case X86_PREFIX_ADDR_SIZE:
            return true;
        default:
            return false;
    }
}

static bool is_rex_prefix(uint8_t byte)
{
    return (byte & X86_REX_MASK) == X86_REX_BASE;
}

static uint32_t modrm_extra_bytes(uint8_t modrm, uint8_t sib_byte, 
                                  bool *has_sib)
{
    uint8_t mod = (modrm & X86_MODRM_MOD_MASK) >> X86_MODRM_MOD_SHIFT; 
    uint8_t rm = modrm & X86_MODRM_RM_MASK; 

    uint32_t extra = 0; 
    *has_sib = false; 

    if(rm == X86_MPRM_RM_SIB && mod != (X86_MODRM_MOD_REG >> X86_MODRM_MOD_SHIFT))
    {
        has_sib = true; 
        extra += 1; 
    }

    switch(mod)
    {
        case 0x0: /*mod=00*/
            
            if(rm == X86_MODRM_RM_RIPREL & !(has_sib))
            {
                extra += 4; 
            }
            else if(*has_sib){
                uint8_t sib_base = sib_byte & 0x07; 
                if(sib_base == 5)
                {
                    extra += 4; 
                }
            }
            break; 
        case 0x1: /*mod=01: 8-bit displacement*/
            extra += 1; 
            break; 

        case 0x2: /*mod=10: 32-bit signed displacement*/
            extra += 4; 
            break;

        case 0x3:
            pr_warn("RELM: x86_decode: ModRM mod=11 in memory write context\n");
            break;
    }

    return true; 
}

int relm_x86_decode_apic_write(struct vcpu *vcpu, struct x86_decode_write *out)
{
    uint8_t bytes[X86_INS_LEN]; 
    uint32_t pos = 0; 
    bool rex_r = false; 
    bool rex_present = false; 

    uint64_t rip = __vmread(GUEST_RIP); 

    memset(out, 0, sizeof(*out)); 
    out->result = X86_DECODE_FAIL; 
    out->reg_num = 0xFF; /*sentinel: no register*/ 

    if(relm_guest_read_bytes(vcpu, rip, bytes, X86_MAX_INSN_LEN) != 0)
    {
        pr_warn("RELM: x86_decode: failed to read %u bytes at GUEST_RIP=0x%llx\n",
                X86_MAX_INSN_LEN, rip);
        return -1;
    }

    /*skip legacy prefix */ 
    {
        int prefix_count = 0;
        while(pos < X86_MAX_INSN_LEN &&
              prefix_count < 4 &&
              is_legacy_prefix(bytes[pos]))
        {
            PDEBUG("RELM: x86_decode: skipping legacy prefix 0x%02x at pos %u\n",
                   bytes[pos], pos);
            pos++;
            prefix_count++;
        }
    }

    if(pos < X86_MAX_INSN_LEN && is_rex_prefix(bytes[pos]))
    {
        uint8_t rex = bytes[pos]; 
        rex_present = true; 

        /*REX.R*/ 
        rex_r = (rex & X86_REX_R) != 0; 
        
        PDEBUG("RELM: x86_decode: REX byte=0x%02x: W=%u R=%u X=%u B=%u\n",
               rex,
               (rex & X86_REX_W) ? 1 : 0,
               (rex & X86_REX_R) ? 1 : 0,
               (rex & X86_REX_X) ? 1 : 0,
               (rex & X86_REX_B) ? 1 : 0);

        pos++; 
    }

    if(pos >= X86_MAX_INSN_LEN)
    {
        pr_warn("RELM: x86_decode: no opcode byte (ran out of instruction bytes)\n");
        return -1;
    }


    uint8_t opcode = bytes[pos++]; 
    
    PDEBUG("RELM: x86_decode: opcode=0x%02x%s at GUEST_RIP=0x%llx\n",
           opcode, rex_present ? " (REX present)" : "", rip);

    if(pos >= X86_MAX_INSN_LEN)
    {
        pr_warn("RELM: x86_decode: missing ModRM byte\n");
        return -1;
    }
 
    uint8_t modrm = bytes[pos++];
    
    uint8_t modrm_reg = (modrm & X86_MODRM_REG_MASK) >> X86_MODRM_MOD_SHIFT;
    uint8_t reg_num   = modrm_reg + (rex_r ? 8 : 0);
 
    uint8_t mod = (modrm & X86_MODRM_MOD_MASK) >> X86_MODRM_MOD_SHIFT;
    uint8_t rm  =  modrm & X86_MODRM_RM_MASK;

    uint8_t sib_byte = 0;
    if(rm == X86_MODRM_RM_SIB && mod != 0x3)
    {
        if(pos >= X86_MAX_INSN_LEN)
        {
            pr_warn("RELM: x86_decode: missing SIB byte\n");
            return -1;
        }
        sib_byte = bytes[pos++];  /* consume the SIB byte */
 
        PDEBUG("RELM: x86_decode: SIB byte=0x%02x (scale=%u index=%u base=%u)\n",
               sib_byte,
               (sib_byte >> 6) & 0x3,
               (sib_byte >> 3) & 0x7,
               sib_byte & 0x7);
    }

   
    bool has_sib = (sib_byte != 0 || (rm == X86_MODRM_RM_SIB && mod != 0x3));
    uint32_t disp_size = modrm_extra_bytes(modrm, sib_byte, &has_sib);
 
    if(pos + disp_size > X86_MAX_INSN_LEN)
    {
        pr_warn("RELM: x86_decode: displacement overflows instruction buffer\n");
        return -1;
    }
 
    pos += disp_size;  /* skip past the displacement bytes */

     switch(opcode)
    {
        case X86_OP_MOV_RM_R:
        case X86_OP_MOV_RM8_R8:
        {
            uint32_t value = relm_x86_gpr_value(vcpu, reg_num);
 
            out->result   = X86_DECODE_REG;
            out->reg_num  = reg_num;
            out->value    = value;
            out->insn_len = (uint8_t)pos;  /* everything consumed so far */
 
            pr_info("RELM: x86_decode: APIC write from %s (reg %u) = 0x%08x "
                    "insn_len=%u\n",
                    relm_x86_gpr_name(reg_num), reg_num, value, pos);
            return 0;
        }
 
        case X86_OP_MOV_RM_IMM:
        {
            if(pos + 4 > X86_MAX_INSN_LEN)
            {
                pr_warn("RELM: x86_decode: immediate overflows instruction "
                        "buffer (pos=%u)\n", pos);
                return -1;
            }
 
            uint32_t imm32 =
                (uint32_t)bytes[pos + 0]          |   /* bits  7:0  */
                (uint32_t)bytes[pos + 1] << 8      |   /* bits 15:8  */
                (uint32_t)bytes[pos + 2] << 16     |   /* bits 23:16 */
                (uint32_t)bytes[pos + 3] << 24;        /* bits 31:24 */
 
            pos += 4;  /* consume the 4-byte immediate */
 
            out->result   = X86_DECODE_IMM;
            out->reg_num  = 0xFF;  /* no register */
            out->value    = imm32;
            out->insn_len = (uint8_t)pos;
 
            pr_info("RELM: x86_decode: APIC write immediate 0x%08x "
                    "insn_len=%u\n", imm32, pos);
            return 0;
        }
 
        case X86_OP_MOV_RM8_IMM8:
        {
            /* 8-bit immediate: a single byte after the ModRM+displacement */
            if(pos >= X86_MAX_INSN_LEN)
            {
                pr_warn("RELM: x86_decode: missing imm8 byte\n");
                return -1;
            }
 
            uint32_t imm8 = (uint32_t)bytes[pos++];
 
            out->result   = X86_DECODE_IMM;
            out->reg_num  = 0xFF;
            out->value    = imm8;
            out->insn_len = (uint8_t)pos;
 
            pr_info("RELM: x86_decode: APIC write imm8 0x%02x insn_len=%u\n",
                    imm8, pos);
            return 0;
        }
 
        default:
            pr_warn("RELM: x86_decode: unrecognised opcode 0x%02x "
                    "at GUEST_RIP=0x%llx (bytes: %02x %02x %02x %02x %02x %02x %02x %02x)\n",
                    opcode, rip,
                    bytes[0], bytes[1], bytes[2], bytes[3],
                    bytes[4], bytes[5], bytes[6], bytes[7]);
            return -1;
    }
}

