# Flow Cache Design Document

## 1. Overview

Flow cache for high-performance packet processing.
Caches results of slow-path lookups (ACL, QoS, etc.) keyed by
5-tuple + vrfid.  Designed as a demonstration of the librix
index-based data structures (`rix_hash.h`).

### Usage scenario

- Called immediately after L3/L4 validity checks
- Packets are vector-processed (~256 packets per batch)
- Pipeline-style processing to hide DRAM latency
- Per-thread instance (lock-free, no synchronization)
- Target: VPP original plugin, shared memory applications

### Performance targets and measured results

| Operation | Condition | Target (cy/op) | Measured (cy/op) |
|---|---|---|---|
| find (pipelined) | DRAM cold buckets | ~100 | 55-73 |
| insert (on miss) | bucket L2 warm (post-find) | ~60 | 75-245 |
| touch (on hit) | node warm (post-prefetch) | ~10 | ~10 |
| expire scan step | sequential (HW prefetcher) | ~10 | 75-133 |
| expire amortized/pkt | 256 steps, few evicts | ~15-25 | 8-12 |
| pkt loop (tight) | lookup+insert+expire | ~200-250 | 102-158 |

Measured at 100K entries on Xeon.  Pipeline effectively hides
DRAM latency: single find costs ~388 cy/key (DRAM-cold) vs
~57 cy/key pipelined — a 6.8x improvement.

### Xeon throughput budget

At 2 GHz Xeon, 150 cy/pkt = 13.3 Mpps per core.
2 Mpps uses ~15% of one core — adequate for most deployments.

## 2. Three Operating Modes

The flow cache is provided in three variants to match deployment needs.
All variants share the same implementation via macro templates
(see §12 Template Architecture).

### 2.1 Separate tables (IPv4-only or IPv6-only)

| | `flow4_cache` | `flow6_cache` |
|---|---|---|
| Header | `flow4_cache.h` | `flow6_cache.h` |
| Key struct | `flow4_key` (20B) | `flow6_key` (44B) |
| Entry size | 128B (2 CL) | 128B (2 CL) |
| Key comparison | 6-field equality | memcmp×2 + 4-field |
| Use case | IPv4-only environments | IPv6-only environments |

Rationale for separate tables:
- Fixed key size per variant enables compiler-optimized cmp_fn
- No v4/v6 branch in hot path
- Uniform element pool (no per-entry size discrimination)
- IPv4-only: key is 20B, saving 24B hash input per lookup

### 2.2 Unified table (dual-stack)

| | `flowu_cache` |
|---|---|
| Header | `flow_unified_cache.h` |
| Key struct | `flowu_key` (44B) |
| Entry size | 128B (2 CL) |
| Key comparison | `memcmp(a, b, 44)` |
| Use case | Dual-stack (IPv4 + IPv6 in single table) |

The unified key uses a `family` field (1 byte) at offset 0:

```c
struct flowu_key {
    uint8_t  family;       /*  1B: FLOW_FAMILY_IPV4(4) / IPV6(6) */
    uint8_t  proto;        /*  1B */
    uint16_t src_port;     /*  2B */
    uint16_t dst_port;     /*  2B */
    uint16_t pad;          /*  2B */
    uint32_t vrfid;        /*  4B */
    union {                /* 32B */
        struct { uint32_t src, dst; uint8_t _pad[24]; } v4;
        struct { uint8_t  src[16], dst[16]; }           v6;
    } addr;
};  /* 44B total */
```

Properties:
- IPv4 entries zero-pad the unused 24 bytes (`_pad`)
- `family` is part of the key, so v4/v6 with same ports/proto never collide
- `memcmp(a, b, 44)` is a single comparison for both families
- Helper functions `flowu_key_v4()` / `flowu_key_v6()` for key construction
- Pool capacity = v4 + v6 combined — no separate sizing needed
- Single hash table, single bucket array, single free list

### 2.3 Performance comparison

Measured at 1K entries (tight sizing, pkt loop):

| Metric | IPv4 | IPv6 | Unified |
|---|---|---|---|
| lookup (100% hit) | 44 cy/key | 54 cy/key | 52 cy/key |
| pkt loop | 134 cy/pkt | 143 cy/pkt | 146 cy/pkt |
| expire amortized | 9.0 cy/pkt | 7.8 cy/pkt | 8.6 cy/pkt |

Unified performs within ~10% of IPv6-only.  The 24B padding in
IPv4 keys adds negligible hash/compare overhead because the
pipeline hides DRAM latency — the bottleneck is memory access,
not computation.

