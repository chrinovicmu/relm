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

struct vmcb_seg {
    u16 selector;
    u16 attrib;
    u32 limit;
    u64 base;
} __attribute__((packed));


struct vmcb_save_area {
    struct vmcb_seg es;   /* 0x000 */
    struct vmcb_seg cs;   /* 0x010 */
    struct vmcb_seg ss;   /* 0x020 */
    struct vmcb_seg ds;   /* 0x030 */
    struct vmcb_seg fs;   /* 0x040: hidden part is tier 2       */
    struct vmcb_seg gs;   /* 0x050: hidden part is tier 2       */
    struct vmcb_seg gdtr; /* 0x060: base/limit only             */
    struct vmcb_seg ldtr; /* 0x070: tier 2                      */
    struct vmcb_seg idtr; /* 0x080: base/limit only             */
    struct vmcb_seg tr;   /* 0x090: tier 2                      */

    u8 reserved_1[43]; /* 0x0A0 */

    /*
    * Current privilege level, an explicit field.  VMX derives CPL from
    * SS.DPL; SVM stores it directly (real mode => 0, V86 => 3).
    */
    u8 cpl;           /* 0x0CB */
    u8 reserved_2[4]; /* 0x0CC */

    /*
    * Guest EFER``
    * SVME bit (12) must be set here or VMRUN fails with VMEXIT_INVALID —
    * even for guests that never heard of SVM.
    */
    u64 efer; /* 0x0D0 */

    u8 reserved_3[112]; /* 0x0D8 */

    u64 cr4;    /* 0x148 */
    u64 cr3;    /* 0x150 */
    u64 cr0;    /* 0x158 */
    u64 dr7;    /* 0x160 */
    u64 dr6;    /* 0x168 */
    u64 rflags; /* 0x170 */
    u64 rip;    /* 0x178 */

    u8 reserved_4[88]; /* 0x180 */

    u64 rsp; /* 0x1D8 */

    /* Shadow-stack (CET) state — modern parts only, unused in step 1. */
    u64 s_cet;     /* 0x1E0 */
    u64 ssp;       /* 0x1E8 */
    u64 isst_addr; /* 0x1F0 */

    /*
    * RAX — saved/loaded by VMRUN because VMRUN *consumes* the real RAX
    * as the VMCB pointer; the guest's RAX has to live somewhere else.
    * The other 14 GPRs have no field anywhere in the VMCB (tier 3).
    */
    u64 rax; /* 0x1F8 */
     u64 star;           /* 0x200 */
    u64 lstar;          /* 0x208 */
    u64 cstar;          /* 0x210 */
    u64 sfmask;         /* 0x218 */
    u64 kernel_gs_base; /* 0x220 */
    u64 sysenter_cs;    /* 0x228 */
    u64 sysenter_esp;   /* 0x230 */
    u64 sysenter_eip;   /* 0x238 */

    /*
   * CR2 gets a real field: VMRUN restores the guest's page-fault
   d* address so #PF handling inside the guest survives world switches.
   */
    u64 cr2; /* 0x240 */

    u8 reserved_5[32]; /* 0x248 */

    u64 g_pat; /* 0x268: guest PAT, honored during
              * nested walks — the NPT memtype
              * story, see vm_arch.h point 5 */

    /* Debug-control + last-branch-record MSRs (LBR virtualization). */
    u64 dbgctl;         /* 0x270 */
    u64 br_from;        /* 0x278 */
    u64 br_to;          /* 0x280 */
    u64 last_excp_from; /* 0x288 */
    u64 last_excp_to;   /* 0x290 */

    u8 reserved_6[(VMCB_SIZE - VMCB_SAVE_AREA_OFFSET) - 0x298];
} __attribute__((packed));

static_assert(sizeof(struct vmcb_control_area) == VMCB_CONTROL_AREA_SIZE);
static_assert(offsetof(struct vmcb_control_area, pause_filter_thresh) == 0x03C);
static_assert(offsetof(struct vmcb_control_area, iopm_base_pa) == 0x040);
static_assert(offsetof(struct vmcb_control_area, asid) == 0x058);
static_assert(offsetof(struct vmcb_control_area, int_ctl) == 0x060);
static_assert(offsetof(struct vmcb_control_area, exit_code) == 0x070);
static_assert(offsetof(struct vmcb_control_area, nested_ctl) == 0x090);
static_assert(offsetof(struct vmcb_control_area, event_inj) == 0x0A8);
static_assert(offsetof(struct vmcb_control_area, nested_cr3) == 0x0B0);
static_assert(offsetof(struct vmcb_control_area, next_rip) == 0x0C8);

static_assert(sizeof(struct vmcb_seg) == 16);
static_assert(offsetof(struct vmcb_save_area, cpl) == 0x0CB);
static_assert(offsetof(struct vmcb_save_area, efer) == 0x0D0);
static_assert(offsetof(struct vmcb_save_area, cr4) == 0x148);
static_assert(offsetof(struct vmcb_save_area, rflags) == 0x170);
static_assert(offsetof(struct vmcb_save_area, rip) == 0x178);
static_assert(offsetof(struct vmcb_save_area, rsp) == 0x1D8);
static_assert(offsetof(struct vmcb_save_area, rax) == 0x1F8);
static_assert(offsetof(struct vmcb_save_area, star) == 0x200);
static_assert(offsetof(struct vmcb_save_area, cr2) == 0x240);
static_assert(offsetof(struct vmcb_save_area, g_pat) == 0x268);

static_assert(offsetof(struct vmcb, save) == VMCB_SAVE_AREA_OFFSET);
static_assert(sizeof(struct vmcb) == VMCB_SIZE);

/*tlb control */ 
#define TLB_CONTROL_DO_NOTHING          0 
#define TLB_CONTROL_FLUSH_ALL_ASIDS     1
#define TLB_CONTROL_FLUSH_ASID          3 /*this vmcb */ 
#define TLB_CONTROL_SLUSH_ASID_LOCAL    7 /*this ASID, non-global only */ 


