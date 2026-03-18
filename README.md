# librix -- Relative Index Library

Index-based data structures for shared memory.

librix provides relative (index-pointing) implementations of the classic BSD
data structures--SLIST, LIST, STAILQ, TAILQ, CIRCLEQ, Red-Black tree, and
three variants of a high-performance cuckoo hash table--so you can store them
directly in shared memory or mmapped files without embedding raw pointers.

## Table of Contents

- [Why indices?](#why-indices)
- [Design](#design)
- [Repository layout](#repository-layout)
- [Getting started](#getting-started)
- [Common helpers](#common-helpers)
- [Queue structures](#queue-structures)
  - [RIX_SLIST](#rix_slist)
  - [RIX_LIST](#rix_list)
  - [RIX_STAILQ](#rix_stailq)
  - [RIX_TAILQ](#rix_tailq)
  - [RIX_CIRCLEQ](#rix_circleq)
- [Red-Black tree -- RIX_RB](#red-black-tree--rix_rb)
- [Cuckoo hash tables](#cuckoo-hash-tables)
  - [Variant comparison](#variant-comparison)
  - [RIX_HASH (fingerprint, variable-length key)](#rix_hash-fingerprint-variable-length-key)
  - [RIX_HASH32 (uint32_t key)](#rix_hash32-uint32_t-key)
  - [RIX_HASH64 (uint64_t key)](#rix_hash64-uint64_t-key)
- [Flow cache sample](#flow-cache-sample)
- [Build](#build)
- [Concurrency](#concurrency)
- [Testing](#testing)
- [License](#license)

---

## Why indices?

Raw pointers are process-local and non-relocatable.  By storing **unsigned
indices** instead:

- Structures are relocatable across processes and after remapping.
- They live naturally in shared memory and file-backed regions.
- Zero-initialization makes every head/node empty by construction.
- Mixed 32/64-bit processes mapping the same region avoid pointer-size mismatches.

---

## Design

| Concept | Detail |
|---------|--------|
| Nil sentinel | `RIX_NIL = 0` -- the zero value means "no element" |
| 1-origin mapping | valid indices are `1 ... UINT_MAX-1`; `pool[i]` <-> index `i+1` |
| No stored pointers | heads and link fields contain indices only |
| Transient pointers | conversion is done at call time by passing `base` (your element array) |
| Standard | C11; no external dependencies; single-header per subsystem |

Index <-> pointer conversion macros:

```c
RIX_IDX_FROM_PTR(base, p)   /* (p - base) + 1  (NULL -> RIX_NIL) */
RIX_PTR_FROM_IDX(base, i)   /* base + (i-1)    (0 -> NULL)        */
```

---

## Repository layout

```
include/
  librix.h          umbrella header (includes all subsystems)
  rix/
    rix_defs_private.h  common macros, index helpers  (internal; auto-included)
    rix_hash_arch.h     arch dispatch, SIMD helpers   (internal; auto-included)
    rix_queue.h     SLIST / LIST / STAILQ / TAILQ / CIRCLEQ
    rix_tree.h      Red-Black tree
    rix_hash.h      cuckoo hash -- fingerprint variant (variable-length key)
    rix_hash32.h    cuckoo hash -- uint32_t key variant
    rix_hash64.h    cuckoo hash -- uint64_t key variant
    rix_hash_key.h  cuckoo hash -- uint32_t and uint64_t variants combined
samples/
  DESIGN.md         design document
  DESIGN_JP.md      design document (Japanese)
  fcache/           library
    include/
      flow_cache.h            umbrella: includes all three variant headers
      flow_cache_decl.h       shared declarations used by variant headers
      flow4_cache.h           IPv4 5-tuple flow cache  (20-byte key)
      flow6_cache.h           IPv6 5-tuple flow cache  (44-byte key)
      flow_unified_cache.h    IPv4+IPv6 in one table (44-byte key, family field)
    src/
      flow4.c                 IPv4 public wrapper + backend selection
      flow4_backend.c         IPv4 backend template, compiled as gen/sse/avx2/avx512
      flow6.c                 IPv6 public wrapper + backend selection
      flow6_backend.c         IPv6 backend template, compiled as gen/sse/avx2/avx512
      flowu.c                 Unified public wrapper + backend selection
      flowu_backend.c         Unified backend template, compiled as gen/sse/avx2/avx512
      backend.h               backend ops table (internal)
      body.h                  implementation template (internal)
      hash_direct.h           direct-find hash generate helper (internal)
    lib/                      build output (libfcache.a / libfcache.so)
  fcache2/          experimental action-cache redesign (flow4-only)
    include/
      flow_cache2.h          umbrella for fcache2 public headers
      flow4_cache2.h         flow4 action-cache API
    src/
      flow4.c                flow4-only implementation
    lib/                     build output (libfcache2.a / libfcache2.so)
  test/
    fcache_test.c           correctness tests + benchmarks (all 3 variants)
    fcache_test_body.h      template: test + benchmark functions (internal)
    ht4_backend.c           test-only raw-hash template, compiled as gen/sse/avx2/avx512
    ht4.h                   test-only declarations for raw flow4 hash benches
    perf.sh                 perf stat wrapper for single-workload runs
```

---

## Getting started

```c
#include "librix.h"      /* queue + tree + all hash variants */
```

Or include only what you need:

```c
#include "rix/rix_queue.h"   /* queue structures only              */
#include "rix/rix_tree.h"    /* Red-Black tree only                */
#include "rix/rix_hash.h"    /* cuckoo hash (fp)                   */
#include "rix/rix_hash32.h"  /* cuckoo hash (u32 key)              */
#include "rix/rix_hash64.h"  /* cuckoo hash (u64 key)              */
#include "rix/rix_hash_key.h"/* cuckoo hash (u32 + u64, combined)  */
```

### Queue quick start

```c
struct node {
    int value;
    RIX_TAILQ_ENTRY(node) link;
};

RIX_TAILQ_HEAD(qhead);
struct qhead h;
struct node *base;   /* your element array in shared memory */

RIX_TAILQ_INIT(&h);

RIX_TAILQ_INSERT_TAIL(&h, base, &base[0], link);
RIX_TAILQ_INSERT_TAIL(&h, base, &base[1], link);

struct node *it;
RIX_TAILQ_FOREACH(it, &h, base, link) {
    /* use it->value */
}

RIX_TAILQ_REMOVE(&h, base, &base[0], link);
```

---

## Common helpers

Available via any rix public header (defined internally in `rix_defs_private.h`):

```c
RIX_NIL                          /* 0 -- null index */
RIX_IDX_FROM_PTR(base, p)        /* pointer -> index */
RIX_PTR_FROM_IDX(base, i)        /* index -> pointer (NULL if i==0) */
RIX_IDX_IS_NIL(i)                /* i == RIX_NIL */
RIX_IDX_IS_VALID(i, cap)         /* 1 <= i <= cap */

RIX_MIN(a, b)
RIX_MAX(a, b)
RIX_COUNT_OF(arr)
RIX_OFFSET_OF(type, field)
RIX_CONTAINER_OF(ptr, type, field)
RIX_SWAP(a, b)
RIX_CLAMP(v, lo, hi)
RIX_ASSERT(expr)
RIX_STATIC_ASSERT(expr, msg)
```

---

## Queue structures

In all macros below:
- `type` -- your element struct (bare name, no `struct` keyword)
- `field` -- the embedded link field inside `type`
- `base` -- `type *` pointer to the element array
- `head` -- pointer to the container head

### RIX_SLIST

Singly-linked list.  O(1) insert-head, O(n) remove.

```c
/* Declarations */
RIX_SLIST_ENTRY(type)                   /* link field inside struct */
RIX_SLIST_HEAD(name)                    /* declare head type */
RIX_SLIST_HEAD_INITIALIZER(var)         /* static initializer */

/* Init & query */
RIX_SLIST_INIT(head)
RIX_SLIST_EMPTY(head)                   /* 1 if empty */
RIX_SLIST_FIRST(head, base)             /* first element or NULL */
RIX_SLIST_NEXT(elm, base, field)        /* next element or NULL */

/* Modifiers */
RIX_SLIST_INSERT_HEAD(head, base, elm, field)
RIX_SLIST_INSERT_AFTER(base, slistelm, elm, field)
RIX_SLIST_REMOVE_HEAD(head, base, field)
RIX_SLIST_REMOVE_AFTER(base, elm, field)
RIX_SLIST_REMOVE(head, base, elm, type, field)   /* O(n) search */

/* Iteration */
RIX_SLIST_FOREACH(var, head, base, field)
RIX_SLIST_FOREACH_SAFE(var, head, base, field, tvar)
RIX_SLIST_FOREACH_PREVINDEX(var, varidxp, head, base, field)
```

### RIX_LIST

Doubly-linked list with O(1) insert/remove anywhere.

```c
/* Declarations */
RIX_LIST_ENTRY(type)
RIX_LIST_HEAD(name)
RIX_LIST_HEAD_INITIALIZER(var)

/* Init & query */
RIX_LIST_INIT(head)
RIX_LIST_EMPTY(head)
RIX_LIST_FIRST(head, base)
RIX_LIST_NEXT(elm, base, field)

/* Modifiers */
RIX_LIST_INSERT_HEAD(head, base, elm, field)
RIX_LIST_INSERT_AFTER(head, base, listelm, elm, field)
RIX_LIST_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_LIST_REMOVE(head, base, elm, field)
RIX_LIST_SWAP(head1, head2, base, type, field)

/* Iteration */
RIX_LIST_FOREACH(var, head, base, field)
RIX_LIST_FOREACH_SAFE(var, head, base, field, tvar)
```

### RIX_STAILQ

Singly-linked tail queue.  O(1) insert-head and insert-tail.

```c
/* Declarations */
RIX_STAILQ_ENTRY(type)
RIX_STAILQ_HEAD(name)
RIX_STAILQ_HEAD_INITIALIZER(var)

/* Init & query */
RIX_STAILQ_INIT(head)
RIX_STAILQ_EMPTY(head)
RIX_STAILQ_FIRST(head, base)
RIX_STAILQ_LAST(head, base)
RIX_STAILQ_NEXT(head, base, elm, field)

/* Modifiers */
RIX_STAILQ_INSERT_HEAD(head, base, elm, field)
RIX_STAILQ_INSERT_TAIL(head, base, elm, field)
RIX_STAILQ_INSERT_AFTER(head, base, tqelm, elm, field)
RIX_STAILQ_REMOVE_HEAD(head, base, field)
RIX_STAILQ_REMOVE_AFTER(head, base, elm, field)
RIX_STAILQ_REMOVE(head, base, elm, type, field)   /* O(n) search */
RIX_STAILQ_REMOVE_HEAD_UNTIL(head, base, elm, field)
RIX_STAILQ_CONCAT(head1, head2, base, field)
RIX_STAILQ_SWAP(head1, head2, base)

/* Iteration */
RIX_STAILQ_FOREACH(var, head, base, field)
RIX_STAILQ_FOREACH_SAFE(var, head, base, field, tvar)
```

### RIX_TAILQ

Doubly-linked tail queue.  O(1) insert/remove at head, tail, or any position.

```c
/* Declarations */
RIX_TAILQ_ENTRY(type)
RIX_TAILQ_HEAD(name)
RIX_TAILQ_HEAD_INITIALIZER(var)

/* Init & query */
RIX_TAILQ_INIT(head)
RIX_TAILQ_RESET(head)           /* alias of INIT */
RIX_TAILQ_EMPTY(head)
RIX_TAILQ_FIRST(head, base)
RIX_TAILQ_LAST(head, base)
RIX_TAILQ_NEXT(head, base, elm, field)
RIX_TAILQ_PREV(head, base, elm, field)

/* Modifiers */
RIX_TAILQ_INSERT_HEAD(head, base, elm, field)
RIX_TAILQ_INSERT_TAIL(head, base, elm, field)
RIX_TAILQ_INSERT_AFTER(head, base, listelm, elm, field)
RIX_TAILQ_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_TAILQ_REMOVE(head, base, elm, field)
RIX_TAILQ_CONCAT(head1, head2, base, field)
RIX_TAILQ_SWAP(head1, head2, base)

/* Iteration */
RIX_TAILQ_FOREACH(var, head, base, field)
RIX_TAILQ_FOREACH_SAFE(var, head, base, field, tvar)
RIX_TAILQ_FOREACH_REVERSE(var, head, base, field)
```

### RIX_CIRCLEQ

Circular doubly-linked list.  FIRST wraps to LAST; traversal is cyclic.

```c
/* Declarations */
RIX_CIRCLEQ_ENTRY(type)
RIX_CIRCLEQ_HEAD(name)
RIX_CIRCLEQ_HEAD_INITIALIZER(var)

/* Init & query */
RIX_CIRCLEQ_INIT(head)
RIX_CIRCLEQ_EMPTY(head)
RIX_CIRCLEQ_FIRST(head, base)
RIX_CIRCLEQ_LAST(head, base, field)
RIX_CIRCLEQ_NEXT(elm, base, field)
RIX_CIRCLEQ_PREV(elm, base, field)

/* Modifiers */
RIX_CIRCLEQ_INSERT_HEAD(head, base, elm, field)
RIX_CIRCLEQ_INSERT_TAIL(head, base, elm, field)
RIX_CIRCLEQ_INSERT_AFTER(head, base, listelm, elm, field)
RIX_CIRCLEQ_INSERT_BEFORE(head, base, listelm, elm, field)
RIX_CIRCLEQ_REMOVE(head, base, elm, field)

/* Iteration (one full lap) */
RIX_CIRCLEQ_FOREACH(var, head, base, field)
RIX_CIRCLEQ_FOREACH_REVERSE(var, head, base, field)
RIX_CIRCLEQ_FOREACH_SAFE(var, head, base, field, tvar)
RIX_CIRCLEQ_FOREACH_REVERSE_SAFE(var, head, base, field, tvar)
```

---

## Red-Black tree -- RIX_RB

Self-balancing BST.  O(log n) insert, remove, find.

### Quick start

```c
struct rbnode {
    int key;
    RIX_RB_ENTRY(rbnode) rb;
};

static int rb_cmp(const struct rbnode *a, const struct rbnode *b) {
    return (a->key > b->key) - (a->key < b->key);
}

RIX_RB_HEAD(rbtree);
RIX_RB_PROTOTYPE(rbt, rbnode, rb, rb_cmp)
RIX_RB_GENERATE (rbt, rbnode, rb, rb_cmp)

void demo(struct rbtree *rh, struct rbnode *base) {
    RIX_RB_INIT(rh);

    base[0].key = 42;
    RIX_RB_INSERT(rbt, rh, base, &base[0]);

    struct rbnode probe = { .key = 42 };
    struct rbnode *hit = RIX_RB_FIND(rbt, rh, base, &probe);

    struct rbnode *it;
    RIX_RB_FOREACH(it, rbt, rh, base) { /* ascending */ }
}
```

### API reference

```c
/* Declarations & codegen */
RIX_RB_ENTRY(type)
RIX_RB_HEAD(name)
RIX_RB_HEAD_INITIALIZER(var)
RIX_RB_INIT(head)
RIX_RB_PROTOTYPE(name, type, field, cmp)     /* extern declaration */
RIX_RB_GENERATE (name, type, field, cmp)     /* full implementation */

/* Operations */
RIX_RB_INSERT(name, head, base, elm)   /* NULL -> inserted; non-NULL -> duplicate */
RIX_RB_REMOVE(name, head, base, elm)   /* returns removed elm */
RIX_RB_FIND  (name, head, base, key)   /* exact match or NULL */
RIX_RB_NFIND (name, head, base, key)   /* lower bound (first node >= key) */
RIX_RB_MIN   (name, head, base)
RIX_RB_MAX   (name, head, base)
RIX_RB_NEXT  (name, base, elm)
RIX_RB_PREV  (name, base, elm)

/* Iteration */
RIX_RB_FOREACH        (var, name, head, base)   /* ascending  */
RIX_RB_FOREACH_REVERSE(var, name, head, base)   /* descending */
```

Comparator signature: `int cmp(const type *a, const type *b)` -- strict weak ordering.

---

## Cuckoo hash tables

Three header-only, index-based cuckoo hash variants.  All share:

- **16 slots per bucket** (SIMD-parallel slot scan)
- **Runtime SIMD dispatch** -- Generic / SSE / AVX2 / AVX-512 selected per source file via `rix_hash_arch_init(enable)`
- **Two candidate buckets per key** via XOR symmetry -- O(1) remove, no rehash
- **N-ahead pipelined lookup** API -- hides DRAM latency across multiple requests
- **1-origin index storage** -- `RIX_NIL = 0` marks empty slots; no raw pointers

### Variant comparison

| Header | Key storage | Bucket size | Best for |
|--------|------------|-------------|----------|
| `rix_hash.h`   | fingerprint in bucket, full key in node | 128 B (2 CL) | Variable-length / large keys |
| `rix_hash32.h` | `uint32_t` key in bucket               | 128 B (2 CL) | 32-bit integer keys |
| `rix_hash64.h` | `uint64_t` key in bucket               | 192 B (3 CL) | 64-bit integer keys |

Performance (DRAM-cold, 10 M entries, pipelined x8):

| Variant | Cycles/op |
|---------|-----------|
| `rix_hash32` | ~58-60 |
| `rix_hash64` | ~62-66 |
| `rix_hash` (fp) | ~84-88 |

#### Bucket scan performance (`find_u32x16`, 16 slots/bucket, L2-warm)

The innermost hot path is the fingerprint / key scan across 16 slots per bucket.
Measured on a single core with 128 buckets resident in L2 cache:

| Build flags | Runtime level | cy/bucket | Notes |
|-------------|---------------|-----------|-------|
| `-msse4.2`       | GEN (force 0) | 36.0 | pure scalar loop |
| `-msse4.2`       | **SSE**       |  6.3 | XMM 128-bit — ×5.7 vs GEN |
| `-mavx2 -msse4.2`| SSE           |  6.0 | XMM 128-bit |
| `-mavx2 -msse4.2`| **AVX2**      |  3.3 | YMM 256-bit — ×1.8 vs SSE |
| `-mavx2 -msse4.2`| GEN (force 0) |  3.8 | compiler auto-vectorizes to AVX2 * |

\* With `-mavx2` the compiler applies AVX2 auto-vectorization to the GEN scalar
loop itself, so forcing `enable=0` still executes AVX2 instructions.

**Takeaway:** SSE is most beneficial on CPUs with SSE4.2 but without AVX2
(Sandy Bridge / Ivy Bridge, 2011–2012).  On AVX2 or later CPUs the manual AVX2
path is the right choice.

---

### RIX_HASH (fingerprint, variable-length key)

Node struct must include a `hash_field` integer that stores the current-bucket
hash. `SLOT` variants additionally store the current slot in a caller-defined
integer `slot_field`.

```c
#include "rix/rix_hash.h"

/* 1. typedef required -- macro uses the bare identifier */
typedef struct mynode mynode;
struct mynode {
    uint8_t  key[16];    /* variable-length key field */
    uint32_t cur_hash;   /* current-bucket hash (any name) */
    uint32_t value;
};

/* 2. Declare head and generate the API */
RIX_HASH_HEAD(myht);
RIX_HASH_GENERATE(myht, mynode, key, cur_hash, my_cmp_fn)
/* Optional typed hash hook:
 * RIX_HASH_GENERATE_EX(myht, mynode, key, cur_hash,
 *                      my_cmp_fn, my_hash_fn)
 */

typedef struct mynode_slot mynode_slot;
struct mynode_slot {
    uint8_t  key[16];
    uint32_t cur_hash;
    uint16_t slot;
    uint16_t value;
};

RIX_HASH_HEAD(myht_slot);
RIX_HASH_GENERATE_SLOT(myht_slot, mynode_slot, key, cur_hash, slot, my_cmp_fn)

/* 3. Optional: enable SIMD in this source file */
rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

/* 4. Allocate 64-byte-aligned bucket array */
struct rix_hash_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));
memset(buckets, 0, NB_BK * sizeof(*buckets));   /* 0 = all slots empty */
mynode *pool = calloc(N, sizeof(*pool));         /* 1-origin: pool[0] = index 1 */

struct myht head;
RIX_HASH_INIT(&head, NB_BK);   /* NB_BK must be power of 2 */
```

#### Single-shot operations

```c
/* Insert: NULL -> success; other ptr -> duplicate; elm itself -> table full */
mynode *dup = myht_insert(&head, buckets, pool, &pool[i]);

/* Find by key pointer */
mynode *hit = myht_find(&head, buckets, pool, key_ptr);

/* Remove by node pointer */
mynode *rem = myht_remove(&head, buckets, pool, &pool[i]);

/* Walk all entries: cb returns 0 to continue, non-zero to stop */
myht_walk(&head, buckets, pool, cb, arg);
```

#### Pipelined (staged) find

Issue multiple lookups in flight to hide DRAM latency:

```c
struct rix_hash_find_ctx_s ctx[4];
const void *keys[4] = { k0, k1, k2, k3 };
mynode *results[4];

/* Stage 1: hash + bucket prefetch */
myht_hash_key4(ctx, &head, buckets, keys);
/* Stage 2: fingerprint scan */
myht_scan_bk4 (ctx, &head, buckets);
/* Stage 3: full key comparison */
myht_cmp_key4 (ctx, pool, results);
```

Bulk variants: `_key1` / `_key2` / `_key4` / `_key8` (suffix = count).

#### `RIX_HASH_GENERATE` options

| Variant | Macro |
|---------|-------|
| External linkage | `RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)` |
| External linkage + custom hash | `RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)` |
| External linkage + slot-aware remove | `RIX_HASH_GENERATE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)` |
| External linkage + slot-aware remove + custom hash | `RIX_HASH_GENERATE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn)` |
| `static` linkage | `RIX_HASH_GENERATE_STATIC(name, type, key_field, hash_field, cmp_fn)` |
| `static` linkage + custom hash | `RIX_HASH_GENERATE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)` |
| `static` linkage + slot-aware remove | `RIX_HASH_GENERATE_STATIC_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)` |
| `static` linkage + slot-aware remove + custom hash | `RIX_HASH_GENERATE_STATIC_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn)` |

`cmp_fn` signature: `int cmp_fn(const type *a, const type *b)` -- returns 0 if equal.
`hash_fn` signature: `union rix_hash_hash_u hash_fn(const key_type *key, uint32_t mask)`.
`slot_field` may be any caller-defined integer type that can represent
`[0, RIX_HASH_BUCKET_ENTRY_SZ - 1]`.

SLOT variants maintain:

- `node->hash_field & mask == current_bucket`
- `buckets[current_bucket].idx[node->slot_field] == node_idx`

This makes `remove()` direct-slot and avoids the `idx[16]` scan used by the
non-SLOT variants.

---

### RIX_HASH32 (uint32_t key)

No `hash_field` required in the node struct.  The key itself is stored in the
bucket, so `scan_bk` performs exact 32-bit comparison.

```c
#include "rix/rix_hash32.h"

typedef struct entry32 entry32;
struct entry32 {
    uint32_t key;    /* key field -- any name */
    uint32_t value;
};

RIX_HASH32_HEAD(ht32);
RIX_HASH32_GENERATE(ht32, entry32, key)

rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

struct rix_hash32_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));
memset(buckets, 0, NB_BK * sizeof(*buckets));
entry32 *pool = calloc(N, sizeof(*pool));

struct ht32 head;
RIX_HASH32_INIT(&head, NB_BK);
```

#### API

```c
entry32 *ht32_insert(&head, buckets, pool, &pool[i]);
entry32 *ht32_find  (&head, buckets, pool, key_value);   /* key by value */
entry32 *ht32_remove(&head, buckets, pool, key_value);
int      ht32_walk  (&head, buckets, pool, cb, arg);

/* Pipelined find (same stage pattern as rix_hash) */
struct rix_hash32_find_ctx_s ctx[4];
uint32_t keys[4] = { k0, k1, k2, k3 };
entry32 *results[4];

ht32_hash_key4(ctx, &head, buckets, keys);
ht32_scan_bk4 (ctx, &head, buckets);
ht32_cmp_key4 (ctx, pool, results);
```

---

### RIX_HASH64 (uint64_t key)

Same interface as RIX_HASH32 with `uint64_t` keys.  Bucket is 192 B (3 cache lines)
instead of 128 B.

```c
#include "rix/rix_hash64.h"

typedef struct entry64 entry64;
struct entry64 {
    uint64_t key;
    uint32_t value;
};

RIX_HASH64_HEAD(ht64);
RIX_HASH64_GENERATE(ht64, entry64, key)

struct rix_hash64_bucket_s *buckets =
    aligned_alloc(64, NB_BK * sizeof(*buckets));

struct ht64 head;
RIX_HASH64_INIT(&head, NB_BK);

entry64 *ht64_insert(&head, buckets, pool, &pool[i]);
entry64 *ht64_find  (&head, buckets, pool, key_value);
entry64 *ht64_remove(&head, buckets, pool, key_value);
```

Pipelined stages follow the same pattern: `ht64_hash_key4`, `ht64_scan_bk4`,
`ht64_cmp_key4`.

#### Important notes (all hash variants)

- `rix_hash_arch_init(enable)` is optional. Without it, each source file
  stays on the Generic path by default.
- For SIMD acceleration, call `rix_hash_arch_init(enable)` in each source file
  that uses hash operations. Pass `RIX_HASH_ARCH_AUTO` to use the best
  available SIMD level (recommended).
  Pass `RIX_HASH_ARCH_SSE` to cap at SSE XMM (SSE4.2, no AVX2).
  Pass `RIX_HASH_ARCH_AVX2` to cap at AVX2 even if AVX-512 is present.
  Pass `0` to force Generic (scalar) — useful for benchmarking.
- Bucket arrays must be **64-byte aligned** (`aligned_alloc(64, ...)` or `posix_memalign`).
- `NB_BK` must be a **power of 2** and at least 2.
- `insert` return values:
  - `NULL` -- success
  - other pointer -- duplicate (key already present)
  - `elm` itself -- table full (kickout depth exhausted)
- Safe operating fill rate: **<= 80%** of total slots (`NB_BK * 16`).

---

## Flow cache sample

`samples/` provides a complete, production-grade flow cache built on top of
`rix_hash`.  Three variants handle IPv4-only, IPv6-only, and unified IPv4+IPv6
traffic in a single table.

All three variants are built as small fat binaries
(`GEN` + `SSE` + `AVX2` + `AVX512` backends). `*_cache_init()` accepts an explicit
backend request and can also auto-select the best supported backend at
runtime.

### Include

```c
#include "flow_cache.h"   /* umbrella: includes all three variants */
```

### Variants

| Variant | Header | Key | Entry size |
|---------|--------|-----|------------|
| `flow4` | `flow4_cache.h` | IPv4 5-tuple + vrfid (20 B) | 128 B (2 CL) |
| `flow6` | `flow6_cache.h` | IPv6 5-tuple + vrfid (44 B) | 128 B (2 CL) |
| `flowu` | `flow_unified_cache.h` | IPv4 or IPv6 + family field (44 B) | 128 B (2 CL) |

### Lifecycle

```c
/* Allocate resources */
unsigned nb_bk = flow_cache_nb_bk_hint(max_entries); /* ~50% fill sizing */
struct rix_hash_bucket_s *buckets =
    aligned_alloc(64, nb_bk * sizeof(*buckets));
struct flow4_entry *pool =
    aligned_alloc(64, flow_cache_pool_size(max_entries, sizeof(*pool)));
unsigned pool_count = flow_cache_pool_count(max_entries); /* 2^n, min 64 */

/* Initialise */
struct flow4_cache fc;
flow4_cache_init(&fc, buckets, nb_bk, pool, pool_count,
                 FLOW_CACHE_BACKEND_AUTO, /* or GEN / SSE / AVX2 / AVX512 */
                 30000,           /* timeout_ms: 30 seconds */
                 NULL, NULL, NULL); /* init_cb, fini_cb, cb_arg (NULL = no-op fast path) */

printf("backend = %s\n",
       flow_cache_backend_name(flow4_cache_backend(&fc)));

/* Bulk clear (e.g. VRF delete, interface down) */
flow4_cache_flush(&fc);
```

For sample benchmarking/debugging, `samples/test/flow_cache_test` also accepts
`--backend auto|gen|sse|avx2|avx512` and prints the actual backend selected per
variant. For perf-friendly single-workload runs it also supports
`--bench-case`, `--list-bench-cases`, `--json`, `--measure-child`, and
`--pause-before-measure`.
Packet-loop cases include `pkt_hit_only`, `pkt_miss_only`, `pkt_std`
(90% hit / 10% miss), and `pkt_tight`.
For the built-in flow-cache variants, the hash stage also uses fixed-size
CRC32 fast paths for 20-byte and 44-byte keys, avoiding the generic
`hash_bytes()` loop on the hot path.
The insert fast path also warms the two candidate bucket lines before the
duplicate / empty-slot scan.
For miss-heavy batches, `flow*_cache_insert_batch()` precomputes the
miss-group hashes, warms candidate buckets from that plan, and then runs
the hashed insert path without re-hashing each key.
For packet-loop perf cases, confirmed hits also prefetch CL1 before the
post-lookup update, and the benchmark backs off `cache_expire()` by
miss-rate tier: 1 / 2 / 4 / 8 batches. This avoids snapping straight back
to every-batch expire on a small miss count.

### `fcache2` (experimental redesign)

`samples/fcache2/` is a separate flow4-only prototype that keeps the `fcache`
lookup pipeline, but changes only the entry layout and aging policy.

- single-cache-line flow4 entry (64B, CL0-only)
- `RIX_HASH_GENERATE_SLOT_EX` for slot-aware remove
- `lookup_batch()` returns `entry_idx` only; it does not return
  a direct entry pointer or AP payload
- `fill_miss_batch()` only installs missed keys and returns the resulting
  `entry_idx`
- `fc2_flow4_cache_maintain()` scans a caller-selected bucket window for
  idle/background reclaim without forcing a global walk
- current flow4 implementation follows the same 4-stage batch lookup
  pipeline as `fcache` (`hash_key -> scan_bk -> prefetch_node -> cmp_key`)
  and binds the staged fingerprint scans directly to AVX2 helpers
- `fill_miss_batch()` uses the same prehashed, bucket-prefetch insert plan
  shape as `fcache`, then applies local pressure relief before insert
- insert-time local pressure relief is limited to the candidate bucket only:
  it staged-prefetches the occupied entries in that bucket, evaluates the
  full bucket, and reclaims at most one oldest expired victim; the density
  trigger is tightened as global fill rises (`15/16 -> 14/16 -> 13/16`)
- idle/background maintenance uses grouped bucket walks with bucket prefetch
  and staged entry prefetch; each visited bucket evaluates all expired
  entries and removes them via `remove_at()`
- `fc2_flow4_cache_stats()` exposes lookup/fill counts plus local-relief and
  maintenance call/check/eviction counters for verification
- bucket and entry prefetch are routed through shared `rix_hash` helpers,
  so lookup/insert/maintenance paths use the same prefetch vocabulary

`fcache2` intentionally has no built-in hit-path governor and no global expire
walk.  The design goal is to keep pure search equivalent to `fcache`, then
compare only the aging policy:

- local insert-bucket relief (fill-linked `15/16 -> 14/16 -> 13/16` +
  sampled `8-of-16`)
- explicit idle/background maintenance (`16x1`)

Current scope is intentionally narrow: flow4 only, idx-oriented results, and
side-by-side comparison with `fcache`.

For the current `fc2` bench, the preferred entry points are the parameterized
bench modes in `tests/fcache2/fc2_bench`:

```sh
./tests/fcache2/fc2_bench rate_compare <desired_entries> <start_fill_pct> <hit_pct> <pps>
./tests/fcache2/fc2_bench rate_compare_timeout <desired_entries> <start_fill_pct> <hit_pct> <pps> <timeout_ms>
./tests/fcache2/fc2_bench rate_fc2_only <desired_entries> <start_fill_pct> <hit_pct> <pps>
./tests/fcache2/fc2_bench rate_trace_custom <desired_entries> <start_fill_pct> <hit_pct> <pps> <timeout_ms> <soak_mul> <report_ms> <fill0> <fill1> <fill2> <fill3> <k0> <k1> <k2> <k3> [kick_scale]
```

The current accepted fill-control trace profile is:

```text
thresholds: 70 / 73 / 75 / 77
kicks:      0 / 0 / 1 / 2
```

Previously validated combinations can be replayed with:

```sh
./samples/test/run_fc2_bench_matrix.sh flow4
make -C samples/test matrix VARIANT=flow4
make -C samples/fcache2 matrix
make -C tests/fcache2 matrix
```

### Packet processing loop

```c
uint64_t now = flow_cache_rdtsc();

/* 1. Pipelined batch lookup */
flow4_cache_lookup_batch(&fc, keys, nb_pkts, results);

/* 2. Per-packet post-processing */
unsigned misses = 0;
for (unsigned i = 0; i < nb_pkts; i++) {
    if (results[i]) {
        flow4_cache_touch(results[i], now);   /* hit: refresh timestamp */
        /* update userdata directly, e.g. MY_PAYLOAD(results[i])->packets++ */
    } else {
        flow4_cache_insert(&fc, &keys[i], now); /* miss: init_cb fills userdata */
        misses++;
    }
}

/* 3. Adaptive timeout adjustment */
flow4_cache_adjust_timeout(&fc, misses);

/* 4. Aging eviction */
flow4_cache_expire(&fc, now);
/* flow4_cache_expire() auto-switches to the 2stage path under high pressure.
   flow4_cache_expire_2stage() is still available to force that path. */
```

### API summary

```c
/* Lifecycle */
void     flow4_cache_init (fc, buckets, nb_bk, pool, pool_count, backend,
                           timeout_ms, init_cb, fini_cb, cb_arg);
void     flow4_cache_flush(fc);

/* Backend selection / inspection */
typedef enum {
    FLOW_CACHE_BACKEND_AUTO,
    FLOW_CACHE_BACKEND_GEN,
    FLOW_CACHE_BACKEND_SSE,
    FLOW_CACHE_BACKEND_AVX2,
    FLOW_CACHE_BACKEND_AVX512,
} flow_cache_backend_t;
flow_cache_backend_t flow4_cache_backend(fc);    /* actual backend after init */
const char *flow_cache_backend_name(flow_cache_backend_t backend);

/* Sizing helpers */
unsigned flow_cache_pool_count(max_entries);              /* element count for cache_init (2^n, min 64) */
size_t   flow_cache_pool_size (max_entries, entry_size);  /* bytes for aligned_alloc */
unsigned flow_cache_nb_bk_hint(max_entries);              /* bucket count for ~50% fill (2^n) */

/* cache_init input contract:
 *   pool_count must already be 2^n and >= 64
 *   backend request may fall back to GEN if unsupported */

/* Lookup */
void           flow4_cache_lookup_batch(fc, keys, nb_pkts, results);
unsigned       flow4_cache_lookup_touch_batch(fc, keys, nb_pkts, now, results);
struct flow4_entry *flow4_cache_find   (fc, key);   /* single, no pipeline */

/* Mutation */
struct flow4_entry *flow4_cache_insert(fc, key, now);
void                flow4_cache_remove(fc, entry);

/* Hit processing */
void flow4_cache_touch(entry, now);   /* refresh timestamp; update userdata after */

/* Aging */
void flow4_cache_expire         (fc, now);
void flow4_cache_expire_2stage  (fc, now);
void flow4_cache_adjust_timeout (fc, misses);

/* Statistics */
void     flow4_cache_stats     (fc, out);   /* fills struct flow_cache_stats */
unsigned flow4_cache_nb_entries(fc);

/* Timestamp (variant-independent) */
uint64_t flow_cache_rdtsc(void);
uint64_t flow_cache_calibrate_tsc_hz(void);
uint64_t flow_cache_ms_to_tsc(tsc_hz, ms);
```

Replace `flow4` with `flow6` or `flowu` for the other variants.
All three expose the identical API surface.

### FC_CALL -- variant-generic call macro

```c
#include "flow_cache.h"

/* FC_CALL(prefix, suffix) expands to prefix##_##suffix */
FC_CALL(flow4, cache_init)(&fc, buckets, nb_bk, pool, pool_count,
                           FLOW_CACHE_BACKEND_AUTO, timeout_ms,
                           init_cb, fini_cb, cb_arg);
FC_CALL(flow4, cache_lookup_batch)(&fc, keys, nb_pkts, results);
FC_CALL(flow4, cache_insert)(&fc, &keys[i], now);
FC_CALL(flow4, cache_touch)(results[i], now);
FC_CALL(flow4, cache_adjust_timeout)(&fc, misses);
FC_CALL(flow4, cache_expire)(&fc, now);
FC_CALL(flow4, cache_backend)(&fc);

/* prefix can be a macro token -- it is fully expanded first */
#define MY_FC flow6
FC_CALL(MY_FC, cache_init)(&fc, ...);   /* -> flow6_cache_init(...) */
```

### Statistics

```c
struct flow_cache_stats {
    uint64_t lookups;     /* total lookup_batch calls */
    uint64_t hits;        /* cache hits               */
    uint64_t misses;      /* cache misses             */
    uint64_t inserts;     /* entries inserted         */
    uint64_t evictions;   /* entries expired by timeout */
    uint64_t removes;     /* entries explicitly removed (remove/flush) */
    uint32_t nb_entries;  /* current entry count      */
    uint32_t max_entries; /* pool capacity            */
};
```

### Performance (measured, DRAM-cold)

| Operation | Cycles |
|-----------|--------|
| Pipelined batch lookup | 150-220 cy/pkt |
| Expire (amortised) | ~10-20 cy/pkt |

---

## Build

C11 required.  Suggested flags:

```sh
# AVX2 (recommended default)
cc -std=gnu11 -O3 -mavx2 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror  \
   -I/path/to/librix/include       \
   your_sources.c

# AVX-512
cc -std=gnu11 -O3 -mavx512f -mavx2 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror             \
   -I/path/to/librix/include                  \
   your_sources.c

# Generic scalar only (no SIMD search; CRC32C hash retained)
cc -std=gnu11 -O3 -msse4.2 \
   -Wall -Wextra -Wshadow -Werror \
   -I/path/to/librix/include      \
   your_sources.c
```

When using the bundled test/benchmark suite, the `SIMD=` make variable
selects the SIMD level for all sub-targets:

```sh
make SIMD=avx2    # default
make SIMD=avx512
make SIMD=gen
```

Compiler and optimisation level are also overridable:

```sh
make CC=gcc   OPTLEVEL=3
make CC=clang OPTLEVEL=3
```

The current tree is expected to build with both GCC and Clang in this mode.

For `samples/fcache`, the lookup pipeline constants default to
`FLOW_CACHE_LOOKUP_STEP_KEYS=16`,
`FLOW_CACHE_LOOKUP_AHEAD_STEPS=8`, and
`FLOW_CACHE_LOOKUP_AHEAD_KEYS=128`.
`AHEAD_KEYS` is the software-pipeline stage distance, not a count of
hardware prefetch requests.
You can override them for tuning runs via `EXTRA_CFLAGS`:

```sh
make -C samples/fcache static CC=gcc OPTLEVEL=3 \
     EXTRA_CFLAGS='-DFLOW_CACHE_LOOKUP_STEP_KEYS=8 -DFLOW_CACHE_LOOKUP_AHEAD_KEYS=64'
make -C samples/test all CC=gcc OPTLEVEL=3 \
     EXTRA_CFLAGS='-DFLOW_CACHE_LOOKUP_STEP_KEYS=8 -DFLOW_CACHE_LOOKUP_AHEAD_KEYS=64'
```

For address/UB sanitizers during development:

```sh
-fsanitize=address,undefined -fno-omit-frame-pointer
```

---

## Concurrency

librix has no internal synchronization.  All structures are suitable for
single-threaded use or for multi-reader/single-writer access under your own
locking (futex, pthread mutex, process-shared primitives, ...).

Lock-free / RCU operation is out of scope and would require additional design.

---

## Testing

```sh
# Queue / tree / hash unit tests
make -C tests

# Flow cache correctness tests + benchmarks
make -C samples
./samples/test/flow_cache_test -n 1000000

# Single workload for perf stat / perf record
./samples/test/flow_cache_test --bench-case flow4:pkt_std --backend avx2 --json

# Pause once just before the measured loop, then attach perf manually
./samples/test/flow_cache_test --bench-case flow4:pkt_hit_only \
    --backend avx2 --pause-before-measure --json

# Wrapper around perf stat
make -C samples/test perf PERF_CASE=flow4:pkt_std PERF_BACKEND=avx2
```

Test coverage includes:

- Empty / singleton / multi-element transitions for every operation
- All insert/remove variants
- Safe iteration while removing elements
- Red-Black invariants (root black, no red-red, equal black height)
- Hash table: duplicate detection, staged pipeline correctness, walk count
- Fuzz: random insert/find/remove verified against a reference model
- Flow cache: find, remove, flush, expire, batch lookup, insert exhaustion

---

## License

BSD 3-Clause.  See [LICENSE](LICENSE).

---

## Acknowledgements

Queue and tree APIs mirror the BSD `sys/queue.h` / `sys/tree.h` interfaces,
replacing raw pointers with indices to enable robust shared-memory deployments.

The cuckoo hash table follows the XOR-based two-bucket scheme for O(1)
amortised insert with prefetch-driven staged lookup.
