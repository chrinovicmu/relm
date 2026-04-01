/* SPDX-License-Identifier: GPL-2.0
 *
 * e820.c — e820 physical memory map builder for RELM guests
 *
 * Constructs the memory type map that SeaBIOS will expose to the guest OS
 * via INT 0x15, AX=0xE820. The map describes which physical address ranges
 * are usable RAM, which are reserved for MMIO/ROM, and which are ACPI.
 */
 
#include <linux/printk.h>
#include <linux/string.h>
 
#include <include/vm.h>
#include <include/firmware/e820.h>
#include <stdint.h>
#include <utils/utils.h>


statc int relm_e820_add(struct relm_820_map *map, 
                        uint64_t addr, uint64_t size, uint32_t type)
{
    if(map->count >= RELM_E820_MAX_ENTRIES)
    {
        pr_err("RELM: e820: map full (%u entries), cannot add "
               "[0x%llx - 0x%llx) type=%u\n",
               RELM_E820_MAX_ENTRIES, addr, addr + size, type);
        return -ENOSPC;
    }

    map->entries[map->count].addr = addr;
    map->entries[map->count].size = size;
    map->entries[map->count].type = type;
    map->count++;
 
    PDEBUG("RELM: e820: [%u] 0x%016llx - 0x%016llx type=%u\n",
           map->count - 1, addr, addr + size, type);
 
    return 0;
}

/* relm_e820_build
 * ----------------
 * Construct the full e820 map for the guest, covering all physical address
 * ranges from 0 to 4 GB relevant to a 128 MB RELM guest.
 *
 * The layout we produce (for 128 MB = 0x08000000 bytes of total guest RAM):
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  GPA Range                  │  Size     │ Type │ Description        │
 *  ├─────────────────────────────┼───────────┼──────┼────────────────────┤
 *  │  0x00000000 – 0x0009FFFF   │  640 KB   │  1   │ Conventional RAM   │
 *  │  0x000A0000 – 0x000FFFFF   │  384 KB   │  2   │ VGA/BIOS reserved  │
 *  │  0x00100000 – 0x07FDFFFF   │  ~127 MB  │  1   │ Extended RAM       │
 *  │  0x07FE0000 – 0x07FFFFFF   │  128 KB   │  2   │ Guest page tables  │
 *  │  0xFFFE0000 – 0xFFFFFFFF   │  128 KB   │  2   │ SeaBIOS ROM        │
 *  └─────────────────────────────────────────────────────────────────────┘
 *
 * Why we split RAM around the VGA hole (0xA0000-0xFFFFF):
 *   The x86 PC architecture permanently hardwires 0xA0000-0xFFFFF for VGA
 *   and BIOS. There is no physical RAM there even if the CPU address bus
 *   could address it — the bus decode logic routes those addresses to the
 *   VGA card or BIOS ROM chip, not to the DRAM. The OS must know this or
 *   it would try to use VGA registers as heap memory, causing random crashes.
 *
 * Why we reserve the guest page table area:
 *   RELM places its guest page tables at the top of guest RAM (see
 *   relm_vm_create_guest_page_tables in vm.c). If the guest OS allocates
 *   physical pages from this region and overwrites the page tables, the
 *   guest's own virtual-to-physical mapping breaks and it triple-faults.
 *   Marking it reserved prevents the guest allocator from touching it.
 *
 * Why we reserve 0xFFFE0000-0xFFFFFFFF:
 *   This is where SeaBIOS is loaded (the BIOS ROM area at top of 4 GB).
 *   The CPU's reset vector at 0xFFFFFFF0 points here. The OS must never
 *   allocate physical memory at these addresses.
 */

int relm_e820_build(const struct relm_vm *vm, struct relm_e820_map *map)
{
    uint64_t total_ram = vm->total_guest_ram; 

     /* ram_end: the first GPA byte AFTER all guest RAM.
     * Extended RAM starts at 1 MB and extends to ram_end. */
    uint64_t ram_end;
 
    /* extended_ram_end: how far extended RAM goes before the page table
     * reservation begins. Computed as total_ram minus the reservation. */
    uint64_t extended_ram_end;
 
    /* page_table_base: start of the guest page table reservation.
     * This is the top RELM_E820_PAGETABLE_SIZE bytes of guest RAM. */
    uint64_t page_table_base;
 
    int ret;
 
    /* Validate that the VM has enough RAM for the required reservations.
     * We need at least 2 MB: 1 MB for the VGA hole cutoff + 128 KB for
     * page tables + some usable extended memory above 1 MB. */
    if(total_ram < 2 * 1024 * 1024)
    {
        pr_err("RELM: e820: guest RAM %llu MB is too small "
               "(minimum 2 MB)\n", total_ram / (1024 * 1024));
        return -EINVAL;
    }

    ram_end = total_ram; 
    page_table_base = ram_end - RELM_E820_PAGETABLE_SIZE; 
    extended_ram_end = page_table_base; 

    memset(map, 0, sizeof(*map));

    ret = relm_e820_add(map,
                        0x00000000ULL,
                        RELM_E820_CONV_MEM_END,
                        E820_TYPE_RAM);
    if(ret) 
        return ret;

    ret = relm_e820_add(map,
                        RELM_E820_VGA_RESERVED_BASE,
                        RELM_E820_VGA_RESERVED_END - RELM_E820_VGA_RESERVED_BASE,
                        E820_TYPE_RESERVED);
    if(ret)
        return ret;

    ret = relm_e820_add(map,
                        RELM_E820_EXTENDED_BASE,
                        extended_ram_end - RELM_E820_EXTENDED_BASE,
                        E820_TYPE_RAM);
    if(ret)
        return ret;

    ret = relm_e820_add(map,
                        page_table_base,
                        RELM_E820_PAGETABLE_SIZE,
                        E820_TYPE_RESERVED);
    if(ret) return ret;

    ret = relm_e820_add(map,
                        RELM_E820_BIOS_ROM_BASE,
                        RELM_E820_BIOS_ROM_SIZE,
                        E820_TYPE_RESERVED);
    if(ret) return ret;
 
    pr_info("RELM: e820: built %u-entry map for %llu MB guest\n",
            map->count, total_ram / (1024 * 1024));
 
    return 0;
}

void relm_e820_dump(const struct relm_e820_map *map)
{
    uint32_t i;
    static const char *type_names[] = {
        [E820_TYPE_RAM]       = "RAM",
        [E820_TYPE_RESERVED]  = "RESERVED",
        [E820_TYPE_ACPI]      = "ACPI-RECLAIM",
        [E820_TYPE_NVS]       = "ACPI-NVS",
        [E820_TYPE_UNUSABLE]  = "UNUSABLE",
        [E820_TYPE_PMEM]      = "PMEM",
    };
 
    pr_info("RELM: e820 map (%u entries):\n", map->count);
    for(i = 0; i < map->count; i++)
    {
        const struct e820_entry *e = &map->entries[i];
        const char *name = (e->type < ARRAY_SIZE(type_names) && type_names[e->type])
                           ? type_names[e->type] : "UNKNOWN";
 
        pr_info("  [%u] 0x%016llx – 0x%016llx  %6llu KB  %s\n",
                i,
                e->addr,
                e->addr + e->size,
                e->size / 1024,
                name);
    }
}
 
