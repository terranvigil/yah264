/*
 * checkasm: motion compensation - luma and chroma interpolation, the half-pel
 * plane rows, and the prediction averages.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Two kinds of row here, and the difference is deliberate.
 *
 * The KERNEL rows name a symbol and feed it the window contract the dispatcher
 * promises it -- an integer position plus a sub-pel phase, with the block far
 * enough inside the plane that no clamping is in play -- and check it against
 * the portable reference at the same position.
 *
 * The ORACLE rows (cpu_mask 0) drive the public entry point over every shape,
 * phase and position INCLUDING the ones outside the window, where the
 * dispatcher takes its border body or gathers a clamped tile. Those paths are C
 * on every architecture and they are where a coordinate bug actually lives: the
 * 4:4:4 border overflow of 08-29 was one. They are not a tautology because the
 * reference is the fully-clamped definitional one, not the path under test.
 */

#include "checkasm.h"
#include "dsp/arch.h"
#include "dsp/interp.h"

static pixel *pa, *pb;

static void plane(void)
{
    if (!pa) {
        pa = ca_guard_alloc(STRIDE * PLANE_H * sizeof(pixel));
        pb = ca_guard_alloc(STRIDE * PLANE_H * sizeof(pixel));
    }
    ca_fill(pa, STRIDE * PLANE_H);
    ca_fill(pb, STRIDE * PLANE_H);
}

/* A plane with edge-REPLICATED borders, which is what every reference plane the
 * encoder hands these kernels looks like: replication equals the spec's
 * coordinate clamp, so reading into the border is reading the clamped sample. */
static void replicate(pixel *org, int rstride, int w, int h, int border)
{
    for (int y = 0; y < h; y++) {
        pixel *row = org + (ptrdiff_t)y * rstride;
        for (int x = -border; x < 0; x++) row[x] = row[0];
        for (int x = w; x < w + border; x++) row[x] = row[w - 1];
    }
    for (int y = -border; y < 0; y++)
        memcpy(org + (ptrdiff_t)y * rstride - border, org - border,
               (size_t)(w + 2 * border) * sizeof(pixel));
    for (int y = h; y < h + border; y++)
        memcpy(org + (ptrdiff_t)y * rstride - border,
               org + (ptrdiff_t)(h - 1) * rstride - border,
               (size_t)(w + 2 * border) * sizeof(pixel));
}

enum { MW = 48, MH = 32, MB = Y264_LUMA_BORDER,
       MST = MW + 2 * MB, MPH = MH + 2 * MB };

static pixel mbuf[MPH * MST];
static pixel *mref;

static void mplane(void)
{
    mref = mbuf + (size_t)MB * MST + MB;
    for (int y = 0; y < MH; y++)
        ca_fill(mref + (size_t)y * MST, MW);
    replicate(mref, MST, MW, MH, MB);
}

/* The definitional six-tap and the sample clip, shared by the row kernels
 * and by the whole-plane oracle. */
#define HTAP6(a, b, c, d, e, f) ((a) - 5 * (b) + 20 * (c) + 20 * (d) - 5 * (e) + (f))
#define HCLIP(v) ((v) < 0 ? 0 : (v) > PIXEL_MAX ? PIXEL_MAX : (v))

#if Y264_HAVE_NEON || Y264_HAVE_SSE4

/* ---- one body per group, the kernel as an argument ------------------------
 *
 * Wave 1 of docs/x86-plan.md put the pixel family on this shape and wave 2
 * puts this family on it: a group's checks, its adversarial fills and its page
 * guards are written ONCE and take the kernel they run as a parameter, with
 * one thin row function per tier on top. A tier is then a list of names and
 * symbols rather than a copy of the checks, and a twin that lands later
 * inherits every fill the row already had instead of restating the ones whose
 * point it happened to notice. */

typedef void (*mc_luma_fn)(pixel *, int, const pixel *, int,
                           int, int, int, int, int);
typedef void (*mc_chroma_fn)(pixel *, int, const pixel *, int,
                             int, int, int, int, int);
typedef void (*mc_chroma8_fn)(pixel *, int, const pixel *, int,
                              int, int, int, int);
typedef void (*pred_copy_fn)(pixel *, int, const pixel *, int, int, int);
typedef void (*pred_avg2_fn)(pixel *, int, const pixel *, const pixel *,
                             int, int, int);
typedef void (*avg_wt_fn)(pixel *, const pixel *, const pixel *, int, int, int);
typedef void (*hrow_fn)(int32_t *, const pixel *, int, int);
typedef void (*outrow_fn)(pixel *, pixel *, pixel *,
                          const int32_t *, const int32_t *, const int32_t *,
                          const int32_t *, const int32_t *, const int32_t *,
                          const pixel *, const pixel *, const pixel *,
                          const pixel *, const pixel *, const pixel *, int, int);

/* ---- the prediction fetches ---------------------------------------------- */

/* The destination is written at a stride wider than the block on purpose: the
 * row padding is poisoned and checked, which is the net for a kernel that
 * stores a whole vector into a narrower block. */
static int run_pred_copy(const char *name, pred_copy_fn kern)
{
    enum { DS = 24 };
    static const int ws[] = { 16, 8, 4 }, hs[] = { 16, 8, 4, 2 };
    pixel *d1 = ca_guard_alloc(DS * 16 * sizeof(pixel));
    pixel d2[DS * 16];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        plane();
        for (unsigned wi = 0; wi < sizeof(ws) / sizeof(ws[0]); wi++)
            for (unsigned hi = 0; hi < sizeof(hs) / sizeof(hs[0]); hi++) {
                int w = ws[wi], h = hs[hi];
                int off = (t * 7) % 5, ss = STRIDE - (t & 1);
                memset(d1, CA_POISON, DS * 16 * sizeof(pixel));
                ca_guard_poison(d1, DS * 16 * sizeof(pixel));
                memset(d2, CA_POISON, sizeof(d2));
                kern(d1, DS, pa + off, ss, w, h);
                y264_pred_copy_c(d2, DS, pa + off, ss, w, h);
                if (memcmp(d1, d2, DS * 16 * sizeof(pixel))) {
                    if (!bad) ca_fail("%s: w=%d h=%d", name, w, h);
                    bad++;
                }
                if (!bad && (ca_guard_check(d1, DS * 16 * sizeof(pixel), name) ||
                             ca_check_pad_pix(d1, DS, w, h, name)))
                    bad++;
            }
    }
    /* The source window is exactly w x h at the stride given. */
    for (unsigned wi = 0; wi < sizeof(ws) / sizeof(ws[0]) && !bad; wi++) {
        int w = ws[wi], h = 16;
        size_t win = ca_pg_window_pix(STRIDE, w, h);
        for (int tail = 0; tail <= 1; tail++) {
            ca_pg g;
            pixel *s = ca_pg_alloc(&g, win, tail);
            pixel dst[16 * 16];
            ca_pg_fill_pix(&g, win / sizeof(pixel));
            ca_pg_arm(name);
            kern(dst, 16, s, STRIDE, w, h);
            ca_pg_disarm();
            ca_pg_free(&g);
        }
    }
    CA_BENCH2(name, kern(d1, DS, pa, STRIDE, 16, 16),
                    y264_pred_copy_c(d2, DS, pa, STRIDE, 16, 16));
    ca_guard_free(d1);
    return bad;
}

static int run_pred_avg2(const char *name, pred_avg2_fn kern)
{
    enum { DS = 24 };
    static const int ws[] = { 16, 8, 4 }, hs[] = { 16, 8, 4, 2 };
    pixel *d1 = ca_guard_alloc(DS * 16 * sizeof(pixel));
    pixel d2[DS * 16];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        plane();
        for (unsigned wi = 0; wi < sizeof(ws) / sizeof(ws[0]); wi++)
            for (unsigned hi = 0; hi < sizeof(hs) / sizeof(hs[0]); hi++) {
                int w = ws[wi], h = hs[hi];
                int off = (t * 7) % 5, ss = STRIDE - (t & 1);
                memset(d1, CA_POISON, DS * 16 * sizeof(pixel));
                ca_guard_poison(d1, DS * 16 * sizeof(pixel));
                memset(d2, CA_POISON, sizeof(d2));
                kern(d1, DS, pa + off, pb + off + 1, ss, w, h);
                y264_pred_avg2_c(d2, DS, pa + off, pb + off + 1, ss, w, h);
                if (memcmp(d1, d2, DS * 16 * sizeof(pixel))) {
                    if (!bad) ca_fail("%s: w=%d h=%d", name, w, h);
                    bad++;
                }
                if (!bad && (ca_guard_check(d1, DS * 16 * sizeof(pixel), name) ||
                             ca_check_pad_pix(d1, DS, w, h, name)))
                    bad++;
            }
    }
    /* Both sources are guarded at once and at the same tail: a kernel that
     * over-read both by the same amount would still produce the right
     * average out of a heap buffer. */
    for (unsigned wi = 0; wi < sizeof(ws) / sizeof(ws[0]) && !bad; wi++) {
        int w = ws[wi], h = 16;
        size_t win = ca_pg_window_pix(STRIDE, w, h);
        for (int tail = 0; tail <= 1; tail++) {
            ca_pg ga, gb;
            pixel *s1 = ca_pg_alloc(&ga, win, tail);
            pixel *s2 = ca_pg_alloc(&gb, win, tail);
            pixel dst[16 * 16];
            ca_pg_fill_pix(&ga, win / sizeof(pixel));
            ca_pg_fill_pix(&gb, win / sizeof(pixel));
            ca_pg_arm(name);
            kern(dst, 16, s1, s2, STRIDE, w, h);
            ca_pg_disarm();
            ca_pg_free(&gb);
            ca_pg_free(&ga);
        }
    }
    CA_BENCH2(name, kern(d1, DS, pa, pb, STRIDE, 16, 16),
                    y264_pred_avg2_c(d2, DS, pa, pb, STRIDE, 16, 16));
    ca_guard_free(d1);
    return bad;
}

/* Every (w0, w1) the implicit weight derivation can produce -- w1 in
 * [-64, 128], w0 = 64 - w1 -- plus the unweighted (32, 32) fast path, over
 * every packed block length the encoder passes. The extreme weights are where
 * the lane bound and the saturating narrow are load-bearing. */
