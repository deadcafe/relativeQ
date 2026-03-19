/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 */

#ifndef _FLOW_CACHE2_H_
#define _FLOW_CACHE2_H_

/*
 * fcache2 currently reuses the same flow4 batch lookup pipeline geometry as
 * fcache so the two implementations remain directly comparable.
 */
#ifndef FLOW_CACHE_LOOKUP_STEP_KEYS
#define FLOW_CACHE_LOOKUP_STEP_KEYS   16u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_STEPS
#define FLOW_CACHE_LOOKUP_AHEAD_STEPS 8u
#endif
#ifndef FLOW_CACHE_LOOKUP_AHEAD_KEYS
#define FLOW_CACHE_LOOKUP_AHEAD_KEYS \
    (FLOW_CACHE_LOOKUP_STEP_KEYS * FLOW_CACHE_LOOKUP_AHEAD_STEPS)
#endif

#include "flow4_cache2.h"
#include "flow6_cache2.h"
#include "flowu_cache2.h"

#endif /* _FLOW_CACHE2_H_ */

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
