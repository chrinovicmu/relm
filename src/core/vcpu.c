#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/errno.h>
#include <linux/cpumask.h>
#include <linux/printk.h>
 
#include <include/relm/vcpu.h>
#include <include/relm/vm.h>
#include <utils/utils.h>

/* Per-CPU current vCPU tracking.
 *
 * Stores a pointer to whichever struct vcpu is currently executing on
 * each logical CPU. Set to the vcpu before VM-entry, cleared on exit.
 * This lets the VM-exit handler find the vcpu without passing it as a
 * parameter through the assembly stub.
 *
 * Declared here (not in vmx.c) because the tracking itself is generic —
 * every arch's exit handler uses this same mechanism. */ 

DEFINE_PER_CPU(struct vcpu *, current_vcpu);
 
static inline void relm_set_current_vcpu(struct vcpu *vcpu)
{
    this_cpu_write(current_vcpu, vcpu);
}
 
struct vcpu *relm_get_current_vcpu(void)
{
    return this_cpu_read(current_vcpu);
}

struct vcpu *relm_vcpu_create(struct relm_vm *vm, int vpid, 
                              const struct vcpu_arch_ops *arch_ops)
{
    struct vcpu *vcpu; 
    int ret; 

    if(!vm || !arch_ops || !arch_ops->vcpu_alloc)
        return ERR_PTR(-EINVAL); 

    /*generic allocation*/ 

    vcpu = kzalloc(sizeof(*vcpu), GFP_KERNEL); 
    if(!vcpu)
        return ERR_PTR(-ENOMEM); 

    vcpu->vm = vm; 
    vcpu->vpid = vpid; 
    vcpu->state = VCPU_STATE_UNINITIALIZED;
    vcpu->ops = arch_ops; 

    spin_lock_init(&vcpu->lock); 
    init_waitqueue_head(&vcpu->wq); 

    /*allocate host kernel stack for vCPU thread to execute on*/ 
    vcpu->host_stack = (void*)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 
                                               RELM_HOST_STACK_ORDER); 

    if (!vcpu->host_stack) {
        pr_err("RELM: VCPU%d: failed to allocate host stack\n", vpid);
        ret = -ENOMEM;
        goto _out_free_vcpu;
    }

    /* Arch-specific Phase 1 
     * vcpu_alloc sets up everything arch-specific that can be done on
     * any CPU: VMCS allocation, bitmaps, MSR areas, APIC pages, host_rsp.
     * It must not touch VMCS fields — that requires VMPTRLD which needs
     * the pinned CPU (Phase 2, vcpu_init).
     */
    ret = vcpu->ops->vcpu_alloc(vcpu);
    if (ret != 0) {
        pr_err("RELM: VCPU%d: arch vcpu_alloc failed: %d\n", vpid, ret);
        goto _out_free_stack;
    }

    vcpu->state = VCPU_STATE_INITIALIZED;
    pr_info("RELM: VCPU%d: Phase 1 complete\n", vpid);
    return vcpu;

_out_free_stack:
    free_pages((unsigned long)vcpu->host_stack, RELM_HOST_STACK_ORDER);
_out_free_vcpu:
    kfree(vcpu);
    return ERR_PTR(ret);
}

void relm_vcpu_free(struct vcpu *vcpu)
{
    if (!vcpu)
        return;

    if (vcpu->ops && vcpu->ops->vcpu_free)
        vcpu->ops->vcpu_free(vcpu);

    if (vcpu->host_stack)
        free_pages((unsigned long)vcpu->host_stack, RELM_HOST_STACK_ORDER);

    kfree(vcpu);
}

/* CPU pinning — generic, no arch knowledge needed.
 *
 * relm_vcpu_pin_to_cpu restricts the vCPU kthread to a single logical
 * CPU using set_cpus_allowed_ptr, then spins on schedule() until the
 * scheduler actually migrates us there. This is necessary because VMX
 * operations (VMCLEAR, VMPTRLD) are per-CPU — a VMCS loaded on CPU N
 * must be cleared on CPU N before another CPU can use it.
 *
 * relm_vcpu_unpin_and_stop restores the full online CPU mask and stops
 * the kthread cleanly.
 */ 
