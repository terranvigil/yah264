/*
 * me.h - motion estimation
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef YAH264_ME_H
#define YAH264_ME_H

#include <stdint.h>
#include "../common/bitdepth.h"

/* Search for the best quarter-pel motion vector for a `w`x`h` luma block at
 * source position (bx,by), against reference plane `ref` (pw x ph). `pmvx/pmvy`
 * is the predicted MV, used as a search start and to bias the cost toward
 * cheap-to-code vectors. `seeds` is `nseeds` extra {x,y} quarter-pel candidates
 * (e.g. spatial-neighbour MVs) probed as additional search starts; pass NULL/0
 * for none. Writes the winning MV to the mvx and mvy outputs and returns its
 * SAD-based cost. */
int y264_me_search(const pixel *src, int sstride,
                   const pixel *ref, int rstride, int pw, int ph,
                   int bx, int by, int w, int h,
                   int pmvx, int pmvy, int lambda,
                   const int *seeds, int nseeds,
                   int *mvx, int *mvy);

/* Set the analysis effort (subme) for motion search; call once per encode. */
void y264_me_set_subme(int subme);

/* Set the ME method (x264-style --me), decoupled from the preset: 0 = auto
 * (follow the subme gate: hex at subme<8, UMH at >=8), 1 = dia, 2 = hex,
 * 3 = umh. Call once per encode before any worker runs ME. */
void y264_me_set_method(int method);

/* Whether the hex-parity compensation features (rich MV seeds, behaviour-matched
 * lowres MV field, terminal square refine, round-to-nearest fpel align) are
 * active for the current method/subme/env. Single source of truth shared by the
 * search (me.c), the seed assembly (macroblock.c) and the lowres field
 * (encoder.c) so they switch together. */
int y264_me_hex_features(void);

/* Set the subpel refinement pattern (-1 = auto/square). Presets drive this; the
 * Y264_SUBPEL env overrides it. Call once per encode before any worker runs ME. */
void y264_me_set_subpel(int subpel);

/* Lowres oracle for the escalation gates (docs/adaptive-me-design.md). Set
 * before a 16x16 ref0 search with the per-MB lookahead prior (lr_inter cost +
 * lr_seed MV, full-res qpel); pass valid=0 to clear for searches where it
 * doesn't map. The search only records distributions from it
 * (byte-identical). Thread-local. */
void y264_me_set_oracle(int valid, long cost, int mvx, int mvy);

/* Content-adaptive ME frame flag: 1 = this frame's searches run
 * cheap (no UMH, capped-diamond subpel). Thread-local; the MB analysis entry
 * points stamp it from f->me_cheap. */
void y264_me_set_cheap(int on);

/* Reset the x264-style half-pel qpel-skip threshold. Call once at the start of
 * each MB's inter analysis (the threshold accumulates the best candidate cost
 * across that MB's partition x ref searches). TLS; deterministic under the
 * wavefront. */
void y264_me_reset_hpel_thresh(void);
void y264_me_set_isb(int b);   /* oracle attribution: current MB is in a B frame */
void y264_me_set_et_off(int off); /* importance rescue: suppress ME_ET for this MB */
void y264_me_set_et_class(int c); /* ME_ET frame class: 1=P, 2=ref B, 4=nonref B */
void y264_me_set_stq(int q);      /* single-thread quality: ME_ET disengages */
void y264_me_set_list(int l);  /* reference list of the next search (the half-pel threshold is per list) */

