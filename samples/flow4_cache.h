/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW4_CACHE_H_
#define _FLOW4_CACHE_H_

#include <rix/rix_defs.h>
#include <rix/rix_queue.h>
#include <rix/rix_hash.h>

#include "flow_cache.h"

/*===========================================================================
 * IPv4 flow key: 5-tuple + vrfid (20 bytes)
 *===========================================================================*/
struct flow4_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
};

/*===========================================================================
 * IPv4 flow entry (128 bytes = 2 cache lines)
 *
 * CL0 (0-63):  lookup hot path — accessed during pipelined find
 * CL1 (64-127): counters & management — accessed on hit / eviction
 *===========================================================================*/
#define FLOW4_FLAG_VALID   (1u << 0)

struct flow4_entry {
    /* --- CL0: lookup hot path (64 bytes) --- */
    struct flow4_key     key;           /* 20B */
    uint32_t             cur_hash;      /*  4B: hash_field for O(1) remove */
    uint32_t             action;        /*  4B: cached ACL result */
    uint32_t             qos_class;     /*  4B: cached QoS class */
    uint32_t             flags;         /*  4B: FLOW4_FLAG_VALID, etc. */
    uint32_t             reserved0[7];  /* 28B: pad to 64B */

    /* --- CL1: counters & management (64 bytes) --- */
    uint64_t             last_ts;       /*  8B: last access TSC */
    uint64_t             packets;       /*  8B */
    uint64_t             bytes;         /*  8B */
    RIX_SLIST_ENTRY(struct flow4_entry) free_link;  /* 4B: free list */
    uint32_t             reserved1[9];  /* 36B: pad to 64B */
} __attribute__((aligned(64)));

/*===========================================================================
 * Key comparison function for RIX_HASH_GENERATE
 *===========================================================================*/
static inline int
flow4_cmp(const void *a, const void *b)
{
    const struct flow4_key *ka = (const struct flow4_key *)a;
    const struct flow4_key *kb = (const struct flow4_key *)b;

    return (ka->src_ip   == kb->src_ip   &&
            ka->dst_ip   == kb->dst_ip   &&
            ka->src_port == kb->src_port &&
            ka->dst_port == kb->dst_port &&
            ka->proto    == kb->proto    &&
            ka->vrfid    == kb->vrfid);
}

/*===========================================================================
 * Generate hash table functions: flow4_ht_*
 *===========================================================================*/
RIX_HASH_HEAD(flow4_ht);
RIX_HASH_GENERATE(flow4_ht, flow4_entry, key, cur_hash, flow4_cmp)

/*===========================================================================
 * Flow cache instance (per-thread)
 *===========================================================================*/
RIX_SLIST_HEAD(flow4_free_head, flow4_entry);

struct flow4_cache {
    /* hash table */
    struct flow4_ht              ht_head;
    struct rix_hash_bucket_s    *buckets;
    unsigned                     nb_bk;

    /* node pool */
    struct flow4_entry          *pool;
    unsigned                     pool_cap;

    /* free list */
    struct flow4_free_head       free_head;

    /* aging */
    unsigned                     age_cursor;
    uint64_t                     timeout_tsc;

    /* stats */
    struct flow_cache_stats      stats;
};

/*===========================================================================
 * API
 *===========================================================================*/

/* Initialize cache.  Caller provides pre-allocated memory:
 *   buckets: nb_bk × sizeof(struct rix_hash_bucket_s), aligned 64B
 *   pool:    pool_cap × sizeof(struct flow4_entry), aligned 64B
 *   timeout_ms: idle timeout in milliseconds (0 = no expiry)
 */
void flow4_cache_init(struct flow4_cache *fc,
                      struct rix_hash_bucket_s *buckets,
                      unsigned nb_bk,
                      struct flow4_entry *pool,
                      unsigned pool_cap,
                      uint64_t timeout_ms);

/* Pipelined batch lookup.
 * results[i] = hit entry or NULL (miss).
 */
void flow4_cache_lookup_batch(struct flow4_cache *fc,
                              const struct flow4_key *keys,
                              unsigned nb_pkts,
                              struct flow4_entry **results);

/* Insert on miss (action = FLOW_ACTION_NONE).
 * Returns inserted entry, or NULL if pool exhausted.
 */
struct flow4_entry *flow4_cache_insert(struct flow4_cache *fc,
                                       const struct flow4_key *key,
                                       uint64_t now);

/* Update action after slow-path ACL/QoS lookup. */
static inline void
flow4_cache_update_action(struct flow4_entry *entry,
                          uint32_t action,
                          uint32_t qos_class)
{
    entry->action    = action;
    entry->qos_class = qos_class;
}

/* Touch: update timestamp + counters on hit. */
static inline void
flow4_cache_touch(struct flow4_entry *entry, uint64_t now,
                  uint32_t pkt_len)
{
    entry->last_ts = now;
    entry->packets++;
    entry->bytes += pkt_len;
}

/* Check if hash table fill rate exceeds ~75% threshold.
 * Uses shift: total_slots - (total_slots >> 2) = 3/4 * total_slots.
 * Caller should call flow4_cache_expire() when this returns true.
 */
static inline int
flow4_cache_over_threshold(const struct flow4_cache *fc)
{
    unsigned total_slots = (fc->ht_head.rhh_mask + 1u) << 4;
    return fc->ht_head.rhh_nb >= total_slots - (total_slots >> 2);
}

/* Aging expire: scan up to max_expire entries from cursor. */
void flow4_cache_expire(struct flow4_cache *fc,
                        uint64_t now,
                        unsigned max_expire);

/* Get statistics snapshot. */
void flow4_cache_stats(const struct flow4_cache *fc,
                       struct flow_cache_stats *out);

#endif /* _FLOW4_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
