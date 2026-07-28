#ifndef SVM_H
#define SVM_H


#include <linux/types.h>
#include <linux/spinlock.h>

#include <vmcb.h>   
#include <npt.h>    

struct relm_vm;   
struct vcpu;      

#ifndef MSR_EFER
#define MSR_EFER            0xC0000080  /* same MSR VMX's EFER writes hit;
                                         * SVM claims one more bit in it   */
#endif

#ifndef EFER_SVME
#define EFER_SVME           (1ULL << 12) 
#endif

#ifndef MSR_VM_CR
#define MSR_VM_CR           0xC0010114  /* BIOS-level SVM lock/disable knob */
#endif 

/* VM_CR bit 3: once set (usually alongside SVMDIS by BIOS), EFER.SVME can
 * never be set again until the next reset — software has no override. */
#ifndef MSR_VM_CR_LOCK
#define MSR_VM_CR_LOCK      (1ULL << 3)
#endif

/* VM_CR bit 4: SVM disabled. With LOCK also set this is permanent; without
 * LOCK it is merely BIOS's current choice and harmless for us to check but
 * not expected to be encountered on hardware RELM targets. */
#ifndef MSR_VM_CR_SVMDIS
#define MSR_VM_CR_SVMDIS    (1ULL << 4)
#endif

#ifndef MSR_VM_HSAVE_PA
#define MSR_VM_HSAVE_PA     0xC0010117  /* per-(pinned-)vCPU: physical addr
                                         * of the host-save-area page (see
                                         * 'hsave' in vcpu_arch.h)          */
#endif

#define SVM_ASID_FROM_VPID(vpid)   ((uint32_t)(vpid) + 1)

bool relm_svm_check_support(void);
void relm_enable_svm_operation(void);
int relm_svm_enable_on_all_cpus(void);
void relm_svm_disable_on_all_cpus(void);

/*phase 1 : allocate VMCB structure */ 
struct vcpu *relm_vcpu_alloc_init(struct relm_vm *vm, int vcpu_id);

/*fill in the VMCB control area 
 * offsets 0x00-0x3FF*/  
int relm_vcpu_vmcb_setup(struct vcpu *vcpu);

/*                                                         
 * relm_init_vmcb_state() — fill in the VMCB SAVE area (vmcb.h, offsets
 * 0x400+) with the guest's INITIAL architectural state*/
int relm_init_vmcb_state(struct vcpu *vcpu);

void relm_free_vcpu(struct vcpu *vcpu);
void relm_dump_vcpu(struct vcpu *vcpu);

#endif
