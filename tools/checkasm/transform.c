/*
 * checkasm: transforms, quant, dequant and the scan kernels.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * THE QUANT GROUPS ARE WHY THIS SPLIT HAPPENED. In the single-file harness they
 * could not name their kernels -- the quant multiplier row is a table the
 * dispatcher owns and nothing exported it -- so they compared the public entry
 * point against ITSELF with Y264_ASM_OFF flipped in between. Two problems with
 * that. It tests the ablation switch as much as the kernel, and the moment a
 * kernel and the C path share a defect the group agrees with itself.
 *
 * The kernels take the row as an ARGUMENT, so this file builds the flat-scaling
 * rows from the specification's normAdjust tables (ITU-T H.264 8.5.9, and the
 * 8x8 equivalent) and hands them to the kernel, while the reference side runs
 * the library's own weighted path with a flat matrix of 16 -- which produces
 * the identical multipliers by construction, (16*mf + 8)/16 == mf, and derives
 * them from the library's tables rather than from these. So the two sides agree
 * only if BOTH transcriptions are right. A wrong row here fails the group; it
 * cannot pass it. The 4x4 forward row is cross-checked against the library's
 * public y264_mf4_at() on top of that, which pins the category map the 8x8
 * rows are built with the same way.
 */

#include "checkasm.h"
#include "dsp/arch.h"
#include "dsp/transform.h"
#include "encoder/cabac.h"

/* ---- the flat scaling rows, from the specification -----------------------
 *
 * Forward normAdjust MF[qp%6][category] and inverse normAdjust V[qp%6][cat],
 * with the 4x4 category a pure function of the raster position's parity and the
 * 8x8 category the six-region map of (y%4, x%4). */
static const int CA_MF4[6][3] = {
    { 13107, 5243, 8066 }, { 11916, 4660, 7490 }, { 10082, 4194, 6554 },
    {  9362, 3647, 5825 }, {  8192, 3355, 5243 }, {  7282, 2893, 4559 },
};
static const int CA_V4[6][3] = {
    { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 },
    { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 },
};
static const int CA_MF8[6][6] = {
    { 13107, 12222, 16777, 11428, 20972, 15481 },
    { 11916, 11058, 14980, 10826, 19174, 14290 },
    { 10082,  9675, 12710,  8943, 15978, 11985 },
    {  9362,  8931, 11984,  8228, 14913, 11259 },
    {  8192,  7740, 10486,  7346, 13159,  9777 },
    {  7282,  6830,  9118,  6428, 11570,  8640 },
};
static const int CA_V8[6][6] = {
    { 20, 19, 25, 18, 32, 24 }, { 22, 21, 28, 19, 35, 26 },
    { 26, 24, 33, 23, 42, 31 }, { 28, 26, 35, 25, 45, 33 },
    { 32, 30, 40, 28, 51, 38 }, { 36, 34, 46, 32, 58, 43 },
};

static int ca_cat4(int idx)
{
    int x = idx & 3, y = idx >> 2;
    if (!(x & 1) && !(y & 1)) return 0;
    if ((x & 1) && (y & 1))   return 1;
    return 2;
}

static int ca_cat8(int idx)
{
    static const int map[4][4] = {
        { 0, 1, 2, 1 }, { 1, 3, 5, 3 }, { 2, 5, 4, 5 }, { 1, 3, 5, 3 },
    };
    return map[(idx >> 3) & 3][idx & 3];
}

static void ca_mf4_row(int32_t row[16], int qp)
{
    for (int i = 0; i < 16; i++) row[i] = CA_MF4[qp % 6][ca_cat4(i)];
}
static void ca_mf8_row(int32_t row[64], int qp)
{
    for (int i = 0; i < 64; i++) row[i] = CA_MF8[qp % 6][ca_cat8(i)];
}
static void ca_dq4_row(int32_t row[16], int qp)
{
    for (int i = 0; i < 16; i++) row[i] = 16 * CA_V4[qp % 6][ca_cat4(i)];
}
static void ca_dq8_row(int32_t row[64], int qp)
{
    for (int i = 0; i < 64; i++) row[i] = 16 * CA_V8[qp % 6][ca_cat8(i)];
}

