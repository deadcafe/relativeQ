/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <time.h>

#include "flow4_cache.h"

/*===========================================================================
 * TSC helpers (lfence/rdtscp for precise measurement)
 *===========================================================================*/
static inline uint64_t
tsc_start(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t
tsc_end(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtscp\n\tlfence\n\t" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo;
}

/*===========================================================================
 * PRNG
 *===========================================================================*/
static uint64_t xr64 = 0xDEADBEEF1234ABCDULL;

static inline uint64_t
xorshift64(void)
{
    xr64 ^= xr64 >> 12;
    xr64 ^= xr64 << 25;
    xr64 ^= xr64 >> 27;
    return xr64 * 0x2545F4914F6CDD1DULL;
}

/*===========================================================================
 * Key generation
 *===========================================================================*/
static struct flow4_key
make_key(uint32_t src, uint32_t dst, uint16_t sp, uint16_t dp,
         uint8_t proto, uint32_t vrf)
{
    struct flow4_key k;
    memset(&k, 0, sizeof(k));
    k.src_ip   = src;
    k.dst_ip   = dst;
    k.src_port = sp;
    k.dst_port = dp;
    k.proto    = proto;
    k.vrfid    = vrf;
    return k;
}

static void
make_random_key(struct flow4_key *k)
{
    uint64_t r0 = xorshift64();
    uint64_t r1 = xorshift64();
    uint64_t r2 = xorshift64();
    memset(k, 0, sizeof(*k));
    k->src_ip   = (uint32_t)r0;
    k->dst_ip   = (uint32_t)(r0 >> 32);
    k->src_port = (uint16_t)r1;
    k->dst_port = (uint16_t)(r1 >> 16);
    k->proto    = (uint8_t)(r1 >> 32);
    k->vrfid    = (uint32_t)r2;
}

/*===========================================================================
 * next_pow2: round up to power-of-two
 *===========================================================================*/
static unsigned
next_pow2(unsigned v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

/*===========================================================================
 * Allocate aligned memory via mmap + hugepage hint
 *===========================================================================*/
static void *
alloc_aligned(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    madvise(p, size, MADV_HUGEPAGE);
    return p;
}

/*===========================================================================
 * Wall-clock helper
 *===========================================================================*/
static double
now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/*===========================================================================
 * Test: structure sizes and alignment
 *===========================================================================*/
static void
test_struct_sizes(void)
{
    printf("[T] struct sizes\n");
    printf("    flow4_key:   %zu bytes\n", sizeof(struct flow4_key));
    printf("    flow4_entry: %zu bytes (align %zu)\n",
           sizeof(struct flow4_entry),
           __alignof__(struct flow4_entry));
    printf("    flow4_cache: %zu bytes\n", sizeof(struct flow4_cache));

    assert(sizeof(struct flow4_key) == 20);
    assert(sizeof(struct flow4_entry) == 128);
    assert(__alignof__(struct flow4_entry) == 64);
    printf("    PASS\n");
}

/*===========================================================================
 * Test: init + insert + single find
 *===========================================================================*/
static void
test_init_insert_find(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                      struct flow4_entry *pool, unsigned pool_cap)
{
    printf("[T] init/insert/find\n");

    struct flow4_cache fc;
    uint64_t now = 1000000;

    flow4_cache_init(&fc, buckets, nb_bk, pool, pool_cap, 0);

    struct flow4_key k1 = make_key(0x0A000001, 0x0A000002,
                                    1234, 80, 6, 100);
    struct flow4_entry *e1 = flow4_cache_insert(&fc, &k1, now);
    assert(e1 != NULL);
    assert(e1->action == FLOW_ACTION_NONE);
    assert(e1->flags & FLOW4_FLAG_VALID);

    struct flow4_entry *results[1];
    flow4_cache_lookup_batch(&fc, &k1, 1, results);
    assert(results[0] == e1);

    struct flow4_key k2 = make_key(0x0A000003, 0x0A000004,
                                    5678, 443, 6, 200);
    flow4_cache_lookup_batch(&fc, &k2, 1, results);
    assert(results[0] == NULL);

    flow4_cache_update_action(e1, 42, 7);
    assert(e1->action == 42);
    assert(e1->qos_class == 7);

    flow4_cache_touch(e1, now + 100, 64);
    assert(e1->last_ts == now + 100);
    assert(e1->packets == 1);
    assert(e1->bytes == 64);

    printf("    PASS\n");
}

/*===========================================================================
 * Test: expire
 *===========================================================================*/
static void
test_expire(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
            struct flow4_entry *pool, unsigned pool_cap)
{
    printf("[T] expire\n");

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk, pool, pool_cap, 1 /* 1 ms */);

    struct flow4_key k1 = make_key(1, 2, 3, 4, 6, 0);
    uint64_t now = flow_cache_rdtsc();
    struct flow4_entry *e1 = flow4_cache_insert(&fc, &k1, now);
    assert(e1 != NULL);

    flow4_cache_expire(&fc, now, pool_cap);
    struct flow4_entry *results[1];
    flow4_cache_lookup_batch(&fc, &k1, 1, results);
    assert(results[0] == e1);

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&delay, NULL);
    uint64_t later = flow_cache_rdtsc();

    flow4_cache_expire(&fc, later, pool_cap);
    flow4_cache_lookup_batch(&fc, &k1, 1, results);
    assert(results[0] == NULL);

    struct flow_cache_stats st;
    flow4_cache_stats(&fc, &st);
    assert(st.inserts == 1);
    assert(st.evictions == 1);

    printf("    PASS\n");
}

