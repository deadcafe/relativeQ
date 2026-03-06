/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 */
#ifndef _REL_QUEUE_H_
#define _REL_QUEUE_H_

/*
 * Single List
 */
#define REL_SLIST_ENTRY(type) \
    struct {                  \
        unsigned rsle_next;   \
    }

#define REL_SLIST_HEAD(name, type) \
    struct name {                  \
        unsigned rslh_first;       \
    }

#define REL_SLIST_HEAD_INITIALIZER(headvar) \
    { REL_NIL }

#define REL_SLIST_EMPTY(head) ((head)->rslh_first == REL_NIL)
#define REL_SLIST_FIRST(head, base) \
    (REL_PTR_FROM_IDX((base), (head)->rslh_first))
#define REL_SLIST_NEXT(elm, base, field) \
    (REL_PTR_FROM_IDX((base), (elm)->field.rsle_next))

#define REL_SLIST_INIT(head)          \
    do {                              \
        (head)->rslh_first = REL_NIL; \
    } while (0)

#define REL_SLIST_INSERT_HEAD(head, base, elm, field)         \
    do {                                                      \
        unsigned __first = (head)->rslh_first;                \
        (elm)->field.rsle_next = __first;                     \
        (head)->rslh_first = REL_IDX_FROM_PTR((base), (elm)); \
    } while (0)

#define REL_SLIST_INSERT_AFTER(base, slistelm, elm, field)             \
    do {                                                               \
        unsigned __next = (slistelm)->field.rsle_next;                 \
        (elm)->field.rsle_next = __next;                               \
        (slistelm)->field.rsle_next = REL_IDX_FROM_PTR((base), (elm)); \
    } while (0)

#define REL_SLIST_REMOVE_HEAD(head, base, field)                              \
    do {                                                                      \
        unsigned __first = (head)->rslh_first;                                \
        if (__first != REL_NIL) {                                             \
            unsigned __next = REL_PTR_FROM_IDX((base), __first)->field.rsle_next; \
            (head)->rslh_first = __next;                                      \
        }                                                                     \
    } while (0)

#define REL_SLIST_REMOVE_AFTER(base, elm, field)                  \
    do {                                                          \
        unsigned __rem = (elm)->field.rsle_next;                  \
        if (__rem != REL_NIL) {                                   \
            (elm)->field.rsle_next =                              \
                REL_PTR_FROM_IDX((base), __rem)->field.rsle_next; \
        }                                                         \
    } while (0)

#define REL_SLIST_REMOVE(head, base, elm, type, field)                        \
    do {                                                                      \
        if (REL_SLIST_FIRST((head), (base)) == (elm)) {                       \
            REL_SLIST_REMOVE_HEAD((head), (base), field);                     \
        } else {                                                              \
            struct type *__curelm = REL_SLIST_FIRST((head), (base));          \
            while (__curelm && REL_SLIST_NEXT(__curelm, (base), field) != (elm)) \
                __curelm = REL_SLIST_NEXT(__curelm, (base), field);           \
            if (__curelm)                                                     \
                REL_SLIST_REMOVE_AFTER((base), __curelm, field);              \
        }                                                                     \
    } while (0)

#define REL_SLIST_FOREACH(var, head, base, field)                \
    for ((var) = REL_SLIST_FIRST((head), (base)); (var) != NULL; \
         (var) = REL_SLIST_NEXT((var), (base), field))

#define REL_SLIST_FOREACH_SAFE(var, head, base, field, tvar)            \
    for ((var) = REL_SLIST_FIRST((head), (base)),                       \
        (tvar) = ((var) ? REL_SLIST_NEXT((var), (base), field) : NULL); \
         (var) != NULL; (var) = (tvar),                                 \
        (tvar) = ((var) ? REL_SLIST_NEXT((var), (base), field) : NULL))

#define REL_SLIST_FOREACH_PREVINDEX(var, varidxp, head, base, field) \
    for ((varidxp) = &((head)->rslh_first);                          \
         ((var) = REL_PTR_FROM_IDX((base), *(varidxp))) != NULL;     \
         (varidxp) = &((var)->field.rsle_next))

