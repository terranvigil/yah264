/*
 * sfpipe.c - THROWAWAY serial-frame pipeline prototype (docs/archive/serial-frame-prototype.md)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * NOT part of the shipped encoder. Answers one question from the frame-pipeline
 * rearchitecture investigation's reopen condition: does one-thread-per-whole-frame
 * pipelining (x264's frame-threading execution shape: no wavefront, no in-frame
 * row parallelism, consecutive frames offset by a row-gated reference dependency)
 * beat the row-wavefront on small grids?
 *
 * Scope, deliberately narrow so the number is cheap but the WORK is real:
 * - IPPP only (bframes 0), ref 1, CQP, CABAC, 4:2:0 8-bit, no lookahead
 * (no mb-tree, no scene-cut), single IDR at frame 0.
 * - Each frame is encoded start-to-finish by ONE thread via the real
 * y264_frame_analyze / y264_frame_emit, with the real per-row trailer
 * (deblock + border-extend + hpel band) run inline on the same thread
 * between rows -- exactly x264's <reference-internal> shape.
 * - Frame i's ME reads frame i-1's actual recon/hpel, row-gated with the
 * staircase's own publish/consume budget (stair_trailer_task's bounds) and
 * the fixed vertical MV clamp 16*LAG-24 px via f.stair_clamp0_poc.
 * - The P-slice weighted-prediction estimate uses the staircase's source-DC
 * substitution when pipelined (the reference recon is still streaming).
 *
 * Honesty check built in: with --lag 0 (clamp off) and -t 1 the output is
 * byte-identical to
 * yah264 --qp Q --bframes 0 --ref 1 --rc-lookahead 0 --no-scenecut --no-sei
 * minus the per-frame lowres analysis the real encoder still runs (see the doc).
 *
 * Build (not wired into meson, on purpose):
 * clang -O3 -std=c11 -Iinclude -Isrc -DY264_BIT_DEPTH=8 tools/sfpipe.c \
 * build/libyah264.a -lpthread -lm -o build/sfpipe
 */
#include "yah264.h"
#include "encoder/macroblock.h"
#include "encoder/me.h"
#include "encoder/set.h"
#include "encoder/cabac.h"
#include "encoder/cavlc.h"
#include "encoder/deblock.h"
#include "dsp/mc.h"
#include "dsp/pixel.h"
#include "dsp/transform.h"
#include "common/bitstream.h"
#include "common/nal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#define LB Y264_LUMA_BORDER
#define CB Y264_CHROMA_BORDER

/* ---- geometry / config (set once before threads start) ---- */
static int g_w, g_h, g_wmb, g_hmb, g_pw, g_ph;      /* padded dims */
static int g_ps[3];                                  /* plane strides */
static int g_mvs;                                    /* mv grid stride */
static int g_qp = 26;
static int g_lag = 4;                                /* 0 = clamp off (full-frame dep) */
static int g_threads = 1;
static int g_nslots;
static int g_nframes;
static int g_subme = 7, g_subpel = 2;                /* medium tier (CLI default) */
static size_t g_rbsp_cap;

/* ---- input frames (raster, preloaded) ---- */
typedef struct { uint8_t *y, *u, *v; } rawframe_t;
static rawframe_t *g_in;

/* ---- per-frame output ---- */
static uint8_t **g_out;
static size_t   *g_outlen;

/* ---- lifecycle (global lock; cold path) ---- */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static uint8_t *g_analyzed, *g_retired, *g_summed;   /* per frame index */
static uint64_t *g_srcsum;                           /* per frame index */
static _Atomic int g_next;

struct slot {
    /* planes: interior pointers, bordered allocations */
    pixel *src[3], *rec[3], *hp[3];
    int32_t *hp_scratch;
    /* decision grids (private per in-flight frame) */
    int8_t *nnz[3], *i4mode, *refidx, *refidx1, *colref;
    int16_t *mvx, *mvy, *mvx1, *mvy1, *mvdx, *mvdy, *mvdx1, *mvdy1;
    int16_t *colmvx, *colmvy, *colpoc;
    int *mbcbp;
    uint8_t *mb_tr8;
    uint8_t *rbsp;
    /* publish watermark (hot path) */
    pthread_mutex_t mx; pthread_cond_t cv;
    int pub;                        /* consumable rows; g_hmb = fully done */
    /* trailer state (owner thread only) */
    int trail, ext_l, ext_c, hp_done;
    /* frame ctx */
    y264_frame_t f;
    y264_bs_t bs;
    y264_cabac_t cb;
    y264_hpel_ref_t hctx;
    struct slot *prev;              /* reference producer (frame-1's slot) */
    int frame;
};
static struct slot *g_slot;

