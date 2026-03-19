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
 * rix_hash.h - index-based cuckoo hash table, single header, C11
 *
 * Key: arbitrary-size struct member; sizeof inferred at compile time.
 * Key comparison: user-supplied cmp_fn(const KEY_TYPE *, const KEY_TYPE *)
 *   passed to RIX_HASH_GENERATE; return 0 if equal.  A SIMD implementation gives
 *   best throughput; a thin memcmp wrapper works for correctness.
 * Runtime SIMD dispatch: Generic / AVX2 / AVX-512 (fingerprint search only).
 * Macro-based type-safe API via RIX_HASH_GENERATE (same style as rix_tree.h).
 * Index-based: RIX_NIL=0, 1-origin indices (no raw pointers stored).
 * No typedef: type is always used as "struct type".
 */

#ifndef _RIX_HASH_H_
#  define _RIX_HASH_H_

#  include "rix_defs_private.h"

#  include <stdint.h>

#  include "rix_hash_arch.h"

#  ifndef _RIX_HASH_FIND_U32X16
#    define _RIX_HASH_FIND_U32X16(arr, val) \
        rix_hash_arch->find_u32x16((arr), (val))
#  endif

#  ifndef _RIX_HASH_FIND_U32X16_2
#    define _RIX_HASH_FIND_U32X16_2(arr, val0, val1, mask0, mask1) \
        rix_hash_arch->find_u32x16_2((arr), (val0), (val1), (mask0), (mask1))
#  endif

/*
 * Map a generated hash-table prefix and an operation suffix to the actual
 * generated helper symbol.  This only performs token pasting; availability of
 * a given op depends on the selected RIX_HASH_GENERATE_* variant.
 */
#  ifndef RIX_HASH_FUNC
#    define RIX_HASH_FUNC(name, op) name##_##op
#  endif

/*===========================================================================
 * Fingerprint-variant bucket layout
 * 128 bytes = 2 x 64-byte cache lines, aligned to 64 bytes.
 *===========================================================================*/
struct rix_hash_bucket_s {
    uint32_t hash[RIX_HASH_BUCKET_ENTRY_SZ]; /* 64 bytes: fingerprints       */
    uint32_t idx [RIX_HASH_BUCKET_ENTRY_SZ]; /* 64 bytes: 1-origin node idx  */
} __attribute__((aligned(64)));

/*===========================================================================
 * Head struct and init macros
 *===========================================================================*/
#  define RIX_HASH_HEAD(name)                                                   \
    struct name {                                                             \
        unsigned rhh_mask;                                                    \
        unsigned rhh_nb;                                                      \
    }

/* RIX_HASH_INIT: see convenience macro API below */

/*===========================================================================
 * Find context (staged pipeline)
 *===========================================================================*/
struct rix_hash_find_ctx_s {
    union rix_hash_hash_u     hash;
    struct rix_hash_bucket_s *bk[2];
    uint32_t                  fp;          /* full fingerprint stored in bk  */
    uint32_t                  fp_hits[2];  /* bitmask: fp match & slot !NIL  */
    const void               *key;         /* pointer to search key          */
};

/*---------------------------------------------------------------------------
 * rix_hash_prefetch_key - prefetch key data into L1/L2 before name_hash_key.
 *
 * hash_fn(key) loads key data from DRAM.  Issue this for all N keys in a
 * tight loop BEFORE the hash_key stage to hide that DRAM latency:
 *
 *   for (i = 0; i < N; i++) rix_hash_prefetch_key(keys[i]);  // Stage 0
 *   for (i = 0; i < N; i++) name_hash_key(..., keys[i]);      // Stage 1
 *   for (i = 0; i < N; i++) name_scan_bk(...);                // Stage 2
 *   for (i = 0; i < N; i++) name_cmp_key(...);                // Stage 3
 *
 * Since key_field is typically at offset 0 of the node struct, this prefetch
 * also covers the cmp_key stage's full-key comparison.
 *---------------------------------------------------------------------------*/
static RIX_FORCE_INLINE void
rix_hash_prefetch_key(const void *key)
{
    __builtin_prefetch(key, 0, 0);
}

/* Prefetch a node/entry head before key comparison. */
static RIX_FORCE_INLINE void
_rix_hash_prefetch_entry(const void *entry)
{
    __builtin_prefetch(entry, 0, 1);
}

/* Prefetch only the fingerprint/hash line of a bucket. */
static RIX_FORCE_INLINE void
_rix_hash_prefetch_bucket_hash(const struct rix_hash_bucket_s *bucket)
{
    __builtin_prefetch(&bucket->hash[0], 0, 1);
}

/* Prefetch only the idx[] line of a bucket. */
static RIX_FORCE_INLINE void
_rix_hash_prefetch_bucket_idx(const struct rix_hash_bucket_s *bucket)
{
    __builtin_prefetch(&bucket->idx[0], 0, 1);
}

/* Prefetch both cache lines of a bucket for ordinary read-mostly access. */
static RIX_FORCE_INLINE void
_rix_hash_prefetch_bucket(const struct rix_hash_bucket_s *bucket)
{
    _rix_hash_prefetch_bucket_hash(bucket);
    _rix_hash_prefetch_bucket_idx(bucket);
}

/* Backward-compatible alias: insert path also wants both bucket lines. */
static RIX_FORCE_INLINE void
_rix_hash_prefetch_bucket_insert(const struct rix_hash_bucket_s *bucket)
{
    _rix_hash_prefetch_bucket(bucket);
}

/*===========================================================================
 * Internal bucket-index arithmetic helpers
 *===========================================================================*/

/*
 * Derive the two candidate bucket indices and the fingerprint from a hash.
 *
 * The hash function guarantees (val32[0] & mask) != (val32[1] & mask), so
 * no collision handling is needed here.
 *
 *   bk0 = val32[0] & mask          (primary bucket)
 *   bk1 = val32[1] & mask          (secondary bucket, guaranteed != bk0)
 *   fp  = val32[0] ^ val32[1]      (fingerprint: XOR of both hashes)
 *
 * fp != 0 is guaranteed: since (val32[0] & mask) != (val32[1] & mask), at
 * least one lower bit differs, so the XOR is non-zero.  This means fp never
 * matches the empty-slot sentinel (hash[s] = 0), eliminating the need for a
 * _nilm filter in fingerprint searches.
 *
 * Node invariant: node->hash_field always holds the hash that corresponds
 * to the bucket where the node currently resides:
 *   node->hash_field & mask == current_bucket_index
 * On insert into bk0: hash_field = val32[0].  Into bk1: hash_field = val32[1].
 * On kickout to alt bucket: hash_field = fp ^ hash_field_old (1 XOR, no rehash).
 * Alternate bucket: alt_bk = (fp ^ hash_field) & mask  (same XOR trick).
 * Remove: bk = hash_field & mask -> direct O(1) slot lookup, no hash call.
 */
static RIX_FORCE_INLINE void
_rix_hash_buckets(const union rix_hash_hash_u h, unsigned mask,
                  unsigned *bk0_out, unsigned *bk1_out, uint32_t *fp_out)
{
    *bk0_out = h.val32[0] & mask;
    *bk1_out = h.val32[1] & mask;
    *fp_out  = h.val32[0] ^ h.val32[1];
}

