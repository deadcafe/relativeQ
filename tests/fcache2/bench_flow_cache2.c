/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <assert.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../samples/fcache/include/flow4_cache.h"
#include "../../samples/fcache/include/flow_cache.h"
#include "../../samples/fcache2/include/flow4_cache2.h"

enum {
    FC2_BENCH_ALIGN = 64u,
    FC2_BENCH_DESIRED_ENTRIES = 32768u,
    FC2_BENCH_QUERY = 256u,
    FC2_BENCH_SWEEP_QUERY = 256u,
    FC2_BENCH_OLD_EXPIRE_PERIOD = 4096u,
    FC2_BENCH_HIT_REPEAT = 200u,
    FC2_BENCH_MISS_REPEAT = 80u,
    FC2_BENCH_MIXED_REPEAT = 80u
};

struct old_stats_delta {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t inserts;
    uint64_t evictions;
};

static size_t
bench_round_up(size_t n, size_t align)
{
    return (n + align - 1u) & ~(align - 1u);
}

static void *
bench_alloc(size_t size)
{
    size_t alloc_size = bench_round_up(size, FC2_BENCH_ALIGN);
    void *p = aligned_alloc(FC2_BENCH_ALIGN, alloc_size);

    if (p == NULL) {
        perror("aligned_alloc");
        exit(1);
    }
    memset(p, 0, alloc_size);
    return p;
}

static inline struct flow4_key
bench_make_key(unsigned i)
{
    struct flow4_key k;

    memset(&k, 0, sizeof(k));
    k.src_ip = 0x0a000000u | (i & 0x00ffffffu);
    k.dst_ip = 0x14000000u | ((i * 2654435761u) & 0x00ffffffu);
    k.src_port = (uint16_t)(1024u + (i & 0x7fffu));
    k.dst_port = (uint16_t)(2048u + ((i >> 11) & 0x7fffu));
    k.proto = (uint8_t)(6u + (i & 1u));
    k.vrfid = 1u + (i >> 24);
    return k;
}

static inline struct fc2_flow4_key
bench_make_key2(unsigned i)
{
    struct flow4_key k = bench_make_key(i);
    struct fc2_flow4_key k2;

    memset(&k2, 0, sizeof(k2));
    k2.src_ip = k.src_ip;
    k2.dst_ip = k.dst_ip;
    k2.src_port = k.src_port;
    k2.dst_port = k.dst_port;
    k2.proto = k.proto;
    k2.vrfid = k.vrfid;
    return k2;
}

struct old_cache_ctx {
    struct flow4_cache fc;
    struct rix_hash_bucket_s *buckets;
    struct flow4_entry *pool;
    flow_cache_backend_t backend;
};

struct new_cache_ctx {
    struct fc2_flow4_cache fc;
    struct rix_hash_bucket_s *buckets;
    struct fc2_flow4_entry *pool;
};

struct new_stats_delta {
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

static volatile uint64_t bench_relief_sink;

struct hotseed_profile {
    const char *name;
    unsigned start_fill_pct;
    unsigned interval_ge_90;
    unsigned interval_ge_85;
    unsigned interval_ge_80;
    unsigned interval_ge_start;
};

static const struct hotseed_profile hotseed_profiles[] = {
    { "aggr75", 75u, 2u, 4u, 8u, 16u },
    { "mid75", 75u, 4u, 8u, 16u, 32u },
    { "lite75", 75u, 8u, 16u, 32u, 64u },
    { "pre70", 70u, 8u, 16u, 32u, 64u },
    { "pre65", 65u, 8u, 16u, 32u, 128u }
};

static unsigned
new_prefill(struct new_cache_ctx *ctx,
            const struct fc2_flow4_key *keys,
            unsigned n,
            uint64_t now);

static struct new_stats_delta
bench_new_stats_snapshot(const struct new_cache_ctx *ctx)
{
    struct fc2_flow4_stats stats;
    struct new_stats_delta snap;

    fc2_flow4_cache_stats(&ctx->fc, &stats);
    snap.lookups = stats.lookups;
    snap.hits = stats.hits;
    snap.misses = stats.misses;
    snap.fills = stats.fills;
    snap.fill_full = stats.fill_full;
    snap.relief_calls = stats.relief_calls;
    snap.relief_bucket_checks = stats.relief_bucket_checks;
    snap.relief_evictions = stats.relief_evictions;
    snap.relief_bk0_evictions = stats.relief_bk0_evictions;
    snap.relief_bk1_evictions = stats.relief_bk1_evictions;
    snap.maint_calls = stats.maint_calls;
    snap.maint_bucket_checks = stats.maint_bucket_checks;
    snap.maint_evictions = stats.maint_evictions;
    return snap;
}

static struct new_stats_delta
bench_new_stats_diff(struct new_stats_delta after,
                     struct new_stats_delta before)
{
    struct new_stats_delta diff;

    diff.lookups = after.lookups - before.lookups;
    diff.hits = after.hits - before.hits;
    diff.misses = after.misses - before.misses;
    diff.fills = after.fills - before.fills;
    diff.fill_full = after.fill_full - before.fill_full;
    diff.relief_calls = after.relief_calls - before.relief_calls;
    diff.relief_bucket_checks = after.relief_bucket_checks - before.relief_bucket_checks;
    diff.relief_evictions = after.relief_evictions - before.relief_evictions;
    diff.relief_bk0_evictions = after.relief_bk0_evictions - before.relief_bk0_evictions;
    diff.relief_bk1_evictions = after.relief_bk1_evictions - before.relief_bk1_evictions;
    diff.maint_calls = after.maint_calls - before.maint_calls;
    diff.maint_bucket_checks = after.maint_bucket_checks - before.maint_bucket_checks;
    diff.maint_evictions = after.maint_evictions - before.maint_evictions;
    return diff;
}

static void
old_cache_setup(struct old_cache_ctx *ctx,
                unsigned nb_bk,
                unsigned max_entries,
                flow_cache_backend_t backend)
{
    ctx->buckets = bench_alloc((size_t)nb_bk * sizeof(*ctx->buckets));
    ctx->pool = bench_alloc((size_t)max_entries * sizeof(*ctx->pool));
    ctx->backend = backend;
    flow4_cache_init(&ctx->fc, ctx->buckets, nb_bk, ctx->pool, max_entries,
                     backend, 0u, NULL, NULL, NULL);
}

static void
old_cache_setup_timeout(struct old_cache_ctx *ctx,
                        unsigned nb_bk,
                        unsigned max_entries,
                        flow_cache_backend_t backend,
                        uint64_t timeout_ms)
{
    ctx->buckets = bench_alloc((size_t)nb_bk * sizeof(*ctx->buckets));
    ctx->pool = bench_alloc((size_t)max_entries * sizeof(*ctx->pool));
    ctx->backend = backend;
    flow4_cache_init(&ctx->fc, ctx->buckets, nb_bk, ctx->pool, max_entries,
                     backend, timeout_ms, NULL, NULL, NULL);
}

static void
new_cache_setup(struct new_cache_ctx *ctx, unsigned nb_bk, unsigned max_entries)
{
    struct fc2_flow4_config cfg = {
        .timeout_tsc = UINT64_C(1000000),
        .pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS
    };

    ctx->buckets = bench_alloc((size_t)nb_bk * sizeof(*ctx->buckets));
    ctx->pool = bench_alloc((size_t)max_entries * sizeof(*ctx->pool));
    fc2_flow4_cache_init(&ctx->fc, ctx->buckets, nb_bk, ctx->pool, max_entries,
                         &cfg);
}

static void
new_cache_setup_cfg(struct new_cache_ctx *ctx,
                    unsigned nb_bk,
                    unsigned max_entries,
                    const struct fc2_flow4_config *cfg)
{
    ctx->buckets = bench_alloc((size_t)nb_bk * sizeof(*ctx->buckets));
    ctx->pool = bench_alloc((size_t)max_entries * sizeof(*ctx->pool));
    fc2_flow4_cache_init(&ctx->fc, ctx->buckets, nb_bk, ctx->pool, max_entries,
                         cfg);
}

static struct old_stats_delta
bench_old_stats_snapshot(const struct old_cache_ctx *ctx)
{
    struct flow_cache_stats stats;
    struct old_stats_delta snap;

    flow4_cache_stats(&ctx->fc, &stats);
    snap.lookups = stats.lookups;
    snap.hits = stats.hits;
    snap.misses = stats.misses;
    snap.inserts = stats.inserts;
    snap.evictions = stats.evictions;
    return snap;
}

static struct old_stats_delta
bench_old_stats_diff(struct old_stats_delta after,
                     struct old_stats_delta before)
{
    struct old_stats_delta diff;

