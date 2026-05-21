/* SPDX-License-Identifier: GPL-2.0
 *
 * linux_loader.c — direct Linux/x86 boot loader.
 */ 

#include <linux/firmware.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mm.h>

#include <include/vm.h>
#include <include/ept.h>
#include <include/firmware/e820.h>
#include <include/boot/linux_loader.h>
#include <utils/utils.h>

static int relm_guest_write_u8(struct relm_vm *vm, uint64_t gpa, uint8_t v)
{
    int n = relm_vm_copy_to_guest(vm, gpa, &v, sizeof(v));
    return (n == sizeof(v)) ? 0 : (n < 0 ? n : -EIO);
}

static int relm_guest_write_u32(struct relm_vm *vm, uint64_t gpa, uint32_t v)
{
    int n = relm_vm_copy_to_guest(vm, gpa, &v, sizeof(v));
    return (n == sizeof(v)) ? 0 : (n < 0 ? n : -EIO);
}

static int relm_validate_setup_header(const struct relm_setup_header *hdr)
{
    if(hdr->boot_flag != RELM_BOOT_FLAG_MAGIC)
    {
        pr_err("RELM: linux_loader: bad boot_flag 0x%04x (expected 0x%04x)\n",
               hdr->boot_flag, RELM_BOOT_FLAG_MAGIC);
        return -EINVAL;
    }

    if(hdr->header != RELM_BOOT_HDRS_MAGIC)
    {
        pr_err("RELM: linux_loader: bad header magic 0x%08x "
               "(expected \"HdrS\" = 0x%08x)\n",
               hdr->header, RELM_BOOT_HDRS_MAGIC);
        return -EINVAL;
    }

    if(hdr->version < RELM_BOOT_PROTOCOL_MIN)
    {
        pr_err("RELM: linux_loader: boot protocol 0x%04x too old "
               "(need >= 0x%04x for 64-bit entry)\n",
               hdr->version, RELM_BOOT_PROTOCOL_MIN);
        return -ENOTSUPP;
    }

    if(!(hdr->xloadflags & RELM_XLF_KERNEL_64))
    {
        pr_err("RELM: linux_loader: bzImage has no 64-bit entry point "
               "(xloadflags=0x%04x, XLF_KERNEL_64 missing)\n",
               hdr->xloadflags);
        return -ENOTSUPP;
    }

    if(!(hdr->loadflags & RELM_LOADFLAGS_LOADED_HIGH))
    {
        pr_err("RELM: linux_loader: bzImage is not LOADED_HIGH "
               "(loadflags=0x%02x); zImage unsupported\n",
               hdr->loadflags);
        return -ENOTSUPP;
    }

    PDEBUG("RELM: linux_loader: setup_header OK "
           "(proto=0x%04x xload=0x%04x pref_addr=0x%llx init_size=%u)\n",
           hdr->version, hdr->xloadflags,
           (unsigned long long)hdr->pref_address, hdr->init_size);

    return 0;
}