/*===========================================================================
 * RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)
 * RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn)
 * RIX_HASH_GENERATE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn)
 * RIX_HASH_GENERATE_SLOT_EX(name, type, key_field, hash_field, slot_field,
 *                           cmp_fn, hash_fn)
 *
 *   name       - head struct tag AND generated-function prefix (must match
 *                the tag given to RIX_HASH_HEAD)
 *   type       - element struct type (used as "struct type"; no typedef)
 *   key_field  - field name inside struct type that holds the key.
 *                MUST be a struct type (e.g. struct flow_5tuple key;).
 *                Key size is arbitrary (no alignment constraint); the hash
 *                function handles 8->4->2->1-byte partial reads internally.
 *                Using a struct rather than a plain array (e.g. uint8_t[N])
 *                gives type-safe API parameters via __typeof__:
 *                  name_find(..., const struct my_key *key)
 *                  name_hash_key(..., const struct my_key *key)
 *                Compile errors arise if the wrong key type is passed.
 *   hash_field - field name inside struct type (uint32_t) that stores the
 *                current bucket's hash (updated on kickout to always track
 *                the bucket where the node currently resides).
 *                Must be placed before key_field in the struct (typically
 *                at offset 0, padded to align key_field).
 *                Written at insert time; used during kickout to reconstruct
 *                bk1 = (fp ^ cur_hash) & mask without re-hashing, and during
 *                remove to locate bk0 = cur_hash & mask without any hash call.
 *   cmp_fn     - key comparison function:
 *                  int cmp_fn(const KEY_TYPE *a, const KEY_TYPE *b);
 *                where KEY_TYPE is typeof(((struct type *)0)->key_field).
 *                Returns 0 if the two keys are equal, non-zero otherwise.
 *                A SIMD-optimized implementation (SSE4.1 / AVX2 / AVX-512)
 *                is recommended for hot paths.  A memcmp wrapper also works:
 *                  static int my_cmp(const struct my_key *a,
 *                                    const struct my_key *b) {
 *                      return memcmp(a, b, sizeof(*a));
 *                  }
 *
 * Default hashing is provided internally via rix_hash_hash_bytes_fast()
 * (CRC32C on x86_64+SSE4.2, FNV-1a fallback elsewhere, plus fixed-size
 * fast paths where available).
 *
 * EX variants allow callers to provide:
 *   union rix_hash_hash_u hash_fn(const key_type *key, uint32_t mask);
 * This keeps the typed API while letting fixed-size or domain-specific hash
 * functions be selected explicitly at generate time.
 *
 * SLOT variants additionally require:
 *   slot_field - field name inside struct type that stores the slot index
 *                inside the current bucket.
 *
 *                Contract:
 *                  0 <= node->slot_field < RIX_HASH_BUCKET_ENTRY_SZ
 *                  buckets[current_bucket].idx[node->slot_field] == node_idx
 *
 *                slot_field may be any caller-defined integer type, as long
 *                as it can represent the range
 *                  [0, RIX_HASH_BUCKET_ENTRY_SZ - 1].
 *                rix_hash does not impose a specific integer type.
 *
 *                SLOT variants update both hash_field and slot_field on
 *                insert and kickout so remove can directly clear the slot
 *                without scanning idx[16].  Invariant violations are
 *                programmer errors and may trigger RIX_ASSERT.
 *
 * Generated functions:
 *
 *   Staged find (x1):
 *     void           name_hash_key      (ctx, head, buckets, const key_type *key)
 *     void           name_scan_bk       (ctx, head, buckets)
 *     void           name_prefetch_node (ctx, base)
 *     struct type   *name_cmp_key       (ctx, base)
 *
 *   Staged find (xN bulk; FORCE_INLINE + constant n -> compiler unrolls):
 *     void           name_hash_key_n    (ctx, n, head, buckets, const key_type * const *keys)
 *     void           name_scan_bk_n     (ctx, n, head, buckets)
 *     void           name_prefetch_node_n(ctx, n, base)
 *     void           name_cmp_key_n     (ctx, n, base, results)
 *
 *   Single-shot ops:
 *     struct type   *name_find  (head, buckets, base, const key_type *key)
 *     struct type   *name_insert(head, buckets, base, elm)
 *     struct type   *name_remove(head, buckets, base, elm)
 *     int            name_walk  (head, buckets, base, cb, arg)
 *
 *   (key_type = __typeof__(((struct type *)0)->key_field))
 *===========================================================================*/
/* Derive the key type from key_field for use in typed API parameters. */
#  define _RIX_HASH_KEY_TYPE(type, key_field)     \
    __typeof__(((struct type *)0)->key_field)

#  define _RIX_HASH_SLOT_TYPE(type, slot_field)   \
    __typeof__(((struct type *)0)->slot_field)

#  define RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, attr) \
    attr void name##_init(struct name *head, unsigned nb_bk);                        \
    attr struct type *name##_insert(struct name *head,                               \
                                    struct rix_hash_bucket_s *buckets,               \
                                    struct type *base,                               \
                                    struct type *elm);                               \
    attr unsigned name##_remove_at(struct name *head,                                \
                                   struct rix_hash_bucket_s *buckets,                \
                                   unsigned bk,                                      \
                                   unsigned slot);                                   \
    attr struct type *name##_remove(struct name *head,                               \
                                    struct rix_hash_bucket_s *buckets,               \
                                    struct type *base,                               \
                                    struct type *elm);                               \
    attr int name##_walk(struct name *head,                                          \
                         struct rix_hash_bucket_s *buckets,                          \
                         struct type *base,                                          \
                         int (*cb)(struct type *, void *),                           \
                         void *arg);

#  define RIX_HASH_PROTOTYPE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_PROTOTYPE(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC(name, type, key_field, hash_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_PROTOTYPE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, )

#  define RIX_HASH_PROTOTYPE_STATIC_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_PROTOTYPE_STATIC_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    RIX_HASH_PROTOTYPE_INTERNAL(name, type, key_field, hash_field, cmp_fn, RIX_UNUSED static)

#  define _RIX_HASH_DEFAULT_HASH_FN_NAME(name) name ## _default_hash

#  define _RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)               \
    static RIX_UNUSED RIX_FORCE_INLINE union rix_hash_hash_u                    \
    _RIX_HASH_DEFAULT_HASH_FN_NAME(name)(                                       \
        const _RIX_HASH_KEY_TYPE(type, key_field) *key, uint32_t mask)          \
    {                                                                           \
        return rix_hash_hash_bytes_fast((const void *)key,                      \
                                        sizeof(((struct type *)0)->key_field),  \
                                        mask);                                  \
    }

#  define RIX_HASH_GENERATE_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, )

#  define RIX_HASH_GENERATE_STATIC_EX(name, type, key_field, hash_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, RIX_UNUSED static)

#  define RIX_HASH_GENERATE(name, type, key_field, hash_field, cmp_fn)         \
    _RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn,      \
                               _RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_GENERATE_STATIC(name, type, key_field, hash_field, cmp_fn) \
    _RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn,      \
                               _RIX_HASH_DEFAULT_HASH_FN_NAME(name),           \
                               RIX_UNUSED static)

#  define RIX_HASH_GENERATE_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,         \
                                    slot_field, cmp_fn, hash_fn, )

#  define RIX_HASH_GENERATE_STATIC_SLOT_EX(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,         \
                                    slot_field, cmp_fn, hash_fn,               \
                                    RIX_UNUSED static)

#  define RIX_HASH_GENERATE_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    _RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,         \
                                    slot_field, cmp_fn,                        \
                                    _RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_GENERATE_STATIC_SLOT(name, type, key_field, hash_field, slot_field, cmp_fn) \
    _RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                    \
    RIX_HASH_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field,         \
                                    slot_field, cmp_fn,                        \
                                    _RIX_HASH_DEFAULT_HASH_FN_NAME(name),      \
                                    RIX_UNUSED static)

#  define RIX_HASH_GENERATE_INTERNAL(name, type, key_field, hash_field, cmp_fn, hash_fn, attr) \
                                                                              \
/* ================================================================== */      \
/* Init                                                               */      \
/* ================================================================== */      \
attr void                                                                     \
name##_init(struct name *head,                                                \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    head->rhh_nb   = 0u;                                                      \
}                                                                             \
                                                                              \
/* ------------------------------------------------------------------ */      \
/* Internal helpers: 1-origin index <-> pointer                       */      \
/* ------------------------------------------------------------------ */      \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p) {                        \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i) {                                  \
    return RIX_PTR_FROM_IDX(base, i);                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find - x1                                                   */      \
/* ================================================================== */      \
                                                                              \
/* Stage 1: compute hash, resolve bucket pointers, issue prefetches. */       \
/* Must be called before name_scan_bk.                               */       \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_find_ctx_s *ctx,                              \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets,                            \
                const _RIX_HASH_KEY_TYPE(type, key_field) *key)               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn(key, mask);                                                   \
    unsigned _bk0, _bk1;                                                      \
    uint32_t _fp;                                                             \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    ctx->hash  = _h;                                                          \
    ctx->fp    = _fp;                                                         \
    ctx->key   = (const void *)key;                                           \
    ctx->bk[0] = buckets + _bk0;                                              \
    ctx->bk[1] = buckets + _bk1;                                              \
    _rix_hash_prefetch_bucket(ctx->bk[0]);                                    \
    /* bk_1 not prefetched: bk_0 miss path fetches it lazily in cmp_key. */   \
}                                                                             \
                                                                              \
/* Stage 2: scan bk_0 fingerprints only; produce fp_hits[0] bitmask.  */      \
/* fp != 0 (XOR-based), no _nilm filter needed; fp_hits[1] = 0 here.  */      \
/* Call prefetch_node to hide node-fetch latency before cmp_key.      */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_find_ctx_s *ctx,                               \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_s *buckets __attribute__((unused)))     \
{                                                                             \
    /* fp != 0: no spurious empty-slot matches; no _nilm filter needed. */    \
    ctx->fp_hits[0] = _RIX_HASH_FIND_U32X16(ctx->bk[0]->hash, ctx->fp);       \
    ctx->fp_hits[1] = 0u; /* bk_1 deferred to cmp_key on bk_0 miss */         \
}                                                                             \
                                                                              \
