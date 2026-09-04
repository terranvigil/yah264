/*
 * transform.h - H.264 integer transforms and quantization
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * All blocks are in raster order (index = row*4 + col, or row*2 + col for 2x2).
 * The core inverse transform folds in the final (x + 32) >> 6 normalization, so
 * its output is residual samples ready to add to the prediction.
 */
#ifndef YAH264_TRANSFORM_H
#define YAH264_TRANSFORM_H

#include <stdint.h>
#include "../common/bitdepth.h"

/* Custom quantisation matrices (H.264 scaling lists, 8.5.9). Weights are per
 * transform position in RASTER order; flat is the absence of a matrix (weight
 * 16 everywhere), signalled by passing a NULL weight pointer / w0 == 16 to the
 * quant/dequant kernels below. With the fall-back derivation we signal, chroma
 * shares the luma list, so only Intra/Inter matrices are distinct: [0] = Intra,
 * [1] = Inter. */
typedef struct {
    uint8_t w4[2][16];      /* 4x4 Intra / Inter weights, raster order */
    uint8_t w8[2][64];      /* 8x8 Intra / Inter weights, raster order */
} y264_cqm_t;

/* Fill `m` with the H.264 default (JVT) scaling matrices, Table 7-3/7-4,
 * de-zig-zagged into raster order. */
void y264_cqm_jvt(y264_cqm_t *m);

/* 4x4 core transform (ITU-T H.264 8.5.12). The public entry points dispatch
 * to the best kernel for the detected CPU; the _c variants are the portable
 * references (checkasm baselines). The NEON forward transforms are exact for
 * |input| <= 255 (pixel diffs / reconstructed pixels -- every call site);
 * inverse transforms are exact over the full int16 coefficient domain. */
void y264_fdct4x4(const dctcoef diff[16], dctcoef coef[16]);
void y264_idct4x4(const dctcoef coef[16], dctcoef res[16]);
void y264_fdct4x4_c(const dctcoef diff[16], dctcoef coef[16]);
void y264_idct4x4_c(const dctcoef coef[16], dctcoef res[16]);

/* Fused pixel-domain transforms (the subtract-then-transform and
 * inverse-transform-then-add shape x264 also uses). The
 * forward fuses the src - pred subtract into the DCT's first stage; the
 * inverse fuses the residual add + clip into the last. Bit-exact with the
 * unfused sequences every call site used to run: sub == (diff build; fdct)
 * with the same int16 diff intermediate, add == (idct; clip8(pred + res))
 * with the same int16 truncation of the idct output. The _c variants are the
 * portable references (checkasm baselines). */
void y264_sub4x4_dct(dctcoef coef[16], const pixel *src, int ss,
                     const pixel *pred, int ps);
void y264_add4x4_idct(pixel *dst, int ds, const pixel *pred, int ps,
                      const dctcoef coef[16]);
void y264_sub8x8_dct8(dctcoef coef[64], const pixel *src, int ss,
                      const pixel *pred, int ps);
void y264_add8x8_idct8(pixel *dst, int ds, const pixel *pred, int ps,
                       const dctcoef coef[64]);
void y264_sub4x4_dct_c(dctcoef coef[16], const pixel *src, int ss,
                       const pixel *pred, int ps);
void y264_add4x4_idct_c(pixel *dst, int ds, const pixel *pred, int ps,
                        const dctcoef coef[16]);
void y264_sub8x8_dct8_c(dctcoef coef[64], const pixel *src, int ss,
                        const pixel *pred, int ps);

/* Batched forward transform : an
 * nbw x nbh grid of 4x4 blocks in one call, written in RASTER block order
 * (coef[by*nbw + bx]). nbw is even. Bit-exact with the same nbw*nbh
 * y264_sub4x4_dct calls -- the point is that horizontally adjacent blocks
 * share the 8-wide datapath the single-block kernel half-fills, and that the
 * dispatch branch is paid once per grid instead of once per block. Every load
 * stays inside the grid, so an edge macroblock is safe. */
