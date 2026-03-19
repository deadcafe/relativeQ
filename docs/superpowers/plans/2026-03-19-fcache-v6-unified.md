# fcache IPv6 / Unified Extension Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add flow6 (IPv6) and flowu (Unified IPv4/IPv6) variants to fcache, with tests, benchmarks, and documentation.

**Architecture:** Copy flow4 implementation for each variant, replacing key/entry/hash/cmp. All entries are 64 bytes (1 cache line). AVX2 compile-time fixed. Independent `.c` files per variant.

**Tech Stack:** C11, librix (rix_hash cuckoo), AVX2 SIMD, GNU Make

**Spec:** `docs/superpowers/specs/2026-03-19-fcache-v6-unified-design.md`

---

## Chunk 1: flow6 — Header, Implementation, Build

### Task 1: flow6 header

**Files:**
- Create: `samples/fcache/include/flow6_cache.h`
- Modify: `samples/fcache/include/flow_cache.h`

- [ ] **Step 1: Create flow6_cache.h**

Copy structure from `samples/fcache/include/flow4_cache.h`. Replace all `flow4` → `flow6`, `fc_flow4` → `fc_flow6`. **Important:** Remove the `FLOW_CACHE_LOOKUP_STEP_KEYS` / `FLOW_CACHE_LOOKUP_AHEAD_STEPS` / `FLOW_CACHE_LOOKUP_AHEAD_KEYS` macro definitions — these are already defined in `flow_cache.h` (the umbrella) and `flow4_cache.h`. Do not duplicate them. Change key struct:

```c
struct fc_flow6_key {
    uint8_t  src_ip[16];
    uint8_t  dst_ip[16];
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
} __attribute__((packed));  /* 44B */
```

Entry struct — replace `reserved0[24]` with the larger key; no reserved0 needed:

```c
struct fc_flow6_entry {
    struct fc_flow6_key   key;          /* 44B */
    uint32_t               cur_hash;     /*  4B */
    uint64_t               last_ts;      /*  8B */
    RIX_SLIST_ENTRY(struct fc_flow6_entry) free_link; /* 4B */
    uint16_t               slot;         /*  2B */
    uint16_t               reserved1;    /*  2B */
} __attribute__((aligned(FC_CACHE_LINE_SIZE)));
```

Static assert: `sizeof(struct fc_flow6_entry) == FC_CACHE_LINE_SIZE`.

All API declarations: rename `fc_flow4_` → `fc_flow6_` with same signatures.

- [ ] **Step 2: Update flow_cache.h umbrella**

Add `#include "flow6_cache.h"` after the existing `#include "flow4_cache.h"` in `samples/fcache/include/flow_cache.h`.

- [ ] **Step 3: Compile header to check syntax**

Run: `cd samples/fcache && echo '#include "flow6_cache.h"' | gcc -std=gnu11 -I../../include -Iinclude -mavx2 -msse4.2 -fsyntax-only -xc -`

Expected: no errors.

- [ ] **Step 4: Commit**

```bash
git add samples/fcache/include/flow6_cache.h samples/fcache/include/flow_cache.h
git commit -m "fcache: add flow6 header with 64B IPv6 entry"
```

### Task 2: flow6 implementation

**Files:**
- Create: `samples/fcache/src/flow6.c`
- Modify: `samples/fcache/Makefile`

- [ ] **Step 1: Create flow6.c**

Copy `samples/fcache/src/flow4.c` to `flow6.c`. Replace all occurrences:
- `fc_flow4` → `fc_flow6`
- `flow4` → `flow6`
- `FC_FLOW4` → `FC_FLOW6`

Key change in hash function — key size is now `sizeof(struct fc_flow6_key)` (44B):

```c
static inline union rix_hash_hash_u
fc_flow6_hash_fn(const struct fc_flow6_key *key, uint32_t mask)
{
    return rix_hash_hash_bytes_fast(key, sizeof(*key), mask);
}

int
fc_flow6_cmp(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(struct fc_flow6_key));
}
```

