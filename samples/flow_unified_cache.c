/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#include "flow_unified_cache.h"

#define FC_PREFIX       flowu
#define FC_ENTRY        flowu_entry
#define FC_KEY          flowu_key
#define FC_CACHE        flowu_cache
#define FC_HT_PREFIX    flowu_ht
#define FC_FREE_HEAD    flowu_free_head
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
