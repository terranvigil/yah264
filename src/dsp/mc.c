/*
 * mc.c - motion compensation interpolation (ITU-T H.264 8.4.2.2)
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "mc.h"
#include "../common/ledger.h"
#include "../common/cpu.h"
#include <string.h>
#include <stdlib.h>

#if defined(__aarch64__) && N264_BIT_DEPTH == 8
void n264_mc_luma_neon16(pixel *dst, int dstride, const pixel *ref,
                         int rstride, int ix, int iy, int fx, int fy, int h);
void n264_mc_luma_neon8(pixel *dst, int dstride, const pixel *ref,
                        int rstride, int ix, int iy, int fx, int fy, int h);
void n264_mc_chroma_neon8(pixel *dst, int dstride, const pixel *ref,
                          int rstride, int ix, int iy, int fx, int fy);
void n264_mc_chroma_neon_w4h(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h);
void n264_mc_chroma_neon_w8h(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h);
void n264_hpel_hrow_neon(int32_t *srow, const pixel *row, int x0, int x1);
void n264_pred_copy_neon(pixel *dst, int dstride, const pixel *s, int sstride,
                         int w, int h);
void n264_pred_avg2_neon(pixel *dst, int dstride, const pixel *s1,
                         const pixel *s2, int sstride, int w, int h);
void n264_pixel_avg_wt_neon(pixel *dst, const pixel *a, const pixel *b, int n,
                            int w0, int w1);
void n264_hpel_outrow_neon(pixel *Hr, pixel *Vr, pixel *Cr,
                           const int32_t *s0, const int32_t *s1, const int32_t *s2,
                           const int32_t *s3, const int32_t *s4, const int32_t *s5,
                           const pixel *r0, const pixel *r1, const pixel *r2,
                           const pixel *r3, const pixel *r4, const pixel *r5,
                           int x0, int x1);
#endif

/* F3c border fast path (see n264_mc_chroma). Env-gated lazy static behind an
 * accessor so the encoder-open warm-up can resolve it before any worker runs. */
static int mc_f3c_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("N264_F3C"); v = e ? atoi(e) : 1; }
    return v;
}
void n264_mc_warm_statics(void) { (void)mc_f3c_env(); }

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int clip1(int v)
{
    return v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v);
}

/* Clamped reference-sample access (edge replication for out-of-bounds MVs).
 * RN is the unclamped form, valid only inside the plane's replicated border. */
#define RC(x, y) ref[clampi((y), 0, ph - 1) * rstride + clampi((x), 0, pw - 1)]
#define RN(x, y) ref[(size_t)(y) * rstride + (x)]
#define R(x, y)  (nc ? RN(x, y) : RC(x, y))

/* 6-tap kernel (1,-5,20,20,-5,1) applied to six consecutive samples. */
static inline int tap6(int a, int b, int c, int d, int e, int f)
{
    return a - 5 * b + 20 * c + 20 * d - 5 * e + f;
}

/* Block-separable quarter-pel interpolation. The half-pel planes each cover the
 * output block plus a one-sample right/bottom margin (for the quarter positions
 * that average a sample one column/row away), and are computed once per call
 * rather than per output pixel. Maximum block is 16x16, so planes are 17x17.
 * This matches the per-sample process in H.264 8.4.2.2.1 exactly. */
#define MB 18

/* Portable reference implementation; also the checkasm baseline for the SIMD
 * kernels. Callers use n264_mc_luma, which dispatches to the best kernel the
 * detected CPU supports and falls back here at frame edges / other sizes.
 *
 * `nc` = the whole 6-tap window is inside the reference's replicated border, so
 * the per-sample coordinate clamp is a no-op and is compiled out -- the same
 * in-bounds argument the NEON kernels are gated on, and the clamp is what stops
 * the compiler vectorizing the interpolation loops on the C board. Only the
 * dispatcher sets it; n264_mc_luma_c stays the fully-clamped reference. */
