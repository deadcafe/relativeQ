/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2025 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 */
#ifndef _REL_HASH_H_
#define _REL_HASH_H_

#include <stddef.h>
#include <limits.h>

#ifndef REL_FORCE_INLINE
# if defined(_MSC_VER)
#  define REL_FORCE_INLINE __forceinline
# elif defined(__GNUC__) || defined(__clang__)
#  define REL_FORCE_INLINE __attribute__((always_inline)) inline
# else
#  define REL_FORCE_INLINE inline
# endif
#endif

#ifndef REL_NIL
# define REL_NIL ((unsigned) 0u)
#endif

#ifndef REL_IDX_IS_NIL
# define REL_IDX_IS_NIL(i)    ((unsigned)(i) == REL_NIL)
#endif

#ifndef REL_IDX_FROM_PTR(base, p)
# define REL_IDX_FROM_PTR(base, p)                                      \
    ((unsigned)((p) ? (unsigned)((p) - (base)) + 1u : (unsigned)REL_NIL))
#endif

#ifndef REL_PTR_FROM_IDX
# define REL_PTR_FROM_IDX(base, i)                                      \
    (( (unsigned)(i) != REL_NIL ) ? ((base) + ((unsigned)(i) - 1u)) : NULL)
#endif

#define REL_HASH_BUCKET_ENTRY_SZ	16
#define REL_HASH_INVALID_HASH		REL_NIL
#define REL_HASH_INVALID_IDX		REL_NIL

union rel_hash_hash_u {
    uint64_t val64;
    uint32_t val32[2];
};

#define REL_HASH_ENTRY(type)                    \
    struct type {                               \
        union rel_hash_hash_u hash;             \
        REL_TAILQ_ENTRY(type) entry;            \
    }

typedef union rel_hash_val_u (*hash_calc_func_t)(const void *base, unsigned index);


struct rel_hash_bucket_s {
    uint32_t hash[REL_HASH_BUCKET_ENTRY_SZ];
    uint32_t idx[REL_HASH_BUCKET_ENTRY_SZ];
};


static REL_FORCE_INLINE unsigned
rel_hash_find_u32(const uint32_t *u32x16_p,
                  uint32_t val)
{
    unsgiend ret = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (u32x16_p[i] == val)
            ret |= (1 << i);
    }
    return ret;
}
static REL_FORCE_INLINE unsigned
rel_hash_idx_in_bk(struct rel_hash_bucket_s *bk,
                   unsigned idx)
{
    return rel_hash_find_u32(bk->idx, idx);
}
static REL_FORCE_INLINE unsigned
rel_hash_hash_in_bk(struct rel_hash_bucket_s *bk,
                    unsigned hash)
{
    return rel_hash_find_u32(bk->hash, hash);
}
static REL_FORCE_INLINE int
rel_hash_insert(struct rel_hash_bucket_s *tbl,
                unsigned mask,
                union rel_hash_val_u hash,
                unsigned index)
{

}
static REL_FORCE_INLINE int
rel_hash_remove(struct rel_hash_bucket_s *tbl,
                unsigned mask,
                union rel_hash_val_u hash,
                unsigned index)
{

}
static REL_FORCE_INLINE int
rel_hash_kickout(struct rel_hash_bucket_s *tbl,
                 unsigned mask,
                 struct rel_hash_bucket_s *bk,
                 unsigned pos)
{
    unsigned empties = rel_hash_idx_in_bk(bk, REL_HASH_INVALID_IDX);
    if (empties) {
    } else {
    }
}




#endif	/* !_REL_HASH_H_ */
