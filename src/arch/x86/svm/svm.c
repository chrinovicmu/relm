#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <asm/processor.h>   
#include <asm/msr.h>         

#include <svm.h>
#include <utils/utils.h>

bool relm_svm_check_support(void)
{
    uint32_t ecx, edx;
    uint64_t vm_cr;
    int cpu = smp_processor_id();

    /* Gate 1: does the silicon have SVM at all. */
    ecx = cpuid_ecx(SVM_CPUID_EXT_FEATURES);
    if (!(ecx & SVM_CPUID_FEATURE_BIT)) {
        pr_err("RELM: CPU%d: SVM not supported by CPUID (0x80000001 ECX=0x%x)\n",
               cpu, ecx);
        return false;
    }

    edx = cpuid_edx(SVM_CPUID_SVM_FEATURES);

    vm_cr = 0;
    rdmsrl(MSR_VM_CR, vm_cr);

    if (vm_cr & MSR_VM_CR_SVMDIS) {
        if (vm_cr & MSR_VM_CR_LOCK)
            pr_err("RELM: CPU%d: SVM disabled AND locked by BIOS "
                   "(VM_CR=0x%llx) — cannot be re-enabled until reboot\n",
                   cpu, vm_cr);
        else
            pr_err("RELM: CPU%d: SVM currently disabled by BIOS "
                   "(VM_CR=0x%llx)\n", cpu, vm_cr);
        return false;
    }

    PDEBUG("RELM: CPU%d: SVM support verified (ext_features_edx=0x%x, VM_CR=0x%llx)\n",
           cpu, edx, vm_cr);

    return true;
}

void relm_enable_svm_operation(void)
{
    uint64_t efer = 0;
    int cpu = smp_processor_id();

    rdmsrl(MSR_EFER, efer);

    if (!(efer & EFER_SVME)) {
        efer |= EFER_SVME;
        wrmsrl(MSR_EFER, efer);
    }

    /* Verify — mirrors relm_enable_vmx_operation()'s CR4 re-read. */
    efer = 0;
    rdmsrl(MSR_EFER, efer);
    if (!(efer & EFER_SVME)) {
        pr_err("RELM: CPU%d: EFER.SVME did not stick (EFER=0x%llx)\n",
               cpu, efer);
        return;
    }

    pr_info("RELM: CPU%d: SVM operation enabled\n", cpu);
}

struct svm_enable_work {
    atomic_t failed_cpus;
};

static void relm_svm_enable_cpu(void *arg)
{
    struct svm_enable_work *work = arg;
    int cpu = smp_processor_id();

    if (!relm_svm_check_support()) {
        pr_err("RELM: CPU%d: SVM support/permission check failed, "
               "cannot enable\n", cpu);
        atomic_inc(&work->failed_cpus);
        return;
    }

    relm_enable_svm_operation();
}

static void relm_svm_disable_cpu(void *unused)
{
    uint64_t efer = 0;
    int cpu = smp_processor_id();

    (void)unused;

    rdmsrl(MSR_EFER, efer);

    if (efer & EFER_SVME) {
        efer &= ~EFER_SVME;
        wrmsrl(MSR_EFER, efer);
    }

    PDEBUG("RELM: CPU%d: SVM operation disabled\n", cpu);
}

int relm_svm_enable_on_all_cpus(void)
{
    struct svm_enable_work work;

    atomic_set(&work.failed_cpus, 0);

    pr_info("RELM: Enabling SVM on all %d online CPUs\n",
            num_online_cpus());

    on_each_cpu(relm_svm_enable_cpu, &work, 1);

    if (atomic_read(&work.failed_cpus) > 0) {
        pr_err("RELM: SVM enable failed on %d CPU(s)\n",
               atomic_read(&work.failed_cpus));
        relm_svm_disable_on_all_cpus();
        return -EIO;
    }

    pr_info("RELM: SVM enabled on all CPUs\n");

    return 0;
}

void relm_svm_disable_on_all_cpus(void)
{
    pr_info("RELM: Disabling SVM on all CPUs\n");

    on_each_cpu(relm_svm_disable_cpu, NULL, 1);

    pr_info("RELM: SVM disabled on all CPUs\n");
}