static int run_pixel_avg_wt(const char *name, avg_wt_fn kern)
{
    static const int len[] = { 64, 128, 256 };
    pixel *d1 = ca_guard_alloc(256 * sizeof(pixel));
    pixel d2[256];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) { memset(pa, 0, 256 * sizeof(pixel));
                      memset(pb, 0xff, 256 * sizeof(pixel)); }
        if (t == 1) { memset(pa, 0xff, 256 * sizeof(pixel));
                      memset(pb, 0, 256 * sizeof(pixel)); }
        for (int w1 = -64; w1 <= 128; w1++) {
            int w0 = 64 - w1;
            for (unsigned k = 0; k < sizeof(len) / sizeof(len[0]); k++) {
                memset(d1, CA_POISON, 256 * sizeof(pixel));
                ca_guard_poison(d1, (size_t)len[k] * sizeof(pixel));
                memset(d2, 0xa5, sizeof(d2));
                kern(d1, pa, pb, len[k], w0, w1);
                y264_pixel_avg_wt_c(d2, pa, pb, len[k], w0, w1);
                if (memcmp(d1, d2, (size_t)len[k] * sizeof(pixel))) {
                    if (!bad) ca_fail("%s: w0=%d w1=%d n=%d", name, w0, w1, len[k]);
                    bad++;
                }
                if (!bad && ca_guard_check(d1, (size_t)len[k] * sizeof(pixel), name))
                    bad++;
            }
        }
    }
    CA_BENCH2(name, kern(d1, pa, pb, 256, 32, 32),
                    y264_pixel_avg_wt_c(d2, pa, pb, 256, 32, 32));
    ca_guard_free(d1);
    return bad;
}

typedef void (*avg_wt_2d_fn)(pixel *, int, const pixel *, int, const pixel *, int,
                             int, int, int, int);
/* The strided form over every block shape the B analysis hands it (8x8
 * quadrants, 16-wide partition rects, 8/4/2-wide chroma), at odd operand
 * strides so a stride mix-up cannot pass, and every implicit weight. */
static int run_pixel_avg_wt_2d(const char *name, avg_wt_2d_fn kern)
{
    static const int shp[][2] = { { 8, 8 }, { 16, 16 }, { 16, 8 }, { 8, 16 },
                                  { 8, 4 }, { 4, 4 }, { 4, 8 }, { 2, 2 }, { 2, 4 } };
    enum { DS = 19, AS = 17, BS = 23 };
    pixel *d1 = ca_guard_alloc(16 * DS * sizeof(pixel));
    pixel d2[16 * DS];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t == 0) { memset(pa, 0, 16 * BS * sizeof(pixel));
                      memset(pb, 0xff, 16 * BS * sizeof(pixel)); }
        if (t == 1) { memset(pa, 0xff, 16 * BS * sizeof(pixel));
                      memset(pb, 0, 16 * BS * sizeof(pixel)); }
        for (int w1 = -64; w1 <= 128; w1 += (t < 2 ? 1 : 7)) {
            int w0 = 64 - w1;
            for (unsigned k = 0; k < sizeof(shp) / sizeof(shp[0]); k++) {
                int w = shp[k][0], h = shp[k][1];
                memset(d1, CA_POISON, 16 * DS * sizeof(pixel));
                memcpy(d2, d1, sizeof(d2));
                kern(d1, DS, pa, AS, pb, BS, w, h, w0, w1);
                y264_pixel_avg_wt_2d_c(d2, DS, pa, AS, pb, BS, w, h, w0, w1);
                if (memcmp(d1, d2, 16 * DS * sizeof(pixel))) {
                    if (!bad) ca_fail("%s: w0=%d w1=%d %dx%d", name, w0, w1, w, h);
                    bad++;
                }
            }
        }
    }
    /* Both operands mapped to exactly w x h at their own strides, at the
     * flat pair and at a weighted one: a row step that loads a whole register
     * for a narrower block faults here. */
    for (unsigned k = 0; k < sizeof(shp) / sizeof(shp[0]) && !bad; k++) {
        int w = shp[k][0], h = shp[k][1];
        size_t wa = ca_pg_window_pix(AS, w, h), wb = ca_pg_window_pix(BS, w, h);
        for (int tail = 0; tail <= 1; tail++)
            for (int wt = 0; wt < 2; wt++) {
                ca_pg ga, gb;
                pixel *sa = ca_pg_alloc(&ga, wa, tail);
                pixel *sb = ca_pg_alloc(&gb, wb, tail);
                ca_pg_fill_pix(&ga, wa / sizeof(pixel));
                ca_pg_fill_pix(&gb, wb / sizeof(pixel));
                memset(d1, CA_POISON, 16 * DS * sizeof(pixel));
                memcpy(d2, d1, sizeof(d2));
                ca_pg_arm(name);
                kern(d1, DS, sa, AS, sb, BS, w, h, wt ? 40 : 32, wt ? 24 : 32);
                ca_pg_disarm();
                y264_pixel_avg_wt_2d_c(d2, DS, sa, AS, sb, BS, w, h,
                                       wt ? 40 : 32, wt ? 24 : 32);
                if (memcmp(d1, d2, 16 * DS * sizeof(pixel))) {
                    if (!bad) ca_fail("%s: guarded %dx%d", name, w, h);
                    bad++;
                }
                ca_pg_free(&gb);
                ca_pg_free(&ga);
            }
    }
    CA_BENCH2(name, kern(d1, 16, pa, 16, pb, 16, 8, 8, 40, 24),
                    y264_pixel_avg_wt_2d_c(d2, 16, pa, 16, pb, 16, 8, 8, 40, 24));
    ca_guard_free(d1);
    return bad;
}

typedef void (*bilin4_row_fn)(pixel *, const pixel *, const pixel *, int, int,
                              int, int, int, int);
/* The lowres plane's bilinear row against the scalar row: every phase weight
 * set, lengths on both sides of every register step, the output guarded and
 * both rows mapped to exactly the n + ax columns the row declares. */
static int run_bilin4_row(const char *name, bilin4_row_fn kern)
{
    static const int ns[] = { 5, 15, 16, 17, 31, 32, 33, 47, 48, 56 };
    enum { NMAX = 56 };
    pixel *o = ca_guard_alloc(NMAX * sizeof(pixel));
    pixel r[NMAX];
    int bad = 0;
    for (int t = 0; t < TRIALS && !bad; t++) {
        plane();
        if (t % 4 == 1) for (int i = 0; i < STRIDE * 8; i++) pa[i] = ca_rnd_edge();
        for (int fy = 0; fy < 4; fy++)
            for (int fx = 0; fx < 4; fx++) {
                if (!(fx | fy)) continue;
                int ax = fx ? 1 : 0;
                int n = ns[(unsigned)(t + fx + 4 * fy) % (sizeof(ns) / sizeof(ns[0]))] - ax;
                int w00 = (4 - fx) * (4 - fy), w10 = fx * (4 - fy), w01 = (4 - fx) * fy, w11 = fx * fy;
                const pixel *r0 = pa + (int)ca_below(4) * STRIDE, *r1 = r0 + STRIDE;
                memset(o, CA_POISON, NMAX * sizeof(pixel));
                ca_guard_poison(o, (size_t)n * sizeof(pixel));
                kern(o, r0, r1, n, ax, w00, w10, w01, w11);
                for (int X = 0; X < n; X++)
                    r[X] = (pixel)((w00 * r0[X] + w10 * r0[X + ax] + w01 * r1[X] + w11 * r1[X + ax] + 8) >> 4);
                if (memcmp(o, r, (size_t)n * sizeof(pixel))) {
                    if (!bad) ca_fail("%s: phase %d,%d n=%d", name, fx, fy, n);
                    bad++;
                }
                if (!bad && ca_guard_check(o, (size_t)n * sizeof(pixel), name)) bad++;
            }
    }
    for (unsigned k = 0; k < sizeof(ns) / sizeof(ns[0]) && !bad; k++)
        for (int ax = 0; ax <= 1; ax++)
            for (int tail = 0; tail <= 1; tail++) {
                int n = ns[k] - ax;
                ca_pg g0, g1;
                pixel *s0 = ca_pg_alloc(&g0, (size_t)(n + ax) * sizeof(pixel), tail);
                pixel *s1 = ca_pg_alloc(&g1, (size_t)(n + ax) * sizeof(pixel), tail);
                ca_pg_fill_pix(&g0, (size_t)(n + ax));
                ca_pg_fill_pix(&g1, (size_t)(n + ax));
                ca_pg_arm(name);
                kern(o, s0, s1, n, ax, ax ? 6 : 12, ax ? 6 : 0, ax ? 2 : 4, ax ? 2 : 0);
                ca_pg_disarm();
                ca_pg_free(&g1);
                ca_pg_free(&g0);
            }
    ca_guard_free(o);
    return bad;
}

typedef void (*mc_chroma_bi8_fn)(pixel *, int, const pixel *, int, int, int,
                                 const pixel *, int, int, int, int, int);
/* Two 8x8 chroma compensations averaged in one pass, against the plain C
 * compensation into buffers and the plain C average, over every phase pair
 * and weight pair. Then each list mapped to exactly the nine columns of nine
 * rows the kernel declares. */
