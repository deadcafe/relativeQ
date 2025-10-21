#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sched.h>

#include "fcache.h"

#ifndef ARRAYOF
# define ARRAYOF(_a)     (sizeof(_a)/sizeof(_a[0]))
#endif

#ifndef SWAP
# define SWAP(a,b) do { typeof(a) _c = (a); (a) = (b); (b) = _c;} while (0)
#endif

static bool Enable_SSE41  = true;
static bool Enable_SSE42  = true;
static bool Enable_AVX2   = true;
static bool Enable_AVX512 = true;

/*
 * Arch handler
 */
typedef uint64_t (*find_idx_in_bucket_t)(const struct flow_cache_s *,
                                         const struct flow_bucket_s *,
                                         uint32_t);
typedef uint64_t (*find_hval_in_bucket_single_t)(const struct flow_cache_s *,
                                                 const struct flow_bucket_s *,
                                                 uint32_t);
typedef void (*find_hval_in_bucket_double_t)(const struct flow_cache_s *,
                                             struct flow_pipeline_ctx_s *);

typedef int (*cmp_flow_key_t)(struct flow_cache_s *, const struct flow_key_s *, const struct flow_key_s *);
typedef int (*debug_fprintf_t)(FILE *stream, const char *format, ...);


#if !defined(ENABLE_TRACER)
static int
null_fprintf(FILE *stream __attribute__((unused)),
             const char *format __attribute__((unused)),
             ...)
{
    return 0;
}

static debug_fprintf_t debug_fprintf = null_fprintf;

always_inline void
set_debug_handler(const struct flow_cache_s *fcache)
{
    if (fcache->is_debug)
        debug_fprintf = fprintf;
}

always_inline void
cls_debug_handler(const struct flow_cache_s *fcache)
{
    if (fcache->is_debug)
        debug_fprintf = null_fprintf;
}

# define TRACER(fmt,...)        debug_fprintf(stderr, "%s():%d " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
# define TRACER(fmt,...)

//static debug_fprintf_t debug_fprintf = null_fprintf;

always_inline void
set_debug_handler(const struct flow_cache_s *fcache __attribute__((unused)))
{
    ;
}

always_inline void
cls_debug_handler(const struct flow_cache_s *fcache __attribute__((unused)))
{
    ;
}
#endif

/*******************************************************************************
 * lower
 ******************************************************************************/
/*
 *
 */
always_inline void
prefetch(const void *p,
         int locality)
{
    __builtin_prefetch(p, 0, locality);    /* non temporal */
}

/*
 * Read Timestamp Counter (x86 only)
 */
always_inline uint64_t
rdtsc(void)
{
    return __builtin_ia32_rdtsc();
}

/*******************************************************************************
 * IDX Pool
 ******************************************************************************/
/*
 *
 */
always_inline size_t
idx_pool_sizeof(unsigned nb)
{
    size_t sz = sizeof(unsigned) * nb;

    return sz;
}

/*
 * Hash Read Method
 */
always_inline uint32_t
hash2val(const union hash_u hash)
{
    return hash.val32[0] ^ hash.val32[1];
}

/*
 * Hash to bucket index (Even/Odd)
 */
always_inline uint32_t
hash2idx(const struct flow_cache_s *fcache,
         const union hash_u hash,
         int even_odd)
{
    return (hash.val32[even_odd] & fcache->bk_mask);
}

/*
 *
 */
always_inline unsigned
bucket_idx(const struct flow_cache_s *fcache,
           const struct flow_bucket_s *bk)
{
    if (!bk)
        return INVALID_IDX;
    return bk - fcache->buckets;
}

/*
 *
 */
always_inline struct flow_bucket_s *
bucket_ptr(const struct flow_cache_s *fcache,
           uint32_t idx)
{
    if (idx == INVALID_IDX)
        return NULL;
    return &fcache->buckets[idx];
}

always_inline struct flow_bucket_s *
fetch_bucket(const struct flow_cache_s *fcache,
             uint32_t idx)
{
    struct flow_bucket_s *bk = bucket_ptr(fcache, idx);
    if (bk) {
        prefetch(bk->hval, 3);
        prefetch(bk->idx, 3);
    }
    return bk;
}

/*
 * current bucket index to another index
 */
always_inline struct flow_bucket_s *
fetch_another_bucket(const struct flow_cache_s *fcache,
                     const struct flow_bucket_s *bk,
                     unsigned pos)
{
    uint32_t hval = bk->hval[pos];
    uint32_t idx = ((bucket_idx(fcache, bk) ^ hval) & fcache->bk_mask);

    return fetch_bucket(fcache, idx);
}

/*
 *
 */
always_inline uint32_t
node_idx(const struct flow_cache_s *fcache,
         const struct flow_node_s *node)
{
    if (!node)
        return IDXQ_NULL;
    return node - fcache->nodes;
}

/*
 *
 */
always_inline struct flow_node_s *
node_ptr(const struct flow_cache_s *fcache,
         uint32_t idx)
{
    if (idx == IDXQ_NULL)
        return NULL;
    return &fcache->nodes[idx];
}

/*
 *
 */
always_inline struct flow_node_s *
fetch_node(const struct flow_cache_s *fcache,
           const struct flow_bucket_s *bk,
           unsigned pos)
{
    struct flow_node_s *n = node_ptr(fcache, bk->idx[pos]);
    if (n)
        prefetch(n, 0);
    return n;
}

/*
 *
 */
always_inline struct flow_key_s *
fetch_bk_key(const struct flow_cache_s *fcache,
             const struct flow_bucket_s *bk,
             unsigned pos)
{
    struct flow_key_s *key;
    struct flow_node_s *node = node_ptr(fcache, bk->idx[pos]);

    if (node) {
        key = &node->key;
        prefetch(key, 1);
    } else {
        key = NULL;
    }
    return key;
}

/*
 *
 */
always_inline void
set_key(struct flow_node_s *node,
        const struct flow_key_s *key,
        union hash_u hash)
{
    if (node) {
        memcpy(&node->key, key, sizeof(*key));
        node->key.hash = hash;
    }
}

/*
 *
 */
always_inline union hash_u
fetch_hash(const struct flow_node_s *node)
{
    if (node)
        return node->key.hash;
    return (union hash_u) INVALID_HASH64;
}

/*
 *
 */
always_inline void
set_bucket(struct flow_bucket_s *bk,
           unsigned pos,
           uint32_t node_idx,
           uint32_t hval)
{
    bk->hval[pos] = hval;
    bk->idx[pos] = node_idx;

    /* XXX */
}

/*
 *
 */
always_inline void
clear_bucket(struct flow_bucket_s *bk,
             unsigned pos)
{
    bk->idx[pos] = INVALID_IDX;
    bk->hval[pos] = INVALID_HVAL;

    /* XXX */
}

always_inline void
move_bucket(struct flow_bucket_s *dst,
            unsigned dpos,
            struct flow_bucket_s *src,
            unsigned spos)
{
    dst->hval[dpos] = src->hval[spos];
    dst->idx[dpos] = src->idx[spos];
    src->hval[spos] = INVALID_HVAL;
    src->idx[spos] = INVALID_IDX;

    /* XXX */
}

/*
 *
 */
always_inline void
idx_pool_prefetch(const struct flow_cache_s *fcache)
{
    prefetch(&fcache->idx_pool->idx_array[fcache->idx_pool->nb_used], 1);
    (void) fcache;
}