### 2.4 Choosing a variant

- **IPv4-only** (`flow4_cache`): Legacy or IPv4-exclusive network segments.
  Smallest key (20B), fastest per-key hash computation.
- **IPv6-only** (`flow6_cache`): IPv6-exclusive segments.
- **Unified** (`flowu_cache`): Dual-stack.  Single pool simplifies
  memory management and avoids separate capacity planning for v4/v6.
  Recommended for new deployments.

## 3. Key Structures

### 3.1 IPv4 flow key (20 bytes)

```c
struct flow4_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
};
```

### 3.2 IPv6 flow key (44 bytes)

```c
struct flow6_key {
    uint8_t  src_ip[16];
    uint8_t  dst_ip[16];
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
};
```

### 3.3 Unified flow key (44 bytes)

See §2.2 above.

## 4. Node Layout

All three variants use the same 128-byte (2 CL) entry layout.

### 4.1 Design principles

- CL0 (bytes 0-63): fields accessed during pipelined lookup (hot)
- CL1 (bytes 64-127): fields accessed only on hit (warm) or eviction (cold)
- Node aligned to 64 bytes (`__attribute__((aligned(64)))`)
- `free_link` in CL1 — entry is either in hash table or on free list

### 4.2 Entry layout (all variants)

```
CL0 (64 bytes):
  key           20B (v4) / 44B (v6, unified)
  cur_hash       4B   hash_field for O(1) remove
  action         4B   cached ACL result
  qos_class      4B   cached QoS class
  flags          4B   VALID, etc.
  reserved      28B (v4) / 4B (v6, unified)   pad to 64B

CL1 (64 bytes):
  last_ts        8B   last access TSC
  packets        8B   packet counter
  bytes          8B   byte counter
  free_link      4B   SLIST entry (free list index)
  reserved      36B   pad to 64B
```

Total: 128 bytes = 2 cache lines, for all three variants.

IPv6 key (44B) fits in CL0 with 4B reserved.
IPv4 key (20B) fits in CL0 with 28B reserved — more padding but
same entry size, so pool memory is identical.

## 5. Hash Table Configuration

- Hash variant: `rix_hash.h` fingerprint (arbitrary key size, cmp_fn)
- `RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)`
- `hash_field` (`cur_hash`) enables O(1) remove (no rehash)
- Kickout uses XOR trick: `alt_bk = (fp ^ hash_field) & mask`
- Bucket: 128 bytes (2 CL), 16 slots/bucket
- Runtime SIMD dispatch: Generic / AVX2 / AVX-512

### 5.1 Table sizing

Auto-sizing targets ~50% fill: `nb_bk = next_pow2(max_entries * 2 / 16)`.

| max_entries | nb_bk | total slots | bucket mem | node mem (128B) | total |
|---|---|---|---|---|---|
| 100K | 16K | 256K | 2 MB | 12.5 MB | 14.5 MB |
| 1M | 128K | 2M | 16 MB | 128 MB | 144 MB |
| 4M | 512K | 8M | 64 MB | 512 MB | 576 MB |
| 10M | 2M | 32M | 256 MB | 1.25 GB | 1.5 GB |

Per-thread allocation.

### 5.2 bk[0] placement rate vs fill rate

At ≤75% fill, 98%+ of entries reside in their primary bucket (bk[0]).
`scan_bk` touches only 1 CL for these entries, minimizing DRAM access.

| Fill % | bk[0] % | bk[1] % |
|---|---|---|
| 10-50 | 99.5-100 | 0-0.5 |
| 60 | 99.7 | 0.3 |
| 70 | 99.2 | 0.8 |
| 75 | 98+ | ~2 |
| 80 | 96 | 4 |
| 90 | 91 | 9 |

This is why the 75% threshold is important (§8).

## 6. Pipeline Design

### 6.1 Lookup pipeline

`rix_hash.h` staged find has 4 stages:

```
Stage 0: hash_key       compute hash, prefetch bucket[0] CL0
Stage 1: scan_bk        SIMD fingerprint scan in bucket[0]
Stage 2: prefetch_node  prefetch flow_entry CL0 for candidates
Stage 3: cmp_key        full key comparison, return hit/NULL
```

### 6.2 N-ahead software pipeline

Process nb_pkts packets with BATCH-wide steps, KPD batches ahead:

