#include <linux/errno.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/io.h>

#include <relm/vm.h>
#include <relm/vcpu.h>
#include <virtio/mmio.h>
#include <stage2.h>

static DEFINE_MUTEX(s2_runtime_lock);
static struct relm_stage2_runtime_ops s2_runtime;
static struct relm_stage2_geometry s2_limits;
static bool s2_runtime_ready;

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

/* Overflow-safe "[address, address+size) fits in a 2^bits-byte space?" —
 * written as size <= limit-address rather than address+size <= limit so a
 * huge address/size pair can't wrap past `limit` and look valid. */
static bool s2_range_fits(u64 address, u64 size, unsigned int bits)
{
    u64 limit = 1ULL << bits;

    return size && address < limit && size <= limit - address;
}

static bool s2_page_aligned(u64 value)
{
    return !(value & (S2_PAGE_SIZE_4KB - 1));
}

static u64 s2_level_size(unsigned int level)
{
    return 1ULL << (12U + 9U * (3U - level));
}

static unsigned int s2_index(u64 ipa, unsigned int level)
{
    return (unsigned int)((ipa >> (12U + 9U * (3U - level))) & 0x1ff);
}

/* geometry -> VTCR_EL2. T0SZ=64-ipa_bits; SL0 picks the start level; PS is
 * the output-width encoding; VS only needs setting for 16-bit VMIDs. */
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

    *vttbr = (u64)root_pa | ((u64)vmid << S2_VTTBR_VMID_SHIFT);
    return 0;
}

/*
 * Host ID registers -> the widest geometry this host can support. Each
 * field is a real architectural bitfield (Linux arch/arm64/tools/sysreg):
 * EL2[11:8] (1 or 2 = AArch64 EL2 present), TGran4_2[43:40] (0 defers to
 * the older TGran4[31:28] field; otherwise only 2/3 mean "supported"),
 * PARange[3:0] (an index into pa_widths[], not a bit count), VMIDBits[7:4]
 * (2 means 16-bit VMIDs, not literally 2 — hence the explicit translation
 * at the bottom rather than arithmetic on the raw field).
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

    /* This backend's descriptor format (non-LPA2) tops out at 48 bits even
     * if PARange reports wider — see S2_MAX_PA_BITS. */
    limits->pa_bits = pa_widths[pa] > 48 ? 48 : pa_widths[pa];
    limits->ipa_bits = limits->pa_bits;
    limits->start_level = limits->ipa_bits <= 39 ? 1 : 0;
    limits->vmid_bits = vmids == 2 ? 16 : 8;
    return 0;
}

/* One-time handshake: the EL2 runtime hands over its invalidate callback
 * and the host ID-register values describing every CPU it will run guests
 * on. x86 has no equivalent step at all (INVEPT needs no registration). */
int relm_stage2_register_runtime(const struct relm_stage2_runtime_ops *ops)
{
    struct relm_stage2_geometry limits;
    int ret;

    if (!ops || !ops->invalidate)
        return -EINVAL;
    ret = s2_decode_limits(ops, &limits);
    if (ret)
        return ret;

    mutex_lock(&s2_runtime_lock);
    if (s2_runtime_ready) {
        ret = -EBUSY;
    } else {
        s2_runtime = *ops;
        s2_limits = limits;
        s2_runtime_ready = true;
        ret = 0;
    }
    mutex_unlock(&s2_runtime_lock);
    return ret;
}

* One-time handshake: the EL2 runtime hands over its invalidate callback
 * and the host ID-register values describing every CPU it will run guests
 * on. x86 has no equivalent step at all (INVEPT needs no registration). */
int relm_stage2_register_runtime(const struct relm_stage2_runtime_ops *ops)
{
    struct relm_stage2_geometry limits;
    int ret;

    if (!ops || !ops->invalidate)
        return -EINVAL;
    ret = s2_decode_limits(ops, &limits);
    if (ret)
        return ret;

    mutex_lock(&s2_runtime_lock);
    if (s2_runtime_ready) {
        ret = -EBUSY;
    } else {
        s2_runtime = *ops;
        s2_limits = limits;
        s2_runtime_ready = true;
        ret = 0;
    }
    mutex_unlock(&s2_runtime_lock);
    return ret;
}

