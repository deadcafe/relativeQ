/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 */
#ifndef _REL_QUEUE_TREE_H_
#define _REL_QUEUE_TREE_H_

#include <stddef.h>
#include <limits.h>
#include <assert.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* --------------------------------------------------------------------------
 * Index basics
 * -------------------------------------------------------------------------- */
#if defined(_MSC_VER)
# define REL_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
# define REL_FORCE_INLINE __attribute__((always_inline)) inline
#else
# define REL_FORCE_INLINE inline
#endif

/* 0 is the sentinel NIL index (end marker). */
#ifndef REL_NIL
# define REL_NIL ((unsigned)0u)
#endif

/* Return non-zero if i is REL_NIL. */
#define REL_IDX_IS_NIL(i)    ((unsigned)(i) == REL_NIL)

/* Return non-zero if i is within [1 .. cap]. REL_NIL is NOT counted as valid. */
#define REL_IDX_IS_VALID(i, cap)                                \
    ((unsigned)(i) >= 1u && (unsigned)(i) <= (unsigned)(cap))

/* Convert element pointer (p) to 1-origin index relative to array base (base).
 * If p == NULL, return REL_NIL (0).
 * REQUIREMENT: (p) and (base) must be pointers to the SAME element type.
 */
#define REL_IDX_FROM_PTR(base, p)                                       \
    ((unsigned)((p) ? (unsigned)((p) - (base)) + 1u : (unsigned)REL_NIL))
    
/* Convert 1-origin index (i) back to element pointer using array base (base).
 * If i == REL_NIL (0), return NULL.
 */
#define REL_PTR_FROM_IDX(base, i)                                       \
    (( (unsigned)(i) != REL_NIL ) ? ((base) + ((unsigned)(i) - 1u)) : NULL)
    
/* Convert 1-origin index to 0-origin offset (undefined for REL_NIL). */
#define REL_IDX_TO_OFF0(i)   ((size_t)((unsigned)(i) - 1u))

/* --------------------------------------------------------------------------
 * Diagnostics & assertions
 * -------------------------------------------------------------------------- */

/* Runtime assertion (enabled when NDEBUG is not defined). */
#ifndef REL_ASSERT
# define REL_ASSERT(cond)  assert(cond)
#endif

/* Compile-time assertion wrapper (C11). */
#ifndef REL_STATIC_ASSERT
# define REL_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

/* Optional helper to validate pointer/index round-trip in debug builds.
 * Example:
 *   REL_ASSERT_RELATION(base, idx, ptr);
 */
#define REL_ASSERT_RELATION(base, idx, ptr)                             \
    do {                                                                \
        const unsigned __rel_idx_tmp = (unsigned)(idx);                 \
        const void    *__rel_ptr_tmp = (const void*)(ptr);              \
        REL_ASSERT( (__rel_ptr_tmp == NULL) == REL_IDX_IS_NIL(__rel_idx_tmp) ); \
        REL_ASSERT( REL_PTR_FROM_IDX((base), __rel_idx_tmp) == (ptr) ); \
        REL_ASSERT( REL_IDX_FROM_PTR((base), (ptr)) == __rel_idx_tmp ); \
    } while (0)

/* --------------------------------------------------------------------------
 * Small utilities (pure C11, no compiler-specific builtins)
 * -------------------------------------------------------------------------- */

#ifndef REL_MIN
# define REL_MIN(a,b)  ( ((a) < (b)) ? (a) : (b) )
#endif
#ifndef REL_MAX
# define REL_MAX(a,b)  ( ((a) > (b)) ? (a) : (b) )
#endif

/* Number of elements in a static array. */
#ifndef REL_COUNT_OF
# define REL_COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Byte offset of a field within a struct. */
#ifndef REL_OFFSET_OF
# define REL_OFFSET_OF(type, member) offsetof(type, member)
#endif

/* container_of: get struct pointer from pointer to its member. */
#ifndef REL_CONTAINER_OF
# define REL_CONTAINER_OF(ptr, type, member) \
    ((type*)( (char*)(ptr) - REL_OFFSET_OF(type, member) ))
#endif

/* Swap two variables of the same type. */
#ifndef REL_SWAP
# define REL_SWAP(T, a, b) do { T __rel_tmp__ = (a); (a) = (b); (b) = __rel_tmp__; } while (0)
#endif

/* Clamp x into [lo, hi]. */
#ifndef REL_CLAMP
# define REL_CLAMP(x, lo, hi) ( ((x) < (lo)) ? (lo) : ( ((x) > (hi)) ? (hi) : (x) ) )
#endif

/* Align n up/down to the nearest multiple of a (a must be power of two). */
#ifndef REL_ALIGN_DOWN
# define REL_ALIGN_DOWN(n, a)  ( ( (size_t)(n) ) & ~((size_t)(a) - 1u) )
#endif
#ifndef REL_ALIGN_UP
# define REL_ALIGN_UP(n, a)    REL_ALIGN_DOWN( (size_t)(n) + ((size_t)(a) - 1u), (a) )
#endif

/* --------------------------------------------------------------------------
 * Build-time sanity checks (optional)
 * -------------------------------------------------------------------------- */

/* Ensure unsigned is at least 32-bit (typical for index domain). */
    REL_STATIC_ASSERT (sizeof (unsigned) >= 4, "unsigned must be >= 32 bits");

/* Ensure pointer subtraction yields ptrdiff_t (C guarantee). */
    REL_STATIC_ASSERT (sizeof (ptrdiff_t) >= sizeof (void *),
		       "ptrdiff_t must be >= pointer size");

/*
 * Single List
 */
#define REL_SLIST_ENTRY(type)	\
    struct {			\
        unsigned rsle_next;     \
    }

#define REL_SLIST_HEAD(name)   \
    struct name {              \
        unsigned rslh_first;   \
    }

#define REL_SLIST_HEAD_INITIALIZER(headvar) { REL_NIL }

#define REL_SLIST_EMPTY(head)        ((head)->rslh_first == REL_NIL)
#define REL_SLIST_FIRST(head, base)  ( REL_PTR_FROM_IDX((base), (head)->rslh_first) )
#define REL_SLIST_NEXT(elm, base, field)			\
    ( REL_PTR_FROM_IDX((base), (elm)->field.rsle_next) )

#define REL_SLIST_INIT(head)                    \
    do {                                        \
        (head)->rslh_first = REL_NIL;           \
    } while (0)

#define REL_SLIST_INSERT_HEAD(head, base, elm, field)                   \
    do {                                                                \
        unsigned __first = (head)->rslh_first;				\
        (elm)->field.rsle_next = __first;                               \
        (head)->rslh_first = REL_IDX_FROM_PTR((base), (elm));		\
    } while (0)

#define REL_SLIST_INSERT_AFTER(base, slistelm, elm, field)              \
    do {                                                                \
        unsigned __next = (slistelm)->field.rsle_next;			\
        (elm)->field.rsle_next = __next;                                \
        (slistelm)->field.rsle_next = REL_IDX_FROM_PTR((base), (elm));	\
    } while (0)

#define REL_SLIST_REMOVE_HEAD(head, base, field)                        \
    do {                                                                \
        unsigned __first = (head)->rslh_first;				\
        if (__first != REL_NIL) {                                       \
            unsigned __next = REL_PTR_FROM_IDX((base), __first)->field.rsle_next; \
            (head)->rslh_first = __next;                                \
        }                                                               \
    } while (0)