/* Stage 3: prefetch node data for all fp_hits positions.             */      \
/* Hides node-fetch DRAM latency; call for all N contexts before cmp_key. */  \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node(struct rix_hash_find_ctx_s *ctx,                         \
                     struct type *base)                                       \
{                                                                             \
    for (int _i = 0; _i < 2; _i++) {                                          \
        uint32_t _hits = ctx->fp_hits[_i];                                    \
        while (_hits) {                                                       \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            _hits &= _hits - 1u;                                              \
            _rix_hash_prefetch_entry(                                         \
                name##_hptr(base, ctx->bk[_i]->idx[_bit]));                   \
        }                                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
/* Stage 4: compare keys for bk_0 hits; lazily scan bk_1 on bk_0 miss. */     \
/* fp != 0 (XOR-based), so no _nilm filter is needed in bk_1 scan.    */      \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_find_ctx_s *ctx,                               \
               struct type *base)                                             \
{                                                                             \
    /* Fast path: bk_0 (primary bucket) */                                    \
    uint32_t _hits = ctx->fp_hits[0];                                         \
    while (_hits) {                                                           \
        unsigned      _bit  = (unsigned)__builtin_ctz(_hits);                 \
        _hits &= _hits - 1u;                                                  \
        unsigned      _nidx = ctx->bk[0]->idx[_bit];                          \
        if (_nidx == (unsigned)RIX_NIL) continue; /* removed slot */         \
        struct type  *_node = name##_hptr(base, _nidx);                       \
        if (!_node) __builtin_unreachable();                                  \
        if (cmp_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,    \
                   &_node->key_field) == 0)                                   \
            return _node;                                                     \
    }                                                                         \
    /* Slow path: bk_0 miss -> lazily fetch and scan bk_1 (secondary) */       \
    {                                                                         \
        _hits = _RIX_HASH_FIND_U32X16(ctx->bk[1]->hash, ctx->fp);             \
        while (_hits) {                                                       \
            unsigned      _bit  = (unsigned)__builtin_ctz(_hits);             \
            _hits &= _hits - 1u;                                              \
            unsigned      _nidx = ctx->bk[1]->idx[_bit];                      \
            if (_nidx == (unsigned)RIX_NIL) continue; /* removed slot */     \
            struct type  *_node = name##_hptr(base, _nidx);                   \
            if (!_node) __builtin_unreachable();                              \
            if (cmp_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key, \
                       &_node->key_field) == 0)                               \
                return _node;                                                 \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find - xN bulk                                              */      \
/* FORCE_INLINE + constant n -> compiler unrolls identically to xN.   */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_find_ctx_s *ctx,                            \
                  int n,                                                      \
                  struct name *head,                                          \
                  struct rix_hash_bucket_s *buckets,                          \
                  const _RIX_HASH_KEY_TYPE(type, key_field) * const *keys)    \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_hash_key(&ctx[_j], head, buckets, keys[_j]);                   \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_find_ctx_s *ctx,                             \
                 int n,                                                       \
                 struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets)                           \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_scan_bk(&ctx[_j], head, buckets);                              \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_find_ctx_s *ctx,                       \
                       int n,                                                 \
                       struct type *base)                                     \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_prefetch_node(&ctx[_j], base);                                 \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_find_ctx_s *ctx,                             \
                 int n,                                                       \
                 struct type *base,                                           \
                 struct type **results)                                       \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        results[_j] = name##_cmp_key(&ctx[_j], base);                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Single-shot find  = hash_key + scan_bk + cmp_key                   */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_find(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                                \
            struct type *base,                                                \
            const _RIX_HASH_KEY_TYPE(type, key_field) *key)                   \
{                                                                             \
    struct rix_hash_find_ctx_s _ctx;                                          \
    name##_hash_key(&_ctx, head, buckets, key);                               \
    name##_scan_bk (&_ctx, head, buckets);                                    \
    return name##_cmp_key(&_ctx, base);                                       \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Internal insert helper with precomputed hash                       */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_insert_hashed(struct name *head,                                       \
                     struct rix_hash_bucket_s *buckets,                       \
                     struct type *base,                                       \
                     struct type *elm,                                        \
                     union rix_hash_hash_u _h)                                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned _bk0, _bk1;                                                      \
    uint32_t _fp;                                                             \
    uint32_t _hits_fp[2];                                                     \
    uint32_t _hits_zero[2];                                                   \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    /* Store h.val32[0] in hash_field initially; updated later if bk1 chosen */\
    elm->hash_field = _h.val32[0];                                            \
    _rix_hash_prefetch_bucket(buckets + _bk0);                                \
    if (_bk1 != _bk0)                                                         \
        _rix_hash_prefetch_bucket(buckets + _bk1);                            \
                                                                              \
    /* Scan both candidate buckets once up front so duplicate detection and   \
     * empty-slot search reuse the same CL0 load. */                          \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_s *_bk =                                       \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        _RIX_HASH_FIND_U32X16_2(_bk->hash, _fp, 0u,                           \
                                &_hits_fp[_i], &_hits_zero[_i]);              \
    }                                                                         \
                                                                              \
    /* Duplicate check in both candidate buckets before inserting.            \
     * fp != 0, so no _nilm filter is needed. */                              \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_s *_bk =                                       \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        uint32_t _hits = _hits_fp[_i];                                        \
        while (_hits) {                                                       \
            unsigned      _bit  = (unsigned)__builtin_ctz(_hits);             \
            _hits &= _hits - 1u;                                              \
            struct type  *_node = name##_hptr(base, _bk->idx[_bit]);          \
            if (!_node) __builtin_unreachable();                              \
            if (cmp_fn(&elm->key_field, &_node->key_field) == 0)              \
                return _node; /* duplicate */                                 \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Fast path: find an empty slot in bk0 then bk1.                     */  \
    /* Empty slot: hash[s]==0 (fp is never 0 by construction). CL0 only.  */  \
    /* node->hash_field stores the hash corresponding to the bucket where */  \
    /* the node is placed: h.val32[0] (bk0) or h.val32[1] (bk1).          */  \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash_bucket_s *_bk = buckets + _bki;                       \
        uint32_t _nilm = _hits_zero[_i];                                      \
        if (_nilm) {                                                          \
            unsigned _slot   = (unsigned)__builtin_ctz(_nilm);                \
            _bk->hash[_slot] = _fp;                                           \
            _bk->idx [_slot] = name##_hidx(base, elm);                        \
            /* update hash_field: store the hash of the chosen bucket */      \
            if (_i == 1) elm->hash_field = _h.val32[1];                       \
            head->rhh_nb++;                                                   \
            return NULL; /* success */                                        \
        }                                                                     \
    }                                                                         \
                                                                              \
    /* Slow path: cuckoo kickout loop.                                    */  \
    /*                                                                    */  \
    /* Invariant: node->hash_field always holds the hash corresponding to */  \
    /* the bucket where the node currently resides.                       */  \
    /*   node->hash_field & mask == bucket_index_of_node                  */  \
    /*                                                                    */  \
    /* XOR trick: fp = hash_0 ^ hash_1 (stored in bucket slot).           */  \
    /* Given hash_current (= node->hash_field), the alternate bucket is:  */  \
    /*   alt_bk   = (fp ^ hash_current) & mask                            */  \
    /*   alt_hash = fp ^ hash_current                                     */  \
    /* This works in both directions (XOR is self-inverse).               */  \
    /*                                                                    */  \
    /* _new_hash tracks the hash value to write into hash_field when      */  \
    /* _new_idx is placed at _cur_bk.                                     */  \
    {                                                                         \
        uint32_t _new_fp   = _fp;                                             \
        unsigned _new_idx  = name##_hidx(base, elm);                          \
        unsigned _cur_bk   = _bk0;                                            \
        uint32_t _new_hash = _h.val32[0]; /* h.val32[0]: bk0's hash */        \
                                                                              \
        for (int _d = 0; _d < RIX_HASH_FOLLOW_DEPTH; _d++) {                  \
            struct rix_hash_bucket_s *_bk = buckets + _cur_bk;                \
            unsigned _pos = (unsigned)_d & (RIX_HASH_BUCKET_ENTRY_SZ - 1u);   \
                                                                              \
            /* Save victim */                                                 \
            uint32_t     _vic_fp   = _bk->hash[_pos];                         \
            unsigned     _vic_idx  = _bk->idx [_pos];                         \
            struct type *_vic_node = name##_hptr(base, _vic_idx);             \
            if (!_vic_node) __builtin_unreachable(); /* occupied slot */      \
            /* victim->hash_field holds hash s.t. hash & mask == _cur_bk */   \
            uint32_t _vic_hash = _vic_node->hash_field;                       \
                                                                              \
            /* Place new entry; update its hash_field to match _cur_bk */     \
            _bk->hash[_pos] = _new_fp;                                        \
            _bk->idx [_pos] = _new_idx;                                       \
            { struct type *_np = name##_hptr(base, _new_idx);                 \
              if (!_np) __builtin_unreachable(); /* elm is always valid */     \
              _np->hash_field = _new_hash; }                                  \
                                                                              \
            /* victim's alt bucket and hash (XOR, no rehash) */               \
            unsigned _alt_bk   = (_vic_fp ^ _vic_hash) & mask;                \
            uint32_t _alt_hash = _vic_fp ^ _vic_hash;                         \
                                                                              \
            /* Try to place victim in the alt bucket */                       \
            struct rix_hash_bucket_s *_alt = buckets + _alt_bk;               \
            _rix_hash_prefetch_bucket(_alt);                                  \
            uint32_t _nilm = _RIX_HASH_FIND_U32X16(_alt->hash, 0u);           \
            if (_nilm) {                                                      \
                unsigned _slot    = (unsigned)__builtin_ctz(_nilm);           \
                _alt->hash[_slot] = _vic_fp;                                  \
                _alt->idx [_slot] = _vic_idx;                                 \
                _vic_node->hash_field = _alt_hash; /* update to alt bucket */ \
                head->rhh_nb++;                                               \
                return NULL; /* success */                                    \
            }                                                                 \
            /* Victim becomes the next entry to displace */                   \
            _new_fp   = _vic_fp;                                              \
            _new_idx  = _vic_idx;                                             \
            _new_hash = _alt_hash;                                            \
            _cur_bk   = _alt_bk;                                              \
        }                                                                     \
        /* Kickout exhausted; elm was not inserted */                         \
    }                                                                         \
    return elm; /* table full */                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Insert with cuckoo kickout                                         */      \
