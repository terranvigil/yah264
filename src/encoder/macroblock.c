/*
 * macroblock.c - closed-loop intra macroblock coding (I_16x16 luma + chroma)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "macroblock.h"
#include "../common/ledger.h"
#include "skiporacle.h"
#include "../common/stgprof.h"
#include "cavlc.h"
#include "me.h"
#include "../dsp/predict.h"
#include "../dsp/transform.h"
#include "../dsp/mc.h"
#include "../dsp/pixel.h"
#include "../common/cpu.h"
#include "../common/threadpool.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <pthread.h>

/* --- Decision-flip attribution log. Diagnostic
 * only, zero behaviour change: when Y264_MB_LOG=<path> is set, every coded MB
 * emits one line `poc mbx mby slice mode part ref amv j ssd`. Paired fixed-QP
 * encodes (default vs Y264_NO_UMH=1) join on (poc,mbx,mby); a divergent MB is a
 * DECISION FLIP if (slice,mode,part,ref) differs, else an MV difference. Weight
 * by |Δj| (true RD damage) and |Δssd| (distortion damage). Run --threads 1 so
 * the raster log order is deterministic; the mutex only guards accidental
 * multi-thread use. */
static FILE *s_mb_log;
static pthread_mutex_t s_mb_log_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t s_mb_log_once = PTHREAD_ONCE_INIT;
static void mb_log_open(void)
{
    const char *p = getenv("Y264_MB_LOG");
    if (p && *p) s_mb_log = fopen(p, "w");
}
static void mb_log_line(int poc, int mbx, int mby, char slice, int mode,
                        int part, int ref, long amv, long j, long ssd)
{
    pthread_once(&s_mb_log_once, mb_log_open);
    if (!s_mb_log) return;
    pthread_mutex_lock(&s_mb_log_mx);
    fprintf(s_mb_log, "%d %d %d %c %d %d %d %ld %ld %ld\n",
            poc, mbx, mby, slice, mode, part, ref, amv, j, ssd);
    pthread_mutex_unlock(&s_mb_log_mx);
}
static int mb_log_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_MB_LOG"); v = (e && *e) ? 1 : 0; }
    return v;
}

/* --- Y264_BLATE_STAT=<path> (measurement, t1 only): one line per B MB with
 * the final verdict beside the PRE-ME evidence a B early-skip decision could
 * consult -- direct SATD, skip-recon SSD, the lookahead pair legs' lowres
 * costs, lowres-vs-direct MV disagreement, and the mb-tree offset. The
 * question it answers: does the lookahead separate the late-skip population
 * (full tournament, skip verdict) from the coded one, BEFORE any search?
 * Fields: poc mbx mby mode path isref bdist dsatd satd16min c0 c1 ci da0 da1
 * mbtoff qp (mode 0 skip / 1 direct / 2 inter / 3 intra; path 0 full
 * tournament, 1 early-probe commit, 2 mid-tournament exit). */
static FILE *s_blate_fp;
static pthread_once_t s_blate_once = PTHREAD_ONCE_INIT;
static void blate_open(void)
{
    const char *p = getenv("Y264_BLATE_STAT");
    if (p && *p) s_blate_fp = fopen(p, "w");
}
static FILE *blate_fp(void)
{
    pthread_once(&s_blate_once, blate_open);
    return s_blate_fp;
}

/* 4x4 coefficient zig-zag scan: scan order -> raster index (row*4 + col). */
/* Coefficient zig-zag scan, scan order -> raster index. One source of truth
 * with the scan kernels and the scaling-list de-zig-zag: dsp/transform.c. */
#define ZIGZAG  y264_zigzag4
#define ZIGZAG8 y264_zigzag8

/* Scaling-list weight pointers for the active frame. Chroma shares the luma
 * list (as our signalled matrices do), so selection is only Intra vs Inter.
 * NULL means the flat default (byte-identical fast/NEON quant path). */
static const uint8_t *cqm_w4(const y264_frame_t *f, int intra)
{
    return f->cqm ? f->cqm->w4[intra ? 0 : 1] : NULL;
}
static const uint8_t *cqm_w8(const y264_frame_t *f, int intra)
{
    return f->cqm ? f->cqm->w8[intra ? 0 : 1] : NULL;
}
/* DC-position weight (matrix element 0), 16 when flat. */
static int cqm_dc4(const y264_frame_t *f, int intra)
{
    return f->cqm ? f->cqm->w4[intra ? 0 : 1][0] : 16;
}

/* 8x8 luma block (i8x8) top-left position within a macroblock, in 4x4 units. */
static const int B8_X[4] = { 0, 2, 0, 2 };
static const int B8_Y[4] = { 0, 0, 2, 2 };

/* luma4x4BlkIdx -> block position in 4x4 units. */
/* Analysis effort (x264-style subme) is threaded per-call into rdoq_4x4_ctx (its
 * only consumer) rather than held in a file static — W0 thread-safety, so a
 * wavefront can run rows of one frame on different threads. subme<=8 uses the
 * lighter single-pass RDOQ; >=9 the full iterated exact-distortion one. */

static const int BLK_X[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
static const int BLK_Y[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };

/* Inverse of BLK_X/BLK_Y: raster (row*4+col) within the MB -> blkIdx. */
static const int ZIDX[16] = { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 };

/* coded_block_pattern me(v) mapping for Intra_4x4 with ChromaArrayType 1
 * (Table 9-4): codeNum -> cbp. Inverted at use time to encode a cbp. */
static const uint8_t CBP_INTRA[48] = {
    47, 31, 15,  0, 23, 27, 29, 30,  7, 11, 13, 14, 39, 43, 45, 46,
    16,  3,  5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44,  1,  2,  4,
     8, 17, 18, 20, 24,  6,  9, 22, 25, 32, 33, 34, 36, 40, 38, 41
};

static int cbp_to_codenum(int cbp)
{
    for (int i = 0; i < 48; i++)
        if (CBP_INTRA[i] == cbp)
            return i;
    return 0;
}

/* coded_block_pattern me(v) mapping for inter macroblocks (Table 9-4, inter
 * column): codeNum -> cbp. */
static const uint8_t CBP_INTER[48] = {
     0, 16,  1,  2,  4,  8, 32,  3,  5, 10, 12, 15, 47,  7, 11, 13,
    14,  6,  9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41
};

static int cbp_inter_to_codenum(int cbp)
{
    for (int i = 0; i < 48; i++)
        if (CBP_INTER[i] == cbp)
            return i;
    return 0;
}

/* coded_block_pattern me(v) for ChromaArrayType 0/3 (Table 9-4(b)): only the 4
 * luma cbp bits (no chroma cbp), so a 16-entry map. Used for 4:4:4. */
static const uint8_t CBP_INTRA444[16] = { 15, 0, 7, 11, 13, 14, 3, 5, 10, 12, 1, 2, 4, 8, 6, 9 };
static const uint8_t CBP_INTER444[16] = { 0, 1, 2, 4, 8, 3, 5, 10, 12, 15, 7, 11, 13, 14, 6, 9 };
static int cbp444_to_codenum(int cbp, int inter)
{
    const uint8_t *t = inter ? CBP_INTER444 : CBP_INTRA444;
    for (int i = 0; i < 16; i++)
        if (t[i] == cbp) return i;
    return 0;
}

/* One MV-prediction neighbour: motion, refIdx, and whether the block exists
 * (is inside the frame / decoded). An in-frame intra block is available but has
 * refIdx -1 and mv 0. */
typedef struct { int mvx, mvy, ref, avail; } mv_nb_t;

static mv_nb_t nb_at(y264_frame_t *f, int bx, int by)
{
    mv_nb_t n = { 0, 0, -1, 0 };
    if (bx < 0 || by < 0 || bx >= f->wmb * 4 || by >= f->hmb * 4)
        return n;                                   /* not available */
    int i = by * f->mv_stride + bx;
    n.ref = f->refidx[i];
    n.avail = 1;
    /* Intra/unused neighbour (refIdx -1) contributes mv 0 to the median. */
    if (n.ref >= 0) { n.mvx = f->mvx[i]; n.mvy = f->mvy[i]; }
    return n;
}

static int median3(int a, int b, int c)
{
    int mx = a > b ? a : b; mx = mx > c ? mx : c;
    int mn = a < b ? a : b; mn = mn < c ? mn : c;
    return a + b + c - mx - mn;
}

/* Median MV predictor for a 16x16 partition with current refIdx `cref`
 * (8.4.1.3.1/.2). Neighbours that reference the same picture (same refIdx) take
 * priority over the median. */
static void mv_predict(y264_frame_t *f, int mbx, int mby, int cref, int *pmvx, int *pmvy)
{
    int bx = mbx * 4, by = mby * 4;
    mv_nb_t A = nb_at(f, bx - 1, by);               /* left */
    mv_nb_t B = nb_at(f, bx, by - 1);               /* top */
    mv_nb_t C = nb_at(f, bx + 4, by - 1);           /* top-right */
    if (!C.avail)
        C = nb_at(f, bx - 1, by - 1);               /* replace C with D */

    /* If both B and C are unavailable but A is, A stands in for both. */
    if (!B.avail && !C.avail && A.avail)
        B = C = A;

    int mA = (A.ref == cref), mB = (B.ref == cref), mC = (C.ref == cref);
    if (mA + mB + mC == 1) {
        mv_nb_t *o = mA ? &A : (mB ? &B : &C);
        *pmvx = o->mvx; *pmvy = o->mvy;
    } else {
        *pmvx = median3(A.mvx, B.mvx, C.mvx);
        *pmvy = median3(A.mvy, B.mvy, C.mvy);
    }
}

/* nb_at / median predictor generalized to a specific motion field (list 0 or 1),
 * for B-slice per-list MV prediction. */
static mv_nb_t nb_at_f(y264_frame_t *f, int16_t *mx, int16_t *my, int8_t *rf, int bx, int by)
{
    mv_nb_t n = { 0, 0, -1, 0 };
    if (bx < 0 || by < 0 || bx >= f->wmb * 4 || by >= f->hmb * 4)
        return n;
    int i = by * f->mv_stride + bx;
    n.ref = rf[i]; n.avail = 1;
    /* A neighbour that does not use this list (predFlagLX == 0, refIdx -1)
 * contributes mvLX = 0 to the median predictor (8.4.1.3.2). */
    if (n.ref >= 0) { n.mvx = mx[i]; n.mvy = my[i]; }
    return n;
}

static void mv_predict_f(y264_frame_t *f, int16_t *mx, int16_t *my, int8_t *rf,
                         int mbx, int mby, int cref, int *pmvx, int *pmvy)
{
    int bx = mbx * 4, by = mby * 4;
    mv_nb_t A = nb_at_f(f, mx, my, rf, bx - 1, by);
    mv_nb_t B = nb_at_f(f, mx, my, rf, bx, by - 1);
    mv_nb_t C = nb_at_f(f, mx, my, rf, bx + 4, by - 1);
    if (!C.avail)
        C = nb_at_f(f, mx, my, rf, bx - 1, by - 1);
    if (!B.avail && !C.avail && A.avail)
        B = C = A;
    int mA = (A.ref == cref), mB = (B.ref == cref), mC = (C.ref == cref);
    if (mA + mB + mC == 1) {
        mv_nb_t *o = mA ? &A : (mB ? &B : &C);
        *pmvx = o->mvx; *pmvy = o->mvy;
    } else {
        *pmvx = median3(A.mvx, B.mvx, C.mvx);
        *pmvy = median3(A.mvy, B.mvy, C.mvy);
    }
}

/* mb_qp_delta: se(v) for CAVLC, unary at contexts 60/62/63 for CABAC. Codes
 * cur_qp - prev_qp and advances the prediction chain. Call only where the
 * syntax carries mb_qp_delta (cbp > 0, or I_16x16). */
static void qpd_cavlc(y264_bs_t *bs, y264_frame_t *f, int cur_qp)
{
    int d = cur_qp - f->prev_qp;
    y264_bs_write_se(bs, d);
    f->prev_qp = cur_qp;
    f->last_qp_delta = d;
    f->qpd_coded = 1;
}
static void cabac_mb_qp_delta(y264_cabac_t *c, y264_frame_t *f, int cur_qp)
{
    int d = cur_qp - f->prev_qp;
    int code = d > 0 ? 2 * d - 1 : -2 * d;      /* signed -> codeNum (se mapping) */
    for (int i = 0; ; i++) {
        int bit = i < code;
        int ctx = i == 0 ? 60 + (f->last_qp_delta != 0) : (i == 1 ? 62 : 63);
        y264_cabac_encode_decision(c, ctx, bit);
        if (!bit) break;
    }
    f->prev_qp = cur_qp;
    f->last_qp_delta = d;
    f->qpd_coded = 1;
}

/* AQ env overrides, as accessors rather than statics buried in aq_analyze: the
 * warm-up (y264_mb_warm_statics) can only reach a lazy static through a callable
 * accessor, and anything it cannot reach races on first use across GOP workers. */
/* The whole-system x264 mb-tree mode. Declared in macroblock.h; the terms it
 * moves and why they move together are documented at each default site
 * (mbt_ac_gain, the mb-tree strength, aq_anchor_default, aq_chroma_env,
 * crf_ped_env, crf_pbscale_env, aq_mode_env, and the CLI's aq-strength
 * default).
 *
 * It is ONE knob on purpose: substituting halves of x264's mb-tree into our
 * consumption context measures worse than either whole, because the axes are
 * jointly adapted. */
int y264_mbt_derived(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_MBT_DERIVED"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}

/* aq-mode: 2 (autovariance) is ours; x264 medium uses 1 (variance), which under
 * the x264 mode is not merely a metric swap -- mode 1 there is the ABSOLUTE
 * derivation (see the xmode branch in aq_analyze), the same field the mb-tree
 * fold consumes. */
static int aq_mode_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_AQ_MODE");
                 v = e ? atoi(e) : (y264_mbt_derived() ? 1 : 2); }
    return v;
}
static float aq2_bias_env(void)
{
    static float v = -1.0f;
    if (v < 0.0f) { const char *e = getenv("Y264_AQ2_BIAS"); v = e ? (float)atof(e) : 14.0f; }
    return v;
}
static int aq_boost_env(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_AQ_BOOST"); v = e ? atoi(e) : 0; }
    return v;
}
static int aq_octile_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_AQ_OCTILE"); v = e ? atoi(e) : 4; }
    return v;
}
static float aq_dark_env(void)
{
    static float v = -1.0f;
    if (v < 0.0f) { const char *e = getenv("Y264_AQ_DARK"); v = e ? (float)atof(e) : 0.0f; }
    return v;
}

/* AC energy of one block in x264's units: npix * variance, i.e. its
 * <reference-internal> (ssd - sum^2/npix). The luma 16x16 case goes through
 * y264_dsp.var16x16; this covers the chroma footprint, whose size depends on
 * the chroma format. Mirrors blk_ac_energy in encoder.c -- the two must agree,
 * because under the x264 mode this field and the mb-tree's fold are the same
 * field derived twice. */
static double mb_ac_energy(const pixel *p, int stride, int w, int h)
{
    uint64_t s1 = 0, s2 = 0;
    for (int y = 0; y < h; y++) {
        const pixel *r = p + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            uint32_t v = r[x];
            s1 += v; s2 += (uint64_t)v * v;
        }
    }
    double e = (double)s2 - (double)s1 * (double)s1 / (double)(w * h);
    return e > 0 ? e : 0;
}

/* x264's per-frame AQ pass at aq-mode 1, exactly:
 *
 * qp_adj = strength * 1.0397 * (log2(ac_energy) - (14.427 + 2*(BD-8)))
 *
 * with ac_energy summed over every plane. Three things separate it from the
 * shipped autovariance path below, and all three are load-bearing for mb-tree:
 * - the centre is an ABSOLUTE constant, not the frame mean, so the field
 * carries a DC that states how complex this frame is. x264's entire
 * frame-level CRF adaptation IS that DC (see crf_cplx_env in encoder.c);
 * centring on the frame mean removes it.
 * - it is the SAME derivation mbtree_invqscale folds into the mb-tree offset,
 * so a non-reference B -- which codes the AQ field alone, x264
 *-- and its anchor agree about what a flat MB is. Ours
 * diverge: autovariance here, log-variance there.
 * - it is unclamped. The +/-8 below is ours; x264 clips only the final QP,
 * and at strength 1.0 against an absolute anchor the tails are real.
 * The anchor arrives in log2(energy)-8 units, the frame the rest of this
 * encoder's AQ works in (aq_anchor_default). */
static void aq_analyze_absolute(y264_frame_t *f)
{
    int n = f->wmb * f->hmb, ss = f->src_stride[0];
    int chroma = f->aq_chroma && f->src[1] && f->src[2];
    int cw = chroma ? 16 / f->sub_w : 0, ch = chroma ? 16 / f->sub_h : 0;
    int cs = f->src_stride[1];
    float *l = malloc((size_t)n * sizeof(float));
    if (!l) { for (int i = 0; i < n; i++) f->aq_off[i] = 0; return; }
    double sum = 0;
    for (int mby = 0; mby < f->hmb; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++) {
            const pixel *s = f->src[0] + (mby * 16) * ss + mbx * 16;
            uint32_t v2[2];
            y264_dsp.var16x16(s, ss, v2);
            double mean = v2[0] / 256.0, var = v2[1] / 256.0 - mean * mean;
            if (var < 0) var = 0;
            double en = var * 256.0;
            if (chroma)
                en += mb_ac_energy(f->src[1] + (mby * ch) * cs + mbx * cw, cs, cw, ch)
                    + mb_ac_energy(f->src[2] + (mby * ch) * cs + mbx * cw, cs, cw, ch);
            float m = (float)(log2(en < 1.0 ? 1.0 : en) - 8.0);
            l[mby * f->wmb + mbx] = m;
            sum += m;
        }
    /* aq_abs is x264's case and what the mode arms; the frame-mean fallback is
 * only reachable by forcing Y264_CRF_AQABS=0 for attribution. */
    double centre = f->aq_abs ? f->aq_anchor : sum / n;
    double str = f->aq_strength * 1.0397;
    for (int i = 0; i < n; i++) {
        int off = (int)lround(str * (l[i] - centre));
        if (off < -51) off = -51; else if (off > 51) off = 51;
        f->aq_off[i] = (int8_t)off;
    }
    free(l);
}

/* Variance adaptive quantization: lower QP on flat macroblocks (where blocking
 * and banding are visible), raise it on busy ones (where distortion is masked).
 * The offset is strength * (log2 MB variance - frame mean), so it centres on
 * zero and leaves the average QP near the frame target. */
static void aq_analyze(y264_frame_t *f)
{
    if (!f->aq_off) return;
    int n = f->wmb * f->hmb, ss = f->src_stride[0];
    if (f->aq_strength <= 0.f) {
        for (int i = 0; i < n; i++) f->aq_off[i] = 0;
        return;
    }
    if (y264_mbt_derived()) { aq_analyze_absolute(f); return; }
    /* aq-mode: 1 = variance AQ, frame-mean-centred. 2 = auto-variance (x264
 * aq-mode 2, DEFAULT): the per-MB metric is the 8th root of AC energy
 * (gentler than log on busy MBs), the effective strength is scaled by the
 * frame-average metric, and the centring point is corrected by the second
 * moment -- so busy/high-spread frames get gentler AQ, which the flat mode
 * over-corrected (bus/coastguard/tempete resisted a flat strength at every
 * value; mode 2 turns those +0.2..0.4% regressions into -0.7..-1.5% wins).
 * mode-2 vs AQ-off measured a clean -1.05% VMAF-NEG corpus mean, no
 * regression. Env override Y264_AQ_MODE; centre bias Y264_AQ2_BIAS. */
    int aqmode = aq_mode_env();
    float aq2bias = aq2_bias_env();
    /* B1 variance-boost (SVT-AV1-PSY octile AQ, opt-in): the whole-MB variance
 * overstates the flatness of a mixed MB (a flat region beside one edge reads as
 * high-variance -> no protection -> the flat part bands). Also compute a
 * low-octile of the 16 4x4 sub-block variances; where that flattest sub-region is
 * flatter than the frame, boost (lower QP) to protect it -- min(mb_off, low_off),
 * so it never raises QP. Y264_AQ_BOOST on; Y264_AQ_OCTILE picks the sorted index. */
    int aq_boost = aq_boost_env();
    int aq_oct = aq_octile_env();

    float *lv = malloc((size_t)n * sizeof(float));
    float *ml = malloc((size_t)n * sizeof(float));
    float *lowv = aq_boost ? malloc((size_t)n * sizeof(float)) : NULL;
    if (!lv || !ml || (aq_boost && !lowv)) {
        free(lv); free(ml); free(lowv); for (int i = 0; i < n; i++) f->aq_off[i] = 0; return;
    }
    double sum = 0, sum2 = 0;
    for (int mby = 0; mby < f->hmb; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++) {
            const pixel *s = f->src[0] + (mby * 16) * ss + mbx * 16;
            uint32_t s1, s2;
            uint32_t ss1[16] = {0}, ss2[16] = {0};       /* per-4x4 sub-block */
            if (aq_boost) {
                s1 = 0; s2 = 0;
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 16; x++) {
                        int p = s[y * ss + x];
                        s1 += p; s2 += (uint32_t)p * p;
                        int k = (y >> 2) * 4 + (x >> 2); ss1[k] += p; ss2[k] += (uint32_t)p * p;
                    }
            } else {
                uint32_t v2[2];
                y264_dsp.var16x16(s, ss, v2);
                s1 = v2[0]; s2 = v2[1];
            }
            double mean = s1 / 256.0, var = s2 / 256.0 - mean * mean;
            if (var < 0) var = 0;
            /* mode 1: log2(var); mode 2: (AC energy + 1)^(1/8), energy = var*256. */
            float metric = aqmode >= 2 ? powf((float)(var * 256.0) + 1.0f, 0.125f)
                                       : log2f((float)var + 1.0f);
            lv[mby * f->wmb + mbx] = metric;
            ml[mby * f->wmb + mbx] = (float)mean;
            sum += metric; sum2 += (double)metric * metric;
            if (aq_boost) {
                float subv[16];
                for (int k = 0; k < 16; k++) {
                    double sm = ss1[k] / 16.0, sv = ss2[k] / 16.0 - sm * sm;
                    subv[k] = sv > 0 ? (float)sv : 0.0f;
                }
                for (int i = 1; i < 16; i++) {           /* insertion sort ascending */
                    float t = subv[i]; int j = i - 1;
                    while (j >= 0 && subv[j] > t) { subv[j + 1] = subv[j]; j--; }
                    subv[j + 1] = t;
                }
                int o = aq_oct < 1 ? 1 : (aq_oct > 14 ? 14 : aq_oct);
                float lo = (subv[o - 1] + 2.0f * subv[o] + subv[o + 1]) * 0.25f;  /* 1:2:1 blend */
                /* scale the flattest sub-region's variance to the MB-energy metric:
 * "the metric this MB would have if it were as flat as this region". */
                lowv[mby * f->wmb + mbx] = aqmode >= 2 ? powf((float)(lo * 256.0) + 1.0f, 0.125f)
                                                       : log2f(lo + 1.0f);
            }
        }
    float avg = (float)(sum / n);
    /* mode 2: second-moment-corrected centre and auto-scaled strength. */
    float center = avg, strength = f->aq_strength;
    if (aqmode >= 2) {
        float avg2 = (float)(sum2 / n);
        center = avg - 0.5f * (avg2 - aq2bias) / (avg > 1e-3f ? avg : 1e-3f);
        strength = f->aq_strength * avg;
    }
    /* aq-mode 3 (dark bias, opt-in): banding/blocking is most visible in dark
 * flats, so amplify the AQ offset for low-luma MBs. Off by default until
 * measured; Y264_AQ_DARK sets the strength (~0.5-1.0). */
    float darkstr = aq_dark_env();
    for (int i = 0; i < n; i++) {
        float bias = 1.0f;
        if (darkstr > 0.0f) {
            float m = ml[i];                        /* MB mean luma [0,255] */
            float d = (64.0f - m) / 64.0f;          /* >0 for dark MBs (mean<64) */
            if (d > 0.0f) bias = 1.0f + darkstr * (d > 1.0f ? 1.0f : d);
        }
        int off = (int)lroundf(strength * bias * (lv[i] - center));
        if (aq_boost) {                              /* protect the flattest sub-region */
            int offl = (int)lroundf(strength * bias * (lowv[i] - center));
            if (offl < off) off = offl;
        }
        if (off < -8) off = -8;
        if (off > 8) off = 8;
        f->aq_off[i] = (int8_t)off;
    }
    free(lv); free(ml); free(lowv);
}

/* Per-MB QP hooks. mb_qp_pre sets the quant QP for this MB (frame QP plus any
 * AQ offset); mb_qp_post, for a macroblock that coded no mb_qp_delta, resets the
 * CABAC context predictor (its inferred delta is 0). The decoder-visible QPY for
 * the deblock pass is recorded in pass 1 by commit_qpy (W0 step 5), not here. */
static void mb_qp_pre(y264_frame_t *f, int mbx, int mby)
{
    int i = mby * f->wmb + mbx;
    /* When mb-tree runs, its per-MB offset is the x264-style COMBINED offset
 * (qp_offset_aq folded in), so the standalone AQ offset must not be added
 * again. Standalone AQ applies only where mb-tree is off (e.g. bframes 0). */
    int q = f->mbtree_off
          ? (f->mbt_frac ? (int)lround(f->qp + f->mbtree_off[i] / 2.0)
                         : f->qp + f->mbtree_off[i])
          : f->qp + (f->aq_off ? f->aq_off[i] : 0);
    if (q < 0) q = 0;
    if (q > 51) q = 51;
    f->cur_qp = q;
    f->cur_chroma_qp = y264_chroma_qp(q, 0);
    f->cur_qp_scaled = q + Y264_QP_BD_OFFSET;
    f->cur_chroma_qp_scaled = f->cur_chroma_qp + Y264_QP_BD_OFFSET;
    f->qpd_coded = 0;
}
static void mb_qp_post(y264_frame_t *f, int mbx, int mby)
{
    (void)mbx; (void)mby;
    if (!f->qpd_coded)
        f->last_qp_delta = 0;               /* inferred mb_qp_delta 0 */
}

/* Position-deterministic QP of an MB (pure function of the QP map; == mb_qp_pre's
 * cur_qp without the side effects). */
static int mb_cur_qp(const y264_frame_t *f, int mbx, int mby)
{
    int i = mby * f->wmb + mbx;
    int q = f->mbtree_off
          ? (f->mbt_frac ? (int)lround(f->qp + f->mbtree_off[i] / 2.0)
                         : f->qp + f->mbtree_off[i])
          : f->qp + (f->aq_off ? f->aq_off[i] : 0);
    return q < 0 ? 0 : (q > 51 ? 51 : q);
}

/* W1: deterministic prev_qp for the pass-1 mb_qp_delta pricing -- the QP map's
 * raster predecessor (design's approximation), so analysis is position-independent
 * of the true chain and thus wavefront-parallelisable. The entropy pass still
 * codes the real raster chain. Identical to the true chain under CQP (all deltas
 * 0) and whenever the predecessor codes residual (the common case); differs only
 * for a post-skip/no-residual predecessor -> BD-gated. */
static int predict_prev_qp(const y264_frame_t *f, int mbx, int mby)
{
    if (mbx == 0 && mby == 0) return f->qp;             /* slice-init (SliceQPY) */
    return mbx > 0 ? mb_cur_qp(f, mbx - 1, mby)
                   : mb_cur_qp(f, f->wmb - 1, mby - 1);
}

/* Position-independent mb_qp_delta pricing: the RD trials price se(cur_qp - prev_qp)
 * against the QP-MAP raster predecessor (predict_prev_qp), NOT the true entropy
 * chain. This is what makes analysis thread-count-INDEPENDENT: the true chain's
 * prev_qp depends on which predecessors coded a delta (order/skip-sensitive under
 * non-uniform QP from mb-tree/AQ/CRF), so a serial-vs-wavefront or 2-vs-8-thread
 * split diverges. DEFAULT ON -- it restores byte-identical output
 * across all thread counts (proven on the conformance syn_320x240 determinism
 * check) and is BD-neutral (+0.00% VMAF-NEG, 6-clip CIF, vs the true chain).
 * Y264_WF_PREDQP=0 escapes to the true raster chain, which is NON-deterministic
 * across threads: a byte-identity canary only. */
static int wf_predqp_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_WF_PREDQP"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}

/* P_Skip MV (8.4.1.1): zero when a required neighbour is unavailable or is a
 * zero-MV refIdx-0 block, otherwise the median predictor. */
static void mv_skip(y264_frame_t *f, int mbx, int mby, int *smvx, int *smvy)
{
    int bx = mbx * 4, by = mby * 4;
    mv_nb_t A = nb_at(f, bx - 1, by);
    mv_nb_t B = nb_at(f, bx, by - 1);
    if (!A.avail || !B.avail ||
        (A.ref == 0 && A.mvx == 0 && A.mvy == 0) ||
        (B.ref == 0 && B.mvx == 0 && B.mvy == 0)) {
        *smvx = 0; *smvy = 0;
    } else {
        mv_predict(f, mbx, mby, 0, smvx, smvy);
    }
}

static void set_mb_motion(y264_frame_t *f, int mbx, int mby, int mvx, int mvy, int ref)
{
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++) {
            int i = (mby * 4 + by) * f->mv_stride + (mbx * 4 + bx);
            f->mvx[i] = (int16_t)mvx;
            f->mvy[i] = (int16_t)mvy;
            f->refidx[i] = (int8_t)ref;
        }
}

/* Reset BOTH list fields of a macroblock to "intra/unused" (refIdx -1). A B MB's
 * partition trials write list-1 motion (search_b_part -> set_region_motion_f on
 * f->refidx1) into the grid during analysis; if intra then wins, resetting only
 * list-0 (set_mb_motion) leaves refidx1 >= 0, so deblock's is_intra (needs both
 * lists < 0) misfires and the boundary strength disagrees with the decoder. */
static void set_mb_intra_motion(y264_frame_t *f, int mbx, int mby)
{
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++) {
            int i = (mby * 4 + by) * f->mv_stride + (mbx * 4 + bx);
            f->mvx[i] = f->mvy[i] = f->mvx1[i] = f->mvy1[i] = 0;
            f->refidx[i] = f->refidx1[i] = -1;
        }
}

/* Set motion for a rectangular region of 4x4 blocks (partition), in absolute
 * 4x4 coordinates. */
static void set_region_motion(y264_frame_t *f, int bx0, int by0, int w4, int h4,
                              int mvx, int mvy, int ref)
{
    for (int by = 0; by < h4; by++)
        for (int bx = 0; bx < w4; bx++) {
            int i = (by0 + by) * f->mv_stride + (bx0 + bx);
            f->mvx[i] = (int16_t)mvx;
            f->mvy[i] = (int16_t)mvy;
            f->refidx[i] = (int8_t)ref;
        }
}

static void save_mb_mv(y264_frame_t *f, int mbx, int mby, int16_t *bx, int16_t *by, int8_t *br)
{
    int k = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int i = (mby * 4 + y) * f->mv_stride + (mbx * 4 + x);
            bx[k] = f->mvx[i]; by[k] = f->mvy[i]; br[k] = f->refidx[i]; k++;
        }
}

static void load_mb_mv(y264_frame_t *f, int mbx, int mby, const int16_t *bx, const int16_t *by, const int8_t *br)
{
    int k = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int i = (mby * 4 + y) * f->mv_stride + (mbx * 4 + x);
            f->mvx[i] = bx[k]; f->mvy[i] = by[k]; f->refidx[i] = br[k]; k++;
        }
}

static void save_mb_mv_f(y264_frame_t *f, const int16_t *mx, const int16_t *my,
                         const int8_t *rf, int mbx, int mby,
                         int16_t *bx, int16_t *by, int8_t *br)
{
    int k = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int i = (mby * 4 + y) * f->mv_stride + (mbx * 4 + x);
            bx[k] = mx[i]; by[k] = my[i]; br[k] = rf[i]; k++;
        }
}

static void load_mb_mv_f(y264_frame_t *f, int16_t *mx, int16_t *my, int8_t *rf,
                         int mbx, int mby, const int16_t *bx, const int16_t *by,
                         const int8_t *br)
{
    int k = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int i = (mby * 4 + y) * f->mv_stride + (mbx * 4 + x);
            mx[i] = bx[k]; my[i] = by[k]; rf[i] = br[k]; k++;
        }
}

/* MV predictor for a partition (8.4.1.3.1), matching x264's directional-then-
 * count logic. `bx0,by0` is the partition's top-left 4x4 (absolute), `w4` its
 * width in 4x4 units, `part` the mb partition (1=16x8, 2=8x16), `pidx` 0/1. */
static void partition_mvp_f(y264_frame_t *f, int16_t *mx, int16_t *my, int8_t *rf,
                            int bx0, int by0, int w4, int part, int pidx, int cref,
                            int *pmvx, int *pmvy)
{
    mv_nb_t A = nb_at_f(f, mx, my, rf, bx0 - 1, by0);
    mv_nb_t B = nb_at_f(f, mx, my, rf, bx0, by0 - 1);
    mv_nb_t C = nb_at_f(f, mx, my, rf, bx0 + w4, by0 - 1);
    /* The 16x8 bottom partition's top-right neighbour sits in the raster-later
 * MB to the right: not yet decoded, so not available (Table 6-3). The
 * bottom-right 8x8 of a B_8x8 (part 3, pidx 3) has the same problem for the
 * same reason -- its C is (bx0+2, by0-1), which is the next macroblock. */
    if ((part == 1 && pidx == 1) || (part == 3 && pidx == 3))
        C.avail = 0;
    if (!C.avail) {
        C = nb_at_f(f, mx, my, rf, bx0 - 1, by0 - 1);
    }
    int mA = (A.ref == cref), mB = (B.ref == cref), mC = (C.ref == cref);

    if (part == 1 && pidx == 0 && mB) { *pmvx = B.mvx; *pmvy = B.mvy; return; }
    if (part == 1 && pidx == 1 && mA) { *pmvx = A.mvx; *pmvy = A.mvy; return; }
    if (part == 2 && pidx == 0 && mA) { *pmvx = A.mvx; *pmvy = A.mvy; return; }
    if (part == 2 && pidx == 1 && mC) { *pmvx = C.mvx; *pmvy = C.mvy; return; }

    int count = mA + mB + mC;
    if (count == 1) {
        mv_nb_t *o = mA ? &A : (mB ? &B : &C);
        *pmvx = o->mvx; *pmvy = o->mvy;
    } else if (count == 0 && !B.avail && !C.avail && A.avail) {
        *pmvx = A.mvx; *pmvy = A.mvy;
    } else {
        *pmvx = median3(A.mvx, B.mvx, C.mvx);
        *pmvy = median3(A.mvy, B.mvy, C.mvy);
    }
}

static void partition_mvp(y264_frame_t *f, int bx0, int by0, int w4,
                          int part, int pidx, int cref, int *pmvx, int *pmvy)
{
    partition_mvp_f(f, f->mvx, f->mvy, f->refidx, bx0, by0, w4, part, pidx,
                    cref, pmvx, pmvy);
}

/* Set motion for a rectangular region on an explicit list's grids. */
static void set_region_motion_f(y264_frame_t *f, int16_t *mx, int16_t *my, int8_t *rf,
                                int bx0, int by0, int w4, int h4,
                                int mvx, int mvy, int ref)
{
    for (int by = 0; by < h4; by++)
        for (int bx = 0; bx < w4; bx++) {
            int i = (by0 + by) * f->mv_stride + (bx0 + bx);
            mx[i] = (int16_t)mvx;
            my[i] = (int16_t)mvy;
            rf[i] = (int8_t)ref;
        }
}

/* MV predictor for a P_8x8 sub-partition: the general median (8.4.1.3.1), no
 * directional shortcut. C (top-right) needs decode-order availability: a cell
 * in the raster-later MB to the right is not yet decoded, and an in-MB cell is
 * only decoded once the sub-partition search has committed it (refidx >= 0 --
 * uncommitted cells still hold the per-frame reset value -1, and in-MB cells
 * are never intra while evaluating an inter MB). A and B are always decoded by
 * sub-partition scan order. */
static void sub_mvp(y264_frame_t *f, int bx0, int by0, int w4, int mbx, int mby,
                    int cref, int *pmvx, int *pmvy)
{
    mv_nb_t A = nb_at(f, bx0 - 1, by0);
    mv_nb_t B = nb_at(f, bx0, by0 - 1);
    int cx = bx0 + w4, cy = by0 - 1;
    mv_nb_t C = nb_at(f, cx, cy);
    if (cy >= mby * 4 &&
        (cx > mbx * 4 + 3 || f->refidx[cy * f->mv_stride + cx] < 0))
        C.avail = 0;
    if (!C.avail)
        C = nb_at(f, bx0 - 1, by0 - 1);
    if (!B.avail && !C.avail && A.avail)
        B = C = A;
    int mA = (A.ref == cref), mB = (B.ref == cref), mC = (C.ref == cref);
    if (mA + mB + mC == 1) {
        mv_nb_t *o = mA ? &A : (mB ? &B : &C);
        *pmvx = o->mvx; *pmvy = o->mvy;
    } else {
        *pmvx = median3(A.mvx, B.mvx, C.mvx);
        *pmvy = median3(A.mvy, B.mvy, C.mvy);
    }
}

/* Clamp a reconstructed sample to the valid pixel range [0, PIXEL_MAX]
 * (255 at 8-bit, 1023 at 10-bit). Name kept for churn; range is bit-depth aware. */
static inline int clip8(int v) { return v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v); }

/* Implicit weighted-biprediction weights (8.4.2.3.2), derived from POC distances
 * of the used list-0 reference `l0ref` and list-1 reference 0. w0 + w1 == 64;
 * falls back to (32,32) (= the plain (a+b+1)/2 average) when the geometry is
 * degenerate or the scale factor is out of range. */
static void bipred_weights(y264_frame_t *f, int l0ref, int *w0, int *w1)
{
    *w0 = 32; *w1 = 32;
    if (!f->weighted_bipred) return;
    int poc_l0 = f->refs_poc[l0ref];
    int td = f->poc_l1 - poc_l0;
    if (td < -128) td = -128; else if (td > 127) td = 127;
    if (td == 0) return;
    int tb = f->poc - poc_l0;
    if (tb < -128) tb = -128; else if (tb > 127) tb = 127;
    int tx = (16384 + abs(td / 2)) / td;
    int dsf = (tb * tx + 32) >> 6;
    if (dsf < -1024) dsf = -1024; else if (dsf > 1023) dsf = 1023;
    int w = dsf >> 2;
    if (w < -64 || w > 128) return;
    *w1 = w; *w0 = 64 - w;
}

/* Weighted average of two predictions: (a*w0 + b*w1 + 32) >> 6, clipped. With
 * (32,32) this is exactly (a + b + 1) >> 1. */
#define bipred_avg y264_pixel_avg_wt

/* Explicit luma weighted prediction (8.4.2.3.1) applied in place to a P-slice
 * luma prediction block: Clip1( ((p * w + 2^(D-1)) >> D) + o ). No-op unless
 * active for reference r. `stride` lets it run over a strided rec block or a
 * packed prediction. */
static void apply_wp_luma(y264_frame_t *f, pixel *pred, int stride, int bw, int bh, int r)
{
    if (!f->wp_luma[r]) return;
    /* luma_offset_l0 is signaled in the 8-bit domain; the decoder applies it as
 * offset << (BitDepthY - 8) (8.4.2.3.2). */
    int w = f->wp_w[r], o = f->wp_o[r] << (Y264_BIT_DEPTH - 8);
    int D = f->wp_denom, rnd = D ? (1 << (D - 1)) : 0;
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            pixel *p = &pred[y * stride + x];
            *p = (pixel)clip8(((*p * w + rnd) >> D) + o);
        }
}

/* nC context from the left and top 4x4 neighbours (9.2.1). Grid cells are -1
 * when outside the frame. */
static int cbf_nb(y264_frame_t *f, int comp, int bx, int by, int intra);

static int derive_nc(const int8_t *grid, int stride, int bx, int by)
{
    int a = (bx > 0) ? grid[by * stride + (bx - 1)] : -1;
    int b = (by > 0) ? grid[(by - 1) * stride + bx] : -1;
    if (a >= 0 && b >= 0) return (a + b + 1) >> 1;
    if (a >= 0) return a;
    if (b >= 0) return b;
    return 0;
}

/* Sum of 4x4 SATD over a block, using the dispatched kernels. Full 16x16 / 8x8
 * tiles use the fused kernels (one call, inlined 4x4s); remainders fall to 4x4.
 * Bit-identical to looping satd4x4 -- same value, fewer indirect calls. */
static int satd_block(const pixel *src, int ss, const pixel *pred, int ps,
                      int w, int h)
{
    if (w == 16 && h == 16)
        return y264_dsp.satd16x16(src, ss, pred, ps);
    int s = 0, by = 0;
    for (; by + 8 <= h; by += 8) {
        int bx = 0;
        for (; bx + 8 <= w; bx += 8)
            s += y264_dsp.satd8x8(src + by*ss + bx, ss, pred + by*ps + bx, ps);
        for (; bx < w; bx += 4)
            for (int y = 0; y < 8; y += 4)
                s += y264_dsp.satd4x4(src + (by+y)*ss + bx, ss, pred + (by+y)*ps + bx, ps);
    }
    for (; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4)
            s += y264_dsp.satd4x4(src + by*ss + bx, ss, pred + by*ps + bx, ps);
    return s;
}

/* Sum of squared differences (distortion) between two blocks. */
static int ssd_block(const pixel *a, int as, const pixel *b, int bs,
                     int w, int h)
{
    NLED(ssd_call, 1); NLED(ssd_pix, (uint64_t)w*h);
#if defined(__aarch64__) && Y264_BIT_DEPTH == 8
    int have_neon = y264_asm_on(Y264_ASM_SSD);   /* cached in cpu.c */
    if (have_neon) {
        if (w == 16) return y264_ssd_16xh_neon(a, as, b, bs, h);
        if (w == 8)  return y264_ssd_8xh_neon(a, as, b, bs, h);
    }
#endif
    int s = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int d = a[y * as + x] - b[y * bs + x];
            s += d * d;
        }
    return s;
}

/* Bits to CAVLC-code a 4x4 block's scan-order levels (nC=0; only relative bit
 * counts matter for RDOQ, and nC is a constant offset for a given TotalCoeff). */
static int block_bits(const dctcoef scan[16], int maxc)
{
    NLED(cavlc_scratch, 1);
    return y264_cavlc_residual_len(scan, maxc, 0, 1);
}

/* psy-trellis strength x256 (Y264_PSY_TRELLIS, default 0 = off). x264's
 * psy-trellis idea: inside the quant search, reward reconstructions that keep
 * AC frequency energy (SSD alone prefers zeroing coefficients, which blurs
 * texture). Scoring the RESIDUAL's energy at block level is content-dependent
 * and loses; what works is scoring the RECONSTRUCTION's spectrum per
 * coefficient (prediction + residual), which this does exactly:
 * J -= psy * sum_AC |fdct(clip(pred + res))|. Everything stays in the genuine
 * forward-transform domain (no dequant-vs-fdct scale mapping). */
/* psy-trellis strength x256 for this frame. Comes from param.psy_trellis
 * (f->psy_trellis), threaded per-frame so it is thread-safe under the wavefront.
 * Y264_PSY_TRELLIS overrides it (any value, incl 0) for A/B testing. */
/* Psy-in-Viterbi gate (Y264_PSY_VITERBI, default 0): with psy_trellis armed,
 * run the psy objective INSIDE the Viterbi lattice (see trellis_core) instead
 * of falling back to the greedy search -- the greedy path is the +30%-wall
 * refusal that keeps both psy class gates default-off. The lattice form drops
 * only the greedy's clip coupling; its class wins are gated separately. */
static int psy_viterbi_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PSY_VITERBI"); v = e ? atoi(e) : 0; }
    return v;
}

static int psy_trellis_env(void)
{
    static int env = -2;   /* -2 = unread, -1 = unset, >=0 = override x256 */
    if (env == -2) { const char *e = getenv("Y264_PSY_TRELLIS");
                     env = e ? (int)(atof(e) * 256.0 + 0.5) : -1; }
    return env;
}
static int psy_trellis_x256(const y264_frame_t *f)
{
    int env = psy_trellis_env();
    if (env >= 0) return env;
    return (int)(f->psy_trellis * 256.0f + 0.5f);
}
/* Deep-quant psy ramp. The psy-trellis wins grow with strength on grain and
 * its losses are mid-band;
 * the deep-quant retention deficit is high-QP only. So: a per-MB-QP FLOOR on
 * the psy-trellis strength -- 0 at qp<=qp0, linear to s_x256 at qp>=qp1 --
 * that never touches the mid band and composes with --tune grain by max.
 * Y264_PSY_TRELLIS_RAMP=<qp0>,<qp1>,<s_x256>; unset = off = byte-identical. */
static const int *psy_ramp_env(void)
{
    static int r[3] = { -3, 0, 0 };
    if (r[0] == -3) {
        int v[3] = { -1, 0, 0 };
        const char *e = getenv("Y264_PSY_TRELLIS_RAMP");
        for (int i = 0; e && i < 3; i++) {
            v[i] = atoi(e);
            e = strchr(e, ',');
            if (e) e++;
        }
        if (v[1] <= v[0] || v[2] <= 0) v[0] = -1;   /* malformed: off */
        r[1] = v[1]; r[2] = v[2];
        r[0] = v[0];
    }
    return r;
}
/* Returns strength_x256 | (lattice_flag << 16): gate-originated psy asks for
 * the Viterbi-lattice form; consumers mask with 0xFFFF for the strength. */
static int psy_trellis_qp(const y264_frame_t *f, int qp)
{
    int lat = f->psy_lattice ? (1 << 16) : 0;
    int base = psy_trellis_x256(f);
    const int *r = psy_ramp_env();
    if (r[0] >= 0 && qp > r[0]) {
        int v = qp >= r[1] ? r[2] : r[2] * (qp - r[0]) / (r[1] - r[0]);
        if (v > base) return v | lat;
    }
    return base ? (base | lat) : 0;
}
/* B partition orientation early-terminate slack, /16 (0 disables). */
static int bpo_env(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_BPO"); v = e ? atoi(e) : 18; }
    return v;
}
/* Y264_PROBE_DEADZONE=1: pre-seed-fix skip-probe admissions (diagnostic). */
static int probe_deadzone_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PROBE_DEADZONE"); v = e ? atoi(e) : 0; }
    return v;
}

/* J-distortion of a 4x4 candidate: (SSD << 8) minus, with psy-trellis on and a
 * prediction block supplied (luma only), the AC-spectrum retention reward.
 * With psy off or pred NULL this is exactly (block_dist << 8). */
static long block_J(const dctcoef lev[16], const dctcoef diff[16], int qp,
                    const uint8_t *w, const pixel *pred, int ps, int psy256)
{
    dctcoef coef[16], res[16];
    y264_dequant_4x4(lev, coef, qp, w);
    y264_idct4x4(coef, res);
    long d = 0;
    for (int i = 0; i < 16; i++) {
        int e = res[i] - diff[i];
        d += (long)e * e;
    }
    long J = d << 8;
    if (pred && psy256 > 0) {
        dctcoef rec[16], rdct[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                rec[y * 4 + x] = (dctcoef)clip8(pred[y * ps + x] + res[y * 4 + x]);
        y264_fdct4x4(rec, rdct);
        long s = 0;
        for (int k = 1; k < 16; k++) s += labs(rdct[k]);   /* AC only */
        J -= (long)psy256 * s;
    }
    return J;
}

/* RD-optimized quantization of one 4x4 luma block. Starts from the naive
 * quantization, then greedily eliminates trailing coefficients and lowers levels
 * where it reduces J = SSD(pixel-domain) + lambda*bits. Distortion is exact (via
 * idct) so there is no coefficient-vs-pixel scaling to get wrong. */
/* ac: this is an AC-only block (I_16x16 luma AC, or chroma AC) whose DC is
 * coded separately, so force position 0 to zero and trellis the rest. The DC
 * distortion term is then constant across trellis choices and cancels out. */
/* Bit cost of a 4x4's scan-order levels under the active entropy coder: CAVLC
 * table costs, or the CABAC estimate from the engine's current context states
 * (cb non-NULL; cat 1 = I16 luma AC, 2 = luma 4x4, 4 = chroma AC -- the AC
 * categories code 15 coefficients from scan position 1). Returns bits x256.
 * The CABAC coded_block_flag neighbour terms are approximated as 0/0: only
 * that one bin's context choice is affected. */
static long scan_bits_4x4(const y264_cabac_t *cb, int cat, const dctcoef scan[16],
                          int nza, int nzb)
{
    if (!cb)
        return (long)block_bits(scan, 16) << 8;
    if (cat == 2)
        return y264_cabac_residual_bits(cb, 2, scan, nza, nzb);
    return y264_cabac_residual_bits(cb, cat, scan + 1, nza, nzb);
}

/* Single-forward-Viterbi RDOQ: one reverse Viterbi pass with transform-domain
 * distortion + incremental sig/last + running level-context rate, in place of
 * the greedy RDOQ (block_J idct + full entropy re-walk per trial, iterated to
 * convergence for 8x8). BD-neutral-to-better (7-clip VMAF-NEG -0.24%) and faster
 * (~1.1-1.3x pure-C). Gate: it is the medium-tier RDOQ (subme <= 8); subme >= 9
 * keeps the full greedy so the max-quality default stays byte-identical. Env
 * Y264_VITERBI forces on(1)/off(0) for A/B. Only CABAC (cb != NULL) and non-psy
 * blocks use it -- CAVLC and psy-trellis keep the greedy search. */
/* Transform-size pre-decision (SA8D-vs-SATD, encode one size) at the medium tier
 * (subme<=8); subme>=9 encodes both and RD-picks (byte-identical). Y264_TR_PRE
 * forces on(1)/off(0). */
static int tr_pre_on(int subme)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_TR_PRE"); env = e ? atoi(e) : -1; }
    if (env >= 0) return env;
    return (subme > 0 ? subme : 10) <= 8;
}

/* RDOQ/trellis seed bias in 1/64-of-step units (x264 quant*_bias0 semantics).
 * x264's trellis seeds from ROUND-TO-NEAREST (bias 32/64) and considers
 * {q-1, q}, so a marginal HF coefficient (frac >= 0.5) always enters the
 * candidate set and RD decides keep-vs-drop. Our legacy seed is the JM inter
 * deadzone (bias 10.67/64): frac in [.5,.833) seeds one level low, and a
 * would-be level-1 coefficient seeds at 0 -- candidate set {0} -- so it is
 * dropped with NO RD consideration (the away-nudge skips zeros too). That is a
 * systematic HF-suppression bias no downstream search can undo -- measured as
 * the bulk of the base-coding VMAF-NEG floor vs x264 (reproducer gaps
 * bus +9.6/foreman +10.8/coastguard +12.0 -> +3.8/+4.7/+3.1 with seed 32 +
 * the aligned skip probe; shipped-config corpus mean -2.8%, park_joy -6.7%,
 * no clip regressing). DEFAULT 32 (behaviour-matched round-to-nearest; a value
 * sweep confirmed 32 over 24/40). Escape: Y264_RDOQ_SEED64=-1 restores the
 * legacy deadzone seed. */
static int rdoq_seed64(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_RDOQ_SEED64"); v = e ? atoi(e) : 32; }
    return v;
}

static int viterbi_rdoq(int subme)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_VITERBI"); env = e ? (e[0] != '0' ? 1 : 0) : -1; }
    if (env >= 0) return env;
    return (subme > 0 ? subme : 10) <= 8;
}

/* S4/W-C trellis-at-commit (x264 trellis=1 placement): RD trials quantise with
 * the plain deadzone (no Viterbi, no greedy search) and only the winning mode
 * is re-encoded with the full RDOQ before commit. This is where x264 medium
 * puts its trellis; running the Viterbi in EVERY trial is 31% of the pure-C
 * profile. DEFAULT ON at the medium tier for the measured trade: 1.18x CIF /
 * 1.29x 720p pure-C for +1.42% VMAF-NEG BD. Y264_TRELLIS_COMMIT=0 restores
 * trellis-in-trials; subme>=9 is untouched (byte-identical max quality).
 * Thread-local: set/cleared inside one MB's analysis, which runs whole on one
 * worker; the emit and I-frame paths never set it, so they keep the full RDOQ. */
static _Thread_local int s_rd_trial = 0;

/* The RDOQ level a call should run at, folding the trial flag into the frame's
 * setting. It exists to be HOISTED: on macOS every read of a _Thread_local is
 * a _tlv_get_addr call, and reading s_rd_trial inside rdoq_4x4_ctx costs
 * sixteen walks per luma plane, four more per chroma component, once per RD
 * candidate. The callers below read it once per block loop instead and pass
 * the result in the `trellis` argument the kernels already take, which is
 * byte-identical because trial mode and trellis 0 select the same deadzone
 * branch. The ME thread-locals carry the same cost, which is why those are
 * one struct. */
static inline int rd_trellis(const y264_frame_t *f)
{
    return s_rd_trial ? 0 : f->trellis;
}

static int trellis_commit_on(int subme, int trellis)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_TRELLIS_COMMIT"); env = e ? atoi(e) : 1; }
    /* trellis 2 = RDOQ in every mode decision, so the commit-only placement is
 * exactly what it turns off. Level 0 never reaches here (the quantiser
 * short-circuits), and level 1 is the shipped default. */
    if (trellis >= 2) return 0;
    return env > 0 && (subme > 0 ? subme : 10) <= 8;
}

/* src/ss + pred/ps are the block's source and prediction (any strides); the
 * pixel-diff residual the greedy search scores against is rebuilt from them on
 * demand -- the hot Viterbi/trial paths never need it, which is what lets the
 * call sites run the fused sub-dct without a separate diff build. psy_ok
 * gates the psy-trellis reward (luma only). */
static int resprof_on(void);   /* defined below; used by the trellis counters */
enum { TP_QUANT = 0, TP_SETUP, TP_LATTICE, TP_OUT, TP_N };
static int trprof_on(void);
static inline uint64_t tp_now(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
extern _Atomic uint64_t g_tp_ns[], g_tp_calls;
extern _Atomic uint64_t g_tp_cat[], g_tp_cat_tr[];
extern int y264_tl_on;
extern _Atomic uint64_t y264_tl_calls, y264_tl_coef, y264_tl_node;
extern _Atomic uint64_t y264_est_bins, y264_est_bypass;
extern _Atomic uint64_t y264_est_coefs;
extern _Atomic uint64_t y264_real_coefs;
/* G3: is the CABAC emit path dominated by the REAL emitter or by the RD
 * estimator running the same code in est_mode? */
static _Atomic uint64_t g_em_real, g_em_est;
static _Atomic uint64_t g_blk8_est, g_blk4_est;   /* residual BLOCK visits in est mode */
#define TP_ADD(ph, t0) do { if (tp_on) { uint64_t _t = tp_now(); \
        atomic_fetch_add_explicit(&g_tp_ns[ph], _t - (t0), memory_order_relaxed); (t0) = _t; } } while (0)
static void rdoq_4x4_ctx(const y264_cabac_t *cb, const pixel *src, int ss,
                     const pixel *pred, int ps,
                     const dctcoef coef[16], dctcoef lev[16], int qp, int lambda,
                     int intra, int ac, int cat, const uint8_t *w, int nza, int nzb,
                     int psy_ok, int subme, int psy_trel, int trellis)
{
    int psy_raw = psy_ok ? psy_trel : 0;
    int psy = psy_raw & 0xFFFF;
    /* trellis 0 (x264 --trellis 0): no RDOQ anywhere, plain deadzone. The S4
 * trial path already does exactly this, so level 0 is that path taken
 * unconditionally rather than only inside an RD trial. */
    if (trellis == 0) {                /* deadzone only */
        y264_quant_4x4(coef, lev, qp, intra, w);
        if (ac) lev[0] = 0;
        return;
    }
    const int tp_on = trprof_on();
    uint64_t tp_t = 0;
    if (tp_on) { atomic_fetch_add_explicit(&g_tp_calls, 1, memory_order_relaxed);
                 atomic_fetch_add_explicit(&g_tp_cat[cat & 7], 1, memory_order_relaxed);
                 tp_t = tp_now(); }
    int s64 = rdoq_seed64();
    if (s64 >= 0) y264_quant_4x4_f64(coef, lev, qp, s64, w);
    else          y264_quant_4x4(coef, lev, qp, intra, w);
    if (ac) lev[0] = 0;

    /* All-zero early-out: trailing-elimination can only zero more, and the
 * level nudges only touch already-nonzero coefficients, so an all-zero
 * naive quant stays all-zero. Skip the block_J idct + entropy estimate
 * (the initial bestJ) entirely -- output is bit-identical. */
    {
        int any = 0;
        for (int k = 0; k < 16; k++) if (lev[k]) { any = 1; break; }
        if (!any) { TP_ADD(TP_QUANT, tp_t); return; }
    }
    TP_ADD(TP_QUANT, tp_t);

    if (resprof_on()) { extern _Atomic uint64_t g_rp_tr4;
        atomic_fetch_add_explicit(&g_rp_tr4, 1, memory_order_relaxed); }
    if (tp_on) atomic_fetch_add_explicit(&g_tp_cat_tr[cat & 7], 1, memory_order_relaxed);
    int psyv = psy > 0 && ((psy_raw >> 16) || psy_viterbi_on());
    if (cb && (!psy || psyv) && viterbi_rdoq(subme)) {
        int base = ac ? 1 : 0;
        int n = ac ? 15 : 16;            /* AC blocks (cat 1/4) drop the DC */
        int qn[16], absc[16], w2[16], out[16];
        int psyp[16];
        const int *psyp_p = NULL;
        int psy_lo = 0;
        if (psyv) {
            /* fdct(pred), sign-folded per coefficient: the lattice reward is
 * |sgn*P + unq(level)| == |P + sgn*unq|. DC excluded on full
 * blocks (psy_lo), matching block_J's AC-only sum. */
            static const pixel Z16[16];
            dctcoef P[16];
            y264_sub4x4_dct(P, pred, ps, Z16, 4);
            for (int i = 0; i < n; i++) {
                int r = ZIGZAG[i + base];
                psyp[i] = (coef[r] < 0 ? -P[r] : P[r]);
            }
            psyp_p = psyp;
            psy_lo = base ? 0 : 1;
        }
        long unmf[16];
        const long *unmf_p = unmf;
        const int *w2_p = w2;
        int *qn_p = qn, *absc_p = absc;
        if (!w) {
            /* Flat CQM: the trellis reads every operand in scan order, so the
 * unquant / distortion-weight rows are read from their scan-order
 * copies (no gather, no copy) and the two coefficient arrays come
 * from one scan kernel each. The AC categories drop the DC by
 * starting the same scan one position in. */
            y264_zigzag_abs_4x4(qn, lev);
            y264_zigzag_abs_4x4(absc, coef);
            qn_p = qn + base; absc_p = absc + base;
            unmf_p = y264_unquant4_row_zz(qp) + base;
            w2_p = y264_dct4_w2_row_zz() + base;
        } else for (int i = 0; i < n; i++) {
            int r = ZIGZAG[i + base];
            int lv = lev[r];
            qn[i] = lv < 0 ? -lv : lv;
            int cf = coef[r];
            absc[i] = cf < 0 ? -cf : cf;
            unmf[i] = y264_unquant4_mf(r, qp, w);
            w2[i] = y264_dct4_w2(r);
        }
        TP_ADD(TP_SETUP, tp_t);
        y264_cabac_trellis_4x4(cb, cat, nza, nzb, lambda, n, qn_p, absc_p, unmf_p, w2_p,
                               psyv ? psy : 0, psyp_p, psy_lo, out);
        TP_ADD(TP_LATTICE, tp_t);
        if (ac) lev[0] = 0;
        for (int i = 0; i < n; i++) {
            int r = ZIGZAG[i + base];
            int s = coef[r] < 0 ? -1 : 1;
            lev[r] = (dctcoef)(s * out[i]);
        }
        TP_ADD(TP_OUT, tp_t);
        return;
    }

    /* Greedy search: rebuild the pixel-diff residual (same subtract, same
 * int16 narrowing as the call site's own diff). */
    dctcoef diff[16];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            diff[y * 4 + x] = (dctcoef)(src[y * ss + x] - pred[y * ps + x]);

    dctcoef scan[16];
    for (int k = 0; k < 16; k++) scan[k] = lev[ZIGZAG[k]];
    long bestJ = block_J(lev, diff, qp, w, pred, ps, psy)
               + (long)lambda * scan_bits_4x4(cb, cat, scan, nza, nzb);

    /* Greedy trailing elimination + toward-zero level-lowering, iterated to
 * convergence: lowering one coefficient can make lowering another (or
 * clearing a now-trailing run) worthwhile, and a coefficient that should
 * walk several steps toward zero needs more than the single pass. Each
 * accepted move strictly lowers J, so this terminates. */
    /* subme>=9: full iterated bidirectional search (max quality). subme<=8: a
 * single pass with toward-zero nudging only (x264 trellis=1-class cost) --
 * much cheaper, tiny quality cost. */
    int full = (subme > 0 ? subme : 10) >= 9;
    int changed = 1, pass = 0;
    while (changed && (full || pass == 0)) {
        changed = 0; pass++;
        /* Eliminate trailing (highest-frequency) coefficients while it helps. */
        for (;;) {
            int last = -1;
            for (int k = 15; k >= 0; k--)
                if (lev[ZIGZAG[k]]) { last = k; break; }
            if (last < 0) break;
            int pos = ZIGZAG[last], saved = lev[pos];
            lev[pos] = 0;
            for (int k = 0; k < 16; k++) scan[k] = lev[ZIGZAG[k]];
            long j = block_J(lev, diff, qp, w, pred, ps, psy)
                   + (long)lambda * scan_bits_4x4(cb, cat, scan, nza, nzb);
            if (j < bestJ) { bestJ = j; changed = 1; }
            else { lev[pos] = saved; break; }
        }
        /* Nudge individual levels by one where it helps. Try toward zero
 * (fewer bits) and, since the naive deadzone rounds aggressively down,
 * also away from zero (toward round-to-nearest, less distortion) --
 * the away move recovers coefficients the deadzone over-suppressed,
 * which pays off at low QP where distortion dominates. Exact pixel
 * distortion keeps every candidate honestly scored. */
        for (int k = 0; k < 16; k++) {
            int pos = ZIGZAG[k], v = lev[pos];
            if (!v) continue;
            int saved = v;
            int down = v > 0 ? v - 1 : v + 1;   /* toward zero */
            lev[pos] = down;
            for (int s = 0; s < 16; s++) scan[s] = lev[ZIGZAG[s]];
            long jd = block_J(lev, diff, qp, w, pred, ps, psy)
                    + (long)lambda * scan_bits_4x4(cb, cat, scan, nza, nzb);
            if (jd < bestJ) { bestJ = jd; changed = 1; continue; }
            if (!full) { lev[pos] = saved; continue; }   /* light: no away probe */
            int up = v > 0 ? v + 1 : v - 1;     /* away from zero */
            lev[pos] = up;
            for (int s = 0; s < 16; s++) scan[s] = lev[ZIGZAG[s]];
            long ju = block_J(lev, diff, qp, w, pred, ps, psy)
                    + (long)lambda * scan_bits_4x4(cb, cat, scan, nza, nzb);
            if (ju < bestJ) { bestJ = ju; changed = 1; }
            else lev[pos] = saved;
        }
    }
}

/* Bits to code an 8x8 block's 64 zig-zag levels: CAVLC codes it as four
 * interleaved 4x4 sub-blocks (scan8[4*i+j] -> sub j), so sum the four costs. */
static int block_bits_8x8(const dctcoef scan8[64])
{
    int total = 0;
    for (int j = 0; j < 4; j++) {
        NLED(cavlc_scratch, 1);
        total += y264_cavlc_residual_len(scan8 + j, 16, 0, 4);   /* in place */
    }
    return total;
}

/* 8x8 J-distortion with the psy-trellis AC-retention reward (see block_J). */
static long block_J_8x8(const dctcoef lev[64], const dctcoef diff[64], int qp,
                        const uint8_t *w, const pixel *pred, int ps, int psy256)
{
    dctcoef coef[64], res[64];
    y264_dequant_8x8(lev, coef, qp, w);
    y264_idct8x8(coef, res);
    long d = 0;
    for (int i = 0; i < 64; i++) { int e = res[i] - diff[i]; d += (long)e * e; }
    long J = d << 8;
    if (pred && psy256 > 0) {
        dctcoef rec[64], rdct[64];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                rec[y * 8 + x] = (dctcoef)clip8(pred[y * ps + x] + res[y * 8 + x]);
        y264_fdct8x8(rec, rdct);
        long s = 0;
        for (int k = 1; k < 64; k++) s += labs(rdct[k]);   /* AC only */
        /* The 8x8 transform has ~4x the coefficient gain of the 4x4 (same
 * pixels spread over 4x the basis support); >>2 puts the reward on the
 * 4x4 scale so one strength serves both transform sizes. */
        J -= ((long)psy256 * s) >> 2;
    }
    return J;
}

/* RD-optimized quantization of one 8x8 block: naive quant, then greedy trailing
 * elimination and level-lowering in zig-zag order where each reduces
 * J = SSD + lambda*bits. Distortion is exact via the 8x8 dequant/idct. */
static long scan_bits_8x8(const y264_cabac_t *cb, const dctcoef scan[64])
{
    if (!cb)
        return (long)block_bits_8x8(scan) << 8;
    return y264_cabac_residual_8x8_bits(cb, scan);
}

/* Scan-order nonzero mask + |level|>=2 flag of an 8x8 level block -- the exact
 * values a zigzag re-gather after rdoq_8x8 would compute. */
#define scan_mask_8x8 y264_scan_mask_8x8

/* src/pred block pointers + psy_ok: see rdoq_4x4_ctx. omsk/obig (optional,
 * both or neither): scan-order nonzero mask + big flag of the OUTPUT levels,
 * so callers need not re-gather the 64-entry zigzag -- the Viterbi path folds
 * it into its write-back, the all-zero exit is free, and the deadzone/greedy
 * exits run the identical walk the caller would. */
static void rdoq_8x8(const y264_cabac_t *cb, const pixel *src, int ss,
                     const pixel *pred, int ps,
                     const dctcoef coef[64], dctcoef lev[64], int qp, int lambda,
                     int intra, const uint8_t *w, int psy_ok, int psy_trel,
                     int subme, uint64_t *omsk, int *obig, int trellis)
{
    int psy_raw = psy_ok ? psy_trel : 0;
    int psy = psy_raw & 0xFFFF;
    if (trellis == 0) {                /* deadzone only (see rdoq_4x4_ctx) */
        y264_quant_8x8(coef, lev, qp, intra, w);
        if (omsk) scan_mask_8x8(lev, omsk, obig);
        return;
    }
    int s64 = rdoq_seed64();
    if (s64 >= 0) y264_quant_8x8_f64(coef, lev, qp, s64, w);
    else          y264_quant_8x8(coef, lev, qp, intra, w);
    /* All-zero early-out (see rdoq_4x4_ctx): stays all-zero, skip the initial
 * block_J idct + entropy estimate. Bit-identical output. */
    {
        int any = 0;
        for (int k = 0; k < 64; k++) if (lev[k]) { any = 1; break; }
        if (!any) {
            if (omsk) { *omsk = 0; *obig = 0; }
            return;
        }
    }

    if (resprof_on()) { extern _Atomic uint64_t g_rp_tr8;
        atomic_fetch_add_explicit(&g_rp_tr8, 1, memory_order_relaxed); }
    int psyv = psy > 0 && ((psy_raw >> 16) || psy_viterbi_on());
    if (cb && (!psy || psyv) && viterbi_rdoq(subme)) {
        int qn[64], absc[64], w2[64], out[64];
        long unmf[64];
        const long *unmf_p = unmf;
        const int *w2_p = w2;
        int psyp[64];
        const int *psyp_p = NULL;
        if (psyv) {
            /* see rdoq_4x4_ctx; the 8x8 reward carries block_J_8x8's >>2
 * transform-gain compensation on the strength. */
            static const pixel Z64[64];
            dctcoef P[64];
            y264_sub8x8_dct8(P, pred, ps, Z64, 8);
            for (int i = 0; i < 64; i++) {
                int r = ZIGZAG8[i];
                psyp[i] = (coef[r] < 0 ? -P[r] : P[r]);
            }
            psyp_p = psyp;
        }
        if (!w) {                               /* flat CQM: see rdoq_4x4_ctx */
            y264_zigzag_abs_8x8(qn, lev);
            y264_zigzag_abs_8x8(absc, coef);
            unmf_p = y264_unquant8_row_zz(qp);
            w2_p = y264_dct8_w2_row_zz();
        } else for (int i = 0; i < 64; i++) {   /* scan order */
            int r = ZIGZAG8[i];
            int lv = lev[r];
            qn[i] = lv < 0 ? -lv : lv;
            int cf = coef[r];
            absc[i] = cf < 0 ? -cf : cf;
            unmf[i] = y264_unquant8_mf(r, qp, w);
            w2[i] = y264_dct8_w2(r);
        }
        y264_cabac_trellis_8x8(cb, lambda, qn, absc, unmf_p, w2_p,
                               psyv ? psy >> 2 : 0, psyp_p, 1, out);
        uint64_t msk = 0;
        int big = 0;
        for (int i = 0; i < 64; i++) {          /* out[i] >= 0: msk/big fold in free */
            int r = ZIGZAG8[i];
            int s = coef[r] < 0 ? -1 : 1;
            lev[r] = (dctcoef)(s * out[i]);
            msk |= (uint64_t)(out[i] != 0) << i;
            big |= out[i] >= 2;
        }
        if (omsk) { *omsk = msk; *obig = big; }
        return;
    }

    /* Greedy search: rebuild the pixel-diff residual (see rdoq_4x4_ctx). */
    dctcoef diff[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            diff[y * 8 + x] = (dctcoef)(src[y * ss + x] - pred[y * ps + x]);

    dctcoef scan[64];
    for (int k = 0; k < 64; k++) scan[k] = lev[ZIGZAG8[k]];
    long bestJ = block_J_8x8(lev, diff, qp, w, pred, ps, psy)
               + (long)lambda * scan_bits_8x8(cb, scan);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (;;) {
            int last = -1;
            for (int k = 63; k >= 0; k--) if (lev[ZIGZAG8[k]]) { last = k; break; }
            if (last < 0) break;
            int pos = ZIGZAG8[last], saved = lev[pos];
            lev[pos] = 0;
            for (int k = 0; k < 64; k++) scan[k] = lev[ZIGZAG8[k]];
            long j = block_J_8x8(lev, diff, qp, w, pred, ps, psy)
                   + (long)lambda * scan_bits_8x8(cb, scan);
            if (j < bestJ) { bestJ = j; changed = 1; } else { lev[pos] = saved; break; }
        }
        for (int k = 0; k < 64; k++) {
            int pos = ZIGZAG8[k], v = lev[pos];
            if (!v) continue;
            int saved = v;
            int down = v > 0 ? v - 1 : v + 1;   /* toward zero */
            lev[pos] = down;
            for (int s = 0; s < 64; s++) scan[s] = lev[ZIGZAG8[s]];
            long jd = block_J_8x8(lev, diff, qp, w, pred, ps, psy)
                    + (long)lambda * scan_bits_8x8(cb, scan);
            if (jd < bestJ) { bestJ = jd; changed = 1; continue; }
            int up = v > 0 ? v + 1 : v - 1;     /* away from zero (deadzone recovery) */
            lev[pos] = up;
            for (int s = 0; s < 64; s++) scan[s] = lev[ZIGZAG8[s]];
            long ju = block_J_8x8(lev, diff, qp, w, pred, ps, psy)
                    + (long)lambda * scan_bits_8x8(cb, scan);
            if (ju < bestJ) { bestJ = ju; changed = 1; }
            else lev[pos] = saved;
        }
    }
    if (omsk) scan_mask_8x8(lev, omsk, obig);
}

/* Lagrangian multiplier for RD mode decision, J = SSD + lambda*bits. Follows
 * the standard H.264 model lambda = 0.85 * 2^((QP-12)/3), rounded to int. */
static int lambda_mode(int qp)
{
    static const int tab[52] = {
        0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,2,2,3,3,4,5,7,9,11,14,17,
        22,27,34,43,54,69,86,109,137,173,218,274,345,435,548,691,870,
        1097,1381,1741,2193,2763,3482,4386,5526,6963
    };
    return tab[qp < 0 ? 0 : (qp > 51 ? 51 : qp)];
}

/* x264 does not run its trellis on the
 * mode lambda -- a squared-lambda curve at 0.85^2 (inter) / 0.65^2
 * (intra) against lambda2's 0.9, so its coefficient bits are priced at ~0.80x
 * (inter) and ~0.47x (intra) of mode RD, while all nine RDOQ sites here reuse
 * lambda_mode. Y264_TRELLIS_LAMBDA=<inter_x256>,<intra_x256> scales the lambda
 * handed to those sites and to the skip probe (which must price like the inter
 * coder it predicts). Default 256,256 is byte-identical. */
static const int *trellis_lambda_env(void)
{
    /* {inter_x256, intra_x256, qp0, qp1}. qp0<0 = constant scale; otherwise
 * the scale is 256 at qp<=qp0 and ramps linearly to the configured value
 * at qp>=qp1 -- the matched-rate band killed the unconditional form
 * (median +0.71%) while the crf 32-41 band won 11/12, so the discount is
 * priced only where the deep-quant mechanism lives. */
    static int s[4] = { -1, -1, -1, -1 };
    if (s[0] < 0) {
        /* shipped default: intra at x264's 0.65^2/0.85^2 ratio, ramped in over
 * qp 28..38. Gates: crf 32-41 band 11/12 negative (median -0.77%),
 * matched-rate band median +0.01% worst bus +0.74% (a rate lean,
 * +0.6-0.9% bits for +0.2 NEG at its bottom rung), t12 wall inside
 * the control's spread, determ 16/16 under load. Escape
 * Y264_TRELLIS_LAMBDA=256,256 restores the unramped bitstream. */
        int v[4] = { 256, 150, 28, 38 };
        const char *e = getenv("Y264_TRELLIS_LAMBDA");
        if (e) { v[0] = 256; v[1] = 256; v[2] = -1; v[3] = -1; }
        for (int i = 0; e && i < 4; i++) {
            v[i] = atoi(e);
            e = strchr(e, ',');
            if (e) e++;
        }
        if (v[3] <= v[2]) v[2] = -1;            /* degenerate ramp: constant */
        s[1] = v[1] > 0 ? v[1] : 256;
        s[2] = v[2]; s[3] = v[3];
        s[0] = v[0] > 0 ? v[0] : 256;
    }
    return s;
}
static int lambda_trellis(int qp, int intra)
{
    const int *s = trellis_lambda_env();
    int sc = s[intra ? 1 : 0];
    if (sc != 256 && s[2] >= 0) {
        if (qp <= s[2])     sc = 256;
        else if (qp < s[3]) sc = 256 + ((sc - 256) * (qp - s[2])) / (s[3] - s[2]);
    }
    int l = lambda_mode(qp);
    return sc == 256 ? l : (int)(((long)l * sc + 128) >> 8);
}

/* ME / partition / ref Lagrangian in the SATD/SAD domain (the standard
 * ~2^((qp-12)/6)). A linear `4 + qp/4` over-penalizes MVD ~2x at medium QP --
 * suppressing the large, predictor-distant motion vectors erratic content needs
 * and leaving that energy in the residual -- and under-penalizes ~3-5x at high
 * QP. Y264_ME_LAMBDA=0 selects that linear value for the A/B. */
static int me_lambda_old(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_LAMBDA"); v = (e && e[0] == '0') ? 1 : 0; }
    return v;
}
/* Motion-search lambda: the standard exponential rate-distortion weight,
 * lambda(qp) = 2^((qp-12)/6), clamped at 1. Doubling every 6 QP is the same
 * relation the quantiser itself follows -- qstep doubles per 6 QP by
 * construction (8.5.9) -- which is why this exponent and not a fitted one.
 *
 * Evaluated rather than tabulated, in integer arithmetic: split the exponent
 * into whole octaves and a remainder, take the remainder from six Q16
 * sixth-octave constants, and shift. Checked exact against a float
 * round(2^((qp-12)/6)) reference on all 52 QPs, and the encoder's output is
 * byte-identical to the tabulated form it replaces. */
static int lambda_me(int qp)
{
    /* Q16 2^(r/6), r = 0..5 */
    static const uint32_t sixth[6] = { 65536, 73562, 82570, 92682, 104032, 116772 };
    int q = qp < 0 ? 0 : (qp > 51 ? 51 : qp);
    if (me_lambda_old())
        return 4 + q / 4;
    int k = q - 12;
    if (k < 0)
        return 1;
    int v = (int)(((((uint64_t)sixth[k % 6] << (k / 6)) + 32768)) >> 16);
    return v < 1 ? 1 : v;
}

/* RD lambda in 1/16 units. lambda_mode's table is the squared-lambda curve
 * scaled by a near-constant 0.94 and rounded to int. The constant scale is
 * harmless -- it cancels from every RD comparison. The
 * ROUNDING does not: its error oscillates (qp19 -6%, qp20 -7%, qp21 +3%,
 * qp22 +5.5%), so two macroblocks one QP apart are compared on relative lambdas
 * about 10% wrong. With a frame-level lambda that is one constant per slice and
 * invisible; per-MB it distorts RD BETWEEN macroblocks.
 *
 * Confirmed by operating point: per-MB lambda on ducks reads +6.49% at high rate
 * (qp 16-24, lambda 2-14, error +/-6%) and -0.64% at low rate (qp 28-38, lambda
 * 34-435, error <1%) -- same clip, opposite sign, flipping exactly where the
 * table stops being coarse.
 *
 * So carry lambda at 4 extra bits and shift the product. With the gate OFF the
 * value is exactly 16x the integer table, so the shift recovers that integer and
 * the default path stays byte-identical -- which is also the check that no use
 * site was missed. */
static int lambda16_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_LAMBDA16"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}

static long lambda_mode16(int qp)
{
    /* the squared-lambda curve * 0.94 / 16, rounded -- four extra fractional bits. */
    static const long fine[52] = {
        1,1,1,2,2,3,3,4, 5,7,9,11,14,17,21,27,
        34,43,54,68,86,108,136,172, 217,273,344,433,546,688,867,1092,
        1376,1733,2184,2752,3467,4368,5504,6934, 8737,11008,13868,17474,
        22016,27737,34947,44032, 55475,69894,88064,110950
    };
    static const int tab[52] = {
        0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,2,2,3,3,4,5,7,9,11,14,17,
        22,27,34,43,54,69,86,109,137,173,218,274,345,435,548,691,870,
        1097,1381,1741,2193,2763,3482,4386,5526,6963
    };
    int q = qp < 0 ? 0 : (qp > 51 ? 51 : qp);
    return lambda16_on() ? fine[q] : 16L * tab[q];
}

/* J-domain products: lambda is in 1/16 units, so shift (or divide) it back out. */
#define Y264_LAMJ(l, b)  (((long)(l) * (long)(b)) >> 4)
#define Y264_LAMJD(l, b) ((double)(l) * (double)(b) / 16.0)

/* Save/restore a macroblock's nnz grid cells (luma 4x4 + chroma 2x2 per comp),
 * so a trial coding pass used only to measure bits has no lasting effect. */
/* THE ROW WIDTH HAS TO BE A COMPILE-TIME CONSTANT HERE TOO.
 *
 * Walking the nnz grid one BYTE at a time -- sixteen loads for the luma 4x4
 * and cbw*cbh more per chroma plane -- costs 1.8% of the shipped wall on
 * samsung, because these three run once per RD candidate. A row is four
 * bytes of luma or two of
 * chroma, so it is one load once clang knows the length, and it only knows it
 * behind the format branch: f->cbw is a runtime read. Same bytes, same order,
 * same buffer layout. */
#define MB_NNZ_LUMA(BODY)                                                      \
    do {                                                                       \
        int s_ = f->nnz_stride[0];                                             \
        int8_t *g_ = f->nnz[0] + (mby * 4) * s_ + mbx * 4;                     \
        for (int by = 0; by < 4; by++) { BODY; }                               \
    } while (0)
#define MB_NNZ_CHROMA(W, BODY)                                                 \
    do {                                                                       \
        for (int c = 1; c < 3; c++) {                                          \
            int s_ = f->nnz_stride[c], h_ = f->cbh;                            \
            int8_t *g_ = f->nnz[c] + (mby * h_) * s_ + mbx * (W);              \
            for (int by = 0; by < h_; by++) { BODY; k += (W); }                \
        }                                                                      \
    } while (0)

static void save_mb_nnz(y264_frame_t *f, int mbx, int mby, int8_t *buf)
{
    MB_NNZ_LUMA(memcpy(buf + by * 4, g_ + by * s_, 4));
    int k = 16;
    if (f->cbw == 2) MB_NNZ_CHROMA(2, memcpy(buf + k, g_ + by * s_, 2));
    else             MB_NNZ_CHROMA(4, memcpy(buf + k, g_ + by * s_, 4));
}

static void load_mb_nnz(y264_frame_t *f, int mbx, int mby, const int8_t *buf)
{
    MB_NNZ_LUMA(memcpy(g_ + by * s_, buf + by * 4, 4));
    int k = 16;
    if (f->cbw == 2) MB_NNZ_CHROMA(2, memcpy(g_ + by * s_, buf + k, 2));
    else             MB_NNZ_CHROMA(4, memcpy(g_ + by * s_, buf + k, 4));
}

/* Save/restore the mb_qp_delta prediction chain around a trial coding pass. The
 * write functions call qpd_cavlc/cabac_mb_qp_delta, which advance prev_qp; a
 * scratch pass done only to measure bits must not perturb the real chain (this is
 * invisible without AQ, since then every cur_qp == prev_qp and the delta is 0). */
struct qp_chain { int prev_qp, last_qp_delta, qpd_coded; };
static void qp_save(const y264_frame_t *f, struct qp_chain *s)
{
    s->prev_qp = f->prev_qp; s->last_qp_delta = f->last_qp_delta; s->qpd_coded = f->qpd_coded;
}
static void qp_load(y264_frame_t *f, const struct qp_chain *s)
{
    f->prev_qp = s->prev_qp; f->last_qp_delta = s->last_qp_delta; f->qpd_coded = s->qpd_coded;
}

/* RD distortion of a reconstructed macroblock vs the source (luma + chroma).
 * Y264_PSY_CHROMA_X256 scales the CHROMA share (256 = x1.0 =
 * byte-identical default). The psy energy term inflates only the luma half of
 * the metric, which silently deflates chroma's relative weight in every RD
 * verdict; x264 compensates with i_chroma_lambda2_offset under psy -- this is
 * our form of the same rebalance, applied to the distortion side. */
static int psy_chroma_x256(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PSY_CHROMA_X256"); v = e ? atoi(e) : 256; }
    return v;
}
static int ssd_mb(y264_frame_t *f, int mbx, int mby)
{
    int d = ssd_block(f->src[0] + (mby*16)*f->src_stride[0] + mbx*16, f->src_stride[0],
                      f->rec[0] + (mby*16)*f->rec_stride[0] + mbx*16, f->rec_stride[0],
                      16, 16);
    {
    int cw = 16 / f->sub_w, ch = 16 / f->sub_h;
    long cd = 0;
    for (int c = 1; c < 3; c++)
        cd += ssd_block(f->src[c] + (mby*ch)*f->src_stride[c] + mbx*cw, f->src_stride[c],
                        f->rec[c] + (mby*ch)*f->rec_stride[c] + mbx*cw, f->rec_stride[c],
                        cw, ch);
    d += (int)((cd * psy_chroma_x256()) >> 8);
    }
    return d;
}

/* Both luma AC texture energies of a 16x16 block, in one dsp call:
 * te[0] = SATD support (each 4x4 tile's SATD against a flat block of its own
 * rounded mean), te[1] = SA8D support (the four 8x8 Hadamard AC energies).
 * The RD metric always wants the pair for the same block, and the 8x8
 * coefficients are one 2x2 butterfly away from the 4x4 tile coefficients, so
 * the kernel transforms the pixels once for both. */
static void texture_energy(const pixel *p, int stride, long te[2])
{
    y264_dsp.texture_ac48_16x16(p, stride, te);
}


/* MB distortion for RD decisions: SSD plus, with psy-RD on, a penalty for the
 * reconstruction's texture energy departing from the source's (x264's psy-rd
 * idea: SSD alone prefers blur -- a skip or flat mode that averages texture
 * away scores well on SSD but looks worse than a noisier, energy-preserving
 * choice). Decision-only: the reconstruction itself stays spec-exact.
 * The flat psy_rd weight is the measured optimum for THIS metric under the
 * VMAF gate: x264 scales its term by the ME lambda(QP), but applied to our summed
 * 4x4-tile AC SATD it measured BD-VMAF losses at both the full scale and a
 * quarter of it. */
/* A/B knobs (both unset = byte-identical to the shipped default).
 * Y264_PSY_RD=<float> overrides the flat psy-RD weight (0 = off).
 * Y264_PSY_RD_RAMP=<qp0>,<qp1>,<hi_x100> scales the weight by the CURRENT MB's
 * QP: x1.00 at qp<=qp0 rising linearly to hi_x100/100 at qp>=qp1 --
 * regime-shaped arms want the ramp form (as the trellis intra-lambda does),
 * against the AC-retention deficit that lives at deep quant. Distinct from an
 * unconditional ME-lambda scaling: the shallow band keeps the tuned 1.0. */
static float psy_rd_env(void)
{
    static float v = -2.0f;
    if (v == -2.0f) { const char *e = getenv("Y264_PSY_RD");
                      v = e ? (float)atof(e) : -1.0f; }
    return v;
}
static const int *psy_rd_ramp_env(void)
{
    static int r[3], have = -1;
    if (have == -1) {
        have = 0;
        const char *e = getenv("Y264_PSY_RD_RAMP");
        if (e && sscanf(e, "%d,%d,%d", &r[0], &r[1], &r[2]) == 3 && r[1] > r[0])
            have = 1;
    }
    return have ? r : NULL;
}
static long dist_mb(y264_frame_t *f, int mbx, int mby)
{
    long d = ssd_mb(f, mbx, mby);
    float psyw = f->psy_rd;
    { float o = psy_rd_env(); if (o >= 0.0f) psyw = o; }
    { const int *r = psy_rd_ramp_env();
      if (r && f->cur_qp > r[0]) {
          int q = f->cur_qp > r[1] ? r[1] : f->cur_qp;
          psyw = psyw * (float)(100 * (r[1] - q) + r[2] * (q - r[0]))
                      / (100.0f * (float)(r[1] - r[0]));
      } }
    if (psyw > 0.0f) {
        const pixel *s = f->src[0] + (mby * 16) * f->src_stride[0] + mbx * 16;
        const pixel *r = f->rec[0] + (mby * 16) * f->rec_stride[0] + mbx * 16;
        /* x264's psy metric averages the SATD-support (4x4) and SA8D-support
 * (8x8) texture-energy differences, halved to avoid transform-size
 * bias. The 8x8 Hadamard has ~4x the gain of the 4x4, so >>2 puts it on
 * the SATD scale before averaging. (Applying x264's ME lambda scaling to a
 * 4x4-only metric loses on VMAF; the richer metric is what pays.) */
        /* The src-side terms are invariant across a MB's RD candidates -- memo
 * them once per (mbx,mby) instead of recomputing every dist_mb call. */
        long ts4, ts8;
        if (f->te_mbx == mbx && f->te_mby == mby) {
            ts4 = f->te_src4; ts8 = f->te_src8;
        } else {
            long ts[2];
            texture_energy(s, f->src_stride[0], ts);
            ts4 = ts[0]; ts8 = ts[1];
            f->te_src4 = ts4; f->te_src8 = ts8; f->te_mbx = mbx; f->te_mby = mby;
        }
        long tr[2];
        texture_energy(r, f->rec_stride[0], tr);
        long de4 = labs(ts4 - tr[0]);
        long de8 = labs(ts8 - tr[1]) >> 2;
        long de = (de4 + de8) / 2;
        d += (long)(psyw * (float)de);
    }
    return d;
}

/* Encode one Intra16x16 luma macroblock: mode decision, forward transform +
 * quant, reconstruction into rec, CAVLC coding of DC then AC. Returns the chosen
 * prediction mode; sets *cbp_luma to 0 or 15 and fills ac_nnz[16]. */
struct luma_result {
    int mode;
    int cbp_luma;
    dctcoef dc_scan[16];        /* quantized DC levels in scan order */
    dctcoef ac_scan[16][15];    /* quantized AC levels per block, scan order */
    int ac_nnz[16];             /* TotalCoeff of each AC block */
};

/* The three broadcast I16x16 modes (VERT, HORIZ, DC) in one pass over the
 * source, via the fused kernel: their predictions are a repeated row, a
 * repeated sample and a constant, so none of them has to be materialised.
 * PLANE is not a broadcast and stays on the builder. Edge samples are gathered
 * exactly as y264_intra16x16 gathers them (zero-filled when unavailable), so
 * the DC derivation and every cost match the per-mode loop bit for bit.
 * Returns the best (cost, mode) over the three; the caller adds PLANE. */
static void i16_costs_x3(const pixel *src, int ss, const pixel *rec, int rs,
                         int have_top, int have_left, int *best_cost, int *best_mode)
{
    pixel top[16], left[16];
    int st = 0, sl = 0;
    for (int i = 0; i < 16; i++) {
        top[i]  = have_top  ? rec[-rs + i] : 0;
        left[i] = have_left ? rec[i * rs - 1] : 0;
        st += top[i];
        sl += left[i];
    }
    int dc;
    if (have_top && have_left) dc = (st + sl + 16) >> 5;
    else if (have_top)         dc = (st + 8) >> 4;
    else if (have_left)        dc = (sl + 8) >> 4;
    else                       dc = 1 << (Y264_BIT_DEPTH - 1);

    int c3[3];
    y264_dsp.intra_satd_x3_16(src, ss, top, left, dc, c3);
    /* DC first, then VERT, then HORIZ, and strictly less-than: the same order
 * and the same tie-break as the loop this replaces. */
    *best_cost = c3[2];
    *best_mode = Y264_I16_DC;
    if (have_top && c3[0] < *best_cost)  { *best_cost = c3[0]; *best_mode = Y264_I16_VERT; }
    if (have_left && c3[1] < *best_cost) { *best_cost = c3[1]; *best_mode = Y264_I16_HORIZ; }
}

static int encode_luma16(y264_frame_t *f, int mbx, int mby,
                         int have_top, int have_left, struct luma_result *lr)
{
    const int trel = rd_trellis(f);   /* hoisted: one TLS walk per block loop */
    int stride_s = f->src_stride[0], stride_r = f->rec_stride[0];
    const pixel *src = f->src[0] + (mby * 16) * stride_s + mbx * 16;
    pixel *rec = f->rec[0] + (mby * 16) * stride_r + mbx * 16;

    /* mode decision over the available I16x16 modes: the three broadcast modes
 * fused, then PLANE, then the winner's prediction built once. */
    int best_cost, best_mode;
    i16_costs_x3(src, stride_s, rec, stride_r, have_top, have_left,
                 &best_cost, &best_mode);
    pixel best_pred[256];
    if (have_top && have_left) {
        y264_intra16x16(best_pred, rec, stride_r, Y264_I16_PLANE, have_top, have_left);
        int cost = satd_block(src, stride_s, best_pred, 16, 16, 16);
        if (cost < best_cost) { best_cost = cost; best_mode = Y264_I16_PLANE; }
    }
    if (best_mode != Y264_I16_PLANE)
        y264_intra16x16(best_pred, rec, stride_r, best_mode, have_top, have_left);
    lr->mode = best_mode;

    /* forward transform: per 4x4 block collect DC, quant AC. */
    dctcoef dc_raster[16];
    dctcoef ac_lev[16][16];     /* raster order per block, position 0 unused */
    int8_t loc16[4][4];         /* decided AC-nnz grid for exact cbf context */
    int lbx0 = mbx * 4, lby0 = mby * 4;
    dctcoef coefs[16][16];
    y264_sub_dct4_blocks(coefs, 4, 4, src, stride_s, best_pred, 16);
    for (int by = 0; by < 4; by++) {
        for (int bx = 0; bx < 4; bx++) {
            int b = by * 4 + bx;
            int nza = bx > 0 ? (loc16[by][bx-1] > 0)
                             : cbf_nb(f, 0, lbx0 + bx - 1, lby0 + by, 1);
            int nzb = by > 0 ? (loc16[by-1][bx] > 0)
                             : cbf_nb(f, 0, lbx0 + bx, lby0 + by - 1, 1);
            dctcoef *coef = coefs[b];
            dctcoef lev[16];
            const pixel *bsrc = src + (by * 4) * stride_s + bx * 4;
            const pixel *bpred = best_pred + (by * 4) * 16 + bx * 4;
            dc_raster[b] = coef[0];
            rdoq_4x4_ctx(f->cabac, bsrc, stride_s, bpred, 16, coef, lev, f->cur_qp_scaled, lambda_trellis(f->cur_qp, 1), 1, 1, 1, cqm_w4(f, 1), nza, nzb, 0, f->subme, psy_trellis_qp(f, f->cur_qp), trel);
            int nz = 0;
            for (int k = 0; k < 16; k++) { ac_lev[b][k] = lev[k]; if (lev[k]) nz++; }
            loc16[by][bx] = (int8_t)(nz > 0);
        }
    }

    /* luma DC: Hadamard then quant. */
    dctcoef hdc[16], dclev[16];
    y264_hadamard4x4(dc_raster, hdc);
    y264_quant_dc_luma(hdc, dclev, f->cur_qp_scaled, 1, cqm_dc4(f, 1));

    /* cbp_luma: 15 if any AC coefficient survives, else 0. */
    int any_ac = 0;
    for (int b = 0; b < 16 && !any_ac; b++)
        for (int k = 1; k < 16; k++)
            if (ac_lev[b][k]) { any_ac = 1; break; }
    lr->cbp_luma = any_ac ? 15 : 0;

    /* reconstruct DC coefficients that seed each block. */
    dctcoef rdc_had[16], rdc[16];
    y264_hadamard4x4(dclev, rdc_had);
    y264_dequant_dc_luma(rdc_had, rdc, f->cur_qp_scaled, cqm_dc4(f, 1));

    /* per-block reconstruction and AC scan extraction. AC-all-zero blocks take
 * the DC-only inverse: with a lone coef[0] both idct passes just propagate
 * it, so every output is exactly (dc + 32) >> 6 -- a flat add, no dequant,
 * no idct (x264's add*_idct_dc shape). Byte-identical. */
    for (int by = 0; by < 4; by++) {
        for (int bx = 0; bx < 4; bx++) {
            int b = by * 4 + bx;
            if (!loc16[by][bx]) {
                int flat = (rdc[b] + 32) >> 6;
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        int v = best_pred[(by*4+y)*16 + bx*4+x] + flat;
                        rec[(by*4+y)*stride_r + bx*4+x] = (pixel)clip8(v);
                    }
                continue;
            }
            dctcoef coef[16];
            y264_dequant_4x4(ac_lev[b], coef, f->cur_qp_scaled, cqm_w4(f, 1));
            coef[0] = rdc[b];                       /* seed reconstructed DC */
            y264_add4x4_idct(rec + (by * 4) * stride_r + bx * 4, stride_r,
                             best_pred + (by * 4) * 16 + bx * 4, 16, coef);
        }
    }

    /* DC levels in scan order. */
    for (int k = 0; k < 16; k++)
        lr->dc_scan[k] = dclev[ZIGZAG[k]];

    /* AC levels in scan order (positions 1..15) per block, in blkIdx order. */
    for (int i = 0; i < 16; i++) {
        int b = BLK_Y[i] * 4 + BLK_X[i];
        int nz = 0;
        for (int k = 0; k < 15; k++) {
            dctcoef v = lr->cbp_luma ? ac_lev[b][ZIGZAG[k + 1]] : 0;
            lr->ac_scan[i][k] = v;
            if (v) nz++;
        }
        lr->ac_nnz[i] = lr->cbp_luma ? nz : 0;
    }
    return best_cost;
}

/* Intra4x4 luma: per-block closed-loop mode decision and reconstruction.
 * Returns the summed SATD cost; fills modes, scan-order levels, nnz, cbp_luma. */
struct i4_result {
    int8_t  mode[16];
    dctcoef lev[16][16];        /* scan-order quantized levels per blkIdx */
    int     nnz[16];
    int     cbp_luma;           /* 4 bits, one per 8x8 */
};

static int topright_avail(y264_frame_t *f, int mbx, int mby, int bxm, int bym)
{
    int cur_ay = mby * 4 + bym;
    int trx = mbx * 4 + bxm + 1;
    int try_ = cur_ay - 1;
    if (try_ < 0 || trx >= f->wmb * 4)
        return 0;
    int nmbx = trx / 4, nmby = try_ / 4;
    if (nmby < mby) return 1;               /* neighbour is in an earlier MB row */
    if (nmbx < mbx) return 1;               /* earlier MB in the same row */
    if (nmbx > mbx) return 0;               /* MB to the right: not yet decoded */
    return ZIDX[(try_ % 4) * 4 + (trx % 4)] < ZIDX[bym * 4 + bxm];
}

static int i4_mode_allowed(int mode, int ht, int hl)
{
    switch (mode) {
    case Y264_I4_VERT: case Y264_I4_DDL: case Y264_I4_VL: return ht;
    case Y264_I4_HORIZ: case Y264_I4_HU:                  return hl;
    case Y264_I4_DC:                                      return 1;
    default:                                              return ht && hl; /* DDR/VR/HD */
    }
}

static int encode_luma4x4(y264_frame_t *f, int mbx, int mby, struct i4_result *r)
{
    const int trel = rd_trellis(f);   /* hoisted: one TLS walk per block loop */
    int ss = f->src_stride[0], rs = f->rec_stride[0];
    int ms = f->i4mode_stride;
    int total_cost = 0;
    int8_t loc[4][4];           /* decided nnz grid for exact cbf context */

    for (int blk = 0; blk < 16; blk++) {
        int bxm = BLK_X[blk], bym = BLK_Y[blk];
        int ax = mbx * 4 + bxm, ay = mby * 4 + bym;
        const pixel *src = f->src[0] + (ay * 4) * ss + ax * 4;
        pixel *rec = f->rec[0] + (ay * 4) * rs + ax * 4;

        int ht = ay > 0, hl = ax > 0, htl = ht && hl;
        int htr = topright_avail(f, mbx, mby, bxm, bym);

        int pred_mode;
        if (!ht || !hl)
            pred_mode = Y264_I4_DC;
        else {
            int a = f->i4mode[ay * ms + (ax - 1)];
            int b = f->i4mode[(ay - 1) * ms + ax];
            pred_mode = a < b ? a : b;
        }

        /* One fused pass costs all nine modes (the disallowed ones too, off the
 * same zero-filled neighbours the builder would use -- skipped here,
 * exactly as the per-mode loop skipped their builds), then the winner's
 * prediction is built once. */
        pixel best_pred[16];
        int mcost[9];
        int best_cost = -1, best_mode = Y264_I4_DC;
        y264_dsp.intra4x4_x9(src, ss, rec, rs, ht, hl, htl, htr, mcost);
        for (int mode = 0; mode < 9; mode++) {
            if (!i4_mode_allowed(mode, ht, hl))
                continue;
            int cost = mcost[mode] + (mode == pred_mode ? 0 : 4);
            if (best_cost < 0 || cost < best_cost) {
                best_cost = cost;
                best_mode = mode;
            }
        }
        y264_intra4x4(best_pred, rec, rs, best_mode, ht, hl, htl, htr);
        r->mode[blk] = (int8_t)best_mode;
        f->i4mode[ay * ms + ax] = (int8_t)best_mode;
        total_cost += best_cost;

        dctcoef coef[16], lev[16], rcoef[16];
        y264_sub4x4_dct(coef, src, ss, best_pred, 4);
        int nza = bxm > 0 ? (loc[bym][bxm-1] > 0) : cbf_nb(f, 0, ax - 1, ay, 1);
        int nzb = bym > 0 ? (loc[bym-1][bxm] > 0) : cbf_nb(f, 0, ax, ay - 1, 1);
        rdoq_4x4_ctx(f->cabac, src, ss, best_pred, 4, coef, lev, f->cur_qp_scaled, lambda_trellis(f->cur_qp, 1), 1, 0, 2, cqm_w4(f, 1), nza, nzb, 0, f->subme, psy_trellis_qp(f, f->cur_qp), trel);

        int nz = 0;
        for (int k = 0; k < 16; k++) {
            dctcoef v = lev[ZIGZAG[k]];
            r->lev[blk][k] = v;
            if (v) nz++;
        }
        r->nnz[blk] = nz;
        loc[bym][bxm] = (int8_t)(nz > 0);
        /* All-zero block: zero residual, rec = best_pred (already a pixel). */
        if (nz) {
            y264_dequant_4x4(lev, rcoef, f->cur_qp_scaled, cqm_w4(f, 1));
            y264_add4x4_idct(rec, rs, best_pred, 4, rcoef);
        } else {
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    rec[y * rs + x] = (pixel)best_pred[y * 4 + x];
        }
    }

    r->cbp_luma = 0;
    for (int i8 = 0; i8 < 4; i8++)
        for (int i4 = 0; i4 < 4; i4++)
            if (r->nnz[i8 * 4 + i4])
                r->cbp_luma |= (1 << i8);
    return total_cost;
}

/* Intra8x8 luma (High profile). Per-8x8 closed-loop mode decision, 8x8 transform,
 * reconstruction. Same directional modes as I_4x4; mode signalling is per-8x8. */
struct i8_result {
    int8_t  mode[4];            /* prediction mode of each 8x8 (raster TL,TR,BL,BR) */
    dctcoef lev[4][64];         /* raster-order quantized levels per 8x8 block */
    int     nnz8[4];            /* nonzero count per 8x8 */
    int     cbp_luma;           /* 4 bits, one per 8x8 */
};

/* Availability of the 8 top-right reference samples for 8x8 block `blk` within an
 * MB at (mbx,mby). See 8.3.2: TL uses the MB above; TR the above-right MB; BL the
 * already-decoded TR block of this MB; BR has no decoded top-right neighbour. */
static int topright8_avail(int mbx, int mby, int blk, int wmb)
{
    switch (blk) {
    case 0: return mby > 0;
    case 1: return mby > 0 && mbx < wmb - 1;
    case 2: return 1;
    default: return 0;
    }
}

static int encode_luma8x8(y264_frame_t *f, int mbx, int mby, struct i8_result *r)
{
    const int trel = rd_trellis(f);   /* hoisted: one TLS walk per block loop */
    int ss = f->src_stride[0], rs = f->rec_stride[0];
    int ms = f->i4mode_stride;
    int total_cost = 0;

    for (int blk = 0; blk < 4; blk++) {
        int b8x = B8_X[blk], b8y = B8_Y[blk];
        int ax4 = mbx * 4 + b8x, ay4 = mby * 4 + b8y;      /* absolute 4x4 coords */
        int px = mbx * 16 + b8x * 4, py = mby * 16 + b8y * 4;
        const pixel *src = f->src[0] + py * ss + px;
        pixel *rec = f->rec[0] + py * rs + px;

        int ht = ay4 > 0, hl = ax4 > 0, htl = ht && hl;
        int htr = topright8_avail(mbx, mby, blk, f->wmb);

        int pred_mode;
        if (!ht || !hl)
            pred_mode = Y264_I4_DC;
        else {
            int a = f->i4mode[ay4 * ms + (ax4 - 1)];
            int b = f->i4mode[(ay4 - 1) * ms + ax4];
            pred_mode = a < b ? a : b;
        }

        pixel pred[64], best_pred[64];
        int best_cost = -1, best_mode = Y264_I4_DC;
        /* 8.3.2.2.1's low-pass depends on the neighbourhood, not the mode, and
 * this loop was paying it nine times per block. Derive it once and let
 * each mode read it. Bit-exact: y264_intra8x8_c is now literally these
 * two calls. */
        y264_i8_edge_t edge;
        y264_intra8x8_edge_c(&edge, rec, rs, ht, hl, htl, htr);
        for (int mode = 0; mode < 9; mode++) {
            if (!i4_mode_allowed(mode, ht, hl))
                continue;
            y264_intra8x8_from_edge(pred, &edge, mode, ht, hl);
            int cost = satd_block(src, ss, pred, 8, 8, 8) + (mode == pred_mode ? 0 : 4);
            if (best_cost < 0 || cost < best_cost) {
                best_cost = cost;
                best_mode = mode;
                memcpy(best_pred, pred, 64 * sizeof(pixel));
            }
        }
        r->mode[blk] = (int8_t)best_mode;
        for (int yy = 0; yy < 2; yy++)
            for (int xx = 0; xx < 2; xx++)
                f->i4mode[(ay4 + yy) * ms + (ax4 + xx)] = (int8_t)best_mode;
        total_cost += best_cost;

        dctcoef coef[64], lev[64], rcoef[64];
        y264_sub8x8_dct8(coef, src, ss, best_pred, 8);
        uint64_t msk;
        int big;
        rdoq_8x8(f->cabac, src, ss, best_pred, 8, coef, lev, f->cur_qp_scaled, lambda_trellis(f->cur_qp, 1), 1, cqm_w8(f, 1), 0, psy_trellis_qp(f, f->cur_qp), f->subme, &msk, &big, trel);

        int nz = __builtin_popcountll(msk);     /* raster nonzeros == scan nonzeros */
        memcpy(r->lev[blk], lev, 64 * sizeof(dctcoef));
        r->nnz8[blk] = nz;
        /* All-zero block: zero residual, rec = best_pred (already a pixel). */
        if (nz) {
            y264_dequant_8x8(lev, rcoef, f->cur_qp_scaled, cqm_w8(f, 1));
            y264_add8x8_idct8(rec, rs, best_pred, 8, rcoef);
        } else {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    rec[y * rs + x] = (pixel)best_pred[y * 8 + x];
        }
    }

    r->cbp_luma = 0;
    for (int blk = 0; blk < 4; blk++)
        if (r->nnz8[blk]) r->cbp_luma |= (1 << blk);
    return total_cost;
}

/* W0: the nnz grid cell of a coded block equals its CAVLC total_coeff, which is
 * just the count of nonzero coefficients (y264_cavlc_residual returns the same).
 * The author pass writes grids from this; the emit pass writes bits only. */
static inline int nz_count(const dctcoef *c, int n)
{
    int t = 0;
    for (int k = 0; k < n; k++) if (c[k]) t++;
    return t;
}

/* Author the nnz grid for one 8x8 luma block (see write_luma8x8_residual_cavlc:
 * same interleaved-subblock layout, but counts nonzeros instead of coding). */
static void author_luma8x8_nnz(y264_frame_t *f, int mbx, int mby, int blk,
                               const dctcoef lev[64])
{
    int lstride = f->nnz_stride[0];
    int8_t *lnnz = f->nnz[0];
    int bx0 = mbx * 4, by0 = mby * 4;
    dctcoef scan8[64];
    for (int k = 0; k < 64; k++) scan8[k] = lev[ZIGZAG8[k]];
    for (int j = 0; j < 4; j++) {
        dctcoef sub[16];
        for (int i = 0; i < 16; i++) sub[i] = scan8[4 * i + j];
        int lb = blk * 4 + j;
        int ax = bx0 + BLK_X[lb], ay = by0 + BLK_Y[lb];
        lnnz[ay * lstride + ax] = (int8_t)nz_count(sub, 16);
    }
}

/* CAVLC residual for one 8x8 luma block: the 64 zig-zag coefficients split into
 * four interleaved 4x4 sub-blocks (level8x8[4*i + j] -> sub j, position i), each
 * coded as a 4x4 residual with nC from its 4x4 grid cell (8.5.6 / 9.2.1). */
static void emit_luma8x8_residual_cavlc(y264_bs_t *bs, y264_frame_t *f,
                                        int mbx, int mby, int blk,
                                        const dctcoef lev[64])
{
    int lstride = f->nnz_stride[0];
    int8_t *lnnz = f->nnz[0];
    int bx0 = mbx * 4, by0 = mby * 4;

    dctcoef scan8[64];
    for (int k = 0; k < 64; k++) scan8[k] = lev[ZIGZAG8[k]];

    for (int j = 0; j < 4; j++) {
        dctcoef sub[16];
        for (int i = 0; i < 16; i++) sub[i] = scan8[4 * i + j];
        int lb = blk * 4 + j;
        int ax = bx0 + BLK_X[lb], ay = by0 + BLK_Y[lb];
        int nc = derive_nc(lnnz, lstride, ax, ay);   /* reads author-written nnz */
        y264_cavlc_residual(bs, sub, 16, nc);
    }
}

/* Wrapper for callers not yet split into author/emit passes (intra). */
static void write_luma8x8_residual_cavlc(y264_bs_t *bs, y264_frame_t *f,
                                         int mbx, int mby, int blk,
                                         const dctcoef lev[64])
{
    author_luma8x8_nnz(f, mbx, mby, blk, lev);
    emit_luma8x8_residual_cavlc(bs, f, mbx, mby, blk, lev);
}

/* Max chroma 4x4 blocks per component: 8 for 4:2:2 (2x4). (4:4:4 uses the
 * luma coding path, not chroma_result.) */
#define Y264_CHROMA_MAXBLK 8

struct chroma_result {
    int mode;
    int cbp;                    /* 0 none, 1 DC only, 2 DC+AC */
    int ndc;                    /* DC coeff count: 4 (4:2:0) or 8 (4:2:2) */
    dctcoef dc_scan[2][Y264_CHROMA_MAXBLK];       /* DC levels, transmit scan order */
    dctcoef ac_scan[2][Y264_CHROMA_MAXBLK][15];   /* [comp][block][scan] */
    int ac_nnz[2][Y264_CHROMA_MAXBLK];
};

/* Level-index -> raster (row*2+col) for the 4:2:2 chroma DC scan (eq 8-305). */
static const int SCAN422DC[8] = { 0, 2, 1, 4, 6, 3, 5, 7 };

/* Forward chroma-DC transform + quant, format-aware. Reads raster DCs in
 * dc_raster[nblk], writes quantized levels (raster) to dclev, and the transmit-
 * order levels to dc_out. Returns whether any DC level is nonzero. */
static int chroma_dc_fwd(const y264_frame_t *f, int intra, const dctcoef *dc_raster,
                         dctcoef *dclev, dctcoef *dc_out)
{
    int any = 0;
    if (f->cf_idc == 2) {
        dctcoef hdc[8];
        y264_chroma422_dc(dc_raster, hdc);
        y264_quant_dc_chroma422(hdc, dclev, f->cur_chroma_qp_scaled, intra, cqm_dc4(f, intra));
        for (int s = 0; s < 8; s++) { dc_out[s] = dclev[SCAN422DC[s]]; if (dc_out[s]) any = 1; }
    } else {
        dctcoef hdc[4];
        y264_hadamard2x2(dc_raster, hdc);
        y264_quant_dc_chroma(hdc, dclev, f->cur_chroma_qp_scaled, intra, cqm_dc4(f, intra));
        for (int k = 0; k < 4; k++) { dc_out[k] = dclev[k]; if (dclev[k]) any = 1; }
    }
    return any;
}

/* Inverse chroma-DC transform + scale, format-aware. dclev is raster; rdc is the
 * reconstructed per-block DC (raster [row*cbw+col] == [by*cbw+bx]). */
static void chroma_dc_inv(const y264_frame_t *f, int intra, const dctcoef *dclev, dctcoef *rdc)
{
    if (f->cf_idc == 2) {
        dctcoef rf[8];
        y264_chroma422_dc(dclev, rf);
        y264_dequant_dc_chroma422(rf, rdc, f->cur_chroma_qp_scaled, cqm_dc4(f, intra));
    } else {
        dctcoef rf[4];
        y264_hadamard2x2(dclev, rf);
        y264_dequant_dc_chroma(rf, rdc, f->cur_chroma_qp_scaled, cqm_dc4(f, intra));
    }
}

static void encode_chroma(y264_frame_t *f, int mbx, int mby,
                          int have_top, int have_left, struct chroma_result *cr)
{
    const int trel = rd_trellis(f);   /* hoisted: one TLS walk per block loop */
    /* chroma mode decision (shared by Cb and Cr). */
    int modes[4], nm = 0;
    modes[nm++] = Y264_IC_DC;
    if (have_left) modes[nm++] = Y264_IC_HORIZ;
    if (have_top) modes[nm++] = Y264_IC_VERT;
    if (have_top && have_left) modes[nm++] = Y264_IC_PLANE;

    int cw = 16 / f->sub_w, ch = 16 / f->sub_h;    /* MbWidthC x MbHeightC */
    int cbw = f->cbw, cbh = f->cbh, nblk = cbw * cbh;
    cr->ndc = nblk;

    pixel pred[2][256], best_pred[2][256];
    int best_cost = -1, best_mode = Y264_IC_DC;
    for (int i = 0; i < nm; i++) {
        int cost = 0;
        for (int c = 0; c < 2; c++) {
            int ss = f->src_stride[1 + c], rs = f->rec_stride[1 + c];
            const pixel *src = f->src[1 + c] + (mby * ch) * ss + mbx * cw;
            pixel *rec = f->rec[1 + c] + (mby * ch) * rs + mbx * cw;
            y264_intra_chroma(pred[c], rec, rs, modes[i], have_top, have_left, cw, ch);
            cost += satd_block(src, ss, pred[c], cw, cw, ch);
        }
        if (best_cost < 0 || cost < best_cost) {
            best_cost = cost;
            best_mode = modes[i];
            for (int c = 0; c < 2; c++)
                for (int k = 0; k < cw * ch; k++) best_pred[c][k] = pred[c][k];
        }
    }
    cr->mode = best_mode;

    int any_ac = 0, any_dc = 0;
    for (int c = 0; c < 2; c++) {
        int ss = f->src_stride[1 + c], rs = f->rec_stride[1 + c];
        const pixel *src = f->src[1 + c] + (mby * ch) * ss + mbx * cw;
        pixel *rec = f->rec[1 + c] + (mby * ch) * rs + mbx * cw;

        dctcoef dc_raster[Y264_CHROMA_MAXBLK], ac_lev[Y264_CHROMA_MAXBLK][16];
        int8_t loc[4][2];
        int cbx0 = mbx * cbw, cby0 = mby * cbh;
        dctcoef coefs[Y264_CHROMA_MAXBLK][16];
        y264_sub_dct4_blocks(coefs, cbw, cbh, src, ss, best_pred[c], cw);
        for (int blk = 0; blk < nblk; blk++) {
            int bx = blk % cbw, by = blk / cbw;
            int nza = bx > 0 ? (loc[by][bx-1] > 0)
                             : cbf_nb(f, 1 + c, cbx0 + bx - 1, cby0 + by, 1);
            int nzb = by > 0 ? (loc[by-1][bx] > 0)
                             : cbf_nb(f, 1 + c, cbx0 + bx, cby0 + by - 1, 1);
            dctcoef *coef = coefs[by * cbw + bx];
            dctcoef lev[16];
            const pixel *bsrc = src + (by * 4) * ss + bx * 4;
            const pixel *bpred = best_pred[c] + (by * 4) * cw + bx * 4;
            dc_raster[by * cbw + bx] = coef[0];
            rdoq_4x4_ctx(f->cabac, bsrc, ss, bpred, cw, coef, lev, f->cur_chroma_qp_scaled, lambda_trellis(f->cur_chroma_qp, 1), 1, 1, 4, cqm_w4(f, 1), nza, nzb, 0, f->subme, psy_trellis_qp(f, f->cur_chroma_qp), trel);
            int nz = 0;
            for (int k = 0; k < 16; k++) { ac_lev[blk][k] = lev[k]; if (lev[k]) nz++; }
            loc[by][bx] = (int8_t)(nz > 0);
        }

        dctcoef dclev[Y264_CHROMA_MAXBLK];
        if (chroma_dc_fwd(f, 1, dc_raster, dclev, cr->dc_scan[c])) any_dc = 1;
        for (int blk = 0; blk < nblk; blk++)
            for (int k = 1; k < 16; k++)
                if (ac_lev[blk][k]) { any_ac = 1; break; }

        /* reconstruct chroma DC then blocks (DC-only fast path: see the i16
 * recon loop -- flat (dc+32)>>6, byte-identical). */
        dctcoef rdc[Y264_CHROMA_MAXBLK];
        chroma_dc_inv(f, 1, dclev, rdc);
        for (int blk = 0; blk < nblk; blk++) {
            int bx = blk % cbw, by = blk / cbw;
            if (!loc[by][bx]) {
                int flat = (rdc[by * cbw + bx] + 32) >> 6;
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        int v = best_pred[c][(by*4+y)*cw + bx*4+x] + flat;
                        rec[(by*4+y)*rs + bx*4+x] = (pixel)clip8(v);
                    }
                continue;
            }
            dctcoef coef[16];
            y264_dequant_4x4(ac_lev[blk], coef, f->cur_chroma_qp_scaled, cqm_w4(f, 1));
            coef[0] = rdc[by * cbw + bx];
            y264_add4x4_idct(rec + (by * 4) * rs + bx * 4, rs,
                             best_pred[c] + (by * 4) * cw + bx * 4, cw, coef);
        }

        /* stash AC scan + nnz; finalized once cbp is known below. */
        for (int blk = 0; blk < nblk; blk++) {
            int nz = 0;
            for (int k = 0; k < 15; k++) {
                dctcoef v = ac_lev[blk][ZIGZAG[k + 1]];
                cr->ac_scan[c][blk][k] = v;
                if (v) nz++;
            }
            cr->ac_nnz[c][blk] = nz;
        }
    }

    cr->cbp = any_ac ? 2 : (any_dc ? 1 : 0);
    if (cr->cbp < 2)                                /* AC not transmitted */
        for (int c = 0; c < 2; c++)
            for (int blk = 0; blk < nblk; blk++)
                cr->ac_nnz[c][blk] = 0;
}

/* Shared chroma DC+AC residual syntax, used by intra and inter macroblocks. */
/* Author chroma AC nnz grid (DC has no grid cell). Mirrors write_chroma_residual's
 * AC layout; counts nonzeros instead of coding. */
static void author_chroma_residual_nnz(y264_frame_t *f, int mbx, int mby,
                                       const struct chroma_result *cr)
{
    int cbw = f->cbw, cbh = f->cbh, nblk = cbw * cbh;
    for (int c = 0; c < 2; c++) {
        int cstride = f->nnz_stride[1 + c];
        int8_t *cnnz = f->nnz[1 + c];
        int cbx0 = mbx * cbw, cby0 = mby * cbh;
        for (int blk = 0; blk < nblk; blk++) {
            int bx = cbx0 + blk % cbw, by = cby0 + blk / cbw;
            cnnz[by * cstride + bx] =
                (cr->cbp == 2) ? (int8_t)nz_count(cr->ac_scan[c][blk], 15) : 0;
        }
    }
}

static void emit_chroma_residual(y264_bs_t *bs, y264_frame_t *f,
                                 int mbx, int mby, const struct chroma_result *cr)
{
    int cbw = f->cbw, cbh = f->cbh, nblk = cbw * cbh;
    if (cr->cbp) {
        int ndc = cr->ndc, ncdc = f->cf_idc == 2 ? -2 : -1;
        for (int c = 0; c < 2; c++)
            y264_cavlc_residual(bs, cr->dc_scan[c], ndc, ncdc);
    }
    if (cr->cbp != 2) return;
    for (int c = 0; c < 2; c++) {
        int cstride = f->nnz_stride[1 + c];
        int8_t *cnnz = f->nnz[1 + c];
        int cbx0 = mbx * cbw, cby0 = mby * cbh;
        for (int blk = 0; blk < nblk; blk++) {
            int bx = cbx0 + blk % cbw, by = cby0 + blk / cbw;
            int nc = derive_nc(cnnz, cstride, bx, by);   /* reads author-written nnz */
            dctcoef buf[16];
            for (int k = 0; k < 15; k++) buf[k] = cr->ac_scan[c][blk][k];
            y264_cavlc_residual(bs, buf, 15, nc);
        }
    }
}

/* ---- 4:4:4: chroma coded like luma (ChromaArrayType==3) --------------------
 * Cb/Cr reuse the co-located luma block's intra mode (8.3.4.5): no chroma
 * pred-mode syntax, no chroma-DC transform. These coders mirror encode_luma16 /
 * encode_luma4x4 but on the full-res chroma plane 1+comp with the mode supplied
 * (no RD search), reconstruct into f->rec[1+comp], and fill a luma_result /
 * i4_result for the luma-style entropy path. */
static void c444_i16(y264_frame_t *f, int mbx, int mby, int comp, int mode,
                     int have_top, int have_left, struct luma_result *lr)
{
    const int trel = rd_trellis(f);   /* hoisted: one TLS walk per block loop */
    int p = 1 + comp;
    int ss = f->src_stride[p], rs = f->rec_stride[p];
    const pixel *src = f->src[p] + (mby * 16) * ss + mbx * 16;
    pixel *rec = f->rec[p] + (mby * 16) * rs + mbx * 16;

    pixel pred[256];
    y264_intra16x16(pred, rec, rs, mode, have_top, have_left);
    lr->mode = mode;

    dctcoef dc_raster[16], ac_lev[16][16];
    int8_t loc16[4][4];
    int lbx0 = mbx * 4, lby0 = mby * 4;
    dctcoef coefs[16][16];
    y264_sub_dct4_blocks(coefs, 4, 4, src, ss, pred, 16);
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++) {
            int b = by * 4 + bx;
            int nza = bx > 0 ? (loc16[by][bx-1] > 0) : cbf_nb(f, p, lbx0 + bx - 1, lby0 + by, 1);
            int nzb = by > 0 ? (loc16[by-1][bx] > 0) : cbf_nb(f, p, lbx0 + bx, lby0 + by - 1, 1);
            dctcoef *coef = coefs[b];
            dctcoef lev[16];
            const pixel *bsrc = src + (by * 4) * ss + bx * 4;
            const pixel *bpred = pred + (by * 4) * 16 + bx * 4;
            dc_raster[b] = coef[0];
            rdoq_4x4_ctx(f->cabac, bsrc, ss, bpred, 16, coef, lev, f->cur_chroma_qp_scaled, lambda_trellis(f->cur_chroma_qp, 1), 1, 1, 1, cqm_w4(f, 1), nza, nzb, 0, f->subme, psy_trellis_qp(f, f->cur_chroma_qp), trel);
            int nz = 0;
            for (int k = 0; k < 16; k++) { ac_lev[b][k] = lev[k]; if (lev[k]) nz++; }
            loc16[by][bx] = (int8_t)(nz > 0);
        }

    dctcoef hdc[16], dclev[16];
    y264_hadamard4x4(dc_raster, hdc);
    y264_quant_dc_luma(hdc, dclev, f->cur_chroma_qp_scaled, 1, cqm_dc4(f, 1));
    int any_ac = 0;
    for (int b = 0; b < 16 && !any_ac; b++)
        for (int k = 1; k < 16; k++) if (ac_lev[b][k]) { any_ac = 1; break; }
    lr->cbp_luma = any_ac ? 15 : 0;

    dctcoef rdc_had[16], rdc[16];
    y264_hadamard4x4(dclev, rdc_had);
    y264_dequant_dc_luma(rdc_had, rdc, f->cur_chroma_qp_scaled, cqm_dc4(f, 1));
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++) {
            int b = by * 4 + bx;
            dctcoef coef[16];
            y264_dequant_4x4(ac_lev[b], coef, f->cur_chroma_qp_scaled, cqm_w4(f, 1));
            coef[0] = rdc[b];
            y264_add4x4_idct(rec + (by * 4) * rs + bx * 4, rs,
                             pred + (by * 4) * 16 + bx * 4, 16, coef);
        }

    for (int k = 0; k < 16; k++) lr->dc_scan[k] = dclev[ZIGZAG[k]];
    for (int i = 0; i < 16; i++) {
        int b = BLK_Y[i] * 4 + BLK_X[i];
        int nz = 0;
        for (int k = 0; k < 15; k++) {
            dctcoef v = lr->cbp_luma ? ac_lev[b][ZIGZAG[k + 1]] : 0;
            lr->ac_scan[i][k] = v; if (v) nz++;
        }
        lr->ac_nnz[i] = lr->cbp_luma ? nz : 0;
    }
}

/* 4:4:4 I_4x4 chroma: per-4x4-block intra using the luma-inherited modes. */
static void c444_i4(y264_frame_t *f, int mbx, int mby, int comp,
                    const int8_t modes[16], struct i4_result *r)
{
    const int trel = rd_trellis(f);   /* hoisted: one TLS walk per block loop */
    int p = 1 + comp;
    int ss = f->src_stride[p], rs = f->rec_stride[p];
    int8_t loc[4][4];
    for (int blk = 0; blk < 16; blk++) {
        int bxm = BLK_X[blk], bym = BLK_Y[blk];
        int ax = mbx * 4 + bxm, ay = mby * 4 + bym;
        const pixel *src = f->src[p] + (ay * 4) * ss + ax * 4;
        pixel *rec = f->rec[p] + (ay * 4) * rs + ax * 4;
        int ht = ay > 0, hl = ax > 0, htl = ht && hl;
        int htr = topright_avail(f, mbx, mby, bxm, bym);
        int mode = modes[blk];
        pixel pred[16];
        y264_intra4x4(pred, rec, rs, mode, ht, hl, htl, htr);
        dctcoef coef[16], lev[16], rcoef[16];
        y264_sub4x4_dct(coef, src, ss, pred, 4);
        int nza = bxm > 0 ? (loc[bym][bxm-1] > 0) : cbf_nb(f, p, ax - 1, ay, 1);
        int nzb = bym > 0 ? (loc[bym-1][bxm] > 0) : cbf_nb(f, p, ax, ay - 1, 1);
        rdoq_4x4_ctx(f->cabac, src, ss, pred, 4, coef, lev, f->cur_chroma_qp_scaled, lambda_trellis(f->cur_chroma_qp, 1), 1, 0, 2, cqm_w4(f, 1), nza, nzb, 0, f->subme, psy_trellis_qp(f, f->cur_chroma_qp), trel);
        y264_dequant_4x4(lev, rcoef, f->cur_chroma_qp_scaled, cqm_w4(f, 1));
        y264_add4x4_idct(rec, rs, pred, 4, rcoef);
        int nz = 0;
        for (int k = 0; k < 16; k++) { dctcoef v = lev[ZIGZAG[k]]; r->lev[blk][k] = v; if (v) nz++; }
        r->nnz[blk] = nz;
        loc[bym][bxm] = (int8_t)(nz > 0);
    }
    r->cbp_luma = 0;
    for (int i8 = 0; i8 < 4; i8++)
        for (int i4 = 0; i4 < 4; i4++)
            if (r->nnz[i8 * 4 + i4]) r->cbp_luma |= (1 << i8);
}

/* Mark this macroblock's 4x4 luma blocks as non-Intra4x4 (DC) for later mode
 * prediction, and stamp the motion field. */
static void mark_mb_meta(y264_frame_t *f, int mbx, int mby, int mvx, int mvy, int ref)
{
    int ms = f->i4mode_stride;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            f->i4mode[(mby * 4 + by) * ms + (mbx * 4 + bx)] = 2;
    if (f->slice_type == 1)
        set_mb_motion(f, mbx, mby, mvx, mvy, ref);
}

/* Result of intra analysis, enough to write the macroblock syntax later. */
struct intra_mb {
    int use_i4;                 /* I_NxN chosen (vs I_16x16) */
    int use_i8;                 /* I_NxN with 8x8 transform (implies use_i4) */
    int cost;
    struct luma_result lr;
    struct i4_result ir;
    struct i8_result i8;
    struct chroma_result cr;
    /* 4:4:4 only: Cb/Cr coded like luma with the inherited luma mode. lr_c holds
 * the I_16x16 result per component; ir_c the I_4x4 result per component. */
    struct luma_result lr_c[2];
    struct i4_result ir_c[2];
};

/* Estimated luma bits for each intra mode, for the RD decision. Residual bits use
 * the CAVLC cost (a monotone proxy that also tracks CABAC ordering); ~2 bits per
 * coded block covers prediction-mode signalling, so partitioned modes carry their
 * side-info penalty. Chroma is identical across luma modes and cancels. */
static long i16_luma_bits(const struct luma_result *lr)
{
    long b = block_bits(lr->dc_scan, 16);
    if (lr->cbp_luma)
        for (int i = 0; i < 16; i++)
            b += block_bits(lr->ac_scan[i], 15);   /* prices scan[0..14] only */
    return b;
}
static long i4_luma_bits(const struct i4_result *ir)
{
    long b = 0;
    for (int i = 0; i < 16; i++) {
        b += 2;
        if (ir->nnz[i]) b += block_bits(ir->lev[i], 16);
    }
    return b;
}
static long i8_luma_bits(const struct i8_result *i8)
{
    long b = 1;
    for (int blk = 0; blk < 4; blk++) {
        b += 2;
        if (i8->cbp_luma & (1 << blk)) {
            dctcoef scan8[64];
            for (int k = 0; k < 64; k++) scan8[k] = i8->lev[blk][ZIGZAG8[k]];
            b += block_bits_8x8(scan8);
        }
    }
    return b;
}

static long ssd_luma_mb(y264_frame_t *f, int mbx, int mby)
{
    return ssd_block(f->src[0] + (mby * 16) * f->src_stride[0] + mbx * 16, f->src_stride[0],
                     f->rec[0] + (mby * 16) * f->rec_stride[0] + mbx * 16, f->rec_stride[0],
                     16, 16);
}

/* Inter-frame intra pruning: return 1 to run the
 * expensive I_4x4/I_8x8 sub-search; return 0 to keep only the cheap I_16x16 when
 * the inter SATD already beats it (a finer intra rarely then wins the RD). On at
 * the medium tier (subme<=8); Y264_INTRA_SKIP forces on(1)/off(0). inter_satd < 0
 * (I frames, or the callers that don't pass one) always runs the full search. */
static int intra_fine_m16(void)
{
    static int fm16 = -2;
    if (fm16 == -2) { const char *e = getenv("Y264_INTRA_FINE_M"); fm16 = e ? atoi(e) : 16; }
    return fm16;
}

static int intra_fine_on(int subme, long inter_satd, long i16_satd)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_INTRA_SKIP"); env = e ? atoi(e) : -1; }
    int on = env >= 0 ? env : ((subme > 0 ? subme : 10) <= 8);
    if (!on || inter_satd < 0) return 1;
    /* Skip the fine (i4/i8) search when i16 SATD says intra is well behind
 * inter. Margin in 1/16 units (Y264_INTRA_FINE_M); a 2x margin (32) is
 * vacuous against the 1.5x ADMISSION margin -- every admitted MB then runs
 * the full i4+i8 encode. A finer intra can still beat
 * inter when i16 is merely somewhat worse, so this stays looser than a
 * winner-take-all gate. */
    return (long)i16_satd <= inter_satd * intra_fine_m16() / 16;
}

/* W-B (S1): whether to run the full intra encode in inter-frame (P/B) analysis
 * at all. x264 SATD-screens intra and only RD-codes it when it is competitive
 * with the inter winner; without the screen every inter MB pays a full i16
 * encode (fdct+RDOQ+recon+chroma). Off at subme>=9 (byte-identical). Env
 * Y264_INTRA_SCREEN: 0 force off, 1 force on. */
/* Q3 probe accessor: Y264_INTRA_RDBONUS=<x256>[,<qp0>], idx 0 = scale (0 =
 * off), idx 1 = qp0 (default 40). Warmed at open like every env static. */
static int intra_rdbonus(int idx)
{
    static int v[2] = { -2, 40 };
    if (v[0] == -2) {
        const char *e = getenv("Y264_INTRA_RDBONUS");
        int q = 40;
        if (e) { const char *c = strchr(e, ','); if (c) q = atoi(c + 1); }
        v[1] = q;
        v[0] = e ? atoi(e) : 0;
    }
    return v[idx];
}

static int intra_screen_on(int subme)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_INTRA_SCREEN"); env = e ? atoi(e) : -1; }
    return env >= 0 ? env : ((subme > 0 ? subme : 10) <= 8);
}

/* W-B1 (S1): predict-only i16 SATD screen. Mirrors the mode-decision loop at the
 * head of encode_luma16 (predict each available I16x16 mode, keep the min SATD)
 * WITHOUT the transform/quant/recon tail -- neighbours are the already-decoded
 * top/left pixels, so this returns exactly the best_cost encode_luma16 would. */
static int i16_screen_satd(y264_frame_t *f, int mbx, int mby,
                           int have_top, int have_left)
{
    int stride_s = f->src_stride[0], stride_r = f->rec_stride[0];
    const pixel *src = f->src[0] + (mby * 16) * stride_s + mbx * 16;
    pixel *rec = f->rec[0] + (mby * 16) * stride_r + mbx * 16;

    int best, bmode;
    i16_costs_x3(src, stride_s, rec, stride_r, have_top, have_left, &best, &bmode);
    if (have_top && have_left) {
        pixel pred[256];
        y264_intra16x16(pred, rec, stride_r, Y264_I16_PLANE, have_top, have_left);
        int cost = satd_block(src, stride_s, pred, 16, 16, 16);
        if (cost < best) best = cost;
    }
    return best;
}

/* W-B1 (S1): admit the full intra encode only when the i16 SATD screen shows
 * intra is not hopeless vs the inter SATD winner. x264's tight 9/8 threshold
 * screens too much here: our inter is weaker on motion (a +5.26% deficit), so
 * we lean on P/B intra as a fallback more than x264 does -- 9/8 costs +0.48% BD
 * (stefan +1.68). The 2x margin (matching the BD-neutral intra_fine_on
 * i4/i8 early-out) skips only truly-hopeless intra. Returns 1 = run intra, 0 =
 * screen it out. inter_satd<0 (unknown / subme>=9) always admits. */
static int intra_admit_m16(void)
{
    static int m16 = -2;
    if (m16 == -2) { const char *e = getenv("Y264_INTRA_ADMIT_M"); m16 = e ? atoi(e) : 24; }
    return m16;
}

/* Feed the intra SATD screen a pure-distortion inter reference instead of the
 * lambda-weighted one. Default OFF: it gates clean (twelve clips, both bands,
 * medians -0.13%/-0.02%) but the wall is 0.3-0.8% at the median, under the 1%
 * bar, because it removes only 2.7 of the 27 admission points it is named
 * after -- `isi` is a min the rate-free direct candidate usually wins, so the
 * two sides are already equal on most B MBs, and the P screen (best_satd,
 * below) has the same mismatch untouched. */
static int intra_screen_pure(void)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_INTRA_SCREEN_PURE"); env = e ? atoi(e) : 0; }
    return env;
}

/* Y264_B_INTRA_ADMIT_M: the intra admission margin for the B side alone,
 * because the two sides do not earn their intra trial equally. On bbb at
 * crf 31, BPROF bills 44.7 ms of B intra (35.1 of it to macroblocks whose
 * verdict is SKIP) and the trial wins 54 macroblocks out of 313,200.
 *
 * DEFAULT 12 (0.75x), against the shared 24 the P side keeps. Band at matched
 * achieved rate, 12 clips both bands: median +0.01%, mean +0.04%, worst bus
 * +0.48%, the only clip past the 0.3% threshold. Wall t12 net of control:
 * sintel +2.49%, park_joy +1.87%, samsung +1.64%, bbb +0.50%, so the natural
 * clips pay for it as much as the animation pair. recon_sweep 300/300.
 * The curve saturates here: 8 reads the same wall on three of the four clips
 * and harms tempete as well (worst +0.71%), and 0 -- never admitting B intra
 * at all -- buys nothing beyond 8, so the trials still admitted are cheap.
 * 8 stays available as the animation-heavy arm; -1 restores the shared
 * P-side margin and is the escape back to the pre-flip path. */
static int intra_admit_m16_b(void)
{
    static int m16 = -2;
    if (m16 == -2) { const char *e = getenv("Y264_B_INTRA_ADMIT_M"); m16 = e ? atoi(e) : 12; }
    return m16 < 0 ? intra_admit_m16() : m16;
}

static int intra_admit_g(y264_frame_t *f, int mbx, int mby, long inter_satd, int isb)
{
    if (!intra_screen_on(f->subme) || inter_satd < 0) return 1;
    /* Admission margin in 1/16 units. A 2x margin (32) keeps intra available
 * as a fallback for weak inter; 24 (1.5x) measures BD-NEUTRAL (corpus mean
 * +0.05%, worst stefan +0.39) for 1.06x CIF / 1.11x 720p pure-C. Tighter margins pay more speed but tempete
 * (high-detail pan) objects: 18 = +2.44%, 20 = +1.06% on that clip alone --
 * intra stays mid-range competitive on texture. Y264_INTRA_ADMIT_M. */
    long i16 = i16_screen_satd(f, mbx, mby, mby > 0, mbx > 0);
    int hit = i16 < inter_satd * (isb ? intra_admit_m16_b() : intra_admit_m16()) / 16;
    NLED(intra_admit_try, 1); NLED(intra_admit_hit, hit);
    if (isb) { NLED(intra_admit_try_b, 1); NLED(intra_admit_hit_b, hit); }
    return hit;
}

static inline int intra_admit(y264_frame_t *f, int mbx, int mby, long inter_satd)
{
    return intra_admit_g(f, mbx, mby, inter_satd, 0);
}

/* Analyse an intra macroblock: pick I_16x16 / I_4x4 / I_8x8 by rate-distortion
 * (J = SSD + lambda*bits) rather than SATD, reconstruct into rec, update the
 * i4mode grid, and fill `o`. Writes no bitstream. inter_satd (>=0) enables the
 * inter-frame early-out (skip I_4x4/I_8x8 when inter beats I_16x16); pass -1 to
 * always run the full search (I frames). */
static void analyze_intra_g(y264_frame_t *f, int mbx, int mby, struct intra_mb *o,
                            long inter_satd)
{
    int ms = f->i4mode_stride;
    int rs = f->rec_stride[0];
    int have_top = mby > 0, have_left = mbx > 0;
    int bx0 = mbx * 4, by0 = mby * 4;
    pixel tmp16[256];

    pixel *reclum = f->rec[0] + (mby * 16) * rs + mbx * 16;
    pixel tmp8[256];

    long lam = lambda_mode16(f->cur_qp);

    int i16_satd = encode_luma16(f, mbx, mby, have_top, have_left, &o->lr);
    long J16 = ssd_luma_mb(f, mbx, mby) + Y264_LAMJ(lam, i16_luma_bits(&o->lr));
    for (int y = 0; y < 16; y++)
        memcpy(tmp16 + y * 16, reclum + y * rs, 16 * sizeof(pixel));

    int fine = intra_fine_on(f->subme, inter_satd, i16_satd);

    long J8 = -1;
    if (fine && f->transform8x8) {
        encode_luma8x8(f, mbx, mby, &o->i8);
        J8 = ssd_luma_mb(f, mbx, mby) + Y264_LAMJ(lam, i8_luma_bits(&o->i8));
        for (int y = 0; y < 16; y++)
            memcpy(tmp8 + y * 16, reclum + y * rs, 16 * sizeof(pixel));
    }

    long J4 = -1;
    if (fine) {
        encode_luma4x4(f, mbx, mby, &o->ir);
        J4 = ssd_luma_mb(f, mbx, mby) + Y264_LAMJ(lam, i4_luma_bits(&o->ir));
    }

    int mode = 16; long best = J16;
    if (J4 >= 0 && J4 < best) { best = J4; mode = 4; }
    if (J8 >= 0 && J8 < best) { best = J8; mode = 8; }
    o->use_i4 = (mode != 16);
    o->use_i8 = (mode == 8);

    if (mode == 16) {
        for (int y = 0; y < 16; y++)
            memcpy(reclum + y * rs, tmp16 + y * 16, 16 * sizeof(pixel));
        for (int by = 0; by < 4; by++)
            for (int bx = 0; bx < 4; bx++)
                f->i4mode[(by0 + by) * ms + (bx0 + bx)] = 2;
    } else if (mode == 8) {
        for (int y = 0; y < 16; y++)
            memcpy(reclum + y * rs, tmp8 + y * 16, 16 * sizeof(pixel));
        for (int blk = 0; blk < 4; blk++)
            for (int yy = 0; yy < 2; yy++)
                for (int xx = 0; xx < 2; xx++)
                    f->i4mode[(by0 + B8_Y[blk] + yy) * ms + (bx0 + B8_X[blk] + xx)] =
                        o->i8.mode[blk];
    }
    if (f->cf_idc == 3) {
        /* 4:4:4: Cb/Cr coded like luma with the inherited luma mode (no 4-mode
 * chroma predictor). I_8x8 chroma is not yet wired; luma avoids I_8x8
 * for 4:4:4 (transform8x8 gated off at open until 3x). */
        for (int comp = 0; comp < 2; comp++) {
            if (mode == 16)
                c444_i16(f, mbx, mby, comp, o->lr.mode, have_top, have_left, &o->lr_c[comp]);
            else
                c444_i4(f, mbx, mby, comp, o->ir.mode, &o->ir_c[comp]);
        }
    } else {
        encode_chroma(f, mbx, mby, have_top, have_left, &o->cr);
    }
    o->cost = (int)best;
}

/* B intra with the fine gate armed (see analyze_b_mb). DEFAULT ON: ABR band
 * improves on 11/12 clips (median -1.0%, worst sintel +0.98%), CRF band
 * neutral within +-0.61%, samsung-lo wall -5.65%. Y264_B_INTRA_FINE=0 is the
 * escape back to the ungated trial. */
static int b_intra_fine_env(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_B_INTRA_FINE"); v = e ? atoi(e) : 1; }
    return v;
}
/* Mid-tournament B_SKIP commit (see analyze_b_mb). DEFAULT ON at 1 (non-ref
 * B's only): CRF band byte-identical 12/12, ABR band worst +0.89.
 * 0 = full tournament, 2 = also exit reference B's under the absolute-SSD
 * bound (experimental), 3 = also exit reference B's whose mb-tree offset is
 * >= 0, the E1 propagation guard (see bx_ref_admit). */
static int b_skip_exit_env(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_B_SKIP_EXIT"); v = e ? atoi(e) : 1; }
    return v;
}
static void analyze_intra_gb(y264_frame_t *f, int mbx, int mby, struct intra_mb *o,
                             long inter_satd)
{
    NLED_SITE_SAVE(prev);
    NLED_SITE(Y264_LED_SITE_BINTRA);
    analyze_intra_g(f, mbx, mby, o, inter_satd);
    NLED_SITE(prev);
}

/* Full intra analysis (no inter-frame early-out) -- I frames and the B path. */
static void analyze_intra(y264_frame_t *f, int mbx, int mby, struct intra_mb *o)
{
    NLED_SITE_SAVE(prev);
    NLED_SITE(prev == Y264_LED_SITE_BME ? Y264_LED_SITE_BINTRA
                                        : Y264_LED_SITE_IFRAME);
    analyze_intra_g(f, mbx, mby, o, -1);
    NLED_SITE(prev);
}

/* 4:4:4 CAVLC: write one chroma component's residual (luma-style), gated by the
 * shared CodedBlockPatternLuma, updating the component's nnz grid. */
/* Author the 4:4:4 Cb/Cr nnz grid (see write_c444_comp; counts nonzeros). The
 * I16 chroma DC (!use_i4) has no grid cell. */
static void author_c444_comp_nnz(y264_frame_t *f, int mbx, int mby,
                                 int comp, int use_i4, int shared_cbp,
                                 const struct luma_result *lr_c, const struct i4_result *ir_c)
{
    int p = 1 + comp, stride = f->nnz_stride[p];
    int8_t *nnz = f->nnz[p];
    int bx0 = mbx * 4, by0 = mby * 4;
    for (int i = 0; i < 16; i++) {
        int bx = bx0 + BLK_X[i], by = by0 + BLK_Y[i];
        int coded = use_i4 ? (shared_cbp & (1 << (i / 4))) != 0 : shared_cbp != 0;
        const dctcoef *lev = use_i4 ? ir_c->lev[i] : lr_c->ac_scan[i];
        int n = use_i4 ? 16 : 15;
        nnz[by * stride + bx] = coded ? (int8_t)nz_count(lev, n) : 0;
    }
}

static void emit_c444_comp_cavlc(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                                 int comp, int use_i4, int shared_cbp,
                                 const struct luma_result *lr_c, const struct i4_result *ir_c)
{
    int p = 1 + comp, stride = f->nnz_stride[p];
    int8_t *nnz = f->nnz[p];
    int bx0 = mbx * 4, by0 = mby * 4;
    if (!use_i4) {
        int ncdc = derive_nc(nnz, stride, bx0, by0);
        y264_cavlc_residual(bs, lr_c->dc_scan, 16, ncdc);          /* I16 chroma DC */
        for (int i = 0; i < 16; i++) {
            int bx = bx0 + BLK_X[i], by = by0 + BLK_Y[i];
            if (shared_cbp) {
                int nc = derive_nc(nnz, stride, bx, by);
                dctcoef buf[16];
                for (int k = 0; k < 15; k++) buf[k] = lr_c->ac_scan[i][k];
                y264_cavlc_residual(bs, buf, 15, nc);
            }
        }
    } else {
        for (int i8 = 0; i8 < 4; i8++)
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4, bx = bx0 + BLK_X[blk], by = by0 + BLK_Y[blk];
                if (shared_cbp & (1 << i8)) {
                    int nc = derive_nc(nnz, stride, bx, by);
                    y264_cavlc_residual(bs, ir_c->lev[blk], 16, nc);
                }
            }
    }
}


/* 4:4:4 intra macroblock (CAVLC): Cb/Cr coded like luma, one shared cbp gates all
 * three planes, no intra_chroma_pred_mode. */
static void emit_intra444_cavlc(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                                int mbt_off, const struct intra_mb *o)
{
    int lstride = f->nnz_stride[0];
    int8_t *lnnz = f->nnz[0];
    int ms = f->i4mode_stride, bx0 = mbx * 4, by0 = mby * 4;
    const struct luma_result *lr = &o->lr;
    const struct i4_result *ir = &o->ir;

    if (o->use_i4) {
        int cbp = ir->cbp_luma | o->ir_c[0].cbp_luma | o->ir_c[1].cbp_luma;  /* shared */
        y264_bs_write_ue(bs, 0 + mbt_off);          /* I_NxN */
        if (f->transform8x8) y264_bs_write1(bs, 0); /* transform_size_8x8_flag (4:4:4 8x8 tbd) */
        for (int blk = 0; blk < 16; blk++) {
            int ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
            int pm;
            if (ay == 0 || ax == 0) pm = 2;
            else {
                int a = f->i4mode[ay*ms+(ax-1)], b = f->i4mode[(ay-1)*ms+ax];
                pm = a < b ? a : b;
            }
            int ch = ir->mode[blk];
            if (ch == pm) y264_bs_write1(bs, 1);
            else { y264_bs_write1(bs, 0); y264_bs_write(bs, 3, ch < pm ? ch : ch - 1); }
        }
        y264_bs_write_ue(bs, cbp444_to_codenum(cbp, 0));
        if (cbp) qpd_cavlc(bs, f, f->cur_qp);
        for (int i8 = 0; i8 < 4; i8++)
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                if (cbp & (1 << i8)) {
                    int nc = derive_nc(lnnz, lstride, ax, ay);
                    y264_cavlc_residual(bs, ir->lev[blk], 16, nc);
                }
            }
        emit_c444_comp_cavlc(bs, f, mbx, mby, 0, 1, cbp, NULL, &o->ir_c[0]);
        emit_c444_comp_cavlc(bs, f, mbx, mby, 1, 1, cbp, NULL, &o->ir_c[1]);
    } else {
        int cbp = (lr->cbp_luma || o->lr_c[0].cbp_luma || o->lr_c[1].cbp_luma) ? 15 : 0;
        int mb_type = 1 + lr->mode + (cbp ? 12 : 0);  /* cbp_chroma field = 0 for 4:4:4 */
        y264_bs_write_ue(bs, mb_type + mbt_off);
        qpd_cavlc(bs, f, f->cur_qp);
        int ncdc = derive_nc(lnnz, lstride, bx0, by0);
        y264_cavlc_residual(bs, lr->dc_scan, 16, ncdc);
        for (int i = 0; i < 16; i++) {
            int bx = bx0 + BLK_X[i], by = by0 + BLK_Y[i];
            if (cbp) {
                int nc = derive_nc(lnnz, lstride, bx, by);
                dctcoef buf[16];
                for (int k = 0; k < 15; k++) buf[k] = lr->ac_scan[i][k];
                y264_cavlc_residual(bs, buf, 15, nc);
            }
        }
        emit_c444_comp_cavlc(bs, f, mbx, mby, 0, 0, cbp, &o->lr_c[0], NULL);
        emit_c444_comp_cavlc(bs, f, mbx, mby, 1, 0, cbp, &o->lr_c[1], NULL);
    }
    if (f->mb_tr8) f->mb_tr8[mby * f->wmb + mbx] = 0;
    if (f->slice_type == 1) set_mb_intra_motion(f, mbx, mby);
}

/* Author the intra residual nnz grids (luma + chroma) from the analysed result;
 * no bitstream. Covers 4:4:4, I_8x8, I_4x4, and I_16x16 layouts. */
static void author_intra_residual(y264_frame_t *f, int mbx, int mby,
                                  const struct intra_mb *o)
{
    int lstride = f->nnz_stride[0];
    int8_t *lnnz = f->nnz[0];
    int bx0 = mbx * 4, by0 = mby * 4;
    const struct luma_result *lr = &o->lr;
    const struct i4_result *ir = &o->ir;

    if (f->cf_idc == 3) {
        if (o->use_i4) {
            int cbp = ir->cbp_luma | o->ir_c[0].cbp_luma | o->ir_c[1].cbp_luma;
            for (int i8 = 0; i8 < 4; i8++)
                for (int i4 = 0; i4 < 4; i4++) {
                    int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                    lnnz[ay * lstride + ax] =
                        (cbp & (1 << i8)) ? (int8_t)nz_count(ir->lev[blk], 16) : 0;
                }
            author_c444_comp_nnz(f, mbx, mby, 0, 1, cbp, NULL, &o->ir_c[0]);
            author_c444_comp_nnz(f, mbx, mby, 1, 1, cbp, NULL, &o->ir_c[1]);
        } else {
            int cbp = (lr->cbp_luma || o->lr_c[0].cbp_luma || o->lr_c[1].cbp_luma) ? 15 : 0;
            for (int i = 0; i < 16; i++) {          /* I16 luma AC (DC has no grid cell) */
                int ax = bx0 + BLK_X[i], ay = by0 + BLK_Y[i];
                lnnz[ay * lstride + ax] = cbp ? (int8_t)nz_count(lr->ac_scan[i], 15) : 0;
            }
            author_c444_comp_nnz(f, mbx, mby, 0, 0, cbp, &o->lr_c[0], NULL);
            author_c444_comp_nnz(f, mbx, mby, 1, 0, cbp, &o->lr_c[1], NULL);
        }
        return;
    }

    if (o->use_i4) {
        if (o->use_i8) {
            for (int blk = 0; blk < 4; blk++) {
                if (o->i8.cbp_luma & (1 << blk))
                    author_luma8x8_nnz(f, mbx, mby, blk, o->i8.lev[blk]);
                else
                    for (int j = 0; j < 4; j++) {
                        int lb = blk * 4 + j;
                        lnnz[(by0 + BLK_Y[lb]) * lstride + (bx0 + BLK_X[lb])] = 0;
                    }
            }
        } else {
            for (int i8 = 0; i8 < 4; i8++)
                for (int i4 = 0; i4 < 4; i4++) {
                    int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                    lnnz[ay * lstride + ax] =
                        (ir->cbp_luma & (1 << i8)) ? (int8_t)nz_count(ir->lev[blk], 16) : 0;
                }
        }
    } else {
        for (int i = 0; i < 16; i++) {              /* I16 luma AC (DC has no grid cell) */
            int ax = bx0 + BLK_X[i], ay = by0 + BLK_Y[i];
            lnnz[ay * lstride + ax] = lr->cbp_luma ? (int8_t)nz_count(lr->ac_scan[i], 15) : 0;
        }
    }
    author_chroma_residual_nnz(f, mbx, mby, &o->cr);
}

/* Emit the syntax for a previously analysed intra macroblock, with mb_type
 * offset `mbt_off` (0 in I slices, 5 in P slices). Reads nnz from the authored
 * grid; leaves motion/mb_tr8 grids consistent for later MBs. */
static void emit_intra_syntax(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                              int mbt_off, const struct intra_mb *o)
{
    if (f->cf_idc == 3) { emit_intra444_cavlc(bs, f, mbx, mby, mbt_off, o); return; }
    int lstride = f->nnz_stride[0];
    int8_t *lnnz = f->nnz[0];
    int ms = f->i4mode_stride;
    int bx0 = mbx * 4, by0 = mby * 4;
    const struct luma_result *lr = &o->lr;
    const struct i4_result *ir = &o->ir;
    const struct chroma_result *cr = &o->cr;

    if (o->use_i4) {
        y264_bs_write_ue(bs, 0 + mbt_off);      /* mb_type I_NxN */
        if (f->transform8x8)
            y264_bs_write1(bs, o->use_i8 ? 1 : 0);   /* transform_size_8x8_flag */
        int nblk = o->use_i8 ? 4 : 16;
        for (int blk = 0; blk < nblk; blk++) {
            int ax = bx0 + (o->use_i8 ? B8_X[blk] : BLK_X[blk]);
            int ay = by0 + (o->use_i8 ? B8_Y[blk] : BLK_Y[blk]);
            int pred_mode;
            if (ay == 0 || ax == 0)
                pred_mode = 2;
            else {
                int a = f->i4mode[ay * ms + (ax - 1)];
                int b = f->i4mode[(ay - 1) * ms + ax];
                pred_mode = a < b ? a : b;
            }
            int chosen = o->use_i8 ? o->i8.mode[blk] : ir->mode[blk];
            if (chosen == pred_mode)
                y264_bs_write1(bs, 1);
            else {
                y264_bs_write1(bs, 0);
                y264_bs_write(bs, 3, chosen < pred_mode ? chosen : chosen - 1);
            }
        }
        y264_bs_write_ue(bs, cr->mode);
        int cbp_luma = o->use_i8 ? o->i8.cbp_luma : ir->cbp_luma;
        int cbp = cbp_luma | (cr->cbp << 4);
        y264_bs_write_ue(bs, cbp_to_codenum(cbp));
        if (cbp > 0)
            qpd_cavlc(bs, f, f->cur_qp);

        if (o->use_i8) {
            for (int blk = 0; blk < 4; blk++)
                if (o->i8.cbp_luma & (1 << blk))
                    emit_luma8x8_residual_cavlc(bs, f, mbx, mby, blk, o->i8.lev[blk]);
        } else for (int i8 = 0; i8 < 4; i8++)
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4;
                int ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                if (ir->cbp_luma & (1 << i8)) {
                    int nc = derive_nc(lnnz, lstride, ax, ay);
                    y264_cavlc_residual(bs, ir->lev[blk], 16, nc);
                }
            }
    } else {
        int mb_type = 1 + lr->mode + 4 * cr->cbp + (lr->cbp_luma ? 12 : 0);
        y264_bs_write_ue(bs, mb_type + mbt_off);
        y264_bs_write_ue(bs, cr->mode);
        qpd_cavlc(bs, f, f->cur_qp);

        int ncdc = derive_nc(lnnz, lstride, bx0, by0);
        y264_cavlc_residual(bs, lr->dc_scan, 16, ncdc);

        if (lr->cbp_luma)
            for (int i = 0; i < 16; i++) {
                int bx = bx0 + BLK_X[i], by = by0 + BLK_Y[i];
                int nc = derive_nc(lnnz, lstride, bx, by);
                dctcoef buf[16];
                for (int k = 0; k < 15; k++) buf[k] = lr->ac_scan[i][k];
                y264_cavlc_residual(bs, buf, 15, nc);
            }
    }

    emit_chroma_residual(bs, f, mbx, mby, cr);
    if (f->mb_tr8)
        f->mb_tr8[mby * f->wmb + mbx] = (pixel)o->use_i8;
    if (f->slice_type == 1)
        set_mb_intra_motion(f, mbx, mby);           /* intra: both lists -1 */
}

/* Wrapper: author grids then emit, for the current single-pass callers. */
static void write_intra_syntax(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                               int mbt_off, const struct intra_mb *o)
{
    author_intra_residual(f, mbx, mby, o);
    emit_intra_syntax(bs, f, mbx, mby, mbt_off, o);
}

/* Chroma residual coding for an inter macroblock: motion-compensated
 * prediction is supplied in pred[2][64]; quantisation uses the inter bias. */
/* rec = Clip1(pred + dc) over a 4x4 block: the DC-only chroma reconstruction,
 * and at these rates it is the COMMON case -- no AC coefficient survives the
 * quantiser, so the whole block is its prediction plus a constant. Written a
 * row at a time through a local so the four samples are one fixed-length
 * store instead of four strided ones. */
static inline void add_dc_4x4(pixel *rec, int rs, const pixel *pred, int ps, int dc)
{
    for (int y = 0; y < 4; y++) {
        pixel t[4];
        for (int x = 0; x < 4; x++)
            t[x] = (pixel)clip8(pred[y * ps + x] + dc);
        memcpy(rec + y * rs, t, 4 * sizeof(pixel));
    }
}

static void encode_chroma_inter(y264_frame_t *f, int mbx, int mby,
                                pixel pred[2][256], struct chroma_result *cr)
{
    const int trel = rd_trellis(f);   /* hoisted: one TLS walk per block loop */
    int cw = 16 / f->sub_w, ch = 16 / f->sub_h;
    int cbw = f->cbw, cbh = f->cbh, nblk = cbw * cbh;
    int any_ac = 0, any_dc = 0;
    cr->mode = 0;
    cr->ndc = nblk;
    dctcoef ac_lev[2][Y264_CHROMA_MAXBLK][16];
    int nzc[2][Y264_CHROMA_MAXBLK];
    for (int c = 0; c < 2; c++) {
        int ss = f->src_stride[1 + c], rs = f->rec_stride[1 + c];
        const pixel *src = f->src[1 + c] + (mby * ch) * ss + mbx * cw;
        pixel *rec = f->rec[1 + c] + (mby * ch) * rs + mbx * cw;

        dctcoef dc_raster[Y264_CHROMA_MAXBLK];
        int8_t loc[4][2];
        int cbx0 = mbx * cbw, cby0 = mby * cbh;
        dctcoef coefs[Y264_CHROMA_MAXBLK][16];
        y264_sub_dct4_blocks(coefs, cbw, cbh, src, ss, pred[c], cw);
        for (int blk = 0; blk < nblk; blk++) {
            int bx = blk % cbw, by = blk / cbw;
            int nza = bx > 0 ? (loc[by][bx-1] > 0)
                             : cbf_nb(f, 1 + c, cbx0 + bx - 1, cby0 + by, 0);
            int nzb = by > 0 ? (loc[by-1][bx] > 0)
                             : cbf_nb(f, 1 + c, cbx0 + bx, cby0 + by - 1, 0);
            dctcoef *coef = coefs[by * cbw + bx];
            dctcoef lev[16];
            const pixel *bsrc = src + (by * 4) * ss + bx * 4;
            const pixel *bpred = pred[c] + (by * 4) * cw + bx * 4;
            dc_raster[by * cbw + bx] = coef[0];
            rdoq_4x4_ctx(f->cabac, bsrc, ss, bpred, cw, coef, lev, f->cur_chroma_qp_scaled, lambda_trellis(f->cur_chroma_qp, 0), 0, 1, 4, cqm_w4(f, 0), nza, nzb, 0, f->subme, psy_trellis_qp(f, f->cur_chroma_qp), trel);
            memcpy(ac_lev[c][blk], lev, sizeof lev);
            int nz = 0;
            for (int k = 0; k < 16; k++) nz += lev[k] != 0;   /* branchless */
            nzc[c][blk] = nz;
            if (nz) any_ac = 1;          /* lev[0] is forced 0 (AC block), so
 * nz counts exactly the AC nonzeros */
            loc[by][bx] = (int8_t)(nz > 0);
        }
        dctcoef dclev[Y264_CHROMA_MAXBLK];
        if (chroma_dc_fwd(f, 0, dc_raster, dclev, cr->dc_scan[c])) any_dc = 1;

        dctcoef rdc[Y264_CHROMA_MAXBLK];
        chroma_dc_inv(f, 0, dclev, rdc);
        for (int blk = 0; blk < nblk; blk++) {
            int bx = blk % cbw, by = blk / cbw;
            if (!loc[by][bx]) {              /* DC-only fast path (see i16 recon) */
                int flat = (rdc[by * cbw + bx] + 32) >> 6;
                if (!flat) {                 /* zero DC too: rec = pred rows */
                    for (int y = 0; y < 4; y++)
                        memcpy(rec + (by*4+y)*rs + bx*4,
                               pred[c] + (by*4+y)*cw + bx*4, 4 * sizeof(pixel));
                } else {
                    add_dc_4x4(rec + (by*4)*rs + bx*4, rs,
                               pred[c] + (by*4)*cw + bx*4, cw, flat);
                }
                continue;
            }
            dctcoef coef[16];
            y264_dequant_4x4(ac_lev[c][blk], coef, f->cur_chroma_qp_scaled, cqm_w4(f, 0));
            coef[0] = rdc[by * cbw + bx];
            y264_add4x4_idct(rec + (by * 4) * rs + bx * 4, rs,
                             pred[c] + (by * 4) * cw + bx * 4, cw, coef);
        }
    }
    cr->cbp = any_ac ? 2 : (any_dc ? 1 : 0);
    /* ac_scan/ac_nnz are only read when cbp == 2 (every consumer gates on it:
 * author_chroma_residual_nnz, emit_chroma_residual, the cabac chroma AC
 * writer), so fill them only then; a zero block's row is a memset --
 * identical values (its ac_lev is all-zero), not a re-gather. */
    if (any_ac) {
        for (int c = 0; c < 2; c++)
            for (int blk = 0; blk < nblk; blk++) {
                if (nzc[c][blk])
                    for (int k = 0; k < 15; k++)
                        cr->ac_scan[c][blk][k] = ac_lev[c][blk][ZIGZAG[k + 1]];
                else
                    memset(cr->ac_scan[c][blk], 0, 15 * sizeof(dctcoef));
                cr->ac_nnz[c][blk] = nzc[c][blk];   /* same count: lev[0] == 0 */
            }
    } else {
        for (int c = 0; c < 2; c++)
            for (int blk = 0; blk < nblk; blk++)
                cr->ac_nnz[c][blk] = 0;
    }
}

struct inter_result {
    int part;                   /* 0 = 16x16, 1 = 16x8, 2 = 8x16, 3 = P_8x8 */
    int ref[4];                 /* P multi-ref: list-0 refIdx per partition */
    int sub[4];                 /* P_8x8: sub_mb_type per 8x8 (0=8x8 1=8x4 2=4x8 3=4x4) */
    int bmode;                  /* B slices: 0 = L0, 1 = L1, 2 = Bi (uniform) */
    int b8m[4];                 /* B_8x8 (bpart 3): per-8x8 sub_mb_type, using the
 * standard's own numbering -- 0 = B_Direct_8x8,
 * 1 = B_L0_8x8, 2 = B_L1_8x8, 3 = B_Bi_8x8. The
 * motion for quadrant b lives at mv[b] (list 0)
 * and mv[4+b] (list 1); ref[b] is its list-0
 * reference. */
    int bpart;                  /* B slices: 0 = 16x16 modes, 1 = 16x8, 2 = 8x16,
 * 3 = B_8x8 (four independent quadrants)
 * (both partitions use bmode; mv/pmv layout:
 * [0..1] = L0 p0/p1, [2..3] = L1 p0/p1;
 * ref[0..1] = per-partition list-0 refs) */
    int mvx[16], mvy[16];       /* per-(sub)partition motion; P_8x8 at b*4+s */
    int pmvx[16], pmvy[16];     /* MV predictor for mvd coding */
    int cbp_luma;
    dctcoef lev[16][16];        /* scan-order levels per blkIdx (LumaLevel4x4) */
    int nnz[16];
    int tr8;                    /* 1 = 8x8 luma transform (transform_size_8x8_flag) */
    dctcoef lev8[4][64];        /* raster-order 8x8 levels per quadrant (tr8) */
    int nnz8[4];                /* coeff count per 8x8 quadrant (tr8) */
    struct chroma_result cr;
    /* 4:4:4 only: Cb/Cr coded like luma inter (16 4x4 blocks per component). */
    struct i4_result cr_c[2];
    int cbp444;                 /* 4:4:4 shared cbp (luma|Cb|Cr per 8x8) */
};

/* Sub-partitions per sub_mb_type, and sub-partition s's pixel rect within its
 * 8x8 (shapes: 0 = 8x8, 1 = 8x4, 2 = 4x8, 3 = 4x4, subs in raster order). */
static const int SUB_NS[4] = { 1, 2, 2, 4 };
static void sub_rect(int shape, int s, int *ox, int *oy, int *w, int *h)
{
    if (shape == 0)      { *ox = 0;           *oy = 0;            *w = 8; *h = 8; }
    else if (shape == 1) { *ox = 0;           *oy = s * 4;        *w = 8; *h = 4; }
    else if (shape == 2) { *ox = s * 4;       *oy = 0;            *w = 4; *h = 8; }
    else                 { *ox = (s & 1) * 4; *oy = (s >> 1) * 4; *w = 4; *h = 4; }
}

/* Build the motion-compensated prediction for a partition layout. Each partition
 * predicts from its own list-0 reference plane (pref[p]); single-ref collapses to
 * pref = {0,0} = f->refs[0] = f->ref. */
static void build_inter_pred(y264_frame_t *f, int mbx, int mby, int part,
                             const int *mvx, const int *mvy, const int *pref,
                             const int *psub, pixel pred[256], pixel cpred[2][256])
{
    int refs = f->ref_stride[0], pw = f->padded_w, ph = f->padded_h;
    int lx = mbx * 16, ly = mby * 16;
    const pixel *r0 = f->refs[pref[0]][0], *r1 = f->refs[pref[1]][0];
    /* S1: read the frame-wide half-pel planes (built once per ref) instead of
 * re-running the 6-tap filter per RD candidate. Byte-identical to y264_mc_luma;
 * a real win on the scalar/no-SIMD path (copy/average vs 6-tap convolution). */
    if (part == 0) {
        y264_me_mc_luma(pred, r0, refs, pw, ph, lx, ly, mvx[0], mvy[0], 16, 16);
    } else if (part == 1) {                 /* 16x8: top, bottom */
        y264_me_mc_luma(pred, r0, refs, pw, ph, lx, ly, mvx[0], mvy[0], 16, 8);
        y264_me_mc_luma(pred + 8 * 16, r1, refs, pw, ph, lx, ly + 8,
                        mvx[1], mvy[1], 16, 8);
    } else if (part == 2) {                 /* 8x16: left, right */
        y264_me_mc_luma(pred, r0, refs, pw, ph, lx, ly, mvx[0], mvy[0], 8, 16);
        y264_me_mc_luma(pred + 8, r1, refs, pw, ph, lx + 8, ly,
                        mvx[1], mvy[1], 8, 16);
    } else {                                /* P_8x8: per-8x8 sub-partitions */
        for (int b = 0; b < 4; b++) {
            int Bx = (b & 1) * 8, By = (b >> 1) * 8;
            for (int s2 = 0; s2 < SUB_NS[psub[b]]; s2++) {
                int ox, oy, w, h;
                sub_rect(psub[b], s2, &ox, &oy, &w, &h);
                y264_me_mc_luma(pred + (By + oy) * 16 + Bx + ox,
                                f->refs[pref[b]][0], refs, pw, ph,
                                lx + Bx + ox, ly + By + oy,
                                mvx[b * 4 + s2], mvy[b * 4 + s2], w, h);
            }
        }
    }
    /* Explicit P luma weighting, per partition (partitions may use different
 * references with different weights). */
    if (part == 0) {
        apply_wp_luma(f, pred, 16, 16, 16, pref[0]);
    } else if (part == 1) {
        apply_wp_luma(f, pred, 16, 16, 8, pref[0]);
        apply_wp_luma(f, pred + 8 * 16, 16, 16, 8, pref[1]);
    } else if (part == 2) {
        apply_wp_luma(f, pred, 16, 8, 16, pref[0]);
        apply_wp_luma(f, pred + 8, 16, 8, 16, pref[1]);
    } else {
        for (int b = 0; b < 4; b++)
            apply_wp_luma(f, pred + (b >> 1) * 8 * 16 + (b & 1) * 8, 16, 8, 8, pref[b]);
    }
    if (!cpred)
        return;                 /* A1(a): luma-only build for the qpel-RD nudge trials */
    if (f->cf_idc == 3) {
        /* 4:4:4: chroma is full-res, so MC mirrors luma exactly (luma 6-tap, same
 * MVs/partitions) on each chroma plane. (WP off; chroma WP is separate.) */
        for (int c = 0; c < 2; c++) {
            const pixel *cr0 = f->refs[pref[0]][1 + c], *cr1 = f->refs[pref[1]][1 + c];
            int crs = f->ref_stride[1 + c];
            pixel *cp = cpred[c];
            if (part == 0) {
                y264_mc_luma_b(cp, 16, cr0, crs, pw, ph, lx, ly, mvx[0], mvy[0], 16, 16, Y264_CHROMA_BORDER);
            } else if (part == 1) {
                y264_mc_luma_b(cp, 16, cr0, crs, pw, ph, lx, ly, mvx[0], mvy[0], 16, 8, Y264_CHROMA_BORDER);
                y264_mc_luma_b(cp + 8 * 16, 16, cr1, crs, pw, ph, lx, ly + 8, mvx[1], mvy[1], 16, 8, Y264_CHROMA_BORDER);
            } else if (part == 2) {
                y264_mc_luma_b(cp, 16, cr0, crs, pw, ph, lx, ly, mvx[0], mvy[0], 8, 16, Y264_CHROMA_BORDER);
                y264_mc_luma_b(cp + 8, 16, cr1, crs, pw, ph, lx + 8, ly, mvx[1], mvy[1], 8, 16, Y264_CHROMA_BORDER);
            } else {
                for (int b = 0; b < 4; b++) {
                    int Bx = (b & 1) * 8, By = (b >> 1) * 8;
                    for (int s2 = 0; s2 < SUB_NS[psub[b]]; s2++) {
                        int ox, oy, w, h;
                        sub_rect(psub[b], s2, &ox, &oy, &w, &h);
                        y264_mc_luma_b(cp + (By + oy) * 16 + Bx + ox, 16,
                                     f->refs[pref[b]][1 + c], crs, pw, ph,
                                     lx + Bx + ox, ly + By + oy,
                                     mvx[b * 4 + s2], mvy[b * 4 + s2], w, h, Y264_CHROMA_BORDER);
                    }
                }
            }
        }
        return;
    }
    int sw = f->sub_w, sh = f->sub_h, cw = 16 / sw, chh = 16 / sh;
    for (int c = 0; c < 2; c++) {
        const pixel *c0 = f->refs[pref[0]][1 + c], *c1 = f->refs[pref[1]][1 + c];
        int crs = f->ref_stride[1 + c], cpw = pw / sw, cph = ph / sh;
        int cx = mbx * cw, cy = mby * chh;
        if (part == 0) {
            y264_mc_chroma(cpred[c], cw, c0, crs, cpw, cph, cx, cy, mvx[0], mvy[0], cw, chh, sw, sh);
        } else if (part == 1) {                 /* 16x8 -> chroma cw x (chh/2) */
            int h2 = chh / 2;
            y264_mc_chroma(cpred[c], cw, c0, crs, cpw, cph, cx, cy, mvx[0], mvy[0], cw, h2, sw, sh);
            y264_mc_chroma(cpred[c] + h2 * cw, cw, c1, crs, cpw, cph, cx, cy + h2,
                           mvx[1], mvy[1], cw, h2, sw, sh);
        } else if (part == 2) {                 /* 8x16 -> chroma (cw/2) x chh */
            int w2 = cw / 2;
            y264_mc_chroma(cpred[c], cw, c0, crs, cpw, cph, cx, cy, mvx[0], mvy[0], w2, chh, sw, sh);
            y264_mc_chroma(cpred[c] + w2, cw, c1, crs, cpw, cph, cx + w2, cy,
                           mvx[1], mvy[1], w2, chh, sw, sh);
        } else {
            for (int b = 0; b < 4; b++) {
                int Bx = (b & 1) * (8 / sw), By = (b >> 1) * (8 / sh);
                for (int s2 = 0; s2 < SUB_NS[psub[b]]; s2++) {
                    int ox, oy, w, h;
                    sub_rect(psub[b], s2, &ox, &oy, &w, &h);
                    y264_mc_chroma(cpred[c] + (By + oy / sh) * cw + Bx + ox / sw, cw,
                                   f->refs[pref[b]][1 + c], crs, cpw, cph,
                                   cx + Bx + ox / sw, cy + By + oy / sh,
                                   mvx[b * 4 + s2], mvy[b * 4 + s2], w / sw, h / sh, sw, sh);
                }
            }
        }
    }
}

/* Code the inter luma residual with the 4x4 transform into local buffers.
 * Fills lev (scan-order), nnz, rec4 (256 raster); returns the luma SSD. */
/* dct-decimate (x264, on by default at medium): drop an INTER residual block
 * whose only nonzeros are a few isolated +-1 coefficients — cheaper to send
 * cbf=0 than to code them, at negligible quality cost. A coefficient with
 * |level|>=2 makes the block un-decimatable. Recon-safe: we zero the quantised
 * levels before dequant/idct, so the decoder sees the same (empty) block. */
/* Run-weight table for the 4x4 decimate score, indexed by the zero gap before
 * each nonzero coefficient. A nonzero sitting right after another is expensive
 * to drop and scores 3; isolated ones far down the scan score nothing.
 *
 * Swept rather than assumed, because unlike the tables around it nothing in the
 * standard fixes these values. Eight shapes over the twelve-clip corpus: the
 * head weight is the only term that matters, and it has to be at least 3:
 * the score of an adjacent nonzero pair
 * must clear the decimate threshold, or such a pair becomes droppable and the
 * tool stops meaning what it means. Dropping the head to 2 costs +2.29%, which
 * is that boundary rather than a tuning penalty.
 *
 * Above head 3 the surface is flat -- every candidate landed within +/-0.13% on
 * the CRF band and none separated from the rest on the deep band. The tail below
 * is our point on that plateau, chosen because it reads at or slightly ahead of
 * every alternative on both bands (CRF -0.07%, deep +0.01% over six clips, both
 * far inside the band's own noise) at no cost in speed or size. The threshold
 * it is paired with matters too: at threshold 4 this same table costs +2.37%,
 * so 3/2 is the right partner. */
static const uint8_t DECIMATE_T4[16] = { 3,2,2,1,1,1,1,1,0,0,0,0,0,0,0,0 };

/* Y264_DCTDEC_TAB4: instrument. Override the 4x4 run-weight table with 16
 * comma-separated values, so its shape can be swept rather than asserted.
 * Measurement-only; unset resolves to DECIMATE_T4 above. */
static const uint8_t *dctdec_tab4(void)
{
    static uint8_t ovr[16];
    static int resolved = 0;
    static const uint8_t *sel = DECIMATE_T4;
    if (!resolved) {
        const char *e = getenv("Y264_DCTDEC_TAB4");
        if (e && *e) {
            int n = 0;
            for (const char *p = e; *p && n < 16; ) {
                ovr[n++] = (uint8_t)strtol(p, (char **)&p, 10);
                if (*p == ',') p++; else break;
            }
            if (n == 16) sel = ovr;
        }
        resolved = 1;
    }
    return sel;
}
static const uint8_t DECIMATE_T8[64] = {
    3,3,3,3,2,2,2,2,1,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static int dctdec_cfg(int *t4, int *t8)
{
    /* Thresholds lighter than x264's (6/4): our full RDOQ already drops the
 * marginal coefficients, so x264's threshold over-decimates. 3/2 is a net
 * BD win here (and a small speed win — fewer coeffs to entropy-code). */
    static int on = -1, th4 = 3, th8 = 2;
    if (on < 0) {
        const char *v;
        on = (v = getenv("Y264_DCTDEC")) ? atoi(v) : 1;
        if ((v = getenv("Y264_DCTDEC_T4"))) th4 = atoi(v);
        if ((v = getenv("Y264_DCTDEC_T8"))) th8 = atoi(v);
    }
    *t4 = th4; *t8 = th8; return on;
}

/* Decimate score over a scan-order nonzero bitmask (callers pre-check that no
 * |level| >= 2, which forces keep): sum of the run-weighted table, one term per
 * set bit. Identical to an array walk -- run == the zero gap before each
 * nonzero, and a run can never reach n so an n-1 clamp would be dead. */
static int decimate_mask(uint64_t msk, const uint8_t *tab)
{
    int score = 0, prev = -1;
    while (msk) {
        int i = __builtin_ctzll(msk);
        score += tab[i - prev - 1];
        prev = i;
        msk &= msk - 1;
    }
    return score;
}

static long inter_res_4x4_plane(y264_frame_t *f, int mbx, int mby, int p,
                          const pixel *src, int ss,
                          const pixel pred[256],
                          dctcoef lev[16][16], int nnz[16], pixel rec4[256])
{
    const int trel = rd_trellis(f);   /* hoisted: one TLS walk per block loop */
    /* 4:4:4 codes Cb/Cr like luma but with the chroma QP (QPc); for luma this is
 * cur_qp_scaled. Below QP 30 QPc==QPY so this is a no-op for existing tests. */
    int qp_scaled = p == 0 ? f->cur_qp_scaled : f->cur_chroma_qp_scaled;
    int qp_mode   = p == 0 ? f->cur_qp        : f->cur_chroma_qp;
    int dt4, dt8, dcON = dctdec_cfg(&dt4, &dt8);
    long dist = 0;
    /* Exact coded_block_flag neighbour context (RDOQ cost). Within-MB
 * neighbours are already decided in blkIdx order (left/top always precede);
 * cross-MB neighbours come from the finalized frame nnz grid via cbf_nb.
 * loc[by][bx] = decided nnz of this MB's 4x4 blocks. */
    int8_t loc[4][4];
    int bx0 = mbx * 4, by0 = mby * 4;
    /* Every block's forward transform up front, in one batched call: the
 * sixteen transforms depend only on src and pred, and the loop below
 * consumes them in blkIdx order. Bit-exact with the per-block calls. */
    dctcoef coefs[16][16];
    y264_sub_dct4_blocks(coefs, 4, 4, src, ss, pred, 16);
    for (int blk = 0; blk < 16; blk++) {
        int bx = BLK_X[blk], by = BLK_Y[blk];
        int nza = bx > 0 ? (loc[by][bx-1] > 0)
                         : cbf_nb(f, p, bx0 + bx - 1, by0 + by, 0);
        int nzb = by > 0 ? (loc[by-1][bx] > 0)
                         : cbf_nb(f, p, bx0 + bx, by0 + by - 1, 0);
        dctcoef *coef = coefs[by * 4 + bx];
        dctcoef q[16], rcoef[16];
        const pixel *bsrc = src + (by * 4) * ss + bx * 4;
        const pixel *bpred = pred + (by * 4) * 16 + bx * 4;
        rdoq_4x4_ctx(f->cabac, bsrc, ss, bpred, 16, coef, q, qp_scaled, lambda_trellis(qp_mode, 0), 0, 0, 2, cqm_w4(f, 0), nza, nzb,
                     p == 0, f->subme, psy_trellis_qp(f, qp_mode), trel);
        /* one zigzag pass: scan-order levels + nonzero mask + |level|>=2 flag
 * (big forces keep, so the decimate walk only ever sees +-1 levels) */
        uint32_t msk;
        int big;
        y264_zigzag_scan_4x4(lev[blk], q, &msk, &big);
        if (dcON && !big && decimate_mask(msk, dctdec_tab4()) < dt4) {
            memset(lev[blk], 0, sizeof(lev[blk]));
            msk = 0;
        }
        int nz = __builtin_popcount(msk);
        nnz[blk] = nz;
        loc[by][bx] = (int8_t)(nz > 0);
        /* All-zero block: residual is zero, so rec = clip8(pred) = pred (pred
 * is already a valid pixel). Skip dequant+idct. */
        if (nz) {
            y264_dequant_4x4(q, rcoef, qp_scaled, cqm_w4(f, 0));
            y264_add4x4_idct(rec4 + (by * 4) * 16 + bx * 4, 16, bpred, 16, rcoef);
        } else {
            for (int y = 0; y < 4; y++)
                memcpy(rec4 + (by*4+y)*16 + bx*4, bpred + y * 16, 4 * sizeof(pixel));
        }
    }
    /* recon-vs-source SSD over the whole 16x16: identical to a per-pixel
 * accumulation -- rec - (pred + diff) == rec - src exactly (integer diff,
 * no narrowing in range), summed in the same total. */
    dist = ssd_block(src, ss, rec4, 16, 16, 16);
    return dist;
}

/* Code the inter luma residual with the 8x8 transform into local buffers.
 * Fills lev8 (raster), nnz8, rec8 (256 raster); returns the luma SSD. */
static long inter_res_8x8(y264_frame_t *f, const pixel *src, int ss,
                          const pixel pred[256],
                          dctcoef lev8[4][64], int nnz8[4], pixel rec8[256])
{
    const int trel = rd_trellis(f);   /* hoisted: one TLS walk per block loop */
    long dist = 0;
    int dt4, dt8, dcON = dctdec_cfg(&dt4, &dt8);
    for (int q = 0; q < 4; q++) {
        int ox = (q & 1) * 8, oy = (q >> 1) * 8;
        dctcoef coef[64], lev[64], rcoef[64];
        const pixel *bsrc = src + oy * ss + ox;
        const pixel *bpred = pred + oy * 16 + ox;
        y264_sub8x8_dct8(coef, bsrc, ss, bpred, 16);
        /* rdoq hands back the scan-order nonzero mask + |level|>=2 flag (the
 * old zigzag re-gather, folded into its exits); the raster copy into
 * lev8 is a straight memcpy and nnz a popcount */
        uint64_t msk;
        int big;
        rdoq_8x8(f->cabac, bsrc, ss, bpred, 16, coef, lev, f->cur_qp_scaled, lambda_trellis(f->cur_qp, 0), 0, cqm_w8(f, 0),
                 1, psy_trellis_qp(f, f->cur_qp), f->subme, &msk, &big, trel);
        if (dcON && !big && decimate_mask(msk, DECIMATE_T8) < dt8) {
            memset(lev8[q], 0, sizeof(lev8[q]));
            msk = 0;
        } else {
            memcpy(lev8[q], lev, sizeof(lev8[q]));
        }
        int nz = __builtin_popcountll(msk);
        nnz8[q] = nz;
        /* All-zero block: zero residual, rec = pred. */
        if (nz) {
            y264_dequant_8x8(lev, rcoef, f->cur_qp_scaled, cqm_w8(f, 0));
            y264_add8x8_idct8(rec8 + oy * 16 + ox, 16, bpred, 16, rcoef);
        } else {
            for (int y = 0; y < 8; y++)
                memcpy(rec8 + (oy+y)*16 + ox, bpred + y * 16, 8 * sizeof(pixel));
        }
    }
    /* recon-vs-source SSD (see inter_res_4x4_plane). */
    dist = ssd_block(src, ss, rec8, 16, 16, 16);
    return dist;
}

/* Luma residual bits for one transform choice, scratch-coded with the real nC
 * context (the frame nnz grid), so the 4x4-vs-8x8 decision matches how the bits
 * are actually spent. The nnz grid is saved and restored. Only luma differs
 * between the two transforms; mb_type/mvd/chroma are identical and cancel. */
static long inter_luma_bits(y264_frame_t *f, int mbx, int mby,
                            const struct inter_result *ir, int tr8, int cbp)
{
    int8_t nz[16 + 32]; save_mb_nnz(f, mbx, mby, nz);
    int lstride = f->nnz_stride[0]; int8_t *lnnz = f->nnz[0];
    int bx0 = mbx * 4, by0 = mby * 4;
    y264_bs_t sb; y264_bs_init_count(&sb);        /* pricing only */
    if (tr8) {
        for (int blk = 0; blk < 4; blk++)
            if (cbp & (1 << blk))
                write_luma8x8_residual_cavlc(&sb, f, mbx, mby, blk, ir->lev8[blk]);
            else
                for (int j = 0; j < 4; j++) {
                    int lb = blk * 4 + j;
                    lnnz[(by0 + BLK_Y[lb]) * lstride + (bx0 + BLK_X[lb])] = 0;
                }
    } else {
        for (int i8 = 0; i8 < 4; i8++)
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                if (cbp & (1 << i8)) {
                    int nc = derive_nc(lnnz, lstride, ax, ay);
                    dctcoef b2[16];
                    for (int k = 0; k < 16; k++) b2[k] = ir->lev[blk][k];
                    lnnz[ay * lstride + ax] = (int8_t)y264_cavlc_residual(&sb, b2, 16, nc);
                } else {
                    lnnz[ay * lstride + ax] = 0;
                }
            }
    }
    long bits = y264_bs_pos_bits(&sb);
    load_mb_nnz(f, mbx, mby, nz);
    return bits;
}

/* Y264_RESCENSUS=1: HOW MANY TIMES each coded MB's residual is fully encoded,
 * split by call site, against the theoretical minimum of one commit per coded
 * MB. The cluster -- residual encode + trellis/RDOQ + CABAC residual -- is
 * ~16.5% of our t12
 * wall against x264's ~9.5%, and per-op is already priced at ~1.4x with its
 * volume arms refused, so the open axis is VOLUME.
 * A census counts; it never changes a decision, and the counters are only
 * touched when the env is set (default: not even loaded). */
enum { RES_SITE_P = 0, RES_SITE_BDIR, RES_SITE_B8, RES_SITE_OTHER, RES_NSITE };
static const char *res_site_name[RES_NSITE] = { "p-inter-rd", "b-direct-rd", "b-8x8-rd", "other" };
static _Atomic uint64_t g_res_calls[RES_NSITE];
static _Atomic uint64_t g_res_mbs_inter;     /* MBs that ended up CODED as inter */
static _Thread_local int g_res_site = RES_SITE_OTHER;
static int rescensus_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_RESCENSUS"); v = e ? atoi(e) : 0; }
    return v;
}
static void res_census_dump(void)
{
    uint64_t tot = 0, mbs = atomic_load(&g_res_mbs_inter);
    for (int s = 0; s < RES_NSITE; s++) tot += atomic_load(&g_res_calls[s]);
    if (!tot) return;
    fprintf(stderr, "\n=== Y264_RESCENSUS: residual-encode VOLUME ===\n");
    fprintf(stderr, "coded inter MBs: %llu   full residual encodes: %llu   "
            "per coded MB: %.2f\n", (unsigned long long)mbs,
            (unsigned long long)tot, mbs ? (double)tot / (double)mbs : 0.0);
    for (int s = 0; s < RES_NSITE; s++) {
        uint64_t n = atomic_load(&g_res_calls[s]);
        if (!n) continue;
        fprintf(stderr, "  %-12s %10llu  %5.1f%%  per-MB %.2f\n", res_site_name[s],
                (unsigned long long)n, 100.0 * (double)n / (double)tot,
                mbs ? (double)n / (double)mbs : 0.0);
    }
    fprintf(stderr, "=== end RESCENSUS ===\n");
}
static void res_census_register(void)
{
    static int done = 0;
    if (!done) { done = 1; atexit(res_census_dump); }
}

/* Y264_RESPROF=1: WHERE the time goes INSIDE one residual encode. The op
 * measures ~2.5-3x x264's while we run 35% FEWER of them, so the whole
 * remaining goal-3 median sits on this function's phases.
 * A t1 instrument (the accumulators are relaxed atomics, so an MT run sums
 * without tearing but the wall attribution only means anything serial). */
enum { RP_LUMA4 = 0, RP_LUMA8, RP_PRICE, RP_CHROMA, RP_TAIL, RP_NPHASE };
static const char *rp_name[RP_NPHASE] = { "luma4", "luma8", "price-bits", "chroma", "copy+tail" };
static _Atomic uint64_t g_rp_ns[RP_NPHASE];
static _Atomic uint64_t g_rp_calls;
static _Atomic uint64_t g_rp_pre[3];   /* [0]=pre -1 both, [1]=pre 0 4x4, [2]=pre 1 8x8 */
static _Atomic uint64_t g_rp_c4, g_rp_c8, g_rp_cn;
/* Trellis INVOCATIONS, to split "we run it more often" from "ours
 * costs more per call" against x264's quant_*_trellis counts. */
_Atomic uint64_t g_rp_tr4, g_rp_tr8;
/* Y264_TRPROF=1: where one 4x4 trellis call's time goes.
 * quant = the naive quant + all-zero scan; setup = scan-order operand build
 * (zigzag_abs, unquant/w2 rows, psy fdct); lattice = y264_cabac_trellis_4x4;
 * out = writing the chosen levels back. t1 instrument. */
static const char *tp_nm[TP_N] = { "quant+zerochk", "setup", "lattice", "writeback" };
_Atomic uint64_t g_tp_ns[TP_N], g_tp_calls;
_Atomic uint64_t g_tp_cat[8], g_tp_cat_tr[8];
/* Selective-RD sizing (Y264_RESPROF=2 with Y264_TR_PRE=0): bucket each
 * RD-decided MB by the CHEAP metric's margin m = c8*2*100/c4, and record how
 * often the metric's verdict matches RD's and how much J is at stake. If the
 * margin predicts the stake, RD only the ambiguous buckets. */
#define RS_NB 6
static const char *rs_lab[RS_NB] = { "m<90", "90-97", "97-100", "100-103", "103-110", "m>=110" };
static _Atomic uint64_t g_rs_n[RS_NB], g_rs_4win[RS_NB], g_rs_agree[RS_NB], g_rs_dj[RS_NB];
/* DEFAULT 1. 0 restores the degenerate compare byte-exactly. */
static int tr_pre_fix(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_TR_PRE_FIX"); v = e ? atoi(e) : 1; }
    return v;
}
/* DEFAULT 105: keep 8x8 as the default and flip to
 * 4x4 only where the metric is CONFIDENT (20.2% of coded inter MBs on bus).
 * The metric is asymmetric -- its 8x8 verdicts agree with true RD only
 * 27-47% of the time, its confident 4x4 verdicts 75-88% -- so this captures
 * 82% of the -1.58% RD-decide oracle for ~0.8% of wall against RD's 12-19%.
 * Swept: 103 -1.11%, 105 -1.30%, 107 -0.96%, 110 -0.88% (band median). */
static int tr_pre_bias(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_TR_PRE_BIAS"); v = e ? atoi(e) : 105; }
    return v;
}
static int trprof_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_TRPROF"); v = e ? atoi(e) : 0; }
    return v;
}
static int resprof_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_RESPROF"); v = e ? atoi(e) : 0; }
    return v;
}
/* Y264_EST_PROF: phase attribution of REAL P-inter rate-estimate calls by
 * in-place replay -- at sampled est_inter_mb_bits calls, loop the call's
 * phases (save/restore, author, emit header, tail cbp, tail luma, tail
 * chroma) thousands of times on the live MB state and print ns per phase.
 * In-situ timers cannot resolve one 125-ns call; repetition can,
 * at the price of measuring the CACHE-HOT cost -- the gap between the hot
 * full-call figure and the ~125 ns in-encoder census is memory effects.
 * Output unchanged (every iteration restores). Run at t1. Default 0 = off. */
static int est_prof_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_EST_PROF"); v = e ? atoi(e) : 0; }
    return v;
}
/* Y264_EST_SCRTRACE: per-trial (site, frame, mb, dist, j) trace for the
 * shape-3 dist-admissible screen's FIRE-RATE question: offline, replay each
 * MB tournament in call order and count est calls where dist alone >= the
 * running best j.
 * t1 only (stderr interleaves at t12). Default 0 = off. */
static int est_scrtrace_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_EST_SCRTRACE"); v = e ? atoi(e) : 0; }
    return v;
}
/* Y264_DIRECT_SCORE=1: MEASUREMENT ONLY, t1. Per B frame, the direct
 * prediction's SSD against the source summed over the macroblocks that
 * evaluate it, plus the count. Run the SAME encode twice, once per --direct
 * mode, and diff the per-frame lines: that says whether a per-frame mode
 * choice could beat the per-clip one, which is the ceiling question for an
 * auto mode. Deriving the OTHER mode in-slice is not safe -- temporal needs
 * every co-located reference resolvable in this slice's list 0, which only
 * the slice-level guard establishes -- so two passes is the honest form.
 * Touches no encoder state; output is identical on or off. */
static int direct_score_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_DIRECT_SCORE"); v = e ? atoi(e) : 0; }
    return v;
}
long y264_dscore_ssd;
long y264_dscore_n;
/* Y264_DIRECT_SCORE=2: x264's actual signal. Derive BOTH direct modes and run
 * the B-skip probe on each, counting how many macroblocks each would make
 * skippable. [0] = temporal, [1] = spatial, matching the slice flag's sense.
 * Only scored where the alternate mode is legal for this slice, which for
 * temporal means every co-located reference resolves in list 0 -- the frame
 * carries that as direct_alt_ok. */
long y264_dscore_skip[2];
/* Y264_DIRECT_WHY: [0] direct unavailable, [1] total. NON-ATOMIC, incremented
 * from the macroblock loop, so above one thread it LOSES COUNTS and its totals
 * are approximate. It reads as encoder nondeterminism if you let it: three runs
 * at t18 gave 34440 / 28045 / 26874 on the same clip, which is this counter
 * racing with itself and not the encoder disagreeing. Read it at t1, or read
 * colhash and the output md5, which are the sound signals. */
long y264_tdir_mb[2];
/* Y264_DIRECT_AUTO: run the skippability score every B frame and let the next
 * B slice take the higher one, which is x264's --direct auto. Separate from
 * Y264_DIRECT_SCORE's accumulator on purpose, so arming the instrument and
 * arming the decision cannot quietly consume each other's counts. */
int y264_mb_direct_auto_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_DIRECT_AUTO"); v = e ? atoi(e) : 0; }
    return v;
}
static int direct_why_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_DIRECT_WHY"); v = e ? atoi(e) : 0; }
    return v;
}

static void escr(int site, y264_frame_t *f, int mbx, int mby, long dist, double j,
                 double lbj)
{
    if (est_scrtrace_on())
        fprintf(stderr, "ESCR %d %d %d %ld %.0f %.0f\n",
                site, f->poc, mby * f->wmb + mbx, dist, j, lbj);
}

/* Admissible LOWER BOUND (x256) on a candidate's est bits: every nonzero
 * coefficient codes exactly one bypass sign bit = 256; ctx bins can price
 * arbitrarily near zero and contribute nothing. Reads only the counts the
 * residual coder already stored on the candidate. 4:4:4 keeps luma only
 * (weaker, still admissible). */
static long est_bits_lb(const y264_frame_t *f, const struct inter_result *ir)
{
    long nz = 0;
    if (ir->tr8) {
        nz += ir->nnz8[0] + ir->nnz8[1] + ir->nnz8[2] + ir->nnz8[3];
    } else {
        for (int b = 0; b < 16; b++) nz += ir->nnz[b];
    }
    if (f->cf_idc != 3) {
        const struct chroma_result *cr = &ir->cr;
        if (cr->cbp) {
            nz += nz_count(cr->dc_scan[0], cr->ndc);
            nz += nz_count(cr->dc_scan[1], cr->ndc);
        }
        if (cr->cbp == 2) {
            int nblk = f->cbw * f->cbh;
            for (int c2 = 0; c2 < 2; c2++)
                for (int b = 0; b < nblk; b++) nz += cr->ac_nnz[c2][b];
        }
    }
    return 256 * nz;
}
static inline uint64_t rp_now(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void rp_dump(void)
{
    uint64_t tot = 0, n = atomic_load(&g_rp_calls);
    for (int i = 0; i < RP_NPHASE; i++) tot += atomic_load(&g_rp_ns[i]);
    if (!n) return;
    fprintf(stderr, "\n=== Y264_RESPROF: inside encode_inter_res_tp ===\n");
    fprintf(stderr, "calls: %llu   attributed: %.1f ms   per call: %.0f ns\n",
            (unsigned long long)n, tot / 1e6, (double)tot / (double)n);
    for (int i = 0; i < RP_NPHASE; i++) {
        uint64_t v = atomic_load(&g_rp_ns[i]);
        fprintf(stderr, "  %-10s %8.1f ms  %5.1f%%  %6.0f ns/call\n", rp_name[i],
                v / 1e6, 100.0 * (double)v / (double)tot, (double)v / (double)n);
    }
    fprintf(stderr, "  transform pre-decision: both=%llu 4x4=%llu 8x8=%llu\n",
            (unsigned long long)atomic_load(&g_rp_pre[0]),
            (unsigned long long)atomic_load(&g_rp_pre[1]),
            (unsigned long long)atomic_load(&g_rp_pre[2]));
    { uint64_t n = atomic_load(&g_tp_calls), tot = 0;
      for (int i = 0; i < TP_N; i++) tot += atomic_load(&g_tp_ns[i]);
      if (n) {
        fprintf(stderr, "  TRPROF 4x4 trellis: %llu calls, %.1f ms, %.0f ns/call\n",
                (unsigned long long)n, tot/1e6, (double)tot/(double)n);
        for (int i = 0; i < TP_N; i++)
            fprintf(stderr, "    %-14s %7.1f ms  %5.1f%%  %6.1f ns/call\n", tp_nm[i],
                    atomic_load(&g_tp_ns[i])/1e6,
                    100.0*(double)atomic_load(&g_tp_ns[i])/(double)tot,
                    (double)atomic_load(&g_tp_ns[i])/(double)n);
        { uint64_t c = atomic_load(&y264_tl_calls);
          if (c) fprintf(stderr, "    LATTICE work: %llu calls, %.2f coef/call, %.2f node-updates/call\n",
                         (unsigned long long)c,
                         (double)atomic_load(&y264_tl_coef)/(double)c,
                         (double)atomic_load(&y264_tl_node)/(double)c); }
        fprintf(stderr, "    by cat (1=I16acY 2=lumaY 4=chromaAC): ");
        for (int c = 0; c < 8; c++) if (atomic_load(&g_tp_cat[c]))
            fprintf(stderr, "cat%d %llu/%llu(tr)  ", c,
                    (unsigned long long)atomic_load(&g_tp_cat[c]),
                    (unsigned long long)atomic_load(&g_tp_cat_tr[c]));
        fprintf(stderr, "\n");
      } }
    { uint64_t b8 = atomic_load(&g_blk8_est), b4 = atomic_load(&g_blk4_est),
               ne = atomic_load(&g_em_est);
      if (ne) fprintf(stderr, "  est BLOCK visits: %llu 8x8 + %llu 4x4 = %.2f blocks per est-estimate\n",
                      (unsigned long long)b8, (unsigned long long)b4,
                      (double)(b8+b4)/(double)ne); }
    { uint64_t eb = atomic_load(&y264_est_bins), by = atomic_load(&y264_est_bypass),
               ee2 = atomic_load(&g_em_est) + atomic_load(&g_em_real);
      if (eb && ee2) fprintf(stderr, "  est BINS: %llu ctx + %llu bypass = %.1f bins per MB-estimate\n",
                             (unsigned long long)eb, (unsigned long long)by,
                             (double)(eb+by)/(double)ee2);
      { uint64_t cf = atomic_load(&y264_est_coefs);
        if (cf && ee2) fprintf(stderr, "  est COEFS: %llu = %.2f coefficients per MB-estimate (%.2f bins/coef)\n",
                               (unsigned long long)cf, (double)cf/(double)ee2,
                               (double)(eb+by)/(double)cf);
        { uint64_t rc = atomic_load(&y264_real_coefs), rr = atomic_load(&g_em_real);
          if (rc && rr) fprintf(stderr, "  REAL coefs: %llu over %llu real emits = %.2f per coded inter MB\n",
                                (unsigned long long)rc, (unsigned long long)rr,
                                (double)rc/(double)rr); } } }
    { uint64_t er = atomic_load(&g_em_real), ee = atomic_load(&g_em_est);
      if (er || ee) fprintf(stderr, "  emit_cabac_inter_tail: real %llu   est-mode %llu (%.1fx)\n",
                            (unsigned long long)er, (unsigned long long)ee,
                            er ? (double)ee/(double)er : 0.0); }
    { uint64_t t4 = atomic_load(&g_rp_tr4), t8 = atomic_load(&g_rp_tr8);
      if (t4 || t8) fprintf(stderr, "  trellis invocations: 4x4 %llu   8x8 %llu\n",
                            (unsigned long long)t4, (unsigned long long)t8); }
    { uint64_t tn = 0; for (int b = 0; b < RS_NB; b++) tn += atomic_load(&g_rs_n[b]);
      if (tn) {
        fprintf(stderr, "  selective-RD sizing (bucket | share | 4x4 wins by RD | metric agrees | mean |dJ|/lambda)\n");
        for (int b = 0; b < RS_NB; b++) {
            uint64_t n = atomic_load(&g_rs_n[b]); if (!n) continue;
            fprintf(stderr, "    %-8s %5.1f%%   %5.1f%%   %5.1f%%   %8.1f\n", rs_lab[b],
                    100.0*(double)n/(double)tn,
                    100.0*(double)atomic_load(&g_rs_4win[b])/(double)n,
                    100.0*(double)atomic_load(&g_rs_agree[b])/(double)n,
                    (double)atomic_load(&g_rs_dj[b])/(double)n);
        }
      } }
    { uint64_t n4 = atomic_load(&g_rp_c4), n8 = atomic_load(&g_rp_c8), nn = atomic_load(&g_rp_cn);
      if (nn) fprintf(stderr, "  tr-pre metrics: mean satd(4x4 support)=%.1f  mean sa8d(8x8)=%.1f  ratio=%.3f\n",
                      (double)n4/nn, (double)n8/nn, (double)n8/(double)n4); }
    fprintf(stderr, "=== end RESPROF ===\n");
}
static void rp_register(void){ static int d = 0; if (!d) { d = 1; atexit(rp_dump); } }
#define RP_ADD(ph, t0) do { if (rp_on) { uint64_t _t = rp_now(); \
        atomic_fetch_add_explicit(&g_rp_ns[ph], _t - (t0), memory_order_relaxed); (t0) = _t; } } while (0)

/* Code the inter residual from a prepared prediction, reconstruct into rec.
 * When --transform-8x8 is on, tries the 8x8 luma transform against the 4x4 and
 * keeps the lower-J choice (ir->tr8). `allow8x8` gates it off for P_8x8 with
 * sub-partitions below 8x8 (transform_size_8x8_flag constraint). */
static void encode_inter_res_tp(y264_frame_t *f, int mbx, int mby,
                             const pixel pred[256], pixel cpred[2][256],
                             int allow8x8, struct inter_result *ir, long lam,
                             int skip_chroma, int *trpre);
static void encode_inter_res(y264_frame_t *f, int mbx, int mby,
                             const pixel pred[256], pixel cpred[2][256],
                             int allow8x8, struct inter_result *ir, long lam,
                             int skip_chroma)
{
    encode_inter_res_tp(f, mbx, mby, pred, cpred, allow8x8, ir, lam, skip_chroma, NULL);
}
static void encode_inter_res_tp(y264_frame_t *f, int mbx, int mby,
                             const pixel pred[256], pixel cpred[2][256],
                             int allow8x8, struct inter_result *ir, long lam,
                             int skip_chroma, int *trpre)
{
    if (rescensus_on()) {
        res_census_register();
        atomic_fetch_add_explicit(&g_res_calls[g_res_site], 1, memory_order_relaxed);
    }
    const int rp_on = resprof_on();
    uint64_t rp_t = 0;
    if (rp_on) { rp_register(); atomic_fetch_add_explicit(&g_rp_calls, 1, memory_order_relaxed);
                 rp_t = rp_now(); }
    int ss = f->src_stride[0], rs = f->rec_stride[0];
    const pixel *src = f->src[0] + (mby * 16) * ss + mbx * 16;
    pixel *rec = f->rec[0] + (mby * 16) * rs + mbx * 16;

    /* Transform-size pre-decision: at the medium tier
 * pick 4x4 vs 8x8 by SA8D-vs-SATD of the residual and encode ONLY that size,
 * instead of fully encoding both and comparing by RD -- the redundant second
 * transform is ~16% of the pure-C encode. subme>=9 keeps encode-both + RD pick
 * (byte-identical max-quality default). Y264_TR_PRE forces on(1)/off(-1). */
    int pre = -1;   /* -1 = encode both (RD-decide); 0 = 4x4 only; 1 = 8x8 only */
    if (f->transform8x8 && allow8x8 && tr_pre_on(f->subme) &&
        trpre && *trpre >= 0) {
        pre = *trpre;               /* shared per-MB decision (Y264_TR_PRE_SHARE) */
    } else if (f->transform8x8 && allow8x8 && tr_pre_on(f->subme)) {
        extern uint64_t y264_bp2_pre; extern int y264_bp2_on;
        uint64_t t0 = 0;
        if (y264_bp2_on) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
                           t0 = (uint64_t)ts.tv_sec*1000000000ull + ts.tv_nsec; }
        int c4 = y264_dsp.satd16x16(src, ss, pred, 16);
        int c8 = y264_dsp.sa8d16x16(src, ss, pred, 16);
        if (resprof_on()) {
            atomic_fetch_add_explicit(&g_rp_c4, (unsigned)c4, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_rp_c8, (unsigned)c8, memory_order_relaxed);
            atomic_fetch_add_explicit(&g_rp_cn, 1, memory_order_relaxed);
        }
        /* SCALE: our SATD is the RAW 4x4-Hadamard abs sum, x264's is that >>1,
 * so satd16x16 here is 2x x264's -- while sa8d_c_8x8 carries x264's own
 * ((sum+2)>>2) normalisation and is 1x. Comparing them directly makes the
 * pre-decision degenerate: 8x8 wins 81114/81114 calls on bus, i.e. the
 * 4x4 transform is unreachable at the medium tier. Y264_TR_PRE_FIX=1
 * puts both on our scale before the compare, and is the DEFAULT. Set 0
 * to recover the degenerate behaviour byte-exactly.
 *
 * Do NOT treat this screen as an approximation to be improved toward
 * full RD: measured head-to-head, encode-both-and-RD-pick is WORSE
 * than this heuristic on 5 of 7 clips (+0.28% median BD-rate). */
        /* BIAS (Y264_TR_PRE_BIAS, x100): with the scale corrected we pick 8x8
 * on 39% of coded inter MBs where x264 picks it on 24-27% (measured)
 * -- the compare still leans 8x8, so the bias
 * tightens it. 100 = the plain corrected compare. */
        pre = (tr_pre_fix()
               ? ((long)c8 * 2 * 100 < (long)c4 * tr_pre_bias())
               : (c8 < c4)) ? 1 : 0;
        if (y264_bp2_on) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
                           y264_bp2_pre += (uint64_t)ts.tv_sec*1000000000ull + ts.tv_nsec - t0; }
        if (trpre) *trpre = pre;    /* first trial decides for the MB */
    }

    pixel rec4[256], rec8[256];
    int tr8 = 0, cbp = 0, cbp4 = 0;
    long j4 = 0;
    const pixel *win = rec4;
    if (rp_on) atomic_fetch_add_explicit(&g_rp_pre[pre + 1], 1, memory_order_relaxed);
    RP_ADD(RP_TAIL, rp_t);                       /* tr-pre decision + setup */
    if (pre != 1) {                              /* 4x4 (unless SA8D chose 8x8) */
        long dist4 = inter_res_4x4_plane(f, mbx, mby, 0, src, ss, pred, ir->lev, ir->nnz, rec4);
        RP_ADD(RP_LUMA4, rp_t);
        for (int i8 = 0; i8 < 4; i8++)
            for (int i4 = 0; i4 < 4; i4++)
                if (ir->nnz[i8 * 4 + i4]) cbp4 |= (1 << i8);
        /* Only PRICE the residual when the transform size is RD-decided (pre==-1).
 * When the SA8D pre-decision forced the size (pre 0/1, medium tier), j4/j8
 * are never compared -- inter_luma_bits (a side-effect-free CAVLC scratch
 * code, ~2% of pure-C, mispriced under CABAC anyway) would be computed and
 * discarded. Skipping it is byte-identical. */
        if (pre == -1) { j4 = dist4 + Y264_LAMJ(lam, inter_luma_bits(f, mbx, mby, ir, 0, cbp4));
                         RP_ADD(RP_PRICE, rp_t); }
        win = rec4; cbp = cbp4;
    }
    if (f->transform8x8 && allow8x8 && pre != 0) {   /* 8x8 (unless SA8D chose 4x4) */
        long dist8 = inter_res_8x8(f, src, ss, pred, ir->lev8, ir->nnz8, rec8);
        RP_ADD(RP_LUMA8, rp_t);
        int cbp8 = 0;
        for (int q = 0; q < 4; q++) if (ir->nnz8[q]) cbp8 |= (1 << q);
        if (pre == 1) {                              /* forced 8x8: no pricing needed */
            win = rec8; tr8 = 1; cbp = cbp8;
        } else {                                     /* pre==-1: RD-decide 4x4 vs 8x8 */
            long j8 = dist8 + Y264_LAMJ(lam, inter_luma_bits(f, mbx, mby, ir, 1, cbp8));
            RP_ADD(RP_PRICE, rp_t);
            if (rp_on >= 2) {           /* selective-RD sizing, probe only */
                int pc4 = y264_dsp.satd16x16(src, ss, pred, 16);
                int pc8 = y264_dsp.sa8d16x16(src, ss, pred, 16);
                int m = pc4 ? (int)((long)pc8 * 2 * 100 / pc4) : 100;
                int b = m < 90 ? 0 : m < 97 ? 1 : m < 100 ? 2 : m < 103 ? 3 : m < 110 ? 4 : 5;
                int rd_wants_8x8 = (cbp8 && j8 < j4);
                int metric_says_8x8 = (m < 100);
                atomic_fetch_add_explicit(&g_rs_n[b], 1, memory_order_relaxed);
                if (!rd_wants_8x8) atomic_fetch_add_explicit(&g_rs_4win[b], 1, memory_order_relaxed);
                if (rd_wants_8x8 == metric_says_8x8)
                    atomic_fetch_add_explicit(&g_rs_agree[b], 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_rs_dj[b],
                    (uint64_t)(labs(j4 - j8) / (lam > 0 ? lam : 1)), memory_order_relaxed);
            }
            if (cbp8 && j8 < j4) { win = rec8; tr8 = 1; cbp = cbp8; }
        }
    }
    for (int y = 0; y < 16; y++)
        memcpy(rec + y * rs, win + y * 16, 16 * sizeof(pixel));

    /* transform_size_8x8_flag is only coded when CodedBlockPatternLuma>0. With no
 * luma residual the reconstruction is the prediction for either transform, so
 * 4x4 semantics; otherwise the decoder (inferring flag 0) would deblock the
 * internal edges while we skipped them. */
    ir->tr8 = cbp ? tr8 : 0;
    ir->cbp_luma = cbp;

    /* A1(a): for the qpel-RD nudge trials, hold chroma fixed at the base (leave
 * ir->cr and f->rec chroma untouched) -- a +/-1/4-pel luma nudge barely moves
 * the chroma prediction, and the winner gets a full re-encode. The held chroma
 * terms are constant across trials, so the trial J stays comparable to base_j. */
    RP_ADD(RP_TAIL, rp_t);
    if (skip_chroma)
        return;

    if (f->cf_idc == 3) {
        /* 4:4:4: Cb/Cr coded like luma inter (16 4x4 blocks from the luma-6-tap
 * chroma prediction). Shared cbp = luma | Cb | Cr per 8x8. */
        for (int comp = 0; comp < 2; comp++) {
            int p = 1 + comp, css = f->src_stride[p], crs = f->rec_stride[p];
            const pixel *csrc = f->src[p] + (mby * 16) * css + mbx * 16;
            pixel *crec = f->rec[p] + (mby * 16) * crs + mbx * 16;
            pixel crec4[256];
            inter_res_4x4_plane(f, mbx, mby, p, csrc, css, cpred[comp],
                                ir->cr_c[comp].lev, ir->cr_c[comp].nnz, crec4);
            int c = 0;
            for (int i8 = 0; i8 < 4; i8++)
                for (int i4 = 0; i4 < 4; i4++)
                    if (ir->cr_c[comp].nnz[i8 * 4 + i4]) c |= (1 << i8);
            ir->cr_c[comp].cbp_luma = c;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) crec[y*crs + x] = crec4[y*16 + x];
        }
        ir->cbp444 = cbp | ir->cr_c[0].cbp_luma | ir->cr_c[1].cbp_luma;
    } else {
        encode_chroma_inter(f, mbx, mby, cpred, &ir->cr);
    }
    RP_ADD(RP_CHROMA, rp_t);
}

/* Encode a partitioned inter macroblock: build prediction then code residual.
 * For P_8x8 (part 3) `psub` carries the four sub_mb_types and mvx/mvy index
 * b*4+s; the 8x8 transform is only legal when every sub-partition is 8x8. */
static void encode_inter_mb(y264_frame_t *f, int mbx, int mby, int part,
                            const int *mvx, const int *mvy, const int *pref,
                            const int *psub, struct inter_result *ir, long lam,
                            int skip_chroma)
{
    pixel pred[256], cpred[2][256];
    build_inter_pred(f, mbx, mby, part, mvx, mvy, pref, psub, pred,
                     skip_chroma ? NULL : cpred);
    int allow8x8 = 1;
    if (part == 3)
        for (int b = 0; b < 4; b++) {
            ir->sub[b] = psub[b];
            if (psub[b]) allow8x8 = 0;
        }
    encode_inter_res(f, mbx, mby, pred, cpred, allow8x8, ir, lam, skip_chroma);
    ir->part = part;
    int nmv = part == 3 ? 16 : (part ? 2 : 1);
    for (int p = 0; p < nmv; p++) { ir->mvx[p] = mvx[p]; ir->mvy[p] = mvy[p]; }
}

/* THE ROW WIDTH HAS TO BE A COMPILE-TIME CONSTANT HERE.
 *
 * `w` is `16 / f->sub_w`, a runtime value, so `memcpy(.., w * sizeof(pixel))`
 * compiles to a CALL to _platform_memmove for every row -- thirty-two calls per
 * snapshot at 4:2:0, each moving eight or sixteen bytes, where the call costs
 * more than the copy. These two functions run once per RD candidate, and
 * `_platform_memmove` measures **3.02% of the shipped wall** at samsung
 * 1200 kbps, the third-largest uncovered symbol in the whole profile.
 *
 * w is only ever 8 or 16 -- luma is always 16, chroma is 16/sub_w -- so
 * splitting on it hands clang a constant length and it inlines the copy to
 * vector load/stores. Byte-identical: same bytes, same order, same buffer. */
#define MB_REC_ROWS(COPY, W)                                                   \
    do { for (int y = 0; y < h; y++) { COPY; p += (W); } } while (0)

static void save_mb_rec(y264_frame_t *f, int mbx, int mby, pixel *buf)
{
    STG_BEG(STG_SNAP);
    pixel *p = buf;
    for (int c = 0; c < 3; c++) {
        int w = c ? 16 / f->sub_w : 16, h = c ? 16 / f->sub_h : 16, rs = f->rec_stride[c];
        const pixel *rec = f->rec[c] + (mby * h) * rs + mbx * w;
        if (w == 16)
            MB_REC_ROWS(memcpy(p, rec + y * rs, 16 * sizeof(pixel)), 16);
        else if (w == 8)
            MB_REC_ROWS(memcpy(p, rec + y * rs, 8 * sizeof(pixel)), 8);
        else
            MB_REC_ROWS(memcpy(p, rec + y * rs, (size_t)w * sizeof(pixel)), w);
    }
    STG_END();
}

static void load_mb_rec(y264_frame_t *f, int mbx, int mby, const pixel *buf)
{
    STG_BEG(STG_SNAP);
    const pixel *p = buf;
    for (int c = 0; c < 3; c++) {
        int w = c ? 16 / f->sub_w : 16, h = c ? 16 / f->sub_h : 16, rs = f->rec_stride[c];
        pixel *rec = f->rec[c] + (mby * h) * rs + mbx * w;
        if (w == 16)
            MB_REC_ROWS(memcpy(rec + y * rs, p, 16 * sizeof(pixel)), 16);
        else if (w == 8)
            MB_REC_ROWS(memcpy(rec + y * rs, p, 8 * sizeof(pixel)), 8);
        else
            MB_REC_ROWS(memcpy(rec + y * rs, p, (size_t)w * sizeof(pixel)), w);
    }
    STG_END();
}

/* Set all nnz grid cells of this macroblock to a value (0 for skipped MBs). */
static void clear_mb_nnz(y264_frame_t *f, int mbx, int mby)
{
    static const int8_t Z[4] = { 0, 0, 0, 0 };
    int k = 0; (void)k;
    MB_NNZ_LUMA(memcpy(g_ + by * s_, Z, 4));
    if (f->cbw == 2) MB_NNZ_CHROMA(2, memcpy(g_ + by * s_, Z, 2));
    else             MB_NNZ_CHROMA(4, memcpy(g_ + by * s_, Z, 4));
}

/* cbp + luma/chroma residual, shared by P and B inter macroblocks. */
/* transform_size_8x8_flag presence (7.3.5): CodedBlockPatternLuma > 0 and, for
 * P_8x8, no sub-partition below 8x8 (noSubMbPartSizeLessThan8x8Flag). */
static int tr8_flag_present(y264_frame_t *f, const struct inter_result *ir)
{
    if (!f->transform8x8 || ir->cbp_luma <= 0)
        return 0;
    if (ir->part == 3)
        for (int b = 0; b < 4; b++)
            if (ir->sub[b])
                return 0;
    return 1;
}

/* Author the inter luma nnz grid (all three cases: 4:4:4 shared-cbp, tr8, 4x4). */
static void author_inter_luma_nnz(y264_frame_t *f, int mbx, int mby,
                                  const struct inter_result *ir)
{
    int lstride = f->nnz_stride[0];
    int8_t *lnnz = f->nnz[0];
    int bx0 = mbx * 4, by0 = mby * 4;

    if (f->cf_idc == 3) {                          /* 4:4:4 luma, shared cbp444 */
        int cbp = ir->cbp444;
        for (int i8 = 0; i8 < 4; i8++)
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                lnnz[ay * lstride + ax] =
                    (cbp & (1 << i8)) ? (int8_t)nz_count(ir->lev[blk], 16) : 0;
            }
        return;
    }
    if (ir->tr8) {
        for (int blk = 0; blk < 4; blk++) {
            if (ir->cbp_luma & (1 << blk))
                author_luma8x8_nnz(f, mbx, mby, blk, ir->lev8[blk]);
            else
                for (int j = 0; j < 4; j++) {
                    int lb = blk * 4 + j;
                    lnnz[(by0 + BLK_Y[lb]) * lstride + (bx0 + BLK_X[lb])] = 0;
                }
        }
    } else {
        for (int i8 = 0; i8 < 4; i8++)
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                lnnz[ay * lstride + ax] =
                    (ir->cbp_luma & (1 << i8)) ? (int8_t)nz_count(ir->lev[blk], 16) : 0;
            }
    }
}

/* W0 step 4 primitive: author all inter residual nnz grids (luma + chroma) from
 * the decided result, with no bitstream. Pass 1 calls this; pass 2 emits. */
static void author_inter_residual(y264_frame_t *f, int mbx, int mby,
                                  const struct inter_result *ir)
{
    author_inter_luma_nnz(f, mbx, mby, ir);
    if (f->cf_idc == 3) {
        author_c444_comp_nnz(f, mbx, mby, 0, 1, ir->cbp444, NULL, &ir->cr_c[0]);
        author_c444_comp_nnz(f, mbx, mby, 1, 1, ir->cbp444, NULL, &ir->cr_c[1]);
    } else {
        author_chroma_residual_nnz(f, mbx, mby, &ir->cr);
    }
}

/* Emit inter residual bits, reading nnz from the (already authored) grid. */
static void emit_inter_residual(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                                const struct inter_result *ir)
{
    int lstride = f->nnz_stride[0];
    int8_t *lnnz = f->nnz[0];
    int bx0 = mbx * 4, by0 = mby * 4;

    if (f->cf_idc == 3) {                          /* 4:4:4: Cb/Cr like luma, shared cbp */
        int cbp = ir->cbp444;
        y264_bs_write_ue(bs, cbp444_to_codenum(cbp, 1));
        if (cbp > 0) qpd_cavlc(bs, f, f->cur_qp);
        for (int i8 = 0; i8 < 4; i8++)
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                if (cbp & (1 << i8)) {
                    int nc = derive_nc(lnnz, lstride, ax, ay);
                    y264_cavlc_residual(bs, ir->lev[blk], 16, nc);
                }
            }
        emit_c444_comp_cavlc(bs, f, mbx, mby, 0, 1, cbp, NULL, &ir->cr_c[0]);
        emit_c444_comp_cavlc(bs, f, mbx, mby, 1, 1, cbp, NULL, &ir->cr_c[1]);
        return;
    }

    int cbp = ir->cbp_luma | (ir->cr.cbp << 4);
    y264_bs_write_ue(bs, cbp_inter_to_codenum(cbp));
    /* transform_size_8x8_flag (after cbp, before mb_qp_delta). Present when
 * CodedBlockPatternLuma>0, except that a P_8x8 with any sub-partition below
 * 8x8 carries no flag at all (7.3.5: noSubMbPartSizeLessThan8x8Flag). */
    if (tr8_flag_present(f, ir))
        y264_bs_write1(bs, ir->tr8);
    if (cbp > 0)
        qpd_cavlc(bs, f, f->cur_qp);            /* mb_qp_delta */

    if (ir->tr8) {
        for (int blk = 0; blk < 4; blk++)
            if (ir->cbp_luma & (1 << blk))
                emit_luma8x8_residual_cavlc(bs, f, mbx, mby, blk, ir->lev8[blk]);
    } else {
        for (int i8 = 0; i8 < 4; i8++)
            for (int i4 = 0; i4 < 4; i4++) {
                int blk = i8 * 4 + i4;
                int ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                if (ir->cbp_luma & (1 << i8)) {
                    int nc = derive_nc(lnnz, lstride, ax, ay);
                    y264_cavlc_residual(bs, ir->lev[blk], 16, nc);
                }
            }
    }
    emit_chroma_residual(bs, f, mbx, mby, &ir->cr);
}

/* Wrapper: author grids then emit, for the current single-pass caller. */
static void write_inter_residual(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                                 const struct inter_result *ir)
{
    author_inter_residual(f, mbx, mby, ir);
    emit_inter_residual(bs, f, mbx, mby, ir);
}

/* ref_idx_l0 as te(v): a single flag (!ref) when the active range is 1, else ue. */
static void write_ref_idx_cavlc(y264_bs_t *bs, int ref, int active)
{
    if (active <= 1) return;
    if (active == 2) y264_bs_write1(bs, ref == 0 ? 1 : 0);
    else y264_bs_write_ue(bs, ref);
}

/* Emit an inter MB's bits (mb_type, ref_idx, mvd, residual). Reads the nnz grid
 * authored earlier; motion is authored by commit_inter_motion (not here). */
static void emit_inter_mb(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                          const struct inter_result *ir)
{
    /* mb_type: P_L0_16x16=0, P_L0_L0_16x8=1, P_L0_L0_8x16=2, P_8x8=3. */
    y264_bs_write_ue(bs, ir->part);
    int nparts = ir->part == 3 ? 4 : (ir->part ? 2 : 1);
    if (ir->part == 3)
        for (int b = 0; b < 4; b++)
            y264_bs_write_ue(bs, ir->sub[b]);   /* sub_mb_type (7.4.5.2 P codes) */
    /* ref_idx_l0 per partition (H.264 orders all ref_idx before all mvd). */
    for (int p = 0; p < nparts; p++)
        write_ref_idx_cavlc(bs, ir->ref[p], f->nref);
    if (ir->part == 3) {
        for (int b = 0; b < 4; b++)
            for (int s2 = 0; s2 < SUB_NS[ir->sub[b]]; s2++) {
                y264_bs_write_se(bs, ir->mvx[b * 4 + s2] - ir->pmvx[b * 4 + s2]);
                y264_bs_write_se(bs, ir->mvy[b * 4 + s2] - ir->pmvy[b * 4 + s2]);
            }
    } else {
        for (int p = 0; p < nparts; p++) {
            y264_bs_write_se(bs, ir->mvx[p] - ir->pmvx[p]);   /* mvd_l0 x */
            y264_bs_write_se(bs, ir->mvy[p] - ir->pmvy[p]);   /* mvd_l0 y */
        }
    }
    emit_inter_residual(bs, f, mbx, mby, ir);
}

/* Wrapper: author residual grids then emit, for the current single-pass caller. */
static void write_inter_mb(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                           const struct inter_result *ir)
{
    author_inter_residual(f, mbx, mby, ir);
    emit_inter_mb(bs, f, mbx, mby, ir);
}

/* B macroblock: B_L0_16x16=1, B_L1_16x16=2, B_Bi_16x16=3, ref_idx_l0 when the
 * list-0 range allows a choice (list 1 stays single-ref), one mvd per used list
 * (L0 before L1), then the shared inter residual. */
/* Two-partition B mb_type codes, uniform modes only (both partitions L0, both
 * L1, or both Bi). Verified against x264 cavlc.c <reference-internal> and
 * cabac.c i_mb_bits: golomb {L0,L1,Bi} x {16x8,8x16} and the CABAC packed-bit
 * suffixes (LSB-first, sentinel-1 terminated) for the same. */
static const uint8_t B_PART_GOLOMB[3][2] = { {4, 5}, {6, 7}, {20, 21} };
static const uint8_t B_PART_CBITS[3][2]  = { {0x31, 0x29}, {0x39, 0x25}, {0x47, 0x67} };

static void emit_b_mb(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                      const struct inter_result *ir)
{
    int useL0 = (ir->bmode == 0 || ir->bmode == 2);
    int useL1 = (ir->bmode == 1 || ir->bmode == 2);
    if (ir->bpart == 3) {                       /* B_8x8: four sub_mb_types */
        y264_bs_write_ue(bs, 22);
        for (int b = 0; b < 4; b++)
            y264_bs_write_ue(bs, ir->b8m[b]);   /* 7.4.5.2: our numbering IS theirs */
        for (int b = 0; b < 4; b++)             /* ref_idx_l0, coded sub-mbs only */
            if (ir->b8m[b] == 1 || ir->b8m[b] == 3)
                write_ref_idx_cavlc(bs, ir->ref[b], f->nref);
        /* ref_idx_l1: single active reference, never coded. */
        for (int b = 0; b < 4; b++)
            if (ir->b8m[b] == 1 || ir->b8m[b] == 3) {
                y264_bs_write_se(bs, ir->mvx[b] - ir->pmvx[b]);
                y264_bs_write_se(bs, ir->mvy[b] - ir->pmvy[b]);
            }
        for (int b = 0; b < 4; b++)
            if (ir->b8m[b] == 2 || ir->b8m[b] == 3) {
                y264_bs_write_se(bs, ir->mvx[4 + b] - ir->pmvx[4 + b]);
                y264_bs_write_se(bs, ir->mvy[4 + b] - ir->pmvy[4 + b]);
            }
        emit_inter_residual(bs, f, mbx, mby, ir);
        return;
    }
    if (ir->bpart) {                            /* two-partition uniform modes */
        y264_bs_write_ue(bs, B_PART_GOLOMB[ir->bmode][ir->bpart - 1]);
        if (useL0)
            for (int p = 0; p < 2; p++)
                write_ref_idx_cavlc(bs, ir->ref[p], f->nref);
        /* ref_idx_l1: single active reference, never coded. */
        if (useL0)
            for (int p = 0; p < 2; p++) {
                y264_bs_write_se(bs, ir->mvx[p] - ir->pmvx[p]);
                y264_bs_write_se(bs, ir->mvy[p] - ir->pmvy[p]);
            }
        if (useL1)
            for (int p = 0; p < 2; p++) {
                y264_bs_write_se(bs, ir->mvx[2 + p] - ir->pmvx[2 + p]);
                y264_bs_write_se(bs, ir->mvy[2 + p] - ir->pmvy[2 + p]);
            }
        emit_inter_residual(bs, f, mbx, mby, ir);
        return;
    }
    y264_bs_write_ue(bs, ir->bmode + 1);        /* 0->1(L0), 1->2(L1), 2->3(Bi) */
    if (useL0)                                  /* ref_idx_l0 */
        write_ref_idx_cavlc(bs, ir->ref[0], f->nref);
    if (useL0) {                                /* mvd_l0 */
        y264_bs_write_se(bs, ir->mvx[0] - ir->pmvx[0]);
        y264_bs_write_se(bs, ir->mvy[0] - ir->pmvy[0]);
    }
    if (useL1) {                                /* mvd_l1 */
        y264_bs_write_se(bs, ir->mvx[1] - ir->pmvx[1]);
        y264_bs_write_se(bs, ir->mvy[1] - ir->pmvy[1]);
    }
    emit_inter_residual(bs, f, mbx, mby, ir);
}

/* Wrapper: author residual grids then emit, for the single-pass B trial path. */
static void write_b_mb(y264_bs_t *bs, y264_frame_t *f, int mbx, int mby,
                       const struct inter_result *ir)
{
    author_inter_residual(f, mbx, mby, ir);
    emit_b_mb(bs, f, mbx, mby, ir);
}

/* A2/B-bidir: the L0 and L1 unipred blocks the 16x16 tournament builds for
 * bmode 0 / bmode 1. Bi (bmode 2) is exactly their weighted average -- same ref,
 * same MV -- so caching them lets Bi skip its two MC passes (byte-identical). */
struct bpred_cache { pixel l[2][256]; pixel c[2][2][256]; };

/* Build a B prediction (bmode 0=L0, 1=L1, 2=Bi averaged) into pred/cpred.
 * `l0ref` selects the list-0 reference (and its implicit bipred weights). When
 * `bc` is non-NULL, bmode 0/1 stash their unipred block and bmode 2 averages the
 * stashed blocks instead of re-running motion compensation. */
static void build_bpred(y264_frame_t *f, int mbx, int mby, int bmode, int l0ref,
                        int mvL0x, int mvL0y, int mvL1x, int mvL1y,
                        pixel pred[256], pixel cpred[2][256], struct bpred_cache *bc)
{
    int pw = f->padded_w, ph = f->padded_h, lx = mbx * 16, ly = mby * 16;
    int c444 = f->cf_idc == 3;
    /* chroma geometry, format-aware: 4:2:0 = 8x8, 4:2:2 = 8x16 (sub_h=1). */
    int sw = f->sub_w, sh = f->sub_h;
    int cw = 16 / sw, chh = 16 / sh, cpw = pw / sw, cph = ph / sh;
    int cx = mbx * cw, cy = mby * chh, cn = cw * chh;
    if (bmode != 2) {
        const pixel *const *ref = bmode ? f->ref1 : f->refs[l0ref];
        const int *rs = bmode ? f->ref1_stride : f->ref_stride;
        int vx = bmode ? mvL1x : mvL0x, vy = bmode ? mvL1y : mvL0y;
        y264_me_mc_luma(pred, ref[0], rs[0], pw, ph, lx, ly, vx, vy, 16, 16);
        for (int c = 0; c < 2; c++)
            if (c444)   /* 4:4:4: chroma = luma 6-tap, full-res */
                y264_mc_luma_b(cpred[c], 16, ref[1 + c], rs[1 + c], pw, ph, lx, ly, vx, vy, 16, 16, Y264_CHROMA_BORDER);
            else
                y264_mc_chroma(cpred[c], cw, ref[1 + c], rs[1 + c], cpw, cph,
                               cx, cy, vx, vy, cw, chh, sw, sh);
        if (bc) {           /* stash this unipred block for the Bi average */
            memcpy(bc->l[bmode], pred, 256 * sizeof(pixel));
            for (int c = 0; c < 2; c++) memcpy(bc->c[bmode][c], cpred[c], 256 * sizeof(pixel));
        }
    } else if (bc) {        /* Bi = weighted average of the cached L0 / L1 blocks */
        int w0, w1; bipred_weights(f, l0ref, &w0, &w1);
        bipred_avg(pred, bc->l[0], bc->l[1], 256, w0, w1);
        for (int c = 0; c < 2; c++)
            bipred_avg(cpred[c], bc->c[0][c], bc->c[1][c], c444 ? 256 : cn, w0, w1);
    } else {
        int w0, w1; bipred_weights(f, l0ref, &w0, &w1);
        const pixel *const *r0 = f->refs[l0ref];
        pixel p0[256], p1[256], c0[2][256], c1[2][256];
        y264_me_mc_luma(p0, r0[0], f->ref_stride[0], pw, ph, lx, ly, mvL0x, mvL0y, 16, 16);
        y264_me_mc_luma(p1, f->ref1[0], f->ref1_stride[0], pw, ph, lx, ly, mvL1x, mvL1y, 16, 16);
        bipred_avg(pred, p0, p1, 256, w0, w1);
        for (int c = 0; c < 2; c++) {
            if (c444) {
                y264_mc_luma_b(c0[c], 16, r0[1 + c], f->ref_stride[1 + c], pw, ph, lx, ly, mvL0x, mvL0y, 16, 16, Y264_CHROMA_BORDER);
                y264_mc_luma_b(c1[c], 16, f->ref1[1 + c], f->ref1_stride[1 + c], pw, ph, lx, ly, mvL1x, mvL1y, 16, 16, Y264_CHROMA_BORDER);
                bipred_avg(cpred[c], c0[c], c1[c], 256, w0, w1);
            } else {
                y264_mc_chroma(c0[c], cw, r0[1 + c], f->ref_stride[1 + c], cpw, cph, cx, cy, mvL0x, mvL0y, cw, chh, sw, sh);
                y264_mc_chroma(c1[c], cw, f->ref1[1 + c], f->ref1_stride[1 + c], cpw, cph, cx, cy, mvL1x, mvL1y, cw, chh, sw, sh);
                bipred_avg(cpred[c], c0[c], c1[c], cn, w0, w1);
            }
        }
    }
}

/* Commit a B macroblock's per-list motion into the L0/L1 fields. */
static void commit_b_motion(y264_frame_t *f, int mbx, int mby, const struct inter_result *ir)
{
    int ms = f->i4mode_stride;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            f->i4mode[(mby * 4 + by) * ms + (mbx * 4 + bx)] = 2;
    int useL0 = (ir->bmode == 0 || ir->bmode == 2);
    int useL1 = (ir->bmode == 1 || ir->bmode == 2);
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++) {
            int i = (mby * 4 + by) * f->mv_stride + (mbx * 4 + bx);
            f->mvx[i]  = (int16_t)(useL0 ? ir->mvx[0] : 0);
            f->mvy[i]  = (int16_t)(useL0 ? ir->mvy[0] : 0);
            f->refidx[i]  = useL0 ? (int8_t)ir->ref[0] : -1;
            f->mvx1[i] = (int16_t)(useL1 ? ir->mvx[1] : 0);
            f->mvy1[i] = (int16_t)(useL1 ? ir->mvy[1] : 0);
            f->refidx1[i] = useL1 ? 0 : -1;
        }
}

/* Geometry of partition p of a partition mode: pixel rect (rx,ry,rw,rh) and its
 * top-left / size in 4x4 units (bx4,by4,w4,h4). */
static void part_rect(int mbx, int mby, int part, int p,
                      int *rx, int *ry, int *rw, int *rh,
                      int *bx4, int *by4, int *w4, int *h4)
{
    int lx = mbx * 16, ly = mby * 16, bx0 = mbx * 4, by0 = mby * 4;
    if (part == 0) {
        *rx = lx; *ry = ly; *rw = 16; *rh = 16; *bx4 = bx0; *by4 = by0; *w4 = 4; *h4 = 4;
    } else if (part == 1) {                 /* 16x8 */
        *rw = 16; *rh = 8; *w4 = 4; *h4 = 2;
        *rx = lx; *bx4 = bx0;
        *ry = ly + p * 8; *by4 = by0 + p * 2;
    } else {                                /* 8x16 */
        *rw = 8; *rh = 16; *w4 = 2; *h4 = 4;
        *ry = ly; *by4 = by0;
        *rx = lx + p * 8; *bx4 = bx0 + p * 2;
    }
}

/* Bits to code ref_idx_l0 = r with num_ref_idx_l0_active = active (te(v)): one bit
 * when the range is 1, else the ue(v) length of r. */
static int ref_bits(int r, int active)
{
    if (active <= 1) return 0;
    if (active == 2) return 1;
    int b = 1, v = r + 1;
    while (v >>= 1) b += 2;
    return b;
}

static long est_inter_mb_bits(y264_frame_t *f, int mbx, int mby,
                              const struct inter_result *ir);
static long est_intra_mb_bits(y264_frame_t *f, int mbx, int mby,
                              const struct intra_mb *o, int slice);
static long est_b_bits(y264_frame_t *f, int mbx, int mby,
                       const struct inter_result *ir, int direct);
static long est_b_skip_bits(y264_frame_t *f, int mbx, int mby);

struct p_mb;
struct b_rec;
/* W0 step 6b: advance the private est_ctx by est-coding a decided MB's exact bin
 * sequence (mb_skip_flag + residual) from est_ctx, leaving the real engine ctx +
 * qp chain + grids untouched. Together with a per-row est_ctx re-init this
 * decouples the RD estimator from the serial arithmetic engine. */
static long est_commit_p(y264_frame_t *f, int mbx, int mby, const struct p_mb *r);
static long est_commit_b(y264_frame_t *f, int mbx, int mby, const struct b_rec *r);

/* est-vs-real self-check: Y264_EST_CHECK prints the est-total / actual-frame-bit
 * ratio per CABAC frame; ~1.0 confirms the est model tracks the arithmetic coder. */
static int est_check_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_EST_CHECK"); v = e ? atoi(e) : 0; }
    return v;
}

/* Y264_UNSAFE_NO_EMIT=1: skip pass 2 entirely -- the frame is analyzed and
 * reconstructed exactly as always, and then its slice_data is never written.
 * The output bitstream is INVALID BY CONSTRUCTION (header-only NALs), which is
 * the point: it prices the ceiling of every possible emission-overlap scheme by
 * deleting the work outright, the same trick Y264_UNSAFE_NO_PREVPWAIT plays on
 * a wait. Nothing here may ever be shipped on. Safe as a probe only because
 * emit is a pure sink under CRF: no decision, no recon and no analyze input
 * reads a coded size, so the arm does the identical work minus the emission
 * (gated by the recon-identity check in bench/drain/probe.py). */
static int unsafe_no_emit(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_UNSAFE_NO_EMIT"); v = e ? atoi(e) : 0; }
    return v;
}

/* est_ctx maintenance mode: 0 = track the live ctx (byte-identical to pre-6b);
 * 1 = slice-init every row; 2 = WPP-init from MB (1, r-1). Default 2 (WPP): the
 * RD estimator is row-private (independent of the serial arithmetic engine, so W1
 * can parallelize rows), measured BD-neutral vs mode 0 (avg ~-0.07% VMAF-NEG on
 * the 7-clip CIF gate). Y264_EST_CTX=0 restores the byte-identical estimator. */
static int est_ctx_mode(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_EST_CTX"); v = e ? atoi(e) : 2; }
    return v;
}

/* I-slice CABAC pass 1 on the wavefront, with the row-private WPP estimator P
 * and B use. DEFAULT ON: run serially this is ~26 ms per I frame at 720p with
 * every worker asleep. It moves I-slice bits (the RD estimator becomes
 * row-private, and mb_qp_delta prices against the QP map's predecessor rather
 * than the true chain -- the two approximations P and B already ship, both
 * BD-gated there). Y264_ICB_WF=0 restores the serial live-ctx loop. */
static int icb_wf_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ICB_WF"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}

/* CABAC-accurate RD rate is on by default for CABAC streams; Y264_CABAC_RD=0
 * disables it (A/B testing), reverting to the CAVLC bit-count proxy. */
static int cabac_rd_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_CABAC_RD"); v = e ? atoi(e) : 1; }
    return v;
}

/* REFUTED middle path for probe-admitted MBs (kept env-gated for
 * re-tuning): RD-compare skip vs inter16-coded-at-skip-MV before full
 * analysis, commit skip when it wins the 2-way. Measured on samsung_720p CRF30
 * pure-C 1t: 66.5% of admitted MBs commit, but only ~2% wall-clock comes back
 * (the trial itself is a full RD encode) and quality breaks (-1.6 VMAF at +2%
 * bits -- the committed set includes MBs whose SEARCHED MV would beat skip).
 * Superseded by the trellis-aligned probe (probe_signif_rdoq below), which
 * recovers ~3x more wall-clock with BD intact. Y264_MIDSKIP=1 enables;
 * Y264_MIDSKIP_MARGIN biases the 2-way (lambda-bits; >0 skips more). */
static int midskip_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_MIDSKIP"); v = e ? atoi(e) : 0; }
    return v;
}
static long midskip_margin(void)
{
    static int init = 0;
    static long v = 0;
    if (!init) { const char *e = getenv("Y264_MIDSKIP_MARGIN"); v = e ? atol(e) : 0; init = 1; }
    return v;
}

/* Y264_P_SKIP_EXIT: the P-side late-skip class. 35% of P_Skip verdicts fail
 * the zero-residual early probe, run the full tournament and end skip by RD
 * anyway; x264's
 * equivalent MBs never exist (probe-commit or coded inter). Two bracketing
 * exits: 1 = post-RD -- when skip already beats the inter candidate's RD,
 * commit without the intra trial (forgoes only an intra win over a winning
 * skip). 2 = post-16x16-SATD -- compare the skip candidate's own SATD (no
 * bits) against the searched 16x16 cost, x264's B-exit comparison shape at
 * their P-exit placement; bypasses partitions, RD and intra. Not the refuted
 * midskip: no extra RD trial, and the searched MV competes first. */
static int pskip_exit_mode(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_P_SKIP_EXIT"); v = e ? atoi(e) : 0; }
    return v;
}

/* A1(b): admission gate for the 16x16 "insurance" full-RD on the subme<=8 path.
 * x264's subme-7 model only spends that extra RD when the 16x16 SATD is still
 * within 5/4 of the winning shape's SATD; a much-worse 16x16 won't win the RD
 * either, so skip it. OFF by default (byte-identical to HEAD) -- a fast-preset
 * speed knob, not a default flip: measured ~2-3% faster (both scalar and NEON)
 * but only VMAF-NEG-neutral with per-clip scatter (foreman +0.50 / tempete +0.55
 * / akiyo -0.57 / mobile -0.63, mean ~-0.1%), so the small speed gain doesn't
 * justify the ~+0.5% regressions on a max-quality default. Y264_RD_ADMIT=1 enables
 * it; Y264_RD_ADMIT_MARGIN sets the numerator over 4 (default 5 = the 5/4 gate). */
static int rd_admit_16(long satd16, long winner)
{
    static int on = -1, num = 5;
    if (on < 0) {
        const char *e = getenv("Y264_RD_ADMIT"); on = e ? atoi(e) : 0;
        const char *m = getenv("Y264_RD_ADMIT_MARGIN"); if (m) num = atoi(m);
    }
    if (!on) return 1;
    return satd16 * 4 <= winner * num;      /* 16x16 within num/4 of the winner */
}

/* x264's early-termination flag partition gate --
 * KILLED, default OFF, kept env-gated as a documented negative + a
 * reproducer (like Y264_ADME / midskip). x264 medium (subme 7 < 10 =>
 * the early-termination flag) runs p16x16 and p8x8, then runs 16x8/8x16 ONLY when the 8x8
 * split looks promising vs 16x16: the 8x8 cost < the 16x16 cost + the rectangle threshold
 * (the rectangle threshold = MV rate of two 8x8s). yah264 runs all four shapes
 * unconditionally -- extra ME work (two partition searches over all refs) when
 * 16x16 or 8x8 is clearly best.
 *
 * WHY KILLED: a genuine quality/speed trade, not a quality-free win, with a
 * SHARP frontier. At the constant-threshold sweep (Y264_PART_THRESH, mlam units):
 * thresh 0 -> ~6% faster (foreman CIF pure-C 1t), but +0.55% VMAF-NEG mean
 * over 6 CIF clips (foreman +0.84, mobile +0.69, coastguard +1.02,
 * stefan +0.35, bus +0.32, tempete +0.09) -- fails the <=0 gate,
 * coastguard breaches "no clip meaningfully worse".
 * thresh 16 -> speed already collapses to ~1-1.5% (the skips that win speed
 * ARE the ones that cost BD).
 * thresh 64 -> BD ~neutral but speed == OFF (admits ~all MBs to 16x8/8x16).
 * No constant threshold has meaningful speed AND mean<=0. A per-MB behaviour-matched
 * the rectangle threshold (needs the 8x8 halves' MV cost plumbed out of eval_inter_part) might
 * do better on both axes -- and MODE 3 below is exactly that (x264's
 * adaptive per-MB margin), shipped as the DEFAULT (see part_earlyterm below).
 * The KILLED verdict above stands for the CONSTANT-threshold modes 1/2 only.
 * Y264_PART_EARLYTERM=0 restores the all-four partition order byte-exactly. */

/* Y264_ME_ET_IMP=<T>: the ME_ET importance rescue. An MB whose mb-tree offset
 * is <= -T keeps its full integer search -- the early-out's quality cost sits
 * on the propagation-heavy MBs, so exempting them aims to buy back the band
 * rows (foreman -1.30 / stefan -1.10 at matched rate) for a minority of the
 * exits. 0 (default) = rescue off, byte-identical. */
static int me_et_imp(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ME_ET_IMP"); v = e ? atoi(e) : 0; }
    return v;
}
static void me_et_imp_stamp(y264_frame_t *f, int mbx, int mby)
{
    int T = me_et_imp();
    if (!T) return;                       /* off: TLS flag stays 0 everywhere */
    int i = mby * f->wmb + mbx;
    y264_me_set_et_off(f->mbtree_off &&
                       f->mbtree_off[i] <= -T * (f->mbt_frac ? 2 : 1));
}

static int part_earlyterm(void)
{
    /* DEFAULT 4: mode 3 = x264's adaptive margin, taken despite its band cost;
 * mode 4 = the same margin plus the importance rescue, which buys back
 * -0.63% median (5/5 clips, akiyo -1.04 .. stefan/mobile -0.39) of mode
 * 3's -1.32% attributed cost for ~1-3 points of the 4.33% wall the trade
 * banks -- a better exchange than the escape. Y264_PART_EARLYTERM=3
 * selects mode 3 alone, =0 the all-four partition order. */
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PART_EARLYTERM"); v = e ? atoi(e) : 4; }
    return v;
}
static long part_thresh(long mlam)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PART_THRESH"); v = e ? atoi(e) : 0; }
    return (long)v * mlam;
}
/* DIAG: Y264_P_RECT=0 skips 16x8/8x16 (bounding experiment); 8x8 kept. Hoisted
 * file-static warmed in y264_mb_warm_statics -- as a function-local first-touch
 * it raced between two GOP workers' wavefronts (same-value init, but the TSan
 * floor is 0 so a report must mean a real bug). */
static int p_rect_on(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_P_RECT"); v = e ? atoi(e) : 1; }
    return v;
}

/* SVT-AV1 / x265-style modulation of the flat partition gate. The flat gate
 * (Y264_PART_EARLYTERM=1) killed BD because a constant threshold treats every MB
 * alike: coastguard's textured water/boundaries paid +1.02% VMAF-NEG while
 * foreman's flat background won the speed. The three-encoder audit says the
 * discriminative axis is marginal-gain + heterogeneity + importance, never motion
 * magnitude. Mode 2 keeps the marginal-gain base gate but NEVER skips the
 * 16x8/8x16 search on a PROTECTED MB:
 * - importance (SVT TPL delta-q analogue): mb-tree flags this MB as feeding many
 * future MBs (strongly-negative QP offset) -> keep full search.
 * - heterogeneity (SVT me_8x8_cost_variance analogue): the local lowres-inter
 * cost field is dispersed (motion/texture boundary) -> the 16x16-vs-8x8-only
 * call is unreliable -> keep full search.
 * Both read already-computed lookahead fields (no extra ME). All env-tunable.
 *
 * VERDICT (PARKED, default OFF, kept as reproducer): the modulation
 * WORKS directionally but hits same wall. VMAF-NEG BD vs default, 6 CIF, 120f,
 * crf30-46: mean flat(=1) +0.755% -> adaptive(=2) +0.598%, and sharply better on
 * the textured clips the guards target (mobile 0.83->0.11, tempete 1.01->0.69,
 * coastguard 0.43->0.26 -- the heterogeneity interlock did exactly its job). BUT
 * (a) still fails the ship gate (mean +0.60% >> +0.10%, 4/6 clips > +0.5%), and
 * (b) speed ceiling is only ~2-4% pure-C -- to hold BD the guards must protect so
 * many MBs that almost no search is cut. This is yah264's "less quality per ME
 * candidate -> needs the search" wall, measured a FIFTH way (after blanket-
 * UMH-off, blanket-rect-off, flat early-term, oracle-gate). No frontier point has
 * both mean<=+0.10 AND meaningful speed. Redirect: quality-per-candidate, the
 * x264 way. */
static int part_slack_x4(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PART_SLACK_X4"); v = e ? atoi(e) : 4; }
    return v;
}
static int part_important_off(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PART_IMPORTANT"); v = e ? atoi(e) : 2; }
    return v;
}
static int part_hetero_pct(void)          /* protect when CoV^2*100 exceeds this */
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PART_HETERO"); v = e ? atoi(e) : 16; }
    return v;
}
/* True when the 3x3 lowres-inter-cost neighbourhood around (mbx,mby) is dispersed
 * (relative variance above the CoV^2 threshold) -- a motion/texture boundary the
 * coarse gate must not prune. Integer math over the fully-precomputed lookahead
 * field, so it is neighbour-symmetric and thread-count deterministic. */
static int part_hetero(y264_frame_t *f, int mbx, int mby)
{
    if (!f->lr_seed_cost) return 0;
    long long sum = 0, sumsq = 0; int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int x = mbx + dx, y = mby + dy;
            if (x < 0 || y < 0 || x >= f->wmb || y >= f->hmb) continue;
            long long c = f->lr_seed_cost[y * f->wmb + x];
            sum += c; sumsq += c * c; n++;
        }
    if (n < 2) return 0;
    long long mean = sum / n;
    if (mean <= 0) return 0;
    long long var = sumsq / n - mean * mean;          /* population variance */
    return var * 100 > mean * mean * (long long)part_hetero_pct();
}
/* Decide whether to run the expensive 16x8/8x16 search. Mode 1 = the exact flat
 * gate (byte-identical reproducer); mode 2 = the SVT-modulated adaptive gate. */
static int part_search_rect(y264_frame_t *f, int mbx, int mby,
                            long cost8_raw, long cost16_raw, long mlam,
                            long mv_slack)
{
    /* Mode 3 (x264's shipped margin): the slack is this MB's
 * own 8x8 mv cost for the two blocks a rect would merge -- adaptive per
 * MB, so static MBs (near-zero mv cost) prune hard while real motion
 * keeps its rect searches. Modes 1/2 keep the fixed lambda threshold.
 *
 * Mode 4 (the quality-regain arm): mode 3's margin PLUS an importance
 * rescue -- an MB whose mb-tree offset
 * says the future leans on it keeps its rect searches even when the
 * margin would prune. The BD the gate spends should concentrate exactly
 * on propagation-heavy MBs, so rescuing them aims to buy back most of
 * the band cost (bus -2.50 at matched rate) for a
 * minority of the pruned searches. Threshold = Y264_PART_IMPORTANT (the
 * parked mode-2 machinery, same units). */
    if (part_earlyterm() >= 3) {
        /* Y264_PART_SLACK_X4 scales the adaptive margin
 * in quarters (4 = x1.0 = byte-identical default; 2 = prune twice as
 * hard). Swept jointly with Y264_PART_IMPORTANT (the rescue axis). */
        if (cost8_raw < cost16_raw + ((mv_slack * part_slack_x4()) >> 2)) return 1;
        if (part_earlyterm() == 4) {
            int ii = mby * f->wmb + mbx;
            if (f->mbtree_off &&
                f->mbtree_off[ii] <= -part_important_off() * (f->mbt_frac ? 2 : 1))
                return 1;
        }
        return 0;
    }
    if (cost8_raw < cost16_raw + part_thresh(mlam)) return 1;   /* marginal-gain: promising */
    if (part_earlyterm() != 2) return 0;                        /* mode 1: flat gate, skip */
    int i = mby * f->wmb + mbx;
    if (f->mbtree_off &&
        f->mbtree_off[i] <= -part_important_off() * (f->mbt_frac ? 2 : 1)) return 1;  /* important */
    if (part_hetero(f, mbx, mby)) return 1;                     /* boundary: protect */
    return 0;
}

/* Full-RD score of one candidate MV set for an inter MB: build prediction,
 * code the residual (RDOQ), and measure J = dist_mb + lam*bits via the real
 * write_inter_mb path. Leaves f->rec + ir consistent with the candidate; the
 * nnz grid and qp chain are restored so trials don't leak state. `pmvx/pmvy`
 * are the decoder-exact predictors (unchanged by the nudge) used to price mvd. */
static long inter_rd_score(y264_frame_t *f, int mbx, int mby, int part,
                           const int *mvx, const int *mvy, const int *pmvx,
                           const int *pmvy, const int *pref, const int *psub,
                           struct inter_result *ir, long lam, int skip_chroma)
{
    NLED(rd_part, 1);
    STG_BEG(STG_INTERRD);
    for (int p = 0; p < 4; p++) ir->ref[p] = pref[p];
    encode_inter_mb(f, mbx, mby, part, mvx, mvy, pref, psub, ir, lam, skip_chroma);
    for (int p = 0; p < 16; p++) { ir->pmvx[p] = pmvx[p]; ir->pmvy[p] = pmvy[p]; }
    long bits;
    if (cabac_rd_on() && f->cabac) {
        bits = est_inter_mb_bits(f, mbx, mby, ir) >> 8;   /* CABAC-accurate integer bits */
    } else {
        int8_t nz[16 + 32]; save_mb_nnz(f, mbx, mby, nz);
        struct qp_chain qc; qp_save(f, &qc);
        y264_bs_t sb; y264_bs_init_count(&sb);        /* pricing only */
        write_inter_mb(&sb, f, mbx, mby, ir);
        bits = (long)y264_bs_pos_bits(&sb);
        load_mb_nnz(f, mbx, mby, nz);
        qp_load(f, &qc);
    }
    long j = dist_mb(f, mbx, mby) + Y264_LAMJ(lam, bits);
    escr(1, f, mbx, mby, dist_mb(f, mbx, mby), (double)j,
         (double)Y264_LAMJ(lam, est_bits_lb(f, ir) >> 8));
    STG_END();  /* STG_INTERRD */
    return j;
}

/* qpel-RD env config. A1(a) `lumaonly` scores the trials luma-only (chroma held
 * at base), env-gated until BD-measured. */
static int qpelrd_cfg(int *hyst, int *lumaonly)
{
    static int enabled = -1, h = 4, l = 0;
    if (enabled < 0) {
        const char *v = getenv("Y264_QPELRD"); enabled = v ? atoi(v) : 1;
        const char *hv = getenv("Y264_QPELRD_HYST"); if (hv) h = atoi(hv);
        const char *lv = getenv("Y264_QPELRD_LUMA"); if (lv) l = atoi(lv);
    }
    *hyst = h; *lumaonly = l;
    return enabled;
}

/* Bounded qpel-RD refinement : SATD already owns the
 * committed MV/ref/shape; here we only try nudging one partition's MV by +/-1
 * quarter-pel (single 4-neighbour pass, NOT iterated), scored by the true
 * full-RD path above. A nudge is accepted only past a hysteresis margin so ties
 * keep the predictor-coherent SATD MV; ref/shape are never touched. We nudge
 * only partitions whose MV no later MVP in this MB depends on (16x16: the sole
 * MV; 16x8/8x16: the second, coding-order-last partition), so the stored
 * predictors stay decoder-exact and the bitstream self-consistent. Returns the
 * (possibly lower) J with f->rec + ir left on the winning candidate. */
static long qpel_rd_nudge(y264_frame_t *f, int mbx, int mby, int part,
                          int *mvx, int *mvy, const int *pmvx, const int *pmvy,
                          const int *pref, const int *psub,
                          struct inter_result *ir, long lam, long base_j)
{
    int hyst, lumaonly, enabled = qpelrd_cfg(&hyst, &lumaonly);
    if (!enabled || part == 3) return base_j;      /* P_8x8 skipped (see header) */
    /* qpel-RD is a subme>=8 tool, gated on the RD level rather than the subpel one
 * (subme>=8) and never runs it at medium (subme 7). Skipping it there removes
 * 4-5 full-MB inter RD encodes per P MB. The subme-10 default is unaffected. */
    if ((f->subme > 0 ? f->subme : 10) < 8) return base_j;

    /* Coding-order-last partition (no successor's MVP depends on it). */
    int mvi = (part == 0) ? 0 : 1;
    static const int dx[4] = { 1, -1, 0, 0 };
    static const int dy[4] = { 0, 0, 1, -1 };
    long margin = Y264_LAMJ(lam, hyst);
    int ox = mvx[mvi], oy = mvy[mvi];
    long best_j = base_j; int bx = ox, by = oy;

    /* Snapshot the base candidate (rec + ir -- the complete net-mutated set of
 * inter_rd_score) so that when no nudge wins we restore it instead of
 * re-encoding the base a second time. inter_rd_score self-restores nnz + the
 * qp chain, so only rec + ir need saving. Byte-identical to the re-encode. */
    struct inter_result ir_base = *ir;
    pixel rec_base[3 * 256];
    save_mb_rec(f, mbx, mby, rec_base);

    for (int k = 0; k < 4; k++) {
        mvx[mvi] = ox + dx[k]; mvy[mvi] = oy + dy[k];
        long jc = inter_rd_score(f, mbx, mby, part, mvx, mvy, pmvx, pmvy,
                                 pref, psub, ir, lam, lumaonly);
        if (jc < best_j - margin) { best_j = jc; bx = mvx[mvi]; by = mvy[mvi]; }
    }
    mvx[mvi] = bx; mvy[mvi] = by;
    if (bx == ox && by == oy) {
        /* Winner is the base: restore its rec + ir and skip the re-encode. */
        *ir = ir_base;
        load_mb_rec(f, mbx, mby, rec_base);
        return base_j;
    }
    /* A nudge won: re-encode it so f->rec + ir match the returned J (the last
 * trial above may have left a losing candidate in place). */
    return inter_rd_score(f, mbx, mby, part, mvx, mvy, pmvx, pmvy,
                          pref, psub, ir, lam, 0);
}

/* Evaluate one partition mode: per-partition motion search (over all list-0
 * references) + encode, returning J = SSD + lambda*bits and filling `ir`. */
/* mb_type ue(v) bit lengths for P partitions 0..3 (P_L0_16x16 / 16x8 / 8x16 /
 * P_8x8), used to make the SATD partition comparison rate-aware like x264. */
static const int PART_MBTYPE_BITS[4] = { 1, 3, 3, 5 };

/* Search the motion for partition `part` (SATD-driven, x264 subme-7 model). When
 * rd_final==0 it only does the ME search, records the motion into `ir`, and
 * returns the SATD-based cost (residual SATD + lambda*mv/ref bits + mb_type bits)
 * for the partition decision -- NO reconstruction or entropy. When rd_final==1 it
 * additionally runs the full RD (real recon + entropy) + qpel-RD nudge on the
 * chosen partition and returns the RD cost. This mirrors x264: SATD picks the
 * partition, full RD scores only the winner. */
static int temporal_seed(y264_frame_t *f, int mbx, int mby, int r, int *sx, int *sy);
static int temporal_seeds(y264_frame_t *f, int mbx, int mby, int r, int *out, int max);
static int temporal_seed_on(int subme);
static int lr_seed_on(void);
static int rich_seeds(void);

/* stage-5 seed dump: see the call-site comment in inter_analyze. */
static void me_dump(const y264_frame_t *f, int mbx, int mby, int px, int py,
                    int tx, int ty, const int *seeds, int nseeds, long c)
{
    static FILE *fp;
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("Y264_ME_DUMP");
        fp = e && e[0] ? fopen(e, "w") : NULL;
        on = fp != NULL;
    }
    if (!on) return;
    fprintf(fp, "%d %d %d %d %d %d %d %ld %d", f->poc, mbx, mby, px, py, tx, ty, c, nseeds);
    for (int k = 0; k < nseeds; k++)
        fprintf(fp, " %d %d", seeds[2 * k], seeds[2 * k + 1]);
    fputc('\n', fp);
}

/* v3 staircase (Y264_STAIR_DEPTH): does this slice's list-0 reference r need
 * the fixed vertical clamp? True for any (possibly in-flight) recent anchor in
 * the clamp SET, keyed by POC -- a machine-invariant function, never of thread
 * count. The set is packed, so the loop exits on the first empty slot and the
 * common unclamped case is the same single compare it always was.
 * The qpel-RD nudge is left unclamped: it moves the searched MV by at most one
 * quarter-pel, well inside the LAG budget's 18-row margin, and closure over
 * medians/skip holds at clamp+1 the same way. Every hop shares one bound: a
 * hop-h reference is protected by h chained row gates, so its true budget is
 * h * LAG and hop 1's bound is conservative for all of them. */
static inline int stair_l0_clamp(const y264_frame_t *f, int r)
{
    for (int h = 0; h < Y264_STAIR_HOPS && f->stair_clamp0_poc[h] >= 0; h++)
        if (f->refs_poc[r] == f->stair_clamp0_poc[h])
            return 1;
    return 0;
}

static int p8_seed16_on(void);
long y264_pstat_part[4], y264_pstat_srch[4];   /* BPROF: eval_inter_part calls / searches by part.
 * Plain globals: counted only under Y264_BPROF, which is t1-only (TSan caught
 * the ungated version racing across the wavefront). */
static int bprof_env(void);
/* Y264_RECT_REFS: x264's rect reference set. A 16x8 or 8x16 partition is
 * searched only on the list-0 references its two 8x8 halves chose, not on
 * every reference; x264's <reference-internal>/p8x16 do the same from
 * me8x8[].i_ref. Only meaningful in the x264 partition order (8x8 before the
 * rects, PART_EARLYTERM), which is where the 8x8 refs exist; the single-thread
 * quality order (stq) never reaches it, so --threads 1 is byte-identical.
 *
 * DEFAULT ON (2026-09-01). Priced on the low-rate HD cells that carry the
 * board's worst-clip leg: rect searches -63% and the P tournament -22% on
 * sunflower_1080p; t12 wall sunflower -7.5%, shields -3.5%, pedestrian -3.1%,
 * park_joy -3.5%, samsung -3%, foreman -3% (medians of 5, control inside 1%).
 * CRF band (both arms in the threaded order, 14 clips, 150f): median +0.06%,
 * range -0.43 (pedestrian) .. +0.72 (bus, saturated). Y264_RECT_REFS=0 escapes
 * to the all-refs rect search. */
static int rect_refs_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_RECT_REFS"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}
static _Thread_local const int *tl_rect_refs;   /* the 8x8 winners' refs, or NULL */

/* Y264_P8_REFCLAMP=1: the 8x8-stage reference clamp (plan item B2), the same
 * rule x264 is documented to apply, written independently. When
 * the 16x16 search chose ref 0 and both the top and left neighbours are
 * coded inter, the P_8x8 stage searches only refs 0..max(neighbour refs)
 * over the six neighbour positions (top-left, top, top+2,
 * top-right, left, left+2, in 4x4 cells): refs older than any neighbour used
 * are not searched. At nref 3 that is 12 searches down to 4 on most MBs. Our
 * part-3 loop had no clamp at all. Applies at every thread count, t1
 * included (x264 runs it at every preset below placebo).
 * DEFAULT ON (2026-09-02). 8x8 searches -47% on sunflower_1080p. Walls,
 * medians of 5 round-robin with a control inside 0.2%: t1 sunflower -4.8%,
 * shields -3.1%, pedestrian -3.4%, samsung -2.6%, park_joy -2.4%, foreman
 * -2.2%; t12 -2.2 / -1.6 / -1.0 / -1.3 / -1.5 / -0.6%. CRF band, 14 clips,
 * 150f, t1: median -0.04% (stq order) / -0.07% (threaded order), worst clip
 * +0.19 / +0.27, best -1.04 / -0.65. Record:
 * local/records/p8-refclamp-2026-09-02.md. Y264_P8_REFCLAMP=0 escapes to the
 * unclamped all-refs 8x8 search. */
static int p8_refclamp_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_P8_REFCLAMP"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}
static _Thread_local int tl_ref16 = -1;         /* the 16x16 winner's ref for the 8x8 clamp, -1 = none */

/* Y264_P_REF0EXIT=1: the post-ref-0 exit, shape-preserving (plan item B1,
 * Appendix A2 (c)). After the ref-0 16x16 search, if its MV lands within L1
 * distance 1 qpel of the P_Skip MV, the remaining references are not
 * searched at 16x16 and the 8x8 stage and the rectangles are skipped: the
 * inter candidate IS that 16x16, and it still goes through the full RD
 * three-way compare against skip and intra (the placement x264 documents,
 * after its ref-0 16x16 search; x264 commits P_SKIP there outright, we keep
 * the compare). The verdict changes only where a split or a non-zero ref would
 * have beaten a 16x16 already on the skip MV: 0.6-1.2% of the full-tournament
 * MBs on the low-rate HD clips, 8.8% on park_joy (Y264_PSKIP_CENSUS,
 * local/records/pskip-census-2026-09-02.md). Applies at every thread count.
 * =1 is the MV test alone: band median +0.15..+0.38%, sita/bus/foreman
 * +0.7..+1.6 (local/records/p-ref0exit-2026-09-02.md). =2 adds the second
 * term of x264's documented gate, the winner's raw SATD (mv and ref bits
 * stripped) under
 * Y264_P_REF0EXIT_K x mlam (x264: 300 x lambda), so the exit is taken only
 * where the residual is small enough that skip is plausible anyway.
 * DEFAULT 2 (2026-09-02) at K=600 (see p_ref0exit_k), on top of Y264_P8_REFCLAMP. Walls at K=300, medians of 5
 * round-robin, control inside 0.4%: t1 sunflower -5.2%, samsung -3.2%,
 * pedestrian -2.6%, park_joy -0.7%, foreman -0.5%, shields -0.4%; t12
 * sunflower -1.5%, pedestrian -1.2%. CRF band, 14 clips, 150f, t1: median
 * +0.03% (stq order) / +0.10% (threaded), worst foreman +0.68 / stockholm
 * +0.68. Mode 1 is the larger prize (t1 shields -14.9%, sunflower -7.2%,
 * the rest -5.6..-6.7%) at a band cost of +0.15..+0.38% median with sita /
 * bus / foreman / stockholm at +0.7..+1.6: a rate trade left to the owner
 * (local/records/p-ref0exit-2026-09-02.md). =0 escapes to the full search. */
static int p_ref0exit_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_P_REF0EXIT"); v = e ? atoi(e) : 2; if (v < 0) v = 0; }
    return v;
}
/* K sweep 2026-09-02 (local/records/p-ref0exit-ksweep-2026-09-02.log), each
 * vs K=300 with B2 on: K=600 t1 shields -5.6% / pedestrian -2.7 / foreman
 * -2.0, band median +0.02 (stq) / +0.05 (threaded), worst +0.49 / +0.90;
 * K=1200 t1 shields -14.0% / foreman -5.5 / park_joy -3.1 / pedestrian
 * -3.1, band median +0.27 / +0.11, worst +0.92 / +1.01 with foreman,
 * stockholm, coastguard at +0.8..+0.9. DEFAULT 600: the largest K whose
 * worst clip stays under +0.5% in the goal-1 order. 1200 and mode 1 are the
 * owner's worst-clip trades. */
static long p_ref0exit_k(void)
{
    static long v = -1;
    if (v < 0) { const char *e = getenv("Y264_P_REF0EXIT_K"); v = e ? atol(e) : 600; if (v < 0) v = 600; }
    return v;
}
static _Thread_local int tl_rx_armed, tl_rx_hit, tl_rx_smvx, tl_rx_smvy;   /* the exit's skip MV, per P MB */
static int mvd_bits(int d);
static int p8_maxref(y264_frame_t *f, int mbx, int mby)
{
    int maxref = f->nref - 1;
    if (maxref <= 0 || tl_ref16 != 0 || !p8_refclamp_on()) return maxref;
    int bx0 = mbx * 4, by0 = mby * 4, w4 = f->wmb * 4;
    if (bx0 == 0 || by0 == 0) return maxref;      /* no top or no left: unclamped */
    const int8_t *g = f->refidx; int st = f->mv_stride;
    if (g[(by0 - 1) * st + bx0] < 0 || g[by0 * st + bx0 - 1] < 0) return maxref;   /* intra top/left */
    static const int dx[6] = { -1, 0, 2, 4, -1, -1 }, dy[6] = { -1, -1, -1, -1, 0, 2 };
    int m = 0;
    for (int i = 0; i < 6; i++) {
        int nx = bx0 + dx[i], ny = by0 + dy[i];
        if (nx >= w4) continue;                    /* top-right off the frame edge */
        int r = g[ny * st + nx];
        if (r > m) m = r;
    }
    return m;
}
static long eval_inter_part(y264_frame_t *f, int mbx, int mby, int part,
                            int mlam, long lam, struct inter_result *ir, int rd_final,
                            const int *seed16 /* qpel {x,y} of the 16x16 winner, or NULL */)
{
    STG_BEG(STG_ME);
    if (bprof_env()) y264_pstat_part[part]++;
    int ss = f->src_stride[0], refs = f->ref_stride[0];
    int mvx[16] = {0}, mvy[16] = {0}, pmvx[16] = {0}, pmvy[16] = {0};
    int pref[4] = {0,0,0,0}, psub[4] = {0,0,0,0};
    int16_t svx[16], svy[16]; int8_t svr[16];
    long satd = 0;
    save_mb_mv(f, mbx, mby, svx, svy, svr);

    if (part == 3) {
        /* Per 8x8: pick the reference with a single-MV 8x8 search (mixed refs at
 * 8x8 granularity), then pick the sub-partition shape at that reference.
 * Winners commit into the motion grid as we go, so later predictors --
 * in this block and in later blocks -- see them, matching the decoder's
 * derivation order. */
        static const int SUBTYPE_BITS[4] = { 1, 3, 3, 5 };   /* ue(v) lengths */
        for (int b = 0; b < 4; b++) {
            int Bpx = mbx * 16 + (b & 1) * 8, Bpy = mby * 16 + (b >> 1) * 8;
            int bx4 = mbx * 4 + (b & 1) * 2, by4 = mby * 4 + (b >> 1) * 2;
            long best8 = -1; int r8 = 0;
            int m8x = 0, m8y = 0, p8x = 0, p8y = 0;
            int maxref = p8_maxref(f, mbx, mby);
            for (int r = 0; r <= maxref; r++) {
                int px, py, tx, ty;
                sub_mvp(f, bx4, by4, 2, mbx, mby, r, &px, &py);
                if (stair_l0_clamp(f, r))
                    y264_me_set_ymax(f->stair_mvy_max);
                int sd[2]; int nsd = 0;
                if (seed16 && p8_seed16_on()) { sd[0] = seed16[0]; sd[1] = seed16[1]; nsd = 1; }
                if (bprof_env()) y264_pstat_srch[3]++;
                long c = y264_me_search(f->src[0] + Bpy * ss + Bpx, ss,
                                        f->refs[r][0], refs, f->padded_w,
                                        f->padded_h, Bpx, Bpy, 8, 8,
                                        px, py, mlam, nsd ? sd : NULL, nsd, &tx, &ty)
                       + (long)mlam * ref_bits(r, f->nref);
                if (stair_l0_clamp(f, r))
                    y264_me_set_ymax(INT_MAX);
                if (best8 < 0 || c < best8) {
                    best8 = c; r8 = r;
                    m8x = tx; m8y = ty; p8x = px; p8y = py;
                }
            }
            best8 -= (long)mlam * ref_bits(r8, f->nref);   /* common to all shapes */

            int16_t bvx[4], bvy[4]; int8_t bvr[4];   /* the block's 2x2 grid cells */
            for (int k = 0; k < 4; k++) {
                int gi = (by4 + (k >> 1)) * f->mv_stride + bx4 + (k & 1);
                bvx[k] = f->mvx[gi]; bvy[k] = f->mvy[gi]; bvr[k] = f->refidx[gi];
            }
            long bestc = best8 + (long)mlam * SUBTYPE_BITS[0];
            int bsub = 0;
            int wvx[4] = { m8x, 0, 0, 0 }, wvy[4] = { m8y, 0, 0, 0 };
            int wpx[4] = { p8x, 0, 0, 0 }, wpy[4] = { p8y, 0, 0, 0 };
            /* Sub-8x8 shapes (8x4/4x8/4x4) are a subme>=8 tool -- x264 medium keeps
 * p4x4 off (its sub-8x8 partition flag) and commits 8x8 sub only. Skipping
 * them removes ~32 of ~41 motion searches per P MB. Default (subme 10)
 * runs all shapes -> byte-identical. */
            int nshape = (f->subme > 0 ? f->subme : 10) >= 8 ? 4 : 1;
            for (int shape = 1; shape < nshape; shape++) {
                for (int k = 0; k < 4; k++) {        /* reset the block's cells */
                    int gi = (by4 + (k >> 1)) * f->mv_stride + bx4 + (k & 1);
                    f->mvx[gi] = bvx[k]; f->mvy[gi] = bvy[k]; f->refidx[gi] = bvr[k];
                }
                long c = (long)mlam * SUBTYPE_BITS[shape];
                int tvx[4], tvy[4], tpx[4], tpy[4];
                for (int s2 = 0; s2 < SUB_NS[shape]; s2++) {
                    int ox, oy, w, h;
                    sub_rect(shape, s2, &ox, &oy, &w, &h);
                    int sbx4 = bx4 + ox / 4, sby4 = by4 + oy / 4;
                    int px, py, tx, ty;
                    sub_mvp(f, sbx4, sby4, w / 4, mbx, mby, r8, &px, &py);
                    if (stair_l0_clamp(f, r8))
                        y264_me_set_ymax(f->stair_mvy_max);
                    c += y264_me_search(f->src[0] + (Bpy + oy) * ss + Bpx + ox, ss,
                                        f->refs[r8][0], refs, f->padded_w,
                                        f->padded_h, Bpx + ox, Bpy + oy, w, h,
                                        px, py, mlam, NULL, 0, &tx, &ty);
                    if (stair_l0_clamp(f, r8))
                        y264_me_set_ymax(INT_MAX);
                    tvx[s2] = tx; tvy[s2] = ty; tpx[s2] = px; tpy[s2] = py;
                    set_region_motion(f, sbx4, sby4, w / 4, h / 4, tx, ty, r8);
                }
                if (c < bestc) {
                    bestc = c; bsub = shape;
                    for (int s2 = 0; s2 < SUB_NS[shape]; s2++) {
                        wvx[s2] = tvx[s2]; wvy[s2] = tvy[s2];
                        wpx[s2] = tpx[s2]; wpy[s2] = tpy[s2];
                    }
                }
            }
            pref[b] = r8; psub[b] = bsub;
            satd += bestc;
            for (int s2 = 0; s2 < SUB_NS[bsub]; s2++) {   /* commit the winner */
                int ox, oy, w, h;
                sub_rect(bsub, s2, &ox, &oy, &w, &h);
                set_region_motion(f, bx4 + ox / 4, by4 + oy / 4, w / 4, h / 4,
                                  wvx[s2], wvy[s2], pref[b]);
                mvx[b * 4 + s2] = wvx[s2];  mvy[b * 4 + s2] = wvy[s2];
                pmvx[b * 4 + s2] = wpx[s2]; pmvy[b * 4 + s2] = wpy[s2];
            }
        }
    } else {
        int nparts = part ? 2 : 1;
        for (int p = 0; p < nparts; p++) {
            int rx, ry, rw, rh, bx4, by4, w4, h4;
            part_rect(mbx, mby, part, p, &rx, &ry, &rw, &rh, &bx4, &by4, &w4, &h4);
            long best = -1;
            /* Spatial-neighbour MV seeds for the full-MB search: the median
 * predictor can miss when true motion matches one neighbour. */
            int seeds[16], nseeds = 0;
            if (part == 0) {
                /* Spatial-neighbour MV seeds: left, top, topright always; topleft
 * is the fourth x264 <reference-internal> corner (<reference-source>), added only at the
 * medium tier so the subme-10 default stays byte-identical. */
                int nbx = mbx * 4, nby = mby * 4;
                int nsp4 = rich_seeds() ? 4 : 3;
                mv_nb_t nb[4] = { nb_at(f, nbx - 1, nby), nb_at(f, nbx, nby - 1),
                                  nb_at(f, nbx + 4, nby - 1), nb_at(f, nbx - 1, nby - 1) };
                for (int k = 0; k < nsp4; k++)
                    if (nb[k].avail) {
                        seeds[2 * nseeds] = nb[k].mvx;
                        seeds[2 * nseeds + 1] = nb[k].mvy;
                        nseeds++;
                    }
            }
            int nsp = nseeds;                /* spatial seed count (temporal added per-ref) */
            /* S2 seed instrument:
 * one record per 16x16 ref0 P search -- predictor, every seed
 * candidate, final MV+cost (qpel units). Offline analysis ranks
 * candidate seed SOURCES by where the far-moves land. t1 only
 * (unsynchronized stream); dedupe offline on (poc,mbx,mby).
 * Y264_ME_DUMP=<path>; default inert. */

            /* Mixed refs (x264 --mixed-refs, on at medium): each partition picks
 * its own list-0 reference; the predictors are refIdx-aware, so they
 * stay correct when partitions land on different refs. */
            for (int r = 0; r < f->nref; r++) {
                int px, py, tx, ty;
                if (tl_rect_refs && (part == 1 || part == 2)) {
                    int ra = part == 1 ? tl_rect_refs[2 * p]     : tl_rect_refs[p];
                    int rb = part == 1 ? tl_rect_refs[2 * p + 1] : tl_rect_refs[p + 2];
                    if (r != ra && r != rb) continue;
                }
                if (part == 0) mv_predict(f, mbx, mby, r, &px, &py);
                else partition_mvp(f, bx4, by4, w4, part, p, r, &px, &py);
                nseeds = nsp;
                if (temporal_seed_on(f->subme))
                    nseeds += temporal_seeds(f, mbx, mby, r, seeds + 2 * nseeds,
                                             rich_seeds() ? 3 : 1);
                /* Lowres (lookahead) MV of this MB vs ref0 -- the current-frame
 * motion x264 seeds from the lowres MVs. On accelerating pans the
 * collocated (previous-frame) seed is stale; this tracks the pan.
 * ref0 + 16x16 only (it chains into the sub-part searches). */
                if (part == 0 && r == 0 && f->lr_seed_mvx && lr_seed_on()) {
                    int li = mby * f->wmb + mbx;
                    seeds[2 * nseeds] = f->lr_seed_mvx[li];
                    seeds[2 * nseeds + 1] = f->lr_seed_mvy[li];
                    nseeds++;
                }
                /* Oracle for the escalation gates: the lowres
 * prior maps cleanly to the 16x16 ref0 search. Keyed on data
 * availability only (NOT lr_seed_on, which gates the rich_seeds
 * experiment); the data is stashed whenever subme<=8. Single-shot
 * (the search consumes+clears it), so other refs/partitions/B see none. */
                if (part == 0 && r == 0 && f->lr_seed_cost && f->lr_seed_mvx) {
                    int li = mby * f->wmb + mbx;
                    y264_me_set_oracle(1, f->lr_seed_cost[li], f->lr_seed_mvx[li], f->lr_seed_mvy[li]);
                }
                if (stair_l0_clamp(f, r))
                    y264_me_set_ymax(f->stair_mvy_max);
                if (bprof_env()) y264_pstat_srch[part]++;
                long c = y264_me_search(f->src[0] + ry * ss + rx, ss, f->refs[r][0], refs,
                                        f->padded_w, f->padded_h, rx, ry, rw, rh,
                                        px, py, mlam, seeds, nseeds, &tx, &ty)
                       + (long)mlam * ref_bits(r, f->nref);
                if (stair_l0_clamp(f, r))
                    y264_me_set_ymax(INT_MAX);
                if (part == 0 && r == 0)
                    me_dump(f, mbx, mby, px, py, tx, ty, seeds, nseeds, c);
                if (best < 0 || c < best) {
                    best = c; pref[p] = r;
                    mvx[p] = tx; mvy[p] = ty; pmvx[p] = px; pmvy[p] = py;
                }
                if (part == 0 && r == 0 && tl_rx_armed &&
                    labs(tx - tl_rx_smvx) + labs(ty - tl_rx_smvy) <= 1 &&
                    (tl_rx_armed < 2 ||
                     c - (long)mlam * (ref_bits(0, f->nref) + mvd_bits(tx - px) + mvd_bits(ty - py))
                         < p_ref0exit_k() * (long)mlam)) {
                    tl_rx_hit = 1;               /* Y264_P_REF0EXIT: refs 1..N-1 not searched */
                    break;
                }
            }
            if (part != 0 && p == 0)  /* partition 1's predictor sees partition 0 */
                set_region_motion(f, bx4, by4, w4, h4, mvx[0], mvy[0], pref[0]);
            satd += best;
        }
    }
    load_mb_mv(f, mbx, mby, svx, svy, svr);

    if (!rd_final) {
        /* Record the SATD-decided motion for a later full-RD of the winner. */
        ir->part = part;
        for (int p = 0; p < 16; p++) {
            ir->mvx[p] = mvx[p]; ir->mvy[p] = mvy[p];
            ir->pmvx[p] = pmvx[p]; ir->pmvy[p] = pmvy[p];
        }
        for (int p = 0; p < 4; p++) { ir->ref[p] = pref[p]; ir->sub[p] = psub[p]; }
        long rv = satd + (long)mlam * PART_MBTYPE_BITS[part];
        STG_END();  /* STG_ME */
        return rv;
    }

    long j = inter_rd_score(f, mbx, mby, part, mvx, mvy, pmvx, pmvy,
                            pref, psub, ir, lam, 0);
    /* Bounded qpel-RD nudge of the SATD winner (SATD still owns the field). */
    j = qpel_rd_nudge(f, mbx, mby, part, mvx, mvy, pmvx, pmvy, pref, psub, ir, lam, j);
    STG_END();  /* STG_ME */
    return j;
}

/* Commit the chosen partitioned motion into the frame's motion field. */
static void commit_inter_motion(y264_frame_t *f, int mbx, int mby, const struct inter_result *ir)
{
    int ms = f->i4mode_stride;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            f->i4mode[(mby * 4 + by) * ms + (mbx * 4 + bx)] = 2;
    if (ir->part == 3) {
        for (int b = 0; b < 4; b++)
            for (int s2 = 0; s2 < SUB_NS[ir->sub[b]]; s2++) {
                int ox, oy, w, h;
                sub_rect(ir->sub[b], s2, &ox, &oy, &w, &h);
                set_region_motion(f, mbx * 4 + (b & 1) * 2 + ox / 4,
                                  mby * 4 + (b >> 1) * 2 + oy / 4, w / 4, h / 4,
                                  ir->mvx[b * 4 + s2], ir->mvy[b * 4 + s2],
                                  ir->ref[b]);
            }
        return;
    }
    int nparts = ir->part ? 2 : 1;
    for (int p = 0; p < nparts; p++) {
        int rx, ry, rw, rh, bx4, by4, w4, h4;
        part_rect(mbx, mby, ir->part, p, &rx, &ry, &rw, &rh, &bx4, &by4, &w4, &h4);
        set_region_motion(f, bx4, by4, w4, h4, ir->mvx[p], ir->mvy[p], ir->ref[p]);
    }
}

/* Per-8x8 derived motion for a direct B macroblock. Spatial fills a uniform
 * refL0; temporal derives it per 8x8 from the co-located reference. */
struct direct_mv { int refL0[4], refL1; int mvL0[4][2], mvL1[4][2]; };

/* MinPositive of the three MV-predictor neighbours' refIdx in a given field:
 * 0 if any neighbour uses the list (single reference), else -1. */
static int min_pos_ref(y264_frame_t *f, int16_t *mx, int16_t *my, int8_t *rf, int mbx, int mby)
{
    int bx = mbx * 4, by = mby * 4, r = -1;
    mv_nb_t A = nb_at_f(f, mx, my, rf, bx - 1, by);
    mv_nb_t B = nb_at_f(f, mx, my, rf, bx, by - 1);
    mv_nb_t C = nb_at_f(f, mx, my, rf, bx + 4, by - 1);
    if (!C.avail) C = nb_at_f(f, mx, my, rf, bx - 1, by - 1);
    int refs[3] = { A.ref, B.ref, C.ref };
    for (int i = 0; i < 3; i++)
        if (refs[i] >= 0 && (r < 0 || refs[i] < r)) r = refs[i];
    return r;
}

/* Spatial direct MV derivation (8.4.1.2.2), single reference per list, with
 * direct_8x8_inference: each 8x8 samples the co-located block at its outer
 * corner for the colZeroFlag test. */
static void spatial_direct(y264_frame_t *f, int mbx, int mby, struct direct_mv *d)
{
    int refL0 = min_pos_ref(f, f->mvx, f->mvy, f->refidx, mbx, mby);
    int refL1 = min_pos_ref(f, f->mvx1, f->mvy1, f->refidx1, mbx, mby);
    int directZero = 0;
    if (refL0 < 0 && refL1 < 0) { refL0 = 0; refL1 = 0; directZero = 1; }
    for (int b = 0; b < 4; b++) d->refL0[b] = refL0;
    d->refL1 = refL1;

    int mp0x = 0, mp0y = 0, mp1x = 0, mp1y = 0;
    if (refL0 >= 0 && !directZero)
        mv_predict_f(f, f->mvx, f->mvy, f->refidx, mbx, mby, refL0, &mp0x, &mp0y);
    if (refL1 >= 0 && !directZero)
        mv_predict_f(f, f->mvx1, f->mvy1, f->refidx1, mbx, mby, refL1, &mp1x, &mp1y);

    static const int cx[4] = { 0, 3, 0, 3 }, cy[4] = { 0, 0, 3, 3 };
    for (int b = 0; b < 4; b++) {
        int ci = (mby * 4 + cy[b]) * f->mv_stride + (mbx * 4 + cx[b]);
        int cr = f->colref[ci], cmx = f->colmvx[ci], cmy = f->colmvy[ci];
        int colZero = (cr == 0 && cmx >= -1 && cmx <= 1 && cmy >= -1 && cmy <= 1);
        int z0 = (refL0 < 0) || directZero || (refL0 == 0 && colZero);
        int z1 = (refL1 < 0) || directZero || (refL1 == 0 && colZero);
        d->mvL0[b][0] = z0 ? 0 : mp0x; d->mvL0[b][1] = z0 ? 0 : mp0y;
        d->mvL1[b][0] = z1 ? 0 : mp1x; d->mvL1[b][1] = z1 ? 0 : mp1y;
    }
}

/* Temporal direct (8.4.1.2.3), direct_8x8_inference: each 8x8 samples the
 * co-located block at its outer corner, maps the picture it referenced (via
 * the stored POC) into this slice's list0, and scales the co-located MV by the
 * POC distances: mvL0 = (DistScaleFactor * mvCol + 128) >> 8, mvL1 = mvL0 -
 * mvCol. An intra co-located block gives refIdx 0 with zero motion.
 *
 * The clause requires the picture refIdxCol names to be present in this slice's
 * list 0, and that binds where the derivation RUNS, so a corner that does not
 * resolve costs this macroblock its direct mode rather than the slice its
 * temporal mode. x264's <reference-internal> returns 0 on the
 * same condition. Returns 0 when any of the four sampled corners fails to
 * resolve, and d is then not usable. */
/* Y264_TDIR_L0ONLY=1 (plan C1): refuse temporal direct for a macroblock when
 * a sampled corner's colocated block predicted from list 1 only. The
 * derivation would scale a backward vector as if it were forward motion;
 * refusal takes the MB down the same path as an unresolvable colocated
 * reference (direct_ok off). DEFAULT OFF pending the rate-anchored BD.
 * Y264_DIRECT_WHY counts, per encode, how many corners resolved through
 * list 1 versus list 0 (printed at close). */
static void tdir_why_dump(void);
static int tdir_l0only_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_TDIR_L0ONLY"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
static int tdir_why_on(void)
{
    static int v = -1;
    if (v < 0) { v = getenv("Y264_DIRECT_WHY") ? 1 : 0; if (v) atexit(tdir_why_dump); }
    return v;
}
static _Atomic long g_tdir_c_l0, g_tdir_c_l1, g_tdir_c_intra, g_tdir_refused;
static void tdir_why_dump(void)
{
    long l0 = atomic_load(&g_tdir_c_l0), l1 = atomic_load(&g_tdir_c_l1), in = atomic_load(&g_tdir_c_intra);
    long n = l0 + l1 + in;
    if (!n) return;
    fprintf(stderr, "tdir corners: via list0 %ld (%.1f%%)  via list1-only %ld (%.1f%%)  intra %ld (%.1f%%)  MBs refused by L0ONLY %ld\n",
            l0, 100.0 * l0 / n, l1, 100.0 * l1 / n, in, 100.0 * in / n, atomic_load(&g_tdir_refused));
}

static int temporal_direct(y264_frame_t *f, int mbx, int mby, struct direct_mv *d)
{
    static const int cx[4] = { 0, 3, 0, 3 }, cy[4] = { 0, 0, 3, 3 };
    d->refL1 = 0;
    int why = tdir_why_on();
    for (int b = 0; b < 4; b++) {
        int ci = (mby * 4 + cy[b]) * f->mv_stride + (mbx * 4 + cx[b]);
        int cp = f->colpoc[ci];
        int from1 = cp >= 0 && (cp & Y264_COLPOC_L1);
        if (cp >= 0) cp &= Y264_COLPOC_MASK;
        if (why) atomic_fetch_add_explicit(cp < 0 ? &g_tdir_c_intra : from1 ? &g_tdir_c_l1 : &g_tdir_c_l0, 1, memory_order_relaxed);
        if (from1 && tdir_l0only_on()) {
            if (why) atomic_fetch_add_explicit(&g_tdir_refused, 1, memory_order_relaxed);
            return 0;
        }
        int mvx = f->colmvx[ci], mvy = f->colmvy[ci];
        int r = 0;
        if (cp < 0) {
            mvx = 0; mvy = 0;
        } else {
            int found = 0;
            for (int k = 0; k < f->nref; k++)
                if (f->refs_poc[k] == cp) { r = k; found = 1; break; }
            if (!found) return 0;
        }
        d->refL0[b] = r;
        int poc0 = f->refs_poc[r];
        int tb = f->poc - poc0, td = f->poc_l1 - poc0;
        if (tb < -128) tb = -128; else if (tb > 127) tb = 127;
        if (td < -128) td = -128; else if (td > 127) td = 127;
        if (td == 0) {                          /* degenerate: unscaled fallback */
            d->mvL0[b][0] = mvx; d->mvL0[b][1] = mvy;
            d->mvL1[b][0] = 0;   d->mvL1[b][1] = 0;
            continue;
        }
        int tx = (16384 + abs(td / 2)) / td;
        int dsf = (tb * tx + 32) >> 6;
        if (dsf < -1024) dsf = -1024; else if (dsf > 1023) dsf = 1023;
        d->mvL0[b][0] = (dsf * mvx + 128) >> 8;
        d->mvL0[b][1] = (dsf * mvy + 128) >> 8;
        d->mvL1[b][0] = d->mvL0[b][0] - mvx;
        d->mvL1[b][1] = d->mvL0[b][1] - mvy;
        { static int dg = -1;   /* DIAG: refuse an out-of-range derived vector */
          if (dg < 0) { const char *e = getenv("Y264_DIAG_TDIRLIM"); dg = e ? atoi(e) : 0; }
          if (dg) { const int L = dg * 4;
            if (d->mvL0[b][0] < -L || d->mvL0[b][0] > L ||
                d->mvL0[b][1] < -L || d->mvL0[b][1] > L ||
                d->mvL1[b][0] < -L || d->mvL1[b][0] > L ||
                d->mvL1[b][1] < -L || d->mvL1[b][1] > L) return 0; } }
    }
    return 1;
}

/* Scale + clip one collocated block's MV into this frame's ref-r interval. The
 * collocated block's MV spans (colframepoc -> colpoc); rescale it to (poc ->
 * refs_poc[r]). Clip to +/-64 pel (x264 <reference-internal> in spirit): an unclipped
 * scaled MV from degenerate collocated data can be far larger than any real
 * motion, and refining outward into it is where the encoder's edge extension and
 * the decoder's diverge (a recon-match break). Returns 1 on a valid seed. */
static int scale_col_mv(y264_frame_t *f, int ci, int td, int *sx, int *sy)
{
    int cp = f->colpoc[ci];
    if (cp < 0) return 0;
    cp &= Y264_COLPOC_MASK;
    int cd = f->colframepoc - cp;            /* collocated block's ref distance */
    if (cd == 0) return 0;
    int mx = (f->colmvx[ci] * td) / cd;
    int my = (f->colmvy[ci] * td) / cd;
    const int LIM = 64 * 4;                  /* +/-64 pel in quarter-pel */
    *sx = mx < -LIM ? -LIM : (mx > LIM ? LIM : mx);
    *sy = my < -LIM ? -LIM : (my > LIM ? LIM : my);
    return 1;
}

/* Temporal (collocated, POC-scaled) MV predictor as an integer-search seed --
 * x264's <reference-internal> (<reference-source>) probes the collocated block plus its right
 * (dx=1) and below (dy=1) neighbours, since motion is locally coherent and a
 * neighbour often tracks it better than the exact collocated cell. Probes the
 * first `npos` positions (1 = just the collocated cell, 3 = the full x264 set),
 * writing a seed (2 ints, quarter-pel) into out[] for each valid one; returns the
 * count. This is the ME seed yah264 lacked -- the reason it needed UMH. */
static int temporal_seeds(y264_frame_t *f, int mbx, int mby, int r, int *out, int npos)
{
    static const int ndx[3] = { 0, 1, 0 }, ndy[3] = { 0, 0, 1 };
    int td = f->poc - f->refs_poc[r];        /* this frame's ref-r distance */
    int n = 0;
    for (int k = 0; k < npos; k++) {
        int nx = mbx + ndx[k], ny = mby + ndy[k];
        if (nx >= f->wmb || ny >= f->hmb) continue;
        int ci = (ny * 4) * f->mv_stride + (nx * 4);
        if (scale_col_mv(f, ci, td, &out[2 * n], &out[2 * n + 1])) n++;
    }
    return n;
}

/* Single-position temporal seed (back-compat wrapper for the B path). */
static int temporal_seed(y264_frame_t *f, int mbx, int mby, int r, int *sx, int *sy)
{
    int out[2];
    if (temporal_seeds(f, mbx, mby, r, out, 1)) { *sx = out[0]; *sy = out[1]; return 1; }
    return 0;
}

/* Temporal/collocated + spatial ME seeds for the P search: on by default at the
 * medium tier (subme <= 8); subme >= 9 keeps the max-quality default byte-identical.
 * Y264_TEMPORAL_SEED forces on(1)/off(0) for A/B. */
static int temporal_seed_on(int subme)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_TEMPORAL_SEED"); env = e ? atoi(e) : -1; }
    if (env >= 0) return env;
    return (subme > 0 ? subme : 10) <= 8;
}

/* Rich ME seed set: the topleft spatial corner,
 * the 2nd/3rd temporal positions, and the lowres/lookahead MV -- i.e. the full
 * x264 <reference-internal> mvc list. Under the UMH wide grid it measures
 * neutral-to-slightly-worse (the grid papers over a coarse seed), so it stays OFF
 * for the default UMH path -- keeping that path byte-identical. But when UMH is
 * disabled (Y264_NO_UMH, the hex-only x264-medium-ME parity path) hex has no wide
 * scan to compensate, so the richer predictors are exactly the distant seeds x264's
 * <reference-internal> starts from; auto-enable them there. Explicit Y264_RICH_SEEDS
 * wins. */
static int rich_seeds(void)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_RICH_SEEDS"); env = e ? atoi(e) : -1; }
    if (env >= 0) return env;
    return y264_me_hex_features();      /* on for --me hex + auto medium/fast tiers */
}

/* Lowres/lookahead MV seed for the P search; only when rich_seeds is on.
 * Y264_LR_SEED forces on(1)/off(0) within that. */
static int lr_seed_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_LR_SEED"); v = e ? atoi(e) : 1; }
    return rich_seeds() && v;
}

/* Seed the B L0/L1 searches with the spatial-neighbour MVs, the direct MV, and
 * the POC-scaled temporal (collocated) MV -- the same predictors x264 feeds into
 * its B motion search. yah264's B ME had NO seeds, so on the hex-only path (no
 * UMH wide scan) it could not reach bus's distant zoom basin; the oracle-seed
 * diagnostic localized the hex-vs-UMH gap to B-frame ME reach (B frames recover
 * ~16pt of the bus gap under an oracle seed, P frames only ~2.5pt). TRAP: the
 * seed array holds 3 spatial + 1 direct + 1 temporal = 5 entries (10 ints), so
 * an 8-int seed0 overruns the stack.
 * Bit values (for A/B): 1 = all; bit0 L0-spatial, bit1 L1-spatial, bit2 temporal.
 * DEFAULT ON under Y264_NO_UMH (mirrors rich_seeds); the default UMH path is
 * untouched (byte-identical). Y264_B_SEEDS overrides. */
static int b_seeds_on(void)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_B_SEEDS"); env = e ? atoi(e) : -1; }
    if (env >= 0) return env;
    return y264_me_hex_features();      /* mirrors rich_seeds */
}

/* Build a direct prediction: per 8x8, MC from the used list(s), bi-averaged.
 * List 0 predicts from that 8x8's derived refL0 (weights follow it too). */
static void build_direct_pred(y264_frame_t *f, int mbx, int mby, const struct direct_mv *d,
                              pixel pred[256], pixel cpred[2][256])
{
    int pw = f->padded_w, ph = f->padded_h;
    for (int b = 0; b < 4; b++) {
        int rb = d->refL0[b] < 0 ? 0 : d->refL0[b];
        const pixel *const *r0 = f->refs[rb];
        int w0, w1; bipred_weights(f, rb, &w0, &w1);
        int ox = (b & 1) * 8, oy = (b >> 1) * 8;
        int lx = mbx * 16 + ox, ly = mby * 16 + oy;
        pixel *dst = pred + oy * 16 + ox;
        if (d->refL0[b] >= 0 && d->refL1 >= 0) {
            pixel p0[256], p1[256];             /* stride 16 -> cached-plane MC */
            y264_me_mc_luma(p0, r0[0], f->ref_stride[0], pw, ph, lx, ly, d->mvL0[b][0], d->mvL0[b][1], 8, 8);
            y264_me_mc_luma(p1, f->ref1[0], f->ref1_stride[0], pw, ph, lx, ly, d->mvL1[b][0], d->mvL1[b][1], 8, 8);
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
                dst[y * 16 + x] = (pixel)clip8((p0[y*16+x] * w0 + p1[y*16+x] * w1 + 32) >> 6);
        } else if (d->refL0[b] >= 0) {
            y264_me_mc_luma(dst, r0[0], f->ref_stride[0], pw, ph, lx, ly, d->mvL0[b][0], d->mvL0[b][1], 8, 8);
        } else {
            y264_me_mc_luma(dst, f->ref1[0], f->ref1_stride[0], pw, ph, lx, ly, d->mvL1[b][0], d->mvL1[b][1], 8, 8);
        }
        if (f->cf_idc == 3) {                       /* 4:4:4: chroma 8x8 = luma 6-tap */
            for (int c = 0; c < 2; c++) {
                pixel *cdst = cpred[c] + oy * 16 + ox;
                if (d->refL0[b] >= 0 && d->refL1 >= 0) {
                    pixel q0[64], q1[64];
                    y264_mc_luma_b(q0, 8, r0[1 + c], f->ref_stride[1 + c], pw, ph, lx, ly, d->mvL0[b][0], d->mvL0[b][1], 8, 8, Y264_CHROMA_BORDER);
                    y264_mc_luma_b(q1, 8, f->ref1[1 + c], f->ref1_stride[1 + c], pw, ph, lx, ly, d->mvL1[b][0], d->mvL1[b][1], 8, 8, Y264_CHROMA_BORDER);
                    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
                        cdst[y * 16 + x] = (pixel)clip8((q0[y*8+x] * w0 + q1[y*8+x] * w1 + 32) >> 6);
                } else if (d->refL0[b] >= 0) {
                    y264_mc_luma_b(cdst, 16, r0[1 + c], f->ref_stride[1 + c], pw, ph, lx, ly, d->mvL0[b][0], d->mvL0[b][1], 8, 8, Y264_CHROMA_BORDER);
                } else {
                    y264_mc_luma_b(cdst, 16, f->ref1[1 + c], f->ref1_stride[1 + c], pw, ph, lx, ly, d->mvL1[b][0], d->mvL1[b][1], 8, 8, Y264_CHROMA_BORDER);
                }
            }
            continue;
        }
        /* chroma quadrant geometry, format-aware: each 8x8 luma quadrant maps to
 * a (8/sub_w)x(8/sub_h) chroma quadrant. 4:2:0 = 4x4, 4:2:2 = 4x8. */
        int sw = f->sub_w, sh = f->sub_h, cw = 16 / sw, chh = 16 / sh;
        int qw = 8 / sw, qh = 8 / sh, cpw = pw / sw, cph = ph / sh;
        int cxo = (b & 1) * qw, cyo = (b >> 1) * qh;
        int ccx = mbx * cw + cxo, ccy = mby * chh + cyo;
        for (int c = 0; c < 2; c++) {
            pixel *cdst = cpred[c] + cyo * cw + cxo;
            if (d->refL0[b] >= 0 && d->refL1 >= 0) {
                pixel q0[64], q1[64];
                y264_mc_chroma(q0, qw, r0[1 + c], f->ref_stride[1 + c], cpw, cph, ccx, ccy, d->mvL0[b][0], d->mvL0[b][1], qw, qh, sw, sh);
                y264_mc_chroma(q1, qw, f->ref1[1 + c], f->ref1_stride[1 + c], cpw, cph, ccx, ccy, d->mvL1[b][0], d->mvL1[b][1], qw, qh, sw, sh);
                for (int y = 0; y < qh; y++) for (int x = 0; x < qw; x++)
                    cdst[y * cw + x] = (pixel)clip8((q0[y*qw+x] * w0 + q1[y*qw+x] * w1 + 32) >> 6);
            } else if (d->refL0[b] >= 0) {
                y264_mc_chroma(cdst, cw, r0[1 + c], f->ref_stride[1 + c], cpw, cph, ccx, ccy, d->mvL0[b][0], d->mvL0[b][1], qw, qh, sw, sh);
            } else {
                y264_mc_chroma(cdst, cw, f->ref1[1 + c], f->ref1_stride[1 + c], cpw, cph, ccx, ccy, d->mvL1[b][0], d->mvL1[b][1], qw, qh, sw, sh);
            }
        }
    }
}

/* --- B_8x8 (four independently predicted quadrants) ---------------------
 *
 * x264 codes 6.4% of its B macroblocks this way and, more importantly, derives
 * from the same analysis the early-terminate estimates that make its 16x8/8x16
 * searches affordable.
 *
 * Deliberately 8x8 granularity only: the standard also allows 8x4/4x8/4x4
 * sub-partitions inside a B_8x8, and x264 does not use them at medium either
 * (b8x8 without b-sub-8x8). Adding them later needs the sub_mb_type table
 * extended and nothing else here to change.
 *
 * The motion for quadrant b is carried in the existing arrays: mv[b] for list 0
 * and mv[4+b] for list 1, ref[b] for the list-0 reference. */

/* Per-quadrant prediction. Structurally build_direct_pred's loop, but the MVs
 * come from the chosen sub_mb_type rather than all from the direct field --
 * kept as a separate function rather than generalising that one, so the
 * direct path stays byte-identical. */
static void build_b8_pred(y264_frame_t *f, int mbx, int mby,
                          const struct inter_result *ir,
                          pixel pred[256], pixel cpred[2][256])
{
    int pw = f->padded_w, ph = f->padded_h;
    for (int b = 0; b < 4; b++) {
        int uL0 = ir->sub[b] & 1, uL1 = (ir->sub[b] >> 1) & 1;
        int rb = ir->ref[b] < 0 ? 0 : ir->ref[b];
        int mv0[2] = { ir->mvx[b], ir->mvy[b] };
        int mv1[2] = { ir->mvx[4 + b], ir->mvy[4 + b] };
        const pixel *const *r0 = f->refs[rb];
        int w0, w1; bipred_weights(f, rb, &w0, &w1);
        int ox = (b & 1) * 8, oy = (b >> 1) * 8;
        int lx = mbx * 16 + ox, ly = mby * 16 + oy;
        pixel *dst = pred + oy * 16 + ox;
        if (uL0 && uL1) {
            pixel p0[256], p1[256];
            y264_me_mc_luma(p0, r0[0], f->ref_stride[0], pw, ph, lx, ly, mv0[0], mv0[1], 8, 8);
            y264_me_mc_luma(p1, f->ref1[0], f->ref1_stride[0], pw, ph, lx, ly, mv1[0], mv1[1], 8, 8);
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
                dst[y * 16 + x] = (pixel)clip8((p0[y*16+x] * w0 + p1[y*16+x] * w1 + 32) >> 6);
        } else if (uL0) {
            y264_me_mc_luma(dst, r0[0], f->ref_stride[0], pw, ph, lx, ly, mv0[0], mv0[1], 8, 8);
        } else {
            y264_me_mc_luma(dst, f->ref1[0], f->ref1_stride[0], pw, ph, lx, ly, mv1[0], mv1[1], 8, 8);
        }
        if (f->cf_idc == 3) {                   /* 4:4:4: chroma = luma 6-tap */
            for (int c = 0; c < 2; c++) {
                pixel *cdst = cpred[c] + oy * 16 + ox;
                if (uL0 && uL1) {
                    pixel q0[64], q1[64];
                    y264_mc_luma_b(q0, 8, r0[1 + c], f->ref_stride[1 + c], pw, ph, lx, ly, mv0[0], mv0[1], 8, 8, Y264_CHROMA_BORDER);
                    y264_mc_luma_b(q1, 8, f->ref1[1 + c], f->ref1_stride[1 + c], pw, ph, lx, ly, mv1[0], mv1[1], 8, 8, Y264_CHROMA_BORDER);
                    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
                        cdst[y * 16 + x] = (pixel)clip8((q0[y*8+x] * w0 + q1[y*8+x] * w1 + 32) >> 6);
                } else if (uL0) {
                    y264_mc_luma_b(cdst, 16, r0[1 + c], f->ref_stride[1 + c], pw, ph, lx, ly, mv0[0], mv0[1], 8, 8, Y264_CHROMA_BORDER);
                } else {
                    y264_mc_luma_b(cdst, 16, f->ref1[1 + c], f->ref1_stride[1 + c], pw, ph, lx, ly, mv1[0], mv1[1], 8, 8, Y264_CHROMA_BORDER);
                }
            }
            continue;
        }
        int sw = f->sub_w, sh = f->sub_h, cw = 16 / sw, chh = 16 / sh;
        int qw = 8 / sw, qh = 8 / sh, cpw = pw / sw, cph = ph / sh;
        int cxo = (b & 1) * qw, cyo = (b >> 1) * qh;
        int ccx = mbx * cw + cxo, ccy = mby * chh + cyo;
        for (int c = 0; c < 2; c++) {
            pixel *cdst = cpred[c] + cyo * cw + cxo;
            if (uL0 && uL1) {
                pixel q0[64], q1[64];
                y264_mc_chroma(q0, qw, r0[1 + c], f->ref_stride[1 + c], cpw, cph, ccx, ccy, mv0[0], mv0[1], qw, qh, sw, sh);
                y264_mc_chroma(q1, qw, f->ref1[1 + c], f->ref1_stride[1 + c], cpw, cph, ccx, ccy, mv1[0], mv1[1], qw, qh, sw, sh);
                for (int y = 0; y < qh; y++) for (int x = 0; x < qw; x++)
                    cdst[y * cw + x] = (pixel)clip8((q0[y*qw+x] * w0 + q1[y*qw+x] * w1 + 32) >> 6);
            } else if (uL0) {
                y264_mc_chroma(cdst, cw, r0[1 + c], f->ref_stride[1 + c], cpw, cph, ccx, ccy, mv0[0], mv0[1], qw, qh, sw, sh);
            } else {
                y264_mc_chroma(cdst, cw, f->ref1[1 + c], f->ref1_stride[1 + c], cpw, cph, ccx, ccy, mv1[0], mv1[1], qw, qh, sw, sh);
            }
        }
    }
}

/* Commit B_8x8 motion into the L0/L1 fields, per quadrant. */
static void commit_b8_motion(y264_frame_t *f, int mbx, int mby,
                             const struct inter_result *ir)
{
    int ms = f->i4mode_stride;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            f->i4mode[(mby * 4 + by) * ms + (mbx * 4 + bx)] = 2;
    for (int b = 0; b < 4; b++) {
        int uL0 = ir->sub[b] & 1, uL1 = (ir->sub[b] >> 1) & 1;
        int rb = ir->ref[b];
        int m0x = ir->mvx[b], m0y = ir->mvy[b];
        int m1x = ir->mvx[4 + b], m1y = ir->mvy[4 + b];
        int ox = (b & 1) * 2, oy = (b >> 1) * 2;
        for (int yy = 0; yy < 2; yy++)
            for (int xx = 0; xx < 2; xx++) {
                int i = (mby * 4 + oy + yy) * f->mv_stride + (mbx * 4 + ox + xx);
                /* An unused list is zeroed, not left stale -- the convention
 * commit_b_motion and commit_bpart_motion both keep. */
                f->mvx[i] = (int16_t)(uL0 ? m0x : 0);
                f->mvy[i] = (int16_t)(uL0 ? m0y : 0);
                f->refidx[i] = uL0 ? (int8_t)rb : -1;
                f->mvx1[i] = (int16_t)(uL1 ? m1x : 0);
                f->mvy1[i] = (int16_t)(uL1 ? m1y : 0);
                f->refidx1[i] = uL1 ? 0 : -1;
            }
    }
}

/* Commit direct per-8x8 motion into the L0/L1 fields. */
static void commit_direct_motion(y264_frame_t *f, int mbx, int mby, const struct direct_mv *d)
{
    int ms = f->i4mode_stride;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            f->i4mode[(mby * 4 + by) * ms + (mbx * 4 + bx)] = 2;
    for (int b = 0; b < 4; b++) {
        int ox = (b & 1) * 2, oy = (b >> 1) * 2;
        for (int yy = 0; yy < 2; yy++)
            for (int xx = 0; xx < 2; xx++) {
                int i = (mby * 4 + oy + yy) * f->mv_stride + (mbx * 4 + ox + xx);
                if (d->refL0[b] >= 0) { f->mvx[i] = (int16_t)d->mvL0[b][0]; f->mvy[i] = (int16_t)d->mvL0[b][1]; f->refidx[i] = (int8_t)d->refL0[b]; }
                else f->refidx[i] = -1;
                if (d->refL1 >= 0) { f->mvx1[i] = (int16_t)d->mvL1[b][0]; f->mvy1[i] = (int16_t)d->mvL1[b][1]; f->refidx1[i] = 0; }
                else f->refidx1[i] = -1;
            }
    }
}

/* Reconstruct a macroblock directly from a prediction (B_Skip: no residual). */
static void store_pred_rec(y264_frame_t *f, int mbx, int mby,
                           const pixel pred[256], pixel cpred[2][256])
{
    int rs = f->rec_stride[0];
    pixel *rec = f->rec[0] + (mby * 16) * rs + mbx * 16;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) rec[y * rs + x] = pred[y * 16 + x];
    /* Chroma is stored format-aware: cpred is a (16/sub_w)x(16/sub_h) block at
 * stride cw. In 4:4:4 (sub_w=sub_h=1) that's a full-res stride-16 16x16
 * block at origin (mbx*16,mby*16), matching build_direct_pred's layout. */
    int cw = 16 / f->sub_w, chh = 16 / f->sub_h;
    for (int c = 0; c < 2; c++) {
        int crs = f->rec_stride[1 + c];
        pixel *crec = f->rec[1 + c] + (mby * chh) * crs + mbx * cw;
        for (int y = 0; y < chh; y++)
            for (int x = 0; x < cw; x++) crec[y * crs + x] = cpred[c][y * cw + x];
    }
}

/* Per-partition per-list motion for the B partition modes, searched once and
 * shared by the three uniform-mode combinations. */
struct bpart_mo {
    int l0ref[2];
    int l0mv[2][2], l0pmv[2][2];
    int l1mv[2][2], l1pmv[2][2];
};

/* Search both partitions of a 16x8/8x16 split on both lists, committing each
 * partition's per-list winner into that list's grid so partition 1's
 * predictors see partition 0 (both grids restored before returning). List 0
 * searches all active references with ref bits in the cost (mixed refs). */
/* Y264_B_RECT_SEED: search each rectangular partition from the 16x16 winner
 * instead of cold and over every reference. The cold form is what made
 * Y264_B_RECT unaffordable -- two partitions x two splits x nref list-0
 * searches per macroblock, ~12 at --ref 3, none of them seeded, on top of the
 * 16x16 work that had already found a winner per list. x264's
 * the B 16x8/8x16 analyses start from the 16x16 result instead.
 * 1 = seed + pin list 0 to the 16x16 winner's reference (default), 0 = the
 * cold form, for the A/B. */
/* Y264_B_8X8: code B macroblocks as four independently predicted 8x8 quadrants
 * (B_8x8, sub_mb_types B_Direct/L0/L1/Bi_8x8). x264 uses this for 6.4% of its B
 * macroblocks and derives from the same analysis the estimates that make its
 * rectangular searches affordable. */
static int b_8x8_on(void)
{
    static int v = -1;
    /* DEFAULT ON. The B_8x8 partition, gated at QGATE=10 with x264's
 * mb_type/sub_mb_type rate charged (b8_rate_on), carries the board's
 * QUALITY leg: dVMAF -0.49 / -0.48 / -0.48 against a 0.5 bar, from
 * -0.61 / -0.56 / -0.56. Gates: CRF band 10/12 negative (median -0.33%,
 * worst +0.02%), ABR band 9/12 negative with touchdown's +11.63%
 * disproved by the ladder shift (+1.23/+1.86 at +/-13%), recon_sweep
 * 300/300, determ_repeat 2/2 under six spinners. It costs 1.1-1.3% of t1
 * wall (wall_ab, arm minus its control). Y264_B_8X8=0 is the escape. */
    if (v < 0) { const char *e = getenv("Y264_B_8X8"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}

/* Y264_B8_DIRECT=0: exclude B_Direct_8x8 quadrants from the sub_mb_type choice.
 * Kept as the A/B control; the mode is recon-verified on that subset. */
static int b8_direct_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_B8_DIRECT"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}

/* Y264_B8_NORD=1: measurement probe, see the call site. */
static int b8_nord_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_B8_NORD"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}

/* Y264_B_SKIP_EXIT_SSD=<n>: the ABSOLUTE distortion bound under which
 * Y264_B_SKIP_EXIT=2 readmits a REFERENCE B to the mid-tournament skip exit.
 *
 * At 512 it never fires -- `=2` produces the same bitstream as `=1` on every
 * clip tried, which is why its BD reads +0.00%. 512 SSD over a 16x16 luma
 * block is an average squared error of two per pixel, i.e. a numerically
 * perfect skip.
 *
 * That matters because ref-B readmission on the OTHER bound is refused -- a
 * lambda-scaled one, which grows with lambda and is therefore most generous
 * exactly at the low rates where the propagation damage lives (akiyo +2.22,
 * park_joy +1.68). So the whole range between "never fires" and "too generous
 * at low rate" is untested, and an ABSOLUTE bound does not have the lambda
 * bound's failure mode by construction.
 *
 * Why it is worth testing: Y264_BPROF says skip-verdict macroblocks consume
 * about 193 ms of the B tournament's 396 ms on samsung, because a third of them
 * run the full motion search and RD phase and a fifth run intra before we
 * decide to skip. */
static long b_skip_exit_ssd(void)
{
    static long v = -1;
    if (v < 0) { const char *e = getenv("Y264_B_SKIP_EXIT_SSD"); v = e ? atol(e) : 512; }
    return v;
}

/* Ref-B readmission to the mid-tournament exit: is THIS reference B macroblock
 * allowed to leave the tournament early?
 *
 * =2 uses the absolute SSD bound above. =3 (E1) uses the PROPAGATION FIELD
 * instead, which is the currency the damage actually
 * travels in: mb-tree's per-MB offset is negative exactly where this macroblock
 * is a source other frames will reference, so `offset >= 0` says "nothing
 * downstream reads this". Guarding ref-B's on slice type (blanket), on a
 * lambda-scaled distortion bound (generous at the low rates where the
 * damage lives -- akiyo +2.22, park_joy +1.68) and on an absolute SSD bound
 * (selects the empty set) all miss the quantity the damage propagates
 * through. `Y264_BLATE_STAT` measured that 26-35% of ref-B late skips
 * are propagation-important against 4-18% of coded ref-B macroblocks, i.e. two
 * thirds of what the blanket ban protects are not leaves.
 *
 * With no mb-tree field the guard has no signal, so it admits nothing and =3
 * degenerates to =1 -- deliberately, since a guard reading an unpopulated
 * signal must fail closed, not open. */
static int bx_ref_admit(const y264_frame_t *f, int mbx, int mby, long bdist_x)
{
    int v = b_skip_exit_env();
    if (v >= 3)
        return f->mbtree_off && f->mbtree_off[mby * f->wmb + mbx] >= 0;
    return v >= 2 && bdist_x <= b_skip_exit_ssd();
}

/* Y264_B8_STAT=1: engagement counters for the B_8x8 / B_RECT arm. The wall
 * price of the arm is two populations, not one -- the macroblocks whose
 * quadrant SEARCH runs and the (smaller) set that also gets its RD trial -- and
 * every gate proposed for it moves one of the two. This prints both, plus the
 * quadrant searches thrown away by the mid-tournament skip exit (x264 never
 * pays those: its B_SKIP return precedes the B 8x8 analysis) and the
 * rectangular searches the estimates admitted. Default inert; the counters are
 * plain (non-atomic) globals, so read it at --threads 1 like BPROF. */
static int b8_stat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_B8_STAT"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
static uint64_t b8s_mb, b8s_search, b8s_gated, b8s_rd, b8s_win, b8s_skipexit, b8s_rect;
/* Mid-tournament skip-exit population, for Y264_B_SKIP_EXIT: macroblocks
 * reaching the check, and why each is or is not admitted. */
static uint64_t bx_reach, bx_exitok, bx_isref, bx_mode0, bx_taken, bx_refblocked, bx_refadmit;
/* E2 stages A/C population, for Y264_BSKIP_ADMIT / _CGUARD. The whole arm is a
 * population argument -- the confirmation's decision is not the problem, the
 * number of macroblocks paying for it is -- so every stage prints its own
 * count and no null is believed without them. */
static uint64_t bxa_reach, bxa_admit, bxa_probe, bxa_held, bxa_commit,
                bxa_nb, bxa_mv, bxa_gsatd, bxa_gcost, bxa_gprop;
static void b8_stat_dump(void)
{
    if (b8s_mb)
    fprintf(stderr, "B8STAT b_mbs=%llu search=%llu (%.1f%%) qgated=%llu rd=%llu (%.1f%%) "
                    "beat-best=%llu (%.1f%% of rd) wasted-by-skipexit=%llu (%.1f%% of search) "
                    "rect-searches=%llu\n",
            (unsigned long long)b8s_mb,
            (unsigned long long)b8s_search, 100.0 * b8s_search / b8s_mb,
            (unsigned long long)b8s_gated,
            (unsigned long long)b8s_rd, 100.0 * b8s_rd / b8s_mb,
            (unsigned long long)b8s_win, b8s_rd ? 100.0 * b8s_win / b8s_rd : 0.0,
            (unsigned long long)b8s_skipexit,
            b8s_search ? 100.0 * b8s_skipexit / b8s_search : 0.0,
            (unsigned long long)b8s_rect);
    if (bx_reach)
        fprintf(stderr, "BXSTAT reach=%llu exit_ok=%llu (%.1f%%) mode0=%llu (%.1f%%) "
                        "ref-slice=%llu (%.1f%%) taken=%llu (%.1f%%) "
                        "blocked-only-by-ref-rule=%llu (%.1f%%) "
                        "ref-readmitted=%llu (%.1f%% of blocked)\n",
            (unsigned long long)bx_reach,
            (unsigned long long)bx_exitok, 100.0*bx_exitok/bx_reach,
            (unsigned long long)bx_mode0, 100.0*bx_mode0/bx_reach,
            (unsigned long long)bx_isref, 100.0*bx_isref/bx_reach,
            (unsigned long long)bx_taken, 100.0*bx_taken/bx_reach,
            (unsigned long long)bx_refblocked, 100.0*bx_refblocked/bx_reach,
            (unsigned long long)bx_refadmit,
            bx_refblocked ? 100.0*bx_refadmit/bx_refblocked : 0.0);
    if (bxa_reach)
        fprintf(stderr, "BXASTAT reach=%llu admitted=%llu (%.1f%%) [nb=%llu mv=%llu] "
                        "probed=%llu (%.1f%%) "
                        "probe-held=%llu (%.1f%% of probed) committed=%llu (%.1f%% of reach) "
                        "declined-by satd=%llu cost=%llu prop=%llu\n",
            (unsigned long long)bxa_reach,
            (unsigned long long)bxa_admit, 100.0*bxa_admit/bxa_reach,
            (unsigned long long)bxa_nb, (unsigned long long)bxa_mv,
            (unsigned long long)bxa_probe, 100.0*bxa_probe/bxa_reach,
            (unsigned long long)bxa_held,
            bxa_probe ? 100.0*bxa_held/bxa_probe : 0.0,
            (unsigned long long)bxa_commit, 100.0*bxa_commit/bxa_reach,
            (unsigned long long)bxa_gsatd, (unsigned long long)bxa_gcost,
            (unsigned long long)bxa_gprop);
}
static void b8_stat_register(void)
{
    static int done = 0;
    if (!done) { done = 1; atexit(b8_stat_dump); }
}

/* E2 stage C: the guard set at the post-ref0
 * B_SKIP commit, Y264_BSKIP_CGUARD bits. Runs only where the probe held AND
 * both ref-0 searches landed on the direct MVs, i.e. on a population that is
 * already small, so each armed bit buys safety with work that is paid rarely.
 *
 * bit0: direct must be SATD-competitive with the ref-0 searches. The bexit_ok
 * shape off the two searches x264 itself considers sufficient --
 * y264_me_search's return is SATD + mv-rate after qpel refinement, the
 * same currency as the SATD phase's satd16[], so the 33/32 bound
 * transfers. At tolerance 1 the MV agreement nearly implies this; it
 * earns its keep at E4's tolerance 2, where "landed within 2 qpel" no
 * longer bounds the cost gap.
 * bit1: the skip recon's own distortion must be cheap in lambda units
 * (skip_costgate's k; the bit asserts even where CONFIRM's arming chose
 * not to consult it). Rate-awareness SATD cannot supply: surviving the
 * quantizer is what skip IS. Requires Y264_SKIP_COSTGATE, else declines.
 * bit2: reference B's additionally need the propagation guard E1 validated
 * (mbtree_off >= 0 -- the COMBINED offset, i.e. AQ's masking allowance
 * must cover the propagation debt). Fails closed without a
 * field, exactly as bx_ref_admit does. */
static int bskip_cguard_ok(const y264_frame_t *f, int mbx, int mby,
                           const pixel *src, int ss, const pixel *dp,
                           long cL0, long cL1, long bdist_x, long lam)
{
    int mask = f->bskip_cguard;
    if (!mask) return 1;
    if (mask & 1) {
        long m = cL0 < cL1 ? cL0 : cL1;
        long dsatd = satd_block(src, ss, dp, 16, 16, 16);
        if (dsatd > m * 33 / 32) { if (b8_stat_on()) bxa_gsatd++; return 0; }
    }
    if (mask & 2) {
        if (!f->skip_costgate || bdist_x > Y264_LAMJ(lam, f->skip_costgate)) {
            if (b8_stat_on()) bxa_gcost++;
            return 0;
        }
    }
    if ((mask & 4) && f->slice_is_ref) {
        if (!f->mbtree_off || f->mbtree_off[mby * f->wmb + mbx] < 0) {
            if (b8_stat_on()) bxa_gprop++;
            return 0;
        }
    }
    return 1;
}


/* Y264_B8_RATE=1: charge the B_8x8 side of x264's mb_type / sub_mb_type rate
 * tables in the SATD domain WITHOUT touching the 16x16 and direct costs the way
 * Y264_BMB_COST does. Two jobs it does, and both are measured problems:
 *
 * - the RD screen does not screen. `sum8` is compared against a threshold
 * derived from the other candidates, and four independently searched
 * quadrants nearly always beat one 16x16 motion vector on distortion, so
 * 69-78% of B macroblocks buy an RD trial and only 4.5-11% of those trials
 * beat the running best (Y264_B8_STAT). The side information the split codes
 * -- one mb_type plus four sub_mb_types plus up to eight mvds -- is exactly
 * what the screen was missing, and charging it is what x264 does
 * (i_mb_b_cost_table[B_8x8] = 9, i_sub_mb_b_cost_table 1/3/3/5).
 * - it fixes the sub-type ranking's bias toward B_Direct_8x8, which today is
 * scored with no rate term at all.
 *
 * Defaults to Y264_BMB_COST so the whole-tournament knob keeps its old meaning.
 * Not byte-identical; priced on the CRF band. */
static int bmb_cost_on(void);
static int b8_rate_on(void)
{
    static int v = -2;
    /* DEFAULT ON: it is half of why the gate is affordable.
 * Without <reference-internal> on the rectangular ESTIMATE a gated
 * macroblock has no 8x8 analysis and therefore no estimates, leaving the
 * surviving candidate set as the one comparison carrying no
 * side-information term -- the gate alone costs half a BD point, with the
 * rate charge three hundredths. Falls back to bmb_cost_on only if
 * explicitly set to a negative value. */
    if (v == -2) { const char *e = getenv("Y264_B8_RATE"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v < 0 ? bmb_cost_on() : v;
}

/* Y264_B8_QGATE=<n>: run the quadrant search only where the best 16x16-level
 * prediction's residual is UNEVENLY distributed across the four quadrants.
 *
 * A split can only pay where the quadrants want different motion, and the tell
 * is already in hand for free: the winning 16x16 prediction is built, so its
 * four 8x8 SATDs cost four satd8x8 against the eight motion searches (plus, via
 * the estimates, the rectangular ones) they decide. The gate admits when
 *
 * qmax * 4 * 16 > qsum * (16 + n)
 *
 * i.e. when the worst quadrant carries more than (1 + n/16) of its even share.
 * n = 0 is inert (any imbalance at all admits, which is almost every MB); the
 * knob is the slack in sixteenths. Not byte-identical -- it is a quality/wall
 * trade and is priced on the CRF band like every other one. */
static int b8_qgate(void)
{
    static int v = -1;
    /* DEFAULT 10, the knee. Priced against QGATE=6 on
 * wall_ab: 1.09-1.34% of t1 wall against 1.99-3.94%, for about half the BD
 * gain and keeping it on bus and stefan, the two clips that carry goal 1's
 * quality deficit. QGATE=6 OVERSHOOTS -- goal 1 needed 0.11 dVMAF and 6
 * delivers 0.15, all of it paid in the median-speed leg. 16 is past the
 * knee (foreman goes positive). 0 disables the gate. */
    if (v < 0) { const char *e = getenv("Y264_B8_QGATE"); v = e ? atoi(e) : 10; }
    return v;
}

/* Y264_BMB_COST=1: charge the mb_type / sub_mb_type bits in the SATD-domain
 * ranking, as x264 does with i_mb_b_cost_table and i_sub_mb_b_cost_table
 * B_Direct 1, B_L0/B_L1 3, B_Bi 5, B_8x8 9; the sub_mb_type table
 * has the same shape (direct 1, L0/L1 3, Bi 5). We charged nothing, which
 * undercharges exactly the modes that code the most side information -- Bi
 * against unipred, and any split against 16x16. Default off pending its band
 * round. */
static int bmb_cost_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_BMB_COST"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}

/* Y264_BBI_PEN=<bits>: an extra, deliberately unprincipled rate charge on the
 * Bi direction in the SATD ranking. We pick Bi 2-3x as often as x264 does
 * (mobile 41.2% vs 20.4%, foreman 13.9% vs 4.7%, bus 26.8% vs 12.7% on the
 * 'mb B' split), and the mb_type table refutes rate asymmetry as the cause --
 * so this exists to answer the next question instead: whether choosing Bi that
 * often is HARMING us. Bound first, mechanism second. Default 0 = inert. */
static int bbi_pen(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_BBI_PEN"); v = e ? atoi(e) : 0; }
    return v;
}

/* Y264_BBI_RD=<bits>: the same Bi probe as Y264_BBI_PEN but applied at the RD
 * stage, which is where the choice is actually made -- the SATD screen admits
 * all three directions whenever they are within thresh of each other, which for
 * L0/L1/Bi is most macroblocks. Default 0 = inert. */
static int bbi_rd_pen(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_BBI_RD"); v = e ? atoi(e) : 0; }
    return v;
}

static int b_rect_seed_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_B_RECT_SEED"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}

static void search_b_part(y264_frame_t *f, int mbx, int mby, int part, int mlam,
                          struct bpart_mo *mo,
                          const int *s0mv, int s0ref, const int *s1mv)
{
    int ss = f->src_stride[0], refs0 = f->ref_stride[0], refs1 = f->ref1_stride[0];
    int16_t svx[16], svy[16]; int8_t svr[16];
    int16_t svx1[16], svy1[16]; int8_t svr1[16];
    save_mb_mv(f, mbx, mby, svx, svy, svr);
    save_mb_mv_f(f, f->mvx1, f->mvy1, f->refidx1, mbx, mby, svx1, svy1, svr1);

    for (int p = 0; p < 2; p++) {
        int rx, ry, rw, rh, bx4, by4, w4, h4;
        part_rect(mbx, mby, part, p, &rx, &ry, &rw, &rh, &bx4, &by4, &w4, &h4);
        long best = -1;
        int seeded = b_rect_seed_on() && s0mv;
        int rlo = seeded ? s0ref : 0, rhi = seeded ? s0ref + 1 : f->nref;
        int sd0[2]; int nsd0 = 0;
        if (seeded) { sd0[0] = s0mv[0]; sd0[1] = s0mv[1]; nsd0 = 1; }
        for (int r = rlo; r < rhi; r++) {
            int px, py, tx, ty;
            partition_mvp_f(f, f->mvx, f->mvy, f->refidx, bx4, by4, w4, part, p,
                            r, &px, &py);
            if (stair_l0_clamp(f, r))       /* v5: list-0 = the in-flight ref B */
                y264_me_set_ymax(f->stair_mvy_max);
            long c = y264_me_search(f->src[0] + ry * ss + rx, ss, f->refs[r][0],
                                    refs0, f->padded_w, f->padded_h, rx, ry, rw, rh,
                                    px, py, mlam, nsd0 ? sd0 : NULL, nsd0, &tx, &ty)
                   + (long)mlam * ref_bits(r, f->nref);
            if (stair_l0_clamp(f, r))
                y264_me_set_ymax(INT_MAX);
            if (best < 0 || c < best) {
                best = c; mo->l0ref[p] = r;
                mo->l0mv[p][0] = tx; mo->l0mv[p][1] = ty;
                mo->l0pmv[p][0] = px; mo->l0pmv[p][1] = py;
            }
        }
        set_region_motion(f, bx4, by4, w4, h4, mo->l0mv[p][0], mo->l0mv[p][1],
                          mo->l0ref[p]);
        int px, py;
        partition_mvp_f(f, f->mvx1, f->mvy1, f->refidx1, bx4, by4, w4, part, p,
                        0, &px, &py);
        if (f->stair_clamp)                 /* list-1 = the in-flight anchor */
            y264_me_set_ymax(f->stair_mvy_max);
        y264_me_set_list(1);                /* same per-list threshold as 16x16 */
        int sd1[2]; int nsd1 = 0;
        if (b_rect_seed_on() && s1mv) { sd1[0] = s1mv[0]; sd1[1] = s1mv[1]; nsd1 = 1; }
        y264_me_search(f->src[0] + ry * ss + rx, ss, f->ref1[0], refs1,
                       f->padded_w, f->padded_h, rx, ry, rw, rh, px, py, mlam,
                       nsd1 ? sd1 : NULL, nsd1, &mo->l1mv[p][0], &mo->l1mv[p][1]);
        y264_me_set_list(0);
        if (f->stair_clamp)
            y264_me_set_ymax(INT_MAX);
        mo->l1pmv[p][0] = px; mo->l1pmv[p][1] = py;
        set_region_motion_f(f, f->mvx1, f->mvy1, f->refidx1, bx4, by4, w4, h4,
                            mo->l1mv[p][0], mo->l1mv[p][1], 0);
    }
    load_mb_mv(f, mbx, mby, svx, svy, svr);
    load_mb_mv_f(f, f->mvx1, f->mvy1, f->refidx1, mbx, mby, svx1, svy1, svr1);
}

/* Per-partition MC cache: the L0 (combo 0) and L1 (combo 1) predictions, so the
 * Bi combo (2) averages them instead of re-running both list MCs. Mirrors
 * bpred_cache for the 16x16 modes. */
struct bpart_cache { pixel l[2][256]; pixel c[2][2][256]; };

/* Build a two-partition B prediction for a uniform combo (both partitions L0,
 * L1, or Bi); each partition's list-0 side uses its own reference and the
 * matching implicit bipred weights. When bc != NULL, combo 0/1 stash their
 * unipred and combo 2 (Bi) averages the stash -- byte-identical to re-MC, but
 * without redoing the L0/L1 motion comp for the Bi candidate. */
static void build_bpart_pred(y264_frame_t *f, int mbx, int mby, int part, int combo,
                             const struct bpart_mo *mo,
                             pixel pred[256], pixel cpred[2][256],
                             struct bpart_cache *bc)
{
    int pw = f->padded_w, ph = f->padded_h;
    /* Bi from cached L0/L1: weighted-average the stashed unipreds over each
 * partition's rect (per-partition weights track l0ref[p]). */
    if (combo == 2 && bc) {
        int c444 = f->cf_idc == 3;
        int sw = f->sub_w, sh = f->sub_h, cstr = 16 / sw;
        for (int p = 0; p < 2; p++) {
            int rx, ry, rw, rh, bx4, by4, w4, h4;
            part_rect(mbx, mby, part, p, &rx, &ry, &rw, &rh, &bx4, &by4, &w4, &h4);
            int ox = rx - mbx * 16, oy = ry - mby * 16;
            int w0, w1; bipred_weights(f, mo->l0ref[p], &w0, &w1);
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) {
                    int i = (oy + y) * 16 + ox + x;
                    pred[i] = (pixel)clip8((bc->l[0][i] * w0 + bc->l[1][i] * w1 + 32) >> 6);
                }
            if (c444) {
                for (int c = 0; c < 2; c++)
                    for (int y = 0; y < rh; y++)
                        for (int x = 0; x < rw; x++) {
                            int i = (oy + y) * 16 + ox + x;
                            cpred[c][i] = (pixel)clip8((bc->c[0][c][i] * w0 + bc->c[1][c][i] * w1 + 32) >> 6);
                        }
            } else {
                int cw = rw / sw, chh = rh / sh, cox = ox / sw, coy = oy / sh;
                for (int c = 0; c < 2; c++)
                    for (int y = 0; y < chh; y++)
                        for (int x = 0; x < cw; x++) {
                            int i = (coy + y) * cstr + cox + x;
                            cpred[c][i] = (pixel)clip8((bc->c[0][c][i] * w0 + bc->c[1][c][i] * w1 + 32) >> 6);
                        }
            }
        }
        return;
    }
    for (int p = 0; p < 2; p++) {
        int rx, ry, rw, rh, bx4, by4, w4, h4;
        part_rect(mbx, mby, part, p, &rx, &ry, &rw, &rh, &bx4, &by4, &w4, &h4);
        int ox = rx - mbx * 16, oy = ry - mby * 16;
        pixel *dst = pred + oy * 16 + ox;
        const pixel *const *r0 = f->refs[mo->l0ref[p]];
        if (combo == 0) {
            y264_me_mc_luma(dst, r0[0], f->ref_stride[0], pw, ph, rx, ry,
                         mo->l0mv[p][0], mo->l0mv[p][1], rw, rh);
        } else if (combo == 1) {
            y264_me_mc_luma(dst, f->ref1[0], f->ref1_stride[0], pw, ph, rx, ry,
                         mo->l1mv[p][0], mo->l1mv[p][1], rw, rh);
        } else {
            pixel p0[256], p1[256];
            int w0, w1; bipred_weights(f, mo->l0ref[p], &w0, &w1);
            y264_me_mc_luma(p0, r0[0], f->ref_stride[0], pw, ph, rx, ry,
                         mo->l0mv[p][0], mo->l0mv[p][1], rw, rh);
            y264_me_mc_luma(p1, f->ref1[0], f->ref1_stride[0], pw, ph, rx, ry,
                         mo->l1mv[p][0], mo->l1mv[p][1], rw, rh);
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++)
                    dst[y * 16 + x] = (pixel)clip8((p0[y*16+x] * w0 + p1[y*16+x] * w1 + 32) >> 6);
        }
        if (f->cf_idc == 3) {                       /* 4:4:4: chroma = luma 6-tap, full-res */
            for (int c = 0; c < 2; c++) {
                pixel *cdst = cpred[c] + oy * 16 + ox;
                if (combo == 0) {
                    y264_mc_luma_b(cdst, 16, r0[1 + c], f->ref_stride[1 + c], pw, ph, rx, ry,
                                 mo->l0mv[p][0], mo->l0mv[p][1], rw, rh, Y264_CHROMA_BORDER);
                } else if (combo == 1) {
                    y264_mc_luma_b(cdst, 16, f->ref1[1 + c], f->ref1_stride[1 + c], pw, ph, rx, ry,
                                 mo->l1mv[p][0], mo->l1mv[p][1], rw, rh, Y264_CHROMA_BORDER);
                } else {
                    pixel q0[256], q1[256];
                    int w0, w1; bipred_weights(f, mo->l0ref[p], &w0, &w1);
                    y264_mc_luma_b(q0, 16, r0[1 + c], f->ref_stride[1 + c], pw, ph, rx, ry,
                                 mo->l0mv[p][0], mo->l0mv[p][1], rw, rh, Y264_CHROMA_BORDER);
                    y264_mc_luma_b(q1, 16, f->ref1[1 + c], f->ref1_stride[1 + c], pw, ph, rx, ry,
                                 mo->l1mv[p][0], mo->l1mv[p][1], rw, rh, Y264_CHROMA_BORDER);
                    for (int y = 0; y < rh; y++)
                        for (int x = 0; x < rw; x++)
                            cdst[y*16+x] = (pixel)clip8((q0[y*16+x] * w0 + q1[y*16+x] * w1 + 32) >> 6);
                }
            }
            continue;
        }
        /* chroma geometry, format-aware (4:2:0 halves both dims; 4:2:2 keeps
 * height, sub_h=1). cstr = full chroma-MB stride 16/sub_w. */
        int sw = f->sub_w, sh = f->sub_h, cstr = 16 / sw;
        int cx = rx / sw, cy = ry / sh, cw = rw / sw, chh = rh / sh;
        int cox = ox / sw, coy = oy / sh;
        for (int c = 0; c < 2; c++) {
            pixel *cdst = cpred[c] + coy * cstr + cox;
            if (combo == 0) {
                y264_mc_chroma(cdst, cstr, r0[1 + c], f->ref_stride[1 + c], pw / sw, ph / sh,
                               cx, cy, mo->l0mv[p][0], mo->l0mv[p][1], cw, chh, sw, sh);
            } else if (combo == 1) {
                y264_mc_chroma(cdst, cstr, f->ref1[1 + c], f->ref1_stride[1 + c], pw / sw,
                               ph / sh, cx, cy, mo->l1mv[p][0], mo->l1mv[p][1], cw, chh, sw, sh);
            } else {
                pixel q0[128], q1[128];
                int w0, w1; bipred_weights(f, mo->l0ref[p], &w0, &w1);
                y264_mc_chroma(q0, cw, r0[1 + c], f->ref_stride[1 + c], pw / sw, ph / sh,
                               cx, cy, mo->l0mv[p][0], mo->l0mv[p][1], cw, chh, sw, sh);
                y264_mc_chroma(q1, cw, f->ref1[1 + c], f->ref1_stride[1 + c], pw / sw,
                               ph / sh, cx, cy, mo->l1mv[p][0], mo->l1mv[p][1], cw, chh, sw, sh);
                for (int y = 0; y < chh; y++)
                    for (int x = 0; x < cw; x++)
                        cdst[y * cstr + x] = (pixel)clip8((q0[y*cw+x] * w0 + q1[y*cw+x] * w1 + 32) >> 6);
            }
        }
    }
    if (bc && combo < 2) {          /* stash this unipred for the Bi average */
        memcpy(bc->l[combo], pred, 256 * sizeof(pixel));
        for (int c = 0; c < 2; c++)
            memcpy(bc->c[combo][c], cpred[c], 256 * sizeof(pixel));
    }
}

/* RD a B 16x16 mode from an already-built prediction: code the residual, return
 * J = SSD + lambda*bits, leaving the reconstruction in rec. Split out of
 * eval_b_mode so the threshold-survivor path can build the pred once (for its
 * SATD rank) and RD only the survivors from that same pred. */
/* Y264_BPROF2: sub-decomposition of one B RD trial (encode / dist / bits),
 * measurement instrument for the RD-cost-model arm, default off, t1 only. */
static int bprof2_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_BPROF2"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
/* Y264_TR_PRE_SHARE=1: decide the trial transform size ONCE per B MB (on the
 * first trial's residual) and reuse it across that MB's direct + 16x16 mode
 * trials, x264's once-per-MB shape (the transform-size decision). The s4 winner
 * re-encode keeps its own decision (their transform_rd refinement analogue).
 * Changes output; default off pending its BD round. */
/* Y264_P8_SEED16=1: seed each 8x8 block's reference search with the 16x16
 * winner's MV, x264's P 8x8 analysis shape (the 16x16 MV as the first candidate).
 * Our 8x8 searched from the median alone, which biases its cost high on
 * motion -- the measured reason every rect early-terminate gate misfired.
 * Changes output; default off pending its BD round. */
static int p8_seed16_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_P8_SEED16"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
static int tr_share_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_TR_PRE_SHARE"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
static uint64_t bp2_enc, bp2_dist, bp2_bits; static long bp2_n;
uint64_t y264_bp2_pre; int y264_bp2_on;
static void bp2_dump(void)
{
    fprintf(stderr, "BPROF2: rd_b_mode trials=%ld encode=%.1fms (tr-pre %.1fms, all callers) dist=%.1fms bits=%.1fms\n",
            bp2_n, bp2_enc / 1e6, y264_bp2_pre / 1e6, bp2_dist / 1e6, bp2_bits / 1e6);
}
static inline uint64_t bp2_now(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void bp2_reg(void){ static int d = 0; if (!d) { d = 1; atexit(bp2_dump); } }

static double rd_b_mode(y264_frame_t *f, int mbx, int mby, int bmode, int l0ref,
                        const int mvL0[2], const int mvL1[2],
                        const int pL0[2], const int pL1[2],
                        pixel pred[256], pixel cpred[2][256],
                        long lam, struct inter_result *ir, int *trpre)
{
    int p2 = bprof2_env();
    if (y264_bp2_on != p2) y264_bp2_on = p2;   /* write-once: the unconditional store raced under TSan */
    uint64_t t0 = p2 ? (bp2_reg(), bp2_n++, bp2_now()) : 0;
    g_res_site = RES_SITE_P;
    encode_inter_res_tp(f, mbx, mby, pred, cpred, 1, ir, lam, 0, trpre);
    g_res_site = RES_SITE_OTHER;
    if (p2) { uint64_t t = bp2_now(); bp2_enc += t - t0; t0 = t; }
    ir->part = 0;                   /* B 16x16 modes: tr8_flag_present must not
 * see a stale P_8x8 marker */
    ir->bpart = 0;
    ir->bmode = bmode;
    ir->ref[0] = l0ref; ir->ref[1] = 0;
    ir->mvx[0] = mvL0[0]; ir->mvy[0] = mvL0[1];
    ir->mvx[1] = mvL1[0]; ir->mvy[1] = mvL1[1];
    ir->pmvx[0] = pL0[0]; ir->pmvy[0] = pL0[1];
    ir->pmvx[1] = pL1[0]; ir->pmvy[1] = pL1[1];

    double j;
    if (cabac_rd_on() && f->cabac) {
        long d = dist_mb(f, mbx, mby);
        if (p2) { uint64_t t = bp2_now(); bp2_dist += t - t0; t0 = t; }
        j = d + Y264_LAMJD(lam, est_b_bits(f, mbx, mby, ir, 0) / 256.0);
        escr(2, f, mbx, mby, d, j, Y264_LAMJD(lam, est_bits_lb(f, ir) / 256.0));
        if (p2) bp2_bits += bp2_now() - t0;
    } else {
        int8_t nz[16 + 32]; save_mb_nnz(f, mbx, mby, nz);
        struct qp_chain qc; qp_save(f, &qc);
        y264_bs_t sb; y264_bs_init_count(&sb);        /* pricing only */
        write_b_mb(&sb, f, mbx, mby, ir);
        j = dist_mb(f, mbx, mby) + Y264_LAMJD(lam, y264_bs_pos_bits(&sb));
        load_mb_nnz(f, mbx, mby, nz);
        qp_load(f, &qc);
    }
    return j;
}

/* Evaluate one B prediction mode (0=L0,1=L1,2=Bi): build prediction, code the
 * residual, return J = SSD + lambda*bits, leaving the reconstruction in rec. */
static double eval_b_mode(y264_frame_t *f, int mbx, int mby, int bmode, int l0ref,
                        const int mvL0[2], const int mvL1[2],
                        const int pL0[2], const int pL1[2],
                        long lam, struct inter_result *ir, struct bpred_cache *bc)
{
    pixel pred[256], cpred[2][256];
    build_bpred(f, mbx, mby, bmode, l0ref, mvL0[0], mvL0[1], mvL1[0], mvL1[1], pred, cpred, bc);
    return rd_b_mode(f, mbx, mby, bmode, l0ref, mvL0, mvL1, pL0, pL1, pred, cpred, lam, ir, NULL);
}

/* se(v) code length of one signed mvd component (2|d| or 2|d|-1 -> ue length). */
static int mvd_bits(int d)
{
    unsigned k = d > 0 ? (unsigned)(2 * d - 1) : (unsigned)(-2 * d);
    int l = 0;
    for (unsigned t = k + 1; t >>= 1; ) l++;
    return 2 * l + 1;
}

/* mv-rate proxy (SATD-domain, mlam * mvd bits) for a two-partition B combo:
 * L0 (0)/L1 (1) code one list's mvds, Bi (2) codes both. Keeps the SATD ranking
 * from over-favouring Bi (lower distortion but ~2x the mv bits). */
static int bpart_mv_rate(const struct bpart_mo *mo, int combo, int mlam)
{
    int useL0 = (combo == 0 || combo == 2), useL1 = (combo == 1 || combo == 2);
    int bits = 0;
    for (int p = 0; p < 2; p++) {
        if (useL0) bits += mvd_bits(mo->l0mv[p][0] - mo->l0pmv[p][0])
                         + mvd_bits(mo->l0mv[p][1] - mo->l0pmv[p][1]);
        if (useL1) bits += mvd_bits(mo->l1mv[p][0] - mo->l1pmv[p][0])
                         + mvd_bits(mo->l1mv[p][1] - mo->l1pmv[p][1]);
    }
    return mlam * bits;
}

/* RD a two-partition B combo from an already-built prediction (split out of
 * eval_b_part so the threshold path can reuse the SATD-scan pred). */
static double rd_b_part(y264_frame_t *f, int mbx, int mby, int part, int combo,
                        const struct bpart_mo *mo,
                        pixel pred[256], pixel cpred[2][256],
                        long lam, struct inter_result *ir)
{
    encode_inter_res(f, mbx, mby, pred, cpred, 1, ir, lam, 0);
    ir->part = 0;
    ir->bpart = part;
    ir->bmode = combo;
    for (int p = 0; p < 2; p++) {
        ir->ref[p] = mo->l0ref[p];
        ir->mvx[p] = mo->l0mv[p][0];      ir->mvy[p] = mo->l0mv[p][1];
        ir->pmvx[p] = mo->l0pmv[p][0];    ir->pmvy[p] = mo->l0pmv[p][1];
        ir->mvx[2 + p] = mo->l1mv[p][0];  ir->mvy[2 + p] = mo->l1mv[p][1];
        ir->pmvx[2 + p] = mo->l1pmv[p][0]; ir->pmvy[2 + p] = mo->l1pmv[p][1];
    }

    double j;
    if (cabac_rd_on() && f->cabac) {
        j = dist_mb(f, mbx, mby) + Y264_LAMJD(lam, est_b_bits(f, mbx, mby, ir, 0) / 256.0);
        escr(3, f, mbx, mby, dist_mb(f, mbx, mby), j,
             Y264_LAMJD(lam, est_bits_lb(f, ir) / 256.0));
    } else {
        int8_t nz[16 + 32]; save_mb_nnz(f, mbx, mby, nz);
        struct qp_chain qc; qp_save(f, &qc);
        y264_bs_t sb; y264_bs_init_count(&sb);        /* pricing only */
        write_b_mb(&sb, f, mbx, mby, ir);
        j = dist_mb(f, mbx, mby) + Y264_LAMJD(lam, y264_bs_pos_bits(&sb));
        load_mb_nnz(f, mbx, mby, nz);
        qp_load(f, &qc);
    }
    return j;
}

/* Evaluate one two-partition B combo from the shared search: build the
 * prediction, code the residual, return J, leaving the recon in rec. */
static double eval_b_part(y264_frame_t *f, int mbx, int mby, int part, int combo,
                        const struct bpart_mo *mo, long lam, struct inter_result *ir)
{
    pixel pred[256], cpred[2][256];
    build_bpart_pred(f, mbx, mby, part, combo, mo, pred, cpred, NULL);
    return rd_b_part(f, mbx, mby, part, combo, mo, pred, cpred, lam, ir);
}

/* Threshold-survivor B mode decision (x264's B RD stage: SATD-rank
 * every inter B mode {Direct, L0, L1, Bi, 16x8-winner, 8x16-winner}, then full-RD
 * only the survivors within thresh = i_satd_inter*(17+psy)/16 + 1 of the best inter
 * SATD -- adaptive ~2-4 RD instead of the fixed ~6. Preserves the quality-critical
 * Bi-vs-uni choice (co-clustered modes both survive), unlike a top-1 SATD gate
 * (which measured +3.2% bus). On at the medium tier (subme <= 8); subme >= 9 keeps
 * the full tournament so the max-quality default is byte-identical.
 * Y264_B_THRESH forces on(1)/off(0). */
static int b_rect_on(void)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_B_RECT"); env = e ? atoi(e) : 0; }
    return env;
}

static int b_thresh_on(int subme)
{
    static int env = -2;
    if (env == -2) { const char *e = getenv("Y264_B_THRESH"); env = e ? atoi(e) : -1; }
    if (env >= 0) return env;
    return (subme > 0 ? subme : 10) <= 8;
}

/* mvd + ref rate (SATD-domain, mlam-weighted) for a 16x16 B mode, matching the
 * mvd_bits model the two-partition gate uses so all inter modes share one SATD
 * threshold. L0 (0) codes list-0 mvd + ref, L1 (1) list-1 mvd, Bi (2) both. */
static int b16_mv_rate(int bmode, const int mvL0[2], const int mvL1[2],
                       const int pL0[2], const int pL1[2], int rref, int mlam)
{
    int rL0 = mvd_bits(mvL0[0] - pL0[0]) + mvd_bits(mvL0[1] - pL0[1]) + rref;
    int rL1 = mvd_bits(mvL1[0] - pL1[0]) + mvd_bits(mvL1[1] - pL1[1]);
    int bits = bmode == 0 ? rL0 : bmode == 1 ? rL1 : rL0 + rL1;
    return mlam * bits;
}

/* Luma-only prediction of one 8x8 quadrant, for the SATD ranking of the four
 * sub_mb_types. STRIDE 16: y264_me_mc_luma hard-codes a stride-16 destination
 * on both its paths, so the buffer is 8 rows of 16 and the reader must use the
 * same stride. (Reading 64 pixels back at stride 8 overflows: it scribbles
 * the caller's stack and ranks the sub-types on garbage.) */
#define B8_PRED_STRIDE 16
static void b8_blk_pred(y264_frame_t *f, int mbx, int mby, int b,
                        int uL0, int uL1, int rb, const int *mv0, const int *mv1,
                        pixel out[8 * B8_PRED_STRIDE])
{
    int pw = f->padded_w, ph = f->padded_h;
    int lx = mbx * 16 + (b & 1) * 8, ly = mby * 16 + (b >> 1) * 8;
    int w0, w1; bipred_weights(f, rb < 0 ? 0 : rb, &w0, &w1);
    if (uL0 && uL1) {
        pixel p0[8 * B8_PRED_STRIDE], p1[8 * B8_PRED_STRIDE];
        y264_me_mc_luma(p0, f->refs[rb < 0 ? 0 : rb][0], f->ref_stride[0], pw, ph, lx, ly, mv0[0], mv0[1], 8, 8);
        y264_me_mc_luma(p1, f->ref1[0], f->ref1_stride[0], pw, ph, lx, ly, mv1[0], mv1[1], 8, 8);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int i = y * B8_PRED_STRIDE + x;
                out[i] = (pixel)clip8((p0[i] * w0 + p1[i] * w1 + 32) >> 6);
            }
    } else if (uL0) {
        y264_me_mc_luma(out, f->refs[rb < 0 ? 0 : rb][0], f->ref_stride[0], pw, ph, lx, ly, mv0[0], mv0[1], 8, 8);
    } else {
        y264_me_mc_luma(out, f->ref1[0], f->ref1_stride[0], pw, ph, lx, ly, mv1[0], mv1[1], 8, 8);
    }
}

/* Choose a sub_mb_type per quadrant: Direct / L0 / L1 / Bi, ranked by the
 * quadrant's SATD plus its motion rate. Each quadrant's motion is committed
 * before the next one's predictor is formed (partition 1 must see partition 0,
 * as in search_b_part), and the grids are restored at exit -- commit_b8_motion
 * writes them for real if this mode wins.
 *
 * List 0 is pinned to the 16x16 winner's reference and both lists are seeded
 * from the 16x16 winners, for the same reason the rectangular search is: a
 * cold search over every reference is what makes these modes unaffordable. */
/* Per-quadrant analysis by-products, kept so the rectangular searches can be
 * ESTIMATED from them instead of run cold -- x264's
 * "try to estimate the cost of b16x8/b8x16 based on the satd scores of the
 * b8x8 modes". satd[list][quadrant] is distortion only;
 * rate[list][quadrant] is that list's mv (+ref for list 0) in bits. */
struct b8_parts { int satd[3][4]; int rate[2][4]; };

static long search_b_8x8(y264_frame_t *f, int mbx, int mby, int mlam,
                         const struct direct_mv *d, int direct_ok,
                         const pixel dp[256],
                         const int *mvL0_16, int r0_16, const int *mvL1_16,
                         struct inter_result *ir, struct b8_parts *pp)
{
    long sum8 = 0;              /* SATD + mv/ref rate, summed over the quadrants;
 * commensurate with satd16[] and ptsatd[] */
    int ss = f->src_stride[0], refs0 = f->ref_stride[0], refs1 = f->ref1_stride[0];
    int16_t svx[16], svy[16]; int8_t svr[16];
    int16_t svx1[16], svy1[16]; int8_t svr1[16];
    save_mb_mv(f, mbx, mby, svx, svy, svr);
    save_mb_mv_f(f, f->mvx1, f->mvy1, f->refidx1, mbx, mby, svx1, svy1, svr1);

    for (int b = 0; b < 4; b++) {
        int ox = (b & 1) * 8, oy = (b >> 1) * 8;
        int bx4 = mbx * 4 + (b & 1) * 2, by4 = mby * 4 + (b >> 1) * 2;
        int rx = mbx * 16 + ox, ry = mby * 16 + oy;
        const pixel *src = f->src[0] + ry * ss + rx;

        int p0[2], p1[2], m0[2], m1[2];
        partition_mvp_f(f, f->mvx, f->mvy, f->refidx, bx4, by4, 2, 3, b, r0_16, &p0[0], &p0[1]);
        int sd0[2] = { mvL0_16[0], mvL0_16[1] };
        if (stair_l0_clamp(f, r0_16)) y264_me_set_ymax(f->stair_mvy_max);
        y264_me_search(src, ss, f->refs[r0_16][0], refs0, f->padded_w, f->padded_h,
                       rx, ry, 8, 8, p0[0], p0[1], mlam, sd0, 1, &m0[0], &m0[1]);
        if (stair_l0_clamp(f, r0_16)) y264_me_set_ymax(INT_MAX);

        partition_mvp_f(f, f->mvx1, f->mvy1, f->refidx1, bx4, by4, 2, 3, b, 0, &p1[0], &p1[1]);
        int sd1[2] = { mvL1_16[0], mvL1_16[1] };
        if (f->stair_clamp) y264_me_set_ymax(f->stair_mvy_max);
        y264_me_set_list(1);
        y264_me_search(src, ss, f->ref1[0], refs1, f->padded_w, f->padded_h,
                       rx, ry, 8, 8, p1[0], p1[1], mlam, sd1, 1, &m1[0], &m1[1]);
        y264_me_set_list(0);
        if (f->stair_clamp) y264_me_set_ymax(INT_MAX);

        int rref = ref_bits(r0_16, f->nref);
        int r0b = mvd_bits(m0[0] - p0[0]) + mvd_bits(m0[1] - p0[1]) + rref;
        int r1b = mvd_bits(m1[0] - p1[0]) + mvd_bits(m1[1] - p1[1]);
        /* Ranking the four sub_mb_types costs two motion compensations, not
 * five. The DIRECT quadrant is scored against the direct prediction
 * already built for the B_Skip candidate (`dp`, stride 16, same MVs and
 * the same weighted average -- build_direct_pred and b8_blk_pred agree
 * pixel for pixel), and BI is the weighted average of the L0 and L1
 * predictions this same loop just built. x264 does both
 * (the B 8x8 analysis scores direct against the reconstruction and averages its
 * two unipreds). Byte-identical, and it is the ranking half of the
 * mode's wall. */
        pixel blk[8 * B8_PRED_STRIDE];
        pixel uni[2][8 * B8_PRED_STRIDE];       /* [0] = the L0 pred, [1] = L1 */
        int w0, w1; bipred_weights(f, r0_16, &w0, &w1);
        long best = LONG_MAX; int bsub = 1;
        for (int sub = 0; sub < 4; sub++) {
            /* Y264_B8_DIRECT=0 excludes B_Direct_8x8 quadrants, the subset the
 * mode is verified on. */
            if (sub == 0 && (!direct_ok || !b8_direct_on())) continue;
            const pixel *cand; int cstride = B8_PRED_STRIDE;
            if (sub == 0) {
                cand = dp + oy * 16 + ox; cstride = 16;
            } else if (sub == 3) {
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        int i = y * B8_PRED_STRIDE + x;
                        blk[i] = (pixel)clip8((uni[0][i] * w0 + uni[1][i] * w1 + 32) >> 6);
                    }
                cand = blk;
            } else {
                b8_blk_pred(f, mbx, mby, b, sub == 1, sub == 2, r0_16,
                            m0, m1, uni[sub - 1]);
                cand = uni[sub - 1];
            }
            long c = satd_block(src, ss, cand, cstride, 8, 8);
            if (pp && sub >= 1) pp->satd[sub - 1][b] = (int)c;
            if (sub == 1) c += (long)mlam * r0b;
            else if (sub == 2) c += (long)mlam * r1b;
            else if (sub == 3) c += (long)mlam * (r0b + r1b);
            if (b8_rate_on())
                c += (long)mlam * (sub == 0 ? 1 : sub == 3 ? 5 : 3);
            if (c < best) { best = c; bsub = sub; }
        }
        sum8 += best;
        if (pp) { pp->rate[0][b] = r0b; pp->rate[1][b] = r1b; }
        ir->b8m[b] = bsub;
        /* The result is self-contained: a direct quadrant's DERIVED motion and
 * reference are stored here too, so nothing downstream (prediction,
 * commit, the ref_idx context) has to reach back for the direct field
 * -- which is only valid while this macroblock is being analysed, and
 * the CABAC emit runs in a later pass. sub[] carries list usage:
 * bit 0 = list 0, bit 1 = list 1. */
        int uL0 = bsub == 0 ? (d->refL0[b] >= 0) : (bsub == 1 || bsub == 3);
        int uL1 = bsub == 0 ? (d->refL1 >= 0)    : (bsub == 2 || bsub == 3);
        ir->sub[b] = (uL0 ? 1 : 0) | (uL1 ? 2 : 0);
        ir->ref[b] = bsub == 0 ? d->refL0[b] : r0_16;
        ir->mvx[b]     = bsub == 0 ? d->mvL0[b][0] : m0[0];
        ir->mvy[b]     = bsub == 0 ? d->mvL0[b][1] : m0[1];
        ir->mvx[4 + b] = bsub == 0 ? d->mvL1[b][0] : m1[0];
        ir->mvy[4 + b] = bsub == 0 ? d->mvL1[b][1] : m1[1];
        ir->pmvx[b] = p0[0];        ir->pmvy[b] = p0[1];
        ir->pmvx[4 + b] = p1[0];    ir->pmvy[4 + b] = p1[1];

        /* Commit this quadrant so the next one's predictor sees it. */
        set_region_motion_f(f, f->mvx, f->mvy, f->refidx, bx4, by4, 2, 2,
                            uL0 ? ir->mvx[b] : 0, uL0 ? ir->mvy[b] : 0,
                            uL0 ? ir->ref[b] : -1);
        set_region_motion_f(f, f->mvx1, f->mvy1, f->refidx1, bx4, by4, 2, 2,
                            uL1 ? ir->mvx[4 + b] : 0, uL1 ? ir->mvy[4 + b] : 0,
                            uL1 ? 0 : -1);
    }
    ir->part = 0;
    ir->bpart = 3;
    ir->bmode = 0;
    load_mb_mv(f, mbx, mby, svx, svy, svr);
    load_mb_mv_f(f, f->mvx1, f->mvy1, f->refidx1, mbx, mby, svx1, svy1, svr1);
    if (b8_rate_on()) sum8 += (long)mlam * 9;       /* i_mb_b_cost_table[B_8x8] */
    return sum8;
}

/* Commit a two-partition B macroblock's motion into both lists' grids. */
static void commit_bpart_motion(y264_frame_t *f, int mbx, int mby,
                                const struct inter_result *ir)
{
    int ms = f->i4mode_stride;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            f->i4mode[(mby * 4 + by) * ms + (mbx * 4 + bx)] = 2;
    int useL0 = (ir->bmode == 0 || ir->bmode == 2);
    int useL1 = (ir->bmode == 1 || ir->bmode == 2);
    for (int p = 0; p < 2; p++) {
        int rx, ry, rw, rh, bx4, by4, w4, h4;
        part_rect(mbx, mby, ir->bpart, p, &rx, &ry, &rw, &rh, &bx4, &by4, &w4, &h4);
        if (useL0)
            set_region_motion(f, bx4, by4, w4, h4, ir->mvx[p], ir->mvy[p], ir->ref[p]);
        else
            set_region_motion(f, bx4, by4, w4, h4, 0, 0, -1);
        if (useL1)
            set_region_motion_f(f, f->mvx1, f->mvy1, f->refidx1, bx4, by4, w4, h4,
                                ir->mvx[2 + p], ir->mvy[2 + p], 0);
        else
            set_region_motion_f(f, f->mvx1, f->mvy1, f->refidx1, bx4, by4, w4, h4,
                                0, 0, -1);
    }
}

/* CABAC MB writers (defined further down) used by the B-slice CABAC path. */
static void cabac_mb_skip(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby, int skip, int base);
static void write_b_direct_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                 const struct inter_result *ir);
static void write_b_inter_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                const struct inter_result *ir);
static void write_intra_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                              const struct intra_mb *o, int slice);
/* W0 4e two-pass: the B analyze/emit halves (below) call these author/emit
 * helpers before their definitions further down. */
static void author_b_direct_cabac(y264_frame_t *f, int mbx, int mby,
                                  const struct inter_result *ir);
static void emit_b_direct_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                const struct inter_result *ir);
static void author_b_inter_cabac(y264_frame_t *f, int mbx, int mby,
                                 const struct inter_result *ir);
static void emit_b_inter_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                               const struct inter_result *ir);
static void author_intra_cabac(y264_frame_t *f, int mbx, int mby, const struct intra_mb *o);
static void emit_intra_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                             const struct intra_mb *o, int slice);
static int probe_skip(y264_frame_t *f, int mbx, int mby, int strict, int dec);

/* Y264_FLATSKIP_STAT=1 -- the CEILING for the flat-content skip gate, measured
 * before any threshold is chosen (the standing rule: a ceiling before a
 * threshold). For every B macroblock that ESCAPES the early skip probe and
 * therefore pays the full tournament, record the source texture energy (the
 * flatness signal, memoed in dist_mb as te_src4) against the B_Skip distortion
 * already computed at that point, binned by what the macroblock finally chose.
 *
 * What it answers: if a gate accepted skip for everything under (energy E,
 * distortion D), how much tournament would it delete, and how many of those
 * macroblocks were NOT going to be skip anyway -- i.e. how many would move
 * bits. The refused arms all widened acceptance globally; the question here is
 * whether FLATNESS separates the free population from the expensive one.
 *
 * MEASURED 2026-08-27, AND IT DOES NOT. bbb_720p, 60 frames, CRF 25: 108,279 B
 * macroblocks escape the probe and 70.1% of them still end as SKIP, so the
 * prize is real -- their tournament work is ~26% of all macroblock analysis.
 * But pooled by axis, only DISTORTION separates them:
 *
 *   by B_Skip distortion   97.0% -> 83.0% -> 62.7%   (monotone)
 *   by source texture energy 96.3% -> 83.8% -> 58.9% -> 68.3%   (it is not)
 *
 * The largest energy row, 81,886 macroblocks, sits at 68.3% -- a coin flip
 * dressed as a signal. So a flat-content gate cannot be built on this feature,
 * and the queue item's premise is wrong. The signal that DOES work is the skip
 * distortion the mid-tournament exit already uses; the only safe pre-tournament
 * bound (97% precision) covers 5% of the escaped population, which is not worth
 * the bits risk. See docs/work-queue.md.
 *
 * t1 only (plain globals), default inert, verified md5-identical on/off. */
#define FS_EB 8                       /* texture-energy buckets, log2 */
#define FS_DB 8                       /* skip-distortion buckets, log2 */
static long fs_esc[FS_EB][FS_DB];     /* escaped the probe, by (E, D) */
static long fs_skip[FS_EB][FS_DB];    /* ...and still ended as skip */
/* The RD-FLOOR curve (x264's entry commit at its RD level: commit
 * skip when its distortion is under the minimum RD cost ANY non-skip mode
 * could pay -- x264 prices that floor at 6 bits (minimum CAVLC cost of a coded
 * MB) times lambda2. Sweep the bits constant to get coverage/precision without
 * committing to x264's 6. Indexed [bits: 3,6,12,24]. */
static const int fs_rdbits[4] = { 3, 6, 12, 24 };
static long fs_rd[4], fs_rds[4];      /* escapees under bound / of which skip */
void y264_flatskip_stat_dump(void);
static int flatskip_stat_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("Y264_FLATSKIP_STAT"); v = e ? atoi(e) : 0;
        if (v) atexit(y264_flatskip_stat_dump);
    }
    return v;
}
static int fs_bucket(long v, int nb)
{
    int b = 0;
    while (v > 0 && b < nb - 1) { v >>= 2; b++; }
    return b;
}
void y264_flatskip_stat_dump(void)
{
    if (!flatskip_stat_on()) return;
    long tot = 0, tots = 0;
    for (int e = 0; e < FS_EB; e++)
        for (int d = 0; d < FS_DB; d++) { tot += fs_esc[e][d]; tots += fs_skip[e][d]; }
    if (!tot) return;
    fprintf(stderr, "FLATSKIP: %ld B MBs escaped the early probe, %ld (%.1f%%) "
            "still ended as SKIP -- that is the population a flat gate could "
            "delete for free\n", tot, tots, 100.0 * tots / tot);
    for (int q = 0; q < 4; q++)
        if (fs_rd[q])
            fprintf(stderr, "FLATSKIP RD-FLOOR bits=%-2d: %ld escapees under the "
                    "bound (%.1f%% of escaped), %.2f%% of those end as SKIP\n",
                    fs_rdbits[q], fs_rd[q], 100.0 * fs_rd[q] / tot,
                    100.0 * fs_rds[q] / fs_rd[q]);
    fprintf(stderr, "FLATSKIP: rows = source texture energy (flat at top), "
            "cols = B_Skip distortion (cheap at left); each cell "
            "escaped/of-which-skip\n");
    for (int e = 0; e < FS_EB; e++) {
        long re = 0; for (int d = 0; d < FS_DB; d++) re += fs_esc[e][d];
        if (!re) continue;
        fprintf(stderr, "  E<%-9ld", (long)1 << (2 * (e + 1)));
        for (int d = 0; d < FS_DB; d++)
            fprintf(stderr, " %7ld/%-7ld", fs_esc[e][d], fs_skip[e][d]);
        fprintf(stderr, "\n");
    }
}
static int probe_skip_g(y264_frame_t *f, int mbx, int mby, int strict, int dec,
                        int *tol);
static int mv_agrees(int amx, int amy, int bmx, int bmy, int tol);

/* B slice: per MB choose L0 / L1 / Bi / intra by RD. No B_Skip/direct yet, so
 * every macroblock is coded explicitly with mb_skip_run 0. */
/* Does the winning intra MB carry mb_qp_delta? (Mirrors the qpd_cavlc conditions
 * in the emit paths.) The two-pass analysis uses this to advance the raster-order
 * prev_qp chain the RD cost trials read: the mb_qp_delta se(v) length depends on
 * cur_qp - prev_qp, so a stale prev_qp misprices trials under mb-tree / AQ and
 * flips mode decisions. */
static int intra_codes_qpd(const y264_frame_t *f, const struct intra_mb *o)
{
    if (!o->use_i4) return 1;                        /* I_16x16 always codes mb_qp_delta */
    if (f->cf_idc == 3)
        return (o->ir.cbp_luma | o->ir_c[0].cbp_luma | o->ir_c[1].cbp_luma) > 0;
    int cbp_luma = o->use_i8 ? o->i8.cbp_luma : o->ir.cbp_luma;
    return (cbp_luma | o->cr.cbp) > 0;
}

/* Advance the raster-order prev_qp chain as an emitted mb_qp_delta would, so the
 * analysis pass's cost trials price se(cur_qp - prev_qp) correctly. */
static void advance_qpd_chain(y264_frame_t *f, int codes_qpd)
{
    if (codes_qpd) { f->last_qp_delta = f->cur_qp - f->prev_qp; f->prev_qp = f->cur_qp; }
    else f->last_qp_delta = 0;
}

/* W0 step 5: advance the prev_qp chain AND record the decoder-visible QPY for the
 * deblock pass (mbqp = prev_qp after the advance = cur_qp if a delta is coded, else
 * the carried prev_qp). Called from pass 1, so f->mbqp is a pure function of the
 * committed records -- independent of the serial entropy emit (mb_qp_post does
 * not write it). Byte-identical to an emit-time fill. */
static void commit_qpy(y264_frame_t *f, int mbx, int mby, int codes_qpd)
{
    advance_qpd_chain(f, codes_qpd);
    if (f->mbqp)
        f->mbqp[mby * f->wmb + mbx] = (uint8_t)f->prev_qp;
}

/* Decision record for one analysed B macroblock. dmv is the direct motion (used
 * by B_Skip and B_Direct commit); the union holds the winner's inter payload
 * (mode 1 = B_Direct residual, mode 2 = B inter) or intra. */
struct b_rec {
    uint8_t mode;                   /* 0 skip, 1 direct, 2 inter, 3 intra */
    struct direct_mv dmv;
    union { struct inter_result ir; struct intra_mb intra; } u;
};

static int b_codes_qpd(const y264_frame_t *f, const struct b_rec *r)
{
    if (r->mode == 0) return 0;                      /* B_Skip: no residual */
    if (r->mode == 3) return intra_codes_qpd(f, &r->u.intra);
    const struct inter_result *ir = &r->u.ir;        /* direct or inter residual */
    return (f->cf_idc == 3 ? ir->cbp444 : (ir->cbp_luma | ir->cr.cbp)) > 0;
}

/* Analyse one B macroblock: RD mode decision (skip / direct / inter / intra),
 * leaving the winner's reconstruction in f->rec and mb_tr8 set. No grid
 * authoring beyond the intra/direct trials' self-restored scratch, no bitstream.
 * Shared by the CABAC single pass and the CAVLC two-pass. */
/* The list-1 16x16 search (single-ref), kept separate from analyze_b_mb so the
 * confirmation path can run it BEFORE the list-0 ref loop instead of after.
 * That order is x264's (the "extremely munged" loop whose whole purpose is
 * list1-ref0, list0-ref0, try skip, then the rest), and it is what lets a
 * confirmed skip abandon list 0's remaining references as well.
 *
 * It is NOT free to reorder, which is why it is conditional: our halfpel
 * threshold is a single per-MB accumulator shared by both lists, where x264
 * keeps one per list (the half-pel threshold). Search order therefore
 * feeds back into qpel gating, and the swap changes the bitstream. Off, the
 * order is untouched and the default stays byte-identical. */
static long search_b_l1(y264_frame_t *f, int mbx, int mby, const pixel *src,
                        int ss, int refs1, int pw, int ph, int mlam, int bsm,
                        int *seed1, int ns1, int pL1[2], int mvL1[2])
{
    mv_predict_f(f, f->mvx1, f->mvy1, f->refidx1, mbx, mby, 0, &pL1[0], &pL1[1]);
    if ((bsm & 8) && f->lr_bseed_mvx1) {   /* lowres pair MV vs list1 */
        int li = mby * f->wmb + mbx;
        seed1[2*ns1] = f->lr_bseed_mvx1[li];
        seed1[2*ns1+1] = f->lr_bseed_mvy1[li]; ns1++;
    }
    /* Staircase: cap the list-1 vertical reach (the reference may still be
 * encoding below row mby+LAG). Every OTHER list-1 MV producer closes over
 * this: spatial direct/skip are medians of already-clamped coded L1 MVs
 * (temporal direct is excluded by the stair gate), so the coded stream
 * never reads past the published rows. */
    if (f->stair_clamp)
        y264_me_set_ymax(f->stair_mvy_max);
    y264_me_set_list(1);                /* list 1 owns its own halfpel threshold */
    long c = y264_me_search(src, ss, f->ref1[0], refs1, pw, ph, mbx * 16, mby * 16,
                            16, 16, pL1[0], pL1[1], mlam, seed1, ns1, &mvL1[0], &mvL1[1]);
    y264_me_set_list(0);
    if (f->stair_clamp)
        y264_me_set_ymax(INT_MAX);
    return c;        /* SATD + mv-rate (qpel refinement scores SATD); the stage-C
 * competitiveness guard reads it, nothing else does */
}

/* Per-MB analysis lambda (Y264_MB_LAMBDA). x264 rebuilds its ME lambda and
 * the RD lambdas from the macroblock QP at analysis entry, so the motion search
 * and RD mode decision use the SAME QP the quantiser will use for that
 * macroblock. We compute both ONCE PER SLICE from the FRAME QP while mb_qp_pre
 * hands the quantiser a per-MB QP from mb-tree and AQ. So every modulated
 * macroblock decides its modes and motion at one rate-distortion tradeoff and
 * is quantised at another.
 *
 * That is exactly the shape of the mb-tree result: our propagation field is
 * +0.82 correlated with x264's and its AQ term +0.998, yet propagation buys us
 * ~0 where it buys x264 9-14% -- and raising the strength makes ours WORSE,
 * because the mismatch grows with the modulation amplitude. */
static int mb_lambda_on(void)
{
    /* DEFAULT 5 since 08-27: mbtree-component lambda on non-flat MBs. Bands
 * -2.06/-2.81 medians with no loser outside noise, ABR side 5/5 negative,
 * wall free; the one payer is CGI animation (bbb +3.14) against the
 * -29.76% lead there. Battery: conformance 518/518, recon_thread_gate,
 * determ_repeat 16/16x12, abr_decode_gate, all armed. Y264_MB_LAMBDA=0
 * restores the frame-level lambda. */
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_MB_LAMBDA"); v = e ? atoi(e) : 5; }
    return v;
}

/* Y264_MB_LAMBDA=<mode>[,<qp0>]: qp0 (default 30) is mode 6's frame-QP floor. */
static int mb_lambda_qp0(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("Y264_MB_LAMBDA");
        const char *c = e ? strchr(e, ',') : NULL;
        v = c ? atoi(c + 1) : 30;
    }
    return v;
}

/* Mode 1: lambda follows the full modulated QP (mb-tree + AQ). Mode 2: the
 * mb-tree component only -- mbtree_off is the COMBINED x264-style offset, so
 * subtracting aq_off recovers the propagation term. The AQ half of the
 * modulation measured as a small lambda-following loss on its own while the
 * mb-tree half carried the whole win, so mode 2 chases that split. Modes 3/4
 * are gated forms of 2, hunting sita's win without bbb's price: 3 engages
 * only on FLAT source MBs (var16x16 below the psy flat gate's 25 threshold),
 * 4 only where the mb-tree component is NEGATIVE (mb-tree invested there;
 * where it taxes, keep the frame lambda). */
/* Returns the QP to derive the analysis lambdas from, or -1 = NOT ENGAGED
 * (the caller must then leave the per-slice lambdas untouched -- returning a
 * QP here instead silently substituted lambda(f->qp) or, worse, mode-1
 * behaviour on the mbtree-less non-ref B frames, and that leak was
 * byte-visible under Y264_MB_LAMBDA=6,99 which should be a no-op). */
static int mb_lambda_qp(const y264_frame_t *f, int mbx, int mby)
{
    int mode = mb_lambda_on();
    if (mode == 1) return f->cur_qp;
    /* Mode 7 = mode 5 plus a non-ref-B leg: a frame with NO mb-tree field but
 * a cascade-raised QP (the flat-B / pyramid-leaf case) decides its modes
 * and motion at the anchor-grade lambda (cur_qp - lambda_casc) while the
 * quantiser keeps the cascade. Built chasing sita's B-half: at the starved
 * band we hold 84% of B MBs in the direct-or-skip bucket vs x264's 79% and
 * code ~25% fewer searched-MV MBs (scripts/b_census.py). REFUSED 08-29: it
 * moves the census toward x264 (direct-or-skip 84.1->83.1, searched-MV
 * 14.5->15.1%) and reads +2.16% BD-NEG at matched rate on that band -- the
 * bought MVs pay fair-to-worse, same as the bought intra did on the P side.
 * The census signature is a symptom, not a recipe. Kept as the probe that
 * records that. */
    if (mode == 7 && !f->mbtree_off && f->lambda_casc > 0) {
        int q = f->cur_qp - f->lambda_casc;
        if (q < 0) q = 0; else if (q > 51) q = 51;
        return q;
    }
    if (mode < 2 || !f->mbtree_off || !f->aq_off) return -1;
    /* Mode 6 = mode 5 with a frame-QP floor: the arm is regime-shaped (deep
 * band uniformly won, standing-band losses all at the qp~22-26 rungs), so
 * engage only at frame QP >= qp0. */
    if (mode == 6 && f->qp < mb_lambda_qp0()) return -1;
    int q = f->cur_qp - f->aq_off[mby * f->wmb + mbx];
    if (q < 0) q = 0; else if (q > 51) q = 51;
    if (mode == 3 || mode == 5 || mode == 6 || mode == 7) {
        /* 3 = flat MBs only; 5 = the inverse, non-flat only. Measured: sita's
 * whole win lives on the NON-flat minority (mode 3 reads +0.98 there,
 * the win gone) while bbb's loss is mostly flat-borne (+5.53 of +6.19),
 * so 5 is the separating gate. */
        const pixel *s = f->src[0] + (mby * 16) * f->src_stride[0] + mbx * 16;
        uint32_t v2[2];
        y264_dsp.var16x16(s, f->src_stride[0], v2);
        int64_t var256sq = (int64_t)v2[1] * 256 - (int64_t)v2[0] * v2[0];
        int isflat = var256sq < (int64_t)25 << 16;
        if (isflat != (mode == 3)) return -1;
    } else if (mode == 4) {
        if (q >= f->qp) return -1;
    }
    return q;
}

/* --- Y264_BPROF: per-stage wall attribution for the B tournament (t1 only,
 * measurement instrument, default off). Stages are cut by BPCUT; bp_stage
 * tracks the open stage so goto exits attribute correctly. Aggregated per
 * final verdict (skip/direct/inter/intra) and printed at process exit. --- */
#include <time.h>
static int bprof_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_BPROF"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
static inline uint64_t bp_now(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#define BP_NSTAGE 7
static const char *bp_name[BP_NSTAGE] =
    { "skip-eval", "me", "direct-rd", "satd-phase", "rd-surv", "intra", "s4-tail" };
static uint64_t bp_ns[4][BP_NSTAGE];   /* [verdict][stage] */
static uint64_t bp_cnt[4][BP_NSTAGE];  /* MBs that spent time in stage */
static uint64_t bp_mbs[4];
/* Shape of the INTER verdicts, so the B tournament can be compared against
 * x264's own "mb B" line (B16..8 / L0 / L1 / BI). Knowing that we skip 49.2%
 * where x264 skips 37.7% says the tournament is mis-tuned; knowing WHICH
 * coded mode we pick instead is what says which B tool is weak.
 * bp_bmode[0..2] = L0 / L1 / Bi; bp_bpart[0..2] = 16x16 / 16x8 / 8x16. */
static uint64_t bp_bmode[3], bp_bpart[3], bp_b8;
/* Y264_BDIR_STAT: the L0/L1/Bi rank, split into distortion and rate. */
static uint64_t bdir_win[3], bdir_dist[3], bdir_rate[3], bdir_n;
static int bdir_stat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_BDIR_STAT"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
/* P side of the two-sided table: same cut style,
 * verdicts skip/inter/intra. subme>=9 attributes its per-partition RD loop
 * to inter-rd (there is no separate SATD phase at that tier). */
#define PP_NSTAGE 6
static const char *pp_name[PP_NSTAGE] =
    { "skip-eval", "probe", "me-satd", "inter-rd", "intra", "s4-tail" };
static uint64_t pp_ns[3][PP_NSTAGE];
static uint64_t pp_cnt[3][PP_NSTAGE];
static uint64_t pp_mbs[3];
/* Y264_PPRUNE_PROBE=1: the CEILING on EXACT pruning of the P tournament.
 *
 * Every P-side arm in this tree is an APPROXIMATION -- P_SKIP_EXIT 1/2/3,
 * PART_EARLYTERM flat and adaptive, RD_ADMIT -- each changes the verdict and
 * each was refused on BD. The untried direction is EXACT: reach the identical
 * verdict with less work, so the gate is md5 rather than a band round.
 *
 * The bound. Every candidate's RD cost is D + lambda*R with D >= 0, so a
 * candidate needing at least R bits costs at least Y264_LAMJ(lam, R). It is
 * provably dead when that exceeds j_skip, which is already in hand before any
 * search runs (analyze_p_mb computes it first and then never uses it as a
 * bound -- the partition searches screen on s16 and intra on best_satd). With
 * j_skip = dist + LAMJ(lam,1) and LAMJ(l,b) = l*b/16, the condition is
 *
 * R > 16*dist/lam + 1 == B
 *
 * so B is skip's distortion expressed in BIT-EQUIVALENTS, and every candidate
 * whose minimum possible rate exceeds B bits can be skipped exactly. B is the
 * whole ceiling in one number: B ~ 5-10 and even P_L0_16x16 at mvd 0 / cbp 0
 * prunes; B ~ 20-40 and only the 8x8 split (mb_type + 4 sub types + 8 mvds)
 * prunes; B in the hundreds and nothing does.
 *
 * Probe only, counts nothing else, default inert. Binned by FINAL verdict and
 * taken only on macroblocks that ran the full tournament (the early-probe
 * commits jump past this point), which is exactly the late-skip class. */
static int pprune_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PPRUNE_PROBE"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
#define PPRUNE_NB 9
static uint64_t g_pprune[2][PPRUNE_NB];      /* [verdict skip?][bit-equivalent bin] */
static const char *pprune_bin[PPRUNE_NB] =
    { "<=4", "5-8", "9-16", "17-32", "33-64", "65-128", "129-256", "257-1k", ">1k" };
static void pprune_note(long j_skip, long dist, long lam, int is_skip)
{
    if (!pprune_on() || lam <= 0 || j_skip >= LONG_MAX / 4)
        return;
    long B = (dist * 16) / lam + 1;          /* the exact bound, in bits */
    int b = B <= 4 ? 0 : B <= 8 ? 1 : B <= 16 ? 2 : B <= 32 ? 3 : B <= 64 ? 4
          : B <= 128 ? 5 : B <= 256 ? 6 : B <= 1024 ? 7 : 8;
    __atomic_fetch_add(&g_pprune[is_skip ? 1 : 0][b], 1, __ATOMIC_RELAXED);
}
static void pprune_dump(void)
{
    if (!pprune_on())
        return;
    fprintf(stderr, "PPRUNE: exact-bound ceiling -- skip distortion in BIT-EQUIVALENTS\n"
                    "        (a candidate needing more than B bits minimum is provably dead)\n");
    for (int v = 1; v >= 0; v--) {
        uint64_t n = 0;
        for (int b = 0; b < PPRUNE_NB; b++) n += g_pprune[v][b];
        if (!n) continue;
        fprintf(stderr, "PPRUNE %-9s n=%-8llu", v ? "verdict=SKIP" : "verdict=CODED",
                (unsigned long long)n);
        for (int b = 0; b < PPRUNE_NB; b++)
            fprintf(stderr, " %s:%.1f%%", pprune_bin[b], 100.0 * (double)g_pprune[v][b] / (double)n);
        fprintf(stderr, "\n");
    }
}
/* Y264_PSKIP_CENSUS=1: the P-tournament census behind plan step B0
 * (plan work item B). Taken only on macroblocks
 * that ran the full tournament on the subme<=8 path (the early-probe commits
 * jump past it), keyed by the FINAL verdict, so the SKIP row is exactly the
 * late-skip class. Default inert, atomic bins, output byte-identical with the
 * knob off (one extra SATD when on, never a decision). The columns:
 *   mv16-smv   L1 qpel distance, ref-0 16x16 winner to the skip MV. x264's
 *              exit fires at <= 1 on ref 0, so that fraction is the ceiling
 *              of arm B1 (the shape-preserving exit).
 *   qualify    ref16 == 0 and L1 <= 1; qual-alt is the subset whose final
 *              verdict a 16x16-only search would change (inter won on a
 *              split or a non-zero ref), i.e. B1's verdict-change population.
 *   jskip/jint j_skip / j_inter: the margin a calibrated RD exit has.
 *   satd marg  (satd(src, skip pred) - the 16x16 SATD winner, mv and mbtype
 *              bits included) / mlam: the margin P_SKIP_EXIT=2 lacked (it
 *              exits when this is < 0).
 *   win part   the inter candidate's partition on inter verdicts; p8-nzref is
 *              how often P_8x8 won with a non-zero ref (arm B2's population);
 *              intra-ran is how often the intra candidate was admitted.
 * The probe's tolerance is not a column: skipdec_p defaults to 0 (strict),
 * so it reads 0 on every P macroblock. */
#define PCEN_MVB 5
#define PCEN_RB  6
#define PCEN_SB  7
static uint64_t pcen_n[3], pcen_mv[3][PCEN_MVB], pcen_ref0[3], pcen_qual[3],
                pcen_qual_alt[3], pcen_ratio[3][PCEN_RB], pcen_satd[3][PCEN_SB],
                pcen_part[3][4], pcen_p8_nz[3], pcen_intra_ran[3];
static void pcen_dump(void)
{
    static const char *vn[3] = { "SKIP", "INTER", "INTRA" };
    static const char *mvb[PCEN_MVB] = { "0", "1", "2", "3-4", "5+" };
    static const char *rb[PCEN_RB] = { "<.9", ".9-.99", ".99-1", "1-1.01", "1.01-1.1", ">=1.1" };
    static const char *sb[PCEN_SB] = { "<-32", "-32..-9", "-8..-1", "0", "1..8", "9..32", ">32" };
    uint64_t tot = pcen_n[0] + pcen_n[1] + pcen_n[2];
    if (!tot) return;
    uint64_t q = pcen_qual[0] + pcen_qual[1] + pcen_qual[2];
    uint64_t qa = pcen_qual_alt[0] + pcen_qual_alt[1] + pcen_qual_alt[2];
    fprintf(stderr, "PCEN: P tournament census, full-tournament MBs only, by FINAL verdict\n"
                    "PCEN  total=%llu skip=%.1f%% inter=%.1f%% intra=%.1f%% | "
                    "qualify(ref0 & L1<=1)=%.1f%% of total, verdict-changing=%.2f%% of total\n",
            (unsigned long long)tot, 100.0 * pcen_n[0] / tot, 100.0 * pcen_n[1] / tot,
            100.0 * pcen_n[2] / tot, 100.0 * q / tot, 100.0 * qa / tot);
    for (int v = 0; v < 3; v++) {
        double n = (double)pcen_n[v];
        if (!pcen_n[v]) continue;
        fprintf(stderr, "PCEN %-5s n=%-8llu mv16-smv:", vn[v], (unsigned long long)pcen_n[v]);
        for (int b = 0; b < PCEN_MVB; b++) fprintf(stderr, " %s:%.1f%%", mvb[b], 100.0 * pcen_mv[v][b] / n);
        fprintf(stderr, " | ref0:%.1f%% qualify:%.1f%% qual-alt:%.2f%%\n",
                100.0 * pcen_ref0[v] / n, 100.0 * pcen_qual[v] / n, 100.0 * pcen_qual_alt[v] / n);
        fprintf(stderr, "PCEN %-5s   jskip/jint:", vn[v]);
        for (int b = 0; b < PCEN_RB; b++) fprintf(stderr, " %s:%.1f%%", rb[b], 100.0 * pcen_ratio[v][b] / n);
        fprintf(stderr, "\nPCEN %-5s   satd-margin/mlam:", vn[v]);
        for (int b = 0; b < PCEN_SB; b++) fprintf(stderr, " %s:%.1f%%", sb[b], 100.0 * pcen_satd[v][b] / n);
        fprintf(stderr, "\nPCEN %-5s   win-part 16x16:%.1f%% 16x8:%.1f%% 8x16:%.1f%% 8x8:%.1f%% "
                        "p8-nzref:%.1f%% intra-ran:%.1f%%\n", vn[v],
                100.0 * pcen_part[v][0] / n, 100.0 * pcen_part[v][1] / n,
                100.0 * pcen_part[v][2] / n, 100.0 * pcen_part[v][3] / n,
                100.0 * pcen_p8_nz[v] / n, 100.0 * pcen_intra_ran[v] / n);
    }
}
static int pcen_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("Y264_PSKIP_CENSUS");
        v = e ? (atoi(e) ? 1 : 0) : 0;
        if (v) atexit(pcen_dump);       /* resolved on the main thread by the warm */
    }
    return v;
}
#define PCEN_ADD(c) __atomic_fetch_add(&(c), 1, __ATOMIC_RELAXED)
static void pcen_note(int mode, long j_skip, long j_inter, long satd16, long satd_skip,
                      int mlam, const struct inter_result *ir0,
                      const struct inter_result *ir, int smvx, int smvy, int intra_ran)
{
    int v = mode < 0 || mode > 2 ? 1 : mode;
    PCEN_ADD(pcen_n[v]);
    int d = labs(ir0->mvx[0] - smvx) + labs(ir0->mvy[0] - smvy);
    PCEN_ADD(pcen_mv[v][d == 0 ? 0 : d == 1 ? 1 : d == 2 ? 2 : d <= 4 ? 3 : 4]);
    int ref0 = ir0->ref[0] == 0;
    if (ref0) PCEN_ADD(pcen_ref0[v]);
    if (ref0 && d <= 1) {
        PCEN_ADD(pcen_qual[v]);
        if (mode == 1 && (ir->part != 0 || ir->ref[0] != 0)) PCEN_ADD(pcen_qual_alt[v]);
    }
    if (j_inter > 0 && j_skip < LONG_MAX / 4) {
        double r = (double)j_skip / (double)j_inter;
        PCEN_ADD(pcen_ratio[v][r < 0.9 ? 0 : r < 0.99 ? 1 : r < 1.0 ? 2
                              : r < 1.01 ? 3 : r < 1.1 ? 4 : 5]);
    }
    if (mlam > 0 && satd16 >= 0) {
        long m = (satd_skip - satd16) / mlam;      /* truncates toward zero */
        PCEN_ADD(pcen_satd[v][m < -32 ? 0 : m < -8 ? 1 : m < 0 ? 2 : m == 0 ? 3
                             : m <= 8 ? 4 : m <= 32 ? 5 : 6]);
    }
    if (mode == 1) {
        PCEN_ADD(pcen_part[v][ir->part & 3]);
        if (ir->part == 3 && (ir->ref[0] | ir->ref[1] | ir->ref[2] | ir->ref[3]))
            PCEN_ADD(pcen_p8_nz[v]);
    }
    if (intra_ran) PCEN_ADD(pcen_intra_ran[v]);
}
#undef PCEN_ADD
static void bp_dump(void)
{
    static const char *vn[4] = { "SKIP", "DIRECT", "INTER", "INTRA" };
    fprintf(stderr, "BPROF: B-MB tournament stage attribution (ms | MBs touched)\n");
    for (int v = 0; v < 4; v++) {
        if (!bp_mbs[v]) continue;
        fprintf(stderr, "BPROF %-6s mbs=%-8llu", vn[v], (unsigned long long)bp_mbs[v]);
        for (int s = 0; s < BP_NSTAGE; s++)
            fprintf(stderr, " %s=%.1f/%llu", bp_name[s], bp_ns[v][s] / 1e6,
                    (unsigned long long)bp_cnt[v][s]);
        fprintf(stderr, "\n");
    }
    uint64_t tot = 0;
    for (int v = 0; v < 4; v++) for (int s = 0; s < BP_NSTAGE; s++) tot += bp_ns[v][s];
    {
        uint64_t nm = bp_bmode[0] + bp_bmode[1] + bp_bmode[2];
        uint64_t np = bp_bpart[0] + bp_bpart[1] + bp_bpart[2] + bp_b8;
        if (nm)
            fprintf(stderr, "BPROF INTER shape: L0 %.1f%% L1 %.1f%% BI %.1f%% | "
                    "16x16 %.1f%% 16x8 %.1f%% 8x16 %.1f%% 8x8 %.1f%%  (x264 prints "
                    "the same split on its 'mb B' line; L0/L1/BI exclude B_8x8, "
                    "whose quadrants each pick their own direction)\n",
                    100.0 * bp_bmode[0] / nm, 100.0 * bp_bmode[1] / nm,
                    100.0 * bp_bmode[2] / nm,
                    np ? 100.0 * bp_bpart[0] / np : 0.0,
                    np ? 100.0 * bp_bpart[1] / np : 0.0,
                    np ? 100.0 * bp_bpart[2] / np : 0.0,
                    np ? 100.0 * bp_b8 / np : 0.0);
    }
    fprintf(stderr, "BPROF total in analyze_b_mb: %.1f ms\n", tot / 1e6);
    if (bdir_n) {
        static const char *dn[3] = { "L0", "L1", "BI" };
        fprintf(stderr, "BDIR over %llu MBs (16x16 SATD rank):\n",
                (unsigned long long)bdir_n);
        for (int i = 0; i < 3; i++)
            fprintf(stderr, "BDIR  %s win %5.1f%%  mean distortion %7.1f  mean mv+ref rate %6.1f\n",
                    dn[i], 100.0 * bdir_win[i] / bdir_n,
                    (double)bdir_dist[i] / bdir_n, (double)bdir_rate[i] / bdir_n);
    }
    static const char *pvn[3] = { "SKIP", "INTER", "INTRA" };
    fprintf(stderr, "PPROF: P-MB tournament stage attribution (ms | MBs touched)\n");
    for (int v = 0; v < 3; v++) {
        if (!pp_mbs[v]) continue;
        fprintf(stderr, "PPROF %-6s mbs=%-8llu", pvn[v], (unsigned long long)pp_mbs[v]);
        for (int s = 0; s < PP_NSTAGE; s++)
            fprintf(stderr, " %s=%.1f/%llu", pp_name[s], pp_ns[v][s] / 1e6,
                    (unsigned long long)pp_cnt[v][s]);
        fprintf(stderr, "\n");
    }
    tot = 0;
    for (int v = 0; v < 3; v++) for (int s = 0; s < PP_NSTAGE; s++) tot += pp_ns[v][s];
    fprintf(stderr, "PPROF total in analyze_p_mb: %.1f ms\n", tot / 1e6);
    fprintf(stderr, "PPART calls 16x16=%ld 16x8=%ld 8x16=%ld 8x8=%ld | searches 16x16=%ld 16x8=%ld 8x16=%ld 8x8=%ld\n",
            y264_pstat_part[0], y264_pstat_part[1], y264_pstat_part[2], y264_pstat_part[3],
            y264_pstat_srch[0], y264_pstat_srch[1], y264_pstat_srch[2], y264_pstat_srch[3]);
    pprune_dump();
}
static void bp_register(void)
{
    static int done = 0;
    if (!done) { done = 1; atexit(bp_dump); }
}

/* E2 stage A clause escapes: ADMIT arms BOTH clauses (that is the design's
 * admission rule); these isolate one for attribution, because a two-clause OR
 * measured only as a whole is the "never price one lever of a group alone"
 * trap pointed the other way. */
static int bskip_admit_nb(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_BSKIP_ADMIT_NB"); v = e ? atoi(e) : 1; }
    return v;
}
static int bskip_admit_mv(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_BSKIP_ADMIT_MV"); v = e ? atoi(e) : 1; }
    return v;
}

/* `recs` is the frame's whole analysis-record array, so stage A can read the
 * LEFT and TOP verdicts: both are complete before this macroblock starts, in
 * raster order trivially and under the wavefront by the same dependency that
 * already lets this function read their coded motion and nnz. Same-frame only
 * -- no cross-frame state, so nothing here touches the settled-bound class. */
static void analyze_b_mb(y264_frame_t *f, int mbx, int mby, int mlam, long lam,
                         int8_t *nzbuf, pixel *snap_best,
                         const struct b_rec *recs, struct b_rec *out)
{
    int bp_on = bprof_env();
    uint64_t bp_loc[BP_NSTAGE] = {0};
    uint64_t bp_last = bp_on ? (bp_register(), bp_now()) : 0;
    int bp_stage = 0;
#define BPCUT(n) do { if (bp_on) { uint64_t _nw = bp_now();                    \
                     bp_loc[bp_stage] += _nw - bp_last; bp_last = _nw; }        \
                     bp_stage = (n); } while (0)
    NLED_SITE(Y264_LED_SITE_BME); NLED(mb_b, 1);
    int ss = f->src_stride[0];
    int refs0 = f->ref_stride[0], refs1 = f->ref1_stride[0];
    int pw = f->padded_w, ph = f->padded_h;
    mb_qp_pre(f, mbx, mby);
    if (mb_lambda_on()) { int lq = mb_lambda_qp(f, mbx, mby);
                          if (lq >= 0) { mlam = lambda_me(lq); lam = lambda_mode16(lq); } }
    y264_me_set_cheap(f->me_cheap);     /* per-frame adaptive-ME flag (TLS) */
    y264_me_reset_hpel_thresh();        /* x264's half-pel threshold: fresh per MB */
    y264_me_set_isb(1);                 /* oracle attribution: B frame */
    y264_me_set_stq(f->stq);
    y264_me_set_et_class(f->slice_is_ref ? 2 : 4);
    me_et_imp_stamp(f, mbx, mby);       /* importance rescue for the ET exits */
    const pixel *src = f->src[0] + (mby * 16) * ss + mbx * 16;

    y264_bs_t sb;
    double best = 0;
    int mode = 0;                    /* 0 skip, 1 direct, 2 inter, 3 intra */
    int bl_path = 0, bl_satd16 = -1; /* Y264_BLATE_STAT: decision path + best
 * searched 16x16 SATD (when it ran) */
    struct inter_result ires, idir, tmp;
    int s4 = trellis_commit_on(f->subme, f->trellis);
    s_rd_trial = s4;                /* S4: deadzone quant in the RD trials below */

    /* --- direct prediction (shared by B_Skip and B_Direct) --- */
    /* temporal_direct returns 0 from INSIDE its four-block loop, so a refusal
 * leaves some entries written and the rest untouched. Two readers below take
 * block 0 without asking whether the derivation succeeded (the staircase's
 * range checks, and the ME seed list), so an uninitialised dmv feeds STACK
 * GARBAGE into the motion search. That is deterministic exactly as long as the
 * worker's stack history is -- and two sibling B leaves sharing the pool's
 * workers (Y264_FPIPE) make it not, which is why per-macroblock temporal
 * direct was irreproducible above one thread. Zero it once: an 84-byte stack
 * clear per B macroblock, against the RD tournament that follows, and it closes
 * the class rather than the one reader that happened to show it. */
    struct direct_mv dmv;
    memset(&dmv, 0, sizeof dmv);
    int tdir_ok = 1;
    if (f->direct_temporal) {
        tdir_ok = temporal_direct(f, mbx, mby, &dmv);
        if (direct_why_on()) { y264_tdir_mb[1]++; y264_tdir_mb[0] += !tdir_ok; }
    } else
        spatial_direct(f, mbx, mby, &dmv);
    /* v5 staircase (Y264_STAIR_BDEPTH): the derived direct list-0 MV is a
 * median that can include neighbours coded against OTHER (unclamped)
 * references, so it may exceed the fixed bound of a still-streaming
 * reference. Such a direct candidate is simply never evaluated -- no MC
 * read past the published rows, and B_Skip/B_Direct lose by construction
 * (the P_Skip guard's shape). Deterministic: a pure function of coded
 * neighbour state, applied by the env gate alone. The LIST-1 side needs no
 * guard -- list 1 is single-ref, so its median is over already-clamped
 * coded MVs (the v1 closure), and only reachable at nref > 1 on list 0. */
    int b8_thresh = 0x7fffffff;     /* B_8x8's RD screen; set by the subme<9 path */
    long bdist_x = LONG_MAX;        /* skip candidate's distortion (mid-tournament exit) */
    int trpre_mb = -1;              /* shared per-MB trial transform size (Y264_TR_PRE_SHARE) */
    int *tp = tr_share_on() ? &trpre_mb : NULL;
    /* DIAG Y264_DIAG_COLWATCH: hash the co-located field at the FIRST and LAST
     * macroblock of a B frame. A prep-time hash proves nothing about a field
     * that is read during the macroblock loop -- if the picture supplying it is
     * still being encoded, the window that matters opens after prep. */
    { static int dg = -1;
      if (dg < 0) { const char *e = getenv("Y264_DIAG_COLWATCH"); dg = e ? atoi(e) : 0; }
      if (dg && ((mbx == 0 && mby == 0) || (mbx == f->wmb - 1 && mby == f->hmb - 1))) {
        size_t n = (size_t)f->mv_stride * f->hmb * 4;
        unsigned long h = 1469598103934665603UL;
        for (size_t i = 0; i < n; i++) {
            unsigned v = (unsigned)(uint16_t)f->colpoc[i]
                       ^ ((unsigned)(uint16_t)f->colmvx[i] << 8)
                       ^ ((unsigned)(uint16_t)f->colmvy[i] << 16);
            h = (h ^ v) * 1099511628211UL;
        }
        fprintf(stderr, "colwatch poc=%d %s h=%016lx\n", f->poc,
                (mbx == 0 && mby == 0) ? "first" : "last ", h);
      } }
    int direct_ok = tdir_ok;
    /* DIAG Y264_DIAG_DIRECTOK=n: force direct unavailable on a FIXED, content-
     * independent set of macroblocks, to test the direct_ok==0 path on its own. */
    { static int dg = -1;
      if (dg < 0) { const char *e = getenv("Y264_DIAG_DIRECTOK"); dg = e ? atoi(e) : 0; }
      if (dg > 0 && ((mby * f->wmb + mbx) % dg) == 0) direct_ok = 0; }
    if (f->stair_clamp0_poc[0] >= 0)        /* packed: [0] empty = set empty */
        for (int b = 0; b < 4; b++)
            if (dmv.refL0[b] >= 0 && stair_l0_clamp(f, dmv.refL0[b]) &&
                dmv.mvL0[b][1] > f->stair_mvy_max)
                direct_ok = 0;
    /* The list-1 twin, and it exists only for TEMPORAL direct. Spatial's list-1
 * vector is a median over already-clamped coded MVs, so the clamp closes over
 * it (the v1 closure). Temporal's is synthesised as mvL0 - mvCol out of another
 * picture's motion field, which no closure bounds, so it takes the same
 * explicit test the list-1 SEARCHES are held to. Without this the staircase
 * cannot run with temporal direct at all -- see stair_tdir_on in encoder.c. */
    if (f->direct_temporal && f->stair_clamp && dmv.refL1 >= 0)
        for (int b = 0; b < 4; b++)
            if (dmv.mvL1[b][1] > f->stair_mvy_max)
                direct_ok = 0;
    /* The deferred B-skip confirmation. bconf arms it; try_skip carries the
 * tolerant probe's held answer across the searches; skor_post is the same
 * exit driven by the recorded oracle instead of the real test, which is how
 * the arm gets a ceiling before it gets a threshold. */
    int bconf = f->bskip_confirm;
    int skor_post = y264_skor_at_post() && y264_skor_mode() == 2;
    int try_skip = 0;
    int fs_pend = 0, fs_e = 0, fs_d = 0, fs_rdm = 0;  /* Y264_FLATSKIP_STAT bookkeeping */
    pixel dp[256], dcp[2][256];
    if (direct_ok) {
        build_direct_pred(f, mbx, mby, &dmv, dp, dcp);
        if (direct_score_on()) {        /* measurement: prediction error only */
            y264_dscore_ssd += ssd_block(f->src[0] + (mby * 16) * f->src_stride[0]
                                         + mbx * 16, f->src_stride[0], dp, 16, 16, 16);
            y264_dscore_n++;
        }
        if ((direct_score_on() >= 2 || (y264_mb_direct_auto_on() && f->direct_auto))
            && f->direct_alt_ok) {
            /* Both modes' skippability, x264's signal. The probe reads its
 * prediction out of rec, so each arm writes rec and the caller's
 * content is restored before anything downstream sees it. */
            pixel snap_ds[384];
            struct direct_mv alt;
            pixel adp[256], adcp[2][256];
            save_mb_rec(f, mbx, mby, snap_ds);
            for (int m = 0; m < 2; m++) {   /* 0 = temporal, 1 = spatial */
                if (m == (f->direct_temporal ? 0 : 1)) {
                    store_pred_rec(f, mbx, mby, dp, dcp);
                } else {
                    if (m) spatial_direct(f, mbx, mby, &alt);
                    else if (!temporal_direct(f, mbx, mby, &alt)) continue;
                    build_direct_pred(f, mbx, mby, &alt, adp, adcp);
                    store_pred_rec(f, mbx, mby, adp, adcp);
                }
                int sk = probe_skip(f, mbx, mby, 1, 0) ? 1 : 0;
                y264_dscore_skip[m] += sk;
                if (f->dauto_acc)
                    __atomic_fetch_add(&f->dauto_acc[m], (long)sk, __ATOMIC_RELAXED);
            }
            load_mb_rec(f, mbx, mby, snap_ds);
        }

        /* B_Skip: reconstruct from the direct prediction, no residual. */
        store_pred_rec(f, mbx, mby, dp, dcp);
        long bdist = dist_mb(f, mbx, mby);
        bdist_x = bdist;                /* hoisted for the mid-tournament exit */
        best = bdist
             + (cabac_rd_on() && f->cabac
                ? Y264_LAMJD(lam, est_b_skip_bits(f, mbx, mby) / 256.0)
                : Y264_LAMJD(lam, 1));
        save_mb_rec(f, mbx, mby, snap_best);

        /* Early B_Skip (subme<=8): if the direct residual is negligible under a
 * round-to-nearest test, commit it before any ME / RD analysis. Stricter
 * than P_Skip's deadzone probe -- direct MVs are guesses. */
        /* skipdec_b routes B through the P path's deadzone + trellis + decimate
 * acceptance instead of round-to-nearest. x264 runs ONE probe for both
 * (the B skip probe is probe_skip with the bidirectional form), so the strict B test is
 * ours alone -- and B is where the late skips are (28-41% of all B
 * macroblocks, against 6-34% of P). */
        int bdec = f->skipdec_b;
        /* Lambda-scaled admission gate, x264's other half on P, and the
 * its RD-level B test, which is this same shape:
 * i_bskip_cost <= (6*i_lambda2 + 128) >> 8 against ssd_mb. Its job here
 * is not to save probe effort -- our probe already runs before ME -- but
 * to refuse the TOLERANCE where the direct prediction is visibly poor.
 * That is where the lowres MV confirmation is least trustworthy, and it
 * is the same place the corpus put the cost: mobile +2.92% and tempete
 * +1.99% at the high band, both detail clips. */
        if (bdec && f->skip_costgate && bdist > Y264_LAMJ(lam, f->skip_costgate))
            bdec = 0;
        if (bdec && f->skip_mvagree_b) {
            /* Both lists must agree, as in the B 16x16 analysis: it clears
 * try_skip on the list-1 disagreement and only commits once list 0
 * agrees too. Direct MVs are per-4x4; block 0 stands for the
 * partition the way x264's direct_mv[l][0] does. */
            int i = mby * f->wmb + mbx;
            if (!f->lr_bseed_mvx0 || !f->lr_bseed_mvx1)
                bdec = 0;
            else if (!mv_agrees(f->lr_bseed_mvx0[i], f->lr_bseed_mvy0[i],
                                dmv.mvL0[0][0], dmv.mvL0[0][1], f->skip_mvagree_b)
                  || !mv_agrees(f->lr_bseed_mvx1[i], f->lr_bseed_mvy1[i],
                                dmv.mvL1[0][0], dmv.mvL1[0][1], f->skip_mvagree_b))
                bdec = 0;
        }
        /* x264's deferred-skip flag: ONE probe, and at medium its
 * answer is recorded rather than acted on. The graded form splits that
 * answer in two. An all-zero residual owes nobody anything -- the coder
 * emits no coefficients, so there is no tolerance being spent and the
 * skip commits here, as it does today. A residual that only passed
 * BECAUSE a block was forgiven is the case x264 defers, and it is
 * deferred here to the post-search confirmation below.
 *
 * The bconf arm therefore replaces the strict probe rather than running
 * after it. Two probes make the gate a net loss at the high point
 * before it skips anything; see probe_skip_g. */
        /* E2 stage A: pre-ME admission. The lookahead already estimated this
 * macroblock's motion against both anchors; where its pair MVs land on
 * the direct MVs, a skip is plausible enough to be worth the probe's
 * DCT of sixteen blocks plus chroma. Where they do not, the probe is
 * overwhelmingly going to say no and we have paid for the answer.
 *
 * This gates the PROBE, not the skip -- everything declined here still
 * runs the full tournament and can still leave by the shipped
 * mid-tournament exit. It does change bits, because a macroblock that
 * is never probed is never early-committed, so it is a pure function of
 * per-frame state read in raster order and carries no new plumbing. */
        int admitted = 1;
        if (f->bskip_admit) {
            int i = mby * f->wmb + mbx;
            /* (left AND top skipped): the measured admission signal -- it
 * catches 55-68% of late skips while shrinking the probed
 * population 2.5-4x (Y264_BLATE_STAT). Border macroblocks
 * simply fail the clause; the MV clause still speaks for them. */
            int nb = bskip_admit_nb() && mbx > 0 && mby > 0 &&
                     recs[mby * f->wmb + mbx - 1].mode == 0 &&
                     recs[(mby - 1) * f->wmb + mbx].mode == 0;
            int mv = bskip_admit_mv() &&
                     f->lr_bseed_mvx0 && f->lr_bseed_mvx1 &&
                     mv_agrees(f->lr_bseed_mvx0[i], f->lr_bseed_mvy0[i],
                               dmv.mvL0[0][0], dmv.mvL0[0][1], f->bskip_admit) &&
                     mv_agrees(f->lr_bseed_mvx1[i], f->lr_bseed_mvy1[i],
                               dmv.mvL1[0][0], dmv.mvL1[0][1], f->bskip_admit);
            admitted = nb || mv;
            if (b8_stat_on()) { if (nb) bxa_nb++; if (mv) bxa_mv++; }
        }
        if (b8_stat_on()) { b8_stat_register(); bxa_reach++; if (admitted) bxa_admit++; }
        if (f->subme <= 8) {
            if (f->bskip_probe) {
              /* Stage A declining means NO probe runs at all -- not the strict
 * one either. Falling through to the `else` would pay a probe for
 * every declined macroblock and leave the arm exactly the net
 * loss it already is. */
              if (admitted) {
                int tolerated = 0;
                if (b8_stat_on()) bxa_probe++;
                if (probe_skip_g(f, mbx, mby, 0, f->bskip_dec, &tolerated)) {
                    if (!tolerated) { NLED(mb_b_early, 1); mode = 0; bl_path = 1; goto b_decided; }
                    /* Term 2 only: without a confirmation armed, a forgiven
 * block is simply not skipped -- that arm is refused.
 *
 * skip_costgate is x264's THIRD gate term, not a tuned
 * guard: its P gate will not believe
 * probe_pskip unless the macroblock is also cheap
 * (SATD without the MV rate < 300*the ME lambda), and the B path's
 * its RD-level branch attests ssd_mb against a
 * lambda2-scaled bound for the same reason. A confirmed MV
 * says the MOTION is right; it says nothing about whether
 * the residual mattered, which is why MV confirmation
 * alone costs mobile +2.81% and tempete +3.54% at the
 * HIGH band where residual detail is what the bits buy. */
                    if (bconf && (!f->skip_costgate ||
                                  bdist <= Y264_LAMJ(lam, f->skip_costgate))) {
                        try_skip = 1; NLED(mb_b_try, 1);
                        if (b8_stat_on()) bxa_held++;
                    }
                }
              }
            } else if (probe_skip(f, mbx, mby, bdec ? 0 : 1, bdec)) {
                NLED(mb_b_early, 1); mode = 0; bl_path = 1; goto b_decided;
            } else if (flatskip_stat_on()) {
                /* Escaped: it will now pay the full tournament. te_src4 is the
 * source texture energy dist_mb just memoed for this MB. */
                fs_pend = 1;
                fs_e = fs_bucket(f->te_mbx == mbx && f->te_mby == mby
                                 ? f->te_src4 : 0, FS_EB);
                fs_d = fs_bucket(bdist_x, FS_DB);
                fs_rdm = 0;
                for (int q = 0; q < 4; q++)
                    if (bdist_x <= Y264_LAMJ(lam, fs_rdbits[q])) fs_rdm |= 1 << q;
            }
        }
        if (!y264_skor_at_post() &&
            y264_skor_mode() == 2 && y264_skor_ask(f->skor_key, 1, mbx, mby, f->wmb)) {
            mode = 0; goto b_decided;       /* measurement bound; see skiporacle.h */
        }
    } else {
        best = 1e30;                 /* an inter mode always scores below this
 * (the SATD argmin always survives the RD
 * threshold); the intra admission below
 * carries the belt-and-braces fallback so
 * a mode whose recon was never built can
 * never win. */
    }

    /* List-0 motion search over all active references (ref bits in the cost);
 * list 1 stays single-ref. Bi reuses the winning L0 ref. Seed each list's
 * search with its spatial-neighbour MVs and the temporal direct MV (already
 * POC-scaled in dmv) -- on high-motion B frames the true MV is far from the
 * median but near the collocated MV, so this is the motion-quality lever the
 * P path's seeds gave (x264 feeds the same temporal predictor into B ME). */
    /* Up to 3 spatial neighbours + 1 direct MV + 1 per-ref temporal seed = 5
 * entries (10 ints) on L0; size to 16 like the P path so the temporal add in
 * the ref loop can never run past the end (an overflow here corrupts the B
 * recon). */
    BPCUT(1);
    int seed0[16], ns0 = 0, seed1[16], ns1 = 0;
    int bsm = b_seeds_on(); if (bsm == 1) bsm = 15;  /* 1=all; bit0 L0sp, bit1 L1sp,
 * bit2 tmp, bit3 lowres pair */
    if (bsm) {
        int nbx = mbx * 4, nby = mby * 4;
        if (bsm & 1) {
            mv_nb_t n0[3] = { nb_at_f(f, f->mvx, f->mvy, f->refidx, nbx - 1, nby),
                              nb_at_f(f, f->mvx, f->mvy, f->refidx, nbx, nby - 1),
                              nb_at_f(f, f->mvx, f->mvy, f->refidx, nbx + 4, nby - 1) };
            for (int k = 0; k < 3; k++)
                if (n0[k].avail) { seed0[2*ns0]=n0[k].mvx; seed0[2*ns0+1]=n0[k].mvy; ns0++; }
            /* Only when a direct MV was actually derived: an unavailable
 * temporal direct has no predictor to offer, and the zeroed placeholder
 * is not a free stand-in -- the seed list's LENGTH feeds the search's
 * dedup and tie-breaking, so keeping it moves bits on about half of a
 * CIF/720p x bframes x threads matrix. There is no prior behaviour to
 * preserve either way (this seed was undefined until the memset above),
 * and spatial always derives, so the shipped default is untouched. */
            if (tdir_ok) {
                seed0[2*ns0] = dmv.mvL0[0][0]; seed0[2*ns0+1] = dmv.mvL0[0][1]; ns0++;
            }
        }
        if (bsm & 2) {
            mv_nb_t n1[3] = { nb_at_f(f, f->mvx1, f->mvy1, f->refidx1, nbx - 1, nby),
                              nb_at_f(f, f->mvx1, f->mvy1, f->refidx1, nbx, nby - 1),
                              nb_at_f(f, f->mvx1, f->mvy1, f->refidx1, nbx + 4, nby - 1) };
            for (int k = 0; k < 3; k++)
                if (n1[k].avail) { seed1[2*ns1]=n1[k].mvx; seed1[2*ns1+1]=n1[k].mvy; ns1++; }
            if (tdir_ok) {
                seed1[2*ns1] = dmv.mvL1[0][0]; seed1[2*ns1+1] = dmv.mvL1[0][1]; ns1++;
            }
        }
    }
    int nsp0 = ns0;
    int pL0[2] = {0,0}, pL1[2], mvL0[2] = {0,0}, mvL1[2], r0 = 0;
    long bestc = -1;
    /* Reorder only when something can commit on it; see search_b_l1 on why the
 * swap is not free. */
    int reorder = (bconf && try_skip) || y264_skor_at_post() == 2;
    long cL1 = LONG_MAX;            /* L1 ref-0 cost, for stage C's guard */
    if (reorder)
        cL1 = search_b_l1(f, mbx, mby, src, ss, refs1, pw, ph, mlam, bsm,
                          seed1, ns1, pL1, mvL1);
    for (int r = 0; r < f->nref; r++) {
        int px, py, tx, ty, tsx, tsy;
        mv_predict_f(f, f->mvx, f->mvy, f->refidx, mbx, mby, r, &px, &py);
        ns0 = nsp0;
        if ((bsm & 4) && temporal_seed(f, mbx, mby, r, &tsx, &tsy)) {
            seed0[2*ns0] = tsx; seed0[2*ns0+1] = tsy; ns0++;
        }
        /* Lowres pair MV of this MB vs list0 (x264 <reference-source> ref16x16's
 * the lowres MVs seed, ref0 only): the current-B motion the lookahead
 * measured directly, which tracks zoom/divergent motion the spatial
 * and collocated predictors miss. */
        if (r == 0 && (bsm & 8) && f->lr_bseed_mvx0) {
            int li = mby * f->wmb + mbx;
            seed0[2*ns0] = f->lr_bseed_mvx0[li];
            seed0[2*ns0+1] = f->lr_bseed_mvy0[li]; ns0++;
        }
        if (stair_l0_clamp(f, r))       /* v5: list-0 = the in-flight ref B */
            y264_me_set_ymax(f->stair_mvy_max);
        long c = y264_me_search(src, ss, f->refs[r][0], refs0, pw, ph,
                                mbx * 16, mby * 16, 16, 16, px, py, mlam,
                                seed0, ns0, &tx, &ty)
               + (long)mlam * ref_bits(r, f->nref);
        if (stair_l0_clamp(f, r))
            y264_me_set_ymax(INT_MAX);
        if (bestc < 0 || c < bestc) {
            bestc = c; r0 = r;
            mvL0[0] = tx; mvL0[1] = ty; pL0[0] = px; pL0[1] = py;
        }
        /* The confirmation, at x264's exact point: ref 0
 * of each list has now been searched for real, so the tolerance the
 * probe applied has something to be safe against. Committing here
 * abandons list 0's refs 1..n-1, Bi, the direct RD, the subpartitions,
 * intra and the whole RD stage. Compare ref 0's OWN result (tx,ty), not
 * the running best -- x264 tests each list's 16x16 MV right after ref 0, when
 * the two are the same thing; ours is a best-of-all-refs accumulator. */
        if (r == 0 && reorder && direct_ok &&
            (skor_post ? y264_skor_ask(f->skor_key, 1, mbx, mby, f->wmb)
                       : (mv_agrees(tx, ty, dmv.mvL0[0][0], dmv.mvL0[0][1], bconf) &&
                          mv_agrees(mvL1[0], mvL1[1], dmv.mvL1[0][0], dmv.mvL1[0][1], bconf)
                          && bskip_cguard_ok(f, mbx, mby, src, ss, dp, c, cL1,
                                             bdist_x, lam)))) {
            NLED(mb_b_early, 1); NLED(mb_b_conf, 1); mode = 0;
            if (b8_stat_on()) bxa_commit++;
            goto b_decided;
        }
    }
    if (!reorder) {
        search_b_l1(f, mbx, mby, src, ss, refs1, pw, ph, mlam, bsm,
                    seed1, ns1, pL1, mvL1);
        /* The same bound at the unreordered exit point: both lists are searched,
 * nothing downstream has run. Keeps the byte-identity proof. */
        if (skor_post && direct_ok &&
            y264_skor_ask(f->skor_key, 1, mbx, mby, f->wmb)) {
            mode = 0; goto b_decided;
        }
    }

    /* B_Direct_16x16: direct prediction plus residual. */
    BPCUT(2);
    struct qp_chain qc; qp_save(f, &qc);
    double j;
    if (direct_ok) {
    idir.part = 0;
    idir.bpart = 0;
    g_res_site = RES_SITE_BDIR;
    encode_inter_res_tp(f, mbx, mby, dp, dcp, 1, &idir, lam, 0, tp);
    g_res_site = RES_SITE_OTHER;
    if (cabac_rd_on() && f->cabac) {
        j = dist_mb(f, mbx, mby) + Y264_LAMJD(lam, est_b_bits(f, mbx, mby, &idir, 1) / 256.0);
        escr(4, f, mbx, mby, dist_mb(f, mbx, mby), j,
             Y264_LAMJD(lam, est_bits_lb(f, &idir) / 256.0));
    } else {
        save_mb_nnz(f, mbx, mby, nzbuf);
        y264_bs_init_count(&sb);        /* pricing only */
        y264_bs_write_ue(&sb, 0);        /* mb_type B_Direct_16x16 */
        write_inter_residual(&sb, f, mbx, mby, &idir);
        j = dist_mb(f, mbx, mby) + Y264_LAMJD(lam, y264_bs_pos_bits(&sb));
        load_mb_nnz(f, mbx, mby, nzbuf);
    }
    qp_load(f, &qc);
    if (j < best) { best = j; mode = 1; save_mb_rec(f, mbx, mby, snap_best); }
    }

    /* B 16x16 (L0/L1/Bi) + two-partition (16x8/8x16) inter modes.
 *
 * Threshold-survivor path (medium, subme<=8): SATD-rank all inter modes, then
 * full-RD only those within thresh of the best inter SATD.
 * Otherwise (subme>=9, byte-identical to HEAD): RD all three 16x16 modes and,
 * for the two-partition splits, the SATD-collapsed winner (b_satd_gate). */
    BPCUT(3);
    long b_isatd = -1;              /* W-B (S1): best inter SATD, for the intra screen */
    int bexit_ok = 0;               /* mid-tournament skip exit precondition, set below */
    /* B_8x8 state, hoisted: on the threshold path the SEARCH runs before the
 * rectangular one so its per-quadrant SATDs can estimate that one's cost;
 * the RD trial still happens with the other candidates'. */
    struct inter_result i8;
    struct b8_parts b8p;
    long sum8 = LONG_MAX;
    int b8_have = 0;
    int b8_gated = 0;               /* Y264_B8_QGATE declined this macroblock */
    if (b8_stat_on() && b_8x8_on()) { b8_stat_register(); b8s_mb++; }
    if (b_thresh_on(f->subme)) {
        /* --- SATD phase: build each mode's prediction ONCE (16x16 with a shared
 * cache so Bi reuses the L0/L1 blocks, exactly HEAD's MC cost for the three
 * modes) and rank by luma SATD + mv-rate. Keep the built preds so the RD
 * phase reuses them -- no re-MC. --- */
        struct bpred_cache bpc;
        pixel p16[3][256], c16[3][2][256];
        int satd16[3], rref = ref_bits(r0, f->nref);
        int satd16min = 0x7fffffff;      /* best 16x16-level SATD (incl mv-rate) */
        int bw16 = 0;                    /* .. and which mode holds it (QGATE) */
        long isi_pure = 0x7fffffff;      /* same winner, DISTORTION only (intra screen) */
        int bd_d[3], bd_r[3];            /* Y264_BDIR_STAT: distortion vs rate */
        for (int bm = 0; bm < 3; bm++) {
            build_bpred(f, mbx, mby, bm, r0, mvL0[0], mvL0[1], mvL1[0], mvL1[1],
                        p16[bm], c16[bm], &bpc);
            int d = satd_block(src, ss, p16[bm], 16, 16, 16);
            int mr = b16_mv_rate(bm, mvL0, mvL1, pL0, pL1, rref, mlam);
            if (bmb_cost_on()) mr += mlam * (bm == 2 ? 5 : 3);
            if (bm == 2) mr += mlam * bbi_pen();
            bd_d[bm] = d; bd_r[bm] = mr;
            satd16[bm] = d + mr;
            if (satd16[bm] < satd16min) { satd16min = satd16[bm]; bw16 = bm; }
            if (d < isi_pure) isi_pure = d;
        }
        bl_satd16 = satd16min;
        /* Why L1-alone is chosen half as often as x264 chooses it: split each
 * direction's rank into the part the SEARCH earns (distortion) and the
 * part the COST MODEL charges (mv + ref bits). If L1's distortion is
 * competitive and it still loses, the asymmetry is in the rate. */
        if (bdir_stat_on()) {
            int w = 0;
            for (int bm = 1; bm < 3; bm++) if (satd16[bm] < satd16[w]) w = bm;
            bdir_win[w]++;
            for (int bm = 0; bm < 3; bm++) {
                bdir_dist[bm] += bd_d[bm];
                bdir_rate[bm] += bd_r[bm];
            }
            bdir_n++;
        }
        struct bpart_mo mo_s[3];
        pixel ppt[3][256], cpt[3][2][256];
        int ptsatd[3] = { 0, 0x7fffffff, 0x7fffffff }, ptcombo[3] = { 0, 0, 0 };
        /* B_8x8 first, so 16x8/8x16 can be ESTIMATED from its quadrants rather
 * than searched cold. This is x264's order and the reason its rectangular
 * modes are affordable at medium: four cold searches per macroblock is what
 * makes ours cost 26-38% wall
 * for -0.79% BD, which is why they are off. */
        /* --- direct's SATD, the screen the 16x16 modes ride, and their RD, all
 * BEFORE any subpartition analysis. That is x264's order (b16x16, the
 * direct-competitiveness test, the first B RD stage, the B_SKIP
 * return, and only then the B 8x8 analysis) and the reason it
 * matters is the return: a macroblock that leaves here has cost us
 * nothing for the splits, instead of paying for the full quadrant
 * search and the rectangular ones and then throwing them away --
 * 19-30% of all the quadrant searches on the board's clips
 * (Y264_B8_STAT).
 *
 * The screen here is the 16x16 modes' own (x264's `i_cost` at its
 * first b_rd call is the best of direct / L0 / L1 / Bi and nothing
 * else). Folding the rectangular SATDs in only ever tightens it. --- */
        int dsatd = direct_ok ? satd_block(src, ss, dp, 16, 16, 16) /* direct anchors */
                              : 0x7fffffff;                        /* .. unless barred */
        int isi = (direct_ok && bmb_cost_on()) ? dsatd + mlam : dsatd;
        /* x264's precondition on the early B_SKIP commit:
 * only trust it where the direct prediction is SATD-competitive with
 * the best searched 16x16 mode. Without this the exit over-skips
 * at the ABR band's low rates (akiyo +2.21%,
 * park_joy +1.56%), which is where the SATD ranking is least
 * reliable and a non-survivor mode's lost RD chance costs most. */
        if (direct_ok && (long)isi <= (long)satd16min * 33 / 32)
            bexit_ok = 1;
        if (direct_ok && dsatd < isi_pure) isi_pure = dsatd;  /* direct carries no mv rate */
        for (int bm = 0; bm < 3; bm++) if (satd16[bm] < isi) isi = satd16[bm];
        int thresh16 = isi * (17 + (f->psy_rd > 0.0f ? 1 : 0)) / 16 + 1;

        BPCUT(4);
        for (int bm = 0; bm < 3; bm++) {
            if (satd16[bm] >= thresh16) continue;
            j = rd_b_mode(f, mbx, mby, bm, r0, mvL0, mvL1, pL0, pL1,
                          p16[bm], c16[bm], lam, &tmp, tp);
            if (bm == 2 && bbi_rd_pen()) j += Y264_LAMJD(lam, bbi_rd_pen());
            if (j < best) { best = j; mode = 2; ires = tmp; save_mb_rec(f, mbx, mby, snap_best); }
        }
        /* x264's B_SKIP return. The outer copy below still guards the
 * subme>=9 branch, which never reaches this one -- so THIS is the live
 * check on the shipped path and any bound belongs here. */
        if (b8_stat_on()) {
            b8_stat_register(); bx_reach++;
            if (bexit_ok) bx_exitok++;
            if (f->slice_is_ref) bx_isref++;
            if (mode == 0) bx_mode0++;
            if (bexit_ok && mode == 0 && f->slice_is_ref) {
                bx_refblocked++;
                if (bx_ref_admit(f, mbx, mby, bdist_x)) bx_refadmit++;
            }
        }
        if (b_skip_exit_env() && bexit_ok && mode == 0 && f->subme <= 8 &&
            (!f->slice_is_ref || bx_ref_admit(f, mbx, mby, bdist_x))) {
            if (b8_stat_on()) bx_taken++;
            bl_path = 2;
            goto b_decided;
        }

        BPCUT(3);
        long b8_est[3] = { 0, LONG_MAX, LONG_MAX };   /* [1] = 16x8, [2] = 8x16 */
        int b8_want = b_8x8_on();
        if (b8_want && b8_qgate()) {
            /* Four satd8x8 of the winning 16x16 prediction decide whether the
 * eight quadrant searches are worth running: an evenly spread
 * residual means one motion fits whole macroblock. */
            const pixel *wp = p16[bw16];
            long q[4], qsum = 0, qmax = 0;
            for (int b = 0; b < 4; b++) {
                int ox = (b & 1) * 8, oy = (b >> 1) * 8;
                q[b] = y264_dsp.satd8x8(src + oy * ss + ox, ss, wp + oy * 16 + ox, 16);
                qsum += q[b];
                if (q[b] > qmax) qmax = q[b];
            }
            if (qmax * 4 * 16 <= qsum * (16 + b8_qgate())) {
                b8_want = 0; b8_gated = 1;
                if (b8_stat_on()) b8s_gated++;
            }
        }
        if (b8_want) {
            if (b8_stat_on()) { b8_stat_register(); b8s_search++; }
            memset(&i8, 0, sizeof i8);
            sum8 = search_b_8x8(f, mbx, mby, mlam, &dmv, direct_ok, dp,
                                mvL0, r0, mvL1, &i8, &b8p);
            b8_have = 1;
            /* x264's estimate: each rectangular partition is the better of its
 * two quadrants' L0 / L1 / Bi sums, charged the AVERAGE of their
 * motion rates (one partition codes one mv per list, not two). */
            int estdir[2][2] = { { 0, 0 }, { 0, 0 } };   /* [split][partition] */
            for (int i = 0; i < 2; i++) {
                int q16[2] = { 2 * i, 2 * i + 1 };      /* 16x8 partition i */
                int q8[2]  = { i, i + 2 };              /* 8x16 partition i */
                const int *qq[2] = { q16, q8 };
                for (int sp = 0; sp < 2; sp++) {
                    const int *q = qq[sp];
                    long r0a = ((long)b8p.rate[0][q[0]] + b8p.rate[0][q[1]] + 1) >> 1;
                    long r1a = ((long)b8p.rate[1][q[0]] + b8p.rate[1][q[1]] + 1) >> 1;
                    long c0 = (long)b8p.satd[0][q[0]] + b8p.satd[0][q[1]] + mlam * r0a;
                    long c1 = (long)b8p.satd[1][q[0]] + b8p.satd[1][q[1]] + mlam * r1a;
                    long cb = (long)b8p.satd[2][q[0]] + b8p.satd[2][q[1]]
                            + mlam * (r0a + r1a);
                    long b = c0; int bd = 0;
                    if (c1 < b) { b = c1; bd = 1; }
                    if (cb < b) { b = cb; bd = 2; }
                    estdir[sp][i] = bd;
                    if (b8_est[sp + 1] == LONG_MAX) b8_est[sp + 1] = 0;
                    b8_est[sp + 1] += b;
                }
            }
            /* x264 charges the split's own mb_type on the estimate before it is
 * compared with the running best (<reference-internal>, added to
 * i_cost_est*[1] so the pair is charged once). Without it the
 * rectangular estimate is the only candidate in the comparison
 * carrying no side-information term, which is why arming
 * Y264_B8_RATE alone sends the admitted rectangular searches UP. */
            if (b8_rate_on()) {
                /* mb_type bit cost of each (dir0, dir1) 16x8 split, indexed
 * [dir0*3 + dir1] with dir 0/1/2 = L0/L1/Bi. These are not a
 * tuned proxy: they are the exp-Golomb lengths of the mb_type
 * codes the syntax actually emits. Table 7-14 assigns the 16x8
 * variants 4 (L0,L0), 8 (L0,L1), 12 (L0,Bi), 10 (L1,L0),
 * 6 (L1,L1), 14 (L1,Bi), 16 (Bi,L0), 18 (Bi,L1), 20 (Bi,Bi),
 * and ue(v) codes value v in 2*floor(log2(v+1))+1 bits -- so
 * the row falls out as 5/7/7, 7/5/7, 9/9/9. */
                static const uint8_t B16X8_MBTYPE[9] = { 4, 8, 12, 10, 6, 14, 16, 18, 20 };
                uint8_t b16x8_cost[9];
                for (int t = 0; t < 9; t++) {
                    unsigned n = (unsigned)B16X8_MBTYPE[t] + 1u, lg = 0;
                    while (n >>= 1) lg++;
                    b16x8_cost[t] = (uint8_t)(2 * lg + 1);
                }
                for (int sp = 0; sp < 2; sp++)
                    if (b8_est[sp + 1] != LONG_MAX)
                        b8_est[sp + 1] += (long)mlam *
                            b16x8_cost[estdir[sp][0] * 3 + estdir[sp][1]];
            }
        }
        /* Orientation early-terminate (x264's B 16x8/8x16 analysis
 * gating): search the first split (16x8); only search the second (8x16)
 * when the first came within thresh of the best 16x16-level SATD -- i.e.
 * a split is competitive for this MB. When the first split lost outright
 * to 16x16, the orthogonal split almost never wins, so x264 skips its
 * refine there too. bpo is the /16 slack (18 ~ 1.125x); 0 disables (both
 * orientations always searched = byte-identical). Off at subme>=9. */
        int bpo = bpo_env();
        int obound = (bpo && f->subme < 9) ? (int)((long)satd16min * bpo / 16) : 0x7fffffff;
        /* B 16x8/8x16 OFF at the medium tier: the rect searches are ~2/3 of
 * all subpel probe pixels (4 full hex+subpel searches per B MB before
 * gating), and the A/B measures 1.14-1.18x
 * pure-C wall for BD -0.26% VMAF-NEG (BETTER, 6/7 clips improve) -- the
 * modes' mv/mb-type overhead outweighed their prediction gain at this
 * operating point. x264 medium reaches the same place via its b8x8-based
 * early_terminate estimates, which yah264 lacks (no B_8x8 coding).
 * Y264_B_RECT=1 restores the searches; subme>=9 keeps them (max-quality
 * tournament, byte-identical). */
        long b8_best = satd16min;               /* running best, x264's i_cost */
        if (b8_have && sum8 < b8_best) b8_best = sum8;
        /* A macroblock the quadrant gate declined wants no split at all, so it
 * declines the rectangular ones too -- and it has to: without the 8x8
 * analysis their estimates do not exist and they would run COLD, which
 * is the form that priced them at 26-38% wall. */
        int rect_ok = b_rect_on() && !b8_gated;
        for (int bp = rect_ok ? 1 : 3; bp <= 2; bp++) {
            if (bp == 2 && ptsatd[1] >= obound) break;   /* 16x8 lost -> skip 8x16 */
            /* x264: only search a rectangular split whose ESTIMATE already beats
 * the best cost so far. Inert when B_8x8 is off (no estimate). */
            if (b8_est[bp] != LONG_MAX && b8_est[bp] >= b8_best) continue;
            if (b8_stat_on()) b8s_rect++;
            search_b_part(f, mbx, mby, bp, mlam, &mo_s[bp], mvL0, r0, mvL1);
            int bestc = 0, bestsatd = 0x7fffffff;
            pixel pred[256], cpred[2][256];
            struct bpart_cache bpartc;
            for (int bm = 0; bm < 3; bm++) {
                build_bpart_pred(f, mbx, mby, bp, bm, &mo_s[bp], pred, cpred, &bpartc);
                int d = satd_block(src, ss, pred, 16, 16, 16);
                if (d < isi_pure) isi_pure = d;
                int s = d + bpart_mv_rate(&mo_s[bp], bm, mlam);
                if (s < bestsatd) {
                    bestsatd = s; bestc = bm;
                    memcpy(ppt[bp], pred, sizeof pred);
                    memcpy(cpt[bp], cpred, sizeof cpred);
                }
            }
            ptsatd[bp] = bestsatd; ptcombo[bp] = bestc;
        }
        for (int bp = 1; bp <= 2; bp++) if (ptsatd[bp] < isi) isi = ptsatd[bp];
        /* sum8 deliberately does NOT join isi. It is the right running best for
 * the rectangular ESTIMATE gate (b8_best, x264's i_cost), but folding it
 * into the RD screen tightens thresh wherever B_8x8 is the best SATD and
 * screens out 16x16 candidates that go on to win RD: measured, it took
 * the B_8x8 band from -1.27% to -0.81%. Two different jobs, two
 * variables. */
        /* The intra screen compares a PURE-SATD i16 against this. Handing it the
 * rate-inclusive cost makes the two sides incommensurate, and the mv-rate
 * term is lambda-scaled: as QP rises the right-hand side inflates and the
 * screen admits more and more intra, which is backwards -- measured 43.9%
 * -> 69.5% admission from 25000 to 4000 kbit/s on park_joy_720p. Feed it
 * distortion-vs-distortion instead; the 1.5x margin absorbs the rate
 * asymmetry (it was calibrated against this same screen). Env-gated for
 * the A/B; NOT byte-identical, so it needs a BD round before it ships. */
        b_isatd = intra_screen_pure() ? isi_pure : isi;
        int thresh = isi * (17 + (f->psy_rd > 0.0f ? 1 : 0)) / 16 + 1;
        b8_thresh = thresh;         /* the B_8x8 candidate rides the same screen */

        /* --- RD phase: only the survivors, reusing the prediction built above.
 * The 16x16 modes already had theirs, above the B_SKIP return. --- */
        BPCUT(4);
        for (int bp = 1; bp <= 2; bp++) {
            if (ptsatd[bp] >= thresh) continue;
            j = rd_b_part(f, mbx, mby, bp, ptcombo[bp], &mo_s[bp],
                          ppt[bp], cpt[bp], lam, &tmp);
            if (j < best) { best = j; mode = 2; ires = tmp; save_mb_rec(f, mbx, mby, snap_best); }
        }
    } else {
        /* B_L0 / B_L1 / B_Bi 16x16: RD all three (byte-identical max-quality path).
 * bm 0/1 stash their unipred; bm 2 (Bi) averages the stash (same ref+MV). */
        struct bpred_cache bpc;
        for (int bm = 0; bm < 3; bm++) {
            j = eval_b_mode(f, mbx, mby, bm, r0, mvL0, mvL1, pL0, pL1, lam, &tmp, &bpc);
            if (j < best) { best = j; mode = 2; ires = tmp; save_mb_rec(f, mbx, mby, snap_best); }
        }
        for (int bp = 1; bp <= 2; bp++) {
            struct bpart_mo mo;
            /* subme >= 9 keeps the cold, exhaustive form: this tier's whole
 * contract is the byte-identical max-quality tournament. */
            search_b_part(f, mbx, mby, bp, mlam, &mo, NULL, 0, NULL);
            for (int bm = 0; bm < 3; bm++) {
                j = eval_b_part(f, mbx, mby, bp, bm, &mo, lam, &tmp);
                if (j < best) { best = j; mode = 2; ires = tmp; save_mb_rec(f, mbx, mby, snap_best); }
            }
        }
    }

    /* Mid-tournament skip commit, x264's B_SKIP early return:
 * 3406-3415): after the 16x16 RD stage, when the skip candidate is still
 * the running RD best -- it has now beaten direct's RD and every RD'd
 * 16x16 mode -- x264 commits B_SKIP and returns before intra and the
 * subpartitions. This is not the falsified pre-ME skip gate: no
 * speculative probe is involved, the comparison uses costs the
 * tournament already computed. x264 runs intra on 6% of its skip-verdict
 * B MBs against our 48%. What this exit forgoes: an intra win over a
 * currently-winning skip, a subset of the 0.14% of B MBs that end intra
 * at all. */
    /* The BD evidence behind the shipped scope. Unguarded, the exit over-skips
 * at the ABR band (akiyo +2.21%). x264's direct-competitiveness
 * precondition fixes touchdown and halves park_joy but leaves akiyo
 * +1.48 ABR / sintel +1.70 CRF -- temporal-propagation shapes. Scoped to
 * non-reference B's the BD is fully clean (CRF band byte-identical 12/12,
 * ABR worst +0.89) but it forfeits samsung, because mb-tree's QP cascade
 * means non-ref B's already skip via the cheap early probe and the
 * late-skip population sits on REFERENCE B's. Readmitting ref B's under a
 * lambda-scaled cheapness bound brings the ABR damage back (akiyo +2.22,
 * park_joy +1.68): the bound grows with lambda, so it is generous exactly
 * at the low rates where the propagation damage lives -- no good point on
 * that curve. SHIPPED scope = non-ref B's only, default on.
 * Y264_B_SKIP_EXIT=0 restores the full tournament; =2 additionally exits
 * reference B's whose skip recon is near-exact in ABSOLUTE terms (SSD
 * bound, rate-independent), the one untested theory. */
    if (b_skip_exit_env() && bexit_ok && mode == 0 && f->subme <= 8 &&
        (!f->slice_is_ref || bx_ref_admit(f, mbx, mby, bdist_x))) {
        if (b8_stat_on() && b8_have) b8s_skipexit++;
        bl_path = 2;
        goto b_decided;
    }

    /* Intra. W-B (S1): SATD-screen first -- skip the full intra encode + RD when
 * i16 is not competitive with the inter/direct winner (b_isatd). Off at
 * subme>=9 (b_isatd<0 -> always admit -> byte-identical). */
    struct intra_mb intra;
    BPCUT(5);
    /* B_8x8: four independently predicted quadrants, RD'd as one more inter
 * candidate. Placed after the 16x16 and rectangular modes so it competes
 * against a settled `best`, and gated by Y264_B_8X8. */
    if (b_8x8_on() && !b8_gated) {
        if (!b8_have) {
            if (b8_stat_on()) b8s_search++;
            memset(&i8, 0, sizeof i8);
            sum8 = search_b_8x8(f, mbx, mby, mlam, &dmv, direct_ok, dp,
                                mvL0, r0, mvL1, &i8, &b8p);
        }
        /* Same SATD screen the 16x16 and rectangular candidates pass: RD the
 * quadrant split only where its summed 8x8 cost is within thresh of the
 * best SATD in the tournament. The RD trial is the larger half of the
 * mode's wall (search+ranking 3.4-7.9%, whole mode 5.8-13.6% on the
 * board's cells), and most macroblocks are not close. At subme >= 9 the
 * screen is inactive, as it is for every other candidate there. */
        if (sum8 >= (long)b8_thresh) goto b8_done;
        /* Y264_B8_NORD=1: run the search + sub-type ranking and throw the
 * result away, so the wall A/B splits mode's cost into its search
 * half and its RD half. Output must be byte-identical to the default
 * (the candidate is never taken) -- that is the probe's own gate. */
        if (b8_nord_on()) goto b8_done;
        if (b8_stat_on()) b8s_rd++;
        pixel p8[256], c8[2][256];
        build_b8_pred(f, mbx, mby, &i8, p8, c8);
        qp_save(f, &qc);
        g_res_site = RES_SITE_B8;
        encode_inter_res_tp(f, mbx, mby, p8, c8, 1, &i8, lam, 0, tp);
        g_res_site = RES_SITE_OTHER;
        if (cabac_rd_on() && f->cabac) {
            j = dist_mb(f, mbx, mby) + Y264_LAMJD(lam, est_b_bits(f, mbx, mby, &i8, 0) / 256.0);
            escr(5, f, mbx, mby, dist_mb(f, mbx, mby), j,
                 Y264_LAMJD(lam, est_bits_lb(f, &i8) / 256.0));
        } else {
            save_mb_nnz(f, mbx, mby, nzbuf);
            y264_bs_init_count(&sb);        /* pricing only */
            write_b_mb(&sb, f, mbx, mby, &i8);
            j = dist_mb(f, mbx, mby) + Y264_LAMJD(lam, y264_bs_pos_bits(&sb));
            load_mb_nnz(f, mbx, mby, nzbuf);
        }
        qp_load(f, &qc);
        if (j < best) {
            best = j; mode = 2; ires = i8;
            if (b8_stat_on()) b8s_win++;
            save_mb_rec(f, mbx, mby, snap_best);
        }
b8_done: ;
    }

    /* Y264_B_INTRA_FINE: arm the i4/i8 fine gate on the B side by passing the
 * inter SATD reference instead of -1, symmetric with the P path. */
    if (intra_admit_g(f, mbx, mby, b_isatd, 1) || best >= 1e29) {
        if (b_intra_fine_env())
            analyze_intra_gb(f, mbx, mby, &intra, b_isatd);
        else
            analyze_intra(f, mbx, mby, &intra);
        save_mb_nnz(f, mbx, mby, nzbuf);
        qp_save(f, &qc);
        y264_bs_init_count(&sb);        /* pricing only */
        write_intra_syntax(&sb, f, mbx, mby, 23, &intra);
        j = dist_mb(f, mbx, mby) + Y264_LAMJD(lam, y264_bs_pos_bits(&sb));
        load_mb_nnz(f, mbx, mby, nzbuf);
        qp_load(f, &qc);
        if (j < best) { best = j; mode = 3; save_mb_rec(f, mbx, mby, snap_best); }
    }

b_decided:
    if (fs_pend) {
        fs_esc[fs_e][fs_d]++;
        if (mode == 0) fs_skip[fs_e][fs_d]++;
        for (int q = 0; q < 4; q++)
            if (fs_rdm & (1 << q)) { fs_rd[q]++; if (mode == 0) fs_rds[q]++; }
    }
    BPCUT(6);
    if (s4) {                       /* S4: re-encode the winner with the full RDOQ */
        s_rd_trial = 0;
        if (mode == 1) {
            qp_save(f, &qc);
            encode_inter_res(f, mbx, mby, dp, dcp, 1, &idir, lam, 0);
            qp_load(f, &qc);
            save_mb_rec(f, mbx, mby, snap_best);
        } else if (mode == 2) {
            /* Rebuild the winning prediction from the motion recorded in ires
 * (works for both the threshold and the full-tournament paths). */
            pixel rp[256], rcp[2][256];
            if (ires.bpart == 3) {          /* B_8x8: four quadrants */
                build_b8_pred(f, mbx, mby, &ires, rp, rcp);
            } else if (ires.bpart == 0) {
                build_bpred(f, mbx, mby, ires.bmode, ires.ref[0],
                            ires.mvx[0], ires.mvy[0], ires.mvx[1], ires.mvy[1],
                            rp, rcp, NULL);
            } else {
                struct bpart_mo mo;
                for (int p = 0; p < 2; p++) {
                    mo.l0ref[p] = ires.ref[p];
                    mo.l0mv[p][0] = ires.mvx[p];       mo.l0mv[p][1] = ires.mvy[p];
                    mo.l0pmv[p][0] = ires.pmvx[p];     mo.l0pmv[p][1] = ires.pmvy[p];
                    mo.l1mv[p][0] = ires.mvx[2 + p];   mo.l1mv[p][1] = ires.mvy[2 + p];
                    mo.l1pmv[p][0] = ires.pmvx[2 + p]; mo.l1pmv[p][1] = ires.pmvy[2 + p];
                }
                build_bpart_pred(f, mbx, mby, ires.bpart, ires.bmode, &mo, rp, rcp, NULL);
            }
            qp_save(f, &qc);
            encode_inter_res(f, mbx, mby, rp, rcp, 1, &ires, lam, 0);
            qp_load(f, &qc);
            save_mb_rec(f, mbx, mby, snap_best);
        } else if (mode == 3) {
            if (b_intra_fine_env())
                analyze_intra_gb(f, mbx, mby, &intra, b_isatd);
            else
                analyze_intra(f, mbx, mby, &intra);
            save_mb_rec(f, mbx, mby, snap_best);
        }
    }
    load_mb_rec(f, mbx, mby, snap_best);

    /* Set mb_tr8 to the winning mode's transform size (intra I_8x8, or an inter
 * 8x8 transform on B_Direct/B_L0/L1/Bi); the intra trial's scratch write left
 * it dirty otherwise. */
    if (f->mb_tr8)
        f->mb_tr8[mby * f->wmb + mbx] =
            (mode == 3) ? (pixel)intra.use_i8 :
            (mode == 1) ? (pixel)idir.tr8 :
            (mode == 2) ? (pixel)ires.tr8 : 0;

    NLED(mb_b_skip, mode == 0);
    if (y264_skor_mode() == 1) y264_skor_put(f->skor_key, 1, mbx, mby, f->wmb, mode == 0);
    if (blate_fp()) {
        int li = mby * f->wmb + mbx;
        int ds = direct_ok ? satd_block(src, ss, dp, 16, 16, 16) : -1;
        long da0 = -1, da1 = -1;
        if (f->lr_bseed_mvx0 && direct_ok) {
            long t;
            da0 = labs(f->lr_bseed_mvx0[li] - dmv.mvL0[0][0]);
            t = labs(f->lr_bseed_mvy0[li] - dmv.mvL0[0][1]); if (t > da0) da0 = t;
            da1 = labs(f->lr_bseed_mvx1[li] - dmv.mvL1[0][0]);
            t = labs(f->lr_bseed_mvy1[li] - dmv.mvL1[0][1]); if (t > da1) da1 = t;
        }
        fprintf(blate_fp(), "%d %d %d %d %d %d %ld %d %d %d %d %d %ld %ld %d %d\n",
                f->poc, mbx, mby, mode, bl_path, f->slice_is_ref,
                bdist_x == LONG_MAX ? -1L : bdist_x, ds, bl_satd16,
                f->lr_bseed_c0 ? f->lr_bseed_c0[li] : -1,
                f->lr_bseed_c1 ? f->lr_bseed_c1[li] : -1,
                f->lr_bseed_ci ? f->lr_bseed_ci[li] : -1,
                da0, da1, f->mbtree_off ? f->mbtree_off[li] : 0, f->cur_qp);
    }
    out->mode = (uint8_t)mode;
    out->dmv = dmv;
    if (mode == 1) out->u.ir = idir;
    else if (mode == 2) out->u.ir = ires;
    else if (mode == 3) out->u.intra = intra;

    if (mb_log_on()) {
        int part = -1, ref = -1; long amv = 0;
        if (mode == 2) {                 /* B inter: the UMH-searched path */
            part = ires.bpart * 4 + ires.bmode; ref = ires.ref[0];
            int np = ires.bpart == 0 ? 1 : 2;
            for (int i = 0; i < np; i++)          /* L0 [0..1], L1 [2..3] */
                amv += labs(ires.mvx[i]) + labs(ires.mvy[i])
                     + labs(ires.mvx[2 + i]) + labs(ires.mvy[2 + i]);
        } else if (mode == 3) {          /* intra */
            part = 8 + intra.use_i8;
        }                                 /* mode 0/1: direct MVs, not searched */
        mb_log_line(f->poc, mbx, mby, 'B', mode, part, ref, amv,
                    lround(best), ssd_luma_mb(f, mbx, mby));
    }
    if (rescensus_on() && mode == 2)
        atomic_fetch_add_explicit(&g_res_mbs_inter, 1, memory_order_relaxed);
    if (bp_on) {
        uint64_t nw = bp_now();
        bp_loc[bp_stage] += nw - bp_last;
        bp_mbs[mode]++;
        if (mode == 2 && out) {                 /* the coded-inter shape */
            /* B_8x8 (bpart 3) carries no macroblock-level bmode -- its four
 * quadrants each pick their own -- so counting it as L0 would
 * silently inflate that column. It gets its own cell, which is what
 * x264's 'B16..8' third number is. */
            if (out->u.ir.bpart == 3) bp_b8++;
            else {
                if ((unsigned)out->u.ir.bmode < 3) bp_bmode[out->u.ir.bmode]++;
                if ((unsigned)out->u.ir.bpart < 3)  bp_bpart[out->u.ir.bpart]++;
            }
        }
        for (int s = 0; s < BP_NSTAGE; s++) if (bp_loc[s]) {
            bp_ns[mode][s] += bp_loc[s];
            bp_cnt[mode][s]++;
        }
    }
#undef BPCUT
}

/* W1 B-slice pass-1a wavefront runners (defined after y264_frame_encode's helpers). */
static int b_wf_run(y264_frame_t *f, struct b_rec *recs, int mlam, long lam, int wt);
static int bcb_wf_run(y264_frame_t *f, struct b_rec *recs, int mlam, long lam,
                      int wt, const uint8_t *slice_ctx);

/* B-slice emit halves (W2 split). analyze_b_slice fills recs/qc0/slice_ctx; these
 * walk the records and write the bitstream, mirroring the P emit_p_* pair. */
static void emit_b_cabac(y264_frame_t *f, struct b_rec *recs,
                         const struct qp_chain *qc0, const uint8_t *slice_ctx)
{
    y264_cabac_t *bc = f->cabac;
    memcpy(bc->ctx, slice_ctx, Y264_CABAC_CTX);   /* restore slice-init (pass 1 clobbered) */
    qp_load(f, qc0);
    for (int mby = 0; mby < f->hmb; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++) {
            struct b_rec *r = &recs[mby * f->wmb + mbx];
            mb_qp_pre(f, mbx, mby);
            cabac_mb_skip(bc, f, mbx, mby, r->mode == 0, 24);
            if (r->mode == 1)      emit_b_direct_cabac(bc, f, mbx, mby, &r->u.ir);
            else if (r->mode == 2) emit_b_inter_cabac(bc, f, mbx, mby, &r->u.ir);
            else if (r->mode == 3) emit_intra_cabac(bc, f, mbx, mby, &r->u.intra, 2);
            mb_qp_post(f, mbx, mby);
            int last = (mby == f->hmb - 1 && mbx == f->wmb - 1);
            y264_cabac_encode_terminate(bc, last);
        }
}

static void emit_b_cavlc(y264_bs_t *bs, y264_frame_t *f, struct b_rec *recs,
                         const struct qp_chain *qc0)
{
    qp_load(f, qc0);
    int skip_run = 0;
    for (int mby = 0; mby < f->hmb; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++) {
            struct b_rec *r = &recs[mby * f->wmb + mbx];
            mb_qp_pre(f, mbx, mby);
            if (r->mode == 0) {                     /* B_Skip: counted in mb_skip_run */
                skip_run++;
                mb_qp_post(f, mbx, mby);
                continue;
            }
            y264_bs_write_ue(bs, skip_run); skip_run = 0;
            if (r->mode == 1) {
                y264_bs_write_ue(bs, 0);            /* mb_type B_Direct_16x16 */
                emit_inter_residual(bs, f, mbx, mby, &r->u.ir);
            } else if (r->mode == 2) {
                emit_b_mb(bs, f, mbx, mby, &r->u.ir);
            } else {
                emit_intra_syntax(bs, f, mbx, mby, 23, &r->u.intra);
            }
            mb_qp_post(f, mbx, mby);
        }
    y264_bs_write_ue(bs, skip_run);
}

/* B-slice analyze (W2 split): passes 1+1b -> recs + qc0 + slice_ctx (CABAC).
 * Returns the malloc'd records array; caller emits via emit_b_{cabac,cavlc}. */
static struct b_rec *analyze_b_slice(y264_frame_t *f, struct qp_chain *out_qc0,
                                     uint8_t *out_slice_ctx, long *out_est_total)
{
    *out_est_total = 0;
    int mlam = lambda_me(f->qp);            /* B ME lambda (SATD/SAD domain) */
    long lam = lambda_mode16(f->qp);
    int8_t nzbuf[16 + 32];
    pixel snap_best[16 * 16 + 2 * 16 * 16];
    y264_cabac_t *bc = f->cabac;
    if (bc) {
        for (int i = 0; i < f->wmb * f->hmb; i++) f->mbcbp[i] = -1;
        size_t nmv = (size_t)f->mv_stride * f->hmb * 4;
        for (size_t i = 0; i < nmv; i++) {
            f->mvdx[i] = 0;  f->mvdy[i] = 0;
            f->mvdx1[i] = 0; f->mvdy1[i] = 0;
        }
    }

    if (bc) {
        /* CABAC B: two passes (W0 4e), mirroring the P flip. Pass 1 analyses +
 * authors grids + records + advances est_ctx via est_commit_b in ALL modes;
 * pass 2 emits owning the engine ctx. QP chain advanced in pass 1 (for the
 * RD trials), rewound, re-driven in pass 2. est_commit_b clobbers bc->ctx, so
 * restore slice-init before pass 2. Mode 0 byte-identical; WPP default BD-gated. */
        int m6b = est_ctx_mode();
        uint8_t slice_ctx[Y264_CABAC_CTX], wpp_ctx[Y264_CABAC_CTX];
        long est_total = 0;
        struct b_rec *recs = malloc((size_t)f->wmb * f->hmb * sizeof(*recs));
        struct qp_chain qc0; qp_save(f, &qc0);
        memcpy(bc->est_ctx, bc->ctx, Y264_CABAC_CTX);   /* est_ctx = slice-init */
        memcpy(slice_ctx, bc->ctx, Y264_CABAC_CTX);
        memcpy(wpp_ctx, bc->ctx, Y264_CABAC_CTX);
        int wt = f->pool ? ntp_pool_nthreads((ntp_pool_t *)f->pool) : 0;
        int pred = (wt > 1 && m6b == 2) || wf_predqp_env();
        int done_1a = wt > 1 && m6b == 2 && bcb_wf_run(f, recs, mlam, lam, wt, slice_ctx);
        if (!done_1a)
            for (int mby = 0; mby < f->hmb; mby++) {
                if (m6b == 1)      memcpy(bc->est_ctx, slice_ctx, Y264_CABAC_CTX);
                else if (m6b == 2) memcpy(bc->est_ctx, wpp_ctx, Y264_CABAC_CTX);
                for (int mbx = 0; mbx < f->wmb; mbx++) {
                    struct b_rec *r = &recs[mby * f->wmb + mbx];
                    if (f->row_gate && mbx == 0)    /* staircase (serial fallback) */
                        f->row_gate(f->row_gate_ctx, mby);
                    if (pred) f->prev_qp = predict_prev_qp(f, mbx, mby);
                    memcpy(bc->ctx, bc->est_ctx, Y264_CABAC_CTX);   /* RDOQ reads est_ctx via ctx */
                    analyze_b_mb(f, mbx, mby, mlam, lam, nzbuf, snap_best, recs, r);
                    if (r->mode == 0) {                 /* B_Skip */
                        clear_mb_nnz(f, mbx, mby);
                        commit_direct_motion(f, mbx, mby, &r->dmv);
                        f->mbcbp[mby * f->mbcbp_stride + mbx] = (1 << 20);
                    } else if (r->mode == 1) {          /* B_Direct_16x16 */
                        author_b_direct_cabac(f, mbx, mby, &r->u.ir);
                        commit_direct_motion(f, mbx, mby, &r->dmv);
                    } else if (r->mode == 2) {          /* B inter (16x16 or partitions) */
                        author_b_inter_cabac(f, mbx, mby, &r->u.ir);
                        if (r->u.ir.bpart == 3) commit_b8_motion(f, mbx, mby, &r->u.ir);
                        else if (r->u.ir.bpart) commit_bpart_motion(f, mbx, mby, &r->u.ir);
                        else commit_b_motion(f, mbx, mby, &r->u.ir);
                    } else {                            /* intra */
                        author_intra_cabac(f, mbx, mby, &r->u.intra);
                        set_mb_intra_motion(f, mbx, mby);
                    }
                    est_total += est_commit_b(f, mbx, mby, r);
                    if (m6b == 2 && mbx == 1) memcpy(wpp_ctx, bc->est_ctx, Y264_CABAC_CTX);
                    if (!pred) commit_qpy(f, mbx, mby, b_codes_qpd(f, r));
                }
            }
        if (pred) {                                   /* serial QPY-chain resolve */
            qp_load(f, &qc0);
            for (int mby = 0; mby < f->hmb; mby++)
                for (int mbx = 0; mbx < f->wmb; mbx++) {
                    mb_qp_pre(f, mbx, mby);
                    commit_qpy(f, mbx, mby, b_codes_qpd(f, &recs[mby * f->wmb + mbx]));
                }
        }
        *out_qc0 = qc0;
        memcpy(out_slice_ctx, slice_ctx, sizeof slice_ctx);
        *out_est_total = est_total;
        return recs;
    }

    /* CAVLC: two passes (W0). Pass 1 analyses + reconstructs + authors grids +
 * records; pass 2 emits owning skip_run + the QP chain. Pass 1 keeps the
 * raster prev_qp chain in sync so the RD trials price mb_qp_delta correctly
 * (see the P path); the chain is rewound before the real emit. */
    struct b_rec *recs = malloc((size_t)f->wmb * f->hmb * sizeof(*recs));
    struct qp_chain qc0; qp_save(f, &qc0);
    int wt = f->pool ? ntp_pool_nthreads((ntp_pool_t *)f->pool) : 0;
    int pred = wt > 1 || wf_predqp_env();
    int done_1a = wt > 1 && b_wf_run(f, recs, mlam, lam, wt);   /* 1a: parallel analyze */
    if (!done_1a)
        for (int mby = 0; mby < f->hmb; mby++)      /* 1a: serial analyze + author */
            for (int mbx = 0; mbx < f->wmb; mbx++) {
                struct b_rec *r = &recs[mby * f->wmb + mbx];
                if (f->row_gate && mbx == 0)        /* staircase (serial fallback) */
                    f->row_gate(f->row_gate_ctx, mby);
                if (pred) f->prev_qp = predict_prev_qp(f, mbx, mby);
                analyze_b_mb(f, mbx, mby, mlam, lam, nzbuf, snap_best, recs, r);
                if (r->mode == 0) {                 /* B_Skip */
                    clear_mb_nnz(f, mbx, mby);
                    commit_direct_motion(f, mbx, mby, &r->dmv);
                } else if (r->mode == 1) {          /* B_Direct_16x16 */
                    author_inter_residual(f, mbx, mby, &r->u.ir);
                    commit_direct_motion(f, mbx, mby, &r->dmv);
                } else if (r->mode == 2) {          /* B inter */
                    author_inter_residual(f, mbx, mby, &r->u.ir);
                    if (r->u.ir.bpart == 3) commit_b8_motion(f, mbx, mby, &r->u.ir);
                    else if (r->u.ir.bpart) commit_bpart_motion(f, mbx, mby, &r->u.ir);
                    else commit_b_motion(f, mbx, mby, &r->u.ir);
                } else {                            /* intra */
                    author_intra_residual(f, mbx, mby, &r->u.intra);
                    set_mb_intra_motion(f, mbx, mby);
                }
                if (!pred)
                    commit_qpy(f, mbx, mby, b_codes_qpd(f, r));
            }
    if (pred) {                                     /* 1b: serial QPY-chain resolve */
        qp_load(f, &qc0);
        for (int mby = 0; mby < f->hmb; mby++)
            for (int mbx = 0; mbx < f->wmb; mbx++) {
                mb_qp_pre(f, mbx, mby);
                commit_qpy(f, mbx, mby, b_codes_qpd(f, &recs[mby * f->wmb + mbx]));
            }
    }

    *out_qc0 = qc0;
    return recs;
}

/* --- CABAC macroblock coding (intra slices) --- */

static int mbcbp_get(y264_frame_t *f, int mbx, int mby)
{
    if (mbx < 0 || mby < 0 || mbx >= f->wmb || mby >= f->hmb)
        return -1;
    return f->mbcbp[mby * f->mbcbp_stride + mbx];
}

/* coded_block_flag neighbour term for a 4x4/AC block from the nnz grid; an
 * out-of-frame neighbour of an intra block contributes 1. */
/* coded_block_flag neighbour term. For an unavailable neighbour the inferred
 * condTermFlag is 1 for an intra current MB but 0 for an inter one (9.3.3.1.1.9). */
static int cbf_nb(y264_frame_t *f, int comp, int bx, int by, int intra)
{
    int w = f->nnz_stride[comp];
    int gw = comp ? f->wmb * f->cbw : f->wmb * 4;
    int gh = comp ? f->hmb * f->cbh : f->hmb * 4;
    if (bx < 0 || by < 0 || bx >= gw || by >= gh)
        return intra ? 1 : 0;
    int v = f->nnz[comp][by * w + bx];
    return v > 0 ? 1 : 0;
}

/* coded_block_flag neighbour term for a DC block from the per-MB cbp cache. */
static int dc_nb(y264_frame_t *f, int mbx, int mby, int bit, int intra)
{
    int v = mbcbp_get(f, mbx, mby);
    if (v < 0) return intra ? 1 : 0;            /* unavailable: intra 1, inter 0 */
    return (v >> bit) & 1;
}

/* mb_type for an intra MB in an I slice (9.3.2.5 binarization). */
/* Intra mb_type binarization (I_NxN vs I_16x16 + cbp/pred bins). The six context
 * indices differ by slice: I-slice (3+ctx,6,7,8,9,10), intra-in-P (17,18,19,19,
 * 20,20), intra-in-B (32,33,34,34,35,35). The prefix bin that selects intra is
 * coded by the caller for P/B. */
static void cabac_mb_type_intra(y264_cabac_t *c, int use_i4, int cbp_luma,
                                int cbp_chroma, int pred, int ctx0, int ctx1,
                                int ctx2, int ctx3, int ctx4, int ctx5)
{
    if (use_i4) {
        y264_cabac_encode_decision(c, ctx0, 0);
        return;
    }
    y264_cabac_encode_decision(c, ctx0, 1);
    y264_cabac_encode_terminate(c, 0);          /* terminal bin (ctxIdx 276) */
    y264_cabac_encode_decision(c, ctx1, cbp_luma ? 1 : 0);
    if (cbp_chroma == 0) {
        y264_cabac_encode_decision(c, ctx2, 0);
    } else {
        y264_cabac_encode_decision(c, ctx2, 1);
        y264_cabac_encode_decision(c, ctx3, cbp_chroma >> 1);
    }
    y264_cabac_encode_decision(c, ctx4, pred >> 1);
    y264_cabac_encode_decision(c, ctx5, pred & 1);
}

static void cabac_mb_type_i(y264_cabac_t *c, int use_i4, int cbp_luma,
                            int cbp_chroma, int pred, int ctx0)
{
    cabac_mb_type_intra(c, use_i4, cbp_luma, cbp_chroma, pred, ctx0, 6, 7, 8, 9, 10);
}

static void cabac_intra4x4_mode(y264_cabac_t *c, int pred, int mode)
{
    if (pred == mode) {
        y264_cabac_encode_decision(c, 68, 1);
    } else {
        y264_cabac_encode_decision(c, 68, 0);
        if (mode > pred) mode--;
        y264_cabac_encode_decision(c, 69, mode & 1);
        y264_cabac_encode_decision(c, 69, (mode >> 1) & 1);
        y264_cabac_encode_decision(c, 69, (mode >> 2) & 1);
    }
}

static void cabac_chroma_pred_mode(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby, int mode)
{
    int la = mbcbp_get(f, mbx - 1, mby), lb = mbcbp_get(f, mbx, mby - 1);
    int ctx = 0;
    if (la >= 0 && ((la >> 12) & 3) != 0) ctx++;
    if (lb >= 0 && ((lb >> 12) & 3) != 0) ctx++;
    y264_cabac_encode_decision(c, 64 + ctx, mode > 0);
    if (mode > 0) {
        y264_cabac_encode_decision(c, 64 + 3, mode > 1);
        if (mode > 1)
            y264_cabac_encode_decision(c, 64 + 3, mode > 2);
    }
}

static void cabac_cbp_luma(y264_cabac_t *c, int cbp, int cbp_l, int cbp_t)
{
    y264_cabac_encode_decision(c, 76 - ((cbp_l >> 1) & 1) - ((cbp_t >> 1) & 2), (cbp >> 0) & 1);
    y264_cabac_encode_decision(c, 76 - ((cbp   >> 0) & 1) - ((cbp_t >> 2) & 2), (cbp >> 1) & 1);
    y264_cabac_encode_decision(c, 76 - ((cbp_l >> 3) & 1) - ((cbp   << 1) & 2), (cbp >> 2) & 1);
    y264_cabac_encode_decision(c, 76 - ((cbp   >> 2) & 1) - ((cbp   >> 0) & 2), (cbp >> 3) & 1);
}

static void cabac_cbp_chroma(y264_cabac_t *c, int cbp_chroma, int cbp_l, int cbp_t)
{
    int cbp_a = (cbp_l < 0 ? 0 : cbp_l) & 0x30, cbp_b = (cbp_t < 0 ? 0 : cbp_t) & 0x30;
    int ctx = 0;
    if (cbp_a && cbp_l != -1) ctx++;
    if (cbp_b && cbp_t != -1) ctx += 2;
    if (cbp_chroma == 0) {
        y264_cabac_encode_decision(c, 77 + ctx, 0);
    } else {
        y264_cabac_encode_decision(c, 77 + ctx, 1);
        ctx = 4;
        if (cbp_a == 0x20) ctx++;
        if (cbp_b == 0x20) ctx += 2;
        y264_cabac_encode_decision(c, 77 + ctx, cbp_chroma >> 1);
    }
}

/* Code intra chroma residual via CABAC. Returns the two DC coded_block_flags. */

/* Full intra macroblock via CABAC (I slice). */
/* mb_skip_flag: ctxIdxInc = (leftAvail && !leftSkip) + (topAvail && !topSkip),
 * base 11 for P, 24 for B. The per-MB skip flag lives in mbcbp bit 20. */
static void cabac_mb_skip(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby, int skip, int base)
{
    int la = mbcbp_get(f, mbx - 1, mby), lt = mbcbp_get(f, mbx, mby - 1);
    int ctx = base + (la >= 0 && !((la >> 20) & 1)) + (lt >= 0 && !((lt >> 20) & 1));
    y264_cabac_encode_decision(c, ctx, skip);
}

/* transform_size_8x8_flag (ctxIdxOffset 399). ctxIdxInc = condTermFlagA +
 * condTermFlagB, where a neighbour contributes 1 if it is available and used the
 * 8x8 transform (mbcbp bit 22). */
static void cabac_transform_8x8_flag(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby, int flag)
{
    int la = mbcbp_get(f, mbx - 1, mby), lt = mbcbp_get(f, mbx, mby - 1);
    int ctx = 399 + (la >= 0 && ((la >> 22) & 1)) + (lt >= 0 && ((lt >> 22) & 1));
    y264_cabac_encode_decision(c, ctx, flag);
}

/* One mvd component: UEGk (k=3, uCoff=9) prefix + EG3 bypass suffix + sign.
 * ctxbase 40 for x, 47 for y; ctx is the neighbour-derived ctxIdxInc (0..2). */
static void cabac_mvd_comp(y264_cabac_t *c, int ctxbase, int ctx, int mvd)
{
    static const uint8_t ce[8] = { 3, 4, 5, 6, 6, 6, 6, 6 };
    if (c->est_mode) { y264_cabac_est_mvd(c, ctxbase, ctx, mvd); return; }
    if (mvd == 0) { y264_cabac_encode_decision(c, ctxbase + ctx, 0); return; }
    int a = mvd < 0 ? -mvd : mvd;
    y264_cabac_encode_decision(c, ctxbase + ctx, 1);
    if (a < 9) {
        for (int i = 1; i < a; i++) y264_cabac_encode_decision(c, ctxbase + ce[i - 1], 1);
        y264_cabac_encode_decision(c, ctxbase + ce[a - 1], 0);
    } else {
        for (int i = 1; i < 9; i++) y264_cabac_encode_decision(c, ctxbase + ce[i - 1], 1);
        y264_cabac_encode_ueg_bypass(c, 3, a - 9);
    }
    y264_cabac_encode_bypass(c, mvd < 0);
}

/* Code mvd for a partition whose top-left 4x4 is (bx4,by4), size (w4,h4) blocks.
 * dx/dy are mv-pmv; fx/fy are the per-4x4 abs-mvd fields (list 0 or 1). The
 * context comes from the left/top 4x4 neighbours; the partition's abs mvd is
 * then stored across its blocks for later neighbours. */
/* Author the abs-mvd grid for a partition (the neighbour-context field cabac_mvd
 * reads); no bitstream. Pass 1 (two-pass) calls this. */
static void author_mvd(y264_frame_t *f, int bx4, int by4, int w4, int h4,
                       int dx, int dy, int16_t *fx, int16_t *fy)
{
    int st = f->mv_stride;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax > 127) ax = 127;
    if (ay > 127) ay = 127;
    for (int y = 0; y < h4; y++)
        for (int x = 0; x < w4; x++) {
            fx[(by4 + y) * st + bx4 + x] = (int16_t)ax;
            fy[(by4 + y) * st + bx4 + x] = (int16_t)ay;
        }
}

/* Emit one partition's mvd bins, reading the authored abs-mvd grid for context. */
/* Bit-split instrument: per-frame
 * mode/mv/coeff bit accounting on the P CABAC emit, comparable to x264's
 * pass-1 tex=/mv=/misc= columns. Position = settled bits (byte pointer x8 +
 * queue); deltas telescope, so per-category sums are exact to the sub-bit
 * fraction in `range`. cbp+dQP land in coeff (x264 puts them in tex as well).
 * Y264_BITSTAT=1, IPPP CABAC t1; prints one line per P frame. Default inert. */
static int bitstat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_BITSTAT"); v = e ? atoi(e) : 0; }
    return v;
}
static long g_bits_mode, g_bits_mv, g_bits_coef;
static long g_nmvd, g_summvd, g_part[4];   /* vector count, sum|mvd| qpel, P part histo */
static _Thread_local int g_bitstat_live;  /* emit_inter_cabac also runs inside RD
 * trials; count only the real emit. Thread-local
 * because WIDE chains emit two frames
 * concurrently, and a shared flag is a (benign at
 * default-0, corrupting when armed) data race. An
 * emit runs whole on one thread, so TLS is exact. */
static long cab_pos(const y264_cabac_t *c)
{
    return (long)((uintptr_t)c->p * 8) + c->queue;
}

static void emit_mvd(y264_cabac_t *c, y264_frame_t *f, int bx4, int by4,
                     int dx, int dy, const int16_t *fx, const int16_t *fy)
{
    int st = f->mv_stride;
    int sx = (bx4 > 0 ? fx[by4 * st + bx4 - 1] : 0) + (by4 > 0 ? fx[(by4 - 1) * st + bx4] : 0);
    int sy = (bx4 > 0 ? fy[by4 * st + bx4 - 1] : 0) + (by4 > 0 ? fy[(by4 - 1) * st + bx4] : 0);
    cabac_mvd_comp(c, 40, (sx > 2) + (sx > 32), dx);
    cabac_mvd_comp(c, 47, (sy > 2) + (sy > 32), dy);
    if (g_bitstat_live) {
        g_nmvd++;
        g_summvd += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    }
}

/* W0 4e: author the CABAC grids a residual coder writes (nnz + the DC-presence
 * bits of the mbcbp cache) from the decided levels, no engine. The nnz cell is
 * still the coder's total_coeff (= nonzero count); the CABAC 8x8 transform writes
 * the whole-block count into all four 4x4 cells (unlike CAVLC's per-subblock
 * counts), so this is not the CAVLC author. */
static void author_cabac_chroma_nnz(y264_frame_t *f, int mbx, int mby,
                                    const struct chroma_result *cr, int *cb_dc, int *cr_dc)
{
    *cb_dc = *cr_dc = 0;
    if (cr->cbp) {                                  /* chroma DC coded -> presence bit */
        *cb_dc = nz_count(cr->dc_scan[0], cr->ndc) > 0;
        *cr_dc = nz_count(cr->dc_scan[1], cr->ndc) > 0;
    }
    author_chroma_residual_nnz(f, mbx, mby, cr);    /* AC nnz (same nonzero-count grid) */
}

static void emit_cabac_chroma_residual(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                       const struct chroma_result *cr, int intra)
{
    if (cr->cbp)
        for (int comp = 0; comp < 2; comp++) {
            int nza = dc_nb(f, mbx - 1, mby, 9 + comp, intra);
            int nzb = dc_nb(f, mbx, mby - 1, 9 + comp, intra);
            int dc_cat = f->cf_idc == 2 ? 5 : 3;
            y264_cabac_residual(c, dc_cat, cr->dc_scan[comp], nza, nzb);
        }
    if (cr->cbp != 2) return;
    int cbw = f->cbw, cbh = f->cbh, nblk = cbw * cbh;
    for (int comp = 0; comp < 2; comp++) {
        int cbx0 = mbx * cbw, cby0 = mby * cbh;
        for (int blk = 0; blk < nblk; blk++) {
            int bx = cbx0 + blk % cbw, by = cby0 + blk / cbw;
            int nza = cbf_nb(f, 1 + comp, bx - 1, by, intra);
            int nzb = cbf_nb(f, 1 + comp, bx, by - 1, intra);
            /* cat 4 reads exactly CNT_M1[4]+1 = 15 coefficients: the scan row
 * is passed as-is, no 16-element staging copy */
            y264_cabac_residual(c, 4, cr->ac_scan[comp][blk], nza, nzb);
        }
    }
}

/* 4:4:4 per-component nnz author (per-4x4 nonzero count), matching
 * write444_i4_comp_cabac's grid writes. */
static void author_444_comp_nnz(y264_frame_t *f, int mbx, int mby, int comp, int cbp,
                                const dctcoef lev[16][16])
{
    int stride = f->nnz_stride[comp];
    int8_t *nnz = f->nnz[comp];
    int bx0 = mbx * 4, by0 = mby * 4;
    for (int i8 = 0; i8 < 4; i8++)
        for (int i4 = 0; i4 < 4; i4++) {
            int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
            nnz[ay * stride + ax] = (cbp & (1 << i8)) ? (int8_t)nz_count(lev[blk], 16) : 0;
        }
}

static void emit444_i4_comp_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                  int comp, int cbp, int intra, const dctcoef lev[16][16])
{
    static const int I4CAT[3] = { 2, 8, 12 };
    int bx0 = mbx * 4, by0 = mby * 4;
    for (int i8 = 0; i8 < 4; i8++)
        for (int i4 = 0; i4 < 4; i4++) {
            int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
            if (cbp & (1 << i8)) {
                int a = cbf_nb(f, comp, ax - 1, ay, intra), b = cbf_nb(f, comp, ax, ay - 1, intra);
                y264_cabac_residual(c, I4CAT[comp], lev[blk], a, b);
            }
        }
}

/* Author the CABAC inter-tail grids (nnz for luma+chroma, and the mbcbp cache)
 * from the decided result. Pass-1 (two-pass) will call this; the single-pass
 * wrapper calls it before the emit.
 *
 * In EST MODE only the grid state the TRIAL'S OWN emit reads back
 * is authored -- everything authored here is restored before any other reader
 * runs, so writes that serve future macroblocks are skipped:
 * - CABAC 8x8 luma blocks have no coded_block_flag, so cbf_nb never reads
 * the tr8 luma nnz cells -> skip that author (and the nz_counts under it);
 * - chroma AC cbf_nb reads chroma nnz only when cr->cbp == 2;
 * - the self mbcbp word and the DC presence bits are read by NEIGHBOUR
 * lookups of LATER macroblocks only -> skip;
 * - the cbp==0 clear serves later neighbours only -> skip. */
static void author_cabac_inter_tail(y264_frame_t *f, int mbx, int mby,
                                    const struct inter_result *ir, int extra_mbcbp)
{
    int lstride = f->nnz_stride[0];
    int8_t *lnnz = f->nnz[0];
    int bx0 = mbx * 4, by0 = mby * 4;
    int est = f->cabac && f->cabac->est_mode;

    if (f->cf_idc == 3) {
        int cbp = ir->cbp444;
        if (cbp) {
            author_444_comp_nnz(f, mbx, mby, 0, cbp, ir->lev);
            author_444_comp_nnz(f, mbx, mby, 1, cbp, ir->cr_c[0].lev);
            author_444_comp_nnz(f, mbx, mby, 2, cbp, ir->cr_c[1].lev);
        } else if (!est) {
            clear_mb_nnz(f, mbx, mby);
        }
        if (!est)
            f->mbcbp[mby * f->mbcbp_stride + mbx] = (cbp & 0xf) | extra_mbcbp;
        return;
    }

    int cbp_luma = ir->cbp_luma;
    const struct chroma_result *cr = &ir->cr;
    int cb_dc = 0, cr_dc = 0;
    if (cbp_luma || cr->cbp) {
        if (ir->tr8) {
            if (!est)
                for (int blk = 0; blk < 4; blk++) {
                    int nzv = 0;
                    if (cbp_luma & (1 << blk))
                        /* whole-block count in all 4 cells; a nonzero COUNT is
 * scan-order invariant, so count the raster block as-is
 * instead of gathering it through the zigzag first */
                        nzv = nz_count(ir->lev8[blk], 64);
                    for (int j = 0; j < 4; j++) {
                        int lb = blk * 4 + j;
                        lnnz[(by0 + BLK_Y[lb]) * lstride + (bx0 + BLK_X[lb])] = (int8_t)nzv;
                    }
                }
        } else {
            for (int i8 = 0; i8 < 4; i8++)
                for (int i4 = 0; i4 < 4; i4++) {
                    int blk = i8 * 4 + i4, ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                    lnnz[ay * lstride + ax] =
                        (cbp_luma & (1 << i8)) ? (int8_t)nz_count(ir->lev[blk], 16) : 0;
                }
        }
        if (!est) {
            author_cabac_chroma_nnz(f, mbx, mby, cr, &cb_dc, &cr_dc);
        } else if (cr->cbp == 2) {
            author_chroma_residual_nnz(f, mbx, mby, cr);
        }
    } else if (!est) {
        clear_mb_nnz(f, mbx, mby);
    }

    if (!est)
        f->mbcbp[mby * f->mbcbp_stride + mbx] =
            (cbp_luma & 0xf) | (cr->cbp << 4) | (cb_dc << 9) | (cr_dc << 10) |
            ((ir->tr8 & 1) << 22) | extra_mbcbp;
}

/* Emit the CABAC inter tail bins (cbp, mb_qp_delta, luma+chroma residual),
 * reading the nnz / mbcbp grids that author_cabac_inter_tail wrote.
 * parts is an EST_PROF bench mask (1 cbp/tr8/qpd, 2 luma residual, 4 chroma
 * residual); every real caller passes 7 via the wrapper below. */
static void emit_cabac_inter_tail_ex(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                             const struct inter_result *ir, int parts)
{
    if (resprof_on())
        atomic_fetch_add_explicit(c->est_mode ? &g_em_est : &g_em_real, 1,
                                  memory_order_relaxed);
    int cbp_luma = ir->cbp_luma;
    const struct chroma_result *cr = &ir->cr;
    int bx0 = mbx * 4, by0 = mby * 4;
    int la = mbcbp_get(f, mbx - 1, mby), lt = mbcbp_get(f, mbx, mby - 1);

    if (f->cf_idc == 3) {                          /* 4:4:4: Cb/Cr like luma, shared cbp */
        int cbp = ir->cbp444;
        if (parts & 1) cabac_cbp_luma(c, cbp, la, lt); /* no cabac_cbp_chroma */
        if (cbp) {
            if (parts & 1) cabac_mb_qp_delta(c, f, f->cur_qp);
            if (parts & 2) {
                emit444_i4_comp_cabac(c, f, mbx, mby, 0, cbp, 0, ir->lev);
                emit444_i4_comp_cabac(c, f, mbx, mby, 1, cbp, 0, ir->cr_c[0].lev);
                emit444_i4_comp_cabac(c, f, mbx, mby, 2, cbp, 0, ir->cr_c[1].lev);
            }
        }
        return;
    }

    if (parts & 1) {
        cabac_cbp_luma(c, cbp_luma, la, lt);
        cabac_cbp_chroma(c, cr->cbp, la, lt);
        /* transform_size_8x8_flag (present when CodedBlockPatternLuma>0 and, for
 * P_8x8, no sub-partition is below 8x8). */
        if (tr8_flag_present(f, ir))
            cabac_transform_8x8_flag(c, f, mbx, mby, ir->tr8);
    }

    if (cbp_luma || cr->cbp) {
        if (parts & 1) cabac_mb_qp_delta(c, f, f->cur_qp);
        if (!(parts & 2)) { }
        else if (ir->tr8) {
            for (int blk = 0; blk < 4; blk++)
                if (cbp_luma & (1 << blk)) {
                    if (resprof_on() && c->est_mode)
                        atomic_fetch_add_explicit(&g_blk8_est, 1, memory_order_relaxed);
                    if (c->est_mode) {          /* fused gather, bit-exact */
                        y264_cabac_residual_8x8_est(c, ir->lev8[blk]);
                    } else {
                        dctcoef scan8[64];
                        for (int k = 0; k < 64; k++) scan8[k] = ir->lev8[blk][ZIGZAG8[k]];
                        y264_cabac_residual_8x8(c, scan8);
                    }
                }
        } else {
            for (int i8 = 0; i8 < 4; i8++)
                for (int i4 = 0; i4 < 4; i4++) {
                    int blk = i8 * 4 + i4;
                    int ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                    if (cbp_luma & (1 << i8)) {
                        if (resprof_on() && c->est_mode)
                            atomic_fetch_add_explicit(&g_blk4_est, 1, memory_order_relaxed);
                        int a = cbf_nb(f, 0, ax - 1, ay, 0), b = cbf_nb(f, 0, ax, ay - 1, 0);
                        y264_cabac_residual(c, 2, ir->lev[blk], a, b);
                    }
                }
        }
        if (parts & 4) emit_cabac_chroma_residual(c, f, mbx, mby, cr, 0);
    }
}

/* refIdxL0 of the neighbour 4x4 block at (nx,ny) for the ref_idx_l0 context
 * (condTermFlag = refIdxL0 > 0). A same-MB, already-coded neighbour is not in
 * the grid yet (the writer runs before commit), so its ref comes from ir: the
 * containing 8x8's ref for P_8x8, else partition 0's. */
static int ref_idx_neighbour(y264_frame_t *f, int nx, int ny, int mbx, int mby,
                             const struct inter_result *ir)
{
    if (nx < 0 || ny < 0) return -1;                 /* frame edge: unavailable */
    if (nx >= mbx * 4 && ny >= mby * 4) {            /* inside the current MB */
        if (ir->part == 3)
            return ir->ref[((ny - mby * 4) >> 1) * 2 + ((nx - mbx * 4) >> 1)];
        return ir->ref[0];
    }
    return f->refidx[ny * f->mv_stride + nx];        /* previous MB (intra = -1) */
}

/* ref_idx_l0 (ctxIdxOffset 54), unary binarization: value ones then a 0. binIdx 0
 * context from the left/top neighbours' refIdxL0>0; binIdx 1 ctx 4; rest ctx 5. */
static void cabac_ref_idx(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                          int bx4, int by4, int ref, const struct inter_result *ir)
{
    int ra = ref_idx_neighbour(f, bx4 - 1, by4, mbx, mby, ir);
    int rb = ref_idx_neighbour(f, bx4, by4 - 1, mbx, mby, ir);
    int inc = (ra > 0) + 2 * (rb > 0);
    y264_cabac_encode_decision(c, 54 + inc, ref > 0);
    for (int b = 1; b <= ref; b++)
        y264_cabac_encode_decision(c, 54 + (b == 1 ? 4 : 5), b < ref);
}

/* Top-left 4x4 of partition p for any P partition mode (3 = P_8x8 raster). */
static void part_topleft(int mbx, int mby, int part, int p, int *bx4, int *by4)
{
    if (part == 3) {
        *bx4 = mbx * 4 + (p & 1) * 2;
        *by4 = mby * 4 + (p >> 1) * 2;
    } else {
        int rx, ry, rw, rh, w4, h4;
        part_rect(mbx, mby, part, p, &rx, &ry, &rw, &rh, bx4, by4, &w4, &h4);
    }
}

/* Author the P inter grids (mvd + nnz + mbcbp) from the record; no bitstream.
 * est mode, part==0: a single 16x16 partition's mvd contexts read only cells
 * OUTSIDE this MB (committed neighbour state), so the trial's mvd author is
 * skipped -- est_p_save/restore skip the matching round trip. */
static void author_inter_cabac(y264_frame_t *f, int mbx, int mby, const struct inter_result *ir)
{
    if (ir->part == 0 && f->cabac && f->cabac->est_mode) {
        author_cabac_inter_tail(f, mbx, mby, ir, 0);
        return;
    }
    if (ir->part == 3) {
        for (int b = 0; b < 4; b++)
            for (int s2 = 0; s2 < SUB_NS[ir->sub[b]]; s2++) {
                int ox, oy, w, h;
                sub_rect(ir->sub[b], s2, &ox, &oy, &w, &h);
                author_mvd(f, mbx * 4 + (b & 1) * 2 + ox / 4,
                           mby * 4 + (b >> 1) * 2 + oy / 4, w / 4, h / 4,
                           ir->mvx[b * 4 + s2] - ir->pmvx[b * 4 + s2],
                           ir->mvy[b * 4 + s2] - ir->pmvy[b * 4 + s2], f->mvdx, f->mvdy);
            }
    } else {
        int nparts = ir->part ? 2 : 1;
        for (int p = 0; p < nparts; p++) {
            int bx4, by4;
            part_topleft(mbx, mby, ir->part, p, &bx4, &by4);
            int w4 = ir->part == 2 ? 2 : 4, h4 = ir->part == 1 ? 2 : 4;
            author_mvd(f, bx4, by4, w4, h4, ir->mvx[p] - ir->pmvx[p],
                       ir->mvy[p] - ir->pmvy[p], f->mvdx, f->mvdy);
        }
    }
    author_cabac_inter_tail(f, mbx, mby, ir, 0);
}

/* Emit the P inter bins, reading the authored mvd / nnz / mbcbp grids.
 * tail_parts is the EST_PROF bench mask forwarded to the tail (0 = header
 * only); every real caller passes 7 via the wrapper below. */
static void emit_inter_cabac_ex(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                             const struct inter_result *ir, int tail_parts)
{
    const int bst = g_bitstat_live;
    long b0 = bst ? cab_pos(c) : 0;
    if (bst) g_part[ir->part & 3]++;
    y264_cabac_encode_decision(c, 14, 0);                 /* P_L0 (inter) */
    if (ir->part == 0) {
        y264_cabac_encode_decision(c, 15, 0);
        y264_cabac_encode_decision(c, 16, 0);
    } else if (ir->part == 1) {                           /* 16x8 */
        y264_cabac_encode_decision(c, 15, 1);
        y264_cabac_encode_decision(c, 17, 1);
    } else if (ir->part == 2) {                           /* 8x16 */
        y264_cabac_encode_decision(c, 15, 1);
        y264_cabac_encode_decision(c, 17, 0);
    } else {                                              /* P_8x8 */
        y264_cabac_encode_decision(c, 15, 0);
        y264_cabac_encode_decision(c, 16, 1);
        /* sub_mb_type (9.3.2.5): 8x8 "1"; 8x4 "00"; 4x8 "011"; 4x4 "010". */
        for (int b = 0; b < 4; b++) {
            int sub = ir->sub[b];
            y264_cabac_encode_decision(c, 21, sub == 0);
            if (sub == 0) continue;
            y264_cabac_encode_decision(c, 22, sub != 1);
            if (sub != 1)
                y264_cabac_encode_decision(c, 23, sub == 2);
        }
    }

    long b1 = bst ? cab_pos(c) : 0;
    int nparts = ir->part == 3 ? 4 : (ir->part ? 2 : 1);
    /* ref_idx_l0 per partition (before the mvds). */
    if (f->nref > 1)
        for (int p = 0; p < nparts; p++) {
            int bx4, by4;
            part_topleft(mbx, mby, ir->part, p, &bx4, &by4);
            cabac_ref_idx(c, f, mbx, mby, bx4, by4, ir->ref[p], ir);
        }
    if (ir->part == 3) {
        for (int b = 0; b < 4; b++)
            for (int s2 = 0; s2 < SUB_NS[ir->sub[b]]; s2++) {
                int ox, oy, w, h;
                sub_rect(ir->sub[b], s2, &ox, &oy, &w, &h);
                emit_mvd(c, f, mbx * 4 + (b & 1) * 2 + ox / 4,
                         mby * 4 + (b >> 1) * 2 + oy / 4,
                         ir->mvx[b * 4 + s2] - ir->pmvx[b * 4 + s2],
                         ir->mvy[b * 4 + s2] - ir->pmvy[b * 4 + s2], f->mvdx, f->mvdy);
            }
    } else {
        for (int p = 0; p < nparts; p++) {
            int bx4, by4;
            part_topleft(mbx, mby, ir->part, p, &bx4, &by4);
            emit_mvd(c, f, bx4, by4, ir->mvx[p] - ir->pmvx[p],
                     ir->mvy[p] - ir->pmvy[p], f->mvdx, f->mvdy);
        }
    }

    long b2 = bst ? cab_pos(c) : 0;
    if (tail_parts) emit_cabac_inter_tail_ex(c, f, mbx, mby, ir, tail_parts);
    if (bst) {
        g_bits_mode += b1 - b0;
        g_bits_mv   += b2 - b1;
        g_bits_coef += cab_pos(c) - b2;
    }
}

static void emit_inter_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                             const struct inter_result *ir)
{
    emit_inter_cabac_ex(c, f, mbx, mby, ir, 7);
}

/* Single-pass wrapper: author grids then emit bins. */
static void write_inter_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                              const struct inter_result *ir)
{
    author_inter_cabac(f, mbx, mby, ir);
    emit_inter_cabac(c, f, mbx, mby, ir);
}

/* B macroblock mb_type ctxIdxInc: neighbours that are neither B_Skip (mbcbp
 * bit 20) nor B_Direct (bit 21) increment the context. */
static int b_mbtype_ctx(y264_frame_t *f, int mbx, int mby)
{
    int la = mbcbp_get(f, mbx - 1, mby), lt = mbcbp_get(f, mbx, mby - 1);
    return (la >= 0 && !((la >> 20) & 3)) + (lt >= 0 && !((lt >> 20) & 3));
}

/* B_Direct_16x16: mb_type bin then residual (direct-derived motion, no mvd). */
/* Author the B_Direct grids (nnz + mbcbp, direct bit 21); no engine. */
static void author_b_direct_cabac(y264_frame_t *f, int mbx, int mby,
                                  const struct inter_result *ir)
{
    author_cabac_inter_tail(f, mbx, mby, ir, 1 << 21);
}

/* Emit the B_Direct bins (mb_type + inter tail), reading the authored grids.
 * tail_parts is the EST_PROF bench mask; real callers pass 7 via the wrapper. */
static void emit_b_direct_cabac_ex(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                const struct inter_result *ir, int tail_parts)
{
    y264_cabac_encode_decision(c, 27 + b_mbtype_ctx(f, mbx, mby), 0);
    if (tail_parts) emit_cabac_inter_tail_ex(c, f, mbx, mby, ir, tail_parts);
}

static void emit_b_direct_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                const struct inter_result *ir)
{
    emit_b_direct_cabac_ex(c, f, mbx, mby, ir, 7);
}

/* Single-pass wrapper: author grids then emit bins. */
static void write_b_direct_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                 const struct inter_result *ir)
{
    author_b_direct_cabac(f, mbx, mby, ir);
    emit_b_direct_cabac(c, f, mbx, mby, ir);
}

/* A B_8x8 quadrant codes its own ref_idx_l0 (B_L0_8x8 / B_Bi_8x8); direct and
 * L1-only quadrants do not. */
static int b8_codes_l0(int sub) { return sub == 1 || sub == 3; }

/* ref_idx_l0 condTermFlag for a B neighbour MB (9.3.3.1.1.6): 0 when the MB is
 * unavailable, coded B_Skip/B_Direct (mbcbp bits 20/21), does not use list 0
 * (refidx -1, includes intra), or uses reference 0.
 *
 * A B_8x8 neighbour needs its QUADRANT looked at, not just the macroblock:
 * SubMbPredMode(B_Direct_8x8) is Direct, so predModeEqualFlag is 0 and the
 * flag is 0 there even though the quadrant's DERIVED refIdxL0 sits in the grid
 * and can exceed 0. mbcbp bits 24-27 carry that per-quadrant direct bitmap
 * (x264's mb.skipbp). */
static int b_ref_nb(y264_frame_t *f, int nx, int ny)
{
    if (nx < 0 || ny < 0) return 0;
    int v = mbcbp_get(f, nx >> 2, ny >> 2);
    if (v < 0 || ((v >> 20) & 3)) return 0;
    int quad = (((ny >> 1) & 1) << 1) | ((nx >> 1) & 1);
    if ((v >> 24) & (1 << quad)) return 0;
    return f->refidx[ny * f->mv_stride + nx] > 0;
}

/* Author the B inter grids: the abs-mvd fields for every coded list/partition
 * (list 0 -> mvdx/mvdy, list 1 -> mvdx1/mvdy1) plus the inter tail (nnz + mbcbp);
 * no engine. Mirrors the emit order so the emit's neighbour context matches. */
static void author_b_inter_cabac(y264_frame_t *f, int mbx, int mby,
                                 const struct inter_result *ir)
{
    int useL0 = (ir->bmode == 0 || ir->bmode == 2);
    int useL1 = (ir->bmode == 1 || ir->bmode == 2);
    /* est mode, 16x16: single-partition mvd contexts read only cells outside
 * this MB, so the trial's mvd author is skipped (matches est_b_save). */
    if (ir->bpart == 0 && f->cabac && f->cabac->est_mode) {
        author_cabac_inter_tail(f, mbx, mby, ir, 0);
        return;
    }
    if (ir->bpart == 3) {                       /* B_8x8 */
        for (int b = 0; b < 4; b++) {
            int bx4 = mbx * 4 + (b & 1) * 2, by4 = mby * 4 + (b >> 1) * 2;
            int uL0 = (ir->b8m[b] == 1 || ir->b8m[b] == 3);
            int uL1 = (ir->b8m[b] == 2 || ir->b8m[b] == 3);
            /* A direct quadrant codes no mvd; author zeroes so a later
 * neighbour's context sees 0 rather than a stale value. */
            author_mvd(f, bx4, by4, 2, 2, uL0 ? ir->mvx[b] - ir->pmvx[b] : 0,
                       uL0 ? ir->mvy[b] - ir->pmvy[b] : 0, f->mvdx, f->mvdy);
            author_mvd(f, bx4, by4, 2, 2, uL1 ? ir->mvx[4 + b] - ir->pmvx[4 + b] : 0,
                       uL1 ? ir->mvy[4 + b] - ir->pmvy[4 + b] : 0, f->mvdx1, f->mvdy1);
        }
        /* Record which quadrants are B_Direct_8x8 so a later macroblock's
 * ref_idx condTermFlag can see them (b_ref_nb). */
        int skipbp = 0;
        for (int b = 0; b < 4; b++) skipbp |= (ir->b8m[b] == 0) << b;
        author_cabac_inter_tail(f, mbx, mby, ir, skipbp << 24);
        return;
    }
    if (ir->bpart) {
        int w4 = ir->bpart == 1 ? 4 : 2, h4 = ir->bpart == 1 ? 2 : 4;
        if (useL0)
            for (int p = 0; p < 2; p++) {
                int bx4, by4;
                part_topleft(mbx, mby, ir->bpart, p, &bx4, &by4);
                author_mvd(f, bx4, by4, w4, h4, ir->mvx[p] - ir->pmvx[p],
                           ir->mvy[p] - ir->pmvy[p], f->mvdx, f->mvdy);
            }
        if (useL1)
            for (int p = 0; p < 2; p++) {
                int bx4, by4;
                part_topleft(mbx, mby, ir->bpart, p, &bx4, &by4);
                author_mvd(f, bx4, by4, w4, h4, ir->mvx[2 + p] - ir->pmvx[2 + p],
                           ir->mvy[2 + p] - ir->pmvy[2 + p], f->mvdx1, f->mvdy1);
            }
    } else {
        int bx4 = mbx * 4, by4 = mby * 4;
        if (useL0)
            author_mvd(f, bx4, by4, 4, 4, ir->mvx[0] - ir->pmvx[0],
                       ir->mvy[0] - ir->pmvy[0], f->mvdx, f->mvdy);
        if (useL1)
            author_mvd(f, bx4, by4, 4, 4, ir->mvx[1] - ir->pmvx[1],
                       ir->mvy[1] - ir->pmvy[1], f->mvdx1, f->mvdy1);
    }
    author_cabac_inter_tail(f, mbx, mby, ir, 0);
}

/* B_L0 / B_L1 / B_Bi 16x16 (bmode 0/1/2); ref_idx_l0 when the list-0 range
 * allows a choice (list 1 stays single-ref). Emits bins reading authored grids.
 * tail_parts is the EST_PROF bench mask (see emit_cabac_inter_tail_ex); every
 * real caller passes 7 via the wrapper below. */
static void emit_b_inter_cabac_ex(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                               const struct inter_result *ir, int tail_parts)
{
    int ctx = b_mbtype_ctx(f, mbx, mby);
    y264_cabac_encode_decision(c, 27 + ctx, 1);           /* not B_Direct */
    if (ir->bpart == 3) {                                 /* B_8x8 */
        /* mb_type 22: five 1-bins after the prefix, contexts 27+3, 27+4 then
 * 27+5 (Table 9-34, B-slice mb_type binarisation). */
        y264_cabac_encode_decision(c, 30, 1);
        y264_cabac_encode_decision(c, 31, 1);
        y264_cabac_encode_decision(c, 32, 1);
        y264_cabac_encode_decision(c, 32, 1);
        y264_cabac_encode_decision(c, 32, 1);
        for (int b = 0; b < 4; b++) {          /* sub_mb_type (9.3.2.5 B table) */
            int sub = ir->b8m[b];
            if (sub == 0) { y264_cabac_encode_decision(c, 36, 0); continue; }
            y264_cabac_encode_decision(c, 36, 1);
            if (sub == 3) {                    /* B_Bi_8x8 */
                y264_cabac_encode_decision(c, 37, 1);
                y264_cabac_encode_decision(c, 38, 0);
                y264_cabac_encode_decision(c, 39, 0);
                y264_cabac_encode_decision(c, 39, 0);
            } else {
                y264_cabac_encode_decision(c, 37, 0);
                y264_cabac_encode_decision(c, 39, sub == 2);
            }
        }
        if (f->nref > 1)
            for (int b = 0; b < 4; b++) {
                if (!(ir->b8m[b] == 1 || ir->b8m[b] == 3)) continue;
                int bx4 = mbx * 4 + (b & 1) * 2, by4 = mby * 4 + (b >> 1) * 2;
                /* condTermFlag (9.3.3.1.1.6). Inside the macroblock the
 * neighbour quadrant decides it, and a DIRECT quadrant scores
 * 0 whatever its reference: SubMbPredMode(B_Direct_8x8) is
 * Direct, so predModeEqualFlag is 0. Its derived refIdxL0 does
 * sit in the grid and can exceed 0 once the list has more than
 * one entry, so reading the grid instead desyncs the decoder on
 * exactly that combination -- mixed sub-types plus --ref > 1 --
 * and nothing else. */
                int ra = (b & 1) ? (b8_codes_l0(ir->b8m[b - 1]) && ir->ref[b - 1] > 0)
                                 : b_ref_nb(f, bx4 - 1, by4);
                int rb = (b >= 2) ? (b8_codes_l0(ir->b8m[b - 2]) && ir->ref[b - 2] > 0)
                                  : b_ref_nb(f, bx4, by4 - 1);
                int inc = ra + 2 * rb;
                y264_cabac_encode_decision(c, 54 + inc, ir->ref[b] > 0);
                for (int k = 1; k <= ir->ref[b]; k++)
                    y264_cabac_encode_decision(c, 54 + (k == 1 ? 4 : 5), k < ir->ref[b]);
            }
        for (int b = 0; b < 4; b++)
            if (ir->b8m[b] == 1 || ir->b8m[b] == 3) {
                int bx4 = mbx * 4 + (b & 1) * 2, by4 = mby * 4 + (b >> 1) * 2;
                emit_mvd(c, f, bx4, by4, ir->mvx[b] - ir->pmvx[b],
                         ir->mvy[b] - ir->pmvy[b], f->mvdx, f->mvdy);
            }
        for (int b = 0; b < 4; b++)
            if (ir->b8m[b] == 2 || ir->b8m[b] == 3) {
                int bx4 = mbx * 4 + (b & 1) * 2, by4 = mby * 4 + (b >> 1) * 2;
                emit_mvd(c, f, bx4, by4, ir->mvx[4 + b] - ir->pmvx[4 + b],
                         ir->mvy[4 + b] - ir->pmvy[4 + b], f->mvdx1, f->mvdy1);
            }
        if (tail_parts) emit_cabac_inter_tail_ex(c, f, mbx, mby, ir, tail_parts);
        return;
    }
    if (ir->bpart) {                                      /* two-partition modes */
        /* Packed-bit mb_type suffix (LSB-first, sentinel-1 terminated); the
 * second bin's context is 31 when the first suffix bin is 1, else 32.
 * The same loop reproduces our 16x16 binarizations bit-exactly. */
        int bits = B_PART_CBITS[ir->bmode][ir->bpart - 1];
        y264_cabac_encode_decision(c, 30, bits & 1);
        y264_cabac_encode_decision(c, 32 - (bits & 1), (bits >> 1) & 1);
        bits >>= 2;
        if (bits != 1) {
            y264_cabac_encode_decision(c, 32, bits & 1); bits >>= 1;
            y264_cabac_encode_decision(c, 32, bits & 1); bits >>= 1;
            y264_cabac_encode_decision(c, 32, bits & 1); bits >>= 1;
            if (bits != 1)
                y264_cabac_encode_decision(c, 32, bits & 1);
        }
        int useL0 = (ir->bmode == 0 || ir->bmode == 2);
        int useL1 = (ir->bmode == 1 || ir->bmode == 2);
        if (f->nref > 1 && useL0)
            for (int p = 0; p < 2; p++) {
                int bx4, by4;
                part_topleft(mbx, mby, ir->bpart, p, &bx4, &by4);
                int ra = (bx4 - 1 >= mbx * 4) ? (ir->ref[0] > 0)
                                              : b_ref_nb(f, bx4 - 1, by4);
                int rb = (by4 - 1 >= mby * 4) ? (ir->ref[0] > 0)
                                              : b_ref_nb(f, bx4, by4 - 1);
                int inc = ra + 2 * rb;
                y264_cabac_encode_decision(c, 54 + inc, ir->ref[p] > 0);
                for (int b = 1; b <= ir->ref[p]; b++)
                    y264_cabac_encode_decision(c, 54 + (b == 1 ? 4 : 5), b < ir->ref[p]);
            }
        if (useL0)
            for (int p = 0; p < 2; p++) {
                int bx4, by4;
                part_topleft(mbx, mby, ir->bpart, p, &bx4, &by4);
                emit_mvd(c, f, bx4, by4, ir->mvx[p] - ir->pmvx[p],
                         ir->mvy[p] - ir->pmvy[p], f->mvdx, f->mvdy);
            }
        if (useL1)
            for (int p = 0; p < 2; p++) {
                int bx4, by4;
                part_topleft(mbx, mby, ir->bpart, p, &bx4, &by4);
                emit_mvd(c, f, bx4, by4, ir->mvx[2 + p] - ir->pmvx[2 + p],
                         ir->mvy[2 + p] - ir->pmvy[2 + p], f->mvdx1, f->mvdy1);
            }
        if (tail_parts) emit_cabac_inter_tail_ex(c, f, mbx, mby, ir, tail_parts);
        return;
    }
    if (ir->bmode == 0) {                                 /* B_L0_16x16 */
        y264_cabac_encode_decision(c, 30, 0);
        y264_cabac_encode_decision(c, 32, 0);
    } else if (ir->bmode == 1) {                          /* B_L1_16x16 */
        y264_cabac_encode_decision(c, 30, 0);
        y264_cabac_encode_decision(c, 32, 1);
    } else {                                              /* B_Bi_16x16 */
        y264_cabac_encode_decision(c, 30, 1);
        y264_cabac_encode_decision(c, 31, 0);
        y264_cabac_encode_decision(c, 32, 0);
        y264_cabac_encode_decision(c, 32, 0);
        y264_cabac_encode_decision(c, 32, 0);
    }

    int bx4 = mbx * 4, by4 = mby * 4;
    if (f->nref > 1 && (ir->bmode == 0 || ir->bmode == 2)) {   /* ref_idx_l0 */
        int inc = b_ref_nb(f, bx4 - 1, by4) + 2 * b_ref_nb(f, bx4, by4 - 1);
        y264_cabac_encode_decision(c, 54 + inc, ir->ref[0] > 0);
        for (int b = 1; b <= ir->ref[0]; b++)
            y264_cabac_encode_decision(c, 54 + (b == 1 ? 4 : 5), b < ir->ref[0]);
    }
    if (ir->bmode == 0 || ir->bmode == 2)                 /* mvd_l0 */
        emit_mvd(c, f, bx4, by4, ir->mvx[0] - ir->pmvx[0], ir->mvy[0] - ir->pmvy[0],
                 f->mvdx, f->mvdy);
    if (ir->bmode == 1 || ir->bmode == 2)                 /* mvd_l1 */
        emit_mvd(c, f, bx4, by4, ir->mvx[1] - ir->pmvx[1], ir->mvy[1] - ir->pmvy[1],
                 f->mvdx1, f->mvdy1);

    if (tail_parts) emit_cabac_inter_tail_ex(c, f, mbx, mby, ir, tail_parts);
}

static void emit_b_inter_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                               const struct inter_result *ir)
{
    emit_b_inter_cabac_ex(c, f, mbx, mby, ir, 7);
}

/* Single-pass wrapper: author grids then emit bins. */
static void write_b_inter_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                const struct inter_result *ir)
{
    author_b_inter_cabac(f, mbx, mby, ir);
    emit_b_inter_cabac(c, f, mbx, mby, ir);
}

/* slice: 0 = I slice, 1 = intra MB inside a P slice, 2 = inside a B slice. */
/* 4:4:4 CABAC: one I_16x16 component (0=Y,1=Cb,2=Cr) — DC then AC, using the
 * per-component ctxBlockCat (Y 0/1, Cb 6/7, Cr 10/11) and coded_block_flag
 * neighbour terms (DC from the mbcbp DC-cbf bit, AC from the component nnz grid).
 * Returns the DC coded_block_flag. */
/* Author the 4:4:4 I_16x16 component nnz grid (AC blocks) from the levels, and
 * return the DC-presence bit; no engine. The nnz cell is the coder's total_coeff
 * (= nonzero count), matching what y264_cabac_residual returns. */
static int author444_i16_comp_nnz(y264_frame_t *f, int mbx, int mby,
                                  int comp, int cbp, const struct luma_result *lr)
{
    int stride = f->nnz_stride[comp];
    int8_t *nnz = f->nnz[comp];
    int bx0 = mbx * 4, by0 = mby * 4;
    int dc_cbf = nz_count(lr->dc_scan, 16) > 0;
    for (int i = 0; i < 16; i++) {
        int ax = bx0 + BLK_X[i], ay = by0 + BLK_Y[i];
        nnz[ay * stride + ax] = cbp ? (int8_t)nz_count(lr->ac_scan[i], 15) : 0;
    }
    return dc_cbf;
}

/* Emit one 4:4:4 I_16x16 component's residual bins (DC always, AC when cbp),
 * reading the authored nnz for the AC coded_block_flag context. */
static void emit444_i16_comp_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                   int comp, int cbp, const struct luma_result *lr)
{
    static const int DCCAT[3] = { 0, 6, 10 }, ACCAT[3] = { 1, 7, 11 }, DCBIT[3] = { 8, 9, 10 };
    int bx0 = mbx * 4, by0 = mby * 4;
    int nza = dc_nb(f, mbx - 1, mby, DCBIT[comp], 1);
    int nzb = dc_nb(f, mbx, mby - 1, DCBIT[comp], 1);
    y264_cabac_residual(c, DCCAT[comp], lr->dc_scan, nza, nzb);
    if (!cbp) return;
    for (int i = 0; i < 16; i++) {
        int ax = bx0 + BLK_X[i], ay = by0 + BLK_Y[i];
        int a = cbf_nb(f, comp, ax - 1, ay, 1), b = cbf_nb(f, comp, ax, ay - 1, 1);
        /* AC cats read exactly 15 coefficients: pass the scan row as-is */
        y264_cabac_residual(c, ACCAT[comp], lr->ac_scan[i], a, b);
    }
}

/* 4:4:4 intra author: nnz for all three components + the mbcbp cache (shared cbp,
 * per-component DC presence, use_i4 flag); no engine. Pass 1 calls this. */
static void author_intra444_cabac(y264_frame_t *f, int mbx, int mby, const struct intra_mb *o)
{
    int bx0 = mbx * 4, by0 = mby * 4;
    const struct luma_result *lr = &o->lr;
    const struct i4_result *ir = &o->ir;
    int use_i4 = o->use_i4;
    int cbp = use_i4 ? (ir->cbp_luma | o->ir_c[0].cbp_luma | o->ir_c[1].cbp_luma)
                     : ((lr->cbp_luma || o->lr_c[0].cbp_luma || o->lr_c[1].cbp_luma) ? 0xf : 0);
    int y_dc = 0, cb_dc = 0, cr_dc = 0;
    if (cbp || !use_i4) {
        if (!use_i4) {
            y_dc  = author444_i16_comp_nnz(f, mbx, mby, 0, cbp, lr);
            cb_dc = author444_i16_comp_nnz(f, mbx, mby, 1, cbp, &o->lr_c[0]);
            cr_dc = author444_i16_comp_nnz(f, mbx, mby, 2, cbp, &o->lr_c[1]);
        } else {
            author_444_comp_nnz(f, mbx, mby, 0, cbp, ir->lev);
            author_444_comp_nnz(f, mbx, mby, 1, cbp, o->ir_c[0].lev);
            author_444_comp_nnz(f, mbx, mby, 2, cbp, o->ir_c[1].lev);
        }
    } else {
        for (int comp = 0; comp < 3; comp++) {
            int stride = f->nnz_stride[comp]; int8_t *nnz = f->nnz[comp];
            for (int i = 0; i < 16; i++) nnz[(by0 + BLK_Y[i]) * stride + (bx0 + BLK_X[i])] = 0;
        }
    }
    if (f->mb_tr8) f->mb_tr8[mby * f->wmb + mbx] = 0;
    f->mbcbp[mby * f->mbcbp_stride + mbx] =
        (cbp & 0xf) | (y_dc << 8) | (cb_dc << 9) | (cr_dc << 10) | (use_i4 << 11);
}

/* 4:4:4 intra emit: mb_type, i4 modes, cbp_luma, residuals; reads the authored
 * grids. Cb/Cr coded like luma, one shared cbp, no intra_chroma_pred_mode. */
static void emit_intra444_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                const struct intra_mb *o, int slice)
{
    int bx0 = mbx * 4, by0 = mby * 4, ms = f->i4mode_stride;
    const struct luma_result *lr = &o->lr;
    const struct i4_result *ir = &o->ir;
    int use_i4 = o->use_i4;
    int la = mbcbp_get(f, mbx - 1, mby), lt = mbcbp_get(f, mbx, mby - 1);
    int cbp = use_i4 ? (ir->cbp_luma | o->ir_c[0].cbp_luma | o->ir_c[1].cbp_luma)
                     : ((lr->cbp_luma || o->lr_c[0].cbp_luma || o->lr_c[1].cbp_luma) ? 0xf : 0);
    int mtcbpl = use_i4 ? (cbp != 0) : (cbp ? 1 : 0);

    /* mb_type: the P/B-slice intra prefix must precede the intra mb_type (like the
 * non-4:4:4 path; cbp_chroma = 0 for 4:4:4). Without this, an intra MB in a P/B
 * slice codes the I-slice binarization and desyncs the decoder at the next MB. */
    if (slice == 1) {                       /* intra MB in a P slice */
        y264_cabac_encode_decision(c, 14, 1);
        cabac_mb_type_intra(c, use_i4, mtcbpl, 0, lr->mode, 17, 18, 19, 19, 20, 20);
    } else if (slice == 2) {                /* intra MB in a B slice */
        y264_cabac_encode_decision(c, 27 + b_mbtype_ctx(f, mbx, mby), 1);
        y264_cabac_encode_decision(c, 27 + 3, 1);
        y264_cabac_encode_decision(c, 27 + 4, 1);
        y264_cabac_encode_decision(c, 27 + 5, 1);
        y264_cabac_encode_decision(c, 27 + 5, 0);
        y264_cabac_encode_decision(c, 27 + 5, 1);
        cabac_mb_type_intra(c, use_i4, mtcbpl, 0, lr->mode, 32, 33, 34, 34, 35, 35);
    } else {                                /* I slice */
        int ctx0 = 3 + (la >= 0 && !((la >> 11) & 1)) + (lt >= 0 && !((lt >> 11) & 1));
        cabac_mb_type_i(c, use_i4, mtcbpl, 0, lr->mode, ctx0);
    }
    if (use_i4)
        for (int blk = 0; blk < 16; blk++) {
            int ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
            int pm;
            if (ay == 0 || ax == 0) pm = 2;
            else { int a = f->i4mode[ay*ms+(ax-1)], b = f->i4mode[(ay-1)*ms+ax]; pm = a < b ? a : b; }
            cabac_intra4x4_mode(c, pm, ir->mode[blk]);
        }
    /* no intra_chroma_pred_mode for 4:4:4 */
    if (use_i4) cabac_cbp_luma(c, cbp, la, lt);   /* no cabac_cbp_chroma */

    if (cbp || !use_i4) {
        cabac_mb_qp_delta(c, f, f->cur_qp);
        if (!use_i4) {
            emit444_i16_comp_cabac(c, f, mbx, mby, 0, cbp, lr);
            emit444_i16_comp_cabac(c, f, mbx, mby, 1, cbp, &o->lr_c[0]);
            emit444_i16_comp_cabac(c, f, mbx, mby, 2, cbp, &o->lr_c[1]);
        } else {
            emit444_i4_comp_cabac(c, f, mbx, mby, 0, cbp, 1, ir->lev);
            emit444_i4_comp_cabac(c, f, mbx, mby, 1, cbp, 1, o->ir_c[0].lev);
            emit444_i4_comp_cabac(c, f, mbx, mby, 2, cbp, 1, o->ir_c[1].lev);
        }
    }
}


/* Author the 4:2:0/4:2:2 intra CABAC grids (luma + chroma nnz, mbcbp cache with
 * DC-presence + chroma pred-mode + transform-8x8 bits) from the decided result;
 * no engine. The I_8x8 nnz cell carries the whole-block count in all four 4x4
 * cells (the CABAC coded_block_flag neighbour convention), unlike CAVLC. */
static void author_intra_cabac_420(y264_frame_t *f, int mbx, int mby, const struct intra_mb *o)
{
    int lstride = f->nnz_stride[0];
    int8_t *lnnz = f->nnz[0];
    int bx0 = mbx * 4, by0 = mby * 4;
    const struct luma_result *lr = &o->lr;
    const struct i4_result *ir = &o->ir;
    const struct chroma_result *cr = &o->cr;
    int use_i4 = o->use_i4, use_i8 = o->use_i8;
    int cbp_luma = use_i8 ? o->i8.cbp_luma : (use_i4 ? ir->cbp_luma : (lr->cbp_luma ? 0xf : 0));
    int cbp_chroma = cr->cbp;

    int luma_dc_cbf = 0, cb_dc = 0, cr_dc = 0;
    if (cbp_luma || cbp_chroma || !use_i4) {
        if (!use_i4) {                        /* I_16x16 */
            luma_dc_cbf = nz_count(lr->dc_scan, 16) > 0;
            for (int i = 0; i < 16; i++) {
                int ax = bx0 + BLK_X[i], ay = by0 + BLK_Y[i];
                lnnz[ay * lstride + ax] = lr->cbp_luma ? (int8_t)nz_count(lr->ac_scan[i], 15) : 0;
            }
        } else if (use_i8) {                     /* I_8x8: whole-block count in all 4 cells */
            for (int blk = 0; blk < 4; blk++) {
                int nzv = 0;
                if (cbp_luma & (1 << blk))
                    /* count is scan-order invariant: no zigzag gather needed */
                    nzv = nz_count(o->i8.lev[blk], 64);
                for (int j = 0; j < 4; j++) {
                    int lb = blk * 4 + j;
                    lnnz[(by0 + BLK_Y[lb]) * lstride + (bx0 + BLK_X[lb])] = (int8_t)nzv;
                }
            }
        } else {                                 /* I_NxN, 4x4 transform */
            for (int i8 = 0; i8 < 4; i8++)
                for (int i4 = 0; i4 < 4; i4++) {
                    int blk = i8 * 4 + i4;
                    int ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                    lnnz[ay * lstride + ax] =
                        (cbp_luma & (1 << i8)) ? (int8_t)nz_count(ir->lev[blk], 16) : 0;
                }
        }
    } else {
        for (int i = 0; i < 16; i++)
            lnnz[(by0 + BLK_Y[i]) * lstride + (bx0 + BLK_X[i])] = 0;
    }

    author_cabac_chroma_nnz(f, mbx, mby, cr, &cb_dc, &cr_dc);

    if (f->mb_tr8)
        f->mb_tr8[mby * f->wmb + mbx] = (pixel)use_i8;

    /* Record this MB's cbp cache for neighbour context. */
    f->mbcbp[mby * f->mbcbp_stride + mbx] =
        (cbp_luma & 0xf) | (cbp_chroma << 4) |
        (luma_dc_cbf << 8) | (cb_dc << 9) | (cr_dc << 10) |
        (o->use_i4 << 11) | (cr->mode << 12) | (use_i8 << 22);
}

/* Emit the 4:2:0/4:2:2 intra CABAC bins (mb_type, transform-8x8, i4 modes,
 * chroma pred mode, cbp, residuals) reading the authored grids. */
static void emit_intra_cabac_420(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                                 const struct intra_mb *o, int slice)
{
    int ms = f->i4mode_stride;
    int bx0 = mbx * 4, by0 = mby * 4;
    const struct luma_result *lr = &o->lr;
    const struct i4_result *ir = &o->ir;
    const struct chroma_result *cr = &o->cr;

    int use_i4 = o->use_i4, use_i8 = o->use_i8;
    int la = mbcbp_get(f, mbx - 1, mby), lt = mbcbp_get(f, mbx, mby - 1);
    int cbp_luma = use_i8 ? o->i8.cbp_luma : (use_i4 ? ir->cbp_luma : (lr->cbp_luma ? 0xf : 0));
    int cbp_chroma = cr->cbp;
    int mtcbpl = use_i4 ? (ir->cbp_luma != 0) : lr->cbp_luma;
    if (slice == 1) {                       /* intra MB in a P slice */
        y264_cabac_encode_decision(c, 14, 1);
        cabac_mb_type_intra(c, use_i4, mtcbpl, cbp_chroma, lr->mode,
                            17, 18, 19, 19, 20, 20);
    } else if (slice == 2) {                /* intra MB in a B slice */
        y264_cabac_encode_decision(c, 27 + b_mbtype_ctx(f, mbx, mby), 1);  /* not B_Direct */
        y264_cabac_encode_decision(c, 27 + 3, 1);
        y264_cabac_encode_decision(c, 27 + 4, 1);
        y264_cabac_encode_decision(c, 27 + 5, 1);
        y264_cabac_encode_decision(c, 27 + 5, 0);
        y264_cabac_encode_decision(c, 27 + 5, 1);
        cabac_mb_type_intra(c, use_i4, mtcbpl, cbp_chroma, lr->mode,
                            32, 33, 34, 34, 35, 35);
    } else {                                /* I slice */
        int ctx0 = 3 + (la >= 0 && !((la >> 11) & 1)) + (lt >= 0 && !((lt >> 11) & 1));
        cabac_mb_type_i(c, use_i4, mtcbpl, cbp_chroma, lr->mode, ctx0);
    }

    if (use_i4 && f->transform8x8)
        cabac_transform_8x8_flag(c, f, mbx, mby, use_i8);
    if (use_i4) {
        int nblk = use_i8 ? 4 : 16;
        for (int blk = 0; blk < nblk; blk++) {
            int ax = bx0 + (use_i8 ? B8_X[blk] : BLK_X[blk]);
            int ay = by0 + (use_i8 ? B8_Y[blk] : BLK_Y[blk]);
            int pred_mode;
            if (ay == 0 || ax == 0) pred_mode = 2;
            else {
                int a = f->i4mode[ay * ms + (ax - 1)];
                int b = f->i4mode[(ay - 1) * ms + ax];
                pred_mode = a < b ? a : b;
            }
            cabac_intra4x4_mode(c, pred_mode, use_i8 ? o->i8.mode[blk] : ir->mode[blk]);
        }
    }
    cabac_chroma_pred_mode(c, f, mbx, mby, cr->mode);

    if (use_i4) {
        cabac_cbp_luma(c, cbp_luma, la, lt);
        cabac_cbp_chroma(c, cbp_chroma, la, lt);
    }

    if (cbp_luma || cbp_chroma || !use_i4) {
        cabac_mb_qp_delta(c, f, f->cur_qp);     /* mb_qp_delta */

        if (!use_i4) {                        /* I_16x16 */
            int nza = dc_nb(f, mbx - 1, mby, 8, 1), nzb = dc_nb(f, mbx, mby - 1, 8, 1);
            y264_cabac_residual(c, 0, lr->dc_scan, nza, nzb);
            if (lr->cbp_luma)
                for (int i = 0; i < 16; i++) {
                    int ax = bx0 + BLK_X[i], ay = by0 + BLK_Y[i];
                    int a = cbf_nb(f, 0, ax - 1, ay, 1), b = cbf_nb(f, 0, ax, ay - 1, 1);
                    /* cat 1 reads exactly 15 coefficients: pass the row as-is */
                    y264_cabac_residual(c, 1, lr->ac_scan[i], a, b);
                }
        } else if (use_i8) {                     /* I_8x8: one 8x8 residual per set cbp bit */
            for (int blk = 0; blk < 4; blk++)
                if (cbp_luma & (1 << blk)) {
                    if (c->est_mode) {          /* fused gather, bit-exact */
                        y264_cabac_residual_8x8_est(c, o->i8.lev[blk]);
                    } else {
                        dctcoef scan8[64];
                        for (int k = 0; k < 64; k++) scan8[k] = o->i8.lev[blk][ZIGZAG8[k]];
                        y264_cabac_residual_8x8(c, scan8);
                    }
                }
        } else {                                 /* I_NxN, 4x4 transform */
            for (int i8 = 0; i8 < 4; i8++)
                for (int i4 = 0; i4 < 4; i4++) {
                    int blk = i8 * 4 + i4;
                    int ax = bx0 + BLK_X[blk], ay = by0 + BLK_Y[blk];
                    if (cbp_luma & (1 << i8)) {
                        int a = cbf_nb(f, 0, ax - 1, ay, 1), b = cbf_nb(f, 0, ax, ay - 1, 1);
                        y264_cabac_residual(c, 2, ir->lev[blk], a, b);
                    }
                }
        }
    }

    emit_cabac_chroma_residual(c, f, mbx, mby, cr, 1);
}

/* Author the intra CABAC grids (4:4:4 or 4:2:0/4:2:2). */
static void author_intra_cabac(y264_frame_t *f, int mbx, int mby, const struct intra_mb *o)
{
    if (f->cf_idc == 3) author_intra444_cabac(f, mbx, mby, o);
    else author_intra_cabac_420(f, mbx, mby, o);
}

/* Emit the intra CABAC bins (4:4:4 or 4:2:0/4:2:2). */
static void emit_intra_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                             const struct intra_mb *o, int slice)
{
    if (f->cf_idc == 3) emit_intra444_cabac(c, f, mbx, mby, o, slice);
    else emit_intra_cabac_420(c, f, mbx, mby, o, slice);
}

/* Single-pass wrapper: author grids then emit bins. */
static void write_intra_cabac(y264_cabac_t *c, y264_frame_t *f, int mbx, int mby,
                              const struct intra_mb *o, int slice)
{
    author_intra_cabac(f, mbx, mby, o);
    emit_intra_cabac(c, f, mbx, mby, o, slice);
}

/* Does any coefficient in [k0,16) of a forward-transformed 4x4 quantize nonzero?
 * strict=0: the actual deadzone quant (y264_quant_4x4) -- an exact "cbp>0" test.
 * strict=1: round-to-nearest (survives iff |coef|*mf >= 2^(qbits-1)), a much
 * tighter "is this residual truly negligible" gate used for the B_Skip probe,
 * whose derived direct MVs are guesses (the aggressive deadzone over-skips). */
static int coef_signif(const dctcoef coef[16], int qp, int strict,
                       const uint8_t *w, int k0)
{
    if (!strict) {
        dctcoef lev[16];
        /* The probe must ask "would the coder code anything?", so it has to
 * quantize with the coder's own rounding. With the RDOQ seed override
 * active the trellis seeds round-to-nearest and keeps a superset of the
 * deadzone quant; testing with the plain deadzone would false-skip MBs
 * whose marginal HF residual the coder retains (measured ~3% BD on
 * bus/coastguard at flat CQP). */
        int s64 = probe_deadzone_env() ? -1 : rdoq_seed64();
        if (s64 >= 0) y264_quant_4x4_f64(coef, lev, qp, s64, w);
        else          y264_quant_4x4(coef, lev, qp, 0, w);
        for (int k = k0; k < 16; k++) if (lev[k]) return 1;
        return 0;
    }
    long thr = 1L << (15 + qp / 6 - 1);
    const int *mfr = y264_mf4_row(qp);        /* same numbers, built once */
    for (int k = k0; k < 16; k++) {
        int mf = mfr[k];
        if (w) mf = (mf * 16 + (w[k] >> 1)) / w[k];
        int a = coef[k] < 0 ? -coef[k] : coef[k];
        if ((long)a * mf >= thr) return 1;
    }
    return 0;
}

/* x264-style early Skip probe (subme<=8). The skip/direct MC prediction is
 * already in f->rec; forward-transform the residual for luma and chroma and
 * early-out on the first surviving coefficient. All-zero means the skip codes to
 * nothing (cbp 0 at the predicted MV), so it wins RD outright and we can commit it
 * without ME / partition / intra analysis. Deadzone quant (strict=0) keeps a
 * superset of what RDOQ+decimate would, so it never yields a false P_Skip; B uses
 * strict=1 (round-to-nearest) because direct MVs need a tighter gate. */
/* Trellis-aligned significance for the P skip probe. The seed quant keeps a
 * SUPERSET of what the coder finally codes (the trellis prunes marginal
 * levels by RD), so admitting on the seed alone sends MBs to full analysis
 * that still code to nothing -- the bulk of the seed-fix speed cost (~8% of
 * a 720p encode). When the seed keeps something, ask
 * the coder's own medium-tier Viterbi trellis whether any level survives;
 * all-zero means the skip truly codes to nothing and wins RD outright.
 * Falls back to seed-admission when the trellis
 * model doesn't apply (CAVLC, psy-trellis, greedy tiers).
 * Y264_PROBE_TRELLIS=0 restores the seed-only probe. */
static int probe_trellis_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PROBE_TRELLIS"); v = e ? atoi(e) : 1; }
    return v;
}
/* PROBE_KEEP: a level the decimator can never drop (|level| >= 2, x264's
 * <reference-internal> returns 9 for the same case), or a block the probe
 * cannot model. Any value at or above the mode-2 threshold and above every
 * per-block threshold, so it fails both acceptance tests. */
#define PROBE_KEEP 99

/* Significance of one 4x4 as the skip probe sees it. Sets *any when a level
 * survives at all (the pre-decimate answer, which is mode 0's verdict and has
 * to stay exact); returns the run-weighted decimate score the coder's own
 * decimator would compute over those levels.
 *
 * The two are not the same question, and that is the defect this answers. A
 * lone +-1 in the last scan position scores 0 -- DECIMATE_T4[15] is 0 -- so it
 * is a level that survived the trellis AND a block inter_res_4x4_plane goes on
 * to zero. Asking only the first question sends macroblocks our own coder
 * codes to cbp 0 through a full ME + intra + RD tournament, to be discarded
 * as skip anyway. */
static int probe_signif_rdoq(y264_frame_t *f, const dctcoef coef[16],
                             int qp, int qpm, int cat, int ac,
                             int nza, int nzb, const uint8_t *w, int *any,
                             int notr)
{
    dctcoef lev[16];
    int s64 = rdoq_seed64();
    if (s64 >= 0) y264_quant_4x4_f64(coef, lev, qp, s64, w);
    else          y264_quant_4x4(coef, lev, qp, 0, w);
    if (ac) lev[0] = 0;
    *any = 0;
    for (int k = ac ? 1 : 0; k < 16; k++) if (lev[k]) { *any = 1; break; }
    if (!*any) return 0;
    int base = ac ? 1 : 0, n = ac ? 15 : 16;
    int qn[16], absc[16], w2[16], out[16];
    if (notr || !probe_trellis_on() || !f->cabac || psy_trellis_qp(f, qpm)
        || !viterbi_rdoq(f->subme)) {
        /* Trellis model doesn't apply (CAVLC, psy-trellis, greedy tiers): score
 * the SEED levels instead. They are a superset of what the coder finally
 * keeps, so the score is an over-estimate and the acceptance stays
 * conservative -- strictly better than an unconditional reject. */
        for (int i = 0; i < n; i++) out[i] = lev[ZIGZAG[i + base]];
    } else {
        long unmf[16];
        const long *ur = w ? NULL : y264_unquant4_row(qp);   /* flat CQM: no per-coef calls */
        const int *wr = y264_dct4_w2_row();
        for (int i = 0; i < n; i++) {
            int r = ZIGZAG[i + base];
            int lv = lev[r];
            qn[i] = lv < 0 ? -lv : lv;
            int cf = coef[r];
            absc[i] = cf < 0 ? -cf : cf;
            unmf[i] = ur ? ur[r] : y264_unquant4_mf(r, qp, w);
            w2[i] = wr[r];
        }
        y264_cabac_trellis_4x4(f->cabac, cat, nza, nzb, lambda_trellis(qpm, 0),
                               n, qn, absc, unmf, w2, 0, NULL, 0, out);
    }
    /* Re-ask *any against the levels the coder would actually emit: the trellis
 * can prune the seed to nothing, and mode 0 treats that as a
 * pass. Same scan-order mask feeds the decimate walk. */
    uint32_t msk = 0;
    for (int i = 0; i < n; i++) {
        int v = out[i];
        if (!v) continue;
        if ((unsigned)(v + 1) > 2u) { *any = 1; return PROBE_KEEP; }  /* |level| >= 2 */
        msk |= 1u << (i + base);
    }
    *any = msk != 0;
    return msk ? decimate_mask(msk, dctdec_tab4()) : 0;
}

/* `tol`, when non-NULL, comes back 1 if this acceptance DEPARTS from the shipped
 * strict test -- either some block spent the decimation tolerance, or the
 * deadzone+trellis quantizer zeroed a coefficient that round-to-nearest would
 * have kept. 0 means the shipped test would have accepted it too.
 *
 * That is the set that owes a confirmation, and getting it wrong is
 * measurable. Committing on "the coder codes no coefficients"
 * looks tolerance-free and is not: the deadzone bin is much wider than
 * round-to-nearest, so it accepts macroblocks that carry real residual energy
 * the quantizer happens to discard at this QP, and a searched MV would often
 * have won there. Doing that lifts samsung's B early catch 51.1% -> 67.2% and
 * costs mobile +16.9% / tempete +10.9% BD-VMAF-NEG. So the departure, not the
 * decimate score, is what has to be confirmed.
 *
 * With that definition !tol is exactly the shipped accept set, which makes the
 * probe-only arm byte-identical by construction rather than by luck.
 *
 * That split is what lets the confirmation path run ONE probe instead of two.
 * Running the strict test first and the tolerant one after it costs a second
 * full quantize+trellis on every macroblock the strict test rejects, which at
 * the high operating point is nearly all of them (bus at 2500: strict catches
 * 0.0% of B, so 100% paid twice and 2.0% converted) -- and that is a net LOSS
 * before the gate has skipped anything. x264 runs the B skip probe once. */
static int probe_skip(y264_frame_t *f, int mbx, int mby, int strict, int dec)
{
    return probe_skip_g(f, mbx, mby, strict, dec, NULL);
}

static int probe_skip_g(y264_frame_t *f, int mbx, int mby, int strict, int dec,
                        int *tol)
{
    if (tol) *tol = 0;
    /* Cost probe only (Y264_BSKIP_NOTRELLIS). Scoped to the deferred B path by
 * `tol`, so the P probe is untouched and this cannot be confused with a P
 * result. Seed levels are a superset of what the trellis keeps, so dropping
 * the trellis makes the acceptance strictly more conservative. */
    int notr = tol && f->bskip_notrellis;
    const uint8_t *lw = cqm_w4(f, 0);
    int ss = f->src_stride[0], rs = f->rec_stride[0];
    const pixel *src = f->src[0] + (mby * 16) * ss + mbx * 16;
    const pixel *pred = f->rec[0] + (mby * 16) * rs + mbx * 16;
    int bx0 = mbx * 4, by0 = mby * 4;
    /* dec: how tolerant the luma acceptance is (frame field, see macroblock.h).
 * B reaches this path only when skipdec_b asked for it; strict=1 keeps the
 * old round-to-nearest B test. dt4 is the coder's own per-block threshold,
 * so mode 1 accepts exactly the blocks inter_res_4x4_plane would zero. */
    if (strict) dec = 0;
    int dt4, dt8, dcON = dctdec_cfg(&dt4, &dt8);
    (void)dt8;
    if (!dcON) dec = 0;                 /* decimation off: nothing to be consistent with */
    int mb_score = 0;
    for (int blk = 0; blk < 16; blk++) {
        int bx = BLK_X[blk], by = BLK_Y[blk];
        dctcoef coef[16];
        y264_sub4x4_dct(coef, src + (by * 4) * ss + bx * 4, ss,
                        pred + (by * 4) * rs + bx * 4, rs);
        if (strict) {
            if (coef_signif(coef, f->cur_qp_scaled, strict, lw, 0)) return 0;
            continue;
        }
        /* Exact cbf neighbour ctx: within-MB predecessors all coded to
 * nothing (or we'd have bailed), cross-MB from the frame grid.
 * Decimated blocks carry nnz 0, so this invariant survives modes 1-3
 * unchanged -- a block we accept is a block the coder zeroes. */
        int nza = bx > 0 ? 0 : cbf_nb(f, 0, bx0 + bx - 1, by0 + by, 0);
        int nzb = by > 0 ? 0 : cbf_nb(f, 0, bx0 + bx, by0 + by - 1, 0);
        int any;
        int s = probe_signif_rdoq(f, coef, f->cur_qp_scaled, f->cur_qp,
                                  2, 0, nza, nzb, lw, &any, notr);
        /* Same DCT, so asking the shipped test as well costs a compare walk and
 * not a second probe. Two full probes are a net loss at the high
 * point. */
        if (tol && !*tol && coef_signif(coef, f->cur_qp_scaled, 1, lw, 0))
            *tol = 1;
        if (!any) continue;                     /* codes to nothing either way */
        if (!dec) return 0;                     /* mode 0: any surviving level fails */
        if (dec & 1) {                          /* coder-consistent, per block */
            if (s < dt4) { if (tol) *tol = 1; continue; }  /* the coder zeroes it */
            if (dec == 1) return 0;
        }
        /* mode 2: x264's whole-MB accumulation. Its
 * granularity differs from ours rather than merely being looser -- one
 * block scoring 5 fails mode 1 and passes mode 2, sixteen blocks
 * scoring 2 pass mode 1 and fail mode 2 -- so the two are attributed
 * separately. Mode 3 is the composition, not a union: our decimator
 * runs first (those blocks code to nothing, so they spend none of the
 * budget) and x264's MB budget then applies to what survives it. */
        if (s >= PROBE_KEEP) return 0;
        if (tol) *tol = 1;
        mb_score += s;
        if (mb_score >= f->skipdec_t) return 0;
    }
    if (f->cf_idc == 3) {                       /* 4:4:4: chroma coded like luma */
        for (int c = 1; c <= 2; c++) {
            int css = f->src_stride[c], crs = f->rec_stride[c];
            const pixel *csrc = f->src[c] + (mby*16)*css + mbx*16;
            const pixel *cpred = f->rec[c] + (mby*16)*crs + mbx*16;
            for (int blk = 0; blk < 16; blk++) {
                int bx = BLK_X[blk], by = BLK_Y[blk];
                dctcoef coef[16];
                y264_sub4x4_dct(coef, csrc + (by * 4) * css + bx * 4, css,
                                cpred + (by * 4) * crs + bx * 4, crs);
                if (coef_signif(coef, f->cur_qp_scaled, strict, lw, 0)) return 0;
            }
        }
        return 1;
    }
    int cw = 16 / f->sub_w, ch = 16 / f->sub_h;
    int cbw = f->cbw, cbh = f->cbh, nblk = cbw * cbh;
    for (int c = 0; c < 2; c++) {
        int css = f->src_stride[1+c], crs = f->rec_stride[1+c];
        const pixel *csrc = f->src[1+c] + (mby*ch)*css + mbx*cw;
        const pixel *cpred = f->rec[1+c] + (mby*ch)*crs + mbx*cw;
        dctcoef dc_raster[Y264_CHROMA_MAXBLK];
        for (int blk = 0; blk < nblk; blk++) {
            int bx = blk % cbw, by = blk / cbw;
            dctcoef coef[16];
            y264_sub4x4_dct(coef, csrc + (by * 4) * css + bx * 4, css,
                            cpred + (by * 4) * crs + bx * 4, crs);
            dc_raster[by*cbw + bx] = coef[0];
            if (strict) {
                if (coef_signif(coef, f->cur_chroma_qp_scaled, strict, lw, 1)) return 0;
            } else {
                int nza = bx > 0 ? 0 : cbf_nb(f, 1 + c, mbx*cbw + bx - 1, mby*cbh + by, 0);
                int nzb = by > 0 ? 0 : cbf_nb(f, 1 + c, mbx*cbw + bx, mby*cbh + by - 1, 0);
                /* No decimate tolerance on chroma: our chroma inter path
 * (inter_chroma_res) has no decimator, so "any level survives"
 * IS the coder-consistent test here. x264 does tolerate a
 * chroma score under 7, but it decimates chroma to match. */
                int cany;
                probe_signif_rdoq(f, coef, f->cur_chroma_qp_scaled,
                                  f->cur_chroma_qp, 4, 1, nza, nzb, lw, &cany, notr);
                if (cany) return 0;
                if (tol && !*tol &&
                    coef_signif(coef, f->cur_chroma_qp_scaled, 1, lw, 1))
                    *tol = 1;                   /* deadzone kept it, strict wouldn't */
            }
        }
        dctcoef dclev[Y264_CHROMA_MAXBLK], dcout[Y264_CHROMA_MAXBLK];
        if (chroma_dc_fwd(f, 0, dc_raster, dclev, dcout)) return 0;
    }
    (void)cbh;
    return 1;
}

/* Does an independent motion estimate agree that the skip MV is the right MV?
 *
 * This is the term that PAYS for decimation tolerance, and reading x264 as a
 * whole rather than as two quoted lines is what surfaces it. x264 does not
 * commit a skip on its probe: at subme >= 3 (medium is 7) the P gate
 * requires |the searched MV - the skip MV| <= 1 after a real 16x16 ME on
 * ref 0, and it explicitly does NOT take the B skip probe's word for it
 * -- it stores the deferred-skip flag and defers to the B 16x16 analysis, which searches
 * list1 ref0 and list0 ref0 and commits B_SKIP only if BOTH land within 1 of
 * the direct MV. Tolerating a decimate score under 6 is safe there precisely
 * because the search has already confirmed the MV; take the tolerance without
 * the confirmation and it is just over-skipping.
 *
 * We probe before any ME, so there is no searched MV to compare against -- but the
 * lookahead has already estimated this macroblock's motion, and its result
 * costs nothing to read. Coarser than x264's check (lowres, so the tolerance is
 * a knob rather than 1) and it cannot confirm what the full search would have
 * found, which is exactly why the recovered fraction is measured and not
 * assumed. Returns 1 when no lookahead MV exists, leaving the caller to fall
 * back to the strict acceptance rather than skip unverified. */
static int mv_agrees(int amx, int amy, int bmx, int bmy, int tol)
{
    int dx = amx - bmx, dy = amy - bmy;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx + dy <= tol;
}

/* P: the lookahead's ref0 estimate against the derived P_Skip MV. */
static int skip_mv_confirmed_p(y264_frame_t *f, int mbx, int mby,
                               int smvx, int smvy)
{
    if (!f->skip_mvagree_p) return 1;
    if (!f->lr_seed_mvx || !f->lr_seed_mvy) return 0;
    int i = mby * f->wmb + mbx;
    return mv_agrees(f->lr_seed_mvx[i], f->lr_seed_mvy[i], smvx, smvy,
                     f->skip_mvagree_p);
}

static int probe_pskip(y264_frame_t *f, int mbx, int mby, int smvx, int smvy)
{
    int dec = f->skipdec_p;
    /* The guard gates the TOLERANCE, not the probe: where the lookahead cannot
 * confirm the MV we fall back to the shipped strict test, which needs no
 * confirmation because it accepts nothing the coder would code. */
    if (dec && !skip_mv_confirmed_p(f, mbx, mby, smvx, smvy)) dec = 0;
    return probe_skip(f, mbx, mby, 0, dec);
}

/* Snapshot the LIVE half of the est context array for one pricing trial.
 * y264_cabac_ctx_n is a runtime read of f->cf_idc, so passing it as the
 * length compiles to a _platform_memmove CALL; branching on the format hands
 * clang the constant and it inlines the copy. Contexts 460..1023 exist only for
 * 4:4:4's Cb/Cr residual, so the short form is the whole live state.
 * (Same bug class as save_mb_rec's row width and the nnz grid walks.) */
static inline void est_ctx_snap(uint8_t *dst, const uint8_t *src, int cf_idc)
{
    if (cf_idc == 3) memcpy(dst, src, Y264_CABAC_CTX);
    else             memcpy(dst, src, Y264_CABAC_CTX_BASE);
}

/* The snapshot half of a P-inter est trial (ctx copy + every grid the walk
 * mutates), factored so Y264_EST_PROF can time save/author/emit/restore
 * separately on the same live MB. save swaps the engine onto the private ctx
 * copy; restore swaps back and undoes every grid write. */
struct est_p_snap {
    uint8_t ctx_s[Y264_CABAC_CTX];
    uint8_t *ctx_sv;
    int8_t  nz[16 + 32];
    int16_t mx_s[16], my_s[16];
    int     cbp_s;
    struct qp_chain qc;
};

/* Which grids does an est trial's author actually write? mvd only
 * for multi-partition candidates; nnz only when the emit will read cbf_nb
 * cells inside the MB (4x4 luma, chroma AC, any 4:4:4 residual). Mirrors the
 * est gates in author_inter_cabac / author_cabac_inter_tail exactly. */
static int est_p_saves_mvd(const struct inter_result *ir)
{
    return ir->part != 0;
}
static int est_saves_nnz(const y264_frame_t *f, const struct inter_result *ir)
{
    if (f->cf_idc == 3) return ir->cbp444 != 0;
    return ((ir->cbp_luma | ir->cr.cbp) && !ir->tr8) || ir->cr.cbp == 2;
}

static void est_p_save(y264_frame_t *f, int mbx, int mby, struct est_p_snap *s,
                       int save_mvd, int save_nnz)
{
    y264_cabac_t *c = f->cabac;
    est_ctx_snap(s->ctx_s, c->est_ctx, f->cf_idc); /* W0 step 6: trial from est_ctx */
    s->ctx_sv = c->ctx; c->ctx = s->ctx_s;         /* ... over a private copy of it */
    if (save_nnz) save_mb_nnz(f, mbx, mby, s->nz);
    int st = f->mv_stride;
    if (save_mvd)
        for (int by = 0; by < 4; by++)
            for (int bx = 0; bx < 4; bx++) {
                int i = (mby*4+by)*st + (mbx*4+bx);
                s->mx_s[by*4+bx] = f->mvdx[i]; s->my_s[by*4+bx] = f->mvdy[i];
            }
    s->cbp_s = f->mbcbp[mby * f->mbcbp_stride + mbx];
    qp_save(f, &s->qc);
}

static void est_p_restore(y264_frame_t *f, int mbx, int mby, struct est_p_snap *s,
                          int save_mvd, int save_nnz)
{
    y264_cabac_t *c = f->cabac;
    qp_load(f, &s->qc);
    c->ctx = s->ctx_sv;
    if (save_nnz) load_mb_nnz(f, mbx, mby, s->nz);
    int st = f->mv_stride;
    if (save_mvd)
        for (int by = 0; by < 4; by++)
            for (int bx = 0; bx < 4; bx++) {
                int i = (mby*4+by)*st + (mbx*4+bx);
                f->mvdx[i] = s->mx_s[by*4+bx]; f->mvdy[i] = s->my_s[by*4+bx];
            }
    f->mbcbp[mby * f->mbcbp_stride + mbx] = s->cbp_s;
}

static long est_inter_mb_bits_inner(y264_frame_t *f, int mbx, int mby,
                                    const struct inter_result *ir)
{
    y264_cabac_t *c = f->cabac;
    int smvd = est_p_saves_mvd(ir), snnz = est_saves_nnz(f, ir);
    struct est_p_snap s;
    est_p_save(f, mbx, mby, &s, smvd, snnz);

    c->est_mode = 1; c->est_bits = 0;
    write_inter_cabac(c, f, mbx, mby, ir);
    long bits = c->est_bits;
    c->est_mode = 0;

    est_p_restore(f, mbx, mby, &s, smvd, snnz);
    return bits;
}

/* Y264_EST_PROF replay bench: on one sampled live MB, time cumulative phase
 * prefixes of the est call. Cache-HOT by construction (the same MB looped);
 * the gap to the ~125 ns in-encoder census is memory effects. */
static void est_prof_run(y264_frame_t *f, int mbx, int mby,
                         const struct inter_result *ir, unsigned long long n)
{
    y264_cabac_t *c = f->cabac;
    enum { K = 2000, R = 4, NPH = 6 };
    static const char *phn[NPH] = {
        "save+restore", "+author grids", "+emit header",
        "+tail cbp/qpd", "+tail luma resid", "+tail chroma (=full)" };
    double best[NPH];
    for (int ph = 0; ph < NPH; ph++) {
        best[ph] = 1e30;
        for (int r = 0; r < R; r++) {
            uint64_t t0 = rp_now();
            for (int i = 0; i < K; i++) {
                struct est_p_snap s;
                est_p_save(f, mbx, mby, &s, est_p_saves_mvd(ir), est_saves_nnz(f, ir));
                if (ph >= 1) {
                    c->est_mode = 1; c->est_bits = 0;
                    author_inter_cabac(f, mbx, mby, ir);
                    if (ph >= 2)
                        emit_inter_cabac_ex(c, f, mbx, mby, ir,
                            ph == 2 ? 0 : ph == 3 ? 1 : ph == 4 ? 3 : 7);
                    c->est_mode = 0;
                }
                est_p_restore(f, mbx, mby, &s, est_p_saves_mvd(ir), est_saves_nnz(f, ir));
            }
            double ns = (double)(rp_now() - t0) / K;
            if (ns < best[ph]) best[ph] = ns;
        }
    }
    /* the 460-byte ctx copy alone, and the untouched full call as a check */
    static uint8_t ctx_dst[Y264_CABAC_CTX];
    volatile uint8_t sink8 = 0;
    double bctx = 1e30, breal = 1e30;
    for (int r = 0; r < R; r++) {
        uint64_t t0 = rp_now();
        for (int i = 0; i < K; i++) {
            est_ctx_snap(ctx_dst, c->est_ctx, f->cf_idc);
            sink8 ^= ctx_dst[i & 255];
        }
        double ns = (double)(rp_now() - t0) / K;
        if (ns < bctx) bctx = ns;
    }
    volatile long sinkb = 0;
    for (int r = 0; r < R; r++) {
        uint64_t t0 = rp_now();
        for (int i = 0; i < K; i++) sinkb += est_inter_mb_bits_inner(f, mbx, mby, ir);
        double ns = (double)(rp_now() - t0) / K;
        if (ns < breal) breal = ns;
    }
    (void)sink8;
    /* split the luma-residual phase: the 64-element zigzag gather alone, and
 * y264_cabac_residual_8x8 alone on pre-gathered arrays (tr8 MBs only) */
    double bgat = -1, bres8 = -1;
    if (ir->tr8 && f->cf_idc != 3 && (ir->cbp_luma & 0xf)) {
        volatile dctcoef sinkc = 0;
        bgat = 1e30;
        for (int r = 0; r < R; r++) {
            uint64_t t0 = rp_now();
            for (int i = 0; i < K; i++)
                for (int blk = 0; blk < 4; blk++)
                    if (ir->cbp_luma & (1 << blk)) {
                        dctcoef scan8[64];
                        for (int k2 = 0; k2 < 64; k2++)
                            scan8[k2] = ir->lev8[blk][ZIGZAG8[k2]];
                        sinkc ^= scan8[i & 63];
                    }
            double ns = (double)(rp_now() - t0) / K;
            if (ns < bgat) bgat = ns;
        }
        dctcoef pre[4][64]; int nblk = 0;
        for (int blk = 0; blk < 4; blk++)
            if (ir->cbp_luma & (1 << blk)) {
                for (int k2 = 0; k2 < 64; k2++)
                    pre[nblk][k2] = ir->lev8[blk][ZIGZAG8[k2]];
                nblk++;
            }
        struct est_p_snap s;
        est_p_save(f, mbx, mby, &s, 1, 1);
        c->est_mode = 1; c->est_bits = 0;
        bres8 = 1e30;
        for (int r = 0; r < R; r++) {
            uint64_t t0 = rp_now();
            for (int i = 0; i < K; i++)
                for (int j = 0; j < nblk; j++)
                    y264_cabac_residual_8x8(c, pre[j]);
            double ns = (double)(rp_now() - t0) / K;
            if (ns < bres8) bres8 = ns;
        }
        c->est_mode = 0;
        est_p_restore(f, mbx, mby, &s, 1, 1);
    }
    long bits = est_inter_mb_bits_inner(f, mbx, mby, ir);
    char buf[1280]; int o = 0;
    o += snprintf(buf + o, sizeof buf - o,
        "EST_PROF n=%llu mb=(%d,%d) part=%d tr8=%d cbpY=0x%x cbpC=%d bits=%ld\n",
        n, mbx, mby, ir->part, ir->tr8, ir->cbp_luma, ir->cr.cbp, bits);
    for (int ph = 0; ph < NPH; ph++)
        o += snprintf(buf + o, sizeof buf - o, "  %-22s %7.1f ns   (+%5.1f)\n",
                      phn[ph], best[ph], best[ph] - (ph ? best[ph - 1] : 0.0));
    o += snprintf(buf + o, sizeof buf - o,
        "  %-22s %7.1f ns\n  %-22s %7.1f ns\n",
        "full call (as-is)", breal, "ctx-snap alone", bctx);
    if (bgat >= 0)
        o += snprintf(buf + o, sizeof buf - o,
            "  %-22s %7.1f ns\n  %-22s %7.1f ns\n",
            "zigzag gather alone", bgat, "resid8 pre-gathered", bres8);
    fputs(buf, stderr);
}

/* CABAC rate estimate (x256 fractional bits) of an inter MB, for mode decision.
 * Runs the real coder in est_mode over a snapshot of every neighbour grid it
 * mutates (contexts, nnz, mvd, mbcbp), then restores them -- so it is
 * side-effect-free and can be called during RD without perturbing the stream. */
static long est_inter_mb_bits(y264_frame_t *f, int mbx, int mby,
                              const struct inter_result *ir)
{
    NLED(est_mb, 1);
    if (est_prof_on()) {
        static _Atomic unsigned long long n_calls;
        unsigned long long n = atomic_fetch_add_explicit(&n_calls, 1,
                                                         memory_order_relaxed);
        /* eight samples spread across the encode; exact match so exactly one
 * thread claims each even under the wavefront */
        if (n >= 2500 && n <= 37500 && n % 5000 == 2500)
            est_prof_run(f, mbx, mby, ir, n);
    }
    return est_inter_mb_bits_inner(f, mbx, mby, ir);
}

/* CABAC rate estimate (x256) of an intra MB, mirroring est_inter_mb_bits.
 * write_intra_cabac mutates ctx + nnz + mbcbp + the qp chain (no mvd); snapshot
 * and restore them (matching what the CAVLC intra RD already restores + ctx). */
static long est_intra_mb_bits(y264_frame_t *f, int mbx, int mby,
                              const struct intra_mb *o, int slice)
{
    NLED(est_mb, 1);
    y264_cabac_t *c = f->cabac;
    uint8_t ctx_s[Y264_CABAC_CTX];
    est_ctx_snap(ctx_s, c->est_ctx, f->cf_idc); /* W0 step 6: trial from est_ctx */
    uint8_t *ctx_sv = c->ctx; c->ctx = ctx_s;   /* ... over a private copy of it */
    int8_t nz[16 + 32]; save_mb_nnz(f, mbx, mby, nz);
    int cbp_i = mby * f->mbcbp_stride + mbx, cbp_s = f->mbcbp[cbp_i];
    int tr8_i = mby * f->wmb + mbx;
    pixel tr8_s = f->mb_tr8 ? f->mb_tr8[tr8_i] : 0;
    int ims = f->i4mode_stride; int8_t i4_s[16];
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            i4_s[by*4+bx] = f->i4mode[(mby*4+by)*ims + (mbx*4+bx)];
    struct qp_chain qc; qp_save(f, &qc);

    c->est_mode = 1; c->est_bits = 0;
    write_intra_cabac(c, f, mbx, mby, o, slice);
    long bits = c->est_bits;
    c->est_mode = 0;

    qp_load(f, &qc);
    c->ctx = ctx_sv;
    load_mb_nnz(f, mbx, mby, nz);
    f->mbcbp[cbp_i] = cbp_s;
    if (f->mb_tr8) f->mb_tr8[tr8_i] = tr8_s;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            f->i4mode[(mby*4+by)*ims + (mbx*4+bx)] = i4_s[by*4+bx];
    return bits;
}

/* CABAC rate estimate (x256) of a B macroblock (est_b_bits below): direct=1 ->
 * write_b_direct_cabac, else write_b_inter_cabac (L0/L1/Bi, 16x16 or
 * partitions). The snapshot half is the B superset of neighbour grids (both
 * mvd lists, plus ctx/nnz/mbcbp/qp/mb_tr8), factored like est_p_snap for
 * Y264_EST_PROF. */
struct est_b_snap {
    uint8_t ctx_s[Y264_CABAC_CTX];
    uint8_t *ctx_sv;
    int8_t  nz[16 + 32];
    int     cbp_s;
    pixel   tr8_s;
    int16_t x0[16], y0[16], x1[16], y1[16];
    struct qp_chain qc;
};

/* save_mvd: B_Direct and 16x16 trials never touch the mvd grids (their
 * authors skip author_mvd), so only partitioned inter trials round-trip the
 * four arrays; save_nnz mirrors author_cabac_inter_tail's est gates. */
static void est_b_save(y264_frame_t *f, int mbx, int mby, struct est_b_snap *s,
                       int save_mvd, int save_nnz)
{
    y264_cabac_t *c = f->cabac;
    est_ctx_snap(s->ctx_s, c->est_ctx, f->cf_idc); /* W0 step 6: trial from est_ctx */
    s->ctx_sv = c->ctx; c->ctx = s->ctx_s;         /* ... over a private copy of it */
    if (save_nnz) save_mb_nnz(f, mbx, mby, s->nz);
    s->cbp_s = f->mbcbp[mby * f->mbcbp_stride + mbx];
    s->tr8_s = f->mb_tr8 ? f->mb_tr8[mby * f->wmb + mbx] : 0;
    int st = f->mv_stride;
    /* Copy SHAPE, not element-by-element: a row of an MB's mvd grid is four
 * CONTIGUOUS int16 (8 bytes), so this is four 8-byte moves per array, not
 * sixteen strided loads with index arithmetic (the runtime-length-memcpy
 * bug class, one level up -- the length here is a compile-time constant). */
    if (save_mvd)
        for (int by = 0; by < 4; by++) {
            int i = (mby*4+by)*st + mbx*4, k = by*4;
            memcpy(&s->x0[k], &f->mvdx[i],  4 * sizeof(int16_t));
            memcpy(&s->y0[k], &f->mvdy[i],  4 * sizeof(int16_t));
            memcpy(&s->x1[k], &f->mvdx1[i], 4 * sizeof(int16_t));
            memcpy(&s->y1[k], &f->mvdy1[i], 4 * sizeof(int16_t));
        }
    qp_save(f, &s->qc);
}

static void est_b_restore(y264_frame_t *f, int mbx, int mby, struct est_b_snap *s,
                          int save_mvd, int save_nnz)
{
    y264_cabac_t *c = f->cabac;
    qp_load(f, &s->qc);
    c->ctx = s->ctx_sv;
    if (save_nnz) load_mb_nnz(f, mbx, mby, s->nz);
    f->mbcbp[mby * f->mbcbp_stride + mbx] = s->cbp_s;
    if (f->mb_tr8) f->mb_tr8[mby * f->wmb + mbx] = s->tr8_s;
    int st = f->mv_stride;
    if (save_mvd)
        for (int by = 0; by < 4; by++) {
            int i = (mby*4+by)*st + mbx*4, k = by*4;
            memcpy(&f->mvdx[i],  &s->x0[k], 4 * sizeof(int16_t));
            memcpy(&f->mvdy[i],  &s->y0[k], 4 * sizeof(int16_t));
            memcpy(&f->mvdx1[i], &s->x1[k], 4 * sizeof(int16_t));
            memcpy(&f->mvdy1[i], &s->y1[k], 4 * sizeof(int16_t));
        }
}

static long est_b_bits_inner(y264_frame_t *f, int mbx, int mby,
                             const struct inter_result *ir, int direct)
{
    y264_cabac_t *c = f->cabac;
    int smvd = !direct && ir->bpart != 0, snnz = est_saves_nnz(f, ir);
    struct est_b_snap s;
    est_b_save(f, mbx, mby, &s, smvd, snnz);

    c->est_mode = 1; c->est_bits = 0;
    cabac_mb_skip(c, f, mbx, mby, 0, 24);   /* non-skip: pay mb_skip_flag=0 (dear on static) */
    if (direct) write_b_direct_cabac(c, f, mbx, mby, ir);
    else        write_b_inter_cabac(c, f, mbx, mby, ir);
    long bits = c->est_bits;
    c->est_mode = 0;

    est_b_restore(f, mbx, mby, &s, smvd, snnz);
    return bits;
}

/* Y264_EST_PROF replay bench, B flavour of est_prof_run. */
static void est_prof_run_b(y264_frame_t *f, int mbx, int mby,
                           const struct inter_result *ir, int direct,
                           unsigned long long n)
{
    y264_cabac_t *c = f->cabac;
    enum { K = 2000, R = 4, NPH = 6 };
    static const char *phn[NPH] = {
        "save+restore", "+author grids", "+skip+emit header",
        "+tail cbp/qpd", "+tail luma resid", "+tail chroma (=full)" };
    double best[NPH];
    for (int ph = 0; ph < NPH; ph++) {
        best[ph] = 1e30;
        for (int r = 0; r < R; r++) {
            uint64_t t0 = rp_now();
            for (int i = 0; i < K; i++) {
                struct est_b_snap s;
                est_b_save(f, mbx, mby, &s, !direct && ir->bpart != 0,
                           est_saves_nnz(f, ir));
                if (ph >= 1) {
                    c->est_mode = 1; c->est_bits = 0;
                    if (direct) author_b_direct_cabac(f, mbx, mby, ir);
                    else        author_b_inter_cabac(f, mbx, mby, ir);
                    if (ph >= 2) {
                        int parts = ph == 2 ? 0 : ph == 3 ? 1 : ph == 4 ? 3 : 7;
                        cabac_mb_skip(c, f, mbx, mby, 0, 24);
                        if (direct) emit_b_direct_cabac_ex(c, f, mbx, mby, ir, parts);
                        else        emit_b_inter_cabac_ex(c, f, mbx, mby, ir, parts);
                    }
                    c->est_mode = 0;
                }
                est_b_restore(f, mbx, mby, &s, !direct && ir->bpart != 0,
                              est_saves_nnz(f, ir));
            }
            double ns = (double)(rp_now() - t0) / K;
            if (ns < best[ph]) best[ph] = ns;
        }
    }
    volatile long sinkb = 0;
    double breal = 1e30;
    for (int r = 0; r < R; r++) {
        uint64_t t0 = rp_now();
        for (int i = 0; i < K; i++)
            sinkb += est_b_bits_inner(f, mbx, mby, ir, direct);
        double ns = (double)(rp_now() - t0) / K;
        if (ns < breal) breal = ns;
    }
    long bits = est_b_bits_inner(f, mbx, mby, ir, direct);
    char buf[1024]; int o = 0;
    o += snprintf(buf + o, sizeof buf - o,
        "EST_PROF B n=%llu mb=(%d,%d) %s bmode=%d bpart=%d tr8=%d cbpY=0x%x cbpC=%d bits=%ld\n",
        n, mbx, mby, direct ? "direct" : "inter", ir->bmode, ir->bpart,
        ir->tr8, ir->cbp_luma, ir->cr.cbp, bits);
    for (int ph = 0; ph < NPH; ph++)
        o += snprintf(buf + o, sizeof buf - o, "  %-22s %7.1f ns   (+%5.1f)\n",
                      phn[ph], best[ph], best[ph] - (ph ? best[ph - 1] : 0.0));
    o += snprintf(buf + o, sizeof buf - o, "  %-22s %7.1f ns\n",
                  "full call (as-is)", breal);
    fputs(buf, stderr);
}

static long est_b_bits(y264_frame_t *f, int mbx, int mby,
                       const struct inter_result *ir, int direct)
{
    if (est_prof_on()) {
        static _Atomic unsigned long long n_calls;
        unsigned long long n = atomic_fetch_add_explicit(&n_calls, 1,
                                                         memory_order_relaxed);
        if (n >= 10000 && n <= 110000 && n % 20000 == 10000)
            est_prof_run_b(f, mbx, mby, ir, direct, n);
    }
    return est_b_bits_inner(f, mbx, mby, ir, direct);
}

/* CABAC rate estimate (x256) of the B mb_skip_flag=1 bin -- the true cost of
 * coding this MB as B_Skip (cheap when neighbours are skip). */
static long est_b_skip_bits(y264_frame_t *f, int mbx, int mby)
{
    y264_cabac_t *c = f->cabac;
    uint8_t ctx_s[Y264_CABAC_CTX];
    est_ctx_snap(ctx_s, c->est_ctx, f->cf_idc); /* W0 step 6: trial from est_ctx */
    uint8_t *ctx_sv = c->ctx; c->ctx = ctx_s;   /* ... over a private copy of it */
    c->est_mode = 1; c->est_bits = 0;
    cabac_mb_skip(c, f, mbx, mby, 1, 24);
    long bits = c->est_bits;
    c->est_mode = 0;
    c->ctx = ctx_sv;
    return bits;
}

/* Snapshot of the per-MB state an est-coding run perturbs (the B superset): the
 * engine ctx, both mvd lists, nnz, mbcbp, mb_tr8, i4mode, and the QP chain. Used
 * by est_commit_* to est-code a winner into est_ctx while leaving everything real
 * untouched. */
struct est_snap {
    int8_t  nnz[16 + 32];
    int     cbp;
    pixel   tr8;
    int16_t x0[16], y0[16], x1[16], y1[16];
    int8_t  i4[16];
    struct qp_chain qc;
};

static void est_snap_save(y264_frame_t *f, int mbx, int mby, struct est_snap *s)
{
    save_mb_nnz(f, mbx, mby, s->nnz);
    s->cbp = f->mbcbp[mby * f->mbcbp_stride + mbx];
    s->tr8 = f->mb_tr8 ? f->mb_tr8[mby * f->wmb + mbx] : 0;
    int st = f->mv_stride, ims = f->i4mode_stride;
    for (int by = 0; by < 4; by++) {
        int i = (mby*4+by)*st + mbx*4, k = by*4;
        memcpy(&s->x0[k], &f->mvdx[i],  4 * sizeof(int16_t));
        memcpy(&s->y0[k], &f->mvdy[i],  4 * sizeof(int16_t));
        memcpy(&s->x1[k], &f->mvdx1[i], 4 * sizeof(int16_t));
        memcpy(&s->y1[k], &f->mvdy1[i], 4 * sizeof(int16_t));
        memcpy(&s->i4[k], &f->i4mode[(mby*4+by)*ims + mbx*4], 4 * sizeof(int8_t));
    }
    qp_save(f, &s->qc);
}

static void est_snap_restore(y264_frame_t *f, int mbx, int mby, const struct est_snap *s)
{
    load_mb_nnz(f, mbx, mby, s->nnz);
    f->mbcbp[mby * f->mbcbp_stride + mbx] = s->cbp;
    if (f->mb_tr8) f->mb_tr8[mby * f->wmb + mbx] = s->tr8;
    int st = f->mv_stride, ims = f->i4mode_stride;
    for (int by = 0; by < 4; by++) {
        int i = (mby*4+by)*st + mbx*4, k = by*4;
        memcpy(&f->mvdx[i],  &s->x0[k], 4 * sizeof(int16_t));
        memcpy(&f->mvdy[i],  &s->y0[k], 4 * sizeof(int16_t));
        memcpy(&f->mvdx1[i], &s->x1[k], 4 * sizeof(int16_t));
        memcpy(&f->mvdy1[i], &s->y1[k], 4 * sizeof(int16_t));
        memcpy(&f->i4mode[(mby*4+by)*ims + mbx*4], &s->i4[k], 4 * sizeof(int8_t));
    }
    qp_load(f, &s->qc);
}

/* Advance est_ctx by est-coding a B winner's exact bins (mb_skip_flag + residual);
 * returns the estimated bits (x256) for the est-vs-real self-check. */
static long est_commit_b(y264_frame_t *f, int mbx, int mby, const struct b_rec *r)
{
    y264_cabac_t *c = f->cabac;
    struct est_snap s; est_snap_save(f, mbx, mby, &s);
    uint8_t *ctx_sv = c->ctx; c->ctx = c->est_ctx;  /* adapt est_ctx IN PLACE */
    c->est_mode = 1; c->est_bits = 0;
    cabac_mb_skip(c, f, mbx, mby, r->mode == 0, 24);
    if (r->mode == 1)      write_b_direct_cabac(c, f, mbx, mby, &r->u.ir);
    else if (r->mode == 2) write_b_inter_cabac(c, f, mbx, mby, &r->u.ir);
    else if (r->mode == 3) write_intra_cabac(c, f, mbx, mby, &r->u.intra, 2);
    c->est_mode = 0;
    long bits = c->est_bits;
    c->ctx = ctx_sv;
    est_snap_restore(f, mbx, mby, &s);
    return bits;
}

/* Decision record for one analysed P macroblock. The union holds only the
 * winner's payload (skip needs neither), tagged by mode. */
struct p_mb {
    uint8_t mode;               /* 0 skip, 1 inter, 2 intra */
    uint8_t eff_skip;           /* coded as P_Skip (mode 0, or a no-residual inter16 == skip MV) */
    int smvx, smvy;             /* P_Skip MV (for eff_skip authoring) */
    union { struct inter_result ires; struct intra_mb intra; } u;
};

static int mb_codes_qpd(const y264_frame_t *f, const struct p_mb *r)
{
    if (r->eff_skip) return 0;                      /* P_Skip: no residual, no delta */
    if (r->mode == 1) {                             /* inter */
        const struct inter_result *ir = &r->u.ires;
        return (f->cf_idc == 3 ? ir->cbp444 : (ir->cbp_luma | ir->cr.cbp)) > 0;
    }
    return intra_codes_qpd(f, &r->u.intra);         /* intra */
}

/* Advance est_ctx by est-coding a P winner's exact bins (mb_skip_flag + residual);
 * returns the estimated bits (x256) for the est-vs-real self-check. */
static long est_commit_p(y264_frame_t *f, int mbx, int mby, const struct p_mb *r)
{
    y264_cabac_t *c = f->cabac;
    struct est_snap s; est_snap_save(f, mbx, mby, &s);
    uint8_t *ctx_sv = c->ctx; c->ctx = c->est_ctx;  /* adapt est_ctx IN PLACE */
    c->est_mode = 1; c->est_bits = 0;
    cabac_mb_skip(c, f, mbx, mby, r->eff_skip, 11);
    if (!r->eff_skip) {
        if (r->mode == 1) write_inter_cabac(c, f, mbx, mby, &r->u.ires);
        else              write_intra_cabac(c, f, mbx, mby, &r->u.intra, 1);
    }
    c->est_mode = 0;
    long bits = c->est_bits;
    c->ctx = ctx_sv;
    est_snap_restore(f, mbx, mby, &s);
    return bits;
}

/* Advance est_ctx by est-coding an I-slice intra MB's bins (mb_qp_delta + residual);
 * the two-pass I loop uses this so analyze_intra's RDOQ (which reads est_ctx via the
 * CABAC rate model) sees the same context single-pass tracked from the live engine. */
static long est_commit_i(y264_frame_t *f, int mbx, int mby, const struct intra_mb *o)
{
    y264_cabac_t *c = f->cabac;
    struct est_snap s; est_snap_save(f, mbx, mby, &s);
    uint8_t *ctx_sv = c->ctx; c->ctx = c->est_ctx;  /* adapt est_ctx IN PLACE */
    c->est_mode = 1; c->est_bits = 0;
    write_intra_cabac(c, f, mbx, mby, o, 0);        /* slice 0 = I slice */
    c->est_mode = 0;
    long bits = c->est_bits;
    c->ctx = ctx_sv;
    est_snap_restore(f, mbx, mby, &s);
    return bits;
}

/* Analyse one P macroblock: RD mode decision (skip / inter / intra), leaving the
 * winner's reconstruction in f->rec and mb_tr8 set. No grid authoring beyond the
 * intra trial's self-restored scratch, and no bitstream. Fills *out. Shared by
 * the CABAC single pass and the CAVLC two-pass. */
static void analyze_p_mb(y264_frame_t *f, int mbx, int mby, int mlam, long lam,
                         pixel *snap_skip, pixel *snap_inter, int8_t *nzbuf,
                         struct p_mb *out)
{
    STG_BEG(STG_DECIDE);
    int pp_on = bprof_env();
    uint64_t pp_loc[PP_NSTAGE] = {0};
    uint64_t pp_last = pp_on ? (bp_register(), bp_now()) : 0;
    int pp_stage = 0;
#define PPCUT(n) do { if (pp_on) { uint64_t _nw = bp_now();                    \
                     pp_loc[pp_stage] += _nw - pp_last; pp_last = _nw; }        \
                     pp_stage = (n); } while (0)
    NLED_SITE(Y264_LED_SITE_PME); NLED(mb_p, 1);
    int rs = f->rec_stride[0], refs = f->ref_stride[0];
    mb_qp_pre(f, mbx, mby);
    if (mb_lambda_on()) { int lq = mb_lambda_qp(f, mbx, mby);
                          if (lq >= 0) { mlam = lambda_me(lq); lam = lambda_mode16(lq); } }
    y264_me_set_cheap(f->me_cheap);     /* per-frame adaptive-ME flag (TLS) */
    y264_me_reset_hpel_thresh();        /* x264's half-pel threshold: fresh per MB */
    y264_me_set_isb(0);                 /* oracle attribution: P frame */
    y264_me_set_stq(f->stq);
    y264_me_set_et_class(1);
    me_et_imp_stamp(f, mbx, mby);       /* importance rescue for the ET exits */
    int smvx, smvy;
    mv_skip(f, mbx, mby, &smvx, &smvy);
    pixel *rec = f->rec[0] + (mby * 16) * rs + mbx * 16;
    int s4 = trellis_commit_on(f->subme, f->trellis);
    s_rd_trial = s4;                /* S4: deadzone quant in the RD trials below */

    /* v3 staircase (Y264_STAIR_DEPTH): P_Skip predicts from ref 0. When ref 0
 * is the (possibly in-flight) previous anchor, its reads must respect the
 * fixed clamp -- but the derived skip MV is a median that can include
 * neighbours coded against OTHER (unclamped) references, so it may exceed
 * the bound. Such a skip candidate is simply never evaluated: no MC read,
 * j_skip loses by construction. Deterministic (a pure function of coded
 * neighbour MVs), applied by the env gate alone. */
    int skip_ok = !(stair_l0_clamp(f, 0) && smvy > f->stair_mvy_max);
    long j_skip = LONG_MAX / 2;

    /* --- skip candidate: pure motion-compensated prediction --- */
    if (skip_ok) {
    STG_BEG(STG_SKIP);
    y264_mc_luma(rec, rs, f->ref[0], refs, f->padded_w, f->padded_h,
                 mbx * 16, mby * 16, smvx, smvy, 16, 16);
    apply_wp_luma(f, rec, rs, 16, 16, 0);   /* P_Skip predicts from ref 0 */
    if (f->cf_idc == 3) {                   /* 4:4:4: chroma skip = luma 6-tap MC */
        for (int c = 0; c < 2; c++)
            y264_mc_luma_b(f->rec[1 + c] + (mby*16)*f->rec_stride[1+c] + mbx*16,
                         f->rec_stride[1 + c], f->ref[1 + c], f->ref_stride[1 + c],
                         f->padded_w, f->padded_h, mbx*16, mby*16, smvx, smvy, 16, 16, Y264_CHROMA_BORDER);
    } else {
    int cw = 16 / f->sub_w, chh = 16 / f->sub_h;
    for (int c = 0; c < 2; c++)
        y264_mc_chroma(f->rec[1 + c] + (mby*chh)*f->rec_stride[1+c] + mbx*cw,
                       f->rec_stride[1 + c], f->ref[1 + c], f->ref_stride[1 + c],
                       f->padded_w / f->sub_w, f->padded_h / f->sub_h,
                       mbx * cw, mby * chh, smvx, smvy, cw, chh, f->sub_w, f->sub_h);
    }
    j_skip = dist_mb(f, mbx, mby) + Y264_LAMJ(lam, 1);
    STG_END();  /* STG_SKIP */
    save_mb_rec(f, mbx, mby, snap_skip);
    }

    /* Early P_Skip (subme<=8): if the skip residual quantizes to nothing
 * it wins RD outright -- commit it without any further analysis. */
    struct inter_result ires, cand, ires0;
    struct intra_mb intra;
    long j_inter;
    long best_satd = -1;                         /* inter SATD (subme<=8), for intra early-out */
    long pc_satd16 = -1;                         /* Y264_PSKIP_CENSUS: the ref-0 16x16 SATD winner */
    int mode, early;
    PPCUT(1);
    if (skip_ok && f->subme <= 8) { STG_BEG(STG_PROBE); early = probe_pskip(f, mbx, mby, smvx, smvy); STG_END(); }
    else early = 0;
    /* Measurement bound only (see skiporacle.h): replay the recorded final
 * verdict. Same verdict, same P_Skip recon -> byte-identical, so the wall
 * delta is exactly what a perfect early-skip predictor would buy. */
    if (!early && skip_ok && y264_skor_mode() == 2 &&
        y264_skor_ask(f->skor_key, 0, mbx, mby, f->wmb))
        early = 1;
    long j_win = 0;
    if (early) {
        NLED(mb_p_early, 1);
        mode = 0;
        j_win = j_skip;
        goto decided;
    }

    /* Refuted RD middle path (see midskip_on): env-gated OFF. */
    if (f->subme <= 8 && midskip_on()) {
        int mvx16[16] = {0}, mvy16[16] = {0}, pmvx16[16] = {0}, pmvy16[16] = {0};
        int pref0[4] = {0,0,0,0}, psub0[4] = {0,0,0,0};
        mvx16[0] = smvx; mvy16[0] = smvy;
        mv_predict(f, mbx, mby, 0, &pmvx16[0], &pmvy16[0]);
        long j16 = inter_rd_score(f, mbx, mby, 0, mvx16, mvy16, pmvx16, pmvy16,
                                  pref0, psub0, &cand, lam, 0);
        if (j_skip <= j16 + Y264_LAMJ(lam, midskip_margin())) {
            mode = 0;
            j_win = j_skip;
            goto decided;
        }
    }

    /* --- inter candidates. subme>=9: full RD on every partition (max
 * quality). subme<=8 (x264 subme-7 model): SATD picks the partition
 * shape, full RD scores it + 16x16 (insurance against SATD
 * over-splitting at low bitrate) -- 1-2 RDs instead of four. --- */
    if (f->subme >= 9) {
        PPCUT(3);
        j_inter = eval_inter_part(f, mbx, mby, 0, mlam, lam, &ires, 1, NULL);
        save_mb_rec(f, mbx, mby, snap_inter);
        for (int part = 1; part <= 3; part++) {
            long j = eval_inter_part(f, mbx, mby, part, mlam, lam, &cand, 1, NULL);
            if (j < j_inter) { j_inter = j; ires = cand; save_mb_rec(f, mbx, mby, snap_inter); }
        }
    } else {
        PPCUT(2);
        tl_rx_hit = 0;
        tl_rx_armed = skip_ok ? p_ref0exit_on() : 0;   /* 1 = MV test, 2 = MV + SATD bound */
        tl_rx_smvx = smvx; tl_rx_smvy = smvy;
        best_satd = eval_inter_part(f, mbx, mby, 0, mlam, lam, &ires0, 0, NULL);
        tl_rx_armed = 0;
        pc_satd16 = best_satd;
        tl_ref16 = ires0.ref[0];                 /* the 8x8 stage's clamp key (P8_REFCLAMP) */
        if (skip_ok && pskip_exit_mode() == 2) {
            const pixel *src0 = f->src[0] + (mby * 16) * (size_t)f->src_stride[0] + mbx * 16;
            long satd_skip = y264_dsp.satd16x16(src0, f->src_stride[0], snap_skip, 16);
            if (satd_skip < best_satd) { tl_ref16 = -1; mode = 0; j_win = j_skip; goto decided; }
        }
        int s16[2] = { ires0.mvx[0], ires0.mvy[0] };
        long satd_16 = best_satd;                /* A1b: 16x16 SATD for the insurance-RD admission gate */
        ires = ires0;
        if (tl_rx_hit) {                     /* Y264_P_REF0EXIT: the 16x16 is the candidate */
        } else if (!f->stq && part_earlyterm()) {   /* stq: pre-flip all-four order */
            /* x264 order + gate: 16x16 (done), 8x8, then 16x8/8x16 only if the
 * 8x8 split looks promising. Compare raw SATD (strip PART_MBTYPE_BITS,
 * as x264's 8x8/16x16 costs exclude the mb-type ue(v)). */
            long cost16_raw = best_satd - (long)mlam * PART_MBTYPE_BITS[0];
            long s8 = eval_inter_part(f, mbx, mby, 3, mlam, lam, &cand, 0, s16);
            /* x264's rectangle threshold: the mv cost of the 8x8 blocks a rect merges
 * (the two 8x8 halves' MV cost), from THIS MB's own search result. */
            long mv_slack = (long)mlam *
                (mvd_bits(cand.mvx[1] - cand.pmvx[1]) + mvd_bits(cand.mvy[1] - cand.pmvy[1]) +
                 mvd_bits(cand.mvx[2] - cand.pmvx[2]) + mvd_bits(cand.mvy[2] - cand.pmvy[2]));
            if (s8 < best_satd) { best_satd = s8; ires = cand; }
            long cost8_raw = s8 - (long)mlam * PART_MBTYPE_BITS[3];
            if (part_search_rect(f, mbx, mby, cost8_raw, cost16_raw, mlam, mv_slack)) {
                int refs8[4] = { cand.ref[0], cand.ref[1], cand.ref[2], cand.ref[3] };
                tl_rect_refs = rect_refs_on() ? refs8 : NULL;
                for (int part = 1; part <= 2; part++) {
                    long s = eval_inter_part(f, mbx, mby, part, mlam, lam, &cand, 0, s16);
                    if (s < best_satd) { best_satd = s; ires = cand; }
                }
                tl_rect_refs = NULL;
            }
        } else {
            for (int part = p_rect_on() ? 1 : 3; part <= 3; part++) {   /* original: all four shapes */
                long s = eval_inter_part(f, mbx, mby, part, mlam, lam, &cand, 0, s16);
                if (s < best_satd) { best_satd = s; ires = cand; }
            }
        }
        tl_ref16 = -1;
        PPCUT(3);
        j_inter = inter_rd_score(f, mbx, mby, ires.part, ires.mvx, ires.mvy,
                                 ires.pmvx, ires.pmvy, ires.ref, ires.sub, &ires, lam, 0);
        j_inter = qpel_rd_nudge(f, mbx, mby, ires.part, ires.mvx, ires.mvy,
                                ires.pmvx, ires.pmvy, ires.ref, ires.sub, &ires, lam, j_inter);
        save_mb_rec(f, mbx, mby, snap_inter);
        if (ires.part != 0 && rd_admit_16(satd_16, best_satd)) {   /* 16x16 insurance RD, gated */
            long j0 = inter_rd_score(f, mbx, mby, 0, ires0.mvx, ires0.mvy,
                                     ires0.pmvx, ires0.pmvy, ires0.ref, ires0.sub, &ires0, lam, 0);
            j0 = qpel_rd_nudge(f, mbx, mby, 0, ires0.mvx, ires0.mvy,
                               ires0.pmvx, ires0.pmvy, ires0.ref, ires0.sub, &ires0, lam, j0);
            if (j0 < j_inter) { j_inter = j0; ires = ires0; save_mb_rec(f, mbx, mby, snap_inter); }
        }
    }

    /* --- intra candidate. W-B (S1): SATD-screen intra first -- only run the full
 * encode + RD when i16 is competitive with the inter winner (x264 skips it on
 * most inter MBs). Pass the inter SATD so the survivor path can also skip its
 * i4x4/i8x8 sub-search when inter beats i16 (x264). --- */
    y264_bs_t sb;
    long j_intra = -1;
    PPCUT(4);
    if (skip_ok && j_skip <= j_inter) {
        int xm = pskip_exit_mode();
        if (xm == 1 || xm == 2) {           /* post-RD bypass (band-refuted, kept for study) */
            mode = 0; j_win = j_skip;
            goto decided;
        }
        if (xm == 3 && best_satd >= 0) {    /* screen-bar fix: intra must beat the WINNER */
            const pixel *src0 = f->src[0] + (mby * 16) * (size_t)f->src_stride[0] + mbx * 16;
            long satd_skip = y264_dsp.satd16x16(src0, f->src_stride[0], snap_skip, 16);
            if (satd_skip < best_satd) best_satd = satd_skip;
        }
    }
    if (intra_admit(f, mbx, mby, best_satd)) {
        STG_BEG(STG_INTRA);
        NLED_SITE(Y264_LED_SITE_PINTRA);
        analyze_intra_g(f, mbx, mby, &intra, best_satd);
        NLED_SITE(Y264_LED_SITE_PME);
        if (cabac_rd_on() >= 2 && f->cabac) {   /* intra CABAC-RD: net-negative on
 * motion (CAVLC over-priced intra,
 * which helpfully discouraged it in
 * P); gated off, Y264_CABAC_RD=2 to try */
            j_intra = dist_mb(f, mbx, mby)
                    + Y264_LAMJ(lam, est_intra_mb_bits(f, mbx, mby, &intra, 1) >> 8);
        } else {
            save_mb_nnz(f, mbx, mby, nzbuf);
            struct qp_chain qc; qp_save(f, &qc);
            y264_bs_init_count(&sb);        /* pricing only */
            write_intra_syntax(&sb, f, mbx, mby, 5, &intra);
            j_intra = dist_mb(f, mbx, mby) + Y264_LAMJ(lam, y264_bs_pos_bits(&sb));
            load_mb_nnz(f, mbx, mby, nzbuf);
            qp_load(f, &qc);
        }
        /* rec currently holds the intra reconstruction. */
        STG_END();  /* STG_INTRA */
    }

    /* Q3 probe: deep-quant intra admission
 * bonus. x264 codes 4.0% intra on foreman QP48 to our 1.7%, the screen is
 * measured non-binding there and full RD rejects the rest -- consistent
 * with refresh whose value is cumulative across the reference chain (the
 * hf_join skip/skip inheritance), which one frame's RD cannot see.
 * Y264_INTRA_RDBONUS=<x256>[,<qp0>] scales j_intra at cur_qp >= qp0
 * (default 40) before the three-way compare; prices whether BOUGHT intra
 * refresh raises deep-quant NEG at all. Default inert. */
    if (intra_rdbonus(0) > 0 && j_intra >= 0 && f->cur_qp >= intra_rdbonus(1))
        j_intra = j_intra * intra_rdbonus(0) >> 8;
    int pc_intra_ran = j_intra >= 0;             /* Y264_PSKIP_CENSUS */
    if (j_intra >= 0) { mode = 2; }              /* 0 skip, 1 inter, 2 intra */
    else { mode = 1; j_intra = j_inter; }        /* intra screened out: base on inter */
    long best = j_intra;
    if (j_skip <= best) { best = j_skip; mode = 0; }
    if (j_inter < best) { best = j_inter; mode = 1; }
    j_win = best;
    /* Ceiling probe only (pprune_note): reached exactly by the macroblocks that
 * ran the full tournament, which is the late-skip class when mode == 0. */
    if (skip_ok)
        pprune_note(j_skip, j_skip - Y264_LAMJ(lam, 1), lam, mode == 0);
    if (skip_ok && pc_satd16 >= 0 && pcen_on()) {   /* Y264_PSKIP_CENSUS: counts only */
        const pixel *src0 = f->src[0] + (mby * 16) * (size_t)f->src_stride[0] + mbx * 16;
        long satd_skip = y264_dsp.satd16x16(src0, f->src_stride[0], snap_skip, 16);
        pcen_note(mode, j_skip, j_inter, pc_satd16, satd_skip, mlam, &ires0, &ires,
                  smvx, smvy, pc_intra_ran);
    }

decided:
    PPCUT(5);
    if (s4) {                       /* S4: re-encode the winner with the full RDOQ */
        s_rd_trial = 0;
        if (mode == 1) {
            j_win = inter_rd_score(f, mbx, mby, ires.part, ires.mvx, ires.mvy,
                                   ires.pmvx, ires.pmvy, ires.ref, ires.sub,
                                   &ires, lam, 0);
            save_mb_rec(f, mbx, mby, snap_inter);
        } else if (mode == 2) {
            NLED_SITE(Y264_LED_SITE_PINTRA);
            analyze_intra_g(f, mbx, mby, &intra, best_satd);
            NLED_SITE(Y264_LED_SITE_PME);
        }
    }
    /* The intra trial above set mb_tr8 via its scratch write; set it to
 * the winning mode's transform size (intra I_8x8 or inter 8x8). */
    if (f->mb_tr8)
        f->mb_tr8[mby * f->wmb + mbx] =
            (mode == 2) ? (pixel)intra.use_i8 : (mode == 1 ? (pixel)ires.tr8 : 0);

    NLED(mb_p_skip, mode == 0);
    if (y264_skor_mode() == 1) y264_skor_put(f->skor_key, 0, mbx, mby, f->wmb, mode == 0);
    if (mode == 0) load_mb_rec(f, mbx, mby, snap_skip);
    else if (mode == 1) load_mb_rec(f, mbx, mby, snap_inter);
    /* mode 2: the intra reconstruction is already in f->rec */

    /* no coded residual: 4:4:4 keeps it in cbp444 (Cb/Cr coded like luma),
 * not the 4:2:0 chroma_result ires.cr which 4:4:4 never populates. */
    int no_res = (f->cf_idc == 3) ? (ires.cbp444 == 0)
                                  : (ires.cbp_luma == 0 && ires.cr.cbp == 0);
    int eff_skip = (mode == 0) ||
        (mode == 1 && ires.part == 0 && ires.ref[0] == 0 &&
         ires.mvx[0] == smvx && ires.mvy[0] == smvy && no_res);

    out->mode = (uint8_t)mode;
    out->eff_skip = (uint8_t)eff_skip;
    out->smvx = smvx;
    out->smvy = smvy;
    if (mode == 1) out->u.ires = ires;
    else if (mode == 2) out->u.intra = intra;

    if (mb_log_on()) {
        int part, ref; long amv = 0;
        if (mode == 0) {                 /* P_Skip */
            part = -1; ref = 0; amv = labs(smvx) + labs(smvy);
        } else if (mode == 1) {          /* inter: part shape, list-0 ref, MV magnitude */
            part = ires.part; ref = ires.ref[0];
            int np = ires.part == 3 ? 16 : (ires.part == 0 ? 1 : 2);
            for (int i = 0; i < np; i++) amv += labs(ires.mvx[i]) + labs(ires.mvy[i]);
        } else {                         /* intra: transform size in `part`, no ref/MV */
            part = 8 + intra.use_i8; ref = -1;
        }
        mb_log_line(f->poc, mbx, mby, 'P', mode, part, ref, amv, j_win,
                    ssd_luma_mb(f, mbx, mby));
    }
    if (rescensus_on() && mode == 1)         /* P verdicts: 0 skip, 1 inter, 2 intra */
        atomic_fetch_add_explicit(&g_res_mbs_inter, 1, memory_order_relaxed);
    if (pp_on) {
        uint64_t nw = bp_now();
        pp_loc[pp_stage] += nw - pp_last;
        int v = mode == 0 ? 0 : (mode == 1 ? 1 : 2);
        pp_mbs[v]++;
        for (int s = 0; s < PP_NSTAGE; s++) if (pp_loc[s]) {
            pp_ns[v][s] += pp_loc[s];
            pp_cnt[v][s]++;
        }
    }
#undef PPCUT
    STG_END();  /* STG_DECIDE (function-scope) */
}

/* W1: I-CAVLC pass-1a analysis on the row wavefront. Each worker gets a private
 * shallow copy of the frame struct so per-MB scalars (cur_qp*, qpd_coded) are
 * thread-local while all buffer pointers stay shared -- MB writes are disjoint and
 * neighbour reads are of wavefront-completed cells. Output identical to the serial
 * pass-1a (analyze_intra is position-independent of the QP chain, verified). */
struct icavlc_wf {
    y264_frame_t    *base;
    y264_frame_t    *fc;            /* per-worker frame copies [nthreads] */
    struct intra_mb *recs;
    int              wmb;
};
static void icavlc_wf_init(void *ctx, int idx)
{
    struct icavlc_wf *w = ctx;
    w->fc[idx] = *w->base;          /* private scalars, shared buffers */
}
static void icavlc_wf_cell(void *ctx, int idx, int r, int c)
{
    struct icavlc_wf *w = ctx;
    y264_frame_t *f = &w->fc[idx];
    struct intra_mb *o = &w->recs[r * w->wmb + c];
    mb_qp_pre(f, c, r);
    analyze_intra(f, c, r, o);
    author_intra_residual(f, c, r, o);
    if (f->mb_tr8) f->mb_tr8[r * w->wmb + c] = (pixel)o->use_i8;
}

/* W1: P-slice pass-1a analysis on the wavefront. Like the I version but analyze_p_mb
 * does motion search, so each worker installs the thread-local half-pel context
 * (thread_init) and gets its own analyze scratch. Predecessor QP pricing is always
 * on here (deterministic). */
#define P_WF_SNAP (16 * 16 + 2 * 16 * 16)
struct p_wf {
    y264_frame_t *base;
    y264_frame_t *fc;                       /* [nthreads] private frame copies */
    struct p_mb  *recs;
    int           wmb, mlam;
    long          lam;
    pixel       (*snap_skip)[P_WF_SNAP];    /* [nthreads] per-worker scratch */
    pixel       (*snap_inter)[P_WF_SNAP];
    int8_t      (*nzbuf)[16 + 32];
    /* staircase (row_done set): per-row true-QPY-chain resolve state. rowqc[r]
 * is the chain state AFTER row r; row r+1's resolve loads it (ordered by
 * the wavefront's top-right dependency, so no extra synchronisation). */
    struct qp_chain qc0;
    struct qp_chain *rowqc;
};

/* Staircase inline 1b: resolve row r's true QPY chain into f->mbqp the moment
 * the row's LAST cell completes, so the trailing per-row deblock (which needs
 * final per-MB QPs) never waits for the frame-level serial resolve -- which the
 * caller SKIPS when row_done is set. Byte-identical to that serial resolve:
 * same chain-in (row r-1's chain-out), same per-MB ops, raster order. codes_qpd
 * is a callback so the P and (identically-shaped) record types share this. */
static void wf_row_qpy_resolve(y264_frame_t *f, int r, int wmb,
                               const struct qp_chain *qc_in, struct qp_chain *qc_out,
                               const struct p_mb *recs)
{
    struct qp_chain hold;
    qp_save(f, &hold);                  /* protect this worker's analysis chain */
    qp_load(f, qc_in);
    for (int x = 0; x < wmb; x++) {
        mb_qp_pre(f, x, r);
        commit_qpy(f, x, r, mb_codes_qpd(f, &recs[x]));
    }
    qp_save(f, qc_out);
    qp_load(f, &hold);
}
static void p_wf_init(void *ctx, int idx)
{
    struct p_wf *w = ctx;
    w->fc[idx] = *w->base;
    y264_me_set_hpel((const y264_hpel_ref_t *)w->base->hpel_ctx,
                     w->base->hpel_n, w->base->hpel_stride);
}
/* Multi-frame pool re-entry (a worker resuming a PARKED row of this job after
 * serving another frame): re-install the thread-local hpel registry only. The
 * per-worker frame/cabac copies are per-job arrays, untouched by the other
 * job, and hold row-in-progress state that thread_init would clobber. */
static void p_wf_attach(void *ctx, int idx)
{
    struct p_wf *w = ctx;
    (void)idx;
    y264_me_set_hpel((const y264_hpel_ref_t *)w->base->hpel_ctx,
                     w->base->hpel_n, w->base->hpel_stride);
}
static void p_wf_cell(void *ctx, int idx, int r, int c)
{
    struct p_wf *w = ctx;
    y264_frame_t *f = &w->fc[idx];
    struct p_mb *rec = &w->recs[r * w->wmb + c];
    if (f->row_gate && c == 0)      /* staircase v3: wait for the prev anchor */
        f->row_gate(f->row_gate_ctx, r);
    f->prev_qp = predict_prev_qp(f, c, r);
    analyze_p_mb(f, c, r, w->mlam, w->lam, w->snap_skip[idx], w->snap_inter[idx],
                 w->nzbuf[idx], rec);
    if (rec->eff_skip) {
        clear_mb_nnz(f, c, r);
        mark_mb_meta(f, c, r, rec->smvx, rec->smvy, 0);
    } else if (rec->mode == 1) {
        commit_inter_motion(f, c, r, &rec->u.ires);
        author_inter_residual(f, c, r, &rec->u.ires);
    } else {
        author_intra_residual(f, c, r, &rec->u.intra);
        if (f->slice_type == 1) set_mb_intra_motion(f, c, r);
    }
    if (f->row_done && c == w->wmb - 1) {   /* staircase: row r fully analyzed */
        wf_row_qpy_resolve(f, r, w->wmb, r == 0 ? &w->qc0 : &w->rowqc[r - 1],
                           &w->rowqc[r], &w->recs[r * w->wmb]);
        f->row_done(f->row_done_ctx, r);
    }
}
/* Run P pass-1a on the pool; returns 1 on success, 0 to fall back to serial. */
static int p_wf_run(y264_frame_t *f, struct p_mb *recs, int mlam, long lam, int wt)
{
    ntp_pool_t *pool = (ntp_pool_t *)f->pool;
    struct p_wf w = { f, ntp_pool_slot(pool, 0, (size_t)wt * sizeof(y264_frame_t)),
                      recs, f->wmb, mlam, lam,
                      ntp_pool_slot(pool, 1, (size_t)wt * sizeof(pixel[P_WF_SNAP])),
                      ntp_pool_slot(pool, 2, (size_t)wt * sizeof(pixel[P_WF_SNAP])),
                      ntp_pool_slot(pool, 3, (size_t)wt * sizeof(int8_t[16 + 32])),
                      {0, 0, 0},
                      ntp_pool_slot(pool, 7, (size_t)f->hmb * sizeof(struct qp_chain)) };
    qp_save(f, &w.qc0);         /* == the caller's qc0 (saved before this run) */
    int ok = w.fc && w.snap_skip && w.snap_inter && w.nzbuf  && w.rowqc;
    if (ok) {
        ntp_prof_tag("analyze_P");
        ntp_wavefront_gated(pool, f->hmb, f->wmb, p_wf_init, p_wf_attach,
                            p_wf_cell, &w, f->row_ready, f->row_gate_ctx);
    }
    return ok;   /* scratch persists in the pool (freed at pool destroy) */
}

/* W1: B-slice pass-1a analysis on the wavefront (mirrors p_wf; B does bidirectional
 * ME + direct mode, one snap_best scratch). */
struct b_wf {
    y264_frame_t *base;
    y264_frame_t *fc;
    struct b_rec *recs;
    int           wmb, mlam;
    long          lam;
    int8_t      (*nzbuf)[16 + 32];
    pixel       (*snap_best)[P_WF_SNAP];
};
static void b_wf_init(void *ctx, int idx)
{
    struct b_wf *w = ctx;
    w->fc[idx] = *w->base;
    y264_me_set_hpel((const y264_hpel_ref_t *)w->base->hpel_ctx,
                     w->base->hpel_n, w->base->hpel_stride);
}
static void b_wf_attach(void *ctx, int idx)     /* see p_wf_attach */
{
    struct b_wf *w = ctx;
    (void)idx;
    y264_me_set_hpel((const y264_hpel_ref_t *)w->base->hpel_ctx,
                     w->base->hpel_n, w->base->hpel_stride);
}
static void b_wf_cell(void *ctx, int idx, int r, int c)
{
    struct b_wf *w = ctx;
    y264_frame_t *f = &w->fc[idx];
    struct b_rec *rec = &w->recs[r * w->wmb + c];
    if (f->row_gate && c == 0)      /* staircase: wait for the anchor's rows */
        f->row_gate(f->row_gate_ctx, r);
    f->prev_qp = predict_prev_qp(f, c, r);
    analyze_b_mb(f, c, r, w->mlam, w->lam, w->nzbuf[idx], w->snap_best[idx], w->recs, rec);
    if (rec->mode == 0) {
        clear_mb_nnz(f, c, r);
        commit_direct_motion(f, c, r, &rec->dmv);
    } else if (rec->mode == 1) {
        author_inter_residual(f, c, r, &rec->u.ir);
        commit_direct_motion(f, c, r, &rec->dmv);
    } else if (rec->mode == 2) {
        author_inter_residual(f, c, r, &rec->u.ir);
        if (rec->u.ir.bpart == 3) commit_b8_motion(f, c, r, &rec->u.ir);
        else if (rec->u.ir.bpart) commit_bpart_motion(f, c, r, &rec->u.ir);
        else                 commit_b_motion(f, c, r, &rec->u.ir);
    } else {
        author_intra_residual(f, c, r, &rec->u.intra);
    }
}
static int b_wf_run(y264_frame_t *f, struct b_rec *recs, int mlam, long lam, int wt)
{
    ntp_pool_t *pool = (ntp_pool_t *)f->pool;
    struct b_wf w = { f, ntp_pool_slot(pool, 0, (size_t)wt * sizeof(y264_frame_t)),
                      recs, f->wmb, mlam, lam,
                      ntp_pool_slot(pool, 3, (size_t)wt * sizeof(int8_t[16 + 32])),
                      ntp_pool_slot(pool, 1, (size_t)wt * sizeof(pixel[P_WF_SNAP])) };
    int ok = w.fc && w.nzbuf  && w.snap_best;
    if (ok) {
        ntp_prof_tag("analyze_B");
        ntp_wavefront_gated(pool, f->hmb, f->wmb, b_wf_init, b_wf_attach,
                            b_wf_cell, &w, f->row_ready, f->row_gate_ctx);
    }
    return ok;
}

/* W1: P-CABAC pass-1a on the wavefront. Adds to the P pattern the WPP row-private
 * RD estimator: each worker has a private cabac (ctx/est_ctx), each row seeds its
 * est_ctx from the row above's MB-1 snapshot (wpp[r-1], captured at that MB under
 * the wavefront lag) and advances it left-to-right via est_commit_p -- exactly the
 * serial WPP (est_ctx_mode 2), so bit-exact to it. */
struct pcb_wf {
    y264_frame_t *base;
    y264_frame_t *fc;                       /* [nthreads] private frame copies */
    y264_cabac_t *cb;                       /* [nthreads] private cabac (ctx/est_ctx) */
    struct p_mb  *recs;
    int           wmb, mlam;
    long          lam;
    uint8_t     (*wpp)[Y264_CABAC_CTX];     /* [hmb] per-row MB-1 est_ctx snapshot */
    const uint8_t *slice_ctx;
    pixel       (*snap_skip)[P_WF_SNAP];
    pixel       (*snap_inter)[P_WF_SNAP];
    int8_t      (*nzbuf)[16 + 32];
    struct qp_chain qc0;                    /* staircase inline 1b (see p_wf) */
    struct qp_chain *rowqc;
};
static void pcb_wf_init(void *ctx, int idx)
{
    struct pcb_wf *w = ctx;
    w->fc[idx] = *w->base;
    w->cb[idx] = *w->base->cabac;
    y264_cabac_rebind(&w->cb[idx]);   /* the copy aliased the base's buffer */
    w->fc[idx].cabac = &w->cb[idx];
    y264_me_set_hpel((const y264_hpel_ref_t *)w->base->hpel_ctx,
                     w->base->hpel_n, w->base->hpel_stride);
}
static void pcb_wf_attach(void *ctx, int idx)   /* see p_wf_attach: MUST not
 * touch cb[idx] -- its est_ctx
 * carries the parked row's WPP
 * state across the resume */
{
    struct pcb_wf *w = ctx;
    (void)idx;
    y264_me_set_hpel((const y264_hpel_ref_t *)w->base->hpel_ctx,
                     w->base->hpel_n, w->base->hpel_stride);
}
static void pcb_wf_cell(void *ctx, int idx, int r, int c)
{
    struct pcb_wf *w = ctx;
    y264_frame_t *f = &w->fc[idx];
    y264_cabac_t *cb = f->cabac;
    struct p_mb *rec = &w->recs[r * w->wmb + c];
    /* B6 diagnostic (2/40 native crashes land here with fc state dead --
 * NULL cabac / wild pointers -- under the wide-ref3 scaffold, no TSan
 * report). One predictable branch per cell; dumps the identity the
 * crash reports cannot. */
    if (!cb || !w->base) {
        fprintf(stderr, "pcb_wf_cell DEAD STATE: idx=%d r=%d c=%d w=%p base=%p "
                "fc=%p cb=%p wmb=%d" "\n", idx, r, c, (void *)w, (void *)w->base,
                (void *)w->fc, (void *)cb, w->wmb);
        abort();
    }
    if (f->row_gate && c == 0)      /* staircase v3: wait for the prev anchor */
        f->row_gate(f->row_gate_ctx, r);
    if (c == 0)                             /* WPP: seed est_ctx from row above MB-1 */
        memcpy(cb->est_ctx, r == 0 ? w->slice_ctx : w->wpp[r - 1], Y264_CABAC_CTX);
    f->prev_qp = predict_prev_qp(f, c, r);
    memcpy(cb->ctx, cb->est_ctx, Y264_CABAC_CTX);   /* RDOQ reads est_ctx via ctx */
    analyze_p_mb(f, c, r, w->mlam, w->lam, w->snap_skip[idx], w->snap_inter[idx],
                 w->nzbuf[idx], rec);
    if (rec->eff_skip) {
        clear_mb_nnz(f, c, r);
        mark_mb_meta(f, c, r, rec->smvx, rec->smvy, 0);
        f->mbcbp[r * f->mbcbp_stride + c] = (1 << 20);
    } else if (rec->mode == 1) {
        commit_inter_motion(f, c, r, &rec->u.ires);
        author_inter_cabac(f, c, r, &rec->u.ires);
    } else {
        author_intra_cabac(f, c, r, &rec->u.intra);
    }
    est_commit_p(f, c, r, rec);             /* advance this row's est_ctx */
    if (c == 1) memcpy(w->wpp[r], cb->est_ctx, Y264_CABAC_CTX);
    if (f->row_done && c == w->wmb - 1) {   /* staircase: row r fully analyzed */
        wf_row_qpy_resolve(f, r, w->wmb, r == 0 ? &w->qc0 : &w->rowqc[r - 1],
                           &w->rowqc[r], &w->recs[r * w->wmb]);
        f->row_done(f->row_done_ctx, r);
    }
}
static int pcb_wf_run(y264_frame_t *f, struct p_mb *recs, int mlam, long lam,
                      int wt, const uint8_t *slice_ctx)
{
    ntp_pool_t *pool = (ntp_pool_t *)f->pool;
    uint8_t (*wpp)[Y264_CABAC_CTX] = ntp_pool_slot(pool, 6, (size_t)f->hmb * sizeof *wpp);
    struct pcb_wf w = { f, ntp_pool_slot(pool, 0, (size_t)wt * sizeof(y264_frame_t)),
                        ntp_pool_slot(pool, 5, (size_t)wt * sizeof(y264_cabac_t)), recs, f->wmb,
                        mlam, lam, wpp, slice_ctx,
                        ntp_pool_slot(pool, 1, (size_t)wt * sizeof(pixel[P_WF_SNAP])),
                        ntp_pool_slot(pool, 2, (size_t)wt * sizeof(pixel[P_WF_SNAP])),
                        ntp_pool_slot(pool, 3, (size_t)wt * sizeof(int8_t[16 + 32])),
                          {0, 0, 0},
                        ntp_pool_slot(pool, 7, (size_t)f->hmb * sizeof(struct qp_chain)) };
    qp_save(f, &w.qc0);         /* == the caller's qc0 (saved before this run) */
    int ok = wpp && w.fc && w.cb && w.snap_skip && w.snap_inter && w.nzbuf &&
             w.rowqc;
    if (ok) {
        ntp_prof_tag("analyze_Pcb");
        ntp_wavefront_gated(pool, f->hmb, f->wmb, pcb_wf_init, pcb_wf_attach,
                            pcb_wf_cell, &w, f->row_ready, f->row_gate_ctx);
    }
    return ok;
}

/* W1: B-CABAC pass-1a on the wavefront (b_wf + the pcb WPP est_ctx machinery). */
struct bcb_wf {
    y264_frame_t *base;
    y264_frame_t *fc;
    y264_cabac_t *cb;
    struct b_rec *recs;
    int           wmb, mlam;
    long          lam;
    uint8_t     (*wpp)[Y264_CABAC_CTX];
    const uint8_t *slice_ctx;
    int8_t      (*nzbuf)[16 + 32];
    pixel       (*snap_best)[P_WF_SNAP];
};
static void bcb_wf_init(void *ctx, int idx)
{
    struct bcb_wf *w = ctx;
    w->fc[idx] = *w->base;
    w->cb[idx] = *w->base->cabac;
    y264_cabac_rebind(&w->cb[idx]);   /* the copy aliased the base's buffer */
    w->fc[idx].cabac = &w->cb[idx];
    y264_me_set_hpel((const y264_hpel_ref_t *)w->base->hpel_ctx,
                     w->base->hpel_n, w->base->hpel_stride);
}
static void bcb_wf_attach(void *ctx, int idx)   /* see pcb_wf_attach */
{
    struct bcb_wf *w = ctx;
    (void)idx;
    y264_me_set_hpel((const y264_hpel_ref_t *)w->base->hpel_ctx,
                     w->base->hpel_n, w->base->hpel_stride);
}
static void bcb_wf_cell(void *ctx, int idx, int r, int c)
{
    struct bcb_wf *w = ctx;
    y264_frame_t *f = &w->fc[idx];
    y264_cabac_t *cb = f->cabac;
    struct b_rec *rec = &w->recs[r * w->wmb + c];
    if (f->row_gate && c == 0)      /* staircase: wait for the anchor's rows */
        f->row_gate(f->row_gate_ctx, r);
    if (c == 0)
        memcpy(cb->est_ctx, r == 0 ? w->slice_ctx : w->wpp[r - 1], Y264_CABAC_CTX);
    f->prev_qp = predict_prev_qp(f, c, r);
    memcpy(cb->ctx, cb->est_ctx, Y264_CABAC_CTX);
    analyze_b_mb(f, c, r, w->mlam, w->lam, w->nzbuf[idx], w->snap_best[idx], w->recs, rec);
    if (rec->mode == 0) {
        clear_mb_nnz(f, c, r);
        commit_direct_motion(f, c, r, &rec->dmv);
        f->mbcbp[r * f->mbcbp_stride + c] = (1 << 20);
    } else if (rec->mode == 1) {
        author_b_direct_cabac(f, c, r, &rec->u.ir);
        commit_direct_motion(f, c, r, &rec->dmv);
    } else if (rec->mode == 2) {
        author_b_inter_cabac(f, c, r, &rec->u.ir);
        if (rec->u.ir.bpart == 3) commit_b8_motion(f, c, r, &rec->u.ir);
        else if (rec->u.ir.bpart) commit_bpart_motion(f, c, r, &rec->u.ir);
        else                 commit_b_motion(f, c, r, &rec->u.ir);
    } else {
        author_intra_cabac(f, c, r, &rec->u.intra);
        set_mb_intra_motion(f, c, r);
    }
    est_commit_b(f, c, r, rec);
    if (c == 1) memcpy(w->wpp[r], cb->est_ctx, Y264_CABAC_CTX);
}
static int bcb_wf_run(y264_frame_t *f, struct b_rec *recs, int mlam, long lam,
                      int wt, const uint8_t *slice_ctx)
{
    ntp_pool_t *pool = (ntp_pool_t *)f->pool;
    uint8_t (*wpp)[Y264_CABAC_CTX] = ntp_pool_slot(pool, 6, (size_t)f->hmb * sizeof *wpp);
    struct bcb_wf w = { f, ntp_pool_slot(pool, 0, (size_t)wt * sizeof(y264_frame_t)),
                        ntp_pool_slot(pool, 5, (size_t)wt * sizeof(y264_cabac_t)), recs, f->wmb,
                        mlam, lam, wpp, slice_ctx,
                        ntp_pool_slot(pool, 3, (size_t)wt * sizeof(int8_t[16 + 32])),
                          ntp_pool_slot(pool, 1, (size_t)wt * sizeof(pixel[P_WF_SNAP])) };
    int ok = wpp && w.fc && w.cb && w.nzbuf  && w.snap_best;
    if (ok) {
        ntp_prof_tag("analyze_Bcb");
        ntp_wavefront_gated(pool, f->hmb, f->wmb, bcb_wf_init, bcb_wf_attach,
                            bcb_wf_cell, &w, f->row_ready, f->row_gate_ctx);
    }
    return ok;
}

/* I-CABAC pass-1a on the wavefront. The obstacle to parallelising it is that
 * analyze_intra's RDOQ reads est_ctx and the I loop advances est_ctx MB by MB
 * from the live engine -- the same chain the row-private WPP estimator breaks
 * for P and B, and the same fix applies: est_ctx seeds per row from the row
 * above's MB-1 snapshot and advances left to right, so it is bit-exact to the
 * serial WPP form below and thread-count invariant.
 *
 * What it is worth is a scheduling number, not a work one: a 720p I frame is
 * ~26 ms of analysis on the driver with every worker asleep (measured on the
 * board's samsung cell, whose five scene cuts put 131 ms of an 722 ms t18 wall
 * in this loop -- Y264_THREAD_PROF's per-stage pool-idle column). Frames with
 * no cut carry one IDR and see a fifth of that. */
struct icb_wf {
    y264_frame_t    *base;
    y264_frame_t    *fc;                    /* [nthreads] private frame copies */
    y264_cabac_t    *cb;                    /* [nthreads] private cabac */
    struct intra_mb *recs;
    int              wmb;
    uint8_t        (*wpp)[Y264_CABAC_CTX];  /* [hmb] per-row MB-1 est_ctx snapshot */
    const uint8_t   *slice_ctx;
};
static void icb_wf_init(void *ctx, int idx)
{
    struct icb_wf *w = ctx;
    w->fc[idx] = *w->base;
    w->cb[idx] = *w->base->cabac;
    y264_cabac_rebind(&w->cb[idx]);   /* the copy aliased the base's buffer */
    w->fc[idx].cabac = &w->cb[idx];
}
/* Re-entry for a worker resuming a PARKED row: nothing to re-install (intra has
 * no hpel registry), and it must NOT re-init cb[idx] -- that est_ctx carries the
 * parked row's WPP state across the resume (see p_wf_attach). */
static void icb_wf_attach(void *ctx, int idx) { (void)ctx; (void)idx; }
static void icb_wf_cell(void *ctx, int idx, int r, int c)
{
    struct icb_wf *w = ctx;
    y264_frame_t *f = &w->fc[idx];
    y264_cabac_t *cb = f->cabac;
    struct intra_mb *o = &w->recs[r * w->wmb + c];
    if (f->row_gate && c == 0)              /* staircase: wait for the prev anchor */
        f->row_gate(f->row_gate_ctx, r);
    if (c == 0)                             /* WPP: seed est_ctx from row above MB-1 */
        memcpy(cb->est_ctx, r == 0 ? w->slice_ctx : w->wpp[r - 1], Y264_CABAC_CTX);
    mb_qp_pre(f, c, r);
    f->prev_qp = predict_prev_qp(f, c, r);
    memcpy(cb->ctx, cb->est_ctx, Y264_CABAC_CTX);   /* RDOQ reads est_ctx via ctx */
    analyze_intra(f, c, r, o);
    author_intra_cabac(f, c, r, o);
    est_commit_i(f, c, r, o);               /* advance this row's est_ctx */
    if (c == 1) memcpy(w->wpp[r], cb->est_ctx, Y264_CABAC_CTX);
}
static int icb_wf_run(y264_frame_t *f, struct intra_mb *recs, int wt,
                      const uint8_t *slice_ctx)
{
    ntp_pool_t *pool = (ntp_pool_t *)f->pool;
    uint8_t (*wpp)[Y264_CABAC_CTX] = ntp_pool_slot(pool, 6, (size_t)f->hmb * sizeof *wpp);
    struct icb_wf w = { f, ntp_pool_slot(pool, 0, (size_t)wt * sizeof(y264_frame_t)),
                        ntp_pool_slot(pool, 5, (size_t)wt * sizeof(y264_cabac_t)),
                        recs, f->wmb, wpp, slice_ctx };
    int ok = wpp && w.fc && w.cb;
    if (ok) {
        ntp_prof_tag("analyze_Icb");
        ntp_wavefront_gated(pool, f->hmb, f->wmb, icb_wf_init, icb_wf_attach,
                            icb_wf_cell, &w, f->row_ready, f->row_gate_ctx);
    }
    return ok;
}

/* W2: the serial entropy emit of a P slice, extracted from the analyze so it can
 * later run on the background emit thread while the next frame's wavefront runs.
 * Byte-identical to the inline pass-2 loops. */
static void emit_p_cavlc(y264_bs_t *bs, y264_frame_t *f, struct p_mb *recs,
                         const struct qp_chain *qc0)
{
    qp_load(f, qc0);
    int skip_run = 0;
    for (int mby = 0; mby < f->hmb; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++) {
            struct p_mb *r = &recs[mby * f->wmb + mbx];
            mb_qp_pre(f, mbx, mby);
            if (r->eff_skip) {
                skip_run++;
            } else if (r->mode == 1) {
                y264_bs_write_ue(bs, skip_run); skip_run = 0;
                emit_inter_mb(bs, f, mbx, mby, &r->u.ires);
            } else {
                y264_bs_write_ue(bs, skip_run); skip_run = 0;
                emit_intra_syntax(bs, f, mbx, mby, 5, &r->u.intra);
            }
            mb_qp_post(f, mbx, mby);
        }
    y264_bs_write_ue(bs, skip_run);
}
static void emit_p_cabac(y264_frame_t *f, struct p_mb *recs,
                         const struct qp_chain *qc0, const uint8_t *slice_ctx)
{
    y264_cabac_t *pc = f->cabac;
    memcpy(pc->ctx, slice_ctx, Y264_CABAC_CTX);   /* restore slice-init (pass 1 clobbered) */
    qp_load(f, qc0);
    g_bitstat_live = bitstat_on();
    long bst0 = g_bitstat_live ? cab_pos(pc) : 0;
    for (int mby = 0; mby < f->hmb; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++) {
            struct p_mb *r = &recs[mby * f->wmb + mbx];
            mb_qp_pre(f, mbx, mby);
            cabac_mb_skip(pc, f, mbx, mby, r->eff_skip, 11);
            if (!r->eff_skip) {
                if (r->mode == 1) emit_inter_cabac(pc, f, mbx, mby, &r->u.ires);
                else              emit_intra_cabac(pc, f, mbx, mby, &r->u.intra, 1);
            }
            mb_qp_post(f, mbx, mby);
            int last = (mby == f->hmb - 1 && mbx == f->wmb - 1);
            y264_cabac_encode_terminate(pc, last);
        }
    if (g_bitstat_live) {
        g_bitstat_live = 0;
        long tot = cab_pos(pc) - bst0;   /* other = skip flags, terminates, intra MBs */
        fprintf(stderr, "BITSTAT: poc=%d mode=%ld mv=%ld coef=%ld other=%ld tot=%ld"
                " nmvd=%ld summvd=%ld p16=%ld p16x8=%ld p8x16=%ld p8=%ld\n",
                f->poc, g_bits_mode, g_bits_mv, g_bits_coef,
                tot - g_bits_mode - g_bits_mv - g_bits_coef, tot,
                g_nmvd, g_summvd, g_part[0], g_part[1], g_part[2], g_part[3]);
        g_bits_mode = g_bits_mv = g_bits_coef = 0;
        g_nmvd = g_summvd = 0;
        g_part[0] = g_part[1] = g_part[2] = g_part[3] = 0;
    }
}

/* I-slice emit halves (W2 split), matching the P/B emit_* pairs. */
static void emit_i_cabac(y264_frame_t *f, struct intra_mb *recs,
                         const struct qp_chain *qc0, const uint8_t *slice_ctx)
{
    y264_cabac_t *c = f->cabac;
    memcpy(c->ctx, slice_ctx, Y264_CABAC_CTX);   /* pass 1 clobbered ctx via est_commit_i */
    qp_load(f, qc0);
    for (int mby = 0; mby < f->hmb; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++) {
            mb_qp_pre(f, mbx, mby);
            emit_intra_cabac(c, f, mbx, mby, &recs[mby * f->wmb + mbx], 0);
            mb_qp_post(f, mbx, mby);
            int last = (mby == f->hmb - 1 && mbx == f->wmb - 1);
            y264_cabac_encode_terminate(c, last);
        }
}

static void emit_i_cavlc(y264_bs_t *bs, y264_frame_t *f, struct intra_mb *recs,
                         const struct qp_chain *qc0)
{
    qp_load(f, qc0);
    for (int mby = 0; mby < f->hmb; mby++)
        for (int mbx = 0; mbx < f->wmb; mbx++) {
            mb_qp_pre(f, mbx, mby);
            emit_intra_syntax(bs, f, mbx, mby, 0, &recs[mby * f->wmb + mbx]);
            mb_qp_post(f, mbx, mby);
        }
}

/* W2 emit-overlap: the job produced by y264_frame_analyze and consumed by
 * y264_frame_emit. Owns the malloc'd records array. qc0 is the frame-start QP
 * chain; slice_ctx (CABAC) is the slice-init context to restore before pass 2;
 * est_total feeds the optional est-vs-real self-check (Y264_EST_CHECK). */
struct y264_emit_job {
    int  slice_type;        /* 0 = I, 1 = P, 2 = B */
    int  cabac;
    void *recs;             /* struct intra_mb* / p_mb* / b_rec* per slice_type */
    struct qp_chain qc0;
    uint8_t slice_ctx[Y264_CABAC_CTX];
    long est_total;
};

/* W2 pass 1: mode decision + reconstruction + decision grids + the raster QPY
 * chain for every MB. Returns a heap job for pass 2 (y264_frame_emit); rec[] is
 * left ready to serve as a reference (deblock still runs in build_slice). */
y264_emit_job_t *y264_frame_analyze(y264_frame_t *f)
{
    struct y264_emit_job *j = malloc(sizeof *j);
    j->slice_type = f->slice_type;
    j->cabac = f->cabac ? 1 : 0;
    j->est_total = 0;
    aq_analyze(f);
    if (f->mb_tr8)
        memset(f->mb_tr8, 0, (size_t)f->wmb * f->hmb);
    if (f->slice_type == 0 && f->cabac) {
        /* I slice, CABAC: two passes (W0 4e). Pass 1 analyses + authors grids +
 * records + advances est_ctx via est_commit_i; pass 2 emits, owning the
 * engine ctx + QP chain. analyze_intra's RDOQ reads est_ctx through the
 * CABAC rate model, so pass 1 must keep est_ctx == what single-pass tracked
 * from the live engine (the I slice always tracks live ctx, mode-independent).
 * est_commit_i clobbers c->ctx, so restore slice-init before pass 2. */
        y264_cabac_t *c = f->cabac;
        int m6b = icb_wf_env() ? est_ctx_mode() : 0;
        for (int i = 0; i < f->wmb * f->hmb; i++) f->mbcbp[i] = -1;
        struct intra_mb *recs = malloc((size_t)f->wmb * f->hmb * sizeof(*recs));
        struct qp_chain qc0; qp_save(f, &qc0);
        uint8_t wpp_ctx[Y264_CABAC_CTX];
        memcpy(c->est_ctx, c->ctx, Y264_CABAC_CTX);   /* est_ctx = slice-init */
        memcpy(j->slice_ctx, c->ctx, Y264_CABAC_CTX);
        memcpy(wpp_ctx, c->ctx, Y264_CABAC_CTX);
        int wt = f->pool ? ntp_pool_nthreads((ntp_pool_t *)f->pool) : 0;
        int pred = icb_wf_env() && ((wt > 1 && m6b == 2) || wf_predqp_env());
        int done_1a = wt > 1 && m6b == 2 && icb_wf_run(f, recs, wt, j->slice_ctx);
        if (!done_1a)
            for (int mby = 0; mby < f->hmb; mby++) {
                if (m6b == 1)      memcpy(c->est_ctx, j->slice_ctx, Y264_CABAC_CTX);
                else if (m6b == 2) memcpy(c->est_ctx, wpp_ctx, Y264_CABAC_CTX);
                for (int mbx = 0; mbx < f->wmb; mbx++) {
                    struct intra_mb *o = &recs[mby * f->wmb + mbx];
                    if (f->row_gate && mbx == 0)    /* staircase (serial fallback) */
                        f->row_gate(f->row_gate_ctx, mby);
                    mb_qp_pre(f, mbx, mby);
                    if (pred) f->prev_qp = predict_prev_qp(f, mbx, mby);
                    memcpy(c->ctx, c->est_ctx, Y264_CABAC_CTX);   /* RDOQ in analyze reads est_ctx via c->ctx */
                    analyze_intra(f, mbx, mby, o);
                    author_intra_cabac(f, mbx, mby, o);
                    est_commit_i(f, mbx, mby, o);            /* advance est_ctx for the next MB */
                    if (m6b == 2 && mbx == 1) memcpy(wpp_ctx, c->est_ctx, Y264_CABAC_CTX);
                    if (!pred) commit_qpy(f, mbx, mby, intra_codes_qpd(f, o));
                }
            }
        if (pred) {                                   /* serial QPY-chain resolve */
            qp_load(f, &qc0);
            for (int mby = 0; mby < f->hmb; mby++)
                for (int mbx = 0; mbx < f->wmb; mbx++) {
                    mb_qp_pre(f, mbx, mby);
                    commit_qpy(f, mbx, mby, intra_codes_qpd(f, &recs[mby * f->wmb + mbx]));
                }
        }
        j->recs = recs;
        j->qc0 = qc0;
        return j;
    }
    if (f->slice_type == 0) {
        /* I slice, CAVLC: W1 three-way split. Pass 1a analyses + authors grids
 * (parallelisable -- no raster dependency); pass 1b resolves the raster
 * QPY chain serially (commit_qpy); pass 2 emits. For I slices every MB
 * codes residual, so the raster QP predecessor the pass-1 RD prices
 * against equals the chain value -> byte-identical to the interleaved form. */
        struct intra_mb *recs = malloc((size_t)f->wmb * f->hmb * sizeof(*recs));
        struct qp_chain qc0; qp_save(f, &qc0);
        int wt = f->pool ? ntp_pool_nthreads((ntp_pool_t *)f->pool) : 0;
        if (wt > 1) {                                          /* 1a: parallel analyze */
            struct icavlc_wf w = { f, ntp_pool_slot((ntp_pool_t *)f->pool, 0,
                                                    (size_t)wt * sizeof(y264_frame_t)),
                                   recs, f->wmb };
            if (w.fc) {
                ntp_prof_tag("analyze_Icavlc");
                ntp_wavefront((ntp_pool_t *)f->pool, f->hmb, f->wmb,
                              icavlc_wf_init, icavlc_wf_cell, &w);
            } else wt = 0;                                     /* OOM -> fall through serial */
        }
        if (wt <= 1)
            for (int mby = 0; mby < f->hmb; mby++)             /* 1a: serial analyze */
                for (int mbx = 0; mbx < f->wmb; mbx++) {
                    struct intra_mb *o = &recs[mby * f->wmb + mbx];
                    mb_qp_pre(f, mbx, mby);
                    analyze_intra(f, mbx, mby, o);
                    author_intra_residual(f, mbx, mby, o);
                    if (f->mb_tr8) f->mb_tr8[mby * f->wmb + mbx] = (pixel)o->use_i8;
                }
        for (int mby = 0; mby < f->hmb; mby++)                 /* 1b: QPY resolve */
            for (int mbx = 0; mbx < f->wmb; mbx++) {
                mb_qp_pre(f, mbx, mby);
                commit_qpy(f, mbx, mby, intra_codes_qpd(f, &recs[mby * f->wmb + mbx]));
            }
        j->recs = recs;
        j->qc0 = qc0;
        return j;
    }
    if (f->slice_type == 2) {
        j->recs = analyze_b_slice(f, &j->qc0, j->slice_ctx, &j->est_total);
        return j;
    }

    /* P slice: RD mode decision per MB, J = SSD + lambda*bits. */
    int mlam = lambda_me(f->qp);            /* motion-search MV bias (SATD/SAD domain) */
    long lam = lambda_mode16(f->qp);          /* RD multiplier (SSD domain) */
    y264_cabac_t *pc = f->cabac;
    if (pc) {
        for (int i = 0; i < f->wmb * f->hmb; i++) f->mbcbp[i] = -1;
        size_t nmv = (size_t)f->mv_stride * f->hmb * 4;
        for (size_t i = 0; i < nmv; i++) { f->mvdx[i] = 0; f->mvdy[i] = 0; }
    }
    pixel snap_skip[16 * 16 + 2 * 16 * 16];
    pixel snap_inter[16 * 16 + 2 * 16 * 16];
    int8_t nzbuf[16 + 32];

    if (pc) {
        /* CABAC P: two passes (W0 4e). Pass 1 analyses + authors grids + records +
 * advances the row-private est_ctx via est_commit_p in ALL modes (there is no
 * live engine in pass 1: est_decision transitions ctx with the same tables as
 * the real coder, so est_commit_p leaves est_ctx == what a real emit would --
 * mode 0 stays byte-identical). Pass 2 emits, owning the engine ctx. The QP
 * chain is advanced in pass 1 for the RD trials (mb_codes_qpd), rewound, and
 * re-driven for real in pass 2. est_commit_p uses the pre-advance prev_qp,
 * matching what the pass-2 emit codes. Since est_commit_p writes pc->ctx, it
 * is restored to slice-init before pass 2. (The WPP default, mode 2, is BD-
 * gated, not byte-identical.) */
        int m6b = est_ctx_mode();
        uint8_t slice_ctx[Y264_CABAC_CTX], wpp_ctx[Y264_CABAC_CTX];
        long est_total = 0;
        struct p_mb *recs = malloc((size_t)f->wmb * f->hmb * sizeof(*recs));
        struct qp_chain qc0; qp_save(f, &qc0);
        memcpy(pc->est_ctx, pc->ctx, Y264_CABAC_CTX);   /* est_ctx = slice-init */
        memcpy(slice_ctx, pc->ctx, Y264_CABAC_CTX);
        memcpy(wpp_ctx, pc->ctx, Y264_CABAC_CTX);
        int wt = f->pool ? ntp_pool_nthreads((ntp_pool_t *)f->pool) : 0;
        /* the wavefront needs per-row WPP est_ctx (mode 2); predecessor pricing then
 * makes analyze position-deterministic. Serial WPP stays true-chain unless
 * Y264_WF_PREDQP forces the predecessor path for BD comparison. */
        int pred = (wt > 1 && m6b == 2) || wf_predqp_env();
        int done_1a = wt > 1 && m6b == 2 && pcb_wf_run(f, recs, mlam, lam, wt, slice_ctx);
        if (!done_1a)
            for (int mby = 0; mby < f->hmb; mby++) {
                if (m6b == 1)      memcpy(pc->est_ctx, slice_ctx, Y264_CABAC_CTX);
                else if (m6b == 2) memcpy(pc->est_ctx, wpp_ctx, Y264_CABAC_CTX);
                for (int mbx = 0; mbx < f->wmb; mbx++) {
                    struct p_mb *r = &recs[mby * f->wmb + mbx];
                    if (f->row_gate && mbx == 0)    /* staircase (serial fallback) */
                        f->row_gate(f->row_gate_ctx, mby);
                    if (pred) f->prev_qp = predict_prev_qp(f, mbx, mby);
                    memcpy(pc->ctx, pc->est_ctx, Y264_CABAC_CTX);   /* RDOQ reads est_ctx via ctx */
                    analyze_p_mb(f, mbx, mby, mlam, lam, snap_skip, snap_inter, nzbuf, r);
                    if (r->eff_skip) {
                        clear_mb_nnz(f, mbx, mby);
                        mark_mb_meta(f, mbx, mby, r->smvx, r->smvy, 0);
                        f->mbcbp[mby * f->mbcbp_stride + mbx] = (1 << 20);
                    } else if (r->mode == 1) {
                        commit_inter_motion(f, mbx, mby, &r->u.ires);
                        author_inter_cabac(f, mbx, mby, &r->u.ires);
                    } else {
                        author_intra_cabac(f, mbx, mby, &r->u.intra);
                    }
                    est_total += est_commit_p(f, mbx, mby, r);
                    if (m6b == 2 && mbx == 1) memcpy(wpp_ctx, pc->est_ctx, Y264_CABAC_CTX);
                    if (!pred) commit_qpy(f, mbx, mby, mb_codes_qpd(f, r));
                }
            }
        if (pred && !(done_1a && f->row_done)) {      /* serial QPY-chain resolve
 * (staircase did it per row) */
            qp_load(f, &qc0);
            for (int mby = 0; mby < f->hmb; mby++)
                for (int mbx = 0; mbx < f->wmb; mbx++) {
                    mb_qp_pre(f, mbx, mby);
                    commit_qpy(f, mbx, mby, mb_codes_qpd(f, &recs[mby * f->wmb + mbx]));
                }
        }
        j->recs = recs;
        j->qc0 = qc0;
        memcpy(j->slice_ctx, slice_ctx, sizeof slice_ctx);
        j->est_total = est_total;
        return j;
    }

    /* CAVLC: two passes (W0 step 4). Pass 1 analyses + reconstructs + authors all
 * grids (nnz / motion / i4mode / mb_tr8) and records the decision; pass 2
 * walks the records in raster order and emits, owning the serial skip_run and
 * QP-delta chain. Reconstruction and grids are identical to single-pass, so
 * CAVLC output is byte-identical.
 *
 * The RD cost trials in pass 1 read f->prev_qp (the mb_qp_delta se(v) length),
 * so pass 1 must advance that raster-order chain as the winner would have; the
 * chain is then rewound and re-driven for real in pass 2. */
    struct p_mb *recs = malloc((size_t)f->wmb * f->hmb * sizeof(*recs));
    struct qp_chain qc0; qp_save(f, &qc0);          /* frame-start QP chain */
    int wt = f->pool ? ntp_pool_nthreads((ntp_pool_t *)f->pool) : 0;
    int pred = wt > 1 || wf_predqp_env();           /* W1: deterministic delta pricing */
    int done_1a = wt > 1 && p_wf_run(f, recs, mlam, lam, wt);  /* 1a: parallel analyze */
    if (!done_1a)
        for (int mby = 0; mby < f->hmb; mby++)      /* 1a: serial analyze + author */
            for (int mbx = 0; mbx < f->wmb; mbx++) {
                struct p_mb *r = &recs[mby * f->wmb + mbx];
                if (f->row_gate && mbx == 0)        /* staircase (serial fallback) */
                    f->row_gate(f->row_gate_ctx, mby);
                if (pred) f->prev_qp = predict_prev_qp(f, mbx, mby);
                analyze_p_mb(f, mbx, mby, mlam, lam, snap_skip, snap_inter, nzbuf, r);
                if (r->eff_skip) {
                    clear_mb_nnz(f, mbx, mby);
                    mark_mb_meta(f, mbx, mby, r->smvx, r->smvy, 0);
                } else if (r->mode == 1) {
                    commit_inter_motion(f, mbx, mby, &r->u.ires);
                    author_inter_residual(f, mbx, mby, &r->u.ires);
                } else {
                    author_intra_residual(f, mbx, mby, &r->u.intra);
                    if (f->slice_type == 1) set_mb_intra_motion(f, mbx, mby);
                }
                if (!pred)                          /* interleaved true chain (default) */
                    commit_qpy(f, mbx, mby, mb_codes_qpd(f, r));
            }
    if (pred && !(done_1a && f->row_done)) {        /* 1b: serial QPY-chain resolve
 * (staircase did it per row) */
        qp_load(f, &qc0);
        for (int mby = 0; mby < f->hmb; mby++)
            for (int mbx = 0; mbx < f->wmb; mbx++) {
                mb_qp_pre(f, mbx, mby);
                commit_qpy(f, mbx, mby, mb_codes_qpd(f, &recs[mby * f->wmb + mbx]));
            }
    }

    j->recs = recs;
    j->qc0 = qc0;
    return j;
}

/* W2 pass 2: write the slice_data bitstream from the analyze job, then free it.
 * Dispatches over slice type / entropy coder to the matching emit_* half. */
void y264_frame_emit(y264_bs_t *bs, y264_frame_t *f, y264_emit_job_t *job)
{
    uint8_t *cst = (job->cabac && f->cabac) ? f->cabac->p : NULL;
    if (unsafe_no_emit()) {         /* ceiling probe: garbage out, see above */
        free(job->recs);
        free(job);
        return;
    }
    switch (job->slice_type) {
    case 0:
        if (job->cabac) emit_i_cabac(f, job->recs, &job->qc0, job->slice_ctx);
        else            emit_i_cavlc(bs, f, job->recs, &job->qc0);
        break;
    case 2:
        if (job->cabac) emit_b_cabac(f, job->recs, &job->qc0, job->slice_ctx);
        else            emit_b_cavlc(bs, f, job->recs, &job->qc0);
        break;
    default:
        if (job->cabac) emit_p_cabac(f, job->recs, &job->qc0, job->slice_ctx);
        else            emit_p_cavlc(bs, f, job->recs, &job->qc0);
        break;
    }
    /* est-vs-real self-check (Y264_EST_CHECK), P/B CABAC only; I has no est. */
    if (cst && job->slice_type != 0 && est_check_on()) {
        long ab = (long)(f->cabac->p - cst) * 8;
        fprintf(stderr, "EST-CHECK %c poc=%d est=%ld actual=%ld ratio=%.3f\n",
                job->slice_type == 2 ? 'B' : 'P', f->poc, job->est_total / 256,
                ab, ab ? (job->est_total / 256.0) / ab : 0);
    }
    free(job->recs);
    free(job);
}

void y264_frame_encode(y264_bs_t *bs, y264_frame_t *f)
{
    y264_emit_job_t *job = y264_frame_analyze(f);
    y264_frame_emit(bs, f, job);
}

/* Resolve every env-gated lazy static reachable from the analyze wavefront ONCE,
 * on the calling (main) thread, before any worker runs. Each accessor caches its
 * getenv result in a file-static keyed only on the env var (arg-independent), so
 * after this warm-up the wavefront threads only READ them -> no data race. The
 * CLI opens+closes a prime encoder on the main thread before spawning GOP
 * workers, which is where this runs. */
void y264_mb_warm_statics(void)
{
    (void)mb_log_on(); (void)wf_predqp_env(); (void)tr_pre_on(10);
    (void)rdoq_seed64(); (void)viterbi_rdoq(10); (void)psy_viterbi_on(); (void)intra_fine_on(10, -1, 0);
    (void)intra_screen_on(10); (void)intra_screen_pure(); (void)intra_rdbonus(0);
    (void)me_lambda_old(); (void)lambda_me(26);
    (void)lambda_mode(26); (void)trellis_lambda_env(); (void)est_check_on(); (void)est_ctx_mode();
    (void)unsafe_no_emit();
    (void)icb_wf_env(); (void)pskip_exit_mode(); (void)b_intra_fine_env();
    (void)b_skip_exit_env(); (void)p8_seed16_on(); (void)tr_share_on();
    (void)bprof_env(); (void)bprof2_env(); (void)bitstat_on(); (void)rescensus_on();
    (void)resprof_on(); (void)trprof_on(); (void)tr_pre_fix(); (void)est_prof_on(); (void)est_scrtrace_on();
    /* Read-compare-write, not a plain store: this warm runs on EVERY
 * encoder_open, and the CLI opens one encoder per GOP from concurrent
 * workers, so an unconditional store here is a genuine perpetual writer --
 * every worker rewrote the same global for the life of the encode. It was
 * the one report left on the GOP-parallel 4:4:4 repro after the whole
 * env-gate class was warmed. The prime encoder does the first write on the
 * main thread; after that this compares equal and no worker writes. */
    { const int tl = trprof_on(); if (y264_tl_on != tl) y264_tl_on = tl; }
    (void)tr_pre_bias();
    (void)cabac_rd_on(); (void)midskip_on(); (void)midskip_margin();
    (void)part_earlyterm(); (void)temporal_seed_on(10); (void)lr_seed_on(); (void)rect_refs_on(); (void)p8_refclamp_on(); (void)p_ref0exit_on(); (void)p_ref0exit_k();
    (void)rich_seeds(); (void)b_seeds_on(); (void)b_thresh_on(10);
    (void)probe_trellis_on();
    (void)trellis_commit_on(10, 1); (void)intra_admit_m16(); (void)intra_admit_m16_b(); (void)direct_score_on(); (void)intra_fine_m16();
    (void)b_rect_on();
    (void)y264_mbt_derived();     /* before aq_mode_env: its default reads it */
    (void)aq_mode_env(); (void)aq2_bias_env(); (void)aq_boost_env();
    (void)aq_octile_env(); (void)aq_dark_env(); (void)psy_trellis_env(); (void)psy_ramp_env();
    (void)psy_rd_env(); (void)psy_rd_ramp_env(); (void)psy_chroma_x256();
    (void)bpo_env(); (void)probe_deadzone_env(); (void)part_thresh(1);
    (void)part_important_off(); (void)part_hetero_pct(); (void)part_slack_x4(); (void)mb_lambda_on(); (void)mb_lambda_qp0();
    (void)lambda16_on(); (void)rd_admit_16(0, 0);
    (void)p_rect_on();
    { int t4, t8, h, l; (void)dctdec_cfg(&t4, &t8); (void)qpelrd_cfg(&h, &l); }
    /* 08-29 TSan sweep, second pass: the B-decision and probe gates. Every one
 * is a lazy env static read from analyse on wavefront workers, so first
 * touch races between them -- all benign same-value init, but each one is
 * a report that fogs the next real hunt. Resolved here on the main thread
 * so the workers only ever read. (scripts/env_gate_audit.py enumerates the
 * class; run it after adding a gate.) */
    (void)dctdec_tab4(); (void)me_et_imp();
    (void)b_8x8_on(); (void)b8_direct_on(); (void)b8_nord_on();
    (void)b_skip_exit_ssd(); (void)b8_stat_on(); (void)b8_rate_on();
    (void)b8_qgate(); (void)bmb_cost_on(); (void)bbi_pen(); (void)bbi_rd_pen();
    (void)b_rect_seed_on(); (void)flatskip_stat_on(); (void)bdir_stat_on();
    (void)pprune_on(); (void)pcen_on(); (void)bskip_admit_nb(); (void)bskip_admit_mv(); (void)tdir_l0only_on(); (void)tdir_why_on();
    y264_me_warm_statics();     /* + the motion-search env statics */
    /* Premultiplied MV-cost tables for every lambda ME can see: both lambda_me
 * variants over the full QP range (the env picks one; priming both is
 * harmless -- a few unused 16K tables). */
    for (int qp = 0; qp <= 51; qp++) {
        y264_me_prime_lambda(lambda_me(qp));
        y264_me_prime_lambda(4 + qp / 4);
    }
    y264_mc_warm_statics();
    y264_transform_warm_statics();
}

