/*
 * encoder.h - internal encoder state
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef YAH264_ENCODER_INT_H
#define YAH264_ENCODER_INT_H

#include "yah264.h"
#include "set.h"
#include "macroblock.h"
#include "me.h"
#include "../dsp/pixel.h"
#include "../common/threadpool.h"
#include <stdint.h>
#include <stdio.h>

/* One lowres 8x8 block analysed against one reference leg. Distortion and rate
 * stay in SEPARATE fields (TPL-ready): consumers compose a scalar via lr_cost;
 * TPL (later) reads the fields apart. d_inter is pure SATD; r_inter is a bit
 * count (0 until motion is priced). See docs/rate-aware-lookahead-design.md. */
typedef struct y264_lr_blk {
    int32_t d_inter;
    int32_t r_inter;
    int16_t mvx, mvy;
} y264_lr_blk;

/* Ring array size for the lookahead window (struct yah264_encoder.la[] and
 * la_thread.q[]): la_depth's own clamp (64) plus headroom for Y264_LA_BUF's
 * extra input buffering (docs/sync-lookahead-design.md). la_depth itself --
 * the WINDOW a given mb-tree/scene-cut walk is capped at -- never grows past
 * 64; only ring CAPACITY (la_cap = la_depth + la_buf) uses the extra room. */
#define Y264_LA_CAP_MAX 80

/* Distinct anchor subpel sets one mb-tree Phase A can share (encoder.mbt_sub).
 * A --bframes 3 --rc-lookahead 40 window brackets its sources with about 11
 * anchors, so 12 covers the default shape whole; past the cap a source falls
 * back to building into its worker's own set. Each set is 15 lowres planes --
 * 3.5 MB at 720p. */
#define Y264_MBT_SUB_MAX 12

/* Narrowest pool that runs more than one grid at a time. Below it the
 * staircase (stair_ready) and the concurrent B leaves (fpipe_ready) decline to
 * engage. yah264_frame_thread_cap floors on it for the same reason:
 * refusing a worker a lone grid cannot use is cheap, but refusing the eight
 * that turn the multi-frame machinery on is not (measured 559 -> 433 ms on
 * CIF t18).
 *
 * That 559/433 measurement moved THREE gates at once, so it prices them
 * jointly and attributes nothing to any one. The lookahead lead is separated
 * out (la_pool_min): a pool of 2 running its own lookahead thread measures
 * -3.8% to -5.4%, so "a dedicated thread is pure overhead" is false for the
 * lead. It may still be true for the other two; nobody has separated those.
 *
 * The floor is 2, priced down from 8 one step at a time, each step measured:
 * floor 6 at t6 is 21-30% faster than floor 8 (bus 0.177->0.124s, foreman
 * 0.155->0.108, samsung 0.563->0.442) and flips all three cells to BEAT
 * x264 (0.83-0.98x) where floor 8 reads 1.40-1.61x;
 * floor 4 at t4 is 22-24% faster (bus 0.197->0.154s, foreman 0.172->0.131)
 * and BEATS x264 (0.84-0.85x); t5 26-29% faster (1.02-1.04x);
 * floor 2 at t3 is 18% faster (foreman 0.194->0.160s, 0.90x vs x264), t2 9%
 * (0.222s, 1.04x).
 * Thread counts above each step stay byte-identical across it, every lowered
 * floor is deterministic under load, and size moves at most +-0.3% (the
 * engaged mechanisms are the band-priced ones). 2 is the terminal value --
 * la_pool_min's own floor is 2 for the same reason ("a lead with no pool at
 * all leads nothing"), and --threads 1 has no pool. */
#define Y264_MT_POOL_MIN 2

/* Burst slots in the staircase ring (struct stair_ctx.bur[], and the per-POC
 * caches an anchor's still-streaming recon forces on its consumers).
 * Everything genuinely per-anchor lives in struct stair_burst, so the ring
 * widens with this constant; the CHAIN that executes a burst (driver, leaves,
 * bemit, serial_done, the ref-B pipeline) is a singleton, and stair_run_burst
 * drains one chain before submitting the next. So K > 2 buys reuse DISTANCE,
 * not concurrency: a launched slot is recycled K launches later instead of 2.
 * Raising it alone cannot change bits; running several chains concurrently is
 * what makes the extra slots run. */
#define Y264_STAIR_K 3

/* The list-0 clamp reaches K-1 BURSTS back and no further, whatever --ref is:
 * a launch drains until the ring holds fewer than K bursts, so when an anchor
 * preps, at most K-1 of its predecessors are still live and everything older
 * has been retired by construction. --ref decides whether the older of those
 * K-1 is REACHABLE from a list 0; it does not change how many there are.
 *
 * Two clamp slots per burst: its anchor and its reference B, both of which
 * a deep enough list 0 reaches while they stream. */
_Static_assert(Y264_STAIR_HOPS == (Y264_STAIR_K - 1) + 1,
               "the clamp set is exactly as deep as the burst ring allows");

/* Ceiling on the picture-buffer pool (struct dpb_bag below). The arithmetic
 * bound is K launches x (one anchor + that burst's reference B's) recycles
 * outstanding at once; 16 covers K=3 at every b-pyramid shape this encoder
 * builds. The pool is sized from the shape at open, not from this. */
#define Y264_DPB_POOL_MAX 16

/* 2-pass complexity compression . Named because the pass-2
 * budget solve and yah264_2pass_stat_weight -- which a GOP-parallel caller
 * uses to size each GOP's share of that budget -- have to agree on it. */
#define Y264_TP_QCOMP 0.6

#define LR_MV_INVALID  INT16_MIN
/* Reference legs of a lookahead entry. Fixed roles for now; Q7 generalises to
 * [list][dist] behind the same accessor, so no consumer indexes leg[] literally
 * beyond these names. */
enum { LR_LEG_PREV = 0,     /* vs previous display frame (push time) */
       LR_LEG_NEXT = 1,     /* vs next frame / future anchor (R1; unused R0) */
       LR_LEG_ANCHOR = 2,   /* vs previous typed anchor (anchor entries) */
       LR_NLEGS = 3 };

struct yah264_encoder {
    yah264_param_t param;
    struct y264_hw *hw;         /* the hardware session when this handle is the hardware mode; every entry point dispatches on it first */
    struct hw_sc   *hw_sc;      /* its causal scene-cut state (hw_scenecut_probe) */

    int width;
    int height;
    int width_in_mbs;
    int height_in_mbs;
    int padded_w;           /* width_in_mbs * 16 */
    int padded_h;           /* height_in_mbs * 16 */

    /* Chroma subsampling. cf_idc is H.264 chroma_format_idc (1/2/3). sub_w/sub_h
 * are SubWidthC/SubHeightC: the luma:chroma sample ratio per axis (2 = half
 * resolution, 1 = full). 4:2:0 = (2,2), 4:2:2 = (2,1), 4:4:4 = (1,1). Chroma
 * plane dims are padded_w/sub_w x padded_h/sub_h. */
    int cf_idc;
    int sub_w, sub_h;

    y264_sps_t sps;
    y264_pps_t pps;

    /* Custom quantisation matrices. cqm_on selects between the flat default
 * (cqm unused, quant kernels take their byte-identical fast path) and the
 * JVT scaling lists held in `cqm`. */
    int        cqm_on;
    y264_cqm_t cqm;

    uint32_t cpu;
    y264_pixel_fn_t pixel;  /* dispatched kernels, wired for later phases */

