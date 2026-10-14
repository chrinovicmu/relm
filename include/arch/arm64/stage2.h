#ifndef RELM_ARCH_ARM64_STAGE2_H
#define RELM_ARCH_ARM64_STAGE2_H

#include <linux/build_bug.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct relm_vm;
struct vcpu;
struct relm_stage2;
struct relm_stage2_table;

/* Walk geometry: Arm numbers levels from the root, L0 through L3. */
#define S2_LEVELS                  4U
#define S2_ENTRIES_PER_TABLE       512U
#define S2_GRANULE_SHIFT           12U
#define S2_TABLE_SIZE              (1ULL << S2_GRANULE_SHIFT)
#define S2_MIN_IPA_BITS            32U
#define S2_MAX_IPA_BITS            48U
#define S2_MAX_PA_BITS             48U
#define S2_DEFAULT_IPA_BITS        39U
#define S2_DESC_VALID              (1ULL << 0)
#define S2_DESC_TYPE_MASK          0x3ULL
#define S2_DESC_INVALID            0x0ULL
#define S2_DESC_BLOCK              0x1ULL
#define S2_DESC_TABLE              0x3ULL
#define S2_DESC_PAGE               0x3ULL

/* Leaf MemAttr[3:0], bits [5:2], with HCR_EL2.FWB=0 — direct stage-2
 * encodings, not indices into guest MAIR_EL1. Device mappings must be XN. */
#define S2_MEMATTR_SHIFT           2U
#define S2_MEMATTR_MASK            (0xfULL << S2_MEMATTR_SHIFT)
//mmio 
#define S2_MEMTYPE_DEVICE_NGNRNE    (0x0ULL << S2_MEMATTR_SHIFT)
#define S2_MEMTYPE_DEVICE_NGNRE     (0x1ULL << S2_MEMATTR_SHIFT)

#define S2_MEMTYPE_NORMAL_NC        (0x5ULL << S2_MEMATTR_SHIFT)
#define S2_MEMTYPE_NORMAL_WB        (0xfULL << S2_MEMATTR_SHIFT)


/* S2AP[1:0], bits [7:6]: 00 denies, 01 read, 10 write, 11 both. Stage 1 can
 * restrict further; there's no x86 U/S bit — kernel and user both go
 * through this same stage. */
#define S2_ACCESS_READ             (1ULL << 6)
#define S2_ACCESS_WRITE            (1ULL << 7)
#define S2_ACCESS_MASK             (S2_ACCESS_READ | S2_ACCESS_WRITE)

/* SH[1:0], bits [9:8], baseline format (VTCR_EL2.DS=0). */
#define S2_SH_MASK                 (0x3ULL << 8)
#define S2_SH_NON_SHAREABLE        (0x0ULL << 8)
#define S2_SH_OUTER                (0x2ULL << 8)
#define S2_SH_INNER                (0x3ULL << 8)

/* AF, bit 10: with HW updates disabled, clear AF faults on access*/ 
#define S2_AF                      (1ULL << 10)

/* XN, bit 54: execute permission is "clear XN", not a separate X bit. */
#define S2_XN                      (1ULL << 54)
#define S2_RWX                     (S2_ACCESS_READ | S2_ACCESS_WRITE)
#define S2_RW                      (S2_RWX | S2_XN)
#define S2_RX                      S2_ACCESS_READ
#define S2_R                       (S2_ACCESS_READ | S2_XN)

#define S2_LEAF_FLAGS_MASK         (S2_MEMATTR_MASK | S2_ACCESS_MASK | \
                                   S2_SH_MASK | S2_XN)

/* PA payloads, <=48-bit non-LPA2 format. Block alignment exceeds page
 * alignment, so a block can't just reuse the page mask. */
#define S2_TABLE_ADDR_MASK         0x0000fffffffff000ULL
#define S2_PAGE_ADDR_MASK          S2_TABLE_ADDR_MASK
#define S2_BLOCK_2MB_ADDR_MASK     (S2_TABLE_ADDR_MASK & \
                                   ~(S2_PAGE_SIZE_2MB - 1))
#define S2_BLOCK_1GB_ADDR_MASK     (S2_TABLE_ADDR_MASK & \
                                   ~(S2_PAGE_SIZE_1GB - 1))

