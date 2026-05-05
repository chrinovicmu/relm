/* SPDX-License-Identifier: GPL-2.0
 *
 * fw_cfg.c — QEMU fw_cfg virtual device emulation for RELM
 *
 * Emulates the QEMU fw_cfg I/O port device that SeaBIOS uses to read
 * virtual machine configuration (memory map, CPU count, boot parameters).
 * See fw_cfg.h for the full architectural background.
 */

#include <linux/printk.h>
#include <linux/slab.h>       
#include <linux/string.h>     
#include <linux/byteorder/generic.h>  
 
#include <include/vm.h>
#include <include/firmware/e820.h>
#include <include/firmware/fw_cfg.h>
#include <stdint.h>
#include <utils/utils.h>


static const uint8_t fw_cfg_signature[4] = {'Q', 'E', 'M', 'U'};

void relm_fw_cfg_init(struct fw_cfg_device *fw) 
{
    memset(fw, 0, siezeof(*fw)); 
    
    spin_lock_init(&fw->lock); 

    fw->next_file_key = FW_CFG_FILE_FIRST; 
    fw->initialized = true; 

    PDEBUG("RELM:firmware:fw_cfg: device initialised (max_items=%u)\n",
            FW_CFG_MAX_ITEMS);
}

void relm_fw_cfg_destroy(struct fw_cfg_device *fw)
{
    uint32_t i;
 
    spin_lock(&fw->lock);
 
    for(i = 0; i < FW_CFG_MAX_ITEMS; i++)
    {
        if(fw->items[i].in_use && fw->items[i].data)
        {
            kfree(fw->items[i].data);
            fw->items[i].data = NULL;
        }
    }
 
    fw->item_count = 0;
    fw->selected   = NULL;
    fw->initialised = false;
 
    spin_unlock(&fw->lock);
}

int relm_fe_cfg_register(struct fw_cfg_device *fw, 
                         uint16_t key, 
                         const char *name, 
                         const void *data, 
                         uint32_t size)
{
    struct fw_cfg_item *item; 
    uint32_t slot; 

    if(!fw || !fw->initialised)
    {
        pr_err("RELM:firmware:fw_cfg: register called on uninitialised device\n");
        return -EINVAL;
    }

    if(size > FW_CFG_MAX_ITEM_SIZE)
    {
        pr_err("RELM:firmware:fw_cfg: item size %u exceeds maximum %u\n",
               size, FW_CFG_MAX_ITEM_SIZE);
        return -EINVAL;
    }

    spin_lock(&fw->lock);
 
    if(fw->item_count >= FW_CFG_MAX_ITEMS)
    {
        spin_unlock(&fw->lock);
        pr_err("RELM:firmware:fw_cfg: item registry full (%u items)\n",
               FW_CFG_MAX_ITEMS);
        return -ENOSPC;
    }

    /* Check for duplicate key — registering the same key twice is a bug */
    for(slot = 0; slot < fw->item_count; slot++)
    {
        if(fw->items[slot].in_use && fw->items[slot].key == key)
        {
            spin_unlock(&fw->lock);
            pr_err("RELM:firmware:fw_cfg: duplicate key 0x%04x\n", key);
            return -EEXIST;
        }
    }

    slot = fw->item_count; 
    item = &fw->items[slot]; 

    if(size > 0 && data)
    {
        item->data = kmalloc(size, GFP_ATOMIC); 
        if(!item->data)
        {
            spin_unlock(&fw->lock);
            pr_err("RELM:firmware:fw_cfg: kmalloc failed for item key=0x%04x "
                   "size=%u\n", key, size);
            return -ENOMEM;
        }

        memcpy(item->data, data, size); 
    }
    else {
        item->data = NULL 
    }

    item->key = key; 
    item->size = size; 
    item->cursor = 0; 
    item->in_use = true; 
    
    /* Copy the name if provided. Well-known items have no name (""). */
    if(name && name[0])
        strncpy(item->name, name, FW_CFG_MAX_NAME_LEN);
    else
        item->name[0] = '\0';
 
    fw->item_count++;
 
    spin_unlock(&fw->lock);
 
    PDEBUG("RELM:firmware:fw_cfg: registered key=0x%04x name='%s' size=%u\n",
           key, item->name[0] ? item->name : "(well-known)", size);
 
    return 0;
}