/* The one place the transcription is checked against the library instead of
 * against a kernel: if these disagree, every row built above is suspect and
 * saying so beats letting a quant group fail with a confusing diff. */
static int ca_rows_sane(void)
{
    for (int qp = 0; qp <= 51; qp++)
        for (int i = 0; i < 16; i++)
            if (CA_MF4[qp % 6][ca_cat4(i)] != y264_mf4_at(i, qp)) {
                ca_fail("quant rows: the harness's normAdjust transcription "
                        "disagrees with the library at qp %d idx %d (%d vs %d)",
                        qp, i, CA_MF4[qp % 6][ca_cat4(i)], y264_mf4_at(i, qp));
                return 1;
            }
    return 0;
}

static const uint8_t flat16[64] = {
    16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16, 16,16,16,16,16,16,16,16,
};

#if Y264_HAVE_NEON

/* ---- coefficient-domain transforms ---------------------------------------
 *
 * Forward inputs stay in the pixel-difference domain [-PIXEL_MAX, PIXEL_MAX]:
 * that is the kernels' exactness domain and the only domain the encoder feeds
 * them. The inverse gets full-range int16 coefficients, including the
 * saturation corners, because the recon path must match the scalar
 * truncate-then-clip exactly.
 */
typedef void (*coeffn)(const dctcoef *, dctcoef *);

static int run_coef(const char *name, coeffn kern, coeffn cref, int n, int fwd)
{
    dctcoef *in = ca_guard_alloc(64 * sizeof(dctcoef));
    dctcoef *o1 = ca_guard_alloc(64 * sizeof(dctcoef));
    dctcoef o2[64];
    int bad = 0;
    for (int t = 0; t < TRIALS * 4; t++) {
        for (int i = 0; i < n; i++)
            in[i] = fwd ? (dctcoef)((int)ca_rnd() - (int)ca_rnd())
                        : (dctcoef)(int16_t)(ca_rnd() | ((unsigned)ca_rnd() << 8));
        if (t == 0 && !fwd) { in[0] = 32767; in[1] = -32768; }
        ca_guard_poison(o1, (size_t)n * sizeof(dctcoef));
        kern(in, o1);
        cref(in, o2);
        if (memcmp(o1, o2, n * sizeof(dctcoef))) {
            if (!bad) ca_fail("%s: coefficients differ", name);
            bad++;
        }
        if (!bad && ca_guard_check(o1, n * sizeof(dctcoef), name))
            bad++;
    }
    CA_BENCH2(name, kern(in, o1), cref(in, o2));
    ca_guard_free(in);
    ca_guard_free(o1);
    return bad;
}

static int t_fdct4x4(void)  { return run_coef("fdct4x4", y264_fdct4x4_neon, y264_fdct4x4_c, 16, 1); }
static int t_idct4x4(void)  { return run_coef("idct4x4", y264_idct4x4_neon, y264_idct4x4_c, 16, 0); }
static int t_fdct8x8(void)  { return run_coef("fdct8x8", y264_fdct8x8_neon, y264_fdct8x8_c, 64, 1); }
static int t_idct8x8(void)  { return run_coef("idct8x8", y264_idct8x8_neon, y264_idct8x8_c, 64, 0); }

/* ---- quant and dequant --------------------------------------------------- */

static int t_quant_4x4(void)
{
    dctcoef in[16], l1[16], l2[16];
    int32_t row[16];
    int bad = ca_rows_sane();
    for (int t = 0; t < TRIALS * 4 && !bad; t++) {
        int qp = (int)ca_below(52), intra = t & 1;
        ca_mf4_row(row, qp);
        for (int i = 0; i < 16; i++)
            in[i] = (dctcoef)((int)ca_rnd() - (int)ca_rnd());
        y264_quant_4x4_neon(in, l1, qp, intra, row);
        y264_quant_4x4(in, l2, qp, intra, flat16);
        if (memcmp(l1, l2, sizeof(l1))) {
            ca_fail("quant_4x4: qp=%d intra=%d", qp, intra);
            bad++;
        }
    }
    CA_BENCH2("quant_4x4", y264_quant_4x4_neon(in, l1, 26, 0, row),
                           y264_quant_4x4(in, l2, 26, 0, flat16));
    return bad;
}

