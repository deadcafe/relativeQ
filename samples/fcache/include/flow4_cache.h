/**
 * @file flow4_cache.h
 * @brief IPv4 5-tuple flow cache built on cuckoo hashing (fcache).
 *
 * A high-performance, fixed-size flow cache for IPv4 5-tuple lookups.
 * Internally backed by a rix_hash cuckoo hash table with 4-stage
 * N-ahead pipelined batch lookup (hash_key -> scan_bk -> prefetch_node
 * -> cmp_key) to hide DRAM latency.
 *
 * Typical datapath usage:
 * @code
 *   // 1. Batch lookup
 *   uint16_t miss_idx[BATCH];
 *   unsigned misses = fc_flow4_cache_lookup_batch(
 *       &fc, keys, BATCH, now_tsc, results, miss_idx);
 *
 *   // 2. Process hits  (results[i].entry_idx != 0)
 *   // 3. Fill misses   (allocate new entries for cache misses)
 *   fc_flow4_cache_fill_miss_batch(
 *       &fc, keys, miss_idx, misses, now_tsc, results);
 *
 *   // 4. Periodic maintenance  (expire stale entries)
 *   fc_flow4_cache_maintain(&fc, cursor, 64, now_tsc);
 * @endcode
 *
 * Entries are 64-byte cache-line aligned.  The cache uses TSC-based
 * timestamps for expiration with adaptive timeout scaling based on
 * fill level.
 */

/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW4_CACHE_H_
#define _FLOW4_CACHE_H_

#include <stdint.h>
#include <rix/rix_hash.h>
#include <rix/rix_queue.h>

/** @brief Cache-line size used for entry alignment. */
#define FC_CACHE_LINE_SIZE 64u

#ifndef FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS
/** @brief Default insert-pressure threshold (empty slots per bucket). */
#define FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS 1u
#endif

/** @name Pipeline geometry (compile-time tuning)
 *
 *  Control the N-ahead pipeline depth used by lookup_batch.
 *  - @c FLOW_CACHE_LOOKUP_STEP_KEYS  keys processed per pipeline stage.
 *  - @c FLOW_CACHE_LOOKUP_AHEAD_STEPS  number of stages ahead to prefetch.
 *  - @c FLOW_CACHE_LOOKUP_AHEAD_KEYS  total look-ahead window (derived).
 *
 *  Larger values improve throughput on high-latency memory but increase
 *  stack usage.  The defaults (16 x 8 = 128 keys) work well for typical
 *  DDR4/DDR5 latencies.
 * @{ */
#ifndef FLOW_CACHE_LOOKUP_STEP_KEYS
#define FLOW_CACHE_LOOKUP_STEP_KEYS   16u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_STEPS
#define FLOW_CACHE_LOOKUP_AHEAD_STEPS 8u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_KEYS
#define FLOW_CACHE_LOOKUP_AHEAD_KEYS \
    (FLOW_CACHE_LOOKUP_STEP_KEYS * FLOW_CACHE_LOOKUP_AHEAD_STEPS)
#endif
/** @} */

/**
 * @brief IPv4 5-tuple lookup key (24 bytes).
 *
 * The @c zero field must always be 0.  It pads the key to exactly 24 bytes
 * (3 x 8 bytes), enabling an optimised CRC32C hash path on x86-64.
 */
struct fc_flow4_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  pad[3];
    uint32_t vrfid;
    uint32_t zero;              /**< Must be 0; makes key 24B (3 x 8B). */
};

/**
 * @brief Per-key result returned by lookup / fill operations.
 *
 * @c entry_idx is a 1-origin pool index.  Zero means the key was not
 * found (miss) or the cache was full.  Use @c RIX_PTR_FROM_IDX(pool, idx)
 * to obtain the entry pointer.
 */
struct fc_flow4_result {
    uint32_t entry_idx; /**< 1-origin pool index; 0 = miss / full. */
};

/**
 * @brief Cache entry, one per cache line (64 bytes).
 *
 * Managed internally; callers access entries through pool + entry_idx.
 */
struct fc_flow4_entry {
    struct fc_flow4_key   key;          /**< 24B lookup key. */
    uint32_t               cur_hash;     /**< Hash stored for O(1) remove. */
    uint64_t               last_ts;      /**< Last-access TSC; 0 = free. */
    RIX_SLIST_ENTRY(struct fc_flow4_entry) free_link;
    uint16_t               slot;         /**< Slot within current bucket. */
    uint16_t               reserved1;
    uint8_t                reserved0[16];
} __attribute__((aligned(FC_CACHE_LINE_SIZE)));

