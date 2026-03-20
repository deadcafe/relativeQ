# Flow Cache

Production-quality flow cache implementations built on librix (`rix_hash.h`).
Designed for high-performance packet processing with pipeline-style batch
lookup to hide DRAM latency.

## Quick Start

```sh
# Build v1 library
make -C samples/fcache static

# Build v2 library
make -C samples/fcache static

# Run v2 tests
make -C tests/fcache fc_test && tests/fcache/fc_test

# Run v2 variant benchmark (flow4 vs flow6 vs flowu)
make -C tests/fcache fc_vbench && taskset -c 0 tests/fcache/fc_vbench

# Run v1-vs-v2 comparison benchmark
make -C tests/fcache fc_bench && taskset -c 0 tests/fcache/fc_bench

# Run v1 tests + benchmarks
make -C samples/test all && samples/test/flow_cache_test -n 1000000
```

## Two Versions

| | fcache (v1) | fcache (v2) |
|---|---|---|
| Entry size | 128B (2 CL) | 64B (1 CL) |
| User payload | 64B in CL1 | None (caller uses entry_idx) |
| Variants | flow4, flow6, flowu | flow4, flow6, flowu |
| SIMD dispatch | Runtime (gen/sse/avx2/avx512) | AVX2 direct bind |
| Expire | Global walk + threshold | Local relief + bucket-budget maintenance |

v2 is the active development target.  v1 remains for comparison.

---


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

#### Pool-size breakdown: lookup performance (IPv4, batch=256, DRAM-cold)

| Pool | Memory | batch lookup | single find | Pipeline effect |
|---|---|---|---|---|
| 1K | 144KB (L2) | 40 cy/key | 111 cy/key | 2.8x |
| 100K | 18MB (LLC boundary) | 47-60 cy/key | 322 cy/key | 6.0x |
| 1M | 144MB (DRAM) | 100-132 cy/key | 953 cy/key | 7.5x |
| 4M | 576MB (DRAM) | 115-143 cy/key | 1066 cy/key | 8.1x |

Pipeline benefit grows with pool size — 1K→4M is a 4000x size increase,
yet batch lookup only degrades 40→135 cy/key (3.4x).

#### Pool-size breakdown: packet processing loop (IPv4, standard sizing)

| Pool | lookup+insert | expire/pkt | total |
|---|---|---|---|
| 1K | 119 cy/pkt | 13 cy/pkt | 132 cy/pkt |
| 1M | 150 cy/pkt | 37 cy/pkt | 187 cy/pkt |
| 4M | 170 cy/pkt | 36 cy/pkt | 206 cy/pkt |

### Xeon throughput budget

At 2 GHz Xeon, 200 cy/pkt = 10 Mpps per core.
2 Mpps uses ~20% of one core — adequate for most deployments.

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
| Key comparison | `memcmp(a, b, 20)` | `memcmp(a, b, 44)` |
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

- CL0 (bytes 0-63): fields accessed during pipelined lookup and expire scan (hot)
- CL1 (bytes 64-127): user payload — managed entirely by caller via callbacks
- Node aligned to 64 bytes (`__attribute__((aligned(64)))`)
- `last_ts` and `free_link` both in CL0 — expire scan and free-list ops stay in one CL

### 4.2 Entry layout (all variants)

```
CL0 (64 bytes) — lookup + expire hot path:
  key           20B (v4) / 44B (v6, unified)
  cur_hash       4B   hash_field for O(1) remove
  last_ts        8B   last access TSC (0 = invalid / free)
  free_link      4B   SLIST entry (free list index)
  reserved      28B (v4) / 4B (v6, unified)   pad to 64B

CL1 (64 bytes) — user payload:
  userdata      64B   caller-defined
                      union { uint8_t u8[64]; uint64_t u64[8]; }
                      init_cb initializes on insert; fini_cb cleans up on eviction
```

Total: 128 bytes = 2 cache lines, for all three variants.

IPv6 key (44B) fits in CL0 with 4B reserved.
IPv4 key (20B) fits in CL0 with 28B reserved — more padding but
same entry size, so pool memory is identical.

#### CL0/CL1 placement rationale

- **`last_ts` in CL0**: expire scan reads only `last_ts` to decide expiry.
  Having it in CL0 means non-expired entries never touch CL1, and SW
  prefetch can pipeline CL0 reads efficiently.
- **`free_link` in CL0**: an entry is either in the hash table or on the
  free list, never both.  `free_link` is used only during free-list push/pop,
  which already accesses CL0 for `last_ts = 0` clearing — one CL for both.
- **`userdata` in CL1**: the library never reads or writes CL1.  Callers
  access it via `init_cb` (after insert) and `fini_cb` (before eviction),
  or directly after a cache hit.  This keeps CL1 cold on the lookup hot path.

### 4.4 `fcache` — single-cache-line redesign

`samples/fcache/` is a redesign that fits each entry in a single 64-byte
cache line (half the 128B of `fcache`).  No user payload is stored in the
entry — the caller uses the returned `entry_idx` to reference its own data
externally.  Three variants are provided: flow4 (IPv4), flow6 (IPv6),
and flowu (unified IPv4/IPv6).

#### 4.4.1 Entry design (all 64B / 1 CL)

```
flow4 entry (64B):
  fc_flow4_key    20B   src_ip/dst_ip/src_port/dst_port/proto/vrfid
  cur_hash          4B   hash_field for O(1) remove
  last_ts           8B   last access TSC (0 = free)
  free_link         4B   SLIST entry (free list index)
  slot              2B   slot in current bucket
  reserved1         2B
  reserved0        24B   pad to 64B

flow6 entry (64B):
  fc_flow6_key    44B   src_ip[16]/dst_ip[16]/src_port/dst_port/proto/vrfid
  cur_hash          4B
  last_ts           8B
  free_link         4B
  slot              2B
  reserved1         2B
  (zero reserved bytes — key fills the space)

flowu entry (64B):
  fc_flowu_key    44B   family/proto/ports/vrfid/addr union
  cur_hash          4B
  last_ts           8B
  free_link         4B
  slot              2B
  reserved1         2B
  (zero reserved bytes — same layout as flow6)
```

All entry structs carry `__attribute__((packed))` on the key and
`__attribute__((aligned(64)))` on the entry, with a static assertion
checking `sizeof == 64`.

#### 4.4.2 flowu unified key

The unified key uses a `family` field at byte 0 and a union for addresses:

```c
struct fc_flowu_key {
    uint8_t  family;        /* FC_FLOW_FAMILY_IPV4=4 / IPV6=6 */
    uint8_t  proto;
    uint16_t src_port, dst_port;
    uint16_t pad;
    uint32_t vrfid;
    union {
        struct { uint32_t src; uint32_t dst; uint8_t _pad[24]; } v4;
        struct { uint8_t  src[16]; uint8_t  dst[16]; }          v6;
    } addr;
};  /* 44B total */
```

