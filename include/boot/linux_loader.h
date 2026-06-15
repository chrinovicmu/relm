#ifndef RELM_BOOT_LOADER_H
#define RELM_BOOT_LOADER_H

#include <linux/types.h>

struct relm_vm;
struct device;

struct relm_boot_config 
{
    const char *cmdline;
    const char *initrd_name;
    const char *kernel_name;
};

/* relm_boot_load — generic entry point for loading a guest OS. */ 
int relm_boot_load(struct relm_vm *vm,
                   struct device  *dev,
                   const struct relm_boot_config *cfg);

/*
 * relm_boot_info — log a human-readable summary of what was loaded.
 */
void relm_boot_info(const struct relm_vm *vm);

#endif /* RELM_BOOT_LOADER_H */
