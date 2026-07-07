#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
 
#include <include/relm/vm.h>
#include <include/virtio/virtio.h>
#include <include/virtio/mmio.h>
#include <utils/utils.h>

static struct relm_virtio_device *g_registered_devices[RELM_VIRTIO_MMIO_MAX_DEVICES]; 
static unsigned int g_registered_count = 0;

int relm_virtio_mmio_register_device(struct relm_virtio_device *dev, 
                                     uint64_t base_gpa, 
                                     unsigned irq)
{
    int ret; 

    if (!dev || !dev->ops)
        return -EINVAL;
 
    if (g_registered_count >= RELM_VIRTIO_MMIO_MAX_DEVICES) 
    {
        pr_err("RELM: virtio-mmio: device registry full (max %d)\n",
               RELM_VIRTIO_MMIO_MAX_DEVICES);
        return -ENOSPC;
    }

    ret = relm_vm_reserve_mmio_region(dev->vm, base_gpa, 
                                      RELM_VIRTIO_MMIO_REGION_SIZE, 
                                      dev->name); 
    if (ret) {
        pr_err("RELM: virtio-mmio: failed to reserve MMIO range for "
               "device '%s': %d\n", dev->name, ret);
        return ret;
    }

    dev->mmio_base_gpa = base_gpa; 
    dev->irq = irq; 

    g_registered_devices[g_registered_count++] = dev; 
    
    pr_info("RELM: virtio-mmio: registered device '%s' (type=%u) at "
            "GPA 0x%llx-0x%llx, IRQ %u\n",
            dev->name, dev->device_id,
            base_gpa, base_gpa + RELM_VIRTIO_MMIO_REGION_SIZE - 1, irq);
 
    return 0;
}

void relm_virtio_mmio_unregister_device(struct relm_virtio_device *dev)
{
    unsigned int i;
 
    if (!dev)
        return;
 
    for (i = 0; i < g_registered_count; i++) {
        if (g_registered_devices[i] == dev)
        {
            unsigned int j;
            for (j = i; j + 1 < g_registered_count; j++) {
                g_registered_devices[j] = g_registered_devices[j + 1];
            }
            g_registered_count--;
            relm_vm_release_mmio_region(dev->vm, dev->mmio_base_gpa);
 
            pr_info("RELM: virtio-mmio: unregistered device '%s'\n",
                    dev->name);
            return;
        }
    }
 
    pr_warn("RELM: virtio-mmio: unregister called for device '%s' "
            "which was never registered\n", dev->name);
}

/*
 * find_device_for_gpa — locate which registered device, if any, owns
 * the GPA range containing fault_gpa. Private to this file — Section 4
 * is the only caller, reached after Section 1's cheaper generic gate
 * has already confirmed fault_gpa is SOME reserved range.
 */
static struct relm_virtio_device *find_device_for_gpa(uint64_t fault_gpa)
{
    unsigned int i;
 
    for (i = 0; i < g_registered_count; i++) 
    {
        struct relm_virtio_device *dev = g_registered_devices[i];
        uint64_t end = dev->mmio_base_gpa + RELM_VIRTIO_MMIO_REGION_SIZE;
 
        if (fault_gpa >= dev->mmio_base_gpa && fault_gpa < end) {
            return dev;
        }
    }
    return NULL;
}


