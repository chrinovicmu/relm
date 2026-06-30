#include <linux/types.h>

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

/*device-specifc config space*/ 
#define VIRTIO_MMIO_CONFIG_GENERATION_OFF   0x0fc 
#define VIRTIO_MMIO_CONFIG_OFF              0x100 


/*total size of the register block + config space. 
* page aligned*/ 
#define RELM_VIRTIO_MMIO_REGION             0x1000U 

#define RELM_VIRTIO_MMIO_BASE_GPA           0xd0000000ULL

#define RELM_VIRTIO_MMIO_IRQ   5

#endif 
