/*
 * mcstf.h - the motion-compensated temporal pre-filter (--mcstf).
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A picture's own noise is uncorrelated with its neighbours' and its content
 * is not, so the average of a few motion-compensated neighbours is the same
 * picture with less noise in it, and a picture with less noise costs fewer
 * bits at the same apparent quality. That is a claim about NOISY sources
 * only: on clean content the average has nothing to remove and can only blur.
 *
 * Ported from yah265's filter (our own HEVC encoder, same authors),
 * and it keeps that one's shape: three weights keep the average from
 * blurring -- a neighbour counts less the further away in time it is, less
 * where its block did not match, and less at any pixel where it disagrees
 * with the original by more than noise explains -- and a noise reading gates
 * clean pictures out entirely.
 *
 * WHERE IT SITS: a queue in front of the lookahead. The filter needs the
 * frames on both sides of the one it filters, and the lookahead has built its
 * lowres plane and costs by the time a later frame arrives, so pictures go in
 * here and filtered ones come out `range` pictures later. Everything
 * downstream -- lowres costs, scene cuts, mb-tree, mode decision, the coded
 * samples -- sees one picture, the filtered one. Driver-serial, integer and
 * in display order, so the filtered picture is a pure function of the source
 * at every thread count.
 */

#ifndef YAH264_MCSTF_H
#define YAH264_MCSTF_H

#include <stdint.h>

#include "../common/bitdepth.h"

#define Y264_MCSTF_MAX_RANGE 4

typedef struct y264_mcstf y264_mcstf;

/* The configuration, resolved from the environment on the opening thread.
 * period, sigma_q8 and clean_q8 are fixed there; tests set their own. */
typedef struct y264_mcstf_cfg {
    int range;          /* neighbours per side, 1..4 */
    int period;         /* filter pictures whose display index % period == 0 */
    int sigma_q8;       /* photometric width, 8-bit levels, Q8 */
    int clean_q8;       /* noise reading below which a picture is left alone */
} y264_mcstf_cfg;

/* Fill `c` from the Y264_MCSTF and Y264_MCSTF_RANGE environment, which the
 * CLI's --mcstf and --mcstf-range set. Returns nonzero when the filter is
 * armed. */
int y264_mcstf_cfg_env(y264_mcstf_cfg *c);

/* `sx`/`sy` are the chroma shifts (1/1 at 4:2:0). NULL on allocation
 * failure. */
y264_mcstf *y264_mcstf_open(const y264_mcstf_cfg *c, int w, int h, int sx,
                            int sy);
void y264_mcstf_close(y264_mcstf *m);

/* Copy one source picture in; the caller's buffer is free on return. */
void y264_mcstf_push(y264_mcstf *m, const pixel *const plane[3],
                     const int stride[3], int64_t pts);
/* Whether a filtered picture can be taken out; with `flush`, drain the tail
 * against the neighbours that exist. */
int y264_mcstf_ready(const y264_mcstf *m, int flush);
/* Take it out: its displayed area, valid until the next pop. `filtered`
 * says whether the filter ran on it. */
void y264_mcstf_pop(y264_mcstf *m, const pixel *plane[3], int stride[3],
                    int64_t *pts, int *filtered);

#endif /* YAH264_MCSTF_H */