always_inline void
idx_pool_next_prefetch(const struct flow_cache_s *fcache,
                       unsigned nb)
{
    unsigned top = fcache->idx_pool->nb_used;
    unsigned tail = top + nb;

    if (tail >= fcache->idx_pool->array_size)
        tail = fcache->idx_pool->array_size;

    for (unsigned i = top; i < tail; i++) {
        unsigned idx = fcache->idx_pool->idx_array[i];

        prefetch(node_ptr(fcache, idx), 1);
    }
}

/*
 * prefetch neighbor node in used node queue
 */
always_inline void
prefetch_neighbor(const struct flow_cache_s *fcache,
                  const struct flow_node_s *node)
{
#if 0
    struct flow_node_s *p;

    p = IDXQ_NEXT(&fcache->idx_pool->used_fifo, node, entry);
    prefetch_node(p);

    p = IDXQ_PREV(&fcache->idx_pool->used_fifo, node, entry);
    prefetch_node(p);
#else
    (void) fcache;
    (void) node;
#endif
}

/*
 *
 */
always_inline void
prefetch_node_in_bucket(const struct flow_cache_s *fcache,
                        const struct flow_bucket_s *bk,
                        uint64_t hits)
{
#if 1
    if (hits) {
        unsigned pos = __builtin_ctzll(hits);

        for (hits >>= pos; hits; pos++, hits >>= 1) {
            if (hits & 1) {
#if 1
                fetch_node(fcache, bk, pos);
#else
                (void) bk;
                (void) pos;
                (void) fcache;
#endif
            }
        }
    }
#else
    (void) bk;
    (void) hits;
    (void) fcache;
#endif
}

#define BUCKET_DRIVER_GENERATE(name)                            	\
static inline uint64_t                                  	        \
name##_find_idx_in_bucket(const struct flow_cache_s *fcache,   	        \
                          const struct flow_bucket_s * bk,              \
                          uint32_t idx)                                 \
{                                                                       \
    (void) fcache;                                                      \
    TRACER("bk:%u\n"                                                    \
           "    %08x %08x %08x %08x %08x %08x %08x %08x\n"              \
           "    %08x %08x %08x %08x %08x %08x %08x %08x\n"              \
           "idx:%08x\n",                                                \
           bucket_idx(fcache, bk),                                      \
           bk->idx[0], bk->idx[1], bk->idx[2], bk->idx[3],              \
           bk->idx[4], bk->idx[5], bk->idx[6], bk->idx[7],              \
           bk->idx[8], bk->idx[9], bk->idx[10], bk->idx[11],            \
           bk->idx[12], bk->idx[13], bk->idx[14], bk->idx[15],          \
           idx);                                                        \
    uint64_t flags = name##_find_32x16(bk->idx, idx);                   \
    TRACER("flags:%04x\n", flags);                                      \
    return flags;                                                       \
}                                                                       \
static inline uint64_t                                                  \
name##_find_hval_in_bucket_single(const struct flow_cache_s *fcache,    \
                                  const struct flow_bucket_s *bk,       \
                                  uint32_t hval)                        \
{                                                                       \
    (void) fcache;                                                      \
    TRACER("bk:%u\n"                                                    \
           "    %08x %08x %08x %08x %08x %08x %08x %08x\n"              \
           "    %08x %08x %08x %08x %08x %08x %08x %08x\n"              \
           "hval:%08x\n",                                               \
           bucket_idx(fcache, bk),                                      \
           bk->hval[0], bk->hval[1], bk->hval[2], bk->hval[3],          \
           bk->hval[4], bk->hval[5], bk->hval[6], bk->hval[7],          \
           bk->hval[8], bk->hval[9], bk->hval[10], bk->hval[11],        \
           bk->hval[12], bk->hval[13], bk->hval[14], bk->hval[15],      \
           hval);                                                       \
    uint64_t flags = name##_find_32x16(bk->hval, hval);                 \
    TRACER("flags:%04x\n", flags);                                      \
    return flags;                                                       \
}                                                                       \
static inline void                                                      \
name##_find_hval_in_bucket_double(const struct flow_cache_s *fcache,    \
                                  struct flow_pipeline_ctx_s *ctx)      \
{                                                                       \
    ctx->bk[0].hits = name##_find_hval_in_bucket_single(fcache, ctx->bk[0].ptr, hash2val(ctx->hash)); \
    ctx->bk[1].hits = name##_find_hval_in_bucket_single(fcache, ctx->bk[1].ptr, hash2val(ctx->hash)); \
    prefetch_node_in_bucket(fcache, ctx->bk[0].ptr, ctx->bk[0].hits);	\
    prefetch_node_in_bucket(fcache, ctx->bk[1].ptr, ctx->bk[1].hits);   \
}                                                                               \


/*****************************************************************************
 * default Handler -->
 *****************************************************************************/
always_inline uint64_t
GENERIC_find_32x16(const uint32_t *array,
                   uint32_t val)
{
    uint64_t hits = 0;

    for (unsigned pos = 0; pos < 16; pos++) {
        if (array[pos] == val)
            hits |= (1 << pos);
    }
    return hits;
}

/*
 *
 */
static inline uint32_t
murmurhash3_32(const uint32_t *blocks,
               unsigned nblocks,
               uint32_t seed)
{
    uint32_t c1 = 0xcc9e2d51;
    uint32_t c2 = 0x1b873593;
    uint32_t r1 = 15;
    uint32_t r2 = 13;
    uint32_t m = 5;
    uint32_t n = 0xe6546b64;
    uint32_t hash = seed;

    for (unsigned i = 0; i < nblocks; i++) {
        uint32_t k = blocks[i];
        k *= c1;
        k = (k << r1) | (k >> (32 - r1));
        k *= c2;

        hash ^= k;
        hash = ((hash << r2) | (hash >> (32 - r2))) * m + n;
    }

    hash ^= (nblocks * 4);
    hash ^= (hash >> 16);
    hash *= 0x85ebca6b;
    hash ^= (hash >> 13);
    hash *= 0xc2b2ae35;
    hash ^= (hash >> 16);

    return hash;
}

/*
 *
 */
static inline union hash_u
GENERIC_hash_func(const struct flow_key_s *key,
                  uint32_t mask)
{
    union hash_u hash;

    hash.val32[0] = 0;
    hash.val32[1] = 0xdeadbeef;

    for (unsigned i = 0; i < ARRAYOF(key->val.d32); i+=2) {
        hash.val32[0] = murmurhash3_32(&key->val.d32[i], 2, hash.val32[0]);
        hash.val32[1] = murmurhash3_32(&hash.val32[0], 1, hash.val32[1]);
    }

    while (((hash.val32[1] & mask) == (hash.val32[0] & mask)) ||
           ((hash.val32[0] ^ hash.val32[1]) == INVALID_HVAL)) {
        uint32_t h = __builtin_bswap32(hash.val32[1]);

        h = ~murmurhash3_32(hash.val32, 2, h);
        hash.val32[1] = h ^ hash.val32[0];
    }

    return hash;
}

/*
 *
 */
static inline int
GENERIC_cmp_flow_key(struct flow_cache_s *fcache,
                     const struct flow_key_s *dst,
                     const struct flow_key_s *src)
{
    int ret;
    uint64_t tsc = rdtsc();

    ret = memcmp(&dst->val, &src->val, sizeof(dst->val));
    fcache->cmp_cnt += 1;
    fcache->cmp_tsc += rdtsc() - tsc;
    return ret;
}

BUCKET_DRIVER_GENERATE(GENERIC);

/*****************************************************************************
 * <-- default Handler
 *****************************************************************************/

#if defined(__x86_64__)
#include <x86intrin.h>
#include <cpuid.h>

/*****************************************************************************
 * SSE4.1 depened code -->
 *****************************************************************************/