/*
 * LIST
 */
#define REL_LIST_ENTRY(type) \
    struct {                 \
        unsigned rle_next;   \
        unsigned rle_prev;   \
    }

#define REL_LIST_HEAD(name, type) \
    struct name {                 \
        unsigned rlh_first;       \
    }

#define REL_LIST_HEAD_INITIALIZER(headvar) \
    { REL_NIL }

#define REL_LIST_EMPTY(head) ((head)->rlh_first == REL_NIL)
#define REL_LIST_FIRST(head, base) (REL_PTR_FROM_IDX((base), (head)->rlh_first))
#define REL_LIST_NEXT(elm, base, field) \
    (REL_PTR_FROM_IDX((base), (elm)->field.rle_next))

#define REL_LIST_INIT(head)          \
    do {                             \
        (head)->rlh_first = REL_NIL; \
    } while (0)

#define REL_LIST_INSERT_HEAD(head, base, elm, field)                   \
    do {                                                               \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));              \
        unsigned __first = (head)->rlh_first;                          \
        (elm)->field.rle_prev = REL_NIL;                               \
        (elm)->field.rle_next = __first;                               \
        if (__first != REL_NIL)                                        \
            REL_PTR_FROM_IDX((base), __first)->field.rle_prev = __idx; \
        (head)->rlh_first = __idx;                                     \
    } while (0)

#define REL_LIST_INSERT_AFTER(head, base, listelm, elm, field)        \
    do {                                                              \
        unsigned __listidx = REL_IDX_FROM_PTR((base), (listelm));     \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));             \
        unsigned __next = (listelm)->field.rle_next;                  \
        (elm)->field.rle_prev = __listidx;                            \
        (elm)->field.rle_next = __next;                               \
        (listelm)->field.rle_next = __idx;                            \
        if (__next != REL_NIL)                                        \
            REL_PTR_FROM_IDX((base), __next)->field.rle_prev = __idx; \
    } while (0)

#define REL_LIST_INSERT_BEFORE(head, base, listelm, elm, field)       \
    do {                                                              \
        unsigned __listidx = REL_IDX_FROM_PTR((base), (listelm));     \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));             \
        unsigned __prev = (listelm)->field.rle_prev;                  \
        (elm)->field.rle_prev = __prev;                               \
        (elm)->field.rle_next = __listidx;                            \
        (listelm)->field.rle_prev = __idx;                            \
        if (__prev != REL_NIL)                                        \
            REL_PTR_FROM_IDX((base), __prev)->field.rle_next = __idx; \
        else                                                          \
            (head)->rlh_first = __idx;                                \
    } while (0)

#define REL_LIST_REMOVE(head, base, elm, field)                        \
    do {                                                               \
        unsigned __next = (elm)->field.rle_next;                       \
        unsigned __prev = (elm)->field.rle_prev;                       \
        if (__next != REL_NIL)                                         \
            REL_PTR_FROM_IDX((base), __next)->field.rle_prev = __prev; \
        if (__prev != REL_NIL)                                         \
            REL_PTR_FROM_IDX((base), __prev)->field.rle_next = __next; \
        else                                                           \
            (head)->rlh_first = __next;                                \
        (elm)->field.rle_next = REL_NIL;                               \
        (elm)->field.rle_prev = REL_NIL;                               \
    } while (0)

#define REL_LIST_SWAP(head1, head2, base, type, field)                         \
    do {                                                                       \
        unsigned __f1 = (head1)->rlh_first;                                    \
        unsigned __f2 = (head2)->rlh_first;                                    \
        (head1)->rlh_first = __f2;                                             \
        (head2)->rlh_first = __f1;                                             \
        do {                                                                   \
            if ((head1)->rlh_first != REL_NIL)                                 \
                REL_PTR_FROM_IDX((base), (head1)->rlh_first)->field.rle_prev = \
                    REL_NIL;                                                   \
        } while (0);                                                           \
        do {                                                                   \
            if ((head2)->rlh_first != REL_NIL)                                 \
                REL_PTR_FROM_IDX((base), (head2)->rlh_first)->field.rle_prev = \
                    REL_NIL;                                                   \
        } while (0);                                                           \
        (void)(base);                                                          \
        (void)(sizeof(struct type));                                           \
    } while (0)