int relm_kernel_load(struct relm_vm *vm, 
                     struct device *dev, 
                     struct relm_setup_header *hdr_out, 
                     uint64_t *kernel_entry_gpa)
{
    const struct firmware *fw = NULL; 
    struct relm_setup_header hdr; 
    uint64_t setup_size; 
    uint64_t pm_kernel_size; 
    uint64_t load_gpa; 
    int ret; 

    if(!vm || !dev || !hdr_out || !kernel_entry_gpa)
        return -EINVAL;

    /* Pull the bzImage off /lib/firmware/relm/vmlinuz. */
    ret = request_firmware(&fw, RELM_KERNEL_FW_NAME, dev);
    if(ret)
    {
        pr_err("RELM: linux_loader: failed to load '%s' (error %d)\n"
               "  Place a bzImage at /lib/firmware/%s\n",
               RELM_KERNEL_FW_NAME, ret, RELM_KERNEL_FW_NAME);
        return ret;
    }

    if(fw->size < RELM_BZIMAGE_HDR_OFFSET + sizeof(struct relm_setup_header))
    {
        pr_err("RELM: linux_loader: bzImage too small (%zu bytes)\n",
               fw->size);
        ret = -EINVAL;
        goto _out_release;
    }

    memcpy(&hdr, fw->data + RELM_BZIMAGE_HDR_OFFSET, sizeof(hdr)); 
    
    ret = relm_validate_setup_header(&hdr); 
    if(ret)
        goto _out_release; 

    /*real mode setup size in bytes */ 
    uint8_t sects = hdr.setup_sects ? hdr.setup_sects : 4;
    setup_size = ((uint64_t)sects + 1ULL) * 512ULL;
    if(fw->size <= setup_size)
    {
        pr_err("RELM: linux_loader: bzImage has no protected-mode payload "
               "(file=%zu setup=%llu)\n", fw->size, setup_size);
        ret = -EINVAL;
        goto _out_release;
    }

    /*protected mode kernel size*/ 
    pm_kernel_size = fw->size - setup_size; 
    if(pm_kernel_size > RELM_KERNEL_MAX_SIZE)
    {
        pr_err("RELM: linux_loader: kernel too large (%llu > %llu bytes)\n",
               pm_kernel_size, RELM_KERNEL_MAX_SIZE);
        ret = -E2BIG;
        goto _out_release;
    }

    /*honour kernels prefered load address if given*/ 
    load_gpa = hdr.pref_address ? hdr.pref_address : RELM_KERNEL_LOAD_GPA; 

    /* Sanity: don't run off the end of guest RAM, and make sure we're inside
     * the region the page tables identity-map (first 1 GiB). */
    if(load_gpa + pm_kernel_size > vm->total_guest_ram)
    {
        pr_err("RELM: linux_loader: kernel @0x%llx+%llu exceeds guest RAM %llu\n",
               load_gpa, pm_kernel_size, vm->total_guest_ram);
        ret = -ENOMEM;
        goto _out_release;
    }

    pr_info("RELM: linux_loader: bzImage %zu bytes "
            "(setup=%llu, pm_kernel=%llu)\n",
            fw->size, setup_size, pm_kernel_size);
    pr_info("RELM: linux_loader: copying protected-mode kernel to GPA 0x%llx\n",
            load_gpa);

    ret = relm_vm_copy_to_guest(vm, load_gpa,
                                fw->data + setup_size,
                                pm_kernel_size);
    if(ret < 0)
    {
        pr_err("RELM: linux_loader: copy_to_guest failed: %d\n", ret);
        goto _out_release;
    }

    /* startup_64 = pref_address + 0x200. This is the address we will write
     * into GUEST_RIP at VM-entry. */
    *kernel_entry_gpa = load_gpa + RELM_STARTUP_64_OFFSET;
    *hdr_out = hdr;

    pr_info("RELM: linux_loader: kernel loaded, startup_64 @ GPA 0x%llx\n",
            *kernel_entry_gpa);

    ret = 0;

_out_release:
    release_firmware(fw);
    return ret;
}

