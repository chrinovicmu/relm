/* SPDX-License-Identifier: GPL-2.0
 *
 * iommu.c — IOMMU integration for RELM hypervisor
 *
 * Maintains an IOMMU domain per VM whose mappings mirror the EPT exactly.
 * Every guest RAM GPA is mapped to the same HPA in both the EPT and the IOMMU
 * domain, ensuring that device DMA (using GPAs as I/O Virtual Addresses) is
 * correctly translated to the guest's actual host physical pages.
 *
 * See iommu.h for the full architectural background and threat model.
 */
 
#include <linux/iommu.h>       
#include <linux/pci.h>         
#include <linux/printk.h>   
#include <linux/list.h>     
#include <linux/slab.h>                                 
#include <linux/mm.h>       
 
#include <include/vm.h>     
#include <include/ept.h>       
#include <include/iommu.h>
#include <utils/utils.h>

static int relm_iommu_fault_handler(struct iommu_domain *domain, 
                                    struct device *dev, 
                                    unsigned long iova, 
                                    int flags, 
                                    void *token)
{
    struct relm_vm *vm = (struct relm_vm*)token; 
    vm->iommu.fault_count++; 

    pr_err("RELM: IOMMU FAULT on VM '%s': device=%s IOVA=0x%lx %s\n"
           "      fault_count=%llu — DMA transaction aborted by IOMMU\n",
           vm->vm_name,
           dev_name(dev),
           iova,
           (flags & IOMMU_FAULT_WRITE) ? "WRITE" : "READ",
           vm->iommu.fault_count);

    return -ENOSYS;
}

int relm_iommu_init(struct relm_vm *vm, struct device *dev)
{
    struct relm_iommu_context *iommu = &vm->iommu; 

    if(!iommu_present(dev->bus))
    {
        pr_err("RELM: IOMMU: no IOMMU hardware present on bus '%s'\n"
               "  Check BIOS settings: enable Intel VT-d or AMD-Vi\n"
               "  Check kernel cmdline: remove 'intel_iommu=off' or 'iommu=off'\n"
               "  Check kernel config: CONFIG_INTEL_IOMMU=y or CONFIG_AMD_IOMMU=y\n",
               dev->bus->name);
        return -ENODEV;

    }
    if(!device_iommu_capable(dev, IOMMU_CAP_INTR_REMAP))
    {
        pr_warn("RELM: IOMMU: interrupt remapping NOT supported\n"
                "  Devices can inject arbitrary MSI interrupts — security risk!\n"
                "  Enable Intel VT-d interrupt remapping for production use.\n");
        /* Continue anyway in development mode */
    }

    iommu->domain = iommu_domain_alloc(dev->bus); 
    if(!iommu->domain)
    {
        pr_err("RELM: IOMMU: iommu_domain_alloc() failed for VM '%s'\n"
               "  The IOMMU driver could not allocate a domain.\n",
               vm->vm_name);
        return -ENOMEM;
    }

    iommu_set_fault_handler(iommu->domain,
                             relm_iommu_fault_handler,
                             (void *)vm);

    INIT_LIST_HEAD(&iommu->devices);
    iommu->n_devs           = 0;
    iommu->enabled          = true;
    iommu->n_mapped_pages   = 0;
    iommu->fault_count      = 0;
 
    pr_info("RELM: IOMMU: domain created for VM '%s' (IOMMU_DOMAIN_UNMANAGED)\n",
            vm->vm_name);
 
    return 0;
    
}

void relm_iommu_destroy(struct relm_vm *vm)
{
    struct relm_iommu_context *iommu = &vm->iommu; 
    struct relm_pass_device *entry, *tmp; 

    if(!iommu->enabled || iommu->domain)
        return; 

    list_for_each_entry(entry, &iommu->devices, link){
    
        pr_info("RELM: IOMMU: detaching device %s from VM '%s'\n",
                pci_name(entry->pdev), vm->vm_name);

        iommu_detach_device(iommu->domain, &entry->pdev->dev); 
        pci_dev_put(entry->pdev);
        list_del(&entry->link);
        kfree(entry);
    }

    iommu->n_devs = 0;
    iommu_domain_free(iommu->domain);
    iommu->domain  = NULL;
    iommu->enabled = false;
 
    pr_info("RELM: IOMMU: domain destroyed for VM '%s' "
            "(%llu pages were mapped, %llu faults recorded)\n",
            vm->vm_name, iommu->n_mapped_pages, iommu->fault_count);
}