#if defined(__SSE4_1__)

/*
 *
 */
always_inline uint64_t
SSE41_cmp_flag(const __m128i *hash,
               const __m128i key)
{
    uint64_t flags = 0;

    for (unsigned i = 0; i < 4; i++) {
        __m128i cmp_result = _mm_cmpeq_epi32(key, hash[i]);
        flags |= _mm_movemask_ps(_mm_castsi128_ps(cmp_result)) << (i * 4);
    }
    return flags;
}

/*
 *
 */
always_inline uint64_t
SSE41_find_32x16(const uint32_t *array,
                 uint32_t val)
{
    __m128i target[4];
    __m128i key = _mm_set1_epi32(val);

    for (unsigned i = 0; i < ARRAYOF(target); i++)
        target[i] = _mm_load_si128((__m128i *) (volatile void *) &array[i * 4]);
    return SSE41_cmp_flag(target, key);
}

/*
 *
 */
static inline int
SSE41_cmp_flow_key(struct flow_cache_s *fcache,
                   const struct flow_key_s *a,
                   const struct flow_key_s *b)
{
    const __m128i * ptr_a = (const __m128i *) a;
    const __m128i * ptr_b = (const __m128i *) b;
    unsigned mask = 0xFFFF;

    TRACER("a:%p b:%p\n", a, b);

    uint64_t tsc = rdtsc();
    for (unsigned i = 0; i < 3; i++) {
        __m128i chunk1_a = _mm_loadu_si128(&ptr_a[i]);
        __m128i chunk1_b = _mm_loadu_si128(&ptr_b[i]);
        __m128i result1 = _mm_cmpeq_epi8(chunk1_a, chunk1_b);

        mask &= _mm_movemask_epi8(result1);
    }
    fcache->cmp_cnt += 1;
    fcache->cmp_tsc += rdtsc() - tsc;

    TRACER("mask:%08x\n", mask);
    return (mask != 0xFFFF);
}

BUCKET_DRIVER_GENERATE(SSE41);

#endif
/*****************************************************************************
 * <-- SSE4.1 depened code
 *****************************************************************************/

/*****************************************************************************
 * SSE4.2 depened code -->
 *****************************************************************************/
#if defined(__SSE4_2__)
/*
 *
 */
static inline union hash_u
SSE42_calc_hash(const struct flow_key_s *key,
                uint32_t mask)
{
    union hash_u hash;

    hash.val32[0] = 0;
    hash.val32[1] = 0xdeadbeef;

    for (unsigned i = 0; i < ARRAYOF(key->val.d64); i++) {
        hash.val32[0] = _mm_crc32_u64(hash.val32[0], key->val.d64[i]);
        hash.val32[1] = _mm_crc32_u32(hash.val32[1], hash.val32[0]);
    }

    while (((hash.val32[1] & mask) == (hash.val32[0] & mask)) ||
           ((hash.val32[0] ^ hash.val32[1]) == INVALID_HVAL)) {
        uint32_t h = __builtin_bswap32(hash.val32[1]);

        h = ~_mm_crc32_u64(h, hash.val64);
        hash.val32[1] = h ^ hash.val32[0];
    }
    return hash;
}
#endif
/*****************************************************************************
 * <--- SSE4.2 depened code
 *****************************************************************************/

/*****************************************************************************
 * AVX2 depened code -->
 *****************************************************************************/
#if defined(__AVX2__)
/*
 *
 */
always_inline uint64_t
AVX2_cmp_flag(const __m256i hash_lo,
              const __m256i hash_hi,
              const __m256i key)
{
    uint64_t mask_lo = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(key, hash_lo)));
    uint64_t mask_hi = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(key, hash_hi)));
    return (mask_hi << 8) | mask_lo;
}

/*
 *
 */
always_inline uint64_t
AVX2_find_32x16(const uint32_t *array,
                uint32_t val)
{
    return AVX2_cmp_flag(_mm256_load_si256((__m256i *) (volatile void *) &array[0]),
                         _mm256_load_si256((__m256i *) (volatile void *) &array[8]),
                         _mm256_set1_epi32(val));
}

/*
 *
 */
static inline int
AVX2_cmp_flow_key(struct flow_cache_s *fcache,
                  const struct flow_key_s *a,
                  const struct flow_key_s *b)
{
    const __m256i * ptr_a = (const __m256i *) a;
    const __m256i * ptr_b = (const __m256i *) b;

    TRACER("a:%p b:%p\n", a, b);

    uint64_t tsc = rdtsc();
    __m256i chunk1_a = _mm256_loadu_si256(ptr_a);
    __m256i chunk1_b = _mm256_loadu_si256(ptr_b);
    __m256i result1 = _mm256_cmpeq_epi8(chunk1_a, chunk1_b);
    int mask1 = _mm256_movemask_epi8(result1);

    const __m128i * ptr_a128 = (const __m128i *) (ptr_a + 1);
    const __m128i * ptr_b128 = (const __m128i *) (ptr_b + 1);
    __m128i chunk2_a = _mm_loadu_si128(ptr_a128);
    __m128i chunk2_b = _mm_loadu_si128(ptr_b128);
    __m128i result2 = _mm_cmpeq_epi8(chunk2_a, chunk2_b);
    int mask2 = _mm_movemask_epi8(result2);

    fcache->cmp_cnt += 1;
    fcache->cmp_tsc += rdtsc() - tsc;

    TRACER("mask1:%08x mask2:%08x\n", mask1, mask2);
    return !((mask1 == -1) && (mask2 == 0xFFFF));
}

BUCKET_DRIVER_GENERATE(AVX2);

#endif /* __AVX2__ */

/*****************************************************************************
 * <--- AVX2 depened code
 *****************************************************************************/

/*****************************************************************************
 * AVX512 depened code -->
 *****************************************************************************/
#if defined(__AVX512F__)
/*
 *
 */
always_inline uint64_t
AVX512_cmp_flag(__m512i hash,
                __m512i key)
{
    return _mm512_cmpeq_epi32_mask(key, hash);
}

/*
 *
 */
always_inline uint64_t
AVX512_find_32x16(const uint32_t *array,
                  uint32_t val)
{
    __m512i target = _mm512_load_si512((__m512i *) (volatile void *) array);
    __m512i key = _mm512_set1_epi32(val);

    return AVX512_cmp_flag(target, key);
}

BUCKET_DRIVER_GENERATE(AVX512);

#endif /* __AVX512F__ */
/*****************************************************************************
 * <--- AVX512 depened code
 *****************************************************************************/
/*
 * Arch Drivers
 */
static find_idx_in_bucket_t find_idx_in_bucket = GENERIC_find_idx_in_bucket;
static find_hval_in_bucket_single_t find_hval_in_bucket_single = GENERIC_find_hval_in_bucket_single;
static find_hval_in_bucket_double_t find_hval_in_bucket_double = GENERIC_find_hval_in_bucket_double;
static hash_func_t hash_func = GENERIC_hash_func;
static cmp_flow_key_t cmp_flow_key = GENERIC_cmp_flow_key;

/*
 * check cpuid AVX2,BMI,SSE4_2(crc32c)
 */
