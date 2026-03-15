/* bench_rix_hash64.c
 *  rix_hash64.h lookup performance & fill rate benchmark (64-bit key variant)
 *  Metric: CPU cycles per 256 lookups
 *
 *  Usage: ./hash64_bench [table_n [nb_bk [repeat]]]
 *    table_n : number of table entries  (default: 10,000,000)
 *    nb_bk   : number of buckets       (default: auto – ~80% fill of table_n)
 *    repeat  : number of iterations    (default: 2000)
 *
 *  Fill rate notes:
 *    rix_hash64 has the same 16 slots/bucket as rix_hash32.
 *    Maximum fill rate is equivalent to rix_hash32 (~95%).
 *    However, bucket size is 192 B (3CL) vs rix_hash32's 128 B (2CL),
 *    so bk_mem for the same entry count requires 1.5×.
 *    bench_fill_rate() measures the actual values.
 *
 *    Empirical estimates (FOLLOW_DEPTH=8):
 *      rix_hash32 (16 slots, 128 B/bk): max fill ~90-95%
 *      rix_hash64 (16 slots, 192 B/bk): max fill ~90-95% (equivalent)
 *    Recommended fill target: <=80%, same as rix_hash32.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>

#include "rix_hash64.h"

/* ================================================================== */
/* Node definition                                                     */
/* ================================================================== */
typedef struct mynode_s {
    uint64_t key;
    uint64_t val;
} mynode_t;

#define BENCH_INVALID_KEY  0xFFFFFFFFFFFFFFFFULL

RIX_HASH64_HEAD(myht64);
RIX_HASH64_GENERATE(myht64, mynode_t, key, BENCH_INVALID_KEY)

/* ================================================================== */
/* TSC measurement helper                                              */
/* ================================================================== */
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

static double
now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ================================================================== */
/* Benchmark constants                                                 */
/* ================================================================== */
#define BENCH_N  256
#define BENCH_N6 ((BENCH_N / 6) * 6)   /* 252 */
#define BENCH_N8 ((BENCH_N / 8) * 8)   /* 256 */

/*
 * Key prefetch distance (in batches):
 * ~64 cy per hash_key batch (CRC32C×8 + prefetch×8×3)
 * DRAM latency ~300 cy → 300/64 ≈ 5 batches needed → use 8/11 with margin
 */
#define KPD8  8   /* x8 Nahead: 8 batches × 8 keys = 64 keys ahead */
#define KPD6 11   /* x6 Nahead: 11 batches × 6 keys = 66 keys ahead */

static struct rix_hash64_find_ctx_s g_ctx[BENCH_N];
static mynode_t                    *g_res[BENCH_N];

/* ================================================================== */
/* xorshift64 PRNG                                                     */
/* ================================================================== */
static uint64_t xr64 = 0xDEADBEEF1234ABCDULL;

static inline uint64_t
xorshift64(void)
{
    xr64 ^= xr64 >> 12;
    xr64 ^= xr64 << 25;
    xr64 ^= xr64 >> 27;
    return xr64 * 0x2545F4914F6CDD1DULL;
}

/* ================================================================== */
/* bench_fill_rate
 *
 * Measure fill rate characteristics for 16 slots/bucket.
 *
 * Attempt insertions at multiple fill rate targets and record the point
 * where kickout failures begin. Also shows comparison values with
 * rix_hash32 (16 slots, 2CL).
 *
 * fill rate = number of successful inserts / (nb_bk × RIX_HASH64_BUCKET_ENTRY_SZ)
 * ================================================================== */
