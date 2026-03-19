/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * fc_cache_body.h - Include-template for per-variant cache implementations.
 *
 * Before including, define:
 *   FC_PREFIX       e.g. flow4       (token, not string)
 *   FC_PRESSURE     e.g. FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS
 *
 * The caller must also provide, before including this file:
 *   static inline int
 *   fc_<PREFIX>_cmp(const struct fc_<PREFIX>_key *a,
 *                    const struct fc_<PREFIX>_key *b);
 *   static inline union rix_hash_hash_u
 *   fc_<PREFIX>_hash_fn(const struct fc_<PREFIX>_key *key, uint32_t mask);
 *
 * All other names (types, hash-table helpers, API functions) are derived
 * from FC_PREFIX automatically.
 */

#ifndef FC_PREFIX
#error "FC_PREFIX must be defined before including fc_cache_body.h"
#endif

/* Token-pasting helpers */
#define _FC_CAT2(a, b) a##b
#define _FC_CAT(a, b)  _FC_CAT2(a, b)

/* fc_flow4_ht_xxx */
#define FC_HT(name)   _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _ht_##name))

/* fc_flow4_xxx (internal helpers) */
#define FC_INT(name)  _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _##name))

/* fc_flow4_cache_xxx (public API) */
#define FC_API(name)  _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _cache_##name))

/* Derive type names from FC_PREFIX */
#define FC_KEY_T       struct _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _key))
#define FC_RESULT_T    struct _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _result))
#define FC_ENTRY_T     struct _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _entry))
#define FC_CACHE_T     struct _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _cache))
#define FC_CONFIG_T    struct _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _config))
#define FC_STATS_T     struct _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _stats))
#define FC_FREE_HEAD_T struct _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _free_head))

/* Derive cmp / hash_fn names: fc_<PREFIX>_cmp, fc_<PREFIX>_hash_fn */
#define FC_CMP_FN      FC_INT(cmp)
#define FC_HASH_FN     FC_INT(hash_fn)

#ifndef FLOW_CACHE_LOOKUP_STEP_KEYS
#define FLOW_CACHE_LOOKUP_STEP_KEYS   16u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_STEPS
#define FLOW_CACHE_LOOKUP_AHEAD_STEPS 8u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_KEYS
#define FLOW_CACHE_LOOKUP_AHEAD_KEYS \
    (FLOW_CACHE_LOOKUP_STEP_KEYS * FLOW_CACHE_LOOKUP_AHEAD_STEPS)
#endif

enum {
    FC_RELIEF_STAGE_SLOTS = 4u
};

/*===========================================================================
 * AVX2 direct-bind for fingerprint scans
 *===========================================================================*/
#if defined(__AVX2__)
#undef _RIX_HASH_FIND_U32X16
#undef _RIX_HASH_FIND_U32X16_2
#define _RIX_HASH_FIND_U32X16(arr, val) \
    _rix_hash_find_u32x16_AVX2((arr), (val))
#define _RIX_HASH_FIND_U32X16_2(arr, val0, val1, mask0, mask1) \
    _rix_hash_find_u32x16_2_AVX2((arr), (val0), (val1), (mask0), (mask1))
#endif

RIX_HASH_GENERATE_STATIC_SLOT_EX(
    _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _ht)),
    _FC_CAT(fc_, _FC_CAT(FC_PREFIX, _entry)),
    key, cur_hash, slot,
    FC_CMP_FN, FC_HASH_FN)

#if defined(__AVX2__)
#undef _RIX_HASH_FIND_U32X16_2
#undef _RIX_HASH_FIND_U32X16
#endif

/*===========================================================================
 * Small inline helpers
 *===========================================================================*/
static inline void
FC_INT(prefetch_insert_hash)(const FC_CACHE_T *fc,
                              union rix_hash_hash_u h)
{
    unsigned bk0, bk1;
    uint32_t fp;
    unsigned mask = fc->ht_head.rhh_mask;

    _rix_hash_buckets(h, mask, &bk0, &bk1, &fp);
    (void)fp;
    _rix_hash_prefetch_bucket(fc->buckets + bk0);
    if (bk1 != bk0)
        _rix_hash_prefetch_bucket(fc->buckets + bk1);
}

