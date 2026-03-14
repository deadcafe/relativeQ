/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_cache_decl.h - Flow cache base definitions and per-variant declarations.
 *
 * This file serves two roles, selected by whether FC_PREFIX is defined:
 *
 *   Section 1 - Once-only base definitions (include-guarded):
 *     TSC helpers, FC_CALL macro, pipeline/expire constants,
 *     flow_cache_stats, flow_cache_nb_bk_hint.
 *     Safe to include before struct definitions.
 *
 *   Section 2 - Per-variant template declarations (active when FC_PREFIX defined):
 *     struct FC_CACHE, function prototypes, inline helpers.
 *     Instantiated once per variant (flow4, flow6, flowu).
 *
 * Typical usage in a variant header (e.g. flow4_cache.h):
 *
 *   // Early include: Section 1 only (FC_PREFIX not yet defined)
 *   #include "flow_cache_decl.h"
 *
 *   struct flow4_entry { FLOW_CACHE_CL(0); ... };  // uses Section 1 macros
 *   ...
 *
 *   // Template include: Section 2 only (FC_PREFIX now defined)
 *   #define FC_PREFIX    flow4
 *   #define FC_ENTRY     flow4_entry
 *   ...
 *   #include "flow_cache_decl.h"
 *   #undef FC_PREFIX
 *   ...
 */

/*===========================================================================
 * Section 1: Once-only base definitions
 * (formerly flow_cache_defs.h)
 *===========================================================================*/
#ifndef _FLOW_CACHE_DECL_ONCE_H_
#define _FLOW_CACHE_DECL_ONCE_H_

#include <stdint.h>
#include <time.h>
#if defined(__x86_64__)
# include <cpuid.h>
#endif
#if defined(__linux__)
# include <stdio.h>
#endif

/*===========================================================================
 * TSC helpers
 *===========================================================================*/