    /* MB-aligned working planes (edge-replicated from the input picture). */
    pixel   *plane[3];
    int      pstride[3];

    /* Reconstructed planes (MB-aligned) and per-4x4 nnz grids for CAVLC context.
 * ref[] holds the previous reconstructed frame, the reference for P slices. */
    pixel   *rec[3];
    /* the most recently CODED frame's recon planes. rec[] is only a label for
 * "the buffer the next frame codes into" -- reference handoff rotates the
 * buffer away (dpb swap / ref1 swap), so the public recon API tracks the
 * coded frame's actual buffer here. */
    pixel   *rec_out[3];
    pixel   *ref[3];            /* list-0 reference (previous anchor recon) = refring[0] */
    pixel   *ref1[3];           /* list-1 reference (future anchor recon), for B */
    /* B-pyramid list-1: the DPB picture itself, not a copy. Pointing the slice at
 * the DPB plane lets the half-pel registry match its CACHED hpel (built once at
 * dpb_store); a copy into ref1[] would never match by pointer, so every B
 * would rebuild the list-1 half-pel planes from scratch. NULL = no pyramid list-1
 * (the flat-B path, where ref1[] holds the anchor recon and has no DPB entry). */
    pixel   *cur_l1p[3];
    /* Multi-reference ring for the IPPP path (list-0, most-recent-first). refring[0]
 * aliases ref[]. nref is the configured count; nref_valid grows as refs
 * accumulate after an IDR (min(nref, frames since IDR)). */
    int      nref;
    int      nref_valid;
    pixel   *refring[16][3];
    int      refring_fn[16];
    int      refring_poc[16];
    /* A2: precomputed half-pel luma planes, rebuilt per slice for the references
 * ME searches (indices 0..nref-1 = list 0, index nref = list-1 anchor for B).
 * hpel_scratch holds the horizontal 6-tap intermediates during a build. */
    pixel        *hpel_buf[17][3];      /* [ref][H,V,C] */
    /* Non-pyramid per-buffer half-pel ownership: each rotating reference buffer
 * (rec / ref1 / refring) owns an H/V/C triple that travels with it as the
 * pointers rotate, so a stored anchor recon is 6-tap filtered exactly once
 * (the dpb_store hpel cache's flat-path analogue). Valid flags travel with
 * the buffers too; a triple is only consulted while its buffer sits in the
 * current reference set, where content and hpel are always consistent. */
    int           flat_hp_on;
    pixel        *rec_hp[3];
    int           rec_hpv;
    pixel        *ref1_hp[3];
    int           ref1_hpv;
    pixel        *ring_hp[16][3];
    int           ring_hpv[16];
    y264_hpel_ref_t hpel_ctx[17];
    int32_t      *hpel_scratch;
    int           hpel_ws_n;            /* per-worker hpel scratch slots (parallel build) */
    size_t        hpel_ws_bytes;        /* size of each; grows with the band height */
    int32_t      *hpel_scratch_ws[64];
    int           hpel_on;              /* built + registered this run */
    int8_t  *nnz[3];
    int      nnz_stride[3];
    int8_t  *i4mode;
    int      i4mode_stride;
    int     *mbcbp;             /* per-MB cbp cache for CABAC context */

    /* Per-4x4 motion fields carried across the frame for MV prediction (L0, L1). */
    int16_t *mvx, *mvy;
    int8_t  *refidx;
    int16_t *mvx1, *mvy1;
    int8_t  *refidx1;
    int16_t *mvdx, *mvdy, *mvdx1, *mvdy1;   /* per-4x4 abs mvd, CABAC context */
    int8_t  *aq_off;                        /* per-MB AQ QP offset (NULL = AQ off) */
    uint8_t *mbqp;                          /* per-MB coded QP, for deblock */
    uint8_t *mb_tr8;                        /* per-MB: 8x8 transform used (for deblock) */
    float    aq_strength;                   /* variance-AQ strength */
    /* AQ energy/anchor shape, resolved once at open (never read from the
 * environment on a worker thread). aq_abs 1 = offset against the absolute
 * anchor instead of the frame mean; aq_chroma 1 = x264's all-plane
 * ac_energy. See crf_cplx_env / aq_chroma_env in encoder.c. */
    int      aq_abs;
    int      aq_chroma;
    double   aq_anchor;
    /* Co-located motion of the list-1 anchor, saved for B direct modes. */
    int16_t *colmvx, *colmvy;
    int8_t  *colref;
    int16_t *colpoc;            /* per-4x4 referenced-picture POC (temporal) */
    int      colframepoc;       /* POC of the frame colmv came from (temporal seed) */
    int      col_l0poc0;        /* that frame's own list-0[0] POC (Y264_TDIR_LEGAL) */
    int      mv_stride;
    /* The most recently coded frame's reference-list POCs (captured in
 * build_slice), used to resolve colpoc when it enters the DPB / col grid. */
    int      cur_l0poc[16];
    int      cur_l0n;
    int      cur_l1poc0;        /* B slices: list1[0] POC, else -1 */
    /* Y264_DIRECT_AUTO: x264's per-slice direct-mode score, [0] temporal,
     * [1] spatial, counting macroblocks each mode would make skippable, with
     * a 9/10 decay once the total passes the macroblock count. Per encoder
     * instance, and the knob refuses threads > 1, because a running total
     * accumulated across frames is order-dependent and GOP workers do not
     * encode in slice order. */
    long     direct_score[2];
    long     dauto_pending[2];      /* serial path: counts of frames coded so far,
                                     * folded into direct_score at the next B prep */

    /* Decoded picture buffer for hierarchical B (b-pyramid). Each live reference
 * keeps its recon planes and list-0 motion (for co-located spatial-direct). */
    struct dpb_entry {
        pixel   *plane[3];
        pixel   *hpel[3];   /* cached half-pel planes (H,V,C), built once at store
 * time and reused by every frame that references this
 * picture -- avoids the per-encoded-frame rebuild */
        int      hpel_valid;
        int16_t *mvx, *mvy;
        int8_t  *refidx;
        int16_t *colpoc;    /* per-4x4: POC of the picture each block references
 * (resolved through the frame's own lists at store
 * time; -1 = intra/unused), for temporal direct.
 * Bit Y264_COLPOC_L1 marks a block that predicted
 * from list 1 only (its POC is the list-1 one). */
        int      col_l0poc0;/* this picture's own list-0[0] POC when it was
 * coded (-1 for an I picture): the slice-level
 * temporal-direct legality test (Y264_TDIR_LEGAL) */
        int      poc;
        int      frame_num;
        int      used;
    } dpb[16];
    int      dpb_size;          /* allocated DPB slots */

