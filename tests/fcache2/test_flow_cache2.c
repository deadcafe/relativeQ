/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../samples/fcache2/include/flow4_cache2.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

static struct fc2_flow4_key
make_key(unsigned i)
{
    struct fc2_flow4_key k;

    memset(&k, 0, sizeof(k));
    k.src_ip = 0x0a000001u + i;
    k.dst_ip = 0x0a100001u + i;
    k.src_port = (uint16_t)(1000u + i);
    k.dst_port = (uint16_t)(2000u + i);
    k.proto = 17u;
    k.vrfid = 1u;
    return k;
}

static unsigned
count_hits(const struct fc2_flow4_result *results, unsigned n)
{
    unsigned hits = 0u;

    for (unsigned i = 0; i < n; i++)
        if (results[i].entry_idx != 0u)
            hits++;
    return hits;
}

static void
test_lookup_fill_remove(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_KEYS = 8u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc2_flow4_entry pool[MAX_ENTRIES];
    struct fc2_flow4_cache fc;
    struct fc2_flow4_key keys[NB_KEYS];
    struct fc2_flow4_result results[NB_KEYS];
    uint16_t miss_idx[NB_KEYS];
    unsigned miss_count;

    printf("[T] fc2 lookup/fill/remove\n");
    fc2_flow4_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL);
    for (unsigned i = 0; i < NB_KEYS; i++)
        keys[i] = make_key(i);

    miss_count = fc2_flow4_cache_lookup_batch(&fc, keys, NB_KEYS, 1u,
                                              results, miss_idx);
    if (miss_count != NB_KEYS)
        FAILF("initial miss_count=%u expected %u", miss_count, NB_KEYS);
    for (unsigned i = 0; i < NB_KEYS; i++) {
        if (results[i].entry_idx != 0u)
            FAILF("initial result[%u].entry_idx=%u", i, results[i].entry_idx);
        if (miss_idx[i] != i)
            FAILF("miss_idx[%u]=%u expected %u", i, miss_idx[i], i);
    }

    if (fc2_flow4_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count,
                                        10u, results) != NB_KEYS)
        FAIL("fill_miss_batch inserted count mismatch");
    if (fc2_flow4_cache_nb_entries(&fc) != NB_KEYS)
        FAILF("nb_entries=%u expected %u",
              fc2_flow4_cache_nb_entries(&fc), NB_KEYS);

    miss_count = fc2_flow4_cache_lookup_batch(&fc, keys, NB_KEYS, 20u,
                                              results, miss_idx);
    if (miss_count != 0u)
        FAILF("post-fill miss_count=%u expected 0", miss_count);
    for (unsigned i = 0; i < NB_KEYS; i++) {
        if (results[i].entry_idx == 0u)
            FAILF("result[%u] entry_idx is 0", i);
    }

    if (!fc2_flow4_cache_remove_idx(&fc, results[3].entry_idx))
        FAIL("remove_idx failed");
    if (fc2_flow4_cache_remove_idx(&fc, results[3].entry_idx))
        FAIL("remove_idx should fail on removed entry");

    miss_count = fc2_flow4_cache_lookup_batch(&fc, &keys[3], 1u, 30u,
                                              &results[3], miss_idx);
    if (miss_count != 1u || results[3].entry_idx != 0u)
        FAIL("removed key should miss");
}