static int run_mc_chroma_bi8(const char *name, mc_chroma_bi8_fn kern)
{
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        plane();
        for (int wi = -64; wi <= 128 && bad < 4; wi += (t < 2 ? 4 : 36)) {
            int w1 = t == 0 ? 32 : wi, w0 = 64 - w1;   /* trial 0: the flat pair */
            int fx0 = (int)ca_below(8), fy0 = (int)ca_below(8);
            int fx1 = (int)ca_below(8), fy1 = (int)ca_below(8);
            int ix0 = 4 + (int)ca_below(16), iy0 = 4 + (int)ca_below(8);
            int ix1 = 4 + (int)ca_below(16), iy1 = 4 + (int)ca_below(8);
            pixel c0[64], c1[64], ref[64], out[8 * 11];
            y264_mc_chroma_c(c0, 8, pa, STRIDE, 48, 32, ix0, iy0, fx0, fy0, 8, 8, 2, 2);
            y264_mc_chroma_c(c1, 8, pb, STRIDE, 48, 32, ix1, iy1, fx1, fy1, 8, 8, 2, 2);
            y264_pixel_avg_wt_c(ref, c0, c1, 64, w0, w1);
            memset(out, 0xA5, sizeof out);
            kern(out + 8, 8, pa + iy0 * STRIDE + ix0, STRIDE, fx0, fy0,
                 pb + iy1 * STRIDE + ix1, STRIDE, fx1, fy1, w0, w1);
            if (memcmp(out + 8, ref, 64) || out[0] != 0xA5 || out[8 * 9] != 0xA5) {
                if (!bad) ca_fail("%s: w %d/%d phases %d,%d %d,%d", name, w0, w1,
                                  fx0, fy0, fx1, fy1);
                bad++;
            }
        }
    }
    for (int ph = 0; ph < 64 && !bad; ph += 9) {
        size_t win = ca_pg_window_pix(STRIDE, 9, 9);
        for (int tail = 0; tail <= 1; tail++)
            for (int wt = 0; wt < 2; wt++) {
                ca_pg g0, g1;
                pixel *s0 = ca_pg_alloc(&g0, win, tail);
                pixel *s1 = ca_pg_alloc(&g1, win, tail);
                pixel out[64];
                ca_pg_fill_pix(&g0, win / sizeof(pixel));
                ca_pg_fill_pix(&g1, win / sizeof(pixel));
                ca_pg_arm(name);
                kern(out, 8, s0, STRIDE, ph & 7, ph >> 3, s1, STRIDE,
                     (ph >> 3), ph & 7, wt ? 40 : 32, wt ? 24 : 32);
                ca_pg_disarm();
                ca_pg_free(&g1);
                ca_pg_free(&g0);
            }
    }
    return bad;
}

typedef void (*avg_mb420_flat_fn)(pixel *, pixel *, pixel *,
                                  const pixel *, int, const pixel *, int,
                                  const pixel *, const pixel *, int,
                                  const pixel *, const pixel *, int);
/* The fused flat 4:2:0 macroblock average against three C flat averages,
 * with the three destinations guarded and, in a second pass, all six sources
 * mapped to exactly their 16x16 and 8x8 windows. */
static int run_avg_mb420_flat(const char *name, avg_mb420_flat_fn kern)
{
    int bad = 0;
    pixel *p = ca_guard_alloc(256 * sizeof(pixel));
    pixel *cu = ca_guard_alloc(64 * sizeof(pixel));
    pixel *cv = ca_guard_alloc(64 * sizeof(pixel));
    pixel rp[256], ru[64], rv[64];
    for (int t = 0; t < TRIALS && !bad; t++) {
        plane();
        const pixel *l0 = pa + (int)ca_below(8) * STRIDE + (int)ca_below(16);
        const pixel *l1 = pb + (int)ca_below(8) * STRIDE + (int)ca_below(16);
        const pixel *u0 = pa + 20 * STRIDE + (int)ca_below(16), *v0 = u0 + 24;
        const pixel *u1 = pb + 20 * STRIDE + (int)ca_below(16), *v1 = u1 + 24;
        ca_guard_poison(p, 256 * sizeof(pixel));
        ca_guard_poison(cu, 64 * sizeof(pixel));
        ca_guard_poison(cv, 64 * sizeof(pixel));
        kern(p, cu, cv, l0, STRIDE, l1, STRIDE, u0, v0, STRIDE, u1, v1, STRIDE);
        y264_pixel_avg_wt_2d_c(rp, 16, l0, STRIDE, l1, STRIDE, 16, 16, 32, 32);
        y264_pixel_avg_wt_2d_c(ru, 8, u0, STRIDE, u1, STRIDE, 8, 8, 32, 32);
        y264_pixel_avg_wt_2d_c(rv, 8, v0, STRIDE, v1, STRIDE, 8, 8, 32, 32);
        if (memcmp(p, rp, 256) || memcmp(cu, ru, 64) || memcmp(cv, rv, 64)) {
            ca_fail("%s: trial %d", name, t);
            bad++;
        }
        if (!bad && (ca_guard_check(p, 256 * sizeof(pixel), name) ||
                     ca_guard_check(cu, 64 * sizeof(pixel), name) ||
                     ca_guard_check(cv, 64 * sizeof(pixel), name)))
            bad++;
    }
    for (int tail = 0; tail <= 1 && !bad; tail++) {
        enum { LS = 21, CS = 13 };        /* odd strides, different per list */
        size_t wl0 = ca_pg_window_pix(LS, 16, 16), wl1 = ca_pg_window_pix(LS + 2, 16, 16);
        size_t wc0 = ca_pg_window_pix(CS, 8, 8), wc1 = ca_pg_window_pix(CS + 2, 8, 8);
        ca_pg g[6];
        pixel *l0 = ca_pg_alloc(&g[0], wl0, tail), *l1 = ca_pg_alloc(&g[1], wl1, tail);
        pixel *u0 = ca_pg_alloc(&g[2], wc0, tail), *v0 = ca_pg_alloc(&g[3], wc0, tail);
        pixel *u1 = ca_pg_alloc(&g[4], wc1, tail), *v1 = ca_pg_alloc(&g[5], wc1, tail);
        ca_pg_fill_pix(&g[0], wl0 / sizeof(pixel)); ca_pg_fill_pix(&g[1], wl1 / sizeof(pixel));
        ca_pg_fill_pix(&g[2], wc0 / sizeof(pixel)); ca_pg_fill_pix(&g[3], wc0 / sizeof(pixel));
        ca_pg_fill_pix(&g[4], wc1 / sizeof(pixel)); ca_pg_fill_pix(&g[5], wc1 / sizeof(pixel));
        ca_pg_arm(name);
        kern(p, cu, cv, l0, LS, l1, LS + 2, u0, v0, CS, u1, v1, CS + 2);
        ca_pg_disarm();
        y264_pixel_avg_wt_2d_c(rp, 16, l0, LS, l1, LS + 2, 16, 16, 32, 32);
        y264_pixel_avg_wt_2d_c(ru, 8, u0, CS, u1, CS + 2, 8, 8, 32, 32);
        y264_pixel_avg_wt_2d_c(rv, 8, v0, CS, v1, CS + 2, 8, 8, 32, 32);
        if (memcmp(p, rp, 256) || memcmp(cu, ru, 64) || memcmp(cv, rv, 64)) {
            ca_fail("%s: guarded, tail %d", name, tail);
            bad++;
        }
        for (int k = 5; k >= 0; k--) ca_pg_free(&g[k]);
    }
    ca_guard_free(cv);
    ca_guard_free(cu);
    ca_guard_free(p);
    return bad;
}

/* ---- the interpolation kernels ------------------------------------------- */

/* The read window a luma kernel DECLARES, phase by phase: the block itself,
 * the six-tap's reach wherever the phase filters, and the one row and column
 * the odd phases fetch a step further on. `right` is how far past the block's
 * own last column that tier's horizontal filter reaches, and it is a per-row
 * argument because the tiers differ: the NEON 8-wide form takes ONE sixteen
 * byte load where the x86 one takes six loads of eight, so NEON declares five
 * columns past the block and x86 declares two. Mapping the union would let
 * either hide inside the other's slack. */
static void mc_luma_pg(const char *what, mc_luma_fn kern, int w, int right)
{
    for (int h = 4; h <= 16; h += 4)
        for (int ph = 0; ph < 16; ph++) {
            int fx = ph & 3, fy = ph >> 2;
            int c0 = fx ? -2 : 0, cn = fx ? w + right + 3 : w + 1;
            int r0 = fy ? -2 : 0, rn = fy ? h + 6 : h + 1;
            if (!fx && !fy) { c0 = 0; cn = w; r0 = 0; rn = h; }
            size_t win = ca_pg_window_pix(STRIDE, cn, rn);
            for (int tail = 0; tail <= 1; tail++) {
                ca_pg g;
                pixel *m = ca_pg_alloc(&g, win, tail);
                pixel dst[16 * 16];
                ca_pg_fill_pix(&g, win / sizeof(pixel));
                ca_pg_arm(what);
                kern(dst, 16, m - (ptrdiff_t)r0 * STRIDE - c0, STRIDE,
                     0, 0, fx, fy, h);
                ca_pg_disarm();
                ca_pg_free(&g);
            }
        }
}

/* The window kernels, at positions the dispatcher would hand them: block
 * inside the plane, every sub-pel phase, both widths. The reference is
 * y264_mc_luma_c at the same motion vector, which is the fully-clamped
 * definitional path. */
static int run_mc_luma_win(const char *name, mc_luma_fn k16, mc_luma_fn k8,
                           int right16, int right8)
{
    int bad = 0;
    for (int t = 0; t < TRIALS / 4 + 1; t++) {
        mplane();
        for (int ph = 0; ph < 16; ph++)
            for (int h = 4; h <= 16; h += 4)
                for (int wsel = 0; wsel < 2; wsel++) {
                    int w = wsel ? 16 : 8;
                    int bx = 16, by = 8;
                    int mvx = ph & 3, mvy = (ph >> 2) & 3;
                    int ix = bx + (mvx >> 2), iy = by + (mvy >> 2);
                    pixel d1[16 * 16], d2[16 * 16];
                    memset(d1, 1, sizeof(d1));
                    memset(d2, 2, sizeof(d2));
                    if (w == 16)
                        k16(d1, 16, mref, MST, ix, iy, mvx & 3, mvy & 3, h);
                    else
                        k8(d1, 8, mref, MST, ix, iy, mvx & 3, mvy & 3, h);
                    y264_mc_luma_c(d2, 16, mref, MST, MW, MH, bx, by,
                                   mvx, mvy, w, h);
                    for (int y = 0; y < h; y++)
                        if (memcmp(d1 + (size_t)y * (w == 16 ? 16 : 8),
                                   d2 + (size_t)y * 16,
                                   (size_t)w * sizeof(pixel))) {
                            if (!bad)
                                ca_fail("%s: %dx%d phase %d,%d",
                                        name, w, h, mvx, mvy);
                            bad++;
                            break;
                        }
                }
        if (t == 0 && ca_bench) {
            pixel d1[16 * 16];
            /* phase 2,2: the centre of the quarter-pel grid, both taps live */
            CA_BENCH2("mc_luma 16x16 h2v2",
                      k16(d1, 16, mref, MST, 16, 8, 2, 2, 16),
                      y264_mc_luma_c(d1, 16, mref, MST, MW, MH, 16, 8,
                                     2, 2, 16, 16));
            CA_BENCH2("mc_luma 8x8 h2v2",
                      k8(d1, 8, mref, MST, 16, 8, 2, 2, 8),
                      y264_mc_luma_c(d1, 16, mref, MST, MW, MH, 16, 8,
                                     2, 2, 8, 8));
        }
    }
    if (!bad) {
        mc_luma_pg(name, k16, 16, right16);
        mc_luma_pg(name, k8, 8, right8);
    }
    return bad;
}