The `#include` at the top changes to `#include "flow6_cache.h"`. The `RIX_HASH_GENERATE_STATIC_SLOT_EX` invocation is renamed to use `fc_flow6_ht`, `fc_flow6_entry`, `fc_flow6_cmp`, `fc_flow6_hash_fn` — this wires the hash table code generation to the flow6-specific functions.

Everything else (relief, maintain, lookup_batch, fill_miss_batch) is a mechanical rename.

- [ ] **Step 2: Update Makefile**

In `samples/fcache/Makefile`:
- Add `$(INCDIR)/flow6_cache.h` to `PUB_HDRS`
- Add `$(LIBDIR)/flow6.o` to `OBJS`
- Add build rule:

```makefile
$(LIBDIR)/flow6.o: $(SRCDIR)/flow6.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -c -o $@ $<
```

- [ ] **Step 3: Build library**

Run: `cd samples/fcache && make clean && make static`

Expected: builds `lib/libfcache.a` containing flow4.o and flow6.o.

- [ ] **Step 4: Commit**

```bash
git add samples/fcache/src/flow6.c samples/fcache/Makefile
git commit -m "fcache: add flow6 implementation (copy from flow4, 44B key)"
```

### Task 3: flow6 functional tests

**Files:**
- Modify: `tests/fcache/test_flow_cache.c`

- [ ] **Step 1: Add flow6 include**

Add `#include "../../samples/fcache/include/flow6_cache.h"` at top.

- [ ] **Step 2: Add flow6 make_key helper**

```c
static struct fc_flow6_key
make_key6(unsigned i)
{
    struct fc_flow6_key k;
    uint32_t v = 0x20010db8u + i;

    memset(&k, 0, sizeof(k));
    memcpy(k.src_ip, &v, 4);
    v += 0x1000u;
    memcpy(k.dst_ip, &v, 4);
    k.src_port = (uint16_t)(1000u + i);
    k.dst_port = (uint16_t)(2000u + i);
    k.proto = 6u;
    k.vrfid = 1u;
    return k;
}
```

- [ ] **Step 3: Add flow6 count_hits helper**

```c
static unsigned
count_hits6(const struct fc_flow6_result *results, unsigned n)
{
    unsigned hits = 0u;
    for (unsigned i = 0; i < n; i++)
        if (results[i].entry_idx != 0u)
            hits++;
    return hits;
}
```

- [ ] **Step 4: Add flow6 test functions**

Port ALL six flow4 tests with `flow4` → `flow6` renames and `make_key` → `make_key6`:
- `test_flow6_lookup_fill_remove` (from `test_lookup_fill_remove`)
- `test_flow6_pressure_relief` (from `test_pressure_relief_on_fill_miss`)
- `test_flow6_fill_miss_full_without_relief` (from `test_fill_miss_full_without_relief`)
- `test_flow6_duplicate_miss_batch` (from `test_duplicate_miss_batch`)
- `test_flow6_flush_and_invalid_remove` (from `test_flush_and_invalid_remove`)
- `test_flow6_maintenance` (from `test_maintenance_bucket_count`)

- [ ] **Step 5: Add flow6 timeout boundary test**

New test not present in flow4 (covers spec item 4):

```c
static void
test_flow6_timeout_boundary(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 16u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc_flow6_entry pool[MAX_ENTRIES];
    struct fc_flow6_cache fc;
    struct fc_flow6_config cfg = { .timeout_tsc = 100u, .pressure_empty_slots = 1u };
    struct fc_flow6_key keys[2];
    struct fc_flow6_result results[2];
    uint16_t miss_idx[2];

    printf("[T] fc flow6 timeout boundary\n");
    fc_flow6_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, &cfg);
    keys[0] = make_key6(0);
    keys[1] = make_key6(1);

    /* Insert at t=1000 */
    fc_flow6_cache_lookup_batch(&fc, keys, 2u, 1000u, results, miss_idx);
    fc_flow6_cache_fill_miss_batch(&fc, keys, miss_idx, 2u, 1000u, results);

    /* At t=1099 (just before timeout): maintain should NOT evict */
    if (fc_flow6_cache_maintain(&fc, 0u, NB_BK, 1099u) != 0u)
        FAIL("should not evict before timeout boundary");
    if (fc_flow6_cache_nb_entries(&fc) != 2u)
        FAIL("entries should remain at timeout boundary - 1");

    /* At t=1101 (just after timeout): maintain SHOULD evict */
    if (fc_flow6_cache_maintain(&fc, 0u, NB_BK, 1101u) == 0u)
        FAIL("should evict after timeout boundary");
}
```

