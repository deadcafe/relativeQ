# fcache IPv6 / Unified Extension Design

**Date:** 2026-03-19
**Status:** Approved
**Scope:** samples/fcache — add flow6 (IPv6) and flowu (Unified IPv4/IPv6) variants

## Context

fcache (v2) currently supports only IPv4 (flow4). v1 (fcache) has all three
variants (flow4/flow6/flowu) but v2 is the active line of development and will
eventually replace v1. librix itself is still evolving; samples drive its
completion and must be production-quality.

## Goals

1. Add flow6 and flowu to fcache with the same API pattern as flow4.
2. All entries remain 64 bytes (single cache line) — no user payload; AP uses
   the entry index to reference its own data structures.
3. Tests and benchmarks for all three variants.
4. Update DESIGN.md / DESIGN_JP.md documentation.
5. Keep AVX2-only for now; arch portability is a future phase.

## Entry Structures (all 64B = 1CL)

### fc_flow4_entry (existing)

| Offset | Size | Field       |
|--------|------|-------------|
| 0-19   | 20B  | key         |
| 20-23  |  4B  | cur_hash    |
| 24-31  |  8B  | last_ts     |
| 32-35  |  4B  | free_link   |
| 36-37  |  2B  | slot        |
| 38-39  |  2B  | reserved1   |
| 40-63  | 24B  | reserved0   |

### fc_flow6_entry (new)

| Offset | Size | Field       |
|--------|------|-------------|
| 0-43   | 44B  | key (5-tuple + vrfid) |
| 44-47  |  4B  | cur_hash    |
| 48-55  |  8B  | last_ts     |
| 56-59  |  4B  | free_link   |
| 60-61  |  2B  | slot        |
| 62-63  |  2B  | reserved1   |

fc_flow6_key: src_ip[16] + dst_ip[16] + src_port(2) + dst_port(2) +
proto(1) + pad[3] + vrfid(4) = 44 bytes.

### fc_flowu_entry (new)

Same layout as flow6. fc_flowu_key: family(1) + proto(1) + src_port(2) +
dst_port(2) + pad(2) + vrfid(4) + addr union(32) = 44 bytes. IPv4 entries
zero-pad the unused 24 bytes in addr.v4._pad. Family field prevents v4/v6
collisions.

**Note:** flowu reorders fields vs flow6 so that family+proto are at offset 0
(matches v1 flowu_key ordering). flow6_key follows the flow4 ordering:
addrs first, then ports/proto/vrfid.

### Design notes

- **Maximum density:** flow6/flowu entries have zero reserved bytes beyond
  reserved1(2B). No room for future per-entry metadata. If the key needs to
  grow, a 2CL entry or different key encoding would be required. This is a
  conscious tradeoff for cache density.
- **`__attribute__((packed))`:** All key structs use packed (same as v1) to
  ensure no compiler-inserted padding changes memcmp semantics.
- **Thread safety:** Each cache instance is single-threaded; caller is
  responsible for partitioning across threads.
- **Pipeline geometry:** Initial implementation uses the same
  FLOW_CACHE_LOOKUP_STEP_KEYS / AHEAD_STEPS for all variants. Tuning for
  44B keys deferred.
- **cmp function:** Each variant defines its own `fc_{flow6,flowu}_cmp()`
  non-static function, passed to `RIX_HASH_GENERATE_STATIC_SLOT_EX`.

## File Structure

### Headers (samples/fcache/include/)

| File              | Description                                   |
|-------------------|-----------------------------------------------|
| flow_cache.h     | Umbrella — includes all three variant headers |
| flow4_cache.h    | Existing — unchanged                          |
| flow6_cache.h    | New — flow6 key/entry/cache/API declarations  |
| flowu_cache.h    | New — flowu key/entry/cache/API + key helpers |

### Implementation (samples/fcache/src/)

| File     | Description                                          |
|----------|------------------------------------------------------|
| flow4.c  | Existing — unchanged                                 |
| flow6.c  | New — copy of flow4.c with key/entry/hash/cmp swapped |
| flowu.c  | New — based on flow6.c + family field handling       |

### Build

Add flow6.o and flowu.o to fcache/Makefile. Both compiled with -mavx2.

## API (uniform across all variants)