int relm_iommu_map(struct relm_vm *vm, 
                   uint64_t gpa, uint64_t hpa, 
                   size_t size, bool write)
{
    struct relm_iommu_context *iommu = &vm->iommu; 
    int prot; 
    int ret; 

    if(!iommu->enabled || !iommu->domain)
        return 0; 

    /*all guest RAM mappings allow device reads. write permission 
        * depends on whether this a region the guest driver
    * intends the device write to */ 
    prot = IOMMU_READ; 
    if(write)
        prot |= IOMMU_WRITE; 
    prot |= IOMMU_CACHE; 
    
    ret = iommu_map(iommu->domain, (unsigned long)gpa, (phys_addr_t)hpa,
                    size, prot);
    if(ret)
    {
        pr_err("RELM: IOMMU: iommu_map failed: GPA=0x%llx HPA=0x%llx "
               "size=%zu prot=0x%x ret=%d\n",
               gpa, hpa, size, prot, ret);
        return ret;
    }
 
    /* Track the number of mapped pages for diagnostics.
     * size / PAGE_SIZE = number of 4 KB pages this call mapped. */
    iommu->n_mapped_pages += size / PAGE_SIZE;
 
    PDEBUG("RELM: IOMMU: mapped IOVA=0x%llx → HPA=0x%llx size=%zu %s\n",
           gpa, hpa, size, write ? "RW" : "RO");
 
    return 0;

}

void relm_iommu_unmap(struct relm_vm *vm, uint64_t gpa, size_t size)
{
    struct relm_iommu_context *iommu = &vm->iommu;
    size_t unmapped;
 
    if(!iommu->enabled || !iommu->domain)
        return;
 
    unmapped = iommu_unmap(iommu->domain, (unsigned long)gpa, size);

    if(unmapped != size)
    {
        pr_warn("RELM: IOMMU: partial unmap at GPA=0x%llx: "
                "requested=%zu unmapped=%zu\n",
                gpa, size, unmapped);
    }
 
    uint64_t pages = unmapped / PAGE_SIZE;
    if(pages <= iommu->n_mapped_pages)
        iommu->n_mapped_pages -= pages;
    else
        iommu->n_mapped_pages = 0;
 
    PDEBUG("RELM: IOMMU: unmapped IOVA=0x%llx size=%zu\n", gpa, size);
}
 
int relm_iommu_map_guest_ram(struct relm_vm *vm)
{
    struct relm_iommu_context *iommu = &vm->iommu;
    uint64_t gpa;
    uint64_t hpa;
    uint64_t n_mapped  = 0;
    uint64_t n_skipped = 0;
    bool is_write = true; 
    int      ret;
 
    if(!iommu->enabled || !iommu->domain)
    {
        pr_warn("RELM: IOMMU: map_guest_ram called on uninitialised domain\n");
        return -EINVAL;
    }
 
    pr_info("RELM: IOMMU: mapping %llu MB of guest RAM "
            "(GPA 0x0 – 0x%llx)...\n",
            vm->total_guest_ram / (1024 * 1024),
            vm->total_guest_ram);
 
    for(gpa = 0; gpa < vm->total_guest_ram; gpa += PAGE_SIZE)
    {
        ret = relm_ept_get_mapping(vm->ept, gpa, &hpa);
        if(ret)
        {
            n_skipped++;
            continue;
        }
 
        hpa &= PAGE_MASK;
 
        ret = relm_iommu_map(vm, gpa, hpa, PAGE_SIZE, is_write);
        if(ret)
        {
            pr_err("RELM: IOMMU: map_guest_ram failed at GPA=0x%llx "
                   "HPA=0x%llx: %d\n", gpa, hpa, ret);
            return ret;
        }
 
        n_mapped++;
    }
 
    pr_info("RELM: IOMMU: guest RAM mapped: %llu pages mapped, "
            "%llu GPA slots skipped (MMIO holes/reserved)\n",
            n_mapped, n_skipped);
 
    return 0;
}