- [ ] **Step 6: Add flow6 test calls to main()**

Add after the existing flow4 test calls:
```c
    test_flow6_lookup_fill_remove();
    test_flow6_pressure_relief();
    test_flow6_fill_miss_full_without_relief();
    test_flow6_duplicate_miss_batch();
    test_flow6_flush_and_invalid_remove();
    test_flow6_maintenance();
    test_flow6_timeout_boundary();
```

Update success message: `"ALL FCACHE TESTS PASSED (flow4 + flow6)\n"`

- [ ] **Step 7: Build and run tests**

Run:
```bash
cd tests/fcache && make clean && make fc_test && ./fc_test
```

Expected: all tests pass, including new flow6 tests.

- [ ] **Step 8: Commit**

```bash
git add tests/fcache/test_flow_cache.c
git commit -m "tests: add flow6 functional tests for fcache"
```

---

## Chunk 2: flowu — Header, Implementation, Tests

### Task 4: flowu header

**Files:**
- Create: `samples/fcache/include/flowu_cache.h`
- Modify: `samples/fcache/include/flow_cache.h`

- [ ] **Step 1: Create flowu_cache.h**

Key struct (44B, same size as flow6, different field order):

```c
#define FC_FLOW_FAMILY_IPV4  4
#define FC_FLOW_FAMILY_IPV6  6

struct fc_flowu_key {
    uint8_t  family;        /*  1B */
    uint8_t  proto;         /*  1B */
    uint16_t src_port;      /*  2B */
    uint16_t dst_port;      /*  2B */
    uint16_t pad;           /*  2B */
    uint32_t vrfid;         /*  4B */
    union {                 /* 32B */
        struct { uint32_t src; uint32_t dst; uint8_t _pad[24]; } v4;
        struct { uint8_t src[16]; uint8_t dst[16]; } v6;
    } addr;
} __attribute__((packed));  /* 44B */
```

Entry struct — identical layout to flow6_entry with flowu types.

Key construction helpers (static inline in header):

```c
static inline struct fc_flowu_key
fc_flowu_key_v4(uint32_t src_ip, uint32_t dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t proto, uint32_t vrfid)
{
    struct fc_flowu_key k;
    memset(&k, 0, sizeof(k));
    k.family   = FC_FLOW_FAMILY_IPV4;
    k.proto    = proto;
    k.src_port = src_port;
    k.dst_port = dst_port;
    k.vrfid    = vrfid;
    k.addr.v4.src = src_ip;
    k.addr.v4.dst = dst_ip;
    return k;
}

static inline struct fc_flowu_key
fc_flowu_key_v6(const uint8_t *src_ip, const uint8_t *dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  uint8_t proto, uint32_t vrfid)
{
    struct fc_flowu_key k;
    memset(&k, 0, sizeof(k));
    k.family   = FC_FLOW_FAMILY_IPV6;
    k.proto    = proto;
    k.src_port = src_port;
    k.dst_port = dst_port;
    k.vrfid    = vrfid;
    memcpy(k.addr.v6.src, src_ip, 16);
    memcpy(k.addr.v6.dst, dst_ip, 16);
    return k;
}
```

All API declarations: `fc_flowu_` prefix.

- [ ] **Step 2: Update flow_cache.h umbrella**

Add `#include "flowu_cache.h"` after flow6 include.

- [ ] **Step 3: Compile header to check syntax**

Run: `cd samples/fcache && echo '#include "flowu_cache.h"' | gcc -std=gnu11 -I../../include -Iinclude -mavx2 -msse4.2 -fsyntax-only -xc -`

