/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * fc_cache_generate.h - GENERATE macro for per-variant cache implementations.
 *
 * Usage in .c files:
 *
 *   #include "flow4_cache.h"
 *   #include "fc_cache_generate.h"
 *
 *   static inline int
 *   fc_flow4_cmp(const struct fc_flow4_key *a,
 *                 const struct fc_flow4_key *b) { ... }
 *   static inline union rix_hash_hash_u
 *   fc_flow4_hash_fn(const struct fc_flow4_key *key,
 *                     uint32_t mask) { ... }
 *
 *   FC_CACHE_GENERATE(flow4, FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS,
 *                      fc_flow4_hash_fn, fc_flow4_cmp)
 */

#ifndef _FC_CACHE_GENERATE_H_
#define _FC_CACHE_GENERATE_H_

/*===========================================================================
 * Parameterized token-paste helpers
 *===========================================================================*/
#define _FCG_CAT2(a, b)  a##b
#define _FCG_CAT(a, b)   _FCG_CAT2(a, b)

#define _FCG_HT(p, name)   _FCG_CAT(fc_, _FCG_CAT(p, _ht_##name))
#define _FCG_INT(p, name)  _FCG_CAT(fc_, _FCG_CAT(p, _##name))

/* Public API names: when FC_ARCH_SUFFIX is defined (e.g. -DFC_ARCH_SUFFIX=_avx2),
 * generated function names become fc_flow4_cache_lookup_batch_avx2 etc.
 * Without FC_ARCH_SUFFIX, the original names are generated (fc_flow4_cache_lookup_batch). */
#ifdef FC_ARCH_SUFFIX
#define _FCG_API(p, name)  _FCG_CAT(_FCG_CAT(fc_, _FCG_CAT(p, _cache_##name)), FC_ARCH_SUFFIX)
#else
#define _FCG_API(p, name)  _FCG_CAT(fc_, _FCG_CAT(p, _cache_##name))
#endif

#define _FCG_KEY_T(p)       struct _FCG_CAT(fc_, _FCG_CAT(p, _key))
#define _FCG_RESULT_T(p)    struct _FCG_CAT(fc_, _FCG_CAT(p, _result))
#define _FCG_ENTRY_T(p)     struct _FCG_CAT(fc_, _FCG_CAT(p, _entry))
#define _FCG_CACHE_T(p)     struct _FCG_CAT(fc_, _FCG_CAT(p, _cache))
#define _FCG_CONFIG_T(p)    struct _FCG_CAT(fc_, _FCG_CAT(p, _config))
#define _FCG_STATS_T(p)     struct _FCG_CAT(fc_, _FCG_CAT(p, _stats))

/*===========================================================================
 * AVX2 direct-bind (file scope, applied to all GENERATE expansions)
 *===========================================================================*/
#if defined(__AVX2__)
#undef _RIX_HASH_FIND_U32X16
#undef _RIX_HASH_FIND_U32X16_2
#define _RIX_HASH_FIND_U32X16(arr, val) \
    _rix_hash_find_u32x16_AVX2((arr), (val))
#define _RIX_HASH_FIND_U32X16_2(arr, val0, val1, mask0, mask1) \
    _rix_hash_find_u32x16_2_AVX2((arr), (val0), (val1), (mask0), (mask1))
#endif

/*===========================================================================
 * Pipeline geometry defaults
 *===========================================================================*/
#ifndef FLOW_CACHE_LOOKUP_STEP_KEYS
#define FLOW_CACHE_LOOKUP_STEP_KEYS   16u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_STEPS
#define FLOW_CACHE_LOOKUP_AHEAD_STEPS 8u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_KEYS
#define FLOW_CACHE_LOOKUP_AHEAD_KEYS \
    (FLOW_CACHE_LOOKUP_STEP_KEYS * FLOW_CACHE_LOOKUP_AHEAD_STEPS)
#endif

#ifndef _FC_RELIEF_STAGE_SLOTS
#define _FC_RELIEF_STAGE_SLOTS 4u
#endif

/* Maximum buckets per maintain_step filter+reclaim pass (VLA cap). */
/* 256 entries * 4B = 1 KB - safe for any stack. */
#ifndef _FC_MAINT_STEP_MAX_BKS
#define _FC_MAINT_STEP_MAX_BKS 256u
#endif

/*===========================================================================
 * Sub-macro 1: Hash-table GENERATE
 *===========================================================================*/
#define _FC_GENERATE_HT(p, hash_fn, cmp_fn)                               \
RIX_HASH_GENERATE_STATIC_SLOT_EX(                                         \
    _FCG_CAT(fc_, _FCG_CAT(p, _ht)),                                   \
    _FCG_CAT(fc_, _FCG_CAT(p, _entry)),                                \
    key, cur_hash, slot,                                                   \
    cmp_fn, hash_fn)

/*===========================================================================
 * Sub-macro 2: Internal helper functions
 *===========================================================================*/
