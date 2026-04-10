/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_FW_CFG_H
#define RELM_FW_CFG_H
 
#include <linux/types.h>
#include <linux/spinlock.h>
#include <include/e820.h>
 
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
    uint32_t size;          /* big-endian */
    uint16_t select;        /* big-endian */
    /* reserved: padding, must be zero. */
    uint16_t reserved;
 
    /* name: null-terminated ASCII filename, at most 55 characters + null.
     * SeaBIOS searches this field for known filenames like "etc/e820".
     * The name is a path-like string but does not correspond to any real
     * filesystem — it is just a convention for identification. */
    char     name[56];      /* null-terminated, rest zero-padded */
 
} __attribute__((packed));

#define FW_CFG_MAX_ITEMS        32
#define FW_CFG_MAX_ITEM_SIZE    (4 * 1024)
#define FW_CFG_MAX_NAME_LEN     55