IPv4 entries zero-pad the unused 24 bytes of the union so `memcmp`
produces correct equality semantics.  Key construction helpers
`fc_flowu_key_v4()` / `fc_flowu_key_v6()` `memset` the struct to
zero before filling fields.

#### 4.4.3 API pattern (uniform across variants)

Each variant exposes the same function set (substitute `flow4` / `flow6` /
`flowu` for `PREFIX`):

| Function | Purpose |
|---|---|
| `fc_PREFIX_cache_init()` | Initialize cache with buckets, pool, config |
| `fc_PREFIX_cache_flush()` | Return all entries to free list |
| `fc_PREFIX_cache_lookup_batch()` | Pipelined batch lookup, returns miss count |
| `fc_PREFIX_cache_fill_miss_batch()` | Insert missed keys with pressure relief |
| `fc_PREFIX_cache_maintain()` | Bucket-budgeted idle/background reclaim |
| `fc_PREFIX_cache_remove_idx()` | Remove a single entry by index |
| `fc_PREFIX_cache_nb_entries()` | Current entry count |
| `fc_PREFIX_cache_stats()` | Snapshot counters |

#### 4.4.4 Implementation details

- Same 4-stage batch lookup pipeline as `fcache`
  (hash_key → scan_bk → prefetch_node → cmp_key)
- Staged fingerprint scans bind directly to AVX2 helpers
- `fill_miss_batch()` uses prehash + bucket prefetch insert plan,
  then local pressure relief when the candidate bucket is dense
- Relief density trigger tightens as global fill rises
  (`15/16 → 14/16 → 13/16`)
- Maintenance performs grouped bucket walks with staged entry prefetch
  and expire-all reclaim per visited bucket
- Bucket removal unified on `remove_at()` across relief and maintenance
- No global expire walk — aging bounded to insert-triggered relief and
  explicit bucket-budgeted maintenance

#### 4.4.5 Variant benchmark comparison (32K entries, batch=256, AVX2)

```
                        flow4     flow6     flowu    6/4     u/4
  hit_lookup (cy/key)    91        107       109    +17%    +20%
  miss_fill  (cy/key)   240        271       255    +13%     +7%
  mixed_90_10 (cy/key)  111        120       123     +8%    +11%
```

The 44B key variants (flow6/flowu) show 8-20% overhead over 20B flow4,
primarily from larger `memcmp` in the comparison function.  The flowu
family field adds no measurable overhead beyond flow6.

#### 4.4.6 Bench validation

Current bench policy validation is driven from `tests/fcache/fc_bench`
parameterized modes.  The currently accepted fill-control trace profile is:

```text
thresholds: 70 / 73 / 75 / 77
kicks:      0 / 0 / 1 / 2
```

Replay scripts:

```sh
./samples/test/run_fc_bench_matrix.sh flow4
make -C samples/fcache matrix
make -C tests/fcache matrix
make -C tests/fcache vbench    # variant comparison
```

## 5. Hash Table Configuration

- Hash variant: `rix_hash.h` fingerprint (arbitrary key size, cmp_fn)
- `RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)`
- Optional typed hash hook:
  `RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)`
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
Stage 3: cmp_key        full key comparison, return hit/NULL, then warm CL1 on hit
```

### 6.2 N-ahead software pipeline

Process `nb_pkts` packets in `STEP_KEYS`-wide steps. Each stage runs
`AHEAD_KEYS` keys ahead of the next stage:

```
STEP_KEYS   = 16   (keys processed per step)
AHEAD_STEPS = 8    (how many steps each stage runs ahead)
AHEAD_KEYS  = 128  (= STEP_KEYS * AHEAD_STEPS)
```

```c
for (i = 0; i < nb_pkts + 3 * AHEAD_KEYS; i += STEP_KEYS) {
    if (i                        < nb_pkts) hash_key_n      (i,              STEP_KEYS);
    if (i >= AHEAD_KEYS       && ...)       scan_bk_n       (i-AHEAD_KEYS,   STEP_KEYS);
    if (i >= 2*AHEAD_KEYS     && ...)       prefetch_node_n (i-2*AHEAD_KEYS, STEP_KEYS);
    if (i >= 3*AHEAD_KEYS     && ...)       cmp_key_n       (i-3*AHEAD_KEYS, STEP_KEYS);
}
```

`AHEAD_KEYS` is a software-pipeline distance. It does not mean that the code
issues 128 hardware prefetches at once.

For flow-cache packet loops, a confirmed hit also triggers a CL1 write-prefetch
so the following `cache_touch()` and payload counter update do not stall on
the second cache line.

### 6.3 Timestamp

- TSC (`rdtsc`) read **once** before the lookup loop
- Same `now` value used for all packets in the vector
- No per-packet TSC read
- Calibrated at init time via 50 ms sleep

## 7. Hit / Miss Processing

### 7.1 Miss-on-find: immediate insert, init_cb initializes userdata

On cache miss, insert a new entry **immediately** (not deferred to
slow path).  After a successful insert, `init_cb(entry, cb_arg)` is
called to initialize `entry->userdata`.

The insert fast path also warms the two candidate bucket lines before
the duplicate / empty-slot scan, so the common case does not wait for
that bucket fetch as late.
For miss-heavy batches, `cache_insert_batch()` precomputes hashes once,
warms candidate buckets from that plan, and then enters the hashed
insert path without re-hashing each key.

Rationale:
- The lookup just ran; bucket cache lines are still in L2 (~5 cy
  access vs ~200 cy DRAM).  Insert cost drops significantly.
- The flow is now in the cache.  Subsequent packets for the same
  flow will hit immediately.
- Payload layout (counters, action, QoS class, etc.) is defined
  entirely by the caller — the library imposes no schema on CL1.

For the benchmark packet loop, consecutive hits to the same entry are also
coalesced into one `cache_touch()` plus one payload update. The expire cadence
is then selected from miss-rate tiers (1 / 2 / 4 / 8 batches) and moved one
step per batch toward that target, so a light miss rate does not immediately
collapse the benchmark back to every-batch expire.

### 7.2 Processing flow

```
process_vector(pkts[256]):

  now = rdtsc()                             // 1 TSC read per vector

  Phase 1: pipelined lookup                 // ~55-73 cy/pkt
    cache_lookup_batch(keys, results)

  Phase 2: process results                  // sequential
    misses = 0
    for each pkt:
      if hit:
        cache_touch(entry, now)             // ~10 cy (CL0 write only)
        // update entry->userdata directly (e.g. MY_PAYLOAD(entry)->packets++)
      if miss:
        cache_insert(fc, key, now)          // ~60 cy (bucket L2 warm)
        // init_cb called automatically; enqueue to slow-path
        misses++

  Phase 3: timeout adjust + expire          // ~13-40 cy/pkt amortized
    cache_adjust_timeout(fc, misses)        // miss-rate-driven timeout tuning
    cache_expire(fc, now)                   // adaptive scan (always runs)
