/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * Intentionally has no include guard: include once per direct hash variant.
 *
 * Required:
 *   FC_HASH_DIRECT_HT_PREFIX
 *   FC_HASH_DIRECT_ENTRY
 *   FC_HASH_DIRECT_CMP_FN
 *   FC_HASH_DIRECT_HASH_FN
 *   FC_HASH_DIRECT_TARGET      gen | sse | avx2 | avx512
 *
 * Optional:
 *   FC_HASH_DIRECT_DECLARE_HEAD
 */

#ifndef FC_HASH_DIRECT_HT_PREFIX
#error "FC_HASH_DIRECT_HT_PREFIX must be defined"
#endif

#ifndef FC_HASH_DIRECT_ENTRY
#error "FC_HASH_DIRECT_ENTRY must be defined"
#endif

#ifndef FC_HASH_DIRECT_CMP_FN
#error "FC_HASH_DIRECT_CMP_FN must be defined"
#endif

#ifndef FC_HASH_DIRECT_HASH_FN
#error "FC_HASH_DIRECT_HASH_FN must be defined"
#endif

#ifndef FC_HASH_DIRECT_TARGET
#error "FC_HASH_DIRECT_TARGET must be defined"
#endif

#define _FC_HASH_DIRECT_ISA_(target) _FC_HASH_DIRECT_ISA_##target
#define _FC_HASH_DIRECT_ISA(target)  _FC_HASH_DIRECT_ISA_(target)
#define _FC_HASH_DIRECT_ISA_gen      GEN
#define _FC_HASH_DIRECT_ISA_sse      SSE
#define _FC_HASH_DIRECT_ISA_avx2     AVX2
#define _FC_HASH_DIRECT_ISA_avx512   AVX512

#define _FC_HASH_DIRECT_FIND_(isa)   _rix_hash_find_u32x16_##isa
#define _FC_HASH_DIRECT_FIND(isa)    _FC_HASH_DIRECT_FIND_(isa)
#define _FC_HASH_DIRECT_FIND2_(isa)  _rix_hash_find_u32x16_2_##isa
#define _FC_HASH_DIRECT_FIND2(isa)   _FC_HASH_DIRECT_FIND2_(isa)

#ifdef FC_HASH_DIRECT_DECLARE_HEAD
RIX_HASH_HEAD(FC_HASH_DIRECT_HT_PREFIX);
#endif

#undef _RIX_HASH_FIND_U32X16
#undef _RIX_HASH_FIND_U32X16_2
#define _RIX_HASH_FIND_U32X16(arr, val) \
    _FC_HASH_DIRECT_FIND(_FC_HASH_DIRECT_ISA(FC_HASH_DIRECT_TARGET))((arr), (val))
#define _RIX_HASH_FIND_U32X16_2(arr, val0, val1, mask0, mask1) \
    _FC_HASH_DIRECT_FIND2(_FC_HASH_DIRECT_ISA(FC_HASH_DIRECT_TARGET))((arr), \
        (val0), (val1), (mask0), (mask1))
RIX_HASH_GENERATE_STATIC_EX(FC_HASH_DIRECT_HT_PREFIX, FC_HASH_DIRECT_ENTRY,
                            key, cur_hash, FC_HASH_DIRECT_CMP_FN,
                            FC_HASH_DIRECT_HASH_FN)
#undef _RIX_HASH_FIND_U32X16_2
#undef _RIX_HASH_FIND_U32X16

#undef FC_HASH_DIRECT_DECLARE_HEAD
#undef FC_HASH_DIRECT_TARGET
#undef FC_HASH_DIRECT_HASH_FN
#undef FC_HASH_DIRECT_CMP_FN
#undef FC_HASH_DIRECT_ENTRY
#undef FC_HASH_DIRECT_HT_PREFIX
#undef _FC_HASH_DIRECT_FIND2
#undef _FC_HASH_DIRECT_FIND2_
#undef _FC_HASH_DIRECT_FIND
#undef _FC_HASH_DIRECT_FIND_
#undef _FC_HASH_DIRECT_ISA_avx512
#undef _FC_HASH_DIRECT_ISA_avx2
#undef _FC_HASH_DIRECT_ISA_sse
#undef _FC_HASH_DIRECT_ISA_gen
#undef _FC_HASH_DIRECT_ISA
#undef _FC_HASH_DIRECT_ISA_