static inline void
FC_INT(result_set_hit)(FC_RESULT_T *result, uint32_t entry_idx)
{
    result->entry_idx = entry_idx;
}

static inline void
FC_INT(result_set_miss)(FC_RESULT_T *result)
{
    result->entry_idx = 0u;
}

static inline void
FC_INT(result_set_filled)(FC_RESULT_T *result, uint32_t entry_idx)
{
    result->entry_idx = entry_idx;
}

/*===========================================================================
 * Timeout / threshold management
 *===========================================================================*/
static inline void
FC_INT(update_eff_timeout)(FC_CACHE_T *fc)
{
    unsigned live = fc->ht_head.rhh_nb;
    uint64_t max_tsc = fc->timeout_tsc;
    unsigned lo = fc->timeout_lo_entries;
    unsigned hi = fc->timeout_hi_entries;

    if (fc->total_slots == 0u || max_tsc == 0u) {
        fc->eff_timeout_tsc = max_tsc;
        return;
    }
    if (live <= lo) {
        fc->eff_timeout_tsc = max_tsc;
    } else if (live >= hi) {
        fc->eff_timeout_tsc = fc->timeout_min_tsc;
    } else {
        uint64_t span_entries = (uint64_t)(hi - lo);
        uint64_t used_entries = (uint64_t)(live - lo);
        uint64_t span_tsc = max_tsc - fc->timeout_min_tsc;
        uint64_t shrink = (used_entries * span_tsc) / span_entries;

        fc->eff_timeout_tsc = max_tsc - shrink;
    }
    if (fc->eff_timeout_tsc == 0u)
        fc->eff_timeout_tsc = 1u;
}

static inline unsigned
FC_INT(threshold64)(unsigned total_slots, unsigned parts64)
{
    return (unsigned)(((uint64_t)total_slots * parts64) >> 6);
}

static void
FC_INT(init_thresholds)(FC_CACHE_T *fc)
{
    fc->timeout_lo_entries = FC_INT(threshold64)(fc->total_slots, 38u);
    fc->timeout_hi_entries = FC_INT(threshold64)(fc->total_slots, 48u);
    fc->timeout_min_tsc = fc->timeout_tsc >> 3;
    if (fc->timeout_min_tsc == 0u)
        fc->timeout_min_tsc = 1u;
    fc->relief_mid_entries = FC_INT(threshold64)(fc->total_slots, 45u);
    fc->relief_hi_entries = FC_INT(threshold64)(fc->total_slots, 48u);
}

static inline unsigned
FC_INT(relief_empty_slots)(const FC_CACHE_T *fc)
{
    unsigned live = fc->ht_head.rhh_nb;
    unsigned empty_slots = fc->pressure_empty_slots;

    /*
     * Start local relief earlier as global fill rises:
     *   default : 15/16 occupied (empty <= 1)
     *   >= ~70% : 14/16 occupied (empty <= 2)
     *   >= 75%  : 13/16 occupied (empty <= 3)
     */
    if (live >= fc->relief_hi_entries) {
        if (empty_slots < 3u)
            empty_slots = 3u;
    } else if (live >= fc->relief_mid_entries) {
        if (empty_slots < 2u)
            empty_slots = 2u;
    }
    return empty_slots;
}

/*===========================================================================
 * Free-list management
 *===========================================================================*/
static inline FC_ENTRY_T *
FC_INT(alloc_entry)(FC_CACHE_T *fc)
{
    FC_ENTRY_T *entry = RIX_SLIST_FIRST(&fc->free_head, fc->pool);

    if (entry != NULL)
        RIX_SLIST_REMOVE_HEAD(&fc->free_head, fc->pool, free_link);
    return entry;
}

static inline void
FC_INT(free_entry)(FC_CACHE_T *fc, FC_ENTRY_T *entry)
{
    entry->last_ts = 0u;
    RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
}

/*===========================================================================
 * Bucket scanning / reclaim
 *===========================================================================*/