static void
test_pressure_relief_on_fill_miss(void)
{
    enum { NB_BK = 4u, MAX_ENTRIES = 32u, FILL = 32u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc2_flow4_entry pool[MAX_ENTRIES];
    struct fc2_flow4_cache fc;
    struct fc2_flow4_config cfg = {
        .timeout_tsc = 10u,
        .pressure_empty_slots = 15u
    };
    struct fc2_flow4_key keys[FILL];
    struct fc2_flow4_result results[FILL];
    uint16_t miss_idx[FILL];
    struct fc2_flow4_key newcomer;
    struct fc2_flow4_result newcomer_result;
    uint16_t newcomer_miss_idx;
    unsigned miss_count;

    printf("[T] fc2 pressure relief on fill_miss\n");
    fc2_flow4_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg);
    for (unsigned i = 0; i < FILL; i++)
        keys[i] = make_key(i);

    miss_count = fc2_flow4_cache_lookup_batch(&fc, keys, FILL, 100u,
                                              results, miss_idx);
    if (miss_count != FILL)
        FAILF("pressure test initial miss_count=%u expected %u", miss_count, FILL);
    if (fc2_flow4_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count,
                                        100u, results) != FILL)
        FAIL("pressure test initial fill failed");
    if (fc2_flow4_cache_nb_entries(&fc) != FILL)
        FAILF("pressure test initial fill entries=%u expected %u",
              fc2_flow4_cache_nb_entries(&fc), FILL);

    newcomer = make_key(1000u);
    miss_count = fc2_flow4_cache_lookup_batch(&fc, &newcomer, 1u, 1000u,
                                              &newcomer_result,
                                              &newcomer_miss_idx);
    if (miss_count != 1u || newcomer_result.entry_idx != 0u)
        FAIL("newcomer should miss before fill");

    if (fc2_flow4_cache_fill_miss_batch(&fc, &newcomer, &newcomer_miss_idx, 1u,
                                        1000u,
                                        &newcomer_result) != 1u)
        FAIL("pressure relief did not free one slot for newcomer");
    if (newcomer_result.entry_idx == 0u)
        FAIL("newcomer_result.entry_idx should be non-zero");
    if (fc2_flow4_cache_nb_entries(&fc) != FILL)
        FAILF("pressure test final entries=%u expected %u",
              fc2_flow4_cache_nb_entries(&fc), FILL);

    miss_count = fc2_flow4_cache_lookup_batch(&fc, &newcomer, 1u, 1001u,
                                              &newcomer_result,
                                              &newcomer_miss_idx);
    if (miss_count != 0u || newcomer_result.entry_idx == 0u)
        FAIL("newcomer should hit after pressure-relief fill");

    (void)fc2_flow4_cache_lookup_batch(&fc, keys, FILL, 1002u, results, miss_idx);
    if (count_hits(results, FILL) != FILL - 1u)
        FAILF("expected exactly one old entry to be evicted, hits=%u",
              count_hits(results, FILL));
}

static void
test_fill_miss_full_without_relief(void)
{
    enum { NB_BK = 4u, MAX_ENTRIES = 16u, FILL = 16u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc2_flow4_entry pool[MAX_ENTRIES];
    struct fc2_flow4_cache fc;
    struct fc2_flow4_config cfg = {
        .timeout_tsc = UINT64_C(1) << 40,
        .pressure_empty_slots = 15u
    };
    struct fc2_flow4_key keys[FILL];
    struct fc2_flow4_result results[FILL];
    uint16_t miss_idx[FILL];
    struct fc2_flow4_key newcomer;
    struct fc2_flow4_result newcomer_result;
    uint16_t newcomer_miss_idx;
    unsigned miss_count;

    printf("[T] fc2 no relief when entries are fresh\n");
    fc2_flow4_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg);
    for (unsigned i = 0; i < FILL; i++)
        keys[i] = make_key(2000u + i);
    miss_count = fc2_flow4_cache_lookup_batch(&fc, keys, FILL, 100u,
                                              results, miss_idx);
    if (miss_count != FILL)
        FAIL("fresh-fill initial miss count mismatch");
    if (fc2_flow4_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count,
                                        100u, results) != FILL)
        FAIL("fresh-fill initial insert failed");

    newcomer = make_key(3000u);
    miss_count = fc2_flow4_cache_lookup_batch(&fc, &newcomer, 1u, 101u,
                                              &newcomer_result,
                                              &newcomer_miss_idx);
    if (miss_count != 1u)
        FAIL("fresh-fill newcomer miss expected");
    if (fc2_flow4_cache_fill_miss_batch(&fc, &newcomer, &newcomer_miss_idx, 1u,
                                        101u,
                                        &newcomer_result) != 0u)
        FAIL("fresh-fill should not insert newcomer without relief");
    if (newcomer_result.entry_idx != 0u)
        FAILF("fresh-fill newcomer_result.entry_idx=%u expected 0",
              newcomer_result.entry_idx);
}

static void
test_duplicate_miss_batch(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 16u, NB_KEYS = 2u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc2_flow4_entry pool[MAX_ENTRIES];
    struct fc2_flow4_cache fc;
    struct fc2_flow4_key keys[NB_KEYS];
    struct fc2_flow4_result results[NB_KEYS];
    uint16_t miss_idx[NB_KEYS];
    unsigned miss_count;

    printf("[T] fc2 duplicate miss batch\n");
    fc2_flow4_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL);
    keys[0] = make_key(4000u);
    keys[1] = keys[0];

    miss_count = fc2_flow4_cache_lookup_batch(&fc, keys, NB_KEYS, 1u,
                                              results, miss_idx);
    if (miss_count != NB_KEYS)
        FAILF("duplicate batch initial miss_count=%u expected %u",
              miss_count, NB_KEYS);
    if (fc2_flow4_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count,
                                        10u, results) != 1u)
        FAIL("duplicate batch should insert exactly one entry");
    if (fc2_flow4_cache_nb_entries(&fc) != 1u)
        FAILF("duplicate batch nb_entries=%u expected 1",
              fc2_flow4_cache_nb_entries(&fc));
    if (results[0].entry_idx == 0u || results[1].entry_idx == 0u)
        FAIL("duplicate batch entry_idx should be non-zero");
    if (results[0].entry_idx != results[1].entry_idx)
        FAILF("duplicate batch entry mismatch %u vs %u",
              results[0].entry_idx, results[1].entry_idx);
}