/*                                                                    */      \
/* Returns:                                                           */      \
/*   NULL     - inserted successfully                                 */      \
/*   elm      - table full (kickout exhausted)                        */      \
/*   other    - duplicate found, returns the existing node            */      \
/* ================================================================== */      \
attr struct type *                                                            \
name##_insert(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, \
                mask);                                                        \
    return name##_insert_hashed(head, buckets, base, elm, _h);                \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Remove - evict a known node from the table                         */      \
/*                                                                    */      \
/* Takes the node pointer directly (not a key).                       */      \
/* elm->hash_field & mask always gives the current bucket index, so   */      \
/* removal is O(1): find node by idx, then zero-clear hash[slot].     */      \
/* No key comparison, no bk1 fallback.                                */      \
/* Returns elm on success, NULL if elm is not currently in the table. */      \
/* ================================================================== */      \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                           \
                 unsigned bk,                                                 \
                 unsigned slot);                                              \
                                                                              \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    /* hash_field & mask == current bucket (insert invariant) */              \
    unsigned _bk = (unsigned)(elm->hash_field & head->rhh_mask);              \
    struct rix_hash_bucket_s *_b = buckets + _bk;                             \
    uint32_t _hits = _RIX_HASH_FIND_U32X16(_b->idx, (uint32_t)node_idx);      \
    if (_hits) {                                                              \
        unsigned _slot   = (unsigned)__builtin_ctz(_hits);                    \
        if (name##_remove_at(head, buckets, _bk, _slot) !=                    \
            (unsigned)RIX_NIL)                                                \
            return elm;                                                       \
    }                                                                         \
    return NULL; /* not in table */                                           \
}                                                                             \
                                                                              \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                           \
                 unsigned bk,                                                 \
                 unsigned slot)                                               \
{                                                                             \
    struct rix_hash_bucket_s *_b = buckets + bk;                              \
    unsigned _idx;                                                            \
    RIX_ASSERT(slot < RIX_HASH_BUCKET_ENTRY_SZ);                              \
    if (slot >= RIX_HASH_BUCKET_ENTRY_SZ)                                     \
        return (unsigned)RIX_NIL;                                             \
    _idx = _b->idx[slot];                                                     \
    if (_idx == (unsigned)RIX_NIL)                                            \
        return (unsigned)RIX_NIL;                                             \
    _b->hash[slot] = 0u;                                                      \
    _b->idx [slot] = (uint32_t)RIX_NIL;                                       \
    head->rhh_nb--;                                                           \
    return _idx;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Walk - iterate over all occupied slots                             */      \
/*                                                                    */      \
/* cb(node, arg): return 0 to continue, non-zero to stop.             */      \
/* Returns 0 when all entries are visited, or the first non-zero cb   */      \
/* return value.                                                      */      \
/* ================================================================== */      \
attr int                                                                      \
name##_walk(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                                \
            struct type *base,                                                \
            int (*cb)(struct type *, void *),                                 \
            void *arg)                                                        \
{                                                                             \
    unsigned _nb_bk = head->rhh_mask + 1u;                                    \
    for (unsigned _b = 0u; _b < _nb_bk; _b++) {                               \
        struct rix_hash_bucket_s *_bk = buckets + _b;                         \
        for (unsigned _s = 0u; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {         \
            unsigned     _nidx = _bk->idx[_s];                                \
            if (_nidx == (unsigned)RIX_NIL)                                   \
                continue;                                                     \
            struct type *_node = name##_hptr(base, _nidx);                    \
            int   _r    = cb(_node, arg);                                     \
            if (_r)                                                           \
                return _r;                                                    \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}

#  define RIX_HASH_GENERATE_SLOT_INTERNAL(name, type, key_field, hash_field, slot_field, cmp_fn, hash_fn, attr) \
                                                                              \
/* ================================================================== */      \
/* Init                                                               */      \
/* ================================================================== */      \
attr void                                                                     \
name##_init(struct name *head,                                                \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    head->rhh_nb   = 0u;                                                      \
}                                                                             \
                                                                              \
/* ------------------------------------------------------------------ */      \
/* Internal helpers: 1-origin index <-> pointer                       */      \
/* ------------------------------------------------------------------ */      \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p) {                        \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i) {                                  \
    return RIX_PTR_FROM_IDX(base, i);                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find - x1                                                   */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_find_ctx_s *ctx,                              \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets,                            \
                const _RIX_HASH_KEY_TYPE(type, key_field) *key)               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn(key, mask);                                                   \
    unsigned _bk0, _bk1;                                                      \
    uint32_t _fp;                                                             \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    ctx->hash  = _h;                                                          \
    ctx->fp    = _fp;                                                         \
    ctx->key   = (const void *)key;                                           \
    ctx->bk[0] = buckets + _bk0;                                              \
    ctx->bk[1] = buckets + _bk1;                                              \
    _rix_hash_prefetch_bucket(ctx->bk[0]);                                    \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_find_ctx_s *ctx,                               \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_s *buckets __attribute__((unused)))     \
{                                                                             \
    ctx->fp_hits[0] = _RIX_HASH_FIND_U32X16(ctx->bk[0]->hash, ctx->fp);       \
    ctx->fp_hits[1] = 0u;                                                     \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node(struct rix_hash_find_ctx_s *ctx,                         \
                     struct type *base)                                       \
{                                                                             \
    uint32_t _hits = ctx->fp_hits[0];                                         \
    if (_hits) {                                                              \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        unsigned _nidx = ctx->bk[0]->idx[_bit];                               \
        if (_nidx != (unsigned)RIX_NIL)                                       \
            _rix_hash_prefetch_entry(name##_hptr(base, _nidx));               \
    }                                                                         \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_find_ctx_s *ctx,                               \
               struct type *base)                                             \
{                                                                             \
    uint32_t _hits = ctx->fp_hits[0];                                         \
    while (_hits) {                                                           \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        _hits &= _hits - 1u;                                                  \
        unsigned _nidx = ctx->bk[0]->idx[_bit];                               \
        if (_nidx == (unsigned)RIX_NIL) continue;                             \
        struct type *_node = name##_hptr(base, _nidx);                        \
        if (!_node) __builtin_unreachable();                                  \
        if (cmp_fn(&_node->key_field,                                        \
                   (const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key) == 0) \
            return _node;                                                     \
    }                                                                         \
    _hits = _RIX_HASH_FIND_U32X16(ctx->bk[1]->hash, ctx->fp);                 \
    while (_hits) {                                                           \
        unsigned _bit = (unsigned)__builtin_ctz(_hits);                       \
        _hits &= _hits - 1u;                                                  \
        unsigned _nidx = ctx->bk[1]->idx[_bit];                               \
        if (_nidx == (unsigned)RIX_NIL) continue;                             \
        struct type *_node = name##_hptr(base, _nidx);                        \
        if (!_node) __builtin_unreachable();                                  \
        if (cmp_fn(&_node->key_field,                                        \
                   (const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key) == 0) \
            return _node;                                                     \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_find_ctx_s *ctx,                            \
                  unsigned n,                                                 \
                  struct name *head,                                          \
                  struct rix_hash_bucket_s *buckets,                          \
                  const _RIX_HASH_KEY_TYPE(type, key_field) * const *keys)    \
{                                                                             \
    for (unsigned i = 0; i < n; i++)                                          \
        name##_hash_key(&ctx[i], head, buckets, keys[i]);                     \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_find_ctx_s *ctx,                             \
                 unsigned n,                                                  \
                 struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets)                           \
{                                                                             \
    for (unsigned i = 0; i < n; i++)                                          \
        name##_scan_bk(&ctx[i], head, buckets);                               \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_find_ctx_s *ctx,                       \
                       unsigned n,                                            \
                       struct type *base)                                     \
{                                                                             \
    for (unsigned i = 0; i < n; i++)                                          \
        name##_prefetch_node(&ctx[i], base);                                  \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_find_ctx_s *ctx,                             \
                 unsigned n,                                                  \
                 struct type *base,                                           \
                 struct type **results)                                       \
{                                                                             \
    for (unsigned i = 0; i < n; i++)                                          \
        results[i] = name##_cmp_key(&ctx[i], base);                           \
}                                                                             \
                                                                              \
attr struct type *                                                            \
name##_find(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                                \
            struct type *base,                                                \
            const _RIX_HASH_KEY_TYPE(type, key_field) *key)                   \
{                                                                             \
    struct rix_hash_find_ctx_s _ctx;                                          \
    name##_hash_key(&_ctx, head, buckets, key);                               \
    name##_scan_bk(&_ctx, head, buckets);                                     \
    return name##_cmp_key(&_ctx, base);                                       \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_insert_hashed(struct name *head,                                       \
                     struct rix_hash_bucket_s *buckets,                       \
                     struct type *base,                                       \
                     struct type *elm,                                        \
                     union rix_hash_hash_u _h)                                \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    unsigned _bk0, _bk1;                                                      \
    uint32_t _fp;                                                             \
    uint32_t _hits_fp[2];                                                     \
    uint32_t _hits_zero[2];                                                   \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    elm->hash_field = _h.val32[0];                                            \
    _rix_hash_prefetch_bucket(buckets + _bk0);                                \
    if (_bk1 != _bk0)                                                         \
        _rix_hash_prefetch_bucket(buckets + _bk1);                            \
                                                                              \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_s *_bk =                                       \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        _RIX_HASH_FIND_U32X16_2(_bk->hash, _fp, 0u,                           \
                                &_hits_fp[_i], &_hits_zero[_i]);              \
    }                                                                         \
                                                                              \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_s *_bk =                                       \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        uint32_t _hits = _hits_fp[_i];                                        \
        while (_hits) {                                                       \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            _hits &= _hits - 1u;                                              \
            struct type *_node = name##_hptr(base, _bk->idx[_bit]);           \
            if (!_node) __builtin_unreachable();                              \
            if (cmp_fn(&elm->key_field, &_node->key_field) == 0)              \
                return _node;                                                 \
        }                                                                     \
    }                                                                         \
                                                                              \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash_bucket_s *_bk = buckets + _bki;                       \
        uint32_t _nilm = _hits_zero[_i];                                      \
        if (_nilm) {                                                          \
            unsigned _slot = (unsigned)__builtin_ctz(_nilm);                  \
            _bk->hash[_slot] = _fp;                                           \
            _bk->idx[_slot]  = name##_hidx(base, elm);                        \
            if (_i == 1)                                                      \
                elm->hash_field = _h.val32[1];                                \
            elm->slot_field = (_RIX_HASH_SLOT_TYPE(type, slot_field))_slot;   \
            head->rhh_nb++;                                                   \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
                                                                              \
    {                                                                         \
        uint32_t _new_fp   = _fp;                                             \
        unsigned _new_idx  = name##_hidx(base, elm);                          \
        unsigned _cur_bk   = _bk0;                                            \
        uint32_t _new_hash = _h.val32[0];                                     \
                                                                              \
        for (int _d = 0; _d < RIX_HASH_FOLLOW_DEPTH; _d++) {                  \
            struct rix_hash_bucket_s *_bk = buckets + _cur_bk;                \
            unsigned _pos = (unsigned)_d & (RIX_HASH_BUCKET_ENTRY_SZ - 1u);   \
            uint32_t _vic_fp   = _bk->hash[_pos];                             \
            unsigned _vic_idx  = _bk->idx[_pos];                              \
            struct type *_vic_node = name##_hptr(base, _vic_idx);             \
            if (!_vic_node) __builtin_unreachable();                          \
            uint32_t _vic_hash = _vic_node->hash_field;                       \
                                                                              \
            _bk->hash[_pos] = _new_fp;                                        \
            _bk->idx[_pos]  = _new_idx;                                       \
            { struct type *_np = name##_hptr(base, _new_idx);                 \
              if (!_np) __builtin_unreachable();                              \
              _np->hash_field = _new_hash;                                    \
              _np->slot_field = (_RIX_HASH_SLOT_TYPE(type, slot_field))_pos; }\
                                                                              \
            unsigned _alt_bk   = (_vic_fp ^ _vic_hash) & mask;                \
            uint32_t _alt_hash = _vic_fp ^ _vic_hash;                         \
            struct rix_hash_bucket_s *_alt = buckets + _alt_bk;               \
            _rix_hash_prefetch_bucket(_alt);                                  \
            uint32_t _nilm = _RIX_HASH_FIND_U32X16(_alt->hash, 0u);           \
            if (_nilm) {                                                      \
                unsigned _slot = (unsigned)__builtin_ctz(_nilm);              \
                _alt->hash[_slot] = _vic_fp;                                  \
                _alt->idx[_slot]  = _vic_idx;                                 \
                _vic_node->hash_field = _alt_hash;                            \
                _vic_node->slot_field =                                       \
                    (_RIX_HASH_SLOT_TYPE(type, slot_field))_slot;             \
                head->rhh_nb++;                                               \
                return NULL;                                                  \
            }                                                                 \
            _new_fp   = _vic_fp;                                              \
            _new_idx  = _vic_idx;                                             \
            _new_hash = _alt_hash;                                            \
            _cur_bk   = _alt_bk;                                              \
        }                                                                     \
    }                                                                         \
    return elm;                                                               \
}                                                                             \
                                                                              \
attr struct type *                                                            \
name##_insert(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, \
                mask);                                                        \
    return name##_insert_hashed(head, buckets, base, elm, _h);                \
}                                                                             \
                                                                              \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                           \
                 unsigned bk,                                                 \
                 unsigned slot);                                              \
                                                                              \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned _bk = (unsigned)(elm->hash_field & head->rhh_mask);              \
    unsigned _slot = (unsigned)elm->slot_field;                               \
    struct rix_hash_bucket_s *_b = buckets + _bk;                             \
    RIX_ASSERT(_slot < RIX_HASH_BUCKET_ENTRY_SZ);                             \
    if (_slot >= RIX_HASH_BUCKET_ENTRY_SZ)                                    \
        return NULL;                                                          \
    if (_b->idx[_slot] != (uint32_t)node_idx)                                 \
        return NULL;                                                          \
    if (name##_remove_at(head, buckets, _bk, _slot) !=                        \
        (unsigned)RIX_NIL)                                                    \
        return elm;                                                           \
    return NULL;                                                              \
}                                                                             \
                                                                              \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                           \
                 unsigned bk,                                                 \
                 unsigned slot)                                               \
{                                                                             \
    struct rix_hash_bucket_s *_b = buckets + bk;                              \
    unsigned _idx;                                                            \
    RIX_ASSERT(slot < RIX_HASH_BUCKET_ENTRY_SZ);                              \
    if (slot >= RIX_HASH_BUCKET_ENTRY_SZ)                                     \
        return (unsigned)RIX_NIL;                                             \
    _idx = _b->idx[slot];                                                     \
    if (_idx == (unsigned)RIX_NIL)                                            \
        return (unsigned)RIX_NIL;                                             \
    _b->hash[slot] = 0u;                                                      \
    _b->idx [slot] = (uint32_t)RIX_NIL;                                       \
    head->rhh_nb--;                                                           \
    return _idx;                                                              \
}                                                                             \
                                                                              \