int relm_stage2_unregister_runtime(void)
{
    mutex_lock(&s2_runtime_lock);
    s2_runtime_ready = false;
    memset(&s2_runtime, 0, sizeof(s2_runtime));
    mutex_unlock(&s2_runtime_lock);
    return 0;
}

bool relm_stage2_check_support(void)
{
    bool supported;

    mutex_lock(&s2_runtime_lock);
    supported = s2_runtime_ready;
    mutex_unlock(&s2_runtime_lock);
    return supported;
}

/* Narrow the host's registered ceiling down to exactly the IPA width one
 * VM asked for; pa_bits/vmid_bits always come straight from the host
 * ceiling. -EOPNOTSUPP (not -EINVAL) when the request is well-formed but
 * simply wider than this host supports. */
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


static void *s2_alloc_table(void)
{
    void *table = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);

    if (unlikely(!table))
        pr_err("RELM: stage2: failed to alloc table page\n");
    return table;
}

static void s2_free_table_tree(s2_entry_t *table, unsigned int level)
{
    unsigned int i;

    if (level == 3) {
        free_page((unsigned long)table);
        return;
    }
    for (i = 0; i < S2_ENTRIES_PER_TABLE; i++) {
        u64 entry = table[i];

        if ((entry & S2_DESC_TYPE_MASK) == S2_DESC_TABLE)
            s2_free_table_tree(phys_to_virt(entry & S2_TABLE_ADDR_MASK),
                               level + 1);
    }
    free_page((unsigned long)table);
}

/*
 * Build a brand-new, empty context: encode VTCR up front (rejects a bad
 * geometry before anything is allocated), check the request against this
 * host's registered ceiling, allocate the context struct and its root
 * table, and take the caller-supplied VMID as-is (bounds-checked — see the
 * struct relm_stage2 comment in stage2.h for why there's no allocator).
 */
struct relm_stage2 *relm_stage2_context_create(
    const struct relm_stage2_geometry *geometry, u16 vmid)
{
    struct relm_stage2 *stage2;
    u64 vtcr;
    int ret;

    ret = relm_stage2_encode_vtcr(geometry, &vtcr);
    if (ret)
        return ERR_PTR(ret);
    if (geometry->vmid_bits == 8 && vmid > 255)
        return ERR_PTR(-ERANGE);

    mutex_lock(&s2_runtime_lock);
    if (!s2_runtime_ready || geometry->ipa_bits > s2_limits.ipa_bits ||
        geometry->pa_bits > s2_limits.pa_bits ||
        geometry->vmid_bits > s2_limits.vmid_bits) {
        mutex_unlock(&s2_runtime_lock);
        return ERR_PTR(-EOPNOTSUPP);
    }
    mutex_unlock(&s2_runtime_lock);

    stage2 = kzalloc(sizeof(*stage2), GFP_KERNEL);
    if (!stage2)
        return ERR_PTR(-ENOMEM);

    stage2->root = s2_alloc_table();
    if (!stage2->root) {
        kfree(stage2);
        return ERR_PTR(-ENOMEM);
    }
    stage2->root_pa = virt_to_phys(stage2->root);
    stage2->geometry = *geometry;
    stage2->vtcr_el2 = vtcr;
    stage2->vmid = vmid;
    mutex_init(&stage2->lock);

    return stage2;
}

static int s2_invalidate(struct relm_stage2 *stage2)
{
    u64 vttbr;
    int ret;

    ret = relm_stage2_encode_vttbr(&stage2->geometry, stage2->root_pa,
                                   stage2->vmid, &vttbr);
    if (ret)
        return ret;
    return s2_runtime.invalidate(stage2->vtcr_el2, vttbr);
}

int relm_stage2_invalidate_context(struct relm_stage2 *stage2)
{
    int ret;

    if (IS_ERR_OR_NULL(stage2))
        return -EINVAL;
    mutex_lock(&stage2->lock);
    ret = s2_invalidate(stage2);
    mutex_unlock(&stage2->lock);
    return ret;
}