int relm_initrd_load(struct relm_vm *vm,
                     struct device *dev,
                     uint32_t *initrd_size)
{
    const struct firmware *fw = NULL;
    int ret;

    if(!vm || !dev || !initrd_size)
        return -EINVAL;

    *initrd_size = 0;

    ret = request_firmware(&fw, RELM_INITRD_FW_NAME, dev);
    if(ret)
    {
        pr_info("RELM: linux_loader: no initrd at '%s' (err %d) — "
                "booting without initramfs\n", RELM_INITRD_FW_NAME, ret);
        return 0;
    }

    if(fw->size > RELM_INITRD_MAX_SIZE)
    {
        pr_err("RELM: linux_loader: initrd too large (%zu > %llu)\n",
               fw->size, RELM_INITRD_MAX_SIZE);
        ret = -E2BIG;
        goto _out_release;
    }

    if(RELM_INITRD_LOAD_GPA + fw->size > vm->total_guest_ram)
    {
        pr_err("RELM: linux_loader: initrd would overflow guest RAM\n");
        ret = -ENOMEM;
        goto _out_release;
    }

    ret = relm_vm_copy_to_guest(vm, RELM_INITRD_LOAD_GPA,
                                fw->data, fw->size);
    if(ret < 0)
    {
        pr_err("RELM: linux_loader: initrd copy_to_guest failed: %d\n", ret);
        goto _out_release;
    }

    *initrd_size = (uint32_t)fw->size;

    pr_info("RELM: linux_loader: initrd %zu bytes loaded @ GPA 0x%llx\n",
            fw->size, (unsigned long long)RELM_INITRD_LOAD_GPA);

    ret = 0;

_out_release:
    release_firmware(fw);
    return ret;
}

int relm_cmdline_load(struct relm_vm *vm, const char *cmdline)
{
    size_t len;
    int ret;

    if(!vm)
        return -EINVAL;

    if(!cmdline)
        cmdline = RELM_DEFAULT_CMDLINE;

    /* +1 for the trailing NUL we always write. */
    len = strnlen(cmdline, RELM_CMDLINE_MAX_LEN - 1);

    /* Zero the entire cmdline window first so any old bytes don't leak into
     * a shorter new cmdline. */
    ret = relm_vm_zero_guest_memory(vm, RELM_CMDLINE_GPA, RELM_CMDLINE_MAX_LEN);
    if(ret < 0)
    {
        pr_err("RELM: linux_loader: zero cmdline area failed: %d\n", ret);
        return ret;
    }

    ret = relm_vm_copy_to_guest(vm, RELM_CMDLINE_GPA, cmdline, len);
    if(ret < 0)
    {
        pr_err("RELM: linux_loader: cmdline copy_to_guest failed: %d\n", ret);
        return ret;
    }

    /* Explicit NUL terminator at offset len. The zero pass above already
     * cleared this byte, but be explicit — future refactors of the zeroing
     * step shouldn't silently break the string. */
    ret = relm_guest_write_u8(vm, RELM_CMDLINE_GPA + len, 0);
    if(ret)
        return ret;

    pr_info("RELM: linux_loader: cmdline (%zu bytes) @ GPA 0x%llx: \"%s\"\n",
            len, (unsigned long long)RELM_CMDLINE_GPA, cmdline);

    return 0;
}


