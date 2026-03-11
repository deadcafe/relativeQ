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

### Goals

| Operation | Condition | Target (cy/op) |
|---|---|---|
| find (pipelined) | DRAM cold buckets | ~100 |
| insert (on miss) | bucket L2 warm (post-find) | ~60 |
| touch (on hit) | node warm (post-prefetch) | ~10 |
| expire scan step | sequential (HW prefetcher) | ~10 |
| expire evict | bucket cold | ~200-300 |
| expire amortized/pkt | 256 steps, few evicts | ~15-25 |

- Miss path: insert immediately with empty action, slow-path
  ACL/QoS fills in action later
- Eviction: time-based aging (N seconds idle), sweep rate-limited
  to vector size

## 2. Separation of IPv4 and IPv6

IPv4 and IPv6 flow caches are separate instances.

| | IPv4 | IPv6 |
|---|---|---|
| Key size | 20 bytes | 40 bytes |
| Node size | 128 bytes (2 CL) | 192 bytes (3 CL) |
| Hash variant | `rix_hash.h` (fingerprint) | `rix_hash.h` (fingerprint) |
| GENERATE | `flow4_ht` | `flow6_ht` |

Rationale:
- Fixed key size per variant enables compiler-optimized cmp_fn
- No v4/v6 branch in hot path
- Node pool allocation is simpler (uniform element size)
- Prefetch strategy can differ (v6 nodes span 3 CL)

## 3. Key Structures

### 3.1 IPv4 flow key

```c
struct flow4_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
};  /* 20 bytes, 4-byte aligned */
```

### 3.2 IPv6 flow key

```c
struct flow6_key {
    uint8_t  src_ip[16];
    uint8_t  dst_ip[16];
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
};  /* 40 bytes, 4-byte aligned */
```

## 4. Node Layout

### 4.1 Design principles

- CL0 (bytes 0-63): fields accessed during pipelined lookup (hot)
- CL1 (bytes 64-127): fields accessed only on hit (warm) or eviction (cold)
- Node aligned to 64 bytes
- `lru_link` / `free_link` share the same field via union
  (node is either in-use on LRU list, or on free list, never both)

### 4.2 IPv4 flow entry (128 bytes = 2 CL)

```c
struct flow4_entry {
    /* --- CL0: lookup hot path --- */
    struct flow4_key     key;           /* 20B: match key */
    uint32_t             cur_hash;      /*  4B: hash_field (O(1) remove) */
    uint32_t             action;        /*  4B: cached ACL result */
    uint32_t             qos_class;     /*  4B: cached QoS class */
    uint32_t             flags;         /*  4B: VALID, etc. */
    uint32_t             reserved0[7];  /* 28B: pad CL0 to 64B */

    /* --- CL1: counters & management --- */
    uint64_t             last_ts;       /*  8B: last access TSC */
    uint64_t             packets;       /*  8B: packet counter */
    uint64_t             bytes;         /*  8B: byte counter */
    union {
        RIX_SLIST_ENTRY(struct flow4_entry) free_link;  /* free list */
    } mgmt;
    uint8_t              reserved1[__pad_to_64];        /* pad CL1 to 64B */
} __attribute__((aligned(64)));
/* total: 128 bytes = 2 cache lines */
```

Note: exact padding sizes to be determined at implementation time
based on `RIX_SLIST_ENTRY` size (typically 4 bytes = 1 unsigned index).

### 4.3 IPv6 flow entry (192 bytes = 3 CL)

Same structure but `flow6_key` (40B) makes CL0 larger.
May spill into CL1; layout to be finalized at implementation.

## 5. Hash Table Configuration

- Hash variant: `rix_hash.h` fingerprint (arbitrary key size, cmp_fn)
- `RIX_HASH_GENERATE(flow4_ht, struct flow4_entry, key, cur_hash, flow4_cmp)`
- GENERATE with `hash_field` (cur_hash) for O(1) remove
  - remove is frequent (eviction), O(1) is important
  - kickout uses XOR trick (no re-hash)
- Bucket: 128 bytes (2 CL), 16 slots/bucket

### 5.1 Table sizing

Expected table size: 2^20 - 2^28 bits (nb_bk).

| nb_bk bits | buckets | capacity (×16) | bucket memory | node memory (128B) |
|---|---|---|---|---|
| 20 | 1M | 16M | 128 MB | 2 GB |
| 24 | 16M | 256M | 2 GB | 32 GB |
| 28 | 256M | 4G | 32 GB | 512 GB |

Per-thread allocation.  Actual deployment size depends on
expected concurrent flow count and available memory.

## 6. Pipeline Design

### 6.1 Lookup pipeline

`rix_hash.h` staged find has 4 stages:

