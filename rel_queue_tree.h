/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 */
#ifndef _REL_QUEUE_TREE_H_
#define _REL_QUEUE_TREE_H_

#include <assert.h>
#include <limits.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Index basics
 * -------------------------------------------------------------------------- */
#if defined(_MSC_VER)
#define REL_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define REL_FORCE_INLINE __attribute__((always_inline)) inline
#else
#define REL_FORCE_INLINE inline
#endif

/* 0 is the sentinel NIL index (end marker). */
#ifndef REL_NIL
#define REL_NIL ((unsigned)0u)
#endif

/* Return non-zero if i is REL_NIL. */
#define REL_IDX_IS_NIL(i) ((unsigned)(i) == REL_NIL)

/* Return non-zero if i is within [1 .. cap]. REL_NIL is NOT counted as valid.
 */
#define REL_IDX_IS_VALID(i, cap) \
    ((unsigned)(i) >= 1u && (unsigned)(i) <= (unsigned)(cap))

/* Convert element pointer (p) to 1-origin index relative to array base (base).
 * If p == NULL, return REL_NIL (0).
 * REQUIREMENT: (p) and (base) must be pointers to the SAME element type.
 */
#define REL_IDX_FROM_PTR(base, p) \
    ((unsigned)((p) ? (unsigned)((p) - (base)) + 1u : (unsigned)REL_NIL))

/* Convert 1-origin index (i) back to element pointer using array base (base).
 * If i == REL_NIL (0), return NULL.
 */
#define REL_PTR_FROM_IDX(base, i) \
    (((unsigned)(i) != REL_NIL) ? ((base) + ((unsigned)(i) - 1u)) : NULL)

/* Convert 1-origin index to 0-origin offset (undefined for REL_NIL). */
#define REL_IDX_TO_OFF0(i) ((size_t)((unsigned)(i) - 1u))

/* --------------------------------------------------------------------------
 * Diagnostics & assertions
 * -------------------------------------------------------------------------- */

/* Runtime assertion (enabled when NDEBUG is not defined). */
#ifndef REL_ASSERT
#define REL_ASSERT(cond) assert(cond)
#endif

/* Compile-time assertion wrapper (C11). */
#ifndef REL_STATIC_ASSERT
#define REL_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

/* Optional helper to validate pointer/index round-trip in debug builds.
 * Example:
 *   REL_ASSERT_RELATION(base, idx, ptr);
 */
#define REL_ASSERT_RELATION(base, idx, ptr)                                   \
    do {                                                                      \
        const unsigned __rel_idx_tmp = (unsigned)(idx);                       \
        const void *__rel_ptr_tmp = (const void *)(ptr);                      \
        REL_ASSERT((__rel_ptr_tmp == NULL) == REL_IDX_IS_NIL(__rel_idx_tmp)); \
        REL_ASSERT(REL_PTR_FROM_IDX((base), __rel_idx_tmp) == (ptr));         \
        REL_ASSERT(REL_IDX_FROM_PTR((base), (ptr)) == __rel_idx_tmp);         \
    } while (0)

/* --------------------------------------------------------------------------
 * Small utilities (pure C11, no compiler-specific builtins)
 * -------------------------------------------------------------------------- */

#ifndef REL_MIN
#define REL_MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef REL_MAX
#define REL_MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* Number of elements in a static array. */
#ifndef REL_COUNT_OF
#define REL_COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Byte offset of a field within a struct. */
#ifndef REL_OFFSET_OF
#define REL_OFFSET_OF(type, member) offsetof(type, member)
#endif

/* container_of: get struct pointer from pointer to its member. */
#ifndef REL_CONTAINER_OF
#define REL_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - REL_OFFSET_OF(type, member)))
#endif

/* Swap two variables of the same type. */
#ifndef REL_SWAP
#define REL_SWAP(T, a, b)    \
    do {                     \
        T __rel_tmp__ = (a); \
        (a) = (b);           \
        (b) = __rel_tmp__;   \
    } while (0)
#endif

/* Clamp x into [lo, hi]. */
#ifndef REL_CLAMP
#define REL_CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))
#endif

/* Align n up/down to the nearest multiple of a (a must be power of two). */
#ifndef REL_ALIGN_DOWN
#define REL_ALIGN_DOWN(n, a) (((size_t)(n)) & ~((size_t)(a) - 1u))
#endif
#ifndef REL_ALIGN_UP
#define REL_ALIGN_UP(n, a) REL_ALIGN_DOWN((size_t)(n) + ((size_t)(a) - 1u), (a))
#endif

/* --------------------------------------------------------------------------
 * Build-time sanity checks (optional)
 * -------------------------------------------------------------------------- */

/* Ensure unsigned is at least 32-bit (typical for index domain). */
REL_STATIC_ASSERT(sizeof(unsigned) >= 4, "unsigned must be >= 32 bits");

/* Ensure pointer subtraction yields ptrdiff_t (C guarantee). */
REL_STATIC_ASSERT(sizeof(ptrdiff_t) >= sizeof(void *), "ptrdiff_t must be >= pointer size");

#include <rel/rel_queue.h>
#include <rel/rel_tree.h>

#ifdef __cplusplus
}
#endif

#endif /* _REL_QUEUE_TREE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