    /* The buffers a DPB slot lends its occupant, pooled so their lifetime is the
 * PICTURE's and not the SLOT's.
 *
 * A slot owns plane/hpel/mv for as long as the slot exists, so recycling it
 * hands the recon buffer to the next writer and rebuilds the half-pel and
 * colocated grids in place -- under the previous occupant, if a burst is
 * still searching it. That is what stair_slot_readers_wait blocks for, and
 * blocking is what converts the ring's width back into serialized waiting
 * (26-57 fires per encode, each joining an older chain outright).
 *
 * Pooled, a recycle takes a fresh bag and parks the retiring picture's bag
 * until nothing that could name it is still live. Nothing waits. The pool
 * only exists where Y264_STAIR_WIDE can engage (b-pyramid, nref <= 1, no
 * rcp), and an exhausted pool falls back to the wait, which is the shape
 * the code had before it. */
    struct dpb_bag {
        pixel   *plane[3];
        pixel   *hpel[3];
        int16_t *mvx, *mvy;
        int8_t  *refidx;
        int16_t *colpoc;
    } dpbp_free[Y264_DPB_POOL_MAX];
    int            dpbp_nfree;
    /* Parked: `seq` is the newest launch at the moment the bag was retired, so
 * every burst that can hold a pointer into it has seq <= this. Released
 * once the oldest live burst is newer than that -- one test that covers
 * readsets, the colocated redirect and the deferred recon replay alike,
 * instead of three that each have to stay complete. */
    struct { struct dpb_bag b; unsigned seq; } dpbp_pend[Y264_DPB_POOL_MAX];
    int            dpbp_npend;
    int            dpbp_n;      /* bags allocated; 0 = pool off, always wait */
    int            dpbp_hi;     /* measured high-water parked bags */
    int            dpbp_exh;    /* takes that found the pool empty (fell back) */
    int            dpbp_take;   /* recycles served from the pool */
    int      next_frame_num;    /* running FrameNum counter (reset at IDR) */
    int      last_ref_fn;       /* FrameNum of the most recently coded reference */
    int      b_pyramid;         /* 1 = referenced hierarchical B */
    int      cur_ref_l0_fn;     /* P slice: FrameNum to pin as list0[0] */
    int      prev_anchor_poc;   /* POC of the previous anchor (list-0 base) */
    /* The anchor before that one: hop 2 of the list-0 clamp set. Maintained
 * beside prev_anchor_poc on EVERY path that assigns it (the staircase
 * launch and the serial anchor), so like prev_anchor_poc it is coding-order
 * state and not a function of whether the concurrency engaged. -1 = none,
 * which is also what an IDR leaves behind (POC restarts there, so the
 * previous GOP's anchor is not a key any more). */
    int      prev_anchor_poc2;
    /* The same history for recent bursts' REFERENCE B's -- the other half
 * of the list-0 clamp set. A burst's reference B is coded right after its
 * anchor and so outranks every older picture on FrameNum, which puts it at
 * index 0 or 1 of every later P slice's list 0 at --ref >= 2: the nearest
 * streaming picture an anchor reads, not the furthest.
 *
 * Only [0] is read (the immediate predecessor's, which is the only burst
 * whose reference B an anchor row-gates against -- see stair_refbgate_on).
 * A P anchor preps BEFORE its own launch pushes, so [0] names its
 * predecessor there. The array is K deep because that is what the push
 * costs anyway and a future round that reaches further wants the history to
 * already be there rather than to re-derive when it was safe to keep.
 *
 * Kept beside prev_anchor_poc on the same two paths and reset the same way
 * at an IDR, so it is coding-order state and never a function of what was
 * live. -1 = that burst had no single reference B (stair_refb_poc's shape
 * test, which is exactly the nrefb == 1 shapes). */
    int      refb_hist[Y264_STAIR_K];
    int      stat_hop2_slices;  /* Y264_STAIR_STAT: P slices prepped with hop 2 */
    int      stat_hop2_refs;    /* of which, list-0 entries that take the clamp */
    int      stat_refbgate;     /* Y264_STAIR_STAT: launches whose ref-B wait the
 * row gate replaced (per live burst) */
    int      stat_refbblock;    /* and launches that still had to block */
    /* Staircase depth: a streaming anchor's SOURCE-luma interior sum, the
 * wp-estimate substitute for its not-yet-readable recon. Computed at every
 * pyramid anchor arrival when the depth gate is on.
 *
 * A ring keyed by POC, not one scalar: at depth 2 the only clamped list-0
 * reference is the immediate predecessor, so "the last one" would be an
 * adequate key, but a K-slot burst ring can have K anchors streaming and a
 * consumer has to name the one it means. Written once per anchor in coding
 * order and searched NEWEST-FIRST, so with one chain in flight the lookup
 * returns exactly what a scalar would hold. Reset at IDR, since POC restarts there and
 * a stale same-POC entry from the previous GOP would otherwise be
 * reachable once lookups stop being "the newest".
 *
 * Reference B's are cached here too (same substitution, same POC key), so a
 * burst can contribute an anchor AND a reference B. */
    struct { int poc; uint64_t sum; int valid; } anchor_srcsum[2 * Y264_STAIR_K];
    int      anchor_srcsum_w;   /* next ring slot to write */
    int      cur_b_depth;       /* temporal depth of the B being coded (0 = anchor) */
    int      since_idr;         /* frames since the last IDR (managed; scenecut resets) */
    int      sc_have_prev;      /* lowres_prev holds a valid frame */
    /* Half-resolution luma for lookahead analysis (scene-cut ME etc.). lr_w/lr_h
 * are padded/2, so a 16x16 macroblock maps to one 8x8 lowres block. */
    int      lr_w, lr_h;
    pixel   *lowres_cur;        /* current frame, downscaled */
    pixel   *lowres_prev;       /* previous frame, downscaled */
    /* Per-macroblock lookahead costs/MVs from the last lowres analysis (cur vs
 * prev): intra and best-inter SATD, and the winning lowres MV. */
    int     *lr_intra, *lr_inter;
    int16_t *lr_mvx, *lr_mvy;
    /* mb-tree: per-MB QP offset for an anchor, from how much the buffered B's of
 * its mini-GOP depend on each of its macroblocks (backward propagation). */
    /* Early-skip probe acceptance, resolved once in encoder_open (never a lazy
 * static: workers read it concurrently). See y264_frame_t.skipdec_*. */
    int      skipdec_p, skipdec_b, skipdec_t;
    int      skip_mvagree_p, skip_mvagree_b, skip_costgate;
    int      bskip_confirm, bskip_dec, bskip_probe, bskip_notrellis;
    int      bskip_admit, bskip_cguard;     /* E2 stages A and C */
    int      mbtree_on;
    int      mbtree_apply;      /* mbtree_off is valid for the frame being emitted */
    int      mbtree_skip;       /* Y264_MBTREE_OFF probe: compute/apply skipped */
    int8_t  *mbtree_off;
    /* Per-MB lowres ANCHOR-leg MV (quarter-pel), stashed at pop for the P ME seed;
 * lr_seed_valid marks it populated for the frame about to be coded. */
    int16_t *lr_seed_mvx, *lr_seed_mvy;
    int32_t *lr_seed_cost;      /* per-MB lowres inter SATD (d_inter), the ME oracle cost */
    int      lr_seed_valid;
    pixel   *lowres_tmp;        /* scratch lowres for downscaling a buffered B */
    pixel   *code_panchor_lr;   /* previous anchor's lowres, retained at code time for
 * the behaviour-matched mb-tree buffered-B list0 cost */
    int      code_panchor_have; /* code_panchor_lr populated (0 right after an IDR) */
    int      code_panchor_poc;  /* its POC -- the buffered-B memo's past-leg key */
    long     code_panchor_push; /* its lookahead push index (gpq key; 0 = unknown) */
    /* Precomputed quarter-pel subpel planes for the two mb-tree lowres reference
 * legs (list0/list1). Index [phase] where phase = (fy<<2)|fx, 1..15 used
 * (phase 0 = integer = the ref plane itself). Each is lr_w*lr_h, stride lr_w.
 * Hoists blk8_satd_qp's per-candidate bilinear out of the lowres ME search. */
    pixel   *lr_subpel[2][16];