#define REL_SLIST_REMOVE_AFTER(base, elm, field)                        \
    do {                                                                \
        unsigned __rem = (elm)->field.rsle_next;                        \
        if (__rem != REL_NIL) {						\
            (elm)->field.rsle_next = REL_PTR_FROM_IDX((base), __rem)->field.rsle_next; \
        }                                                               \
    } while (0)
    
#define REL_SLIST_REMOVE(head, base, elm, type, field)                  \
    do {                                                                \
        if (REL_SLIST_FIRST((head), (base)) == (elm)) {			\
            REL_SLIST_REMOVE_HEAD((head), (base), field);               \
        } else {                                                        \
            struct type *__curelm = REL_SLIST_FIRST((head), (base));    \
            while (__curelm && REL_SLIST_NEXT(__curelm, (base), field) != (elm)) \
                __curelm = REL_SLIST_NEXT(__curelm, (base), field);     \
            if (__curelm) REL_SLIST_REMOVE_AFTER((base), __curelm, field); \
        }                                                               \
    } while (0)

#define REL_SLIST_FOREACH(var, head, base, field)			\
    for ((var) = REL_SLIST_FIRST((head), (base));                       \
         (var) != NULL;							\
         (var) = REL_SLIST_NEXT((var), (base), field))
    
#define REL_SLIST_FOREACH_SAFE(var, head, base, field, tvar)		\
    for ((var) = REL_SLIST_FIRST((head), (base)),                       \
             (tvar) = ((var) ? REL_SLIST_NEXT((var), (base), field) : NULL); \
         (var) != NULL;							\
         (var) = (tvar),                                                \
             (tvar) = ((var) ? REL_SLIST_NEXT((var), (base), field) : NULL))

#define REL_SLIST_FOREACH_PREVINDEX(var, varidxp, head, base, field)	\
    for ((varidxp) = &((head)->rslh_first);				\
         ((var) = REL_PTR_FROM_IDX((base), *(varidxp))) != NULL;        \
         (varidxp) = &((var)->field.rsle_next))

/*
 * LIST
 */
#define REL_LIST_ENTRY(type)  \
    struct {		      \
        unsigned rle_next;    \
        unsigned rle_prev;    \
    }

#define REL_LIST_HEAD(name)  \
    struct name {            \
        unsigned rlh_first;  \
    }

#define REL_LIST_HEAD_INITIALIZER(headvar) { REL_NIL }

#define REL_LIST_EMPTY(head)        ((head)->rlh_first == REL_NIL)
#define REL_LIST_FIRST(head, base)  ( REL_PTR_FROM_IDX((base), (head)->rlh_first) )
#define REL_LIST_NEXT(elm, base, field)			\
    ( REL_PTR_FROM_IDX((base), (elm)->field.rle_next) )

#define REL_LIST_INIT(head)                     \
    do {                                        \
        (head)->rlh_first = REL_NIL;            \
    } while (0)

#define REL_LIST_INSERT_HEAD(head, base, elm, field)                    \
    do {                                                                \
        unsigned __idx   = REL_IDX_FROM_PTR((base), (elm));             \
        unsigned __first = (head)->rlh_first;				\
        (elm)->field.rle_prev = REL_NIL;                                \
        (elm)->field.rle_next = __first;                                \
        if (__first != REL_NIL)						\
            REL_PTR_FROM_IDX((base), __first)->field.rle_prev = __idx;	\
        (head)->rlh_first = __idx;                                      \
    } while (0)

#define REL_LIST_INSERT_AFTER(head, base, listelm, elm, field)          \
    do {                                                                \
        unsigned __listidx = REL_IDX_FROM_PTR((base), (listelm));       \
        unsigned __idx     = REL_IDX_FROM_PTR((base), (elm));		\
        unsigned __next    = (listelm)->field.rle_next;			\
        (elm)->field.rle_prev = __listidx;                              \
        (elm)->field.rle_next = __next;					\
        (listelm)->field.rle_next = __idx;                              \
        if (__next != REL_NIL)						\
            REL_PTR_FROM_IDX((base), __next)->field.rle_prev = __idx;   \
    } while (0)

#define REL_LIST_INSERT_BEFORE(head, base, listelm, elm, field)         \
    do {                                                                \
        unsigned __listidx = REL_IDX_FROM_PTR((base), (listelm));       \
        unsigned __idx     = REL_IDX_FROM_PTR((base), (elm));		\
        unsigned __prev    = (listelm)->field.rle_prev;			\
        (elm)->field.rle_prev = __prev;					\
        (elm)->field.rle_next = __listidx;                              \
        (listelm)->field.rle_prev = __idx;                              \
        if (__prev != REL_NIL)						\
            REL_PTR_FROM_IDX((base), __prev)->field.rle_next = __idx;   \
        else								\
            (head)->rlh_first = __idx;					\
    } while (0)

#define REL_LIST_REMOVE(head, base, elm, field)                         \
    do {                                                                \
        unsigned __next = (elm)->field.rle_next;                        \
        unsigned __prev = (elm)->field.rle_prev;                        \
        if (__next != REL_NIL)						\
            REL_PTR_FROM_IDX((base), __next)->field.rle_prev = __prev;	\
        if (__prev != REL_NIL)						\
            REL_PTR_FROM_IDX((base), __prev)->field.rle_next = __next;	\
        else								\
            (head)->rlh_first = __next;					\
        (elm)->field.rle_next = REL_NIL;                                \
        (elm)->field.rle_prev = REL_NIL;                                \
    } while (0)

#define REL_LIST_SWAP(head1, head2, base, type, field)                  \
    do {                                                                \
        unsigned __f1 = (head1)->rlh_first;                             \
        unsigned __f2 = (head2)->rlh_first;                             \
        (head1)->rlh_first = __f2;                                      \
        (head2)->rlh_first = __f1;                                      \
        do {                                                            \
            if ((head1)->rlh_first != REL_NIL)				\
                REL_PTR_FROM_IDX((base), (head1)->rlh_first)->field.rle_prev = REL_NIL; \
        } while (0);                                                    \
        do {                                                            \
            if ((head2)->rlh_first != REL_NIL)				\
                REL_PTR_FROM_IDX((base), (head2)->rlh_first)->field.rle_prev = REL_NIL; \
        } while (0);                                                    \
        (void)(base);                                                   \
        (void)(sizeof(struct type));                                    \
    } while (0)

#define REL_LIST_FOREACH(var, head, base, field)			\
    for ((var) = REL_LIST_FIRST((head), (base));                        \
         (var) != NULL;							\
         (var) = REL_LIST_NEXT((var), (base), field))

#define REL_LIST_FOREACH_SAFE(var, head, base, field, tvar)		\
    for ((var) = REL_LIST_FIRST((head), (base)),                        \
             (tvar) = ((var) ? REL_LIST_NEXT((var), (base), field) : NULL); \
         (var) != NULL;							\
         (var) = (tvar),                                                \
             (tvar) = ((var) ? REL_LIST_NEXT((var), (base), field) : NULL))

/*
 * STAILQ
 */
#define REL_STAILQ_ENTRY(type)	 \
    struct {			 \
        unsigned rsqe_next;      \
    }

