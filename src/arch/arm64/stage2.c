#include <linux/errno.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/io.h>

#include <relm/vm.h>
#include <relm/vcpu.h>
#include <stage2.h>


struct relm_stage2_table {
    s2_entry_t *entries;
    phys_addr_t pa;
    unsigned int level;
    struct relm_stage2_table *next;
};


struct s2_walk {
    s2_entry_t *entry;
    u64 descriptor;
    u64 base;
    u64 size;
    unsigned int level;
};

/*
 * A point-in-time copy of vm->memory.mmio_regions, taken under vm->lock
 * and then used WITHOUT that lock for the rest of a mapping operation.
 * This exists because s2_map() below needs to consult the MMIO list many
 * times while it may also sleep (table allocation is GFP_KERNEL, and
 * s2_begin_update()/s2_runtime.invalidate() are not guaranteed non-sleeping
 * either) — and vm->lock is a plain spinlock, so nothing that can sleep is
 * allowed to run while holding it. Snapshotting once up front avoids ever
 * needing to re-acquire that spinlock mid-mapping.
*/ 
struct s2_mmio_snapshot {
    unsigned int count;
    struct {
        u64 start;
        u64 end;
    } region[RELM_MAX_MMIO_REGIONS];
};

/*
 * What kind of leaf s2_map()'s inner loop should try to install for each
 * stride of the requested range:
 *   S2_MAP_PAGES          — always 4 KiB leaves (relm_stage2_map_page/range).
 *   S2_MAP_BLOCK          — always one 2 MiB leaf; caller already validated
 *                           the whole request is exactly 2 MiB and aligned
 *                           (relm_stage2_map_huge_page).
 *   S2_MAP_PREFER_BLOCKS  — try a 2 MiB block per stride, falling back to
 *                           4 KiB leaves wherever alignment or an MMIO hole
 *                           doesn't allow one (relm_stage2_map_guest_ram_huge).
 */
enum s2_map_mode {
    S2_MAP_PAGES,
    S2_MAP_BLOCK,
    S2_MAP_PREFER_BLOCKS,
};


/*
 * Global (not per-VM) state. There is exactly one EL2 runtime for the
 * whole host, so the registration/limits/VMID-namespace below are process-
 * wide singletons, mirrored by the single s2_runtime_lock protecting all
 * of them.
 */
static DEFINE_MUTEX(s2_runtime_lock);
static struct relm_stage2_runtime_ops s2_runtime; /* Copy of the registered callback struct. */
static struct relm_stage2_geometry s2_limits;     /* This host's maximum supported geometry. */
static bool s2_runtime_ready;      /* True between a successful register and unregister. */
static unsigned int s2_contexts;   /* Live context count; pins the runtime, blocks unregister. */
static u64 s2_generation;          /* Bumped on (re)registration; a context's VMID is only valid for the generation it was allocated under. */

/*
 * One bit per possible VMID value, 0..65535, covering both the 8-bit and
 * 16-bit VMID spaces at once (an 8-bit-VMID context only ever allocates
 * from 0..255, but the bitmap doesn't need to know that — s2_alloc_vmid()
 * is simply told how many bits wide to search). Bit 0 (VMID 0) is pre-set
 * so it is never handed out: VMID 0 is conventionally reserved, not a real
 * VM's translation tag.
 */
static u64 s2_vmids[65536U / 64U];

static int s2_pa_encoding(unsigned int bits)
{
    switch (bits) {
    case 32: return 0;
    case 36: return 1;
    case 40: return 2;
    case 42: return 3;
    case 44: return 4;
    case 48: return 5;
    default: return -EINVAL;
    }
}

static bool s2_geometry_valid(const struct relm_stage2_geometry *geometry)
{
    return geometry && geometry->ipa_bits >= S2_MIN_IPA_BITS &&
           geometry->ipa_bits <= S2_MAX_IPA_BITS &&
           s2_pa_encoding(geometry->pa_bits) >= 0 &&
           geometry->start_level == (geometry->ipa_bits <= 39 ? 1 : 0) &&
           (geometry->vmid_bits == 8 || geometry->vmid_bits == 16);
}