static unsigned
FC_INT(scan_bucket_slots)(FC_CACHE_T *fc,
                           unsigned bk_idx,
                           uint64_t expire_before,
                           unsigned *expired_slots,
                           int *oldest_slot)
{
    struct rix_hash_bucket_s *bucket = fc->buckets + bk_idx;
    uint64_t oldest_ts = UINT64_MAX;
    FC_ENTRY_T *entries[RIX_HASH_BUCKET_ENTRY_SZ];
    unsigned slots[RIX_HASH_BUCKET_ENTRY_SZ];
    unsigned cur_base = 0u;
    unsigned cur_count;
    unsigned used = 0u;
    unsigned expired_count = 0u;
    unsigned next_base = 0u;
    unsigned next_count = 0u;
    int dummy_oldest_slot = -1;
    int *oldest_slotp = (oldest_slot != NULL) ? oldest_slot : &dummy_oldest_slot;

    /*
     * Local relief already has the target bucket on-cache.  Prefetch entry
     * heads in small waves, but first compact the occupied slots so sparse
     * buckets do not waste prefetch lanes on RIX_NIL entries.
     */
    for (unsigned slot = 0; slot < RIX_HASH_BUCKET_ENTRY_SZ; slot++) {
        unsigned idx = bucket->idx[slot];
        FC_ENTRY_T *entry;

        if (idx == (unsigned)RIX_NIL)
            continue;
        entry = FC_HT(hptr)(fc->pool, idx);
        slots[used] = slot;
        entries[used] = entry;
        used++;
    }
    *oldest_slotp = -1;
    if (used == 0u)
        return 0u;

    cur_count = (used < FC_RELIEF_STAGE_SLOTS) ? used : FC_RELIEF_STAGE_SLOTS;
    while (next_count < cur_count) {
        __builtin_prefetch(entries[next_count], 0, 3);
        next_count++;
    }
    next_base = cur_count;

    while (cur_count != 0u) {
        next_count = 0u;
        while (next_count < FC_RELIEF_STAGE_SLOTS && next_base < used) {
            __builtin_prefetch(entries[next_base], 0, 3);
            next_count++;
            next_base++;
        }
        for (unsigned s = 0; s < cur_count; s++) {
            unsigned idx = cur_base + s;
            unsigned slot = slots[idx];
            FC_ENTRY_T *entry = entries[idx];

            if (entry->last_ts >= expire_before)
                continue;
            expired_slots[expired_count] = slot;
            expired_count++;
            if (entry->last_ts < oldest_ts) {
                oldest_ts = entry->last_ts;
                *oldest_slotp = (int)slot;
            }
        }
        cur_base += cur_count;
        cur_count = next_count;
    }

    return expired_count;
}

static int
FC_INT(reclaim_bucket)(FC_CACHE_T *fc,
                        unsigned bk_idx,
                        uint64_t expire_before)
{
    FC_ENTRY_T *victim;
    unsigned removed_idx;
    unsigned expired_slots[RIX_HASH_BUCKET_ENTRY_SZ];
    unsigned expired_count;
    int victim_slot;

    expired_count = FC_INT(scan_bucket_slots)(fc, bk_idx, expire_before,
                                               expired_slots, &victim_slot);
    if (expired_count == 0u)
        return 0;
    removed_idx = FC_HT(remove_at)(&fc->ht_head, fc->buckets,
                                    bk_idx, (unsigned)victim_slot);
    RIX_ASSERT(removed_idx != (unsigned)RIX_NIL);
    victim = FC_HT(hptr)(fc->pool, removed_idx);
    RIX_ASSERT(victim != NULL);
    FC_INT(free_entry)(fc, victim);
    return 1;
}

static unsigned
FC_INT(reclaim_bucket_all)(FC_CACHE_T *fc,
                            unsigned bk_idx,
                            uint64_t expire_before)
{
    unsigned expired_slots[RIX_HASH_BUCKET_ENTRY_SZ];
    unsigned evicted;

    evicted = FC_INT(scan_bucket_slots)(fc, bk_idx, expire_before,
                                         expired_slots, NULL);
    for (unsigned i = 0; i < evicted; i++) {
        FC_ENTRY_T *victim;
        unsigned removed_idx;

        removed_idx = FC_HT(remove_at)(&fc->ht_head, fc->buckets,
                                        bk_idx, expired_slots[i]);
        RIX_ASSERT(removed_idx != (unsigned)RIX_NIL);
        victim = FC_HT(hptr)(fc->pool, removed_idx);
        RIX_ASSERT(victim != NULL);
        FC_INT(free_entry)(fc, victim);
    }
    return evicted;
}

