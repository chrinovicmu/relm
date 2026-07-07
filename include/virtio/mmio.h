#include <linux/types.h>
#include <linux/mm.h>
 
struct vcpu;
struct relm_vm;
struct relm_virtio_device;

#define VIRTIO_MMIO_MAGIC_VALUE_OFF         0x000
#define VIRTIO_MMIO_VERSION_OFF             0x004
#define VIRTIO_MMIO_DEVICE_ID_OFF           0x008
#define VIRTIO_MMIO_VENDOR_ID_OFF           0x00c

/*feature negotiation */ 
#define VIRTIO_MMIO_DEVICE_FEATURES_OFF     0x010 
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL_OFF 0x014 
#define VIRTIO_MMIO_DRIVER_FEATURES_OFF     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL_OFF 0x024 

/*virtqueue configuration */ 
#define VIRTIO_MMIO_QUEUE_SEL_OFF           0x030 
#define VIRTIO_MMIO_QUEUE_NUM_MAX_OFF       0x034 
#define VIRTIO_MMIO_QUEUE_NUM_OFF           0x038 
#define VIRTIO_MMIO_QUEUE_READY_OFF         0x044 

/*notifications and interrupt*/ 
#define VIRTIO_MMIO_QUEUE_NOTIFY_OFF        0x050 
#define VIRTIO_MMIO_INTERRUPT_STATUS_OFF    0x060 
#define VIRTIO_MMIO_INTERRUPT_ACK_OFF       0x064 

/*device status */ 
#define VIRTIO_MMIO_STATUS_OFF              0x070 

/*virtqueue memory layout (64 bit pointers) */ 

/*physical address of descriptor area*/ 
#define VIRTIO_MMIO_QUEUE_DESC_LOW_OFF      0x080 
#define VIRTIO_MMIO_QUEUE_DESC_HIGH_OFF     0x084 

/*64 bit address of avail ring */ 
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW_OFF    0x090 
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH_OFF   0x094 

/*64 bit address of used ring */ 
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW_OFF    0x0a0 
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH_OFF   0x0a4 

#define VIRTIO_MMIO_CONFIG_GENERATION_OFF   0x0fc 

/*device-specifc config space*/ 
#define VIRTIO_MMIO_CONFIG_OFF              0x100 

/*total size of the register block + config space. 
* page aligned*/ 
#define RELM_VIRTIO_MMIO_REGION             0x1000U 

#define RELM_VIRTIO_MMIO_IRQ   5

#define RELM_MAC_MMIO_REGIONS  8 

struct relm_mmio_region{
    uint64_t gpa_start; 
    uint64_t size; 
    char name[32]; 
}; 

int relm_vm_reserve_mmio_region(struct relm_vm *vm, 
                                uint64_t gpa_start, 
                                uint64_t size, 
                                const char *name); 

void relm_vm_release_mmio_region(struct relm_vm *vm, 
                                 uint64_t gpa_start);

bool relm_vm_gpa_is_mmio_region(struct relm_vm *vm, uint64_t fault_gpa);

 
/*
 * relm_vm_mmio_region_count / relm_vm_mmio_region_at — iteration
 * interface for the e820 map builder. relm_vm_mmio_region_at copies
 * the entry OUT into the caller's struct (rather than returning an
 * internal pointer) so the caller can never hold a pointer that goes
 * stale if a region is released mid-iteration.
 */
unsigned int relm_vm_mmio_region_count(struct relm_vm *vm);
int relm_vm_mmio_region_at(struct relm_vm *vm, unsigned int index,
                          struct relm_mmio_region *out);

int relm_virtio_mmio_register_device(struct relm_virtio_device *dev,
                                     uint64_t base_gpa,
                                     unsigned int irq);

void relm_virtio_mmio_unregister_device(struct relm_virtio_device *dev);




