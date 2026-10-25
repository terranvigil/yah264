/*
 * mcstf.c - the motion-compensated temporal pre-filter. mcstf.h says what it
 * is and where it sits; this file is the queue, the motion search and the
 * weighted average.
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <math.h>
#include "../common/env.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcstf.h"
#include "../dsp/metrics.h"

#define BLK     16      /* motion block, full resolution */
#define PAD     48      /* covers the search's furthest reach plus a block */
#define SUB     4       /* coarse stage subsampling */
#define COARSE  8       /* coarse radius, in coarse steps (+-32 samples) */
#define FINE    4       /* full-resolution refinement radius */
#define GTAB    512

/* ---- configuration -------------------------------------------------- */

static double env_f(const char *s, double def, double lo, double hi)
{
    double d = s ? atof(s) : def;
    return d < lo ? lo : d > hi ? hi : d;
}

int y264_mcstf_cfg_env(y264_mcstf_cfg *c)
{
    memset(c, 0, sizeof *c);
    /* 1: at two neighbours a side the 120-frame band read median -0.85 with
     * foreman_cif +1.25; at one, -0.37 with nothing worse than +0.00 at 60
     * frames, for half the search (docs/denoise.md) */
    c->range = (int)env_f(y264_env("Y264_MCSTF_RANGE"), 1, 1, Y264_MCSTF_MAX_RANGE);
    /* 4 = the anchor spacing at the default --bframes 3: the pictures a
     * mini-GOP predicts from are the ones whose noise propagates */
    c->period = (int)env_f(y264_env("Y264_MCSTF_PERIOD"), 4, 1, 64);
    /* 6 levels: the grain the corpus carries sits at 1-4.5 levels of
     * standard deviation, so six covers it with margin, and at three widths
     * a disagreement the search got wrong is worth a hundredth of a weight */
    c->sigma_q8 = (int)(env_f(y264_env("Y264_MCSTF_SIGMA"), 6.0, 0.25, 64.0) * 256.0 + 0.5);
    /* The bar on the TEMPORAL noise reading (mc_tnoise), in 8-bit levels of
     * match error. The rendered animations read 0.00-0.25 and are left
     * alone; every natural clip in the corpus reads above 0.75. The sibling
     * encoder gates a spatial reading at 1.5 instead, and that reading cannot
     * separate the film-grain titles (1.38-1.44 at 720p, and a pillarboxed
     * 1924 scan at 0.50) from foreman_cif (1.19-1.44), which is why the
     * reading here is temporal. */
    c->clean_q8 = (int)(env_f(y264_env("Y264_MCSTF_CLEAN"), 0.5, 0.0, 64.0) * 256.0 + 0.5);
    c->adapt = (int)env_f(y264_env("Y264_MCSTF_ADAPT"), 0, 0, 1);
    /* the error, in quarters of the noise's own match error, up to which a
     * block keeps its full weight under Y264_MCSTF_ADAPT */
    c->adapt_k = (int)env_f(y264_env("Y264_MCSTF_ADAPT_K"), 5, 4, 64);
    c->subpel = (int)env_f(y264_env("Y264_MCSTF_SUBPEL"), 0, 0, 1);
    c->cut = (int)env_f(y264_env("Y264_MCSTF_CUT"), 0, 0, 1);
    c->qadapt = (int)env_f(y264_env("Y264_MCSTF_QADAPT"), 0, 0, 1);
    c->qp_off = (int)env_f(y264_env("Y264_MCSTF_QP"), 0, -6, 6);
    c->trace = (int)env_f(y264_env("Y264_MCSTF_TRACE"), 0, 0, 1);
    return (int)env_f(y264_env("Y264_MCSTF"), 0, 0, 1);
}

/* ---- the queue ------------------------------------------------------ */

typedef struct mc_pic {
    pixel *buf[3];
    pixel *org[3];
    int stride[3];
    pixel *coarse;
    int cstride;
    int display_idx;
    int64_t pts;
} mc_pic;

