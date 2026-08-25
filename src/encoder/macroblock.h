/*
 * macroblock.h - closed-loop intra macroblock coding
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef NEXT264_MACROBLOCK_H
#define NEXT264_MACROBLOCK_H

#include "../common/bitstream.h"
#include "cabac.h"
#include "../dsp/transform.h"
#include <stdint.h>

/* How many still-streaming references one slice's list-0 clamp can name (see
 * stair_clamp0_poc). The burst ring drains before it ever holds more than K
 * bursts, so at the moment an anchor preps at most K-1 of its predecessors are
 * still live, and that bound is a property of the ring, not of --ref.
 *
 * Plus one for the IMMEDIATE predecessor's REFERENCE B, which a deep list 0
 * reaches ahead of both anchors (it is coded after its anchor, so it outranks
 * them on FrameNum). Only the immediate predecessor's, not every live burst's --
 * see the row-gate reasoning in encoder.c's stair_refbgate_on.
 *
 * The set is therefore not "newest-first" in any meaningful sense -- a
 * reference B is NEWER than its own anchor -- and it is PACKED, which is the
 * only property the membership scan's early exit depends on.
 *
 * Deliberately declared here rather than pulled from encoder.h -- macroblock.h
 * is the encoder-facing side of the frame contract and does not include the
 * encoder's private header. */
#define N264_STAIR_HOPS 3

/* Per-frame coding context. The encoder fills this, then calls n264_frame_encode
 * which writes the slice data and leaves the reconstructed frame in rec[]. */
