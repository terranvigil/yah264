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
#include "dsp/mc.h"

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
             * the reference's plane filter is priced per 16x16 tile, so the
             * record converts. */
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

/* ---- the rows, one thin function per tier -------------------------------- */

#if Y264_HAVE_NEON

static int t_pred_copy_neon(void)
{ return run_pred_copy("pred_copy", y264_pred_copy_neon); }
static int t_pred_avg2_neon(void)
{ return run_pred_avg2("pred_avg2", y264_pred_avg2_neon); }
static int t_pixel_avg_wt_neon(void)
{ return run_pixel_avg_wt("pixel_avg_wt", y264_pixel_avg_wt_neon); }
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

#undef HTAP6
#undef HCLIP


const ca_test ca_mc_tests[] = {
#if Y264_HAVE_NEON
    { "pred_copy",         "mc", Y264_CPU_NEON, t_pred_copy_neon },
    { "pred_avg2",         "mc", Y264_CPU_NEON, t_pred_avg2_neon },
    { "pixel_avg_wt",      "mc", Y264_CPU_NEON, t_pixel_avg_wt_neon },
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
#endif
#if Y264_HAVE_AVX2
    { "pred_copy_avx2",    "mc", Y264_CPU_AVX2_ALL, t_pred_copy_avx2 },
    { "pred_avg2_avx2",    "mc", Y264_CPU_AVX2_ALL, t_pred_avg2_avx2 },
    { "pixel_avg_wt_avx2", "mc", Y264_CPU_AVX2_ALL, t_pixel_avg_wt_avx2 },
    { "mc_luma_win_avx2",  "mc", Y264_CPU_AVX2_ALL, t_mc_luma_win_avx2 },
    { "mc_chroma_win_avx2","mc", Y264_CPU_AVX2_ALL, t_mc_chroma_win_avx2 },
    { "hpel_rows_avx2",    "mc", Y264_CPU_AVX2_ALL, t_hpel_rows_avx2 },
#endif
    { "mc_luma",      "mc", 0, t_mc_luma_oracle },
    { "mc_luma_hp",   "mc", 0, t_mc_luma_hp },
    { "mc_chroma",    "mc", 0, t_mc_chroma_oracle },
    { "hpel_build",   "mc", 0, t_hpel_build },
    { NULL, "mc", 0, NULL },
};
