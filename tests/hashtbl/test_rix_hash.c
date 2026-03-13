/* test_rix_hash.c
 *  RIX_HASH (cuckoo hash table) – unit & fuzz tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rix_hash.h"

#define FAIL(msg) do { \
    fprintf(stderr, "FAIL %s:%d:%s: %s\n", __FILE__, __LINE__, __func__, (msg)); \
    abort(); \
} while (0)

#define FAILF(fmt, ...) do { \
    fprintf(stderr, "FAIL %s:%d:%s: " fmt "\n", __FILE__, __LINE__, __func__, __VA_ARGS__); \
    abort(); \
} while (0)

/* ================================================================== */
/* Node definition (128-bit key as struct)                             */
/* ================================================================== */
struct mykey {
    uint64_t hi;
    uint64_t lo;
};

struct mynode {
    uint32_t  cur_hash; /* hash_field: hash of the current bucket (updated on kickout) */
    uint32_t  _pad;     /* alignment padding */
    struct mykey key;   /* key_field: must be a struct, sizeof = 16 */
};

/* 16-byte key comparison using SSE2 (struct mykey = two uint64_t) */
static RIX_FORCE_INLINE int
mykey_cmp(const void *a, const void *b)
{
    __m128i va = _mm_loadu_si128((const __m128i *)a);
    __m128i vb = _mm_loadu_si128((const __m128i *)b);
    return _mm_movemask_epi8(_mm_cmpeq_epi8(va, vb)) == 0xffff;
}

RIX_HASH_HEAD(myht);
RIX_HASH_GENERATE(myht, mynode, key, cur_hash, mykey_cmp)

/* ================================================================== */
/* nohf variant: no hash_field (8-byte node)                           */
/* ================================================================== */
struct mynode_nohf {
    struct mykey key;
};

RIX_HASH_HEAD(myht_nohf);
RIX_HASH_GENERATE_NOHF(myht_nohf, mynode_nohf, key, mykey_cmp)

/* ================================================================== */
/* Globals for basic tests                                             */
/* ================================================================== */
#define NB_BASIC     20u
#define NB_BK_BASIC   4u  /* 4 × 16 = 64 slots for 20 nodes  */

static struct mynode          g_basic[NB_BASIC];
static struct rix_hash_bucket_s g_bk[NB_BK_BASIC] __attribute__((aligned(64)));
static struct myht            g_head;

static struct mynode_nohf       g_nohf[NB_BASIC];
static struct rix_hash_bucket_s g_bk_nohf[NB_BK_BASIC] __attribute__((aligned(64)));
static struct myht_nohf         g_head_nohf;

static void
basic_init(void)
{
    memset(g_basic, 0, sizeof(g_basic));
    memset(g_bk,    0, sizeof(g_bk));
    RIX_HASH_INIT(myht, &g_head, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        g_basic[i].key.hi = (uint64_t)(i + 1);
        g_basic[i].key.lo = 0xDEADC0DE00000000ULL;
    }
}

/* Insert all NB_BASIC nodes; abort on failure. */
static void
basic_insert_all(void)
{
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode *ret = myht_insert(&g_head, g_bk, g_basic, &g_basic[i]);
        if (ret != NULL)
            FAILF("insert[%u] failed (ret=%p)", i, (void *)ret);
    }
    if (g_head.rhh_nb != NB_BASIC)
        FAILF("nb after all inserts: expected %u got %u",
              NB_BASIC, g_head.rhh_nb);
}

/* ================================================================== */
/* test_init_empty                                                     */
/* ================================================================== */
static void
test_init_empty(void)
{
    printf("[T] init/empty\n");
    basic_init();

    if (g_head.rhh_nb != 0u)
        FAIL("init: rhh_nb not zero");
    if (g_head.rhh_mask != NB_BK_BASIC - 1u)
        FAILF("init: mask expected %u got %u", NB_BK_BASIC - 1u, g_head.rhh_mask);

    /* find on empty table must return NULL */
    struct mykey k = { 1ULL, 0xDEADC0DE00000000ULL };
    struct mynode *r = myht_find(&g_head, g_bk, g_basic, &k);
    if (r != NULL)
        FAIL("find on empty table should return NULL");

    /* remove on empty table: g_basic[0] has keys set but was never inserted */
    if (myht_remove(&g_head, g_bk, g_basic, &g_basic[0]) != NULL)
        FAIL("remove on empty table should return NULL");
}

