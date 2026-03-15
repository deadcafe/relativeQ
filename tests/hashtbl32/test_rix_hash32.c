/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * Unit tests for rix_hash32.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "rix_hash32.h"

/*---------------------------------------------------------------------------
 * Test node definition
 *---------------------------------------------------------------------------*/
typedef struct mynode_s {
    uint32_t key;
    uint32_t val;
} mynode_t;

/*---------------------------------------------------------------------------
 * Test helpers
 *---------------------------------------------------------------------------*/
#define NB_BUCKETS   256          /* must be power of 2 */
#define NB_NODES     (NB_BUCKETS * 16 / 2)  /* ~50% load factor */
#define INVALID_KEY  0xFFFFFFFFu  /* sentinel; never used as a real key */

RIX_HASH32_HEAD(myht32);
RIX_HASH32_GENERATE(myht32, mynode_t, key, INVALID_KEY)

static struct myht32              head;
static struct rix_hash32_bucket_s buckets[NB_BUCKETS];
static mynode_t                   nodes[NB_NODES];

static void
reset_table(void)
{
    RIX_HASH32_INIT(myht32, &head, buckets, NB_BUCKETS);
    memset(nodes, 0, sizeof(nodes));
}

#define PASS(fmt, ...) printf("  PASS: " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) do {                                                    \
    printf("  FAIL: " fmt " [%s:%d]\n", ##__VA_ARGS__, __FILE__, __LINE__);   \
    abort();                                                                   \
} while (0)

/*---------------------------------------------------------------------------
 * Test: basic insert and find
 *---------------------------------------------------------------------------*/
static void
test_insert_find(void)
{
    printf("[test_insert_find]\n");
    reset_table();

    nodes[0].key = 42;
    nodes[0].val = 99;

    mynode_t *r = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);
    if (r != NULL)
        FAIL("insert returned non-NULL: %p", (void *)r);

    if (head.rhh_nb != 1)
        FAIL("rhh_nb expected 1, got %u", head.rhh_nb);

    mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 42);
    if (f != &nodes[0])
        FAIL("find returned wrong node: %p vs %p", (void *)f, (void *)&nodes[0]);

    if (f->val != 99)
        FAIL("val mismatch: %u", f->val);

    PASS("insert and find key=42");
}

/*---------------------------------------------------------------------------
 * Test: find miss
 *---------------------------------------------------------------------------*/
static void
test_find_miss(void)
{
    printf("[test_find_miss]\n");
    reset_table();

    nodes[0].key = 10;
    RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);

    mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 999);
    if (f != NULL)
        FAIL("find_miss returned non-NULL: %p", (void *)f);

    PASS("find miss returns NULL");
}

/*---------------------------------------------------------------------------
 * Test: duplicate insert returns existing node
 *---------------------------------------------------------------------------*/
static void
test_duplicate_insert(void)
{
    printf("[test_duplicate_insert]\n");
    reset_table();

    nodes[0].key = 7;
    nodes[1].key = 7;

    mynode_t *r0 = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);
    if (r0 != NULL)
        FAIL("first insert returned non-NULL: %p", (void *)r0);

    mynode_t *r1 = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[1]);
    if (r1 != &nodes[0])
        FAIL("duplicate insert returned %p, expected %p",
             (void *)r1, (void *)&nodes[0]);

    if (head.rhh_nb != 1)
        FAIL("rhh_nb should be 1 after dup insert, got %u", head.rhh_nb);

    PASS("duplicate insert returns existing node");
}

/*---------------------------------------------------------------------------
 * Test: remove
 *---------------------------------------------------------------------------*/
static void
test_remove(void)
{
    printf("[test_remove]\n");
    reset_table();

    nodes[0].key = 55;
    RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);

    mynode_t *r = RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[0]);
    if (r != &nodes[0])
        FAIL("remove returned wrong node: %p", (void *)r);

    if (head.rhh_nb != 0)
        FAIL("rhh_nb expected 0 after remove, got %u", head.rhh_nb);

    mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 55);
    if (f != NULL)
        FAIL("find after remove returned non-NULL: %p", (void *)f);

    PASS("remove and subsequent find");
}

/*---------------------------------------------------------------------------
 * Test: remove not-in-table returns NULL
 *---------------------------------------------------------------------------*/
static void
test_remove_miss(void)
{
    printf("[test_remove_miss]\n");
    reset_table();

    nodes[0].key = 3;
    /* not inserted */

    mynode_t *r = RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[0]);
    if (r != NULL)
        FAIL("remove_miss returned non-NULL: %p", (void *)r);

    PASS("remove of not-in-table returns NULL");
}

