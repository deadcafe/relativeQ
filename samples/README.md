# Flow Cache Samples

Production-quality flow cache implementations using librix index-based
data structures (`rix_hash.h`).  Designed for high-performance packet
processing with pipeline-style batch lookup to hide DRAM latency.

## Two Versions

| | fcache (v1) | fcache (v2) |
|---|---|---|
| Entry size | 128B (2 CL) | 64B (1 CL) |
| User payload | 64B in CL1 | None (caller uses entry_idx) |
| Variants | flow4, flow6, flowu | flow4, flow6, flowu |
| SIMD dispatch | Runtime (gen/sse/avx2/avx512) | AVX2 direct bind |
| Expire | Global walk + threshold | Local relief + bucket-budget maintenance |

v2 is the active development target.  v1 remains for comparison.

## Quick Start

```sh
# Build everything
make -C samples/fcache static
make -C samples/fcache static

# Run v2 tests
make -C tests/fcache fc_test && tests/fcache/fc_test

# Run v2 variant benchmark (flow4 vs flow6 vs flowu)
make -C tests/fcache fc_vbench && taskset -c 0 tests/fcache/fc_vbench

# Run v1-vs-v2 comparison benchmark
make -C tests/fcache fc_bench && taskset -c 0 tests/fcache/fc_bench
```

## File Structure

```
samples/
  fcache/          v1 library (128B entries)
  fcache/         v2 library (64B entries)
  test/            v1 tests + benchmarks

tests/fcache/     v2 tests + benchmarks
  test_flow_cache2.c       correctness tests (all 3 variants)
  bench_flow_cache2.c      v1-vs-v2 comparison bench
  bench_fc_variants.c     v2 variant comparison bench
```

## v2 Variants

- **flow4**: IPv4 5-tuple + vrfid (20B key)
- **flow6**: IPv6 5-tuple + vrfid (44B key)
- **flowu**: Unified IPv4/IPv6 with family field (44B key)

All entries are 64 bytes (single cache line).  See `DESIGN.md` for
entry layouts and benchmark data.

## API (v2)

Each variant exposes the same function set (substitute `flow4` / `flow6` /
`flowu` for `PREFIX`):

```c
fc_PREFIX_cache_init()             // initialize
fc_PREFIX_cache_flush()            // return all entries to free list
fc_PREFIX_cache_lookup_batch()     // pipelined batch lookup
fc_PREFIX_cache_fill_miss_batch()  // insert missed keys
fc_PREFIX_cache_maintain()         // idle/background reclaim
fc_PREFIX_cache_remove_idx()       // remove by index
fc_PREFIX_cache_nb_entries()       // current count
fc_PREFIX_cache_stats()            // counters snapshot
```

## Dependencies

- librix headers: `rix/rix_hash.h`, `rix/rix_queue.h`, `rix/rix_defs.h`
- Compiler: GCC or Clang with `-mavx2 -msse4.2`
- No external libraries