/* ================================================================== */
/* test_insert_find_remove                                             */
/* ================================================================== */
static void
test_insert_find_remove(void)
{
    printf("[T] insert/find/remove\n");
    basic_init();
    basic_insert_all();

    /* Find each inserted node */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode *f = myht_find(&g_head, g_bk, g_basic, &g_basic[i].key);
        if (f == NULL)
            FAILF("find[%u] returned NULL", i);
        if (f != &g_basic[i])
            FAILF("find[%u] returned wrong node %p expected %p",
                  i, (void *)f, (void *)&g_basic[i]);
    }

    /* Find a non-existent key */
    struct mykey bad = { 9999ULL, 0xDEADC0DE00000000ULL };
    if (myht_find(&g_head, g_bk, g_basic, &bad) != NULL)
        FAIL("find of non-existent key should return NULL");

    /* Remove even-indexed nodes */
    for (unsigned i = 0; i < NB_BASIC; i += 2) {
        struct mynode *rem = myht_remove(&g_head, g_bk, g_basic, &g_basic[i]);
        if (rem != &g_basic[i])
            FAILF("remove[%u] returned wrong node", i);
    }
    if (g_head.rhh_nb != NB_BASIC / 2)
        FAILF("nb after removals: expected %u got %u", NB_BASIC / 2, g_head.rhh_nb);

    /* Removed must not be found; remaining must be found */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode *f = myht_find(&g_head, g_bk, g_basic, &g_basic[i].key);
        if (i % 2 == 0) {
            if (f != NULL)
                FAILF("removed node[%u] still found", i);
        } else {
            if (f == NULL || f != &g_basic[i])
                FAILF("remaining node[%u] not found or wrong", i);
        }
    }

    /* Remove already-removed node must return NULL */
    if (myht_remove(&g_head, g_bk, g_basic, &g_basic[0]) != NULL)
        FAIL("re-remove of already-removed node should return NULL");
}

/* ================================================================== */
/* test_duplicate                                                      */
/* ================================================================== */
static void
test_duplicate(void)
{
    printf("[T] duplicate insert\n");
    basic_init();

    /* Insert node[0] */
    struct mynode *r0 = myht_insert(&g_head, g_bk, g_basic, &g_basic[0]);
    if (r0 != NULL)
        FAIL("first insert must return NULL");

    /* Re-insert the same node – must return the existing node */
    struct mynode *r1 = myht_insert(&g_head, g_bk, g_basic, &g_basic[0]);
    if (r1 != &g_basic[0])
        FAILF("dup insert must return existing node %p, got %p",
              (void *)&g_basic[0], (void *)r1);
    if (g_head.rhh_nb != 1u)
        FAILF("nb must still be 1 after dup, got %u", g_head.rhh_nb);

    /*
     * Insert a different node object that has the same key value.
     * insert() writes cur_hash to elm before the dup check, but dup is a
     * stack variable so that write is harmless.  The dup check then finds
     * &g_basic[0] and returns early before any bucket slot is modified.
     */
    struct mynode dup;
    dup.key = g_basic[0].key;
    struct mynode *r2 = myht_insert(&g_head, g_bk, g_basic, &dup);
    if (r2 != &g_basic[0])
        FAILF("key-dup insert must return existing node %p, got %p",
              (void *)&g_basic[0], (void *)r2);
    if (g_head.rhh_nb != 1u)
        FAILF("nb must still be 1 after key-dup, got %u", g_head.rhh_nb);
}