```

Phase 3 runs unconditionally every batch.  Scan depth auto-scales
with fill level (§8.2).  At low fill (≤50%), only 64 entries are
scanned — minimal cost.

### 7.3 User payload management

CL1 (`userdata`) is fully caller-owned.  Two callbacks manage its
lifecycle:

```c
/* Called after successful insert — initialize userdata here */
void init_cb(struct PREFIX_entry *entry, void *arg);

/* Called before every eviction/remove/flush — save or release userdata */
void fini_cb(struct PREFIX_entry *entry,
             flow_cache_fini_reason_t reason, void *arg);
```

Passing `NULL` for either callback assigns a built-in no-op —
no conditional branches at call sites.

```c
typedef enum {
    FLOW_CACHE_FINI_TIMEOUT,   /* removed by aging expire */
    FLOW_CACHE_FINI_EVICTED,   /* forced eviction (pool full) */
    FLOW_CACHE_FINI_REMOVED,   /* explicit cache_remove() call */
    FLOW_CACHE_FINI_FLUSHED,   /* cache_flush() bulk clear */
} flow_cache_fini_reason_t;
```

## 8. Eviction Strategy

### 8.1 No TAILQ for LRU

TAILQ-based LRU requires write to prev/next nodes on every hit.
These writes touch random cold cache lines (prev/next neighbors),
destroying pipeline efficiency.

Instead: **timestamp-only approach**.

- Hit: write `last_ts = now` to the hit node's CL0 (already warm from lookup)
- No linked-list manipulation on hit path
- Eviction finds victims by scanning

### 8.2 Adaptive expire (unconditional, fill-level-driven)

`cache_expire()` is intended to be called every batch. Scan depth and
effective timeout auto-scale with workload, and on large high-pressure
pools it also switches internally to `cache_expire_2stage()`.

#### Fill-level-driven scan depth (16 levels)

```c
static inline unsigned
cache_expire_scan(const struct cache *fc)
{
    unsigned nb   = fc->ht_head.rhh_nb;
    unsigned half = fc->max_entries >> 1;
    if (nb <= half) return FLOW_CACHE_EXPIRE_SCAN_MIN;  /* 64 */
    unsigned excess = nb - half;
    unsigned shift  = __builtin_ctz(fc->max_entries) - 4;
    unsigned level  = excess >> shift;
    if (level >= 4) return FLOW_CACHE_EXPIRE_SCAN_MAX;  /* 1024 */
    return FLOW_CACHE_EXPIRE_SCAN_MIN << level;         /* 64..512 */
}
```

| Fill | Level | Scan/batch |
|---|---|---|
| ≤ 50% | 0 | 64 |
| 50–56% | 1 | 128 |
| 56–62% | 2 | 256 |
| 62–69% | 3 | 512 |
| ≥ 69% | 4+ | 1024 |

At low fill (≤50%), only 64 entries are scanned — minimal cost.
As fill rises, scan increases exponentially to reclaim entries faster.

#### Miss-rate-driven timeout adjustment

Call `cache_adjust_timeout(fc, misses)` once per batch.  High miss
rate (many new flows) shortens the effective timeout; quiet periods
let it recover toward the configured base.

```c
/* All shifts, no division */
decay = (eff_timeout * misses) >> FLOW_CACHE_TIMEOUT_DECAY_SHIFT;
eff_timeout -= decay;                             /* accelerate eviction */
eff_timeout += (base - eff) >> FLOW_CACHE_TIMEOUT_RECOVER_SHIFT; /* recover */
```

Floor: `FLOW_CACHE_TIMEOUT_MIN_MS` (default 1000 ms).

#### Expire body (buffered default / extra two-stage pipeline)

```c
void cache_expire(struct cache *fc, uint64_t now)
{
    unsigned max_scan = cache_expire_scan(fc);
    struct entry *pending[pf_dist] = {0};
    for (unsigned i = 0; i < max_scan + pf_dist; i++) {
        if (pending[i % pf_dist])
            hash_remove_and_recycle(pending[i % pf_dist]);
        if (i >= max_scan)
            continue;

        /* SW prefetch: pf_dist ahead in CL0 */
        prefetch(&pool[(cursor + i + pf_dist) & mask]);
        entry = &pool[(cursor + i) & mask];
        if (entry->last_ts == 0) continue;        /* free slot */
        if (now - entry->last_ts <= timeout) continue;  /* alive */
        prefetch(bucket[idx[] line for entry->cur_hash]);
        prefetch(bucket[hash[] line for entry->cur_hash]);
        pending[i % pf_dist] = entry;
    }
    cursor += max_scan;
}
```

The two-stage variant (`cache_expire_2stage`) adds an extra mid-distance
bucket prefetch (`pf_dist/2`) for candidates about to be hash_removed,
reducing DRAM stalls further when the pool is DRAM-resident and eviction
rate is high. The default `cache_expire()` path switches to `2stage`
automatically only when:

- `max_entries >= 4096`
- `cache_expire_level() >= 4`

`cache_expire_level() >= 4` corresponds to the point where fill pressure
has reached the top scan tier, which is roughly beyond 75% occupancy for
power-of-two pools. Standard-sizing packet-loop benchmarks stay below that
zone, so they normally use the buffered default path, while tight/full
configurations are the ones that exercise `2stage`.

For built-in flow-cache keys, the hash stage also bypasses the generic
byte-loop CRC helper when the key size is known at compile time.  The
current specialisations cover 20-byte IPv4 keys and 44-byte IPv6/unified
keys.

#### Sweep coverage

- Cursor advances by `scan` entries per call regardless of eviction count
- Full pool traversal period = max_entries / (scan × batches/s)
- At 2 Mpps, batch=256:
  - Level 0 (scan=64): 1M pool → full sweep in ~2.0 s
  - Level 4 (scan=1024): 1M pool → full sweep in ~0.13 s
- Minimum timeout 1.0 s — level-0 sweep always covers it.

### 8.3 Insert guarantee: three-tier fallback

`cache_insert()` always succeeds (never returns NULL).  Three tiers
ensure a free entry is always available:

```
1. Free list       → O(1) SLIST_FIRST pop
2. evict_one       → cursor scan up to 1/8 of pool for expired entries
3. evict_bucket_oldest → force-evict the oldest entry in the target bucket
```

#### evict_one (expired scan, 1/8 bound)

```c
static struct entry *
cache_evict_one(struct cache *fc, uint64_t now)
{
    const unsigned max_scan = fc->max_entries >> 3;  /* 1/8 of pool */
    for (unsigned i = 0; i < max_scan; i++) {
        prefetch(&pool[(cursor + pf_dist) & mask]);
        entry = &pool[cursor];
        cursor = (cursor + 1) & mask;
        if (entry->last_ts == 0) continue;
        if (now - entry->last_ts <= timeout) continue;
        hash_remove(entry);
        return entry;
    }
    return NULL;  /* no expired entry found in 1/8 scan */
}
```

The 1/8 bound caps worst-case cost:

| Pool | Full scan | 1/8 scan |
|---|---|---|
| 100K | 33,207 cy/pkt | 3,988 cy/pkt |
| 1M | — | acceptable |
| 4M | — | acceptable |

With a full scan, 100K pool could hit 33K cy/pkt — scanning all 131K×128B
= 16MB of memory when all entries are alive.  The 1/8 bound reduces this to
16K entries = 2MB.

#### evict_bucket_oldest (force evict, last resort)

When evict_one finds no expired entry, identify target buckets (bk0, bk1)
from the insertion key's hash, then force-evict the oldest entry across both:

```c
static struct entry *
cache_evict_bucket_oldest(struct cache *fc, const struct key *key)
{
    /* Hash key to locate bk0, bk1 */
    hash = crc32(key);
    find bk0, bk1 from hash;

    /* Scan all 32 slots (16×2 buckets) for oldest entry */
    oldest = NULL;
    for each slot in bk0, bk1:
        if entry->last_ts < oldest_ts:
            oldest = entry;

    hash_remove(oldest);
    return oldest;
}
```

This function **simultaneously frees a pool entry and a bucket slot**, so
the subsequent `ht_insert` takes the fast path (no cuckoo displacement needed).

This is the last resort — only fires when evict_one finds no expired entry
in its 1/8-pool scan.  In practice this is extremely rare because adaptive
timeout (§8.2) shortens the effective timeout long before the pool saturates.

### 8.4 Simulation results and pool sizing

Simulation via `expire_sim.py` (2 Mpps, base_timeout=5.0s, min_timeout=1.0s):

#### Steady-state by pool size

| miss% | new/s | 100K fill% | 100K TO | 1M fill% | 1M TO | 4M fill% | 4M TO |
|---|---|---|---|---|---|---|---|
| 5% | 39K | 19.5% | 5.0s | 19.5% | 5.0s | 4.9% | 5.0s |
| 10% | 78K | 39.1% | 5.0s | 39.1% | 5.0s | 9.8% | 5.0s |
| 20% | 156K | 78.1% | 5.0s | 78.1% | 5.0s | 19.5% | 5.0s |
| 30% | 234K | 100.0% | 2.2s | 100.0% | 4.5s | 29.3% | 5.0s |
| 50% | 390K | 100.0% | 1.3s | 100.0% | 2.6s | 48.8% | 5.0s |

Effective timeout is determined solely by miss rate, not pool size.
When the pool saturates, fill-level-driven scan further shortens timeout.

#### Key observations

- **5-10% miss**: normal operation — all pool sizes stay well below capacity,
  timeout at maximum.
- **20% miss**: 100K pool at 78% fill (still stable).
- **30%+ miss**: 100K and 1M pools saturate; timeout auto-shortens.
  4M pool has headroom.
- **Natural feedback**: high miss rate → more ACL slow-path cost → effective
  pps drops → fewer new flows/s → fill rate decreases.

#### Pool sizing guideline

```
pool_size ≥ max_new_flow_rate × base_timeout × 2
```

| Scenario | Recommended pool |
|---|---|
| 2Mpps, 10% miss, 5s TO | ≥ 780K → 1M |
| 2Mpps, 20% miss, 5s TO | ≥ 1.56M → 2M |
| 2Mpps, 30% miss, 5s TO | ≥ 2.34M → 4M |

Typical miss rates: 5-10% normal, 15-20% peak, 30%+ under attack.

#### Sweep coverage

- Cursor advances by `scan` per call (independent of eviction count)
- Full pool sweep period = max_entries / (scan × batches_per_second)
- For timeout N seconds, sweep period must be < N.
  At typical sizes and `timeout_ms` ≥ 1000, coverage is sufficient.

### 8.5 Timeout parameter

```c
void cache_init(..., flow_cache_backend_t backend, uint64_t timeout_ms);
```

Timeout value is a runtime parameter.
Appropriate value depends on workload:
- Short-lived flows (web): 1-5 seconds
- Long-lived flows (streaming): 30-60 seconds
- 0 = no expiry (`timeout_tsc = UINT64_MAX`)

Backend selection is also a runtime parameter:
- `FLOW_CACHE_BACKEND_AUTO` picks the best supported backend at init time
- `FLOW_CACHE_BACKEND_GEN` forces the generic backend
- `FLOW_CACHE_BACKEND_SSE` requests the SSE4.2/XMM backend
- `FLOW_CACHE_BACKEND_AVX2` / `FLOW_CACHE_BACKEND_AVX512` request SIMD backends
- Unsupported explicit requests fall back to `GEN`

## 9. Free List Management

- Pre-allocated pool of entry[max_entries]
- `max_entries` must be a power of 2 and at least 64
- At init: all entries pushed to SLIST free list
- insert: pop from free list → if empty, evict_one
- expire/evict: push back to free list
- `free_link` field in CL0 (entry is either in hash table or on
  free list, never both; CL0 covers both last_ts and free_link)

## 10. API

All three variants expose the same API shape via macro templates.
Function names use the variant prefix: `flow4_`, `flow6_`, or `flowu_`.

```c
/* Initialization — caller provides pre-allocated memory.
 * init_cb: called after each successful insert (NULL installs a shared no-op callback).
 * fini_cb: called before every eviction/remove/flush (NULL installs a shared no-op callback).
 * Hot paths skip the indirect callback call entirely while those default
 * no-op callbacks are active.
 * max_entries must already be rounded via flow_cache_pool_count():
 * power-of-2 and >= 64.
 * backend is a request; unsupported SIMD requests fall back to GEN. */
