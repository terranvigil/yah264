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

#if Y264_HAVE_NEON

/* ---- the prediction fetches ---------------------------------------------- */

/* The destination is written at a stride wider than the block on purpose: the
 * row padding is poisoned and checked, which is the net for a kernel that
 * stores a whole vector into a narrower block. */
static int t_pred_copy(void)
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
                y264_pred_copy_neon(d1, DS, pa + off, ss, w, h);
                y264_pred_copy_c(d2, DS, pa + off, ss, w, h);
                if (memcmp(d1, d2, DS * 16 * sizeof(pixel))) {
                    if (!bad) ca_fail("pred_copy: w=%d h=%d", w, h);
                    bad++;
                }
                if (!bad && (ca_guard_check(d1, DS * 16 * sizeof(pixel), "pred_copy") ||
                             ca_check_pad_pix(d1, DS, w, h, "pred_copy")))
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
            ca_pg_arm("pred_copy");
            y264_pred_copy_neon(dst, 16, s, STRIDE, w, h);
            ca_pg_disarm();
            ca_pg_free(&g);
        }
    }
    CA_BENCH2("pred_copy 16x16", y264_pred_copy_neon(d1, DS, pa, STRIDE, 16, 16),
                                 y264_pred_copy_c(d2, DS, pa, STRIDE, 16, 16));
    ca_guard_free(d1);
    return bad;
}

static int t_pred_avg2(void)
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
                y264_pred_avg2_neon(d1, DS, pa + off, pb + off + 1, ss, w, h);
                y264_pred_avg2_c(d2, DS, pa + off, pb + off + 1, ss, w, h);
                if (memcmp(d1, d2, DS * 16 * sizeof(pixel))) {
                    if (!bad) ca_fail("pred_avg2: w=%d h=%d", w, h);
                    bad++;
                }
                if (!bad && (ca_guard_check(d1, DS * 16 * sizeof(pixel), "pred_avg2") ||
                             ca_check_pad_pix(d1, DS, w, h, "pred_avg2")))
                    bad++;
            }
    }
    CA_BENCH2("pred_avg2 16x16", y264_pred_avg2_neon(d1, DS, pa, pb, STRIDE, 16, 16),
                                 y264_pred_avg2_c(d2, DS, pa, pb, STRIDE, 16, 16));
    CA_BENCH2("pred_avg2 8x8", y264_pred_avg2_neon(d1, DS, pa, pb, STRIDE, 8, 8),
                               y264_pred_avg2_c(d2, DS, pa, pb, STRIDE, 8, 8));
    ca_guard_free(d1);
    return bad;
}

/* Every (w0, w1) the implicit weight derivation can produce -- w1 in
 * [-64, 128], w0 = 64 - w1 -- plus the unweighted (32, 32) fast path, over
 * every packed block length the encoder passes. The extreme weights are where
 * the lane bound and the saturating narrow are load-bearing. */
static int t_pixel_avg_wt(void)
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
                y264_pixel_avg_wt_neon(d1, pa, pb, len[k], w0, w1);
                y264_pixel_avg_wt_c(d2, pa, pb, len[k], w0, w1);
                if (memcmp(d1, d2, (size_t)len[k] * sizeof(pixel))) {
                    if (!bad) ca_fail("pixel_avg_wt: w0=%d w1=%d n=%d", w0, w1, len[k]);
                    bad++;
                }
                if (!bad && ca_guard_check(d1, (size_t)len[k] * sizeof(pixel),
                                           "pixel_avg_wt"))
                    bad++;
            }
        }
    }
    CA_BENCH2("pixel_avg 32/32", y264_pixel_avg_wt_neon(d1, pa, pb, 256, 32, 32),
                                 y264_pixel_avg_wt_c(d2, pa, pb, 256, 32, 32));
    CA_BENCH2("pixel_avg 21/43", y264_pixel_avg_wt_neon(d1, pa, pb, 256, 21, 43),
                                 y264_pixel_avg_wt_c(d2, pa, pb, 256, 21, 43));
    ca_guard_free(d1);
    return bad;
}

/* ---- the interpolation kernels ------------------------------------------- */

/* The window kernel, at positions the dispatcher would hand it: block inside
 * the plane, every sub-pel phase, both widths. The reference is y264_mc_luma_c
 * at the same motion vector, which is the fully-clamped definitional path. */
static int t_mc_luma_kernel(void)
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
                        y264_mc_luma_neon16(d1, 16, mref, MST, ix, iy,
                                            mvx & 3, mvy & 3, h);
                    else
                        y264_mc_luma_neon8(d1, 8, mref, MST, ix, iy,
                                           mvx & 3, mvy & 3, h);
                    y264_mc_luma_c(d2, 16, mref, MST, MW, MH, bx, by,
                                   mvx, mvy, w, h);
                    for (int y = 0; y < h; y++)
                        if (memcmp(d1 + (size_t)y * (w == 16 ? 16 : 8),
                                   d2 + (size_t)y * 16,
                                   (size_t)w * sizeof(pixel))) {
                            if (!bad)
                                ca_fail("mc_luma_neon%d: %dx%d phase %d,%d",
                                        w, w, h, mvx, mvy);
                            bad++;
                            break;
                        }
                }
    }
    return bad;
}