int relm_fw_cfg_register_file(struct fw_cfg_device *fw, 
                              const char *name, 
                              const void *data, 
                              uint32_t size, 
                              uint16_t *key_out)
{
    uint16_t key; 
    int ret; 
    
    if(!name || !name[0])
    {
        pr_err("RELM:firmware:fw_cfg: register_file: empty name\n");
        return -EINVAL;
    }

    if(strlen(name) > FW_CFG_MAX_NAME_LEN)
    {
        pr_err("RELM:firmware:fw_cfg: filename '%s' exceeds %u characters\n",
               name, FW_CFG_MAX_NAME_LEN);
        return -EINVAL;
    }

    spin_lock(&fw->lock); 
    key = fw->next_file_key++; 
    spin_unlock(&fw->lock); 

    ret = relm_fw_cfg_register(fw, key, name, data, size);
    if(ret)
        return ret;
 
    if(key_out)
        *key_out = key;
 
    pr_info("RELM:firmware:fw_cfg: named file '%s' registered as key=0x%04x "
            "(%u bytes)\n", name, key, size);
 
    return 0;
}

static int relm_fw_cfg_build_file_dir(struct fw_cfg_device *fw)
{
    uint32_t n_files = 0; 
    uint32_t i; 
    uint32_t dir_size; 
    struct fw_cfg_file_dir *dir; 
    int ret; 

    for(i = 0; i < fw->item_count; i++)
    {
        if(fw->items[i].in_use &&
           fw->items[i].key >= FW_CFG_FILE_FIRST)
            n_files++; 
    }

    dir_size = sizeof(struct fw_cfg_file_dir) + 
        n_files * sizeof(struct fw_cfg_file); 

    dir = kzalloc(dir_size, GFP_KERNEL); 
    if(!dir)
    {
        pr_err("RELM:firmware:fw_cfg: failed to allocate file directory buffer "
               "(%u bytes)\n", dir_size);
        return -ENOMEM;
    }

    dir->count = cpu_to_be32(n_files); 
    
    uint32_t idx = 0; 
    for(i = 0; i < fw->item_count; ++i)
    {
        if(!fw->items[i].in_use ||
           fw->items[i].key < FW_CFG_FILE_FIRST)
            continue;

        struct fw_cfg_file *entry = &dir->files[idx]; 
        entry->size = cpu_to_be32(fw->items[i].size); 
        entry->select = cpu_to_be16(fw->items[i].key); 
        entry->reserved = cpu_to_be16(0); 

        strscpy(entry->name,
                fw->items[i].name,
                sizeof(entry->name));

        PDEBUG("RELM:firmware:fw_cfg: dir entry '%s' key=0x%04x size=%u\n",
               entry->name,
               fw->items[i].key,
               fw->items[i].size);
        idx++; 
    }

    ret = relm_fw_cfg_register(fw, FW_CFG_FILE_DIR, NULL, dir, dir_size);

    kfree(dir);

    if (ret && ret != -EEXIST) {
        pr_err("RELM: fw_cfg: failed to register FILE_DIR: %d\n", ret);
        return ret;
    }

    pr_info("RELM:firmware:fw_cfg: file directory built (%u files, %zu bytes)\n",
            n_files, dir_size);

    return 0;
}

