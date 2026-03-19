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

#include "../../samples/fcache/include/flow4_cache.h"
#include "../../samples/fcache/include/flow6_cache.h"
#include "../../samples/fcache/include/flowu_cache.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

/*===========================================================================
 * Key makers
 *===========================================================================*/
static struct fc_flow4_key
make_key4(unsigned i)
{
    struct fc_flow4_key k;

    memset(&k, 0, sizeof(k));
    k.src_ip = 0x0a000001u + i;
    k.dst_ip = 0x0a100001u + i;
    k.src_port = (uint16_t)(1000u + i);
    k.dst_port = (uint16_t)(2000u + i);
    k.proto = 17u;
    k.vrfid = 1u;
    return k;
}

static struct fc_flow6_key
make_key6(unsigned i)
{
    struct fc_flow6_key k;
    uint32_t v;

    memset(&k, 0, sizeof(k));
    k.src_ip[0] = 0x20; k.src_ip[1] = 0x01;
    k.src_ip[2] = 0x0d; k.src_ip[3] = 0xb8;
    v = i;
    memcpy(k.src_ip + 12, &v, 4);
    k.dst_ip[0] = 0x20; k.dst_ip[1] = 0x01;
    k.dst_ip[2] = 0x0d; k.dst_ip[3] = 0xb9;
    v = i + 0x1000u;
    memcpy(k.dst_ip + 12, &v, 4);
    k.src_port = (uint16_t)(1000u + i);
    k.dst_port = (uint16_t)(2000u + i);
    k.proto = 6u;
    k.vrfid = 1u;
    return k;
}

static struct fc_flowu_key
make_keyu_v4(unsigned i)
{
    return fc_flowu_key_v4(
        0x0a000001u + i, 0x0a100001u + i,
        (uint16_t)(1000u + i), (uint16_t)(2000u + i),
        17u, 1u);
}

static struct fc_flowu_key
make_keyu_v6(unsigned i)
{
    uint8_t src[16] = {0x20, 0x01, 0x0d, 0xb8};
    uint8_t dst[16] = {0x20, 0x01, 0x0d, 0xb9};
    uint32_t v = i;

    memcpy(src + 12, &v, 4);
    v = i + 0x1000u;
    memcpy(dst + 12, &v, 4);
    return fc_flowu_key_v6(
        src, dst,
        (uint16_t)(1000u + i), (uint16_t)(2000u + i),
        6u, 1u);
}

/*===========================================================================
 * Macro-templated test body
 *
 * PREFIX   : flow4, flow6, flowu
 * KEY_T    : struct fc_flow4_key, etc.
 * RESULT_T : struct fc_flow4_result, etc.
 * ENTRY_T  : struct fc_flow4_entry, etc.
 * CACHE_T  : struct fc_flow4_cache, etc.
 * CONFIG_T : struct fc_flow4_config, etc.
 * STATS_T  : struct fc_flow4_stats, etc.
 * MAKE_KEY : make_key4, make_key6, make_keyu_v4
 *===========================================================================*/
#define COUNT_HITS(RESULT_T, results, n) \
    ({ unsigned _hits = 0u; \
       for (unsigned _i = 0; _i < (n); _i++) \
           if ((results)[_i].entry_idx != 0u) _hits++; \
       _hits; })

#define DEFINE_TESTS(PREFIX, KEY_T, RESULT_T, ENTRY_T, CACHE_T, CONFIG_T, STATS_T, MAKE_KEY) \
\
static void \
test_##PREFIX##_lookup_fill_remove(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_KEYS = 8u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
    uint16_t miss_idx[NB_KEYS]; \
    unsigned miss_count; \
