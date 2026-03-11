/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * rix_hash64.h – index-based cuckoo hash table for 64-bit keys, C11
 *
 * Key type: uint64_t, stored directly in bucket (no fingerprint array).
 * No hash_field required in the node struct.
 * Runtime SIMD dispatch via rix_hash_arch (shared with rix_hash.h).
 * Index-based: RIX_NIL=0, 1-origin indices (no raw pointers stored).
 *
 * Bucket layout:
 *   CL0+CL1: uint64_t key[16] – 128 bytes: actual 64-bit key values
 *   CL2:     uint32_t idx[16] –  64 bytes: 1-origin node indices
 *
 * Empty slot sentinel: idx[s] == RIX_NIL (0).
 * scan_bk performs exact 64-bit key match against bk->key[] using find_u64x16.
 * remove re-hashes elm->key_field to locate the bucket, then uses find_u32x16
 * directly on idx[16] (16 uint32_t, perfectly sized) to find the slot by index.
 * kickout re-hashes the victim key to find the alternate bucket.
 *
 * Slot count vs rix_hash32:
 *   rix_hash32: 16 slots/bucket, 128 B/bucket (2 CL)
 *   rix_hash64: 16 slots/bucket, 192 B/bucket (3 CL)
 *
 *   Both variants hold 16 entries per bucket, so fill-rate behaviour is
 *   identical.  The only extra cost is the wider key array (128 B vs 64 B):
 *   rix_hash64 uses 1.5× the bucket memory of rix_hash32 for the same entry
 *   count.
 *
 *   Empirical fill-rate (FOLLOW_DEPTH=8, measured by bench_rix_hash64):
 *     <=80 % fill : 0 kickout failures (safe operating range)
 *     <=95 % fill : 0 kickout failures
 *     ~100% fill  : kickout failures begin
 *   Conclusion: fill rate behaviour is essentially identical to rix_hash32.
 *
 * Performance comparison (DRAM-cold, same entry count=10 M, FOLLOW_DEPTH=8):
 *   rix_hash32 (1048576 bk, 16 slots, 128 B/bk): x8 avg ~58-60 cy/op
 *   rix_hash64 (1048576 bk, 16 slots, 192 B/bk): x8 avg ~62-66 cy/op
 *   rix_hash fp (1048576 bk, fingerpt, 128 B/bk): x8 avg ~84-88 cy/op
 *
 *   The extra cache-line per bucket (192 B vs 128 B) accounts for the small
 *   throughput gap.  (see bench_rix_hash64 for empirical measurements)
 */

#ifndef _RIX_HASH64_H_
#define _RIX_HASH64_H_

#include "rix_hash.h"

/*===========================================================================
 * Bucket entry count
 *===========================================================================*/
#define RIX_HASH64_BUCKET_ENTRY_SZ 16

/*===========================================================================
 * Bucket layout
 * 192 bytes = 3 × 64-byte cache lines, aligned to 64 bytes.
 *
 *   CL0+CL1  key[0..15] : 16 × uint64_t  = 128 B  (SIMD key scan target)
 *   CL2      idx[0..15] : 16 × uint32_t  =  64 B  (node indices, 1-origin)
 *
 * idx[16] is exactly uint32_t[16] – find_u32x16 scans it directly in remove.
 *===========================================================================*/
struct rix_hash64_bucket_s {
    uint64_t key[RIX_HASH64_BUCKET_ENTRY_SZ]; /* CL0+CL1: 64-bit keys        */
    uint32_t idx[RIX_HASH64_BUCKET_ENTRY_SZ]; /* CL2: 1-origin node idx      */
} __attribute__((aligned(64)));

/*===========================================================================
 * Head struct
 *===========================================================================*/
#define RIX_HASH64_HEAD(name)                                                 \
    struct name {                                                             \
        unsigned rhh_mask;                                                    \
        unsigned rhh_nb;                                                      \
    }

/*===========================================================================
 * Find context (staged pipeline)
 *===========================================================================*/
struct rix_hash64_find_ctx_s {
    struct rix_hash64_bucket_s *bk[2];
    uint64_t                    key;        /* 64-bit search key             */
    uint32_t                    hits[2];    /* bitmask: key match in bk[i]   */
};