#define _FC_GENERATE_INTERNAL(p)                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, prefetch_insert_hash)(const _FCG_CACHE_T(p) *fc,            \
                                   union rix_hash_hash_u h)                \
{                                                                          \
    unsigned bk0, bk1;                                                     \
    uint32_t fp;                                                           \
    unsigned mask = fc->ht_head.rhh_mask;                                  \
    _rix_hash_buckets(h, mask, &bk0, &bk1, &fp);                          \
    (void)fp;                                                              \
    _rix_hash_prefetch_bucket(fc->buckets + bk0);                          \
    _rix_hash_prefetch_bucket(fc->buckets + bk1);                          \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, result_set_hit)(_FCG_RESULT_T(p) *result,                   \
                             uint32_t entry_idx)                           \
{                                                                          \
    result->entry_idx = entry_idx;                                         \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, result_set_miss)(_FCG_RESULT_T(p) *result)                  \
{                                                                          \
    result->entry_idx = 0u;                                                \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, result_set_filled)(_FCG_RESULT_T(p) *result,                \
                                uint32_t entry_idx)                        \
{                                                                          \
    result->entry_idx = entry_idx;                                         \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, update_eff_timeout)(_FCG_CACHE_T(p) *fc)                    \
{                                                                          \
    unsigned live = fc->ht_head.rhh_nb;                                    \
    uint64_t max_tsc = fc->timeout_tsc;                                    \
    unsigned lo = fc->timeout_lo_entries;                                   \
    unsigned hi = fc->timeout_hi_entries;                                   \
    if (fc->total_slots == 0u || max_tsc == 0u) {                          \
        fc->eff_timeout_tsc = max_tsc;                                     \
        return;                                                            \
    }                                                                      \
    if (live <= lo) {                                                       \
        fc->eff_timeout_tsc = max_tsc;                                     \
    } else if (live >= hi) {                                               \
        fc->eff_timeout_tsc = fc->timeout_min_tsc;                         \
    } else {                                                               \
        uint64_t span_entries = (uint64_t)(hi - lo);                       \
        uint64_t used_entries = (uint64_t)(live - lo);                     \
        uint64_t span_tsc = max_tsc - fc->timeout_min_tsc;                \
        uint64_t shrink = (used_entries * span_tsc) / span_entries;        \
        fc->eff_timeout_tsc = max_tsc - shrink;                            \
    }                                                                      \
    if (fc->eff_timeout_tsc == 0u)                                         \
        fc->eff_timeout_tsc = 1u;                                          \
}                                                                          \
                                                                           \
static inline unsigned                                                     \
_FCG_INT(p, threshold64)(unsigned total_slots, unsigned parts64)          \
{                                                                          \
    return (unsigned)(((uint64_t)total_slots * parts64) >> 6);             \
}                                                                          \
                                                                           \
static void                                                                \
_FCG_INT(p, init_thresholds)(_FCG_CACHE_T(p) *fc)                       \
{                                                                          \
    fc->timeout_lo_entries =                                               \
        _FCG_INT(p, threshold64)(fc->total_slots, 38u);                   \
    fc->timeout_hi_entries =                                               \
        _FCG_INT(p, threshold64)(fc->total_slots, 48u);                   \
    fc->timeout_min_tsc = fc->timeout_tsc >> 3;                            \
    if (fc->timeout_min_tsc == 0u)                                         \
        fc->timeout_min_tsc = 1u;                                          \
    fc->relief_mid_entries =                                               \
        _FCG_INT(p, threshold64)(fc->total_slots, 45u);                   \
    fc->relief_hi_entries =                                                \
        _FCG_INT(p, threshold64)(fc->total_slots, 48u);                   \
}                                                                          \
                                                                           \
static inline unsigned                                                     \
_FCG_INT(p, relief_empty_slots)(const _FCG_CACHE_T(p) *fc)              \
{                                                                          \
    unsigned live = fc->ht_head.rhh_nb;                                    \
    unsigned empty_slots = fc->pressure_empty_slots;                       \
    if (live >= fc->relief_hi_entries) {                                    \
        if (empty_slots < 3u)                                              \
            empty_slots = 3u;                                              \
    } else if (live >= fc->relief_mid_entries) {                           \
        if (empty_slots < 2u)                                              \
            empty_slots = 2u;                                              \
    }                                                                      \
    return empty_slots;                                                    \
}                                                                          \
                                                                           \
static inline _FCG_ENTRY_T(p) *                                          \
_FCG_INT(p, alloc_entry)(_FCG_CACHE_T(p) *fc)                           \
{                                                                          \
    _FCG_ENTRY_T(p) *entry =                                             \
        RIX_SLIST_FIRST(&fc->free_head, fc->pool);                        \
    if (RIX_LIKELY(entry != NULL))                                         \
        RIX_SLIST_REMOVE_HEAD(&fc->free_head, fc->pool, free_link);       \
    return entry;                                                          \
}                                                                          \
                                                                           \