struct y264_mcstf {
    y264_mcstf_cfg c;
    int w, h, sx, sy;
    int cw, ch;
    int cap;
    mc_pic *p;          /* sliding window, display order p[0..n-1] */
    int n, next_idx, out_pos;
    pixel *out_buf[3];
    pixel *out[3];
    int out_stride[3];
    int bw, bh;
    /* per (neighbour slot, block): motion in quarter samples, the match
     * error (mean absolute, Q4), the combined weight (Q8) and the inverse of
     * the block's relative photometric width (Q8, 256 = the full width) */
    int16_t *mv;
    uint16_t *berr, *bwt, *binv;
    int sigma_eff;      /* coded-depth units, Q8 */
    uint16_t gauss[GTAB];
    int last_noise_q8;
};

/* temporal strength by distance, Q8; the original counts 256 */
static const int mc_tstr[Y264_MCSTF_MAX_RANGE + 1] = { 256, 218, 154, 115, 90 };

static int plane_w(const y264_mcstf *m, int c) { return c ? m->w >> m->sx : m->w; }
static int plane_h(const y264_mcstf *m, int c) { return c ? m->h >> m->sy : m->h; }

static void pad_plane(pixel *org, int stride, int w, int h, int pad)
{
    for (int y = 0; y < h; y++) {
        pixel *r = org + (size_t)y * stride;
        for (int x = 1; x <= pad; x++) {
            r[-x] = r[0];
            r[w - 1 + x] = r[w - 1];
        }
    }
    for (int y = 1; y <= pad; y++) {
        memcpy(org - (ptrdiff_t)y * stride - pad, org - pad,
               (size_t)(w + 2 * pad) * sizeof(pixel));
        memcpy(org + (size_t)(h - 1 + y) * stride - pad,
               org + (size_t)(h - 1) * stride - pad,
               (size_t)(w + 2 * pad) * sizeof(pixel));
    }
}

void y264_mcstf_close(y264_mcstf *m)
{
    if (!m)
        return;
    for (int i = 0; m->p && i < m->cap; i++) {
        for (int c = 0; c < 3; c++)
            free(m->p[i].buf[c]);
        free(m->p[i].coarse);
    }
    free(m->p);
    for (int c = 0; c < 3; c++)
        free(m->out_buf[c]);
    free(m->mv);
    free(m->berr);
    free(m->bwt);
    free(m->binv);
    free(m);
}