/* The chroma kernels, at the shapes the dispatcher routes to each of them.
 * `k8` is the fixed 8x8 form where a tier has one and NULL where the 8-wide
 * kernel serves that height too. */
static int run_mc_chroma_win(const char *name, mc_chroma_fn k8h,
                             mc_chroma_fn k4h, mc_chroma8_fn k8)
{
    enum { CW = 40, CH = 24, CB = Y264_CHROMA_BORDER, CST = CW + 2 * CB };
    static pixel cbuf[(CH + 2 * CB) * CST];
    pixel *cref = cbuf + (size_t)CB * CST + CB;
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        for (int y = 0; y < CH; y++)
            ca_fill(cref + (size_t)y * CST, CW);
        replicate(cref, CST, CW, CH, CB);
        for (int w = 4; w <= 8; w += 4)
            for (int h = 2; h <= 16; h += 2)
                for (int fx = 0; fx < 8; fx++)
                    for (int fy = 0; fy < 8; fy++) {
                        int cbx = 8, cby = 4;
                        int mvx = fx, mvy = fy;   /* sub_w == sub_h == 2 */
                        int ix = cbx + (mvx >> 3), iy = cby + (mvy >> 3);
                        pixel d1[16 * 16], d2[16 * 16];
                        memset(d1, 1, sizeof(d1));
                        memset(d2, 2, sizeof(d2));
                        if (w == 4)
                            k4h(d1, 16, cref, CST, ix, iy, mvx & 7, mvy & 7, h);
                        else if (h == 8 && k8)
                            k8(d1, 16, cref, CST, ix, iy, mvx & 7, mvy & 7);
                        else
                            k8h(d1, 16, cref, CST, ix, iy, mvx & 7, mvy & 7, h);
                        y264_mc_chroma_c(d2, 16, cref, CST, CW, CH, cbx, cby,
                                         mvx, mvy, w, h, 2, 2);
                        for (int y = 0; y < h; y++)
                            if (memcmp(d1 + (size_t)y * 16, d2 + (size_t)y * 16,
                                       (size_t)w * sizeof(pixel))) {
                                if (!bad)
                                    ca_fail("%s: %dx%d phase %d,%d",
                                            name, w, h, fx, fy);
                                bad++;
                                break;
                            }
                    }
        if (t == 0 && ca_bench) {
            pixel d1[16 * 16];
            CA_BENCH2("mc_chroma 8x8",
                      k8h(d1, 16, cref, CST, 8, 4, 3, 5, 8),
                      y264_mc_chroma_c(d1, 16, cref, CST, CW, CH, 8, 4,
                                       3, 5, 8, 8, 2, 2));
            CA_BENCH2("mc_chroma 4x8",
                      k4h(d1, 16, cref, CST, 8, 4, 3, 5, 8),
                      y264_mc_chroma_c(d1, 16, cref, CST, CW, CH, 8, 4,
                                       3, 5, 4, 8, 2, 2));
        }
    }
    /* The bilinear reads one column and one row past the block, and nothing
     * further, whatever the phase -- including the phases whose weight on
     * that column is zero. */
    for (int w = 4; w <= 8 && !bad; w += 4)
        for (int h = 2; h <= 16; h += 2) {
            size_t win = ca_pg_window_pix(CST, w + 1, h + 1);
            for (int tail = 0; tail <= 1; tail++) {
                ca_pg g;
                pixel *m = ca_pg_alloc(&g, win, tail);
                pixel dst[16 * 16];
                ca_pg_fill_pix(&g, win / sizeof(pixel));
                ca_pg_arm(name);
                if (w == 4)
                    k4h(dst, 16, m, CST, 0, 0, 3, 5, h);
                else if (h == 8 && k8)
                    k8(dst, 16, m, CST, 0, 0, 3, 5);
                else
                    k8h(dst, 16, m, CST, 0, 0, 3, 5, h);
                ca_pg_disarm();
                ca_pg_free(&g);
            }
        }
    return bad;
}

/* ---- the half-pel plane rows ---------------------------------------------
 *
 * These two are the row kernels the plane builder calls over the interior
 * span, and they were only ever reachable through the whole-plane build
 * before. Both are checked against the definitional 6-tap at every column of
 * the span, which is what the builder's own C body computes.
 *
 * The spans are several, not one. A kernel that computes sixteen columns at a
 * time has a tail to get back onto a span of nine, and a span of exactly the
 * vector width is the case where the tail step is zero; the NEON pair only
 * ever saw one span of 59. */
/* noinline: an inlinable reference is loop-invariant inside the bench and
 * the compiler hoists it out, which reads as a 0.2 ns C row. */
__attribute__((noinline))
static void hrow_ref(int32_t *o, const pixel *rw, int x0, int x1)
{
    for (int x = x0; x < x1; x++)
        o[x] = HTAP6(rw[x-2], rw[x-1], rw[x], rw[x+1], rw[x+2], rw[x+3]);
}

__attribute__((noinline))
static void outrow_ref(pixel *H, pixel *V, pixel *C, const int32_t **s,
                       const pixel **r, int x0, int x1)
{
    for (int x = x0; x < x1; x++) {
        H[x] = (pixel)HCLIP((s[2][x] + 16) >> 5);
        C[x] = (pixel)HCLIP((HTAP6(s[0][x], s[1][x], s[2][x], s[3][x], s[4][x],
                                   s[5][x]) + 512) >> 10);
        V[x] = (pixel)HCLIP((HTAP6(r[0][x], r[1][x], r[2][x], r[3][x], r[4][x],
                                   r[5][x]) + 16) >> 5);
    }
}

static int run_hpel_rows(const char *name, hrow_fn hrow, outrow_fn outrow)
{
    enum { HW = 64, HPAD = 16, HRST = HW + 2 * HPAD };
    static const int spans[] = { 8, 9, 15, 16, 17, 59 };
    static pixel row[HRST], r[6][HRST];
    static int32_t s[6][HRST], sref[HRST];
    static pixel Hr[HRST], Vr[HRST], Cr[HRST];
    pixel *rw = row + HPAD;
    int bad = 0;
    for (int t = 0; t < TRIALS && !bad; t++) {
        int x0 = 2, x1 = x0 + spans[t % (int)(sizeof(spans) / sizeof(spans[0]))];
        ca_fill(row, HRST);
        for (int x = x0; x < x1; x++)
            sref[x] = HTAP6(rw[x-2], rw[x-1], rw[x], rw[x+1], rw[x+2], rw[x+3]);
        memset(s[0], 0x5a, sizeof(s[0]));
        hrow(s[0], rw, x0, x1);
        for (int x = x0; x < x1 && !bad; x++)
            if (s[0][x] != sref[x]) {
                ca_fail("%s hrow: span %d x=%d opt=%d ref=%d", name, x1 - x0,
                        x, s[0][x], sref[x]);
                bad++;
            }
        /* the output row: H from the middle intermediate, C from the vertical
         * 6-tap over the intermediates, V from the vertical 6-tap over the
         * integer rows */
        for (int k = 0; k < 6; k++) {
            ca_fill(r[k], HRST);
            for (int x = 0; x < HRST; x++)
                s[k][x] = (int32_t)((int)ca_rnd() * 32 - 4096);
        }
        memset(Hr, 0x5a, sizeof(Hr));
        memset(Vr, 0x5a, sizeof(Vr));
        memset(Cr, 0x5a, sizeof(Cr));
        outrow(Hr, Vr, Cr, s[0], s[1], s[2], s[3], s[4], s[5],
               r[0], r[1], r[2], r[3], r[4], r[5], x0, x1);
        for (int x = x0; x < x1 && !bad; x++) {
            int hv = HCLIP((s[2][x] + 16) >> 5);
            int cv = HCLIP((HTAP6(s[0][x], s[1][x], s[2][x], s[3][x], s[4][x],
                                  s[5][x]) + 512) >> 10);
            int vv = HCLIP((HTAP6(r[0][x], r[1][x], r[2][x], r[3][x], r[4][x],
                                  r[5][x]) + 16) >> 5);
            if (Hr[x] != hv || Vr[x] != vv || Cr[x] != cv) {
                ca_fail("%s outrow: span %d x=%d H %d/%d V %d/%d C %d/%d",
                        name, x1 - x0, x, Hr[x], hv, Vr[x], vv, Cr[x], cv);
                bad++;
            }
        }
        if (t == 0 && ca_bench) {
            /* per ROW of span 59 columns, the unit the plane builder calls;
             * the record is kept per 16x16 tile, so it converts. */
            CA_BENCH2("hpel_hrow 59col",
                      hrow(s[0], rw, 2, 61),
                      hrow_ref(sref, rw, 2, 61));
            CA_BENCH2("hpel_outrow 59col",
                      outrow(Hr, Vr, Cr, s[0], s[1], s[2], s[3],
                             s[4], s[5], r[0], r[1], r[2], r[3],
                             r[4], r[5], 2, 61),
                      outrow_ref(Hr, Vr, Cr, (const int32_t **)(int32_t *[]){
                                 s[0], s[1], s[2], s[3], s[4], s[5] },
                                 (const pixel **)(pixel *[]){
                                 r[0], r[1], r[2], r[3], r[4], r[5] }, 2, 61));
        }
    }
    return bad;
}

#endif /* Y264_HAVE_NEON || Y264_HAVE_SSE4 */

#if Y264_HAVE_SSE4

/* ---- the bodies only the x86 tiers have rows for -------------------------- */

typedef void (*lr_hp_row_fn)(pixel *, pixel *, pixel *, const pixel *,
                             const pixel *, const pixel *, int);