    diff.lookups = after.lookups - before.lookups;
    diff.hits = after.hits - before.hits;
    diff.misses = after.misses - before.misses;
    diff.inserts = after.inserts - before.inserts;
    diff.evictions = after.evictions - before.evictions;
    return diff;
}

static void
old_cache_reset(struct old_cache_ctx *ctx)
{
    flow4_cache_flush(&ctx->fc);
}

static void
new_cache_reset(struct new_cache_ctx *ctx)
{
    fc2_flow4_cache_flush(&ctx->fc);
}

static inline struct fc2_flow4_entry *
bench_fc2_entry_from_idx(struct fc2_flow4_entry *pool, unsigned idx)
{
    return (idx == (unsigned)RIX_NIL) ? NULL : RIX_PTR_FROM_IDX(pool, idx);
}

static inline void
bench_fc2_free_entry(struct new_cache_ctx *ctx,
                     struct fc2_flow4_entry *entry)
{
    entry->last_ts = 0u;
    RIX_SLIST_INSERT_HEAD(&ctx->fc.free_head, ctx->pool, entry, free_link);
}

static inline unsigned
bench_fc2_remove_at(struct new_cache_ctx *ctx,
                    unsigned bk,
                    unsigned slot)
{
    struct rix_hash_bucket_s *bucket = &ctx->buckets[bk];
    unsigned idx = bucket->idx[slot];

    if (idx == (unsigned)RIX_NIL)
        return (unsigned)RIX_NIL;
    bucket->hash[slot] = 0u;
    bucket->idx[slot] = (unsigned)RIX_NIL;
    ctx->fc.ht_head.rhh_nb--;
    return idx;
}

static inline unsigned
bench_relief_empty_count(const struct rix_hash_bucket_s *bucket)
{
    unsigned n = 0u;

    for (unsigned i = 0; i < RIX_HASH_BUCKET_ENTRY_SZ; i++) {
        if (bucket->hash[i] == 0u)
            n++;
    }
    return n;
}

static inline unsigned
bench_relief_naive_scan(const struct rix_hash_bucket_s *bucket,
                        struct fc2_flow4_entry *pool,
                        uint64_t now_tsc,
                        uint64_t eff_timeout_tsc,
                        unsigned pressure_empty_slots,
                        unsigned bk_idx)
{
    unsigned start;
    struct fc2_flow4_entry *victim = NULL;
    uint64_t oldest_ts = UINT64_MAX;

    if (bench_relief_empty_count(bucket) > pressure_empty_slots)
        return 0u;

    /* Match fc2 local-relief slot dispersion cheaply. */
    start = (unsigned)((now_tsc ^ bk_idx) & (RIX_HASH_BUCKET_ENTRY_SZ - 1u));
    for (unsigned i = 0; i < RIX_HASH_BUCKET_ENTRY_SZ; i++) {
        unsigned slot = (start + i) & (RIX_HASH_BUCKET_ENTRY_SZ - 1u);
        unsigned idx = bucket->idx[slot];
        struct fc2_flow4_entry *entry = bench_fc2_entry_from_idx(pool, idx);

        if (entry == NULL || entry->last_ts == 0u)
            continue;
        if (now_tsc - entry->last_ts <= eff_timeout_tsc)
            continue;
        if (entry->last_ts < oldest_ts) {
            oldest_ts = entry->last_ts;
            victim = entry;
        }
    }
    return (victim != NULL) ? RIX_IDX_FROM_PTR(pool, victim) : 0u;
}

static inline unsigned
bench_relief_sampled_scan(const struct rix_hash_bucket_s *bucket,
                          struct fc2_flow4_entry *pool,
                          uint64_t now_tsc,
                          uint64_t eff_timeout_tsc,
                          unsigned pressure_empty_slots,
                          unsigned bk_idx)
{
    enum { FC2_RELIEF_SCAN_SLOTS = 8u };
    unsigned start;
    struct fc2_flow4_entry *victim = NULL;
    uint64_t oldest_ts = UINT64_MAX;

    if (bench_relief_empty_count(bucket) > pressure_empty_slots)
        return 0u;

    /* Match fc2 local-relief slot dispersion cheaply. */
    start = (unsigned)((now_tsc ^ bk_idx) & (RIX_HASH_BUCKET_ENTRY_SZ - 1u));
    for (unsigned i = 0; i < FC2_RELIEF_SCAN_SLOTS; i++) {
        unsigned slot = (start + i) & (RIX_HASH_BUCKET_ENTRY_SZ - 1u);
        unsigned idx = bucket->idx[slot];
        struct fc2_flow4_entry *entry = bench_fc2_entry_from_idx(pool, idx);

        if (entry == NULL || entry->last_ts == 0u)
            continue;
        if (now_tsc - entry->last_ts <= eff_timeout_tsc)
            continue;
        if (entry->last_ts < oldest_ts) {
            oldest_ts = entry->last_ts;
            victim = entry;
        }
    }
    return (victim != NULL) ? RIX_IDX_FROM_PTR(pool, victim) : 0u;
}

static inline unsigned
bench_maint_scan_slots(const struct rix_hash_bucket_s *bucket,
                       struct fc2_flow4_entry *pool,
                       uint64_t now_tsc,
                       uint64_t eff_timeout_tsc,
                       unsigned bk_idx,
                       unsigned slots_per_bucket,
                       unsigned *expired_slots)
{
    unsigned expired = 0u;
    unsigned start;

    /* Cheaply vary the starting slot so maintenance does not bias low slots. */
    start = (unsigned)((now_tsc ^ bk_idx) & (RIX_HASH_BUCKET_ENTRY_SZ - 1u));
    for (unsigned i = 0; i < slots_per_bucket; i++) {
        unsigned slot = (start + i) & (RIX_HASH_BUCKET_ENTRY_SZ - 1u);
        unsigned idx = bucket->idx[slot];
        struct fc2_flow4_entry *entry = bench_fc2_entry_from_idx(pool, idx);

        if (entry == NULL || entry->last_ts == 0u)
            continue;
        if (now_tsc - entry->last_ts <= eff_timeout_tsc)
            continue;
        expired_slots[expired++] = slot;
    }
    return expired;
}

static unsigned
bench_maintain_grouped(struct new_cache_ctx *ctx,
                       unsigned start_bk,
                       unsigned bucket_budget,
                       unsigned lanes_per_wave,
                       unsigned slots_per_bucket,
                       uint64_t now_tsc)
{
    unsigned evicted = 0u;
    unsigned nb_bk = ctx->fc.nb_bk;

    if (nb_bk == 0u || bucket_budget == 0u || lanes_per_wave == 0u ||
        slots_per_bucket == 0u)
        return 0u;

    start_bk %= nb_bk;
    for (unsigned base = 0; base < bucket_budget; base += lanes_per_wave) {
        unsigned victims[16][RIX_HASH_BUCKET_ENTRY_SZ];
        unsigned victim_counts[16] = {
            0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
            0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
        };
        unsigned bks[16];
        unsigned lanes = bucket_budget - base;

        if (lanes > 16u)
            lanes = 16u;
        if (lanes > lanes_per_wave)
            lanes = lanes_per_wave;
        for (unsigned lane = 0; lane < lanes; lane++) {
            unsigned bk = start_bk + base + lane;

            if (bk >= nb_bk)
                bk -= nb_bk;
            bks[lane] = bk;
            victim_counts[lane] =
                bench_maint_scan_slots(&ctx->buckets[bk], ctx->pool,
                                       now_tsc, ctx->fc.eff_timeout_tsc,
                                       bk, slots_per_bucket, victims[lane]);
        }
        for (unsigned lane = 0; lane < lanes; lane++) {
            for (unsigned i = 0; i < victim_counts[lane]; i++) {
                unsigned removed_idx = bench_fc2_remove_at(ctx, bks[lane],
                                                           victims[lane][i]);
                struct fc2_flow4_entry *entry;

                if (removed_idx == (unsigned)RIX_NIL)
                    continue;
                entry = bench_fc2_entry_from_idx(ctx->pool, removed_idx);
                bench_fc2_free_entry(ctx, entry);
                evicted++;
            }
        }
    }
    return evicted;
}

static void
bench_relief_only(void)
{
    enum { RELIEF_REPEAT = 200000u };
    struct new_cache_ctx ctx;
    struct rix_hash_bucket_s *bucket;
    uint64_t t0, t1;
    double naive_cy;
    double sampled_cy;
    uint64_t now_tsc = UINT64_C(10000000);
    uint64_t eff_timeout_tsc = UINT64_C(1000);
    unsigned victim_idx = 0u;

    new_cache_setup(&ctx, 1024u, 16384u);
    bucket = &ctx.buckets[17];

    for (unsigned i = 0; i < RIX_HASH_BUCKET_ENTRY_SZ; i++) {
        unsigned entry_idx = i + 1u;
        struct fc2_flow4_entry *entry = &ctx.pool[i];

        entry->key = bench_make_key2(1000u + i);
        entry->cur_hash = 17u;
        entry->slot = (uint16_t)i;
        entry->last_ts = now_tsc - (uint64_t)((i + 1u) * 200u);
        bucket->idx[i] = entry_idx;
        bucket->hash[i] = 0x100u + i;
    }

    t0 = flow_cache_rdtsc();
    for (unsigned r = 0; r < RELIEF_REPEAT; r++) {
        victim_idx = bench_relief_naive_scan(bucket, ctx.pool,
                                             now_tsc + r, eff_timeout_tsc,
                                             FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS,
                                             17u);
        bench_relief_sink += victim_idx;
    }
    t1 = flow_cache_rdtsc();
    naive_cy = (double)(t1 - t0) / (double)RELIEF_REPEAT;

    t0 = flow_cache_rdtsc();
    for (unsigned r = 0; r < RELIEF_REPEAT; r++) {
        victim_idx = bench_relief_sampled_scan(bucket, ctx.pool,
                                               now_tsc + r, eff_timeout_tsc,
                                               FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS,
                                               17u);
        bench_relief_sink += victim_idx;
    }
    t1 = flow_cache_rdtsc();
    sampled_cy = (double)(t1 - t0) / (double)RELIEF_REPEAT;

    printf("\nfc2 relief-only scan (single hot bucket, cycles/call)\n");
    printf("full16=%8.2f  sample8=%8.2f  victim=%u\n",
           naive_cy, sampled_cy, victim_idx);

    free(ctx.pool);
    free(ctx.buckets);
}

static void
bench_maintain_only(unsigned bucket_budget)
{
    enum { NB_BK = 65536u, MAX_ENTRIES = 1048576u };
    struct new_cache_ctx ctx;
    struct fc2_flow4_key *prefill_keys;
    unsigned total_slots = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (total_slots * 85u) / 100u;
    uint64_t now = UINT64_C(1000000000);
    uint64_t t0, t1;
    uint64_t evicted = 0u;

    new_cache_setup(&ctx, NB_BK, MAX_ENTRIES);
    prefill_keys = bench_alloc((size_t)MAX_ENTRIES * sizeof(*prefill_keys));
    for (unsigned i = 0; i < MAX_ENTRIES; i++)
        prefill_keys[i] = bench_make_key2(i);

    (void)new_prefill(&ctx, prefill_keys, start_entries, 1u);
    for (unsigned i = 0; i < start_entries; i++)
        ctx.pool[i].last_ts = 1u;

    t0 = flow_cache_rdtsc();
    for (unsigned bk = 0; bk < NB_BK; bk += bucket_budget)
        evicted += fc2_flow4_cache_maintain(&ctx.fc, bk, bucket_budget, now);
    t1 = flow_cache_rdtsc();

    printf("fc2 maintain-only: nb_bk=%u budget=%u start_fill=85%% cycles/bucket=%8.2f"
           " evicted=%" PRIu64 "\n",
           NB_BK,
           bucket_budget,
           (double)(t1 - t0) / (double)NB_BK,
           evicted);

    free(prefill_keys);
    free(ctx.pool);
    free(ctx.buckets);
}

static void
bench_maintain_only_paused(unsigned bucket_budget)
{
    enum { NB_BK = 65536u, MAX_ENTRIES = 1048576u };
    struct new_cache_ctx ctx;
    struct fc2_flow4_key *prefill_keys;
    unsigned total_slots = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (total_slots * 85u) / 100u;
    uint64_t now = UINT64_C(1000000000);
    uint64_t t0, t1;
    uint64_t evicted = 0u;

    new_cache_setup(&ctx, NB_BK, MAX_ENTRIES);
    prefill_keys = bench_alloc((size_t)MAX_ENTRIES * sizeof(*prefill_keys));
    for (unsigned i = 0; i < MAX_ENTRIES; i++)
        prefill_keys[i] = bench_make_key2(i);

    (void)new_prefill(&ctx, prefill_keys, start_entries, 1u);
    for (unsigned i = 0; i < start_entries; i++)
        ctx.pool[i].last_ts = 1u;

    printf("[perf-ready] pid=%ld mode=maint%u nb_bk=%u budget=%u start_fill=85%%\n",
           (long)getpid(), bucket_budget, NB_BK, bucket_budget);
    fflush(stdout);
    raise(SIGSTOP);

    t0 = flow_cache_rdtsc();
    for (unsigned bk = 0; bk < NB_BK; bk += bucket_budget)
        evicted += fc2_flow4_cache_maintain(&ctx.fc, bk, bucket_budget, now);
    t1 = flow_cache_rdtsc();

    printf("fc2 maintain-only isolated: nb_bk=%u budget=%u start_fill=85%% cycles/bucket=%8.2f"
           " evicted=%" PRIu64 "\n",
           NB_BK,
           bucket_budget,
           (double)(t1 - t0) / (double)NB_BK,
           evicted);

    free(prefill_keys);
    free(ctx.pool);
    free(ctx.buckets);
}

static void
bench_maintain_grouped_only(unsigned lanes,
                            unsigned slots_per_bucket)
{
    enum { NB_BK = 65536u, MAX_ENTRIES = 1048576u };
    struct new_cache_ctx ctx;
    struct fc2_flow4_key *prefill_keys;
    unsigned total_slots = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (total_slots * 85u) / 100u;
    uint64_t now = UINT64_C(1000000000);
    uint64_t t0, t1;
    uint64_t evicted = 0u;

    new_cache_setup(&ctx, NB_BK, MAX_ENTRIES);
    prefill_keys = bench_alloc((size_t)MAX_ENTRIES * sizeof(*prefill_keys));
    for (unsigned i = 0; i < MAX_ENTRIES; i++)
        prefill_keys[i] = bench_make_key2(i);

    (void)new_prefill(&ctx, prefill_keys, start_entries, 1u);
    ctx.fc.eff_timeout_tsc = ctx.fc.timeout_tsc ? (ctx.fc.timeout_tsc >> 2) : 1u;
    if (ctx.fc.eff_timeout_tsc == 0u)
        ctx.fc.eff_timeout_tsc = 1u;
    for (unsigned i = 0; i < start_entries; i++)
        ctx.pool[i].last_ts = 1u;

    t0 = flow_cache_rdtsc();
    for (unsigned bk = 0; bk < NB_BK; bk += lanes)
        evicted += bench_maintain_grouped(&ctx, bk, lanes,
                                          lanes, slots_per_bucket, now);
    t1 = flow_cache_rdtsc();

    printf("fc2 maintain-%ux%u: nb_bk=%u start_fill=85%% cycles/bucket=%8.2f"
           " evicted=%" PRIu64 "\n",
           lanes,
           slots_per_bucket,
           NB_BK,
           (double)(t1 - t0) / (double)NB_BK,
           evicted);

    free(prefill_keys);
    free(ctx.pool);
    free(ctx.buckets);
}

static void
bench_maintain_compare_v1_v2_16(void)
{
    enum { NB_BK = 65536u, MAX_ENTRIES = 1048576u };
    struct new_cache_ctx ctx_v1;
    struct new_cache_ctx ctx_v2;
    struct fc2_flow4_key *prefill_keys;
    unsigned total_slots = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (total_slots * 85u) / 100u;
    uint64_t now = UINT64_C(1000000000);
    uint64_t t0, t1;
    uint64_t evicted_v1 = 0u;
    uint64_t evicted_v2 = 0u;

    prefill_keys = bench_alloc((size_t)MAX_ENTRIES * sizeof(*prefill_keys));
    for (unsigned i = 0; i < MAX_ENTRIES; i++)
        prefill_keys[i] = bench_make_key2(i);

    new_cache_setup(&ctx_v1, NB_BK, MAX_ENTRIES);
    (void)new_prefill(&ctx_v1, prefill_keys, start_entries, 1u);
    ctx_v1.fc.eff_timeout_tsc = ctx_v1.fc.timeout_tsc ?
        (ctx_v1.fc.timeout_tsc >> 2) : 1u;
    if (ctx_v1.fc.eff_timeout_tsc == 0u)
        ctx_v1.fc.eff_timeout_tsc = 1u;
    for (unsigned i = 0; i < start_entries; i++)
        ctx_v1.pool[i].last_ts = 1u;

    t0 = flow_cache_rdtsc();
    for (unsigned bk = 0; bk < NB_BK; bk += 16u)
        evicted_v1 += bench_maintain_grouped(&ctx_v1, bk, 16u, 16u, 16u, now);
    t1 = flow_cache_rdtsc();
    printf("fc2 maintain-v1-16x16: nb_bk=%u start_fill=85%% cycles/bucket=%8.2f"
           " evicted=%" PRIu64 "\n",
           NB_BK,
           (double)(t1 - t0) / (double)NB_BK,
           evicted_v1);

    new_cache_setup(&ctx_v2, NB_BK, MAX_ENTRIES);
    (void)new_prefill(&ctx_v2, prefill_keys, start_entries, 1u);
    ctx_v2.fc.eff_timeout_tsc = ctx_v2.fc.timeout_tsc ?
        (ctx_v2.fc.timeout_tsc >> 2) : 1u;
    if (ctx_v2.fc.eff_timeout_tsc == 0u)
        ctx_v2.fc.eff_timeout_tsc = 1u;
    for (unsigned i = 0; i < start_entries; i++)
        ctx_v2.pool[i].last_ts = 1u;

    t0 = flow_cache_rdtsc();
    for (unsigned bk = 0; bk < NB_BK; bk += 16u)
        evicted_v2 += fc2_flow4_cache_maintain(&ctx_v2.fc, bk, 16u, now);
    t1 = flow_cache_rdtsc();
    printf("fc2 maintain-v2-16x16: nb_bk=%u start_fill=85%% cycles/bucket=%8.2f"
           " evicted=%" PRIu64 "\n",
           NB_BK,
           (double)(t1 - t0) / (double)NB_BK,
           evicted_v2);

    free(prefill_keys);
    free(ctx_v1.pool);
    free(ctx_v1.buckets);
    free(ctx_v2.pool);
    free(ctx_v2.buckets);
}

static void
bench_maintain_v1_16_paused(void)
{
    enum { NB_BK = 65536u, MAX_ENTRIES = 1048576u };
    struct new_cache_ctx ctx;
    struct fc2_flow4_key *prefill_keys;
    unsigned total_slots = NB_BK * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (total_slots * 85u) / 100u;
    uint64_t now = UINT64_C(1000000000);
    uint64_t t0, t1;
    uint64_t evicted = 0u;

    new_cache_setup(&ctx, NB_BK, MAX_ENTRIES);
    prefill_keys = bench_alloc((size_t)MAX_ENTRIES * sizeof(*prefill_keys));
    for (unsigned i = 0; i < MAX_ENTRIES; i++)
        prefill_keys[i] = bench_make_key2(i);

    (void)new_prefill(&ctx, prefill_keys, start_entries, 1u);
    ctx.fc.eff_timeout_tsc = ctx.fc.timeout_tsc ? (ctx.fc.timeout_tsc >> 2) : 1u;
    if (ctx.fc.eff_timeout_tsc == 0u)
        ctx.fc.eff_timeout_tsc = 1u;
    for (unsigned i = 0; i < start_entries; i++)
        ctx.pool[i].last_ts = 1u;

    printf("[perf-ready] pid=%ld mode=maintv1_16x16 nb_bk=%u budget=16 start_fill=85%%\n",
           (long)getpid(), NB_BK);
    fflush(stdout);
    raise(SIGSTOP);

    t0 = flow_cache_rdtsc();
    for (unsigned bk = 0; bk < NB_BK; bk += 16u)
        evicted += bench_maintain_grouped(&ctx, bk, 16u, 16u, 16u, now);
    t1 = flow_cache_rdtsc();

    printf("fc2 maintain-v1-16x16 isolated: nb_bk=%u start_fill=85%% cycles/bucket=%8.2f"
           " evicted=%" PRIu64 "\n",
           NB_BK,
           (double)(t1 - t0) / (double)NB_BK,
           evicted);

    free(prefill_keys);
    free(ctx.pool);
    free(ctx.buckets);
}

static unsigned
old_prefill(struct old_cache_ctx *ctx,
            const struct flow4_key *keys,
            unsigned n,
            uint64_t now)
{
    for (unsigned i = 0; i < n; i++)
        (void)flow4_cache_insert(&ctx->fc, &keys[i], now);
    return flow4_cache_nb_entries(&ctx->fc);
}

static unsigned
new_prefill(struct new_cache_ctx *ctx,
            const struct fc2_flow4_key *keys,
            unsigned n,
            uint64_t now)
{
    unsigned offset = 0u;
    struct fc2_flow4_result *results =
        bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    uint16_t *miss_idx =
        bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    while (offset < n) {
        unsigned chunk = n - offset;
        unsigned miss_count;

        if (chunk > FC2_BENCH_QUERY)
            chunk = FC2_BENCH_QUERY;
        miss_count = fc2_flow4_cache_lookup_batch(&ctx->fc, keys + offset, chunk, now,
                                                  results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&ctx->fc, keys + offset, miss_idx,
                                              miss_count, now, results);
        offset += chunk;
    }
    free(miss_idx);
    free(results);
    return fc2_flow4_cache_nb_entries(&ctx->fc);
}

static unsigned
bench_old_active_scan(const struct old_cache_ctx *ctx)
{
    unsigned n = 0u;

    for (unsigned i = 0; i < ctx->fc.max_entries; i++)
        if (ctx->pool[i].last_ts != 0u)
            n++;
    return n;
}

static unsigned
bench_new_active_scan(const struct new_cache_ctx *ctx)
{
    unsigned n = 0u;

    for (unsigned i = 0; i < ctx->fc.max_entries; i++)
        if (ctx->pool[i].last_ts != 0u)
            n++;
    return n;
}

static double
bench_old_hit(struct old_cache_ctx *ctx,
              const struct flow4_key *keys,
              unsigned n,
              unsigned repeat)
{
    struct flow4_entry **results = bench_alloc((size_t)n * sizeof(*results));
    uint64_t t0, t1;

    for (unsigned w = 0; w < 20u; w++) {
        assert(flow4_cache_lookup_touch_batch(&ctx->fc, keys, n,
                                              (uint64_t)w + 1u, results) == 0u);
    }

    t0 = flow_cache_rdtsc();
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 100u;
        assert(flow4_cache_lookup_touch_batch(&ctx->fc, keys, n, now,
                                              results) == 0u);
    }
    t1 = flow_cache_rdtsc();

    free(results);
    return (double)(t1 - t0) / (double)(n * repeat);
}

static double
bench_old_pkt_hit(struct old_cache_ctx *ctx,
                  const struct flow4_key *keys,
                  unsigned n,
                  unsigned repeat)
{
    struct flow4_entry **results = bench_alloc((size_t)n * sizeof(*results));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 100u;
        uint64_t t0, t1;

        t0 = flow_cache_rdtsc();
        assert(flow4_cache_lookup_touch_batch(&ctx->fc, keys, n, now, results) == 0u);
        flow4_cache_adjust_timeout(&ctx->fc, 0u);
        flow4_cache_expire(&ctx->fc, now);
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(results);
    return (double)total / (double)(n * repeat);
}

static double
bench_new_hit(struct new_cache_ctx *ctx,
              const struct fc2_flow4_key *keys,
              unsigned n,
              unsigned repeat)
{
    struct fc2_flow4_result *results = bench_alloc((size_t)n * sizeof(*results));
    uint16_t *miss_idx = bench_alloc((size_t)n * sizeof(*miss_idx));
    uint64_t t0, t1;

    for (unsigned w = 0; w < 20u; w++)
        assert(fc2_flow4_cache_lookup_batch(&ctx->fc, keys, n, (uint64_t)w + 1u,
                                            results, miss_idx) == 0u);

    t0 = flow_cache_rdtsc();
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 100u;
        assert(fc2_flow4_cache_lookup_batch(&ctx->fc, keys, n, now,
                                            results, miss_idx) == 0u);
    }
    t1 = flow_cache_rdtsc();

    free(miss_idx);
    free(results);
    return (double)(t1 - t0) / (double)(n * repeat);
}

static double
bench_new_pkt_hit(struct new_cache_ctx *ctx,
                  const struct fc2_flow4_key *keys,
                  unsigned n,
                  unsigned repeat)
{
    struct fc2_flow4_result *results = bench_alloc((size_t)n * sizeof(*results));
    uint16_t *miss_idx = bench_alloc((size_t)n * sizeof(*miss_idx));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 100u;
        uint64_t t0, t1;

        t0 = flow_cache_rdtsc();
        assert(fc2_flow4_cache_lookup_batch(&ctx->fc, keys, n, now,
                                            results, miss_idx) == 0u);
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(miss_idx);
    free(results);
    return (double)total / (double)(n * repeat);
}

static double
bench_old_miss_fill(struct old_cache_ctx *ctx,
                    const struct flow4_key *keys,
                    unsigned n,
                    unsigned repeat)
{
    struct flow4_entry **results = bench_alloc((size_t)n * sizeof(*results));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 1000u;
        uint64_t t0, t1;

        old_cache_reset(ctx);
        t0 = flow_cache_rdtsc();
        assert(flow4_cache_lookup_touch_batch(&ctx->fc, keys, n, now,
                                              results) == n);
        for (unsigned i = 0; i < n; i++) {
            assert(results[i] == NULL);
            assert(flow4_cache_insert(&ctx->fc, &keys[i], now) != NULL);
        }
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(results);
    return (double)total / (double)(n * repeat);
}

