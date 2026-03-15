/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_CACHE_H_
#define _FLOW_CACHE_H_

/*
 * flow_cache.h - Umbrella header for the flow cache library.
 *
 * Include this single header to access all three flow cache variants:
 *
 *   flow4_cache   IPv4 5-tuple + vrfid (20-byte key, rix_hash)
 *   flow6_cache   IPv6 5-tuple + vrfid (44-byte key, rix_hash)
 *   flowu_cache   Unified IPv4/IPv6 in one table (44-byte key, family field)
 *
 * API
 * ---
 * All per-variant functions follow the pattern:
 *
 *   <variant>_cache_<operation>(...)
 *
 * where <variant> is one of:  flow4  flow6  flowu
 *
 *   Lifecycle
 *     flow4_cache_init(fc, buckets, nb_bk, pool, max_entries, timeout_ms,
 *                      init_cb, fini_cb, cb_arg)
 *     flow4_cache_flush(fc)
 *
 *   Sizing
 *     flow_cache_pool_count(max_entries)              -- pool element count for cache_init (rounded to 2^n, min 64)
 *     flow_cache_pool_size(max_entries, entry_size)   -- pool byte count for aligned_alloc (size_t)
 *     flow_cache_nb_bk_hint(max_entries)              -- bucket count for ~50% fill
 *
 *   Lookup
 *     flow4_cache_lookup_batch(fc, keys, nb_pkts, results)
 *     flow4_cache_find(fc, key)          -- single key, no pipeline
 *
 *   Mutation
 *     flow4_cache_insert(fc, key, now)
 *     flow4_cache_remove(fc, entry)
 *
 *   Hit processing
 *     flow4_cache_touch(entry, now)   -- refresh timestamp; update userdata after
 *
 *   Aging (call every batch)
 *     flow4_cache_expire(fc, now)
 *     flow4_cache_expire_2stage(fc, now) -- bucket-prefetch variant
 *     flow4_cache_adjust_timeout(fc, misses)
 *
 *   Statistics
 *     flow4_cache_stats(fc, out)
 *     flow4_cache_nb_entries(fc)
 *
 *   Timestamp (TSC, variant-independent)
 *     flow_cache_rdtsc()
 *     flow_cache_calibrate_tsc_hz()
 *     flow_cache_ms_to_tsc(tsc_hz, ms)
 *
 * FC_CALL: variant-generic call macro
 * ------------------------------------
 * Because the API is macro-generated, function names can be hard to follow.
 * FC_CALL(prefix, suffix) expands to prefix##_##suffix after full macro
 * expansion of both arguments.  The underscore is inserted automatically.
 *
 *   FC_CALL(flow4, cache_init)(&fc, buckets, nb_bk, pool, max_entries, timeout_ms,
 *                              init_cb, fini_cb, cb_arg);
 *   FC_CALL(flow4, cache_lookup_batch)(&fc, keys, nb_pkts, results);
 *   FC_CALL(flow4, cache_insert)(&fc, &key, now);
 *   FC_CALL(flow4, cache_touch)(entry, now);
 *   FC_CALL(flow4, cache_adjust_timeout)(&fc, misses);
 *   FC_CALL(flow4, cache_expire)(&fc, now);
 *
 * When the prefix is a macro token, it is fully expanded first:
 *   #define MY_FC flow4
 *   FC_CALL(MY_FC, cache_init)(&fc, ...);   // → flow4_cache_init(&fc, ...)
 *
 * Typical packet processing loop (using FC_CALL):
 *
 *   #define MY_FC flow4
 *
 *   uint64_t now = flow_cache_rdtsc();
 *   FC_CALL(MY_FC, cache_lookup_batch)(&fc, keys, nb_pkts, results);
 *
 *   unsigned misses = 0;
 *   for (unsigned i = 0; i < nb_pkts; i++) {
 *       if (results[i]) {
 *           FC_CALL(MY_FC, cache_touch)(results[i], now);
 *           // update userdata directly (e.g. MY_PAYLOAD(results[i])->packets++)
 *       } else {
 *           FC_CALL(MY_FC, cache_insert)(&fc, &keys[i], now); // init_cb fills userdata
 *           misses++;
 *       }
 *   }
 *   FC_CALL(MY_FC, cache_adjust_timeout)(&fc, misses);
 *   FC_CALL(MY_FC, cache_expire)(&fc, now);
 */

#include "flow4_cache.h"
#include "flow6_cache.h"
#include "flow_unified_cache.h"

#endif /* _FLOW_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