int relm_iommu_attach_device(struct relm_vm *vm, struct pci_dev *pdev)
{
    struct relm_iommu_context *iommu = &vm->iommu; 
    struct relm_pass_device *entry; 
    int ret; 

    if(!iommu->enabled || !iommu->domain)
    {
        pr_err("RELM: IOMMU: attach_device called on uninitialised domain\n");
        return -EINVAL;
    }

    entry = kmalloc(sizeof(*entry), GFP_KERNEL); 
    if(!entry)
        return -ENOMEM; 

    entry->pdev = pci_dev_get(pdev); 
    INIT_LIST_HEAD(&entry->link); 


    ret = iommu_attach_device(iommu->domain, &pdev->dev);
    if(ret)
    {
        pr_err("RELM: IOMMU: iommu_attach_device failed for %s: %d\n"
               "  Is the device already attached to another domain?\n"
               "  Is the device unbound from its host driver?\n",
               pci_name(pdev), ret);
        pci_dev_put(pdev);
        kfree(entry);
        return ret;
    }

    list_add(&entry->link, &iommu->devices);
    iommu->n_devs++;
 
    pr_info("RELM: IOMMU: device %s (%04x:%04x) attached to VM '%s'\n"
            "All DMA from this device now translated through VM's IOMMU domain\n",
            pci_name(pdev),
            pdev->vendor, pdev->device,
            vm->vm_name);
 
    return 0;
}

void relm_iommu_detach_device(struct relm_vm *vm, struct pci_dev *pdev)
{
    struct relm_iommu_context      *iommu = &vm->iommu;
    struct relm_pass_device *entry, *tmp;
 
    if(!iommu->enabled || !iommu->domain)
        return;
 
    /* Find and remove the entry from the tracking list */
    list_for_each_entry_safe(entry, tmp, &iommu->devices, link)
    {
        if(entry->pdev == pdev)
        {
            iommu_detach_device(iommu->domain, &pdev->dev);
 
            pr_info("RELM: IOMMU: device %s detached from VM '%s'\n",
                    pci_name(pdev), vm->vm_name);
 
            pci_dev_put(entry->pdev);
            list_del(&entry->link);
            kfree(entry);
            iommu->n_devs--;
            return;
        }
    }
 
    pr_warn("RELM: IOMMU: detach requested for %s but device not found "
            "in VM '%s' device list\n", pci_name(pdev), vm->vm_name);
}

void relm_iommu_dump_stats(const struct relm_vm *vm)
{
    const struct relm_iommu_context      *iommu = &vm->iommu;
    const struct relm_pass_device *entry;
 
    pr_info("RELM: IOMMU stats for VM '%s':\n", vm->vm_name);
    pr_info("  enabled:        %s\n",       iommu->enabled ? "yes" : "no");
    pr_info("  domain:         %p\n",        iommu->domain);
    pr_info("  n_mapped_pages: %llu (%llu MB)\n",
            iommu->n_mapped_pages,
            iommu->n_mapped_pages * PAGE_SIZE / (1024 * 1024));
    pr_info("  fault_count:    %llu\n",      iommu->fault_count);
    pr_info("  n_devs:      %u\n",        iommu->n_devs);
 
    if(!list_empty(&iommu->devices))
    {
        pr_info("  attached devices:\n");
        list_for_each_entry(entry, &iommu->devices, link)
        {
            pr_info("    %s (%04x:%04x %s)\n",
                    pci_name(entry->pdev),
                    entry->pdev->vendor,
                    entry->pdev->device,
                    entry->pdev->driver ?
                        entry->pdev->driver->name : "(no driver)");
        }
    }
}
 