/*---------------------------------------------------------------------------
 * Test: invalid_key slots are written correctly by name_init
 *---------------------------------------------------------------------------*/
static void
test_init_invalid_key(void)
{
    printf("[test_init_invalid_key]\n");
    reset_table();

    /* After init, every key slot must hold INVALID_KEY and every idx RIX_NIL */
    unsigned bad_key = 0, bad_idx = 0;
    for (unsigned b = 0; b < NB_BUCKETS; b++) {
        for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++) {
            if (buckets[b].key[s] != INVALID_KEY) bad_key++;
            if (buckets[b].idx[s] != (uint32_t)RIX_NIL) bad_idx++;
        }
    }
    if (bad_key)
        FAIL("init: %u slots have key != INVALID_KEY", bad_key);
    if (bad_idx)
        FAIL("init: %u slots have idx != RIX_NIL", bad_idx);

    PASS("name_init fills all slots with invalid_key / RIX_NIL");
}

/*---------------------------------------------------------------------------
 * Test: remove restores invalid_key (not 0) to the freed slot
 *---------------------------------------------------------------------------*/
static void
test_remove_restores_invalid_key(void)
{
    printf("[test_remove_restores_invalid_key]\n");
    reset_table();

    nodes[0].key = 42;
    RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);
    RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[0]);

    /* All key slots must be INVALID_KEY again */
    unsigned bad = 0;
    for (unsigned b = 0; b < NB_BUCKETS; b++)
        for (unsigned s = 0; s < RIX_HASH_BUCKET_ENTRY_SZ; s++)
            if (buckets[b].idx[s] == (uint32_t)RIX_NIL &&
                buckets[b].key[s] != INVALID_KEY)
                bad++;
    if (bad)
        FAIL("remove: %u empty slots still have non-invalid key", bad);

    PASS("remove restores invalid_key to freed slot");
}

/*---------------------------------------------------------------------------
 * Test: key = 0 is a valid key when invalid_key = UINT32_MAX
 *---------------------------------------------------------------------------*/
static void
test_key_zero(void)
{
    printf("[test_key_zero]\n");
    reset_table();   /* invalid_key = UINT32_MAX; key=0 is valid */

    nodes[0].key = 0;
    nodes[0].val = 123;

    mynode_t *r = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[0]);
    if (r != NULL)
        FAIL("insert key=0 returned non-NULL: %p", (void *)r);

    mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 0);
    if (f != &nodes[0])
        FAIL("find key=0 returned wrong node: %p", (void *)f);

    mynode_t *rem = RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[0]);
    if (rem != &nodes[0])
        FAIL("remove key=0 returned wrong node: %p", (void *)rem);

    mynode_t *f2 = RIX_HASH32_FIND(myht32, &head, buckets, nodes, 0);
    if (f2 != NULL)
        FAIL("find key=0 after remove returned non-NULL: %p", (void *)f2);

    PASS("key=0 insert/find/remove (invalid_key=UINT32_MAX)");
}

/*---------------------------------------------------------------------------
 * Test: walk
 *---------------------------------------------------------------------------*/
static int
walk_cb(mynode_t *node, void *arg)
{
    unsigned *cnt = (unsigned *)arg;
    (*cnt)++;
    (void)node;
    return 0;
}

static void
test_walk(void)
{
    printf("[test_walk]\n");
    reset_table();

    /* Insert 8 nodes with distinct keys */
    for (int i = 0; i < 8; i++) {
        nodes[i].key = (uint32_t)(100 + i);
        mynode_t *r  = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[i]);
        if (r != NULL)
            FAIL("insert[%d] returned non-NULL", i);
    }

    unsigned cnt = 0;
    int wret = RIX_HASH32_WALK(myht32, &head, buckets, nodes, walk_cb, &cnt);
    if (wret != 0)
        FAIL("walk returned non-zero: %d", wret);
    if (cnt != 8)
        FAIL("walk visited %u nodes, expected 8", cnt);

    PASS("walk visits all %u inserted nodes", cnt);
}

/*---------------------------------------------------------------------------
 * Test: bulk insert / find at ~50% load
 *---------------------------------------------------------------------------*/