__attribute__((constructor))
static void
x86_handler_init (void)
{
    uint32_t eax = 0, ebx, ecx, edx;

    (void) Enable_SSE41;
    (void) Enable_SSE42;
    (void) Enable_AVX2;
    (void) Enable_AVX512;

    // Get the highest function parameter.
    __get_cpuid(0, &eax, &ebx, &ecx, &edx);

    // Check if the function parameter for extended features is available.
    if (eax >= 7) {
        __cpuid_count(1, 0, eax, ebx, ecx, edx);

#if defined(__SSE4_1__)
        if (Enable_SSE41 && (ecx & bit_SSE4_1)) {
            TRACER("use SSE4.1 ready\n");

            find_idx_in_bucket         = SSE41_find_idx_in_bucket;
            find_hval_in_bucket_single = SSE41_find_hval_in_bucket_single;
            find_hval_in_bucket_double = SSE41_find_hval_in_bucket_double;
            cmp_flow_key               = SSE41_cmp_flow_key;
        }
#endif

#if defined(__SSE4_2__)
        if (Enable_SSE42 && (ecx & bit_SSE4_2)) {
            TRACER("use SSE4.2 ready\n");

            hash_func = SSE42_calc_hash;
        }
#endif

        __cpuid_count(7, 0, eax, ebx, ecx, edx);

#if defined(__AVX2__)
        if (Enable_AVX2 && (ebx & bit_AVX2)) {
            TRACER("use AVX2 ready\n");

            find_idx_in_bucket         = AVX2_find_idx_in_bucket;
            find_hval_in_bucket_single = AVX2_find_hval_in_bucket_single;
            find_hval_in_bucket_double = AVX2_find_hval_in_bucket_double;
            cmp_flow_key               = AVX2_cmp_flow_key;
        }
#endif

#if defined(__AVX512F__)
        if (Enable_AVX512 && (ebx & bit_AVX512F)) {
            TRACER("use AVX512F ready\n");

            find_idx_in_bucket         = AVX512_find_idx_in_bucket;
            find_hval_in_bucket_single = AVX512_find_hval_in_bucket_single;
            find_hval_in_bucket_double = AVX512_find_hval_in_bucket_double;
        }
#endif
    }
}
#endif /* __x86_64__ */


/*******************************************************************************
 * Node queue
 ******************************************************************************/
/*
 *
 */
always_inline void
nodeq_init(struct flow_node_q_s *head,
           struct flow_node_s *tbl)
{
    IDXQ_INIT(head, tbl, entry);
}

/*
 *
 */
always_inline void
nodeq_remove(struct flow_node_q_s *head,
             struct flow_node_s *node)
{
    IDXQ_REMOVE(head, node, entry);
}

/*
 *
 */
always_inline void
nodeq_enq(struct flow_node_q_s *head,
          struct flow_node_s *node)
{
    IDXQ_INSERT_TAIL(head, node, entry);
}

/*
 *
 */
always_inline struct flow_node_s *
nodeq_deq(struct flow_node_q_s *head)
{
    struct flow_node_s *node = IDXQ_FIRST(head);

    if (node)
        IDXQ_REMOVE(head, node, entry);
    return node;
}

/*
 *
 */
always_inline void
nodeq_push(struct flow_node_q_s *head,
           struct flow_node_s *node)
{
    IDXQ_INSERT_HEAD(head, node, entry);
}

/*
 *
 */
  always_inline struct flow_node_s *
nodeq_pop(struct flow_node_q_s *head)
{
    struct flow_node_s *node = IDXQ_FIRST(head);

    if (node)
        IDXQ_REMOVE(head, node, entry);
    return node;
}

/*******************************************************************************
 * IDX Pool
 ******************************************************************************/
/*
 *
 */
always_inline void
idx_pool_init(struct idx_pool_s *idx_pool,
              unsigned nb,
              struct flow_node_s *node_array,
              void *idx_array)
{
    TRACER("idx_pool:%p nb:%u node_array:%p idx_array:%p\n",
           idx_pool, nb, node_array, idx_array);

    nodeq_init(&idx_pool->used_fifo, node_array);

    idx_pool->array_size = nb;
    idx_pool->node_array = node_array;
    idx_pool->idx_array = idx_array;
    idx_pool->nb_used = 0;

    memset(node_array, -1, sizeof(*node_array) * nb);
    for (uint32_t idx = 0; idx < nb; idx++)
        idx_pool->idx_array[idx] = idx;
}

/*
 *
 */
always_inline uint32_t
idx_pool_alloc(struct flow_cache_s *fcache)
{
    uint32_t idx = INVALID_IDX;

    if (fcache->idx_pool->array_size > fcache->idx_pool->nb_used) {
        idx = fcache->idx_pool->idx_array[fcache->idx_pool->nb_used++];
        nodeq_enq(&fcache->idx_pool->used_fifo, node_ptr(fcache, idx));
        idx_pool_next_prefetch(fcache, 2);
    }

    TRACER("node:%u\n", idx);
    return idx;
}

/*
 *
 */
always_inline void
idx_pool_free(struct flow_cache_s *fcache,
              uint32_t idx)
{
    if (idx != INVALID_IDX) {
        nodeq_remove(&fcache->idx_pool->used_fifo, node_ptr(fcache, idx));
        fcache->idx_pool->idx_array[--(fcache->idx_pool->nb_used)] = idx;
    }
}

/*
 *
 */
always_inline struct flow_bucket_s *
fetch_current_bucket(const struct flow_cache_s *fcache,
                     const struct flow_node_s *node,
                     unsigned *pos_p)
{
    union hash_u hash = fetch_hash(node);
    uint32_t idx = node_idx(fcache, node);
    struct flow_bucket_s *bk = NULL;

    for (int eo = 0; eo < 2; eo++) {
        bk = bucket_ptr(fcache, hash2idx(fcache, hash, eo));

        uint64_t hits = find_idx_in_bucket(fcache, bk, idx);
        if (hits) {
            *pos_p = __builtin_ctzll(hits);
            goto end;
        }
        bk = NULL;
    }

 end:
    TRACER("bucket:%u pos:%u node:%u\n",
           bucket_idx(fcache, bk), *pos_p, idx);
    return bk;
}

/*******************************************************************************
 * Node manager
 ******************************************************************************/
/*
 *
 */
always_inline struct flow_node_s *
fetch_old_node(const struct flow_cache_s *fcache)
{
    struct flow_node_s *node;

    node = IDXQ_FIRST(&fcache->idx_pool->used_fifo);
    return node;
}

/*
 * Number of Empty Slot in Bucket
 */
unsigned
nb_empty_slot(const struct flow_cache_s *fcache,
              struct flow_bucket_s *bk)
{
    uint64_t mask = find_hval_in_bucket_single(fcache, bk, INVALID_HVAL);
    (void) fcache;

    return __builtin_popcountll(mask);
}

/*
 *
 */
always_inline void
refetch_bucket(struct flow_pipeline_engine_s *engine,
               const struct flow_bucket_s *bk)
{
    for (unsigned i = 0; i < engine->pool_size; i++) {
        if (engine->ctx_pool[i].state != FLOW_XSTATE_CMP_KEY)
            continue;

        if (engine->ctx_pool[i].bk[0].ptr == bk ||
            engine->ctx_pool[i].bk[1].ptr == bk)
            engine->ctx_pool[i].state = FLOW_XSTATE_REFETCH_NODE;
    }
}

/**
 * @brief Set all bits below MSB
 *
 * @param v: input integer
 * @return integer
 */
always_inline unsigned
combine_ms1b (unsigned v)
{
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;

    return v;
}

/**
 * @brief Returns the nearest power of 2 times greater than
 *
 * @param input integer
 * @return integer
 */
always_inline unsigned
align_pow2 (unsigned v)
{
    v--;
    v = combine_ms1b(v);
    return v + 1;
}

/*
 *
 */
