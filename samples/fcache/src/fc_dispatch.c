/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * fc_dispatch.c - Runtime arch dispatch for fcache variants.
 *
 * Provides the unsuffixed public API that forwards hot-path calls
 * through the selected ops table, and cold-path calls to the _gen
 * (generic) implementation.
 *
 * Call fc_arch_init(FC_ARCH_AUTO) once at startup before any cache
 * operations.  Without init, the generic implementation is used.
 */

#include <rix/rix_hash_arch.h>
#include "fc_ops.h"

/*===========================================================================
 * Per-variant dispatch state
 *===========================================================================*/
static const struct fc_flow4_ops *_fc_flow4_active = &fc_flow4_ops_gen;
static const struct fc_flow6_ops *_fc_flow6_active = &fc_flow6_ops_gen;
static const struct fc_flowu_ops *_fc_flowu_active = &fc_flowu_ops_gen;

/*===========================================================================
 * fc_arch_init -- one-time CPU detection and ops selection
 *===========================================================================*/
void
fc_arch_init(unsigned arch_enable)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    FC_OPS_SELECT(flow4, arch_enable, &_fc_flow4_active);
    FC_OPS_SELECT(flow6, arch_enable, &_fc_flow6_active);
    FC_OPS_SELECT(flowu, arch_enable, &_fc_flowu_active);
}

/*===========================================================================
 * Cold-path wrappers -- forward to _gen (not SIMD-sensitive)
 *===========================================================================*/

/* flow4 cold-path */
void
fc_flow4_cache_init(struct fc_flow4_cache *fc,
                    struct rix_hash_bucket_s *buckets,
                    unsigned nb_bk,
                    struct fc_flow4_entry *pool,
                    unsigned max_entries,
                    const struct fc_flow4_config *cfg)
{
    fc_flow4_ops_gen.init(fc, buckets, nb_bk, pool, max_entries, cfg);
}

void
fc_flow4_cache_flush(struct fc_flow4_cache *fc)
{
    fc_flow4_ops_gen.flush(fc);
}

unsigned
fc_flow4_cache_nb_entries(const struct fc_flow4_cache *fc)
{
    return fc_flow4_ops_gen.nb_entries(fc);
}

int
fc_flow4_cache_remove_idx(struct fc_flow4_cache *fc, uint32_t entry_idx)
{
    return fc_flow4_ops_gen.remove_idx(fc, entry_idx);
}

void
fc_flow4_cache_stats(const struct fc_flow4_cache *fc,
                     struct fc_flow4_stats *out)
{
    fc_flow4_ops_gen.stats(fc, out);
}

/* flow6 cold-path */
void
fc_flow6_cache_init(struct fc_flow6_cache *fc,
                    struct rix_hash_bucket_s *buckets,
                    unsigned nb_bk,
                    struct fc_flow6_entry *pool,
                    unsigned max_entries,
                    const struct fc_flow6_config *cfg)
{
    fc_flow6_ops_gen.init(fc, buckets, nb_bk, pool, max_entries, cfg);
}

void
fc_flow6_cache_flush(struct fc_flow6_cache *fc)
{
    fc_flow6_ops_gen.flush(fc);
}

unsigned
fc_flow6_cache_nb_entries(const struct fc_flow6_cache *fc)
{
    return fc_flow6_ops_gen.nb_entries(fc);
}

int
fc_flow6_cache_remove_idx(struct fc_flow6_cache *fc, uint32_t entry_idx)
{
    return fc_flow6_ops_gen.remove_idx(fc, entry_idx);
}

void
fc_flow6_cache_stats(const struct fc_flow6_cache *fc,
                     struct fc_flow6_stats *out)
{
    fc_flow6_ops_gen.stats(fc, out);
}

/* flowu cold-path */
void
fc_flowu_cache_init(struct fc_flowu_cache *fc,
                    struct rix_hash_bucket_s *buckets,
                    unsigned nb_bk,
                    struct fc_flowu_entry *pool,
                    unsigned max_entries,
                    const struct fc_flowu_config *cfg)
{
    fc_flowu_ops_gen.init(fc, buckets, nb_bk, pool, max_entries, cfg);
}

void
fc_flowu_cache_flush(struct fc_flowu_cache *fc)
{
    fc_flowu_ops_gen.flush(fc);
}