static void mc_luma_body(pixel *dst, int dstride,
                         const pixel *ref, int rstride, int pw, int ph,
                         int bx, int by, int mvx, int mvy, int w, int h, int nc)
{
    int ix = bx + (mvx >> 2), iy = by + (mvy >> 2);
    int fx = mvx & 3, fy = mvy & 3;

    if (fx == 0 && fy == 0) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst[y * dstride + x] = (pixel)R(ix + x, iy + y);
        return;
    }

    int mw = w + 1, mh = h + 1;                 /* plane extent incl. margin */
    int G[MB][MB], H[MB][MB], V[MB][MB], J[MB][MB];

    int needH = (fx != 0);
    int needV = (fy != 0);
    int needJ = (fx == 2 && fy != 0) || (fy == 2 && fx != 0);
    int needG = ((fx & 1) && fy == 0) || (fx == 0 && (fy & 1));

    if (needG)
        for (int y = 0; y < mh; y++)
            for (int x = 0; x < mw; x++)
                G[y][x] = R(ix + x, iy + y);

    if (needH)
        for (int y = 0; y < mh; y++)
            for (int x = 0; x < mw; x++)
                H[y][x] = clip1((tap6(R(ix+x-2, iy+y), R(ix+x-1, iy+y),
                                      R(ix+x, iy+y), R(ix+x+1, iy+y),
                                      R(ix+x+2, iy+y), R(ix+x+3, iy+y)) + 16) >> 5);

    if (needV)
        for (int y = 0; y < mh; y++)
            for (int x = 0; x < mw; x++)
                V[y][x] = clip1((tap6(R(ix+x, iy+y-2), R(ix+x, iy+y-1),
                                      R(ix+x, iy+y), R(ix+x, iy+y+1),
                                      R(ix+x, iy+y+2), R(ix+x, iy+y+3)) + 16) >> 5);

    if (needJ) {
        /* Horizontal 6-tap intermediates (unclipped), rows iy-2..iy+h+3. */
        int cc[MB + 5][MB];
        for (int y = -2; y < mh + 3; y++)
            for (int x = 0; x < mw; x++)
                cc[y + 2][x] = tap6(R(ix+x-2, iy+y), R(ix+x-1, iy+y),
                                    R(ix+x, iy+y), R(ix+x+1, iy+y),
                                    R(ix+x+2, iy+y), R(ix+x+3, iy+y));
        for (int y = 0; y < mh; y++)
            for (int x = 0; x < mw; x++)
                J[y][x] = clip1((tap6(cc[y][x], cc[y+1][x], cc[y+2][x],
                                      cc[y+3][x], cc[y+4][x], cc[y+5][x]) + 512) >> 10);
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int v;
            switch (fy * 4 + fx) {
            case 0*4+1: v = (G[y][x] + H[y][x] + 1) >> 1; break;         /* a */
            case 0*4+2: v = H[y][x]; break;                             /* b */
            case 0*4+3: v = (G[y][x+1] + H[y][x] + 1) >> 1; break;       /* c */
            case 1*4+0: v = (G[y][x] + V[y][x] + 1) >> 1; break;         /* d */
            case 2*4+0: v = V[y][x]; break;                             /* h */
            case 3*4+0: v = (G[y+1][x] + V[y][x] + 1) >> 1; break;       /* n */
            case 1*4+1: v = (H[y][x] + V[y][x] + 1) >> 1; break;         /* e */
            case 1*4+2: v = (H[y][x] + J[y][x] + 1) >> 1; break;         /* f */
            case 1*4+3: v = (H[y][x] + V[y][x+1] + 1) >> 1; break;       /* g */
            case 2*4+1: v = (V[y][x] + J[y][x] + 1) >> 1; break;         /* i */
            case 2*4+2: v = J[y][x]; break;                             /* j */
            case 2*4+3: v = (J[y][x] + V[y][x+1] + 1) >> 1; break;       /* k */
            case 3*4+1: v = (V[y][x] + H[y+1][x] + 1) >> 1; break;       /* p */
            case 3*4+2: v = (J[y][x] + H[y+1][x] + 1) >> 1; break;       /* q */
            case 3*4+3: v = (V[y][x+1] + H[y+1][x] + 1) >> 1; break;     /* r */
            default:    v = R(ix + x, iy + y); break;
            }
            dst[y * dstride + x] = (pixel)v;
        }
    }
}