/* One row of the three lowres half-pel planes against the C row, at every
 * length class the step-back tail has (a multiple of the register, one over,
 * one under, the eight-column floor), on random and on all-extreme rows. The
 * three outputs are guarded separately and the three source rows are mapped to
 * exactly the columns the row declares: r0 from column 1, r1 and r2 from
 * column 0, all three through column 2n. */
static int run_lr_hp_row(const char *name, lr_hp_row_fn kern)
{
    static const int ns[] = { 8, 9, 15, 16, 17, 23, 24, 31, 32, 33, 47, 61 };
    enum { NMAX = 61 };
    pixel *o[3], ref[3][NMAX];
    int bad = 0;
    for (int k = 0; k < 3; k++) o[k] = ca_guard_alloc(NMAX * sizeof(pixel));
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t % 5 == 1)
            for (int i = 0; i < STRIDE * 8; i++) { pa[i] = ca_rnd_edge(); pb[i] = ca_rnd_edge(); }
        const pixel *r0 = pa + (t & 3), *r1 = pa + 4 * STRIDE + (t % 3), *r2 = pb + (t % 5);
        for (unsigned k = 0; k < sizeof(ns) / sizeof(ns[0]); k++) {
            int n = ns[k];
            for (int j = 0; j < 3; j++) {
                memset(o[j], CA_POISON, NMAX * sizeof(pixel));
                ca_guard_poison(o[j], (size_t)n * sizeof(pixel));
            }
            kern(o[0], o[1], o[2], r0, r1, r2, n);
            y264_lr_hp_row_c(ref[0], ref[1], ref[2], r0, r1, r2, n);
            for (int j = 0; j < 3; j++) {
                if (memcmp(o[j], ref[j], (size_t)n * sizeof(pixel))) {
                    if (!bad) ca_fail("%s: plane %d n=%d", name, j, n);
                    bad++;
                }
                if (!bad && ca_guard_check(o[j], (size_t)n * sizeof(pixel), name))
                    bad++;
            }
        }
    }
    for (unsigned k = 0; k < sizeof(ns) / sizeof(ns[0]) && !bad; k++) {
        int n = ns[k];
        for (int tail = 0; tail <= 1; tail++) {
            ca_pg g0, g1, g2;
            pixel *s0 = ca_pg_alloc(&g0, (size_t)(2 * n) * sizeof(pixel), tail);
            pixel *s1 = ca_pg_alloc(&g1, (size_t)(2 * n + 1) * sizeof(pixel), tail);
            pixel *s2 = ca_pg_alloc(&g2, (size_t)(2 * n + 1) * sizeof(pixel), tail);
            ca_pg_fill_pix(&g0, (size_t)(2 * n));
            ca_pg_fill_pix(&g1, (size_t)(2 * n + 1));
            ca_pg_fill_pix(&g2, (size_t)(2 * n + 1));
            ca_pg_arm(name);
            kern(o[0], o[1], o[2], s0 - 1, s1, s2, n);
            ca_pg_disarm();
            y264_lr_hp_row_c(ref[0], ref[1], ref[2], s0 - 1, s1, s2, n);
            for (int j = 0; j < 3; j++)
                if (memcmp(o[j], ref[j], (size_t)n * sizeof(pixel))) {
                    if (!bad) ca_fail("%s: guarded plane %d n=%d", name, j, n);
                    bad++;
                }
            ca_pg_free(&g2);
            ca_pg_free(&g1);
            ca_pg_free(&g0);
        }
    }
    CA_BENCH2(name, kern(o[0], o[1], o[2], pa, pa + 4 * STRIDE, pb, NMAX),
                    y264_lr_hp_row_c(ref[0], ref[1], ref[2], pa, pa + 4 * STRIDE, pb, NMAX));
    for (int k = 0; k < 3; k++) ca_guard_free(o[k]);
    return bad;
}

typedef void (*hpel_band_fn)(pixel *, pixel *, pixel *, int, const pixel *, int,
                             int, int, int, uint8_t *, int, int);

/* One geometry of the whole-band half-pel build against the definitional
 * six-tap with clamped coordinates. The reference plane is mapped to exactly
 * pw x ph at its stride, because the kernel pads each row itself and may read
 * nothing else; the three outputs and the work memory are guarded, and the
 * output rows' stride padding must come back untouched. The bands are cut at
 * uneven rows so every ring phase starts one. */
static int hpel_band_geom(const char *name, hpel_band_fn kern, int pw, int ph,
                          int B, int trial)
{
    const int W = pw + 2 * B, RST = pw + 7, ST = W + 5, rows = ph + 2 * B;
    const size_t osz = (size_t)rows * ST * sizeof(pixel);
    const size_t msz = (size_t)6 * (W + 32) + (size_t)12 * W;
    const size_t win = ca_pg_window_pix(RST, pw, ph);
    int bad = 0;
    for (int tail = 0; tail <= 1 && !bad; tail++) {
        ca_pg g;
        pixel *refp = ca_pg_alloc(&g, win, tail);
        pixel *o[3];
        uint8_t *mem = ca_guard_alloc(msz);
        ca_pg_fill_pix(&g, win / sizeof(pixel));
        if (trial % 3 == 1)       /* extremes: the lane bounds are load-bearing */
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                    refp[(size_t)y * RST + x] = ca_rnd_edge();
        if (trial % 3 == 2)       /* the pattern that maximises the tap of taps */
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                    refp[(size_t)y * RST + x] =
                        (pixel)((((x % 6) == 2 || (x % 6) == 3) ^
                                 ((y % 6) == 2 || (y % 6) == 3)) ? 0 : PIXEL_MAX);
        for (int k = 0; k < 3; k++) {
            o[k] = ca_guard_alloc(osz);
            memset(o[k], CA_POISON, osz);
            ca_guard_poison(o[k], osz);
        }
        ca_guard_poison(mem, msz);
        size_t org = (size_t)B * ST + B;
        int cuts[6] = { -B, -B + 7, 3 < ph ? 3 : 1, ph > 8 ? ph - 4 : ph,
                        ph + 1, ph + B };
        ca_pg_arm(name);
        for (int b = 0; b + 1 < 6; b++)
            if (cuts[b] < cuts[b + 1])
                kern(o[0] + org, o[1] + org, o[2] + org, ST, refp, RST, pw, ph, B,
                     mem, cuts[b], cuts[b + 1]);
        ca_pg_disarm();
#define HCL(v, n) ((v) < 0 ? 0 : (v) >= (n) ? (n) - 1 : (v))
#define HR(x, y) refp[(size_t)HCL(y, ph) * RST + HCL(x, pw)]
        for (int y = -B; y < ph + B && bad < 4; y++)
            for (int x = -B; x < pw + B; x++) {
                int hv = HCLIP((HTAP6(HR(x-2,y), HR(x-1,y), HR(x,y), HR(x+1,y),
                                      HR(x+2,y), HR(x+3,y)) + 16) >> 5);
                int vv = HCLIP((HTAP6(HR(x,y-2), HR(x,y-1), HR(x,y), HR(x,y+1),
                                      HR(x,y+2), HR(x,y+3)) + 16) >> 5);
                int cc[6];
                for (int rr = -2; rr <= 3; rr++)
                    cc[rr + 2] = HTAP6(HR(x-2,y+rr), HR(x-1,y+rr), HR(x,y+rr),
                                       HR(x+1,y+rr), HR(x+2,y+rr), HR(x+3,y+rr));
                int cv = HCLIP((HTAP6(cc[0], cc[1], cc[2], cc[3], cc[4], cc[5]) + 512) >> 10);
                size_t q = org + (size_t)y * ST + x;
                if (o[0][q] != hv || o[1][q] != vv || o[2][q] != cv) {
                    if (!bad)
                        ca_fail("%s %dx%d b%d at (%d,%d): H %d/%d V %d/%d C %d/%d",
                                name, pw, ph, B, x, y, o[0][q], hv, o[1][q], vv,
                                o[2][q], cv);
                    bad++;
                }
            }
#undef HCL
#undef HR
        for (int k = 0; k < 3 && !bad; k++) {
            if (ca_guard_check(o[k], osz, name)) bad++;
            /* every row was written W wide; the rest of the stride is padding */
            const uint8_t *pad = (const uint8_t *)o[k];
            for (int y = 0; y < rows && !bad; y++)
                for (int x = W; x < ST; x++)
                    if (pad[((size_t)y * ST + x) * sizeof(pixel)] != CA_POISON) {
                        ca_fail("%s %dx%d b%d: wrote row %d padding", name, pw, ph, B, y);
                        bad++;
                        break;
                    }
        }
        if (!bad && ca_guard_check(mem, msz, name)) bad++;
        for (int k = 2; k >= 0; k--) ca_guard_free(o[k]);
        ca_guard_free(mem);
        ca_pg_free(&g);
    }
    return bad;
}

/* W = pw + 2 * border at the sixteen-column floor, at a multiple of the step
 * and at two widths that leave a step-back tail, each on random, extreme and
 * worst-case planes. */
static int run_hpel_band(const char *name, hpel_band_fn kern)
{
    static const int geo[][3] = { { 8, 9, 4 }, { 64, 20, 16 }, { 61, 37, 32 },
                                  { 23, 6, 5 } };
    int bad = 0;
    for (int t = 0; t < 6 && !bad; t++)
        for (unsigned k = 0; k < sizeof(geo) / sizeof(geo[0]) && !bad; k++)
            bad += hpel_band_geom(name, kern, geo[k][0], geo[k][1], geo[k][2], t);
    return bad;
}

#endif /* Y264_HAVE_SSE4 */

/* ---- the rows, one thin function per tier -------------------------------- */

#if Y264_HAVE_NEON

static int t_pred_copy_neon(void)
{ return run_pred_copy("pred_copy", y264_pred_copy_neon); }
static int t_pred_avg2_neon(void)
{ return run_pred_avg2("pred_avg2", y264_pred_avg2_neon); }
static int t_pixel_avg_wt_neon(void)
{ return run_pixel_avg_wt("pixel_avg_wt", y264_pixel_avg_wt_neon); }
static int t_mc_chroma_bi8_neon(void)
{ return run_mc_chroma_bi8("mc_chroma_bi8", y264_mc_chroma_bi8_neon); }
static int t_avg_mb420_flat_neon(void)
{ return run_avg_mb420_flat("avg_mb420_flat", y264_avg_mb420_flat_neon); }

