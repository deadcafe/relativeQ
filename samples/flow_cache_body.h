/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_cache_body.h - Macro-templated flow cache implementation.
 *
 * Before including this file, define:
 *   FC_PREFIX        e.g. flow4
 *   FC_ENTRY         e.g. flow4_entry
 *   FC_KEY           e.g. flow4_key
 *   FC_CACHE         e.g. flow4_cache
 *   FC_HT_PREFIX     e.g. flow4_ht
 *   FC_FREE_HEAD     e.g. flow4_free_head
 */

#include <assert.h>
#include <string.h>

/* token-paste helpers */
#define _FC_CAT(a, b)    a ## b
#define FC_CAT(a, b)     _FC_CAT(a, b)
#define FC_FN(suffix)    FC_CAT(FC_PREFIX, suffix)

/*===========================================================================
 * Internal: fill-rate-driven expire pressure
 *
 * expire_level: 0-15 based on how far nb_entries exceeds 50% of max_entries.
 * expire_scan:  64 -> 1024 entries scanned per expire() call.
 *
 * Scan level computation (max_entries is power of 2):
 *   excess = nb - max/2
 *   level  = excess >> (log2(max) - 4)
 *   level 0-3: scan 64..512,  level >= 4: scan 1024
 *===========================================================================*/
static inline unsigned
FC_FN(_cache_expire_level)(const struct FC_CACHE *fc)
{
    unsigned nb   = fc->ht_head.rhh_nb;
    unsigned max  = fc->max_entries;
    unsigned half = max >> 1;

    if (nb <= half)
        return 0;

    unsigned excess = nb - half;
    unsigned shift  = (unsigned)__builtin_ctz(max) - 4; /* max >= 64 */
    unsigned level  = excess >> shift;

    return (level > 15) ? 15 : level;
}

static inline unsigned
FC_FN(_cache_expire_scan)(const struct FC_CACHE *fc)
{
    unsigned level = FC_FN(_cache_expire_level)(fc);

    if (level >= 4)
        return FLOW_CACHE_EXPIRE_SCAN_MAX;

    return (unsigned)FLOW_CACHE_EXPIRE_SCAN_MIN << level;
}

/*===========================================================================
 * Init
 *===========================================================================*/
void
FC_FN(_cache_init)(struct FC_CACHE *fc,
                   struct rix_hash_bucket_s *buckets,
                   unsigned nb_bk,
                   struct FC_ENTRY *pool,
                   unsigned max_entries,
                   uint64_t timeout_ms)
{
    memset(fc, 0, sizeof(*fc));

    rix_hash_arch_init();

    FC_CAT(FC_HT_PREFIX, _init)(&fc->ht_head, nb_bk);
    fc->buckets = buckets;
    fc->nb_bk   = nb_bk;
    memset(buckets, 0, nb_bk * sizeof(*buckets));

    assert((max_entries & (max_entries - 1)) == 0 &&
           "max_entries must be a power of 2");
    fc->pool         = pool;
    fc->max_entries  = max_entries;
    fc->entries_mask = max_entries - 1;
    memset(pool, 0, max_entries * sizeof(*pool));

    RIX_SLIST_INIT(&fc->free_head);
    for (unsigned i = max_entries; i-- > 0; )
        RIX_SLIST_INSERT_HEAD(&fc->free_head, pool, &pool[i], free_link);

    fc->age_cursor = 0;
    if (timeout_ms > 0) {
        uint64_t tsc_hz = flow_cache_calibrate_tsc_hz();
        fc->timeout_tsc = flow_cache_ms_to_tsc(tsc_hz, timeout_ms);
        fc->min_timeout_tsc = flow_cache_ms_to_tsc(tsc_hz,
                                                    FLOW_CACHE_TIMEOUT_MIN_MS);
    } else {
        fc->timeout_tsc = UINT64_MAX;
        fc->min_timeout_tsc = UINT64_MAX;
    }
    fc->eff_timeout_tsc = fc->timeout_tsc;
}

/*===========================================================================
 * Evict one expired entry (timeout-based, scans pool via age cursor)
 *
 * Scans at most max_entries/4 per call to bound worst-case cost.
 * Returns NULL if no expired entry found within the scan window;
 * caller falls through to evict_bucket_oldest.
 *===========================================================================*/