/*===========================================================================
 * Internal hash function for 64-bit keys (CRC32C-based, SSE4.2)
 *
 * Produces two 32-bit hashes h0, h1 such that
 * (h0 & mask) != (h1 & mask) – guaranteed by the retry loop.
 *
 *   h0 = CRC32C(0,   key)
 *   h1 = CRC32C(~h0, key), re-seeded until bucket indices differ.
 *===========================================================================*/
static RIX_FORCE_INLINE _RIX_UNUSED union rix_hash_hash_u
_rix_hash64_fn(uint64_t key, uint32_t mask)
{
    union rix_hash_hash_u r;
#if defined(__x86_64__) && defined(__SSE4_2__)
    uint32_t h0   = (uint32_t)__builtin_ia32_crc32di(0ULL, key);
    uint32_t bk0  = h0 & mask;
    uint32_t seed = ~h0;
    uint32_t h1;
    do {
        h1   = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, key);
        seed = (uint32_t)__builtin_ia32_crc32di((uint64_t)seed, ~key);
    } while ((h1 & mask) == bk0);
    r.val32[0] = h0;
    r.val32[1] = h1;
#else
    /* Generic fallback: mix 64-bit key into two independent 32-bit hashes */
    uint32_t lo  = (uint32_t)key;
    uint32_t hi  = (uint32_t)(key >> 32);
    uint32_t h0  = (lo ^ (hi * 2654435761u)) * 2246822519u;
    uint32_t bk0 = h0 & mask;
    uint32_t h1  = (hi ^ (lo * 2246822519u)) * 2654435761u ^ h0;
    uint32_t inc = 1u;
    while ((h1 & mask) == bk0) {
        h1  = (h1 ^ inc) * 2246822519u;
        inc++;
    }
    r.val32[0] = h0;
    r.val32[1] = h1;
#endif
    return r;
}

/*===========================================================================
 * RIX_HASH64_GENERATE(name, type, key_field, invalid_key)
 *
 *   name        – head struct tag AND generated-function prefix (must match
 *                 the tag given to RIX_HASH64_HEAD)
 *   type        – element struct type
 *   key_field   – field name inside type; must be uint64_t
 *   invalid_key – compile-time constant written to bk->key[] for empty/
 *                 removed slots.  Must not equal any key the caller will
 *                 ever insert or search.
 *
 * The hash function (_rix_hash64_fn) is provided internally.
 * No hash_field is required in the node struct.
 *
 * Invalid key contract:
 *   name_init() fills every bk->key[] slot with invalid_key.
 *   name_remove() restores the freed slot's key to invalid_key.
 *   With this contract:
 *     – scan_bk never hits an empty/removed slot for a valid search key,
 *       so cmp_key / prefetch_node need no idx != RIX_NIL guard.
 *     – cmp_key reduces to a single __builtin_ctz on the hit mask.
 *     – invalid_key is a compile-time constant; no runtime load from head.
 *
 * Fill rate note:
 *   With 16 slots per bucket (same as rix_hash32), cuckoo achieves
 *   the same fill rate behaviour.  Operating up to ~95 % fill is safe.
 *   Each bucket is 192 B (3 CL) vs 128 B (2 CL) for rix_hash32, so
 *   rix_hash64 uses 1.5× the bucket memory for the same entry count.
 *   See bench_rix_hash64 for measured curves.
 *
 * Generated functions:
 *
 *   Initialisation (must be called before any other operation):
 *     void  name_init (head, buckets, nb_bk)
 *
 *   Staged find (x1):
 *     void  name_hash_key      (ctx, head, buckets, key)  key: uint64_t
 *     void  name_scan_bk       (ctx, head, buckets)
 *     void  name_prefetch_node (ctx, base)
 *     type *name_cmp_key       (ctx, base)
 *
 *   Staged find (xN bulk; FORCE_INLINE + constant n → compiler unrolls):
 *     void  name_hash_key_n    (ctx, n, head, buckets, keys)
 *     void  name_scan_bk_n     (ctx, n, head, buckets)
 *     void  name_prefetch_node_n(ctx, n, base)
 *     void  name_cmp_key_n     (ctx, n, base, results)
 *
 *   Single-shot ops:
 *     type *name_find  (head, buckets, base, key)   key is uint64_t
 *     type *name_insert(head, buckets, base, elm)
 *     type *name_remove(head, buckets, base, elm)   elm is type *
 *     int   name_walk  (head, buckets, base, cb, arg)
 *===========================================================================*/