void PREFIX_cache_init(struct PREFIX_cache *fc,
                       struct rix_hash_bucket_s *buckets,
                       unsigned nb_bk,
                       struct PREFIX_entry *pool,
                       unsigned max_entries,
                       flow_cache_backend_t backend,
                       uint64_t timeout_ms,
                       void (*init_cb)(struct PREFIX_entry *entry, void *arg),
                       void (*fini_cb)(struct PREFIX_entry *entry,
                                       flow_cache_fini_reason_t reason,
                                       void *arg),
                       void *cb_arg);

/* Flush — remove all entries, fini_cb called for each */
void PREFIX_cache_flush(struct PREFIX_cache *fc);

/* Single-key lookup (non-pipelined) */
struct PREFIX_entry *PREFIX_cache_find(struct PREFIX_cache *fc,
                                       const struct PREFIX_key *key);

/* Batch lookup — pipelined hot path */
void PREFIX_cache_lookup_batch(struct PREFIX_cache *fc,
                               const struct PREFIX_key *keys,
                               unsigned nb_pkts,
                               struct PREFIX_entry **results);
unsigned PREFIX_cache_lookup_touch_batch(struct PREFIX_cache *fc,
                                         const struct PREFIX_key *keys,
                                         unsigned nb_pkts,
                                         uint64_t now,
                                         struct PREFIX_entry **results);

/* Insert — called on miss, bucket L2 warm; always succeeds (3-tier fallback).
 * init_cb(entry, cb_arg) is called after successful insert. */