int relm_fw_cfg_setup(struct fw_cfg_device *fw, struct relm_vm *vm)
{
    uint32_t ram_size_le; 
    uint16_t nb_cpus_le; 
    uint16_t max_cpus_le; 
    uint16_t nographic; 
    uint16_t boot_menu; 
    uint8_t boot_device; 
    uint32_t fw_cfg_id; 
    uint8_t uuid[16]; 
    int ret; 

    if(!fw->initialised)
    {
        pr_err("RELM:firmware:fw_cfg: setup called on uninitialised device\n");
        return -EINVAL;
    }

    ret = relm_fw_cfg_register(fw, FW_CFG_SIGNATURE, NULL,
                                fw_cfg_signature, sizeof(fw_cfg_signature));
    if(ret) 
        return ret;

    fw_cfg_id = 0x1;  /* bit 0 = I/O interface supported, bit 1 = 0 no DMA */
    ret = relm_fw_cfg_register(fw, FW_CFG_ID, NULL,
                                &fw_cfg_id, sizeof(fw_cfg_id));
    if(ret)
        return ret;

    memset(uuid, 0, sizeof(uuid));
    ret = relm_fw_cfg_register(fw, FW_CFG_UUID, NULL, uuid, sizeof(uuid));
    if(ret) return ret;

    ram_size_le = (uint32_t)vm->total_guest_ram;  /* fits in 32 bits for ≤4 GB */
    ret = relm_fw_cfg_register(fw, FW_CFG_RAM_SIZE, NULL,
                                &ram_size_le, sizeof(ram_size_le));
    if(ret) 
        return ret;

    nographic = 1;
    ret = relm_fw_cfg_register(fw, FW_CFG_NOGRAPHIC, NULL,
                                &nographic, sizeof(nographic));
    if(ret) 
        return ret;

    nb_cpus_le = (uint16_t)vm->online_vcpus;
    if(nb_cpus_le == 0) 
        nb_cpus_le = 1;  /* always at least 1 CPU */
    ret = relm_fw_cfg_register(fw, FW_CFG_NB_CPUS, NULL,
                                &nb_cpus_le, sizeof(nb_cpus_le));
    if(ret)
        return ret;

    uint16_t machine_id = 1;
    ret = relm_fw_cfg_register(fw, FW_CFG_MACHINE_ID, NULL,
                                &machine_id, sizeof(machine_id));
    if(ret) 
        return ret;

    /*this is for direct kernel boot. for now we register them as 0 (disabled)
     * to enable we will need to set KERNEL_ADDR to the GPA where the kernel is loaded,
     * KERNEL_SIZE as it's size. KERNEL)CMDLINE to the GPA of the command line string, 
     * and INITRD_ADDR/SIZE similarly*/ 
    uint32_t zero32 = 0;
    ret = relm_fw_cfg_register(fw, FW_CFG_KERNEL_ADDR,    NULL, &zero32, 4);
    if(ret) 
        return ret;
    ret = relm_fw_cfg_register(fw, FW_CFG_KERNEL_SIZE,    NULL, &zero32, 4);
    if(ret) 
        return ret;
    ret = relm_fw_cfg_register(fw, FW_CFG_KERNEL_CMDLINE, NULL, &zero32, 4);
    if(ret) 
        return ret;
    ret = relm_fw_cfg_register(fw, FW_CFG_INITRD_ADDR,    NULL, &zero32, 4);
    if(ret)
        return ret;
    ret = relm_fw_cfg_register(fw, FW_CFG_INITRD_SIZE,    NULL, &zero32, 4);
    if(ret) 
        return ret;


    /*boot device : 'c' for first hard disk*/ 
    boot_device = 'c';
    ret = relm_fw_cfg_register(fw, FW_CFG_BOOT_DEVICE, NULL,
                                &boot_device, sizeof(boot_device));
    if(ret) 
        return ret;

    /*ignore NUMA configuration for now, we leave item empty*/ 
    ret = relm_fw_cfg_register(fw, FW_CFG_NUMA, NULL, NULL, 0);
    if(ret) return ret;


    /*skip boot device selection menu*/ 
    boot_menu = 0;
    ret = relm_fw_cfg_register(fw, FW_CFG_BOOT_MENU, NULL,
                                &boot_menu, sizeof(boot_menu));
    if(ret)
        return ret;

    max_cpus_le = nb_cpus_le;
    ret = relm_fw_cfg_register(fw, FW_CFG_MAX_CPUS, NULL,
                                &max_cpus_le, sizeof(max_cpus_le));
    if(ret) 
        return ret;

    ret = relm_e820_build(vm, &fw->e820_map);
    if(ret)
    {
        pr_err("RELM:firmware:fw_cfg: failed to build e820 map: %d\n", ret);
        return ret;
    }
 
    relm_e820_dump(&fw->e820_map);

    ret = relm_fw_cfg_register_file(fw, "etc/e820",
                                     fw->e820_map.entries,
                                     fw->e820_map.count * sizeof(struct e820_entry),
                                     NULL);
    if(ret) 
        return ret;

    /*0 miliseconds, halt immediately if bootaboe device is not found*/ 
    uint32_t boot_fail_wait = 0;
    ret = relm_fw_cfg_register_file(fw, "etc/boot-fail-wait",
                                     &boot_fail_wait, sizeof(boot_fail_wait),
                                     NULL);
    if(ret) 
        return ret;

    ret = relm_fw_cfg_build_file_dir(fw);
    if(ret) return ret;
 
    pr_info("RELM:firmware:fw_cfg: setup complete (%u items registered)\n",
            fw->item_count);
 
    return 0;
}

