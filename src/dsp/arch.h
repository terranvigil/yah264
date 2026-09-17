/*
 * arch.h - one home for every architecture-specific kernel prototype
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The kernels themselves live in src/dsp/<family>_<arch>.c. Their prototypes
 * used to sit in whichever .c file dispatched to them, which meant checkasm
 * could not name a kernel without redeclaring it, and several groups ended up
 * testing the DISPATCHER instead -- flipping Y264_ASM_OFF to get a C answer and
 * flipping it back to get the kernel's. A harness that can be turned into a
 * C-against-C tautology by an environment variable is not a harness, so the
 * declarations moved here and every check now names the symbol it means.
 *
 * Nothing in this header is a definition and nothing changes what the library
 * compiles to: the dispatching translation units include it instead of
 * repeating the same prototypes.
 *
 * The guard is per ARCH and per DEPTH. Today's kernels are 8-bit aarch64 NEON
 * (16-bit lanes are phase E of the high-bit-depth plan) and the x86-64 section
 * is where the SSE4.2 / AVX2 / AVX-512 twins land; both sections are empty on
 * the other architecture, so a dispatch site's `#if Y264_HAVE_NEON` needs no
 * second opinion about the depth.
 */
#ifndef YAH264_DSP_ARCH_H
#define YAH264_DSP_ARCH_H

#include <stdint.h>

#include "pixel.h"          /* pixel, dctcoef, via common/bitdepth.h */
#include "deblock.h"        /* struct y264_bs_ctx */
#include "../common/cpu.h"  /* Y264_HAVE_NEON/SSE4/AVX2/AVX512 and the tiers */

#if Y264_HAVE_NEON

/* ---- pixel metrics (src/dsp/pixel_neon.c) --------------------------------
 *
 * The _dotprod twins need FEAT_DotProd and are selected over the plain NEON
 * ones only where UDOT measured a win, so BOTH forms are live kernels on a
 * dotprod box and both are registered in checkasm. */