/* The chroma kernels, at the shapes the dispatcher routes to each of the three
 * (w == 4 any even h, w == 8 h == 8, w == 8 any other even h). */
static int t_mc_chroma_kernel(void)
{
    enum { CW = 40, CH = 24, CB = Y264_CHROMA_BORDER,
           CST = CW + 2 * CB };
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
                            y264_mc_chroma_neon_w4h(d1, 16, cref, CST, ix, iy,
                                                    mvx & 7, mvy & 7, h);
                        else if (h == 8)
                            y264_mc_chroma_neon8(d1, 16, cref, CST, ix, iy,
                                                 mvx & 7, mvy & 7);
                        else
                            y264_mc_chroma_neon_w8h(d1, 16, cref, CST, ix, iy,
                                                    mvx & 7, mvy & 7, h);
                        y264_mc_chroma_c(d2, 16, cref, CST, CW, CH, cbx, cby,
                                         mvx, mvy, w, h, 2, 2);
                        for (int y = 0; y < h; y++)
                            if (memcmp(d1 + (size_t)y * 16, d2 + (size_t)y * 16,
                                       (size_t)w * sizeof(pixel))) {
                                if (!bad)
                                    ca_fail("mc_chroma kernel: %dx%d phase %d,%d",
                                            w, h, fx, fy);
                                bad++;
                                break;
                            }
                    }
    }
    return bad;
}

/* ---- the half-pel plane rows ---------------------------------------------
 *
 * These two are the row kernels the plane builder calls over the interior
 * span, and they were only ever reachable through the whole-plane build
 * before. Both are checked against the definitional 6-tap at every column of
 * the span, which is what the builder's own C body computes. */
static int t_hpel_rows(void)
{
    enum { HW = 64, HPAD = 16, HRST = HW + 2 * HPAD };
    static pixel row[HRST], r[6][HRST];
    static int32_t s[6][HRST], sref[HRST];
    static pixel Hr[HRST], Vr[HRST], Cr[HRST];
    pixel *rw = row + HPAD;
    int bad = 0;
    for (int t = 0; t < TRIALS && !bad; t++) {
        int x0 = 2, x1 = HW - 3;
        ca_fill(row, HRST);
        for (int x = x0; x < x1; x++)
            sref[x] = HTAP6(rw[x-2], rw[x-1], rw[x], rw[x+1], rw[x+2], rw[x+3]);
        memset(s[0], 0x5a, sizeof(s[0]));
        y264_hpel_hrow_neon(s[0], rw, x0, x1);
        for (int x = x0; x < x1 && !bad; x++)
            if (s[0][x] != sref[x]) {
                ca_fail("hpel_hrow: x=%d opt=%d ref=%d", x, s[0][x], sref[x]);
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
        y264_hpel_outrow_neon(Hr, Vr, Cr, s[0], s[1], s[2], s[3], s[4], s[5],
                              r[0], r[1], r[2], r[3], r[4], r[5], x0, x1);
        for (int x = x0; x < x1 && !bad; x++) {
            int hv = HCLIP((s[2][x] + 16) >> 5);
            int cv = HCLIP((HTAP6(s[0][x], s[1][x], s[2][x], s[3][x], s[4][x],
                                  s[5][x]) + 512) >> 10);
            int vv = HCLIP((HTAP6(r[0][x], r[1][x], r[2][x], r[3][x], r[4][x],
                                  r[5][x]) + 16) >> 5);
            if (Hr[x] != hv || Vr[x] != vv || Cr[x] != cv) {
                ca_fail("hpel_outrow: x=%d H %d/%d V %d/%d C %d/%d", x,
                        Hr[x], hv, Vr[x], vv, Cr[x], cv);
                bad++;
            }
        }
    }
    return bad;
}

#endif /* Y264_HAVE_NEON */

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
    { "pred_copy",    "mc", Y264_CPU_NEON, t_pred_copy },
    { "pred_avg2",    "mc", Y264_CPU_NEON, t_pred_avg2 },
    { "pixel_avg_wt", "mc", Y264_CPU_NEON, t_pixel_avg_wt },
    { "mc_luma_win",  "mc", Y264_CPU_NEON, t_mc_luma_kernel },
    { "mc_chroma_win","mc", Y264_CPU_NEON, t_mc_chroma_kernel },
    { "hpel_rows",    "mc", Y264_CPU_NEON, t_hpel_rows },
#endif
    { "mc_luma",      "mc", 0, t_mc_luma_oracle },
    { "mc_chroma",    "mc", 0, t_mc_chroma_oracle },
    { "hpel_build",   "mc", 0, t_hpel_build },
    { NULL, "mc", 0, NULL },
};
