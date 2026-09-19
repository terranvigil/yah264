/*
 * macroblock.h - closed-loop intra macroblock coding
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef YAH264_MACROBLOCK_H
#define YAH264_MACROBLOCK_H

#include "yah264.h"          /* YAH264_PART_*: the frame carries a resolved mask */
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
#define Y264_STAIR_HOPS 3

/* Per-frame coding context. The encoder fills this, then calls y264_frame_encode
 * which writes the slice data and leaves the reconstructed frame in rec[]. */
/* colpoc flag: the colocated block predicted from list 1 only, so the stored
 * POC is its list-1 reference. Readers mask with Y264_COLPOC_MASK; POCs stay
 * under 16384 (keyint-bounded). */
#define Y264_COLPOC_L1   0x4000
#define Y264_COLPOC_MASK 0x3fff

/* Row-band summary of the lookahead fields, built once per frame in
 * y264_frame_analyze and read-only for the whole macroblock loop.
 *
 * Every gate in the tournament that already consults coarser-than-macroblock
 * data re-derives it per macroblock, and none of the fields it reads changes
 * during the frame. A band verdict is therefore a property of the CONTENT, not
 * of lambda, which is the axis the per-block bounds of the two previous stages
 * failed on: a lambda-scaled bound is most generous exactly at the low rates
 * where a wrong verdict costs most.
 *
 * Only frame-constant inputs go in here. The previous band's skip fraction was
 * in the design and is NOT: under the wavefront the row above runs two
 * macroblocks ahead, so the band above is incomplete when the next band starts
 * and its counter would read differently at each thread count. */