static pixel *palloc(int w, int h, int b)
{
    size_t stride = (size_t)w + 2 * b;
    pixel *base = malloc(stride * (h + 2 * b) * sizeof(pixel));
    if (!base) { fprintf(stderr, "sfpipe: OOM\n"); exit(1); }
    memset(base, 0, stride * (h + 2 * b) * sizeof(pixel));
    return base + (size_t)b * stride + b;
}

static void pad_plane(pixel *dst, int dstride, int dw, int dh,
                      const uint8_t *src, int sstride, int sw, int sh)
{
    for (int y = 0; y < dh; y++) {
        int sy = (y < sh) ? y : sh - 1;
        const uint8_t *s = src + (size_t)sy * sstride;
        pixel *d = dst + (size_t)y * dstride;
        int x = 0;
        for (; x < sw; x++) d[x] = s[x];
        pixel edge = s[sw - 1];
        for (; x < dw; x++) d[x] = edge;
    }
}

/* estimate_wp_luma, verbatim from encoder.c (8-bit path). */
static int wp_estimate(const pixel *src, const pixel *ref, int denom,
                       int64_t sumref_ovr, int *w, int *o)
{
    int W = g_w, H = g_h, ss = g_ps[0], rs = g_ps[0];
    uint64_t sumsrc = 0, sumref = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            sumsrc += src[y * ss + x];
    if (sumref_ovr >= 0) {
        sumref = (uint64_t)sumref_ovr;
    } else {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                sumref += ref[y * rs + x];
    }
    int64_t N = (int64_t)W * H;
    int scale = 1 << denom;
    int msrc = (int)(sumsrc / N), mref = (int)(sumref / N);
    if (sumref == 0 || abs(msrc - mref) < 2)
        return 0;
    int ww = (int)((sumsrc * scale + sumref / 2) / sumref);
    if (ww < 1) ww = 1; else if (ww > 127) ww = 127;
    int64_t num = sumsrc * scale - sumref * ww;
    int oo = (int)((num + (num >= 0 ? N * scale / 2 : -(N * scale / 2))) / (N * scale));
    if (oo < -128) oo = -128; else if (oo > 127) oo = 127;
    if (ww == scale && oo == 0)
        return 0;
    *w = ww; *o = oo;
    return 1;
}

static uint64_t src_luma_sum(const pixel *src)
{
    uint64_t sum = 0;
    for (int y = 0; y < g_h; y++)
        for (int x = 0; x < g_w; x++)
            sum += src[y * g_ps[0] + x];
    return sum;
}

/* ---- publish / wait ---- */
static void publish(struct slot *s, int v)
{
    pthread_mutex_lock(&s->mx);
    s->pub = v;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mx);
}
static void pub_wait(struct slot *s, int need)
{
    pthread_mutex_lock(&s->mx);
    while (s->pub < need)
        pthread_cond_wait(&s->cv, &s->mx);
    pthread_mutex_unlock(&s->mx);
}

/* Incremental border extension, verbatim from the staircase trailer. */
static void ext_lr(pixel *p, int stride, int w, int b, int y0, int y1)
{
    for (int y = y0; y < y1; y++) {
        pixel *row = p + (size_t)y * stride;
        pixel lval = row[0], rval = row[w - 1];
        for (int x = 0; x < b; x++) { row[-b + x] = lval; row[w + x] = rval; }
    }
}
static void ext_top(pixel *p, int stride, int w, int b)
{
    for (int y = 1; y <= b; y++)
        memcpy(p - (size_t)y * stride - b, p - b, ((size_t)w + 2 * b) * sizeof(pixel));
}
static void ext_bottom(pixel *p, int stride, int w, int h, int b)
{
    for (int y = 1; y <= b; y++)
        memcpy(p + (size_t)(h - 1 + y) * stride - b,
               p + (size_t)(h - 1) * stride - b, ((size_t)w + 2 * b) * sizeof(pixel));
}

