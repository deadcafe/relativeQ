/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_cache_common.h - Macro-templated flow cache declarations.
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

#include "flow_cache_defs.h"

/* token-paste helpers */
#define _FCC_CAT(a, b, c)      a ## b ## c
#define FCC_CAT(a, b, c)       _FCC_CAT(a, b, c)
#define FCC_FN(prefix, suffix) FCC_CAT(prefix, _, suffix)

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
 *
 * Typical packet processing loop:
 *
 *   uint64_t now = flow_cache_rdtsc();
 *
 *   // 1. Pipelined batch lookup (DRAM-latency hiding)
 *   PREFIX_cache_lookup_batch(&fc, keys, nb_pkts, results);
 *
 *   // 2. Per-packet post-processing
 *   unsigned misses = 0;
 *   for (unsigned i = 0; i < nb_pkts; i++) {
 *       if (results[i]) {
 *           PREFIX_cache_touch(results[i], now, pkt_len[i]);  // hit
 *       } else {
 *           struct ENTRY *e = PREFIX_cache_insert(&fc, &keys[i], now); // miss
 *           if (e) { e->action = slow_path(&keys[i]); }
 *           misses++;
 *       }
 *   }
 *
 *   // 3. Adaptive timeout adjustment (call every batch)
 *   PREFIX_cache_adjust_timeout(&fc, misses);
 *
 *   // 4. Aging eviction (call every batch or every N batches)
 *   PREFIX_cache_expire(&fc, now);
 *
 *   // 5. Explicit remove (e.g., TCP FIN/RST, admin teardown)
 *   PREFIX_cache_remove(&fc, entry);
 *
 *   // 6. Bulk clear (e.g., VRF delete, interface down)
 *   PREFIX_cache_flush(&fc);
 *===========================================================================*/

/* Lifecycle */
void FCC_FN(FC_PREFIX, cache_init)(struct FC_CACHE *fc,
                         struct rix_hash_bucket_s *buckets,
                         unsigned nb_bk,
                         struct FC_ENTRY *pool,
                         unsigned max_entries,
                         uint64_t timeout_ms);

void FCC_FN(FC_PREFIX, cache_flush)(struct FC_CACHE *fc);

/* Lookup */
void FCC_FN(FC_PREFIX, cache_lookup_batch)(struct FC_CACHE *fc,
                                 const struct FC_KEY *keys,
                                 unsigned nb_pkts,
                                 struct FC_ENTRY **results);

struct FC_ENTRY *FCC_FN(FC_PREFIX, cache_find)(struct FC_CACHE *fc,
                                     const struct FC_KEY *key);

/* Mutation */
struct FC_ENTRY *FCC_FN(FC_PREFIX, cache_insert)(struct FC_CACHE *fc,
                                       const struct FC_KEY *key,
                                       uint64_t now);

void FCC_FN(FC_PREFIX, cache_remove)(struct FC_CACHE *fc,
                            struct FC_ENTRY *entry);

/* Aging */
void FCC_FN(FC_PREFIX, cache_expire)(struct FC_CACHE *fc, uint64_t now);

/*
 * expire_2stage: bucket-prefetch variant of expire.
 *
 * Adds a mid-distance stage that prefetches the hash bucket before
 * _remove(), warming it when the pool >> LLC or eviction rate is high.
 * Has extra overhead (stage-1 check) that hurts when buckets are already
 * warm (small pool) or eviction is rare.  Prefer expire() by default.
 */
void FCC_FN(FC_PREFIX, cache_expire_2stage)(struct FC_CACHE *fc, uint64_t now);

/* Statistics */
void FCC_FN(FC_PREFIX, cache_stats)(const struct FC_CACHE *fc,
                          struct flow_cache_stats *out);

/*===========================================================================
 * Inline helpers
 *===========================================================================*/

/* Current number of active entries. */
static inline unsigned
FCC_FN(FC_PREFIX, cache_nb_entries)(const struct FC_CACHE *fc)
{
    return fc->ht_head.rhh_nb;
}

/* Update cached slow-path result for a hit entry. */
static inline void
FCC_FN(FC_PREFIX, cache_update_action)(struct FC_ENTRY *entry,
                             uint32_t action,
                             uint32_t qos_class)
{
    entry->action    = action;
    entry->qos_class = qos_class;
}

/* Record a packet hit: refresh timestamp and accumulate counters. */
static inline void
FCC_FN(FC_PREFIX, cache_touch)(struct FC_ENTRY *entry, uint64_t now,
                     uint32_t pkt_len)
{
    entry->last_ts = now;
    entry->packets++;
    entry->bytes += pkt_len;
}

/*
 * Miss-rate-driven timeout adjustment.
 *
 * Call once per batch, passing the number of misses (new flows) in that
 * batch.  High miss rate shortens eff_timeout_tsc (more aggressive aging);
 * quiet periods let it recover toward the configured base timeout.
 *
 * All shift/multiply, no division.
 *   FLOW_CACHE_TIMEOUT_DECAY_SHIFT   controls decay sensitivity (larger = gentler).
 *   FLOW_CACHE_TIMEOUT_RECOVER_SHIFT controls recovery speed    (larger = slower).
 */
static inline void
FCC_FN(FC_PREFIX, cache_adjust_timeout)(struct FC_CACHE *fc, unsigned misses)
{
    uint64_t eff    = fc->eff_timeout_tsc;
    uint64_t base   = fc->timeout_tsc;
    uint64_t min_to = fc->min_timeout_tsc;

    if (misses > 0) {
        uint64_t decay = (eff * misses) >> FLOW_CACHE_TIMEOUT_DECAY_SHIFT;
        if (decay == 0)
            decay = 1;
        eff = (eff > min_to + decay) ? eff - decay : min_to;
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