/* The explicit-bias form, which the trellis seeds with. `f` is the caller's to
 * compute: the deadzone in 1/64-of-step units scaled into the qbits domain. */
static int t_quant_f64(void)
{
    dctcoef i4[16], a4[16], b4[16], i8[64], a8[64], b8[64];
    int32_t r4[16], r8[64];
    int bad = 0;
    for (int t = 0; t < TRIALS * 4 && !bad; t++) {
        int qp = (int)ca_below(52);
        int f64 = 1 + (int)ca_below(63);
        int f4 = (int)(((int64_t)f64 << (15 + qp / 6)) >> 6);
        int f8 = (int)(((int64_t)f64 << (16 + qp / 6)) >> 6);
        ca_mf4_row(r4, qp);
        ca_mf8_row(r8, qp);
        for (int i = 0; i < 16; i++) i4[i] = (dctcoef)((int)ca_rnd() - (int)ca_rnd());
        for (int i = 0; i < 64; i++) i8[i] = (dctcoef)((int)ca_rnd() - (int)ca_rnd());
        y264_quant_4x4_fneon(i4, a4, qp, f4, r4);
        y264_quant_4x4_f64(i4, b4, qp, f64, flat16);
        if (memcmp(a4, b4, sizeof(a4))) { ca_fail("quant_4x4_f64: qp=%d f64=%d", qp, f64); bad++; }
        y264_quant_8x8_fneon(i8, a8, qp, f8, r8);
        y264_quant_8x8_f64(i8, b8, qp, f64, flat16);
        if (memcmp(a8, b8, sizeof(a8))) { ca_fail("quant_8x8_f64: qp=%d f64=%d", qp, f64); bad++; }
    }
    return bad;
}

/* Levels are bounded so the dequantised coefficient stays inside 16 bits (the
 * largest flat scale is 45 at the 8x8 positions with qp%6 == 5, times
 * 2^(qp/6)): past that the C path wraps and the kernel saturates, and no
 * conforming stream carries such a level. */
static int t_dequant(void)
{
    int bad = 0;
    for (int t = 0; t < TRIALS && !bad; t++) {
        dctcoef l4[16], c4a[16], c4b[16], l8[64], c8a[64], c8b[64];
        int32_t r4[16], r8[64];
        int qp = t % 52;
        int lim = 32767 / (45 << (qp / 6));
        if (lim > 4095) lim = 4095;
        if (lim < 1) lim = 1;
        ca_dq4_row(r4, qp);
        ca_dq8_row(r8, qp);
        for (int k = 0; k < 16; k++)
            l4[k] = (dctcoef)((int)ca_below((unsigned)(2 * lim + 1)) - lim);
        for (int k = 0; k < 64; k++)
            l8[k] = (dctcoef)((int)ca_below((unsigned)(2 * lim + 1)) - lim);
        y264_dequant_4x4_neon(l4, c4a, qp, r4);
        y264_dequant_4x4(l4, c4b, qp, flat16);
        if (memcmp(c4a, c4b, sizeof(c4a))) {
            for (int k = 0; k < 16; k++)
                if (c4a[k] != c4b[k]) {
                    ca_fail("dequant_4x4: qp=%d k=%d lev=%d opt=%d ref=%d",
                            qp, k, (int)l4[k], (int)c4a[k], (int)c4b[k]);
                    break;
                }
            bad++;
        }
        y264_dequant_8x8_neon(l8, c8a, qp, r8);
        y264_dequant_8x8(l8, c8b, qp, flat16);
        if (memcmp(c8a, c8b, sizeof(c8a))) { ca_fail("dequant_8x8: qp=%d", qp); bad++; }
    }
    return bad;
}

/* ---- the fused sub-dct / add-idct ---------------------------------------- */

static pixel *src, *prd;

static void blocks(void)
{
    if (!src) {
        src = ca_guard_alloc(STRIDE * PLANE_H * sizeof(pixel));
        prd = ca_guard_alloc(STRIDE * PLANE_H * sizeof(pixel));
    }
    ca_fill(src, STRIDE * PLANE_H);
    ca_fill(prd, STRIDE * PLANE_H);
}