#define REL_LIST_FOREACH(var, head, base, field)                \
    for ((var) = REL_LIST_FIRST((head), (base)); (var) != NULL; \
         (var) = REL_LIST_NEXT((var), (base), field))

#define REL_LIST_FOREACH_SAFE(var, head, base, field, tvar)            \
    for ((var) = REL_LIST_FIRST((head), (base)),                       \
        (tvar) = ((var) ? REL_LIST_NEXT((var), (base), field) : NULL); \
         (var) != NULL; (var) = (tvar),                                \
        (tvar) = ((var) ? REL_LIST_NEXT((var), (base), field) : NULL))

/*
 * STAILQ
 */
#define REL_STAILQ_ENTRY(type) \
    struct {                   \
        unsigned rsqe_next;    \
    }

#define REL_STAILQ_HEAD(name, type) \
    struct name {                   \
        unsigned rsqh_first;        \
        unsigned rsqh_last;         \
    }

#define REL_STAILQ_HEAD_INITIALIZER(headvar) \
    { REL_NIL, REL_NIL }

#define REL_STAILQ_EMPTY(head) ((head)->rsqh_first == REL_NIL)
#define REL_STAILQ_FIRST(head, base) \
    (REL_PTR_FROM_IDX((base), (head)->rsqh_first))
#define REL_STAILQ_LAST(head, base) \
    (REL_PTR_FROM_IDX((base), (head)->rsqh_last))
#define REL_STAILQ_NEXT(head, base, elm, field) \
    (REL_PTR_FROM_IDX((base), (elm)->field.rsqe_next))

#define REL_STAILQ_INIT(head)         \
    do {                              \
        (head)->rsqh_first = REL_NIL; \
        (head)->rsqh_last = REL_NIL;  \
    } while (0)

#define REL_STAILQ_INSERT_HEAD(head, base, elm, field)    \
    do {                                                  \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm)); \
        unsigned __first = (head)->rsqh_first;            \
        (elm)->field.rsqe_next = __first;                 \
        (head)->rsqh_first = __idx;                       \
        if (__first == REL_NIL) {                         \
            (head)->rsqh_last = __idx;                    \
        }                                                 \
    } while (0)

#define REL_STAILQ_INSERT_TAIL(head, base, elm, field)                        \
    do {                                                                      \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));                     \
        (elm)->field.rsqe_next = REL_NIL;                                     \
        if ((head)->rsqh_last != REL_NIL) {                                   \
            REL_PTR_FROM_IDX((base), (head)->rsqh_last)->field.rsqe_next = __idx; \
        } else {                                                              \
            (head)->rsqh_first = __idx;                                       \
        }                                                                     \
        (head)->rsqh_last = __idx;                                            \
    } while (0)

#define REL_STAILQ_INSERT_AFTER(head, base, tqelm, elm, field) \
    do {                                                       \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));      \
        unsigned __next = (tqelm)->field.rsqe_next;            \
        (elm)->field.rsqe_next = __next;                       \
        (tqelm)->field.rsqe_next = __idx;                      \
        if (__next == REL_NIL) {                               \
            (head)->rsqh_last = __idx;                         \
        }                                                      \
    } while (0)

#define REL_STAILQ_REMOVE_HEAD(head, base, field)                             \
    do {                                                                      \
        unsigned __first = (head)->rsqh_first;                                \
        if (__first != REL_NIL) {                                             \
            unsigned __next = REL_PTR_FROM_IDX((base), __first)->field.rsqe_next; \
            (head)->rsqh_first = __next;                                      \
            if (__next == REL_NIL)                                            \
                (head)->rsqh_last = REL_NIL;                                  \
        }                                                                     \
    } while (0)