typedef struct {
    const pixel   *src[3];      /* MB-aligned source planes (Y, Cb, Cr) */
    int            src_stride[3];
    pixel         *rec[3];      /* reconstructed planes, same geometry */
    int            rec_stride[3];
    const pixel   *ref[3];      /* list-0 reference (past anchor) = refs[0] */
    int            ref_stride[3];
    const pixel   *ref1[3];     /* list-1 reference (future anchor), B slices only */
    int            ref1_stride[3];
    /* Multi-reference list 0, most-recent-first; all share ref_stride. nref is
 * num_ref_idx_l0_active for this slice (>= 1). refs[0] == ref[]. For B
 * slices the list holds past references only (list 1 stays single-ref).
 * refs_poc gives each entry's POC, for implicit biprediction weights. */
    const pixel   *refs[16][3];
    int            refs_poc[16];
    int            nref;

    int slice_type;             /* 0 = I slice, 1 = P slice, 2 = B slice */
    int transform8x8;           /* PPS transform_8x8_mode_flag (I_8x8 allowed) */
    int weighted_bipred;        /* 1 = implicit weighted biprediction (idc 2) */
    int poc, poc_l0, poc_l1;    /* current / list-0-ref / list-1-ref POC, for WP */
    int wp_luma[16];            /* P slice: explicit luma weight active, per ref */
    int wp_w[16], wp_o[16];     /* per-ref luma weight and offset */
    int wp_denom;               /* shared luma log2 weight denom */
    int padded_w, padded_h;     /* reference picture bounds for MV clamping */

    /* Per-4x4 motion fields for MV prediction. refidx == -1 marks intra/unused.
 * The unsuffixed field is list 0; the *1 field is list 1 (B slices). */
    int16_t *mvx, *mvy;
    int8_t  *refidx;
    int16_t *mvx1, *mvy1;
    int8_t  *refidx1;
    /* Per-4x4 absolute mvd components (clamped), for CABAC mvd neighbour context.
 * The *1 fields are list 1. Cleared per frame; intra/skip blocks stay 0. */
    int16_t *mvdx, *mvdy, *mvdx1, *mvdy1;
    /* Co-located motion of the list-1 anchor (its list-0 field), for B direct.
 * colpoc holds each block's referenced-picture POC; direct_temporal selects
 * the slice's derivation (8.4.1.2.3 vs spatial 8.4.1.2.2). */
    int16_t *colmvx, *colmvy;
    int8_t  *colref;
    int16_t *colpoc;
    int      colframepoc;       /* POC of the frame the colmv field came from */
    int      direct_temporal;
    int      mv_stride;

    /* Per-4x4-block non-zero-coefficient counts, for CAVLC nC context.
 * nnz[0] is luma (wmb*4 x hmb*4), nnz[1]/nnz[2] chroma (wmb*2 x hmb*2).
 * Cells are initialised to -1 meaning "outside the frame / unavailable". */
    int8_t *nnz[3];
    int     nnz_stride[3];

    /* Intra4x4 prediction mode of each luma 4x4 block (wmb*4 x hmb*4), used to
 * predict the mode of later blocks. I_16x16 macroblocks store DC (2). */
    int8_t *i4mode;
    int     i4mode_stride;

    int wmb, hmb;               /* frame size in macroblocks */
    /* Chroma format: cf_idc is chroma_format_idc (1/2/3); sub_w/sub_h are
 * SubWidthC/SubHeightC. Chroma MB is (16/sub_w) x (16/sub_h) samples =
 * cbw x cbh 4x4 blocks per component (cbw = 4/sub_w, cbh = 4/sub_h). */
    int cf_idc, sub_w, sub_h;
    int cbw, cbh;               /* chroma 4x4 blocks per MB, per axis */
    int subme;                  /* analysis level (x264-style); <=8 = fast paths */
    int slice_is_ref;           /* this picture is a reference (nal_ref_idc>0) */
    int mbt_frac;               /* mbtree_off is in HALF-QP units (N264_MBT_FRAC) */
    int trellis;                /* 0 = deadzone only (no RDOQ anywhere), 1 = at
 * commit (x264's placement, our default),
 * 2 = in every RD trial. */
    /* Early-skip probe acceptance (see probe_skip). Resolved from the env once
 * in encoder_open and copied per frame, so worker threads only ever read
 * it -- a lazy static here would join the warm_lr_statics race class.
 * skipdec_p/skipdec_b: 0 = off (probe fails on any surviving coefficient),
 * 1 = coder-consistent (accept blocks our own decimator would zero),
 * 2 = x264 MB-wide accumulation, 3 = either. skipdec_t = the mode-2
 * threshold (x264 uses 6). */
    int skipdec_p, skipdec_b, skipdec_t;
    /* Qpel L1 tolerance for the agreement guard that PAYS for that tolerance:
 * accept a decimation-tolerant skip only where the lookahead's own motion
 * estimate lands on the skip/direct MV. 0 = no guard. x264 uses <= 1
 * against a full-resolution 16x16 ME result; ours compares against the
 * lowres lookahead MV, which is coarser, so the tolerance is a knob. */
    int skip_mvagree_p, skip_mvagree_b;
    /* B only: refuse the tolerance when the direct prediction's SSD exceeds
 * this multiple of the RD lambda. 0 = no gate. */
    int skip_costgate;
    /* B only: qpel tolerance for the POST-SEARCH confirmation (x264's actual
 * structure -- the tolerant probe's answer is deferred until real 16x16 ME
 * on list0 ref0 and list1 ref0 has confirmed the direct MV). 0 = off.
 * bskip_dec = the acceptance mode that deferred probe runs. Unlike
 * skip_mvagree_b this compares against a SEARCH result, not the lookahead. */
    int bskip_confirm, bskip_dec, bskip_probe;
    int bskip_notrellis;        /* cost probe: skip the trellis in the deferred B probe */
    /* PRE-ME admission. Decides who
 * pays the speculative probe, and admits NOTHING to the skip itself, so a
 * false positive costs time only. bskip_admit is a qpel tolerance on the
 * lowres pair MVs against the direct MVs; 0 = off (probe everyone, the
 * measured 1.6-3.3%-of-wall defect). bskip_cguard is the guard mask:
 * bit0 direct SATD-competitive with the ref-0 searches, bit1 the skip's own
 * distortion is cheap in lambda units, bit2 the ref-B propagation guard. */
    int bskip_admit, bskip_cguard;
    int skor_key;               /* absolute display index; skip-oracle key only */
    int qp;                     /* frame base luma QP */
    int chroma_qp;              /* derived chroma QP for the base QP */
    int cur_qp;                 /* current MB luma QP (= qp unless AQ varies it) */
    int cur_chroma_qp;          /* current MB chroma QP */
    /* Quantiser-scaling QPs: the signaled QP plus QpBdOffset (= QP + 6*(BD-8)),
 * i.e. QP'Y / QP'C. Equal to cur_qp / cur_chroma_qp at 8-bit. The transform
 * quant/dequant kernels key their scaling off these; signaling (mb_qp_delta,
 * deblock) uses the un-offset cur_qp / cur_chroma_qp. */
    int cur_qp_scaled;
    int cur_chroma_qp_scaled;
    int8_t *aq_off;             /* per-MB luma QP offset from AQ (NULL = none) */
    int8_t *mbtree_off;         /* per-MB luma QP offset from mb-tree (NULL = none) */
    /* Per-MB lookahead (lowres, vs this frame's ref0/anchor) MV, quarter-pel, as
 * an integer-search seed. NULL when no lookahead ran. Indexed mby*wmb+mbx.
 * P-frame ref0 only: the current-frame motion x264 seeds from lowres_mvs. */
    int16_t *lr_seed_mvx, *lr_seed_mvy;
    int32_t *lr_seed_cost;      /* per-MB lowres inter SATD, the ME-gate oracle cost */
    /* B frames: lowres pair-MV seeds (fullres qpel, POC-scaled to this B's
 * actual list-0/list-1 refs) -- x264 seeds B ME from lowres_mvs[list][dist]
 * the same way (<reference-source> ref16x16). NULL when absent. */
    int16_t *lr_bseed_mvx0, *lr_bseed_mvy0, *lr_bseed_mvx1, *lr_bseed_mvy1;
    /* Measurement only (N264_BLATE_STAT): the pair legs' lowres costs (l0 / l1
 * d_inter, own d_intra), unscaled lowres SATD units. NULL when absent. */
    int32_t *lr_bseed_c0, *lr_bseed_c1, *lr_bseed_ci;
    int me_cheap;               /* content-adaptive ME: 1 = low-motion frame, run
 * cheap searches (no UMH, capped subpel) */
    uint8_t *mbqp;              /* per-MB coded luma QP, for the deblock pass */
    uint8_t *mb_tr8;            /* per-MB: 1 if the 8x8 luma transform was used */
    float aq_strength;          /* variance-AQ strength (0 = off) */
    /* The AQ field's derivation parameters, mirrored from the encoder so the
 * standalone AQ this frame codes (non-reference B, or any frame when
 * mb-tree is off) is the SAME field mbtree_invqscale folds into the mb-tree
 * offset. Only the x264 mode (n264_mbt_derived) reads them; the shipped
 * default keeps aq_analyze's own autovariance derivation. */
    int aq_abs;                 /* offset against aq_anchor, not the frame mean */
    int aq_chroma;              /* energy sums every plane */
    float aq_anchor;            /* the absolute anchor, in log2(energy)-8 units */
    float psy_rd;               /* psy-RD strength (0 = off, SSD-only) */
    int stq;                    /* single-thread quality mode: at wf_width==1 the
 * speed trades (ME_ET family, PART early-term)
 * disengage -- single-thread has the speed margin
 * and needs the quality. Thread-variant output by
 * policy; t2+ byte-identical. */
    int   psy_lattice;          /* psy came from a CLASS GATE: run it inside the
 * Viterbi lattice (the measured-cheap form that
 * keeps the flat class); manual --psy-trellis /
 * --tune grain keep the greedy search whose
 * textured-class wins the lattice loses. */
    float psy_trellis;          /* psy-trellis strength (0 = off): reward AC-energy
 * retention in the RDOQ quant search (grain/detail) */
    int prev_qp;                /* QPY carried by the mb_qp_delta prediction chain */
    int last_qp_delta;          /* previous MB's mb_qp_delta (for CABAC context) */
    int qpd_coded;              /* set when the current MB coded an mb_qp_delta */
    /* Per-MB memo of the src-side psy texture energy (invariant across a MB's
 * RD candidates); keyed on (te_mbx,te_mby), -1 = unset. Values are a pure
 * function of src(mbx,mby), so the memo is byte-identical. */
    long te_src4, te_src8;
    int te_mbx, te_mby;

    /* CABAC: engine (NULL for CAVLC) and a per-MB cbp cache for context
 * derivation. mbcbp packs luma 8x8 cbp (bits 0-3), chroma cbp (bits 4-5),
 * luma-DC cbf (bit 8), chroma-DC cbf (bits 9-10); -1 means unavailable. */
    n264_cabac_t *cabac;
    int          *mbcbp;
    int           mbcbp_stride;

    /* Custom quantisation matrices (scaling lists), or NULL for the flat
 * default. When NULL the quant/dequant kernels take their fast/NEON path
 * and output is byte-identical to a build without CQM. */
    const n264_cqm_t *cqm;

    /* In-frame row-wavefront pool (ntp_pool_t*), or NULL for serial. When set
 * and >1 thread, the pass-1 analysis loop runs on it; NULL = serial (default,
 * byte-identical). Kept as void* to avoid coupling this header to threadpool.h. */
    void *pool;
    /* This slice's ME half-pel context (const n264_hpel_ref_t*), so a wavefront
 * worker can install it (n264_me_set_hpel is thread-local) before motion
 * search. void* to avoid coupling this header to me.h. */
    const void *hpel_ctx;
    int   hpel_n;
    int   hpel_stride;

    /* Staircase. Producer side:
 * row_done(ctx, mby) fires on the wavefront worker that completes the LAST
 * cell of each MB row (rows complete in increasing order -- the top-right
 * dependency makes a row's last cell wait for the full row above), feeding
 * the trailing per-row consumability pipeline (deblock/borders/hpel/colmv).
 * Consumer side: row_gate(ctx, mby) fires before the FIRST cell of each MB
 * row, so a B frame can block until the in-flight anchor has published
 * enough consumable rows. Both NULL by default (no cost beyond one branch).
 * stair_clamp: clamp this slice's list-1 vertical MVs to the fixed
 * staircase bound (a pure function of the env gate + frame structure, never
 * of thread count -- the repo's determinism invariant). */
    void (*row_done)(void *ctx, int mby);
    void  *row_done_ctx;
    void (*row_gate)(void *ctx, int mby);
    void  *row_gate_ctx;
    /* Non-blocking twin of row_gate for the multi-frame pool: "may row mby
 * start now?". Shares row_gate_ctx. When set, the analyze wavefront runs
 * gated (ntp_wavefront_gated) so a not-yet-ready row is never CLAIMED and
 * its worker serves another in-flight frame instead of blocking inside the
 * cell; row_gate stays as the blocking form for the serial fallback. Must
 * be monotonic per row (an atomic watermark read). */
    int  (*row_ready)(void *ctx, int mby);
    int    stair_clamp;
    /* Clamp this slice's LIST-0 searches against the references whose
 * POC is in this SET (the possibly-in-flight recent anchors). A set rather
 * than one POC because width (N264_STAIR_WIDE) can have several anchors
 * streaming at once, and at --ref > 1 more than one of them can be in the
 * same list 0. PACKED and newest-first: slot h+1 is populated only if slot h
 * is, so the membership test stops at the first -1 and an unpopulated set
 * costs exactly the one compare the scalar this replaced cost.
 * Like stair_clamp, a pure function of the env gates + frame structure. */
    int    stair_clamp0_poc[N264_STAIR_HOPS];
    /* Thread-scaled clamp: the vertical qpel reach every site above
 * applies once stair_clamp / a stair_clamp0_poc hit fires. Mirrors
 * the encoder's e->stair_mvy_max, resolved once at open by
 * stair_lag_for as a function of frame height and pool width, never
 * below N264_STAIR_MVY_MAX (me.h). A pure function of encoder config
 * + thread count, so fixed for one open -- same config and thread
 * count reproduce the same clamp and the same bitstream. */
    int    stair_mvy_max;
} n264_frame_t;