static int t_sub_dct(void)
{
    dctcoef c1[64], c2[64];
    int bad = 0;
    for (int t = 0; t < TRIALS * 4; t++) {
        blocks();
        y264_sub4x4_dct_neon(c1, src, STRIDE, prd, STRIDE);
        y264_sub4x4_dct_c(c2, src, STRIDE, prd, STRIDE);
        if (memcmp(c1, c2, 16 * sizeof(dctcoef))) { if (!bad) ca_fail("%s", "sub4x4_dct"); bad++; }
        y264_sub8x8_dct8_neon(c1, src, STRIDE, prd, STRIDE);
        y264_sub8x8_dct8_c(c2, src, STRIDE, prd, STRIDE);
        if (memcmp(c1, c2, 64 * sizeof(dctcoef))) { if (!bad) ca_fail("%s", "sub8x8_dct8"); bad++; }
    }
    /* Both operands sized to exactly the block they declare, at both tails. */
    if (!bad) {
        const struct { const char *n; int w; void (*k)(dctcoef *, const pixel *, int,
                                                       const pixel *, int); } ks[] = {
            { "sub4x4_dct",  4, y264_sub4x4_dct_neon },
            { "sub8x8_dct8", 8, y264_sub8x8_dct8_neon },
        };
        for (unsigned k = 0; k < 2; k++)
            for (int tail = 0; tail <= 1; tail++) {
                ca_pg gs, gp;
                size_t win = ca_pg_window_pix(STRIDE, ks[k].w, ks[k].w);
                pixel *s = ca_pg_alloc(&gs, win, tail);
                pixel *p = ca_pg_alloc(&gp, win, tail);
                dctcoef out[64];
                ca_pg_fill_pix(&gs, win / sizeof(pixel));
                ca_pg_fill_pix(&gp, win / sizeof(pixel));
                ca_pg_arm(ks[k].n);
                ks[k].k(out, s, STRIDE, p, STRIDE);
                ca_pg_disarm();
                ca_pg_free(&gs);
                ca_pg_free(&gp);
            }
    }
    CA_BENCH2("sub4x4_dct", y264_sub4x4_dct_neon(c1, src, STRIDE, prd, STRIDE),
                            y264_sub4x4_dct_c(c2, src, STRIDE, prd, STRIDE));
    CA_BENCH2("sub8x8_dct8", y264_sub8x8_dct8_neon(c1, src, STRIDE, prd, STRIDE),
                             y264_sub8x8_dct8_c(c2, src, STRIDE, prd, STRIDE));
    return bad;
}

/* The recon add: the destination is written at a stride wider than the block,
 * so the row padding is poisoned and checked as well as the borders. */
static int t_add_idct(void)
{
    enum { DS = 24 };
    dctcoef c[64];
    pixel *d1 = ca_guard_alloc(DS * 8 * sizeof(pixel));
    pixel d2[DS * 8];
    int bad = 0;
    for (int t = 0; t < TRIALS * 4; t++) {
        blocks();
        for (int i = 0; i < 64; i++)
            c[i] = (dctcoef)(int16_t)(ca_rnd() | ((unsigned)ca_rnd() << 8));
        if (t == 0) { c[0] = 32767; c[1] = -32768; }    /* saturation corner */
        memset(d1, CA_POISON, DS * 8 * sizeof(pixel));
        ca_guard_poison(d1, DS * 8 * sizeof(pixel));
        memset(d2, CA_POISON, sizeof(d2));
        y264_add4x4_idct_neon(d1, DS, prd, STRIDE, c);
        y264_add4x4_idct_c(d2, DS, prd, STRIDE, c);
        if (memcmp(d1, d2, DS * 4 * sizeof(pixel))) { if (!bad) ca_fail("%s", "add4x4_idct"); bad++; }
        if (!bad && (ca_guard_check(d1, DS * 8 * sizeof(pixel), "add4x4_idct") ||
                     ca_check_pad_pix(d1, DS, 4, 4, "add4x4_idct"))) bad++;
        memset(d1, CA_POISON, DS * 8 * sizeof(pixel));
        ca_guard_poison(d1, DS * 8 * sizeof(pixel));
        memset(d2, CA_POISON, sizeof(d2));
        y264_add8x8_idct8_neon(d1, DS, prd, STRIDE, c);
        y264_add8x8_idct8_c(d2, DS, prd, STRIDE, c);
        if (memcmp(d1, d2, DS * 8 * sizeof(pixel))) { if (!bad) ca_fail("%s", "add8x8_idct8"); bad++; }
        if (!bad && (ca_guard_check(d1, DS * 8 * sizeof(pixel), "add8x8_idct8") ||
                     ca_check_pad_pix(d1, DS, 8, 8, "add8x8_idct8"))) bad++;
    }
    CA_BENCH2("add4x4_idct", y264_add4x4_idct_neon(d1, DS, prd, STRIDE, c),
                             y264_add4x4_idct_c(d2, DS, prd, STRIDE, c));
    CA_BENCH2("add8x8_idct8", y264_add8x8_idct8_neon(d1, DS, prd, STRIDE, c),
                              y264_add8x8_idct8_c(d2, DS, prd, STRIDE, c));
    ca_guard_free(d1);
    return bad;
}

