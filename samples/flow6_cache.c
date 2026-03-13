/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include "flow6_cache.h"

RIX_HASH_GENERATE_STATIC(flow6_ht, flow6_entry, key, cur_hash, flow6_cmp)

#define FC_PREFIX       flow6
#define FC_ENTRY        flow6_entry
#define FC_KEY          flow6_key
#define FC_CACHE        flow6_cache
#define FC_HT_PREFIX    flow6_ht
#define FC_FREE_HEAD    flow6_free_head
#include "flow_cache_body.h"
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
