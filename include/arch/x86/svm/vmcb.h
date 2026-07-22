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

    /*pause looop exiting throttle,*/ 
    u16 pause_filter_thresh /* 0x03C*/ 
    u16 pause_filter_count /* 0x03E*/ 

    u64 iopm_base_pa;  /* 0x040 */ 
    u64 msrpm_base_pa; /* 0x048 */ 

    u64 tsc_offset; /*0x50 */ 

    /*tlb tagging + flushing */ 
    u32 asid; /* 0x058 */ 
    u8 tlb_ctl; /*0x05C */ 
    u8 reserved_2[3]; /* 0x05D */

    u32 int_ctl; /* 0x060 */
    u32 int_vector; /* 0x064: bits 7:0 = V_INTR_VECTOR */
    u32 int_state;  /* 0x068 */
    u8 reserved_3[4]; /* 0x06C */

    u64 exit_code;         /* 0x070: -1 = VMEXIT_INVALID (bad VMCB)   */
    u64 exit_info_1;       /* 0x078 */
    u64 exit_info_2;       /* 0x080 */
    u32 exit_int_info;     /* 0x088 */
    u32 exit_int_info_err; /* 0x08C */ 

    /*nested paging controls */ 
    u64 nested_ctl; /* 0x090 */ 

    /*gpa of the virtual apic backing page if AVIC is enabled*/ 
    u64 avic_vapic_bar; /* 0x098*/ 

    /*gpa of guest-hypervisor communication block used in SEV-ES and SEV-SNP*/ 
    u64 ghcb_gpa; /*0x0A0 */ 

    /*LBR virtualization + virtulized VMSAVE/VMLOAD enables 
     * bit 0 : enable LBR virt 
     * bit 1 : enable VMLOAD/VMSAVE wihtout intecepts for nested 
     * virtualization */  
    u64 virt_ext; /* 0xB8 */
    u64 clean; /* 0x0C0 */
    u32 reserved_4; /* 0x0C4 */

    u64 next_rip;      /* 0x0C8 */
    u8 insn_len;       /* 0x0D0 */
    u8 insn_bytes[15]; /* 0x0D1 */

    /* AVIC page pointers — unused until the local-APIC bring-up step. */
    u64 avic_backing_page; /* 0x0E0 */
    u8 reserved_5[8];      /* 0x0E8 */
    u64 avic_logical_id;   /* 0x0F0 */
    u64 avic_physical_id;  /* 0x0F8 */

    u8 reserved_6[VMCB_CONTROL_AREA_SIZE - 0x100];
} __attribute__((packed)); 