Each variant uses prefix `fc_{flow4,flow6,flowu}_`:

```
cache_init(cache, buckets, nb_bk, pool, max_entries, config)
cache_flush(cache)
cache_nb_entries(cache)
cache_stats(cache, out)

/* bulk (hot-path, pipelined) */
cache_find_bulk(cache, keys, nb_keys, now, results)
cache_findadd_bulk(cache, keys, nb_keys, now, results)
cache_add_bulk(cache, keys, nb_keys, now, results)
cache_del_bulk(cache, keys, nb_keys)
cache_del_idx_bulk(cache, idxs, nb_idxs)

/* single-key (convenience, calls bulk with n=1) */
cache_find(cache, key, now)       → entry_idx
cache_findadd(cache, key, now)    → entry_idx
cache_add(cache, key, now)        → entry_idx
cache_del(cache, key)
cache_del_idx(cache, entry_idx)   → 0/1

/* maintenance */
cache_maintain(cache, start_bk, bucket_count, now)
cache_maintain_step_ex(cache, start_bk, bucket_count, skip_threshold, now)
cache_maintain_step(cache, now, idle)

/* query (cold-path) */
cache_walk(cache, cb, arg)
```

## Variant-Specific Details

### Hash function

All variants use `rix_hash_hash_bytes_fast(key, sizeof(*key), mask)`.
Key sizes: flow4=20B, flow6=44B, flowu=44B.

### Key comparison

- flow4: `memcmp(a, b, 20)` — compiler inlines as 2x8B + 1x4B
- flow6/flowu: `memcmp(a, b, 44)` — compiler inlines as 5x8B + 1x4B or
  2x256b SIMD loads with AVX2

### SIMD binding

Same AVX2 fingerprint scan binding as flow4. Cuckoo bucket structure is
key-size-independent; SIMD scan operates on hash[] and idx[] arrays.

### Relief / Maintain

Identical logic to flow4. Entry management fields (cur_hash, last_ts,
free_link, slot) have the same semantics across all variants.

### flowu key helpers

```c
static inline struct fc_flowu_key fc_flowu_key_v4(src_ip, dst_ip, ...);
static inline struct fc_flowu_key fc_flowu_key_v6(src_ip, dst_ip, ...);
```

## Test Plan

### Functional tests (per variant)

1. Static assert: `sizeof(entry) == 64`
2. Insert → lookup hit → maintain expire → lookup miss
3. Pool full: fill_full counter incremented
4. Timeout boundary: entries at exact threshold
5. Relief: pressure-triggered bucket reclaim
6. Remove by index
7. flowu-specific: v4 and v6 keys coexist without collision

### Benchmarks (per variant)

Primary metric: **cy/pkt**

| Scenario   | Description                        |
|------------|------------------------------------|
| PKT_HIT    | 100% hit — lookup + touch          |
| PKT_MISS   | 100% miss — insert + relief        |
| PKT_STD    | 80% hit / 20% miss — typical load  |

Additional for flowu: v4-only, v6-only, mixed workloads.

Cross-variant comparison: flow4 vs flow6 to quantify 44B key cost.

## Phased Implementation

### Phase 1: flow6

1. flow6_cache.h
2. flow6.c (copy from flow4.c, swap key/entry/hash/cmp)
3. Makefile update
4. Tests + benchmarks

### Phase 2: flowu

1. flowu_cache.h (+ key helpers)
2. flowu.c (based on flow6.c + family)
3. Makefile update
4. Tests + benchmarks (including v4/v6 coexistence)

### Phase 3: Documentation

1. DESIGN.md / DESIGN_JP.md — update for 3-variant v2
2. README.md / README_JP.md — create for v2
3. Benchmark results

### Phase 4: Refactoring (future, out of scope)

Extract common logic to fc_body.h template header once all three variants
are stable.

## Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Entry size | 64B (1CL) all variants | Index-based: no user payload, maximize cache density |
| Template strategy | Independent .c files first | Understand differences before abstracting |
| SIMD backend | AVX2 compile-time fixed | Only AVX2 hardware available now; runtime dispatch later |
| API pattern | Uniform across variants | Predictable, easy to test and benchmark |
| Scope | Phase 1-3 | Phase 4 refactoring deferred to next cycle |