\
    printf("[T] fc " #PREFIX " lookup/fill/remove\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(i); \
\
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, keys, NB_KEYS, 1u, \
                                                   results, miss_idx); \
    if (miss_count != NB_KEYS) \
        FAILF("initial miss_count=%u expected %u", miss_count, NB_KEYS); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx != 0u) \
            FAILF("initial result[%u].entry_idx=%u", i, results[i].entry_idx); \
        if (miss_idx[i] != i) \
            FAILF("miss_idx[%u]=%u expected %u", i, miss_idx[i], i); \
    } \
\
    if (fc_##PREFIX##_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count, \
                                             10u, results) != NB_KEYS) \
        FAIL("fill_miss_batch inserted count mismatch"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != NB_KEYS) \
        FAILF("nb_entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), NB_KEYS); \
\
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, keys, NB_KEYS, 20u, \
                                                   results, miss_idx); \
    if (miss_count != 0u) \
        FAILF("post-fill miss_count=%u expected 0", miss_count); \
    for (unsigned i = 0; i < NB_KEYS; i++) { \
        if (results[i].entry_idx == 0u) \
            FAILF("result[%u] entry_idx is 0", i); \
    } \
\
    if (!fc_##PREFIX##_cache_remove_idx(&fc, results[3].entry_idx)) \
        FAIL("remove_idx failed"); \
    if (fc_##PREFIX##_cache_remove_idx(&fc, results[3].entry_idx)) \
        FAIL("remove_idx should fail on removed entry"); \
\
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, &keys[3], 1u, 30u, \
                                                   &results[3], miss_idx); \
    if (miss_count != 1u || results[3].entry_idx != 0u) \
        FAIL("removed key should miss"); \
} \
\
static void \
test_##PREFIX##_pressure_relief(void) \
{ \
    enum { NB_BK = 4u, MAX_ENTRIES = 32u, FILL = 32u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = 10u, .pressure_empty_slots = 15u }; \
    KEY_T keys[FILL]; \
    RESULT_T results[FILL]; \
    uint16_t miss_idx[FILL]; \
    KEY_T newcomer; \
    RESULT_T newcomer_result; \
    uint16_t newcomer_miss_idx; \
    unsigned miss_count; \
\
    printf("[T] fc " #PREFIX " pressure relief on fill_miss\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(i); \
\
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, keys, FILL, 100u, \
                                                   results, miss_idx); \
    if (miss_count != FILL) \
        FAILF("pressure test initial miss_count=%u expected %u", miss_count, FILL); \
    if (fc_##PREFIX##_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count, \
                                             100u, results) != FILL) \
        FAIL("pressure test initial fill failed"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != FILL) \
        FAILF("pressure test initial fill entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), FILL); \
\
    newcomer = MAKE_KEY(1000u); \
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, &newcomer, 1u, 1000u, \
                                                   &newcomer_result, \
                                                   &newcomer_miss_idx); \
    if (miss_count != 1u || newcomer_result.entry_idx != 0u) \
        FAIL("newcomer should miss before fill"); \
\
    if (fc_##PREFIX##_cache_fill_miss_batch(&fc, &newcomer, &newcomer_miss_idx, 1u, \
                                             1000u, &newcomer_result) != 1u) \
        FAIL("pressure relief did not free one slot for newcomer"); \
    if (newcomer_result.entry_idx == 0u) \
        FAIL("newcomer_result.entry_idx should be non-zero"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != FILL) \
        FAILF("pressure test final entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), FILL); \
\
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, &newcomer, 1u, 1001u, \
                                                   &newcomer_result, \
                                                   &newcomer_miss_idx); \
    if (miss_count != 0u || newcomer_result.entry_idx == 0u) \
        FAIL("newcomer should hit after pressure-relief fill"); \