    /* Per-worker mb-tree Phase-A workspace (parallel lowres ME over the lookahead
 * window; the float splat that accumulates onto the shared prop grids stays
 * serial for byte-identity). Allocated lazily to the pool thread count. */
    int      mbt_nws;                 /* number of per-worker slots allocated */
    pixel   *mbt_subpel[64][2][16];   /* private subpel legs per worker */
    /* Shared subpel phase-planes, keyed by the anchor lowres they were built
 * from. A source's legs are its bracketing ANCHORS', and a window holds far
 * fewer anchors than the pool holds workers -- ~11 at --bframes 3
 * --rc-lookahead 40 against 18 -- so the per-worker sets above hold the same
 * 15 planes rebuilt N times over. Built once per Phase A and read
 * concurrently; the per-worker sets stay as the fallback past the cap and
 * for a second Phase A that finds the cache claimed. */
    pixel       *mbt_sub[Y264_MBT_SUB_MAX][16];
    const pixel *mbt_sub_key[Y264_MBT_SUB_MAX];
    long         mbt_sub_stamp[Y264_MBT_SUB_MAX]; /* the anchor's push index behind the
                                       * key, -1 = empty: a ring slot is reused for a
                                       * new frame under the same pointer, so the
                                       * pointer alone cannot name a set across calls */
    unsigned     mbt_sub_use[Y264_MBT_SUB_MAX];   /* last call that read the set (LRU) */
    unsigned     mbt_sub_call;        /* Phase A call counter for the LRU */
    int          mbt_sub_n;           /* sets allocated (grows to the cap) */
    int          mbt_sub_busy;        /* a Phase A owns the cache */
    pixel   *mbt_lrtmp[64];           /* private downscale scratch per worker */
    double  *mbt_invq[64];            /* private invqscale scratch per worker */
    float   *mbt_aqoff[64];

    /* Lookahead window: input is delayed la_depth frames so mb-tree can
 * propagate dependencies backward from future frames. Each entry is
 * analysed at push (lowres, scene-cut flag via a push-side replica of the
 * core's since_idr state machine, and for anchors a lowres ME against the
 * previous anchor); the core encodes entries as they pop. */
    struct la_entry {
        pixel   *plane[3];      /* padded input copy */
        pixel   *lowres;
        int      is_cut;        /* scene cut here (the core obeys this flag) */
        int      is_idr;
        int      is_anchor;
        int      typed;         /* is_anchor finalized (lags one push: b-adapt
 * needs the next frame's backward cost) */
        int      since_val;     /* finalize-side since_idr at this frame */
        long     sum_icost;     /* frame-vs-prev intra sum, for the deferred
 * scene-cut decision (finalize, not push) */
        long     sum_cost[LR_NLEGS]; /* frame min(intra,leg) sums, per leg */
        int      sc_cleared;    /* flash suppression: a prior frame's finalize
 * cleared this frame's scene-cut candidacy */
        int32_t *d_intra;       /* per-MB lowres intra cost, own frame */
        y264_lr_blk *leg[LR_NLEGS]; /* per-MB per-leg {D,R,MV}: PREV = vs prev frame,
 * ANCHOR = vs prev anchor (anchor
 * entries) / vs prev anchor as the B's list-0
 * lowres pair (B entries, bleg_have); NEXT = the
 * B's list-1 lowres pair vs its future anchor. */
        int      bleg_have;     /* B entry: ANCHOR/NEXT pair legs filled */
        int      bleg_poc0, bleg_poc1;  /* the pair-leg anchor POCs */
        int      aleg_have, aleg_poc0;  /* anchor entry: leg[LR_LEG_ANCHOR] is its field
                                         * vs the previous anchor (POC aleg_poc0), which
                                         * Phase A can reuse instead of re-searching */
        /* mb-tree Phase-A memoization: the per-source lowres-ME slice is a pure
 * function of (this frame, its bracketing past+future anchors), invariant
 * across every anchor's mb-tree that sources this frame while the window
 * slides. Cache it once, keyed on the two anchor POCs, and reuse -- kills
 * the O(window^2) re-ME. Byte-identical (pure memoization). Cleared when
 * the ring slot is reused at push. */
        long     push_idx;      /* absolute lookahead push index, 1-based (the
 * gpq key -- POC restarts per IDR and would
 * silently union GOPs) */
        int      mbt_pa_valid;
        int      mbt_pa_past_poc, mbt_pa_fut_poc;
        long        *mbt_pa_pi, *mbt_pa_pin;
        signed char *mbt_pa_plu;
        int         *mbt_pa_pmv;
        double      *mbt_pa_psw;
    } la[Y264_LA_CAP_MAX];
    int      la_depth;          /* window size in frames (0 = off); every
 * mb-tree/scene-cut window walk is capped at
 * this value regardless of la_cap */
    int      la_buf;            /* Y264_LA_BUF: extra input-buffering frames
 * ahead of the window (0 = no extra buffering) */
    int      la_cap;            /* ring capacity = la_depth + la_buf; only
 * used for slot indices / wraparound / fill
 * threshold, never for a window walk bound */
    int      la_n, la_head;     /* buffered count, ring index of the oldest */
    long     la_push_seq;       /* pushes so far (assigns la_entry.push_idx) */
    int      la_since_idr;      /* finalize-side replica of since_idr */
    pixel   *la_lr_prev;        /* lowres of the previously pushed frame */
    int      la_have_prev;
    pixel   *la_fin_prev_lr;    /* lowres of the previously *finalized* frame (the
 * pre-flash reference for the 1-frame flash test) */
    int      la_have_prev_fin;  /* a frame has been finalized (finalize-side prev) */
    pixel   *la_anchor_lr;      /* lowres of the previously typed anchor */
    int      la_anchor_have;
    /* Previous anchor pair's ANCHOR-leg MV field (lowres quarter-pel): the
 * colocated temporal seed for the next pair's lowres ME (motion
 * propagation -- tracks accelerating/zoom motion a from-zero search
 * cannot reach). Cleared at IDR anchors (chain restart). */
    int16_t *la_anchor_mvx, *la_anchor_mvy;
    int      la_anchor_mv_have;
    int      la_anchor_poc;     /* POC (since_val*2) of the previously typed anchor */

    /* B-frame lowres pair seeds (x264's lowres MVs analogue): each
 * typed B gets a lowres MV field vs the previous anchor (leg[LR_LEG_ANCHOR])
 * and vs its future anchor (leg[LR_LEG_NEXT]), computed at the future
 * anchor's la_finalize. Stashed at pop (fullres qpel) into bseed_pend, then
 * buffered per reorder slot like bmotion[], scaled to the coded B's actual
 * ref POCs into bseed_cur at code time (b-pyramid legs vary). */
    int16_t *bseed_pend[4];     /* l0x, l0y, l1x, l1y; nmb each */
    int      bseed_pend_valid, bseed_pend_poc0, bseed_pend_poc1;
    int16_t *bseed[8][4];
    int      bseed_valid[8], bseed_poc0[8], bseed_poc1[8];
    int16_t *bseed_cur[4];      /* scaled to the coded B's refs, attached to f */
    /* Measurement only (Y264_BLATE_STAT, t1): the pair legs' per-MB lowres
 * COSTS carried beside the MV seeds -- [0] l0 d_inter, [1] l1 d_inter,
 * [2] own-frame d_intra. Allocated only when the stat is armed. */
    int32_t *bseedc_pend[3];
    int32_t *bseedc[8][3];
    int      cur_bseed;         /* reorder slot being coded (-1 = none) */
    int      la_brun;           /* push-side length of the current B run */
    int      badapt_on;         /* adaptive B placement active */
    int      anchor_seq;        /* non-pyramid: anchors coded since the IDR */
    double  *la_prop_a, *la_prop_b;  /* chain-propagation scratch (nmb each) */

