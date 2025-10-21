#ifndef _FCACHE_H_
#define _FCACHE_H_

#include <sys/types.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>

#include "index_queue.h"

#ifndef always_inline
# define always_inline  static inline __attribute__ ((__always_inline__))
#endif  /* !always_inline */

#define CACHELINE_SIZE	64

#ifndef cache_aligned
# define cache_aligned	__attribute__((aligned(CACHELINE_SIZE)))
#endif	/* !cache_aligned */

#define BUCKET_ENTRY_SZ	16
#define NB_ENTRIES_MIN	(BUCKET_ENTRY_SZ * BUCKET_ENTRY_SZ * BUCKET_ENTRY_SZ)
#define MAX_DEPTH	2
#define PIPELINE_NB	9

#define INVALID_HASH64	UINT64_C(-1)	/* -1 : invalid */
#define INVALID_HVAL	UINT32_C(-1)
#define INVALID_IDX	UINT32_C(-1)	/* -1 : invalid */
#define INVALID_STATE	(-1)		/* -1 : invalid */
#define INVALID_FLAGS	UINT64_C(-1)

/*
 * 8 bytes
 */
union hash_u {
    uint64_t val64;
    uint32_t val32[2];
};

/*
 * 48 + 8 bytes
 */
struct flow_key_s {
    union {
        uint8_t data[48];
        uint32_t d32[12];
        uint64_t d64[6];
    } val;
    union hash_u hash;
};

/*
 *　128 bytes
 */
struct flow_node_s {
    struct flow_key_s key;

    IDXQ_ENTRY(flow_node_s) entry;

    /* flow data */
    uint32_t test_id;
    unsigned data[8];

} __attribute__((aligned(CACHELINE_SIZE)));

/*
 * 128 bytes
 */
struct flow_bucket_s {
    uint32_t hval[BUCKET_ENTRY_SZ];		/* hash value */
    uint32_t idx[BUCKET_ENTRY_SZ];		/* node index */
};

/*
 * callback functions
 */
typedef union hash_u (*hash_func_t)(const struct flow_key_s *, uint32_t);
typedef void (*node_initializer_t)(struct flow_node_s *);

IDXQ_HEAD(flow_node_q_s, flow_node_s);

/*
 *
 */
struct idx_pool_s {
    struct flow_node_q_s used_fifo;
    unsigned array_size;
    unsigned nb_used;
    struct flow_node_s *node_array;
    uint32_t *idx_array;
} __attribute__((aligned(CACHELINE_SIZE)));

/*
 *
 */
enum flow_pipeline_state_e {
    FLOW_STATE_INVALID = -1,

    FLOW_XSTATE_WAIT_2,
    FLOW_XSTATE_WAIT_1,
    FLOW_XSTATE_PREFETCH_KEY,
    FLOW_XSTATE_FETCH_BUCKET,
    FLOW_XSTATE_FETCH_NODE,
    FLOW_XSTATE_REFETCH_NODE,
    FLOW_XSTATE_CMP_KEY,

    FLOW_STATE_NB,
};

/*
 *
 */
struct flow_pipeline_ctx_s {
    struct {				/* bucket propaties */
        struct flow_bucket_s *ptr;	/* bucket index */
        uint64_t hits;			/* hval match flags */
    } bk[2];				/* Even/Odd */

    union hash_u hash;
    struct flow_key_s *fkey_p;
    struct flow_node_s **node_pp;

    unsigned idx;				/* request index */
    enum flow_pipeline_state_e state;	/* */
} __attribute__((aligned(CACHELINE_SIZE)));

/*
 *
 */
struct flow_pipeline_engine_s {
    struct flow_pipeline_ctx_s ctx_pool[PIPELINE_NB * 3];

    struct flow_node_s ** node_pp;
    struct flow_key_s ** fkey_pp;
    uint32_t *bk_idx;
    hash_func_t hash_func;
    unsigned pool_size;
    unsigned next;
    unsigned req_nb;
    unsigned node_nb;
};

/*
 * flow_cache_s
 * idx pool
 * node array
 * bcuket array
 */
