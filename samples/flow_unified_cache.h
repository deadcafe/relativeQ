/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * flow_unified_cache.h - Unified IPv4/IPv6 flow cache.
 *
 * Single hash table handles both address families.
 * Key includes family field so v4/v6 entries never collide.
 */

#ifndef _FLOW_UNIFIED_CACHE_H_
#define _FLOW_UNIFIED_CACHE_H_

#include <string.h>
#include <rix/rix_defs.h>
#include <rix/rix_queue.h>
#include <rix/rix_hash.h>
#include "flow_cache_defs.h"

/*===========================================================================
 * Unified flow key: 5-tuple + vrfid + family (44 bytes)
 *
 * IPv4 entries zero-pad the unused 24 bytes in addr.v4.pad.
 * family is part of the key, so v4/v6 entries with the same
 * port/proto/vrfid never match.
 *===========================================================================*/
#define FLOW_FAMILY_IPV4  4
#define FLOW_FAMILY_IPV6  6

struct flowu_key {
    uint8_t  family;       /*  1B: FLOW_FAMILY_IPV4 / IPV6 */
    uint8_t  proto;        /*  1B */
    uint16_t src_port;     /*  2B */
    uint16_t dst_port;     /*  2B */
    uint16_t pad;          /*  2B */
    uint32_t vrfid;        /*  4B */
    union {                /* 32B */
        struct {
            uint32_t src;
            uint32_t dst;
            uint8_t  _pad[24];
        } v4;
        struct {
            uint8_t  src[16];
            uint8_t  dst[16];
        } v6;
    } addr;
} __attribute__((packed));  /* 44B total */

/*===========================================================================
 * Key construction helpers
 *===========================================================================*/
static inline struct flowu_key
flowu_key_v4(uint32_t src_ip, uint32_t dst_ip,
             uint16_t src_port, uint16_t dst_port,
             uint8_t proto, uint32_t vrfid)
{
    struct flowu_key k;
    memset(&k, 0, sizeof(k));
    k.family   = FLOW_FAMILY_IPV4;
    k.proto    = proto;
    k.src_port = src_port;
    k.dst_port = dst_port;
    k.vrfid    = vrfid;
    k.addr.v4.src = src_ip;
    k.addr.v4.dst = dst_ip;
    return k;
}

static inline struct flowu_key
flowu_key_v6(const uint8_t *src_ip, const uint8_t *dst_ip,
             uint16_t src_port, uint16_t dst_port,
             uint8_t proto, uint32_t vrfid)
{
    struct flowu_key k;
    memset(&k, 0, sizeof(k));
    k.family   = FLOW_FAMILY_IPV6;
    k.proto    = proto;
    k.src_port = src_port;
    k.dst_port = dst_port;
    k.vrfid    = vrfid;
    memcpy(k.addr.v6.src, src_ip, 16);
    memcpy(k.addr.v6.dst, dst_ip, 16);
    return k;
}

/*===========================================================================
 * Unified flow entry (128 bytes = 2 cache lines)
 *
 * CL0 (0-63):  lookup hot path
 * CL1 (64-127): counters & management
 *===========================================================================*/
struct flowu_entry {
    FLOW_CACHE_CL(0);                   /* lookup + expire hot path */
    struct flowu_key     key;           /* 44B */
    uint32_t             cur_hash;      /*  4B: hash_field for O(1) remove */
    uint64_t             last_ts;       /*  8B: last access TSC (0 = invalid) */
    RIX_SLIST_ENTRY(struct flowu_entry) free_link;  /* 4B: free list */
    uint8_t              reserved0[4];  /*  4B: pad to 64B */

    FLOW_CACHE_CL(1);                   /* counters & management */
    uint32_t             action;        /*  4B: cached ACL result */
    uint32_t             qos_class;     /*  4B: cached QoS class */
    uint64_t             packets;       /*  8B */
    uint64_t             bytes;         /*  8B */
    uint8_t              reserved1[40]; /* 40B: pad to 64B */
} __attribute__((aligned(FLOW_CACHE_LINE_SIZE)));

/*===========================================================================
 * Key comparison: full 44-byte memcmp (family included)
 *===========================================================================*/
static inline int __attribute__((nonnull(1,2)))
flowu_cmp(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(struct flowu_key)) == 0;
}

/*===========================================================================
 * Hash table head type: flowu_ht
 * RIX_HASH_GENERATE is expanded in each *.c that needs the implementations.
 *===========================================================================*/
RIX_HASH_HEAD(flowu_ht);

/*===========================================================================
 * Cache struct, API declarations, and inline helpers
 *===========================================================================*/
#define FC_PREFIX       flowu
#define FC_ENTRY        flowu_entry
#define FC_KEY          flowu_key
#define FC_CACHE        flowu_cache
#define FC_HT           flowu_ht
#define FC_FREE_HEAD    flowu_free_head
#include "flow_cache_common.h"
#undef FC_PREFIX
#undef FC_ENTRY
#undef FC_KEY
#undef FC_CACHE
#undef FC_HT
#undef FC_FREE_HEAD

#endif /* _FLOW_UNIFIED_CACHE_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