#define REL_STAILQ_HEAD(name)	\
    struct name {               \
        unsigned rsqh_first;    \
        unsigned rsqh_last;     \
    }

#define REL_STAILQ_HEAD_INITIALIZER(headvar) { REL_NIL, REL_NIL }

#define REL_STAILQ_EMPTY(head)        ((head)->rsqh_first == REL_NIL)
#define REL_STAILQ_FIRST(head, base)  ( REL_PTR_FROM_IDX((base), (head)->rsqh_first) )
#define REL_STAILQ_LAST(head, base)   ( REL_PTR_FROM_IDX((base), (head)->rsqh_last)  )
#define REL_STAILQ_NEXT(head, base, elm, field)                 \
    ( REL_PTR_FROM_IDX((base), (elm)->field.rsqe_next) )

#define REL_STAILQ_INIT(head)                   \
    do {                                        \
        (head)->rsqh_first = REL_NIL;           \
        (head)->rsqh_last  = REL_NIL;           \
    } while (0)

#define REL_STAILQ_INSERT_HEAD(head, base, elm, field)                  \
    do {                                                                \
        unsigned __idx   = REL_IDX_FROM_PTR((base), (elm));             \
        unsigned __first = (head)->rsqh_first;				\
        (elm)->field.rsqe_next = __first;                               \
        (head)->rsqh_first = __idx;                                     \
        if (__first == REL_NIL) {                                       \
            (head)->rsqh_last = __idx;					\
        }                                                               \
    } while (0)

#define REL_STAILQ_INSERT_TAIL(head, base, elm, field)                  \
    do {                                                                \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));               \
        (elm)->field.rsqe_next = REL_NIL;                               \
        if ((head)->rsqh_last != REL_NIL) {                             \
            REL_PTR_FROM_IDX((base), (head)->rsqh_last)->field.rsqe_next = __idx; \
        } else {                                                        \
            (head)->rsqh_first = __idx;					\
        }                                                               \
        (head)->rsqh_last = __idx;                                      \
    } while (0)

#define REL_STAILQ_INSERT_AFTER(head, base, tqelm, elm, field)          \
    do {                                                                \
        unsigned __idx  = REL_IDX_FROM_PTR((base), (elm));              \
        unsigned __next = (tqelm)->field.rsqe_next;                     \
        (elm)->field.rsqe_next = __next;                                \
        (tqelm)->field.rsqe_next = __idx;                               \
        if (__next == REL_NIL) {                                        \
            (head)->rsqh_last = __idx;					\
        }                                                               \
    } while (0)

#define REL_STAILQ_REMOVE_HEAD(head, base, field)                       \
    do {                                                                \
        unsigned __first = (head)->rsqh_first;				\
        if (__first != REL_NIL) {                                       \
            unsigned __next = REL_PTR_FROM_IDX((base), __first)->field.rsqe_next; \
            (head)->rsqh_first = __next;                                \
            if (__next == REL_NIL)                                      \
                (head)->rsqh_last = REL_NIL;                            \
        }                                                               \
    } while (0)

#define REL_STAILQ_REMOVE_AFTER(head, base, elm, field)                 \
    do {                                                                \
        unsigned __rem = (elm)->field.rsqe_next;                        \
        if (__rem != REL_NIL) {						\
            unsigned __nn = REL_PTR_FROM_IDX((base), __rem)->field.rsqe_next; \
            (elm)->field.rsqe_next = __nn;                              \
            if (__nn == REL_NIL)                                        \
                (head)->rsqh_last = REL_IDX_FROM_PTR((base), (elm));    \
        }                                                               \
    } while (0)

#define REL_STAILQ_REMOVE(head, base, elm, type, field)                 \
    do {                                                                \
        if (REL_STAILQ_FIRST((head), (base)) == (elm)) {                \
            REL_STAILQ_REMOVE_HEAD((head), (base), field);              \
        } else {                                                        \
            struct type *__curelm = REL_STAILQ_FIRST((head), (base));   \
            while (__curelm &&                                          \
                   REL_PTR_FROM_IDX((base), __curelm->field.rsqe_next) != (elm)) { \
                __curelm = REL_STAILQ_NEXT((head), (base), __curelm, field); \
            }                                                           \
            if (__curelm) {                                             \
                REL_STAILQ_REMOVE_AFTER((head), (base), __curelm, field); \
            }                                                           \
        }                                                               \
    } while (0)

#define REL_STAILQ_REMOVE_HEAD_UNTIL(head, base, elm, field)            \
    do {                                                                \
        unsigned __next = (elm) ? (elm)->field.rsqe_next : REL_NIL;     \
        (head)->rsqh_first = __next;					\
        if (__next == REL_NIL)                                          \
            (head)->rsqh_last = REL_NIL;                                \
    } while (0)

#define REL_STAILQ_CONCAT(head1, head2, base, field)                    \
    do {                                                                \
        if (!REL_STAILQ_EMPTY((head2))) {                               \
            if (!REL_STAILQ_EMPTY((head1))) {                           \
                REL_PTR_FROM_IDX((base), (head1)->rsqh_last)->field.rsqe_next = (head2)->rsqh_first; \
            } else {                                                    \
                (head1)->rsqh_first = (head2)->rsqh_first;              \
            }                                                           \
            (head1)->rsqh_last = (head2)->rsqh_last;                    \
            (head2)->rsqh_first = REL_NIL;                              \
            (head2)->rsqh_last  = REL_NIL;                              \
        }                                                               \
    } while (0)

#define REL_STAILQ_SWAP(head1, head2, base)                             \
    do {                                                                \
        unsigned __f = (head1)->rsqh_first;                             \
        unsigned __l = (head1)->rsqh_last;                              \
        (void)(base);                                                   \
        (head1)->rsqh_first = (head2)->rsqh_first;                      \
        (head1)->rsqh_last  = (head2)->rsqh_last;                       \
        (head2)->rsqh_first = __f;                                      \
        (head2)->rsqh_last  = __l;                                      \
    } while (0)

#define REL_STAILQ_FOREACH(var, head, base, field)			\
  for ((var) = REL_STAILQ_FIRST((head), (base));			\
       (var) != NULL;							\
       (var) = REL_STAILQ_NEXT((head), (base), (var), field))

#define REL_STAILQ_FOREACH_SAFE(var, head, base, field, tvar)		\
    for ((var) = REL_STAILQ_FIRST((head), (base)),			\
             (tvar) = ((var) ? REL_STAILQ_NEXT((head), (base), (var), field) : NULL); \
         (var) != NULL;							\
         (var) = (tvar),                                                \
             (tvar) = ((var) ? REL_STAILQ_NEXT((head), (base), (var), field) : NULL))

/*
 * TAILQ
 */
#define REL_TAILQ_ENTRY(type)  \
    struct {		       \
        unsigned rqe_next;     \
        unsigned rqe_prev;     \
    }

#define REL_TAILQ_HEAD(name)  \
    struct name {             \
        unsigned rqh_first;   \
        unsigned rqh_last;    \
    }

#define REL_TAILQ_HEAD_INITIALIZER(headvar) { REL_NIL, REL_NIL }

#define REL_TAILQ_INIT(head)                    \
    do {                                        \
        (head)->rqh_first = REL_NIL;            \
        (head)->rqh_last  = REL_NIL;            \
    } while (0)

#define REL_TAILQ_RESET(head) REL_TAILQ_INIT(head)