struct flow_cache_s {
    uint32_t bk_mask;		/* bucket index mask */
    unsigned nb;
    unsigned max;
    bool is_debug;
    unsigned ctx_pool_size;
    unsigned _reserved;

    hash_func_t calc_hash;
    node_initializer_t node_init;

    /* tables */
    struct idx_pool_s *idx_pool;
    struct flow_bucket_s *buckets;
    struct flow_node_s *nodes;

    uint64_t cnt;
    uint64_t tsc;
    uint64_t fails;
    uint64_t cmp_cnt;
    uint64_t cmp_tsc;

    struct flow_pipeline_engine_s engine __attribute__((aligned(CACHELINE_SIZE)));

    char payload[] __attribute__((aligned(CACHELINE_SIZE)));
};

/*
 * prototypes
 */
extern struct flow_cache_s *fcache_create(unsigned nb,
                                          unsigned ctx_size,
                                          hash_func_t func,
                                          node_initializer_t node_init);

extern void fcache_init(struct flow_cache_s *fcache,
                        unsigned nb,
                        unsigned ctx_size,
                        hash_func_t func,
                        node_initializer_t node_init);

extern void fcache_reset(struct flow_cache_s *fcache);

extern struct flow_cache_s *fcache_alloc(unsigned nb);

extern void fcache_free(struct flow_cache_s *fcache);

extern size_t fcache_sizeof(unsigned nb);

extern void free_node(struct flow_cache_s *fcache,
                      struct flow_node_s *node);

extern unsigned find_node_bulk(struct flow_cache_s *fcache,
                               struct flow_key_s **fkey_pp,
                               unsigned nb,
                               struct flow_node_s **node_pp,
                               int with_hash);
/*
 *
 */
static inline struct flow_node_s *
find_node_oneshot(struct flow_cache_s *fcache,
                  struct flow_key_s *fkey_p)
{
        struct flow_node_s *node_p = NULL;

        find_node_bulk(fcache, &fkey_p, 1, &node_p, 0);
        return node_p;
}

/* for debug */
extern int flipflop_node(const struct flow_cache_s *fcache,
                         const struct flow_node_s *node);

extern void dump_fcache(struct flow_cache_s *fcache,
                        FILE *stream,
                        const char *title);

extern void dump_idx_pool(const struct idx_pool_s *pool,
                          FILE *stream,
                          const char *title);

extern void dump_bucket(const struct flow_cache_s *fcache,
                        FILE *stream,
                        const char *title,
                        const struct flow_bucket_s *bk);

extern void dump_node(const struct flow_cache_s *fcache,
                      FILE *stream,
                      const char *title,
                      const struct flow_node_s *node);

extern void dump_key(const struct flow_cache_s *fcache,
                     const struct flow_key_s *key,
                     FILE *stream,
                     const char *title);

extern struct flow_bucket_s *current_bucket(const struct flow_cache_s *fcache,
                                            const struct flow_node_s * node);

extern struct flow_bucket_s *another_bucket(const struct flow_cache_s *fcache,
                                            const struct flow_node_s * node);

extern union hash_u calc_hash_default(const struct flow_key_s *key,
                                      uint32_t mask);

extern int fcache_walk(struct flow_cache_s *fcache,
                       int (*cb_func)(struct flow_cache_s *, struct flow_node_s *, void *),
                       void *arg);

extern unsigned fcache_node_num(const struct flow_cache_s *fcache);

/*
 *
 */
static inline union hash_u
fcache_calc_hash(const struct flow_cache_s *fcache,
                 const struct flow_key_s *key)
{
        return fcache->calc_hash(key, fcache->bk_mask);
}

extern int verify_node(const struct flow_cache_s *fcache,
                       const struct flow_node_s *node,
                       const struct flow_key_s *key);

extern unsigned nb_empty_slot(const struct flow_cache_s *fcache,
                              struct flow_bucket_s *bk);

extern void speed_test_hash(void);
extern void speed_test_cmp_hval_in_bucket(void);


#endif	/* !_FCACHE_H_ */