struct PREFIX_entry *PREFIX_cache_insert(struct PREFIX_cache *fc,
                                         const struct PREFIX_key *key,
                                         uint64_t now);

/* Explicit remove — fini_cb called with FLOW_CACHE_FINI_REMOVED */
void PREFIX_cache_remove(struct PREFIX_cache *fc,
                         struct PREFIX_entry *entry);

/* Touch — refresh timestamp (inline, ~10 cy).
 * Update entry->userdata directly after returning. */
static inline void
PREFIX_cache_touch(struct PREFIX_entry *entry, uint64_t now);

/* Miss-rate-driven timeout adjustment (inline, call once per batch) */
static inline void
PREFIX_cache_adjust_timeout(struct PREFIX_cache *fc, unsigned misses);

/* Adaptive expire — scan depth auto-scales with fill level and auto-switches
 * to the 2stage path on large, high-pressure pools (always call). */
void PREFIX_cache_expire(struct PREFIX_cache *fc, uint64_t now);

/* Two-stage expire — adds mid-distance bucket prefetch for DRAM-resident pools */
void PREFIX_cache_expire_2stage(struct PREFIX_cache *fc, uint64_t now);

/* Entry count (inline) */
static inline unsigned
PREFIX_cache_nb_entries(const struct PREFIX_cache *fc);

/* Actual backend selected for this cache instance (inline) */
static inline flow_cache_backend_t
PREFIX_cache_backend(const struct PREFIX_cache *fc);

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
is defined in each variant's header. Most logic is generated directly from
templates; each variant additionally uses a thin public wrapper that selects
between multiple precompiled backends.

### 11.1 Template parameters

| Macro | Example (IPv4) | Purpose |
|---|---|---|
| `FC_PREFIX` | `flow4` | Function name prefix |
| `FC_ENTRY` | `flow4_entry` | Entry struct tag |
| `FC_KEY` | `flow4_key` | Key struct tag |
| `FC_CACHE` | `flow4_cache` | Cache struct tag |
| `FC_HT` | `flow4_ht` | Hash table head struct tag |
| `FC_FREE_HEAD` | `flow4_free_head` | Free list head tag |

### 11.2 Template files

| File | Generates | Used by |
|---|---|---|
| `include/flow_cache_decl.h` | Cache struct, API declarations, inline helpers | public variant `.h` files |
| `src/backend.h` | Backend ops table layout | fat-backend `.c` files |
| `src/hash_direct.h` | direct-find `RIX_HASH_GENERATE_STATIC_EX` expansion | fat-backend `.c`, test-only raw-hash `.c` |
| `src/body.h` | init, insert, insert_batch, lookup_batch, expire, stats, perf pkt-loop mixes | `.c` files |
| `test/fcache_test_body.h` | Test + benchmark functions | test `.c` |

### 11.3 Adding a new variant

To add a new flow cache variant (e.g., MPLS):

1. Define key struct, entry struct, comparison function
2. `RIX_HASH_HEAD` / `RIX_HASH_GENERATE`
3. Set `FC_*` macros, `#include "flow_cache_decl.h"` in header (twice: once before structs, once after)
4. Set `FC_*` macros, `#include "body.h"` in source
5. Set `FCT_*` macros, `#include "fcache_test_body.h"` in test

For a new variant, the preferred pattern is:
1. thin public wrapper for runtime backend selection
2. one backend template source compiled multiple times (`gen`, `sse`, `avx2`, `avx512`)
3. shared template reuse for the full implementation body

## 12. File Structure

```
samples/
  README.md                this document
  README_JP.md             Japanese version
  Makefile                 top-level: delegates to fcache/ and test/

  fcache/                  v1 library (128B 2-CL entries, user payload)
    Makefile
    include/
      flow_cache.h             umbrella: includes all three variant headers
      flow_cache_decl.h        base defs (Section 1) + cache struct/API decls (Section 2)
      flow4_cache.h            IPv4: key, entry, cmp, public API
      flow6_cache.h            IPv6: key, entry, cmp, public API
      flow_unified_cache.h     Unified: key (family+union), entry, cmp, public API
    src/
      backend.h               backend ops table
      body.h                  implementation template (init/insert/lookup/expire)
      hash_direct.h           direct-find hash generate template
      flow4.c                 IPv4: public wrapper + backend selection
      flow4_backend.c         IPv4: backend template, compiled as gen/sse/avx2/avx512
      flow6.c                 IPv6: public wrapper + backend selection
      flow6_backend.c         IPv6: backend template, compiled as gen/sse/avx2/avx512
      flowu.c                 Unified: public wrapper + backend selection
      flowu_backend.c         Unified: backend template, compiled as gen/sse/avx2/avx512
    lib/                      build output (libfcache.a / libfcache.so)

  fcache/                 v2 library (64B 1-CL entries, no user payload)
    Makefile
    include/
      flow_cache2.h            umbrella: includes all three variant headers
      flow4_cache2.h           IPv4: 20B key, 64B entry, public API
      flow6_cache2.h           IPv6: 44B key, 64B entry, public API
      flowu_cache2.h           Unified: 44B key (family+union), 64B entry, public API
    src/
      flow4.c                 IPv4: AVX2-bound cuckoo hash + batch pipeline
      flow6.c                 IPv6: AVX2-bound cuckoo hash + batch pipeline
      flowu.c                 Unified: AVX2-bound cuckoo hash + batch pipeline
    lib/                      build output (libfcache.a / libfcache.so)

  test/                    v1 test and benchmark binary
    Makefile
    fcache_test.c            correctness tests + benchmarks (all 3 variants)
    fcache_test_body.h       template: test + benchmark functions
    ht4_backend.c            flow4 raw-hash template, compiled as gen/sse/avx2/avx512
    ht4.h                    declarations for raw flow4 hash test helpers
    perf.sh                  perf stat wrapper for single benchmark cases

tests/fcache/             v2 test and benchmark binaries
  Makefile
  test_flow_cache2.c         correctness tests (all 3 variants, macro-template)
  bench_flow_cache2.c        v1-vs-v2 comparison bench (flow4)
  bench_fc_variants.c       v2 variant comparison bench (flow4 vs flow6 vs flowu)
```

## 13. Build Dependencies

```
rix/rix_defs_private.h  index macros, utilities (private: included by rix headers)
rix/rix_queue.h         SLIST (free list)
rix/rix_hash.h          fingerprint hash table (SIMD dispatch)
```

No TAILQ dependency (LRU replaced by timestamp-only approach).
Requires `-mavx2` or `-mavx512f` for SIMD-accelerated hash operations.
The sample Makefiles default to `OPTLEVEL=3` and are intended to build with
both `CC=gcc` and `CC=clang`.

## 14. Thread Safety

The flow cache is designed for a **per-thread, lock-free** model.

### Model

| Aspect | Detail |
|---|---|
| Instance ownership | One `flow4_cache` / `flow6_cache` per thread |
| Synchronization | None — no locks, no atomics, no RCU |
| Shared state | None between threads |
| age_cursor | Per-cache; incremented only by owning thread |