/*===========================================================================
 * Test: batch lookup
 *===========================================================================*/
static void
test_batch_lookup(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                  struct flow4_entry *pool, unsigned pool_cap)
{
    printf("[T] batch lookup\n");

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk, pool, pool_cap, 0);

    unsigned n = (pool_cap < 128) ? pool_cap / 2 : 128;
    struct flow4_key *keys = malloc(n * sizeof(*keys));
    struct flow4_entry **entries = malloc(n * sizeof(*entries));
    assert(keys && entries);

    for (unsigned i = 0; i < n; i++) {
        keys[i] = make_key(i, i + 1000, (uint16_t)i, (uint16_t)(i + 100),
                           6, i / 10);
        entries[i] = flow4_cache_insert(&fc, &keys[i], 1000);
        assert(entries[i] != NULL);
    }

    struct flow4_entry **results = malloc(n * sizeof(*results));
    assert(results);
    flow4_cache_lookup_batch(&fc, keys, n, results);
    for (unsigned i = 0; i < n; i++)
        assert(results[i] == entries[i]);

    free(keys);
    free(entries);
    free(results);
    printf("    PASS\n");
}

/*===========================================================================
 * Test: insert on free-list exhaustion (forced evict)
 *===========================================================================*/
static void
test_insert_exhaustion(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                       struct flow4_entry *pool, unsigned pool_cap)
{
    printf("[T] insert on free-list exhaustion\n");

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk, pool, pool_cap, 1 /* 1 ms */);

    uint64_t now = flow_cache_rdtsc();
    unsigned filled = 0;
    for (unsigned i = 0; i < pool_cap; i++) {
        struct flow4_key k = make_key(i + 1, i + 10001, (uint16_t)(i & 0xFFFF),
                                      (uint16_t)((i >> 16) & 0xFFFF), 6, i / 10);
        struct flow4_entry *e = flow4_cache_insert(&fc, &k, now);
        if (e)
            filled++;
    }
    printf("    filled: %u / %u\n", filled, pool_cap);

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&delay, NULL);
    uint64_t later = flow_cache_rdtsc();

    struct flow4_key kn = make_key(0xDEAD, 0xBEEF, 9999, 8888, 17, 42);
    struct flow4_entry *en = flow4_cache_insert(&fc, &kn, later);
    assert(en != NULL);
    assert(en->action == FLOW_ACTION_NONE);

    struct flow4_entry *results[1];
    flow4_cache_lookup_batch(&fc, &kn, 1, results);
    assert(results[0] == en);

    struct flow_cache_stats st;
    flow4_cache_stats(&fc, &st);
    assert(st.evictions >= 1);
    printf("    evictions: %" PRIu64 "\n", st.evictions);

    /* all live, no expiry — insert must return NULL */
    flow4_cache_init(&fc, buckets, nb_bk, pool, pool_cap, 0);
    now = flow_cache_rdtsc();
    for (unsigned i = 0; i < pool_cap; i++) {
        struct flow4_key k = make_key(i + 1, i + 10001, (uint16_t)(i & 0xFFFF),
                                      (uint16_t)((i >> 16) & 0xFFFF), 6, i / 10);
        flow4_cache_insert(&fc, &k, now);
    }
    struct flow4_key kf = make_key(0xFFFF, 0xFFFF, 1111, 2222, 6, 0);
    struct flow4_entry *ef = flow4_cache_insert(&fc, &kf, now);
    assert(ef == NULL);

    printf("    PASS\n");
}