static int t_bilin4_row_neon(void)
{ return run_bilin4_row("bilin4_row", y264_bilin4_row_neon); }

static int t_pixel_avg_wt_2d_neon(void)
{ return run_pixel_avg_wt_2d("pixel_avg_wt_2d", y264_pixel_avg_wt_2d_neon); }
/* The NEON 8-wide form takes one sixteen-byte load from ix-2, so it declares
 * five columns past the block where the 16-wide form declares two. */
static int t_mc_luma_win_neon(void)
{ return run_mc_luma_win("mc_luma_win", y264_mc_luma_neon16,
                         y264_mc_luma_neon8, 2, 5); }
static int t_mc_chroma_win_neon(void)
{ return run_mc_chroma_win("mc_chroma_win", y264_mc_chroma_neon_w8h,
                           y264_mc_chroma_neon_w4h, y264_mc_chroma_neon8); }
static int t_hpel_rows_neon(void)
{ return run_hpel_rows("hpel_rows", y264_hpel_hrow_neon, y264_hpel_outrow_neon); }

#endif /* Y264_HAVE_NEON */

#if Y264_HAVE_SSE4

static int t_pred_copy_sse4(void)
{ return run_pred_copy("pred_copy_sse4", y264_pred_copy_sse4); }
static int t_pred_avg2_sse4(void)
{ return run_pred_avg2("pred_avg2_sse4", y264_pred_avg2_sse4); }
static int t_pixel_avg_wt_sse4(void)
{ return run_pixel_avg_wt("pixel_avg_wt_sse4", y264_pixel_avg_wt_sse4); }
static int t_mc_luma_win_sse4(void)
{ return run_mc_luma_win("mc_luma_win_sse4", y264_mc_luma16_sse4,
                         y264_mc_luma8_sse4, 2, 2); }
static int t_mc_chroma_win_sse4(void)
{ return run_mc_chroma_win("mc_chroma_win_sse4", y264_mc_chroma_w8h_sse4,
                           y264_mc_chroma_w4h_sse4, NULL); }
static int t_hpel_rows_sse4(void)
{ return run_hpel_rows("hpel_rows_sse4", y264_hpel_hrow_sse4,
                       y264_hpel_outrow_sse4); }
static int t_hpel_band_sse4(void)
{ return run_hpel_band("hpel_band_sse4", y264_hpel_build_band_sse4); }
static int t_bilin4_row_sse4(void)
{ return run_bilin4_row("bilin4_row_sse4", y264_bilin4_row_sse4); }
static int t_lr_hp_row_sse4(void)
{ return run_lr_hp_row("lr_hp_row_sse4", y264_lr_hp_row_sse4); }
static int t_pixel_avg_wt_2d_sse4(void)
{ return run_pixel_avg_wt_2d("pixel_avg_wt_2d_sse4", y264_pixel_avg_wt_2d_sse4); }
static int t_mc_chroma_bi8_sse4(void)
{ return run_mc_chroma_bi8("mc_chroma_bi8_sse4", y264_mc_chroma_bi8_sse4); }
static int t_avg_mb420_flat_sse4(void)
{ return run_avg_mb420_flat("avg_mb420_flat_sse4", y264_avg_mb420_flat_sse4); }

#endif /* Y264_HAVE_SSE4 */

#if Y264_HAVE_AVX2

static int t_pred_copy_avx2(void)
{ return run_pred_copy("pred_copy_avx2", y264_pred_copy_avx2); }
static int t_pred_avg2_avx2(void)
{ return run_pred_avg2("pred_avg2_avx2", y264_pred_avg2_avx2); }
static int t_pixel_avg_wt_avx2(void)
{ return run_pixel_avg_wt("pixel_avg_wt_avx2", y264_pixel_avg_wt_avx2); }
static int t_mc_luma_win_avx2(void)
{ return run_mc_luma_win("mc_luma_win_avx2", y264_mc_luma16_avx2,
                         y264_mc_luma8_avx2, 2, 2); }
static int t_mc_chroma_win_avx2(void)
{ return run_mc_chroma_win("mc_chroma_win_avx2", y264_mc_chroma_w8h_avx2,
                           y264_mc_chroma_w4h_avx2, NULL); }
static int t_hpel_rows_avx2(void)
{ return run_hpel_rows("hpel_rows_avx2", y264_hpel_hrow_avx2,
                       y264_hpel_outrow_avx2); }
static int t_hpel_band_avx2(void)
{ return run_hpel_band("hpel_band_avx2", y264_hpel_build_band_avx2); }
static int t_bilin4_row_avx2(void)
{ return run_bilin4_row("bilin4_row_avx2", y264_bilin4_row_avx2); }
static int t_lr_hp_row_avx2(void)
{ return run_lr_hp_row("lr_hp_row_avx2", y264_lr_hp_row_avx2); }
static int t_pixel_avg_wt_2d_avx2(void)
{ return run_pixel_avg_wt_2d("pixel_avg_wt_2d_avx2", y264_pixel_avg_wt_2d_avx2); }
static int t_mc_chroma_bi8_avx2(void)
{ return run_mc_chroma_bi8("mc_chroma_bi8_avx2", y264_mc_chroma_bi8_avx2); }
static int t_avg_mb420_flat_avx2(void)
{ return run_avg_mb420_flat("avg_mb420_flat_avx2", y264_avg_mb420_flat_avx2); }

#endif /* Y264_HAVE_AVX2 */

/* ---- the oracles ---------------------------------------------------------- */

/* Luma MC over every partition shape x all sixteen phases x positions that
 * force each of the three paths: the kernel window, the in-border C body, and
 * the out-of-window tile gather (a block whose integer position is past the
 * plane's replicated border, which is 55% of the calls on a CIF clip). The
 * border replication above is what makes reading past the frame equal the
 * spec's coordinate clamp, exactly as the encoder's reference planes do. */
static int t_mc_luma_oracle(void)
{
    static const struct { int w, h; } msh[] = {
        { 16, 16 }, { 16, 8 }, { 8, 16 }, { 8, 8 }, { 8, 4 }, { 4, 8 }, { 4, 4 },
    };
    static const int off[] = { 0, -20, -29, -30, -31, -40, -80, 20, 29, 31, 40, 80 };
    int bad = 0, n = 0;
    pixel d1[16 * 16], d2[16 * 16];

    mplane();

    for (unsigned si = 0; si < sizeof(msh) / sizeof(msh[0]); si++)
        for (int ph = 0; ph < 16; ph++)
            for (unsigned ox = 0; ox < sizeof(off) / sizeof(off[0]); ox++)
                for (unsigned oy = 0; oy < sizeof(off) / sizeof(off[0]); oy++) {
                    int mvx = off[ox] * 4 + (ph & 3);
                    int mvy = off[oy] * 4 + (ph >> 2);
                    memset(d1, 1, sizeof(d1));
                    memset(d2, 2, sizeof(d2));
                    y264_mc_luma(d1, 16, mref, MST, MW, MH, 16, 16,
                                 mvx, mvy, msh[si].w, msh[si].h);
                    y264_mc_luma_c(d2, 16, mref, MST, MW, MH, 16, 16,
                                   mvx, mvy, msh[si].w, msh[si].h);
                    n++;
                    for (int y = 0; y < msh[si].h; y++)
                        if (memcmp(d1 + y * 16, d2 + y * 16,
                                   (size_t)msh[si].w * sizeof(pixel))) {
                            if (!bad)
                                ca_fail("mc_luma: %dx%d mv %d,%d",
                                        msh[si].w, msh[si].h, mvx, mvy);
                            bad++;
                            break;
                        }
                }
    (void)n;
    return bad;
}

/* Chroma MC over every partition-derived block shape and chroma format axis
 * split, at random in-window motion vectors. */
static int t_mc_chroma_oracle(void)
{
    static const struct { int w, h; } shp[] = {
        { 8, 8 }, { 8, 4 }, { 8, 16 }, { 4, 8 }, { 4, 4 }, { 4, 2 }, { 2, 4 }, { 2, 2 },
    };
    static const struct { int sw, sh; } fmt[] = { { 2, 2 }, { 2, 1 }, { 1, 1 } };
    pixel d1[16 * 16], d2[16 * 16];
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        plane();
        const pixel *rf = pa + 4 * STRIDE + 12;
        for (unsigned si = 0; si < sizeof(shp) / sizeof(shp[0]); si++)
            for (unsigned fi = 0; fi < sizeof(fmt) / sizeof(fmt[0]); fi++) {
                int mvx = (int)ca_below(33) - 16;
                int mvy = (int)ca_below(33) - 16;
                memset(d1, 1, sizeof(d1));
                memset(d2, 2, sizeof(d2));
                y264_mc_chroma(d1, 16, rf, STRIDE, 40, 24, 8, 8, mvx, mvy,
                               shp[si].w, shp[si].h, fmt[fi].sw, fmt[fi].sh);
                y264_mc_chroma_c(d2, 16, rf, STRIDE, 40, 24, 8, 8, mvx, mvy,
                                 shp[si].w, shp[si].h, fmt[fi].sw, fmt[fi].sh);
                for (int y = 0; y < shp[si].h; y++)
                    if (memcmp(d1 + y * 16, d2 + y * 16,
                               (size_t)shp[si].w * sizeof(pixel))) {
                        if (!bad)
                            ca_fail("mc_chroma: %dx%d fmt %d%d mv %d,%d",
                                    shp[si].w, shp[si].h, fmt[fi].sw,
                                    fmt[fi].sh, mvx, mvy);
                        bad++;
                        break;
                    }
            }
    }
    return bad;
}

/* The plane-read predictor against the definitional six-tap path. The four
 * planes are the C builder's own output, so this is the claim the routing rests
 * on stated as a test: for every quarter-pel phase and every partition shape,
 * a copy or a 2-tap average of the cached planes IS y264_mc_luma_c's block.
 *
 * Positions run from the far border to the far border, because the plane read
 * has no clamp of its own -- the border replication the builder wrote is the
 * clamp, and a phase that reaches a row or column further on (n/p/q/r, c/g/k/r)
 * is exactly where an off-by-one would hide.
 *
 * The page-guard pass is separate and uses random maps: values do not matter
 * there, only that each plane's read stops at the window the kernel declares --
 * w x h from (ix,iy), one row down for fy==3, one column right for fx==3. */