int relm_stage2_context_destroy(struct relm_stage2 *stage2)
{
    int ret;

    if (!stage2)
        return 0;
    mutex_lock(&stage2->lock);
    ret = s2_invalidate(stage2);
    mutex_unlock(&stage2->lock);
    if (ret)
        return ret;

    s2_free_table_tree(stage2->root, stage2->geometry.start_level);
    kfree(stage2);
    return 0;
}

/*
 * get-or-create the child table a non-leaf entry should point at — the
 * ARM analogue of relm_ept_get_or_create_table(). If the slot is already
 * a table, just resolve its PA back to a host VA; otherwise allocate a
 * fresh table and wire the parent entry to it. No AF/reserved-bit
 * validation here, same trust level ept.c's version has.
 */
static s2_entry_t *s2_get_or_create(s2_entry_t *entry)
{
    void *child;

    if ((*entry & S2_DESC_TYPE_MASK) == S2_DESC_TABLE)
        return phys_to_virt(*entry & S2_TABLE_ADDR_MASK);

    child = s2_alloc_table();
    if (!child)
        return NULL;
    *entry = virt_to_phys(child) | S2_DESC_TABLE;
    return child;
}

/* Descend from the root to `leaf_level`, allocating intermediate tables as
 * needed, then write the leaf. An existing mapping is overwritten with a
 * warning — the same behaviour relm_ept_map_page has, not an error. */
static int s2_map_one(struct relm_stage2 *stage2, u64 ipa, phys_addr_t pa,
                      u64 flags, unsigned int leaf_level)
{
    s2_entry_t *table = stage2->root;
    s2_entry_t *leaf;
    unsigned int level;

    for (level = stage2->geometry.start_level; level < leaf_level; level++) {
        table = s2_get_or_create(&table[s2_index(ipa, level)]);
        if (!table)
            return -ENOMEM;
    }

    leaf = &table[s2_index(ipa, leaf_level)];
    if (*leaf & S2_DESC_VALID) {
        pr_warn("RELM: stage2 IPA 0x%llx already mapped, overwriting\n", ipa);
    } else {
        if (leaf_level == 3)
            stage2->stats.pages_4kb++;
        else
            stage2->stats.pages_2mb++;
        stage2->stats.total_mapped += s2_level_size(leaf_level);
    }
    *leaf = (pa & S2_PAGE_ADDR_MASK) | flags | S2_AF |
            (leaf_level == 3 ? S2_DESC_PAGE : S2_DESC_BLOCK);
    return 0;
}

static int s2_map(struct relm_stage2 *stage2, u64 ipa, phys_addr_t pa,
                  u64 flags, unsigned int leaf_level)
{
    int ret;

    if (IS_ERR_OR_NULL(stage2) || !s2_page_aligned(ipa) || !s2_page_aligned(pa))
        return -EINVAL;
    if (!(flags & S2_ACCESS_READ))
        return -EINVAL;
    if (!s2_range_fits(ipa, s2_level_size(leaf_level), stage2->geometry.ipa_bits) ||
        !s2_range_fits(pa, s2_level_size(leaf_level), stage2->geometry.pa_bits))
        return -ERANGE;

    mutex_lock(&stage2->lock);
    ret = s2_map_one(stage2, ipa, pa, flags, leaf_level);
    mutex_unlock(&stage2->lock);
    return ret;
}

int relm_stage2_map_page(struct relm_stage2 *stage2, u64 ipa,
                        phys_addr_t pa, u64 flags)
{
    return s2_map(stage2, ipa, pa, flags, 3);
}

int relm_stage2_map_huge_page(struct relm_stage2 *stage2, u64 ipa,
                             phys_addr_t pa, u64 flags)
{
    if ((ipa | (u64)pa) & (S2_PAGE_SIZE_2MB - 1))
        return -EINVAL;
    return s2_map(stage2, ipa, pa, flags, 2);
}

/* Page-by-page loop, same shape as relm_ept_map_range. */
int relm_stage2_map_range(struct relm_stage2 *stage2, u64 ipa,
                         phys_addr_t pa, u64 size, u64 flags)
{
    u64 offset;
    int ret;

    if (!size || !s2_page_aligned(size))
        return -EINVAL;
    for (offset = 0; offset < size; offset += S2_PAGE_SIZE_4KB) {
        ret = relm_stage2_map_page(stage2, ipa + offset, pa + offset, flags);
        if (ret)
            return ret;
    }
    return 0;
}

