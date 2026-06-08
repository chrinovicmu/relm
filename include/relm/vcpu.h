#ifndef RELM_VCPU_H
#define RELM_VCPU_H

#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>

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
    uint16 vpid;    /*virtual processor id*/ 
    int target_cpu_id; 

    enum vcpu_state state; 
    bool halted; 
    int launched; 
    
    struct task_struct *host_task; 
    void *host_stack; 

    spinlock_t lock; 
    wait_queue_head_t; 

    const struct vcpu_arch_ops *ops; 

    struct vcpu_stats stats; 

    struct vcpu_arch arch ; 
};

struct vcpu_arch_ops
{
    /*initialize arch-specific vcpu state*/ 
    int (*vcpu_init)(struct vcpu *vcpu); 

    /*tear down and free everything allocated by vcpu_init*/ 
    void (*vcpu_destroy)(struct vcpu *vcpu); 

    /*enter guest, return on vm-exit. the arch layer is
    * responsible for saving and restoring guest_regs and populating*/ 
    int (*vcpu_run)(struct vcpu *vcpu); 

    /*generic exit dispather after decoding exit reason 
     * exit reason from vcpu->arch, lets arch layer handle it's own exits*/
    int (*handle_exit)(struct vcpu *vcpu); 

    /*sets up memory virtualization layer */ 
    int (*setup_mmu)(struct vcpu *vcpu); 

    /*insert virtual interrupt into guest*/ 
    int (*inject_irq)(struct vcpu, unsigned int vector); 

    /*dump arch registers to kernel log for debugging*/ 
    int (*dump_regs)(struct *vcpu); 

}; 

struct vcpu *relm_vcpu_create(struct relm_vm *vm, int vcpu_id);
void relm_vcpu_destroy(struct vcpu *vcpu);
int relm_vcpu_run(struct vcpu *vcpu);
int relm_vcpu_stop(struct vcpu *vcpu);
struct vcpu *relm_get_current_vcpu(void);

#endif /* RELM_VCPU_H *









}