always_inline unsigned
nb_nodes (unsigned nb_entries)
{
    nb_entries = nb_entries * 16 / 13;
    if (nb_entries < NB_ENTRIES_MIN)
        nb_entries = NB_ENTRIES_MIN;
    nb_entries = align_pow2(nb_entries);

    TRACER("nb nodes:%u\n", nb_entries);
    return nb_entries;
}

/*
 * max buckets + 1
 */
always_inline unsigned
nb_buckets (unsigned nb_entries)
{
    unsigned nb_buckets = nb_entries / BUCKET_ENTRY_SZ;

    TRACER("nb buckets:%u\n", nb_buckets);
    return nb_buckets;
}

/*******************************************************************************
 * Context Engine
 *******************************************************************************/
/*
 *
 */
always_inline int
flipflop_bucket(const struct flow_cache_s *fcache,
                struct flow_pipeline_engine_s *engine,
                struct flow_bucket_s *src_bk,
                unsigned src_pos)
{
    struct flow_bucket_s *dst_bk = fetch_another_bucket(fcache, src_bk, src_pos);
    unsigned dst_pos = -1;
    int ret = -1;

    uint64_t empty = find_hval_in_bucket_single(fcache, dst_bk, INVALID_HVAL);
    if (empty) {
        dst_pos = __builtin_ctzll(empty);

        move_bucket(dst_bk, dst_pos, src_bk, src_pos);

        if (engine) {
            refetch_bucket(engine, dst_bk);
            refetch_bucket(engine, src_bk);
        }

        ret = 0;
    }
    TRACER("src bk:%u pos:%u  dst bk:%u pos:%u ret:%d\n",
           bucket_idx(fcache, src_bk), src_pos,
           bucket_idx(fcache, dst_bk), dst_pos,
           ret);
    return ret;
}

/*
 * kick out egg
 */
static int
kickout_node(const struct flow_cache_s *fcache,
             struct flow_pipeline_engine_s *engine,
             struct flow_bucket_s *bk,
             int cnt)
{
    int pos = -1;

    TRACER("bk:%u cnt:%d\n", bucket_idx(fcache, bk), cnt);

    if (cnt--) {
        for (unsigned i = 0; i < BUCKET_ENTRY_SZ; i++) {
            if (!flipflop_bucket(fcache, engine, bk, i)) {
                pos = i;
                goto end;
            }
        }

        for (unsigned i = 0; i < BUCKET_ENTRY_SZ; i++) {
            struct flow_bucket_s *ano_bk = fetch_another_bucket(fcache, bk, i);

            if (kickout_node(fcache, engine, ano_bk, cnt) < 0)
                continue;

            if (!flipflop_bucket(fcache, engine, bk, i)) {
                pos = i;
                goto end;
            }
        }
    }
 end:
    return pos;
}

/*
 *
 */
always_inline struct flow_node_s *
find_node_in_bucket(const struct flow_cache_s *fcache,
                    const struct flow_bucket_s *bk,
                    uint64_t hits,
                    const struct flow_key_s *fkey,
                    unsigned *pos_p)
{
    struct flow_node_s *node = NULL;
    unsigned pos = -1;

    TRACER("bk:%u fkey:%p hits:%"PRIx64"\n", bucket_idx(fcache, bk), fkey, hits);

    if (hits) {
        pos = __builtin_ctzll(hits);
        for (hits >>= pos; hits; pos++, hits >>= 1) {
            if (hits & 1) {
                node = fetch_node(fcache, bk, pos);
                if (!memcmp(&node->key.val, &fkey->val, sizeof(node->key.val))) {
                    *pos_p = pos;
                    break;
                }
                node = NULL;
                TRACER("mismatched.\n");
            }
        }
    }

    TRACER("node:%u pos:%u\n", node_idx(fcache, node), pos);
    return node;
}

/*
 *
 */
static struct flow_node_s *
insert_node(struct flow_cache_s *fcache,
            struct flow_pipeline_engine_s *engine,
            struct flow_pipeline_ctx_s *ctx)
{
    struct flow_bucket_s *bk;
    int pos;
    uint32_t node_idx = INVALID_IDX;

    TRACER("bk[0]:%u bk[1]:%u hval:%08x\n",
           bucket_idx(fcache, ctx->bk[0].ptr), bucket_idx(fcache, ctx->bk[1].ptr),
           hash2val(ctx->hash));

    uint64_t empt[2];

#if 0
    empt[0] = find_hval_in_bucket_single(fcache, ctx->bk[0].ptr, INVALID_HVAL);
    empt[1] = find_hval_in_bucket_single(fcache, ctx->bk[1].ptr, INVALID_HVAL);
    if (empt[0] || empt[1]) {
        /* which use bk[0] or bk[1] */

        unsigned cnt[2];

        cnt[0] = __builtin_popcountll(empt[0]);
        cnt[1] = __builtin_popcountll(empt[1]);

        TRACER("Count bk[0]:%u bk[1]:%u\n", cnt[0], cnt[1]);

        if (cnt[0] < cnt[1]) {
            bk = ctx->bk[1].ptr;
            pos = __builtin_ctzll(empt[1]);
        } else {
            bk = ctx->bk[0].ptr;
            pos = __builtin_ctzll(empt[0]);
        }
    }
#else
    empt[0] = find_hval_in_bucket_single(fcache, ctx->bk[0].ptr, INVALID_HVAL);
    if (!empt[0])
        empt[1] = find_hval_in_bucket_single(fcache, ctx->bk[1].ptr, INVALID_HVAL);

    if (empt[0]) {
        bk = ctx->bk[0].ptr;
        pos = __builtin_ctzll(empt[0]);
    } else if (empt[1]) {
        bk = ctx->bk[1].ptr;
        pos = __builtin_ctzll(empt[1]);
    }
#endif
    else {
        /* No Vacancy */
        pos = kickout_node(fcache, engine, ctx->bk[0].ptr, MAX_DEPTH);
        if (pos < 0) {
            pos = kickout_node(fcache, engine, ctx->bk[1].ptr, MAX_DEPTH);
            if (pos < 0) {
                /* error */
                TRACER("failed to insert node\n");
                goto end;
            }
            bk = ctx->bk[1].ptr;
        } else {
            bk = ctx->bk[0].ptr;
        }
    }

    node_idx = idx_pool_alloc(fcache);
    if (node_idx != INVALID_IDX) {
        set_bucket(bk, pos, node_idx, hash2val(ctx->hash));
        set_key(node_ptr(fcache, node_idx), ctx->fkey_p, ctx->hash);

        fcache->node_init(node_ptr(fcache, node_idx));

        refetch_bucket(engine, bk);

        TRACER("node:%u bk:%u pos:%d\n", node_idx, bucket_idx(fcache, bk), pos);
    } else {
        TRACER("failed to allcate node\n");
    }

 end:
    return node_ptr(fcache, node_idx);
}

/*
 *
 */
static inline union hash_u
read_hash(const struct flow_key_s *key,
          uint32_t mask __attribute__((unused)))
{
    return key->hash;
}

/*
 *
 */
always_inline void
init_pipeline(struct flow_pipeline_engine_s *engine,
              unsigned nb,
              struct flow_node_s **node_pp,
              struct flow_key_s **fkey_pp,
              unsigned pool_size)
{
    engine->node_pp = node_pp;
    engine->fkey_pp = fkey_pp;
    engine->next = 0;
    engine->req_nb = nb;
    engine->node_nb = 0;
    engine->pool_size = pool_size;

    for (unsigned i = 0; i < pool_size; i++) {
        engine->ctx_pool[i].idx = INVALID_IDX;

        switch (i % 3) {
        case 0:
            engine->ctx_pool[i].state = FLOW_XSTATE_PREFETCH_KEY;
            break;
        case 1:
            engine->ctx_pool[i].state = FLOW_XSTATE_WAIT_1;
            break;
        case 2:
            engine->ctx_pool[i].state = FLOW_XSTATE_WAIT_2;
            break;
        }
    }
}