void relm_fw_cfg_select(struct fw_cfg_device *fw, uint16_t key)
{
    uint32_t i;
 
    spin_lock(&fw->lock);
    fw->selected = NULL;
 
    for(i = 0; i < fw->item_count; i++)
    {
        if(fw->items[i].in_use && fw->items[i].key == key)
        {
            fw->selected = &fw->items[i];
            fw->items[i].cursor = 0;
 
            PDEBUG("RELM:firmware:fw_cfg: selected key=0x%04x '%s' "
                   "(%u bytes)\n",
                   key,
                   fw->items[i].name[0] ? fw->items[i].name : "(well-known)",
                   fw->items[i].size);
            break;
        }
    }
 
    if(!fw->selected)
    {
        PDEBUG("RELM: fw_cfg: select key=0x%04x — unknown item "
               "(reads will return 0)\n", key);
    }
 
    spin_unlock(&fw->lock);
}
 
uint8_t relm_fw_cfg_read(struct fw_cfg_device *fw)
{
    uint8_t byte = 0;
 
    spin_lock(&fw->lock);
 
    if(fw->selected &&
       fw->selected->data &&
       fw->selected->cursor < fw->selected->size)
    {
        byte = fw->selected->data[fw->selected->cursor];
        fw->selected->cursor++;
    }
    spin_unlock(&fw->lock);
 
    return byte;
}

int relm_fw_cfg_handle_io(struct fw_cfg_device *fw,
                           uint16_t port, bool is_write,
                           uint32_t size, uint32_t *value)
{
    switch(port)
    {
        case FW_CFG_PORT_SEL:
    
            if(is_write)
            {
                uint16_t key = (uint16_t)(*value & 0xFFFF);
                relm_fw_cfg_select(fw, key);
            }
            else {
                *value = 0;
            }
            break;
 
        case FW_CFG_PORT_DATA:

            if(!is_write)
            {
                uint32_t result = 0;
                uint32_t b;
 
                for(b = 0; b < size; b++)
                {
                    /* Read next byte and place it in byte position b.
                     * Little-endian: byte 0 goes to bits [7:0], etc. */
                    result |= ((uint32_t)relm_fw_cfg_read(fw)) << (b * 8);
                }
 
                *value = result;
            }
            /* writes to 0x511 are silently ignored */
            break;
 
        default:
            pr_warn("RELM:firmware:fw_cfg: unexpected port 0x%04x\n", port);
            break;
    }
 
    return 1;
}
 