- [ ] **Step 4: Commit**

```bash
git add samples/fcache/include/flowu_cache.h samples/fcache/include/flow_cache.h
git commit -m "fcache: add flowu header with unified IPv4/IPv6 key"
```

### Task 5: flowu implementation

**Files:**
- Create: `samples/fcache/src/flowu.c`
- Modify: `samples/fcache/Makefile`

- [ ] **Step 1: Create flowu.c**

Copy `samples/fcache/src/flow6.c` (created in Task 2) to `flowu.c`. Replace:
- `fc_flow6` → `fc_flowu`
- `flow6` → `flowu`
- `FC_FLOW6` → `FC_FLOWU`

Hash and cmp remain the same pattern (44B key via memcmp/hash_bytes_fast).

- [ ] **Step 2: Update Makefile**

Add `$(INCDIR)/flowu_cache.h` to `PUB_HDRS`, `$(LIBDIR)/flowu.o` to `OBJS`, and build rule:

```makefile
$(LIBDIR)/flowu.o: $(SRCDIR)/flowu.c | $(LIBDIR)
	$(CC) $(CFLAGS_BASE) -mavx2 -msse4.2 -c -o $@ $<
```

- [ ] **Step 3: Build library**

Run: `cd samples/fcache && make clean && make static`

Expected: builds with flow4.o, flow6.o, flowu.o.

- [ ] **Step 4: Commit**

```bash
git add samples/fcache/src/flowu.c samples/fcache/Makefile
git commit -m "fcache: add flowu implementation (unified IPv4/IPv6)"
```

### Task 6: flowu functional tests

**Files:**
- Modify: `tests/fcache/test_flow_cache.c`

- [ ] **Step 1: Add flowu include and helpers**

Include `flowu_cache.h`. Add `make_keyu_v4()` and `make_keyu_v6()` helpers using `fc_flowu_key_v4()` / `fc_flowu_key_v6()`.

```c
static struct fc_flowu_key
make_keyu_v4(unsigned i)
{
    return fc_flowu_key_v4(
        0x0a000001u + i, 0x0a100001u + i,
        (uint16_t)(1000u + i), (uint16_t)(2000u + i),
        17u, 1u);
}

static struct fc_flowu_key
make_keyu_v6(unsigned i)
{
    uint8_t src[16] = {0x20, 0x01, 0x0d, 0xb8};
    uint8_t dst[16] = {0x20, 0x01, 0x0d, 0xb8};
    uint32_t v = i;

    memcpy(src + 12, &v, 4);
    v += 0x1000u;
    memcpy(dst + 12, &v, 4);
    return fc_flowu_key_v6(
        src, dst,
        (uint16_t)(1000u + i), (uint16_t)(2000u + i),
        6u, 1u);
}
```

- [ ] **Step 2: Add flowu basic tests**

Port ALL six base tests plus timeout boundary from flow6 tests:
- `test_flowu_lookup_fill_remove`
- `test_flowu_pressure_relief`
- `test_flowu_fill_miss_full_without_relief`
- `test_flowu_duplicate_miss_batch`
- `test_flowu_flush_and_invalid_remove`
- `test_flowu_maintenance`
- `test_flowu_timeout_boundary`

Use `make_keyu_v4()` for these (basic functionality, same as flow4/flow6 tests).

- [ ] **Step 3: Add flowu v4/v6 coexistence test**

