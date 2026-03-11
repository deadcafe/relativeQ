/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <string.h>
#include "flow4_cache.h"

/*===========================================================================
 * Init
 *===========================================================================*/
void
flow4_cache_init(struct flow4_cache *fc,
                 struct rix_hash_bucket_s *buckets,
                 unsigned nb_bk,
                 struct flow4_entry *pool,
                 unsigned pool_cap,
                 uint64_t timeout_ms)
{
    memset(fc, 0, sizeof(*fc));

    /* ensure arch dispatch is initialized in this TU */
    rix_hash_arch_init();

    /* hash table */
    flow4_ht_init(&fc->ht_head, nb_bk);
    fc->buckets = buckets;
    fc->nb_bk   = nb_bk;
    memset(buckets, 0, nb_bk * sizeof(*buckets));

    /* node pool */
    fc->pool     = pool;
    fc->pool_cap = pool_cap;
    memset(pool, 0, pool_cap * sizeof(*pool));

    /* free list: push all entries */
    RIX_SLIST_INIT(&fc->free_head);
    for (unsigned i = 0; i < pool_cap; i++) {
        RIX_SLIST_INSERT_HEAD(&fc->free_head, pool, &pool[i], free_link);
    }

    /* aging */
    fc->age_cursor = 0;
    if (timeout_ms > 0) {
        uint64_t tsc_hz = flow_cache_calibrate_tsc_hz();
        fc->timeout_tsc = flow_cache_ms_to_tsc(tsc_hz, timeout_ms);
    } else {
        fc->timeout_tsc = UINT64_MAX;  /* no expiry */
    }
}

/*===========================================================================
 * Evict one expired entry (for insert on free-list exhaustion).
 * Scans up to pool_cap entries from age_cursor.
 * Returns evicted entry (removed from hash, flags cleared), or NULL.
 *===========================================================================*/
static struct flow4_entry *
flow4_cache_evict_one(struct flow4_cache *fc, uint64_t now)
{
    for (unsigned i = 0; i < fc->pool_cap; i++) {
        unsigned idx = fc->age_cursor % fc->pool_cap;
        fc->age_cursor++;

        struct flow4_entry *entry = &fc->pool[idx];
        if (!(entry->flags & FLOW4_FLAG_VALID))
            continue;
        if (now - entry->last_ts <= fc->timeout_tsc)
            continue;

        /* expired — remove from hash table */
        flow4_ht_remove(&fc->ht_head, fc->buckets, fc->pool, entry);
        entry->flags = 0;
        fc->stats.evictions++;
        return entry;
    }
    return NULL;  /* all entries are live */
}

/*===========================================================================
 * Insert (on miss, action = NONE)
 *===========================================================================*/
struct flow4_entry *
flow4_cache_insert(struct flow4_cache *fc,
                   const struct flow4_key *key,
                   uint64_t now)
{
    /* pop from free list */
    struct flow4_entry *entry = RIX_SLIST_FIRST(&fc->free_head, fc->pool);

    if (entry == NULL) {
        /* free list exhausted — try to evict one expired entry */
        entry = flow4_cache_evict_one(fc, now);
        if (entry == NULL)
            return NULL;   /* all entries are live, insert fails */
    } else {
        RIX_SLIST_REMOVE_HEAD(&fc->free_head, fc->pool, free_link);
    }

    /* initialize entry */
    entry->key       = *key;
    entry->action    = FLOW_ACTION_NONE;
    entry->qos_class = 0;
    entry->flags     = FLOW4_FLAG_VALID;
    entry->last_ts   = now;
    entry->packets   = 0;
    entry->bytes     = 0;

    /* insert into hash table */
    struct flow4_entry *dup =
        flow4_ht_insert(&fc->ht_head, fc->buckets, fc->pool, entry);
    if (dup == NULL) {
        /* success */
        fc->stats.inserts++;
        return entry;
    }
    if (dup == entry) {
        /* table full (kickout exhausted) — return entry to free list */
        entry->flags = 0;
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
        return NULL;
    }
    /* duplicate key found — return entry to free list, return existing */
    entry->flags = 0;
    RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
    dup->last_ts = now;
    return dup;
}