### Rationale

In the target environment (VPP plugin, DPDK poll-mode):

- Each worker thread owns a private packet queue and processes it exclusively.
- The flow cache holds thread-local state (per-flow counters, action cache).
- No inter-thread sharing means no false sharing, no contention, and no cache-line bouncing.
- This matches VPP's "no global mutable state" architecture.

### Implications for users

- **Do not share** a single cache instance between threads.
- **Do not call** any flow cache API from two threads concurrently on the same instance.
- To process traffic across multiple cores, allocate one cache per worker thread.
- If cross-thread access to flow statistics is required, read them from a safe point
  (e.g., control-plane thread reading counters after a quiescent period) or copy to a
  separate stats structure with appropriate memory ordering.

### Shared memory usage

librix data structures (hash table, free list) store **indices, not pointers**.
This makes the raw data relocatable — valid across processes mapping the same
shared memory region at different virtual addresses.  However, concurrent
modification still requires external coordination (e.g., per-shard locks or
RCU) when multiple processes share the same `flow_cache` instance.
The single-writer model above removes this requirement entirely.

## 15. Competitive Analysis

### 15.1 Feature comparison

| Feature | **librix** | **DPDK rte_hash** | **OVS EMC** | **VPP bihash** | **nf_conntrack** |
|---|---|---|---|---|---|
| Structure | 16-way cuckoo, FP | 8-way cuckoo | direct-mapped | 4/8-way cuckoo | chained hash |
| Key storage | node (FP bucket) | in bucket | in node | in bucket | in node |
| Lookup | 4-stage pipeline+SIMD | no pipeline | 1CL direct | no pipeline | chain walk |
| Remove | O(1) cur_hash | O(1) position | O(1) | O(n) rehash | O(1) hlist |
| Shared memory | **native** | No (pointer) | No | No | No |
| Eviction | adaptive scan+force | none (manual) | timestamp | none | conntrack GC |
| SIMD | AVX2/512 FP scan | CRC32 only | none | none | none |
| Batch | native | bulk lookup | none | none | none |
| Thread model | per-thread, lock-free | RCU or lock | per-thread | per-thread | RCU + spinlock |

### 15.2 Individual comparisons

#### vs DPDK rte_hash

rte_hash is the de facto standard for data-plane hash tables.  8-way
buckets achieve high fill rates, but pipelined batch lookup is not provided.
`rte_hash_lookup_bulk` makes independent DRAM accesses per key — latency
accumulates with cold caches.  librix's 4-stage pipeline overlaps DRAM
accesses, giving a 5-8x advantage at large pool sizes.

rte_hash has mature DPDK ecosystem integration (mempool, ring, EAL) and
higher operational maturity beyond raw hash performance.

#### vs OVS EMC (Exact Match Cache)

EMC is direct-mapped (1-way): on hit, only 1 CL access.  Extremely fast
but high miss rate (any collision causes a miss).  EMC serves as a front
cache for SMC (Signature Match Cache = cuckoo) in a 2-tier design.

librix uses a single 16-way cuckoo tier — no fast hit-only path, but miss
rate is orders of magnitude lower.  Overall packet processing throughput
typically favors librix.

#### vs VPP bihash

bihash is VPP's standard hash table.  Remove requires O(n) rehash —
unsuitable for frequent eviction.  Flow cache use cases need frequent
remove, making cur_hash-based O(1) remove a decisive advantage.
bihash also lacks pipelined lookup, so performance gap widens at scale.

#### vs nf_conntrack

Linux kernel connection tracker.  Chained hash has scalability limits;
GC uses RCU + timer.  Not designed for data-plane acceleration — its
strength is feature richness (NAT, helper, expectation).  Different
design goals make direct comparison inappropriate.

### 15.3 Architectural strengths

**Index-based design (primary differentiator)**

No pointer storage means shared memory, mmap, and cross-process access
work natively — a property no other high-performance flow table
implementation provides.  Decisive advantage when VPP plugins or
multi-process access is required.

**Memory access pattern optimization**

Consistent CL0/CL1 separation.  Placing `last_ts` in CL0 means expire
scan completes in CL0 only — alive entries never touch CL1.  The most
frequent operations (lookup → hit decision) complete with minimum cache
line accesses.

**4-stage pipeline effect**

Measured 2.8x (L2) → 8.1x (DRAM-cold) speedup.  Effect grows with pool
size — correctly achieving DRAM latency hiding.  40-143 cy/key is
excellent for DRAM-cold conditions.

### 15.4 Eviction strategy assessment

**Adaptive timeout design is sound**

- Decay/recovery equilibrium converges stably (control-theory perspective)
- Shift-only arithmetic, no division — appropriate for data plane
- 1.0s minimum floor guarantees cache effectiveness at any miss rate

**Insert guarantee via 3-tier fallback**

Correct design decision: cache insert should never fail.
`evict_bucket_oldest` simultaneously frees a pool entry and a bucket slot,
ensuring the subsequent `ht_insert` always takes the fast path — elegant.

**evict_one 1/8 bound**

The 1/8 bound caps worst-case cost, but when all entries are alive and no
expired entry is found, every call incurs 1/8-scan → evict_bucket_oldest —
potentially thousands of cycles per insert.  In practice adaptive timeout
shortens the effective timeout first, so evict_bucket_oldest is extremely
rare in real workloads.

### 15.5 Areas for improvement

- Batch insert (pipeline across multiple misses) not implemented.
  Potential improvement when miss rate is high.

**Functionality**

- No per-flow timeout (e.g., TCP established vs SYN) — all entries share
  the same effective timeout.
- No runtime timeout change API (init-time only).

**Operations**

- Dynamic pool resize is not supported (pre-allocated, fixed size).

### 15.6 Summary

An exceptionally complete design for a data-plane flow cache:

1. **Pipeline + SIMD + CL placement** designed coherently throughout
2. **Adaptive eviction** eliminates manual tuning while maintaining stability
3. **Insert guarantee** ensures cache reliability
4. **Index-based** enables shared memory deployment

150-215 cy/pkt (lookup + insert + expire combined) = 10 Mpps/core at 2 GHz
Xeon — sufficient for practical deployments.  Template architecture delivers
three variants with zero code duplication, maintaining high maintainability.

---

## 16. Benchmark Results

Measured with `samples/test/fc_bench`.  entries=32768, nb_bk=4096, query=256.
Each variant fills 50% (16384 active entries) before measurement.

### 16.1 Datapath Performance (cycles/key, no expire)

| Operation | flow4 | flow6 | flowu | 6/4 | u/4 |
|-----------|------:|------:|------:|----:|----:|
| hit_lookup | **37** | 51 | 53 | +36% | +42% |
| miss_fill | **110** | 139 | 142 | +26% | +29% |
| mixed 90/10 | **47** | 61 | 65 | +28% | +38% |

