/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_CACHE_H_
#define _FLOW_CACHE_H_

#include <stdint.h>
#include <time.h>

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
 * TSC frequency calibration
 *
 * Calibrate TSC Hz by sleeping briefly and measuring elapsed ticks.
 * Called once at init time.  Returns ticks per second.
 *===========================================================================*/
static inline uint64_t
flow_cache_calibrate_tsc_hz(void)
{
    struct timespec req = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50 ms */
    uint64_t t0 = flow_cache_rdtsc();
    nanosleep(&req, NULL);
    uint64_t t1 = flow_cache_rdtsc();
    /* scale 50 ms → 1 s */
    return (t1 - t0) * 20;
}

/* Convert milliseconds to TSC ticks. */
static inline uint64_t
flow_cache_ms_to_tsc(uint64_t tsc_hz, uint64_t ms)
{
    return tsc_hz / 1000 * ms;
}

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
    uint64_t evictions;     /* entries expired/evicted */
    uint32_t nb_entries;    /* current entry count */
    uint32_t pool_cap;      /* pool capacity */
};

/*===========================================================================
 * Pipeline tuning parameters
 *===========================================================================*/
#define FLOW_CACHE_BATCH   8    /* keys per pipeline step */
#define FLOW_CACHE_KPD     8    /* pipeline depth (batches ahead) */
#define FLOW_CACHE_DIST    (FLOW_CACHE_BATCH * FLOW_CACHE_KPD)  /* 64 */

#endif /* _FLOW_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