/* Staircase list-1 vertical MV cap, in quarter-pel, RELATIVE to
 * the searched block's macroblock: a B row r clamped to this can only read
 * anchor luma rows <= 16(r+LAG) - 5 (block bottom 16 + max MV + 6-tap margin
 * 3), which the row gate guarantees are consumable (final rows >= 16(r+LAG)+13
 * when it releases).
 *
 * Y264_STAIR_LAG below is the FLOOR, not the value in force: the value used at
 * runtime is yah264_encoder_t.stair_lag, computed once per encoder_open by
 * stair_lag_for (encoder.c) as a function of frame height and pool width --
 * the same device x264 uses to bound its inter-thread MV range -- and never allowed below
 * this floor. The soundness argument for ANY lag >= this floor is a closed
 * form, not a re-measurement: the row-gate's producer-side publish bound
 * (16(r+LAG)+13 luma / +10 hpel / 8(r+LAG)+6 chroma, stair_trailer_task) and
 * this macro's reader-side touch bound both scale by exactly 16 per unit of
 * LAG, so the slack between them (19/16/10 rows) is a LAG-independent
 * constant -- raising the runtime value can never erode it. Lowering the
 * FLOOR itself is the thing that needs new arithmetic, which is exactly what
 * this constant still guards.
 *
 * The #ifndef is a HEADROOM PROBE hook only (-DY264_STAIR_LAG=k in a scratch
 * build dir): the margin above leaves 2 rows of rec / 1 of hpel slack at LAG 4,
 * so any k < 4 reads unwritten rows and its OUTPUT IS UNSOUND. It exists to
 * price a shorter chain, which measured 1.01x on CIF. Never ship k != 4 without
 * re-deriving the margin arithmetic (that includes never lowering the runtime
 * floor below it). */
#ifndef Y264_STAIR_LAG
#define Y264_STAIR_LAG      4
#endif
#define Y264_STAIR_MVY_MAX  (4 * (16 * Y264_STAIR_LAG - 24))   /* +40 px, qpel;
 * the FLOOR value */

/* Set/clear the vertical qpel MV cap for the NEXT y264_me_search calls on this
 * thread (INT_MAX = uncapped, the default; capped probes are skipped, capped
 * starts clamped). Thread-local, like the other per-search state. */
void y264_me_set_ymax(int ymax_qpel);
/* Level MV limits in qpel (|mv| < lim), 0 = none; set per worker with the hpel planes. */
void y264_me_set_mvlim(int xlim_qpel, int ylim_qpel);

/* Dump the ME statistics (Y264_ME_STATS) to stderr; no-op when unset. Call
 * once at encoder close, on the main thread. */
void y264_me_stats_dump(void);

/* One reference's precomputed half-pel planes (see y264_mc_build_hpel). `ref` is
 * the integer reference plane pointer ME is called with; h/v/c share its stride
 * and interior origin. */
typedef struct {
    const pixel *ref;
    const pixel *h, *v, *c;
} y264_hpel_ref_t;

/* Register the half-pel planes for the references ME will search this slice.
 * Thread-local: call once per slice (on the encoding thread) before the ME
 * calls. When a search's reference matches a registered entry (and the block is
 * in bounds), sub-pel probes read the planes instead of interpolating. Pass n=0
 * to disable (ME falls back to on-the-fly interpolation, bit-identical). */
void y264_me_set_hpel(const y264_hpel_ref_t *refs, int n, int stride);

/* Resolve the motion search's env-gated lazy statics on the calling thread (see
 * y264_mb_warm_statics). Idempotent. */
void y264_me_warm_statics(void);

/* Build the premultiplied MV-cost table for one ME lambda (idempotent,
 * CAS-published, safe from any thread). y264_mb_warm_statics primes every value
 * lambda_me can produce; an unprimed lambda keeps the exact multiply path. */
void y264_me_prime_lambda(int lambda);

/* --- lazy-hpel census. Dead unless
 * Y264_HPEL_CENSUS=<band rows> is set; single-threaded measurement only.
 * The encoder calls _built once per half-pel plane build (rows in plane space,
 * i.e. -border .. padded_h+border), which flushes and reports what the previous
 * build of that same plane was actually read for. ME marks the rows it reads.
 * Report goes to stderr at process exit. */
int  y264_hpel_census_on(void);
void y264_hpel_census_built(const void *h, int y0, int y1, int stride);

/* Plane-read luma MC into a stride-16 pred block, byte-identical to y264_mc_luma,
 * reading the registered half-pel planes (y264_me_set_hpel) when in bounds. Used by
 * inter-prediction RD candidates to avoid re-running the 6-tap filter each time. */
void y264_me_mc_luma(pixel *pred, const pixel *ref, int rs, int pw, int ph,
                     int bx, int by, int mvx, int mvy, int w, int h);

#endif /* YAH264_ME_H */