    /* B-frame reorder buffer: pictures held until their future anchor arrives. */
    int      bframes;           /* configured B-frames between anchors */
    pixel   *bplane[8][3];      /* buffered B source planes (display order) */
    /* mb-tree Phase-A memo carried out of the lookahead ring with a buffered B
 * (stolen by pointer-swap at buffering time): while the B sat in the ring
 * its slice was memoized against the SAME bracketing anchors it is coded
 * with, so Phase A's per-call re-ME of every buffered B (the one source
 * class the ring memo could never cover) is a pure cache hit. */
    struct mbt_bmemo {
        int valid, past_poc, fut_poc;
        long *pi, *pin;
        signed char *plu;
        int *pmv;
        double *psw;
    } bmbt[8];
    struct la_entry *cur_la_en; /* ring entry being coded by encode_frame_core
 * (NULL on the legacy no-lookahead path); lets
 * the B-buffering site steal the entry's memo */
    /* Reference-B mb-tree (Y264_MBT_BREF). x264 propagates leaf -> ref B ->
 * anchor and gives the reference B its own offset field; anc[] holds only
 * is_anchor entries, so without this leaves deposit straight onto anchors
 * and the reference B is in the graph nowhere. Promoting it to a
 * propagation TARGET needs its lowres to persist for the whole walk --
 * buffered B's are downscaled on demand into a per-thread temp because they
 * are otherwise only SOURCES, but anc[].lr is read as a reference plane by
 * every source that brackets onto it. */
    pixel   *blowres[8];        /* persistent lowres per buffered B (ref-B target) */
    int8_t  *bmbtree_off[8];    /* that B's own mb-tree offset field */
    int      bmbtree_valid[8];
    int      bpoc[8];           /* buffered B POC */
    int      bdisp[8];          /* buffered B display index (input order) */
    long     bpush[8];          /* buffered B lookahead push index (gpq key;
 * 0 = unknown -> CPU path) */
    int      bmotion[8];        /* buffered B lowres motion score (adaptive ME) */
    int      btdiff[8];         /* buffered B lowres |tdiff| EWMA x256 (psy calm gate) */
    int      cur_lr_motion;     /* motion score of the frame being encoded */
    int      cur_lr_tdiff;      /* |tdiff| EWMA x256 of the frame being encoded */
    int      lr_tdiff_ewma;     /* chained at ARRIVAL order (deterministic) */
    int      nbuf;              /* number currently buffered */
    int      cur_disp;          /* display index of the frame being emitted */
    /* Display indices of finalised frames, in coding order, as a FIFO the
     * caller drains. A muxer needs this and had no way to get it: a call can
     * emit an anchor plus several B's, so a caller pairing output packets with
     * input timestamps in arrival order gets every B-frame's timestamp wrong.
     *
     * It is a FIFO rather than a per-call list because a frame's NAL and its
     * finalisation are decoupled: the deferred-NAL contract lets an entropy
     * emit still be in flight at the next call, which appends it ahead of that
     * call's own frames. So a call can finalise five frames and append four
     * NALs, and the fifth belongs to the next call's output. Draining by packet
     * count rather than by call keeps the two in step. */
    int      emit_disp[128];
    int      emit_count;
    int      mbt_oracle_idx;    /* mb-tree replay probe: prepared record index
 * for the imminent mbt_resolve (-1 = none) */

    /* Per-emitted-frame reconstruction callback (coding order). Lets a caller
 * capture every frame's recon, including reordered B's. */
    void   (*recon_cb)(void *ud, const yah264_picture_t *rec, int disp_index);
    void    *recon_ud;

    int qp;
    int chroma_qp;

    /* Single-pass ABR rate control (rc.method 1). A reactive controller adjusts
 * a running base QP each frame to track the target average bitrate, using the
 * previous frame's (bits, QP) as a complexity estimate and pulling back a
 * running buffer error. Inactive (abr_on 0) leaves the constant-QP path. */
    int      abr_on;
    double   abr_qp;            /* running base QP (fractional) */
    double   abr_target_bpf;    /* target bits per frame */
    double   abr_cum_target;    /* cumulative target bits so far */
    double   abr_cum_actual;    /* cumulative actual bits so far */
    double   abr_last_bits;     /* bits of the previous coded frame */
    int      abr_last_qp;       /* base QP of the previous coded frame */
    double   abr_scale[3];      /* calibrated bits*qscale per unit complexity, per frame type (I/P/B) */
    double   abr_cur_cplx;      /* complexity of the frame being coded */
    int      abr_inited[3];
    int      abr_rf;            /* x264's ABR allocation model (param.rc.abr_model
 * or Y264_ABR_RF); resolved once at open */     /* per-type scale has been calibrated at least once */
    int      abr_rf2;           /* Y264_ABR_RF2: the CRF path plus a rate factor (plan A2) */
    double   rf2_rceq;          /* its duration-only rceq, constant per encode */
    /* x264's ABR rate factor. The per-type scale
 * above solves qscale = scale*rceq/target, and since scale IS bits*qscale/rceq
 * that makes bits == target for EVERY frame -- constant bits per frame, which
 * inverts the I/P/B cascade. These two accumulators replace it: their RATIO is
 * the rate factor, q = rceq/rate_factor, so bits follow C^0.6. Self-normalising,
 * with no gain constant to tune. Kept in OUR qscale convention (2^((qp-12)/6),
 * no 0.85) so they stay consistent with the open-time seed. */
    double   rf_cplx_sum;          /* sum of bits * qscale / rceq */
    double   rf_wanted_bits; /* sum of per-frame target bits */
    double   ptrack_qp;         /* P-equivalent QP track, for the I anchor */
    double   ptrack_norm;
    int      last_nonb_type;     /* -1 until the first non-B is decided */
    double   last_ref_qp[2];     /* coded QP of the last two non-B frames, for B */
    double   last_qscale_type[3]; /* per-type previous qscale, for x264's asymmetric clip */
    double   rf2_kc[3];      /* A5b: per-type EMA of contrib / decide complexity (in-flight prediction) */
    int      rf2_kc_cal[3];
    double   st_cplxsum, st_cplxcount;  /* x264 short_term_cplx*: the rate factor
 * runs on a BLURRED complexity, not the raw
 * per-frame one. sintel opens on near-black
 * frames whose C is legitimately ~1, and a
 * raw signal lets that degenerate start
 * poison the accumulators. */
    double   abr_fps;
    double   abr_cmean;         /* running sum of decide complexity, for the */
    int      abr_cn;            /* degenerate-complexity guard (Y264_ABR_CGUARD) */

    /* Constant rate factor (YAH264_RC_CRF). Sets each frame's QP from crf plus a
 * complexity term relative to a per-frame-type running geometric-mean
 * reference, so quality is constant and simple frames get more bits. */
    int      crf_on;
    double   crf;               /* target rate factor (rc.rf) */
    double   crf_qcomp;         /* complexity compression (x264 qcompress, 0.6);
 * under 2-pass it is Y264_TP_QCOMP, which
 * yah264_2pass_stat_weight also reads */
    double   crf_cblur;         /* blurred anchor (P) complexity, absolute */
    int      crf_cblur_init;
    /* mb-tree operating-point shift: CRF is open-loop
 * and never accounts for mb-tree lowering per-MB QP, so mb-tree redistribution
 * costs extra bits and hurts under CRF (helps under ABR). x264 adds a fixed
 * rate-factor shift (~(1-qcomp)*13.5) that mb-tree's negative offsets net back
 * out; the shift is UNIFORM so the cross-frame differential (static gets more,
 * motion less) survives. yah264 shifts the CRF base by the running-average
 * mb-tree reduction. Gated Y264_CRF_CL. */
    int      crf_cl;            /* mb-tree operating-point devices active */
    double   mbtree_mean_off;   /* mean mb-tree QP offset of the frame being coded (<=0) */

