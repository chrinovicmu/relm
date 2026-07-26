#ifndef RELM_ARCH_X86_SVM_VM_ARCH_H
#define RELM_ARCH_X86_SVM_VM_ARCH_H
#include <linux/types.h>
#include <include/relm/iommu.h>

struct npt_context;

struct relm_vm_arch 
{
    struct npt_context *npt; 

    /*guest physical address of it's own top-level page table. 
     * relm boot code puts it in CR3*/ 
    uint64_t pml4_gpa; 

    uint64_t kernel_entry_gpa; 
    uint64_t boot_params_gpa; 

    struct relm_iommu_context iommu; 
}; 

extern const struct relm_vm_operations svm_vm_ops;
#define RELM_ARCH_VM_OPS svm_vm_ops; 

extern const struct relm_mem_ops svm_mem_ops; 
#define RELM_ARCH_MEM_OPS svm_mem_ops; 

#endif 


