/* bench_rel_hash.c
 *  rel_hash.h 検索性能ベンチマーク
 *  指標: 256 検索あたりの CPU cycle 数
 *
 *  Usage: ./hash_bench [table_n [nb_bk [repeat]]]
 *    table_n : テーブル登録数  (default: 100,000,000)
 *    nb_bk   : バケット数      (default: 自動 – table_n を ~50% 充填)
 *    repeat  : 計測繰り返し数  (default: 2000)
 *
 *  キャッシュコールド計測:
 *    repeat × 256 個のランダムキーを事前生成し、各イテレーションで
 *    異なるバケットにアクセスする。バケット総量 (≈2GB) >> L3 キャッシュ
 *    のため、大半のアクセスが DRAM ミスになる。
 *    各パターンは独立ループで実行し、前パターンが thrash したキャッシュ
 *    状態から開始する。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/mman.h>

#include "rel_hash.h"

/* ================================================================== */
/* ノード定義・ハッシュ関数                                            */
/* ================================================================== */
struct mykey {
    uint64_t hi;
};

struct mynode {
    uint32_t     cur_hash; /* hash_field: hash of the current bucket (updated on kickout) */
    uint32_t     _pad;     /* alignment padding */
    struct mykey key;      /* key_field: 8-byte key */
};

/* 8-byte key comparison */
static REL_FORCE_INLINE int
mykey_cmp(const void *a, const void *b)
{
    return *(const uint64_t *)a == *(const uint64_t *)b;
}

REL_HASH_HEAD(myht);
REL_HASH_GENERATE(myht, mynode, key, cur_hash, mykey_cmp)

/* ================================================================== */
/* nohf variant: no hash_field in node (8-byte node)                  */
/* ================================================================== */
struct mynode_nohf {
    struct mykey key;        /* key at offset 0, no hash_field        */
};

REL_HASH_HEAD(myht_nohf);
REL_HASH_GENERATE_NOHF(myht_nohf, mynode_nohf, key, mykey_cmp)

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
#define BENCH_N  256                   /* 1イテレーションの検索数 */
#define BENCH_N6 ((BENCH_N / 6) * 6)  /* 252: x6バッチ用 (256÷6の切り捨て) */
#define BENCH_N8 ((BENCH_N / 8) * 8)  /* 256: x8バッチ用 */

/* キー先行 prefetch 距離 (バッチ数): ~64 keys ahead ≒ 1 DRAM latency */
#define KPD4  16  /* 16 batches × 4 = 64 keys */
#define KPD6  11  /* 11 batches × 6 = 66 keys */
#define KPD8   8  /*  8 batches × 8 = 64 keys */