static void prev_sub_dct4_16x16(dctcoef (*g)[16])
{
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            y264_sub4x4_dct_neon(g[by * 4 + bx],
                                 src + (by * 4) * STRIDE + bx * 4, STRIDE,
                                 prd + (by * 4) * STRIDE + bx * 4, STRIDE);
}

/* Every grid shape the encoder asks for: 16x16 luma, 4:2:0 and 4:2:2 chroma. */
static int t_sub_dct4_blocks(void)
{
    static const struct { int w, h; } grid[] = { { 4, 4 }, { 2, 2 }, { 2, 4 } };
    dctcoef g1[16][16], g2[16][16];
    int bad = 0;
    for (int t = 0; t < TRIALS * 4; t++) {
        blocks();
        for (unsigned k = 0; k < sizeof(grid) / sizeof(grid[0]); k++) {
            int n = grid[k].w * grid[k].h;
            memset(g1, 0x5a, sizeof(g1));
            memset(g2, 0xa5, sizeof(g2));
            y264_sub_dct4_blocks_neon(g1, grid[k].w, grid[k].h, src, STRIDE, prd, STRIDE);
            y264_sub_dct4_blocks_c(g2, grid[k].w, grid[k].h, src, STRIDE, prd, STRIDE);
            if (memcmp(g1, g2, n * 16 * sizeof(dctcoef))) {
                if (!bad) ca_fail("sub_dct4_blocks: %dx%d grid", grid[k].w, grid[k].h);
                bad++;
            }
            /* Nothing past the blocks the grid declares. */
            if (!bad)
                for (int i = n * 16; i < 256 && !bad; i++)
                    if (((const dctcoef *)g1)[i] != (dctcoef)0x5a5a) {
                        ca_fail("sub_dct4_blocks: wrote past block %d of %d", i / 16, n);
                        bad++;
                    }
        }
    }
    /* `prev` is the path the batch displaced: sixteen separate NEON 4x4
     * forward transforms, one per block of the macroblock. */
    CA_BENCH3("sub_dct4_16x16",
              y264_sub_dct4_blocks_neon(g1, 4, 4, src, STRIDE, prd, STRIDE),
              y264_sub_dct4_blocks_c(g2, 4, 4, src, STRIDE, prd, STRIDE),
              prev_sub_dct4_16x16(g1));
    return bad;
}

/* ---- the scan kernels ----------------------------------------------------
 *
 * Levels are drawn small and sparse, the shape they really have after quant,
 * but the corners matter: -32768 is the value an abs-based |level| >= 2 test
 * gets wrong, and an all-zero / all-big block pins both ends of the mask and
 * the big flag. */
