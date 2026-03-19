/*-
 * SPDX-License-Identifier: BSD 3-Clause License
 *
 * Copyright (c) 2026 deadcafe.beef@gmail.com
 * All rights reserved.
 *
 * bench_fc.c - fcache benchmark: flow4 / flow6 / flowu.
 *
 * Structured as:
 *   bench_fc_common.h   shared utilities
 *   bench_fc_body.h     include-template (one instantiation per variant)
 *   bench_fc.c          key generators, variant dispatch, main
 *
 * Usage:
 *   fc_bench [variant] <mode> [args...]
 *   fc_bench datapath               (quick 3-variant comparison)
 *   fc_bench flow4 rate_fc_only <desired> <start_fill%> <hit%> <pps>
 *   fc_bench flow6 rate_trace_custom <desired> <start_fill%> <hit%> <pps> ...
 */

#include "bench_fc_common.h"

/*===========================================================================
 * Key generators
 *===========================================================================*/
static inline struct fc_flow4_key
fcb_make_key4(unsigned i)
{
    struct fc_flow4_key k;

    memset(&k, 0, sizeof(k));
    k.src_ip   = 0x0a000000u | (i & 0x00ffffffu);
    k.dst_ip   = 0x14000000u | ((i * 2654435761u) & 0x00ffffffu);
    k.src_port = (uint16_t)(1024u + (i & 0x7fffu));
    k.dst_port = (uint16_t)(2048u + ((i >> 11) & 0x7fffu));
    k.proto    = (uint8_t)(6u + (i & 1u));
    k.vrfid    = 1u + (i >> 24);
    return k;
}

static inline struct fc_flow6_key
fcb_make_key6(unsigned i)
{
    struct fc_flow6_key k;
    uint32_t a, b;

    memset(&k, 0, sizeof(k));
    a = 0x0a000000u | (i & 0x00ffffffu);
    b = 0x14000000u | ((i * 2654435761u) & 0x00ffffffu);
    k.src_ip[0] = 0x20; k.src_ip[1] = 0x01;
    memcpy(&k.src_ip[12], &a, 4);
    k.dst_ip[0] = 0x20; k.dst_ip[1] = 0x01;
    memcpy(&k.dst_ip[12], &b, 4);
    k.src_port = (uint16_t)(1024u + (i & 0x7fffu));
    k.dst_port = (uint16_t)(2048u + ((i >> 11) & 0x7fffu));
    k.proto    = (uint8_t)(6u + (i & 1u));
    k.vrfid    = 1u + (i >> 24);
    return k;
}

static inline struct fc_flowu_key
fcb_make_keyu(unsigned i)
{
    uint32_t src = 0x0a000000u | (i & 0x00ffffffu);
    uint32_t dst = 0x14000000u | ((i * 2654435761u) & 0x00ffffffu);

    return fc_flowu_key_v4(src, dst,
                            (uint16_t)(1024u + (i & 0x7fffu)),
                            (uint16_t)(2048u + ((i >> 11) & 0x7fffu)),
                            (uint8_t)(6u + (i & 1u)),
                            1u + (i >> 24));
}

/*===========================================================================
 * Instantiate per-variant bench functions via include-template
 *===========================================================================*/

/* --- flow4 --- */
#define FCB_PREFIX    flow4
#define FCB_KEY_T     struct fc_flow4_key
#define FCB_RESULT_T  struct fc_flow4_result
#define FCB_ENTRY_T   struct fc_flow4_entry
#define FCB_CACHE_T   struct fc_flow4_cache
#define FCB_CONFIG_T  struct fc_flow4_config
#define FCB_STATS_T   struct fc_flow4_stats
#define FCB_PRESSURE  FC_FLOW4_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FCB_MAKE_KEY(i) fcb_make_key4(i)
#include "bench_fc_body.h"

/* --- flow6 --- */
#define FCB_PREFIX    flow6
#define FCB_KEY_T     struct fc_flow6_key
#define FCB_RESULT_T  struct fc_flow6_result
#define FCB_ENTRY_T   struct fc_flow6_entry
#define FCB_CACHE_T   struct fc_flow6_cache
#define FCB_CONFIG_T  struct fc_flow6_config
#define FCB_STATS_T   struct fc_flow6_stats
#define FCB_PRESSURE  FC_FLOW6_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FCB_MAKE_KEY(i) fcb_make_key6(i)
#include "bench_fc_body.h"