#define REL_STAILQ_REMOVE_AFTER(head, base, elm, field)                       \
    do {                                                                      \
        unsigned __rem = (elm)->field.rsqe_next;                              \
        if (__rem != REL_NIL) {                                               \
            unsigned __nn = REL_PTR_FROM_IDX((base), __rem)->field.rsqe_next; \
            (elm)->field.rsqe_next = __nn;                                    \
            if (__nn == REL_NIL)                                              \
                (head)->rsqh_last = REL_IDX_FROM_PTR((base), (elm));          \
        }                                                                     \
    } while (0)

#define REL_STAILQ_REMOVE(head, base, elm, type, field)                       \
    do {                                                                      \
        if (REL_STAILQ_FIRST((head), (base)) == (elm)) {                      \
            REL_STAILQ_REMOVE_HEAD((head), (base), field);                    \
        } else {                                                              \
            struct type *__curelm = REL_STAILQ_FIRST((head), (base));         \
            while (__curelm &&                                                \
                   REL_PTR_FROM_IDX((base), __curelm->field.rsqe_next) != (elm)) { \
                __curelm = REL_STAILQ_NEXT((head), (base), __curelm, field);  \
            }                                                                 \
            if (__curelm) {                                                   \
                REL_STAILQ_REMOVE_AFTER((head), (base), __curelm, field);     \
            }                                                                 \
        }                                                                     \
    } while (0)

#define REL_STAILQ_REMOVE_HEAD_UNTIL(head, base, elm, field)        \
    do {                                                            \
        unsigned __next = (elm) ? (elm)->field.rsqe_next : REL_NIL; \
        (head)->rsqh_first = __next;                                \
        if (__next == REL_NIL)                                      \
            (head)->rsqh_last = REL_NIL;                            \
    } while (0)

#define REL_STAILQ_CONCAT(head1, head2, base, field)                          \
    do {                                                                      \
        if (!REL_STAILQ_EMPTY((head2))) {                                     \
            if (!REL_STAILQ_EMPTY((head1))) {                                 \
                REL_PTR_FROM_IDX((base), (head1)->rsqh_last)->field.rsqe_next = \
                    (head2)->rsqh_first;                                      \
            } else {                                                          \
                (head1)->rsqh_first = (head2)->rsqh_first;                    \
            }                                                                 \
            (head1)->rsqh_last = (head2)->rsqh_last;                          \
            (head2)->rsqh_first = REL_NIL;                                    \
            (head2)->rsqh_last = REL_NIL;                                     \
        }                                                                     \
    } while (0)

#define REL_STAILQ_SWAP(head1, head2, base)        \
    do {                                           \
        unsigned __f = (head1)->rsqh_first;        \
        unsigned __l = (head1)->rsqh_last;         \
        (void)(base);                              \
        (head1)->rsqh_first = (head2)->rsqh_first; \
        (head1)->rsqh_last = (head2)->rsqh_last;   \
        (head2)->rsqh_first = __f;                 \
        (head2)->rsqh_last = __l;                  \
    } while (0)

#define REL_STAILQ_FOREACH(var, head, base, field)                \
    for ((var) = REL_STAILQ_FIRST((head), (base)); (var) != NULL; \
         (var) = REL_STAILQ_NEXT((head), (base), (var), field))

#define REL_STAILQ_FOREACH_SAFE(var, head, base, field, tvar)                 \
    for ((var) = REL_STAILQ_FIRST((head), (base)),                            \
        (tvar) = ((var) ? REL_STAILQ_NEXT((head), (base), (var), field) : NULL); \
         (var) != NULL; (var) = (tvar),                                       \
        (tvar) = ((var) ? REL_STAILQ_NEXT((head), (base), (var), field) : NULL))

/*
 * TAILQ
 */
#define REL_TAILQ_ENTRY(type) \
    struct {                  \
        unsigned rqe_next;    \
        unsigned rqe_prev;    \
    }

#define REL_TAILQ_HEAD(name, type) \
    struct name {                  \
        unsigned rqh_first;        \
        unsigned rqh_last;         \
    }

