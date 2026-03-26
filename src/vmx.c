#include <asm/processor-flags.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/smp.h> 
#include <linux/kthread.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/io.h>
#include <asm/desc.h> 

#include <include/vmx.h>
#include <include/vm.h>
#include <include/ept.h>
#include <include/vmx_ops.h>
#include <include/vmexit.h>
#include <include/apic.h>
#include <include/vmcs_state.h>
#include <utils/utils.h>

static DEFINE_PER_CPU(struct host_cpu *, relm_per_cpu_hcpu); 
struct host_cpu *relm_get_per_cpu_hcpu(void)
{
    return this_cpu_read(relm_per_cpu_hcpu); 
}

static int relm_vmxon(struct host_cpu *hcpu);
static int relm_vmxoff(struct host_cpu *hcpu);
static int relm_setup_vmxon_region(struct host_cpu *hcpu);
static int relm_setup_vmcs_region(struct vcpu *vcpu);
static int relm_setup_io_bitmap(struct vcpu *vcpu);
static int relm_setup_msr_bitmap(struct vcpu *vcpu);
static int relm_setup_msr_areas(struct vcpu *vcpu,
                               const uint32_t *vmexit_list,  size_t vmexit_count,
                               const uint32_t *vmentry_list, const uint64_t *vmentry_values,
                               size_t vmentry_count);

static int relm_vcpu_setup_msr_state(struct vcpu *vcpu);
static int relm_setup_exec_controls(struct vcpu *vcpu);
static int vmx_apply_exec_controls(struct vcpu *vcpu);
static void vmx_init_exec_controls(struct vcpu *vcpu);

static void relm_free_io_bitmap(struct vcpu *vcpu);
static void relm_free_msr_bitmap(struct vcpu *vcpu);
static void relm_free_vmcs_region(struct vcpu *vcpu);
static void relm_free_all_msr_areas(struct vcpu *vcpu);
static void relm_free_vmxon_region(struct host_cpu *hcpu);

static struct msr_entry *alloc_msr_entry(size_t n_entries);
static void free_msr_area(struct msr_entry *area, size_t n_entries);
static void populate_msr_load_area(struct msr_entry *area, size_t count,
                                   const uint32_t *indices, const uint64_t *values);
static void populate_msr_store_area(struct msr_entry *area, size_t count, 
                                    const uint32_t *indices);

static inline bool relm_vcpu_io_bitmap_enabled(struct vcpu *vcpu);
static inline bool relm_vcpu_msr_bitmap_enabled(struct vcpu *vcpu);
static inline unsigned int msr_area_order(size_t bytes);

const uint32_t relm_vmexit_msr_indices[] = {
    MSR_IA32_EFER,
    MSR_IA32_STAR,
    MSR_IA32_LSTAR,
    MSR_IA32_CSTAR,
    MSR_IA32_FMASK,
    MSR_IA32_FS_BASE,
    MSR_IA32_GS_BASE
};

#define RELM_VMEXIT_MSR_COUNT \ 
    ARRAY_SIZE(relm_vmexit_msr_indices)

const uint32_t relm_vmentry_msr_indices[] = {
    MSR_IA32_EFER,
    MSR_IA32_STAR,
    MSR_IA32_LSTAR,
    MSR_IA32_CSTAR,
    MSR_IA32_FMASK,
    MSR_IA32_FS_BASE,
    MSR_IA32_GS_BASE
};

#define RELM_VMENTRY_MSR_COUNT \
    ARRAY_SIZE(relm_vmentry_msr_indices)

uint64_t relm_vmentry_msr_values[RELM_VMENTRY_MSR_COUNT]; 


inline bool relm_vmx_support(void) 
{
    unsigned int eax = 1, ebx, ecx = 0, edx;

    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(eax)
        : "memory"
    );  

    PDEBUG("RELM: CPUID(1) ECX=0x%x\n", ecx);

    return (ecx & (1 << 5)) != 0;  /* VMX bit */
}

inline void relm_enable_vmx_operation(void)
{
    uint64_t cr4;

    /* Read CR4 safely */
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    /* Only set VMXE if not already set */
    if (!(cr4 & (1UL << 13))) {
        cr4 |= (1UL << 13);
        asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    }

    /* Optional verification (for debug) */
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (!(cr4 & (1UL << 13)))
        pr_err("RELM:  not set on CPU %d\n", smp_processor_id());

    pr_info("RELM: VMX operation enabled\n"); 
}

bool relm_setup_feature_control(void)
{
    uint64_t fc;

    fc = __rdmsr1(MSR_IA32_FEATURE_CONTROL);

    const uint64_t required =
        IA32_FEATURE_CONTROL_LOCKED |
        IA32_FEATURE_CONTROL_MSR_VMXON_ENABLE_OUTSIDE_SMX;

    /* MSR already locked — just validate. */
    if (fc & IA32_FEATURE_CONTROL_LOCKED) {
        if (!(fc & IA32_FEATURE_CONTROL_MSR_VMXON_ENABLE_OUTSIDE_SMX)) {
            pr_err("RELM: CPU%d: feature control locked, VMXON not enabled\n",
                   smp_processor_id());
            return false;
        }
        return true;
    }

    uint64_t new_val = fc | required;
    __wrmsr(MSR_IA32_FEATURE_CONTROL,
            (uint32_t)(new_val & 0xFFFFFFFFULL),
            (uint32_t)((new_val >> 32) & 0xFFFFFFFFULL));

    fc = __rdmsr1(MSR_IA32_FEATURE_CONTROL);
    if ((fc & required) != required) {
        pr_err("RELM: CPU%d: failed to configure IA32_FEATURE_CONTROL\n",
               smp_processor_id());
        return false;
    }

    return true;
}

struct vmx_enable_work{
    atomic_t failed_cpus; 
}; 

static void relm_vmx_enable_cpu(void *arg)
{
    struct vmx_enable_work *work = arg; 
    int cpu = smp_processor_id(); 
    struct host_cpu *hcpu= per_cpu(relm_per_cpu_hcpu, cpu);

    if(!hcpu)
    {
        pr_err("RELM: CPU%d : no host_cpu pre_allocated, cannot enable VMX\n", 
               cpu); 
        atomic_inc(&work->failed_cpus); 
        return; 
    }

    relm_enable_vmx_operation(); 

    if(!relm_setup_feature_control()){
        pr_err("RELM: CPU%d: feature control setup failed\n", cpu); 
        atomic_inc(&work->failed_cpus); 
        return; 
    }

    if(relm_vmxon(hcpu) != 0)
    {
        pr_err("RELM: CPU%d: feature control setup failed\n", cpu); 
        atomic_inc(&work->failed_cpus); 
        return; 
    }

    pr_info("RELM: CPU%d: VMX enabled successfully\n", cpu);

}

static void relm_vmx_disable_cpu(void *unused)
{
    int cpu = smp_processor_id();
    struct host_cpu *hcpu = per_cpu(relm_per_cpu_hcpu, cpu);

    if (!hcpu)
        return;

    if (relm_vmxoff(hcpu) != 0)
        pr_err("RELM: CPU%d: VMXOFF failed\n", cpu);

    uint64_t cr4;
    __asm__ __volatile__
        ("mov %%cr4, %0"
        : "=r"(cr4));

    cr4 &= ~(1UL << 13); /* clear VMXE */
    __asm__ __volatile__(
        "mov %0, %%cr4" 
        :: "r"(cr4) : "memory");
}

int relm_vmx_enable_on_all_cpus(void)
{
    struct vmx_enable_work work; 
    struct host_cpu *hcpu; 
    int cpu; 
    int ret = 0; 

    atomic_set(&work.failed_cpus, 0); 

    pr_info("RELM: Enabling VMX on all %d online CPUs\n",
            num_online_cpus());

    for_each_online_cpu(cpu){

        hcpu = kzalloc(sizeof(*hcpu), GFP_KERNEL);
        if(!hcpu)
        {
            pr_err("RELM: CPU%d: failed to allocate host_cpu\n", 
                   cpu); 
            ret = -ENOMEM; 
            goto _cleanup; 
        }

        hcpu->logical_cpu_id = cpu; 
        spin_lock_init(&hcpu->lock); 

        if(relm_setup_vmxon_region(hcpu) != 0)
        {
            pr_err("RELM: CPU%d: failed to allocate VMXON region\n", 
                   cpu); 
            kfree(hcpu); 
            ret = -ENOMEM; 
            goto _cleanup; 
        }

        /*store in per-cpu varaible. the IPI handler reads this.*/ 
        per_cpu(relm_per_cpu_hcpu, cpu) = hcpu;  
    }

    on_each_cpu(relm_vmx_enable_cpu, &work, 1); 

    if(atomic_read(&work.failed_cpus) > 0)
    {
        pr_err("RELM: VMX enable failed on %d CPU(s)\n"), 
               atomic_read(&work.failed_cpus); 
        ret = -EIO; 
        goto _cleanup; 
    }

    pr_info("RELM: VMX enabled\n on all CPUS\n"); 

    return 0; 

_cleanup:

    relm_vmx_disable_on_all_cpus(); 
    return ret; 
}

void relm_vmx_disable_on_all_cpus(void)
{
    int cpu; 
    struct host_cpu *hcpu; 

    pr_info("RELM: Disablingi VMX on all CPUs"); 

    on_each_cpu(relm_vmx_disable_cpu, NULL, 1); 

    for_each_online_cpu(cpu)
    {
        hcpu = per_cpu(relm_per_cpu_hcpu, cpu); 
        if(!hcpu)
            continue; 

        relm_free_vmxon_region(hcpu); 
        kfree(hcpu); 
        per_cpu(relm_per_cpu_hcpu, cpu) = NULL; 
    }

    pr_info("RELM: VMX disabled on all CPUs\n"); 

}