```
BATCH = 8          (keys processed per step)
KPD   = 8          (pipeline depth in batches)
DIST  = BATCH*KPD  (= 64 keys ahead)
```

```c
for (i = 0; i < nb_pkts + 3 * DIST; i += BATCH) {
    if (i                  < nb_pkts) hash_key_n      (i,        BATCH);
    if (i >= DIST       && ...)       scan_bk_n       (i-DIST,   BATCH);
    if (i >= 2*DIST     && ...)       prefetch_node_n (i-2*DIST, BATCH);
    if (i >= 3*DIST     && ...)       cmp_key_n       (i-3*DIST, BATCH);
}
```

### 6.3 Timestamp

- TSC (`rdtsc`) read **once** before the lookup loop
- Same `now` value used for all packets in the vector
- No per-packet TSC read
- Calibrated at init time via 50 ms sleep

## 7. Hit / Miss Processing

### 7.1 Miss-on-find: immediate insert with empty action

On cache miss, insert a new entry **immediately** (not deferred to
slow path).  The entry is created with `action = FLOW_ACTION_NONE`.

Rationale:
- The lookup just ran; bucket cache lines are still in L2 (~5 cy
  access vs ~200 cy DRAM).  Insert cost drops significantly.
- The flow is now in the cache.  Subsequent packets for the same
  flow will hit immediately (even before ACL fills in the action).
- The slow-path ACL/QoS lookup runs later and **updates** the
  existing entry's action/qos_class fields in-place.

### 7.2 Processing flow

```
process_vector(pkts[256]):

  now = rdtsc()                             // 1 TSC read per vector

  Phase 1: pipelined lookup                 // ~55-73 cy/pkt
    cache_lookup_batch(keys, results)

  Phase 2: process results                  // sequential
    for each pkt:
      if hit:
        cache_touch(entry, now, len)        // ~10 cy (CL1 warm)
        if entry->action != NONE:
          apply cached action/qos           // fast path
        else:
          enqueue to slow-path              // action not yet filled
      if miss:
        cache_insert(fc, key, now)          // ~60 cy (bucket L2 warm)
        enqueue to slow-path                // ACL/QoS lookup needed

  Phase 3: threshold expire                 // ~8-12 cy/pkt amortized
    if cache_over_threshold(fc):            // shift-based 75% check
      cache_expire(fc, now, nb_pkts)

  (slow path: ACL/QoS lookup, then update entry->action/qos_class)
```

### 7.3 Slow-path update

After ACL/QoS lookup completes, the existing cache entry is
updated in-place:

```c
cache_update_action(entry, action, qos_class);
```

This is a CL0 write.  No hash table manipulation needed.

## 8. Eviction Strategy

### 8.1 No TAILQ for LRU

TAILQ-based LRU requires write to prev/next nodes on every hit.
These writes touch random cold cache lines (prev/next neighbors),
destroying pipeline efficiency.

Instead: **timestamp-only approach**.

- Hit: write `last_ts = now` to the hit node's CL1 (already warm)
- No linked-list manipulation on hit path
- Eviction finds victims by scanning

### 8.2 Two-tier eviction

#### Tier 1: Threshold expire (post-batch)

After each 256-packet batch, check if hash table fill exceeds ~75%:

```c
static inline int
cache_over_threshold(const struct cache *fc)
{
    unsigned total_slots = (fc->ht_head.rhh_mask + 1u) << 4;
    return fc->ht_head.rhh_nb >= total_slots - (total_slots >> 2);
}
```

If over threshold, call `cache_expire(fc, now, nb_pkts)` to scan
up to nb_pkts pool entries and remove expired ones.

Properties:
- Shift-based check (no division): `total_slots - (total_slots >> 2)`
  = 3/4 × total_slots
- Only fires when fill ≥ 75% — in standard sizing (~50% fill), this
  never triggers
- Bounded to 256 entries per call — amortized cost: ~8-12 cy/pkt
- Keeps bk[0] placement rate at 98%+ (see §5.2)

#### Tier 2: Evict-one on free-list exhaustion

When free list is empty (all pool entries in use) and insert is needed:

```c
static struct entry *
cache_evict_one(struct cache *fc, uint64_t now)
{
    for (i = 0; i < fc->max_entries; i++) {
        entry = &pool[fc->age_cursor % fc->max_entries];
        fc->age_cursor++;
        if (entry is VALID && now - entry->last_ts > timeout_tsc) {
            hash_remove(entry);
            return entry;   /* reuse for new flow */
        }
    }
    return NULL;  /* all entries live, insert fails */
}
```

