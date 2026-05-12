/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_SEABIOS_H
#define RELM_SEABIOS_H
 
#include <linux/types.h>
#include <include/firmware/fw_cfg.h>
#include <include/firmware/e820.h>
 
struct relm_vm;
struct vcpu;

#define SEABIOS_SIZE                (128 * 1024ULL) /*128 KB*/ 

/*GPA where SeaBIOS is mapped at the top of the 4GB address space 
 * 0xFFFE0000 = 4GB - 128 KB. this covers the reset vector at 0xFFFFFF0*/
#define SEABIOS_ROM_TOP_GPA         (0x100000000ULL - SEABIOS_SIZE) 

/*GPA to where the reset vector executes a far jump to (1MB)
*/ 
#define SEABIOS_ROM_LOW_GPA         0x000E0000ULL 

/*EPT flags for BIOS ROM mapping
 * read and execute only, uncacheable*/ 
#define SEABIOS_EPT_FLAGS           (0x5ULL)   /* Read=1 Write=0 Execute=1 MT=UC */

/* Firmware name for request_firmware() lookup.
 * The kernel looks for this in /lib/firmware/ (and its subdirectories).
 * User must: cp /usr/share/seabios/bios.bin /lib/firmware/seabios/bios.bin */
#define SEABIOS_FIRMWARE_NAME   "seabios/bios.bin"

#define SEABIOS_INITIAL_CS_SELECTOR 0xF000 
#define SEABIOS_INITIAL_CS_BASE     0xFFFF0000ULL

/*64 kb after the base */ 
#define SEABIOS_INITIAL_CS_LIIMIT   0x0000FFFFUL
#define SEABIOS_INITIAL_CS_ACCESS   0x0000009BUL 

/* IP at reset = 0xFFF0. Linear = CS.base + IP = 0xFFFF0000 + 0xFFF0 = 0xFFFFFFF0 */
#define SEABIOS_INITIAL_RIP         0x0000FFF0ULL

/* DS/ES/FS/GS/SS at reset: selector=0, base=0, limit=0xFFFF.
 * Access = 0x93: Present(1) DPL=0 S=1 Type=3(read/write/accessed) */
#define SEABIOS_INITIAL_DS_SELECTOR 0x0000
#define SEABIOS_INITIAL_DS_BASE     0x00000000ULL
#define SEABIOS_INITIAL_DS_LIMIT    0x0000FFFFUL
#define SEABIOS_INITIAL_DS_ACCESS   0x00000093UL

/* GDTR/IDTR at reset: base=0, limit=0xFFFF (real mode vectors at 0000:0000) */
#define SEABIOS_INITIAL_GDTR_BASE   0x00000000ULL
#define SEABIOS_INITIAL_GDTR_LIMIT  0x0000FFFFUL
#define SEABIOS_INITIAL_IDTR_BASE   0x00000000ULL
#define SEABIOS_INITIAL_IDTR_LIMIT  0x0000FFFFUL

/* LDTR: access = 0x82 (LDT descriptor, present, DPL=0) */
#define SEABIOS_INITIAL_LDTR_ACCESS 0x00000082UL
 
/* TR: access = 0x8B (busy 32-bit TSS, present, DPL=0) */
#define SEABIOS_INITIAL_TR_ACCESS   0x0000008BUL

/* CR0 at reset: bit 4 (ET=1) is always 1 for Pentium+. PE=0, PG=0.
 * 0x60000010 = bit 4 (ET) + bits 30:29 (NW=0, CD=0 → cache enabled).
 * With UNRESTRICTED_GUEST the VMX checks allow PE=0 in the guest. */
#define SEABIOS_INITIAL_CR0         0x60000010ULL
 
/* CR4 at reset: all bits 0 */
#define SEABIOS_INITIAL_CR4         0x00000000ULL
 
/* EFER at reset: all bits 0 (LME=0, LMA=0, SCE=0, NXE=0) */
#define SEABIOS_INITIAL_EFER        0x00000000ULL
 
/* RFLAGS at reset: bit 1 (reserved, always 1) set. All others 0.
 * IF=0 (interrupts disabled), TF=0 (no trap), VM=0 (not virtual-8086). */
#define SEABIOS_INITIAL_RFLAGS      0x00000002ULL
 
/* VM_ENTRY_CONTROLS without IA32E_MODE_GUEST bit.
 * When starting SeaBIOS in real mode, IA32E_MODE_GUEST (bit 9) is  0.
 * Compare with RELM's normal long-mode controls which have bit 9 set. */
#define VM_ENTRY_CONTROLS_REALMODE  (VM_ENTRY_LOAD_GUEST_PAT   | \
                                    VM_ENTRY_LOAD_IA32_EFER    | \
                                    VM_ENTRY_LOAD_DEBUG)

struct relm_firmware_data 
{
    uint64_t seabios_va; 
    uint64_t seabios_pa; 
    struct fw_cfg_device fw_cfg;  
}; 

/*relm_seabios_load - load the SeaBIOS binary and map it into the guest EPT */ 
int relm_seabios_load(struct relm_vm *vm, struct device *dev); 
void relm_seabios_unload(struct relm_vm *vm, struct device *dev); 

/*configure a VCPU's initial state for SeaBIOS */ 
int relm_seabios_set_vcpu_state(struct vcpu *vcpu); 

int relm_seabios_setup(struct relm_vm *vm, struct device *dev); 

#endif 