int relm_boot_params_build(struct relm_vm *vm, 
                           const struct relm_setup_header *hdr_in, 
                           uint32_t initrd_size)
{
    struct relm_setup_header hdr; 
    struct relm_e820_map e820; 
    uint32_t i; 
    int ret; 

    if(!vm || !hdr_in)
        return -EINVAL;

     /* Start with a clean 4 KiB page in the guest. */
    ret = relm_vm_zero_guest_memory(vm, RELM_BOOT_PARAMS_GPA, RELM_BP_SIZE);
    if(ret < 0)
    {
        pr_err("RELM: linux_loader: zero boot_params failed: %d\n", ret);
        return ret;
    }

    /* Build the e820 map for this VM (delegates to firmware/e820.c). */
    ret = relm_e820_build(vm, &e820);
    if(ret)
    {
        pr_err("RELM: linux_loader: e820_build failed: %d\n", ret);
        return ret;
    }
    relm_e820_dump(&e820);

    if(e820.count > RELM_BP_E820_MAX)
    {
        pr_err("RELM: linux_loader: too many e820 entries (%u > %u)\n",
               e820.count, RELM_BP_E820_MAX);
        return -E2BIG;
    }

    hdr = *hdr_in;
    hdr.type_of_loader = RELM_BP_LOADER_TYPE;
    hdr.cmd_line_ptr = (uint32_t)RELM_CMDLINE_GPA;
    hdr.cmdline_size = RELM_CMDLINE_MAX_LEN - 1;
    hdr.ramdisk_image = initrd_size ? (uint32_t)RELM_INITRD_LOAD_GPA : 0;
    hdr.ramdisk_size = initrd_size;
    hdr.vid_mode = 0xFFFFU;

    ret = relm_vm_copy_to_guest(vm,
                                RELM_BOOT_PARAMS_GPA + RELM_BP_OFFSET_HDR,
                                &hdr, sizeof(hdr));
    if(ret < 0)
    {
        pr_err("RELM: linux_loader: write setup_header failed: %d\n", ret);
        return ret;
    }
   
    ret = relm_guest_write_u8(vm,
                              RELM_BOOT_PARAMS_GPA + RELM_BP_OFFSET_E820_ENTRIES,
                              (uint8_t)e820.count);
    if(ret)
        return ret;

    for(i = 0; i < e820.count; i++)
    {
        struct relm_bp_e820_entry e;
        uint64_t gpa = RELM_BOOT_PARAMS_GPA +
                       RELM_BP_OFFSET_E820_TABLE +
                       i * sizeof(e);

        e.addr = e820.entries[i].addr;
        e.size = e820.entries[i].size;
        e.type = e820.entries[i].type;

        ret = relm_vm_copy_to_guest(vm, gpa, &e, sizeof(e));
        if(ret < 0)
        {
            pr_err("RELM: linux_loader: write e820[%u] failed: %d\n", i, ret);
            return ret;
        }
    }

    vm->boot_params_gpa = RELM_BOOT_PARAMS_GPA;

    pr_info("RELM: linux_loader: boot_params @ GPA 0x%llx "
            "(cmdline=0x%x initrd=0x%x/%u e820_entries=%u)\n",
            (unsigned long long)RELM_BOOT_PARAMS_GPA,
            hdr.cmd_line_ptr, hdr.ramdisk_image, hdr.ramdisk_size,
            e820.count);

    return 0;

}

int relm_linux_loader_setup(struct relm_vm *vm,
                            struct device *dev,
                            const char *cmdline)
{
    struct relm_setup_header hdr;
    uint64_t entry_gpa = 0;
    uint32_t initrd_size = 0;
    int ret;

    if(!vm || !dev)
        return -EINVAL;

    pr_info("RELM: linux_loader: starting direct kernel boot setup\n");

    ret = relm_kernel_load(vm, dev, &hdr, &entry_gpa);
    if(ret)
    {
        pr_err("RELM: linux_loader: kernel load failed: %d\n", ret);
        return ret;
    }

    ret = relm_initrd_load(vm, dev, &initrd_size);
    if(ret)
    {
        pr_err("RELM: linux_loader: initrd load failed: %d\n", ret);
        return ret;
    }

    ret = relm_cmdline_load(vm, cmdline);
    if(ret)
    {
        pr_err("RELM: linux_loader: cmdline load failed: %d\n", ret);
        return ret;
    }

    ret = relm_boot_params_build(vm, &hdr, initrd_size);
    if(ret)
    {
        pr_err("RELM: linux_loader: boot_params build failed: %d\n", ret);
        return ret;
    }

    vm->kernel_entry_gpa = entry_gpa;

    pr_info("RELM: linux_loader: setup complete\n");
    pr_info("RELM:   kernel entry (startup_64) GPA = 0x%llx\n", entry_gpa);
    pr_info("RELM:   boot_params (zero page)  GPA = 0x%llx\n",
            (unsigned long long)vm->boot_params_gpa);
    pr_info("RELM:   guest will enter directly in 64-bit long mode\n");

    return 0;
}

uint64_t relm_linux_loader_entry_gpa(const struct relm_vm *vm)
{
    return vm ? vm->kernel_entry_gpa : 0;
}


