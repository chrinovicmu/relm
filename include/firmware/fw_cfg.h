/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_FW_CFG_H
#define RELM_FW_CFG_H
 
#include <linux/types.h>
#include <linux/spinlock.h>
#include <include/firmware/e820.h>
 
struct relm_vm;

#define FW_CFG_SIGNATURE        0x0000
#define FW_CFG_ID               0x0001 
#define FW_CFG_UUID             0x0002 
#define FW_CFG_RAM_SIZE         0x0003 
#define FW_CFG_NOGRAPHIC        0x0004
#define FW_CFG_NB_CPUS          0x0005 
#define FW_CFG_MACHINE_ID       0x0006
#define FW_CFG_KERNEL_ADDR      0x0007 
#define FW_CFG_KERNEL_SIZE      0x0008 
#define FW_CFG_KERNEL_CMDLINE   0x0009 
#define FW_CFG_INITRD_ADDR      0x000A
#define FW_CFG_INITRD_SIZE      0x000B 
#define FW_CFG_BOOT_DEVICE      0x000C 
#define FW_CFG_NUMA             0x000D
#define FW_CFG_BOOT_MENU        0x000E 
#define FW_CFG_MAX_CPUS         0x000F 
#define FW_CFG_FILE_DIR         0x0019 
#define FW_CFG_FILE_FIRST       0x8000

struct fw_cfg_file 
{
    uint32_t size; 
    uint16_t select;
    uint16_t reserved;
    char name[56]; 
}__attribute__((packed)); 


/* Maximum number of fw_cfg items RELM will register.
 * We need: SIGNATURE, ID, UUID, RAM_SIZE, NOGRAPHIC, NB_CPUS, MACHINE_ID,
 *          KERNEL_ADDR, KERNEL_SIZE, KERNEL_CMDLINE, INITRD_ADDR,
 *          INITRD_SIZE, BOOT_DEVICE, NUMA, BOOT_MENU, MAX_CPUS,
 *          FILE_DIR (built dynamically) + named files.
 * 32 is generous for a minimal implementation. */
#define FW_CFG_MAX_ITEMS        32
#define FW_CFG_MAX_NAME_LEN     55
#define FW_CFG_MAX_ITEM_SIZE    (4 * 1024)

struct fw_cfg_item
{
    uint16_t key; 
    char name[FW_CFG_MAX_NAME_LEN]; 
    uint8_t *data; 
    uint32_t size; 
    uint32_t cursor; 
    bool in_use; 
};

struct fw_cfg_device
{
    struct fw_cfg_item items[FW_CFG_MAX_ITEMS]; 
    uint32_t item_count; 
    struct fw_cfg_item *selected; 
    uint16_t next_file_key; 
    struct relm_e820_map e820_map; 
    spinlock_t lock; 
    bool initialized; 
};

struct fw_cfg_file_dir {
    __be32 count;
    struct fw_cfg_file files[];
} __attribute__((packed));


void relm_fw_cfg_init(struct fw_cfg_device *fw); 
void relm_fw_cfg_destroy(struct fw_cfg_device *fw);

/* relm_fw_cfg_register - register one item in the fw_cfg device.
 *
 * @fw:   the fw_cfg device
 * @key:  16-bit selector key (use a FW_CFG_* constant or FW_CFG_FILE_FIRST+n)
 * @name: optional file name for named items (NULL or "" for well-known items)
 * @data: item payload bytes (copied into an internal buffer with kmalloc)
 * @size: number of bytes in @data
 *
 * Returns 0 on success, negative errno on failure.
 */
int relm_fw_cfg_register(struct fw_cfg_device *fw,
                          uint16_t key,
                          const char *name,
                          const void *data,
                          uint32_t size);


/* relm_fw_cfg_register_file - register a named file with an auto-assigned key.
 * Assigns the next available key >= FW_CFG_FILE_FIRST, registers the item,
 * and returns the assigned key via @key_out.
 *
 * Named files are listed in FW_CFG_FILE_DIR so SeaBIOS can discover them
 * by filename without knowing the key in advance.
 */
int relm_fw_cfg_register_file(struct fw_cfg_device *fw,
                               const char           *name,
                               const void           *data,
                               uint32_t              size,
                               uint16_t             *key_out);

/*high level function thta populates the fw_cfg device with 
    * everything SeaBIOS needs */ 
int relm_fw_cfg_setup(struct fw_cfg_device *fw, struct relm_vm *vm); 

/*handle wrtie to port 0x510 */ 
void relm_fw_cfg_select(struct fw_cfg_device *fw, uint16_t key); 

/*handle read to port 0x511 */ 
uint8_t relm_fw_cfg_read(struct fw_cfg_device *fw); 

int relm_fw_cfg_handle_io(struct fw_cfg_device *fw,
                           uint16_t port, bool is_write,
                           uint32_t size, uint32_t *value);

/* fw_cfg I/O port addresses */
#define FW_CFG_PORT_SEL     0x510   
#define FW_CFG_PORT_DATA    0x511   
#define FW_CFG_PORT_DMA_HI  0x514   /* DMA high 32 bits (not implemented) */
#define FW_CFG_PORT_DMA_LO  0x518   /* DMA low  32 bits (not implemented) */
 
#endif 
