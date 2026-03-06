/* bench_rel_hash32.c
 *  rel_hash32.h 検索性能ベンチマーク (32-bit key 版)
 *  指標: 256 検索あたりの CPU cycle 数
 *
 *  Usage: ./hash32_bench [table_n [nb_bk [repeat]]]
 *    table_n : テーブル登録数  (default: 10,000,000)
 *    nb_bk   : バケット数      (default: 自動 – table_n を ~80% 充填)
 *    repeat  : 計測繰り返し数  (default: 2000)
 *
 *  128-bit 版との相違点:
 *    - キーは uint32_t 値そのものであり、キーデータへのポインタ間接参照が不要。
 *      key_pool は uint32_t[] であり、HW prefetcher が順次アクセスを処理する。
 *      → key prefetch stage (rel_hash_prefetch_key) は不要・不採用。
 *    - hash_field がノードに存在しないため、bk_0/bk_1 配置の確認は
 *      各スロットのキーを再ハッシュして判定する。
 *    - ノードサイズが 8 bytes と小さいため、DRAM-cold テストのために
 *      bk_mem を大きく設定する (バケット数を増やす)。
 *    - cmp_key は idx != REL_NIL の確認のみ (フルキー比較なし) → 高速。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>

#include "rel_hash32.h"

/* ================================================================== */
/* ノード定義                                                          */
/* ================================================================== */
typedef struct mynode_s {
    uint32_t key;
    uint32_t val;
} mynode_t;

#define BENCH_INVALID_KEY  0xFFFFFFFFu   /* sentinel: never used as a real key */

REL_HASH32_HEAD(myht32);
REL_HASH32_GENERATE(myht32, mynode_t, key, BENCH_INVALID_KEY)

/* ================================================================== */
/* TSC 計測ヘルパ                                                      */
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
/* ベンチマーク定数・グローバルバッファ                                */
/* ================================================================== */
#define BENCH_N  256                   /* 1 イテレーションの検索数 */
#define BENCH_N6 ((BENCH_N / 6) * 6)  /* 252: x6 バッチ用 */
#define BENCH_N8 ((BENCH_N / 8) * 8)  /* 256: x8 バッチ用 */

/*
 * 先行プリフェッチ距離 (バッチ数):
 * hash_key 1バッチあたり ~48 cy (CRC32×8 + prefetch×8×1)
 * DRAM latency ~300 cy → 300/48 ≈ 7 バッチ必要 → 余裕を持って 8/11
 */
#define KPD8  8   /* x8 Nahead: 8 batches × 8 keys = 64 keys 先行 */
#define KPD6 11   /* x6 Nahead: 11 batches × 6 keys = 66 keys 先行 */

static struct rel_hash32_find_ctx_s g_ctx[BENCH_N];
static mynode_t                    *g_res[BENCH_N];