/* Does [ipa, ipa+size) touch any reserved MMIO window on this VM? Walks
 * the live registry via the same count/at iterators ept_range_overlaps_mmio
 * (ept.c) uses — no local snapshot kept. */
static bool s2_range_overlaps_mmio(struct relm_vm *vm, u64 ipa, u64 size)
{
    unsigned int i, count;
    struct relm_mmio_region region;
    u64 end = ipa + size;

    if (!vm)
        return false;
    count = relm_vm_mmio_region_count(vm);
    for (i = 0; i < count; i++) {
        if (relm_vm_mmio_region_at(vm, i, &region) < 0)
            continue;
        if (ipa < region.gpa_start + region.size && region.gpa_start < end)
            return true;
    }
    return false;
}

/* MMIO-aware 4 KiB-only guest RAM mapper: same page-by-page loop as
 * relm_stage2_map_range, plus skipping any page that overlaps a reserved
 * MMIO window — left unmapped so it still traps as a stage-2 abort into
 * the (future) MMIO emulation path instead of being RAM-backed. */
int relm_stage2_map_guest_ram_4kb(struct relm_stage2 *stage2,
                                 struct relm_vm *vm, u64 ipa,
                                 phys_addr_t pa, u64 size, u64 flags)
{
    u64 offset;
    int ret;

    if (!vm || !size || !s2_page_aligned(size))
        return -EINVAL;
    for (offset = 0; offset < size; offset += S2_PAGE_SIZE_4KB) {
        if (s2_range_overlaps_mmio(vm, ipa + offset, S2_PAGE_SIZE_4KB))
            continue;
        ret = relm_stage2_map_page(stage2, ipa + offset, pa + offset, flags);
        if (ret)
            return ret;
    }
    return 0;
}


/*
 * Guest RAM mapper preferring 2 MiB blocks: walks in 2 MiB strides, using
 * one huge leaf when the stride is aligned, whole, and MMIO-free, else
 * falling back to relm_stage2_map_guest_ram_4kb for just that stride —
 * same degrade-to-page-granularity-at-an-MMIO-boundary behaviour as
 * relm_ept_map_guest_ram_huge.
 */
int relm_stage2_map_guest_ram_huge(struct relm_stage2 *stage2,
                                  struct relm_vm *vm, u64 ipa,
                                  phys_addr_t pa, u64 size, u64 flags)
{
    u64 end, offset = 0;
    int ret;

    if (!vm || !size || !s2_page_aligned(size))
        return -EINVAL;
    end = ipa + size;

    while (ipa + offset < end) {
        u64 address = ipa + offset;
        phys_addr_t physical = pa + offset;
        u64 remaining = end - address;

        if (remaining >= S2_PAGE_SIZE_2MB &&
            !((address | (u64)physical) & (S2_PAGE_SIZE_2MB - 1)) &&
            !s2_range_overlaps_mmio(vm, address, S2_PAGE_SIZE_2MB)) {
            ret = relm_stage2_map_huge_page(stage2, address, physical, flags);
            if (ret)
                return ret;
            offset += S2_PAGE_SIZE_2MB;
        } else {
            u64 to_boundary = S2_PAGE_SIZE_2MB - (address & (S2_PAGE_SIZE_2MB - 1));
            u64 chunk = min(remaining, to_boundary);

            ret = relm_stage2_map_guest_ram_4kb(stage2, vm, address, physical,
                                               chunk, flags);
            if (ret)
                return ret;
            offset += chunk;
        }
    }
    return 0;
}

/*
 * Walk down to whatever leaf covers `ipa`, following table descriptors
 * via phys_to_virt() directly — no allocation ledger to consult, same
 * trust level as ept.c's own walkers. Fills *out_pa/*out_base/*out_size
 * from whichever leaf (page or block) was found; -ENOENT on a hole.
 */