\
    (void)fc_##PREFIX##_cache_lookup_batch(&fc, keys, FILL, 1002u, results, miss_idx); \
    if (COUNT_HITS(RESULT_T, results, FILL) != FILL - 1u) \
        FAILF("expected exactly one old entry to be evicted, hits=%u", \
              COUNT_HITS(RESULT_T, results, FILL)); \
} \
\
static void \
test_##PREFIX##_fill_miss_full_without_relief(void) \
{ \
    enum { NB_BK = 4u, MAX_ENTRIES = 16u, FILL = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = UINT64_C(1) << 40, .pressure_empty_slots = 15u }; \
    KEY_T keys[FILL]; \
    RESULT_T results[FILL]; \
    uint16_t miss_idx[FILL]; \
    KEY_T newcomer; \
    RESULT_T newcomer_result; \
    uint16_t newcomer_miss_idx; \
    unsigned miss_count; \
\
    printf("[T] fc " #PREFIX " no relief when entries are fresh\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(2000u + i); \
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, keys, FILL, 100u, \
                                                   results, miss_idx); \
    if (miss_count != FILL) \
        FAIL("fresh-fill initial miss count mismatch"); \
    if (fc_##PREFIX##_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count, \
                                             100u, results) != FILL) \
        FAIL("fresh-fill initial insert failed"); \
\
    newcomer = MAKE_KEY(3000u); \
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, &newcomer, 1u, 101u, \
                                                   &newcomer_result, \
                                                   &newcomer_miss_idx); \
    if (miss_count != 1u) \
        FAIL("fresh-fill newcomer miss expected"); \
    if (fc_##PREFIX##_cache_fill_miss_batch(&fc, &newcomer, &newcomer_miss_idx, 1u, \
                                             101u, &newcomer_result) != 0u) \
        FAIL("fresh-fill should not insert newcomer without relief"); \
    if (newcomer_result.entry_idx != 0u) \
        FAILF("fresh-fill newcomer_result.entry_idx=%u expected 0", \
              newcomer_result.entry_idx); \
} \
\
static void \
test_##PREFIX##_duplicate_miss_batch(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u, NB_KEYS = 2u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
    uint16_t miss_idx[NB_KEYS]; \
    unsigned miss_count; \
\
    printf("[T] fc " #PREFIX " duplicate miss batch\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    keys[0] = MAKE_KEY(4000u); \
    keys[1] = keys[0]; \
\
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, keys, NB_KEYS, 1u, \
                                                   results, miss_idx); \
    if (miss_count != NB_KEYS) \
        FAILF("duplicate batch initial miss_count=%u expected %u", \
              miss_count, NB_KEYS); \
    if (fc_##PREFIX##_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count, \
                                             10u, results) != 1u) \
        FAIL("duplicate batch should insert exactly one entry"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 1u) \
        FAILF("duplicate batch nb_entries=%u expected 1", \
              fc_##PREFIX##_cache_nb_entries(&fc)); \
    if (results[0].entry_idx == 0u || results[1].entry_idx == 0u) \
        FAIL("duplicate batch entry_idx should be non-zero"); \
    if (results[0].entry_idx != results[1].entry_idx) \
        FAILF("duplicate batch entry mismatch %u vs %u", \
              results[0].entry_idx, results[1].entry_idx); \
} \
\
static void \
test_##PREFIX##_flush_and_invalid_remove(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u, NB_KEYS = 4u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    KEY_T keys[NB_KEYS]; \
    RESULT_T results[NB_KEYS]; \
    uint16_t miss_idx[NB_KEYS]; \
    unsigned miss_count; \
\
    printf("[T] fc " #PREFIX " flush and invalid remove\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        keys[i] = MAKE_KEY(5000u + i); \
\
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, keys, NB_KEYS, 1u, \
                                                   results, miss_idx); \
    if (miss_count != NB_KEYS) \
        FAIL("flush test initial miss count mismatch"); \
    if (fc_##PREFIX##_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count, \
                                             10u, results) != NB_KEYS) \
        FAIL("flush test initial fill failed"); \
