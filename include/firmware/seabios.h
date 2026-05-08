/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RELM_SEABIOS_H
#define RELM_SEABIOS_H
 
#include <linux/types.h>
#include <include/fw_cfg.h>
#include <include/e820.h>
 
struct relm_vm;
struct vcpu;

#define SEABIOS_SIZE            (128 * 1024ULL) /*128 KB*/ 

/*GPA where SeaBIOS is mapped at the top of the 4GB address space 
 * 0xFFFE0000 = 4GB - 128 KB. this covers the reset vector at 0xFFFFFF0*/
#define SEABIOS_ROM_TOP_GPA     (0x100000000ULL - SEABIOS_SIZE) 

/*GPA to where the reset vector executes a far jump to (1MB)
*/ 
#define SEABIOS_ROM_LOW_GPA     0x000E0000ULL 

/*EPT flags for BIOS ROM mapping
 * read and execute only, uncacheable*/ 
#define SEABIOS_EPT_FLAGS       (0x5ULL)   /* Read=1 Write=0 Execute=1 MT=UC */

/* Firmware name for request_firmware() lookup.
 * The kernel looks for this in /lib/firmware/ (and its subdirectories).
 * User must: cp /usr/share/seabios/bios.bin /lib/firmware/seabios/bios.bin */
#define SEABIOS_FIRMWARE_NAME   "seabios/bios.bin"