void n264_mc_luma_c(pixel *dst, int dstride,
                    const pixel *ref, int rstride, int pw, int ph,
                    int bx, int by, int mvx, int mvy, int w, int h)
{
    mc_luma_body(dst, dstride, ref, rstride, pw, ph, bx, by, mvx, mvy, w, h, 0);
}

/* Does the interpolation window of this block fit inside the plane's border?
 * Widest read is columns [ix-2, ix+w+3] and rows [iy-2, iy+h+3] (the +1 margin
 * plane plus the 6-tap reach). */
static inline int mc_luma_inborder(int ix, int iy, int pw, int ph, int w, int h)
{
    const int B = N264_LUMA_BORDER;
    return ix - 2 >= -B && iy - 2 >= -B
        && ix + w + 3 <= pw - 1 + B && iy + h + 3 <= ph - 1 + B;
}
#undef MB


#if defined(__aarch64__) && N264_BIT_DEPTH == 8
/* Clamped source tile, so an out-of-window block can still run the kernel.
 *
 * The NEON path's guard is `ix >= 2 - N264_LUMA_BORDER`: past that the six-tap
 * would read outside the plane's replicated border and the block fell all the
 * way to the fully-clamped scalar interpolator. On the board's WORST clip that
 * is not a corner case -- bus_cif at its solved CRF takes the scalar path on
 * **55.6% of luma MC calls** (14723 of 26755, every one of them the ix guard),
 * and `n264_mc_luma_c` is 1.33% of that clip's wall while the NEON kernel is
 * 0.06%. park_joy 11.3% and stefan 9.1% are the same shape; foreman and
 * samsung, which have little global motion, are under 1%.
 *
 * The fix is to hand the kernel a window it CAN read: gather the samples the
 * scalar path would have clamped to, once, into a small tile, then filter it
 * normally. Byte-identical by construction -- same clamped samples, same
 * six-tap -- and it turns ~36 clamped reads per output pixel into 2.75.
 *
 * The kernel reads rows iy-2..iy+h+3 and columns ix-2..ix+18, so the tile is
 * (h+6) rows of MCL_TS bytes with the block placed at (2,2). MCL_TS is 32 so
 * that the kernel's 16-byte loads cannot run off the end. */
#define MCL_TS 32
static void mc_luma_tile(pixel *tile, const pixel *ref, int rstride,
                         int pw, int ph, int ix, int iy, int h)
{
    int x0 = ix - 2, rows = h + 6;
    int lft = x0 < 0 ? (-x0 > MCL_TS ? MCL_TS : -x0) : 0;   /* cols clamped to 0 */
    int rgt = pw - x0;                                      /* first col past pw-1 */
    if (rgt < lft) rgt = lft;
    if (rgt > MCL_TS) rgt = MCL_TS;
    for (int j = 0; j < rows; j++) {
        int sy = iy - 2 + j;
        sy = sy < 0 ? 0 : (sy > ph - 1 ? ph - 1 : sy);
        const pixel *row = ref + (size_t)sy * rstride;
        pixel *t = tile + (size_t)j * MCL_TS;
        if (lft)         memset(t, row[0], (size_t)lft);
        if (rgt > lft)   memcpy(t + lft, row + x0 + lft, (size_t)(rgt - lft));
        if (rgt < MCL_TS) memset(t + rgt, row[pw - 1], (size_t)(MCL_TS - rgt));
    }
}
#endif