static struct FC_ENTRY *
FC_FN(_cache_evict_one)(struct FC_CACHE *fc, uint64_t now)
{
    uint64_t timeout = fc->eff_timeout_tsc;
    const unsigned pf_dist = FLOW_CACHE_EXPIRE_PF_DIST;
    const unsigned max_scan = fc->max_entries >> 3;  /* 1/8 of pool */

    for (unsigned i = 0; i < max_scan; i++) {
        unsigned pf_idx = (fc->age_cursor + pf_dist) & fc->entries_mask;
        __builtin_prefetch(&fc->pool[pf_idx], 0, 1);

        struct FC_ENTRY *entry = &fc->pool[fc->age_cursor];
        fc->age_cursor = (fc->age_cursor + 1) & fc->entries_mask;

        if (entry->last_ts == 0)
            continue;
        if (now - entry->last_ts <= timeout)
            continue;

        FC_CAT(FC_HT_PREFIX, _remove)(&fc->ht_head, fc->buckets,
                                       fc->pool, entry);
        entry->last_ts = 0;
        fc->stats.evictions++;
        return entry;
    }
    return NULL;
}

/*===========================================================================
 * Evict the oldest entry in the target bucket (forced, last resort)
 *
 * Hash the key to find bk0/bk1, scan all entries in both candidate
 * buckets, evict the one with the smallest last_ts.
 * Guarantees a free pool entry AND a free bucket slot for subsequent
 * ht_insert (fast path, no cuckoo needed).
 *===========================================================================*/
static struct FC_ENTRY *
FC_FN(_cache_evict_bucket_oldest)(struct FC_CACHE *fc,
                                   const struct FC_KEY *key)
{
    unsigned mask = fc->ht_head.rhh_mask;
    union rix_hash_hash_u h =
        _rix_hash_fn_crc32((const void *)key, sizeof(struct FC_KEY), mask);
    unsigned bk0, bk1;
    uint32_t fp;
    _rix_hash_buckets(h, mask, &bk0, &bk1, &fp);
    (void)fp;

    struct FC_ENTRY *oldest = NULL;
    uint64_t oldest_ts = UINT64_MAX;

    for (int i = 0; i < 2; i++) {
        struct rix_hash_bucket_s *bk = &fc->buckets[i == 0 ? bk0 : bk1];
        for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {
            unsigned nidx = bk->idx[s];
            if (nidx == (unsigned)RIX_NIL)
                continue;
            struct FC_ENTRY *e =
                FC_CAT(FC_HT_PREFIX, _hptr)(fc->pool, nidx);
            if (e->last_ts == 0)
                continue;
            if (e->last_ts < oldest_ts) {
                oldest_ts = e->last_ts;
                oldest = e;
            }
        }
    }

    if (oldest == NULL)
        return NULL;

    FC_CAT(FC_HT_PREFIX, _remove)(&fc->ht_head, fc->buckets,
                                   fc->pool, oldest);
    oldest->last_ts = 0;
    fc->stats.evictions++;
    return oldest;
}

/*===========================================================================
 * Insert - never fails (cache semantics: forced eviction as last resort)
 *
 * Priority: free list -> evict_one (expired) -> evict_bucket_oldest (forced).
 * The bucket eviction frees both a pool entry and a bucket slot, so
 * ht_insert always finds an empty slot in the fast path.
 *===========================================================================*/
struct FC_ENTRY *
FC_FN(_cache_insert)(struct FC_CACHE *fc,
                     const struct FC_KEY *key,
                     uint64_t now)
{
    struct FC_ENTRY *entry = RIX_SLIST_FIRST(&fc->free_head, fc->pool);

    if (entry == NULL) {
        entry = FC_FN(_cache_evict_one)(fc, now);
        if (entry == NULL) {
            /* Pool exhausted, no expired entry found.
             * Force-evict the oldest entry in the target bucket. */
            entry = FC_FN(_cache_evict_bucket_oldest)(fc, key);
            if (entry == NULL)
                return NULL;  /* both buckets empty - shouldn't happen */
        }
    } else {
        RIX_SLIST_REMOVE_HEAD(&fc->free_head, fc->pool, free_link);
    }

    entry->key       = *key;
    entry->action    = FLOW_ACTION_NONE;
    entry->qos_class = 0;
    entry->last_ts   = now;
    entry->packets   = 0;
    entry->bytes     = 0;

    struct FC_ENTRY *dup =
        FC_CAT(FC_HT_PREFIX, _insert)(&fc->ht_head, fc->buckets,
                                       fc->pool, entry);
    if (dup == NULL) {
        fc->stats.inserts++;
        return entry;
    }
    if (dup == entry) {
        /* Cuckoo kickout exhausted - extremely rare.
         * Entry was not placed; return it to free list. */
        entry->last_ts = 0;
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
        return NULL;
    }
    /* Duplicate key already exists - touch it instead. */
    entry->last_ts = 0;
    RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
    dup->last_ts = now;
    return dup;
}

/*===========================================================================
 * Pipelined batch lookup
 *===========================================================================*/
void
FC_FN(_cache_lookup_batch)(struct FC_CACHE *fc,
                           const struct FC_KEY *keys,
                           unsigned nb_pkts,
                           struct FC_ENTRY **results)
{
    struct rix_hash_find_ctx_s ctx[nb_pkts];