#define REL_TAILQ_EMPTY(head)        ((head)->rqh_first == REL_NIL)
#define REL_TAILQ_FIRST(head, base)  ( REL_PTR_FROM_IDX((base), (head)->rqh_first) )
#define REL_TAILQ_LAST(head, base)   ( REL_PTR_FROM_IDX((base), (head)->rqh_last)  )

#define REL_TAILQ_NEXT(head, base, elm, field)		\
  ( REL_PTR_FROM_IDX((base), (elm)->field.rqe_next) )
    
#define REL_TAILQ_PREV(head, base, elm, field)		\
    (((elm)->field.rqe_prev == REL_NIL) ? NULL		\
     : REL_PTR_FROM_IDX((base), (elm)->field.rqe_prev))

#define REL_TAILQ_FOREACH(var, head, base, field)                       \
    for ((var) = REL_TAILQ_FIRST((head), (base));                       \
         (var) != NULL;							\
         (var) = REL_TAILQ_NEXT((head), (base), (var), field))

#define REL_TAILQ_FOREACH_SAFE(var, head, base, field, tvar)            \
    for ((var) = REL_TAILQ_FIRST((head), (base)),                       \
             (tvar) = ((var) ? REL_TAILQ_NEXT((head), (base), (var), field) : NULL); \
         (var) != NULL;							\
         (var) = (tvar),                                                \
             (tvar) = ((var) ? REL_TAILQ_NEXT((head), (base), (var), field) : NULL))

#define REL_TAILQ_FOREACH_REVERSE(var, head, base, field)               \
    for ((var) = REL_TAILQ_LAST((head), (base));                        \
         (var) != NULL;							\
         (var) = REL_TAILQ_PREV((head), (base), (var), field))

#define __REL_SET_NEXT(elm, field, v_idx) do { (elm)->field.rqe_next = (v_idx); } while (0)
#define __REL_SET_PREV(elm, field, v_idx) do { (elm)->field.rqe_prev = (v_idx); } while (0)

#define REL_TAILQ_INSERT_HEAD(head, base, elm, field)                   \
    do {                                                                \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));               \
        unsigned __first = (head)->rqh_first;				\
        __REL_SET_PREV((elm), field, REL_NIL);				\
        __REL_SET_NEXT((elm), field, __first);				\
        if (__first != REL_NIL) {                                       \
            __REL_SET_PREV( REL_PTR_FROM_IDX((base), __first), field, __idx ); \
        } else {                                                        \
            (head)->rqh_last = __idx;                                   \
        }                                                               \
        (head)->rqh_first = __idx;                                      \
    } while (0)

#define REL_TAILQ_INSERT_TAIL(head, base, elm, field)                   \
    do {                                                                \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));               \
        unsigned __last = (head)->rqh_last;                             \
        __REL_SET_NEXT((elm), field, REL_NIL);				\
        __REL_SET_PREV((elm), field, __last);				\
        if (__last != REL_NIL) {                                        \
            __REL_SET_NEXT( REL_PTR_FROM_IDX((base), __last), field, __idx ); \
        } else {                                                        \
            (head)->rqh_first = __idx;					\
        }                                                               \
        (head)->rqh_last = __idx;                                       \
    } while (0)

#define REL_TAILQ_INSERT_AFTER(head, base, listelm, elm, field)         \
    do {                                                                \
        unsigned __listidx = REL_IDX_FROM_PTR((base), (listelm));       \
        unsigned __idx     = REL_IDX_FROM_PTR((base), (elm));		\
        unsigned __next    = (listelm)->field.rqe_next;			\
        __REL_SET_NEXT((elm), field, __next);				\
        __REL_SET_PREV((elm), field, __listidx);                        \
        __REL_SET_NEXT((listelm), field, __idx);                        \
        if (__next != REL_NIL) {                                        \
            __REL_SET_PREV( REL_PTR_FROM_IDX((base), __next), field, __idx ); \
        } else {                                                        \
            (head)->rqh_last = __idx;                                   \
        }                                                               \
    } while (0)

#define REL_TAILQ_INSERT_BEFORE(head, base, listelm, elm, field)        \
    do {                                                                \
        unsigned __listidx = REL_IDX_FROM_PTR((base), (listelm));       \
        unsigned __idx     = REL_IDX_FROM_PTR((base), (elm));		\
        unsigned __prev    = (listelm)->field.rqe_prev;			\
        __REL_SET_PREV((elm), field, __prev);				\
        __REL_SET_NEXT((elm), field, __listidx);                        \
        __REL_SET_PREV((listelm), field, __idx);                        \
        if (__prev != REL_NIL) {                                        \
            __REL_SET_NEXT( REL_PTR_FROM_IDX((base), __prev), field, __idx ); \
        } else {                                                        \
            (head)->rqh_first = __idx;					\
        }                                                               \
    } while (0)

#define REL_TAILQ_REMOVE(head, base, elm, field)                        \
    do {                                                                \
        unsigned __idx  = REL_IDX_FROM_PTR((base), (elm));              \
        unsigned __next = (elm)->field.rqe_next;                        \
        unsigned __prev = (elm)->field.rqe_prev;                        \
        if (__next != REL_NIL) {                                        \
            __REL_SET_PREV( REL_PTR_FROM_IDX((base), __next), field, __prev ); \
        } else {                                                        \
            (head)->rqh_last = __prev;					\
        }                                                               \
        if (__prev != REL_NIL) {                                        \
            __REL_SET_NEXT( REL_PTR_FROM_IDX((base), __prev), field, __next ); \
        } else {                                                        \
            (head)->rqh_first = __next;					\
        }                                                               \
        __REL_SET_NEXT((elm), field, REL_NIL);				\
        __REL_SET_PREV((elm), field, REL_NIL);				\
        (void)__idx;							\
    } while (0)

#define REL_TAILQ_CONCAT(head1, head2, base, field)                     \
    do {                                                                \
        if (!REL_TAILQ_EMPTY((head2))) {                                \
            if (!REL_TAILQ_EMPTY((head1))) {                            \
                unsigned __h1_last = (head1)->rqh_last;                 \
                unsigned __h2_first = (head2)->rqh_first;               \
                __REL_SET_NEXT( REL_PTR_FROM_IDX((base), __h1_last), field, __h2_first ); \
                __REL_SET_PREV( REL_PTR_FROM_IDX((base), __h2_first), field, __h1_last ); \
            } else {                                                    \
                (head1)->rqh_first = (head2)->rqh_first;                \
            }                                                           \
            (head1)->rqh_last = (head2)->rqh_last;                      \
            (head2)->rqh_first = REL_NIL;                               \
            (head2)->rqh_last  = REL_NIL;                               \
        }                                                               \
    } while (0)

#define REL_TAILQ_SWAP(head1, head2, base)                              \
    do {                                                                \
        unsigned __f = (head1)->rqh_first;                              \
        unsigned __l = (head1)->rqh_last;                               \
        (void)(base);                                                   \
        (head1)->rqh_first = (head2)->rqh_first;                        \
        (head1)->rqh_last  = (head2)->rqh_last;				\
        (head2)->rqh_first = __f;                                       \
        (head2)->rqh_last  = __l;                                       \
    } while (0)

/*
 * CIRCLEQ
 */
#define REL_CIRCLEQ_ENTRY(type)				\
    struct {                                            \
        unsigned rce_next;                              \
        unsigned rce_prev;                              \
    }
    
