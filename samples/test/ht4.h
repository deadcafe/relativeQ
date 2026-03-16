/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef HT4_H
#define HT4_H

#include "flow_cache.h"

struct flow4_ht_direct_head {
    unsigned rhh_mask;
    unsigned rhh_nb;
};
RIX_STATIC_ASSERT(sizeof(struct flow4_ht_direct_head) == sizeof(struct flow4_ht),
                  "flow4_ht_direct_head must match flow4_ht layout");

struct flow4_ht_direct_ops {
    void (*init)(struct flow4_ht_direct_head *head, unsigned nb_bk);
    struct flow4_entry *(*find)(struct flow4_ht_direct_head *head,
                                struct rix_hash_bucket_s *buckets,
                                struct flow4_entry *pool,
                                const struct flow4_key *key);
    struct flow4_entry *(*insert)(struct flow4_ht_direct_head *head,
                                  struct rix_hash_bucket_s *buckets,
                                  struct flow4_entry *pool,
                                  struct flow4_entry *entry);
};

static inline union rix_hash_hash_u
flow4_ht_hash_fn(const struct flow4_key *key, uint32_t mask)
{
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
}

#endif /* HT4_H */