#define RIX_HASH64_GENERATE(name, type, key_field, invalid_key)               \
                                                                              \
/* ================================================================== */      \
/* Initialisation                                                     */      \
/*                                                                    */      \
/* Must be called before any other operation on this table.           */      \
/* Fills every bk->key[] slot with invalid_key, every bk->idx[] slot  */      \
/* with RIX_NIL.  nb_bk must be a power of 2.                         */      \
/* ================================================================== */      \
                                                                              \
static _RIX_UNUSED void                                                       \
name##_init(struct name *head,                                                \
            struct rix_hash64_bucket_s *buckets,                              \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    head->rhh_nb   = 0u;                                                      \
    for (unsigned _b = 0u; _b < nb_bk; _b++) {                                \
        struct rix_hash64_bucket_s *_bk = buckets + _b;                       \
        for (unsigned _s = 0u; _s < RIX_HASH64_BUCKET_ENTRY_SZ; _s++) {       \
            _bk->key[_s] = (uint64_t)(invalid_key);                           \
            _bk->idx[_s] = (uint32_t)RIX_NIL;                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
/* ------------------------------------------------------------------ */      \
/* Internal helpers: 1-origin index <-> pointer                       */      \
/* ------------------------------------------------------------------ */      \
static RIX_FORCE_INLINE _RIX_UNUSED unsigned                                  \
name##_hidx(type *base, const type *p) {                                      \
    return RIX_IDX_FROM_PTR(base, (type *)(uintptr_t)p);                      \
}                                                                             \
static RIX_FORCE_INLINE _RIX_UNUSED type *                                    \
name##_hptr(type *base, unsigned i) {                                         \
    return RIX_PTR_FROM_IDX(base, i);                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find – x1                                                   */      \
/* ================================================================== */      \
                                                                              \
/* Stage 1: compute hash, resolve bucket pointers, issue bk[0] prefetch. */   \
/* Prefetch all 3 cache lines of bk[0]:                                  */   \
/*   CL0 (  +0): key[ 0.. 7] – needed by scan_bk (find_u64x16 lo half)   */   \
/*   CL1 ( +64): key[ 8..15] – needed by scan_bk (find_u64x16 hi half)   */   \
/*   CL2 (+128): idx[ 0..15] – needed by prefetch_node and cmp_key       */   \
/* bk[1] is not prefetched; it is scanned lazily in cmp_key on bk[0]     */   \
/* miss (moderate fill -> most hits land in bk[0]).                      */   \
/* key is passed by value (uint64_t), not as a pointer.                  */   \
static RIX_FORCE_INLINE _RIX_UNUSED void                                      \
name##_hash_key(struct rix_hash64_find_ctx_s *ctx,                            \
                struct name *head,                                            \
                struct rix_hash64_bucket_s *buckets,                          \
                uint64_t key)                                                 \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h = _rix_hash64_fn(key, mask);                     \
    ctx->key   = key;                                                         \
    ctx->bk[0] = buckets + (_h.val32[0] & mask);                              \
    ctx->bk[1] = buckets + (_h.val32[1] & mask);                              \
    __builtin_prefetch((const char *)ctx->bk[0] +   0, 0, 1); /* CL0: key lo */\
    __builtin_prefetch((const char *)ctx->bk[0] +  64, 0, 1); /* CL1: key hi */\
    __builtin_prefetch((const char *)ctx->bk[0] + 128, 0, 1); /* CL2: idx    */\
}                                                                             \
                                                                              \
/* Stage 2: scan bk[0]->key[] for ctx->key using find_u64x16. */              \
/* hits[1] = 0; bk[1] is scanned lazily in cmp_key on bk[0] miss. */          \
static RIX_FORCE_INLINE _RIX_UNUSED void                                      \
name##_scan_bk(struct rix_hash64_find_ctx_s *ctx,                             \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash64_bucket_s *buckets __attribute__((unused)))   \
{                                                                             \
    ctx->hits[0] = rix_hash_arch->find_u64x16(ctx->bk[0]->key, ctx->key);     \
    ctx->hits[1] = 0u;                                                        \
}                                                                             \
                                                                              \
