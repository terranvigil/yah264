/*
 * nr.h - coefficient-domain noise reduction (--nr, Y264_NR_INTRA / _INTER).
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A per-coefficient-POSITION offset, learned from the running magnitude of the
 * coefficients this encode has already produced, and taken toward zero off
 * every coefficient before the block is quantised. A position whose
 * coefficients are usually small is a position where the residual is mostly
 * noise, and it is attenuated; a position that is usually loud carries the
 * picture and is left nearly alone. One reciprocal does both, so there is one
 * constant (the strength) and no classification anywhere.
 *
 * Adapted from the sibling HEVC encoder's module of the same name (same
 * project, same authors) to H.264's two transform sizes. The one change the
 * syntax forces is the position weight: H.264's integer transforms are not
 * orthonormal, so a coefficient's raw magnitude carries a per-position gain
 * (the squared norm of its basis row pair, 1 to 6.25 across a 4x4). A noise
 * floor of the same size therefore reads larger at a high-gain position, and
 * the offset is scaled by the gain squared, which makes the attenuation the
 * same fraction of the noise at every position (nr.c, nr_weights).
 *
 * The statistics are kept per CATEGORY: intra/inter x luma/chroma x 4x4/8x8.
 * Chroma 8x8 only exists at 4:4:4, and it rides the chroma group.
 */

#ifndef YAH264_NR_H
#define YAH264_NR_H

#include <stdint.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "../common/bitdepth.h"

#define Y264_NR_CATS   8
#define Y264_NR_GROUP  80                   /* 16 + 64 positions */
#define Y264_NR_SLOTS  (4 * Y264_NR_GROUP)
#define Y264_NR_MAX    2000
/* where a category's running sums are halved: past this the estimate is an
 * average over more content than it is about */
#define Y264_NR_HALVE  ((uint64_t)1 << 20)

static inline int y264_nr_group(int intra, int chroma)
{
    return (intra ? 0 : 2) + (chroma ? 1 : 0);
}

typedef struct y264_nr_tally {
    uint64_t sum[Y264_NR_SLOTS];
    uint32_t cnt[Y264_NR_CATS];
    uint32_t seen;              /* blocks offered; one in four is tallied */
} y264_nr_tally;

typedef struct y264_nr {
    uint64_t sum[Y264_NR_SLOTS];
    uint64_t cnt[Y264_NR_CATS];
    uint16_t off[Y264_NR_SLOTS];
} y264_nr;

void y264_nr_fold(y264_nr *nr, const y264_nr_tally *t);
/* re-solve every offset; a zero strength or an empty category gives zeros,
 * which is exactly the quantiser without this feature */
void y264_nr_offsets(y264_nr *nr, int intra, int inter);

/* Spatial noise reading of a luma plane, Q8 in 8-bit levels (nr.c). */
int y264_nr_noise_q8(const pixel *p, int stride, int w, int h);

/* The subtraction alone: branch-free and restrict-qualified so it
 * vectorises; it runs on every block every RD trial quantises. */
static inline void y264_nr_sub(dctcoef *restrict coef,
                               const uint16_t *restrict o, int i0, int n)
{
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    /* |v| saturating-minus the offset IS max(|v| - o, 0); the sign goes back
     * on by a select. Bit-exact with the loop below. */
    dctcoef c0 = coef[0];
    for (int i = 0; i < n; i += 8) {
        int16x8_t v = vld1q_s16(coef + i);
        uint16x8_t a = vqsubq_u16(vreinterpretq_u16_s16(vabsq_s16(v)), vld1q_u16(o + i));
        int16x8_t p = vreinterpretq_s16_u16(a);
        vst1q_s16(coef + i, vbslq_s16(vcltzq_s16(v), vnegq_s16(p), p));
    }
    if (i0)
        coef[0] = c0;
    return;
#endif
    for (int i = i0; i < n; i++) {
        int v = coef[i];
        int s = v >> 31;                        /* 0 or -1 */
        int a = ((v ^ s) - s) - (int)o[i];
        a &= ~(a >> 31);                        /* max(a, 0) */
        coef[i] = (dctcoef)((a ^ s) - s);
    }
}

/* Attenuate one block IN PLACE (raster order, n = 16 or 64) and, when `t` is
 * set, tally it first: the estimator has to describe the content, not the
 * picture the previous estimate already made. `ac` skips position 0 (an
 * Intra16x16 or chroma AC block, whose DC is coded elsewhere). */
static inline void y264_nr_block(const uint16_t *off, y264_nr_tally *t,
                                 dctcoef *coef, int n, int intra, int chroma,
                                 int ac)
{
    int g = y264_nr_group(intra, chroma);
    int base = g * Y264_NR_GROUP + (n == 64 ? 16 : 0);
    const uint16_t *o = off + base;
    int i0 = ac ? 1 : 0;
    /* A quarter of the blocks describe the population as well as all of them
     * do, for a quarter of the cost; the choice is a counter, so it is the
     * same blocks on every run at one thread. */
    if (t && !(t->seen++ & 3)) {
        uint64_t *s = t->sum + base;
        for (int i = i0; i < n; i++) {
            int v = coef[i];
            s[i] += (uint64_t)(v < 0 ? -v : v);
        }
        t->cnt[g * 2 + (n == 64)]++;
    }
    y264_nr_sub(coef, o, i0, n);
}

#endif /* YAH264_NR_H */
