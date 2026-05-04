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
#include <include/firmaware/e820.h>
#include <include/firmaware/fw_cfg.h>
#include <stdint.h>
#include <utils/utils.h>


static const uint8_t fw_cfg_signature[4] = {'Q', 'E', 'M', 'U'};

void relm_fw_cfg_init(struct fw_cfg_device *fw) 
{
    memset(fw, 0, siezeof(*fw)); 
    
    spin_lock_init(&fw->lock); 

    fw->next_file_key = FW_CFG_FILE_FIRST; 
    fw->initialized = true; 

    PDEBUG("RELM: fw_cfg: device initialised (max_items=%u)\n",
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
        pr_err("RELM: fw_cfg: register called on uninitialised device\n");
        return -EINVAL;
    }

    if(size > FW_CFG_MAX_ITEM_SIZE)
    {
        pr_err("RELM: fw_cfg: item size %u exceeds maximum %u\n",
               size, FW_CFG_MAX_ITEM_SIZE);
        return -EINVAL;
    }

    spin_lock(&fw->lock);
 
    if(fw->item_count >= FW_CFG_MAX_ITEMS)
    {
        spin_unlock(&fw->lock);
        pr_err("RELM: fw_cfg: item registry full (%u items)\n",
               FW_CFG_MAX_ITEMS);
        return -ENOSPC;
    }

    /* Check for duplicate key — registering the same key twice is a bug */
    for(slot = 0; slot < fw->item_count; slot++)
    {
        if(fw->items[slot].in_use && fw->items[slot].key == key)
        {
            spin_unlock(&fw->lock);
            pr_err("RELM: fw_cfg: duplicate key 0x%04x\n", key);
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
            pr_err("RELM: fw_cfg: kmalloc failed for item key=0x%04x "
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
 
    PDEBUG("RELM: fw_cfg: registered key=0x%04x name='%s' size=%u\n",
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
        pr_err("RELM: fw_cfg: register_file: empty name\n");
        return -EINVAL;
    }

    if(strlen(name) > FW_CFG_MAX_NAME_LEN)
    {
        pr_err("RELM: fw_cfg: filename '%s' exceeds %u characters\n",
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
 
    pr_info("RELM: fw_cfg: named file '%s' registered as key=0x%04x "
            "(%u bytes)\n", name, key, size);
 
    return 0;
}





