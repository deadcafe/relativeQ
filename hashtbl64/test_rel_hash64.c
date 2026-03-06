/* test_rel_hash64.c
 *  REL_HASH64 (cuckoo hash table, 64-bit key) – unit & fuzz tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rel_hash64.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

/* ================================================================== */
/* Node definition                                                     */
/* ================================================================== */
typedef struct mynode_s {
    uint64_t key;
    uint64_t val;
} mynode_t;

#define INVALID_KEY  0xFFFFFFFFFFFFFFFFULL

REL_HASH64_HEAD(myht64);
REL_HASH64_GENERATE(myht64, mynode_t, key, INVALID_KEY)

/* ================================================================== */
/* Globals for basic tests                                             */
/* ================================================================== */
#define NB_BASIC     20u
#define NB_BK_BASIC   4u  /* 4 × 8 = 32 slots for 20 nodes */

static mynode_t                    g_basic[NB_BASIC];
static struct rel_hash64_bucket_s  g_bk[NB_BK_BASIC] __attribute__((aligned(64)));
static struct myht64               g_head;

static void
basic_init(void)
{
    memset(g_basic, 0, sizeof(g_basic));
    memset(g_bk,    0, sizeof(g_bk));
    REL_HASH64_INIT(myht64, &g_head, g_bk, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        g_basic[i].key = (uint64_t)(i + 1);
        g_basic[i].val = i;
    }
}

/* ================================================================== */
/* Tests                                                               */
/* ================================================================== */
static void
test_init_empty(void)
{
    printf("[T] init/empty\n");
    basic_init();
    if (g_head.rhh_nb != 0)
        FAIL("rhh_nb != 0 after init");
    /* All slots must have invalid_key sentinel */
    for (unsigned b = 0; b < NB_BK_BASIC; b++)
        for (unsigned s = 0; s < REL_HASH64_BUCKET_ENTRY_SZ; s++)
            if (g_bk[b].key[s] != INVALID_KEY)
                FAILF("key[%u][%u] not INVALID_KEY after init", b, s);
}

static void
test_insert_find_remove(void)
{
    printf("[T] insert/find/remove\n");
    basic_init();

    /* Insert all */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        mynode_t *r = REL_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &g_basic[i]);
        if (r != NULL)
            FAILF("insert[%u] failed (ret=%p)", i, (void *)r);
    }
    if (g_head.rhh_nb != NB_BASIC)
        FAILF("rhh_nb=%u expected %u", g_head.rhh_nb, NB_BASIC);

    /* Find all */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        mynode_t *r = REL_HASH64_FIND(myht64, &g_head, g_bk, g_basic, g_basic[i].key);
        if (r != &g_basic[i])
            FAILF("find[%u] returned %p expected %p", i, (void *)r, (void *)&g_basic[i]);
    }

    /* Miss */
    mynode_t *miss = REL_HASH64_FIND(myht64, &g_head, g_bk, g_basic, 0xDEAD000000000000ULL);
    if (miss != NULL)
        FAIL("miss find returned non-NULL");

    /* Remove half */
    for (unsigned i = 0; i < NB_BASIC; i += 2) {
        mynode_t *r = REL_HASH64_REMOVE(myht64, &g_head, g_bk, g_basic, &g_basic[i]);
        if (r != &g_basic[i])
            FAILF("remove[%u] returned %p", i, (void *)r);
    }
    if (g_head.rhh_nb != NB_BASIC / 2)
        FAILF("rhh_nb=%u after half remove", g_head.rhh_nb);

    /* Verify odd still found, even gone */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        mynode_t *r = REL_HASH64_FIND(myht64, &g_head, g_bk, g_basic, g_basic[i].key);
        if (i % 2 == 1 && r != &g_basic[i])
            FAILF("odd[%u] not found after remove", i);
        if (i % 2 == 0 && r != NULL)
            FAILF("even[%u] still found after remove", i);
    }
}

static void
test_duplicate_insert(void)
{
    printf("[T] duplicate insert\n");
    basic_init();

    mynode_t *r = REL_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &g_basic[0]);
    if (r != NULL) FAIL("first insert failed");

    /* Insert duplicate */
    mynode_t dup = g_basic[0];
    r = REL_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &dup);
    if (r != &g_basic[0])
        FAILF("dup insert: expected %p got %p", (void *)&g_basic[0], (void *)r);
}

