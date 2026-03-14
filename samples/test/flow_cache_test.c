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
#include <getopt.h>

#include "flow_cache.h"

/*
 * flow4_ht functions needed by flow4_bench_single_find (ht_find) and
 * flow4_bench_bk0_rate (ht_init, ht_insert) which probe raw hash table
 * internals directly.  flow6_ht / flowu_ht are only accessed via the
 * cache API, so no generate is needed for them here.
 */
RIX_HASH_GENERATE_STATIC(flow4_ht, flow4_entry, key, cur_hash, flow4_cmp)

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
 * Key generation - IPv4
 *===========================================================================*/
static struct flow4_key
make_key4(uint32_t src, uint32_t dst, uint16_t sp, uint16_t dp,
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
make_random_key4(struct flow4_key *k)
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
 * Key generation - IPv6
 *===========================================================================*/
static void
make_random_key6(struct flow6_key *k)
{
    uint64_t r0 = xorshift64();
    uint64_t r1 = xorshift64();
    uint64_t r2 = xorshift64();
    uint64_t r3 = xorshift64();
    uint64_t r4 = xorshift64();
    memset(k, 0, sizeof(*k));
    memcpy(k->src_ip,     &r0, 8);
    memcpy(k->src_ip + 8, &r1, 8);
    memcpy(k->dst_ip,     &r2, 8);
    memcpy(k->dst_ip + 8, &r3, 8);
    k->src_port = (uint16_t)r4;
    k->dst_port = (uint16_t)(r4 >> 16);
    k->proto    = (uint8_t)(r4 >> 32);
    k->vrfid    = (uint32_t)(r4 >> 40);
}

/*===========================================================================
 * Key generation - Unified (random: 50% v4, 50% v6)
 *===========================================================================*/
static void
make_random_keyu(struct flowu_key *k)
{
    uint64_t r0 = xorshift64();
    if (r0 & 1) {
        /* IPv4 */
        uint64_t r1 = xorshift64();
        uint64_t r2 = xorshift64();
        *k = flowu_key_v4((uint32_t)r0, (uint32_t)(r0 >> 32),
                           (uint16_t)r1, (uint16_t)(r1 >> 16),
                           (uint8_t)(r1 >> 32), (uint32_t)r2);
    } else {
        /* IPv6 */
        uint64_t r1 = xorshift64();
        uint64_t r2 = xorshift64();
        uint64_t r3 = xorshift64();
        uint64_t r4 = xorshift64();
        uint8_t src[16], dst[16];
        memcpy(src,     &r0, 8);
        memcpy(src + 8, &r1, 8);
        memcpy(dst,     &r2, 8);
        memcpy(dst + 8, &r3, 8);
        *k = flowu_key_v6(src, dst,
                           (uint16_t)r4, (uint16_t)(r4 >> 16),
                           (uint8_t)(r4 >> 32), (uint32_t)(r4 >> 40));
    }
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
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/*===========================================================================
 * Bench parameters
 *===========================================================================*/
#define BENCH_BATCH     256u    /* packets per vector */
#define BENCH_WARMUP     50u

/*===========================================================================
 * IPv4-specific tests (init/insert/find, insert_exhaustion use make_key4)
 *===========================================================================*/
static void
flow4_test_init_insert_find(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                            struct flow4_entry *pool, unsigned max_entries)
{
    printf("[T-IPv4] init/insert/find\n");

    struct flow4_cache fc;
    uint64_t now = 1000000;

    flow4_cache_init(&fc, buckets, nb_bk, pool, max_entries, 0);

    struct flow4_key k1 = make_key4(0x0A000001, 0x0A000002,
                                     1234, 80, 6, 100);
    struct flow4_entry *e1 = flow4_cache_insert(&fc, &k1, now);
    assert(e1 != NULL);
    assert(e1->action == FLOW_ACTION_NONE);
    assert(e1->last_ts != 0);

    struct flow4_entry *results[1];
    flow4_cache_lookup_batch(&fc, &k1, 1, results);
    assert(results[0] == e1);

    struct flow4_key k2 = make_key4(0x0A000003, 0x0A000004,
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

static void
flow4_test_insert_exhaustion(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                             struct flow4_entry *pool, unsigned max_entries)
{
    printf("[T-IPv4] insert on free-list exhaustion\n");

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk, pool, max_entries, 1 /* 1 ms */);

    uint64_t now = flow_cache_rdtsc();
    unsigned filled = 0;
    for (unsigned i = 0; i < max_entries; i++) {
        struct flow4_key k = make_key4(i + 1, i + 10001,
                                        (uint16_t)(i & 0xFFFF),
                                        (uint16_t)((i >> 16) & 0xFFFF),
                                        6, i / 10);
        struct flow4_entry *e = flow4_cache_insert(&fc, &k, now);
        if (e)
            filled++;
    }
    printf("    filled: %u / %u\n", filled, max_entries);

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&delay, NULL);
    uint64_t later = flow_cache_rdtsc();

    struct flow4_key kn = make_key4(0xDEAD, 0xBEEF, 9999, 8888, 17, 42);
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

    /* all live, no expiry - insert must still succeed (forced bucket eviction) */
    flow4_cache_init(&fc, buckets, nb_bk, pool, max_entries, 0);
    now = flow_cache_rdtsc();
    for (unsigned i = 0; i < max_entries; i++) {
        struct flow4_key k = make_key4(i + 1, i + 10001,
                                        (uint16_t)(i & 0xFFFF),
                                        (uint16_t)((i >> 16) & 0xFFFF),
                                        6, i / 10);
        flow4_cache_insert(&fc, &k, now);
    }
    struct flow4_key kf = make_key4(0xFFFF, 0xFFFF, 1111, 2222, 6, 0);
    struct flow4_entry *ef = flow4_cache_insert(&fc, &kf, now);
    assert(ef != NULL);  /* cache never fails: oldest bucket entry evicted */
    flow4_cache_stats(&fc, &st);
    printf("    filled: %u / %u\n", st.nb_entries, st.max_entries);
    printf("    evictions: %" PRIu64 "\n", st.evictions);

    printf("    PASS\n");
}

/*===========================================================================
 * IPv6-specific test (init/insert/find)
 *===========================================================================*/
static void
flow6_test_init_insert_find(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                            struct flow6_entry *pool, unsigned max_entries)
{
    printf("[T-IPv6] init/insert/find\n");

    struct flow6_cache fc;
    uint64_t now = 1000000;

    flow6_cache_init(&fc, buckets, nb_bk, pool, max_entries, 0);

    uint8_t src1[16] = {0,0,0,0,0,0,0,0, 0,0,0,0, 0,0x0a,0,1};
    uint8_t dst1[16] = {0,0,0,0,0,0,0,0, 0,0,0,0, 0,0x0a,0,2};
    struct flow6_key k1;
    memset(&k1, 0, sizeof(k1));
    memcpy(k1.src_ip, src1, 16);
    memcpy(k1.dst_ip, dst1, 16);
    k1.src_port = 1234; k1.dst_port = 80; k1.proto = 6; k1.vrfid = 100;

    struct flow6_entry *e1 = flow6_cache_insert(&fc, &k1, now);
    assert(e1 != NULL);
    assert(e1->action == FLOW_ACTION_NONE);
    assert(e1->last_ts != 0);

    struct flow6_entry *results[1];
    flow6_cache_lookup_batch(&fc, &k1, 1, results);
    assert(results[0] == e1);

    struct flow6_key k2;
    make_random_key6(&k2);
    flow6_cache_lookup_batch(&fc, &k2, 1, results);
    assert(results[0] == NULL);

    flow6_cache_update_action(e1, 42, 7);
    assert(e1->action == 42);
    assert(e1->qos_class == 7);

    flow6_cache_touch(e1, now + 100, 64);
    assert(e1->last_ts == now + 100);
    assert(e1->packets == 1);
    assert(e1->bytes == 64);

    printf("    PASS\n");
}

/*===========================================================================
 * IPv4-specific bench: single find + bk0 rate
 *===========================================================================*/
static void
flow4_bench_single_find(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                        struct flow4_entry *pool, unsigned max_entries,
                        unsigned repeat)
{
    printf("\n[B-IPv4] single find (no pipeline) - hit vs miss latency\n");

    struct flow4_cache fc;
    flow4_cache_init(&fc, buckets, nb_bk, pool, max_entries, 0);

    unsigned actual_fill;
    struct flow4_key *hit_keys;

    /* instantiate via template helper */
#define FCT_MAKE_RANDOM_KEY(kp) make_random_key4(kp)
    {
        hit_keys = malloc((size_t)(max_entries * 3 / 4) * sizeof(*hit_keys));
        assert(hit_keys);
        actual_fill = 0;
        for (unsigned i = 0; i < max_entries * 3 / 4; i++) {
            make_random_key4(&hit_keys[i]);
            struct flow4_entry *e = flow4_cache_insert(&fc, &hit_keys[i], 1000);
            if (e)
                actual_fill++;
        }
    }
#undef FCT_MAKE_RANDOM_KEY
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
        make_random_key4(&mkeys[i]);
    }

    /* 100% hit */
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
    /* 100% miss */
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

static void
flow4_bench_bk0_rate(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                     struct flow4_entry *pool, unsigned max_entries)
{
    unsigned nb_bk_local = next_pow2((max_entries + RIX_HASH_BUCKET_ENTRY_SZ - 1)
                                     / RIX_HASH_BUCKET_ENTRY_SZ);
    if (nb_bk_local > nb_bk)
        nb_bk_local = nb_bk;
    unsigned total_slots = nb_bk_local * RIX_HASH_BUCKET_ENTRY_SZ;

    printf("\n[B-IPv4] bk[0] placement rate vs fill rate\n");
    printf("    max_entries=%u  nb_bk=%u  total_slots=%u\n",
           max_entries, nb_bk_local, total_slots);

    printf("\n    fill%%   entries   bk0%%    bk1%%\n");
    printf("    -----   -------   -----   -----\n");

    for (unsigned pct = 10; pct <= 90; pct += 10) {
        struct flow4_ht ht_head;
        flow4_ht_init(&ht_head, nb_bk_local);
        memset(buckets, 0, (size_t)nb_bk_local * sizeof(*buckets));
        memset(pool, 0, (size_t)max_entries * sizeof(*pool));

        unsigned target = (unsigned)((uint64_t)total_slots * pct / 100);
        if (target > max_entries)
            target = max_entries;

        unsigned inserted = 0;
        unsigned pool_idx = 0;
        for (unsigned i = 0; i < target && pool_idx < max_entries; i++) {
            struct flow4_entry *e = &pool[pool_idx++];
            make_random_key4(&e->key);
            e->last_ts = 1;  /* non-zero = valid */

            struct flow4_entry *dup =
                flow4_ht_insert(&ht_head, buckets, pool, e);
            if (dup == NULL)
                inserted++;
            else
                e->last_ts = 0;
        }

        if (inserted == 0) {
            printf("    %3u%%   %7u   (skip)\n", pct, inserted);
            continue;
        }

        unsigned mask = ht_head.rhh_mask;
        unsigned in_bk0 = 0, in_bk1 = 0;
        for (unsigned i = 0; i < pool_idx; i++) {
            struct flow4_entry *e = &pool[i];
            if (e->last_ts == 0)
                continue;
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
 * Instantiate templated tests & benchmarks for IPv4
 *===========================================================================*/
#define FCT_PREFIX        flow4
#define FCT_KEY           flow4_key
#define FCT_ENTRY         flow4_entry
#define FCT_CACHE         flow4_cache
#define FCT_LABEL         "IPv4"
#define FCT_KEY_BYTES     20
#define FCT_MAKE_RANDOM_KEY(kp)  make_random_key4(kp)
#include "flow_cache_test_body.h"
#undef FCT_PREFIX
#undef FCT_KEY
#undef FCT_ENTRY
#undef FCT_CACHE
#undef FCT_LABEL
#undef FCT_KEY_BYTES
#undef FCT_MAKE_RANDOM_KEY

/*===========================================================================
 * Instantiate templated tests & benchmarks for IPv6
 *===========================================================================*/
#define FCT_PREFIX        flow6
#define FCT_KEY           flow6_key
#define FCT_ENTRY         flow6_entry
#define FCT_CACHE         flow6_cache
#define FCT_LABEL         "IPv6"
#define FCT_KEY_BYTES     44
#define FCT_MAKE_RANDOM_KEY(kp)  make_random_key6(kp)
#include "flow_cache_test_body.h"
#undef FCT_PREFIX
#undef FCT_KEY
#undef FCT_ENTRY
#undef FCT_CACHE
#undef FCT_LABEL
#undef FCT_KEY_BYTES
#undef FCT_MAKE_RANDOM_KEY

/*===========================================================================
 * Instantiate templated tests & benchmarks for Unified
 *===========================================================================*/
#define FCT_PREFIX        flowu
#define FCT_KEY           flowu_key
#define FCT_ENTRY         flowu_entry
#define FCT_CACHE         flowu_cache
#define FCT_LABEL         "Unified"
#define FCT_KEY_BYTES     44
#define FCT_MAKE_RANDOM_KEY(kp)  make_random_keyu(kp)
#include "flow_cache_test_body.h"
#undef FCT_PREFIX
#undef FCT_KEY
#undef FCT_ENTRY
#undef FCT_CACHE
#undef FCT_LABEL
#undef FCT_KEY_BYTES
#undef FCT_MAKE_RANDOM_KEY

/*===========================================================================
 * Unified-specific test: v4 and v6 coexist in same table
 *===========================================================================*/
static void
flowu_test_mixed(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                 struct flowu_entry *pool, unsigned max_entries)
{
    printf("[T-Unified] mixed v4/v6 in same table\n");

    struct flowu_cache fc;
    uint64_t now = 1000000;
    flowu_cache_init(&fc, buckets, nb_bk, pool, max_entries, 0);

    /* insert an IPv4 flow */
    struct flowu_key k4 = flowu_key_v4(0x0A000001, 0x0A000002,
                                        1234, 80, 6, 100);
    struct flowu_entry *e4 = flowu_cache_insert(&fc, &k4, now);
    assert(e4 != NULL);
    assert(e4->key.family == FLOW_FAMILY_IPV4);

    /* insert an IPv6 flow with same ports/proto/vrf */
    uint8_t src6[16] = {0x20,0x01,0x0d,0xb8, 0,0,0,0, 0,0,0,0, 0,0,0,1};
    uint8_t dst6[16] = {0x20,0x01,0x0d,0xb8, 0,0,0,0, 0,0,0,0, 0,0,0,2};
    struct flowu_key k6 = flowu_key_v6(src6, dst6, 1234, 80, 6, 100);
    struct flowu_entry *e6 = flowu_cache_insert(&fc, &k6, now);
    assert(e6 != NULL);
    assert(e6->key.family == FLOW_FAMILY_IPV6);
    assert(e6 != e4);   /* must be different entries (family differs) */

    /* lookup both - must find correct entries */
    struct flowu_entry *results[2];
    struct flowu_key keys[2] = { k4, k6 };
    flowu_cache_lookup_batch(&fc, keys, 2, results);
    assert(results[0] == e4);
    assert(results[1] == e6);

    /* lookup with swapped families - must miss */
    struct flowu_key k4_as_v6 = k4;
    k4_as_v6.family = FLOW_FAMILY_IPV6;
    flowu_cache_lookup_batch(&fc, &k4_as_v6, 1, results);
    assert(results[0] == NULL);

    struct flow_cache_stats st;
    flowu_cache_stats(&fc, &st);
    assert(st.inserts == 2);
    assert(st.nb_entries == 2);

    printf("    PASS\n");
}

/*===========================================================================
 * Usage
 *===========================================================================*/
static void
usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "\n"
            "Options:\n"
            "  -n, --entries N    Entry pool capacity, rounded up to next power-of-2\n"
            "                     (default: 1024, min: 64)\n"
            "  -b, --buckets N    Hash bucket count, must be power-of-2\n"
            "                     (default: auto, targets ~50%% fill)\n"
            "  -r, --repeat N     Benchmark repeat count (default: auto)\n"
            "  -h, --help         Show this help and exit\n"
            "\n"
            "Each bucket holds %u slots.  Bucket memory = N * %zu bytes.\n"
            "\n"
            "Examples:\n"
            "  %s -n 1000000\n"
            "  %s -n 100000000 -b 8388608\n"
            "  %s -n 100000000 -b 8388608 -r 500\n",
            prog,
            (unsigned)RIX_HASH_BUCKET_ENTRY_SZ,
            sizeof(struct rix_hash_bucket_s),
            prog, prog, prog);
}

/*===========================================================================
 * Call-site macro for template-generated functions (mirrors FCT_FN in body.h)
 * FCT_CALL(prefix, suffix) expands to prefix##_##suffix after full macro
 * expansion of both arguments.  Consistent with FC_CALL(prefix, suffix).
 *===========================================================================*/
#define _FCT_CALL_CAT(a, b, c)      a ## b ## c
#define FCT_CALL_CAT(a, b, c)       _FCT_CALL_CAT(a, b, c)
#define FCT_CALL(prefix, suffix)    FCT_CALL_CAT(prefix, _, suffix)

/*===========================================================================
 * Main
 *===========================================================================*/
int
main(int argc, char **argv)
{
    rix_hash_arch_init();

    unsigned max_entries = 1048576;  /* 2^20 ~= 1M: realistic DRAM-cold benchmark */
    unsigned nb_bk = 0;    /* auto */
    unsigned repeat = 0;   /* auto */

    static const struct option long_opts[] = {
        { "entries", required_argument, NULL, 'n' },
        { "buckets", required_argument, NULL, 'b' },
        { "repeat",  required_argument, NULL, 'r' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL,      0,                 NULL,  0  },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:b:r:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n':
            max_entries = next_pow2((unsigned)strtoul(optarg, NULL, 0));
            if (max_entries < 64) {
                fprintf(stderr, "error: --entries must be >= 64\n");
                return 1;
            }
            break;
        case 'b':
            nb_bk = (unsigned)strtoul(optarg, NULL, 0);
            if (nb_bk != 0 && (nb_bk & (nb_bk - 1)) != 0) {
                fprintf(stderr, "error: --buckets must be a power of 2\n");
                return 1;
            }
            break;
        case 'r':
            repeat = (unsigned)strtoul(optarg, NULL, 0);
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* auto-size buckets: target ~50% fill (16 slots per bucket) */
    if (nb_bk == 0) {
        unsigned slots_needed = max_entries * 2;
        nb_bk = next_pow2((slots_needed + RIX_HASH_BUCKET_ENTRY_SZ - 1)
                          / RIX_HASH_BUCKET_ENTRY_SZ);
        if (nb_bk < 16)
            nb_bk = 16;
    }

    /* auto repeat: fewer for large tables */
    if (repeat == 0) {
        if (max_entries >= 100000000)
            repeat = 200;
        else if (max_entries >= 10000000)
            repeat = 500;
        else if (max_entries >= 1000000)
            repeat = 1000;
        else
            repeat = 2000;
    }

    size_t bk_size    = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    size_t pool4_size = (size_t)max_entries * sizeof(struct flow4_entry);
    size_t pool6_size = (size_t)max_entries * sizeof(struct flow6_entry);
    size_t poolu_size = (size_t)max_entries * sizeof(struct flowu_entry);

    printf("=== flow cache test & bench (IPv4 / IPv6 / Unified) ===\n");
    printf("  max_entries   = %u\n", max_entries);
    printf("  nb_bk      = %u  (slots = %u)\n",
           nb_bk, nb_bk * RIX_HASH_BUCKET_ENTRY_SZ);
    printf("  bucket mem = %.1f MB\n", (double)bk_size   / 1e6);
    printf("  pool4 mem  = %.1f MB\n", (double)pool4_size / 1e6);
    printf("  pool6 mem  = %.1f MB\n", (double)pool6_size / 1e6);
    printf("  poolu mem  = %.1f MB\n", (double)poolu_size / 1e6);
    printf("  repeat     = %u\n", repeat);
    printf("\n");

    struct rix_hash_bucket_s *buckets = alloc_aligned(bk_size);
    struct flow4_entry *pool4 = alloc_aligned(pool4_size);
    struct flow6_entry *pool6 = alloc_aligned(pool6_size);
    struct flowu_entry *poolu = alloc_aligned(poolu_size);

    /* --- IPv4 correctness tests --- */
    printf("--- IPv4 ---\n");
    FCT_CALL(flow4, test_struct_sizes)();
    flow4_test_init_insert_find(buckets, nb_bk, pool4, max_entries); /* IPv4-specific */
    FCT_CALL(flow4, test_find)(buckets, nb_bk, pool4, max_entries);
    FCT_CALL(flow4, test_remove)(buckets, nb_bk, pool4, max_entries);
    FCT_CALL(flow4, test_flush)(buckets, nb_bk, pool4, max_entries);
    FCT_CALL(flow4, test_expire)(buckets, nb_bk, pool4, max_entries);
    FCT_CALL(flow4, test_batch_lookup)(buckets, nb_bk, pool4, max_entries);
    flow4_test_insert_exhaustion(buckets, nb_bk, pool4, max_entries); /* IPv4-specific */
    printf("\nALL IPv4 TESTS PASSED\n");

    /* --- IPv6 correctness tests --- */
    printf("\n--- IPv6 ---\n");
    FCT_CALL(flow6, test_struct_sizes)();
    flow6_test_init_insert_find(buckets, nb_bk, pool6, max_entries); /* IPv6-specific */
    FCT_CALL(flow6, test_find)(buckets, nb_bk, pool6, max_entries);
    FCT_CALL(flow6, test_remove)(buckets, nb_bk, pool6, max_entries);
    FCT_CALL(flow6, test_flush)(buckets, nb_bk, pool6, max_entries);
    FCT_CALL(flow6, test_expire)(buckets, nb_bk, pool6, max_entries);
    FCT_CALL(flow6, test_batch_lookup)(buckets, nb_bk, pool6, max_entries);
    printf("\nALL IPv6 TESTS PASSED\n");

    /* --- Unified correctness tests --- */
    printf("\n--- Unified ---\n");
    FCT_CALL(flowu, test_struct_sizes)();
    FCT_CALL(flowu, test_find)(buckets, nb_bk, poolu, max_entries);
    FCT_CALL(flowu, test_remove)(buckets, nb_bk, poolu, max_entries);
    FCT_CALL(flowu, test_flush)(buckets, nb_bk, poolu, max_entries);
    FCT_CALL(flowu, test_expire)(buckets, nb_bk, poolu, max_entries);
    FCT_CALL(flowu, test_batch_lookup)(buckets, nb_bk, poolu, max_entries);
    flowu_test_mixed(buckets, nb_bk, poolu, max_entries); /* Unified-specific */
    printf("\nALL Unified TESTS PASSED\n");

    /* --- IPv4 benchmarks --- */
    printf("\n========== IPv4 benchmarks ==========\n");
    FCT_CALL(flow4, bench_insert)(buckets, nb_bk, pool4, max_entries);
    FCT_CALL(flow4, bench_lookup)(buckets, nb_bk, pool4, max_entries, repeat);
    flow4_bench_single_find(buckets, nb_bk, pool4, max_entries, repeat); /* IPv4-specific */
    FCT_CALL(flow4, bench_expire)(buckets, nb_bk, pool4, max_entries);
    FCT_CALL(flow4, bench_pkt_loop)(buckets, nb_bk, pool4, max_entries, repeat);
    flow4_bench_bk0_rate(buckets, nb_bk, pool4, max_entries); /* IPv4-specific */

    /* --- IPv6 benchmarks --- */
    printf("\n========== IPv6 benchmarks ==========\n");
    FCT_CALL(flow6, bench_insert)(buckets, nb_bk, pool6, max_entries);
    FCT_CALL(flow6, bench_lookup)(buckets, nb_bk, pool6, max_entries, repeat);
    FCT_CALL(flow6, bench_expire)(buckets, nb_bk, pool6, max_entries);
    FCT_CALL(flow6, bench_pkt_loop)(buckets, nb_bk, pool6, max_entries, repeat);

    /* --- Unified benchmarks --- */
    printf("\n========== Unified benchmarks ==========\n");
    FCT_CALL(flowu, bench_insert)(buckets, nb_bk, poolu, max_entries);
    FCT_CALL(flowu, bench_lookup)(buckets, nb_bk, poolu, max_entries, repeat);
    FCT_CALL(flowu, bench_expire)(buckets, nb_bk, poolu, max_entries);
    FCT_CALL(flowu, bench_pkt_loop)(buckets, nb_bk, poolu, max_entries, repeat);

    printf("\n");

    munmap(buckets, bk_size);
    munmap(pool4, pool4_size);
    munmap(pool6, pool6_size);
    munmap(poolu, poolu_size);
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