static bool s2_range_fits(u64 address, u64 size, unsigned int bits)
{
    u64 limit = 1ULL << bits;

    return size && address < limit && size <= limit - address;
}

static bool s2_page_aligned(u64 value)
{
    return !(value & (S2_PAGE_SIZE_4KB - 1));
}

int relm_stage2_encode_vtcr(const struct relm_stage2_geometry *geometry,
                           u64 *vtcr)
{
    u64 value;

    if (!vtcr || !s2_geometry_valid(geometry))
        return -EINVAL;

    value = S2_VTCR_WALK_FLAGS | (64U - geometry->ipa_bits);
    value |= geometry->start_level ? S2_VTCR_SL0_L1 : S2_VTCR_SL0_L0;
    value |= (u64)s2_pa_encoding(geometry->pa_bits) << S2_VTCR_PS_SHIFT;
    if (geometry->vmid_bits == 16)
        value |= S2_VTCR_VS_16BIT;
    *vtcr = value;
    return 0;
}

int relm_stage2_encode_vttbr(const struct relm_stage2_geometry *geometry,
                            phys_addr_t root_pa, u16 vmid, u64 *vttbr)
{
    if (!vttbr || !s2_geometry_valid(geometry) || !s2_page_aligned(root_pa))
        return -EINVAL;
    if (!s2_range_fits(root_pa, S2_TABLE_SIZE, geometry->pa_bits) ||
        (geometry->vmid_bits == 8 && vmid > 255))
        return -ERANGE;

    /* The validated address needs no lossy mask; CnP and reserved bits stay 0. */
    *vttbr = (u64)root_pa | ((u64)vmid << S2_VTTBR_VMID_SHIFT);
    return 0;
}

/*
 * Reads three raw host ID registers (handed in by whatever registers the
 * EL2 runtime, NOT read live from hardware here) and turns them into "the
 * widest stage-2 geometry this host can actually support." Every field
 * extracted below is a literal bitfield from the real architectural
 * register, cross-checked against Linux's own arch/arm64/tools/sysreg
 * table so the shifts/widths here are not guesswork:
 *
 *   - el2  = ID_AA64PFR0_EL1.EL2[11:8]. 1 or 2 both mean "AArch64 EL2 is
 *     implemented" (2 additionally means EL2 also supports AArch32, which
 *     this backend doesn't care about) — anything else (0 = no EL2, or an
 *     AArch32-only encoding) means this backend cannot run here at all.
 *   - granule/granule2 = ID_AA64MMFR0_EL1.TGran4[31:28] and .TGran4_2[43:40].
 *     TGran4_2 is the NEWER, unambiguous field; when it is 0 ("not
 *     implemented, defer to the old field") this falls back to checking
 *     TGran4 directly (0 or 1 both mean "4 KiB supported", using slightly
 *     different historical encodings). When TGran4_2 IS implemented, only
 *     its own 2/3 encodings mean "supported" — this is exactly the
 *     "TGran4_2==0 inherits TGran4" rule called out below.
 *   - pa   = ID_AA64MMFR0_EL1.PARange[3:0], an index into pa_widths[] (NOT
 *     a bit count by itself — that's why there's a lookup table instead of
 *     just shifting a number).
 *   - vmids = ID_AA64MMFR1_EL1.VMIDBits[7:4]. The architecturally confusing
 *     part: the encoding for "16-bit VMIDs supported" is the numeral 2, not
 *     16 and not 1 — hence the explicit `vmids == 2 ? 16 : 8` translation
 *     at the bottom rather than any arithmetic on the raw field.
 *
 * PAGE_SHIFT != S2_GRANULE_SHIFT and CONFIG_CPU_BIG_ENDIAN both bail out
 * because this whole implementation hard-codes 4 KiB little-endian walks
 * (see the file-level "Initial implementation contract" in stage2.h) —
 * these aren't host capability checks, they're "this build of the kernel
 * itself is not one this code was written for" checks.
 */
