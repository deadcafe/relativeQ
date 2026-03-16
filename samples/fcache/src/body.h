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
 *
 * INTERNAL USE ONLY.
 * This is a private implementation header of the fcache library.
 * Do not include it directly; it is included by src/flow4.c,
 * src/flow6.c, and src/flowu.c only.
 */

#include <assert.h>
#include <string.h>

/* token-paste helpers (definitions only; calls use FC_CALL from flow_cache_decl.h) */
#define _FC_FN_CAT(a, b, c)  a ## b ## c
#define FC_FN_CAT(a, b, c)   _FC_FN_CAT(a, b, c)
#define FC_FN(prefix, suffix) FC_FN_CAT(prefix, _, suffix)

#ifndef FC_IMPL_ATTR
#define FC_IMPL_ATTR
#endif

/*===========================================================================
 * Default no-op lifecycle callbacks
 *
 * Assigned to fc->init_cb / fc->fini_cb when the caller passes NULL to
 * cache_init().  Using actual function pointers instead of NULL checks
 * eliminates all conditional branches at every call site.
 *===========================================================================*/
static void
FC_FN(FC_PREFIX, default_init_cb)(struct FC_ENTRY *entry __attribute__((unused)),
                                   void *arg __attribute__((unused)))
{
}

static void
FC_FN(FC_PREFIX, default_fini_cb)(struct FC_ENTRY *entry __attribute__((unused)),
                                   flow_cache_fini_reason_t reason __attribute__((unused)),
                                   void *arg __attribute__((unused)))
{
}

static RIX_FORCE_INLINE void
FC_FN(FC_PREFIX, call_init_cb)(struct FC_CACHE *fc, struct FC_ENTRY *entry)
{
    if (fc->init_cb != FC_FN(FC_PREFIX, default_init_cb))
        fc->init_cb(entry, fc->cb_arg);
}

static RIX_FORCE_INLINE void
FC_FN(FC_PREFIX, call_fini_cb)(struct FC_CACHE *fc,
                               struct FC_ENTRY *entry,
                               flow_cache_fini_reason_t reason)
{
    if (fc->fini_cb != FC_FN(FC_PREFIX, default_fini_cb))
        fc->fini_cb(entry, reason, fc->cb_arg);
}

static RIX_FORCE_INLINE void
FC_FN(FC_PREFIX, cache_prefetch_hit_write)(struct FC_ENTRY *entry)
{
    __builtin_prefetch((char *)entry + FLOW_CACHE_LINE_SIZE, 1, 1);
}

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
FC_FN(FC_PREFIX, cache_expire_level)(const struct FC_CACHE *fc)
{
    unsigned nb   = fc->ht_head.rhh_nb;
    unsigned max  = fc->max_entries;
    unsigned half = max >> 1;

    if (max < 64u)
        return 0;

    if (nb <= half)
        return 0;

    unsigned excess = nb - half;
    unsigned shift  = (unsigned)__builtin_ctz(max) - 4; /* max >= 64 */
    unsigned level  = excess >> shift;

    return (level > 15) ? 15 : level;
}

static inline unsigned
FC_FN(FC_PREFIX, cache_expire_scan)(const struct FC_CACHE *fc)
{
    unsigned level = FC_CALL(FC_PREFIX, cache_expire_level)(fc);

    if (level >= 4)
        return FLOW_CACHE_EXPIRE_SCAN_MAX;

    return (unsigned)FLOW_CACHE_EXPIRE_SCAN_MIN << level;
}

static inline int
FC_FN(FC_PREFIX, cache_use_expire_2stage)(const struct FC_CACHE *fc)
{
    if (fc->max_entries < 4096u)
        return 0;
    return FC_CALL(FC_PREFIX, cache_expire_level)(fc) >= 4u;
}

static inline unsigned
FC_FN(FC_PREFIX, cache_expire_pf_dist)(const struct FC_CACHE *fc)
{
    (void)fc;
    return FLOW_CACHE_EXPIRE_PF_DIST;
}

static inline void
FC_FN(FC_PREFIX, cache_prefetch_remove_bucket)(struct FC_CACHE *fc,
                                               unsigned bk)
{
    struct rix_hash_bucket_s *bucket = &fc->buckets[bk];

    /* remove() first scans idx[] (CL1), then clears both idx[] and hash[]. */
    __builtin_prefetch(&bucket->idx[0], 0, 1);
    __builtin_prefetch(&bucket->idx[0], 1, 1);
    __builtin_prefetch(&bucket->hash[0], 1, 1);
}

FC_IMPL_ATTR void
FC_FN(FC_PREFIX, cache_expire_2stage)(struct FC_CACHE *fc,
                                      uint64_t now);