y264_mcstf *y264_mcstf_open(const y264_mcstf_cfg *cfg, int w, int h, int sx,
                            int sy, double rf)
{
    y264_dsp_init();
    y264_mcstf *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->c = *cfg;
    m->w = w; m->h = h; m->sx = sx; m->sy = sy;
    m->cap = 2 * cfg->range + 1;
    m->cw = (w + SUB - 1) / SUB;
    m->ch = (h + SUB - 1) / SUB;
    m->bw = (w + BLK - 1) / BLK;
    m->bh = (h + BLK - 1) / BLK;
    size_t nb = (size_t)m->bw * m->bh * m->cap;
    m->p = calloc((size_t)m->cap, sizeof(*m->p));
    m->mv = calloc(nb * 2, sizeof(*m->mv));
    m->berr = calloc(nb, sizeof(*m->berr));
    m->bwt = calloc(nb, sizeof(*m->bwt));
    m->binv = calloc(nb, sizeof(*m->binv));
    if (!m->p || !m->mv || !m->berr || !m->bwt || !m->binv)
        goto fail;
    for (int i = 0; i < m->cap; i++) {
        for (int c = 0; c < 3; c++) {
            int st = plane_w(m, c) + 2 * PAD;
            m->p[i].stride[c] = st;
            m->p[i].buf[c] = malloc((size_t)st * (plane_h(m, c) + 2 * PAD) * sizeof(pixel));
            if (!m->p[i].buf[c])
                goto fail;
            m->p[i].org[c] = m->p[i].buf[c] + (size_t)PAD * st + PAD;
        }
        m->p[i].cstride = m->cw + 2 * PAD;
        m->p[i].coarse = malloc((size_t)m->p[i].cstride * (m->ch + 2 * PAD) * sizeof(pixel));
        if (!m->p[i].coarse)
            goto fail;
    }
    for (int c = 0; c < 3; c++) {
        m->out_stride[c] = plane_w(m, c) + 2 * PAD;
        m->out_buf[c] = malloc((size_t)m->out_stride[c] * (plane_h(m, c) + 2 * PAD) * sizeof(pixel));
        if (!m->out_buf[c])
            goto fail;
        m->out[c] = m->out_buf[c] + (size_t)PAD * m->out_stride[c] + PAD;
    }
    /* Y264_MCSTF_QADAPT: the width follows the rate factor, doubling every
     * twelve QP. A fine quantiser keeps most of the noise anyway and the
     * filter should keep its hands off detail there; a coarse one throws the
     * noise away at a cost, and averaging it out first is cheaper. */
    double qs = 1.0;
    if (cfg->qadapt) {
        qs = pow(2.0, (rf - 26.0) / 12.0);
        if (qs < 0.6) qs = 0.6;
        if (qs > 1.8) qs = 1.8;
    }
    m->sigma_eff = (int)(cfg->sigma_q8 * qs * (1 << (Y264_BIT_DEPTH - 8)) + 0.5);
    /* 256 * exp(-d^2 / 2 sigma^2), tabulated once over |d| in coded units */
    double sg = m->sigma_eff / 256.0, k = -1.0 / (2.0 * sg * sg);
    for (int d = 0; d < GTAB; d++)
        m->gauss[d] = (uint16_t)(256.0 * exp(k * (double)d * d) + 0.5);
    return m;
fail:
    y264_mcstf_close(m);
    return NULL;
}

static void build_coarse(y264_mcstf *m, mc_pic *p)
{
    pixel *dst = p->coarse + (size_t)PAD * p->cstride + PAD;
    for (int y = 0; y < m->ch; y++)
        for (int x = 0; x < m->cw; x++) {
            unsigned acc = 0;
            for (int j = 0; j < SUB; j++) {
                int yy = y * SUB + j < m->h ? y * SUB + j : m->h - 1;
                const pixel *r = p->org[0] + (size_t)yy * p->stride[0];
                for (int i = 0; i < SUB; i++) {
                    int xx = x * SUB + i < m->w ? x * SUB + i : m->w - 1;
                    acc += r[xx];
                }
            }
            dst[(size_t)y * p->cstride + x] = (pixel)((acc + SUB * SUB / 2) / (SUB * SUB));
        }
    pad_plane(dst, p->cstride, m->cw, m->ch, PAD);
}

void y264_mcstf_push(y264_mcstf *m, const pixel *const plane[3],
                     const int stride[3], int64_t pts)
{
    if (m->n == m->cap) {
        mc_pic gone = m->p[0];
        memmove(m->p, m->p + 1, (size_t)(m->cap - 1) * sizeof(*m->p));
        m->p[m->cap - 1] = gone;
        m->n--;
        m->out_pos--;
    }
    mc_pic *p = &m->p[m->n];
    for (int c = 0; c < 3; c++) {
        int pw = plane_w(m, c), ph = plane_h(m, c);
        for (int y = 0; y < ph; y++)
            memcpy(p->org[c] + (size_t)y * p->stride[c],
                   plane[c] + (size_t)y * stride[c], (size_t)pw * sizeof(pixel));
        pad_plane(p->org[c], p->stride[c], pw, ph, PAD);
    }
    p->display_idx = m->next_idx++;
    p->pts = pts;
    build_coarse(m, p);
    m->n++;
}

int y264_mcstf_ready(const y264_mcstf *m, int flush)
{
    if (m->out_pos >= m->n)
        return 0;
    return flush || m->out_pos + m->c.range < m->n;
}