#define REL_TAILQ_HEAD_INITIALIZER(headvar) \
    { REL_NIL, REL_NIL }

#define REL_TAILQ_INIT(head)         \
    do {                             \
        (head)->rqh_first = REL_NIL; \
        (head)->rqh_last = REL_NIL;  \
    } while (0)

#define REL_TAILQ_RESET(head) REL_TAILQ_INIT(head)

#define REL_TAILQ_EMPTY(head) ((head)->rqh_first == REL_NIL)
#define REL_TAILQ_FIRST(head, base) \
    (REL_PTR_FROM_IDX((base), (head)->rqh_first))
#define REL_TAILQ_LAST(head, base) (REL_PTR_FROM_IDX((base), (head)->rqh_last))

#define REL_TAILQ_NEXT(head, base, elm, field) \
    (REL_PTR_FROM_IDX((base), (elm)->field.rqe_next))

#define REL_TAILQ_PREV(head, base, elm, field) \
    (((elm)->field.rqe_prev == REL_NIL)        \
         ? NULL                                \
         : REL_PTR_FROM_IDX((base), (elm)->field.rqe_prev))

#define REL_TAILQ_FOREACH(var, head, base, field)                \
    for ((var) = REL_TAILQ_FIRST((head), (base)); (var) != NULL; \
         (var) = REL_TAILQ_NEXT((head), (base), (var), field))

#define REL_TAILQ_FOREACH_SAFE(var, head, base, field, tvar)                  \
    for ((var) = REL_TAILQ_FIRST((head), (base)),                             \
        (tvar) = ((var) ? REL_TAILQ_NEXT((head), (base), (var), field) : NULL); \
         (var) != NULL; (var) = (tvar),                                       \
        (tvar) = ((var) ? REL_TAILQ_NEXT((head), (base), (var), field) : NULL))

#define REL_TAILQ_FOREACH_REVERSE(var, head, base, field)       \
    for ((var) = REL_TAILQ_LAST((head), (base)); (var) != NULL; \
         (var) = REL_TAILQ_PREV((head), (base), (var), field))

#define __REL_SET_NEXT(elm, field, v_idx) \
    do {                                  \
        (elm)->field.rqe_next = (v_idx);  \
    } while (0)

#define __REL_SET_PREV(elm, field, v_idx) \
    do {                                  \
        (elm)->field.rqe_prev = (v_idx);  \
    } while (0)

#define REL_TAILQ_INSERT_HEAD(head, base, elm, field)                        \
    do {                                                                     \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));                    \
        unsigned __first = (head)->rqh_first;                                \
        __REL_SET_PREV((elm), field, REL_NIL);                               \
        __REL_SET_NEXT((elm), field, __first);                               \
        if (__first != REL_NIL) {                                            \
            __REL_SET_PREV(REL_PTR_FROM_IDX((base), __first), field, __idx); \
        } else {                                                             \
            (head)->rqh_last = __idx;                                        \
        }                                                                    \
        (head)->rqh_first = __idx;                                           \
    } while (0)

#define REL_TAILQ_INSERT_TAIL(head, base, elm, field)                       \
    do {                                                                    \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));                   \
        unsigned __last = (head)->rqh_last;                                 \
        __REL_SET_NEXT((elm), field, REL_NIL);                              \
        __REL_SET_PREV((elm), field, __last);                               \
        if (__last != REL_NIL) {                                            \
            __REL_SET_NEXT(REL_PTR_FROM_IDX((base), __last), field, __idx); \
        } else {                                                            \
            (head)->rqh_first = __idx;                                      \
        }                                                                   \
        (head)->rqh_last = __idx;                                           \
    } while (0)

#define REL_TAILQ_INSERT_AFTER(head, base, listelm, elm, field)             \
    do {                                                                    \
        unsigned __listidx = REL_IDX_FROM_PTR((base), (listelm));           \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));                   \
        unsigned __next = (listelm)->field.rqe_next;                        \
        __REL_SET_NEXT((elm), field, __next);                               \
        __REL_SET_PREV((elm), field, __listidx);                            \
        __REL_SET_NEXT((listelm), field, __idx);                            \
        if (__next != REL_NIL) {                                            \
            __REL_SET_PREV(REL_PTR_FROM_IDX((base), __next), field, __idx); \
        } else {                                                            \
            (head)->rqh_last = __idx;                                       \
        }                                                                   \
    } while (0)