/*===========================================================================
 * Init
 *===========================================================================*/
FC_IMPL_ATTR void
FC_FN(FC_PREFIX, cache_init)(struct FC_CACHE *fc,
                   struct rix_hash_bucket_s *buckets,
                   unsigned nb_bk,
                   struct FC_ENTRY *pool,
                   unsigned max_entries,
                   uint64_t timeout_ms,
                   void (*init_cb)(struct FC_ENTRY *entry, void *arg),
                   void (*fini_cb)(struct FC_ENTRY *entry,
                                   flow_cache_fini_reason_t reason, void *arg),
                   void *cb_arg)
{
    memset(fc, 0, sizeof(*fc));

    FC_CALL(FC_HT_PREFIX, init)(&fc->ht_head, nb_bk);
    fc->buckets = buckets;
    fc->nb_bk   = nb_bk;
    memset(buckets, 0, nb_bk * sizeof(*buckets));

    RIX_ASSERT(max_entries >= 64u &&
               "max_entries must be >= 64; use flow_cache_pool_count()");
    RIX_ASSERT((max_entries & (max_entries - 1)) == 0 &&
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

    /* Assign default no-op callbacks when caller passes NULL.
     * fc->init_cb / fini_cb are guaranteed non-NULL after this point. */
    fc->init_cb = init_cb ? init_cb : FC_FN(FC_PREFIX, default_init_cb);
    fc->fini_cb = fini_cb ? fini_cb : FC_FN(FC_PREFIX, default_fini_cb);
    fc->cb_arg  = cb_arg;
}

/*===========================================================================
 * Evict one expired entry (timeout-based, scans pool via age cursor)
 *
 * Scans at most max_entries/4 per call to bound worst-case cost.
 * Returns NULL if no expired entry found within the scan window;
 * caller falls through to evict_bucket_oldest.
 *===========================================================================*/
static struct FC_ENTRY *
FC_FN(FC_PREFIX, cache_evict_one)(struct FC_CACHE *fc, uint64_t now)
{
    uint64_t timeout = fc->eff_timeout_tsc;
    const unsigned pf_dist = FC_CALL(FC_PREFIX, cache_expire_pf_dist)(fc);
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

        FC_CALL(FC_HT_PREFIX, remove)(&fc->ht_head, fc->buckets,
                                       fc->pool, entry);
        FC_CALL(FC_PREFIX, call_fini_cb)(fc, entry, FLOW_CACHE_FINI_TIMEOUT);
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
FC_FN(FC_PREFIX, cache_evict_bucket_oldest)(struct FC_CACHE *fc,
                                   const struct FC_KEY *key)
{
    unsigned mask = fc->ht_head.rhh_mask;
    union rix_hash_hash_u h = FC_HT_HASH_FN(key, mask);
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
                FC_CALL(FC_HT_PREFIX, hptr)(fc->pool, nidx);
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

    FC_CALL(FC_HT_PREFIX, remove)(&fc->ht_head, fc->buckets,
                                   fc->pool, oldest);
    FC_CALL(FC_PREFIX, call_fini_cb)(fc, oldest, FLOW_CACHE_FINI_EVICTED);
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
FC_IMPL_ATTR struct FC_ENTRY *
FC_FN(FC_PREFIX, cache_insert)(struct FC_CACHE *fc,
                     const struct FC_KEY *key,
                     uint64_t now)
{
    struct FC_ENTRY *entry = RIX_SLIST_FIRST(&fc->free_head, fc->pool);

    if (entry == NULL) {
        entry = FC_CALL(FC_PREFIX, cache_evict_one)(fc, now);
        if (entry == NULL) {
            /* Pool exhausted, no expired entry found.
             * Force-evict the oldest entry in the target bucket. */
            entry = FC_CALL(FC_PREFIX, cache_evict_bucket_oldest)(fc, key);
            if (entry == NULL)
                return NULL;  /* both buckets empty - shouldn't happen */
        }
    } else {
        RIX_SLIST_REMOVE_HEAD(&fc->free_head, fc->pool, free_link);
    }

    entry->key     = *key;
    entry->last_ts = now;

    struct FC_ENTRY *dup =
        FC_CALL(FC_HT_PREFIX, insert)(&fc->ht_head, fc->buckets,
                                       fc->pool, entry);
    if (dup == NULL) {
        /* New entry placed successfully - let caller initialize payload. */
        FC_CALL(FC_PREFIX, call_init_cb)(fc, entry);
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
FC_IMPL_ATTR void
FC_FN(FC_PREFIX, cache_lookup_batch)(struct FC_CACHE *fc,
                           const struct FC_KEY *keys,
                           unsigned nb_pkts,
                           struct FC_ENTRY **results)
{
    struct rix_hash_find_ctx_s ctx[nb_pkts];
    uint64_t hit_count = 0;

    const unsigned ahead_keys = FLOW_CACHE_LOOKUP_AHEAD_KEYS;
    const unsigned step_keys = FLOW_CACHE_LOOKUP_STEP_KEYS;
    const unsigned total = nb_pkts + 3 * ahead_keys;

    for (unsigned i = 0; i < total; i += step_keys) {
        if (i < nb_pkts) {
            unsigned n = (i + step_keys <= nb_pkts) ? step_keys : (nb_pkts - i);
            for (unsigned j = 0; j < n; j++)
                FC_CALL(FC_HT_PREFIX, hash_key)(&ctx[i + j],
                                                  &fc->ht_head, fc->buckets,
                                                  &keys[i + j]);
        }

        if (i >= ahead_keys && i - ahead_keys < nb_pkts) {
            unsigned base = i - ahead_keys;
            unsigned n = (base + step_keys <= nb_pkts) ? step_keys : (nb_pkts - base);
            for (unsigned j = 0; j < n; j++)
                FC_CALL(FC_HT_PREFIX, scan_bk)(&ctx[base + j],
                                                &fc->ht_head, fc->buckets);
        }

        if (i >= 2 * ahead_keys && i - 2 * ahead_keys < nb_pkts) {
            unsigned base = i - 2 * ahead_keys;
            unsigned n = (base + step_keys <= nb_pkts) ? step_keys : (nb_pkts - base);
            for (unsigned j = 0; j < n; j++)
                FC_CALL(FC_HT_PREFIX, prefetch_node)(&ctx[base + j],
                                                      fc->pool);
        }

        if (i >= 3 * ahead_keys && i - 3 * ahead_keys < nb_pkts) {
            unsigned base = i - 3 * ahead_keys;
            unsigned n = (base + step_keys <= nb_pkts) ? step_keys : (nb_pkts - base);
            for (unsigned j = 0; j < n; j++) {
                struct FC_ENTRY *entry =
                    FC_CALL(FC_HT_PREFIX, cmp_key)(&ctx[base + j], fc->pool);
                results[base + j] = entry;
                if (entry != NULL) {
                    FC_CALL(FC_PREFIX, cache_prefetch_hit_write)(entry);
                    hit_count++;
                }
            }
        }
    }

    fc->stats.lookups += nb_pkts;
    fc->stats.hits += hit_count;
    fc->stats.misses += nb_pkts - hit_count;
}

/*===========================================================================
 * Aging expire - buffered bucket-prefetch pipeline (default)
 *
 * Scan count: fill-rate-driven (level 0..15, 64->1024).
 * Timeout: miss-rate-driven (eff_timeout_tsc, adjusted by caller).
 *
 * Prefetches entry CL0 ahead, and when an expired entry is detected,
 * prefetches its bucket and defers the actual remove by pf_dist iterations.
 * This hides bucket miss latency without storing extra per-entry metadata.
 *
 * Suitable as the default path across low and moderate eviction rates.
 * See FC_FN(FC_PREFIX, cache_expire_2stage) for the extra mid-distance
 * bucket-prefetch stage used at higher eviction pressure.
 *===========================================================================*/
FC_IMPL_ATTR void
FC_FN(FC_PREFIX, cache_expire)(struct FC_CACHE *fc,
                     uint64_t now)
{
    if (FC_CALL(FC_PREFIX, cache_use_expire_2stage)(fc)) {
        FC_CALL(FC_PREFIX, cache_expire_2stage)(fc, now);
        return;
    }

    unsigned max_scan = FC_CALL(FC_PREFIX, cache_expire_scan)(fc);
    uint64_t timeout  = fc->eff_timeout_tsc;
    const unsigned pf_dist = FC_CALL(FC_PREFIX, cache_expire_pf_dist)(fc);
    struct FC_ENTRY *pending[FLOW_CACHE_EXPIRE_PF_DIST] = { 0 };
    const unsigned mask = fc->entries_mask;
    const unsigned bk_mask = fc->ht_head.rhh_mask;
    unsigned cursor = fc->age_cursor;

    for (unsigned i = 0; i < max_scan + pf_dist; i++) {
        unsigned slot = i % pf_dist;

        if (pending[slot] != NULL) {
            struct FC_ENTRY *entry = pending[slot];

            FC_CALL(FC_HT_PREFIX, remove)(&fc->ht_head, fc->buckets,
                                           fc->pool, entry);
            FC_CALL(FC_PREFIX, call_fini_cb)(fc, entry,
                                             FLOW_CACHE_FINI_TIMEOUT);
            entry->last_ts = 0;

            RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
            fc->stats.evictions++;
            pending[slot] = NULL;
        }

        if (i >= max_scan)
            continue;

        /* SW prefetch: CL0 only (last_ts is in CL0) */
        unsigned pf_idx = (cursor + i + pf_dist) & mask;
        __builtin_prefetch(&fc->pool[pf_idx], 0, 1);

        struct FC_ENTRY *entry = &fc->pool[(cursor + i) & mask];

        if (entry->last_ts == 0)
            continue;
        if (now - entry->last_ts <= timeout)
            continue;

        /* Warm both bucket lines used by remove(): idx[] scan, then clears. */
        FC_CALL(FC_PREFIX, cache_prefetch_remove_bucket)(fc,
                                                         entry->cur_hash & bk_mask);
        pending[slot] = entry;
    }

    fc->age_cursor = (cursor + max_scan) & mask;
}

/*===========================================================================
 * Aging expire - 2-stage pipeline (bucket prefetch variant)
 *
 * Same adaptive 16-level pressure as the default expire path, but adds a
 * second bucket-prefetch stage: entries at mid-distance (pf_dist/2) are checked early
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
FC_IMPL_ATTR void
FC_FN(FC_PREFIX, cache_expire_2stage)(struct FC_CACHE *fc,
                            uint64_t now)
{
    unsigned max_scan = FC_CALL(FC_PREFIX, cache_expire_scan)(fc);
    uint64_t timeout  = fc->eff_timeout_tsc;
    const unsigned pf_dist    = FC_CALL(FC_PREFIX, cache_expire_pf_dist)(fc);
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
            FC_CALL(FC_PREFIX, cache_prefetch_remove_bucket)(fc, bk);
        }

        /* Stage 2: process current entry (bucket warm if expired) */
        struct FC_ENTRY *entry = &fc->pool[(cursor + i) & mask];

        if (entry->last_ts == 0)
            continue;
        if (now - entry->last_ts <= timeout)
            continue;

        FC_CALL(FC_HT_PREFIX, remove)(&fc->ht_head, fc->buckets,
                                       fc->pool, entry);
        FC_CALL(FC_PREFIX, call_fini_cb)(fc, entry, FLOW_CACHE_FINI_TIMEOUT);
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
FC_IMPL_ATTR struct FC_ENTRY *
FC_FN(FC_PREFIX, cache_find)(struct FC_CACHE *fc,
                   const struct FC_KEY *key)
{
    struct rix_hash_find_ctx_s ctx;
    FC_CALL(FC_HT_PREFIX, hash_key)(&ctx, &fc->ht_head, fc->buckets, key);
    FC_CALL(FC_HT_PREFIX, scan_bk)(&ctx, &fc->ht_head, fc->buckets);
    FC_CALL(FC_HT_PREFIX, prefetch_node)(&ctx, fc->pool);
    return FC_CALL(FC_HT_PREFIX, cmp_key)(&ctx, fc->pool);
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
FC_IMPL_ATTR void
FC_FN(FC_PREFIX, cache_remove)(struct FC_CACHE *fc,
                     struct FC_ENTRY *entry)
{
    FC_CALL(FC_HT_PREFIX, remove)(&fc->ht_head, fc->buckets,
                                   fc->pool, entry);
    FC_CALL(FC_PREFIX, call_fini_cb)(fc, entry, FLOW_CACHE_FINI_REMOVED);
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
FC_IMPL_ATTR void
FC_FN(FC_PREFIX, cache_flush)(struct FC_CACHE *fc)
{
    /* Notify caller for each active entry before wiping the pool. */
    for (unsigned i = 0; i < fc->max_entries; i++) {
        if (fc->pool[i].last_ts != 0)
            FC_CALL(FC_PREFIX, call_fini_cb)(fc, &fc->pool[i],
                                             FLOW_CACHE_FINI_FLUSHED);
    }

    unsigned flushed = fc->ht_head.rhh_nb;

    FC_CALL(FC_HT_PREFIX, init)(&fc->ht_head, fc->nb_bk);
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
FC_IMPL_ATTR void
FC_FN(FC_PREFIX, cache_stats)(const struct FC_CACHE *fc,
                    struct flow_cache_stats *out)
{
    *out = fc->stats;
    out->nb_entries  = fc->ht_head.rhh_nb;
    out->max_entries = fc->max_entries;
}

/* clean up local macros */
#undef FC_FN
#undef FC_FN_CAT
#undef _FC_FN_CAT
#undef FC_IMPL_ATTR

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
