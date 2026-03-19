/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <assert.h>
#include <string.h>

#include "flow6_cache2.h"

/* Pipeline geometry — shared with flow4 */
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
    FC2_RELIEF_STAGE_SLOTS = 4u
};

#ifndef FC2_LOOKUP_CTX_PIPELINES
#define FC2_LOOKUP_CTX_PIPELINES 0u
#endif

#ifndef FC2_LOOKUP_SPLIT_BK1
#define FC2_LOOKUP_SPLIT_BK1 0u
#endif

static inline union rix_hash_hash_u
fc2_flow6_hash_fn(const struct fc2_flow6_key *key, uint32_t mask)
{
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
}

int
fc2_flow6_cmp(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(struct fc2_flow6_key));
}

#if defined(__AVX2__)
/*
 * fc2 currently targets an AVX2-focused flow6 datapath.  Bind the staged
 * fingerprint scans directly to the AVX2 helpers so lookup does not go
 * through runtime function-pointer dispatch.
 */
#undef _RIX_HASH_FIND_U32X16
#undef _RIX_HASH_FIND_U32X16_2
#define _RIX_HASH_FIND_U32X16(arr, val) \
    _rix_hash_find_u32x16_AVX2((arr), (val))
#define _RIX_HASH_FIND_U32X16_2(arr, val0, val1, mask0, mask1) \
    _rix_hash_find_u32x16_2_AVX2((arr), (val0), (val1), (mask0), (mask1))
#endif
/*
 * Generate static cuckoo-hash helpers for flow6, including:
 *   fc2_flow6_ht_find / insert_hashed / remove / hptr
 */
RIX_HASH_GENERATE_STATIC_SLOT_EX(fc2_flow6_ht, fc2_flow6_entry,
                                 key, cur_hash, slot,
                                 fc2_flow6_cmp, fc2_flow6_hash_fn)
#if defined(__AVX2__)
#undef _RIX_HASH_FIND_U32X16_2
#undef _RIX_HASH_FIND_U32X16
#endif