/*===========================================================================
 * Maintenance
 *===========================================================================*/
static unsigned
FC_INT(maintain_grouped)(FC_CACHE_T *fc,
                             unsigned start_bk,
                             unsigned bucket_count,
                             uint64_t now_tsc)
{
    unsigned evicted = 0u;
    unsigned cur_bk;
    unsigned mask;
    unsigned next_bk;
    uint64_t expire_before;

    RIX_ASSERT(fc->nb_bk != 0u);
    mask = fc->ht_head.rhh_mask;
    expire_before = (now_tsc > fc->eff_timeout_tsc) ?
        (now_tsc - fc->eff_timeout_tsc) : 0u;
    next_bk = start_bk & mask;
    _rix_hash_prefetch_bucket_idx(&fc->buckets[next_bk]);
    while (bucket_count-- != 0u) {
        unsigned reclaimed;

        cur_bk = next_bk;
        next_bk = (next_bk + 1u) & mask;
        _rix_hash_prefetch_bucket_idx(&fc->buckets[next_bk]);
        fc->stats.maint_bucket_checks++;
        reclaimed = FC_INT(reclaim_bucket_all)(fc, cur_bk, expire_before);
        fc->stats.maint_evictions += reclaimed;
        evicted += reclaimed;
    }
    return evicted;
}

/*===========================================================================
 * Insert relief
 *===========================================================================*/
static void
FC_INT(insert_relief_hashed)(FC_CACHE_T *fc,
                              union rix_hash_hash_u h,
                              uint64_t now_tsc)
{
    unsigned bk0, bk1;
    uint32_t fp;
    uint32_t hits_fp;
    uint32_t hits_zero;
    uint64_t expire_before;
    unsigned pressure_empty_slots;

    if (fc->total_slots == 0u)
        return;

    fc->stats.relief_calls++;
    FC_INT(update_eff_timeout)(fc);
    expire_before = (now_tsc > fc->eff_timeout_tsc) ?
        (now_tsc - fc->eff_timeout_tsc) : 0u;
    _rix_hash_buckets(h, fc->ht_head.rhh_mask, &bk0, &bk1, &fp);
    pressure_empty_slots = FC_INT(relief_empty_slots)(fc);
    fc->stats.relief_bucket_checks++;
    rix_hash_arch->find_u32x16_2(fc->buckets[bk0].hash, fp, 0u,
                                 &hits_fp, &hits_zero);
    (void)hits_fp;
    if ((unsigned)__builtin_popcount(hits_zero) <= pressure_empty_slots &&
        FC_INT(reclaim_bucket)(fc, bk0, expire_before)) {
        fc->stats.relief_evictions++;
        fc->stats.relief_bk0_evictions++;
        return;
    }
    if (bk1 != bk0) {
        fc->stats.relief_bucket_checks++;
        rix_hash_arch->find_u32x16_2(fc->buckets[bk1].hash, fp, 0u,
                                     &hits_fp, &hits_zero);
        if ((unsigned)__builtin_popcount(hits_zero) <= pressure_empty_slots &&
            FC_INT(reclaim_bucket)(fc, bk1, expire_before)) {
            fc->stats.relief_evictions++;
            fc->stats.relief_bk1_evictions++;
        }
    }
}

/*===========================================================================
 * Public API: init / flush / nb_entries
 *===========================================================================*/
