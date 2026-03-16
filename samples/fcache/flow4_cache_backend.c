/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef FLOW_CACHE_BACKEND_NAME
#error "FLOW_CACHE_BACKEND_NAME must be defined"
#endif

#include "flow4_cache.h"

#define FC_BACKEND_PREFIX_(prefix, backend) prefix ## _backend_ ## backend
#define FC_BACKEND_PREFIX(prefix, backend)  FC_BACKEND_PREFIX_(prefix, backend)
#define FC_BACKEND_OPS_(prefix, backend)    prefix ## _cache_ops_ ## backend
#define FC_BACKEND_OPS(prefix, backend)     FC_BACKEND_OPS_(prefix, backend)
#define FC_BACKEND_FN_(prefix, backend, suffix) \
    prefix ## _backend_ ## backend ## _ ## suffix
#define FC_BACKEND_FN(prefix, backend, suffix) \
    FC_BACKEND_FN_(prefix, backend, suffix)

#define FC_PREFIX       flow4
#define FC_ENTRY        flow4_entry
#define FC_KEY          flow4_key
#define FC_CACHE        flow4_cache
#include "flow_cache_backend_private.h"
#undef FC_CACHE
#undef FC_KEY
#undef FC_ENTRY
#undef FC_PREFIX

static inline union rix_hash_hash_u
flow4_ht_hash_fn(const struct flow4_key *key, uint32_t mask)
{
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
}

RIX_HASH_GENERATE_STATIC_EX(flow4_ht, flow4_entry, key, cur_hash,
                            flow4_cmp, flow4_ht_hash_fn)

#define FC_PREFIX       FC_BACKEND_PREFIX(flow4, FLOW_CACHE_BACKEND_NAME)
#define FC_ENTRY        flow4_entry
#define FC_KEY          flow4_key
#define FC_CACHE        flow4_cache
#define FC_HT_PREFIX    flow4_ht
#define FC_FREE_HEAD    flow4_free_head
#define FC_IMPL_ATTR    static
#include "flow_cache_body_private.h"
#undef FC_PREFIX
#undef FC_ENTRY
#undef FC_KEY
#undef FC_CACHE
#undef FC_HT_PREFIX
#undef FC_FREE_HEAD
#undef FC_IMPL_ATTR

const struct flow4_cache_ops FC_BACKEND_OPS(flow4, FLOW_CACHE_BACKEND_NAME) = {
    FC_BACKEND_FN(flow4, FLOW_CACHE_BACKEND_NAME, cache_init),
    FC_BACKEND_FN(flow4, FLOW_CACHE_BACKEND_NAME, cache_flush),
    FC_BACKEND_FN(flow4, FLOW_CACHE_BACKEND_NAME, cache_lookup_batch),
    FC_BACKEND_FN(flow4, FLOW_CACHE_BACKEND_NAME, cache_find),
    FC_BACKEND_FN(flow4, FLOW_CACHE_BACKEND_NAME, cache_insert),
    FC_BACKEND_FN(flow4, FLOW_CACHE_BACKEND_NAME, cache_remove),
    FC_BACKEND_FN(flow4, FLOW_CACHE_BACKEND_NAME, cache_expire),
    FC_BACKEND_FN(flow4, FLOW_CACHE_BACKEND_NAME, cache_expire_2stage),
    FC_BACKEND_FN(flow4, FLOW_CACHE_BACKEND_NAME, cache_stats),
};

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