int relm_vcpu_pin_to_cpu(struct vcpu *vcpu, int target_cpu_id)
{
    cpumask_t mask;
    int ret;
 
    if (!vcpu || !vcpu->host_task) {
        pr_err("RELM: VCPU%d: cannot pin — no host task\n",
               vcpu ? vcpu->vpid : -1);
        return -EINVAL;
    }
 
    if (!cpu_possible(target_cpu_id)) {
        pr_err("RELM: VCPU%d: target CPU%d is not possible\n",
               vcpu->vpid, target_cpu_id);
        return -EINVAL;
    }
 
    if (!cpu_online(target_cpu_id)) {
        pr_err("RELM: VCPU%d: target CPU%d is not online\n",
               vcpu->vpid, target_cpu_id);
        return -EINVAL;
    }
 
    PDEBUG("RELM: VCPU%d: pinning to CPU%d (currently on CPU%d)\n",
           vcpu->vpid, target_cpu_id, smp_processor_id());
 
    cpumask_clear(&mask);
    cpumask_set_cpu(target_cpu_id, &mask);
 
    /*
     * set_cpus_allowed_ptr marks the task as only runnable on target_cpu_id.
     * The scheduler will migrate us on the next preemption point —
     * typically the schedule() call below.
     */
    ret = set_cpus_allowed_ptr(vcpu->host_task, &mask);
    if (ret != 0) {
        pr_err("RELM: VCPU%d: set_cpus_allowed_ptr to CPU%d failed: %d\n",
               vcpu->vpid, target_cpu_id, ret);
        return ret;
    }
 
    /*
     * Yield until we are actually running on the target CPU.
     * set_cpus_allowed_ptr guarantees eventual migration but not
     * immediate — we must wait for the scheduler to act.
     */
    schedule();
 
    while (smp_processor_id() != target_cpu_id) {
        pr_warn("RELM: VCPU%d: not yet on CPU%d (on CPU%d), retrying\n",
                vcpu->vpid, target_cpu_id, smp_processor_id());
        schedule();
    }
 
    pr_info("RELM: VCPU%d: pinned to CPU%d\n",
            vcpu->vpid, smp_processor_id());
 
    return 0;
}
 
void relm_vcpu_unpin_and_stop(struct vcpu *vcpu)
{
    if (!vcpu || !vcpu->host_task)
        return;
 
    PDEBUG("RELM: VCPU%d: unpinning and stopping\n", vcpu->vpid);
 
    /* Restore full CPU affinity before stopping so the thread can
     * be scheduled anywhere to complete its exit path. */
    set_cpus_allowed_ptr(vcpu->host_task, cpu_online_mask);
    kthread_stop(vcpu->host_task);
}
 

/* relm_vcpu_loop - the VCPU execution thread 
 *this is the body of the kdthread created by relm_run_vpcu()
 * pins CPU 
 * arch phase 2 (write memory strucutres to registers)
 * guest exectuion loop: enter guest -> handle exit -> repeat 
 * arch cleanup 
 * */ 

static int relm_vcpu_loop(void *data)
{
    struct vcpu *vcpu = (struct vcpu*)data; 
    int ret; 

    PDEBUG("RELM: VCPU%d thread starting on CPU%d\n",
            vcpu->vpid, smp_processor_id());

    /*
     * Register as current vCPU before pinning so that any pre-pin
     * callbacks can find us. Re-registered after pinning (post-migration
     * the per-CPU variable on the old CPU no longer matters).
     */
    relm_set_current_vcpu(vcpu); 

    ret = relm_vcpu_pin_to_cpu(vcpu, vcpu->target_cpu_id);
    if (ret != 0) {
        pr_err("RELM: VCPU%d: CPU pinning failed: %d\n",
               vcpu->vpid, ret);
        vcpu->state = VCPU_STATE_ERROR;        
        goto _out_clear_vcpu;   
    }

    relm_set_current_vcpu(vcpu);

    PDEBUG("RELM: VCPU%d: running on CPU%d\n",
        vcpu->vpid, smp_processor_id());

    if(!vcpu->ops || !vcpu->ops->vcpu_init)
    {
        pr_err("RELM: VCPU%d: no vcpu_init op — cannot run\n", vcpu->vpid);
        vcpu->state = VCPU_STATE_ERROR;
        goto _out_clear_vcpu;
    }

    /*
     * Arch Phase 2 — MUST run on the pinned CPU. This is where the VMCS is
     * made current (VMCLEAR/VMPTRLD) and every host+guest VMCS field is
     * written. The previous code checked a nonexistent `ops->init` member and
     * then fell straight into the run loop WITHOUT ever calling it, so VM-entry
     * executed against a completely unprogrammed VMCS — the guest could never
     * boot. We now actually invoke vcpu_init and fail cleanly if it errors.
     */
    ret = vcpu->ops->vcpu_init(vcpu);
    if (ret != 0) {
        pr_err("RELM: VCPU%d: arch vcpu_init (Phase 2 VMCS setup) failed: %d\n",
               vcpu->vpid, ret);
        vcpu->state = VCPU_STATE_ERROR;
        goto _out_arch_destroy;
    }

    vcpu->state = VCPU_STATE_RUNNING;

    while(!kthread_should_stop()){
        
        /* Enter the guest.
         * The arch layer is responsible for:
         * - Saving guest GPRs into vcpu->arch.regs before returning
         * - Populating vcpu->arch.exit_reason and exit_qualification
         
         * Returns 0 on a normal exit, negative on a fatal error
         * (VMLAUNCH/VMRESUME failure).
         */
        ret = vcpu->ops->vcpu_run(vcpu); 
        if (ret != 0) 
        {
            pr_err("RELM: VCPU%d: vcpu_run failed (ret=%d) — "
                   "VM entry error\n", vcpu->vpid, ret);
            if (vcpu->ops->dump_regs)
                vcpu->ops->dump_regs(vcpu); 
            vcpu->state = VCPU_STATE_ERROR;
            break;   /* Fatal VM-entry error — exit the loop */ 
        }

        /*generic exit handling 
         *for HLT, CPUID, hypercalls, I/O needing device emulation
         * */ 
        ret = vcpu->ops->handle_exit(vcpu); 
        if (ret < 0) 
        {
            pr_err("RELM: VCPU%d: handle_exit fatal: %d\n",
                   vcpu->vpid, ret);
            vcpu->state = VCPU_STATE_ERROR;
            break;
        }

        vcpu->stats.total_exits++; 
        vcpu->vm->stats.total_exits++; 

        /*Check if something (stop request, shutdown hypercall) changed
         * the state out from under us during exit handling. */
        if (vcpu->state != VCPU_STATE_RUNNING) {
            pr_info("RELM: VCPU%d: state changed to %d, "
                    "exiting loop after %llu exits\n",
                    vcpu->vpid, vcpu->state,
                    vcpu->stats.total_exits);
            break;
        }

        /* HLT handling.
         *
         * If handle_exit set vcpu->halted (HLT exit), sleep on the
         * wait queue until an interrupt is injected or the vCPU is
         * stopped. The wakeup path clears vcpu->halted before calling
         * wake_up().
         */
        if (vcpu->halted) {
            PDEBUG("RELM: VCPU%d: halted, sleeping on wq\n", vcpu->vpid);
            /* Sleep until woken by interrupt injection or stop request */
            wait_event_interruptible(vcpu->wq,
                !vcpu->halted || kthread_should_stop());
            PDEBUG("RELM: VCPU%d: woken from halt\n", vcpu->vpid);
        }
    }

    pr_info("RELM: VCPU%d: execution loop exiting "
            "(total exits: %llu)\n",
            vcpu->vpid, vcpu->stats.total_exits);

_out_arch_destroy:
    if (vcpu->ops && vcpu->ops->vcpu_destroy)
        vcpu->ops->vcpu_destroy(vcpu);

    if (vcpu->state != VCPU_STATE_ERROR)
        vcpu->state = VCPU_STATE_STOPPED;

_out_clear_vcpu:
    relm_set_current_vcpu(NULL);
    pr_info("RELM: VCPU%d: thread exiting on CPU%d\n",
            vcpu->vpid, smp_processor_id());

    return ret; 
}