/*===========================================================================
 * Bench parameters
 *===========================================================================*/
#define BENCH_BATCH     256u    /* packets per vector */
#define BENCH_WARMUP     50u

/*===========================================================================
 * Fill cache with N random keys, return array of inserted keys.
 * Caller must free returned array.
 *===========================================================================*/
static struct flow4_key *
fill_cache(struct flow4_cache *fc, unsigned count, unsigned *actual_out)
{
    struct flow4_key *keys = malloc((size_t)count * sizeof(*keys));
    assert(keys);
    unsigned actual = 0;
    for (unsigned i = 0; i < count; i++) {
        make_random_key(&keys[i]);
        struct flow4_entry *e = flow4_cache_insert(fc, &keys[i], 1000);
        if (e)
            actual++;
    }
    *actual_out = actual;
    return keys;
}

/*===========================================================================
 * Bench: insert throughput
 *===========================================================================*/
static void
bench_insert(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
             struct flow4_entry *pool, unsigned pool_cap)
{
    printf("\n[B] insert (pool_cap=%u, nb_bk=%u)\n", pool_cap, nb_bk);

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk, pool, pool_cap, 0);

    unsigned total = pool_cap;
    struct flow4_key *keys = malloc((size_t)total * sizeof(*keys));
    assert(keys);
    for (unsigned i = 0; i < total; i++)
        make_random_key(&keys[i]);

    uint64_t t0 = tsc_start();
    unsigned inserted = 0;
    for (unsigned i = 0; i < total; i++) {
        struct flow4_entry *e = flow4_cache_insert(&fc, &keys[i], 1000);
        if (e)
            inserted++;
    }
    uint64_t t1 = tsc_end();

    double cy = (inserted > 0) ? (double)(t1 - t0) / inserted : 0;
    printf("    inserted: %u / %u  (%.1f%% fill)\n",
           inserted, pool_cap,
           100.0 * inserted / (nb_bk * RIX_HASH_BUCKET_ENTRY_SZ));
    printf("    %.1f cycles/insert\n", cy);

    free(keys);
}

/*===========================================================================
 * Bench: pipelined batch lookup — hit-rate sweep (DRAM-cold)
 *
 * For each hit rate (0%, 10%, 20%, ... 100%), build a batch of
 * BENCH_BATCH keys where `hit_pct`% are drawn randomly from
 * registered keys and the rest are random miss keys.
 * Each repeat uses different random keys from the pool so that
 * buckets/nodes are DRAM-cold (working set >> L3).
 *===========================================================================*/