static inline void
fc2_flow6_prefetch_insert_hash(const struct fc2_flow6_cache *fc,
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
fc2_flow6_result_set_hit(struct fc2_flow6_result *result,
                         uint32_t entry_idx)
{
    result->entry_idx = entry_idx;
}

static inline void
fc2_flow6_result_set_miss(struct fc2_flow6_result *result)
{
    result->entry_idx = 0u;
}

static inline void
fc2_flow6_result_set_filled(struct fc2_flow6_result *result,
                            uint32_t entry_idx)
{
    result->entry_idx = entry_idx;
}

static inline void
fc2_flow6_update_eff_timeout(struct fc2_flow6_cache *fc)
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
fc2_flow6_threshold64(unsigned total_slots, unsigned parts64)
{
    return (unsigned)(((uint64_t)total_slots * parts64) >> 6);
}

static void
fc2_flow6_init_thresholds(struct fc2_flow6_cache *fc)
{
    fc->timeout_lo_entries = fc2_flow6_threshold64(fc->total_slots, 38u);
    fc->timeout_hi_entries = fc2_flow6_threshold64(fc->total_slots, 48u);
    fc->timeout_min_tsc = fc->timeout_tsc >> 3;
    if (fc->timeout_min_tsc == 0u)
        fc->timeout_min_tsc = 1u;
    fc->relief_mid_entries = fc2_flow6_threshold64(fc->total_slots, 45u);
    fc->relief_hi_entries = fc2_flow6_threshold64(fc->total_slots, 48u);
}

static inline unsigned
fc2_flow6_relief_empty_slots(const struct fc2_flow6_cache *fc)
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

static inline struct fc2_flow6_entry *
fc2_flow6_alloc_entry(struct fc2_flow6_cache *fc)
{
    struct fc2_flow6_entry *entry = RIX_SLIST_FIRST(&fc->free_head, fc->pool);

    if (entry != NULL)
        RIX_SLIST_REMOVE_HEAD(&fc->free_head, fc->pool, free_link);
    return entry;
}

static inline void
fc2_flow6_free_entry(struct fc2_flow6_cache *fc,
                     struct fc2_flow6_entry *entry)
{
    entry->last_ts = 0u;
    RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
}

static unsigned
fc2_flow6_scan_bucket_slots(struct fc2_flow6_cache *fc,
                            unsigned bk_idx,
                            uint64_t expire_before,
                            unsigned *expired_slots,
                            int *oldest_slot)
{
    struct rix_hash_bucket_s *bucket = fc->buckets + bk_idx;
    uint64_t oldest_ts = UINT64_MAX;
    struct fc2_flow6_entry *entries[RIX_HASH_BUCKET_ENTRY_SZ];
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
        struct fc2_flow6_entry *entry;

        if (idx == (unsigned)RIX_NIL)
            continue;
        entry = RIX_HASH_FUNC(fc2_flow6_ht, hptr)(fc->pool, idx);
        slots[used] = slot;
        entries[used] = entry;
        used++;
    }
    *oldest_slotp = -1;
    if (used == 0u)
        return 0u;

    cur_count = (used < FC2_RELIEF_STAGE_SLOTS) ? used : FC2_RELIEF_STAGE_SLOTS;
    while (next_count < cur_count) {
        __builtin_prefetch(entries[next_count], 0, 3);
        next_count++;
    }
    next_base = cur_count;

    while (cur_count != 0u) {
        next_count = 0u;
        while (next_count < FC2_RELIEF_STAGE_SLOTS && next_base < used) {
            __builtin_prefetch(entries[next_base], 0, 3);
            next_count++;
            next_base++;
        }
        for (unsigned s = 0; s < cur_count; s++) {
            unsigned idx = cur_base + s;
            unsigned slot = slots[idx];
            struct fc2_flow6_entry *entry = entries[idx];

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
fc2_flow6_reclaim_bucket(struct fc2_flow6_cache *fc,
                         unsigned bk_idx,
                         uint64_t expire_before)
{
    struct fc2_flow6_entry *victim;
    unsigned removed_idx;
    unsigned expired_slots[RIX_HASH_BUCKET_ENTRY_SZ];
    unsigned expired_count;
    int victim_slot;

    expired_count = fc2_flow6_scan_bucket_slots(fc, bk_idx, expire_before,
                                                expired_slots, &victim_slot);
    if (expired_count == 0u)
        return 0;
    removed_idx = RIX_HASH_FUNC(fc2_flow6_ht, remove_at)(&fc->ht_head,
                                                         fc->buckets,
                                                         bk_idx,
                                                         (unsigned)victim_slot);
    RIX_ASSERT(removed_idx != (unsigned)RIX_NIL);
    victim = RIX_HASH_FUNC(fc2_flow6_ht, hptr)(fc->pool, removed_idx);
    RIX_ASSERT(victim != NULL);
    fc2_flow6_free_entry(fc, victim);
    return 1;
}

static unsigned
fc2_flow6_reclaim_bucket_all(struct fc2_flow6_cache *fc,
                             unsigned bk_idx,
                             uint64_t expire_before)
{
    unsigned expired_slots[RIX_HASH_BUCKET_ENTRY_SZ];
    unsigned evicted;

    evicted = fc2_flow6_scan_bucket_slots(fc, bk_idx, expire_before,
                                          expired_slots, NULL);
    for (unsigned i = 0; i < evicted; i++) {
        struct fc2_flow6_entry *victim;
        unsigned removed_idx;

        removed_idx = RIX_HASH_FUNC(fc2_flow6_ht, remove_at)(&fc->ht_head,
                                                             fc->buckets,
                                                             bk_idx,
                                                             expired_slots[i]);
        RIX_ASSERT(removed_idx != (unsigned)RIX_NIL);
        victim = RIX_HASH_FUNC(fc2_flow6_ht, hptr)(fc->pool, removed_idx);
        RIX_ASSERT(victim != NULL);
        fc2_flow6_free_entry(fc, victim);
    }
    return evicted;
}

static unsigned
fc2_flow6_maintain_grouped_v2(struct fc2_flow6_cache *fc,
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
        reclaimed = fc2_flow6_reclaim_bucket_all(fc, cur_bk, expire_before);
        fc->stats.maint_evictions += reclaimed;
        evicted += reclaimed;
    }
    return evicted;
}

static void
fc2_flow6_insert_relief_hashed(struct fc2_flow6_cache *fc,
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
    fc2_flow6_update_eff_timeout(fc);
    expire_before = (now_tsc > fc->eff_timeout_tsc) ?
        (now_tsc - fc->eff_timeout_tsc) : 0u;
    _rix_hash_buckets(h, fc->ht_head.rhh_mask, &bk0, &bk1, &fp);
    pressure_empty_slots = fc2_flow6_relief_empty_slots(fc);
    fc->stats.relief_bucket_checks++;
    rix_hash_arch->find_u32x16_2(fc->buckets[bk0].hash, fp, 0u,
                                 &hits_fp, &hits_zero);
    (void)hits_fp;
    if ((unsigned)__builtin_popcount(hits_zero) <= pressure_empty_slots &&
        fc2_flow6_reclaim_bucket(fc, bk0, expire_before)) {
        fc->stats.relief_evictions++;
        fc->stats.relief_bk0_evictions++;
        return;
    }
    if (bk1 != bk0) {
        fc->stats.relief_bucket_checks++;
        rix_hash_arch->find_u32x16_2(fc->buckets[bk1].hash, fp, 0u,
                                     &hits_fp, &hits_zero);
        if ((unsigned)__builtin_popcount(hits_zero) <= pressure_empty_slots &&
            fc2_flow6_reclaim_bucket(fc, bk1, expire_before)) {
            fc->stats.relief_evictions++;
            fc->stats.relief_bk1_evictions++;
        }
    }
}

void
fc2_flow6_cache_init(struct fc2_flow6_cache *fc,
                     struct rix_hash_bucket_s *buckets,
                     unsigned nb_bk,
                     struct fc2_flow6_entry *pool,
                     unsigned max_entries,
                     const struct fc2_flow6_config *cfg)
{
    struct fc2_flow6_config defcfg = {
        .timeout_tsc = UINT64_C(1000000),
        .pressure_empty_slots = FC2_FLOW6_DEFAULT_PRESSURE_EMPTY_SLOTS
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
        cfg->pressure_empty_slots : FC2_FLOW6_DEFAULT_PRESSURE_EMPTY_SLOTS;
    fc2_flow6_init_thresholds(fc);

    RIX_SLIST_INIT(&fc->free_head);
    fc2_flow6_ht_init(&fc->ht_head, nb_bk);
    for (unsigned i = 0; i < max_entries; i++)
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, &fc->pool[i], free_link);
}

void
fc2_flow6_cache_flush(struct fc2_flow6_cache *fc)
{
    memset(fc->buckets, 0, (size_t)fc->nb_bk * sizeof(*fc->buckets));
    RIX_SLIST_INIT(&fc->free_head);
    fc2_flow6_ht_init(&fc->ht_head, fc->nb_bk);
    for (unsigned i = 0; i < fc->max_entries; i++) {
        fc->pool[i].last_ts = 0u;
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, &fc->pool[i], free_link);
    }
}

unsigned
fc2_flow6_cache_nb_entries(const struct fc2_flow6_cache *fc)
{
    return fc->ht_head.rhh_nb;
}

unsigned
fc2_flow6_cache_lookup_batch(struct fc2_flow6_cache *fc,
                             const struct fc2_flow6_key *keys,
                             unsigned nb_keys,
                             uint64_t now,
                             struct fc2_flow6_result *results,
                             uint16_t *miss_idx)
{
#if FC2_LOOKUP_CTX_PIPELINES > 0u
    struct rix_hash_find_ctx_s ctx[FC2_LOOKUP_CTX_PIPELINES];
    unsigned key_idx[FC2_LOOKUP_CTX_PIPELINES];
    uint8_t stage[FC2_LOOKUP_CTX_PIPELINES];
    unsigned next_key = 0u;
    unsigned done = 0u;
    unsigned miss_count = 0u;
    uint64_t hit_count = 0u;

    memset(stage, 0, sizeof(stage));
    while (done < nb_keys) {
        for (unsigned lane = 0; lane < FC2_LOOKUP_CTX_PIPELINES; lane++) {
            switch (stage[lane]) {
            case 0:
                if (next_key >= nb_keys)
                    break;
                key_idx[lane] = next_key++;
                RIX_HASH_FUNC(fc2_flow6_ht, hash_key)(&ctx[lane], &fc->ht_head,
                                                      fc->buckets,
                                                      &keys[key_idx[lane]]);
                stage[lane] = 1u;
                break;
            case 1:
                RIX_HASH_FUNC(fc2_flow6_ht, scan_bk)(&ctx[lane], &fc->ht_head,
                                                     fc->buckets);
                stage[lane] = 2u;
                break;
            case 2:
                RIX_HASH_FUNC(fc2_flow6_ht, prefetch_node)(&ctx[lane], fc->pool);
                stage[lane] = 3u;
                break;
            default: {
                unsigned idx = key_idx[lane];
                struct fc2_flow6_entry *entry =
                    RIX_HASH_FUNC(fc2_flow6_ht, cmp_key)(&ctx[lane], fc->pool);

                if (entry == NULL) {
                    fc2_flow6_result_set_miss(&results[idx]);
                    if (miss_idx != NULL)
                        miss_idx[miss_count] = (uint16_t)idx;
                    miss_count++;
                } else {
                    entry->last_ts = now;
                    fc2_flow6_result_set_hit(&results[idx],
                                             RIX_IDX_FROM_PTR(fc->pool, entry));
                    hit_count++;
                }
                stage[lane] = 0u;
                done++;
                break;
            }
            }
        }
    }
#else
    struct rix_hash_find_ctx_s ctx[nb_keys];
    unsigned miss_count = 0u;
    uint64_t hit_count = 0u;
    const unsigned ahead_keys = FLOW_CACHE_LOOKUP_AHEAD_KEYS;
    const unsigned step_keys = FLOW_CACHE_LOOKUP_STEP_KEYS;
    const unsigned total =
#if FC2_LOOKUP_SPLIT_BK1
        nb_keys + 5u * ahead_keys;
#else
        nb_keys + 3u * ahead_keys;
#endif
#if FC2_LOOKUP_SPLIT_BK1
    uint32_t need_bk1[nb_keys];

    memset(need_bk1, 0, sizeof(need_bk1));
#endif

    for (unsigned i = 0; i < total; i += step_keys) {
        if (i < nb_keys) {
            unsigned n = (i + step_keys <= nb_keys) ? step_keys : (nb_keys - i);

            for (unsigned j = 0; j < n; j++) {
                RIX_HASH_FUNC(fc2_flow6_ht, hash_key)(&ctx[i + j], &fc->ht_head,
                                                      fc->buckets, &keys[i + j]);
            }
        }

        if (i >= ahead_keys && i - ahead_keys < nb_keys) {
            unsigned base = i - ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++)
                RIX_HASH_FUNC(fc2_flow6_ht, scan_bk)(&ctx[base + j],
                                                     &fc->ht_head,
                                                     fc->buckets);
        }

        if (i >= 2u * ahead_keys && i - 2u * ahead_keys < nb_keys) {
            unsigned base = i - 2u * ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++)
                RIX_HASH_FUNC(fc2_flow6_ht, prefetch_node)(&ctx[base + j],
                                                           fc->pool);
        }

        if (i >= 3u * ahead_keys && i - 3u * ahead_keys < nb_keys) {
            unsigned base = i - 3u * ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++) {
                unsigned idx = base + j;
                struct fc2_flow6_entry *entry = NULL;
#if FC2_LOOKUP_SPLIT_BK1
                uint32_t hits0 = ctx[idx].fp_hits[0];

                while (hits0) {
                    unsigned bit = (unsigned)__builtin_ctz(hits0);
                    unsigned nidx;

                    hits0 &= hits0 - 1u;
                    nidx = ctx[idx].bk[0]->idx[bit];
                    if (nidx == (unsigned)RIX_NIL)
                        continue;
                    entry = RIX_PTR_FROM_IDX(fc->pool, nidx);
                    if (entry != NULL &&
                        fc2_flow6_cmp((const void *)&entry->key, ctx[idx].key) == 0) {
                        entry->last_ts = now;
                        fc2_flow6_result_set_hit(&results[idx],
                                                 RIX_IDX_FROM_PTR(fc->pool, entry));
                        hit_count++;
                        break;
                    }
                }

                if (entry == NULL) {
                    need_bk1[idx] = 1u;
                    _rix_hash_prefetch_bucket(ctx[idx].bk[1]);
                    continue;
                }
#else
                entry = RIX_HASH_FUNC(fc2_flow6_ht, cmp_key)(&ctx[idx], fc->pool);

                if (entry == NULL) {
                    fc2_flow6_result_set_miss(&results[idx]);
                    if (miss_idx != NULL)
                        miss_idx[miss_count] = (uint16_t)idx;
                    miss_count++;
                    continue;
                }
                entry->last_ts = now;
                fc2_flow6_result_set_hit(&results[idx],
                                         RIX_IDX_FROM_PTR(fc->pool, entry));
                hit_count++;
#endif
            }
        }
