/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW4_CACHE2_H_
#define _FLOW4_CACHE2_H_

#include <stdint.h>
#include <rix/rix_hash.h>
#include <rix/rix_queue.h>

#define FC2_CACHE_LINE_SIZE 64u
#ifndef FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS 1u
#endif

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

struct fc2_flow4_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
} __attribute__((packed));

struct fc2_flow4_result {
    uint32_t entry_idx; /* 1-origin; 0 = miss / full */
};

struct fc2_flow4_entry {
    /* Single-cache-line flow4 entry used by the current fc2 prototype. */
    struct fc2_flow4_key   key;          /* 20B */
    uint32_t               cur_hash;     /* current-bucket hash */
    uint64_t               last_ts;      /* 0 = free / invalid */
    RIX_SLIST_ENTRY(struct fc2_flow4_entry) free_link;
    uint16_t               slot;         /* slot in current bucket */
    uint16_t               reserved1;
    uint8_t                reserved0[24];
} __attribute__((aligned(FC2_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct fc2_flow4_entry) == FC2_CACHE_LINE_SIZE,
                  "fc2_flow4_entry must be 64 bytes");

RIX_HASH_HEAD(fc2_flow4_ht);
RIX_SLIST_HEAD(fc2_flow4_free_head, fc2_flow4_entry);

struct fc2_flow4_config {
    uint64_t timeout_tsc;
    unsigned pressure_empty_slots;
};

struct fc2_flow4_stats {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t fills;
    uint64_t fill_full;
    uint64_t relief_calls;
    uint64_t relief_bucket_checks;
    uint64_t relief_evictions;
    uint64_t relief_bk0_evictions;
    uint64_t relief_bk1_evictions;
    uint64_t maint_calls;
    uint64_t maint_bucket_checks;
    uint64_t maint_evictions;
};

struct fc2_flow4_cache {
    struct fc2_flow4_ht        ht_head;
    struct rix_hash_bucket_s  *buckets;
    struct fc2_flow4_entry    *pool;
    unsigned                   nb_bk;
    unsigned                   max_entries;
    unsigned                   total_slots;
    uint64_t                   timeout_tsc;
    uint64_t                   eff_timeout_tsc;
    unsigned                   pressure_empty_slots;
    unsigned                   timeout_lo_entries;
    unsigned                   timeout_hi_entries;
    uint64_t                   timeout_min_tsc;
    unsigned                   relief_mid_entries;
    unsigned                   relief_hi_entries;
    struct fc2_flow4_stats     stats;
    struct fc2_flow4_free_head free_head;
};

int fc2_flow4_cmp(const void *a, const void *b);
void fc2_flow4_cache_init(struct fc2_flow4_cache *fc,
                          struct rix_hash_bucket_s *buckets,
                          unsigned nb_bk,
                          struct fc2_flow4_entry *pool,
                          unsigned max_entries,
                          const struct fc2_flow4_config *cfg);
void fc2_flow4_cache_flush(struct fc2_flow4_cache *fc);
unsigned fc2_flow4_cache_nb_entries(const struct fc2_flow4_cache *fc);
unsigned fc2_flow4_cache_lookup_batch(struct fc2_flow4_cache *fc,
                                      const struct fc2_flow4_key *keys,
                                      unsigned nb_keys,
                                      uint64_t now,
                                      struct fc2_flow4_result *results,
                                      uint16_t *miss_idx);
unsigned fc2_flow4_cache_fill_miss_batch(struct fc2_flow4_cache *fc,
                                         const struct fc2_flow4_key *keys,
                                         const uint16_t *miss_idx,
                                         unsigned miss_count,
                                         uint64_t now,
                                         struct fc2_flow4_result *results);
unsigned fc2_flow4_cache_maintain(struct fc2_flow4_cache *fc,
                                  unsigned start_bk,
                                  unsigned bucket_count,
                                  uint64_t now);
int fc2_flow4_cache_remove_idx(struct fc2_flow4_cache *fc, uint32_t entry_idx);
void fc2_flow4_cache_stats(const struct fc2_flow4_cache *fc,
                           struct fc2_flow4_stats *out);

#endif /* _FLOW4_CACHE2_H_ */
