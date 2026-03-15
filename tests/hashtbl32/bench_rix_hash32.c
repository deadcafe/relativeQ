/* bench_rix_hash32.c
 *  rix_hash32.h lookup performance benchmark (32-bit key variant)
 *  Metric: CPU cycles per 256 lookups
 *
 *  Usage: ./hash32_bench [table_n [nb_bk [repeat]]]
 *    table_n : number of table entries  (default: 10,000,000)
 *    nb_bk   : number of buckets       (default: auto – ~80% fill of table_n)
 *    repeat  : number of iterations    (default: 2000)
 *
 *  Differences from the 128-bit variant:
 *    - Keys are uint32_t values directly; no pointer indirection to key data.
 *      key_pool is uint32_t[], so the HW prefetcher handles sequential access.
 *      → key prefetch stage (rix_hash_prefetch_key) is not needed/used.
 *    - Since hash_field is not stored in the node, bk_0/bk_1 placement is
 *      determined by re-hashing each slot's key.
 *    - Node size is small (8 bytes), so bk_mem must be large to ensure
 *      DRAM-cold testing (increase the number of buckets).
 *    - cmp_key only checks idx != RIX_NIL (no full key comparison) → fast.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>

#include "rix_hash32.h"

/* ================================================================== */
/* Node definition                                                     */
/* ================================================================== */
typedef struct mynode_s {
    uint32_t key;
    uint32_t val;
} mynode_t;

#define BENCH_INVALID_KEY  0xFFFFFFFFu   /* sentinel: never used as a real key */

RIX_HASH32_HEAD(myht32);
RIX_HASH32_GENERATE(myht32, mynode_t, key, BENCH_INVALID_KEY)

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
/* Benchmark constants and global buffers                              */
/* ================================================================== */
#define BENCH_N  256                   /* lookups per iteration */
#define BENCH_N6 ((BENCH_N / 6) * 6)  /* 252: for x6 batch */
#define BENCH_N8 ((BENCH_N / 8) * 8)  /* 256: for x8 batch */

/*
 * Key prefetch distance (in batches):
 * ~48 cy per hash_key batch (CRC32×8 + prefetch×8×1)
 * DRAM latency ~300 cy → 300/48 ≈ 7 batches needed → use 8/11 with margin
 */
#define KPD8  8   /* x8 Nahead: 8 batches × 8 keys = 64 keys ahead */
#define KPD6 11   /* x6 Nahead: 11 batches × 6 keys = 66 keys ahead */

static struct rix_hash32_find_ctx_s g_ctx[BENCH_N];
static mynode_t                    *g_res[BENCH_N];