\
    if (fc_##PREFIX##_cache_remove_idx(&fc, 0u)) \
        FAIL("remove_idx(0) should fail"); \
    if (fc_##PREFIX##_cache_remove_idx(&fc, MAX_ENTRIES + 1u)) \
        FAIL("remove_idx(out-of-range) should fail"); \
\
    fc_##PREFIX##_cache_flush(&fc); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAILF("flush left nb_entries=%u expected 0", \
              fc_##PREFIX##_cache_nb_entries(&fc)); \
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, keys, NB_KEYS, 20u, \
                                                   results, miss_idx); \
    if (miss_count != NB_KEYS) \
        FAILF("post-flush miss_count=%u expected %u", miss_count, NB_KEYS); \
    for (unsigned i = 0; i < NB_KEYS; i++) \
        if (results[i].entry_idx != 0u) \
            FAILF("post-flush result[%u].entry_idx=%u expected 0", \
                  i, results[i].entry_idx); \
} \
\
static void \
test_##PREFIX##_maintenance(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, FILL = 24u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = 10u, .pressure_empty_slots = 1u }; \
    KEY_T keys[FILL]; \
    RESULT_T results[FILL]; \
    uint16_t miss_idx[FILL]; \
    STATS_T stats; \
    unsigned miss_count; \
    unsigned before_entries; \
    unsigned evicted; \
\
    printf("[T] fc " #PREFIX " maintenance bucket count\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(6000u + i); \
\
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, keys, FILL, 100u, \
                                                   results, miss_idx); \
    if (miss_count != FILL) \
        FAIL("maintenance initial miss count mismatch"); \
    if (fc_##PREFIX##_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count, \
                                             100u, results) != FILL) \
        FAIL("maintenance initial fill failed"); \
