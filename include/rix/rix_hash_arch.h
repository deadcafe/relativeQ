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
 * rix_hash_arch.h - arch-dependent infrastructure shared by all hash variants
 *
 * Provides runtime SIMD dispatch (Generic / AVX2 / AVX-512) for fingerprint
 * and key search operations used by rix_hash.h, rix_hash32.h, rix_hash64.h,
 * and rix_hash_key.h.
 *
 * Included via the _RIX_HASH_COMMON_ guard so that including multiple hash
 * variant headers in a single translation unit compiles this block only once.
 *
 * Call rix_hash_arch_init() once at program startup before any table ops.
 */

#ifndef _RIX_HASH_ARCH_H_
#  define _RIX_HASH_ARCH_H_

#  include "rix_defs_private.h"

#  include <stdint.h>

#  if defined(__x86_64__)
#    include <immintrin.h>
#    include <cpuid.h>
#  endif

#  ifndef _RIX_HASH_COMMON_
#    define _RIX_HASH_COMMON_

/* Silence unused-variable warnings for static symbols */
#    ifndef RIX_UNUSED
#      define RIX_UNUSED __attribute__((unused))
#    endif

/*===========================================================================
 * Constants shared across all hash variants
 *===========================================================================*/
#    define RIX_HASH_BUCKET_ENTRY_SZ 16
#    define RIX_HASH_FOLLOW_DEPTH 8

/*===========================================================================
 * rix_hash_arch_init() enable-mask bits
 *
 *   0                    - force Generic (scalar); useful for benchmarking
 *   RIX_HASH_ARCH_AVX2   - allow AVX2   if CPU supports it
 *   RIX_HASH_ARCH_AVX512 - allow AVX-512F if CPU supports it
 *   RIX_HASH_ARCH_AUTO   - use best available (same as legacy behaviour)
 *
 * Higher levels take priority: AVX-512 > AVX2 > Generic.
 * CPUID is always checked; the mask is a whitelist, not an override.
 *===========================================================================*/
#    define RIX_HASH_ARCH_AVX2   (1u << 0)
#    define RIX_HASH_ARCH_AVX512 (1u << 1)
#    define RIX_HASH_ARCH_AUTO   (RIX_HASH_ARCH_AVX2 | RIX_HASH_ARCH_AVX512)

/*===========================================================================
 * Hash union
 *===========================================================================*/
union rix_hash_hash_u {
    uint64_t val64;
    uint32_t val32[2];
};

/*===========================================================================
 * Arch handler - runtime SIMD dispatch
 *===========================================================================*/
struct rix_hash_arch_s {
    /* find val in uint32_t[16], returns 16-bit bitmask of hit positions */
    uint32_t (*find_u32x16)(const uint32_t *arr, uint32_t val);
    /* find val in uint64_t[16], returns 16-bit bitmask of hit positions */
    uint32_t (*find_u64x16)(const uint64_t *arr, uint64_t  val);
};

/* Set once by rix_hash_arch_init(); static per-TU (each TU must call init). */
static RIX_UNUSED const struct rix_hash_arch_s *rix_hash_arch;

/*---------------------------------------------------------------------------
 * Generic (scalar) implementations
 *---------------------------------------------------------------------------*/
static RIX_FORCE_INLINE uint32_t
_rix_hash_find_u32x16_GEN(const uint32_t *arr, uint32_t val)
{
    uint32_t mask = 0u;
    for (unsigned i = 0u; i < 16u; i++)
        if (arr[i] == val)
            mask |= (1u << i);
    return mask;
}

static RIX_FORCE_INLINE uint32_t
_rix_hash_find_u64x16_GEN(const uint64_t *arr, uint64_t val)
{
    uint32_t mask = 0u;
    for (unsigned i = 0u; i < 16u; i++)
        if (arr[i] == val)
            mask |= (1u << i);
    return mask;
}

static RIX_UNUSED const struct rix_hash_arch_s _rix_hash_arch_GEN = {
    _rix_hash_find_u32x16_GEN,
    _rix_hash_find_u64x16_GEN,
};

/*---------------------------------------------------------------------------
 * AVX2 implementations
 *---------------------------------------------------------------------------*/
#    if defined(__x86_64__) && defined(__AVX2__)

static RIX_FORCE_INLINE uint32_t
_rix_hash_find_u32x16_AVX2(const uint32_t *arr, uint32_t val)
{
    __m256i vval = _mm256_set1_epi32((int)val);
    __m256i a0   = _mm256_loadu_si256((const __m256i *)(const void *)arr);
    __m256i a1   = _mm256_loadu_si256((const __m256i *)(const void *)(arr + 8));
    __m256i eq0  = _mm256_cmpeq_epi32(a0, vval);
    __m256i eq1  = _mm256_cmpeq_epi32(a1, vval);
    uint32_t lo  = (uint32_t)(unsigned)
        _mm256_movemask_ps(_mm256_castsi256_ps(eq0));
    uint32_t hi  = (uint32_t)(unsigned)
        _mm256_movemask_ps(_mm256_castsi256_ps(eq1));
    return lo | (hi << 8);
}

