#include <linux/kernel.h>
#include <linux/printk.h>

#include <relm/vm.h>
#include <relm/iommu.h>
#include <vmx.h>
#include <ept.h>
#include <arch.h>

int relm_arch_init(void)
{
    if (!relm_vmx_support()) {
        pr_err("RELM: CPU does not support VMX\n");
        return -ENODEV;
    }

    return relm_vmx_enable_on_all_cpus();
}

void relm_arch_exit(void)
{
    relm_vmx_disable_on_all_cpus();
}

void relm_arch_vm_ready(struct relm_vm *vm)
{
    if (!vm || !vm->arch.ept) {
        pr_warn("RELM: arch_vm_ready called with no EPT context\n");
        return;
    }

    pr_info("RELM: VM '%s' ready — EPTP=0x%llx\n",
            vm->vm_name, vm->arch.ept->eptp);
}

bool relm_arch_iommu_enabled(struct relm_vm *vm)
{
    return vm && vm->arch.iommu.enabled;
}


