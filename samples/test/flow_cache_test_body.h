/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_cache_test_body.h - Macro-templated test & benchmark functions.
 *
 * Before including this file, define:
 *   FCT_PREFIX        e.g. flow4           (for function naming)
 *   FCT_KEY           e.g. flow4_key
 *   FCT_ENTRY         e.g. flow4_entry
 *   FCT_CACHE         e.g. flow4_cache
 *   FCT_LABEL         e.g. "IPv4"
 *   FCT_KEY_BYTES     e.g. 20
 *   FCT_MAKE_RANDOM_KEY(kp)  - macro or function to fill *kp randomly
 *
 * Also requires: tsc_start/end, xorshift64, alloc_aligned,
 *                now_sec, BENCH_BATCH, BENCH_WARMUP, flow_cache_rdtsc
 */

/* token-paste helpers */
#define _FCT_CAT(a, b, c)   a ## b ## c
#define FCT_CAT(a, b, c)    _FCT_CAT(a, b, c)
#define FCT_FN(prefix, suffix) FCT_CAT(prefix, _, suffix)

#ifndef FLOW_CACHE_TEST_BACKEND_DECLARED
#define FLOW_CACHE_TEST_BACKEND_DECLARED
extern flow_cache_backend_t flow_cache_test_backend;
#endif

#ifndef FLOW_CACHE_TEST_NOINLINE
#if defined(__GNUC__) || defined(__clang__)
#define FLOW_CACHE_TEST_NOINLINE __attribute__((noinline))
#else
#define FLOW_CACHE_TEST_NOINLINE
#endif
#endif

#ifndef FLOW_CACHE_TEST_RESULT_TYPES_DEFINED
#define FLOW_CACHE_TEST_RESULT_TYPES_DEFINED
struct flow_cache_pkt_bench_result {
    double lookup_insert_cy_per_pkt;
    double expire_cy_per_call;
    double expire_cy_per_pkt;
    unsigned total_pkts;
    unsigned total_hits;
    unsigned total_misses;
    unsigned total_inserts;
    unsigned total_expire_calls;
    uint64_t total_evictions;
    unsigned final_entries;
    unsigned total_slots;
};
#endif

/*===========================================================================
 * Test: structure sizes and alignment
 *===========================================================================*/
static void
FCT_FN(FCT_PREFIX,test_struct_sizes)(void)
{
    printf("[T-%s] struct sizes\n", FCT_LABEL);
    printf("    key:   %zu bytes\n", sizeof(struct FCT_KEY));
    printf("    entry: %zu bytes (align %zu)\n",
           sizeof(struct FCT_ENTRY),
           __alignof__(struct FCT_ENTRY));
    printf("    cache: %zu bytes\n", sizeof(struct FCT_CACHE));

    assert(sizeof(struct FCT_KEY) == FCT_KEY_BYTES);
    assert(sizeof(struct FCT_ENTRY) == 128);
    assert(__alignof__(struct FCT_ENTRY) == 64);
    printf("    PASS\n");
}

/*===========================================================================
 * Test: find (non-pipelined single lookup)
 *===========================================================================*/
static void
FCT_FN(FCT_PREFIX,test_find)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                   struct FCT_ENTRY *pool, unsigned max_entries)
{
    printf("[T-%s] find\n", FCT_LABEL);

    struct FCT_CACHE fc;
    uint64_t now = 1000000;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 0,
                                    NULL, NULL, NULL);

    struct FCT_KEY k1;
    FCT_MAKE_RANDOM_KEY(&k1);

    /* miss before insert */
    assert(FC_CALL(FCT_PREFIX, cache_find)(&fc, &k1) == NULL);

    /* insert, then find */
    struct FCT_ENTRY *e1 = FC_CALL(FCT_PREFIX, cache_insert)(&fc, &k1, now);
    assert(e1 != NULL);
    assert(FC_CALL(FCT_PREFIX, cache_find)(&fc, &k1) == e1);

    /* different key - must miss */
    struct FCT_KEY k2;
    FCT_MAKE_RANDOM_KEY(&k2);
    assert(FC_CALL(FCT_PREFIX, cache_find)(&fc, &k2) == NULL);

    printf("    PASS\n");
}

/*===========================================================================
 * Test: explicit remove
 *===========================================================================*/