This is slow path — rare when threshold expire is active.

#### Both tiers share

- Same `age_cursor` (per-thread, lock-free)
- Same `timeout_tsc` parameter
- Same `cache_expire()` / `hash_remove()` functions

### 8.3 Why no unconditional expire per vector

In standard sizing (pool ≈ 50% fill), fill never reaches 75%,
so threshold expire never fires.  Unconditional expire would waste
cycles scanning entries that are all live.  The threshold check
(`cache_over_threshold`) is a single comparison — zero cost when
not needed.

### 8.4 Sweep coverage

- Cursor advances by max_expire per call (even if fewer expired)
- Full pool traversal period = max_entries / max_expire calls
- At 256 pkts/vector, 10 Gbps, ~20-50 us/vector:
  - 1M pool: full sweep in 4096 vectors ~ 80-200 ms
  - 10M pool: full sweep in ~40K vectors ~ 0.8-2 s
- For timeout N seconds, sweep period must be < N.
  At typical sizes, any timeout >= 1 s is fine.

### 8.5 Timeout parameter

```c
void cache_init(..., uint64_t timeout_ms);
```

Timeout value is a runtime parameter.
Appropriate value depends on workload:
- Short-lived flows (web): 1-5 seconds
- Long-lived flows (streaming): 30-60 seconds
- 0 = no expiry (`timeout_tsc = UINT64_MAX`)

## 9. Free List Management

- Pre-allocated pool of entry[max_entries]
- At init: all entries pushed to SLIST free list
- insert: pop from free list → if empty, evict_one
- expire/evict: push back to free list
- `free_link` field in CL1 (entry is either in hash table or on
  free list, never both)

## 10. API

All three variants expose the same API shape via macro templates.
Function names use the variant prefix: `flow4_`, `flow6_`, or `flowu_`.

```c
/* Initialization — caller provides pre-allocated memory */
void PREFIX_cache_init(struct PREFIX_cache *fc,
                       struct rix_hash_bucket_s *buckets,
                       unsigned nb_bk,
                       struct PREFIX_entry *pool,
                       unsigned max_entries,
                       uint64_t timeout_ms);

/* Batch lookup — pipelined hot path */
void PREFIX_cache_lookup_batch(struct PREFIX_cache *fc,
                               const struct PREFIX_key *keys,
                               unsigned nb_pkts,
                               struct PREFIX_entry **results);

/* Insert — called on miss, bucket L2 warm from lookup */
struct PREFIX_entry *PREFIX_cache_insert(struct PREFIX_cache *fc,
                                         const struct PREFIX_key *key,
                                         uint64_t now);

/* Update action — called after slow-path ACL/QoS lookup */
static inline void
PREFIX_cache_update_action(struct PREFIX_entry *entry,
                           uint32_t action, uint32_t qos_class);

/* Touch — update timestamp + counters (inline, per hit) */
static inline void
PREFIX_cache_touch(struct PREFIX_entry *entry,
                   uint64_t now, uint32_t pkt_len);

/* Threshold check — shift-based ~75% fill test */
static inline int
PREFIX_cache_over_threshold(const struct PREFIX_cache *fc);

/* Aging expire — bounded to max_expire entries */
void PREFIX_cache_expire(struct PREFIX_cache *fc,
                         uint64_t now, unsigned max_expire);

/* Statistics snapshot */
void PREFIX_cache_stats(const struct PREFIX_cache *fc,
                        struct flow_cache_stats *out);
```

### Unified-specific helpers

```c
struct flowu_key flowu_key_v4(uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               uint8_t proto, uint32_t vrfid);

struct flowu_key flowu_key_v6(const uint8_t *src_ip, const uint8_t *dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               uint8_t proto, uint32_t vrfid);
```

## 11. Template Architecture

All flow cache variants share implementation via C macro templates.
Protocol-specific code (key struct, entry struct, comparison function)
is defined in each variant's header.  Everything else is generated.

### 11.1 Template parameters

| Macro | Example (IPv4) | Purpose |
|---|---|---|
| `FC_PREFIX` | `flow4` | Function name prefix |
| `FC_ENTRY` | `flow4_entry` | Entry struct tag |
| `FC_KEY` | `flow4_key` | Key struct tag |
| `FC_CACHE` | `flow4_cache` | Cache struct tag |
| `FC_FLAG_VALID` | `FLOW4_FLAG_VALID` | Valid flag constant |
| `FC_HT` / `FC_HT_PREFIX` | `flow4_ht` | Hash table name |
| `FC_FREE_HEAD` | `flow4_free_head` | Free list head tag |