RIX_STATIC_ASSERT(sizeof(struct fc_flow4_entry) == FC_CACHE_LINE_SIZE,
                  "fc_flow4_entry must be 64 bytes");

RIX_HASH_HEAD(fc_flow4_ht);
RIX_SLIST_HEAD(fc_flow4_free_head, fc_flow4_entry);

/**
 * @brief Initialization parameters.
 *
 * Pass NULL to fc_flow4_cache_init() to use built-in defaults
 * (timeout_tsc = 1000000, pressure = FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS).
 */
struct fc_flow4_config {
    uint64_t timeout_tsc;           /**< Entry lifetime in TSC ticks.
                                         Adaptive scaling shrinks this
                                         as the cache fills. */
    unsigned pressure_empty_slots;  /**< Insert-pressure threshold.
                                         Relief eviction triggers when a
                                         target bucket has <= this many
                                         empty slots. */
};

/**
 * @brief Cumulative counters (monotonically increasing).
 *
 * Retrieve with fc_flow4_cache_stats().  All fields are updated
 * atomically per call (single-writer assumed).
 */
struct fc_flow4_stats {
    uint64_t lookups;               /**< Keys submitted to lookup_batch. */
    uint64_t hits;                  /**< Keys found in lookup_batch. */
    uint64_t misses;                /**< Keys not found in lookup_batch. */
    uint64_t fills;                 /**< Entries inserted by fill_miss_batch. */
    uint64_t fill_full;             /**< Inserts failed (cache full). */
    uint64_t relief_calls;          /**< Times insert-relief was invoked. */
    uint64_t relief_bucket_checks;  /**< Buckets scanned by relief. */
    uint64_t relief_evictions;      /**< Entries evicted by relief (total). */
    uint64_t relief_bk0_evictions;  /**< Relief evictions from primary bucket. */
    uint64_t relief_bk1_evictions;  /**< Relief evictions from alternate bucket. */
    uint64_t maint_calls;           /**< Times maintain was called. */
    uint64_t maint_bucket_checks;   /**< Buckets scanned by maintain. */
    uint64_t maint_evictions;       /**< Entries evicted by maintain. */
};

/**
 * @brief Flow cache instance.
 *
 * All storage (buckets, pool) is caller-provided; the cache itself
 * contains only metadata and pointers.  This allows placement in
 * shared memory with no internal heap allocation.
 */
struct fc_flow4_cache {
    /* --- CL0: lookup / fill hot path --- */
    struct rix_hash_bucket_s  *buckets;
    struct fc_flow4_entry    *pool;
    struct fc_flow4_ht        ht_head;
    uint64_t                   timeout_tsc;
    uint64_t                   eff_timeout_tsc;
    uint64_t                   timeout_min_tsc;
    unsigned                   nb_bk;
    unsigned                   max_entries;
    unsigned                   total_slots;
    unsigned                   pressure_empty_slots;
    /* --- CL1 --- */
    unsigned                   timeout_lo_entries;
    unsigned                   timeout_hi_entries;
    unsigned                   relief_mid_entries;
    unsigned                   relief_hi_entries;
    struct fc_flow4_free_head free_head;
    struct fc_flow4_stats     stats;
};

/**
 * @brief Initialize a flow cache.
 *
 * Zeroes all buckets and pool entries, builds the free list, and sets
 * internal thresholds derived from @p nb_bk.
 *
 * @param[out] fc          Cache instance to initialize.
 * @param[in]  buckets     Bucket array, @p nb_bk elements.
 *                         Must be aligned to @c sizeof(struct rix_hash_bucket_s).
 * @param[in]  nb_bk       Number of buckets (must be a power of 2).
 * @param[in]  pool        Entry pool, @p max_entries elements.
 *                         Must be 64-byte aligned.
 * @param[in]  max_entries Pool capacity (number of entries).
 * @param[in]  cfg         Configuration, or NULL for defaults.
 */
void fc_flow4_cache_init(struct fc_flow4_cache *fc,
                          struct rix_hash_bucket_s *buckets,
                          unsigned nb_bk,
                          struct fc_flow4_entry *pool,
                          unsigned max_entries,
                          const struct fc_flow4_config *cfg);

/**
 * @brief Remove all entries and reset the cache to its initial state.
 *
 * Bucket and pool memory is reused (not freed).  Stats are preserved.
 *
 * @param[in,out] fc  Cache instance.
 */
