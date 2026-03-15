/* bench_scan.c - GEN vs SSE vs AVX2 find_u32x16 microbenchmark (cache-warm)
 *
 * Measures only the fingerprint scan (scan_bk stage) in isolation.
 * Bucket array is small enough to stay in L1/L2 cache throughout.
 * Each iteration scans NB_BK buckets (16 x uint32_t each) sequentially.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "rix_hash.h"

/* ------------------------------------------------------------------ */
/* TSC helpers                                                          */
/* ------------------------------------------------------------------ */
static inline uint64_t tsc_start(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t tsc_end(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtscp\n\tlfence\n\t" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo;
}

/* ------------------------------------------------------------------ */
/* Bench parameters                                                     */
/* ------------------------------------------------------------------ */
#define NB_BK   128          /* number of buckets; fits in L2 (128×64B=8KB)  */
#define REPEAT  2000000u     /* iterations                                   */

int main(void)
{
    /* Allocate bucket array */
    struct rix_hash_bucket_s *bk =
        (struct rix_hash_bucket_s *)aligned_alloc(64,
            NB_BK * sizeof(struct rix_hash_bucket_s));
    if (!bk) { perror("aligned_alloc"); return 1; }

    /* Fill with pseudo-random data; a few slots left as RIX_NIL=0 */
    uint64_t rng = 0xDEADBEEF12345678ULL;
    for (int i = 0; i < NB_BK; i++) {
        for (int j = 0; j < 16; j++) {
            rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
            /* leave slot 0 of every bucket as 0 (NIL) for a realistic hit */
            bk[i].hash[j] = (j == 0) ? 0u : (uint32_t)(rng >> 17);
            bk[i].idx[j]  = (j == 0) ? 0u : (uint32_t)(rng & 0xFFFFFFF) + 1u;
        }
    }

    /* Warm L2 cache */
    volatile uint32_t sink = 0;
    for (int i = 0; i < NB_BK; i++)
        for (int j = 0; j < 16; j++)
            sink += bk[i].hash[j];
    (void)sink;

    printf("bench_scan: NB_BK=%d  REPEAT=%u\n", NB_BK, REPEAT);
    printf("bucket array: %zu bytes\n\n", NB_BK * sizeof(struct rix_hash_bucket_s));

    /* ---------------------------------------------------------------- */
    /* GEN                                                               */
    /* ---------------------------------------------------------------- */
    {
        uint64_t min_cy = UINT64_MAX, sum_cy = 0;
        volatile uint32_t sink2 = 0;

        for (unsigned r = 0; r < REPEAT; r++) {
            uint32_t acc = 0;
            uint64_t t0 = tsc_start();
            for (int i = 0; i < NB_BK; i++) {
                acc |= _rix_hash_find_u32x16_GEN(bk[i].hash, (uint32_t)(r & 0xFFFF));
            }
            uint64_t cy = tsc_end() - t0;
            sink2 |= acc;
            if (cy < min_cy) min_cy = cy;
            sum_cy += cy;
        }
        (void)sink2;
        printf("GEN  : min=%6llu cy/%d bk  = %5.2f cy/bk  avg=%6llu cy\n",
               (unsigned long long)min_cy, NB_BK,
               (double)min_cy / NB_BK,
               (unsigned long long)(sum_cy / REPEAT));
    }

#if defined(__SSE4_2__)
    /* ---------------------------------------------------------------- */
    /* SSE (XMM 128-bit: cmpeq_epi32/SSE2, cmpeq_epi64/SSE4.1,        */
    /*      CRC32C/SSE4.2; all under -msse4.2)                         */
    /* ---------------------------------------------------------------- */
    {
        uint64_t min_cy = UINT64_MAX, sum_cy = 0;
        volatile uint32_t sink2 = 0;

        for (unsigned r = 0; r < REPEAT; r++) {
            uint32_t acc = 0;
            uint64_t t0 = tsc_start();
            for (int i = 0; i < NB_BK; i++) {
                acc |= _rix_hash_find_u32x16_SSE(bk[i].hash, (uint32_t)(r & 0xFFFF));
            }
            uint64_t cy = tsc_end() - t0;
            sink2 |= acc;
            if (cy < min_cy) min_cy = cy;
            sum_cy += cy;
        }
        (void)sink2;
        printf("SSE  : min=%6llu cy/%d bk  = %5.2f cy/bk  avg=%6llu cy\n",
               (unsigned long long)min_cy, NB_BK,
               (double)min_cy / NB_BK,
               (unsigned long long)(sum_cy / REPEAT));
    }
#else
    printf("SSE  : not compiled\n");
#endif

#if defined(__AVX2__)
    /* ---------------------------------------------------------------- */
    /* AVX2                                                              */
    /* ---------------------------------------------------------------- */
    {
        uint64_t min_cy = UINT64_MAX, sum_cy = 0;
        volatile uint32_t sink2 = 0;

        for (unsigned r = 0; r < REPEAT; r++) {
            uint32_t acc = 0;
            uint64_t t0 = tsc_start();
            for (int i = 0; i < NB_BK; i++) {
                acc |= _rix_hash_find_u32x16_AVX2(bk[i].hash, (uint32_t)(r & 0xFFFF));
            }
            uint64_t cy = tsc_end() - t0;
            sink2 |= acc;
            if (cy < min_cy) min_cy = cy;
            sum_cy += cy;
        }
        (void)sink2;
        printf("AVX2 : min=%6llu cy/%d bk  = %5.2f cy/bk  avg=%6llu cy\n",
               (unsigned long long)min_cy, NB_BK,
               (double)min_cy / NB_BK,
               (unsigned long long)(sum_cy / REPEAT));
    }
#else
    printf("AVX2 : not compiled\n");
#endif

    free(bk);
    return 0;
}