/* ================================================================== */
/* xorshift64 PRNG (for key pool generation)                          */
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
/* bench_find                                                          */
/* ================================================================== */
static void
bench_find(unsigned table_n, unsigned nb_bk, unsigned repeat)
{
    /* ---- Memory estimate --------------------------------------------- */
    size_t node_mem = (size_t)table_n * sizeof(mynode_t);
    size_t bk_mem   = (size_t)nb_bk   * sizeof(struct rix_hash32_bucket_s);
    size_t pool_mem = (size_t)repeat * BENCH_N * sizeof(uint32_t);
    printf("[BENCH] table_n=%u  nb_bk=%u  slots=%u\n",
           table_n, nb_bk, nb_bk * RIX_HASH_BUCKET_ENTRY_SZ);
    printf("  memory : nodes=%.1f MB  buckets=%.1f MB"
           "  key_pool=%.1f MB  total=%.1f MB\n",
           node_mem / 1e6, bk_mem / 1e6, pool_mem / 1e6,
           (node_mem + bk_mem + pool_mem) / 1e6);

    /* ---- Node allocation and key setup ------------------------------- */
    /*
     * mmap + MADV_HUGEPAGE: reduces TLB misses.
     * Even though nodes are small (8 bytes), 2MB hugepages suppress
     * TLB misses during random access.
     */
    mynode_t *nodes = (mynode_t *)mmap(NULL, node_mem,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes == MAP_FAILED) { perror("mmap nodes"); exit(1); }
    madvise(nodes, node_mem, MADV_HUGEPAGE);

    /*
     * Keys are 1-origin sequential: no duplicates, avoids 0 (RIX_NIL).
     * Random keys via xorshift64 are also valid, but in 32-bit key space
     * the collision probability is table_n^2 / 2^33, ~1.2% for table_n=10M.
     * Sequential keys guarantee zero collisions and a known insert rate.
     */
    for (unsigned i = 0; i < table_n; i++) {
        nodes[i].key = i + 1u;   /* 1-origin: key=0 is avoided though not strictly necessary */
        nodes[i].val = i;
    }

    /* ---- Bucket allocation ------------------------------------------- */
    struct rix_hash32_bucket_s *bk =
        (struct rix_hash32_bucket_s *)mmap(NULL, bk_mem,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk == MAP_FAILED) { perror("mmap bk"); exit(1); }
    madvise(bk, bk_mem, MADV_HUGEPAGE);
    struct myht32 head;
    RIX_HASH32_INIT(myht32, &head, bk, nb_bk);

    /* ---- Insertion --------------------------------------------------- */
    printf("  inserting...\n"); fflush(stdout);
    unsigned n_hit = 0;
    unsigned report_step = table_n / 10;
    if (report_step == 0) report_step = 1;

    /*
     * Insert path measurement (32-bit variant):
     *   bk0_fast: bk_0 has a free slot → fast path to bk_0
     *   bk1_fast: bk_0 full & bk_1 has a free slot → fast path to bk_1
     *   kickout : both full → cuckoo kickout
     */
    uint64_t ins_bk0_fast = 0, ins_bk1_fast = 0, ins_kickout = 0;

    double t_ins_start = now_sec();
    for (unsigned i = 0; i < table_n; i++) {
        /* Determine insert path before inserting */
        union rix_hash_hash_u _h =
            rix_hash_arch->hash_u32((uint32_t)nodes[i].key, head.rhh_mask);
        unsigned _b0 = _h.val32[0] & head.rhh_mask;
        unsigned _b1 = _h.val32[1] & head.rhh_mask;
        uint32_t _nilm0 = rix_hash_arch->find_u32x16(bk[_b0].key,
                                                      BENCH_INVALID_KEY);
        uint32_t _nilm1 = rix_hash_arch->find_u32x16(bk[_b1].key,
                                                      BENCH_INVALID_KEY);
        if      (_nilm0) ins_bk0_fast++;
        else if (_nilm1) ins_bk1_fast++;
        else             ins_kickout++;

        if (RIX_HASH32_INSERT(myht32, &head, bk, nodes, &nodes[i]) == NULL)
            n_hit++;
        if ((i + 1) % report_step == 0) {
            printf("    %3u%%\r",
                   (unsigned)(100ULL * (i + 1) / table_n));
            fflush(stdout);
        }
    }
    double t_ins = now_sec() - t_ins_start;
    double fill = 100.0 * n_hit / ((double)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ);
    printf("  inserted : %u/%u (%.1f%% fill)  %.2f s  %.1f ns/insert\n",
           n_hit, table_n, fill, t_ins, t_ins * 1e9 / table_n);
    printf("  insert paths: bk0_fast=%" PRIu64 "  bk1_fast=%" PRIu64
           "  kickout=%" PRIu64 "\n",
           ins_bk0_fast, ins_bk1_fast, ins_kickout);

    /* ---- bk_0 hit rate measurement ----------------------------------- */
    /*
     * Scan all bucket slots to verify bk_0/bk_1 placement.
     * Since hash_field is not stored in the node, the primary bucket
     * (bk_0 = h.val32[0] & mask) is determined by re-hashing each slot's key.
     */
    {
        uint64_t in_bk0 = 0, in_bk1 = 0;
        unsigned mask = head.rhh_mask;

        for (unsigned b = 0; b < nb_bk; b++) {
            for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {
                uint32_t nidx = bk[b].idx[s];
                if (nidx == (uint32_t)RIX_NIL)
                    continue;
                uint32_t key = bk[b].key[s];
                union rix_hash_hash_u h = rix_hash_arch->hash_u32(key, mask);
                if (b == (h.val32[0] & mask))
                    in_bk0++;
                else
                    in_bk1++;
            }
        }

        uint64_t total = in_bk0 + in_bk1;
        printf("  bk_0 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%"
               "  (no bk_1 access needed during find)\n",
               in_bk0, total, 100.0 * in_bk0 / total);
        printf("  bk_1 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%"
               "  (bk_1 access required after bk_0 miss)\n",
               in_bk1, total, 100.0 * in_bk1 / total);
    }

    /* ---- Key pool generation ----------------------------------------- */
    /*
     * Generate repeat × BENCH_N uint32_t key values.
     * Unlike the 128-bit variant, keys are values directly (no pointer indirection).
     * key_pool is accessed sequentially, so the HW prefetcher handles
     * DRAM misses for key data automatically.
     * → key prefetch stage (equivalent to rix_hash_prefetch_key) is not needed.
     *
     * key_pool stores only keys of inserted nodes so all lookups hit
     * (ensuring bk_1 access on miss does not always occur).
     */
    uint32_t *key_pool = (uint32_t *)malloc(pool_mem);
    if (!key_pool) { perror("malloc key_pool"); exit(1); }

    for (size_t k = 0; k < (size_t)repeat * BENCH_N; k++) {
        unsigned idx = (unsigned)(xorshift64() % n_hit);
        key_pool[k] = nodes[idx].key;
    }

    /* ---- Warmup (instruction cache and branch predictor only) --------- */
    printf("  warmup...\n"); fflush(stdout);
    for (unsigned w = 0; w < 20 && w < repeat; w++) {
        uint32_t *wk = key_pool + (size_t)w * BENCH_N;

        /* single find */
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH32_FIND(myht32, &head, bk, nodes, wk[i]);
        /* x1 staged */
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_HASH_KEY(myht32, &g_ctx[i], &head, bk, wk[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_SCAN_BK(myht32, &g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH32_CMP_KEY(myht32, &g_ctx[i], nodes);
        /* x2 */
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_HASH_KEY2(myht32, &g_ctx[b * 2], &head, bk, wk + b * 2);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_SCAN_BK2(myht32, &g_ctx[b * 2], &head, bk);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_PREFETCH_NODE2(myht32, &g_ctx[b * 2], nodes);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_CMP_KEY2(myht32, &g_ctx[b * 2], nodes, &g_res[b * 2]);
        /* x4 */
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_HASH_KEY4(myht32, &g_ctx[b * 4], &head, bk, wk + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_SCAN_BK4(myht32, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_PREFETCH_NODE4(myht32, &g_ctx[b * 4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_CMP_KEY4(myht32, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        /* x6 */
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 6], 6, &head, bk, wk + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        /* x8 */
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 8], 8, &head, bk, wk + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        /* x8 Nahead warmup */
        for (int b = 0; b < KPD8 && b < BENCH_N8/8; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b*8], 8, &head, bk, wk + b*8);
        for (int b = 0; b < BENCH_N8/8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8/8)
                RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[pf*8], 8, &head, bk, wk + pf*8);
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b*8], 8, &head, bk);
        }
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        /* x6 Nahead warmup */
        for (int b = 0; b < KPD6 && b < BENCH_N6/6; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b*6], 6, &head, bk, wk + b*6);
        for (int b = 0; b < BENCH_N6/6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6/6)
                RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[pf*6], 6, &head, bk, wk + pf*6);
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b*6], 6, &head, bk);
        }
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b*6], 6, nodes);
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b*6], 6, nodes, &g_res[b*6]);
    }

    /* ---- Measurement ------------------------------------------------- */
    /*
     * Measure each pattern in an independent loop.
     * Unlike the 128-bit variant, there is no key prefetch stage.
     * Instead, compare the effect of bk+node staging across x1/x2/x4/x6/x8.
     *
     * MSHR utilization efficiency (3 CL/lookup: bk_0 CL0 + CL1 + node):
     *   x1: 3 CL in flight
     *   x2: 6 CL in flight
     *   x4: 12 CL in flight
     *   x6: 18 CL in flight (Core/Ryzen MSHR~20: 90%)
     *   x8: 24 CL in flight (Xeon SP   MSHR~32: 75%)
     */
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

    /* ---- [0] Sequential find ----------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH32_FIND(myht32, &head, bk, nodes, ik[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[0].min_cy) result[0].min_cy = cy;
        result[0].sum_cy += cy;
    }

    /* ---- [1] x1: hash_key → scan_bk → cmp_key (node not prefetched) -- */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_HASH_KEY(myht32, &g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_SCAN_BK(myht32, &g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH32_CMP_KEY(myht32, &g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[1].min_cy) result[1].min_cy = cy;
        result[1].sum_cy += cy;
    }

    /* ---- [2] x1: hash_key → scan_bk → prefetch_node → cmp_key --- */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_HASH_KEY(myht32, &g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_SCAN_BK(myht32, &g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            RIX_HASH32_PREFETCH_NODE(myht32, &g_ctx[i], nodes);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = RIX_HASH32_CMP_KEY(myht32, &g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[2].min_cy) result[2].min_cy = cy;
        result[2].sum_cy += cy;
    }

    /* ---- [3] x2: hash_key2 → scan_bk2 → prefetch_node2 → cmp_key2 */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_HASH_KEY2(myht32, &g_ctx[b * 2], &head, bk, ik + b * 2);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_SCAN_BK2(myht32, &g_ctx[b * 2], &head, bk);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_PREFETCH_NODE2(myht32, &g_ctx[b * 2], nodes);
        for (int b = 0; b < BENCH_N / 2; b++)
            RIX_HASH32_CMP_KEY2(myht32, &g_ctx[b * 2], nodes, &g_res[b * 2]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[3].min_cy) result[3].min_cy = cy;
        result[3].sum_cy += cy;
    }

    /* ---- [4] x4: hash_key4 → scan_bk4 → cmp_key4 (node not prefetched) */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_HASH_KEY4(myht32, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_SCAN_BK4(myht32, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_CMP_KEY4(myht32, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[4].min_cy) result[4].min_cy = cy;
        result[4].sum_cy += cy;
    }

    /* ---- [5] x4: hash_key4 → scan_bk4 → prefetch_node4 → cmp_key4 */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_HASH_KEY4(myht32, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_SCAN_BK4(myht32, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_PREFETCH_NODE4(myht32, &g_ctx[b * 4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            RIX_HASH32_CMP_KEY4(myht32, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[5].min_cy) result[5].min_cy = cy;
        result[5].sum_cy += cy;
    }

    /* ---- [6] x6: hash_key6 → scan_bk6 → prefetch_node6 → cmp_key6 */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 6], 6, &head, bk, ik + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[6].min_cy) result[6].min_cy = cy;
        result[6].sum_cy += cy;
    }

    /* ---- [7] x8: hash_key8 → scan_bk8 → prefetch_node8 → cmp_key8 */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 8], 8, &head, bk, ik + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[7].min_cy) result[7].min_cy = cy;
        result[7].sum_cy += cy;
    }

    /* ---- [8] x6 Nahead ----------------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6/6; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b*6], 6, &head, bk, ik + b*6);
        for (int b = 0; b < BENCH_N6/6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6/6)
                RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[pf*6], 6, &head, bk, ik + pf*6);
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b*6], 6, &head, bk);
        }
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b*6], 6, nodes);
        for (int b = 0; b < BENCH_N6/6; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b*6], 6, nodes, &g_res[b*6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[8].min_cy) result[8].min_cy = cy;
        result[8].sum_cy += cy;
    }

    /* ---- [9] x8 Nahead ----------------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8/8; b++)
            RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[b*8], 8, &head, bk, ik + b*8);
        for (int b = 0; b < BENCH_N8/8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8/8)
                RIX_HASH32_HASH_KEY_N(myht32, &g_ctx[pf*8], 8, &head, bk, ik + pf*8);
            RIX_HASH32_SCAN_BK_N(myht32, &g_ctx[b*8], 8, &head, bk);
        }
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8/8; b++)
            RIX_HASH32_CMP_KEY_N(myht32, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[9].min_cy) result[9].min_cy = cy;
        result[9].sum_cy += cy;
    }

    /* ---- Output results ---------------------------------------------- */
    {
        /*
         * MSHR theoretical minimum (after bk_0-only optimization):
         *   256 lookup × 3 CL (bk_0 CL0 + CL1 + node) = 768 fetches
         *   bk_1 only on bk_0 miss (~80% fill → rare)
         *   MSHR capacity ~20, DRAM latency ~300 cycles
         *   → min: 768 / 20 × 300 = 11520 cycles/256 = 45 cycles/op
         *
         * Comparison points vs 128-bit variant:
         *   - hash_fn is cheaper (CRC32 of 32-bit vs 128-bit)
         *   - cmp_key only checks idx != NIL (no full key comparison)
         *   - smaller node (8 bytes) → higher hit rate per node CL
         */
        double mshr_min = 11520.0;
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
        printf("  note: avg reflects steady-state DRAM-bound throughput;\n");
        printf("        min reflects best-case (partially cache-warm) throughput.\n");
        printf("        bk_mem=%.1f MB vs typical L3 cache → avg is cache-cold.\n\n",
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
    unsigned table_n = 10000000u; /* 10M: realistic flow-table size */
    unsigned nb_bk   = 0;
    unsigned repeat  = 2000;

    if (argc >= 2) table_n = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc >= 3) nb_bk   = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc >= 4) repeat  = (unsigned)strtoul(argv[3], NULL, 10);

    if (nb_bk == 0) {
        /* Default: target ~80% fill (slots = table_n / 0.8) */
        nb_bk = 1;
        while ((uint64_t)nb_bk * RIX_HASH_BUCKET_ENTRY_SZ * 4 <
               (uint64_t)table_n * 5)
            nb_bk <<= 1;
    }

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);
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