/*
 *
 */
always_inline unsigned
do_ctx(struct flow_cache_s *fcache,
       struct flow_pipeline_engine_s *engine,
       const unsigned nb,
       struct flow_pipeline_ctx_s *ctx)
{
    unsigned done = 0;
    struct flow_node_s *node = NULL;
    //        uint64_t tsc = rdtsc();

    TRACER("Start--> nb:%u next:%u ctx:%ld state:%d idx:%u\n",
           nb, engine->next,
           ctx - engine->ctx_pool, ctx->state, ctx->idx);

    switch (ctx->state) {
    default:
        break;

    case FLOW_XSTATE_WAIT_2:
        ctx->state = FLOW_XSTATE_WAIT_1;
        break;

    case FLOW_XSTATE_WAIT_1:
        ctx->state = FLOW_XSTATE_PREFETCH_KEY;
        break;

    case FLOW_XSTATE_PREFETCH_KEY:
        if (engine->next < nb) {
            ctx->idx     = engine->next++;
            ctx->fkey_p  = engine->fkey_pp[ctx->idx];
            ctx->node_pp = &engine->node_pp[ctx->idx];

            prefetch(ctx->fkey_p, 1);

            ctx->state = FLOW_XSTATE_FETCH_BUCKET;
        }
        break;

    case FLOW_XSTATE_FETCH_BUCKET:
        /* calk hash and fetch bucket */
        ctx->hash = engine->hash_func(ctx->fkey_p, fcache->bk_mask);

        ctx->bk[0].ptr  = fetch_bucket(fcache, hash2idx(fcache, ctx->hash, 0));
        ctx->bk[0].hits = INVALID_FLAGS;

        ctx->bk[1].ptr  = fetch_bucket(fcache, hash2idx(fcache, ctx->hash, 1));
        ctx->bk[1].hits = INVALID_FLAGS;

        ctx->state = FLOW_XSTATE_FETCH_NODE;
        break;

    case FLOW_XSTATE_FETCH_NODE:
        /* find hash value in bucket */
        find_hval_in_bucket_double(fcache, ctx);

        ctx->state = FLOW_XSTATE_CMP_KEY;
        break;

    case FLOW_XSTATE_REFETCH_NODE:
        find_hval_in_bucket_double(fcache, ctx);
        /* fall through */

    case FLOW_XSTATE_CMP_KEY:
    {
        unsigned pos;

        node = find_node_in_bucket(fcache, ctx->bk[0].ptr, ctx->bk[0].hits,
                                   ctx->fkey_p, &pos);
        if (!node)
            node = find_node_in_bucket(fcache, ctx->bk[1].ptr, ctx->bk[1].hits,
                                       ctx->fkey_p, &pos);

        if (!node) {
            cls_debug_handler(fcache);
            node = insert_node(fcache, engine, ctx);
            set_debug_handler(fcache);
        }

        *ctx->node_pp = node;
        if (node)
            engine->node_nb += 1;
        else
            fcache->fails += 1;

        done = 1;
        ctx->idx = INVALID_IDX;
        ctx->state = FLOW_XSTATE_PREFETCH_KEY;
    }
    break;
    }

    //        fcache->eng_perf[update_state].tsc += (rdtsc() - tsc);

    TRACER("<--  End nb:%u next:%u ctx:%ld state:%d idx:%u done:%u node:%u\n",
           nb, engine->next,
           ctx - engine->ctx_pool, ctx->state, ctx->idx,
           done, node_idx(fcache, node));

    return done;
}

/*
 *
 */
always_inline unsigned
pipeline_engine(struct flow_cache_s *fcache,
                struct flow_node_s **node_pp,
                struct flow_key_s **fkey_pp,
                unsigned nb,
                int with_hash)
{
    struct flow_pipeline_engine_s *engine = &fcache->engine;
    unsigned cnt = 0;

    set_debug_handler(fcache);

    uint64_t tsc = rdtsc();
    idx_pool_next_prefetch(fcache, 2);

    init_pipeline(engine, nb, node_pp, fkey_pp, fcache->ctx_pool_size);
    if (with_hash)
        engine->hash_func = read_hash;
    else
        engine->hash_func = fcache->calc_hash;

    while (nb > cnt) {
        for (unsigned i = 0; (nb > cnt) && (i < engine->pool_size); i++) {
            cnt += do_ctx(fcache, engine, nb, &engine->ctx_pool[i]);
        }
    }

    if (cnt) {
        fcache->tsc += (rdtsc() - tsc);
        fcache->cnt += cnt;
    }

    cls_debug_handler(fcache);

    return engine->node_nb;
}

/*
 *
 */
static void
null_node_init(struct flow_node_s *node __attribute__((unused)))
{
    /* nothing to do */
}

/*******************************************************************************
 * API
 *******************************************************************************/
/*
 *
 */
int
flipflop_node(const struct flow_cache_s *fcache,
              const struct flow_node_s *node)
{
    int ret = -1;
    uint32_t idx = node_idx(fcache, node);
    if (idx != INVALID_IDX) {
        unsigned pos = -1;
        struct flow_bucket_s *bk = fetch_current_bucket(fcache, node, &pos);

        if (bk)
            ret = flipflop_bucket(fcache, NULL, bk, pos);
    }
    return ret;
}

/*
 *
 */
unsigned
find_node_bulk(struct flow_cache_s *fcache,
               struct flow_key_s **fkey_pp,
               unsigned nb,
               struct flow_node_s **node_pp,
               int with_hash)
{
    return pipeline_engine(fcache, node_pp, fkey_pp, nb, with_hash);
}

/*
 *
 */
size_t
fcache_sizeof(unsigned nb)
{
    size_t sz = sizeof(struct flow_cache_s) + sizeof(struct idx_pool_s);

    nb = nb_nodes(nb);
    sz += sizeof(struct flow_bucket_s) * nb_buckets(nb);

    sz += sizeof(struct flow_node_s) * nb;
    sz += idx_pool_sizeof(nb);

    TRACER("nb:%u sz:%zu\n", nb, sz);
    return sz;
}

/*
 *
 */
struct flow_cache_s *
fcache_alloc(unsigned nb)
{
    struct flow_cache_s *fc = aligned_alloc(CACHELINE_SIZE, fcache_sizeof(nb));

    TRACER("nb:%u fc:%p\n", nb, fc);
    return fc;
}

/*
 *
 */
void
fcache_free(struct flow_cache_s *fcache)
{
    TRACER("fchace:%p\n", fcache);
    free(fcache);
}

/*
 *
 */