static void
bench_lookup(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
             struct flow4_entry *pool, unsigned pool_cap,
             unsigned repeat)
{
    printf("\n[B] pipelined batch lookup — hit-rate sweep (DRAM-cold)\n");
    printf("    pool_cap=%u  nb_bk=%u  batch=%u  repeat=%u\n",
           pool_cap, nb_bk, BENCH_BATCH, repeat);

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk, pool, pool_cap, 0);

    /* fill cache to ~75% of pool */
    unsigned fill_target = pool_cap * 3 / 4;
    unsigned actual_fill;
    double t_fill0 = now_sec();
    struct flow4_key *hit_keys = fill_cache(&fc, fill_target, &actual_fill);
    double t_fill1 = now_sec();
    printf("    filled: %u entries (%.1f%% of slots) in %.1f s\n",
           actual_fill,
           100.0 * actual_fill / (nb_bk * RIX_HASH_BUCKET_ENTRY_SZ),
           t_fill1 - t_fill0);

    if (actual_fill < BENCH_BATCH) {
        printf("    SKIP: too few entries for benchmark\n");
        free(hit_keys);
        return;
    }

    struct flow4_entry **results = malloc(BENCH_BATCH * sizeof(*results));
    struct flow4_key *batch = malloc(BENCH_BATCH * sizeof(*batch));
    assert(results && batch);

    /*
     * Pre-generate key pools for DRAM-cold measurement.
     * Each repeat uses a different batch of keys so that buckets/nodes
     * are cold (working set >> L3).
     *
     * miss_pool: random keys unlikely to exist in cache
     * hit_pool:  randomly sampled from registered keys
     */
    size_t pool_keys = (size_t)repeat * BENCH_BATCH;
    struct flow4_key *miss_pool = malloc(pool_keys * sizeof(*miss_pool));
    struct flow4_key *hit_pool  = malloc(pool_keys * sizeof(*hit_pool));
    assert(miss_pool && hit_pool);

    for (size_t i = 0; i < pool_keys; i++) {
        make_random_key(&miss_pool[i]);
        hit_pool[i] = hit_keys[xorshift64() % actual_fill];
    }

    printf("\n    hit%%   cycles/key\n");
    printf("    ----   ----------\n");

    for (unsigned pct = 0; pct <= 100; pct += 20) {
        unsigned nb_hit  = BENCH_BATCH * pct / 100;

        /* build all batches upfront */
        struct flow4_key *all_batches = malloc(pool_keys * sizeof(*all_batches));
        assert(all_batches);
        for (unsigned r = 0; r < repeat; r++) {
            struct flow4_key *bp = all_batches + (size_t)r * BENCH_BATCH;
            const struct flow4_key *hp = hit_pool + (size_t)r * BENCH_BATCH;
            const struct flow4_key *mp = miss_pool + (size_t)r * BENCH_BATCH;
            for (unsigned i = 0; i < nb_hit; i++)
                bp[i] = hp[i];
            for (unsigned i = nb_hit; i < BENCH_BATCH; i++)
                bp[i] = mp[i - nb_hit];
        }

        /* warmup */
        for (unsigned r = 0; r < BENCH_WARMUP && r < repeat; r++)
            flow4_cache_lookup_batch(&fc,
                                     all_batches + (size_t)r * BENCH_BATCH,
                                     BENCH_BATCH, results);

        /* measure — only lookup is inside tsc window */
        uint64_t total_cy = 0;
        for (unsigned r = 0; r < repeat; r++) {
            const struct flow4_key *bp =
                all_batches + (size_t)r * BENCH_BATCH;
            uint64_t t0 = tsc_start();
            flow4_cache_lookup_batch(&fc, bp, BENCH_BATCH, results);
            uint64_t t1 = tsc_end();
            total_cy += t1 - t0;
        }

        double avg = (double)total_cy / ((uint64_t)repeat * BENCH_BATCH);
        printf("    %3u%%   %6.1f\n", pct, avg);
        free(all_batches);
    }

    free(miss_pool);
    free(hit_pool);

    free(hit_keys);
    free(results);
    free(batch);
}

/*===========================================================================
 * Bench: expire
 *===========================================================================*/
static void
bench_expire(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
             struct flow4_entry *pool, unsigned pool_cap)
{
    printf("\n[B] expire (pool_cap=%u)\n", pool_cap);

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk, pool, pool_cap, 1 /* 1 ms */);

    unsigned filled;
    struct flow4_key *keys = fill_cache(&fc, pool_cap, &filled);
    printf("    filled: %u entries\n", filled);

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&delay, NULL);
    uint64_t later = flow_cache_rdtsc();

    uint64_t t0 = tsc_start();
    flow4_cache_expire(&fc, later, pool_cap);
    uint64_t t1 = tsc_end();

    struct flow_cache_stats st;
    flow4_cache_stats(&fc, &st);

    double cy = (st.evictions > 0) ? (double)(t1 - t0) / st.evictions : 0;
    printf("    evicted: %" PRIu64 "  %.1f cycles/eviction\n",
           st.evictions, cy);

    free(keys);
}

/*===========================================================================
 * Bench: single find (no pipeline) — measures raw hit/miss DRAM cost
 *
 * Uses flow4_ht_find (non-pipelined) to isolate the latency difference
 * between hit (bk[0] only) and miss (bk[0] + lazy bk[1]).
 *===========================================================================*/
