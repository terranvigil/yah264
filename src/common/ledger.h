/*
 * ledger.h - op-count ledger for the differential next264-vs-x264 work
 * comparison (docs/archive/x264-structural-walk.md). Compile-time gated: build with
 * -DN264_OP_LEDGER (meson: -Dc_args=-DN264_OP_LEDGER in a dedicated build
 * dir). Single-threaded measurement runs only (plain increments, no atomics);
 * field layout mirrors the x264-side ledger so the dump lines diff 1:1.
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef N264_LEDGER_H
#define N264_LEDGER_H

/* SATD attribution sites. Declared unconditionally so NLED_SITE(...) call sites
 * still name-resolve in a normal build, where the macro compiles to nothing. */
enum {
    N264_LED_SITE_OTHER,
    N264_LED_SITE_LOWRES,   /* lookahead / mb-tree lowres ME (bitrate-blind) */
    N264_LED_SITE_PME,      /* P inter: ME probes + partition SATD rank */
    N264_LED_SITE_PINTRA,   /* P intra analysis */
    N264_LED_SITE_BME,      /* B inter: mode build + ME */
    N264_LED_SITE_BINTRA,   /* B intra analysis */
    N264_LED_SITE_IFRAME,   /* I-slice intra analysis */
    N264_LED_SITE_N
};

#ifdef N264_OP_LEDGER
#include <stdint.h>
typedef struct {
    uint64_t sad_call, sad_pix;
    uint64_t satd_call, satd_pix;
    uint64_t sa8d_call, sa8d_pix;
    uint64_t ssd_call, ssd_pix;
    uint64_t var_pix, hac_call;
    uint64_t mc_luma_call, mc_luma_pix;
    uint64_t getref_ptr, getref_build, getref_pix;
    uint64_t mc_chroma_call, mc_chroma_pix;
    uint64_t avg_call, avg_pix;
    uint64_t hpel_call;
    uint64_t q4_blk, q8_blk, qdc_blk, dq4_blk, dq8_blk;
    uint64_t dct4_blk, dct8_blk, idct4_blk, idct8_blk, idctdc_blk, dcxf_blk;
    uint64_t bin_real, bin_bypass, bin_term, bin_est;
    uint64_t trellis4, trellis8, trellisdc;
    uint64_t rd_mb, rd_part;
    uint64_t probe_int, probe_sub, cavlc_scratch, est_mb;
    /* satd_pix split by CALLER. The totals above say satd volume is flat across
 * bitrate while x264's falls 32%; they cannot say which caller is flat, and
 * guessing from a sampling profile is how buckets get mislabelled here.
 * Site is a plain global set by NLED_SITE at each analysis entry. */
    uint64_t satd_site_pix[N264_LED_SITE_N];
    uint64_t satd_site_call[N264_LED_SITE_N];
    uint64_t mb_p, mb_p_early, mb_b, mb_b_early;
    uint64_t intra_admit_try, intra_admit_hit;   /* the P/B intra SATD screen */
    /* split, because only the B site has the rate-inclusive reference the
 * units fix touches; the aggregate hides a 3:1 B:P mix */
    uint64_t intra_admit_try_b, intra_admit_hit_b;
    uint64_t mb_p_skip, mb_b_skip;               /* FINAL skip verdict, vs the early one */
    /* The deferred B confirmation, split so a null result can be attributed:
 * b_try = the tolerant probe passed and its answer was held; b_conf = the
 * two ref-0 searches then agreed with the direct MV and it committed. try
 * without conf is probe effort spent for nothing, which is the shape that
 * turns a gate into a net LOSS at the high operating point. */
    uint64_t mb_b_try, mb_b_conf;
} n264_led_t;
extern n264_led_t n264_led;
/* TLS: the lookahead can run on a different thread from MB analysis even at
 * --threads 1, and a shared site index would smear one into the other. */
extern _Thread_local int n264_led_site;
#define NLED(field, n) (n264_led.field += (uint64_t)(n))
#define NLED_SITE(s)   (n264_led_site = (s))
#define NLED_SITE_SAVE(v) int v = n264_led_site
#define NLED_SATD(px) (n264_led.satd_site_pix[n264_led_site] += (px), \
                       n264_led.satd_site_call[n264_led_site]++)
#else
#define NLED(field, n) ((void)0)
#define NLED_SITE(s)   ((void)0)
#define NLED_SITE_SAVE(v) ((void)0)
#define NLED_SATD(px)  ((void)0)
#endif

#endif