flow4 uses inline CRC32C (`__builtin_ia32_crc32di` x3) and XOR-based 24B key
comparison, bypassing the `rix_hash_arch->hash_bytes` function-pointer dispatch
and `memcmp`.  The 26-42% advantage over flow6/flowu comes primarily from
**function-pointer elimination**, not key-size difference.

hit_lookup at 37 cy (L1-hot) translates to ~80 Mpps at 3 GHz.

### 16.2 Maintenance: Full-Table Sweep (fill=75%, all expired)

| Table size | flow4 cy/entry | flow6 cy/entry | flowu cy/entry |
|-----------|------:|------:|------:|
| 0.6 MB (1K bk) | 19 | 20 | 20 |
| 5.0 MB (8K bk) | 26 | 29 | 29 |
| 72 MB (64K bk) | 66 | 65 | 61 |

- **L2-resident (≤5 MB)**: 16-20 cy/entry, all variants equivalent.
  No key comparison during expire (TSC check only), so flow4's
  inline optimization has no effect.
- **DRAM-resident (72 MB)**: ~60-66 cy/entry (~3x degradation).
  Memory-latency dominated — random node-array access hits DRAM.
  Variant differences vanish under memory pressure.

### 16.3 Maintenance: Partial Sweep (fill=75%, all expired)

| Table | step | flow4 cy/entry | flow6 cy/entry | flowu cy/entry |
|-------|-----:|------:|------:|------:|
| 0.6 MB | 64-1024 | 19-21 | 20-21 | 19-21 |
| 5.0 MB | 64-1024 | 24-28 | 25-30 | 24-29 |
| 72 MB | 64-1024 | 61-62 | 62-63 | 63-67 |

Step-size has minimal impact on per-entry cost (±15%).  Cursor management
overhead is negligible, confirming that interleaving small maintenance
steps between packet batches does not hurt efficiency.

At 72 MB / step=64: ~47K cy/call ≈ 16 µs @3 GHz — fits comfortably
between packet batches.

### 16.4 Summary

| Metric | Value | Note |
|--------|-------|------|
| flow4 hit lookup | 37 cy | ~80 Mpps @3 GHz |
| flow4 mixed 90/10 | 47 cy | Realistic workload |
| flow4 miss fill | 110 cy | Including hash + insert |
| Maintenance (L2) | 16-20 cy/entry | Variant-independent |
| Maintenance (DRAM) | 60-66 cy/entry | Memory-latency bound |
| Partial sweep overhead | <15% | Step-size independent |

- **flow4** is clearly the fastest variant for datapath — inline CRC32C +
  XOR compare eliminates dispatch overhead
- **flow6/flowu** pay 26-42% more in datapath but share identical
  maintenance performance
- **Maintenance scales linearly** with entry count; partial sweep
  amortizes cleanly across packet batches
- **DRAM is the bottleneck** at large table sizes — all variants converge

---

## 17. Architecture-Specific Dispatch

### 17.1 Design Overview

The flowcache compiles the **same source** at four architecture levels
and selects the best implementation at runtime via a function-pointer
table (ops table).  This lets the compiler apply arch-specific
optimizations — instruction selection, scheduling, auto-vectorization —
without hand-written SIMD in the application code.

```
                      fc_arch_init(FC_ARCH_AUTO)
                              │
                              ▼
┌─────────────────────────────────────────────────┐
│  fc_dispatch.o  (no arch flags)                 │
│  - rix_hash_arch_init() for this TU             │
│  - FC_OPS_SELECT() per variant                  │
│  - unsuffixed API wrappers                      │
│    hot-path  → _fc_flow4_active->lookup_batch() │
│    cold-path → fc_flow4_cache_init_gen()        │
└────────────────────┬────────────────────────────┘
                     │ selects ops table
      ┌──────────────┼──────────────┐
      ▼              ▼              ▼
 ops_gen        ops_avx2       ops_avx512 ...
 (flow4/6/u)   (flow4/6/u)   (flow4/6/u)
```

**Key points**:

- The public API uses **unsuffixed** function names
  (`fc_flow4_cache_lookup_batch`, etc.).  These are thin wrappers
  in `fc_dispatch.c` that forward to the selected ops table.
- **Hot-path** functions (lookup, fill, maintain) dispatch through
  the ops pointer.  **Cold-path** functions (init, flush, stats,
  remove, nb_entries) forward directly to the `_gen` build.
- Each arch-specific TU has a `__attribute__((constructor))` that
  calls `rix_hash_arch_init(RIX_HASH_ARCH_AUTO)`, ensuring
  `rix_hash_hash_bytes_fast()` uses the correct SIMD path.

### 17.2 Architecture Tiers

| Tier | Compiler flags | Key features | Target |
|------|----------------|-------------|--------|
| GEN | (none) | Scalar only, no CRC32C | ARM portability baseline |
| SSE4.2 | `-msse4.2` | CRC32C, 128-bit SIMD | Older x86_64 servers |
| AVX2 | `-mavx2 -msse4.2` | 256-bit SIMD | Mainstream Xeon |
| AVX-512 | `-mavx512f -mavx2 -msse4.2` | 512-bit SIMD | Xeon Scalable (primary target) |

GEN exists as a portability baseline for future ARM support.
On x86_64, SSE2 is ABI-guaranteed; SSE4.2 is not (absent on pre-2008
Core 2), but all practical Xeon targets have it.

### 17.3 Build Matrix

Each variant source (e.g. `flow4.c`) is compiled four times with
`-DFC_ARCH_SUFFIX=_<tier>` and the corresponding `-m` flags.
The dispatch wrapper is compiled once without arch flags.

```makefile
VARIANTS = flow4 flow6 flowu

# GEN (portable, no SIMD flags)
$(LIBDIR)/%_gen.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -DFC_ARCH_SUFFIX=_gen -c -o $@ $<

# SSE4.2
$(LIBDIR)/%_sse.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -msse4.2 -DFC_ARCH_SUFFIX=_sse -c -o $@ $<

# AVX2
$(LIBDIR)/%_avx2.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -mavx2 -msse4.2 -DFC_ARCH_SUFFIX=_avx2 -c -o $@ $<

# AVX-512
$(LIBDIR)/%_avx512.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -mavx512f -mavx2 -msse4.2 -DFC_ARCH_SUFFIX=_avx512 -c -o $@ $<

# Dispatch wrapper (no arch flags, no FC_ARCH_SUFFIX)
$(LIBDIR)/fc_dispatch.o: $(SRCDIR)/fc_dispatch.c
	$(CC) $(CFLAGS) -c -o $@ $<
```

Total objects: 3 variants x 4 tiers + 1 dispatch = **13 objects**.

`FC_ARCH_SUFFIX` causes `FC_CACHE_GENERATE` to append the suffix to
all generated function names (e.g. `fc_flow4_cache_lookup_batch_avx2`).
Without the macro, original unsuffixed names are generated.

### 17.4 Hash Function Strategy

Hash functions remain **inline, macro-controlled** — not dispatched
via function pointers.  Inlining is critical for pipeline performance.