static void
bench_single_find(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                  struct flow4_entry *pool, unsigned pool_cap,
                  unsigned repeat)
{
    printf("\n[B] single find (no pipeline) — hit vs miss latency\n");

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk, pool, pool_cap, 0);

    unsigned actual_fill;
    struct flow4_key *hit_keys = fill_cache(&fc, pool_cap * 3 / 4, &actual_fill);
    printf("    filled: %u entries\n", actual_fill);

    if (actual_fill < 256) {
        printf("    SKIP: too few entries\n");
        free(hit_keys);
        return;
    }

    unsigned nkeys = repeat * 4;
    if (nkeys > 200000)
        nkeys = 200000;

    struct flow4_key *hkeys = malloc(nkeys * sizeof(*hkeys));
    struct flow4_key *mkeys = malloc(nkeys * sizeof(*mkeys));
    assert(hkeys && mkeys);
    for (unsigned i = 0; i < nkeys; i++) {
        hkeys[i] = hit_keys[xorshift64() % actual_fill];
        make_random_key(&mkeys[i]);
    }

    /* 100% hit — single find */
    {
        uint64_t total_cy = 0;
        for (unsigned i = 0; i < nkeys; i++) {
            uint64_t t0 = tsc_start();
            struct flow4_entry *e __attribute__((unused)) =
                flow4_ht_find(&fc.ht_head, fc.buckets, fc.pool, &hkeys[i]);
            uint64_t t1 = tsc_end();
            total_cy += t1 - t0;
        }
        printf("    single hit:  %6.1f cycles/key  (%u keys)\n",
               (double)total_cy / nkeys, nkeys);
    }

    /* 100% miss — single find */
    {
        uint64_t total_cy = 0;
        for (unsigned i = 0; i < nkeys; i++) {
            uint64_t t0 = tsc_start();
            struct flow4_entry *e __attribute__((unused)) =
                flow4_ht_find(&fc.ht_head, fc.buckets, fc.pool, &mkeys[i]);
            uint64_t t1 = tsc_end();
            total_cy += t1 - t0;
        }
        printf("    single miss: %6.1f cycles/key  (%u keys)\n",
               (double)total_cy / nkeys, nkeys);
    }

    /* pipelined batch for comparison */
    {
        struct flow4_entry **results = malloc(BENCH_BATCH * sizeof(*results));
        assert(results);

        uint64_t cy_hit = 0, cy_miss = 0;
        unsigned nrep = nkeys / BENCH_BATCH;
        for (unsigned r = 0; r < nrep; r++) {
            uint64_t t0 = tsc_start();
            flow4_cache_lookup_batch(&fc,
                                     hkeys + (size_t)r * BENCH_BATCH,
                                     BENCH_BATCH, results);
            uint64_t t1 = tsc_end();
            cy_hit += t1 - t0;
        }
        for (unsigned r = 0; r < nrep; r++) {
            uint64_t t0 = tsc_start();
            flow4_cache_lookup_batch(&fc,
                                     mkeys + (size_t)r * BENCH_BATCH,
                                     BENCH_BATCH, results);
            uint64_t t1 = tsc_end();
            cy_miss += t1 - t0;
        }
        unsigned total_keys = nrep * BENCH_BATCH;
        printf("    batch  hit:  %6.1f cycles/key  (%u keys)\n",
               (double)cy_hit / total_keys, total_keys);
        printf("    batch  miss: %6.1f cycles/key  (%u keys)\n",
               (double)cy_miss / total_keys, total_keys);
        free(results);
    }

    free(hkeys);
    free(mkeys);
    free(hit_keys);
}

/*===========================================================================
 * Bench: packet processing loop — lookup + insert + expire
 *
 * Simulates real packet processing:
 *   1. Receive up to 256 packets (90% existing flows, 10% new flows)
 *   2. Pipelined batch lookup
 *   3. Insert on miss
 *   4. If fill > 75%, expire nb_pkts entries
 *===========================================================================*/