void y264_sub_dct4_blocks(dctcoef (*coef)[16], int nbw, int nbh,
                          const pixel *src, int ss, const pixel *pred, int ps);
void y264_sub_dct4_blocks_c(dctcoef (*coef)[16], int nbw, int nbh,
                            const pixel *src, int ss, const pixel *pred, int ps);
void y264_add8x8_idct8_c(pixel *dst, int ds, const pixel *pred, int ps,
                         const dctcoef coef[64]);

/* Forward-quant multiplier for raster position idx at qp (flat scaling), for the
 * encoder's round-to-nearest skip probe. */
int y264_mf4_at(int idx, int qp);

/* Trellis-RDOQ helpers (see transform.c): inverse-quant multiplier in
 * fdct-coefficient units, and the per-position transform-domain distortion
 * weight (coef_err^2 * w2 == pixel SSD in (pixel^2 * 256 * 25) units). */
long y264_unquant4_mf(int idx, int qp, const uint8_t *w);
int y264_dct4_w2(int idx);
long y264_unquant8_mf(int idx, int qp, const uint8_t *w);
int y264_dct8_w2(int idx);
/* Flat-CQM per-QP rows of the four values above (identical numbers, built by
 * those functions once at open): the rdoq trellis prep loads instead of calling. */
const int  *y264_mf4_row(int qp);
const long *y264_unquant4_row(int qp);
const long *y264_unquant8_row(int qp);
const int *y264_dct4_w2_row(void);
const int *y264_dct8_w2_row(void);
/* The same rows already permuted into zig-zag scan order -- the RDOQ trellis
 * consumes every per-position operand in scan order, so it reads these
 * directly instead of gathering through the scan table per coefficient. */
const long *y264_unquant4_row_zz(int qp);
const long *y264_unquant8_row_zz(int qp);
const int *y264_dct4_w2_row_zz(void);
const int *y264_dct8_w2_row_zz(void);

/* Coefficient zig-zag scan (frame), scan order -> raster index. */
extern const uint8_t y264_zigzag4[16];
extern const uint8_t y264_zigzag8[64];

/* Zig-zag scan kernels . The RDOQ
 * trellis wants absolute magnitudes in scan order; the decimator wants the
 * scan-order nonzero bitmask and whether any |level| >= 2 (a big level forces
 * a keep, so the decimate walk only ever sees +-1). Bit-exact with the
 * per-coefficient gathers they replace. */
void y264_zigzag_abs_4x4(int out[16], const dctcoef in[16]);   /* C only */
void y264_zigzag_abs_8x8(int out[64], const dctcoef in[64]);
void y264_zigzag_abs_8x8_c(int out[64], const dctcoef in[64]);
/* Scan-order nonzero mask + big flag, without materializing the scan. */
void y264_scan_mask_8x8(const dctcoef lev[64], uint64_t *omsk, int *obig);
void y264_scan_mask_8x8_c(const dctcoef lev[64], uint64_t *omsk, int *obig);
/* Scan into `out` and produce the mask + big flag in the same pass. */
void y264_zigzag_scan_4x4(dctcoef out[16], const dctcoef in[16],
                          uint32_t *omsk, int *obig);
void y264_zigzag_scan_4x4_c(dctcoef out[16], const dctcoef in[16],
                            uint32_t *omsk, int *obig);

/* 8x8 transform (ITU-T H.264 8.5.13), High profile. Blocks are raster order
 * (row*8 + col). The inverse folds in the final (x + 32) >> 6 normalization. */
void y264_fdct8x8(const dctcoef diff[64], dctcoef coef[64]);
void y264_idct8x8(const dctcoef coef[64], dctcoef res[64]);
void y264_fdct8x8_c(const dctcoef diff[64], dctcoef coef[64]);
void y264_idct8x8_c(const dctcoef coef[64], dctcoef res[64]);