#define REL_TAILQ_INSERT_BEFORE(head, base, listelm, elm, field)            \
    do {                                                                    \
        unsigned __listidx = REL_IDX_FROM_PTR((base), (listelm));           \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));                   \
        unsigned __prev = (listelm)->field.rqe_prev;                        \
        __REL_SET_PREV((elm), field, __prev);                               \
        __REL_SET_NEXT((elm), field, __listidx);                            \
        __REL_SET_PREV((listelm), field, __idx);                            \
        if (__prev != REL_NIL) {                                            \
            __REL_SET_NEXT(REL_PTR_FROM_IDX((base), __prev), field, __idx); \
        } else {                                                            \
            (head)->rqh_first = __idx;                                      \
        }                                                                   \
    } while (0)

#define REL_TAILQ_REMOVE(head, base, elm, field)                             \
    do {                                                                     \
        unsigned __next = (elm)->field.rqe_next;                             \
        unsigned __prev = (elm)->field.rqe_prev;                             \
        if (__next != REL_NIL) {                                             \
            __REL_SET_PREV(REL_PTR_FROM_IDX((base), __next), field, __prev); \
        } else {                                                             \
            (head)->rqh_last = __prev;                                       \
        }                                                                    \
        if (__prev != REL_NIL) {                                             \
            __REL_SET_NEXT(REL_PTR_FROM_IDX((base), __prev), field, __next); \
        } else {                                                             \
            (head)->rqh_first = __next;                                      \
        }                                                                    \
        __REL_SET_NEXT((elm), field, REL_NIL);                               \
        __REL_SET_PREV((elm), field, REL_NIL);                               \
    } while (0)

#define REL_TAILQ_CONCAT(head1, head2, base, field)                         \
    do {                                                                    \
        if (!REL_TAILQ_EMPTY((head2))) {                                    \
            if (!REL_TAILQ_EMPTY((head1))) {                                \
                unsigned __h1_last = (head1)->rqh_last;                     \
                unsigned __h2_first = (head2)->rqh_first;                   \
                __REL_SET_NEXT(REL_PTR_FROM_IDX((base), __h1_last), field,  \
                               __h2_first);                                 \
                __REL_SET_PREV(REL_PTR_FROM_IDX((base), __h2_first), field, \
                               __h1_last);                                  \
            } else {                                                        \
                (head1)->rqh_first = (head2)->rqh_first;                    \
            }                                                               \
            (head1)->rqh_last = (head2)->rqh_last;                          \
            (head2)->rqh_first = REL_NIL;                                   \
            (head2)->rqh_last = REL_NIL;                                    \
        }                                                                   \
    } while (0)

#define REL_TAILQ_SWAP(head1, head2, base)       \
    do {                                         \
        unsigned __f = (head1)->rqh_first;       \
        unsigned __l = (head1)->rqh_last;        \
        (void)(base);                            \
        (head1)->rqh_first = (head2)->rqh_first; \
        (head1)->rqh_last = (head2)->rqh_last;   \
        (head2)->rqh_first = __f;                \
        (head2)->rqh_last = __l;                 \
    } while (0)

/*
 * CIRCLEQ
 */
#define REL_CIRCLEQ_ENTRY(type) \
    struct {                    \
        unsigned rce_next;      \
        unsigned rce_prev;      \
    }

#define REL_CIRCLEQ_HEAD(name, type) \
    struct name {                    \
        unsigned rch_first;          \
    }

#define REL_CIRCLEQ_HEAD_INITIALIZER(headvar) \
    { REL_NIL }

#define REL_CIRCLEQ_EMPTY(head) ((head)->rch_first == REL_NIL)
#define REL_CIRCLEQ_FIRST(head, base) \
    (REL_PTR_FROM_IDX((base), (head)->rch_first))