unsigned
fc_flowu_cache_nb_entries(const struct fc_flowu_cache *fc)
{
    return fc_flowu_ops_gen.nb_entries(fc);
}

int
fc_flowu_cache_remove_idx(struct fc_flowu_cache *fc, uint32_t entry_idx)
{
    return fc_flowu_ops_gen.remove_idx(fc, entry_idx);
}

void
fc_flowu_cache_stats(const struct fc_flowu_cache *fc,
                     struct fc_flowu_stats *out)
{
    fc_flowu_ops_gen.stats(fc, out);
}

/*===========================================================================
 * Hot-path wrappers -- dispatch through selected ops table
 *===========================================================================*/

/* flow4 hot-path */
void
fc_flow4_cache_lookup_batch(struct fc_flow4_cache *fc,
                            const struct fc_flow4_key *keys,
                            unsigned nb_keys, uint64_t now,
                            struct fc_flow4_result *results)
{
    _fc_flow4_active->lookup_batch(fc, keys, nb_keys, now, results);
}

unsigned
fc_flow4_cache_maintain(struct fc_flow4_cache *fc,
                        unsigned start_bk, unsigned bucket_count,
                        uint64_t now)
{
    return _fc_flow4_active->maintain(fc, start_bk, bucket_count, now);
}

unsigned
fc_flow4_cache_maintain_step_ex(struct fc_flow4_cache *fc,
                                unsigned start_bk, unsigned bucket_count,
                                unsigned skip_threshold, uint64_t now)
{
    return _fc_flow4_active->maintain_step_ex(fc, start_bk, bucket_count,
                                               skip_threshold, now);
}

unsigned
fc_flow4_cache_maintain_step(struct fc_flow4_cache *fc,
                             uint64_t now, int idle)
{
    return _fc_flow4_active->maintain_step(fc, now, idle);
}

/* flow6 hot-path */
void
fc_flow6_cache_lookup_batch(struct fc_flow6_cache *fc,
                            const struct fc_flow6_key *keys,
                            unsigned nb_keys, uint64_t now,
                            struct fc_flow6_result *results)
{
    _fc_flow6_active->lookup_batch(fc, keys, nb_keys, now, results);
}

unsigned
fc_flow6_cache_maintain(struct fc_flow6_cache *fc,
                        unsigned start_bk, unsigned bucket_count,
                        uint64_t now)
{
    return _fc_flow6_active->maintain(fc, start_bk, bucket_count, now);
}

unsigned
fc_flow6_cache_maintain_step_ex(struct fc_flow6_cache *fc,
                                unsigned start_bk, unsigned bucket_count,
                                unsigned skip_threshold, uint64_t now)
{
    return _fc_flow6_active->maintain_step_ex(fc, start_bk, bucket_count,
                                               skip_threshold, now);
}

unsigned
fc_flow6_cache_maintain_step(struct fc_flow6_cache *fc,
                             uint64_t now, int idle)
{
    return _fc_flow6_active->maintain_step(fc, now, idle);
}

/* flowu hot-path */
void
fc_flowu_cache_lookup_batch(struct fc_flowu_cache *fc,
                            const struct fc_flowu_key *keys,
                            unsigned nb_keys, uint64_t now,
                            struct fc_flowu_result *results)
{
    _fc_flowu_active->lookup_batch(fc, keys, nb_keys, now, results);
}

unsigned
fc_flowu_cache_maintain(struct fc_flowu_cache *fc,
                        unsigned start_bk, unsigned bucket_count,
                        uint64_t now)
{
    return _fc_flowu_active->maintain(fc, start_bk, bucket_count, now);
}

unsigned
fc_flowu_cache_maintain_step_ex(struct fc_flowu_cache *fc,
                                unsigned start_bk, unsigned bucket_count,
                                unsigned skip_threshold, uint64_t now)
{
    return _fc_flowu_active->maintain_step_ex(fc, start_bk, bucket_count,
                                               skip_threshold, now);
}

unsigned
fc_flowu_cache_maintain_step(struct fc_flowu_cache *fc,
                             uint64_t now, int idle)
{
    return _fc_flowu_active->maintain_step(fc, now, idle);
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