    /* VBV (leaky-bucket buffer model). Layered on top of the base QP: raises it
 * when the buffer would underflow (frame too big), lowers it when the buffer
 * is near full. vbv_fill is the current buffer occupancy in bits. */
    int      vbv_on;
    double   vbv_rate;          /* bits added per frame (maxrate / fps) */
    double   vbv_size;          /* buffer capacity in bits */
    double   vbv_fill;          /* current buffer occupancy in bits */
    double   vbv_scale;         /* calibrated bits*qscale per unit complexity */
    int      vbv_inited;        /* scale calibrated at least once */
    /* The declared handoff occupancy (param.rc.vbv_seg_join). Half the buffer,
 * which is vbv_fill_budget's fixed point, so a segment that starts here
 * exits here under the budget's own dynamics. */
    double   vbv_seg_h;
    int      vbv_first_bound;   /* the first coded frame is still unbounded:
 * re-encode it against its measured size */
    /* Y264_VBV_BOUND: extend that measured-size bound from the first frame to
 * EVERY frame. Doing so requires every frame to reach the serial emit,
 * because that is the only route with a retry window -- see vbv_bound_all.
 * Resolved once here so no worker reads an env. */
    int      vbv_bound_on;
    int      vbv_cbr;           /* ABR target == maxrate: the buffer fills only as
 * fast as the target spends, so near-overflow must be spent down. Capped
 * VBR (maxrate > target) overflows harmlessly and must NOT spend up. */
    double   rc_cplx;           /* complexity of the frame being coded (shared) */

    /* 2-pass rate control (YAH264_RC_2PASS). Pass 1 codes at a fixed QP and appends a
 * per-frame (type, complexity, bits, coded-QP) record; pass 2 reads them,
 * allocates the target bits proportional to complexity^qcomp, and sets each
 * frame's coded QP directly (frame_qp returns it unchanged in pass 2). */
    int      tp_pass;           /* 0 off, 1 analysis, 2 final */
    FILE    *tp_fp;             /* pass 1: stats output */
    struct tp_stat { int type; double cplx; double bits; int qp; int is_ref; } *tp_stats;
    int      tp_n, tp_idx;      /* pass 2: record count and cursor */
    double   tp_target;         /* pass 2: target total bits */
    double   tp_sum_cq;         /* pass 2: sum of complexity^qcomp */
    double   tp_rem_target;     /* pass 2: budget left for uncoded frames */
    double   tp_rem_cq;         /* pass 2: sum of complexity^qcomp of uncoded frames */
    double   tp_cur_cq;         /* pass 2: current frame's complexity^qcomp */

    /* Offline pass-2 plan (Y264_TP_PLAN): x264's <reference-internal> -- one global rate
 * factor bisected so the whole slice's modelled bits equal the target, I/B
 * qscales forced off the P qscale, and a BOUNDED runtime correction against
 * the plan's own expected-bits curve. A greedy remaining-budget split
 * instead has no frame-type relation (so it inverts the cascade) and no
 * bound (so it runs away). */
    int      tp_plan_on;        /* master gate */
    int      tp_difflim;        /* force I/B qscale off P (<reference-internal>) */
    int      tp_corr;           /* runtime correction against the plan */
    int      tp_resolve;        /* correct by re-solving the remaining curve */
    int      tp_dbg;            /* Y264_TP_DBG: per-frame plan trace */
    int      tp_rctrace;        /* Y264_RC_TRACE on the rcp commit path */
    double   tp_bexp;           /* <reference-internal> exponent (1.0 = the baseline model) */
    double   tp_cplxblur;       /* complexity blur radius in frames, 0 = off */
    double   tp_qblur;          /* qscale blur radius in frames, 0 = off */
    double   tp_ipf, tp_pbf;    /* qscale ip / pb factors */
    double  *tp_cost;           /* per record: QP-invariant cost, bits at qscale 1 */
    double  *tp_qrec;           /* per record: pass-1 qscale */
    double  *tp_q;              /* per record: planned qscale */
    double  *tp_ebits;          /* [n+1] modelled cumulative bits before record i */
    double   tp_fps;            /* frames per second, for the drift band */
    int      tp_cwarm;          /* accounted frames before the model-bias term engages */
    int      tp_nacc;           /* frames accounted so far */
    double   tp_actual;         /* bits coded so far (committed) */
    double   tp_ebsum;          /* modelled bits of those frames at their coded QP */

    /* Deterministic fixed-lag RC feedback (Y264_RC_PIPE, default on;
 * docs/rc-parallel-design.md). Lets ABR/2-pass ride the frame pipeline:
 * a frame's QP decision reads the committed ledger plus PREDICTIONS for
 * the in-flight frames; actuals commit on a schedule keyed purely to
 * decide order (a burst pops right after the next anchor's decision), so
 * output is identical at every thread count and engagement level. */
    int      rcp_on;            /* env on && (abr_on || tp_pass ||
 * (vbv_on && Y264_RC_PIPE_VBV)) */
    struct rcp_pend {
        int8_t  type, is_ref, dropped, filled;
        int8_t  tight;          /* decided in a serial-tight VBV burst: pops at
 * fill (the warm-phase schedule), so the next
 * decide reads its ACTUAL bits */
        int     fqp;            /* coded QP at decide (accounting model input) */
        double  qoff;           /* RF2: the frame's mean mb-tree QP offset at decide */
        int     base_qp;        /* e->qp at decide (predecided-fallback restore) */
        double  rceq;           /* ABR: rate-compressed complexity at decide */
        double  cplx;           /* pass-1 stat complexity */
        double  cq;             /* pass 2: complexity^qcomp share */
        int     tp_rec;         /* pass 2 plan: stats record index, -1 if none */
        double  pred;           /* predicted bits at decide */
        double  vpred;          /* VBV-model predicted bits at decide (virtual
 * buffer advance; error == VBV model error) */
        double  vcplx;          /* VBV-model complexity (lowres intra sum) */
        double  bits;           /* actual coded bits (valid when filled) */
        unsigned seq;           /* decide sequence number */
        /* Capacity: one entry per frame decided and not yet accounted. At the
 * zero-lag schedule that is one burst (<= 8) plus the next anchor plus
 * a W2 pending, which 24 covers. Under Y264_RCP_LAG the
 * ring holds up to K bursts at once, so it is K*8 + the W2 pending, and
 * an entry that does not fit is silently not pushed -- which would
 * slide every later fill onto the wrong frame rather than fail loudly.
 * stair_run_burst drains on the same bound as a second line of defence. */
    } rcp[Y264_STAIR_K * 8 + 8];
    int      rcp_head, rcp_n;   /* FIFO of decided-but-unaccounted frames */
    _Atomic unsigned rcp_seq;   /* decide counter. Atomic for ONE reader: the
 * arrival-side stair_ready warm check runs
 * before the serial_done handshake while the
 * driver's chain decides increment it; the
 * warm threshold is monotonic and long past
 * by the time a chain can be in flight, so
 * the racing read's VALUE is stable -- the
 * atomic is for the memory model, not the
 * schedule. */
    unsigned rcp_anchor_seq;    /* seq of the newest non-B decide (pop key) */
    unsigned rcp_anchor_hist[Y264_STAIR_K];
                                /* the last K non-B decide seqs, newest first.
 * Y264_RCP_LAG n pops against the anchor n
 * decides back, so the lag is a coding-order
 * fact rather than "whatever has finished".
 * Reset at open only: a decide sequence is
 * monotone across IDRs like rcp_seq itself. */
    int      rcp_predecided;    /* entries already pushed for frames a bailed
 * prep hands back to the serial path */
    int      rcp_cal[3];        /* per-type: a real calibration replaced the
 * open-time seed (first measurement snaps,
 * later ones EMA -- the burst lag would drag
 * a bad seed across ~2x more frames) */
    double   rcp_bcplx[8];      /* buffered B lowres complexity (at arrival) */
    double   rcp_arr_cme;       /* arriving frame's lowres complexity */
    double   rcp_cur_cme;       /* complexity for the frame being decided */
    /* The VBV model's complexity input is the arrival-captured lowres INTRA
 * sum (pure source energy) -- NOT the cme min(intra, inter), which
 * collapses to ~0 on static/noise content (a 3 Mbit sparkle anchor read
 * cme ~tiny: the model went blind exactly where VBV must see). Captured
 * and plumbed alongside cme at every decide site. */
    double   rcp_bcvi[8];       /* buffered B lowres intra sum (at arrival) */
    double   rcp_arr_cvi;       /* arriving frame's lowres intra sum */
    double   rcp_cur_cvi;       /* VBV complexity for the frame being decided */