/* Stage 3: prefetch node data for all hits[0] positions. */                  \
static RIX_FORCE_INLINE _RIX_UNUSED void                                      \
name##_prefetch_node(struct rix_hash64_find_ctx_s *ctx,                       \
                     type *base)                                              \
{                                                                             \
    uint32_t _hits = ctx->hits[0];                                            \
    while (_hits) {                                                           \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        _hits &= _hits - 1u;                                                  \
        __builtin_prefetch(                                                   \
            name##_hptr(base, ctx->bk[0]->idx[_bit]), 0, 0);                  \
    }                                                                         \
}                                                                             \
                                                                              \
/* Stage 4: return the node for the hit; lazily scan bk[1] on miss. */        \
static RIX_FORCE_INLINE _RIX_UNUSED type *                                    \
name##_cmp_key(struct rix_hash64_find_ctx_s *ctx,                             \
               type *base)                                                    \
{                                                                             \
    /* Fast path: bk[0] */                                                    \
    uint32_t _hits = ctx->hits[0];                                            \
    if (_hits) {                                                              \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        return name##_hptr(base, ctx->bk[0]->idx[_bit]);                      \
    }                                                                         \
    /* Slow path: bk[0] miss – lazily fetch and scan bk[1] */                 \
    {                                                                         \
        _hits = rix_hash_arch->find_u64x16(ctx->bk[1]->key, ctx->key);        \
        if (_hits) {                                                          \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            return name##_hptr(base, ctx->bk[1]->idx[_bit]);                  \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find – xN bulk                                              */      \
/* FORCE_INLINE + constant n -> compiler unrolls identically to xN.   */      \
/* ================================================================== */      \
static RIX_FORCE_INLINE _RIX_UNUSED void                                      \
name##_hash_key_n(struct rix_hash64_find_ctx_s *ctx,                          \
                  int n,                                                      \
                  struct name *head,                                          \
                  struct rix_hash64_bucket_s *buckets,                        \
                  const uint64_t *keys)                                       \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_hash_key(&ctx[_j], head, buckets, keys[_j]);                   \
}                                                                             \
                                                                              \
static RIX_FORCE_INLINE _RIX_UNUSED void                                      \
name##_scan_bk_n(struct rix_hash64_find_ctx_s *ctx,                           \
                 int n,                                                       \
                 struct name *head,                                           \
                 struct rix_hash64_bucket_s *buckets)                         \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_scan_bk(&ctx[_j], head, buckets);                              \
}                                                                             \
                                                                              \
static RIX_FORCE_INLINE _RIX_UNUSED void                                      \
name##_prefetch_node_n(struct rix_hash64_find_ctx_s *ctx,                     \
                       int n,                                                 \
                       type *base)                                            \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_prefetch_node(&ctx[_j], base);                                 \
}                                                                             \
                                                                              \
static RIX_FORCE_INLINE _RIX_UNUSED void                                      \
name##_cmp_key_n(struct rix_hash64_find_ctx_s *ctx,                           \
                 int n,                                                       \
                 type *base,                                                  \
                 type **results)                                              \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        results[_j] = name##_cmp_key(&ctx[_j], base);                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Single-shot find = hash_key + scan_bk + cmp_key                    */      \