static unsigned sad_wh(const pixel *a, int as, const pixel *b, int bs, int w, int h)
{
    unsigned s = 0;
    for (int y = 0; y < h; y++, a += as, b += bs)
        for (int x = 0; x < w; x++) {
            int d = (int)a[x] - (int)b[x];
            s += (unsigned)(d < 0 ? -d : d);
        }
    return s;
}

/* the coarse stage's 4x4, inlined: 289 of them per block per neighbour, and
 * a call apiece cost more than the arithmetic */
static inline unsigned sad4x4(const pixel *a, int as, const pixel *b, int bs)
{
    unsigned s = 0;
    for (int y = 0; y < 4; y++, a += as, b += bs)
        for (int x = 0; x < 4; x++) {
            int d = (int)a[x] - (int)b[x];
            s += (unsigned)(d < 0 ? -d : d);
        }
    return s;
}

/* bilinear sample at integer (xi, yi) plus fraction (fx, fy) / den */
static inline int bilin(const pixel *p, int st, int fx, int fy, int den)
{
    if (!(fx | fy))
        return p[0];
    int a = p[0], b = p[1], c = p[st], d = p[st + 1];
    int v = a * (den - fx) * (den - fy) + b * fx * (den - fy) +
            c * (den - fx) * fy + d * fx * fy;
    int dd = den * den;
    return (v + dd / 2) / dd;
}

/* SAD of the block against the reference at a quarter-sample vector */
static unsigned sad_q(const mc_pic *cur, const mc_pic *ref, int x0, int y0,
                      int w, int h, int mvx, int mvy)
{
    int st = ref->stride[0], cs = cur->stride[0];
    const pixel *co = cur->org[0] + (size_t)y0 * cs + x0;
    const pixel *ro = ref->org[0] + (ptrdiff_t)(y0 + (mvy >> 2)) * st + x0 + (mvx >> 2);
    int fx = mvx & 3, fy = mvy & 3;
    if (!(fx | fy))
        return sad_wh(co, cs, ro, st, w, h);
    unsigned s = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = (int)co[(size_t)y * cs + x] - bilin(ro + (ptrdiff_t)y * st + x, st, fx, fy, 4);
            s += (unsigned)(d < 0 ? -d : d);
        }
    return s;
}

#define SHORTER(dx, dy, bx, by) ((dx) * (dx) + (dy) * (dy) < (bx) * (bx) + (by) * (by))

/* One block against one neighbour: coarse search on the subsampled luma,
 * then integer refinement at full resolution. Ties go
 * strictly to the shorter vector, so a still picture filters against itself
 * undisplaced and the answer depends on the pictures, not the scan order.
 * Returns the vector in quarter samples and the mean absolute error, Q4. */
static void mc_search(const y264_mcstf *m, const mc_pic *cur, const mc_pic *ref,
                      int bx, int by, int *mvx, int *mvy, unsigned *err)
{
    int x0 = bx * BLK, y0 = by * BLK;
    int w = BLK < m->w - x0 ? BLK : m->w - x0;
    int h = BLK < m->h - y0 ? BLK : m->h - y0;
    int cx = x0 / SUB, cy = y0 / SUB;
    /* the dispatched kernels: bit-exact with the plain loop, so the filter
     * is the same picture on every tier */
    y264_sad_fn sad16 = y264_dsp.sad[Y264_PU_16x16];
    int full = w == 16 && h == 16;
    const pixel *cc = cur->coarse + (size_t)(PAD + cy) * cur->cstride + PAD + cx;
    const pixel *rc = ref->coarse + (size_t)(PAD + cy) * ref->cstride + PAD + cx;
    unsigned best = ~0u;
    int bdx = 0, bdy = 0;
    for (int dy = -COARSE; dy <= COARSE; dy++)
        for (int dx = -COARSE; dx <= COARSE; dx++) {
            unsigned s = sad4x4(cc, cur->cstride, rc + (ptrdiff_t)dy * ref->cstride + dx,
                                ref->cstride);
            if (s < best || (s == best && SHORTER(dx, dy, bdx, bdy))) {
                best = s; bdx = dx; bdy = dy;
            }
        }
    bdx *= SUB;
    bdy *= SUB;
    const pixel *co = cur->org[0] + (size_t)y0 * cur->stride[0] + x0;
    const pixel *ro = ref->org[0] + (size_t)y0 * ref->stride[0] + x0;
    best = ~0u;
    int fdx = bdx, fdy = bdy;
    for (int dy = bdy - FINE; dy <= bdy + FINE; dy++)
        for (int dx = bdx - FINE; dx <= bdx + FINE; dx++) {
            unsigned s = full
                ? (unsigned)sad16(co, cur->stride[0], ro + (ptrdiff_t)dy * ref->stride[0] + dx, ref->stride[0])
                : sad_wh(co, cur->stride[0], ro + (ptrdiff_t)dy * ref->stride[0] + dx,
                         ref->stride[0], w, h);
            if (s < best || (s == best && SHORTER(dx, dy, fdx, fdy))) {
                best = s; fdx = dx; fdy = dy;
            }
        }
    unsigned area = (unsigned)(w * h);
    *mvx = fdx * 4;
    *mvy = fdy * 4;
    *err = (best * 16u + area / 2) / area;
}

