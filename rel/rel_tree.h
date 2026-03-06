/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 */
#ifndef _REL_TREE_H_
#define _REL_TREE_H_

/*
 * RB Tree
 */
#define REL_RB_HEAD(name)  \
    struct name {          \
        unsigned rrh_root; \
    }

#define REL_RB_HEAD_INITIALIZER(var) \
    { REL_NIL }

#define REL_RB_INIT(head)           \
    do {                            \
        (head)->rrh_root = REL_NIL; \
    } while (0)

#define REL_RB_ENTRY(type)   \
    struct {                 \
        unsigned rbe_parent; \
        unsigned rbe_left;   \
        unsigned rbe_right;  \
        unsigned rbe_color;  \
    }

#define REL_RB_RED ((unsigned)0)
#define REL_RB_BLACK ((unsigned)1)

#define REL_RB_ROOT(head, base) (REL_PTR_FROM_IDX((base), (head)->rrh_root))
#define REL_RB_EMPTY(head) ((head)->rrh_root == REL_NIL)
#define REL_RB_PARENT(elm, base, field) \
    (REL_PTR_FROM_IDX((base), (elm)->field.rbe_parent))
#define REL_RB_LEFT(elm, base, field) \
    (REL_PTR_FROM_IDX((base), (elm)->field.rbe_left))
#define REL_RB_RIGHT(elm, base, field) \
    (REL_PTR_FROM_IDX((base), (elm)->field.rbe_right))
#define REL_RB_COLOR(elm, field) ((elm)->field.rbe_color)

#define REL_RB_PROTOTYPE(name, type, field, cmp)                               \
    type *name##_REL_RB_INSERT(struct name *head, type *base, type *elm);      \
    type *name##_REL_RB_REMOVE(struct name *head, type *base, type *elm);      \
    type *name##_REL_RB_FIND(struct name *head, type *base, const type *key);  \
    type *name##_REL_RB_NFIND(struct name *head, type *base, const type *key); \
    type *name##_REL_RB_MINMAX(struct name *head, type *base, int dir);        \
    type *name##_REL_RB_NEXT(type *base, type *elm);                           \
    type *name##_REL_RB_PREV(type *base, type *elm);

#define REL_RB_INSERT(name, head, base, elm) \
    name##_REL_RB_INSERT((head), (base), (elm))
#define REL_RB_REMOVE(name, head, base, elm) \
    name##_REL_RB_REMOVE((head), (base), (elm))
#define REL_RB_FIND(name, head, base, key) \
    name##_REL_RB_FIND((head), (base), (key))
#define REL_RB_NFIND(name, head, base, key) \
    name##_REL_RB_NFIND((head), (base), (key))
#define REL_RB_MIN(name, head, base) name##_REL_RB_MINMAX((head), (base), -1)
#define REL_RB_MAX(name, head, base) name##_REL_RB_MINMAX((head), (base), +1)
#define REL_RB_NEXT(name, base, elm) name##_REL_RB_NEXT((base), (elm))
#define REL_RB_PREV(name, base, elm) name##_REL_RB_PREV((base), (elm))

