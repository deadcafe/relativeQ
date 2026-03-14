/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include "flow4_cache.h"

RIX_HASH_GENERATE_STATIC(flow4_ht, flow4_entry, key, cur_hash, flow4_cmp)

#define FC_PREFIX       flow4
#define FC_ENTRY        flow4_entry
#define FC_KEY          flow4_key
#define FC_CACHE        flow4_cache
#define FC_HT_PREFIX    flow4_ht
#define FC_FREE_HEAD    flow4_free_head
#include "flow_cache_body_private.h"
#undef FC_PREFIX
#undef FC_ENTRY
#undef FC_KEY
#undef FC_CACHE
#undef FC_HT_PREFIX
#undef FC_FREE_HEAD

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