#if FC2_LOOKUP_SPLIT_BK1

        if (i >= 4u * ahead_keys && i - 4u * ahead_keys < nb_keys) {
            unsigned base = i - 4u * ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++) {
                unsigned idx = base + j;

                if (!need_bk1[idx])
                    continue;
                ctx[idx].fp_hits[1] =
                    rix_hash_arch->find_u32x16(ctx[idx].bk[1]->hash, ctx[idx].fp);
                if (ctx[idx].fp_hits[1]) {
                    unsigned bit = (unsigned)__builtin_ctz(ctx[idx].fp_hits[1]);
                    unsigned nidx = ctx[idx].bk[1]->idx[bit];

                    if (nidx != (unsigned)RIX_NIL)
                        _rix_hash_prefetch_entry(RIX_PTR_FROM_IDX(fc->pool, nidx));
                }
            }
        }

        if (i >= 5u * ahead_keys && i - 5u * ahead_keys < nb_keys) {
            unsigned base = i - 5u * ahead_keys;
            unsigned n = (base + step_keys <= nb_keys) ? step_keys : (nb_keys - base);

            for (unsigned j = 0; j < n; j++) {
                unsigned idx = base + j;
                uint32_t hits1;
                struct fc2_flow6_entry *entry = NULL;

                if (!need_bk1[idx])
                    continue;
                hits1 = ctx[idx].fp_hits[1];
                while (hits1) {
                    unsigned bit = (unsigned)__builtin_ctz(hits1);
                    unsigned nidx;

                    hits1 &= hits1 - 1u;
                    nidx = ctx[idx].bk[1]->idx[bit];
                    if (nidx == (unsigned)RIX_NIL)
                        continue;
                    entry = RIX_PTR_FROM_IDX(fc->pool, nidx);
                    if (entry != NULL &&
                        fc2_flow6_cmp((const void *)&entry->key, ctx[idx].key) == 0) {
                        entry->last_ts = now;
                        fc2_flow6_result_set_hit(&results[idx],
                                                 RIX_IDX_FROM_PTR(fc->pool, entry));
                        hit_count++;
                        break;
                    }
                }
                if (entry == NULL) {
                    fc2_flow6_result_set_miss(&results[idx]);
                    if (miss_idx != NULL)
                        miss_idx[miss_count] = (uint16_t)idx;
                    miss_count++;
                }
            }
        }