static int s2_decode_limits(const struct relm_stage2_runtime_ops *ops,
                            struct relm_stage2_geometry *limits)
{
    static const u8 pa_widths[] = { 32, 36, 40, 42, 44, 48, 52, 56 };
    u64 el2 = (ops->host_pfr0_el1 >> 8) & 0xf;
    u64 granule = (ops->host_mmfr0_el1 >> 28) & 0xf;
    u64 granule2 = (ops->host_mmfr0_el1 >> 40) & 0xf;
    u64 pa = ops->host_mmfr0_el1 & 0xf;
    u64 vmids = (ops->host_mmfr1_el1 >> 4) & 0xf;

    if (PAGE_SHIFT != S2_GRANULE_SHIFT)
        return -EOPNOTSUPP;
#ifdef CONFIG_CPU_BIG_ENDIAN
    return -EOPNOTSUPP;
#endif
    if ((el2 != 1 && el2 != 2) || pa >= ARRAY_SIZE(pa_widths) ||
        (vmids != 0 && vmids != 2))
        return -EOPNOTSUPP;
    if (granule2 == 0) {
        if (granule != 0 && granule != 1)
            return -EOPNOTSUPP;
    } else if (granule2 != 2 && granule2 != 3) {
        return -EOPNOTSUPP;
    }

    /* Clamp to 48: this backend's descriptor format (non-LPA2) tops out
     * there even if the real hardware PARange reports a wider 52/56-bit
     * address space — see S2_MAX_PA_BITS in stage2.h. */
    limits->pa_bits = pa_widths[pa] > 48 ? 48 : pa_widths[pa];
    limits->ipa_bits = limits->pa_bits;
    limits->start_level = limits->ipa_bits <= 39 ? 1 : 0;
    limits->vmid_bits = vmids == 2 ? 16 : 8;
    return 0;
}


/*
 * One-time handshake: the (future) EL2 runtime calls this exactly once to
 * hand over its callback table and the host ID-register values that
 * describe every CPU it is willing to run guests on. Rejected while a
 * previous registration is still active (s2_runtime_ready) OR while any
 * context still exists (s2_contexts) — re-registering out from under a
 * live context would silently change what "this host's limits" means for
 * tables that already exist. s2_generation == ~0 is treated as permanent
 * exhaustion (-EOVERFLOW) rather than wrapping back to 0, because a
 * wrapped generation counter could make a STALE context's vmid_generation
 * field compare as valid again by coincidence.
 */
int relm_stage2_register_runtime(const struct relm_stage2_runtime_ops *ops)
{
    struct relm_stage2_geometry limits;
    int ret;

    if (!ops || !ops->begin_update || !ops->end_update ||
        !ops->publish_table || !ops->invalidate)
        return -EINVAL;
    ret = s2_decode_limits(ops, &limits);
    if (ret)
        return ret;

    mutex_lock(&s2_runtime_lock);
    if (s2_runtime_ready || s2_contexts) {
        ret = -EBUSY;
    } else if (s2_generation == ~(u64)0) {
        ret = -EOVERFLOW;
    } else {
        s2_runtime = *ops;
        s2_limits = limits;
        ++s2_generation;
        memset(s2_vmids, 0, sizeof(s2_vmids));
        s2_vmids[0] = 1; /* Zero is not allocated to a VM. */
        s2_runtime_ready = true;
        ret = 0;
    }
    mutex_unlock(&s2_runtime_lock);
    return ret;
}

/* Mirror of register: refuses while any context still references the
 * runtime (-EBUSY), otherwise wipes the callback table so nothing can call
 * through a stale s2_runtime after this returns. */
int relm_stage2_unregister_runtime(void)
{
    int ret = 0;

    mutex_lock(&s2_runtime_lock);
    if (s2_contexts) {
        ret = -EBUSY;
    } else {
        s2_runtime_ready = false;
        memset(&s2_runtime, 0, sizeof(s2_runtime));
    }
    mutex_unlock(&s2_runtime_lock);
    return ret;
}