/* Y264_MCSTF_SUBPEL: a half- then a quarter-sample step around the integer
 * winner, taken only where the caller found the integer match poor. */
static void mc_subpel(const y264_mcstf *m, const mc_pic *cur, const mc_pic *ref,
                      int bx, int by, int *mvx, int *mvy, unsigned *err)
{
    int x0 = bx * BLK, y0 = by * BLK;
    int w = BLK < m->w - x0 ? BLK : m->w - x0;
    int h = BLK < m->h - y0 ? BLK : m->h - y0;
    unsigned area = (unsigned)(w * h);
    int qx = *mvx, qy = *mvy;
    unsigned best = sad_q(cur, ref, x0, y0, w, h, qx, qy);
    for (int step = 2; step >= 1; step >>= 1) {
        int cx0 = qx, cy0 = qy;
        for (int dy = -step; dy <= step; dy += step)
            for (int dx = -step; dx <= step; dx += step) {
                if (!(dx | dy))
                    continue;
                unsigned s = sad_q(cur, ref, x0, y0, w, h, cx0 + dx, cy0 + dy);
                if (s < best) {
                    best = s; qx = cx0 + dx; qy = cy0 + dy;
                }
            }
    }
    *mvx = qx;
    *mvy = qy;
    *err = (best * 16u + area / 2) / area;
}

/* the block-error weight carried over from yah265, Q8: half at 6 levels of mean absolute
 * error, never zero -- a badly matched block still has pixels that matched,
 * and the photometric weight decides those */
static int berr_w_fixed(unsigned err_q4)
{
    unsigned half = (6u << 4) << (Y264_BIT_DEPTH - 8);
    return (int)((256u * half + (half + err_q4) / 2) / (half + err_q4));
}

/* The temporal noise reading: the 25th percentile of a neighbour's block
 * match errors, over blocks that are neither near black nor near white.
 * Where the match is right the error IS the noise -- two independent draws of
 * deviation s differ by a mean absolute 1.13 s -- whatever the noise's
 * spatial grain, which is what a spatial high-pass cannot see: a film scan's
 * grain spans several samples and reads almost clean to a Laplacian, and
 * changes completely from one picture to the next. Q4, coded units. */
