/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <assert.h>

#include "flow_unified_cache.h"

#define FC_PREFIX       flowu
#define FC_ENTRY        flowu_entry
#define FC_KEY          flowu_key
#define FC_CACHE        flowu_cache
#include "backend.h"
#undef FC_CACHE
#undef FC_KEY
#undef FC_ENTRY
#undef FC_PREFIX

extern const struct flowu_cache_ops flowu_cache_ops_gen;
extern const struct flowu_cache_ops flowu_cache_ops_sse;
extern const struct flowu_cache_ops flowu_cache_ops_avx2;
extern const struct flowu_cache_ops flowu_cache_ops_avx512;

static const struct flowu_cache_ops *
flowu_cache_select_ops(flow_cache_backend_t requested,
                       flow_cache_backend_t *backend_id)
{
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (requested == FLOW_CACHE_BACKEND_AVX512 &&
        __builtin_cpu_supports("avx512f")) {
        *backend_id = FLOW_CACHE_BACKEND_AVX512;
        return &flowu_cache_ops_avx512;
    }
    if (requested == FLOW_CACHE_BACKEND_AVX2 &&
        __builtin_cpu_supports("avx2")) {
        *backend_id = FLOW_CACHE_BACKEND_AVX2;
        return &flowu_cache_ops_avx2;
    }
    if (requested == FLOW_CACHE_BACKEND_SSE &&
        __builtin_cpu_supports("sse4.2")) {
        *backend_id = FLOW_CACHE_BACKEND_SSE;
        return &flowu_cache_ops_sse;
    }
    if (requested == FLOW_CACHE_BACKEND_AUTO) {
        if (__builtin_cpu_supports("avx512f")) {
            *backend_id = FLOW_CACHE_BACKEND_AVX512;
            return &flowu_cache_ops_avx512;
        }
        if (__builtin_cpu_supports("avx2")) {
            *backend_id = FLOW_CACHE_BACKEND_AVX2;
            return &flowu_cache_ops_avx2;
        }
        if (__builtin_cpu_supports("sse4.2")) {
            *backend_id = FLOW_CACHE_BACKEND_SSE;
            return &flowu_cache_ops_sse;
        }
    }
#endif
    *backend_id = FLOW_CACHE_BACKEND_GEN;
    return &flowu_cache_ops_gen;
}

static inline const struct flowu_cache_ops *
flowu_cache_ops_from(const struct flowu_cache *fc)
{
    const struct flowu_cache_ops *ops =
        (const struct flowu_cache_ops *)fc->ops;
    assert(ops != NULL);
    return ops;
}

void
flowu_cache_init(struct flowu_cache *fc,
                 struct rix_hash_bucket_s *buckets,
                 unsigned nb_bk,
                 struct flowu_entry *pool,
                 unsigned max_entries,
                 flow_cache_backend_t backend,
                 uint64_t timeout_ms,
                 void (*init_cb)(struct flowu_entry *entry, void *arg),
                 void (*fini_cb)(struct flowu_entry *entry,
                                 flow_cache_fini_reason_t reason, void *arg),
                 void *cb_arg)
{
    flow_cache_backend_t backend_id;
    const struct flowu_cache_ops *ops =
        flowu_cache_select_ops(backend, &backend_id);

    ops->init(fc, buckets, nb_bk, pool, max_entries, timeout_ms,
              init_cb, fini_cb, cb_arg);
    fc->ops = ops;
    fc->backend_id = backend_id;
}

void flowu_cache_flush(struct flowu_cache *fc) { flowu_cache_ops_from(fc)->flush(fc); }
void flowu_cache_lookup_batch(struct flowu_cache *fc, const struct flowu_key *keys,
                              unsigned nb_pkts, struct flowu_entry **results)
{ flowu_cache_ops_from(fc)->lookup_batch(fc, keys, nb_pkts, results); }
unsigned flowu_cache_lookup_touch_batch(struct flowu_cache *fc,
                                        const struct flowu_key *keys,
                                        unsigned nb_pkts, uint64_t now,
                                        struct flowu_entry **results)
{ return flowu_cache_ops_from(fc)->lookup_touch_batch(fc, keys, nb_pkts, now, results); }
struct flowu_entry *flowu_cache_find(struct flowu_cache *fc, const struct flowu_key *key)
{ return flowu_cache_ops_from(fc)->find(fc, key); }
struct flowu_entry *flowu_cache_insert(struct flowu_cache *fc, const struct flowu_key *key,
                                       uint64_t now)
{ return flowu_cache_ops_from(fc)->insert(fc, key, now); }
void flowu_cache_insert_batch(struct flowu_cache *fc, const struct flowu_key *keys,
                              unsigned nb_keys, uint64_t now,
                              struct flowu_entry **results)
{ flowu_cache_ops_from(fc)->insert_batch(fc, keys, nb_keys, now, results); }
void flowu_cache_remove(struct flowu_cache *fc, struct flowu_entry *entry)
{ flowu_cache_ops_from(fc)->remove(fc, entry); }
void flowu_cache_expire(struct flowu_cache *fc, uint64_t now)
{ flowu_cache_ops_from(fc)->expire(fc, now); }
void flowu_cache_expire_2stage(struct flowu_cache *fc, uint64_t now)
{ flowu_cache_ops_from(fc)->expire_2stage(fc, now); }
void flowu_cache_stats(const struct flowu_cache *fc, struct flow_cache_stats *out)
{ flowu_cache_ops_from(fc)->stats(fc, out); }

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