/* Runtime-dispatched luma MC. Auto-selects the best kernel for the detected CPU
 * (currently NEON on aarch64 for the common in-bounds 16x16 case) and falls back
 * to the portable path otherwise. */
/* --- half-pel plane fetch (see mc.h) --- */
void n264_pred_copy_c(pixel *dst, int dstride, const pixel *s, int sstride,
                      int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[y * dstride + x] = s[y * sstride + x];
}

void n264_pred_avg2_c(pixel *dst, int dstride, const pixel *s1, const pixel *s2,
                      int sstride, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[y * dstride + x] =
                (pixel)((s1[y * sstride + x] + s2[y * sstride + x] + 1) >> 1);
}

void n264_pred_copy(pixel *dst, int dstride, const pixel *s, int sstride,
                    int w, int h)
{
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    if (n264_asm_on(N264_ASM_MC) && (w == 4 || w == 8 || w == 16)) {
        n264_pred_copy_neon(dst, dstride, s, sstride, w, h);
        return;
    }
#endif
    n264_pred_copy_c(dst, dstride, s, sstride, w, h);
}

void n264_pred_avg2(pixel *dst, int dstride, const pixel *s1, const pixel *s2,
                    int sstride, int w, int h)
{
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    if (n264_asm_on(N264_ASM_MC) && (w == 4 || w == 8 || w == 16)) {
        n264_pred_avg2_neon(dst, dstride, s1, s2, sstride, w, h);
        return;
    }
#endif
    n264_pred_avg2_c(dst, dstride, s1, s2, sstride, w, h);
}

void n264_pixel_avg_wt_c(pixel *dst, const pixel *a, const pixel *b, int n,
                         int w0, int w1)
{
    for (int i = 0; i < n; i++)
        dst[i] = (pixel)clip1((a[i] * w0 + b[i] * w1 + 32) >> 6);
}

void n264_pixel_avg_wt(pixel *dst, const pixel *a, const pixel *b, int n,
                       int w0, int w1)
{
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    if (n264_asm_on(N264_ASM_MC)) {
        n264_pixel_avg_wt_neon(dst, a, b, n, w0, w1);
        return;
    }
#endif
    n264_pixel_avg_wt_c(dst, a, b, n, w0, w1);
}

