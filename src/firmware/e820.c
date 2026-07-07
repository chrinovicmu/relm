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
#include <utils/utils.h>



static int relm_e820_add(struct relm_e820_map *map, 
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


int relm_e820_build(const struct relm_vm *vm, struct relm_e820_map *map)
{
    uint64_t total_ram = vm->memory.total_guest_ram;
    int ret;
 
    /* Validate minimum RAM size: need at least 1 MB for the VGA hole
     * boundary plus some usable extended memory above 1 MB. */
    if(total_ram < 2 * 1024 * 1024)
    {
        pr_err("RELM: e820: guest RAM %llu MB is too small "
               "(minimum 2 MB)\n", total_ram / (1024 * 1024));
        return -EINVAL;
    }
 
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
                        total_ram - RELM_E820_EXTENDED_BASE,
                        E820_TYPE_RAM);
    if(ret) 
        return ret;
   
    ret = relm_e820_add(map,
                        RELM_E820_BIOS_ROM_BASE,
                        RELM_E820_BIOS_ROM_SIZE,
                        E820_TYPE_RESERVED);
    if(ret)
        return ret;
 
    PDEBUG("RELM: e820: built %u-entry map for %llu MB guest\n",
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