/*pin vcpu thread to a specific physical host cpu for cpu affinity
*/  
int relm_vcpu_pin_to_cpu(struct vcpu *vcpu, int target_cpu_id)
{
    int ret;
    cpumask_t new_mask; 

    if(!vcpu || !vcpu->host_task)
    {
        pr_err("RELM: VCPU%d: cannot pin - no host task assigned.\n", 
               vcpu? vcpu->vpid : -1); 
        return -EINVAL; 
    }

     if(!cpu_possible(target_cpu_id))
    {
        pr_err("RELM: VCPU%d: target CPU%d is not possible\n",
               vcpu->vpid, target_cpu_id);
        return -EINVAL;
    }

     if(!cpu_online(target_cpu_id))
    {
        pr_err("RELM: VCPU%d: target CPU%d is not online\n",
               vcpu->vpid, target_cpu_id);
        return -EINVAL;
    }

    PDEBUG("RELM: VCPU%d: pinning to CPU%d (currently on CPU%d)\n",
           vcpu->vpid, target_cpu_id, smp_processor_id());

    cpumask_clear(&new_mask); 
    cpumask_set_cpu(target_cpu_id, &new_mask); 

    /* restrict task to target_cpu_id only. if we are not currently on that
     * CPU the scheduler will migrate us on the next preemption point. */
    ret = set_cpus_allowed_ptr(vcpu->host_task, &new_mask);
    if(ret != 0)
    {
        pr_err("RELM: VCPU%d: set_cpus_allowed_ptr to CPU%d failed: %d\n",
               vcpu->vpid, target_cpu_id, ret);
        return ret;
    }

    schedule(); 

    while(smp_processor_id() != target_cpu_id)
    {
        pr_warn("RELM: VCPU%d: not yet on CPU%d (on CPU%d), retrying\n",
                vcpu->vpid, target_cpu_id, smp_processor_id());
        schedule();
    }

    pr_info("RELM: VCPU%d: pinned to CPU%d\n", vcpu->vpid, smp_processor_id());

    return ret; 
}


void relm_vcpu_unpin_and_stop(struct vcpu *vcpu)
{
    if(!vcpu || !vcpu->host_task)
        return;

    PDEBUG("RELM: VCPU%d: unpinning and stopping\n", vcpu->vpid);

    set_cpus_allowed_ptr(vcpu->host_task, cpu_online_mask);
    kthread_stop(vcpu->host_task);
}

/*IO bitmap bit in the execution controls need to be set in order to use io bimaps*/ 
static inline bool relm_vcpu_io_bitmap_enabled(struct vcpu *vcpu)
{
    return vcpu->controls.primary_proc & VMCS_PROC_USE_IO_BITMAPS; 
}

/*MSR bitmap bit in the execution controls need to be set in order to use msr bimaps */ 
static inline bool relm_vcpu_msr_bitmap_enabled(struct vcpu *vcpu)
{
    return vcpu->controls.primary_proc & VMCS_PROC_USE_MSR_BITMAPS; 
}

static inline bool relm_vcpu_ept_enabled(struct vcpu *vcpu)
{
    return (vcpu->controls.secondary_proc & VMCS_PROC2_ENABLE_EPT) != 0;
}

static int relm_setup_vmxon_region(struct host_cpu *hcpu)
{
    if(!hcpu)
        return -EINVAL; 

    /*allocate one page, page-aligned, zeroed */ 
    hcpu->vmxon = (struct vmxon_region *)
        __get_free_page(GFP_KERNEL | __GFP_ZERO); 

    if(!hcpu->vmxon)
        return -ENOMEM; 

    /*set VMX revision identifier */ 
    *(uint32_t *)hcpu->vmxon = _vmcs_revision_id(); 
    hcpu->vmxon_pa = virt_to_phys(hcpu->vmxon); 

    PDEBUG("VMXON region physicall address : 0x%llx\n", hcpu->vmxon_pa);     
    return 0; 
}

static int relm_vmxon(struct host_cpu *hcpu)
{
    uint64_t rflags; 

    if(!hcpu || !hcpu->vmxon_pa)
    {
        pr_err("RELM: Invalid vmxon region\n"); 
        return -EINVAL; 
    }

    asm volatile (
        "vmxon %[pa]\n\t"           
        "pushfq\n\t"                
        "popq %[rflags]\n\t"        
        : [rflags] "=r" (rflags)    
        : [pa] "m" (hcpu->vmxon_pa) 
        : "cc", "memory"            
    );

    if (rflags & X86_EFLAGS_CF) {
        pr_err("RELM: VMXON failed - VMfailInvalid (CF=1), error: %d\n",
               __vmread(VMCS_INSTRUCTION_ERROR_FIELD));
        return -EIO;
    }
    if (rflags & X86_EFLAGS_ZF) {
        pr_err("RELM: VMXON failed - VMfailValid (ZF=1), error: %d\n",
               __vmread(VMCS_INSTRUCTION_ERROR_FIELD));
        return -EIO;
    }

    return 0; 
}

static int relm_vmxoff(struct host_cpu *hcpu)
{
    uint64_t rflags; 

    asm volatile (
        "vmxoff\n\t"
        "pushfq\n\t"
        "popq %[rflags]\n\t"
        : [rflags] "=r" (rflags)
        :
        : "cc", "memory"
    );

     if (rflags & X86_EFLAGS_CF)
    {
        pr_err("RELM: VMXOFF failed - VMfailInvalid\n");
        return -EIO;
    }
    if (rflags & X86_EFLAGS_ZF) 
    {
        pr_err("RELM: VMXOFF failed - VMfailValid\n");
        return -EIO;
    }
    
    pr_info("RELM: VMXOFF successful, CPU exited VMX operation\n");

    return 0;
}

static int relm_setup_vmcs_region(struct vcpu *vcpu)
{
    if(!vcpu)
        return -EINVAL; 

    uint32_t vmcs_size = _get_vmcs_size(); 
    size_t alloc_size = (vmcs_size <= PAGE_SIZE) ? PAGE_SIZE : PAGE_ALIGN(vmcs_size); 

    vcpu->vmcs = (struct vmcs_region *)__get_free_pages(
        GFP_KERNEL | __GFP_ZERO, get_order(alloc_size)); 
    if(!vcpu->vmcs)
        return -ENOMEM; 

    *(uint32_t *)vcpu->vmcs = _vmcs_revision_id();
    vcpu->vmcs_pa = virt_to_phys(vcpu->vmcs);

    pr_info("VMCS region alllocated, revision ID set, and loaded\n"); 
    return 0;
}

int relm_vmclear(struct vcpu *vcpu)
{
    uint64_t rflags; 
    
    if(!vcpu || !vcpu->vmcs_pa)
    {
        pr_err("RELM: Invalid VMCS region\n"); 
        return -EINVAL; 
    }

    asm volatile (
        "vmclear %[pa]\n\t"
        "pushfq\n\t"
        "popq %[rflags]\n\t"
        : [rflags] "=r" (rflags)
        : [pa] "m" (vcpu->vmcs_pa)
        : "cc", "memory"
    );

    if (rflags & X86_EFLAGS_CF) 
    {
        pr_err("RELM: VMCLEAR failed - VMfailInvalid\n");
        return -EIO;
    }
    if (rflags & X86_EFLAGS_ZF) 
    {
        pr_err("RELM: VMCLEAR failed - VMfailValid\n");
        return -EIO;
    }

    PDEBUG("RELM: VCPU%d VMCLEAR ok (PA=0x%llx)\n", vcpu->vpid, vcpu->vmcs_pa);

    return 0;
}

int relm_vmptrld(struct vcpu *vcpu)
{
    uint64_t rflags;
    
    if (!vcpu || !vcpu->vmcs_pa)
    {
        pr_err("RELM: Invalid VMCS region\n");
        return -EINVAL;
    }
    
    asm volatile (
        "vmptrld %[pa]\n\t"
        "pushfq\n\t"
        "popq %[rflags]\n\t"
        : [rflags] "=r" (rflags)
        : [pa] "m" (vcpu->vmcs_pa)
        : "cc", "memory"
    );
    
    if (rflags & X86_EFLAGS_CF) 
    {
        pr_err("RELM: VMPTRLD failed - VMfailInvalid\n");
        return -EIO;
    }
    if (rflags & X86_EFLAGS_ZF) 
    {
        pr_err("RELM: VMPTRLD failed - VMfailValid\n");
        return -EIO;
    }

    PDEBUG("RELM: VCPU%d VMPTRLD ok (PA=0x%llx)\n", vcpu->vpid, vcpu->vmcs_pa);
    
    return 0;
}

/*
static struct host_cpu * relm_host_cpu_create(int logical_cpu_id)
{
    struct host_cpu * hcpu;
    int ret; 

    hcpu = kmalloc(sizeof(*hcpu), GFP_KERNEL); 
    if(!hcpu)
        return ERR_PTR(-ENOMEM); 

    hcpu->logical_cpu_id = logical_cpu_id; 

    if(relm_setup_vmxon_region(hcpu) != 0)
    {
        pr_err("failed to setup vmxon region on host cpu : %d\n", 
               hcpu->logical_cpu_id); 

        hcpu->logical_cpu_id = -1; 
        kfree(hcpu); 
        return NULL;  
    }

    ret = relm_vmxon(hcpu); 
    if(ret != 0)
    {
        pr_err("RELM: VMXON failed on CPU %d", logical_cpu_id); 
        relm_free_vmxon_region(hcpu); 
        kfree(hcpu); 
        return NULL; 
    }

    return hcpu; 
}

static void relm_destroy_host_cpu(struct host_cpu *hcpu)
{
    if(hcpu)
    {
        relm_vmxoff(hcpu); 
        relm_free_vmxon_region(hcpu); 
        kfree(hcpu); 
        hcpu = NULL; 
    }
}
*/ 
static int relm_setup_cr_controls(struct vcpu *vcpu)
{
    if(!vcpu)
        return -EINVAL; 

    uint64_t fixed0, fixed1; 
    uint64_t cr0_mask, cr4_mask; 

    /*INITIAL GUEST CR0 */ 
    fixed0 = __rdmsr1(MSR_IA32_VMX_CR0_FIXED0); 
    fixed1 = __rdmsr1(MSR_IA32_VMX_CR0_FIXED1);

    /*Initial state: paging (PG), Protected Mode (PE), Numeric Error(NE) */ 
    vcpu->cr0 = X86_CR0_PG | X86_CR0_PE | X86_CR0_NE; 

    /*hardware sanitation */ 
    vcpu->cr0 = (vcpu->cr0 | fixed0) & fixed1; 

    /*INITIAL GUEST CR4 */ 
    fixed0 = __rdmsr1(MSR_IA32_VMX_CR4_FIXED0); 
    fixed1 = __rdmsr1(MSR_IA32_VMX_CR4_FIXED1); 

    vcpu->cr4 = X86_CR4_PAE; 
    vcpu->cr4 = (vcpu->cr4 | fixed0) & fixed1; 

    /*configure host mask 
     * if bit is 1 in mask, guest cannot change it withot a vmexit
     * */

    cr0_mask = X86_CR0_PG | X86_CR0_PE | X86_CR0_NE | X86_CR0_CD | X86_CR0_NW;
    cr4_mask = X86_CR4_PAE | X86_CR4_PSE;

    CHECK_VMWRITE(GUEST_CR0, vcpu->cr0); 
    CHECK_VMWRITE(GUEST_CR4, vcpu->cr4); 
    CHECK_VMWRITE(GUEST_CR3, vcpu->cr3); 

    CHECK_VMWRITE(CR0_READ_SHADOW, vcpu->cr0); 
    CHECK_VMWRITE(CR4_READ_SHADOW, vcpu->cr4 & ~X86_CR4_VMXE); 

    CHECK_VMWRITE(CR0_GUEST_HOST_MASK, cr0_mask); 
    CHECK_VMWRITE(CR4_GUEST_HOST_MASK, cr4_mask); 

    return 0; 
}