/* ================================================================== */      \
static RIX_FORCE_INLINE _RIX_UNUSED type *                                    \
name##_find(struct name *head,                                                \
            struct rix_hash64_bucket_s *buckets,                              \
            type *base,                                                       \
            uint64_t key)                                                     \
{                                                                             \
    struct rix_hash64_find_ctx_s _ctx;                                        \
    name##_hash_key(&_ctx, head, buckets, key);                               \
    name##_scan_bk (&_ctx, head, buckets);                                    \
    return name##_cmp_key(&_ctx, base);                                       \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Insert with cuckoo kickout                                         */      \
/*                                                                    */      \
/* Returns:                                                           */      \
/*   NULL     – inserted successfully                                 */      \
/*   elm      – table full (kickout exhausted)                        */      \
/*   other    – duplicate found, returns the existing node            */      \
/*                                                                    */      \
/* No hash_field in node: kickout re-hashes victim key to find alt    */      \
/* bucket (slightly more expensive than XOR trick, but rare path).    */      \
/* ================================================================== */      \
static _RIX_UNUSED type *                                                     \
name##_insert(struct name *head,                                              \
              struct rix_hash64_bucket_s *buckets,                            \
              type *base,                                                     \
              type *elm)                                                      \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        _rix_hash64_fn((uint64_t)elm->key_field, mask);                       \
    unsigned _bk0 = _h.val32[0] & mask;                                       \
    unsigned _bk1 = _h.val32[1] & mask;                                       \
                                                                              \
    /* Duplicate check in both candidate buckets. */                          \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash64_bucket_s *_bk =                                     \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        uint32_t _hits = rix_hash_arch->find_u64x16(_bk->key,                 \
                                                   (uint64_t)elm->key_field); \
        if (_hits) {                                                          \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            return name##_hptr(base, _bk->idx[_bit]); /* duplicate */         \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Fast path: find an empty slot in bk0 then bk1 */                       \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash64_bucket_s *_bk = buckets + _bki;                     \
        uint32_t _nilm = rix_hash_arch->find_u64x16(_bk->key,                 \
                                            (uint64_t)(invalid_key));         \
        if (_nilm) {                                                          \
            unsigned _slot    = (unsigned)__builtin_ctz(_nilm);               \
            _bk->key[_slot] = (uint64_t)elm->key_field;                       \
            _bk->idx[_slot] = name##_hidx(base, elm);                         \
            head->rhh_nb++;                                                   \
            return NULL; /* success */                                        \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Slow path: cuckoo kickout loop. */                                     \
    /* No hash_field in node: re-hash victim key to find its alt bucket. */   \
    {                                                                         \
        uint64_t _new_key = (uint64_t)elm->key_field;                         \
        unsigned _new_idx = name##_hidx(base, elm);                           \
        unsigned _cur_bk  = _bk0;                                             \
                                                                              \
        for (int _d = 0; _d < RIX_HASH_FOLLOW_DEPTH; _d++) {                  \
            struct rix_hash64_bucket_s *_bk = buckets + _cur_bk;              \
            unsigned _pos =                                                   \
                (unsigned)_d & (RIX_HASH64_BUCKET_ENTRY_SZ - 1u);             \
                                                                              \
            /* Save victim */                                                 \
            uint64_t _vic_key = _bk->key[_pos];                               \
            unsigned _vic_idx = _bk->idx[_pos];                               \
                                                                              \
            /* Place new entry */                                             \
            _bk->key[_pos] = _new_key;                                        \
            _bk->idx[_pos] = _new_idx;                                        \
                                                                              \
            /* Re-hash victim to find its two candidate buckets */            \
            union rix_hash_hash_u _vh = _rix_hash64_fn(_vic_key, mask);       \
            unsigned _vic_bk0 = _vh.val32[0] & mask;                          \
            unsigned _alt_bk  = (_vic_bk0 == _cur_bk)                         \
                                ? (_vh.val32[1] & mask)                       \
                                : _vic_bk0;                                   \
                                                                              \
            /* Try to place victim in alt bucket */                           \
            struct rix_hash64_bucket_s *_alt = buckets + _alt_bk;             \
            uint32_t _nilm = rix_hash_arch->find_u64x16(_alt->key,            \
                                            (uint64_t)(invalid_key));         \
            if (_nilm) {                                                      \
                unsigned _slot    = (unsigned)__builtin_ctz(_nilm);           \
                _alt->key[_slot] = _vic_key;                                  \
                _alt->idx[_slot] = _vic_idx;                                  \
                head->rhh_nb++;                                               \
                return NULL; /* success */                                    \
            }                                                                 \
            /* Victim becomes the next entry to displace */                   \
            _new_key = _vic_key;                                              \
            _new_idx = _vic_idx;                                              \
            _cur_bk  = _alt_bk;                                               \
        }                                                                     \
        /* Kickout exhausted; elm was not inserted */                         \
    }                                                                         \
    return elm; /* table full */                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Remove – evict a known node from the table                         */      \
/*                                                                    */      \
/* Re-hashes elm->key_field to locate the two candidate buckets, then */      \
/* searches each by node index using find_u32x16 on idx[16] (exactly  */      \
/* 16 uint32_t).                                                      */      \
/* Returns elm on success, NULL if elm is not currently in the table. */      \
/* ================================================================== */      \
static _RIX_UNUSED type *                                                     \
name##_remove(struct name *head,                                              \
              struct rix_hash64_bucket_s *buckets,                            \
              type *base,                                                     \
              type *elm)                                                      \
{                                                                             \
    unsigned mask     = head->rhh_mask;                                       \
    unsigned node_idx = name##_hidx(base, elm);                               \
    union rix_hash_hash_u _h =                                                \
        _rix_hash64_fn((uint64_t)elm->key_field, mask);                       \
    unsigned _bk0 = _h.val32[0] & mask;                                       \
    unsigned _bk1 = _h.val32[1] & mask;                                       \
                                                                              \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash64_bucket_s *_bk =                                     \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        /* Scan idx[16] as uint32_t[16] to locate slot by node index */       \
        uint32_t _hits = rix_hash_arch->find_u32x16(                          \
                             (const uint32_t *)_bk->idx,                      \
                             (uint32_t)node_idx);                             \
        if (_hits) {                                                          \
            unsigned _slot    = (unsigned)__builtin_ctz(_hits);               \
            _bk->key[_slot] = (uint64_t)(invalid_key);                        \
            _bk->idx[_slot] = (uint32_t)RIX_NIL;                              \
            head->rhh_nb--;                                                   \
            return elm;                                                       \
        }                                                                     \
    }                                                                         \
    return NULL; /* not in table */                                           \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Walk – iterate over all occupied slots                             */      \
/* ================================================================== */      \
static _RIX_UNUSED int                                                        \
name##_walk(struct name *head,                                                \
            struct rix_hash64_bucket_s *buckets,                              \
            type *base,                                                       \
            int (*cb)(type *, void *),                                        \
            void *arg)                                                        \
{                                                                             \
    unsigned _nb_bk = head->rhh_mask + 1u;                                    \
    for (unsigned _b = 0u; _b < _nb_bk; _b++) {                               \
        struct rix_hash64_bucket_s *_bk = buckets + _b;                       \
        for (unsigned _s = 0u; _s < RIX_HASH64_BUCKET_ENTRY_SZ; _s++) {       \
            unsigned _nidx = _bk->idx[_s];                                    \
            if (_nidx == (unsigned)RIX_NIL)                                   \
                continue;                                                     \
            type *_node = name##_hptr(base, _nidx);                           \
            int   _r    = cb(_node, arg);                                     \
            if (_r)                                                           \
                return _r;                                                    \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}

/*===========================================================================
 * Convenience macro API
 *
 * Single-shot ops:
 *   RIX_HASH64_INIT        (name, head, buckets, nb_bk)
 *   RIX_HASH64_FIND        (name, head, buckets, base, key)
 *   RIX_HASH64_INSERT      (name, head, buckets, base, elm)
 *   RIX_HASH64_REMOVE      (name, head, buckets, base, elm)
 *   RIX_HASH64_WALK        (name, head, buckets, base, cb, arg)
 *
 * Staged find – x1:
 *   RIX_HASH64_HASH_KEY    (name, ctx, head, buckets, key)
 *   RIX_HASH64_SCAN_BK     (name, ctx, head, buckets)
 *   RIX_HASH64_PREFETCH_NODE(name, ctx, base)
 *   RIX_HASH64_CMP_KEY     (name, ctx, base)
 *
 * Staged find – x2 / x4:
 *   RIX_HASH64_HASH_KEY2   (name, ctx, head, buckets, keys)
 *   RIX_HASH64_SCAN_BK2    (name, ctx, head, buckets)
 *   RIX_HASH64_PREFETCH_NODE2(name, ctx, base)
 *   RIX_HASH64_CMP_KEY2    (name, ctx, base, results)
 *
 *   RIX_HASH64_HASH_KEY4   (name, ctx, head, buckets, keys)
 *   RIX_HASH64_SCAN_BK4    (name, ctx, head, buckets)
 *   RIX_HASH64_PREFETCH_NODE4(name, ctx, base)
 *   RIX_HASH64_CMP_KEY4    (name, ctx, base, results)
 *
 * Staged find – xN:
 *   RIX_HASH64_HASH_KEY_N  (name, ctx, n, head, buckets, keys)
 *   RIX_HASH64_SCAN_BK_N   (name, ctx, n, head, buckets)
 *   RIX_HASH64_PREFETCH_NODE_N(name, ctx, n, base)
 *   RIX_HASH64_CMP_KEY_N   (name, ctx, n, base, results)
 *===========================================================================*/

/* ---- single-shot ops ---------------------------------------------------- */
#define RIX_HASH64_INIT(name, head, buckets, nb_bk)                           \
    name##_init(head, buckets, nb_bk)

#define RIX_HASH64_FIND(name, head, buckets, base, key)                       \
    name##_find(head, buckets, base, key)

#define RIX_HASH64_INSERT(name, head, buckets, base, elm)                     \
    name##_insert(head, buckets, base, elm)

#define RIX_HASH64_REMOVE(name, head, buckets, base, elm)                     \
    name##_remove(head, buckets, base, elm)

#define RIX_HASH64_WALK(name, head, buckets, base, cb, arg)                   \
    name##_walk(head, buckets, base, cb, arg)

/* ---- staged find – x1 --------------------------------------------------- */
#define RIX_HASH64_HASH_KEY(name, ctx, head, buckets, key)                    \
    name##_hash_key(ctx, head, buckets, key)

#define RIX_HASH64_SCAN_BK(name, ctx, head, buckets)                          \
    name##_scan_bk(ctx, head, buckets)

#define RIX_HASH64_PREFETCH_NODE(name, ctx, base)                             \
    name##_prefetch_node(ctx, base)

#define RIX_HASH64_CMP_KEY(name, ctx, base)                                   \
    name##_cmp_key(ctx, base)

/* ---- staged find – x2 --------------------------------------------------- */
#define RIX_HASH64_HASH_KEY2(name, ctx, head, buckets, keys)                  \
    name##_hash_key_n(ctx, 2, head, buckets, keys)

#define RIX_HASH64_SCAN_BK2(name, ctx, head, buckets)                         \
    name##_scan_bk_n(ctx, 2, head, buckets)

#define RIX_HASH64_PREFETCH_NODE2(name, ctx, base)                            \
    name##_prefetch_node_n(ctx, 2, base)

#define RIX_HASH64_CMP_KEY2(name, ctx, base, results)                         \
    name##_cmp_key_n(ctx, 2, base, results)

/* ---- staged find – x4 --------------------------------------------------- */
#define RIX_HASH64_HASH_KEY4(name, ctx, head, buckets, keys)                  \
    name##_hash_key_n(ctx, 4, head, buckets, keys)

#define RIX_HASH64_SCAN_BK4(name, ctx, head, buckets)                         \
    name##_scan_bk_n(ctx, 4, head, buckets)

#define RIX_HASH64_PREFETCH_NODE4(name, ctx, base)                            \
    name##_prefetch_node_n(ctx, 4, base)

#define RIX_HASH64_CMP_KEY4(name, ctx, base, results)                         \
    name##_cmp_key_n(ctx, 4, base, results)

/* ---- staged find – xN --------------------------------------------------- */
#define RIX_HASH64_HASH_KEY_N(name, ctx, n, head, buckets, keys)              \
    name##_hash_key_n(ctx, n, head, buckets, keys)

#define RIX_HASH64_SCAN_BK_N(name, ctx, n, head, buckets)                     \
    name##_scan_bk_n(ctx, n, head, buckets)

#define RIX_HASH64_PREFETCH_NODE_N(name, ctx, n, base)                        \
    name##_prefetch_node_n(ctx, n, base)

#define RIX_HASH64_CMP_KEY_N(name, ctx, n, base, results)                     \
    name##_cmp_key_n(ctx, n, base, results)

#endif /* _RIX_HASH64_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