static int t_scan(void)
{
    dctcoef z4[16], z8[64], o1[64], o2[64];
    int i1[64], i2[64];
    int bad = 0;
    for (int t = 0; t < TRIALS * 4; t++) {
        for (int i = 0; i < 64; i++) {
            unsigned r = ca_rnd();
            z8[i] = (dctcoef)((r & 3) ? 0 : (int)ca_rnd() - 128);
        }
        if (t == 0) memset(z8, 0, sizeof(z8));
        if (t == 1) for (int i = 0; i < 64; i++) z8[i] = -32768;
        if (t == 2) for (int i = 0; i < 64; i++) z8[i] = (dctcoef)(i & 1 ? 1 : -1);
        if (t == 3) for (int i = 0; i < 64; i++) z8[i] = 32767;
        for (int i = 0; i < 16; i++) z4[i] = z8[i];

        memset(i1, 0x5a, sizeof(i1));
        memset(i2, 0xa5, sizeof(i2));
        y264_zigzag_abs_8x8_neon(i1, z8);
        y264_zigzag_abs_8x8_c(i2, z8);
        if (memcmp(i1, i2, sizeof(i1))) { if (!bad) ca_fail("%s", "zigzag_abs_8x8"); bad++; }

        uint64_t k1, k2;
        int b1, b2;
        y264_scan_mask_8x8_neon(z8, &k1, &b1);
        y264_scan_mask_8x8_c(z8, &k2, &b2);
        if (k1 != k2 || b1 != b2) { if (!bad) ca_fail("%s", "scan_mask_8x8"); bad++; }

        uint32_t m1, m2;
        memset(o1, 0x5a, sizeof(o1));
        memset(o2, 0xa5, sizeof(o2));
        y264_zigzag_scan_4x4_neon(o1, z4, &m1, &b1);
        y264_zigzag_scan_4x4_c(o2, z4, &m2, &b2);
        if (m1 != m2 || b1 != b2 || memcmp(o1, o2, 16 * sizeof(dctcoef))) {
            if (!bad) ca_fail("%s", "zigzag_scan_4x4");
            bad++;
        }
    }
    {
        uint64_t k; uint32_t m; int b;
        CA_BENCH2("zigzag_abs_8x8", y264_zigzag_abs_8x8_neon(i1, z8),
                                    y264_zigzag_abs_8x8_c(i2, z8));
        CA_BENCH2("scan_mask_8x8", y264_scan_mask_8x8_neon(z8, &k, &b),
                                   y264_scan_mask_8x8_c(z8, &k, &b));
        CA_BENCH2("zigzag_scan_4x4", y264_zigzag_scan_4x4_neon(o1, z4, &m, &b),
                                     y264_zigzag_scan_4x4_c(o2, z4, &m, &b));
    }
    return bad;
}

#endif /* Y264_HAVE_NEON */

/* ---- the trellis lattice, bench only --------------------------------------
 *
 * Not a correctness group: y264_cabac_trellis_4x4 has no second implementation
 * to diff against, and the row exists because the encoder-level census says the
 * lattice does ~7.8 coefficient steps and ~17 node updates per call yet costs
 * ~160 ns -- about 33 cycles a node update -- and in-situ timers cannot resolve
 * a function that small. Inputs are synthesised to the MEASURED sparsity, so
 * the cost model matches the real workload rather than a dense worst case.
 * It reports ok without running anything unless --bench is on, and --list says
 * so by naming it.
 */