static int s2_walk(struct relm_stage2 *stage2, u64 ipa, u64 *out_pa,
                   u64 *out_base, u64 *out_size)
{
    s2_entry_t *table = stage2->root;
    unsigned int level;

    for (level = stage2->geometry.start_level; level < S2_LEVELS; level++) {
        s2_entry_t entry = table[s2_index(ipa, level)];
        u64 type = entry & S2_DESC_TYPE_MASK;

        if (!entry)
            return -ENOENT;
        if (level < 3 && type == S2_DESC_TABLE) {
            table = phys_to_virt(entry & S2_TABLE_ADDR_MASK);
            continue;
        }
        /* Reaching here at L0 means the entry was NOT a table descriptor
         * (that case already continued the loop above) — L0 can never be
         * a leaf itself, it must always point to L1, so this is a
         * corrupt entry, not a giant 512 GiB mapping. Otherwise: block at
         * L1/L2, page at L3. */
        if (level == 0 ||
            (level == 3 && type != S2_DESC_PAGE) ||
            (level < 3 && type != S2_DESC_BLOCK))
            return -EINVAL;

        *out_size = s2_level_size(level);
        *out_base = ipa & ~(*out_size - 1);
        *out_pa = entry & S2_PAGE_ADDR_MASK & ~(*out_size - 1);
        return 0;
    }
    return -ENOENT;
}

int relm_stage2_get_mapping(struct relm_stage2 *stage2, u64 ipa, phys_addr_t *pa)
{
    u64 leaf_pa, base, size;
    int ret;

    if (IS_ERR_OR_NULL(stage2) || !pa)
        return -EINVAL;
    mutex_lock(&stage2->lock);
    ret = s2_walk(stage2, ipa, &leaf_pa, &base, &size);
    if (!ret)
        *pa = leaf_pa + (ipa - base);
    mutex_unlock(&stage2->lock);
    return ret;
}

/*
 * Remove whatever single leaf covers `ipa`. `ipa` must be that leaf's own
 * aligned base (checked via s2_walk's *out_base) — unlike a 4 KiB page, a
 * 2 MiB block can't be partially cleared, so a request that lands inside
 * one without naming its exact start is rejected (-EINVAL) rather than
 * zeroing the whole block out from under the rest of its range.
 */
int relm_stage2_unmap_page(struct relm_stage2 *stage2, u64 ipa)
{
    u64 leaf_pa, base, size;
    s2_entry_t *table;
    unsigned int level;
    int ret;

    if (IS_ERR_OR_NULL(stage2))
        return -EINVAL;

    mutex_lock(&stage2->lock);
    ret = s2_walk(stage2, ipa, &leaf_pa, &base, &size);
    if (ret == -ENOENT) {
        mutex_unlock(&stage2->lock);
        return 0;
    }
    if (ret) {
        mutex_unlock(&stage2->lock);
        return ret;
    }
    if (base != ipa) {
        mutex_unlock(&stage2->lock);
        return -EINVAL;
    }

    table = stage2->root;
    for (level = stage2->geometry.start_level; s2_level_size(level) != size; level++)
        table = phys_to_virt(table[s2_index(ipa, level)] & S2_TABLE_ADDR_MASK);

    table[s2_index(ipa, level)] = S2_DESC_INVALID;
    if (level == 3)
        stage2->stats.pages_4kb--;
    else
        stage2->stats.pages_2mb--;
    stage2->stats.total_mapped -= size;
    mutex_unlock(&stage2->lock);
    return 0;
}

/* Page-by-page/block-by-block loop, same shape as relm_stage2_map_range. */
int relm_stage2_unmap_range(struct relm_stage2 *stage2, u64 ipa, u64 size)
{
    u64 end = ipa + size;
    int ret;

    if (!size || !s2_page_aligned(ipa) || !s2_page_aligned(size))
        return -EINVAL;
    while (ipa < end) {
        ret = relm_stage2_unmap_page(stage2, ipa);
        if (ret)
            return ret;
        ipa += S2_PAGE_SIZE_4KB;
    }
    return 0;
}


/*
 * VM-level entry point: pick the default IPA width, validate every
 * already-registered RAM region fits it, create the context using
 * vm->vm_id directly as the VMID (see the struct relm_stage2 comment in
 * stage2.h), and publish it.
 */
