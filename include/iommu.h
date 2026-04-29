/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_IOMMU_H
#define RELM_IOMMU_H
 
#include <linux/types.h>
#include <linux/list.h>
#include <linux/iommu.h>   
#include <linux/pci.h>  
#include <stddef.h>
#include <stdint.h>
 
struct relm_vm;
struct ept_context;



/*tracks each pass-trhough device */ 
struct relm_pass_device
{
    struct pci_dev *dev; 
    struct list_head link; 

}; 

struct relm_iommu_context
{
    struct iommu_domain *domain; 
    struct list_head devices; 
    uint32_t n_devs; 
    bool enabled; 
    uint64_t n_mapped_pages; 
    uint64_t fault_count; 

}; 

int relm_iommu_init(struct relm_vm *vm, struct device *dev);
void relm_iommu_destroy(struct relm_vm *vm); 
int relm_iommu_map(struct relm_vm *vm, uint64_t gpa, uint64_t hpa
                   size_t size, bool write); 
void relm_iommu_unmap(struct relm_vm *vm, uint64_t gpa, size_t size);
int relm_iommu_attach_device(struct relm_vm *vm, struct pci_dev *pdev);
void relm_iommu_detach_device(struct relm_vm *vm, struct pci_dev *pdev);
int relm_iommu_map_guest_ram(struct relm_vm *vm);
void relm_iommu_dump_stats(const struct relm_vm *vm);

#ifdef CONFIG_IOMMU_API
#define RELM_IOMMU_SUPPORTED 1
#else
#define RELM_IOMMU_SUPPORTED 0
#warning "RELM: CONFIG_IOMMU_API not set — IOMMU support disabled"
#endif
 
#endif /* RELM_IOMMU_H */