### 11.2 Template files

| File | Generates | Used by |
|---|---|---|
| `flow_cache_common.h` | Cache struct, API declarations, inline helpers | `.h` files |
| `flow_cache_body_private.h` | init, insert, lookup_batch, expire, stats | `.c` files |
| `flow_cache_test_body.h` | Test + benchmark functions | test `.c` |

### 11.3 Adding a new variant

To add a new flow cache variant (e.g., MPLS):

1. Define key struct, entry struct, comparison function
2. `RIX_HASH_HEAD` / `RIX_HASH_GENERATE`
3. Set `FC_*` macros, `#include "flow_cache_common.h"` in header
4. Set `FC_*` macros, `#include "flow_cache_body_private.h"` in source
5. Set `FCT_*` macros, `#include "flow_cache_test_body.h"` in test

No other code changes needed — all logic is in the templates.

## 12. File Structure

```
samples/
  DESIGN.md                this document

  flow_cache.h             common definitions (TSC, stats, pipeline params)
  flow_cache_common.h      template: cache struct + API declarations
  flow_cache_body_private.h        template: implementation (init/insert/lookup/expire)
  flow_cache_test_body.h   template: test + benchmark functions

  flow4_cache.h            IPv4: key, entry, cmp, hash generate, template expand
  flow4_cache.c            IPv4: template expand (33 lines)
  flow6_cache.h            IPv6: key, entry, cmp, hash generate, template expand
  flow6_cache.c            IPv6: template expand (33 lines)
  flow_unified_cache.h     Unified: key (family+union), entry, cmp, template expand
  flow_unified_cache.c     Unified: template expand (33 lines)

  flow_cache_test.c        correctness tests + benchmarks (all 3 variants)
  Makefile
```

## 13. Build Dependencies

```
rix/rix_defs.h       index macros, utilities
rix/rix_queue.h      SLIST (free list)
rix/rix_hash.h       fingerprint hash table (SIMD dispatch)
```

No TAILQ dependency (LRU replaced by timestamp-only approach).
Requires `-mavx2` or `-mavx512f` for SIMD-accelerated hash operations.

## 14. Competitive Analysis

| System | Key storage | Remove | Bucket | Index-based |
|---|---|---|---|---|
| **librix flow cache** | fingerprint + node | O(1) via cur_hash | 16-way, 2CL | Yes |
| DPDK rte_hash | in bucket | O(1) via position | 8-way | No (pointer) |
| OVS EMC | in node | O(1) | 1-way | No (pointer) |
| VPP bihash | in bucket | O(n) rehash | 4/8-way | No (pointer) |
| nf_conntrack | in node | O(1) via hlist | chained | No (pointer) |

librix advantages:
- **Index-based**: relocatable across processes, shared memory ready
- **16-way bucket**: lower collision rate at high fill
- **O(1) remove**: cur_hash field eliminates rehash on eviction
- **N-ahead pipeline**: 4-stage software pipeline, SIMD fingerprint scan
- **Template architecture**: single implementation serves all key types

## Appendix A. Using librix

librix is a header-only C11 library providing index-based data structures
for shared memory and mmapped regions.  This appendix explains the core
concepts and API patterns.  The flow cache (§1-14) is a real-world
application built on top of these primitives.

### A.1 Core concept: indices instead of pointers

Traditional BSD `queue.h` / `tree.h` store raw pointers in list/tree
nodes.  This breaks when:
- Structures are placed in shared memory (different base address per process)
- Memory is remapped (`mremap`, file-backed mmap)
- Structures cross 32/64-bit process boundaries

librix replaces all pointers with **unsigned indices** (1-origin):

```
Index 0 = RIX_NIL (sentinel, analogous to NULL)
Index 1 = base[0]
Index 2 = base[1]
  ...
Index n = base[n-1]
```

Each API call takes a `base` pointer (the element array), converting
indices to pointers at call time.  The stored indices are position-
independent — valid regardless of where `base` is mapped.

```c
#include <rix/rix_defs.h>

struct item pool[1024];

/* pointer → index */
unsigned idx = RIX_IDX_FROM_PTR(pool, &pool[42]);  /* → 43 */

/* index → pointer */
struct item *p = RIX_PTR_FROM_IDX(pool, 43);       /* → &pool[42] */

/* NIL handling */
unsigned nil = RIX_IDX_FROM_PTR(pool, NULL);        /* → 0 (RIX_NIL) */
struct item *np = RIX_PTR_FROM_IDX(pool, RIX_NIL);  /* → NULL */
```