static void
test_flush_and_invalid_remove(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 16u, NB_KEYS = 4u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc2_flow4_entry pool[MAX_ENTRIES];
    struct fc2_flow4_cache fc;
    struct fc2_flow4_key keys[NB_KEYS];
    struct fc2_flow4_result results[NB_KEYS];
    uint16_t miss_idx[NB_KEYS];
    unsigned miss_count;

    printf("[T] fc2 flush and invalid remove\n");
    fc2_flow4_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL);
    for (unsigned i = 0; i < NB_KEYS; i++)
        keys[i] = make_key(5000u + i);

    miss_count = fc2_flow4_cache_lookup_batch(&fc, keys, NB_KEYS, 1u,
                                              results, miss_idx);
    if (miss_count != NB_KEYS)
        FAIL("flush test initial miss count mismatch");
    if (fc2_flow4_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count,
                                        10u, results) != NB_KEYS)
        FAIL("flush test initial fill failed");

    if (fc2_flow4_cache_remove_idx(&fc, 0u))
        FAIL("remove_idx(0) should fail");
    if (fc2_flow4_cache_remove_idx(&fc, MAX_ENTRIES + 1u))
        FAIL("remove_idx(out-of-range) should fail");

    fc2_flow4_cache_flush(&fc);
    if (fc2_flow4_cache_nb_entries(&fc) != 0u)
        FAILF("flush left nb_entries=%u expected 0",
              fc2_flow4_cache_nb_entries(&fc));
    miss_count = fc2_flow4_cache_lookup_batch(&fc, keys, NB_KEYS, 20u,
                                              results, miss_idx);
    if (miss_count != NB_KEYS)
        FAILF("post-flush miss_count=%u expected %u", miss_count, NB_KEYS);
    for (unsigned i = 0; i < NB_KEYS; i++)
        if (results[i].entry_idx != 0u)
            FAILF("post-flush result[%u].entry_idx=%u expected 0",
                  i, results[i].entry_idx);
}

static void
test_maintenance_bucket_count(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, FILL = 24u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc2_flow4_entry pool[MAX_ENTRIES];
    struct fc2_flow4_cache fc;
    struct fc2_flow4_config cfg = {
        .timeout_tsc = 10u,
        .pressure_empty_slots = 1u
    };
    struct fc2_flow4_key keys[FILL];
    struct fc2_flow4_result results[FILL];
    uint16_t miss_idx[FILL];
    struct fc2_flow4_stats stats;
    unsigned miss_count;
    unsigned before_entries;
    unsigned evicted;

    printf("[T] fc2 maintenance bucket count\n");
    fc2_flow4_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg);
    for (unsigned i = 0; i < FILL; i++)
        keys[i] = make_key(6000u + i);

    miss_count = fc2_flow4_cache_lookup_batch(&fc, keys, FILL, 100u,
                                              results, miss_idx);
    if (miss_count != FILL)
        FAIL("maintenance initial miss count mismatch");
    if (fc2_flow4_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count,
                                        100u, results) != FILL)
        FAIL("maintenance initial fill failed");

    before_entries = fc2_flow4_cache_nb_entries(&fc);
    evicted = fc2_flow4_cache_maintain(&fc, 0u, NB_BK, 1000u);
    if (evicted == 0u)
        FAIL("maintenance should evict at least one expired entry");
    if (fc2_flow4_cache_nb_entries(&fc) != before_entries - evicted)
        FAILF("maintenance entries=%u expected %u",
              fc2_flow4_cache_nb_entries(&fc), before_entries - evicted);

    fc2_flow4_cache_stats(&fc, &stats);
    if (stats.maint_calls != 1u)
        FAILF("maint_calls=%" PRIu64 " expected 1", stats.maint_calls);
    if (stats.maint_bucket_checks != NB_BK)
        FAILF("maint_bucket_checks=%" PRIu64 " expected %u",
              stats.maint_bucket_checks, NB_BK);
    if (stats.maint_evictions != evicted)
        FAILF("maint_evictions=%" PRIu64 " expected %u",
              stats.maint_evictions, evicted);
}

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
    test_lookup_fill_remove();
    test_pressure_relief_on_fill_miss();
    test_fill_miss_full_without_relief();
    test_duplicate_miss_batch();
    test_flush_and_invalid_remove();
    test_maintenance_bucket_count();
    printf("ALL FCACHE2 TESTS PASSED\n");
    return 0;
}