static unsigned mc_tnoise(const y264_mcstf *m, const mc_pic *cur, const uint16_t *be)
{
    enum { NBIN = 1024 };
    static const int lo = 24, hi = 232;
    int hist[NBIN];
    int n = 0;
    memset(hist, 0, sizeof hist);
    for (int by = 0; by < m->bh; by++)
        for (int bx = 0; bx < m->bw; bx++) {
            int x0 = bx * BLK, y0 = by * BLK;
            int w = BLK < m->w - x0 ? BLK : m->w - x0;
            int h = BLK < m->h - y0 ? BLK : m->h - y0;
            unsigned sum = 0;
            for (int y = 0; y < h; y += 2)
                for (int x = 0; x < w; x += 2)
                    sum += cur->org[0][(size_t)(y0 + y) * cur->stride[0] + x0 + x];
            unsigned mean = (sum / (unsigned)(((w + 1) / 2) * ((h + 1) / 2))) >> (Y264_BIT_DEPTH - 8);
            if (mean < lo || mean > hi)
                continue;
            unsigned e = be[(size_t)by * m->bw + bx];
            hist[e < NBIN - 1 ? e : NBIN - 1]++;
            n++;
        }
    if (n * 8 < m->bw * m->bh)
        return 0;
    int want = n / 4, run = 0;
    for (int i = 0; i < NBIN; i++) {
        run += hist[i];
        if (run > want)
            return (unsigned)i;
    }
    return NBIN - 1;
}

/* Filter picture `ci` of the window into m->out; 0 when the picture reads
 * too clean to filter and m->out was not written. */