/*===========================================================================
 * Pipelined batch lookup
 *===========================================================================*/
void
flow4_cache_lookup_batch(struct flow4_cache *fc,
                         const struct flow4_key *keys,
                         unsigned nb_pkts,
                         struct flow4_entry **results)
{
    struct rix_hash_find_ctx_s ctx[nb_pkts];

    /*
     * N-ahead software pipeline:
     *
     *   hash_key  runs DIST steps ahead of scan_bk
     *   scan_bk   runs DIST steps ahead of prefetch_node
     *   prefetch_node runs DIST steps ahead of cmp_key
     *
     * DIST = FLOW_CACHE_BATCH * FLOW_CACHE_KPD = 64
     */
    const unsigned DIST = FLOW_CACHE_DIST;
    const unsigned BATCH = FLOW_CACHE_BATCH;
    const unsigned total = nb_pkts + 3 * DIST;

    for (unsigned i = 0; i < total; i += BATCH) {
        /* Stage 0: hash_key — prefetch buckets */
        if (i < nb_pkts) {
            unsigned n = (i + BATCH <= nb_pkts) ? BATCH : (nb_pkts - i);
            for (unsigned j = 0; j < n; j++)
                flow4_ht_hash_key(&ctx[i + j],
                                  &fc->ht_head, fc->buckets,
                                  &keys[i + j]);
        }

        /* Stage 1: scan_bk — fingerprint match */
        if (i >= DIST && i - DIST < nb_pkts) {
            unsigned base = i - DIST;
            unsigned n = (base + BATCH <= nb_pkts) ? BATCH : (nb_pkts - base);
            for (unsigned j = 0; j < n; j++)
                flow4_ht_scan_bk(&ctx[base + j],
                                 &fc->ht_head, fc->buckets);
        }

        /* Stage 2: prefetch_node — prefetch flow entry CL0 */
        if (i >= 2 * DIST && i - 2 * DIST < nb_pkts) {
            unsigned base = i - 2 * DIST;
            unsigned n = (base + BATCH <= nb_pkts) ? BATCH : (nb_pkts - base);
            for (unsigned j = 0; j < n; j++)
                flow4_ht_prefetch_node(&ctx[base + j], fc->pool);
        }

        /* Stage 3: cmp_key — full key comparison */
        if (i >= 3 * DIST && i - 3 * DIST < nb_pkts) {
            unsigned base = i - 3 * DIST;
            unsigned n = (base + BATCH <= nb_pkts) ? BATCH : (nb_pkts - base);
            for (unsigned j = 0; j < n; j++)
                results[base + j] = flow4_ht_cmp_key(&ctx[base + j],
                                                      fc->pool);
        }
    }

    /* update stats */
    fc->stats.lookups += nb_pkts;
    for (unsigned i = 0; i < nb_pkts; i++) {
        if (results[i])
            fc->stats.hits++;
        else
            fc->stats.misses++;
    }
}

/*===========================================================================
 * Aging expire
 *===========================================================================*/
void
flow4_cache_expire(struct flow4_cache *fc,
                   uint64_t now,
                   unsigned max_expire)
{
    for (unsigned i = 0; i < max_expire; i++) {
        unsigned idx = fc->age_cursor % fc->pool_cap;
        fc->age_cursor++;

        struct flow4_entry *entry = &fc->pool[idx];
        if (!(entry->flags & FLOW4_FLAG_VALID))
            continue;
        if (now - entry->last_ts <= fc->timeout_tsc)
            continue;

        /* expired — remove from hash table */
        flow4_ht_remove(&fc->ht_head, fc->buckets, fc->pool, entry);
        entry->flags = 0;

        /* return to free list */
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);
        fc->stats.evictions++;
    }
}

/*===========================================================================
 * Statistics
 *===========================================================================*/
void
flow4_cache_stats(const struct flow4_cache *fc,
                  struct flow_cache_stats *out)
{
    *out = fc->stats;
    out->nb_entries = fc->ht_head.rhh_nb;
    out->pool_cap   = fc->pool_cap;
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