    const unsigned DIST = FLOW_CACHE_DIST;
    const unsigned BATCH = FLOW_CACHE_BATCH;
    const unsigned total = nb_pkts + 3 * DIST;

    for (unsigned i = 0; i < total; i += BATCH) {
        if (i < nb_pkts) {
            unsigned n = (i + BATCH <= nb_pkts) ? BATCH : (nb_pkts - i);
            for (unsigned j = 0; j < n; j++)
                FC_CAT(FC_HT_PREFIX, _hash_key)(&ctx[i + j],
                                                  &fc->ht_head, fc->buckets,
                                                  &keys[i + j]);
        }

        if (i >= DIST && i - DIST < nb_pkts) {
            unsigned base = i - DIST;
            unsigned n = (base + BATCH <= nb_pkts) ? BATCH : (nb_pkts - base);
            for (unsigned j = 0; j < n; j++)
                FC_CAT(FC_HT_PREFIX, _scan_bk)(&ctx[base + j],
                                                &fc->ht_head, fc->buckets);
        }

        if (i >= 2 * DIST && i - 2 * DIST < nb_pkts) {
            unsigned base = i - 2 * DIST;
            unsigned n = (base + BATCH <= nb_pkts) ? BATCH : (nb_pkts - base);
            for (unsigned j = 0; j < n; j++)
                FC_CAT(FC_HT_PREFIX, _prefetch_node)(&ctx[base + j],
                                                      fc->pool);
        }

        if (i >= 3 * DIST && i - 3 * DIST < nb_pkts) {
            unsigned base = i - 3 * DIST;
            unsigned n = (base + BATCH <= nb_pkts) ? BATCH : (nb_pkts - base);
            for (unsigned j = 0; j < n; j++)
                results[base + j] =
                    FC_CAT(FC_HT_PREFIX, _cmp_key)(&ctx[base + j],
                                                    fc->pool);
        }
    }

    fc->stats.lookups += nb_pkts;
    for (unsigned i = 0; i < nb_pkts; i++) {
        if (results[i])
            fc->stats.hits++;
        else
            fc->stats.misses++;
    }
}

/*===========================================================================
 * Aging expire - single-stage pipeline (default)
 *
 * Scan count: fill-rate-driven (level 0..15, 64->1024).
 * Timeout: miss-rate-driven (eff_timeout_tsc, adjusted by caller).
 *
 * Prefetches entry CL0 ahead; bucket access on remove is unpipelined.
 * Suitable when pool fits in LLC or eviction rate is low.
 * See FC_FN(_cache_expire_2stage) for bucket-prefetch variant.
 *===========================================================================*/
void
FC_FN(_cache_expire)(struct FC_CACHE *fc,
                     uint64_t now)
{
    unsigned max_scan = FC_CAT(FC_PREFIX, _cache_expire_scan)(fc);
    uint64_t timeout  = fc->eff_timeout_tsc;
    const unsigned pf_dist = FLOW_CACHE_EXPIRE_PF_DIST;
    const unsigned mask = fc->entries_mask;
    unsigned cursor = fc->age_cursor;

    for (unsigned i = 0; i < max_scan; i++) {
        /* SW prefetch: CL0 only (last_ts is in CL0) */
        unsigned pf_idx = (cursor + i + pf_dist) & mask;
        __builtin_prefetch(&fc->pool[pf_idx], 0, 1);

        struct FC_ENTRY *entry = &fc->pool[(cursor + i) & mask];

        if (entry->last_ts == 0)
            continue;
        if (now - entry->last_ts <= timeout)
            continue;

        FC_CAT(FC_HT_PREFIX, _remove)(&fc->ht_head, fc->buckets,
                                       fc->pool, entry);
        entry->last_ts = 0;

        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
        fc->stats.evictions++;
    }

    fc->age_cursor = (cursor + max_scan) & mask;
}

/*===========================================================================
 * Aging expire - 2-stage pipeline (bucket prefetch variant)
 *
 * Same adaptive 16-level pressure as single-stage, but adds a second
 * pipeline stage: entries at mid-distance (pf_dist/2) are checked early
 * and, if expired, their hash bucket is prefetched so that _remove()
 * hits warm cache lines.
 *
 * Pipeline stages per iteration:
 *   [cursor + pf_dist]      prefetch entry CL0
 *   [cursor + pf_dist/2]    entry warm -> if expired, prefetch bucket
 *   [cursor]                remove (bucket warm) + return to free list
 *
 * Beneficial when pool >> LLC and eviction rate per call is high.
 * Overhead of stage 1 check hurts when buckets are already warm (small
 * pool) or eviction is rare.
 *===========================================================================*/