Zero-initialization produces a valid empty state for all data structures
(all index fields are 0 = RIX_NIL).

### A.2 Queues (`rix/rix_queue.h`)

Five queue variants, mirroring BSD `sys/queue.h`:

| Macro prefix | Type | Operations |
|---|---|---|
| `RIX_SLIST` | Singly-linked list | INSERT_HEAD, REMOVE_HEAD, FOREACH |
| `RIX_LIST` | Doubly-linked list | INSERT_HEAD/AFTER/BEFORE, REMOVE |
| `RIX_STAILQ` | Singly-linked tail queue | INSERT_HEAD/TAIL, REMOVE_HEAD |
| `RIX_TAILQ` | Doubly-linked tail queue | INSERT_HEAD/TAIL/AFTER/BEFORE, REMOVE |
| `RIX_CIRCLEQ` | Circular queue | INSERT_HEAD/TAIL/AFTER/BEFORE, REMOVE |

All macros take `base` as an extra argument for index-pointer conversion.

#### Example: SLIST (singly-linked list)

```c
#include <rix/rix_queue.h>

struct node {
    int value;
    RIX_SLIST_ENTRY(struct node) link;
};

RIX_SLIST_HEAD(node_head, node);

/* pool and head */
struct node pool[100];
struct node_head head;

/* init */
RIX_SLIST_INIT(&head);

/* insert — note: base comes before elm */
RIX_SLIST_INSERT_HEAD(&head, pool, &pool[0], link);
RIX_SLIST_INSERT_HEAD(&head, pool, &pool[1], link);

/* iterate */
struct node *n;
RIX_SLIST_FOREACH(n, &head, pool, link) {
    printf("value = %d\n", n->value);
}

/* remove head */
RIX_SLIST_REMOVE_HEAD(&head, pool, link);
```

#### Example: TAILQ (doubly-linked tail queue)

```c
#include <rix/rix_queue.h>

struct task {
    int priority;
    RIX_TAILQ_ENTRY(struct task) tlink;
};

RIX_TAILQ_HEAD(task_head, task);

struct task pool[256];
struct task_head head;

RIX_TAILQ_INIT(&head);

/* insert at tail */
RIX_TAILQ_INSERT_TAIL(&head, pool, &pool[0], tlink);
RIX_TAILQ_INSERT_TAIL(&head, pool, &pool[1], tlink);

/* remove specific element */
RIX_TAILQ_REMOVE(&head, pool, &pool[0], task, tlink);

/* iterate (forward and reverse) */
struct task *t;
RIX_TAILQ_FOREACH(t, &head, pool, tlink) {
    printf("priority = %d\n", t->priority);
}
```

**Important**: The `type` argument in `RIX_SLIST_HEAD`, `RIX_TAILQ_REMOVE`,
etc. must NOT include `struct` — e.g., use `node`, not `struct node`.
The macros prepend `struct` internally.

### A.3 Red-black tree (`rix/rix_tree.h`)

Balanced binary search tree with O(log n) insert/find/remove.

```c
#include <rix/rix_tree.h>

struct record {
    uint32_t key;
    uint32_t data;
    RIX_RB_ENTRY(struct record) rb_entry;
};

/* comparison function: return <0, 0, >0 */
static int
record_cmp(const struct record *a, const struct record *b)
{
    return (a->key < b->key) ? -1 : (a->key > b->key);
}

/* declare head and generate functions */
RIX_RB_HEAD(record_tree);
RIX_RB_GENERATE(record_tree, record, rb_entry, record_cmp)

struct record pool[1000];
struct record_tree tree;

RIX_RB_INIT(&tree);

/* insert */
pool[0].key = 42;
RIX_RB_INSERT(record_tree, &tree, pool, &pool[0]);

/* find */
struct record query = { .key = 42 };
struct record *found = RIX_RB_FIND(record_tree, &tree, pool, &query);

/* ordered iteration */
struct record *r;
RIX_RB_FOREACH(r, record_tree, &tree, pool) {
    printf("key=%u data=%u\n", r->key, r->data);
}

/* remove */
RIX_RB_REMOVE(record_tree, &tree, pool, &pool[0]);
```

### A.4 Hash tables (`rix/rix_hash.h`)

Cuckoo hash table with 16-way buckets, SIMD-accelerated fingerprint
matching, and O(1) remove.