/* VTCR_EL2 fields: T0SZ = 64 - ipa_bits; SL0 is an encoding (L0=2, L1=1 for
 * 4 KiB granules), not the numeric level. IRGN0/ORGN0/SH0 describe walks
 * of RELM's own tables, separate from each leaf's guest-RAM attributes. */
#define S2_VTCR_T0SZ_MASK          0x3fULL
#define S2_VTCR_SL0_SHIFT          6U
#define S2_VTCR_SL0_MASK           (0x3ULL << S2_VTCR_SL0_SHIFT)
#define S2_VTCR_SL0_L0             (0x2ULL << S2_VTCR_SL0_SHIFT)
#define S2_VTCR_SL0_L1             (0x1ULL << S2_VTCR_SL0_SHIFT)
#define S2_VTCR_IRGN0_WBWA         (0x1ULL << 8)
#define S2_VTCR_ORGN0_WBWA         (0x1ULL << 10)
#define S2_VTCR_SH0_INNER          (0x3ULL << 12)
#define S2_VTCR_TG0_4K             (0x0ULL << 14)
#define S2_VTCR_PS_SHIFT           16U
#define S2_VTCR_PS_MASK            (0x7ULL << S2_VTCR_PS_SHIFT)
#define S2_VTCR_PS_32BIT           (0x0ULL << S2_VTCR_PS_SHIFT)
#define S2_VTCR_PS_36BIT           (0x1ULL << S2_VTCR_PS_SHIFT)
#define S2_VTCR_PS_40BIT           (0x2ULL << S2_VTCR_PS_SHIFT)
#define S2_VTCR_PS_42BIT           (0x3ULL << S2_VTCR_PS_SHIFT)
#define S2_VTCR_PS_44BIT           (0x4ULL << S2_VTCR_PS_SHIFT)
#define S2_VTCR_PS_48BIT           (0x5ULL << S2_VTCR_PS_SHIFT)
#define S2_VTCR_VS_8BIT            (0x0ULL << 19)
#define S2_VTCR_VS_16BIT           (0x1ULL << 19)
#define S2_VTCR_HA                 (1ULL << 21) /* Kept clear initially. */
#define S2_VTCR_HD                 (1ULL << 22) /* Kept clear initially. */
#define S2_VTCR_RES1               (1ULL << 31)
#define S2_VTCR_DS                 (1ULL << 32) /* No LPA2 descriptors. */

#define S2_VTCR_WALK_FLAGS         (S2_VTCR_RES1 | S2_VTCR_SH0_INNER | \
                                   S2_VTCR_ORGN0_WBWA | \
                                   S2_VTCR_IRGN0_WBWA | S2_VTCR_TG0_4K)

/* VTTBR_EL2: VMID at bit 48 (top byte zero in 8-bit mode). CnP stays 0. */
#define S2_VTTBR_VMID_SHIFT        48U
#define S2_VTTBR_VMID_MASK_8BIT    (0xffULL << S2_VTTBR_VMID_SHIFT)
#define S2_VTTBR_VMID_MASK_16BIT   (0xffffULL << S2_VTTBR_VMID_SHIFT)
#define S2_VTTBR_BADDR_MASK        S2_TABLE_ADDR_MASK
#define S2_VTTBR_CNP               (1ULL << 0)

/* Abort syndrome fields in ESR_EL2 — the stage-2 counterpart of EPT
 * qualification/NPF bits. EC must be a lower-EL abort before using these. */
#define S2_ESR_EC_SHIFT            26U
#define S2_ESR_EC_MASK             (0x3fULL << S2_ESR_EC_SHIFT)
#define S2_ESR_EC_IABT_LOWER       (0x20ULL << S2_ESR_EC_SHIFT)
#define S2_ESR_EC_DABT_LOWER       (0x24ULL << S2_ESR_EC_SHIFT)
#define S2_ESR_FSC_MASK            0x3fULL
#define S2_ESR_FSC_TYPE_MASK       0x3cULL
#define S2_ESR_FSC_LEVEL_MASK      0x03ULL
#define S2_ESR_FSC_TRANSLATION     0x04ULL
#define S2_ESR_FSC_ACCESS_FLAG     0x08ULL
#define S2_ESR_FSC_PERMISSION      0x0cULL
#define S2_ESR_WNR                 (1ULL << 6)  /* Data-abort: write, not read. */
#define S2_ESR_S1PTW               (1ULL << 7)  /* Fault during the guest's own stage-1 walk. */
#define S2_ESR_FNV                 (1ULL << 10) /* FAR not valid for this fault. */
#define S2_ESR_ISV                 (1ULL << 24) /* Instruction syndrome valid. */