#define REL_CIRCLEQ_HEAD(name)			\
    struct name {                               \
        unsigned rch_first;                     \
    }

#define REL_CIRCLEQ_HEAD_INITIALIZER(headvar) { REL_NIL }

#define REL_CIRCLEQ_EMPTY(head)        ((head)->rch_first == REL_NIL)
#define REL_CIRCLEQ_FIRST(head, base)  ( REL_PTR_FROM_IDX((base),(head)->rch_first) )
    
#define REL_CIRCLEQ_LAST(head, base, field)				\
    ( REL_CIRCLEQ_EMPTY(head) ? NULL :					\
      REL_PTR_FROM_IDX((base), REL_CIRCLEQ_FIRST((head),(base))->field.rce_prev) )

#define REL_CIRCLEQ_NEXT(elm, base, field) ( REL_PTR_FROM_IDX((base),(elm)->field.rce_next) )
#define REL_CIRCLEQ_PREV(elm, base, field) ( REL_PTR_FROM_IDX((base),(elm)->field.rce_prev) )

#define REL_CIRCLEQ_INIT(head) do { (head)->rch_first = REL_NIL; } while (0)

#define REL_CIRCLEQ_INSERT_HEAD(head, base, elm, field)                 \
    do {                                                                \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));               \
        if (REL_CIRCLEQ_EMPTY(head)) {					\
            (head)->rch_first = __idx;					\
            (elm)->field.rce_next = __idx;                              \
            (elm)->field.rce_prev = __idx;                              \
        } else {                                                        \
            unsigned __first = (head)->rch_first;                       \
            unsigned __last  = REL_PTR_FROM_IDX((base), __first)->field.rce_prev; \
            (elm)->field.rce_next = __first;                            \
            (elm)->field.rce_prev = __last;                             \
            REL_PTR_FROM_IDX((base), __first)->field.rce_prev = __idx;  \
            REL_PTR_FROM_IDX((base), __last )->field.rce_next = __idx;  \
            (head)->rch_first = __idx;					\
        }                                                               \
    } while (0)

#define REL_CIRCLEQ_INSERT_TAIL(head, base, elm, field)                 \
    do {                                                                \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));               \
        if (REL_CIRCLEQ_EMPTY(head)) {					\
            (head)->rch_first = __idx;					\
            (elm)->field.rce_next = __idx;                              \
            (elm)->field.rce_prev = __idx;                              \
        } else {                                                        \
            unsigned __first = (head)->rch_first;                       \
            unsigned __last  = REL_PTR_FROM_IDX((base), __first)->field.rce_prev; \
            (elm)->field.rce_next = __first;                            \
            (elm)->field.rce_prev = __last;                             \
            REL_PTR_FROM_IDX((base), __first)->field.rce_prev = __idx;  \
            REL_PTR_FROM_IDX((base), __last )->field.rce_next = __idx;  \
        }                                                               \
    } while (0)

#define REL_CIRCLEQ_INSERT_AFTER(head, base, listelm, elm, field)       \
    do {                                                                \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));               \
        unsigned __after = REL_IDX_FROM_PTR((base), (listelm));		\
        unsigned __next  = (listelm)->field.rce_next;			\
        (elm)->field.rce_prev = __after;                                \
        (elm)->field.rce_next = __next;					\
        (listelm)->field.rce_next = __idx;                              \
        REL_PTR_FROM_IDX((base), __next)->field.rce_prev = __idx;       \
    } while (0)

#define REL_CIRCLEQ_INSERT_BEFORE(head, base, listelm, elm, field)      \
    do {                                                                \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));               \
        unsigned __before = REL_IDX_FROM_PTR((base), (listelm));        \
        unsigned __prev   = (listelm)->field.rce_prev;			\
        (elm)->field.rce_next = __before;                               \
        (elm)->field.rce_prev = __prev;					\
        REL_PTR_FROM_IDX((base), __prev)->field.rce_next = __idx;       \
        (listelm)->field.rce_prev = __idx;                              \
        if ((head)->rch_first == __before)                              \
            (head)->rch_first = __idx;                                  \
    } while (0)

#define REL_CIRCLEQ_REMOVE(head, base, elm, field)                      \
    do {                                                                \
        unsigned __idx  = REL_IDX_FROM_PTR((base), (elm));              \
        unsigned __next = (elm)->field.rce_next;                        \
        unsigned __prev = (elm)->field.rce_prev;                        \
        if (__next == __idx) {						\
            (head)->rch_first = REL_NIL;                                \
        } else {                                                        \
            REL_PTR_FROM_IDX((base),__prev)->field.rce_next = __next;   \
            REL_PTR_FROM_IDX((base),__next)->field.rce_prev = __prev;   \
            if ((head)->rch_first == __idx)                             \
                (head)->rch_first = __next;                             \
        }                                                               \
        (elm)->field.rce_next = REL_NIL;                                \
        (elm)->field.rce_prev = REL_NIL;                                \
    } while (0)

#define REL_CIRCLEQ_FOREACH(var, head, base, field)			\
    for ( (var) = REL_CIRCLEQ_FIRST((head),(base));			\
          (var) != NULL;                                                \
          (var) = ( ((var)->field.rce_next == (head)->rch_first)        \
                    ? NULL : REL_PTR_FROM_IDX((base), (var)->field.rce_next) ) )

#define REL_CIRCLEQ_FOREACH_REVERSE(var, head, base, field)		\
    for ( unsigned __rq_start = (head)->rch_first                       \
              ? REL_PTR_FROM_IDX((base), (head)->rch_first)->field.rce_prev \
              : REL_NIL,                                                \
              __rq_i = __rq_start;                                      \
          __rq_i != REL_NIL && ((var) = REL_PTR_FROM_IDX((base), __rq_i), 1); \
          __rq_i = ( REL_PTR_FROM_IDX((base), __rq_i)->field.rce_prev == __rq_start \
                     ? REL_NIL						\
                     : REL_PTR_FROM_IDX((base), __rq_i)->field.rce_prev ) )

#define REL_CIRCLEQ_FOREACH_SAFE(var, head, base, field, tvar)		\
    for ( unsigned __rq_start = (head)->rch_first,			\
              __rq_i     = __rq_start,                                  \
              __rq_next  = REL_NIL;                                     \
          __rq_i != REL_NIL &&						\
              ((var) = REL_PTR_FROM_IDX((base), __rq_i),                \
               __rq_next = (var)->field.rce_next,                       \
               (tvar) = REL_PTR_FROM_IDX((base), __rq_next), 1);        \
          __rq_i = ( (__rq_next == __rq_start) ||                       \
                     (__rq_start && REL_PTR_FROM_IDX((base), __rq_start)->field.rce_next == REL_NIL) \
                     ? REL_NIL : __rq_next ),				\
              __rq_start = ( REL_IDX_FROM_PTR((base), (var)) == __rq_start \
                             ? __rq_next : __rq_start ) )