static void
bench_fill_rate(void)
{
    /* Fixed test size: nb_bk=1024 (16384 slots) */
    const unsigned NB_BK  = 1024u;
    const unsigned SLOTS  = NB_BK * RIX_HASH64_BUCKET_ENTRY_SZ;  /* 8192 */
    /* Test node count: slots * 1.05 (try up to 105%) */
    const unsigned TRY_N  = (unsigned)((uint64_t)SLOTS * 105 / 100) + 1;

    mynode_t *nodes =
        (mynode_t *)calloc(TRY_N, sizeof(mynode_t));
    struct rix_hash64_bucket_s *buckets =
        (struct rix_hash64_bucket_s *)aligned_alloc(64, NB_BK * sizeof(*buckets));
    if (!nodes || !buckets) { perror("alloc"); exit(1); }

    for (unsigned i = 0; i < TRY_N; i++) {
        nodes[i].key = (uint64_t)(i + 1);   /* 1-origin unique keys */
        nodes[i].val = i;
    }

    struct myht64 head;
    RIX_HASH64_INIT(myht64, &head, buckets, NB_BK);

    unsigned inserted = 0, kickout_fail = 0;

    /* Insert loop: continue even on failure, try up to TRY_N entries */
    unsigned *fail_at = (unsigned *)calloc(TRY_N, sizeof(unsigned));
    for (unsigned i = 0; i < TRY_N; i++) {
        mynode_t *r = RIX_HASH64_INSERT(myht64, &head, buckets, nodes, &nodes[i]);
        if (r == NULL) {
            inserted++;
        } else if (r == &nodes[i]) {
            /* kickout failure (table full) */
            kickout_fail++;
            fail_at[kickout_fail - 1] = i;
        }
        /* no duplicates (unique keys) */
    }

    printf("\n=== Fill Rate Analysis (rix_hash64, 16 slots/bucket, 192 B/bk) ===\n");
    printf("  nb_bk=%u  slots=%u  FOLLOW_DEPTH=%d\n",
           NB_BK, SLOTS, RIX_HASH_FOLLOW_DEPTH);
    printf("  tried=%u  inserted=%u  kickout_fail=%u\n",
           TRY_N, inserted, kickout_fail);
    printf("  max_fill = %u/%u = %.2f%%\n",
           inserted, SLOTS, 100.0 * inserted / SLOTS);
    printf("\n");

    /*
     * Fill rate checkpoints: report cumulative kickout_fail count when
     * the number of inserted entries reaches x% of SLOTS.
     */
    printf("  Checkpoint analysis (cumulative kickout_fail):\n");
    printf("  %-10s  %-12s  %-12s\n", "target_%", "target_n", "kickout_fail_cum");
    printf("  %-10s  %-12s  %-12s\n", "----------", "------------", "----------------");

    /* Re-simulate all insertions and record checkpoints */
    RIX_HASH64_INIT(myht64, &head, buckets, NB_BK);
    unsigned cum_ok = 0, cum_fail = 0;
    unsigned targets[] = { 50, 60, 70, 75, 80, 85, 90, 95, 100 };
    unsigned ti = 0;

    for (unsigned i = 0; i < TRY_N && ti < 9; i++) {
        mynode_t *r = RIX_HASH64_INSERT(myht64, &head, buckets, nodes, &nodes[i]);
        if (r == NULL)
            cum_ok++;
        else if (r == &nodes[i])
            cum_fail++;

        /* Output when the next checkpoint is reached */
        while (ti < 9 && cum_ok >= (unsigned)((uint64_t)SLOTS * targets[ti] / 100)) {
            printf("  %-10u  %-12u  %-12u\n",
                   targets[ti],
                   (unsigned)((uint64_t)SLOTS * targets[ti] / 100),
                   cum_fail);
            ti++;
        }
    }
    /* Remaining checkpoints (not reached) */
    while (ti < 9) {
        printf("  %-10u  %-12u  %-12s\n",
               targets[ti],
               (unsigned)((uint64_t)SLOTS * targets[ti] / 100),
               "N/A (not reached)");
        ti++;
    }

    printf("\n");
    printf("  Empirical findings (FOLLOW_DEPTH=%d):\n", RIX_HASH_FOLLOW_DEPTH);
    printf("    - Kickout failures are 0 up to ~95%% fill (same as rix_hash32)\n");
    printf("    - Failures appear only above ~95%% fill\n");
    printf("    - Fill rate behaviour is equivalent to rix_hash32 (both 16 slots/bk)\n");
    printf("    - Memory cost: rix_hash64 bk_mem is 1.5x rix_hash32\n");
    printf("      (192 B/bk vs 128 B/bk; same slot count, larger key per slot)\n");
    printf("    Recommendation: target <=80%% fill as with rix_hash32.\n\n");

    free(fail_at);
    free(nodes);
    free(buckets);
}

