#ifndef RELM_ARCH_X86_VMX_VM_ARCH_H
#define RELM_ARCH_X86_VMX_VM_ARCH_H

#include <include/arch/x86/vmx/ept.h>
#include <include/relm/iommu.h>

struct relm_vm_arch
{
    struct ept_context *ept; 
    uint64_t pml4_gpa; 

    uint64_t kernel_entry_gpa; 
    uint64_t boot_params_gpa; 

    struct relm_iommu_context iommu; 
}; 

extern const struct relm_vm_operations vmx_vm_ops; 

#endif 