typedef struct {
    int32_t lr_mean;      /* band mean of lr_seed_cost (lowres inter), P frames */
    int32_t lr_cov2;      /* population CoV^2 * 100 of lr_seed_cost over the band */
    int32_t cp_mean;      /* band mean of the pair legs' lowres inter cost, B frames */
    int32_t cp_cov2;      /* population CoV^2 * 100 of the same */
    int32_t ci_mean;      /* band mean of the pair legs' own lowres intra cost */
    int32_t prop_frac;    /* x256 share of the band whose mb-tree offset is negative */
    int32_t n;            /* macroblocks summarised (0 = nothing populated it) */
} y264_band_t;

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
    /* PAFF. field_pic = 1 means this picture is ONE FIELD: src/rec/ref point at
 * that parity's first row and every stride is doubled, so the coder sees an
 * ordinary picture of half the height and is told nothing else about the
 * other field. What it does have to know is where the standard makes a
 * field picture differ from a frame one: residual blocks are written in the
 * field scan out of the field half of the CABAC context set, and the
 * deblocking filter drops intra HORIZONTAL macroblock edges from strength 4
 * to 3 and halves the vertical motion threshold. The halved vertical motion
 * range arrives through mv_ylim_q like any other level bound.
 * field_parity is 0 for a top field, 1 for a bottom field. */
    int field_pic, field_parity;
    /* 8.4.1.4, and the only thing a reference field of the OTHER parity costs
 * this coder. In 4:2:0 the two parities' chroma rows sit a quarter of a
 * chroma sample apart inside their own field grids, so a prediction that
 * crosses parity has to move chroma by that quarter where luma moves by
 * nothing. The correction is a constant per (this parity, that reference's
 * parity) pair -- 2 * (this - that), in the eighth-chroma units
 * y264_mc_chroma reads the vertical component in -- so it is resolved once
 * per slice onto the reference view and every motion-compensation site adds
 * it. Zero for every frame picture and every same-parity field, which is
 * what leaves progressive output untouched. 4:4:4 predicts chroma with the
 * luma filter and never reads these; field coding is 4:2:0 only. */
    int8_t cmv_l0[16];
    int8_t cmv_l1;
    int transform8x8;           /* PPS transform_8x8_mode_flag (I_8x8 allowed) */
    int weighted_bipred;        /* 1 = implicit weighted biprediction (idc 2) */
    /* PPS constrained_intra_pred_flag, and only where it can bite: an inter
 * neighbour then supplies neither reference samples nor an intra mode
 * predictor. 0 in I slices, where every neighbour is intra already. */
    int constrained_intra;
    int poc, poc_l0, poc_l1;    /* current / list-0-ref / list-1-ref POC, for WP */
    int wp_luma[16];            /* P slice: explicit luma weight active, per ref */
    int wp_w[16], wp_o[16];     /* per-ref luma weight and offset */
    int wp_denom;               /* shared luma log2 weight denom */
    /* --weightp 2: the duplicate list-0 slot and the slot it duplicates, or -1
 * and -1 where this slice carries none. The two name the SAME picture, so
 * the motion search cannot tell them apart -- it would score both the same
 * and the ref-index bits would then settle it against the duplicate every
 * time. The search skips the duplicate outright and the choice is made once
 * the motion is settled, on the two predictions themselves (wp_pick). */
    int wp_dup, wp_dup_of;
    int8_t wp_refmap[16];       /* refIdx -> reference picture, for 8.7.2.1 */
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
    /* Y264_DIRECT_SCORE=2 only: whether the OTHER direct mode is legal for this
 * slice, so the scorer can derive it safely. Temporal needs every co-located
 * reference resolvable in list 0; spatial always is. */
    int      direct_alt_ok;
    int      direct_auto;      /* Y264_DIRECT_AUTO armed and honoured for this slice */
    long    *dauto_acc;        /* where this frame's skippability counts go: the
                                * serial path's pending pair, or the owning
                                * stair burst's (summed at its drain). Atomic adds:
                                * the wavefront rows share it */
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
    /* Slices. The picture is cut into `nslices` independent slices on MB-row
 * boundaries: slice s owns rows [slice_row0[s], slice_row0[s+1]), so
 * slice_row0 holds nslices+1 entries and the last one is hmb. slice_y0[mby]
 * is the first row of the slice CONTAINING row mby, which is the form every
 * neighbour test wants. nslices 1 with both pointers NULL is one slice per
 * picture: mb_ytop stays 0 and every test below reads exactly as the plain
 * frame-edge test it replaced.
 *
 * mb_ytop is the per-macroblock mirror of slice_y0: the first MB row this
 * macroblock's slice owns. A neighbour is available when it is inside the
 * frame AND at or below mb_ytop -- slices are row-contiguous and the only
 * rows a macroblock reads are its own and the one above, so that one bound
 * is the whole of "outside the slice". mb_qp_pre publishes it, and resets
 * the mb_qp_delta prediction chain at each slice start, so every analyze
 * and emit path picks it up at the one call they all already make. Mutable
 * per MB like cur_qp: the wavefront hands each worker its own frame copy. */
    int nslices;
    const int     *slice_row0;
    const int16_t *slice_y0;
    int mb_ytop;
    /* Chroma format: cf_idc is chroma_format_idc (1/2/3); sub_w/sub_h are
 * SubWidthC/SubHeightC. Chroma MB is (16/sub_w) x (16/sub_h) samples =
 * cbw x cbh 4x4 blocks per component (cbw = 4/sub_w, cbh = 4/sub_h). */
    int cf_idc, sub_w, sub_h;
    int cbw, cbh;               /* chroma 4x4 blocks per MB, per axis */
    int subme;                  /* analysis level; <=8 = fast paths */
    int partitions;             /* YAH264_PART_* mask, resolved at encoder_open:
 * which partition shapes this picture's mode decision
 * may try. Read per macroblock, never re-derived. */
    int slice_is_ref;           /* this picture is a reference (nal_ref_idc>0) */
    int mbt_frac;               /* mbtree_off is in HALF-QP units (Y264_MBT_FRAC) */
    int trellis;                /* 0 = deadzone only (no RDOQ anywhere), 1 = at
 * commit (our default),
 * 2 = in every RD trial. */
    /* Early-skip probe acceptance (see probe_skip). Resolved from the env once
 * in encoder_open and copied per frame, so worker threads only ever read
 * it -- a lazy static here would join the warm_lr_statics race class.
 * skipdec_p/skipdec_b: 0 = off (probe fails on any surviving coefficient),
 * 1 = coder-consistent (accept blocks our own decimator would zero),
 * 2 = MB-wide accumulation, 3 = either. skipdec_t = the mode-2
 * threshold (the reference encoder uses 6). */
    int skipdec_p, skipdec_b, skipdec_t;
    /* Qpel L1 tolerance for the agreement guard that PAYS for that tolerance:
 * accept a decimation-tolerant skip only where the lookahead's own motion
 * estimate lands on the skip/direct MV. 0 = no guard. A full-resolution
 * 16x16 ME result would want <= 1; ours compares against the lowres
 * lookahead MV, which is coarser, so the tolerance is a knob. */
    int skip_mvagree_p, skip_mvagree_b;
    /* B only: refuse the tolerance when the direct prediction's SSD exceeds
 * this multiple of the RD lambda. 0 = no gate. */
    int skip_costgate;
    /* B only: qpel tolerance for the POST-SEARCH confirmation: the tolerant
 * probe's answer is deferred until real 16x16 ME on list0 ref0 and list1
 * ref0 has confirmed the direct MV. 0 = off.
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
 * distortion is cheap in lambda units, bit2 the ref-B propagation guard.
 * docs/b-skip-decision-design.md. */
    int bskip_admit, bskip_cguard;
    /* HD parity stage 2, candidate 1: the PRE-ME B skip verdict, taken on the
 * RD floor rather than on a motion signal. A coded macroblock has to pay at
 * least its mb_type and cbp syntax, so no coded candidate can score below
 * lambda times that minimum rate; where the skip candidate's own distortion
 * already sits under that floor, the tournament's answer is settled and
 * every search after this point is spent confirming it. 0 = off, 1 =
 * non-reference B slices only, 2 = also reference B's the propagation guard
 * admits. b_preme_bits is the floor, in bits. */
    int b_preme_skip, b_preme_bits;
    /* HD parity stage 2, candidate 2: refuse the P sub-partition searches when
 * the 16x16 result is already this cheap in lambda units AND the lookahead
 * neighbourhood says the block is homogeneous and not a propagation source.
 * 0 = off. */
    int p_part_gate;
    /* HD parity stage 2, candidate 3: RD at most this many SATD survivors per
 * candidate set in the B tournament, by rank instead of by score. 0 = off,
 * i.e. every candidate inside the score threshold is RD'd. */
    int rd_surv_rank;
    /* Band-level decisions, stage 4. band_c is the frame's [3*band] lookahead
 * aggregate array -- mean pair-leg cost, its CoV^2 x100, mean lowres intra
 * cost -- summarised at stash time and carried per buffered B. NULL when the
 * lookahead did not populate it, and then the band table's B columns stay
 * zero and every band rule below is inert. */
    const int32_t *band_c;
    /* The margin, in 16ths, by which a row band's lookahead intra cost has to
 * exceed its inter cost before the B intra SATD screen and the intra trial
 * are both skipped in that band. 0 = off. */
    int b_intra_band;
    int skor_key;               /* absolute display index; skip-oracle key only */
    int qp;                     /* frame base luma QP */
    int chroma_qp;              /* derived chroma QP for the base QP */
    /* PPS chroma_qp_index_offset, folded into every luma->chroma QP mapping
 * this frame makes (the quantiser's and the deblock filter's). 0 = the
 * default, where every mapping is the plain 8.5.8 table. */
    int chroma_qp_off;
    /* In-loop deblocking for this frame: `deblock_on` 0 writes
 * disable_deblocking_filter_idc 1 and runs no filter at all;
 * deblock_a / deblock_b are slice_alpha_c0_offset_div2 and
 * slice_beta_offset_div2 (-6..6), added DOUBLED to the edge QP before the
 * threshold lookup, as 8.7.2.2 requires. Both 0 = the shipped filter. */
    int deblock_on, deblock_a, deblock_b;
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
    /* B temporal-cascade share of this frame's QP (frame_qp's casc term, after
 * Y264_CRF_PBSCALE). 0 on anchors and wherever the cascade did not apply.
 * Read only by Y264_MB_LAMBDA=7: decide non-ref-B modes/motion at the
 * anchor-grade lambda (cur_qp - lambda_casc) while quantising at cur_qp. */
    int lambda_casc;
    /* Per-MB lookahead (lowres, vs this frame's ref0/anchor) MV, quarter-pel, as
 * an integer-search seed. NULL when no lookahead ran. Indexed mby*wmb+mbx.
 * P-frame ref0 only: the current-frame motion seeded from the lowres MVs. */
    int16_t *lr_seed_mvx, *lr_seed_mvy;
    int32_t *lr_seed_cost;      /* per-MB lowres inter SATD, the ME-gate oracle cost */
    /* B frames: lowres pair-MV seeds (fullres qpel, POC-scaled to this B's
 * actual list-0/list-1 refs), fed into the 16x16 predictor list the same
 * way the P seeds are. NULL when absent. */
    int16_t *lr_bseed_mvx0, *lr_bseed_mvy0, *lr_bseed_mvx1, *lr_bseed_mvy1;
    /* Measurement only (Y264_BLATE_STAT): the pair legs' lowres costs (l0 / l1
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
 * offset. Only the derived mb-tree mode (y264_mbt_derived) reads them; the shipped
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
    y264_cabac_t *cabac;
    int          *mbcbp;
    int           mbcbp_stride;

    /* Custom quantisation matrices (scaling lists), or NULL for the flat
 * default. When NULL the quant/dequant kernels take their fast/NEON path
 * and output is byte-identical to a build without CQM. */
    const y264_cqm_t *cqm;

    /* In-frame row-wavefront pool (ntp_pool_t*), or NULL for serial. When set
 * and >1 thread, the pass-1 analysis loop runs on it; NULL = serial (default,
 * byte-identical). Kept as void* to avoid coupling this header to threadpool.h. */
    void *pool;
    /* This slice's ME half-pel context (const y264_hpel_ref_t*), so a wavefront
 * worker can install it (y264_me_set_hpel is thread-local) before motion
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
 * than one POC because width (Y264_STAIR_WIDE) can have several anchors
 * streaming at once, and at --ref > 1 more than one of them can be in the
 * same list 0. PACKED and newest-first: slot h+1 is populated only if slot h
 * is, so the membership test stops at the first -1 and an unpopulated set
 * costs exactly the one compare the scalar this replaced cost.
 * Like stair_clamp, a pure function of the env gates + frame structure. */
    int    stair_clamp0_poc[Y264_STAIR_HOPS];
    /* Thread-scaled clamp: the vertical qpel reach every site above
 * applies once stair_clamp / a stair_clamp0_poc hit fires. Mirrors
 * the encoder's e->stair_mvy_max, resolved once at open by
 * stair_lag_for as a function of frame height and pool width, never
 * below Y264_STAIR_MVY_MAX (me.h). A pure function of encoder config
 * + thread count, so fixed for one open -- same config and thread
 * count reproduce the same clamp and the same bitstream. */
    int    stair_mvy_max;
    int    mv_xlim_q, mv_ylim_q; /* the level's MV range in qpel (|mv| < lim), from the SPS level */
    /* Band-level decisions in the tournament. Both buffers are owned by
 * y264_frame_analyze for the length of one analyze call and are NULL
 * outside it; nothing in the emit half reads them. */
    const y264_band_t *bands;   /* [nbands], NULL = no table built */
    int     nbands;
    int     band_rows;          /* macroblock rows per band (Y264_BAND_ROWS) */
    /* Per-macroblock precompute of the P sub-partition gate's two interlocks,
 * one byte per macroblock: bit0 = the 3x3 lowres-cost neighbourhood is
 * dispersed, bit1 = the mb-tree offset says other frames read this one.
 * Both are pure functions of frame-constant lookahead fields, so this is
 * work elimination and not a decision: the gate's verdict is unchanged. */
    const uint8_t *gate_bits;   /* [wmb*hmb], NULL = compute per macroblock */
} y264_frame_t;