void n264_mc_luma(pixel *dst, int dstride,
                  const pixel *ref, int rstride, int pw, int ph,
                  int bx, int by, int mvx, int mvy, int w, int h)
{
    NLED(mc_luma_call, 1); NLED(mc_luma_pix, (uint64_t)w*h);
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    /* n264_cpu_detect is warmed on the main thread at encoder open and returns
 * a cached value, so reading it here is race-free under the wavefront. A
 * per-function lazy static (the old pattern) was written concurrently by
 * workers -- a benign but TSan-flagged init race. */
    int have_neon = n264_asm_on(N264_ASM_MC);
    /* The NEON kernel computes 16-wide rows; narrower blocks compute into a
 * 16-wide temp and copy out (the ~2x extra math still beats the scalar
 * clamped path by an order of magnitude, and the profile put mc_luma_c at
 * ~75% of encode CPU once sub-16 partitions landed). */
    if (have_neon && w <= 16 && h <= 16) {
        int ix = bx + (mvx >> 2), iy = by + (mvy >> 2);
        /* Reference planes carry edge-replicated borders (allocated by the
 * encoder per mc.h), so the in-bounds window extends past the frame:
 * reading the borders equals the spec's coordinate clamping. */
        if (ix >= 2 - N264_LUMA_BORDER && iy >= 2 - N264_LUMA_BORDER &&
            iy + h + 4 <= ph + N264_LUMA_BORDER) {
            if (w > 8 && ix + 19 <= pw + N264_LUMA_BORDER) {
                if (w == 16) {
                    n264_mc_luma_neon16(dst, dstride, ref, rstride, ix, iy,
                                        mvx & 3, mvy & 3, h);
                } else {
                    pixel tmp[16 * 16];
                    n264_mc_luma_neon16(tmp, 16, ref, rstride, ix, iy,
                                        mvx & 3, mvy & 3, h);
                    for (int y = 0; y < h; y++)
                        memcpy(dst + y * dstride, tmp + y * 16, w);
                }
                return;
            }
            if (w <= 8 && ix + 14 <= pw + N264_LUMA_BORDER) {   /* 16B loads from ix-2 */
                if (w == 8) {
                    n264_mc_luma_neon8(dst, dstride, ref, rstride, ix, iy,
                                       mvx & 3, mvy & 3, h);
                } else {
                    pixel tmp[8 * 16];
                    n264_mc_luma_neon8(tmp, 8, ref, rstride, ix, iy,
                                       mvx & 3, mvy & 3, h);
                    for (int y = 0; y < h; y++)
                        memcpy(dst + y * dstride, tmp + y * 8, w);
                }
                return;
            }
        }
        /* Outside the window the kernel can read directly: gather the clamped
 * samples into a tile and run it on that (see mc_luma_tile). */
        pixel tile[MCL_TS * 22];
        mc_luma_tile(tile, ref, rstride, pw, ph, ix, iy, h);
        if (w == 16) {
            n264_mc_luma_neon16(dst, dstride, tile, MCL_TS, 2, 2,
                                mvx & 3, mvy & 3, h);
        } else {
            pixel tmp[16 * 16];
            n264_mc_luma_neon16(tmp, 16, tile, MCL_TS, 2, 2, mvx & 3, mvy & 3, h);
            for (int y = 0; y < h; y++)
                memcpy(dst + y * dstride, tmp + y * 16, (size_t)w * sizeof(pixel));
        }
        return;
    }
#endif
    if (mc_luma_inborder(bx + (mvx >> 2), by + (mvy >> 2), pw, ph, w, h)) {
        mc_luma_body(dst, dstride, ref, rstride, pw, ph, bx, by, mvx, mvy, w, h, 1);
        return;
    }
    n264_mc_luma_c(dst, dstride, ref, rstride, pw, ph, bx, by, mvx, mvy, w, h);
}

void n264_mc_chroma_c(pixel *dst, int dstride,
                      const pixel *ref, int rstride, int pw, int ph,
                      int cbx, int cby, int mvx, int mvy, int w, int h,
                      int sub_w, int sub_h)
{
    /* Per-axis MV interpretation (8.4.1.4 / 8.4.2.2.2): half-res axis is
 * eighth-pel; full-res axis is quarter-pel scaled into eighth-pel space. */
    int ix, iy, fx, fy;
    if (sub_w == 2) { ix = cbx + (mvx >> 3); fx = mvx & 7; }
    else            { ix = cbx + (mvx >> 2); fx = (mvx & 3) << 1; }
    if (sub_h == 2) { iy = cby + (mvy >> 3); fy = mvy & 7; }
    else            { iy = cby + (mvy >> 2); fy = (mvy & 3) << 1; }
    /* F3c (like F3): the chroma plane carries a replicated border, so an in-bounds
 * block reads it directly -- border pixel == nearest edge == clampi -- with no
 * per-pixel clamp (4 clamped reads/pixel in the tail path). Byte-identical. */
    int f3c = mc_f3c_env();
    const int CB = N264_CHROMA_BORDER;
    if (f3c && ix >= -CB && iy >= -CB && ix + w <= pw - 1 + CB && iy + h <= ph - 1 + CB) {
        const pixel *r = ref + iy * rstride + ix;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int A = r[y*rstride + x],     B = r[y*rstride + x + 1];
                int C = r[(y+1)*rstride + x], D = r[(y+1)*rstride + x + 1];
                int v = ((8 - fx) * (8 - fy) * A + fx * (8 - fy) * B +
                         (8 - fx) * fy * C + fx * fy * D + 32) >> 6;
                dst[y * dstride + x] = (pixel)v;
            }
        return;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int X = ix + x, Y = iy + y;
            int A = RC(X, Y), B = RC(X + 1, Y);
            int C = RC(X, Y + 1), D = RC(X + 1, Y + 1);
            int v = ((8 - fx) * (8 - fy) * A + fx * (8 - fy) * B +
                     (8 - fx) * fy * C + fx * fy * D + 32) >> 6;
            dst[y * dstride + x] = (pixel)v;
        }
    }
}