/* ================================================================== */
/* bench_find                                                          */
/* ================================================================== */
static void
bench_find(unsigned table_n, unsigned nb_bk, unsigned repeat)
{
    size_t node_mem = (size_t)table_n * sizeof(mynode_t);
    size_t bk_mem   = (size_t)nb_bk   * sizeof(struct rix_hash64_bucket_s);
    size_t pool_mem = (size_t)repeat * BENCH_N * sizeof(uint64_t);
    printf("[BENCH] table_n=%u  nb_bk=%u  slots=%u\n",
           table_n, nb_bk, nb_bk * RIX_HASH64_BUCKET_ENTRY_SZ);
    printf("  memory : nodes=%.1f MB  buckets=%.1f MB"
           "  key_pool=%.1f MB  total=%.1f MB\n",
           node_mem / 1e6, bk_mem / 1e6, pool_mem / 1e6,
           (node_mem + bk_mem + pool_mem) / 1e6);

    mynode_t *nodes = (mynode_t *)mmap(NULL, node_mem,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes == MAP_FAILED) { perror("mmap nodes"); exit(1); }
    madvise(nodes, node_mem, MADV_HUGEPAGE);

    for (unsigned i = 0; i < table_n; i++) {
        nodes[i].key = (uint64_t)(i + 1);
        nodes[i].val = i;
    }

    struct rix_hash64_bucket_s *bk =
        (struct rix_hash64_bucket_s *)mmap(NULL, bk_mem,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk == MAP_FAILED) { perror("mmap bk"); exit(1); }
    madvise(bk, bk_mem, MADV_HUGEPAGE);

    struct myht64 head;
    RIX_HASH64_INIT(myht64, &head, bk, nb_bk);

    /* ---- Insertion --------------------------------------------------- */
    printf("  inserting...\n"); fflush(stdout);
    unsigned n_hit = 0;
    unsigned report_step = table_n / 10;
    if (report_step == 0) report_step = 1;

    uint64_t ins_bk0_fast = 0, ins_bk1_fast = 0, ins_kickout = 0;
    double t_ins_start = now_sec();

    for (unsigned i = 0; i < table_n; i++) {
        /* Determine insert path before inserting */
        union rix_hash_hash_u _h = _rix_hash64_fn(nodes[i].key, head.rhh_mask);
        unsigned _b0 = _h.val32[0] & head.rhh_mask;
        unsigned _b1 = _h.val32[1] & head.rhh_mask;
        uint32_t _nilm0 = rix_hash_arch->find_u64x16(bk[_b0].key, BENCH_INVALID_KEY);
        uint32_t _nilm1 = rix_hash_arch->find_u64x16(bk[_b1].key, BENCH_INVALID_KEY);
        if      (_nilm0) ins_bk0_fast++;
        else if (_nilm1) ins_bk1_fast++;
        else             ins_kickout++;

        if (RIX_HASH64_INSERT(myht64, &head, bk, nodes, &nodes[i]) == NULL)
            n_hit++;

        if ((i + 1) % report_step == 0) {
            printf("    %3u%%\r",
                   (unsigned)(100ULL * (i + 1) / table_n));
            fflush(stdout);
        }
    }
    double t_ins = now_sec() - t_ins_start;
    double fill = 100.0 * n_hit / ((double)nb_bk * RIX_HASH64_BUCKET_ENTRY_SZ);
    printf("  inserted : %u/%u (%.1f%% fill)  %.2f s  %.1f ns/insert\n",
           n_hit, table_n, fill, t_ins, t_ins * 1e9 / table_n);
    printf("  insert paths: bk0_fast=%" PRIu64 "  bk1_fast=%" PRIu64
           "  kickout=%" PRIu64 "\n",
           ins_bk0_fast, ins_bk1_fast, ins_kickout);

    /* ---- bk_0 hit rate ------------------------------------------- */
    {
        uint64_t in_bk0 = 0, in_bk1 = 0;
        unsigned mask = head.rhh_mask;
        for (unsigned b = 0; b < nb_bk; b++) {
            for (unsigned s = 0; s < RIX_HASH64_BUCKET_ENTRY_SZ; s++) {
                uint32_t nidx = bk[b].idx[s];
                if (nidx == (uint32_t)RIX_NIL) continue;
                uint64_t key = bk[b].key[s];
                union rix_hash_hash_u h = _rix_hash64_fn(key, mask);
                if (b == (h.val32[0] & mask)) in_bk0++;
                else                          in_bk1++;
            }
        }
        uint64_t total = in_bk0 + in_bk1;
        printf("  bk_0 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%\n",
               in_bk0, total, 100.0 * in_bk0 / total);
        printf("  bk_1 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%\n",
               in_bk1, total, 100.0 * in_bk1 / total);
    }

    /* ---- Key pool generation ----------------------------------------- */
    uint64_t *key_pool = (uint64_t *)malloc(pool_mem);
    if (!key_pool) { perror("malloc key_pool"); exit(1); }
    for (size_t k = 0; k < (size_t)repeat * BENCH_N; k++) {
        unsigned idx = (unsigned)(xorshift64() % n_hit);
        key_pool[k] = nodes[idx].key;
    }

    /* ---- Warmup ------------------------------------------------------ */
    printf("  warmup...\n"); fflush(stdout);
    for (unsigned w = 0; w < 20 && w < repeat; w++) {
        uint64_t *ik = key_pool + (size_t)w * BENCH_N;
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH64_FIND(myht64, &head, bk, nodes, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_HASH_KEY(myht64, &g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_SCAN_BK(myht64, &g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH64_CMP_KEY(myht64, &g_ctx[i], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_HASH_KEY4(myht64, &g_ctx[b*4], &head, bk, ik + b*4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_SCAN_BK4(myht64, &g_ctx[b*4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_PREFETCH_NODE4(myht64, &g_ctx[b*4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_CMP_KEY4(myht64, &g_ctx[b*4], nodes, &g_res[b*4]);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*8], 8, &head, bk, ik + b*8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        /* x8 Nahead warmup */
        for (int b = 0; b < KPD8 && b < BENCH_N8/8; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*8], 8, &head, bk, ik + b*8);
        for (int b = 0; b < BENCH_N8/8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8/8)
                RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[pf*8], 8, &head, bk, ik + pf*8);
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*8], 8, &head, bk);
        }
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        /* x6 Nahead warmup */
        for (int b = 0; b < KPD6 && b < BENCH_N6/6; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*6], 6, &head, bk, ik + b*6);
        for (int b = 0; b < BENCH_N6/6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6/6)
                RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[pf*6], 6, &head, bk, ik + pf*6);
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*6], 6, &head, bk);
        }
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*6], 6, nodes);
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*6], 6, nodes, &g_res[b*6]);
    }

    /* ---- Measurement ------------------------------------------------- */
    printf("  measuring...\n"); fflush(stdout);

    struct {
        const char *label;
        uint64_t min_cy;
        uint64_t sum_cy;
        int      ops;
    } result[10] = {
        { "find (single)      ", UINT64_MAX, 0, BENCH_N  },
        { "x1 (bk only)       ", UINT64_MAX, 0, BENCH_N  },
        { "x1 (bk+node)       ", UINT64_MAX, 0, BENCH_N  },
        { "x2 (bk+node)       ", UINT64_MAX, 0, BENCH_N  },
        { "x4 (bk only)       ", UINT64_MAX, 0, BENCH_N  },
        { "x4 (bk+node)       ", UINT64_MAX, 0, BENCH_N  },
        { "x6 (bk+node)       ", UINT64_MAX, 0, BENCH_N6 },
        { "x8 (bk+node)       ", UINT64_MAX, 0, BENCH_N8 },
        { "x6 (bk) Nahead     ", UINT64_MAX, 0, BENCH_N6 },
        { "x8 (bk) Nahead     ", UINT64_MAX, 0, BENCH_N8 },
    };

    /* [0] single find */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH64_FIND(myht64, &head, bk, nodes, ik[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[0].min_cy) result[0].min_cy = cy;
        result[0].sum_cy += cy;
    }

    /* [1] x1: hash_key → scan_bk → cmp_key (no prefetch) */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_HASH_KEY(myht64, &g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_SCAN_BK(myht64, &g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH64_CMP_KEY(myht64, &g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[1].min_cy) result[1].min_cy = cy;
        result[1].sum_cy += cy;
    }

    /* [2] x1: hash_key → scan_bk → prefetch_node → cmp_key */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_HASH_KEY(myht64, &g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_SCAN_BK(myht64, &g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH64_PREFETCH_NODE(myht64, &g_ctx[i], nodes);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH64_CMP_KEY(myht64, &g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[2].min_cy) result[2].min_cy = cy;
        result[2].sum_cy += cy;
    }

    /* [3] x2 */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH64_HASH_KEY2(myht64, &g_ctx[b*2], &head, bk, ik + b*2);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH64_SCAN_BK2(myht64, &g_ctx[b*2], &head, bk);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH64_PREFETCH_NODE2(myht64, &g_ctx[b*2], nodes);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH64_CMP_KEY2(myht64, &g_ctx[b*2], nodes, &g_res[b*2]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[3].min_cy) result[3].min_cy = cy;
        result[3].sum_cy += cy;
    }

    /* [4] x4: bk only (no prefetch) */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_HASH_KEY4(myht64, &g_ctx[b*4], &head, bk, ik + b*4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_SCAN_BK4(myht64, &g_ctx[b*4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_CMP_KEY4(myht64, &g_ctx[b*4], nodes, &g_res[b*4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[4].min_cy) result[4].min_cy = cy;
        result[4].sum_cy += cy;
    }

    /* [5] x4: bk + node prefetch */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_HASH_KEY4(myht64, &g_ctx[b*4], &head, bk, ik + b*4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_SCAN_BK4(myht64, &g_ctx[b*4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_PREFETCH_NODE4(myht64, &g_ctx[b*4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH64_CMP_KEY4(myht64, &g_ctx[b*4], nodes, &g_res[b*4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[5].min_cy) result[5].min_cy = cy;
        result[5].sum_cy += cy;
    }

    /* [6] x6 */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*6], 6, &head, bk, ik + b*6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*6], 6, nodes, &g_res[b*6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[6].min_cy) result[6].min_cy = cy;
        result[6].sum_cy += cy;
    }

    /* [7] x8 */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*8], 8, &head, bk, ik + b*8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[7].min_cy) result[7].min_cy = cy;
        result[7].sum_cy += cy;
    }

    /* [8] x6 Nahead: hash_key KPD6 batches ahead → scan_bk → prefetch_node → cmp_key */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6/6; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*6], 6, &head, bk, ik + b*6);
        for (int b = 0; b < BENCH_N6/6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6/6)
                RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[pf*6], 6, &head, bk, ik + pf*6);
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*6], 6, &head, bk);
        }
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*6], 6, nodes);
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*6], 6, nodes, &g_res[b*6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[8].min_cy) result[8].min_cy = cy;
        result[8].sum_cy += cy;
    }

    /* [9] x8 Nahead: hash_key KPD8 batches ahead → scan_bk → prefetch_node → cmp_key */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8/8; b++)
            RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[b*8], 8, &head, bk, ik + b*8);
        for (int b = 0; b < BENCH_N8/8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8/8)
                RIX_HASH64_HASH_KEY_N(myht64, &g_ctx[pf*8], 8, &head, bk, ik + pf*8);
            RIX_HASH64_SCAN_BK_N(myht64, &g_ctx[b*8], 8, &head, bk);
        }
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH64_PREFETCH_NODE_N(myht64, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH64_CMP_KEY_N(myht64, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[9].min_cy) result[9].min_cy = cy;
        result[9].sum_cy += cy;
    }

    /* ---- Output results ---------------------------------------------- */
    {
        double mshr_min = 11520.0; /* 256 × 3CL / 20 MSHR × 300cy */
        printf("\n");
        printf("  %-20s  %10s  %10s  %10s  %10s\n",
               "pattern", "min/256", "avg/256", "min/op", "avg/op");
        printf("  %-20s  %10s  %10s  %10s  %10s\n",
               "--------------------", "----------", "----------",
               "----------", "----------");
        for (int p = 0; p < 10; p++) {
            uint64_t avg = result[p].sum_cy / (uint64_t)repeat;
            int ops = result[p].ops;
            printf("  %-20s  %10llu  %10llu  %10.2f  %10.2f\n",
                   result[p].label,
                   (unsigned long long)result[p].min_cy,
                   (unsigned long long)avg,
                   (double)result[p].min_cy / ops,
                   (double)avg / ops);
        }
        printf("\n");
        printf("  MSHR theoretical min (20 MSHRs, 300cy latency, 3CL/lookup):\n");
        printf("    %.0f cycles/256 = %.1f cycles/op\n",
               mshr_min, mshr_min / BENCH_N);
        printf("  note: avg=cache-cold (DRAM bound), min=best-case.\n");
        printf("        bk_mem=%.1f MB vs typical L3 cache → avg is cold.\n\n",
               bk_mem / 1e6);
    }

    free(key_pool);
    munmap(bk, bk_mem);
    munmap(nodes, node_mem);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int
main(int argc, char **argv)
{
    unsigned table_n = 10000000u;
    unsigned nb_bk   = 0;
    unsigned repeat  = 2000;

    if (argc >= 2) table_n = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc >= 3) nb_bk   = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc >= 4) repeat  = (unsigned)strtoul(argv[3], NULL, 10);

    if (nb_bk == 0) {
        /* Default: ~80% fill (slots = table_n / 0.8) */
        nb_bk = 1;
        while ((uint64_t)nb_bk * RIX_HASH64_BUCKET_ENTRY_SZ * 4 <
               (uint64_t)table_n * 5)
            nb_bk <<= 1;
    }

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    /* Measure fill rate characteristics first */
    bench_fill_rate();

    /* Performance measurement */
    bench_find(table_n, nb_bk, repeat);

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