static double
bench_old_pkt_miss_fill(struct old_cache_ctx *ctx,
                        const struct flow4_key *keys,
                        unsigned n,
                        unsigned repeat)
{
    struct flow4_entry **results = bench_alloc((size_t)n * sizeof(*results));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 1000u;
        uint64_t t0, t1;
        unsigned misses = 0u;

        old_cache_reset(ctx);
        t0 = flow_cache_rdtsc();
        (void)flow4_cache_lookup_touch_batch(&ctx->fc, keys, n, now, results);
        for (unsigned i = 0; i < n; i++) {
            if (results[i] != NULL)
                continue;
            if (flow4_cache_insert(&ctx->fc, &keys[i], now) != NULL)
                misses++;
        }
        flow4_cache_adjust_timeout(&ctx->fc, misses);
        flow4_cache_expire(&ctx->fc, now);
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(results);
    return (double)total / (double)(n * repeat);
}

static double
bench_new_miss_fill(struct new_cache_ctx *ctx,
                    const struct fc2_flow4_key *keys,
                    unsigned n,
                    unsigned repeat)
{
    struct fc2_flow4_result *results = bench_alloc((size_t)n * sizeof(*results));
    uint16_t *miss_idx = bench_alloc((size_t)n * sizeof(*miss_idx));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 1000u;
        uint64_t t0, t1;
        unsigned miss_count;

        new_cache_reset(ctx);
        t0 = flow_cache_rdtsc();
        miss_count = fc2_flow4_cache_lookup_batch(&ctx->fc, keys, n, now,
                                                  results, miss_idx);
        assert(miss_count == n);
        assert(fc2_flow4_cache_fill_miss_batch(&ctx->fc, keys, miss_idx,
                                               miss_count, now, results) == n);
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(miss_idx);
    free(results);
    return (double)total / (double)(n * repeat);
}

static double
bench_new_pkt_miss_fill(struct new_cache_ctx *ctx,
                        const struct fc2_flow4_key *keys,
                        unsigned n,
                        unsigned repeat)
{
    struct fc2_flow4_result *results = bench_alloc((size_t)n * sizeof(*results));
    uint16_t *miss_idx = bench_alloc((size_t)n * sizeof(*miss_idx));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 1000u;
        uint64_t t0, t1;
        unsigned miss_count;

        new_cache_reset(ctx);
        t0 = flow_cache_rdtsc();
        miss_count = fc2_flow4_cache_lookup_batch(&ctx->fc, keys, n, now,
                                                  results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&ctx->fc, keys, miss_idx,
                                              miss_count, now, results);
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(miss_idx);
    free(results);
    return (double)total / (double)(n * repeat);
}

static double
bench_old_mixed(struct old_cache_ctx *ctx,
                const struct flow4_key *prefill_keys,
                unsigned prefill_n,
                const struct flow4_key *query_keys,
                unsigned query_n,
                unsigned repeat)
{
    struct flow4_entry **results = bench_alloc((size_t)query_n * sizeof(*results));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 5000u;
        uint64_t t0, t1;

        old_cache_reset(ctx);
        (void)old_prefill(ctx, prefill_keys, prefill_n, now);

        t0 = flow_cache_rdtsc();
        (void)flow4_cache_lookup_touch_batch(&ctx->fc, query_keys, query_n, now,
                                             results);
        for (unsigned i = 0; i < query_n; i++) {
            if (results[i] == NULL)
                assert(flow4_cache_insert(&ctx->fc, &query_keys[i], now) != NULL);
        }
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(results);
    return (double)total / (double)(query_n * repeat);
}

static double
bench_old_pkt_mixed(struct old_cache_ctx *ctx,
                    const struct flow4_key *prefill_keys,
                    unsigned prefill_n,
                    const struct flow4_key *query_keys,
                    unsigned query_n,
                    unsigned repeat)
{
    struct flow4_entry **results = bench_alloc((size_t)query_n * sizeof(*results));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 5000u;
        uint64_t t0, t1;
        unsigned misses = 0u;

        old_cache_reset(ctx);
        (void)old_prefill(ctx, prefill_keys, prefill_n, now);

        t0 = flow_cache_rdtsc();
        (void)flow4_cache_lookup_touch_batch(&ctx->fc, query_keys, query_n, now,
                                             results);
        for (unsigned i = 0; i < query_n; i++) {
            if (results[i] != NULL)
                continue;
            if (flow4_cache_insert(&ctx->fc, &query_keys[i], now) != NULL)
                misses++;
        }
        flow4_cache_adjust_timeout(&ctx->fc, misses);
        flow4_cache_expire(&ctx->fc, now);
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(results);
    return (double)total / (double)(query_n * repeat);
}

static double
bench_new_mixed(struct new_cache_ctx *ctx,
                const struct fc2_flow4_key *prefill_keys,
                unsigned prefill_n,
                const struct fc2_flow4_key *query_keys,
                unsigned query_n,
                unsigned repeat)
{
    struct fc2_flow4_result *results = bench_alloc((size_t)query_n * sizeof(*results));
    uint16_t *miss_idx = bench_alloc((size_t)query_n * sizeof(*miss_idx));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 5000u;
        uint64_t t0, t1;
        unsigned miss_count;

        new_cache_reset(ctx);
        (void)new_prefill(ctx, prefill_keys, prefill_n, now);

        t0 = flow_cache_rdtsc();
        miss_count = fc2_flow4_cache_lookup_batch(&ctx->fc, query_keys, query_n, now,
                                                  results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&ctx->fc, query_keys, miss_idx,
                                              miss_count, now, results);
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(miss_idx);
    free(results);
    return (double)total / (double)(query_n * repeat);
}

static double
bench_new_pkt_mixed(struct new_cache_ctx *ctx,
                    const struct fc2_flow4_key *prefill_keys,
                    unsigned prefill_n,
                    const struct fc2_flow4_key *query_keys,
                    unsigned query_n,
                    unsigned repeat)
{
    struct fc2_flow4_result *results = bench_alloc((size_t)query_n * sizeof(*results));
    uint16_t *miss_idx = bench_alloc((size_t)query_n * sizeof(*miss_idx));
    uint64_t total = 0u;

    for (unsigned r = 0; r < repeat; r++) {
        uint64_t now = (uint64_t)r + 5000u;
        uint64_t t0, t1;
        unsigned miss_count;

        new_cache_reset(ctx);
        (void)new_prefill(ctx, prefill_keys, prefill_n, now);

        t0 = flow_cache_rdtsc();
        miss_count = fc2_flow4_cache_lookup_batch(&ctx->fc, query_keys, query_n, now,
                                                  results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&ctx->fc, query_keys, miss_idx,
                                              miss_count, now, results);
        t1 = flow_cache_rdtsc();
        total += t1 - t0;
    }

    free(miss_idx);
    free(results);
    return (double)total / (double)(query_n * repeat);
}

static void
bench_emit3(const char *label, double old_auto, double old_gen, double new_cy)
{
    printf("%-18s auto=%8.2f  gen=%8.2f  fc2=%8.2f\n",
           label, old_auto, old_gen, new_cy);
}

static void
bench_emit2(const char *label, double old_auto, double new_cy)
{
    printf("%-18s old=%8.2f  fc2=%8.2f\n", label, old_auto, new_cy);
}

static void
bench_emit_relief(const char *label, const struct new_stats_delta *stats)
{
    printf("%-18s relief_calls=%" PRIu64 "  bucket_checks=%" PRIu64
           "  evict=%" PRIu64 " (bk0=%" PRIu64 ", bk1=%" PRIu64 ")  full=%" PRIu64 "\n",
           label,
           stats->relief_calls,
           stats->relief_bucket_checks,
           stats->relief_evictions,
           stats->relief_bk0_evictions,
           stats->relief_bk1_evictions,
           stats->fill_full);
}

static double
bench_new_bk0_rate(const struct new_cache_ctx *ctx)
{
    uint64_t in_bk0 = 0u;
    uint64_t in_bk1 = 0u;
    unsigned mask = ctx->fc.ht_head.rhh_mask;

    for (unsigned i = 0; i < ctx->fc.max_entries; i++) {
        const struct fc2_flow4_entry *e = &ctx->pool[i];
        union rix_hash_hash_u h;
        unsigned bk0;
        unsigned cur_bk;

        if (e->last_ts == 0u)
            continue;
        h = rix_hash_hash_bytes_fast(&e->key, sizeof(e->key), mask);
        bk0 = h.val32[0] & mask;
        cur_bk = e->cur_hash & mask;
        if (cur_bk == bk0)
            in_bk0++;
        else
            in_bk1++;
    }
    return (in_bk0 + in_bk1) ? (100.0 * (double)in_bk0 / (double)(in_bk0 + in_bk1)) : 0.0;
}

static void
bench_fill_sweep(const struct flow4_key *old_prefill_keys,
                 const struct fc2_flow4_key *new_prefill_keys,
                 unsigned max_entries)
{
    static const unsigned fill_pct_list[] = { 25u, 50u, 75u, 85u, 90u };
    struct old_cache_ctx old_auto;
    struct new_cache_ctx newc;
    unsigned nb_bk = 2048u;
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    struct flow4_key *old_query_hit = bench_alloc((size_t)FC2_BENCH_SWEEP_QUERY *
                                                  sizeof(*old_query_hit));
    struct fc2_flow4_key *new_query_hit = bench_alloc((size_t)FC2_BENCH_SWEEP_QUERY *
                                                      sizeof(*new_query_hit));

    printf("\nflow4 fill sweep (fair hit path, cycles/pkt)\n");
    printf("fill%%  nb_bk  actual%%  fc2_bk0%%  old_pkt  fc2_pkt  relief\n");
    printf("-----  -----  -------  --------  -------  -------  ------\n");

    for (unsigned p = 0; p < sizeof(fill_pct_list) / sizeof(fill_pct_list[0]); p++) {
        unsigned pct = fill_pct_list[p];
        unsigned target_entries = (unsigned)(((uint64_t)total_slots * pct) / 100u);
        unsigned old_entries;
        unsigned new_entries;
        unsigned active_entries;
        double new_bk0;
        double old_hit;
        double new_hit;
        struct new_stats_delta before;
        struct new_stats_delta after;
        struct new_stats_delta delta;
        if (target_entries > max_entries)
            target_entries = max_entries;
        old_cache_setup(&old_auto, nb_bk, max_entries, FLOW_CACHE_BACKEND_AUTO);
        new_cache_setup(&newc, nb_bk, max_entries);
        old_entries = old_prefill(&old_auto, old_prefill_keys, target_entries, 1u);
        new_entries = new_prefill(&newc, new_prefill_keys, target_entries, 1u);
        active_entries = (old_entries < new_entries) ? old_entries : new_entries;
        if (active_entries == 0u) {
            printf("%5u  %5u  %7.1f  %8.1f  %7s  %7s\n",
                   pct, nb_bk, 0.0, 0.0, "skip", "skip");
            free(newc.pool);
            free(newc.buckets);
            free(old_auto.pool);
            free(old_auto.buckets);
            continue;
        }

        for (unsigned i = 0; i < FC2_BENCH_SWEEP_QUERY; i++) {
            unsigned idx = i % active_entries;
            old_query_hit[i] = old_prefill_keys[idx];
            new_query_hit[i] = new_prefill_keys[idx];
        }

        before = bench_new_stats_snapshot(&newc);
        new_bk0 = bench_new_bk0_rate(&newc);
        old_hit = bench_old_pkt_hit(&old_auto, old_query_hit, FC2_BENCH_SWEEP_QUERY, 80u);
        new_hit = bench_new_pkt_hit(&newc, new_query_hit, FC2_BENCH_SWEEP_QUERY, 80u);
        after = bench_new_stats_snapshot(&newc);
        delta = bench_new_stats_diff(after, before);

        printf("%5u  %5u  %7.1f  %8.1f  %7.2f  %7.2f  %6" PRIu64 "\n",
               pct, nb_bk, 100.0 * (double)active_entries / (double)total_slots,
               new_bk0, old_hit, new_hit, delta.relief_evictions);

        free(newc.pool);
        free(newc.buckets);
        free(old_auto.pool);
        free(old_auto.buckets);
    }

    free(new_query_hit);
    free(old_query_hit);
}

static void
bench_fill_growth(unsigned start_fill_pct,
                  unsigned nb_bk,
                  unsigned max_entries,
                  unsigned rounds,
                  unsigned query_n)
{
    struct new_cache_ctx ctx;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned active_entries;
    unsigned start_active;
    unsigned max_seen;
    uint64_t now = 1u;
    uint64_t now_step = UINT64_C(125000);

    if (start_entries > max_entries)
        start_entries = max_entries;

    new_cache_setup(&ctx, nb_bk, max_entries);
    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)query_n * sizeof(*query_keys));
    results = bench_alloc((size_t)query_n * sizeof(*results));
    miss_idx = bench_alloc((size_t)query_n * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    active_entries = new_prefill(&ctx, prefill_keys, start_entries, now);
    start_active = active_entries;
    max_seen = active_entries;
    before = bench_new_stats_snapshot(&ctx);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned miss_base = 1000000u + round * query_n;
        unsigned miss_count;

        now += now_step;
        for (unsigned i = 0; i < query_n; i++) {
            if ((i % 10u) == 9u) {
                query_keys[i] = bench_make_key2(miss_base + i);
            } else {
                unsigned idx = (round * 17u + i * 13u) % active_entries;
                query_keys[i] = prefill_keys[idx];
            }
        }

        miss_count = fc2_flow4_cache_lookup_batch(&ctx.fc, query_keys, query_n, now,
                                                  results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&ctx.fc, query_keys, miss_idx,
                                              miss_count, now, results);
        active_entries = fc2_flow4_cache_nb_entries(&ctx.fc);
        if (active_entries > max_seen)
            max_seen = active_entries;
    }

    after = bench_new_stats_snapshot(&ctx);
    delta = bench_new_stats_diff(after, before);
    printf("\nfc2 fill growth (persistent mixed_90_10, cycles not measured)\n");
    printf("req_fill=%u%%  actual_start_fill=%.1f%%  rounds=%u  query=%u  step_tsc=%" PRIu64 "\n",
           start_fill_pct,
           100.0 * (double)start_entries / (double)total_slots,
           rounds,
           query_n,
           now_step);
    printf("start_entries=%u  end_entries=%u  max_entries_seen=%u  end_fill=%.1f%%\n",
           start_active,
           active_entries,
           max_seen,
           100.0 * (double)active_entries / (double)total_slots);
    printf("lookups=%" PRIu64 "  misses=%" PRIu64 "  fills=%" PRIu64 "  full=%" PRIu64 "\n",
           delta.lookups, delta.misses, delta.fills, delta.fill_full);
    printf("relief_calls=%" PRIu64 "  bucket_checks=%" PRIu64
           "  evictions=%" PRIu64 " (bk0=%" PRIu64 ", bk1=%" PRIu64 ")\n",
           delta.relief_calls,
           delta.relief_bucket_checks,
           delta.relief_evictions,
           delta.relief_bk0_evictions,
           delta.relief_bk1_evictions);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(ctx.pool);
    free(ctx.buckets);
}

