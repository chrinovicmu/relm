#ifndef RELM_VIRTIO_H
#define RELM_VIRTIO_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <include/virtio/mmio.h> 

#define VIRTIO_MMIO_VALUE           0x74726976U
#define VIRTIO_MMIO_VERSION_MODERN  2U /*modern */ 
#define VIRTIO_ID_NET               1U
#define VIRTIO_ID_BLOCK             2U
#define VIRTIO_ID_CONSOLE           3U
#define VIRTIO_ID_RNG               4U
#define VIRTIO_ID_BALLOON           5U
#define VIRTIO_ID_RPMSG             7U
#define VIRTIO_ID_SCSI              8U
#define VIRTIO_ID_9P                9U
#define VIRTIO_ID_RPROC_SERIAL      11U
#define VIRTIO_ID_CAIF              12U
#define VIRTIO_ID_GPU               16U
#define VIRTIO_ID_INPUT             18U
#define VIRTIO_ID_VSOCK             19U
#define VIRTIO_ID_CRYPTO            20U
#define VIRTIO_ID_IOMMU             23U
#define VIRTIO_ID_MEM               24U
#define VIRTIO_ID_FS                26U
#define VIRTIO_ID_PMEM              27U
#define VIRTIO_ID_MAC80211_HWSIM    29U 
#define RELM_VIRTIO_VENDOR_ID       0x52454C4DU  /* "RELM" in ASCII */ 


/* Descriptor flags (struct vring_desc.flags). */
#define VRING_DESC_F_NEXT               1  /* descriptor continues via 'next'       */
#define VRING_DESC_F_WRITE              2  /* device-writable (host writes INTO it) */
#define VRING_DESC_F_INDIRECT           4  /* points to a list of further descriptors : Not currently implemented*/ 

#define VIRTIO_CONFIG_S_ACKNOWLEDGE     1
#define VIRTIO_CONFIG_S_DRIVER          2
#define VIRTIO_CONFIG_S_DRIVER_OK       4
#define VIRTIO_CONFIG_S_FAILED          0x80

/* relm currently supports at most this many virtqueues per device.
 * virtio-blk uses 1 (the request queue). virtio-net commonly uses 2
 * (rx, tx) or more with multiqueue — 8 gives headroom for a future
 * multiqueue net implementation without needing to revisit this
 * constant. Devices with fewer queues simply leave the higher indices
 * unconfigured (ready == false). */
#define RELM_VIRTIO_MAX_QUEUES 8

#define RELM_VIRTIO_MMIO_MAX_DEVICES 4 

struct relm_vm; 
struct relm_virtio_device; 
struct relm_virtio_device_ops;


struct vring_desc{
    __le64 addr; 
    __le32 len; 
    __le16 flags; 
    __le16 next; 
}__atrribute__((packed)); 

/*skip interrupts for next completion */ 
#define VRING_AVAIL_F_NO_INTERRUPT 1

/*available ring header */ 
struct vring_avail {
    __le16 flags; 
    __le16 idx; 
}; 

/*guest should not notify host when new buffer are added to the ring*/ 
#define VRING_USED_F_NO_NOTIFY      1 

struct vring_used_entry{
    __le32 id; 
    __le32 len; 
} __atrribute__((packed)); 

struct vring_used{
    __le16 flags; 
    __le16 idx; 
}; 


struct relm_virtqueue
{
    uint64_t queue_size; 
    uint64_t desc_gpa; 
    uint64_t avail_gpa; 
    uint64_t used_gpa;

    /*host read position */ 
    uint16 last_avial_idx; 

    /*true once all GPAs are non-zero, QeueReady is set, 
     * and Device status has reached DRIVER_OK */ 
    bool ready; 
    spinlock_t lock; 

}; 

struct relm_virtio_device
{
    uint32_t device_id; 

    /*base GPA of device's MMIO region*/ 
    uint64_t mmio_base_gpa; 
    unsigned int irq;
    struct relm_vm *vm; 
    uint8_t status; 
    uint32 guest_features; 
    uint8_t isr; 
    struct relm_virtqueue queues[RELM_VIRTIO_MAX_QUEUES]; 
    unsigned int queue_count; 
    const struct relm_virtio_device_ops *op; 
    char name[32]; 
    spinlock_t reg_lock; 
}; 

struct relm_virtio_device_ops{

    /*return DeviceFeatures bitmap
     * sel : 0 = low bits, 1 = high bits*/  
    uint32_t (*device_features)(struct relm_virtio_device *dev, 
                                uint32_t sel); 

    void (*drivers_features_ack)(struct relm_virtio_device *dev, 
                                 uint32_t sel, uint32_t value); 

    uint16 (*queue_num_max)(struct relm_virtio_device *dev, 
                            unsigned int queue_index);

    uint32_t (*config_read)(struct relm_virtio_device *dev, 
                            unsigned int offset, int size); 

    /*called when the guest wrties to QeueNotify with given queue_index*/ 
    int (*queue_notify)(struct relm_virtio_device *dev, 
                        unsigned int queue_index);

    void (*status_changed)(struct relm_virtio_device *dev, 
                           uint8_t new_status); 
}; 


typedef int (*relm_virtio_chain_handler_fn)(struct relm_virtio_device *dev, 
                                            unsigned int queue_index, 
                                            uint16_t head_idx, 
                                            uint32_t *bytes_written_out); 

int relm_virtio_process_queue(struct relm_virtio_device *dev, 
                              unsigned int queue_index, 
                              relm_virtio_chain_handler_fn handler); 

static inline uint64_t vring_avail_ring_gpa(uint64_t avail_base_gpa)
{
    return avail_base_gpa + sizeof(struct vring_avail); 
}

static inline uint64_t vring_used_ring_gpa(uint64_t used_base_gpa)
{
    return used_base_gpa + sizeof(struct vring_used); 
}

static inline uint64_t vring_avail_total_size(uint16 queue_size)
{
/* flags(2) + idx(2) + ring[queue_size]*2 + used_event(2) */
    return sizeof(struct vring_avail)
        + (uint64_t)queue_size + sizeof(__le16) + sizeof(__le16); 
}

static inline uint64_t vring_used_total_size(uint16_t queue_size)
{
    /* flags(2) + idx(2) + ring[queue_size]*8 + avail_event(2) */
    return sizeof(struct vring_used)
         + (uint64_t)queue_size * sizeof(struct vring_used_elem)
         + sizeof(__le16);
}
 
static inline uint64_t vring_desc_table_total_size(uint16_t queue_size)
{
    return (uint64_t)queue_size * sizeof(struct vring_desc);
}



#endif 