static void
FCT_FN(FCT_PREFIX,test_remove)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                     struct FCT_ENTRY *pool, unsigned max_entries)
{
    printf("[T-%s] remove\n", FCT_LABEL);

    struct FCT_CACHE fc;
    uint64_t now = 1000000;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 0,
                                    NULL, NULL, NULL);

    struct FCT_KEY k1;
    FCT_MAKE_RANDOM_KEY(&k1);

    struct FCT_ENTRY *e1 = FC_CALL(FCT_PREFIX, cache_insert)(&fc, &k1, now);
    assert(e1 != NULL);
    assert(FC_CALL(FCT_PREFIX, cache_nb_entries)(&fc) == 1);

    FC_CALL(FCT_PREFIX, cache_remove)(&fc, e1);
    assert(FC_CALL(FCT_PREFIX, cache_nb_entries)(&fc) == 0);

    /* verify gone via both lookup paths */
    struct FCT_ENTRY *results[1];
    FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc, &k1, 1, results);
    assert(results[0] == NULL);
    assert(FC_CALL(FCT_PREFIX, cache_find)(&fc, &k1) == NULL);

    /* stats: remove counted separately from evictions */
    struct flow_cache_stats st;
    FC_CALL(FCT_PREFIX, cache_stats)(&fc, &st);
    assert(st.inserts   == 1);
    assert(st.removes   == 1);
    assert(st.evictions == 0);

    /* re-insert into the freed slot */
    struct FCT_ENTRY *e2 = FC_CALL(FCT_PREFIX, cache_insert)(&fc, &k1, now);
    assert(e2 != NULL);
    assert(FC_CALL(FCT_PREFIX, cache_nb_entries)(&fc) == 1);

    printf("    PASS\n");
}

/*===========================================================================
 * Test: flush (bulk remove)
 *===========================================================================*/
static void
FCT_FN(FCT_PREFIX,test_flush)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                    struct FCT_ENTRY *pool, unsigned max_entries)
{
    printf("[T-%s] flush\n", FCT_LABEL);

    struct FCT_CACHE fc;
    uint64_t now = 1000000;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 0,
                                    NULL, NULL, NULL);

    unsigned n = (max_entries < 64u) ? max_entries / 2u : 32u;
    struct FCT_KEY *keys = malloc(n * sizeof(*keys));
    assert(keys);
    for (unsigned i = 0; i < n; i++) {
        FCT_MAKE_RANDOM_KEY(&keys[i]);
        struct FCT_ENTRY *e = FC_CALL(FCT_PREFIX, cache_insert)(&fc, &keys[i], now);
        assert(e != NULL);
    }
    assert(FC_CALL(FCT_PREFIX, cache_nb_entries)(&fc) == n);

    FC_CALL(FCT_PREFIX, cache_flush)(&fc);
    assert(FC_CALL(FCT_PREFIX, cache_nb_entries)(&fc) == 0);

    /* verify all entries gone */
    struct FCT_ENTRY **results = malloc(n * sizeof(*results));
    assert(results);
    FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc, keys, n, results);
    for (unsigned i = 0; i < n; i++)
        assert(results[i] == NULL);

    /* removes counter reflects flushed count */
    struct flow_cache_stats st;
    FC_CALL(FCT_PREFIX, cache_stats)(&fc, &st);
    assert(st.removes == (uint64_t)n);

    /* cache fully functional after flush */
    for (unsigned i = 0; i < n; i++) {
        struct FCT_ENTRY *e = FC_CALL(FCT_PREFIX, cache_insert)(&fc, &keys[i], now);
        assert(e != NULL);
    }
    assert(FC_CALL(FCT_PREFIX, cache_nb_entries)(&fc) == n);

    free(results);
    free(keys);
    printf("    PASS\n");
}

/*===========================================================================
 * Test: expire
 *===========================================================================*/
static void
FCT_FN(FCT_PREFIX,test_expire)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                     struct FCT_ENTRY *pool, unsigned max_entries)
{
    printf("[T-%s] expire\n", FCT_LABEL);

    struct FCT_CACHE fc;
    struct FCT_KEY k1;
    FCT_MAKE_RANDOM_KEY(&k1);

    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 1 /* 1 ms */,
                                    NULL, NULL, NULL);

    uint64_t now = flow_cache_rdtsc();
    struct FCT_ENTRY *e1 = FC_CALL(FCT_PREFIX, cache_insert)(&fc, &k1, now);
    assert(e1 != NULL);

    /* expire with entry still alive - should not evict */
    for (unsigned i = 0; i < max_entries / FLOW_CACHE_EXPIRE_SCAN_MIN + 1; i++)
        FC_CALL(FCT_PREFIX, cache_expire)(&fc, now);
    struct FCT_ENTRY *results[1];
    FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc, &k1, 1, results);
    assert(results[0] == e1);

    /* wait for timeout, then expire - should evict */
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&delay, NULL);
    uint64_t later = flow_cache_rdtsc();

    for (unsigned i = 0; i < max_entries / FLOW_CACHE_EXPIRE_SCAN_MIN + 1; i++)
        FC_CALL(FCT_PREFIX, cache_expire)(&fc, later);
    FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc, &k1, 1, results);
    assert(results[0] == NULL);

    struct flow_cache_stats st;
    FC_CALL(FCT_PREFIX, cache_stats)(&fc, &st);
    assert(st.inserts == 1);
    assert(st.evictions == 1);

    printf("    PASS\n");
}

/*===========================================================================
 * Test: batch lookup
 *===========================================================================*/