```c
static void
test_flowu_v4_v6_coexist(void)
{
    enum { NB_BK = 8u, MAX_ENTRIES = 32u, NB_V4 = 4u, NB_V6 = 4u };
    struct rix_hash_bucket_s buckets[NB_BK];
    struct fc_flowu_entry pool[MAX_ENTRIES];
    struct fc_flowu_cache fc;
    struct fc_flowu_key keys[NB_V4 + NB_V6];
    struct fc_flowu_result results[NB_V4 + NB_V6];
    uint16_t miss_idx[NB_V4 + NB_V6];
    unsigned miss_count;
    unsigned total = NB_V4 + NB_V6;

    printf("[T] fc flowu v4/v6 coexistence\n");
    fc_flowu_cache_init(&fc, buckets, NB_BK, pool, MAX_ENTRIES, NULL);

    for (unsigned i = 0; i < NB_V4; i++)
        keys[i] = make_keyu_v4(i);
    for (unsigned i = 0; i < NB_V6; i++)
        keys[NB_V4 + i] = make_keyu_v6(i);

    /* All miss initially */
    miss_count = fc_flowu_cache_lookup_batch(&fc, keys, total, 1u,
                                               results, miss_idx);
    if (miss_count != total)
        FAILF("coexist initial miss_count=%u expected %u", miss_count, total);

    /* Fill all */
    if (fc_flowu_cache_fill_miss_batch(&fc, keys, miss_idx, miss_count,
                                         10u, results) != total)
        FAIL("coexist fill failed");

    /* All hit */
    miss_count = fc_flowu_cache_lookup_batch(&fc, keys, total, 20u,
                                               results, miss_idx);
    if (miss_count != 0u)
        FAILF("coexist post-fill miss_count=%u expected 0", miss_count);

    /* Verify v4 and v6 got distinct entries */
    for (unsigned i = 0; i < total; i++) {
        if (results[i].entry_idx == 0u)
            FAILF("coexist result[%u] entry_idx is 0", i);
        for (unsigned j = i + 1; j < total; j++) {
            if (results[i].entry_idx == results[j].entry_idx)
                FAILF("coexist result[%u] == result[%u] = %u",
                      i, j, results[i].entry_idx);
        }
    }

    /* Remove a v4 entry; v6 entries should still hit */
    if (!fc_flowu_cache_remove_idx(&fc, results[0].entry_idx))
        FAIL("coexist remove v4 entry failed");
    miss_count = fc_flowu_cache_lookup_batch(&fc, &keys[NB_V4], NB_V6, 30u,
                                               &results[NB_V4], miss_idx);
    if (miss_count != 0u)
        FAIL("coexist v6 entries should still hit after v4 remove");
}
```

- [ ] **Step 4: Add flowu test calls to main()**

```c
    test_flowu_lookup_fill_remove();
    test_flowu_pressure_relief();
    test_flowu_fill_miss_full_without_relief();
    test_flowu_duplicate_miss_batch();
    test_flowu_flush_and_invalid_remove();
    test_flowu_maintenance();
    test_flowu_timeout_boundary();
    test_flowu_v4_v6_coexist();
```

Update success message: `"ALL FCACHE TESTS PASSED (flow4 + flow6 + flowu)\n"`

- [ ] **Step 5: Build and run tests**

