/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_cache_backend_private.h - private backend ops template
 *
 * Intentionally has no include guard: include once per variant with
 * FC_PREFIX / FC_ENTRY / FC_KEY / FC_CACHE defined.
 */

#define _FCB_CAT(a, b, c)      a ## b ## c
#define FCB_CAT(a, b, c)       _FCB_CAT(a, b, c)
#define FCB_FN(prefix, suffix) FCB_CAT(prefix, _, suffix)

struct FCB_FN(FC_PREFIX, cache_ops) {
    void (*init)(struct FC_CACHE *fc,
                 struct rix_hash_bucket_s *buckets,
                 unsigned nb_bk,
                 struct FC_ENTRY *pool,
                 unsigned max_entries,
                 flow_cache_backend_t backend,
                 uint64_t timeout_ms,
                 void (*init_cb)(struct FC_ENTRY *entry, void *arg),
                 void (*fini_cb)(struct FC_ENTRY *entry,
                                 flow_cache_fini_reason_t reason, void *arg),
                 void *cb_arg);
    void (*flush)(struct FC_CACHE *fc);
    void (*lookup_batch)(struct FC_CACHE *fc,
                         const struct FC_KEY *keys,
                         unsigned nb_pkts,
                         struct FC_ENTRY **results);
    struct FC_ENTRY *(*find)(struct FC_CACHE *fc, const struct FC_KEY *key);
    struct FC_ENTRY *(*insert)(struct FC_CACHE *fc,
                               const struct FC_KEY *key,
                               uint64_t now);
    void (*remove)(struct FC_CACHE *fc, struct FC_ENTRY *entry);
    void (*expire)(struct FC_CACHE *fc, uint64_t now);
    void (*expire_2stage)(struct FC_CACHE *fc, uint64_t now);
    void (*stats)(const struct FC_CACHE *fc, struct flow_cache_stats *out);
};

#undef FCB_FN
#undef FCB_CAT
#undef _FCB_CAT
