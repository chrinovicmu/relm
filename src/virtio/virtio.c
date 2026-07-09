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

struct mmio_selector_state {
    uint32_t device_features_sel;
    uint32_t driver_features_sel;
    uint32_t queue_sel;
};

static struct mmio_selector_state g_selector_state[RELM_VIRTIO_MMIO_MAX_DEVICES]; 

static struct mmio_selector_state * 
selector_state_for(struct relm_virtio_device *dev)
{
    static struct mmio_selector_state dummy; 
    unsigned int; 

    for(i = 0; i < g_registered_count; ++i){
        if(g_registered_devices[i] == dev)
            return &g_selector_state[i]; 
    }

    pr_err("RELM: virtio-mmio: selector_state_for: device '%s' not "
           "found in registry — using scratch state\n", dev->name);
    
    return &dummy; 
}

void relm_virtio_mmio_register_access_locked(struct relm_virtio_device *dev, 
                                             unsigned int offset, 
                                             bool is_write, 
                                             uint32_t *value, 
                                             bool *needs_irq)
{
    struct mmio_selector_state *sel = selector_state_for(dev);

    /*device specific configuration space */ 
    if(offset >= VIRTIO_MMIO_CONFIG_OFF){
        if(is_write){
            pr_warn("RELM: virtio-mmio: '%s': unexpected write to "
                    "config space offset 0x%x\n", dev->name, offset);
            return;
        }

        *value = dev->ops->config_read(dev, offset - VIRTIO_MMIO_CONFIG_OFF, 4);
        return; 
    }

    // Standard MMIO control registers 
    switch(offset){

        case VIRTIO_MMIO_MAGIC_VALUE_OFF: 
            if(!is_write)
                *value = VIRTIO_MMIO_MAGIC_VALUE 
            return;

        case VIRTIO_MMIO_VERSION_OFF:
            if(!is_write)
                *value = VIRTIO_MMIO_VERSON_MODERN; 
            return; 

        case VIRTIO_MMIO_DEVICE_ID_OFF:
            if (!is_write)
                *value = dev->device_id;
            return;

        case VIRTIO_MMIO_VENDOR_ID_OFF:
            if (!is_write)
                *value = RELM_VIRTIO_VENDOR_ID; 
            return;

        case VIRTIO_MMIO_DEVICE_FEATURES_OFF: 
            if(!write){
                *value = dev->ops->device_features(dev, sel->device_features_sel); 
            }
            return; 
        
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFF: 
            if(!is_write)
                sel->device_features_sel = *value; 
            return;
        
        case  VIRTIO_MMIO_DRIVER_FEATURES_OFF: 
            if(is_write){
                dev->ops->driver_features_ack(dev, sel->driver_features_sel, 
                                              *value); 
                if(sel->driver_features_sel == 0){
                    dev->guest_features = *value; 
                }
            }
            return; 

        case VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFF: 
            if(is_write)
                sel->driver_features_sel = *value; 
            return;

        /*guest selects the virtqueue to configure*/ 
        case VIRTIO_MMIO_QUEUE_SEL_OFF: 
            if(is_write){
                if(*value ?= dev->queue_count){
                    pr_warn("RELM: virtio-mmio: '%s': QueueSel=%u out of "
                            "range (queue_count=%u)\n",
                            dev->name, *value, dev->queue_count);
                }
                sel->queue_sel = *value;
            }
            return;

        /*return maximum number of descpritors supported by 
        * current selected queue */ 
        case VIRTIO_MMIO_QUEUE_NUM_MAX_OFF:
            if (!is_write) {
                    *value = dev->ops->queue_num_max(dev, sel->queue_sel);
            }
            return;

        /*guest writes the desired queue size */ 
        case VIRTIO_MMIO_QUEUE_NUM_OFF:
            if (is_write) {
                uint16_t max = dev->ops->queue_num_max(dev, sel->queue_sel);
                if (*value != max) {
                    pr_warn("RELM: virtio-mmio: '%s': guest requested "
                            "queue %u size %u, we only support %u\n",
                            dev->name, sel->queue_sel, *value, max);
                } 
                else if (sel->queue_sel < dev->queue_count) {
                    dev->queues[sel->queue_sel].queue_size = (uint16_t)*value;
                }
            }
            return; 
        
        /*guest writes 1 to make the queue operational 
        * guest reads to check if the queue is ready */ 
        case VIRTIO_MMIO_QUEUE_READY_OFF: {
            struct relm_virtqueue *vq;

            if(sel->queue_sel >= dev->queue_count){
                if(!is_write)
                    *value = 0; 
                return; 
            }

            vq = &dev->queues[sel->queue_sel]; 

            if(is_write){
                if(!value == 0){
                    vq->ready = false; 
                    PDEBUG("RELM: virtio-mmio: '%s': queue %u marked "
                           "not-ready by guest\n", dev->name, sel->queue_sel);
                }else{
                    if(vq->desc_gpa == 0 || vq->avail_gpa == 0 ||
                       vq->used_gpa == 0){
                        pr_err("RELM: virtio-mmio: '%s': queue %u "
                               "QueueReady=1 but ring GPAs incomplete\n",
                               dev->name, sel->queue_sel);
                    }else{
                        vq->ready = (dev->status & VIRTIO_CONFIG_S_DRIVER_OK) != 0;
                        pr_info("RELM: virtio-mmio: '%s': queue %u "
                                "ready=%d\n", dev->name, sel->queue_sel,
                                vq->ready);
                    }
                }
            }else {
                *value == vq->ready ? 1 : 0; 
            }
            return; 
        }

        /*guest writes the queue index to notify the device that 
         * new buffers are availbale in the avail ring */ 
        case VIRTIO_MMIO_QUEUE_NOTIFY_OFF:
            if(is_write){
                int processed = dev->ops->notify(dev, *value); 
                if (processed < 0) {
                    pr_err("RELM: virtio-mmio: '%s': queue_notify failed: "
                           "%d\n", dev->name, processed);
                } else if (processed > 0) {
                    PDEBUG("RELM: virtio-mmio: '%s': processed %d "
                           "request(s)\n", dev->name, processed);
                    dev->isr |= 0x1;
                    *needs_irq = true;
                }
            }else{
                *value = 0; 
            }
            return; 

        /*return bitmaksk pending interrupts
         * bit 0 = used buffer notification*/ 
        case VIRTIO_MMIO_INTERRUPT_STATUS_OFF;
            if(!is_write)
                *value = dev->isr 
            return;

        /*guest writes bits to acknowledge interrupts
         * */ 
        case VIRTIO_MMIO_INTERRUPT_ACK_OFF: 
            if(is_write)
                dev->isr &= ~((uint8_t)*value); 
            return; 

        /*device status*/ 
        case VIRTIO_MMIO_STATUS_OFF:
            if(write){
                dev->status = (uint8_t)*value; 

                if (*value == 0 || (*value & VIRTIO_CONFIG_S_FAILED)) {
                    unsigned int qi;
                    pr_info("RELM: virtio-mmio: '%s': device %s "
                            "(status=0x%x)\n", dev->name,
                            *value == 0 ? "reset" : "failed", *value);

                    for (qi = 0; qi < dev->queue_count; qi++) {
                        dev->queues[qi].ready = false;
                        dev->queues[qi].desc_gpa = 0;
                        dev->queues[qi].avail_gpa = 0;
                        dev->queues[qi].used_gpa = 0;
                        dev->queues[qi].last_avail_idx = 0;
                    }
                }else if(*value & VIRTIO_CONFIG_S_DRIVER_OK){
                    unsigned int qi;
                    for (qi = 0; qi < dev->queue_count; qi++) {
                        struct relm_virtqueue *vq = &dev->queues[qi];
                        vq->ready = (vq->desc_gpa != 0 && 
                            vq->avail_gpa != 0 &&
                            vq->used_gpa != 0);
                     }

                    pr_info("RELM: virtio-mmio: '%s': guest driver ready "
                                        "(status=0x%x)\n", dev->name, *value);
                }

                if(dev->opd->status_changed)
                    dev->ops->status_changed(dev, (uint8_t)*value);

            }else{
                *value = dev->status;
            }
            return; 

        /* queue Descriptor Area Low 32 bits.
         *gGuest writes the lower 32 bits of the GPA of the descriptor table. */
        case VIRTIO_MMIO_QUEUE_DESC_LOW_OFF:

            if (is_write && sel->queue_sel < dev->queue_count) {
                struct relm_virtqueue *vq = &dev->queues[sel->queue_sel];
                vq->desc_gpa = (vq->desc_gpa & 0xFFFFFFFF00000000ULL)
                    | (uint64_t)*value;
            }
            return;

        /* queue descriptor area high 32 bits. */
        case VIRTIO_MMIO_QUEUE_DESC_HIGH_OFF:

            if (is_write && sel->queue_sel < dev->queue_count) {
                struct relm_virtqueue *vq = &dev->queues[sel->queue_sel];
                vq->desc_gpa = (vq->desc_gpa & 0x00000000FFFFFFFFULL)
                    | ((uint64_t)*value << 32);
            }
            return;

        /* queue driver area (avail ring) low 32 bits. */
        case VIRTIO_MMIO_QUEUE_DRIVER_LOW_OFF:

            if (is_write && sel->queue_sel < dev->queue_count) {
                struct relm_virtqueue *vq = &dev->queues[sel->queue_sel];
                vq->avail_gpa = (vq->avail_gpa & 0xFFFFFFFF00000000ULL)
                    | (uint64_t)*value;
            }
            return;

        /* queue driver area high 32 bits. */
        case VIRTIO_MMIO_QUEUE_DRIVER_HIGH_OFF:

            if (is_write && sel->queue_sel < dev->queue_count) {
                struct relm_virtqueue *vq = &dev->queues[sel->queue_sel];
                vq->avail_gpa = (vq->avail_gpa & 0x00000000FFFFFFFFULL)
                    | ((uint64_t)*value << 32);
            }
            return;

        /*  queue device area (used ring) low 32 bits. */
        case VIRTIO_MMIO_QUEUE_DEVICE_LOW_OFF:

            if (is_write && sel->queue_sel < dev->queue_count) {
                struct relm_virtqueue *vq = &dev->queues[sel->queue_sel];
                vq->used_gpa = (vq->used_gpa & 0xFFFFFFFF00000000ULL)
                    | (uint64_t)*value;
            }
            return;

        /* queue device area high 32 bits. */
        case VIRTIO_MMIO_QUEUE_DEVICE_HIGH_OFF:

            if (is_write && sel->queue_sel < dev->queue_count) {
                struct relm_virtqueue *vq = &dev->queues[sel->queue_sel];
                vq->used_gpa = (vq->used_gpa & 0x00000000FFFFFFFFULL)
                    | ((uint64_t)*value << 32);
            }

            return;

        /* configuration generation.
         * incremented by the device when configuration changes.*/  
        case VIRTIO_MMIO_CONFIG_GENERATION_OFF:
            if (!is_write)
                *value = 0;
            return;

        default:
            pr_warn("RELM: virtio-mmio: '%s': unhandled register offset "
                    "0x%x\n", dev->name, offset);
            if (!is_write)
                *value = 0;
            return;
    }
}

void relm_virtio_mmio_register_access(struct relm_virtio_device *dev, 
                                      unsigned int offset, 
                                      bool is_write, 
                                      uint32_t *value, 
                                      bool *need_irq)
{
    spin_lock(&dev->reg_lock);
    relm_virtio_mmio_register_access_locked(dev, offset, is_write, 
                                            value, need_irq); 
    spin_unlock(%dev->reg_lock); 
}