static inline void                                                         \
_FCG_INT(p, free_entry)(_FCG_CACHE_T(p) *fc,                            \
                         _FCG_ENTRY_T(p) *entry)                          \
{                                                                          \
    entry->last_ts = 0u;                                                   \
    RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool, entry, free_link);    \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, scan_bucket_slots)(_FCG_CACHE_T(p) *fc,                     \
                                unsigned bk_idx,                           \
                                uint64_t expire_before,                    \
                                unsigned *expired_slots,                   \
                                int *oldest_slot)                          \
{                                                                          \
    struct rix_hash_bucket_s *bucket = fc->buckets + bk_idx;              \
    uint64_t oldest_ts = UINT64_MAX;                                       \
    _FCG_ENTRY_T(p) *entries[RIX_HASH_BUCKET_ENTRY_SZ];                  \
    unsigned slots[RIX_HASH_BUCKET_ENTRY_SZ];                             \
    unsigned cur_base = 0u;                                                \
    unsigned cur_count;                                                    \
    unsigned used = 0u;                                                    \
    unsigned expired_count = 0u;                                           \
    unsigned next_base = 0u;                                               \
    unsigned next_count = 0u;                                              \
    int dummy_oldest_slot = -1;                                            \
    int *oldest_slotp = (oldest_slot != NULL) ?                            \
        oldest_slot : &dummy_oldest_slot;                                  \
    for (unsigned slot = 0; slot < RIX_HASH_BUCKET_ENTRY_SZ; slot++) {    \
        unsigned idx = bucket->idx[slot];                                  \
        _FCG_ENTRY_T(p) *entry;                                          \
        if (idx == (unsigned)RIX_NIL)                                      \
            continue;                                                      \
        entry = _FCG_HT(p, hptr)(fc->pool, idx);                         \
        slots[used] = slot;                                                \
        entries[used] = entry;                                             \
        used++;                                                            \
    }                                                                      \
    *oldest_slotp = -1;                                                    \
    if (RIX_UNLIKELY(used == 0u))                                          \
        return 0u;                                                         \
    cur_count = (used < _FC_RELIEF_STAGE_SLOTS) ?                         \
        used : _FC_RELIEF_STAGE_SLOTS;                                    \
    while (next_count < cur_count) {                                       \
        __builtin_prefetch(entries[next_count], 0, 3);                     \
        next_count++;                                                      \
    }                                                                      \
    next_base = cur_count;                                                 \
    while (cur_count != 0u) {                                              \
        next_count = 0u;                                                   \
        while (next_count < _FC_RELIEF_STAGE_SLOTS &&                     \
               next_base < used) {                                         \
            __builtin_prefetch(entries[next_base], 0, 3);                  \
            next_count++;                                                  \
            next_base++;                                                   \
        }                                                                  \
        for (unsigned s = 0; s < cur_count; s++) {                         \
            unsigned idx = cur_base + s;                                   \
            unsigned slot = slots[idx];                                    \
            _FCG_ENTRY_T(p) *entry = entries[idx];                        \
            if (entry->last_ts >= expire_before)                           \
                continue;                                                  \
            expired_slots[expired_count] = slot;                           \
            expired_count++;                                               \
            if (entry->last_ts < oldest_ts) {                              \
                oldest_ts = entry->last_ts;                                \
                *oldest_slotp = (int)slot;                                 \
            }                                                              \
        }                                                                  \
        cur_base += cur_count;                                             \
        cur_count = next_count;                                            \
    }                                                                      \
    return expired_count;                                                  \
}                                                                          \
                                                                           \
static int                                                                 \
_FCG_INT(p, reclaim_bucket)(_FCG_CACHE_T(p) *fc,                        \
                             unsigned bk_idx,                              \
                             uint64_t expire_before)                       \
{                                                                          \
    _FCG_ENTRY_T(p) *victim;                                              \
    unsigned removed_idx;                                                  \
    unsigned expired_slots[RIX_HASH_BUCKET_ENTRY_SZ];                     \
    unsigned expired_count;                                                \
    int victim_slot;                                                       \
    expired_count = _FCG_INT(p, scan_bucket_slots)(fc, bk_idx,            \
        expire_before, expired_slots, &victim_slot);                       \
    if (RIX_UNLIKELY(expired_count == 0u))                                 \
        return 0;                                                          \
    removed_idx = _FCG_HT(p, remove_at)(&fc->ht_head, fc->buckets,       \
                                         bk_idx, (unsigned)victim_slot);   \
    RIX_ASSERT(removed_idx != (unsigned)RIX_NIL);                          \
    victim = _FCG_HT(p, hptr)(fc->pool, removed_idx);                    \
    RIX_ASSERT(victim != NULL);                                            \
    _FCG_INT(p, free_entry)(fc, victim);                                  \
    return 1;                                                              \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, reclaim_bucket_all)(_FCG_CACHE_T(p) *fc,                    \
                                 unsigned bk_idx,                          \
                                 uint64_t expire_before)                   \
{                                                                          \
    unsigned expired_slots[RIX_HASH_BUCKET_ENTRY_SZ];                     \
    unsigned evicted;                                                      \
    evicted = _FCG_INT(p, scan_bucket_slots)(fc, bk_idx,                  \
                                              expire_before,               \
                                              expired_slots, NULL);        \
    for (unsigned i = 0; i < evicted; i++) {                               \
        _FCG_ENTRY_T(p) *victim;                                          \
        unsigned removed_idx;                                              \
        removed_idx = _FCG_HT(p, remove_at)(&fc->ht_head, fc->buckets,   \
                                             bk_idx, expired_slots[i]);    \
        RIX_ASSERT(removed_idx != (unsigned)RIX_NIL);                      \
        victim = _FCG_HT(p, hptr)(fc->pool, removed_idx);                \
        RIX_ASSERT(victim != NULL);                                        \
        _FCG_INT(p, free_entry)(fc, victim);                              \
    }                                                                      \
    return evicted;                                                        \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, maintain_grouped)(_FCG_CACHE_T(p) *fc,                   \
                                  unsigned start_bk,                       \
                                  unsigned bucket_count,                   \
                                  uint64_t now_tsc)                        \
{                                                                          \
    unsigned evicted = 0u;                                                 \
    unsigned cur_bk;                                                       \
    unsigned mask;                                                         \
    unsigned next_bk;                                                      \
    uint64_t expire_before;                                                \
    RIX_ASSERT(fc->nb_bk != 0u);                                           \
    mask = fc->ht_head.rhh_mask;                                           \
    expire_before = (now_tsc > fc->eff_timeout_tsc) ?                      \
        (now_tsc - fc->eff_timeout_tsc) : 0u;                             \
    next_bk = start_bk & mask;                                            \
    _rix_hash_prefetch_bucket_idx(&fc->buckets[next_bk]);                  \
    while (bucket_count-- != 0u) {                                         \
        unsigned reclaimed;                                                \
        cur_bk = next_bk;                                                  \
        next_bk = (next_bk + 1u) & mask;                                  \
        _rix_hash_prefetch_bucket_idx(&fc->buckets[next_bk]);              \
        fc->stats.maint_bucket_checks++;                                   \
        reclaimed = _FCG_INT(p, reclaim_bucket_all)(fc, cur_bk,           \
                                                     expire_before);       \
        fc->stats.maint_evictions += reclaimed;                            \
        evicted += reclaimed;                                              \
    }                                                                      \
    return evicted;                                                        \
}                                                                          \
                                                                           \