#define REL_CIRCLEQ_FOREACH_REVERSE_SAFE(var, head, base, field, tvar)	\
    for ( unsigned __rq_start = (head)->rch_first                       \
              ? REL_PTR_FROM_IDX((base), (head)->rch_first)->field.rce_prev \
              : REL_NIL,                                                \
              __rq_i     = __rq_start,					\
              __rq_prev  = REL_NIL;                                     \
          __rq_i != REL_NIL &&						\
              ((var) = REL_PTR_FROM_IDX((base), __rq_i),                \
               __rq_prev = (var)->field.rce_prev,                       \
               (tvar) = REL_PTR_FROM_IDX((base), __rq_prev), 1);        \
          __rq_i = ( (__rq_prev == __rq_start) ||                       \
                     (__rq_start && REL_PTR_FROM_IDX((base), __rq_start)->field.rce_next == REL_NIL) \
                     ? REL_NIL : __rq_prev ),				\
              __rq_start = ( REL_IDX_FROM_PTR((base), (var)) == __rq_start \
                             ? __rq_prev : __rq_start ) )

/*
 * RB Tree
 */
#define REL_RB_HEAD(name)			\
    struct name {                               \
        unsigned rrh_root;                      \
    }

#define REL_RB_HEAD_INITIALIZER(var) { REL_NIL }

#define REL_RB_INIT(head)                               \
    do {                                                \
        (head)->rrh_root = REL_NIL;                     \
    } while (0)

#define REL_RB_ENTRY(type)                      \
    struct {                                    \
        unsigned rbe_parent;                    \
        unsigned rbe_left;                      \
        unsigned rbe_right;                     \
        unsigned rbe_color;                     \
    }

#define REL_RB_RED   ((unsigned) 0)
#define REL_RB_BLACK ((unsigned) 1)

#define REL_RB_ROOT(head, base)         ( REL_PTR_FROM_IDX((base), (head)->rrh_root) )
#define REL_RB_EMPTY(head)              ( (head)->rrh_root == REL_NIL )
#define REL_RB_PARENT(elm, base, field) ( REL_PTR_FROM_IDX((base), (elm)->field.rbe_parent) )
#define REL_RB_LEFT(elm, base, field)   ( REL_PTR_FROM_IDX((base), (elm)->field.rbe_left) )
#define REL_RB_RIGHT(elm, base, field)  ( REL_PTR_FROM_IDX((base), (elm)->field.rbe_right) )
#define REL_RB_COLOR(elm, field)        ( (elm)->field.rbe_color )

#define REL_RB_PROTOTYPE(name, type, field, cmp)			\
    type * name##_REL_RB_INSERT(struct name *head, type *base, type *elm); \
    type * name##_REL_RB_REMOVE(struct name *head, type *base, type *elm); \
    type * name##_REL_RB_FIND  (struct name *head, type *base, const type *key); \
    type * name##_REL_RB_NFIND (struct name *head, type *base, const type *key); \
    type * name##_REL_RB_MINMAX(struct name *head, type *base, int dir); \
    type * name##_REL_RB_NEXT  (type *base, type *elm);			\
    type * name##_REL_RB_PREV  (type *base, type *elm);

#define REL_RB_INSERT(name, head, base, elm)       name##_REL_RB_INSERT((head),(base), (elm))
#define REL_RB_REMOVE(name, head, base, elm)       name##_REL_RB_REMOVE((head),(base), (elm))
#define REL_RB_FIND(name, head, base, key)         name##_REL_RB_FIND  ((head),(base), (key))
#define REL_RB_NFIND(name, head, base, key)        name##_REL_RB_NFIND ((head),(base), (key))
#define REL_RB_MIN(name, head, base)               name##_REL_RB_MINMAX((head),(base), -1)
#define REL_RB_MAX(name, head, base)               name##_REL_RB_MINMAX((head),(base), +1)
#define REL_RB_NEXT(name, base, elm)               name##_REL_RB_NEXT  ((base),(elm))
#define REL_RB_PREV(name, base, elm)               name##_REL_RB_PREV  ((base),(elm))