static void
FCT_FN(FCT_PREFIX,test_batch_lookup)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                           struct FCT_ENTRY *pool, unsigned max_entries)
{
    printf("[T-%s] batch lookup\n", FCT_LABEL);

    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 0,
                                    NULL, NULL, NULL);

    unsigned n = (max_entries < 128) ? max_entries / 2 : 128;
    struct FCT_KEY *keys = malloc(n * sizeof(*keys));
    struct FCT_ENTRY **entries = malloc(n * sizeof(*entries));
    assert(keys && entries);

    for (unsigned i = 0; i < n; i++) {
        FCT_MAKE_RANDOM_KEY(&keys[i]);
        entries[i] = FC_CALL(FCT_PREFIX, cache_insert)(&fc, &keys[i], 1000);
        assert(entries[i] != NULL);
    }

    struct FCT_ENTRY **results = malloc(n * sizeof(*results));
    assert(results);
    FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc, keys, n, results);
    for (unsigned i = 0; i < n; i++)
        assert(results[i] == entries[i]);

    free(keys);
    free(entries);
    free(results);
    printf("    PASS\n");
}

/*===========================================================================
 * Fill cache with N random keys
 *===========================================================================*/
static struct FCT_KEY *
FCT_FN(FCT_PREFIX,fill_cache)(struct FCT_CACHE *fc, unsigned count, unsigned *actual_out)
{
    struct FCT_KEY *keys = malloc((size_t)count * sizeof(*keys));
    assert(keys);
    unsigned actual = 0;
    for (unsigned i = 0; i < count; i++) {
        FCT_MAKE_RANDOM_KEY(&keys[i]);
        struct FCT_ENTRY *e = FC_CALL(FCT_PREFIX, cache_insert)(fc, &keys[i], 1000);
        if (e)
            actual++;
    }
    *actual_out = actual;
    return keys;
}

static double
FCT_FN(FCT_PREFIX,measure_insert)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                        struct FCT_ENTRY *pool, unsigned max_entries,
                        unsigned *inserted_out)
{
    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 0,
                                    NULL, NULL, NULL);

    struct FCT_KEY *keys = malloc((size_t)max_entries * sizeof(*keys));
    assert(keys);
    for (unsigned i = 0; i < max_entries; i++)
        FCT_MAKE_RANDOM_KEY(&keys[i]);

    uint64_t t0 = tsc_start();
    unsigned inserted = 0;
    for (unsigned i = 0; i < max_entries; i++) {
        struct FCT_ENTRY *e =
            FC_CALL(FCT_PREFIX, cache_insert)(&fc, &keys[i], 1000);
        if (e)
            inserted++;
    }
    uint64_t t1 = tsc_end();

    free(keys);
    *inserted_out = inserted;
    return (inserted > 0) ? (double)(t1 - t0) / inserted : 0.0;
}

static double
FCT_FN(FCT_PREFIX,measure_lookup_hit_rate)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                                 struct FCT_ENTRY *pool, unsigned max_entries,
                                 unsigned repeat, unsigned hit_pct)
{
    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 0,
                                    NULL, NULL, NULL);

    unsigned fill_target = max_entries * 3 / 4;
    unsigned actual_fill;
    struct FCT_KEY *hit_keys =
        FCT_FN(FCT_PREFIX, fill_cache)(&fc, fill_target, &actual_fill);
    assert(actual_fill >= BENCH_BATCH);

    struct FCT_ENTRY **results = malloc(BENCH_BATCH * sizeof(*results));
    size_t pool_keys = (size_t)repeat * BENCH_BATCH;
    struct FCT_KEY *miss_pool = malloc(pool_keys * sizeof(*miss_pool));
    struct FCT_KEY *hit_pool  = malloc(pool_keys * sizeof(*hit_pool));
    struct FCT_KEY *all_batches = malloc(pool_keys * sizeof(*all_batches));
    assert(results && miss_pool && hit_pool && all_batches);

    for (size_t i = 0; i < pool_keys; i++) {
        FCT_MAKE_RANDOM_KEY(&miss_pool[i]);
        hit_pool[i] = hit_keys[xorshift64() % actual_fill];
    }

    unsigned nb_hit = BENCH_BATCH * hit_pct / 100;
    for (unsigned r = 0; r < repeat; r++) {
        struct FCT_KEY *bp = all_batches + (size_t)r * BENCH_BATCH;
        const struct FCT_KEY *hp = hit_pool + (size_t)r * BENCH_BATCH;
        const struct FCT_KEY *mp = miss_pool + (size_t)r * BENCH_BATCH;
        for (unsigned i = 0; i < nb_hit; i++)
            bp[i] = hp[i];
        for (unsigned i = nb_hit; i < BENCH_BATCH; i++)
            bp[i] = mp[i - nb_hit];
    }

    unsigned warmup = flow_cache_test_warmup;
    if (warmup > repeat)
        warmup = repeat;
    for (unsigned r = 0; r < warmup; r++) {
        FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc,
                            all_batches + (size_t)r * BENCH_BATCH,
                            BENCH_BATCH, results);
    }

    uint64_t total_cy = 0;
    for (unsigned r = 0; r < repeat; r++) {
        const struct FCT_KEY *bp = all_batches + (size_t)r * BENCH_BATCH;
        uint64_t t0 = tsc_start();
        FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc, bp, BENCH_BATCH, results);
        uint64_t t1 = tsc_end();
        total_cy += t1 - t0;
    }

    free(all_batches);
    free(hit_pool);
    free(miss_pool);
    free(results);
    free(hit_keys);

    return (double)total_cy / (double)((uint64_t)repeat * BENCH_BATCH);
}