/* The per-row consumability pipeline: stair_trailer_task's body (minus the
 * colmv commit -- no B frames, no temporal direct), run INLINE on the frame's
 * own thread. Row j requires analysis rows <= j+1 complete (deblock j+1's top
 * edge rewrites the last rows of j). */
static void row_trail(struct slot *s, int j)
{
    int last = (j == g_hmb - 1);
    y264_frame_t *f = &s->f;
    int chh = g_ph / 2;                     /* 4:2:0 */
    y264_deblock_rows(f, j, j + 1);
    int fin_l = last ? g_ph : 16 * j + 13;
    int fin_c = last ? chh : (j + 1) * 8 - 2;
    ext_lr(f->rec[0], g_ps[0], g_pw, LB, s->ext_l, fin_l);
    for (int c = 1; c < 3; c++)
        ext_lr(f->rec[c], g_ps[c], g_pw / 2, CB, s->ext_c, fin_c);
    s->ext_l = fin_l;
    s->ext_c = fin_c;
    if (j == 0) {
        ext_top(f->rec[0], g_ps[0], g_pw, LB);
        for (int c = 1; c < 3; c++)
            ext_top(f->rec[c], g_ps[c], g_pw / 2, CB);
    }
    if (last) {
        ext_bottom(f->rec[0], g_ps[0], g_pw, g_ph, LB);
        for (int c = 1; c < 3; c++)
            ext_bottom(f->rec[c], g_ps[c], g_pw / 2, chh, CB);
    }
    int hi = last ? g_ph + LB : 16 * j + 10;
    if (hi > s->hp_done) {
        y264_mc_build_hpel_rows(s->hp[0], s->hp[1], s->hp[2], g_ps[0],
                                f->rec[0], g_ps[0], g_pw, g_ph, LB,
                                s->hp_scratch, g_ps[0], s->hp_done, hi);
        s->hp_done = hi;
    }
    publish(s, last ? g_hmb : j);
}

static void trail_to(struct slot *s, int jmax)
{
    if (jmax > g_hmb - 1) jmax = g_hmb - 1;
    for (; s->trail <= jmax; s->trail++)
        row_trail(s, s->trail);
}

/* Consumer+producer hook, fired by the serial analyze loop before the first
 * cell of each MB row: rows < mby of THIS frame are analyzed (trail them), and
 * the reference must have published min(mby+LAG, hmb) rows (stair_row_gate's
 * exact bound). */
static void gate_cb(void *ctx, int mby)
{
    struct slot *s = ctx;
    trail_to(s, mby - 2);
    if (s->prev && g_lag > 0) {
        int need = mby + g_lag;
        if (need > g_hmb) need = g_hmb;
        pub_wait(s->prev, need);
    }
}

/* ---- lifecycle helpers ---- */
static void mark(uint8_t *arr, int i)
{
    pthread_mutex_lock(&g_mx);
    arr[i] = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mx);
}
static void wait_flag(const uint8_t *arr, int i)
{
    pthread_mutex_lock(&g_mx);
    while (!arr[i])
        pthread_cond_wait(&g_cv, &g_mx);
    pthread_mutex_unlock(&g_mx);
}

