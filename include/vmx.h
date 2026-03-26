#ifndef VMX_H
#define VMX_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <include/vmx_ops.h>
#include <include/vmcs.h>
#include <include/ept.h> 
#include <include/apic.h>

#define VPID_TO_INDEX(vpid) ((vpid) -1)
#define INDEX_TO_VPID(index) ((index) + 1)
#define VPID_IS_VALID(vpid, max) \
    ((vpid) > 0 && (vpid) <= (max))

#define HOST_STACK_ORDER    2  
#define HOST_STACK_SIZE     (PAGE_SIZE << HOST_STACK_ORDER) // 16 KB

#define RELM_MAX_MANAGED_MSRS   8 
#define RELM_CR3_MAX_TARGETS    4
#define RELM_CR3_CACHE_SIZE     16 
#define RELM_CR3_PROMOTE_THRESHOLD 10 


#define CR_ACCESS_CR_NUMBER_MASK    0x0000000FU  /* bits 3:0  = CR number    */
#define CR_ACCESS_CR_NUMBER_CR3     3            /* we only handle CR3        */
#define CR_ACCESS_TYPE_MASK         0x00000030U  /* bits 5:4  = access type  */
#define CR_ACCESS_TYPE_WRITE        0x00000000U  /* 00 = MOV to CR           */
#define CR_ACCESS_SOURCE_REG_MASK   0x00000F00U  /* bits 11:8 = source GPR   */
#define CR_ACCESS_SOURCE_REG_SHIFT  8
 
static const uint32_t CR3_TARGET_VALUES[RELM_CR3_MAX_TARGETS] = {
    CR3_TARGET_VALUE0,
    CR3_TARGET_VALUE1,
    CR3_TARGET_VALUE2,
    CR3_TARGET_VALUE3,
};

/*cr3 target cache entry */ 
struct cr3_target_entry
{
    /*gpa written to cr3*/ 
    uint64_t cr3_value; 

    /*times the guest wrote this exact cr3 value */  
    uint64_t hit_count; 
    
    /*timestamp of most recent hit*/ 
    uint64_t last_seen_ns; 

    /*is currently in a slot*/ 
    bool promoted; 
}; 

struct cr3_shadow_cache
{
    struct cr3_target_entry candidates[RELM_CR3_CACHE_SIZE]; 
    uint64_t candidate_count; 
    uint32_t promoted_count; 
    uint64_t total_cr3_exits; 
    uint64_t total_suppressions; 
    spinlock_t lock; 
};

struct relm_vm;  // forward declaration

struct vcpu_stats
{
    uint64_t total_exits; 
    uint64_t hypercalls; 
    uint64_t hlt_exits;
    uint64_t cpuid_exits;
    uint64_t start_time_ns; 
    uint64_t end_time_ns; 

}; 
/* Guest registers */
struct guest_regs {

    unsigned long rax;
    unsigned long rbx;
    unsigned long rcx;
    unsigned long rdx;
    unsigned long rsi;    
    unsigned long rdi;    
    unsigned long rbp;    
    unsigned long rsp;
    
    unsigned long r8;
    unsigned long r9;
    unsigned long r10;
    unsigned long r11;
    unsigned long r12;
    unsigned long r13;
    unsigned long r14;
    unsigned long r15;

    unsigned long rip;
    unsigned long rflags;

    unsigned long cs, ds, es, fs, gs, ss;

    unsigned long fs_base;
    unsigned long gs_base;
} __attribute__((packed));

enum vcpu_state {
    VCPU_STATE_UNINITIALIZED, 
    VCPU_STATE_INITIALIZED, 
    VCPU_STATE_RUNNING, 
    VCPU_STATE_HALTED, 
    VCPU_STATE_STOPPED, 
    VCPU_STATE_SHUTDOWN, 
    VCPU_STATE_ERROR
}; 

/* Virtual CPU structure */
struct vcpu {

    struct relm_vm *vm;
    struct host_cpu *hcpu;

    uint16_t vpid;
    int target_cpu_id;  

    int launched; 
    enum vcpu_state state; 
    bool halted;

    struct virt_apic apic; 

    spinlock_t lock; 
    wait_queue_head_t wq; 

    struct task_struct *host_task; 

    void *host_stack; 
    uint64_t host_rsp; 

    uint64_t vmentry_host_rsp; 
    struct vmcs_region *vmcs;
    uint64_t vmcs_pa;

    struct vmx_exec_ctrls controls; 

    void *msr_bitmap;
    uint64_t msr_bitmap_pa; 

    uint8_t *io_bitmap;
    uint64_t io_bitmap_pa;

    uint32_t exception_bitmap; 

    /*MSR managment */ 
    struct msr_entry *vmexit_store_area; 
    uint64_t vmexit_store_pa;

    struct msr_entry *vmexit_load_area; 
    uint64_t vmexit_load_pa; 

    struct msr_entry *vmentry_load_area; 
    uint64_t vmentry_load_pa;

    uint32_t msr_indices[RELM_MAX_MANAGED_MSRS]; 
    uint32_t msr_count; 

    size_t vmexit_count;
    size_t vmentry_count; 

    struct guest_regs regs;

    unsigned long cr0, cr3, cr4, cr8;
    unsigned long efer;

    uint64_t gdtr_base; 
    u16 gdtr_limit; 

    uint64_t idtr_base; 
    u16 idtr_limit; 

    uint64_t exit_reason;
    uint64_t exit_qualification;
    struct cr3_shadow_cache cr3_cache;
    struct vcpu_stats stats; 

};

struct host_cpu
{
    int logical_cpu_id; 
    struct vmxon_region *vmxon; 
    uint64_t vmxon_pa; 

    int vpcu_count; 
    struct vcpu **vcpus; 

    spinlock_t lock; 
};

typedef void (*cr3_eviction_policy_fn)(
    const struct cr3_target_entry *candidates,
    uint32_t                       n_candidates,
    uint64_t                      *selected_out,
    uint32_t                      *count_out); 

inline bool relm_vmx_support(void);
inline void relm_enable_vmx_operation(void);
bool relm_setup_feature_control(void);
int relm_vmx_enable_on_all_cpus(void); 
void relm_vmx_disable_on_all_cpus(void);

struct host_cpu *relm_get_per_cpu_hcpu(void);
struct vcpu *relm_vcpu_alloc_init(struct relm_vm *vm, int vcpu_id);

int relm_vcpu_vmcs_setup(struct vcpu *vcpu);
int relm_vcpu_pin_to_cpu(struct vcpu *vcpu, int target_cpu_id);
void relm_vcpu_unpin_and_stop(struct vcpu *vcpu);

int relm_vmclear(struct vcpu *vcpu); 
int relm_vmptrld(struct vcpu *vcpu); 
void relm_free_vcpu(struct vcpu *vcpu);

int relm_init_vmcs_state(struct vcpu *vcpu);
void relm_dump_vcpu(struct vcpu *vcpu); 

void relm_cr3_cache_init(struct cr3_shadow_cache *cache); 
void relm_cr3_cache_record(struct vcpu *vcpu, uint64_t cr3_value);
void relm_cr3_cache_apply(struct cr3_shadow_cache *cache);
int relm_cr3_cache_handle_exit(struct vcpu *vcpu, uint64_t exit_qual);
void relm_cr3_cache_policy_frequency(
    const struct cr3_target_entry *candidates,
    uint32_t                       n_candidates,
    uint64_t                      *selected_out,
    uint32_t                      *count_out);
 
void relm_cr3_cache_dump(const struct cr3_shadow_cache *cache);
 
#endif /* VMX_H */