Run:
```bash
cd tests/fcache && make clean && make fc_test && ./fc_test
```

Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add tests/fcache/test_flow_cache.c
git commit -m "tests: add flowu functional tests including v4/v6 coexistence"
```

---

## Chunk 3: Benchmarks

### Task 7: flow6 and flowu benchmarks

**Files:**
- Modify: `tests/fcache/bench_flow_cache.c`
- Modify: `samples/test/run_fc_bench_matrix.sh`

Note: `tests/fcache/Makefile` does not need modification — the bench binary links against `$(LIBFC)` (the full `.a` archive), which already includes the new flow6.o and flowu.o from library Makefile changes in Tasks 2/5.

This is a large file (4790 lines). The approach is to add flow6 and flowu benchmark functions following the existing flow4 patterns.

- [ ] **Step 1: Add flow6/flowu includes to bench**

Add includes for `flow6_cache.h` and `flowu_cache.h` in `tests/fcache/bench_flow_cache.c`.

- [ ] **Step 2: Add flow6 key maker and cache context**

Add `bench_make_key6()` (IPv6 version) and `struct new6_cache_ctx` (with `fc_flow6_cache`, `fc_flow6_entry *pool`, `rix_hash_bucket_s *buckets`).

Add corresponding `new6_prefill`, `bench_new6_stats_snapshot`, `bench_new6_stats_diff` functions following the flow4 pattern.

- [ ] **Step 3: Add flow6 benchmark functions**

Port the core flow4 benchmarks to flow6:
- `bench_new6_hit` — lookup batch all-hit
- `bench_new6_miss_fill` — lookup + fill all-miss
- `bench_new6_pkt_hit` — packet loop hit-only
- `bench_new6_pkt_miss_fill` — packet loop miss-only
- `bench_new6_pkt_mixed` — packet loop mixed

Each is a mechanical port: replace `fc_flow4_` → `fc_flow6_`, `flow4` → `flow6`, key maker → `bench_make_key6`.

- [ ] **Step 4: Add flowu key makers and cache context**

Add `bench_make_keyu_v4()`, `bench_make_keyu_v6()` using `fc_flowu_key_v4()` / `fc_flowu_key_v6()`.

Add `struct newu_cache_ctx` and corresponding stats/prefill helpers.

- [ ] **Step 5: Add flowu benchmark functions**

Same pattern as flow6. Additionally, for mixed v4/v6 workloads, add a key maker that alternates:

```c
static inline struct fc_flowu_key
bench_make_keyu_mixed(unsigned i)
{
    if (i & 1u)
        return bench_make_keyu_v6(i >> 1);
    return bench_make_keyu_v4(i >> 1);
}
```

- [ ] **Step 6: Add command-line variant selection**

Extend the main() argument parsing to accept `flow6`, `flowu`, or `all` variants, running the appropriate set of benchmarks. Keep the default as `flow4` for backwards compatibility.

- [ ] **Step 7: Update bench matrix script**

In `samples/test/run_fc_bench_matrix.sh`:
- Add `run_flow6_matrix()` function (same structure as flow4)
- Add `run_flowu_matrix()` function (including mixed v4/v6 cases)
- Add `flow6` and `flowu` to the case statement
- Update usage text

- [ ] **Step 8: Build and run quick benchmark**

Run:
```bash
cd tests/fcache && make clean && make fc_bench
./fc_bench flow6 rate_fc_only 65536 75 100 500000
./fc_bench flowu rate_fc_only 65536 75 100 500000
```

Expected: benchmark runs and produces cy/pkt output.

- [ ] **Step 9: Commit**

```bash
git add tests/fcache/bench_flow_cache.c samples/test/run_fc_bench_matrix.sh
git commit -m "bench: add flow6 and flowu benchmarks with matrix support"
```

---

## Chunk 4: Documentation

### Task 8: Update DESIGN docs

**Files:**
- Modify: `samples/DESIGN.md`
- Modify: `samples/DESIGN_JP.md`

- [ ] **Step 1: Read current DESIGN.md structure**

Read the existing document to understand section structure and where v2 content should be added.

- [ ] **Step 2: Update DESIGN.md**

Add/update sections for:
- v2 overview: 3 variants (flow4/flow6/flowu), 64B single-CL entries
- Entry structure tables for all 3 variants
- Key comparison details (20B vs 44B, SIMD implications)
- flowu key design (family field, zero-padding, key helpers)
- API reference (uniform API pattern)
- Benchmark results (fill in after running benchmarks)

- [ ] **Step 3: Update DESIGN_JP.md**

Mirror the English updates in Japanese.

- [ ] **Step 4: Commit**

```bash
git add samples/DESIGN.md samples/DESIGN_JP.md
git commit -m "docs: update DESIGN for fcache flow6/flowu variants"
```

### Task 9: Create README docs

**Files:**
- Create: `samples/README.md`
- Create: `samples/README_JP.md`

- [ ] **Step 1: Create README.md**

Overview of samples/:
- Purpose: production-quality flow cache demonstration using librix
- v2 (fcache): active development, 3 variants
- Quick start: build, test, benchmark commands
- File structure overview
- API quick reference

- [ ] **Step 2: Create README_JP.md**

Japanese version of README.md.

- [ ] **Step 3: Commit**

```bash
git add samples/README.md samples/README_JP.md
git commit -m "docs: add samples README for fcache"
```
