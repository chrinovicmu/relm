#include <linux/build_bug.h>
#include <linux/types.h>

#define VMCB_SIZE 4096
#define VMCB_CONTROL_AREA_SIZE 0x400
#define VMCB_SAVE_AREA_OFFSET 0x400 

struct vmcb_control_area{

    u32 intercept_cr;         /* 0x000: lo 16 = CR reads, hi 16 = writes */
    u32 intercept_dr;         /* 0x004: same split for DR0..DR15*/
    u32 intercept_exceptions; /* 0x008: bit N = intercept vector N  */
    u32 intercept_1;          /* 0x00C: INTR/NMI/HLT/IOIO/MSR/CPUID */
    u32 intercept_2;          /* 0x010: VMRUN/VMMCALL/VMLOAD/VMSAVE  */
    u32 intercept_3;          /* 0x014: INVLPGB/INVPCID/..*/

    u8 resrved_1[0x03C - 0x018]; 

}