static struct rel_hash_find_ctx_s g_ctx[BENCH_N];
static struct mynode             *g_res[BENCH_N];
static struct mynode_nohf        *g_res_nohf[BENCH_N];

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
bench_find(unsigned table_n, unsigned nb_bk, unsigned repeat, int rand_keys)
{
    /* ---- メモリ見積もり ------------------------------------------- */
    size_t node_mem      = (size_t)table_n * sizeof(struct mynode);
    size_t node_nohf_mem = (size_t)table_n * sizeof(struct mynode_nohf);
    size_t bk_mem        = (size_t)nb_bk   * sizeof(struct rel_hash_bucket_s);
    size_t pool_mem      = (size_t)repeat * BENCH_N * sizeof(void *);
    printf("[BENCH] table_n=%u  nb_bk=%u  slots=%u  keys=%s\n",
           table_n, nb_bk, nb_bk * REL_HASH_BUCKET_ENTRY_SZ,
           rand_keys ? "random" : "sequential");
    printf("  memory : nodes=%.1f MB  buckets=%.1f MB"
           "  key_pool=%.1f MB  total=%.1f MB\n",
           node_mem / 1e6, bk_mem / 1e6, pool_mem / 1e6,
           (node_mem + bk_mem + pool_mem) / 1e6);
    printf("  nohf   : nodes=%.1f MB  buckets=%.1f MB"
           "  (node: %zu B vs %zu B)\n",
           node_nohf_mem / 1e6, bk_mem / 1e6,
           sizeof(struct mynode_nohf), sizeof(struct mynode));

    /* ---- ノード確保・キー設定 ------------------------------------- */
    /*
     * mmap + MADV_HUGEPAGE: TLB ミスを削減する。
     * 4KB ページ時、1.6GB をランダムアクセスすると L2 TLB (1536 entries) を
     * 大幅に超え、各アクセスにページウォーク (~20-100 cycles) が加算される。
     * 2MB hugepage では必要エントリ数が 1/512 に減り TLB miss がほぼ消える。
     */
    struct mynode *nodes = (struct mynode *)mmap(NULL, node_mem,
                                       PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes == MAP_FAILED) { perror("mmap nodes"); exit(1); }
    madvise(nodes, node_mem, MADV_HUGEPAGE);
    if (rand_keys) {
        /* ランダムキー: xorshift64 で 128-bit key を生成 */
        for (unsigned i = 0; i < table_n; i++) {
            nodes[i].key.hi = xorshift64();
        }
    } else {
        /* 連番キー: CRC32C の均一分布を示すためのリファレンス */
        for (unsigned i = 0; i < table_n; i++) {
            nodes[i].key.hi = (uint64_t)(i + 1);
        }
    }

    /* ---- バケット確保 --------------------------------------------- */
    struct rel_hash_bucket_s *bk =
        (struct rel_hash_bucket_s *)mmap(NULL, bk_mem,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk == MAP_FAILED) { perror("mmap bk"); exit(1); }
    madvise(bk, bk_mem, MADV_HUGEPAGE);
    memset(bk, 0, bk_mem);

    struct myht head;
    REL_HASH_INIT(myht, &head, nb_bk);

    /* ---- nohf ノード・バケット確保 -------------------------------- */
    struct mynode_nohf *nodes_nohf =
        (struct mynode_nohf *)mmap(NULL, node_nohf_mem,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (nodes_nohf == MAP_FAILED) { perror("mmap nodes_nohf"); exit(1); }
    madvise(nodes_nohf, node_nohf_mem, MADV_HUGEPAGE);
    if (rand_keys) {
        for (unsigned i = 0; i < table_n; i++)
            nodes_nohf[i].key.hi = xorshift64();
    } else {
        for (unsigned i = 0; i < table_n; i++)
            nodes_nohf[i].key.hi = (uint64_t)(i + 1);
    }

    struct rel_hash_bucket_s *bk_nohf =
        (struct rel_hash_bucket_s *)mmap(NULL, bk_mem,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bk_nohf == MAP_FAILED) { perror("mmap bk_nohf"); exit(1); }
    madvise(bk_nohf, bk_mem, MADV_HUGEPAGE);
    memset(bk_nohf, 0, bk_mem);

    struct myht_nohf head_nohf;
    REL_HASH_INIT(myht_nohf, &head_nohf, nb_bk);

    /* ---- 挿入 ----------------------------------------------------- */
    printf("  inserting (nohf)...\n"); fflush(stdout);
    for (unsigned i = 0; i < table_n; i++) {
        myht_nohf_insert(&head_nohf, bk_nohf, nodes_nohf, &nodes_nohf[i]);
        if ((i + 1) % (table_n / 10) == 0) {
            printf("    %3u%%\r", (unsigned)(100ULL * (i + 1) / table_n));
            fflush(stdout);
        }
    }
    printf("  nohf inserted : %u entries\n", head_nohf.rhh_nb);

    printf("  inserting...\n"); fflush(stdout);
    unsigned n_hit = 0;
    unsigned report_step = table_n / 10;
    if (report_step == 0) report_step = 1;

    /*
     * 挿入経路の計測:
     *   bk0_fast : bk_0 に空きあり → fast path で bk_0 へ
     *   bk1_fast : bk_0 満杯 & bk_1 空きあり → fast path で bk_1 へ
     *   kickout  : 両方満杯 → cuckoo kickout (新 entry は常に bk_0 へ)
     * ※ 重複チェックは事前に行われるが、このベンチは重複なし
     */
    uint64_t ins_bk0_fast = 0, ins_bk1_fast = 0, ins_kickout = 0;

    double t_ins_start = now_sec();
    for (unsigned i = 0; i < table_n; i++) {
        /* 挿入前に経路を判定 */
        union rel_hash_hash_u _h =
            _rel_hash_fn_crc32(&nodes[i].key, sizeof(nodes[i].key),
                               head.rhh_mask);
        unsigned _b0 = _h.val32[0] & head.rhh_mask;
        unsigned _b1 = _h.val32[1] & head.rhh_mask;
        uint32_t _nilm0 = rel_hash_arch->find_u32x16(bk[_b0].idx,
                                                      (uint32_t)REL_NIL);
        uint32_t _nilm1 = rel_hash_arch->find_u32x16(bk[_b1].idx,
                                                      (uint32_t)REL_NIL);
        if      (_nilm0) ins_bk0_fast++;
        else if (_nilm1) ins_bk1_fast++;
        else             ins_kickout++;

        if (myht_insert(&head, bk, nodes, &nodes[i]) == NULL)
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
     * 全バケットスロットをスキャンして占有スロット数と bk_0/bk_1 配置を確認。
     * node->hash_field & mask == current_bucket (new design invariant) を
     * 各スロットで検証し、bk_0 (primary) と bk_1 (secondary) の分布を計測。
     *
     * insert が fail したノードにも hash_field が書き込まれるため、
     * ノード配列のスキャンではなくバケットスロットを直接スキャンする。
     */
    {
        uint64_t in_bk0 = 0, in_bk1 = 0;
        unsigned mask = head.rhh_mask;

        for (unsigned b = 0; b < nb_bk; b++) {
            for (unsigned s = 0; s < REL_HASH_BUCKET_ENTRY_SZ; s++) {
                uint32_t nidx = bk[b].idx[s];
                if (nidx == (uint32_t)REL_NIL)
                    continue;
                struct mynode *node = &nodes[nidx - 1u]; /* 1-origin */
                /* Verify invariant: node->cur_hash & mask == b */
                if ((node->cur_hash & mask) != b) {
                    printf("  [BUG] slot b=%u s=%u: cur_hash&mask=%u\n",
                           b, s, node->cur_hash & mask);
                }
                /* Determine primary bucket via re-hash */
                union rel_hash_hash_u h =
                    _rel_hash_fn_crc32(&node->key, sizeof(node->key), mask);
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
     * repeat × BENCH_N 個のランダムキーポインタを生成。
     * 各イテレーションは BENCH_N 個の互いに異なる（ランダム）バケットを
     * アクセスするため、バケット総量 (bk_mem) >> L3 キャッシュ のとき、
     * 大半のアクセスが DRAM レベルのキャッシュミスになる。
     */
    size_t pool_len = (size_t)repeat * BENCH_N;
    const struct mykey **key_pool = (const struct mykey **)malloc(pool_mem);
    if (!key_pool) { perror("malloc key_pool"); exit(1); }

    for (size_t k = 0; k < pool_len; k++) {
        unsigned idx = (unsigned)(xorshift64() % table_n);
        key_pool[k] = &nodes[idx].key;
    }

    /* ---- nohf キープール生成 --------------------------------------- */
    const struct mykey **key_pool_nohf = (const struct mykey **)malloc(pool_mem);
    if (!key_pool_nohf) { perror("malloc key_pool_nohf"); exit(1); }
    for (size_t k = 0; k < pool_len; k++) {
        unsigned idx = (unsigned)(xorshift64() % table_n);
        key_pool_nohf[k] = &nodes_nohf[idx].key;
    }

    /* ---- ウォームアップ (命令キャッシュ・分岐予測器のみ) ---------- */
    /*
     * 最初の 20 イテレーション分 (key_pool[0..20*BENCH_N-1]) で
     * 内部ループの命令を事前に実行する。バケットデータは大量のため
     * この程度では L3 に残らない。
     */
    printf("  warmup...\n"); fflush(stdout);
    for (unsigned w = 0; w < 20 && w < repeat; w++) {
        const struct mykey **wk = key_pool + (size_t)w * BENCH_N;
        for (int i = 0; i < BENCH_N; i++) g_res[i] = myht_find(&head, bk, nodes, wk[i]);
        for (int i = 0; i < BENCH_N; i++) myht_hash_key(&g_ctx[i], &head, bk, wk[i]);
        for (int i = 0; i < BENCH_N; i++) myht_scan_bk (&g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++) g_res[i] = myht_cmp_key(&g_ctx[i], nodes);
        for (int i = 0; i < BENCH_N; i++) rel_hash_prefetch_key(wk[i]);
        for (int b = 0; b < BENCH_N / 4; b++) REL_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, wk + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++) REL_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++) REL_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        for (int b = 0; b < BENCH_N6 / 6; b++) REL_HASH_HASH_KEY_N(myht, &g_ctx[b * 6], 6, &head, bk, wk + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++) REL_HASH_SCAN_BK_N(myht, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++) REL_HASH_CMP_KEY_N(myht, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        for (int b = 0; b < BENCH_N8 / 8; b++) REL_HASH_HASH_KEY_N(myht, &g_ctx[b * 8], 8, &head, bk, wk + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++) REL_HASH_SCAN_BK_N(myht, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++) REL_HASH_CMP_KEY_N(myht, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        /* nohf warmup */
        const struct mykey **nwk = key_pool_nohf + (size_t)w * BENCH_N;
        for (int b = 0; b < BENCH_N6 / 6; b++) REL_HASH_HASH_KEY_N(myht_nohf, &g_ctx[b * 6], 6, &head_nohf, bk_nohf, nwk + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++) REL_HASH_SCAN_BK_N(myht_nohf, &g_ctx[b * 6], 6, &head_nohf, bk_nohf);
        for (int b = 0; b < BENCH_N6 / 6; b++) REL_HASH_CMP_KEY_N(myht_nohf, &g_ctx[b * 6], 6, nodes_nohf, (struct mynode_nohf **)&g_res_nohf[b * 6]);
        for (int b = 0; b < BENCH_N8 / 8; b++) REL_HASH_HASH_KEY_N(myht_nohf, &g_ctx[b * 8], 8, &head_nohf, bk_nohf, nwk + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++) REL_HASH_SCAN_BK_N(myht_nohf, &g_ctx[b * 8], 8, &head_nohf, bk_nohf);
        for (int b = 0; b < BENCH_N8 / 8; b++) REL_HASH_CMP_KEY_N(myht_nohf, &g_ctx[b * 8], 8, nodes_nohf, (struct mynode_nohf **)&g_res_nohf[b * 8]);
    }

    /* ---- 計測 ----------------------------------------------------- */
    /*
     * 各パターンを独立ループで測定する。前パターンが同じキープールを
     * 走査したため、L3 は thrash 済み → 次パターンは実質キャッシュコールド。
     */
    printf("  measuring...\n"); fflush(stdout);

    struct {
        const char *label;
        uint64_t min_cy;
        uint64_t sum_cy;
        int      ops;   /* ops per iteration (varies for x6 due to 256÷6) */
    } result[18] = {
        { "find (single)      ", UINT64_MAX, 0, BENCH_N  },
        { "x1 (bk only)       ", UINT64_MAX, 0, BENCH_N  },
        { "x1 (bk+node)       ", UINT64_MAX, 0, BENCH_N  },
        { "x4 (bk only)       ", UINT64_MAX, 0, BENCH_N  },
        { "x4 (bk+node)       ", UINT64_MAX, 0, BENCH_N  },
        { "x2 (bk+node)       ", UINT64_MAX, 0, BENCH_N  },
        { "x4 (key+bk) bulk   ", UINT64_MAX, 0, BENCH_N  },
        { "x4 (key+bk) Nahead ", UINT64_MAX, 0, BENCH_N  },
        { "x6 (bk+node)       ", UINT64_MAX, 0, BENCH_N6 },
        { "x6 (key+bk) Nahead ", UINT64_MAX, 0, BENCH_N6 },
        { "x8 (bk+node)       ", UINT64_MAX, 0, BENCH_N8 },
        { "x8 (key+bk) Nahead ", UINT64_MAX, 0, BENCH_N8 },
        { "nohf x6 Nahead     ", UINT64_MAX, 0, BENCH_N6 },
        { "nohf x8 Nahead     ", UINT64_MAX, 0, BENCH_N8 },
        { "remove regular     ", UINT64_MAX, 0, BENCH_N  },
        { "remove nohf        ", UINT64_MAX, 0, BENCH_N  },
        { "insert regular     ", UINT64_MAX, 0, BENCH_N  },
        { "insert nohf        ", UINT64_MAX, 0, BENCH_N  },
    };

    /* ---- [0] 逐次 find ------------------------------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = myht_find(&head, bk, nodes, ik[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[0].min_cy) result[0].min_cy = cy;
        result[0].sum_cy += cy;
    }

    /* ---- [1] x1: hash_key → scan_bk → cmp_key (node 未 prefetch) - */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_hash_key(&g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            myht_scan_bk(&g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = myht_cmp_key(&g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[1].min_cy) result[1].min_cy = cy;
        result[1].sum_cy += cy;
    }

    /* ---- [2] x1: hash_key → scan_bk → prefetch_node → cmp_key --- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_hash_key(&g_ctx[i], &head, bk, ik[i]);
        for (int i = 0; i < BENCH_N; i++)
            myht_scan_bk(&g_ctx[i], &head, bk);
        for (int i = 0; i < BENCH_N; i++)
            myht_prefetch_node(&g_ctx[i], nodes);
        for (int i = 0; i < BENCH_N; i++)
            g_res[i] = myht_cmp_key(&g_ctx[i], nodes);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[2].min_cy) result[2].min_cy = cy;
        result[2].sum_cy += cy;
    }

    /* ---- [3] x4: hash_key4 → scan_bk4 → cmp_key4 (node 未 prefetch) */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[3].min_cy) result[3].min_cy = cy;
        result[3].sum_cy += cy;
    }

    /* ---- [4] x4: hash_key4 → scan_bk4 → prefetch_node4 → cmp_key4 */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_PREFETCH_NODE4(myht, &g_ctx[b * 4], nodes);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[4].min_cy) result[4].min_cy = cy;
        result[4].sum_cy += cy;
    }

    /* ---- [5] x2: hash_key2 → scan_bk2 → prefetch_node2 → cmp_key2 */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH_HASH_KEY2(myht, &g_ctx[b * 2], &head, bk, ik + b * 2);
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH_SCAN_BK2(myht, &g_ctx[b * 2], &head, bk);
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH_PREFETCH_NODE2(myht, &g_ctx[b * 2], nodes);
        for (int b = 0; b < BENCH_N / 2; b++)
            REL_HASH_CMP_KEY2(myht, &g_ctx[b * 2], nodes, &g_res[b * 2]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[5].min_cy) result[5].min_cy = cy;
        result[5].sum_cy += cy;
    }

    /* ---- [6] x4: prefetch_key × 256 → hash_key4 → scan_bk4 → cmp_key4 */
    /*
     * Stage 0: 全キーを DRAM から先行ロード。
     * hash_fn(key) のキーデータ DRAM ミスを隠蔽するのが目的。
     * key_field がノード先頭にあるため、cmp_key のキー比較も同時に隠蔽できる。
     */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            rel_hash_prefetch_key(ik[i]);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, ik + b * 4);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[6].min_cy) result[6].min_cy = cy;
        result[6].sum_cy += cy;
    }

    /* ---- [7] x4: N-ahead スライディングウィンドウ key prefetch --- */
    /*
     * KPD4 = 16 バッチ (64 keys)。hash_key4 が key-in-cache で ~20 cy/batch
     * とすると 16 batches ≈ 320 cy ≒ DRAM latency → 1 レイテンシ分先行。
     */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD4 && b < BENCH_N / 4; b++)
            for (int j = 0; j < 4; j++)
                rel_hash_prefetch_key(ik[b * 4 + j]);
        for (int b = 0; b < BENCH_N / 4; b++) {
            int pf = b + KPD4;
            if (pf < BENCH_N / 4)
                for (int j = 0; j < 4; j++)
                    rel_hash_prefetch_key(ik[pf * 4 + j]);
            REL_HASH_HASH_KEY4(myht, &g_ctx[b * 4], &head, bk, ik + b * 4);
        }
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_SCAN_BK4(myht, &g_ctx[b * 4], &head, bk);
        for (int b = 0; b < BENCH_N / 4; b++)
            REL_HASH_CMP_KEY4(myht, &g_ctx[b * 4], nodes, &g_res[b * 4]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[7].min_cy) result[7].min_cy = cy;
        result[7].sum_cy += cy;
    }

    /* ---- [8] x6: hash_key_n(6) → scan_bk_n(6) → prefetch_node_n(6) → cmp_key_n(6) */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH_HASH_KEY_N(myht, &g_ctx[b * 6], 6, &head, bk, ik + b * 6);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH_SCAN_BK_N(myht, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH_PREFETCH_NODE_N(myht, &g_ctx[b * 6], 6, nodes);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH_CMP_KEY_N(myht, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[8].min_cy) result[8].min_cy = cy;
        result[8].sum_cy += cy;
    }

    /* ---- [9] x6: N-ahead スライディングウィンドウ key prefetch --- */
    /* KPD6 = 11 バッチ (66 keys) ≒ 1 DRAM latency */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6 / 6; b++)
            for (int j = 0; j < 6; j++)
                rel_hash_prefetch_key(ik[b * 6 + j]);
        for (int b = 0; b < BENCH_N6 / 6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6 / 6)
                for (int j = 0; j < 6; j++)
                    rel_hash_prefetch_key(ik[pf * 6 + j]);
            REL_HASH_HASH_KEY_N(myht, &g_ctx[b * 6], 6, &head, bk, ik + b * 6);
        }
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH_SCAN_BK_N(myht, &g_ctx[b * 6], 6, &head, bk);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH_CMP_KEY_N(myht, &g_ctx[b * 6], 6, nodes, &g_res[b * 6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[9].min_cy) result[9].min_cy = cy;
        result[9].sum_cy += cy;
    }

    /* ---- [10] x8: hash_key_n(8) → scan_bk_n(8) → prefetch_node_n(8) → cmp_key_n(8) */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH_HASH_KEY_N(myht, &g_ctx[b * 8], 8, &head, bk, ik + b * 8);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH_SCAN_BK_N(myht, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH_PREFETCH_NODE_N(myht, &g_ctx[b * 8], 8, nodes);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH_CMP_KEY_N(myht, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[10].min_cy) result[10].min_cy = cy;
        result[10].sum_cy += cy;
    }

    /* ---- [11] x8: N-ahead スライディングウィンドウ key prefetch --- */
    /* KPD8 = 8 バッチ (64 keys) ≒ 1 DRAM latency */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8 / 8; b++)
            for (int j = 0; j < 8; j++)
                rel_hash_prefetch_key(ik[b * 8 + j]);
        for (int b = 0; b < BENCH_N8 / 8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8 / 8)
                for (int j = 0; j < 8; j++)
                    rel_hash_prefetch_key(ik[pf * 8 + j]);
            REL_HASH_HASH_KEY_N(myht, &g_ctx[b * 8], 8, &head, bk, ik + b * 8);
        }
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH_SCAN_BK_N(myht, &g_ctx[b * 8], 8, &head, bk);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH_CMP_KEY_N(myht, &g_ctx[b * 8], 8, nodes, &g_res[b * 8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[11].min_cy) result[11].min_cy = cy;
        result[11].sum_cy += cy;
    }

    /* ---- [12] nohf x6: N-ahead key prefetch ----------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_nohf + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD6 && b < BENCH_N6 / 6; b++)
            for (int j = 0; j < 6; j++)
                rel_hash_prefetch_key(ik[b * 6 + j]);
        for (int b = 0; b < BENCH_N6 / 6; b++) {
            int pf = b + KPD6;
            if (pf < BENCH_N6 / 6)
                for (int j = 0; j < 6; j++)
                    rel_hash_prefetch_key(ik[pf * 6 + j]);
            REL_HASH_HASH_KEY_N(myht_nohf, &g_ctx[b * 6], 6, &head_nohf, bk_nohf, ik + b * 6);
        }
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH_SCAN_BK_N(myht_nohf, &g_ctx[b * 6], 6, &head_nohf, bk_nohf);
        for (int b = 0; b < BENCH_N6 / 6; b++)
            REL_HASH_CMP_KEY_N(myht_nohf, &g_ctx[b * 6], 6, nodes_nohf, (struct mynode_nohf **)&g_res_nohf[b * 6]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[12].min_cy) result[12].min_cy = cy;
        result[12].sum_cy += cy;
    }

    /* ---- [13] nohf x8: N-ahead key prefetch ----------------------- */
    for (unsigned r = 0; r < repeat; r++) {
        const struct mykey **ik = key_pool_nohf + (size_t)r * BENCH_N;
        uint64_t t0 = tsc_start();
        for (int b = 0; b < KPD8 && b < BENCH_N8 / 8; b++)
            for (int j = 0; j < 8; j++)
                rel_hash_prefetch_key(ik[b * 8 + j]);
        for (int b = 0; b < BENCH_N8 / 8; b++) {
            int pf = b + KPD8;
            if (pf < BENCH_N8 / 8)
                for (int j = 0; j < 8; j++)
                    rel_hash_prefetch_key(ik[pf * 8 + j]);
            REL_HASH_HASH_KEY_N(myht_nohf, &g_ctx[b * 8], 8, &head_nohf, bk_nohf, ik + b * 8);
        }
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH_SCAN_BK_N(myht_nohf, &g_ctx[b * 8], 8, &head_nohf, bk_nohf);
        for (int b = 0; b < BENCH_N8 / 8; b++)
            REL_HASH_CMP_KEY_N(myht_nohf, &g_ctx[b * 8], 8, nodes_nohf, (struct mynode_nohf **)&g_res_nohf[b * 8]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[13].min_cy) result[13].min_cy = cy;
        result[13].sum_cy += cy;
    }

    /* ---- [14] remove regular: hash_field & mask → O(1) bucket lookup */
    /*
     * nodes[0..BENCH_N-1] は全て bk_0 に存在 (bk_0 hit rate 100%)。
     * 各 repeat: BENCH_N 削除 → BENCH_N 再挿入 (テーブル状態を復元)。
     */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_remove(&head, bk, nodes, &nodes[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[14].min_cy) result[14].min_cy = cy;
        result[14].sum_cy += cy;
        for (int i = 0; i < BENCH_N; i++)
            myht_insert(&head, bk, nodes, &nodes[i]);
    }

    /* ---- [15] remove nohf: re-hash key → scan bk_0 (then bk_1 on miss) */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_nohf_remove(&head_nohf, bk_nohf, nodes_nohf, &nodes_nohf[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[15].min_cy) result[15].min_cy = cy;
        result[15].sum_cy += cy;
        for (int i = 0; i < BENCH_N; i++)
            myht_nohf_insert(&head_nohf, bk_nohf, nodes_nohf, &nodes_nohf[i]);
    }

    /* ---- [16/17] insert bench: pre-remove BENCH_N entries, then measure */
    /*
     * BENCH_N エントリを削除して空きスロットを確保してから計測。
     * 各 repeat: BENCH_N 挿入(計測) → BENCH_N 削除(リセット)。
     */
    for (int i = 0; i < BENCH_N; i++)
        myht_remove(&head, bk, nodes, &nodes[i]);
    for (int i = 0; i < BENCH_N; i++)
        myht_nohf_remove(&head_nohf, bk_nohf, nodes_nohf, &nodes_nohf[i]);

    /* ---- [16] insert regular */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_insert(&head, bk, nodes, &nodes[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[16].min_cy) result[16].min_cy = cy;
        result[16].sum_cy += cy;
        for (int i = 0; i < BENCH_N; i++)
            myht_remove(&head, bk, nodes, &nodes[i]);
    }

    /* ---- [17] insert nohf */
    for (unsigned r = 0; r < repeat; r++) {
        uint64_t t0 = tsc_start();
        for (int i = 0; i < BENCH_N; i++)
            myht_nohf_insert(&head_nohf, bk_nohf, nodes_nohf, &nodes_nohf[i]);
        uint64_t cy = tsc_end() - t0;
        if (cy < result[17].min_cy) result[17].min_cy = cy;
        result[17].sum_cy += cy;
        for (int i = 0; i < BENCH_N; i++)
            myht_nohf_remove(&head_nohf, bk_nohf, nodes_nohf, &nodes_nohf[i]);
    }

    /* ---- 結果出力 -------------------------------------------------- */
    {
        /* MSHR理論最小値 (bk_0-only最適化後):
         * 256 lookup × 3 cachelines (bk0×2, node×1) = 768 fetches
         *   bk_1 は bk_0 ミス時にのみ遅延ロード (充填率80%では事実上発生しない)
         * MSHR capacity ~20, DRAM latency ~300 cycles
         * → min: 768/20 × 300 = 11520 cycles/256 = 45 cycles/op  */
        double mshr_min = 11520.0;
        printf("\n");
        printf("  %-20s  %10s  %10s  %10s  %10s\n",
               "pattern", "min/256", "avg/256", "min/op", "avg/op");
        printf("  %-20s  %10s  %10s  %10s  %10s\n",
               "--------------------", "----------", "----------", "----------", "----------");
        for (int p = 0; p < 18; p++) {
            if (p == 14)
                printf("  %-20s  %10s  %10s  %10s  %10s\n",
                       "-- insert/remove --", "", "", "", "");
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
        printf("  MSHR theoretical min (20 MSHRs, 300cy latency, 5CL/lookup):\n");
        printf("    %.0f cycles/256 = %.1f cycles/op\n", mshr_min, mshr_min / BENCH_N);
        printf("  note: avg reflects steady-state DRAM-bound throughput;\n");
        printf("        min reflects best-case (partially cache-warm) throughput.\n");
        printf("        bk_mem=%.1f MB vs typical L3 cache → avg is cache-cold.\n\n",
               bk_mem / 1e6);
    }

    free(key_pool);
    free(key_pool_nohf);
    munmap(bk, bk_mem);
    munmap(bk_nohf, bk_mem);
    munmap(nodes, node_mem);
    munmap(nodes_nohf, node_nohf_mem);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int
main(int argc, char **argv)
{
    unsigned table_n  = 100000000u; /* 100M */
    unsigned nb_bk    = 0;
    unsigned repeat   = 2000;
    int      rand_keys = 0; /* 0=sequential, 1=random */

    if (argc >= 2) table_n   = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc >= 3) nb_bk     = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc >= 4) repeat    = (unsigned)strtoul(argv[3], NULL, 10);
    if (argc >= 5) rand_keys = (int)strtoul(argv[4], NULL, 10);

    if (nb_bk == 0) {
        /* デフォルト: ~80% 充填を目標 (slots = table_n / 0.8) */
        nb_bk = 1;
        while ((uint64_t)nb_bk * REL_HASH_BUCKET_ENTRY_SZ * 4 < (uint64_t)table_n * 5)
            nb_bk <<= 1;
    }

    rel_hash_arch_init();
    bench_find(table_n, nb_bk, repeat, rand_keys);
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