static void
test_bulk(void)
{
    printf("[test_bulk]\n");
    reset_table();

    unsigned inserted = 0;

    for (unsigned i = 0; i < NB_NODES; i++) {
        nodes[i].key = i + 1;   /* avoid key=0 here */
        nodes[i].val = i * 2;
        mynode_t *r = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[i]);
        if (r == NULL) {
            inserted++;
        } else if (r == &nodes[i]) {
            /* table full */
            break;
        }
        /* r == other: duplicate (should not happen with unique keys) */
    }

    if (inserted != head.rhh_nb)
        FAIL("rhh_nb mismatch: inserted=%u rhh_nb=%u", inserted, head.rhh_nb);

    /* Verify all inserted nodes are findable */
    unsigned found = 0;
    for (unsigned i = 0; i < inserted; i++) {
        mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, nodes[i].key);
        if (f == NULL)
            FAIL("find failed for key=%u (i=%u)", nodes[i].key, i);
        if (f->val != nodes[i].val)
            FAIL("val mismatch for key=%u: got %u, expected %u",
                 nodes[i].key, f->val, nodes[i].val);
        found++;
    }

    PASS("bulk: inserted=%u found=%u load=%.1f%%",
         inserted, found, 100.0 * inserted / (NB_BUCKETS * 16));
}

/*---------------------------------------------------------------------------
 * Test: staged find x4
 *---------------------------------------------------------------------------*/
static void
test_staged_x4(void)
{
    printf("[test_staged_x4]\n");
    reset_table();

    /* Insert 4 nodes */
    for (int i = 0; i < 4; i++) {
        nodes[i].key = (uint32_t)(200 + i);
        nodes[i].val = (uint32_t)(i + 1);
        mynode_t *r  = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[i]);
        if (r != NULL)
            FAIL("insert[%d] failed", i);
    }

    /* Staged x4 find */
    struct rix_hash32_find_ctx_s ctx[4];
    uint32_t keys[4] = { 200, 201, 202, 203 };
    mynode_t *results[4];

    RIX_HASH32_HASH_KEY4(myht32, ctx, &head, buckets, keys);
    RIX_HASH32_SCAN_BK4(myht32, ctx, &head, buckets);
    RIX_HASH32_PREFETCH_NODE4(myht32, ctx, nodes);
    RIX_HASH32_CMP_KEY4(myht32, ctx, nodes, results);

    for (int i = 0; i < 4; i++) {
        if (results[i] != &nodes[i])
            FAIL("staged x4: result[%d]=%p expected=%p",
                 i, (void *)results[i], (void *)&nodes[i]);
    }

    PASS("staged find x4 returns correct nodes");
}

/*---------------------------------------------------------------------------
 * Test: remove all inserted nodes
 *---------------------------------------------------------------------------*/
static void
test_remove_all(void)
{
    printf("[test_remove_all]\n");
    reset_table();

    int n = 32;
    for (int i = 0; i < n; i++) {
        nodes[i].key = (uint32_t)(500 + i);
        mynode_t *r  = RIX_HASH32_INSERT(myht32, &head, buckets, nodes, &nodes[i]);
        if (r != NULL)
            FAIL("insert[%d] failed", i);
    }

    for (int i = 0; i < n; i++) {
        mynode_t *r = RIX_HASH32_REMOVE(myht32, &head, buckets, nodes, &nodes[i]);
        if (r != &nodes[i])
            FAIL("remove[%d] returned %p, expected %p",
                 i, (void *)r, (void *)&nodes[i]);
    }

    if (head.rhh_nb != 0)
        FAIL("rhh_nb should be 0 after removing all, got %u", head.rhh_nb);

    /* Table should be empty: all finds return NULL */
    for (int i = 0; i < n; i++) {
        mynode_t *f = RIX_HASH32_FIND(myht32, &head, buckets, nodes, nodes[i].key);
        if (f != NULL)
            FAIL("find after remove_all key=%u returned non-NULL",
                 nodes[i].key);
    }

    PASS("remove_all: %d nodes inserted and removed, table empty", n);
}

/*---------------------------------------------------------------------------
 * main
 *---------------------------------------------------------------------------*/
int
main(void)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    printf("=== rix_hash32 tests ===\n");

    test_insert_find();
    test_find_miss();
    test_duplicate_insert();
    test_remove();
    test_remove_miss();
    test_init_invalid_key();
    test_remove_restores_invalid_key();
    test_key_zero();
    test_walk();
    test_bulk();
    test_staged_x4();
    test_remove_all();

    printf("ALL RIX_HASH32 TESTS PASSED\n");
    return 0;
}