void
FC_API(init)(FC_CACHE_T *fc,
              struct rix_hash_bucket_s *buckets,
              unsigned nb_bk,
              FC_ENTRY_T *pool,
              unsigned max_entries,
              const FC_CONFIG_T *cfg)
{
    FC_CONFIG_T defcfg = {
        .timeout_tsc = UINT64_C(1000000),
        .pressure_empty_slots = FC_PRESSURE
    };

    if (cfg == NULL)
        cfg = &defcfg;

    memset(fc, 0, sizeof(*fc));
    memset(buckets, 0, (size_t)nb_bk * sizeof(*buckets));
    memset(pool, 0, (size_t)max_entries * sizeof(*pool));
    fc->buckets = buckets;
    fc->pool = pool;
    fc->nb_bk = nb_bk;
    fc->max_entries = max_entries;
    fc->total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    fc->timeout_tsc = cfg->timeout_tsc;
    fc->eff_timeout_tsc = cfg->timeout_tsc ? cfg->timeout_tsc : 1u;
    fc->pressure_empty_slots = cfg->pressure_empty_slots ?
        cfg->pressure_empty_slots : FC_PRESSURE;
    FC_INT(init_thresholds)(fc);

    RIX_SLIST_INIT(&fc->free_head);
    FC_HT(init)(&fc->ht_head, nb_bk);
    for (unsigned i = 0; i < max_entries; i++)
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, &fc->pool[i], free_link);
}

void
FC_API(flush)(FC_CACHE_T *fc)
{
    memset(fc->buckets, 0, (size_t)fc->nb_bk * sizeof(*fc->buckets));
    RIX_SLIST_INIT(&fc->free_head);
    FC_HT(init)(&fc->ht_head, fc->nb_bk);
    for (unsigned i = 0; i < fc->max_entries; i++) {
        fc->pool[i].last_ts = 0u;
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, &fc->pool[i], free_link);
    }
}

unsigned
FC_API(nb_entries)(const FC_CACHE_T *fc)
{
    return fc->ht_head.rhh_nb;
}

/*===========================================================================
 * Public API: lookup_batch
 *===========================================================================*/
unsigned
FC_API(lookup_batch)(FC_CACHE_T *fc,
                      const FC_KEY_T *keys,
                      unsigned nb_keys,
                      uint64_t now,
                      FC_RESULT_T *results,
                      uint16_t *miss_idx)
{
    struct rix_hash_find_ctx_s ctx[nb_keys];
    unsigned miss_count = 0u;
    uint64_t hit_count = 0u;
    const unsigned ahead_keys = FLOW_CACHE_LOOKUP_AHEAD_KEYS;
    const unsigned step_keys = FLOW_CACHE_LOOKUP_STEP_KEYS;
    const unsigned total = nb_keys + 3u * ahead_keys;

    for (unsigned i = 0; i < total; i += step_keys) {
        if (i < nb_keys) {
            unsigned n = (i + step_keys <= nb_keys) ? step_keys : (nb_keys - i);

            for (unsigned j = 0; j < n; j++) {
                FC_HT(hash_key)(&ctx[i + j], &fc->ht_head,
                                 fc->buckets, &keys[i + j]);
            }
        }

        if (i >= ahead_keys && i - ahead_keys < nb_keys) {
            unsigned base = i - ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++)
                FC_HT(scan_bk)(&ctx[base + j], &fc->ht_head, fc->buckets);
        }

        if (i >= 2u * ahead_keys && i - 2u * ahead_keys < nb_keys) {
            unsigned base = i - 2u * ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++)
                FC_HT(prefetch_node)(&ctx[base + j], fc->pool);
        }

        if (i >= 3u * ahead_keys && i - 3u * ahead_keys < nb_keys) {
            unsigned base = i - 3u * ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++) {
                unsigned idx = base + j;
                FC_ENTRY_T *entry;

                entry = FC_HT(cmp_key)(&ctx[idx], fc->pool);

                if (entry == NULL) {
                    FC_INT(result_set_miss)(&results[idx]);
                    if (miss_idx != NULL)
                        miss_idx[miss_count] = (uint16_t)idx;
                    miss_count++;
                    continue;
                }
                entry->last_ts = now;
                FC_INT(result_set_hit)(&results[idx],
                                        RIX_IDX_FROM_PTR(fc->pool, entry));
                hit_count++;
            }
        }
    }

    fc->stats.lookups += nb_keys;
    fc->stats.hits += hit_count;
    fc->stats.misses += nb_keys - hit_count;
    return miss_count;
}

