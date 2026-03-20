/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * fc_ops.h - Architecture-dispatch ops table for fcache variants.
 *
 * Each variant (flow4, flow6, flowu) has an ops table with function
 * pointers to the datapath-hot functions.  The init function selects
 * the best available implementation based on CPU features.
 */

#ifndef _FC_OPS_H_
#define _FC_OPS_H_

#include "flow4_cache.h"
#include "flow6_cache.h"
#include "flowu_cache.h"

/*===========================================================================
 * Arch enable flags (mirrors rix_hash_arch.h convention)
 *===========================================================================*/
#define FC_ARCH_GEN     0u
#define FC_ARCH_SSE     (1u << 0)
#define FC_ARCH_AVX2    (1u << 1)
#define FC_ARCH_AVX512  (1u << 2)
#define FC_ARCH_AUTO    (FC_ARCH_SSE | FC_ARCH_AVX2 | FC_ARCH_AVX512)

/*===========================================================================
 * Ops table macro: generates struct fc_<prefix>_ops
 *===========================================================================*/
#define FC_OPS_DEFINE(prefix)                                                  \
struct fc_##prefix##_ops {                                                     \
    unsigned (*lookup_batch)(struct fc_##prefix##_cache *fc,                    \
                             const struct fc_##prefix##_key *keys,              \
                             unsigned nb_keys, uint64_t now,                    \
                             struct fc_##prefix##_result *results,              \
                             uint16_t *miss_idx);                              \
    unsigned (*fill_miss_batch)(struct fc_##prefix##_cache *fc,                 \
                                const struct fc_##prefix##_key *keys,           \
                                const uint16_t *miss_idx,                      \
                                unsigned miss_count, uint64_t now,              \
                                struct fc_##prefix##_result *results);          \
    unsigned (*maintain)(struct fc_##prefix##_cache *fc,                        \
                         unsigned start_bk, unsigned bucket_count,              \
                         uint64_t now);                                         \
    unsigned (*maintain_step_ex)(struct fc_##prefix##_cache *fc,                \
                                 unsigned start_bk, unsigned bucket_count,      \
                                 unsigned skip_threshold, uint64_t now);        \
    unsigned (*maintain_step)(struct fc_##prefix##_cache *fc,                   \
                              uint64_t now, int idle);                          \
}

FC_OPS_DEFINE(flow4);
FC_OPS_DEFINE(flow6);
FC_OPS_DEFINE(flowu);

/*===========================================================================
 * Per-arch ops table declarations (defined in arch-specific .o files)
 *===========================================================================*/
#define FC_OPS_DECLARE(prefix, suffix)                                         \
    extern const struct fc_##prefix##_ops fc_##prefix##_ops##suffix

/* GEN (always available) */
FC_OPS_DECLARE(flow4, _gen);
FC_OPS_DECLARE(flow6, _gen);
FC_OPS_DECLARE(flowu, _gen);

/* SSE4.2 */
FC_OPS_DECLARE(flow4, _sse);
FC_OPS_DECLARE(flow6, _sse);
FC_OPS_DECLARE(flowu, _sse);

/* AVX2 */
FC_OPS_DECLARE(flow4, _avx2);
FC_OPS_DECLARE(flow6, _avx2);
FC_OPS_DECLARE(flowu, _avx2);

/* AVX-512 */
FC_OPS_DECLARE(flow4, _avx512);
FC_OPS_DECLARE(flow6, _avx512);
FC_OPS_DECLARE(flowu, _avx512);

/*===========================================================================
 * Cold-path function declarations (non-SIMD, _gen only)
 *===========================================================================*/
#define FC_COLD_DECLARE(prefix)                                                \
    extern void                                                                \
    fc_##prefix##_cache_init_gen(struct fc_##prefix##_cache *fc,               \
                                 struct rix_hash_bucket_s *buckets,            \
                                 unsigned nb_bk,                               \
                                 struct fc_##prefix##_entry *pool,             \
                                 unsigned max_entries,                          \
                                 const struct fc_##prefix##_config *cfg);      \
    extern void                                                                \
    fc_##prefix##_cache_flush_gen(struct fc_##prefix##_cache *fc);             \
    extern unsigned                                                            \
    fc_##prefix##_cache_nb_entries_gen(                                        \
        const struct fc_##prefix##_cache *fc);                                 \
    extern int                                                                 \
    fc_##prefix##_cache_remove_idx_gen(struct fc_##prefix##_cache *fc,         \
                                       uint32_t entry_idx);                    \
    extern void                                                                \
    fc_##prefix##_cache_stats_gen(const struct fc_##prefix##_cache *fc,        \
                                  struct fc_##prefix##_stats *out)

#ifndef FC_ARCH_SUFFIX
FC_COLD_DECLARE(flow4);
FC_COLD_DECLARE(flow6);
FC_COLD_DECLARE(flowu);

/*===========================================================================
 * fc_arch_init -- call once at startup
 *===========================================================================*/
void fc_arch_init(unsigned arch_enable);
#endif

/*===========================================================================
 * Runtime selection helper
 *===========================================================================*/
#define FC_OPS_SELECT(prefix, arch_enable, out_ops)                            \
do {                                                                           \
    *(out_ops) = &fc_##prefix##_ops_gen;                                       \
    _FC_OPS_SELECT_BODY(prefix, arch_enable, out_ops)                          \
} while (0)

#if defined(__x86_64__)
#define _FC_OPS_SELECT_BODY(prefix, arch_enable, out_ops)                      \
    __builtin_cpu_init();                                                      \
    if (((arch_enable) & FC_ARCH_AVX512) &&                                    \
        __builtin_cpu_supports("avx512f")) {                                   \
        *(out_ops) = &fc_##prefix##_ops_avx512;                                \
    } else if (((arch_enable) & (FC_ARCH_AVX2 | FC_ARCH_AVX512)) &&            \
               __builtin_cpu_supports("avx2")) {                               \
        *(out_ops) = &fc_##prefix##_ops_avx2;                                  \
    } else if (((arch_enable) & (FC_ARCH_SSE | FC_ARCH_AVX2 |                  \
                                  FC_ARCH_AVX512)) &&                           \
               __builtin_cpu_supports("sse4.2")) {                             \
        *(out_ops) = &fc_##prefix##_ops_sse;                                   \
    }
#else
#define _FC_OPS_SELECT_BODY(prefix, arch_enable, out_ops)  (void)(arch_enable);
#endif

#endif /* _FC_OPS_H_ */
