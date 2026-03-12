/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_cache_common.h — Macro-templated flow cache declarations.
 *
 * Before including this file, define:
 *   FC_PREFIX        e.g. flow4
 *   FC_ENTRY         e.g. flow4_entry
 *   FC_KEY           e.g. flow4_key
 *   FC_CACHE         e.g. flow4_cache
 *   FC_HT            e.g. flow4_ht        (hash table head struct tag)
 *   FC_FREE_HEAD     e.g. flow4_free_head
 *
 * The entry type (struct FC_ENTRY) and hash table (RIX_HASH_HEAD/GENERATE)
 * must already be defined before including this file.
 */

#include "flow_cache.h"

/* token-paste helpers */
#define _FCC_CAT(a, b)   a ## b
#define FCC_CAT(a, b)    _FCC_CAT(a, b)
#define FCC_FN(suffix)   FCC_CAT(FC_PREFIX, suffix)

/*===========================================================================
 * Free list head + cache struct
 *===========================================================================*/
RIX_SLIST_HEAD(FC_FREE_HEAD, FC_ENTRY);

struct FC_CACHE {
    /* hash table */
    struct FC_HT                 ht_head;
    struct rix_hash_bucket_s    *buckets;
    unsigned                     nb_bk;

    /* node pool (max_entries must be power of 2) */
    struct FC_ENTRY             *pool;
    unsigned                     max_entries;
    unsigned                     entries_mask;  /* max_entries - 1 */

    /* free list */
    struct FC_FREE_HEAD          free_head;

    /* aging */
    unsigned                     age_cursor;
    uint64_t                     timeout_tsc;     /* base timeout (configured) */
    uint64_t                     eff_timeout_tsc; /* effective timeout (auto-adjusted) */
    uint64_t                     min_timeout_tsc; /* minimum timeout floor */

    /* stats */
    struct flow_cache_stats      stats;
};

/*===========================================================================
 * API declarations
 *===========================================================================*/
void FCC_FN(_cache_init)(struct FC_CACHE *fc,
                         struct rix_hash_bucket_s *buckets,
                         unsigned nb_bk,
                         struct FC_ENTRY *pool,
                         unsigned max_entries,
                         uint64_t timeout_ms);

void FCC_FN(_cache_lookup_batch)(struct FC_CACHE *fc,
                                 const struct FC_KEY *keys,
                                 unsigned nb_pkts,
                                 struct FC_ENTRY **results);

struct FC_ENTRY *FCC_FN(_cache_insert)(struct FC_CACHE *fc,
                                       const struct FC_KEY *key,
                                       uint64_t now);

void FCC_FN(_cache_expire)(struct FC_CACHE *fc,
                           uint64_t now);

void FCC_FN(_cache_expire_2stage)(struct FC_CACHE *fc,
                                   uint64_t now);

void FCC_FN(_cache_stats)(const struct FC_CACHE *fc,
                          struct flow_cache_stats *out);

/*===========================================================================
 * Inline helpers
 *===========================================================================*/
static inline void
FCC_FN(_cache_update_action)(struct FC_ENTRY *entry,
                             uint32_t action,
                             uint32_t qos_class)
{
    entry->action    = action;
    entry->qos_class = qos_class;
}

static inline void
FCC_FN(_cache_touch)(struct FC_ENTRY *entry, uint64_t now,
                     uint32_t pkt_len)
{
    entry->last_ts = now;
    entry->packets++;
    entry->bytes += pkt_len;
}

/*
 * Adaptive expire: scan from fill-rate level, timeout from miss-rate.
 *
 * Scan count: fill-rate-driven (level 0..15, scan 64→1024).
 * Timeout:    miss-rate-driven smooth adjustment (no oscillation).
 *
 * Scan level computation uses shifts (max_entries is power of 2):
 *   excess = nb - max/2
 *   level  = excess >> (log2(max) - 4)
 *   level 0-3: scan  64..512,  level >= 4: scan 1024
 */
static inline unsigned
FCC_FN(_cache_expire_level)(const struct FC_CACHE *fc)
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
FCC_FN(_cache_expire_scan)(const struct FC_CACHE *fc)
{
    unsigned level = FCC_FN(_cache_expire_level)(fc);

    if (level >= 4)
        return FLOW_CACHE_EXPIRE_SCAN_MAX;

    return FLOW_CACHE_EXPIRE_SCAN_MIN << level;
}

/*
 * Miss-rate-driven timeout adjustment.
 *
 * Called after each batch with the number of misses (new flows).
 * - Each miss decays eff_timeout by misses/(batch*16) fraction.
 * - Recovery: eff_timeout slowly restores toward base timeout.
 * - At steady state: decay == recovery → stable timeout.
 *
 * All shift/multiply, no division.
 *
 * FLOW_CACHE_TIMEOUT_DECAY_SHIFT controls decay sensitivity.
 *   batch=256 → log2(256)+4 = 12.  Larger = gentler.
 * FLOW_CACHE_TIMEOUT_RECOVER_SHIFT controls recovery speed.
 *   8 = recover 1/256 of gap per batch.  Larger = slower.
 */
static inline void
FCC_FN(_cache_adjust_timeout)(struct FC_CACHE *fc,
                               unsigned misses)
{
    uint64_t eff = fc->eff_timeout_tsc;
    uint64_t base = fc->timeout_tsc;
    uint64_t min_to = fc->min_timeout_tsc;

    /* decay: proportional to miss count */
    if (misses > 0) {
        uint64_t decay = (eff * misses) >> FLOW_CACHE_TIMEOUT_DECAY_SHIFT;
        if (decay == 0)
            decay = 1;
        if (eff > min_to + decay)
            eff -= decay;
        else
            eff = min_to;
    }

    /* recovery: slowly restore toward base */
    eff += (base - eff) >> FLOW_CACHE_TIMEOUT_RECOVER_SHIFT;

    fc->eff_timeout_tsc = eff;
}

/* clean up local macros */
#undef FCC_FN
#undef FCC_CAT
#undef _FCC_CAT

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