static void
run_pkt_loop(struct rix_hash_bucket_s *buckets, unsigned nb_bk_use,
             struct flow4_entry *pool, unsigned pool_cap,
             unsigned repeat, const char *label)
{
    unsigned total_slots = nb_bk_use * RIX_HASH_BUCKET_ENTRY_SZ;

    printf("\n[B] packet processing loop — %s\n", label);
    printf("    pool_cap=%u  nb_bk=%u  total_slots=%u  batch=%u  repeat=%u\n",
           pool_cap, nb_bk_use, total_slots, BENCH_BATCH, repeat);

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk_use, pool, pool_cap, 1 /* 1 ms */);

    /* pre-fill to just under 75% threshold (1 batch margin) */
    unsigned prefill = total_slots - (total_slots >> 2) - BENCH_BATCH;
    if (prefill > pool_cap)
        prefill = pool_cap * 70 / 100;
    unsigned filled;
    struct flow4_key *prefill_keys = fill_cache(&fc, prefill, &filled);
    printf("    pre-filled: %u entries (%.1f%% fill)\n",
           filled, 100.0 * filled / total_slots);

    /* age out pre-filled entries so expire can reclaim them */
    struct timespec age_delay = { .tv_sec = 0, .tv_nsec = 5000000 };
    nanosleep(&age_delay, NULL);

    /* pre-generate batches: 90% hit (first half of prefill) + 10% miss */
    size_t total_keys = (size_t)repeat * BENCH_BATCH;
    struct flow4_key *pkt_keys = malloc(total_keys * sizeof(*pkt_keys));
    assert(pkt_keys);
    unsigned nb_hit_per_batch = BENCH_BATCH * 90 / 100;
    unsigned active_flows = filled / 2;
    if (active_flows < 1)
        active_flows = 1;
    for (unsigned r = 0; r < repeat; r++) {
        struct flow4_key *bp = pkt_keys + (size_t)r * BENCH_BATCH;
        for (unsigned i = 0; i < nb_hit_per_batch; i++)
            bp[i] = prefill_keys[xorshift64() % active_flows];
        for (unsigned i = nb_hit_per_batch; i < BENCH_BATCH; i++)
            make_random_key(&bp[i]);
    }
    free(prefill_keys);

    struct flow4_entry **results = malloc(BENCH_BATCH * sizeof(*results));
    assert(results);

    uint64_t total_lookup_cy = 0;
    uint64_t total_expire_cy = 0;
    unsigned total_hits = 0, total_misses = 0, total_inserts = 0;
    unsigned total_expire_calls = 0;

    for (unsigned r = 0; r < repeat; r++) {
        const struct flow4_key *batch_keys =
            pkt_keys + (size_t)r * BENCH_BATCH;
        uint64_t now = flow_cache_rdtsc();

        /* step 1+2: pipelined batch lookup */
        uint64_t t0 = tsc_start();
        flow4_cache_lookup_batch(&fc, batch_keys, BENCH_BATCH, results);

        /* step 3: insert on miss, touch on hit */
        for (unsigned i = 0; i < BENCH_BATCH; i++) {
            if (results[i] != NULL) {
                flow4_cache_touch(results[i], now, 64);
                total_hits++;
            } else {
                struct flow4_entry *e =
                    flow4_cache_insert(&fc, &batch_keys[i], now);
                if (e)
                    total_inserts++;
                total_misses++;
            }
        }
        uint64_t t1 = tsc_end();
        total_lookup_cy += t1 - t0;

        /* step 4: expire if over 75% threshold */
        if (flow4_cache_over_threshold(&fc)) {
            uint64_t te0 = tsc_start();
            flow4_cache_expire(&fc, now, BENCH_BATCH);
            uint64_t te1 = tsc_end();
            total_expire_cy += te1 - te0;
            total_expire_calls++;
        }
    }

    unsigned total_pkts = repeat * BENCH_BATCH;
    struct flow_cache_stats st;
    flow4_cache_stats(&fc, &st);

    printf("    total packets: %u\n", total_pkts);
    printf("    hits: %u (%.1f%%)  misses: %u  inserts: %u\n",
           total_hits, 100.0 * total_hits / total_pkts,
           total_misses, total_inserts);
    printf("    expire calls: %u / %u batches  evictions: %" PRIu64 "\n",
           total_expire_calls, repeat, st.evictions);
    printf("    final entries: %u (%.1f%% fill)\n",
           st.nb_entries,
           100.0 * st.nb_entries / total_slots);
    printf("    lookup+insert: %.1f cycles/pkt\n",
           (double)total_lookup_cy / total_pkts);
    if (total_expire_calls > 0)
        printf("    expire:        %.1f cycles/call (%.1f cy/pkt amortized)\n",
               (double)total_expire_cy / total_expire_calls,
               (double)total_expire_cy / total_pkts);
    else
        printf("    expire:        not triggered (fill < 75%%)\n");

    free(pkt_keys);
    free(results);
}