#define REL_CIRCLEQ_LAST(head, base, field) \
    (REL_CIRCLEQ_EMPTY(head) ? NULL : REL_PTR_FROM_IDX((base), REL_CIRCLEQ_FIRST((head), (base))->field.rce_prev))

#define REL_CIRCLEQ_NEXT(elm, base, field) \
    (REL_PTR_FROM_IDX((base), (elm)->field.rce_next))

#define REL_CIRCLEQ_PREV(elm, base, field) \
    (REL_PTR_FROM_IDX((base), (elm)->field.rce_prev))

#define REL_CIRCLEQ_INIT(head)       \
    do {                             \
        (head)->rch_first = REL_NIL; \
    } while (0)

#define REL_CIRCLEQ_INSERT_HEAD(head, base, elm, field)                \
    do {                                                               \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));              \
        if (REL_CIRCLEQ_EMPTY(head)) {                                 \
            (head)->rch_first = __idx;                                 \
            (elm)->field.rce_next = __idx;                             \
            (elm)->field.rce_prev = __idx;                             \
        } else {                                                       \
            unsigned __first = (head)->rch_first;                      \
            unsigned __last =                                          \
                REL_PTR_FROM_IDX((base), __first)->field.rce_prev;     \
            (elm)->field.rce_next = __first;                           \
            (elm)->field.rce_prev = __last;                            \
            REL_PTR_FROM_IDX((base), __first)->field.rce_prev = __idx; \
            REL_PTR_FROM_IDX((base), __last)->field.rce_next = __idx;  \
            (head)->rch_first = __idx;                                 \
        }                                                              \
    } while (0)

#define REL_CIRCLEQ_INSERT_TAIL(head, base, elm, field)                \
    do {                                                               \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));              \
        if (REL_CIRCLEQ_EMPTY(head)) {                                 \
            (head)->rch_first = __idx;                                 \
            (elm)->field.rce_next = __idx;                             \
            (elm)->field.rce_prev = __idx;                             \
        } else {                                                       \
            unsigned __first = (head)->rch_first;                      \
            unsigned __last =                                          \
                REL_PTR_FROM_IDX((base), __first)->field.rce_prev;     \
            (elm)->field.rce_next = __first;                           \
            (elm)->field.rce_prev = __last;                            \
            REL_PTR_FROM_IDX((base), __first)->field.rce_prev = __idx; \
            REL_PTR_FROM_IDX((base), __last)->field.rce_next = __idx;  \
        }                                                              \
    } while (0)

#define REL_CIRCLEQ_INSERT_AFTER(head, base, listelm, elm, field) \
    do {                                                          \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));         \
        unsigned __after = REL_IDX_FROM_PTR((base), (listelm));   \
        unsigned __next = (listelm)->field.rce_next;              \
        (elm)->field.rce_prev = __after;                          \
        (elm)->field.rce_next = __next;                           \
        (listelm)->field.rce_next = __idx;                        \
        REL_PTR_FROM_IDX((base), __next)->field.rce_prev = __idx; \
    } while (0)

#define REL_CIRCLEQ_INSERT_BEFORE(head, base, listelm, elm, field) \
    do {                                                           \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));          \
        unsigned __before = REL_IDX_FROM_PTR((base), (listelm));   \
        unsigned __prev = (listelm)->field.rce_prev;               \
        (elm)->field.rce_next = __before;                          \
        (elm)->field.rce_prev = __prev;                            \
        REL_PTR_FROM_IDX((base), __prev)->field.rce_next = __idx;  \
        (listelm)->field.rce_prev = __idx;                         \
        if ((head)->rch_first == __before)                         \
            (head)->rch_first = __idx;                             \
    } while (0)

