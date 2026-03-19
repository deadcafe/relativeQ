/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_fc_common.h - Shared utilities for fcache benchmarks.
 */

#ifndef _BENCH_FC_COMMON_H_
#define _BENCH_FC_COMMON_H_

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "flow_cache.h"

/*===========================================================================
 * Constants
 *===========================================================================*/
enum {
    FCB_ALIGN           = 64u,
    FCB_QUERY           = 256u,
    FCB_HIT_REPEAT      = 200u,
    FCB_MISS_REPEAT     = 80u,
    FCB_MIXED_REPEAT    = 80u
};

/*===========================================================================
 * TSC helpers (no v1 dependency)
 *===========================================================================*/
static inline uint64_t
fcb_rdtsc(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

static inline uint64_t
fcb_calibrate_tsc_hz(void)
{
    struct timespec t0, t1;
    uint64_t tsc0, tsc1, ns;
    struct timespec req = { .tv_nsec = 1000000 }; /* 1 ms */

    clock_gettime(CLOCK_MONOTONIC, &t0);
    tsc0 = fcb_rdtsc();
    nanosleep(&req, NULL);
    tsc1 = fcb_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u
                + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    return ns ? (tsc1 - tsc0) * 1000000000ULL / ns : 0;
}

/*===========================================================================
 * Sizing helpers (replaces v1 flow_cache_pool_count / nb_bk_hint)
 *===========================================================================*/
static inline unsigned
fcb_pool_count(unsigned desired)
{
    if (desired < 64u)
        desired = 64u;
    desired--;
    desired |= desired >> 1;
    desired |= desired >> 2;
    desired |= desired >> 4;
    desired |= desired >> 8;
    desired |= desired >> 16;
    return desired + 1u;
}

static inline unsigned
fcb_nb_bk_hint(unsigned max_entries)
{
    unsigned n = (max_entries + 7u) / 8u;
    if (n < 2u)
        n = 2u;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1u;
}

/*===========================================================================
 * Allocation
 *===========================================================================*/
static inline size_t
fcb_round_up(size_t n, size_t align)
{
    return (n + align - 1u) & ~(align - 1u);
}

static inline void *
fcb_alloc(size_t size)
{
    size_t alloc_size = fcb_round_up(size, FCB_ALIGN);
    void *p = aligned_alloc(FCB_ALIGN, alloc_size);

    if (p == NULL) {
        perror("aligned_alloc");
        exit(1);
    }
    memset(p, 0, alloc_size);
    return p;
}

/*===========================================================================
 * Output helpers
 *===========================================================================*/
static inline void
fcb_emit3(const char *label, double v0, double v1, double v2)
{
    printf("  %-20s  flow4=%7.2f  flow6=%7.2f  flowu=%7.2f"
           "  (6/4=%+.1f%%  u/4=%+.1f%%)\n",
           label, v0, v1, v2,
           (v1 / v0 - 1.0) * 100.0,
           (v2 / v0 - 1.0) * 100.0);
}

/*===========================================================================
 * Batch-maint kick calculator
 *===========================================================================*/
static inline unsigned
fcb_batch_maint_kicks(unsigned fill_pct,
                       unsigned fill0, unsigned fill1,
                       unsigned fill2, unsigned fill3,
                       unsigned kicks0, unsigned kicks1,
                       unsigned kicks2, unsigned kicks3,
                       unsigned kick_scale)
{
    unsigned kicks;

    if (fill_pct >= fill3)
        kicks = kicks3;
    else if (fill_pct >= fill2)
        kicks = kicks2;
    else if (fill_pct >= fill1)
        kicks = kicks1;
    else if (fill_pct >= fill0)
        kicks = kicks0;
    else
        kicks = 0u;
    return kicks * kick_scale;
}

#endif /* _BENCH_FC_COMMON_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