static void
bench_pkt_loop(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
               struct flow4_entry *pool, unsigned pool_cap,
               unsigned repeat)
{
    /* (1) standard sizing: pool_cap ≈ 50% of slots — normal operation */
    run_pkt_loop(buckets, nb_bk, pool, pool_cap, repeat,
                 "standard sizing (pool ≈ 50%% fill)");

    /* (2) tight sizing: pool_cap > 74% of total_slots — stress threshold */
    unsigned nb_bk_tight = next_pow2((pool_cap + RIX_HASH_BUCKET_ENTRY_SZ - 1)
                                     / RIX_HASH_BUCKET_ENTRY_SZ);
    /* if total_slots >> pool_cap, halve so pool can reach 74%+ fill */
    while (nb_bk_tight > 1 &&
           pool_cap < ((uint64_t)nb_bk_tight * RIX_HASH_BUCKET_ENTRY_SZ * 3 >> 2))
        nb_bk_tight >>= 1;
    if (nb_bk_tight <= nb_bk)
        run_pkt_loop(buckets, nb_bk_tight, pool, pool_cap, repeat,
                     "tight sizing (pool ≈ 100%% slots)");
}

/*===========================================================================
 * Bench: bk[0] placement rate vs fill rate
 *
 * Measures what fraction of entries reside in their primary bucket (bk[0])
 * at different hash table fill levels.
 * Uses a dedicated nb_bk so that pool_cap ≈ total_slots, allowing fill up
 * to 90%.  Bypasses flow4_cache_insert's 75% cap by inserting directly
 * into the hash table.
 *===========================================================================*/
static void
bench_bk0_rate(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
               struct flow4_entry *pool, unsigned pool_cap)
{
    /*
     * Choose nb_bk_local so that pool_cap ≈ total_slots.
     * This lets us fill the hash table up to ~90% using pool entries.
     */
    unsigned nb_bk_local = next_pow2((pool_cap + RIX_HASH_BUCKET_ENTRY_SZ - 1)
                                     / RIX_HASH_BUCKET_ENTRY_SZ);
    if (nb_bk_local > nb_bk)
        nb_bk_local = nb_bk;  /* don't exceed allocated bucket memory */
    unsigned total_slots = nb_bk_local * RIX_HASH_BUCKET_ENTRY_SZ;

    printf("\n[B] bk[0] placement rate vs fill rate\n");
    printf("    pool_cap=%u  nb_bk=%u  total_slots=%u\n",
           pool_cap, nb_bk_local, total_slots);

    printf("\n    fill%%   entries   bk0%%    bk1%%\n");
    printf("    -----   -------   -----   -----\n");

    for (unsigned pct = 10; pct <= 90; pct += 10) {
        /* reinit with local nb_bk */
        struct flow4_ht ht_head;
        flow4_ht_init(&ht_head, nb_bk_local);
        memset(buckets, 0, (size_t)nb_bk_local * sizeof(*buckets));
        memset(pool, 0, (size_t)pool_cap * sizeof(*pool));

        /* target fill = pct% of total_slots */
        unsigned target = (unsigned)((uint64_t)total_slots * pct / 100);
        if (target > pool_cap)
            target = pool_cap;

        /* insert directly into hash table (bypass 75% cap) */
        unsigned inserted = 0;
        unsigned pool_idx = 0;
        for (unsigned i = 0; i < target && pool_idx < pool_cap; i++) {
            struct flow4_entry *e = &pool[pool_idx++];
            make_random_key(&e->key);
            e->flags = FLOW4_FLAG_VALID;

            struct flow4_entry *dup =
                flow4_ht_insert(&ht_head, buckets, pool, e);
            if (dup == NULL) {
                inserted++;
            } else {
                e->flags = 0;  /* failed or duplicate */
            }
        }

        if (inserted == 0) {
            printf("    %3u%%   %7u   (skip)\n", pct, inserted);
            continue;
        }

        /* count bk[0] vs bk[1] placement */
        unsigned mask = ht_head.rhh_mask;
        unsigned in_bk0 = 0, in_bk1 = 0;
        for (unsigned i = 0; i < pool_idx; i++) {
            struct flow4_entry *e = &pool[i];
            if (!(e->flags & FLOW4_FLAG_VALID))
                continue;

            /* recompute hash to get primary bucket */
            union rix_hash_hash_u h =
                _rix_hash_fn_crc32((const void *)&e->key,
                                   sizeof(struct flow4_key), mask);
            unsigned bk0 = h.val32[0] & mask;
            unsigned cur_bk = e->cur_hash & mask;

            if (cur_bk == bk0)
                in_bk0++;
            else
                in_bk1++;
        }

        double bk0_pct = 100.0 * in_bk0 / (in_bk0 + in_bk1);
        double bk1_pct = 100.0 * in_bk1 / (in_bk0 + in_bk1);
        double actual_fill = 100.0 * inserted / total_slots;
        printf("    %3.0f%%   %7u   %5.1f   %5.1f   (actual fill: %.1f%%)\n",
               (double)pct, inserted, bk0_pct, bk1_pct, actual_fill);
    }
}