attr int                                                                      \
name##_walk(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                                \
            struct type *base,                                                \
            int (*cb)(struct type *, void *),                                 \
            void *arg)                                                        \
{                                                                             \
    for (unsigned _b = 0; _b <= head->rhh_mask; _b++) {                      \
        struct rix_hash_bucket_s *_bk = buckets + _b;                         \
        for (unsigned _s = 0; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {          \
            unsigned _idx = _bk->idx[_s];                                     \
            if (_idx == (unsigned)RIX_NIL) continue;                          \
            struct type *_node = name##_hptr(base, _idx);                     \
            if (!_node) __builtin_unreachable();                              \
            int _rc = cb(_node, arg);                                         \
            if (_rc) return _rc;                                              \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}

/*===========================================================================
 * RIX_HASH_GENERATE_NOHF(name, type, key_field, cmp_fn)
 * RIX_HASH_GENERATE_NOHF_EX(name, type, key_field, cmp_fn, hash_fn)
 *
 * Variant of RIX_HASH_GENERATE without hash_field in the node struct.
 * All find operations are identical to RIX_HASH_GENERATE.
 * insert: no hash_field writes; cuckoo kickout re-hashes victim's key
 *         to locate the alternate bucket (one extra hash call per kickout).
 * remove: re-hashes elm's key to try bk_0 then bk_1 (two scans worst case).
 * Benefit: smaller node struct - no uint32_t hash_field member required.
 *===========================================================================*/
#  define RIX_HASH_NOHF_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, attr) \
    attr void name##_init(struct name *head, unsigned nb_bk);                 \
    attr struct type *name##_insert(struct name *head,                        \
                                    struct rix_hash_bucket_s *buckets,        \
                                    struct type *base,                        \
                                    struct type *elm);                        \
    attr unsigned name##_remove_at(struct name *head,                         \
                                   struct rix_hash_bucket_s *buckets,         \
                                   unsigned bk,                               \
                                   unsigned slot);                            \
    attr struct type *name##_remove(struct name *head,                        \
                                    struct rix_hash_bucket_s *buckets,        \
                                    struct type *base,                        \
                                    struct type *elm);                        \
    attr int name##_walk(struct name *head,                                   \
                         struct rix_hash_bucket_s *buckets,                   \
                         struct type *base,                                   \
                         int (*cb)(struct type *, void *),                    \
                         void *arg);