static double
FCT_FN(FCT_PREFIX,measure_single_find)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                             struct FCT_ENTRY *pool, unsigned max_entries,
                             unsigned repeat, int hit)
{
    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 0,
                                    NULL, NULL, NULL);

    unsigned actual_fill;
    struct FCT_KEY *hit_keys =
        FCT_FN(FCT_PREFIX, fill_cache)(&fc, max_entries * 3 / 4, &actual_fill);
    assert(actual_fill > 0);

    unsigned nkeys = repeat * 4;
    if (nkeys > 200000)
        nkeys = 200000;

    struct FCT_KEY *keys = malloc((size_t)nkeys * sizeof(*keys));
    assert(keys);
    for (unsigned i = 0; i < nkeys; i++) {
        if (hit)
            keys[i] = hit_keys[xorshift64() % actual_fill];
        else
            FCT_MAKE_RANDOM_KEY(&keys[i]);
    }

    uint64_t total_cy = 0;
    for (unsigned i = 0; i < nkeys; i++) {
        uint64_t t0 = tsc_start();
        struct FCT_ENTRY *e __attribute__((unused)) =
            FC_CALL(FCT_PREFIX, cache_find)(&fc, &keys[i]);
        uint64_t t1 = tsc_end();
        total_cy += t1 - t0;
    }

    free(keys);
    free(hit_keys);
    return (double)total_cy / nkeys;
}

static double
FCT_FN(FCT_PREFIX,measure_expire_full)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                             struct FCT_ENTRY *pool, unsigned max_entries,
                             uint64_t *evicted_out)
{
    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 1,
                                    NULL, NULL, NULL);

    unsigned filled;
    struct FCT_KEY *keys =
        FCT_FN(FCT_PREFIX, fill_cache)(&fc, max_entries, &filled);
    (void)filled;

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&delay, NULL);
    uint64_t later = flow_cache_rdtsc();

    uint64_t t0 = tsc_start();
    FC_CALL(FCT_PREFIX, cache_expire)(&fc, later);
    uint64_t t1 = tsc_end();

    struct flow_cache_stats st;
    FC_CALL(FCT_PREFIX, cache_stats)(&fc, &st);
    free(keys);

    *evicted_out = st.evictions;
    return (st.evictions > 0) ? (double)(t1 - t0) / (double)st.evictions : 0.0;
}

