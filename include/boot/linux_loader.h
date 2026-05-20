#ifndef RELM_LINUX_LOADER_H
#define RELM_LINUX_LOADER_H
 
#include <linux/types.h>
 
struct relm_vm;
struct device;


/*memory map for direct boot (128 MB guest )
 *
 *  GPA 0x00020000  : kernel command line string (4 KB max)
 *  GPA 0x0003A000  : ACPI tables area (SeaBIOS fills this)
 *  GPA 0x000C0000  : linuxboot.bin option ROM (~3 KB, mapped R/X by RELM)
 *  GPA 0x000E0000  : SeaBIOS shadow (128 KB, mapped R/X by seabios.c)
 *  GPA 0x01000000  : Linux bzImage kernel (16 MB load point)
 *  GPA 0x04000000  : initrd image (64 MB load point)
 *  GPA 0xFFFE0000  : SeaBIOS ROM at reset vector (128 KB, by seabios.c)
 * */ 

/*linuxboot.bin option ROM. placed in the option ROM scan area 
 * SeaBIOS scans 0xC0000-0xDFFFF for 0x55AA at 512-byte boundaries*/ 
#define RELM_LINUXBOOT_GPA      0x000C0000ULL

#define RELM_CMDLINE_GPA        0x00020000ULL
#define RELM_CMDLINE_MAX_LEN    4096U

/*kernel bzImage load address */ 
#define RELM_KERNEL_LOAD_GPA    0x01000000ULL  /* 16 MB */
#define RELM_KERNEL_MAX_SIZE    (64ULL * 1024 * 1024)

/*initrd load address*/ 
#define RELM_INITRD_LOAD_GPA    0x04000000ULL  /* 64 MB */
#define RELM_INITRD_MAX_SIZE    (60ULL * 1024 * 1024)

#define RELM_LINUXBOOT_FW_NAME  "seabios/linuxboot.bin"
#define RELM_KERNEL_FW_NAME     "relm/vmlinuz"
#define RELM_INITRD_FW_NAME     "relm/initrd.img"

/* default kernel command line used when the user has not overridden it.
 * console=ttyS0: output to serial port (RELM has no VGA emulation)
 * earlyprintk=serial: early boot messages also to serial
 * root=/dev/vda:  virtio-blk disk (if present; ignored for initrd-only boot)
 * rw:             mount root read-write
 * nokaslr:        disable kernel ASLR for now */
#define RELM_DEFAULT_CMDLINE    "console=ttyS0,115200 earlyprintk=serial " \
                                "root=/dev/vda rw nokaslr"

#define LINUXBOOT_EPT_FLAGS     (0x5ULL) /* R=1 R=0 X=1 MT=UC*/ 


int relm_linux_loader_setup(struct relm_vm *vm, struct device *dev,
                            const char *cmdline); 

/*load linuxboot.bin into option ROM area*/ 
int relm_linuxboot_load(struct relm_vm *vm, struct device *dev); 

/*load vmlinuz into guest RAM at RELM_KERNEL_LOAD_GPA*/ 
int relm_kernel_load(struct relm_vm *vm, struct device *dev, 
                     uint32_t *size); 

/*load initrd.img into guest RAM at RELM_INITRD_LOAD_GPA*/ 
int relm_initrd_load(struct relm_vm *vm, struct device *dev, 
                     uint32_t *size); 

/*write kernel command line into guest RAM*/ 
int relm_cmdline_load(struct relm_vm *vm, const char *cmdline); 

/*update fw_cfg items with actual kernel/initrd*/ 
int relm_fw_cfg_patch_boot(struct relm_vm *vm, 
                           uint32_t kernel_size, 
                           uint32_t initrd_size); 

#endif 