static inline uint64_t
flow_cache_rdtsc(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/*===========================================================================
 * TSC frequency detection - fallback chain:
 *   1. CPUID 0x15  - TSC/Crystal ratio  (x86, Intel Skylake+)
 *   2. CPUID 0x16  - processor base MHz (x86, Intel Broadwell+)
 *   3. sysfs       - tsc_freq_khz       (Linux, not always present)
 *   4. clock_gettime - 1 ms measurement (portable last resort)
 *===========================================================================*/

#if defined(__x86_64__)
/* CPUID 0x15: TSC / Core Crystal Clock ratio.
 * ECX gives crystal Hz directly; if zero, look it up by CPU model. */
static inline uint64_t
_tsc_hz_cpuid15(void)
{
    uint32_t a, b, c, d;
    if (!__get_cpuid(0x15, &a, &b, &c, &d) || a == 0 || b == 0)
        return 0;
    if (c != 0)
        return (uint64_t)c * b / a;

    /* ECX == 0: determine crystal Hz from CPU model (Intel errata) */
    uint32_t ma = 0, mb = 0, mc = 0, md = 0;
    __get_cpuid(1, &ma, &mb, &mc, &md);
    unsigned model  = ((ma >> 4) & 0xf) | (((ma >> 16) & 0xf) << 4);
    unsigned family = ((ma >> 8) & 0xf) + ((ma >> 20) & 0xff);
    uint64_t crystal_hz = 0;
    if (family == 6) {
        switch (model) {
        case 0x4e: case 0x5e:           /* Skylake */
        case 0x8e: case 0x9e:           /* Kaby Lake */
        case 0xa5: case 0xa6:           /* Comet Lake */
            crystal_hz = 24000000; break;
        case 0x55:                      /* Skylake-X / Cascade Lake */
            crystal_hz = 25000000; break;
        case 0x5c: case 0x5f:           /* Goldmont (Atom) */
        case 0x7a:                      /* Goldmont Plus */
            crystal_hz = 19200000; break;
        default: break;
        }
    }
    return crystal_hz ? crystal_hz * b / a : 0;
}

/* CPUID 0x16: processor base frequency in MHz (Intel Broadwell+).
 * AMD returns 0 here; gives nominal base, which matches invariant TSC. */
static inline uint64_t
_tsc_hz_cpuid16(void)
{
    uint32_t a, b, c, d;
    if (!__get_cpuid(0x16, &a, &b, &c, &d))
        return 0;
    uint32_t base_mhz = a & 0xffff;
    return base_mhz ? (uint64_t)base_mhz * 1000000 : 0;
}
#endif /* __x86_64__ */

#if defined(__linux__)
/* sysfs: /sys/devices/system/cpu/cpu0/tsc_freq_khz
 * Present on some kernels; covers both Intel and AMD. */
static inline uint64_t
_tsc_hz_sysfs(void)
{
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/tsc_freq_khz", "r");
    if (!f)
        return 0;
    unsigned long long khz = 0;
    int n = fscanf(f, "%llu", &khz);
    (void)n;
    fclose(f);
    return khz ? (uint64_t)khz * 1000 : 0;
}
#endif /* __linux__ */

/* clock_gettime: measure TSC ticks against CLOCK_MONOTONIC over 1 ms.
 * Works on any platform; uses actual elapsed ns so nanosleep slop cancels. */
static inline uint64_t
_tsc_hz_clock_gettime(void)
{
    struct timespec t0, t1;
    uint64_t tsc0, tsc1;
    struct timespec req = { .tv_nsec = 1000000 }; /* 1 ms */

    clock_gettime(CLOCK_MONOTONIC, &t0);
    tsc0 = flow_cache_rdtsc();
    nanosleep(&req, NULL);
    tsc1 = flow_cache_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    uint64_t ns = (uint64_t)(t1.tv_sec  - t0.tv_sec)  * 1000000000ULL
                + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    return ns ? (tsc1 - tsc0) * 1000000000ULL / ns : 0;
}

static inline uint64_t
flow_cache_calibrate_tsc_hz(void)
{
    uint64_t hz;
#if defined(__x86_64__)
    if ((hz = _tsc_hz_cpuid15()))   return hz;  /* Intel Skylake+   */
    if ((hz = _tsc_hz_cpuid16()))   return hz;  /* Intel Broadwell+ */
#endif
#if defined(__linux__)
    if ((hz = _tsc_hz_sysfs()))     return hz;  /* Linux sysfs      */
#endif
    return _tsc_hz_clock_gettime();             /* portable 1ms measure */
}

/* Convert milliseconds to TSC ticks. */
static inline uint64_t
flow_cache_ms_to_tsc(uint64_t tsc_hz, uint64_t ms)
{
    return tsc_hz / 1000 * ms;
}

/*===========================================================================
 * FC_CALL: variant-generic API call macro
 *
 * FC_CALL(prefix, suffix) expands to prefix##_##suffix after full macro
 * expansion of both arguments, so a macro prefix works correctly.
 * The underscore separator is inserted automatically by the macro.
 *
 * Example:
 *   FC_CALL(flow4, cache_init)(&fc, buckets, nb_bk, pool, max_entries, timeout_ms);
 *   FC_CALL(flow4, cache_lookup_batch)(&fc, keys, nb_pkts, results);
 *   FC_CALL(flow4, cache_insert)(&fc, &key, now);
 *   FC_CALL(flow4, cache_touch)(entry, now, pkt_len);
 *   FC_CALL(flow4, cache_expire)(&fc, now);
 *   FC_CALL(flow4, cache_remove)(&fc, entry);
 *   FC_CALL(flow4, cache_flush)(&fc);
 *   FC_CALL(flow4, cache_stats)(&fc, &st);
 *
 * When the prefix itself is a macro (e.g. #define MY_VARIANT flow6),
 * the two-level indirection ensures it is expanded before concatenation:
 *   FC_CALL(MY_VARIANT, cache_init)(&fc, ...);
 *===========================================================================*/
#define _FC_CALL_CAT(a, b, c)       a ## b ## c
#define FC_CALL_CAT(a, b, c)        _FC_CALL_CAT(a, b, c)
#define FC_CALL(prefix, suffix)     FC_CALL_CAT(prefix, _, suffix)

/*===========================================================================
 * Common constants
 *===========================================================================*/
#define FLOW_ACTION_NONE   0u   /* action not yet filled by slow path */

/*===========================================================================
 * Statistics
 *===========================================================================*/
struct flow_cache_stats {
    uint64_t lookups;       /* total lookup calls */
    uint64_t hits;          /* cache hits */
    uint64_t misses;        /* cache misses */
    uint64_t inserts;       /* entries inserted */
    uint64_t evictions;     /* entries expired by timeout (expire/expire_2stage) */
    uint64_t removes;       /* entries explicitly removed by caller (remove/flush) */
    uint32_t nb_entries;    /* current entry count */
    uint32_t max_entries;   /* pool capacity */
};

/*===========================================================================
 * Sizing helper
 *===========================================================================*/

/*
 * flow_cache_nb_bk_hint - recommended nb_bk for a target ~50% slot fill.
 *
 * Each RIX_HASH bucket holds 16 slots (16-way cuckoo).
 * For 50% fill: total_slots = nb_bk * 16 = 2 * max_entries
 *               => nb_bk = ceil(max_entries / 8), rounded up to power of 2.
 * nb_bk must be a power of 2 (used as a bitmask inside rix_hash).
 * Minimum returned value is 2.
 */
static inline unsigned
flow_cache_nb_bk_hint(unsigned max_entries)
{
    unsigned n = (max_entries + 7u) / 8u;   /* ceil(max_entries / 8) */
    if (n < 2u)
        n = 2u;
    /* round up to next power of 2 */
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1u;
}

/*===========================================================================
 * Pipeline tuning parameters
 *===========================================================================*/
#define FLOW_CACHE_LINE_SIZE  64  /* CPU cache line size in bytes */

/*
 * Cache-line boundary marker (cf. DPDK RTE_CACHE_LINE_VAR).
 * A zero-length aligned member forces the next field onto a fresh cache line
 * and makes the CL layout visible directly in the struct definition.
 * Usage: place FLOW_CACHE_CL(0), FLOW_CACHE_CL(1), ... at each boundary.
 */
#define FLOW_CACHE_CL(n) \
    char _cl##n[0] __attribute__((aligned(FLOW_CACHE_LINE_SIZE)))

#define FLOW_CACHE_BATCH   8    /* keys per pipeline step */
#define FLOW_CACHE_KPD     8    /* pipeline depth (batches ahead) */
#define FLOW_CACHE_DIST    (FLOW_CACHE_BATCH * FLOW_CACHE_KPD)  /* 64 */

/*===========================================================================
 * Expire tuning parameters
 *===========================================================================*/
#define FLOW_CACHE_EXPIRE_SCAN_MIN    64   /* base scan count (low fill) */
#define FLOW_CACHE_EXPIRE_SCAN_MAX  1024   /* max scan count (near full) */
#define FLOW_CACHE_EXPIRE_PF_DIST     16   /* SW prefetch distance (entries) */

/* Miss-rate-driven timeout adjustment parameters */
#define FLOW_CACHE_TIMEOUT_DECAY_SHIFT    12  /* log2(batch) + 4 for batch=256 */
#define FLOW_CACHE_TIMEOUT_RECOVER_SHIFT   8  /* recovery speed: 1/256 per batch */
#define FLOW_CACHE_TIMEOUT_MIN_MS       1000  /* min timeout = 1.0 second */

#endif /* _FLOW_CACHE_DECL_ONCE_H_ */

/*===========================================================================
 * Section 2: Per-variant template declarations
 * (formerly flow_cache_common.h)
 *
 * Active only when FC_PREFIX is defined by the including variant header.
 * Before including this section, define:
 *   FC_PREFIX        e.g. flow4
 *   FC_ENTRY         e.g. flow4_entry
 *   FC_KEY           e.g. flow4_key
 *   FC_CACHE         e.g. flow4_cache
 *   FC_HT            e.g. flow4_ht        (hash table head struct tag)
 *   FC_FREE_HEAD     e.g. flow4_free_head
 *
 * The entry type (struct FC_ENTRY) and hash table (RIX_HASH_HEAD/GENERATE)
 * must already be defined before this section is activated.
 *===========================================================================*/
#ifdef FC_PREFIX

/* token-paste helpers */
#define _FCC_CAT(a, b, c)      a ## b ## c
#define FCC_CAT(a, b, c)       _FCC_CAT(a, b, c)
#define FCC_FN(prefix, suffix) FCC_CAT(prefix, _, suffix)

/*===========================================================================
 * Free list head + cache struct
 *===========================================================================*/
RIX_SLIST_HEAD(FC_FREE_HEAD, FC_ENTRY);

struct FC_CACHE {
    /* hash table */
    struct FC_HT                 ht_head;
    struct rix_hash_bucket_s    *buckets;
    unsigned                     nb_bk;

    /* node pool (max_entries must be power of 2) */
    struct FC_ENTRY             *pool;
    unsigned                     max_entries;
    unsigned                     entries_mask;  /* max_entries - 1 */

    /* free list */
    struct FC_FREE_HEAD          free_head;

    /* aging */
    unsigned                     age_cursor;
    uint64_t                     timeout_tsc;     /* base timeout (configured) */
    uint64_t                     eff_timeout_tsc; /* effective timeout (auto-adjusted) */
    uint64_t                     min_timeout_tsc; /* minimum timeout floor */

    /* stats */
    struct flow_cache_stats      stats;
};

/*===========================================================================
 * API declarations
 *
 * Typical packet processing loop:
 *
 *   uint64_t now = flow_cache_rdtsc();
 *
 *   // 1. Pipelined batch lookup (DRAM-latency hiding)
 *   PREFIX_cache_lookup_batch(&fc, keys, nb_pkts, results);
 *
 *   // 2. Per-packet post-processing
 *   unsigned misses = 0;
 *   for (unsigned i = 0; i < nb_pkts; i++) {
 *       if (results[i]) {
 *           PREFIX_cache_touch(results[i], now, pkt_len[i]);  // hit
 *       } else {
 *           struct ENTRY *e = PREFIX_cache_insert(&fc, &keys[i], now); // miss
 *           if (e) { e->action = slow_path(&keys[i]); }
 *           misses++;
 *       }
 *   }
 *
 *   // 3. Adaptive timeout adjustment (call every batch)
 *   PREFIX_cache_adjust_timeout(&fc, misses);
 *
 *   // 4. Aging eviction (call every batch or every N batches)
 *   PREFIX_cache_expire(&fc, now);
 *
 *   // 5. Explicit remove (e.g., TCP FIN/RST, admin teardown)
 *   PREFIX_cache_remove(&fc, entry);
 *
 *   // 6. Bulk clear (e.g., VRF delete, interface down)
 *   PREFIX_cache_flush(&fc);
 *===========================================================================*/

/* Lifecycle */
void FCC_FN(FC_PREFIX, cache_init)(struct FC_CACHE *fc,
                         struct rix_hash_bucket_s *buckets,
                         unsigned nb_bk,
                         struct FC_ENTRY *pool,
                         unsigned max_entries,
                         uint64_t timeout_ms);

void FCC_FN(FC_PREFIX, cache_flush)(struct FC_CACHE *fc);

/* Lookup */
void FCC_FN(FC_PREFIX, cache_lookup_batch)(struct FC_CACHE *fc,
                                 const struct FC_KEY *keys,
                                 unsigned nb_pkts,
                                 struct FC_ENTRY **results);

struct FC_ENTRY *FCC_FN(FC_PREFIX, cache_find)(struct FC_CACHE *fc,
                                     const struct FC_KEY *key);

/* Mutation */
struct FC_ENTRY *FCC_FN(FC_PREFIX, cache_insert)(struct FC_CACHE *fc,
                                       const struct FC_KEY *key,
                                       uint64_t now);

void FCC_FN(FC_PREFIX, cache_remove)(struct FC_CACHE *fc,
                            struct FC_ENTRY *entry);

/* Aging */
void FCC_FN(FC_PREFIX, cache_expire)(struct FC_CACHE *fc, uint64_t now);

/*
 * expire_2stage: bucket-prefetch variant of expire.
 *
 * Adds a mid-distance stage that prefetches the hash bucket before
 * _remove(), warming it when the pool >> LLC or eviction rate is high.
 * Has extra overhead (stage-1 check) that hurts when buckets are already
 * warm (small pool) or eviction is rare.  Prefer expire() by default.
 */
void FCC_FN(FC_PREFIX, cache_expire_2stage)(struct FC_CACHE *fc, uint64_t now);

/* Statistics */
void FCC_FN(FC_PREFIX, cache_stats)(const struct FC_CACHE *fc,
                          struct flow_cache_stats *out);

/*===========================================================================
 * Inline helpers
 *===========================================================================*/

/* Current number of active entries. */
static inline unsigned
FCC_FN(FC_PREFIX, cache_nb_entries)(const struct FC_CACHE *fc)
{
    return fc->ht_head.rhh_nb;
}

/* Update cached slow-path result for a hit entry. */
static inline void
FCC_FN(FC_PREFIX, cache_update_action)(struct FC_ENTRY *entry,
                             uint32_t action,
                             uint32_t qos_class)
{
    entry->action    = action;
    entry->qos_class = qos_class;
}

/* Record a packet hit: refresh timestamp and accumulate counters. */
static inline void
FCC_FN(FC_PREFIX, cache_touch)(struct FC_ENTRY *entry, uint64_t now,
                     uint32_t pkt_len)
{
    entry->last_ts = now;
    entry->packets++;
    entry->bytes += pkt_len;
}

/*
 * Miss-rate-driven timeout adjustment.
 *
 * Call once per batch, passing the number of misses (new flows) in that
 * batch.  High miss rate shortens eff_timeout_tsc (more aggressive aging);
 * quiet periods let it recover toward the configured base timeout.
 *
 * All shift/multiply, no division.
 *   FLOW_CACHE_TIMEOUT_DECAY_SHIFT   controls decay sensitivity (larger = gentler).
 *   FLOW_CACHE_TIMEOUT_RECOVER_SHIFT controls recovery speed    (larger = slower).
 */
static inline void
FCC_FN(FC_PREFIX, cache_adjust_timeout)(struct FC_CACHE *fc, unsigned misses)
{
    uint64_t eff    = fc->eff_timeout_tsc;
    uint64_t base   = fc->timeout_tsc;
    uint64_t min_to = fc->min_timeout_tsc;

    if (misses > 0) {
        uint64_t decay = (eff * misses) >> FLOW_CACHE_TIMEOUT_DECAY_SHIFT;
        if (decay == 0)
            decay = 1;
        eff = (eff > min_to + decay) ? eff - decay : min_to;
    }

    /* recovery: slowly restore toward base */
    eff += (base - eff) >> FLOW_CACHE_TIMEOUT_RECOVER_SHIFT;

    fc->eff_timeout_tsc = eff;
}

/* clean up local macros */
#undef FCC_FN
#undef FCC_CAT
#undef _FCC_CAT

#endif /* FC_PREFIX */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