void
fcache_init(struct flow_cache_s *fcache,
            unsigned nb,
            unsigned ctx_size,
            hash_func_t func,
            node_initializer_t node_init)
{
    TRACER("fcache:%p nb:%u ctx:%u\n", fcache, nb, ctx_size);

    if (!ctx_size)
        ctx_size = 1;
    ctx_size *= 3;

    if (ctx_size > ARRAYOF(fcache->engine.ctx_pool))
        ctx_size = ARRAYOF(fcache->engine.ctx_pool);

    unsigned node_nb = nb_nodes(nb);
    unsigned bucket_nb = nb_buckets(node_nb);

    TRACER("re-size node:%u bucket:%u\n", node_nb, bucket_nb);

    fcache->max = (node_nb / 16) * 13;
    fcache->bk_mask = (uint32_t) bucket_nb - 1;
    fcache->nb = node_nb;
    fcache->cnt = 0;

    fcache->tsc = 0;
    fcache->cmp_cnt = 0;
    fcache->cmp_tsc = 0;
    fcache->fails = 0;
    fcache->is_debug = false;
    fcache->ctx_pool_size = ctx_size;

    if (func)
        fcache->calc_hash = func;
    else
        fcache->calc_hash = hash_func;

    if (node_init)
        fcache->node_init = node_init;
    else
        fcache->node_init = null_node_init;

    size_t sz = 0;

    fcache->idx_pool = (struct idx_pool_s *) &fcache->payload[sz];
    sz += sizeof(*(fcache->idx_pool));

    fcache->buckets = (struct flow_bucket_s *) &fcache->payload[sz];
    memset(fcache->buckets, -1, sizeof(struct flow_bucket_s) * bucket_nb);
    sz += sizeof(struct flow_bucket_s) * bucket_nb;

    fcache->nodes = (struct flow_node_s *) &fcache->payload[sz];
    sz += sizeof(struct flow_node_s) * node_nb;

    idx_pool_init(fcache->idx_pool, node_nb, fcache->nodes, &fcache->payload[sz]);
}

/*
 *
 */
void
fcache_reset(struct flow_cache_s *fcache)
{
    fcache_init(fcache, fcache->max, fcache->ctx_pool_size / 3,
                fcache->calc_hash, fcache->node_init);
}

/*
 *
 */
struct flow_cache_s *
fcache_create(unsigned nb,
              unsigned ctx_size,
              hash_func_t func,
              node_initializer_t node_init)
{
    TRACER("nb:%u func:%p\n", nb, func);

    struct flow_cache_s *fcache = fcache_alloc(nb);

    if (fcache)
        fcache_init(fcache, nb, ctx_size, func, node_init);

    TRACER("fcache:%p\n", fcache);
    return fcache;
}

/*
 *
 */
void
free_node(struct flow_cache_s *fcache,
          struct flow_node_s *node)
{
    if (node) {
        unsigned pos = -1;
        struct flow_bucket_s *bk = fetch_current_bucket(fcache, node, &pos);

        if (bk)
            clear_bucket(bk, pos);

        idx_pool_free(fcache, node_idx(fcache, node));
    }
}

/*
 *
 */
union hash_u
calc_hash_default(const struct flow_key_s *key,
                  uint32_t mask)
{
    return GENERIC_hash_func(key, mask);
}



/*
 *
 */
void
dump_key(const struct flow_cache_s *fcache,
         const struct flow_key_s *key,
         FILE *stream,
         const char *title)
{
    char msg[256];
    unsigned len;
    union hash_u hash = fcache_calc_hash(fcache, key);

    len = snprintf(msg, sizeof(msg), "%s hval:%08x key:",
                   title,  hash2val(hash));
    for (unsigned i = 0; i < ARRAYOF(key->val.d32); i++)
        snprintf(&msg[len], sizeof(msg) - len, "%08x ", key->val.d32[i]);
    fprintf(stream, "%s\n", msg);
}

/*
 *
 */
void
dump_node(const struct flow_cache_s *fcache,
          FILE *stream,
          const char *title,
          const struct flow_node_s *node)
{
    fprintf(stream, "%s node:%u\n", title, node_idx(fcache, node));
    dump_key(fcache, &node->key, stream, "--->");
}

/*
 *
 */
void
dump_bucket(const struct flow_cache_s *fcache,
            FILE *stream,
            const char *title,
            const struct flow_bucket_s *bk)
{
    fprintf(stream,
            "%s bk:%u\n"
            "  hval:%08x %08x %08x %08x %08x %08x %08x %08x\n"
            "       %08x %08x %08x %08x %08x %08x %08x %08x\n"
            "  idx :%08x %08x %08x %08x %08x %08x %08x %08x\n"
            "       %08x %08x %08x %08x %08x %08x %08x %08x\n"
            ,
            title,
            bucket_idx(fcache, bk),
            bk->hval[0], bk->hval[1], bk->hval[2], bk->hval[3],
            bk->hval[4], bk->hval[5], bk->hval[6], bk->hval[7],
            bk->hval[8], bk->hval[9], bk->hval[10], bk->hval[11],
            bk->hval[12], bk->hval[13], bk->hval[14], bk->hval[15],
            bk->idx[0], bk->idx[1], bk->idx[2], bk->idx[3],
            bk->idx[4], bk->idx[5], bk->idx[6], bk->idx[7],
            bk->idx[8], bk->idx[9], bk->idx[10], bk->idx[11],
            bk->idx[12], bk->idx[13], bk->idx[14], bk->idx[15]
        );
}


/*
 *
 */
void
dump_idx_pool(const struct idx_pool_s *pool,
              FILE *stream,
              const char *title)
{
    fprintf(stream,
            "%s pool:%p size:%u nb:%u node:%p idx:%p\n",
            title, pool,
            pool->array_size,
            pool->nb_used,
            pool->node_array,
            pool->idx_array);
}

/*
 *
 */
void
dump_fcache(struct flow_cache_s *fcache,
            FILE *stream,
            const char *title)
{
    uint64_t cnt = fcache->cnt;
    uint64_t tsc = fcache->tsc;

    if (!cnt)
        cnt = 1;

    fprintf(stream,
            "%s fcache:%p mask:%08x nb:%u max:%u ctx:%u fails:%"PRIu64" cnt:%"PRIu64" %0.2f cmp:%0.2f\n",
            title, fcache,
            fcache->bk_mask, fcache->nb, fcache->max, fcache->ctx_pool_size,
            fcache->fails, cnt,
            (double) tsc / cnt, (double) fcache->cmp_tsc / (fcache->cmp_cnt + 1));

    dump_idx_pool(fcache->idx_pool, stream, "    ");
}

/*
 *
 */
struct flow_bucket_s *
current_bucket(const struct flow_cache_s *fcache,
               const struct flow_node_s * node)
{
    unsigned pos = -1;
    return fetch_current_bucket(fcache, node, &pos);
}

/*
 *
 */
struct flow_bucket_s *
another_bucket(const struct flow_cache_s *fcache,
               const struct flow_node_s * node)
{
    struct flow_bucket_s *bk = NULL;

    if (node) {
        unsigned pos = -1;
        uint32_t idx = bucket_idx(fcache, fetch_current_bucket(fcache, node, &pos));
        idx = (idx ^ hash2val(node->key.hash)) & fcache->bk_mask;
        bk = bucket_ptr(fcache, idx);
    }
    return bk;
}

/*
 *
 */
static int
walk_default(struct flow_cache_s *fcache,
             struct flow_node_s *node,
             void *arg)
{
    int *nb_p = arg;
    char title[80];

    snprintf(title, sizeof(title), "Walk Node %d", *nb_p);
    dump_node(fcache, stdout, title, node);

    return 0;
}

/*
 *
 */
int
fcache_walk(struct flow_cache_s *fcache,
            int (*cb_func)(struct flow_cache_s *, struct flow_node_s *, void *),
            void *arg)
{
    unsigned bk_idx = 0;
    int nb = 0;
    int ret = 0;

    if (!cb_func) {
        cb_func = walk_default;
        arg = &nb;
    }

    do {
        struct flow_bucket_s * bk = bucket_ptr(fcache, bk_idx);

        for (unsigned i = 0; !ret && i < BUCKET_ENTRY_SZ; i++) {
            if (bk->hval[i] != INVALID_HVAL) {
                struct flow_node_s * node;

                node = node_ptr(fcache, bk->idx[i]);
                ret = cb_func(fcache, node, arg);
                nb += 1;
            }
        }

        bk_idx++;
        bk_idx &= fcache->bk_mask;
    } while (!ret && bk_idx);

    return ret;
}