bool relm_stage2_check_support(void)
{
    bool supported;

    mutex_lock(&s2_runtime_lock);
    supported = s2_runtime_ready;
    mutex_unlock(&s2_runtime_lock);
    return supported;
}

int relm_stage2_choose_geometry(unsigned int requested_ipa_bits,
                               struct relm_stage2_geometry *geometry)
{
    struct relm_stage2_geometry chosen;
    int ret = 0;

    if (!geometry || requested_ipa_bits < S2_MIN_IPA_BITS ||
        requested_ipa_bits > S2_MAX_IPA_BITS)
        return -EINVAL;

    mutex_lock(&s2_runtime_lock);
    if (!s2_runtime_ready || requested_ipa_bits > s2_limits.ipa_bits) {
        ret = -EOPNOTSUPP;
    } else {
        chosen = s2_limits;
        chosen.ipa_bits = (u8)requested_ipa_bits;
        chosen.start_level = requested_ipa_bits <= 39 ? 1 : 0;
        *geometry = chosen;
    }
    mutex_unlock(&s2_runtime_lock);
    return ret;

}

static struct relm_stage2_table *s2_find_table(const struct relm_stage2 *stage2,
                                             phys_addr_t pa,
                                             unsigned int level)
{
    struct relm_stage2_table *table;

    for (table = stage2->owned_tables; table; table = table->next)
        if (table->pa == pa && table->level == level)
            return table;
    return NULL;
}

/*
 * Allocate one new, zeroed 4 KiB table page for `stage2` (which may well
 * be a throwaway `copy`, not the live context) at the given level, record
 * it in stage2->owned_tables, and hand the record back. The PA range
 * check after virt_to_phys() is not paranoia for its own sake: on a host
 * with less than full 48-bit physical memory this can never fail, but if
 * this backend ever runs on a host whose RAM genuinely sits above this
 * context's configured pa_bits, a table page living outside the
 * architecturally expressible range would silently corrupt the address
 * field of whatever descriptor points at it — caught here instead, as
 * -ERANGE, before that table is ever linked into a tree.
 */
static int s2_alloc_table(struct relm_stage2 *stage2, unsigned int level,
                          struct relm_stage2_table **out)
{
    struct relm_stage2_table *table;

