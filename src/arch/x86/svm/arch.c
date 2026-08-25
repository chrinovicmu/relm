/* SPDX-License-Identifier: GPL-2.0 */
/*
 * src/arch/x86/svm/arch.c — SVM backend's relm_arch_* wrappers.
 *
*/ 

#include <linux/kernel.h>
#include <linux/printk.h>

#include <relm/vm.h>
#include <relm/iommu.h>
#include <svm.h>
#include <npt.h>
#include <arch.h>

int relm_arch_init(void)
{
    if (!relm_svm_check_support()) {
        pr_err("RELM: CPU does not support SVM\n");
        return -ENODEV;
    }

    return relm_svm_enable_on_all_cpus();
}

void relm_arch_exit(void)
{
    relm_svm_disable_on_all_cpus();
}

void relm_arch_vm_ready(struct relm_vm *vm)
{
    if (!vm || !vm->arch.npt) {
        pr_warn("RELM: arch_vm_ready called with no NPT context\n");
        return;
    }

    pr_info("RELM: VM '%s' ready — nCR3=0x%llx\n",
            vm->vm_name, vm->arch.npt->pml4_pa);
}

bool relm_arch_iommu_enabled(struct relm_vm *vm)
{
    return vm && vm->arch.iommu.enabled;
}