/*
 *
 */
unsigned
fcache_node_num(const struct flow_cache_s *fcache)
{
    return fcache->idx_pool->nb_used;
}

/*
 *
 */
int
verify_node(const struct flow_cache_s *fcache,
            const struct flow_node_s *node,
            const struct flow_key_s *key)
{
    int ret = -1;
    uint32_t idx = node_idx(fcache, node);

    if (!node) {
        fprintf(stderr, "node is NULL\n");
        goto end;
    }

    if (memcmp(&key->val, &node->key.val, sizeof(key->val))) {
        fprintf(stderr, "mismatched key. node:%u\n", idx);
        goto end;
    }

    struct flow_bucket_s *cur_bk, *ano_bk;
    unsigned pos = -1;

    cur_bk = fetch_current_bucket(fcache, node, &pos);
    if (!cur_bk) {
        fprintf(stderr, "unknown current bucket. node:%u\n", idx);
        goto end;
    }

    ano_bk = fetch_another_bucket(fcache, cur_bk, pos);
    if (!ano_bk) {
        fprintf(stderr, "unknown another bucket. node:%u\n", idx);
        goto end;
    }

    union hash_u hash = fcache_calc_hash(fcache, &node->key);
    if (node->key.hash.val64 != hash.val64) {
        fprintf(stderr, "mismatched hash. node:%u\n", idx);
        goto end;
    }

    if (((hash2val(hash) ^ bucket_idx(fcache, ano_bk)) & fcache->bk_mask) !=
        bucket_idx(fcache, cur_bk)) {
        fprintf(stderr, "mismatched bucket index. node:%u\n", idx);
        goto end;
    }

    ret = 0;
 end:
    //        fprintf(stdout, "%s ret:%d\n", __func__, ret);
    return ret;
}
/*******************************************************************************
 * spped test
 *******************************************************************************/

static uint64_t
_speed_test_hash(hash_func_t func)
{
    struct flow_key_s *keys = calloc(1024 * 32, sizeof(*keys));

    for (unsigned i = 0; i < 1024 * 32; i++) {
        for (unsigned j = 0; j < ARRAYOF(keys[i].val.d32); j++) {
            keys[i].val.d32[j] = random();
        }
    }

    uint64_t tsc = rdtsc();

    for (unsigned i = 0; i < 1024 * 32; i++)
        func(&keys[i], (1 << 18) - 1);

    tsc = rdtsc() - tsc;
    free(keys);
    return tsc / (1024 * 32);
}

void
speed_test_hash(void)
{
    uint64_t tsc;

    for (unsigned i = 0; i < 1; i++) {
        tsc = _speed_test_hash(GENERIC_hash_func);
        fprintf(stdout, "Speed default hash: %"PRIu64"\n", tsc);

        tsc = _speed_test_hash(SSE42_calc_hash);
        fprintf(stdout, "Speed CRC32 hash: %"PRIu64"\n", tsc);
    }
}

static uint64_t
_speed_test_find_hval_in_bucket_double(struct flow_cache_s *fcache,
                                       struct flow_pipeline_ctx_s *ctx,
                                       find_hval_in_bucket_double_t func)
{
    struct flow_bucket_s *bk = bucket_ptr(fcache, 0);

    uint64_t tsc = rdtsc();
    for (unsigned i = 0; i < 1024 * 32; i++) {
        unsigned j = i % BUCKET_ENTRY_SZ;
                
        bk->hval[j] = 1;
        bk->idx[j] = 0;

        func(fcache, ctx);

        bk->hval[j] = -1;
        bk->idx[j] = -1;
    }

    return (rdtsc() - tsc) / (1024 * 32);
}

static uint64_t
_speed_test_find_hval_in_bucket_single(struct flow_cache_s *fcache,
                                       const struct flow_bucket_s * bk,
                                       uint32_t hval,
                                       find_hval_in_bucket_single_t func)
{
    sched_yield();
    uint64_t xxx = 0;

    uint64_t tsc = rdtsc();
    for (unsigned i = 0; i < 1024 * 32; i++) {
        xxx &= func(fcache, bk, hval);
    }
    fcache->cmp_tsc = xxx;	/* dummy */

    return (rdtsc() - tsc) / (1024 * 32);
}

static inline void
test_32x16(void)
{
    uint32_t val[16];

    for (unsigned i = 0; i < ARRAYOF(val); i++)
        val[i] = i;
    for (unsigned i = 0; i < ARRAYOF(val); i++) {
        uint64_t flags = GENERIC_find_32x16(val, i);

        if (flags != SSE41_find_32x16(val, i) ||
            flags != AVX2_find_32x16(val, i))
            fprintf(stdout, "Bad\n");

        fprintf(stdout, "%uth %"PRIx64"\n", i, flags);
    }
}

void
speed_test_cmp_hval_in_bucket(void)
{
    struct flow_cache_s fcache cache_aligned;
    struct flow_bucket_s bk[2] cache_aligned;
    struct flow_node_s node[BUCKET_ENTRY_SZ] cache_aligned;
    struct flow_pipeline_ctx_s ctx cache_aligned;
    uint64_t tsc;

    struct idx_pool_s idx_pool cache_aligned;

    memset(bk, -1, sizeof(bk));
    memset(&fcache, 0, sizeof(fcache));
    memset(&ctx, -1, sizeof(ctx));
    memset(&idx_pool, 0, sizeof(idx_pool));

    idx_pool.node_array = node;

    fcache.idx_pool = &idx_pool;
    fcache.buckets = bk;
    fcache.bk_mask = 0xff;

    ctx.hash.val32[0] = 1;
    ctx.hash.val32[1] = 0;
    ctx.bk[0].ptr = &bk[0];
    ctx.bk[1].ptr = &bk[1];

    for (unsigned i = 0; i < 1; i++) {
        tsc = _speed_test_find_hval_in_bucket_double(&fcache, &ctx,
                                                     GENERIC_find_hval_in_bucket_double);
        fprintf(stdout, "Speed default double: %"PRIu64"\n", tsc);

        tsc = _speed_test_find_hval_in_bucket_double(&fcache, &ctx,
                                                     SSE41_find_hval_in_bucket_double);
        fprintf(stdout, "Speed SSE41 double: %"PRIu64"\n", tsc);

        tsc = _speed_test_find_hval_in_bucket_double(&fcache, &ctx,
                                                     AVX2_find_hval_in_bucket_double);
        fprintf(stdout, "Speed AVX2 double: %"PRIu64"\n", tsc);

        fprintf(stdout, "xxx\n");


        tsc = _speed_test_find_hval_in_bucket_single(&fcache, bk, INVALID_HVAL,
                                                     GENERIC_find_hval_in_bucket_single);
        fprintf(stdout, "Speed default single: %"PRIu64"\n", tsc);
        tsc = _speed_test_find_hval_in_bucket_single(&fcache, bk, INVALID_HVAL,
                                                     SSE41_find_hval_in_bucket_single);
        fprintf(stdout, "Speed SSE41 single: %"PRIu64"\n", tsc);
        tsc = _speed_test_find_hval_in_bucket_single(&fcache, bk, INVALID_HVAL,
                                                     AVX2_find_hval_in_bucket_single);
        fprintf(stdout, "Speed AVX2 single: %"PRIu64"\n", tsc);
    }
}