Three variants share the same bucket structure:

| Header | Key storage | Key type | Bucket size |
|---|---|---|---|
| `rix_hash.h` | Fingerprint in bucket, key in node | Arbitrary struct | 128B (2CL) |
| `rix_hash32.h` | `uint32_t` key in bucket | `uint32_t` | 128B (2CL) |
| `rix_hash64.h` | `uint64_t` key in bucket | `uint64_t` | 192B (3CL) |

#### A.4.1 Setup: fingerprint variant (`rix_hash.h`)

```c
#include <rix/rix_hash.h>

struct my_key {
    uint32_t src;
    uint32_t dst;
    uint16_t port;
    uint16_t pad;
};

struct my_node {
    struct my_key key;
    uint32_t      cur_hash;   /* hash_field: required for O(1) remove */
    int           data;
};

/* Key comparison: return non-zero if equal */
static int
my_cmp(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(struct my_key)) == 0;
}

/* Declare head struct + generate all functions */
RIX_HASH_HEAD(my_ht);
RIX_HASH_GENERATE(my_ht, my_node, key, cur_hash, my_cmp)
```

`RIX_HASH_GENERATE` produces the following functions:

| Function | Description |
|---|---|
| `my_ht_init(head, nb_bk)` | Initialize with `nb_bk` buckets (power of 2) |
| `my_ht_find(head, bk, base, key)` | Single-shot lookup |
| `my_ht_insert(head, bk, base, elm)` | Insert node (returns NULL on success) |
| `my_ht_remove(head, bk, base, elm)` | O(1) remove via `cur_hash` field |
| `my_ht_walk(head, bk, base, cb, arg)` | Iterate all entries |

Staged find functions (for N-ahead pipeline):

| Function | Stage | Description |
|---|---|---|
| `my_ht_hash_key(ctx, head, bk, key)` | 0 | Hash key, prefetch bucket |
| `my_ht_scan_bk(ctx, head, bk)` | 1 | SIMD fingerprint scan |
| `my_ht_prefetch_node(ctx, base)` | 2 | Prefetch candidate node |
| `my_ht_cmp_key(ctx, base)` | 3 | Full key comparison |

#### A.4.2 Basic usage

```c
/* IMPORTANT: call once per TU at startup */
rix_hash_arch_init();

/* allocate */
unsigned nb_bk = 1024;   /* must be power of 2 */
struct rix_hash_bucket_s *buckets = calloc(nb_bk, sizeof(*buckets));
struct my_node *pool = calloc(10000, sizeof(*pool));

/* init */
struct my_ht ht;
my_ht_init(&ht, nb_bk);

/* insert */
pool[0].key = (struct my_key){ .src = 1, .dst = 2, .port = 80 };
struct my_node *dup = my_ht_insert(&ht, buckets, pool, &pool[0]);
/* dup == NULL: success
 * dup == &pool[0]: table full (kickout exhausted)
 * dup == other: duplicate key found (returns existing entry) */

/* find */
struct my_key query = { .src = 1, .dst = 2, .port = 80 };
struct my_node *found = my_ht_find(&ht, buckets, pool, &query);

/* remove — O(1) via cur_hash, no rehash needed */
my_ht_remove(&ht, buckets, pool, &pool[0]);

/* walk all entries */
int print_cb(struct my_node *node, void *arg) {
    printf("src=%u dst=%u\n", node->key.src, node->key.dst);
    return 0;  /* 0 = continue, non-zero = stop */
}
my_ht_walk(&ht, buckets, pool, print_cb, NULL);
```

#### A.4.3 N-ahead pipelined lookup

For high-throughput bulk lookups (e.g., packet processing), use the
4-stage pipeline to hide DRAM latency:

```c
#define BATCH  8
#define KPD    8
#define DIST   (BATCH * KPD)   /* 64 keys ahead */

void
batch_find(struct my_ht *ht,
           struct rix_hash_bucket_s *bk,
           struct my_node *base,
           const struct my_key *keys,
           unsigned nb_keys,
           struct my_node **results)
{
    struct rix_hash_find_ctx_s ctx[nb_keys];
    unsigned total = nb_keys + 3 * DIST;

    for (unsigned i = 0; i < total; i += BATCH) {
        /* Stage 0: hash + prefetch bucket */
        if (i < nb_keys) {
            unsigned n = (i + BATCH <= nb_keys) ? BATCH : nb_keys - i;
            for (unsigned j = 0; j < n; j++)
                my_ht_hash_key(&ctx[i+j], ht, bk, &keys[i+j]);
        }
        /* Stage 1: SIMD fingerprint scan */
        if (i >= DIST && i - DIST < nb_keys) {
            unsigned b = i - DIST;
            unsigned n = (b + BATCH <= nb_keys) ? BATCH : nb_keys - b;
            for (unsigned j = 0; j < n; j++)
                my_ht_scan_bk(&ctx[b+j], ht, bk);
        }
        /* Stage 2: prefetch candidate node */
        if (i >= 2*DIST && i - 2*DIST < nb_keys) {
            unsigned b = i - 2*DIST;
            unsigned n = (b + BATCH <= nb_keys) ? BATCH : nb_keys - b;
            for (unsigned j = 0; j < n; j++)
                my_ht_prefetch_node(&ctx[b+j], base);
        }
        /* Stage 3: full key comparison */
        if (i >= 3*DIST && i - 3*DIST < nb_keys) {
            unsigned b = i - 3*DIST;
            unsigned n = (b + BATCH <= nb_keys) ? BATCH : nb_keys - b;
            for (unsigned j = 0; j < n; j++)
                results[b+j] = my_ht_cmp_key(&ctx[b+j], base);
        }
    }
}
```

This achieves ~55-73 cy/key (DRAM-cold) vs ~388 cy/key for single
`my_ht_find()` — a 5-7x improvement.

#### A.4.4 `rix_hash_arch_init()` — SIMD dispatch

`rix_hash_arch_init()` detects CPU features (AVX2, AVX-512) and sets
function pointers for the fastest SIMD fingerprint scan available.

**Must be called once per translation unit** that uses hash tables,
before any hash operation.  The dispatch variable `rix_hash_arch` is
`static` per TU, so each `.c` file needs its own call.

```c
/* At the top of main() or module init: */
rix_hash_arch_init();
```

Build with `-mavx2` or `-mavx512f` to enable SIMD acceleration.
Without these flags, the generic scalar fallback is used.

#### A.4.5 Fixed-key variants

For `uint32_t` or `uint64_t` keys, use the specialized variants
that store keys directly in the bucket (no fingerprint, no node-side
key comparison):

```c
#include <rix/rix_hash32.h>

struct my_entry {
    uint32_t data;
};

RIX_HASH32_HEAD(idx_ht);
RIX_HASH32_GENERATE(idx_ht, my_entry)

/* Usage is similar but key is a uint32_t value, not a struct pointer */
idx_ht_insert(&ht, buckets, pool, &pool[0], key_value);
struct my_entry *found = idx_ht_find(&ht, buckets, pool, key_value);
```

### A.5 Shared memory deployment

Because all data structures store indices (not pointers), they can
be placed directly in shared memory:

```c
/* Process A: create and populate */
void *shm = mmap(NULL, size, PROT_READ|PROT_WRITE,
                 MAP_SHARED, shm_fd, 0);
struct my_node *base = (struct my_node *)shm;
struct my_ht *ht = (struct my_ht *)(shm + node_area_size);
/* ... init and insert ... */

/* Process B: attach and query (different virtual address) */
void *shm2 = mmap(NULL, size, PROT_READ|PROT_WRITE,
                  MAP_SHARED, shm_fd, 0);
struct my_node *base2 = (struct my_node *)shm2;
struct my_ht *ht2 = (struct my_ht *)(shm2 + node_area_size);
/* find works with base2 — indices are position-independent */
struct my_node *found = my_ht_find(ht2, buckets2, base2, &query);
```

No pointer fixup needed.  The same index values are valid in both
processes because they are offsets from `base`, not absolute addresses.

### A.6 Summary: BSD queue.h vs librix

| Aspect | BSD `queue.h` | librix |
|---|---|---|
| Node linkage | Raw pointers | Unsigned indices (1-origin) |
| Sentinel | `NULL` | `RIX_NIL` (0) |
| API extra arg | — | `base` (element array pointer) |
| Shared memory | Requires pointer fixup | Works directly |
| mmap/remap | Breaks all pointers | Indices remain valid |
| 32/64-bit interop | Pointer size mismatch | Indices are `unsigned` (32-bit) |
| Zero-init | Invalid state | Valid empty state |
| Data structures | SLIST, LIST, STAILQ, TAILQ, CIRCLEQ | Same + RB tree + 3 hash variants |
| Performance | Pointer dereference | Index arithmetic + dereference |