/*===========================================================================
 * Public API: fill_miss_batch
 *===========================================================================*/
unsigned
FC_API(fill_miss_batch)(FC_CACHE_T *fc,
                         const FC_KEY_T *keys,
                         const uint16_t *miss_idx,
                         unsigned miss_count,
                         uint64_t now,
                         FC_RESULT_T *results)
{
    enum { FC_FILL_PLAN_KEYS = 64 };
    unsigned inserted = 0u;

    for (unsigned base = 0; base < miss_count; base += FC_FILL_PLAN_KEYS) {
        union rix_hash_hash_u hashes[FC_FILL_PLAN_KEYS];
        unsigned n = miss_count - base;

        if (n > FC_FILL_PLAN_KEYS)
            n = FC_FILL_PLAN_KEYS;

        for (unsigned i = 0; i < n; i++) {
            unsigned key_idx = miss_idx[base + i];

            hashes[i] = FC_HASH_FN(&keys[key_idx], fc->ht_head.rhh_mask);
            FC_INT(prefetch_insert_hash)(fc, hashes[i]);
        }

        for (unsigned i = 0; i < n; i++) {
            unsigned key_idx = miss_idx[base + i];
            FC_ENTRY_T *entry;
            FC_ENTRY_T *ret;

            FC_INT(insert_relief_hashed)(fc, hashes[i], now);
            entry = FC_INT(alloc_entry)(fc);
            if (entry == NULL) {
                fc->stats.fill_full++;
                FC_INT(result_set_miss)(&results[key_idx]);
                continue;
            }

            entry->key = keys[key_idx];
            entry->last_ts = now;
            ret = FC_HT(insert_hashed)(&fc->ht_head, fc->buckets,
                                        fc->pool, entry, hashes[i]);
            if (ret == NULL) {
                fc->stats.fills++;
                FC_INT(result_set_filled)(&results[key_idx],
                                           RIX_IDX_FROM_PTR(fc->pool, entry));
                inserted++;
                continue;
            }
            FC_INT(free_entry)(fc, entry);
            if (ret != entry) {
                ret->last_ts = now;
                FC_INT(result_set_filled)(&results[key_idx],
                                           RIX_IDX_FROM_PTR(fc->pool, ret));
                continue;
            }
            fc->stats.fill_full++;
            FC_INT(result_set_miss)(&results[key_idx]);
            continue;
        }
    }
    return inserted;
}

/*===========================================================================
 * Public API: maintain / remove_idx / stats
 *===========================================================================*/
unsigned
FC_API(maintain)(FC_CACHE_T *fc,
                  unsigned start_bk,
                  unsigned bucket_count,
                  uint64_t now)
{
    fc->stats.maint_calls++;
    FC_INT(update_eff_timeout)(fc);
    return FC_INT(maintain_grouped)(fc, start_bk, bucket_count, now);
}

int
FC_API(remove_idx)(FC_CACHE_T *fc, uint32_t entry_idx)
{
    FC_ENTRY_T *entry;

    if (entry_idx == 0u || entry_idx > fc->max_entries)
        return 0;
    entry = RIX_PTR_FROM_IDX(fc->pool, entry_idx);
    if (entry == NULL)
        return 0;
    if (FC_HT(remove)(&fc->ht_head, fc->buckets, fc->pool, entry) == NULL)
        return 0;
    FC_INT(free_entry)(fc, entry);
    return 1;
}

void
FC_API(stats)(const FC_CACHE_T *fc, FC_STATS_T *out)
{
    *out = fc->stats;
}

/*===========================================================================
 * Cleanup macro namespace
 *===========================================================================*/
#undef _FC_CAT2
#undef _FC_CAT
#undef FC_HT
#undef FC_INT
#undef FC_API
#undef FC_PREFIX
#undef FC_KEY_T
#undef FC_RESULT_T
#undef FC_ENTRY_T
#undef FC_CACHE_T
#undef FC_CONFIG_T
#undef FC_STATS_T
#undef FC_PRESSURE
#undef FC_FREE_HEAD_T
#undef FC_CMP_FN
#undef FC_HASH_FN

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