#define S2_PAGE_SIZE_4KB           (1ULL << 12)
#define S2_PAGE_SIZE_2MB           (1ULL << 21)
#define S2_PAGE_SIZE_1GB           (1ULL << 30)

/*
 * L0 selects a 512 GiB span and must point to L1 in this format.
 * L1 can point to L2 or contain a 1 GiB block; L2 can point to L3 or
 * contain a 2 MiB block; L3 contains 4 KiB pages, never another table.
 * A 39-bit IPA root starts at L1. These macros do not validate the IPA;
 * validate its width first so high bits cannot alias an existing mapping.
 */
#define S2_L0_INDEX(ipa)           (((u64)(ipa) >> 39) & 0x1ffULL)
#define S2_L1_INDEX(ipa)           (((u64)(ipa) >> 30) & 0x1ffULL)
#define S2_L2_INDEX(ipa)           (((u64)(ipa) >> 21) & 0x1ffULL)
#define S2_L3_INDEX(ipa)           (((u64)(ipa) >> 12) & 0x1ffULL)
#define S2_PAGE_OFFSET(ipa)        ((u64)(ipa) & (S2_PAGE_SIZE_4KB - 1))


typedef u64 s2_entry_t;

typedef struct {
    s2_entry_t entries[S2_ENTRIES_PER_TABLE];
} __attribute__((aligned(4096))) s2_l0_t;

typedef struct {
    s2_entry_t entries[S2_ENTRIES_PER_TABLE];
} __attribute__((aligned(4096))) s2_l1_t;

typedef struct {
    s2_entry_t entries[S2_ENTRIES_PER_TABLE];
} __attribute__((aligned(4096))) s2_l2_t;

typedef struct {
    s2_entry_t entries[S2_ENTRIES_PER_TABLE];
} __attribute__((aligned(4096))) s2_l3_t;

static_assert(sizeof(s2_entry_t) == 8);
static_assert(sizeof(s2_l0_t) == S2_TABLE_SIZE);
static_assert(sizeof(s2_l1_t) == S2_TABLE_SIZE);
static_assert(sizeof(s2_l2_t) == S2_TABLE_SIZE);
static_assert(sizeof(s2_l3_t) == S2_TABLE_SIZE);
static_assert(__alignof__(s2_l0_t) == S2_TABLE_SIZE);
static_assert(__alignof__(s2_l1_t) == S2_TABLE_SIZE);
static_assert(__alignof__(s2_l2_t) == S2_TABLE_SIZE);
static_assert(__alignof__(s2_l3_t) == S2_TABLE_SIZE);

struct relm_stage2_geometry {
    u8 ipa_bits;      /* 32..48; controls T0SZ and the guest's physical limit. */
    u8 pa_bits;       /* Output PA width: 32/36/40/42/44/48. */
    u8 start_level;   /* First level walked: 1 for ipa_bits<=39, else 0. */
    u8 vmid_bits;     /* Allocator/HW VMID width, 8 or 16 -> VTCR_EL2.VS. */
};

/*
 * Boundary with the future EL2/CPU-lifecycle backend. No default callback
 * here fakes a successful TLB flush. host_pfr0/mmfr0/mmfr1_el1 describe a
 * validated common host config across all eligible CPUs, not a guest's
 * feature_regs values. The provider guarantees little-endian walks,
 * HCR.FWB=0, and the baseline controls above on every entry.
 */
struct relm_stage2_runtime_ops {
    u64 host_pfr0_el1;  /* ID_AA64PFR0_EL1: EL2 availability. */
    u64 host_mmfr0_el1; /* ID_AA64MMFR0_EL1: PA range, stage-2 granules. */
    u64 host_mmfr1_el1; /* ID_AA64MMFR1_EL1: VMIDBits. */