static inline unsigned                                                     \
_FCG_INT(p, bucket_used_slots)(const struct rix_hash_bucket_s *bucket)     \
{                                                                          \
    uint32_t zero_mask = rix_hash_arch->find_u32x16(bucket->idx, 0u);      \
    return 16u - (unsigned)__builtin_popcount(zero_mask);                   \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, maintain_step_filter_reclaim)(                                 \
    _FCG_CACHE_T(p) *fc,                                                   \
    unsigned start_bk,                                                     \
    unsigned bucket_count,                                                 \
    uint64_t expire_before,                                                \
    unsigned skip_threshold)                                               \
{                                                                          \
    enum { PF_AHEAD = 4u };                                                \
    unsigned evicted = 0u;                                                 \
    unsigned mask = fc->ht_head.rhh_mask;                                  \
    /* Pass 1 - filter: SIMD-scan idx[] to collect non-sparse buckets. */ \
    /* Touches only the idx[] cache line per bucket (sequential). */       \
    unsigned work[bucket_count];                                           \
    unsigned work_count = 0u;                                              \
    unsigned scan_bk = start_bk;                                           \
    for (unsigned i = 0; i < bucket_count; i++) {                          \
        unsigned used_slots;                                               \
        fc->stats.maint_bucket_checks++;                                   \
        used_slots = _FCG_INT(p, bucket_used_slots)(                       \
            &fc->buckets[scan_bk]);                                        \
        if (used_slots <= skip_threshold) {                                \
            fc->stats.maint_step_skipped_bks++;                            \
        } else {                                                           \
            work[work_count++] = scan_bk;                                  \
        }                                                                  \
        scan_bk = (scan_bk + 1u) & mask;                                   \
    }                                                                      \
    /* Pass 2 - reclaim: process only candidate buckets with N-ahead */   \
    /* prefetch.  Prefetch distance is stable (no skip branches). */       \
    for (unsigned j = 0; j < PF_AHEAD && j < work_count; j++)              \
        _rix_hash_prefetch_bucket(&fc->buckets[work[j]]);                  \
    for (unsigned i = 0; i < work_count; i++) {                            \
        unsigned reclaimed;                                                \
        if (i + PF_AHEAD < work_count)                                     \
            _rix_hash_prefetch_bucket(&fc->buckets[work[i + PF_AHEAD]]);   \
        reclaimed = _FCG_INT(p, reclaim_bucket_all)(fc, work[i],           \
                                                     expire_before);       \
        fc->stats.maint_evictions += reclaimed;                            \
        evicted += reclaimed;                                              \
    }                                                                      \
    return evicted;                                                        \
}                                                                          \
                                                                           \
static unsigned                                                            \
_FCG_INT(p, maintain_step_grouped)(_FCG_CACHE_T(p) *fc,                   \
                                    unsigned bucket_count,                  \
                                    uint64_t now_tsc,                       \
                                    unsigned skip_threshold)                \
{                                                                          \
    unsigned evicted = 0u;                                                 \
    unsigned mask;                                                         \
    uint64_t expire_before;                                                \
    RIX_ASSERT(fc->nb_bk != 0u);                                           \
    mask = fc->ht_head.rhh_mask;                                           \
    if (bucket_count > fc->nb_bk)                                          \
        bucket_count = fc->nb_bk;                                          \
    expire_before = (now_tsc > fc->eff_timeout_tsc) ?                      \
        (now_tsc - fc->eff_timeout_tsc) : 0u;                             \
    unsigned start_bk = fc->maint_cursor & mask;                            \
    unsigned cur_bk = start_bk;                                            \
    unsigned swept = bucket_count;                                         \
    while (bucket_count > 0u) {                                            \
        unsigned chunk = (bucket_count > _FC_MAINT_STEP_MAX_BKS) ?         \
            _FC_MAINT_STEP_MAX_BKS : bucket_count;                        \
        evicted += _FCG_INT(p, maintain_step_filter_reclaim)(              \
            fc, cur_bk, chunk, expire_before, skip_threshold);             \
        cur_bk = (cur_bk + chunk) & mask;                                  \
        bucket_count -= chunk;                                             \
    }                                                                      \
    fc->maint_cursor = cur_bk;                                             \
    fc->last_maint_start_bk = start_bk;                                   \
    fc->last_maint_sweep_bk = swept;                                       \
    return evicted;                                                        \
}                                                                          \
                                                                           \