/* ================================================================== */
/* test_staged_find                                                    */
/* ================================================================== */
static void
test_staged_find(void)
{
    printf("[T] staged find x1/x2/x4\n");
    basic_init();
    basic_insert_all();

    struct mykey bad = { 9999ULL, 0xDEADC0DE00000000ULL };

    /* --- x1 staged --- */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct rix_hash_find_ctx_s ctx;
        RIX_HASH_HASH_KEY(myht, &ctx, &g_head, g_bk, &g_basic[i].key);
        RIX_HASH_SCAN_BK (myht, &ctx, &g_head, g_bk);
        struct mynode *r = RIX_HASH_CMP_KEY(myht, &ctx, g_basic);
        if (r == NULL || r != &g_basic[i])
            FAILF("staged x1 find[%u] mismatch", i);
    }
    {   /* non-existent */
        struct rix_hash_find_ctx_s ctx;
        RIX_HASH_HASH_KEY(myht, &ctx, &g_head, g_bk, &bad);
        RIX_HASH_SCAN_BK (myht, &ctx, &g_head, g_bk);
        if (RIX_HASH_CMP_KEY(myht, &ctx, g_basic) != NULL)
            FAIL("staged x1 find of non-existent should be NULL");
    }

    /* --- x2 staged --- */
    {
        struct rix_hash_find_ctx_s ctx2[2];
        const struct mykey *keys2[2] = { &g_basic[0].key, &bad };
        struct mynode *res2[2];

        RIX_HASH_HASH_KEY2(myht, ctx2, &g_head, g_bk, keys2);
        RIX_HASH_SCAN_BK2 (myht, ctx2, &g_head, g_bk);
        RIX_HASH_CMP_KEY2 (myht, ctx2, g_basic, res2);

        if (res2[0] != &g_basic[0])
            FAILF("staged x2[0] mismatch: got %p", (void *)res2[0]);
        if (res2[1] != NULL)
            FAILF("staged x2[1] non-existent should be NULL, got %p",
                  (void *)res2[1]);
    }

    /* --- x4 staged --- */
    {
        struct rix_hash_find_ctx_s ctx4[4];
        const struct mykey *keys4[4] = {
            &g_basic[3].key, &bad, &g_basic[7].key, &g_basic[11].key
        };
        struct mynode *res4[4];

        RIX_HASH_HASH_KEY4(myht, ctx4, &g_head, g_bk, keys4);
        RIX_HASH_SCAN_BK4 (myht, ctx4, &g_head, g_bk);
        RIX_HASH_CMP_KEY4 (myht, ctx4, g_basic, res4);

        if (res4[0] != &g_basic[3])
            FAILF("staged x4[0] mismatch: got %p", (void *)res4[0]);
        if (res4[1] != NULL)
            FAILF("staged x4[1] non-existent should be NULL, got %p",
                  (void *)res4[1]);
        if (res4[2] != &g_basic[7])
            FAILF("staged x4[2] mismatch: got %p", (void *)res4[2]);
        if (res4[3] != &g_basic[11])
            FAILF("staged x4[3] mismatch: got %p", (void *)res4[3]);
    }
}

/* ================================================================== */
/* test_walk                                                           */
/* ================================================================== */
static int walk_count_cb(struct mynode *node, void *arg)
{
    (void)node;
    unsigned *cnt = (unsigned *)arg;
    (*cnt)++;
    return 0;
}

static int walk_stop_cb(struct mynode *node, void *arg)
{
    (void)node;
    int *cnt = (int *)arg;
    (*cnt)++;
    return (*cnt >= 3) ? 99 : 0; /* stop after 3 */
}

static void
test_walk(void)
{
    printf("[T] walk\n");
    basic_init();
    basic_insert_all();

    unsigned cnt = 0;
    int ret = myht_walk(&g_head, g_bk, g_basic, walk_count_cb, &cnt);
    if (ret != 0)
        FAILF("walk returned non-zero: %d", ret);
    if (cnt != NB_BASIC)
        FAILF("walk count: expected %u got %u", NB_BASIC, cnt);

    /* Remove 5 nodes and walk again */
    for (unsigned i = 0; i < 5; i++)
        myht_remove(&g_head, g_bk, g_basic, &g_basic[i]);
    cnt = 0;
    myht_walk(&g_head, g_bk, g_basic, walk_count_cb, &cnt);
    if (cnt != NB_BASIC - 5)
        FAILF("walk after remove: expected %u got %u", NB_BASIC - 5, cnt);

    /* Early-stop callback */
    basic_init();
    basic_insert_all();
    int scnt = 0;
    ret = myht_walk(&g_head, g_bk, g_basic, walk_stop_cb, &scnt);
    if (ret != 99)
        FAILF("early-stop walk should return 99, got %d", ret);
    if (scnt != 3)
        FAILF("early-stop walk cb called %d times, expected 3", scnt);
}

/* ================================================================== */
/* Fuzz test                                                           */
/* ================================================================== */
static unsigned xr_fuzz;

static unsigned
xorshift32(void)
{
    unsigned x = xr_fuzz;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (xr_fuzz = x);
}

static unsigned
rnd_in(unsigned lo, unsigned hi)
{
    return lo + (xorshift32() % (hi - lo + 1));
}