/* ---- one whole frame, start to finish, on the calling thread ---- */
static void encode_frame(int i)
{
    struct slot *s = &g_slot[i % g_nslots];
    struct slot *prev = i > 0 ? &g_slot[(i - 1) % g_nslots] : NULL;

    /* slot recycle: previous occupant retired AND its reader done reading */
    if (i >= g_nslots) {
        pthread_mutex_lock(&g_mx);
        while (!g_retired[i - g_nslots] || !g_analyzed[i - g_nslots + 1])
            pthread_cond_wait(&g_cv, &g_mx);
        pthread_mutex_unlock(&g_mx);
    }
    s->frame = i;
    s->prev = prev;
    s->pub = 0; s->trail = 0; s->ext_l = 0; s->ext_c = 0; s->hp_done = -LB;

    /* pad input + source DC (the wp substitute the NEXT frame reads) */
    pad_plane(s->src[0], g_ps[0], g_pw, g_ph, g_in[i].y, g_w, g_w, g_h);
    pad_plane(s->src[1], g_ps[1], g_pw / 2, g_ph / 2, g_in[i].u, g_w / 2, g_w / 2, g_h / 2);
    pad_plane(s->src[2], g_ps[2], g_pw / 2, g_ph / 2, g_in[i].v, g_w / 2, g_w / 2, g_h / 2);
    g_srcsum[i] = src_luma_sum(s->src[0]);
    mark(g_summed, i);

    int is_idr = (i == 0);
    int type = is_idr ? 0 : 1;
    int fqp = g_qp + (is_idr ? -3 : 0);
    if (fqp < 0) fqp = 0;
    if (fqp > 51) fqp = 51;
    int fcqp = y264_chroma_qp(fqp, 0);
    int poc = 2 * i;
    int frame_num = i & 15;                     /* log2_max_frame_num_minus4 = 0 */

    /* Clamp off (--lag 0): the reference dependency is the WHOLE frame, so
 * everything that reads it (the wp estimate below, then analyze) waits for
 * the full publish up front. */
    if (prev && g_lag <= 0)
        pub_wait(prev, g_hmb);

    /* ---- slice header (build_slice_prep's exact field sequence) ---- */
    y264_bs_t *bs = &s->bs;
    y264_bs_init(bs, s->rbsp, g_rbsp_cap);
    y264_bs_write_ue(bs, 0);                    /* first_mb_in_slice */
    y264_bs_write_ue(bs, type == 0 ? 7 : 5);    /* slice_type I=7 P=5 */
    y264_bs_write_ue(bs, 0);                    /* pps_id */
    y264_bs_write(bs, 4, frame_num);
    if (is_idr)
        y264_bs_write_ue(bs, 0);                /* idr_pic_id */
    y264_bs_write(bs, 8, poc & 255);            /* pic_order_cnt_lsb */
    int wp_luma[16] = {0}, wp_w[16] = {0}, wp_o[16] = {0};
    if (type == 1) {
        y264_bs_write1(bs, 0);                  /* num_ref_idx_active_override */
        y264_bs_write1(bs, 0);                  /* ref_pic_list_modification_l0 */
        /* pred_weight_table (weighted_pred_flag = 1, like the encoder).
 * Pipelined (clamp on): the reference recon is streaming -- use its
 * source-luma DC, the staircase substitution. Clamp off: recon is
 * complete (we waited above), read it like the serial encoder. */
        y264_bs_write_ue(bs, 5);                /* luma_log2_weight_denom */
        y264_bs_write_ue(bs, 0);                /* chroma_log2_weight_denom */
        wait_flag(g_summed, i - 1);
        int64_t sro = g_lag > 0 ? (int64_t)g_srcsum[i - 1] : -1;
        wp_luma[0] = wp_estimate(s->src[0], prev->rec[0], 5, sro,
                                 &wp_w[0], &wp_o[0]);
        y264_bs_write1(bs, wp_luma[0]);
        if (wp_luma[0]) {
            y264_bs_write_se(bs, wp_w[0]);
            y264_bs_write_se(bs, wp_o[0]);
        }
        y264_bs_write1(bs, 0);                  /* chroma_weight_l0_flag */
    }
    if (is_idr) {                               /* dec_ref_pic_marking */
        y264_bs_write1(bs, 0);
        y264_bs_write1(bs, 0);
    } else {
        y264_bs_write1(bs, 0);                  /* sliding window */
    }
    if (type != 0)
        y264_bs_write_ue(bs, 0);                /* cabac_init_idc */
    y264_bs_write_se(bs, fqp - 26);             /* slice_qp_delta */
    y264_bs_write_ue(bs, 0);                    /* deblock on */
    y264_bs_write_se(bs, 0);
    y264_bs_write_se(bs, 0);

    /* ---- frame context (build_slice_prep's f, IPPP/ref1/CQP subset) ---- */
    y264_frame_t *f = &s->f;
    memset(f, 0, sizeof(*f));
    for (int c = 0; c < 3; c++) {
        f->src[c] = s->src[c];
        f->src_stride[c] = g_ps[c];
        f->rec[c] = s->rec[c];
        f->rec_stride[c] = g_ps[c];
        f->ref[c] = prev ? prev->rec[c] : s->rec[c];
        f->ref_stride[c] = g_ps[c];
        f->ref1[c] = NULL;
        f->ref1_stride[c] = g_ps[c];
        f->refs[0][c] = f->ref[c];
        f->nnz[c] = s->nnz[c];
    }
    f->nnz_stride[0] = g_wmb * 4;
    f->nnz_stride[1] = f->nnz_stride[2] = g_wmb * 2;
    f->nref = 1;
    f->refs_poc[0] = prev ? 2 * (i - 1) : 0;
    f->i4mode = s->i4mode;
    f->i4mode_stride = g_wmb * 4;
    f->cabac = NULL;
    f->mbcbp = s->mbcbp;
    f->mbcbp_stride = g_wmb;
    f->mvx = s->mvx;   f->mvy = s->mvy;   f->refidx = s->refidx;
    f->mvx1 = s->mvx1; f->mvy1 = s->mvy1; f->refidx1 = s->refidx1;
    f->mvdx = s->mvdx; f->mvdy = s->mvdy; f->mvdx1 = s->mvdx1; f->mvdy1 = s->mvdy1;
    f->colmvx = s->colmvx; f->colmvy = s->colmvy; f->colref = s->colref;
    f->colpoc = s->colpoc;
    f->colframepoc = 0;
    f->direct_temporal = 0;
    f->mv_stride = g_mvs;
    f->slice_type = type;
    f->cqm = NULL;
    f->transform8x8 = 1;
    f->weighted_bipred = 0;
    f->poc = poc;
    f->poc_l0 = f->refs_poc[0];
    f->poc_l1 = f->refs_poc[0];
    f->wp_luma[0] = wp_luma[0]; f->wp_w[0] = wp_w[0]; f->wp_o[0] = wp_o[0];
    f->wp_denom = 5;
    f->padded_w = g_pw;
    f->padded_h = g_ph;
    f->wmb = g_wmb;
    f->hmb = g_hmb;
    f->cf_idc = 1; f->sub_w = 2; f->sub_h = 2; f->cbw = 2; f->cbh = 2;
    f->subme = g_subme;
    f->qp = fqp;
    f->chroma_qp = fcqp;
    f->cur_qp = fqp;
    f->cur_chroma_qp = fcqp;
    f->prev_qp = fqp;
    f->last_qp_delta = 0;
    f->te_mbx = -1; f->te_mby = -1;
    f->aq_off = NULL; f->mbtree_off = NULL;
    f->lr_seed_mvx = NULL; f->lr_seed_mvy = NULL; f->lr_seed_cost = NULL;
    f->lr_bseed_mvx0 = f->lr_bseed_mvy0 = NULL;
    f->lr_bseed_mvx1 = f->lr_bseed_mvy1 = NULL;
    f->me_cheap = 0;
    f->mbqp = NULL;
    f->mb_tr8 = s->mb_tr8;
    f->aq_strength = 0.f;
    f->psy_rd = 1.0f;                           /* param default (x264-match) */
    f->psy_trellis = 0.f;
    f->pool = NULL;                             /* serial analyze: the whole point */
    f->row_done = NULL; f->row_done_ctx = NULL;
    f->row_ready = NULL;
    f->row_gate = type == 1 ? gate_cb : NULL;   /* fires per row, serial loop */
    f->row_gate_ctx = s;
    f->stair_clamp = 0;
    f->stair_clamp0_poc[0] = (type == 1 && g_lag > 0) ? f->refs_poc[0] : -1;
    for (int h = 1; h < Y264_STAIR_HOPS; h++) f->stair_clamp0_poc[h] = -1;
    f->stair_mvy_max = g_lag > 0 ? 4 * (16 * g_lag - 24) : 4 * (16 * 4 - 24);

    size_t nmv = (size_t)g_mvs * g_hmb * 4;
    for (size_t k = 0; k < nmv; k++) { s->refidx[k] = -1; s->refidx1[k] = -1; }

    /* half-pel registry: the reference's planes, built by ITS trailer */
    if (type == 1) {
        s->hctx = (y264_hpel_ref_t){ prev->rec[0], prev->hp[0], prev->hp[1], prev->hp[2] };
        y264_me_set_hpel(&s->hctx, 1, g_ps[0]);
        f->hpel_ctx = &s->hctx; f->hpel_n = 1; f->hpel_stride = g_ps[0];
    } else {
        y264_me_set_hpel(NULL, 0, 0);
        f->hpel_ctx = NULL; f->hpel_n = 0; f->hpel_stride = 0;
    }

    /* ---- analyze (+ inline trailer via gate_cb) + emit, CABAC ---- */
    while (y264_bs_pos_bits(bs) & 7)
        y264_bs_write1(bs, 1);                  /* cabac_alignment_one_bit */
    y264_cabac_init_engine(&s->cb, bs->p);
    y264_cabac_init_contexts(&s->cb, type, 0, fqp);
    f->cabac = &s->cb;

    y264_emit_job_t *job = y264_frame_analyze(f);
    mark(g_analyzed, i);                        /* recycle: reader of slot i-1 done */
    trail_to(s, g_hmb - 1);                     /* finish deblock/extend/hpel, publish hmb */
    y264_frame_emit(bs, f, job);
    size_t rbsp_size = (size_t)(s->cb.p - bs->start);

    /* ---- NAL wrap into the per-frame output ---- */
    size_t cap = 5 + rbsp_size + rbsp_size / 2 + 64;
    g_out[i] = malloc(cap);
    g_outlen[i] = y264_nal_write(g_out[i], cap, is_idr ? 3 : 2,
                                 is_idr ? 5 : 1, s->rbsp, rbsp_size);
    mark(g_retired, i);
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        int i = atomic_fetch_add(&g_next, 1);
        if (i >= g_nframes)
            break;
        encode_frame(i);
    }
    return NULL;
}