static void
bench_rate_limited_compare_timeout(unsigned desired_entries,
                                   unsigned nb_bk,
                                   unsigned start_fill_pct,
                                   unsigned hit_pct,
                                   unsigned pps,
                                   unsigned timeout_ms)
{
    struct old_cache_ctx old_auto;
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct flow4_key *old_prefill_keys;
    struct fc2_flow4_key *new_prefill_keys;
    struct flow4_key *old_query;
    struct fc2_flow4_key *new_query;
    struct flow4_entry **old_results;
    struct fc2_flow4_result *new_results;
    uint16_t *miss_idx;
    struct old_stats_delta old_before;
    struct old_stats_delta old_after;
    struct old_stats_delta old_delta;
    struct new_stats_delta new_before;
    struct new_stats_delta new_after;
    struct new_stats_delta new_delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned old_start_active;
    unsigned new_start_active;
    unsigned old_active;
    unsigned new_active;
    unsigned old_max_seen;
    unsigned new_max_seen;
    unsigned old_expire_credit = 0u;
    unsigned old_expire_misses = 0u;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t old_total_cycles = 0u;
    uint64_t new_total_cycles = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    rounds += rounds / 2u;

    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    old_cache_setup_timeout(&old_auto, nb_bk, max_entries, FLOW_CACHE_BACKEND_AUTO, timeout_ms);
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    old_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*old_prefill_keys));
    new_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*new_prefill_keys));
    old_query = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_query));
    new_query = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_query));
    old_results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_results));
    new_results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++) {
        old_prefill_keys[i] = bench_make_key(i);
        new_prefill_keys[i] = bench_make_key2(i);
    }

    now = 1u;
    (void)old_prefill(&old_auto, old_prefill_keys, start_entries, now);
    (void)new_prefill(&newc, new_prefill_keys, start_entries, now);
    old_start_active = bench_old_active_scan(&old_auto);
    new_start_active = bench_new_active_scan(&newc);
    old_active = old_start_active;
    new_active = new_start_active;
    old_max_seen = old_start_active;
    new_max_seen = new_start_active;
    old_before = bench_old_stats_snapshot(&old_auto);
    new_before = bench_new_stats_snapshot(&newc);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;
        unsigned miss_base = 10000000u + round * FC2_BENCH_QUERY;
        unsigned misses = 0u;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                old_query[i] = old_prefill_keys[idx];
                new_query[i] = new_prefill_keys[idx];
            } else {
                old_query[i] = bench_make_key(miss_base + i);
                new_query[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        (void)flow4_cache_lookup_touch_batch(&old_auto.fc, old_query, FC2_BENCH_QUERY,
                                             now, old_results);
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (old_results[i] != NULL)
                continue;
            if (flow4_cache_insert(&old_auto.fc, &old_query[i], now) != NULL)
                misses++;
        }
        old_expire_credit += FC2_BENCH_QUERY;
        old_expire_misses += misses;
        if (old_expire_credit >= FC2_BENCH_OLD_EXPIRE_PERIOD) {
            flow4_cache_adjust_timeout(&old_auto.fc, old_expire_misses);
            flow4_cache_expire(&old_auto.fc, now);
            old_expire_credit = 0u;
            old_expire_misses = 0u;
        }
        t1 = flow_cache_rdtsc();
        old_total_cycles += t1 - t0;
        old_active = bench_old_active_scan(&old_auto);
        if (old_active > old_max_seen)
            old_max_seen = old_active;

        t0 = flow_cache_rdtsc();
        misses = fc2_flow4_cache_lookup_batch(&newc.fc, new_query, FC2_BENCH_QUERY,
                                              now, new_results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, new_query, miss_idx,
                                              misses, now, new_results);
        t1 = flow_cache_rdtsc();
        new_total_cycles += t1 - t0;
        new_active = bench_new_active_scan(&newc);
        if (new_active > new_max_seen)
            new_max_seen = new_active;
    }

    if (old_expire_credit != 0u) {
        uint64_t t0 = flow_cache_rdtsc();
        flow4_cache_adjust_timeout(&old_auto.fc, old_expire_misses);
        flow4_cache_expire(&old_auto.fc, now);
        uint64_t t1 = flow_cache_rdtsc();
        old_total_cycles += t1 - t0;
    }

    old_after = bench_old_stats_snapshot(&old_auto);
    new_after = bench_new_stats_snapshot(&newc);
    old_delta = bench_old_stats_diff(old_after, old_before);
    new_delta = bench_new_stats_diff(new_after, new_before);

    printf("\nflow4 persistent packet path (%u-entry class, AVX2 auto vs fc2)\n",
           desired_entries);
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=%ums  nb_bk=%u  pool=%u\n",
           pps,
           hit_pct,
           start_fill_pct,
           timeout_ms,
           nb_bk,
           max_entries);
    printf("old : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  evict=%" PRIu64 "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)old_total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           old_delta.lookups ? (100.0 * (double)old_delta.hits / (double)old_delta.lookups) : 0.0,
           old_delta.misses,
           old_delta.evictions,
           100.0 * (double)old_start_active / (double)total_slots,
           100.0 * (double)old_active / (double)total_slots,
           100.0 * (double)old_max_seen / (double)total_slots);
    printf("fc2 : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%  bk0-relief=%" PRIu64
           "  bk1-relief=%" PRIu64 "\n",
           (double)new_total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           new_delta.lookups ? (100.0 * (double)new_delta.hits / (double)new_delta.lookups) : 0.0,
           new_delta.misses,
           new_delta.relief_evictions,
           new_delta.maint_calls,
           new_delta.maint_evictions,
           100.0 * (double)new_start_active / (double)total_slots,
           100.0 * (double)new_active / (double)total_slots,
           100.0 * (double)new_max_seen / (double)total_slots,
           new_delta.relief_bk0_evictions,
           new_delta.relief_bk1_evictions);

    free(miss_idx);
    free(new_results);
    free(old_results);
    free(new_query);
    free(old_query);
    free(new_prefill_keys);
    free(old_prefill_keys);
    free(newc.pool);
    free(newc.buckets);
    free(old_auto.pool);
    free(old_auto.buckets);
}

static void
bench_rate_limited_compare(unsigned desired_entries,
                           unsigned nb_bk,
                           unsigned start_fill_pct,
                           unsigned hit_pct,
                           unsigned pps)
{
    bench_rate_limited_compare_timeout(desired_entries, nb_bk, start_fill_pct,
                                       hit_pct, pps, 1000u);
}

static void
bench_rate_limited_compare_maint16x1(unsigned desired_entries,
                                     unsigned nb_bk,
                                     unsigned hit_pct,
                                     unsigned pps)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (total_slots * 85u) / 100u;
    unsigned rounds;
    unsigned active;
    unsigned max_seen;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t total_cycles = 0u;
    unsigned maint_cursor = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    rounds += rounds / 2u;

    new_cache_setup(&newc, nb_bk, max_entries);
    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query_keys));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    active = bench_new_active_scan(&newc);
    max_seen = active;
    before = bench_new_stats_snapshot(&newc);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;
        unsigned miss_base = 20000000u + round * FC2_BENCH_QUERY;
        unsigned misses;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query_keys[i] = prefill_keys[idx];
            } else {
                query_keys[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        misses = fc2_flow4_cache_lookup_batch(&newc.fc, query_keys, FC2_BENCH_QUERY,
                                              now, results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query_keys, miss_idx,
                                              misses, now, results);
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            (void)fc2_flow4_cache_maintain(&newc.fc, maint_cursor, 16u, now);
            maint_cursor += 16u;
            if (maint_cursor >= nb_bk)
                maint_cursor -= nb_bk;
        }
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("\nfc2 persistent packet path + 1 kick/packet 16x1\n");
    printf("pps=%u  hit_target=%u%%  timeout=1000ms  nb_bk=%u  pool=%u\n",
           pps, hit_pct, nb_bk, max_entries);
    printf("fc2 : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses,
           delta.relief_evictions,
           delta.maint_calls,
           delta.maint_evictions,
           100.0 * (double)start_entries / (double)total_slots,
           100.0 * (double)active / (double)total_slots,
           100.0 * (double)max_seen / (double)total_slots);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

static void
bench_rate_limited_compare_maint16x1_interval(unsigned desired_entries,
                                              unsigned nb_bk,
                                              unsigned start_fill_pct,
                                              unsigned hit_pct,
                                              unsigned pps,
                                              unsigned maintain_fill_pct,
                                              unsigned maintain_interval)
{
    struct old_cache_ctx old_auto;
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct flow4_key *old_prefill_keys;
    struct fc2_flow4_key *new_prefill_keys;
    struct flow4_key *old_query;
    struct fc2_flow4_key *new_query;
    struct flow4_entry **old_results;
    struct fc2_flow4_result *new_results;
    uint16_t *miss_idx;
    struct old_stats_delta old_before;
    struct old_stats_delta old_after;
    struct old_stats_delta old_delta;
    struct new_stats_delta new_before;
    struct new_stats_delta new_after;
    struct new_stats_delta new_delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned old_start_active;
    unsigned new_start_active;
    unsigned old_active;
    unsigned new_active;
    unsigned old_max_seen;
    unsigned new_max_seen;
    unsigned old_expire_credit = 0u;
    unsigned old_expire_misses = 0u;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t old_total_cycles = 0u;
    uint64_t new_total_cycles = 0u;
    unsigned hit_credit = 0u;
    uint64_t maint_calls = 0u;
    uint64_t maint_evictions = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    rounds += rounds / 2u;

    cfg.timeout_tsc = tsc_hz;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;

    old_cache_setup_timeout(&old_auto, nb_bk, max_entries, FLOW_CACHE_BACKEND_AUTO, 1000u);
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    old_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*old_prefill_keys));
    new_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*new_prefill_keys));
    old_query = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_query));
    new_query = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_query));
    old_results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_results));
    new_results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++) {
        old_prefill_keys[i] = bench_make_key(i);
        new_prefill_keys[i] = bench_make_key2(i);
    }

    now = 1u;
    (void)old_prefill(&old_auto, old_prefill_keys, start_entries, now);
    (void)new_prefill(&newc, new_prefill_keys, start_entries, now);
    old_start_active = bench_old_active_scan(&old_auto);
    new_start_active = bench_new_active_scan(&newc);
    old_active = old_start_active;
    new_active = new_start_active;
    old_max_seen = old_start_active;
    new_max_seen = new_start_active;
    old_before = bench_old_stats_snapshot(&old_auto);
    new_before = bench_new_stats_snapshot(&newc);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;
        unsigned miss_base = 40000000u + round * FC2_BENCH_QUERY;
        unsigned misses = 0u;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                old_query[i] = old_prefill_keys[idx];
                new_query[i] = new_prefill_keys[idx];
            } else {
                old_query[i] = bench_make_key(miss_base + i);
                new_query[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        (void)flow4_cache_lookup_touch_batch(&old_auto.fc, old_query, FC2_BENCH_QUERY,
                                             now, old_results);
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (old_results[i] != NULL)
                continue;
            if (flow4_cache_insert(&old_auto.fc, &old_query[i], now) != NULL)
                misses++;
        }
        old_expire_credit += FC2_BENCH_QUERY;
        old_expire_misses += misses;
        if (old_expire_credit >= FC2_BENCH_OLD_EXPIRE_PERIOD) {
            flow4_cache_adjust_timeout(&old_auto.fc, old_expire_misses);
            flow4_cache_expire(&old_auto.fc, now);
            old_expire_credit = 0u;
            old_expire_misses = 0u;
        }
        t1 = flow_cache_rdtsc();
        old_total_cycles += t1 - t0;
        old_active = bench_old_active_scan(&old_auto);
        if (old_active > old_max_seen)
            old_max_seen = old_active;

        t0 = flow_cache_rdtsc();
        misses = fc2_flow4_cache_lookup_batch(&newc.fc, new_query, FC2_BENCH_QUERY,
                                              now, new_results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, new_query, miss_idx,
                                              misses, now, new_results);
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            unsigned fill_pct;
            struct fc2_flow4_entry *entry;
            unsigned start_bk;
            unsigned evicted;

            fill_pct = (newc.fc.total_slots == 0u) ? 0u :
                (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
            if (fill_pct <= maintain_fill_pct)
                continue;
            if (new_results[i].entry_idx == 0u)
                continue;
            hit_credit++;
            if (hit_credit < maintain_interval)
                continue;
            hit_credit = 0u;

            entry = RIX_PTR_FROM_IDX(newc.pool, new_results[i].entry_idx);
            if (entry == NULL)
                continue;
            start_bk = entry->cur_hash & newc.fc.ht_head.rhh_mask;
            evicted = bench_maintain_grouped(&newc, start_bk, 16u, 16u, 1u, now);
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        new_total_cycles += t1 - t0;
        new_active = bench_new_active_scan(&newc);
        if (new_active > new_max_seen)
            new_max_seen = new_active;
    }

    if (old_expire_credit != 0u) {
        uint64_t t0 = flow_cache_rdtsc();
        flow4_cache_adjust_timeout(&old_auto.fc, old_expire_misses);
        flow4_cache_expire(&old_auto.fc, now);
        uint64_t t1 = flow_cache_rdtsc();
        old_total_cycles += t1 - t0;
    }

    old_after = bench_old_stats_snapshot(&old_auto);
    new_after = bench_new_stats_snapshot(&newc);
    old_delta = bench_old_stats_diff(old_after, old_before);
    new_delta = bench_new_stats_diff(new_after, new_before);

    printf("\nfc2 persistent packet path + fill>%u%% / every-%u-hit 16x1\n",
           maintain_fill_pct, maintain_interval);
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=1000ms  nb_bk=%u  pool=%u\n",
           pps, hit_pct, start_fill_pct, nb_bk, max_entries);
    printf("old : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  evict=%" PRIu64 "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)old_total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           old_delta.lookups ? (100.0 * (double)old_delta.hits / (double)old_delta.lookups) : 0.0,
           old_delta.misses,
           old_delta.evictions,
           100.0 * (double)old_start_active / (double)total_slots,
           100.0 * (double)old_active / (double)total_slots,
           100.0 * (double)old_max_seen / (double)total_slots);
    printf("fc2 : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)new_total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           new_delta.lookups ? (100.0 * (double)new_delta.hits / (double)new_delta.lookups) : 0.0,
           new_delta.misses,
           new_delta.relief_evictions,
           maint_calls,
           maint_evictions,
           100.0 * (double)new_start_active / (double)total_slots,
           100.0 * (double)new_active / (double)total_slots,
           100.0 * (double)new_max_seen / (double)total_slots);

    free(miss_idx);
    free(new_results);
    free(old_results);
    free(new_query);
    free(old_query);
    free(new_prefill_keys);
    free(old_prefill_keys);
    free(newc.pool);
    free(newc.buckets);
    free(old_auto.pool);
    free(old_auto.buckets);
}

static void
bench_rate_limited_compare_hyst_wide(unsigned desired_entries,
                                     unsigned nb_bk,
                                     unsigned start_fill_pct,
                                     unsigned hit_pct,
                                     unsigned pps)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned active;
    unsigned max_seen;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t total_cycles = 0u;
    unsigned drain_mode = 0u;
    unsigned hit_credit = 0u;
    uint64_t maint_calls = 0u;
    uint64_t maint_evictions = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    rounds += rounds / 2u;

    new_cache_setup(&newc, nb_bk, max_entries);
    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query_keys));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    active = bench_new_active_scan(&newc);
    max_seen = active;
    before = bench_new_stats_snapshot(&newc);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;
        unsigned miss_base = 30000000u + round * FC2_BENCH_QUERY;
        unsigned misses;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query_keys[i] = prefill_keys[idx];
            } else {
                query_keys[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        misses = fc2_flow4_cache_lookup_batch(&newc.fc, query_keys, FC2_BENCH_QUERY,
                                              now, results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query_keys, miss_idx,
                                              misses, now, results);
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            unsigned fill_pct;
            unsigned width;
            unsigned interval;
            struct fc2_flow4_entry *entry;
            unsigned start_bk;
            unsigned evicted;

            fill_pct = (newc.fc.total_slots == 0u) ? 0u :
                (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
            if (!drain_mode) {
                if (fill_pct > 90u)
                    drain_mode = 1u;
            } else if (fill_pct <= 85u) {
                drain_mode = 0u;
                hit_credit = 0u;
            }
            if (!drain_mode)
                continue;
            if (results[i].entry_idx == 0u)
                continue;

            if (fill_pct >= 96u) {
                width = 16u;
                interval = 8u;
            } else if (fill_pct >= 94u) {
                width = 14u;
                interval = 12u;
            } else if (fill_pct >= 92u) {
                width = 12u;
                interval = 16u;
            } else if (fill_pct >= 91u) {
                width = 10u;
                interval = 24u;
            } else {
                width = 8u;
                interval = 32u;
            }

            hit_credit++;
            if (hit_credit < interval)
                continue;
            hit_credit = 0u;

            entry = RIX_PTR_FROM_IDX(newc.pool, results[i].entry_idx);
            if (entry == NULL)
                continue;
            start_bk = entry->cur_hash & newc.fc.ht_head.rhh_mask;
            evicted = bench_maintain_grouped(&newc, start_bk, width, width, 1u, now);
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("\nfc2 persistent packet path + hyst wide hit-bk x1\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=1000ms  nb_bk=%u  pool=%u\n",
           pps, hit_pct, start_fill_pct, nb_bk, max_entries);
    printf("fc2 : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses,
           delta.relief_evictions,
           maint_calls,
           maint_evictions,
           100.0 * (double)start_entries / (double)total_slots,
           100.0 * (double)active / (double)total_slots,
           100.0 * (double)max_seen / (double)total_slots);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

static void
bench_rate_limited_compare_hotseed16x1_gov(unsigned desired_entries,
                                           unsigned nb_bk,
                                           unsigned start_fill_pct,
                                           unsigned hit_pct,
                                           unsigned pps,
                                           unsigned timeout_ms,
                                           const struct hotseed_profile *profile)
{
    struct old_cache_ctx old_auto;
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct flow4_key *old_prefill_keys;
    struct fc2_flow4_key *new_prefill_keys;
    struct flow4_key *old_query;
    struct fc2_flow4_key *new_query;
    struct flow4_entry **old_results;
    struct fc2_flow4_result *new_results;
    uint16_t *miss_idx;
    struct old_stats_delta old_before;
    struct old_stats_delta old_after;
    struct old_stats_delta old_delta;
    struct new_stats_delta new_before;
    struct new_stats_delta new_after;
    struct new_stats_delta new_delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned old_start_active;
    unsigned new_start_active;
    unsigned old_active;
    unsigned new_active;
    unsigned old_max_seen;
    unsigned new_max_seen;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t old_total_cycles = 0u;
    uint64_t new_total_cycles = 0u;
    uint64_t maint_calls = 0u;
    uint64_t maint_evictions = 0u;
    unsigned insert_credit = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    rounds += rounds / 2u;

    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;

    old_cache_setup_timeout(&old_auto, nb_bk, max_entries, FLOW_CACHE_BACKEND_AUTO, timeout_ms);
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    old_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*old_prefill_keys));
    new_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*new_prefill_keys));
    old_query = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_query));
    new_query = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_query));
    old_results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_results));
    new_results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++) {
        old_prefill_keys[i] = bench_make_key(i);
        new_prefill_keys[i] = bench_make_key2(i);
    }

    now = 1u;
    (void)old_prefill(&old_auto, old_prefill_keys, start_entries, now);
    (void)new_prefill(&newc, new_prefill_keys, start_entries, now);
    old_start_active = bench_old_active_scan(&old_auto);
    new_start_active = bench_new_active_scan(&newc);
    old_active = old_start_active;
    new_active = new_start_active;
    old_max_seen = old_start_active;
    new_max_seen = new_start_active;
    old_before = bench_old_stats_snapshot(&old_auto);
    new_before = bench_new_stats_snapshot(&newc);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;
        unsigned miss_base = 60000000u + round * FC2_BENCH_QUERY;
        unsigned misses = 0u;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                old_query[i] = old_prefill_keys[idx];
                new_query[i] = new_prefill_keys[idx];
            } else {
                old_query[i] = bench_make_key(miss_base + i);
                new_query[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        (void)flow4_cache_lookup_touch_batch(&old_auto.fc, old_query, FC2_BENCH_QUERY,
                                             now, old_results);
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (old_results[i] != NULL)
                continue;
            if (flow4_cache_insert(&old_auto.fc, &old_query[i], now) != NULL)
                misses++;
        }
        flow4_cache_adjust_timeout(&old_auto.fc, misses);
        flow4_cache_expire(&old_auto.fc, now);
        t1 = flow_cache_rdtsc();
        old_total_cycles += t1 - t0;
        old_active = bench_old_active_scan(&old_auto);
        if (old_active > old_max_seen)
            old_max_seen = old_active;

        t0 = flow_cache_rdtsc();
        misses = fc2_flow4_cache_lookup_batch(&newc.fc, new_query, FC2_BENCH_QUERY,
                                              now, new_results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, new_query, miss_idx,
                                              misses, now, new_results);
        for (unsigned i = 0; i < misses; i++) {
            unsigned key_idx = miss_idx[i];
            unsigned fill_pct;
            unsigned interval;
            struct fc2_flow4_entry *entry;
            unsigned start_bk;
            unsigned evicted;

            if (new_results[key_idx].entry_idx == 0u)
                continue;
            fill_pct = (newc.fc.total_slots == 0u) ? 0u :
                (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
            if (fill_pct < profile->start_fill_pct)
                continue;
            if (fill_pct >= 90u)
                interval = profile->interval_ge_90;
            else if (fill_pct >= 85u)
                interval = profile->interval_ge_85;
            else if (fill_pct >= 80u)
                interval = profile->interval_ge_80;
            else
                interval = profile->interval_ge_start;

            insert_credit++;
            if (insert_credit < interval)
                continue;
            insert_credit = 0u;

            entry = RIX_PTR_FROM_IDX(newc.pool, new_results[key_idx].entry_idx);
            if (entry == NULL)
                continue;
            start_bk = entry->cur_hash & newc.fc.ht_head.rhh_mask;
            evicted = bench_maintain_grouped(&newc, start_bk, 16u, 16u, 1u, now);
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        new_total_cycles += t1 - t0;
        new_active = bench_new_active_scan(&newc);
        if (new_active > new_max_seen)
            new_max_seen = new_active;
    }

    old_after = bench_old_stats_snapshot(&old_auto);
    new_after = bench_new_stats_snapshot(&newc);
    old_delta = bench_old_stats_diff(old_after, old_before);
    new_delta = bench_new_stats_diff(new_after, new_before);

    printf("\nfc2 persistent packet path + hot-insert-bk 16x1 governor (%s)\n",
           profile->name);
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=%ums  nb_bk=%u  pool=%u"
           "  interval[>=90/85/80/%u]=%u/%u/%u/%u\n",
           pps, hit_pct, start_fill_pct, timeout_ms, nb_bk, max_entries,
           profile->start_fill_pct,
           profile->interval_ge_90, profile->interval_ge_85,
           profile->interval_ge_80, profile->interval_ge_start);
    printf("old : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  evict=%" PRIu64 "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)old_total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           old_delta.lookups ? (100.0 * (double)old_delta.hits / (double)old_delta.lookups) : 0.0,
           old_delta.misses,
           old_delta.evictions,
           100.0 * (double)old_start_active / (double)total_slots,
           100.0 * (double)old_active / (double)total_slots,
           100.0 * (double)old_max_seen / (double)total_slots);
    printf("fc2 : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)new_total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           new_delta.lookups ? (100.0 * (double)new_delta.hits / (double)new_delta.lookups) : 0.0,
           new_delta.misses,
           new_delta.relief_evictions,
           maint_calls,
           maint_evictions,
           100.0 * (double)new_start_active / (double)total_slots,
           100.0 * (double)new_active / (double)total_slots,
           100.0 * (double)new_max_seen / (double)total_slots);

    free(miss_idx);
    free(new_results);
    free(old_results);
    free(new_query);
    free(old_query);
    free(new_prefill_keys);
    free(old_prefill_keys);
    free(newc.pool);
    free(newc.buckets);
    free(old_auto.pool);
    free(old_auto.buckets);
}