    /* VBV under the pipeline (Y264_RC_PIPE_VBV, default on;
 * docs/rc-parallel-design.md). The buffer ledger e->vbv_fill
 * advances ONLY on actuals (at pops); decides see a virtual buffer that
 * charges each in-flight entry a conservative r_hi * vpred. The per-burst
 * fallback trigger (rcp_vbv_gate, at anchor arrival on the API thread with
 * everything drained -- pure function of decided state) sets rcp_vbv_tight:
 * a tight burst runs the serial K=0 schedule (stair/fpipe disengaged,
 * rc_waits forced, entries pop at fill), i.e. exact serial VBV behavior. */
    int      rcp_vbv_tight;     /* current burst decides on actuals, serially */
    double   rcp_vbv_scale[3];  /* per-type bits*qscale per unit complexity
 * (rcp decide-complexity domain; the serial
 * single vbv_scale stays untouched) */
    int      rcp_vbv_cal[3];    /* per-type: first calibration snaps, later EMA */
    double   rcp_vbv_sscale;    /* SHARED cross-type scale (serial's shape), same
 * lowres domain: a regime shock (a 1 Mbit IDR at
 * a missed cut) snaps it up, so the very next
 * B's clip prediction inflates too. Predictions
 * use max(per-type, shared) -- per-type accuracy
 * in steady state, shared shock propagation at
 * shifts (conservative near the constraint). */
    int      rcp_vbv_scal;
    double   rcp_abr_calqp[3];  /* the same EMA for plain ABR, read only by the
 * Y264_RCP_LAG guard. Under the shipped zero-lag
 * schedule an anchor decides on actuals, so the
 * ladder is corrected every burst and needs no
 * regime bound; with lag it steps the full swing
 * limit several decides running before anything
 * lands, which is the VBV round's windup in a
 * mode that had never met it. */
    double   rcp_vbv_calqp[3];  /* per-type EMA of the ACCOUNTED coded QP: the
 * regime the scale is calibrated at. The bits
 * model cannot extrapolate far below it (a
 * static-content scale calibrated at skip-all
 * QPs under-predicts a low-QP frame 40x), so
 * pipelined B decides floor their QP at
 * calqp - Y264_VBV_QPD -- serial feedback
 * recovers per frame; a flying burst must not
 * dive blind. */
    double   rcp_vbv_calc[3];   /* per-type EMA of the accounted complexity:
 * the content regime; the burst gate goes
 * tight when a coming frame jumps Y264_VBV_CJUMP
 * past it (the missed-cut / fade-end shape) */
    double   rcp_vbv_shock;     /* decaying max of recent accounted overshoot
 * ratios (bits/vpred), floor 1: the clip
 * multiplies its prediction by it, so after a
 * regime shock the model is only trusted as
 * far as it has recently earned. ~1 in steady
 * state (no effect); decays 0.7/account. */
    /* Y264_VBV_STAT counters */
    int      rcp_vbv_nburst, rcp_vbv_ntight, rcp_vbv_nclamp;

    int frame_num;
    int idr_pic_id;
    int poc;
    int ref0_poc;               /* POC of the list-0 reference (past anchor) */
    int ref1_poc;               /* POC of the list-1 reference (future anchor) */
    int anchor_fn;              /* frame_num of the most recent anchor (for B frames) */

    /* Scratch RBSP buffer (pre-emulation) and the Annex-B output buffer. */
    uint8_t *rbsp;
    size_t   rbsp_cap;
    uint8_t *out;
    size_t   out_cap;

    /* Worst case per call. At depth 2 it is a deferred prior emit + ONE
 * drained staircase burst (anchor + up to 8 stashed B NALs) + this call's
 * own serial output -- drain(9) + flush_buffered_p(7) + the IDR (1) + a W2
 * pending (1) = 18, which 24 covers.
 *
 * Width K retires up to K bursts in ONE call (the terminal flush, and any
 * site whose meaning is "nothing may be in flight past here"), so the drain
 * term is K*9 = 27 at K=3 and the ceiling is 36. This is a real API-visible
 * bound, not a formality: a 24-entry array is hit exactly by three
 * bframes-7 bursts, and with no bounds check append_nal writes through
 * nal[24] into nal_count itself, so the encoder silently returns a short
 * stream with a success code. append_nal refuses past the end; this is
 * sized so it never has to. */
    yah264_nal_t nal[48];
    int           nal_count;

    int64_t frame_count;
    int     headers_done;

    /* In-frame row-wavefront pool (NULL unless Y264_WF_THREADS>1). Separate
 * from CLI GOP-parallelism (--threads) to avoid N*N oversubscription. The
 * first frame runs serial (wf_warmed=0) to initialise every lazy static
 * cache (CPU detect, config, tables) on one thread before any parallel frame
 * touches them -- otherwise those idempotent first-writes trip TSan. */
    struct ntp_pool *pool;
    /* Optional nextgpu backend for the lookahead's lowres integer search
 * (src/encoder/gpu.h). NULL whenever the library is not linked or
 * Y264_GPU_LOWRES is not armed; Metal availability is probed lazily at
 * the first batch begin (gpu.h), and the CPU search stays complete
 * and default. */
    struct y264_gpu *gpu;
    struct y264_gpu *gpu_warm;  /* the pre-thread's own handle: the warm and the
 * NEXT walk overlap by design (the warm joins at
 * the launch AFTER that walk), so sharing one
 * stateful handle would race. Two handles are
 * two independent Metal queues. */
    struct y264_gpq *gpq;       /* per-push Phase-A leg batching (Y264_GPU_PHASEA,
 * gpu.h): the chain submits SEARCHQ legs at push,
 * the walk/warm consume fields frames later. */
    int              wf_warmed;