/* ---- level table, verbatim from encoder.c ---- */
static int level_idc_for(int fs, long mbps, long dpb_mbs)
{
    static const struct { int idc; long max_mbps, max_fs, max_dpb; } L[] = {
        {10,     1485,     99,    396}, {11,     3000,    396,    900},
        {12,     6000,    396,   2376}, {13,    11880,    396,   2376},
        {20,    11880,    396,   2376}, {21,    19800,    792,   4752},
        {22,    20250,   1620,   8100}, {30,    40500,   1620,   8100},
        {31,   108000,   3600,  18000}, {32,   216000,   5120,  20480},
        {40,   245760,   8192,  32768}, {41,   245760,   8192,  32768},
        {42,   522240,   8704,  34816}, {50,   589824,  22080, 110400},
        {51,   983040,  36864, 184320}, {52,  2073600,  36864, 184320},
        {60,  4177920, 139264, 696320}, {61,  8355840, 139264, 696320},
        {62, 16711680, 139264, 696320},
    };
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++)
        if (fs <= L[i].max_fs && mbps <= L[i].max_mbps && dpb_mbs <= L[i].max_dpb)
            return L[i].idc;
    return 62;
}

static int read_line(FILE *fp, char *buf, int cap)
{
    int n = 0, c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') { buf[n] = '\0'; return n; }
        if (n < cap - 1) buf[n++] = (char)c;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL;
    int max_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) in_path = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) g_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lag") && i + 1 < argc) g_lag = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--qp") && i + 1 < argc) g_qp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--subme") && i + 1 < argc) g_subme = atoi(argv[++i]);
        else { fprintf(stderr, "sfpipe: bad arg '%s'\n", argv[i]); return 2; }
    }
    if (!in_path || !out_path) {
        fprintf(stderr, "usage: sfpipe -i in.y4m -o out.264 [-t N] [--lag L] "
                        "[--qp Q] [--frames N]\n"
                        "  --lag 0 = clamp off (full-frame reference dependency)\n");
        return 2;
    }
    if (g_lag > 0 && g_lag < 2) { fprintf(stderr, "sfpipe: lag >= 2 (deblock+hpel margin)\n"); return 2; }

    FILE *fp = fopen(in_path, "rb");
    if (!fp) { fprintf(stderr, "sfpipe: cannot open %s\n", in_path); return 1; }
    char line[512];
    if (read_line(fp, line, sizeof line) < 0 || strncmp(line, "YUV4MPEG2", 9)) {
        fprintf(stderr, "sfpipe: not Y4M\n"); return 1;
    }
    int fpsn = 25, fpsd = 1;
    for (char *tok = strtok(line + 9, " "); tok; tok = strtok(NULL, " ")) {
        if (tok[0] == 'W') g_w = atoi(tok + 1);
        else if (tok[0] == 'H') g_h = atoi(tok + 1);
        else if (tok[0] == 'F') sscanf(tok + 1, "%d:%d", &fpsn, &fpsd);
        else if (tok[0] == 'C' && strncmp(tok + 1, "420", 3)) {
            fprintf(stderr, "sfpipe: 4:2:0 only\n"); return 1;
        }
    }
    if (g_w <= 0 || g_h <= 0) { fprintf(stderr, "sfpipe: bad geometry\n"); return 1; }

    g_wmb = (g_w + 15) / 16; g_hmb = (g_h + 15) / 16;
    g_pw = g_wmb * 16; g_ph = g_hmb * 16;
    g_ps[0] = g_pw + 2 * LB;
    g_ps[1] = g_ps[2] = g_pw / 2 + 2 * CB;
    g_mvs = g_wmb * 4;
    size_t frame_bytes = (size_t)g_ps[0] * (g_ph + 2 * LB)
                       + 2 * (size_t)g_ps[1] * (g_ph / 2 + 2 * CB);
    g_rbsp_cap = frame_bytes + (size_t)g_wmb * g_hmb * 3072 + 8192;

    /* read all frames (like the CLI's threaded reader) */
    size_t ysz = (size_t)g_w * g_h, csz = ysz / 4;
    int cap = 0;
    while (!max_frames || g_nframes < max_frames) {
        if (read_line(fp, line, sizeof line) < 0) break;
        if (strncmp(line, "FRAME", 5)) { fprintf(stderr, "sfpipe: bad FRAME\n"); return 1; }
        if (g_nframes == cap) {
            cap = cap ? cap * 2 : 64;
            g_in = realloc(g_in, (size_t)cap * sizeof(*g_in));
        }
        rawframe_t *r = &g_in[g_nframes];
        r->y = malloc(ysz); r->u = malloc(csz); r->v = malloc(csz);
        if (fread(r->y, 1, ysz, fp) != ysz || fread(r->u, 1, csz, fp) != csz ||
            fread(r->v, 1, csz, fp) != csz) {
            fprintf(stderr, "sfpipe: short read\n"); return 1;
        }
        g_nframes++;
    }
    fclose(fp);
    if (!g_nframes) return 0;

    /* one-time global init (what encoder_open + the CLI prime do) */
    y264_dsp_init();
    y264_cabac_warm();
    y264_cavlc_warm();
    y264_mb_warm_statics();
    y264_me_set_subme(g_subme);
    y264_me_set_method(YAH264_ME_AUTO);   /* not 0 -- 0 is _DIA since the x264 renumbering */
    y264_me_set_subpel(g_subpel);

    if (g_threads < 1) g_threads = 1;
    g_nslots = g_threads + 2;
    if (g_nslots > g_nframes) g_nslots = g_nframes;
    g_slot = calloc((size_t)g_nslots, sizeof(*g_slot));
    size_t nmv = (size_t)g_mvs * g_hmb * 4, nmb = (size_t)g_wmb * g_hmb;
    for (int k = 0; k < g_nslots; k++) {
        struct slot *s = &g_slot[k];
        for (int c = 0; c < 3; c++) {
            int cw = c ? g_pw / 2 : g_pw, ch = c ? g_ph / 2 : g_ph, b = c ? CB : LB;
            s->src[c] = palloc(cw, ch, b);
            s->rec[c] = palloc(cw, ch, b);
        }
        for (int c = 0; c < 3; c++) s->hp[c] = palloc(g_pw, g_ph, LB);
        s->hp_scratch = malloc((size_t)g_ps[0] * (g_ph + 2 * LB + 5) * sizeof(int32_t));
        s->nnz[0] = malloc((size_t)g_wmb * 4 * g_hmb * 4);
        s->nnz[1] = malloc((size_t)g_wmb * 2 * g_hmb * 2);
        s->nnz[2] = malloc((size_t)g_wmb * 2 * g_hmb * 2);
        s->i4mode = malloc((size_t)g_wmb * 4 * g_hmb * 4);
        s->mbcbp = malloc(nmb * sizeof(int));
        s->mvx = malloc(nmv * 2);  s->mvy = malloc(nmv * 2);
        s->mvx1 = malloc(nmv * 2); s->mvy1 = malloc(nmv * 2);
        s->mvdx = malloc(nmv * 2); s->mvdy = malloc(nmv * 2);
        s->mvdx1 = malloc(nmv * 2); s->mvdy1 = malloc(nmv * 2);
        s->refidx = malloc(nmv);   s->refidx1 = malloc(nmv);
        s->colmvx = calloc(nmv, 2); s->colmvy = calloc(nmv, 2);
        s->colref = calloc(nmv, 1); s->colpoc = calloc(nmv, 2);
        s->mb_tr8 = malloc(nmb);
        s->rbsp = malloc(g_rbsp_cap);
        pthread_mutex_init(&s->mx, NULL);
        pthread_cond_init(&s->cv, NULL);
    }
    g_out = calloc((size_t)g_nframes, sizeof(*g_out));
    g_outlen = calloc((size_t)g_nframes, sizeof(*g_outlen));
    g_analyzed = calloc((size_t)g_nframes + 1, 1);
    g_retired = calloc((size_t)g_nframes, 1);
    g_summed = calloc((size_t)g_nframes, 1);
    g_srcsum = calloc((size_t)g_nframes, sizeof(uint64_t));
    g_analyzed[g_nframes] = 1;      /* claim test for the last slots' recycles */

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t *tid = malloc((size_t)g_threads * sizeof(pthread_t));
    for (int t = 0; t < g_threads; t++)
        pthread_create(&tid[t], NULL, worker, NULL);
    for (int t = 0; t < g_threads; t++)
        pthread_join(tid[t], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    /* headers + frames, in order */
    FILE *out = fopen(out_path, "wb");
    if (!out) { fprintf(stderr, "sfpipe: cannot open %s\n", out_path); return 1; }
    {
        y264_sps_t sps; memset(&sps, 0, sizeof sps);
        sps.profile_idc = 100;                  /* transform8x8 -> High */
        sps.chroma_format_idc = 1;
        sps.entropy_coding_mode_flag = 1;
        sps.sps_id = 0;
        sps.log2_max_frame_num_minus4 = 0;
        sps.pic_order_cnt_type = 0;
        sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
        sps.max_num_ref_frames = 1;
        sps.max_num_reorder_frames = 0;
        sps.max_dec_frame_buffering = 1;
        long fps = fpsd > 0 ? (fpsn + fpsd - 1) / fpsd : 25;
        sps.level_idc = level_idc_for(g_wmb * g_hmb, (long)g_wmb * g_hmb * fps,
                                      (long)g_wmb * g_hmb * 2);
        if (fpsn > 0 && fpsd > 0) {
            sps.vui_timing = 1;
            sps.num_units_in_tick = fpsd;
            sps.time_scale = 2 * fpsn;
        }
        sps.width_in_mbs = g_wmb;
        sps.height_in_map_units = g_hmb;
        sps.frame_mbs_only_flag = 1;
        sps.direct_8x8_inference_flag = 1;
        sps.crop_right = (g_pw - g_w) / 2;
        sps.crop_bottom = (g_ph - g_h) / 2;
        y264_pps_t pps; memset(&pps, 0, sizeof pps);
        pps.pps_id = 0; pps.sps_id = 0;
        pps.entropy_coding_mode_flag = 1;
        pps.weighted_pred_flag = 1;
        pps.weighted_bipred_idc = 0;
        pps.deblocking_filter_control_present_flag = 1;
        pps.transform_8x8_mode_flag = 1;
        uint8_t hdr_rbsp[512], hdr_nal[1024];
        y264_bs_t hbs;
        y264_bs_init(&hbs, hdr_rbsp, sizeof hdr_rbsp);
        y264_sps_write(&hbs, &sps);
        size_t n = y264_nal_write(hdr_nal, sizeof hdr_nal, 3, 7, hdr_rbsp,
                                  (size_t)(hbs.p - hbs.start));
        fwrite(hdr_nal, 1, n, out);
        y264_bs_init(&hbs, hdr_rbsp, sizeof hdr_rbsp);
        y264_pps_write(&hbs, &pps);
        n = y264_nal_write(hdr_nal, sizeof hdr_nal, 3, 8, hdr_rbsp,
                           (size_t)(hbs.p - hbs.start));
        fwrite(hdr_nal, 1, n, out);
    }
    for (int i = 0; i < g_nframes; i++)
        fwrite(g_out[i], 1, g_outlen[i], out);
    fclose(out);
    fprintf(stderr, "sfpipe: %d frames, %d threads, %d slots, lag %d: %.0f ms "
            "(%.1f fps)\n", g_nframes, g_threads, g_nslots, g_lag, ms,
            g_nframes * 1000.0 / ms);
    return 0;
}