\
    before_entries = fc_##PREFIX##_cache_nb_entries(&fc); \
    evicted = fc_##PREFIX##_cache_maintain(&fc, 0u, NB_BK, 1000u); \
    if (evicted == 0u) \
        FAIL("maintenance should evict at least one expired entry"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != before_entries - evicted) \
        FAILF("maintenance entries=%u expected %u", \
              fc_##PREFIX##_cache_nb_entries(&fc), before_entries - evicted); \
\
    fc_##PREFIX##_cache_stats(&fc, &stats); \
    if (stats.maint_calls != 1u) \
        FAILF("maint_calls=%" PRIu64 " expected 1", stats.maint_calls); \
    if (stats.maint_bucket_checks != NB_BK) \
        FAILF("maint_bucket_checks=%" PRIu64 " expected %u", \
              stats.maint_bucket_checks, NB_BK); \
    if (stats.maint_evictions != evicted) \
        FAILF("maint_evictions=%" PRIu64 " expected %u", \
              stats.maint_evictions, evicted); \
} \
\
static void \
test_##PREFIX##_timeout_boundary(void) \
{ \
    enum { NB_BK = 8u, MAX_ENTRIES = 16u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = 100u, .pressure_empty_slots = 1u }; \
    KEY_T keys[2]; \
    RESULT_T results[2]; \
    uint16_t miss_idx[2]; \
\
    printf("[T] fc " #PREFIX " timeout boundary\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    keys[0] = MAKE_KEY(7000u); \
    keys[1] = MAKE_KEY(7001u); \
\
    fc_##PREFIX##_cache_lookup_batch(&fc, keys, 2u, 1000u, results, miss_idx); \
    fc_##PREFIX##_cache_fill_miss_batch(&fc, keys, miss_idx, 2u, 1000u, results); \
\
    if (fc_##PREFIX##_cache_maintain(&fc, 0u, NB_BK, 1099u) != 0u) \
        FAIL("should not evict before timeout boundary"); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 2u) \
        FAIL("entries should remain at timeout boundary - 1"); \
\
    if (fc_##PREFIX##_cache_maintain(&fc, 0u, NB_BK, 1101u) == 0u) \
        FAIL("should evict after timeout boundary"); \
} \
\
static void \
test_##PREFIX##_maintain_step(void) \
{ \
    enum { NB_BK = 16u, MAX_ENTRIES = 64u, FILL = 48u }; \
    struct rix_hash_bucket_s buckets[NB_BK]; \
    ENTRY_T pool[MAX_ENTRIES]; \
    CACHE_T fc; \
    CONFIG_T cfg = { .timeout_tsc = 100u, .pressure_empty_slots = 1u }; \
    KEY_T keys[FILL]; \
    RESULT_T results[FILL]; \
    uint16_t miss_idx[FILL]; \
    STATS_T stats; \
    unsigned miss_count; \
    unsigned total_evicted = 0u; \
\
    printf("[T] fc " #PREFIX " maintain_step\n"); \
    fc_##PREFIX##_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg); \
    for (unsigned i = 0; i < FILL; i++) \
        keys[i] = MAKE_KEY(8000u + i); \
\
    /* Fill entries at ts=100 */ \
    miss_count = fc_##PREFIX##_cache_lookup_batch(&fc, keys, FILL, 100u, \
                                                   results, miss_idx); \
    if (miss_count != FILL) \
        FAIL("maintain_step initial miss count mismatch"); \
    if (fc_##PREFIX##_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count, \
                                             100u, results) != FILL) \
        FAIL("maintain_step initial fill failed"); \
\
    /* Before timeout: 8 step calls should evict nothing */ \
    for (unsigned s = 0; s < 8u; s++) { \
        unsigned ev = fc_##PREFIX##_cache_maintain_step(&fc, 199u); \
        if (ev != 0u) \
            FAILF("maintain_step pre-timeout step %u evicted %u", s, ev); \
    } \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != FILL) \
        FAIL("maintain_step pre-timeout should not evict"); \
\
    /* After timeout: one full sweep (8 steps) evicts most entries. */ \
    /* Sparse buckets (used_slots <= pressure_empty_slots) are */ \
    /* intentionally skipped — maintain_step is for busy caches; */ \
    /* use maintain() for full cleanup. */ \
    for (unsigned s = 0; s < 8u; s++) \
        total_evicted += fc_##PREFIX##_cache_maintain_step(&fc, 1000u); \
    if (total_evicted == 0u) \
        FAIL("maintain_step should evict expired entries"); \
    unsigned remaining = fc_##PREFIX##_cache_nb_entries(&fc); \
    if (total_evicted + remaining != FILL) \
        FAILF("maintain_step: evicted=%u + remaining=%u != %u", \
              total_evicted, remaining, (unsigned)FILL); \
    /* Should evict the majority */ \
    if (total_evicted < FILL / 2u) \
        FAILF("maintain_step: evicted=%u too few (fill=%u)", \
              total_evicted, (unsigned)FILL); \
\
    /* Stats: 8 pre-timeout + 8 post-timeout = 16 step calls */ \
    fc_##PREFIX##_cache_stats(&fc, &stats); \
    if (stats.maint_step_calls != 16u) \
        FAILF("maint_step_calls=%" PRIu64 " expected 16", \
              stats.maint_step_calls); \
    /* SIMD skip should have kicked in for sparse buckets */ \
    if (remaining > 0u && stats.maint_step_skipped_bks == 0u) \
        FAIL("maintain_step should skip sparse buckets"); \
\
    /* Full cleanup via maintain() for remaining entries */ \
    fc_##PREFIX##_cache_maintain(&fc, 0u, NB_BK, 1000u); \
    if (fc_##PREFIX##_cache_nb_entries(&fc) != 0u) \
        FAIL("maintain() should clean up remaining entries"); \
}

/*===========================================================================
 * Instantiate tests for all three variants
 *===========================================================================*/
DEFINE_TESTS(flow4, struct fc_flow4_key, struct fc_flow4_result,
             struct fc_flow4_entry, struct fc_flow4_cache,
             struct fc_flow4_config, struct fc_flow4_stats,
             make_key4)

DEFINE_TESTS(flow6, struct fc_flow6_key, struct fc_flow6_result,
             struct fc_flow6_entry, struct fc_flow6_cache,
             struct fc_flow6_config, struct fc_flow6_stats,
             make_key6)

DEFINE_TESTS(flowu, struct fc_flowu_key, struct fc_flowu_result,
             struct fc_flowu_entry, struct fc_flowu_cache,
             struct fc_flowu_config, struct fc_flowu_stats,
             make_keyu_v4)

/*===========================================================================
 * flowu-specific: v4/v6 coexistence
 *===========================================================================*/
static void
test_flowu_v4_v6_coexist(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_V4 = 4u, NB_V6 = 4u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc_flowu_entry pool[MAX_ENTRIES];
    struct fc_flowu_cache fc;
    struct fc_flowu_key keys[NB_V4 + NB_V6];
    struct fc_flowu_result results[NB_V4 + NB_V6];
    uint16_t miss_idx[NB_V4 + NB_V6];
    unsigned miss_count;
    unsigned total = NB_V4 + NB_V6;

    printf("[T] fc flowu v4/v6 coexistence\n");
    fc_flowu_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL);

    for (unsigned i = 0; i < NB_V4; i++)
        keys[i] = make_keyu_v4(i);
    for (unsigned i = 0; i < NB_V6; i++)
        keys[NB_V4 + i] = make_keyu_v6(i);

    /* All miss initially */
    miss_count = fc_flowu_cache_lookup_batch(&fc, keys, total, 1u,
                                               results, miss_idx);
    if (miss_count != total)
        FAILF("coexist initial miss_count=%u expected %u", miss_count, total);

    /* Fill all */
    if (fc_flowu_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count,
                                         10u, results) != total)
        FAIL("coexist fill failed");

    /* All hit */
    miss_count = fc_flowu_cache_lookup_batch(&fc, keys, total, 20u,
                                               results, miss_idx);
    if (miss_count != 0u)
        FAILF("coexist post-fill miss_count=%u expected 0", miss_count);

    /* Verify v4 and v6 got distinct entries */
    for (unsigned i = 0; i < total; i++) {
        if (results[i].entry_idx == 0u)
            FAILF("coexist result[%u] entry_idx is 0", i);
        for (unsigned j = i + 1; j < total; j++) {
            if (results[i].entry_idx == results[j].entry_idx)
                FAILF("coexist result[%u] == result[%u] = %u",
                      i, j, results[i].entry_idx);
        }
    }

    /* Remove a v4 entry; v6 entries should still hit */
    if (!fc_flowu_cache_remove_idx(&fc, results[0].entry_idx))
        FAIL("coexist remove v4 entry failed");
    miss_count = fc_flowu_cache_lookup_batch(&fc, &keys[NB_V4], NB_V6, 30u,
                                               &results[NB_V4], miss_idx);
    if (miss_count != 0u)
        FAIL("coexist v6 entries should still hit after v4 remove");
}

/*===========================================================================
 * Run all tests
 *===========================================================================*/
#define RUN_TESTS(PREFIX) \
    test_##PREFIX##_lookup_fill_remove(); \
    test_##PREFIX##_pressure_relief(); \
    test_##PREFIX##_fill_miss_full_without_relief(); \
    test_##PREFIX##_duplicate_miss_batch(); \
    test_##PREFIX##_flush_and_invalid_remove(); \
    test_##PREFIX##_maintenance(); \
    test_##PREFIX##_timeout_boundary(); \
    test_##PREFIX##_maintain_step()

int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    RUN_TESTS(flow4);
    RUN_TESTS(flow6);
    RUN_TESTS(flowu);
    test_flowu_v4_v6_coexist();

    printf("ALL FCACHE TESTS PASSED (flow4 + flow6 + flowu)\n");
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