/*===========================================================================
 * Usage
 *===========================================================================*/
static void
usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [pool_cap [nb_bk [repeat]]]\n"
            "  pool_cap : entry pool capacity (default: 1024)\n"
            "  nb_bk    : hash buckets, must be power-of-2\n"
            "             (default: auto, ~50%% fill target)\n"
            "  repeat   : benchmark repeat count (default: auto)\n"
            "\n"
            "  Each bucket holds 16 slots.\n"
            "  Example: %s 1000000\n"
            "           %s 100000000 8388608\n"
            "           %s 100000000 8388608 500\n",
            prog, prog, prog, prog);
}

/*===========================================================================
 * Main
 *===========================================================================*/
int
main(int argc, char **argv)
{
    rix_hash_arch_init();

    unsigned pool_cap = 1024;
    unsigned nb_bk = 0;    /* auto */
    unsigned repeat = 0;   /* auto */

    if (argc > 1 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    if (argc > 1) {
        pool_cap = (unsigned)strtoul(argv[1], NULL, 0);
        if (pool_cap < 64) {
            fprintf(stderr, "pool_cap must be >= 64\n");
            return 1;
        }
    }
    if (argc > 2) {
        nb_bk = (unsigned)strtoul(argv[2], NULL, 0);
        if (nb_bk != 0 && (nb_bk & (nb_bk - 1)) != 0) {
            fprintf(stderr, "nb_bk must be a power of 2 (0 = auto)\n");
            return 1;
        }
    }
    if (argc > 3) {
        repeat = (unsigned)strtoul(argv[3], NULL, 0);
    }

    /* auto-size buckets: target ~50% fill (16 slots per bucket) */
    if (nb_bk == 0) {
        unsigned slots_needed = pool_cap * 2;
        nb_bk = next_pow2((slots_needed + RIX_HASH_BUCKET_ENTRY_SZ - 1)
                          / RIX_HASH_BUCKET_ENTRY_SZ);
        if (nb_bk < 16)
            nb_bk = 16;
    }

    /* auto repeat: fewer for large tables */
    if (repeat == 0) {
        if (pool_cap >= 100000000)
            repeat = 200;
        else if (pool_cap >= 10000000)
            repeat = 500;
        else if (pool_cap >= 1000000)
            repeat = 1000;
        else
            repeat = 2000;
    }

    size_t bk_size   = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    size_t pool_size = (size_t)pool_cap * sizeof(struct flow4_entry);

    printf("=== flow4_cache test & bench ===\n");
    printf("  pool_cap   = %u\n", pool_cap);
    printf("  nb_bk      = %u  (slots = %u)\n",
           nb_bk, nb_bk * RIX_HASH_BUCKET_ENTRY_SZ);
    printf("  bucket mem = %.1f MB\n", bk_size / 1e6);
    printf("  pool mem   = %.1f MB\n", pool_size / 1e6);
    printf("  total mem  = %.1f MB\n", (bk_size + pool_size) / 1e6);
    printf("  repeat     = %u\n", repeat);
    printf("\n");

    struct rix_hash_bucket_s *buckets = alloc_aligned(bk_size);
    struct flow4_entry *pool = alloc_aligned(pool_size);

    /* --- correctness tests --- */
    test_struct_sizes();
    test_init_insert_find(buckets, nb_bk, pool, pool_cap);
    test_expire(buckets, nb_bk, pool, pool_cap);
    test_batch_lookup(buckets, nb_bk, pool, pool_cap);
    test_insert_exhaustion(buckets, nb_bk, pool, pool_cap);
    printf("\nALL TESTS PASSED\n");

    /* --- benchmarks --- */
    bench_insert(buckets, nb_bk, pool, pool_cap);
    bench_lookup(buckets, nb_bk, pool, pool_cap, repeat);
    bench_single_find(buckets, nb_bk, pool, pool_cap, repeat);
    bench_expire(buckets, nb_bk, pool, pool_cap);
    bench_pkt_loop(buckets, nb_bk, pool, pool_cap, repeat);
    bench_bk0_rate(buckets, nb_bk, pool, pool_cap);

    printf("\n");

    munmap(buckets, bk_size);
    munmap(pool, pool_size);
    return 0;
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