static void
test_staged_find(void)
{
    printf("[T] staged find x1/x2/x4\n");
    basic_init();
    for (unsigned i = 0; i < NB_BASIC; i++)
        REL_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &g_basic[i]);

    /* x1 staged */
    {
        struct rel_hash64_find_ctx_s ctx;
        REL_HASH64_HASH_KEY(myht64, &ctx, &g_head, g_bk, g_basic[3].key);
        REL_HASH64_SCAN_BK (myht64, &ctx, &g_head, g_bk);
        mynode_t *r = REL_HASH64_CMP_KEY(myht64, &ctx, g_basic);
        if (r != &g_basic[3]) FAIL("x1 staged find failed");
    }

    /* x2 staged */
    {
        struct rel_hash64_find_ctx_s ctx2[2];
        uint64_t keys2[2] = { g_basic[0].key, g_basic[1].key };
        mynode_t *res2[2];
        REL_HASH64_HASH_KEY_N(myht64, ctx2, 2, &g_head, g_bk, keys2);
        REL_HASH64_SCAN_BK_N (myht64, ctx2, 2, &g_head, g_bk);
        REL_HASH64_PREFETCH_NODE_N(myht64, ctx2, 2, g_basic);
        REL_HASH64_CMP_KEY_N (myht64, ctx2, 2, g_basic, res2);
        if (res2[0] != &g_basic[0] || res2[1] != &g_basic[1])
            FAIL("x2 staged find failed");
    }

    /* x4 staged */
    {
        struct rel_hash64_find_ctx_s ctx4[4];
        uint64_t keys4[4] = {
            g_basic[4].key, g_basic[5].key, g_basic[6].key, g_basic[7].key
        };
        mynode_t *res4[4];
        REL_HASH64_HASH_KEY4(myht64, ctx4, &g_head, g_bk, keys4);
        REL_HASH64_SCAN_BK4 (myht64, ctx4, &g_head, g_bk);
        REL_HASH64_PREFETCH_NODE4(myht64, ctx4, g_basic);
        REL_HASH64_CMP_KEY4 (myht64, ctx4, g_basic, res4);
        for (int i = 0; i < 4; i++)
            if (res4[i] != &g_basic[4 + i])
                FAILF("x4 staged find [%d] failed", i);
    }
}

static unsigned g_walk_visited;

static int
walk_cb(mynode_t *n, void *arg)
{
    (void)arg;
    g_walk_visited++;
    if (n < g_basic || n >= g_basic + NB_BASIC)
        FAIL("walk: node out of range");
    return 0;
}

static void
test_walk(void)
{
    printf("[T] walk\n");
    basic_init();
    for (unsigned i = 0; i < NB_BASIC; i++)
        REL_HASH64_INSERT(myht64, &g_head, g_bk, g_basic, &g_basic[i]);

    g_walk_visited = 0;
    REL_HASH64_WALK(myht64, &g_head, g_bk, g_basic, walk_cb, NULL);
    if (g_walk_visited != NB_BASIC)
        FAILF("walk visited %u expected %u", g_walk_visited, NB_BASIC);
}

/* ================================================================== */
/* Fuzz test                                                           */
/* ================================================================== */
static void
test_fuzz(unsigned seed, unsigned N, unsigned nb_bk, unsigned ops)
{
    printf("[T] fuzz seed=%u N=%u nb_bk=%u ops=%u\n", seed, N, nb_bk, ops);

    mynode_t                   *nodes   = calloc(N, sizeof(*nodes));
    struct rel_hash64_bucket_s *buckets = calloc(nb_bk, sizeof(*buckets));
    int                        *in_tbl  = calloc(N, sizeof(int));
    if (!nodes || !buckets || !in_tbl) { perror("calloc"); abort(); }

    struct myht64 head;
    REL_HASH64_INIT(myht64, &head, buckets, nb_bk);
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key = (uint64_t)(i + 1);
        nodes[i].val = i;
    }

    /* Simple LCG */
    uint64_t rng = seed;
    for (unsigned op = 0; op < ops; op++) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        unsigned idx = (unsigned)((rng >> 33) % N);
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        int do_insert = (int)((rng >> 63) & 1);

        if (do_insert && !in_tbl[idx]) {
            mynode_t *r = REL_HASH64_INSERT(myht64, &head, buckets, nodes, &nodes[idx]);
            if (r == NULL) {
                in_tbl[idx] = 1;
            } else if (r != &nodes[idx]) {
                FAILF("fuzz insert[%u] returned unexpected %p", idx, (void *)r);
            }
        } else if (!do_insert && in_tbl[idx]) {
            mynode_t *r = REL_HASH64_REMOVE(myht64, &head, buckets, nodes, &nodes[idx]);
            if (r != &nodes[idx])
                FAILF("fuzz remove[%u] failed", idx);
            in_tbl[idx] = 0;
        }

        /* Verify find */
        mynode_t *r = REL_HASH64_FIND(myht64, &head, buckets, nodes, nodes[idx].key);
        if (in_tbl[idx] && r != &nodes[idx])
            FAILF("fuzz find[%u] missing", idx);
        if (!in_tbl[idx] && r != NULL)
            FAILF("fuzz find[%u] ghost", idx);
    }

    free(nodes); free(buckets); free(in_tbl);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int
main(void)
{
    rel_hash_arch_init();

    test_init_empty();
    test_insert_find_remove();
    test_duplicate_insert();
    test_staged_find();
    test_walk();
    test_fuzz(3237998097u, 512, 64, 200000);

    printf("ALL REL_HASH64 TESTS PASSED\n");
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