static int t_mc_luma_hp(void)
{
    enum { HPW = 64, HPH = 48, HB = 8, HST = HPW + 2 * HB,
           DS = 24 };                         /* wider dst: padding is checked */
    static pixel Gb[(HPH + 2 * HB) * HST], Hb[(HPH + 2 * HB) * HST],
                 Vb[(HPH + 2 * HB) * HST], Cb[(HPH + 2 * HB) * HST];
    static int32_t scratch[HST * (HPH + 2 * HB + 5)];
    static const struct { int w, h; } shp[] = {
        { 16, 16 }, { 16, 8 }, { 8, 16 }, { 8, 8 }, { 8, 4 }, { 4, 8 }, { 4, 4 },
    };
    size_t org = (size_t)HB * HST + HB;
    pixel *G = Gb + org, *H = Hb + org, *V = Vb + org, *C = Cb + org;
    pixel *d1 = ca_guard_alloc(DS * 16 * sizeof(pixel));
    pixel d2[16 * 16];
    int bad = 0;

    for (int t = 0; t < TRIALS / 4 + 1 && !bad; t++) {
        for (int y = 0; y < HPH; y++)
            ca_fill(G + (size_t)y * HST, HPW);
        replicate(G, HST, HPW, HPH, HB);
        y264_mc_build_hpel(H, V, C, HST, G, HST, HPW, HPH, HB, scratch, HST);

        for (unsigned si = 0; si < sizeof(shp) / sizeof(shp[0]) && !bad; si++) {
            int w = shp[si].w, h = shp[si].h;
            /* Every position whose read window -- the block plus the one row
             * and column the odd phases reach -- is inside the built extent. */
            const int px[] = { -HB, -HB + 1, -3, 0, 1, 7, HPW - w, HPW,
                               HPW + HB - w - 1 };
            const int py[] = { -HB, -HB + 1, -3, 0, 1, 5, HPH - h, HPH,
                               HPH + HB - h - 1 };
            for (unsigned oy = 0; oy < sizeof(py) / sizeof(py[0]) && !bad; oy++)
                for (unsigned ox = 0; ox < sizeof(px) / sizeof(px[0]) && !bad; ox++)
                    for (int qi = 0; qi < 16; qi++) {
                        int fx = qi & 3, fy = qi >> 2;
                        int ix = px[ox], iy = py[oy];
                        if (ix < -HB || iy < -HB ||
                            ix + w + 1 > HPW + HB || iy + h + 1 > HPH + HB)
                            continue;
                        memset(d1, CA_POISON, DS * 16 * sizeof(pixel));
                        ca_guard_poison(d1, DS * 16 * sizeof(pixel));
                        memset(d2, CA_POISON, sizeof(d2));
                        y264_mc_luma_hp(d1, DS, G, H, V, C, HST,
                                        ix, iy, fx, fy, w, h);
                        y264_mc_luma_c(d2, 16, G, HST, HPW, HPH, ix, iy,
                                       fx, fy, w, h);
                        for (int y = 0; y < h; y++)
                            if (memcmp(d1 + (size_t)y * DS, d2 + y * 16,
                                       (size_t)w * sizeof(pixel))) {
                                ca_fail("mc_luma_hp: %dx%d phase %d,%d at (%d,%d)",
                                        w, h, fx, fy, ix, iy);
                                bad++;
                                break;
                            }
                        if (!bad &&
                            (ca_guard_check(d1, DS * 16 * sizeof(pixel), "mc_luma_hp") ||
                             ca_check_pad_pix(d1, DS, w, h, "mc_luma_hp")))
                            bad++;
                    }
        }
    }

    /* Each source plane reads exactly its declared window and no further. The
     * two live planes get their own mapping; the other two are never indexed,
     * so a stand-in pointer is enough to fill the argument. */
    for (unsigned si = 0; si < sizeof(shp) / sizeof(shp[0]) && !bad; si++) {
        int w = shp[si].w, h = shp[si].h;
        size_t win = ca_pg_window_pix(HST, w, h);
        for (int qi = 0; qi < 16; qi++) {
            int fx = qi & 3, fy = qi >> 2;
            int ia = y264_qpel_plane_a[qi], ib = y264_qpel_plane_b[qi];
            for (int tail = 0; tail <= 1; tail++) {
                ca_pg ga, gb;
                pixel dst[16 * 16];
                const pixel *pl[4] = { G, H, V, C };
                pixel *sa = ca_pg_alloc(&ga, win, tail);
                ca_pg_fill_pix(&ga, win / sizeof(pixel));
                pl[ia] = sa - (fy == 3 ? HST : 0);
                pixel *sb = NULL;
                if (qi & 5) {
                    sb = ca_pg_alloc(&gb, win, tail);
                    ca_pg_fill_pix(&gb, win / sizeof(pixel));
                    pl[ib] = sb - (fx == 3 ? 1 : 0);
                }
                ca_pg_arm("mc_luma_hp");
                y264_mc_luma_hp(dst, 16, pl[0], pl[1], pl[2], pl[3], HST,
                                0, 0, fx, fy, w, h);
                ca_pg_disarm();
                if (sb) ca_pg_free(&gb);
                ca_pg_free(&ga);
            }
        }
    }

    CA_BENCH2("mc_luma_hp 16x16 h2v2",
              y264_mc_luma_hp(d1, DS, G, H, V, C, HST, 8, 8, 2, 2, 16, 16),
              y264_mc_luma_c(d2, 16, G, HST, HPW, HPH, 8, 8, 2, 2, 16, 16));
    CA_BENCH2("mc_luma_hp 16x16 h1v1",
              y264_mc_luma_hp(d1, DS, G, H, V, C, HST, 8, 8, 1, 1, 16, 16),
              y264_mc_luma_c(d2, 16, G, HST, HPW, HPH, 8, 8, 1, 1, 16, 16));
    ca_guard_free(d1);
    return bad;
}

/* The whole half-pel plane against the definitional clamped 6-tap formulas, at
 * every bordered position. Recon path: it must be exact. */
static int t_hpel_build(void)
{
    enum { HPW = 64, HPH = 48, HB = 8, HPAD = HB + 8,
           HRST = HPW + 2 * HPAD, HST = HPW + 2 * HB };
    static pixel refbuf[(HPH + 2 * HPAD) * HRST];
    static pixel Hp[(HPH + 2 * HB) * HST], Vp[(HPH + 2 * HB) * HST],
                 Cp[(HPH + 2 * HB) * HST];
    static int32_t scratch[HST * (HPH + 2 * HB + 5)];
    pixel *refp = refbuf + (size_t)HPAD * HRST + HPAD;
    size_t org = (size_t)HB * HST + HB;
    int bad = 0;

    for (int y = 0; y < HPH; y++)
        ca_fill(refp + (size_t)y * HRST, HPW);
    replicate(refp, HRST, HPW, HPH, HPAD);
    y264_mc_build_hpel(Hp + org, Vp + org, Cp + org, HST,
                       refp, HRST, HPW, HPH, HB, scratch, HST);
#define HCL(v, n) ((v) < 0 ? 0 : (v) >= (n) ? (n) - 1 : (v))
#define HR(x, y) refp[(size_t)HCL(y, HPH) * HRST + HCL(x, HPW)]
    for (int y = -HB; y < HPH + HB && bad < 4; y++)
        for (int x = -HB; x < HPW + HB; x++) {
            int hv = HCLIP((HTAP6(HR(x-2,y), HR(x-1,y), HR(x,y), HR(x+1,y),
                                  HR(x+2,y), HR(x+3,y)) + 16) >> 5);
            int vv = HCLIP((HTAP6(HR(x,y-2), HR(x,y-1), HR(x,y), HR(x,y+1),
                                  HR(x,y+2), HR(x,y+3)) + 16) >> 5);
            int cc[6];
            for (int rr = -2; rr <= 3; rr++)
                cc[rr + 2] = HTAP6(HR(x-2,y+rr), HR(x-1,y+rr), HR(x,y+rr),
                                   HR(x+1,y+rr), HR(x+2,y+rr), HR(x+3,y+rr));
            int cv = HCLIP((HTAP6(cc[0], cc[1], cc[2], cc[3], cc[4], cc[5]) + 512) >> 10);
            size_t o = org + (size_t)y * HST + x;
            if (Hp[o] != hv || Vp[o] != vv || Cp[o] != cv) {
                if (!bad)
                    ca_fail("hpel_build at (%d,%d): H %d/%d V %d/%d C %d/%d",
                            x, y, Hp[o], hv, Vp[o], vv, Cp[o], cv);
                bad++;
            }
        }
#undef HCL
#undef HR
    return bad;
}

/* The same check at the encoder's 32-sample border, an odd width, and the
 * build split into row bands the way the pool runs it (each band with its
 * own scratch sized for that band), so the whole-band path meets its clamps
 * at every edge and its band seams. */