static uint32_t relm_get_max_cr3_targets(void)
{
    uint64_t vmx_misc = __rdmsr1(MSR_IA32_VMX_MISC);
    return (uint8_t)((vmx_misc >> 16) & 0x1FF); 
}

/**
 * relm_set_cr3_target_value - write a GPA into a specific hardware slot 
 * @idx: the slot index (typically 0 to 3)
 * @target_gpa : the guest physical address of the page table root 
 */ 

static int relm_set_cr3_target_value(uint32_t idx, 
                                    uint64_t target_gpa)
{
    if(idx >= relm_get_max_cr3_targets())
        return -EINVAL; 

    switch(idx)
    {
        case 0:
            CHECK_VMWRITE(CR3_TARGET_VALUE0, target_gpa); 
            break;     
        case 1:
            CHECK_VMWRITE(CR3_TARGET_VALUE1, target_gpa); 
            break; 
        case 2:
            CHECK_VMWRITE(CR3_TARGET_VALUE2, target_gpa); 
            break; 
        case 3:
            CHECK_VMWRITE(CR3_TARGET_VALUE3, target_gpa); 
            break;

        default:
            return -ENOTSUPP; 
    }

    return 0; 
}

static int relm_set_cr3_target_count(uint32_t count)
{
    CHECK_VMWRITE(CR3_TARGET_COUNT, (uint64_t)count); 
    return 0; 
}

static int relm_init_cr3_targets(void)
{
    if(relm_set_cr3_target_count(0) != 0)
        return -1; 

    for(int i = 0; i < 4; i++)
        relm_set_cr3_target_value(i, 0); 

    return 0; 
}

/*which IO operation */ 
static int relm_setup_io_bitmap(struct vcpu *vcpu)
{
    if(!vcpu)
        return -EINVAL;

    size_t total_bitmap_size = 2 * VMCS_IO_BITMAP_SIZE;

    vcpu->io_bitmap = (uint8_t *)__get_free_pages(GFP_KERNEL,
                                                   get_order(total_bitmap_size));
    if(!vcpu->io_bitmap)
    {
        pr_err("Failed to allocate I/O bitmap memory\n"); 
        return -ENOMEM; 
    }

    /* clear entire I/O bitmap (all 0  == allow all ports) */ 
    memset(vcpu->io_bitmap, 0, total_bitmap_size);

    vcpu->io_bitmap_pa = virt_to_phys(vcpu->io_bitmap); 
    
    PDEBUG("Allocated and cleared I/O bitmap at VA %p PA 0x%llx\n", 
            vcpu->io_bitmap, (unsigned long long )vcpu->io_bitmap_pa);

    /*write the address to the VMCS */ 
    if(_vmwrite(VMCS_IO_BITMAP_A, vcpu->io_bitmap_pa) != 0)
    {
        PDEBUG("VMWrite VMCS_IO_BITMAP_A failed\n");
        relm_free_io_bitmap(vcpu); 
        return -EIO; 
    }

    if(_vmwrite(VMCS_IO_BITMAP_B, vcpu->io_bitmap_pa + VMCS_IO_BITMAP_SIZE) != 0)
    {
        PDEBUG("VMWrite VMCS_IO_BITMAP_B failed\n"); 
        relm_free_io_bitmap(vcpu); 
        return -EIO; 
    }

    PDEBUG("IO bitmap A physical address : 0x%llx\nIO bitmap B physical address : 0x%llx\n", 
           vcpu->io_bitmap_pa, 
           vcpu->io_bitmap_pa + VMCS_IO_BITMAP_SIZE); 

    return 0; 

}

static int relm_update_exception_bitmap(struct vcpu *vcpu)
{
    CHECK_VMWRITE(VMCS_EXCEPTION_BITMAP, (vcpu->exception_bitmap & 0xFFFFFFFF)); 
    return 0; 
}

static int relm_set_exception_intercept(struct vcpu *vcpu, int vector)
{
    int ret;
    if(vector >= 32)
        ret = -EINVAL; 

    vcpu->exception_bitmap |= (1U << vector); 
    ret = relm_update_exception_bitmap(vcpu); 

    return ret; 
}

static int relm_clear_exception_intercept(struct vcpu *vcpu, int vector) 
{
    int ret;
    if(vector >= 32)
        ret = -EINVAL;

    vcpu->exception_bitmap &= ~(1U << vector); 
    ret = relm_update_exception_bitmap(vcpu); 

    return ret;  
}

