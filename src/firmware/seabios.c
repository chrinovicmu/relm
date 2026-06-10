/* SPDX-License-Identifier: GPL-2.0
 *
 * seabios.c — SeaBIOS binary loader and VCPU real-mode state initialisation
 *
 * Loads the SeaBIOS firmware binary into guest physical memory and configures
 * the VCPU's initial architectural state so it starts execution at the x86
 * reset vector (0xFFFFFFF0), which is the first instruction SeaBIOS runs.
 */
 
#include <linux/firmware.h>   
#include <linux/printk.h>
#include <linux/slab.h>       
#include <linux/gfp.h>
#include <linux/string.h>     
#include <linux/mm.h>         
 
#include <include/vmx.h>          
#include <include/vm.h>           
#include <include/vmx_ops.h>    
#include <include/ept.h>          
#include <include/vmexit.h>       
#include <include/firmware/fw_cfg.h>
#include <include/firmware/e820.h>
#include <include/firmware/seabios.h>
#include <utils/utils.h> 

int relm_seabios_load(struct relm_vm *vm, struct device *dev)
{
    const struct firmware *fw = NULL; 

    void *seabios_va = NULL; 
    uint64_t seabios_pa; 

    const int page_order = get_order(SEABIOS_SIZE); 
    uint64_t offset; 
    uint64_t gpa, hpa; 
    int ret; 

    ret = request_firmware(&fw, SEABIOS_FIRMWARE_NAME, dev); 
    if(ret)
    {
        pr_err("RELM: SeaBIOS: failed to load '%s' (error %d)\n"
               "  Copy SeaBIOS: cp /usr/share/seabios/bios.bin "
               "/lib/firmware/seabios/bios.bin\n",
               SEABIOS_FIRMWARE_NAME, ret);
        return ret;
    }

    if(fw->size != SEABIOS_SIZE)
    {
        pr_err("RELM: SeaBIOS: unexpected binary size %zu (expected %llu)\n",
               fw->size, SEABIOS_SIZE);
        release_firmware(fw);
        return -EINVAL;
    }

    pr_info("RELM: SeaBIOS: loaded %zu bytes from '%s'\n",
            fw->size, SEABIOS_FIRMWARE_NAME);


    seabios_va = (void *)__get_free_pages(GFP_KERNEL |
                                          __GFP_ZERO, page_order); 
    if(!seabios_va)
    {
        pr_err("RELM: SeaBIOS: failed to allocate %llu KB for ROM image\n",
               SEABIOS_SIZE / 1024);
        release_firmware(fw);
        return -ENOMEM;
    }

    seabios_pa = virt_to_phys(seabios_va); 
    memcpy(seabios_va, fw->data, fw->size);
    release_firmware(fw); 
    fw = NULL; 

    vm->fw_data->seabios_hva = seabios_va; 
    vm->fw_data->seabios_hpa = seabios_pa; 

    /*map seabios into guest GPA */ 
    for(offset = 0; offset < SEABIOS_SIZE; offset += PAGE_SIZE)
    {
        gpa = SEABIOS_ROM_TOP_GPA + offset; 
        hpa = seabios_pa + offset; 

        ret = relm_ept_map_page(vm->arch.ept, gpa, hpa, SEABIOS_EPT_FLAGS);
        if(ret)
        {
            pr_err("RELM: SeaBIOS: EPT map failed at GPA=0x%llx "
                   "HPA=0x%llx: %d\n", gpa, hpa, ret);
            /* Partial failure: cleanup is done by relm_seabios_unload() */
            return ret;
        }
    } 

    PDEBUG("RELM: SeaBIOS: mapped at GPA 0x%llx–0x%llx (BIOS shadow area)\n",
            SEABIOS_ROM_LOW_GPA, SEABIOS_ROM_LOW_GPA + SEABIOS_SIZE);
 
    return 0;
}

void relm_seabios_unload(struct relm_vm *vm)
{
    if(vm->fw_data->seabios_hva)
    {
        free_pages((unsigned long)vm->fw_data->seabios_hva,
                   get_order(SEABIOS_SIZE));
        vm->fw_data->seabios_hva = NULL;
        vm->fw_data->seabios_hpa = 0;
 
        pr_info("RELM: SeaBIOS: ROM pages freed\n");
    }
}

int relm_seabios_setup(struct relm_vm *vm, struct device *dev)
{
    int ret;
 
    ret = relm_seabios_load(vm, dev);
    if(ret)
    {
        pr_err("RELM: SeaBIOS setup: failed to load binary: %d\n", ret);
        return ret;
    }
 
    /* Step 2: Register all fw_cfg items SeaBIOS will read during boot */
    ret = relm_fw_cfg_setup(&vm->fw_data->fw_cfg, vm);
    if(ret)
    {
        pr_err("RELM: SeaBIOS setup: failed to setup fw_cfg: %d\n", ret);
        relm_seabios_unload(vm);
        return ret;
    }
 
    pr_info("RELM: SeaBIOS setup complete.\n");
    pr_info("RELM: VCPU real-mode state will be set in Phase 2 "
            "(relm_seabios_set_vcpu_state in vmcs_setup).\n");
    pr_info("RELM: Boot sequence:\n");
    pr_info("  1. CPU fetches 0xFFFFFFF0 → SeaBIOS reset code\n");
    pr_info("  2. SeaBIOS reads fw_cfg (ports 0x510/0x511)\n");
    pr_info("  3. SeaBIOS reads 'etc/e820' → builds memory map\n");
    pr_info("  4. SeaBIOS scans for bootable disk / direct kernel boot\n");
    pr_info("  5. OS kernel starts, reads e820 via INT 0x15, AX=0xE820\n");
 
    return 0;
}