int  y264_sad_16x16_neon(const pixel *, int, const pixel *, int);
int  y264_sad_16x8_neon(const pixel *, int, const pixel *, int);
int  y264_sad_8x16_neon(const pixel *, int, const pixel *, int);
int  y264_sad_8x8_neon(const pixel *, int, const pixel *, int);
int  y264_sad_16x16_neon_dotprod(const pixel *, int, const pixel *, int);
int  y264_sad_8x16_neon_dotprod(const pixel *, int, const pixel *, int);
void y264_sad_x4_16x16_neon(const pixel *, int, const pixel *, const pixel *,
                            const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_16x8_neon(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x16_neon(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x8_neon(const pixel *, int, const pixel *, const pixel *,
                          const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x4_neon(const pixel *, int, const pixel *, const pixel *,
                          const pixel *, const pixel *, int, int[4]);
int  y264_satd_4x4_neon(const pixel *, int, const pixel *, int);
int  y264_satd_8x8_neon(const pixel *, int, const pixel *, int);
void y264_satd_x4_8x8_neon(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
int  y264_satd_16x16_neon_ded(const pixel *, int, const pixel *, int);
int  y264_sa8d_8x8_neon(const pixel *, int, const pixel *, int);
int  y264_sa8d_16x16_neon(const pixel *, int, const pixel *, int);
long y264_hadamard_ac_8x8_neon(const pixel *, int);
long y264_texture_ac4_16x16_neon(const pixel *, int);
void y264_texture_ac48_16x16_neon(const pixel *, int, long[2]);
void y264_var_16x16_neon(const pixel *, int, uint32_t[2]);
void y264_var_16x16_neon_dotprod(const pixel *, int, uint32_t[2]);
void y264_intra4x4_x9_neon(const pixel *, int, const pixel *, int,
                           int, int, int, int, int[9]);
void y264_intra_satd_x3_16x16_neon(const pixel *, int, const pixel *,
                                   const pixel *, int, int[3]);
/* Sum of squared differences; width 16 or 8, any height. Dispatched from the
 * macroblock code rather than the pixel table, which is why the argument type
 * is the 8-bit sample the kernel actually loads. */
int y264_ssd_16xh_neon(const uint8_t *a, int as, const uint8_t *b, int bs, int h);
int y264_ssd_8xh_neon(const uint8_t *a, int as, const uint8_t *b, int bs, int h);

/* ---- transform, quant, scan (src/dsp/transform_neon.c) -------------------
 *
 * The quant and dequant kernels take the flat-CQM multiplier ROW for the qp as
 * an argument -- the caller owns the table -- so a checker names the kernel and
 * supplies the row itself rather than reaching through the dispatcher. */
void y264_quant_4x4_neon(const dctcoef coef[16], dctcoef lev[16], int qp, int intra,
                         const int32_t mfrow[16]);
void y264_quant_4x4_fneon(const dctcoef coef[16], dctcoef lev[16], int qp, int f,
                          const int32_t mfrow[16]);
void y264_quant_8x8_fneon(const dctcoef coef[64], dctcoef lev[64], int qp, int f,
                          const int32_t mfrow[64]);
void y264_dequant_4x4_neon(const dctcoef lev[16], dctcoef coef[16], int qp,
                           const int32_t lsrow[16]);
void y264_dequant_8x8_neon(const dctcoef lev[64], dctcoef coef[64], int qp,
                           const int32_t lsrow[64]);
void y264_fdct4x4_neon(const dctcoef diff[16], dctcoef coef[16]);
void y264_idct4x4_neon(const dctcoef coef[16], dctcoef res[16]);
void y264_fdct8x8_neon(const dctcoef diff[64], dctcoef coef[64]);
void y264_idct8x8_neon(const dctcoef coef[64], dctcoef res[64]);
void y264_sub4x4_dct_neon(dctcoef coef[16], const pixel *src, int ss,
                          const pixel *pred, int ps);
void y264_add4x4_idct_neon(pixel *dst, int ds, const pixel *pred, int ps,
                           const dctcoef coef[16]);
void y264_sub8x8_dct8_neon(dctcoef coef[64], const pixel *src, int ss,
                           const pixel *pred, int ps);
void y264_add8x8_idct8_neon(pixel *dst, int ds, const pixel *pred, int ps,
                            const dctcoef coef[64]);
void y264_sub_dct4_blocks_neon(dctcoef (*coef)[16], int nbw, int nbh,
                               const pixel *src, int ss,
                               const pixel *pred, int ps);
void y264_zigzag_abs_8x8_neon(int out[64], const dctcoef in[64]);
void y264_scan_mask_8x8_neon(const dctcoef lev[64], uint64_t *omsk, int *obig);
void y264_zigzag_scan_4x4_neon(dctcoef out[16], const dctcoef in[16],
                               uint32_t *omsk, int *obig);

/* ---- motion compensation, half-pel planes (src/dsp/mc_neon.c) ------------ */
void y264_mc_luma_neon16(pixel *dst, int dstride, const pixel *ref,
                         int rstride, int ix, int iy, int fx, int fy, int h);
void y264_mc_luma_neon8(pixel *dst, int dstride, const pixel *ref,
                        int rstride, int ix, int iy, int fx, int fy, int h);
void y264_mc_chroma_neon8(pixel *dst, int dstride, const pixel *ref,
                          int rstride, int ix, int iy, int fx, int fy);
void y264_mc_chroma_neon_w4h(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h);
void y264_mc_chroma_neon_w8h(pixel *dst, int dstride, const pixel *ref,
                             int rstride, int ix, int iy, int fx, int fy, int h);
void y264_pred_copy_neon(pixel *dst, int dstride, const pixel *s, int sstride,
                         int w, int h);
void y264_pred_avg2_neon(pixel *dst, int dstride, const pixel *s1,
                         const pixel *s2, int sstride, int w, int h);
void y264_pixel_avg_wt_neon(pixel *dst, const pixel *a, const pixel *b, int n,
                            int w0, int w1);
void y264_hpel_hrow_neon(int32_t *srow, const pixel *row, int x0, int x1);
void y264_hpel_outrow_neon(pixel *Hr, pixel *Vr, pixel *Cr,
                           const int32_t *s0, const int32_t *s1, const int32_t *s2,
                           const int32_t *s3, const int32_t *s4, const int32_t *s5,
                           const pixel *r0, const pixel *r1, const pixel *r2,
                           const pixel *r3, const pixel *r4, const pixel *r5,
                           int x0, int x1);

/* ---- intra prediction (src/dsp/predict_neon.c) ---------------------------
 *
 * 4x4 has no NEON builder on purpose: the blocks are too small to amortize the
 * edge-filter precompute and it measured a net loss. */
void y264_intra16x16_neon(pixel pred[256], const pixel *rec, int stride,
                          int mode, int have_top, int have_left);
void y264_intra_chroma_neon(pixel *pred, const pixel *rec, int stride,
                            int mode, int have_top, int have_left, int cw, int ch);
void y264_intra8x8_neon(pixel pred[64], const pixel *rec, int stride,
                        int mode, int have_top, int have_left,
                        int have_topleft, int have_topright);
void y264_intra8x8_from_edge_neon(pixel pred[64], const pixel e[32], int mode);

/* ---- deblocking (src/dsp/deblock_neon.c) ---------------------------------
 *
 * Vertical chroma edges stay scalar: the shape needs a gather/scatter across
 * the stride and measured 0.87x. 4:4:4 chroma takes the luma-style filter, so
 * it is on the scalar path too. */
void y264_deblock_strength_neon(const struct y264_bs_ctx *c,
                                uint8_t bsv[4][4], uint8_t bsh[4][4]);
void y264_deblock_luma_v4_neon(pixel *q0, int stride, int bs, int alpha,
                               int beta, int tc0);
void y264_deblock_luma_h4_neon(pixel *q0, int stride, int bs, int alpha,
                               int beta, int tc0);
void y264_deblock_chroma8_h_neon(pixel *q0, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g);

#endif /* Y264_HAVE_NEON */

/* ---- x86-64 ---------------------------------------------------------------
 *
 * The tiers are separate translation units compiled with their own -m flags
 * (`src/dsp/x86/<family>_sse4.c`, `_avx2.c`, `_avx512.c`), so the suffix on a
 * name here is the tier its symbol was BUILT for, not merely the widest thing
 * it uses: an AVX2 file built with -mfma may emit an FMA anywhere in it, which
 * is why y264_cpu_tier() demands the tier's whole feature set and not just its
 * headline bit. checkasm registers each tier's rows behind that tier's mask.
 * C11 intrinsics only: there is no assembly in this tree, and hygiene_check
 * refuses any under src/dsp/x86/.
 *
 * The names below are declared and not yet defined. Wave 1 of
 * docs/x86-plan.md defines the pixel family; nothing calls one until then, and
 * the empty family files under src/dsp/x86/ exist so that the build shape --
 * three static libraries, their -m flags, the per-family opt-in lists -- is
 * exercised by every x86 build from now on rather than written blind on the
 * day the first kernel lands. */
#if Y264_HAVE_SSE4

int  y264_sad_16x16_sse4(const pixel *, int, const pixel *, int);
int  y264_sad_16x8_sse4(const pixel *, int, const pixel *, int);
int  y264_sad_8x16_sse4(const pixel *, int, const pixel *, int);
int  y264_sad_8x8_sse4(const pixel *, int, const pixel *, int);
void y264_sad_x4_16x16_sse4(const pixel *, int, const pixel *, const pixel *,
                            const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_16x8_sse4(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x16_sse4(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x8_sse4(const pixel *, int, const pixel *, const pixel *,
                          const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x4_sse4(const pixel *, int, const pixel *, const pixel *,
                          const pixel *, const pixel *, int, int[4]);
int  y264_satd_4x4_sse4(const pixel *, int, const pixel *, int);
int  y264_satd_8x8_sse4(const pixel *, int, const pixel *, int);
void y264_satd_x4_8x8_sse4(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
int  y264_satd_16x16_sse4(const pixel *, int, const pixel *, int);
int  y264_sa8d_8x8_sse4(const pixel *, int, const pixel *, int);
int  y264_sa8d_16x16_sse4(const pixel *, int, const pixel *, int);
long y264_hadamard_ac_8x8_sse4(const pixel *, int);
long y264_texture_ac4_16x16_sse4(const pixel *, int);
void y264_texture_ac48_16x16_sse4(const pixel *, int, long[2]);
void y264_var_16x16_sse4(const pixel *, int, uint32_t[2]);
void y264_intra4x4_x9_sse4(const pixel *, int, const pixel *, int,
                           int, int, int, int, int[9]);
void y264_intra_satd_x3_16x16_sse4(const pixel *, int, const pixel *,
                                   const pixel *, int, int[3]);
int  y264_ssd_16xh_sse4(const uint8_t *a, int as, const uint8_t *b, int bs, int h);
int  y264_ssd_8xh_sse4(const uint8_t *a, int as, const uint8_t *b, int bs, int h);

#endif /* Y264_HAVE_SSE4 */

#if Y264_HAVE_AVX2

int  y264_sad_16x16_avx2(const pixel *, int, const pixel *, int);
int  y264_sad_16x8_avx2(const pixel *, int, const pixel *, int);
int  y264_sad_8x16_avx2(const pixel *, int, const pixel *, int);
int  y264_sad_8x8_avx2(const pixel *, int, const pixel *, int);
void y264_sad_x4_16x16_avx2(const pixel *, int, const pixel *, const pixel *,
                            const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_16x8_avx2(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x16_avx2(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x8_avx2(const pixel *, int, const pixel *, const pixel *,
                          const pixel *, const pixel *, int, int[4]);
void y264_sad_x4_8x4_avx2(const pixel *, int, const pixel *, const pixel *,
                          const pixel *, const pixel *, int, int[4]);
int  y264_satd_4x4_avx2(const pixel *, int, const pixel *, int);
int  y264_satd_8x8_avx2(const pixel *, int, const pixel *, int);
void y264_satd_x4_8x8_avx2(const pixel *, int, const pixel *, const pixel *,
                           const pixel *, const pixel *, int, int[4]);
int  y264_satd_16x16_avx2(const pixel *, int, const pixel *, int);
int  y264_sa8d_8x8_avx2(const pixel *, int, const pixel *, int);
int  y264_sa8d_16x16_avx2(const pixel *, int, const pixel *, int);
long y264_hadamard_ac_8x8_avx2(const pixel *, int);
long y264_texture_ac4_16x16_avx2(const pixel *, int);
void y264_texture_ac48_16x16_avx2(const pixel *, int, long[2]);
void y264_var_16x16_avx2(const pixel *, int, uint32_t[2]);
void y264_intra4x4_x9_avx2(const pixel *, int, const pixel *, int,
                           int, int, int, int, int[9]);
void y264_intra_satd_x3_16x16_avx2(const pixel *, int, const pixel *,
                                   const pixel *, int, int[3]);
int  y264_ssd_16xh_avx2(const uint8_t *a, int as, const uint8_t *b, int bs, int h);
int  y264_ssd_8xh_avx2(const uint8_t *a, int as, const uint8_t *b, int bs, int h);

#endif /* Y264_HAVE_AVX2 */

#if Y264_HAVE_AVX512
/* AVX-512 is a gated experiment (`-Davx512=true`, off by default) for the
 * pixel metrics and MC only, and only once the AVX2 twin of a kernel has
 * shipped and been measured on both Intel and AMD parts. Nothing is declared
 * here ahead of that measurement, because a _avx512 name in this header would
 * read as a decision that has not been taken. */
#endif /* Y264_HAVE_AVX512 */

#endif /* YAH264_DSP_ARCH_H */