void
FC_FN(_cache_expire_2stage)(struct FC_CACHE *fc,
                            uint64_t now)
{
    unsigned max_scan = FC_CAT(FC_PREFIX, _cache_expire_scan)(fc);
    uint64_t timeout  = fc->eff_timeout_tsc;
    const unsigned pf_dist    = FLOW_CACHE_EXPIRE_PF_DIST;
    const unsigned bk_pf_dist = pf_dist / 2;
    const unsigned mask = fc->entries_mask;
    unsigned cursor = fc->age_cursor;

    for (unsigned i = 0; i < max_scan; i++) {
        /* Stage 0: prefetch entry CL0 far ahead */
        unsigned pf_idx = (cursor + i + pf_dist) & mask;
        __builtin_prefetch(&fc->pool[pf_idx], 0, 1);

        /* Stage 1: mid-distance entry is warm - prefetch its bucket
         * if it looks expired (cur_hash is in CL0, already cached) */
        unsigned mid_idx = (cursor + i + bk_pf_dist) & mask;
        struct FC_ENTRY *mid = &fc->pool[mid_idx];
        if (mid->last_ts != 0 && now - mid->last_ts > timeout) {
            unsigned bk = mid->cur_hash & fc->ht_head.rhh_mask;
            __builtin_prefetch(&fc->buckets[bk], 1, 1);
        }

        /* Stage 2: process current entry (bucket warm if expired) */
        struct FC_ENTRY *entry = &fc->pool[(cursor + i) & mask];

        if (entry->last_ts == 0)
            continue;
        if (now - entry->last_ts <= timeout)
            continue;

        FC_CAT(FC_HT_PREFIX, _remove)(&fc->ht_head, fc->buckets,
                                       fc->pool, entry);
        entry->last_ts = 0;

        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
        fc->stats.evictions++;
    }

    fc->age_cursor = (cursor + max_scan) & mask;
}

/*===========================================================================
 * Find - non-pipelined single key lookup (slow path / debug)
 *
 * Executes all 4 pipeline stages back-to-back without inter-packet overlap.
 * Use lookup_batch() for high-throughput packet processing.
 *===========================================================================*/
struct FC_ENTRY *
FC_FN(_cache_find)(struct FC_CACHE *fc,
                   const struct FC_KEY *key)
{
    struct rix_hash_find_ctx_s ctx;
    FC_CAT(FC_HT_PREFIX, _hash_key)(&ctx, &fc->ht_head, fc->buckets, key);
    FC_CAT(FC_HT_PREFIX, _scan_bk)(&ctx, &fc->ht_head, fc->buckets);
    FC_CAT(FC_HT_PREFIX, _prefetch_node)(&ctx, fc->pool);
    return FC_CAT(FC_HT_PREFIX, _cmp_key)(&ctx, fc->pool);
}

/*===========================================================================
 * Remove - explicit caller-driven removal
 *
 * Removes entry from the hash table and returns it to the free pool.
 * Increments stats.removes (distinct from stats.evictions, which tracks
 * timeout-based evictions performed by expire/expire_2stage).
 *
 * Use cases: TCP FIN/RST, admin teardown, policy invalidation.
 *===========================================================================*/
void
FC_FN(_cache_remove)(struct FC_CACHE *fc,
                     struct FC_ENTRY *entry)
{
    FC_CAT(FC_HT_PREFIX, _remove)(&fc->ht_head, fc->buckets,
                                   fc->pool, entry);
    entry->last_ts = 0;
    RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
    fc->stats.removes++;
}

/*===========================================================================
 * Flush - remove all entries atomically
 *
 * Reinitializes the hash table and free list, returning all pool entries.
 * Increments stats.removes by the number of entries that were present.
 *
 * Use cases: VRF delete, interface down, graceful shutdown.
 *===========================================================================*/
void
FC_FN(_cache_flush)(struct FC_CACHE *fc)
{
    unsigned flushed = fc->ht_head.rhh_nb;

    FC_CAT(FC_HT_PREFIX, _init)(&fc->ht_head, fc->nb_bk);
    memset(fc->buckets, 0, fc->nb_bk * sizeof(*fc->buckets));
    memset(fc->pool, 0, fc->max_entries * sizeof(*fc->pool));

    RIX_SLIST_INIT(&fc->free_head);
    for (unsigned i = fc->max_entries; i-- > 0; )
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, &fc->pool[i],
                              free_link);

    fc->age_cursor = 0;
    fc->stats.removes += flushed;
}

/*===========================================================================
 * Statistics
 *===========================================================================*/
void
FC_FN(_cache_stats)(const struct FC_CACHE *fc,
                    struct flow_cache_stats *out)
{
    *out = fc->stats;
    out->nb_entries  = fc->ht_head.rhh_nb;
    out->max_entries = fc->max_entries;
}

/* clean up local macros */
#undef FC_FN
#undef FC_CAT
#undef _FC_CAT

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