static void                                                                \
_FCG_INT(p, insert_relief_hashed)(_FCG_CACHE_T(p) *fc,                  \
                                   union rix_hash_hash_u h,                \
                                   uint64_t now_tsc)                       \
{                                                                          \
    unsigned bk0, bk1;                                                     \
    uint32_t fp;                                                           \
    uint32_t hits_fp;                                                      \
    uint32_t hits_zero;                                                    \
    uint64_t expire_before;                                                \
    unsigned pressure_empty_slots;                                         \
    if (fc->total_slots == 0u)                                             \
        return;                                                            \
    fc->stats.relief_calls++;                                              \
    _FCG_INT(p, update_eff_timeout)(fc);                                  \
    expire_before = (now_tsc > fc->eff_timeout_tsc) ?                      \
        (now_tsc - fc->eff_timeout_tsc) : 0u;                             \
    _rix_hash_buckets(h, fc->ht_head.rhh_mask, &bk0, &bk1, &fp);         \
    pressure_empty_slots = _FCG_INT(p, relief_empty_slots)(fc);           \
    fc->stats.relief_bucket_checks++;                                      \
    rix_hash_arch->find_u32x16_2(fc->buckets[bk0].hash, fp, 0u,           \
                                 &hits_fp, &hits_zero);                    \
    (void)hits_fp;                                                         \
    if ((unsigned)__builtin_popcount(hits_zero) <=                         \
            pressure_empty_slots &&                                        \
        _FCG_INT(p, reclaim_bucket)(fc, bk0, expire_before)) {            \
        fc->stats.relief_evictions++;                                      \
        fc->stats.relief_bk0_evictions++;                                  \
        return;                                                            \
    }                                                                      \
    if (bk1 != bk0) {                                                      \
        fc->stats.relief_bucket_checks++;                                  \
        rix_hash_arch->find_u32x16_2(fc->buckets[bk1].hash, fp, 0u,       \
                                     &hits_fp, &hits_zero);                \
        if ((unsigned)__builtin_popcount(hits_zero) <=                     \
                pressure_empty_slots &&                                    \
            _FCG_INT(p, reclaim_bucket)(fc, bk1, expire_before)) {        \
            fc->stats.relief_evictions++;                                  \
            fc->stats.relief_bk1_evictions++;                              \
        }                                                                  \
    }                                                                      \
}

/*===========================================================================
 * Sub-macro 3a: Forward declarations for API functions
 *
 * When FC_ARCH_SUFFIX is defined, the suffixed function names have no
 * prototypes in any header.  Emit forward declarations to satisfy
 * -Wmissing-prototypes.
 *===========================================================================*/
#ifdef FC_ARCH_SUFFIX
#define _FC_GENERATE_API_DECLS(p)                                          \
void _FCG_API(p, init)(_FCG_CACHE_T(p) *,                                \
                        struct rix_hash_bucket_s *, unsigned,               \
                        _FCG_ENTRY_T(p) *, unsigned,                      \
                        const _FCG_CONFIG_T(p) *);                        \
void _FCG_API(p, flush)(_FCG_CACHE_T(p) *);                              \
unsigned _FCG_API(p, nb_entries)(const _FCG_CACHE_T(p) *);                \
unsigned _FCG_API(p, lookup_batch)(_FCG_CACHE_T(p) *,                     \
    const _FCG_KEY_T(p) *, unsigned, uint64_t,                             \
    _FCG_RESULT_T(p) *, uint16_t *);                                      \
unsigned _FCG_API(p, fill_miss_batch)(_FCG_CACHE_T(p) *,                  \
    const _FCG_KEY_T(p) *, const uint16_t *, unsigned, uint64_t,           \
    _FCG_RESULT_T(p) *);                                                  \
unsigned _FCG_API(p, maintain)(_FCG_CACHE_T(p) *,                         \
    unsigned, unsigned, uint64_t);                                         \
unsigned _FCG_API(p, maintain_step_ex)(_FCG_CACHE_T(p) *,                 \
    unsigned, unsigned, unsigned, uint64_t);                                \
unsigned _FCG_API(p, maintain_step)(_FCG_CACHE_T(p) *,                    \
    uint64_t, int);                                                        \
int _FCG_API(p, remove_idx)(_FCG_CACHE_T(p) *, uint32_t);                \
void _FCG_API(p, stats)(const _FCG_CACHE_T(p) *, _FCG_STATS_T(p) *);
#else
#define _FC_GENERATE_API_DECLS(p) /* prototypes in variant header */
#endif

/*===========================================================================
 * Sub-macro 3: Public API functions
 *===========================================================================*/
#define _FC_GENERATE_API(p, pressure, hash_fn)                             \
_FC_GENERATE_API_DECLS(p)                                                  \
                                                                           \
