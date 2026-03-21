/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW6_CACHE_H_
#define _FLOW6_CACHE_H_

#include <stdint.h>
#include <rix/rix_hash.h>
#include <rix/rix_queue.h>

#ifndef FC_CACHE_LINE_SIZE
#define FC_CACHE_LINE_SIZE 64u
#endif
#ifndef FC_FLOW6_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FC_FLOW6_DEFAULT_PRESSURE_EMPTY_SLOTS 1u
#endif

struct fc_flow6_key {
    uint8_t  src_ip[16];
    uint8_t  dst_ip[16];
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
} __attribute__((packed));

struct fc_flow6_result {
    uint32_t entry_idx; /* 1-origin; 0 = miss / full */
};

struct fc_flow6_entry {
    struct fc_flow6_key   key;          /* 44B */
    uint32_t               cur_hash;     /* current-bucket hash */
    uint64_t               last_ts;      /* 0 = free / invalid */
    RIX_SLIST_ENTRY(struct fc_flow6_entry) free_link;
    uint16_t               slot;         /* slot in current bucket */
    uint16_t               reserved1;
} __attribute__((aligned(FC_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct fc_flow6_entry) == FC_CACHE_LINE_SIZE,
                  "fc_flow6_entry must be 64 bytes");

RIX_HASH_HEAD(fc_flow6_ht);
RIX_SLIST_HEAD(fc_flow6_free_head, fc_flow6_entry);

struct fc_flow6_config {
    uint64_t timeout_tsc;
    unsigned pressure_empty_slots;
    uint64_t maint_interval_tsc;
    unsigned maint_base_bk;
    unsigned maint_fill_threshold;
};

struct fc_flow6_stats {
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
    uint64_t maint_step_calls;
    uint64_t maint_step_skipped_bks;
};

struct fc_flow6_cache {
    /* --- CL0: lookup / fill hot path --- */
    struct rix_hash_bucket_s  *buckets;
    struct fc_flow6_entry    *pool;
    struct fc_flow6_ht        ht_head;
    uint64_t                   timeout_tsc;
    uint64_t                   eff_timeout_tsc;
    uint64_t                   timeout_min_tsc;
    unsigned                   nb_bk;
    unsigned                   max_entries;
    unsigned                   total_slots;
    unsigned                   pressure_empty_slots;
    /* --- CL1 --- */
    unsigned                   timeout_lo_entries;
    unsigned                   timeout_hi_entries;
    unsigned                   relief_mid_entries;
    unsigned                   relief_hi_entries;
    unsigned                   maint_cursor;
    uint64_t                   last_maint_tsc;
    uint64_t                   last_maint_fills;
    uint64_t                   maint_interval_tsc;
    unsigned                   maint_base_bk;
    unsigned                   maint_fill_threshold;
    unsigned                   last_maint_start_bk;
    unsigned                   last_maint_sweep_bk;
    struct fc_flow6_free_head free_head;
    struct fc_flow6_stats     stats;
};

void fc_flow6_cache_init(struct fc_flow6_cache *fc,
                          struct rix_hash_bucket_s *buckets,
                          unsigned nb_bk,
                          struct fc_flow6_entry *pool,
                          unsigned max_entries,
                          const struct fc_flow6_config *cfg);
void fc_flow6_cache_flush(struct fc_flow6_cache *fc);
unsigned fc_flow6_cache_nb_entries(const struct fc_flow6_cache *fc);
void fc_flow6_cache_lookup_batch(struct fc_flow6_cache *fc,
                                  const struct fc_flow6_key *keys,
                                  unsigned nb_keys,
                                  uint64_t now,
                                  struct fc_flow6_result *results);
unsigned fc_flow6_cache_maintain(struct fc_flow6_cache *fc,
                                  unsigned start_bk,
                                  unsigned bucket_count,
                                  uint64_t now);
unsigned fc_flow6_cache_maintain_step_ex(struct fc_flow6_cache *fc,
                                          unsigned start_bk,
                                          unsigned bucket_count,
                                          unsigned skip_threshold,
                                          uint64_t now);
unsigned fc_flow6_cache_maintain_step(struct fc_flow6_cache *fc,
                                       uint64_t now,
                                       int idle);
int fc_flow6_cache_remove_idx(struct fc_flow6_cache *fc, uint32_t entry_idx);
void fc_flow6_cache_stats(const struct fc_flow6_cache *fc,
                           struct fc_flow6_stats *out);

#endif /* _FLOW6_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