#  define RIX_HASH_NOHF_PROTOTYPE_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_NOHF_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, )

#  define RIX_HASH_NOHF_PROTOTYPE_STATIC_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_NOHF_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_NOHF_PROTOTYPE(name, type, key_field, cmp_fn) \
    RIX_HASH_NOHF_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, )

#  define RIX_HASH_NOHF_PROTOTYPE_STATIC(name, type, key_field, cmp_fn) \
    RIX_HASH_NOHF_PROTOTYPE_INTERNAL(name, type, key_field, cmp_fn, RIX_UNUSED static)

#  define RIX_HASH_GENERATE_NOHF_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_NOHF_INTERNAL(name, type, key_field, cmp_fn, hash_fn, )

#  define RIX_HASH_GENERATE_NOHF_STATIC_EX(name, type, key_field, cmp_fn, hash_fn) \
    RIX_HASH_GENERATE_NOHF_INTERNAL(name, type, key_field, cmp_fn, hash_fn, RIX_UNUSED static)

#  define RIX_HASH_GENERATE_NOHF(name, type, key_field, cmp_fn)               \
    _RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                   \
    RIX_HASH_GENERATE_NOHF_INTERNAL(name, type, key_field, cmp_fn,            \
                                    _RIX_HASH_DEFAULT_HASH_FN_NAME(name), )

#  define RIX_HASH_GENERATE_NOHF_STATIC(name, type, key_field, cmp_fn)        \
    _RIX_HASH_DEFINE_DEFAULT_HASH_FN(name, type, key_field)                   \
    RIX_HASH_GENERATE_NOHF_INTERNAL(name, type, key_field, cmp_fn,            \
                                    _RIX_HASH_DEFAULT_HASH_FN_NAME(name),     \
                                    RIX_UNUSED static)

#  define RIX_HASH_GENERATE_NOHF_INTERNAL(name, type, key_field, cmp_fn, hash_fn, attr) \
                                                                              \
/* ================================================================== */      \
/* Init                                                               */      \
/* ================================================================== */      \
attr void                                                                     \
name##_init(struct name *head,                                                \
            unsigned nb_bk)                                                   \
{                                                                             \
    head->rhh_mask = nb_bk - 1u;                                              \
    head->rhh_nb   = 0u;                                                      \
}                                                                             \
                                                                              \
/* ------------------------------------------------------------------ */      \
/* Internal helpers: 1-origin index <-> pointer                       */      \
/* ------------------------------------------------------------------ */      \
static RIX_UNUSED RIX_FORCE_INLINE unsigned                                   \
name##_hidx(struct type *base, const struct type *p) {                        \
    return RIX_IDX_FROM_PTR(base, (struct type *)(uintptr_t)p);               \
}                                                                             \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_hptr(struct type *base, unsigned i) {                                  \
    return RIX_PTR_FROM_IDX(base, i);                                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find - x1                                                   */      \
/* ================================================================== */      \
                                                                              \
/* Stage 1: compute hash, resolve bucket pointers, issue prefetches. */       \
/* Must be called before name_scan_bk.                               */       \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key(struct rix_hash_find_ctx_s *ctx,                              \
                struct name *head,                                            \
                struct rix_hash_bucket_s *buckets,                            \
                const _RIX_HASH_KEY_TYPE(type, key_field) *key)               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn(key, mask);                                                   \
    unsigned _bk0, _bk1;                                                      \
    uint32_t _fp;                                                             \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    ctx->hash  = _h;                                                          \
    ctx->fp    = _fp;                                                         \
    ctx->key   = (const void *)key;                                           \
    ctx->bk[0] = buckets + _bk0;                                              \
    ctx->bk[1] = buckets + _bk1;                                              \
    _rix_hash_prefetch_bucket(ctx->bk[0]);                                    \
}                                                                             \
                                                                              \
/* Stage 2: scan bk_0 fingerprints only; produce fp_hits[0] bitmask.  */      \
/* fp != 0 (XOR-based), no _nilm filter needed; fp_hits[1] = 0 here.  */      \
/* Call prefetch_node to hide node-fetch latency before cmp_key.      */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk(struct rix_hash_find_ctx_s *ctx,                               \
               struct name *head __attribute__((unused)),                     \
               struct rix_hash_bucket_s *buckets __attribute__((unused)))     \
{                                                                             \
    ctx->fp_hits[0] = _RIX_HASH_FIND_U32X16(ctx->bk[0]->hash, ctx->fp);       \
    ctx->fp_hits[1] = 0u;                                                     \
}                                                                             \
                                                                              \