static int mc_filter(y264_mcstf *m, int ci)
{
    const mc_pic *cur = &m->p[ci];
    const size_t nblk = (size_t)m->bw * m->bh;
    const int R = m->c.range;
    int nb = 0;
    int nbi[2 * Y264_MCSTF_MAX_RANGE], nbd[2 * Y264_MCSTF_MAX_RANGE];
    int nbs[2 * Y264_MCSTF_MAX_RANGE];
    /* search slot of (side, distance): past 0..R-1, future R..2R-1 */
#define SLOT(side, d) ((side) < 0 ? (d) - 1 : R + (d) - 1)
    uint64_t emean[2 * Y264_MCSTF_MAX_RANGE];
    int searched[2 * Y264_MCSTF_MAX_RANGE] = { 0 };

    /* Every neighbour's integer search. The farther ones are needed unless a
     * cut stops a side, and that needs the noise reading first, so the two
     * nearest go first. */
    for (int pass = 0; pass < 2; pass++)
        for (int side = -1; side <= 1; side += 2)
            for (int d = pass ? 2 : 1; d <= (pass ? R : 1); d++) {
                int j = ci + side * d;
                if (j < 0 || j >= m->n)
                    continue;
                int sl = SLOT(side, d);
                int16_t *mv = m->mv + (size_t)sl * nblk * 2;
                uint16_t *be = m->berr + (size_t)sl * nblk;
                uint64_t esum = 0;
                for (int by = 0; by < m->bh; by++)
                    for (int bx = 0; bx < m->bw; bx++) {
                        int vx, vy;
                        unsigned e;
                        mc_search(m, cur, &m->p[j], bx, by, &vx, &vy, &e);
                        size_t bi = (size_t)by * m->bw + bx;
                        mv[bi * 2] = (int16_t)vx;
                        mv[bi * 2 + 1] = (int16_t)vy;
                        be[bi] = (uint16_t)(e > 65535u ? 65535u : e);
                        esum += e;
                    }
                emean[sl] = esum / nblk;
                searched[sl] = 1;
            }

    /* The noise: the quieter of the two nearest neighbours' readings, since
     * a cut sits on at most one side of a picture. */
    unsigned tn = ~0u;
    for (int side = -1; side <= 1; side += 2) {
        int sl = SLOT(side, 1);
        if (!searched[sl])
            continue;
        unsigned t = mc_tnoise(m, cur, m->berr + (size_t)sl * nblk);
        if (t < tn) tn = t;
    }
    if (tn == ~0u)
        tn = 0;
    m->last_noise_q8 = (int)((tn << 4) >> (Y264_BIT_DEPTH - 8));   /* Q8 levels */
    /* the gate reads the same levels the knob is written in */
    if ((int)((tn * 16u) >> (Y264_BIT_DEPTH - 8)) < m->c.clean_q8) {
        if (m->c.trace)
            fprintf(stderr, "mcstf   disp=%d tnoise=%.2f gated\n", cur->display_idx,
                    ((tn * 16u) >> (Y264_BIT_DEPTH - 8)) / 256.0);
        return 0;
    }
    /* e0: the error a right match shows at this noise, never under a level */
    unsigned lvl = 16u << (Y264_BIT_DEPTH - 8);
    unsigned e0 = tn > lvl ? tn : lvl;
    unsigned subpel_q4 = 0;
    if (m->c.subpel)
        subpel_q4 = m->c.adapt ? 2 * e0 : (6u * 16u) << (Y264_BIT_DEPTH - 8);
    /* a neighbour across a cut matches nowhere: its mean error sits far past
     * the noise's and past any ordinary motion miss */
    unsigned cut_q4 = 3 * e0 + ((12u * 16u) << (Y264_BIT_DEPTH - 8));

    for (int side = -1; side <= 1; side += 2)
        for (int d = 1; d <= R; d++) {
            int sl = SLOT(side, d);
            if (!searched[sl])
                break;
            int iscut = m->c.cut && emean[sl] > cut_q4;
            if (m->c.trace)
                fprintf(stderr, "mcstf   nb disp=%d d=%+d emean=%.2f e0=%.2f%s\n",
                        cur->display_idx, side * d, emean[sl] / 16.0, e0 / 16.0,
                        iscut ? " CUT" : "");
            if (iscut)
                break;          /* this one and everything past it on this side */
            if (subpel_q4) {
                int16_t *mv = m->mv + (size_t)sl * nblk * 2;
                uint16_t *be = m->berr + (size_t)sl * nblk;
                const mc_pic *ref = &m->p[ci + side * d];
                for (int by = 0; by < m->bh; by++)
                    for (int bx = 0; bx < m->bw; bx++) {
                        size_t bi = (size_t)by * m->bw + bx;
                        if (be[bi] <= subpel_q4)
                            continue;
                        int vx = mv[bi * 2], vy = mv[bi * 2 + 1];
                        unsigned e;
                        mc_subpel(m, cur, ref, bx, by, &vx, &vy, &e);
                        mv[bi * 2] = (int16_t)vx;
                        mv[bi * 2 + 1] = (int16_t)vy;
                        be[bi] = (uint16_t)(e > 65535u ? 65535u : e);
                    }
            }
            nbi[nb] = ci + side * d;
            nbd[nb] = d;
            nbs[nb] = sl;
            nb++;
        }
#undef SLOT
    if (!nb)
        return 0;

    /* per (neighbour, block): the combined weight and the width */
    for (int k = 0; k < nb; k++) {
        const uint16_t *be = m->berr + (size_t)nbs[k] * nblk;
        uint16_t *bw = m->bwt + (size_t)k * nblk, *bv = m->binv + (size_t)k * nblk;
        for (size_t bi = 0; bi < nblk; bi++) {
            unsigned e = be[bi];
            int wb, inv = 256;
            if (m->c.adapt) {
                /* Y264_MCSTF_ADAPT: a block that matches to within what the
                 * noise explains keeps its full weight; past that the weight
                 * falls with the square of the excess and the filter narrows
                 * in proportion, so a moving edge the search missed is
                 * averaged less AND over a smaller photometric window. */
                unsigned ok = e0 * (unsigned)m->c.adapt_k / 4;
                if (e <= ok) {
                    wb = 256;
                } else {
                    double r = (double)ok / e;
                    wb = (int)(256.0 * r * r + 0.5);
                    inv = (int)(256.0 / r + 0.5);
                    if (inv > 4096) inv = 4096;
                }
            } else {
                wb = berr_w_fixed(e);
            }
            int w = (mc_tstr[nbd[k]] * wb) >> 8;
            bw[bi] = (uint16_t)w;
            bv[bi] = (uint16_t)inv;
        }
    }

    /* Block by block, so each neighbour's vector, weight and width are
     * looked up once per block rather than once per sample. */
    for (int c = 0; c < 3; c++) {
        int sx = c ? m->sx : 0, sy = c ? m->sy : 0;
        int pw = plane_w(m, c), ph = plane_h(m, c);
        int bszx = BLK >> sx, bszy = BLK >> sy;
        int denx = 4 << sx, deny = 4 << sy;
        int den = denx > deny ? denx : deny;
        for (int by = 0; by < m->bh; by++) {
            int y0 = by * bszy, y1 = y0 + bszy < ph ? y0 + bszy : ph;
            for (int bx = 0; bx < m->bw; bx++) {
                int x0 = bx * bszx, x1 = x0 + bszx < pw ? x0 + bszx : pw;
                size_t bi = (size_t)by * m->bw + bx;
                int kn = 0;
                int kw[2 * Y264_MCSTF_MAX_RANGE], kinv[2 * Y264_MCSTF_MAX_RANGE];
                int kfx[2 * Y264_MCSTF_MAX_RANGE], kfy[2 * Y264_MCSTF_MAX_RANGE];
                int kst[2 * Y264_MCSTF_MAX_RANGE];
                const pixel *kp[2 * Y264_MCSTF_MAX_RANGE];
                for (int k = 0; k < nb; k++) {
                    int w0 = m->bwt[(size_t)k * nblk + bi];
                    if (!w0)
                        continue;
                    const int16_t *mv = m->mv + ((size_t)nbs[k] * nblk + bi) * 2;
                    const mc_pic *r = &m->p[nbi[k]];
                    int mx = mv[0], my = mv[1];
                    kw[kn] = w0;
                    kinv[kn] = m->binv[(size_t)k * nblk + bi];
                    /* unequal shifts (4:2:2) put both fractions on the finer
                     * denominator */
                    kfx[kn] = (mx & (denx - 1)) * (den / denx);
                    kfy[kn] = (my & (deny - 1)) * (den / deny);
                    kst[kn] = r->stride[c];
                    kp[kn] = r->org[c] + (ptrdiff_t)(my >> (2 + sy)) * r->stride[c] + (mx >> (2 + sx));
                    kn++;
                }
                for (int y = y0; y < y1; y++) {
                    const pixel *o = cur->org[c] + (size_t)y * cur->stride[c];
                    pixel *dst = m->out[c] + (size_t)y * m->out_stride[c];
                    for (int x = x0; x < x1; x++) {
                        int org = o[x];
                        int wsum = 256, acc = 256 * org;
                        for (int k = 0; k < kn; k++) {
                            const pixel *rp = kp[k] + (ptrdiff_t)y * kst[k] + x;
                            int rv = (kfx[k] | kfy[k]) ? bilin(rp, kst[k], kfx[k], kfy[k], den) : rp[0];
                            int diff = rv - org;
                            if (diff < 0) diff = -diff;
                            int gi = kinv[k] == 256 ? diff : (diff * kinv[k]) >> 8;
                            if (gi >= GTAB)
                                continue;
                            int wq = (kw[k] * m->gauss[gi]) >> 8;
                            acc += wq * rv;
                            wsum += wq;
                        }
                        dst[x] = (pixel)((acc + wsum / 2) / wsum);
                    }
                }
            }
        }
    }
    return 1;
}

void y264_mcstf_pop(y264_mcstf *m, const pixel *plane[3], int stride[3],
                    int64_t *pts, int *filtered)
{
    int ci = m->out_pos;
    const mc_pic *cur = &m->p[ci];
    int filt = (cur->display_idx % m->c.period) == 0 && m->n > 1;
    m->last_noise_q8 = -1;
    if (filt)
        filt = mc_filter(m, ci);
    for (int c = 0; c < 3; c++) {
        plane[c] = filt ? m->out[c] : cur->org[c];
        stride[c] = filt ? m->out_stride[c] : cur->stride[c];
    }
    if (m->c.trace)
        fprintf(stderr, "mcstf %5d filt=%d tnoise=%.2f\n", cur->display_idx, filt,
                m->last_noise_q8 < 0 ? -1.0 : m->last_noise_q8 / 256.0);
    *pts = cur->pts;
    *filtered = filt;
    m->out_pos++;
}