/* Build H/V/C half-pel planes for one reference, bit-exact with the on-the-fly
 * n264_mc_luma_c 6-tap process (same clamp-to-frame reads, same rounding). The
 * horizontal-half plane and the centre plane share the unclipped horizontal
 * 6-tap intermediates, computed once into `scratch`; the centre plane then
 * runs the vertical 6-tap over those intermediates (matching mc_luma_c's cc[]
 * path), and the vertical-half plane runs the vertical 6-tap over the integer
 * samples directly. */
void n264_mc_build_hpel_rows(pixel *Hp, pixel *Vp, pixel *Cp, int stride,
                             const pixel *ref, int rstride, int pw, int ph, int border,
                             int32_t *scratch, int sstride, int ry0, int ry1)
{
    NLED(hpel_call, 1);
    const int x0 = -border, x1 = pw + border;   /* output columns [x0, x1) */
    int y0 = -border, y1 = ph + border;         /* full output rows [y0, y1) */
    /* Row BAND [ry0, ry1) of the output, so the build can be split across
 * workers. Each band redoes the 5 horizontal rows of overlap its vertical
 * filter needs (~12% extra horizontal work at 18 bands), which keeps the
 * bands independent -- every output row is a pure function of the source, so
 * a banded build is bit-identical to the whole-frame one. */
    if (ry0 > y0) y0 = ry0;
    if (ry1 < y1) y1 = ry1;
    if (y1 <= y0) return;
    /* scratch holds the unclipped horizontal 6-tap for rows [y0-2, y1+3),
 * indexed scratch[(y - (y0-2)) * sstride + (x - x0)]. */
    const int sy0 = y0 - 2;
    int32_t *const sbase = scratch - (size_t)sy0 * sstride - x0;

    /* Interior columns (xm2>=0 and xp3<=pw-1) need no clamp; only the border
 * columns do. Splitting out the interior avoids 6 clamps/pixel over the bulk
 * of the frame. Byte-identical (clamp is a no-op where coords are in range). */
    int xin0 = 2, xin1 = pw - 3;                    /* interior = [xin0, xin1) */
    if (xin0 < x0) xin0 = x0;
    if (xin1 > x1) xin1 = x1;
    if (xin1 < xin0) xin1 = xin0;                   /* tiny frame: no interior */
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    int hpel_neon = n264_asm_on(N264_ASM_HPEL) && xin1 - xin0 >= 8;
#endif
    for (int y = sy0; y < y1 + 3; y++) {
        int cy = y < 0 ? 0 : (y >= ph ? ph - 1 : y);   /* clamp row to frame */
        const pixel *row = ref + (size_t)cy * rstride;
        int32_t *srow = sbase + (size_t)y * sstride;
        int x;
        for (x = x0; x < xin0; x++) {                    /* left border */
            int xm2 = x-2 < 0 ? 0 : x-2, xm1 = x-1 < 0 ? 0 : x-1;
            int xp1 = x+1 >= pw ? pw-1 : x+1, xp2 = x+2 >= pw ? pw-1 : x+2, xp3 = x+3 >= pw ? pw-1 : x+3;
            int x0c = x < 0 ? 0 : (x >= pw ? pw-1 : x);
            srow[x] = tap6(row[xm2], row[xm1], row[x0c], row[xp1], row[xp2], row[xp3]);
        }
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
        if (hpel_neon) {
            n264_hpel_hrow_neon(srow, row, xin0, xin1);
            x = xin1;
        } else
#endif
        for (x = xin0; x < xin1; x++)                    /* interior: no clamp */
            srow[x] = tap6(row[x-2], row[x-1], row[x], row[x+1], row[x+2], row[x+3]);
        for (x = xin1; x < x1; x++) {                    /* right border */
            int xm2 = x-2 < 0 ? 0 : x-2, xm1 = x-1 < 0 ? 0 : x-1, x0c = x < 0 ? 0 : (x >= pw ? pw-1 : x);
            int xp1 = x+1 >= pw ? pw-1 : x+1, xp2 = x+2 >= pw ? pw-1 : x+2, xp3 = x+3 >= pw ? pw-1 : x+3;
            srow[x] = tap6(row[xm2], row[xm1], row[x0c], row[xp1], row[xp2], row[xp3]);
        }
    }

    for (int y = y0; y < y1; y++) {
        pixel *Hr = Hp + (size_t)y * stride;
        pixel *Vr = Vp + (size_t)y * stride;
        pixel *Cr = Cp + (size_t)y * stride;
        const int32_t *s0 = sbase + (size_t)(y - 2) * sstride;
        const int32_t *s1 = s0 + sstride, *s2 = s1 + sstride, *s3 = s2 + sstride;
        const int32_t *s4 = s3 + sstride, *s5 = s4 + sstride;
        /* rows for the vertical (integer) filter, clamped to the frame */
        int ym2 = y - 2, ym1 = y - 1, y0c = y, yp1 = y + 1, yp2 = y + 2, yp3 = y + 3;
        ym2 = ym2 < 0 ? 0 : (ym2 >= ph ? ph - 1 : ym2);
        ym1 = ym1 < 0 ? 0 : (ym1 >= ph ? ph - 1 : ym1);
        y0c = y0c < 0 ? 0 : (y0c >= ph ? ph - 1 : y0c);
        yp1 = yp1 < 0 ? 0 : (yp1 >= ph ? ph - 1 : yp1);
        yp2 = yp2 < 0 ? 0 : (yp2 >= ph ? ph - 1 : yp2);
        yp3 = yp3 < 0 ? 0 : (yp3 >= ph ? ph - 1 : yp3);
        const pixel *r0 = ref + (size_t)ym2 * rstride, *r1 = ref + (size_t)ym1 * rstride;
        const pixel *r2 = ref + (size_t)y0c * rstride, *r3 = ref + (size_t)yp1 * rstride;
        const pixel *r4 = ref + (size_t)yp2 * rstride, *r5 = ref + (size_t)yp3 * rstride;
        /* [0, pw) needs no x-clamp, so it runs as its own loop -- the per-pixel
 * clamp in the general body is what stops the compiler vectorizing the
 * bulk of the row on the C path (and it is a no-op there anyway). The
 * NEON kernel covers exactly the same span when it is available;
 * whichever runs, only the true borders take the clamped body. */
        int seg_hi[2], seg_lo[2];
        int nseg = 0;
        int v0 = x0 > 0 ? x0 : 0, v1 = x1 < pw ? x1 : pw;
        int mid = 0;
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
        if (hpel_neon && v1 - v0 >= 8) {
            n264_hpel_outrow_neon(Hr, Vr, Cr, s0, s1, s2, s3, s4, s5,
                                  r0, r1, r2, r3, r4, r5, v0, v1);
            mid = 1;
        }
#endif
        if (!mid && v0 < v1) {
            for (int x = v0; x < v1; x++) {
                Hr[x] = (pixel)clip1((s2[x] + 16) >> 5);
                Cr[x] = (pixel)clip1((tap6(s0[x], s1[x], s2[x], s3[x], s4[x], s5[x]) + 512) >> 10);
                Vr[x] = (pixel)clip1((tap6(r0[x], r1[x], r2[x], r3[x], r4[x], r5[x]) + 16) >> 5);
            }
            mid = 1;
        }
        if (mid) {
            if (x0 < v0) { seg_lo[nseg] = x0; seg_hi[nseg] = v0; nseg++; }
            if (v1 < x1) { seg_lo[nseg] = v1; seg_hi[nseg] = x1; nseg++; }
        } else {
            seg_lo[nseg] = x0; seg_hi[nseg] = x1; nseg++;
        }
        for (int seg = 0; seg < nseg; seg++)
            for (int x = seg_lo[seg]; x < seg_hi[seg]; x++) {
                Hr[x] = (pixel)clip1((s2[x] + 16) >> 5);
                Cr[x] = (pixel)clip1((tap6(s0[x], s1[x], s2[x], s3[x], s4[x], s5[x]) + 512) >> 10);
                int cx = x < 0 ? 0 : (x >= pw ? pw - 1 : x);
                Vr[x] = (pixel)clip1((tap6(r0[cx], r1[cx], r2[cx], r3[cx], r4[cx], r5[cx]) + 16) >> 5);
            }
    }
}