/* Stage 3: prefetch node data for all fp_hits positions.             */      \
/* Hides node-fetch DRAM latency; call for all N contexts before cmp_key. */  \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node(struct rix_hash_find_ctx_s *ctx,                         \
                     struct type *base)                                       \
{                                                                             \
    for (int _i = 0; _i < 2; _i++) {                                          \
        uint32_t _hits = ctx->fp_hits[_i];                                    \
        while (_hits) {                                                       \
            unsigned _bit = (unsigned)__builtin_ctz(_hits);                   \
            _hits &= _hits - 1u;                                              \
            _rix_hash_prefetch_entry(                                         \
                name##_hptr(base, ctx->bk[_i]->idx[_bit]));                   \
        }                                                                     \
    }                                                                         \
}                                                                             \
                                                                              \
/* Stage 4: compare keys for bk_0 hits; lazily scan bk_1 on bk_0 miss. */     \
/* fp != 0 (XOR-based), so no _nilm filter is needed in bk_1 scan.     */     \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_cmp_key(struct rix_hash_find_ctx_s *ctx,                               \
               struct type *base)                                             \
{                                                                             \
    uint32_t _hits = ctx->fp_hits[0];                                         \
    while (_hits) {                                                           \
        unsigned      _bit  = (unsigned)__builtin_ctz(_hits);                 \
        _hits &= _hits - 1u;                                                  \
        unsigned      _nidx = ctx->bk[0]->idx[_bit];                          \
        struct type  *_node = name##_hptr(base, _nidx);                       \
        if (cmp_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key,    \
                   &_node->key_field) == 0)                                   \
            return _node;                                                     \
    }                                                                         \
    {                                                                         \
        _hits = _RIX_HASH_FIND_U32X16(ctx->bk[1]->hash, ctx->fp);             \
        while (_hits) {                                                       \
            unsigned      _bit  = (unsigned)__builtin_ctz(_hits);             \
            _hits &= _hits - 1u;                                              \
            unsigned      _nidx = ctx->bk[1]->idx[_bit];                      \
            struct type  *_node = name##_hptr(base, _nidx);                   \
            if (cmp_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)ctx->key, \
                       &_node->key_field) == 0)                               \
                return _node;                                                 \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Staged find - xN bulk                                              */      \
/* FORCE_INLINE + constant n -> compiler unrolls identically to xN.   */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_hash_key_n(struct rix_hash_find_ctx_s *ctx,                            \
                  int n,                                                      \
                  struct name *head,                                          \
                  struct rix_hash_bucket_s *buckets,                          \
                  const _RIX_HASH_KEY_TYPE(type, key_field) * const *keys)    \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_hash_key(&ctx[_j], head, buckets, keys[_j]);                   \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_scan_bk_n(struct rix_hash_find_ctx_s *ctx,                             \
                 int n,                                                       \
                 struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets)                           \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_scan_bk(&ctx[_j], head, buckets);                              \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_prefetch_node_n(struct rix_hash_find_ctx_s *ctx,                       \
                       int n,                                                 \
                       struct type *base)                                     \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        name##_prefetch_node(&ctx[_j], base);                                 \
}                                                                             \
                                                                              \
static RIX_UNUSED RIX_FORCE_INLINE void                                       \
name##_cmp_key_n(struct rix_hash_find_ctx_s *ctx,                             \
                 int n,                                                       \
                 struct type *base,                                           \
                 struct type **results)                                       \
{                                                                             \
    for (int _j = 0; _j < n; _j++)                                            \
        results[_j] = name##_cmp_key(&ctx[_j], base);                         \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Single-shot find  = hash_key + scan_bk + cmp_key                   */      \
/* ================================================================== */      \
static RIX_UNUSED RIX_FORCE_INLINE struct type *                              \
name##_find(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                                \
            struct type *base,                                                \
            const _RIX_HASH_KEY_TYPE(type, key_field) *key)                   \
{                                                                             \
    struct rix_hash_find_ctx_s _ctx;                                          \
    name##_hash_key(&_ctx, head, buckets, key);                               \
    name##_scan_bk (&_ctx, head, buckets);                                    \
    return name##_cmp_key(&_ctx, base);                                       \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Insert with cuckoo kickout (no hash_field)                         */      \
/*                                                                    */      \
/* Returns:                                                           */      \
/*   NULL     - inserted successfully                                 */      \
/*   elm      - table full (kickout exhausted)                        */      \
/*   other    - duplicate found, returns the existing node            */      \
/* ================================================================== */      \
attr struct type *                                                            \
name##_insert(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, \
                mask);                                                        \
    unsigned _bk0, _bk1;                                                      \
    uint32_t _fp;                                                             \
    uint32_t _hits_fp[2];                                                     \
    uint32_t _hits_zero[2];                                                   \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    for (int _i = 0; _i < 2; _i++) {                                          \
        struct rix_hash_bucket_s *_bk =                                       \
            buckets + (_i == 0 ? _bk0 : _bk1);                                \
        _RIX_HASH_FIND_U32X16_2(_bk->hash, _fp, 0u,                           \
                                &_hits_fp[_i], &_hits_zero[_i]);              \
        uint32_t _hits = _hits_fp[_i];                                        \
        while (_hits) {                                                       \
            unsigned      _bit  = (unsigned)__builtin_ctz(_hits);             \
            _hits &= _hits - 1u;                                              \
            struct type  *_node = name##_hptr(base, _bk->idx[_bit]);          \
            if (cmp_fn(&elm->key_field, &_node->key_field) == 0)              \
                return _node;                                                 \
        }                                                                     \
    }                                                                         \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash_bucket_s *_bk = buckets + _bki;                       \
        uint32_t _nilm = _hits_zero[_i];                                      \
        if (_nilm) {                                                          \
            unsigned _slot   = (unsigned)__builtin_ctz(_nilm);                \
            _bk->hash[_slot] = _fp;                                           \
            _bk->idx [_slot] = name##_hidx(base, elm);                        \
            head->rhh_nb++;                                                   \
            return NULL;                                                      \
        }                                                                     \
    }                                                                         \
    {                                                                         \
        uint32_t _new_fp   = _fp;                                             \
        unsigned _new_idx  = name##_hidx(base, elm);                          \
        unsigned _cur_bk   = _bk0;                                            \
        for (int _d = 0; _d < RIX_HASH_FOLLOW_DEPTH; _d++) {                  \
            struct rix_hash_bucket_s *_bk = buckets + _cur_bk;                \
            unsigned _pos = (unsigned)_d & (RIX_HASH_BUCKET_ENTRY_SZ - 1u);   \
            uint32_t     _vic_fp   = _bk->hash[_pos];                         \
            unsigned     _vic_idx  = _bk->idx [_pos];                         \
            struct type *_vic_node = name##_hptr(base, _vic_idx);             \
            _bk->hash[_pos] = _new_fp;                                        \
            _bk->idx [_pos] = _new_idx;                                       \
            /* Re-hash victim's key to find alternate bucket */               \
            union rix_hash_hash_u _vic_h =                                    \
                hash_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)         \
                        &_vic_node->key_field, mask);                         \
            unsigned _vic_bk0 = _vic_h.val32[0] & mask;                       \
            unsigned _vic_bk1 = _vic_h.val32[1] & mask;                       \
            unsigned _alt_bk  = (_cur_bk == _vic_bk0) ? _vic_bk1 : _vic_bk0;  \
            struct rix_hash_bucket_s *_alt = buckets + _alt_bk;               \
            uint32_t _nilm = _RIX_HASH_FIND_U32X16(_alt->hash, 0u);           \
            if (_nilm) {                                                      \
                unsigned _slot    = (unsigned)__builtin_ctz(_nilm);           \
                _alt->hash[_slot] = _vic_fp;                                  \
                _alt->idx [_slot] = _vic_idx;                                 \
                head->rhh_nb++;                                               \
                return NULL;                                                  \
            }                                                                 \
            _new_fp  = _vic_fp;                                               \
            _new_idx = _vic_idx;                                              \
            _cur_bk  = _alt_bk;                                               \
        }                                                                     \
    }                                                                         \
    return elm;                                                               \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Remove (re-hash key; scan bk_0 then bk_1)                          */      \
/* ================================================================== */      \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                           \
                 unsigned bk,                                                 \
                 unsigned slot);                                              \
                                                                              \
attr struct type *                                                            \
name##_remove(struct name *head,                                              \
              struct rix_hash_bucket_s *buckets,                              \
              struct type *base,                                              \
              struct type *elm)                                               \
{                                                                             \
    unsigned node_idx = name##_hidx(base, elm);                               \
    unsigned mask = head->rhh_mask;                                           \
    union rix_hash_hash_u _h =                                                \
        hash_fn((const _RIX_HASH_KEY_TYPE(type, key_field) *)&elm->key_field, \
                mask);                                                        \
    unsigned _bk0, _bk1;                                                      \
    uint32_t _fp;                                                             \
    _rix_hash_buckets(_h, mask, &_bk0, &_bk1, &_fp);                          \
    for (int _i = 0; _i < 2; _i++) {                                          \
        unsigned _bki = (_i == 0) ? _bk0 : _bk1;                              \
        struct rix_hash_bucket_s *_b = buckets + _bki;                        \
        uint32_t _hits = _RIX_HASH_FIND_U32X16(_b->idx,                       \
                                               (uint32_t)node_idx);           \
        if (_hits) {                                                          \
            unsigned _slot   = (unsigned)__builtin_ctz(_hits);                \
            if (name##_remove_at(head, buckets, _bki, _slot) !=               \
                (unsigned)RIX_NIL)                                            \
                return elm;                                                   \
        }                                                                     \
    }                                                                         \
    return NULL;                                                              \
}                                                                             \
                                                                              \