static void
FCT_FN(FCT_PREFIX,measure_pkt_loop)(struct rix_hash_bucket_s *buckets, unsigned nb_bk_use,
                          struct FCT_ENTRY *pool, unsigned max_entries,
                          unsigned repeat, struct flow_cache_pkt_bench_result *out)
{
    unsigned total_slots = nb_bk_use * RIX_HASH_BUCKET_ENTRY_SZ;
    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk_use, pool, max_entries,
                                    flow_cache_test_backend, 1,
                                    NULL, NULL, NULL);

    unsigned prefill = total_slots - (total_slots >> 2) - BENCH_BATCH;
    if (prefill > max_entries)
        prefill = max_entries * 70 / 100;
    unsigned filled;
    struct FCT_KEY *prefill_keys =
        FCT_FN(FCT_PREFIX, fill_cache)(&fc, prefill, &filled);

    struct timespec age_delay = { .tv_sec = 0, .tv_nsec = 5000000 };
    nanosleep(&age_delay, NULL);

    size_t total_keys = (size_t)repeat * BENCH_BATCH;
    struct FCT_KEY *pkt_keys = malloc(total_keys * sizeof(*pkt_keys));
    struct FCT_ENTRY **results = malloc(BENCH_BATCH * sizeof(*results));
    assert(pkt_keys && results);

    unsigned nb_hit_per_batch = BENCH_BATCH * 90 / 100;
    unsigned active_flows = filled / 2;
    if (active_flows < 1)
        active_flows = 1;
    for (unsigned r = 0; r < repeat; r++) {
        struct FCT_KEY *bp = pkt_keys + (size_t)r * BENCH_BATCH;
        for (unsigned i = 0; i < nb_hit_per_batch; i++)
            bp[i] = prefill_keys[xorshift64() % active_flows];
        for (unsigned i = nb_hit_per_batch; i < BENCH_BATCH; i++)
            FCT_MAKE_RANDOM_KEY(&bp[i]);
    }
    free(prefill_keys);

    uint64_t total_lookup_cy = 0;
    uint64_t total_expire_cy = 0;
    unsigned total_hits = 0, total_misses = 0, total_inserts = 0;
    unsigned total_expire_calls = 0;

    for (unsigned r = 0; r < repeat; r++) {
        const struct FCT_KEY *batch_keys =
            pkt_keys + (size_t)r * BENCH_BATCH;
        uint64_t now = flow_cache_rdtsc();

        uint64_t t0 = tsc_start();
        FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc, batch_keys, BENCH_BATCH, results);

        unsigned batch_misses = 0;
        for (unsigned i = 0; i < BENCH_BATCH; i++) {
            if (results[i] != NULL) {
                FC_CALL(FCT_PREFIX, cache_touch)(results[i], now);
                TP(results[i])->packets++;
                TP(results[i])->bytes += 64;
                total_hits++;
            } else {
                struct FCT_ENTRY *e =
                    FC_CALL(FCT_PREFIX, cache_insert)(&fc, &batch_keys[i], now);
                if (e)
                    total_inserts++;
                total_misses++;
                batch_misses++;
            }
        }
        uint64_t t1 = tsc_end();
        total_lookup_cy += t1 - t0;

        FC_CALL(FCT_PREFIX, cache_adjust_timeout)(&fc, batch_misses);

        uint64_t te0 = tsc_start();
        FC_CALL(FCT_PREFIX, cache_expire)(&fc, now);
        uint64_t te1 = tsc_end();
        total_expire_cy += te1 - te0;
        total_expire_calls++;
    }

    struct flow_cache_stats st;
    FC_CALL(FCT_PREFIX, cache_stats)(&fc, &st);

    out->total_pkts = repeat * BENCH_BATCH;
    out->total_hits = total_hits;
    out->total_misses = total_misses;
    out->total_inserts = total_inserts;
    out->total_expire_calls = total_expire_calls;
    out->total_evictions = st.evictions;
    out->final_entries = st.nb_entries;
    out->total_slots = total_slots;
    out->lookup_insert_cy_per_pkt = (double)total_lookup_cy / out->total_pkts;
    out->expire_cy_per_call = (double)total_expire_cy / total_expire_calls;
    out->expire_cy_per_pkt = (double)total_expire_cy / out->total_pkts;

    free(pkt_keys);
    free(results);
}

static FLOW_CACHE_TEST_NOINLINE void
FCT_FN(FCT_PREFIX,perf_insert_case)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                          struct FCT_ENTRY *pool, unsigned max_entries,
                          unsigned repeat __attribute__((unused)))
{
    unsigned inserted = 0;
    double cy = FCT_FN(FCT_PREFIX, measure_insert)(buckets, nb_bk, pool,
                                                   max_entries, &inserted);
    flow_cache_test_emit_result(FCT_VARIANT, "insert",
                                "cycles/insert", cy, max_entries, nb_bk,
                                repeat, flow_cache_test_backend,
                                flow_cache_test_selected_backend(FCT_VARIANT, buckets,
                                                                 nb_bk, pool,
                                                                 max_entries));
}

static FLOW_CACHE_TEST_NOINLINE void
FCT_FN(FCT_PREFIX,perf_lookup_hit80_case)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                                struct FCT_ENTRY *pool, unsigned max_entries,
                                unsigned repeat)
{
    double cy = FCT_FN(FCT_PREFIX, measure_lookup_hit_rate)(buckets, nb_bk, pool,
                                                            max_entries, repeat,
                                                            80);
    flow_cache_test_emit_result(FCT_VARIANT, "lookup_hit80",
                                "cycles/key", cy, max_entries, nb_bk,
                                repeat, flow_cache_test_backend,
                                flow_cache_test_selected_backend(FCT_VARIANT, buckets,
                                                                 nb_bk, pool,
                                                                 max_entries));
}

static FLOW_CACHE_TEST_NOINLINE void
FCT_FN(FCT_PREFIX,perf_find_single_hit_case)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                                   struct FCT_ENTRY *pool, unsigned max_entries,
                                   unsigned repeat)
{
    double cy = FCT_FN(FCT_PREFIX, measure_single_find)(buckets, nb_bk, pool,
                                                        max_entries, repeat, 1);
    flow_cache_test_emit_result(FCT_VARIANT, "find_single_hit",
                                "cycles/key", cy, max_entries, nb_bk,
                                repeat, flow_cache_test_backend,
                                flow_cache_test_selected_backend(FCT_VARIANT, buckets,
                                                                 nb_bk, pool,
                                                                 max_entries));
}

static FLOW_CACHE_TEST_NOINLINE void
FCT_FN(FCT_PREFIX,perf_find_single_miss_case)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                                    struct FCT_ENTRY *pool, unsigned max_entries,
                                    unsigned repeat)
{
    double cy = FCT_FN(FCT_PREFIX, measure_single_find)(buckets, nb_bk, pool,
                                                        max_entries, repeat, 0);
    flow_cache_test_emit_result(FCT_VARIANT, "find_single_miss",
                                "cycles/key", cy, max_entries, nb_bk,
                                repeat, flow_cache_test_backend,
                                flow_cache_test_selected_backend(FCT_VARIANT, buckets,
                                                                 nb_bk, pool,
                                                                 max_entries));
}