#define REL_CIRCLEQ_REMOVE(head, base, elm, field)                     \
    do {                                                               \
        unsigned __idx = REL_IDX_FROM_PTR((base), (elm));              \
        unsigned __next = (elm)->field.rce_next;                       \
        unsigned __prev = (elm)->field.rce_prev;                       \
        if (__next == __idx) {                                         \
            (head)->rch_first = REL_NIL;                               \
        } else {                                                       \
            REL_PTR_FROM_IDX((base), __prev)->field.rce_next = __next; \
            REL_PTR_FROM_IDX((base), __next)->field.rce_prev = __prev; \
            if ((head)->rch_first == __idx)                            \
                (head)->rch_first = __next;                            \
        }                                                              \
        (elm)->field.rce_next = REL_NIL;                               \
        (elm)->field.rce_prev = REL_NIL;                               \
    } while (0)

#define REL_CIRCLEQ_FOREACH(var, head, base, field)                \
    for ((var) = REL_CIRCLEQ_FIRST((head), (base)); (var) != NULL; \
         (var) =                                                   \
             (((var)->field.rce_next == (head)->rch_first)         \
                  ? NULL                                           \
                  : REL_PTR_FROM_IDX((base), (var)->field.rce_next)))

#define REL_CIRCLEQ_FOREACH_REVERSE(var, head, base, field)             \
    for (unsigned __rq_start =                                          \
                      (head)->rch_first                                 \
                          ? REL_PTR_FROM_IDX((base), (head)->rch_first) \
                                ->field.rce_prev                        \
                          : REL_NIL,                                    \
                  __rq_i = __rq_start;                                  \
         __rq_i != REL_NIL &&                                           \
         ((var) = REL_PTR_FROM_IDX((base), __rq_i), 1);                 \
         __rq_i =                                                       \
             (REL_PTR_FROM_IDX((base), __rq_i)->field.rce_prev ==       \
                      __rq_start                                        \
                  ? REL_NIL                                             \
                  : REL_PTR_FROM_IDX((base), __rq_i)->field.rce_prev))

#define REL_CIRCLEQ_FOREACH_SAFE(var, head, base, field, tvar)           \
    for (unsigned                                                        \
             __rq_start = (head)->rch_first,                             \
             __rq_i = __rq_start, __rq_next = REL_NIL;                   \
         __rq_i != REL_NIL &&                                            \
         ((var) = REL_PTR_FROM_IDX((base), __rq_i),                      \
         __rq_next = (var)->field.rce_next,                              \
         (tvar) = REL_PTR_FROM_IDX((base), __rq_next), 1);               \
         __rq_i = ((__rq_next == __rq_start) ||                          \
                           (__rq_start &&                                \
                            REL_PTR_FROM_IDX((base), __rq_start)         \
                                    ->field.rce_next == REL_NIL)         \
                       ? REL_NIL                                         \
                       : __rq_next),                                     \
             __rq_start = (REL_IDX_FROM_PTR((base), (var)) == __rq_start \
                               ? __rq_next                               \
                               : __rq_start))

#define REL_CIRCLEQ_FOREACH_REVERSE_SAFE(var, head, base, field, tvar)   \
    for (unsigned                                                        \
             __rq_start =                                                \
                 (head)->rch_first                                       \
                     ? REL_PTR_FROM_IDX((base), (head)->rch_first)       \
                           ->field.rce_prev                              \
                     : REL_NIL,                                          \
             __rq_i = __rq_start, __rq_prev = REL_NIL;                   \
         __rq_i != REL_NIL &&                                            \
         ((var) = REL_PTR_FROM_IDX((base), __rq_i),                      \
         __rq_prev = (var)->field.rce_prev,                              \
         (tvar) = REL_PTR_FROM_IDX((base), __rq_prev), 1);               \
         __rq_i = ((__rq_prev == __rq_start) ||                          \
                           (__rq_start &&                                \
                            REL_PTR_FROM_IDX((base), __rq_start)         \
                                    ->field.rce_next == REL_NIL)         \
                       ? REL_NIL                                         \
                       : __rq_prev),                                     \
             __rq_start = (REL_IDX_FROM_PTR((base), (var)) == __rq_start \
                               ? __rq_prev                               \
                               : __rq_start))

#endif /* _REL_QUEUE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