/* ================================================================== */
/* xorshift64 PRNG (キープール生成用)                                  */
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
    /* ---- メモリ見積もり ------------------------------------------- */
    size_t node_mem = (size_t)table_n * sizeof(mynode_t);
    size_t bk_mem   = (size_t)nb_bk   * sizeof(struct rel_hash32_bucket_s);
    size_t pool_mem = (size_t)repeat * BENCH_N * sizeof(uint32_t);
    printf("[BENCH] table_n=%u  nb_bk=%u  slots=%u\n",
           table_n, nb_bk, nb_bk * REL_HASH_BUCKET_ENTRY_SZ);
    printf("  memory : nodes=%.1f MB  buckets=%.1f MB"
           "  key_pool=%.1f MB  total=%.1f MB\n",
           node_mem / 1e6, bk_mem / 1e6, pool_mem / 1e6,
           (node_mem + bk_mem + pool_mem) / 1e6);

    /* ---- ノード確保・キー設定 ------------------------------------- */
    /*
     * mmap + MADV_HUGEPAGE: TLB ミスを削減する。
     * ノードは 8 bytes と小さいが、ランダムアクセス時の TLB miss を
     * 2MB hugepage で抑制する。
     */
    mynode_t *nodes = (mynode_t *)mmap(NULL, node_mem,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes == MAP_FAILED) { perror("mmap nodes"); exit(1); }
    madvise(nodes, node_mem, MADV_HUGEPAGE);

    /*
     * キーは 1-origin 連番: 重複なし、0 (REL_NIL) を避ける。
     * xorshift64 で生成したランダムキーでも可だが、32-bit 空間の
     * 衝突確率は table_n^2 / 2^33 で、table_n=10M では ~1.2% 程度。
     * 連番は衝突ゼロで挿入率を保証する。
     */
    for (unsigned i = 0; i < table_n; i++) {
        nodes[i].key = i + 1u;   /* 1-origin: key=0 は空きスロットと区別不要だが回避 */
        nodes[i].val = i;
    }

    /* ---- バケット確保 --------------------------------------------- */
    struct rel_hash32_bucket_s *bk =
        (struct rel_hash32_bucket_s *)mmap(NULL, bk_mem,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk == MAP_FAILED) { perror("mmap bk"); exit(1); }
    madvise(bk, bk_mem, MADV_HUGEPAGE);
    struct myht32 head;
    REL_HASH32_INIT(myht32, &head, bk, nb_bk);

    /* ---- 挿入 ----------------------------------------------------- */
    printf("  inserting...\n"); fflush(stdout);
    unsigned n_hit = 0;
    unsigned report_step = table_n / 10;
    if (report_step == 0) report_step = 1;

    /*
     * 挿入経路の計測 (32-bit 版):
     *   bk0_fast: bk_0 に空きあり → fast path で bk_0 へ
     *   bk1_fast: bk_0 満杯 & bk_1 に空きあり → fast path で bk_1 へ
     *   kickout : 両方満杯 → cuckoo kickout
     */
    uint64_t ins_bk0_fast = 0, ins_bk1_fast = 0, ins_kickout = 0;

    double t_ins_start = now_sec();
    for (unsigned i = 0; i < table_n; i++) {
        /* 挿入前に経路を判定 */
        union rel_hash_hash_u _h =
            _rel_hash32_fn((uint32_t)nodes[i].key, head.rhh_mask);
        unsigned _b0 = _h.val32[0] & head.rhh_mask;
        unsigned _b1 = _h.val32[1] & head.rhh_mask;
        uint32_t _nilm0 = rel_hash_arch->find_u32x16(bk[_b0].key,
                                                      BENCH_INVALID_KEY);
        uint32_t _nilm1 = rel_hash_arch->find_u32x16(bk[_b1].key,
                                                      BENCH_INVALID_KEY);
        if      (_nilm0) ins_bk0_fast++;
        else if (_nilm1) ins_bk1_fast++;
        else             ins_kickout++;

        if (REL_HASH32_INSERT(myht32, &head, bk, nodes, &nodes[i]) == NULL)
            n_hit++;
        if ((i + 1) % report_step == 0) {
            printf("    %3u%%\r",
                   (unsigned)(100ULL * (i + 1) / table_n));
            fflush(stdout);
        }
    }
    double t_ins = now_sec() - t_ins_start;
    double fill = 100.0 * n_hit / ((double)nb_bk * REL_HASH_BUCKET_ENTRY_SZ);
    printf("  inserted : %u/%u (%.1f%% fill)  %.2f s  %.1f ns/insert\n",
           n_hit, table_n, fill, t_ins, t_ins * 1e9 / table_n);
    printf("  insert paths: bk0_fast=%" PRIu64 "  bk1_fast=%" PRIu64
           "  kickout=%" PRIu64 "\n",
           ins_bk0_fast, ins_bk1_fast, ins_kickout);

    /* ---- bk_0 hit rate 計測 --------------------------------------- */
    /*
     * 全バケットスロットをスキャンして bk_0/bk_1 配置を確認。
     * hash_field がノードに存在しないため、各スロットのキーを
     * 再ハッシュして primary bucket (bk_0 = h.val32[0] & mask) を確認する。
     */
    {
        uint64_t in_bk0 = 0, in_bk1 = 0;
        unsigned mask = head.rhh_mask;

        for (unsigned b = 0; b < nb_bk; b++) {
            for (unsigned s = 0; s < REL_HASH_BUCKET_ENTRY_SZ; s++) {
                uint32_t nidx = bk[b].idx[s];
                if (nidx == (uint32_t)REL_NIL)
                    continue;
                uint32_t key = bk[b].key[s];
                union rel_hash_hash_u h = _rel_hash32_fn(key, mask);
                if (b == (h.val32[0] & mask))
                    in_bk0++;
                else
                    in_bk1++;
            }
        }

        uint64_t total = in_bk0 + in_bk1;
        printf("  bk_0 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%"
               "  (find 時 bk_1 アクセス不要)\n",
               in_bk0, total, 100.0 * in_bk0 / total);
        printf("  bk_1 hit rate : %" PRIu64 "/%" PRIu64 " = %.2f%%"
               "  (bk_0 ミス後に bk_1 参照が必要)\n",
               in_bk1, total, 100.0 * in_bk1 / total);
    }

    /* ---- キープール生成 ------------------------------------------- */
    /*
     * repeat × BENCH_N 個の uint32_t キー値を生成。
     * 128-bit 版と異なり、キーは値そのもの (ポインタ間接参照なし)。
     * key_pool は連続メモリへの順次アクセスとなり、HW prefetcher が
     * キーデータの DRAM ミスを自動的に処理する。
     * → key prefetch stage (rel_hash_prefetch_key 相当) は不要。
     *
     * key_pool には挿入済みノードのキーのみを格納し、全検索がヒットする
     * ように設計する (miss 時の bk_1 アクセスが常に発生しないようにする)。
     */
    uint32_t *key_pool = (uint32_t *)malloc(pool_mem);
    if (!key_pool) { perror("malloc key_pool"); exit(1); }

    for (size_t k = 0; k < (size_t)repeat * BENCH_N; k++) {
        unsigned idx = (unsigned)(xorshift64() % n_hit);
        key_pool[k] = nodes[idx].key;
    }

    /* ---- ウォームアップ (命令キャッシュ・分岐予測器のみ) ---------- */
    printf("  warmup...\n"); fflush(stdout);
    for (unsigned w = 0; w < 20 && w < repeat; w++) {
        uint32_t *wk = key_pool + (size_t)w * BENCH_N;

        /* single find */
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = REL_HASH32_FIND(myht32, &head, bk, nodes, wk[i]);
        /* x1 staged */
        for (int i = 0; i < BENCH_N; i++)
            REL_HASH32_HASH_KEY(myht32, &g_ctx[i], &head, bk, wk[i]);
        for (int i = 0; i < BENCH_N; i++)
            REL_HASH32_SCAN_BK(myht32, &g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = REL_HASH32_CMP_KEY(myht32, &g_ctx[i], nodes);
        /* x2 */
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH32_HASH_KEY2(myht32, &g_ctx[b * 2], &head, bk, wk + b * 2);
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH32_SCAN_BK2(myht32, &g_ctx[b * 2], &head, bk);
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH32_PREFETCH_NODE2(myht32, &g_ctx[b * 2], nodes);
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH32_CMP_KEY2(myht32, &g_ctx[b * 2], nodes, &g_res[b * 2]);
        /* x4 */
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_HASH_KEY4(myht32, &g_ctx[b * 4], &head, bk, wk + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_SCAN_BK4(myht32, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_PREFETCH_NODE4(myht32, &g_ctx[b * 4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_CMP_KEY4(myht32, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        /* x6 */
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 6], 6, &head, bk, wk + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        /* x8 */
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 8], 8, &head, bk, wk + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        /* x8 Nahead warmup */
        for (int b = 0; b < KPD8 && b < BENCH_N8/8; b++)
            REL_HASH32_HASH_KEY_N(myht32, &g_ctx[b*8], 8, &head, bk, wk + b*8);
        for (int b = 0; b < BENCH_N8/8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8/8)
                REL_HASH32_HASH_KEY_N(myht32, &g_ctx[pf*8], 8, &head, bk, wk + pf*8);
            REL_HASH32_SCAN_BK_N(myht32, &g_ctx[b*8], 8, &head, bk);
        }
        for (int b = 0; b < BENCH_N8/8; b++)
            REL_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8/8; b++)
            REL_HASH32_CMP_KEY_N(myht32, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        /* x6 Nahead warmup */
        for (int b = 0; b < KPD6 && b < BENCH_N6/6; b++)
            REL_HASH32_HASH_KEY_N(myht32, &g_ctx[b*6], 6, &head, bk, wk + b*6);
        for (int b = 0; b < BENCH_N6/6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6/6)
                REL_HASH32_HASH_KEY_N(myht32, &g_ctx[pf*6], 6, &head, bk, wk + pf*6);
            REL_HASH32_SCAN_BK_N(myht32, &g_ctx[b*6], 6, &head, bk);
        }
        for (int b = 0; b < BENCH_N6/6; b++)
            REL_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b*6], 6, nodes);
        for (int b = 0; b < BENCH_N6/6; b++)
            REL_HASH32_CMP_KEY_N(myht32, &g_ctx[b*6], 6, nodes, &g_res[b*6]);
    }

    /* ---- 計測 ----------------------------------------------------- */
    /*
     * 各パターンを独立ループで測定する。
     * 128-bit 版と異なり key prefetch stage は存在しない。
     * 代わりに bk+node staging の効果を x1/x2/x4/x6/x8 で比較する。
     *
     * MSHR 利用効率 (3 CL/lookup: bk_0 CL0 + CL1 + node):
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

    /* ---- [0] 逐次 find ------------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = REL_HASH32_FIND(myht32, &head, bk, nodes, ik[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[0].min_cy) result[0].min_cy = cy;
        result[0].sum_cy += cy;
    }

    /* ---- [1] x1: hash_key → scan_bk → cmp_key (node 未 prefetch) - */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            REL_HASH32_HASH_KEY(myht32, &g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            REL_HASH32_SCAN_BK(myht32, &g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = REL_HASH32_CMP_KEY(myht32, &g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[1].min_cy) result[1].min_cy = cy;
        result[1].sum_cy += cy;
    }

    /* ---- [2] x1: hash_key → scan_bk → prefetch_node → cmp_key --- */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            REL_HASH32_HASH_KEY(myht32, &g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            REL_HASH32_SCAN_BK(myht32, &g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            REL_HASH32_PREFETCH_NODE(myht32, &g_ctx[i], nodes);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = REL_HASH32_CMP_KEY(myht32, &g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[2].min_cy) result[2].min_cy = cy;
        result[2].sum_cy += cy;
    }

    /* ---- [3] x2: hash_key2 → scan_bk2 → prefetch_node2 → cmp_key2 */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH32_HASH_KEY2(myht32, &g_ctx[b * 2], &head, bk, ik + b * 2);
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH32_SCAN_BK2(myht32, &g_ctx[b * 2], &head, bk);
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH32_PREFETCH_NODE2(myht32, &g_ctx[b * 2], nodes);
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH32_CMP_KEY2(myht32, &g_ctx[b * 2], nodes, &g_res[b * 2]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[3].min_cy) result[3].min_cy = cy;
        result[3].sum_cy += cy;
    }

    /* ---- [4] x4: hash_key4 → scan_bk4 → cmp_key4 (node 未 prefetch) */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_HASH_KEY4(myht32, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_SCAN_BK4(myht32, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_CMP_KEY4(myht32, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[4].min_cy) result[4].min_cy = cy;
        result[4].sum_cy += cy;
    }

    /* ---- [5] x4: hash_key4 → scan_bk4 → prefetch_node4 → cmp_key4 */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_HASH_KEY4(myht32, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_SCAN_BK4(myht32, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_PREFETCH_NODE4(myht32, &g_ctx[b * 4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH32_CMP_KEY4(myht32, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[5].min_cy) result[5].min_cy = cy;
        result[5].sum_cy += cy;
    }

    /* ---- [6] x6: hash_key6 → scan_bk6 → prefetch_node6 → cmp_key6 */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 6], 6, &head, bk, ik + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[6].min_cy) result[6].min_cy = cy;
        result[6].sum_cy += cy;
    }

    /* ---- [7] x8: hash_key8 → scan_bk8 → prefetch_node8 → cmp_key8 */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH32_HASH_KEY_N(myht32, &g_ctx[b * 8], 8, &head, bk, ik + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH32_SCAN_BK_N(myht32, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b * 8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH32_CMP_KEY_N(myht32, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[7].min_cy) result[7].min_cy = cy;
        result[7].sum_cy += cy;
    }

    /* ---- [8] x6 Nahead ----------------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6/6; b++)
            REL_HASH32_HASH_KEY_N(myht32, &g_ctx[b*6], 6, &head, bk, ik + b*6);
        for (int b = 0; b < BENCH_N6/6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6/6)
                REL_HASH32_HASH_KEY_N(myht32, &g_ctx[pf*6], 6, &head, bk, ik + pf*6);
            REL_HASH32_SCAN_BK_N(myht32, &g_ctx[b*6], 6, &head, bk);
        }
        for (int b = 0; b < BENCH_N6/6; b++)
            REL_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b*6], 6, nodes);
        for (int b = 0; b < BENCH_N6/6; b++)
            REL_HASH32_CMP_KEY_N(myht32, &g_ctx[b*6], 6, nodes, &g_res[b*6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[8].min_cy) result[8].min_cy = cy;
        result[8].sum_cy += cy;
    }

    /* ---- [9] x8 Nahead ----------------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        uint32_t *ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8/8; b++)
            REL_HASH32_HASH_KEY_N(myht32, &g_ctx[b*8], 8, &head, bk, ik + b*8);
        for (int b = 0; b < BENCH_N8/8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8/8)
                REL_HASH32_HASH_KEY_N(myht32, &g_ctx[pf*8], 8, &head, bk, ik + pf*8);
            REL_HASH32_SCAN_BK_N(myht32, &g_ctx[b*8], 8, &head, bk);
        }
        for (int b = 0; b < BENCH_N8/8; b++)
            REL_HASH32_PREFETCH_NODE_N(myht32, &g_ctx[b*8], 8, nodes);
        for (int b = 0; b < BENCH_N8/8; b++)
            REL_HASH32_CMP_KEY_N(myht32, &g_ctx[b*8], 8, nodes, &g_res[b*8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[9].min_cy) result[9].min_cy = cy;
        result[9].sum_cy += cy;
    }

    /* ---- 結果出力 -------------------------------------------------- */
    {
        /*
         * MSHR 理論最小値 (bk_0-only 最適化後):
         *   256 lookup × 3 CL (bk_0 CL0 + CL1 + node) = 768 fetches
         *   bk_1 は bk_0 ミス時のみ (充填率 ~80% では少数)
         *   MSHR capacity ~20, DRAM latency ~300 cycles
         *   → min: 768 / 20 × 300 = 11520 cycles/256 = 45 cycles/op
         *
         * 128-bit 版との比較ポイント:
         *   - hash_fn はより安価 (CRC32 of 32-bit vs 128-bit)
         *   - cmp_key は idx != NIL チェックのみ (フルキー比較なし)
         *   - ノードが小さい (8 bytes) → node CL 当たりのヒット率が高い
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
        /* デフォルト: ~80% 充填を目標 (slots = table_n / 0.8) */
        nb_bk = 1;
        while ((uint64_t)nb_bk * REL_HASH_BUCKET_ENTRY_SZ * 4 <
               (uint64_t)table_n * 5)
            nb_bk <<= 1;
    }

    rel_hash_arch_init();
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
