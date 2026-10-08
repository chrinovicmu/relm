#ifndef RELM_ARCH_ARM64_VM_ARCH_H
#define RELM_ARCH_ARM64_VM_ARCH_H

#include <linux/mutex.h>
#include <linux/types.h>

#include "arm_features.h"
#include <include/relm/iommu.h>


struct  relm_vm_operations; 
struct  relm_mem_ops; 


struct relm_vm_arch{

    struct relm_stage2 *stage2; 
    struct relm_vgic *vgic; 
    const struct relm_arm64_guest_features *features; 
    u64 kerne_entry_ipa; 
    u64 dtb_ipa; 
    u64 dtb_size; 
    u64 initrd_ipa; 
    u64 initrd_size; 

    /*True after the direct linux boot loader has validated the image
    * DBT, and optional initrd metadata and guest memory contents*/ 
    bool boot_prepared; 

    u32 psci_version; 
    u32 smccc_version; 

    struct relm_iommu_context iommu; 

    struct mutex config_lock; 

    bool initialized; 
}; 

extern const struct relm_vm_operations arm64_vm_ops;
#define RELM_ARCH_VM_OPS arm64_vm_ops

extern const struct relm_mem_ops arm64_mem_ops;
#define RELM_ARCH_MEM_OPS arm64_mem_ops

#endif /* RELM_ARCH_ARM64_VM_ARCH_H */
