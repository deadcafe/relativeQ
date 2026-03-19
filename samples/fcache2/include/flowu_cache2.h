/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flowu_cache2.h - Unified IPv4/IPv6 flow cache (fcache2).
 *
 * Single hash table handles both address families.
 * Key includes family field so v4/v6 entries never collide.
 */

#ifndef _FLOWU_CACHE2_H_
#define _FLOWU_CACHE2_H_

#include <stdint.h>
#include <string.h>
#include <rix/rix_hash.h>
#include <rix/rix_queue.h>

#ifndef FC2_CACHE_LINE_SIZE
#define FC2_CACHE_LINE_SIZE 64u
#endif
#ifndef FC2_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FC2_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS 1u
#endif

#define FC2_FLOW_FAMILY_IPV4  4
#define FC2_FLOW_FAMILY_IPV6  6

struct fc2_flowu_key {
    uint8_t  family;        /*  1B: FC2_FLOW_FAMILY_IPV4 / IPV6 */
    uint8_t  proto;         /*  1B */
    uint16_t src_port;      /*  2B */
    uint16_t dst_port;      /*  2B */
    uint16_t pad;           /*  2B */
    uint32_t vrfid;         /*  4B */
    union {                 /* 32B */
        struct {
            uint32_t src;
            uint32_t dst;
            uint8_t  _pad[24];
        } v4;
        struct {
            uint8_t  src[16];
            uint8_t  dst[16];
        } v6;
    } addr;
} __attribute__((packed));  /* 44B total */

/*===========================================================================
 * Key construction helpers
 *===========================================================================*/
static inline struct fc2_flowu_key
fc2_flowu_key_v4(uint32_t src_ip, uint32_t dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t proto, uint32_t vrfid)
{
    struct fc2_flowu_key k;
    memset(&k, 0, sizeof(k));
    k.family   = FC2_FLOW_FAMILY_IPV4;
    k.proto    = proto;
    k.src_port = src_port;
    k.dst_port = dst_port;
    k.vrfid    = vrfid;
    k.addr.v4.src = src_ip;
    k.addr.v4.dst = dst_ip;
    return k;
}

static inline struct fc2_flowu_key
fc2_flowu_key_v6(const uint8_t *src_ip, const uint8_t *dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t proto, uint32_t vrfid)
{
    struct fc2_flowu_key k;
    memset(&k, 0, sizeof(k));
    k.family   = FC2_FLOW_FAMILY_IPV6;
    k.proto    = proto;
    k.src_port = src_port;
    k.dst_port = dst_port;
    k.vrfid    = vrfid;
    memcpy(k.addr.v6.src, src_ip, 16);
    memcpy(k.addr.v6.dst, dst_ip, 16);
    return k;
}

struct fc2_flowu_result {
    uint32_t entry_idx; /* 1-origin; 0 = miss / full */
};

struct fc2_flowu_entry {
    struct fc2_flowu_key   key;          /* 44B */
    uint32_t               cur_hash;     /* current-bucket hash */
    uint64_t               last_ts;      /* 0 = free / invalid */
    RIX_SLIST_ENTRY(struct fc2_flowu_entry) free_link;
    uint16_t               slot;         /* slot in current bucket */
    uint16_t               reserved1;
} __attribute__((aligned(FC2_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct fc2_flowu_entry) == FC2_CACHE_LINE_SIZE,
                  "fc2_flowu_entry must be 64 bytes");

RIX_HASH_HEAD(fc2_flowu_ht);
RIX_SLIST_HEAD(fc2_flowu_free_head, fc2_flowu_entry);

struct fc2_flowu_config {
    uint64_t timeout_tsc;
    unsigned pressure_empty_slots;
};

struct fc2_flowu_stats {
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

struct fc2_flowu_cache {
    struct fc2_flowu_ht        ht_head;
    struct rix_hash_bucket_s  *buckets;
    struct fc2_flowu_entry    *pool;
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
    struct fc2_flowu_stats     stats;
    struct fc2_flowu_free_head free_head;
};

int fc2_flowu_cmp(const void *a, const void *b);
void fc2_flowu_cache_init(struct fc2_flowu_cache *fc,
                          struct rix_hash_bucket_s *buckets,
                          unsigned nb_bk,
                          struct fc2_flowu_entry *pool,
                          unsigned max_entries,
                          const struct fc2_flowu_config *cfg);
void fc2_flowu_cache_flush(struct fc2_flowu_cache *fc);
unsigned fc2_flowu_cache_nb_entries(const struct fc2_flowu_cache *fc);
unsigned fc2_flowu_cache_lookup_batch(struct fc2_flowu_cache *fc,
                                      const struct fc2_flowu_key *keys,
                                      unsigned nb_keys,
                                      uint64_t now,
                                      struct fc2_flowu_result *results,
                                      uint16_t *miss_idx);
unsigned fc2_flowu_cache_fill_miss_batch(struct fc2_flowu_cache *fc,
                                         const struct fc2_flowu_key *keys,
                                         const uint16_t *miss_idx,
                                         unsigned miss_count,
                                         uint64_t now,
                                         struct fc2_flowu_result *results);
unsigned fc2_flowu_cache_maintain(struct fc2_flowu_cache *fc,
                                  unsigned start_bk,
                                  unsigned bucket_count,
                                  uint64_t now);
int fc2_flowu_cache_remove_idx(struct fc2_flowu_cache *fc, uint32_t entry_idx);
void fc2_flowu_cache_stats(const struct fc2_flowu_cache *fc,
                           struct fc2_flowu_stats *out);

#endif /* _FLOWU_CACHE2_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