    /* Thread-scaled clamp: the staircase's row-gate margin and vertical MV
 * clamp, computed once at open from height_in_mbs and the pool width by
 * stair_lag_for (encoder.c) -- the same device x264 uses to bound its
 * inter-thread MV range. Never
 * below Y264_STAIR_LAG (me.h), the tested-sound floor -- see the soundness
 * note on yah264_stair_lag_for. Fixed for the life of one encoder_open,
 * so same config + same --threads always gives the same value (single-run
 * determinism); a DIFFERENT --threads may legitimately choose a different
 * value and therefore different bits, which is permitted by policy
 * (docs/advantages.md).
 * stair_mvy_max = 4*(16*stair_lag-24), the runtime twin of me.h's
 * Y264_STAIR_MVY_MAX macro (which holds the floor's own value). */
    int              stair_lag;
    int              stair_mvy_max;

    /* The pool width this open WILL build, resolved before the pool itself
 * because dpbp_open's sizing rule (and therefore the width-engagement
 * predicate it reads) runs earlier in encoder_open than ntp_pool_create.
 * 0 or 1 means no pool. A pool that then fails to come up leaves this
 * standing but zeroes rcp_lag, so nothing runs wide with no pool. */
    int              wf_width;

    /* The rate-control burst lag (Y264_RCP_LAG) as this instance
 * actually applies it -- the env value when this
 * configuration could ever run a wide ring, 0 when it could not.
 * Resolved once in encoder_open from STATIC configuration only (env,
 * params, pool width), never from runtime engagement state, so bits stay
 * a function of (input, config, --threads) and single-run determinism at
 * a fixed thread count holds. See stair_wide_capable in encoder.c. */
    int              rcp_lag;

    /* The launch-split PROBE (env Y264_ABR_EARLY; mode 1 is the probe, the
 * shipped default is 2, the drain split, and 0 restores the prologue drain). Unsafe by
 * construction: it drops the zero-lag prologue drain so the anchor's jobs
 * register before its predecessor is retired, which means the decide runs
 * on a FIFO missing that predecessor's actuals. Measurement only -- it
 * bounds what drain placement is worth and is never a shipping path. See
 * abr_early_env in encoder.c. */
    int              abr_early;

    /* W2 emit-overlap (env Y264_W2, on whenever there is a pool). Frame N's entropy emit runs on
 * a background thread (bg) concurrent with frame N+1's analyze. Recon never
 * reads entropy output, so emit can trail; only the deferred NAL-append + RC
 * accounting wait on it. Two snapshot gens ping-pong: analyze writes the
 * shared e-> grids as usual, then the emit-read grids are copied into gen[g]
 * and the trailing emit reads from there, freeing the next analyze to clobber
 * the shared grids. Off by default -> the whole pipeline is bypassed and the
 * serial path stays byte-identical. */
    int      w2_on;
    ntp_bg_t *bg;
    int      w2_gen;                 /* next gen index to use (toggles 0/1) */
    struct w2_gen {
        int8_t  *nnz[3];
        int8_t  *i4mode;
        int     *mbcbp;
        int16_t *mvx, *mvy, *mvx1, *mvy1;
        int8_t  *refidx, *refidx1;
        int16_t *mvdx, *mvdy, *mvdx1, *mvdy1;
        uint8_t *mb_tr8;             /* NULL unless transform8x8 */
        int8_t  *aq_off;            /* per-MB AQ QP offset (NULL unless AQ on) */
        int8_t  *mbtree_off;        /* per-MB mb-tree QP offset (NULL unless mb-tree) */
        uint8_t *rbsp;              /* this gen's pre-emulation bitstream buffer */
    } gen[2];
    /* The one in-flight (or just-analyzed) frame awaiting emit + append + RC. */
    struct w2_pending {
        int active;
        y264_emit_job_t *job;
        y264_frame_t f;             /* emit's private frame (grids -> gen) */
        y264_bs_t    bs;            /* CAVLC writer / CABAC header writer */
        y264_cabac_t cb;            /* CABAC arithmetic engine (unused for CAVLC) */
        int cabac, geni;
        uint8_t *rbsp;              /* gen[geni].rbsp (bitstream to append) */
        uint8_t *bs_start;          /* bs.start captured for the size computation */
        int ref_idc, nal_type;
        /* deferred RC accounting (mirrors emit_frame's tail) */
        int rc_type, rc_qp, is_ref;
        double rc_bits_qp;         /* frame_qp(type,is_ref) captured for account */
        double rc_cplx;
    } pipe;

    /* Frame pipeline (env Y264_FPIPE): the two non-reference sibling B leaves
 * of a bframes>=3 mini-GOP encode concurrently as jobs on the SHARED
 * multi-frame pool, each with fully private frame state (rec/grids/
 * colmv/cabac/bitstream). Created
 * lazily the first time a leaf pair engages, so GOP-parallel CLI workers
 * that never hit one stay lean.
 * fp_state: 0 = unresolved, 1 = ready, -1 = unavailable (small pool / OOM /
 * env off at open). */
    int                fp_state;
    ntp_bg_t          *fp_bg;       /* drives the second leaf; first runs inline */
    struct fpipe_leaf *fp_leaf[2];

    /* Staircase (env Y264_STAIR, default on): the reference-frame staircase.
 * A mini-GOP's P anchor and its buffered B's encode as concurrent jobs on
 * the ONE shared multi-frame pool, each B row's CLAIM gated on the anchor's
 * published CONSUMABLE rows (analyzed + deblocked + border-extended +
 * hpel-built + colmv-committed). Bits are governed ONLY by the env-gated fixed list-1 vertical
 * MV clamp (thread-count-invariant); the concurrency engages
 * opportunistically and never changes output. State/ctx in encoder.c.
 * st_state: 0 = unresolved, 1 = ready, -1 = unavailable. */
    int                st_state;
    struct stair_ctx  *st;

    /* Decoupled lookahead thread (env Y264_LA_THREAD, default off): the
 * per-frame lookahead chain (push-time lowres analysis, the deferred
 * finalize, the anchor/B-pair lowres ME) runs on ONE dedicated thread in
 * push order; the API thread pads + enqueues and every consumer read of a
 * chain output blocks on the producing step first. Pure handoff of WHO
 * computes -- values and order are identical by construction. State/ctx
 * in encoder.c. la_prev_pushed is the chain-side "entry pushed just
 * before this one" (the finalize target), owned by whoever runs chain
 * steps (the la thread when engaged, the API thread otherwise/at flush). */
    int                la_th_on;
    /* Y264_LA_INLINE resolved: run the chain's tiny per-push parallel_fors on
 * the calling thread instead of fanning them into the pool. Auto follows
 * la_th_on. Per-open constant, and byte-identical either way. */
    int                la_inline;
    struct la_thread  *la_th;
    struct la_entry   *la_prev_pushed;

    /* mb-tree prefetch (env Y264_MBT_PRE, default off): the anchor's
 * compute_mbtree runs on a dedicated thread, launched at the pop of the
 * last buffered B of its mini-GOP -- one encode call before the anchor
 * needs it -- instead of on the GOP driver at the anchor's own pop.
 * Everything it reads (the B copies in bplane, the ring window, the
 * anchor's own entry) is already final at that point and is not mutated
 * before the latch, so it is the same values in the same order: WHO
 * computes changes, nothing else. State/ctx in encoder.c. */
    struct mbt_pre    *mbtp;
};

#endif /* YAH264_ENCODER_INT_H */