void                                                                       \
_FCG_API(p, init)(_FCG_CACHE_T(p) *fc,                                  \
                   struct rix_hash_bucket_s *buckets,                      \
                   unsigned nb_bk,                                         \
                   _FCG_ENTRY_T(p) *pool,                                 \
                   unsigned max_entries,                                    \
                   const _FCG_CONFIG_T(p) *cfg)                           \
{                                                                          \
    _FCG_CONFIG_T(p) defcfg = {                                           \
        .timeout_tsc = UINT64_C(1000000),                                  \
        .pressure_empty_slots = (pressure)                                 \
    };                                                                     \
    if (cfg == NULL)                                                        \
        cfg = &defcfg;                                                     \
    memset(fc, 0, sizeof(*fc));                                            \
    memset(buckets, 0, (size_t)nb_bk * sizeof(*buckets));                  \
    memset(pool, 0, (size_t)max_entries * sizeof(*pool));                   \
    fc->buckets = buckets;                                                 \
    fc->pool = pool;                                                       \
    fc->nb_bk = nb_bk;                                                     \
    fc->max_entries = max_entries;                                         \
    fc->total_slots = nb_bk * RIX_HASH_BUCKET_ENTRY_SZ;                   \
    fc->timeout_tsc = cfg->timeout_tsc;                                    \
    fc->eff_timeout_tsc = cfg->timeout_tsc ? cfg->timeout_tsc : 1u;        \
    fc->pressure_empty_slots = cfg->pressure_empty_slots ?                 \
        cfg->pressure_empty_slots : (pressure);                            \
    fc->maint_interval_tsc = cfg->maint_interval_tsc;                      \
    fc->maint_base_bk = cfg->maint_base_bk ? cfg->maint_base_bk : nb_bk;  \
    fc->maint_fill_threshold = cfg->maint_fill_threshold;                  \
    fc->last_maint_tsc = 0u;                                               \
    fc->last_maint_fills = 0u;                                             \
    _FCG_INT(p, init_thresholds)(fc);                                     \
    RIX_SLIST_INIT(&fc->free_head);                                        \
    _FCG_HT(p, init)(&fc->ht_head, nb_bk);                               \
    for (unsigned i = 0; i < max_entries; i++)                              \
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool,                    \
                              &fc->pool[i], free_link);                    \
}                                                                          \
                                                                           \
void                                                                       \
_FCG_API(p, flush)(_FCG_CACHE_T(p) *fc)                                 \
{                                                                          \
    memset(fc->buckets, 0, (size_t)fc->nb_bk * sizeof(*fc->buckets));      \
    RIX_SLIST_INIT(&fc->free_head);                                        \
    _FCG_HT(p, init)(&fc->ht_head, fc->nb_bk);                           \
    for (unsigned i = 0; i < fc->max_entries; i++) {                        \
        fc->pool[i].last_ts = 0u;                                         \
        RIX_SLIST_INSERT_HEAD(&fc->free_head, fc->pool,                    \
                              &fc->pool[i], free_link);                    \
    }                                                                      \
}                                                                          \
                                                                           \
unsigned                                                                   \
_FCG_API(p, nb_entries)(const _FCG_CACHE_T(p) *fc)                      \
{                                                                          \
    return fc->ht_head.rhh_nb;                                             \
}                                                                          \
                                                                           \
unsigned                                                                   \
_FCG_API(p, lookup_batch)(_FCG_CACHE_T(p) *fc,                          \
                           const _FCG_KEY_T(p) *keys,                     \
                           unsigned nb_keys,                               \
                           uint64_t now,                                   \
                           _FCG_RESULT_T(p) *results,                     \
                           uint16_t *miss_idx)                             \
{                                                                          \
    struct rix_hash_find_ctx_s ctx[nb_keys];                               \
    unsigned miss_count = 0u;                                              \
    uint64_t hit_count = 0u;                                               \
    const unsigned ahead_keys = FLOW_CACHE_LOOKUP_AHEAD_KEYS;              \
    const unsigned step_keys = FLOW_CACHE_LOOKUP_STEP_KEYS;                \
    const unsigned total = nb_keys + 3u * ahead_keys;                      \
    for (unsigned i = 0; i < total; i += step_keys) {                      \
        if (i < nb_keys) {                                                 \
            unsigned n = (i + step_keys <= nb_keys) ?                      \
                step_keys : (nb_keys - i);                                 \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, hash_key)(&ctx[i + j], &fc->ht_head,          \
                                      fc->buckets, &keys[i + j]);         \
        }                                                                  \
        if (i >= ahead_keys && i - ahead_keys < nb_keys) {                \
            unsigned base = i - ahead_keys;                                \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, scan_bk)(&ctx[base + j],                      \
                                     &fc->ht_head, fc->buckets);          \
        }                                                                  \
        if (i >= 2u * ahead_keys &&                                        \
            i - 2u * ahead_keys < nb_keys) {                               \
            unsigned base = i - 2u * ahead_keys;                           \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++)                               \
                _FCG_HT(p, prefetch_node)(&ctx[base + j], fc->pool);     \
        }                                                                  \
        if (i >= 3u * ahead_keys &&                                        \
            i - 3u * ahead_keys < nb_keys) {                               \
            unsigned base = i - 3u * ahead_keys;                           \
            unsigned n = (base + step_keys <= nb_keys) ?                   \
                step_keys : (nb_keys - base);                              \
            for (unsigned j = 0; j < n; j++) {                             \
                unsigned idx = base + j;                                   \
                _FCG_ENTRY_T(p) *entry;                                   \
                entry = _FCG_HT(p, cmp_key)(&ctx[idx], fc->pool);        \
                if (RIX_UNLIKELY(entry == NULL)) {                         \
                    _FCG_INT(p, result_set_miss)(&results[idx]);          \
                    if (miss_idx != NULL)                                   \
                        miss_idx[miss_count] = (uint16_t)idx;              \
                    miss_count++;                                          \
                    continue;                                              \
                }                                                          \
                entry->last_ts = now;                                      \
                _FCG_INT(p, result_set_hit)(&results[idx],                \
                    RIX_IDX_FROM_PTR(fc->pool, entry));                    \
                hit_count++;                                               \
            }                                                              \
        }                                                                  \
    }                                                                      \
    fc->stats.lookups += nb_keys;                                          \
    fc->stats.hits += hit_count;                                           \
    fc->stats.misses += nb_keys - hit_count;                               \
    return miss_count;                                                     \
}                                                                          \
                                                                           \
