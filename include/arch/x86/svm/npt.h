#include <linux/types.h>
#include <linux/spinlock.h>

struct relm_vml; 
struct vcpu; 

/* PML4 -> PDPT -> PD -> PT*/ 
#define NPT_LEVELS               4

/*4 KiB table / 8-byte entiries */ 
#define NPT_ENTRIES_PER_TABLE   512 

/* Leaf sizes per level: PT leaf, PD leaf (PS=1), PDPT leaf (PS=1). */
#define NPT_PAGE_SIZE_4KB       (4ULL * 1024)
#define NPT_PAGE_SIZE_2MB       (2ULL * 1024 * 1024)
#define NPT_PAGE_SIZE_1GB       (1ULL * 1024 * 1024 * 1024)

#define NPT_PRESENT             (1ULL << 0)  
#define NPT_WRITABLE            (1ULL << 1)  
#define NPT_USER                (1ULL << 2)  
#define NPT_PWT                 (1ULL << 3)  
#define NPT_PCD                 (1ULL << 4)  
#define NPT_ACCESSED            (1ULL << 5) 
#define NPT_DIRTY               (1ULL << 6)  
#define NPT_PAGE_SIZE           (1ULL << 7)  
#define NPT_GLOBAL              (1ULL << 8)  
#define NPT_NX                  (1ULL << 63) 

#define NPT_ADDR_MASK           0x000FFFFFFFFFF000ULL
#define NPT_RWX                 (NPT_PRESENT | NPT_WRITABLE | NPT_USER)
#define NPT_RW                  (NPT_PRESENT | NPT_WRITABLE | NPT_USER | NPT_NX)
#define NPT_RX                  (NPT_PRESENT | NPT_USER)
#define NPT_R                   (NPT_PRESENT | NPT_USER | NPT_NX)

#define NPT_MEMTYPE_UC          (NPT_PWT | NPT_PCD)
#define NPT_MEMTYPE_WB          0ULL

#define NPT_PML4_INDEX(gpa)     (((gpa) >> 39) & 0x1FF)
#define NPT_PDPT_INDEX(gpa)     (((gpa) >> 30) & 0x1FF)
#define NPT_PD_INDEX(gpa)       (((gpa) >> 21) & 0x1FF)
#define NPT_PT_INDEX(gpa)       (((gpa) >> 12) & 0x1FF)
#define NPT_PAGE_OFFSET(gpa)    ((gpa) & 0xFFF)

/* #NPF error code — EXITINFO1 on exit 0x400 (point 4).
 *
 * Bits 15:0 mirror the architectural #PF error code (the same bits a real
 * #PF pushes on the exception stack), describing the access that faulted;
 * bits 63:32 add nested-paging context.
 */
#define NPF_ERR_PRESENT         (1ULL << 0)  /* 1 = NPT entry was present, so
                                              * this is a PERMISSION fault
                                              * (write to RO, fetch from NX);
                                              * 0 = not present — the case
                                              * RELM's MMIO trap produces   */
#define NPF_ERR_WRITE           (1ULL << 1)  /* 1 = faulting access was a
                                              * write, 0 = read             */
#define NPF_ERR_USER            (1ULL << 2)  /* user-mode access; for the
                                              * nested walk itself this is
                                              * always 1 (walk-as-user)     */
#define NPF_ERR_RSVD            (1ULL << 3)  /* reserved bit set in an NPT*/ 
#define NPF_ERR_FETCH           (1ULL << 4)  /* instruction fetch faulted
                                              * (NX set or not-present)     */
/* NPF-specific context, upper dword: */
#define NPF_ERR_GPA_FINAL       (1ULL << 32) /* faulted translating the
                                              * guest's FINAL physical addr*/ 
#define NPF_ERR_GPT_WALK        (1ULL << 33) /* faulted translating a GPA of
                                              * the GUEST'S OWN page tables
                                              * (its stage-1 walk touched an 
                                              * unmmaped/protected GPA */ 

/* Capability discovery — CPUID, not MSRs. */ 

#define SVM_CPUID_EXT_FEATURES  0x80000001   /* ECX bit 2 => SVM exists     */
#define SVM_CPUID_FEATURE_BIT   (1U << 2)

#define SVM_CPUID_SVM_FEATURES  0x8000000A   /* EDX = SVM feature flags     */
#define SVM_FEAT_NPT            (1U << 0)    /* nested paging supported —
                                              * the one bit we require      */
#define SVM_FEAT_LBR_VIRT       (1U << 1)    /* LBR virtualization          */
#define SVM_FEAT_NRIPS          (1U << 3)    /* next_rip valid on intercepts
                                              * (vmcb.h decode assists)     */
#define SVM_FEAT_FLUSH_ASID     (1U << 6)    /* TLB_CONTROL values 3/7 (per-
                                              * ASID) accepted, not just
                                              * flush-all                   */
#define SVM_FEAT_DECODE_ASSIST  (1U << 7)    /* insn_bytes valid on #NPF —
                                              * feeds the MMIO decoder      */

typedef uint64_t npt_entry_t; 

typedef struct {
    npt_entry_t entries[NPT_ENTRIES_PER_TABLE];
} __attribute__((aligned(4096))) npt_pml4_t;   /* level 4: root, nCR3 target */

typedef struct {
    npt_entry_t entries[NPT_ENTRIES_PER_TABLE];
} __attribute__((aligned(4096))) npt_pdpt_t;   /* level 3: 1 GiB per entry   */

typedef struct {
    npt_entry_t entries[NPT_ENTRIES_PER_TABLE];
} __attribute__((aligned(4096))) npt_pd_t;     /* level 2: 2 MiB per entry   */

typedef struct {
    npt_entry_t entries[NPT_ENTRIES_PER_TABLE];
} __attribute__((aligned(4096))) npt_pt_t;     /* level 1: 4 KiB per entry   */


struct npt_context {

    npt_pml4_t *pml4;       /* root table, kernel VA — for our own walks   */

    uint64_t pml4_pa;       /* root table, physical address THE nCR3 value*/ 

    struct {
        uint64_t pages_4kb;     /* 4 KiB leaves installed                  */
        uint64_t pages_2mb;     /* 2 MiB leaves (PS in PDE) — future       */
        uint64_t pages_1gb;     /* 1 GiB leaves (PS in PDPTE) — future     */
        uint64_t total_mapped;  /* bytes currently mapped                  */
    } stats;

    spinlock_t lock;
};

bool relm_npt_check_support(void);
int relm_setup_npt(struct relm_vm *vm);
struct npt_context *relm_npt_context_create(void);
void relm_npt_context_destroy(struct npt_context *npt);
int relm_npt_map_page(struct npt_context *npt, uint64_t gpa,
                      uint64_t hpa, uint64_t flags);
int relm_npt_map_range(struct npt_context *npt, uint64_t gpa_start,
                       uint64_t hpa_start, uint64_t size, uint64_t flags);
int relm_npt_unmap_page(struct npt_context *npt, uint64_t gpa);
int relm_npt_get_mapping(struct npt_context *npt, uint64_t gpa, uint64_t *hpa);
int relm_npt_create_guest_page_tables(struct relm_vm *vm);
int relm_npt_handle_fault(struct vcpu *vcpu, uint64_t gpa, uint64_t error_code);
void relm_npt_invalidate(struct relm_vm *vm);
void relm_npt_dump_tables(struct npt_context *npt);

#endif