static void
test_fuzz(unsigned seed, unsigned N, unsigned nb_bk, unsigned ops)
{
    printf("[T] fuzz seed=%u N=%u nb_bk=%u ops=%u\n", seed, N, nb_bk, ops);
    xr_fuzz = seed ? seed : 0xC0FFEE11u;

    /* Allocate node pool */
    struct mynode *nodes = (struct mynode *)calloc((size_t)N, sizeof(struct mynode));
    if (!nodes) { perror("calloc nodes"); exit(1); }

    /* Assign unique keys: key.hi = 1-origin index, key.lo = 0 */
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (uint64_t)(i + 1);
        nodes[i].key.lo = 0;
    }

    /* Allocate aligned bucket array (zeroed) */
    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign");
        exit(1);
    }
    memset(bk, 0, bk_sz);

    /* Head */
    struct myht head;
    RIX_HASH_INIT(myht, &head, nb_bk);

    /* Model: present[i] = 1 means nodes[i-1] is in the table */
    unsigned char *present = (unsigned char *)calloc((size_t)N + 1, 1);
    if (!present) { perror("calloc present"); exit(1); }

    unsigned in_table = 0; /* shadow of head.rhh_nb */

    for (unsigned step = 0; step < ops; step++) {
        unsigned op = xorshift32() % 100;

        if (op < 60) {
            /* Insert */
            unsigned idx = rnd_in(1, N); /* 1-origin */
            struct mynode *elm = &nodes[idx - 1];
            struct mynode *ret = myht_insert(&head, bk, nodes, elm);

            if (present[idx]) {
                /* Must return the existing node */
                if (ret != elm)
                    FAILF("fuzz insert dup[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
            } else {
                if (ret == elm) {
                    /* Table full – acceptable, just skip */
                } else if (ret != NULL) {
                    FAILF("fuzz insert[%u]: unexpected ret %p", idx, (void *)ret);
                } else {
                    present[idx] = 1;
                    in_table++;
                }
            }

        } else if (op < 80) {
            /* Remove */
            unsigned idx = rnd_in(1, N);
            struct mynode *elm = &nodes[idx - 1];
            struct mynode *ret = myht_remove(&head, bk, nodes, elm);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("fuzz remove[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
                present[idx] = 0;
                in_table--;
            } else {
                if (ret != NULL)
                    FAILF("fuzz remove absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }

        } else {
            /* Find */
            unsigned idx = rnd_in(1, N);
            struct mynode *elm = &nodes[idx - 1];
            struct mynode *ret = myht_find(&head, bk, nodes, &elm->key);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("fuzz find[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
            } else {
                if (ret != NULL)
                    FAILF("fuzz find absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }
        }

        /* Periodically verify rhh_nb matches the model */
        if ((step & 0x3FFu) == 0u) {
            if (head.rhh_nb != in_table)
                FAILF("fuzz step %u: rhh_nb=%u model=%u",
                      step, head.rhh_nb, in_table);
        }
    }

    /* Final consistency check */
    if (head.rhh_nb != in_table)
        FAILF("fuzz final: rhh_nb=%u model=%u", head.rhh_nb, in_table);

    /* Walk must visit exactly in_table nodes */
    unsigned walk_cnt = 0;
    myht_walk(&head, bk, nodes, walk_count_cb, &walk_cnt);
    if (walk_cnt != in_table)
        FAILF("fuzz walk: expected %u got %u", in_table, walk_cnt);

    free(present);
    free(bk);
    free(nodes);
}

/* ================================================================== */
/* nohf helpers                                                        */
/* ================================================================== */
static void
nohf_init(void)
{
    memset(g_nohf,    0, sizeof(g_nohf));
    memset(g_bk_nohf, 0, sizeof(g_bk_nohf));
    RIX_HASH_INIT(myht_nohf, &g_head_nohf, NB_BK_BASIC);
    for (unsigned i = 0; i < NB_BASIC; i++) {
        g_nohf[i].key.hi = (uint64_t)(i + 1);
        g_nohf[i].key.lo = 0xDEADC0DE00000000ULL;
    }
}

static void
nohf_insert_all(void)
{
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode_nohf *ret =
            myht_nohf_insert(&g_head_nohf, g_bk_nohf, g_nohf, &g_nohf[i]);
        if (ret != NULL)
            FAILF("nohf insert[%u] failed (ret=%p)", i, (void *)ret);
    }
    if (g_head_nohf.rhh_nb != NB_BASIC)
        FAILF("nohf nb after all inserts: expected %u got %u",
              NB_BASIC, g_head_nohf.rhh_nb);
}

/* ================================================================== */
/* test_nohf_duplicate                                                 */
/* ================================================================== */
static void
test_nohf_duplicate(void)
{
    printf("[T] nohf duplicate insert\n");
    nohf_init();

    /* Insert node[0] */
    struct mynode_nohf *r0 =
        myht_nohf_insert(&g_head_nohf, g_bk_nohf, g_nohf, &g_nohf[0]);
    if (r0 != NULL)
        FAIL("nohf first insert must return NULL");

    /* Re-insert same node – must return existing node */
    struct mynode_nohf *r1 =
        myht_nohf_insert(&g_head_nohf, g_bk_nohf, g_nohf, &g_nohf[0]);
    if (r1 != &g_nohf[0])
        FAILF("nohf dup insert must return existing node %p, got %p",
              (void *)&g_nohf[0], (void *)r1);
    if (g_head_nohf.rhh_nb != 1u)
        FAILF("nohf nb must still be 1 after dup, got %u", g_head_nohf.rhh_nb);

    /* Different node object with same key value */
    struct mynode_nohf dup;
    dup.key = g_nohf[0].key;
    struct mynode_nohf *r2 =
        myht_nohf_insert(&g_head_nohf, g_bk_nohf, g_nohf, &dup);
    if (r2 != &g_nohf[0])
        FAILF("nohf key-dup insert must return existing node %p, got %p",
              (void *)&g_nohf[0], (void *)r2);
    if (g_head_nohf.rhh_nb != 1u)
        FAILF("nohf nb must still be 1 after key-dup, got %u",
              g_head_nohf.rhh_nb);
}

/* ================================================================== */
/* test_nohf_insert_find_remove                                        */
/* ================================================================== */
static void
test_nohf_insert_find_remove(void)
{
    printf("[T] nohf insert/find/remove\n");
    nohf_init();
    nohf_insert_all();

    /* Find each inserted node */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode_nohf *f =
            myht_nohf_find(&g_head_nohf, g_bk_nohf, g_nohf, &g_nohf[i].key);
        if (f == NULL)
            FAILF("nohf find[%u] returned NULL", i);
        if (f != &g_nohf[i])
            FAILF("nohf find[%u] returned wrong node %p expected %p",
                  i, (void *)f, (void *)&g_nohf[i]);
    }

    /* Non-existent key */
    struct mykey bad = { 9999ULL, 0xDEADC0DE00000000ULL };
    if (myht_nohf_find(&g_head_nohf, g_bk_nohf, g_nohf, &bad) != NULL)
        FAIL("nohf find of non-existent key should return NULL");

    /* Remove even-indexed nodes */
    for (unsigned i = 0; i < NB_BASIC; i += 2) {
        struct mynode_nohf *rem =
            myht_nohf_remove(&g_head_nohf, g_bk_nohf, g_nohf, &g_nohf[i]);
        if (rem != &g_nohf[i])
            FAILF("nohf remove[%u] returned wrong node", i);
    }
    if (g_head_nohf.rhh_nb != NB_BASIC / 2)
        FAILF("nohf nb after removals: expected %u got %u",
              NB_BASIC / 2, g_head_nohf.rhh_nb);

    /* Removed must not be found; remaining must be found */
    for (unsigned i = 0; i < NB_BASIC; i++) {
        struct mynode_nohf *f =
            myht_nohf_find(&g_head_nohf, g_bk_nohf, g_nohf, &g_nohf[i].key);
        if (i % 2 == 0) {
            if (f != NULL)
                FAILF("nohf removed node[%u] still found", i);
        } else {
            if (f == NULL || f != &g_nohf[i])
                FAILF("nohf remaining node[%u] not found or wrong", i);
        }
    }

    /* Re-remove already-removed node must return NULL */
    if (myht_nohf_remove(&g_head_nohf, g_bk_nohf, g_nohf, &g_nohf[0]) != NULL)
        FAIL("nohf re-remove of already-removed node should return NULL");
}

/* ================================================================== */
/* test_nohf_fuzz                                                      */
/* ================================================================== */
static int walk_nohf_count_cb(struct mynode_nohf *node, void *arg)
{
    (void)node;
    unsigned *cnt = (unsigned *)arg;
    (*cnt)++;
    return 0;
}

static void
test_nohf_fuzz(unsigned seed, unsigned N, unsigned nb_bk, unsigned ops)
{
    printf("[T] nohf fuzz seed=%u N=%u nb_bk=%u ops=%u\n",
           seed, N, nb_bk, ops);
    xr_fuzz = seed ? seed : 0xC0FFEE11u;

    struct mynode_nohf *nodes =
        (struct mynode_nohf *)calloc((size_t)N, sizeof(struct mynode_nohf));
    if (!nodes) { perror("calloc nohf nodes"); exit(1); }
    for (unsigned i = 0; i < N; i++) {
        nodes[i].key.hi = (uint64_t)(i + 1);
        nodes[i].key.lo = 0;
    }

    struct rix_hash_bucket_s *bk = NULL;
    size_t bk_sz = (size_t)nb_bk * sizeof(struct rix_hash_bucket_s);
    if (posix_memalign((void **)&bk, 64, bk_sz) != 0) {
        perror("posix_memalign"); exit(1);
    }
    memset(bk, 0, bk_sz);

    struct myht_nohf head;
    RIX_HASH_INIT(myht_nohf, &head, nb_bk);

    unsigned char *present = (unsigned char *)calloc((size_t)N + 1, 1);
    if (!present) { perror("calloc present"); exit(1); }
    unsigned in_table = 0;

    for (unsigned step = 0; step < ops; step++) {
        unsigned op = xorshift32() % 100;

        if (op < 60) {
            unsigned idx = rnd_in(1, N);
            struct mynode_nohf *elm = &nodes[idx - 1];
            struct mynode_nohf *ret =
                myht_nohf_insert(&head, bk, nodes, elm);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("nohf fuzz insert dup[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
            } else {
                if (ret == elm) {
                    /* table full – skip */
                } else if (ret != NULL) {
                    FAILF("nohf fuzz insert[%u]: unexpected ret %p",
                          idx, (void *)ret);
                } else {
                    present[idx] = 1;
                    in_table++;
                }
            }
        } else if (op < 80) {
            unsigned idx = rnd_in(1, N);
            struct mynode_nohf *elm = &nodes[idx - 1];
            struct mynode_nohf *ret =
                myht_nohf_remove(&head, bk, nodes, elm);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("nohf fuzz remove[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
                present[idx] = 0;
                in_table--;
            } else {
                if (ret != NULL)
                    FAILF("nohf fuzz remove absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }
        } else {
            unsigned idx = rnd_in(1, N);
            struct mynode_nohf *elm = &nodes[idx - 1];
            struct mynode_nohf *ret =
                myht_nohf_find(&head, bk, nodes, &elm->key);

            if (present[idx]) {
                if (ret != elm)
                    FAILF("nohf fuzz find[%u]: expected %p got %p",
                          idx, (void *)elm, (void *)ret);
            } else {
                if (ret != NULL)
                    FAILF("nohf fuzz find absent[%u]: expected NULL got %p",
                          idx, (void *)ret);
            }
        }

        if ((step & 0x3FFu) == 0u) {
            if (head.rhh_nb != in_table)
                FAILF("nohf fuzz step %u: rhh_nb=%u model=%u",
                      step, head.rhh_nb, in_table);
        }
    }

    if (head.rhh_nb != in_table)
        FAILF("nohf fuzz final: rhh_nb=%u model=%u", head.rhh_nb, in_table);

    unsigned walk_cnt = 0;
    myht_nohf_walk(&head, bk, nodes, walk_nohf_count_cb, &walk_cnt);
    if (walk_cnt != in_table)
        FAILF("nohf fuzz walk: expected %u got %u", in_table, walk_cnt);

    free(present);
    free(bk);
    free(nodes);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int
main(int argc, char **argv)
{
    unsigned seed   = 0xC0FFEE11u;
    unsigned N      = 512u;
    unsigned nb_bk  = 64u;  /* 64 × 16 = 1024 slots; N/capacity = 0.5 */
    unsigned ops    = 200000u;

    if (argc >= 2) seed  = (unsigned)strtoul(argv[1], NULL, 10);
    if (argc >= 3) N     = (unsigned)strtoul(argv[2], NULL, 10);
    if (argc >= 4) nb_bk = (unsigned)strtoul(argv[3], NULL, 10);
    if (argc >= 5) ops   = (unsigned)strtoul(argv[4], NULL, 10);

    rix_hash_arch_init();

    test_init_empty();
    test_insert_find_remove();
    test_duplicate();
    test_staged_find();
    test_walk();
    test_fuzz(seed, N, nb_bk, ops);

    test_nohf_insert_find_remove();
    test_nohf_duplicate();
    test_nohf_fuzz(seed, N, nb_bk, ops);

    printf("ALL RIX_HASH TESTS PASSED\n");
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
