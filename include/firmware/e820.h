/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_E820_H
#define RELM_E820_H
 
#include <linux/types.h>
 
struct relm_vm;


/*OS may allocate freely */ 
#define E820_TYPE_RAM       1

/*OS must not use this range */ 
#define E820_TYPE_RESERVED  2 


/*contains ACPI tabales. OS may reclaim this memory after 
* it has finished parsing ACPI tables (after early boot) */ 
#define E820_TYPE_ACPI      3 

/*non-volatile storage */ 
#define E820_TYPE_NVS       4

/*unsable*/ 
#define E820_TYPE_UNUSABLE 5 

/*persistent memory*/ 
#define E820_TYPE_PMEM      7 


struct e820_entry 
{
    uint64_t addr; 
    uint64_t size; 
    uint32_t type; 
}__attribute__((packed));


/*620 KB conventional mmeory uppper boundary */ 
#define RELM_E820_CONV_MEM_END  0x000A0000ULL 

/*384 KB VGA/BIOS region*/  
#define RELM_E820_VGA_RESERVED_BASE 0x000A0000ULL
#define RELM_E820_VGA_RESERVED_END  0x00100000ULL

/*extended memory base: phsyical RAM resumes above 1MB */ 
#define RELM_E820_EXTENDED_BASE     0x00100000ULL 

/*SeaBIOS ROM at the top of 4 GB (0xFFFE0000-0xFFFFFFFF, 128 KB) */ 
#define RELM_E820_BIOS_ROM_BASE     0xFFFE0000ULL 
#define RELM_E820_BIOS_ROM_SIZE     (128 * 1024ULL)
#define RELM_E820_BIOS_ROM_END      (RELM_E820_BIOS_ROM_BASE + RELM_E820_BIOS_ROM_SIZE)

#define RELM_E820_MAX_ENTRIES       8   

struct relm_e820_map 
{
    struct e820_entry entries[RELM_E820_MAX_ENTRIES]; 
    uint32_t count; 
}; 


int relm_e820_build(const struct relm_vm *vm, struct relm_e820_map *map); 
void relm_e820_dump(const struct relm_e820_map *map); 

#endif 