int relm_setup_stage2(struct relm_vm *vm)
{
    struct relm_stage2_geometry geometry;
    struct relm_stage2 *stage2;
    struct guest_mem_region *region;
    int ret;

    if (!vm)
        return -EINVAL;
    mutex_lock(&vm->arch.config_lock);
    if (vm->arch.stage2) {
        ret = -EEXIST;
        goto unlock;
    }
    ret = relm_stage2_choose_geometry(S2_DEFAULT_IPA_BITS, &geometry);
    if (ret)
        goto unlock;
    for (region = vm->memory.mem_regions; region; region = region->next) {
        if (!s2_range_fits(region->gpa_start, region->size, geometry.ipa_bits)) {
            ret = -ERANGE;
            goto unlock;
        }
    }
    stage2 = relm_stage2_context_create(&geometry, (u16)vm->vm_id);
    if (IS_ERR(stage2)) {
        ret = (int)PTR_ERR(stage2);
        goto unlock;
    }
    vm->arch.stage2 = stage2;
    ret = 0;
unlock:
    mutex_unlock(&vm->arch.config_lock);
    return ret;
}


int relm_stage2_handle_fault(struct vcpu *vcpu)
{
    const struct guest_regs *regs;
    u64 ec, type;

    if (!vcpu || !vcpu->vm || !vcpu->vm->arch.stage2 ||
        vcpu->arch.exit_kind != RELM_ARM64_EXIT_SYNC)
        return -EINVAL;
    regs = &vcpu->arch.regs;
    ec = regs->esr_el2 & S2_ESR_EC_MASK;
    if (ec != S2_ESR_EC_IABT_LOWER && ec != S2_ESR_EC_DABT_LOWER)
        return -EINVAL;
    type = regs->esr_el2 & S2_ESR_FSC_TYPE_MASK;
    pr_err("RELM: stage2 %s abort: PC=%#llx ESR=%#llx FAR=%#llx HPFAR=%#llx\n",
           ec == S2_ESR_EC_IABT_LOWER ? "instruction" : "data",
           regs->pc, regs->esr_el2, regs->far_el2, regs->hpfar_el2);
    if (regs->esr_el2 & S2_ESR_S1PTW)
        pr_err("RELM: abort occurred during a guest stage-1 table walk\n");

    return type == S2_ESR_FSC_PERMISSION ? -EACCES : -EFAULT;
}

static void s2_dump_table(s2_entry_t *table, unsigned int level, u64 base)
{
    unsigned int i;

    for (i = 0; i < S2_ENTRIES_PER_TABLE; i++) {
        u64 entry = table[i];
        u64 address = base + i * s2_level_size(level);
        u64 type = entry & S2_DESC_TYPE_MASK;

        if (!entry)
            continue;
        if (level < 3 && type == S2_DESC_TABLE) {
            s2_dump_table(phys_to_virt(entry & S2_TABLE_ADDR_MASK),
                         level + 1, address);
        } else {
            pr_info("RELM: stage2 L%u IPA=%#llx PA=%#llx size=%#llx %c%c%c attr=%#llx\n",
                    level, address, entry & S2_PAGE_ADDR_MASK,
                    s2_level_size(level), entry & S2_ACCESS_READ ? 'r' : '-',
                    entry & S2_ACCESS_WRITE ? 'w' : '-',
                    entry & S2_XN ? '-' : 'x', entry & S2_MEMATTR_MASK);
        }
    }
}

void relm_stage2_dump_tables(struct relm_stage2 *stage2)
{
    if (IS_ERR_OR_NULL(stage2))
        return;
    mutex_lock(&stage2->lock);
    pr_info("RELM: stage2 root=%#llx VTCR=%#llx VMID=%u\n",
            (u64)stage2->root_pa, stage2->vtcr_el2, (unsigned int)stage2->vmid);
    pr_info("RELM: stage2 leaves: 4K=%llu 2M=%llu 1G=%llu bytes=%llu\n",
            stage2->stats.pages_4kb, stage2->stats.pages_2mb,
            stage2->stats.pages_1gb, stage2->stats.total_mapped);
    s2_dump_table(stage2->root, stage2->geometry.start_level, 0);
    mutex_unlock(&stage2->lock);
}