    /* Take an exclusive guest-entry gate, unload this context everywhere,
     * drain its HW/SW EL2 walkers. No gate held on error. Lock order this
     * establishes: gate -> update_lock. */
    int (*begin_update)(struct relm_stage2 *stage2);

    /* Release the gate. runnable=true publishes the context's current
     * root/VTCR/VMID before reopening entry; false keeps entry blocked
     * (maintenance failure or destruction). Must not fail. */
    void (*end_update)(struct relm_stage2 *stage2, bool runnable);

    /* Make a host table allocation visible to EL2 walks (cache clean +
     * barriers). Must not fail; the table is still unpublished as a root. */
    void (*publish_table)(const void *address, size_t size);

    /* Synchronous, all-relevant-CPU stage-1+stage-2 invalidation for this
     * VTCR/VTTBR. Called with the entry gate and update_lock held. */
    int (*invalidate)(u64 vtcr, u64 vttbr);
};

nt relm_stage2_register_runtime(const struct relm_stage2_runtime_ops *ops);
int relm_stage2_unregister_runtime(void);


struct relm_stage2{
    s2_entry_t *root; /*host VA of the root table */ 
    phys_addr_t root_pa;
    struct relm_stage2_geometry geometry;
    
    u64 vtcr_el2; 
    u16 vmid; 
    u64 vmid_generation;
    bool vmid_valid; 
    bool failed; 
    struct relm_stage2_table *owned_table; 

    struct {
        u64 pages_4kb;
        u64 pages_2mb;
        u64 pages_1gb;     /* Stays zero until 1 GiB mappings are supported. */
        u64 total_mapped;
    } stats;

     struct mutex update_lock;
}; 

bool relm_stage2_check_support(void);

int relm_stage2_choose_geometry(unsigned int requested_ipa_bits,
                               struct relm_stage2_geometry *geometry);

int relm_stage2_encode_vtcr(const struct relm_stage2_geometry *geometry,
                           u64 *vtcr);
int relm_stage2_encode_vttbr(const struct relm_stage2_geometry *geometry,
                            phys_addr_t root_pa, u16 vmid, u64 *vttbr);

int relm_setup_stage2(struct relm_vm *vm);

struct relm_stage2 *relm_stage2_context_create(
    const struct relm_stage2_geometry *geometry);

int relm_stage2_context_destroy(struct relm_stage2 *stage2);

int relm_stage2_map_page(struct relm_stage2 *stage2, u64 ipa,
                        phys_addr_t pa, u64 flags);

/* Contiguous range via 4 KiB leaves. Rolls back this call's own changes on
 * failure; never removes a pre-existing mapping. */
int relm_stage2_map_range(struct relm_stage2 *stage2, u64 ipa,
                         phys_addr_t pa, u64 size, u64 flags);

/* One aligned 2 MiB L2 block; backing must be physically contiguous, owned,
 * uniform. No 1 GiB mapping API yet despite the format being defined. */
int relm_stage2_map_huge_page(struct relm_stage2 *stage2, u64 ipa,
                             phys_addr_t pa, u64 flags);

int relm_stage2_map_guest_ram_4kb(struct relm_stage2 *stage2,
                                 struct relm_vm *vm, u64 ipa,
                                 phys_addr_t pa, u64 size, u64 flags);

/* preferring 2 MiB blocks where alignment and MMIO-freedom allow;
 * falls back to 4 KiB leaves per misaligned/overlapping stride. */
int relm_stage2_map_guest_ram_huge(struct relm_stage2 *stage2,
                                  struct relm_vm *vm, u64 ipa,
                                  phys_addr_t pa, u64 size, u64 flags);

int relm_stage2_unmap_page(struct relm_stage2 *stage2, u64 ipa);

int relm_stage2_unmap_range(struct relm_stage2 *stage2, u64 ipa, u64 size);

int relm_stage2_get_mapping(struct relm_stage2 *stage2, u64 ipa,
                           phys_addr_t *pa);

int relm_stage2_handle_fault(struct vcpu *vcpu);

int relm_stage2_invalidate_context(struct relm_stage2 *stage2);

void relm_stage2_dump_tables(struct relm_stage2 *stage2);

#endif /* RELM_ARCH_ARM64_STAGE2_H */

