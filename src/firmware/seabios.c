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

    ret = request_firmware(&fw, SEABIO_FIRMWARE_NAME, dev); 
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
                                          _GFP_ZERO, page_order); 
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

    vm->fw_data->seabios_va = seabios_va; 
    vm->fw_data->seabios_pa = seabios_pa; 

    /*map seabios into guest GPA */ 
    for(offset = 0; offset < SEABIOS_SIZE; offset += PAGE_SIZE)
    {
        gpa = SEABIO_ROM_TOP_GPA + offset; 
        hpa = seabios_pa + offset; 

        ret = relm_ept_map_page(vm->ept, gpa, hpa, SEABIOS_EPT_FLAGS);
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
    if(vm->seabios_host_va)
    {
        free_pages((unsigned long)vm->seabios_host_va,
                   get_order(SEABIOS_SIZE));
        vm->seabios_host_va = NULL;
        vm->seabios_host_pa = 0;
 
        pr_info("RELM: SeaBIOS: ROM pages freed\n");
    }
}

/*set VCPU is 16-bit real mode for x86 architectural reset*/  
int relm_seabios_set_vcpu_state(struct vcpu *vcpu)
{
    uint32_t current_entry_controls; 
    uint32_t new_entry_controls;
    int ret; 


    /* SEABIOS_INITIAL_CR0 = 0x60000010
     * chaching disabled*/ 
    _vmwrite(GUEST_CR0, SEABIOS_INITIAL_CR0);
    _vmwrite(GUEST_CR4, SEABIOS_INITIAL_CR4);
    _vmwrite(GUEST_CR3, 0ULL);
    _vmwrite(VMCS_GUEST_IA32_EFER, SEABIOS_INITIAL_EFER);

    current_entry_controls = (uint32_t)__vmread(VM_ENTR)
}