```
Stage 0: hash_key       compute hash, prefetch bucket[0] CL0
Stage 1: scan_bk        SIMD fingerprint scan in bucket[0]
Stage 2: prefetch_node  prefetch flow_entry CL0 for hits
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

Expected throughput: ~100 cycles/op (fingerprint, x8, N-ahead,
per MEMORY.md benchmarks).

### 6.3 Timestamp

- TSC (`rdtsc`) read **once** before the lookup loop
- Same `now` value used for all packets in the vector
- No per-packet TSC read

## 7. Hit / Miss Processing

### 7.1 Miss-on-find: immediate insert with empty action

On cache miss, insert a new entry **immediately** (not deferred to
slow path).  The entry is created with action = NONE (empty).

Rationale:
- The lookup just ran; bucket cache lines are still in L2 (~5 cy
  access vs ~200 cy DRAM).  Insert cost drops from ~200 cy to ~60 cy.
- The flow is now in the cache.  Subsequent packets for the same
  flow will hit immediately (even before ACL fills in the action).
- The slow-path ACL/QoS lookup runs later and **updates** the
  existing entry's action/qos_class fields in-place.

### 7.2 Processing flow

```
process_vector(pkts[256]):

  now = rdtsc()                             // 1 TSC read per vector

  Phase 1: pipelined lookup                 // ~100 cy/pkt
    flow_cache_lookup_batch(keys, results)

  Phase 2: process results                  // sequential
    for each pkt:
      if hit:
        flow_cache_touch(entry, now, len)   // ~10 cy (CL1 warm)
        if entry->action != NONE:
          apply cached action/qos           // fast path
        else:
          enqueue to slow-path              // action not yet filled
      if miss:
        flow_cache_insert(fc, key, now)     // ~60 cy (bucket L2 warm)
        enqueue to slow-path                // ACL/QoS lookup needed

  Phase 3: aging expire                     // ~15-25 cy/pkt amortized
    flow_cache_expire(fc, now, nb_pkts)

  (slow path: ACL/QoS lookup, then update entry->action/qos_class)
```

### 7.3 Slow-path update

After ACL/QoS lookup completes, the existing cache entry is
updated in-place:

```c
static inline void
flow4_cache_update_action(struct flow4_entry *entry,
                          uint32_t action,
                          uint32_t qos_class)
{
    entry->action    = action;
    entry->qos_class = qos_class;
}
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

### 8.2 Eviction: unified aging sweep (no capacity-based eviction)

No separate capacity-based eviction on insert.  Instead, aging sweep
runs after every lookup vector with a rate limit equal to the
preceding vector size.

#### Design

```c
void flow_cache_expire(fc, now, max_expire) {
    unsigned expired = 0;
    while (expired < max_expire) {
        entry = &pool[fc->age_cursor % fc->pool_cap];
        fc->age_cursor++;
        if (entry is VALID && now - entry->last_ts > fc->timeout_tsc) {
            hash_remove(entry);
            free_list_push(entry);
            expired++;
        }
    }
}
```

Called after each lookup vector:
```c
flow_cache_expire(fc, now, nb_pkts);   /* nb_pkts ~ 256 */
```

#### Rationale

- **Rate limiting**: expire at most nb_pkts entries per vector.
  Expire cost is bounded to O(nb_pkts) per vector — symmetric
  with lookup cost.
- **No O(n) pool scan**: cursor advances incrementally, no full scan.
- **75% overshoot is negligible**: worst case, nb_pkts (~256) entries
  exceed 75% temporarily.  At table sizes of 2^20-2^28 buckets
  (16M-4G capacity), 256 extra entries is < 0.002%.
- **No capacity-based eviction**: the aging sweep alone keeps the
  table from growing unbounded.  If all flows are active (no
  expired entries), the table fills beyond 75% — this is correct
  behavior (all flows are live, cache should hold them).
- **Eliminates O(n) find_oldest**: at 2^20-2^28 table sizes,
  scanning millions of entries on insert is unacceptable.

#### Sweep coverage

- Cursor advances by nb_pkts per vector (even if fewer entries expired)
- Full pool traversal period = pool_cap / nb_pkts vectors
- At 256 pkts/vector, 10Gbps, ~20-50 us/vector:
  - 2^20 (1M) pool: full sweep in 4096 vectors ~ 80-200 ms
  - 2^24 (16M) pool: full sweep in 65536 vectors ~ 1.3-3.3 s
  - 2^28 (256M) pool: full sweep in ~1M vectors ~ 20-50 s
- For timeout N seconds, sweep period must be < N.
  At large table sizes (2^28), minimum timeout should be > 60 s.
  At typical sizes (2^20-2^24), any timeout >= 1 s is fine.