unsigned                                                                   \
_FCG_API(p, fill_miss_batch)(_FCG_CACHE_T(p) *fc,                       \
                              const _FCG_KEY_T(p) *keys,                  \
                              const uint16_t *miss_idx,                    \
                              unsigned miss_count,                         \
                              uint64_t now,                                \
                              _FCG_RESULT_T(p) *results)                  \
{                                                                          \
    unsigned inserted = 0u;                                                \
    for (unsigned base = 0; base < miss_count; base += 64u) {              \
        union rix_hash_hash_u hashes[64];                                  \
        unsigned n = miss_count - base;                                    \
        if (n > 64u)                                                       \
            n = 64u;                                                       \
        for (unsigned i = 0; i < n; i++) {                                 \
            unsigned key_idx = miss_idx[base + i];                         \
            hashes[i] = hash_fn(&keys[key_idx],                            \
                                      fc->ht_head.rhh_mask);              \
            _FCG_INT(p, prefetch_insert_hash)(fc, hashes[i]);             \
        }                                                                  \
        for (unsigned i = 0; i < n; i++) {                                 \
            unsigned key_idx = miss_idx[base + i];                         \
            _FCG_ENTRY_T(p) *entry;                                       \
            _FCG_ENTRY_T(p) *ret;                                         \
            _FCG_INT(p, insert_relief_hashed)(fc, hashes[i], now);        \
            entry = _FCG_INT(p, alloc_entry)(fc);                         \
            if (RIX_UNLIKELY(entry == NULL)) {                             \
                fc->stats.fill_full++;                                     \
                _FCG_INT(p, result_set_miss)(&results[key_idx]);          \
                continue;                                                  \
            }                                                              \
            entry->key = keys[key_idx];                                    \
            entry->last_ts = now;                                          \
            ret = _FCG_HT(p, insert_hashed)(&fc->ht_head,                \
                fc->buckets, fc->pool, entry, hashes[i]);                  \
            if (RIX_LIKELY(ret == NULL)) {                                  \
                fc->stats.fills++;                                         \
                _FCG_INT(p, result_set_filled)(&results[key_idx],         \
                    RIX_IDX_FROM_PTR(fc->pool, entry));                    \
                inserted++;                                                \
                continue;                                                  \
            }                                                              \
            _FCG_INT(p, free_entry)(fc, entry);                           \
            if (RIX_UNLIKELY(ret != entry)) {                               \
                ret->last_ts = now;                                        \
                _FCG_INT(p, result_set_filled)(&results[key_idx],         \
                    RIX_IDX_FROM_PTR(fc->pool, ret));                      \
                continue;                                                  \
            }                                                              \
            fc->stats.fill_full++;                                         \
            _FCG_INT(p, result_set_miss)(&results[key_idx]);              \
            continue;                                                      \
        }                                                                  \
    }                                                                      \
    return inserted;                                                       \
}                                                                          \
                                                                           \
unsigned                                                                   \
_FCG_API(p, maintain)(_FCG_CACHE_T(p) *fc,                              \
                       unsigned start_bk,                                  \
                       unsigned bucket_count,                              \
                       uint64_t now)                                       \
{                                                                          \
    fc->stats.maint_calls++;                                               \
    _FCG_INT(p, update_eff_timeout)(fc);                                  \
    return _FCG_INT(p, maintain_grouped)(fc, start_bk,                 \
                                            bucket_count, now);            \
}                                                                          \
                                                                           \
unsigned                                                                   \
_FCG_API(p, maintain_step_ex)(_FCG_CACHE_T(p) *fc,                       \
                               unsigned start_bk,                           \
                               unsigned bucket_count,                       \
                               unsigned skip_threshold,                     \
                               uint64_t now)                                \
{                                                                          \
    fc->stats.maint_step_calls++;                                          \
    fc->stats.maint_calls++;                                               \
    _FCG_INT(p, update_eff_timeout)(fc);                                  \
    fc->maint_cursor = start_bk & fc->ht_head.rhh_mask;                   \
    return _FCG_INT(p, maintain_step_grouped)(fc, bucket_count, now,      \
                                               skip_threshold);             \
}                                                                          \
                                                                           \