/* Encode all macroblocks of the frame as intra (I_16x16 luma + intra chroma),
 * writing the slice_data to `bs` and filling rec[]. Thin wrapper over the
 * analyze/emit split below (kept for callers that don't overlap the two). */
void y264_frame_encode(y264_bs_t *bs, y264_frame_t *f);

/* Emit-overlap: y264_frame_encode split into two halves so the entropy emit
 * of frame N can run (on a background thread) concurrent with frame N+1's
 * analyze. y264_frame_analyze runs passes 1+1b (mode decision + reconstruction
 * + decision grids + the raster QPY chain), leaving rec[] ready to serve as a
 * reference, and returns a heap-allocated job describing what pass 2 must emit.
 * y264_frame_emit runs pass 2 (the bitstream) from that job and frees it. The
 * job owns the malloc'd records array; the caller must pass every job returned
 * by analyze to exactly one emit. */
typedef struct y264_emit_job y264_emit_job_t;
y264_emit_job_t *y264_frame_analyze(y264_frame_t *f);
void             y264_frame_emit(y264_bs_t *bs, y264_frame_t *f, y264_emit_job_t *job);

/* The same pass 2, one slice at a time: emits the macroblock rows slice `s`
 * owns and leaves the job alive, so a caller that writes a header and starts an
 * entropy coder per slice can drive the picture itself. The caller owns the
 * job either way and must end with exactly one y264_frame_emit_free; plain
 * y264_frame_emit is slice 0 plus the free, which is the whole picture while
 * nslices is 1. */
void             y264_frame_emit_slice(y264_bs_t *bs, y264_frame_t *f,
                                       y264_emit_job_t *job, int s);
void             y264_frame_emit_free(y264_emit_job_t *job);

/* Resolve env-gated analyze lazy statics on the main thread before workers run
 * (called from yah264_encoder_open); keeps the analyze wavefront TSan-clean. */
void             y264_mb_warm_statics(void);

/* Y264_MBT_DERIVED: the whole-system derived mb-tree mode. One gate for the whole
 * jointly-adapted set of constants and compositions that separate our mb-tree
 * from the reference encoder's -- the field's derivation AND its consumption -- because every
 * axis-aligned half of it is measured-refused. Lives here rather than in
 * encoder.c because aq_analyze needs it too. */
int              y264_mbt_derived(void);

#endif /* YAH264_MACROBLOCK_H */
