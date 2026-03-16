/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <assert.h>

#include "flow4_cache.h"

#define FC_PREFIX       flow4
#define FC_ENTRY        flow4_entry
#define FC_KEY          flow4_key
#define FC_CACHE        flow4_cache
#include "backend.h"
#undef FC_CACHE
#undef FC_KEY
#undef FC_ENTRY
#undef FC_PREFIX

extern const struct flow4_cache_ops flow4_cache_ops_gen;
extern const struct flow4_cache_ops flow4_cache_ops_sse;
extern const struct flow4_cache_ops flow4_cache_ops_avx2;
extern const struct flow4_cache_ops flow4_cache_ops_avx512;

static const struct flow4_cache_ops *
flow4_cache_select_ops(flow_cache_backend_t requested,
                       flow_cache_backend_t *backend_id)
{
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (requested == FLOW_CACHE_BACKEND_AVX512 &&
        __builtin_cpu_supports("avx512f")) {
        *backend_id = FLOW_CACHE_BACKEND_AVX512;
        return &flow4_cache_ops_avx512;
    }
    if (requested == FLOW_CACHE_BACKEND_AVX2 &&
        __builtin_cpu_supports("avx2")) {
        *backend_id = FLOW_CACHE_BACKEND_AVX2;
        return &flow4_cache_ops_avx2;
    }
    if (requested == FLOW_CACHE_BACKEND_SSE &&
        __builtin_cpu_supports("sse4.2")) {
        *backend_id = FLOW_CACHE_BACKEND_SSE;
        return &flow4_cache_ops_sse;
    }
    if (requested == FLOW_CACHE_BACKEND_AUTO) {
        if (__builtin_cpu_supports("avx512f")) {
            *backend_id = FLOW_CACHE_BACKEND_AVX512;
            return &flow4_cache_ops_avx512;
        }
        if (__builtin_cpu_supports("avx2")) {
            *backend_id = FLOW_CACHE_BACKEND_AVX2;
            return &flow4_cache_ops_avx2;
        }
        if (__builtin_cpu_supports("sse4.2")) {
            *backend_id = FLOW_CACHE_BACKEND_SSE;
            return &flow4_cache_ops_sse;
        }
    }
#endif
    *backend_id = FLOW_CACHE_BACKEND_GEN;
    return &flow4_cache_ops_gen;
}

static inline const struct flow4_cache_ops *
flow4_cache_ops_from(const struct flow4_cache *fc)
{
    const struct flow4_cache_ops *ops =
        (const struct flow4_cache_ops *)fc->ops;
    assert(ops != NULL);
    return ops;
}

void
flow4_cache_init(struct flow4_cache *fc,
                 struct rix_hash_bucket_s *buckets,
                 unsigned nb_bk,
                 struct flow4_entry *pool,
                 unsigned max_entries,
                 flow_cache_backend_t backend,
                 uint64_t timeout_ms,
                 void (*init_cb)(struct flow4_entry *entry, void *arg),
                 void (*fini_cb)(struct flow4_entry *entry,
                                 flow_cache_fini_reason_t reason, void *arg),
                 void *cb_arg)
{
    flow_cache_backend_t backend_id;
    const struct flow4_cache_ops *ops =
        flow4_cache_select_ops(backend, &backend_id);

    ops->init(fc, buckets, nb_bk, pool, max_entries, timeout_ms,
              init_cb, fini_cb, cb_arg);
    fc->ops = ops;
    fc->backend_id = backend_id;
}

void
flow4_cache_flush(struct flow4_cache *fc)
{
    flow4_cache_ops_from(fc)->flush(fc);
}

void
flow4_cache_lookup_batch(struct flow4_cache *fc,
                         const struct flow4_key *keys,
                         unsigned nb_pkts,
                         struct flow4_entry **results)
{
    flow4_cache_ops_from(fc)->lookup_batch(fc, keys, nb_pkts, results);
}

struct flow4_entry *
flow4_cache_find(struct flow4_cache *fc, const struct flow4_key *key)
{
    return flow4_cache_ops_from(fc)->find(fc, key);
}

struct flow4_entry *
flow4_cache_insert(struct flow4_cache *fc,
                   const struct flow4_key *key,
                   uint64_t now)
{
    return flow4_cache_ops_from(fc)->insert(fc, key, now);
}

void
flow4_cache_insert_batch(struct flow4_cache *fc,
                         const struct flow4_key *keys,
                         unsigned nb_keys,
                         uint64_t now,
                         struct flow4_entry **results)
{
    flow4_cache_ops_from(fc)->insert_batch(fc, keys, nb_keys, now, results);
}

void
flow4_cache_remove(struct flow4_cache *fc, struct flow4_entry *entry)
{
    flow4_cache_ops_from(fc)->remove(fc, entry);
}

void
flow4_cache_expire(struct flow4_cache *fc, uint64_t now)
{
    flow4_cache_ops_from(fc)->expire(fc, now);
}

void
flow4_cache_expire_2stage(struct flow4_cache *fc, uint64_t now)
{
    flow4_cache_ops_from(fc)->expire_2stage(fc, now);
}

void
flow4_cache_stats(const struct flow4_cache *fc, struct flow_cache_stats *out)
{
    flow4_cache_ops_from(fc)->stats(fc, out);
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
