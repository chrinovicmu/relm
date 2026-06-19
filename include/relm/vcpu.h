#ifndef RELM_VCPU_H
#define RELM_VCPU_H

#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <relm_arch/vcpu_arch.h>

#define RELM_HOST_STACK_ORDER   2

struct relm_vm;
struct vcpu_arch_ops; 

enum vcpu_state {
    VCPU_STATE_UNINITIALIZED,
    VCPU_STATE_INITIALIZED,
    VCPU_STATE_RUNNING,
    VCPU_STATE_HALTED,      /* HLT instruction executed */
    VCPU_STATE_STOPPED,     /* requested stop, clean */
    VCPU_STATE_SHUTDOWN,    /* guest requested shutdown */
    VCPU_STATE_ERROR,       /* unrecoverable fault */
};


struct vcpu_stats {
    uint64_t total_exits;
    uint64_t hypercalls;
    uint64_t hlt_exits;
    uint64_t cpuid_exits;
    uint64_t io_exits;
    uint64_t mmio_exits;
    uint64_t start_time_ns;
    uint64_t end_time_ns;
};

struct vcpu 
{
    struct relm_vm *vm; 
    uint16_t vpid;    /*virtual processor id*/ 
    int target_cpu_id; 

    enum vcpu_state state; 
    bool halted; 
    int launched; 
    
    struct task_struct *host_task; 
    void *host_stack; 

    spinlock_t lock; 
    wait_queue_head_t wq; 

    const struct vcpu_arch_ops *ops; 

    struct vcpu_stats stats; 

    struct vcpu_arch arch ; 
};

struct vcpu_arch_ops {

    /*
     * vcpu_alloc — arch-specific Phase 1 initialisation.
     *
     * struct vcpu is allocated and its identity fields are set.
     * The arch uses this to allocate everything it needs:
     *   VMX:  VMCS region, IO bitmap, MSR bitmap, MSR areas,
     *         exec controls, APIC pages, host_rsp.
     *   SVM:  VMCB, MSRPM, IOPM.
     *   ARM:  VGIC config, sysreg shadow allocation.
     *
     * Returns 0 on success, negative errno on failure.
     * On failure the generic layer frees host_stack and the vcpu struct.
     */
    int  (*vcpu_alloc)(struct vcpu *vcpu);

    /*
     * vcpu_init — arch Phase 2, runs ON the pinned host CPU.
     *
     * On VMX: VMCLEAR, VMPTRLD, write all VMCS host+guest fields.
     * Must be called from the vCPU kthread after CPU pinning.
     * Phase 1 (vcpu_alloc) can run on any CPU; Phase 2 cannot.
     */
    int  (*vcpu_init)(struct vcpu *vcpu);

    /* Enter the guest. Returns on every exit. */
    int  (*vcpu_run)(struct vcpu *vcpu);

    /* Handle an exit. Returns 1 = handled, 0 = pass to generic. */
    int  (*handle_exit)(struct vcpu *vcpu);

    /* Free everything allocated by vcpu_alloc. */
    void (*vcpu_free)(struct vcpu *vcpu);

    /* Free CPU-bound state allocated by vcpu_init (e.g. VMCLEAR). */
    void (*vcpu_destroy)(struct vcpu *vcpu);

    int  (*setup_mmu)(struct vcpu *vcpu);
    int  (*inject_irq)(struct vcpu *vcpu, unsigned int vector);
    int  (*read_msr)(struct vcpu *vcpu, uint32_t msr, uint64_t *val);
    int  (*write_msr)(struct vcpu *vcpu, uint32_t msr, uint64_t val);
    void (*dump_regs)(struct vcpu *vcpu);
};

struct vcpu *relm_vcpu_create(struct relm_vm *vm, int vcpu_id, 
                              const struct vcpu_arch_ops *arch_ops);
void relm_vcpu_destroy(struct vcpu *vcpu);

int relm_vcpu_run(struct vcpu *vcpu);
int relm_vcpu_stop(struct vcpu *vcpu);

struct vcpu *relm_get_current_vcpu(void);

int  relm_vcpu_pin_to_cpu(struct vcpu *vcpu, int target_cpu_id);
void relm_vcpu_unpin_and_stop(struct vcpu *vcpu);

#endif /* RELM_VCPU_H */ 