```c
static inline union rix_hash_hash_u
fc_flow4_hash_fn(const struct fc_flow4_key *key, uint32_t mask)
{
#if defined(__x86_64__) && defined(__SSE4_2__)
    /* Direct CRC32C — 3 x crc32q for 24B key */
    ...
#else
    /* GEN fallback — rix_hash_hash_bytes_fast() */
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
#endif
}
```

When compiled with `-msse4.2` or higher, `__SSE4_2__` is
defined and CRC32C is used.  The GEN build falls back to the
multiplicative hash.  No source changes needed — the compiler flag
controls the code path.

The same applies to SIMD find operations (`_RIX_HASH_FIND_U32X16_2`):
when `__AVX2__` is defined, `fc_cache_generate.h` rebinds the macro
to the AVX2 intrinsic directly at file scope.

For `rix_hash_hash_bytes_fast()` (used by flow6/flowu GEN fallback),
each arch TU auto-initializes its per-TU `rix_hash_arch` pointer via
a `__attribute__((constructor))` generated by `FC_CACHE_GENERATE`.

### 17.5 Ops Table Structure

```c
/* Generated by FC_OPS_DEFINE(flow4) in fc_ops.h */
struct fc_flow4_ops {
    unsigned (*lookup_batch)(struct fc_flow4_cache *fc,
                             const struct fc_flow4_key *keys,
                             unsigned nb_keys, uint64_t now,
                             struct fc_flow4_result *results,
                             uint16_t *miss_idx);
    unsigned (*fill_miss_batch)(struct fc_flow4_cache *fc,
                                const struct fc_flow4_key *keys,
                                const uint16_t *miss_idx,
                                unsigned miss_count, uint64_t now,
                                struct fc_flow4_result *results);
    unsigned (*maintain)(struct fc_flow4_cache *fc,
                         unsigned start_bk, unsigned bucket_count,
                         uint64_t now);
    unsigned (*maintain_step_ex)(struct fc_flow4_cache *fc,
                                 unsigned start_bk, unsigned bucket_count,
                                 unsigned skip_threshold, uint64_t now);
    unsigned (*maintain_step)(struct fc_flow4_cache *fc,
                              uint64_t now, int idle);
};
```

One ops table instance per arch tier, per variant (12 total).
Generated by `FC_OPS_TABLE(prefix, FC_ARCH_SUFFIX)` in each
arch-specific `.c` file.

### 17.6 Runtime Selection

```c
/* fc_dispatch.c */
#include <rix/rix_hash_arch.h>
#include "fc_ops.h"

static const struct fc_flow4_ops *_fc_flow4_active = &fc_flow4_ops_gen;
static const struct fc_flow6_ops *_fc_flow6_active = &fc_flow6_ops_gen;
static const struct fc_flowu_ops *_fc_flowu_active = &fc_flowu_ops_gen;

void
fc_arch_init(unsigned arch_enable)
{
    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);   /* for this TU */
    FC_OPS_SELECT(flow4, arch_enable, &_fc_flow4_active);
    FC_OPS_SELECT(flow6, arch_enable, &_fc_flow6_active);
    FC_OPS_SELECT(flowu, arch_enable, &_fc_flowu_active);
}

/* Hot-path wrapper example */
unsigned
fc_flow4_cache_lookup_batch(struct fc_flow4_cache *fc,
                            const struct fc_flow4_key *keys,
                            unsigned nb_keys, uint64_t now,
                            struct fc_flow4_result *results,
                            uint16_t *miss_idx)
{
    return _fc_flow4_active->lookup_batch(fc, keys, nb_keys, now,
                                          results, miss_idx);
}

/* Cold-path wrapper example */
void
fc_flow4_cache_init(struct fc_flow4_cache *fc, ...)
{
    fc_flow4_cache_init_gen(fc, ...);
}
```

`FC_OPS_SELECT` uses `__builtin_cpu_supports()` to probe the CPU
and selects the best available ops table (AVX-512 > AVX2 > SSE > GEN).
The dispatch cost is one indirect call per batch — negligible.

**Caller usage** — one init call replaces both `rix_hash_arch_init()`
and per-variant ops selection:

```c
int main(void) {
    fc_arch_init(FC_ARCH_AUTO);   /* does everything */
    /* ... use fc_flow4_cache_init(), fc_flow4_cache_lookup_batch(), etc. */
}
```

### 17.7 Portability Notes

The runtime detection uses `__builtin_cpu_supports()` (GCC ≥ 4.8,
Clang ≥ 3.7).  On x86_64, this calls `cpuid` internally.  On AArch64
(GCC ≥ 14), it reads `getauxval(AT_HWCAP)`.

**Alternative detection methods** if `__builtin_cpu_supports()` is
unavailable or insufficient:

| Method | Portability | Notes |
|--------|-------------|-------|
| `getauxval(AT_HWCAP)` | Linux (glibc/musl) | `<sys/auxv.h>`, works on x86_64 and ARM |
| `elf_aux_info()` | FreeBSD | BSD equivalent of getauxval |
| Direct `cpuid` | x86 only | `<cpuid.h>`, no OS dependency |
| `/proc/cpuinfo` | Linux only | Slow (file I/O + string parse), not for init hot path |
| `IsProcessorFeaturePresent()` | Windows | Win32 API |

To port to a new platform:

1. Add a new tier constant (e.g. `FC_ARCH_NEON`)
2. Compile the variant sources with the appropriate `-march` flag
   and `-DFC_ARCH_SUFFIX=_neon`
3. Add detection logic in `_FC_OPS_SELECT_BODY` using the
   platform's feature-detection API
4. Add `FC_OPS_DECLARE(flow4, _neon)` etc. in `fc_ops.h`
5. No changes to pipeline code or public API

### 17.8 File Summary

| File | Role |
|------|------|
| `fc_cache_generate.h` | `FC_CACHE_GENERATE` — generates all functions with optional suffix; emits per-TU `rix_hash_arch` constructor; `FC_OPS_TABLE` generates ops table instance |
| `fc_ops.h` | `FC_OPS_DEFINE` — ops struct definition; `FC_OPS_DECLARE` — extern declarations; `FC_OPS_SELECT` — runtime CPU detection; `FC_COLD_DECLARE` — cold-path `_gen` externs; `fc_arch_init()` prototype |
| `fc_dispatch.c` | Unsuffixed public API wrappers; `fc_arch_init()` implementation (absorbs `rix_hash_arch_init()`) |
| `flow4.c`, `flow6.c`, `flowu.c` | Variant-specific hash/cmp + `FC_CACHE_GENERATE` + `FC_OPS_TABLE` |

### 17.9 Test Script

Run benchmarks and save results as evidence:

```sh
./run_fc_bench_all.sh              # auto-detect arch, save to bench_results/
./run_fc_bench_all.sh -o /tmp/out  # custom output directory
```

Results are saved to `bench_results/<hostname>_<timestamp>.txt`.
Run on each target machine to collect cross-platform evidence.