### 8.3 Insert when free list is empty

If free list is exhausted (all pool entries in use), insert must
evict an entry.  Since aging sweep runs every vector, this should
be rare.  When it does happen:

```c
/* scan forward from age_cursor until an expired entry is found */
for (i = 0; i < pool_cap; i++) {
    entry = &pool[(fc->age_cursor + i) % pool_cap];
    if (entry is VALID && now - entry->last_ts > timeout_tsc) {
        evict(entry);
        return entry;  /* reuse for new flow */
    }
}
/* truly full: all flows are active, insert fails */
return NULL;
```

This is slow path and expected to be rare.

### 8.4 Timeout parameter

```c
struct flow_cache {
    ...
    uint64_t timeout_tsc;   /* N seconds converted to TSC ticks */
};

void flow_cache_init(..., double timeout_sec) {
    fc->timeout_tsc = (uint64_t)(timeout_sec * tsc_hz);
}
```

Timeout value is a runtime parameter, not compile-time.
Appropriate value depends on workload:
- Short-lived flows (web): 1-5 seconds may suffice
- Long-lived flows (streaming): 30-60 seconds
- Default for testing: TBD (to be tuned with benchmarks)

## 9. Free List Management

- Pre-allocated pool of flow_entry[pool_cap]
- At init: all entries added to SLIST free list
- insert: pop from free list, if empty → evict oldest
- remove/evict: push back to free list
- `free_link` field reuses space in CL1 (entry is not in hash table
  when on free list, so no conflict)

## 10. API

```c
/* Initialization */
void flow4_cache_init(struct flow4_cache *fc,
                      unsigned nb_bk,        /* buckets, power of 2 */
                      unsigned pool_cap,      /* max entries */
                      double   timeout_sec);  /* idle timeout */

/* Batch lookup — pipelined hot path */
void flow4_cache_lookup_batch(struct flow4_cache *fc,
                              const struct flow4_key *keys,
                              unsigned nb_pkts,
                              struct flow4_entry **results);

/* Insert — called on miss, bucket L2 warm from lookup.
 * Entry is created with action=FLOW_ACTION_NONE.
 * Returns inserted entry (caller enqueues to slow-path ACL). */
struct flow4_entry *flow4_cache_insert(struct flow4_cache *fc,
                                       const struct flow4_key *key,
                                       uint64_t now);

/* Update action — called after slow-path ACL/QoS lookup */
static inline void
flow4_cache_update_action(struct flow4_entry *entry,
                          uint32_t action,
                          uint32_t qos_class)
{
    entry->action    = action;
    entry->qos_class = qos_class;
}

/* Touch — update timestamp + counters (inline, called per hit) */
static inline void
flow4_cache_touch(struct flow4_entry *entry, uint64_t now,
                  uint32_t pkt_len)
{
    entry->last_ts = now;
    entry->packets++;
    entry->bytes += pkt_len;
}

/* Aging expire — call once per vector, bounded to max_expire */
void flow4_cache_expire(struct flow4_cache *fc,
                        uint64_t now,
                        unsigned max_expire);

/* Statistics */
void flow4_cache_stats(const struct flow4_cache *fc,
                       struct flow_cache_stats *out);
```

IPv6 API is identical with `flow6_` prefix.

## 11. File Structure

```
samples/
  DESIGN.md              this document
  flow_cache.h           common definitions (stats, rdtsc helper)
  flow4_cache.h          IPv4 flow cache (struct + RIX_HASH_GENERATE + API)
  flow4_cache.c          IPv4 implementation
  flow6_cache.h          IPv6 flow cache
  flow6_cache.c          IPv6 implementation
  flow_cache_test.c      correctness tests (IPv4 + IPv6)
  flow_cache_bench.c     pipeline performance benchmark
  Makefile
```

## 12. Build Dependencies

```
rel/rix_defs.h       index macros, utilities
rel/rix_queue.h      SLIST (free list)
rel/rix_hash.h       fingerprint hash table
```

No TAILQ dependency (LRU replaced by timestamp-only approach).

## 13. Future Considerations

- Staged find xN convenience wrappers specific to flow cache
- Adaptive timeout based on cache pressure
- Per-flow TTL (different timeout per flow type)
- Integration with flow6_cache for dual-stack
- Shared-memory deployment (index-based design is ready)

## 14. Open Questions

- [ ] Optimal BATCH and KPD values (needs benchmarking)
- [ ] flow_entry reserved fields: what additional cached data?
- [ ] IPv6 node layout: 2 CL or 3 CL?
- [ ] Default timeout_sec recommendation
- [ ] Table size recommendation per deployment scenario
