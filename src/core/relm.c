/* SPDX-License-Identifier: GPL-2.0 */
/*
 * src/core/module.c — RELM hypervisor kernel module entry point.
 */ 

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/printk.h>

#include <include/relm/vm.h>       
#include <include/relm/vcpu.h>     
#include <include/iommu.h>         
#include <include/boot/linux_loader.h>  

#include <relm_arch/arch.h>
#include <utils/utils.h>

static struct relm_vm *my_vm = NULL; 
static struct platform_device *relm_pdev = NULL; 

static int __init relm_module_init(void)
{
    int ret; 
    const int vm_id = 1; 
    const int vpid = 1; 
 
    pr_info("RELM: loading (ARCH=%s)\n",
#ifdef RELM_ARCH_NAME
            RELM_ARCH_NAME
#else
            "unknown"
#endif
    );

    /*Arch initialization*/ 
    ret = relm_arch_init();
    if (ret) 
    {
        pr_err("RELM: arch init failed: %d\n", ret);
        return ret;
    }
    pr_info("RELM: arch init complete\n");

    relm_pdev = platform_device_register_simple("relm", 0, NULL, 0);
    if (IS_ERR(relm_pdev)) 
    {
        ret = PTR_ERR(relm_pdev);
        relm_pdev = NULL;
        pr_err("RELM: platform device registration failed: %d\n", ret);
        goto _cleanup_arch;
    }
    pr_info("RELM: platform device registered "
            "(/sys/devices/platform/relm.0)\n");

    my_vm = relm_create_vm(vm_id, "RELM-VM-01",
                           (uint64_t)RELM_VM_GUEST_RAM_SIZE);
    if (!my_vm) 
    {
        pr_err("RELM: VM creation failed\n");
        ret = -ENOMEM;
        goto _cleanup_pdev;
    }
    pr_info("RELM: VM '%s' created (%llu MB guest RAM)\n",
            my_vm->vm_name,
            my_vm->total_guest_ram / (1024ULL * 1024ULL));

    /* Let the arch layer log its own state (EPTP on x86, VTTBR on arm64)*/
    relm_arch_vm_ready(my_vm);

    /*IOMMU initialization*/ 
    ret = relm_iommu_init(my_vm, &relm_pdev->dev);
    if (ret) 
    {
        pr_warn("RELM: IOMMU init failed (%d) — "
                "device passthrough disabled\n"
                "  (expected if VT-d/SMMU is disabled in firmware)\n",
                ret);
        /*
         * Not fatal. Clear ret so the init sequence continues.
         * The guest runs without IOMMU protection. Passthrough
         * devices won't be available to it.
         */
        ret = 0;
    }

    /*load the linux guest kernel */ 
    ret = relm_linux_loader_setup(my_vm, &relm_pdev->dev, NULL);
    if (ret) {
        pr_err("RELM: linux loader failed: %d\n"
               "  Place a bzImage at /lib/firmware/%s\n"
               "  (optional initrd at /lib/firmware/%s)\n",
               ret,
               RELM_KERNEL_FW_NAME,
               RELM_INITRD_FW_NAME);
        goto _cleanup_vm;
    }
    pr_info("RELM: linux loader complete "
            "(kernel + boot_params in guest RAM)\n");

    /*If IOMMU is active, mirroe every guest RAM pgae into 
    * IOMMU domain so that passthrough devices can DMA to 
    * guest physical addresses */ 
    if (relm_arch_iommu_enabled(my_vm)) {
        ret = relm_iommu_map_guest_ram(my_vm);
        if (ret) {
            pr_err("RELM: IOMMU guest RAM mapping failed: %d\n", ret);
            goto _cleanup_vm;
        }
        pr_info("RELM: guest RAM mirrored into IOMMU domain\n");
    }

    /*add first vCPU (phase 1)*/ 
    ret = relm_vm_add_vcpu(my_vm, vpid);
    if (ret != 0) {
        pr_err("RELM: failed to add VCPU %d: %d\n", vpid, ret);
        goto _cleanup_vm;
    }
    pr_info("RELM: VCPU %d created (Phase 1 complete)\n", vpid);

    /*start the VM*/ 
    ret = relm_run_vm(my_vm);
    if (ret != 0) {
        pr_err("RELM: failed to run VM: %d\n", ret);
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

_cleanup_arch:
    relm_arch_exit();
    return ret;
}
static void __exit relm_module_exit(void)
{
    pr_info("RELM: shutting down\n");

    if (my_vm) {
        relm_stop_vm(my_vm);
        if (my_vm->ops && my_vm->ops->print_stats)
            my_vm->ops->print_stats(my_vm);

        relm_iommu_dump_stats(my_vm);
        relm_destroy_vm(my_vm);
        my_vm = NULL;
    }

    if (relm_pdev) {
        platform_device_unregister(relm_pdev);
        relm_pdev = NULL;
    }
    relm_arch_exit();

    pr_info("RELM: unloaded\n");
}

module_init(relm_module_init);
module_exit(relm_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chrinovic Mukanya");
MODULE_DESCRIPTION("RELM — A Multi-arch Type-1 Linux hypervisor");
MODULE_VERSION("0.2");
