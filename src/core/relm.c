#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/platform_device.h> 

#include <include/vmx.h>
#include <include/vm.h>
#include <include/ept.h>
#include <include/vmx_ops.h>
#include <include/vmexit.h>
#include <include/boot/linux_loader.h>
#include <include/vmcs_state.h>
#include <utils/utils.h>

static struct relm_vm *my_vm = NULL; 
static struct platform_device *relm_pdev = NULL; 

static int __init relm_module_init(void)
{
    int ret;
    int vm_id = 1; 
    int vpid = 1;
    struct vcpu *vcpu; 

    if(!relm_vmx_support())
    {
        pr_err("RELM: VMX is not surppoted on hardware"); 
        return -ENODEV; 
    }

    ret = relm_vmx_enable_on_all_cpus();
    if (ret) 
    {
        pr_err("RELM: Failed to enable VMX on all CPUs: %d\n", ret);
        return ret;
    }
    PDEBUG("RELM: VMX enabled on all CPUs\n");

    relm_pdev = platform_device_register_simple("relm", 0, NULL, 0); 
    if(IS_ERR(relm_pdev))
    {
        ret = PTR_ERR(relm_pdev);
        relm_pdev = NULL;
        pr_err("RELM: Failed to register platform device: %d\n", ret);
        goto _cleanup_vmx;
    }

    PDEBUG("RELM: Platform device registered (/sys/devices/platform/relm.0)\n");
    
    my_vm = relm_create_vm(vm_id, "RELM-VM-01", (uint64_t)RELM_VM_GUEST_RAM_SIZE);
    if(!my_vm)
    {
        pr_err("RELM: VM creation failed (out of memory?)\n");
        ret = -ENOMEM;
        goto _cleanup_pdev;
    }
    pr_info("RELM: VM '%s' created: %llu MB guest RAM, EPT EPTP=0x%llx\n",
            my_vm->vm_name,
            my_vm->total_guest_ram / (1024 * 1024),
            my_vm->ept->eptp);


    ret = relm_iommu_init(my_vm, &relm_pdev->dev);
    if(ret)
    {
        pr_warn("RELM: IOMMU init failed (%d) — device pass-through disabled\n"
                "  (This is expected if VT-d is disabled in BIOS)\n", ret);
        /* Not fatal — continue without IOMMU. Guest can still run via EPT alone. */
        ret = 0;
    }

    /*igonored old firmware seutp *
    ret = relm_seabios_setup(my_vm, &relm_pdev->dev);
    if(ret)
    {
        pr_err("RELM: SeaBIOS setup failed: %d\n"
               "  Ensure /lib/firmware/seabios/bios.bin exists:\n"
               "    apt install seabios\n"
               "    mkdir -p /lib/firmware/seabios\n"
               "    cp /usr/share/seabios/bios.bin /lib/firmware/seabios/bios.bin\n",
               ret);
        goto _cleanup_vm;
    }
    pr_info("RELM: SeaBIOS loaded and fw_cfg populated\n");
*/ 

     /* Direct Linux/x86 boot — no SeaBIOS. We load the bzImage, build the
     * boot_params zero page (e820 + cmdline + initrd ptrs), and the VMCS
     * will enter the guest straight at startup_64 in 64-bit long mode. */
    ret = relm_linux_loader_setup(my_vm, &relm_pdev->dev, NULL);
    if(ret)
    {
        pr_err("RELM: linux_loader setup failed: %d\n"
               "  Place a bzImage at /lib/firmware/%s\n"
               "  (optional initrd at /lib/firmware/%s)\n",
               ret, RELM_KERNEL_FW_NAME, RELM_INITRD_FW_NAME);
        goto _cleanup_vm;
    }
    pr_info("RELM: linux_loader complete (kernel + boot_params in guest RAM)\n");

    if(my_vm->iommu.enabled)
    {
        ret = relm_iommu_map_guest_ram(my_vm);
        if(ret)
        {
            pr_err("RELM: IOMMU RAM mapping failed: %d\n", ret);
            goto _cleanup_vm;
        }
        pr_info("RELM: Guest RAM mirrored into IOMMU domain\n");
    }

    ret = relm_vm_add_vcpu(my_vm, vpid);
    if(ret != 0)
    {
        pr_err("RELM: Failed to add VCPU %d: %d\n", vpid, ret);
        goto _cleanup_vm;
    }
    pr_info("RELM: VCPU %d created (Phase 1 complete)\n", vpid); 


    ret = relm_run_vm(my_vm);
    if (ret != 0)
    {
        pr_err("RELM: Failed to run VM (error: %d)\n", ret);
        goto _cleanup_vm;
    }
 
    pr_info("RELM: VM '%s' is running\n", my_vm->vm_name);
 
    return 0;  

_cleanup_vm:
    relm_destroy_vm(my_vm);
    my_vm = NULL;
_cleanup_pdev:
    platform_device_unregister(relm_pdev);
    relm_pdev = NULL;
_cleanup_vmx:
    relm_vmx_disable_on_all_cpus();
    return ret;
}
 
static void __exit relm_module_exit(void)
{
    pr_info("RELM: Shutting down\n");
 
    /* Stop and destroy the VM first — IOMMU detach, device quiesce,
     * VCPU halt, RAM free all happen inside relm_destroy_vm. */
    if(my_vm)
    {
        relm_stop_vm(my_vm);
 
        if(my_vm->ops && my_vm->ops->print_stats)
            my_vm->ops->print_stats(my_vm);
 
        relm_iommu_dump_stats(my_vm);
 
        relm_destroy_vm(my_vm);
        my_vm = NULL;
    }
 
    /* Destroy the firmware-loading platform device. */
    if(relm_pdev)
    {
        platform_device_unregister(relm_pdev);
        relm_pdev = NULL;
    }
 
    /* VMXOFF on every CPU — must come last, after all VMs are destroyed.
     * Any pending VMCS must be cleared (VMCLEAR) before VMXOFF. */
    relm_vmx_disable_on_all_cpus();
    pr_info("RELM: Module unloaded\n");
}
 
module_init(relm_module_init); 
module_exit(relm_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chrinovic M");
MODULE_DESCRIPTION("A Type-1 Linux Hypervisor");
MODULE_VERSION("0.1");