static FLOW_CACHE_TEST_NOINLINE void
FCT_FN(FCT_PREFIX,perf_expire_full_case)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                               struct FCT_ENTRY *pool, unsigned max_entries,
                               unsigned repeat __attribute__((unused)))
{
    uint64_t evicted = 0;
    double cy = FCT_FN(FCT_PREFIX, measure_expire_full)(buckets, nb_bk, pool,
                                                        max_entries, &evicted);
    (void)evicted;
    flow_cache_test_emit_result(FCT_VARIANT, "expire_full",
                                "cycles/eviction", cy, max_entries, nb_bk,
                                repeat, flow_cache_test_backend,
                                flow_cache_test_selected_backend(FCT_VARIANT, buckets,
                                                                 nb_bk, pool,
                                                                 max_entries));
}

static FLOW_CACHE_TEST_NOINLINE void
FCT_FN(FCT_PREFIX,perf_pkt_std_case)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                           struct FCT_ENTRY *pool, unsigned max_entries,
                           unsigned repeat)
{
    struct flow_cache_pkt_bench_result result;
    FCT_FN(FCT_PREFIX, measure_pkt_loop)(buckets, nb_bk, pool, max_entries,
                                         repeat, &result);
    flow_cache_test_emit_pkt_result(FCT_VARIANT, "pkt_std",
                                    max_entries, nb_bk, repeat,
                                    flow_cache_test_backend,
                                    flow_cache_test_selected_backend(FCT_VARIANT, buckets,
                                                                     nb_bk, pool,
                                                                     max_entries),
                                    &result);
}

static FLOW_CACHE_TEST_NOINLINE void
FCT_FN(FCT_PREFIX,perf_pkt_tight_case)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                             struct FCT_ENTRY *pool, unsigned max_entries,
                             unsigned repeat)
{
    (void)nb_bk;
    unsigned nb_bk_tight = rix_hash_nb_bk_hint(max_entries / 2u);
    while (nb_bk_tight > 1 &&
           max_entries < ((uint64_t)nb_bk_tight * RIX_HASH_BUCKET_ENTRY_SZ * 3 >> 2))
        nb_bk_tight >>= 1;

    struct flow_cache_pkt_bench_result result;
    FCT_FN(FCT_PREFIX, measure_pkt_loop)(buckets, nb_bk_tight, pool, max_entries,
                                         repeat, &result);
    flow_cache_test_emit_pkt_result(FCT_VARIANT, "pkt_tight",
                                    max_entries, nb_bk_tight, repeat,
                                    flow_cache_test_backend,
                                    flow_cache_test_selected_backend(FCT_VARIANT, buckets,
                                                                     nb_bk_tight, pool,
                                                                     max_entries),
                                    &result);
}

/*===========================================================================
 * Bench: insert throughput
 *===========================================================================*/
static void
FCT_FN(FCT_PREFIX,bench_insert)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                      struct FCT_ENTRY *pool, unsigned max_entries)
{
    printf("\n[B-%s] insert (max_entries=%u, nb_bk=%u)\n",
           FCT_LABEL, max_entries, nb_bk);

    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 0,
                                    NULL, NULL, NULL);

    unsigned total = max_entries;
    struct FCT_KEY *keys = malloc((size_t)total * sizeof(*keys));
    assert(keys);
    for (unsigned i = 0; i < total; i++)
        FCT_MAKE_RANDOM_KEY(&keys[i]);

    uint64_t t0 = tsc_start();
    unsigned inserted = 0;
    for (unsigned i = 0; i < total; i++) {
        struct FCT_ENTRY *e = FC_CALL(FCT_PREFIX, cache_insert)(&fc, &keys[i], 1000);
        if (e)
            inserted++;
    }
    uint64_t t1 = tsc_end();

    double cy = (inserted > 0) ? (double)(t1 - t0) / inserted : 0;
    printf("    inserted: %u / %u  (%.1f%% fill)\n",
           inserted, max_entries,
           100.0 * inserted / (nb_bk * RIX_HASH_BUCKET_ENTRY_SZ));
    printf("    %.1f cycles/insert\n", cy);

    free(keys);
}

/*===========================================================================
 * Bench: pipelined batch lookup - hit-rate sweep (DRAM-cold)
 *===========================================================================*/