/* Forward quantize / inverse quantize a full 8x8 block (8.5.13.1). `w` is the
 * 64-entry raster scaling matrix, or NULL for the flat 16 matrix. */
void y264_quant_8x8(const dctcoef coef[64], dctcoef lev[64], int qp, int intra,
                    const uint8_t *w);
void y264_dequant_8x8(const dctcoef lev[64], dctcoef coef[64], int qp,
                      const uint8_t *w);

/* Hadamard transforms for the Intra16x16 luma DC (4x4) and chroma DC (2x2).
 * The same butterfly serves forward and inverse; scaling lives in quant. */
void y264_hadamard4x4(const dctcoef in[16], dctcoef out[16]);
void y264_hadamard2x2(const dctcoef in[4], dctcoef out[4]);

/* Map a luma QP to the chroma QP (8.5.8), with chroma_qp_index_offset folded
 * into `qp_bd_offset_free` by the caller (we pass offset separately). */
int y264_chroma_qp(int qp_luma, int chroma_qp_index_offset);

/* Forward quantize a full 4x4 residual-transform block. `intra` selects the
 * rounding bias. `w` is the 16-entry raster scaling matrix, or NULL for flat.
 * Writes quantized levels (raster). */
void y264_quant_4x4(const dctcoef coef[16], dctcoef lev[16], int qp, int intra,
                    const uint8_t *w);

/* Forward quant with an explicit rounding bias in 1/64-of-step units (32 =
 * round-to-nearest, the reference's trellis-seed semantics). */
void y264_quant_4x4_f64(const dctcoef coef[16], dctcoef lev[16], int qp, int f64,
                        const uint8_t *w);
void y264_quant_8x8_f64(const dctcoef coef[64], dctcoef lev[64], int qp, int f64,
                        const uint8_t *w);

/* Inverse quantize (scale) a 4x4 block of levels back to transform coefficients.
 * `w` is the raster scaling matrix, or NULL for flat. */
/* Resolve the transform env-gated lazy statics on the calling thread (see
 * y264_mb_warm_statics). Idempotent. */
void y264_transform_warm_statics(void);

void y264_dequant_4x4(const dctcoef lev[16], dctcoef coef[16], int qp,
                      const uint8_t *w);

/* Intra16x16 luma DC: forward quant of the 16 Hadamard DC values, and the
 * inverse scaling (8.5.10). `w0` is the DC-position scaling weight (16 = flat). */
void y264_quant_dc_luma(const dctcoef had[16], dctcoef lev[16], int qp, int intra,
                        int w0);
void y264_dequant_dc_luma(const dctcoef lev[16], dctcoef out[16], int qp, int w0);

/* Chroma DC: forward quant of the 4 Hadamard DC values, and inverse scaling
 * (8.5.11). `w0` is the DC-position scaling weight (16 = flat). */
void y264_quant_dc_chroma(const dctcoef had[4], dctcoef lev[4], int qp, int intra,
                          int w0);
void y264_dequant_dc_chroma(const dctcoef lev[4], dctcoef out[4], int qp, int w0);

/* 4:2:2 chroma DC: the 8 DC values form a 4-row x 2-col array (raster
 * [row*2+col]). The 2x4 transform (8.5.11.1, 2-point horizontal + 4-point
 * vertical) is self-inverse up to a factor of 8; the same butterfly serves
 * forward and inverse. The scaling (8.5.11.2) uses qP_DC = chroma qP + 3 and a
 * net >>6, so it needs its own dequant. `qp` is the chroma qP (the +3 is added
 * inside); `w0` is the DC scaling weight (16 = flat). */
void y264_chroma422_dc(const dctcoef in[8], dctcoef out[8]);
void y264_quant_dc_chroma422(const dctcoef f[8], dctcoef lev[8], int qp, int intra,
                             int w0);
void y264_dequant_dc_chroma422(const dctcoef f[8], dctcoef out[8], int qp, int w0);

#endif /* YAH264_TRANSFORM_H */
