/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef HT4_BACKEND_NAME
#error "HT4_BACKEND_NAME must be defined"
#endif

#include <string.h>

#include "ht4.h"

#define HT4_OPS_(backend) flow4_ht_direct_ops_##backend
#define HT4_OPS(backend)  HT4_OPS_(backend)

#define FC_HASH_DIRECT_DECLARE_HEAD 1
#define FC_HASH_DIRECT_HT_PREFIX flow4_ht_direct_impl
#define FC_HASH_DIRECT_ENTRY     flow4_entry
#define FC_HASH_DIRECT_CMP_FN    flow4_cmp
#define FC_HASH_DIRECT_HASH_FN   flow4_ht_hash_fn
#define FC_HASH_DIRECT_TARGET    HT4_BACKEND_NAME
#include "hash_direct.h"

static void
flow4_ht_direct_init_impl(struct flow4_ht_direct_head *head, unsigned nb_bk)
{
    struct flow4_ht_direct_impl tmp;

    flow4_ht_direct_impl_init(&tmp, nb_bk);
    memcpy(head, &tmp, sizeof(tmp));
}

static struct flow4_entry *
flow4_ht_direct_find_impl(struct flow4_ht_direct_head *head,
                          struct rix_hash_bucket_s *buckets,
                          struct flow4_entry *pool,
                          const struct flow4_key *key)
{
    struct flow4_ht_direct_impl tmp;

    memcpy(&tmp, head, sizeof(tmp));
    return flow4_ht_direct_impl_find(&tmp, buckets, pool, key);
}

static struct flow4_entry *
flow4_ht_direct_insert_impl(struct flow4_ht_direct_head *head,
                            struct rix_hash_bucket_s *buckets,
                            struct flow4_entry *pool,
                            struct flow4_entry *entry)
{
    struct flow4_ht_direct_impl tmp;
    struct flow4_entry *ret;

    memcpy(&tmp, head, sizeof(tmp));
    ret = flow4_ht_direct_impl_insert(&tmp, buckets, pool, entry);
    memcpy(head, &tmp, sizeof(tmp));
    return ret;
}

const struct flow4_ht_direct_ops
HT4_OPS(HT4_BACKEND_NAME) = {
    flow4_ht_direct_init_impl,
    flow4_ht_direct_find_impl,
    flow4_ht_direct_insert_impl,
};

#undef HT4_OPS
#undef HT4_OPS_