static void
FCT_FN(FCT_PREFIX,bench_lookup)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                      struct FCT_ENTRY *pool, unsigned max_entries,
                      unsigned repeat)
{
    printf("\n[B-%s] pipelined batch lookup - hit-rate sweep (DRAM-cold)\n",
           FCT_LABEL);
    printf("    max_entries=%u  nb_bk=%u  batch=%u  repeat=%u\n",
           max_entries, nb_bk, BENCH_BATCH, repeat);

    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 0,
                                    NULL, NULL, NULL);

    unsigned fill_target = max_entries * 3 / 4;
    unsigned actual_fill;
    double t_fill0 = now_sec();
    struct FCT_KEY *hit_keys = FCT_FN(FCT_PREFIX,fill_cache)(&fc, fill_target,
                                                    &actual_fill);
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

    struct FCT_ENTRY **results = malloc(BENCH_BATCH * sizeof(*results));
    struct FCT_KEY *batch = malloc(BENCH_BATCH * sizeof(*batch));
    assert(results && batch);

    size_t pool_keys = (size_t)repeat * BENCH_BATCH;
    struct FCT_KEY *miss_pool = malloc(pool_keys * sizeof(*miss_pool));
    struct FCT_KEY *hit_pool  = malloc(pool_keys * sizeof(*hit_pool));
    assert(miss_pool && hit_pool);

    for (size_t i = 0; i < pool_keys; i++) {
        FCT_MAKE_RANDOM_KEY(&miss_pool[i]);
        hit_pool[i] = hit_keys[xorshift64() % actual_fill];
    }

    printf("\n    hit%%   cycles/key\n");
    printf("    ----   ----------\n");

    for (unsigned pct = 0; pct <= 100; pct += 20) {
        unsigned nb_hit = BENCH_BATCH * pct / 100;

        struct FCT_KEY *all_batches = malloc(pool_keys * sizeof(*all_batches));
        assert(all_batches);
        for (unsigned r = 0; r < repeat; r++) {
            struct FCT_KEY *bp = all_batches + (size_t)r * BENCH_BATCH;
            const struct FCT_KEY *hp = hit_pool + (size_t)r * BENCH_BATCH;
            const struct FCT_KEY *mp = miss_pool + (size_t)r * BENCH_BATCH;
            for (unsigned i = 0; i < nb_hit; i++)
                bp[i] = hp[i];
            for (unsigned i = nb_hit; i < BENCH_BATCH; i++)
                bp[i] = mp[i - nb_hit];
        }

        for (unsigned r = 0; r < BENCH_WARMUP && r < repeat; r++)
            FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc,
                                         all_batches + (size_t)r * BENCH_BATCH,
                                         BENCH_BATCH, results);

        uint64_t total_cy = 0;
        for (unsigned r = 0; r < repeat; r++) {
            const struct FCT_KEY *bp =
                all_batches + (size_t)r * BENCH_BATCH;
            uint64_t t0 = tsc_start();
            FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc, bp, BENCH_BATCH, results);
            uint64_t t1 = tsc_end();
            total_cy += t1 - t0;
        }

        double avg = (double)total_cy / (double)((uint64_t)repeat * BENCH_BATCH);
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
FCT_FN(FCT_PREFIX,bench_expire)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                      struct FCT_ENTRY *pool, unsigned max_entries)
{
    printf("\n[B-%s] expire (max_entries=%u)\n", FCT_LABEL, max_entries);

    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk, pool, max_entries,
                                    flow_cache_test_backend, 1 /* 1 ms */,
                                    NULL, NULL, NULL);

    unsigned filled;
    struct FCT_KEY *keys = FCT_FN(FCT_PREFIX,fill_cache)(&fc, max_entries, &filled);
    printf("    filled: %u entries\n", filled);

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
    nanosleep(&delay, NULL);
    uint64_t later = flow_cache_rdtsc();

    uint64_t t0 = tsc_start();
    FC_CALL(FCT_PREFIX, cache_expire)(&fc, later);
    uint64_t t1 = tsc_end();

    struct flow_cache_stats st;
    FC_CALL(FCT_PREFIX, cache_stats)(&fc, &st);

    double cy = (st.evictions > 0) ? (double)(t1 - t0) / (double)st.evictions : 0.0;
    printf("    evicted: %" PRIu64 "  %.1f cycles/eviction\n",
           st.evictions, cy);

    free(keys);
}

/*===========================================================================
 * Bench: packet processing loop - lookup + insert + expire
 *===========================================================================*/