/*MSRs that cause VM exit when accessed by guest */ 
static int relm_setup_msr_bitmap(struct vcpu *vcpu)
{
    vcpu->msr_bitmap = (uint8_t *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
    if(!vcpu->msr_bitmap)
    {
        pr_info("Failed to allocate MSR bitmap\n"); 
        return -ENOMEM;
    }

    vcpu->msr_bitmap_pa = virt_to_phys(vcpu->msr_bitmap); 

    /*mark IA32_SYSENTER_CS as causing a VM exit */ 
    uint32_t msr_index = IA32_SYSENTER_CS;
    uint8_t *bitmap = (uint8_t*)vcpu->msr_bitmap;
    uint32_t byte = msr_index / 8;
    uint8_t bit = msr_index % 8; 
    bitmap[byte] |= (1 << bit); 


    if(_vmwrite(VMCS_MSR_BITMAP, vcpu->msr_bitmap_pa) != 0){
        relm_free_msr_bitmap(vcpu); 
        return -EIO; 
    }

    return 0; 
}

/*util round size up to full pagess and computer order */ 
static inline unsigned int msr_area_order(size_t bytes)
{
    size_t pages  = DIV_ROUND_UP(bytes, PAGE_SIZE); 
    return get_order(pages * PAGE_SIZE); 
}

static struct msr_entry *alloc_msr_entry(size_t n_entries)
{
    size_t size = n_entries * sizeof(struct msr_entry); 
    unsigned int order = msr_area_order(size); 

    struct page *p = alloc_pages(GFP_KERNEL | __GFP_ZERO, order); 
    if(!p)
        return ERR_PTR(-ENOMEM); 

    return (struct msr_entry*)page_address(p); 
}

static void free_msr_area(struct msr_entry *area, size_t n_entries)
{
    if(!area)
        return; 

    size_t size = n_entries * sizeof(struct msr_entry); 
    unsigned int order = msr_area_order(size); 
    free_pages((unsigned long)area, order); 
}

/*populate msr-load are from parallel arrays */ 
static void populate_msr_load_area(struct msr_entry *area, size_t count,
                              const uint32_t *indices, const uint64_t *values)
{
    size_t i; 
    for(i = 0; i < count; ++i)
    {
        area[i].index = indices[i]; 
        area[i].reserved = 0; 
        area[i].value = values[i]; 
    }

}

/*populate MSR-store areas index fields. CPU will write this values on VM-exit */ 
static void populate_msr_store_area(struct msr_entry *area, size_t count, 
                                    const uint32_t *indices)
{
    size_t i;
    for(i = 0; i < count; ++i)
    {
        area[i].index = indices[i];
        area[i].reserved = 0; 
        area[i].value = 0; 
    }

}

static int relm_setup_msr_areas(struct vcpu *vcpu,
                    const uint32_t *vmexit_list,  size_t vmexit_count,
                    const uint32_t *vmentry_list, const uint64_t *vmentry_values,
                    size_t vmentry_count)
{
    int rc = 0;
    size_t i;

    if (!vcpu)
        return -EINVAL;

    #ifndef UINT16_MAX
    #define UINT16_MAX 65535
    #endif 

    /* VMCS count fields are 16-bit */
    if (vmexit_count > UINT16_MAX || vmentry_count > UINT16_MAX)
        return -EINVAL; 

    /*VM-exit MSR-store area (guest MSRs → memory on exit) */
    vcpu->vmexit_store_area = alloc_msr_entry(vmexit_count);
    if (IS_ERR(vcpu->vmexit_store_area)) 
    {
        rc = PTR_ERR(vcpu->vmexit_store_area); 
        vcpu->vmexit_store_area = NULL; 
        goto _out;
    }
    vcpu->vmexit_store_pa = virt_to_phys(vcpu->vmexit_store_area);

    /* VM-exit MSR-load area (memory → host MSRs on exit) */
    vcpu->vmexit_load_area = alloc_msr_entry(vmentry_count);
    if (IS_ERR(vcpu->vmexit_load_area))
    {
        rc = PTR_ERR(vcpu->vmexit_load_area); 
        vcpu->vmexit_load_area = NULL; 
        goto _out_free_exit_store;
    }
    vcpu->vmexit_load_pa = virt_to_phys(vcpu->vmexit_load_area);

    /* VM-entry MSR-load area (memory → guest MSRs on entry) */
    vcpu->vmentry_load_area = alloc_msr_entry(vmentry_count);
    if (IS_ERR(vcpu->vmentry_load_area))
    {
        rc = PTR_ERR(vcpu->vmentry_load_area); 
        vcpu->vmentry_load_area = NULL; 
        goto _out_free_exit_load;
    }
    vcpu->vmentry_load_pa = virt_to_phys(vcpu->vmentry_load_area);

    populate_msr_store_area(vcpu->vmexit_store_area, vmexit_count, vmexit_list);

    /* On VM-exit, restore host MSR values (typically the ones the guest sees on entry) */
    for (i = 0; i < vmentry_count; ++i) 
    {
        uint32_t idx = vmentry_list[i];
        uint64_t val = vmentry_values ? vmentry_values[i] : 0;

        vcpu->vmexit_load_area[i].index    = idx;
        vcpu->vmexit_load_area[i].reserved = 0;
        vcpu->vmexit_load_area[i].value    = val;
    }

    populate_msr_load_area(vcpu->vmentry_load_area,
                           vmentry_count,
                           vmentry_list,
                           vmentry_values);

    vcpu->vmexit_count  = vmexit_count;
    vcpu->vmentry_count = vmentry_count;

    if (_vmwrite(VMCS_EXIT_MSR_STORE_ADDR, vcpu->vmexit_store_pa) ||
        _vmwrite(VMCS_EXIT_MSR_STORE_COUNT, (uint64_t)vmexit_count) ||
        _vmwrite(VMCS_EXIT_MSR_LOAD_ADDR,   vcpu->vmexit_load_pa) ||
        _vmwrite(VMCS_EXIT_MSR_LOAD_COUNT,  (uint64_t)vmentry_count) ||
        _vmwrite(VMCS_ENTRY_MSR_LOAD_ADDR,  vcpu->vmentry_load_pa) ||
        _vmwrite(VMCS_ENTRY_MSR_LOAD_COUNT, (uint64_t)vmentry_count)) 
    {
        rc = -EIO;
        goto _out_free_all;
    }

    return 0;

_out_free_all:
    free_msr_area(vcpu->vmentry_load_area, vmentry_count);
    vcpu->vmentry_load_area = NULL;
    vcpu->vmentry_load_pa = 0;

_out_free_exit_load:
    free_msr_area(vcpu->vmexit_load_area, vmentry_count);
    vcpu->vmexit_load_area = NULL;
    vcpu->vmexit_load_pa = 0;

_out_free_exit_store:
    free_msr_area(vcpu->vmexit_store_area, vmexit_count);
    vcpu->vmexit_store_area = NULL;
    vcpu->vmexit_store_pa = 0;

_out:
    return rc;
}

void relm_free_all_msr_areas(struct vcpu *vcpu)
{
    if (!vcpu)
        return;

    if (vcpu->vmexit_store_area)
    {
        free_msr_area(vcpu->vmexit_store_area, vcpu->vmexit_count);
        vcpu->vmexit_store_area = NULL;
        vcpu->vmexit_store_pa = 0;
    }

    if (vcpu->vmexit_load_area) 
    {
        free_msr_area(vcpu->vmexit_load_area, vcpu->vmentry_count);
        vcpu->vmexit_load_area = NULL;
        vcpu->vmexit_load_pa = 0;
    }

    if (vcpu->vmentry_load_area) 
    {
        free_msr_area(vcpu->vmentry_load_area, vcpu->vmentry_count);
        vcpu->vmentry_load_area = NULL;
        vcpu->vmentry_load_pa = 0;
    }

    vcpu->vmexit_count = vcpu->vmentry_count = 0;
}

static int relm_vcpu_setup_msr_state(struct vcpu *vcpu)
{
    uint64_t vmentry_values[RELM_MAX_MANAGED_MSRS] = {0};
    int i = 0; 

    vcpu->msr_indices[i++] = MSR_IA32_EFER;
    vcpu->msr_indices[i++] = MSR_IA32_STAR;
    vcpu->msr_indices[i++] = MSR_IA32_LSTAR;
    vcpu->msr_indices[i++] = MSR_IA32_CSTAR;
    vcpu->msr_indices[i++] = MSR_IA32_FMASK;
    vcpu->msr_indices[i++] = MSR_IA32_FS_BASE;
    vcpu->msr_indices[i++] = MSR_IA32_GS_BASE;
    vcpu->msr_count = i;

    /*prepare guest initial values for vm-entry 
     * default: long mode (LME/LMA) and syscall enable */ 
    vcpu->efer = EFER_LME | EFER_LMA | EFER_SCE;

    vmentry_values[0] = vcpu->efer;          // Guest EFER
    vmentry_values[1] = 0;                  // Guest STAR
    vmentry_values[2] = 0;                  // Guest LSTAR
    vmentry_values[3] = 0;                  // Guest CSTAR
    vmentry_values[4] = 0;                  // Guest FMASK
    vmentry_values[5] = vcpu->regs.fs_base;      // Guest FS_BASE
    vmentry_values[6] = vcpu->regs.gs_base;      // Guest GS_BASE
    

    return relm_setup_msr_areas(vcpu, 
                               vcpu->msr_indices, vcpu->msr_count,
                               vcpu->msr_indices, vmentry_values, 
                               vcpu->msr_count); 
}

static void relm_init_exec_controls(struct vcpu *vcpu)
{
    struct vmx_exec_ctrls *controls = &vcpu->controls; 

    controls->pin_based = 
        VMCS_PIN_EXTINT_EXITING | 
        VMCS_PIN_NMI_EXITING  | 
        VMCS_PIN_VIRTUAL_NMIS;
    
    controls->primary_proc = 
        VMCS_PROC_USE_MSR_BITMAPS | 
        VMCS_PROC_ACTIVATE_SECONDARY |
        VMCS_PROC_HLT_EXITING |
        VMCS_PROC_CR8_LOAD_EXITING | 
        VMCS_PROC_CR8_STORE_EXITING |
        VMCS_PROC_TPR_SHADOW |
        VMCS_PROC_UNCOND_IO_EXITING |
        VMCS_PROC_USE_IO_BITMAPS; 

    controls->secondary_proc = 
        VMCS_PROC2_ENABLE_EPT |
        VMCS_PROC2_RDTSCP | 
        VMCS_PROC2_UNRESTRICTED_GUEST |
        VMCS_PROC2_ENABLE_VMFUNC; 

    if(_cpu_has_vpid())
        controls->secondary_proc = 
            controls->secondary_proc | VMCS_PROC2_VPID; 

    controls->vm_entry =
        VM_ENTRY_IA32E_MODE |
        VMCS_ENTRY_LOAD_GUEST_PAT | 
        VMCS_ENTRY_LOAD_IA32_EFER | 
        VMCS_ENTRY_LOAD_DEBUG; 

    controls->vm_exit = 
        VM_EXIT_HOST_ADDR_SPACE_SIZE | 
        VMCS_EXIT_SAVE_IA32_PAT |
        VMCS_EXIT_LOAD_IA32_PAT |
        VMCS_EXIT_SAVE_EFER |
        VMCS_EXIT_LOAD_EFER | 
        VMCS_EXIT_ACK_INTR_ON_EXIT; 
}

static inline uint32_t vmx_adjust(uint32_t req, uint32_t allowed0, uint32_t allowed1)
{
    req |= allowed0;
    req &= allowed1;
    return req;
}

static int relm_apply_exec_controls(struct vcpu *vcpu)
{
    struct vmx_exec_ctrls *controls = &vcpu->controls;
    uint64_t msr;
    uint32_t allowed0;
    uint32_t allowed1;
    uint32_t final;

    /* pin-based */
    msr = __rdmsr1(MSR_IA32_VMX_PINBASED_CTLS);
    allowed0 = (uint32_t)msr;
    allowed1 = (uint32_t)(msr >> 32);

    final = vmx_adjust(controls->pin_based, allowed0, allowed1);
    CHECK_VMWRITE(VMCS_PIN_BASED_EXEC_CONTROLS, final);

    /* primary processor */
    msr = __rdmsr1(MSR_IA32_VMX_PROCBASED_CTLS);
    allowed0 = (uint32_t)msr;
    allowed1 = (uint32_t)(msr >> 32);

    final = vmx_adjust(controls->primary_proc, allowed0, allowed1);
    CHECK_VMWRITE(VMCS_PRIMARY_PROC_BASED_EXEC_CONTROLS, final);

    /* secondary processor */
    msr = __rdmsr1(MSR_IA32_VMX_PROCBASED_CTLS2);
    allowed0 = (uint32_t)msr;
    allowed1 = (uint32_t)(msr >> 32);

    final = vmx_adjust(controls->secondary_proc, allowed0, allowed1);
    CHECK_VMWRITE(VMCS_SECONDARY_PROC_BASED_EXEC_CONTROLS, final);

    /* entry */
    msr = __rdmsr1(MSR_IA32_VMX_ENTRY_CTLS);
    allowed0 = (uint32_t)msr;
    allowed1 = (uint32_t)(msr >> 32);

    final = vmx_adjust(controls->vm_entry, allowed0, allowed1);
    CHECK_VMWRITE(VMCS_ENTRY_CONTROLS, final);

    /* exit */
    msr = __rdmsr1(MSR_IA32_VMX_EXIT_CTLS);
    allowed0 = (uint32_t)msr;
    allowed1 = (uint32_t)(msr >> 32);

    final = vmx_adjust(controls->vm_exit, allowed0, allowed1);
    CHECK_VMWRITE(VMCS_EXIT_CONTROLS, final);

    return 0;
}

int relm_setup_exec_controls(struct vcpu *vcpu)
{
    if(!vcpu)
        return -EINVAL; 

    relm_init_exec_controls(vcpu); 
    if(relm_apply_exec_controls(vcpu) != 0)
        return -1; 

    return 0; 

}

struct vcpu *relm_vcpu_alloc_init(struct relm_vm *vm, int vpid)
{
    if(!vm)
        return ERR_PTR(-EINVAL); 

    struct vcpu *vcpu;
    int ret; 

    PDEBUG("RELM: Allocating zeroed VCPU struct\n");
    /* Allocate zeroed VCPU struct */
    vcpu = kzalloc(sizeof(*vcpu), GFP_KERNEL);
    if (!vcpu) {
        PDEBUG("RELM: Failed to allocate VCPU struct\n");
        return ERR_PTR(-ENOMEM);
    }
    PDEBUG("RELM: VCPU struct allocated at %p\n", vcpu);

    vcpu->vm = vm;
    vcpu->vpid = vpid;
    vcpu->state = VCPU_STATE_UNINITIALIZED; 
    
    if(relm_apic_alloc(&vcpu->apic) != 0)
    {
        pr_err("RELM: VCPU%d: failed to allocate APIC pages\n", vpid);
        kfree(vcpu);
        return NULL;
    }

    vcpu->halted = false; 
    vcpu->launched = 0; 

    /*default architectural state */ 
    vcpu->regs.rflags = 0x2; 
    vcpu->regs.rip = 0x0; 

    /*cr3 points to guest page tables */
    vcpu->cr3 = vm->pml4_gpa;

    spin_lock_init(&vcpu->lock);
    init_waitqueue_head(&vcpu->wq); 


    PDEBUG("RELM: Allocating vCPU host stack\n");
    /*alllocate vCPU stack */ 
    vcpu->host_stack = (void *)__get_free_pages(
        GFP_KERNEL | __GFP_ZERO, 
        HOST_STACK_ORDER
    ); 
    if(!vcpu->host_stack) {
        pr_err("RELM: Failed to allocate host stack for VCPU%d\n", vpid);
        goto _out_free_vcpu;
    }
    PDEBUG("RELM: Host stack allocated at %p\n", vcpu->host_stack);

    /*point to top of host stack 
     * point to near top of the stack 
     * leave 0x100 bytes for red zone , then 16-byte align*/ 
    vcpu->host_rsp = ((uint64_t)vcpu->host_stack + HOST_STACK_SIZE - 0x100)
                    & ~0xFULL; 
     
    PDEBUG("RELM: Host RSP set to 0x%llx\n", vcpu->host_rsp);

    PDEBUG("RELM: Setting up VMCS region\n");
    /* Allocate and setup VMCS region */
    if (relm_setup_vmcs_region(vcpu) != 0)
    {
        pr_err("RELM: Failed to setup VMCS region\n");
        goto _out_free_host_stack; 
    }
    PDEBUG("RELM: VMCS region setup complete\n");

    relm_init_exec_controls(vcpu); 

    PDEBUG("RELM: Setting up IO bitmap\n");

    if(relm_vcpu_io_bitmap_enabled(vcpu))
    {
        if (relm_setup_io_bitmap(vcpu) != 0)
        {
            pr_err("Failed to setup I/O bitmap\n");
            goto _out_free_msr_areas; 
        }
    }

    PDEBUG("RELM: Setting exception bitmap\n");
    /* we catch #UD (6) to prevent guest crashes on unsupported instructions.
     * we catch #PF (14) if we are using Shadow Paging or debugging memory.
     */
    vcpu->exception_bitmap = (1U << 6) | (1U << 14); 

    PDEBUG("RELM: Setting up MSR bitmap\n");
    /* Setup MSR bitmap */
    if(relm_vcpu_msr_bitmap_enabled(vcpu))
    {
        if (relm_setup_msr_bitmap(vcpu) != 0) 
        {
            pr_err("Failed to setup MSR bitmap\n");
            goto _out_free_io_bitmap; 
        }
    }

    PDEBUG("RELM: VCPU%d: allocating MSR areas\n", vpid);

    if(relm_vcpu_setup_msr_state(vcpu) != 0)
    {
        pr_err("Failed to setup MSR areas\n"); 
        goto _out_free_msr_bitmap; 
    }

    relm_apic_init(&vcpu->apic, (uint8_t)VPID_TO_INDEX(vpid)); 
    
    vcpu->state = VCPU_STATE_INITIALIZED; 

    pr_info("RELM: VCPU%d: Phase 1 complete - all memory allocated"
            " (stack=%p rsp=0x%llx vmcs_pa=0x%llx)\n",
            vpid, vcpu->host_stack, vcpu->host_rsp, vcpu->vmcs_pa);

    return vcpu; 

_out_free_msr_bitmap:
    relm_free_msr_bitmap(vcpu);

_out_free_io_bitmap:
    relm_free_io_bitmap(vcpu);

_out_free_msr_areas:
    relm_free_all_msr_areas(vcpu);

_out_free_vmcs:
    relm_free_vmcs_region(vcpu);

_out_free_host_stack:
    if(vcpu->host_stack)
        free_pages((unsigned long)vcpu->host_stack, HOST_STACK_ORDER);

_out_free_vcpu:
    if(vcpu)
        kfree(vcpu);
    
    return NULL; 
}

int relm_vcpu_vmcs_setup(struct vcpu *vcpu)
{
    int ret; 

    if(!vcpu)
    {
        pr_err("RELM: relm_vcpu_vmcs_setup valiied with NULL vcpu\n"); 
        return -EINVAL; 
    }
    pr_info("RELM: VCPU%d: Phase 2 - VMCS setup starting on CPU%d\n",
            vcpu->vpid, smp_processor_id());

    PDEBUG("RELM: VCPU%d: executing VMCLEAR on CPU%d\n",
           vcpu->vpid, smp_processor_id());
    
    ret = relm_vmclear(vcpu);
    if(ret != 0)
    {
        pr_err("RELM: VCPU%d: VMCLEAR failed on CPU%d: %d\n",
               vcpu->vpid, smp_processor_id(), ret);
        return ret;
    }

    PDEBUG("RELM: VCPU%d: executing VMPTRLD on CPU%d\n",
           vcpu->vpid, smp_processor_id());

    ret = relm_vmptrld(vcpu);
    if(ret != 0)
    {
        pr_err("RELM: VCPU%d: VMPTRLD failed on CPU%d: %d\n",
               vcpu->vpid, smp_processor_id(), ret);
        return ret;
    }

    if(vcpu->vm->ept)
    {
        PDEBUG("RELM: VCPU%d: writing EPT_POINTER=0x%llx\n",
               vcpu->vpid, vcpu->vm->ept->eptp);
        CHECK_VMWRITE(EPT_POINTER, vcpu->vm->ept->eptp);
    }
    else
    {
        pr_err("RELM: VCPU%d: no EPT context - cannot proceed\n", vcpu->vpid);
        return -EINVAL;
    }

    PDEBUG("RELM: VCPU%d: applying execution controls\n", vcpu->vpid);

    if(relm_apply_exec_controls(vcpu) != 0)
    {
        pr_err("RELM: VCPU%d: exec control VMCS write failed\n", vcpu->vpid);
        return -EIO;
    }

    if(_cpu_has_vpid())
    {
        uint64_t vpid_val = (uint64_t)vcpu->vpid;
        PDEBUG("RELM: VCPU%d: writing VMCS_VPID=%llu\n", vcpu->vpid, vpid_val);
        CHECK_VMWRITE(VMCS_VPID, vpid_val);
    }

    if(relm_vcpu_io_bitmap_enabled(vcpu) && vcpu->io_bitmap_pa)
    {
        PDEBUG("RELM: VCPU%d: writing IO bitmap A=0x%llx B=0x%llx\n",
               vcpu->vpid, vcpu->io_bitmap_pa,
               vcpu->io_bitmap_pa + VMCS_IO_BITMAP_SIZE);

        if(_vmwrite(VMCS_IO_BITMAP_A, vcpu->io_bitmap_pa) != 0 ||
           _vmwrite(VMCS_IO_BITMAP_B, vcpu->io_bitmap_pa + VMCS_IO_BITMAP_SIZE) != 0)
        {
            pr_err("RELM: VCPU%d: IO bitmap VMCS write failed\n", vcpu->vpid);
            return -EIO;
        }
    }

    if(relm_vcpu_msr_bitmap_enabled(vcpu) && vcpu->msr_bitmap_pa)
    {
        PDEBUG("RELM: VCPU%d: writing VMCS_MSR_BITMAP=0x%llx\n",
               vcpu->vpid, vcpu->msr_bitmap_pa);

        if(_vmwrite(VMCS_MSR_BITMAP, vcpu->msr_bitmap_pa) != 0)
        {
            pr_err("RELM: VCPU%d: MSR bitmap VMCS write failed\n", vcpu->vpid);
            return -EIO;
        }
    }

    if(vcpu->vmexit_store_pa && vcpu->vmexit_count > 0)
    {
        if(_vmwrite(VMCS_EXIT_MSR_STORE_ADDR, vcpu->vmexit_store_pa) ||
           _vmwrite(VMCS_EXIT_MSR_STORE_COUNT, (uint64_t)vcpu->vmexit_count))
        {
            pr_err("RELM: VCPU%d: vmexit store MSR area VMCS write failed\n",
                   vcpu->vpid);
            return -EIO;
        }
    }

    if(vcpu->vmexit_load_pa && vcpu->vmentry_count > 0)
    {
        if(_vmwrite(VMCS_EXIT_MSR_LOAD_ADDR, vcpu->vmexit_load_pa) ||
           _vmwrite(VMCS_EXIT_MSR_LOAD_COUNT, (uint64_t)vcpu->vmentry_count))
        {
            pr_err("RELM: VCPU%d: vmexit load MSR area VMCS write failed\n",
                   vcpu->vpid);
            return -EIO;
        }
    }
    
    if(vcpu->vmentry_load_pa && vcpu->vmentry_count > 0)
    {
        if(_vmwrite(VMCS_ENTRY_MSR_LOAD_ADDR, vcpu->vmentry_load_pa) ||
           _vmwrite(VMCS_ENTRY_MSR_LOAD_COUNT, (uint64_t)vcpu->vmentry_count))
        {
            pr_err("RELM: VCPU%d: vmentry load MSR area VMCS write failed\n",
                   vcpu->vpid);
            return -EIO;
        }
    }

    PDEBUG("RELM: VCPU%d: MSR area counts: exit_store=%zu exit_load=%zu entry_load=%zu\n",
           vcpu->vpid, vcpu->vmexit_count, vcpu->vmentry_count, vcpu->vmentry_count);

    /*exception vector.
     * bit 6=#UD, bit 14=#PF. */
    CHECK_VMWRITE(VMCS_EXCEPTION_BITMAP,
                  (uint64_t)(vcpu->exception_bitmap & 0xFFFFFFFF));

    PDEBUG("RELM: VCPU%d: exception_bitmap=0x%x written to VMCS\n",
           vcpu->vpid, vcpu->exception_bitmap);

   /* Count=0 means all guest CR3 loads cause VM-exits regardless of value.
     * We can later add the guest PML4 GPA to the target list to avoid exits
     * when the guest loads the page table root we already know about. */
    CHECK_VMWRITE(CR3_TARGET_COUNT, 0ULL);

    if(relm_apic_vmcs_setup(vcpu) != 0)
    {
        pr_err("RELM: VCPU%d: APIC VMCS setup failed\n", vcpu->vpid);
        return -EIO;
    }
 
    if(relm_apic_ept_setup(vcpu) != 0)
    {
        pr_err("RELM: VCPU%d: APIC EPT setup failed\n", vcpu->vpid);
        return -EIO;
    }

    pr_info("RELM: VCPU%d: Phase 2 VMCS setup complete on CPU%d\n",
            vcpu->vpid, smp_processor_id());

    return 0; 
}

void relm_free_io_bitmap(struct vcpu *vcpu)
{
    if(!vcpu)
        return;

    if (vcpu->io_bitmap) 
    {
        free_pages((unsigned long)vcpu->io_bitmap, VMCS_IO_BITMAP_PAGES_ORDER);
        vcpu->io_bitmap = NULL;
        vcpu->io_bitmap_pa = 0;
    }
}

void relm_free_msr_bitmap(struct vcpu *vcpu)
{
    if(!vcpu)
        return; 

    if (vcpu->msr_bitmap) 
    {
        free_page((unsigned long)vcpu->msr_bitmap);
        vcpu->msr_bitmap = NULL;
        vcpu->msr_bitmap_pa = 0;
    }
}

void relm_free_vmcs_region(struct vcpu *vcpu)
{
    if (vcpu->vmcs) 
    {
        free_pages((unsigned long)vcpu->vmcs, get_order(_get_vmcs_size()));
        vcpu->vmcs = NULL;
        vcpu->vmcs_pa = 0;
    }
}

static void relm_free_vmxon_region(struct host_cpu *hcpu)
{
    if (hcpu->vmxon) 
    {
        free_pages((unsigned long)hcpu->vmxon, 0); // VMXON is 1 page
        hcpu->vmxon = NULL;
        hcpu->vmxon_pa = 0;
    }
}

/* Free the entire VCPU */
void relm_free_vcpu(struct vcpu *vcpu)
{
    if (!vcpu)
        return;

    relm_apic_free(&vcpu->apic); 
    relm_free_all_msr_areas(vcpu);
    relm_free_msr_bitmap(vcpu);
    relm_free_io_bitmap(vcpu);
    relm_free_vmcs_region(vcpu);
    
    if(vcpu->host_stack)
        free_pages((unsigned long)vcpu->host_stack, HOST_STACK_ORDER);
    
    kfree(vcpu);
}

static int relm_setup_host_state(struct vcpu *vcpu)
{
    if(!vcpu)
        return -EINVAL; 

    u64 cr0 = _read_cr0();
    u64 cr3 = _read_cr3();
    u64 cr4 = _read_cr4();

    CHECK_VMWRITE(HOST_CR0, cr0);
    CHECK_VMWRITE(HOST_CR3, cr3);
    CHECK_VMWRITE(HOST_CR4, cr4);

    extern void relm_vmexit_handler(void); 
    CHECK_VMWRITE(HOST_RSP, vcpu->host_rsp);
    CHECK_VMWRITE(HOST_RIP, (uint64_t)relm_vmexit_handler);

    /* Segment selectors (must be valid) 
     * masking with 0xF8 ensures bottom 3 bits (RPL and TI) are 0*/
    CHECK_VMWRITE(HOST_CS_SELECTOR, __KERNEL_CS & 0xF8);
    CHECK_VMWRITE(HOST_SS_SELECTOR, __KERNEL_DS & 0xF8);
    CHECK_VMWRITE(HOST_DS_SELECTOR, __KERNEL_DS & 0xF8);
    CHECK_VMWRITE(HOST_ES_SELECTOR, __KERNEL_DS & 0xF8);
    CHECK_VMWRITE(HOST_FS_SELECTOR, 0);
    CHECK_VMWRITE(HOST_GS_SELECTOR, 0);
    
    u16 tr; 
    struct desc_ptr gdt; 
    unsigned long tr_base; 

    /*read TR selector */ 
    __asm__ __volatile__ (
        "str %0" 
        : "=r"(tr)); 

    /*read GDT*/ 
    __asm__ __volatile__ (
            "sgdt %0"
            : "=m"(gdt)); 

    /*calculate TR base from GDT*/ 
    if(tr)
    {
        struct desc_struct *gdt_desc= (struct desc_struct*)gdt.address; 
        unsigned int index = tr >> 3; 
        
        tr_base = gdt_desc[index].base0 |
            (gdt_desc[index].base1 << 16) |
            (gdt_desc[index].base2 << 24); 
    
        #ifdef CONFIG_X86_64
        /*for 64 bit TSS descriptor is 16 bytes*/  
        if(index + 1 < (gdt.size + 1) / 8)
        {
            unsigned long base_high = *(unsigned long *)&gdt_desc[index + 1]; 
            tr_base |= (base_high << 32);
        }
        #endif
    }
    else {
        pr_err("RELM: TR selector is 0, cannot continue\n"); 
        return -EINVAL; 
    }

    CHECK_VMWRITE(HOST_TR_SELECTOR, tr & 0xFFF8); 
    CHECK_VMWRITE(HOST_TR_BASE, tr_base); 
    CHECK_VMWRITE(HOST_GDTR_BASE, gdt.address); 

    struct desc_ptr idt; 
    __asm__ __volatile__ (
        "sidt %0"
        : "=m"(idt));

    CHECK_VMWRITE(HOST_IDTR_BASE, idt.address); 

    CHECK_VMWRITE(VMCS_LINK_POINTER, 0xFFFFFFFFFFFFFFFFULL);
    /* FS/GS base */
    CHECK_VMWRITE(HOST_FS_BASE, __rdmsr1(MSR_FS_BASE));
    CHECK_VMWRITE(HOST_GS_BASE, __rdmsr1(MSR_GS_BASE));

    /* Syscall MSRs */
    CHECK_VMWRITE(HOST_SYSENTER_CS, __rdmsr1(MSR_IA32_SYSENTER_CS));
    CHECK_VMWRITE(HOST_SYSENTER_ESP, __rdmsr1(MSR_IA32_SYSENTER_ESP));
    CHECK_VMWRITE(HOST_SYSENTER_EIP, __rdmsr1(MSR_IA32_SYSENTER_EIP));

    /* EFER must be set */
    CHECK_VMWRITE(HOST_IA32_EFER, __rdmsr1(MSR_EFER));

    return 0; 
}

static int relm_setup_guest_state(struct vcpu *vcpu)
{
    if(!vcpu)
        return -EINVAL; 

    /* Control registers */
    CHECK_VMWRITE(GUEST_CR0, vcpu->cr0);
    CHECK_VMWRITE(GUEST_CR3, vcpu->cr3);
    CHECK_VMWRITE(GUEST_CR4, vcpu->cr4);

    /* RIP / RSP / RFLAGS */
    CHECK_VMWRITE(GUEST_RIP, vcpu->regs.rip);
    CHECK_VMWRITE(GUEST_RSP, vcpu->regs.rsp);
    CHECK_VMWRITE(GUEST_RFLAGS, 0x2); /* reserved bit must be 1 */

    /* Segment selectors (flat) */
    CHECK_VMWRITE(GUEST_CS_SELECTOR, 0x8);
    CHECK_VMWRITE(GUEST_DS_SELECTOR, 0x10);
    CHECK_VMWRITE(GUEST_ES_SELECTOR, 0x10);
    CHECK_VMWRITE(GUEST_SS_SELECTOR, 0x10);
    CHECK_VMWRITE(GUEST_FS_SELECTOR, 0);
    CHECK_VMWRITE(GUEST_GS_SELECTOR, 0);

    /* Segment bases */
    CHECK_VMWRITE(GUEST_CS_BASE, 0);
    CHECK_VMWRITE(GUEST_DS_BASE, 0);
    CHECK_VMWRITE(GUEST_ES_BASE, 0);
    CHECK_VMWRITE(GUEST_SS_BASE, 0);
    CHECK_VMWRITE(GUEST_FS_BASE, 0);
    CHECK_VMWRITE(GUEST_GS_BASE, 0);

    /* Segment limits */
    CHECK_VMWRITE(GUEST_CS_LIMIT, 0xFFFFFFFF);
    CHECK_VMWRITE(GUEST_DS_LIMIT, 0xFFFFFFFF);
    CHECK_VMWRITE(GUEST_ES_LIMIT, 0xFFFFFFFF);
    CHECK_VMWRITE(GUEST_SS_LIMIT, 0xFFFFFFFF);

    /* Segment access rights (64-bit code/data) */
    CHECK_VMWRITE(GUEST_CS_AR_BYTES, 0xA09B);
    CHECK_VMWRITE(GUEST_DS_AR_BYTES, 0xC093);
    CHECK_VMWRITE(GUEST_ES_AR_BYTES, 0xC093);
    CHECK_VMWRITE(GUEST_SS_AR_BYTES, 0xC093);

    CHECK_VMWRITE(GUEST_FS_AR_BYTES, 0x10000);  // Unusable
    CHECK_VMWRITE(GUEST_GS_AR_BYTES, 0x10000);  // Unusable

    CHECK_VMWRITE(GUEST_LDTR_SELECTOR, 0);
    CHECK_VMWRITE(GUEST_LDTR_BASE, 0);
    CHECK_VMWRITE(GUEST_LDTR_LIMIT, 0);
    CHECK_VMWRITE(GUEST_LDTR_AR_BYTES, 0x10000); 
    
    CHECK_VMWRITE(GUEST_TR_SELECTOR, 0);
    CHECK_VMWRITE(GUEST_TR_BASE, 0);
    CHECK_VMWRITE(GUEST_TR_LIMIT, 0x67);  // Minimum TSS size
    CHECK_VMWRITE(GUEST_TR_AR_BYTES, 0x008B);  // Present, Type=TSS Busy
    
    /* GDTR / IDTR */
    CHECK_VMWRITE(GUEST_GDTR_BASE, vcpu->gdtr_base);
    CHECK_VMWRITE(GUEST_GDTR_LIMIT, vcpu->gdtr_limit);
    CHECK_VMWRITE(GUEST_IDTR_BASE, vcpu->idtr_base);
    CHECK_VMWRITE(GUEST_IDTR_LIMIT, vcpu->idtr_limit);

    /* MSRs */
    CHECK_VMWRITE(GUEST_IA32_EFER, vcpu->efer);

    CHECK_VMWRITE(GUEST_ACTIVITY_STATE, 0); 
    CHECK_VMWRITE(GUEST_INTERRUPTIBILITY_INFO, 0); 
    CHECK_VMWRITE(GUEST_PENDING_DBG_EXCEPTIONS, 0);
    CHECK_VMWRITE(VMCS_LINK_POINTER, 0xFFFFFFFFFFFFFFFFULL);
    return 0; 
}

void relm_cr3_cache_init(struct cr3_shadow_cache *cache)
{
    memset(cache, 0, sizeof(cache)); 
    spin_lock_init(&cache->lock); 
    pr_info("RELM: CR3 cache: initialised (candidates=%u slots, max_targets=%u)\n",
            RELM_CR3_CACHE_SIZE, RELM_CR3_MAX_TARGETS);
}

void relm_cr3_cache_policy_frequency(
    const struct cr3_target_entry *candidates,
    uint32_t                       n_candidates,
    uint64_t                      *selected_out,
    uint32_t                      *count_out)
{
    bool     picked[RELM_CR3_CACHE_SIZE] = { false };
    uint32_t n_selected = 0;
    uint32_t pass;
    uint32_t i;
    uint32_t best_idx;
    uint64_t best_count;
    uint64_t best_ts;
   
    uint32_t n_passes = (n_candidates < RELM_CR3_MAX_TARGETS)
                        ? n_candidates
                        : RELM_CR3_MAX_TARGETS;
 
    for(pass = 0; pass < n_passes; pass++)
    {
        best_idx   = RELM_CR3_CACHE_SIZE;
        best_count = 0;
        best_ts    = 0;
 
        for(i = 0; i < n_candidates; i++)
        {
            if(picked[i])
                continue;
 
            if(candidates[i].cr3_value == 0)
                continue;
 
            if(candidates[i].hit_count == 0)
                continue;
    
            if(candidates[i].hit_count > best_count ||
               (candidates[i].hit_count == best_count &&
                candidates[i].last_seen_ns > best_ts))
            {
                best_idx   = i;
                best_count = candidates[i].hit_count;
                best_ts    = candidates[i].last_seen_ns;
            }
        }
 
        /* If best_idx is still RELM_CR3_CACHE_SIZE, we found no valid
         * unpicked candidate in this pass. This happens when fewer than
         * n_passes non-empty non-picked entries exist (e.g. only 2 distinct
         * CR3 values have been observed so far). Stop the outer loop. */
        if(best_idx == RELM_CR3_CACHE_SIZE)
            break;
 
        /* Commit the winner from this pass to the output array.
         * selected_out[n_selected] receives the CR3 physical address. */
        selected_out[n_selected] = candidates[best_idx].cr3_value;
        n_selected++;
 
        /* Mark this index as picked so the next pass skips it.
         * Without this, every pass would select the same (globally best) entry. */
        picked[best_idx] = true;
    }
 
    /* Write the final count to the output variable.
     * The caller uses this to know how many of selected_out[] are valid. */
    *count_out = n_selected;
 
    PDEBUG("RELM: CR3 cache: frequency policy selected %u targets "
           "(from %u candidates)\n", n_selected, n_candidates);
}

void relm_cr3_cache_apply(struct cr3_shadow_cache *cache)
{
    uint64_t new_targets[RELM_CR3_MAX_TARGETS];
    uint32_t new_count = 0;
    uint32_t i, j;
 
    bool changed = false;
 
    relm_cr3_cache_policy_frequency(
        cache->candidates,        /* full candidate array, read-only        */
        cache->candidate_count,   /* number of populated entries            */
        new_targets,              /* output: up to 4 selected CR3 values    */
        &new_count                /* output: actual number written          */
    );
 
    if(new_count != cache->promoted_count)
    {
        changed = true;
    }
    else
    {
        for(i = 0; i < new_count; i++)
        {
            bool found_in_current = false;
 
            for(j = 0; j < RELM_CR3_CACHE_SIZE; j++)
            {
                if(cache->candidates[j].promoted &&
                   cache->candidates[j].cr3_value == new_targets[i])
                {
                    found_in_current = true;
                    break;
                }
            }
 
            if(!found_in_current)
            {
                /* This target is new — not in the current VMCS set */
                changed = true;
                break;
            }
        }
    }
 
    /* If nothing changed, return early to avoid unnecessary VMWRITE calls */
    if(!changed)
    {
        PDEBUG("RELM: CR3 cache: apply — no change to target set\n");
        return;
    }
 
    for(i = 0; i < RELM_CR3_CACHE_SIZE; i++)
    {
        /* Only clear entries that were actually promoted — no-op on others */
        if(cache->candidates[i].promoted)
        {
            cache->candidates[i].promoted = false;
            PDEBUG("RELM: CR3 cache: demoting CR3=0x%llx from VMCS slot\n",
                   cache->candidates[i].cr3_value);
        }
    }
 
    for(i = 0; i < new_count; i++)
    {
        if(_vmwrite(CR3_TARGET_VALUES[i], new_targets[i]) != 0)
        {
            pr_err("RELM: CR3 cache: VMWRITE CR3_TARGET_VALUE%u = 0x%llx failed\n",
                   i, new_targets[i]);
 
            _vmwrite(CR3_TARGET_COUNT, 0);
            cache->promoted_count = 0;
            return;
        }
 
        pr_info("RELM: CR3 cache: VMCS slot %u ← CR3=0x%llx\n",
                i, new_targets[i]);
    }
 
    if(_vmwrite(CR3_TARGET_COUNT, new_count) != 0)
    {
        pr_err("RELM: CR3 cache: VMWRITE CR3_TARGET_COUNT = %u failed\n",
               new_count);
        /* Disable targeting to leave VMCS in a safe state */
        _vmwrite(CR3_TARGET_COUNT, 0);
        cache->promoted_count = 0;
        return;
    }
 
    for(i = 0; i < RELM_CR3_CACHE_SIZE; i++)
    {
        if(cache->candidates[i].cr3_value == 0)
            continue;
 
        for(j = 0; j < new_count; j++)
        {
            if(cache->candidates[i].cr3_value == new_targets[j])
            {
                /* Mark as promoted — the eviction code will skip this entry */
                cache->candidates[i].promoted = true;
                break;
            }
        }
    }
 
    /* Record the new promoted count for use in Step 2 of future apply calls */
    cache->promoted_count = new_count;
 
    pr_info("RELM: CR3 cache: applied %u CR3 targets to VMCS "
            "(total exits seen: %llu)\n",
            new_count, cache->total_cr3_exits);
}
 

void relm_cr3_cache_record(struct vcpu *vcpu, uint64_t cr3_value)
{
    struct cr3_shadow_cache *cache = &vcpu->cr3_cache;
    uint64_t now_ns = ktime_get_ns();
    uint32_t found_idx = RELM_CR3_CACHE_SIZE;
    uint32_t evict_idx = RELM_CR3_CACHE_SIZE;
    uint64_t evict_count = U64_MAX;
    uint32_t empty_idx = RELM_CR3_CACHE_SIZE;
    uint32_t i;

    bool need_apply = false;
    cache->total_cr3_exits++;
    spin_lock(&cache->lock);
 
    for(i = 0; i < RELM_CR3_CACHE_SIZE; i++)
    {
        if(cache->candidates[i].cr3_value == cr3_value)
        {
            found_idx = i;
            break;
        }
 
        if(cache->candidates[i].cr3_value == 0 &&
           empty_idx == RELM_CR3_CACHE_SIZE)
        {
            empty_idx = i;
        }
 
        if(cache->candidates[i].cr3_value != 0 &&
           !cache->candidates[i].promoted   &&
           cache->candidates[i].hit_count < evict_count)
        {
            evict_idx   = i;
            evict_count = cache->candidates[i].hit_count;
        }
    }
 
    if(found_idx < RELM_CR3_CACHE_SIZE)
    {
        cache->candidates[found_idx].hit_count++;
        cache->candidates[found_idx].last_seen_ns = now_ns;
 
        PDEBUG("RELM: CR3 cache: HIT CR3=0x%llx count=%llu\n",
               cr3_value, cache->candidates[found_idx].hit_count);
 
        if(cache->candidates[found_idx].hit_count >= RELM_CR3_PROMOTE_THRESHOLD &&
           !cache->candidates[found_idx].promoted)
        {
            pr_info("RELM: CR3 cache: CR3=0x%llx crossed threshold "
                    "(count=%llu) — triggering promotion cycle\n",
                    cr3_value, cache->candidates[found_idx].hit_count);
            need_apply = true;
        }
 
        spin_unlock(&cache->lock);
        goto _check_apply;
    }
 
    if(empty_idx < RELM_CR3_CACHE_SIZE)
    {
        cache->candidates[empty_idx].cr3_value    = cr3_value;
        cache->candidates[empty_idx].hit_count    = 1;   /* first observation */
        cache->candidates[empty_idx].last_seen_ns = now_ns;
        cache->candidates[empty_idx].promoted     = false;
 
        cache->candidate_count++;
 
        PDEBUG("RELM: CR3 cache: INSERT CR3=0x%llx into slot %u "
               "(%u/%u slots used)\n",
               cr3_value, empty_idx,
               cache->candidate_count, RELM_CR3_CACHE_SIZE);
    }
    else if(evict_idx < RELM_CR3_CACHE_SIZE)
    {
        pr_info("RELM: CR3 cache: EVICT CR3=0x%llx (count=%llu) "
                "← replaced by new CR3=0x%llx\n",
                cache->candidates[evict_idx].cr3_value,
                cache->candidates[evict_idx].hit_count,
                cr3_value);
 
        cache->candidates[evict_idx].cr3_value    = cr3_value;
        cache->candidates[evict_idx].hit_count    = 1;
        cache->candidates[evict_idx].last_seen_ns = now_ns;
        cache->candidates[evict_idx].promoted     = false;
    }
    else
    {
        pr_err("RELM: CR3 cache: BUG: table full and no eviction candidate "
               "(promoted_count=%u) — dropping CR3=0x%llx\n",
               cache->promoted_count, cr3_value);
    }
 
    spin_unlock(&cache->lock);
 
_check_apply:
    if(need_apply)
        relm_cr3_cache_apply(&vcpu->cr3_cache);
}
 

int relm_cr3_cache_handle_exit(struct vcpu *vcpu, uint64_t exit_qual)
{
    uint32_t src_reg = (uint32_t)((exit_qual & CR_ACCESS_SOURCE_REG_MASK)
                                   >> CR_ACCESS_SOURCE_REG_SHIFT);
 
    uint64_t new_cr3;
    uint64_t instr_len;
    uint64_t guest_rip;
 
    const uint64_t *gpr_table[] = {
        vcpu->regs.rax,
        vcpu->regs.rcx,
        vcpu->regs.rdx,
        vcpu->regs.rbx,
        vcpu->regs.rsp,   /* guest RSP — distinct from host RSP */
        vcpu->regs.rbp,
        vcpu->regs.rsi,
        vcpu->regs.rdi,
        vcpu->regs.r8,
        vcpu->regs.r9,
        vcpu->regs.r10,
        vcpu->regs.r11,
        vcpu->regs.r12,
        vcpu->regs.r13,
        vcpu->regs.r14,
        vcpu->regs.r15,
    };
 
    /* Bounds-check src_reg. Values 0–15 are valid (4 bits from hardware).
     * A value outside this range would indicate a hardware bug or memory
     * corruption. We guard defensively. */
    if(src_reg > 15)
    {
        pr_err("RELM: CR3 cache: invalid source GPR %u in EXIT_QUALIFICATION=0x%llx\n",
               src_reg, exit_qual);
        instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
        guest_rip = __vmread(GUEST_RIP);
        _vmwrite(GUEST_RIP, guest_rip + instr_len);
        return 1;
    }
 
    new_cr3 = *gpr_table[src_reg];
 
    PDEBUG("RELM: CR3 exit: CR3=0x%llx from GPR%u (total exits=%llu)\n",
           new_cr3, src_reg, vcpu->cr3_cache.total_cr3_exits + 1);
 
    /* Update VMCS GUEST_CR3 so the guest's address space actually switches.
     * Without this VMWRITE, the guest would continue using the old page table
     * after resuming — all subsequent virtual address translations would use
     * the wrong PML4, causing page faults or executing the wrong code.
     *
     * VMCS encoding for GUEST_CR3: 0x6802 (natural-width guest-state field).
     * Also update vcpu->regs.rip-equivalent for consistency. */
    _vmwrite(0x6802, new_cr3);   /* GUEST_CR3 */
 
    relm_cr3_cache_record(vcpu, new_cr3);
 
    instr_len = __vmread(VM_EXIT_INSTRUCTION_LEN);
    guest_rip = __vmread(GUEST_RIP);
 
    _vmwrite(GUEST_RIP, guest_rip + instr_len);
 
    PDEBUG("RELM: CR3 exit: handled — RIP 0x%llx → 0x%llx\n",
           guest_rip, guest_rip + instr_len);
 
    return 1;
}
 
void relm_cr3_cache_dump(const struct cr3_shadow_cache *cache)
{
    uint32_t i;
 
    pr_info("RELM: CR3 cache dump:\n");
    pr_info("  total_cr3_exits  = %llu\n",  cache->total_cr3_exits);
    pr_info("  total_suppressions = %llu\n", cache->total_suppressions);
    pr_info("  candidate_count  = %u / %u\n",
            cache->candidate_count, RELM_CR3_CACHE_SIZE);
    pr_info("  promoted_count   = %u / %u (VMCS targets)\n",
            cache->promoted_count, RELM_CR3_MAX_TARGETS);
    pr_info("  candidates:\n");
 
    for(i = 0; i < RELM_CR3_CACHE_SIZE; i++)
    {
        if(cache->candidates[i].cr3_value == 0)
            continue;
 
        pr_info("    [%2u] CR3=0x%016llx  hits=%6llu  age_ns=%llu  %s\n",
                i,
                cache->candidates[i].cr3_value,
                cache->candidates[i].hit_count,
                cache->candidates[i].last_seen_ns,
                /* Mark promoted entries clearly so the reader can see which
                 * four are currently suppressing VM-exits in the VMCS */
                cache->candidates[i].promoted ? "PROMOTED ← in VMCS" : "candidate");
    }
}
 

int relm_init_vmcs_state(struct vcpu *vcpu)
{
    if(!vcpu)
        return -EINVAL; 

    int ret;

    PDEBUG("RELM: VCPU%d: setting up CR controls (CR0/CR4 constraints)\n",
           vcpu->vpid);
 
    if((ret = relm_setup_cr_controls(vcpu)) != 0)
    {
        pr_err("RELM: VCPU%d: CR controls setup failed: %d\n", vcpu->vpid, ret);
        return -1;
    }

    if((ret = relm_setup_host_state(vcpu)) != 0)
    {
        pr_err("Host state setup faile : err %d\n", ret); 
        return -1; 
    }
    if((ret = relm_setup_guest_state(vcpu)) != 0)
    {
        pr_err("Guest state setup failed : err %d\n", ret); 
        return -1; 
    }
   
    pr_info("RELM: VCPU%d: VMCS host+guest state written on CPU%d"
            " (CR0=0x%lx CR4=0x%lx RIP=0x%lx RSP=0x%lx)\n",
            vcpu->vpid, smp_processor_id(),
            vcpu->cr0, vcpu->cr4, vcpu->regs.rip, vcpu->regs.rsp);
 
    return 0; 
}

void relm_dump_vcpu(struct vcpu *vcpu)
{
    pr_info("\n*** Guest State ***\n\n");     

    pr_info("CR0: actual=0x%lx, shadow=0x%lx, mask=0x%lx\n",
            (unsigned long)__vmread(GUEST_CR0),
            (unsigned long)__vmread(CR0_READ_SHADOW), 
            (unsigned long)__vmread(CR0_GUEST_HOST_MASK)); 

    pr_info("CR4: actual=0x%lx, shadow=0x%lx, mask=0x%lx\n", 
            (unsigned long)__vmread(GUEST_CR4), 
            (unsigned long)__vmread(CR4_READ_SHADOW), 
            (unsigned long)__vmread(CR4_GUEST_HOST_MASK)); 

    pr_info("CR3: 0x%llx\n", (unsigned long)__vmread(GUEST_CR3));

    if((vcpu->controls.secondary_proc & VMCS_PROC2_ENABLE_EPT) &&
       (__vmread(GUEST_CR4) & X86_CR4_PAE) && 
        !(vcpu->controls.vm_entry & VM_ENTRY_IA32E_MODE))
    {
        pr_info("PDPTE0 = 0x%lx PDPTE1 = 0x%lx\n",
                (unsigned long)__vmread(GUEST_PDPTE(0)),
                (unsigned long)__vmread(GUEST_PDPTE(1))); 

        pr_info("PDPTE2 = 0x%lx PDPTE3 = 0x%lx\n", 
                (unsigned long)__vmread(GUEST_PDPTE(2)),
                (unsigned long)__vmread(GUEST_PDPTE(3))); 
    }

    pr_info("RSP = 0x%lx (0x%lx) RIP = 0x%lx (0x%lx)\n", 
            (unsigned long)__vmread(GUEST_RSP), 
            (unsigned long)vcpu->regs.rsp, 
            (unsigned long)__vmread(GUEST_RIP),
            (unsigned long)vcpu->regs.rip); 

    pr_info("RFLAGS=0x%lx (0x%lx) DR7 = 0x%lx\n", 
            (unsigned long)__vmread(GUEST_RFLAGS), 
            (unsigned long)vcpu->regs.rflags, 
            (unsigned long)__vmread(GUEST_DR7)); 

    pr_info("Sysenter RSP=0x%llx CS:RIP=0x%04:0x%llx\n", 
            (unsigned long)(__vmread(GUEST_SYSENTER_ESP) & 0xFFFFFFFF), 
            (unsigned long)__vmread(GUEST_SYSENTER_CS), 
            (unsigned long)(__vmread(GUEST_SYSENTER_EIP) & 0xFFFFFFFF)); 

    
   
    pr_info("\n*** Host State ***\n\n");

    pr_info("RIP = 0x%016llx (%ps)  RSP = 0x%016llx\n",
        __vmread(HOST_RIP),
        (void *)__vmread(HOST_RIP),
        __vmread(HOST_RSP));

    pr_info("CS=%04x SS=%04x DS=%04x ES=%04x FS=%04x GS=%04x TR=%04x\n",
        (u16)__vmread(HOST_CS_SELECTOR),
        (u16)__vmread(HOST_SS_SELECTOR),
        (u16)__vmread(HOST_DS_SELECTOR),
        (u16)__vmread(HOST_ES_SELECTOR),
        (u16)__vmread(HOST_FS_SELECTOR),
        (u16)__vmread(HOST_GS_SELECTOR),
        (u16)__vmread(HOST_TR_SELECTOR));

    pr_info("FSBase=0x%016llx GSBase=0x%016llx TRBase=0x%016llx\n",
        __vmread(HOST_FS_BASE),
        __vmread(HOST_GS_BASE),
        __vmread(HOST_TR_BASE));

    pr_info("GDTBase=0x%016llx IDTBase=0x%016llx\n",
        __vmread(HOST_GDTR_BASE),
        __vmread(HOST_IDTR_BASE));

    pr_info("CR0=0x%016llx CR3=0x%016llx CR4=0x%016llx\n",
        __vmread(HOST_CR0),
        __vmread(HOST_CR3),
        __vmread(HOST_CR4));

    pr_info("Sysenter ESP=0x%08x CS:EIP=%04x:0x%08x\n",
        (u32)__vmread(HOST_SYSENTER_ESP),
        (u32)__vmread(HOST_SYSENTER_CS),
        (u32)__vmread(HOST_SYSENTER_EIP));

    if (__vmread(VMCS_EXIT_CONTROLS) &
        (VMCS_EXIT_LOAD_EFER | VMCS_EXIT_LOAD_IA32_PAT)) {
        pr_info("EFER=0x%016llx PAT=0x%016llx\n",
            __vmread(HOST_IA32_EFER),
            __vmread(HOST_IA32_PAT));
    }

    if (__vmread(VMCS_EXIT_CONTROLS) &
        VM_EXIT_LOAD_PERF_GLOBAL_CTRL){
        pr_info("PerfGlobalCtrl=0x%016llx\n",
            __vmread(HOST_IA32_PERF_GLOBAL_CTRL));
    }
     
}
