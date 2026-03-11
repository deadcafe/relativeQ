/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _LIBRIX_H_
#define _LIBRIX_H_

/*
 * librix -- Relative Index Library
 *
 * Index-based data structures for shared memory / mmapped regions.
 * All heads and nodes store unsigned indices (1-origin, RIX_NIL=0)
 * instead of raw pointers, enabling:
 *
 *   - Relocatable structures across processes and after remapping
 *   - Direct placement in shared memory and file-backed regions
 *   - Mixed 32/64-bit process interoperability
 *   - Zero-initialization produces valid empty state
 *
 * Each API takes a `base` pointer (element array) for index-to-pointer
 * conversion at call time.
 *
 * -----------------------------------------------------------------------
 * Data Structures
 * -----------------------------------------------------------------------
 *
 *   Queues (rix/rix_queue.h)
 *     RIX_SLIST   -- singly-linked list
 *     RIX_LIST    -- doubly-linked list
 *     RIX_STAILQ  -- singly-linked tail queue
 *     RIX_TAILQ   -- doubly-linked tail queue
 *     RIX_CIRCLEQ -- circular queue
 *
 *   Tree (rix/rix_tree.h)
 *     RIX_RB      -- red-black tree
 *
 *   Hash Tables (rix/rix_hash.h, rix/rix_hash32.h, rix/rix_hash64.h)
 *     RIX_HASH    -- cuckoo hash, 16-way bucket, fingerprint in bucket,
 *                    variable-length key in node.  O(1) remove via
 *                    cur_hash field.
 *     RIX_HASH32  -- cuckoo hash, uint32_t key stored in bucket
 *     RIX_HASH64  -- cuckoo hash, uint64_t key stored in bucket
 *
 *     All hash variants provide:
 *       - Runtime SIMD dispatch (Generic / AVX2 / AVX-512)
 *       - N-ahead pipelined staged find (hash_key, scan_bk,
 *         prefetch_node, cmp_key) for DRAM latency hiding
 *       - init, find, insert, remove, walk
 *
 * -----------------------------------------------------------------------
 * Quick Start
 * -----------------------------------------------------------------------
 *
 *   #include "librix.h"
 *
 *   // For hash tables, call once at startup:
 *   rix_hash_arch_init();
 *
 * -----------------------------------------------------------------------
 * Requirements
 * -----------------------------------------------------------------------
 *
 *   C11 (gnu11), single-header-per-module, no external dependencies.
 *   Optional: -mavx2 / -mavx512f for SIMD-accelerated hash operations.
 */

#include <rix/rix_queue.h>
#include <rix/rix_tree.h>
#include <rix/rix_hash.h>
#include <rix/rix_hash_key.h>

#endif /* _LIBRIX_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