attr unsigned                                                                 \
name##_remove_at(struct name *head,                                           \
                 struct rix_hash_bucket_s *buckets,                           \
                 unsigned bk,                                                 \
                 unsigned slot)                                               \
{                                                                             \
    struct rix_hash_bucket_s *_b = buckets + bk;                              \
    unsigned _idx;                                                            \
    RIX_ASSERT(slot < RIX_HASH_BUCKET_ENTRY_SZ);                              \
    if (slot >= RIX_HASH_BUCKET_ENTRY_SZ)                                     \
        return (unsigned)RIX_NIL;                                             \
    _idx = _b->idx[slot];                                                     \
    if (_idx == (unsigned)RIX_NIL)                                            \
        return (unsigned)RIX_NIL;                                             \
    _b->hash[slot] = 0u;                                                      \
    _b->idx [slot] = (uint32_t)RIX_NIL;                                       \
    head->rhh_nb--;                                                           \
    return _idx;                                                              \
}                                                                             \
                                                                              \
/* ================================================================== */      \
/* Walk                                                               */      \
/* ================================================================== */      \
attr int                                                                      \
name##_walk(struct name *head,                                                \
            struct rix_hash_bucket_s *buckets,                                \
            struct type *base,                                                \
            int (*cb)(struct type *, void *),                                 \
            void *arg)                                                        \
{                                                                             \
    unsigned _nb_bk = head->rhh_mask + 1u;                                    \
    for (unsigned _b = 0u; _b < _nb_bk; _b++) {                               \
        struct rix_hash_bucket_s *_bk = buckets + _b;                         \
        for (unsigned _s = 0u; _s < RIX_HASH_BUCKET_ENTRY_SZ; _s++) {         \
            unsigned     _nidx = _bk->idx[_s];                                \
            if (_nidx == (unsigned)RIX_NIL)                                   \
                continue;                                                     \
            struct type *_node = name##_hptr(base, _nidx);                    \
            int   _r    = cb(_node, arg);                                     \
            if (_r)                                                           \
                return _r;                                                    \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}

/*===========================================================================
 * Sizing helper
 *
 * rix_hash_nb_bk_hint - recommended nb_bk for a target ~50% slot fill.
 *
 * Each bucket holds RIX_HASH_BUCKET_ENTRY_SZ (16) slots.
 * For 50% fill: total_slots = nb_bk * 16 = 2 * max_entries
 *               => nb_bk = ceil(max_entries / 8), rounded up to power of 2.
 * nb_bk must be a power of 2 (used as a bitmask inside the hash table).
 * Minimum returned value is 2.
 *===========================================================================*/
static inline unsigned
rix_hash_nb_bk_hint(unsigned max_entries)
{
    unsigned n = (max_entries + 7u) / 8u;   /* ceil(max_entries / 8) */
    if (n < 2u)
        n = 2u;
    /* round up to next power of 2 */
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1u;
}

/*===========================================================================
 * Convenience macro API
 *
 * Wraps the generated name##_xxx() functions with BSD queue-style macros
 * that embed the table-name prefix explicitly.  The name argument must
 * match the tag passed to RIX_HASH_HEAD / RIX_HASH_GENERATE.
 *
 * Single-shot ops:
 *   RIX_HASH_INIT        (name, head, nb_bk)
 *   RIX_HASH_FIND        (name, head, buckets, base, key)
 *   RIX_HASH_INSERT      (name, head, buckets, base, elm)
 *   RIX_HASH_REMOVE      (name, head, buckets, base, elm)
 *   RIX_HASH_WALK        (name, head, buckets, base, cb, arg)
 *
 * Staged find - x1:
 *   RIX_HASH_HASH_KEY    (name, ctx, head, buckets, key) key: const key_type *
 *   RIX_HASH_SCAN_BK     (name, ctx, head, buckets)
 *   RIX_HASH_PREFETCH_NODE(name, ctx, base)
 *   RIX_HASH_CMP_KEY     (name, ctx, base)
 *
 * Staged find - x2 / x4  (named shorthands; expand to _n internally):
 *   RIX_HASH_HASH_KEY2   (name, ctx, head, buckets, keys)
 *   RIX_HASH_SCAN_BK2    (name, ctx, head, buckets)
 *   RIX_HASH_PREFETCH_NODE2(name, ctx, base)
 *   RIX_HASH_CMP_KEY2    (name, ctx, base, results)
 *
 *   RIX_HASH_HASH_KEY4   (name, ctx, head, buckets, keys)
 *   RIX_HASH_SCAN_BK4    (name, ctx, head, buckets)
 *   RIX_HASH_PREFETCH_NODE4(name, ctx, base)
 *   RIX_HASH_CMP_KEY4    (name, ctx, base, results)
 *
 * Staged find - xN  (arbitrary n; FORCE_INLINE + constant n -> unrolled):
 *   RIX_HASH_HASH_KEY_N  (name, ctx, n, head, buckets, keys)
 *   RIX_HASH_SCAN_BK_N   (name, ctx, n, head, buckets)
 *   RIX_HASH_PREFETCH_NODE_N(name, ctx, n, base)
 *   RIX_HASH_CMP_KEY_N   (name, ctx, n, base, results)
 *===========================================================================*/

/* ---- single-shot ops ---------------------------------------------------- */
#  define RIX_HASH_INIT(name, head, nb_bk)                                      \
    name##_init(head, nb_bk)

#  define RIX_HASH_FIND(name, head, buckets, base, key)                         \
    name##_find(head, buckets, base, key)

#  define RIX_HASH_INSERT(name, head, buckets, base, elm)                       \
    name##_insert(head, buckets, base, elm)

#  define RIX_HASH_REMOVE(name, head, buckets, base, elm)                       \
    name##_remove(head, buckets, base, elm)

#  define RIX_HASH_REMOVE_AT(name, head, buckets, bk, slot)                     \
    name##_remove_at(head, buckets, bk, slot)

#  define RIX_HASH_WALK(name, head, buckets, base, cb, arg)                     \
    name##_walk(head, buckets, base, cb, arg)

/* ---- staged find - x1 --------------------------------------------------- */
#  define RIX_HASH_HASH_KEY(name, ctx, head, buckets, key)                      \
    name##_hash_key(ctx, head, buckets, key)

#  define RIX_HASH_SCAN_BK(name, ctx, head, buckets)                            \
    name##_scan_bk(ctx, head, buckets)

#  define RIX_HASH_PREFETCH_NODE(name, ctx, base)                               \
    name##_prefetch_node(ctx, base)

#  define RIX_HASH_CMP_KEY(name, ctx, base)                                     \
    name##_cmp_key(ctx, base)

/* ---- staged find - x2 --------------------------------------------------- */
#  define RIX_HASH_HASH_KEY2(name, ctx, head, buckets, keys)                    \
    name##_hash_key_n(ctx, 2, head, buckets, keys)

#  define RIX_HASH_SCAN_BK2(name, ctx, head, buckets)                           \
    name##_scan_bk_n(ctx, 2, head, buckets)

#  define RIX_HASH_PREFETCH_NODE2(name, ctx, base)                              \
    name##_prefetch_node_n(ctx, 2, base)

#  define RIX_HASH_CMP_KEY2(name, ctx, base, results)                           \
    name##_cmp_key_n(ctx, 2, base, results)

/* ---- staged find - x4 --------------------------------------------------- */
#  define RIX_HASH_HASH_KEY4(name, ctx, head, buckets, keys)                    \
    name##_hash_key_n(ctx, 4, head, buckets, keys)

#  define RIX_HASH_SCAN_BK4(name, ctx, head, buckets)                           \
    name##_scan_bk_n(ctx, 4, head, buckets)

#  define RIX_HASH_PREFETCH_NODE4(name, ctx, base)                              \
    name##_prefetch_node_n(ctx, 4, base)

#  define RIX_HASH_CMP_KEY4(name, ctx, base, results)                           \
    name##_cmp_key_n(ctx, 4, base, results)

/* ---- staged find - xN (arbitrary n) ------------------------------------- */
#  define RIX_HASH_HASH_KEY_N(name, ctx, n, head, buckets, keys)                \
    name##_hash_key_n(ctx, n, head, buckets, keys)

#  define RIX_HASH_SCAN_BK_N(name, ctx, n, head, buckets)                       \
    name##_scan_bk_n(ctx, n, head, buckets)

#  define RIX_HASH_PREFETCH_NODE_N(name, ctx, n, base)                          \
    name##_prefetch_node_n(ctx, n, base)

#  define RIX_HASH_CMP_KEY_N(name, ctx, n, base, results)                       \
    name##_cmp_key_n(ctx, n, base, results)

#endif /* _RIX_HASH_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
