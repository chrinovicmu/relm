#ifndef RELM_LINUX_LOADER_H
#define RELM_LINUX_LOADER_H

#include <linux/types.h>

struct relm_vm;
struct vcpu;
struct device;

 /* RELM GUEST MEMORY LAYOUT 
 *   GPA 0x00000000               start of guest RAM (128 MiB total)
 *   GPA 0x00007000               boot_params (zero page),       4 KiB
 *   GPA 0x00020000               kernel command line string,    4 KiB max
 *   GPA 0x000A0000 - 0x000FFFFF  legacy VGA / BIOS hole (reserved in e820)
 *   GPA 0x00100000               extended RAM begins (1 MiB)
 *   GPA 0x01000000               protected-mode kernel image (pref_address)
 *   GPA 0x04000000               initrd image (64 MiB load point)
 *   GPA 0x07FFD000 - 0x07FFFFFF  guest page tables */ 

/*BzImage magic at offset 0x1FE*/ 
#define RELM_BOOT_HDRS_MAGIC        0x53726448U  /* "HdrS" */
/* Magic value at offset 0x1FE of every bzImage's first sector. Same as MBR. */
#define RELM_BOOT_FLAG_MAGIC        0xAA55U
/* boot protocol version we require for 64-bit entry. */
#define RELM_BOOT_PROTOCOL_MIN      0x020CU      /* version 2.12 */
/* xloadflags bit 0 — "kernel has 64-bit entry point at startup_64". */
#define RELM_XLF_KERNEL_64          (1U << 0)
/* loadflags bit 0 — protected-mode kernel loaded high*/ 
#define RELM_LOADFLAGS_LOADED_HIGH  (1U << 0) 


#define RELM_BP_E820_MAX            128 
#define RELM_BP_LOADER_TYPE         0xFFU 

struct relm_setup_header 
{
    __u8  setup_sects;          /* 0x1F1: size of setup in 512-byte sectors */
    __u16 root_flags;           /* 0x1F2 */
    __u32 syssize;              /* 0x1F4: protected-mode code size / 16 */
    __u16 ram_size;             /* 0x1F8: obsolete */
    __u16 vid_mode;             /* 0x1FA: default video mode (we set 0xFFFF) */
    __u16 root_dev;             /* 0x1FC */
    __u16 boot_flag;            /* 0x1FE: 0xAA55 */
    __u16 jump;                 /* 0x200: short jump opcode + offset to header */
    __u32 header;               /* 0x202: "HdrS" magic */
    __u16 version;              /* 0x206: boot protocol version, e.g. 0x020F */
    __u32 realmode_swtch;       /* 0x208 */
    __u16 start_sys_seg;        /* 0x20C */
    __u16 kernel_version;       /* 0x20E */
    __u8  type_of_loader;       /* 0x210: we set 0xFF */
    __u8  loadflags;            /* 0x211 */
    __u16 setup_move_size;      /* 0x212 */
    __u32 code32_start;         /* 0x214 */
    __u32 ramdisk_image;        /* 0x218: GPA of initrd (low 32 bits) */
    __u32 ramdisk_size;         /* 0x21C: initrd size in bytes */
    __u32 bootsect_kludge;      /* 0x220 */
    __u16 heap_end_ptr;         /* 0x224 */
    __u8  ext_loader_ver;       /* 0x226 */
    __u8  ext_loader_type;      /* 0x227 */
    __u32 cmd_line_ptr;         /* 0x228: GPA of cmdline string */
    __u32 initrd_addr_max;      /* 0x22C: highest GPA initrd may occupy */
    __u32 kernel_alignment;     /* 0x230 */
    __u8  relocatable_kernel;   /* 0x234 */
    __u8  min_alignment;        /* 0x235 */
    __u16 xloadflags;           /* 0x236: 64-bit entry capability bits */
    __u32 cmdline_size;         /* 0x238: max cmdline length */
    __u32 hardware_subarch;     /* 0x23C */
    __u64 hardware_subarch_data;/* 0x240 */
    __u32 payload_offset;       /* 0x248 */
    __u32 payload_length;       /* 0x24C */
    __u64 setup_data;           /* 0x250: linked list of setup_data blobs */
    __u64 pref_address;         /* 0x258: kernel's preferred load GPA */
    __u32 init_size;            /* 0x260: runtime memory needed at pref_address */
    __u32 handover_offset;      /* 0x264 */
    __u32 kernel_info_offset;   /* 0x268 */
} __packed;


struct relm_bp_e820_entry {
    __u64 addr;
    __u64 size;
    __u32 type;
} __packed;

#define RELM_BP_OFFSET_E820_ENTRIES     0x1E8U
#define RELM_BP_OFFSET_HDR              0x1F1U
#define RELM_BP_OFFSET_E820_TABLE       0x2D0U
#define RELM_BP_SIZE                    0x1000U   /* one 4 KB page */

/* Offset of the "HdrS" header inside a bzImage file on disk. */
#define RELM_BZIMAGE_HDR_OFFSET         0x1F1U
#define RELM_STARTUP_64_OFFSET          0x200ULL

/*boot params zero page gpa*/ 
#define RELM_BOOT_PARAMS_GPA            0x00007000ULL

/* Kernel cmdline string location. */
#define RELM_CMDLINE_GPA                0x00020000ULL
#define RELM_CMDLINE_MAX_LEN            4096U

/*protected-mode kernel image gpa, 16 MB*/ 
#define RELM_KERNEL_LOAD_GPA            0x01000000ULL  /* 16 MiB */
#define RELM_KERNEL_MAX_SIZE            (64ULL * 1024 * 1024)

/*Initrd gpa*/ 
#define RELM_INITRD_LOAD_GPA            0x04000000ULL  /* 64 MiB */
#define RELM_INITRD_MAX_SIZE            (60ULL * 1024 * 1024)

#define RELM_KERNEL_FW_NAME             "relm/vmlinuz"
#define RELM_INITRD_FW_NAME             "relm/initrd.img"


/* Default cmdline if the caller passes NULL.
 *   console=ttyS0,115200   : kernel log to serial (no VGA in RELM)
 *   earlyprintk=serial     : earliest possible serial output
 *   nokaslr                : easier to debug; KASLR moves symbols
 *   panic=1                : reboot 1 s after panic (useful for tests) */
#define RELM_DEFAULT_CMDLINE \
    "console=ttyS0,115200 earlyprintk=serial nokaslr panic=1"

int relm_linux_loader_setup(struct relm_vm *vm, 
                      struct device *dev, 
                      const char *cmdline); 

int relm_kernel_load(struct relm_vm *vm, 
                     struct device *dev, 
                     struct relm_setup_header *hdr_out, 
                     uint64_t *kernel_entry_gpa); 

int relm_initrd_load(struct relm_vm *vm,
                     struct device *dev,
                     uint32_t *initrd_size);

int relm_cmdline_load(struct relm_vm *vm, const char *cmdline);

int relm_boot_params_build(struct relm_vm *vm,
                           const struct relm_setup_header *hdr,
                           uint32_t initrd_size);

uint64_t relm_linux_loader_entry_gpa(const struct relm_vm *vm);

#endif 