    table = kzalloc(sizeof(*table), GFP_KERNEL);
    if (!table)
        return -ENOMEM;
    table->entries = (s2_entry_t *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
    if (!table->entries) {
        kfree(table);
        return -ENOMEM;
    }
    table->pa = virt_to_phys(table->entries);
    if (!s2_page_aligned(table->pa) ||
        !s2_range_fits(table->pa, S2_TABLE_SIZE, stage2->geometry.pa_bits)) {
        free_page((unsigned long)table->entries);
        kfree(table);
        return -ERANGE;
    }
    table->level = level;
    table->next = stage2->owned_tables;
    stage2->owned_tables = table;
    *out = table;
    return 0;
}

static void s2_free_tables(struct relm_stage2_table *table)
{
    while (table) {
        struct relm_stage2_table *next = table->next;

        free_page((unsigned long)table->entries);
        kfree(table);
        table = next;
    }
}

static int s2_alloc_vmid(unsigned int bits)
{
    unsigned int id;

    for (id = 1; id < (1U << bits); ++id) {
        u64 bit = 1ULL << (id % 64U);

        if (!(s2_vmids[id / 64U] & bit)) {
            s2_vmids[id / 64U] |= bit;
            return (int)id;
        }
    }
    return -ENOSPC; /* No rollover without a global invalidation protocol. */
}

/*
 * Build a brand-new, empty stage-2 context: encode VTCR up front (so a bad
 * geometry is rejected before anything is allocated), check the request
 * against this host's registered ceiling, allocate the context object
 * itself, initialize its update_lock, allocate just the root table (every
 * other table gets allocated lazily by s2_insert_leaf as mappings are
 * added), and claim one VMID from the global namespace.
 *
 * Failure unwinds in reverse allocation order via the goto labels below —
 * free_tables only runs if the VMID allocation step failed (so the root
 * table that was already allocated gets freed too), free_context runs if
 * even the root-table allocation failed (nothing to free but the struct
 * itself), and unlock covers every earlier failure. s2_runtime_lock is
 * held across the ENTIRE sequence (not just the VMID allocation) because
 * s2_limits/s2_runtime_ready/s2_generation are all read here and must not
 * change mid-construction.
 */
struct relm_stage2 *relm_stage2_context_create(
    const struct relm_stage2_geometry *geometry)
{
    struct relm_stage2 *stage2;
    struct relm_stage2_table *root;
    u64 vtcr;
    int ret;

    ret = relm_stage2_encode_vtcr(geometry, &vtcr);
    if (ret)
        return ERR_PTR(ret);

    mutex_lock(&s2_runtime_lock);
    if (!s2_runtime_ready || geometry->ipa_bits > s2_limits.ipa_bits ||
        geometry->pa_bits > s2_limits.pa_bits ||
        geometry->vmid_bits > s2_limits.vmid_bits) {
        ret = -EOPNOTSUPP;
        goto unlock;
    }
    stage2 = kzalloc(sizeof(*stage2), GFP_KERNEL);
    if (!stage2) {
        ret = -ENOMEM;
        goto unlock;
    }
    stage2->geometry = *geometry;
    stage2->vtcr_el2 = vtcr;
    mutex_init(&stage2->update_lock);
    ret = s2_alloc_table(stage2, geometry->start_level, &root);
    if (ret)
        goto free_context;
    ret = s2_alloc_vmid(geometry->vmid_bits);
    if (ret < 0)
        goto free_tables;

    stage2->root = root->entries;
    stage2->root_pa = root->pa;
    stage2->vmid = (u16)ret;
    stage2->vmid_generation = s2_generation;
    stage2->vmid_valid = true;
    ++s2_contexts; /* Pins callbacks until destruction has finished using them. */
    mutex_unlock(&s2_runtime_lock);

    /* An empty table also needs publication before its first hardware walk. */
    s2_runtime.publish_table(stage2->root, S2_TABLE_SIZE);
    return stage2;

free_tables:
    s2_free_tables(stage2->owned_tables);
free_context:
    kfree(stage2);
unlock:
    mutex_unlock(&s2_runtime_lock);
    return ERR_PTR(ret);
}

/*
 * First half of every mutating operation's four-step shape (see the file
 * banner at the top). Order matters: the runtime's own entry gate is
 * acquired FIRST, then this context's update_lock — establishing a single
 * global lock order (gate -> update_lock) that every caller follows, which
 * is what avoids a deadlock between two contexts' updates running on
 * different CPUs. Once both are held, this re-validates two things that
 * could have changed since the caller last looked at the context:
 *   - vmid_valid / vmid_generation: catches a context that was destroyed
 *     or whose VMID was invalidated by a runtime re-registration between
 *     the caller obtaining the pointer and getting here (-ESTALE).
 *   - failed, unless the caller explicitly allows operating on a failed
 *     context (allow_failed=true — used by destroy/invalidate, which need
 *     to be able to tear down or re-flush a context that a PREVIOUS
 *     mutation already marked failed).
 * Either check failing releases both locks/the gate again before
 * returning, so a failed s2_begin_update() never leaves anything held.
 */
static int s2_begin_update(struct relm_stage2 *stage2, bool allow_failed)
{
    int ret;

    if (IS_ERR_OR_NULL(stage2))
        return -EINVAL;
    /* The caller's context lifetime pins the runtime registration. */
    ret = s2_runtime.begin_update(stage2);
    if (ret)
        return ret < 0 ? ret : -EIO;
    mutex_lock(&stage2->update_lock);
    if (!stage2->vmid_valid || stage2->vmid_generation != s2_generation)
        ret = -ESTALE;
    else if (stage2->failed && !allow_failed)
        ret = -EIO;
    if (ret) {
        mutex_unlock(&stage2->update_lock);
        s2_runtime.end_update(stage2, false);
    }
    return ret;
}

/*
 * Second half of the four-step shape: release update_lock, then tell the
 * runtime whether guest entry should reopen for this context. "Runnable"
 * means exactly "still has a valid VMID AND wasn't just marked failed" —
 * computed fresh here rather than trusted from the caller, since several
 * different failure paths (a bad commit, a failed invalidate) can set
 * `failed` right before this runs.
 */
static void s2_end_update(struct relm_stage2 *stage2)
{
    bool runnable = stage2->vmid_valid && !stage2->failed;

    mutex_unlock(&stage2->update_lock);
    s2_runtime.end_update(stage2, runnable);
}



/*
 * Ask the runtime to flush every cached stage-1+stage-2 translation
 * tagged with this context's current (vtcr_el2, root_pa, vmid) — i.e. the
 * root/VMID combination that is ABOUT to stop being selected (either
 * because a new tree is about to be swapped in, or because the context is
 * being torn down). Must only ever be called with the entry gate already
 * held (via s2_begin_update), which is what guarantees no CPU can refill a
 * translation for this exact combination while the flush is happening —
 * without that gate, a CPU could walk and cache a translation again a
 * moment after this "finishes." On failure the context is marked `failed`
 * so it can never again be considered runnable by s2_end_update.
 */
static int s2_invalidate_locked(struct relm_stage2 *stage2)
{
    u64 vttbr;
    int ret;

    ret = relm_stage2_encode_vttbr(&stage2->geometry, stage2->root_pa,
                                  stage2->vmid, &vttbr);
    if (!ret)
        ret = s2_runtime.invalidate(stage2->vtcr_el2, vttbr);
    if (ret) {
        stage2->failed = true;
        return ret < 0 ? ret : -EIO;
    }
    return 0;
}


int relm_stage2_context_destroy(struct relm_stage2 *stage2)
{
    int ret;

    if (!stage2)
        return 0;
    ret = s2_begin_update(stage2, true);
    if (ret)
        return ret;
    ret = s2_invalidate_locked(stage2);
    if (ret) {
        s2_end_update(stage2);
        return ret; /* Caller still owns context, VMID, and every table. */
    }

    s2_free_tables(stage2->owned_tables);
    stage2->owned_tables = NULL;
    stage2->root = NULL;
    stage2->vmid_valid = false;
    s2_end_update(stage2); /* Still pins the provider; never reopens guest entry. */

    mutex_lock(&s2_runtime_lock);
    s2_vmids[stage2->vmid / 64U] &= ~(1ULL << (stage2->vmid % 64U));
    --s2_contexts;
    mutex_unlock(&s2_runtime_lock);
    kfree(stage2);
    return 0;
}

/* Byte size of whatever one descriptor at this level covers: L3=4 KiB,
 * L2=2 MiB, L1=1 GiB, L0=512 GiB — each level is 9 bits (512 entries)
 * wider than the next, on top of the 12-bit (4 KiB) page granule. */
static u64 s2_level_size(unsigned int level)
{
    return 1ULL << (12U + 9U * (3U - level));
}

/* Which of the 512 entries in THIS level's table does this IPA fall under —
 * i.e. bits [20:12] for L3, [29:21] for L2, [38:30] for L1, [47:39] for L0. */
static unsigned int s2_index(u64 ipa, unsigned int level)
{
    return (unsigned int)((ipa >> (12U + 9U * (3U - level))) & 0x1ff);
}

static int s2_validate_flags(u64 flags)
{
    u64 memory = flags & S2_MEMATTR_MASK;

    if (flags & ~S2_LEAF_FLAGS_MASK)
        return -EINVAL;
    if (!(flags & S2_ACCESS_READ) || (flags & S2_SH_MASK) == (1ULL << 8))
        return -EINVAL;
    if (memory != S2_MEMTYPE_NORMAL_WB && memory != S2_MEMTYPE_NORMAL_NC &&
        memory != S2_MEMTYPE_DEVICE_NGNRNE && memory != S2_MEMTYPE_DEVICE_NGNRE)
        return -EINVAL;
    if (memory != S2_MEMTYPE_NORMAL_WB && !(flags & S2_XN))
        return -EINVAL;
    return 0;
}