static void
bench_rate_limited_compare_idle_maint(unsigned desired_entries,
                                      unsigned nb_bk,
                                      unsigned start_fill_pct,
                                      unsigned hit_pct,
                                      unsigned pps,
                                      unsigned high_water_pct,
                                      unsigned low_water_pct)
{
    struct old_cache_ctx old_auto;
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct flow4_key *old_prefill_keys;
    struct fc2_flow4_key *new_prefill_keys;
    struct flow4_key *old_query;
    struct fc2_flow4_key *new_query;
    struct flow4_entry **old_results;
    struct fc2_flow4_result *new_results;
    uint16_t *miss_idx;
    struct old_stats_delta old_before;
    struct old_stats_delta old_after;
    struct old_stats_delta old_delta;
    struct new_stats_delta new_before;
    struct new_stats_delta new_after;
    struct new_stats_delta new_delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned old_start_active;
    unsigned new_start_active;
    unsigned old_active;
    unsigned new_active;
    unsigned old_max_seen;
    unsigned new_max_seen;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t batch_budget;
    uint64_t now;
    uint64_t old_total_cycles = 0u;
    uint64_t new_total_cycles = 0u;
    uint64_t idle_maint_cycles = 0u;
    uint64_t idle_maint_calls = 0u;
    uint64_t idle_maint_evict = 0u;
    unsigned maint_cursor = 0u;
    int drain_mode = 0;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    batch_budget = tsc_per_pkt * FC2_BENCH_QUERY;
    rounds = (unsigned)(((uint64_t)pps + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    rounds += rounds / 2u;

    cfg.timeout_tsc = tsc_hz;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;

    old_cache_setup_timeout(&old_auto, nb_bk, max_entries, FLOW_CACHE_BACKEND_AUTO, 1000u);
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    old_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*old_prefill_keys));
    new_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*new_prefill_keys));
    old_query = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_query));
    new_query = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_query));
    old_results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_results));
    new_results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++) {
        old_prefill_keys[i] = bench_make_key(i);
        new_prefill_keys[i] = bench_make_key2(i);
    }

    now = 1u;
    (void)old_prefill(&old_auto, old_prefill_keys, start_entries, now);
    (void)new_prefill(&newc, new_prefill_keys, start_entries, now);
    old_start_active = bench_old_active_scan(&old_auto);
    new_start_active = bench_new_active_scan(&newc);
    old_active = old_start_active;
    new_active = new_start_active;
    old_max_seen = old_start_active;
    new_max_seen = new_start_active;
    old_before = bench_old_stats_snapshot(&old_auto);
    new_before = bench_new_stats_snapshot(&newc);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;
        unsigned miss_base = 50000000u + round * FC2_BENCH_QUERY;
        unsigned misses = 0u;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                old_query[i] = old_prefill_keys[idx];
                new_query[i] = new_prefill_keys[idx];
            } else {
                old_query[i] = bench_make_key(miss_base + i);
                new_query[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        (void)flow4_cache_lookup_touch_batch(&old_auto.fc, old_query, FC2_BENCH_QUERY,
                                             now, old_results);
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (old_results[i] != NULL)
                continue;
            if (flow4_cache_insert(&old_auto.fc, &old_query[i], now) != NULL)
                misses++;
        }
        flow4_cache_adjust_timeout(&old_auto.fc, misses);
        flow4_cache_expire(&old_auto.fc, now);
        t1 = flow_cache_rdtsc();
        old_total_cycles += t1 - t0;
        old_active = bench_old_active_scan(&old_auto);
        if (old_active > old_max_seen)
            old_max_seen = old_active;

        t0 = flow_cache_rdtsc();
        misses = fc2_flow4_cache_lookup_batch(&newc.fc, new_query, FC2_BENCH_QUERY,
                                              now, new_results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, new_query, miss_idx,
                                              misses, now, new_results);
        t1 = flow_cache_rdtsc();
        new_total_cycles += t1 - t0;

        new_active = bench_new_active_scan(&newc);
        if (new_active > new_max_seen)
            new_max_seen = new_active;

        if (!drain_mode) {
            if ((newc.fc.total_slots != 0u) &&
                ((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots) > high_water_pct)
                drain_mode = 1;
        } else if ((newc.fc.total_slots == 0u) ||
                   (((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots) <= low_water_pct)) {
            drain_mode = 0;
        }

        if (drain_mode && (t1 - t0) < batch_budget) {
            uint64_t slack = batch_budget - (t1 - t0);

            while (slack > 0u) {
                unsigned fill_pct;
                unsigned evicted;
                uint64_t mt0, mt1, used;

                fill_pct = (newc.fc.total_slots == 0u) ? 0u :
                    (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
                if (fill_pct <= low_water_pct)
                    break;

                mt0 = flow_cache_rdtsc();
                evicted = fc2_flow4_cache_maintain(&newc.fc, maint_cursor, 16u, now);
                mt1 = flow_cache_rdtsc();
                used = mt1 - mt0;
                if (used > slack)
                    break;
                slack -= used;
                idle_maint_cycles += used;
                idle_maint_calls++;
                idle_maint_evict += evicted;
                maint_cursor += 16u;
                if (maint_cursor >= nb_bk)
                    maint_cursor -= nb_bk;
            }
        }

        new_active = bench_new_active_scan(&newc);
        if (new_active > new_max_seen)
            new_max_seen = new_active;
    }

    old_after = bench_old_stats_snapshot(&old_auto);
    new_after = bench_new_stats_snapshot(&newc);
    old_delta = bench_old_stats_diff(old_after, old_before);
    new_delta = bench_new_stats_diff(new_after, new_before);

    printf("\nfc2 persistent packet path + idle 16x1 maintenance\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  hw/lw=%u/%u%%  timeout=1000ms  nb_bk=%u  pool=%u\n",
           pps, hit_pct, start_fill_pct, high_water_pct, low_water_pct, nb_bk, max_entries);
    printf("old : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  evict=%" PRIu64 "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)old_total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           old_delta.lookups ? (100.0 * (double)old_delta.hits / (double)old_delta.lookups) : 0.0,
           old_delta.misses,
           old_delta.evictions,
           100.0 * (double)old_start_active / (double)total_slots,
           100.0 * (double)old_active / (double)total_slots,
           100.0 * (double)old_max_seen / (double)total_slots);
    printf("fc2 : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  idle_calls=%" PRIu64 "  idle_evict=%" PRIu64
           "  idle_cycles=%" PRIu64 "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)(new_total_cycles + idle_maint_cycles) / (double)(rounds * FC2_BENCH_QUERY),
           new_delta.lookups ? (100.0 * (double)new_delta.hits / (double)new_delta.lookups) : 0.0,
           new_delta.misses,
           new_delta.relief_evictions,
           idle_maint_calls,
           idle_maint_evict,
           idle_maint_cycles,
           100.0 * (double)new_start_active / (double)total_slots,
           100.0 * (double)new_active / (double)total_slots,
           100.0 * (double)new_max_seen / (double)total_slots);

    free(miss_idx);
    free(new_results);
    free(old_results);
    free(new_query);
    free(old_query);
    free(new_prefill_keys);
    free(old_prefill_keys);
    free(newc.pool);
    free(newc.buckets);
    free(old_auto.pool);
    free(old_auto.buckets);
}

static void
bench_rate_limited_fc2_batch_maint(unsigned desired_entries,
                                   unsigned nb_bk,
                                   unsigned start_fill_pct,
                                   unsigned hit_pct,
                                   unsigned pps,
                                   unsigned timeout_ms,
                                   unsigned kick_scale)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned active;
    unsigned max_seen;
    unsigned maint_cursor = 0u;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t total_cycles = 0u;
    uint64_t maint_calls = 0u;
    uint64_t maint_evictions = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps * ((uint64_t)timeout_ms / 1000u)
                         + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (rounds < 1u)
        rounds = 1u;

    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query_keys));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    active = bench_new_active_scan(&newc);
    max_seen = active;
    before = bench_new_stats_snapshot(&newc);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;
        unsigned miss_base = 70000000u + round * FC2_BENCH_QUERY;
        unsigned misses;
        unsigned fill_pct;
        unsigned kicks = 0u;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query_keys[i] = prefill_keys[idx];
            } else {
                query_keys[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        misses = fc2_flow4_cache_lookup_batch(&newc.fc, query_keys, FC2_BENCH_QUERY,
                                              now, results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query_keys, miss_idx,
                                              misses, now, results);

        if (newc.fc.total_slots != 0u)
            fill_pct = (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
        else
            fill_pct = 0u;

        if (fill_pct >= 75u)
            kicks = 8u;
        else if (fill_pct >= 70u)
            kicks = 4u;
        else if (fill_pct >= 65u)
            kicks = 2u;
        else if (fill_pct >= 60u)
            kicks = 1u;

        kicks *= kick_scale;

        for (unsigned k = 0; k < kicks; k++) {
            unsigned evicted = fc2_flow4_cache_maintain(&newc.fc, maint_cursor, 16u, now);
            maint_cursor += 16u;
            if (maint_cursor >= nb_bk)
                maint_cursor -= nb_bk;
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("\nfc2 persistent packet path + batch-maint governor\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=%ums  nb_bk=%u  pool=%u"
           "  rounds=%u  kick_scale=%u  kicks[60/65/70/75]=1/2/4/8\n",
           pps, hit_pct, start_fill_pct, timeout_ms, nb_bk, max_entries, rounds,
           kick_scale);
    printf("fc2 : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses,
           delta.relief_evictions,
           maint_calls,
           maint_evictions,
           100.0 * (double)start_entries / (double)total_slots,
           100.0 * (double)active / (double)total_slots,
           100.0 * (double)max_seen / (double)total_slots);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

static unsigned
bench_fc2_batch_maint_kicks(unsigned fill_pct, unsigned kick_scale)
{
    unsigned kicks = 0u;

    if (fill_pct >= 75u)
        kicks = 8u;
    else if (fill_pct >= 70u)
        kicks = 4u;
    else if (fill_pct >= 65u)
        kicks = 2u;
    else if (fill_pct >= 60u)
        kicks = 1u;
    return kicks * kick_scale;
}

static unsigned
bench_fc2_batch_maint_kicks_70737577(unsigned fill_pct, unsigned kick_scale)
{
    unsigned kicks = 0u;

    if (fill_pct >= 77u)
        kicks = 8u;
    else if (fill_pct >= 75u)
        kicks = 4u;
    else if (fill_pct >= 73u)
        kicks = 2u;
    else if (fill_pct >= 70u)
        kicks = 1u;
    return kicks * kick_scale;
}

static unsigned
bench_fc2_batch_maint_kicks_70737577_0123(unsigned fill_pct, unsigned kick_scale)
{
    unsigned kicks = 0u;

    if (fill_pct >= 77u)
        kicks = 3u;
    else if (fill_pct >= 75u)
        kicks = 2u;
    else if (fill_pct >= 73u)
        kicks = 1u;
    return kicks * kick_scale;
}

static unsigned
bench_fc2_batch_maint_kicks_70737577_0112(unsigned fill_pct, unsigned kick_scale)
{
    unsigned kicks = 0u;

    if (fill_pct >= 77u)
        kicks = 2u;
    else if (fill_pct >= 75u)
        kicks = 1u;
    else if (fill_pct >= 73u)
        kicks = 1u;
    return kicks * kick_scale;
}

static unsigned
bench_fc2_batch_maint_kicks_70737577_0012(unsigned fill_pct, unsigned kick_scale)
{
    unsigned kicks = 0u;

    if (fill_pct >= 77u)
        kicks = 2u;
    else if (fill_pct >= 75u)
        kicks = 1u;
    return kicks * kick_scale;
}

static unsigned
bench_fc2_batch_maint_kicks_custom(unsigned fill_pct,
                                   unsigned fill0,
                                   unsigned fill1,
                                   unsigned fill2,
                                   unsigned fill3,
                                   unsigned kicks0,
                                   unsigned kicks1,
                                   unsigned kicks2,
                                   unsigned kicks3,
                                   unsigned kick_scale)
{
    unsigned kicks = 0u;

    if (fill_pct >= fill3)
        kicks = kicks3;
    else if (fill_pct >= fill2)
        kicks = kicks2;
    else if (fill_pct >= fill1)
        kicks = kicks1;
    else if (fill_pct >= fill0)
        kicks = kicks0;
    return kicks * kick_scale;
}

static unsigned
bench_rate_nb_bk(unsigned desired_entries)
{
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned nb_bk = max_entries / RIX_HASH_BUCKET_ENTRY_SZ;

    return (nb_bk < 2u) ? 2u : nb_bk;
}

static void
bench_rate_limited_fc2_batch_maint_trace(unsigned desired_entries,
                                         unsigned nb_bk,
                                         unsigned start_fill_pct,
                                         unsigned hit_pct,
                                         unsigned pps,
                                         unsigned timeout_ms,
                                         unsigned kick_scale,
                                         unsigned soak_mul,
                                         unsigned report_ms)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned report_rounds;
    unsigned active;
    unsigned max_seen;
    unsigned maint_cursor = 0u;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t total_cycles = 0u;
    uint64_t maint_calls = 0u;
    uint64_t maint_evictions = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps * ((uint64_t)timeout_ms / 1000u)
                         + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (rounds < 1u)
        rounds = 1u;
    rounds *= soak_mul;
    report_rounds = (unsigned)(((uint64_t)pps * ((uint64_t)report_ms / 1000u)
                                + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (report_rounds == 0u)
        report_rounds = 1u;

    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query_keys));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    active = bench_new_active_scan(&newc);
    max_seen = active;
    before = bench_new_stats_snapshot(&newc);

    printf("\nfc2 batch-maint trace\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=%ums  soak=%ux  nb_bk=%u  pool=%u"
           "  kicks[60/65/70/75]=1/2/4/8 * %u\n",
           pps, hit_pct, start_fill_pct, timeout_ms, soak_mul, nb_bk, max_entries,
           kick_scale);
    printf(" time_s  fill%%  max%%  maint_calls  maint_evict  relief_evict\n");

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;
        unsigned miss_base = 80000000u + round * FC2_BENCH_QUERY;
        unsigned fill_pct;
        unsigned kicks;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query_keys[i] = prefill_keys[idx];
            } else {
                query_keys[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        {
            unsigned misses = fc2_flow4_cache_lookup_batch(&newc.fc, query_keys,
                                                           FC2_BENCH_QUERY, now,
                                                           results, miss_idx);
            (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query_keys, miss_idx,
                                                  misses, now, results);
        }

        if (newc.fc.total_slots != 0u)
            fill_pct = (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
        else
            fill_pct = 0u;
        kicks = bench_fc2_batch_maint_kicks(fill_pct, kick_scale);

        for (unsigned k = 0; k < kicks; k++) {
            unsigned evicted = fc2_flow4_cache_maintain(&newc.fc, maint_cursor, 16u, now);
            maint_cursor += 16u;
            if (maint_cursor >= nb_bk)
                maint_cursor -= nb_bk;
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
        if (((round + 1u) % report_rounds) == 0u || round + 1u == rounds) {
            struct new_stats_delta mid =
                bench_new_stats_diff(bench_new_stats_snapshot(&newc), before);
            double sim_sec = ((double)(round + 1u) * (double)FC2_BENCH_QUERY) / (double)pps;

            printf("%7.2f  %5.1f  %4.1f  %11" PRIu64 "  %11" PRIu64 "  %12" PRIu64 "\n",
                   sim_sec,
                   100.0 * (double)active / (double)total_slots,
                   100.0 * (double)max_seen / (double)total_slots,
                   maint_calls,
                   maint_evictions,
                   mid.relief_evictions);
        }
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("summary: cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses,
           delta.relief_evictions,
           maint_calls,
           maint_evictions,
           100.0 * (double)active / (double)total_slots,
           100.0 * (double)max_seen / (double)total_slots);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

static void
bench_rate_limited_fc2_batch_maint_trace_70737577(unsigned desired_entries,
                                                  unsigned nb_bk,
                                                  unsigned start_fill_pct,
                                                  unsigned hit_pct,
                                                  unsigned pps,
                                                  unsigned timeout_ms,
                                                  unsigned kick_scale,
                                                  unsigned soak_mul,
                                                  unsigned report_ms)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned report_rounds;
    unsigned hit_n;
    unsigned maint_cursor = 0u;
    unsigned active;
    unsigned max_seen;
    unsigned maint_calls = 0u;
    unsigned maint_evictions = 0u;
    uint64_t now;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t total_cycles = 0u;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps * ((uint64_t)timeout_ms / 1000u)
                         + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (rounds < 1u)
        rounds = 1u;
    rounds *= soak_mul;
    report_rounds = (unsigned)(((uint64_t)pps * ((uint64_t)report_ms / 1000u)
                                + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (report_rounds == 0u)
        report_rounds = 1u;

    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query_keys));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    active = bench_new_active_scan(&newc);
    max_seen = active;
    before = bench_new_stats_snapshot(&newc);
    hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;

    printf("\nfc2 batch-maint trace\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=%ums  soak=%ux  nb_bk=%u  pool=%u"
           "  kicks[70/73/75/77]=1/2/4/8 * %u\n",
           pps, hit_pct, start_fill_pct, timeout_ms, soak_mul, nb_bk, max_entries,
           kick_scale);
    printf(" time_s  fill%%  max%%  maint_calls  maint_evict  relief_evict\n");

    for (unsigned round = 0; round < rounds; round++) {
        unsigned miss_base = 80000000u + round * FC2_BENCH_QUERY;
        unsigned fill_pct;
        unsigned kicks;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query_keys[i] = prefill_keys[idx];
            } else {
                query_keys[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        {
            unsigned misses = fc2_flow4_cache_lookup_batch(&newc.fc, query_keys,
                                                           FC2_BENCH_QUERY, now,
                                                           results, miss_idx);
            (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query_keys, miss_idx,
                                                  misses, now, results);
        }

        if (newc.fc.total_slots != 0u)
            fill_pct = (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
        else
            fill_pct = 0u;
        kicks = bench_fc2_batch_maint_kicks_70737577(fill_pct, kick_scale);

        for (unsigned k = 0; k < kicks; k++) {
            unsigned evicted = fc2_flow4_cache_maintain(&newc.fc, maint_cursor, 16u, now);
            maint_cursor += 16u;
            if (maint_cursor >= nb_bk)
                maint_cursor -= nb_bk;
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
        if (((round + 1u) % report_rounds) == 0u || round + 1u == rounds) {
            struct new_stats_delta mid =
                bench_new_stats_diff(bench_new_stats_snapshot(&newc), before);
            double sim_sec = ((double)(round + 1u) * (double)FC2_BENCH_QUERY) / (double)pps;

            printf("%7.2f  %5.1f  %4.1f  %11u  %11u  %12" PRIu64 "\n",
                   sim_sec,
                   total_slots != 0u ? ((double)active * 100.0) / (double)total_slots : 0.0,
                   total_slots != 0u ? ((double)max_seen * 100.0) / (double)total_slots : 0.0,
                   maint_calls, maint_evictions, mid.relief_evictions);
        }
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("summary: cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%u  maint_evict=%u"
           "  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / ((double)rounds * (double)FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses, delta.relief_evictions,
           maint_calls, maint_evictions,
           total_slots != 0u ? ((double)active * 100.0) / (double)total_slots : 0.0,
           total_slots != 0u ? ((double)max_seen * 100.0) / (double)total_slots : 0.0);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

static void
bench_rate_limited_fc2_batch_maint_trace_70737577_0123(unsigned desired_entries,
                                                       unsigned nb_bk,
                                                       unsigned start_fill_pct,
                                                       unsigned hit_pct,
                                                       unsigned pps,
                                                       unsigned timeout_ms,
                                                       unsigned kick_scale,
                                                       unsigned soak_mul,
                                                       unsigned report_ms)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned report_rounds;
    unsigned hit_n;
    unsigned maint_cursor = 0u;
    unsigned active;
    unsigned max_seen;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t total_cycles = 0u;
    uint64_t maint_calls = 0u;
    uint64_t maint_evictions = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps * ((uint64_t)timeout_ms / 1000u)
                         + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (rounds < 1u)
        rounds = 1u;
    rounds *= soak_mul;
    report_rounds = (unsigned)(((uint64_t)pps * ((uint64_t)report_ms / 1000u)
                                + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (report_rounds == 0u)
        report_rounds = 1u;

    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query_keys));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    active = bench_new_active_scan(&newc);
    max_seen = active;
    before = bench_new_stats_snapshot(&newc);
    hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;

    printf("\nfc2 batch-maint trace\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=%ums  soak=%ux  nb_bk=%u  pool=%u"
           "  kicks[70/73/75/77]=0/1/2/3 * %u\n",
           pps, hit_pct, start_fill_pct, timeout_ms, soak_mul, nb_bk, max_entries,
           kick_scale);
    printf(" time_s  fill%%  max%%  maint_calls  maint_evict  relief_evict\n");

    for (unsigned round = 0; round < rounds; round++) {
        unsigned miss_base = 80000000u + round * FC2_BENCH_QUERY;
        unsigned fill_pct;
        unsigned kicks;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query_keys[i] = prefill_keys[idx];
            } else {
                query_keys[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        {
            unsigned misses = fc2_flow4_cache_lookup_batch(&newc.fc, query_keys,
                                                           FC2_BENCH_QUERY, now,
                                                           results, miss_idx);
            (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query_keys, miss_idx,
                                                  misses, now, results);
        }

        if (newc.fc.total_slots != 0u)
            fill_pct = (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
        else
            fill_pct = 0u;
        kicks = bench_fc2_batch_maint_kicks_70737577_0123(fill_pct, kick_scale);

        for (unsigned k = 0; k < kicks; k++) {
            unsigned evicted = fc2_flow4_cache_maintain(&newc.fc, maint_cursor, 16u, now);
            maint_cursor += 16u;
            if (maint_cursor >= nb_bk)
                maint_cursor -= nb_bk;
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
        if (((round + 1u) % report_rounds) == 0u || round + 1u == rounds) {
            struct new_stats_delta mid =
                bench_new_stats_diff(bench_new_stats_snapshot(&newc), before);
            double sim_sec = ((double)(round + 1u) * (double)FC2_BENCH_QUERY) / (double)pps;

            printf("%7.2f  %5.1f  %4.1f  %11" PRIu64 "  %11" PRIu64 "  %12" PRIu64 "\n",
                   sim_sec,
                   100.0 * (double)active / (double)total_slots,
                   100.0 * (double)max_seen / (double)total_slots,
                   maint_calls,
                   maint_evictions,
                   mid.relief_evictions);
        }
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("summary: cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses,
           delta.relief_evictions,
           maint_calls,
           maint_evictions,
           100.0 * (double)active / (double)total_slots,
           100.0 * (double)max_seen / (double)total_slots);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

static void
bench_rate_limited_fc2_batch_maint_trace_70737577_0112(unsigned desired_entries,
                                                       unsigned nb_bk,
                                                       unsigned start_fill_pct,
                                                       unsigned hit_pct,
                                                       unsigned pps,
                                                       unsigned timeout_ms,
                                                       unsigned kick_scale,
                                                       unsigned soak_mul,
                                                       unsigned report_ms)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned report_rounds;
    unsigned hit_n;
    unsigned maint_cursor = 0u;
    unsigned active;
    unsigned max_seen;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t total_cycles = 0u;
    uint64_t maint_calls = 0u;
    uint64_t maint_evictions = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps * ((uint64_t)timeout_ms / 1000u)
                         + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (rounds < 1u)
        rounds = 1u;
    rounds *= soak_mul;
    report_rounds = (unsigned)(((uint64_t)pps * ((uint64_t)report_ms / 1000u)
                                + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (report_rounds == 0u)
        report_rounds = 1u;

    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query_keys));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    active = bench_new_active_scan(&newc);
    max_seen = active;
    before = bench_new_stats_snapshot(&newc);
    hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;

    printf("\nfc2 batch-maint trace\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=%ums  soak=%ux  nb_bk=%u  pool=%u"
           "  kicks[70/73/75/77]=0/1/1/2 * %u\n",
           pps, hit_pct, start_fill_pct, timeout_ms, soak_mul, nb_bk, max_entries,
           kick_scale);
    printf(" time_s  fill%%  max%%  maint_calls  maint_evict  relief_evict\n");

    for (unsigned round = 0; round < rounds; round++) {
        unsigned miss_base = 80000000u + round * FC2_BENCH_QUERY;
        unsigned fill_pct;
        unsigned kicks;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query_keys[i] = prefill_keys[idx];
            } else {
                query_keys[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        {
            unsigned misses = fc2_flow4_cache_lookup_batch(&newc.fc, query_keys,
                                                           FC2_BENCH_QUERY, now,
                                                           results, miss_idx);
            (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query_keys, miss_idx,
                                                  misses, now, results);
        }

        if (newc.fc.total_slots != 0u)
            fill_pct = (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
        else
            fill_pct = 0u;
        kicks = bench_fc2_batch_maint_kicks_70737577_0112(fill_pct, kick_scale);

        for (unsigned k = 0; k < kicks; k++) {
            unsigned evicted = fc2_flow4_cache_maintain(&newc.fc, maint_cursor, 16u, now);
            maint_cursor += 16u;
            if (maint_cursor >= nb_bk)
                maint_cursor -= nb_bk;
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
        if (((round + 1u) % report_rounds) == 0u || round + 1u == rounds) {
            struct new_stats_delta mid =
                bench_new_stats_diff(bench_new_stats_snapshot(&newc), before);
            double sim_sec = ((double)(round + 1u) * (double)FC2_BENCH_QUERY) / (double)pps;

            printf("%7.2f  %5.1f  %4.1f  %11" PRIu64 "  %11" PRIu64 "  %12" PRIu64 "\n",
                   sim_sec,
                   100.0 * (double)active / (double)total_slots,
                   100.0 * (double)max_seen / (double)total_slots,
                   maint_calls,
                   maint_evictions,
                   mid.relief_evictions);
        }
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("summary: cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses,
           delta.relief_evictions,
           maint_calls,
           maint_evictions,
           100.0 * (double)active / (double)total_slots,
           100.0 * (double)max_seen / (double)total_slots);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

static void
bench_rate_limited_fc2_batch_maint_trace_70737577_0012(unsigned desired_entries,
                                                       unsigned nb_bk,
                                                       unsigned start_fill_pct,
                                                       unsigned hit_pct,
                                                       unsigned pps,
                                                       unsigned timeout_ms,
                                                       unsigned kick_scale,
                                                       unsigned soak_mul,
                                                       unsigned report_ms)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned report_rounds;
    unsigned hit_n;
    unsigned maint_cursor = 0u;
    unsigned active;
    unsigned max_seen;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t total_cycles = 0u;
    uint64_t maint_calls = 0u;
    uint64_t maint_evictions = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps * ((uint64_t)timeout_ms / 1000u)
                         + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (rounds < 1u)
        rounds = 1u;
    rounds *= soak_mul;
    report_rounds = (unsigned)(((uint64_t)pps * ((uint64_t)report_ms / 1000u)
                                + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (report_rounds == 0u)
        report_rounds = 1u;

    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query_keys));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    active = bench_new_active_scan(&newc);
    max_seen = active;
    before = bench_new_stats_snapshot(&newc);
    hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;

    printf("\nfc2 batch-maint trace\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=%ums  soak=%ux  nb_bk=%u  pool=%u"
           "  kicks[70/73/75/77]=0/0/1/2 * %u\n",
           pps, hit_pct, start_fill_pct, timeout_ms, soak_mul, nb_bk, max_entries,
           kick_scale);
    printf(" time_s  fill%%  max%%  maint_calls  maint_evict  relief_evict\n");

    for (unsigned round = 0; round < rounds; round++) {
        unsigned miss_base = 80000000u + round * FC2_BENCH_QUERY;
        unsigned fill_pct;
        unsigned kicks;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query_keys[i] = prefill_keys[idx];
            } else {
                query_keys[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        {
            unsigned misses = fc2_flow4_cache_lookup_batch(&newc.fc, query_keys,
                                                           FC2_BENCH_QUERY, now,
                                                           results, miss_idx);
            (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query_keys, miss_idx,
                                                  misses, now, results);
        }

        if (newc.fc.total_slots != 0u)
            fill_pct = (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
        else
            fill_pct = 0u;
        kicks = bench_fc2_batch_maint_kicks_70737577_0012(fill_pct, kick_scale);

        for (unsigned k = 0; k < kicks; k++) {
            unsigned evicted = fc2_flow4_cache_maintain(&newc.fc, maint_cursor, 16u, now);
            maint_cursor += 16u;
            if (maint_cursor >= nb_bk)
                maint_cursor -= nb_bk;
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
        if (((round + 1u) % report_rounds) == 0u || round + 1u == rounds) {
            struct new_stats_delta mid =
                bench_new_stats_diff(bench_new_stats_snapshot(&newc), before);
            double sim_sec = ((double)(round + 1u) * (double)FC2_BENCH_QUERY) / (double)pps;

            printf("%7.2f  %5.1f  %4.1f  %11" PRIu64 "  %11" PRIu64 "  %12" PRIu64 "\n",
                   sim_sec,
                   100.0 * (double)active / (double)total_slots,
                   100.0 * (double)max_seen / (double)total_slots,
                   maint_calls,
                   maint_evictions,
                   mid.relief_evictions);
        }
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("summary: cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses,
           delta.relief_evictions,
           maint_calls,
           maint_evictions,
           100.0 * (double)active / (double)total_slots,
           100.0 * (double)max_seen / (double)total_slots);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

static void
bench_rate_limited_fc2_batch_maint_trace_custom(unsigned desired_entries,
                                                unsigned nb_bk,
                                                unsigned start_fill_pct,
                                                unsigned hit_pct,
                                                unsigned pps,
                                                unsigned timeout_ms,
                                                unsigned kick_scale,
                                                unsigned soak_mul,
                                                unsigned report_ms,
                                                unsigned fill0,
                                                unsigned fill1,
                                                unsigned fill2,
                                                unsigned fill3,
                                                unsigned kicks0,
                                                unsigned kicks1,
                                                unsigned kicks2,
                                                unsigned kicks3)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query_keys;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned report_rounds;
    unsigned hit_n;
    unsigned maint_cursor = 0u;
    unsigned active;
    unsigned max_seen;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t total_cycles = 0u;
    uint64_t maint_calls = 0u;
    uint64_t maint_evictions = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps * ((uint64_t)timeout_ms / 1000u)
                         + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (rounds < 1u)
        rounds = 1u;
    rounds *= soak_mul;
    report_rounds = (unsigned)(((uint64_t)pps * ((uint64_t)report_ms / 1000u)
                                + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    if (report_rounds == 0u)
        report_rounds = 1u;

    cfg.timeout_tsc = (tsc_hz / 1000u) * timeout_ms;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query_keys = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query_keys));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    active = bench_new_active_scan(&newc);
    max_seen = active;
    before = bench_new_stats_snapshot(&newc);
    hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;

    printf("\nfc2 batch-maint trace\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=%ums  soak=%ux  nb_bk=%u  pool=%u"
           "  kicks[%u/%u/%u/%u]=%u/%u/%u/%u * %u\n",
           pps, hit_pct, start_fill_pct, timeout_ms, soak_mul, nb_bk, max_entries,
           fill0, fill1, fill2, fill3,
           kicks0, kicks1, kicks2, kicks3,
           kick_scale);
    printf(" time_s  fill%%  max%%  maint_calls  maint_evict  relief_evict\n");

    for (unsigned round = 0; round < rounds; round++) {
        unsigned miss_base = 80000000u + round * FC2_BENCH_QUERY;
        unsigned fill_pct;
        unsigned kicks;
        uint64_t t0, t1;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query_keys[i] = prefill_keys[idx];
            } else {
                query_keys[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        {
            unsigned misses = fc2_flow4_cache_lookup_batch(&newc.fc, query_keys,
                                                           FC2_BENCH_QUERY, now,
                                                           results, miss_idx);
            (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query_keys, miss_idx,
                                                  misses, now, results);
        }

        if (newc.fc.total_slots != 0u)
            fill_pct = (unsigned)((newc.fc.ht_head.rhh_nb * 100u) / newc.fc.total_slots);
        else
            fill_pct = 0u;
        kicks = bench_fc2_batch_maint_kicks_custom(fill_pct,
                                                   fill0, fill1, fill2, fill3,
                                                   kicks0, kicks1, kicks2, kicks3,
                                                   kick_scale);

        for (unsigned k = 0; k < kicks; k++) {
            unsigned evicted = fc2_flow4_cache_maintain(&newc.fc, maint_cursor, 16u, now);
            maint_cursor += 16u;
            if (maint_cursor >= nb_bk)
                maint_cursor -= nb_bk;
            maint_calls++;
            maint_evictions += evicted;
        }
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
        if (((round + 1u) % report_rounds) == 0u || round + 1u == rounds) {
            struct new_stats_delta mid =
                bench_new_stats_diff(bench_new_stats_snapshot(&newc), before);
            double sim_sec = ((double)(round + 1u) * (double)FC2_BENCH_QUERY) / (double)pps;

            printf("%7.2f  %5.1f  %4.1f  %11" PRIu64 "  %11" PRIu64 "  %12" PRIu64 "\n",
                   sim_sec,
                   100.0 * (double)active / (double)total_slots,
                   100.0 * (double)max_seen / (double)total_slots,
                   maint_calls,
                   maint_evictions,
                   mid.relief_evictions);
        }
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("summary: cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses,
           delta.relief_evictions,
           maint_calls,
           maint_evictions,
           100.0 * (double)active / (double)total_slots,
           100.0 * (double)max_seen / (double)total_slots);

    free(miss_idx);
    free(results);
    free(query_keys);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

static void
bench_rate_limited_fc2_only(unsigned desired_entries,
                            unsigned nb_bk,
                            unsigned start_fill_pct,
                            unsigned hit_pct,
                            unsigned pps)
{
    struct new_cache_ctx newc;
    struct fc2_flow4_config cfg;
    struct fc2_flow4_key *prefill_keys;
    struct fc2_flow4_key *query;
    struct fc2_flow4_result *results;
    uint16_t *miss_idx;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;
    unsigned max_entries = flow_cache_pool_count(desired_entries);
    unsigned total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;
    unsigned start_entries = (unsigned)(((uint64_t)total_slots * start_fill_pct) / 100u);
    unsigned rounds;
    unsigned start_active;
    unsigned active;
    unsigned max_seen;
    uint64_t tsc_hz;
    uint64_t tsc_per_pkt;
    uint64_t now;
    uint64_t total_cycles = 0u;

    if (start_entries > max_entries)
        start_entries = max_entries;

    tsc_hz = flow_cache_calibrate_tsc_hz();
    if (tsc_hz == 0u)
        tsc_hz = UINT64_C(2500000000);
    tsc_per_pkt = tsc_hz / pps;
    if (tsc_per_pkt == 0u)
        tsc_per_pkt = 1u;
    rounds = (unsigned)(((uint64_t)pps + FC2_BENCH_QUERY - 1u) / FC2_BENCH_QUERY);
    rounds += rounds / 2u;

    cfg.timeout_tsc = tsc_hz;
    cfg.pressure_empty_slots = FC2_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS;
    new_cache_setup_cfg(&newc, nb_bk, max_entries, &cfg);

    prefill_keys = bench_alloc((size_t)max_entries * sizeof(*prefill_keys));
    query = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*query));
    results = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*results));
    miss_idx = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*miss_idx));

    for (unsigned i = 0; i < max_entries; i++)
        prefill_keys[i] = bench_make_key2(i);

    now = 1u;
    (void)new_prefill(&newc, prefill_keys, start_entries, now);
    start_active = bench_new_active_scan(&newc);
    active = start_active;
    max_seen = start_active;
    before = bench_new_stats_snapshot(&newc);

    for (unsigned round = 0; round < rounds; round++) {
        unsigned hit_n = (FC2_BENCH_QUERY * hit_pct) / 100u;
        unsigned miss_base = 50000000u + round * FC2_BENCH_QUERY;
        uint64_t t0, t1;
        unsigned miss_count;

        now += tsc_per_pkt * FC2_BENCH_QUERY;
        for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
            if (i < hit_n && start_entries > 0u) {
                unsigned idx = (round * 17u + i * 13u) % start_entries;
                query[i] = prefill_keys[idx];
            } else {
                query[i] = bench_make_key2(miss_base + i);
            }
        }

        t0 = flow_cache_rdtsc();
        miss_count = fc2_flow4_cache_lookup_batch(&newc.fc, query, FC2_BENCH_QUERY,
                                                  now, results, miss_idx);
        (void)fc2_flow4_cache_fill_miss_batch(&newc.fc, query, miss_idx,
                                              miss_count, now, results);
        t1 = flow_cache_rdtsc();
        total_cycles += t1 - t0;

        active = bench_new_active_scan(&newc);
        if (active > max_seen)
            max_seen = active;
    }

    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);

    printf("\nfc2-only persistent packet path\n");
    printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=1000ms  nb_bk=%u  pool=%u\n",
           pps, hit_pct, start_fill_pct, nb_bk, max_entries);
    printf("fc2 : cycles/pkt=%8.2f  hit=%.1f%%  misses=%" PRIu64
           "  relief=%" PRIu64 "  maint_calls=%" PRIu64 "  maint_evict=%" PRIu64
           "  start_fill=%.1f%%  end_fill=%.1f%%  max_fill=%.1f%%\n",
           (double)total_cycles / (double)(rounds * FC2_BENCH_QUERY),
           delta.lookups ? (100.0 * (double)delta.hits / (double)delta.lookups) : 0.0,
           delta.misses,
           delta.relief_evictions,
           delta.maint_calls,
           delta.maint_evictions,
           100.0 * (double)start_active / (double)total_slots,
           100.0 * (double)active / (double)total_slots,
           100.0 * (double)max_seen / (double)total_slots);

    free(miss_idx);
    free(results);
    free(query);
    free(prefill_keys);
    free(newc.pool);
    free(newc.buckets);
}

int
main(int argc, char **argv)
{
    unsigned max_entries = flow_cache_pool_count(FC2_BENCH_DESIRED_ENTRIES);
    unsigned prefill_n = (max_entries * 3u) / 4u;
    unsigned nb_bk = flow_cache_nb_bk_hint(max_entries);
    struct old_cache_ctx old_auto;
    struct old_cache_ctx old_gen;
    struct new_cache_ctx newc;
    struct flow4_key *old_prefill_keys;
    struct fc2_flow4_key *new_prefill_keys;
    struct flow4_key *old_query_hit;
    struct fc2_flow4_key *new_query_hit;
    struct flow4_key *old_query_miss;
    struct fc2_flow4_key *new_query_miss;
    struct flow4_key *old_query_mixed;
    struct fc2_flow4_key *new_query_mixed;
    struct new_stats_delta before;
    struct new_stats_delta after;
    struct new_stats_delta delta;

    if (argc > 1) {
        if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
            printf("usage:\n");
            printf("  %s rate_compare <desired_entries> <start_fill_pct> <hit_pct> <pps>\n", argv[0]);
            printf("  %s rate_compare_timeout <desired_entries> <start_fill_pct> <hit_pct> <pps> <timeout_ms>\n", argv[0]);
            printf("  %s rate_fc2_only <desired_entries> <start_fill_pct> <hit_pct> <pps>\n", argv[0]);
            printf("  %s rate_trace_custom <desired_entries> <start_fill_pct> <hit_pct> <pps> <timeout_ms> <soak_mul> <report_ms> <fill0> <fill1> <fill2> <fill3> <k0> <k1> <k2> <k3> [kick_scale]\n",
                   argv[0]);
            printf("  %s maint <bucket_count>\n", argv[0]);
            return 0;
        }
        if (strcmp(argv[1], "maint") == 0 && argc >= 3) {
            unsigned bucket_count = (unsigned)strtoul(argv[2], NULL, 10);

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_only(bucket_count);
            return 0;
        }
        if (strcmp(argv[1], "rate_compare") == 0) {
            unsigned desired_entries;
            unsigned rate_nb_bk;
            unsigned start_fill_pct;
            unsigned hit_pct;
            unsigned pps;

            if (argc < 6) {
                fprintf(stderr, "usage: %s %s <desired_entries> <start_fill_pct> <hit_pct> <pps>\n",
                        argv[0], argv[1]);
                return 2;
            }
            desired_entries = (unsigned)strtoul(argv[2], NULL, 10);
            start_fill_pct = (unsigned)strtoul(argv[3], NULL, 10);
            hit_pct = (unsigned)strtoul(argv[4], NULL, 10);
            pps = (unsigned)strtoul(argv[5], NULL, 10);
            rate_nb_bk = bench_rate_nb_bk(desired_entries);
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare(desired_entries, rate_nb_bk,
                                       start_fill_pct, hit_pct, pps);
            return 0;
        }
        if (strcmp(argv[1], "rate_compare_timeout") == 0) {
            unsigned desired_entries;
            unsigned rate_nb_bk;
            unsigned start_fill_pct;
            unsigned hit_pct;
            unsigned pps;
            unsigned timeout_ms;

            if (argc < 7) {
                fprintf(stderr, "usage: %s %s <desired_entries> <start_fill_pct> <hit_pct> <pps> <timeout_ms>\n",
                        argv[0], argv[1]);
                return 2;
            }
            desired_entries = (unsigned)strtoul(argv[2], NULL, 10);
            start_fill_pct = (unsigned)strtoul(argv[3], NULL, 10);
            hit_pct = (unsigned)strtoul(argv[4], NULL, 10);
            pps = (unsigned)strtoul(argv[5], NULL, 10);
            timeout_ms = (unsigned)strtoul(argv[6], NULL, 10);
            rate_nb_bk = bench_rate_nb_bk(desired_entries);
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare_timeout(desired_entries, rate_nb_bk,
                                               start_fill_pct, hit_pct, pps,
                                               timeout_ms);
            return 0;
        }
        if (strcmp(argv[1], "rate_fc2_only") == 0) {
            unsigned desired_entries;
            unsigned rate_nb_bk;
            unsigned start_fill_pct;
            unsigned hit_pct;
            unsigned pps;

            if (argc < 6) {
                fprintf(stderr, "usage: %s %s <desired_entries> <start_fill_pct> <hit_pct> <pps>\n",
                        argv[0], argv[1]);
                return 2;
            }
            desired_entries = (unsigned)strtoul(argv[2], NULL, 10);
            start_fill_pct = (unsigned)strtoul(argv[3], NULL, 10);
            hit_pct = (unsigned)strtoul(argv[4], NULL, 10);
            pps = (unsigned)strtoul(argv[5], NULL, 10);
            rate_nb_bk = bench_rate_nb_bk(desired_entries);
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_only(desired_entries, rate_nb_bk,
                                        start_fill_pct, hit_pct, pps);
            return 0;
        }
        if (strcmp(argv[1], "rate_trace_custom") == 0) {
            unsigned desired_entries;
            unsigned rate_nb_bk;
            unsigned start_fill_pct;
            unsigned hit_pct;
            unsigned pps;
            unsigned timeout_ms;
            unsigned soak_mul;
            unsigned report_ms;
            unsigned fill0;
            unsigned fill1;
            unsigned fill2;
            unsigned fill3;
            unsigned kicks0;
            unsigned kicks1;
            unsigned kicks2;
            unsigned kicks3;
            unsigned kick_scale = 1u;

            if (argc < 17) {
                fprintf(stderr,
                        "usage: %s %s <desired_entries> <start_fill_pct> <hit_pct> <pps> <timeout_ms> <soak_mul> <report_ms> <fill0> <fill1> <fill2> <fill3> <k0> <k1> <k2> <k3> [kick_scale]\n",
                        argv[0], argv[1]);
                return 2;
            }
            desired_entries = (unsigned)strtoul(argv[2], NULL, 10);
            start_fill_pct = (unsigned)strtoul(argv[3], NULL, 10);
            hit_pct = (unsigned)strtoul(argv[4], NULL, 10);
            pps = (unsigned)strtoul(argv[5], NULL, 10);
            timeout_ms = (unsigned)strtoul(argv[6], NULL, 10);
            soak_mul = (unsigned)strtoul(argv[7], NULL, 10);
            report_ms = (unsigned)strtoul(argv[8], NULL, 10);
            fill0 = (unsigned)strtoul(argv[9], NULL, 10);
            fill1 = (unsigned)strtoul(argv[10], NULL, 10);
            fill2 = (unsigned)strtoul(argv[11], NULL, 10);
            fill3 = (unsigned)strtoul(argv[12], NULL, 10);
            kicks0 = (unsigned)strtoul(argv[13], NULL, 10);
            kicks1 = (unsigned)strtoul(argv[14], NULL, 10);
            kicks2 = (unsigned)strtoul(argv[15], NULL, 10);
            kicks3 = (unsigned)strtoul(argv[16], NULL, 10);
            if (argc >= 18)
                kick_scale = (unsigned)strtoul(argv[17], NULL, 10);
            rate_nb_bk = bench_rate_nb_bk(desired_entries);
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_custom(desired_entries,
                                                            rate_nb_bk,
                                                            start_fill_pct,
                                                            hit_pct,
                                                            pps,
                                                            timeout_ms,
                                                            kick_scale,
                                                            soak_mul,
                                                            report_ms,
                                                            fill0, fill1, fill2, fill3,
                                                            kicks0, kicks1, kicks2, kicks3);
            return 0;
        }
        if (strcmp(argv[1], "maint") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_only(1u);
            return 0;
        }
        if (strcmp(argv[1], "maint8") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_only(8u);
            return 0;
        }
        if (strcmp(argv[1], "maint16") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_only(16u);
            return 0;
        }
        if (strcmp(argv[1], "maint8_pause") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_only_paused(8u);
            return 0;
        }
        if (strcmp(argv[1], "maint16_pause") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_only_paused(16u);
            return 0;
        }
        if (strcmp(argv[1], "maint42") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_grouped_only(4u, 2u);
            return 0;
        }
        if (strcmp(argv[1], "maint18") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_grouped_only(1u, 8u);
            return 0;
        }
        if (strcmp(argv[1], "maint24") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_grouped_only(2u, 4u);
            return 0;
        }
        if (strcmp(argv[1], "maint81") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_grouped_only(8u, 1u);
            return 0;
        }
        if (strcmp(argv[1], "maint101") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_grouped_only(10u, 1u);
            return 0;
        }
        if (strcmp(argv[1], "maint121") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_grouped_only(12u, 1u);
            return 0;
        }
        if (strcmp(argv[1], "maint141") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_grouped_only(14u, 1u);
            return 0;
        }
        if (strcmp(argv[1], "maint161") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_grouped_only(16u, 1u);
            return 0;
        }
        if (strcmp(argv[1], "maint82") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_grouped_only(8u, 2u);
            return 0;
        }
        if (strcmp(argv[1], "maintcmp16") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_compare_v1_v2_16();
            return 0;
        }
        if (strcmp(argv[1], "maintv1_16_pause") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_maintain_v1_16_paused();
            return 0;
        }
        if (strcmp(argv[1], "rate500k_maint16x1") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare_maint16x1(1000000u, 65536u, 90u, 500000u);
            bench_rate_limited_compare_maint16x1(1000000u, 65536u, 80u, 500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_hyst16x1") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare_hyst_wide(1000000u, 65536u, 85u, 90u, 500000u);
            bench_rate_limited_compare_hyst_wide(1000000u, 65536u, 85u, 80u, 500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_hyst16x1_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare_hyst_wide(1000000u, 65536u, 95u, 90u, 500000u);
            bench_rate_limited_compare_hyst_wide(1000000u, 65536u, 95u, 80u, 500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_75_256_16x1") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare_maint16x1_interval(1000000u, 65536u, 85u, 90u,
                                                          500000u, 75u, 256u);
            bench_rate_limited_compare_maint16x1_interval(1000000u, 65536u, 85u, 80u,
                                                          500000u, 75u, 256u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_80_85_90_256_16x1") == 0) {
            static const unsigned fill_list[] = { 80u, 85u, 90u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                bench_rate_limited_compare_maint16x1_interval(1000000u, 65536u, 85u,
                                                              90u, 500000u,
                                                              fill_list[f], 256u);
                bench_rate_limited_compare_maint16x1_interval(1000000u, 65536u, 85u,
                                                              80u, 500000u,
                                                              fill_list[f], 256u);
            }
            return 0;
        }
        if (strcmp(argv[1], "rate500k_idle16x1") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare_idle_maint(1000000u, 65536u, 85u, 90u,
                                                  500000u, 90u, 85u);
            bench_rate_limited_compare_idle_maint(1000000u, 65536u, 85u, 80u,
                                                  500000u, 90u, 85u);
            bench_rate_limited_compare_idle_maint(1000000u, 65536u, 85u, 90u,
                                                  500000u, 89u, 85u);
            bench_rate_limited_compare_idle_maint(1000000u, 65536u, 85u, 80u,
                                                  500000u, 89u, 85u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_fill90_hit95_100") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare(1000000u, 65536u, 90u, 100u, 500000u);
            bench_rate_limited_compare(1000000u, 65536u, 90u, 95u, 500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_fill75_hit100") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare(1000000u, 65536u, 75u, 100u, 500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_fill60_hit100") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_compare(1000000u, 65536u, 60u, 100u, 500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_fill60_hit100_fc2only") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_only(1000000u, 65536u, 60u, 100u, 500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_fill50_hit100_fc2only") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_only(1000000u, 65536u, 50u, 100u, 500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_fill90_hit100_fc2only") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_only(1000000u, 65536u, 90u, 100u, 500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_filllink_1m2m") == 0) {
            static const unsigned entries_list[] = { 1000000u, 2000000u };
            static const unsigned fill_list[] = { 75u, 85u };
            static const unsigned hit_list[] = { 95u, 90u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned e = 0; e < sizeof(entries_list) / sizeof(entries_list[0]); e++) {
                unsigned sweep_nb_bk = (entries_list[e] == 1000000u) ? 65536u : 131072u;

                for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                    for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                        bench_rate_limited_compare(entries_list[e], sweep_nb_bk,
                                                   fill_list[f], hit_list[h],
                                                   500000u);
                    }
                }
            }
            return 0;
        }
        if (strcmp(argv[1], "rate500k_filllink_to10") == 0) {
            static const unsigned fill_list[] = { 75u, 85u };
            static const unsigned hit_list[] = { 95u, 90u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                    bench_rate_limited_compare_timeout(1000000u, 65536u,
                                                       fill_list[f], hit_list[h],
                                                       500000u, 10000u);
                }
            }
            return 0;
        }
        if (strcmp(argv[1], "rate500k_filllink_linear8") == 0) {
            static const unsigned fill_list[] = { 65u, 70u, 75u, 85u };
            static const unsigned hit_list[] = { 95u, 90u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                    bench_rate_limited_compare_timeout(1000000u, 65536u,
                                                       fill_list[f], hit_list[h],
                                                       500000u, 8000u);
                }
            }
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8") == 0) {
            static const unsigned fill_list[] = { 75u, 85u };
            static const unsigned hit_list[] = { 95u, 90u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                    bench_rate_limited_fc2_batch_maint(1000000u, 65536u,
                                                       fill_list[f], hit_list[h],
                                                       500000u, 8000u, 1u);
                }
            }
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak") == 0) {
            static const unsigned fill_list[] = { 75u, 85u };
            static const unsigned hit_list[] = { 95u, 90u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                    bench_rate_limited_fc2_batch_maint(1000000u, 65536u,
                                                       fill_list[f], hit_list[h],
                                                       500000u, 8000u, 1u);
                }
            }
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_75_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 75u, 95u,
                                               500000u, 8000u, 1u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_75_90") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 75u, 90u,
                                               500000u, 8000u, 1u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_85_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 85u, 95u,
                                               500000u, 8000u, 1u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_85_90") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 85u, 90u,
                                               500000u, 8000u, 1u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_60_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace(1000000u, 65536u, 60u, 95u,
                                                     500000u, 8000u, 1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_60_90") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace(1000000u, 65536u, 60u, 90u,
                                                     500000u, 8000u, 1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_75_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace(1000000u, 65536u, 75u, 95u,
                                                     500000u, 8000u, 1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0012") == 0) {
            unsigned start_fill_pct;
            unsigned hit_pct;

            if (argc < 4) {
                fprintf(stderr, "usage: %s %s <start_fill_pct> <hit_pct>\n",
                        argv[0], argv[1]);
                return 2;
            }
            start_fill_pct = (unsigned)strtoul(argv[2], NULL, 10);
            hit_pct = (unsigned)strtoul(argv[3], NULL, 10);
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0012(1000000u, 65536u,
                                                                   start_fill_pct, hit_pct,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate_batchmaint_trace_70737577_0012") == 0) {
            unsigned desired_entries;
            unsigned trace_nb_bk;
            unsigned start_fill_pct;
            unsigned hit_pct;
            unsigned pps;

            if (argc < 6) {
                fprintf(stderr, "usage: %s %s <desired_entries> <start_fill_pct> <hit_pct> <pps>\n",
                        argv[0], argv[1]);
                return 2;
            }
            desired_entries = (unsigned)strtoul(argv[2], NULL, 10);
            start_fill_pct = (unsigned)strtoul(argv[3], NULL, 10);
            hit_pct = (unsigned)strtoul(argv[4], NULL, 10);
            pps = (unsigned)strtoul(argv[5], NULL, 10);
            trace_nb_bk = (desired_entries <= 1000000u) ? 65536u : 131072u;
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0012(desired_entries,
                                                                   trace_nb_bk,
                                                                   start_fill_pct,
                                                                   hit_pct,
                                                                   pps,
                                                                   8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_60_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577(1000000u, 65536u,
                                                              60u, 95u,
                                                              500000u, 8000u,
                                                              1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_60_90") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577(1000000u, 65536u,
                                                              60u, 90u,
                                                              500000u, 8000u,
                                                              1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_75_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577(1000000u, 65536u,
                                                              75u, 95u,
                                                              500000u, 8000u,
                                                              1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0123_60_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0123(1000000u, 65536u,
                                                                   60u, 95u,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0123_60_90") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0123(1000000u, 65536u,
                                                                   60u, 90u,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0123_75_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0123(1000000u, 65536u,
                                                                   75u, 95u,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0112_60_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0112(1000000u, 65536u,
                                                                   60u, 95u,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0112_60_90") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0112(1000000u, 65536u,
                                                                   60u, 90u,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0112_75_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0112(1000000u, 65536u,
                                                                   75u, 95u,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0012_60_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0012(1000000u, 65536u,
                                                                   60u, 95u,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0012_60_90") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0012(1000000u, 65536u,
                                                                   60u, 90u,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_trace_70737577_0012_75_95") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint_trace_70737577_0012(1000000u, 65536u,
                                                                   75u, 95u,
                                                                   500000u, 8000u,
                                                                   1u, 3u, 2000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_75_95_x2") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 75u, 95u,
                                               500000u, 8000u, 2u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_75_95_x4") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 75u, 95u,
                                               500000u, 8000u, 4u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_75_95_x8") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 75u, 95u,
                                               500000u, 8000u, 8u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_75_90_x2") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 75u, 90u,
                                               500000u, 8000u, 2u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_75_90_x4") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 75u, 90u,
                                               500000u, 8000u, 4u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_batchmaint_linear8_soak_75_90_x8") == 0) {
            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            bench_rate_limited_fc2_batch_maint(1000000u, 65536u, 75u, 90u,
                                               500000u, 8000u, 8u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_hotseed16x1_gov") == 0) {
            static const unsigned fill_list[] = { 65u, 70u, 75u, 85u };
            static const unsigned hit_list[] = { 95u, 90u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned p = 0; p < sizeof(hotseed_profiles) / sizeof(hotseed_profiles[0]); p++) {
                for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                    for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                        bench_rate_limited_compare_hotseed16x1_gov(1000000u, 65536u,
                                                                   fill_list[f], hit_list[h],
                                                                   500000u, 1000u,
                                                                   &hotseed_profiles[p]);
                    }
                }
            }
            return 0;
        }
        if (strcmp(argv[1], "rate500k_filllink_to10_hotseed") == 0) {
            static const unsigned fill_list[] = { 75u, 85u };
            static const unsigned hit_list[] = { 95u, 90u };
            static const struct hotseed_profile *profiles[] = {
                &hotseed_profiles[2], /* lite75 */
                &hotseed_profiles[3], /* pre70 */
                &hotseed_profiles[4]  /* pre65 */
            };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned p = 0; p < sizeof(profiles) / sizeof(profiles[0]); p++) {
                for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                    for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                        bench_rate_limited_compare_hotseed16x1_gov(1000000u, 65536u,
                                                                   fill_list[f], hit_list[h],
                                                                   500000u, 10000u,
                                                                   profiles[p]);
                    }
                }
            }
            return 0;
        }
        if (strcmp(argv[1], "rate500k_fill60_80_hit100") == 0) {
            static const unsigned fill_list[] = { 60u, 65u, 70u, 75u, 80u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned i = 0; i < sizeof(fill_list) / sizeof(fill_list[0]); i++)
                bench_rate_limited_compare(1000000u, 65536u, fill_list[i], 100u,
                                           500000u);
            return 0;
        }
        if (strcmp(argv[1], "rate500k_bk1_eval") == 0) {
            static const unsigned fill_list[] = { 50u, 60u, 75u, 85u, 90u };
            static const unsigned hit_list[] = { 100u, 95u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                for (unsigned i = 0; i < sizeof(fill_list) / sizeof(fill_list[0]); i++)
                    bench_rate_limited_fc2_only(1000000u, 65536u, fill_list[i],
                                                hit_list[h], 500000u);
            }
            return 0;
        }
        if (strcmp(argv[1], "sweep_hystwide") == 0) {
            static const unsigned entries_list[] = { 1000000u, 2000000u };
            static const unsigned fill_list[] = { 85u, 90u, 95u };
            static const unsigned hit_list[] = { 90u, 80u };
            static const unsigned pps_list[] = { 250000u, 500000u, 1000000u, 2000000u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned e = 0; e < sizeof(entries_list) / sizeof(entries_list[0]); e++) {
                unsigned sweep_nb_bk = (entries_list[e] == 1000000u) ? 65536u : 131072u;

                for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                    for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                        for (unsigned p = 0; p < sizeof(pps_list) / sizeof(pps_list[0]); p++) {
                            bench_rate_limited_compare_hyst_wide(entries_list[e], sweep_nb_bk,
                                                                 fill_list[f], hit_list[h],
                                                                 pps_list[p]);
                        }
                    }
                }
            }
            return 0;
        }
        if (strcmp(argv[1], "sweep_gov") == 0) {
            static const unsigned entries_list[] = { 1000000u, 2000000u };
            static const unsigned fill_list[] = { 85u, 90u, 95u };
            static const unsigned hit_list[] = { 90u, 80u };
            static const unsigned pps_list[] = { 250000u, 500000u, 1000000u, 2000000u };

            rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
            for (unsigned e = 0; e < sizeof(entries_list) / sizeof(entries_list[0]); e++) {
                unsigned sweep_nb_bk = (entries_list[e] == 1000000u) ? 65536u : 131072u;

                for (unsigned f = 0; f < sizeof(fill_list) / sizeof(fill_list[0]); f++) {
                    for (unsigned h = 0; h < sizeof(hit_list) / sizeof(hit_list[0]); h++) {
                        for (unsigned p = 0; p < sizeof(pps_list) / sizeof(pps_list[0]); p++) {
                            bench_rate_limited_compare(entries_list[e], sweep_nb_bk,
                                                       fill_list[f], hit_list[h],
                                                       pps_list[p]);
                        }
                    }
                }
            }
            return 0;
        }
    }

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    old_cache_setup(&old_auto, nb_bk, max_entries, FLOW_CACHE_BACKEND_AUTO);
    old_cache_setup(&old_gen, nb_bk, max_entries, FLOW_CACHE_BACKEND_GEN);
    new_cache_setup(&newc, nb_bk, max_entries);

    old_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*old_prefill_keys));
    new_prefill_keys = bench_alloc((size_t)max_entries * sizeof(*new_prefill_keys));
    old_query_hit = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_query_hit));
    new_query_hit = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_query_hit));
    old_query_miss = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_query_miss));
    new_query_miss = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_query_miss));
    old_query_mixed = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*old_query_mixed));
    new_query_mixed = bench_alloc((size_t)FC2_BENCH_QUERY * sizeof(*new_query_mixed));

    for (unsigned i = 0; i < max_entries; i++) {
        old_prefill_keys[i] = bench_make_key(i);
        new_prefill_keys[i] = bench_make_key2(i);
    }
    for (unsigned i = 0; i < FC2_BENCH_QUERY; i++) {
        old_query_hit[i] = old_prefill_keys[i % prefill_n];
        new_query_hit[i] = new_prefill_keys[i % prefill_n];
        old_query_miss[i] = bench_make_key(100000u + i);
        new_query_miss[i] = bench_make_key2(100000u + i);
        if ((i % 10u) == 9u) {
            old_query_mixed[i] = old_query_miss[i];
            new_query_mixed[i] = new_query_miss[i];
        } else {
            unsigned idx = (i * 13u) % prefill_n;
            old_query_mixed[i] = old_prefill_keys[idx];
            new_query_mixed[i] = new_prefill_keys[idx];
        }
    }

    {
        unsigned old_auto_entries = old_prefill(&old_auto, old_prefill_keys, prefill_n, 1u);
        unsigned old_gen_entries = old_prefill(&old_gen, old_prefill_keys, prefill_n, 1u);
        unsigned new_entries = new_prefill(&newc, new_prefill_keys, prefill_n, 1u);
        unsigned common_entries = old_auto_entries;

        if (old_gen_entries < common_entries)
            common_entries = old_gen_entries;
        if (new_entries < common_entries)
            common_entries = new_entries;
        if (common_entries == 0u) {
            fprintf(stderr, "bench prefill failed: no common active entries\n");
            return 1;
        }
        prefill_n = common_entries;
    }

    printf("flow4 compare (pure datapath, no expire): entries=%u nb_bk=%u query=%u\n",
           max_entries, nb_bk, FC2_BENCH_QUERY);
    bench_emit3("hit_lookup_touch",
                bench_old_hit(&old_auto, old_query_hit, FC2_BENCH_QUERY,
                              FC2_BENCH_HIT_REPEAT),
                bench_old_hit(&old_gen, old_query_hit, FC2_BENCH_QUERY,
                              FC2_BENCH_HIT_REPEAT),
                bench_new_hit(&newc, new_query_hit, FC2_BENCH_QUERY,
                              FC2_BENCH_HIT_REPEAT));
    bench_emit3("miss_lookup_fill",
                bench_old_miss_fill(&old_auto, old_query_miss, FC2_BENCH_QUERY,
                                    FC2_BENCH_MISS_REPEAT),
                bench_old_miss_fill(&old_gen, old_query_miss, FC2_BENCH_QUERY,
                                    FC2_BENCH_MISS_REPEAT),
                bench_new_miss_fill(&newc, new_query_miss, FC2_BENCH_QUERY,
                                    FC2_BENCH_MISS_REPEAT));
    bench_emit3("mixed_90_10",
                bench_old_mixed(&old_auto, old_prefill_keys, prefill_n,
                                old_query_mixed, FC2_BENCH_QUERY,
                                FC2_BENCH_MIXED_REPEAT),
                bench_old_mixed(&old_gen, old_prefill_keys, prefill_n,
                                old_query_mixed, FC2_BENCH_QUERY,
                                FC2_BENCH_MIXED_REPEAT),
                bench_new_mixed(&newc, new_prefill_keys,
                                prefill_n, new_query_mixed,
                                FC2_BENCH_QUERY, FC2_BENCH_MIXED_REPEAT));

    printf("\nflow4 compare (fair packet path, cycles/pkt)\n");
    before = bench_new_stats_snapshot(&newc);
    bench_emit2("pkt_hit_only",
                bench_old_pkt_hit(&old_auto, old_query_hit, FC2_BENCH_QUERY,
                                  FC2_BENCH_HIT_REPEAT),
                bench_new_pkt_hit(&newc, new_query_hit, FC2_BENCH_QUERY,
                                  FC2_BENCH_HIT_REPEAT));
    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);
    bench_emit_relief("  fc2-hit-stats", &delta);

    before = bench_new_stats_snapshot(&newc);
    bench_emit2("pkt_miss_only",
                bench_old_pkt_miss_fill(&old_auto, old_query_miss, FC2_BENCH_QUERY,
                                        FC2_BENCH_MISS_REPEAT),
                bench_new_pkt_miss_fill(&newc, new_query_miss,
                                        FC2_BENCH_QUERY, FC2_BENCH_MISS_REPEAT));
    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);
    bench_emit_relief("  fc2-miss-stats", &delta);

    before = bench_new_stats_snapshot(&newc);
    bench_emit2("pkt_mixed_90_10",
                bench_old_pkt_mixed(&old_auto, old_prefill_keys, prefill_n,
                                    old_query_mixed, FC2_BENCH_QUERY,
                                    FC2_BENCH_MIXED_REPEAT),
                bench_new_pkt_mixed(&newc, new_prefill_keys,
                                    prefill_n, new_query_mixed,
                                    FC2_BENCH_QUERY, FC2_BENCH_MIXED_REPEAT));
    after = bench_new_stats_snapshot(&newc);
    delta = bench_new_stats_diff(after, before);
    bench_emit_relief("  fc2-mixed-stats", &delta);

    bench_fill_sweep(old_prefill_keys, new_prefill_keys, max_entries);
    bench_relief_only();
    bench_fill_growth(85u, 2048u, max_entries, 128u, FC2_BENCH_QUERY);
    bench_rate_limited_compare(1000000u, 65536u, 85u, 90u, 500000u);
    bench_rate_limited_compare(1000000u, 65536u, 85u, 80u, 500000u);

    free(new_query_mixed);
    free(old_query_mixed);
    free(new_query_miss);
    free(old_query_miss);
    free(new_query_hit);
    free(old_query_hit);
    free(new_prefill_keys);
    free(old_prefill_keys);
    free(newc.pool);
    free(newc.buckets);
    free(old_gen.pool);
    free(old_gen.buckets);
    free(old_auto.pool);
    free(old_auto.buckets);
    return 0;
}