void n264_mc_build_hpel(pixel *Hp, pixel *Vp, pixel *Cp, int stride,
                        const pixel *ref, int rstride, int pw, int ph, int border,
                        int32_t *scratch, int sstride)
{
    n264_mc_build_hpel_rows(Hp, Vp, Cp, stride, ref, rstride, pw, ph, border,
                            scratch, sstride, -border, ph + border);
}

void n264_mc_chroma(pixel *dst, int dstride,
                    const pixel *ref, int rstride, int pw, int ph,
                    int cbx, int cby, int mvx, int mvy, int w, int h,
                    int sub_w, int sub_h)
{
    NLED(mc_chroma_call, 1); NLED(mc_chroma_pix, (uint64_t)w*h);
#if defined(__aarch64__) && N264_BIT_DEPTH == 8
    /* n264_cpu_detect is warmed on the main thread at encoder open and returns
 * a cached value, so reading it here is race-free under the wavefront. A
 * per-function lazy static (the old pattern) was written concurrently by
 * workers -- a benign but TSan-flagged init race. */
    int have_neon = n264_asm_on(N264_ASM_MC);
    /* w8 and w4, both at even heights. Round 2 left w4 on the C after a
 * one-row-per-vector port tied it; the shipped w4 kernel packs two output
 * rows per vector instead, which is what makes a 4-wide row worth
 * vectorising at all. Width 2 stays C -- it does not occur at all in the
 * default configuration (0 of 2.75M calls at the samsung point). */
    if (have_neon && (w == 8 || w == 4) && !(h & 1) && h >= 2) {
        /* Per-axis MV interpretation, exactly as n264_mc_chroma_c. */
        int ix, iy, fx, fy;
        if (sub_w == 2) { ix = cbx + (mvx >> 3); fx = mvx & 7; }
        else            { ix = cbx + (mvx >> 2); fx = (mvx & 3) << 1; }
        if (sub_h == 2) { iy = cby + (mvy >> 3); fy = mvy & 7; }
        else            { iy = cby + (mvy >> 2); fy = (mvy & 3) << 1; }
        if (ix >= -N264_CHROMA_BORDER && iy >= -N264_CHROMA_BORDER &&
            ix + w + 1 <= pw + N264_CHROMA_BORDER &&
            iy + h + 1 <= ph + N264_CHROMA_BORDER) {
            if (w == 4)
                n264_mc_chroma_neon_w4h(dst, dstride, ref, rstride, ix, iy, fx, fy, h);
            else if (h == 8)
                n264_mc_chroma_neon8(dst, dstride, ref, rstride, ix, iy, fx, fy);
            else
                n264_mc_chroma_neon_w8h(dst, dstride, ref, rstride, ix, iy, fx, fy, h);
            return;
        }
    }
#endif
    n264_mc_chroma_c(dst, dstride, ref, rstride, pw, ph, cbx, cby, mvx, mvy, w, h, sub_w, sub_h);
}

#undef R