/* Encode all macroblocks of the frame as intra (I_16x16 luma + intra chroma),
 * writing the slice_data to `bs` and filling rec[]. Thin wrapper over the
 * analyze/emit split below (kept for callers that don't overlap the two). */
void n264_frame_encode(n264_bs_t *bs, n264_frame_t *f);

/* Emit-overlap: n264_frame_encode split into two halves so the entropy emit
 * of frame N can run (on a background thread) concurrent with frame N+1's
 * analyze. n264_frame_analyze runs passes 1+1b (mode decision + reconstruction
 * + decision grids + the raster QPY chain), leaving rec[] ready to serve as a
 * reference, and returns a heap-allocated job describing what pass 2 must emit.
 * n264_frame_emit runs pass 2 (the bitstream) from that job and frees it. The
 * job owns the malloc'd records array; the caller must pass every job returned
 * by analyze to exactly one emit. */
typedef struct n264_emit_job n264_emit_job_t;
n264_emit_job_t *n264_frame_analyze(n264_frame_t *f);
void             n264_frame_emit(n264_bs_t *bs, n264_frame_t *f, n264_emit_job_t *job);

/* Resolve env-gated analyze lazy statics on the main thread before workers run
 * (called from next264_encoder_open); keeps the analyze wavefront TSan-clean. */
void             n264_mb_warm_statics(void);

/* N264_MBT_DERIVED: the whole-system x264 mb-tree mode. One gate for the whole
 * jointly-adapted set of constants and compositions that separate our mb-tree
 * from x264's -- the field's derivation AND its consumption -- because every
 * axis-aligned half of it is measured-refused. Lives here rather than in
 * encoder.c because aq_analyze needs it too. */
int              n264_mbt_derived(void);

#endif /* NEXT264_MACROBLOCK_H */