static int t_trellis_bench(void)
{
    enum { NB = 256, NBIN = 4096 };
    static int qn[NB][16], absc[NB][16], w2[NB][16], out[16];
    static long unmf[NB][16];
    static uint8_t ctxbuf[Y264_CABAC_CTX];
    static uint8_t bins[NBIN], ctxid[NBIN];
    static uint32_t packed[128][2];
    static uint8_t trans2[128][2];
    static uint16_t ent2[128][2];
    static uint8_t mctx[512];
    y264_cabac_t cb;
    uint8_t stt[64];
    double best = 1e30, bp = 1e30, bt = 1e30, bs2 = 1e30;

    if (!ca_bench)
        return 0;
    ca_bench_prep();
    memset(&cb, 0, sizeof cb);
    cb.ctx = ctxbuf;                        /* ctx is a POINTER in y264_cabac_t */
    for (int i = 0; i < Y264_CABAC_CTX; i++) ctxbuf[i] = (uint8_t)(2 * (i % 62) + 1);
    for (int b = 0; b < NB; b++) {
        /* measured shape: last significant coefficient low (mean ~7.8 positions
         * stepped), magnitudes mostly 1, occasional 2-3 */
        int last = 2 + (int)ca_below(12);
        for (int i = 0; i < 16; i++) {
            int live = i <= last && ca_below(100) < 55;
            int mag = !live ? 0 : (ca_below(100) < 78 ? 1 : 1 + (int)ca_below(3));
            qn[b][i] = mag;
            absc[b][i] = mag ? mag * 16 + (int)ca_below(16) : (int)ca_below(6);
            unmf[b][i] = 16 * 16;
            w2[b][i] = 16;
        }
    }
    for (int r = 0; r < BENCH_REPS; r++) {
        double t0 = ca_now_ns();
        for (int i = 0; i < BENCH_ITERS; i++)
            y264_cabac_trellis_4x4(&cb, 2, 0, 0, 400, 16,
                                   qn[i & (NB - 1)], absc[i & (NB - 1)],
                                   unmf[i & (NB - 1)], w2[i & (NB - 1)],
                                   0, NULL, 0, out);
        double ns = (ca_now_ns() - t0) / BENCH_ITERS;
        if (ns < best) best = ns;
    }
    printf("    %-22s %8.1f ns/call   (encoder-level census: ~160 ns)\n",
           "trellis_4x4 lattice", best);

    /* est_decision throughput. Ours is ONE packed lookup (bits << 8 | next
     * state); the variant below is the conventional two-table form, written
     * from the algorithm to test whether the packing is the faster shape on
     * this core. Same bin stream both ways. */
    for (int i = 0; i < NBIN; i++) {
        bins[i] = (uint8_t)(ca_rnd() & 1);
        ctxid[i] = (uint8_t)ca_below(64);   /* small working set, as in a block */
    }
    for (int st = 0; st < 128; st++)
        for (int b = 0; b < 2; b++) {
            uint32_t nxt = (uint32_t)((st + 3 * b) & 127);
            uint32_t bits = (uint32_t)(16 + ((st * 7 + b) & 255));
            packed[st][b] = (bits << 8) | nxt;
            trans2[st][b] = (uint8_t)nxt;
            ent2[st][b]   = (uint16_t)bits;
        }
    for (int r = 0; r < BENCH_REPS; r++) {
        volatile long sinkp = 0;
        double t0;
        memset(stt, 3, sizeof stt);
        t0 = ca_now_ns();
        for (int it = 0; it < BENCH_ITERS; it++) {
            long acc = 0;
            for (int i = 0; i < NBIN; i++) {
                uint8_t *st = &stt[ctxid[i] & 63];
                uint32_t e = packed[*st][bins[i]];
                *st = (uint8_t)e;
                acc += (long)(e >> 8);
            }
            sinkp += acc;
        }
        double ns = (ca_now_ns() - t0) / ((double)BENCH_ITERS * NBIN);
        if (ns < bp) bp = ns;
    }
    for (int r = 0; r < BENCH_REPS; r++) {
        volatile long sinkt = 0;
        double t0;
        memset(stt, 3, sizeof stt);
        t0 = ca_now_ns();
        for (int it = 0; it < BENCH_ITERS; it++) {
            long acc = 0;
            for (int i = 0; i < NBIN; i++) {
                uint8_t *st = &stt[ctxid[i] & 63];
                int is = *st, b = bins[i];
                *st = trans2[is][b];
                acc += ent2[is][b];
            }
            sinkt += acc;
        }
        double ns = (ca_now_ns() - t0) / ((double)BENCH_ITERS * NBIN);
        if (ns < bt) bt = ns;
    }
    printf("    %-22s %8.3f ns/bin  (packed, ours)\n", "est_decision packed", bp);
    printf("    %-22s %8.3f ns/bin  (two-table form)\n", "est_decision two-table", bt);

    /* Feasibility floor for the est-path design brief: a hand-rolled
     * STRAIGHT-LINE whole-MB sizer on our packed primitive, coding the measured
     * typical estimate against a 460-entry context working set. If this lands
     * near the reference encoder's measured 46 ns/estimate, the gap is proven
     * to be the est walk's branchy control flow, not arithmetic. */
    {
        volatile long sink3 = 0;
        for (int r = 0; r < BENCH_REPS; r++) {
            double t0;
            for (int i = 0; i < 512; i++) mctx[i] = (uint8_t)(2 * (i % 62) + 1);
            t0 = ca_now_ns();
            for (int it = 0; it < BENCH_ITERS; it++) {
                long bits = 0;
                uint8_t *cx = mctx;
                /* header: skip, mbtype (3 bins), ref (2), dqp (1) */
                for (int b = 0; b < 7; b++) {
                    uint32_t e = packed[cx[11 + b]][(it >> b) & 1];
                    cx[11 + b] = (uint8_t)e;
                    bits += (long)(e >> 8);
                }
                bits += 256 * 12;                    /* mvd suffix bypass bins */
                for (int b = 0; b < 6; b++) {        /* cbp: 4 luma + 2 chroma */
                    uint32_t e = packed[cx[70 + b]][(it >> b) & 1];
                    cx[70 + b] = (uint8_t)e;
                    bits += (long)(e >> 8);
                }
                int nblk = 2 + (it & 1);             /* 2.55 residual blocks */
                for (int blk = 0; blk < nblk; blk++) {
                    uint8_t *sig = cx + 105 + 16 * blk, *lst = cx + 170 + 16 * blk,
                            *lvl = cx + 230 + 10 * blk;
                    uint32_t e0 = packed[cx[99 + blk]][1];   /* cbf */
                    cx[99 + blk] = (uint8_t)e0;
                    bits += (long)(e0 >> 8);
                    /* 9 coefficients over 13 positions: sig map + last */
                    for (int i = 0; i < 13; i++) {
                        int sbin = (0x1BD5 >> i) & 1;        /* 9 ones */
                        uint32_t e = packed[sig[i]][sbin];
                        sig[i] = (uint8_t)e;
                        bits += (long)(e >> 8);
                        if (sbin) {
                            uint32_t e2 = packed[lst[i]][i == 12];
                            lst[i] = (uint8_t)e2;
                            bits += (long)(e2 >> 8);
                        }
                    }
                    /* levels: 9 lvl1 bins, 2 of them big -> fused unary via the
                     * two-table pair, an upper bound on the fused cost */
                    int node = 0;
                    for (int k = 8; k >= 0; k--) {
                        int big = (k == 2 || k == 6);
                        uint32_t e = packed[lvl[node]][big];
                        lvl[node] = (uint8_t)e;
                        bits += (long)(e >> 8);
                        if (big) {
                            int st = lvl[5 + (node & 3)];
                            lvl[5 + (node & 3)] = trans2[st][1];
                            bits += ent2[st][1] + 256;
                        }
                        node = big ? 4 + (node & 3) : (node < 3 ? node + 1 : node);
                        bits += 256;                 /* sign bypass */
                    }
                }
                sink3 += bits;
            }
            double ns = (ca_now_ns() - t0) / BENCH_ITERS;
            if (ns < bs2) bs2 = ns;
        }
        printf("    %-22s %8.1f ns/estimate  (the reference measures 46; ours 125)\n",
               "straight-line sizer", bs2);
    }
    return 0;
}

const ca_test ca_transform_tests[] = {
#if Y264_HAVE_NEON
    { "fdct4x4",         "transform", Y264_CPU_NEON, t_fdct4x4 },
    { "idct4x4",         "transform", Y264_CPU_NEON, t_idct4x4 },
    { "fdct8x8",         "transform", Y264_CPU_NEON, t_fdct8x8 },
    { "idct8x8",         "transform", Y264_CPU_NEON, t_idct8x8 },
    { "quant_4x4",       "transform", Y264_CPU_NEON, t_quant_4x4 },
    { "quant_f64",       "transform", Y264_CPU_NEON, t_quant_f64 },
    { "dequant",         "transform", Y264_CPU_NEON, t_dequant },
    { "sub_dct",         "transform", Y264_CPU_NEON, t_sub_dct },
    { "add_idct",        "transform", Y264_CPU_NEON, t_add_idct },
    { "sub_dct4_blocks", "transform", Y264_CPU_NEON, t_sub_dct4_blocks },
    { "scan",            "transform", Y264_CPU_NEON, t_scan },
#endif
    { "trellis_bench",   "transform", 0,             t_trellis_bench },
    { NULL, "transform", 0, NULL },
};