void fc_flow4_cache_flush(struct fc_flow4_cache *fc);

/**
 * @brief Return the number of live entries in the cache.
 *
 * @param[in] fc  Cache instance.
 * @return Number of entries currently stored.
 */
unsigned fc_flow4_cache_nb_entries(const struct fc_flow4_cache *fc);

/**
 * @brief Pipelined batch lookup.
 *
 * Looks up @p nb_keys keys using a 4-stage N-ahead pipeline to hide
 * memory latency.  For each hit the corresponding entry's @c last_ts
 * is updated to @p now.
 *
 * @param[in,out] fc        Cache instance.
 * @param[in]     keys      Array of @p nb_keys lookup keys.
 * @param[in]     nb_keys   Number of keys (may exceed the pipeline window).
 * @param[in]     now       Current TSC timestamp.
 * @param[out]    results   Per-key results (@c entry_idx != 0 on hit).
 * @param[out]    miss_idx  Indices into @p keys for missed keys (may be NULL
 *                          if the caller does not need miss positions).
 * @return Number of misses (length of @p miss_idx).
 */
unsigned fc_flow4_cache_lookup_batch(struct fc_flow4_cache *fc,
                                      const struct fc_flow4_key *keys,
                                      unsigned nb_keys,
                                      uint64_t now,
                                      struct fc_flow4_result *results,
                                      uint16_t *miss_idx);

/**
 * @brief Insert entries for previously missed keys.
 *
 * Call after lookup_batch with the returned @p miss_idx array.
 * For each miss, allocates an entry from the free list and inserts it.
 * If the target buckets are under pressure, stale entries are evicted
 * first (insert-relief).  Duplicate keys that were inserted by a
 * concurrent miss are detected and deduplicated.
 *
 * @param[in,out] fc          Cache instance.
 * @param[in]     keys        Same key array passed to lookup_batch.
 * @param[in]     miss_idx    Miss indices returned by lookup_batch.
 * @param[in]     miss_count  Length of @p miss_idx.
 * @param[in]     now         Current TSC timestamp.
 * @param[in,out] results     Same results array; updated for filled keys
 *                            (@c entry_idx set on success, 0 if full).
 * @return Number of entries actually inserted.
 */
unsigned fc_flow4_cache_fill_miss_batch(struct fc_flow4_cache *fc,
                                         const struct fc_flow4_key *keys,
                                         const uint16_t *miss_idx,
                                         unsigned miss_count,
                                         uint64_t now,
                                         struct fc_flow4_result *results);

/**
 * @brief Expire stale entries from a range of buckets.
 *
 * Scans @p bucket_count consecutive buckets starting from @p start_bk
 * (wrapping at the table boundary) and removes any entry whose
 * @c last_ts is older than the effective timeout.  The effective
 * timeout is automatically scaled down as the cache fills.
 *
 * Designed to be called periodically with a rotating cursor, e.g.:
 * @code
 *   evicted = fc_flow4_cache_maintain(&fc, cursor, 64, now);
 *   cursor = (cursor + 64) & (nb_bk - 1);
 * @endcode
 *
 * @param[in,out] fc            Cache instance.
 * @param[in]     start_bk      First bucket index (masked internally).
 * @param[in]     bucket_count  Number of buckets to scan.
 * @param[in]     now           Current TSC timestamp.
 * @return Number of entries evicted.
 */
unsigned fc_flow4_cache_maintain(struct fc_flow4_cache *fc,
                                  unsigned start_bk,
                                  unsigned bucket_count,
                                  uint64_t now);

/**
 * @brief Remove a single entry by pool index.
 *
 * @param[in,out] fc         Cache instance.
 * @param[in]     entry_idx  1-origin pool index (as returned in
 *                           fc_flow4_result::entry_idx).
 * @return 1 if the entry was found and removed, 0 otherwise.
 */
int fc_flow4_cache_remove_idx(struct fc_flow4_cache *fc, uint32_t entry_idx);

/**
 * @brief Snapshot cumulative statistics.
 *
 * Copies the internal counters into @p out.  The caller can diff
 * successive snapshots to obtain per-interval rates.
 *
 * @param[in]  fc   Cache instance.
 * @param[out] out  Destination for the snapshot.
 */
void fc_flow4_cache_stats(const struct fc_flow4_cache *fc,
                           struct fc_flow4_stats *out);

#endif /* _FLOW4_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
