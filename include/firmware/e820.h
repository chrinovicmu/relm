#ifndef RELM_E820_H 
#define RELM_E820_H

#include <linux/types.h>
#include <stdint.h>

/*usable RAM: OS may allocate from this range freely*/ 
#define E820_TYPE_RAM       1 

/*reserved: firmaware, MMIO, ROM*/ 
#define E820_TYPE_RESERVED  2 

/*ACPI reclaim: contain acpi tables, OS may reclaim this memory after parsing 
* ACPI tables */ 
#define E820_TYPE_ACPI      3 

/*ACPI NVS : firmaware uses this across S3 sleep/resume. 
* OS must preserve this range */ 
#define E820_TYPE_NVS       4

#define E820_TYPE_UNUSABLE  5

/*persistent memory(NVDIMM): bytes-addressble persistent storage*/  
#define E820_TYPE_PMEM      7 


/*once contigous phsycall memory range*/ 
struct e820_entry
{
    uint64_t addr; 
    uint64_t size; 
    uint32_t type; 
}__attribute__((packed)); 


/*RELM-specifc contants for out 128 MB guest memory layout */ 

#define RELM_E820_CONV_MEM_END      0x000A0000ULL   /* = 640 KB */
 
/* The VGA/BIOS reserved region: 0xA0000–0xFFFFF (384 KB).
 * 0xA0000-0xBFFFF = VGA frame buffer and graphics MMIO
 * 0xC0000-0xDFFFF = Option ROM area (expansion ROMs, e.g. NIC BIOS)
 * 0xE0000-0xFFFFF = System BIOS ROM (legacy, also mapped at top of 4 GB) */
#define RELM_E820_VGA_RESERVED_BASE 0x000A0000ULL
#define RELM_E820_VGA_RESERVED_END  0x00100000ULL   /* = 1 MB */
 
/* Extended memory base: physical RAM resumes above 1 MB.
 * The "1 MB hole" (0xA0000-0xFFFFF) means usable RAM resumes at exactly 1 MB. */
#define RELM_E820_EXTENDED_BASE     0x00100000ULL   /* = 1 MB */
 
/* RELM guest page table area reservation: the top 128 KB of guest RAM.
 * These pages (PML4, PDPT, PD) are used by RELM's guest address space and
 * must not be overwritten by the guest OS's memory allocator.
 * For 128 MB total: 0x08000000 - 0x20000 = 0x07FE0000. */
#define RELM_E820_PAGETABLE_SIZE    (128 * 1024ULL)
 
/* SeaBIOS ROM at the top of 4 GB (0xFFFE0000–0xFFFFFFFF, 128 KB).
 * This range is ALWAYS reserved in a virtualised system because the BIOS
 * ROM shadow lives here. The CPU's reset vector (0xFFFFFFF0) points into
 * this range. SeaBIOS's binary is mapped here by relm_seabios_load(). */
#define RELM_E820_BIOS_ROM_BASE     0xFFFE0000ULL
#define RELM_E820_BIOS_ROM_SIZE     (128 * 1024ULL)
#define RELM_E820_BIOS_ROM_END      (RELM_E820_BIOS_ROM_BASE + RELM_E820_BIOS_ROM_SIZE)
 
/* Maximum number of e820 entries RELM will generate.
 * Count the entries in relm_e820_build: conventional, VGA, extended,
 * page-table reservation, BIOS ROM = 5 entries, plus 2 spare. */
#define RELM_E820_MAX_ENTRIES       8
 
struct relm_e820_map
{
    struct e820_entry entries[RELM_E820_MAX_ENTRIES];
 
    /* count: number of valid entries in the entries[] array.
     * entries[0..count-1] are populated; entries[count..MAX-1] are zero. */
    uint32_t count;
};
 
int relm_e820_build(const struct relm_vm *vm, struct relm_e820_map *map);
void relm_e820_dump(const struct relm_e820_map *map);

#endif // !