static void
FCT_FN(FCT_PREFIX,run_pkt_loop)(struct rix_hash_bucket_s *buckets, unsigned nb_bk_use,
                      struct FCT_ENTRY *pool, unsigned max_entries,
                      unsigned repeat, const char *label)
{
    unsigned total_slots = nb_bk_use * RIX_HASH_BUCKET_ENTRY_SZ;

    printf("\n[B-%s] packet processing loop - %s\n", FCT_LABEL, label);
    printf("    max_entries=%u  nb_bk=%u  total_slots=%u  batch=%u  repeat=%u\n",
           max_entries, nb_bk_use, total_slots, BENCH_BATCH, repeat);

    struct FCT_CACHE fc;
    FC_CALL(FCT_PREFIX, cache_init)(&fc, buckets, nb_bk_use, pool, max_entries,
                                    flow_cache_test_backend, 1,
                                    NULL, NULL, NULL);

    assert(FC_CALL(FCT_PREFIX, cache_backend)(&fc) != FLOW_CACHE_BACKEND_AUTO);

    unsigned prefill = total_slots - (total_slots >> 2) - BENCH_BATCH;
    if (prefill > max_entries)
        prefill = max_entries * 70 / 100;
    unsigned filled;
    struct FCT_KEY *prefill_keys = FCT_FN(FCT_PREFIX,fill_cache)(&fc, prefill, &filled);
    printf("    pre-filled: %u entries (%.1f%% fill)\n",
           filled, 100.0 * filled / total_slots);

    struct timespec age_delay = { .tv_sec = 0, .tv_nsec = 5000000 };
    nanosleep(&age_delay, NULL);

    size_t total_keys = (size_t)repeat * BENCH_BATCH;
    struct FCT_KEY *pkt_keys = malloc(total_keys * sizeof(*pkt_keys));
    assert(pkt_keys);
    unsigned nb_hit_per_batch = BENCH_BATCH * 90 / 100;
    unsigned active_flows = filled / 2;
    if (active_flows < 1)
        active_flows = 1;
    for (unsigned r = 0; r < repeat; r++) {
        struct FCT_KEY *bp = pkt_keys + (size_t)r * BENCH_BATCH;
        for (unsigned i = 0; i < nb_hit_per_batch; i++)
            bp[i] = prefill_keys[xorshift64() % active_flows];
        for (unsigned i = nb_hit_per_batch; i < BENCH_BATCH; i++)
            FCT_MAKE_RANDOM_KEY(&bp[i]);
    }
    free(prefill_keys);

    struct FCT_ENTRY **results = malloc(BENCH_BATCH * sizeof(*results));
    assert(results);

    uint64_t total_lookup_cy = 0;
    uint64_t total_expire_cy = 0;
    unsigned total_hits = 0, total_misses = 0, total_inserts = 0;
    unsigned total_expire_calls = 0;

    for (unsigned r = 0; r < repeat; r++) {
        const struct FCT_KEY *batch_keys =
            pkt_keys + (size_t)r * BENCH_BATCH;
        uint64_t now = flow_cache_rdtsc();

        uint64_t t0 = tsc_start();
        FC_CALL(FCT_PREFIX, cache_lookup_batch)(&fc, batch_keys, BENCH_BATCH, results);

        unsigned batch_misses = 0;
        for (unsigned i = 0; i < BENCH_BATCH; i++) {
            if (results[i] != NULL) {
                FC_CALL(FCT_PREFIX, cache_touch)(results[i], now);
                TP(results[i])->packets++;  /* demonstration: update via userdata overlay */
                TP(results[i])->bytes += 64;
                total_hits++;
            } else {
                struct FCT_ENTRY *e =
                    FC_CALL(FCT_PREFIX, cache_insert)(&fc, &batch_keys[i], now);
                if (e)
                    total_inserts++;
                total_misses++;
                batch_misses++;
            }
        }
        uint64_t t1 = tsc_end();
        total_lookup_cy += t1 - t0;

        FC_CALL(FCT_PREFIX, cache_adjust_timeout)(&fc, batch_misses);

        {
            uint64_t te0 = tsc_start();
            FC_CALL(FCT_PREFIX, cache_expire)(&fc, now);
            uint64_t te1 = tsc_end();
            total_expire_cy += te1 - te0;
            total_expire_calls++;
        }
    }

    unsigned total_pkts = repeat * BENCH_BATCH;
    struct flow_cache_stats st;
    FC_CALL(FCT_PREFIX, cache_stats)(&fc, &st);

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
    printf("    expire:        %.1f cycles/call (%.1f cy/pkt amortized)\n",
           (double)total_expire_cy / total_expire_calls,
           (double)total_expire_cy / total_pkts);

    free(pkt_keys);
    free(results);
}

static void
FCT_FN(FCT_PREFIX,bench_pkt_loop)(struct rix_hash_bucket_s *buckets, unsigned nb_bk,
                        struct FCT_ENTRY *pool, unsigned max_entries,
                        unsigned repeat)
{
    FCT_FN(FCT_PREFIX,run_pkt_loop)(buckets, nb_bk, pool, max_entries, repeat,
                          "standard sizing (pool ~ 50% fill)");

    /* tight sizing: minimum nb_bk to hold max_entries at ~100% fill */
    unsigned nb_bk_tight = rix_hash_nb_bk_hint(max_entries / 2u);
    while (nb_bk_tight > 1 &&
           max_entries < ((uint64_t)nb_bk_tight * RIX_HASH_BUCKET_ENTRY_SZ * 3 >> 2))
        nb_bk_tight >>= 1;
    if (nb_bk_tight <= nb_bk)
        FCT_FN(FCT_PREFIX,run_pkt_loop)(buckets, nb_bk_tight, pool, max_entries, repeat,
                              "tight sizing (pool ~ 100% slots)");
}

/* clean up local macros */
#undef FCT_FN
#undef FCT_CAT
#undef _FCT_CAT


/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