#endif
    }
#endif

    fc->stats.lookups += nb_keys;
    fc->stats.hits += hit_count;
    fc->stats.misses += nb_keys - hit_count;
    return miss_count;
}

unsigned
fc2_flow6_cache_fill_miss_batch(struct fc2_flow6_cache *fc,
                                const struct fc2_flow6_key *keys,
                                const uint16_t *miss_idx,
                                unsigned miss_count,
                                uint64_t now,
                                struct fc2_flow6_result *results)
{
    enum { FC2_FILL_PLAN_KEYS = 64 };
    unsigned inserted = 0u;

    for (unsigned base = 0; base < miss_count; base += FC2_FILL_PLAN_KEYS) {
        union rix_hash_hash_u hashes[FC2_FILL_PLAN_KEYS];
        unsigned n = miss_count - base;

        if (n > FC2_FILL_PLAN_KEYS)
            n = FC2_FILL_PLAN_KEYS;

        for (unsigned i = 0; i < n; i++) {
            unsigned key_idx = miss_idx[base + i];

            hashes[i] = fc2_flow6_hash_fn(&keys[key_idx], fc->ht_head.rhh_mask);
            fc2_flow6_prefetch_insert_hash(fc, hashes[i]);
        }

        for (unsigned i = 0; i < n; i++) {
            unsigned key_idx = miss_idx[base + i];
            struct fc2_flow6_entry *entry;
            struct fc2_flow6_entry *ret;

            fc2_flow6_insert_relief_hashed(fc, hashes[i], now);
            entry = fc2_flow6_alloc_entry(fc);
            if (entry == NULL) {
                fc->stats.fill_full++;
                fc2_flow6_result_set_miss(&results[key_idx]);
                continue;
            }

            entry->key = keys[key_idx];
            entry->last_ts = now;
            ret = RIX_HASH_FUNC(fc2_flow6_ht, insert_hashed)(&fc->ht_head,
                                                              fc->buckets,
                                                              fc->pool,
                                                              entry,
                                                              hashes[i]);
            if (ret == NULL) {
                fc->stats.fills++;
                fc2_flow6_result_set_filled(&results[key_idx],
                                            RIX_IDX_FROM_PTR(fc->pool, entry));
                inserted++;
                continue;
            }
            fc2_flow6_free_entry(fc, entry);
            if (ret != entry) {
                ret->last_ts = now;
                fc2_flow6_result_set_filled(&results[key_idx],
                                            RIX_IDX_FROM_PTR(fc->pool, ret));
                continue;
            }
            fc->stats.fill_full++;
            fc2_flow6_result_set_miss(&results[key_idx]);
            continue;
        }
    }
    return inserted;
}

unsigned
fc2_flow6_cache_maintain(struct fc2_flow6_cache *fc,
                         unsigned start_bk,
                         unsigned bucket_count,
                         uint64_t now)
{
    fc->stats.maint_calls++;
    fc2_flow6_update_eff_timeout(fc);
    return fc2_flow6_maintain_grouped_v2(fc, start_bk, bucket_count, now);
}

int
fc2_flow6_cache_remove_idx(struct fc2_flow6_cache *fc, uint32_t entry_idx)
{
    struct fc2_flow6_entry *entry;

    if (entry_idx == 0u || entry_idx > fc->max_entries)
        return 0;
    entry = RIX_PTR_FROM_IDX(fc->pool, entry_idx);
    if (entry == NULL)
        return 0;
    if (RIX_HASH_FUNC(fc2_flow6_ht, remove)(&fc->ht_head, fc->buckets,
                                            fc->pool, entry) == NULL)
        return 0;
    fc2_flow6_free_entry(fc, entry);
    return 1;
}

void
fc2_flow6_cache_stats(const struct fc2_flow6_cache *fc,
                      struct fc2_flow6_stats *out)
{
    *out = fc->stats;
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