static RIX_FORCE_INLINE uint32_t
_rix_hash_find_u64x16_AVX2(const uint64_t *arr, uint64_t val)
{
    __m256i vval = _mm256_set1_epi64x((long long)val);
    __m256i v0   =
        _mm256_loadu_si256((const __m256i *)(const void *)(arr +  0));
    __m256i v1   =
        _mm256_loadu_si256((const __m256i *)(const void *)(arr +  4));
    __m256i v2   =
        _mm256_loadu_si256((const __m256i *)(const void *)(arr +  8));
    __m256i v3   =
        _mm256_loadu_si256((const __m256i *)(const void *)(arr + 12));
    uint32_t m0  = (uint32_t)
        _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v0, vval)));
    uint32_t m1  = (uint32_t)
        _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v1, vval)));
    uint32_t m2  = (uint32_t)
        _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v2, vval)));
    uint32_t m3  = (uint32_t)
        _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v3, vval)));
    return m0 | (m1 << 4) | (m2 << 8) | (m3 << 12);
}

static RIX_UNUSED const struct rix_hash_arch_s _rix_hash_arch_AVX2 = {
    _rix_hash_find_u32x16_AVX2,
    _rix_hash_find_u64x16_AVX2,
};

#    endif /* __x86_64__ && __AVX2__ */

/*---------------------------------------------------------------------------
 * AVX-512 implementations
 *---------------------------------------------------------------------------*/
#    if defined(__x86_64__) && defined(__AVX512F__)

static RIX_FORCE_INLINE uint32_t
_rix_hash_find_u32x16_AVX512(const uint32_t *arr, uint32_t val)
{
    __m512i vval = _mm512_set1_epi32((int)val);
    __m512i va   = _mm512_loadu_si512((const void *)arr);
    __mmask16 m  = _mm512_cmpeq_epi32_mask(va, vval);
    return (uint32_t)m;
}

static RIX_FORCE_INLINE uint32_t
_rix_hash_find_u64x16_AVX512(const uint64_t *arr, uint64_t val)
{
    __m512i vval = _mm512_set1_epi64((long long)val);
    __m512i va   = _mm512_loadu_si512((const void *)(arr + 0));
    __m512i vb   = _mm512_loadu_si512((const void *)(arr + 8));
    __mmask8 m0  = _mm512_cmpeq_epi64_mask(va, vval);
    __mmask8 m1  = _mm512_cmpeq_epi64_mask(vb, vval);
    return (uint32_t)m0 | ((uint32_t)m1 << 8);
}

static RIX_UNUSED const struct rix_hash_arch_s _rix_hash_arch_AVX512 = {
    _rix_hash_find_u32x16_AVX512,
    _rix_hash_find_u64x16_AVX512,
};

#    endif /* __x86_64__ && __AVX512F__ */

/*---------------------------------------------------------------------------
 * rix_hash_arch_init - call once at program startup before any table ops.
 *
 * enable: bitmask of RIX_HASH_ARCH_* flags that are allowed.
 *   0                  → Generic (scalar) — useful for benchmarking
 *   RIX_HASH_ARCH_AUTO → use best available (recommended default)
 *
 * CPUID is always verified; the mask is a whitelist, not a forced override.
 * Must be called in each translation unit that uses hash table operations.
 *---------------------------------------------------------------------------*/
static RIX_FORCE_INLINE void
rix_hash_arch_init(uint32_t enable)
{
#    if defined(__x86_64__)
    unsigned eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;

    rix_hash_arch = &_rix_hash_arch_GEN; /* fallback */

    if (enable && __get_cpuid_count(7u, 0u, &eax, &ebx, &ecx, &edx)) {
#      if defined(__AVX512F__)
        /* AVX-512F: EBX bit 16 */
        if ((enable & RIX_HASH_ARCH_AVX512) && (ebx & (1u << 16))) {
            rix_hash_arch = &_rix_hash_arch_AVX512;
            return;
        }
#      endif
#      if defined(__AVX2__)
        /* AVX2: EBX bit 5 */
        if ((enable & RIX_HASH_ARCH_AVX2) && (ebx & (1u << 5))) {
            rix_hash_arch = &_rix_hash_arch_AVX2;
            return;
        }
#      endif
    }
#    else /* !__x86_64__ */
    (void)enable;
    rix_hash_arch = &_rix_hash_arch_GEN;
#    endif
}

#  endif /* _RIX_HASH_COMMON_ */

#endif /* _RIX_HASH_ARCH_H_ */
