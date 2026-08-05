#ifndef VMX_H
#define VMX_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <vmx_ops.h>
#include <vmcs.h>
#include <ept.h>
#include <apic.h>

/*
 * The authoritative 'struct vcpu' lives in <relm/vcpu.h>; the VMX-specific
 * fields it embeds live in 'struct vcpu_arch' (<arch/x86/vmx/vcpu_arch.h>).
 * This header only needs the tag to declare prototypes taking 'struct vcpu *'.
 */
struct vcpu;

#define VPID_TO_INDEX(vpid) ((vpid) -1)
#define INDEX_TO_VPID(index) ((index) + 1)
#define VPID_IS_VALID(vpid, max) \
    ((vpid) > 0 && (vpid) <= (max))

#define HOST_STACK_ORDER    2  
#define HOST_STACK_SIZE     (PAGE_SIZE << HOST_STACK_ORDER) // 16 KB

/* RELM_MAX_MANAGED_MSRS is defined authoritatively in <arch/x86/vmx/vcpu_arch.h>
 * (value 16). It was previously duplicated here as 8, which collided with that
 * definition (redefinition error) since vcpu_arch.h includes this header. */
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
    uint64_t cr3_value; 
    uint64_t hit_count; 
    uint64_t last_seen_ns; 
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

/*
 * NOTE: 'struct vcpu_stats', 'struct guest_regs', 'enum vcpu_state' and the
 * flat 'struct vcpu' used to be defined here. They were pre-refactor
 * duplicates of the authoritative definitions in <relm/vcpu.h> and
 * <arch/x86/vmx/vcpu_arch.h>, and collided with them (this header is included
 * by vcpu_arch.h). They have been removed; all VMX-specific per-vCPU fields now
 * live in 'struct vcpu_arch' (accessed as vcpu->arch.*).
 */

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