#define REL_RB_FOREACH(x, name, head, base)				\
    for ((x) = REL_RB_MIN(name, (head), (base));                        \
         (x) != NULL;                                                   \
         (x) = name##_REL_RB_NEXT((base), (x)))

#define REL_RB_FOREACH_REVERSE(x, name, head, base)			\
    for ((x) = REL_RB_MAX(name, (head), (base));                        \
         (x) != NULL;                                                   \
         (x) = name##_REL_RB_PREV((base), (x)))

#define REL_RB_GENERATE(name, type, field, cmp)				\
    static REL_FORCE_INLINE                                             \
    unsigned name##_idx(type *base, const type *p) {			\
        return REL_IDX_FROM_PTR(base, (type*)p);                        \
    }									\
    static REL_FORCE_INLINE                                             \
    type * name##_ptr(type *base, unsigned idx){                        \
        return REL_PTR_FROM_IDX(base, idx);                             \
    }									\
    static REL_FORCE_INLINE                                             \
    unsigned name##_color_idx(type *base, unsigned idx) {               \
        return (idx == REL_NIL) ? REL_RB_BLACK : name##_ptr(base, idx)->field.rbe_color; \
    }									\
    static REL_FORCE_INLINE                                             \
    void name##_set_color_idx(type *base, unsigned idx, unsigned char c) { \
        if (idx != REL_NIL)                                             \
            name##_ptr(base, idx)->field.rbe_color = c;                 \
    }									\
    static REL_FORCE_INLINE                                             \
    unsigned name##_left_idx(type *base, unsigned idx) {                \
        return (idx == REL_NIL) ? REL_NIL : name##_ptr(base, idx)->field.rbe_left; \
    }									\
    static REL_FORCE_INLINE                                             \
    unsigned name##_right_idx(type *base, unsigned idx) {               \
        return (idx == REL_NIL) ? REL_NIL : name##_ptr(base, idx)->field.rbe_right; \
    }									\
    static REL_FORCE_INLINE                                             \
    unsigned name##_parent_idx(type *base, unsigned idx) {              \
        return (idx == REL_NIL) ? REL_NIL : name##_ptr(base, idx)->field.rbe_parent; \
    }									\
    static REL_FORCE_INLINE                                             \
    void name##_set_left(type *base, unsigned p, unsigned c) {		\
        if (p != REL_NIL)                                               \
            name##_ptr(base, p)->field.rbe_left = c;                    \
        if (c != REL_NIL)                                               \
            name##_ptr(base, c)->field.rbe_parent = p;                  \
    }									\
    static REL_FORCE_INLINE                                             \
    void name##_set_right(type *base, unsigned p, unsigned c) {		\
        if (p!=REL_NIL)                                                 \
            name##_ptr(base, p)->field.rbe_right = c;                   \
        if (c!=REL_NIL)                                                 \
            name##_ptr(base, c)->field.rbe_parent = p;                  \
    }									\
    static REL_FORCE_INLINE                                             \
    void name##_transplant(struct name *head, type *base,               \
                           unsigned u, unsigned v) {                    \
        unsigned up = name##_parent_idx(base, u);                       \
        if (up == REL_NIL) {                                            \
            (head)->rrh_root = v;                                       \
            if (v != REL_NIL)                                           \
                name##_ptr(base, v)->field.rbe_parent = REL_NIL;        \
        } else if (u == name##_left_idx(base, up)) {                    \
            name##_set_left(base, up, v);                               \
        } else {                                                        \
            name##_set_right(base, up, v);                              \
        }                                                               \
    }									\
    static REL_FORCE_INLINE                                             \
    void name##_rotate_left(struct name *head, type *base, unsigned x) { \
        unsigned y  = name##_right_idx(base, x);                        \
        unsigned yL = name##_left_idx(base, y);				\
        name##_set_right(base, x, yL);					\
        unsigned xp = name##_parent_idx(base, x);                       \
        if (xp == REL_NIL) {                                            \
            (head)->rrh_root = y;                                       \
            if (y != REL_NIL) name##_ptr(base, y)->field.rbe_parent = REL_NIL; \
        } else if (x == name##_left_idx(base, xp)) {                    \
            name##_set_left(base, xp, y);                               \
        } else {                                                        \
            name##_set_right(base,xp, y);                               \
        }                                                               \
        name##_set_left(base, y, x);					\
    }									\
    static REL_FORCE_INLINE                                             \
    void name##_rotate_right(struct name *head, type *base, unsigned x) { \
        unsigned y  = name##_left_idx(base, x);				\
        unsigned yR = name##_right_idx(base, y);                        \
        name##_set_left(base, x, yR);					\
        unsigned xp = name##_parent_idx(base, x);                       \
        if (xp == REL_NIL) {                                            \
            (head)->rrh_root = y;                                       \
            if (y != REL_NIL)                                           \
                name##_ptr(base, y)->field.rbe_parent = REL_NIL;        \
        } else if (x == name##_left_idx(base, xp)) {                    \
            name##_set_left(base, xp, y);                               \
        } else {                                                        \
            name##_set_right(base, xp, y);                              \
        }                                                               \
        name##_set_right(base, y, x);					\
    }									\
    type * name##_REL_RB_INSERT(struct name *head, type *base, type *elm) { \
        unsigned z = name##_idx(base, elm);                             \
        /* find place */                                                \
        unsigned y = REL_NIL;						\
        unsigned x = (head)->rrh_root;					\
        while (x != REL_NIL) {						\
            y = x;                                                      \
            int c = (cmp)(elm, name##_ptr(base, x));                    \
            if (c < 0)                                                  \
                x = name##_left_idx(base, x);                           \
            else if (c > 0)                                             \
                x = name##_right_idx(base, x);                          \
            else                                                        \
                return name##_ptr(base, x);                             \
        }                                                               \
        /* link z under y */						\
        elm->field.rbe_parent = y;                                      \
        elm->field.rbe_left  = REL_NIL;                                 \
        elm->field.rbe_right = REL_NIL;                                 \
        elm->field.rbe_color = REL_RB_RED;                              \
        if (y == REL_NIL)                                               \
            (head)->rrh_root = z;                                       \
        else if ((cmp)(elm, name##_ptr(base,y)) < 0)                    \
            name##_set_left (base, y, z);                               \
        else                                                            \
            name##_set_right(base, y, z);                               \
        /* fix-up */							\
        while ((z != (head)->rrh_root) &&                               \
               name##_color_idx(base, name##_parent_idx(base, z)) == REL_RB_RED ) { \
            unsigned p = name##_parent_idx(base, z);                    \
            unsigned g = name##_parent_idx(base, p);                    \
            if (p == name##_left_idx(base, g)) {                        \
                unsigned u = name##_right_idx(base, g); /* uncle */     \
                if (name##_color_idx(base, u) == REL_RB_RED) {          \
                    name##_set_color_idx(base, p, REL_RB_BLACK);        \
                    name##_set_color_idx(base, u, REL_RB_BLACK);        \
                    name##_set_color_idx(base, g, REL_RB_RED);          \
                    z = g;                                              \
                } else {                                                \
                    if (z == name##_right_idx(base, p)) {               \
                        z = p;                                          \
                        name##_rotate_left(head, base, z);              \
                        p = name##_parent_idx(base, z);                 \
                        g = name##_parent_idx(base, p);                 \
                    }                                                   \
                    name##_set_color_idx(base, p, REL_RB_BLACK);        \
                    name##_set_color_idx(base, g, REL_RB_RED);          \
                    name##_rotate_right(head, base, g);                 \
                }                                                       \
            } else { /* mirror */                                       \
                unsigned u = name##_left_idx(base, g);                  \
                if (name##_color_idx(base, u) == REL_RB_RED) {          \
                    name##_set_color_idx(base, p, REL_RB_BLACK);        \
                    name##_set_color_idx(base, u, REL_RB_BLACK);        \
                    name##_set_color_idx(base, g, REL_RB_RED);          \
                    z = g;                                              \
                } else {                                                \
                    if (z == name##_left_idx(base, p)) {                \
                        z = p;                                          \
                        name##_rotate_right(head, base, z);             \
                        p = name##_parent_idx(base,z);                  \
                        g = name##_parent_idx(base,p);                  \
                    }                                                   \
                    name##_set_color_idx(base,p, REL_RB_BLACK);         \
                    name##_set_color_idx(base,g, REL_RB_RED);           \
                    name##_rotate_left(head, base, g);                  \
                }                                                       \
            }                                                           \
        }                                                               \
        name##_set_color_idx(base, (head)->rrh_root, REL_RB_BLACK);     \
        if ((head)->rrh_root != REL_NIL)                                \
            name##_ptr(base, (head)->rrh_root)->field.rbe_parent = REL_NIL; \
        return NULL;							\
    }									\
    type * name##_REL_RB_MINMAX(struct name *head, type *base, int dir){ \
        unsigned x = (head)->rrh_root;					\
        if (x == REL_NIL)                                               \
            return NULL;                                                \
        if (dir < 0) { /* MIN */                                        \
            while (name##_left_idx(base, x) != REL_NIL)                 \
                x = name##_left_idx(base, x);                           \
        } else {                                                        \
            while (name##_right_idx(base, x) != REL_NIL)                \
                x = name##_right_idx(base, x);                          \
        }                                                               \
        return name##_ptr(base, x);                                     \
    }									\
    /* ---- successor / predecessor ---- */				\
    type * name##_REL_RB_NEXT(type *base, type *elm) {                  \
        unsigned x = REL_IDX_FROM_PTR(base, elm);                       \
        if (x == REL_NIL)                                               \
            return NULL;                                                \
        unsigned r = name##_right_idx(base, x);				\
        if (r != REL_NIL){                                              \
            x = r;                                                      \
            while (name##_left_idx(base, x) != REL_NIL)                 \
                x = name##_left_idx(base, x);                            \
            return name##_ptr(base, x);					\
        } else {                                                        \
            unsigned p = name##_parent_idx(base, x);                    \
            while (p != REL_NIL && x == name##_right_idx(base, p)) {    \
                x = p;                                                  \
                p = name##_parent_idx(base, x);                         \
            }                                                           \
            return name##_ptr(base, p);					\
        }                                                               \
    }									\
    type * name##_REL_RB_PREV(type *base, type *elm) {			\
        unsigned x = REL_IDX_FROM_PTR(base, elm);                       \
        if (x == REL_NIL)                                               \
            return NULL;                                                \
        unsigned l = name##_left_idx(base, x);				\
        if (l != REL_NIL) {                                             \
            x = l;                                                      \
            while (name##_right_idx(base,x) != REL_NIL)                 \
                x = name##_right_idx(base, x);                          \
            return name##_ptr(base, x);					\
        } else {                                                        \
            unsigned p = name##_parent_idx(base, x);                    \
            while (p != REL_NIL && x == name##_left_idx(base, p)) {     \
                x = p;                                                  \
                p = name##_parent_idx(base, x);                         \
            }                                                           \
            return name##_ptr(base, p);					\
        }                                                               \
    }									\
    type * name##_REL_RB_FIND(struct name *head, type *base, const type *key) { \
        unsigned x = (head)->rrh_root;					\
        while (x != REL_NIL) {						\
            int c = (cmp)(key, name##_ptr(base, x));                    \
            if      (c < 0)                                             \
                x = name##_left_idx(base, x);                           \
            else if (c > 0)                                             \
                x = name##_right_idx(base, x);                          \
            else                                                        \
                return name##_ptr(base, x);                             \
        }                                                               \
        return NULL;							\
    }									\
    type * name##_REL_RB_NFIND(struct name *head, type *base, const type *key) { \
        unsigned x = (head)->rrh_root, res = REL_NIL;			\
        while (x != REL_NIL) {						\
            int c = (cmp)(key, name##_ptr(base, x));                    \
            if (c <= 0) {                                               \
                res = x;                                                \
                x = name##_left_idx(base, x);                           \
            }                                                           \
            else                                                        \
                x = name##_right_idx(base, x);                          \
        }                                                               \
        return name##_ptr(base, res);					\
    }									\
    type * name##_REL_RB_REMOVE(struct name *head, type *base, type *elm) { \
        unsigned z = REL_IDX_FROM_PTR(base, elm);                       \
        unsigned y = z;							\
        unsigned y_color = name##_color_idx(base, y);			\
        unsigned x = REL_NIL;						\
        unsigned x_parent = REL_NIL;					\
        if (name##_left_idx(base,z) == REL_NIL) {                       \
            x = name##_right_idx(base, z);                              \
            x_parent = name##_parent_idx(base, z);                      \
            name##_transplant(head, base, z, x);                        \
        } else if (name##_right_idx(base, z) == REL_NIL) {              \
            x = name##_left_idx(base, z);                               \
            x_parent = name##_parent_idx(base, z);                      \
            name##_transplant(head, base, z, x);                        \
        } else {                                                        \
            /* successor */                                             \
            y = name##_right_idx(base, z);                              \
            while (name##_left_idx(base, y) != REL_NIL)                 \
                y = name##_left_idx(base, y);                           \
            y_color = name##_color_idx(base, y);                        \
            x = name##_right_idx(base, y);                              \
            if (name##_parent_idx(base, y) == z) {                      \
                x_parent = y;                                           \
                if (x!=REL_NIL)                                         \
                    name##_ptr(base, x)->field.rbe_parent = y;          \
            } else {                                                    \
                x_parent = name##_parent_idx(base, y);                  \
                name##_transplant(head, base, y, x);                    \
                name##_set_right(base, y, name##_right_idx(base, z));   \
            }                                                           \
            name##_transplant(head, base, z, y);                        \
            name##_set_left(base, y, name##_left_idx(base, z));         \
            name##_set_color_idx(base, y, name##_color_idx(base, z));   \
        }                                                               \
        /* fix-up */							\
        if (y_color == REL_RB_BLACK) {					\
            unsigned xi = x;                                            \
            unsigned xpi = x_parent;                                    \
            while ( (xi != (head)->rrh_root) &&                         \
                    (name##_color_idx(base, xi) == REL_RB_BLACK) ) {    \
                if (xi == name##_left_idx(base, xpi)) {                 \
                    unsigned w = name##_right_idx(base, xpi);           \
                    if (name##_color_idx(base, w) == REL_RB_RED) {      \
                        name##_set_color_idx(base, w, REL_RB_BLACK);    \
                        name##_set_color_idx(base, xpi, REL_RB_RED);    \
                        name##_rotate_left(head, base, xpi);            \
                        w = name##_right_idx(base, xpi);                \
                    }                                                   \
                    if (name##_color_idx(base, name##_left_idx(base, w))  == REL_RB_BLACK && \
                        name##_color_idx(base, name##_right_idx(base, w)) == REL_RB_BLACK) { \
                        name##_set_color_idx(base, w, REL_RB_RED);      \
                        xi = xpi;                                       \
                        xpi = name##_parent_idx(base, xi);              \
                    } else {                                            \
                        if (name##_color_idx(base, name##_right_idx(base, w)) == REL_RB_BLACK) { \
                            name##_set_color_idx(base,                  \
                                                 name##_left_idx(base, w),\
                                                 REL_RB_BLACK);         \
                            name##_set_color_idx(base, w, REL_RB_RED);  \
                            name##_rotate_right(head, base, w);         \
                            w = name##_right_idx(base, xpi);            \
                        }                                               \
                        name##_set_color_idx(base, w, name##_color_idx(base, \
                                                                       xpi)); \
                        name##_set_color_idx(base, xpi, REL_RB_BLACK);  \
                        name##_set_color_idx(base, name##_right_idx(base, w),\
                                             REL_RB_BLACK);             \
                        name##_rotate_left(head, base, xpi);            \
                        xi = (head)->rrh_root;                          \
                        xpi = REL_NIL;                                  \
                    }                                                   \
                } else { /* mirror */                                   \
                    unsigned w = name##_left_idx(base, xpi);            \
                    if (name##_color_idx(base, w) == REL_RB_RED) {      \
                        name##_set_color_idx(base, w, REL_RB_BLACK);    \
                        name##_set_color_idx(base, xpi, REL_RB_RED);    \
                        name##_rotate_right(head, base, xpi);           \
                        w = name##_left_idx(base, xpi);                 \
                    }                                                   \
                    if (name##_color_idx(base, name##_right_idx(base, w)) == REL_RB_BLACK && \
                        name##_color_idx(base, name##_left_idx(base, w))  == REL_RB_BLACK) { \
                        name##_set_color_idx(base, w, REL_RB_RED);      \
                        xi = xpi;                                       \
                        xpi = name##_parent_idx(base, xi);              \
                    } else {                                            \
                        if (name##_color_idx(base, name##_left_idx(base, w)) == REL_RB_BLACK) { \
                            name##_set_color_idx(base,                  \
                                                 name##_right_idx(base, w), \
                                                 REL_RB_BLACK);         \
                            name##_set_color_idx(base,w, REL_RB_RED);   \
                            name##_rotate_left(head, base, w);          \
                            w = name##_left_idx(base, xpi);             \
                        }                                               \
                        name##_set_color_idx(base, w, name##_color_idx(base,xpi)); \
                        name##_set_color_idx(base, xpi, REL_RB_BLACK);  \
                        name##_set_color_idx(base, name##_left_idx(base, w), \
                                             REL_RB_BLACK);             \
                        name##_rotate_right(head, base, xpi);           \
                        xi = (head)->rrh_root;                          \
                        xpi = REL_NIL;                                  \
                    }                                                   \
                }                                                       \
            }                                                           \
            name##_set_color_idx(base, xi, REL_RB_BLACK);               \
        }                                                               \
        if ((head)->rrh_root != REL_NIL)                                \
            name##_ptr(base, (head)->rrh_root)->field.rbe_parent = REL_NIL; \
        elm->field.rbe_parent = elm->field.rbe_left = elm->field.rbe_right = REL_NIL; \
        elm->field.rbe_color  = REL_RB_RED;                             \
        return elm;                                                     \
    }

#ifdef __cplusplus
}
#endif

#endif				/* _REL_QUEUE_TREE_H_ */