#define REL_RB_FOREACH(x, name, head, base)                   \
    for ((x) = REL_RB_MIN(name, (head), (base)); (x) != NULL; \
         (x) = name##_REL_RB_NEXT((base), (x)))

#define REL_RB_FOREACH_REVERSE(x, name, head, base)           \
    for ((x) = REL_RB_MAX(name, (head), (base)); (x) != NULL; \
         (x) = name##_REL_RB_PREV((base), (x)))

#define REL_RB_GENERATE(name, type, field, cmp)                               \
    static REL_FORCE_INLINE unsigned name##_idx(type *base, const type *p) {  \
        return REL_IDX_FROM_PTR(base, (type *)p);                             \
    }                                                                         \
    static REL_FORCE_INLINE type *name##_ptr(type *base, unsigned idx) {      \
        return REL_PTR_FROM_IDX(base, idx);                                   \
    }                                                                         \
    static REL_FORCE_INLINE unsigned name##_color_idx(type *base,             \
                                                      unsigned idx) {         \
        return (idx == REL_NIL) ? REL_RB_BLACK                                \
                                : name##_ptr(base, idx)->field.rbe_color;     \
    }                                                                         \
    static REL_FORCE_INLINE void name##_set_color_idx(type *base, unsigned idx,     \
                                                      unsigned char c) {      \
        if (idx != REL_NIL)                                                   \
            name##_ptr(base, idx)->field.rbe_color = c;                       \
    }                                                                         \
    static REL_FORCE_INLINE unsigned name##_left_idx(type *base, unsigned idx) {    \
        return (idx == REL_NIL) ? REL_NIL : name##_ptr(base, idx)->field.rbe_left;  \
    }                                                                         \
    static REL_FORCE_INLINE unsigned name##_right_idx(type *base,             \
                                                      unsigned idx) {         \
        return (idx == REL_NIL) ? REL_NIL                                     \
                                : name##_ptr(base, idx)->field.rbe_right;     \
    }                                                                         \
    static REL_FORCE_INLINE unsigned name##_parent_idx(type *base,            \
                                                       unsigned idx) {        \
        return (idx == REL_NIL) ? REL_NIL                                     \
                                : name##_ptr(base, idx)->field.rbe_parent;    \
    }                                                                         \
    static REL_FORCE_INLINE void name##_set_left(type *base, unsigned p,      \
                                                 unsigned c) {                \
        if (p != REL_NIL)                                                     \
            name##_ptr(base, p)->field.rbe_left = c;                          \
        if (c != REL_NIL)                                                     \
            name##_ptr(base, c)->field.rbe_parent = p;                        \
    }                                                                         \
    static REL_FORCE_INLINE void name##_set_right(type *base, unsigned p,     \
                                                  unsigned c) {               \
        if (p != REL_NIL)                                                     \
            name##_ptr(base, p)->field.rbe_right = c;                         \
        if (c != REL_NIL)                                                     \
            name##_ptr(base, c)->field.rbe_parent = p;                        \
    }                                                                         \
    static REL_FORCE_INLINE void name##_transplant(                           \
        struct name *head, type *base, unsigned u, unsigned v) {              \
        unsigned up = name##_parent_idx(base, u);                             \
        if (up == REL_NIL) {                                                  \
            (head)->rrh_root = v;                                             \
            if (v != REL_NIL)                                                 \
                name##_ptr(base, v)->field.rbe_parent = REL_NIL;              \
        } else if (u == name##_left_idx(base, up)) {                          \
            name##_set_left(base, up, v);                                     \
        } else {                                                              \
            name##_set_right(base, up, v);                                    \
        }                                                                     \
    }                                                                         \
    static REL_FORCE_INLINE void name##_rotate_left(struct name *head,        \
                                                    type *base, unsigned x) { \
        unsigned y = name##_right_idx(base, x);                               \
        unsigned yL = name##_left_idx(base, y);                               \
        name##_set_right(base, x, yL);                                        \
        unsigned xp = name##_parent_idx(base, x);                             \
        if (xp == REL_NIL) {                                                  \
            (head)->rrh_root = y;                                             \
            if (y != REL_NIL)                                                 \
                name##_ptr(base, y)->field.rbe_parent = REL_NIL;              \
        } else if (x == name##_left_idx(base, xp)) {                          \
            name##_set_left(base, xp, y);                                     \
        } else {                                                              \
            name##_set_right(base, xp, y);                                    \
        }                                                                     \
        name##_set_left(base, y, x);                                          \
    }                                                                         \
    static REL_FORCE_INLINE void name##_rotate_right(struct name *head,       \
                                                     type *base, unsigned x) { \
        unsigned y = name##_left_idx(base, x);                                \
        unsigned yR = name##_right_idx(base, y);                              \
        name##_set_left(base, x, yR);                                         \
        unsigned xp = name##_parent_idx(base, x);                             \
        if (xp == REL_NIL) {                                                  \
            (head)->rrh_root = y;                                             \
            if (y != REL_NIL)                                                 \
                name##_ptr(base, y)->field.rbe_parent = REL_NIL;              \
        } else if (x == name##_left_idx(base, xp)) {                          \
            name##_set_left(base, xp, y);                                     \
        } else {                                                              \
            name##_set_right(base, xp, y);                                    \
        }                                                                     \
        name##_set_right(base, y, x);                                         \
    }                                                                         \
    type *name##_REL_RB_INSERT(struct name *head, type *base, type *elm) {    \
        unsigned z = name##_idx(base, elm);                                   \
        /* find place */                                                      \
        unsigned y = REL_NIL;                                                 \
        unsigned x = (head)->rrh_root;                                        \
        while (x != REL_NIL) {                                                \
            y = x;                                                            \
            int c = (cmp)(elm, name##_ptr(base, x));                          \
            if (c < 0)                                                        \
                x = name##_left_idx(base, x);                                 \
            else if (c > 0)                                                   \
                x = name##_right_idx(base, x);                                \
            else                                                              \
                return name##_ptr(base, x);                                   \
        }                                                                     \
        /* link z under y */                                                  \
        elm->field.rbe_parent = y;                                            \
        elm->field.rbe_left = REL_NIL;                                        \
        elm->field.rbe_right = REL_NIL;                                       \
        elm->field.rbe_color = REL_RB_RED;                                    \
        if (y == REL_NIL)                                                     \
            (head)->rrh_root = z;                                             \
        else if ((cmp)(elm, name##_ptr(base, y)) < 0)                         \
            name##_set_left(base, y, z);                                      \
        else                                                                  \
            name##_set_right(base, y, z);                                     \
        /* fix-up */                                                          \
        while ((z != (head)->rrh_root) &&                                     \
               name##_color_idx(base, name##_parent_idx(base, z)) == REL_RB_RED) {  \
            unsigned p = name##_parent_idx(base, z);                          \
            unsigned g = name##_parent_idx(base, p);                          \
            if (p == name##_left_idx(base, g)) {                              \
                unsigned u = name##_right_idx(base, g); /* uncle */           \
                if (name##_color_idx(base, u) == REL_RB_RED) {                \
                    name##_set_color_idx(base, p, REL_RB_BLACK);              \
                    name##_set_color_idx(base, u, REL_RB_BLACK);              \
                    name##_set_color_idx(base, g, REL_RB_RED);                \
                    z = g;                                                    \
                } else {                                                      \
                    if (z == name##_right_idx(base, p)) {                     \
                        z = p;                                                \
                        name##_rotate_left(head, base, z);                    \
                        p = name##_parent_idx(base, z);                       \
                        g = name##_parent_idx(base, p);                       \
                    }                                                         \
                    name##_set_color_idx(base, p, REL_RB_BLACK);              \
                    name##_set_color_idx(base, g, REL_RB_RED);                \
                    name##_rotate_right(head, base, g);                       \
                }                                                             \
            } else { /* mirror */                                             \
                unsigned u = name##_left_idx(base, g);                        \
                if (name##_color_idx(base, u) == REL_RB_RED) {                \
                    name##_set_color_idx(base, p, REL_RB_BLACK);              \
                    name##_set_color_idx(base, u, REL_RB_BLACK);              \
                    name##_set_color_idx(base, g, REL_RB_RED);                \
                    z = g;                                                    \
                } else {                                                      \
                    if (z == name##_left_idx(base, p)) {                      \
                        z = p;                                                \
                        name##_rotate_right(head, base, z);                   \
                        p = name##_parent_idx(base, z);                       \
                        g = name##_parent_idx(base, p);                       \
                    }                                                         \
                    name##_set_color_idx(base, p, REL_RB_BLACK);              \
                    name##_set_color_idx(base, g, REL_RB_RED);                \
                    name##_rotate_left(head, base, g);                        \
                }                                                             \
            }                                                                 \
        }                                                                     \
        name##_set_color_idx(base, (head)->rrh_root, REL_RB_BLACK);           \
        if ((head)->rrh_root != REL_NIL)                                      \
            name##_ptr(base, (head)->rrh_root)->field.rbe_parent = REL_NIL;   \
        return NULL;                                                          \
    }                                                                         \
    type *name##_REL_RB_MINMAX(struct name *head, type *base, int dir) {      \
        unsigned x = (head)->rrh_root;                                        \
        if (x == REL_NIL)                                                     \
            return NULL;                                                      \
        if (dir < 0) { /* MIN */                                              \
            while (name##_left_idx(base, x) != REL_NIL)                       \
                x = name##_left_idx(base, x);                                 \
        } else {                                                              \
            while (name##_right_idx(base, x) != REL_NIL)                      \
                x = name##_right_idx(base, x);                                \
        }                                                                     \
        return name##_ptr(base, x);                                           \
    }                                                                         \
    /* ---- successor / predecessor ---- */                                   \
    type *name##_REL_RB_NEXT(type *base, type *elm) {                         \
        unsigned x = REL_IDX_FROM_PTR(base, elm);                             \
        if (x == REL_NIL)                                                     \
            return NULL;                                                      \
        unsigned r = name##_right_idx(base, x);                               \
        if (r != REL_NIL) {                                                   \
            x = r;                                                            \
            while (name##_left_idx(base, x) != REL_NIL)                       \
                x = name##_left_idx(base, x);                                 \
            return name##_ptr(base, x);                                       \
        } else {                                                              \
            unsigned p = name##_parent_idx(base, x);                          \
            while (p != REL_NIL && x == name##_right_idx(base, p)) {          \
                x = p;                                                        \
                p = name##_parent_idx(base, x);                               \
            }                                                                 \
            return name##_ptr(base, p);                                       \
        }                                                                     \
    }                                                                         \
    type *name##_REL_RB_PREV(type *base, type *elm) {                         \
        unsigned x = REL_IDX_FROM_PTR(base, elm);                             \
        if (x == REL_NIL)                                                     \
            return NULL;                                                      \
        unsigned l = name##_left_idx(base, x);                                \
        if (l != REL_NIL) {                                                   \
            x = l;                                                            \
            while (name##_right_idx(base, x) != REL_NIL)                      \
                x = name##_right_idx(base, x);                                \
            return name##_ptr(base, x);                                       \
        } else {                                                              \
            unsigned p = name##_parent_idx(base, x);                          \
            while (p != REL_NIL && x == name##_left_idx(base, p)) {           \
                x = p;                                                        \
                p = name##_parent_idx(base, x);                               \
            }                                                                 \
            return name##_ptr(base, p);                                       \
        }                                                                     \
    }                                                                         \
    type *name##_REL_RB_FIND(struct name *head, type *base, const type *key) { \
        unsigned x = (head)->rrh_root;                                        \
        while (x != REL_NIL) {                                                \
            int c = (cmp)(key, name##_ptr(base, x));                          \
            if (c < 0)                                                        \
                x = name##_left_idx(base, x);                                 \
            else if (c > 0)                                                   \
                x = name##_right_idx(base, x);                                \
            else                                                              \
                return name##_ptr(base, x);                                   \
        }                                                                     \
        return NULL;                                                          \
    }                                                                         \
    type *name##_REL_RB_NFIND(struct name *head, type *base, const type *key) {\
        unsigned x = (head)->rrh_root, res = REL_NIL;                         \
        while (x != REL_NIL) {                                                \
            int c = (cmp)(key, name##_ptr(base, x));                          \
            if (c <= 0) {                                                     \
                res = x;                                                      \
                x = name##_left_idx(base, x);                                 \
            } else                                                            \
                x = name##_right_idx(base, x);                                \
        }                                                                     \
        return name##_ptr(base, res);                                         \
    }                                                                         \
    type *name##_REL_RB_REMOVE(struct name *head, type *base, type *elm) {    \
        unsigned z = REL_IDX_FROM_PTR(base, elm);                             \
        unsigned y = z;                                                       \
        unsigned y_color = name##_color_idx(base, y);                         \
        unsigned x = REL_NIL;                                                 \
        unsigned x_parent = REL_NIL;                                          \
        if (name##_left_idx(base, z) == REL_NIL) {                            \
            x = name##_right_idx(base, z);                                    \
            x_parent = name##_parent_idx(base, z);                            \
            name##_transplant(head, base, z, x);                              \
        } else if (name##_right_idx(base, z) == REL_NIL) {                    \
            x = name##_left_idx(base, z);                                     \
            x_parent = name##_parent_idx(base, z);                            \
            name##_transplant(head, base, z, x);                              \
        } else {                                                              \
            /* successor */                                                   \
            y = name##_right_idx(base, z);                                    \
            while (name##_left_idx(base, y) != REL_NIL)                       \
                y = name##_left_idx(base, y);                                 \
            y_color = name##_color_idx(base, y);                              \
            x = name##_right_idx(base, y);                                    \
            if (name##_parent_idx(base, y) == z) {                            \
                x_parent = y;                                                 \
                if (x != REL_NIL)                                             \
                    name##_ptr(base, x)->field.rbe_parent = y;                \
            } else {                                                          \
                x_parent = name##_parent_idx(base, y);                        \
                name##_transplant(head, base, y, x);                          \
                name##_set_right(base, y, name##_right_idx(base, z));         \
            }                                                                 \
            name##_transplant(head, base, z, y);                              \
            name##_set_left(base, y, name##_left_idx(base, z));               \
            name##_set_color_idx(base, y, name##_color_idx(base, z));         \
        }                                                                     \
        /* fix-up */                                                          \
        if (y_color == REL_RB_BLACK) {                                        \
            unsigned xi = x;                                                  \
            unsigned xpi = x_parent;                                          \
            while ((xi != (head)->rrh_root) &&                                \
                   (name##_color_idx(base, xi) == REL_RB_BLACK)) {            \
                if (xi == name##_left_idx(base, xpi)) {                       \
                    unsigned w = name##_right_idx(base, xpi);                 \
                    if (name##_color_idx(base, w) == REL_RB_RED) {            \
                        name##_set_color_idx(base, w, REL_RB_BLACK);          \
                        name##_set_color_idx(base, xpi, REL_RB_RED);          \
                        name##_rotate_left(head, base, xpi);                  \
                        w = name##_right_idx(base, xpi);                      \
                    }                                                         \
                    if (name##_color_idx(base, name##_left_idx(base, w)) ==   \
                            REL_RB_BLACK &&                                   \
                        name##_color_idx(base, name##_right_idx(base, w)) ==  \
                            REL_RB_BLACK) {                                   \
                        name##_set_color_idx(base, w, REL_RB_RED);            \
                        xi = xpi;                                             \
                        xpi = name##_parent_idx(base, xi);                    \
                    } else {                                                  \
                        if (name##_color_idx(base, name##_right_idx(base, w)) ==    \
                            REL_RB_BLACK) {                                   \
                            name##_set_color_idx(base, name##_left_idx(base, w),    \
                                                 REL_RB_BLACK);               \
                            name##_set_color_idx(base, w, REL_RB_RED);        \
                            name##_rotate_right(head, base, w);               \
                            w = name##_right_idx(base, xpi);                  \
                        }                                                     \
                        name##_set_color_idx(base, w, name##_color_idx(base, xpi)); \
                        name##_set_color_idx(base, xpi, REL_RB_BLACK);        \
                        name##_set_color_idx(base, name##_right_idx(base, w), \
                                             REL_RB_BLACK);                   \
                        name##_rotate_left(head, base, xpi);                  \
                        xi = (head)->rrh_root;                                \
                        xpi = REL_NIL;                                        \
                    }                                                         \
                } else { /* mirror */                                         \
                    unsigned w = name##_left_idx(base, xpi);                  \
                    if (name##_color_idx(base, w) == REL_RB_RED) {            \
                        name##_set_color_idx(base, w, REL_RB_BLACK);          \
                        name##_set_color_idx(base, xpi, REL_RB_RED);          \
                        name##_rotate_right(head, base, xpi);                 \
                        w = name##_left_idx(base, xpi);                       \
                    }                                                         \
                    if (name##_color_idx(base, name##_right_idx(base, w)) ==  \
                            REL_RB_BLACK &&                                   \
                        name##_color_idx(base, name##_left_idx(base, w)) ==   \
                            REL_RB_BLACK) {                                   \
                        name##_set_color_idx(base, w, REL_RB_RED);            \
                        xi = xpi;                                             \
                        xpi = name##_parent_idx(base, xi);                    \
                    } else {                                                  \
                        if (name##_color_idx(base, name##_left_idx(base, w)) ==     \
                            REL_RB_BLACK) {                                   \
                            name##_set_color_idx(base, name##_right_idx(base, w),   \
                                                 REL_RB_BLACK);               \
                            name##_set_color_idx(base, w, REL_RB_RED);        \
                            name##_rotate_left(head, base, w);                \
                            w = name##_left_idx(base, xpi);                   \
                        }                                                     \
                        name##_set_color_idx(base, w, name##_color_idx(base, xpi)); \
                        name##_set_color_idx(base, xpi, REL_RB_BLACK);        \
                        name##_set_color_idx(base, name##_left_idx(base, w),  \
                                             REL_RB_BLACK);                   \
                        name##_rotate_right(head, base, xpi);                 \
                        xi = (head)->rrh_root;                                \
                        xpi = REL_NIL;                                        \
                    }                                                         \
                }                                                             \
            }                                                                 \
            name##_set_color_idx(base, xi, REL_RB_BLACK);                     \
        }                                                                     \
        if ((head)->rrh_root != REL_NIL)                                      \
            name##_ptr(base, (head)->rrh_root)->field.rbe_parent = REL_NIL;   \
        elm->field.rbe_parent = elm->field.rbe_left = elm->field.rbe_right =  \
            REL_NIL;                                                          \
        elm->field.rbe_color = REL_RB_RED;                                    \
        return elm;                                                           \
    }

#endif /* _REL_TREE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