/* --- flowu --- */
#define FCB_PREFIX    flowu
#define FCB_KEY_T     struct fc_flowu_key
#define FCB_RESULT_T  struct fc_flowu_result
#define FCB_ENTRY_T   struct fc_flowu_entry
#define FCB_CACHE_T   struct fc_flowu_cache
#define FCB_CONFIG_T  struct fc_flowu_config
#define FCB_STATS_T   struct fc_flowu_stats
#define FCB_PRESSURE  FC_FLOWU_DEFAULT_PRESSURE_EMPTY_SLOTS
#define FCB_MAKE_KEY(i) fcb_make_keyu(i)
#include "bench_fc_body.h"

/*===========================================================================
 * Quick 3-variant datapath comparison (no args needed)
 *===========================================================================*/
static void
bench_datapath(void)
{
    unsigned desired = 32768u;
    unsigned max_entries = fcb_pool_count(desired);
    unsigned nb_bk = fcb_nb_bk_hint(max_entries);
    unsigned prefill_n = max_entries / 2u;

    struct fcb_flow4_ctx ctx4;
    struct fcb_flow6_ctx ctx6;
    struct fcb_flowu_ctx ctxu;

    fcb_flow4_ctx_init(&ctx4, nb_bk, max_entries, 1000000000ull);
    fcb_flow6_ctx_init(&ctx6, nb_bk, max_entries, 1000000000ull);
    fcb_flowu_ctx_init(&ctxu, nb_bk, max_entries, 1000000000ull);

    /* generate keys */
    struct fc_flow4_key *p4 = fcb_alloc((size_t)prefill_n * sizeof(*p4));
    struct fc_flow6_key *p6 = fcb_alloc((size_t)prefill_n * sizeof(*p6));
    struct fc_flowu_key *pu = fcb_alloc((size_t)prefill_n * sizeof(*pu));
    struct fc_flow4_key *h4 = fcb_alloc((size_t)FCB_QUERY * sizeof(*h4));
    struct fc_flow6_key *h6 = fcb_alloc((size_t)FCB_QUERY * sizeof(*h6));
    struct fc_flowu_key *hu = fcb_alloc((size_t)FCB_QUERY * sizeof(*hu));
    struct fc_flow4_key *m4 = fcb_alloc((size_t)FCB_QUERY * sizeof(*m4));
    struct fc_flow6_key *m6 = fcb_alloc((size_t)FCB_QUERY * sizeof(*m6));
    struct fc_flowu_key *mu = fcb_alloc((size_t)FCB_QUERY * sizeof(*mu));
    struct fc_flow4_key *x4 = fcb_alloc((size_t)FCB_QUERY * sizeof(*x4));
    struct fc_flow6_key *x6 = fcb_alloc((size_t)FCB_QUERY * sizeof(*x6));
    struct fc_flowu_key *xu = fcb_alloc((size_t)FCB_QUERY * sizeof(*xu));

    for (unsigned i = 0; i < prefill_n; i++) {
        p4[i] = fcb_make_key4(i);
        p6[i] = fcb_make_key6(i);
        pu[i] = fcb_make_keyu(i);
    }
    for (unsigned i = 0; i < FCB_QUERY; i++) {
        h4[i] = fcb_make_key4(i);
        h6[i] = fcb_make_key6(i);
        hu[i] = fcb_make_keyu(i);
        m4[i] = fcb_make_key4(max_entries + i);
        m6[i] = fcb_make_key6(max_entries + i);
        mu[i] = fcb_make_keyu(max_entries + i);
        /* mixed: 90% hit / 10% miss */
        unsigned idx = (i < (FCB_QUERY * 9u / 10u))
                       ? (i % prefill_n)
                       : (prefill_n + i);
        x4[i] = fcb_make_key4(idx);
        x6[i] = fcb_make_key6(idx);
        xu[i] = fcb_make_keyu(idx);
    }

    (void)fcb_flow4_prefill(&ctx4, p4, prefill_n, 1u);
    (void)fcb_flow6_prefill(&ctx6, p6, prefill_n, 1u);
    (void)fcb_flowu_prefill(&ctxu, pu, prefill_n, 1u);

    printf("fcache variant comparison: entries=%u nb_bk=%u query=%u\n",
           max_entries, nb_bk, FCB_QUERY);
    printf("  entries active: flow4=%u  flow6=%u  flowu=%u\n\n",
           fc_flow4_cache_nb_entries(&ctx4.fc),
           fc_flow6_cache_nb_entries(&ctx6.fc),
           fc_flowu_cache_nb_entries(&ctxu.fc));

    printf("pure datapath (cycles/key, no expire):\n");
    fcb_emit3("hit_lookup",
               fcb_flow4_bench_hit(&ctx4, h4, FCB_QUERY, FCB_HIT_REPEAT),
               fcb_flow6_bench_hit(&ctx6, h6, FCB_QUERY, FCB_HIT_REPEAT),
               fcb_flowu_bench_hit(&ctxu, hu, FCB_QUERY, FCB_HIT_REPEAT));
    fcb_emit3("miss_fill",
               fcb_flow4_bench_miss_fill(&ctx4, m4, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flow6_bench_miss_fill(&ctx6, m6, FCB_QUERY, FCB_MISS_REPEAT),
               fcb_flowu_bench_miss_fill(&ctxu, mu, FCB_QUERY, FCB_MISS_REPEAT));

    /* re-prefill for mixed */
    fcb_flow4_ctx_reset(&ctx4);
    fcb_flow6_ctx_reset(&ctx6);
    fcb_flowu_ctx_reset(&ctxu);
    (void)fcb_flow4_prefill(&ctx4, p4, prefill_n, 1u);
    (void)fcb_flow6_prefill(&ctx6, p6, prefill_n, 1u);
    (void)fcb_flowu_prefill(&ctxu, pu, prefill_n, 1u);

    fcb_emit3("mixed_90_10",
               fcb_flow4_bench_mixed(&ctx4, p4, prefill_n, x4, FCB_QUERY, FCB_MIXED_REPEAT),
               fcb_flow6_bench_mixed(&ctx6, p6, prefill_n, x6, FCB_QUERY, FCB_MIXED_REPEAT),
               fcb_flowu_bench_mixed(&ctxu, pu, prefill_n, xu, FCB_QUERY, FCB_MIXED_REPEAT));

    free(xu); free(x6); free(x4);
    free(mu); free(m6); free(m4);
    free(hu); free(h6); free(h4);
    free(pu); free(p6); free(p4);
    fcb_flowu_ctx_free(&ctxu);
    fcb_flow6_ctx_free(&ctx6);
    fcb_flow4_ctx_free(&ctx4);
}

/*===========================================================================
 * Variant dispatch helpers
 *===========================================================================*/
typedef void (*rate_fc_only_fn)(unsigned, unsigned, unsigned, unsigned, unsigned);
typedef void (*rate_trace_fn)(unsigned, unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, unsigned);

static void
select_variant(const char *name,
               rate_fc_only_fn *out_fc_only,
               rate_trace_fn *out_trace)
{
    if (strcmp(name, "flow4") == 0) {
        *out_fc_only = fcb_flow4_rate_fc_only;
        *out_trace    = fcb_flow4_rate_trace_custom;
    } else if (strcmp(name, "flow6") == 0) {
        *out_fc_only = fcb_flow6_rate_fc_only;
        *out_trace    = fcb_flow6_rate_trace_custom;
    } else if (strcmp(name, "flowu") == 0) {
        *out_fc_only = fcb_flowu_rate_fc_only;
        *out_trace    = fcb_flowu_rate_trace_custom;
    } else {
        fprintf(stderr, "unknown variant: %s (use flow4/flow6/flowu)\n", name);
        exit(2);
    }
}

/*===========================================================================
 * main
 *===========================================================================*/
static void
usage(const char *prog)
{
    printf("usage:\n");
    printf("  %s datapath\n", prog);
    printf("  %s [flow4|flow6|flowu] rate_fc_only <desired> <start_fill%%> <hit%%> <pps>\n", prog);
    printf("  %s [flow4|flow6|flowu] rate_trace_custom <desired> <start_fill%%> <hit%%> <pps>"
           " <timeout_ms> <soak_mul> <report_ms>"
           " <fill0> <fill1> <fill2> <fill3>"
           " <k0> <k1> <k2> <k3> [kick_scale]\n", prog);
}

int
main(int argc, char **argv)
{
    const char *variant = "flow4";
    const char *mode;
    int arg_off;

    rix_hash_arch_init(RIX_HASH_ARCH_AUTO);

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /* No-args mode */
    if (strcmp(argv[1], "datapath") == 0) {
        bench_datapath();
        return 0;
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    /* Detect optional variant argument */
    if (strcmp(argv[1], "flow4") == 0 ||
        strcmp(argv[1], "flow6") == 0 ||
        strcmp(argv[1], "flowu") == 0) {
        variant = argv[1];
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        mode = argv[2];
        arg_off = 3;
    } else {
        mode = argv[1];
        arg_off = 2;
    }

    rate_fc_only_fn fn_fc_only;
    rate_trace_fn fn_trace;

    select_variant(variant, &fn_fc_only, &fn_trace);

    if (strcmp(mode, "rate_fc_only") == 0) {
        if (argc - arg_off < 4) {
            fprintf(stderr, "rate_fc_only requires: <desired> <start_fill%%> <hit%%> <pps>\n");
            return 2;
        }
        unsigned desired  = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        unsigned sfill    = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
        unsigned hit      = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
        unsigned pps      = (unsigned)strtoul(argv[arg_off + 3], NULL, 10);
        unsigned nb_bk    = fcb_nb_bk_hint(fcb_pool_count(desired));

        printf("\n%s fc-only persistent packet path\n", variant);
        printf("pps=%u  hit_target=%u%%  start_fill=%u%%  timeout=1000ms"
               "  nb_bk=%u  pool=%u\n",
               pps, hit, sfill, nb_bk, fcb_pool_count(desired));
        fn_fc_only(desired, nb_bk, sfill, hit, pps);
        return 0;
    }

    if (strcmp(mode, "rate_trace_custom") == 0) {
        if (argc - arg_off < 15) {
            fprintf(stderr, "rate_trace_custom requires: <desired> <start_fill%%>"
                    " <hit%%> <pps> <timeout_ms> <soak_mul> <report_ms>"
                    " <fill0> <fill1> <fill2> <fill3>"
                    " <k0> <k1> <k2> <k3> [kick_scale]\n");
            return 2;
        }
        unsigned desired  = (unsigned)strtoul(argv[arg_off + 0], NULL, 10);
        unsigned sfill    = (unsigned)strtoul(argv[arg_off + 1], NULL, 10);
        unsigned hit      = (unsigned)strtoul(argv[arg_off + 2], NULL, 10);
        unsigned pps      = (unsigned)strtoul(argv[arg_off + 3], NULL, 10);
        unsigned tmo      = (unsigned)strtoul(argv[arg_off + 4], NULL, 10);
        unsigned soak     = (unsigned)strtoul(argv[arg_off + 5], NULL, 10);
        unsigned rpt      = (unsigned)strtoul(argv[arg_off + 6], NULL, 10);
        unsigned f0       = (unsigned)strtoul(argv[arg_off + 7], NULL, 10);
        unsigned f1       = (unsigned)strtoul(argv[arg_off + 8], NULL, 10);
        unsigned f2       = (unsigned)strtoul(argv[arg_off + 9], NULL, 10);
        unsigned f3       = (unsigned)strtoul(argv[arg_off + 10], NULL, 10);
        unsigned k0       = (unsigned)strtoul(argv[arg_off + 11], NULL, 10);
        unsigned k1       = (unsigned)strtoul(argv[arg_off + 12], NULL, 10);
        unsigned k2       = (unsigned)strtoul(argv[arg_off + 13], NULL, 10);
        unsigned k3       = (unsigned)strtoul(argv[arg_off + 14], NULL, 10);
        unsigned kscale   = (argc - arg_off >= 16)
                            ? (unsigned)strtoul(argv[arg_off + 15], NULL, 10)
                            : 1u;
        unsigned nb_bk    = fcb_nb_bk_hint(fcb_pool_count(desired));

        fn_trace(desired, nb_bk, sfill, hit, pps, tmo, kscale, soak, rpt,
                 f0, f1, f2, f3, k0, k1, k2, k3);
        return 0;
    }

    fprintf(stderr, "unknown mode: %s\n", mode);
    usage(argv[0]);
    return 2;
}

/*
 * Local Variables:
 * c-file-style: "bsd"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * tab-width: 4
 * End:
 */