/* relm_vcpu_run — start the vCPU kthread.
 *
 * Creates a kthread that runs relm_vcpu_loop(). The thread does its own
*/ 

int relm_vcpu_run(struct vcpu *vcpu)
{
    long err;

    if (!vcpu) {
        pr_err("RELM: relm_vcpu_run: NULL vcpu\n");
        return -EINVAL;
    }

    if (vcpu->host_task) {
        pr_err("RELM: VCPU%d: already running\n", vcpu->vpid);
        return -EBUSY;
    }

    if (vcpu->state != VCPU_STATE_INITIALIZED) {
        pr_err("RELM: VCPU%d: not in INITIALIZED state (state=%d)\n",
               vcpu->vpid, vcpu->state);
        return -EINVAL;
    }

    /* Reset runtime state for a fresh start */
    vcpu->halted = false;
    vcpu->stats.total_exits = 0;
    vcpu->stats.start_time_ns = ktime_to_ns(ktime_get());

    pr_info("RELM: VCPU%d: creating kthread\n", vcpu->vpid);

    vcpu->host_task = kthread_create(
        relm_vcpu_loop,
        vcpu, 
        "relm_vm%d_vcpu%u", 
        vcpu->vm->vm_id, 
        (unsigned int)vcpu->vpid 
    ); 

    if (IS_ERR(vcpu->host_task)) 
    {
        err = PTR_ERR(vcpu->host_task);
        pr_err("RELM: VCPU%d: kthread_create failed: %ld\n",
               vcpu->vpid, err);
        vcpu->host_task = NULL;
        return (int)err;
    }

    wake_up_process(vcpu->host_task);   /* Start the newly created thread */

    PDEBUG("RELM: VCPU%d: kthread started\n", vcpu->vpid);

    return 0;
}

int relm_vcpu_stop(struct vcpu *vcpu)
{
    if (!vcpu)
        return -EINVAL;

    if (!vcpu->host_task) {
        pr_warn("RELM: VCPU%d: stop called but not running\n",
                vcpu->vpid);
        return 0;
    }
    pr_info("RELM: VCPU%d: stopping\n", vcpu->vpid);

    relm_vcpu_unpin_and_stop(vcpu);
    vcpu->host_task = NULL; 
    vcpu->stats.end_time_ns = ktime_to_ns(ktime_get());

    pr_info("RELM: VCPU%d: stopped (total exits: %llu)\n",
            vcpu->vpid, vcpu->stats.total_exits);

    return 0;
}