unsigned                                                                   \
_FCG_API(p, maintain_step)(_FCG_CACHE_T(p) *fc,                          \
                            uint64_t now,                                   \
                            int idle)                                       \
{                                                                          \
    unsigned sweep;                                                        \
    unsigned skip_threshold;                                               \
    fc->stats.maint_step_calls++;                                          \
    if (idle) {                                                            \
        sweep = fc->nb_bk;                                                 \
        skip_threshold = 0u;                                               \
    } else {                                                               \
        uint64_t elapsed = now - fc->last_maint_tsc;                       \
        uint64_t added   = fc->stats.fills - fc->last_maint_fills;         \
        unsigned time_scale = 0u;                                          \
        unsigned entry_scale = 0u;                                         \
        unsigned scale;                                                    \
        if (fc->maint_interval_tsc != 0u)                                  \
            time_scale = (unsigned)(elapsed / fc->maint_interval_tsc);     \
        else                                                               \
            time_scale = 1u;                                               \
        if (fc->maint_fill_threshold != 0u)                                \
            entry_scale = (unsigned)(added / fc->maint_fill_threshold);    \
        scale = (time_scale > entry_scale) ? time_scale : entry_scale;     \
        if (scale == 0u)                                                   \
            return 0u;                                                     \
        sweep = fc->maint_base_bk * scale;                                 \
        if (sweep > fc->nb_bk)                                             \
            sweep = fc->nb_bk;                                             \
        skip_threshold = fc->pressure_empty_slots;                         \
    }                                                                      \
    fc->last_maint_tsc   = now;                                            \
    fc->last_maint_fills = fc->stats.fills;                                \
    fc->stats.maint_calls++;                                               \
    _FCG_INT(p, update_eff_timeout)(fc);                                  \
    return _FCG_INT(p, maintain_step_grouped)(fc, sweep, now,             \
                                               skip_threshold);             \
}                                                                          \
                                                                           \
int                                                                        \
_FCG_API(p, remove_idx)(_FCG_CACHE_T(p) *fc, uint32_t entry_idx)        \
{                                                                          \
    _FCG_ENTRY_T(p) *entry;                                               \
    if (RIX_UNLIKELY(entry_idx == 0u || entry_idx > fc->max_entries))      \
        return 0;                                                          \
    entry = RIX_PTR_FROM_IDX(fc->pool, entry_idx);                         \
    if (entry == NULL)                                                     \
        return 0;                                                          \
    if (_FCG_HT(p, remove)(&fc->ht_head, fc->buckets,                    \
                            fc->pool, entry) == NULL)                      \
        return 0;                                                          \
    _FCG_INT(p, free_entry)(fc, entry);                                   \
    return 1;                                                              \
}                                                                          \
                                                                           \
void                                                                       \
_FCG_API(p, stats)(const _FCG_CACHE_T(p) *fc, _FCG_STATS_T(p) *out)   \
{                                                                          \
    *out = fc->stats;                                                      \
}

/*===========================================================================
 * Per-TU rix_hash_arch auto-initialization (constructor)
 *
 * Each arch-specific TU has its own static rix_hash_arch pointer.
 * Without this, rix_hash_hash_bytes_fast() falls back to Generic
 * even when compiled with -mavx2.  The constructor runs once per
 * shared-library load (or at program start for static linking).
 *===========================================================================*/
#ifdef FC_ARCH_SUFFIX
#define _FC_RIX_ARCH_CTOR(prefix)                                         \
    __attribute__((constructor)) static void                               \
    _FCG_CAT(_fc_rix_arch_init_, prefix)(void)                            \
    {                                                                      \
        rix_hash_arch_init(RIX_HASH_ARCH_AUTO);                            \
    }
#else
#define _FC_RIX_ARCH_CTOR(prefix) /* no-op without FC_ARCH_SUFFIX */
#endif

/*===========================================================================
 * Top-level GENERATE macro
 *===========================================================================*/
#define FC_CACHE_GENERATE(prefix, pressure, hash_fn, cmp_fn)              \
    _FC_RIX_ARCH_CTOR(prefix)                                             \
    _FC_GENERATE_HT(prefix, hash_fn, cmp_fn)                              \
    _FC_GENERATE_INTERNAL(prefix)                                          \
    _FC_GENERATE_API(prefix, pressure, hash_fn)

/*===========================================================================
 * Ops table instance generation (for arch-specific builds)
 *
 * Usage in each arch .c file, after FC_CACHE_GENERATE:
 *   FC_OPS_TABLE(flow4, _avx2)
 *
 * This defines a const struct fc_flow4_ops fc_flow4_ops_avx2 = { ... }
 * pointing to the suffixed function names.
 *===========================================================================*/
#ifdef FC_ARCH_SUFFIX

#define _FC_OPS_FNAME(prefix, name) \
    _FCG_CAT(_FCG_CAT(fc_, _FCG_CAT(prefix, _cache_##name)), FC_ARCH_SUFFIX)

#define _FC_OPS_TNAME(prefix, suffix) \
    _FCG_CAT(_FCG_CAT(fc_, _FCG_CAT(prefix, _ops)), suffix)

#define FC_OPS_TABLE(prefix, suffix)                                           \
const struct fc_##prefix##_ops _FC_OPS_TNAME(prefix, suffix) = {               \
    .lookup_batch    = _FC_OPS_FNAME(prefix, lookup_batch),                    \
    .fill_miss_batch = _FC_OPS_FNAME(prefix, fill_miss_batch),                 \
    .maintain        = _FC_OPS_FNAME(prefix, maintain),                        \
    .maintain_step_ex = _FC_OPS_FNAME(prefix, maintain_step_ex),               \
    .maintain_step   = _FC_OPS_FNAME(prefix, maintain_step),                   \
}

#endif /* FC_ARCH_SUFFIX */

#endif /* _FC_CACHE_GENERATE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