static int t_hpel_build_bands(void)
{
    enum { HPW = 61, HPH = 37, HB = 32, HPAD = HB + 8,
           HRST = HPW + 2 * HPAD + 3, HST = HPW + 2 * HB + 5 };
    static pixel refbuf[(HPH + 2 * HPAD) * HRST];
    static pixel Hp[(HPH + 2 * HB) * HST], Vp[(HPH + 2 * HB) * HST],
                 Cp[(HPH + 2 * HB) * HST];
    static int32_t scratch[HST * (HPH + 2 * HB + 5)];
    pixel *refp = refbuf + (size_t)HPAD * HRST + HPAD;
    size_t org = (size_t)HB * HST + HB;
    int bad = 0;
    for (int y = 0; y < HPH; y++)
        ca_fill(refp + (size_t)y * HRST, HPW);
    replicate(refp, HRST, HPW, HPH, HPAD);
    static const int cuts[] = { -HB, -HB + 7, 3, 4, 20, HPH + 1, HPH + HB };
    for (int b = 0; b + 1 < (int)(sizeof cuts / sizeof cuts[0]); b++)
        y264_mc_build_hpel_rows(Hp + org, Vp + org, Cp + org, HST, refp, HRST, HPW, HPH,
                                HB, scratch, HST, cuts[b], cuts[b + 1]);
#define HCL(v, n) ((v) < 0 ? 0 : (v) >= (n) ? (n) - 1 : (v))
#define HR(x, y) refp[(size_t)HCL(y, HPH) * HRST + HCL(x, HPW)]
    for (int y = -HB; y < HPH + HB && bad < 4; y++)
        for (int x = -HB; x < HPW + HB; x++) {
            int hv = HCLIP((HTAP6(HR(x-2,y), HR(x-1,y), HR(x,y), HR(x+1,y),
                                  HR(x+2,y), HR(x+3,y)) + 16) >> 5);
            int vv = HCLIP((HTAP6(HR(x,y-2), HR(x,y-1), HR(x,y), HR(x,y+1),
                                  HR(x,y+2), HR(x,y+3)) + 16) >> 5);
            int cc[6];
            for (int rr = -2; rr <= 3; rr++)
                cc[rr + 2] = HTAP6(HR(x-2,y+rr), HR(x-1,y+rr), HR(x,y+rr),
                                   HR(x+1,y+rr), HR(x+2,y+rr), HR(x+3,y+rr));
            int cv = HCLIP((HTAP6(cc[0], cc[1], cc[2], cc[3], cc[4], cc[5]) + 512) >> 10);
            size_t o = org + (size_t)y * HST + x;
            if (Hp[o] != hv || Vp[o] != vv || Cp[o] != cv) {
                if (!bad)
                    ca_fail("hpel_build_bands at (%d,%d): H %d/%d V %d/%d C %d/%d",
                            x, y, Hp[o], hv, Vp[o], vv, Cp[o], cv);
                bad++;
            }
        }
#undef HCL
#undef HR
    return bad;
}

#undef HTAP6
#undef HCLIP


/* The C chroma MC's in-window path and the C weighted averages against their
 * plain int formulas: chroma at every phase and width 2/4/8 on random and
 * all-max planes, the averages across the whole implicit weight domain plus
 * weights outside it (which must take the int form). */
static int t_mc_c_forms(void)
{
    enum { CW = 40, CH = 24, CB = Y264_CHROMA_BORDER, CST = CW + 2 * CB };
    static pixel cbuf[(CH + 2 * CB) * CST];
    pixel *cref = cbuf + (size_t)CB * CST + CB;
    int bad = 0;
    for (int t = 0; t < TRIALS; t++) {
        for (int y = 0; y < CH; y++) {
            ca_fill(cref + (size_t)y * CST, CW);
            if (t % 3 == 1) for (int x = 0; x < CW; x++) cref[(size_t)y * CST + x] = PIXEL_MAX;
        }
        replicate(cref, CST, CW, CH, CB);
        for (int w = 2; w <= 8; w *= 2)
            for (int fx = 0; fx < 8; fx++)
                for (int fy = 0; fy < 8; fy++) {
                    pixel d[16 * 8];
                    int cbx = 8, cby = 4, h = 8;
                    y264_mc_chroma_c(d, 16, cref, CST, CW, CH, cbx, cby, fx, fy, w, h, 2, 2);
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++) {
                            const pixel *r = cref + (size_t)(cby + y) * CST + cbx + x;
                            int e = ((8 - fx) * (8 - fy) * r[0] + fx * (8 - fy) * r[1] +
                                     (8 - fx) * fy * r[CST] + fx * fy * r[CST + 1] + 32) >> 6;
                            if (d[y * 16 + x] != e) {
                                if (!bad) ca_fail("mc_chroma_c: %dx%d phase %d,%d", w, h, fx, fy);
                                bad++;
                            }
                        }
                }
    }
    plane();
    static const int odd[][2] = { { 100, -40 }, { -100, 160 }, { 70, 70 }, { 200, -136 } };
    for (int t = 0; t < TRIALS; t++) {
        plane();
        if (t % 3 == 1) for (int i = 0; i < 256; i++) { pa[i] = PIXEL_MAX; pb[i] = 0; }
        if (t % 3 == 2) for (int i = 0; i < 256; i++) { pa[i] = 0; pb[i] = PIXEL_MAX; }
        for (int k = -64 - 4; k <= 128 + 4; k++) {
            int w0, w1;
            if (k < -64)      { w0 = odd[-64 - k - 1][0]; w1 = odd[-64 - k - 1][1]; }
            else if (k > 128) { w0 = odd[k - 129][0];     w1 = odd[k - 129][1]; }
            else              { w0 = k; w1 = 64 - k; }
            pixel d1[64], d2[8 * 16];
            y264_pixel_avg_wt_c(d1, pa, pb, 61, w0, w1);
            y264_pixel_avg_wt_2d_c(d2, 16, pa, 16, pb, 16, 12, 5, w0, w1);
            for (int i = 0; i < 61; i++) {
                int v = (pa[i] * w0 + pb[i] * w1 + 32) >> 6;
                v = v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v);
                if (d1[i] != v) { if (!bad) ca_fail("pixel_avg_wt_c: w %d,%d", w0, w1); bad++; break; }
            }
            for (int y = 0; y < 5; y++)
                for (int x = 0; x < 12; x++) {
                    int v = (pa[y * 16 + x] * w0 + pb[y * 16 + x] * w1 + 32) >> 6;
                    v = v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v);
                    if (d2[y * 16 + x] != v) {
                        if (!bad) ca_fail("pixel_avg_wt_2d_c: w %d,%d", w0, w1);
                        bad++;
                    }
                }
        }
    }
    return bad;
}

const ca_test ca_mc_tests[] = {
#if Y264_HAVE_NEON
    { "pred_copy",         "mc", Y264_CPU_NEON, t_pred_copy_neon },
    { "pred_avg2",         "mc", Y264_CPU_NEON, t_pred_avg2_neon },
    { "pixel_avg_wt",      "mc", Y264_CPU_NEON, t_pixel_avg_wt_neon },
    { "pixel_avg_wt_2d",   "mc", Y264_CPU_NEON, t_pixel_avg_wt_2d_neon },
    { "mc_chroma_bi8",     "mc", Y264_CPU_NEON, t_mc_chroma_bi8_neon },
    { "bilin4_row",        "mc", Y264_CPU_NEON, t_bilin4_row_neon },
    { "avg_mb420_flat",    "mc", Y264_CPU_NEON, t_avg_mb420_flat_neon },
    { "mc_luma_win",       "mc", Y264_CPU_NEON, t_mc_luma_win_neon },
    { "mc_chroma_win",     "mc", Y264_CPU_NEON, t_mc_chroma_win_neon },
    { "hpel_rows",         "mc", Y264_CPU_NEON, t_hpel_rows_neon },
#endif
#if Y264_HAVE_SSE4
    { "pred_copy_sse4",    "mc", Y264_CPU_SSE4_ALL, t_pred_copy_sse4 },
    { "pred_avg2_sse4",    "mc", Y264_CPU_SSE4_ALL, t_pred_avg2_sse4 },
    { "pixel_avg_wt_sse4", "mc", Y264_CPU_SSE4_ALL, t_pixel_avg_wt_sse4 },
    { "mc_luma_win_sse4",  "mc", Y264_CPU_SSE4_ALL, t_mc_luma_win_sse4 },
    { "mc_chroma_win_sse4","mc", Y264_CPU_SSE4_ALL, t_mc_chroma_win_sse4 },
    { "hpel_rows_sse4",    "mc", Y264_CPU_SSE4_ALL, t_hpel_rows_sse4 },
    { "hpel_band_sse4",    "mc", Y264_CPU_SSE4_ALL, t_hpel_band_sse4 },
    { "bilin4_row_sse4",   "mc", Y264_CPU_SSE4_ALL, t_bilin4_row_sse4 },
    { "lr_hp_row_sse4",    "mc", Y264_CPU_SSE4_ALL, t_lr_hp_row_sse4 },
    { "pixel_avg_wt_2d_sse4", "mc", Y264_CPU_SSE4_ALL, t_pixel_avg_wt_2d_sse4 },
    { "mc_chroma_bi8_sse4", "mc", Y264_CPU_SSE4_ALL, t_mc_chroma_bi8_sse4 },
    { "avg_mb420_flat_sse4", "mc", Y264_CPU_SSE4_ALL, t_avg_mb420_flat_sse4 },
#endif
#if Y264_HAVE_AVX2
    { "pred_copy_avx2",    "mc", Y264_CPU_AVX2_ALL, t_pred_copy_avx2 },
    { "pred_avg2_avx2",    "mc", Y264_CPU_AVX2_ALL, t_pred_avg2_avx2 },
    { "pixel_avg_wt_avx2", "mc", Y264_CPU_AVX2_ALL, t_pixel_avg_wt_avx2 },
    { "mc_luma_win_avx2",  "mc", Y264_CPU_AVX2_ALL, t_mc_luma_win_avx2 },
    { "mc_chroma_win_avx2","mc", Y264_CPU_AVX2_ALL, t_mc_chroma_win_avx2 },
    { "hpel_rows_avx2",    "mc", Y264_CPU_AVX2_ALL, t_hpel_rows_avx2 },
    { "hpel_band_avx2",    "mc", Y264_CPU_AVX2_ALL, t_hpel_band_avx2 },
    { "bilin4_row_avx2",   "mc", Y264_CPU_AVX2_ALL, t_bilin4_row_avx2 },
    { "lr_hp_row_avx2",    "mc", Y264_CPU_AVX2_ALL, t_lr_hp_row_avx2 },
    { "pixel_avg_wt_2d_avx2", "mc", Y264_CPU_AVX2_ALL, t_pixel_avg_wt_2d_avx2 },
    { "mc_chroma_bi8_avx2", "mc", Y264_CPU_AVX2_ALL, t_mc_chroma_bi8_avx2 },
    { "avg_mb420_flat_avx2", "mc", Y264_CPU_AVX2_ALL, t_avg_mb420_flat_avx2 },
#endif
    { "mc_luma",      "mc", 0, t_mc_luma_oracle },
    { "mc_luma_hp",   "mc", 0, t_mc_luma_hp },
    { "mc_chroma",    "mc", 0, t_mc_chroma_oracle },
    { "hpel_build",   "mc", 0, t_hpel_build },
    { "hpel_build_bands", "mc", 0, t_hpel_build_bands },
    { "mc_c_forms",      "mc", 0,                 t_mc_c_forms },
    { NULL, "mc", 0, NULL },
};
