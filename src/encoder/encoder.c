/*
 * encoder.c - Phase 0 encoder: SPS/PPS plus IDR frames coded as I_PCM
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * I_PCM carries raw samples, so the output is a lossless copy of the input in a
 * valid H.264 elementary stream. It exercises the whole bitstream/NAL/slice
 * path and decodes bit-exact in any conformant decoder. Real intra coding
 * replaces this in Phase 1.
 */
#include <limits.h>
#include "encoder.h"
#include "../hw/vt.h"
#include "../dsp/mc.h"
#include "me.h"
#include "cabac.h"
#include "gpu.h"
#include "cavlc.h"
#include "deblock.h"
#include "sei.h"
#include "../common/nal.h"
#include "../common/cpu.h"
#include "../common/threadpool.h"
#include "../common/ledger.h"
#include "../dsp/transform.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

/* --- Y264_THREAD_PROF: attribute single-GOP wall-time to serial vs parallel
 * stages. Buckets accumulate main-thread wall-time; TP_ANALYZE is the wavefront
 * (parallel), the rest are serial per-frame stages. Printed at encoder close. */
/* Two rules hold for every bucket here: a bucket never pools a wait with the
 * compute it waits on, and it never pools two waits on unrelated producers.
 * Breaking either one makes the total unreadable -- summing an emit sync with
 * stair_drain's join on the chain DRIVER (which holds the burst's B analyze,
 * i.e. compute) reads as emission when it is not. Hence `emit_sync_wait` (emit
 * only) and `stair_chain_join` (compute) are separate, and `mbtree_warm_join`
 * is separate from `compute_mbtree`. */
enum { TP_LOWRES, TP_LOOKME, TP_MBTREE, TP_MBTWARM, TP_MBTWAIT, TP_CPLX, TP_PREP,
       TP_ANALYZE, TP_EMIT, TP_DEBLOCK, TP_DPBSTORE, TP_BORDERS, TP_NAL,
       TP_EMITWAIT, TP_STAIRJOIN, TP_PAD, TP_LAWAIT, TP_NUM };
static const char *const g_tprof_name[TP_NUM] = {
    "lowres_analyse", "lookahead_me", "compute_mbtree", "mbtree_warm_join",
    "mbtree_chain_wait", "frame_complexity",
    "slice_prep(hpel)", "analyze(WAVEFRONT)", "entropy_emit", "deblock",
    "dpb_store(hpel+copy)", "extend/copy_planes", "append_nal",
    "emit_sync_wait", "stair_chain_join", "pad_input", "la_chain_wait" };
static double g_tprof[TP_NUM];
/* emit_sync_wait by caller (THREAD_PROF only): which drain paid the wait.
 * 0-2 emit_frame_w2 (pre-decide / pre-reuse / post-submit), 3 leaf parent,
 * 4 early anchor fill, 5 stair_drain, 6 burst arrival, 7 end of stream,
 * 8-12 the staircase's B emit drains. */
static int g_ew_site; static double g_ew[16]; static long g_ewn[16];
static double ew_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static long g_enc_calls, g_enc_null_calls;   /* Y264_THREAD_PROF: how the API is driven */
static double g_enc_inside_ms;
static int yah264_encoder_encode_inner(yah264_encoder_t *e, yah264_nal_t **nal, int *count, const yah264_picture_t *pic);
static int g_cfg[12];                         /* Y264_THREAD_PROF: the resolved configuration at open */
static int g_tprof_on = -1;
static double g_tprof_wall0;
/* Per-bucket POOL-EMPTY attribution: how much of each driver stage ran with no
 * live pool job, i.e. how much of the machine that stage held idle. The bucket
 * wall alone cannot say -- a stage that overlaps the wavefront costs the same
 * milliseconds and costs the schedule nothing -- so reading the walls as an
 * idle measure points at the wrong term. Needs
 * Y264_NTP_PROF as well as Y264_THREAD_PROF (the pool's empty clock only runs
 * under its own profiler); the pool is registered by the first encoder to
 * create one, so a GOP-parallel run attributes against that worker's pool. */
static double g_tprof_empty[TP_NUM];
static ntp_pool_t *g_tprof_pool;
static double tprof_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}
/* la-thread chain time, folded at its join. One encoder per GOP worker, so
 * with Y264_LA_THREAD engaged several workers fold into this pair at once --
 * a real (if profiling-only) race that TSan reports 3 runs out of 3. The fold
 * is two adds per encoder at close, so a plain mutex costs nothing. */
static double g_tprof_la[2];
static pthread_mutex_t g_tprof_la_mx = PTHREAD_MUTEX_INITIALIZER;
static int tprof_on(void)
{
    if (g_tprof_on < 0) {
        const char *s = getenv("Y264_THREAD_PROF");
        g_tprof_on = s && atoi(s) ? 1 : 0;
        if (g_tprof_on) g_tprof_wall0 = tprof_ms();
    }
    return g_tprof_on;
}
#define TPROF(bucket, ...) do { \
    if (tprof_on()) { double _t0 = tprof_ms(), _e0 = ntp_pool_empty_ms(g_tprof_pool); \
        __VA_ARGS__; g_tprof[bucket] += tprof_ms() - _t0; \
        g_tprof_empty[bucket] += ntp_pool_empty_ms(g_tprof_pool) - _e0; } \
    else { __VA_ARGS__; } \
} while (0)
/* Carve an inner span out of the bucket already timing the enclosing block, so
 * the two do not double-count and SERIAL total stays a sum. Use it wherever a
 * compute bucket would otherwise swallow a wait. */
#define TPROF_MOVE(from, to, ...) do { \
    if (tprof_on()) { double _t0 = tprof_ms(), _e0 = ntp_pool_empty_ms(g_tprof_pool); \
        __VA_ARGS__; double _d = tprof_ms() - _t0; \
        double _de = ntp_pool_empty_ms(g_tprof_pool) - _e0; \
        g_tprof[to] += _d; g_tprof[from] -= _d; \
        g_tprof_empty[to] += _de; g_tprof_empty[from] -= _de; } \
    else { __VA_ARGS__; } \
} while (0)
/* The lookahead chain's two buckets (lowres push analysis, lookahead ME) can
 * run on the dedicated la thread (Y264_LA_THREAD). Their time is then NOT
 * API-thread serial time: accumulate it chain-locally (chidx 0/1) and fold it
 * into a separate profile line at the thread join, so the dump shows the
 * arrival path's real serial cost -- and so the la thread never writes the
 * g_tprof doubles the API thread is concurrently adding to. */
static _Thread_local int s_la_chain_tls;    /* set for the la thread only */
#define TPROF_LA(e, bucket, chidx, ...) do { \
    if (tprof_on()) { double _t0 = tprof_ms(); __VA_ARGS__; double _d = tprof_ms() - _t0; \
        if (s_la_chain_tls) (e)->la_th->ms[chidx] += _d; else g_tprof[bucket] += _d; } \
    else { __VA_ARGS__; } \
} while (0)

/* --- decoupled lookahead thread (Y264_LA_THREAD, default OFF) ---
 *
 * THE PURITY CLAIM: the per-frame lookahead chain --
 * la_chain_step's push analysis (downscale + la_lr_row) and la_finalize
 * (deferred scene-cut + flash test, b-adapt typing, lowres_anchor_me /
 * lowres_bleg_me) -- is a pure function of (the padded input frames, its own
 * prior chain state). Its inputs are the ring entries' plane/lowres/d_intra/
 * leg fields plus the chain-owned e->la_{lr_prev, have_prev, fin_prev_lr,
 * have_prev_fin, since_idr, brun, anchor_lr/have/poc, anchor_mvx/mvy/
 * mv_have} and e->lr_subpel scratch, plus read-only config and warmed env
 * statics. Nothing in it reads encode-side state: code_panchor_lr (written
 * at mbtree time from the encode side) is read only by compute_mbtree /
 * mbt_pa_source, which stay on the consumer; the mbt_pa_* memos are written
 * and read only consumer-side (under a full-chain wait).
 *
 * So the chain runs on ONE dedicated thread, processing pushes in order; the
 * API thread pads into the ring slot and enqueues, and every consumer read
 * of a chain output blocks on the producing step first:
 * - slot reuse at pad: step s-la_cap (the recycled slot's own analysis
 * is the only chain read of en->plane; la_cap is the ring's rotation
 * period, la_depth + Y264_LA_BUF's extra capacity);
 * - pop (flags, stash_lr_seed, the lr-reuse copy): step pop+bframes+2,
 * the popped entry's last chain writer (its future anchor's finalize
 * fills the B pair legs at most bframes+1 pushes after it, +1 margin);
 * - compute_mbtree's window walk (reads typed/legs up to the newest push
 * the WINDOW is capped to -- la_depth-2 entries past the anchor, exactly
 * as the serial order does; Y264_LA_BUF's extra buffered entries sit
 * further back in the ring and are never read by this walk, which is
 * what lets the chain run ahead of it):
 * the capped chain, not the full one when Y264_LA_BUF > 0;
 * - flush / close: full drain, then the tail finalizes run consumer-side
 * with the chain idle (identical order).
 * Every value is computed from identical inputs in identical order -- only
 * WHO computes it changes -- so the output is byte-identical by
 * construction, and byte-identity vs HEAD is the gate.
 *
 * The chain's pool jobs (lookahead_lr rows, the fme leg batches) register on
 * the shared v2 job table from this thread, riding alongside any in-flight
 * burst's jobs; per-submitting-thread lanes keep their scratch private, and
 * a full table blocks the chain (never the workers), so no cycle exists. */
struct la_thread {
    ntp_bg_t       *bg;             /* the dedicated thread (one long task) */
    pthread_mutex_t mx;
    pthread_cond_t  cv_push;        /* chain waits for enqueued steps */
    pthread_cond_t  cv_done;        /* API waits for chain progress */
    long            pushed;         /* steps enqueued (written by API only) */
    long            done;           /* steps completed (mx) */
    _Atomic long    done_atom;      /* mirror for the waiters' fast path */
    int             exit;
    long            pop_seq;        /* consumer-side pops (API thread only) */
    struct la_entry *q[Y264_LA_CAP_MAX]; /* entry of push s at q[s % cap]; lag
 * is bounded by la_cap (pad wait) */
    double          ms[2];          /* chain-side TP_LOWRES/TP_LOOKME time */
};
static void la_th_main(void *arg);

/* Everything compute_mbtree needs that is NOT config, so the same computation
 * can run at the anchor's own pop on the GOP driver (head = la_head, anchor
 * already popped, anchor_dintra = NULL meaning "use e->lr_intra") or one
 * encode call earlier on the prefetch thread (head = la_head+1, the anchor
 * still sitting at la_head). Passing these explicitly is what stops the
 * computation reading API-thread-owned ring cursors while the driver moves
 * them. Rationale for the prefetch itself: see struct mbt_pre below. */
struct mbt_req {
    pixel *const  *anchor_planes;   /* anchor's padded source planes (Y,Cb,Cr) */
    const pixel   *anchor_lr;       /* anchor's lowres */
    const int32_t *anchor_dintra;   /* anchor's lowres intra costs (NULL = e->lr_intra) */
    int            anchor_poc;
    long           anchor_push;     /* anchor's lookahead push index (gpq key) */
    int            head;            /* ring index of the FIRST window entry */
    int            navail;          /* window entries available forward from head */
    int8_t        *out_off;         /* nmb per-MB QP offsets */
    double        *out_mean;        /* mean offset (drives the CRF reclaim) */
};

/* mb-tree prefetch state; the WHY/WHEN/why-same-bits argument lives at
 * mbt_pre_launch, next to the conditions it turns on. */
struct mbt_pre {
    ntp_bg_t       *bg;
    pthread_mutex_t mx;
    pthread_cond_t  cv_req;         /* worker waits for a request */
    pthread_cond_t  cv_done;        /* driver waits for a result */
    int             req;            /* a request is pending or running */
    int             done;           /* result in out_off/out_mean is complete */
    int             ok;             /* the x264 path ran (0 = driver must redo) */
    int             exit;
    struct mbt_req  rq;             /* the pending request (driver writes, idle) */
    const struct la_entry *anchor;  /* identity: which ring entry it was for */
    int             anchor_poc;
    int             lead;           /* 1 = wait for the chain, don't test it */
    int             warm;           /* 1 = this thread runs Phase-A warms */
    int             kind;           /* 0 = whole-anchor prefetch, 1 = warm */
    int             warm_head, warm_navail;
    long            warm_popseq, warm_pushed;
    long            need;           /* the chain step the walk needs (lead mode) */
    int             anchor_idx;     /* ring index of the anchor (read after the wait) */
    int8_t         *out_off;
    double          out_mean;
    long            hits, misses;   /* Y264_MBT_PRE_DBG accounting */
    double          t_launch;       /* when the request was posted (dbg) */
    double          ms_gap, ms_chainwait, ms_compute, ms_latch;
};
static void mbt_pre_main(void *arg);
static void la_th_wait_step(yah264_encoder_t *e, long need);
static void la_th_wait_mbtree(yah264_encoder_t *e);

/* Y264_MBT_PRE=1: run the anchor's compute_mbtree on a dedicated thread, one
 * encode call ahead of its use. Byte-identical (see mbt_pre_launch).
 * DEFAULT OFF; resolved in warm_lr_statics. */
static int mbt_pre_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_PRE"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Y264_MBT_LEAD=1: the same prefetch thread, but it WAITS for the chain step
 * that makes the walk safe instead of testing for it and declining. That is the
 * whole difference between "moved to another single thread" and "given lead" --
 * see the argument at mbt_pre_launch. DEFAULT OFF; resolved in warm_lr_statics. */
static int mbt_lead_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_LEAD"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Y264_MBT_WARM: run mb-tree's Phase A ahead of the anchor that consumes it,
 * on the prefetch thread, one pass per anchor joined at the next. The argument
 * and the accounting are at mbt_warm_window. DEFAULT ON -- it takes the
 * driver's mb-tree bucket from 94.8 to 64.5 ms on samsung t18. Inert without an
 * off-driver chain to lead: mbt_warm_window returns at its first line when
 * e->la_th is null, which is why the thread it needs is only created when the
 * chain has one. Warmed in warm_lr_statics.
 *
 * "byte-identical by construction (it fills a memo earlier, it does not change
 * one)" is NOT sufficient on its own: if the warm and the walk disagree about
 * a settled-bounded input, which one runs a source changes its Phase-A output.
 * That defect emitted 2-6 distinct bitstreams per twelve runs of one
 * configuration, on four configurations. `scripts/determ_repeat.sh` finds it
 * ONLY on a loaded box -- an idle one passes. See the source-selection comment
 * in mbt_warm_window; the gate is that WARM=1 and WARM=0 produce the same
 * single md5 under load. */
static int mbt_warm_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_WARM"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Coherent lowres ME, Y264_LOWRES_COH (default on). Resolved up front rather
 * than lazily inside compute_mbtree_wholebuf: the Phase-A warm reads it from a
 * second thread, and two threads racing to fill a lazy static is a real bug
 * class. Warmed in warm_lr_statics. */
/* mb-tree offset precision (Y264_MBT_FRAC). x264 keeps a FLOAT frame QP and a
 * FLOAT per-MB offset and rounds ONCE, at clip3(qp + 0.5f) .
 * We round twice: the offset to whole QP at produce, then add it to an
 * already-integer frame QP. The frame-QP rounding is a per-frame constant and so
 * only shifts the operating point (harmless -- a CRF translation cannot move
 * BD), but the OFFSET rounding is per MB and
 * quantises the importance gradient to whole QP steps where x264's is
 * continuous. Under this gate the field is stored in HALF-QP units, which keeps
 * it in int8 (the +/-51 bound becomes +/-102, inside 127) and lets mb_qp_pre do
 * the single rounding. */
static int mbt_frac_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_FRAC"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Y264_MBT_BREF: reference B frames as propagation TARGETS with their own
 * offset field, and mb-tree offsets applied to them -- x264 keys the apply on
 * b_kept_as_ref where we applied to no B at all.
 *
 * Default ON. CRF band median -2.09% BD-NEG with 10 of 12 clips better (ducks
 * -8.86, coastguard -7.10, touchdown -6.25, park_joy -4.93); ABR median
 * -0.84%. It costs a median ~3% of wall, so it is a quality-for-speed trade.
 *
 * sintel is the one real regression, CRF +6.78%, reproducible. It is a
 * recurring outlier but NOT the ABR RC-state pathology: this shows up on the
 * CRF band, which carries no cumulative RC state. Unexplained. */
static int mbt_bref_probe(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_MBT_BREF"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}

/* Y264_MBT_BCEN: does the reference B's mb-tree finish add its own boost mean
 * back (1, shipped) or not (0, x264)? The anchor's finish runs uncentred, so
 * this term does not give the B the same centring F gets: it gives it a
 * systematic +bmean QP, about 2.4 at the shipped strength 1.4 and a mean ratio
 * near 1.75, on the frame the mini-GOP's leaves predict from. x264 centres
 * nothing on any frame, in its mb-tree finish pass.
 *
 * It defaults ON anyway, because the reference-B field was gated WITH it inside
 * (-2.09% median) and an unmeasured correction is not a free one. The x264
 * mode drops it as part of its unit; this knob is how it gets priced on the
 * SHIPPED field, which is a different measurement. */
static int mbt_bcen(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_BCEN");
                 v = s ? (atoi(s) ? 1 : 0) : (y264_mbt_derived() ? 0 : 1); }
    return v;
}

/* mb-tree AC gain (Y264_MBTREE_AC_GAIN, 1.0 = the unscaled field). Our
 * boost term correlates 0.78-0.92 with x264's per-MB field and carries 59% of
 * its spread (sd 1.15 vs 1.96), so the allocation points the right way and is
 * simply too flat. Raising `strength` was already refuted on 10 of 12 clips,
 * for this reason: strength scales the term's MEAN too, and the mean is the
 * anchor-versus-B split (B frames carry no mb-tree offsets at all). This
 * scales only the deviation from the frame mean, so it is a pure
 * redistribution -- 1.7 lands our sd on 1.926 against x264's 1.960 with the
 * mean held to two decimals.
 *
 * Default 1.7: CRF band median -0.70% BD-NEG, 10 of 12 clips better, worst bus
 * +1.10. Two independent derivations agree on the value (the measured sd
 * ratio, 1.70-1.80, and the sweep optimum). The ABR band's two objections are
 * inside those clips' own perturbation floors while its one trustworthy row is
 * samsung -2.10%. */
/* Under the x264 mode the gain is 1.0 AND the mean-hold goes with it: x264's
 * finish writes strength*log2_ratio straight, with no per-frame pivot of any
 * kind. The 1.7 is one half of a jointly-fitted pair (calibrated at aq 1.0
 * rather than the current 0.4, so it is stale for the board clips but
 * corpus-defended), which is exactly why it moves with the mode rather than on
 * its own. */
static double mbt_ac_gain(void)
{
    static double v = -1e9;
    if (v < -1e8) { const char *s = getenv("Y264_MBTREE_AC_GAIN");
                    v = s ? atof(s) : (y264_mbt_derived() ? 1.0 : 1.7); }
    return v;
}

static int mbt_coh(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_LOWRES_COH"); v = s ? atoi(s) : 1; }
    return v;
}

/* CRF content adaptation, Y264_CRF_CPLX, DEFAULT ON. MEASUREMENT TRAP: a
 * matched-CRF-NUMBER sweep (`bdcompare --points`) reads this gate as a large
 * LOSS (samsung +9.30%, touchdown +10.71%) because translating the CRF axis is
 * most of what it does; that is ladder placement, not quality. Gated with
 * `scripts/bd_at_rate.py` at matched achieved BITRATE, 12 clips, four targets
 * each: median -5.17%, 11 of 12 negative, worst +1.24% at the anchor below.
 * ducks -20.69, park_joy -11.86, tempete -8.32, bus -7.83. It is the largest
 * quality arm here by about a factor of three.
 *
 * Why it is that big: the whole quality gap to x264 is mb-tree, and the tell is
 * the intra frame's share of the bits -- theirs takes half of it away on the
 * clips we lose and ours does not move the split. This gate is exactly the DC
 * that moves it.
 *
 * Y264_CRF_CPLX=0 disables it.
 *
 * x264's CRF has no frame-level complexity term at all once mb-tree is on:
 * the rate equation drops the blurred complexity and the rate
 * equation becomes duration-only, so the base QP is the fixed pedestal
 * crf + 13.5*(1-qcomp). ALL of its content adaptation is the DC of the per-MB
 * offset field, and that DC exists because x264's variance AQ is anchored to an
 * ABSOLUTE constant, strength*(log2(ac_energy) - 14.427))
 * rather than to the frame mean. yah264 centres on the frame mean, which
 * removes exactly that DC -- which is why our CRF resolves to a flat crf+5.4.
 *
 * This gate turns on the two terms that differ, both behaviour-matched:
 * - the absolute AQ anchor (mbtree_invqscale), worth up to ~7 QP of spread;
 * - the frame-duration term in the base QP (rc_set_qp_crf), worth 0 at
 * 25 fps and +2.4 at 50.
 * Resolved in warm_lr_statics: read by the lookahead/mb-tree worker threads. */
static int crf_cplx_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_CRF_CPLX"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Per-term overrides, so the bundle can be attributed. Unset = follow the
 * master gate; 0/1 forces the term on its own. The mb-tree strength has
 * Y264_MBTREE_STRENGTH already and the B cascade has Y264_CRF_PBSCALE, so
 * these two complete the set. */
static int crf_aqabs_env(void)
{
    static int v = -3;
    if (v == -3) { const char *s = getenv("Y264_CRF_AQABS"); v = s ? (atoi(s) ? 1 : 0) : -1; }
    return v < 0 ? crf_cplx_env() : v;
}

/* True only when the knob was set EXPLICITLY, so the ABR round can arm the
 * absolute anchor outside CRF without the default flip doing it silently. */
static int crf_aqabs_forced(void)
{
    const char *s = getenv("Y264_CRF_AQABS");
    return s && atoi(s);
}

static int crf_fps_env(void)
{
    static int v = -3;
    if (v == -3) { const char *s = getenv("Y264_CRF_FPS"); v = s ? (atoi(s) ? 1 : 0) : -1; }
    return v < 0 ? crf_cplx_env() : v;
}

/* The CRF pedestal calibration, in QP. The absolute anchor replaces a
 * per-frame mean with a constant, so it shifts the whole corpus's operating
 * point as well as spreading it; this puts that level back where x264's is.
 * Separate from the anchor on purpose -- the anchor also feeds the mb-tree
 * intra-cost weight, so moving the level with it distorts the boost, while the
 * pedestal is linear and BD-neutral by construction.
 *
 * -3.0, and it is PAIRED WITH THE ANCHOR: the anchor sets how far the offsets
 * sit from zero, so re-tuning one re-levels the other. At anchor 7.5 the
 * pedestal wants -2.0 (mean +0.9% vs x264's size at equal CRF); at anchor 5.5
 * that same -2.0 runs 5-24% under and -3.0 restores it (7 clips: mean -1.1%,
 * spread -14.0%..+8.7%). The knob is coarse because the base QP is rounded to an
 * integer per frame where x264 keeps a float, so -2.0 and -2.5 land on the same
 * QP for most clips -- there is no rung between them.
 *
 * It is also the only part of the gate the BD table cannot see: a pure 1-QP
 * level move measures 0.00% BD-VMAF-NEG on ducks, samsung and foreman. */
/* Zero under the x264 mode: the pedestal exists to put OUR fitted anchor's
 * level back where x264's is, and the mode uses x264's anchor and x264's
 * strength, so there is nothing left to correct -- x264 has no such term. The
 * mode's level therefore lands wherever the faithful constants put it, which is
 * why its gate has to be a matched-BITRATE one (scripts/bd_at_rate.py) and not
 * a matched-CRF-number sweep. */
static double crf_ped_env(void)
{
    static double v = -1e9;
    if (v < -1e8) { const char *s = getenv("Y264_CRF_PED");
                    v = s ? atof(s) : (y264_mbt_derived() ? 0.0 : -3.0); }
    return v;
}

static int crf_pb0_env(void)
{
    static int v = -3;
    if (v == -3) { const char *s = getenv("Y264_CRF_PB0"); v = s ? (atoi(s) ? 1 : 0) : -1; }
    return v < 0 ? crf_cplx_env() : v;
}

/* Chroma in the AQ energy, Y264_AQ_CHROMA. The reference's per-macroblock
 * AC-energy term sums every plane, each as ssd - sum^2>>log2(npix),
 * i.e. npix*var, so a 4:2:0 MB's energy is 256*var_y + 64*var_u + 64*var_v.
 * Ours is luma only, which under-reads flat-but-chromatic content -- and it is
 * why the absolute anchor is fitted at 7.5 instead of the derived
 * 14.427 - 8. Folding chroma in makes the metric x264's, so the anchor becomes
 * derived (see aq_anchor_default).
 *
 * DO NOT expect it to pay. Chroma moves the mean of the metric by -0.26..+0.14
 * log2 units across the corpus, not the ~1 unit the fitted anchor might look
 * like it stands in for, and at a FIXED anchor it is worth +0.35..-0.29%
 * BD-VMAF-NEG on six CIF clips -- neutral, faintly negative. It costs ~9% of
 * the mb-tree bucket (+0.6% of single-thread wall at 720p, unmeasurable at 18
 * threads). So it stays off even under the master gate: behaviour-matched on
 * its own does not buy a measurable encode. */
/* The x264 mode arms it anyway, and not because it is expected to pay: it is
 * what makes the anchor DERIVED (14.427 - 8) instead of fitted, and the mode's
 * whole claim is that the constants are x264's rather than ours. */
static int aq_chroma_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_AQ_CHROMA");
                 v = s ? (atoi(s) ? 1 : 0) : (y264_mbt_derived() ? 1 : 0); }
    return v;
}

/* The absolute AQ anchor in yah264's metric. x264 offsets the log2 of a
 * block's AC energy against a fixed anchor (14.427 at 8 bits, plus 2 per
 * extra bit of depth); our metric is the same energy scaled down by the 256
 * luma pixels (log2 energy - 8), so substituting the constant gives
 * 6.427 + 2*(depth-8).
 *
 * DO NOT SUBSTITUTE IT. The anchor only ever appears as strength*(l - anchor),
 * and yah264's aq_strength shipped at 0.3 when this was derived (0.4 today)
 * where x264's is 1.0 -- so the same constant produces a third of x264's
 * offset. Matching the offset instead of the constant,
 * 0.3*(m - A) = 1.0*(m - 6.427), puts A at 21.42 - 2.333*m, which
 * is ~3.3 at this corpus's mean metric m ~ 7.8.
 *
 * And the anchor is not merely a level knob, which is why this matters. A
 * uniform shift in the offsets IS BD-neutral (measured: a 1-QP pedestal move
 * reads 0.00% on ducks/samsung/foreman). The anchor is not uniform, because
 * aq_fold feeds the UNNORMALISED intra weight Fw = 2^(-aq_fold/6) in
 * compute_mbtree_wholebuf, so the mb-tree ratio log2(1 + prop/(intra*Fw)) tilts
 * toward propagated importance as the anchor drops. That tilt is worth ~5% of
 * BD per anchor unit and it has an interior optimum: swept 8.5 -> 2.5 it turns
 * over at 4.5-5.5 on every clip measured.
 *
 * At a matched CRF NUMBER that sweep lands on 5.5 (11 clips: 9 better, worst
 * +1.54%; anchor 7.5 reads -8.01% on ducks but +7.93% on samsung, i.e. the
 * samsung regression is the anchor sitting 2 units too high, not the missing
 * chroma). But a matched-CRF-number sweep is exactly the measurement this gate
 * invalidates. */
/* The shipped value is 4.5, swept with bd_at_rate.py at matched achieved
 * bitrate (3.5 / 4.5 / 5.5 / 7.5 / 9.5 on akiyo, samsung and bus; then the full
 * corpus at 4.5 and 5.5):
 *
 * anchor median mean negative worst
 * 5.5 -5.43% -5.36% 10/12 +2.55%
 * 4.5 -5.17% -6.26% 11/12 +1.24%
 *
 * 4.5 wins on eight of the twelve, has the better mean and the better WORST
 * case, and turns samsung -- the clip that sets the board's max -- from +1.16%
 * into -0.92%. Above 5.5 it degrades fast (samsung +9.85% at 7.5, +24.55% at
 * 9.5), so the optimum is a genuine turn and not a plateau edge.
 *
 * The anchor is not a level knob, which is why a level-cancelling BD at matched
 * rate still sees it: it reaches the bitstream through aq_off AND through the
 * unnormalised intra-cost weight in the mb-tree finish, so it sets how much a
 * clip's own energy damps its mb-tree boost. The PEDESTAL is the level knob and
 * is BD-neutral at matched rate by measurement (+0.00 to +0.45% across
 * -2.0/-3.0/-4.0 on bus and ducks), which is why sweeping the anchor alone is
 * sound here even though the two are paired for the CRF SCALE match. */
/* The x264 mode takes the substitution this comment spends 40 lines refusing --
 * 14.427 - 8 = 6.427 -- and it is consistent there for the reason the refusal
 * gives: the substitution fails at aq_strength 0.4 because the anchor only ever
 * appears as strength*(l - anchor), and the mode restores the 1.0 the constant
 * was derived against. Taking either alone is the refused move. */
static double aq_anchor_default(void)
{
    return (y264_mbt_derived() ? 6.427 : 4.5) + 2.0 * (Y264_BIT_DEPTH - 8);
}

static double aq_anchor_env(void)
{
    static double v = -1e9;
    if (v < -1e8) { const char *s = getenv("Y264_AQ_ANCHOR"); v = s ? atof(s) : aq_anchor_default(); }
    return v;
}

/* B-frame QP cascade scale under the gate. x264 forces the pbratio to 1
 * whenever mb-tree is on, so its pb_offset is 0 and a B
 * codes at the anchor's base QP: its whole B economy is that non-reference B's
 * miss the mb-tree offsets, which yah264 already reproduces. Our extra 1..d+1
 * cascade therefore double-counts, and since B's are ~3/4 of the frames it is
 * most of the residual under-spend at equal CRF. 0 = x264 -- but x264's value
 * is not ours to take: dropping the cascade is worth -8.92% BD on ducks and
 * +7.00% on sintel, a bidirectional trade with no net, so the default keeps
 * yah264's cascade and this stays a probe.
 *
 * The x264 mode takes the 0. The bidirectional trade above is measured against
 * OUR mb-tree field; under x264's field and x264's strength the B's inherit a
 * different offset distribution, so the cascade's compensation is priced
 * against a different thing. Whether it still costs ducks 8.9% is one of the
 * questions the mode's gate answers, and Y264_CRF_PBSCALE=1 splits it out. */
static double crf_pbscale_env(void)
{
    static double v = -1e9;
    if (v < -1e8) { const char *s = getenv("Y264_CRF_PBSCALE");
                    v = s ? atof(s) : (y264_mbt_derived() ? 0.0 : 1.0); }
    return v;
}

static void tprof_dump(void)
{
    if (!tprof_on()) return;
    double wall = tprof_ms() - g_tprof_wall0;
    double ser = 0;
    for (int i = 0; i < TP_NUM; i++) if (i != TP_ANALYZE) ser += g_tprof[i];
    double emp = 0;
    for (int i = 0; i < TP_NUM; i++) emp += g_tprof_empty[i];
    fprintf(stderr, "\n=== Y264_THREAD_PROF (wall %.1f ms) ===\n", wall);
    fprintf(stderr, "  api: %ld encode calls, %ld with pic == NULL (flush polls), %.1f ms inside the calls\n", g_enc_calls, g_enc_null_calls, g_enc_inside_ms);
    fprintf(stderr, "  cfg: threads %d wf_width %d la_depth %d la_buf %d sync_lookahead %d rcp_lag %d abr_early %d bframes %d keyint %d rc.lookahead %d fps %d/%d\n",
            g_cfg[0], g_cfg[1], g_cfg[2], g_cfg[3], g_cfg[4], g_cfg[5], g_cfg[6], g_cfg[7], g_cfg[8], g_cfg[9], g_cfg[10], g_cfg[11]);
    fprintf(stderr, "  %-20s %9s  %5s   %9s\n", "", "ms", "", "pool-idle");
    for (int i = 0; i < TP_NUM; i++)
        fprintf(stderr, "  %-20s %9.1f ms  %5.1f%%   %8.1f ms\n",
                g_tprof_name[i], g_tprof[i], 100.0 * g_tprof[i] / wall,
                g_tprof_empty[i]);
    fprintf(stderr, "  %-20s %9.1f ms  %5.1f%%   %8.1f ms  <- serial (non-wavefront)\n",
            "SERIAL total", ser, 100.0 * ser / wall, emp);
    for (int i = 0; i < 16; i++)
        if (g_ewn[i])
            fprintf(stderr, "    emit_sync_wait site %2d: %8.1f ms  n=%ld\n", i, g_ew[i], g_ewn[i]);
    fprintf(stderr, "  %-20s %9.1f ms  %5.1f%%\n",
            "unattributed", wall - ser - g_tprof[TP_ANALYZE],
            100.0 * (wall - ser - g_tprof[TP_ANALYZE]) / wall);
    if (g_tprof_la[0] > 0 || g_tprof_la[1] > 0)
        fprintf(stderr, "  %-20s %9.1f ms  (lowres %.1f + lookme %.1f; "
                "off the API thread, not in SERIAL)\n",
                "la-thread chain", g_tprof_la[0] + g_tprof_la[1],
                g_tprof_la[0], g_tprof_la[1]);
}

/* MT Lever 2 gate: encode the two non-ref sibling B leaves of a mini-GOP
 * concurrently. DEFAULT ON: the output is byte-identical to serial by
 * construction, every scenario measures >= 1.00x (true single-GOP 1.09-1.11x
 * @18t), and TSan/stress/determinism gates cover it.
 * Y264_FPIPE=0 restores strictly-serial leaves. Hoisted file-static resolved
 * in warm_lr_statics so worker threads only ever read it (TSan floor 0). */
static int fpipe_on_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_FPIPE"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Y264_WF_WARMSERIAL=1: serial first frame. DEFAULT OFF -- the lazy statics it
 * exists to warm are all resolved at open. Resolved in
 * warm_lr_statics. */
static int wf_warmserial(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_WF_WARMSERIAL"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* MT Lever 3 gate: the reference-frame staircase, Y264_STAIR, DEFAULT ON.
 * Resolved in warm_lr_statics (this is a function-local static, unreachable by
 * the warm pass unless called there). */
static int stair_on_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* MT Lever 3 v3: staircase DEPTH. >= 2 keeps the previous burst in flight
 * across the encode API boundary and starts the next anchor against its
 * per-row publish, adding the fixed P list-0 clamp (+ the wp source-sum and
 * P_Skip guard that make its reads bounded). DEFAULT 2; Y264_STAIR_DEPTH=1
 * leaves the bitstream untouched. Resolved in warm_lr_statics (function-local
 * static, warmed there). */
static int stair_depth_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_DEPTH"); v = s ? atoi(s) : 2; }
    return v >= 2;
}

/* MT Lever 3 v5: staircase B DEPTH. With it on, a mini-GOP's non-reference
 * leaves staircase against the still-in-flight REFERENCE B (which grows the
 * anchor's trailing consumability pipeline of its own) instead of waiting for
 * it to complete -- the measured binding constraint of the t18 wall, where the
 * burst span is the B chain serial-among-itself. It adds a fixed vertical
 * clamp on every leaf read of that reference B, in whichever list resolves to
 * it. DEFAULT ON (stair_depth_on returns v>=2 and v defaults to 2, so the
 * async chain is live). Rides Y264_STAIR
 * + Y264_STAIR_DEPTH (the async chain is what carries it). Resolved in
 * warm_lr_statics (function-local static, warmed there). */
static int stair_bdepth_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_BDEPTH"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* The mini-GOP shapes whose reference-B staircase is AUDITED (the leaf clamp
 * surface below covers exactly one in-flight reference B, reachable in either
 * list): a run of 2 or 3 buffered B's, whose flattened plan is
 * [ B(bpoc[1]) reference, leaf(s) ]. Deeper runs (bframes 4..7 full runs build
 * a nested pyramid with several reference B's) keep today's behaviour -- no
 * clamp, no pipelining. Returns the reference B's POC, or -1.
 *
 * Machine-invariant: a pure function of the burst's buffered-B shape, so the
 * clamp it keys is identical at every thread count and whether or not the
 * concurrency engages. */
static int stair_refb_poc(int nbuf, const int *bpoc)
{
    return (nbuf == 2 || nbuf == 3) ? bpoc[1] : -1;
}

/* MT stage 3: staircase WIDTH. With it on, stair_run_burst stops draining the
 * previous chain before it submits next one and defers the drain to the
 * point where the burst ring actually needs the slot back, so up to
 * Y264_STAIR_K chains execute at once instead of one. DEFAULT ON: at --ref <= 1
 * the bitstream is unaffected by construction (thread-invariant, byte-identical
 * to the serialized encode at every K, verified under TSan/ASan/flush-torture/
 * stress), so this is pure speed, not a quality decision. Y264_STAIR_WIDE=0 is
 * the escape hatch. Rides Y264_STAIR + Y264_STAIR_DEPTH (the async chain is
 * what carries it). Resolved in warm_lr_statics (function-local static, warmed
 * there). */
static int stair_wide_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_WIDE"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* MT stage 3 item 3: the MULTI-HOP list-0 clamp. Width can have K bursts live
 * at once, so at --ref > 1 a P anchor's list 0 can name more than one anchor
 * that is still streaming, and the single-hop clamp covers only the newest.
 * With this on, the clamp set reaches Y264_STAIR_HOPS anchors back instead of
 * one.
 *
 * Unlike the other stage-3 gates this one CHANGES BITS: a clamped search is a
 * smaller search. It is what makes lifting the --ref <= 1 restriction in
 * stair_run_burst safe. Resolved in warm_lr_statics. */
static int stair_multihop_on(void)
{
    /* DEFAULT ON: the deeper clamp is what makes ref>1 width safe, and its
     * measured CRF band cost is EXACTLY ZERO on all six ledger clips (the
     * clamp bound 5 list-0 references in a whole samsung encode). Rides with
     * Y264_STAIR_WIDE_REF below; Y264_STAIR_MULTIHOP=0 escapes. */
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_MULTIHOP"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Lift the --ref <= 1 restriction on width so a --ref > 1 shape encodes under
 * real concurrency.
 *
 * Only meaningful with Y264_STAIR_WIDE=1 and Y264_STAIR_MULTIHOP=1 both on: the
 * multi-hop clamp is the thing that makes a deeper list 0 safe under width, and
 * this gate does not imply it. Resolved in warm_lr_statics. */
static int stair_wide_ref_on(void)
{
    /* DEFAULT ON: concurrent chains at ref>1. Hardened against phantom
     * row-count, clamp-set overwrite, warm stand-down and acquire ordering;
     * 0 crashes / one md5 across 60 armed runs, TSan clean.
     * Wall: samsung +9.3%, 720p class +3-4% at t12; CRF band cost 0.00 on
     * six clips. ABR/2-pass are UNAFFECTED: width needs a decide lag and
     * Y264_RCP_LAG defaults 0 there.
     * Y264_STAIR_WIDE_REF=0 escapes to the serialized ref>1 chains. */
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_WIDE_REF"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* How many bursts may stay IN FLIGHT across an anchor's rcp decide. 0 = the
 * shipped zero-lag-anchor schedule -- stair_run_burst retires everything before
 * the launch, so an anchor decides on full actuals and nothing overlaps it.
 * That is also why Y264_STAIR_WIDE does not engage under ABR/2-pass: width IS
 * deferred retirement, and zero lag is its exact negation.
 *
 * There is no third option where the anchor keeps both. Retirement is
 * oldest-first (stair_oldest, which append_nal's coding order depends on), so
 * the bursts a wide ring holds are always the NEWEST ones -- the immediate
 * predecessor is the last thing that can still be flying, not the first thing
 * to drain. "Actuals" means every frame coded before this one, so a drain that
 * leaves anything live is a lagged decide by definition.
 *
 * n > 0 therefore reintroduces the general-case burst lag -- deterministic and
 * in coding order, a function of the launch sequence and K only, never of
 * timing or thread count. 1 = "actuals through burst n-2, predictions for burst
 * n-1", the schedule the pop rule is written for. 2 = one burst deeper, which
 * is what a K=3 ring needs to actually hold three chains.
 *
 * DEFAULT 1 since 2026-09-03 (see rcp_lag_env below). Resolved in warm_lr_statics.
 *
 * This is the ENV value, and no decide reads it directly. What a decide reads
 * is e->rcp_lag, which is this value only where width can actually engage
 * (stair_wide_capable, resolved once at open) and 0 everywhere else. Bits are
 * NOT required to be identical across --threads, so the lag's price falls only
 * on the configurations that get the width; applied uniformly instead (charged
 * to t1 as well as t18) it costs +3.49% BD. */
static int rcp_lag_env(void)
{
    /* DEFAULT 1 since 2026-09-03 (owner decision): the ABR decide runs one
     * burst ahead of the actuals instead of waiting for them, which is the
     * pipeline width the rate-control loop was refusing. Priced on the
     * rate-matched ABR board at auto threads: goal 2 1.12x -> 1.06x, goal 3
     * 1.33x -> 1.25x, worst clips 1.26 -> 1.16 and 1.43 -> 1.35; 7-10% of wall
     * on every 720p/1080p ABR cell at twelve threads. BD at twelve threads vs
     * lag 0 (VMAF-NEG, 120-250f): sintel -1.9, foreman -1.2, samsung -0.1,
     * ducks +0.1, park_joy +0.2, riverbed +0.5, crowd_run +0.7, stockholm
     * +1.4, inside the +-1.7 the instrument shows. What made this shippable
     * is the in-flight prediction from each frame's own complexity
     * (Y264_ABR_RF2_INFLIGHT): with the type-mean form the lag cost sintel
     * +14.4%. At one thread every actual is staged before the decide, so lag
     * 1 is byte-identical to lag 0 there; under a VBV the lag is inert.
     *
     * History: it was 0 because lag 1 emitted BROKEN BITSTREAMS -- the
     * early-anchor fill was appending its NAL as well as billing the ledger,
     * which put an anchor in front of the previous mini-GOP's B's while
     * FrameNum had already been claimed in plan order (see
     * stair_drain_anchor; fixed, recon-match 60/60 at t1 and t12 armed). Gate
     * any change here on scripts/recon_thread_gate.sh AND
     * scripts/abr_decode_gate.sh: a CRF band plus a lag-OFF identity gate
     * never decodes this path, and conformance cannot reach it (--dump-recon
     * forces the serial path). */
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("Y264_RCP_LAG");
        v = s ? atoi(s) : 1;
        if (v < 0) v = 0;
        if (v > Y264_STAIR_K - 1) v = Y264_STAIR_K - 1;
    }
    return v;
}

/* The lag as resolved for THIS encoder: 0 single-threaded (param.threads ==
 * 1, the goal-1 configuration), where there is no pipeline width to buy and
 * the lag would still move the bits and the SPS reorder depth. Every reader
 * that reaches the bitstream goes through here, so lag 1 is byte-identical
 * to lag 0 at one thread. Keys on param.threads, never the resolved width. */
static int dauto_refonly_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_DIRECT_AUTO_REFONLY"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
static int rcp_lag_for(const yah264_encoder_t *e)
{
    return e->param.threads == 1 ? 0 : rcp_lag_env();
}

/* Half-width of the QP window an ABR decide may occupy around the regime its
 * model was calibrated at (the ABR twin of Y264_VBV_QPD). 0 = no guard, and
 * that is the DEFAULT so the shipped path is untouched.
 *
 * It applies at every lag setting, including 0, and that is deliberate rather
 * than incidental: gating it on lag>0 made "lag1 with the guard" vs "lag0
 * without it" read as a 48% BD WIN for the lag, which is not a statement about
 * lag at all -- a tight clamp is close to constant-QP and that beats this ABR
 * ladder's own allocation on a static clip. Isolating the lag needs the guard
 * on both arms. Resolved in warm_lr_statics. */
static int rcp_qpd_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RCP_QPD"); v = s ? atoi(s) : 0; }
    return v;
}

/* Y264_ABR_EARLY: 1 = the unsafe launch-split PROBE, 2 = the shipped DRAIN
 * SPLIT. DEFAULT 2.
 *
 * MODE 1 IS UNSAFE BY CONSTRUCTION and must never be the default -- it is
 * measurement scaffolding, described below. Mode 2 is the safe form: it
 * retires the ANCHOR half of every burst it is about to leave flying, so the
 * decide is short only the B leaves, where the tail lives. B->size is final
 * when a burst's runner returns; the chain driver holds the leaves. Splitting
 * the wait there keeps the overlap without pricing the burst off a FIFO
 * missing its own actuals.
 *
 * THE TRADE, measured, because this one is not free: ~+1.3% BD-VMAF-NEG at
 * --ref 3 and +0.14-0.7% at --ref 1 on cut-heavy content (sintel; 9 of 12
 * clips are byte-identical), for 1.04-1.11x on the ABR board. Rate accuracy is
 * equal-or-better than shipped on every clip including sintel, so the RC
 * failure that killed Y264_RCP_LAG does not reproduce here.
 *
 * ENGAGEMENT is threads-per-GOP-worker, not GOP count: it needs a worker pool
 * reaching Y264_MT_POOL_MIN (2 today, 8 when this was written), and the CLI splits --threads across GOP
 * workers, so roughly --threads >= 8*ceil(frames/keyint). Verified by output
 * hash -- sintel 1152f is inert at t18 and engages at t36 with the same 5
 * GOPs. An encode that gets no speedup is byte-identical and takes no risk,
 * because engagement and speedup share the same staircase. This is NOT rare on
 * wide machines, and it is not GOP-poor-only.
 *
 * Y264_ABR_EARLY=0 restores the zero-lag prologue drain.
 *
 * The probe's own description follows, since mode 1 still exists:
 *
 * It answers one question and nothing else: what is the whole drain-placement
 * overlap worth when the anchor's analysis starts against a still-draining
 * predecessor? It skips stair_run_burst's zero-lag prologue drain so the
 * launch happens first and the late drain at the bottom retires the
 * predecessor after this anchor's jobs are already registered -- the CRF
 * ordering, under ABR. The anchor's decide then runs BEFORE its predecessor's
 * rcp_fill, so it prices the burst off a FIFO that is missing the actuals it
 * is supposed to account for. Bits move, rate control is not what it claims to
 * be, and none of that matters to the wall clock this exists to read.
 *
 * It differs from Y264_RCP_LAG_NOWIDE by deliberately NOT moving
 * rcp_pop_ready's seq bound with it: this isolates the ORDERING from the
 * accounting change, so the number is drain placement alone.
 *
 * Resolved in warm_lr_statics; the encoder reads e->abr_early, resolved once at
 * open, so nothing on a decide path calls getenv. */
static int abr_early_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_ABR_EARLY"); v = s ? atoi(s) : 2; }
    return v;
}

static int wf_narrow_frame(int width, int height);   /* defined with the cap */

/* The --ref half of width's engagement test, in one place: stair_run_burst
 * gates `wide` on it and dpbp_open sizes the buffer pool on it, and those two
 * must agree or a shape runs wide with no pool behind it. */
static int stair_wide_nref_ok(const yah264_encoder_t *e)
{
    return e->nref <= 1 || stair_wide_ref_on() ||
           wf_narrow_frame(e->param.width, e->param.height);
}

/* MEASUREMENT GATE, DEFAULT OFF. Resolved in warm_lr_statics.
 *
 * The lag budget is handed out by stair_wide_capable, whose --ref term exists
 * for WIDTH: a wide ring recycles DPB slots, so it needs the bag pool, so it
 * needs nref <= 1. But the overlap a lag buys does not need width. On the
 * non-wide path stair_run_burst retires the predecessor AFTER the new anchor's
 * jobs are registered, which is exactly what CRF does at --ref 3 and exactly
 * what the zero-lag prologue drain takes away from ABR there. Granting the
 * budget on async capability alone leaves `wide` and dpbp_open refused by
 * stair_wide_nref_ok (both read it directly) while keeping one burst live
 * across the launch.
 *
 * THIS IS A PROBE, NOT A SHIPPABLE FEATURE. It answers one question -- does the
 * lag alone recover the --ref 3 cost, without opening the --ref gate and
 * without a DPB bag pool -- and the answer is yes: -10.4% on foreman and -13.4%
 * on park_joy, against -10.8% and -13.8% for the same shape with width also
 * switched on. Width is worth roughly nothing at --ref 3.
 *
 * What stops it shipping is the price, not the mechanism. It moves bits at
 * --ref > 1, so byte-identity does not cover it; it needs QPD 6 alongside,
 * because at the shipped Y264_RCP_QPD 0 the lagged ABR ladder winds up and
 * costs +48.3% BD-NEG on park_joy; and even with the guard the corpus reads
 * mixed (foreman +1.22%, bus +2.15%, park_joy -5.58% against the shipped
 * default). Flipping it means moving two defaults at once. */
/* Y264_DIRECT_PERMB: the temporal-direct legality check is per macroblock
 * (default) rather than per slice. See docs/b-direct-mode.md, third round. */
static int direct_permb_env(void)
{
    static int v = -2;
    if (v == -2) { const char *e = getenv("Y264_DIRECT_PERMB"); v = e ? atoi(e) : -1; }
    return v;                       /* -1 = unset, take the default (on) */
}

/* Per-macroblock temporal-direct gating. DEFAULT ON at every thread count
 * since 2026-09-01.
 *
 * The gate is correct (8.4.1.2.3 binds where the derivation runs) and worth
 * 19-23 BD points on the clips temporal suits. It was pinned to --threads 1 for
 * a while because the output was not reproducible above that, and the cause was
 * NOT the co-located field, the staircase, or an unbounded derived vector --
 * all three were measured and cleared. It was that `struct direct_mv dmv` in
 * analyze_b_mb was an uninitialised stack local, and temporal_direct returns 0
 * from inside its four-block loop, leaving it PARTLY written; the ME seed list
 * then read block 0 regardless. The frame-wide gate this replaced is immune by
 * construction -- it pre-screens the whole colpoc grid and demotes the frame,
 * so inside a temporal frame the derivation never refuses -- which is why
 * per-MB gating is the change that made the partial fill reachable at all. See
 * analyze_b_mb and docs/b-direct-mode.md.
 *
 * Stack garbage is reproducible for as long as the worker's call history is,
 * which is why it only showed above one thread and only with two sibling B
 * leaves sharing the pool's workers (Y264_FPIPE): --bframes 1 or 2 never form a
 * pair and were always bit-exact. Zeroing dmv makes the reproducer identical
 * across runs AND across thread counts. */
static int direct_permb_for(const yah264_encoder_t *e)
{
    int v = direct_permb_env();
    (void)e;
    return v >= 0 ? v : 1;
}

/* "Can a B slice of this encoder run temporal direct?" -- which is NOT the same
 * question as `param.direct == TEMPORAL` once an auto mode exists, and getting
 * that wrong is a segfault rather than a quality regression. Under the auto
 * rule (param.direct AUTO, the default) individual slices choose temporal
 * while nothing global says so, so every staircase predicate below has to ask this instead: they
 * exclude temporal direct because its mvL1 = mvL0 - mvCol is unbounded and it
 * reads a still-streaming picture's colmv field, and an auto slice does both.
 * Measured, not reasoned: blue_sky at 20 frames crashed 10 of 10 runs with the
 * staircase left armed, and 0 of 10 with Y264_STAIR=0.
 *
 * Static configuration only, as the staircase predicates require: a cached env
 * read and a parameter, both fixed for the life of one encoder_open. */
/* Y264_DIRECT_AUTO: unset follows param.direct (AUTO is the default since
 * 2026-09-03); 0 forces the per-slice rule off (spatial), 1 forces it on
 * over a pinned --direct. */
static int y264_direct_auto_env(void)
{
    static int v = -2;
    if (v < -1) { const char *e = getenv("Y264_DIRECT_AUTO"); v = e ? (atoi(e) ? 1 : 0) : -1; }
    return v;
}
static int direct_auto_wanted(const yah264_encoder_t *e)
{
    int v = y264_direct_auto_env();
    return v >= 0 ? v : e->param.direct == YAH264_DIRECT_AUTO;
}
static int direct_auto_armed(const yah264_encoder_t *e)
{
    /* Every thread count since 2026-09-01: the score is per encoder instance
 * and folded in coding order (see build_slice_prep). */
    return direct_auto_wanted(e) && direct_permb_for(e);
}

static int direct_may_be_temporal(const yah264_encoder_t *e)
{
    return e->param.direct == YAH264_DIRECT_TEMPORAL || direct_auto_armed(e);
}

/* Y264_STAIR_TDIR: let the staircase run WITH temporal direct, which it has
 * always refused. stair_clamp_on gives two reasons and they are not equal.
 *
 * The first is real and this knob depends on fixing it: temporal direct
 * synthesises mvL1 = mvL0 - mvCol, and no closure bounds that (spatial's list-1
 * vector is a median over already-clamped coded MVs, so the clamp closes over
 * it). analyze_b_mb now tests the derived mvL1 against the same stair_mvy_max
 * the list-1 SEARCHES are held to, and drops direct for that macroblock when it
 * exceeds it -- the shape the list-0 side already used.
 *
 * The second reads as "it reads a still-streaming picture's colmv field", and
 * for the B direct case that appears not to bind: a leaf's list-1 is either its
 * own burst's reference B, whose colmv content stair_dpb_commit_content lands
 * before the leaf runs by chain order, or a future anchor, which is complete by
 * coding order. The reader that DOES race is the next anchor's temporal ME seed,
 * and stair_launch already waits on refb_done for exactly that.
 *
 * That reasoning is why the knob exists; what moved it to default ON
 * (2026-09-03) is the determinism gate: scripts/stair_determ.sh 32/32 at 4,
 * 12 and 18 threads with the per-slice auto rule armed, the 1080p clips
 * repeat-identical under load at 12 and 18 threads, and TSan clean, after the
 * uninitialised direct-MV seed that produced the 14/32 reading was fixed.
 * The wall with temporal allowed is identical to spatial-only. */
/* Y264_DAUTO_FOLD (default 2): how far behind its own seq a launch folds
 * the auto-rule counts. 0 = the structural bound, bursts <= n - K - 1, which
 * the slot recycling proves drained (four bursts stale at K = 3: blue_sky
 * decided temporal on 6 slices at twelve threads against 24 at one).
 * d >= 1 = fold bursts <= n - d, WAITING at launch for burst n - d's chain
 * end, where its counts are final; deterministic because the wait makes the
 * fold set a function of n alone. The wait costs overlap only when n - d is
 * still running; measured at 2 first. */
static int dauto_fold_env(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_DAUTO_FOLD"); v = e ? atoi(e) : 2; if (v < 0) v = 0; if (v > Y264_STAIR_K + 1) v = Y264_STAIR_K + 1; }
    return v;
}
static int stair_tdir_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_STAIR_TDIR"); v = e ? atoi(e) : 1; }
    return v;
}

/* Does the slice's direct mode force the staircase off?
 *
 * TRIED AND REFUSED 2026-09-01, in two steps, and the exclusion survived both.
 *
 * Step one: bound mvL1 (analyze_b_mb now does) and lift the exclusion. The
 * threaded recon gate bus-errors on blue_sky_1080p at t4/t12/t18. Narrowing by
 * stage said the fault is v5 reference-B pipelining -- BDEPTH=0 passes,
 * DEPTH=0 passes, WIDE=0 still fails -- because the colocated picture is then a
 * still-streaming reference B whose colmv field is read before
 * stair_dpb_commit_content lands.
 *
 * Step two: exclude only BDEPTH and let clamp, depth and width run. Recon-match
 * passes at t4/t12/t18 on three clips, and the wall price falls from 1.40x to
 * 1.05x, which looked like the answer. It is not:
 * scripts/stair_determ.sh reads 14/32 with the staircase on against 32/32 with
 * it off. The bitstream is not reproducible run to run at a fixed thread count,
 * which recon-match cannot see because each run decodes to whatever that run
 * built. A wall number measured there is measuring a different encoder each
 * time.
 *
 * So the exclusion is not narrowable by stage. What it is waiting on is a
 * leaf-side wait on the co-located field's commit, the same thing refb_done
 * sequences for the next anchor and nothing sequences for the leaves. Until
 * then this knob is an experiment switch over a KNOWN-NONDETERMINISTIC path,
 * and it defaults off for that reason. */
static int stair_direct_blocks(const yah264_encoder_t *e)
{
    return direct_may_be_temporal(e) && !stair_tdir_on();
}

/* Y264_ABR_RF2_BASEDOM: default ON. The rate factor accumulates every frame's
 * bits at the frame's BASE (anchor-domain) QP, not its coded QP. The coded QP
 * carries the type offset (I -3, B +cascade), and crediting a B's bits at
 * base+3 overstates the P-domain rate factor by the B's qscale ratio: on
 * samsung 720p 1200 kbit/s the B frames carried 51% of the bits at +2.5 QP,
 * the bits-weighted qscale read 0.77 QP above the P track, and the loop
 * settled with the stream 15% under target and only the overflow term
 * pulling (2026-09-04). At the base the sum equals wanted x q exactly when
 * the bits equal wanted, whatever the type mix. =0 restores the coded-QP
 * accounting for A/B. */
static int abr_rf2_basedom_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_ABR_RF2_BASEDOM"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
/* Y264_RCP_NODRAIN: default ON. See the emit_frame_w2 comment. */
static int rcp_nodrain_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RCP_NODRAIN"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
static int rcp_lag_nowide_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RCP_LAG_NOWIDE");
                 v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* stair_wide_capable minus the width-only --ref term, for the gate above. Same
 * static-configuration discipline: every term is fixed for the life of one
 * encoder_open, so a decide never asks what is currently in flight. */
static int stair_lag_capable(const yah264_encoder_t *e)
{
    return stair_on_env() && stair_depth_on() && stair_wide_on()
        && e->b_pyramid && !stair_direct_blocks(e)
        && !e->vbv_on
        && e->wf_width >= Y264_MT_POOL_MIN;
}

/* "Could this encoder instance ever run a wide ring, IGNORING rate control?"
 *
 * The rate-control lag exists to pay for width, so an instance that
 * can never run wide should never pay it -- a --threads 1 encode gets no
 * overlap out of the lag and has no reason to carry its bits. Answering that
 * needs a predicate, and the predicate has two hard requirements.
 *
 * STATIC CONFIGURATION ONLY. Every term below is fixed for the life of one
 * encoder_open: three env gates, two parameter-derived fields, and the pool
 * width this open resolved before it built the pool. NOT in it, deliberately:
 * e->wf_warmed and rcp_warm (both true only after the encode has started),
 * whether a burst actually went wide, whether a VBV burst fell back, or
 * anything else stair_ready consults beyond the pool. THE RULE: bits may depend
 * on --threads, but they may never depend on WHEN a chain finished, or the
 * output stops being reproducible run-to-run at a fixed thread count.
 *
 * IGNORING RATE CONTROL, also deliberately, and this is the whole reason the
 * function exists rather than reusing stair_wide_rc_ok. That predicate reads
 * the lag (ABR needs a lag budget to run wide at all); if the lag then read
 * width engagement, the two would define each other. Cutting the loop at the RC
 * term is the natural place: "can this shape run wide" is a question about the
 * pool, the ring and the reference depth, and the answer does not need to know
 * which rate-control mode is asking. VBV is excluded here anyway, and again in
 * stair_wide_rc_ok, because it must be excluded on every branch.
 *
 * The staircase terms mirror stair_clamp_on's static half (b_pyramid, direct
 * != 1) plus the depth gate, since width is deferred retirement of an ASYNC
 * chain and stair_depth_on is what makes a chain async. */
static int stair_wide_capable(const yah264_encoder_t *e)
{
    return stair_on_env() && stair_depth_on() && stair_wide_on()
        && e->b_pyramid && !stair_direct_blocks(e)
        && stair_wide_nref_ok(e)
        && !e->vbv_on
        && e->wf_width >= Y264_MT_POOL_MIN;
}

static int stair_wide_rc_ok(const yah264_encoder_t *e);
/* Width can engage for THIS ENCODE's configuration: capability AND the
 * rate-control half. The width-hardening gates (warm stand-down, settled-bound
 * tightening) key on THIS, not on capable alone: capable is true under ABR
 * where the zero-lag decide keeps width disengaged, and gating the hardening
 * on capable changes ABR bits for no protection gained. Config-invariant: env
 * + params + rc mode, never thread count or live state. */
static int stair_wide_engaged_cfg(const yah264_encoder_t *e)
{
    return stair_wide_capable(e) && stair_wide_rc_ok(e);
}

/* And the rate-control half, for the same reason: stair_run_burst gates `wide`
 * on it and dpbp_open sizes the pool on it. Writing that rule out twice is a
 * trap: the two copies drift and a shape runs wide with no pool behind it,
 * falling back to the reader wait on every recycle.
 *
 * CRF/CQP never set rcp_on and are unaffected. ABR/2-pass need a lag budget,
 * because retiring everything before the decide is what width is. VBV is
 * refused outright whatever the budget: its burst gate (rcp_vbv_gate) drains
 * every live frame at anchor ARRIVAL so the CPB simulation reads the exact
 * buffer, which is a second, safety-critical drain this knob does not reach --
 * and the VBV pipeline's measured actual/predicted tail is unbounded, so a
 * lagged buffer law is not a thing to hand a spare env var.
 *
 * Reads e->rcp_lag, not the env: the budget is only granted to an instance that
 * can run wide, so this stays the exact statement "width needs a lag budget"
 * while a narrow instance's lag reads 0 and its decides take the shipped
 * zero-lag path. No loop -- e->rcp_lag comes from
 * stair_wide_capable, which does not consult this function. */
static int stair_wide_rc_ok(const yah264_encoder_t *e)
{
    return !e->rcp_on || (e->rcp_lag > 0 && !e->vbv_on);
}

/* MT Lever 3 v6: replace the launch-side reference-B
 * content WAIT with the row gate + clamp this codebase already uses for the
 * anchor-vs-anchor case. An anchor's list-0 ME stops blocking until a live
 * burst's reference B has fully landed, and instead claims its rows against
 * that reference B's publish watermark (C->rprog) with its vertical search
 * clamped to the same bound as every other clamp on this arm -- e->stair_mvy_max
 * (a per-open runtime value, see stair_lag_for, not the fixed
 * Y264_STAIR_MVY_MAX).
 *
 * THE MARGIN IS THE EXISTING ONE, AND NOT BY ANALOGY. The bound's soundness for
 * the previous anchor is a local two-sided inequality, not a head-start
 * argument: a reader at MB row r clamped to MVY_MAX = 16*LAG-24 px touches luma
 * rows <= 16(r+LAG)-6, and a producer at pub >= r+LAG has written final luma
 * rows 16(r+LAG)+13, hpel rows 16(r+LAG)+10 and (4:2:0) final chroma rows
 * 8(r+LAG)-4 vs 8(r+LAG)+6. Both sides are properties of ONE function,
 * stair_trailer_task, parameterised only by a struct stair_prog -- and a
 * reference B under BDEPTH runs that same function verbatim. So the bound
 * transfers by identity of the producer, not by re-derivation, and the slack
 * (10 rows in the tightest plane) is independent of LAG.
 *
 * It is also the only sound shape here. A reference B has NEGATIVE publish head
 * start relative to the anchor that reads it: it is launched in its burst's
 * phase 2, AFTER the stair_serial_fire that releases the next anchor's launch.
 * Any "h chained row gates of lead time" argument of the kind that justifies
 * hop 2 would be false for it. The gate has to be direct.
 *
 * ENGAGEMENT IS NARROW, and structurally so. A reference B only HAS a watermark
 * when B->bdepth holds, which needs Y264_STAIR_BDEPTH plus a mini-GOP of
 * exactly one reference B at plan entry 0 (bframes 2 or 3). At bframes 4-7 the
 * pyramid commits its reference B's inline with no per-row publish at all, so
 * there is nothing to gate on and those shapes keep the blocking wait. The
 * decision is per live burst, at the wait.
 *
 * DEFAULT ON, and it CHANGES BITS, like Y264_STAIR_MULTIHOP: two more clamped
 * list-0 references, and unlike hop 2 these bind often (a burst's reference B
 * sits at index 0 or 1 of every later P list 0). Resolved in warm_lr_statics. */
static int stair_refbgate_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_REFBGATE"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Launch the reference B EARLY -- at its own prep, inside phase 1, instead of
 * in phase 2 after every sibling leaf has also been prepped and
 * stair_serial_fire has released the next anchor's launch. Without this, the
 * row gate above recovers only a fifth of its own measured ceiling, because the
 * thing it gates on starts late: a reference B has NEGATIVE publish lead time
 * on the anchor that reads it, being launched in its burst's phase 2, AFTER the
 * stair_serial_fire that releases the next anchor's launch. This moves the
 * launch itself (ntp_bg_submit of the runner + trailer, not just the
 * stair_prog_reset arm) to right after the
 * reference B's own prep completes, so it starts producing rows while its
 * OWN burst's remaining sibling preps are still running on the driver
 * thread, rather than after all of them.
 *
 * Safe for the same reason the v5 pipeline is safe running
 * alongside phase 2: the runner + trailer tasks touch only the reference
 * B's own leaf (L, fully populated by the prep that just ran), the chain's
 * rprog (armed on the line directly above) and thread-local ME state
 * (y264_me_set_hpel) -- never a shared e-> field. A sibling leaf's LATER
 * prep, still to come in phase 1, writes e-> on the driver thread same as
 * always; nothing the runner/trailer read is among those writes. A sibling
 * prep failing after this launch still reaches stair_join_compute
 * unconditionally at the bottom of stair_chain, which knows how to
 * sync a launched-but-abandoned reference B via stair_refb_join -- that
 * path is independent of this flag (it also covers a phase-2 sibling failing
 * after the unmodified launch).
 *
 * DEFAULT OFF. It changes no bits at any --ref (it moves WHEN existing async
 * work starts, not what it computes), but it is new concurrent-execution
 * surface against the same burst's own remaining preps, so it gets its own
 * flag and its own TSan pass rather than riding STAIR_BDEPTH silently.
 * Resolved in warm_lr_statics. */
static int stair_refbearly_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_REFBEARLY"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Frame-concurrency probe: defer a wide chain's previous-anchor
 * full-publish wait until AFTER
 * stair_serial_fire, so the next launch's serial_wait stops transitively
 * waiting for the predecessor's predecessor to fully publish. The wait itself
 * is unchanged -- phase 2's B reads still see a fully-published previous
 * anchor -- it just no longer sits between a launch and the launch after it.
 * Phase 1 preps read DPB bookkeeping (final at the predecessor's own
 * serial_done, which launch ordering already guarantees) and never the
 * predecessor's pixels, so the move is an ORDER change: byte-identity is the
 * gate, not an argument.
 *
 * DEFAULT OFF. Applies only to wide async bursts -- on the non-wide path the
 * API thread drains the predecessor right after the launch, so there is
 * nothing for a deeper launch window to overlap. Resolved in warm_lr_statics. */
static int stair_freelaunch_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_FREELAUNCH"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Run ANY two consecutive non-reference plan entries as the concurrent pair,
 * not only the two the `b - a == 3` shortcut in stair_plan_hier marks.
 *
 * WHY. The pool is 8 wide on CIF (yah264_frame_thread_cap's knee is 7, floored
 * to Y264_MT_POOL_MIN) and one CIF frame's wavefront cannot feed 8 workers: at
 * 22 columns a worker claiming row r needs row r-1 two cells ahead, so the 8th
 * worker starts 14 cells into a 22-cell row and the frame spends its head and
 * tail with workers stalled on slope. Measured as NTP_PROF's ramp+tail: 4.0% of
 * pool time at bframes 3, 36.7% at bframes 4. What separates them is FRAME
 * concurrency, not worker count -- summed job span over lifetime is 3.7 frames
 * in flight at bframes 3 and 1.2 at bframes 4.
 *
 * The pair is where that concurrency comes from inside a burst, and without
 * this it only exists where the recursion bottoms out on a range of exactly
 * 4. That is true for EVERY leaf only at nbuf 3 and 7 -- mini-GOP 4 and 8 -- so
 * bframes 3 and 7 run their leaves two at a time and bframes 1/2/4/5/6 run one
 * at a time. The plan shapes, leaves marked L and reference B's R:
 *
 * nbuf 2: R1, L0 1 solo leaf
 * nbuf 3: R1, [L0 || L2] none
 * nbuf 4: R2, R1, L0, L3 2 solo leaves <- consecutive, pairable
 * nbuf 5: R2, R1, L0, R4, L3 2 solo, separated by a reference B
 * nbuf 6: R3, R1, [L0 || L2], R5, L4 1 solo
 * nbuf 7: R3, R1, [L0 || L2], R5, [L4 || L6] none
 *
 * So the entries this reaches that the shortcut does not are exactly nbuf 4's
 * L0/L3 (and nbuf 8's, which the mini-GOP cap makes unreachable). It is a
 * strict superset: a shortcut-marked pair is two consecutive non-ref entries,
 * so the shortcut never selects a group this one misses.
 *
 * SAFE, and for the reason the shortcut's own pair is safe rather than by
 * analogy to it. A non-reference leaf is read by nothing -- not by the other
 * leaf, not by any later frame -- so two of them never alias. Both are already
 * prepped: phase 1 preps EVERY plan entry before phase 2 runs any of them, so
 * neither encode waits on driver state the other produces. And every reference
 * a leaf does read (its bracketing anchor and reference B's) precedes it in
 * coding order, which is what makes the plan a legal coding order in the first
 * place, so a pair whose entries are adjacent in the plan has both its brackets
 * already committed by the entries before it.
 *
 * CHANGES NO BITS. Same preps from the same state, same NALs stashed in the
 * same coding order, same recon replay order -- only which thread runs the
 * second analyze. Byte-identity is the gate, not a BD sweep.
 *
 * DEFAULT ON. Byte-identical at bframes 2/3/4 x t1/t18, so it cannot move a
 * bitstream; it pays where a plan actually holds two adjacent non-reference
 * leaves, which is bframes 4 (-6.4% foreman, -5.0% park_joy_720p, controls
 * within 0.7%), and is a measured wash at bframes 2 and 3 where the plan has no
 * such pair to generalize. Resolved in warm_lr_statics. */
static int stair_leafrun_on(void)
{
    static int v = -1;
    /* 1 = generalize (default); 0 = the b-a==3 shortcut only; 2 = MEASUREMENT
     * ONLY, suppress every pair including the shortcut's own, to price what the
     * pair is actually worth at nbuf 3/7. */
    if (v < 0) { const char *s = getenv("Y264_STAIR_LEAFRUN"); v = s ? atoi(s) : 1; }
    return v;
}

/* The DPB EVICTION GUARD is redundant once the bag pool
 * serves the recycle, and it is the launch-side wait that serializes the chains
 * at bframes 2/4/6 (82% of the bframes-4 encode). With this on, the guard runs
 * only where the pool cannot cover it.
 *
 * WHAT THE GUARD IS FOR. `stair_dpb_begin` evicts the lowest-FrameNum picture
 * and then hands ITS BUFFER to the incoming anchor as the write target -- so a
 * live burst still searching that picture would have its reference rewritten
 * underneath it. The v3 guard waits for those readers by syncing their driver
 * and runner.
 *
 * WHY THE POOL SUBSUMES IT. `dpbp_recycle` removes the handout: the
 * slot takes a FRESH bag and the retiring picture keeps every buffer it lent
 * out, parked until `dpbp_sweep` proves no burst live at the park has survived.
 * That is the same obligation the guard discharges, discharged without blocking.
 * The pool covers the only path by which the victim's buffer can reach a writer,
 * because the victim's buffer reaches a writer ONLY through the slot handout
 * below, and that handout is exactly what the pool replaces. Where the pool
 * cannot serve -- absent (every non-wide encode) or exhausted -- `!dpbp_recycle`
 * falls through to `stair_slot_readers_wait`, which is a STRICT SUPERSET of the
 * guard on the entry actually handed out: same readset test, and it also syncs
 * the TRAILER, which the guard does not.
 *
 * So the guard is not weakened, it is relocated to the one branch that needs it.
 * The DPB BOOKKEEPING the eviction does (`used = 0`, the slot scan) is untouched
 * and is anyway excluded from every chain by the serial_done handshake.
 *
 * Pure scheduling: it deletes a wait and writes nothing, so byte-identity is the
 * gate rather than a BD sweep.
 *
 * DEFAULT ON. Byte-identical at bframes 2/3/4/6 x t1/t18, so it cannot move a
 * bitstream. It pays only where the wide path engages, i.e. --ref <= 1:
 * foreman_cif t18 bframes 4 -34.6%, bframes 2 -15.3%, duplicate-build control
 * within 0.3%. At --ref 3 it is a measured no-op (+0.2%/-0.1%) because the
 * guard is not on the critical path there, so it does not move the parity
 * scoreboard either (that runs at the preset default --ref 3).
 * Y264_STAIR_EVICTPOOL=0 restores the guard. Resolved in warm_lr_statics. */
static int stair_evictpool_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_EVICTPOOL"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Y264_UNSAFE_NO_REFBWAIT=1: drop the launch-side reference-B content wait
 * entirely. NOT a candidate and never will be -- the wait holds a real
 * read-after-write, and without it an anchor's list-0 ME reads a reference B
 * that is still being written. It answers the one question no safe build can:
 * the CEILING on anything that makes the wait fire less often, which is what
 * decides whether a cleverer predicate is worth its correctness surface. Off by
 * default, resolved in warm_lr_statics, used only by scripts/stair_l0bound.sh. */
static int stair_unsafe_no_refbwait(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_UNSAFE_NO_REFBWAIT"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

static int stair_unsafe_no_rowgate(void);   /* both defined beside */
static int stair_unsafe_gate_arow(void);   /* stair_row_ready */

/* Y264_UNSAFE_NO_PREVPWAIT=1: drop the CHAIN-side wait for the previous
 * anchor's full publish (stair_chain's first statement) entirely. Same status
 * as Y264_UNSAFE_NO_REFBWAIT above and for the same reason -- the wait holds a
 * real read-after-write (a leaf's list-0 ME reads an anchor still being
 * written), so this races by construction and is never a candidate. It exists
 * to price the CEILING on the row-gate version of the same dependency before
 * that version's correctness surface is built. Off by default, resolved in
 * warm_lr_statics, used only by scripts/stair_prevp_sweep.sh (the 19-shape
 * ceiling sweep), scripts/stair_prevp_1080.sh and scripts/stair_prevp_stat.sh. */
static int stair_unsafe_no_prevpwait(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_UNSAFE_NO_PREVPWAIT"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Y264_NO_SCENECUT=1: DIAGNOSTIC ONLY -- suppress scene-cut IDR insertion, to
 * isolate the cost of the drain+dpb_reset+cold-refill barrier a real cut
 * forces. Two call sites (la_finalize's deferred decision, encode_frame_core's
 * since_idr reset) share ONE warmed accessor: separate function-local statics
 * race across concurrent GOP-worker threads on first use (each opens its own
 * encoder but shares this process's statics) unless they are in
 * warm_lr_statics, like every other env-gated static in this file. Resolved in
 * warm_lr_statics. */
/* Single-thread quality mode (see macroblock.h stq). Y264_STQ: unset = engage
 * at wf_width==1; 0 = never (restores the pre-stq t1 encoder); 1 = force on at
 * any width (diagnostic). Resolved in warm_lr_statics. */
static int stq_env(void)
{
    static int v = -2;
    if (v == -2) { const char *s = getenv("Y264_STQ"); v = s ? atoi(s) : -1; }
    return v;
}
static int scenecut_off_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_NO_SCENECUT"); v = s && *s == '1'; }
    return v;
}

/* Y264_STAIR_STAT=1: report the arrival-side blocked time (the v4 overlap
 * window metric) at encoder close. 2 additionally dumps the CHAIN EVENT TRACE
 * -- one line per scheduling transition per ring slot -- which is what turns
 * "the bursts do not overlap" from a timing inference into a structural read
 * (scripts/stair_tl.py renders it). A LEVEL and not a flag so the trace needs
 * no lazy static of its own; every existing caller is a boolean test and 2 is
 * still true. Resolved in warm_lr_statics. */
static int stair_stat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_STAT"); v = s ? atoi(s) : 0;
                 if (v < 0) v = 0; }
    return v;
}

/* Decoupled lookahead thread (x264's shape, re-derived): the
 * per-frame lookahead chain runs on a dedicated thread behind the ring,
 * ahead of the encoder in push order. Engages only at pool >= la_pool_min
 * threads (its own threshold, not the staircase's 8 -- see la_pool_min),
 * la_depth >= bframes+3 (so a
 * popped entry's chain writers are always strictly older than the newest
 * push), and a lead to run into (la_buf > 0 -- see la_lead_for).
 *
 * TRI-STATE, and unset is not "off": -1 = auto (follow the lead
 * la_lead_for resolves, which is x264's own coupling -- its* returns without a thread when --sync-lookahead is 0), 0 = forced off, 1 =
 * forced on. Resolved in warm_lr_statics. */
static int la_thread_env(void)
{
    static int v = -2;
    if (v < -1) { const char *s = getenv("Y264_LA_THREAD"); v = s ? (atoi(s) ? 1 : 0) : -1; }
    return v;
}

/* Frame area, in MBs, at and above which a lookahead-side grid is big enough
 * to be worth handing to the pool. Shared by the chain's fan-out default
 * (la_inline, resolved in open) and the lowres field-ME wavefront
 * (lowres_field_me_prep). */
#define LA_FANOUT_MBS 512

/* Y264_LA_INLINE=1: run the lookahead chain's tiny per-push pool dispatches
 * (la_lr rows, lowres_analyse rows) inline on the calling thread instead of
 * fanning them out. The parallel and serial paths compute identical values by
 * construction (row-order exact reduction), so this changes scheduling only,
 * never bits. Diagnosis gate: on small
 * frames in the overlapped steady state the fan-out queues ~60us of work
 * behind the encode wavefront's rows and the join blocks the GOP driver for
 * multiples of the work itself.
 *
 * TRI-STATE like la_thread_env: -1 = auto (off-driver chain AND a small frame
 * -- see the resolution in open; the frame-size half of it is measured, not
 * assumed), 0 = forced fan-out, 1 = forced inline. */
static int la_inline_env(void)
{
    static int v = -2;
    if (v < -1) { const char *s = getenv("Y264_LA_INLINE"); v = s ? (atoi(s) ? 1 : 0) : -1; }
    return v;
}

/* x264's --sync-lookahead: extra RING CAPACITY
 * ahead of the window, not a smaller window. Every mb-tree/scene-cut window
 * walk stays capped at la_depth regardless of this value -- the ONLY visible
 * effect of k>0 is k more encode calls returning 0 NALs before the first
 * frame emits, and (combined with Y264_LA_THREAD) more wall-clock slack for
 * the chain to finish a push before an arrival call needs it. Clamped in
 * open to fit Y264_LA_CAP_MAX. Resolved in warm_lr_statics.
 *
 * Overrides param.sync_lookahead when set, including to 0, which is why unset
 * has to be INT_MIN and not 0: `Y264_LA_BUF=0` is how a byte-identity or A/B
 * run asks for no extra capacity at all. */
#define Y264_LA_BUF_UNSET INT_MIN
static int la_buf_env(void)
{
    static int v = Y264_LA_BUF_UNSET;
    if (v == Y264_LA_BUF_UNSET) {
        const char *s = getenv("Y264_LA_BUF");
        if (s) v = atoi(s);
    }
    return v;
}

/* The pool width at which the LOOKAHEAD LEAD engages, as its own threshold
 * rather than the staircase's Y264_MT_POOL_MIN.
 *
 * WHY IT IS SEPARABLE AT ALL. Y264_MT_POOL_MIN was 8 (it is 2 now) because a pool of 7 turned
 * stair_ready, fpipe_ready and the lookahead thread off AT ONCE (559 ms vs
 * 433 ms on foreman_cif) -- that measurement
 * prices the three together and says nothing about any one of them. Three of
 * the constant's consumers reach the bitstream (stair_lag_capable and
 * stair_wide_capable's rcp_lag grant, the pool-failure withdrawal, and the
 * thread-scaled stair_lag whose non-engagement below the floor is a stated
 * CORRECTNESS requirement); those keep the shared constant and are not touched
 * here. The lead's two gates -- la_lead_for and the la-thread creation -- are
 * scheduling only and byte-identical either way, which is exactly why they can
 * hold a threshold of their own.
 *
 * WHY THEY SHOULD. The lead's own engage predicate is measured on the corpus
 * and it is the LEAD that matters, not the pool. A size-based gate (engage
 * below yah264_frame_thread_cap's knee) does NOT work: three clips sharing a
 * knee of 21 score -15.2%, -12.3% and +0.8%. Nothing in the >= 8 term tests
 * pool width. Meanwhile the CLI splits --threads across GOP
 * workers, so a long clip at the default keyint hands every worker 3-5 threads
 * and the lead reaches real content at that keyint not at all.
 *
 * Static configuration only (the width this open will build), same rule as
 * la_lead_for itself. Resolved in warm_lr_statics. */
static int la_pool_min(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("Y264_LA_POOL_MIN");
        /* DEFAULT 2, NOT Y264_MT_POOL_MIN: the 559-vs-433 ms measurement
         * behind the 8 moved stair_ready, fpipe_ready and the la thread
         * TOGETHER, so it prices the three jointly and attributes nothing to
         * the lead. A pool of 2 with its own lookahead thread is worth having,
         * and min2 beats min3 in the sweep. Byte-identical by construction (it
         * moves when work starts, not what is computed), and verified so on
         * the multi-GOP shape it unblocks. Y264_LA_POOL_MIN=8 couples it back
         * to the staircase floor. */
        v = s ? atoi(s) : 2;
        if (v < 2) v = 2;               /* a lead with no pool at all leads nothing */
    }
    return v;
}

/* Deterministic fixed-lag RC feedback: lets ABR/2-pass ride the frame pipeline
 * instead of forcing the rc_waits drain. DEFAULT ON.
 * Y264_RC_PIPE=0 restores the serial RC. Resolved in warm_lr_statics. */
static int abr_rf_env(void);        /* defined with the other ABR knobs */
static double abr_tunable(const char *n, double def);
static void slice_overflow_warn(void);
static size_t cabac_zero_words(const y264_frame_t *f, y264_cabac_t *cb, const uint8_t *end);
static int abr_rf2_env(void);
static int tdir_legal_on(void);

static int rc_pipe_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RC_PIPE"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* rcp warm length (serial-tight decides per encoder before the lag begins)
 * and the ABR error-correction gain; tunables for re-calibration. Warmed
 * statics: decides can run on the stair driver thread. */
static int rcp_warm_n(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RCP_WARM"); v = s ? atoi(s) : 12; }
    return v;
}

static double rcp_gain(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_RCP_GAIN"); v = s ? atof(s) : 0.1; }
    return v;
}


/* ABR qcompress for the rcp decide path (the serial rc_set_qp keeps its own
 * per-call getenv untouched). Warmed static: async-chain decides run on the
 * stair driver thread. */
static double abr_qcomp_env(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_ABR_QCOMP"); v = s ? atof(s) : 0.6; }
    return v;
}

/* Y264_RCP_DBG=1: trace rcp decides/fills/pops. Warmed static: decides can
 * run on the stair driver thread. */
static int rcp_dbg_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RCP_DBG"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* VBV under the pipeline. Sub-gate inside Y264_RC_PIPE: DEFAULT ON.
 * Y264_RC_PIPE_VBV=0 keeps VBV on the serial path. Warmed static. */
static int rcp_vbv_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RC_PIPE_VBV"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Proportional overshoot charge for in-flight VBV entries: the virtual
 * buffer charges r_hi * vpred per pending frame, and the burst gate falls
 * back to serial when even that bound approaches the constraint. Corpus
 * measurement (12 runs, ~3k frames): the bits/vpred distribution has p50
 * ~0.8-0.9 and p90 ~1.8-2.3, but an UNBOUNDED tail (near-zero predictions,
 * regime shifts) -- so r_hi covers the proportional body only, and the tail
 * is carried by the extrapolation guards + the shock multiplier + the
 * content-jump gate (see below), which the compliance battery validates.
 * Warmed static (driver-thread decides). */
static double vbv_rhi_env(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_VBV_RHI"); v = s ? atof(s) : 2.0; }
    return v;
}

/* Y264_VBV_FORCE=1: measurement-only -- skip the serial fallback so every
 * burst pipelines (prediction-error instrumentation needs in-flight entries).
 * Config-deterministic like every rcp input; never set in production. */
static int vbv_force_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_FORCE"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Y264_VBV_BOUND=1: bound EVERY frame against its measured coded size, not
 * just the instance's first. Default off -- it costs the stair, for the reason
 * vbv_bound_all sets out. Resolved in warm_lr_statics. */
static int vbv_bound_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_BOUND"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Extrapolation guards (both warmed statics). QPD: how far below the model's
 * calibrated QP regime a PIPELINED B decide may go (the serial schedule has
 * no floor -- its per-frame actuals catch a dive within one frame; a flying
 * burst decides bframes+1 QPs blind, and the bits model under-predicts ~2x
 * per 6 QP of downward extrapolation on static content). CJUMP: complexity
 * ratio past the calibrated content regime at which the burst gate goes
 * tight (the missed-cut / fade-end shape: the scale is calibrated on the old
 * regime and no multiplier covers the jump). */
static int vbv_qpd_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_QPD"); v = s ? atoi(s) : 6; }
    return v;
}

static double vbv_cjump_env(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_VBV_CJUMP"); v = s ? atof(s) : 4.0; }
    return v;
}

/* Y264_VBV_STAT=1: report burst/fallback/clamp counts at encoder close. */
static int vbv_stat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_STAT"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Replicate one input plane into an MB-aligned working plane, extending the
 * right and bottom edges into the padding so full macroblocks are always
 * available. The padded region is cropped away by the SPS on decode. */
static void pad_plane(pixel *dst, int dstride, int dw, int dh,
                      const pixel *src, int sstride, int sw, int sh)
{
    for (int y = 0; y < dh; y++) {
        int sy = (y < sh) ? y : sh - 1;
        const pixel *s = src + (size_t)sy * sstride;
        pixel *d = dst + (size_t)y * dstride;
        int x = 0;
        for (; x < sw; x++)
            d[x] = s[x];
        pixel edge = s[sw - 1];
        for (; x < dw; x++)
            d[x] = edge;
    }
}

static void pad_input_to(yah264_encoder_t *e, const yah264_picture_t *pic,
                         pixel *const dst[3])
{
    pad_plane(dst[0], e->pstride[0], e->padded_w, e->padded_h,
              pic->plane[0], pic->stride[0], e->width, e->height);
    pad_plane(dst[1], e->pstride[1], e->padded_w / e->sub_w, e->padded_h / e->sub_h,
              pic->plane[1], pic->stride[1], e->width / e->sub_w, e->height / e->sub_h);
    pad_plane(dst[2], e->pstride[2], e->padded_w / e->sub_w, e->padded_h / e->sub_h,
              pic->plane[2], pic->stride[2], e->width / e->sub_w, e->height / e->sub_h);
}

static void pad_input(yah264_encoder_t *e, const yah264_picture_t *pic)
{
    pad_input_to(e, pic, e->plane);
}

/* Reference-plane geometry: every plane (source, recon, references) is
 * allocated with an edge border (mc.h constants) and handed around by its
 * interior pointer; strides include the borders, so all row arithmetic is
 * unchanged. extend_borders replicates a recon's edges outward once per
 * stored frame, after which out-of-frame MC positions read the borders
 * directly -- bit-identical to the spec's coordinate clamping. */
static void plane_free(pixel *interior, int w, int b);

/* The buffers a slot holds right now. Captured by anything that writes a
 * picture's CONTENT later than it took the slot -- the anchor trailer and the
 * reference-B content commit both do -- because a slot recycled again in the
 * meantime points at the NEXT picture's bag, and the deferred write would land
 * in it. With slot-owned buffers the distinction does not exist and a plain
 * slot dereference is enough; the pool is what makes it real. */
static struct dpb_bag dpbp_bag_of(const struct dpb_entry *d)
{
    struct dpb_bag g;
    for (int c = 0; c < 3; c++) { g.plane[c] = d->plane[c]; g.hpel[c] = d->hpel[c]; }
    g.mvx = d->mvx; g.mvy = d->mvy; g.refidx = d->refidx; g.colpoc = d->colpoc;
    return g;
}

static void dpbp_open(yah264_encoder_t *e, size_t mvcount);
static void dpbp_bag_free(yah264_encoder_t *e, struct dpb_bag *g,
                          const int pw[3], const int pb[3])
{
    for (int c = 0; c < 3; c++) {
        plane_free(g->plane[c], pw[c], pb[c]);
        plane_free(g->hpel[c], e->padded_w, Y264_LUMA_BORDER);
    }
    free(g->mvx); free(g->mvy); free(g->refidx); free(g->colpoc);
}

/* G1-B probe (Y264_PLANE_PAD): unused columns appended to every picture plane's
 * stride. Nothing reads them -- the interior, both borders and every offset are
 * where they always were -- so the encode is byte-identical and the work volume
 * provably constant, and the only thing that moves is how far apart consecutive
 * rows of an MB-sized window sit. That makes it a direct read on F11's premise
 * that dragging 16 rows across a frame stride costs cache. Default 0. */
static int plane_pad(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("Y264_PLANE_PAD");
        v = s ? atoi(s) : 0;
        if (v < 0) v = 0;
    }
    return v;
}

static pixel *plane_alloc(int w, int h, int b)
{
    size_t stride = (size_t)w + 2 * b + plane_pad();
    pixel *base = malloc(stride * (h + 2 * b) * sizeof(pixel));
    return base ? base + (size_t)b * stride + b : NULL;
}

static void plane_free(pixel *interior, int w, int b)
{
    if (interior)
        free(interior - (size_t)b * ((size_t)w + 2 * b + plane_pad()) - b);
}

/* Claim slot k of the half-pel FALLBACK set, allocating it the first time the
 * slot is actually needed. Returns 0 only on OOM.
 *
 * The set is dead on the pyramid path (every inter reference is a DPB picture
 * with a cached triple) and near-dead on the flat path, so allocating it at open
 * bought 3 full-frame planes per reference of pages that were never written.
 *
 * The lock is a static rather than a per-encoder field because it is taken at
 * most 17 times per encoder, only on the first miss for a slot: build_slice_prep
 * runs on a chain driver as well as on the API thread, so two leaves can reach
 * this at once. */
static int hpel_buf_take(yah264_encoder_t *e, int k)
{
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    int ok = 1;
    pthread_mutex_lock(&lk);
    if (!e->hpel_buf[k][0]) {
        pixel *p[3];
        for (int c = 0; c < 3; c++)
            if (!(p[c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER))) {
                while (c-- > 0) plane_free(p[c], e->padded_w, Y264_LUMA_BORDER);
                ok = 0;
                break;
            }
        if (ok)
            for (int c = 2; c >= 0; c--) e->hpel_buf[k][c] = p[c];
    }
    pthread_mutex_unlock(&lk);
    return ok;
}

static void extend_plane(pixel *p, int stride, int w, int h, int b)
{
    for (int y = 0; y < h; y++) {
        pixel *row = p + (size_t)y * stride;
        pixel lval = row[0], rval = row[w - 1];
        for (int x = 0; x < b; x++) { row[-b + x] = lval; row[w + x] = rval; }
    }
    for (int y = 1; y <= b; y++) {
        memcpy(p - (size_t)y * stride - b, p - b, ((size_t)w + 2 * b) * sizeof(pixel));
        memcpy(p + (size_t)(h - 1 + y) * stride - b,
               p + (size_t)(h - 1) * stride - b, ((size_t)w + 2 * b) * sizeof(pixel));
    }
}

static void extend_borders(yah264_encoder_t *e, pixel *const planes[3])
{
    extend_plane(planes[0], e->pstride[0], e->padded_w, e->padded_h,
                 Y264_LUMA_BORDER);
    for (int c = 1; c < 3; c++)
        extend_plane(planes[c], e->pstride[c], e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                     Y264_CHROMA_BORDER);
}

/* type: 0 = I, 1 = P, 2 = B. is_ref: whether this frame is a reference (drives
 * nal_ref_idc / dec_ref_pic_marking). src[] are the source planes to encode. */
/* Per-frame QP from the base (P) QP. I frames drop a few QP (they are referenced
 * most; ip_ratio ~1.4 -> ~3 QP), B frames rise (pb_ratio; referenced B less). */
/* Temporal-layer QP cascade for a B frame: deeper B's (nearer the leaves,
 * referenced by fewer or no frames) take a larger penalty. Depth 0 is the
 * flat-B case (bframes 1, no pyramid); the pyramid sets depth >= 1. Split out
 * of frame_qp so build_slice_prep can hand the same number to the frame
 * struct (lambda_casc) for the Y264_MB_LAMBDA=7 probe. */
/* Y264_ABR_PBRATE=1: under the rate-factor ABR controller, scale the B QP
 * cascade by the TARGET rate. On the 29-clip table the controller lost to
 * the previous model only at the top of the rate range (riverbed 12.5 and
 * crowd_run 24 Mbit/s), and the cascade at 0 recovered ducks (-7.4) while
 * costing sintel (+6.7): the two ends want opposite cascades, keyed by bits
 * per pixel per frame. Scale 1 at or below Y264_ABR_PBRATE_LO bpp (default
 * 0.10), 0 at or above _HI (0.40), linear between. A rate-keyed rule, not a
 * content-feature selector (that class is closed).
 * DEFAULT ON (2026-09-03). ABR ladders vs the unscaled controller, t1,
 * 120f: ducks -7.50%, riverbed -1.52, crowd_run -1.17, park_joy -0.24;
 * samsung, sintel, foreman, stockholm +0.00 (scale 1 there, byte-identical).
 * =0 restores the flat cascade. */
/* A6 (Y264_ABR_VBVOV, default 0.25; 0 disables): with a VBV the rate-factor
 * integrator's tolerance window is capped at this fraction of the buffer.
 * The plain-ABR window is 2 x tolerance x bitrate x sqrt(seconds elapsed),
 * seconds of bits, so a sustained overspend of most of a one-second buffer
 * reads as a +18% nudge and the buffer drains into the next IDR (ducks CBR:
 * +17.9% and seven underflows). Tying the window to the buffer makes the
 * same drift saturate the overflow term at 2x, which is the response CBR
 * needs. 0.25 beat 0.5 on every 720p cell (ducks CBR +2.2% vs +4.2%, park_joy
 * +1.4% vs +2.7%) and 1.0 lost on all of them; below 0.25 was not swept.
 * Without a VBV the window is unchanged, so plain ABR is untouched. */
static double abr_vbvov_frac(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_ABR_VBVOV"); v = s ? atof(s) : 0.25; if (v < 0) v = 0; }
    return v;
}
/* A5b (Y264_ABR_RF2_INFLIGHT, default on): under the rate-factor controller
 * an in-flight frame (decided, not yet coded) is predicted from ITS OWN
 * decide complexity times a per-type EMA of contribution per unit of
 * complexity, instead of the type's mean contribution. Only reachable with
 * Y264_RCP_LAG >= 1: at lag 0 every anchor drains the ring before it
 * decides, so the default schedule is byte-identical either way. The mean
 * form is what made the lag fail on sintel (+14.4% BD): a lagged burst over
 * the near-black opening credits each in-flight frame at the mean cost, far
 * above what a black frame spends, and the factor drifts before the actuals
 * land. */
static int abr_rf2_inflight_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_ABR_RF2_INFLIGHT"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
static int abr_pbrate_env(double *lo_out, double *hi_out)
{
    static int on = -1; static double lo = 0.10, hi = 0.40;
    if (on < 0) {
        const char *s = getenv("Y264_ABR_PBRATE"); on = s ? (atoi(s) ? 1 : 0) : 1;
        s = getenv("Y264_ABR_PBRATE_LO"); if (s) lo = atof(s);
        s = getenv("Y264_ABR_PBRATE_HI"); if (s) hi = atof(s);
    }
    if (lo_out) *lo_out = lo;
    if (hi_out) *hi_out = hi;
    return on;
}
static double abr_pbrate_scale(const yah264_encoder_t *e)
{
    double lo, hi;
    int on = abr_pbrate_env(&lo, &hi);
    if (!on || !e->abr_rf2 || !e->mbtree_on || e->width <= 0 || e->height <= 0) return 1.0;
    double bpp = e->abr_target_bpf / ((double)e->width * e->height);
    if (bpp <= lo) return 1.0;
    if (bpp >= hi || hi <= lo) return 0.0;
    return (hi - bpp) / (hi - lo);
}
static int frame_b_casc(const yah264_encoder_t *e, int is_ref)
{
    int d = e->cur_b_depth;
    int casc = d <= 0 ? (is_ref ? 1 : 3) : (is_ref ? d : d + 1);
    if (crf_pb0_env() && (e->crf_on || e->abr_rf2) && e->mbtree_on)
        casc = (int)lround(crf_pbscale_env() * casc);   /* B offset 0 under mb-tree; RF2 too (measurement) */
    casc = (int)lround(abr_pbrate_scale(e) * casc);
    return casc;
}

static int fqp_trace_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_FQP_TRACE"); v = s ? atoi(s) : 0; }
    return v;
}

/* The zone (yah264_encoder_set_zones) holding input frame `disp`, or NULL.
 * Binary search; the array is sorted and non-overlapping. */
static const yah264_zone_t *zone_at(const yah264_encoder_t *e, int disp)
{
    int lo = 0, hi = e->nzones - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const yah264_zone_t *z = &e->zones[mid];
        if (disp < z->first) hi = mid - 1;
        else if (disp > z->last) lo = mid + 1;
        else return z;
    }
    return NULL;
}

/* The coded-QP bounds, applied at the one place every path's coded QP goes
 * through. e->qp_min / e->qp_max resolve to 0 and 51 with no --qpmin/--qpmax,
 * which is exactly the clamp this replaces. The frame-TYPE offsets are applied
 * first and clamped after, so a bound is a bound on what is actually coded and
 * not on the base QP the offsets are taken from. */
static inline int qp_bound(const yah264_encoder_t *e, int q)
{
    if (q < e->qp_min) q = e->qp_min;
    if (q > e->qp_max) q = e->qp_max;
    return q;
}

static int frame_qp(const yah264_encoder_t *e, int type, int is_ref)
{
    int q = e->qp;
    if (e->nzones) {                                /* the orchestrator's per-range offset */
        const yah264_zone_t *z = zone_at(e, e->cur_disp);
        if (z) q += (int)lround(z->qp_offset);
    }
    if (e->tp_pass >= 2)                            /* 2-pass sets the coded QP directly */
        return qp_bound(e, q);
    if (type == 0) q -= 3;
    else if (type == 2) q += frame_b_casc(e, is_ref);
    q = qp_bound(e, q);
    /* Y264_FQP_TRACE: this function is NOT pure -- it reads e->qp and
     * e->cur_b_depth, and the depth cascade only applies to B. If one frame gets
     * two different answers the bitstream's slice QP and the QP its recon was
     * built with have parted company, which is drift. Measurement hook only. */
    if (fqp_trace_on())
        fprintf(stderr, "FQP disp=%d type=%d ref=%d depth=%d eqp=%d -> %d\n",
                e->cur_disp, type, is_ref, e->cur_b_depth, e->qp, q);
    return q;
}

/* Estimate an explicit luma weight+offset for a P frame from the frame-level DC
 * ratio of source to one list-0 reference (a fade shows as a global luma scale).
 * Returns 1 and fills the weight/offset (at 2^denom) when a fade is detected. */
static int adme_thresh(void);
static int adme_log(void);
static int psy_flat_gate(int idx);
static int psy_flat_log(void);
static int psy_calm_gate(int idx);

/* sumref_ovr >= 0 substitutes for the reference-plane sum -- the v3 staircase
 * passes the previous anchor's SOURCE-plane sum there (a serial-time constant),
 * because the anchor's RECON may still be streaming when this P slice preps.
 * Fixed by the env gate, so bits stay thread-count-invariant. */
static int estimate_wp_luma(yah264_encoder_t *e, const pixel *src,
                            const pixel *ref, int denom, int64_t sumref_ovr,
                            int *w, int *o)
{
    int W = e->width, H = e->height, ss = e->pstride[0], rs = e->pstride[0];
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
        return 0;                                   /* no meaningful global shift */
    int ww = (int)((sumsrc * scale + sumref / 2) / sumref);
    if (ww < 1) ww = 1; else if (ww > 127) ww = 127;
    int64_t num = sumsrc * scale - sumref * ww;
    int oo = (int)((num + (num >= 0 ? N * scale / 2 : -(N * scale / 2))) / (N * scale));
    /* oo is a sample-domain offset; luma_offset_l0 is signaled in the 8-bit
 * domain (the decoder scales it by 1 << (BitDepth-8)). Reduce with rounding. */
#if Y264_BIT_DEPTH > 8
    { int half = 1 << (Y264_BIT_DEPTH - 9);
      oo = (oo >= 0 ? (oo + half) : (oo - half)) >> (Y264_BIT_DEPTH - 8); }
#endif
    if (oo < -128) oo = -128; else if (oo > 127) oo = 127;
    if (ww == scale && oo == 0)
        return 0;
    *w = ww; *o = oo;
    return 1;
}

/* Interior luma sum of a padded source plane (estimate_wp_luma's sumref
 * region), cached per anchor for the v3 depth wp substitute. */
static uint64_t src_luma_sum(const yah264_encoder_t *e, const pixel *src)
{
    uint64_t sum = 0;
    int W = e->width, H = e->height, ss = e->pstride[0];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            sum += src[y * ss + x];
    return sum;
}

/* Build this slice's RefPicList0 as plane pointers + POCs, mirroring the
 * decoder's list exactly. Non-pyramid: the refring (anchors most-recent-first,
 * which is both the P default PicNum-descending order and the B past-refs
 * POC-descending order). Pyramid: from the DPB — P sorts by PicNum descending
 * and pins cur_ref_l0_fn first (matching the emitted reorder command); B takes
 * past references by POC descending (the default initialisation, list 0 is
 * bounded to past refs so list 1's future anchor never aliases it). Returns
 * num_ref_idx_l0_active. */
static int build_list0(yah264_encoder_t *e, int type,
                       const pixel *pl[16][3], int poc[16])
{
    if (!e->b_pyramid) {
        if (e->nref <= 1) {
            for (int c = 0; c < 3; c++) pl[0][c] = e->ref[c];
            poc[0] = e->ref0_poc;
            return 1;
        }
        int n = e->nref_valid < 1 ? 1 : (e->nref_valid > e->nref ? e->nref : e->nref_valid);
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 3; c++) pl[i][c] = e->refring[i][c];
            poc[i] = e->refring_poc[i];
        }
        return n;
    }
    int cand[16], nc = 0;
    for (int i = 0; i < e->dpb_size && nc < 16; i++)
        if (e->dpb[i].used) cand[nc++] = i;
    if (type == 1) {
        /* PicNum descending == smallest FrameNum distance below current first. */
        int maxfn = 1 << (e->sps.log2_max_frame_num_minus4 + 4);
        for (int i = 1; i < nc; i++)
            for (int j = i; j > 0; j--) {
                int dj = (e->frame_num - e->dpb[cand[j]].frame_num + maxfn) % maxfn;
                int dp = (e->frame_num - e->dpb[cand[j - 1]].frame_num + maxfn) % maxfn;
                if (dj < dp) { int t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t; }
                else break;
            }
        if (e->cur_ref_l0_fn >= 0)                 /* pin the reordered anchor first */
            for (int i = 0; i < nc; i++)
                if (e->dpb[cand[i]].frame_num == e->cur_ref_l0_fn) {
                    int t = cand[i];
                    for (int j = i; j > 0; j--) cand[j] = cand[j - 1];
                    cand[0] = t;
                    break;
                }
    } else {
        int np = 0;                                /* past refs only, POC descending */
        for (int i = 0; i < nc; i++)
            if (e->dpb[cand[i]].poc < e->poc) cand[np++] = cand[i];
        nc = np;
        for (int i = 1; i < nc; i++)
            for (int j = i; j > 0; j--)
                if (e->dpb[cand[j]].poc > e->dpb[cand[j - 1]].poc) {
                    int t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t;
                } else break;
    }
    int n = nc < e->nref ? nc : e->nref;
    if (n < 1) n = 1;
    for (int i = 0; i < n && i < nc; i++) {
        for (int c = 0; c < 3; c++) pl[i][c] = e->dpb[cand[i]].plane[c];
        poc[i] = e->dpb[cand[i]].poc;
    }
    if (nc == 0) {                                 /* should not happen; fall back */
        for (int c = 0; c < 3; c++) pl[0][c] = e->ref[c];
        poc[0] = e->ref0_poc;
    }
    return n;
}

/* One reference's half-pel build, split into row BANDS across the pool. This is
 * the build dpb_store runs for every stored reference -- 106 ms of 2.25 s at 18
 * threads, the largest thing that was hiding in the profiler's "unattributed"
 * bucket. Each band is an independent pure 6-tap over the source, so any thread
 * assignment is bit-identical; per-worker horizontal scratch avoids sharing.
 * Scratch is sized per BAND (rows/band + 5 overlap), not per frame, so the total
 * stays about one frame's worth however many workers there are. */
struct hpel_band_ctx { yah264_encoder_t *e; pixel *H, *V, *C; const pixel *src;
                       int sstride, border, band; };
static int hpel_ensure_ws(yah264_encoder_t *e, int nws, size_t bytes)
{
    if (nws > 64) return 0;
    if (nws <= e->hpel_ws_n && bytes <= e->hpel_ws_bytes) return 1;
    if (bytes > e->hpel_ws_bytes) {         /* grow every existing band buffer */
        for (int w = 0; w < e->hpel_ws_n; w++) {
            void *n = realloc(e->hpel_scratch_ws[w], bytes);
            if (!n) return 0;
            e->hpel_scratch_ws[w] = n;
        }
        e->hpel_ws_bytes = bytes;
    }
    for (int w = e->hpel_ws_n; w < nws; w++) {
        e->hpel_scratch_ws[w] = malloc(e->hpel_ws_bytes);
        if (!e->hpel_scratch_ws[w]) return 0;
        e->hpel_ws_n = w + 1;
    }
    return 1;
}
static void hpel_band_one(void *ctx, int tid, int k)
{
    struct hpel_band_ctx *c = ctx;
    yah264_encoder_t *e = c->e;
    int y0 = -c->border + k * c->band, y1 = y0 + c->band;
    int ymax = e->padded_h + c->border;
    if (y1 > ymax) y1 = ymax;
    y264_mc_build_hpel_rows(c->H, c->V, c->C, e->pstride[0], c->src, e->pstride[0],
                            e->padded_w, e->padded_h, c->border,
                            e->hpel_scratch_ws[tid], c->sstride, y0, y1);
}
/* --- lazy-hpel probe. Two env gates, both dead
 * by default and neither on any hot path (a few calls per frame):
 * Y264_HPEL_PROBE=1 accumulate the wall time of every half-pel build and
 * print it at close. Single-threaded measurement.
 * Y264_HPEL_DOUBLE=1 build every plane TWICE. Byte-identical by construction
 * (the second build writes the same pixels), so the arm's
 * delta prices one full build's marginal cost without the
 * output moving. The ceiling arm runs in this direction
 * because the honest one (build less) cannot be
 * byte-identical without an oracle.
 * The census (which bands are ever read) lives in me.c. */
static int hpel_probe_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_HPEL_PROBE"); v = s && atoi(s) ? 1 : 0; }
    return v;
}
static int hpel_double_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_HPEL_DOUBLE"); v = s ? atoi(s) : 1; }
    return v;
}
static double g_hpb_ms;
static long long g_hpb_calls, g_hpb_rows;
static void hpb_add(double ms, int rows) { g_hpb_ms += ms; g_hpb_calls++; g_hpb_rows += rows; }
__attribute__((destructor)) static void hpb_dump(void)
{
    if (hpel_probe_on() && g_hpb_calls)
        fprintf(stderr, "=== Y264_HPEL_PROBE: build %.1f ms in %lld calls, %lld rows ===\n",
                g_hpb_ms, g_hpb_calls, g_hpb_rows);
}

/* Build one reference's half-pel planes, on `pool` when it is worth it. */
static void hpel_build_once(yah264_encoder_t *e, ntp_pool_t *pool,
                            pixel *H, pixel *V, pixel *C, const pixel *src)
{
    int B = Y264_LUMA_BORDER, sstride = e->padded_w + 2 * B;
    int nt = pool ? ntp_pool_nthreads(pool) : 0;
    int rows = e->padded_h + 2 * B;
    if (nt > 1) {
        int band = (rows + nt - 1) / nt;
        if (band < 32) band = 32;                       /* keep bands worth a task */
        int nb = (rows + band - 1) / band;
        size_t need = (size_t)sstride * (size_t)(band + 5) * sizeof(int32_t);
        if (nb > 1 && hpel_ensure_ws(e, nt, need)) {
            struct hpel_band_ctx c = { e, H, V, C, src, sstride, B, band };
            ntp_prof_tag("hpel_band"); ntp_prio_hint();
            ntp_parallel_for(pool, nb, hpel_band_one, &c);
            return;
        }
    }
    y264_mc_build_hpel(H, V, C, e->pstride[0], src, e->pstride[0],
                       e->padded_w, e->padded_h, B, e->hpel_scratch, sstride);
}
static void hpel_build_ref_on(yah264_encoder_t *e, ntp_pool_t *pool,
                              pixel *H, pixel *V, pixel *C, const pixel *src)
{
    const int B = Y264_LUMA_BORDER;
    double t0 = hpel_probe_on() ? tprof_ms() : 0;
    y264_hpel_census_built(H, -B, e->padded_h + B, e->pstride[0]);
    for (int pass = hpel_double_on(); pass > 0; pass--)
        hpel_build_once(e, pool, H, V, C, src);
    if (hpel_probe_on()) hpb_add(tprof_ms() - t0, e->padded_h + 2 * B);
}
static void hpel_build_ref(yah264_encoder_t *e, pixel *H, pixel *V, pixel *C,
                           const pixel *src)
{
    hpel_build_ref_on(e, e->pool, H, V, C, src);
}

/* Slice header + per-frame f setup (hpel, motion reset), shared by the serial
 * build_slice and the W2 pipeline. Writes the header into `rbsp`, leaves the
 * bitstream writer / frame / SliceQPY / deblock flag in the out params. Does NOT
 * run analyze/emit -- the caller drives those. */
/* MT frame-pipeline Step 1: per-in-flight-frame
 * working buffers that a parallel frame will need its own of. The serial path uses
 * the default context (fw_default), which points at the encoder's shared buffers,
 * so this parameterization is byte-identical until parallel slots are allocated and
 * wired (a later step). build_slice_prep is the one place `f` is wired from `e->`. */
struct frame_work {
    /* Y264_DIRECT_AUTO plumbing. dauto_acc is where the frame's skippability
 * counts accumulate. dauto_valid says the owning stair burst already decided
 * the slice's direct mode (dauto_temporal) at its launch, from a score that
 * is a pure function of the launch sequence; without it a threaded frame
 * leaves param.direct standing. */
    long            *dauto_acc;
    int              dauto_valid, dauto_temporal;
    /* Reference-B mb-tree field, carried per FRAME rather than read from an
 * encoder-global slot: on the stair the walk and the B's emit overlap, so a
 * shared slot races. NULL = this frame has none. */
    const int8_t    *mbtoff_b;
    pixel           *rec[3];
    pixel           *ref1[3];       /* the list-1 reference planes this slice reads */
    int16_t         *colmvx, *colmvy;
    int8_t          *colref;
    int16_t         *colpoc;
    /* The colocated picture's own list-0[0] POC for the slice-level temporal
 * legality guard. The serial and W2 paths leave it invalid and the guard
 * reads e->col_l0poc0, which set_b_refs fills; the staircase preps a leaf
 * before set_b_refs runs for it, so it hands the value in here (it read
 * -1 there and the guard was inert under the staircase until 2026-09-03). */
    int              col_l0poc0_valid, col_l0poc0;
    y264_hpel_ref_t *hpel_ctx;      /* the per-frame half-pel plane registry (array[17]) */
    int16_t        **bseed_cur;     /* the 4 POC-scaled B-seed grids */
    int8_t          *refidx, *refidx1;  /* the motion fields prep RESETS (per-slot,
 * so a leaf prep never zaps a live frame's) */
    /* SOURCE bank the B pair seeds are scaled FROM (default: e->bseed et al).
 * A staircase burst captures its own bank at launch, so an in-flight
 * chain's prep never reads the arrays new arrivals are filling. */
    int16_t *const (*bseed_src)[4];
    const int       *bseed_valid, *bseed_poc0, *bseed_poc1;
    /* Y264_BLATE_STAT's per-MB cost bank, from the same arrays and with the
 * same hazard: NULL on the async stair, whose chain preps while arrivals
 * refill e->bseedc. Measurement is t1-only, so NULL costs nothing there. */
    int32_t *const (*bseedc_src)[3];
    /* P lowres ME seed source (default: e->lr_seed_*). The v3 async anchor
 * points these at its private copies -- the shared arrays are rewritten by
 * every later pop while its analyze is still reading the seeds. */
    const int16_t   *lr_seed_mvx, *lr_seed_mvy;
    const int32_t   *lr_seed_cost;
    /* v5 (Y264_STAIR_BDEPTH): the POC of this mini-GOP's reference B when the
 * burst shape is one the reference-B staircase covers, else -1. A B slice
 * whose list-1 or list-0 resolves to it takes the fixed vertical clamp.
 * A staircase burst passes its OWN captured shape (arrivals rewrite
 * e->nbuf/e->bpoc while an async chain preps). */
    int              refb_poc;
};
static void fw_default(const yah264_encoder_t *e, struct frame_work *fw)
{
    for (int c = 0; c < 3; c++) fw->rec[c] = e->rec[c];
    for (int c = 0; c < 3; c++) fw->ref1[c] = e->cur_l1p[c] ? e->cur_l1p[c] : e->ref1[c];
    fw->colmvx = e->colmvx; fw->colmvy = e->colmvy;
    fw->colref = e->colref; fw->colpoc = e->colpoc;
    fw->col_l0poc0_valid = 0; fw->col_l0poc0 = -1;
    fw->hpel_ctx = (y264_hpel_ref_t *)e->hpel_ctx;
    fw->bseed_cur = (int16_t **)e->bseed_cur;
    fw->refidx = e->refidx; fw->refidx1 = e->refidx1;
    fw->bseed_src = (int16_t *const (*)[4])e->bseed;
    fw->bseedc_src = (int32_t *const (*)[3])e->bseedc;
    fw->bseed_valid = e->bseed_valid;
    fw->bseed_poc0 = e->bseed_poc0;
    fw->bseed_poc1 = e->bseed_poc1;
    fw->lr_seed_mvx = e->lr_seed_mvx;
    fw->lr_seed_mvy = e->lr_seed_mvy;
    fw->lr_seed_cost = e->lr_seed_cost;
    fw->refb_poc = stair_refb_poc(e->nbuf, e->bpoc);
    /* Serial path: the walk and the B's emit cannot overlap, so the encoder-wide
 * field is safe to name here. The stair OVERRIDES this with its burst-owned
 * copy, because there they DO overlap. */
    fw->mbtoff_b = NULL;
    fw->dauto_acc = (long *)e->dauto_pending;
    fw->dauto_valid = 0; fw->dauto_temporal = 0;
    if (e->mbtree_on && e->cur_bseed >= 0 && e->cur_bseed < 8
        && e->bmbtree_valid[e->cur_bseed])
        fw->mbtoff_b = e->bmbtree_off[e->cur_bseed];
}

static int stair_clamp_on(const yah264_encoder_t *e);
static int stair_refbgate_rc_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_STAIR_REFBGATE_RC"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}

/* v6 eligibility, in ONE place because two sides have to agree exactly: the
 * clamp set built in build_slice_prep, and the launch-side decision to replace a
 * reference-B content wait with a row gate. If the second is ever true where the
 * first is false, an anchor reads a streaming picture with no bound on how far
 * down it looks. Both call this, neither restates it.
 *
 * Every term is static -- env gates and e->nref, which is fixed at open. The
 * clamp must be a function of parameters and coding order alone (this encoder's
 * bitstream is invariant to thread count on purpose); only the SKIP is allowed
 * to consult what is live, and skipping less is always safe. */
static int stair_refbgate_elig(const yah264_encoder_t *e)
{
    /* The rate-control pipeline used to refuse the gate outright: at zero lag
 * the anchor's decide drained the reference B for its bits anyway, so the
 * gate had nothing to replace. With a lag budget (rcp_lag > 0, static at
 * open) the decide runs a burst ahead and the launch-side content wait was
 * the whole ABR penalty: foreman 180f t12, 0 of 41 launches gated and 59 ms
 * of a 110 ms encode blocked on reference-B content, against 44 of 44 gated
 * and 12 ms under CRF (2026-09-04, Y264_STAIR_STAT). Y264_STAIR_REFBGATE_RC=0
 * restores the refusal for A/B. */
    return stair_refbgate_on() && stair_bdepth_on() && stair_wide_on() &&
           stair_depth_on() && stair_clamp_on(e) && e->nref >= 2 &&
           (!e->rcp_on || (e->rcp_lag > 0 && stair_refbgate_rc_on()));
}

/* The per-POC source-DC cache a clamped list-0 reference is estimated against
 * (see e->anchor_srcsum). Written once per anchor in coding order, on the API
 * thread, after that anchor's own prep has consumed the previous value. */
static void anchor_srcsum_put(yah264_encoder_t *e, int poc, uint64_t sum)
{
    int w = e->anchor_srcsum_w;
    e->anchor_srcsum[w].poc = poc;
    e->anchor_srcsum[w].sum = sum;
    e->anchor_srcsum[w].valid = 1;
    e->anchor_srcsum_w = (w + 1) % Y264_STAIR_K;
}

/* Newest-first, so a POC that repeats across an IDR can never resolve to the
 * older entry, and so a depth-2 lookup (always "the previous anchor") returns
 * what the single scalar this replaced would have. -1 = no entry. */
static int64_t anchor_srcsum_get(const yah264_encoder_t *e, int poc)
{
    for (int i = 1; i <= Y264_STAIR_K; i++) {
        int k = (e->anchor_srcsum_w - i + Y264_STAIR_K) % Y264_STAIR_K;
        if (e->anchor_srcsum[k].valid && e->anchor_srcsum[k].poc == poc)
            return (int64_t)e->anchor_srcsum[k].sum;
    }
    return -1;
}

/* Is this POC in a slice's list-0 clamp set? The set is packed newest-first, so
 * the scan stops at the first empty slot. The twin of macroblock.c's
 * stair_l0_clamp, which asks the same question of the same set from the ME side
 * -- kept as two three-line functions rather than one shared header inline
 * because the two sides key on different things (a POC here, a list index
 * there) and neither is worth a dependency between the files. */
static int clamp_set_has(const int *set, int poc)
{
    for (int h = 0; h < Y264_STAIR_HOPS && set[h] >= 0; h++)
        if (set[h] == poc)
            return 1;
    return 0;
}

static void anchor_srcsum_reset(yah264_encoder_t *e)
{
    for (int k = 0; k < 2 * Y264_STAIR_K; k++) e->anchor_srcsum[k].valid = 0;
    e->anchor_srcsum_w = 0;
}

/* v6: the reference-B POC history, pushed once per anchor in coding order on
 * the same two paths that shift prev_anchor_poc, and reset at an IDR for the
 * same reason (POC restarts, so a previous GOP's entry would become reachable
 * as a same-numbered key). A pure function of the burst's buffered-B shape --
 * never of whether that burst's reference B was actually PIPELINED, which is a
 * runtime property. The clamp keys on this; only the decision to skip a wait
 * keys on liveness. */
static void refb_hist_push(yah264_encoder_t *e, int poc)
{
    for (int k = Y264_STAIR_K - 1; k > 0; k--) e->refb_hist[k] = e->refb_hist[k - 1];
    e->refb_hist[0] = poc;
}

static void refb_hist_reset(yah264_encoder_t *e)
{
    for (int k = 0; k < Y264_STAIR_K; k++) e->refb_hist[k] = -1;
}

extern long y264_dscore_ssd, y264_dscore_n, y264_dscore_skip[2];
int y264_mb_dauto_stride(void);   /* the auto score's sample stride (macroblock.c); its decay bound scales with it */
    extern long y264_tdir_mb[2];

static void build_slice_prep(yah264_encoder_t *e, int type, int is_idr, int is_ref,
                             pixel *const src[3], uint8_t *rbsp, size_t rbsp_cap,
                             const struct frame_work *fw,
                             y264_bs_t *bs_out, y264_frame_t *f_out,
                             int *fqp_out, int *deblock_out)
{
    y264_bs_t bs;
    y264_bs_init(&bs, rbsp, rbsp_cap);
    int fqp = frame_qp(e, type, is_ref);
    int fcqp = y264_chroma_qp(fqp, e->param.chroma_qp_index_offset);
    if (e->fstats_count < (int)(sizeof e->fstats / sizeof e->fstats[0])) {
        yah264_frame_stats_t *st = &e->fstats[e->fstats_count++];
        st->disp = e->cur_disp; st->type = type; st->is_idr = is_idr; st->is_ref = is_ref; st->qp = fqp;
    }
    /* num_ref_idx_l0_active for this slice (list 1 stays single-ref for B). */
    const pixel *l0p[16][3];
    int l0poc[16];
    int active_ref = (type != 0) ? build_list0(e, type, l0p, l0poc) : 1;
    /* Capture this frame's list POCs: when its recon later serves as the
 * co-located picture, colpoc resolves each block's refIdx to a POC. */
    e->cur_l0n = (type != 0) ? active_ref : 0;
    for (int i = 0; i < e->cur_l0n; i++) e->cur_l0poc[i] = l0poc[i];
    e->cur_l1poc0 = (type == 2) ? e->ref1_poc : -1;

    int slice_type = type == 0 ? 7 : (type == 1 ? 5 : 6);   /* I=7 P=5 B=6 */
    y264_bs_write_ue(&bs, 0);                       /* first_mb_in_slice */
    y264_bs_write_ue(&bs, slice_type);
    y264_bs_write_ue(&bs, e->pps.pps_id);
    int frame_num_bits = e->sps.log2_max_frame_num_minus4 + 4;
    y264_bs_write(&bs, frame_num_bits, e->frame_num);
    /* A sequence that may carry fields has every slice say which it is. Every
 * picture here is a frame picture, so the flag is always 0 and no
 * bottom_field_flag follows it. */
    if (!e->sps.frame_mbs_only_flag)
        y264_bs_write1(&bs, 0);                     /* field_pic_flag */
    if (is_idr)
        y264_bs_write_ue(&bs, e->idr_pic_id);       /* idr_pic_id */
    if (e->sps.pic_order_cnt_type == 0) {
        int poc_bits = e->sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
        y264_bs_write(&bs, poc_bits, e->poc & ((1 << poc_bits) - 1));
    }
    /* B direct mode for this slice: temporal needs every co-located reference
 * resolvable in this slice's list0 (colpoc present); else fall back to
 * spatial. Decided per slice, signalled in the header. */
    int direct_temporal = 0;
    int temporal_legal = 0;
    /* Y264_DIRECT_PERMB: availability is a MACROBLOCK property, not a slice one.
 * 8.4.1.2.3's conformance requirement bites only where the derivation runs, so a
 * block whose co-located reference does not resolve costs that macroblock its
 * direct mode, not the whole slice its temporal mode (temporal_direct returns 0
 * and direct_ok goes down, the staircase clamp's existing path). Without it one
 * unresolvable block in 8160 macroblocks demotes the frame. */
    int permb = direct_permb_for(e);
    /* Y264_DIRECT_AUTO: x264's per-slice rule. Fold the previous B frame's
 * skippability counts into the running score, decay once the total passes the
 * macroblock count, and take the higher. THREADS 1 ONLY: the total is
 * order-dependent and GOP workers do not encode in slice order, so at any
 * other thread count this refuses and leaves param.direct standing rather
 * than emit bits that depend on which chain finished first. */
    int auto_ok = direct_auto_armed(e);
    /* Engagement at any thread count. Arming auto excludes the staircase
 * (stair_direct_blocks), so threaded frames arrive here from the W2 frame
 * pipeline: anchors prepped on the driver in coding order, the two leaves of
 * a pair prepped back to back before either analyses and joined before the
 * next prep. The pending pair is therefore the counts of every frame whose
 * analysis is complete, in coding order, on every thread count, and the
 * per-frame adds are atomic. The stair (Y264_STAIR_TDIR=1) decides at its
 * launch instead and hands the mode in through fw->dauto_valid. */
    int auto_eng = auto_ok;
    static int auto_said = 0;
    if (direct_auto_wanted(e) && !auto_ok && !auto_said) {
        auto_said = 1;
        fprintf(stderr, "yah264: Y264_DIRECT_AUTO needs Y264_DIRECT_PERMB; not engaging\n");
    }
    /* The slice-level temporal legality guard (Y264_TDIR_LEGAL, C2): the
 * colocated picture's own list-0[0] must be this slice's list-0[0], else the
 * colocated motion spans a different interval than the derivation assumes.
 * It bounds a PINNED temporal only. Applying it to the auto rule was tried
 * on 2026-09-03 and lost on every clip it touched (station2 +9.7%, stockholm
 * +3.6%, in_to_tree +2.5%, sunflower +1.7% at t1): the skippability score
 * already refuses temporal where the interval mismatch hurts and keeps it
 * where it does not, which the POC test cannot tell apart. */
    int colp0 = fw->col_l0poc0_valid ? fw->col_l0poc0 : e->col_l0poc0;
    int tl_illegal = tdir_legal_on() && active_ref > 0 && colp0 >= 0 && colp0 != l0poc[0];
    if (type == 2 && auto_eng) {
        if (fw->dauto_valid) {
            direct_temporal = fw->dauto_temporal;     /* decided at burst launch */
        } else {
            long mbs = (long)e->width_in_mbs * e->height_in_mbs;
            e->direct_score[0] += e->dauto_pending[0];
            e->direct_score[1] += e->dauto_pending[1];
            e->dauto_pending[0] = e->dauto_pending[1] = 0;
            while (e->direct_score[0] + e->direct_score[1] > mbs / y264_mb_dauto_stride()) {
                e->direct_score[0] = e->direct_score[0] * 9 / 10;
                e->direct_score[1] = e->direct_score[1] * 9 / 10;
            }
            direct_temporal = e->direct_score[0] > e->direct_score[1];
        }
        temporal_legal = 1;
        if (getenv("Y264_DIRECT_WHY"))
            fprintf(stderr, "dauto: poc=%d score T=%ld S=%ld -> %s%s\n", e->poc,
                    e->direct_score[0], e->direct_score[1],
                    direct_temporal ? "temporal" : "spatial",
                    fw->dauto_valid ? " (burst)" : "");
    } else if (type == 2 && permb) {
        temporal_legal = 1;             /* per-MB from here; the scorer's alt too */
        /* Y264_TDIR_LEGAL (plan C2): the slice-level guard the auto rule
 * needs before its score can be trusted. Temporal is legal for the
 * slice only when the list-1 reference's own first list-0 picture is
 * this slice's first list-0 picture; otherwise the colocated motion
 * spans a different interval than the derivation assumes and the
 * slice falls back to spatial. Unknown (-1: an I colocated picture)
 * leaves the old behaviour. */
        if (tl_illegal)
            temporal_legal = 0;
        direct_temporal = (e->param.direct == YAH264_DIRECT_TEMPORAL) && temporal_legal;
    } else if (type == 2 && (e->param.direct == YAH264_DIRECT_TEMPORAL ||
                      getenv("Y264_DIRECT_SCORE"))) {
        temporal_legal = 1;
        size_t nmv = (size_t)e->mv_stride * e->height_in_mbs * 4;
        for (size_t i = 0; i < nmv && temporal_legal; i++) {
            int cp = fw->colpoc[i];     /* the slice's own col field (fw_default: e->) */
            if (cp < 0) continue;
            cp &= Y264_COLPOC_MASK;
            int found = 0;
            for (int k = 0; k < active_ref; k++)
                if (l0poc[k] == cp) { found = 1; break; }
            if (!found) temporal_legal = 0;
        }
        if (e->param.direct == YAH264_DIRECT_TEMPORAL) direct_temporal = temporal_legal;
    }
    /* Y264_DIRECT_WHY: measurement only. Print this slice's list0 and the
 * distinct co-located reference POCs, marking the ones that do not resolve.
 * Does not touch any decision. */
    if (type == 2 && getenv("Y264_DIRECT_WHY")) {
        int seen[64], cnt[64], ok[64], ns = 0;
        size_t nmv = (size_t)e->mv_stride * e->height_in_mbs * 4;
        long nintra = 0;
        for (size_t i = 0; i < nmv; i++) {
            int cp = fw->colpoc[i];
            if (cp < 0) { nintra++; continue; }
            cp &= Y264_COLPOC_MASK;
            int j = 0;
            for (; j < ns; j++) if (seen[j] == cp) break;
            if (j == ns && ns < 64) { seen[ns] = cp; cnt[ns] = 0; ok[ns] = 0; ns++; }
            if (j < 64) {
                cnt[j]++;
                for (int k = 0; k < active_ref; k++)
                    if (l0poc[k] == cp) { ok[j] = 1; break; }
            }
        }
        fprintf(stderr, "dwhy: poc=%d l1poc0=%d col_l0poc0=%d legal=%d intra=%ld list0=[",
                e->poc, e->ref1_poc, fw->col_l0poc0_valid ? fw->col_l0poc0 : e->col_l0poc0,
                temporal_legal, nintra);
        for (int k = 0; k < active_ref; k++)
            fprintf(stderr, "%d%s", l0poc[k], k + 1 < active_ref ? "," : "");
        fprintf(stderr, "] col=[");
        for (int j = 0; j < ns; j++)
            fprintf(stderr, "%d%s(%d)%s", seen[j], ok[j] ? "" : "*", cnt[j],
                    j + 1 < ns ? "," : "");
        /* Hash the co-located field itself. If two runs at the same thread
 * count disagree HERE, the field is being read while another worker
 * writes it; if they agree and the verdict still moves, the instability
 * is downstream. */
        unsigned long h = 1469598103934665603UL;
        for (size_t i = 0; i < nmv; i++) {
            unsigned v = (unsigned)(uint16_t)fw->colpoc[i]
                       ^ ((unsigned)(uint16_t)fw->colmvx[i] << 8)
                       ^ ((unsigned)(uint16_t)fw->colmvy[i] << 16);
            h = (h ^ v) * 1099511628211UL;
        }
        fprintf(stderr, "] colhash=%016lx tdir_mb=%ld/%ld\n", h,
                y264_tdir_mb[0], y264_tdir_mb[1]);
        y264_tdir_mb[0] = y264_tdir_mb[1] = 0;
    }
    if (type == 2)
        y264_bs_write1(&bs, !direct_temporal);      /* direct_spatial_mv_pred_flag */
    if (type == 1 || type == 2) {
        if (active_ref > 1) {
            y264_bs_write1(&bs, 1);                 /* num_ref_idx_active_override */
            y264_bs_write_ue(&bs, active_ref - 1);  /* num_ref_idx_l0_active_minus1 */
            if (type == 2)
                y264_bs_write_ue(&bs, 0);           /* num_ref_idx_l1_active_minus1 */
        } else {
            y264_bs_write1(&bs, 0);                 /* num_ref_idx_active_override */
        }
        /* In b-pyramid, reference B's outrank the previous anchor by FrameNum, so
 * the default P list0 would pick a B. Reorder to pin the anchor (its
 * FrameNum in cur_ref_l0_fn) at index 0. */
        int maxfn = 1 << (e->sps.log2_max_frame_num_minus4 + 4);
        int diff = e->cur_ref_l0_fn >= 0
                 ? (e->frame_num - e->cur_ref_l0_fn + maxfn) % maxfn : 0;
        if (type == 1 && e->b_pyramid && e->cur_ref_l0_fn >= 0 && diff != 0) {
            y264_bs_write1(&bs, 1);                 /* ref_pic_list_modification_flag_l0 */
            y264_bs_write_ue(&bs, 0);               /* idc 0: abs_diff subtract */
            y264_bs_write_ue(&bs, diff - 1);        /* abs_diff_pic_num_minus1 */
            y264_bs_write_ue(&bs, 3);               /* idc 3: end */
        } else {
            y264_bs_write1(&bs, 0);                 /* ref_pic_list_modification_l0 */
        }
        if (type == 2)
            y264_bs_write1(&bs, 0);                 /* ref_pic_list_modification_l1 */
    }
    /* v3 depth clamp eligibility for this slice: a P's list-0 searches against
 * the previous anchor take the fixed vertical clamp (its recon may still
 * be streaming), keyed by POC. Machine-invariant: env gate + parameters
 * only, so bits never depend on threads or engagement. */
    int clamp0_poc = (type == 1 && stair_depth_on() && stair_clamp_on(e))
                   ? e->prev_anchor_poc : -1;
    /* Hop 2: the anchor before that one. Width lets K bursts run at once, so up
 * to K-1 predecessors are still streaming when this anchor preps -- but only
 * a list 0 deeper than one entry can name the older of them, which is why
 * --ref is in the condition and the ring depth is not (the ring bounds the
 * hop COUNT; --ref decides whether hop 2 is reachable at all).
 *
 * Everything here is static: env gates, e->nref (set once at open from the
 * parameters), and coding-order POC state. Deliberately NOT a check of
 * whether that burst is live right now -- a clamp that engaged on live state
 * would make the bitstream a function of thread scheduling, and this
 * encoder's determinism is invariant to that on purpose. Clamping an anchor
 * that has already published costs a little search reach and nothing else,
 * which is the direction to err in. */
    /* The clamp is what makes width SAFE at --ref > 1, so it arms on exactly
 * the condition width engages on -- the env gate or a narrow frame. The two
 * must never disagree: a wide burst at --ref > 1 with the hop-2 clamp
 * disarmed would search a reference that is still streaming. */
    int clamp0_hop2 = (clamp0_poc >= 0 && stair_wide_on() &&
                       (stair_multihop_on() ||
                        wf_narrow_frame(e->param.width, e->param.height)) &&
                       e->nref >= 2 && !e->rcp_on)
                    ? e->prev_anchor_poc2 : -1;
    /* v6: and the live predecessors' REFERENCE B's, the pictures that actually
 * sit at the front of a deep list 0 while they stream. Same shape of test as
 * hop 2 -- width, plus a list deep enough to reach past the pinned previous
 * anchor (at --ref 1 a P list 0 is exactly {previous anchor}, so nothing
 * here is reachable) -- and gated on BDEPTH as well, because that is what
 * decides whether a reference B publishes per row at all. Without it the
 * clamp would cost search reach and buy no overlap, so the two move
 * together. NOT conditioned on Y264_STAIR_MULTIHOP: a different picture and
 * a different question, and the multi-hop round already answered its own. */
    int refb_clamp_on = type == 1 && clamp0_poc >= 0 && stair_refbgate_elig(e);
    /* The set this slice's list-0 searches (and the wp estimate below) key on.
 * PACKED: slots past the producers stay empty, which is what makes the
 * membership scans stop early and what keeps an unpopulated set exactly as
 * cheap as the single scalar this replaced. Packing (rather than a fixed
 * slot per producer) is load-bearing now that hop 2 can be empty while a
 * reference-B slot is not -- the two are gated independently, and a hole
 * would end the scan before the populated slot behind it. */
    int cand[Y264_STAIR_HOPS];
    int nc2 = 0;
    cand[nc2++] = clamp0_poc;
    cand[nc2++] = clamp0_hop2;
    cand[nc2++] = refb_clamp_on ? e->refb_hist[0] : -1;
    int clamp_set[Y264_STAIR_HOPS];
    int ns = 0;
    for (int h = 0; h < nc2; h++)
        if (cand[h] >= 0) clamp_set[ns++] = cand[h];
    while (ns < Y264_STAIR_HOPS) clamp_set[ns++] = -1;
    if (clamp0_hop2 >= 0 && stair_stat_on()) {
        e->stat_hop2_slices++;              /* P anchor preps are API-thread only */
        for (int i = 0; i < active_ref; i++)
            if (l0poc[i] == clamp0_hop2) e->stat_hop2_refs++;
    }
    /* pred_weight_table for explicit P-slice weighted prediction: one luma
 * weight/offset per active list-0 reference (chroma stays identity). */
    int wp_luma[16] = {0}, wp_w[16] = {0}, wp_o[16] = {0}, wp_denom = 5;
    if (type == 1 && e->pps.weighted_pred_flag) {
        y264_bs_write_ue(&bs, wp_denom);            /* luma_log2_weight_denom */
        y264_bs_write_ue(&bs, 0);                   /* chroma_log2_weight_denom */
        for (int i = 0; i < active_ref; i++) {
            /* Clamped ref: estimate against that anchor's SOURCE DC (cached at
 * its arrival) -- its recon is not fully readable yet. Generalizes
 * to the set for free, since the srcsum ring is POC-keyed and
 * Y264_STAIR_K deep: when this anchor preps, the ring still holds
 * every anchor the set can name. */
            int64_t sro = clamp_set_has(clamp_set, l0poc[i])
                        ? anchor_srcsum_get(e, l0poc[i]) : -1;
            wp_luma[i] = estimate_wp_luma(e, src[0], l0p[i][0], wp_denom, sro,
                                          &wp_w[i], &wp_o[i]);
            y264_bs_write1(&bs, wp_luma[i]);        /* luma_weight_l0_flag */
            if (wp_luma[i]) {
                y264_bs_write_se(&bs, wp_w[i]);     /* luma_weight_l0 */
                y264_bs_write_se(&bs, wp_o[i]);     /* luma_offset_l0 */
            }
            y264_bs_write1(&bs, 0);                 /* chroma_weight_l0_flag (identity) */
        }
    }
    /* dec_ref_pic_marking only for reference pictures. */
    if (is_ref) {
        if (is_idr) {
            y264_bs_write1(&bs, 0);                 /* no_output_of_prior_pics_flag */
            y264_bs_write1(&bs, 0);                 /* long_term_reference_flag */
        } else {
            y264_bs_write1(&bs, 0);                 /* adaptive_ref_pic_marking_mode */
        }
    }
    if (e->pps.entropy_coding_mode_flag && type != 0)
        y264_bs_write_ue(&bs, 0);                   /* cabac_init_idc */
    y264_bs_write_se(&bs, fqp - 26);                /* slice_qp_delta */
    /* In-loop deblocking on every slice type; the bS derivation handles B's
 * dual-list motion (deblock.c strength). Reference B's in the b-pyramid
 * store their filtered recon into the DPB (dpb_store runs after build_slice). */
    int deblock = e->param.deblock;
    y264_bs_write_ue(&bs, deblock ? 0 : 1);         /* disable_deblocking_filter_idc */
    if (deblock) {
        y264_bs_write_se(&bs, e->param.deblock_alpha);  /* slice_alpha_c0_offset_div2 */
        y264_bs_write_se(&bs, e->param.deblock_beta);   /* slice_beta_offset_div2 */
    }

    y264_frame_t f;
    for (int c = 0; c < 3; c++) {
        f.src[c] = src[c];
        f.src_stride[c] = e->pstride[c];
        f.rec[c] = fw->rec[c];
        f.rec_stride[c] = e->pstride[c];
        f.ref[c] = (type != 0) ? l0p[0][c] : e->ref[c];
        f.ref_stride[c] = e->pstride[c];
        f.ref1[c] = fw->ref1[c];
        f.ref1_stride[c] = e->pstride[c];
        f.nnz[c] = e->nnz[c];
        f.nnz_stride[c] = e->nnz_stride[c];
    }
    f.nref = active_ref;
    f.refs_poc[0] = e->ref0_poc;
    if (type != 0)
        for (int i = 0; i < active_ref; i++) {
            for (int c = 0; c < 3; c++)
                f.refs[i][c] = l0p[i][c];
            f.refs_poc[i] = l0poc[i];
        }
    else
        for (int c = 0; c < 3; c++)
            f.refs[0][c] = e->ref[c];
    f.i4mode = e->i4mode;
    f.i4mode_stride = e->i4mode_stride;
    f.cabac = NULL;
    f.mbcbp = e->mbcbp;
    f.mbcbp_stride = e->width_in_mbs;
    f.mvx = e->mvx;   f.mvy = e->mvy;   f.refidx = e->refidx;
    f.mvx1 = e->mvx1; f.mvy1 = e->mvy1; f.refidx1 = e->refidx1;
    f.mvdx = e->mvdx; f.mvdy = e->mvdy; f.mvdx1 = e->mvdx1; f.mvdy1 = e->mvdy1;
    f.colmvx = fw->colmvx; f.colmvy = fw->colmvy; f.colref = fw->colref;
    f.colpoc = fw->colpoc;
    f.colframepoc = e->colframepoc;
    f.direct_temporal = direct_temporal;
    f.direct_alt_ok = temporal_legal;   /* Y264_DIRECT_SCORE=2 scorer only */
    /* Y264_DIRECT_AUTO_REFONLY (measurement, default off): score only the
 * reference B of each mini-GOP; the leaves take the decision without probing
 * the alternate mode. Two thirds of the score's cost on a 3-B pyramid. */
    f.direct_auto = auto_eng && (!dauto_refonly_env() || is_ref);
    f.dauto_acc = fw->dauto_acc;
    f.mv_stride = e->mv_stride;
    f.slice_type = type;
    f.deblock_on = deblock;
    f.deblock_a = e->param.deblock_alpha;
    f.deblock_b = e->param.deblock_beta;
    f.chroma_qp_off = e->param.chroma_qp_index_offset;
    f.cqm = e->cqm_on ? &e->cqm : NULL;
    f.transform8x8 = e->pps.transform_8x8_mode_flag;
    f.weighted_bipred = (e->pps.weighted_bipred_idc == 2);
    /* An I slice has no inter neighbour for the flag to exclude, so it keeps
 * the plain frame-edge availability and pays no test. */
    f.constrained_intra = e->pps.constrained_intra_pred_flag && type != 0;
    f.poc = e->poc;
    /* Measurement only (skiporacle.h): the oracle's key must be unique across
 * the whole encode, and poc RESTARTS at every IDR. cur_disp is the absolute
 * display index and does not. */
    f.skor_key = e->cur_disp;
    f.poc_l0 = e->ref0_poc;
    f.poc_l1 = e->ref1_poc;
    for (int i = 0; i < 16; i++) {
        f.wp_luma[i] = wp_luma[i];
        f.wp_w[i] = wp_w[i];
        f.wp_o[i] = wp_o[i];
    }
    f.wp_denom = wp_denom;
    f.padded_w = e->padded_w;
    f.padded_h = e->padded_h;
    f.wmb = e->width_in_mbs;
    f.hmb = e->height_in_mbs;
    f.cf_idc = e->cf_idc;
    f.sub_w = e->sub_w;
    f.sub_h = e->sub_h;
    f.cbw = 4 / e->sub_w;
    f.cbh = 4 / e->sub_h;
    f.subme = e->param.subme > 0 ? e->param.subme : 10;
    f.slice_is_ref = is_ref;
    f.skipdec_p = e->skipdec_p;
    f.skipdec_b = e->skipdec_b;
    f.skipdec_t = e->skipdec_t;
    f.skip_mvagree_p = e->skip_mvagree_p;
    f.skip_mvagree_b = e->skip_mvagree_b;
    f.skip_costgate = e->skip_costgate;
    f.bskip_confirm = e->bskip_confirm;
    f.bskip_dec = e->bskip_dec;
    f.bskip_probe = e->bskip_probe;
    f.bskip_admit = e->bskip_admit;
    f.bskip_cguard = e->bskip_cguard;
    f.bskip_notrellis = e->bskip_notrellis;
    f.qp = fqp;
    f.chroma_qp = fcqp;
    f.cur_qp = fqp;
    f.cur_chroma_qp = fcqp;
    /* The cascade share of fqp, for the MB_LAMBDA=7 probe; 0 unless this is a
 * B whose frame_qp actually applied the cascade (2-pass sets QP directly). */
    f.lambda_casc = (type == 2 && e->tp_pass < 2) ? frame_b_casc(e, is_ref) : 0;
    f.prev_qp = fqp;                /* mb_qp_delta chain starts at SliceQPY */
    f.last_qp_delta = 0;
    f.te_mbx = -1; f.te_mby = -1;   /* A6 src-texture memo: empty at frame start */
    f.aq_off = e->aq_off;
    /* PROBE (Y264_MBT_BREF): x264 applies mb-tree offsets to any frame kept as a
 * reference, keyed on b_kept_as_ref rather than on B-ness
 * ; we apply to no B at all. This tests the APPLY half
 * with the last anchor's field as a stand-in -- the real build gives the
 * reference B its own field. If a proxy field helps, the apply
 * is worth building; if it hurts, the field itself has to be the B's own. */
    /* x264 applies mb-tree offsets to any frame kept as a reference, keyed on
 * b_kept_as_ref rather than on B-ness. A reference B
 * has its OWN field; e->cur_bseed carries its buffer slot
 * across the emit. Using the anchor's field here instead was measured at
 * +4.70% -- the field has to be the B's own. */
    if (e->mbtree_on && e->mbtree_apply) {
        f.mbtree_off = e->mbtree_off;
    } else if (e->mbtree_on && mbt_bref_probe() && type == 2 && is_ref
               && fw && fw->mbtoff_b) {
        f.mbtree_off = (int8_t *)fw->mbtoff_b;
    } else {
        f.mbtree_off = NULL;
    }
    f.mbt_frac = mbt_frac_on();
    /* P-frame lowres ME seed (ref0). Gated to the medium tier so the subme-10
 * default stays byte-identical; only for P (the anchor leg is P-vs-anchor). */
    /* type FIRST: only the anchor's serial prep may touch e->lr_seed_valid --
 * a v3 driver-side B prep runs concurrent with the API thread's pops,
 * which rewrite it (stash_lr_seed). */
    if (type == 1 && e->lr_seed_valid && (e->param.subme <= 0 || e->param.subme <= 8)) {
        f.lr_seed_mvx = (int16_t *)fw->lr_seed_mvx;
        f.lr_seed_mvy = (int16_t *)fw->lr_seed_mvy;
        f.lr_seed_cost = (int32_t *)fw->lr_seed_cost;
    } else {
        f.lr_seed_mvx = NULL;
        f.lr_seed_mvy = NULL;
        f.lr_seed_cost = NULL;
    }
    /* B-frame lowres pair seeds, POC-scaled from the stored anchor-pair legs to
 * this B's actual list-0/list-1 refs (b-pyramid legs sit between anchors).
 * Same medium-tier gate as the P seed; the search-side gate (b_seeds_on)
 * keeps the default UMH path byte-identical. */
    f.lr_bseed_mvx0 = f.lr_bseed_mvy0 = NULL;
    f.lr_bseed_mvx1 = f.lr_bseed_mvy1 = NULL;
    if (type == 2 && e->cur_bseed >= 0 && e->cur_bseed < 8 &&
        fw->bseed_valid[e->cur_bseed] && fw->bseed_cur[0] &&
        (e->param.subme <= 0 || e->param.subme <= 8)) {
        int b = e->cur_bseed;
        int nmb = e->width_in_mbs * e->height_in_mbs;
        int den0 = e->poc - fw->bseed_poc0[b], num0 = e->poc - e->ref0_poc;
        int den1 = fw->bseed_poc1[b] - e->poc, num1 = e->ref1_poc - e->poc;
        if (den0 > 0 && den1 > 0 && num0 > 0 && num1 > 0) {
            int sf0 = (num0 * 256 + den0 / 2) / den0;
            int sf1 = (num1 * 256 + den1 / 2) / den1;
            for (int i = 0; i < nmb; i++) {
                fw->bseed_cur[0][i] = (int16_t)((fw->bseed_src[b][0][i] * sf0 + 128) >> 8);
                fw->bseed_cur[1][i] = (int16_t)((fw->bseed_src[b][1][i] * sf0 + 128) >> 8);
                fw->bseed_cur[2][i] = (int16_t)((fw->bseed_src[b][2][i] * sf1 + 128) >> 8);
                fw->bseed_cur[3][i] = (int16_t)((fw->bseed_src[b][3][i] * sf1 + 128) >> 8);
            }
            f.lr_bseed_mvx0 = fw->bseed_cur[0]; f.lr_bseed_mvy0 = fw->bseed_cur[1];
            f.lr_bseed_mvx1 = fw->bseed_cur[2]; f.lr_bseed_mvy1 = fw->bseed_cur[3];
        }
    }
    /* Y264_BLATE_STAT: attach the pair legs' lowres costs, unscaled (the
 * serial bank only -- a t1 measurement; MT paths see NULL). */
    f.lr_bseed_c0 = f.lr_bseed_c1 = f.lr_bseed_ci = NULL;
    if (type == 2 && fw->bseedc_src && e->cur_bseed >= 0 && e->cur_bseed < 8 &&
        fw->bseedc_src[e->cur_bseed][0] && fw->bseed_valid[e->cur_bseed]) {
        f.lr_bseed_c0 = fw->bseedc_src[e->cur_bseed][0];
        f.lr_bseed_c1 = fw->bseedc_src[e->cur_bseed][1];
        f.lr_bseed_ci = fw->bseedc_src[e->cur_bseed][2];
    }
    /* Content-adaptive ME: low-motion frames run cheap searches. Medium tier
 * only (like the other subme<=8 fast paths); I slices don't search. */
    f.me_cheap = type != 0 && adme_thresh() > 0
              && (e->param.subme <= 0 || e->param.subme <= 8)
              && e->cur_lr_motion < adme_thresh();
    /* Y264_DIRECT_SCORE: flush the PREVIOUS frame's direct-prediction error here,
 * because prep for the next frame runs after the last one's macroblock loop.
 * t1 only, like every counter of this shape. */
    if (getenv("Y264_DIRECT_SCORE") && y264_dscore_n) {
        fprintf(stderr, "dscore: mbs=%ld ssd=%ld per_mb=%.0f skip_temporal=%ld skip_spatial=%ld\n",
                y264_dscore_n, y264_dscore_ssd,
                (double)y264_dscore_ssd / (double)y264_dscore_n,
                y264_dscore_skip[0], y264_dscore_skip[1]);
        y264_dscore_ssd = 0; y264_dscore_n = 0;
        y264_dscore_skip[0] = y264_dscore_skip[1] = 0;
    }
    if (adme_log() && type != 0)
        fprintf(stderr, "adme: poc=%d type=%d score=%d cheap=%d\n",
                e->poc, type, e->cur_lr_motion, f.me_cheap);
    f.mbqp = e->mbqp;
    f.mb_tr8 = e->mb_tr8;
    f.aq_strength = e->aq_strength;
    f.aq_abs = e->aq_abs;
    f.aq_chroma = e->aq_chroma;
    f.aq_anchor = (float)e->aq_anchor;
    f.psy_rd = e->param.psy_rd;
    /* Keyed on the USER'S total thread request, not this worker's pool width:
 * a multi-threaded encode with short GOPs hands its GOP workers 1-wide
 * pools, and stq engaging there would change MT output and cost MT speed
 * (caught by the conformance thread-canary). --threads 1 is the only
 * shape that gets the quality mode. */
    f.stq = stq_env() < 0 ? (e->param.threads == 1) : stq_env();
    f.psy_lattice = 0;
    f.psy_trellis = e->param.psy_trellis;
    if (psy_flat_gate(0) >= 0 || psy_calm_gate(0) >= 0) {
        int wmb = e->width_in_mbs, hmb = e->height_in_mbs, flat = 0, tex = 0;
        int64_t t = (int64_t)psy_flat_gate(2) << 16;    /* var*256^2 domain */
        int64_t tv = (int64_t)psy_calm_gate(2) << 16;
        for (int mby = 0; mby < hmb; mby++)
            for (int mbx = 0; mbx < wmb; mbx++) {
                const pixel *s = src[0] + (mby * 16) * e->pstride[0] + mbx * 16;
                uint32_t v2[2];
                y264_dsp.var16x16(s, e->pstride[0], v2);
                int64_t var256sq = (int64_t)v2[1] * 256 - (int64_t)v2[0] * v2[0];
                if (var256sq < t) flat++;
                if (var256sq >= tv) tex++;
            }
        int share = flat * 100 / (wmb * hmb);
        int texshare = tex * 100 / (wmb * hmb);
        if (psy_flat_gate(0) >= 0 && share >= psy_flat_gate(0)) {
            float st = psy_flat_gate(1) / 256.0f;
            if (st > f.psy_trellis) { f.psy_trellis = st; f.psy_lattice = 1; }
        }
        int calm = psy_calm_gate(0) >= 0 && e->cur_lr_tdiff < psy_calm_gate(0)
                && texshare >= psy_calm_gate(3) && fqp <= psy_calm_gate(4)
                && (texshare >= psy_calm_gate(5)
                    || e->cur_lr_tdiff < psy_calm_gate(6));
        if (calm) {
            float st = psy_calm_gate(1) / 256.0f;
            if (st > f.psy_trellis) { f.psy_trellis = st; f.psy_lattice = 1; }
        }
        if (psy_flat_log())
            fprintf(stderr, "psyflat: poc=%d type=%d share=%d tex=%d tdiff=%d calm=%d on=%d\n",
                    e->poc, type, share, texshare, e->cur_lr_tdiff, calm,
                    psy_flat_gate(0) >= 0 && share >= psy_flat_gate(0));
    }
    f.trellis = e->param.trellis;
    /* W1 ran the first frame serially to warm the analyze path's lazy statics on
 * one thread. y264_mb_warm_statics has resolved all of them at open since
 * the 07-27 TSan round, so the serial frame now buys nothing and costs a
 * whole frame of driver time with every worker asleep -- ~26 ms for a 720p
 * I frame, per encoder, and a GOP-parallel run opens one per worker.
 * Y264_WF_WARMSERIAL=1 restores it. wf_warmed still gates the staircase,
 * which wants one completed frame for its own reasons; the guarded store
 * keeps a v3 driver-side prep from writing what stair_ready reads. */
    f.pool = (e->wf_warmed || !wf_warmserial()) ? e->pool : NULL;
    if (!e->wf_warmed)
        e->wf_warmed = 1;
    /* Staircase hooks: off by default; the stair driver overrides after prep. */
    f.row_done = NULL; f.row_done_ctx = NULL;
    f.row_gate = NULL; f.row_gate_ctx = NULL;
    f.row_ready = NULL;
    /* The list-1 vertical clamp applies to every B that references the current
 * mini-GOP ANCHOR (poc_l1 == prev_anchor_poc), on EVERY code path (stair
 * driver, serial code_b_hier, fpipe pair) and at every thread count, purely
 * under the env gate -- so the bitstream changes once and stays invariant
 * to how (or whether) the concurrency engages. */
    /* v5 (Y264_STAIR_BDEPTH): the same clamp for a leaf whose list-1 is the
 * mini-GOP's still-in-flight REFERENCE B. List 1 is single-ref, so every
 * other list-1 MV producer (spatial direct/skip medians) closes over it,
 * exactly as for the anchor. */
    int refb_clamp = type == 2 && stair_bdepth_on() && stair_depth_on() &&
                     stair_clamp_on(e) && fw->refb_poc >= 0;
    f.stair_clamp = type == 2 && stair_clamp_on(e) &&
                    (f.poc_l1 == e->prev_anchor_poc ||
                     (refb_clamp && f.poc_l1 == fw->refb_poc));
    /* v3 depth: the P list-0 twin (see clamp_set above) -- macroblock.c tests
 * each list-0 search's refs_poc[r] for membership. v5 reuses SLOT 0 for a B
 * slice: the LATER leaf of a mini-GOP has the reference B as its nearest
 * past reference, i.e. in list 0. Only one picture of a covered shape can be
 * in flight, so one key is enough there, and the hops past it are empty by
 * construction (clamp0_hop2 keys off clamp0_poc, which is P-only). A leaf's
 * OTHER list-0 entries at --ref > 1 reach the previous anchor and older;
 * those are covered by the chain's wait for the previous anchor's full
 * publish, not by this clamp. */
    for (int h = 0; h < Y264_STAIR_HOPS; h++) f.stair_clamp0_poc[h] = clamp_set[h];
    /* INSERT the reference B's clamp, never overwrite slot 0: overwriting
     * clobbers the newest streaming ANCHOR's clamp, so a B leaf whose deep
     * list-0 names that anchor searches it unclamped -- timing-dependent bits
     * at B frames under wide (2/40 divergent runs, both first differing at a
     * mid-GOP B). Shifting drops only the set's OLDEST entry, which the burst
     * ring's own invariant retires (at most K-1 predecessors live), so nothing
     * clampable is lost. */
    if (refb_clamp) {
        for (int h = Y264_STAIR_HOPS - 1; h > 0; h--)
            f.stair_clamp0_poc[h] = f.stair_clamp0_poc[h - 1];
        f.stair_clamp0_poc[0] = fw->refb_poc;
    }
    /* MT stage 3 (thread-scaled clamp): the vertical qpel reach every
 * stair_clamp / stair_l0_clamp site applies. Resolved once at open
 * (stair_lag_for), not recomputed per slice -- see encoder.h. */
    f.stair_mvy_max = e->stair_mvy_max;
    f.mv_xlim_q = e->mv_xlim_q; f.mv_ylim_q = e->mv_ylim_q;

    /* Reset both motion fields: all blocks start "intra/unused" (refIdx -1).
 * Via fw so a per-leaf prep resets ITS slot's grids, never a live frame's. */
    size_t nmv = (size_t)e->mv_stride * e->height_in_mbs * 4;
    for (size_t i = 0; i < nmv; i++) { fw->refidx[i] = -1; fw->refidx1[i] = -1; }

    /* A2: build the half-pel planes for every reference ME will search this
 * slice, and register them (thread-local) for the ME sub-pel probes. Order
 * matches f.refs / f.ref1 so ME resolves each by reference pointer. Each
 * reference's build is an independent pure filter, so they run on the pool
 * (per-worker scratch); byte-identical (no cross-ref state). */
    if (e->hpel_on && type != 0) {
        int n = 0;
        int cap = e->nref + 1; if (cap > 17) cap = 17;
        const pixel *srcs[17];
        for (int i = 0; i < active_ref && n < cap; i++, n++) srcs[n] = l0p[i][0];
        if (type == 2 && n < cap) srcs[n++] = fw->ref1[0];
        /* Prefer each reference's DPB-cached hpel (built once at dpb_store). Fall
 * back to a fresh build into hpel_buf only if a reference isn't cached
 * (defensive -- all inter references are DPB pictures). */
        int B = Y264_LUMA_BORDER, sstride = e->padded_w + 2 * B;
        for (int k = 0; k < n; k++) {
            pixel *h0 = NULL, *h1 = NULL, *h2 = NULL;
            for (int i = 0; i < e->dpb_size; i++)
                if (e->dpb[i].used && e->dpb[i].hpel_valid && e->dpb[i].plane[0] == srcs[k]) {
                    h0 = e->dpb[i].hpel[0]; h1 = e->dpb[i].hpel[1]; h2 = e->dpb[i].hpel[2];
                    break;
                }
            /* Flat path: the reference's owned triple travelled here with the
 * buffer (built once when its recon was stored). */
            if (!h0 && e->flat_hp_on) {
                if (e->ref1_hpv && srcs[k] == e->ref1[0]) {
                    h0 = e->ref1_hp[0]; h1 = e->ref1_hp[1]; h2 = e->ref1_hp[2];
                } else {
                    for (int i = 0; i < e->nref; i++)
                        if (e->ring_hpv[i] && srcs[k] == e->refring[i][0]) {
                            h0 = e->ring_hp[i][0]; h1 = e->ring_hp[i][1]; h2 = e->ring_hp[i][2];
                            break;
                        }
                }
            }
            if (!h0 && !hpel_buf_take(e, k)) {
                /* OOM on the lazy fallback triple: register the references
 * resolved so far and let ME interpolate the rest on the fly,
 * which is the Y264_HPEL=0 path and bit-identical to it. */
                n = k;
                break;
            }
            if (!h0) {
                double t0 = hpel_probe_on() ? tprof_ms() : 0;
                y264_hpel_census_built(e->hpel_buf[k][0], -B, e->padded_h + B,
                                       e->pstride[0]);
                for (int pass = hpel_double_on(); pass > 0; pass--)
                y264_mc_build_hpel(e->hpel_buf[k][0], e->hpel_buf[k][1], e->hpel_buf[k][2],
                                   e->pstride[0], srcs[k], e->pstride[0],
                                   e->padded_w, e->padded_h, B, e->hpel_scratch, sstride);
                if (hpel_probe_on()) hpb_add(tprof_ms() - t0, e->padded_h + 2 * B);
                h0 = e->hpel_buf[k][0]; h1 = e->hpel_buf[k][1]; h2 = e->hpel_buf[k][2];
            }
            fw->hpel_ctx[k] = (y264_hpel_ref_t){ srcs[k], h0, h1, h2 };
        }
        y264_me_set_hpel(fw->hpel_ctx, n, e->pstride[0]);
        f.hpel_ctx = fw->hpel_ctx; f.hpel_n = n; f.hpel_stride = e->pstride[0];
    } else {
        y264_me_set_hpel(NULL, 0, 0);
        f.hpel_ctx = NULL; f.hpel_n = 0; f.hpel_stride = 0;
    }

    *bs_out = bs;
    *f_out = f;
    *fqp_out = fqp;
    *deblock_out = deblock;
}

/* S1 identity-memo ceiling probe.
 * The memo's true condition for replaying frame t-1's search at MB m in frame
 * t is: src_t[m] == src_{t-1}[m] AND the reference window the t-1 search read
 * is bit-identical to the one t would read, i.e. recon(t-1) == recon(t-2)
 * over m's search window (per additional ref, one more recon pair). Per-frame
 * bitmaps by 16x16 memcmp; window = +-2 MB radius (covers hex range 16 plus
 * the subpel margin). hit1 = first-ref-only condition, hit3 = all three refs.
 * Y264_IDENT_STAT=1; t1 + IPPP ONLY (statics assume one encoder, and display
 * order must equal coding order for "previous frame" to mean one thing).
 * Prints one IDENTSTAT line per frame; aggregate offline. Default inert. */
static int ident_stat_env(void)
{
    static int on = -1;
    if (on < 0) { const char *ev = getenv("Y264_IDENT_STAT"); on = ev ? atoi(ev) : 0; }
    return on;
}

static void ident_stat(const y264_frame_t *f, int type)
{
    if (!ident_stat_env()) return;
    static pixel *psrc, *prec;          /* previous frame's src / final recon (luma) */
    static uint8_t *si, *ri[3];         /* this frame's src-ident; recon-ident ring */
    static int nalloc, nfr;
    int wmb = f->wmb, hmb = f->hmb, nmb = wmb * hmb;
    int ss = f->src_stride[0], rs = f->rec_stride[0];
    if (nalloc < nmb) {
        free(psrc); free(prec); free(si);
        for (int i = 0; i < 3; i++) free(ri[i]);
        psrc = malloc((size_t)hmb * 16 * ss * sizeof(pixel));
        prec = malloc((size_t)hmb * 16 * rs * sizeof(pixel));
        si = malloc(nmb);
        for (int i = 0; i < 3; i++) ri[i] = calloc(1, nmb);
        nalloc = nmb; nfr = 0;
    }
    uint8_t *rcur = ri[2];              /* rotate: oldest slot becomes current */
    if (nfr > 0) {
        for (int my = 0; my < hmb; my++)
            for (int mx = 0; mx < wmb; mx++) {
                int m = my * wmb + mx, s = 1, r = 1;
                for (int y = 0; y < 16; y++) {
                    const pixel *a = f->src[0] + (my*16+y)*ss + mx*16;
                    const pixel *b = psrc      + (my*16+y)*ss + mx*16;
                    if (memcmp(a, b, 16 * sizeof(pixel))) { s = 0; break; }
                }
                for (int y = 0; y < 16; y++) {
                    const pixel *a = f->rec[0] + (my*16+y)*rs + mx*16;
                    const pixel *b = prec      + (my*16+y)*rs + mx*16;
                    if (memcmp(a, b, 16 * sizeof(pixel))) { r = 0; break; }
                }
                si[m] = (uint8_t)s; rcur[m] = (uint8_t)r;
            }
        if (type == 1) {                /* P: count against the PRIOR frames' recon pairs */
            int nsrc = 0, h1 = 0, h3 = 0;
            for (int my = 0; my < hmb; my++)
                for (int mx = 0; mx < wmb; mx++) {
                    int m = my * wmb + mx;
                    if (!si[m]) continue;
                    nsrc++;
                    int w1 = 1, w3 = nfr >= 4;
                    for (int dy = -2; dy <= 2 && w1; dy++)
                        for (int dx = -2; dx <= 2 && w1; dx++) {
                            int nx = mx + dx, ny = my + dy;
                            if (nx < 0 || ny < 0 || nx >= wmb || ny >= hmb) continue;
                            int n = ny * wmb + nx;
                            if (!ri[1][n]) { w1 = 0; w3 = 0; }
                            else if (w3 && (!ri[0][n])) w3 = 0;
                        }
                    /* hit3 also needs the third pair; the ring only holds two
 * comparisons besides the current, so hit3 here is the
 * two-ref condition -- an upper bound on three. */
                    h1 += w1; h3 += w3;
                }
            fprintf(stderr, "IDENTSTAT: poc=%d nmb=%d src=%d hit1=%d hit2=%d\n",
                    f->poc, nmb, nsrc, h1, h3);
        }
    }
    for (int y = 0; y < hmb * 16; y++)
        memcpy(psrc + (size_t)y * ss, f->src[0] + (size_t)y * ss, (size_t)wmb * 16 * sizeof(pixel));
    for (int y = 0; y < hmb * 16; y++)
        memcpy(prec + (size_t)y * rs, f->rec[0] + (size_t)y * rs, (size_t)wmb * 16 * sizeof(pixel));
    uint8_t *r2 = ri[0]; ri[0] = ri[1]; ri[1] = rcur; ri[2] = r2;
    nfr++;
}

/* Serial slice build (W2 off): header + analyze + emit + deblock, byte-identical
 * to pre-W2 HEAD. Returns the RBSP size (0 on CAVLC overflow). */
/* Y264_QP_TRACE=1: the frame's mean CODED QP (per-MB map, so AQ and mb-tree
 * offsets included) per frame, for allocation studies. Serial path only. */
static void qp_trace_frame(const yah264_encoder_t *e, const y264_frame_t *f, int type)
{
    static int on = -1;
    if (on < 0) { const char *s = getenv("Y264_QP_TRACE"); on = s ? atoi(s) : 0; }
    if (!on || !f->mbqp) return;
    long n = (long)e->width_in_mbs * e->height_in_mbs, sum = 0, isum = 0, nskip = 0;
    for (long i = 0; i < n; i++) {
        int q = f->qp + (f->mbtree_off ? (f->mbt_frac ? (int)lround(f->mbtree_off[i] / 2.0) : f->mbtree_off[i])
                                       : (f->aq_off ? (int)f->aq_off[i] : 0));
        if (q < 0) q = 0; else if (q > 51) q = 51;
        sum += f->mbqp[i]; isum += q; nskip += f->mbqp[i] != q;
    }
    fprintf(stderr, "QPT type=%d poc=%d slice_qp=%d mean_qp=%.2f intended=%.2f nodelta=%.2f moff=%.2f crf_dc=%.2f map=%s\n",
            type, e->poc, f->qp, (double)sum / n, (double)isum / n, (double)nskip / n, e->mbtree_mean_off, e->crf_dc,
            f->mbtree_off ? (f->mbtree_off == e->mbtree_off ? "anchor" : "own") : (f->aq_off ? "aq" : "none"));
}

static size_t build_slice(yah264_encoder_t *e, int type, int is_idr, int is_ref,
                          pixel *const src[3])
{
    y264_bs_t bs; y264_frame_t f; int fqp, deblock;
    struct frame_work fw; fw_default(e, &fw);
    TPROF(TP_PREP, build_slice_prep(e, type, is_idr, is_ref, src, e->rbsp, e->rbsp_cap,
                     &fw, &bs, &f, &fqp, &deblock));
    if (e->pps.entropy_coding_mode_flag) {
        /* cabac_alignment_one_bit: pad the header to a byte boundary with 1s,
 * then the arithmetic engine writes the slice data from there. */
        while (y264_bs_pos_bits(&bs) & 7)
            y264_bs_write1(&bs, 1);
        y264_cabac_t cb;
        y264_cabac_init_engine(&cb, bs.p); y264_cabac_set_end(&cb, bs.end - 64);
        y264_cabac_init_contexts(&cb, type, 0, fqp);   /* contexts init from SliceQPY */
        f.cabac = &cb;
        y264_emit_job_t *job;
        TPROF(TP_ANALYZE, job = y264_frame_analyze(&f));
        TPROF(TP_EMIT, y264_frame_emit(&bs, &f, job));
        qp_trace_frame(e, &f, type);
        if (deblock)
            TPROF(TP_DEBLOCK, y264_deblock_frame(&f));
        ident_stat(&f, type);
        if (!cb.overflow) cabac_zero_words(&f, &cb, bs.end);
        return cb.overflow ? 0 : (size_t)(cb.p - bs.start);
    }

    y264_emit_job_t *job;
    TPROF(TP_ANALYZE, job = y264_frame_analyze(&f));
    TPROF(TP_EMIT, y264_frame_emit(&bs, &f, job));
    qp_trace_frame(e, &f, type);
    if (deblock)
        TPROF(TP_DEBLOCK, y264_deblock_frame(&f));
    ident_stat(&f, type);

    TPROF(TP_EMIT, y264_bs_rbsp_trailing(&bs));
    if (bs.overflow)
        return 0;
    return (size_t)(bs.p - bs.start);
}

/* Y264_UNSAFE_NO_NAL=1: keep the NAL bookkeeping, delete the byte-at-a-time
 * emulation-prevention scan and the copy. The payload then points at whatever
 * the output buffer already held, so the stream is garbage -- a probe flag,
 * never a shipping one. Prices the assembly half of the drain separately from
 * the entropy half (Y264_UNSAFE_NO_EMIT). */
static int unsafe_no_nal(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_UNSAFE_NO_NAL"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Append one NAL with no access-unit bookkeeping. append_nal wraps this and
 * opens the access unit first; the openers themselves come through here. */
static int append_nal_raw(yah264_encoder_t *e, size_t *off, int ref_idc, int type,
                          const uint8_t *rbsp, size_t rbsp_size);

/* The NALs that open an access unit, in the order 7.4.1.2.3 requires them:
 * the access unit delimiter first, then (elsewhere) the parameter sets, then
 * the SEI. Called immediately before the first slice of a picture -- one slice
 * per picture today, so "the first slice" is "the slice"; --slices will have to
 * revisit this, which is why it is one function and not five copies.
 *
 * primary_pic_type is derived from the NAL type rather than threaded from the
 * five emit sites: an IDR picture is I and nothing else, so it asserts 0, and
 * everything else asserts Table 7-5's 2, "I, P or B may be present", which is
 * true of every non-IDR picture this encoder codes. A tighter value would have
 * to be carried through four concurrent emit paths to say something no decoder
 * acts on. */
static int au_open(yah264_encoder_t *e, size_t *off, int nal_type)
{
    uint8_t buf[64];
    y264_bs_t bs;
    /* The delimiter opens the access unit, so for the FIRST one it was already
 * written ahead of the parameter sets; the pic_timing SEI is not, because
 * 7.4.1.2.3 puts SEI after the parameter sets and an SEI written before the
 * active SPS is a message with nothing to be active for. So the two halves
 * are suppressed separately, and only the delimiter is ever pre-written. */
    if (e->param.aud && !e->au_opened) {
        y264_bs_init(&bs, buf, sizeof buf);
        y264_aud_write(&bs, nal_type == YAH264_NAL_SLICE_IDR ? 0 : 2);
        if (append_nal_raw(e, off, YAH264_NAL_PRIORITY_DISPOSABLE, 9,
                           buf, (size_t)(bs.p - bs.start)) < 0)
            return -1;
    }
    e->au_opened = 0;
    if (e->param.pic_struct) {
        uint8_t msg[16];
        size_t n = y264_sei_pic_timing(msg, sizeof msg, 0);   /* progressive frame */
        y264_bs_init(&bs, buf, sizeof buf);
        y264_sei_write(&bs, 1, msg, n);
        if (append_nal_raw(e, off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                           buf, (size_t)(bs.p - bs.start)) < 0)
            return -1;
    }
    return 0;
}

static int append_nal(yah264_encoder_t *e, size_t *off, int ref_idc, int type,
                      const uint8_t *rbsp, size_t rbsp_size)
{
    if (rbsp_size == 0) { slice_overflow_warn(); }
    if (rbsp_size == 0)
        return -1;
    if ((e->param.aud || e->param.pic_struct) &&
        (type == YAH264_NAL_SLICE || type == YAH264_NAL_SLICE_IDR) &&
        au_open(e, off, type) < 0)
        return -1;
    return append_nal_raw(e, off, ref_idc, type, rbsp, rbsp_size);
}

static int append_nal_raw(yah264_encoder_t *e, size_t *off, int ref_idc, int type,
                          const uint8_t *rbsp, size_t rbsp_size)
{
    if (rbsp_size == 0)
        return -1;
    size_t n = unsafe_no_nal()
             ? (e->out_cap - *off < 5 + rbsp_size + rbsp_size / 2 + 1 ? 0 : rbsp_size + 5)
             : y264_nal_write(e->out + *off, e->out_cap - *off,
                             ref_idc, type, rbsp, rbsp_size);
    if (n == 0)
        return -1;
    if (e->nal_count >= (int)(sizeof e->nal / sizeof e->nal[0]))
        return -1;                      /* never write past the array: it is
 * immediately followed by nal_count */
    yah264_nal_t *nl = &e->nal[e->nal_count++];
    nl->type = type;
    nl->ref_idc = ref_idc;
    nl->size = n;
    nl->payload = e->out + *off;
    *off += n;
    return 0;
}

/* Minimum H.264 level (Annex A, Table A-1) that satisfies the resolution/framerate/
 * DPB constraints: MaxFS (frame size in MBs), MaxMBPS (MB rate), MaxDpbMbs (decoded
 * picture buffer). Returns level_idc (level*10, e.g. 30 = 3.0). This replaces the
 * old hardcoded 5.1 -- a stream that claims a higher level than it needs can be
 * rejected by hardware decoders that support only up to a given level. MaxBR/MaxCPB
 * (bitrate) are not enforced here (they only tighten the level under VBV, and yah264
 * doesn't yet cap-check them); level 6.2 is the clamp if nothing else fits. */
/* Marked reference-B count of one full mini-GOP (defined beside
 * stair_plan_hier, which is the authority on which B's the pyramid marks). */
static int stair_plan_nrefb(int bframes);

/* A slice that outgrew its RBSP buffer (3 KB per macroblock + a frame) is
 * voided rather than written past the buffer; say so once, because the
 * stream then misses a frame. */
static void slice_overflow_warn(void)
{
    static _Atomic int said;
    if (!atomic_exchange(&said, 1))
        fprintf(stderr, "yah264: a slice outgrew its buffer and was dropped (Y264_RBSP_CAP probe, or a pathological input)\n");
}

/* Y264_CZW_STAT=1: per-slice bins/bound print. Warmed on the main thread
 * (cabac_zero_words runs on chain drivers and the W2 thread). */
static int czw_stat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_CZW_STAT"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
/* cabac_zero_words (7.4.2.10 / 9.3.4.6): a picture's BinCountsInNALunits may
 * not exceed (32/3) x NumBytesInVclNALunits + RawMbBits x PicSizeInMbs / 32.
 * With one slice per picture the slice's bin count and byte count are the
 * picture's; when bins outrun the bound, 0x0000 words appended after the
 * trailing bits (each one adds three NAL bytes once emulation prevention
 * lands, i.e. 32 to the bound) restore it. Returns the bytes appended.
 * Y264_CZW_STAT=1 prints the ratio per slice. */
static size_t cabac_zero_words(const y264_frame_t *f, y264_cabac_t *cb, const uint8_t *end)
{
    size_t bytes = (size_t)(cb->p - cb->start);
    int nmb = f->wmb * f->hmb;
    int mbwc = 16 / f->sub_w, mbhc = 16 / f->sub_h;
    double raw_mb_bits = 256.0 * Y264_BIT_DEPTH + 2.0 * mbwc * mbhc * Y264_BIT_DEPTH;
    double bound = 32.0 * (double)bytes / 3.0 + raw_mb_bits * nmb / 32.0;
    size_t k = 0;
    if ((double)cb->nbins > bound)
        k = (size_t)(((double)cb->nbins - bound + 31.0) / 32.0);
    if (czw_stat_on())
        fprintf(stderr, "CZW bins=%u bytes=%zu bound=%.0f ratio=%.3f words=%zu\n",
                cb->nbins, bytes, bound, bound > 0 ? cb->nbins / bound : 0.0, k);
    size_t appended = 0;
    while (k-- && cb->p + 2 <= end) { *cb->p++ = 0; *cb->p++ = 0; appended += 2; }
    return appended;
}

/* FrameNumWrap (8.2.5.3): the sliding window evicts the picture with the
 * smallest WRAPPED frame_num, i.e. a frame_num above the current one is from
 * before the wrap and counts as MaxFrameNum lower. Evicting by raw frame_num
 * threw out the first reference after every wrap (a b-pyramid stream past
 * ~512 frames at keyint > 512): encoder and decoder DPBs diverged, then
 * set_b_refs indexed slot -1. Review 2026-09-04. */
static int fn_wrap(const yah264_encoder_t *e, int fn)
{
    int maxfn = 1 << (e->sps.log2_max_frame_num_minus4 + 4);
    return fn > e->next_frame_num ? fn - maxfn : fn;
}

static int compute_level_idc(int fs, long mbps, long dpb_mbs, long br_kbps, int high)
{
    /* MaxBR (kbit/s, Table A-1) applies to the target or the VBV cap when one
 * is set (0 = no rate constraint, as under CRF/CQP); the High profiles'
 * factor is 1.25x (cpbBrNalFactor 1500 vs 1200). */
    static const struct { int idc; long max_mbps, max_fs, max_dpb, max_br; } L[] = {
        {10,     1485,     99,    396,     64}, {11,     3000,    396,    900,    192},
        {12,     6000,    396,   2376,    384}, {13,    11880,    396,   2376,    768},
        {20,    11880,    396,   2376,   2000}, {21,    19800,    792,   4752,   4000},
        {22,    20250,   1620,   8100,   4000}, {30,    40500,   1620,   8100,  10000},
        {31,   108000,   3600,  18000,  14000}, {32,   216000,   5120,  20480,  20000},
        {40,   245760,   8192,  32768,  20000}, {41,   245760,   8192,  32768,  50000},
        {42,   522240,   8704,  34816,  50000}, {50,   589824,  22080, 110400, 135000},
        {51,   983040,  36864, 184320, 240000}, {52,  2073600,  36864, 184320, 240000},
        {60,  4177920, 139264, 696320, 240000}, {61,  8355840, 139264, 696320, 480000},
        {62, 16711680, 139264, 696320, 800000},
    };
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) {
        long br_cap = high ? L[i].max_br * 5 / 4 : L[i].max_br;
        if (fs <= L[i].max_fs && mbps <= L[i].max_mbps && dpb_mbs <= L[i].max_dpb &&
            br_kbps <= br_cap)
            return L[i].idc;
    }
    return 62;
}

/* The DPB a given level allows at a given frame size, in frames:
 * Min(MaxDpbMbs / PicSizeInMbs, 16) (A.3.1 h). --stitchable declares this
 * instead of what the encode happens to need, so the SPS stops depending on
 * --ref and --bframes. */
/* The DPB --stitchable declares and picks its level for, in frames. 15 rather
 * than 16 because max_num_ref_frames is written alongside it and this encoder
 * caps that at 15; equal values across the three fields is what makes the SPS
 * comparable at a glance. */
#define Y264_STITCH_DPB 15

static int level_max_dpb_frames(int level_idc, int fs)
{
    static const struct { int idc; long max_dpb; } L[] = {
        {10,    396}, {11,    900}, {12,   2376}, {13,   2376}, {20,   2376},
        {21,   4752}, {22,   8100}, {30,   8100}, {31,  18000}, {32,  20480},
        {40,  32768}, {41,  32768}, {42,  34816}, {50, 110400}, {51, 184320},
        {52, 184320}, {60, 696320}, {61, 696320}, {62, 696320},
    };
    long cap = 696320;
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++)
        if (L[i].idc == level_idc) { cap = L[i].max_dpb; break; }
    int n = fs > 0 ? (int)(cap / fs) : 16;
    return n > 16 ? 16 : n < 1 ? 1 : n;
}

/* The widest row-wavefront
 * pool a frame of this size can actually pay for.
 *
 * Cell (r,c) waits on (r-1,c+1), so row r cannot start until row r-1 is two
 * cells in: an R x C grid has a critical path of 2*(R-1)+C cell-times NO MATTER
 * how many workers claim rows. Work over critical path is therefore a hard
 * speedup ceiling, and equally a hard bound on the workers a lone grid can keep
 * busy -- 7 on CIF's 18x22, 21 on 720p's 45x80, 32 on 1080p's 68x120. Admitting
 * more than that buys nothing but wake and futex traffic, which is exactly what
 * the CIF t18 CPU inflation (2.74x the t1 CPU for the same 300 frames) is.
 *
 * The floor is not arithmetic, it is measured: a pool narrower than
 * Y264_MT_POOL_MIN switches off the staircase, the concurrent leaves and the
 * lookahead thread, and losing those costs far more than the workers saved
 * (foreman_cif 300f t18: 559 ms at 7 workers, 433 ms at 8). Since this is an
 * upper bound, the floor can only widen a cap, never inflate a small request.
 *
 * Machine-independent by construction -- no cell cost, no wake constant, no
 * core count. `python3 scripts/wfsim.py sweep` prints the same knees. */
/* The weight one pass-1 stats record contributes to the pass-2 allocation: the
 * QP-invariant coding cost, complexity-compressed. Exported because a caller
 * that splits a stats file along GOP boundaries has to size each GOP's budget
 * with the encoder's own formula -- a copy of it in the CLI would drift the
 * first time qcomp or the cost model moved, and the symptom (a slow rate drift
 * between the passes) would look like a rate-control bug rather than a stale
 * constant. Must stay in step with the tp_sum_cq accumulation in
 * yah264_encoder_open and with rc_set_qp_2pass. */
double yah264_2pass_stat_weight(double bits, int qp)
{
    return pow((bits + 1) * pow(2.0, (qp - 12) / 6.0), Y264_TP_QCOMP);
}

/* --- Offline pass-2 plan (Y264_TP_PLAN) -------------------------------------
 * An independent implementation of the reference's second-pass planner over
 * whatever slice of records this
 * encoder instance was handed -- the whole stream serially, one GOP's section
 * under the threaded splitter. Both cases are the same problem: turn N
 * measured per-frame complexities into N qscales whose modelled bits sum to
 * one budget.
 *
 * The greedy loop this replaces gave frame i a share of the budget still
 * unspent, proportional to cost^qcomp, and inverted it to a QP. That is a
 * correct-looking allocator with two holes, both measured on samsung_720p:
 *
 * - NO FRAME-TYPE RELATION. cost^qcomp is monotone in cost, and a B frame's
 * cost is small, so B frames won the auction and the I frame lost it: the
 * planned QPs came out I 32 / P 27 / B 21, exactly upside down. x264 does
 * not let the curve decide this at all -- its cross-type limiter OVERWRITES
 * the I and B qscales as fixed ratios off the P qscale, and the complexity
 * term only ever moves the P frames.
 * - NO BOUND. Spending against a remaining budget is a closed loop with unit
 * gain and a biased plant: every frame that overshoots its model takes the
 * overshoot out of the frames after it. B frames overshot ~2x (dropping a
 * B from QP 28 to 21 buys far more bits than the 1/qscale model predicts,
 * because it un-skips blocks), so the tail hit the QP 51 rail. x264 solves
 * the whole curve OFFLINE against a single rate factor and lets runtime
 * feedback move it only within a clip3(0.5, 2) band.
 *
 * Kept from the current code: the complexity domain. `cost` here is
 * (bits+1) * qscale_of_pass1, the QP-invariant coding cost, which is exactly
 * the reference's cost-to-bits conversion with tex/mv/misc lumped -- pass 1 records one
 * bits total, so the 1.1/0.5/const split is not available to us (tp_bexp is
 * the one-term approximation of it). Because bits ~ cost/qscale and
 * qscale ~ cost^(1-qcomp), the implied bits still go as cost^qcomp, so the
 * per-GOP budget split in the CLI keeps using yah264_2pass_stat_weight and
 * stays in step with this. */

static double tp_qp2qscale(double qp)  { return pow(2.0, (qp - 12.0) / 6.0); }
static double tp_qscale2qp(double q)   { return 12.0 + 6.0 * log2(q); }

/* Modelled bits for record i at qscale q. tp_bexp 1.0 reproduces the model the
 * greedy loop used (bits strictly inverse to qscale); x264 uses 1.1 on the
 * texture term because skip blocks make texture bits fall off faster than
 * 1/qscale. With one lumped total the exponent is the only handle we have. */
static double tp_bits_at(const yah264_encoder_t *e, int i, double q)
{
    if (q < 0.1) q = 0.1;
    if (e->tp_bexp == 1.0)
        return e->tp_cost[i] / q;
    return e->tp_cost[i] * pow(e->tp_qrec[i] / q, e->tp_bexp) / e->tp_qrec[i];
}

/* The reference's cross-type qscale limiter. Forces the I and B qscales
 * off the P qscale and limits step between consecutive frames of one type.
 *
 * Run in REVERSE record order, as x264 does, and that is load-bearing rather
 * than a quirk: an I frame's quantiser should relate to the P frames it is
 * about to feed, not to the ones behind it, and reverse order is what puts the
 * following GOP's P average in ptrack_qp when the I frame is reached. Forward
 * order leaves the FIRST I frame of a slice with ptrack_norm == 0, which is
 * the fallback branch -- the I frame keeps its own complexity-derived qscale,
 * which is the whole defect. Under the threaded splitter every GOP section
 * starts with an I frame, so forward order would miss every one of them.
 *
 * x264's accum mask is 1 - (intra MBs / MBs)^2, which down-weights a P frame
 * that was mostly intra when averaging "what did P cost here". Our stats carry
 * no intra count, so the mask is 1 and ptrack_qp is a plain running mean of
 * the P QPs since the I frame. The mask only ever matters at a scene cut,
 * where the P frame after the cut is nearly intra -- and yah264 puts an I
 * frame there anyway. */
struct tp_dl {
    double lastq[3];            /* last qscale per type, 0 I / 1 P / 2 B */
    double ptrack_qp, ptrack_norm, last_accum_p_norm;
    int    last_non_b;          /* -1 until a non-B has been seen */
};

static double tp_diff_limited(const yah264_encoder_t *e, struct tp_dl *d,
                              const struct tp_stat *s, double q, double lstep)
{
    int type = s->type;
    if (type == 0) {
        double iq = q;
        if (d->ptrack_norm > 0) {
            double pq = tp_qp2qscale(d->ptrack_qp / d->ptrack_norm);
            q = d->ptrack_norm >= 1
              ? pq / e->tp_ipf
              : d->ptrack_norm * pq / e->tp_ipf + (1 - d->ptrack_norm) * iq;
        }
    } else if (type == 2) {
        if (d->last_non_b >= 0)
            q = d->lastq[d->last_non_b];
        /* x264 charges its pbratio only to a B that is NOT kept as a reference:
 * a pyramid ref-B carries the next frames and is held at its anchor's
 * qscale. is_ref comes off the stats record (5th field); a 4-field
 * record from an older pass 1 reads 0, i.e. the flat-B assumption. */
        if (!s->is_ref)
            q *= e->tp_pbf;
    }
    /* Step limit between consecutive frames of the same type. */
    if (d->last_non_b == type && (type != 0 || d->last_accum_p_norm < 1)) {
        double lo = d->lastq[type] / lstep, hi = d->lastq[type] * lstep;
        if (q > hi) q = hi;
        else if (q < lo) q = lo;
    }
    d->lastq[type] = q;
    if (type != 2) d->last_non_b = type;
    if (type == 0) {
        d->last_accum_p_norm = d->ptrack_norm;
        d->ptrack_norm = 0;
        d->ptrack_qp = 0;
    }
    if (type == 1) {
        d->ptrack_qp += tp_qscale2qp(q);
        d->ptrack_norm += 1;
    }
    return q;
}

/* One pass of the curve at a given rate factor: fill q[] and return the total
 * modelled bits. cplx[] is the (optionally blurred) complexity per record. */
static double tp_solve(yah264_encoder_t *e, double rf, const double *cplx,
                       double *q, double *scratch, double base_cplx)
{
    int n = e->tp_n;
    double icomp = 1.0 - e->crf_qcomp;
    double qmin = tp_qp2qscale(e->qp_min < 1 ? 1 : e->qp_min), qmax = tp_qp2qscale(e->qp_max);
    double lstep = pow(2.0, e->qp_step / 6.0);   /* --qpstep, default 4 */

    for (int i = 0; i < n; i++)
        q[i] = pow(cplx[i], icomp) / rf;

    if (e->tp_difflim) {
        struct tp_dl d;
        d.lastq[0] = d.lastq[1] = d.lastq[2] = pow(base_cplx, icomp) / rf;
        d.ptrack_qp = 0;
        d.ptrack_norm = 0;
        d.last_accum_p_norm = 1;
        d.last_non_b = -1;
        for (int i = n - 1; i >= 0; i--)
            q[i] = tp_diff_limited(e, &d, &e->tp_stats[i], q[i], lstep);
    }

    if (e->tp_qblur > 0 && scratch) {
        /* The reference's QP blur (its --qblur): a gaussian over the qscales of the SAME frame type, so
 * one cheap frame cannot drag its neighbours' quantisers with it. */
        double qb = e->tp_qblur;
        int half = (int)(qb * 2);
        for (int i = 0; i < n; i++) {
            double acc = 0, sum = 0;
            for (int j = -half; j <= half; j++) {
                int k = i + j;
                if (k < 0 || k >= n) continue;
                if (e->tp_stats[k].type != e->tp_stats[i].type) continue;
                double c = exp(-(double)j * j / (qb * qb));
                acc += q[k] * c;
                sum += c;
            }
            scratch[i] = sum > 0 ? acc / sum : q[i];
        }
        memcpy(q, scratch, (size_t)n * sizeof(*q));
    }

    double bits = 0;
    for (int i = 0; i < n; i++) {
        if (q[i] < qmin) q[i] = qmin;
        if (q[i] > qmax) q[i] = qmax;
        bits += tp_bits_at(e, i, q[i]);
    }
    return bits;
}

/* Build the plan. Called once at open, on the API thread, after the records
 * are read. Deterministic function of (records, target, gates) alone -- no
 * thread count, no clock, no coding state -- which is what keeps the threaded
 * splitter's output reproducible. */
static void tp_build_plan(yah264_encoder_t *e)
{
    int n = e->tp_n;
    if (n <= 0 || e->tp_target <= 0)
        return;

    e->tp_cost   = malloc((size_t)n * sizeof(*e->tp_cost));
    e->tp_qrec   = malloc((size_t)n * sizeof(*e->tp_qrec));
    e->tp_q      = malloc((size_t)n * sizeof(*e->tp_q));
    e->tp_ebits  = malloc((size_t)(n + 1) * sizeof(*e->tp_ebits));
    double *cplx    = malloc((size_t)n * sizeof(double));
    double *scratch = malloc((size_t)n * sizeof(double));
    if (!e->tp_cost || !e->tp_qrec || !e->tp_q || !e->tp_ebits || !cplx || !scratch) {
        free(cplx); free(scratch);
        free(e->tp_cost); free(e->tp_qrec); free(e->tp_q); free(e->tp_ebits);
        e->tp_cost = e->tp_qrec = e->tp_q = e->tp_ebits = NULL;
        e->tp_plan_on = 0;
        return;
    }

    for (int i = 0; i < n; i++) {
        e->tp_qrec[i] = tp_qp2qscale(e->tp_stats[i].qp);
        e->tp_cost[i] = (e->tp_stats[i].bits + 1.0) * e->tp_qrec[i];
    }

    /* Complexity blur : a two-sided gaussian over neighbouring
 * frames' costs. Reduces local QP fluctuation without blurring the QPs
 * themselves, which would let one simple frame drag down a complex
 * neighbour and waste bits on it. */
    if (e->tp_cplxblur > 0) {
        double cb = e->tp_cplxblur;
        int half = (int)(cb * 2);
        for (int i = 0; i < n; i++) {
            /* Plain two-sided gaussian. x264 additionally decays the running
 * weight by 1 - (intra MBs / MBs)^2, which stops the blur at a
 * scene change; our stats carry no intra count, so I stood the I
 * frame yah264 places at a cut in for it and MEASURED it -- and it
 * was a wash (sintel_720p, the one corpus clip with hard cuts, read
 * -26.38% with the cut break and -27.14% without). Not carried. */
            double acc = 0, sum = 0;
            for (int j = -half; j <= half; j++) {
                int k = i + j;
                if (k < 0 || k >= n) continue;
                double c = exp(-(double)j * j / 200.0);
                acc += e->tp_cost[k] * c;
                sum += c;
            }
            cplx[i] = sum > 0 ? acc / sum : e->tp_cost[i];
        }
    } else {
        memcpy(cplx, e->tp_cost, (size_t)n * sizeof(double));
    }

    int mbs = ((e->width + 15) / 16) * ((e->height + 15) / 16);
    double base_cplx = (double)mbs * 120.0;      /* x264, with B frames */

    /* Seed the bisection with the rate factor a flat scaling would need, so the
 * search range is content-independent . */
    double e1 = 1.0;
    for (int i = 0; i < n; i++)
        e1 += tp_bits_at(e, i, pow(cplx[i], 1.0 - e->crf_qcomp));
    double step_mult = e->tp_target / e1;

    double rf = 0;
    for (double step = 1e4 * step_mult; step > 1e-7 * step_mult; step *= 0.5) {
        rf += step;
        if (tp_solve(e, rf, cplx, e->tp_q, scratch, base_cplx) > e->tp_target)
            rf -= step;
    }
    if (rf <= 0) rf = step_mult > 0 ? step_mult : 1.0;
    tp_solve(e, rf, cplx, e->tp_q, scratch, base_cplx);

    e->tp_ebits[0] = 0;
    for (int i = 0; i < n; i++)
        e->tp_ebits[i + 1] = e->tp_ebits[i] + tp_bits_at(e, i, e->tp_q[i]);

    free(cplx);
    free(scratch);
}

/* The planned qscale for record `idx`, corrected for how far the coded bits
 * have drifted from the plan's own expected-bits curve. `spent` is the bits
 * actually committed, `pend` the predicted bits of frames in flight under the
 * pipeline -- the same virtual-ledger shape the ABR path uses, so the decision
 * stays a function of decide order and not of thread count.
 *
 * Both correction terms are x264's (its per-frame QP estimate, two-pass branch) and
 * both are BOUNDED, which is the property the greedy loop lacked:
 * - the drift term divides by clip3(., 0.5, 2), so no single frame can move
 * more than 6 QP off plan however far the stream has drifted;
 * - the model term multiplies by (actual / modelled) over finished frames,
 * which corrects a systematic bias in tp_bits_at rather than a local one.
 * The drift band tightens toward the end of the slice (scale_factor), because
 * bits owed with few frames left have to be repaid faster. */
/* Serial commit: the frame just coded is the record the cursor last handed out.
 * The rcp path cannot use this (several frames are in flight, so tp_idx-1 is not
 * the frame that landed) and carries the index in its pending entry instead. */
static void tp_account_plan(yah264_encoder_t *e, double bits, int coded_qp)
{
    e->tp_actual += bits;
    e->tp_nacc++;
    if (!e->tp_plan_on || !e->tp_q || e->tp_idx < 1)
        return;
    e->tp_ebsum += tp_bits_at(e, e->tp_idx - 1, tp_qp2qscale(coded_qp));
}

static double tp_plan_qscale(yah264_encoder_t *e, int idx, double spent, double pend)
{
    double q = e->tp_q[idx];
    if (!e->tp_corr)
        return q;

    double n = (double)e->tp_n;

    /* The measured model bias: actual bits over MODELLED bits at the QPs those
 * frames were really coded at. 1.0 until enough frames have landed. */
    double k = (e->tp_nacc >= e->tp_cwarm && e->tp_ebsum >= 1 && spent > 0)
             ? spent / e->tp_ebsum : 1.0;

    if (e->tp_resolve) {
        /* Re-solve the WHOLE remaining curve against the budget still unspent,
 * every frame, and scale the plan by the single factor that closes it.
 *
 * This is not the greedy loop coming back. The greedy loop had no
 * shape: it re-derived each frame's target independently, so a frame
 * that overshot took the bits out of whichever frames happened to
 * follow. Here the SHAPE is the plan -- every remaining frame moves by
 * the same factor s, so the I/B relation and the complexity curve are
 * exactly preserved -- and only the overall level tracks the budget.
 *
 * It replaces x264's two correction terms, which both failed here for
 * the same reason: x264 measures its bias against a bits model that
 * splits texture, motion and header, and ours lumps them (pass 1
 * records one total). Our bias is therefore large -- 0.88 on
 * stefan_cif -- and x264's cumulative-average term chases it without
 * ever arriving: the ratio walked 0.96 -> 0.88 over 90 frames while
 * the encode finished 4.3% under target. A global re-solve does not
 * have to converge, because it reads the remaining budget directly. */
        double rem_m = e->tp_ebits[e->tp_n] - e->tp_ebits[idx];
        double rem_t = e->tp_target - spent - pend;
        if (rem_m > 1 && rem_t > 1 && k > 0) {
            double s = rem_t / (k * rem_m);
            /* Same band x264 bounds its drift term with: one frame can move at
 * most 6 QP off plan, whatever the arithmetic asks for. Near the
 * last records rem_m goes to zero and s would otherwise explode. */
            if (s < 0.5) s = 0.5;
            if (s > 2.0) s = 2.0;
            q /= s;
        }
        double qlo = tp_qp2qscale(1), qhi = tp_qp2qscale(51);
        if (q < qlo) q = qlo;
        if (q > qhi) q = qhi;
        if (e->tp_dbg)
            fprintf(stderr, "tpp idx=%d qplan=%.3f qfin=%.3f spent=%.0f "
                    "remt=%.0f remm=%.0f k=%.4f\n", idx, e->tp_q[idx], q, spent,
                    rem_t, rem_m, k);
        return q;
    }

    double fps = e->tp_fps > 0 ? e->tp_fps : 25.0;
    double ovf_window = 2.0 * (e->tp_target * fps / n);      /* 2 s of bits */
    double final_bits = e->tp_ebits[e->tp_n];
    if (final_bits > 0) {
        double video_pos = e->tp_ebits[idx] / final_bits;
        double sf = sqrt((1.0 - video_pos) * n);
        ovf_window *= 0.5 * (sf > 0.5 ? sf : 0.5);
    }
    if (ovf_window > 0) {
        double diff = (spent + pend) - e->tp_ebits[idx];
        double f = (ovf_window - diff) / ovf_window;
        if (f < 0.5) f = 0.5;
        if (f > 2.0) f = 2.0;
        q /= f;
    }
    /* x264 gates this on being one second into the STREAM. Under the GOP
 * splitter each worker is its own encoder with its own cursor, so that
 * gate would re-arm every GOP and switch the term off for the first
 * second of each -- 60% of a short tail GOP. Count ACCOUNTED frames
 * instead, which is the sample size the ratio actually needs. */
    {
        double w = (double)idx / n * 100.0;
        if (w > 1.0) w = 1.0;
        if (w > 0 && k != 1.0) q *= pow(k, w);
    }
    double qmin = tp_qp2qscale(1), qmax = tp_qp2qscale(51);
    if (q < qmin) q = qmin;
    if (q > qmax) q = qmax;
    if (e->tp_dbg)
        fprintf(stderr, "tpp idx=%d qplan=%.3f qfin=%.3f spent=%.0f eb=%.0f "
                "ebsum=%.0f ratio=%.4f\n", idx, e->tp_q[idx], q, spent,
                e->tp_ebits[idx], e->tp_ebsum,
                e->tp_ebsum >= 1 ? spent / e->tp_ebsum : 0.0);
    return q;
}

/* Workers a wavefront can keep busy, for k frames overlapping.
 *
 * One frame's diagonal retires in 2(R-1)+C cell-times, so its average
 * parallelism is RC / (2(R-1)+C) -- that is the k=1 form this function shipped
 * with, and it is right about one frame. It is wrong about the POOL, because a
 * consumer frame's rows are row-gated on its reference and start `lag` rows
 * behind it rather than after it: k frames retire in 2(R-1)+C + (k-1)*2*lag
 * cell-times, not k times that.
 *
 * At CIF the difference is the whole of the multi-thread median leg.
 * k=1 gives 396/56 = 7, floored to 8, and ten cores
 * of an 18-core machine have nothing to do. k=2 gives 792/64 = 12 -- which is
 * where the measured optimum sits, +7.9% foreman / +3.7% bus / +3.1% stefan
 * against the k=1 cap, with 18 (the naive "just use the threads") measured
 * 6-11% WORSE than 8 because the extra workers contend for a diagonal that
 * cannot feed them. At 720p every k lands above --threads, so nothing changes.
 *
 * k=2 and not more because two is what the shipped staircase actually overlaps
 * (an anchor and the leaf row-gated onto it); k=3 computes 16.5 at CIF and
 * measures worse than 12. Raise it when the burst ring genuinely runs three. */
static int frame_thread_cap_k(int width, int height, int k, int lag)
{
    int C = (width + 15) / 16, R = (height + 15) / 16;
    if (C < 1) C = 1;
    if (R < 1) R = 1;
    if (k < 1) k = 1;
    int denom = 2 * (R - 1) + C + (k - 1) * 2 * lag;
    int cap = k * R * C / denom;                 /* R == 1 gives 1, as it should */
    if (cap < Y264_MT_POOL_MIN)
        cap = Y264_MT_POOL_MIN;
    return cap;
}

/* A frame is NARROW when one wavefront of it cannot fill a pool worth having:
 * its single-frame cap sits below this knee. That is the whole condition under
 * which overlapping frames have somewhere to go, and it is the condition both
 * halves of the width package key on -- a wider pool and the concurrency to
 * fill it flip together or not at all (k=2 without width is 6-11% SLOWER,
 * width without k=2 is mixed, together +6.1% foreman).
 *
 * Resolution only, deliberately. Keying this on --threads would make the bits a
 * function of the thread count, because the width package arms the multi-hop
 * MV clamp; the staircase's whole design rests on engagement never changing
 * bits. CIF's cap is 8 and 720p's is 45, so the knee separates them with room
 * either side. */
#define Y264_WF_WIDE_KNEE 16

static int wf_narrow_frame(int width, int height)
{
    static int env = -2;
    if (env == -2) { const char *s = getenv("Y264_WF_NARROW"); env = s ? atoi(s) : -1; }
    if (env >= 0) return env ? 1 : 0;       /* the package's single escape */
    return frame_thread_cap_k(width, height, 1, Y264_STAIR_LAG) < Y264_WF_WIDE_KNEE;
}

/* Y264_WF_CAPK: frames assumed to overlap when the pool is sized. Default 2 on
 * a narrow frame (the measured optimum: 12 at CIF, against 8 for one frame and
 * 18 for "just use the threads", which is 6-11% worse than 8), 1 otherwise --
 * a 720p wavefront already outruns --threads, so the k it was sized with never
 * shows. 0 = force the old single-frame cap everywhere. */
static int wf_capk(int width, int height)
{
    /* Separate latch: the resolved "unset" value is -2, which the usual
 * `v < 0` re-check treats as still-unresolved, so the old form re-ran
 * getenv and REWROTE the static on every call -- a perpetual writer that
 * no warm-at-open can quiet (the one TSan report left after the 08-29
 * sweep). Same-value and benign, but it fogs every future TSan hunt. */
    static int resolved = 0;
    static int v;
    if (!resolved) { const char *s = getenv("Y264_WF_CAPK");
                     v = s ? atoi(s) : -2; resolved = 1; }
    if (v >= 1) return v;
    if (v == 0) return 1;
    return wf_narrow_frame(width, height) ? 2 : 1;
}

int yah264_frame_thread_cap(int width, int height)
{
    return frame_thread_cap_k(width, height, wf_capk(width, height), Y264_STAIR_LAG);
}

/* MT stage 3 (thread-scaled clamp): the
 * staircase's row-gate margin and vertical MV clamp as a function of frame
 * height and the encoder's own pool width, in place of the fixed
 * Y264_STAIR_LAG constant. Independently implemented, matching the behaviour of i_mv_range_thread
 * :
 * "half of the available space is reserved and divided evenly among the
 * threads" -- max_range = (height + MARGIN) / pool_threads - MARGIN, half of
 * that is the wanted reach, floored (there: at i_me_range; here: converted
 * back into whole LAG rows and floored at Y264_STAIR_LAG), rounded up to a
 * whole unit. MARGIN_PX is 24 px, three macroblock rows, which is
 * already the same "-24" baked into Y264_STAIR_MVY_MAX's 16*LAG-24 -- not a
 * coincidence, both mark the same producer-side trailer margin
 * (stair_trailer_task's fin_l = 16j+13 vs a consumer touching up to 16j-6:
 * the two constants are the two sides of one inequality).
 *
 * SOUNDNESS: the floor (never returning less than Y264_STAIR_LAG) is the only
 * thing this function has to get right, and it is enforced unconditionally
 * below, not tuned per shape. The reason it is safe to let this return MORE
 * than the floor (which is the entire point -- taller frames at lower pool
 * widths get a wider clamp) is in me.h's Y264_STAIR_MVY_MAX comment: the
 * row-gate bound and the MV clamp bound both scale by exactly 16 per unit of
 * LAG, so the slack between producer and consumer is a LAG-independent
 * constant. Raising LAG can only ever add margin, never remove it. Lowering
 * the FLOOR itself is the unsound direction and this function never does
 * that: LAG 3 is measured UNSOUND at the current margin arithmetic (me.h) and
 * worth only 1.01x on CIF, so this deliberately does not attempt it.
 *
 * On CIF/720p at 8-18 pool threads this returns exactly Y264_STAIR_LAG -- the
 * formula's natural value is already below the floor there, same as x264's own
 * clamp collapses to its me_range floor at high thread counts on
 * small-to-medium frames. It stops being a no-op on taller frames run at a
 * narrower pool (1080p+ at a handful of threads), where it genuinely widens the
 * search reach -- a real MV-clamp change, not a timing one, so THAT case needs
 * its own BD check. */
static int stair_lag_for(int height_in_mbs, int pool_threads)
{
    if (height_in_mbs < 1) height_in_mbs = 1;
    if (pool_threads < 1) pool_threads = 1;
    const long MARGIN_PX = 24;                      /* 3 MB rows of trailer */
    long height_px = (long)height_in_mbs * 16;
    long max_range = (height_px + MARGIN_PX) / pool_threads - MARGIN_PX;
    long want_px = max_range / 2;                   /* x264: half reserved, half free */
    if (want_px < 0) want_px = 0;
    /* Invert Y264_STAIR_MVY_MAX's px = 16*LAG - 24 for the smallest LAG that
 * covers at least want_px of reach (ceil division). */
    long lag = (want_px + MARGIN_PX + 15) / 16;
    if (lag < Y264_STAIR_LAG) lag = Y264_STAIR_LAG;  /* the proven-sound floor */
    if (lag > height_in_mbs) lag = height_in_mbs;    /* self-limiting: the row gate
 * already clamps need at hmb */
    return (int)lag;
}

/* The width of the wavefront pool this open will build: the requested frame
 * threads, capped at the grid's critical-path knee unless Y264_WF_THREADS
 * overrides (probing above the knee is what that env is for). >1 means a pool.
 *
 * Split out of the ntp_pool_create site because two things now need the answer
 * and they run at different points in encoder_open: the pool itself, and
 * (earlier) dpbp_open's sizing rule, which reads the width-engagement
 * predicate. One function so the two can never disagree -- the same reason
 * stair_wide_nref_ok exists. */
/* param.threads is a BUDGET for this one encoder instance: 0 asks the library to
 * decide, 1 is serial, N means up to N. It is the only threading value a caller
 * has to supply, which is the point -- the ffmpeg wrapper ran every encode on one
 * core because the budget was documented as "0 = auto" and nothing implemented
 * auto, so a caller that set only what the header asked for got serial.
 *
 * frame_threads stays the explicit low-level width and WINS when set, because
 * the CLI's GOP workers each carry their own share and must not have it
 * recomputed from the whole-machine budget. */
/* Ceiling on what AUTO asks for. It does not bound an explicit request: a caller
 * who says 32 gets 32, still clamped by what the picture can keep busy.
 *
 * 16 by default, an owner call, and the same ceiling the wider H.264 world
 * settles on. The reasoning is that thread scaling in a row wavefront is bounded
 * by the picture's critical path long before it is bounded by the machine, so
 * past a point extra workers buy wake traffic rather than throughput, and on an
 * asymmetric machine the last few land on the slow cores and lengthen the
 * critical path outright. A default should be the number that is safe on an
 * unknown box, not the largest number that ever helped on a known one.
 *
 * Y264_AUTO_THREADS pins the budget outright, above or below the ceiling, so a
 * sweep never needs a rebuild. Y264_AUTO_THREADS_MAX moves the ceiling alone. */
#define Y264_AUTO_CEILING 16

static int auto_threads(void)
{
    const char *pin = getenv("Y264_AUTO_THREADS");
    if (pin) {
        int v = atoi(pin);
        return v < 1 ? 1 : v;
    }
    {
        const char *c = getenv("Y264_AUTO_THREADS_MAX");
        int ceiling = c ? atoi(c) : Y264_AUTO_CEILING;
        int n = y264_machine_threads();
        if (ceiling < 1)
            ceiling = 1;
        return n > ceiling ? ceiling : n;
    }
}

static int resolve_threads(const yah264_param_t *param)
{
    int t = param->threads;
    if (t <= 0)
        t = auto_threads();
    return t < 1 ? 1 : t;
}

int yah264_threads_auto(void)
{
    return auto_threads();
}

static int wf_width_for(const yah264_param_t *param)
{
    const char *wf = getenv("Y264_WF_THREADS");
    if (wf)
        return atoi(wf);        /* probe override: deliberately uncapped */
    {
        int wt = param->frame_threads > 0 ? param->frame_threads
                                          : resolve_threads(param);
        int cap = yah264_frame_thread_cap(param->width, param->height);
        return wt > cap ? cap : wt;
    }
}

/* The lookahead window this open will build. Split out of encoder_open for the
 * same reason wf_width_for was: two callers now need the answer -- open itself
 * and yah264_lookahead_delay, which the CLI asks before it opens anything --
 * and a second copy of the rule would eventually disagree with the first. */
static int la_depth_for(const yah264_param_t *param)
{
    int mbtree_ipppp = 1;
    { const char *v = getenv("Y264_MBTREE_IPPP"); if (v) mbtree_ipppp = atoi(v); }
    int want_la = (param->bframes > 0) || (mbtree_ipppp && param->rc.lookahead > 0);
    int d = want_la ? param->rc.lookahead : 0;
    if (d < 0) d = 0;
    if (d > 64) d = 64;
    return d;
}

/* How many frames of lead the decoupled lookahead gets: param.sync_lookahead
 * resolved, x264's --sync-lookahead rule ,
 * -- default i_bframe+1, forced to 0 where a lookahead thread cannot pay).
 *
 * Our "cannot pay" test is just "there is no pool to run a chain against".
 * It was very nearly the wavefront's knee instead -- the theory being that
 * yah264_frame_thread_cap bounds the workers a lone grid can keep busy, so a
 * request above the knee means idle cores the la thread can have for free,
 * while a knee at or above the request means the wavefront wants every worker
 * and the thread steals one. That theory predicts the CIF win and the 720p
 * regression, and it is WRONG. Measured at t18, forced lead 4, interleaved
 * best-of-9:
 *
 * foreman_cif -4.2% bus_cif -2.5% (knee 8, below the pool)
 * samsung_720p -15.2% sintel_720p -12.3% ducks_720p -0.7%
 * park_joy_720p +0.8% (knee 21, above the pool)
 * touchdown 1080p -2.2% (knee 32, well above the pool)
 *
 * The knee sorts none of that. What looks like a size effect is a
 * LEAD effect: the same sweep run with the thread engaged and zero lead costs
 * +1.5% to +10.6% on the very clips the lead wins 12-15% on. A chain with no
 * lead cannot overlap anything, so it only adds a contending thread -- which is
 * why this returns the lead and la_th_on follows it, exactly as x264 couples
 * the two returns threadless at --sync-lookahead 0). The
 * payoff is set by the lookahead's share of the driver's critical path, and
 * that is content, not frame size: park_joy is the one clip where it does not
 * pay, and it costs 0.8%.
 *
 * Static configuration only -- param fields and the pool width this open will
 * build. Nothing here asks what the machine is doing at the time, which is the
 * session-6 rule: scheduling may adapt, coding decisions may not. The lead is
 * byte-identical anyway, so all it picks is WHERE work runs and how many frames
 * of input are held first.
 *
 * Pure function of param, so it can be called before e->wf_width exists. */
static int la_lead_for(const yah264_param_t *param, int la_depth)
{
    if (la_depth <= 0)                          /* no window, nothing to lead */
        return 0;
    int env = la_buf_env();
    if (env != Y264_LA_BUF_UNSET)
        return env > 0 ? env : 0;
    if (param->sync_lookahead < 0)              /* explicit off (--sync-lookahead 0) */
        return 0;
    if (param->sync_lookahead > 0)              /* explicit lead */
        return param->sync_lookahead;
    if (wf_width_for(param) < la_pool_min())    /* auto: no pool, no thread to feed */
        return 0;
    int bf = param->bframes;                    /* as open will clamp it */
    if (bf < 0) bf = 0;
    if (bf > 7) bf = 7;
    return bf + 1;                              /* x264's own magnitude */
}

/* Public: the frames of input latency these parameters add, i.e. the resolved
 * lookahead lead. Clamped exactly as open clamps it. This is the ONLY cost of
 * the lead -- the bitstream is identical at any value -- so it is the number to
 * put in front of a caller who cares about latency. B-frame reorder delay is
 * separate and unchanged by this. */
int yah264_lookahead_delay(const yah264_param_t *param)
{
    if (!param) return 0;
    int d = la_depth_for(param);
    int buf = la_lead_for(param, d);
    if (buf < 0) buf = 0;
    if (buf > Y264_LA_CAP_MAX - d) buf = Y264_LA_CAP_MAX - d;
    return buf > 0 ? buf : 0;
}

static int hw_env(void);
static int hw_scenecut_env(void);
static int hw_scenecut_probe(yah264_encoder_t *e, const yah264_picture_t *pic);
static void hw_scenecut_free(yah264_encoder_t *e);
static void warm_lr_statics(void);

static yah264_encoder_t *encoder_open_sw(const yah264_param_t *param)
{
    warm_lr_statics();          /* resolve env-gated lowres statics on this thread
 * (CLI primes on the main thread before workers) */
    (void)tprof_on();           /* profiling gate: warm before any bg thread reads it */
    y264_cabac_warm();          /* build the RDOQ bit table before any worker runs */
    y264_cavlc_warm();          /* likewise the VLC tables */
    if (!param)
        return NULL;
    /* The public enums carry x264's values, and x264 has cases this encoder does
 * not implement. Refuse them here. Accepting one and narrowing it to the
 * nearest thing we do have is exactly the silent-wrong-tool failure the
 * numbering was changed to remove, so every one of these is a hard no:
 * an unsupported csp is not 4:2:0, X264_ME_ESA is not UMH, and
 * X264_DIRECT_PRED_NONE is not spatial. Do NOT go back to a contiguous
 * range test or a switch default: both admit values that then encode as
 * something else. */
    if (param->csp != YAH264_CSP_I420 && param->csp != YAH264_CSP_I422 &&
        param->csp != YAH264_CSP_I444)
        return NULL;
    if (param->rc.method != YAH264_RC_CQP && param->rc.method != YAH264_RC_CRF &&
        param->rc.method != YAH264_RC_ABR && param->rc.method != YAH264_RC_2PASS)
        return NULL;
    if (param->me_method != YAH264_ME_AUTO && param->me_method != YAH264_ME_DIA &&
        param->me_method != YAH264_ME_HEX && param->me_method != YAH264_ME_UMH)
        return NULL;
    if (param->direct != YAH264_DIRECT_SPATIAL && param->direct != YAH264_DIRECT_TEMPORAL
        && param->direct != YAH264_DIRECT_AUTO)
        return NULL;
    if (param->width <= 0 || param->height <= 0)
        return NULL;
    if ((param->width & 1) || (param->height & 1))
        return NULL;                                /* 4:2:0 needs even dims */
    /* The syntax elements appended in 2026-09-16 carry the spec's own domains.
 * Out of range is refused rather than clamped: a clamped offset writes a
 * legal-looking header that does not say what the caller asked for. */
    if (param->deblock_alpha < -6 || param->deblock_alpha > 6 ||
        param->deblock_beta  < -6 || param->deblock_beta  > 6)
        return NULL;
    if (param->chroma_qp_index_offset < -12 || param->chroma_qp_index_offset > 12)
        return NULL;
    if (param->sps_id < 0 || param->sps_id > 31)
        return NULL;
    if (param->mvrange < 0)
        return NULL;
    if (param->rc.qp_min < 0 || param->rc.qp_min > 51 ||
        param->rc.qp_max < 0 || param->rc.qp_max > 51 ||
        param->rc.qp_step < 0 || param->rc.qp_step > 51)
        return NULL;
    if (param->rc.qp_max && param->rc.qp_min > param->rc.qp_max)
        return NULL;
    if (param->rc.vbv_init < 0)
        return NULL;

    yah264_encoder_t *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->param = *param;
    /* Resolve the budget into the copy the encoder reads from, so every
     * predicate keyed on threads sees what this instance actually got rather
     * than the caller's request. stq is the one that matters: it engages the
     * single-thread quality mode on threads == 1, and while the budget stayed
     * unresolved a caller who never set it (0) was not serial by intent yet
     * encoded serial in fact. Resolved, 0 becomes the machine width and stq
     * disengages, which is what a threaded encode should do. */
    e->param.threads = resolve_threads(param);
    e->width = param->width;
    e->height = param->height;
    e->prev_anchor_poc2 = -1;   /* calloc's 0 is a real POC; "none" is -1 */
    refb_hist_reset(e);         /* same reason, for the v6 reference-B history */
    /* SubWidthC/SubHeightC per chroma format (H.264 Table 6-1). */
    switch (param->csp) {
    case YAH264_CSP_I444:  e->cf_idc = 3; e->sub_w = 1; e->sub_h = 1; break;
    case YAH264_CSP_I422:  e->cf_idc = 2; e->sub_w = 2; e->sub_h = 1; break;
    case YAH264_CSP_I420:
    default:                e->cf_idc = 1; e->sub_w = 2; e->sub_h = 2; break;
    }
    /* 4:4:4 I_8x8 chroma is not yet wired; disable the 8x8 transform so luma
 * never selects I_8x8 (which chroma would have to mirror). */
    if (e->cf_idc == 3)
        e->param.transform8x8 = 0;
    e->width_in_mbs = (param->width + 15) / 16;
    e->height_in_mbs = (param->height + 15) / 16;
    /* frame_mbs_only_flag 0 makes FrameHeightInMbs = 2 x PicHeightInMapUnits,
 * so the coded height has to be an even number of macroblock rows. 720p is
 * 45, which is the common case, so --fake-interlaced pads rather than
 * refusing; the extra row is cropped away and the crop unit doubles with
 * the flag, which is what the CropUnitY expression below already says. */
    if (param->fake_interlaced && (e->height_in_mbs & 1))
        e->height_in_mbs++;
    e->padded_w = e->width_in_mbs * 16;
    e->padded_h = e->height_in_mbs * 16;

    /* Early-skip probe acceptance. DEFAULT OFF (0/0) pending the corpus BD
 * round; resolved here so the analyze workers only ever read frame fields.
 * Y264_SKIP_DECIMATE=<p>[,<b>] -- one value sets both legs.
 * 0 off (probe fails on any surviving coefficient, the shipped test)
 * 1 coder-consistent: accept blocks our own decimator would zero
 * 2 x264's whole-MB accumulation against Y264_SKIP_DECIMATE_T (x264: 6)
 * 3 the composition: 1 per block, then 2 on what survives it */
    {
        const char *v = getenv("Y264_SKIP_DECIMATE");
        int p = 0, b = 0;
        if (v && *v) {
            p = atoi(v);
            const char *c = strchr(v, ',');
            b = c ? atoi(c + 1) : p;
        }
        e->skipdec_p = p;
        e->skipdec_b = b;
        v = getenv("Y264_SKIP_DECIMATE_T");
        e->skipdec_t = v && *v ? atoi(v) : 6;
        /* Y264_SKIP_MVAGREE=<p>[,<b>], qpel L1: require the lookahead's motion
 * estimate to land on the skip/direct MV before the tolerance applies.
 * 0 = off, which is the "tolerance without confirmation" arm. Per leg
 * because they need different amounts of it -- the P acceptance at
 * mode 1 is coder-consistent and carries no risk to pay for, while the
 * B leg is replacing a round-to-nearest test with a deadzone one on
 * MVs that were never searched. */
        v = getenv("Y264_SKIP_MVAGREE");
        int ap = 0, ab = 0;
        if (v && *v) {
            ap = atoi(v);
            const char *c = strchr(v, ',');
            ab = c ? atoi(c + 1) : ap;
        }
        e->skip_mvagree_p = ap;
        e->skip_mvagree_b = ab;
        /* Y264_SKIP_COSTGATE=<k>: B tolerance only where dist_mb <= k*lambda. */
        v = getenv("Y264_SKIP_COSTGATE");
        e->skip_costgate = v && *v ? atoi(v) : 0;
        /* Y264_BSKIP_CONFIRM=<tol>[,<dec>]: x264's ACTUAL B structure, and the
 * one point on the previous round's trade-off curve that used a real
 * motion estimate rather than a proxy for it. The tolerant probe's
 * answer is held and believed only after the 16x16
 * searches on list0 ref0 and list1 ref0 both land within <tol> qpel of
 * the direct MV --. tol 0 = off. <dec> is the
 * acceptance mode the DEFERRED probe runs (see skipdec_*), default 1.
 *
 * Distinct from skip_mvagree_b, which asks the same question of the
 * LOOKAHEAD's lowres MV before any search: that is the proxy, and it is
 * measured too coarse. */
        v = getenv("Y264_BSKIP_CONFIRM");
        int bt = 0, bd = 1;
        if (v && *v) {
            bt = atoi(v);
            const char *c = strchr(v, ',');
            if (c) bd = atoi(c + 1);
        }
        e->bskip_confirm = bt;
        e->bskip_dec = bd;
        /* Y264_BSKIP_PROBE=1: the single graded probe, term 1 on its own. It
         * replaces the B round-to-nearest test with the deadzone+trellis one
         * the coder actually runs and commits only where NOTHING survives, so
         * it spends no tolerance and owes no confirmation. Kept separate from
         * BSKIP_CONFIRM because the two are different sizes and bundling
         * changes into one number hides the ones that are actively worse.
         * CONFIRM implies PROBE (it has nothing to defer otherwise) but is
         * measured on top of it, never instead. */
        v = getenv("Y264_BSKIP_PROBE");
        e->bskip_probe = (v && *v) ? atoi(v) : (bt ? 1 : 0);
        v = getenv("Y264_BSKIP_NOTRELLIS");
        e->bskip_notrellis = (v && *v) ? atoi(v) : 0;
        /* Y264_BSKIP_ADMIT=<tol>: E2 stage A. The measured defect of the
 * BSKIP_PROBE/CONFIRM pair is not its decision but its POPULATION -- it
 * probes every B macroblock for 1.6-3.3% of wall and converts a small
 * fraction, which is why the whole arm nets a loss (0.975-0.998) against
 * a same-placement ceiling of 1.040-1.097. Stage A pays the probe only
 * where the lookahead's own pair MVs already land within <tol> qpel of
 * BOTH direct MVs. It gates the probe, never the skip, so its false
 * positives cost time and its false negatives cost only the catches
 * they decline. 0 = off. */
        v = getenv("Y264_BSKIP_ADMIT");
        e->bskip_admit = (v && *v) ? atoi(v) : 0;
        /* Y264_BSKIP_CGUARD=<mask>: E2 stage C's guard set at the post-ref0
 * commit. bit0 = direct must be SATD-competitive with the ref-0
 * searches (bexit_ok's shape, but off the two searches x264 itself
 * considers sufficient rather than the full SATD phase); bit1 = the
 * skip's own distortion must be cheap in lambda units (skip_costgate's
 * shape, the rate-awareness SATD cannot supply); bit2 = reference B's
 * additionally need the propagation guard E1 validated. */
        v = getenv("Y264_BSKIP_CGUARD");
        e->bskip_cguard = (v && *v) ? atoi(v) : 0;
    }

    e->cpu = y264_cpu_detect();
    y264_dsp_init();                                /* auto-select SIMD kernels */
    y264_me_set_subme(param->subme);                /* ME effort (hex vs UMH) */
    y264_me_set_method(param->me_method);           /* --me override (0 = follow subme) */
    y264_me_set_subpel(param->subpel);              /* subpel pattern (preset-scaled) */

    int has_b = e->param.bframes > 0;
    e->sps.entropy_coding_mode_flag = e->param.cabac ? 1 : 0;
    e->sps.profile_idc = y264_profile_idc(e->param.cabac, e->param.bframes); /* Main for CABAC/B, else Baseline */
    if (e->sps.profile_idc == 66)
        e->sps.profile_idc = 77;    /* weighted prediction is always signalled (pps below), and
 * Annex A.2.1 forbids it in Baseline: never claim Baseline */
    /* CAVLC at Main: 9.2.2.1 forbids level_prefix > 15 below High, so the
 * quantiser caps |level| at 2063 (the prefix-15 maximum) and the writer
 * reports if a larger one ever reaches it. High profiles have no cap. */
    {
        int cap = (!e->param.cabac && e->sps.profile_idc < 100) ? 2063 : 0;
        y264_quant_set_level_max(cap);
        y264_cavlc_set_prefix15(cap != 0);
    }
    if (e->param.transform8x8)
        e->sps.profile_idc = 100;                   /* 8x8 transform is High profile */
    /* Custom quant matrices require High profile and carry the scaling lists in
 * the SPS. Flat (cqm 0) leaves the sequence scaling matrix absent, so the
 * decoder uses the flat-16 default and output is byte-identical. */
    e->cqm_on = param->cqm ? 1 : 0;
    if (e->cqm_on) {
        y264_cqm_jvt(&e->cqm);
        e->sps.profile_idc = 100;
        e->sps.cqm = &e->cqm;
    }
#if Y264_BIT_DEPTH > 8
    e->sps.profile_idc = 110;                       /* High 10 (bit_depth > 8) */
#endif
    e->sps.overscan = e->param.overscan;
    e->sps.video_format = e->param.video_format;
    e->sps.pic_struct_present = e->param.pic_struct ? 1 : 0;
    /* video_format lives inside video_signal_type, so naming it opens that
 * block even when no colour description was given. The three colour codes
 * are then pinned to 2 (unspecified) so no colour_description is written:
 * a zeroed struct would read as H.273 code 0, which is "reserved", and a
 * reserved code asserted about real content is worse than saying nothing.
 * yah264_encoder_set_video_signal, which runs later, overwrites all of it
 * when a --colorprim/--transfer/--colormatrix was actually given. */
    if (e->param.video_format >= 0 && e->param.video_format <= 5) {
        e->sps.vs_present = 1;
        if (!e->sps.vs_primaries) e->sps.vs_primaries = 2;
        if (!e->sps.vs_transfer)  e->sps.vs_transfer  = 2;
        if (!e->sps.vs_matrix)    e->sps.vs_matrix    = 2;
    }
    e->sps.chroma_format_idc = e->cf_idc;
    if (e->cf_idc != 1)
        e->sps.profile_idc = e->cf_idc == 3 ? 244 : 122;  /* High 4:4:4 / High 4:2:2 */
    /* A forced profile is a CONSTRAINT: the derivation above says the lowest
 * profile this stream already obeys, and an explicit one may only be at or
 * above it. Below it the answer is a refusal, never a profile_idc the
 * stream does not obey -- a decoder that trusts the header and then meets a
 * tool the profile forbids is the failure this refuses to ship.
 *
 * Baseline is the one that also CHANGES something: A.2.1 forbids weighted
 * prediction, which this encoder signals in the PPS unconditionally, which
 * is why the derivation above never claims 66 on its own. Asking for it
 * turns the signalling off. */
    if (e->param.profile_idc) {
        static const struct { int idc; const char *name; } PROF[] = {
            { 66, "baseline" }, { 77, "main" }, { 100, "high" },
            { 110, "high10" }, { 122, "high422" }, { 244, "high444" },
        };
        int want = e->param.profile_idc, ok = 0;
        const char *wname = "?";
        for (size_t i = 0; i < sizeof PROF / sizeof *PROF; i++)
            if (PROF[i].idc == want) { ok = 1; wname = PROF[i].name; }
        if (!ok) {
            fprintf(stderr, "yah264: unknown profile_idc %d\n", want);
            yah264_encoder_close(e);
            return NULL;
        }
        /* The lowest profile this TOOL-SET obeys, derived from the tools
 * themselves rather than from e->sps.profile_idc above -- that value
 * has already been bumped off 66 because weighted prediction is
 * signalled unconditionally, and Baseline is exactly the request that
 * turns the signalling off. Containment order, which for the six
 * profiles this encoder emits agrees with numeric order. */
        int need = 66;
        if (e->param.cabac || has_b) need = 77;
        if (e->param.transform8x8 || e->cqm_on) need = 100;
        if (Y264_BIT_DEPTH > 8) need = 110;
        if (e->cf_idc == 2) need = 122;
        if (e->cf_idc == 3) need = 244;
        if (want < need) {
            const char *why =
                e->cf_idc == 3               ? "4:4:4 input needs High 4:4:4"
              : e->cf_idc == 2               ? "4:2:2 input needs High 4:2:2 or above"
              : Y264_BIT_DEPTH > 8           ? "this build's bit depth needs High 10 or above"
              : e->cqm_on                    ? "custom quant matrices need High or above"
              : e->param.transform8x8        ? "the 8x8 transform needs High or above"
              :                                "CABAC and B frames need Main or above";
            fprintf(stderr, "yah264: --profile %s cannot code this stream: %s\n", wname, why);
            yah264_encoder_close(e);
            return NULL;
        }
        if (want == 110 && Y264_BIT_DEPTH == 8) {
            fprintf(stderr, "yah264: --profile high10 needs a 10-bit build "
                            "(meson setup build -Dbit_depth=10); this one is 8-bit\n");
            yah264_encoder_close(e);
            return NULL;
        }
        e->sps.profile_idc = want;
        e->profile_forced = 1;
        /* A named profile the stream was just checked against is an assertion
 * the stream obeys it, which is what a constraint_set flag says.
 * constraint_set0 comes from profile_idc 66 itself; Main adds
 * constraint_set1. The High family asserts nothing extra: its
 * constraint_set3 means "Intra profile", which this encoder is not. */
        if (want == 77) e->sps.constraints |= 0x40;   /* constraint_set1_flag */
    }
    /* level_idc computed after max_num_ref_frames (the DPB size) below. */
    e->sps.sps_id = e->param.sps_id;
    /* A flat B run is still B frames; only the hierarchy goes. */
    e->b_pyramid = e->param.bframes >= 2 && e->param.b_pyramid;  /* set early: sizes SPS fields */
    /* Multi-reference list 0: the N most recent anchors (IPPP / flat B), or the
 * N nearest DPB references (b-pyramid). */
    e->nref = (e->param.ref > 1) ? (e->param.ref > 16 ? 16 : e->param.ref) : 1;
    /* The wider FrameNum whenever the pyramid or multi-reference needs it --
 * and always under --stitchable, which is the other SPS field that moves
 * with --ref/--bframes. The wide form is legal at any reference count; it
 * costs 4 bits per slice header, which is what a settings-independent SPS
 * is worth to a caller who asked for one. */
    e->sps.log2_max_frame_num_minus4 =
        (e->b_pyramid || e->nref > 1 || e->param.stitchable) ? 4 : 0;
    e->sps.pic_order_cnt_type = 0;                  /* explicit POC (enables reordering) */
    e->sps.log2_max_pic_order_cnt_lsb_minus4 = 4;  /* 8-bit poc_lsb */
    /* Sliding window: nref anchors, plus the future anchor for B, plus the
 * pyramid's MARKED reference B's. Charging `bframes` here OVER-CHARGES: the
 * pyramid marks only its internal nodes (1 for bframes 2-3, 2 at 4, 3 at
 * 5-7 -- stair_plan_nrefb reads it off the coding plan itself, the same way
 * dpbp_open sizes its bag pool), and the sliding window never holds more
 * marked ref-Bs than one mini-GOP emits. The over-charge costs two whole DPB
 * frames at medium, enough to push a 720p stream from level 3.1 (where x264
 * lands) to 4.0. Y264_DPB_TIGHT=0 restores the wide window -- an escape hatch,
 * because the window's SIZE times decoder-side eviction, so tightening it
 * changes which references the pyramid's lists can offer (bits move). */
    {
        int nrefb = e->b_pyramid ? e->param.bframes : 0;
        /* The tight window is NOT applied where the wide staircase can engage
         * (ref <= 1 shapes): its slot recycling depends on the wide window's
         * slack -- determ_repeat under load reads 4/16 nondeterministic
         * configs with the tight window there (all ref1) and 16/16 with the
         * wide one. The serial path is proven either way (recon_sweep 300/300
         * tight). */
        /* Plus the rc half: under ABR/2-pass the zero-lag decide keeps width
         * disengaged, so the tight window is still safe there and the SPS
         * (hence the auto level) must not move. stair_wide_rc_ok CANNOT be
         * called here -- e->rcp_on/rcp_lag resolve further down in open, so
         * reading them here sees zeros, passes ABR through and moves level_idc
         * 3.1 -> 4.0. Mirror it from the params this early code does have. */
        int sps_rc_decide = rc_pipe_env()
            && ((param->rc.method == YAH264_RC_ABR && param->rc.bitrate > 0)
                || param->rc.pass > 0
                || (param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0))
            && (!(param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0)
                || rcp_vbv_env());
        int sps_vbv = param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0;
        int sps_wide_rc = !sps_rc_decide || (rcp_lag_for(e) > 0 && !sps_vbv);
        if (e->b_pyramid && !(stair_wide_on() && stair_wide_nref_ok(e)
                              && sps_wide_rc)) {
            const char *t = getenv("Y264_DPB_TIGHT");
            if (!t || atoi(t))
                nrefb = stair_plan_nrefb(e->param.bframes);
        }
        int mrf = e->nref + (has_b ? 1 : 0) + nrefb;
        e->sps.max_num_ref_frames = mrf > 15 ? 15 : mrf;
        /* Auto-select the minimum conformant level from resolution/framerate/DPB
 * (Annex A). fps rounded up so MaxMBPS isn't under-counted; the DPB term
 * is max_dec_frame_buffering (A.3.1). The current picture is NOT part of
 * that bound, so a +1 here would be one frame too conservative. */
        int fs = e->width_in_mbs * e->height_in_mbs;
        int fps_num = e->param.timebase.fps_num > 0 ? e->param.timebase.fps_num : 25;
        int fps_den = e->param.timebase.fps_den > 0 ? e->param.timebase.fps_den : 1;
        long fps = (fps_num + fps_den - 1) / fps_den;
        /* --stitchable picks the level for the BIGGEST DPB it will ever
 * declare, not for the one this encode needs. A level that moves with
 * --ref is a level two otherwise-identical encodes disagree about,
 * which is the whole thing the flag exists to prevent; picking it from
 * a fixed 15 frames leaves it a function of frame size, frame rate and
 * rate cap alone, all of which the two encodes share. It costs a
 * higher declared level, and with it a wider MaxVmvR -- so a stitchable
 * stream really is a different encode, not only different bytes. */
        long dpb_mbs = (long)fs * (e->param.stitchable ? Y264_STITCH_DPB
                                                       : e->sps.max_num_ref_frames);
        long br_kbps = e->param.rc.vbv_maxrate > e->param.rc.bitrate ? e->param.rc.vbv_maxrate : e->param.rc.bitrate;
        if (e->param.rc.method != YAH264_RC_ABR && e->param.rc.method != YAH264_RC_2PASS && e->param.rc.vbv_maxrate <= 0)
            br_kbps = 0;                                  /* CRF/CQP without a cap: no MaxBR constraint */
        int auto_level = compute_level_idc(fs, (long)fs * fps, dpb_mbs, br_kbps, e->sps.profile_idc >= 100);
        if (e->param.level_idc > 0) {
            e->sps.level_idc = e->param.level_idc;    /* forced --level */
            if (e->param.level_idc < auto_level)
                fprintf(stderr, "yah264: warning: forced level %d.%d is below the "
                        "conformant minimum %d.%d for this resolution/framerate/DPB "
                        "-- the stream may be non-conformant\n",
                        e->param.level_idc / 10, e->param.level_idc % 10,
                        auto_level / 10, auto_level % 10);
        } else {
            e->sps.level_idc = auto_level;
        }
    }
    /* The level's motion-vector range (Table A-1): vertical MaxVmvR by level,
 * horizontal +-2048 luma samples at every level. Every search window and
 * the temporal-direct legality check clamp to it (review 2026-09-04). */
    {
        int lv = e->sps.level_idc, vr;
        if (lv <= 10) vr = 64; else if (lv <= 20) vr = 128; else if (lv <= 30) vr = 256; else vr = 512;
        e->mv_ylim_q = 4 * vr;
        e->mv_xlim_q = 4 * 2048;
        /* --mvrange narrows the VERTICAL range below the level's, in luma
 * samples. Above the level's is refused at the CLI and ignored here:
 * Table A-1 is a conformance bound, not a suggestion. */
        if (e->param.mvrange > 0 && 4 * e->param.mvrange < e->mv_ylim_q)
            e->mv_ylim_q = 4 * e->param.mvrange;
        /* Declare the bound the search is actually held to, rather than a flat
 * 16 that advertises +-16384 luma samples at every level. ceil(log2) so
 * the declaration is never NARROWER than what the search may return --
 * a --mvrange that is not a power of two rounds the declaration up. */
        {
            int v = 1;
            while ((1 << v) < e->mv_ylim_q) v++;
            e->sps.log2_mv_len_v = v;
        }
        /* Y264_MV_LIMIT=<qpel>: probe override of both limits (0 = the level's). */
        { const char *s = getenv("Y264_MV_LIMIT"); int v = s ? atoi(s) : 0;
          if (v > 0) e->mv_xlim_q = e->mv_ylim_q = v; }
    }
    /* Output delay for the VUI bitstream restriction: the b-pyramid's coding-
 * to-display distance is its depth (ceil(log2(bframes+1))); flat B is 1. */
    {
        int reo = 0;
        if (e->b_pyramid) {
            int d = 0, n = e->param.bframes + 1;
            while ((1 << d) < n) d++;
            reo = d;
        } else if (has_b) {
            reo = 1;
        }
        /* The ABR/2-pass decide lag EMITS each of the next `lag` anchors
 * before the previous mini-GOP's B frames, so the true output delay
 * deepens by the lag. Mirror the same could-engage predicate the DPB
 * sizing above uses (rcp_on resolves late in open, so the SPS must be
 * conservative). Under-declaring is not benign: a conforming decoder
 * outputs on the advertised depth and silently DROPS the late Bs --
 * under-declared, bus t12 ABR decodes 119 of 150 frames. */
        int lag_rc_decide = rc_pipe_env()
            && ((param->rc.method == YAH264_RC_ABR && param->rc.bitrate > 0)
                || param->rc.pass > 0
                || (param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0))
            && (!(param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0)
                || rcp_vbv_env());
        int lag_vbv = param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0;
        if (has_b && lag_rc_decide && rcp_lag_for(e) > 0 && !lag_vbv
            && stair_wide_on() && stair_wide_nref_ok(e))
            reo += rcp_lag_for(e);
        e->sps.max_num_reorder_frames = reo;
        e->sps.max_dec_frame_buffering = e->sps.max_num_ref_frames > reo
                                       ? e->sps.max_num_ref_frames : reo;
        /* --stitchable: declare the level's own maximum instead of what this
 * encode needs, so two streams at the same geometry carry the same SPS
 * whatever --ref and --bframes were. A declared DPB is an upper bound
 * the decoder sizes for, never an instruction to the encoder, so
 * nothing about what is coded moves -- only the three ue(v) in the
 * SPS. The rest of the SPS was already independent of the CONTENT; what
 * this adds is independence of the SETTINGS. */
        if (e->param.stitchable) {
            int fs = e->width_in_mbs * e->height_in_mbs;
            int cap = level_max_dpb_frames(e->sps.level_idc, fs);
            if (cap > Y264_STITCH_DPB) cap = Y264_STITCH_DPB;
            if (cap < e->sps.max_dec_frame_buffering)   /* a forced --level too low */
                cap = e->sps.max_dec_frame_buffering;
            e->sps.max_num_ref_frames = cap;
            e->sps.max_dec_frame_buffering = cap;
            e->sps.max_num_reorder_frames = cap;
        }
    }
    /* VUI timing_info: signal the framerate (H.264 E.2.1: frame_rate =
 * time_scale / (2 * num_units_in_tick)). time_scale = 2*fps_num,
 * num_units_in_tick = fps_den. Lets muxers/players use the real fps instead
 * of guessing from an Annex-B elementary stream. */
    if (e->param.timebase.fps_num > 0 && e->param.timebase.fps_den > 0) {
        e->sps.vui_timing = 1;
        e->sps.num_units_in_tick = e->param.timebase.fps_den;
        e->sps.time_scale = 2 * e->param.timebase.fps_num;
    }
    e->sps.chroma_loc = -1;                      /* no colour signalling until set */
    e->sps.sar_num = e->param.sar_num;           /* VUI aspect ratio (0 = square) */
    e->sps.sar_den = e->param.sar_den;
    e->sps.width_in_mbs = e->width_in_mbs;
    e->sps.frame_mbs_only_flag = e->param.fake_interlaced ? 0 : 1;
    e->sps.height_in_map_units = e->sps.frame_mbs_only_flag
                               ? e->height_in_mbs : e->height_in_mbs / 2;
    e->sps.direct_8x8_inference_flag = 1;
    /* frame_cropping is in CropUnit samples (7.4.2.1.1): CropUnitX = SubWidthC,
 * CropUnitY = SubHeightC * (2 - frame_mbs_only_flag). Progressive here, so the
 * units are the chroma subsampling factors sub_w/sub_h -- NOT a hardcoded 2,
 * which mis-cropped 4:2:2 (SubHeightC=1) and 4:4:4 vertically (decoder cropped
 * too little, e.g. 1080 -> 1084, recon-match fail). Byte-identical for 4:2:0. */
    e->sps.crop_right = (e->padded_w - e->width) / e->sub_w;
    /* CropUnitY = SubHeightC x (2 - frame_mbs_only_flag), so the vertical crop
 * counts in DOUBLE units once the sequence may carry fields. */
    e->sps.crop_bottom = (e->padded_h - e->height)
                       / (e->sub_h * (2 - e->sps.frame_mbs_only_flag));

    e->pps.pps_id = 0;
    e->pps.sps_id = e->param.sps_id;
    e->pps.entropy_coding_mode_flag = e->sps.entropy_coding_mode_flag;  /* honors 4:2:2 CAVLC fallback */
    e->pps.deblocking_filter_control_present_flag = 1;
    e->pps.chroma_qp_index_offset = e->param.chroma_qp_index_offset;
    e->pps.weighted_bipred_idc = (has_b && e->param.weightb &&
                                  e->sps.profile_idc != 66) ? 2 : 0;  /* implicit weighted biprediction */
    /* Explicit P-slice weighted prediction: the pred_weight_table codes one
 * luma weight per active list-0 reference (fade estimation runs per ref). */
    /* A.2.1 forbids weighted prediction in Baseline, so --profile baseline is
 * the one profile that turns a tool off rather than only asserting one. */
    e->pps.weighted_pred_flag = e->sps.profile_idc == 66 ? 0 : 1;
    e->pps.transform_8x8_mode_flag = e->param.transform8x8 ? 1 : 0;
    /* Legal in every profile, and it constrains the encoder rather than
 * asserting a tool, so nothing narrows it. */
    e->pps.constrained_intra_pred_flag = e->param.constrained_intra ? 1 : 0;

    /* Zero-as-unset for all three: 0, 51 and 4 are the literals every rate
 * control used before they were parameters, so an unset struct clamps and
 * steps exactly as it always did. */
    e->qp_min  = param->rc.qp_min;
    e->qp_max  = param->rc.qp_max ? param->rc.qp_max : 51;
    e->qp_step = param->rc.qp_step ? (double)param->rc.qp_step : 4.0;

    e->qp = param->rc.qp;
    if (e->qp < 0) e->qp = 0;
    if (e->qp > 51) e->qp = 51;
    e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);

    e->abr_on = (param->rc.method == YAH264_RC_ABR && param->rc.bitrate > 0);
    if (e->abr_on) {
        double fps = (param->timebase.fps_num > 0 && param->timebase.fps_den > 0)
                   ? (double)param->timebase.fps_num / param->timebase.fps_den : 25.0;
        e->abr_target_bpf = (double)param->rc.bitrate * 1000.0 / fps;
        e->abr_qp = 26.0;                           /* initial guess; converges fast */
        /* Seed the complexity->bits scale (x264's complexity-sum seed) so the FIRST frame
 * isn't computed from the meaningless scale=1.0 default -- that gave the
 * IDR a garbage-high QP and starved the reference (measured: IDR 1518 vs
 * x264 13076 bytes). The scale is per rate-compressed complexity
 * rceq=C^(1-qcomp), so all types share the same seed. */
        int nmbc = e->width_in_mbs * e->height_in_mbs;
        double seed = 0.01 * pow(7.0e5, 0.6) * sqrt((double)(nmbc > 0 ? nmbc : 1));
        for (int t = 0; t < 3; t++) { e->abr_scale[t] = seed; e->abr_inited[t] = 1; }
        e->abr_fps = fps;
        /* x264: the same seed serves as rf_cplx_sum, and
 * rf_wanted_bits opens at one frame of target bits, so the first
 * rate factor is exactly the seeded scale over the frame target, so
 * frame 0 decides identically with or without the model. */
        e->rf_cplx_sum = seed;
        e->rf_wanted_bits = e->abr_target_bpf;
        e->ptrack_norm = 0.01;
        e->ptrack_qp = 24.0 * e->ptrack_norm;      /* x264's initial QP */
        e->last_nonb_type = -1;
        e->last_ref_qp[0] = e->last_ref_qp[1] = -1.0;
        for (int t = 0; t < 3; t++)
            e->last_qscale_type[t] = pow(2.0, (24.0 - 12.0) / 6.0);   /* the initial QP */
        e->st_cplxsum = 0.0; e->st_cplxcount = 0.0;
    }
    e->crf_on = (param->rc.method == YAH264_RC_CRF && param->rc.rf > 0);
    if (e->crf_on) {
        e->crf = param->rc.rf;
        e->crf_qcomp = 0.6;                         /* x264 default qcompress */
        const char *cl = getenv("Y264_CRF_CL");
        e->crf_cl = cl ? atoi(cl) : 1;              /* mb-tree operating-point devices (default on) */
    }
    e->vbv_on = (param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0);
    if (e->vbv_on) {
        double fps = (param->timebase.fps_num > 0 && param->timebase.fps_den > 0)
                   ? (double)param->timebase.fps_num / param->timebase.fps_den : 25.0;
        e->vbv_rate = (double)param->rc.vbv_maxrate * 1000.0 / fps;
        e->vbv_size = (double)param->rc.vbv_bufsize * 1000.0;
        /* Start full unless rc.vbv_init names another occupancy: <= 1 is a
 * fraction of the buffer, above 1 is kbit. 0 = unset = full, the
 * behaviour every stream had before the parameter existed. */
        e->vbv_fill = e->vbv_size;
        if (param->rc.vbv_init > 0) {
            double init = param->rc.vbv_init <= 1.0
                        ? param->rc.vbv_init * e->vbv_size
                        : param->rc.vbv_init * 1000.0;
            if (init > e->vbv_size) init = e->vbv_size;
            e->vbv_fill = init;
        }
        /* The handoff level is half the buffer and not a tunable, because it is
 * already this loop's fixed point: vbv_fill_budget solves for a frame
 * that lands the occupancy back on vbv_size/2 and allows a climb when it
 * is below, so a segment that starts there is a segment starting where
 * its own rate loop returns to. It exits there without being told to,
 * which is why composability needs no machinery beyond this line.
 *
 * An explicit exit constraint is deliberately absent: it was built, measured,
 * and removed on the measurement. */
        e->vbv_seg_h = 0.5 * e->vbv_size;
        /* Only where the buffer is the sole controller. With a bitrate target
 * the integrator owns the rate, and 2-pass plans against real pass-1
 * bits; both were already clean across GOP joins and both stay
 * bit-for-bit what they were. */
        if (param->rc.vbv_seg_join && !e->abr_on && param->rc.method != YAH264_RC_2PASS)
            e->vbv_fill = e->vbv_seg_h;             /* assume only the handoff */
        /* The first coded frame of ANY instance escapes vbv_clip_qp -- the bits
 * model needs one coded frame before a prediction exists. Under a
 * bitrate target the seeded abr_scale covers it; under CRF/CQP nothing
 * does, so the frame gets bounded against its own measured size
 * instead. Not for 2-pass: pass 2 has real bits for frame 0. */
        e->vbv_first_bound = !e->abr_on && param->rc.method != YAH264_RC_2PASS;
        /* Same mode envelope as the first-frame bound, plus the opt-in. */
        e->vbv_bound_on = e->vbv_first_bound && vbv_bound_env();
        /* CBR only: with maxrate == target the buffer fills exactly as fast
 * as the target spends, and a near-overflow buffer must be spent down or
 * the stream pads. Capped VBR (maxrate > target) fills at MAXRATE, so
 * "spend the spare" chases maxrate instead of the target -- measured
 * +30..40% achieved over a 400-target/600-max pair on two clips
 * (regress.py's first harvest, 2026-08-29) -- and overflow there is
 * harmless: vbv_update's clamp is the whole story. x264 draws the line
 * the same way. */
        e->vbv_cbr = e->abr_on && param->rc.vbv_maxrate <= param->rc.bitrate;
    }
    if (param->rc.method == YAH264_RC_2PASS && param->rc.stats) {
        double fps = (param->timebase.fps_num > 0 && param->timebase.fps_den > 0)
                   ? (double)param->timebase.fps_num / param->timebase.fps_den : 25.0;
        e->crf_qcomp = Y264_TP_QCOMP;
        /* pass 3 is pass 2's read plus pass 1's write: the plan is solved from
 * the records that are there, and the coded result is written back over
 * them, so a further pass refines against a real encode instead of the
 * fixed-QP pass 1. Same path, REWRITTEN -- every record is read at open,
 * before the file is truncated. */
        e->tp_pass = param->rc.pass == 2 ? 2 : param->rc.pass == 3 ? 3 : 1;
        if (e->tp_pass == 1) {
            e->tp_fp = fopen(param->rc.stats, "w");
            if (!e->tp_fp) {                        /* the writers use it bare */
                fprintf(stderr, "yah264: cannot open pass-1 stats file '%s' for writing\n",
                        param->rc.stats ? param->rc.stats : "(null)");
                yah264_encoder_close(e);
                return NULL;
            }
            e->qp = 26;                             /* fixed reference QP for pass 1 */
            e->chroma_qp = y264_chroma_qp(26, e->param.chroma_qp_index_offset);
        } else {
            FILE *fp = fopen(param->rc.stats, "r");
            if (fp) {
                int cap = 256;
                e->tp_stats = malloc((size_t)cap * sizeof(*e->tp_stats));
                struct tp_stat s;
                char ln[256];
                /* '#' lines are the GOP-boundary markers a threaded pass 1
 * writes (cli/yah264_cli.c). Skipping them here is what lets
 * ONE stats file feed both readers: this one, which solves the
 * whole stream at once, and the GOP-parallel splitter, which
 * needs to know where each GOP's records start. A file with no
 * markers reads exactly as it always did. */
                while (fgets(ln, sizeof ln, fp)) {
                    if (ln[0] == '#' || ln[0] == '\n' || ln[0] == '\0')
                        continue;
                    /* The 5th field (is_ref) was added for the offline plan's
 * pyramid ref-B handling. Records written before it read as
 * 4 fields and default to 0, the flat-B assumption, so an
 * old stats file still plans. */
                    s.is_ref = 0;
                    int nf = sscanf(ln, "%d %lf %lf %d %d",
                                    &s.type, &s.cplx, &s.bits, &s.qp, &s.is_ref);
                    if (nf < 4)
                        break;                      /* malformed: stop, as before */
                    if (e->tp_n == cap) {
                        cap *= 2;
                        e->tp_stats = realloc(e->tp_stats, (size_t)cap * sizeof(*e->tp_stats));
                    }
                    e->tp_stats[e->tp_n++] = s;
                    /* Complexity is the QP-invariant coding cost (bits at pass-1 QP,
 * scaled to qscale 1), not the raw SATD: a well-predicted frame
 * is cheap even when its SATD is high. */
                    double cost = (s.bits + 1) * pow(2.0, (s.qp - 12) / 6.0);
                    e->tp_sum_cq += pow(cost, e->crf_qcomp);
                }
                fclose(fp);
            }
            if (e->tp_pass == 3) {
                /* Every record is in e->tp_stats by now, so truncating the file
 * just read is safe -- and it has to be the same path, because the
 * CLI hands each GOP worker its own slice file and merges those
 * back in GOP order exactly as it does for pass 1. */
                e->tp_fp = fopen(param->rc.stats, "w");
                if (!e->tp_fp) {
                    fprintf(stderr, "yah264: cannot open pass-3 stats file '%s' for writing\n",
                            param->rc.stats);
                    yah264_encoder_close(e);
                    return NULL;
                }
            }
            double bitrate = param->rc.bitrate > 0 ? param->rc.bitrate : 1000;
            e->tp_target = param->rc.tp_target_bits > 0.0
                         ? param->rc.tp_target_bits
                         : bitrate * 1000.0 * e->tp_n / fps;
            e->tp_rem_target = e->tp_target;
            e->tp_rem_cq = e->tp_sum_cq;
            e->tp_fps = fps;
            /* Every plan gate is resolved HERE, on the API thread at open, into
 * encoder fields -- not into function-local statics. The decide
 * that reads them runs on the stair driver under the pipeline, and
 * a lazily-initialised static read from there is exactly the race
 * class warm_lr_statics exists to clean up. Fields cost nothing and
 * cannot race. */
            const char *ev;
            /* DEFAULT ON. With it off, 2-pass is measurably WORSE than the
             * 1-pass it exists to beat: the fallback allocator ranks frames by
             * how CHEAP they are, so the I frame gets the highest QP in its GOP
             * and the B frames the lowest. On foreman at 400 kbps that reads
             * I/P/B 5219/1935/1547 bytes against 14202/3933/772 with the plan,
             * i.e. the anchor everything predicts from starved at 2.7x a B
             * frame instead of 18x. With the plan on, BD-VMAF-NEG vs 1-pass ABR
             * at matched rate WINS on all seven corpus clips (-1.61% to
             * -35.66%, against +10% to +55% worse without), rate accuracy
             * unchanged within 0.8%. Y264_TP_PLAN=0 selects the fallback. */
            e->tp_plan_on  = (ev = getenv("Y264_TP_PLAN"))     ? atoi(ev) != 0 : 1;
            e->tp_difflim  = (ev = getenv("Y264_TP_DIFFLIM"))  ? atoi(ev) != 0 : e->tp_plan_on;
            e->tp_corr     = (ev = getenv("Y264_TP_CORR"))     ? atoi(ev) != 0 : e->tp_plan_on;
            e->tp_bexp     = (ev = getenv("Y264_TP_BEXP"))     ? atof(ev) / 100.0 : 1.0;
            /* x264's complexity-blur default (its --cplxblur). Measured flat from 10 to 40 on
             * foreman/samsung, so this is x264's constant rather than a fitted
             * one -- and it is not a small term: it is worth -3 BD points on
             * foreman and -8 on samsung, and it is what keeps the rate inside
             * the mode's accuracy. */
            e->tp_cplxblur = (ev = getenv("Y264_TP_CPLXBLUR")) ? atof(ev)
                           : (e->tp_plan_on ? 20.0 : 0.0);
            e->tp_qblur    = (ev = getenv("Y264_TP_QBLUR"))    ? atof(ev) : 0.0;
            e->tp_cwarm    = (ev = getenv("Y264_TP_CWARM"))    ? atoi(ev) : 4;
            e->tp_resolve  = (ev = getenv("Y264_TP_RESOLVE"))  ? atoi(ev) != 0 : e->tp_plan_on;
            e->tp_dbg      = getenv("Y264_TP_DBG") != NULL;
            e->tp_ipf      = (ev = getenv("Y264_TP_IPF"))      ? atof(ev) / 100.0 : 1.4;
            e->tp_pbf      = (ev = getenv("Y264_TP_PBF"))      ? atof(ev) / 100.0 : 1.3;
            if (e->tp_bexp < 0.5) e->tp_bexp = 1.0;
            if (e->tp_ipf < 1.0) e->tp_ipf = 1.4;
            if (e->tp_pbf < 1.0) e->tp_pbf = 1.3;
            if (e->tp_plan_on)
                tp_build_plan(e);
        }
    }
    /* Deterministic fixed-lag RC feedback: lets ABR/2-pass ride the frame
     * pipeline. VBV rides too behind its own sub-gate (Y264_RC_PIPE_VBV): the
     * conservative virtual buffer + per-burst serial fallback, covering
     * ABR/CRF/CQP/2-pass with a VBV constraint. Sub-gate off keeps VBV fully
     * serial. */
    /* Resolved here, not in the per-frame path: rcp_account runs on the stair
 * driver under the pipeline. */
    e->tp_rctrace = getenv("Y264_RC_TRACE") != NULL;
    /* Opt-in ABR model: the public parameter, with the env var as an override
     * for A/B runs. */
    e->abr_rf = abr_rf_env() >= 0 ? abr_rf_env() : (param->rc.abr_model ? 1 : 0);
    e->rcp_on = rc_pipe_env() && (e->abr_on || e->tp_pass || e->vbv_on)
              && (!e->vbv_on || rcp_vbv_env());
    e->rcp_vbv_tight = 1;               /* serial until the first burst gate */
    if (e->rcp_on && e->abr_on) {
        /* The rcp P/B decision complexity is the ME-compensated LOWRES cost
 * (never recon), a different domain from the full-res SATD the shared
 * seed above is calibrated for -- warm-up on foreman measures
 * +36% over the first 25 frames off the 2.8x-low P seed. Re-seed P/B
 * with the corpus geometric mean of the CONVERGED Cme-domain scales
 * (foreman/mobile/akiyo/park_joy/samsung, ~90 and ~13 per sqrt(mbs));
 * I keeps the shared seed (its intra-SATD domain is the same). */
        int nmbc = e->width_in_mbs * e->height_in_mbs;
        e->abr_scale[1] = 90.0 * sqrt((double)(nmbc > 0 ? nmbc : 1));
        e->abr_scale[2] = 13.0 * sqrt((double)(nmbc > 0 ? nmbc : 1));
    }

    e->pstride[0] = e->padded_w + 2 * Y264_LUMA_BORDER + plane_pad();
    e->pstride[1] = e->padded_w / e->sub_w + 2 * Y264_CHROMA_BORDER + plane_pad();
    e->pstride[2] = e->pstride[1];
    /* Full bordered buffer sizes: plane copies move whole buffers so the
 * borders travel with the pixels (extended once per stored recon). */
    size_t luma = (size_t)e->pstride[0] * (e->padded_h + 2 * Y264_LUMA_BORDER);
    size_t chroma = (size_t)e->pstride[1] * (e->padded_h / e->sub_h + 2 * Y264_CHROMA_BORDER);
    e->plane[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
    e->plane[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    e->plane[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    e->rec[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
    e->rec[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    e->rec[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    e->ref[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
    e->ref[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    e->ref[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    /* Multi-ref ring: slot 0 aliases ref[]; slots 1..nref-1 get their own buffers. */
    e->refring[0][0] = e->ref[0];
    e->refring[0][1] = e->ref[1];
    e->refring[0][2] = e->ref[2];
    for (int i = 1; i < e->nref; i++) {
        e->refring[i][0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
        e->refring[i][1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
        e->refring[i][2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    }
    e->nref_valid = 0;
    /* A2: half-pel plane pool. One H/V/C triple per list-0 reference plus one for
 * the list-1 anchor (B slices). Disabled with Y264_HPEL=0 (falls back to
 * on-the-fly interpolation, bit-identical). */
    e->hpel_on = 1;
    { const char *v = getenv("Y264_HPEL"); if (v) e->hpel_on = atoi(v); }
    if (e->hpel_on) {
        /* hpel_buf is the FALLBACK triple set: build_slice_prep uses it only for
 * a reference whose half-pel planes are neither DPB-cached nor carried by
 * a flat-path buffer, which on the pyramid path is nothing at all and on
 * the flat path is the first slice after an IDR. Allocated on first use
 * (hpel_buf_take), not up front: one triple per reference up front is
 * 12.6 MB of never-written pages at 720p/--ref 3, where on-demand costs
 * nothing when it is dead and one malloc when it is not. */
        int sstride = e->padded_w + 2 * Y264_LUMA_BORDER;
        size_t srows = (size_t)e->padded_h + 2 * Y264_LUMA_BORDER + 5;
        e->hpel_scratch = malloc((size_t)sstride * srows * sizeof(int32_t));
        if (!e->hpel_scratch) e->hpel_on = 0;        /* fall back on OOM */
    }
    /* Flat-path owned hpel triples: one per rotating buffer (rec + ref1 + ring),
 * so a stored recon's half-pel build travels with it instead of being redone
 * per referencing slice. Pyramid mode uses the DPB's cached hpel instead. */
    e->flat_hp_on = e->hpel_on && !e->b_pyramid;
    if (e->flat_hp_on) {
        for (int c = 0; c < 3; c++) {
            e->rec_hp[c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
            e->ref1_hp[c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
        }
        for (int i = 0; i < e->nref; i++)
            for (int c = 0; c < 3; c++)
                e->ring_hp[i][c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
    }
    e->lr_w = e->padded_w / 2;
    e->lr_h = e->padded_h / 2;
    e->lowres_cur = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
    e->lowres_prev = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
    {
        size_t nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
        e->lr_intra = malloc(nmb * sizeof(int));
        e->lr_inter = malloc(nmb * sizeof(int));
        e->lr_mvx = malloc(nmb * sizeof(int16_t));
        e->lr_mvy = malloc(nmb * sizeof(int16_t));
        /* Lookahead window for mb-tree chain propagation, IPPP (bframes 0)
         * included. IPPP needs the lambda*MV-rate propfrac damper and the
         * widened offset bound to converge on the long P chain: without them
         * propfrac -> 1 on tracked motion (a pure-SATD blk8_inter) saturates
         * the +/-8 clamp. Y264_MBTREE_IPPP=0 disables it for IPPP. */
        int mbtree_ipppp = 1;
        { const char *v = getenv("Y264_MBTREE_IPPP"); if (v) mbtree_ipppp = atoi(v); }
        e->la_depth = la_depth_for(param);       /* the rule, shared with the
 * public delay query */
        /* The decoupled lookahead's lead: extra
 * ring capacity ahead of the la_depth window, clamped to fit
 * Y264_LA_CAP_MAX. Off (0 buf, la_cap == la_depth) when the window
 * itself is off, when the caller asked for low latency, or when there
 * is no pool to run a chain against -- see la_lead_for. Costs exactly
 * la_buf frames of latency and no bits. */
        e->la_buf = la_lead_for(param, e->la_depth);
        if (e->la_buf < 0) e->la_buf = 0;
        if (e->la_buf > Y264_LA_CAP_MAX - e->la_depth)
            e->la_buf = Y264_LA_CAP_MAX - e->la_depth;
        e->la_cap = e->la_depth + e->la_buf;
        e->mbtree_on = (e->param.bframes > 0) || (mbtree_ipppp && e->la_depth > 0);
        e->abr_rf2 = 0;
        if (e->abr_on && abr_rf2_env()) {
            if (!e->mbtree_on) {
                fprintf(stderr, "yah264: Y264_ABR_RF2 needs mb-tree (bframes > 0); using the default model\n");
            } else {
                e->abr_rf2 = 1;
                double fps = e->abr_fps > 0 ? e->abr_fps : 25.0;
                double dur = 1.0 / fps;
                if (dur < 0.01) dur = 0.01;
                if (dur > 1.00) dur = 1.00;
                e->rf2_rceq = pow(0.04 / dur, 1.0 - abr_qcomp_env());
                int nmb = e->width_in_mbs * e->height_in_mbs;
                e->rf_cplx_sum = 0.01 * 7.0e5 * sqrt((double)(nmb > 0 ? nmb : 1));
                e->rf_wanted_bits = e->abr_target_bpf;
            }
        }
        /* x264 forces mb-tree AND AQ off at constant QP, and so do we:
         * otherwise --qp N is not constant QP at all but mb-tree- and
         * AQ-modulated QP, which is neither what the flag says nor what x264
         * produces. The band deficit is localised to the adaptive-QP subsystem
         * rather than the base coder, so a CQP user would be paying for the
         * bad offset field with no way to ask for what they wanted. */

        /* Y264_MBTREE_OFF=1: measurement probe -- x264's own CQP policy
 * (x264's parameter validation forces mb-tree and aq off
 * at X264_RC_CQP) applied here, at every RC mode. Skips the per-anchor
 * compute_mbtree walk and the per-MB offset apply; the chain (legs,
 * typing, seeds) and the mbtree_on allocations stay, because the
 * lookahead ME reads lr_subpel whatever mbtree does. Changes bits
 * wherever mbtree ran; exists to price the driver-side mbtree wall
 * share against x264's CQP shape. DEFAULT OFF. */
        { const char *v = getenv("Y264_MBTREE_OFF");
          e->mbtree_skip = v && atoi(v); }
        /* CQP takes x264's policy via mbtree_skip, NOT mbtree_on: clearing
         * mbtree_on frees lowres_tmp, which the lookahead ME dereferences
         * whatever mb-tree does (see the comment just above, and
         * build_lr_subpel). */
        if (e->param.rc.method == YAH264_RC_CQP) e->mbtree_skip = 1;
        if (e->mbtree_on) {
            e->mbtree_off = malloc(nmb);
            e->lowres_tmp = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
            e->code_panchor_lr = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
            e->code_panchor_have = 0;
            for (int g = 0; g < 2; g++)
                for (int p = 1; p < 16; p++)
                    e->lr_subpel[g][p] = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
        }
        if (e->la_depth > 0) {
            e->lr_seed_mvx = malloc(nmb * sizeof(int16_t));
            e->lr_seed_mvy = malloc(nmb * sizeof(int16_t));
            e->lr_seed_cost = malloc(nmb * sizeof(int32_t));
            size_t lrsz = (size_t)e->lr_w * e->lr_h;
            for (int i = 0; i < e->la_cap; i++) {
                e->la[i].plane[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
                e->la[i].plane[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
                e->la[i].plane[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
                e->la[i].lowres = malloc(lrsz * sizeof(pixel));
                e->la[i].d_intra = malloc(nmb * sizeof(int32_t));
                for (int g = 0; g < LR_NLEGS; g++)
                    e->la[i].leg[g] = malloc(nmb * sizeof(y264_lr_blk));
            }
            e->la_lr_prev = malloc(lrsz * sizeof(pixel));
            e->la_anchor_lr = malloc(lrsz * sizeof(pixel));
            e->la_anchor_mvx = malloc(nmb * sizeof(int16_t));
            e->la_anchor_mvy = malloc(nmb * sizeof(int16_t));
            e->la_anchor_mv_have = 0;
            e->la_anchor_poc = 0;
            e->cur_bseed = -1;
            e->bseed_pend_valid = 0;
            if (e->param.bframes > 0) {     /* B lowres pair-seed pipeline */
                for (int k = 0; k < 4; k++) {
                    e->bseed_pend[k] = malloc(nmb * sizeof(int16_t));
                    e->bseed_cur[k] = malloc(nmb * sizeof(int16_t));
                    for (int b = 0; b < 8; b++)
                        e->bseed[b][k] = malloc(nmb * sizeof(int16_t));
                }
                for (int b = 0; b < 8; b++) e->bseed_valid[b] = 0;
                if (getenv("Y264_BLATE_STAT"))  /* measurement-only cost bank */
                    for (int k = 0; k < 3; k++) {
                        e->bseedc_pend[k] = malloc(nmb * sizeof(int32_t));
                        for (int b = 0; b < 8; b++)
                            e->bseedc[b][k] = malloc(nmb * sizeof(int32_t));
                    }
            }
            e->badapt_on = param->badapt && e->param.bframes > 0 && e->la_depth >= 2;
            e->la_prop_a = malloc(nmb * sizeof(double));
            e->la_prop_b = malloc(nmb * sizeof(double));
        }
    }

    e->bframes = e->param.bframes;
    if (e->bframes < 0) e->bframes = 0;
    if (e->bframes > 7) e->bframes = 7;
    e->nbuf = 0;
    e->ref1[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);                      /* always allocated: uniform */
    e->ref1[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);                    /* reorder logic uses it even */
    e->ref1[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);                    /* with 0 B frames */
    for (int i = 0; i < e->bframes && i < 8; i++) {
        e->blowres[i] = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
        e->bmbtree_off[i] = malloc((size_t)e->width_in_mbs * e->height_in_mbs);
        e->bmbtree_valid[i] = 0;
    }
    for (int i = 0; i < e->bframes; i++) {
        e->bplane[i][0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
        e->bplane[i][1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
        e->bplane[i][2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    }

    /* The width-engagement inputs, resolved before dpbp_open (the
 * first thing that asks whether this instance can run wide) and therefore
 * before the pool itself is created a few hundred lines below.
 *
 * e->rcp_lag is Y264_RCP_LAG's value where width can
 * engage, 0 where it cannot. Both terms are static configuration, so this
 * is a per-open constant -- a decide never asks whether anything is
 * currently in flight, only what this configuration was opened with. VBV is
 * refused inside stair_wide_capable and again in stair_wide_rc_ok. */
    e->wf_width = wf_width_for(param);
    e->rcp_lag = (rcp_lag_nowide_on() ? stair_lag_capable(e)
                                      : stair_wide_capable(e))
                 ? rcp_lag_for(e) : 0;
    if (g_tprof_on > 0) {
        g_cfg[0] = e->param.threads; g_cfg[1] = e->wf_width; g_cfg[2] = e->la_depth; g_cfg[3] = e->la_buf;
        g_cfg[4] = e->param.sync_lookahead; g_cfg[5] = e->rcp_lag; g_cfg[6] = e->abr_early; g_cfg[7] = e->param.bframes;
        g_cfg[8] = e->param.keyint; g_cfg[9] = e->param.rc.lookahead; g_cfg[10] = e->param.timebase.fps_num; g_cfg[11] = e->param.timebase.fps_den;
    }
    e->abr_early = abr_early_env();             /* probe; see abr_early_env */

    e->mv_stride = e->width_in_mbs * 4;
    size_t mvcount = (size_t)e->mv_stride * e->height_in_mbs * 4;
    e->mvx = malloc(mvcount * sizeof(int16_t));
    e->mvy = malloc(mvcount * sizeof(int16_t));
    e->refidx = malloc(mvcount);
    e->mvx1 = malloc(mvcount * sizeof(int16_t));
    e->mvy1 = malloc(mvcount * sizeof(int16_t));
    e->refidx1 = malloc(mvcount);
    e->mvdx = malloc(mvcount * sizeof(int16_t));
    e->mvdy = malloc(mvcount * sizeof(int16_t));
    e->mvdx1 = malloc(mvcount * sizeof(int16_t));
    e->mvdy1 = malloc(mvcount * sizeof(int16_t));
    e->colmvx = malloc(mvcount * sizeof(int16_t));
    e->colmvy = malloc(mvcount * sizeof(int16_t));
    e->colref = malloc(mvcount);
    e->colpoc = malloc(mvcount * sizeof(int16_t));

    /* DPB slots for b-pyramid: recon planes + list-0 motion per reference. */
    if (e->b_pyramid) {
        e->dpb_size = e->sps.max_num_ref_frames + 1;
        if (e->dpb_size > 16) e->dpb_size = 16;
        for (int i = 0; i < e->dpb_size; i++) {
            e->dpb[i].plane[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
            e->dpb[i].plane[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
            e->dpb[i].plane[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
            e->dpb[i].mvx = malloc(mvcount * sizeof(int16_t));
            e->dpb[i].mvy = malloc(mvcount * sizeof(int16_t));
            e->dpb[i].refidx = malloc(mvcount);
            e->dpb[i].colpoc = malloc(mvcount * sizeof(int16_t));
            e->dpb[i].used = 0;
            e->dpb[i].hpel_valid = 0;
            if (e->hpel_on)
                for (int c = 0; c < 3; c++)
                    e->dpb[i].hpel[c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
            else
                for (int c = 0; c < 3; c++) e->dpb[i].hpel[c] = NULL;
        }
        dpbp_open(e, mvcount);
    }

    /* nnz grids: luma (wmb*4)x(hmb*4), chroma (wmb*cbw)x(hmb*cbh) where cbw/cbh
 * are the chroma 4x4-block counts per MB per axis (4/SubWidthC, 4/SubHeightC:
 * 2x2 for 4:2:0, 2x4 for 4:2:2, 4x4 for 4:4:4). */
    int cbw = 4 / e->sub_w, cbh = 4 / e->sub_h;
    e->nnz_stride[0] = e->width_in_mbs * 4;
    e->nnz_stride[1] = e->nnz_stride[2] = e->width_in_mbs * cbw;
    e->nnz[0] = malloc((size_t)e->nnz_stride[0] * e->height_in_mbs * 4);
    e->nnz[1] = malloc((size_t)e->nnz_stride[1] * e->height_in_mbs * cbh);
    e->nnz[2] = malloc((size_t)e->nnz_stride[2] * e->height_in_mbs * cbh);
    e->i4mode_stride = e->width_in_mbs * 4;
    e->i4mode = malloc((size_t)e->i4mode_stride * e->height_in_mbs * 4);
    e->mbcbp = malloc((size_t)e->width_in_mbs * e->height_in_mbs * sizeof(int));

    e->aq_strength = param->aq_strength;
    if (param->rc.method == YAH264_RC_CQP) e->aq_strength = 0.f;   /* x264*/
    /* Resolved here, not in the AQ kernels: they run on the lookahead/mb-tree
 * pool and an unwarmed lazy static there is a race. */
    /* SCOPED TO CRF. The absolute anchor reaches the per-MB AQ field in EVERY
 * rate mode, unlike the two base-QP terms beside it, which carry their own
 * `e->crf_cl` gate -- so arming Y264_CRF_CPLX unscoped moves ABR's output too.
 * ABR is not measured for it, and its band's per-clip noise floor spans 0.2 to
 * 11 points, so measuring it is a round of its own. The -5.17% median is a CRF
 * number. Y264_CRF_AQABS=1 arms it everywhere for that measurement. */
    /* The x264 mode arms it in every rate mode, because x264's aq-mode 1 is
 * absolute in every rate mode -- the CRF scoping above is a statement about
 * what we have MEASURED, not about x264. The mode's own gate is still the
 * CRF band. */
    e->aq_abs    = crf_aqabs_env()
                && (e->crf_on || crf_aqabs_forced() || y264_mbt_derived());
    e->aq_chroma = aq_chroma_env();
    e->aq_anchor = aq_anchor_env();
    /* The AQ frame-mean (DC) term at x264's strength (1.0 x 1.0397) on the
 * CRF base QP: see crf_frame_dc. Y264_AQ_DC=<aq-strength> (0.4) reproduces
 * the pre-2026-09-05 output byte for byte. */
    { const char *v = getenv("Y264_AQ_DC"); e->aq_dcstr = (v ? atof(v) : 1.0) * 1.0397; }
    e->crf_dc = 0.0;
    size_t nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
    if (e->aq_strength > 0.f)
        e->aq_off = malloc(nmb);
    /* Per-MB coded QP for the deblock: needed whenever the QP varies per MB,
 * i.e. AQ or mb-tree. */
    if (e->aq_strength > 0.f || e->mbtree_on)
        e->mbqp = malloc(nmb);
    if (e->param.transform8x8)
        e->mb_tr8 = malloc(nmb);

    size_t frame_bytes = luma + 2 * chroma;
    /* Worst-case CAVLC expansion happens at QP 0 on incompressible content: a
 * few KB per macroblock. Size for that so a valid 8-bit frame never
 * overflows; the overflow guard in build_idr_rbsp stays as a backstop. */
    size_t num_mbs = (size_t)e->width_in_mbs * e->height_in_mbs;
    e->rbsp_cap = frame_bytes + num_mbs * 3072 + 8192;
    { /* Y264_RBSP_CAP=<bytes>: probe override of the slice buffer (exercises
 * the CABAC/CAVLC overflow backstops; a too-small buffer voids slices). */
      const char *rc = getenv("Y264_RBSP_CAP"); long v = rc ? atol(rc) : 0;
      if (v > 4096) e->rbsp_cap = (size_t)v; }
    e->rbsp = malloc(e->rbsp_cap);
    /* Annex-B worst case: start code + header + rbsp + one emu byte per 2 bytes. */
    e->out_cap = e->rbsp_cap + e->rbsp_cap / 2 + 64;
    e->out = malloc(e->out_cap);

    if (!e->plane[0] || !e->plane[1] || !e->plane[2] ||
        !e->rec[0] || !e->rec[1] || !e->rec[2] ||
        !e->ref[0] || !e->ref[1] || !e->ref[2] ||
        !e->mvx || !e->mvy || !e->refidx ||
        !e->nnz[0] || !e->nnz[1] || !e->nnz[2] || !e->i4mode ||
        !e->rbsp || !e->out) {
        yah264_encoder_close(e);
        return NULL;
    }

    /* W1: in-frame row-wavefront pool from param.frame_threads (Y264_WF_THREADS
 * env overrides, for testing). >1 => pass-1 analysis runs on the wavefront.
 *
 * Capped at the grid's critical-path knee, not at the row count: rows was
 * always the wrong bound (a 22-column row cannot pay for 18 claims however
 * many rows there are), and the knee is never above the row count anyway.
 * Worker count cannot reach the bitstream -- the W1 determinism guarantee --
 * so this is byte-identical by construction. Y264_WF_THREADS deliberately
 * bypasses the cap: probing above the knee is what it is for. */
    {
        if (e->wf_width > 1) e->pool = ntp_pool_create(e->wf_width);
        /* Opened once per encoder; NULL unless armed AND Metal is present. The
 * chunk size is the library's own, so the leg count passed here only
 * bounds the buffers it keeps. */
        e->gpu = y264_gpu_open(e->lr_w, e->lr_h, 48, 96);
        e->gpu_warm = e->gpu ? y264_gpu_open(e->lr_w, e->lr_h, 48, 96) : NULL;
        /* gpq (Y264_GPU_PHASEA): per-push Phase-A legs. Needs the lookahead
 * ring (the chain is the submitter and push order the key). */
        e->gpq = e->la_cap > 0 && e->la_depth > 0
               ? y264_gpq_open(e->lr_w, e->lr_h, e->la_cap, e->bframes + 1) : NULL;
        if (e->pool && !g_tprof_pool) g_tprof_pool = e->pool;   /* prof only */
        /* A pool that failed to come up must not leave a lag budget standing:
 * the budget is granted on the promise of width, and stair_wide_rc_ok
 * would otherwise admit a wide burst with no pool behind it (session
 * 8's own failure, from the other direction). Only reachable on
 * thread-create failure. */
        if (!e->pool || ntp_pool_nthreads(e->pool) < Y264_MT_POOL_MIN)
            e->rcp_lag = 0;
    }

    /* MT stage 3 (thread-scaled clamp): resolve the staircase's row-gate
 * margin now that the pool's real width is known.
 *
 * NOT engaged below Y264_MT_POOL_MIN -- and this is a correctness
 * requirement, not an optimisation. stair_clamp_on (below) applies the
 * clamp AT ANY THREAD COUNT INCLUDING 1: that is what makes a
 * Y264_STAIR=1 (default-on) encode's bitstream identical whether the
 * staircase's wavefront/pool machinery actually runs or falls back to the
 * serial path (stair_row_gate's blocking form is exactly what the serial
 * analyze uses). So --threads 1, and every --threads below the pool
 * engagement floor, must keep computing the FIXED floor value, or
 * w2_canary's default-path byte-identity breaks. Only a pool that has
 * actually reached Y264_MT_POOL_MIN -- the same bar stair_ready and every
 * other stage-3 gate uses -- feeds the real formula; that is
 * exactly the set of configurations whose bits are allowed to vary with
 * --threads. Y264_STAIR_LAG_FORCE is a debug/
 * measurement hook only (like Y264_STAIR_STAT): it overrides the computed
 * value directly, clamped to the same floor, for probing shapes this
 * build can't easily synthesize (e.g. a tall frame at a narrow pool)
 * without a real encode of that size. */
    {
        int pt = e->pool ? ntp_pool_nthreads(e->pool) : 0;
        e->stair_lag = (pt >= Y264_MT_POOL_MIN) ? stair_lag_for(e->height_in_mbs, pt)
                                                 : Y264_STAIR_LAG;
        const char *fl = getenv("Y264_STAIR_LAG_FORCE");
        if (fl) {
            int v = atoi(fl);
            if (v < Y264_STAIR_LAG) v = Y264_STAIR_LAG;
            e->stair_lag = v;
        }
        e->stair_mvy_max = 4 * (16 * e->stair_lag - 24);
    }

    /* W2 emit-overlap: frame N's entropy emit runs on a background thread while
 * frame N+1 analyses. Output is byte-identical (only the timing overlaps).
 *
 * DEFAULT ON whenever this encoder was given in-frame threads. Entropy emit
 * is serial per frame, so once the wavefront parallelises analyze it becomes
 * the dominant serial stage -- 22-24% of single-GOP wall at 8-12 threads on
 * 720p CABAC -- and overlapping it is worth ~1.22-1.25x there, ~1.05-1.11x
 * multi-GOP, and nothing anywhere is slower. Measure it against a SLOW
 * analyze side and it reads ~3%: emit's share of the frame is what makes it
 * pay.
 *
 * Left OFF at --threads 1 so that stays a genuinely single-threaded
 * reference: it would gain ~5% there, but a user asking for one thread
 * should get one thread. Y264_W2=1 forces it on anyway, Y264_W2=0 off.
 * On OOM or thread-create failure, fall back to the serial path silently. */
    {
        const char *w2 = getenv("Y264_W2");
        if (w2 ? atoi(w2) != 0 : e->pool != NULL) {
            size_t mvcount = (size_t)e->mv_stride * e->height_in_mbs * 4;
            size_t nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
            int cbw = 4 / e->sub_w, cbh = 4 / e->sub_h;
            size_t nnz0 = (size_t)e->nnz_stride[0] * e->height_in_mbs * 4;
            size_t nnzc = (size_t)e->nnz_stride[1] * e->height_in_mbs * cbh;
            size_t i4sz = (size_t)e->i4mode_stride * e->height_in_mbs * 4;
            (void)cbw;
            int ok = 1;
            for (int g = 0; g < 2; g++) {
                struct w2_gen *G = &e->gen[g];
                G->nnz[0] = malloc(nnz0); G->nnz[1] = malloc(nnzc); G->nnz[2] = malloc(nnzc);
                G->i4mode = malloc(i4sz);
                G->mbcbp = malloc(nmb * sizeof(int));
                G->mvx = malloc(mvcount * sizeof(int16_t));
                G->mvy = malloc(mvcount * sizeof(int16_t));
                G->mvx1 = malloc(mvcount * sizeof(int16_t));
                G->mvy1 = malloc(mvcount * sizeof(int16_t));
                G->refidx = malloc(mvcount);
                G->refidx1 = malloc(mvcount);
                G->mvdx = malloc(mvcount * sizeof(int16_t));
                G->mvdy = malloc(mvcount * sizeof(int16_t));
                G->mvdx1 = malloc(mvcount * sizeof(int16_t));
                G->mvdy1 = malloc(mvcount * sizeof(int16_t));
                G->mb_tr8 = e->mb_tr8 ? malloc(nmb) : NULL;
                G->aq_off = e->aq_off ? malloc(nmb) : NULL;
                G->mbtree_off = e->mbtree_off ? malloc(nmb) : NULL;
                G->rbsp = malloc(e->rbsp_cap);
                if (!G->nnz[0] || !G->nnz[1] || !G->nnz[2] || !G->i4mode || !G->mbcbp ||
                    !G->mvx || !G->mvy || !G->mvx1 || !G->mvy1 || !G->refidx || !G->refidx1 ||
                    !G->mvdx || !G->mvdy || !G->mvdx1 || !G->mvdy1 || !G->rbsp ||
                    (e->mb_tr8 && !G->mb_tr8) || (e->aq_off && !G->aq_off) ||
                    (e->mbtree_off && !G->mbtree_off))
                    ok = 0;
            }
            e->bg = ok ? ntp_bg_create_named("y264-w2emit") : NULL;
            e->w2_on = (ok && e->bg) ? 1 : 0;
        }
    }

    /* Decoupled lookahead thread (see struct la_thread). Engage gates: the env
 * tri-state, whose AUTO is "we resolved a lead to run into" (e->la_buf > 0,
 * x264's own coupling --returns threadless at
 * --sync-lookahead 0, and a chain with zero lead measured a wash here from
 * the day it landed); a pool of >= la_pool_min workers (below that there
 * is no pool for the wavefront either, so nothing to run ahead of); and
 * la_depth >= bframes+3 so every
 * popped entry's chain writers are strictly older than the newest push. On
 * any failure fall back silently to the in-line chain (byte-identical
 * either way). */
    int la_th_want = la_thread_env();
    if (la_th_want < 0) la_th_want = e->la_buf > 0;
    if (la_th_want && e->la_depth >= e->bframes + 3 &&
        e->pool && ntp_pool_nthreads(e->pool) >= la_pool_min()) {
        struct la_thread *lt = calloc(1, sizeof *lt);
        if (lt) {
            pthread_mutex_init(&lt->mx, NULL);
            pthread_cond_init(&lt->cv_push, NULL);
            pthread_cond_init(&lt->cv_done, NULL);
            lt->bg = ntp_bg_create_named("y264-la");
            if (lt->bg) {
                e->la_th = lt;
                e->la_th_on = 1;
                ntp_bg_submit(lt->bg, la_th_main, e);
            } else {
                pthread_mutex_destroy(&lt->mx);
                pthread_cond_destroy(&lt->cv_push);
                pthread_cond_destroy(&lt->cv_done);
                free(lt);
            }
        }
    }

    /* The chain's fan-out decision, resolved once the la thread's fate is
 * known. Two conditions; the second is measured at 720p, not CIF-tuned.
 *
 * Off-driver chain: the fan-out queues ~60 us of row work behind the encode
 * wavefront's own rows, so the join blocks for a multiple of the work it
 * spread -- worth 3.0 ms with the chain off the driver, against 1-3 ms on
 * it. SMALL FRAME: at CIF a chain row IS ~60 us and inlining wins, but
 * at 720p+ there is enough row work per push that fanning it out is worth
 * -3 to -6% of pure-C t18 wall (samsung/sintel/park_joy) and -2 to -3%
 * as shipped, while inline costs that. Same threshold the lowres field-ME
 * wavefront uses to decide a grid is worth launching at all
 * (lowres_field_me_prep), for the same reason: below a few hundred blocks
 * the launch outweighs the walk.
 *
 * Scheduling only -- the row reduction is order-exact either way. */
    e->la_inline = la_inline_env();
    if (e->la_inline < 0)
        e->la_inline = e->la_th_on &&
                       e->width_in_mbs * e->height_in_mbs < LA_FANOUT_MBS;

    /* Y264_LA_STAT=1: one line per opened encoder saying what this instance
 * actually resolved. A GOP-parallel run opens one encoder per worker (per
 * GOP in the pull-queue case), so this is the only way to see that a
 * per-worker pool is too narrow for the la thread -- the CLI's single
 * lookahead-lead line reports the critical worker and nothing else. */
    if (getenv("Y264_LA_STAT") && atoi(getenv("Y264_LA_STAT")))
        fprintf(stderr, "yah264: la_stat %dx%d req_ft=%d pool=%d la_depth=%d "
                "la_buf=%d la_th=%d la_inline=%d\n",
                e->param.width, e->param.height, param->frame_threads,
                e->pool ? ntp_pool_nthreads(e->pool) : 0,
                e->la_depth, e->la_buf, e->la_th_on, e->la_inline);

    /* mb-tree prefetch thread (see struct mbt_pre). Needs the window (for a
 * walk to prefetch at all) and la_buf >= 1 (for the window's last entry to
 * be typed one call early); without either it silently stays on the
 * driver, byte-identically. */
    /* The warm needs a window to warm (its lead comes from the ring, not from
 * spare ring capacity) and an off-driver chain to lead: mbt_warm_window
 * returns at its first line when there is no la thread, so without one the
 * thread would only be a cond-var round trip per anchor -- which is the
 * whole cost the warm's default-on would otherwise add at --threads 1. The
 * whole-anchor prefetch additionally needs la_buf >= 1 for the window's
 * last entry to be typed one call early. */
    int warm_want = mbt_warm_env() && e->la_th_on;
    int mbtp_want = (warm_want && e->la_depth > 1) ||
                    ((mbt_pre_env() || mbt_lead_env()) && e->la_depth > 1 &&
                     e->la_buf >= 1);
    if (mbtp_want && e->mbtree_on) {
        struct mbt_pre *mp = calloc(1, sizeof *mp);
        if (mp) {
            mp->warm = warm_want;
            /* Lead mode is the waiting gate, and it needs a chain thread to
 * wait ON; with the chain inline on the driver there is nothing
 * ahead to wait for and it degrades to the tested prefetch. */
            mp->lead = !mp->warm && mbt_lead_env() && e->la_th_on;
            mp->out_off = malloc(nmb);
            pthread_mutex_init(&mp->mx, NULL);
            pthread_cond_init(&mp->cv_req, NULL);
            pthread_cond_init(&mp->cv_done, NULL);
            mp->bg = mp->out_off ? ntp_bg_create_named("y264-mbtree") : NULL;
            if (mp->bg) {
                e->mbtp = mp;
                ntp_bg_submit(mp->bg, mbt_pre_main, e);
            } else {
                pthread_mutex_destroy(&mp->mx);
                pthread_cond_destroy(&mp->cv_req);
                pthread_cond_destroy(&mp->cv_done);
                free(mp->out_off);
                free(mp);
            }
        }
    }
    return e;
}

/* SEI user_data_unregistered (payloadType 5): a fixed yah264 UUID plus a human
 * settings string, mirroring x264's SEI so a stream's config is verifiable from
 * the bitstream (strings/ffprobe). Informational only; nal_ref_idc = 0. */
static void write_settings_sei(yah264_encoder_t *e, y264_bs_t *bs)
{
    static const uint8_t uuid[16] = {
        0x6e,0x65,0x78,0x74,0x32,0x36,0x34,0x2d,   /* "yah264-" */
        0x73,0x65,0x69,0x2d,0x76,0x31,0x00,0x01    /* "sei-v1\0\1" */
    };
    const yah264_param_t *p = &e->param;
    char rc[48];
    switch (p->rc.method) {
        case YAH264_RC_CRF:   snprintf(rc, sizeof rc, "crf=%.1f", p->rc.rf); break;
        case YAH264_RC_ABR:   snprintf(rc, sizeof rc, "bitrate=%d", p->rc.bitrate); break;
        case YAH264_RC_2PASS: snprintf(rc, sizeof rc, "2pass bitrate=%d", p->rc.bitrate); break;
        default:               snprintf(rc, sizeof rc, "qp=%d", p->rc.qp); break;
    }
    char s[256];
    int slen = snprintf(s, sizeof s,
        "yah264 %s - options: cabac=%d ref=%d bframes=%d 8x8dct=%d subme=%d "
        "aq=%.2f mbtree=%d keyint=%d %s%s",
        yah264_version(), p->cabac, p->ref, p->bframes, p->transform8x8,
        p->subme > 0 ? p->subme : 10, e->aq_strength,
        p->rc.lookahead > 0 ? 1 : 0, p->keyint, rc,
        y264_mbt_derived() ? " mbtree-mode=wholebuf" : "");
    if (slen < 0) slen = 0;
    if (slen > (int)sizeof s - 1) slen = (int)sizeof s - 1;

    y264_bs_write(bs, 8, 5);                       /* payloadType = user_data_unregistered */
    int psize = 16 + slen;                         /* UUID + string */
    while (psize >= 255) { y264_bs_write(bs, 8, 0xFF); psize -= 255; }
    y264_bs_write(bs, 8, psize);                   /* final payloadSize byte */
    for (int i = 0; i < 16; i++) y264_bs_write(bs, 8, uuid[i]);
    for (int i = 0; i < slen; i++) y264_bs_write(bs, 8, (uint8_t)s[i]);
    y264_bs_rbsp_trailing(bs);
}

int yah264_encoder_headers(yah264_encoder_t *e, yah264_nal_t **nal, int *count)
{
    if (e && e->hw) return y264_hw_headers(e->hw, nal, count);
    if (!e || !nal || !count)
        return -1;
    e->nal_count = 0;
    size_t off = 0;

    y264_bs_t bs;
    /* The access unit delimiter goes ahead of the parameter sets (7.4.1.2.3),
 * so the first AU's delimiter belongs here rather than at its first slice.
 * au_opened tells that slice one is already written. The first picture's
 * pic_timing SEI is NOT written here: it has to follow the parameter sets,
 * so the slice-side opener writes it like every other picture's. */
    if (e->param.aud) {
        uint8_t b0[16];
        y264_bs_t b1;
        y264_bs_init(&b1, b0, sizeof b0);
        y264_aud_write(&b1, 0);                  /* the first picture is an IDR */
        if (append_nal_raw(e, &off, YAH264_NAL_PRIORITY_DISPOSABLE, 9,
                           b0, (size_t)(b1.p - b1.start)) < 0)
            return -1;
        e->au_opened = 1;
    }
    y264_bs_init(&bs, e->rbsp, e->rbsp_cap);
    y264_sps_write(&bs, &e->sps);
    if (append_nal(e, &off, YAH264_NAL_PRIORITY_HIGH, YAH264_NAL_SPS,
                   e->rbsp, (size_t)(bs.p - bs.start)) < 0)
        return -1;

    y264_bs_init(&bs, e->rbsp, e->rbsp_cap);
    y264_pps_write(&bs, &e->pps);
    if (append_nal(e, &off, YAH264_NAL_PRIORITY_HIGH, YAH264_NAL_PPS,
                   e->rbsp, (size_t)(bs.p - bs.start)) < 0)
        return -1;

    if (e->param.sei) {
        y264_bs_init(&bs, e->rbsp, e->rbsp_cap);
        write_settings_sei(e, &bs);
        if (append_nal(e, &off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                       e->rbsp, (size_t)(bs.p - bs.start)) < 0)
            return -1;
    }

    /* The static display-metadata messages. They describe the WHOLE stream and
 * never change, so they are written once beside the parameter sets rather
 * than per picture, which is where the per-picture pic_timing lives. Each
 * one is its own SEI NAL, so a decoder that does not know a payload type
 * skips that NAL rather than the group. */
    {
        uint8_t msg[64];
        size_t n;
        const yah264_param_t *pp = &e->param;
        if (pp->frame_packing >= 0) {
            n = y264_sei_frame_packing(msg, sizeof msg, pp->frame_packing);
            y264_bs_init(&bs, e->rbsp, e->rbsp_cap);
            y264_sei_write(&bs, 45, msg, n);
            if (append_nal(e, &off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                           e->rbsp, (size_t)(bs.p - bs.start)) < 0)
                return -1;
        }
        if (pp->cll_max || pp->cll_avg) {
            n = y264_sei_cll(msg, sizeof msg, pp->cll_max, pp->cll_avg);
            y264_bs_init(&bs, e->rbsp, e->rbsp_cap);
            y264_sei_write(&bs, 144, msg, n);
            if (append_nal(e, &off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                           e->rbsp, (size_t)(bs.p - bs.start)) < 0)
                return -1;
        }
        if (pp->mastering_set) {
            n = y264_sei_mastering(msg, sizeof msg, pp->mastering_prim,
                                   pp->mastering_wp, pp->mastering_max, pp->mastering_min);
            y264_bs_init(&bs, e->rbsp, e->rbsp_cap);
            y264_sei_write(&bs, 137, msg, n);
            if (append_nal(e, &off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                           e->rbsp, (size_t)(bs.p - bs.start)) < 0)
                return -1;
        }
        if (pp->alternative_transfer) {
            n = y264_sei_alt_transfer(msg, sizeof msg, pp->alternative_transfer);
            y264_bs_init(&bs, e->rbsp, e->rbsp_cap);
            y264_sei_write(&bs, 147, msg, n);
            if (append_nal(e, &off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                           e->rbsp, (size_t)(bs.p - bs.start)) < 0)
                return -1;
        }
    }

    e->headers_done = 1;
    *nal = e->nal;
    *count = e->nal_count;
    return 0;
}

/* --- lowres lookahead analysis (shared by scene-cut, adaptive-B, mb-tree) ---
 * All costs are on half-resolution luma: a 16x16 macroblock maps to an 8x8
 * lowres block, so the motion search is ~4x cheaper and reaches twice as far. */

/* Downscale a padded luma plane to half resolution by 2x2 averaging. */
static void downscale(pixel *dst, int dw, int dh, const pixel *src, int ss)
{
    for (int y = 0; y < dh; y++) {
        const pixel *a = src + (2 * y) * ss, *b = a + ss;
        pixel *d = dst + y * dw;
        for (int x = 0; x < dw; x++) {
            int x2 = 2 * x;
            d[x] = (pixel)((a[x2] + a[x2 + 1] + b[x2] + b[x2 + 1] + 2) >> 2);
        }
    }
}

/* SATD of an 8x8 lowres block against a reference block (four 4x4 SATDs). */
static long blk8_satd(const pixel *s, int ss, const pixel *r, int rs)
{
    /* One fused 8x8 SATD (== the four 4x4 SATDs summed) via one dispatched call,
 * instead of four indirect satd4x4 calls -- byte-identical, hits SWAR/NEON
 * fused kernel. Lowres mb-tree calls this per candidate (~20% of pure-C). */
    return y264_dsp.satd8x8(s, ss, r, rs);
}

/* Intra cost of an 8x8 lowres block: 4x4 SATD of each quadrant against its DC. */
static long blk8_intra(const pixel *s, int ss)
{
    long t = 0;
    for (int y = 0; y < 8; y += 4)
        for (int x = 0; x < 8; x += 4) {
            const pixel *b = s + y * ss + x;
            int sum = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) sum += b[j * ss + i];
            pixel flat[16];
            int m = (sum + 8) >> 4;
            for (int k = 0; k < 16; k++) flat[k] = (pixel)m;
            t += y264_dsp.satd4x4(b, ss, flat, 4);
        }
    return t;
}

/* The reference adds an intra penalty of 5 to a 1x-domain satd; blk8_intra_neighbour's
 * best is our deliberately 2x-domain satd8x8 (src/dsp/, verified over 300k
 * blocks), so the FAITHFUL port is 10 -- the census's M0 question, ANSWERED
 * 08-27: the band run (9 clips incl both animation kinds, points 22-40,
 * VMAF-NEG) reads MEDIAN +0.02% for 10 vs 5 -- a wash, so the shipped 5
 * survives, consistent with M1's record of census staleness suspects
 * measuring clean. Per-clip spread exists (bus -8.07 / bbb +6.97, both rows
 * saturation-flagged) and is M4 evidence, not a default question. Resolved
 * once on the main thread (warm_lr_statics); pool workers then read it
 * read-only. */
static int lr_ipen(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_LR_IPEN"); v = e ? atoi(e) : 5; }
    return v;
}

/* behaviour-matched lowres intra cost: a
 * NEIGHBOUR-predicted 8x8 (DC/H/V/plane, the chroma-style set x264 scores),
 * measured with the SAME satd8x8 as the inter metric (blk8_satd), plus x264's
 * intra penalty of 5*lambda(lookahead qp 12) = 5 (pre-shift). This replaces the
 * legacy per-4x4-vs-own-mean cost, whose own-DC + per-subblock mean systematically
 * UNDER-reads intra on smooth/gradient motion content (bus/stefan) and is metric-
 * inconsistent with the DC-inclusive inter satd8x8 -- an underread denominator that
 * clamps the mb-tree propagate fraction regardless of inter quality.
 * The lowres plane has no border, so edge blocks use
 * available-neighbour fallbacks (an edge-MB infidelity vs x264's padded border).
 * Gated Y264_LR_INTRA_NEIGHBOUR; producers dispatch via blk8_intra_dispatch. */
static long blk8_intra_neighbour(const pixel *s, int ss, int have_top, int have_left)
{
    const int dcv = 1 << (Y264_BIT_DEPTH - 1);
    pixel top[8], left[8], tl = (pixel)dcv, pred[64];
    if (have_top)  for (int x = 0; x < 8; x++) top[x]  = s[-ss + x];
    if (have_left) for (int y = 0; y < 8; y++) left[y] = s[y * ss - 1];
    if (have_top && have_left) tl = s[-ss - 1];

    long best = -1;
    /* DC (availability-aware, H.264 rounding) */
    int dc;
    if (have_top && have_left) {
        int st = 0, sl = 0;
        for (int i = 0; i < 8; i++) { st += top[i]; sl += left[i]; }
        dc = (st + sl + 8) >> 4;
    } else if (have_top) {
        int st = 0; for (int i = 0; i < 8; i++) st += top[i]; dc = (st + 4) >> 3;
    } else if (have_left) {
        int sl = 0; for (int i = 0; i < 8; i++) sl += left[i]; dc = (sl + 4) >> 3;
    } else dc = dcv;
    for (int k = 0; k < 64; k++) pred[k] = (pixel)dc;
    best = y264_dsp.satd8x8(s, ss, pred, 8);

    if (have_top) {                                 /* V */
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) pred[y * 8 + x] = top[x];
        long c = y264_dsp.satd8x8(s, ss, pred, 8); if (c < best) best = c;
    }
    if (have_left) {                                /* H */
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) pred[y * 8 + x] = left[y];
        long c = y264_dsp.satd8x8(s, ss, pred, 8); if (c < best) best = c;
    }
    if (have_top && have_left) {                    /* plane (8x8 chroma-style) */
        int H = 0, V = 0;
        for (int i = 0; i < 4; i++) {
            pixel tm = (i == 3) ? tl : top[2 - i];
            pixel lm = (i == 3) ? tl : left[2 - i];
            H += (i + 1) * (top[4 + i] - tm);
            V += (i + 1) * (left[4 + i] - lm);
        }
        int a = 16 * (top[7] + left[7]);
        int b = (17 * H + 16) >> 5;
        int c2 = (17 * V + 16) >> 5;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int v = (a + b * (x - 3) + c2 * (y - 3) + 16) >> 5;
                pred[y * 8 + x] = y264_clip_pixel(v);
            }
        long c = y264_dsp.satd8x8(s, ss, pred, 8); if (c < best) best = c;
    }
    return ((best + lr_ipen()) >> (Y264_BIT_DEPTH - 8));
}

static int s_lr_intra_neighbour = -1;
static long blk8_intra_dispatch(const pixel *s, int ss, int mx, int my)
{
    if (s_lr_intra_neighbour < 0) {
        const char *e = getenv("Y264_LR_INTRA_NEIGHBOUR");
        s_lr_intra_neighbour = e ? atoi(e) : 1;      /* DEFAULT ON (BD-gated win); =0 escapes */
    }
    if (s_lr_intra_neighbour)
        return blk8_intra_neighbour(s, ss, my > 0, mx > 0);
    return blk8_intra(s, ss);
}

/* Best integer-pel inter cost of an 8x8 lowres block via a shrinking-step diamond
 * search; returns the cost and writes the winning lowres MV. */
/* Y264_LR_PREV_SHAPE: score this search's probes with SAD and pay SATD once
 * for the winner -- x264's lowres shape (the cheap compare during the walk,
 * the full metric for the winner). This is the
 * push-time cost of every frame against its previous frame, and it scored
 * a five-level diamond with SATD at every probe: ~60 SATDs per block per
 * frame, 4.8G SATD pixels on sunflower_1080p -- a quarter of ALL our SATD and
 * twice x264's entire lookahead.
 *
 * DEFAULT ON (2026-09-02), together with Y264_LR_SHAPE (the pair-leg search)
 * and Y264_LR_SADINT (Phase A's integer levels): the three take our SATD
 * volume on sunflower_1080p from 18.1G pixels to 10.0G (x264: 11.6G) and are
 * priced as one change. Wall at --threads 12, medians of 5: sunflower -4.3%,
 * shields -2.9%, samsung -3.5%, pedestrian -3.3%, foreman -3.4%, park_joy
 * -2.2%; at t1 sunflower -3.2%, shields -2.0%. CRF band, 16 clips at 150f:
 * median -0.02%, range -1.26 (foreman) .. +1.24 (sintel). Y264_LR_PREV_SHAPE=0
 * Y264_LR_SHAPE=0 Y264_LR_SADINT=0 restores the SATD-probed searches. */
static int lr_prev_shape_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_LR_PREV_SHAPE"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
static long blk8_inter(const pixel *sb, int ss, const pixel *ref, int rs,
                       int lw, int lh, int bx, int by, int *outmvx, int *outmvy)
{
    int cx = 0, cy = 0;
    int shape = lr_prev_shape_on();
#define PREV_PROBE(p) (shape ? (long)y264_dsp.sad[Y264_PU_8x8](sb, ss, (p), rs) : blk8_satd(sb, ss, (p), rs))
    long best = PREV_PROBE(ref + by * rs + bx);
    for (int step = 16; step >= 1; step >>= 1) {
        for (int iter = 0; iter < 8; iter++) {
            int cand[4][2] = { {cx + step, cy}, {cx - step, cy},
                               {cx, cy + step}, {cx, cy - step} };
            int moved = 0;
            for (int k = 0; k < 4; k++) {
                int rx = bx + cand[k][0], ry = by + cand[k][1];
                if (rx < 0 || ry < 0 || rx > lw - 8 || ry > lh - 8) continue;
                long c = PREV_PROBE(ref + ry * rs + rx);
                if (c < best) { best = c; cx = cand[k][0]; cy = cand[k][1]; moved = 1; }
            }
            if (!moved || best == 0) break;
        }
    }
#undef PREV_PROBE
    if (shape)                              /* back into the SATD domain the consumers read */
        best = blk8_satd(sb, ss, ref + (by + cy) * rs + bx + cx, rs);
    *outmvx = cx; *outmvy = cy;
    return best;
}

/* Approximate bits to code one lowres mvd component as signed exp-Golomb: the
 * se(v) codeNum is 2|d| (d<=0) or 2|d|-1 (d>0), whose ue(v) length is
 * 2*floor(log2(codeNum+1))+1. Used only to price motion into the mb-tree
 * propagation fraction (never into scene-cut / b-adapt / CRF costs). */
static int lowres_mvbits(int d)
{
    unsigned k = d > 0 ? (unsigned)(2 * d - 1) : (unsigned)(-2 * d);
    /* floor(log2(k+1)) via clz -- identical to the old shift loop, minus the
 * per-probe loop (this sits under every lowres ME probe's rate term). */
    return 2 * (31 - __builtin_clz(k + 1)) + 1;
}

/* Exact memo for the COH_COST rate term: lround(mvlambda * bits) with bits a
 * small integer sum, so a per-thread 97-entry table keyed on mvlambda replaces
 * a double multiply + lround per probe. Identical values by construction
 * (same lround of the same product); out-of-range bits falls back. TLS, but
 * resolved ONCE per search (coh_tab) so the per-probe lookup is a plain
 * indexed load, not a TLS address walk. Each pool worker fills its own copy
 * once per distinct mvlambda (constant per encode in practice). */
static const long *coh_tab(double mvlambda)
{
    static _Thread_local struct { double key; long t[97]; } s_coh = { -1.0, { 0 } };
    if (mvlambda != s_coh.key) {
        for (int s = 0; s <= 96; s++)
            s_coh.t[s] = (long)lround(mvlambda * s);
        s_coh.key = mvlambda;
    }
    return s_coh.t;
}
static inline long coh_rate(const long *ctab, double mvlambda, int bits)
{
    return bits <= 96 ? ctab[bits] : (long)lround(mvlambda * bits);
}

/* lambda for pricing lowres MV rate into the propagation fraction. Without it,
 * a well-tracked block has inter cost ~ pure SATD ~ 0, so propfrac = 1-inter/intra
 * -> 1 and importance accumulates ~linearly along an IPPP chain (40 hops) until
 * it saturates the offset clamp and collapses the intra-frame QP gradient. A
 * lambda*mvd term pushes propfrac below 1 on real motion so the chain converges.
 * Predictor is the raster left neighbour (coherent pans stay cheap, matching
 * the reference's median-predicted lowres MV rate). Tunable for calibration. */
static double mbtree_mvlambda(void)
{
    double l = 8.0;
    const char *v = getenv("Y264_MBTREE_MVLAMBDA");
    if (v) l = atof(v);
    return l;
}

/* Apply the IPPP anti-saturation treatment (price the lowres MV into the chain
 * propfrac + the wide QP-offset clamp) to the bframes>0 path too. It was
 * originally IPPP-only, leaving the medium (B-frame) chain to over-propagate over
 * ~13 anchor hops and saturate the +/-8 clamp -- the +13-20% motion-clip quality
 * gap vs x264. Default on; Y264_MBTREE_BFIX=0 restores the old B path. */
static int mbtree_bfix(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_MBTREE_BFIX"); v = e ? atoi(e) : 1; }
    return v;
}

/* Analyse the current lowres frame against the previous one, caching per-MB intra
 * and best-inter costs and the winning lowres MV. */
/* One macroblock row of the lowres analysis. Every block reads only the
 * (read-only) lowres planes and writes its own slot, so rows are independent. */
/* env cache; warmed at open (warm_lr_statics) so worker threads only read it */
static int lr_reuse_on(void)
{
    static int env = -2;
    if (env == -2) { const char *v = getenv("Y264_LR_REUSE"); env = v ? atoi(v) : 1; }
    return env;
}

static void lowres_row(yah264_encoder_t *e, int my)
{
    NLED_SITE(Y264_LED_SITE_LOWRES);
    int lw = e->lr_w, wmb = e->width_in_mbs;
    const pixel *cur = e->lowres_cur, *prev = e->lowres_prev;
    for (int mx = 0; mx < wmb; mx++) {
        const pixel *sb = cur + (my * 8) * lw + mx * 8;
        int idx = my * wmb + mx, mvx = 0, mvy = 0;
        e->lr_intra[idx] = (int)blk8_intra_dispatch(sb, lw, mx, my);
        e->lr_inter[idx] = e->sc_have_prev
            ? (int)blk8_inter(sb, lw, prev, lw, lw, e->lr_h, mx * 8, my * 8, &mvx, &mvy)
            : e->lr_intra[idx];
        e->lr_mvx[idx] = (int16_t)mvx;
        e->lr_mvy[idx] = (int16_t)mvy;
    }
}
static void lowres_row_task(void *ctx, int tid, int my)
{
    NLED_SITE(Y264_LED_SITE_LOWRES);
    (void)tid;
    lowres_row((yah264_encoder_t *)ctx, my);
}
static void lowres_analyse(yah264_encoder_t *e)
{
    if (!e->la_inline &&
        e->pool && ntp_pool_nthreads(e->pool) > 1 && e->height_in_mbs > 1) {
        ntp_prof_tag("lowres_analyse"); ntp_prio_hint();
        ntp_parallel_for(e->pool, e->height_in_mbs, lowres_row_task, e);
        return;
    }
    for (int my = 0; my < e->height_in_mbs; my++)
        lowres_row(e, my);
}

/* Content-adaptive ME (speed step #2): frame-level motion score from the
 * lowres MV field lowres_analyse just filled (this display frame vs the
 * previous one; integer lowres pel = 2 full pel). Score = mean (|mvx|+|mvy|)
 * per lowres block, x64 fixed point. Frames below the threshold run cheap ME
 * (no UMH, capped subpel) -- the config that wins BD on static content and
 * only blows up on motion. Y264_ADME = threshold (0 = off, the default until
 * BD-gated); Y264_ADME_LOG=1 dumps per-frame scores for calibration. */
static int adme_thresh(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ADME"); v = e ? atoi(e) : 0; }
    return v;
}

/* The psy-trellis pays only on
 * flat/dark content (sintel -7.68, samsung -0.94 at matched rate; grain and
 * high-motion clips flat-to-positive), and the separating source feature is
 * the share of near-flat macroblocks (sintel 92%, samsung 56% vs <=18% on
 * every measured loser). Y264_PSY_FLAT_GATE=<share_pct>,<s_x256>[,<var_T>]:
 * when >= share_pct percent of a frame's 16x16 luma MBs have variance < var_T
 * (default 25), that frame's psy-trellis strength gets floor s_x256/256.
 * Frame-level and input-derived only, so deterministic at any thread count.
 * Default off = byte-identical. */
static int psy_flat_gate(int idx)
{
    static int v[3] = { -2, 0, 0 };
    if (v[0] == -2) {
        /* DEFAULT ON at 75,307,25: sintel-only by engagement (min share 84
         * vs samsung's max 71 -- fires 120/120 on sintel, 0/120 on every other
         * corpus clip), through the psy lattice. sintel -5.57 high / -8.99 low
         * band at matched rate, +0.8% wall on sintel alone, everything else
         * byte-identical. Y264_PSY_FLAT_GATE=-1 is the escape. */
        int p[3] = { 75, 307, 25 };
        const char *e = getenv("Y264_PSY_FLAT_GATE");
        if (e) { p[0] = -1; p[1] = 0; p[2] = 25; }
        for (int i = 0; e && i < 3; i++) {
            p[i] = atoi(e);
            e = strchr(e, ',');
            if (e) e++;
        }
        if (p[1] <= 0 || p[2] <= 0) p[0] = -1;      /* malformed: off */
        v[1] = p[1]; v[2] = p[2];
        v[0] = p[0];
    }
    return v[idx];
}
static int psy_flat_log(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PSY_FLAT_LOG"); v = e ? atoi(e) : 0; }
    return v;
}

/* Second psy class rule: CALM AND TEXTURED.
 * The four forfeited winners (akiyo/tempete/coastguard/ducks, -0.4..-1.7 at
 * matched rate under constant psy) separate from the losers on uncompensated
 * lowres |tdiff| (winners <= ~10.9 fullres units, mobile 13.2 the near
 * loser) -- but tdiff alone admits touchdown (5.04, label +2.26), whose
 * separator is texture: median MB variance 40 vs akiyo's 65 and the others'
 * 300-800. So: tdiff EWMA below T AND the share of MBs at var >= var_T above
 * share_pct. Y264_PSY_CALM_GATE=<tdiff_T_x256>,<s_x256>[,<var_T>,<share_pct>];
 * default off = byte-identical. Composes with the flat gate by max. */
static int psy_calm_gate(int idx)
{
    /* idx: 0 tdiff_T (<0 off), 1 s_x256, 2 var_T, 3 share_pct, 4 qp_max,
 * 5 share_hi, 6 tdiff_static. v2 admit: tdiff < T AND texshare >= share
 * AND fqp <= qp_max AND (texshare >= share_hi OR tdiff < tdiff_static).
 * The last term is two regions the labels force: dense-texture calm
 * (tempete/coastguard/ducks, texshare >= 85) or near-static (akiyo,
 * tdiff < 300) -- foreman (69, 1165) is neutral-to-negative under psy and
 * fails both; the qp ceiling keeps the gate out of the deep-quant regime
 * where every psy form measured negative (Q2, and this gate's own low
 * band at +0.8..+1.6 on the winners). */
    static int v[7] = { -2, 0, 64, 40, 51, 100, 0 };
    if (v[0] == -2) {
        int p[7] = { -1, 0, 64, 40, 51, 100, 0 };
        const char *e = getenv("Y264_PSY_CALM_GATE");
        for (int i = 0; e && i < 7; i++) {
            p[i] = atoi(e);
            e = strchr(e, ',');
            if (e) e++;
        }
        if (p[1] <= 0 || p[2] <= 0 || p[3] <= 0) p[0] = -1;
        for (int i = 1; i < 7; i++) v[i] = p[i];
        v[0] = p[0];
    }
    return v[idx];
}
static int adme_log(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_ADME_LOG"); v = e ? atoi(e) : 0; }
    return v;
}
static int frame_motion_score(yah264_encoder_t *e)
{
    if (!e->sc_have_prev) return INT_MAX;
    int nmb = e->width_in_mbs * e->height_in_mbs;
    long s = 0;
    for (int i = 0; i < nmb; i++) {
        int ax = e->lr_mvx[i] < 0 ? -e->lr_mvx[i] : e->lr_mvx[i];
        int ay = e->lr_mvy[i] < 0 ? -e->lr_mvy[i] : e->lr_mvy[i];
        s += ax + ay;
    }
    return (int)(s * 64 / nmb);
}

/* Bilinearly distribute `amt` over the up-to-4 8x8 lowres blocks overlapped by
 * a block-sized region at lowres pixel (tx, ty); out-of-frame weight clamps to
 * the edge block so no propagated energy is dropped. */
static void splat_prop(double *grid, int wmb, int hmb, int tx, int ty, double amt)
{
    int bx = tx >> 3, by = ty >> 3;
    int dx = tx - bx * 8, dy = ty - by * 8;
    int w0 = (8 - dx) * (8 - dy), w1 = dx * (8 - dy);
    int w2 = (8 - dx) * dy,       w3 = dx * dy;
    /* The four targets are (bx,by), (bx+1,by), (bx,by+1), (bx+1,by+1), so the
 * clamp has only two distinct x and two distinct y -- the rolled loop
 * recomputed each of them twice. Hoisting them and unrolling keeps the
 * deposit ORDER (k = 0,1,2,3) exactly, which is what makes the offsets
 * bit-identical; the border clamp can make two of the four targets the
 * same cell, so these adds are not independent and must not be reordered.
 * `amt * w / 64.0` stays written that way for the same reason. */
    int x0 = bx     < 0 ? 0 : (bx     >= wmb ? wmb - 1 : bx);
    int x1 = bx + 1 < 0 ? 0 : (bx + 1 >= wmb ? wmb - 1 : bx + 1);
    int y0 = by     < 0 ? 0 : (by     >= hmb ? hmb - 1 : by);
    int y1 = by + 1 < 0 ? 0 : (by + 1 >= hmb ? hmb - 1 : by + 1);
    double *r0 = grid + (size_t)y0 * wmb, *r1 = grid + (size_t)y1 * wmb;
    if (w0) r0[x0] += amt * w0 / 64.0;
    if (w1) r0[x1] += amt * w1 / 64.0;
    if (w2) r1[x0] += amt * w2 / 64.0;
    if (w3) r1[x1] += amt * w3 / 64.0;
}

/* AQ-aware inverse quantiser weights for mb-tree propagation. From a full-res
 * plane, derive per-MB the SAME log2(16x16 variance) offset that aq_analyze
 * applies at encode time (qp_offset_aq = strength*(log2var - mean)), then the
 * matching inverse-qscale weight ~ 2^(-qp_offset_aq/6): flat blocks (negative
 * offset, lower QP) weigh MORE, so they propagate more importance -- x264's
 * inv_qscale_factor. Weights are normalised to mean 1 so the global propagation
 * magnitude is unchanged (the ratio in the finish step is scale-invariant).
 * When aq_off is non-NULL the raw qp_offset_aq (unclamped, float) is returned
 * there for folding into the combined offset. */
/* Sum-of-squared-deviations of a w x h block, the reference's per-plane
 * AC-energy variance for a plane
 * whose block is not 16x16 (chroma at 4:2:0 / 4:2:2). Returns npix*var, the
 * same units as the luma term. */
static double blk_ac_energy(const pixel *p, int stride, int w, int h)
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

/* Y264_MBT_AQIN=<f>: the AQ strength the mb-tree machinery consumes INTERNALLY
 * (the walk's inv-qscale weights and the finish's intra weight Fw), decoupled
 * from the CODED AQ strength. The AQ field plays two roles -- the coded per-MB
 * offset, and the weights that shape the propagation accumulator -- and a
 * coded aq_strength of 0.4 rather than 1.0 flattens the second role:
 * the mb-tree term's whole value at aq 0.4 measures
 * samsung +0.74 / pjoy -2.08 / bus -4.63 (BD-NEG, negative = valuable) against
 * -6.86 / -12.13 / -12.37 with everything at aq 1.0, where x264 reads -14..-16.
 * Unset (default) = weights follow the coded strength, byte-identical. */
static float mbt_aqin(void)
{
    static float v = -2.0f;
    if (v < -1.5f) { const char *s = getenv("Y264_MBT_AQIN"); v = s ? (float)atof(s) : -1.0f; }
    return v;
}

static void mbtree_invqscale(const yah264_encoder_t *e, pixel *const *pl,
                             int stride, int wmb, int hmb,
                             float strength, double *invq, float *aq_off)
{
    int n = wmb * hmb;
    double sum = 0;
    /* The reference's per-macroblock AC-energy term sums every plane; the
 * chroma block is the MB's footprint at the subsampled resolution
 * (16/sub_w x 16/sub_h), which is exactly what its chroma fetch covers. */
    int cw = e->aq_chroma ? 16 / e->sub_w : 0, ch = e->aq_chroma ? 16 / e->sub_h : 0;
    int cstride = e->pstride[1];
    for (int mby = 0; mby < hmb; mby++)
        for (int mbx = 0; mbx < wmb; mbx++) {
            const pixel *s = pl[0] + (mby * 16) * stride + mbx * 16;
            uint32_t v2[2];
            y264_dsp.var16x16(s, stride, v2);
            uint32_t s1 = v2[0], s2 = v2[1];
            double mean = s1 / 256.0, var = s2 / 256.0 - mean * mean;
            if (var < 0) var = 0;
            double l;
            if (e->aq_chroma) {
                /* Energy in x264's absolute units, then scaled back by the 256
 * luma pixels so the anchor stays in the log2(var) frame the
 * rest of this file uses (aq_anchor_default). */
                double en = var * 256.0
                    + blk_ac_energy(pl[1] + (mby * ch) * cstride + mbx * cw, cstride, cw, ch)
                    + blk_ac_energy(pl[2] + (mby * ch) * cstride + mbx * cw, cstride, cw, ch);
                l = log2(en < 1.0 ? 1.0 : en) - 8.0;
            } else {
                l = log2(var + 1.0);
            }
            invq[mby * wmb + mbx] = l;
            sum += l;
        }
    /* Absolute anchor instead of the frame mean under Y264_CRF_CPLX: the DC of
 * this field is x264's CRF complexity term (see crf_cplx_env). The 1.0397
 * is x264's aq-mode-1 strength scale . The propagation
 * weights are renormalised to mean 1 below, so a DC shift leaves them
 * untouched; it reaches the bitstream through aq_off only -- and, exactly
 * as in x264, through the unnormalised intra-cost weight in the mb-tree
 * finish step, which is why a flat clip's AQ discount damps its own
 * mb-tree boost there. */
    int abs_aq = e->aq_abs;
    double avg = abs_aq ? e->aq_anchor : sum / n, wsum = 0;
    double astr = abs_aq ? strength * 1.0397 : strength;
    /* The weight half may run at its own strength (Y264_MBT_AQIN); aq_off --
 * the CODED field -- always uses the caller's strength. */
    float aqin = mbt_aqin();
    double astr_w = aqin >= 0.0f ? (abs_aq ? aqin * 1.0397 : (double)aqin) : astr;
    for (int i = 0; i < n; i++) {
        double base = invq[i] - avg;
        if (aq_off) aq_off[i] = (float)(astr * base);
        invq[i] = pow(2.0, -(astr_w * base) / 6.0);
        wsum += invq[i];
    }
    double norm = n / wsum;
    for (int i = 0; i < n; i++) invq[i] *= norm;
}

/* Chain propagation from the lookahead window into `prop` (nmb doubles) for
 * the anchor being coded now: walk the window's future anchors newest-first,
 * each propagating (intra + inherited) * (1 - inter/intra) onto its previous
 * anchor through its lowres MV field. The walk stops at the next IDR (nothing
 * beyond it depends on the current GOP), which is also why the window can span
 * GOPs without breaking per-GOP thread determinism. */
static void splat_prop_qp(double *grid, int wmb, int hmb, int tx32, int ty32, double amt);

static void la_chain_prop(yah264_encoder_t *e, double *prop)
{
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs, nmb = wmb * hmb;
    int idx[64], na = 0;
    /* Window walk capped at la_depth-2, NOT la_n: at k=0, la_n after
 * a pop is la_depth-1, and the walk's LAST entry (i=la_depth-2) is always
 * the frame pushed in THIS SAME call -- still untyped (typing lags one
 * push, la_finalize), so `!en->typed` always breaks there and only
 * la_depth-2 entries (i=0..la_depth-3) ever get added. With Y264_LA_BUF,
 * that same relative position is NOT the newest push (k more have landed
 * behind it), so it is typed by then and would silently extend the window
 * by one entry unless capped explicitly here: the value must stay
 * independent of la_buf. A la_depth-1 cap passes the byte-identity gate at
 * k=0 and fails at k>0 on bframes 0/3. Ring indices still wrap at the full
 * capacity (la_cap). */
    int wcap = e->la_depth > 1 ? e->la_depth - 2 : 0;
    for (int i = 0; i < e->la_n && i < wcap; i++) {
        struct la_entry *en = &e->la[(e->la_head + i) % e->la_cap];
        if (!en->typed || en->is_idr)
            break;
        if (en->is_anchor)
            idx[na++] = (e->la_head + i) % e->la_cap;
    }
    if (na == 0)
        return;
    double *in = e->la_prop_a, *out = e->la_prop_b;
    double *invq = malloc(nmb * sizeof(double));
    int prop_invq = 1;
    { const char *v = getenv("Y264_MBTREE_PROP_INVQ"); if (v) prop_invq = atoi(v); }
    for (int i = 0; i < nmb; i++) in[i] = 0;
    for (int k = na - 1; k >= 0; k--) {
        struct la_entry *en = &e->la[idx[k]];
        double *dst = (k == 0) ? prop : out;
        if (k != 0)
            for (int i = 0; i < nmb; i++) out[i] = 0;
        /* AQ-aware self-term: this entry's intra importance weighs by its own
 * inv_qscale (flat blocks propagate more), matching the anchor/B path. */
        if (invq && prop_invq)
            mbtree_invqscale(e, en->plane, e->pstride[0], wmb, hmb,
                             e->aq_strength, invq, NULL);
        else
            for (int i = 0; i < nmb; i++) invq[i] = 1.0;
        for (int my = 0; my < hmb; my++)
            for (int mx = 0; mx < wmb; mx++) {
                int i = my * wmb + mx;
                double ia = en->d_intra[i];
                if (ia <= 0) continue;
                const y264_lr_blk *a = &en->leg[LR_LEG_ANCHOR][i];
                double frac = 1.0 - (double)a->d_inter / ia;
                if (frac <= 0) continue;
                double w = invq ? invq[i] : 1.0;
                splat_prop_qp(dst, wmb, hmb, mx * 32 + a->mvx, my * 32 + a->mvy,
                              (ia * w + in[i]) * frac);
            }
        if (k != 0) { double *t = in; in = out; out = t; }
    }
    free(invq);
}

/* Quarter-pel splat (x264's precision: 8-lowres-px MB cell = 32 quarter-units,
 * the x>>5 / x&31 bilinear of mbtree_propagate_list). tx32/ty32 = mx*32 + mv_qpel. */
static void splat_prop_qp(double *grid, int wmb, int hmb, int tx32, int ty32, double amt)
{
    int bx = tx32 >> 5, by = ty32 >> 5;
    int dx = tx32 - bx * 32, dy = ty32 - by * 32;
    int w0 = (32 - dx) * (32 - dy), w1 = dx * (32 - dy);
    int w2 = (32 - dx) * dy,        w3 = dx * dy;
    /* Same shape and the same constraints as splat_prop above: two distinct x,
 * two distinct y, deposit order k = 0,1,2,3 preserved. */
    int x0 = bx     < 0 ? 0 : (bx     >= wmb ? wmb - 1 : bx);
    int x1 = bx + 1 < 0 ? 0 : (bx + 1 >= wmb ? wmb - 1 : bx + 1);
    int y0 = by     < 0 ? 0 : (by     >= hmb ? hmb - 1 : by);
    int y1 = by + 1 < 0 ? 0 : (by + 1 >= hmb ? hmb - 1 : by + 1);
    double *r0 = grid + (size_t)y0 * wmb, *r1 = grid + (size_t)y1 * wmb;
    if (w0) r0[x0] += amt * w0 / 1024.0;
    if (w1) r0[x1] += amt * w1 / 1024.0;
    if (w2) r1[x0] += amt * w2 / 1024.0;
    if (w3) r1[x1] += amt * w3 / 1024.0;
}

/* Precompute the 15 quarter-pel subpel phase-planes of one lr_w*lr_h reference
 * (stride lw). plane[phase], phase=(fy<<2)|fx for 1..15, holds the SAME bilinear
 * blk8_satd_qp computes on the fly -- byte-identical, just hoisted out of the
 * per-candidate ME loop so every block in the frame reuses one interpolation.
 * The bilinear at (Y,X) reads integer (Y..Y+ay, X..X+ax); the last ay rows / ax
 * cols are never addressed by a valid lowres search (bounds guarantee it) so they
 * are left unfilled. */
/* Lowres-subpel probes, both off by default and both byte-identical. They price
 * the one otherwise unmeasured plane region: the 15 quarter-pel phase-planes
 * this function builds per source frame.
 *
 * Y264_LRSUB_DOUBLE=1 build every set twice. The second build writes the same
 * pixels, so the encode cannot move, and the arm's wall
 * delta is the price of ONE build with everything else
 * held fixed (the Y264_HPEL_DOUBLE device).
 * Y264_LRSUB_CENSUS=1 count, per phase, how many blk8_satd_qp reads land on
 * it and which plane rows any of them touch. The build is
 * skippable only where nothing reads.
 *
 * The census marks in blk8_satd_qp, which is on the lowres ME's inner loop, so
 * it is a plain non-atomic store behind one read-once flag: run at --threads 1.
 */
static int lrsub_double(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_LRSUB_DOUBLE"); v = s && atoi(s); }
    return v;
}
static int g_lrs_census = -1;
static unsigned long long g_lrs_reads[16];
static unsigned long long g_lrs_builds;
static unsigned char *g_lrs_row[16];
static int g_lrs_lh, g_lrs_lw;
static int lrsub_census(void)
{
    if (g_lrs_census < 0) {
        const char *s = getenv("Y264_LRSUB_CENSUS");
        g_lrs_census = s && atoi(s) ? 1 : 0;
    }
    return g_lrs_census;
}
/* Y264_LRSUB_PROBE=1: accumulated wall of every phase-plane build, the direct
 * read of the quantity the DOUBLE arm prices by difference. */
static int g_lrs_probe = -1;
static double g_lrs_probe_ms;
static unsigned long long g_lrs_probe_n;
static pthread_mutex_t g_lrs_probe_mx = PTHREAD_MUTEX_INITIALIZER;
__attribute__((destructor)) static void lrsub_probe_dump(void)
{
    if (g_lrs_probe <= 0) return;
    fprintf(stderr, "\n=== Y264_LRSUB_PROBE: %.1f ms over %llu builds ===\n",
            g_lrs_probe_ms, g_lrs_probe_n);
}
static _Atomic unsigned long long g_lrs_site[8];   /* Y264_LRSUB_CENSUS: builds per call site */
static _Atomic unsigned long long g_lrs_plan[16];   /* claim ok / claim refused / legs needing / given a slot / no slot / calls without coh */
__attribute__((destructor)) static void lrsub_census_dump(void)
{
    if (g_lrs_census <= 0 || !g_lrs_builds) return;
    unsigned long long tot = 0;
    int used = 0, rows_t = 0, rows_a = 0;
    for (int p = 1; p < 16; p++) {
        tot += g_lrs_reads[p];
        used += g_lrs_reads[p] > 0;
        if (!g_lrs_row[p]) continue;
        for (int y = 0; y < g_lrs_lh; y++) { rows_a++; rows_t += g_lrs_row[p][y] != 0; }
    }
    fprintf(stderr, "\n=== Y264_LRSUB_CENSUS (%llu sets built, %dx%d) ===\n",
            g_lrs_builds, g_lrs_lw, g_lrs_lh);
    fprintf(stderr, "  phases read %d/15   reads %llu   rows touched %d/%d (%.1f%%)\n",
            used, tot, rows_t, rows_a, rows_a ? 100.0 * rows_t / rows_a : 0.0);
    fprintf(stderr, "  planning: claim ok %llu  overflowed %llu  legs needing %llu  given a slot %llu  no slot %llu  calls without coh %llu\n",
            (unsigned long long)g_lrs_plan[0], (unsigned long long)g_lrs_plan[1], (unsigned long long)g_lrs_plan[2],
            (unsigned long long)g_lrs_plan[3], (unsigned long long)g_lrs_plan[4], (unsigned long long)g_lrs_plan[5]);
    fprintf(stderr, "  none: entered %llu times (retain sum %llu, cap sum %llu, sets sum %llu)\n",
            (unsigned long long)g_lrs_plan[6], (unsigned long long)g_lrs_plan[7], (unsigned long long)g_lrs_plan[8], (unsigned long long)g_lrs_plan[9]);
    fprintf(stderr, "  none by site: %llu %llu %llu %llu %llu\n", (unsigned long long)g_lrs_plan[10], (unsigned long long)g_lrs_plan[11],
            (unsigned long long)g_lrs_plan[12], (unsigned long long)g_lrs_plan[13], (unsigned long long)g_lrs_plan[14]);
    fprintf(stderr, "  builds by site: mbtree-shared %llu  leg-past %llu  leg-fut %llu  push-ref %llu  push-cur %llu\n",
            (unsigned long long)g_lrs_site[0], (unsigned long long)g_lrs_site[1], (unsigned long long)g_lrs_site[2],
            (unsigned long long)g_lrs_site[3], (unsigned long long)g_lrs_site[4]);
    for (int p = 1; p < 16; p++)
        fprintf(stderr, "  phase %2d (fy%d fx%d) %12llu reads %6.2f%%\n",
                p, p >> 2, p & 3, g_lrs_reads[p],
                tot ? 100.0 * g_lrs_reads[p] / tot : 0.0);
}
static void build_lr_subpel_1(pixel *const plane[16], const pixel *ref, int lw, int lh);
/* Hoisted out of build_lr_subpel: a lazy static resolved inside a function the
 * lookahead workers call is unreachable by any warm, so it first-touched on
 * whichever worker got there first. Callable, so warm_lr_statics can resolve
 * it on the main thread. */
static int lrsub_probe(void)
{
    if (g_lrs_probe < 0) {
        const char *s = getenv("Y264_LRSUB_PROBE");
        g_lrs_probe = s && atoi(s) ? 1 : 0;
    }
    return g_lrs_probe;
}

static void build_lr_subpel(pixel *const plane[16], const pixel *ref, int lw, int lh)
{
    (void)lrsub_probe();
    if (lrsub_census()) {
        g_lrs_builds++;
        g_lrs_lw = lw; g_lrs_lh = lh;
        for (int p = 1; p < 16; p++)
            if (!g_lrs_row[p]) g_lrs_row[p] = calloc((size_t)lh, 1);
    }
    double t0 = g_lrs_probe > 0 ? tprof_ms() : 0;
    build_lr_subpel_1(plane, ref, lw, lh);
    if (lrsub_double())
        build_lr_subpel_1(plane, ref, lw, lh);
    if (g_lrs_probe > 0) {
        pthread_mutex_lock(&g_lrs_probe_mx);
        g_lrs_probe_ms += tprof_ms() - t0;
        g_lrs_probe_n++;
        pthread_mutex_unlock(&g_lrs_probe_mx);
    }
}
static void build_lr_subpel_1(pixel *const plane[16], const pixel *ref, int lw, int lh)
{
    for (int fy = 0; fy < 4; fy++)
        for (int fx = 0; fx < 4; fx++) {
            int phase = (fy << 2) | fx;
            if (!phase) continue;
            int ax = fx ? 1 : 0, ay = fy ? 1 : 0;
            int w00 = (4 - fx) * (4 - fy), w10 = fx * (4 - fy);
            int w01 = (4 - fx) * fy,       w11 = fx * fy;
            pixel *out = plane[phase];
            /* Every cell of every plane is written. The last column of the
             * fx>0 phases and the last row of the fy>0 phases replicate the
             * edge (the same pixel the integer search would clamp to). They
             * used to be left unwritten, so an edge block whose subpel probe
             * landed there read whatever the buffer held -- the previous
             * anchor's edge in a reused scratch set, garbage in a fresh one --
             * which made the lowres search depend on buffer history. Found
             * 2026-09-02 when a retained plane cache changed the output. */
            for (int Y = 0; Y < lh; Y++) {
                int Y1 = Y + ay < lh ? Y + ay : lh - 1;
                const pixel *r0 = ref + Y * lw, *r1 = ref + Y1 * lw;
                pixel *o = out + Y * lw;
                for (int X = 0; X < lw - ax; X++) {
                    int v = w00 * r0[X] + w10 * r0[X + ax]
                          + w01 * r1[X] + w11 * r1[X + ax];
                    o[X] = (pixel)((v + 8) >> 4);
                }
                if (ax) {
                    int X = lw - 1;
                    int v = w00 * r0[X] + w10 * r0[X] + w01 * r1[X] + w11 * r1[X];
                    o[X] = (pixel)((v + 8) >> 4);
                }
            }
        }
}

/* SATD of the 8x8 source block against a QUARTER-pel reference position (qmx,qmy
 * in quarter-lowres-pel). Whole positions read the integer ref; sub positions read
 * the matching precomputed phase-plane (build_lr_subpel) -- the cheap analogue of
 * x264's half-pel planes; quarter precision keeps a subpel pan matching tightly so
 * the propagation fraction stays high and importance accumulates over the chain. */
/* Y264_SATDX4: route the lowres search ring through the batched 8x8 SATD.
 * Default OFF -- the kernel is correct and byte-identical (checkasm) but the
 * microbench prices the batch at 1.01x, so this is an A/B probe for the in-situ
 * question, not a shipped path. See the ring in blk8_inter_coh. */
/* Y264_GPU_RANGE: the GPU's exhaustive half-window in LOWRES whole pels. 16 is
 * what our own diamond covers (step 64 qpel = 16 whole pels), so it is the
 * like-for-like setting; 8 is the cheaper operating point (0.53 ms vs 1.52 ms
 * per leg at 720p) and is the default because several legs run per frame. */
static int gpu_range(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_GPU_RANGE"); v = s ? atoi(s) : 8;
                 if (v < 1) v = 1; if (v > 64) v = 64; }
    return v;
}

static int satdx4_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_SATDX4"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Y264_LR_SADINT: score the lowres search's INTEGER-pel levels (step >= 4
 * quarter-pel) with SAD and only the half/quarter-pel levels with SATD --
 * x264's lowres shape (the cheap compare for the integer walk, the full
 * metric for the subpel refinement). Our lowres search scored every probe of a seven-level diamond
 * with SATD against the phase planes: 9.5G SATD pixels on sunflower_1080p
 * against x264's 2.3G for its whole lookahead, while outside the lookahead the
 * two encoders' SATD volumes are equal. DEFAULT ON (2026-09-02), priced with
 * Y264_LR_SHAPE and Y264_LR_PREV_SHAPE as one change; see lr_prev_shape_on. */
static int lr_sadint_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_LR_SADINT"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
static long blk8_sad_qp(const pixel *sb, int ss, const pixel *ref, int rs,
                        pixel *const subpel[16], int bx, int by, int qmx, int qmy)
{
    int ix = qmx >> 2, iy = qmy >> 2, fx = qmx & 3, fy = qmy & 3;
    int phase = (fy << 2) | fx;
    const pixel *base = phase ? subpel[phase] : ref;
    return y264_dsp.sad[Y264_PU_8x8](sb, ss, base + (by + iy) * rs + bx + ix, rs);
}
static long blk8_satd_qp(const pixel *sb, int ss, const pixel *ref, int rs,
                         pixel *const subpel[16], int bx, int by, int qmx, int qmy)
{
    int ix = qmx >> 2, iy = qmy >> 2, fx = qmx & 3, fy = qmy & 3;
    int phase = (fy << 2) | fx;
    const pixel *base = phase ? subpel[phase] : ref;
    if (phase && g_lrs_census > 0) {
        g_lrs_reads[phase]++;
        if (g_lrs_row[phase])
            for (int y = by + iy; y < by + iy + 8 && y < g_lrs_lh; y++)
                if (y >= 0) g_lrs_row[phase][y] = 1;
    }
    return blk8_satd(sb, ss, base + (by + iy) * rs + bx + ix, rs);
}

/* Coherent + quarter-pel lowres ME for mb-tree. The SEARCH minimises
 * SATD + lambda*mvbits(mv - predictor) seeded from the predictor (coherent MV
 * field on pans), and refines to QUARTER-pel (x264's lowres precision) so a subpel
 * pan matches tightly -- else the residual inflates inter cost, cuts the
 * propagation fraction, and mb-tree importance decays over the chain instead of
 * accumulating (why yah264 gained little on motion vs x264). All MVs in
 * QUARTER-lowres-pel; returns the cost INCLUDING mvcost . */
/* gseed: an integer-pel MV from the GPU's exhaustive window (gvalid != 0), in
 * LOWRES WHOLE pels. It is taken as one more seed alongside {predictor, above,
 * zero} and then the diamond starts at step 2 instead of 64 -- the GPU has
 * already searched the wide levels, exhaustively, so re-walking them can only
 * find the same minimum more slowly.
 *
 * The seeds are kept rather than replaced because the GPU searches at lambda 0:
 * it reports where the DISTORTION minimum is, and the rate-aware choice between
 * that and a cheaper-to-code neighbour MV stays here, with the encoder's own
 * rate model. Dropping the predictor seed would quietly change what the search
 * optimises. */
static long blk8_inter_coh(const pixel *sb, int ss, const pixel *ref, int rs,
                           pixel *const subpel[16], int lw, int lh, int bx, int by,
                           int predx, int predy, int ax, int ay, double mvlambda,
                           int gseedx, int gseedy, int gvalid,
                           int *outmvx, int *outmvy)
{
    int xmin = -4 * bx, xmax = 4 * (lw - 8 - bx);        /* quarter-pel MV bounds */
    int ymin = -4 * by, ymax = 4 * (lh - 8 - by);
    int satdx4 = satdx4_env();
#define CLQ(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
    const long *ctab = coh_tab(mvlambda);                /* TLS resolved once */
    int sadint = lr_sadint_on() && !gvalid;               /* integer levels by SAD */
    int sadnow = sadint;                                    /* the metric of the CURRENT level */
#define COH_COST(mx, my) ((sadnow ? blk8_sad_qp(sb, ss, ref, rs, subpel, bx, by, (mx), (my)) \
                                  : blk8_satd_qp(sb, ss, ref, rs, subpel, bx, by, (mx), (my))) \
        + coh_rate(ctab, mvlambda, lowres_mvbits(((mx) - predx) >> 2) + lowres_mvbits(((my) - predy) >> 2)))
    /* Seed the diamond from the best of {left, above, zero} -- a coherent field on
 * smooth motion but able to switch on divergent (zoom) motion where the left
 * neighbour is a poor predictor. mvcost still measures deviation from the left.
 * Duplicate seeds (post-clamp) are exact re-probes: identical cost, strict-<
 * acceptance -> skipping them changes nothing. */
    int cx = CLQ(predx, xmin, xmax), cy = CLQ(predy, ymin, ymax);
    int s0x = cx, s0y = cy;
    long best = COH_COST(cx, cy);
    int t1x = CLQ(ax, xmin, xmax), t1y = CLQ(ay, ymin, ymax);
    if (t1x != s0x || t1y != s0y) {
        long c = COH_COST(t1x, t1y);
        if (c < best) { best = c; cx = t1x; cy = t1y; }
    }
    if (0 >= xmin && 0 <= xmax && 0 >= ymin && 0 <= ymax &&
        (s0x || s0y) && (t1x || t1y)) {                  /* zero-MV candidate */
        long z = COH_COST(0, 0);
        if (z < best) { best = z; cx = 0; cy = 0; }
    }
    if (gvalid) {
        int gx = CLQ(gseedx * 4, xmin, xmax), gy = CLQ(gseedy * 4, ymin, ymax);
        long gc = COH_COST(gx, gy);
        if (gc < best) { best = gc; cx = gx; cy = gy; }
    }
    /* The GPU already covered the wide levels exhaustively; start at the subpel
 * ones. Without a GPU seed this is the full 64..1 walk, unchanged. */
    for (int step = gvalid ? 2 : 64; step >= 1; step >>= 1) {
        if (sadnow && step < 4) {          /* subpel levels: back to SATD, re-score the centre */
            sadnow = 0;
            best = COH_COST(cx, cy);
        }
        /* Opposite-point skip: after a move along cand[k], the opposite
 * candidate (k^1: the pairs are +x/-x, +y/-y) IS the previous centre,
 * whose cost was already evaluated and is >= best (strict-<
 * acceptance) -- an exact re-probe, skipping it changes nothing.
 * Reset per step level (the halved ring is all-new positions). */
        int skipk = -1;
        for (int iter = 0; iter < 8; iter++) {
            int cand[4][2] = { {cx + step, cy}, {cx - step, cy}, {cx, cy + step}, {cx, cy - step} };
            int moved = 0, acc = -1;
            /* Batched ring (Y264_SATDX4): score every live candidate through
 * y264_satd_x4_8x8 BEFORE the ordered strict-< comparisons below.
 * A candidate's SATD does not depend on the running best, so the
 * comparisons see the identical numbers in the identical order --
 * the probe_int_list argument, one metric up, and the reason this is
 * byte-identical rather than merely equivalent.
 *
 * The microbench says the batch is worth 1% (satd is arithmetic-
 * bound, so sharing the source loads buys almost nothing and four
 * back-to-back singles already interleave). This exists to test the
 * one thing the microbench cannot: whether the ENCODER's singles
 * interleave as well, given they are separated by rate lookups,
 * bounds tests and a branch. */
            long ring[4];
            if (satdx4 && !sadnow) {
                const pixel *rp[4]; int rk[4], nr = 0;
                for (int k = 0; k < 4; k++) {
                    ring[k] = LONG_MAX;
                    if (k == skipk) continue;
                    int mx = cand[k][0], my = cand[k][1];
                    if (mx < xmin || mx > xmax || my < ymin || my > ymax) continue;
                    int ix = mx >> 2, iy = my >> 2, fx = mx & 3, fy = my & 3;
                    int phase = (fy << 2) | fx;
                    const pixel *base = phase ? subpel[phase] : ref;
                    if (phase && g_lrs_census > 0) {     /* keep the census identical */
                        g_lrs_reads[phase]++;
                        if (g_lrs_row[phase])
                            for (int y = by + iy; y < by + iy + 8 && y < g_lrs_lh; y++)
                                if (y >= 0) g_lrs_row[phase][y] = 1;
                    }
                    rp[nr] = base + (by + iy) * rs + bx + ix;
                    rk[nr] = k; nr++;
                }
                int j = 0;
                if (nr == 4 && y264_dsp.satd_x4_8x8) {
                    int sc[4];
                    y264_dsp.satd_x4_8x8(sb, ss, rp[0], rp[1], rp[2], rp[3], rs, sc);
                    for (; j < 4; j++) ring[rk[j]] = sc[j];
                }
                for (; j < nr; j++)
                    ring[rk[j]] = blk8_satd(sb, ss, rp[j], rs);
                for (int k = 0; k < 4; k++)
                    if (ring[k] != LONG_MAX)
                        ring[k] += coh_rate(ctab, mvlambda,
                                            lowres_mvbits((cand[k][0] - predx) >> 2) +
                                            lowres_mvbits((cand[k][1] - predy) >> 2));
            }
            for (int k = 0; k < 4; k++) {
                if (k == skipk) continue;
                int mx = cand[k][0], my = cand[k][1];
                if (mx < xmin || mx > xmax || my < ymin || my > ymax) continue;
                long c = (satdx4 && !sadnow) ? ring[k] : COH_COST(mx, my);
                if (c < best) { best = c; cx = mx; cy = my; moved = 1; acc = k; }
            }
            if (!moved) break;
            skipk = acc ^ 1;
        }
    }
#undef COH_COST
#undef CLQ
    *outmvx = cx; *outmvy = cy;                          /* quarter-pel units */
    return best;
}

/* mb-tree source/anchor descriptors, lifted to file scope so the parallel Phase-A
 * worker (mbt_pa_source) can see them. */
struct mbt_anchor { int poc; const pixel *lr; const int32_t *dintra; pixel *const *full;
                    long push; };
struct mbt_source { int poc, is_anchor, bbuf, laidx, laoff; const pixel *lr;
                    const int32_t *dintra; pixel *const *full; long push; };

/* Phase-A context: the per-source lowres ME (the expensive part of mb-tree) is
 * pixel-only and independent per source frame, so it runs on the pool. Each source
 * writes its own disjoint slice of the pa_* result arrays; the float splat that
 * accumulates onto the shared prop grids is done serially afterwards (Phase B),
 * preserving the exact accumulation order -> byte-identical to the fused loop. */
struct mbt_pa_ctx {
    yah264_encoder_t *e;
    const struct mbt_source *src;
    int nmb, wmb, hmb, lw, lh, coh;
    double mvlambda;
    long cost_inf;
    const int *s_past, *s_fut;          /* per-source anchor slots (self via s_self) */
    const int *s_pastpoc, *s_futpoc;    /* past/fut anchor POCs (memo key; INT_MIN = none) */
    const pixel *const *s_pastlr, *const *s_futlr;
    /* GPU integer-search fields, one per source per leg (NULL = CPU search).
 * Filled by ONE batch before the parallel_for, because an ngc_stream is
 * single-threaded by contract and mbt_pa_source runs on the pool. */
    int16_t *const *gpu0, *const *gpu1;
    /* gpq (Y264_GPU_PHASEA): per-source past/future anchor PUSH indices, the
 * field-lookup key. NULL = gpq not in play this call. gpq_maxpush is the
 * caller's chain-wait bound (gpu.h): the highest push_idx among the
 * entries it enumerated. */
    const long *s_pastpush, *s_futpush;
    long gpq_maxpush;
    /* Per-source slot in the shared anchor subpel cache, or -1 to build into
 * this worker's own set. NULL = no cache this call (it was claimed). */
    const int *s_sub0, *s_sub1;
    /* Per-source result pointers: each source's slice lives in its OWNER's memo
 * arrays (ring entry or bbuf slot), computed in place on a miss and read
 * directly by Phase B -- no scratch slabs, no hit/store memcpys. */
    long  *const *pp_pi, *const *pp_pin;
    signed char *const *pp_plu;
    int   *const *pp_pmv;
    double *const *pp_psw;
    const unsigned char *need;          /* 1 = memo miss: compute this source */
    /* Highest ring OFFSET whose B pair legs are settled, i.e. that no in-flight
 * la_finalize can still be writing. Each caller derives it from the chain
 * step IT waited for. INT_MAX when there is no lookahead THREAD: the chain
 * then steps inline on this very thread, so nothing is in flight and the
 * serial path stays byte-identical. Two readers respect it -- mbt_pair_seed
 * and the bleg reuse test in mbt_pa_source. */
    int settled_off;
};

/* --- the shared anchor subpel cache ---------------------------------------
 *
 * Every source's lowres ME searches its bracketing ANCHORS, and the 15
 * quarter-pel phase-planes of an anchor are a pure function of that anchor's
 * lowres. Built per SOURCE into per-WORKER scratch instead, at 18 threads the
 * same set exists up to 18 times and 720p holds 124 MB of it -- the largest
 * resident term in the encoder after the input. The distinct anchors in a
 * window are ~11.
 *
 * One Phase A at a time owns the cache. mbt_pre's warm pass and the driver's
 * own walk can both reach Phase A, and rather than reason about whether they
 * ever overlap, the loser falls back to the per-worker sets.
 * Both paths compute the same bilinear either way, so the arm is byte-identical
 * whichever way the claim goes.
 */
static pthread_mutex_t g_mbt_sub_mx = PTHREAD_MUTEX_INITIALIZER;

static int mbt_sub_claim(yah264_encoder_t *e)
{
    int ok;
    pthread_mutex_lock(&g_mbt_sub_mx);
    ok = !e->mbt_sub_busy;
    if (ok) e->mbt_sub_busy = 1;
    pthread_mutex_unlock(&g_mbt_sub_mx);
    return ok;
}

static void mbt_sub_release(yah264_encoder_t *e)
{
    pthread_mutex_lock(&g_mbt_sub_mx);
    e->mbt_sub_busy = 0;
    pthread_mutex_unlock(&g_mbt_sub_mx);
}

/* Grow the cache to n sets. Returns how many are usable (<= n). */
static int mbt_sub_grow(yah264_encoder_t *e, int n)
{
    size_t lrsz = (size_t)e->lr_w * e->lr_h;
    while (e->mbt_sub_n < n) {
        int i = e->mbt_sub_n;
        for (int p = 1; p < 16; p++)
            if (!(e->mbt_sub[i][p] = malloc(lrsz * sizeof(pixel)))) {
                for (int q = 1; q < p; q++) { free(e->mbt_sub[i][q]); e->mbt_sub[i][q] = NULL; }
                return i;               /* OOM: a shorter cache still works */
            }
        e->mbt_sub_stamp[i] = -1; e->mbt_sub_use[i] = 0;
        e->mbt_sub_n = i + 1;
    }
    return n;
}

struct mbt_sub_ctx { yah264_encoder_t *e; int lw, lh; const int *slots; };

static void mbt_sub_build_one(void *ctx, int tid, int k)
{
    struct mbt_sub_ctx *c = ctx;
    (void)tid;
    int i = c->slots[k];
    if (g_lrs_census > 0) g_lrs_site[0]++; build_lr_subpel(c->e->mbt_sub[i], c->e->mbt_sub_key[i], c->lw, c->lh);
}

/* Key the distinct anchor lowres planes this Phase A will search, build the
 * ones the cache does not already hold, and hand each source the slot its
 * past/future leg lands in (-1 = build into the worker's own set, as before).
 * Returns the number of sets BUILT this call.
 *
 * RETAINED ACROSS CALLS (2026-09-02). A set is a pure function of one anchor's
 * lowres, and an anchor stays in the window for about ten consecutive Phase A
 * calls, so a cache emptied every call rebuilt each set about ten times; at
 * one thread the old cap of 2 x workers never fit the ~11 anchors at all and
 * every source built both of its legs privately -- 231 ms of sunflower_1080p's
 * 590 ms Phase A. The key is (lowres pointer, push index): the ring reuses a
 * slot's pointer for a later frame, and the push index is what tells them
 * apart. Eviction is LRU by call, never of a set this call needs.
 *
 * Still all or nothing within a call: if the anchors do not fit, every source
 * takes the per-worker path, as before, and the cache keeps what it had. */
/* Y264_MBT_SUB_RETAIN=0 restores the per-call cache (emptied every call,
 * capped at 2 x workers) for A/B; the planes are the same either way. */
/* Y264_MBT_SUB_PARTIAL (default 1): when a call's legs do not all fit the
 * cache, keep the slots that were assigned and build them, and let only the
 * legs left over take the per-worker path. The all-or-nothing form threw the
 * whole call to the private path on overflow: sunflower t12 read 238 of 527
 * legs without a slot (Y264_LRSUB_CENSUS "planning" line) with the cache
 * never refused, so every private build was an overflow. Same planes either
 * way, byte-identical. =0 restores all-or-nothing for A/B. */
static int mbt_sub_partial_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_SUB_PARTIAL"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
/* Y264_MBT_SUB_VERIFY: rebuild-and-compare probe inside the parallel_for body;
 * a warmed static, not a per-source getenv on the workers. Default off. */
static int mbt_sub_verify_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_MBT_SUB_VERIFY"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}
static int mbt_sub_retain_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_SUB_RETAIN"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

static int mbt_sub_plan(yah264_encoder_t *e, int ns, const unsigned char *need,
                        const pixel *const *pastlr, const pixel *const *futlr,
                        const long *pastpush, const long *futpush,
                        int *sub0, int *sub1, int nws)
{
    int retain = mbt_sub_retain_on();
    int cap = Y264_MBT_SUB_MAX;
    if (!retain) {
        cap = 2 * (nws > 0 ? nws : 1);
        if (cap > Y264_MBT_SUB_MAX) cap = Y264_MBT_SUB_MAX;
        for (int i = 0; i < e->mbt_sub_n; i++) e->mbt_sub_stamp[i] = -1;
    }
    unsigned call = ++e->mbt_sub_call;
    int fresh[Y264_MBT_SUB_MAX], nfresh = 0;
    for (int s = 0; s < ns; s++) sub0[s] = sub1[s] = -1;
    for (int s = 0; s < ns; s++) {
        if (!need[s])
            continue;
        const pixel *want[2] = { pastlr[s], futlr[s] };
        /* A push index of 0 is "unknown" (the current anchor's rq->anchor_push
         * is only filled for gpq); such a leg gets a stamp no later call can
         * match, so it is built fresh every call rather than aliased. */
        long stamp[2] = { pastpush && pastpush[s] > 0 ? pastpush[s] : -(long)call - 1,
                          futpush  && futpush[s]  > 0 ? futpush[s]  : -(long)call - 1 };
        int *slot[2] = { &sub0[s], &sub1[s] };
        for (int h = 0; h < 2; h++) {
            if (!want[h])
                continue;
            for (int i = 0; i < e->mbt_sub_n; i++)
                if (e->mbt_sub_key[i] == want[h] && e->mbt_sub_stamp[i] == stamp[h]) {
                    *slot[h] = i; e->mbt_sub_use[i] = call; break;
                }
            if (*slot[h] >= 0)
                continue;
            /* Assign: an unallocated slot first, else the least recently used
             * set no source of THIS call has claimed. */
            int pick = -1;
            if (!retain) {                      /* old scheme: fill in order, no eviction */
                int used = 0;
                for (int i = 0; i < e->mbt_sub_n; i++) used += e->mbt_sub_use[i] == call;
                if (used >= cap) { if (g_lrs_census > 0) g_lrs_plan[10 + 0]++; goto none; }
            }
            if (e->mbt_sub_n < cap) {
                /* The target count is taken BEFORE the call: the old form
 * compared the call's return with e->mbt_sub_n + 1 evaluated after
 * the call had already grown it, so every successful grow read as an
 * OOM and threw the whole call to the per-worker path (sunflower
 * t12: 12 of 39 calls, 238 of 527 legs, found 2026-09-04 with the
 * census). */
                int want_n = e->mbt_sub_n + 1;
                if (mbt_sub_grow(e, want_n) < want_n)
                    { if (g_lrs_census > 0) g_lrs_plan[10 + 1]++; goto none; }                  /* OOM: a shorter cache still works */
                pick = e->mbt_sub_n - 1;
            } else {
                unsigned oldest = call;
                for (int i = 0; i < e->mbt_sub_n; i++)
                    if (e->mbt_sub_use[i] != call && e->mbt_sub_use[i] < oldest) {
                        oldest = e->mbt_sub_use[i]; pick = i;
                    }
                if (pick < 0) {                 /* does not fit */
                    if (g_lrs_census > 0) g_lrs_plan[1]++;
                    if (mbt_sub_partial_on() && retain) goto partial;
                    { if (g_lrs_census > 0) g_lrs_plan[10 + 2]++; goto none; }                  /* all-or-nothing: no cache at all */
                }
            }
            e->mbt_sub_key[pick] = want[h];
            e->mbt_sub_stamp[pick] = stamp[h];
            e->mbt_sub_use[pick] = call;
            fresh[nfresh++] = pick;
            *slot[h] = pick;
        }
    }
partial:
    if (nfresh > 0) {
        struct mbt_sub_ctx c = { e, e->lr_w, e->lr_h, fresh };
        if (e->pool && ntp_pool_nthreads(e->pool) > 1 && nfresh > 1) {
            ntp_prof_tag("mbtree_subpel"); ntp_prio_hint();
            ntp_parallel_for(e->pool, nfresh, mbt_sub_build_one, &c);
        } else
            for (int i = 0; i < nfresh; i++) mbt_sub_build_one(&c, 0, i);
    }
    return nfresh;
none:
    if (g_lrs_census > 0) { g_lrs_plan[6]++; g_lrs_plan[7] += (unsigned long long)retain; g_lrs_plan[8] += (unsigned long long)cap; g_lrs_plan[9] += (unsigned long long)e->mbt_sub_n; }
    for (int s = 0; s < ns; s++) sub0[s] = sub1[s] = -1;
    /* Slots assigned this call before the overflow keep their keys but were
     * not built: make sure a later call cannot take them for a hit. */
    for (int i = 0; i < nfresh; i++) e->mbt_sub_stamp[fresh[i]] = -1;
    return 0;
}

/* Allocate (once) per-worker Phase-A scratch sized to the pool thread count. */
static int mbt_ensure_ws(yah264_encoder_t *e, int nws)
{
    if (nws <= e->mbt_nws)
        return 1;
    if (nws > 64) return 0;                              /* struct cap: serial fallback */
    size_t lrsz = (size_t)e->lr_w * e->lr_h;
    int nmb = e->width_in_mbs * e->height_in_mbs;
    for (int w = e->mbt_nws; w < nws; w++) {
        for (int p = 1; p < 16; p++) {
            e->mbt_subpel[w][0][p] = malloc(lrsz * sizeof(pixel));
            e->mbt_subpel[w][1][p] = malloc(lrsz * sizeof(pixel));
            if (!e->mbt_subpel[w][0][p] || !e->mbt_subpel[w][1][p]) return 0;
        }
        e->mbt_lrtmp[w] = malloc(lrsz * sizeof(pixel));
        e->mbt_invq[w]  = malloc((size_t)nmb * sizeof(double));
        e->mbt_aqoff[w] = malloc((size_t)nmb * sizeof(float));
        if (!e->mbt_lrtmp[w] || !e->mbt_invq[w] || !e->mbt_aqoff[w]) return 0;
        e->mbt_nws = w + 1;
    }
    return 1;
}

/* Y264_MBT_SPLIT accounting (see mbt_split_env, printed at close). Declared
 * here because Phase A, above the probe's other users, records its reuse
 * coverage into it. */
static struct { double invq, bind, pa, pb, fin; long calls, srcs, misses;
                long wcalls, wsrcs, wdone, mfresh, mpast, mfut; long mpos[64];
                /* Phase-A REUSE COVERAGE: of the sources that actually run a
 * search, how many took the lookahead's pair fields instead.
 * `anc` and `noring` are
 * the two structural exclusions; `nokey` is a leaf whose bleg
 * fields were computed against different anchors. */
                _Atomic long pa_reuse, pa_scaled, pa_anc, pa_noring, pa_nokey;
                /* WHY a nokey source could not be seeded, so the residual is
 * attributable rather than a single lump: unsettled = past the
 * A1 settled bound, nobleg = the entry never grew pair legs,
 * norange = legs exist but the walk's brackets need
 * EXTRAPOLATION (num > den), which the scale refuses. */
                _Atomic long pa_unsettled, pa_nobleg, pa_norange;
                _Atomic long pa_gpu2, pa_gpu1;
                /* per-group wall (t1 only: plain doubles), setup vs block loop */
                double pa_ms[8], pa_setup_ms, pa_ds_ms, pa_invq_ms, pa_sub_ms; long pa_n[8];
                long sub_built, sub_hit; } g_mbt_split;
static int mbt_split_env(void);
static int gpq_consume_on(void);

/* Symmetric-rounding scale of one MV component: sign-independent, so a seed and
 * its mirror image round the same way. */
static int mv_scale(int v, int num, int den)
{
    int a = v < 0 ? -v : v;
    int r = (a * num + den / 2) / den;
    return v < 0 ? -r : r;
}

/* Y264_MBT_PAIR_SCALE: derive Phase A's reuse seed for a bracketing pair the
 * lookahead did not search, by scaling the pair it did. DEFAULT ON: Phase A
 * 418 -> 248 ms on the samsung cell, +3.3% of wall at t1 and +3.4% at t18, and
 * the CRF band moves the quality's way rather than against it -- 11 of 12 clips between -0.03% and
 * -0.63%, touchdown the lone +0.20%. It moves bits (a 3-candidate eval on a
 * derived seed replaces a diamond, exactly as Y264_MBT_BLEG_REUSE does), so
 * Y264_MBT_PAIR_SCALE=0 is the escape. Resolved in warm_lr_statics. */
static int pair_scale_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_PAIR_SCALE"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Fill out[4][nmb] (l0x, l0y, l1x, l1y, lowres qpel) with seeds for the pair the
 * walk wants, from the pair the lookahead searched. The stored pair lives in the
 * source's la entry while it is in the ring and in e->bseed[] after its pop
 * (full-res qpel there, exactly 2x lowres, because stash_lr_seed doubles it).
 *
 * Scaling is the same distance-scaling geometry x264 uses: a leg is proportional to the
 * temporal distance it spans, so leaf->target = leaf->stored * (distance to
 * target) / (distance to stored). Only a target BETWEEN the source and its
 * stored anchor on the same side is derivable -- extrapolating past the anchor
 * would amplify the error rather than shrink it -- and num == den reproduces the
 * stored MV exactly, which is what keeps the already-covered sources identical.
 * Returns 1 if either leg came out usable. */
static int mbt_unsafe_nosettle(void);
static void la_th_wait(yah264_encoder_t *e, long need);
static int mbt_settle_wait_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_SETTLE_WAIT"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
static int mbt_pair_seed(yah264_encoder_t *e, struct mbt_pa_ctx *c, int s,
                         int nmb, int16_t *out, int *have0, int *have1)
{
    const y264_lr_blk *l0 = NULL, *l1 = NULL;      /* ring flavour */
    const int16_t *b0x = NULL, *b0y = NULL, *b1x = NULL, *b1y = NULL;  /* popped */
    int p0, p1;
    *have0 = *have1 = 0;
    if (c->src[s].laidx >= 0) {
        /* Only entries whose B pair legs are FINAL may be read, and the bound
 * has to be a function of the configuration rather than of a progress
 * counter. `la_th_wait_mbtree` waits for chain step pop_seq+la_depth-1,
 * which types ring offsets up to la_depth-3; an entry's legs, though,
 * are written later still -- by the finalize of the next anchor AFTER
 * it, which reaches back bframes+1 entries (lowres_bleg_me, called from
 * la_finalize after that anchor's own `typed = 1`). So an entry is
 * settled only up to la_depth-4-bframes, and a source past that has its
 * bleg_have flipping under the walk.
 *
 * Reading past that bound emits 3-5 distinct bitstreams in 12 runs of one
 * config (foreman --ref 1 t18), and this reader is where it bites: it
 * accepts ANY stored pair and scales it, where the exact-key reuse path
 * fails closed on a pair that does not match. The alternative fix --
 * widening the driver's wait to cover the reach-back -- also works and
 * costs 10.8% of samsung's wall, so this declines instead and lets the
 * frontier sources search. */
        if (c->src[s].laoff > c->settled_off && !mbt_unsafe_nosettle()) {
            if (mbt_split_env())
                atomic_fetch_add_explicit(&g_mbt_split.pa_unsettled, 1, memory_order_relaxed);
            return 0;
        }
        struct la_entry *ce = &e->la[c->src[s].laidx];
        if (!ce->bleg_have || !ce->leg[LR_LEG_ANCHOR] || !ce->leg[LR_LEG_NEXT]) {
            if (mbt_split_env())
                atomic_fetch_add_explicit(&g_mbt_split.pa_nobleg, 1, memory_order_relaxed);
            return 0;
        }
        l0 = ce->leg[LR_LEG_ANCHOR]; l1 = ce->leg[LR_LEG_NEXT];
        p0 = ce->bleg_poc0; p1 = ce->bleg_poc1;
    } else if (c->src[s].bbuf >= 0) {
        int b = c->src[s].bbuf;
        if (!e->bseed_valid[b] || !e->bseed[b][0] || !e->bseed[b][3])
            return 0;
        b0x = e->bseed[b][0]; b0y = e->bseed[b][1];
        b1x = e->bseed[b][2]; b1y = e->bseed[b][3];
        p0 = e->bseed_poc0[b]; p1 = e->bseed_poc1[b];
    } else return 0;

    int p = c->src[s].poc, w0 = c->s_pastpoc[s], w1 = c->s_futpoc[s];
    int den0 = p - p0, num0 = p - w0;
    int den1 = p1 - p, num1 = w1 - p;
    int ok0 = den0 > 0 && num0 > 0 && num0 <= den0;
    int ok1 = den1 > 0 && num1 > 0 && num1 <= den1;
    if (!ok0 && !ok1) {
        if (mbt_split_env())
            atomic_fetch_add_explicit(&g_mbt_split.pa_norange, 1, memory_order_relaxed);
        return 0;
    }
    for (int i = 0; i < nmb; i++) {
        if (ok0) {
            int x = l0 ? l0[i].mvx : b0x[i] / 2, y = l0 ? l0[i].mvy : b0y[i] / 2;
            out[i]       = (int16_t)mv_scale(x, num0, den0);
            out[nmb + i] = (int16_t)mv_scale(y, num0, den0);
        }
        if (ok1) {
            int x = l1 ? l1[i].mvx : b1x[i] / 2, y = l1 ? l1[i].mvy : b1y[i] / 2;
            out[2 * nmb + i] = (int16_t)mv_scale(x, num1, den1);
            out[3 * nmb + i] = (int16_t)mv_scale(y, num1, den1);
        }
    }
    *have0 = ok0; *have1 = ok1;
    return 1;
}

/* Phase A for one source frame s: lowres ME (+ subpel legs, AQ weight) into the
 * pa_* arrays. Identical arithmetic and predictor chain to the fused serial loop;
 * only the splat is deferred. Runs on pool worker `tid` (its private scratch). */
static int bleg_reuse_on(void);
/* Y264_MBT_LEGTRUST=1 (plan item E1): on an EXACT-key reuse (the lookahead
 * searched this very pair), take the stored leg's SATD and MV as the leg's
 * cost, plus the walk's own MV rate against its chained predictor, instead
 * of re-evaluating three candidates ({leg MV, chained predictor, zero})
 * with fresh SATDs. The cached-cost model a single-lookahead encoder runs:
 * every motion-search cost computed once, the propagation walk only reads.
 * Drops the rescue the 3-candidate eval gives mispriced legs on chaotic
 * motion, so it is output-changing and band-gated. Not for the pair-scaled
 * seeds (a derived seed still needs its SATD) or the GPU fields (which
 * already carry theirs).
 * DEFAULT ON (2026-09-03). MBT_SPLIT on sunflower_1080p t12: the reuse
 * group's evaluation 108 -> 8 ms. Walls, medians of 5 round-robin with a
 * control inside 0.6%: t12 sunflower -1.6%, pedestrian -0.8%, samsung /
 * park_joy / foreman -0.6%, shields 0.0%; t1 -0.2..-1.0%. CRF band, 14
 * clips, 150f, t1: median +0.04% (stq order) / +0.07% (threaded), worst
 * coastguard +0.50 / stockholm +0.63, best station2 -0.52 / coastguard
 * -0.59. Record: local/records/e1-legtrust-2026-09-03.md. =0 escapes to the
 * 3-candidate eval. */
static int mbt_legtrust_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_LEGTRUST"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
static int mbt_areuse_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_AREUSE"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
/* Y264_DIRECT_LRVOTE: lowres simulation of the two B direct derivations, scored
 * against the lookahead's own motion result. Measurement only -- see the Phase B
 * block that reads these. LRVOTE_CZ is the colZeroFlag threshold, in whatever
 * units lowres MV field carries (quarter-pel under the coherent search,
 * integer otherwise), which is why it is a knob rather than the spec's 1. */
static int direct_lrvote_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_DIRECT_LRVOTE"); v = e ? atoi(e) : 0; }
    return v;
}
static int direct_lrvote_cz(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_DIRECT_LRVOTE_CZ"); v = e ? atoi(e) : 1; }
    return v;
}
/* Average the two lowres references at the given (quarter- or integer-pel) MVs
 * into an 8x8 block, integer-pel. Returns 0 if either read leaves the plane --
 * the caller then abstains rather than clamping, so neither derivation is
 * scored on a fabricated prediction. */
static int lr_bipred(pixel *dst, const pixel *srclr, const pixel *plr,
                     const pixel *flr, int lw, int lh, int mx, int my,
                     int m0x, int m0y, int m1x, int m1y, int qshift)
{
    (void)srclr;
    int x0 = mx * 8 + (m0x >> qshift), y0 = my * 8 + (m0y >> qshift);
    int x1 = mx * 8 + (m1x >> qshift), y1 = my * 8 + (m1y >> qshift);
    if (x0 < 0 || y0 < 0 || x0 + 8 > lw || y0 + 8 > lh) return 0;
    if (x1 < 0 || y1 < 0 || x1 + 8 > lw || y1 + 8 > lh) return 0;
    const pixel *a = plr + y0 * lw + x0, *b = flr + y1 * lw + x1;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            dst[y * 8 + x] = (pixel)((a[y * lw + x] + b[y * lw + x] + 1) >> 1);
    return 1;
}

static int med3(int a, int b, int c)
{
    return a > b ? (b > c ? b : (a > c ? c : a))
                 : (a > c ? a : (b > c ? c : b));
}
static void mbt_pa_source(void *ctx, int tid, int s)
{
    struct mbt_pa_ctx *c = ctx;
    yah264_encoder_t *e = c->e;
    NLED_SITE(Y264_LED_SITE_LRPA);
    if (c->need[s]) NLED(leg_pa, 1 + (c->s_futlr[s] != NULL));
    int nmb = c->nmb, wmb = c->wmb, hmb = c->hmb, lw = c->lw, lh = c->lh, coh = c->coh;
    double mvlambda = c->mvlambda;
    pixel *const *subpel0 = e->mbt_subpel[tid][0];
    pixel *const *subpel1 = e->mbt_subpel[tid][1];
    double *invq_s = e->mbt_invq[tid];
    float  *aqoff_s = e->mbt_aqoff[tid];

    /* Memo hit (need==0): this source's slice was already computed against the
 * same bracketing anchors -- either in an earlier anchor's mb-tree (ring
 * sources; the window overlaps) or carried out of the ring with a buffered
 * B. The slice is a pure function of (frame, past-anchor, fut-anchor) and
 * Phase B reads it in place, so there is nothing to do here. */
    if (!c->need[s])
        return;
    double pa_t0 = mbt_split_env() ? tprof_ms() : 0.0, pa_t1 = 0.0; int pa_g = 7;
    long *pi = c->pp_pi[s];
    long *pin = c->pp_pin[s];
    signed char *plu = c->pp_plu[s];
    int *pmv = c->pp_pmv[s];
    double *psw = c->pp_psw[s];

    const pixel *flr;
    double pa_ta = pa_t0 ? tprof_ms() : 0.0;
    if (c->src[s].bbuf >= 0) {
        downscale(e->mbt_lrtmp[tid], lw, lh, c->src[s].full[0], e->pstride[0]);
        flr = e->mbt_lrtmp[tid];
    } else flr = c->src[s].lr;
    double pa_tb = pa_t0 ? tprof_ms() : 0.0;
    mbtree_invqscale(e, c->src[s].full, e->pstride[0], wmb, hmb, e->aq_strength, invq_s, aqoff_s);
    double pa_tc = pa_t0 ? tprof_ms() : 0.0;
    if (pa_t0) { g_mbt_split.pa_ds_ms += pa_tb - pa_ta; g_mbt_split.pa_invq_ms += pa_tc - pa_tb; }

    const pixel *pastlr = c->s_pastlr[s];
    const pixel *futlr  = c->s_futlr[s];
    const int16_t *g0 = c->gpu0 ? c->gpu0[s] : NULL;
    const int16_t *g1 = c->gpu1 ? c->gpu1[s] : NULL;
    /* gpq (Y264_GPU_PHASEA): per-leg quarter-pel fields computed at PUSH time.
 * A covered leg needs no CPU search and no subpel planes -- the per-block
 * eval below prices the GPU's two candidates (its qpel best and zero)
 * with the chained predictor and the encoder's own rate model. */
    int gpq_consume = gpq_consume_on();  /* warmed at open; see the helper */
    const y264_gpq_blk *q0 = NULL, *q1 = NULL;
    if (e->gpq && gpq_consume && coh && c->s_pastpush && c->src[s].push > 0) {
        if (pastlr)
            q0 = y264_gpq_field(e->gpq, c->src[s].push, c->s_pastpush[s],
                                c->gpq_maxpush);
        if (futlr && !c->src[s].is_anchor)
            q1 = y264_gpq_field(e->gpq, c->src[s].push, c->s_futpush[s],
                                c->gpq_maxpush);
    }
    /* The shared set if this leg's anchor got one, else build into the
 * worker's own -- identical planes either way (build_lr_subpel is a pure
 * function of the lowres it reads). */
    if (coh && pastlr && !q0) {
        int i = c->s_sub0 ? c->s_sub0[s] : -1;
        if (i >= 0) {
            if (mbt_sub_verify_on()) {
                build_lr_subpel(subpel0, pastlr, lw, lh);
                for (int p = 1; p < 16; p++)
                    if (memcmp(subpel0[p], e->mbt_sub[i][p], (size_t)lw * lh * sizeof(pixel)))
                        fprintf(stderr, "subverify: MISMATCH past slot %d phase %d src %d key=%p stamp=%ld\n", i, p, s, (const void *)e->mbt_sub_key[i], e->mbt_sub_stamp[i]);
            }
            subpel0 = e->mbt_sub[i];
        }
        else      { if (g_lrs_census > 0) g_lrs_site[1]++; build_lr_subpel(subpel0, pastlr, lw, lh); }
    }
    if (coh && futlr && !q1) {
        int i = c->s_sub1 ? c->s_sub1[s] : -1;
        if (i >= 0) {
            if (mbt_sub_verify_on()) {
                build_lr_subpel(subpel1, futlr, lw, lh);
                for (int p = 1; p < 16; p++)
                    if (memcmp(subpel1[p], e->mbt_sub[i][p], (size_t)lw * lh * sizeof(pixel)))
                        fprintf(stderr, "subverify: MISMATCH fut slot %d phase %d src %d key=%p stamp=%ld\n", i, p, s, (const void *)e->mbt_sub_key[i], e->mbt_sub_stamp[i]);
            }
            subpel1 = e->mbt_sub[i];
        }
        else      { if (g_lrs_census > 0) g_lrs_site[2]++; build_lr_subpel(subpel1, futlr, lw, lh); }
    }

    if (pa_t0) g_mbt_split.pa_sub_ms += tprof_ms() - pa_tc;
    const y264_lr_blk *bleg0 = NULL, *bleg1 = NULL;
    /* Same settled bound as mbt_pair_seed, and for the same reason. This reader
 * predates A1 and looked safe because it fails closed on a pair that does
 * not match -- but "usually decides the same way" is not the same as
 * ordered, TSan flags the read against lowres_bleg_me's write, and a stored
 * pair that DOES match the walk's brackets makes it decide differently
 * depending on when the flag lands. */
    if (bleg_reuse_on() && coh && c->src[s].laidx >= 0 && !c->src[s].is_anchor &&
        (c->src[s].laoff <= c->settled_off || mbt_unsafe_nosettle())) {
        struct la_entry *ce = &e->la[c->src[s].laidx];
        if (ce->bleg_have && ce->bleg_poc0 == c->s_pastpoc[s] &&
            ce->bleg_poc1 == c->s_futpoc[s] && ce->leg[LR_LEG_ANCHOR] &&
            ce->leg[LR_LEG_NEXT])
        { bleg0 = ce->leg[LR_LEG_ANCHOR]; bleg1 = ce->leg[LR_LEG_NEXT]; }
    }
    /* Y264_MBT_AREUSE (default on, 2026-09-02): an ANCHOR source's Phase A
 * search is its field against the previous anchor, which lowres_anchor_me
 * already computed into leg[LR_LEG_ANCHOR] with the previous anchor's POC
 * in aleg_poc0. Same settled bound and the same three-candidate eval as
 * the leaf legs; anchors have no future leg. On sunflower_1080p this was
 * 65 full searches per encode, 118 ms of the 590 ms single-thread Phase A. */
    if (!bleg0 && mbt_areuse_on() && bleg_reuse_on() && coh && c->src[s].laidx >= 0 &&
        c->src[s].is_anchor &&
        (c->src[s].laoff <= c->settled_off || mbt_unsafe_nosettle())) {
        struct la_entry *ce = &e->la[c->src[s].laidx];
        if (ce->aleg_have && ce->aleg_poc0 == c->s_pastpoc[s] && ce->leg[LR_LEG_ANCHOR])
            bleg0 = ce->leg[LR_LEG_ANCHOR];
    }
    /* A1: the pair the walk wants is often NOT the pair the lookahead searched.
 * Since the reference B became a propagation target the walk brackets a leaf
 * between the reference B and an anchor, while the lookahead only ever
 * searched leaf-vs-anchor -- so the exact-key reuse above misses, and the
 * source falls through to a full diamond. It does not have to: the reuse
 * path wants a SEED, not a search, and a seed for leaf->R follows from
 * leaf->anchor by the same constant-velocity scaling lowres_bleg_me already
 * uses for its colocated candidates. The satd is still taken against the
 * true reference, so the cost stays honest; only the starting point is
 * derived. An exact key scales by num/den == 1, so the 123 sources the
 * shipped path already covers come out bit-identical through here. */
    int16_t *pseed = NULL;
    const int16_t *sd0x = NULL, *sd0y = NULL, *sd1x = NULL, *sd1y = NULL;
    if (pair_scale_on() && coh && !bleg0 && !c->src[s].is_anchor) {
        pseed = malloc((size_t)nmb * 4 * sizeof *pseed);
        int h0 = 0, h1 = 0;
        if (pseed && mbt_pair_seed(e, c, s, nmb, pseed, &h0, &h1)) {
            if (h0) { sd0x = pseed; sd0y = pseed + nmb; }
            if (h1) { sd1x = pseed + 2 * nmb; sd1y = pseed + 3 * nmb; }
        }
        if (!sd0x && !sd1x) { free(pseed); pseed = NULL; }
    }
    if (mbt_split_env()) {          /* coverage, not timing: why did this source
 * search instead of reusing? */
        _Atomic long *b = (q0 && (q1 || c->src[s].is_anchor)) ? &g_mbt_split.pa_gpu2
                        : (q0 || q1) ? &g_mbt_split.pa_gpu1
                        : bleg0 ? &g_mbt_split.pa_reuse
                        : (sd0x || sd1x) ? &g_mbt_split.pa_scaled
                        : c->src[s].is_anchor ? &g_mbt_split.pa_anc
                        : c->src[s].laidx < 0 ? &g_mbt_split.pa_noring
                        : &g_mbt_split.pa_nokey;
        atomic_fetch_add_explicit(b, 1, memory_order_relaxed);
        pa_g = b == &g_mbt_split.pa_gpu2 ? 0 : b == &g_mbt_split.pa_gpu1 ? 1 : b == &g_mbt_split.pa_reuse ? 2
             : b == &g_mbt_split.pa_scaled ? 3 : b == &g_mbt_split.pa_anc ? 4 : b == &g_mbt_split.pa_noring ? 5 : 6;
        pa_t1 = tprof_ms();
    }
    const long *bleg_ctab = (bleg0 || pseed || q0 || q1) ? coh_tab(mvlambda) : NULL;
#define COST_INF_L (1L << 40)

    int *rowmv = malloc((size_t)wmb * 4 * sizeof(int));  /* above-row MVs */
    if (!rowmv) {                                        /* OOM: source stays lu=0 (skip) */
        memset(plu, 0, (size_t)nmb);                     /* the slice is the memo array
 * itself now, so clear it here
 * (no calloc'd slab behind it) */
        return;
    }
    for (int i = 0; i < wmb * 4; i++) rowmv[i] = 0;
    for (int my = 0; my < hmb; my++) {
        int pmvx0 = 0, pmvy0 = 0, pmvx1 = 0, pmvy1 = 0;
        for (int mx = 0; mx < wmb; mx++) {
            int i = my * wmb + mx;
            const pixel *sb = flr + (my * 8) * lw + mx * 8;
            long intra = c->src[s].dintra ? c->src[s].dintra[i]
                                          : blk8_intra_dispatch(sb, lw, mx, my);
            plu[i] = 0;
            if (intra <= 0) continue;
            long cost0 = c->cost_inf, cost1 = c->cost_inf;
            int mvx0 = 0, mvy0 = 0, mvx1 = 0, mvy1 = 0;
            if (pastlr) {
                if (q0) {
                    /* GPU field: price {qpel best, zero} with the chained
 * predictor -- the bleg 3-candidate shape with the SATDs
 * read instead of computed. Both MVs are in-plane by the
 * kernel's bound (the CPU search's own bound). */
                    cost0 = COST_INF_L;
                    long cb = (long)q0[i].satd
                            + coh_rate(bleg_ctab, mvlambda,
                                       lowres_mvbits((q0[i].mvx - pmvx0) >> 2) +
                                       lowres_mvbits((q0[i].mvy - pmvy0) >> 2));
                    cost0 = cb; mvx0 = q0[i].mvx; mvy0 = q0[i].mvy;
                    if (q0[i].mvx || q0[i].mvy) {
                        long cz = (long)q0[i].satd0
                                + coh_rate(bleg_ctab, mvlambda,
                                           lowres_mvbits((0 - pmvx0) >> 2) +
                                           lowres_mvbits((0 - pmvy0) >> 2));
                        if (cz < cost0) { cost0 = cz; mvx0 = 0; mvy0 = 0; }
                    }
                } else if (bleg0 && mbt_legtrust_on()) {
                    /* E1: the stored leg's SATD and MV, priced with the
 * walk's own rate against the chained predictor. */
                    cost0 = (long)bleg0[i].d_inter
                          + coh_rate(bleg_ctab, mvlambda,
                                     lowres_mvbits((bleg0[i].mvx - pmvx0) >> 2) +
                                     lowres_mvbits((bleg0[i].mvy - pmvy0) >> 2));
                    mvx0 = bleg0[i].mvx; mvy0 = bleg0[i].mvy;
                } else if (bleg0 || sd0x) {
                    /* A 3-candidate eval ({bleg MV, chained pred,
 * zero} -- phase A's own seed family) instead of trusting
 * the single reused MV; rescues mispriced legs on chaotic
 * motion (coastguard/park_joy) at 3 satds
 * instead of a full diamond. */
                    cost0 = COST_INF_L;
                    int cnd[3][2] = { { bleg0 ? bleg0[i].mvx : sd0x[i],
                                        bleg0 ? bleg0[i].mvy : sd0y[i] },
                                      { pmvx0, pmvy0 }, { 0, 0 } };
                    for (int k = 0; k < 3; k++) {
                        if (k && cnd[k][0] == cnd[0][0] && cnd[k][1] == cnd[0][1]) continue;
                        int qx = cnd[k][0], qy = cnd[k][1];
                        if (qx < -4*(mx*8) || qx > 4*(lw-8-mx*8) ||
                            qy < -4*(my*8) || qy > 4*(lh-8-my*8)) continue;
                        long ck = blk8_satd_qp(sb, lw, pastlr, lw, subpel0, mx * 8, my * 8, qx, qy)
                                + coh_rate(bleg_ctab, mvlambda,
                                           lowres_mvbits((qx - pmvx0) >> 2) +
                                           lowres_mvbits((qy - pmvy0) >> 2));
                        if (ck < cost0) { cost0 = ck; mvx0 = qx; mvy0 = qy; }
                    }
                } else if (coh)
                    cost0 = blk8_inter_coh(sb, lw, pastlr, lw, subpel0, lw, lh, mx * 8, my * 8, pmvx0, pmvy0,
                                           rowmv[mx*4+0], rowmv[mx*4+1], mvlambda,
                                           g0 ? g0[i * 2] : 0, g0 ? g0[i * 2 + 1] : 0, g0 != NULL,
                                           &mvx0, &mvy0);
                else {
                    cost0 = blk8_inter(sb, lw, pastlr, lw, lw, lh, mx * 8, my * 8, &mvx0, &mvy0);
                    cost0 += (long)lround(mvlambda * (lowres_mvbits(mvx0 - pmvx0) + lowres_mvbits(mvy0 - pmvy0)));
                }
                if (cost0 > intra) cost0 = intra;
                pmvx0 = mvx0; pmvy0 = mvy0; rowmv[mx*4+0] = mvx0; rowmv[mx*4+1] = mvy0;
            }
            if (!c->src[s].is_anchor && futlr) {
                if (q1) {
                    cost1 = COST_INF_L;
                    long cb = (long)q1[i].satd
                            + coh_rate(bleg_ctab, mvlambda,
                                       lowres_mvbits((q1[i].mvx - pmvx1) >> 2) +
                                       lowres_mvbits((q1[i].mvy - pmvy1) >> 2));
                    cost1 = cb; mvx1 = q1[i].mvx; mvy1 = q1[i].mvy;
                    if (q1[i].mvx || q1[i].mvy) {
                        long cz = (long)q1[i].satd0
                                + coh_rate(bleg_ctab, mvlambda,
                                           lowres_mvbits((0 - pmvx1) >> 2) +
                                           lowres_mvbits((0 - pmvy1) >> 2));
                        if (cz < cost1) { cost1 = cz; mvx1 = 0; mvy1 = 0; }
                    }
                } else if (bleg1 && mbt_legtrust_on()) {
                    cost1 = (long)bleg1[i].d_inter
                          + coh_rate(bleg_ctab, mvlambda,
                                     lowres_mvbits((bleg1[i].mvx - pmvx1) >> 2) +
                                     lowres_mvbits((bleg1[i].mvy - pmvy1) >> 2));
                    mvx1 = bleg1[i].mvx; mvy1 = bleg1[i].mvy;
                } else if (bleg1 || sd1x) {
                    cost1 = COST_INF_L;
                    int cnd[3][2] = { { bleg1 ? bleg1[i].mvx : sd1x[i],
                                        bleg1 ? bleg1[i].mvy : sd1y[i] },
                                      { pmvx1, pmvy1 }, { 0, 0 } };
                    for (int k = 0; k < 3; k++) {
                        if (k && cnd[k][0] == cnd[0][0] && cnd[k][1] == cnd[0][1]) continue;
                        int qx = cnd[k][0], qy = cnd[k][1];
                        if (qx < -4*(mx*8) || qx > 4*(lw-8-mx*8) ||
                            qy < -4*(my*8) || qy > 4*(lh-8-my*8)) continue;
                        long ck = blk8_satd_qp(sb, lw, futlr, lw, subpel1, mx * 8, my * 8, qx, qy)
                                + coh_rate(bleg_ctab, mvlambda,
                                           lowres_mvbits((qx - pmvx1) >> 2) +
                                           lowres_mvbits((qy - pmvy1) >> 2));
                        if (ck < cost1) { cost1 = ck; mvx1 = qx; mvy1 = qy; }
                    }
                } else if (coh)
                    cost1 = blk8_inter_coh(sb, lw, futlr, lw, subpel1, lw, lh, mx * 8, my * 8, pmvx1, pmvy1,
                                           rowmv[mx*4+2], rowmv[mx*4+3], mvlambda,
                                           g1 ? g1[i * 2] : 0, g1 ? g1[i * 2 + 1] : 0, g1 != NULL,
                                           &mvx1, &mvy1);
                else {
                    cost1 = blk8_inter(sb, lw, futlr, lw, lw, lh, mx * 8, my * 8, &mvx1, &mvy1);
                    cost1 += (long)lround(mvlambda * (lowres_mvbits(mvx1 - pmvx1) + lowres_mvbits(mvy1 - pmvy1)));
                }
                if (cost1 > intra) cost1 = intra;
                pmvx1 = mvx1; pmvy1 = mvy1; rowmv[mx*4+2] = mvx1; rowmv[mx*4+3] = mvy1;
            }
            long inter = intra; int lu = 0;
            if (cost0 < inter) { inter = cost0; lu = 1; }
            if (cost1 < inter) { inter = cost1; lu = 2; }
            if (cost0 < intra && cost1 < intra) { inter = cost0 < cost1 ? cost0 : cost1; lu = 3; }
            if (inter >= intra) continue;                /* intra MB: no propagation */
            pi[i] = intra; pin[i] = inter; plu[i] = (signed char)lu;
            pmv[i*4+0] = mvx0; pmv[i*4+1] = mvy0; pmv[i*4+2] = mvx1; pmv[i*4+3] = mvy1;
            psw[i] = pow(2.0, -(double)aqoff_s[i] / 6.0);
        }
    }
    free(rowmv);
    free(pseed);
    if (mbt_split_env()) { double te = tprof_ms(); g_mbt_split.pa_setup_ms += pa_t1 - pa_t0; g_mbt_split.pa_ms[pa_g] += te - pa_t1; g_mbt_split.pa_n[pa_g]++; }

    /* The slice was computed directly into its owner's memo arrays; mark the
 * owner valid under the current key. Each owner is written by exactly one
 * worker per parallel_for (distinct sources -> distinct slots). */
    int laidx = c->src[s].laidx;
    if (laidx >= 0) {
        struct la_entry *ce = &e->la[laidx];
        ce->mbt_pa_past_poc = c->s_pastpoc[s];
        ce->mbt_pa_fut_poc  = c->s_futpoc[s];
        ce->mbt_pa_valid = 1;
    } else if (c->src[s].bbuf >= 0) {
        struct mbt_bmemo *m = &e->bmbt[c->src[s].bbuf];
        m->past_poc = c->s_pastpoc[s];
        m->fut_poc  = c->s_futpoc[s];
        m->valid = 1;
    }
}

/* behaviour-matched whole-buffer mb-tree. Replaces
 * the per-anchor propagation with x264's single backward pass. At anchor F's code
 * time the display-order dependency buffer [prev_anchor, buffered B's, F, future
 * window frames up to the next IDR] is reconstructed; every frame after F (future
 * B's, future anchors) and every buffered B deposits importance onto the anchor(s)
 * it references, chained anchor->anchor and terminating at F. F's accumulated
 * propagate_cost then finishes into its per-MB offset. Double accumulators make
 * the reference's fixed-point precision guard unnecessary (its 0.5-in-propagate /
 * x2-in-finish scaling cancels to 1). Gated Y264_MBTREE_WHOLEBUF. Writes e->mbtree_off
 * and returns 1, or 0 to fall back to the legacy path.
 * Faithful to: propagate amount = in + intra*w, *(intra-min_inter)/intra; lists_used
 * per-list gating + bipred split; finish off = aq - strength*log2((ic+prop)/ic),
 * strength=5*(1-qcomp), NOT centred; w = 2^(-aq_off/6). */
/* Y264_MBT_SPLIT=1: where compute_mbtree's time actually goes, so the question
 * "which part of this can be led?" is answered with a number. Phase A is
 * memoized and therefore prefetchable; Phase B's float accumulation order is
 * the bits, so it is pinned to the anchor. Probe only, printed at close. */
static pthread_mutex_t g_mbt_split_mx = PTHREAD_MUTEX_INITIALIZER;
/* Y264_MBT_BLEG_REUSE: in Phase A, when a leaf source's bleg pair fields
 * (lowres_bleg_me) exist for the SAME bracketing anchors, price the bleg MV
 * (satd at the MV + mv-rate, phase A's exact cost form) instead of running
 * the coherent search again -- x264 serves both frame-typing and propagation
 * from one memoized lowres-ME store, where running the search again is two
 * searches over the same pairs. The blegs themselves are untouched, so the full-res B seeds do
 * not move; the only output exposure is mb-tree offsets from leg costs taken
 * at the bleg MV instead of the searched one.
 *
 * Default ON. Wall (t1 pure-C, interleaved, medians of 5): samsung
 * -4.97/-3.15%, pjoy -3.98/-3.05%, foreman -4.44/-2.80%, stefan -4.27/-2.62%,
 * controls within +/-0.35%. CRF band median -0.10%, worst mobile +0.43%. The
 * akiyo and coastguard ABR rows that read as a cost are inside those clips'
 * own perturbation noise; the one ABR row that resolves, samsung's, costs
 * +0.47% against that clip's -4.97% of wall. */
static int bleg_reuse_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_MBT_BLEG_REUSE"); v = e ? (atoi(e) ? 1 : 0) : 1; }
    return v;
}

/* Y264_MBT_UNSAFE_NOSETTLE=1: MEASUREMENT ONLY, and it is deliberately unsafe.
 * Ignores the A1 settled bound in both pair readers, so a source whose bleg legs
 * are still being written is read anyway. That is the race A1 fixed -- it emits
 * several distinct bitstreams per run -- so this can only ever bound the prize,
 * never be a shipping shape. It answers one question: if every key-mismatch
 * source could be seeded instead of searched, how much wall would that be?
 * An unsafe delete measures a DIFFERENT encoder, so treat the number as an
 * upper bound and re-price the real fix. */
static int mbt_unsafe_nosettle(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_MBT_UNSAFE_NOSETTLE"); v = e ? (atoi(e) ? 1 : 0) : 0; }
    return v;
}

/* Y264_GPQ_CONSUME=0: probe -- submit gpq rounds but never read them (splits
 * the armed tax into chain side vs walk side). Warmed in warm_lr_statics
 * (the tsan-lazy-static class: pool workers must not first-touch). */
static int gpq_consume_on(void)
{
    static int v = -1;
    if (v < 0) { const char *ev = getenv("Y264_GPQ_CONSUME");
                 v = ev ? (atoi(ev) ? 1 : 0) : 1; }
    return v;
}
static int mbt_split_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_SPLIT"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* GPU leg batch, shared by the WALK (compute_mbtree_wholebuf) and the WARM
 * (mbt_warm_window). Submit registers each needed source's lowres plus its
 * bracket planes and queues one leg per bracket; collect waits and hands the
 * fields to mbt_pa_source as integer-search seeds. Failure at any point
 * leaves the pointers NULL and the CPU path runs -- the required degradation.
 * The two sites use SEPARATE handles (e->gpu / e->gpu_warm): the warm and the
 * next walk overlap by design, and the handle is stateful. */
struct mbt_gpu_batch { int16_t **fields, **g0, **g1; int nfield, inflight; };

static void mbt_gpu_submit(struct y264_gpu *g, struct mbt_gpu_batch *b, int ns,
                           const struct mbt_source *src, const unsigned char *need,
                           const pixel *const *pastlr, const pixel *const *futlr,
                           int lw, int lh)
{
    memset(b, 0, sizeof *b);
    if (!g || !y264_gpu_lowres_begin(g, lw, lh))
        return;
    int nblk2 = (lw / 8) * (lh / 8) * 2;
    b->g0 = calloc((size_t)ns, sizeof *b->g0);
    b->g1 = calloc((size_t)ns, sizeof *b->g1);
    b->fields = calloc((size_t)ns * 2, sizeof *b->fields);
    if (b->g0 && b->g1 && b->fields) {
        for (int s = 0; s < ns; s++) {
            if (!need[s] || !src[s].lr) continue;   /* memo hit / no lowres yet */
            int cs = y264_gpu_lowres_plane(g, src[s].lr);
            if (cs < 0) break;
            const pixel *legref[2] = { pastlr[s], futlr[s] };
            int16_t **legdst[2] = { &b->g0[s], &b->g1[s] };
            for (int h = 0; h < 2; h++) {
                if (!legref[h]) continue;
                int rs2 = y264_gpu_lowres_plane(g, legref[h]);
                if (rs2 < 0) break;
                int16_t *f = malloc((size_t)nblk2 * sizeof(int16_t));
                if (!f) break;
                if (!y264_gpu_lowres_leg(g, cs, rs2, f)) { free(f); break; }
                b->fields[b->nfield++] = f;
                *legdst[h] = f;
            }
        }
        b->inflight = y264_gpu_lowres_submit(g, gpu_range());
    }
    if (!b->inflight) {
        for (int i = 0; i < b->nfield; i++) free(b->fields[i]);
        free(b->fields); free(b->g0); free(b->g1);
        memset(b, 0, sizeof *b);
    }
}

static int mbt_gpu_collect(struct y264_gpu *g, struct mbt_gpu_batch *b)
{
    if (!b->inflight)
        return 0;
    if (y264_gpu_lowres_wait(g))
        return 1;
    for (int i = 0; i < b->nfield; i++) free(b->fields[i]);
    free(b->fields); free(b->g0); free(b->g1);
    memset(b, 0, sizeof *b);
    return 0;
}

static void mbt_gpu_free(struct mbt_gpu_batch *b)
{
    for (int i = 0; i < b->nfield; i++) free(b->fields[i]);
    free(b->fields); free(b->g0); free(b->g1);
    memset(b, 0, sizeof *b);
}

static int compute_mbtree_wholebuf(yah264_encoder_t *e, const struct mbt_req *rq,
                               const float *aq_fold)
{
    int split_on = mbt_split_env();
    double t_s = split_on ? tprof_ms() : 0;
    pixel *const *anchor_src = rq->anchor_planes;
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs, nmb = wmb * hmb;
    int lw = e->lr_w, lh = e->lr_h;
    const pixel *F_lr = rq->anchor_lr;               /* current anchor F's lowres */
    int F_poc = rq->anchor_poc;
    const long COST_INF = 1L << 40;

    enum { MAXA = 64, MAXS = 192 };
    struct mbt_anchor anc[MAXA];
    int na = 0;
    anc[na].poc = F_poc; anc[na].lr = F_lr; anc[na].dintra = NULL;  /* F intra = lr_intra */
    anc[na].full = anchor_src; anc[na].push = rq->anchor_push; na++;
    /* Capped at la_depth-2, not la_n -- see la_chain_prop's comment for why
 * -2 (not -1): at k=0 the walk's last la_n-bounded entry is always the
 * frame pushed THIS call, still untyped, so `!en->typed` breaks there and
 * only la_depth-2 entries are ever used; Y264_LA_BUF makes that position
 * typed by then, so it must be excluded explicitly to keep this walk's
 * value independent of la_buf. Ring indices wrap at la_cap. */
    int wcap = e->la_depth > 1 ? e->la_depth - 2 : 0;
    for (int i = 0; i < rq->navail && i < wcap && na < MAXA; i++) {
        struct la_entry *en = &e->la[(rq->head + i) % e->la_cap];
        if (!en->typed || en->is_idr) break;         /* never cross the IDR */
        if (en->is_anchor) {
            anc[na].poc = en->since_val * 2; anc[na].lr = en->lowres;
            anc[na].dintra = en->d_intra; anc[na].full = en->plane;
            anc[na].push = en->push_idx; na++;
        }
    }

    /* Promote the pyramid's reference B to a propagation TARGET, so the
 * graph is leaf -> ref B -> anchor as x264's is, instead of leaves depositing
 * straight onto anchors. Everything downstream already supports it: the
 * bracketing below is a nearest-POC search so adjacent leaves route onto it
 * for free, and a source that is ALSO an anchor already inherits its own
 * accumulated propagate cost through s_self/inh -- which is exactly what an
 * intermediate hop must do. The lowres has to be persistent (e->blowres),
 * because anc[].lr is read as a reference plane. */
    int refb_anc = -1, refb_buf = -1;
    /* The valid flag is this walk's statement about this walk, so it is
     * cleared here every time. Set-and-never-cleared lets a mini-GOP whose
     * shape grows no reference-B field (nbuf outside 2..3 -- every full run at
     * --bframes 4..7 -- or a missing lowres) inherit the PREVIOUS mini-GOP's
     * field and apply it to its own reference B: the same class as sharing the
     * bytes, one level up, where what is shared is the claim that they are
     * current. */
    e->bmbtree_valid[1] = 0;
    /* NOT serial-only: encoder-global slots for this field (e->blowres,
     * e->bmbtree_off) are written by the walk and read by the B's emit, which
     * overlap under the stair/wavefront, so a later walk overwrites a field an
     * in-flight B is still using -- that shape gives three different md5s from
     * three identical t8 runs. `struct stair_burst` owns the bytes
     * (bmbtoff[8]) and routing goes through `struct frame_work` so the serial
     * and stair paths cannot diverge. Verified t8 3/3 identical, t8 == t18. */
    if (mbt_bref_probe() && (e->nbuf == 2 || e->nbuf == 3) && na < MAXA) {
        refb_buf = 1;                        /* stair_refb_poc: bpoc[1] */
        if (e->blowres[refb_buf] && e->bplane[refb_buf][0]) {
            downscale(e->blowres[refb_buf], lw, lh,
                      e->bplane[refb_buf][0], e->pstride[0]);
            refb_anc = na;
            anc[na].poc = e->bpoc[refb_buf]; anc[na].lr = e->blowres[refb_buf];
            anc[na].dintra = NULL; anc[na].full = e->bplane[refb_buf];
            anc[na].push = e->bpush[refb_buf]; na++;
        } else refb_buf = -1;
    }
    double *prop = calloc((size_t)na * nmb, sizeof(double));  /* per-anchor accumulators */
    double *Fw   = malloc(nmb * sizeof(double));              /* F's weight 2^(-aqoff/6) */
    if (!prop || !Fw) { free(prop); free(Fw); return 0; }
    /* aq_fold is the CODED field; when the internal strength is decoupled
 * (Y264_MBT_AQIN), rescale the exponent to that strength. The field is
 * linear in strength (mbtree_invqscale), so the ratio is exact. */
    double fw_k = 1.0;
    { float aqin = mbt_aqin();
      if (aqin >= 0.0f && e->aq_strength > 0.0f) fw_k = aqin / e->aq_strength; }
    for (int i = 0; i < nmb; i++) Fw[i] = pow(2.0, -(double)aq_fold[i] * fw_k / 6.0);

    double mvlambda = mbtree_mvlambda();
    int bipw = 32;                                   /* x264 default bipred weight (0.5) */
    /* Coherent (predictor-seeded, MV-cost) + quarter-pel + multi-predictor lowres ME:
 * tracks subpel/divergent motion so mb-tree propagation accumulates along the true
 * motion instead of scattering. DEFAULT ON (paired with the CRF operating-point
 * devices below). Y264_LOWRES_COH=0 restores the whole-pel search. */
    int coh = mbt_coh();
    double qcomp = abr_qcomp_env();
    /* x264 uses 5*(1-qcomp)=2.0. yah264's coarser whole-pel lowres ME makes that
 * redistribution too aggressive on motion (BD-measured); 0.7x = 1.4 is the
 * VMAF-NEG optimum across the corpus. */
    /* The 0.7 stands under the absolute anchor too. Restoring x264's full
 * 5*(1-qcomp) = 2.0 does close the bitrate gap at equal CRF, but it costs
 * BD on 10 of 12 corpus clips (+7.04% ducks, +6.2% bus, +6.15% tempete),
 * so the scale match has to come from the pedestal instead. */
    /* The 0.7 is the last of the fitted constants, and it goes under the x264
 * mode for the same reason the others do: it was fitted against OUR field,
 * OUR aq level and OUR AC gain, and the refusals above ("costs BD on 10 of
 * 12 corpus clips") were all measured with those held. The mode restores
 * 5*(1-qcomp) = 2.0 together with them. */
    double strength = 5.0 * (1.0 - qcomp) * (y264_mbt_derived() ? 1.0 : 0.7);
    { const char *v = getenv("Y264_MBTREE_STRENGTH"); if (v) strength = atof(v); }

    /* --- source frames (deposit onto tracked anchors): window frames (poc>F) +
 * buffered B's (poc<F), processed newest-first so an anchor's incoming
 * propagate_cost is complete before it emits onto its own past anchor. --- */
    struct mbt_source src[MAXS];
    int ns = 0;
    for (int i = 0; i < rq->navail && i < wcap && ns < MAXS; i++) {
        struct la_entry *en = &e->la[(rq->head + i) % e->la_cap];
        if (!en->typed || en->is_idr) break;
        src[ns].poc = en->since_val * 2; src[ns].is_anchor = en->is_anchor;
        src[ns].lr = en->lowres; src[ns].dintra = en->d_intra;
        src[ns].full = en->plane; src[ns].bbuf = -1; src[ns].push = en->push_idx;
        src[ns].laidx = (rq->head + i) % e->la_cap; src[ns].laoff = i; ns++;
    }
    for (int b = 0; b < e->nbuf && ns < MAXS; b++) {
        src[ns].poc = e->bpoc[b]; src[ns].is_anchor = 0; src[ns].lr = NULL;
        src[ns].dintra = NULL; src[ns].full = e->bplane[b]; src[ns].bbuf = b;
        src[ns].push = e->bpush[b];
        src[ns].laidx = -1; src[ns].laoff = -1; ns++;
    }
    for (int i = 1; i < ns; i++) {                   /* insertion sort, poc descending */
        struct mbt_source t = src[i]; int j = i - 1;
        while (j >= 0 && src[j].poc < t.poc) { src[j+1] = src[j]; j--; }
        src[j+1] = t;
    }

    /* Per-source anchor topology + reference legs (prop-independent), computed
 * serially so Phase A (parallel) and Phase B (serial splat) both read them. */
    int *s_past = malloc((size_t)ns * sizeof(int));
    int *s_fut  = malloc((size_t)ns * sizeof(int));
    int *s_self = malloc((size_t)ns * sizeof(int));
    int *s_pastpoc = malloc((size_t)ns * sizeof(int));
    int *s_futpoc  = malloc((size_t)ns * sizeof(int));
    const pixel **s_pastlr = malloc((size_t)ns * sizeof(*s_pastlr));
    const pixel **s_futlr  = malloc((size_t)ns * sizeof(*s_futlr));
    long *s_pastpush = malloc((size_t)ns * sizeof(long));
    long *s_futpush  = malloc((size_t)ns * sizeof(long));
    /* Per-source result pointers into the owners' memo arrays (ring entry or
 * bbuf slot); allocated lazily per owner, persistent across calls. Computed
 * in place on a miss, read in place by Phase B -- no scratch slabs. */
    long  *pp_pi[MAXS], *pp_pin[MAXS];
    signed char *pp_plu[MAXS];
    int   *pp_pmv[MAXS];
    double *pp_psw[MAXS];
    unsigned char need[MAXS];
    if (!s_past || !s_fut || !s_self || !s_pastpoc || !s_futpoc || !s_pastlr || !s_futlr
        || !s_pastpush || !s_futpush) {
        free(s_past); free(s_fut); free(s_self); free(s_pastpoc); free(s_futpoc);
        free(s_pastlr); free(s_futlr); free(s_pastpush); free(s_futpush);
        free(prop); free(Fw); return 0;
    }
    for (int s = 0; s < ns; s++) {
        int past = -1, fut = -1, self = -1, bp = -(1 << 30), bf = (1 << 30);
        for (int a = 0; a < na; a++) {
            if (anc[a].poc < src[s].poc && anc[a].poc > bp) { bp = anc[a].poc; past = a; }
            if (anc[a].poc > src[s].poc && anc[a].poc < bf) { bf = anc[a].poc; fut  = a; }
            if (anc[a].poc == src[s].poc) self = a;
        }
        s_past[s] = past; s_fut[s] = fut; s_self[s] = self;
        /* Buffered B's past leg is the already-coded anchor (code_panchor), not a
 * tracked anc[] entry; key it by that anchor's real POC so the memo the B
 * carried out of the ring (same bracketing anchors) can hit. */
        s_pastpoc[s] = past >= 0 ? anc[past].poc
                     : (src[s].bbuf >= 0 && e->code_panchor_have ? e->code_panchor_poc
                                                                 : INT_MIN);
        s_futpoc[s]  = fut  >= 0 ? anc[fut].poc  : INT_MIN;
        s_pastlr[s] = past >= 0 ? anc[past].lr
                    : (src[s].bbuf >= 0 && e->code_panchor_have ? e->code_panchor_lr : NULL);
        s_futlr[s]  = fut  >= 0 ? anc[fut].lr : NULL;
        s_pastpush[s] = past >= 0 ? anc[past].push
                      : (src[s].bbuf >= 0 && e->code_panchor_have ? e->code_panchor_push : 0);
        s_futpush[s]  = fut >= 0 ? anc[fut].push : 0;
    }

    /* Bind each source to its owner's memo arrays and decide compute-vs-hit.
 * Allocation is serial here so the (parallel) Phase A never mallocs. */
    int alloc_ok = 1;
    for (int s = 0; s < ns; s++) {
        int laidx = src[s].laidx;
        if (laidx >= 0) {
            struct la_entry *ce = &e->la[laidx];
            if (!ce->mbt_pa_pi) {
                ce->mbt_pa_pi  = malloc((size_t)nmb * sizeof(long));
                ce->mbt_pa_pin = malloc((size_t)nmb * sizeof(long));
                ce->mbt_pa_plu = malloc((size_t)nmb);
                ce->mbt_pa_pmv = malloc((size_t)nmb * 4 * sizeof(int));
                ce->mbt_pa_psw = malloc((size_t)nmb * sizeof(double));
            }
            pp_pi[s] = ce->mbt_pa_pi;   pp_pin[s] = ce->mbt_pa_pin;
            pp_plu[s] = ce->mbt_pa_plu; pp_pmv[s] = ce->mbt_pa_pmv;
            pp_psw[s] = ce->mbt_pa_psw;
            need[s] = !(ce->mbt_pa_valid &&
                        ce->mbt_pa_past_poc == s_pastpoc[s] &&
                        ce->mbt_pa_fut_poc  == s_futpoc[s]);
            if (need[s] && mbt_split_env()) {
                if (!ce->mbt_pa_valid) g_mbt_split.mfresh++;
                else if (ce->mbt_pa_past_poc != s_pastpoc[s]) g_mbt_split.mpast++;
                else g_mbt_split.mfut++;
            }
        } else {
            struct mbt_bmemo *m = &e->bmbt[src[s].bbuf];
            if (!m->pi) {
                m->pi  = malloc((size_t)nmb * sizeof(long));
                m->pin = malloc((size_t)nmb * sizeof(long));
                m->plu = malloc((size_t)nmb);
                m->pmv = malloc((size_t)nmb * 4 * sizeof(int));
                m->psw = malloc((size_t)nmb * sizeof(double));
            }
            pp_pi[s] = m->pi;   pp_pin[s] = m->pin;
            pp_plu[s] = m->plu; pp_pmv[s] = m->pmv;
            pp_psw[s] = m->psw;
            need[s] = !(m->valid &&
                        m->past_poc == s_pastpoc[s] && m->fut_poc == s_futpoc[s]);
        }
        if (!pp_pi[s] || !pp_pin[s] || !pp_plu[s] || !pp_pmv[s] || !pp_psw[s])
            alloc_ok = 0;
    }
    if (!alloc_ok) {                    /* OOM: legacy fallback (as the slab OOM did) */
        free(s_past); free(s_fut); free(s_self); free(s_pastpoc); free(s_futpoc);
        free(s_pastlr); free(s_futlr); free(s_pastpush); free(s_futpush);
        free(prop); free(Fw); return 0;
    }

    /* Y264_MBT_SETTLE_WAIT (default on): wait for the lookahead chain through
 * the step that finalizes the whole window before Phase A, so the settled
 * bound can drop the wide pipeline's fail-closed reach. The wait is to a
 * fixed step (pop_seq + la_depth - 1, clamped to what was pushed), so which
 * sources reuse their pair legs stays a function of the launch sequence and
 * never of chain timing. At 12 threads on sunflower_1080p the reach
 * declined 150 of 306 sources into full searches whose legs already existed;
 * the chain is normally far ahead, so the wait itself is cheap. */
    if (e->la_th && mbt_settle_wait_on())
        TPROF(TP_LAWAIT, la_th_wait(e, e->la_th->pop_seq + e->la_depth - 1));
    /* Phase A: the per-source lowres ME (the expensive part) -- parallel over the
 * pool when available (each source is independent, writes its own pa_* slice),
 * else serial. Byte-identical either way: no prop reads/writes here. */
    struct mbt_pa_ctx pac = {
        .e = e, .src = src, .nmb = nmb, .wmb = wmb, .hmb = hmb, .lw = lw, .lh = lh,
        .coh = coh, .mvlambda = mvlambda, .cost_inf = COST_INF,
        .s_past = s_past, .s_fut = s_fut, .s_pastlr = s_pastlr, .s_futlr = s_futlr,
        /* Under WIDE chains the walk can run further behind the chain's
 * head than the serial pop cadence assumed, so the settled bound
 * tightens by the wide pipeline's reach -- fail-closed: a declined
 * source searches fresh instead of reusing a pair that may still be
 * mid-write (the TSan pair: mbt_pa_source's bleg reads against
 * la_finalize's leg writes). */
        .settled_off = e->la_th
            ? e->la_depth - 4 - e->bframes
              - (stair_wide_engaged_cfg(e) && !mbt_settle_wait_on()
                 ? Y264_STAIR_K * (e->bframes + 1) : 0)
            : INT_MAX,
        .s_pastpoc = s_pastpoc, .s_futpoc = s_futpoc,
        .s_pastpush = e->gpq ? s_pastpush : NULL,
        .s_futpush  = e->gpq ? s_futpush  : NULL,
        .pp_pi = pp_pi, .pp_pin = pp_pin, .pp_plu = pp_plu,
        .pp_pmv = pp_pmv, .pp_psw = pp_psw, .need = need,
    };
    if (e->gpq) {                   /* chain-wait bound: entries enumerated above */
        long mp = rq->anchor_push;
        for (int s2 = 0; s2 < ns; s2++) if (src[s2].push > mp) mp = src[s2].push;
        for (int a2 = 0; a2 < na; a2++) if (anc[a2].push > mp) mp = anc[a2].push;
        pac.gpq_maxpush = mp;
    }
    if (split_on) {
        double t = tprof_ms();
        g_mbt_split.bind += t - t_s; t_s = t;
        g_mbt_split.calls++; g_mbt_split.srcs += ns;
        for (int s = 0; s < ns; s++) {
            g_mbt_split.misses += need[s] ? 1 : 0;
            /* where in the window does a miss live? src[] is poc-sorted, so
 * recover the ring offset from laidx rather than from s. */
            if (need[s] && src[s].laidx >= 0) {
                int off = (src[s].laidx - rq->head + e->la_cap) % e->la_cap;
                if (off < 64) g_mbt_split.mpos[off]++;
            } else if (need[s]) g_mbt_split.mpos[63]++;
        }
    }
    int pool_nt = e->pool ? ntp_pool_nthreads(e->pool) : 0;
    int use_pool = pool_nt > 1 && mbt_ensure_ws(e, pool_nt);
    if (!use_pool && !mbt_ensure_ws(e, 1)) {                  /* need slot 0 for serial */
        free(s_past); free(s_fut); free(s_self); free(s_pastpoc); free(s_futpoc);
        free(s_pastlr); free(s_futlr); free(s_pastpush); free(s_futpush);
        free(prop); free(Fw); return 0;
    }
    /* GPU integer search (Y264_GPU_LOWRES). SUBMITTED HERE and collected AFTER
 * the subpel-plane build below, so the GPU runs concurrently with that
 * parallel_for instead of in front of it. The synchronous version of this
 * measured 1.86x SLOWER at t12 -- 1.33x even with the search range set so
 * low the GPU did no work -- because blocking the driver before Phase A
 * serializes what ran across twelve pool threads. The overlap IS the
 * feature.
 *
 * Only ring sources are offloaded: a buffered B's lowres is downscaled
 * inside the worker and does not exist yet here, so it keeps the CPU search.
 * Any failure leaves the field pointers NULL and every block takes the CPU
 * path, which is the required degradation. */
    struct mbt_gpu_batch gb;
    memset(&gb, 0, sizeof gb);
    if (e->gpu && coh)
        mbt_gpu_submit(e->gpu, &gb, ns, src, need, s_pastlr, s_futlr, lw, lh);

    int sub0[MAXS], sub1[MAXS], subheld = 0;
    if (coh && (subheld = mbt_sub_claim(e))) {
        { int nb = mbt_sub_plan(e, ns, need, s_pastlr, s_futlr, s_pastpush, s_futpush, sub0, sub1, pool_nt);
          if (mbt_split_env()) g_mbt_split.sub_built += nb; }
        pac.s_sub0 = sub0; pac.s_sub1 = sub1;
        if (g_lrs_census > 0) {
            g_lrs_plan[0]++;
            for (int s2 = 0; s2 < ns; s2++) if (need[s2]) {
                if (s_pastlr[s2]) { g_lrs_plan[2]++; if (sub0[s2] >= 0) g_lrs_plan[3]++; else g_lrs_plan[4]++; }
                if (s_futlr[s2])  { g_lrs_plan[2]++; if (sub1[s2] >= 0) g_lrs_plan[3]++; else g_lrs_plan[4]++; }
            }
        }
    } else if (g_lrs_census > 0) {
        if (!(coh)) g_lrs_plan[5]++; else g_lrs_plan[1]++;
    }
    /* Collect: the GPU has been running through the plane build above. */
    if (mbt_gpu_collect(e->gpu, &gb)) { pac.gpu0 = gb.g0; pac.gpu1 = gb.g1; }
    if (use_pool)
    {
        ntp_prof_tag("mbtree_srcA"); ntp_prio_hint();
        ntp_parallel_for(e->pool, ns, mbt_pa_source, &pac);
    }
    else
        for (int s = 0; s < ns; s++) mbt_pa_source(&pac, 0, s);   /* serial fallback (tid 0) */
    if (subheld) mbt_sub_release(e);
    mbt_gpu_free(&gb);

    if (split_on) { double t = tprof_ms(); g_mbt_split.pa += t - t_s; t_s = t; }
    /* Phase B: deposit the ME importance onto the shared prop accumulators, in the
 * exact source (poc-desc) x raster order of the fused loop -- the float
 * accumulation order is preserved, so the offsets are bit-identical. */
    for (int s = 0; s < ns; s++) {
        const double *inh = s_self[s] >= 0 ? prop + (size_t)s_self[s] * nmb : NULL;
        double *dst0 = s_past[s] >= 0 ? prop + (size_t)s_past[s] * nmb : NULL;
        double *dst1 = s_fut[s]  >= 0 ? prop + (size_t)s_fut[s]  * nmb : NULL;
        const long *pi = pp_pi[s], *pin = pp_pin[s];
        const signed char *plu = pp_plu[s];
        const int *pmv = pp_pmv[s];
        const double *psw = pp_psw[s];
        /* DO NOT SPLIT OR VECTORISE THIS LOOP -- built twice, null twice.
         * The per-MB `d` compute is per-element independent and CAN be hoisted
         * / NEON'd byte-identically (a split+unroll, and an independent
         * f64x2-fma kernel, bit-exact both tiers) -- and both measure EXACT
         * NULL against their controls, because the compute is 7-9% of the
         * stage and the other ~90% is the scatter, whose accumulation order is
         * the bits and whose border clamp aliases two of the four bilinear
         * targets onto one cell. Phase B's serial 4% of the t12 wall is the
         * SCATTER, and collecting it means changing the deposit, which changes
         * the offsets -- a quality arm, not a kernel. */
        for (int my = 0; my < hmb; my++)
            for (int mx = 0; mx < wmb; mx++) {
                int i = my * wmb + mx, lu = plu[i];
                if (!lu) continue;
                long intra = pi[i], inter = pin[i];
                double amount = (inh ? inh[i] : 0.0) + (double)intra * psw[i];
                double d = amount * (double)(intra - inter) / (double)intra;
                if ((lu & 1) && dst0) {
                    double a0 = lu == 3 ? d * bipw / 64.0 : d;
                    if (coh) splat_prop_qp(dst0, wmb, hmb, mx * 32 + pmv[i*4+0], my * 32 + pmv[i*4+1], a0);
                    else     splat_prop(dst0, wmb, hmb, mx * 8 + pmv[i*4+0], my * 8 + pmv[i*4+1], a0);
                }
                if ((lu & 2) && dst1) {
                    double a1 = lu == 3 ? d * (64 - bipw) / 64.0 : d;
                    if (coh) splat_prop_qp(dst1, wmb, hmb, mx * 32 + pmv[i*4+2], my * 32 + pmv[i*4+3], a1);
                    else     splat_prop(dst1, wmb, hmb, mx * 8 + pmv[i*4+2], my * 8 + pmv[i*4+3], a1);
                }
            }
    }
    /* --- Y264_DIRECT_LRVOTE: does the LOOKAHEAD already know which B direct mode
 * this content wants? -------------------------------------------------------
 *
 * The per-clip direct choice is worth up to 23 BD points and the signal that
 * picks it (Y264_DIRECT_SCORE=2, x264's skippability count) is computed during
 * full-resolution B analysis -- far too late to be a shot decision, and its
 * per-slice accumulator form is order-dependent, which is why Y264_DIRECT_AUTO
 * refuses above one thread. A decision made HERE would be a pure function of
 * pre-dispatch state, like every other lookahead output, so it would be
 * deterministic at any thread count. The only question is whether the lowres
 * data can pick the same winner.
 *
 * So SIMULATE both derivations on the lowres field, which is what mb-tree has
 * already computed, and score each against the lowres search result -- the best
 * estimate of true motion available before the encode:
 *
 *   temporal: mvL0 = (dsf * mvCol + 128) >> 8, mvL1 = mvL0 - mvCol, where mvCol
 *             is the FUTURE ANCHOR's own lowres motion vs its past reference --
 *             which is this B's past anchor, so the three POCs line up exactly
 *             as 8.4.1.2.3's do.
 *   spatial:  median of the left / top / top-right neighbours in raster order,
 *             snapped to zero where the co-located motion is within colZero.
 *
 * MEASUREMENT ONLY: reads the memo arrays Phase B just finished with, writes
 * nothing, and runs after the splat so it cannot perturb the accumulation order
 * the offsets depend on. Verified md5-identical on/off.
 *
 * Dedupe by POC when aggregating -- the mb-tree window overlaps, so one frame
 * is a source in several calls and prints several times. */
    if (direct_lrvote_on()) {
        int cz = direct_lrvote_cz();
        int qshift = coh ? 2 : 0;       /* the lowres MV field is quarter-pel
 * under the coherent search, integer
 * otherwise; SATD here is integer-pel */
        pixel *lrtmp = NULL;
        for (int s = 0; s < ns; s++) {
            if (src[s].is_anchor || s_futpoc[s] < 0 || s_pastpoc[s] < 0)
                continue;
            int sf = -1;                        /* the co-located picture */
            for (int k = 0; k < ns; k++)
                if (src[k].poc == s_futpoc[s]) { sf = k; break; }
            if (sf < 0 || s_pastpoc[sf] != s_pastpoc[s])
                continue;                       /* not the 8.4.1.2.3 topology */
            int td = src[sf].poc - s_pastpoc[sf];
            int tb = src[s].poc  - s_pastpoc[sf];
            if (td == 0) continue;
            if (tb < -128) tb = -128; else if (tb > 127) tb = 127;
            int tdc = td < -128 ? -128 : (td > 127 ? 127 : td);
            int tx = (16384 + abs(tdc / 2)) / tdc;
            int dsf = (tb * tx + 32) >> 6;
            if (dsf < -1024) dsf = -1024; else if (dsf > 1023) dsf = 1023;
            const int *pmvb = pp_pmv[s], *pmvc = pp_pmv[sf];
            const signed char *plub = pp_plu[s], *pluc = pp_plu[sf];
            /* Score by SATD (mode 2) rather than MV distance (mode 1). Mode 1
 * measures the wrong thing: its spatial arm predicts from the TRUE
 * neighbouring MVs, so it is strongest exactly where the motion field is
 * smooth -- which is exactly the content temporal direct suits. The bias
 * is anti-correlated with the label, not merely additive. Scoring the two
 * derived BI-predictions on pixels is x264's own question and has no such
 * advantage: a median that lands on the right MV still has to beat the
 * scaled co-located one on the block it actually predicts. */
            int satdmode = direct_lrvote_on() >= 2;
            const pixel *srclr = src[s].lr, *plr = s_pastlr[s], *flr2 = s_futlr[s];
            if (satdmode) {
                if (!plr || !flr2) continue;
                if (!srclr) {                   /* buffered B: no lowres kept */
                    if (!lrtmp) lrtmp = malloc((size_t)lw * lh * sizeof(pixel));
                    if (!lrtmp) continue;
                    downscale(lrtmp, lw, lh, src[s].full[0], e->pstride[0]);
                    srclr = lrtmp;
                }
            }
            long vt = 0, vs = 0, vn = 0;
            for (int my = 0; my < hmb; my++)
                for (int mx = 0; mx < wmb; mx++) {
                    int i = my * wmb + mx;
                    if (plub[i] != 3) continue;         /* need BOTH legs measured,
 * or there is no bidirectional
 * truth to score against */
                    /* And the CO-LOCATED block must have a list-0 leg, because
 * mvCol is read from it below. mbt_pa_source writes pmv[] only
 * on the path that also sets plu != 0; a block that went intra
 * leaves plu 0 with pmv UNTOUCHED, and these memo arrays are
 * malloc'd, never zeroed, and persist across walks. Without this
 * test mvCol is uninitialised heap or a previous frame's motion
 * whenever the anchor's co-located block went intra -- routine on
 * detail content -- which is the very read class this instrument
 * was written to study. */
                    if (!(pluc[i] & 1)) continue;
                    int a0x = pmvb[i*4+0], a0y = pmvb[i*4+1];
                    int a1x = pmvb[i*4+2], a1y = pmvb[i*4+3];
                    int cx = pmvc[i*4+0], cy = pmvc[i*4+1];
                    int t0x = (dsf * cx + 128) >> 8, t0y = (dsf * cy + 128) >> 8;
                    int t1x = t0x - cx, t1y = t0y - cy;
                    /* spatial: raster-order neighbour median, per list, with the
 * colZeroFlag snap.
 *
 * This APPROXIMATES 8.4.1.2.2 rather than implementing it. The
 * real derivation runs min_pos_ref + mv_predict_f over the coded
 * neighbours with the standard's availability rules; here a
 * neighbour counts only if both its legs were measured
 * (plu == 3), which drops single-leg neighbours in the frame
 * interior and not merely at the edges, and fewer than three
 * survivors fall back to the first rather than to a median
 * against zero. Both choices push the spatial arm's score, so
 * treat the T-vs-S share as an ordinal signal and not as a
 * calibrated estimate of either derivation's true error. */
                    int s0x, s0y, s1x, s1y;
                    if (abs(cx) <= cz && abs(cy) <= cz) {
                        s0x = s0y = s1x = s1y = 0;      /* colZeroFlag */
                    } else {
                        int nx[3], ny[3], mxs[3], mys[3], n = 0;
                        int nb[3] = { mx > 0 ? i - 1 : -1,
                                      my > 0 ? i - wmb : -1,
                                      (my > 0 && mx + 1 < wmb) ? i - wmb + 1 : -1 };
                        for (int k = 0; k < 3; k++)
                            if (nb[k] >= 0 && plub[nb[k]] == 3) {
                                nx[n] = pmvb[nb[k]*4+0]; ny[n] = pmvb[nb[k]*4+1];
                                mxs[n] = pmvb[nb[k]*4+2]; mys[n] = pmvb[nb[k]*4+3]; n++;
                            }
                        if (n == 0) { s0x = s0y = s1x = s1y = 0; }
                        else if (n < 3) { s0x = nx[0]; s0y = ny[0];
                                          s1x = mxs[0]; s1y = mys[0]; }
                        else { s0x = med3(nx[0], nx[1], nx[2]);
                               s0y = med3(ny[0], ny[1], ny[2]);
                               s1x = med3(mxs[0], mxs[1], mxs[2]);
                               s1y = med3(mys[0], mys[1], mys[2]); }
                    }
                    long et, es;
                    if (satdmode) {
                        pixel bt[64], bs[64];
                        if (!lr_bipred(bt, srclr, plr, flr2, lw, lh, mx, my,
                                       t0x, t0y, t1x, t1y, qshift) ||
                            !lr_bipred(bs, srclr, plr, flr2, lw, lh, mx, my,
                                       s0x, s0y, s1x, s1y, qshift))
                            continue;           /* either prediction leaves the plane */
                        const pixel *sb2 = srclr + (my * 8) * lw + mx * 8;
                        et = blk8_satd(sb2, lw, bt, 8);
                        es = blk8_satd(sb2, lw, bs, 8);
                    } else {
                        et = (long)abs(t0x - a0x) + abs(t0y - a0y)
                           + abs(t1x - a1x) + abs(t1y - a1y);
                        es = (long)abs(s0x - a0x) + abs(s0y - a0y)
                           + abs(s1x - a1x) + abs(s1y - a1y);
                    }
                    if (et < es) vt++; else if (es < et) vs++;
                    vn++;
                }
            fprintf(stderr, "lrvote poc=%d col=%d past=%d T=%ld S=%ld n=%ld\n",
                    src[s].poc, src[sf].poc, s_pastpoc[s], vt, vs, vn);
        }
        free(lrtmp);
    }
    free(s_past); free(s_fut); free(s_self); free(s_pastpoc); free(s_futpoc);
    free(s_pastlr); free(s_futlr); free(s_pastpush); free(s_futpush);
    if (split_on) { double t = tprof_ms(); g_mbt_split.pb += t - t_s; t_s = t; }

    /* finish F (slot 0): the reference's mb-tree finish pass. x264 does NOT centre (its
 * closed-loop rate controller absorbs the net shift). yah264's CRF is
 * open-loop (QP from complexity, no bit feedback), so under CRF/ABR the shift
 * is not absorbed -- centre the boost term to the frame mean so it redistributes
 * at ~constant rate (the AQ fold is left intact). Y264_MBTREE_CENTER overrides. */
    int center = 0;    /* behaviour-matched default; centring destroys the akiyo cross-frame
 * benefit. The CRF-reclaim fix belongs in rate control, not here. */
    { const char *v = getenv("Y264_MBTREE_CENTER"); if (v) center = atoi(v); }
    /* The anchor's own lowres intra costs. e->lr_intra is the driver's copy of
 * exactly this (encode_frame_core's lr-reuse memcpy from the popped entry's
 * d_intra), so the prefetch passes the entry's array directly and the
 * driver keeps its own -- same values either way. */
    const int32_t *F_intra = rq->anchor_dintra;
    /* The boost term's frame mean: the pivot the AC gain scales around. It needs
 * a pass before the offsets can be written, so that pass caches the ratios
 * rather than paying nmb log2 twice -- mb-tree sits on the serial floor and
 * this runs per anchor. */
    double ac_gain = mbt_ac_gain();
    double term_mean = 0;
    double *ratio_c = NULL;
    if (ac_gain != 1.0 && (ratio_c = malloc((size_t)nmb * sizeof(double)))) {
        for (int i = 0; i < nmb; i++) {
            long li = F_intra ? (long)F_intra[i] : (long)e->lr_intra[i];
            double intra = li < 1 ? 1.0 : (double)li;
            double ic = intra * Fw[i];
            ratio_c[i] = log2((ic + prop[i]) / ic);
            term_mean += ratio_c[i];
        }
        term_mean = term_mean * strength / nmb;
    }
    double boost_mean = 0;
    if (center) {
        for (int i = 0; i < nmb; i++) {
            long li = F_intra ? (long)F_intra[i] : (long)e->lr_intra[i];
            double intra = li < 1 ? 1.0 : (double)li;
            double ic = intra * Fw[i];
            boost_mean += strength * log2((ic + prop[i]) / ic);
        }
        boost_mean /= nmb;
    }
    double dbg_maxr = 0, dbg_sumr = 0; int omin = 100, omax = -100;
    long osum = 0;                                    /* for the mean mb-tree offset */
    /* MEASUREMENT ONLY: dump the per-MB propagation ratio, the same
 * quantity x264's offset carries as -strength*log2_ratio, so the two
 * accumulators can be compared directly. */
    double *rq_dbg_ratio = NULL;
    const char *rq_dbg_path = getenv("Y264_MBT_RATIO_DUMP");
    if (rq_dbg_path) rq_dbg_ratio = malloc((size_t)nmb * sizeof(double));
    for (int i = 0; i < nmb; i++) {
        long li = F_intra ? (long)F_intra[i] : (long)e->lr_intra[i];
        double intra = li < 1 ? 1.0 : (double)li;
        double ic = intra * Fw[i];                    /* intra_cost = intra * inv_qscale */
        double ratio = ratio_c ? ratio_c[i]
                               : log2((ic + prop[i]) / ic);   /* prop[0..nmb] = F's accumulator */
        if (rq_dbg_ratio) rq_dbg_ratio[i] = (double)aq_fold[i];   /* AQ term, not ratio */
        double term = strength * ratio;
        if (ratio_c) term = term_mean + (term - term_mean) * ac_gain;
        double offd = (double)aq_fold[i] - term + boost_mean;
        int sc = mbt_frac_on() ? 2 : 1;
        int off = (int)lround(offd * sc);
        if (off < -51 * sc) off = -51 * sc; else if (off > 51 * sc) off = 51 * sc;
        rq->out_off[i] = (int8_t)off;
        osum += off / sc;
        if (ratio > dbg_maxr) dbg_maxr = ratio; dbg_sumr += ratio;
        if (off < omin) omin = off; if (off > omax) omax = off;
    }
    /* mb_qp_delta is [-26, 25] (7.4.5) between consecutive coded macroblocks,
 * and the first one's is against the slice QP (offset 0), so the frame's
 * offsets must fit a 25-wide window that contains 0 -- the spread, not the
 * magnitude, is the constraint. Only a frame whose span already exceeds
 * that is touched (an extreme --qcomp/--aq-strength pair reached [-31, 18]
 * and emitted -30; review 2026-09-04); the defaults' spans are narrower,
 * so their output is unchanged. */
    {
        int sc = mbt_frac_on() ? 2 : 1;
        if (omax - omin > 25 * sc) {
            int lo = (int)lround((double)osum / nmb) * sc - 12 * sc;
            if (lo < -26 * sc) lo = -26 * sc;
            if (lo > 0) lo = 0;
            int hi = lo + 25 * sc;
            osum = 0; omin = INT_MAX; omax = INT_MIN;
            for (int i = 0; i < nmb; i++) {
                int off = rq->out_off[i];
                if (off < lo) off = lo; else if (off > hi) off = hi;
                rq->out_off[i] = (int8_t)off;
                osum += off / sc;
                if (off < omin) omin = off; if (off > omax) omax = off;
            }
        }
    }
    *rq->out_mean = (double)osum / nmb;               /* <=0, drives the CRF reclaim */
    if (getenv("Y264_MBTREE_DBG"))
        fprintf(stderr, "[mbtree] poc=%d na=%d ns=%d maxratio=%.2f meanratio=%.2f off=[%d,%d]\n",
                F_poc, na, ns, dbg_maxr, dbg_sumr / nmb, omin, omax);
    /* Task #73 part 3: the reference B's accumulator slice becomes ITS OWN field.
 * Same formula as F's below, on its own slice; only F's was converted before
 * because only F is being coded. */
    if (refb_anc > 0 && refb_buf >= 0 && e->bmbtree_off[refb_buf]) {
        /* Built exactly as F's is, on the B's OWN inputs. Three things must be
 * the B's and not the anchor's, and getting any of them wrong shows up
 * as a net QP shift rather than an obvious break:
 * - its AQ term, because mb_qp_pre treats mbtree_off as the COMBINED
 * x264-style offset and does NOT add aq_off on top;
 * - its intra costs, which weight the propagate ratio;
 * - the same mean-centring F gets, or the B takes a systematic shift. */
        const double *rp = prop + (size_t)refb_anc * nmb;
        double *b_invq = malloc((size_t)nmb * sizeof(double));
        float  *b_aq   = malloc((size_t)nmb * sizeof(float));
        if (b_invq && b_aq) {
            /* Costs ~1.2pp of wall on CIF. Reusing the source-side aqoff the
 * walk already computes atwould recover that, but a
 * probe that skips this entirely still leaves +2.2% -- the rest is
 * inherent (an extra anc[] target that every source now splats
 * onto), so the reuse is not worth its plumbing on its own. */
            mbtree_invqscale(e, e->bplane[refb_buf], e->pstride[0], wmb, hmb,
                             e->aq_strength, b_invq, b_aq);
            int lim = (e->bframes == 0 || mbtree_bfix()) ? 51 : 8;
            int sc = mbt_frac_on() ? 2 : 1;
            /* One intra pass, cached: the centring mean and the field both need
 * the same per-MB ratio, and blk8_intra_dispatch over every block is
 * the whole added cost of this feature. */
            double *bratio = malloc((size_t)nmb * sizeof(double));
            if (bratio) {
                double bmean = 0.0;
                for (int my = 0; my < hmb; my++)
                    for (int mx = 0; mx < wmb; mx++) {
                        int i = my * wmb + mx;
                        const pixel *sb = e->blowres[refb_buf] + (my * 8) * lw + mx * 8;
                        long li = blk8_intra_dispatch(sb, lw, mx, my);
                        double intra = li < 1 ? 1.0 : (double)li;
                        double ic = intra * pow(2.0, -(double)b_aq[i] * fw_k / 6.0);
                        bratio[i] = log2((ic + rp[i]) / ic);
                        bmean += strength * bratio[i];
                    }
                bmean /= nmb;
                /* THE REFERENCE B IS CENTRED AND ITS ANCHOR IS NOT. F's finish
 * above runs at boost_mean = 0 (behaviour-matched, `center` has been
 * 0 since the akiyo result), so the `+ bmean` here does not give
 * the B "the same mean-centring F gets" -- it gives it a
 * systematic +bmean QP, which at strength 2.0 and a mean ratio
 * of ~1.75 is about 3.5 QP of frame-level shift relative to the
 * anchor it is a reference for. x264 centres NOTHING, on any
 * frame, in its mb-tree finish pass: the offsets
 * are mean-negative, the frame rate controller absorbs the
 * shift, and that IS the cross-frame bit transfer.
 *
 * The x264 mode drops it, testing "the mean carried, no
 * mean-hold". The default keeps it: the reference-B field is
 * gated WITH this term measured in it (-2.09% median), so
 * removing it there is a separate arm with its own gate, not a
 * free correction. That arm is Y264_MBT_BCEN=0 (mbt_bcen),
 * which is how it gets priced on the shipped field. */
                double bcen = mbt_bcen() ? bmean : 0.0;
                for (int i = 0; i < nmb; i++) {
                    /* The non-xmode path pivots on its own mean (the +bmean is a
 * centring), so the gain scales what is left after it. */
                    double bterm = strength * bratio[i];
                    if (ac_gain != 1.0) bterm = bmean + (bterm - bmean) * ac_gain;
                    double offd = (double)b_aq[i] - bterm + bcen;
                    int off = (int)lround(offd * sc);
                    if (off < -lim * sc) off = -lim * sc;
                    else if (off > lim * sc) off = lim * sc;
                    e->bmbtree_off[refb_buf][i] = (int8_t)off;
                }
                e->bmbtree_valid[refb_buf] = 1;
                free(bratio);
            }
        }
        free(b_invq); free(b_aq);
    }
    if (rq_dbg_ratio) {
        FILE *fp = fopen(rq_dbg_path, "ab");
        if (fp) {
            int n = nmb;
            fwrite(&F_poc, sizeof(int), 1, fp);
            fwrite(&n, sizeof(int), 1, fp);
            fwrite(rq_dbg_ratio, sizeof(double), (size_t)n, fp);
            fclose(fp);
        }
        free(rq_dbg_ratio);
    }
    free(prop); free(Fw); free(ratio_c);
    if (split_on) g_mbt_split.fin += tprof_ms() - t_s;
    return 1;
}

/* mb-tree: estimate how much each anchor macroblock is depended upon by the
 * mini-GOP's buffered B pictures and, when the lookahead window is on, by all
 * future frames up to the next IDR (chain propagation above), and lower its QP
 * accordingly. For each B, a lowres motion search against the anchor gives, per
 * block, the fraction of its cost that comes from predicting off the anchor
 * (1 - inter/intra); that importance lands on the anchor blocks the MV points
 * at. The QP offset is centred to zero mean per frame, so it redistributes
 * quality at roughly constant rate rather than just spending more bits. */
/* wholebuf_path_only: the prefetch thread's mode. The legacy (non-x264) fallback
 * reads the API-owned ring cursors through la_chain_prop and writes the shared
 * e->mbtree_off, so it stays driver-side; when it would be taken the prefetch
 * returns 0 having changed nothing and the driver recomputes inline. Returns 1
 * if rq->out_off/out_mean were filled. */
static int compute_mbtree(yah264_encoder_t *e, const struct mbt_req *rq,
                          int wholebuf_path_only)
{
    pixel *const *anchor_src = rq->anchor_planes;
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs, nmb = wmb * hmb;
    int lw = e->lr_w, lh = e->lr_h;
    const pixel *anchor_lr = rq->anchor_lr;       /* anchor's lowres (post-swap) */
    *rq->out_mean = 0.0;                          /* x264 path sets the real value */
    double *prop = calloc(nmb, sizeof(double));
    double *anchor_invq = malloc(nmb * sizeof(double));
    double *bq = malloc(nmb * sizeof(double));
    float  *aq_fold = malloc(nmb * sizeof(float));
    if (!prop || !anchor_invq || !bq || !aq_fold) {
        free(prop); free(anchor_invq); free(bq); free(aq_fold);
        if (wholebuf_path_only) return 0;
        for (int i = 0; i < nmb; i++) rq->out_off[i] = 0;
        return 1;
    }
    /* Anchor's own AQ offset (to fold in) and inv_qscale (to weight its
 * self-importance in the finish ratio), from the SAME full-res 16x16
 * variance aq_analyze uses at encode time. */
    double t_iq = mbt_split_env() ? tprof_ms() : 0;
    mbtree_invqscale(e, anchor_src, e->pstride[0], wmb, hmb, e->aq_strength,
                     anchor_invq, aq_fold);
    if (mbt_split_env()) g_mbt_split.invq += tprof_ms() - t_iq;

    /* behaviour-matched whole-buffer path. Default ON:
 * BD-measured net win vs the legacy per-anchor heuristics -- helps motion/detail
 * and 720p, at a small accepted akiyo (near-static) regression. Y264_MBTREE_WHOLEBUF=0
 * restores the legacy path. */
    int derivedmode = 1;
    { const char *v = getenv("Y264_MBTREE_WHOLEBUF"); if (v) derivedmode = atoi(v); }
    if (derivedmode && compute_mbtree_wholebuf(e, rq, aq_fold)) {
        memcpy(e->code_panchor_lr, anchor_lr, (size_t)lw * lh * sizeof(pixel));
        e->code_panchor_have = 1;
        e->code_panchor_poc = rq->anchor_poc;
        e->code_panchor_push = rq->anchor_push;
        free(prop); free(anchor_invq); free(bq); free(aq_fold);
        return 1;
    }
    if (wholebuf_path_only) {
        free(prop); free(anchor_invq); free(bq); free(aq_fold);
        return 0;
    }

    if (e->la_depth > 0)
        la_chain_prop(e, prop);

    int prop_invq = 1;
    { const char *v = getenv("Y264_MBTREE_PROP_INVQ"); if (v) prop_invq = atoi(v); }
    /* (A) BOTH-LIST bipred propagation. Each buffered B (display order between the
 * previous anchor and the one coded now) references the current anchor as its
 * list-1 (future) ref and the previous anchor as list-0 (past). Attributing
 * its full importance to the current anchor over-boosts blocks a B near the
 * past anchor barely depends on. Split by temporal distance: the current
 * anchor gets the list-1 fraction tb/td (B closer to it depends more), where
 * td = anchor span, tb = B offset from the past anchor. The list-0 fraction
 * lands on the past anchor, which is already coded -- but the SAME anchor is
 * the past (list-0) ref for the NEXT mini-GOP's B's, still in the lookahead
 * window; those contribute their list-0 fraction here, so in steady state the
 * anchor collects both directions instead of a doubled single direction. */
    int bothlist = 1;
    { const char *v = getenv("Y264_MBTREE_BOTHLIST"); if (v) bothlist = atoi(v); }
    int td = e->poc - e->prev_anchor_poc;
    for (int b = 0; b < e->nbuf; b++) {
        downscale(e->lowres_tmp, lw, lh, e->bplane[b][0], e->pstride[0]);
        /* B's AQ-aware inv_qscale weights its propagated importance. */
        if (prop_invq)
            mbtree_invqscale(e, e->bplane[b], e->pstride[0], wmb, hmb,
                             e->aq_strength, bq, NULL);
        else
            for (int i = 0; i < nmb; i++) bq[i] = 1.0;
        double w1 = 1.0;                                /* list-1 (future) weight */
        if (bothlist && td > 0) {
            int tb = e->bpoc[b] - e->prev_anchor_poc;
            w1 = (double)tb / (double)td;
            if (w1 < 0) w1 = 0; else if (w1 > 1) w1 = 1;
        }
        for (int my = 0; my < hmb; my++)
            for (int mx = 0; mx < wmb; mx++) {
                const pixel *sb = e->lowres_tmp + (my * 8) * lw + mx * 8;
                int i = my * wmb + mx, mvx = 0, mvy = 0;
                long inter = blk8_inter(sb, lw, anchor_lr, lw, lw, lh, mx * 8, my * 8, &mvx, &mvy);
                long intra = blk8_intra_dispatch(sb, lw, mx, my);
                if (intra <= 0) continue;
                double pfrac = 1.0 - (double)inter / (double)intra;
                if (pfrac <= 0) continue;
                splat_prop(prop, wmb, hmb, mx * 8 + mvx, my * 8 + mvy,
                           (double)intra * bq[i] * pfrac * w1);
            }
    }

    /* (A) list-0 half: the NEXT mini-GOP's B's (in the lookahead window, coded
 * after this anchor) reference this anchor as their past/list-0 ref. Add
 * their list-0-weighted importance so the anchor is boosted for future
 * dependents too. Walk the window from the just-popped head until the next
 * typed anchor (its POC bounds the temporal span); stop at an IDR. */
    if (bothlist && e->la_depth > 0) {
        int cur_poc = e->poc, next_apoc = -1;
        int fb[64], nfb = 0;
        /* Self-limiting (stops at the first future anchor, reached well
 * inside la_depth-1 by the engage gate's la_depth>=bframes+3), so no
 * explicit depth cap is needed here -- only the ring-index base
 * changes for Y264_LA_BUF's extra capacity. */
        for (int i = 0; i < e->la_n && nfb < 64; i++) {
            int s = (e->la_head + i) % e->la_cap;
            struct la_entry *en = &e->la[s];
            if (!en->typed || en->is_idr) break;
            if (en->is_anchor) { next_apoc = en->since_val * 2; break; }
            fb[nfb++] = s;
        }
        int tdf = next_apoc - cur_poc;
        if (tdf > 0) {
            for (int k = 0; k < nfb; k++) {
                struct la_entry *en = &e->la[fb[k]];
                int tbf = en->since_val * 2 - cur_poc;
                double w0 = (double)(tdf - tbf) / (double)tdf;  /* toward this (past) anchor */
                if (w0 <= 0) continue;
                if (prop_invq)
                    mbtree_invqscale(e, en->plane, e->pstride[0], wmb, hmb,
                                     e->aq_strength, bq, NULL);
                else
                    for (int i = 0; i < nmb; i++) bq[i] = 1.0;
                for (int my = 0; my < hmb; my++)
                    for (int mx = 0; mx < wmb; mx++) {
                        const pixel *sb = en->lowres + (my * 8) * lw + mx * 8;
                        int i = my * wmb + mx, mvx = 0, mvy = 0;
                        long inter = blk8_inter(sb, lw, anchor_lr, lw, lw, lh,
                                                mx * 8, my * 8, &mvx, &mvy);
                        long intra = blk8_intra_dispatch(sb, lw, mx, my);
                        if (intra <= 0) continue;
                        double pfrac = 1.0 - (double)inter / (double)intra;
                        if (pfrac <= 0) continue;
                        splat_prop(prop, wmb, hmb, mx * 8 + mvx, my * 8 + mvy,
                                   (double)intra * bq[i] * pfrac * w0);
                    }
            }
        }
    }

    /* x264 COMBINED offset: the mb-tree boost is a log ratio whose intra
 * denominator carries the anchor's inv_qscale, so a flat block that AQ
 * already favours gets a DAMPENED boost (avoids the double-count that made
 * static content over-spend when aq_off and mbtree_off were summed). The
 * AQ offset is then folded in, and mb_qp_pre applies this single value. */
    double sum = 0;
    for (int i = 0; i < nmb; i++) {
        double ia = (e->lr_intra[i] < 1 ? 1.0 : (double)e->lr_intra[i]) * anchor_invq[i];
        prop[i] = log2((prop[i] + ia) / ia);         /* >= 0, larger = more depended on */
        sum += prop[i];
    }
    /* Under CRF/ABR the boost is non-centred (x264-style, rate controller
 * absorbs the shift). CQP centres only the boost term so it redistributes
 * at roughly constant rate; the folded AQ offset is left intact. For the
 * IPPP path (bframes 0) our CRF controller does not compensate for the
 * mb-tree mean, so an uncentred long-chain boost is a pure uncompensated
 * bit add; centre it so it redistributes at ~constant rate like CQP. */
    int centered = !(e->crf_on || e->abr_on) || e->bframes == 0;
    { const char *v = getenv("Y264_MBTREE_CENTER"); if (v) centered = atoi(v); }
    double mean = centered ? sum / nmb : 0.0;
    double strength = 2.0;                            /* 5*(1-qcomp), qcomp 0.6 */
    { const char *v = getenv("Y264_MBTREE_STRENGTH"); if (v) strength = atof(v); }
    /* (B) CONTENT-ADAPTIVE strength. x264 uses a flat 2.0, but our fixed
 * allocation is already good on static content (akiyo at parity) and short
 * on high-motion/detail (stefan +21%). Modulate the mb-tree redistribution
 * amplitude per-frame by a cheap motion signal already computed for the
 * anchor: R = Sum min(inter,intra) / Sum intra over the frame's lowres MBs
 * (the scene-cut ratio). R is small when the anchor tracks its predecessor
 * cheaply (static/low-motion) and near 1 when prediction is as costly as
 * intra (high motion / fine detail). Lower strength where fixed allocation
 * already wins, raise it where motion needs sharper redistribution:
 * strength *= clamp(aint + aslope*R, lo, hi). The AQ fold is untouched. */
    int adapt = 1;
    { const char *v = getenv("Y264_MBTREE_ADAPT"); if (v) adapt = atoi(v); }
    if (adapt) {
        double aint = 0.57, aslope = 1.0, lo = 0.5, hi = 1.6;
        { const char *v = getenv("Y264_MBTREE_AINT");   if (v) aint = atof(v); }
        { const char *v = getenv("Y264_MBTREE_ASLOPE"); if (v) aslope = atof(v); }
        { const char *v = getenv("Y264_MBTREE_ALO");    if (v) lo = atof(v); }
        { const char *v = getenv("Y264_MBTREE_AHI");    if (v) hi = atof(v); }
        double si = 0, sm = 0;
        for (int i = 0; i < nmb; i++) {
            double it = e->lr_intra[i] < 1 ? 1.0 : (double)e->lr_intra[i];
            double in = (double)e->lr_inter[i];
            si += it; sm += in < it ? in : it;
        }
        double R = si > 0 ? sm / si : 1.0;
        double m = aint + aslope * R;
        if (m < lo) m = lo; else if (m > hi) m = hi;
        strength *= m;
    }
    /* IPPP carries the offset with a far wider bound than the old +/-8: a long
 * P chain legitimately drives a strongly-referenced block tens of QP steps
 * down, and the hard +/-8 clamp collapsed that gradient (every block
 * saturating turns mb-tree into a flat QP shift). mb_qp_pre re-clamps the
 * final QP to [0,51], so this only preserves the relative gradient; the
 * bound stays inside int8_t. bframes>0 keeps +/-8 (byte-identical shipped
 * path; its shorter anchor chain rarely reaches the clamp). */
    int lim = (e->bframes == 0 || mbtree_bfix()) ? 51 : 8;
    for (int i = 0; i < nmb; i++) {
        int sc = mbt_frac_on() ? 2 : 1;
        int off = (int)lround((aq_fold[i] - strength * (prop[i] - mean)) * sc);
        if (off < -lim * sc) off = -lim * sc; else if (off > lim * sc) off = lim * sc;
        rq->out_off[i] = (int8_t)off;
    }
    free(prop); free(anchor_invq); free(bq); free(aq_fold);
    return 1;
}

/* --- Phase-A pre-warm (the part of mb-tree that can actually be led) -----
 *
 * WHAT THE SPLIT SAYS. Y264_MBT_SPLIT on samsung t18: of the driver's 94.8 ms
 * of mb-tree, Phase A is 70.8 (74%), Phase B 18.4, the anchor's own invqscale
 * 4.1, binding and finish 1.5. Phase B is the float accumulation whose ORDER
 * is the bits, so it is pinned to the anchor. Phase A is not pinned to
 * anything: it is a pure function of (source frame, its two bracketing
 * anchors), it already lives in a per-owner memo, and that memo is already
 * reused across anchors -- 1020 sources over 47 anchors with only 195 misses.
 * Computing a memo earlier cannot change a value, only who paid for it, and
 * that is the whole byte-identity argument.
 *
 * WHY THIS IS THE LEAD THE PREFETCH COULD NOT GET. Whole-anchor prefetch is
 * pinned one encode call before its anchor, because it needs the mini-GOP's
 * buffered B pixels and those are copied at their pops -- and one call is
 * ~0.3 ms of driver work, measured, so there is nothing to hide behind. Phase
 * A has no such dependency: a window entry's brackets are typed by the chain
 * ~bframes+1 steps after the entry itself, tens of frames before any anchor
 * consumes it. So this runs once per anchor and is joined at the NEXT one,
 * with a whole mini-GOP -- the burst, the drain, the B calls -- to run in.
 *
 * WHY IT CANNOT RACE. The warm only ever touches ring positions >= bframes+2
 * forward of the head it was launched with. At most bframes+1 pops happen
 * before the join, and a pop is the only thing that writes a window entry's
 * memo (the steal into e->bmbt), so the warm's range and the driver's writes
 * are disjoint by exactly one entry of margin. The join happens before
 * compute_mbtree, which is the only other writer.
 *
 * A KEY THAT MISSES IS FREE. If the warm derives different bracketing anchors
 * from the ones the consuming walk derives, the walk's `need[]` check sees a
 * stale key and recomputes exactly as it does today. So this can cost speed,
 * never bits -- which is why the derivation below mirrors the walk's rather
 * than sharing code with it: a shared helper drifting would be silent. */
static void mbt_warm_window(yah264_encoder_t *e, int head, int navail,
                            long pop_seq, long pushed)
{
    enum { MAXA = 64, MAXS = 192 };
    int lo = e->bframes + 2;                    /* pop margin, see above */
    /* Under WIDE chains the pop-margin assumption above ("at most bframes+1
     * pops happen before the join") breaks: up to K bursts retire late, so
     * the walk's window can slide K mini-GOPs past the head this warm
     * captured and reach entries the warm is still computing -- TSan reports
     * two pool workers in mbt_pa_source on one source slice. Widen the margin
     * to the wide pipeline's whole reach; the warm loses a little frontier
     * coverage only where width is actually on. */
    if (stair_wide_engaged_cfg(e))
        lo = (Y264_STAIR_K + 1) * (e->bframes + 1) + 1;
    /* THE SCAN MUST REACH PAST THE WALK'S CAP. The walk stops at la_depth-2,
 * so the entries it will newly see NEXT time -- the only ones that are
 * ever fresh misses, 174 of samsung's 195 -- sit beyond that cap, in the
 * ring capacity Y264_LA_BUF adds. Warming only what the walk can already
 * see finds every memo valid and computes NOTHING.
 *
 * Past the cap there is no la_th_wait_mbtree to lean on, so the legality
 * of reading `typed` has to be established here: entry at offset k is
 * typed once chain step pop_seq+k+2 completes (the same numbering
 * la_th_wait_mbtree derives its bound from), so an acquire load of the
 * chain's progress bounds the scan. No waiting -- a chain that has not got
 * there yet just means a shorter warm this pass. */
    /* THE BOUND, and why it carries a margin. Entry at offset o is typed by
 * chain step pop_seq+o+2 (la_th_wait_mbtree derives its own bound from the
 * same numbering), so done >= D makes offsets up to D-pop_seq-2 typed. The
 * driver may read exactly that far because it reads only the struct's type
 * fields. This warm holds ENTRY POINTERS across a parallel_for that runs
 * long after the load, while the chain keeps stepping -- and la_finalize
 * writes an entry's type fields and reaches back another bframes+1 entries
 * to fill B pair legs. TSan caught the difference on sintel 300f at the
 * unpadded bound, on the la_entry array itself. The margin below is the
 * finalize's whole reach plus one; the warm gives up a couple of entries
 * of reach for it, which costs a memo it would have warmed next pass
 * anyway. A racy warm would be worth exactly nothing, and this one is
 * already worth close to nothing. */
    /* WAIT, DO NOT BOUND. Reading the chain's progress counter and scanning as
 * far as that allows is sound on paper -- entry at
 * offset o is typed by step pop_seq+o+2, the same numbering
 * la_th_wait_mbtree derives its bound from -- and TSan still catches it on
 * sintel 300f, twice, on la_finalize's write of is_anchor/typed against
 * this thread's read of them. Reading a progress counter and then reading
 * what it licenses is a different thing from waiting for the step: the
 * bound has to be re-proved against every writer the chain has, including
 * the finalize's reach back over earlier entries, and this warm holds
 * entry pointers across a parallel_for that outlives the load.
 *
 * So it waits, which is what the whole round is about anyway, and takes
 * the driver's own guarantee one step further out. `pushed` is API-owned
 * and captured at the launch, so the step is known to be coming; the
 * chain drains everything enqueued, so this cannot hang. */
    if (!e->la_th)
        return;                     /* chain inline: no producer to wait on */
    int lim = navail;
    long room = pushed - pop_seq - 2;           /* offsets the chain will type */
    if (room < lim) lim = (int)(room < 0 ? 0 : room);
    if (lim <= lo)
        return;
    la_th_wait_step(e, pop_seq + lim + 1);      /* offsets 0..lim-1 now typed */
    if (atomic_load_explicit(&e->la_th->done_atom, memory_order_acquire) <
        pop_seq + lim + 1)
        return;                                 /* close raced the wait */
    if (lim <= lo)
        return;
    int wcap = e->la_depth > 1 ? e->la_depth - 2 : 0;
    /* Warm the NEXT ANCHOR'S WALK, not "the window": the walk's own cap is
 * what generates the fresh misses, so a warm that stops where this
 * anchor's walk stopped finds every memo valid and computes nothing (0
 * computed). Find the next anchor F'
 * at offset d -- it is anc[0] of its own walk, exactly as F is of this one
 * -- and mirror the walk it will run: sources at offsets d+1 .. d+wcap,
 * breaking at the same untyped/IDR conditions, bracketed by the same
 * anchors. Reproducing the walk's TRUNCATION matters as much as its
 * content: a source the walk will key with no future bracket must be
 * warmed with no future bracket, or the key misses and the driver pays
 * twice. Requiring both brackets instead left 68 fresh misses. */
    int d = -1;
    for (int i = 0; i < lim; i++) {
        struct la_entry *en = &e->la[(head + i) % e->la_cap];
        if (!en->typed) break;
        if (en->is_anchor) { d = i; break; }
    }
    if (d < 0)
        return;
    struct mbt_anchor anc[MAXA];
    struct mbt_source src[MAXS];
    int na = 0, ns = 0;
    {
        struct la_entry *fp = &e->la[(head + d) % e->la_cap];
        anc[na].poc = fp->since_val * 2; anc[na].lr = fp->lowres;
        anc[na].dintra = fp->d_intra; anc[na].full = fp->plane;
        anc[na].push = fp->push_idx; na++;
    }
    for (int i = d + 1; i < lim && i <= d + wcap && ns < MAXS; i++) {
        struct la_entry *en = &e->la[(head + i) % e->la_cap];
        if (!en->typed || en->is_idr)
            break;
        if (en->is_anchor && na < MAXA) {
            anc[na].poc = en->since_val * 2; anc[na].lr = en->lowres;
            anc[na].dintra = en->d_intra; anc[na].full = en->plane;
            anc[na].push = en->push_idx; na++;
        }
        if (i < lo)
            continue;                           /* inside the pop margin */
        src[ns].poc = en->since_val * 2; src[ns].is_anchor = en->is_anchor;
        src[ns].lr = en->lowres; src[ns].dintra = en->d_intra;
        src[ns].full = en->plane; src[ns].bbuf = -1; src[ns].push = en->push_idx;
        src[ns].laidx = (head + i) % e->la_cap; src[ns].laoff = i;
        ns++;
    }
    if (ns < 1)
        return;
    int nmb = e->width_in_mbs * e->height_in_mbs;
    int *s_past = malloc((size_t)ns * sizeof(int));
    int *s_fut  = malloc((size_t)ns * sizeof(int));
    int *s_pastpoc = malloc((size_t)ns * sizeof(int));
    int *s_futpoc  = malloc((size_t)ns * sizeof(int));
    const pixel **s_pastlr = malloc((size_t)ns * sizeof(*s_pastlr));
    const pixel **s_futlr  = malloc((size_t)ns * sizeof(*s_futlr));
    long *s_pastpush = malloc((size_t)ns * sizeof(long));
    long *s_futpush  = malloc((size_t)ns * sizeof(long));
    long *pp_pi[MAXS], *pp_pin[MAXS];
    signed char *pp_plu[MAXS];
    int *pp_pmv[MAXS];
    double *pp_psw[MAXS];
    unsigned char need[MAXS];
    if (!s_past || !s_fut || !s_pastpoc || !s_futpoc || !s_pastlr || !s_futlr
        || !s_pastpush || !s_futpush) {
        free(s_past); free(s_fut); free(s_pastpoc); free(s_futpoc);
        free(s_pastlr); free(s_futlr); free(s_pastpush); free(s_futpush);
        return;
    }
    int nneed = 0;
    const int settled = e->la_th
        ? lim - 2 - e->bframes
          - (stair_wide_engaged_cfg(e) ? Y264_STAIR_K * (e->bframes + 1) : 0)
        : INT_MAX;
    for (int s = 0; s < ns; s++) {
        int past = -1, fut = -1, bp = -(1 << 30), bf = (1 << 30);
        for (int a = 0; a < na; a++) {
            if (anc[a].poc < src[s].poc && anc[a].poc > bp) { bp = anc[a].poc; past = a; }
            if (anc[a].poc > src[s].poc && anc[a].poc < bf) { bf = anc[a].poc; fut  = a; }
        }
        /* A window source is never a buffered B, so the walk's code_panchor
 * substitution for a missing past leg cannot apply here: no bracket
 * means INT_MIN / NULL, exactly as the walk computes it. But a source
 * whose brackets are not BOTH resolved yet is skipped rather than
 * computed under a provisional key: the consuming walk would derive
 * the real bracket, miss, and recompute -- paying for the work twice
 * and warming nothing. It comes back into range next pass. */
        s_past[s] = past; s_fut[s] = fut;
        s_pastpoc[s] = past >= 0 ? anc[past].poc : INT_MIN;
        s_futpoc[s]  = fut  >= 0 ? anc[fut].poc  : INT_MIN;
        s_pastlr[s]  = past >= 0 ? anc[past].lr : NULL;
        s_futlr[s]   = fut  >= 0 ? anc[fut].lr  : NULL;
        s_pastpush[s] = past >= 0 ? anc[past].push : 0;
        s_futpush[s]  = fut  >= 0 ? anc[fut].push : 0;
        struct la_entry *ce = &e->la[src[s].laidx];
        if (!ce->mbt_pa_pi) {
            ce->mbt_pa_pi  = malloc((size_t)nmb * sizeof(long));
            ce->mbt_pa_pin = malloc((size_t)nmb * sizeof(long));
            ce->mbt_pa_plu = malloc((size_t)nmb);
            ce->mbt_pa_pmv = malloc((size_t)nmb * 4 * sizeof(int));
            ce->mbt_pa_psw = malloc((size_t)nmb * sizeof(double));
        }
        pp_pi[s] = ce->mbt_pa_pi;   pp_pin[s] = ce->mbt_pa_pin;
        pp_plu[s] = ce->mbt_pa_plu; pp_pmv[s] = ce->mbt_pa_pmv;
        pp_psw[s] = ce->mbt_pa_psw;
        need[s] = !(ce->mbt_pa_valid &&
                    ce->mbt_pa_past_poc == s_pastpoc[s] &&
                    ce->mbt_pa_fut_poc  == s_futpoc[s]);
        if (!pp_pi[s] || !pp_pin[s] || !pp_plu[s] || !pp_pmv[s] || !pp_psw[s])
            need[s] = 0;                        /* OOM: leave it to the walk */
        /* WARM ONLY WHAT THE WALK WOULD COMPUTE THE SAME WAY.
 *
 * Two of Phase A's inputs are settled-bounded -- the bleg pair reuse
 * and the A1 pair seed -- and the bound is an OFFSET test. The warm's
 * offsets are measured from the launch-time ring head; the walk's from
 * its anchor, which sits `d` entries further in. So the same source is
 * `laoff` here and `laoff - d` there, tested against two different
 * bounds, and the two paths can disagree about whether its stored pair
 * is usable. A source the warm declines the reuse for and the walk
 * accepts it for gets DIFFERENT Phase-A output depending on which path
 * happened to run it -- which is scheduling. That is why the warm's
 * output was not the walk's, and why the default emitted several
 * distinct bitstreams per twelve runs of one config on a loaded box.
 *
 * The two bounds cannot simply be equalised: the walk's is the more
 * permissive of the two BECAUSE it runs later, and lending it to the
 * warm would read entries the finalize is still filling. So the warm
 * keeps its own (safe) bound and additionally declines any source the
 * walk would judge differently. What it gives up is coverage on the
 * frontier sources; those cost the walk what they always did. */
        if (need[s]) {
            long wk = (long)d + (long)e->la_depth - 4 - e->bframes
                    - (stair_wide_engaged_cfg(e) ? Y264_STAIR_K * (e->bframes + 1) : 0);
            long lim_off = (long)settled < wk ? (long)settled : wk;
            if ((long)src[s].laoff > lim_off)
                need[s] = 0;
        }
        nneed += need[s];
    }
    if (mbt_split_env()) {          /* probe accounting: several GOP workers
 * fold into one global, so take the lock */
        pthread_mutex_lock(&g_mbt_split_mx);
        g_mbt_split.wcalls++; g_mbt_split.wsrcs += ns; g_mbt_split.wdone += nneed;
        pthread_mutex_unlock(&g_mbt_split_mx);
    }
    if (nneed) {
        struct mbt_pa_ctx pac = {
            .e = e, .src = src, .nmb = nmb, .wmb = e->width_in_mbs,
            .hmb = e->height_in_mbs, .lw = e->lr_w, .lh = e->lr_h,
            .coh = mbt_coh(), .mvlambda = mbtree_mvlambda(), .cost_inf = 1L << 40,
            .s_past = s_past, .s_fut = s_fut, .s_pastlr = s_pastlr,
            .settled_off = settled,
            .s_futlr = s_futlr, .s_pastpoc = s_pastpoc, .s_futpoc = s_futpoc,
            .s_pastpush = e->gpq ? s_pastpush : NULL,
            .s_futpush  = e->gpq ? s_futpush  : NULL,
            .pp_pi = pp_pi, .pp_pin = pp_pin, .pp_plu = pp_plu,
            .pp_pmv = pp_pmv, .pp_psw = pp_psw, .need = need,
        };
        if (e->gpq) {               /* chain-wait bound: entries enumerated above */
            long mp = 0;
            for (int s2 = 0; s2 < ns; s2++) if (src[s2].push > mp) mp = src[s2].push;
            for (int a2 = 0; a2 < na; a2++) if (anc[a2].push > mp) mp = anc[a2].push;
            pac.gpq_maxpush = mp;
        }
        int pool_nt = e->pool ? ntp_pool_nthreads(e->pool) : 0;
        /* DO NOT PUT THE GPU IN THE WARM: built and measured 18-123% WORSE.
         * The warm's GPU wait sits inside the launch handshake the DRIVER
         * blocks on, so a warm slower than one mini-GOP serializes the whole
         * pipeline -- the per-dispatch fixed cost moved rather than removed. A
         * real per-frame batching needs submit-at-push (brackets unknown
         * there) or a two-anchor pipelined warm (submit at warm n,
         * collect+compute at n+1, which delays memo readiness past the
         * consuming walk as the phases stand). The batch helpers, the shared
         * Metal context and the second handle (e->gpu_warm) are the sound
         * substrate for that rework. */
        int sub0[MAXS], sub1[MAXS], subheld = 0;
        if (pac.coh && (subheld = mbt_sub_claim(e))) {
            { int nb = mbt_sub_plan(e, ns, need, s_pastlr, s_futlr, s_pastpush, s_futpush, sub0, sub1, pool_nt);
          if (mbt_split_env()) g_mbt_split.sub_built += nb; }
            pac.s_sub0 = sub0; pac.s_sub1 = sub1;
            if (g_lrs_census > 0) {
                g_lrs_plan[0]++;
                for (int s2 = 0; s2 < ns; s2++) if (need[s2]) {
                    if (s_pastlr[s2]) { g_lrs_plan[2]++; if (sub0[s2] >= 0) g_lrs_plan[3]++; else g_lrs_plan[4]++; }
                    if (s_futlr[s2])  { g_lrs_plan[2]++; if (sub1[s2] >= 0) g_lrs_plan[3]++; else g_lrs_plan[4]++; }
                }
            }
        } else if (g_lrs_census > 0) {
            if (!(pac.coh)) g_lrs_plan[5]++; else g_lrs_plan[1]++;
        }
        if (pool_nt > 1 && mbt_ensure_ws(e, pool_nt)) {
            ntp_prof_tag("mbtree_warm"); ntp_prio_hint();
            ntp_parallel_for(e->pool, ns, mbt_pa_source, &pac);
        } else if (mbt_ensure_ws(e, 1)) {
            for (int s = 0; s < ns; s++) mbt_pa_source(&pac, 0, s);
        }
        if (subheld) mbt_sub_release(e);
    }
    free(s_past); free(s_fut); free(s_pastpoc); free(s_futpoc);
    free(s_pastlr); free(s_futlr); free(s_pastpush); free(s_futpush);
}

/* --- mb-tree prefetch (Y264_MBT_PRE, default OFF) -----------------------
 *
 * WHY. On the default bframes-3 shape at 18 threads the encode hides behind
 * the GOP driver thread, and after the lookahead chain is decoupled
 * (Y264_LA_THREAD + a working Y264_LA_BUF lead) compute_mbtree is what is
 * left standing on the driver -- 36 ms of a ~148 ms t18 wall, measured, and
 * worth 18.6 ms of wall if it goes away entirely.
 * x264 runs mb-tree on its lookahead thread; this runs it on a
 * dedicated one, launched one encode call before the driver needs it.
 *
 * WHEN. The launch point is the pop of the LAST buffered B of a mini-GOP --
 * the earliest moment every input is final:
 * - the B's are in e->bplane (copied at their own pops) and their Phase-A
 * memos have been stolen into e->bmbt; no further B joins, because the
 * next ring entry is the anchor;
 * - the anchor's own entry is at la_head, typed, with its plane and lowres
 * written by its chain step long ago;
 * - the window entries la_head+1 .. la_head+la_depth-2 are typed.
 * The last of those is the only one in question, and it is exactly what
 * Y264_LA_BUF >= 1 buys: without spare ring capacity the window's last entry
 * is still the newest push and still untyped one call early, the walk would
 * break one entry short, and the bits would move. So the gate REQUIRES
 * la_buf >= 1 (x264's own --sync-lookahead is bframes+1) and otherwise stays
 * on the driver.
 *
 * WHY IT IS STILL THE SAME BITS. Between launch and latch the driver runs
 * exactly one encode prologue, which touches: e->la[] slot la_head-1 (the
 * B popped last call -- pad_input_to overwrites it, and the prefetch does not
 * read it, taking its B pixels from bplane); the newly pushed entry's
 * mbt_pa_valid and, with the chain inline, its lowres/d_intra plus the
 * finalize of the entry before it. Those sit at ring positions la_depth-1 and
 * beyond, past the window's last entry at la_depth-2 -- again exactly what
 * la_buf >= 1 separates. Nothing else the walk reads is written by anyone
 * until the latch. Same inputs, same order, same code: byte-identical, and
 * that is the gate.
 *
 * The latch re-checks the anchor's identity (ring entry + POC) and falls back
 * to computing inline on any mismatch -- b-adapt can retype, and a scene cut
 * can renumber -- so a stale or wrong-anchor prefetch costs time, never bits. */
static void mbt_pre_main(void *arg)
{
    yah264_encoder_t *e = arg;
    struct mbt_pre *mp = e->mbtp;
    pthread_mutex_lock(&mp->mx);
    for (;;) {
        while (!mp->exit && !(mp->req && !mp->done))
            pthread_cond_wait(&mp->cv_req, &mp->mx);
        if (mp->exit)
            break;
        struct mbt_req rq = mp->rq;
        int lead = mp->lead, aidx = mp->anchor_idx, kind = mp->kind;
        int wh = mp->warm_head, wn = mp->warm_navail;
        long wps = mp->warm_popseq, wpu = mp->warm_pushed;
        long need = mp->need;
        pthread_mutex_unlock(&mp->mx);
        int ok = 0, poc = 0;
        if (kind) {
            double t0 = tprof_ms();
            mbt_warm_window(e, wh, wn, wps, wpu);
            mp->ms_compute += tprof_ms() - t0;
        } else if (!lead) {
            ok = compute_mbtree(e, &rq, 1);
        } else {
            /* THE LEAD. Every ring read below is behind this wait, so the
 * launch never had to know whether the chain had caught up -- it
 * only had to know the step would arrive (need <= pushed, checked
 * API-side). Same entries, same state, same order as the driver's
 * own la_th_wait_mbtree would have seen: byte-identical. */
            double t0 = tprof_ms();
            la_th_wait_step(e, need);
            double t1 = tprof_ms();
            mp->ms_gap += t0 - mp->t_launch;      /* dbg only: not under mx, and
 * only ever read after join */
            mp->ms_chainwait += t1 - t0;
            struct la_entry *an = &e->la[aidx];
            int wcap = e->la_depth - 2;
            struct la_entry *last = &e->la[(aidx + wcap) % e->la_cap];
            if (atomic_load_explicit(&e->la_th->done_atom, memory_order_acquire) >= need &&
                an->typed && an->is_anchor && !an->is_idr && last->typed) {
                rq.anchor_planes = an->plane;
                rq.anchor_lr     = an->lowres;
                rq.anchor_dintra = an->d_intra;
                rq.anchor_poc    = poc = an->since_val * 2;
                rq.anchor_push   = an->push_idx;
                ok = compute_mbtree(e, &rq, 1);
            }
            mp->ms_compute += tprof_ms() - t1;
        }
        pthread_mutex_lock(&mp->mx);
        if (lead) mp->anchor_poc = poc;
        mp->ok = ok;
        mp->done = 1;
        pthread_cond_broadcast(&mp->cv_done);
    }
    pthread_mutex_unlock(&mp->mx);
}

/* Launch the next anchor's mb-tree, if this pop was the last B before it.
 * Best-effort throughout: any condition that is not plainly satisfiable leaves
 * the work on the driver. Called from the API thread with no request in
 * flight (the previous one was latched at the previous anchor). */
static void mbt_pre_launch(yah264_encoder_t *e)
{
    struct mbt_pre *mp = e->mbtp;
    if (!mp || mp->warm || !e->mbtree_on || e->mbtree_skip ||
        e->la_depth <= 1 || e->la_buf < 1)
        return;
    if (e->la_n < 1)
        return;
    int wcap = e->la_depth - 2;
    if (wcap < 1 || e->la_n - 1 < wcap)
        return;
    struct la_entry *an = &e->la[e->la_head];        /* the anchor, not yet popped */
    /* With the chain on its own thread, typing is only safe to read once the
 * chain has completed the step that types the window's last entry --
 * pop_seq + la_depth here, one more than at the anchor's own pop because
 * pop_seq has not been bumped for the anchor yet.
 *
 * TESTED (the original prefetch) vs WAITED (Y264_MBT_LEAD). The test looks
 * conservative and is in fact the whole defect: with a lead, the chain is
 * BY CONSTRUCTION sitting at its allowed lag (la_buf steps behind the
 * newest push), which is exactly one step short of this condition, so the
 * test declines on most anchors -- 23 of ~45 measured on samsung -- and
 * the ones it accepts are the ones the driver would not have waited for
 * anyway. Nothing was ever led. Waiting instead moves the chain wait OFF
 * the driver too, which is the second half of the prize.
 *
 * A wait needs a step that is guaranteed to arrive: the chain drains
 * everything enqueued, so `need <= pushed` (API-owned, read here) is that
 * guarantee. Past the last push -- the flush tail -- there is nothing to
 * wait for and the anchor stays on the driver, as before. */
    long need = e->la_th ? e->la_th->pop_seq + e->la_depth : 0;
    if (mp->lead && e->la_th) {
        if (need > e->la_th->pushed)
            return;                                  /* flush tail: driver path */
    } else {
        if (e->la_th &&
            atomic_load_explicit(&e->la_th->done_atom, memory_order_acquire) < need)
            return;
        if (!an->typed || !an->is_anchor || an->is_idr)
            return;
        struct la_entry *last = &e->la[(e->la_head + wcap) % e->la_cap];
        if (!last->typed)                            /* window short: driver path */
            return;
    }
    pthread_mutex_lock(&mp->mx);
    /* A request is normally consumed by the very next anchor's mbt_resolve, so
 * none is in flight here. Retire one defensively rather than assume it:
 * overwriting rq under a running worker would let it publish the OLD
 * anchor's offsets under the NEW anchor's identity, which the latch would
 * then accept. Cheap, and it keeps the invariant local. */
    while (mp->req && !mp->done)
        pthread_cond_wait(&mp->cv_done, &mp->mx);
    mp->req = 0; mp->done = 0;
    /* In lead mode the four anchor fields below are filled by the WORKER, after
 * its wait: since_val (hence the POC) is written by the chain's finalize,
 * so reading it here would be reading ahead of the producer. head/navail
 * are ring cursors, API-owned, and already written for the post-pop state
 * the walk runs in -- they are the same either way. */
    mp->rq.anchor_planes = an->plane;
    mp->rq.anchor_lr     = an->lowres;
    mp->rq.anchor_dintra = an->d_intra;
    mp->rq.anchor_poc    = an->since_val * 2;
    mp->rq.anchor_push   = an->push_idx;
    mp->rq.head          = (e->la_head + 1) % e->la_cap;
    mp->rq.navail        = e->la_n - 1;
    mp->rq.out_off       = mp->out_off;
    mp->rq.out_mean      = &mp->out_mean;
    mp->anchor     = an;
    mp->anchor_poc = mp->rq.anchor_poc;
    mp->anchor_idx = e->la_head;
    mp->need       = need;
    mp->t_launch   = tprof_ms();
    mp->req = 1; mp->done = 0; mp->ok = 0;
    pthread_cond_signal(&mp->cv_req);
    pthread_mutex_unlock(&mp->mx);
}

/* --- mb-tree oracle probe (Y264_MBT_REC / Y264_MBT_PLAY, default OFF) ---
 *
 * PROBE, not a feature: bounds the prize of computing mb-tree off the
 * driver's critical path with unbounded lead (x264 runs it on its lookahead
 * thread, genuinely ahead; our MBT_PRE launches one call early and the
 * driver waits out nearly the whole latency -- measured, samsung t18: the
 * TP_MBTREE bucket is 118 ms with MBT_PRE latched as without it). REC=path
 * records every anchor's resolved offsets; PLAY=path replays them with the
 * chain rendezvous and the walk skipped. The gate is byte-identity of the
 * .264 against the recording run, EVERY time the oracle is used.
 *
 * Three traps are designed out, each of which produces a wrong number:
 * - the KEY is (anchor POC, FNV-64 of the anchor's lowres plane), not a
 * display index: GOP workers restart their frame counters, so an index
 * key collides across workers and replays one GOP's offsets into
 * another (+1.0% bits on sintel 300f on any multi-GOP shape), the same
 * trap a poc key hits on multi-IDR clips. The content hash makes the
 * key worker-agnostic; a residual collision needs two anchors with
 * identical poc AND byte-identical lowres planes, and the mandatory
 * identity gate is the backstop for that.
 * - a replay MISS must take the untouched path: full la_th_wait_mbtree,
 * then compute. Skipping the wait in play mode unconditionally lets a
 * miss walk possibly-untyped entries.
 * - a replay HIT must reproduce compute_mbtree's side effect: the
 * code_panchor_lr copy (+ have/poc), which later walks' buffered-B past
 * leg reads. Offsets alone are not the whole contract. */
/* The walk has TWO outputs under Y264_MBT_BREF: the anchor's field
 * and the reference B's (e->bmbtree_off[1], the only slot stair_refb_poc ever
 * names). A replay that carries only the first leaves the reference B with no
 * field at all, which silently breaks this probe: every clip's `play` run
 * comes back +3.7% larger and the identity gate voids.
 * So a record carries the B slot's post-walk state too, verbatim, including the
 * case where this walk did not touch it (the flag is sticky, so replaying the
 * recorded value is what keeps play/base in step by induction). */
#define MBT_OREC_MAGIC 0x324f424du   /* "MBO2": the ref-B-carrying format */
struct mbt_orec { uint64_t key; double mean; uint8_t panchor; uint8_t bvalid; };
static struct { char mode; FILE *fp; pthread_mutex_t mx;
                struct mbt_orec *idx; int8_t *offs; int8_t *boffs; int n, nmb; }
    g_mbt_oracle = { 0, NULL, PTHREAD_MUTEX_INITIALIZER, NULL, NULL, NULL, 0, 0 };
static _Atomic int g_mbt_oracle_init_done;

static uint64_t mbt_oracle_key(const yah264_encoder_t *e)
{
    const uint8_t *p = (const uint8_t *)e->lowres_prev;   /* the anchor's lowres */
    size_t nb = (size_t)e->lr_w * e->lr_h * sizeof(pixel);
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < nb; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h ^ ((uint64_t)(uint32_t)e->poc * 0x9E3779B97F4A7C15ull);
}

static void mbt_oracle_init(int nmb)
{
    if (atomic_load_explicit(&g_mbt_oracle_init_done, memory_order_acquire))
        return;
    pthread_mutex_lock(&g_mbt_oracle.mx);
    if (atomic_load_explicit(&g_mbt_oracle_init_done, memory_order_relaxed)) {
        pthread_mutex_unlock(&g_mbt_oracle.mx);
        return;
    }
    const char *rec = getenv("Y264_MBT_REC"), *play = getenv("Y264_MBT_PLAY");
    g_mbt_oracle.nmb = nmb;
    if (play) {
        FILE *fp = fopen(play, "rb");
        if (fp) {
            int cap = 4096;
            uint32_t magic = 0;
            g_mbt_oracle.idx   = malloc((size_t)cap * sizeof *g_mbt_oracle.idx);
            g_mbt_oracle.offs  = malloc((size_t)cap * nmb);
            g_mbt_oracle.boffs = malloc((size_t)cap * nmb);
            if (fread(&magic, sizeof magic, 1, fp) != 1 || magic != MBT_OREC_MAGIC) {
                fprintf(stderr, "[mbt-oracle] %s is not a MBO2 recording -- "
                        "re-record it; the replay is INERT\n", play);
                cap = 0;                       /* falls through to mode 0 */
            }
            while (cap && g_mbt_oracle.idx && g_mbt_oracle.offs && g_mbt_oracle.boffs) {
                uint64_t key; int32_t rnmb; double mean; uint8_t pan, bval;
                if (fread(&key, sizeof key, 1, fp) != 1 ||
                    fread(&rnmb, sizeof rnmb, 1, fp) != 1 || rnmb != nmb ||
                    fread(&mean, sizeof mean, 1, fp) != 1 ||
                    fread(&pan, 1, 1, fp) != 1 ||
                    fread(&bval, 1, 1, fp) != 1 ||
                    fread(g_mbt_oracle.offs + (size_t)g_mbt_oracle.n * nmb,
                          1, (size_t)nmb, fp) != (size_t)nmb ||
                    fread(g_mbt_oracle.boffs + (size_t)g_mbt_oracle.n * nmb,
                          1, (size_t)nmb, fp) != (size_t)nmb)
                    break;
                g_mbt_oracle.idx[g_mbt_oracle.n] =
                    (struct mbt_orec){ key, mean, pan, bval };
                if (++g_mbt_oracle.n == cap) break;   /* probe cap: plenty */
            }
            fclose(fp);
            if (g_mbt_oracle.n > 0) {
                g_mbt_oracle.mode = 'p';
                fprintf(stderr, "[mbt-oracle] replaying %d anchor field(s)\n",
                        g_mbt_oracle.n);
            }
        }
    } else if (rec) {
        g_mbt_oracle.fp = fopen(rec, "wb");
        if (g_mbt_oracle.fp) {
            uint32_t magic = MBT_OREC_MAGIC;
            fwrite(&magic, sizeof magic, 1, g_mbt_oracle.fp);
            g_mbt_oracle.mode = 'r';
        }
    }
    atomic_store_explicit(&g_mbt_oracle_init_done, 1, memory_order_release);
    pthread_mutex_unlock(&g_mbt_oracle.mx);
}

/* Hash + look up the anchor about to be resolved. Called BEFORE the chain
 * wait so a hit can skip it; the found index is parked on the encoder (per
 * GOP worker -- a file-scope slot would race across workers) for the
 * mbt_resolve that immediately follows. Returns 1 = hit. */
static int mbt_oracle_prepare(yah264_encoder_t *e)
{
    int nmb = e->width_in_mbs * e->height_in_mbs;
    mbt_oracle_init(nmb);
    e->mbt_oracle_idx = -1;
    if (g_mbt_oracle.mode != 'p' || g_mbt_oracle.nmb != nmb)
        return 0;
    uint64_t key = mbt_oracle_key(e);
    for (int i = 0; i < g_mbt_oracle.n; i++)
        if (g_mbt_oracle.idx[i].key == key) {
            e->mbt_oracle_idx = i;
            return 1;
        }
    return 0;                   /* miss: caller waits + computes, untouched */
}

static int mbt_oracle_play(yah264_encoder_t *e)
{
    int i = e->mbt_oracle_idx, nmb = g_mbt_oracle.nmb;
    if (g_mbt_oracle.mode != 'p' || i < 0)
        return 0;
    e->mbt_oracle_idx = -1;
    memcpy(e->mbtree_off, g_mbt_oracle.offs + (size_t)i * nmb, (size_t)nmb);
    e->mbtree_mean_off = g_mbt_oracle.idx[i].mean;
    if (e->bmbtree_off[1]) {           /* the walk's second output */
        memcpy(e->bmbtree_off[1], g_mbt_oracle.boffs + (size_t)i * nmb, (size_t)nmb);
        e->bmbtree_valid[1] = g_mbt_oracle.idx[i].bvalid;
    }
    if (g_mbt_oracle.idx[i].panchor) {  /* the walk's side effect, replayed */
        memcpy(e->code_panchor_lr, e->lowres_prev,
               (size_t)e->lr_w * e->lr_h * sizeof(pixel));
        e->code_panchor_have = 1;
        e->code_panchor_poc = e->poc;
        e->code_panchor_push = 0;       /* oracle replay: gpq key unknown */
    }
    return 1;
}

static void mbt_oracle_record(yah264_encoder_t *e)
{
    int nmb = e->width_in_mbs * e->height_in_mbs;
    mbt_oracle_init(nmb);
    if (g_mbt_oracle.mode != 'r')
        return;
    uint64_t key = mbt_oracle_key(e);
    /* Did this resolve's compute take the x264 path (whose panchor update the
 * replay must reproduce)? Its tail sets exactly these two. */
    uint8_t pan = (e->code_panchor_have && e->code_panchor_poc == e->poc) ? 1 : 0;
    uint8_t bval = (uint8_t)(e->bmbtree_valid[1] && e->bmbtree_off[1]);
    pthread_mutex_lock(&g_mbt_oracle.mx);
    int32_t rnmb = nmb;
    fwrite(&key, sizeof key, 1, g_mbt_oracle.fp);
    fwrite(&rnmb, sizeof rnmb, 1, g_mbt_oracle.fp);
    fwrite(&e->mbtree_mean_off, sizeof(double), 1, g_mbt_oracle.fp);
    fwrite(&pan, 1, 1, g_mbt_oracle.fp);
    fwrite(&bval, 1, 1, g_mbt_oracle.fp);
    fwrite(e->mbtree_off, 1, (size_t)nmb, g_mbt_oracle.fp);
    if (bval)
        fwrite(e->bmbtree_off[1], 1, (size_t)nmb, g_mbt_oracle.fp);
    else {
        static int8_t zpad[64];
        for (int left = nmb; left > 0; left -= (int)sizeof zpad)
            fwrite(zpad, 1, left < (int)sizeof zpad ? (size_t)left : sizeof zpad,
                   g_mbt_oracle.fp);
    }
    pthread_mutex_unlock(&g_mbt_oracle.mx);
}

/* Take the anchor's mb-tree offsets: the prefetched ones if this is the anchor
 * they were computed for, else compute here exactly as before. */
/* Post a Phase-A warm for the window this anchor just walked. Called from the
 * anchor path AFTER mbt_resolve, so the driver's own chain wait (or the lead
 * prefetch's, joined by the resolve) is the happens-before edge that makes the
 * warm's reads of `typed` legal -- it scans exactly the range that wait
 * covered and no further. Joined at the next anchor's resolve. */
static void mbt_warm_launch(yah264_encoder_t *e)
{
    struct mbt_pre *mp = e->mbtp;
    if (!mp || !mp->warm || !e->mbtree_on || e->mbtree_skip || e->la_depth <= 1)
        return;
    /* Under WIDE-capable shapes the warm's step<->offset wait arithmetic
     * keeps losing to the pipeline: TSan catches it reading an entry the chain
     * types a beat later, and widening the bound twice does not fix it. The
     * warm is pure memo prefill -- disabling it can never change bits (a missed
     * key is a fresh compute) and it is worth close to nothing -- so where
     * width can engage, it stands down instead of racing. */
    if (stair_wide_engaged_cfg(e))
        return;
    pthread_mutex_lock(&mp->mx);
    while (mp->req && !mp->done)                 /* previous warm still running */
        pthread_cond_wait(&mp->cv_done, &mp->mx);
    mp->req = 0; mp->done = 0;
    mp->kind = 1;
    mp->warm_head   = e->la_head;
    mp->warm_navail = e->la_n;
    mp->warm_popseq = e->la_th ? e->la_th->pop_seq : 0;
    mp->warm_pushed = e->la_th ? e->la_th->pushed : 0;
    mp->req = 1; mp->ok = 0;
    pthread_cond_signal(&mp->cv_req);
    pthread_mutex_unlock(&mp->mx);
}

/* Retire an in-flight warm before anything touches the memos it writes. */
static void mbt_warm_join(yah264_encoder_t *e)
{
    struct mbt_pre *mp = e->mbtp;
    if (!mp || !mp->warm)
        return;
    pthread_mutex_lock(&mp->mx);
    while (mp->req && !mp->done)
        pthread_cond_wait(&mp->cv_done, &mp->mx);
    if (mp->req && mp->kind) { mp->req = 0; mp->done = 0; mp->hits++; }
    pthread_mutex_unlock(&mp->mx);
}

/* Is a LEAD prefetch in flight for this anchor? Then the chain wait belongs to
 * the prefetch thread, not the driver, and the driver must not do it here --
 * doing it anyway would hand back the half of the prize that consists of not
 * blocking on the chain. On a miss mbt_resolve takes the wait itself, so the
 * honest path is never skipped, only relocated. */
static int mbt_pre_inflight(const yah264_encoder_t *e)
{
    return e->mbtp && e->mbtp->lead && e->mbtp->req;
}

/* Y264_UNSAFE_NO_MBT=1: the driver's mb-tree consumer never runs. The anchor
 * keeps whatever offsets are in e->mbtree_off (zeros on the first anchor, the
 * previous anchor's after that), so the QPs are wrong and the OUTPUT IS NOT THE
 * ENCODER'S -- same spirit as Y264_UNSAFE_NO_EMIT, but with one difference that
 * has to be said out loud: emission is a pure sink and mb-tree is not. Wrong
 * offsets move the QPs, which moves the bits, which moves the analyze and
 * entropy work downstream, so this arm's wall carries a confound the emission
 * probe did not have. It is the RAW ceiling only; bench/mbtree/probe.py resolves
 * the confound against the byte-identical Y264_MBT_PLAY replay, where the walk
 * is equally gone and the output is unchanged. Never read on a default path. */
static int mbt_unsafe_nombt(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_UNSAFE_NO_MBT"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

static void mbt_resolve(yah264_encoder_t *e, pixel *const *anchor_src)
{
    if (mbt_oracle_play(e))
        return;
    if (mbt_unsafe_nombt()) {
        /* the launch side stays: this arm prices the CONSUMER, not the warm */
        TPROF_MOVE(TP_MBTREE, TP_MBTWARM, mbt_warm_join(e));
        return;
    }
    struct mbt_pre *mp = e->mbtp;
    int owed_wait = mbt_pre_inflight(e);      /* the wait the driver did not do */
    TPROF_MOVE(TP_MBTREE, TP_MBTWARM, mbt_warm_join(e));   /* a WAIT, not compute */
    if (mp && !mp->warm) {
        int used = 0;
        double tl = tprof_ms();
        pthread_mutex_lock(&mp->mx);
        if (mp->req) {
            while (!mp->done)
                pthread_cond_wait(&mp->cv_done, &mp->mx);
            mp->ms_latch += tprof_ms() - tl;
            if (mp->ok && mp->anchor == e->cur_la_en && mp->anchor_poc == e->poc) {
                memcpy(e->mbtree_off, mp->out_off,
                       (size_t)e->width_in_mbs * e->height_in_mbs);
                e->mbtree_mean_off = mp->out_mean;
                used = 1;
                mp->hits++;
            } else {
                mp->misses++;
            }
            mp->req = 0; mp->done = 0;
        }
        pthread_mutex_unlock(&mp->mx);
        if (used) {
            mbt_oracle_record(e);
            return;
        }
    }
    if (owed_wait)                  /* prefetch missed: the untouched path, in full */
        la_th_wait_mbtree(e);
    struct mbt_req rq = {
        .anchor_planes = anchor_src, .anchor_lr = e->lowres_prev,
        .anchor_dintra = NULL,     .anchor_poc = e->poc,
        .head = e->la_head,        .navail = e->la_n,
        .out_off = e->mbtree_off,  .out_mean = &e->mbtree_mean_off,
    };
    compute_mbtree(e, &rq, 0);
    mbt_oracle_record(e);
}

/* Scene-cut decision from lowres cost sums (x264: the P cost is
 * the sum of min(intra, best-inter) per MB; a cut is declared when it is not
 * meaningfully below the pure-intra cost. The bias grows with the GOP so a cut
 * gets easier as the next keyframe is due. */
/* Y264_TYPE_ORACLE support (see la_finalize). Loads the type string once;
 * returns the next display-order char, or 0 when unset/exhausted. */
static char *type_oracle_buf = NULL;
static long  type_oracle_n = -2, type_oracle_cur = 0;

/* The load half, split out so warm_lr_statics can resolve the env gate on the
 * main thread WITHOUT consuming a cursor character (warming through
 * type_oracle_next would shift an armed replay by one frame). */
static void type_oracle_load(void)
{
    if (type_oracle_n != -2) return;
    type_oracle_n = -1;
    const char *p = getenv("Y264_TYPE_ORACLE");
    if (p) {
        FILE *fp = fopen(p, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
            type_oracle_buf = malloc(sz + 1);
            if (type_oracle_buf)
                type_oracle_n = (long)fread(type_oracle_buf, 1, sz, fp);
            fclose(fp);
        }
    }
}

static int type_oracle_next(void)
{
    type_oracle_load();
    if (!type_oracle_buf || type_oracle_cur >= type_oracle_n) return 0;
    int c = type_oracle_buf[type_oracle_cur++];
    while ((c == '\n' || c == '\r') && type_oracle_cur < type_oracle_n)
        c = type_oracle_buf[type_oracle_cur++];
    return (c == '\n' || c == '\r') ? 0 : c;
}

/* The scene-cut knobs, resolved from the param struct. scenecut_decide shared
 * the threshold arithmetic already, but each of its four callers re-derived the
 * minimum-GOP guard from its own `keyint / 10` literal -- including the pre-scan,
 * whose whole contract is predicting the encode exactly. Four copies of a
 * constant agree until one of them becomes a parameter. Everything the decision
 * depends on now resolves here, once, and every caller reads it from here.
 *
 * Pure in the param struct: no clock, no thread state, so the split still gets
 * the same answer as the encode on any machine at any thread count. */
struct sc_cfg {
    int    keyint;      /* param->keyint, floored at 1 */
    int    keyint_min;  /* resolved minimum GOP */
    int    off;         /* adaptive cut disabled: only keyint places IDRs */
    double thresh_max;  /* scenecut / 100 */
};

static struct sc_cfg sc_cfg_of(const yah264_param_t *p)
{
    struct sc_cfg s;
    s.keyint = p->keyint > 0 ? p->keyint : 1;

    /* keyint_min 0 = auto. x264's auto is min(keyint/10, fps) clamped to
 * [1, keyint/2+1] (its; yah264's auto is the keyint/10 half
 * without the fps cap, which is what this encoder has always done. The cap
 * only bites above a 10-second GOP -- but that includes 24 fps at the
 * default keyint 250, so adopting it is a real output change and wants its
 * own BD round, not a silent ride-along here. The clamp is x264's, applied
 * to explicit and auto alike; on auto it can never bind (keyint/10 is always
 * below keyint/2+1), so it costs today's path nothing. */
    int km = p->keyint_min > 0 ? p->keyint_min : s.keyint / 10;
    int hi = s.keyint / 2 + 1;
    s.keyint_min = km < 1 ? 1 : (km > hi ? hi : km);

    /* Negative = off, 0 = x264's default 40. Y264_NO_SCENECUT was built as a
 * diagnostic to price the cut barrier and does exactly what --no-scenecut
 * does, so it stays an alias of this one flag rather than a second
 * mechanism -- and, folded in here, it finally reaches the pre-scan too.
 * keyint <= 1 joins them: every frame is an IDR, so no cut can move one. */
    s.thresh_max = (p->scenecut > 0 ? p->scenecut : 40) / 100.0;
    s.off = p->scenecut < 0 || scenecut_off_env() || s.keyint <= 1;
    return s;
}

/* The decision proper, as a pure function of (cfg, sums, gop_size). Split out
 * so the cut pre-scan at the bottom of this file shares this exact arithmetic
 * instead of a copy that could drift from it. */
static int scenecut_decide(const struct sc_cfg *s, long icost, long pcost, int gop_size)
{
    if (icost <= 0)
        return 0;

    double thresh_max = s->thresh_max;
    double thresh_min = thresh_max * 0.25;
    if (s->keyint_min >= s->keyint) thresh_min = thresh_max;
    double bias;
    if (gop_size <= s->keyint_min / 4)
        bias = thresh_min / 4;
    else if (gop_size <= s->keyint_min)
        bias = thresh_min * gop_size / s->keyint_min;
    else
        bias = thresh_min + (thresh_max - thresh_min)
             * (gop_size - s->keyint_min) / (double)(s->keyint - s->keyint_min);
    return (double)pcost >= (1.0 - bias) * (double)icost;
}

static int scenecut_from_sums(yah264_encoder_t *e, long icost, long pcost, int gop_size)
{
    struct sc_cfg s = sc_cfg_of(&e->param);
    return scenecut_decide(&s, icost, pcost, gop_size);
}

/* Core-side scene-cut check from the cached lowres analysis (legacy path,
 * used when no lookahead window carries push-side flags). */
static int sc_early_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_SC_EARLY"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

static int detect_scenecut(yah264_encoder_t *e, int gop_size)
{
    if (!e->sc_have_prev)
        return 0;
    long icost = 0, pcost = 0;
    int n = e->width_in_mbs * e->height_in_mbs;
    for (int i = 0; i < n; i++) {
        icost += e->lr_intra[i];
        pcost += e->lr_intra[i] < e->lr_inter[i] ? e->lr_intra[i] : e->lr_inter[i];
    }
    return scenecut_from_sums(e, icost, pcost, gop_size);
}

static void copy_planes(const yah264_encoder_t *e, pixel *dst[3],
                        pixel *const src[3]);

/* Finalize the pending entry's frame type. Typing lags one push because
 * adaptive B placement needs the successor's costs; `nen` is NULL at flush
 * (no future frame: type as anchor). With b-adapt off this reproduces the
 * fixed cadence exactly (anchor every bframes+1). Anchors additionally run
 * the per-MB lowres ME against the previous typed anchor that feeds the
 * mb-tree chain. */

/* --- behaviour-matched anchor-pair lowres ME the lowres MB cost).
 *
 * The old anchor-leg ME (blk8_inter) started every 8x8 block from (0,0) with a
 * step-halving diamond: no predictor, no motion propagation, no subpel, no MV
 * cost. On divergent/accelerating motion (zoom) the true MV grows frame to
 * frame and varies across the frame, so the from-zero search lands in the
 * wrong basin -- the MV field was noise exactly where the full-res ME needs its
 * lowres seed (the hex-vs-UMH bus deficiency, localized by the oracle test).
 *
 * x264's lowres ME instead: scans in REVERSE raster order and predicts each
 * block from the median of the already-searched right / below / below-left /
 * below-right neighbours ("reverse-order MV prediction"), fast-skips on a
 * near-zero-residual zero predictor, runs a seeded hex search with an
 * MV-rate term (lookahead QP12 -> lambda 1), finishes with a radius-1 square
 * refine, and refines to subpel (lookahead subme 4: 1 hpel + 1 qpel diamond
 * iteration). Implemented here on yah264's lowres plane, plus one predictor
 * x264 does not have: the colocated MV from the PREVIOUS anchor pair's field
 * (motion propagation), which tracks zoom growth across pairs.
 *
 * Stages (Y264_LR_ME):
 * 0 = legacy from-zero diamond 1 = 0 + colocated temporal seed
 * 2 = x264 predictors + hex/square + mvcost (fpel) 3 = 2 + subpel refine
 * All MVs handled and stored in lowres QUARTER-pel (stages 0-2 store fpel
 * values *4); consumers convert once (full-res qpel = lowres qpel * 2).
 *
 * Default stage: 3 when hex is the active search and a full-res consumer will
 * read the field (y264_me_hex_features: --me hex or the auto medium+fast
 * tiers, or the Y264_NO_UMH / seed env overrides), else 0. The UMH path reads
 * nothing from this field, so it keeps the legacy compute (byte-identical AND
 * wall-clock-identical; the x264 field costs ~2.5% single-thread CIF and the
 * lookahead is serial under the wavefront). Y264_LR_ME overrides. */
static int lr_me_stage(void)
{
    static int env = -2;
    if (env == -2) { const char *s = getenv("Y264_LR_ME"); env = s ? atoi(s) : -1; }
    if (env >= 0) return env;
    return y264_me_hex_features() ? 3 : 0;
}

/* Resolve every env-gated lowres/mb-tree lazy static ONCE on the main thread.
 * The CLI opens (and closes) a prime encoder before spawning GOP workers, so
 * this runs first and single-threaded; afterwards the statics are read-only, so
 * the concurrent lowres paths (per-GOP-worker lookahead + the mb-tree Phase-A
 * parallel_for) never write them -> no data race. */
static int abr_cfloor_on(void);
static double abr_cfloor_frac(void);
static int abr_cguard_on(void);
static double abr_cguard_thresh(void);

static int abr_rf_env(void);
static int abr_rfqp_trace(void);
static int abr_rf2_env(void);
static int lr_shape_env(void);

static void warm_lr_statics(void)
{
    (void)gpq_consume_on();
    if (s_lr_intra_neighbour < 0) {
        const char *e = getenv("Y264_LR_INTRA_NEIGHBOUR");
        s_lr_intra_neighbour = e ? atoi(e) : 1;
    }
    (void)mbtree_mvlambda(); (void)mbtree_bfix();
    (void)lr_me_stage(); (void)lr_shape_env(); (void)adme_thresh(); (void)adme_log();
    (void)lr_prev_shape_on(); (void)lr_sadint_on();      /* pool-worker readers: warm here */
    (void)mbt_areuse_on();
    (void)lr_ipen();
    (void)psy_flat_gate(0); (void)psy_flat_log(); (void)psy_calm_gate(0);
    (void)lr_reuse_on(); (void)fpipe_on_env(); (void)stair_on_env();
    (void)wf_warmserial(); (void)wf_narrow_frame(352, 288);   /* warms its env */
    /* Read by Phase A on pool workers, so they must resolve here. The wide
     * config changes WHICH thread first-touches several of these; each one
     * missing from this list is a TSan report. */
    (void)bleg_reuse_on(); (void)pair_scale_on(); (void)mbt_legtrust_on();
    (void)direct_lrvote_on(); (void)direct_lrvote_cz();   /* read in Phase B */
    (void)mbt_unsafe_nosettle(); (void)satdx4_env(); (void)gpu_range();
    (void)lrsub_census(); (void)lrsub_double();
    (void)stair_depth_on(); (void)stair_stat_on(); (void)la_thread_env();
    /* Unwarmed, la_chain_step resolves la_inline_env from whichever GOP
     * worker gets there first: 24 TSan reports over 72 runs. Same-value init,
     * so not a determinism risk, but it lifts the TSan floor above zero and a
     * floor above zero hides the next real report. */
    (void)la_inline_env(); (void)mbt_pre_env(); (void)mbt_lead_env();
    (void)mbt_aqin();
    (void)mbt_coh(); (void)mbt_warm_env(); (void)mbt_split_env();
    (void)stair_bdepth_on(); (void)la_buf_env(); (void)la_pool_min();
    (void)stair_wide_on();
    (void)stair_multihop_on(); (void)stair_wide_ref_on();
    (void)stair_unsafe_no_refbwait(); (void)stair_refbgate_on();
    (void)stair_unsafe_no_rowgate(); (void)stair_unsafe_gate_arow();
    (void)stair_unsafe_no_prevpwait();
    (void)stair_refbearly_on(); (void)stair_leafrun_on();
    (void)stair_freelaunch_on();
    (void)stair_evictpool_on();
    (void)scenecut_off_env(); (void)stq_env();
    (void)rc_pipe_env(); (void)abr_qcomp_env(); (void)rcp_dbg_on();
    (void)abr_cfloor_on(); (void)abr_cfloor_frac(); (void)mbt_frac_on();
    (void)sc_early_on();
    (void)mbt_bref_probe(); (void)mbt_bcen();
    (void)rcp_warm_n(); (void)rcp_gain(); (void)rcp_lag_env(); (void)dauto_refonly_env(); (void)dauto_fold_env(); (void)hw_env(); (void)hw_scenecut_env(); (void)mbt_sub_partial_on(); (void)stair_refbgate_rc_on(); (void)rcp_qpd_env();
    (void)abr_rf_env(); (void)abr_rfqp_trace(); (void)abr_rf2_env(); (void)tdir_legal_on(); (void)abr_pbrate_env(NULL, NULL); (void)abr_vbvov_frac(); (void)abr_rf2_inflight_env();
    (void)rcp_lag_nowide_on(); (void)abr_early_env(); (void)rcp_nodrain_on(); (void)abr_rf2_basedom_on();
    (void)czw_stat_on();
    (void)direct_permb_env(); (void)y264_direct_auto_env(); (void)mbt_sub_retain_on();
    (void)mbt_settle_wait_on(); (void)mbt_sub_verify_on();
    (void)rcp_vbv_env(); (void)vbv_rhi_env(); (void)vbv_force_env();
    (void)vbv_stat_on(); (void)vbv_qpd_env(); (void)vbv_cjump_env();
    /* vbv_bound_env is read by encoder_open, and cli/yah264_cli.c opens one
     * encoder PER GOP from concurrent workers, so open is not a
     * single-threaded context. Primed here on the main thread before any
     * worker. */
    (void)vbv_bound_env();
    /* First: every default below that the x264 mode moves reads it. */
    (void)y264_mbt_derived();
    (void)mbt_ac_gain(); (void)aq_chroma_env();
    (void)crf_cplx_env(); (void)aq_anchor_env(); (void)crf_pbscale_env();
    (void)crf_aqabs_env(); (void)crf_fps_env(); (void)crf_pb0_env();
    (void)crf_ped_env();
    (void)unsafe_no_nal();
    /* 08-29 TSan sweep of the GOP-parallel repro: the three lazy env caches
 * it still flagged. All benign same-value init races, warmed here so the
 * NEXT TSan hunt is not reading four reports to find the real one. wf_capk
 * caches only its env; the dims are dummies. */
    (void)fqp_trace_on(); (void)ident_stat_env(); (void)wf_capk(64, 64);
    /* 08-29 sweep, second pass: the rest of this TU's env gates. Audited
 * mechanically (scripts/env_gate_audit.py) rather than by eye -- the tail
 * is long enough that reading for it is how four of them survived the
 * first pass. */
    (void)tprof_on(); (void)plane_pad();
    (void)hpel_probe_on(); (void)hpel_double_on();
    (void)mbt_unsafe_nombt(); (void)lrsub_probe();
    (void)abr_cguard_on(); (void)abr_cguard_thresh();
    y264_gpu_warm_statics();
    type_oracle_load();
    y264_mb_warm_statics();     /* + the analyze-wavefront env statics */
}

static int median3(int a, int b, int c)
{
    int mx = a > b ? a : b, mn = a < b ? a : b;
    return c > mx ? mx : (c < mn ? mn : c);
}

/* Legacy step-halving diamond, optionally seeded (stage 1): start from the
 * better of (0,0) and the colocated previous-pair MV instead of always zero. */
static long blk8_inter_seed(const pixel *sb, int ss, const pixel *ref, int rs,
                            int lw, int lh, int bx, int by, int sx, int sy,
                            int *outmvx, int *outmvy)
{
    int cx = 0, cy = 0;
    long best = blk8_satd(sb, ss, ref + by * rs + bx, rs);
    if (sx || sy) {
        int rx = bx + sx, ry = by + sy;
        if (rx >= 0 && ry >= 0 && rx <= lw - 8 && ry <= lh - 8) {
            long c = blk8_satd(sb, ss, ref + ry * rs + rx, rs);
            if (c < best) { best = c; cx = sx; cy = sy; }
        }
    }
    for (int step = 16; step >= 1; step >>= 1) {
        for (int iter = 0; iter < 8; iter++) {
            int cand[4][2] = { {cx + step, cy}, {cx - step, cy},
                               {cx, cy + step}, {cx, cy - step} };
            int moved = 0;
            for (int k = 0; k < 4; k++) {
                int rx = bx + cand[k][0], ry = by + cand[k][1];
                if (rx < 0 || ry < 0 || rx > lw - 8 || ry > lh - 8) continue;
                long c = blk8_satd(sb, ss, ref + ry * rs + rx, rs);
                if (c < best) { best = c; cx = cand[k][0]; cy = cand[k][1]; moved = 1; }
            }
            if (!moved || best == 0) break;
        }
    }
    *outmvx = cx; *outmvy = cy;
    return best;
}

/* One block of the behaviour-matched search (stages 2/3). All MVs lowres qpel;
 * predictor (predx,predy) prices the MV rate (lambda 1, x264 lookahead QP12);
 * cand[] are raw qpel candidates (spatial mvc + colocated + zero), each
 * fpel-rounded before probing, as any predictor-seeded search must be.
 * Returns the best TOTAL cost (satd + mvcost) and the pure SATD via *outsatd. */
/* LOWRES SEARCH SHAPE. 0 = the shipped coherent quarter-pel SATD search;
 * 1 = x264's shape: whole-pel SAD during the search, SATD only at the end.
 *
 * the local measurement records (08-14) measured our lookahead at
 * 13.7-13.8% of single-thread CPU on the 720p board clips against x264's
 * 1-2% -- roughly 11-12% of t1 wall in ONE subsystem, and it is more work
 * rather than the same work done slower (instructions retired confirmed it is
 * volume, not CPI). The volume is per-probe metric: every candidate here calls
 * blk8_satd_qp, which is a quarter-pel SATD, where x264 probes whole-pel SAD
 * and pays SATD once. `Y264_LOWRES_COH=0` was already measured and recovers at
 * most 1.7% -- it prices the flag, not the shape, so it does not close this.
 *
 * MEASURED 08-26 late, AND IT DOES NOT PAY. Instruction counts (time -l),
 * 120 frames, CRF 25, arm verified live (md5s differ, sizes +0.04-0.08%):
 * foreman -0.5% t1 / -0.9% t8, samsung -1.1% / -1.3%, bbb -0.9% / -0.8%, with
 * wall flat (+0.0% to -0.8%). About 1%, not the 11-12% the audit projected.
 *
 * The reason is the counterfactual the audit never ran. `Y264_LR_ME=0` takes
 * the whole stage-3 field away, and instructions go UP: samsung +4.8% t1 /
 * +3.8% t8, bbb +3.6% t1, pure-C samsung +5.3% t1. The lowres field already
 * pays for itself -- it seeds the full-res search, and a worse field costs
 * more downstream than the field costs to build. So this subsystem is not a
 * cost centre to be drained; cheapening the probe metric collects ~1% and
 * cheapening it harder risks paying for it twice in full-res ME.
 *
 * That was the legacy clips. On the low-rate HD cells that carry the board's
 * worst-clip leg the pair-leg search is 3.5G of 18.1G SATD pixels, and the
 * x264 shape takes it to 1.2G. DEFAULT ON (2026-09-02) as one change with
 * Y264_LR_PREV_SHAPE and Y264_LR_SADINT; the band and wall are recorded at
 * lr_prev_shape_on. Y264_LR_SHAPE=0 restores the SATD-probed search. */
static int lr_shape_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_LR_SHAPE"); v = s ? atoi(s) : 1; }
    return v;
}

/* Whole-pel SAD probe: the integer part of the qpel vector indexes the
 * reference plane directly, so no subpel plane is touched and no interpolation
 * phase is selected. This is the whole point -- it is the cheap probe. */
static long blk8_sad_fpel(const pixel *sb, int ss, const pixel *ref, int rs,
                          int bx, int by, int qmx, int qmy)
{
    int ix = qmx >> 2, iy = qmy >> 2;
    return y264_dsp.sad[Y264_PU_8x8](sb, ss,
                                     ref + (by + iy) * rs + bx + ix, rs);
}

static long lr_me_block(const pixel *sb, int ss, const pixel *ref, int rs,
                        pixel *const subpel[16], int lw, int lh, int bx, int by,
                        int predx, int predy, const int (*cand)[2], int ncand,
                        int stage, int *outmvx, int *outmvy, long *outsatd)
{
    int xmin = -4 * bx, xmax = 4 * (lw - 8 - bx);
    int ymin = -4 * by, ymax = 4 * (lh - 8 - by);
#define LRCL(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define LRFPEL(v) ((((v) + 2) >> 2) * 4)   /* *4 not <<2: v can be negative (UB) */
#define LRMVCOST(mx, my) (lowres_mvbits((mx) - predx) + lowres_mvbits((my) - predy))
    /* x264 shape: probe whole-pel SAD, pay SATD once at the end. The MV-rate
 * term is halved for the SAD stage because this tree's SATD is deliberately
 * 2x x264's domain (documented in src/dsp/, verified over 300k blocks) while
 * SAD is 1x, so an unscaled lambda would over-price motion by about two
 * against a SAD distortion and bias the search toward the predictor. The
 * factor is the domain ratio, not a tuned constant; the BD round is what
 * judges it. */
    int shape = lr_shape_env();
    long bsatd, best;
    int cx = LRCL(LRFPEL(predx), xmin, xmax), cy = LRCL(LRFPEL(predy), ymin, ymax);
#define LRPROBE(qx, qy) (shape ? blk8_sad_fpel(sb, ss, ref, rs, bx, by, (qx), (qy)) \
                               : blk8_satd_qp(sb, ss, ref, rs, subpel, bx, by, (qx), (qy)))
#define LRCOST(qx, qy) (shape ? (LRMVCOST((qx), (qy)) >> 1) : LRMVCOST((qx), (qy)))
    bsatd = LRPROBE(cx, cy);
    best = bsatd + LRCOST(cx, cy);
    /* Candidate dedup vs EVERY probed start, not just a duplicate of the
 * current best: an exact duplicate returns the identical
 * (s, c), which cannot pass the strict-< acceptance -- skipping it changes
 * neither the winner nor a tie-break. Spatial/colocated/zero candidates
 * frequently coincide after fpel rounding + clamping. */
    int pts[10][2] = { { cx, cy } }, npts = 1;   /* 1 + ncand <= 10 */
    for (int k = 0; k < ncand; k++) {
        int tx = LRCL(LRFPEL(cand[k][0]), xmin, xmax);
        int ty = LRCL(LRFPEL(cand[k][1]), ymin, ymax);
        int dup = 0;
        for (int q = 0; q < npts; q++)
            if (pts[q][0] == tx && pts[q][1] == ty) { dup = 1; break; }
        if (dup) continue;
        if (npts < 10) { pts[npts][0] = tx; pts[npts][1] = ty; npts++; }
        long s = LRPROBE(tx, ty);
        long c = s + LRCOST(tx, ty);
        if (c < best) { best = c; bsatd = s; cx = tx; cy = ty; }
    }
    /* x264's hex search: radius-2 (fpel) hexagon iterated to convergence
 * (bounded by me_range/2 = 8), then a radius-1 8-point square refine. */
    static const int hexp[6][2] = { {-8,0}, {-4,-8}, {4,-8}, {8,0}, {4,8}, {-4,8} };
    /* Direction-tracked dedup (the same hexagon-revisit shape, same proof as the fullres hex
 * in me.c): after a move along hexp[d] only vertices {d-1, d, d+1} are new;
 * the rest were evaluated on an earlier iteration, and evaluated costs are
 * >= best under strict-< acceptance -- exact re-probe removal only. */
    int hdir = -1;
    for (int iter = 0; iter < 8; iter++) {
        int nx = cx, ny = cy; long ns = bsatd, nb = best;
        int nd = hdir;
        for (int k = 0; k < 6; k++) {
            if (hdir >= 0) {
                int rel = (k - hdir + 6) % 6;
                if (rel != 0 && rel != 1 && rel != 5) continue;
            }
            int tx = cx + hexp[k][0], ty = cy + hexp[k][1];
            if (tx < xmin || tx > xmax || ty < ymin || ty > ymax) continue;
            long s = LRPROBE(tx, ty);
            long c = s + LRCOST(tx, ty);
            if (c < nb) { nb = c; ns = s; nx = tx; ny = ty; nd = k; }
        }
        if (nx == cx && ny == cy) break;
        cx = nx; cy = ny; best = nb; bsatd = ns; hdir = nd;
    }
    static const int sqp[8][2] = { {-4,-4}, {0,-4}, {4,-4}, {-4,0},
                                   {4,0}, {-4,4}, {0,4}, {4,4} };
    { int nx = cx, ny = cy; long ns = bsatd, nb = best;
      for (int k = 0; k < 8; k++) {
          int tx = cx + sqp[k][0], ty = cy + sqp[k][1];
          if (tx < xmin || tx > xmax || ty < ymin || ty > ymax) continue;
          long s = LRPROBE(tx, ty);
          long c = s + LRCOST(tx, ty);
          if (c < nb) { nb = c; ns = s; nx = tx; ny = ty; }
      }
      cx = nx; cy = ny; best = nb; bsatd = ns; }
    /* Back into the SATD domain before anything downstream reads this. The
 * whole-pel stage above left bsatd holding a SAD, and the returned value is
 * the block's INTER COST -- it feeds mb-tree's propagation fraction and the
 * B seeds, which are calibrated in SATD. Re-score once at the winner and
 * restore the full MV-rate term; the subpel refine below then runs in the
 * same domain it always did. This single call is x264's "satd only at the
 * end", and it is what makes the cheap search safe rather than merely fast. */
    if (shape) {
        bsatd = blk8_satd_qp(sb, ss, ref, rs, subpel, bx, by, cx, cy);
        best = bsatd + LRMVCOST(cx, cy);
    }
    if (stage >= 3) {
        /* Lookahead subme 4: one half-pel then one quarter-pel 4-point diamond
 * iteration (x264's subme-7 tier allows one of each). */
        for (int step = 2; step >= 1; step--) {
            int nx = cx, ny = cy; long ns = bsatd, nb = best;
            int dia[4][2] = { {step, 0}, {-step, 0}, {0, step}, {0, -step} };
            for (int k = 0; k < 4; k++) {
                int tx = cx + dia[k][0], ty = cy + dia[k][1];
                if (tx < xmin || tx > xmax || ty < ymin || ty > ymax) continue;
                long s = blk8_satd_qp(sb, ss, ref, rs, subpel, bx, by, tx, ty);
                long c = s + LRMVCOST(tx, ty);
                if (c < nb) { nb = c; ns = s; nx = tx; ny = ty; }
            }
            cx = nx; cy = ny; best = nb; bsatd = ns;
        }
    }
#undef LRCOST
#undef LRPROBE
#undef LRMVCOST
#undef LRFPEL
#undef LRCL
    *outmvx = cx; *outmvy = cy; *outsatd = bsatd;
    return best;
}

/* One behaviour-matched lowres MV field, cur vs ref (stages >= 2): reverse-order
 * scan, spatial median predictor, colocated temporal candidates from
 * colx/coly scaled by colsf256/256 (minus the full MV when colsub, i.e.
 * the reference derives the list-1 leg of a pair by subtracting the pair's
 * MV from the list-0 leg), zero-residual
 * fast skip, hex + square (+ subpel at stage 3, planes in `subpel`). When
 * price is set the stored d_inter adds yah264's calibrated mvlambda MV rate
 * vs the predictor and clamps to intra (propfrac semantics); otherwise it is
 * the pure best SATD. MVs stored in lowres qpel. */
struct lr_fme_ctx {
    yah264_encoder_t *e;
    const pixel *cur;
    const int32_t *d_intra;
    const pixel *ref;
    pixel *const *subpel;
    const int16_t *colx, *coly;
    int colsf256, colsub;
    double mvlambda;
    int price;
    y264_lr_blk *leg;
    int stage;
};
static void lr_fme_block(struct lr_fme_ctx *fc, int mx, int my);
/* Flipped-axis wavefront cell: (r, c) -> (my, mx) = (hmb-1-r, wmb-1-c). */
#define LR_FME_CHUNK 8
static void lr_fme_cell(void *ctx, int tid, int r, int c)
{
    struct lr_fme_ctx *fc = ctx;
    (void)tid;
    NLED_SITE(Y264_LED_SITE_LRFME);
    int wmb = fc->e->width_in_mbs, my = fc->e->height_in_mbs - 1 - r;
    int c0 = c * LR_FME_CHUNK, c1 = c0 + LR_FME_CHUNK;
    if (c1 > wmb) c1 = wmb;
    for (int k = c0; k < c1; k++)               /* right-to-left within the chunk */
        lr_fme_block(fc, wmb - 1 - k, my);
}

/* Fill `fc` for one field-ME leg; when the pool path applies, also fill `sp`
 * (the wavefront spec) and return 1 so the caller can run several INDEPENDENT
 * legs as one concurrent batch -- a lone 45x10 chunk grid caps at ~ncols/2
 * parallel, so batching the legs is where the lookahead's parallelism
 * actually comes from. Returns 0 = run it serially (small frame / no pool). */
static int lowres_field_me_prep(yah264_encoder_t *e, const pixel *cur,
                                const int32_t *d_intra, const pixel *ref,
                                pixel *const subpel[16],
                                const int16_t *colx, const int16_t *coly,
                                int colsf256, int colsub,
                                double mvlambda, int price, y264_lr_blk *leg,
                                struct lr_fme_ctx *fc, ntp_wf_spec_t *sp)
{
    *fc = (struct lr_fme_ctx){ e, cur, d_intra, ref, subpel, colx, coly,
                               colsf256, colsub, mvlambda, price, leg,
                               lr_me_stage() };
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs;
    if (e->pool && ntp_pool_nthreads(e->pool) > 1 && wmb * hmb >= LA_FANOUT_MBS) {
        sp->nrows = hmb;
        sp->ncols = (wmb + LR_FME_CHUNK - 1) / LR_FME_CHUNK;
        sp->thread_init = NULL;
        sp->cell_fn = lr_fme_cell;
        sp->ctx = fc;
        return 1;
    }
    return 0;
}

/* The block loop runs in REVERSE raster order and each block's predictor is
 * the median of its right / below / below-left / below-right neighbours'
 * MVs, so this is a wavefront, not a parallel-for -- but a wavefront with
 * the axes flipped. Under (r, c) = (hmb-1-my, wmb-1-mx) those four
 * neighbours map to (r, c-1) and (r-1, c-1 .. c+1), every one of which
 * ntp_wavefront's "left done, row above done through c+1" already
 * guarantees. So the flipped grid is bit-identical to the reverse raster
 * walk. Small frames stay serial: below a few hundred blocks the launch
 * costs more than the walk. */
static void lowres_field_me(yah264_encoder_t *e, const pixel *cur,
                            const int32_t *d_intra, const pixel *ref,
                            pixel *const subpel[16],
                            const int16_t *colx, const int16_t *coly,
                            int colsf256, int colsub,
                            double mvlambda, int price, y264_lr_blk *leg)
{
    NLED_SITE(Y264_LED_SITE_LRFME);
    struct lr_fme_ctx fc;
    ntp_wf_spec_t sp;
    if (lowres_field_me_prep(e, cur, d_intra, ref, subpel, colx, coly,
                             colsf256, colsub, mvlambda, price, leg,
                             &fc, &sp)) {
        ntp_prof_tag("lookahead_fme"); ntp_prio_hint();
        ntp_wavefront_batch(e->pool, 1, &sp);
        return;
    }
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs;
    for (int my = hmb - 1; my >= 0; my--)
        for (int mx = wmb - 1; mx >= 0; mx--)
            lr_fme_block(&fc, mx, my);
}

static void lr_fme_block(struct lr_fme_ctx *fc, int mx, int my)
{
    yah264_encoder_t *e = fc->e;
    const pixel *cur = fc->cur, *ref = fc->ref;
    const int32_t *d_intra = fc->d_intra;
    pixel *const *subpel = fc->subpel;
    const int16_t *colx = fc->colx, *coly = fc->coly;
    int colsf256 = fc->colsf256, colsub = fc->colsub, price = fc->price;
    double mvlambda = fc->mvlambda;
    y264_lr_blk *leg = fc->leg;
    int stage = fc->stage;
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs;
    int lw = e->lr_w, lh = e->lr_h;
    {
        {
            int i = my * wmb + mx;
            const pixel *sb = cur + (my * 8) * lw + mx * 8;
            /* The reference's predictor assembly: zero seeds pre-loaded; collect
 * right, below, below-left, below-right (already searched);
 * median of the first three when more than one was found. */
            int mvc[4][2] = { {0,0}, {0,0}, {0,0}, {0,0} };
            int nmvc = 0;
            if (mx < wmb - 1) {
                mvc[nmvc][0] = leg[i + 1].mvx; mvc[nmvc][1] = leg[i + 1].mvy; nmvc++;
            }
            if (my < hmb - 1) {
                mvc[nmvc][0] = leg[i + wmb].mvx; mvc[nmvc][1] = leg[i + wmb].mvy; nmvc++;
                if (mx > 0) {
                    mvc[nmvc][0] = leg[i + wmb - 1].mvx;
                    mvc[nmvc][1] = leg[i + wmb - 1].mvy; nmvc++;
                }
                if (mx < wmb - 1) {
                    mvc[nmvc][0] = leg[i + wmb + 1].mvx;
                    mvc[nmvc][1] = leg[i + wmb + 1].mvy; nmvc++;
                }
            }
            int predx, predy;
            if (nmvc <= 1) { predx = mvc[0][0]; predy = mvc[0][1]; }
            else {
                predx = median3(mvc[0][0], mvc[1][0], mvc[2][0]);
                predy = median3(mvc[0][1], mvc[1][1], mvc[2][1]);
            }
            /* x264 fast skip: near-zero residual on a zero predictor. */
            if (!predx && !predy) {
                long z = blk8_satd(sb, lw, ref + (my * 8) * lw + mx * 8, lw);
                if (z < (64 << (Y264_BIT_DEPTH - 8))) {
                    long c = z;
                    if (price && c > d_intra[i]) c = d_intra[i];
                    leg[i] = (y264_lr_blk){ (int32_t)c, 0, 0, 0 };
                    return;                 /* one block per call now */
                }
            }
            int cand[8][2]; int nc = 0;
            for (int k = 0; k < nmvc; k++) {
                cand[nc][0] = mvc[k][0]; cand[nc][1] = mvc[k][1]; nc++;
            }
            if (colx) {                     /* colocated motion propagation */
                int cvx = (colx[i] * colsf256 + 128) >> 8;
                int cvy = (coly[i] * colsf256 + 128) >> 8;
                if (colsub) { cvx -= colx[i]; cvy -= coly[i]; }
                cand[nc][0] = cvx; cand[nc][1] = cvy; nc++;
            }
            cand[nc][0] = 0; cand[nc][1] = 0; nc++;
            int mvx, mvy; long satd;
            lr_me_block(sb, lw, ref, lw, subpel, lw, lh,
                        mx * 8, my * 8, predx, predy,
                        (const int (*)[2])cand, nc, stage,
                        &mvx, &mvy, &satd);
            long c = satd;
            if (price) {
                c += (long)lround(mvlambda * (lowres_mvbits((mvx - predx) >> 2) +
                                              lowres_mvbits((mvy - predy) >> 2)));
                if (c > d_intra[i]) c = d_intra[i];  /* propfrac >= 0 */
            }
            leg[i] = (y264_lr_blk){ (int32_t)c, 0, (int16_t)mvx, (int16_t)mvy };
        }
    }
}

/* Fill en->leg[LR_LEG_ANCHOR] (MVs in lowres qpel) for the anchor pair
 * en vs e->la_anchor_lr, then retain the field as the next pair's colocated
 * temporal predictor. The stored d_inter keeps yah264's calibrated
 * propagation pricing (mvlambda vs the block's predictor, clamped to intra)
 * so the legacy mb-tree escape and the seed-oracle cost keep their meaning. */
static void lowres_anchor_me(yah264_encoder_t *e, struct la_entry *en)
{
    NLED_SITE(Y264_LED_SITE_LOWRES);
    NLED(leg_anchor, 1);
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs, nmb = wmb * hmb;
    int lw = e->lr_w, lh = e->lr_h;
    int stage = lr_me_stage();
    double mvlambda = mbtree_mvlambda();
    int price = (e->bframes == 0 || mbtree_bfix());
    y264_lr_blk *leg = en->leg[LR_LEG_ANCHOR];
    const pixel *ref = e->la_anchor_lr;
    int thave = e->la_anchor_mv_have && e->la_anchor_mvx && e->la_anchor_mvy;

    if (stage >= 3) {
        if (g_lrs_census > 0) g_lrs_site[3]++;
        build_lr_subpel(e->lr_subpel[0], ref, lw, lh);
    }

    if (stage <= 1) {                       /* legacy diamond (+ colocated at 1) */
        for (int my = 0; my < hmb; my++) {
            int pmvx = 0, pmvy = 0;         /* raster left-neighbour predictor */
            for (int mx = 0; mx < wmb; mx++) {
                int i = my * wmb + mx, mvx, mvy;
                const pixel *sb = en->lowres + (my * 8) * lw + mx * 8;
                int sx = 0, sy = 0;
                if (stage == 1 && thave) {
                    sx = (e->la_anchor_mvx[i] + 2) >> 2;
                    sy = (e->la_anchor_mvy[i] + 2) >> 2;
                }
                long c = blk8_inter_seed(sb, lw, ref, lw, lw, lh,
                                         mx * 8, my * 8, sx, sy, &mvx, &mvy);
                if (price) {
                    c += (long)lround(mvlambda * (lowres_mvbits(mvx - pmvx) +
                                                  lowres_mvbits(mvy - pmvy)));
                    if (c > en->d_intra[i]) c = en->d_intra[i];  /* propfrac >= 0 */
                }
                leg[i] = (y264_lr_blk){ (int32_t)c, 0,
                                        (int16_t)(mvx * 4), (int16_t)(mvy * 4) };
                pmvx = mvx; pmvy = mvy;
            }
        }
    } else {
        lowres_field_me(e, en->lowres, en->d_intra, ref, e->lr_subpel[0],
                        thave ? e->la_anchor_mvx : NULL,
                        thave ? e->la_anchor_mvy : NULL, 256, 0,
                        mvlambda, price, leg);
    }
    if (e->la_anchor_mvx && e->la_anchor_mvy) {
        for (int i = 0; i < nmb; i++) {
            e->la_anchor_mvx[i] = leg[i].mvx;
            e->la_anchor_mvy[i] = leg[i].mvy;
        }
        e->la_anchor_mv_have = 1;
    }
}

/* Per-B lowres pair fields (x264's lowres MVs): for each typed B
 * between the previous anchor (POC prev_poc, lowres still in e->la_anchor_lr)
 * and the just-typed anchor `en`, compute its list-0 pair field (B vs prev
 * anchor -> leg[LR_LEG_ANCHOR]) and list-1 pair field (B vs en ->
 * leg[LR_LEG_NEXT]). Colocated candidates come from the anchor pair's field
 * scaled by the B's temporal position (the same distance-scaling geometry:
 * dmv0 = mvr*d0/D, dmv1 = dmv0 - mvr). Runs after lowres_anchor_me, which
 * left that field in e->la_anchor_mvx/mvy and (stage 3) the prev anchor's
 * subpel planes in lr_subpel[0]. Stashed at pop as full-res B ME seeds. */
/* NLED_SITE(LOWRES) is set by both lowres ME entries; the analyze_* entries in
 * macroblock.c re-stamp the site, so no restore is needed here. */
static void lowres_bleg_me(yah264_encoder_t *e, struct la_entry *en, int nb,
                           int prev_poc)
{
    NLED_SITE(Y264_LED_SITE_LOWRES);
    int stage = lr_me_stage();
    if (nb <= 0 || stage < 2 || !e->la_anchor_mv_have) return;
    int lw = e->lr_w, lh = e->lr_h;
    int eidx = (int)(en - e->la);
    int en_poc = en->since_val * 2;
    int D = (en_poc - prev_poc) / 2;        /* frames between the anchors */
    if (D <= 1) return;                     /* no B in between */
    if (stage >= 3) {
        if (g_lrs_census > 0) g_lrs_site[4]++;
        build_lr_subpel(e->lr_subpel[1], en->lowres, lw, lh);
    }
    /* Every leg here is INDEPENDENT: disjoint output fields (bn->leg[...]),
 * shared inputs read-only (the two anchor lowres planes + subpel + the
 * anchor MV field). So the 2*nb small wavefronts run as ONE concurrent
 * batch -- a lone 45x10 chunk grid caps at ~5-way, the batch restores the
 * pool-wide parallelism the t18 wait budget showed this stage lacked. */
    struct lr_fme_ctx fcs[16];
    ntp_wf_spec_t sps[16];
    int ns = 0;
    /* bleg_have is published only after the batch that fills leg[] has joined.
 * Setting it inside the loop, i.e. BEFORE ntp_wavefront_batch computes the
 * fields it advertises, lets a reader on another thread see
 * bleg_have == 1 over legs still being written -- and, worse, see
 * the flag flip mid-walk, making the output depend on
 * the schedule (3-5 distinct md5s in 12 runs at foreman --ref 1 t18). The
 * flag means "these legs are complete", so it has to be set where that is
 * true. */
    struct la_entry *pend[16]; int npend = 0;
    for (int j = 1; j <= nb; j++) {
        int bi = (eidx - j + 2 * e->la_cap) % e->la_cap;
        struct la_entry *bn = &e->la[bi];
        if (!bn->typed || bn->is_anchor || bn->is_idr) break;
        int b_poc = bn->since_val * 2;
        int d0 = (b_poc - prev_poc) / 2, d1 = (en_poc - b_poc) / 2;
        if (d0 <= 0 || d1 <= 0 || d0 + d1 != D) break;   /* stale-slot guard */
        int sf0 = (d0 * 256 + D / 2) / D;
        if (ns + 2 > 16) {                       /* flush a full batch (bf > 7) */
            ntp_prof_tag("lookahead_fme"); ntp_prio_hint();
            ntp_wavefront_batch(e->pool, ns, sps);
            ns = 0;
        }
        NLED(leg_anchor, 1); NLED(leg_next, 1);
        if (!lowres_field_me_prep(e, bn->lowres, bn->d_intra, e->la_anchor_lr,
                                  e->lr_subpel[0], e->la_anchor_mvx,
                                  e->la_anchor_mvy, sf0, 0, 0.0, 0,
                                  bn->leg[LR_LEG_ANCHOR], &fcs[ns], &sps[ns]))
            lowres_field_me(e, bn->lowres, bn->d_intra, e->la_anchor_lr,
                            e->lr_subpel[0], e->la_anchor_mvx, e->la_anchor_mvy,
                            sf0, 0, 0.0, 0, bn->leg[LR_LEG_ANCHOR]);
        else
            ns++;
        if (!lowres_field_me_prep(e, bn->lowres, bn->d_intra, en->lowres,
                                  e->lr_subpel[1], e->la_anchor_mvx,
                                  e->la_anchor_mvy, sf0, 1, 0.0, 0,
                                  bn->leg[LR_LEG_NEXT], &fcs[ns], &sps[ns]))
            lowres_field_me(e, bn->lowres, bn->d_intra, en->lowres,
                            e->lr_subpel[1], e->la_anchor_mvx, e->la_anchor_mvy,
                            sf0, 1, 0.0, 0, bn->leg[LR_LEG_NEXT]);
        else
            ns++;
        bn->bleg_poc0 = prev_poc; bn->bleg_poc1 = en_poc;
        if (npend < 16) pend[npend++] = bn;
    }
    if (ns) {
        ntp_prof_tag("lookahead_fme"); ntp_prio_hint();
        ntp_wavefront_batch(e->pool, ns, sps);
    }
    for (int j = 0; j < npend; j++)
        pend[j]->bleg_have = 1;
}

static void la_finalize(yah264_encoder_t *e, struct la_entry *en,
                        const struct la_entry *nen)
{
    if (en->typed)
        return;
    size_t lrsz = (size_t)e->lr_w * e->lr_h;
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs;
    int period = e->bframes + 1;
    struct sc_cfg sc = sc_cfg_of(&e->param);
    int keyint = sc.keyint;

    /* Deferred scene-cut decision (moved here from la_push so the successor is
 * visible). The raw ratio+bias test runs on the sums captured at push; a raw
 * cut is then suppressed as a flash when the *next* frame predicts well from
 * the pre-cut frame -- x264's scenecut flash guard at b-adapt-fast depth
 * (one frame past the candidate). With no cut this is identical to a
 * push-time decision, so no-flash content stays byte-for-byte unchanged. */
    int raw_cut = 0;
    /* x264 gates only the IDR PROMOTION on keyint_min, not the cut itself: a
 * detected cut closer than keyint_min still becomes a plain I frame (the
 * keyframe distance decides I-vs-IDR). With no non-IDR I at all, gating on
 * keyint_min suppresses the cut ENTIRELY, which also makes scenecut_decide's
 * own `gop_size <= keyint_min/4` bias ramp unreachable dead code.
 *
 * sintel's cut lands at ~frame 20 inside keyint_min 25: suppressed, that
 * codes a rigid I B B B P... cadence straight through it and spends 76% of
 * the clip's bits on the 8 frames after it, where x264 puts an I there and
 * spends 24%. */
    int sc_min = sc_early_on() ? 1 : sc.keyint_min;
    if (!sc.off && e->la_have_prev_fin && !en->sc_cleared &&
        e->la_since_idr >= sc_min)
        raw_cut = scenecut_decide(&sc, en->sum_icost, en->sum_cost[LR_LEG_PREV], e->la_since_idr);
    if (raw_cut && e->bframes > 0 && nen && e->la_fin_prev_lr) {
        /* Flash test: does the successor (nen) predict well from the pre-cut
 * frame (la_fin_prev_lr)? If so, the candidate is a brief flash, not a
 * real scene change -- clear the cut and the successor's candidacy. */
        long ic1 = 0, pc1 = 0;
        for (int my = 0; my < hmb; my++)
            for (int mx = 0; mx < wmb; mx++) {
                int i = my * wmb + mx, mvx, mvy;
                const pixel *sb = nen->lowres + (my * 8) * e->lr_w + mx * 8;
                long ii = nen->d_intra[i];
                long pp = blk8_inter(sb, e->lr_w, e->la_fin_prev_lr, e->lr_w,
                                     e->lr_w, e->lr_h, mx * 8, my * 8, &mvx, &mvy);
                ic1 += ii;
                pc1 += ii < pp ? ii : pp;
            }
        if (!scenecut_decide(&sc, ic1, pc1, e->la_since_idr + 1)) {
            raw_cut = 0;
            ((struct la_entry *)nen)->sc_cleared = 1;
        }
    }
    /* Y264_TYPE_ORACLE=<path>: MEASUREMENT ONLY. One char per frame in display
 * order (I=IDR, P/p=anchor, B/b=leaf); replays another encoder's frame-type
 * sequence (e.g. x264's, parsed from its stats file) through this one, so
 * placement policy can be priced separately from everything downstream of
 * it. Single-encoder-per-process (static cursor), default off. */
    int type_oc = type_oracle_next();
    if (type_oc == 'I') { raw_cut = 1; }
    else if (type_oc)   { raw_cut = 0; }
    if (e->nzones) {                                /* a zone's forced IDR is a cut */
        const yah264_zone_t *z = zone_at(e, (int)(en->push_idx - 1));
        if (z && (z->flags & YAH264_ZONE_IDR) && (int)(en->push_idx - 1) == z->first)
            raw_cut = 1;
    }
    en->is_cut = raw_cut;
    if (raw_cut)
        e->la_since_idr = 0;
    int since = e->la_since_idr;
    e->la_since_idr = (e->la_since_idr + 1) % keyint;
    en->is_idr = (since == 0);
    en->since_val = since;
    e->la_fin_prev_lr = en->lowres;
    e->la_have_prev_fin = 1;

    int anchor;
    if (en->is_idr) {
        anchor = 1;
    } else if (!e->badapt_on) {
        anchor = (en->since_val % period) == 0;
    } else if (e->la_brun >= e->bframes || !nen) {
        anchor = 1;
    } else {
        /* Demote the candidate B to an anchor when even bidirectional
 * prediction is nearly as expensive as intra. Deliberately
 * conservative: three cost-driven formulations (pairwise, x264
 * path-cost geometry at integer-pel, the same with half-pel refined
 * costs) all over-demoted and measured +8-12% BD. The bias is the
 * zero-start lowres diamond overestimating exactly the longest-
 * distance term, which always sits on the continue-run path; fixing
 * it needs x264's seeded per-frame-pair lowres MV caches (a full
 * lookahead-ME subsystem). */
        long cb = 0, ci = 0;
        for (int my = 0; my < hmb; my++)
            for (int mx = 0; mx < wmb; mx++) {
                int i = my * wmb + mx;
                const pixel *sb = en->lowres + (my * 8) * e->lr_w + mx * 8;
                int mvx, mvy;
                long bwd = blk8_inter(sb, e->lr_w, nen->lowres, e->lr_w,
                                      e->lr_w, e->lr_h, mx * 8, my * 8, &mvx, &mvy);
                int32_t pv = en->leg[LR_LEG_PREV][i].d_inter;
                long m = pv < bwd ? pv : bwd;
                if (en->d_intra[i] < m) m = en->d_intra[i];
                cb += m;
                ci += en->d_intra[i];
            }
        anchor = (double)cb >= 0.90 * (double)ci;
    }
    if (type_oc && !en->is_idr) {
        /* Respect the bframes buffer cap: an oracle 'B' past the run limit is
 * coerced to anchor (counted, not fatal -- placements can differ by a
 * frame around cuts). */
        if (type_oc == 'B' || type_oc == 'b')
            anchor = (e->la_brun >= e->bframes || !nen) ? 1 : 0;
        else
            anchor = 1;
    }
    en->is_anchor = anchor;
    en->typed = 1;
    if (!anchor) {
        e->la_brun++;
        return;
    }
    int nb_run = e->la_brun;                /* B's since the previous anchor */
    e->la_brun = 0;
    /* Anchor analysis vs the previous anchor. An IDR starts a new chain (its
 * link back is never used: la_chain_prop stops at IDRs), so skip the ME
 * and drop the retained temporal MV field (motion propagation restarts). */
    en->aleg_have = 0;
    if (en->is_idr || !e->la_anchor_have) {
        for (int i = 0; i < wmb * hmb; i++)
            en->leg[LR_LEG_ANCHOR][i] = (y264_lr_blk){ en->d_intra[i], 0, 0, 0 };
        e->la_anchor_mv_have = 0;
    } else {
        TPROF_LA(e, TP_LOOKME, 1, {
            lowres_anchor_me(e, en);
            lowres_bleg_me(e, en, nb_run, e->la_anchor_poc);
        });
        en->aleg_poc0 = e->la_anchor_poc;    /* the walk's past anchor for this source */
        en->aleg_have = 1;
    }
    memcpy(e->la_anchor_lr, en->lowres, lrsz * sizeof(pixel));
    e->la_anchor_have = 1;
    e->la_anchor_poc = en->since_val * 2;
}

/* One macroblock row of la_push's lowres pass (see the call site). */
struct la_lr_ctx { yah264_encoder_t *e; struct la_entry *en; int wmb;
                   long *ic, *pc; };
static void la_lr_row(struct la_lr_ctx *c, int my, long *ic_out, long *pc_out)
{
    NLED_SITE(Y264_LED_SITE_LOWRES);
    yah264_encoder_t *e = c->e;
    struct la_entry *en = c->en;
    long ic = 0, pc = 0;
    for (int mx = 0; mx < c->wmb; mx++) {
        int i = my * c->wmb + mx;
        const pixel *sb = en->lowres + (my * 8) * e->lr_w + mx * 8;
        int mvx, mvy;
        long ii = blk8_intra_dispatch(sb, e->lr_w, mx, my);
        long pp = e->la_have_prev
                ? blk8_inter(sb, e->lr_w, e->la_lr_prev, e->lr_w,
                             e->lr_w, e->lr_h, mx * 8, my * 8, &mvx, &mvy)
                : ii;
        en->d_intra[i] = (int32_t)ii;
        en->leg[LR_LEG_PREV][i] = (y264_lr_blk){
            (int32_t)pp, 0,                              /* r_inter=0 until R1 */
            e->la_have_prev ? (int16_t)mvx : LR_MV_INVALID,
            e->la_have_prev ? (int16_t)mvy : 0 };
        ic += ii;
        pc += ii < pp ? ii : pp;
    }
    *ic_out = ic; *pc_out = pc;
}
static void la_lr_row_task(void *ctx, int tid, int my)
{
    struct la_lr_ctx *c = ctx;
    (void)tid;
    la_lr_row(c, my, &c->ic[my], &c->pc[my]);
}

/* One step of the lookahead chain for a just-claimed entry: downscale, the
 * per-MB lowres analysis (own intra + inter vs the previous frame), and the
 * previous entry's deferred finalize. Runs on the API thread, or -- engaged
 * (Y264_LA_THREAD) -- on the dedicated la thread; see the purity note at
 * struct la_thread. The frame's B/anchor type lags one push (la_finalize). */
static void la_chain_step(yah264_encoder_t *e, struct la_entry *en)
{
    size_t lrsz = (size_t)e->lr_w * e->lr_h;
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs;
    /* en->plane was padded in place by the caller (pad_input_to) -- no copy. */
    long ic = 0, pc = 0;
    TPROF_LA(e, TP_LOWRES, 0, {
    downscale(en->lowres, e->lr_w, e->lr_h, en->plane[0], e->pstride[0]);

    /* Per-MB independent (disjoint writes, read-only source), so it runs a row
 * per task. The two cost sums are accumulated per ROW and reduced in row
 * order after the join: integer addition regroups exactly, so the totals --
 * and every scene-cut decision downstream of them -- are unchanged. */
    struct la_lr_ctx lc = { e, en, wmb, NULL, NULL };
    long *psum = NULL;
    if (!e->la_inline &&
        e->pool && ntp_pool_nthreads(e->pool) > 1 && hmb > 1)
        psum = ntp_pool_slot(e->pool, 5, (size_t)hmb * 2 * sizeof(long));
    if (psum) {
        lc.ic = psum; lc.pc = psum + hmb;
        ntp_prof_tag("lookahead_lr"); ntp_prio_hint();
        ntp_parallel_for(e->pool, hmb, la_lr_row_task, &lc);
        for (int my = 0; my < hmb; my++) { ic += lc.ic[my]; pc += lc.pc[my]; }
    } else {
        long a, b;
        for (int my = 0; my < hmb; my++) {
            la_lr_row(&lc, my, &a, &b);
            ic += a; pc += b;
        }
    }
    });

    /* The scene-cut decision is deferred to la_finalize (one push later), so the
 * successor frame is visible and a one-frame flash can be suppressed the way
 * x264 --preset medium does (b-adapt fast looks exactly one frame past a cut
 * candidate). Only the raw frame-vs-prev sums are captured here. */
    en->sum_icost = ic;
    en->sum_cost[LR_LEG_PREV] = pc;
    en->sc_cleared = 0;
    en->is_cut = 0;
    en->is_idr = 0;
    en->since_val = 0;
    en->is_anchor = 0;                   /* provisional until typed */
    en->typed = 0;
    en->bleg_have = 0;                   /* B pair legs not filled yet */

    memcpy(e->la_lr_prev, en->lowres, lrsz * sizeof(pixel));
    e->la_have_prev = 1;

    /* gpq: submit this push's SEARCHQ legs against the previous reach pushes
 * (all resident by chain seriality). Fire-and-forget -- the walk collects
 * the fields frames later, which is what hides the round cost. */
    if (e->gpq)
        y264_gpq_push(e->gpq, en->push_idx, (int)(en - e->la), en->lowres);

    /* The previous pending entry now has its successor: finalize its type.
 * la_prev_pushed IS the (la_head + la_n - 2) lookup -- the entry
 * pushed immediately before this one -- tracked as a pointer so the
 * chain never reads the API-owned la_head/la_n ring counters. */
    if (e->la_prev_pushed)
        la_finalize(e, e->la_prev_pushed, en);
    e->la_prev_pushed = en;
}

/* --- la-thread plumbing (see the purity note at struct la_thread) --- */

static void la_th_main(void *arg)
{
    yah264_encoder_t *e = arg;
    struct la_thread *lt = e->la_th;
    s_la_chain_tls = 1;
    pthread_mutex_lock(&lt->mx);
    for (;;) {
        while (!lt->exit && lt->done == lt->pushed)
            pthread_cond_wait(&lt->cv_push, &lt->mx);
        if (lt->exit)
            break;                      /* close abandons unprocessed pushes */
        long s = lt->done + 1;
        struct la_entry *en = lt->q[s % e->la_cap];
        pthread_mutex_unlock(&lt->mx);
        la_chain_step(e, en);
        pthread_mutex_lock(&lt->mx);
        lt->done = s;
        atomic_store_explicit(&lt->done_atom, s, memory_order_release);
        pthread_cond_broadcast(&lt->cv_done);
    }
    pthread_mutex_unlock(&lt->mx);
    s_la_chain_tls = 0;
}

static void la_th_enqueue(yah264_encoder_t *e, struct la_entry *en)
{
    struct la_thread *lt = e->la_th;
    pthread_mutex_lock(&lt->mx);
    lt->q[(lt->pushed + 1) % e->la_cap] = en;
    lt->pushed++;
    pthread_cond_signal(&lt->cv_push);
    pthread_mutex_unlock(&lt->mx);
}

/* Block until the chain has completed step `need` (clamped to what has been
 * enqueued -- at flush the clamp turns any need into a full drain). The
 * acquire fast path makes the steady-state check one atomic load. */
static void la_th_wait(yah264_encoder_t *e, long need)
{
    struct la_thread *lt = e->la_th;
    if (!lt)
        return;
    if (need > lt->pushed) need = lt->pushed;   /* pushed: API-thread-owned */
    if (need <= 0 ||
        atomic_load_explicit(&lt->done_atom, memory_order_acquire) >= need)
        return;
    pthread_mutex_lock(&lt->mx);
    while (lt->done < need)
        pthread_cond_wait(&lt->cv_done, &lt->mx);
    pthread_mutex_unlock(&lt->mx);
}

/* la_th_wait for a caller that is NOT the API thread (the mb-tree prefetch).
 * It cannot clamp against `pushed` -- that field is API-owned, and a stale
 * (smaller) read would turn the clamp into an early return over untyped
 * entries. The launch checked `need <= pushed` instead, so the step is known
 * to be coming and this waits for it flatly. `exit` is the only other way out;
 * a caller that leaves on it must not use the entries (mbt_pre_main rechecks
 * done_atom for exactly that). */
static void la_th_wait_step(yah264_encoder_t *e, long need)
{
    struct la_thread *lt = e->la_th;
    if (!lt || need <= 0)
        return;
    if (atomic_load_explicit(&lt->done_atom, memory_order_acquire) >= need)
        return;
    pthread_mutex_lock(&lt->mx);
    while (lt->done < need && !lt->exit)
        pthread_cond_wait(&lt->cv_done, &lt->mx);
    pthread_mutex_unlock(&lt->mx);
}

static void la_th_wait_all(yah264_encoder_t *e)
{
    if (e->la_th)
        la_th_wait(e, e->la_th->pushed);
}

/* The exact wait the mb-tree window walk needs -- NOT the whole chain.
 *
 * Both walks (compute_mbtree_wholebuf and la_chain_prop) are capped at la_depth-2
 * entries past the popped anchor and break at the first `!typed`, so the last
 * entry either can read is k+la_depth-2 for the anchor k just popped. Step
 * numbering is 1-based (la_th_enqueue writes q[pushed+1] then bumps), so entry
 * j is ANALYSED by step j+1 and TYPED by la_finalize during step j+2. The
 * walk's last entry is therefore typed once step k+la_depth completes, and
 * with pop_seq == k+1 the requirement is done >= pop_seq + la_depth - 1.
 * Nothing newer is read, and the chain's concurrent steps only touch entries
 * at k+la_depth-1 and beyond -- disjoint from the walk's range -- so the walk
 * sees the identical entries in the identical state. Byte-identical, by the
 * same argument as the walk's own cap. At la_buf == 0 this is EXACTLY
 * la_th_wait_all (pushed == k+la_cap == k+la_depth), so the default path's
 * wait is unchanged to the step; the cap only ever relaxes BUF > 0.
 *
 * la_th_wait_all here was stricter by exactly la_buf steps (it waits for
 * `pushed`, which runs la_cap ahead of the pop). That is why Y264_LA_BUF made
 * the wall WORSE instead of neutral -- 202 ms at BUF=16: every
 * extra buffered entry is one more chain step the driver blocks on at every
 * anchor, so buffering buys negative lead. With the cap, BUF=k lets the
 * chain lag the driver by k steps, which is the lead x264's --sync-lookahead
 * exists to give. */
static void la_th_wait_mbtree(yah264_encoder_t *e)
{
    if (e->la_th)
        la_th_wait(e, e->la_th->pop_seq + e->la_depth - 1);
}

/* Push the just-padded input into the lookahead window: claim the ring slot
 * (API side -- the ring counters and the memo-invalidate stay consumer-
 * visible in program order) and run or enqueue the chain step. */
static void la_push(yah264_encoder_t *e)
{
    struct la_entry *en = &e->la[(e->la_head + e->la_n) % e->la_cap];
    e->la_n++;
    en->mbt_pa_valid = 0;          /* ring slot reused: its mb-tree memo is stale */
    en->push_idx = ++e->la_push_seq;   /* 1-based, matches the chain's step number */
    if (e->la_th_on)
        la_th_enqueue(e, en);
    else
        la_chain_step(e, en);
}

/* Emit one coded frame: build the slice and append its NAL into e->out at *off. */
/* Cheap per-frame complexity: summed 4x4 SATD of the source against a flat
 * per-block DC (intra frames, spatial texture) or against the list-0 reference at
 * zero motion (inter frames, temporal residual). A lookahead-lite proxy for how
 * many bits frame will need, used to set its QP before it is encoded. */
static double frame_complexity(yah264_encoder_t *e, pixel *const src[3], int intra)
{
    int ss = e->pstride[0], rs = e->pstride[0];
    const pixel *s = src[0], *r = e->ref[0];
    long total = 0;
    for (int y = 0; y + 4 <= e->height; y += 4)
        for (int x = 0; x + 4 <= e->width; x += 4) {
            const pixel *sb = s + y * ss + x;
            if (intra) {
                int sum = 0;
                for (int j = 0; j < 4; j++)
                    for (int i = 0; i < 4; i++) sum += sb[j * ss + i];
                pixel flat[16];
                int m = (sum + 8) >> 4;
                for (int k = 0; k < 16; k++) flat[k] = (pixel)m;
                total += y264_dsp.satd4x4(sb, ss, flat, 4);
            } else {
                total += y264_dsp.satd4x4(sb, ss, r + y * rs + x, rs);
            }
        }
    return (double)total + 1.0;
}

/* ME-compensated frame complexity for CRF: the sum over MBs of the lowres
 * min(intra, best-inter) cost that lowres_analyse just cached for this frame.
 * Unlike frame_complexity above (zero-motion full-res SATD, which conflates
 * motion with coding difficulty and reads foreman ~= ducks), this is what x264's
 * CRF uses -- it reflects post-motion-compensation cost, so hard and easy content
 * separate and the qcomp curve can actually spread QP the way x264 does. */
static double frame_complexity_me(const yah264_encoder_t *e)
{
    long total = 0;
    int n = e->width_in_mbs * e->height_in_mbs;
    for (int i = 0; i < n; i++) {
        int ic = e->lr_intra[i], pc = e->lr_inter[i];
        total += ic < pc ? ic : pc;
    }
    return (double)total + 1.0;
}

/* Lowres INTRA cost sum: pure source energy, no recon, never collapses. The
 * rcp VBV bits model runs on this -- min(intra, inter) reads ~0 on static or
 * noise-vs-motion content (samsung's sparkle anchors: cme ~tiny, actual 3
 * Mbit) and a blind model is exactly what VBV cannot afford. Level
 * differences per type (a B at the same energy costs far less than a P) are
 * absorbed by the per-type scale calibration. */
static double frame_complexity_ilr(const yah264_encoder_t *e)
{
    long total = 0;
    int n = e->width_in_mbs * e->height_in_mbs;
    for (int i = 0; i < n; i++)
        total += e->lr_intra[i];
    return (double)total + 1.0;
}

/* Complexity-driven ABR: predict the QP that hits (buffer-corrected) target
 * from the frame's complexity and a calibrated bits-vs-qscale scale, then set it
 * before encoding. Proactive, so scene changes are handled without a frame of lag. */
/* Per-type bits->qscale scale, falling back to any calibrated type (then 1.0)
 * until this frame type has been coded once. I/P/B have very different
 * bits-per-complexity, so a shared scale mispredicts (esp. costly I frames). */
/* The ABR/CRF decide complexity is frame_complexity_me, min(intra, inter)
 * summed, and it COLLAPSES on static or noise-vs-motion content. The comment on
 * frame_complexity_ilr has recorded this since the VBV round ("samsung's
 * sparkle anchors: cme ~tiny, actual 3 Mbit"), but the floored hybrid built
 * there (cvi = max(cme, intra/4)) was wired to the VBV bits model ONLY, so the
 * ABR decide kept reading the raw signal. Measured on samsung, adjacent
 * same-type frames read C=41801 against C=896066 -- a 21x step that is not
 * content, and since q scales as C^0.4 it hands those frames about 10 QP for
 * nothing, which is most of the ABR ladder's oscillation.
 *
 * Floor at a FRACTION of cvi rather than at cvi: intra/4 sits above healthy cme
 * on samsung, so flooring at cvi itself pins every frame to the intra energy and
 * discards the motion signal -- the failure the ilr comment records as "sintel
 * regressed on it". The collapses read 25k-72k where healthy frames read 680k+,
 * so the floor only has to land between them.
 *
 * Self-BD against the unfloored build on the band ladders: samsung -4.94%,
 * sintel -0.36%, and the other eight clips byte-identical. DEFAULT ON;
 * Y264_ABR_CFLOOR=0 escapes. */
static int abr_cfloor_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_ABR_CFLOOR"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

static double abr_cfloor_frac(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_ABR_CFLOORF"); v = s ? atof(s) : 0.2; }
    return v;
}

/* The full x264 ABR model: rate factor for P, P-track anchors for I and B.
 * All three go in together: the P-track anchors alone fail when layered on a
 * model with no rate factor under them. DEFAULT OFF until the corpus gates
 * it. */
static int abr_cguard_on(void)
{
    /* Default ON whenever the rf model is armed: the guard is part of that
 * model's rate-accuracy fix (sintel-900
 * +48.2% -> +12.3%), not an independent experiment. Explicit env wins. */
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("Y264_ABR_CGUARD");
        if (s) v = atoi(s) ? 1 : 0;
        else { const char *rf = getenv("Y264_ABR_RF"); v = rf && atoi(rf) ? 1 : 0; }
    }
    return v;
}

static double abr_cguard_thresh(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_ABR_CGUARD_T"); v = s ? atof(s) : 4.0; }
    return v;
}

/* Env override of the public param.rc.abr_model. -1 = unset (use the param). */
static int abr_rf_env(void)
{
    static int v = -2;
    if (v == -2) { const char *s = getenv("Y264_ABR_RF"); v = s ? (atoi(s) ? 1 : 0) : -1; }
    return v;
}
/* Y264_ABR_RFQP=1: plan step A1 (work item A), the per-frame ledger of BOTH
 * rate models from one encode. On every
 * rcp_decide the default model's equation (err, target, scale, rceq, the
 * pre-clamp and post-clamp QP) prints beside the rate-factor model's
 * (blurred rceq, rf_cplx_sum, rf_wanted_bits, rf, overflow, the raw,
 * lstep-clipped and anchored QP), and on every rcp_account the coded bits
 * and QP with the accumulators after the update. Whichever model is live
 * decides; the other is a SHADOW: its state (rf_cplx_sum, rf_wanted_bits,
 * the st_cplx EWMA, last_qscale_type, the P-track anchors) is fields only the
 * rf branch reads, so shadowing them on the default path leaves the output
 * byte-identical. rc_set_qp (Y264_RC_PIPE=0) is not traced. Default inert. */
static int abr_rfqp_trace(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_ABR_RFQP"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}
/* Y264_ABR_RF2=1: plan step A2, ABR as the CRF path plus a slowly adapted
 * rate factor (plan work item A). The same
 * design x264 documents for single-pass ABR under mb-tree, written
 * independently from that description (no x264 code): the frame
 * QP is qscale = rceq * rf_cplx_sum / rf_wanted_bits with rceq the
 * duration-only constant (0.04 / dur)^(1 - qcomp) (per-frame complexity is
 * NOT a term: mb-tree's per-MB offsets carry it), rf_cplx_sum seeded from
 * 0.01 * 7e5 * sqrt(mbs) (with the qcomp exponent at 1 under mb-tree, which
 * is what makes the seed a bitrate prior: samsung 448k opens at QP ~44 base)
 * and rf_wanted_bits from one frame of target bits; after each coded
 * frame rf_cplx_sum += bits * qscale(coded qp) / rceq, wanted += target_bpf
 * (no P/B ratio under mb-tree). The bounded correction is the documented one:
 * qscale *= clip(1 + err / (2 * tol * bitrate * max(1, sqrt(t))), 0.5, 2).
 * Mid-stream I frames take the decayed P-QP track (ptrack_qp); B frames
 * take the anchor's base and frame_qp's cascade, exactly as the CRF path.
 * No per-frame lstep clip (A3), no CFLOOR / CGUARD, no abr_scale[]. Needs
 * mb-tree on (bframes > 0 or the IPPPP mb-tree); otherwise it logs and
 * falls back to the previous model.
 * DEFAULT ON (2026-09-02, plan A5) on the 29-clip table
 * (local/records/a4-abr-table-2026-09-02.md): ABR vs x264's ABR at x264's
 * achieved rates, medians CIF -4.1% / 720p -8.2% / 1080p -1.4% where the
 * previous model read -0.5 / +14.7 / +8.8; ABR-vs-own-CRF tax 5.5 / 9.4 /
 * 6.9% against x264's 3.5 / 4.4 / 3.8 and the previous model's 10 / 33 / 18.
 * Behind the previous model only at the top of the rate range (riverbed,
 * crowd_run); the rate-keyed B cascade is the queued arm. Y264_ABR_RF2=0
 * restores the previous model byte for byte. */
/* Y264_TDIR_LEGAL (plan C2): slice-level temporal-direct legality, see the
 * slice-header comment. Reaches only slices that would code temporal
 * direct (--direct temporal, or auto choosing it); the spatial default is
 * untouched. Under the default B-pyramid it sends a structural 37 of 111
 * B slices per 150 frames to spatial on every clip.
 * DEFAULT ON (2026-09-03). Rate-anchored BD vs x264 medium, temporal arm,
 * five 1080p clips, 150f (spatial arm in brackets, unchanged): sunflower
 * +31.2 -> +18.5% (-7.7), pedestrian +7.9 -> +5.0 (-0.4), riverbed +4.9 ->
 * +4.0 (+2.4), station2 -32.1 -> -26.5 (-12.5), blue_sky -8.1 -> -3.0
 * (+16.5): the swing narrows at both ends, which is what a per-slice auto
 * rule needs before its score can be trusted. =0 restores the unguarded
 * temporal mode. */
static int tdir_legal_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_TDIR_LEGAL"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
static int abr_rf2_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_ABR_RF2"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

static double abr_tunable(const char *n, double def)
{
    const char *s = getenv(n);
    return s ? atof(s) : def;
}

/* Bounded, sqrt(t)-damped correction. Only a nudge: the rate factor does the
 * converging, so this term alone undershoots. err and wanted are bits. */
static double abr_overflow(const yah264_encoder_t *e, double err, double wanted)
{
    double bps = e->abr_target_bpf * (e->abr_fps > 0 ? e->abr_fps : 25.0);
    if (bps <= 0) return 1.0;
    double buf = 2.0 * abr_tunable("Y264_ABR_TOL", 1.0) * bps;
    double t = wanted / bps;
    if (t > 1.0) buf *= sqrt(t);
    double ov = 1.0 + err / buf;
    /* x264 clips symmetrically to [0.5, 2]. The DOWNWARD side is what hurts on
 * content that is free for a while: sintel's ~13 near-black opening frames
 * cost 38 bytes each while the time-based target accrues, so the overflow
 * reads a large surplus and spends it by lowering QP -- and the real content
 * then arrives at that lowered QP. Y264_ABR_OVLO tightens only that side. */
    /* 0.8: sintel-900 +12.3% -> +9.4% and the sweep saturates there (0.9
     * reads +9.2); bit-identical on five controls including the
     * undershooters (akiyo, stefan). Only the rf model calls this, so the
     * default path never sees it. */
    double lo = abr_tunable("Y264_ABR_OVLO", 0.8);
    if (ov < lo) ov = lo;
    if (ov > 2.0) ov = 2.0;   /* upside proven inert: raising it to 4 or 8
 * changes nothing, so the overflow is NOT
 * saturating and the overshoot is upstream. */
    return ov;
}

/* The decayed P-QP track (x264 keeps the same), non-B only: our B QP comes from
 * last_ref_qp, so feeding B back in would drag the I anchor toward the B level.
 * An I contributes qp + ip_offset to stay in the P domain. */
static void abr_track_update(yah264_encoder_t *e, double qp, int type)
{
    if (type == 2) return;
    e->ptrack_qp *= 0.95;
    e->ptrack_norm *= 0.95;
    e->ptrack_norm += 1.0;
    e->ptrack_qp += qp + (type == 0 ? abr_tunable("Y264_ABR_IPOFF", 2.92) : 0.0);
    e->last_nonb_type = type;
    e->last_ref_qp[1] = e->last_ref_qp[0];
    e->last_ref_qp[0] = qp;
}

static double abr_scale_for(yah264_encoder_t *e, int type)
{
    if (e->abr_inited[type]) return e->abr_scale[type];
    if (e->abr_inited[1]) return e->abr_scale[1];           /* prefer P */
    if (e->abr_inited[0]) return e->abr_scale[0];
    if (e->abr_inited[2]) return e->abr_scale[2];
    return 1.0;
}

static void rc_set_qp(yah264_encoder_t *e, double C, int type)
{
    double err = e->abr_cum_actual - e->abr_cum_target;      /* >0 = over budget */
    double target = e->abr_target_bpf - err * 0.1;
    if (target < 200) target = 200;

    double scale = abr_scale_for(e, type);
    /* Rate-compressed complexity : C^(1-qcomp) dampens the huge
 * intra complexity so an I-frame isn't starved. rc_account calibrates scale
 * against the SAME rceq, so the two stay consistent. */
    double qcomp = abr_qcomp_env();
    double rceq = pow(C, 1.0 - qcomp);
    e->abr_cur_cplx = rceq;                                  /* rc_account calibrates vs rceq */
    double qscale = scale * rceq / target;                  /* bits = scale*rceq/qscale */
    double qp = 12.0 + 6.0 * log2(qscale);
    /* Swing limit only AFTER a real frame is coded -- the seeded scale means the
 * first frame's QP is a real (complexity+target) estimate, not the abr_qp=26
 * guess, so clamping it to abr_qp+/-4 would anchor the IDR to a
 * bitrate-independent QP and starve it. */
    if (e->abr_cum_actual > 0) {
        if (qp < e->abr_qp - 4) qp = e->abr_qp - 4;
        if (qp > e->abr_qp + 4) qp = e->abr_qp + 4;
    }
    if (qp < 1) qp = 1;
    if (qp > 51) qp = 51;
    e->abr_qp = qp;
    e->qp = (int)lround(qp);
    e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);
}

/* After coding: calibrate the complexity->bits scale from what the frame actually
 * cost at its coded QP, and advance the buffer. */
static void rc_account(yah264_encoder_t *e, double bits, int coded_qp, int type)
{
    double qscale = pow(2.0, (coded_qp - 12) / 6.0);
    double s = bits * qscale / e->abr_cur_cplx;
    e->abr_scale[type] = e->abr_inited[type] ? 0.7 * e->abr_scale[type] + 0.3 * s : s;
    e->abr_inited[type] = 1;
    e->abr_cum_actual += bits;
    e->abr_cum_target += e->abr_target_bpf;
}

/* Constant rate factor, x264-style absolute complexity: the base QP tracks the
 * blurred anchor complexity against a resolution-scaled constant,
 * qp = crf + SLOPE * log2(Cblur / (BASE * mbs)), so complex clips run a higher
 * average QP at the same CRF and easy clips a lower one (qcompress).
 *
 * Cblur is the ME-compensated lowres cost (frame_complexity_me: sum of per-MB
 * min(intra, best-inter)), the same signal x264's CRF uses. The earlier build
 * fed the zero-motion full-res SATD here, which conflated motion with coding
 * difficulty (foreman ~= ducks) and, with a base tuned to that ~9000/MB scale,
 * collapsed CRF to ~constant-QP-at-crf -- it spent 2-3x x264's bits, worse on
 * complex content. On the ME signal foreman P sits at ~820/MB and ducks ~1160.
 *
 * BASE (140/MB) and SLOPE (1.8) are calibrated so crf tracks x264's crf bitrate
 * across foreman/akiyo/mobile/ducks/park_joy (0.9-1.6x, VMAF within ~+/-2 on the
 * complex end). SLOPE is shallower than x264's qcompress (6*(1-0.6)=2.4) because
 * yah264's lowres metric spreads content ~1.3x wider than x264's, so 2.4
 * over-penalized complex clips (mobile lost ~5 VMAF). The blur track follows P
 * anchors only: B frames inherit the anchor base (frame_qp adds the type
 * cascade) and I frames read the track without polluting it with intra-domain
 * magnitudes. Base/slope/cap stay env-overridable for re-calibration.
 * ABR/VBV/2-pass still use the self-calibrating zero-motion frame_complexity. */
#define CRF_BASE_CPLX  140.0    /* per-MB ME-compensated cost that sits at qp=crf */
#define CRF_QP_SLOPE   1.8      /* QP per log2 of complexity (x264 qcomp would be 2.4) */
#define CRF_QP_CAP     1.5      /* max QP discount below crf for easy content */

/* Calibration knobs (env-overridable so the base/cap/slope can be swept against
 * x264 without a rebuild; defaults are the baked-in calibration above). */
static double crf_tunable(const char *name, double def)
{
    const char *v = getenv(name);
    return v ? atof(v) : def;
}

/* The AQ frame-mean (DC) term on the CRF base QP (2026-09-05, the across-shot
 * allocation term). The absolute-anchor AQ offset is strength x (log2 energy
 * - anchor); its per-MB part (the within-frame gradient) was calibrated at
 * strength 0.4 on single-shot clips, where its frame mean is a per-clip
 * constant the band cannot see. Across shots that mean IS the allocation:
 * x264 runs it at strength 1.0 and moves ~4 QP between the shots of a
 * concatenation where we moved ~1.5, which turned a 6-14% per-clip lead
 * into a 5-13% deficit on multi-shot sequences. This adds the missing part,
 * (dcstr - acstr) x (frame mean - anchor), to the ANCHOR's base QP; B frames
 * inherit it through frame_qp's anchor base + cascade, so the P/B relation
 * is untouched and every base-keyed consumer (lookahead lambda, cascade,
 * psy, trellis) sees it -- the offset form of the same term lost 2.4% on
 * mobile for exactly that reason. Gate: ms_cif_30 / ms_720p_50 /
 * ms_1080p_25 -8.2 / -4.1 / -6.2% BD-VMAF-NEG vs flat (x264 gap +13.4 /
 * +4.9 / +7.3 -> +4.4 / +0.3 / +0.7); the 12-clip band at matched rate
 * median +0.30 / mean -0.26 / worst +0.94, sintel -5.73. Non-anchor paths
 * (mode-2 AQ on non-reference B) carry no DC of their own by design. */
static double crf_frame_dc(const yah264_encoder_t *e, pixel *const src[3])
{
    if (!e->aq_abs || !src || !src[0]) return 0.0;
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs, n = wmb * hmb;
    int stride = e->pstride[0], cstride = e->pstride[1];
    int cw = e->aq_chroma && src[1] && src[2] ? 16 / e->sub_w : 0, ch = cw ? 16 / e->sub_h : 0;
    double sum = 0;
    for (int mby = 0; mby < hmb; mby++)
        for (int mbx = 0; mbx < wmb; mbx++) {
            const pixel *s = src[0] + (mby * 16) * stride + mbx * 16;
            uint32_t v2[2];
            y264_dsp.var16x16(s, stride, v2);
            double mean = v2[0] / 256.0, var = v2[1] / 256.0 - mean * mean;
            if (var < 0) var = 0;
            double en = var * 256.0;
            if (cw)
                en += blk_ac_energy(src[1] + (mby * ch) * cstride + mbx * cw, cstride, cw, ch)
                    + blk_ac_energy(src[2] + (mby * ch) * cstride + mbx * cw, cstride, cw, ch);
            sum += log2(en < 1.0 ? 1.0 : en) - 8.0;
        }
    double astr = (double)e->aq_strength * 1.0397;
    return (e->aq_dcstr - astr) * (sum / n - e->aq_anchor);
}

static void rc_set_qp_crf(yah264_encoder_t *e, double C, int type)
{
    if (type == 2)
        return;                                 /* B: anchor base + cascade */
    if (type == 1) {
        e->crf_cblur = e->crf_cblur_init ? 0.6 * e->crf_cblur + 0.4 * C : C;
        e->crf_cblur_init = 1;
    }
    double qp;
    if (!e->crf_cblur_init) {
        qp = e->crf;                            /* leading I: no P track yet */
    } else if (e->crf_cl && e->mbtree_on) {
        /* x264 device 2: with mb-tree ON its frame rate equation becomes duration-based
 * (constant per frame) -- frame-level complexity modulation is DROPPED
 * because mb-tree's per-MB offsets already carry the complexity/importance.
 * Keeping the complexity term would double-count it and overspend. So the
 * base is a flat crf; the shift below supplies the operating-point bias. */
        qp = e->crf;
    } else {
        double base_permb = crf_tunable("Y264_CRF_BASE", CRF_BASE_CPLX);
        double slope = crf_tunable("Y264_CRF_SLOPE", CRF_QP_SLOPE);
        double base = base_permb * e->width_in_mbs * e->height_in_mbs;
        double adj = slope * log2(e->crf_cblur / base);
        /* Asymmetric: complex content takes the full upward adjustment, but the
 * easy-content discount is capped -- yah264 already overspends flat
 * content at qp=crf (coding overhead), so letting it drop further is
 * pure VMAF-saturated waste. */
        double cap = crf_tunable("Y264_CRF_CAP", CRF_QP_CAP);
        if (adj < -cap) adj = -cap;
        qp = e->crf + adj;
    }
    /* mb-tree operating-point shift (x264's mb-tree operating-point offset): a fixed
 * uniform +(1-qcomp)*13.5 QP bias. mb-tree's mean-negative per-MB offsets net it
 * back out on average, so the DIFFERENTIAL survives -- static/heavily-referenced
 * anchors (more negative mean_off) net lower QP, motion anchors net higher. Fixed
 * (not per-frame) so it never centres-away the cross-frame benefit; and it aligns
 * the CRF operating point with x264's constant-quality bitrate. */
    if (e->crf_cl && e->mbtree_on) {
        double shift = crf_tunable("Y264_CRF_CL_SHIFT", (1.0 - e->crf_qcomp) * 13.5);
        qp += shift;
    }
    /* Frame-duration term. Under mb-tree x264's rate equation is duration-only
 * (the rate equation:
 * rceq = (base frame duration / clipped frame duration)^(1 - qcomp)
 * which lands on the base QP as +6*(1-qcomp)*log2(0.04/dur). It is zero at
 * 25 fps, +0.63 at 30, +2.4 at 50, -0.14 at 24 -- measured straight off an
 * instrumented x264 (its rate-control mean QP: 31.03 on every CIF clip at 30 fps, 32.80
 * on 720p50, 30.26 on 720p24). Without it CRF N is a different operating
 * point at every frame rate, which is most of why yah264 overspends
 * park_joy and ducks (both 50 fps) at equal CRF. */
    if (crf_fps_env() && e->crf_cl && e->mbtree_on) {
        double fps = (e->param.timebase.fps_num > 0 && e->param.timebase.fps_den > 0)
                   ? (double)e->param.timebase.fps_num / e->param.timebase.fps_den : 25.0;
        double dur = fps > 0 ? 1.0 / fps : 0.04;
        if (dur < 0.01) dur = 0.01;                 /* the clipped duration, as x264 */
        if (dur > 1.00) dur = 1.00;
        qp += 6.0 * (1.0 - e->crf_qcomp) * log2(0.04 / dur);
    }
    if (crf_aqabs_env() && e->crf_cl && e->mbtree_on)
        qp += crf_ped_env();
    qp += e->crf_dc;                            /* Y264_AQ_DC_BASE, 0 otherwise */
    if (qp < 1) qp = 1;
    if (qp > 51) qp = 51;
    e->qp = (int)lround(qp);
    e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);
}

/* The bits this frame may spend if the buffer is to sit on its half-full target
 * -- the reference's buffer-fill target at a one-frame horizon:
 *
 * target = MIN(fill + total_duration * max_rate * 0.5, size * 0.5)
 * allowed = fill + rate - target
 *
 * Why this is the piece yah264 was missing. The fit test below asks only "does
 * THIS frame fit in what is left", so it cannot fire until the buffer is
 * already nearly empty. Under CRF nothing else looks at bits at all --
 * rc_set_qp_crf is open loop, with no bits term anywhere -- so a rate factor
 * whose natural bitrate sits above the cap drains the buffer from full to empty
 * entirely unopposed, and the encode then runs pinned at the boundary, where
 * every prediction error is an underflow. That is exactly the measured
 * signature: a monotonic drain followed by a long tail of small breaches, and
 * it is why the failure gets WORSE as the cap tightens (a tighter cap reaches
 * the boundary sooner and spends more of the clip sitting on it). x264 holds
 * ~44% fill at every cap on this corpus because this goal, not its fit test,
 * is what carries its capped-VBR compliance.
 *
 * Two regimes fall out of the arithmetic. Above half-full, `allowed` is
 * `fill + rate - size/2`: generous, so content that does not need the cap keeps
 * its CRF quality untouched. Below it, `allowed` collapses to `rate/2` -- half
 * of one frame's arrival -- so the buffer climbs back toward half at a bounded
 * rate. It is a ceiling and never a floor: under CRF the encoder codes what the
 * rate factor asks for and this only ever takes bits away, which preserves the
 * deliberate one-sided CRF+VBV rule.
 *
 * Only without a bitrate target. ABR's integrator (the `err` term in rc_set_qp)
 * already enforces the average, and ABR+VBV measures clean with 62% of the
 * buffer still in hand; a second controller there would fight it and cost the
 * rate accuracy. Callers pass the fit-test limit for abr_on, so every ABR, CBR
 * and 2-pass VBV decision stays bit-for-bit what it was.
 *
 * On soundness against the unbounded prediction tail (actual/predicted p99.9
 * in the hundreds of percent): this budget is a function of the DECLARED buffer and the DECLARED
 * rate only. It is never a multiple of a predicted frame size, so there is no
 * margin here to be wrong about. The prediction still chooses the QP, but the
 * occupancy the budget is computed from is advanced from actual coded bits, so
 * a frame that mispredicts by 300% shows up as a lower fill on the very next
 * decide and is answered then. The device does not try to bound a single
 * prediction -- it keeps a reserve so the loop has room to react to one. */
static double vbv_fill_budget(const yah264_encoder_t *e, double fill)
{
    double half = 0.5 * e->vbv_size;
    double target = fill + 0.5 * e->vbv_rate;
    if (target > half) target = half;
    double allowed = fill + e->vbv_rate - target;
    return allowed > 0.0 ? allowed : 0.0;
}

/* The whole non-ABR bits ceiling for one frame at a given occupancy: the fit
 * test and the half-full budget, whichever is tighter. One function so the
 * serial clamp, the pipelined clamp and the first-frame re-encode bound cannot
 * drift apart.
 *
 * There is deliberately no segment-exit term here. One was built -- a budget
 * that tightened as a segment's last frame approached, so the exit occupancy
 * became a constraint rather than a tendency -- and the ablation said to drop
 * it. It never bound on 30 of the gate's 36 cells, because vbv_fill_budget
 * already holds the occupancy at the handoff. Where it did bind it made things
 * worse on both axes at once, and the mechanism is not subtle: a tighter
 * ceiling raises this frame's QP, the bits it saves stay in the buffer, and the
 * NEXT frame's ceiling is looser by exactly that much. Total spend does not
 * fall, it just moves, and the frame that paid for it is worse. It cost a
 * compliance cell (ducks/10000 serial) and spent 600 kbit/s MORE. */
static double vbv_limit_at(const yah264_encoder_t *e, double fill)
{
    double limit = 0.9 * (fill + e->vbv_rate);      /* fit test: don't drain it dry */
    if (!e->abr_on) {                               /* CRF has no integrator */
        double b = vbv_fill_budget(e, fill);
        if (b < limit) limit = b;
    }
    return limit;
}

/* VBV: clamp the base QP so this frame keeps the buffer within bounds. Predict the
 * frame's bits at the current QP from a calibrated bits*qscale/complexity scale;
 * raise QP if it would drain the buffer past a safety margin, lower it if the
 * buffer is near full (so the peak rate is used rather than wasted). */
static void vbv_clip_qp(yah264_encoder_t *e, double C, int type, int is_ref)
{
    if (!e->vbv_inited)
        return;                                     /* need one frame to calibrate */
    int coded = frame_qp(e, type, is_ref);          /* QP the frame will actually use */
    double qscale = pow(2.0, (coded - 12) / 6.0);
    double pred = e->vbv_scale * C / qscale;         /* predicted bits at this QP */

    double limit = vbv_limit_at(e, e->vbv_fill);
    double newqp = e->qp;
    if (limit > 0.0 && pred > limit) {               /* over budget: shrink the frame */
        newqp = e->qp + 6.0 * log2(pred / limit);
    } else if (e->vbv_cbr) {
        /* Only at CBR does spending the spare buffer make sense; for CRF/CQP
 * VBV is a one-sided cap, and for capped VBR the buffer fills at maxrate
 * so this branch would drag the average there (see the vbv_cbr init). */
        double room = e->vbv_fill + e->vbv_rate - e->vbv_size;   /* overflow if >0 */
        if (room > 0 && pred < room)
            newqp = e->qp + 6.0 * log2(pred / room);
    }
    int q = (int)lround(newqp);
    if (q < 1) q = 1;              /* the VBV loop's own floor, below --qpmin */
    q = qp_bound(e, q);
    e->qp = q;
    e->chroma_qp = y264_chroma_qp(q, e->param.chroma_qp_index_offset);
}

/* Advance the buffer after a coded frame and calibrate the bits model. */
static void vbv_update(yah264_encoder_t *e, double bits, int coded_qp)
{
    double qscale = pow(2.0, (coded_qp - 12) / 6.0);
    double s = bits * qscale / e->rc_cplx;
    e->vbv_scale = e->vbv_inited ? 0.7 * e->vbv_scale + 0.3 * s : s;
    e->vbv_inited = 1;
    e->vbv_fill += e->vbv_rate - bits;               /* fill up, drain this frame */
    if (e->vbv_fill > e->vbv_size) e->vbv_fill = e->vbv_size;   /* clamp overflow */
    if (e->vbv_fill < 0) e->vbv_fill = 0;
}

/* 2-pass final: set this frame's coded QP so its share of the target budget
 * (proportional to complexity^qcomp) is met, using pass 1's measured bits at its
 * coded QP as the per-frame bits model. */
static void rc_set_qp_2pass(yah264_encoder_t *e)
{
    if (e->tp_idx >= e->tp_n)
        return;
    int idx = e->tp_idx++;
    struct tp_stat *s = &e->tp_stats[idx];
    double scaleterm = (s->bits + 1) * pow(2.0, (s->qp - 12) / 6.0);   /* QP-invariant cost */
    e->tp_cur_cq = pow(scaleterm, e->crf_qcomp);
    double qp;
    if (e->tp_plan_on && e->tp_q) {
        qp = tp_qscale2qp(tp_plan_qscale(e, idx, e->tp_actual, 0.0));
    } else {
        /* Give this frame its share of the budget still remaining for uncoded frames.
 * Re-planning every frame makes the total self-correct to the target regardless
 * of per-frame model error -- the last frame absorbs whatever is left. */
        double t = e->tp_rem_target * e->tp_cur_cq / (e->tp_rem_cq > 1 ? e->tp_rem_cq : 1);
        if (t < 1) t = 1;
        qp = 12.0 + 6.0 * log2(scaleterm / t);
    }
    if (qp < 1) qp = 1;
    if (qp > 51) qp = 51;
    e->qp = (int)lround(qp);
    e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);
}

/* --- Deterministic fixed-lag RC feedback (Y264_RC_PIPE) ---------------------
 * ABR/2-pass decisions read the committed ledger
 * plus PREDICTIONS for the in-flight frames; actuals commit on a schedule
 * keyed purely to DECIDE order -- a burst pops right after the next anchor's
 * decision -- so the RC-visible op sequence, and therefore the bitstream, is
 * identical at every thread count and pipeline engagement level (unlike
 * x264's threaded ABR). Fills stage actual bits in coding order on the API
 * thread and never touch the ledger; decides run serially in coding order
 * (API thread, or the stair driver between the serial_done / chain-submit
 * handshakes, which order every rcp access). */
#define RCP_MAX ((int)(sizeof ((yah264_encoder_t *)0)->rcp \
                     / sizeof ((yah264_encoder_t *)0)->rcp[0]))

/* Warm phase: the first RCP_WARM decides run with SERIAL-TIGHT feedback (the
 * pipeline stays disengaged and pops are immediate), so the per-type models
 * calibrate in the rcp complexity domain before any lag begins. Keyed on the
 * DECIDE COUNT -- pure config/sequence state, identical at every thread
 * count. Warm-up was the whole measured accuracy gap: a seed error decides an
 * entire mini-GOP under lag, and the CLI's per-GOP encoders restart the
 * transient every keyint. */
#define RCP_WARM rcp_warm_n()

static int rcp_warm(const yah264_encoder_t *e)
{
    return (int)atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) < RCP_WARM;
}

/* Per-type VBV bits model in the rcp decide-complexity domain, falling back
 * across types until calibrated (abr_scale_for's shape). 0.0 = nothing
 * calibrated yet: predictions unavailable, the clip stays off (the serial
 * !vbv_inited guard) and the burst gate stays tight. */
static double rcp_vbv_scale_for(const yah264_encoder_t *e, int type)
{
    if (e->rcp_vbv_cal[type]) return e->rcp_vbv_scale[type];
    if (e->rcp_vbv_cal[1]) return e->rcp_vbv_scale[1];      /* prefer P */
    if (e->rcp_vbv_cal[0]) return e->rcp_vbv_scale[0];
    if (e->rcp_vbv_cal[2]) return e->rcp_vbv_scale[2];
    return 0.0;
}

/* Effective prediction scale: max(per-type, shared). The shared scale is the
 * serial model's shape -- one EMA across every coded frame -- and exists for
 * regime shocks: a huge mispredicted anchor snaps it up, so the B decides
 * that follow predict big and the clip fires (the per-type B scale, trained
 * on the OLD regime, would stay blind for a whole burst). Over-prediction
 * only binds near the constraint, where conservative is correct. */
static double rcp_vbv_scale_eff(const yah264_encoder_t *e, int type)
{
    double vs = rcp_vbv_scale_for(e, type);
    if (e->rcp_vbv_scal && e->rcp_vbv_sscale > vs)
        vs = e->rcp_vbv_sscale;
    return vs;
}

/* Commit one popped entry's actuals into the committed ledger. */
static void rcp_account(yah264_encoder_t *e, const struct rcp_pend *p)
{
    if (e->vbv_on) {
        double qscale = pow(2.0, (p->fqp - 12) / 6.0);
        double s = p->bits * qscale / (p->vcplx > 0 ? p->vcplx : 1.0);
        /* First measurement per type SNAPS (see the abr note below). The QP
 * and complexity regime EMAs travel with the scale: they define where
 * the model is trustworthy (the extrapolation guards). */
        if (e->rcp_vbv_cal[p->type]) {
            e->rcp_vbv_scale[p->type] = 0.7 * e->rcp_vbv_scale[p->type] + 0.3 * s;
            e->rcp_vbv_calqp[p->type] = 0.7 * e->rcp_vbv_calqp[p->type] + 0.3 * p->fqp;
            e->rcp_vbv_calc[p->type]  = 0.7 * e->rcp_vbv_calc[p->type] + 0.3 * p->vcplx;
        } else {
            e->rcp_vbv_scale[p->type] = s;
            e->rcp_vbv_calqp[p->type] = p->fqp;
            e->rcp_vbv_calc[p->type]  = p->vcplx;
        }
        e->rcp_vbv_cal[p->type] = 1;
        e->rcp_vbv_sscale = e->rcp_vbv_scal
                          ? 0.7 * e->rcp_vbv_sscale + 0.3 * s : s;
        e->rcp_vbv_scal = 1;
        /* Shock: decaying max of the accounted overshoot ratio. Capped (one
 * pathological ratio must not sterilise the encoder for long). */
        if (p->vpred > 0) {
            double r = p->bits / p->vpred;
            if (r > 16.0) r = 16.0;
            double sh = 0.7 * e->rcp_vbv_shock;
            e->rcp_vbv_shock = r > sh ? r : sh;
            if (e->rcp_vbv_shock < 1.0) e->rcp_vbv_shock = 1.0;
        }
        if (rcp_dbg_on() && p->vpred > 0)
            fprintf(stderr, "RCPV seq=%u type=%d bits=%.0f vpred=%.0f r=%.3f\n",
                    p->seq, p->type, p->bits, p->vpred, p->bits / p->vpred);
        /* The buffer law, actuals only (the serial vbv_update law). */
        e->vbv_fill += e->vbv_rate - p->bits;
        if (e->vbv_fill > e->vbv_size) e->vbv_fill = e->vbv_size;
        if (e->vbv_fill < 0) { e->vbv_fill = 0; e->rcp_vbv_nclamp++; }
    }
    if (e->abr_on) {
        double qscale = pow(2.0, (p->fqp - 12) / 6.0);
        double s = p->bits * qscale / p->rceq;
        if (e->abr_rf || e->abr_rf2 || abr_rfqp_trace()) {     /* the trace shadows the rf accumulators */
            /* B divides by pb_factor so a B's cheapness cannot drag the whole
 * rate factor down. Under mb-tree the P/B ratio is 1, which is the RF2
 * form. The accumulators never decay: with a VBV the overflow term's
 * window is tied to the buffer instead (abr_vbvov_frac). */
            double pb = p->type == 2 && !e->abr_rf2 ? abr_tunable("Y264_ABR_PBF", 1.3) : 1.0;
            /* Accumulate at the frame's BASE QP (the frame-level QP before the
 * per-MB offsets), which keeps the
 * prediction "bits at base q" self-consistent with frames coded at
 * base + offset. Y264_ABR_RF2_QOFF=1 accumulates at base + mean
 * offset instead (measurement arm). */
            int accqp = e->abr_rf2 && abr_rf2_basedom_on() ? p->acc_qp : p->fqp;   /* see abr_rf2_basedom_on */
            double qacc = e->abr_rf2 ? pow(2.0, (accqp + (abr_tunable("Y264_ABR_RF2_QOFF", 0.0) > 0 ? p->qoff : 0.0) - 12) / 6.0) : qscale;
            double contrib = p->bits * qacc / (p->rceq * pb);
            if (e->abr_rf2 && p->cplx > 0) {
                double k = contrib / p->cplx;
                e->rf2_kc[p->type] = e->rf2_kc_cal[p->type] ? 0.7 * e->rf2_kc[p->type] + 0.3 * k : k;
                e->rf2_kc_cal[p->type] = 1;
            }
            double n_before = e->rf_wanted_bits / e->abr_target_bpf;
            double mean_before = n_before > 0 ? e->rf_cplx_sum / n_before : 0.0;
            e->rf_cplx_sum += contrib;
            e->rf_wanted_bits += e->abr_target_bpf;
            /* Y264_ABR_RF2_DECAY (seconds, 0 = cumulative): the rate factor
 * forgets with this time constant, so a mis-seeded first second
 * (samsung 720p 1200 kbit/s opens 9 QP above where it settles)
 * stops weighing on every later decide; the cumulative rate error
 * keeps its full memory regardless. Measurement arm 2026-09-04. */
            double tau = abr_tunable("Y264_ABR_RF2_DECAY", 0.0);
            if (e->abr_rf2 && tau > 0) {
                double d = 1.0 - 1.0 / (tau * (e->abr_fps > 0 ? e->abr_fps : 25.0));
                if (d > 0 && d < 1) { e->rf_cplx_sum *= d; e->rf_wanted_bits *= d; }
            }
            abr_track_update(e, p->fqp, p->type);
            if (abr_rfqp_trace())
                fprintf(stderr, "RFQA seq=%u type=%d bits=%.0f fqp=%d qoff=%.2f rceq=%.3f | rf_cplx_sum=%.0f wanted=%.0f "
                        "rfqp6=%.2f acc_p=%.2f contrib=%.0f mean_before=%.0f\n", p->seq, p->type, p->bits, p->fqp,
                        p->qoff, p->rceq, e->rf_cplx_sum, e->rf_wanted_bits,
                        6.0 * log2(e->rf_cplx_sum / e->rf_wanted_bits),
                        e->ptrack_norm > 0 ? e->ptrack_qp / e->ptrack_norm : 0.0, contrib, mean_before);
        }
        /* First measurement per type SNAPS over the open-time seed (a guess);
 * later ones EMA. The burst lag decides a whole mini-GOP on the seed,
 * so letting it drag through the EMA doubled the warm-up overshoot. */
        e->abr_scale[p->type] = e->rcp_cal[p->type]
                              ? 0.7 * e->abr_scale[p->type] + 0.3 * s : s;
        /* The calibrated QP regime per type, for the lag guard in rcp_decide.
 * Travels with the scale for the same reason the VBV one does: it is
 * the statement of where this model has actually been measured. */
        e->rcp_abr_calqp[p->type] = e->rcp_cal[p->type]
                                  ? 0.7 * e->rcp_abr_calqp[p->type] + 0.3 * p->fqp
                                  : p->fqp;
        e->rcp_cal[p->type] = 1;
        e->abr_inited[p->type] = 1;
        e->abr_cum_actual += p->bits;
        e->abr_cum_target += e->abr_target_bpf;
    }
    if (e->tp_rctrace)
        fprintf(stderr, "rct type=%d cplx=%.0f bits=%.0f qp=%d\n",
                p->type, p->cplx, p->bits, p->fqp);
    if (e->tp_fp)          /* pass 1 and pass 3 both record */
        fprintf(e->tp_fp, "%d %.0f %.0f %d %d\n", p->type, p->cplx, p->bits,
                p->fqp, p->is_ref ? 1 : 0);
    if (e->tp_pass >= 2) {
        e->tp_rem_target -= p->bits;
        e->tp_rem_cq -= p->cq;
        e->tp_actual += p->bits;
        e->tp_nacc++;
        /* p->pred is this frame's MODELLED bits at the QP it was actually
 * coded at, so the ratio of these two sums is the plan model's
 * systematic bias -- which is what the correction's second term
 * divides out. Only planned frames contribute to both. */
        if (p->tp_rec >= 0)
            e->tp_ebsum += p->pred;
    }
}

/* The pop BOUND: entries decided before this seq may account. The newest non-B
 * decide is the shipped rule and is exactly one burst of lag; Y264_RCP_LAG n
 * holds n bursts back instead, by naming the anchor n decides ago.
 *
 * This is where the lag has to live. DEFERRING THE DRAIN ALONE IS WRONG: it
 * makes the anchor decide lagged at t8 and zero-lag at t1, because the serial
 * path has nothing in flight, so every entry is filled and the commit loop in
 * rcp_decide takes them all. A lag that is a function of what happens to be
 * finished is not a schedule; the bound has to be a coding-order fact, and a
 * decide sequence number is one.
 *
 * Bits ARE allowed to move with the thread count -- e->rcp_lag is 0 at a width
 * that cannot engage, deliberately -- but only as a function of STATIC
 * configuration resolved at open. A drain-only version reads which chains have
 * finished, which would make the ledger depend on scheduling at a fixed thread
 * count, and run-to-run determinism is not negotiable. */
static unsigned rcp_pop_bound(const yah264_encoder_t *e)
{
    int n = e->rcp_lag;
    if (n == 0) return e->rcp_anchor_seq;        /* shipped rule */
    /* The bound has to leave pending exactly the bursts the drain leaves in
 * flight -- and that is a claim about every decide in the burst, not just
 * the anchor's. Keeping burst n-1 live means burst n's own B decides see it
 * unaccounted too, so popping it at the anchor (which the newest-anchor
 * bound does) would make the serial path account a burst the pipelined path
 * still has flying. One step further back per burst kept: hist[0] is the
 * PREVIOUS anchor, so "seq < hist[n-1]" retires everything older than the
 * n bursts the ring is holding. */
    int i = n - 1;
    if (i > Y264_STAIR_K - 1) i = Y264_STAIR_K - 1;
    return e->rcp_anchor_hist[i];
}

/* Pop every filled entry decided BEFORE the pop bound, oldest first. This is
 * the whole determinism argument: pops land between the anchor's decision and
 * the next decide on every path (serial, W2, stair sync/async), so decides
 * always see the same ledger + pending mix. */
static void rcp_pop_ready(yah264_encoder_t *e)
{
    unsigned bound = rcp_pop_bound(e);
    while (e->rcp_n > 0) {
        struct rcp_pend *p = &e->rcp[e->rcp_head];
        if (!p->filled ||
            ((int)(bound - p->seq) <= 0 &&
             p->seq > (unsigned)RCP_WARM && !p->tight))
            break;          /* warm-phase and tight-burst entries pop on fill */
        if (rcp_dbg_on())
            fprintf(stderr, "RCPP seq=%u bits=%.0f\n", p->seq, p->bits);
        if (!p->dropped)
            rcp_account(e, p);
        e->rcp_head = (e->rcp_head + 1) % RCP_MAX;
        e->rcp_n--;
    }
}

/* Actual coded bits arrive (coding order == push order). */
static void rcp_fill(yah264_encoder_t *e, double bits)
{
    for (int i = 0; i < e->rcp_n; i++) {
        struct rcp_pend *p = &e->rcp[(e->rcp_head + i) % RCP_MAX];
        if (!p->filled) {
            p->filled = 1; p->bits = bits;
            if (rcp_dbg_on())
                fprintf(stderr, "RCPF seq=%u bits=%.0f\n", p->seq, bits);
            break;
        }
    }
    rcp_pop_ready(e);
}

/* Re-price the newest un-filled ledger entry after the first-frame bound moved
 * the QP out from under it. rcp_decide wrote the entry with the QP the model
 * chose; the re-encode ships a different QP, and if the entry keeps the old one
 * then the virtual buffer advances on a frame that was never coded and the
 * accounting calibrates the bits model against the wrong quantiser.
 *
 * This is the half of the retry the write-up warned would be easy to miss: the
 * NAL and the ledger both have to see it. Deterministic -- it reads only the
 * decided QP and the entry, never timing or thread count. */
static void rcp_reqp(yah264_encoder_t *e, int type, int is_ref)
{
    if (e->rcp_n <= 0)
        return;
    struct rcp_pend *p = &e->rcp[(e->rcp_head + e->rcp_n - 1) % RCP_MAX];
    if (p->filled)
        return;                             /* already accounted; not ours */
    int nf = frame_qp(e, type, is_ref);
    double r = pow(2.0, (double)(p->fqp - nf) / 6.0);   /* bits track 1/qscale */
    p->base_qp = e->qp;
    p->fqp = nf;
    p->pred *= r;
    p->vpred *= r;
}

/* CAVLC-overflow drop: remove without accounting (mirrors the serial skip). */
static void rcp_drop(yah264_encoder_t *e)
{
    for (int i = 0; i < e->rcp_n; i++) {
        struct rcp_pend *p = &e->rcp[(e->rcp_head + i) % RCP_MAX];
        if (!p->filled) { p->filled = 1; p->dropped = 1; p->bits = 0; break; }
    }
    rcp_pop_ready(e);
}

/* Flush/close: commit everything that has bits. The caller's flush points are
 * part of the API call sequence, identical at every thread count. */
static void rcp_flush_all(yah264_encoder_t *e)
{
    while (e->rcp_n > 0 && e->rcp[e->rcp_head].filled) {
        struct rcp_pend *p = &e->rcp[e->rcp_head];
        if (rcp_dbg_on())
            fprintf(stderr, "RCPX seq=%u bits=%.0f\n", p->seq, p->bits);
        if (!p->dropped)
            rcp_account(e, p);
        e->rcp_head = (e->rcp_head + 1) % RCP_MAX;
        e->rcp_n--;
    }
}

/* The VIRTUAL buffer: the committed fill (actuals only) plus a conservative
 * advance for each in-flight entry -- rate in, r_hi * vpred out, clamped at the
 * rim per step exactly like the serial law. Pure function of (ledger, pending)
 * state, so it is identical on every path and thread count.
 *
 * drop_tail is how many of the newest entries to leave out. rcp_vbv_clip runs
 * before its own entry is pushed and passes 0; the first-frame measured-size
 * bound runs after and passes 1, so both read the occupancy the frame is
 * removed FROM rather than one that already charges the frame against itself. */
static double rcp_vbv_vfill(const yah264_encoder_t *e, int drop_tail)
{
    double rhi = vbv_rhi_env();
    double fill = e->vbv_fill;
    int n = e->rcp_n - drop_tail;
    for (int i = 0; i < n; i++) {
        const struct rcp_pend *p = &e->rcp[(e->rcp_head + i) % RCP_MAX];
        fill += e->vbv_rate - rhi * p->vpred;
        if (fill > e->vbv_size) fill = e->vbv_size;
        if (fill < 0) fill = 0;
    }
    return fill;
}

/* VBV clip against the virtual buffer, then the serial vbv_clip_qp math. At an
 * anchor decide the pending set is empty (zero-lag) and this IS the serial
 * computation; only in-burst B decides see the conservative bound, and only in
 * pipelined (non-tight) bursts -- a tight burst's pending set is empty too
 * (pops at fill). */
static void rcp_vbv_clip(yah264_encoder_t *e, double C, int type, int is_ref)
{
    double vs = rcp_vbv_scale_eff(e, type);
    if (vs <= 0.0)
        return;                             /* nothing calibrated yet (serial
 * !vbv_inited guard) */
    double fill = rcp_vbv_vfill(e, 0);      /* entry not pushed yet: walk them all */
    int coded = frame_qp(e, type, is_ref);
    double qscale = pow(2.0, (coded - 12) / 6.0);
    double pred = vs * C / qscale;
    if (e->rcp_vbv_shock > 1.0)
        pred *= e->rcp_vbv_shock;   /* recent overshoot: distrust the model */
    /* The budgets on the VIRTUAL fill -- the buffer after the in-flight frames
 * are charged. That is this path's version of x264's VBV plan: an
 * anticipated occupancy rather than a committed one, so a drain already in
 * flight is answered now instead of one burst later. */
    double limit = vbv_limit_at(e, fill);
    double newqp = e->qp;
    if (limit > 0.0 && pred > limit) {      /* over budget: shrink the frame */
        newqp = e->qp + 6.0 * log2(pred / limit);
    } else if (e->vbv_cbr) {
        /* Spend spare buffer only at CBR (serial rule -- see vbv_cbr's init:
 * capped VBR fills at maxrate and this branch would chase it). */
        double room = fill + e->vbv_rate - e->vbv_size;
        if (room > 0 && pred < room)
            newqp = e->qp + 6.0 * log2(pred / room);
    }
    int q = (int)lround(newqp);
    if (q < 1) q = 1;              /* the VBV loop's own floor, below --qpmin */
    q = qp_bound(e, q);
    e->qp = q;
    e->chroma_qp = y264_chroma_qp(q, e->param.chroma_qp_index_offset);
}

/* Per-burst VBV fallback trigger, at anchor ARRIVAL on the API thread with
 * every in-flight frame already retired (the caller drains first). First
 * commits filled tail -- the same position in decide order the anchor's
 * own commit-all would use, so the ledger this reads equals what the decide
 * will read. Then simulates the coming burst's worst case: each frame charged
 * r_hi times its model prediction at the current base QP (the +-4 swing limit
 * bounds the drift the estimate misses; r_hi covers the rest). If any prefix
 * of that trajectory underflows, the burst runs the serial K=0 schedule --
 * stair/fpipe disengaged, rc_waits forced, entries popping at fill -- which
 * is byte-for-byte the serial VBV information flow. Deterministic: reads only
 * decided state (config + content), never timing or thread count. */
static void rcp_vbv_gate(yah264_encoder_t *e, int is_idr)
{
    while (e->rcp_n > 0 && e->rcp[e->rcp_head].filled) {
        struct rcp_pend *p = &e->rcp[e->rcp_head];
        if (rcp_dbg_on())
            fprintf(stderr, "RCPA seq=%u bits=%.0f\n", p->seq, p->bits);
        if (!p->dropped)
            rcp_account(e, p);
        e->rcp_head = (e->rcp_head + 1) % RCP_MAX;
        e->rcp_n--;
    }
    e->rcp_vbv_nburst++;
    int tight = 1;
    double vs1 = rcp_vbv_scale_eff(e, 1);
    if (!rcp_warm(e) && !is_idr && vs1 > 0.0) {
        /* Warm phase is serial-tight anyway; an IDR burst has no trailing B's
 * to ride predictions (pre-IDR B's flush as P frames, each deciding
 * on full actuals) and its intra spike is the classic underflow shape
 * -- stay serial there and reconcile exactly. */
        double rhi = vbv_rhi_env();
        if (e->rcp_vbv_shock > rhi)
            rhi = e->rcp_vbv_shock;     /* recent overshoot widens the margin */
        double cj = vbv_cjump_env();
        double fill = e->vbv_fill;
        tight = 0;
        /* Content-jump guard: the scale is calibrated on the regime it has
 * SEEN; a coming frame whose complexity jumps far past it (missed
 * cut, fade-end flash) breaks the model by more than any fixed
 * multiplier. Take the serial schedule for that burst. */
        if (e->rcp_vbv_calc[1] > 0 && e->rcp_arr_cvi > cj * e->rcp_vbv_calc[1])
            tight = 1;
        if (e->rcp_vbv_cal[2] && e->rcp_vbv_calc[2] > 0)
            for (int i = 0; i < e->nbuf; i++)
                if (e->rcp_bcvi[i] > cj * e->rcp_vbv_calc[2])
                    tight = 1;
        /* The anchor at the current base QP (type-1 frame_qp offset is 0)... */
        double q = pow(2.0, (e->qp - 12) / 6.0);
        fill += e->vbv_rate - rhi * vs1 * e->rcp_arr_cvi / q;
        if (fill > e->vbv_size) fill = e->vbv_size;
        if (fill < 0) tight = 1;
        /* ...then the B's at the smallest cascade offset (+1) over the QPD
 * floor the pipelined decide will enforce: the most-bits bound over
 * every pyramid depth, in the regime the model can speak to. */
        double vs2 = rcp_vbv_scale_eff(e, 2);
        double bqp = e->qp + 1;
        if (e->rcp_vbv_cal[2]) {
            int lo = (int)lround(e->rcp_vbv_calqp[2]) - vbv_qpd_env();
            if (bqp < lo) bqp = lo;
        }
        double qb = pow(2.0, (bqp - 12) / 6.0);
        for (int i = 0; i < e->nbuf && !tight; i++) {
            fill += e->vbv_rate - rhi * vs2 * e->rcp_bcvi[i] / qb;
            if (fill > e->vbv_size) fill = e->vbv_size;
            if (fill < 0) tight = 1;
        }
    }
    if (vbv_force_env())
        tight = 0;                          /* measurement-only override */
    e->rcp_vbv_tight = tight;
    e->rcp_vbv_ntight += tight;
    if (rcp_dbg_on())
        fprintf(stderr, "RCPG tight=%d fill=%.0f nbuf=%d\n",
                tight, e->vbv_fill, e->nbuf);
}

/* The lagged QP decision + entry push. Complexity C: type 0 = the source-only
 * intra SATD (computed here from src, exactly the serial signal); type 1/2 =
 * the ARRIVAL-captured lowres cost in e->rcp_cur_cme -- never recon, which
 * may be streaming under the staircase. The prediction is the model's own
 * expectation at the decided coded QP, so prediction error == model error and
 * the err*0.1 correction absorbs it (the reference's overflow-window shape). */
/* RF2 opening QP (Y264_ABR_RF2_SEED, default 2). The cumulative rate factor
 * never forgets its opening: a first second coded far from where the loop
 * settles is a deficit or surplus the proportional overflow term repays only
 * slowly (its window grows with sqrt(t)). The resolution-only seed opened
 * samsung 720p 1200 kbit/s at QP 33 (settles 29), bus CIF at 35 (settles 38),
 * ducks 720p 28 Mbit/s at QP 6 (settles 27) and riverbed at 15 (settles 34) --
 * the -14% / +18% board misses. A fixed opening (the reference opens every
 * ABR encode at one QP) is right for foreman and wrong by 15 QP for stefan.
 *   mode 0: the resolution-only seed;
 *   mode 1: fixed Y264_ABR_RF2_SEEDQ;
 *   mode 2: fitted on the lookahead window: the median lowres inter cost per
 *           macroblock of the frames that arrived before the first decide and
 *           the target bits per macroblock,
 *             qp = 8.69 + 2.92 log2(cme/mb) - 1.32 log2(bits/mb), clamped 15..45,
 *           least squares on 17 non-board cells (fit rms 3.3 QP), scoring the
 *           ten board clips out of sample at rms 2.5 / max 4.1 QP
 *           (local record abr-undershoot-2026-09-04). */
static double rf2_open_qp(yah264_encoder_t *e)
{
    int mode = (int)abr_tunable("Y264_ABR_RF2_SEED", 2.0);
    double sq = abr_tunable("Y264_ABR_RF2_SEEDQ", 0.0);
    if (mode == 1 && sq > 0) return sq;
    if (mode != 2) return 0.0;
    /* The lookahead ring at the first decide: every buffered frame's
 * min(intra, vs-previous) lowres sum, the same metric the rcp decide
 * reads per frame. The oldest entry is the frame being decided (its
 * inter cost is against nothing) and is skipped. */
    /* A FIXED window of the oldest 32 buffered frames, not "whatever the
 * ring holds": la_n at the first decide grows with the lookahead lead,
 * which is a function of the thread count, and a thread-dependent median
 * made the opening QP (and every later bit) differ between --threads 1
 * and 8 (review 2026-09-04). Index 0 is the oldest BUFFERED frame -- the
 * frame being decided was already popped -- so it is a live sample. */
    double v[32]; int n = 0;
    for (int i = 0; i < e->la_n && n < (int)(sizeof v / sizeof v[0]); i++) {
        const struct la_entry *en = &e->la[(e->la_head + i) % e->la_cap];
        if (en->sum_cost[LR_LEG_PREV] > 0) v[n++] = (double)en->sum_cost[LR_LEG_PREV];
    }
    if (abr_rfqp_trace())
        fprintf(stderr, "RFOPEN mode=%d la_n=%d usable=%d\n", mode, e->la_n, n);
    if (n < 2) return 0.0;
    for (int i = 1; i < n; i++) { double t = v[i]; int j = i; while (j > 0 && v[j-1] > t) { v[j] = v[j-1]; j--; } v[j] = t; }
    double cme = n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    double nmb = (double)(e->width_in_mbs * e->height_in_mbs);
    if (nmb <= 0 || cme < 1.0 || e->abr_target_bpf <= 0) return 0.0;
    double qp = abr_tunable("Y264_ABR_RF2_SEED_A", 8.692)
              + abr_tunable("Y264_ABR_RF2_SEED_B", 2.924) * log2(cme / nmb)
              + abr_tunable("Y264_ABR_RF2_SEED_C", -1.315) * log2(e->abr_target_bpf / nmb);
    if (qp < 15) qp = 15;
    if (qp > 45) qp = 45;
    if (abr_rfqp_trace())
        fprintf(stderr, "RFOPEN n=%d cme/mb=%.0f bits/mb=%.1f -> qp %.2f\n", n, cme / nmb, e->abr_target_bpf / nmb, qp);
    return qp;
}

static void rcp_decide(yah264_encoder_t *e, int type, int is_ref,
                       pixel *const src[3])
{
    double C = type == 0 ? frame_complexity(e, src, 1) : e->rcp_cur_cme;
    if (e->abr_on && type != 0 && abr_cfloor_on()) {
        double f = e->rcp_cur_cvi * abr_cfloor_frac();
        if (f > C) C = f;
    }
    /* VBV-model complexity: the robust source-energy metric, ONE lowres
 * domain for every type so the shared shock-propagation scale is
 * meaningful across types (cur_cvi is captured for anchors and B's
 * alike). */
    double Cv = e->rcp_cur_cvi;
    /* ZERO-LAG ANCHORS: every driver guarantees all prior actuals are staged
 * before a non-B decide (rc_waits drains the W2 pending for non-B frames;
 * the stair drains the fly burst before the launch decide), so anchors
 * commit the whole ledger here and decide on full actuals. Only B frames
 * decide on predictions -- their own burst's, corrected at the next
 * anchor. This halved the measured rate deviation vs the burst-lagged
 * anchor schedule and costs only the anchor-vs-burst-tail overlap the v4
 * window metric already showed was ~0.5-2 ms/anchor.
 *
 * Under Y264_RCP_LAG this loop is exactly what must NOT run. "Everything
 * filled" is a statement about which chains have finished, and on the
 * serial path that is everything -- so it silently restores zero lag at
 * t1 while t8 keeps the burst, and the bits move with the thread count.
 * The seq bound in rcp_pop_ready carries the lag instead, on every path. */
    if (type != 2 && e->rcp_lag == 0)
        while (e->rcp_n > 0 && e->rcp[e->rcp_head].filled) {
            struct rcp_pend *p = &e->rcp[e->rcp_head];
            if (rcp_dbg_on())
                fprintf(stderr, "RCPA seq=%u bits=%.0f\n", p->seq, p->bits);
            if (!p->dropped)
                rcp_account(e, p);
            e->rcp_head = (e->rcp_head + 1) % RCP_MAX;
            e->rcp_n--;
        }
    double pend_pred = 0.0, pend_cq = 0.0, pend_cplxr = 0.0;
    int np = e->rcp_n;
    int rf2_inflight = e->abr_on && e->abr_rf2 && abr_rf2_inflight_env();
    double rf2_mean = e->rf_wanted_bits > 0 ? e->rf_cplx_sum / (e->rf_wanted_bits / e->abr_target_bpf) : 0.0;
    for (int i = 0; i < e->rcp_n; i++) {
        const struct rcp_pend *p = &e->rcp[(e->rcp_head + i) % RCP_MAX];
        if (rf2_inflight) {
            /* A5b: the frame's own complexity when its type is calibrated,
 * the type mean until then. Bits at its decided QP follow from the
 * same contribution (contrib = bits * qscale / rceq). */
            double c = e->rf2_kc_cal[p->type] && p->cplx > 0 ? e->rf2_kc[p->type] * p->cplx : rf2_mean;
            pend_cplxr += c;
            pend_pred += c * p->rceq / pow(2.0, ((abr_rf2_basedom_on() ? p->acc_qp : p->fqp) - 12) / 6.0);
            pend_cq += p->cq;
            continue;
        }
        /* Re-evaluate the in-flight prediction against the CURRENT model
 * (x264 recomputes predicted bits continuously): when a pop just
 * recalibrated the scale, the standing predictions see the surge a
 * whole burst earlier. Pure function of (entry, ledger) state --
 * deterministic. ABR only; a 2-pass entry's scaleterm is static. */
        pend_pred += e->abr_on
                   ? abr_scale_for(e, p->type) * p->rceq
                     / pow(2.0, (p->fqp - 12) / 6.0)
                   : p->pred;
        pend_cq += p->cq;
    }
    struct rcp_pend en;
    memset(&en, 0, sizeof en);
    en.tp_rec = -1;                             /* 0 is a valid record index */
    en.type = (int8_t)type;
    en.is_ref = (int8_t)is_ref;
    en.cplx = C;
    en.bits = -1.0;
    en.qoff = e->mbtree_mean_off;
    double abr_scale_used = 0.0, tp_scaleterm = 0.0;
    int tp_have = 0;
    if (e->abr_on) {
        /* rc_set_qp's equations against the VIRTUAL ledger: committed actuals
 * plus in-flight predictions, each pending frame owing one target_bpf. */
        double err = (e->abr_cum_actual + pend_pred)
                   - (e->abr_cum_target + np * e->abr_target_bpf);
        if (rcp_dbg_on())
            fprintf(stderr, "RCPE seq=%u err=%.0f\n",
                    atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) + 1, err);
        double target = e->abr_target_bpf - err * rcp_gain();
        if (target < 200) target = 200;
        double scale = abr_scale_for(e, type);
        double rceq = pow(C, 1.0 - abr_qcomp_env());
        if (e->abr_rf2) {
            /* A2: the CRF path plus a rate factor (abr_rf2_env). */
            rceq = e->rf2_rceq;
            if (type != 2) {                   /* B: anchor base + frame_qp cascade, as CRF */
                int first = e->abr_cum_actual <= 0 && np == 0;
                if (first && !e->rf2_opened) {
                    double oq = rf2_open_qp(e);
                    if (oq > 0)
                        e->rf_cplx_sum = pow(2.0, (oq - 12.0) / 6.0) * e->abr_target_bpf / e->rf2_rceq;
                    e->rf2_opened = 1;
                }
                double n_done = e->rf_wanted_bits / e->abr_target_bpf;
                double wanted = e->rf_wanted_bits + np * e->abr_target_bpf;
                double cplxr = e->rf_cplx_sum;
                if (rf2_inflight) cplxr += pend_cplxr;
                else if (np > 0 && n_done > 0) cplxr += np * (e->rf_cplx_sum / n_done);
                double q = rceq * cplxr / (wanted > 0 ? wanted : 1.0);
                double ov = 1.0;
                if (!first) {
                    double bps = e->abr_target_bpf * (e->abr_fps > 0 ? e->abr_fps : 25.0);
                    double t = bps > 0 ? wanted / bps : 0.0;
                    double buf = 2.0 * abr_tunable("Y264_ABR_TOL", 1.0) * bps * (t > 1.0 ? sqrt(t) : 1.0);
                    if (e->vbv_on && e->vbv_size > 0 && abr_vbvov_frac() > 0 && buf > abr_vbvov_frac() * e->vbv_size)
                        buf = abr_vbvov_frac() * e->vbv_size;
                    ov = buf > 0 ? 1.0 + err / buf : 1.0;
                    if (ov < 0.5) ov = 0.5;
                    if (ov > 2.0) ov = 2.0;
                    q *= ov;
                }
                double q_eq = q;
                int anch = 0;
                /* Mid-stream I frames ride the decayed P track (P domain;
 * frame_qp's -3 is the I/P ratio). Everything else takes an
 * asymmetric per-type step clip against the last qscale
 * of its type: one qp_step (4) per frame, widened one more step
 * on the side the overflow is pushing. Load-bearing at startup
 * (the first I's cost dominates the young integrator, and
 * without the clip the second frame reads 51) and on content
 * whose cost collapses (sintel's black opening). The first frame
 * is unclipped and seeds the P track at seed x the I/P ratio. */
                if (type == 0 && !first && e->last_nonb_type != 0 && e->ptrack_norm > 0) {
                    q = pow(2.0, (e->ptrack_qp / e->ptrack_norm - 12.0) / 6.0);
                    anch = 1;
                } else if (!first) {
                    double lstep = pow(2.0, abr_tunable("Y264_ABR_QPSTEP", e->qp_step) / 6.0);
                    double lo = e->last_qscale_type[type] / lstep;
                    double hi = e->last_qscale_type[type] * lstep;
                    if (ov > 1.1 && n_done > 3) hi *= lstep;   /* widen only after the first frames */
                    else if (ov < 0.9) lo /= lstep;
                    if (q < lo) q = lo;
                    if (q > hi) q = hi;
                }
                e->last_qscale_type[type] = q;
                if (first) e->last_qscale_type[1] = q * abr_tunable("Y264_ABR_IPF", 1.4);
                double qp = 12.0 + 6.0 * log2(q > 0 ? q : 1e-9);
                /* The first I is coded at the seed QP itself; frame_qp takes
 * 3 off every I, so hand it the seed plus 3. */
                if (type == 0 && first) qp += 3.0;
                if (qp < 1) qp = 1;
                if (qp > 51) qp = 51;
                e->abr_qp = qp;
                e->qp = qp_bound(e, (int)lround(qp));
                e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);
                if (abr_rfqp_trace())
                    fprintf(stderr, "RFQP seq=%u type=%d C=%.0f rceq=%.3f np=%d | rf2 cplxr=%.0f wanted=%.0f "
                            "err=%.0f ov=%.3f qp_eq=%.2f anch=%d qp=%.2f moff=%.2f | live=rf2 qp=%d\n",
                            atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) + 1, type, C, rceq,
                            np, cplxr, wanted, err, ov, 12.0 + 6.0 * log2(q_eq > 0 ? q_eq : 1e-9),
                            anch, qp, e->mbtree_mean_off, e->qp);
            } else if (abr_rfqp_trace()) {
                fprintf(stderr, "RFQP seq=%u type=2 C=%.0f rceq=%.3f np=%d | rf2 B on anchor base | live=rf2 qp=%d\n",
                        atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) + 1, C, rceq, np, e->qp);
            }
        } else {
            double qscale = scale * rceq / target;
            double qp = 12.0 + 6.0 * log2(qscale);
            int anch = 0;
            int tr_on = abr_rfqp_trace();
            double tr_qp_eq = qp;                    /* default model, pre-clamp */
            double tr_blur = rceq, tr_cplxr = 0.0, tr_wanted = 0.0, tr_rf = 0.0;
            double tr_ovf = 1.0, tr_qp_raw = 0.0, tr_qp_rf = 0.0, tr_anch = -1.0;
            /* CGUARD part 2: a degenerate frame must
 * not move the rf model's lstep anchor either. During a black
 * opening the computed q clamps to last/lstep every frame, so
 * last_qscale_type decays exponentially (~2^8.7 over sintel's 13
 * black frames) and the post-cut frames pay it back at QP~1 under
 * the one-lstep-per-frame recovery -- the residual the frozen
 * abr_qp alone leaves. Same criterion as part 1 below. */
            double cpm_g = C / (double)(e->width_in_mbs * e->height_in_mbs);
            int degen = abr_cguard_on() && cpm_g < abr_cguard_thresh();
            if (e->abr_rf || tr_on) {
                /* x264: the rate factor runs on the
 * blurred complexity, an EWMA with 0.5 decay, not the raw
 * per-frame value. I frames are excluded because our I
 * complexity is full-res intra SATD, a different domain from
 * the lowres cme the P/B path uses -- mixing them in one
 * accumulator would be meaningless. */
                if (type != 0 && !degen) {   /* CGUARD: black frames must not drain the EWMA */
                    e->st_cplxsum *= 0.5;
                    e->st_cplxcount *= 0.5;
                    e->st_cplxsum += C;
                    e->st_cplxcount += 1.0;
                }
                if (type != 0 && e->st_cplxcount > 0)
                    tr_blur = pow(e->st_cplxsum / e->st_cplxcount,
                                  1.0 - abr_qcomp_env());
                if (e->abr_rf) rceq = tr_blur;   /* the shadow (Y264_ABR_RFQP) leaves the default model's rceq alone */
                /* P: the self-normalising rate factor. In-flight frames owe
 * their target on the wanted side; their cplxr contribution is
 * the current mean scale, which is what the model would predict
 * for them anyway. The overflow carries the rest of the
 * in-flight error, exactly as err already measures it. */
                double n_done = e->rf_wanted_bits / e->abr_target_bpf;
                double wanted = e->rf_wanted_bits + np * e->abr_target_bpf;
                double cplxr = e->rf_cplx_sum;
                if (np > 0 && n_done > 0) cplxr += np * (e->rf_cplx_sum / n_done);
                double rf = wanted / (cplxr > 0 ? cplxr : 1.0);
                double q = tr_blur / (rf > 0 ? rf : 1.0);
                q *= abr_overflow(e, err, wanted);
                tr_cplxr = cplxr; tr_wanted = wanted; tr_rf = rf;
                tr_qp_raw = 12.0 + 6.0 * log2(q > 0 ? q : 1e-9);
                /* x264's asymmetric per-type clip .
 * WITHOUT THIS the rate factor runs away on content whose
 * complexity signal legitimately collapses: sintel opens on
 * near-black frames with C ~ 1, so cplxr stops growing while
 * wanted grows linearly, rf goes 14 -> 163 in fourteen frames
 * and qp goes NEGATIVE. HEAD survives that only because its
 * +/-4 swing limit absorbs it. Asymmetric because symmetric
 * would block overflow control in rapidly oscillating
 * complexity, which is x264's own stated reason. */
                double lstep = pow(2.0, abr_tunable("Y264_ABR_QPSTEP", e->qp_step) / 6.0);
                double lo = e->last_qscale_type[type] / lstep;
                double hi = e->last_qscale_type[type] * lstep;
                double ovf = abr_overflow(e, err, wanted);
                if (ovf > 1.1) hi *= lstep;
                else if (ovf < 0.9) lo /= lstep;
                if (q < lo) q = lo;
                if (q > hi) q = hi;
                if (!degen) e->last_qscale_type[type] = q;
                tr_ovf = ovf; tr_qp_rf = 12.0 + 6.0 * log2(q);
                if (type == 0 && e->last_nonb_type != 0 && e->abr_cum_actual > 0)
                    tr_anch = e->ptrack_qp / e->ptrack_norm;
                else if (type == 2 && e->last_ref_qp[0] >= 0)
                    tr_anch = (e->last_ref_qp[0] +
                               (e->last_ref_qp[1] >= 0 ? e->last_ref_qp[1] : e->last_ref_qp[0])) / 2.0;
            }
            if (e->abr_rf) {
                qp = tr_qp_rf;
                /* I and B do not run the equation at all and
 * 2434-2450). frame_qp's cascade (I -3, B +1..4) is applied on
 * top of these, which is the shape x264's ip/pb offsets give. */
                if (type == 0 && e->last_nonb_type != 0 && e->abr_cum_actual > 0) {
                    qp = e->ptrack_qp / e->ptrack_norm;
                    anch = 1;
                } else if (type == 2 && e->last_ref_qp[0] >= 0) {
                    double r0 = e->last_ref_qp[0];
                    double r1 = e->last_ref_qp[1] >= 0 ? e->last_ref_qp[1] : r0;
                    qp = (r0 + r1) / 2.0;
                    anch = 1;
                }
            }
            if (!anch && (e->abr_cum_actual > 0 || np > 0)) {  /* swing limit after frame 1 */
                /* Warm-up guard: while this TYPE's model is uncalibrated (the
 * burst lag decides a whole mini-GOP before its first actual
 * lands), halve the downward swing -- a 2x seed error is 6 QP
 * and the failure mode is overspend, not starvation. Rising
 * keeps the full swing. */
                double lo = e->abr_qp - (e->rcp_cal[type] ? 4 : 2);
                if (qp < lo) qp = lo;
                if (qp > e->abr_qp + 4) qp = e->abr_qp + 4;
            }
            /* LAG WINDUP GUARD (Y264_RCP_QPD, off unless a lag budget is set).
 * The swing limit above bounds one STEP, and with zero lag that is
 * enough because an actual lands between every anchor's steps. Under
 * lag several decides run before anything lands, all reading the
 * same uncorrected error, so the ladder walks the full limit in one
 * direction repeatedly -- measured, it reaches QP 51 on akiyo and
 * comes back, with the per-decide sd going 4.5 -> 11.7. That is the
 * VBV round's extrapolation failure arriving in plain ABR, which had
 * never met it because it had never had anchor lag.
 *
 * Same device as Y264_VBV_QPD, and symmetric because this runaway
 * goes both ways: hold the decide within QPD of the regime the model
 * was actually calibrated at. Pure function of the accounted ledger,
 * so it is a coding-order fact like everything else here. */
            int qpd = rcp_qpd_env();
            if (qpd > 0 && e->rcp_cal[type] && !anch) {
                double c = e->rcp_abr_calqp[type];
                if (qp < c - qpd) qp = c - qpd;
                if (qp > c + qpd) qp = c + qpd;
            }
            if (qp < 1) qp = 1;
            if (qp > 51) qp = 51;
            /* DEGENERATE-COMPLEXITY GUARD (Y264_ABR_CGUARD). A frame whose
 * complexity sits at the floor -- sintel opens on ~13 near-black
 * frames, C=1258 against a clip mean two orders larger -- carries no
 * information about the rate model, but it still ratchets abr_qp
 * down to the clamp. The +/-4 swing limit then needs ~9 frames to
 * climb back, and on sintel those are exactly the frames after the
 * scene cut: we spend 76% of the clip's bits there where x264 spends
 * 24%. Under CRF, which has no cumulative state, we concentrate 9.8%
 * against x264's 17.6% -- so this is purely an RC-state defect, not
 * content. Code the frame at the computed QP, but do not let it move
 * the remembered one. */
            /* The criterion has to be ABSOLUTE, per macroblock. A running mean
 * does not work here: the degenerate frames come FIRST, so they
 * define the mean and nothing is ever far enough below it. sintel's
 * black opening reads 1258/3600 = 0.35 per MB where the frame after
 * the cut reads 49523/3600 = 13.8, so a few units per MB separates
 * them cleanly. */
            if (!degen) e->abr_qp = qp;
            e->qp = (int)lround(qp);
            e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);
            if (tr_on)
                fprintf(stderr, "RFQP seq=%u type=%d C=%.0f rceq=%.1f blur=%.1f np=%d degen=%d | "
                        "dflt err=%.0f target=%.0f scale=%.1f qp_eq=%.2f qp=%.2f | "
                        "rf cplxr=%.0f wanted=%.0f rf=%.4f ovf=%.3f qp_raw=%.2f qp_lstep=%.2f anch=%.2f | "
                        "live=%s qp=%d\n",
                        atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) + 1,
                        type, C, rceq, tr_blur, np, degen,
                        err, target, scale, tr_qp_eq, e->abr_rf ? 0.0 : qp,
                        tr_cplxr, tr_wanted, tr_rf, tr_ovf, tr_qp_raw, tr_qp_rf, tr_anch,
                        e->abr_rf ? "rf" : "dflt", e->qp);
        }
        en.rceq = rceq;
        abr_scale_used = scale;
    } else if (e->tp_pass >= 2) {
        if (e->tp_idx < e->tp_n) {
            int idx = e->tp_idx++;
            struct tp_stat *s = &e->tp_stats[idx];
            double scaleterm = (s->bits + 1) * pow(2.0, (s->qp - 12) / 6.0);
            en.cq = pow(scaleterm, e->crf_qcomp);
            double qp;
            if (e->tp_plan_on && e->tp_q) {
                /* Same virtual ledger the ABR branch uses: committed bits plus
 * the in-flight predictions, so a decide under lag sees the
 * frames its own burst has already claimed. The PLAN is not
 * lagged -- it is a function of the stats file alone -- so only
 * the bounded correction moves, and it moves identically at
 * every thread count because pend_pred is a decide-order fact. */
                qp = tp_qscale2qp(tp_plan_qscale(e, idx, e->tp_actual, pend_pred));
                en.tp_rec = idx;
            } else {
                /* rc_set_qp_2pass against the virtual remainder: in-flight frames
 * have already claimed their predicted bits + cq share. */
                double remcq = e->tp_rem_cq - pend_cq;
                double t = (e->tp_rem_target - pend_pred) * en.cq
                         / (remcq > 1 ? remcq : 1);
                if (t < 1) t = 1;
                qp = 12.0 + 6.0 * log2(scaleterm / t);
            }
            if (qp < 1) qp = 1;
            if (qp > 51) qp = 51;
            e->qp = (int)lround(qp);
            e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);
            tp_scaleterm = scaleterm;
            tp_have = 1;
        }
    } else if (e->tp_pass == 0 && e->crf_on) {
        /* CRF+VBV rides rcp too (the VBV virtual buffer needs the FIFO): the
 * serial CRF decision on the arrival-captured Cme -- the same signal
 * the serial head feeds it (a no-op for B frames, which inherit the
 * anchor base + cascade, including any prior VBV clip -- the serial
 * compounding). Pure CRF without VBV never sets rcp_on. */
        if (type != 2) e->crf_dc = crf_frame_dc(e, src);
        rc_set_qp_crf(e, C, type);
    }
    /* else pass 1 / CQP: e->qp is the fixed base */
    if (e->vbv_on && type == 2 && !e->rcp_vbv_tight && e->rcp_vbv_cal[2]) {
        /* Extrapolation guard: a pipelined B decides before any of its
 * burst's actuals land, and the bits model under-predicts savagely
 * below its calibrated QP regime (static content: skip-all frames
 * calibrate the scale at high QP; an ABR ladder chasing a full
 * buffer then dives 4 QP per DECIDE -- per frame serially, but per
 * burst-slot here -- and the first actual lands a 40x overshoot).
 * Floor the flying decide at calqp - QPD; the next anchor re-syncs
 * on actuals and the serial equations take it from there. Tight
 * bursts have per-frame actuals (the serial schedule) -- no floor. */
        int lo = (int)lround(e->rcp_vbv_calqp[2]) - vbv_qpd_env();
        if (e->qp < lo) {
            e->qp = lo > 51 ? 51 : lo;
            e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);
        }
    }
    if (e->vbv_on)
        rcp_vbv_clip(e, Cv, type, is_ref);
    /* The coded QP and the model predictions at it, AFTER any VBV clip (for
 * non-VBV modes nothing ran between: identical values to the pre-VBV
 * ordering). */
    en.fqp = frame_qp(e, type, is_ref);
    if (e->abr_on)
        en.pred = abr_scale_used * en.rceq / pow(2.0, (en.fqp - 12) / 6.0);
    else if (en.tp_rec >= 0)
        en.pred = tp_bits_at(e, en.tp_rec, pow(2.0, (en.fqp - 12) / 6.0));
    else if (tp_have)
        en.pred = tp_scaleterm / pow(2.0, (en.fqp - 12) / 6.0);
    if (e->vbv_on) {
        double vs = rcp_vbv_scale_eff(e, type);
        en.vcplx = Cv;
        en.vpred = vs > 0.0 ? vs * Cv / pow(2.0, (en.fqp - 12) / 6.0) : 0.0;
        en.tight = (int8_t)(e->rcp_vbv_tight ? 1 : 0);
    }
    if (rcp_dbg_on())
        fprintf(stderr, "RCPD seq=%u type=%d ref=%d C=%.0f qp=%d np=%d pp=%.0f ca=%.0f s0=%.0f s1=%.0f s2=%.0f\n",
                atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) + 1,
                type, is_ref, C, e->qp, np, pend_pred, e->abr_cum_actual,
                e->abr_scale[0], e->abr_scale[1], e->abr_scale[2]);
    en.base_qp = e->qp;
    en.acc_qp = e->qp;
    if (e->abr_rf2 && type == 0) {
        /* Y264_ABR_RF2_IACC: 0 = credit an I at the P base current at its
 * decide (unbiased: bits at base = wanted at equilibrium whatever the
 * type mix); 1 = at the I's own coded QP (three below), which
 * under-credits intra bits by the I/P ratio and so biases the loop
 * toward spending on cut-heavy content. Default 1 (2026-09-04): +1..4%
 * closer to target and -0.8..-1.6% BD on foreman/bus/stefan/samsung. */
        if (abr_tunable("Y264_ABR_RF2_IACC", 1.0) > 0)
            en.acc_qp = en.fqp;
        else if (!(e->abr_cum_actual <= 0 && e->rcp_n == 0) && e->last_qscale_type[1] > 0)
            en.acc_qp = (int)lround(12.0 + 6.0 * log2(e->last_qscale_type[1]));
    }
    en.seq = atomic_fetch_add_explicit(&e->rcp_seq, 1, memory_order_relaxed) + 1;
    if (type != 2) {
        for (int k = Y264_STAIR_K - 1; k > 0; k--)
            e->rcp_anchor_hist[k] = e->rcp_anchor_hist[k - 1];
        e->rcp_anchor_hist[0] = e->rcp_anchor_seq;  /* the one now displaced */
        e->rcp_anchor_seq = en.seq;
    }
    if (e->rcp_n < RCP_MAX) {                   /* bounded by the launch drain */
        e->rcp[(e->rcp_head + e->rcp_n) % RCP_MAX] = en;
        e->rcp_n++;
    }
    rcp_pop_ready(e);
}

/* The emit-head hook: either a fresh decision, or -- when a bailed parallel
 * prep hands an already-decided frame back to the serial path -- consume the
 * pre-pushed entry and restore its base QP (the sibling's decide may have
 * moved e->qp since). */
static void rcp_head(yah264_encoder_t *e, int type, int is_ref,
                     pixel *const src[3])
{
    if (e->rcp_predecided > 0) {
        int back = e->rcp_n - e->rcp_predecided;
        if (back < 0) {                     /* a push was dropped on a full ring */
            if (rcp_dbg_on()) fprintf(stderr, "RCP predecided entry missing (n=%d pre=%d)\n", e->rcp_n, e->rcp_predecided);
            back = 0;
        }
        int idx = (e->rcp_head + back) % RCP_MAX;
        e->rcp_predecided--;
        e->qp = e->rcp[idx].base_qp;
        e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);
        return;
    }
    rcp_decide(e, type, is_ref, src);
}

/* W2: copy the emit-read decision grids from the shared e-> arrays (just written
 * by analyze) into gen[g], and repoint f's grid pointers there. The trailing
 * emit then reads gen[g] while the next frame's analyze clobbers the shared
 * arrays. Strides are unchanged, so neighbour indexing is identical. */
static void w2_snapshot_grids(yah264_encoder_t *e, y264_frame_t *f, int g)
{
    struct w2_gen *G = &e->gen[g];
    int hmb = e->height_in_mbs, cbh = 4 / e->sub_h;
    size_t mvc = (size_t)e->mv_stride * hmb * 4;
    size_t nmb = (size_t)e->width_in_mbs * hmb;
    memcpy(G->nnz[0], e->nnz[0], (size_t)e->nnz_stride[0] * hmb * 4);
    memcpy(G->nnz[1], e->nnz[1], (size_t)e->nnz_stride[1] * hmb * cbh);
    memcpy(G->nnz[2], e->nnz[2], (size_t)e->nnz_stride[2] * hmb * cbh);
    memcpy(G->i4mode, e->i4mode, (size_t)e->i4mode_stride * hmb * 4);
    memcpy(G->mbcbp, e->mbcbp, nmb * sizeof(int));
    memcpy(G->mvx, e->mvx, mvc * sizeof(int16_t));
    memcpy(G->mvy, e->mvy, mvc * sizeof(int16_t));
    memcpy(G->mvx1, e->mvx1, mvc * sizeof(int16_t));
    memcpy(G->mvy1, e->mvy1, mvc * sizeof(int16_t));
    memcpy(G->refidx, e->refidx, mvc);
    memcpy(G->refidx1, e->refidx1, mvc);
    memcpy(G->mvdx, e->mvdx, mvc * sizeof(int16_t));
    memcpy(G->mvdy, e->mvdy, mvc * sizeof(int16_t));
    memcpy(G->mvdx1, e->mvdx1, mvc * sizeof(int16_t));
    memcpy(G->mvdy1, e->mvdy1, mvc * sizeof(int16_t));
    if (e->mb_tr8 && G->mb_tr8) memcpy(G->mb_tr8, e->mb_tr8, nmb);
    /* Per-MB QP offsets: mb_qp_pre reads these during emit to rebuild cur_qp, and
 * the next frame's aq_analyze / compute_mbtree overwrite the shared arrays. */
    if (f->aq_off && G->aq_off) memcpy(G->aq_off, f->aq_off, nmb);
    if (f->mbtree_off && G->mbtree_off) memcpy(G->mbtree_off, f->mbtree_off, nmb);
    f->nnz[0] = G->nnz[0]; f->nnz[1] = G->nnz[1]; f->nnz[2] = G->nnz[2];
    f->i4mode = G->i4mode; f->mbcbp = G->mbcbp;
    f->mvx = G->mvx; f->mvy = G->mvy; f->mvx1 = G->mvx1; f->mvy1 = G->mvy1;
    f->refidx = G->refidx; f->refidx1 = G->refidx1;
    f->mvdx = G->mvdx; f->mvdy = G->mvdy; f->mvdx1 = G->mvdx1; f->mvdy1 = G->mvdy1;
    if (G->mb_tr8) f->mb_tr8 = G->mb_tr8;
    if (f->aq_off && G->aq_off) f->aq_off = G->aq_off;
    if (f->mbtree_off && G->mbtree_off) f->mbtree_off = G->mbtree_off;
}

/* W2 background task: write the slice_data bitstream from the pending job (on the
 * bg thread) and finalise the CAVLC byte-alignment. Emit never touches recon, the
 * references, or the shared grids, so it is safe to run concurrent with the next
 * frame's analyze. The size is computed by w2_drain after the sync. */
static void w2_emit_task(void *arg)
{
    struct w2_pending *p = arg;
    y264_frame_emit(&p->bs, &p->f, p->job);
    if (!p->cabac)
        y264_bs_rbsp_trailing(&p->bs);
}

/* W2 drain: block until the pending emit finishes, then append its NAL (in coding
 * order) and run the deferred RC accounting. A no-op when nothing is pending.
 * `off` is the DRAINING call's output cursor, not the submitting call's: a
 * pending emit may outlive the encode call that submitted it (deferred-NAL API
 * contract -- a frame's NAL can be returned by a later call, like x264), so the
 * pending must never hold a pointer into a caller's stack. */
static void w2_drain(yah264_encoder_t *e, size_t *off)
{
    struct w2_pending *p = &e->pipe;
    if (!p->active)
        return;
    if (tprof_on()) {                    /* API thread only: the site split is plain state */
        double _w0 = ew_now();
        TPROF(TP_EMITWAIT, ntp_bg_sync(e->bg));
        g_ew[g_ew_site] += ew_now() - _w0; g_ewn[g_ew_site]++;
    } else
        ntp_bg_sync(e->bg);
    p->active = 0;
    if (p->cabac && !p->cb.overflow) cabac_zero_words(&p->f, &p->cb, p->bs.end);
    size_t size = p->cabac ? (p->cb.overflow ? 0 : (size_t)(p->cb.p - p->bs_start))
                           : (p->bs.overflow ? 0 : (size_t)(p->bs.p - p->bs_start));
    if (size == 0) {
        slice_overflow_warn();
        if (e->rcp_on)
            rcp_drop(e);                /* pending entry retired without accounting */
        return;                         /* overflow: drop (mirrors serial ret 0) */
    }
    int r; TPROF(TP_NAL, r = append_nal(e, off, p->ref_idc, p->nal_type, p->rbsp, size));
    if (r < 0) {
        if (e->rcp_on)
            rcp_drop(e);                /* retire the entry, or every later fill lands one frame late */
        return;
    }
    double bits = 8.0 * (double)size;
    if (e->rcp_on) {                    /* lagged: stage the actuals; the schedule pops */
        rcp_fill(e, bits);
        return;
    }
    /* Deferred RC tail. For ABR/VBV/2-pass the drain runs synchronously before the
 * next frame's RC (rc_waits below), so e->rc_cplx / tp_cur_cq / coded QP are
 * still this frame's. CQP/CRF reach here with none of these set. */
    int fqp = frame_qp(e, p->rc_type, p->is_ref);
    if (e->abr_on)
        rc_account(e, bits, fqp, p->rc_type);
    if (e->vbv_on)
        vbv_update(e, bits, fqp);
    if (e->tp_fp)          /* pass 1 and pass 3 both record */
        fprintf(e->tp_fp, "%d %.0f %.0f %d %d\n", p->rc_type, e->rc_cplx, bits,
                fqp, p->is_ref ? 1 : 0);
    if (e->tp_pass >= 2) {
        e->tp_rem_target -= bits;
        e->tp_rem_cq -= e->tp_cur_cq;
        tp_account_plan(e, bits, fqp);
    }
}

/* Drain the W2 pipeline so all coded NALs are appended and RC accounted. Called
 * at every point where the caller reads the output cursor / NAL count. */
static void w2_flush(yah264_encoder_t *e, size_t *off) { if (e->w2_on) w2_drain(e, off); }

/* W2 pipelined emit_frame: analyze frame N (recon ready), submit its entropy emit
 * to the bg thread, and let it trail into frame N+1's analyze. ABR/VBV/2-pass drain
 * the previous frame before setting this one's QP (they need its coded bits);
 * CQP/CRF overlap. NAL append + RC accounting are deferred to w2_drain. */
/* Record one emitted frame's display index for yah264_encoder_frame_order.
 * Called wherever a frame is finalised, i.e. exactly where recon_cb fires, so
 * the two can never report different sets of frames. */
static void y264_note_emit(yah264_encoder_t *e, int disp)
{
    if (e->emit_count < (int)(sizeof e->emit_disp / sizeof e->emit_disp[0]))
        e->emit_disp[e->emit_count++] = disp;
}

static int emit_frame_w2(yah264_encoder_t *e, size_t *off, int type, int is_idr,
                         int is_ref, pixel *const src[3])
{
    /* Y264_RCP_NODRAIN (default 1): with a lag budget the decide runs on
 * in-flight predictions and the ring's pop bound retires actuals in order,
 * so the anchor-side drain of the pending emit buys the decide nothing and
 * costs the emit its overlap: ducks 720p ABR t12 read emit_sync_wait 52 ms
 * (CRF 3), riverbed 130 ms (CRF 5.5), all pool-empty (2026-09-04). At zero
 * lag, in the warm and under VBV the drain stays. =0 restores it for A/B. */
    int rc_waits = (e->abr_on || e->vbv_on || e->tp_pass)
                 && (!e->rcp_on || rcp_warm(e)
                     || (type != 2 && !(rcp_nodrain_on() && e->rcp_lag > 0 && !e->vbv_on))
                     || (e->vbv_on && e->rcp_vbv_tight));
    if (rc_waits) {
        g_ew_site = 0;
        w2_drain(e, off);               /* prev frame's bits feed this frame's RC */
    }
    if (e->rcp_on) {
        rcp_head(e, type, is_ref, src); /* lagged decide; no drain forced */
    } else if (e->abr_on || e->crf_on || e->vbv_on || e->tp_pass) {
        double C = frame_complexity(e, src, type == 0);
        e->rc_cplx = C;
        double Ccrf = (e->crf_on || getenv("Y264_DBG_CPLX")) ? frame_complexity_me(e) : 0.0;
        if (getenv("Y264_DBG_CPLX"))
            fprintf(stderr, "CPLX type=%d C=%.0f permb=%.1f  Cme=%.0f permb=%.1f\n", type,
                    C, C / (e->width_in_mbs * e->height_in_mbs),
                    Ccrf, Ccrf / (e->width_in_mbs * e->height_in_mbs));
        if (e->abr_on)      rc_set_qp(e, C, type);
        else if (e->crf_on) { if (type != 2) e->crf_dc = crf_frame_dc(e, src); rc_set_qp_crf(e, Ccrf, type); }
        else if (e->tp_pass >= 2) rc_set_qp_2pass(e);
        if (e->vbv_on)      vbv_clip_qp(e, C, type, is_ref);
    }

    int g = e->w2_gen;
    struct w2_gen *G = &e->gen[g];
    struct w2_pending *p = &e->pipe;
    int cabac = e->pps.entropy_coding_mode_flag ? 1 : 0;
    y264_bs_t bs; y264_frame_t f; int fqp, deblock;
    y264_cabac_t cb;                    /* analyze's own engine, NOT the pending's */
    struct frame_work fw; fw_default(e, &fw);
    TPROF(TP_PREP, build_slice_prep(e, type, is_idr, is_ref, src, G->rbsp, e->rbsp_cap,
                     &fw, &bs, &f, &fqp, &deblock));
    if (cabac) {
        while (y264_bs_pos_bits(&bs) & 7)
            y264_bs_write1(&bs, 1);
        y264_cabac_init_engine(&cb, bs.p); y264_cabac_set_end(&cb, bs.end - 64);
        y264_cabac_init_contexts(&cb, type, 0, fqp);
        f.cabac = &cb;                  /* analyze uses the stack engine; emit gets a copy */
    }
    y264_emit_job_t *job;
    TPROF(TP_ANALYZE, job = y264_frame_analyze(&f));
    qp_trace_frame(e, &f, type);
    if (deblock)
        TPROF(TP_DEBLOCK, y264_deblock_frame(&f));   /* recon final before next frame reads it */
    ident_stat(&f, type);

    /* recon-phase tail (needs e->rec, valid until the next analyze). */
    TPROF(TP_BORDERS, extend_borders(e, e->rec));
    for (int c = 0; c < 3; c++) e->rec_out[c] = e->rec[c];
    y264_note_emit(e, e->cur_disp);
    if (e->recon_cb) {
        yah264_picture_t rp;
        rp.csp = e->param.csp; rp.width = e->width; rp.height = e->height; rp.pts = 0;
        for (int c = 0; c < 3; c++) { rp.plane[c] = e->rec[c]; rp.stride[c] = e->pstride[c]; }
        e->recon_cb(e->recon_ud, &rp, e->cur_disp, Y264_BIT_DEPTH);
    }

    w2_snapshot_grids(e, &f, g);        /* freeze the emit-read grids into gen[g] */
    if (!rc_waits) {
        g_ew_site = 1;
        w2_drain(e, off);               /* retire the prev emit (frees the pending) before we reuse it */
    }

    /* Publish this frame as the pending emit and hand it to the bg thread. The
 * engine is COPIED into the pending: analyze's cb is a stack local, so the
 * next frame's analyze cannot touch the engine this emit is using. */
    p->cabac = cabac;
    p->cb = cb;
    y264_cabac_rebind(&p->cb);          /* the copy aliased analyze's stack buffer */
    p->job = job;
    p->f = f;                           /* grids already repointed at gen[g] */
    p->f.cabac = &p->cb;                /* emit reads the pending's own engine copy */
    p->bs = bs;
    p->bs_start = bs.start;
    p->geni = g;
    p->rbsp = G->rbsp;
    p->nal_type = is_idr ? YAH264_NAL_SLICE_IDR : YAH264_NAL_SLICE;
    p->ref_idc = is_idr ? YAH264_NAL_PRIORITY_HIGH : (is_ref ? 2 : 0);
    p->rc_type = type;
    p->is_ref = is_ref;
    p->active = 1;
    e->w2_gen ^= 1;
    ntp_bg_submit(e->bg, w2_emit_task, p);
    /* Under the ring the next decide drains this emit itself (the pre-decide
 * drain above), so waiting here only forfeits the overlap: 12 warm anchors
 * x one 1080p CABAC emit = 84 ms on riverbed ABR t12 (2026-09-04). */
    if (rc_waits && !(e->rcp_on && rcp_nodrain_on())) {
        g_ew_site = 2;
        w2_drain(e, off);               /* serialise: RC tail needs this frame's bits now */
    }
    return 0;
}

/* Is this the frame that gets bounded against its own MEASURED size?
 *
 * Only the first coded frame of the encoder instance, and only where the
 * predictive clamp is the sole line of defence: VBV with no bitrate target and
 * no pass-1 record, i.e. capped VBR and CQP+VBV. ABR/CBR keep their integrator
 * and 2-pass has real bits for frame 0, so both are left exactly as they were
 * and stay bit-for-bit identical.
 *
 * WHY ONLY THE FIRST. Not because later frames never need it -- one of them
 * demonstrably does (the scene-cut residue). Because the first frame is the only one
 * that reaches this function at every thread count. Instrumented on samsung at
 * 18 threads, 28 of 180 frames arrive at emit_frame; the rest are coded by the
 * stair/fpipe pipelined routes, which stash their own NALs. Bounding here alone
 * would therefore make the output depend on how many threads coded it, and
 * thread-invariant output is not something to trade for a compliance cell. The
 * instance's first frame is always the serial anchor -- no burst can be in
 * flight before one exists -- so restricted this way the bound is reached
 * identically at 1 thread and at 18. */
static int vbv_first_bounded(const yah264_encoder_t *e)
{
    return e->vbv_on && e->vbv_first_bound && !e->abr_on && !e->tp_pass;
}

/* Y264_VBV_BOUND: the same bound on EVERY frame rather than only the first.
 *
 * WHY THIS COSTS THE STAIR, and why that is a property of the defect and not a
 * shortcut. A retry needs a window in which the coded size is known and nothing
 * downstream has consumed the frame yet. emit_frame has one: build_slice, then
 * append_nal. The stair's burst anchor does not, and the reason is the stair's
 * whole point -- its B leaves are ROW-GATED on the anchor's publish watermark
 * (stair_chain, the pub_wait composition argument), so they are analyzed
 * against the anchor's recon while the anchor is still being coded. By the time
 * B->size is final in stair_drain, leaves have already been coded against the
 * recon a retry would change. There is no window; re-coding the anchor would
 * mean rewinding a whole burst's DPB stores, refring pushes, colmv application
 * and ledger entries.
 *
 * So a bound with full reach and a pipelined anchor are mutually exclusive
 * given this schedule, and the honest move is to take the one that is sound.
 * With the stair disengaged every frame reaches emit_frame's serial body, one
 * route, one window, and the bound applies to a frame regardless of which
 * thread count coded it -- no frame is bounded on one path and unbounded on
 * another. Measured cost, capped VBR only, 18 threads, median of 5:
 * samsung 1.15x, park_joy 1.25x, ducks 1.28x.
 *
 * fpipe goes with it. A leaf B is non-reference, so its recon feeds nothing and
 * a retry there IS structurally local -- but with the stair off it measured no
 * faster than the serial path (park_joy 1.69s vs 1.75s, inside the noise), and
 * keeping it would need an indexed rcp_reqp because both sibling leaves push
 * ledger entries before either NAL is appended. Not worth the surface. */
static int vbv_bound_all(const yah264_encoder_t *e)
{
    return e->vbv_on && e->vbv_bound_on && !e->abr_on && !e->tp_pass;
}

/* Either bound wants the serial emit: W2 does not know a frame's size until
 * w2_drain, by which point the NAL is appended and the RC has consumed it. */
static int vbv_bounded_now(const yah264_encoder_t *e)
{
    return vbv_first_bounded(e) || vbv_bound_all(e);
}

static int emit_frame(yah264_encoder_t *e, size_t *off, int type, int is_idr,
                      int is_ref, pixel *const src[3])
{
    /* The bounded frame takes the serial path even under W2. The bound needs the
 * coded size BEFORE anything is appended or accounted, and W2 does not know
 * the size until w2_drain -- by which point the NAL is in the output buffer
 * and the RC has already consumed it, so bounding there would mean undoing
 * both, including a ledger pop that may have retired several entries.
 *
 * It is the instance's first frame, so nothing is in flight to overlap with
 * and no pipelining is lost. build_slice and the W2 emit produce identical
 * bytes (scripts/w2_canary.sh), so which one codes it is a scheduling choice
 * and not an output one. */
    if (e->w2_on && !vbv_bounded_now(e))
        return emit_frame_w2(e, off, type, is_idr, is_ref, src);
    if (e->rcp_on) {
        /* Serial path under RC_PIPE: the SAME lagged schedule (decide with the
 * previous frames pending, account at the next anchor's decide), so
 * output matches the engaged path at any thread count. */
        rcp_head(e, type, is_ref, src);
    } else if (e->abr_on || e->crf_on || e->vbv_on || e->tp_pass) {
        double C = frame_complexity(e, src, type == 0);
        e->rc_cplx = C;
        /* CRF uses the ME-compensated lowres cost ; ABR/VBV/2-pass
 * keep the zero-motion metric they self-calibrate their bits model on. */
        double Ccrf = (e->crf_on || getenv("Y264_DBG_CPLX")) ? frame_complexity_me(e) : 0.0;
        if (getenv("Y264_DBG_CPLX"))
            fprintf(stderr, "CPLX type=%d C=%.0f permb=%.1f  Cme=%.0f permb=%.1f\n", type,
                    C, C / (e->width_in_mbs * e->height_in_mbs),
                    Ccrf, Ccrf / (e->width_in_mbs * e->height_in_mbs));
        if (e->abr_on)
            rc_set_qp(e, C, type);
        else if (e->crf_on) {
            if (type != 2) e->crf_dc = crf_frame_dc(e, src);
            rc_set_qp_crf(e, Ccrf, type);
        }
        else if (e->tp_pass >= 2)
            rc_set_qp_2pass(e);
        if (e->vbv_on)
            vbv_clip_qp(e, C, type, is_ref);
    }
    size_t rbsp_size = build_slice(e, type, is_idr, is_ref, src);
    if (rbsp_size == 0) { slice_overflow_warn(); }
    if (rbsp_size == 0)
        return -1;
    /* --- bounded against the MEASURED size ---------------------------------
 *
 * vbv_clip_qp prices a PREDICTION, and the prediction has no bound worth
 * trusting -- actual/predicted is p50 ~0.85, p90 ~2, p99.9 in the hundreds
 * of percent. Everything upstream of here
 * has already been built to soften that tail and none of it can close it,
 * because a scene cut after a static run is a frame whose bits are simply
 * not a function of anything observable before it is coded.
 *
 * So this does not predict. It codes the frame, reads the size off the
 * bitstream, and if the buffer cannot hold what came out, raises the QP by
 * exactly the measured shortfall and codes it again. log2(measured/limit) is
 * the step that would have made THIS frame fit; if the 1/qscale law is off,
 * the next pass measures again and corrects.
 *
 * A bound, not a seed. Seeding the bits model with a calibrated constant is
 * the cheaper fix for the same symptom and it is the wrong one, for the
 * reason the previous VBV round refused a fixed overshoot multiplier: an
 * error tail with p99.9 in the hundreds of percent has no honest constant in
 * it. A seed passes the corpus it was tuned on and breaks on the first
 * adversarial IDR outside it. Nothing here is tuned -- the limit is the same
 * ceiling every other frame answers to, and the QP step is read off the
 * frame that was actually coded.
 *
 * No rollback is needed because nothing has happened yet: build_slice writes
 * into e->rbsp and e->rec and mutates three scalars in e that are pure
 * functions of (type, refs), and this runs before append_nal, before
 * vbv_update and before rcp_fill. Under rcp the decide already pushed a
 * ledger entry, so that gets re-priced -- see rcp_reqp. That pair, the NAL
 * and the ledger, is what the write-up flagged a retry has to reach.
 *
 * Deterministic: the trigger is the coded size, which is a function of the
 * decided QP and the content and of nothing else. Same frames re-encode on
 * every run and at every thread count.
 *
 * Four attempts, then ship what we have. The loop is monotone (each pass
 * raises the QP by at least 1 and stops at 51), so this is a cap on wasted
 * work, not a correctness knob: a frame still over the limit at QP 51 is one
 * the buffer simply cannot hold, and there is no quantiser that fixes it. */
    if (vbv_bounded_now(e)) {
        /* The occupancy this frame is removed FROM. It is the instance's first
 * frame, so under the pipelined path the ledger holds only this frame's
 * own entry and the virtual fill equals the committed one -- the walk is
 * here so the two paths read the same quantity by construction rather
 * than by coincidence. */
        double fill = e->rcp_on ? rcp_vbv_vfill(e, 1) : e->vbv_fill;
        double limit = vbv_limit_at(e, fill);
        e->vbv_first_bound = 0;         /* the one-shot half, spent */
        for (int k = 0; k < 4 && limit > 0.0; k++) {
            double bits = 8.0 * (double)rbsp_size;
            if (bits <= limit)
                break;
            int bump = (int)ceil(6.0 * log2(bits / limit));
            if (bump < 1) bump = 1;
            int nq = e->qp + bump;
            if (nq > 51) nq = 51;
            if (nq == e->qp)
                break;                  /* already at the ceiling */
            e->qp = nq;
            e->chroma_qp = y264_chroma_qp(nq, e->param.chroma_qp_index_offset);
            if (e->rcp_on)
                rcp_reqp(e, type, is_ref);
            size_t sz = build_slice(e, type, is_idr, is_ref, src);
            if (sz == 0)
                break;                  /* keep the last good slice */
            rbsp_size = sz;
        }
    }
    /* The recon may serve as a reference (copied into the ring / DPB / ref1):
 * replicate its edges outward so out-of-frame MC reads the borders. */
    TPROF(TP_BORDERS, extend_borders(e, e->rec));
    for (int c = 0; c < 3; c++) e->rec_out[c] = e->rec[c];
    int nal_type = is_idr ? YAH264_NAL_SLICE_IDR : YAH264_NAL_SLICE;
    int ref_idc = is_idr ? YAH264_NAL_PRIORITY_HIGH : (is_ref ? 2 : 0);
    int r; TPROF(TP_NAL, r = append_nal(e, off, ref_idc, nal_type, e->rbsp, rbsp_size));
    if (r >= 0)
        y264_note_emit(e, e->cur_disp);
    if (r >= 0 && e->recon_cb) {
        yah264_picture_t rp;
        rp.csp = e->param.csp;
        rp.width = e->width;
        rp.height = e->height;
        rp.pts = 0;
        for (int c = 0; c < 3; c++) {
            rp.plane[c] = e->rec[c];
            rp.stride[c] = e->pstride[c];
        }
        e->recon_cb(e->recon_ud, &rp, e->cur_disp, Y264_BIT_DEPTH);
    }
    if (r >= 0 && e->rcp_on) {
        rcp_fill(e, 8.0 * (double)rbsp_size);   /* lagged: schedule pops later */
        return r;
    }
    /* Y264_RC_TRACE: one line per coded frame -- base QP, the type-cascaded
 * coded QP, the mb-tree mean offset and the bits. The comparable x264
 * quantities are the rate controller's mean QP (base) and the AQ-modulated
 * mean minus it (mean
 * offset), so an operating-point divergence is readable side by side. */
    if (r >= 0 && getenv("Y264_RC_TRACE"))
        fprintf(stderr, "rct poc=%d type=%d ref=%d base=%d coded=%d moff=%.3f bits=%.0f\n",
                e->poc, type, is_ref, e->qp, frame_qp(e, type, is_ref),
                e->mbtree_mean_off, 8.0 * (double)rbsp_size);
    if (r >= 0 && e->abr_on)
        rc_account(e, 8.0 * (double)rbsp_size, frame_qp(e, type, is_ref), type);
    if (r >= 0 && e->vbv_on)
        vbv_update(e, 8.0 * (double)rbsp_size, frame_qp(e, type, is_ref));
    if (r >= 0 && e->tp_fp)
        fprintf(e->tp_fp, "%d %.0f %.0f %d %d\n", type, e->rc_cplx,
                8.0 * (double)rbsp_size, frame_qp(e, type, is_ref), is_ref ? 1 : 0);
    if (r >= 0 && e->tp_pass >= 2) {
        e->tp_rem_target -= 8.0 * (double)rbsp_size;   /* spend what this frame used */
        e->tp_rem_cq -= e->tp_cur_cq;
        tp_account_plan(e, 8.0 * (double)rbsp_size, frame_qp(e, type, is_ref));
    }
    return r;
}

/* Y264_PAD_ROWCOPY=1: copy the real columns row by row instead of the whole
 * bordered buffer. Byte-identical at any pad -- the skipped columns are the
 * Y264_PLANE_PAD tail, which nothing reads -- and worth nothing at the default
 * pad 0, where the two are the same bytes and the flat memcpy is faster.
 *
 * It exists because the pad probe moves more than layout: a whole-buffer copy
 * is sized by pstride, so PAD=4096 quadruples every plane copy's BYTES, i.e.
 * the probe moves work volume too. This gate takes that term out so the two
 * can be read apart. */
static void copy_planes(const yah264_encoder_t *e, pixel *dst[3],
                        pixel *const src[3])
{
    /* Interior pointers in, full bordered buffers copied: the borders travel
 * with the pixels, so a plane extended once stays extended through every
 * copy into the reference ring / DPB / list buffers. */
    size_t ol = (size_t)Y264_LUMA_BORDER * e->pstride[0] + Y264_LUMA_BORDER;
    size_t oc = (size_t)Y264_CHROMA_BORDER * e->pstride[1] + Y264_CHROMA_BORDER;
    size_t szl = (size_t)e->pstride[0] * (e->padded_h + 2 * Y264_LUMA_BORDER);
    size_t szc = (size_t)e->pstride[1] * (e->padded_h / e->sub_h + 2 * Y264_CHROMA_BORDER);
    static int rowcopy = -1;
    if (rowcopy < 0) { const char *s = getenv("Y264_PAD_ROWCOPY"); rowcopy = s && atoi(s); }
    if (rowcopy) {
        int rl = e->padded_h + 2 * Y264_LUMA_BORDER;
        int rc = e->padded_h / e->sub_h + 2 * Y264_CHROMA_BORDER;
        size_t wl = (size_t)e->padded_w + 2 * Y264_LUMA_BORDER;
        size_t wc = (size_t)e->padded_w / e->sub_w + 2 * Y264_CHROMA_BORDER;
        for (int y = 0; y < rl; y++)
            memcpy(dst[0] - ol + (size_t)y * e->pstride[0],
                   src[0] - ol + (size_t)y * e->pstride[0], wl * sizeof(pixel));
        for (int c = 1; c < 3; c++)
            for (int y = 0; y < rc; y++)
                memcpy(dst[c] - oc + (size_t)y * e->pstride[1],
                       src[c] - oc + (size_t)y * e->pstride[1], wc * sizeof(pixel));
        return;
    }
    memcpy(dst[0] - ol, src[0] - ol, szl * sizeof(pixel));
    memcpy(dst[1] - oc, src[1] - oc, szc * sizeof(pixel));
    memcpy(dst[2] - oc, src[2] - oc, szc * sizeof(pixel));
}

/* Push the anchor recon into the multi-ref ring as the most-recent list-0
 * reference, by pointer: ref1's buffer (holding the anchor recon since the
 * post-emit swap) is donated to the ring front, the oldest slot's buffer
 * becomes the new ref1 scratch, and e->ref realiases the new front. No pixel
 * copy -- content and borders travel with the pointers. */
static void refring_push(yah264_encoder_t *e, int poc)
{
    pixel *tmp[3] = { e->refring[e->nref - 1][0], e->refring[e->nref - 1][1],
                        e->refring[e->nref - 1][2] };
    for (int i = e->nref - 1; i > 0; i--) {
        e->refring[i][0] = e->refring[i - 1][0];
        e->refring[i][1] = e->refring[i - 1][1];
        e->refring[i][2] = e->refring[i - 1][2];
        e->refring_fn[i] = e->refring_fn[i - 1];
        e->refring_poc[i] = e->refring_poc[i - 1];
    }
    for (int c = 0; c < 3; c++) {
        e->refring[0][c] = e->ref1[c];
        e->ref1[c] = tmp[c];
    }
    if (e->flat_hp_on) {                /* hpel triples rotate with the buffers */
        pixel *hptmp[3] = { e->ring_hp[e->nref - 1][0], e->ring_hp[e->nref - 1][1],
                            e->ring_hp[e->nref - 1][2] };
        int hvtmp = e->ring_hpv[e->nref - 1];
        for (int i = e->nref - 1; i > 0; i--) {
            for (int c = 0; c < 3; c++) e->ring_hp[i][c] = e->ring_hp[i - 1][c];
            e->ring_hpv[i] = e->ring_hpv[i - 1];
        }
        for (int c = 0; c < 3; c++) {
            e->ring_hp[0][c] = e->ref1_hp[c];
            e->ref1_hp[c] = hptmp[c];
        }
        e->ring_hpv[0] = e->ref1_hpv;
        e->ref1_hpv = hvtmp;
    }
    e->refring_fn[0] = e->frame_num;
    e->refring_poc[0] = poc;
    e->ref[0] = e->refring[0][0]; e->ref[1] = e->refring[0][1]; e->ref[2] = e->refring[0][2];
    if (e->nref_valid < e->nref) e->nref_valid++;
}

/* Encode all buffered B pictures as non-reference P frames (used when no future
 * anchor is available: at a GOP boundary or at end of stream). */
static int flush_buffered_p(yah264_encoder_t *e, size_t *off)
{
    if (e->b_pyramid) {
        /* Tail B's with no future anchor: code as non-reference P against the
 * nearest past reference, reusing the last reference's FrameNum. Pin the
 * chosen reference since the default P list orders by FrameNum. */
        for (int i = 0; i < e->nbuf; i++) {
            int slot = -1;
            for (int j = 0; j < e->dpb_size; j++)
                if (e->dpb[j].used && e->dpb[j].poc < e->bpoc[i] &&
                    (slot < 0 || e->dpb[j].poc > e->dpb[slot].poc))
                    slot = j;
            if (slot < 0) return -1;
            for (int c = 0; c < 3; c++) e->ref[c] = e->dpb[slot].plane[c];
            e->ref0_poc = e->dpb[slot].poc;
            e->cur_ref_l0_fn = e->dpb[slot].frame_num;
            e->frame_num = e->last_ref_fn;          /* non-ref: last reference's FrameNum */
            e->poc = e->bpoc[i];
            e->cur_disp = e->bdisp[i];
            e->cur_lr_motion = e->bmotion[i]; e->cur_lr_tdiff = e->btdiff[i];
            e->rcp_cur_cme = e->rcp_bcplx[i];
            e->rcp_cur_cvi = e->rcp_bcvi[i];
            if (emit_frame(e, off, 1, 0, 0, e->bplane[i]) < 0)
                return -1;
        }
        e->nbuf = 0;
        return 0;
    }
    for (int i = 0; i < e->nbuf; i++) {
        e->frame_num = e->anchor_fn;
        e->poc = e->bpoc[i];
        e->cur_disp = e->bdisp[i];
        e->cur_lr_motion = e->bmotion[i]; e->cur_lr_tdiff = e->btdiff[i];
        e->rcp_cur_cme = e->rcp_bcplx[i];
        e->rcp_cur_cvi = e->rcp_bcvi[i];
        if (emit_frame(e, off, 1, 0, 0, e->bplane[i]) < 0)
            return -1;
    }
    e->nbuf = 0;
    return 0;
}

/* --- b-pyramid decoded picture buffer --- */

static void dpb_reset(yah264_encoder_t *e)
{
    for (int i = 0; i < e->dpb_size; i++) e->dpb[i].used = 0;
    e->next_frame_num = 0;
}

/* Reset the co-located motion grid to "no temporal reference": an IDR breaks the
 * prediction chain, so the previous GOP's motion field must not seed the first
 * P frame's temporal MV search (scale_col_mv). Without this the grid held stale
 * (post-IDR) or uninitialised (stream-start) values whose colpoc could pass the
 * >=0 guard, feeding nondeterministic ME seeds -> non-reproducible bitstreams. */
static void col_reset(yah264_encoder_t *e)
{
    size_t mc = (size_t)e->mv_stride * e->height_in_mbs * 4;
    for (size_t i = 0; i < mc; i++) {
        e->colmvx[i] = 0; e->colmvy[i] = 0;
        e->colref[i] = -1; e->colpoc[i] = -1;
    }
    e->colframepoc = 0;
    e->col_l0poc0 = -1;
}

/* Store the just-coded reference (e->rec + e->mvx/mvy/refidx) into a DPB slot,
 * applying the sliding-window: if the DPB is full, evict the reference with the
 * smallest FrameNum (mirrors the decoder, 8.2.5.3). Assigns the running FrameNum. */
static void dpb_store(yah264_encoder_t *e, int poc, size_t mvcount)
{
    int used = 0;
    for (int i = 0; i < e->dpb_size; i++) used += e->dpb[i].used;
    if (used >= e->sps.max_num_ref_frames) {
        int victim = -1, minfn = 0;
        for (int i = 0; i < e->dpb_size; i++)
            if (e->dpb[i].used && (victim < 0 || fn_wrap(e, e->dpb[i].frame_num) < minfn)) {
                victim = i; minfn = fn_wrap(e, e->dpb[i].frame_num);
            }
        if (victim >= 0) e->dpb[victim].used = 0;
    }
    int slot = -1;
    for (int i = 0; i < e->dpb_size; i++) if (!e->dpb[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;                         /* should not happen */
    struct dpb_entry *d = &e->dpb[slot];
    /* Rotate the recon INTO the slot by pointer: the slot's (evicted or free)
 * buffer becomes the next frame's rec, fully rewritten before any read.
 * Borders were extended in emit_frame, so they travel with the buffer.
 * rec_out keeps pointing at the coded frame's pixels (now d->plane). */
    for (int c = 0; c < 3; c++) {
        pixel *t = d->plane[c]; d->plane[c] = e->rec[c]; e->rec[c] = t;
    }
    /* Build this reference's half-pel planes ONCE, here, instead of rebuilding
 * them in every future frame's build_slice_prep that references this picture.
 * Same source (the just-stored recon d->plane[0], borders extended in
 * emit_frame) => byte-identical to the per-frame build. Scratch is per-GOP-
 * worker (own encoder) and used sequentially, so no race. */
    d->hpel_valid = 0;
    if (e->hpel_on && d->hpel[0]) {
        hpel_build_ref(e, d->hpel[0], d->hpel[1], d->hpel[2], d->plane[0]);
        d->hpel_valid = 1;
    }
    /* Store the resolved co-located motion (8.4.1.2.1): MvL0/RefIdxL0 when this
 * picture used list 0, else MvL1/RefIdxL1 (a B reference may be list-1 only).
 * For a P reference list 1 is empty, so this is just its list-0 motion. */
    for (size_t i = 0; i < mvcount; i++) {
        if (e->refidx[i] >= 0) {
            d->mvx[i] = e->mvx[i]; d->mvy[i] = e->mvy[i]; d->refidx[i] = e->refidx[i];
            d->colpoc[i] = (int16_t)(e->refidx[i] < e->cur_l0n
                                     ? e->cur_l0poc[e->refidx[i]] : -1);
        } else if (e->refidx1[i] >= 0) {
            d->mvx[i] = e->mvx1[i]; d->mvy[i] = e->mvy1[i]; d->refidx[i] = e->refidx1[i];
            d->colpoc[i] = (int16_t)(e->cur_l1poc0 >= 0 ? (e->cur_l1poc0 | Y264_COLPOC_L1) : -1);
        } else {
            /* Intra colocated block: no motion. Write MV 0 (8.4.1.2.1 treats an
 * intra colocated block as refIdx 0 / zero MV) rather than leaving the
 * slot's stale contents. These entries persist across GOPs and reach a
 * P frame's temporal ME seed (scale_col_mv) via the col grid where
 * colpoc/colmvx can be inconsistent, so uninitialised garbage here
 * leaked nondeterministic seeds -> non-reproducible bitstreams. */
            d->mvx[i] = 0; d->mvy[i] = 0;
            d->refidx[i] = -1;
            d->colpoc[i] = -1;
        }
    }
    d->col_l0poc0 = e->cur_l0n > 0 ? e->cur_l0poc[0] : -1;
    d->poc = poc;
    d->frame_num = e->next_frame_num;
    d->used = 1;
    e->last_ref_fn = e->next_frame_num;
    int maxfn = 1 << (e->sps.log2_max_frame_num_minus4 + 4);
    e->next_frame_num = (e->next_frame_num + 1) % maxfn;
}

/* Nearest reference in the DPB with POC below (list 0) / above (list 1) `poc`. */
static int dpb_find(yah264_encoder_t *e, int poc, int future)
{
    int best = -1;
    for (int i = 0; i < e->dpb_size; i++) {
        if (!e->dpb[i].used) continue;
        int p = e->dpb[i].poc;
        if (future ? (p > poc) : (p < poc)) {
            if (best < 0 ||
                (future ? (p < e->dpb[best].poc) : (p > e->dpb[best].poc)))
                best = i;
        }
    }
    return best;
}

/* Copy the DPB list-0/list-1 reference planes into e->ref / e->ref1 and load the
 * list-1 co-located motion. Used for a B frame at `poc`; returns the slot indices. */
static void set_b_refs(yah264_encoder_t *e, int poc, size_t mvcount, int *l0, int *l1)
{
    *l0 = dpb_find(e, poc, 0);
    *l1 = dpb_find(e, poc, 1);
    if (*l0 < 0 || *l1 < 0) {              /* DPB/decoder divergence: never index slot -1 */
        int any = -1;
        for (int i = 0; i < e->dpb_size; i++) if (e->dpb[i].used) { any = i; break; }
        fprintf(stderr, "yah264: internal: B reference missing (l0 %d l1 %d); using slot %d\n", *l0, *l1, any);
        if (any < 0) any = 0;
        if (*l0 < 0) *l0 = any;
        if (*l1 < 0) *l1 = any;
    }
    /* Both lists are read straight out of the DPB (no copy). ME resolves list 0
 * through build_list0 (DPB pointers) anyway; e->ref only feeds the RC
 * complexity probe and the I-slice frame wiring, and it is re-aimed before
 * every emit, so aliasing the slot is safe and byte-identical. */
    for (int c = 0; c < 3; c++) e->ref[c] = e->dpb[*l0].plane[c];
    for (int c = 0; c < 3; c++) e->cur_l1p[c] = e->dpb[*l1].plane[c];
    e->ref0_poc = e->dpb[*l0].poc;
    e->ref1_poc = e->dpb[*l1].poc;
    e->colframepoc = e->dpb[*l1].poc;
    e->col_l0poc0 = e->dpb[*l1].col_l0poc0;
    memcpy(e->colmvx, e->dpb[*l1].mvx, mvcount * sizeof(int16_t));
    memcpy(e->colmvy, e->dpb[*l1].mvy, mvcount * sizeof(int16_t));
    memcpy(e->colref, e->dpb[*l1].refidx, mvcount);
    memcpy(e->colpoc, e->dpb[*l1].colpoc, mvcount * sizeof(int16_t));
}

/* --- MT Lever 2 (Y264_FPIPE): non-reference B leaves in parallel ---
 *
 * In the b-pyramid the is_ref==0 leaves of a mini-GOP are mutually independent:
 * they read frozen DPB references and frozen colocated grids, never dpb_store,
 * and write only their own bitstream -- so the two sibling leaves under a
 * bframes-3 node can encode concurrently and stay BYTE-IDENTICAL (same inputs,
 * outputs concatenated in coding order).
 *
 * Shape: everything race-sensitive is done SERIALLY per leaf in coding order
 * (RC head, set_b_refs, slice header, QP capture -- fpipe_prep_leaf), so the
 * shared e-> scalars see the exact serial write sequence; only analyze + emit +
 * deblock run concurrently, on fully private per-leaf state (rec, decision
 * grids, colmv snapshot, cabac engine, rbsp). Both leaves' analyzes register
 * as JOBS on the ONE shared pool (the v2 multi-frame wavefront): each worker
 * claims ready rows from the oldest job that has one, so the pair shares the
 * full worker set instead of splitting it into fixed sub-pools (the v1 shape).
 * Wavefront output is thread-count- and schedule-invariant, so none of this
 * changes bits. Leaves bypass W2 (their entropy emit runs inside their own
 * task); the parent's trailing W2 emit is drained before the leaf NALs are
 * appended so coding order holds. */
/* One consumer's staircase gate set: the still-streaming pictures its clamped
 * reads touch. A B leaf can reference the anchor in one list and the burst's
 * reference B in the other, so a row must clear BOTH watermarks. (Defined here
 * because a leaf carries one; see the staircase section for stair_prog.) */
struct stair_prog;
/* rbidx names the entry that is a v6 REFERENCE-B watermark rather than an
 * anchor's, so the claim gate can report which of the two closed a row. Purely
 * diagnostic (Y264_STAIR_STAT); -1 = none. */
struct stair_gate { struct stair_prog *p[2]; int n; int rbidx; };

struct fpipe_leaf {
    struct w2_gen   g;              /* private decision grids + rbsp (W2 shape) */
    uint8_t        *mbqp;           /* private per-MB coded QP (deblock input) */
    pixel          *rec[3];         /* private working recon */
    int16_t        *colmvx, *colmvy;    /* frozen colocated field snapshot */
    int8_t         *colref;
    int16_t        *colpoc;
    int             col_l0poc0;         /* the snapshot picture's own list-0[0] POC (Y264_TDIR_LEGAL) */
    y264_hpel_ref_t hpel_ctx[17];   /* private half-pel registry (points at DPB) */
    int16_t        *bseed_cur[4];   /* private POC-scaled B-seed grids */
    ntp_pool_t     *pool;           /* the shared pool this leaf's jobs run on */
    /* per-dispatch state */
    y264_frame_t    f;
    y264_bs_t       bs;
    y264_cabac_t    cb;
    int             cabac, dblk, fqp, disp;
    size_t          size;           /* RBSP size, filled by the task (0 = overflow) */
    /* staircase reference-B commit: the POC set captured at this leaf's prep
 * (dpb_store reads e->cur_l0poc, which later preps overwrite). */
    int             l0poc[16], l0n, l1poc0;
    y264_emit_job_t *job;           /* staircase: analyze job awaiting its emit */
    int              ref_idc;       /* staircase: pending-NAL ref_idc */
    struct stair_gate gate;         /* still-streaming pictures this B's rows
 * wait on (anchor, and under BDEPTH the
 * burst reference B) */
    /* v4 staircase: a reference-B's DPB BOOKKEEPING runs at prep time (so
 * serial_done can fire before any B encode); the slot is remembered here
 * and its CONTENT (hpel + colmv) commits after the analyze. NULL = the
 * sync flavour commits both at the run site, as before. */
    struct dpb_entry *commit_slot;
    /* and the buffers that slot lent it, captured there. The content lands a
 * whole encode later, by which time the slot may have been recycled onto a
 * newer picture -- writing through the slot would then overwrite that one's
 * half-pel and colocated motion. Correlated 20/20 with the differing runs
 * when this was the slot pointer. */
    struct dpb_bag commit_bag;
};

/* One leaf's private buffer set (shared by the fpipe pair and the staircase's
 * B context). Returns NULL on OOM (fully freed). */
static void fleaf_free(yah264_encoder_t *e, struct fpipe_leaf *L)
{
    int pw[3] = { e->padded_w, e->padded_w / e->sub_w, e->padded_w / e->sub_w };
    int pb[3] = { Y264_LUMA_BORDER, Y264_CHROMA_BORDER, Y264_CHROMA_BORDER };
    if (!L)
        return;
    struct w2_gen *G = &L->g;
    free(G->nnz[0]); free(G->nnz[1]); free(G->nnz[2]);
    free(G->i4mode); free(G->mbcbp);
    free(G->mvx); free(G->mvy); free(G->mvx1); free(G->mvy1);
    free(G->refidx); free(G->refidx1);
    free(G->mvdx); free(G->mvdy); free(G->mvdx1); free(G->mvdy1);
    free(G->mb_tr8); free(G->aq_off); free(G->rbsp);
    free(L->mbqp);
    for (int c = 0; c < 3; c++) plane_free(L->rec[c], pw[c], pb[c]);
    free(L->colmvx); free(L->colmvy); free(L->colref); free(L->colpoc);
    for (int i = 0; i < 4; i++) free(L->bseed_cur[i]);
    free(L);
}

static struct fpipe_leaf *fleaf_new(yah264_encoder_t *e)
{
    size_t mvcount = (size_t)e->mv_stride * e->height_in_mbs * 4;
    size_t nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
    int cbh = 4 / e->sub_h;
    size_t nnz0 = (size_t)e->nnz_stride[0] * e->height_in_mbs * 4;
    size_t nnzc = (size_t)e->nnz_stride[1] * e->height_in_mbs * cbh;
    size_t i4sz = (size_t)e->i4mode_stride * e->height_in_mbs * 4;
    struct fpipe_leaf *L = calloc(1, sizeof *L);
    if (!L)
        return NULL;
    struct w2_gen *G = &L->g;
    G->nnz[0] = malloc(nnz0); G->nnz[1] = malloc(nnzc); G->nnz[2] = malloc(nnzc);
    G->i4mode = malloc(i4sz);
    G->mbcbp = malloc(nmb * sizeof(int));
    G->mvx = malloc(mvcount * sizeof(int16_t));
    G->mvy = malloc(mvcount * sizeof(int16_t));
    G->mvx1 = malloc(mvcount * sizeof(int16_t));
    G->mvy1 = malloc(mvcount * sizeof(int16_t));
    G->refidx = malloc(mvcount);
    G->refidx1 = malloc(mvcount);
    G->mvdx = malloc(mvcount * sizeof(int16_t));
    G->mvdy = malloc(mvcount * sizeof(int16_t));
    G->mvdx1 = malloc(mvcount * sizeof(int16_t));
    G->mvdy1 = malloc(mvcount * sizeof(int16_t));
    G->mb_tr8 = e->mb_tr8 ? malloc(nmb) : NULL;
    G->aq_off = e->aq_off ? malloc(nmb) : NULL;
    G->mbtree_off = NULL;           /* leaves never carry mb-tree offsets */
    G->rbsp = malloc(e->rbsp_cap);
    L->mbqp = e->mbqp ? malloc(nmb) : NULL;
    L->rec[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
    L->rec[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    L->rec[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER);
    L->colmvx = malloc(mvcount * sizeof(int16_t));
    L->colmvy = malloc(mvcount * sizeof(int16_t));
    L->colref = malloc(mvcount);
    L->colpoc = malloc(mvcount * sizeof(int16_t));
    if (e->bseed_cur[0])
        for (int i = 0; i < 4; i++)
            L->bseed_cur[i] = malloc(nmb * sizeof(int16_t));
    int ok = G->nnz[0] && G->nnz[1] && G->nnz[2] && G->i4mode && G->mbcbp &&
             G->mvx && G->mvy && G->mvx1 && G->mvy1 && G->refidx && G->refidx1 &&
             G->mvdx && G->mvdy && G->mvdx1 && G->mvdy1 && G->rbsp &&
             (!e->mb_tr8 || G->mb_tr8) && (!e->aq_off || G->aq_off) &&
             (!e->mbqp || L->mbqp) &&
             L->rec[0] && L->rec[1] && L->rec[2] &&
             L->colmvx && L->colmvy && L->colref && L->colpoc &&
             (!e->bseed_cur[0] || (L->bseed_cur[0] && L->bseed_cur[1] &&
                                   L->bseed_cur[2] && L->bseed_cur[3]));
    if (!ok) { fleaf_free(e, L); return NULL; }
    return L;
}

static void fpipe_free(yah264_encoder_t *e)
{
    for (int k = 0; k < 2; k++) {
        fleaf_free(e, e->fp_leaf[k]);
        e->fp_leaf[k] = NULL;
    }
    if (e->fp_bg) { ntp_bg_destroy(e->fp_bg); e->fp_bg = NULL; }
}

static int fpipe_alloc(yah264_encoder_t *e)
{
    e->fp_bg = ntp_bg_create_named("y264-fpipe");
    int ok = e->fp_bg != NULL;
    for (int k = 0; ok && k < 2; k++) {
        struct fpipe_leaf *L = fleaf_new(e);
        e->fp_leaf[k] = L;
        if (!L) { ok = 0; break; }
        L->pool = e->pool;      /* v2: both leaves are jobs on the shared pool */
    }
    if (!ok)
        fpipe_free(e);
    return ok;
}

/* Feature gate + lazy bring-up. ABR/VBV/2-pass serialise on each frame's coded
 * bits, so the parallel path stays off there UNLESS the rcp lagged-feedback
 * schedule is on (then a B's QP is again a pure function of pre-dispatch
 * state -- the decide runs at prep, serially, from the lagged ledger); CQP/CRF
 * frame QP for a B is a pure function of pre-dispatch state (rc_set_qp_crf is
 * a no-op for type 2). Requires >= 8 pool threads so splitting the workers
 * across two concurrent leaf jobs still leaves each a useful wavefront width. */
static int fpipe_ready(yah264_encoder_t *e)
{
    if (!fpipe_on_env())
        return 0;
    if (vbv_bound_all(e))
        return 0;                       /* the bound needs the serial emit */
    if ((e->abr_on || e->vbv_on || e->tp_pass) &&
        (!e->rcp_on || rcp_warm(e) || (e->vbv_on && e->rcp_vbv_tight)))
        return 0;
    if (e->fp_state == 0)
        e->fp_state = (e->pool && ntp_pool_nthreads(e->pool) >= Y264_MT_POOL_MIN &&
                       fpipe_alloc(e)) ? 1 : -1;
    return e->fp_state == 1;
}

/* Defensive: a leaf's registered half-pel planes must be DPB-cached (shared
 * read-only). The build_slice_prep fallback builds into the SHARED e->hpel_buf,
 * which the sibling's prep would clobber; it can't fire for pyramid B refs (all
 * inter references are stored DPB pictures with cached hpel), but if it ever
 * did, the pair encodes serially instead. */
static int fpipe_hpel_private(const yah264_encoder_t *e, const y264_frame_t *f)
{
    const y264_hpel_ref_t *hc = (const y264_hpel_ref_t *)f->hpel_ctx;
    for (int k = 0; k < f->hpel_n; k++)
        for (int i = 0; i < 17; i++)
            if (e->hpel_buf[i][0] &&
                (hc[k].h == e->hpel_buf[i][0] || hc[k].v == e->hpel_buf[i][1] ||
                 hc[k].c == e->hpel_buf[i][2]))
                return 0;
    return 1;
}

/* Serial per-leaf setup, in coding order: refs + scalars + RC head + slice
 * header + QP capture into the leaf's private context. Mirrors code_b_hier's
 * leaf case + emit_frame's RC head byte-for-byte; every shared e-> write here
 * happens in the same order as the serial path, so the post-pair encoder state
 * is identical. Returns 0 to demand the serial fallback (shared hpel). */
static int fpipe_prep_leaf(yah264_encoder_t *e, struct fpipe_leaf *L, int m,
                           int depth, size_t mvcount)
{
    int l0, l1;
    set_b_refs(e, e->bpoc[m], mvcount, &l0, &l1);
    e->poc = e->bpoc[m];
    e->cur_disp = e->bdisp[m];
    e->cur_lr_motion = e->bmotion[m]; e->cur_lr_tdiff = e->btdiff[m];
    e->cur_bseed = m;
    e->cur_b_depth = depth;
    e->frame_num = e->last_ref_fn;      /* non-ref: last reference's FrameNum */
    /* emit_frame's RC head (CQP/CRF, or ABR/2-pass under rcp, reach here; for
 * a B rc_set_qp_crf keeps the anchor base + cascade, so e->qp is untouched
 * -- but run the identical calls so observable state (rc_cplx, debug
 * output) matches the serial path). */
    if (e->rcp_on) {
        e->rcp_cur_cme = e->rcp_bcplx[m];       /* arrival-captured lowres cost */
        e->rcp_cur_cvi = e->rcp_bcvi[m];
        rcp_decide(e, 2, 0, NULL);
    } else if (e->crf_on) {
        double C = frame_complexity(e, e->bplane[m], 0);
        e->rc_cplx = C;
        double Ccrf = frame_complexity_me(e);
        if (getenv("Y264_DBG_CPLX"))
            fprintf(stderr, "CPLX type=%d C=%.0f permb=%.1f  Cme=%.0f permb=%.1f\n", 2,
                    C, C / (e->width_in_mbs * e->height_in_mbs),
                    Ccrf, Ccrf / (e->width_in_mbs * e->height_in_mbs));
        rc_set_qp_crf(e, Ccrf, 2);
    }
    /* freeze the colocated field: the sibling's set_b_refs overwrites e->col*. */
    memcpy(L->colmvx, e->colmvx, mvcount * sizeof(int16_t));
    memcpy(L->colmvy, e->colmvy, mvcount * sizeof(int16_t));
    memcpy(L->colref, e->colref, mvcount);
    memcpy(L->colpoc, e->colpoc, mvcount * sizeof(int16_t));
    L->col_l0poc0 = e->col_l0poc0;
    struct frame_work fw;
    fw_default(e, &fw);
    for (int c = 0; c < 3; c++) fw.rec[c] = L->rec[c];
    fw.colmvx = L->colmvx; fw.colmvy = L->colmvy;
    fw.colref = L->colref; fw.colpoc = L->colpoc;
    fw.hpel_ctx = L->hpel_ctx;
    fw.bseed_cur = L->bseed_cur;
    fw.refidx = L->g.refidx; fw.refidx1 = L->g.refidx1;  /* reset the LEAF's fields */
    build_slice_prep(e, 2, 0, 0, e->bplane[m], L->g.rbsp, e->rbsp_cap, &fw,
                     &L->bs, &L->f, &L->fqp, &L->dblk);
    e->cur_bseed = -1;
    if (!fpipe_hpel_private(e, &L->f))
        return 0;
    /* analyze/deblock write these; aim f at the leaf's own set (prep wired the
 * shared e-> grids). mbtree_off stays NULL for leaves (mbtree_apply is 0). */
    y264_frame_t *f = &L->f;
    struct w2_gen *G = &L->g;
    f->nnz[0] = G->nnz[0]; f->nnz[1] = G->nnz[1]; f->nnz[2] = G->nnz[2];
    f->i4mode = G->i4mode; f->mbcbp = G->mbcbp;
    f->mvx = G->mvx; f->mvy = G->mvy; f->mvx1 = G->mvx1; f->mvy1 = G->mvy1;
    f->refidx = G->refidx; f->refidx1 = G->refidx1;
    f->mvdx = G->mvdx; f->mvdy = G->mvdy; f->mvdx1 = G->mvdx1; f->mvdy1 = G->mvdy1;
    if (f->mb_tr8) f->mb_tr8 = G->mb_tr8;
    if (f->aq_off) f->aq_off = G->aq_off;
    if (f->mbqp)   f->mbqp = L->mbqp;
    /* (prep's motion-field reset already hit the leaf's grids, via fw.refidx) */
    f->pool = L->pool;
    L->cabac = e->pps.entropy_coding_mode_flag ? 1 : 0;
    if (L->cabac) {
        while (y264_bs_pos_bits(&L->bs) & 7)
            y264_bs_write1(&L->bs, 1);  /* cabac_alignment_one_bit */
        y264_cabac_init_engine(&L->cb, L->bs.p); y264_cabac_set_end(&L->cb, L->bs.end - 64);
        y264_cabac_init_contexts(&L->cb, 2, 0, L->fqp);
        f->cabac = &L->cb;
    }
    L->disp = e->cur_disp;
    L->size = 0;
    return 1;
}

/* The concurrent part: analyze + emit + deblock on leaf-private state only.
 * Runs on the caller (leaf 0) or the fp_bg thread (leaf 1); installs its own
 * thread-local ME hpel registry (a serial-fallback analyze runs on THIS thread;
 * the wavefront path re-installs per worker from f->hpel_ctx anyway). No TPROF
 * here: the buckets are unsynchronised main-thread accumulators. */
static void fpipe_leaf_task(void *arg)
{
    struct fpipe_leaf *L = arg;
    y264_frame_t *f = &L->f;
    y264_me_set_hpel((const y264_hpel_ref_t *)f->hpel_ctx, f->hpel_n, f->hpel_stride);
    y264_emit_job_t *job = y264_frame_analyze(f);
    y264_frame_emit(&L->bs, f, job);
    if (L->dblk)
        y264_deblock_frame(f);
    if (L->cabac) {
        if (!L->cb.overflow) cabac_zero_words(&L->f, &L->cb, L->bs.end);
        L->size = L->cb.overflow ? 0 : (size_t)(L->cb.p - L->bs.start);
    } else {
        y264_bs_rbsp_trailing(&L->bs);
        L->size = L->bs.overflow ? 0 : (size_t)(L->bs.p - L->bs.start);
    }
}

/* Serial fallback for one leaf (== code_b_hier's is_ref==0 case). Also used
 * when a prep bails: the aborted prep's e-> writes are all overwritten here. */
static int code_b_leaf(yah264_encoder_t *e, int m, int depth, size_t *off,
                       size_t mvcount)
{
    int l0, l1;
    set_b_refs(e, e->bpoc[m], mvcount, &l0, &l1);
    e->poc = e->bpoc[m];
    e->cur_disp = e->bdisp[m];
    e->cur_lr_motion = e->bmotion[m]; e->cur_lr_tdiff = e->btdiff[m];
    e->rcp_cur_cme = e->rcp_bcplx[m];
    e->rcp_cur_cvi = e->rcp_bcvi[m];
    e->cur_bseed = m;
    e->cur_b_depth = depth;
    e->frame_num = e->last_ref_fn;
    int r = emit_frame(e, off, 2, 0, 0, e->bplane[m]);
    e->cur_bseed = -1;
    return r;
}

/* Encode the sibling leaves m0/m1 concurrently and append their NALs in coding
 * order. The parent's W2 emit may still be trailing; it overlaps the leaves'
 * analyze and is drained before the leaf NALs so the bitstream order holds. */
static int code_b_pair(yah264_encoder_t *e, int m0, int m1, int depth,
                       size_t *off, size_t mvcount)
{
    struct fpipe_leaf *L0 = e->fp_leaf[0], *L1 = e->fp_leaf[1];
    int p0 = fpipe_prep_leaf(e, L0, m0, depth, mvcount);
    int p1 = p0 ? fpipe_prep_leaf(e, L1, m1, depth, mvcount) : 0;
    if (!p0 || !p1) {
        /* Bailed preps already pushed their rcp decides; the serial fallback
 * consumes them (rcp_head predecided) instead of deciding twice. */
        if (e->rcp_on)
            e->rcp_predecided = p0 ? 2 : 1;
        if (code_b_leaf(e, m0, depth, off, mvcount) < 0) return -1;
        return code_b_leaf(e, m1, depth, off, mvcount);
    }
    ntp_bg_submit(e->fp_bg, fpipe_leaf_task, L1);
    TPROF(TP_ANALYZE, fpipe_leaf_task(L0));
    g_ew_site = 3;
    w2_flush(e, off);           /* parent's trailing emit precedes the leaf NALs */
    ntp_bg_sync(e->fp_bg);
    for (int k = 0; k < 2; k++) {
        struct fpipe_leaf *L = k ? L1 : L0;
        if (L->size == 0)
            { slice_overflow_warn(); return -1; }          /* CAVLC overflow: mirrors the serial ret 0 */
        int r; TPROF(TP_NAL, r = append_nal(e, off, 0, YAH264_NAL_SLICE,
                                            L->g.rbsp, L->size));
        if (r < 0)
            return -1;
        if (e->rcp_on)
            rcp_fill(e, 8.0 * (double)L->size);
        for (int c = 0; c < 3; c++) e->rec_out[c] = L->rec[c];
        y264_note_emit(e, L->disp);
        if (e->recon_cb) {
            yah264_picture_t rp;
            rp.csp = e->param.csp; rp.width = e->width; rp.height = e->height;
            rp.pts = 0;
            for (int c = 0; c < 3; c++) {
                rp.plane[c] = L->rec[c];
                rp.stride[c] = e->pstride[c];
            }
            e->recon_cb(e->recon_ud, &rp, L->disp, Y264_BIT_DEPTH);
        }
    }
    return 0;
}

/* Recursively code the buffered B pictures in index range [a, b) in hierarchical
 * (temporal-pyramid) order: the middle first as a reference, then each half. */
static int code_b_hier(yah264_encoder_t *e, int a, int b, int depth, size_t *off,
                       size_t mvcount)
{
    if (a >= b) return 0;
    int m = (a + b) / 2;
    int is_ref = (m > a) || (m + 1 < b);            /* has children to reference it */
    int l0, l1;
    set_b_refs(e, e->bpoc[m], mvcount, &l0, &l1);
    e->poc = e->bpoc[m];
    e->cur_disp = e->bdisp[m];
    e->cur_lr_motion = e->bmotion[m]; e->cur_lr_tdiff = e->btdiff[m];
    e->rcp_cur_cme = e->rcp_bcplx[m];
    e->rcp_cur_cvi = e->rcp_bcvi[m];
    e->cur_bseed = m;                               /* lowres pair seeds for this B */
    e->cur_b_depth = depth;                         /* temporal layer, for the QP cascade */
    /* A reference B takes the next FrameNum (dpb_store advances it); a
 * non-reference B reuses the most recently coded reference's FrameNum. */
    e->frame_num = is_ref ? e->next_frame_num : e->last_ref_fn;
    if (emit_frame(e, off, 2, 0, is_ref, e->bplane[m]) < 0) return -1;
    e->cur_bseed = -1;
    if (is_ref)
        TPROF(TP_DPBSTORE, dpb_store(e, e->bpoc[m], mvcount));
    /* Lever 2: b-a==3 means both children are single non-ref leaves ([a,a+1) and
 * [a+2,a+3)) -- the one shape with two independent siblings to run at once. */
    if (b - a == 3 && fpipe_ready(e))
        return code_b_pair(e, a, a + 2, depth + 1, off, mvcount);
    if (code_b_hier(e, a, m, depth + 1, off, mvcount) < 0) return -1;
    if (code_b_hier(e, m + 1, b, depth + 1, off, mvcount) < 0) return -1;
    return 0;
}

/* --- MT Lever 3 (Y264_STAIR): the reference-frame staircase -------------------
 *
 * Without it, a mini-GOP's B frames wait for their future anchor to FULLY
 * finish (analyze + deblock + borders + hpel + colmv commit) -- the dominant
 * serial dependency in single-GOP encoding. The staircase (x264's frame-threading
 * model, re-derived and made deterministic) overlaps them: the anchor encodes
 * as a job on the shared pool while a trailing per-row pipeline makes each of
 * its rows CONSUMABLE (deblock row j once analysis row j+1 is complete -- after
 * that no analysis cell reads row j-1's pixels again, so no intra-border backup
 * is needed; then left/right border-extend the newly-FINAL rows, build the DPB
 * hpel band, commit the colmv rows) and publishes the progress. B frames then
 * start row r once anchor rows <= r + LAG are consumable.
 *
 * DETERMINISM (the repo's hard invariant): the bitstream must be identical at
 * every thread count and run-to-run. So the vertical MV clamp on B list-1 reads
 * of the anchor is a FIXED function of the LAG constant, applied purely by the
 * env gate (Y264_STAIR=1 clamps even at --threads 1); the CONCURRENCY engages
 * opportunistically (pool >= 8, allocation success) and can never change bits.
 * Machine-invariant parameters (ABR/VBV/2-pass, direct=temporal, no pyramid)
 * disable BOTH the clamp and the engagement together.
 *
 * LAG budget (LAG = 4 MB rows): a clamped list-1 read from B row r touches
 * anchor luma rows <= 16r + 16 (block bottom) + (16*LAG - 24) (max MV, px)
 * + 3 (6-tap) = 16(r+LAG) - 5. The gate releases row r at pub >= r+LAG, when
 * trailing row j = r+LAG is done: luma FINAL rows >= 16j + 13 (rows 13..15 of
 * row j await deblock j+1's top edge), hpel rows >= 16j + 10 (needs source
 * +3), chroma final >= (j+1)*CH - 3, colmv rows >= 4j + 4. Margins: rec 18
 * rows, hpel 17, so the fixed clamp can never read an unwritten row. */
/* One in-flight BURST (a mini-GOP: P anchor + its buffered B's). v3 keeps up to
 * TWO of these alive at once (the previous burst's tail overlapping the next
 * anchor's encode across the API boundary), so everything per-anchor lives here
 * instead of on the shared stair_ctx: the frame/bitstream/engine, the per-row
 * progress (arow/pub) with its condvars, the trailing-pipeline state, an
 * ANCHOR-PRIVATE grid generation (the anchor's analyze/emit/deblock never touch
 * the shared e-> decision grids, so the NEXT anchor's serial prep + analyze can
 * run while this one's emit still reads its own), and the deferred output
 * (NAL stash + recon replay events, assembled at stair_drain on the API
 * thread in coding order). */
/* One entry of a burst's flattened coding plan (stair_plan_hier). */
struct stair_bent { int8_t m, depth, is_ref, pair; };

struct stair_burst;

/* The per-row consumability machinery of ONE in-flight reference picture: the
 * analysis-row progress its wavefront feeds, the trailing pipeline's state, and
 * the published watermark its consumers gate on. The burst's P anchor always
 * has one; under Y264_STAIR_BDEPTH a burst REFERENCE B gets an identical one
 * (same trailer, same LAG budget), which is what lets the leaves staircase
 * against it instead of waiting for it to complete. */
struct stair_prog {
    yah264_encoder_t *enc;

    pthread_mutex_t amx;            /* analysis-row progress (cells -> trailer) */
    pthread_cond_t  acv;
    _Atomic int     arow;           /* analysis rows completed */

    pthread_mutex_t pmx;            /* consumable-row publish (trailer -> gates) */
    pthread_cond_t  pcv;
    _Atomic int     pub;            /* highest trailing row done; hmb = fully done */

    int       hmb;                  /* gate clamp bound (= full completion) */

    /* trailer inputs, aimed per dispatch at the picture being streamed */
    y264_frame_t     *f;            /* its analyze/deblock frame (private grids) */
    struct dpb_entry *slot;         /* the DPB slot it was committed to */
    struct dpb_bag    bag;          /* and the buffers that slot lent it, captured
 * at dispatch: the trailer streams content
 * for rows on end, and by the last one the
 * slot may be lending a later picture. */
    int32_t  *hp_scratch;           /* hpel band scratch (single trailing thread) */
    const int *l0poc; int l0n, l1poc0;  /* POC set for the colmv resolve */
    int       ext_done_l, ext_done_c;   /* rows already left/right border-extended */
    int       hp_done;              /* hpel rows built, in [-B, ph+B) row space */
    /* Reference-B flavour: the burst whose refb_done watermark this trailer
 * fires when its content is fully committed (NULL for the anchor). */
    struct stair_burst *refb_of;
};

struct stair_burst {
    yah264_encoder_t *enc;

    struct stair_prog P;            /* the anchor's row progress + trailer */
    struct stair_gate gate;         /* what THIS anchor's own rows wait on */
    /* v6: the fly's REFERENCE-B watermark, when this launch replaced the wait
 * for that reference B's content with a row gate on it. Decided in the wait
 * loop (which knows what is live) and consumed by the gate install below it,
 * because the two are separated by build_slice_prep. NULL = the launch
 * waited, as before. */
    struct stair_burst *rbgate_of;
    unsigned            rbgate_seq;     /* its seq then: the chain re-checks it */

    ntp_bg_t   *runner, *trailer;   /* anchor analyze+emit / trailing pipeline */

    /* in-flight anchor state */
    y264_frame_t  f;
    y264_bs_t     bs;
    y264_cabac_t  cb;
    int           cabac, fqp, disp;
    int           l0poc[16], l0n, l1poc0;   /* captured for the colmv resolve */
    int           poc;              /* this anchor's POC: the key a later launch
 * resolves its col_src by (see
 * stair_col_src_get) */
    struct dpb_entry *slot;         /* the DPB slot the anchor fills in place */
    size_t        size;             /* coded RBSP size (0 = CAVLC overflow) */

    /* anchor-private generation: decision grids + mbqp + per-frame offset
 * copies + the RBSP the slice codes into (g.rbsp). */
    struct w2_gen g;
    uint8_t      *mbqp;
    y264_hpel_ref_t hpel_ctx[17];   /* private half-pel registry */

    long     dauto[2];              /* Y264_DIRECT_AUTO: this burst's frames' counts */
    int      dauto_temporal;        /* ...and the mode its launch decided for them */
    int      joined;                /* runner+trailer synced; burst is serial now */
    _Atomic int live;               /* launched, not yet drained: this slot's
 * readset is part of the eviction union.
 * Raised at the END of stair_launch (so a
 * launch is never in its own union) and
 * cleared at the drain. With one chain in
 * flight this is exactly "== st->fly".
 *
 * ATOMIC because width made it genuinely
 * cross-thread: the API thread clears it at
 * a drain while a chain driver is reading
 * the same window in stair_slot_readers_wait
 * -- TSan caught exactly that. Release on
 * the clear, acquire on the read; a stale
 * 1 only costs a wait on an idle chain, and
 * a stale 0 cannot happen for a burst older
 * than the reader (its store precedes the
 * reader's own launch). */
    _Atomic unsigned seq;           /* launch order, monotonic for the encoder's
 * life. Written before `live` and never
 * touched again while live, so "older than
 * me" is a plain compare that needs no
 * consistent view of the ring's `cur`.
 *
 * ATOMIC because v6's chain-side wait reads
 * another slot's seq to ask "is this still
 * the burst I meant?", and the relaunch that
 * makes the answer no runs on the API thread
 * at the same time. Everything else here is
 * API-thread-only and unaffected. */

    /* Deferred output, assembled at stair_drain in coding order: the anchor's
 * NAL (g.rbsp/size), the stashed B RBSPs, and the recon replay events
 * (rec_out + recon_cb fire on the API thread at drain, never mid-chain). */
    uint8_t *stash; size_t stash_cap, stash_len;
    int      stash_n;
    struct { size_t off, len; int ref_idc; } stash_item[8];
    struct { pixel *pl[3]; int disp; } replay[9];
    int      nreplay;
    int      anchor_out_rec;        /* anchor replay event recorded */
    int      anchor_billed;         /* Y264_ABR_EARLY=2: this burst's ANCHOR
 * actual is already in the rcp ledger, taken
 * from the runner alone. The NAL is NOT out:
 * it is appended at the drain like every
 * other, so coding order survives the lag */
    int      err;                   /* hard chain error; drain reports -1 */
    int      async;                 /* v3: chain runs on the driver, drain deferred */
    int      wide;                  /* Y264_STAIR_WIDE: this burst may execute
 * alongside its predecessors -- its drain
 * waits for the ring to need its slot, and
 * its e->col* restore runs at that drain
 * (on the API thread, in launch order)
 * instead of at its own chain end, where K
 * chains would race the same grids. */
    struct stair_prog *prev_P;      /* wide only: the immediately-preceding
 * anchor's row progress. Its CONTENT is
 * what this burst's leaves read as list 0,
 * unclamped and ungated, so the chain waits
 * for its full publish before it preps.
 * NULL = nothing preceded it. */
    /* ...and the motion grids that slot held when it was named. The restore and
 * the next launch's redirect both read this LONG afterwards -- at the drain
 * for a wide burst -- and the slot may by then be lending a later picture,
 * so the slot pointer answers "which slot", never "whose motion". With
 * slot-owned grids the two were the same thing; the pool separates them,
 * and TSan caught the drain reading a slot a chain was recycling. */
    struct dpb_bag col_bag;
    struct dpb_entry *col_restore;  /* this chain's last B's list-1 slot, whose
 * col copy into e->col* was skipped (the
 * anchor was reading it); NULL if none.
 * Per SLOT, not per stair_ctx: with K
 * chains each accumulates its own, and the
 * chain-end restore consumes only its own. */
    struct dpb_entry *col_src;      /* published with serial_done: the slot whose
 * colmv field the serial path's e->col*
 * restore will copy -- the NEXT anchor's
 * prep aims its temporal-seed reads there
 * (the restore may not have landed yet).
 * NULL = e->col* already holds it. */

    /* v3 (async) extras, allocated on first async engagement: a stable copy of
 * the anchor source (the lookahead ring slot is recycled while the burst is
 * in flight), private lowres-seed copies, the captured B bank, and the set
 * of reference planes this burst READS (DPB-eviction safety). */
    pixel   *asrc[3];
    int16_t *lrs_mvx, *lrs_mvy;
    int32_t *lrs_cost;
    int      nbuf;
    int      bpoc[8], bdisp[8], bmotion[8], btdiff[8];
    double   bcplx[8];              /* captured arrival-time lowres complexity (rcp) */
    double   bcvi[8];               /* captured arrival-time lowres intra sum (vbv) */
    pixel   *bplane[8][3];
    int16_t *bseed[8][4];
    int      bseed_valid[8], bseed_poc0[8], bseed_poc1[8];
    /* The burst OWNS the reference-B mb-tree field. The walk writes
 * e->bmbtree_off during the anchor's serial prep and the B reads it at emit;
 * on the stair those overlap, so an encoder-wide field lets a later walk
 * overwrite a field an in-flight B still needs -- measured as three md5s
 * from three identical t8 runs. Copying the BYTES here (not the pointer) at capture time gives
 * the burst its own, exactly as bplane/bpoc are captured. */
    int8_t  *bmbtoff[8];
    int      bmbtoff_valid[8];
    const pixel *readset[48];
    int      nread;                 /* -1 = overflowed (treat as "reads everything") */

    /* v4: the burst's coding plan (the b-hier recursion flattened at launch;
 * the async chain preps plan[i] into leaf i, so every leaf survives until
 * the drain) and the reference-B content-commit watermark. After
 * serial_done the DPB bookkeeping is final but a ref-B's CONTENT (recon /
 * hpel / colmv) may still be streaming; the next anchor's launch waits on
 * refb_done before any arrival-side read of it (estimate_wp's recon walk
 * at nref > 1, the colocated field when col_src is a ref-B slot).
 * Signalled under P.pmx/P.pcv, like the row publish. */
    struct stair_bent plan[8];
    int      nplan;
    int      nrefb;
    /* Measurement only: the live bursts this launch's ref-B content wait
 * selected, captured so a LEAF's prep (which runs much later, by when the
 * burst may be drained) can be asked the same question about the same set. */
    struct stair_burst *probe_sel[Y264_STAIR_K];
    int      probe_d[Y264_STAIR_K];
    int      probe_nsel;
    _Atomic int refb_done;
    int      bdepth;                /* v5: this burst's reference B pipelines */
};

/* Per-chain execution machinery: K independent copies of everything that
 * EXECUTES a burst, one per burst ring slot. Burst slot k always runs on chain
 * slot k -- stair_launch picks the slot, and both the chain-state reset and the
 * driver submit in stair_run_burst key off the burst they just launched -- so
 * the owning chain is always derivable from the burst in hand (stair_ch), and
 * no call site has to carry an index of its own.
 *
 * K copies exist; only one of them runs at a time. stair_run_burst still fully
 * drains the previous chain before submitting the next, so this is reuse
 * distance and nothing else. */
struct stair_chain {
    /* Phase B: the concurrent B side. The B's ANALYZE serially among
 * themselves (as gated jobs on the shared pool); each one's entropy emit
 * trails on `bemit`, overlapping the next B's analyze (W2's overlap,
 * inside the burst). The sync flavour ping-pongs leaf[0]/leaf[1]; the v4
 * async chain assigns plan entry i to leaf[i] (each B keeps its context
 * until the drain), so async_ready extends the ring to min(bframes, 7). */
    struct fpipe_leaf *leaf[8];
    int      nleaf;                 /* leaf contexts allocated (2 at stair_alloc) */
    int      lparity;               /* next leaf context to use (sync flavour) */
    struct fpipe_leaf *pend;        /* leaf whose emit is in flight on bemit */
    ntp_bg_t *bemit;

    /* v3: the chain driver thread + the serial-phase handshake (the next
 * anchor's serial prep may start once every driver-side shared-state write
 * of the running chain is done -- i.e. after its last B prep). The fire
 * side is handed its burst by the chain task (it runs ON the driver), the
 * wait side takes st->fly (it runs on the API thread and means "the burst I
 * am about to overlap"); either way both name the chain through a burst
 * rather than through "the" flag. */
    ntp_bg_t *driver;
    pthread_mutex_t smx; pthread_cond_t scv;
    int      serial_done;

    /* v5 (Y264_STAIR_BDEPTH): the burst's REFERENCE B streams through a
 * consumability pipeline of its own -- an identical stair_prog + trailer --
 * so the mini-GOP's leaves staircase against it instead of waiting for it
 * to complete. One reference B is in flight per CHAIN (the covered shapes
 * hold exactly one per burst), which is what made one set of machinery
 * serve the whole encoder while one chain ran. */
    struct stair_prog rprog;        /* the in-flight reference B's progress */
    ntp_bg_t *brunner, *btrailer;   /* its analyze+emit / trailing pipeline */
    struct fpipe_leaf *refb_pipe;   /* the leaf it is running in (NULL = none) */
    struct dpb_entry  *refb_slot;   /* its DPB slot: what a later prep gates on */
    int      bdepth_ok;             /* machinery allocated */

    /* arrival-side spare B bank: swapped with e->bplane/e->bseed at an async
 * launch so new arrivals never write the planes the chain is reading. One
 * bank made that a 2-cycle rotation, which was exactly deep enough while
 * the launch handed back the buffers of the burst still in flight. Per
 * chain it is a (K+1)-cycle rotation: the bank a launch hands to the
 * arrival side belongs to the burst that ran on THIS slot K launches ago,
 * which is drained by definition. Strictly further from any live reader. */
    pixel   *bspare[8][3];
    int16_t *sspare[8][4];
};

/* The burst slot ring is Y264_STAIR_K deep (encoder.h). */
struct stair_ctx {
    ntp_pool_t *pool;               /* the ONE shared pool (v2): the anchor and
 * the B's register as concurrent jobs on
 * it; also the publish-kick target. Shared
 * across every chain BY DESIGN -- the
 * wavefront job table is the whole point. */
    struct stair_chain chain[Y264_STAIR_K];

    struct stair_burst bur[Y264_STAIR_K];   /* burst slot ring */
    int      cur;                   /* slot of the most recent launch */
    unsigned seq;                   /* launch counter; stamped into B->seq */
    /* Y264_DIRECT_AUTO under the stair. A drain records its burst's counts by
 * seq; a launch folds every recorded burst up to seq - K - 1 into the score
 * before deciding. Launches take slots in order and a slot is free only once
 * its burst drained, so at launch n every burst <= n - K - 1 IS drained, and
 * the score burst n reads is a function of n alone, never of chain timing. */
    unsigned dauto_folded;
    long     dauto_fifo[2 * Y264_STAIR_K][2];
    _Atomic unsigned dauto_ready[2 * Y264_STAIR_K];   /* seq whose counts sit in that fifo slot (chain end) */
    struct stair_burst *fly;        /* NEWEST live burst: the one an arriving
 * anchor overlaps (its row gate, its
 * ref-B commit wait, its serial_done).
 * NULL when nothing is in flight. */
    int      nlive;                 /* live bursts, ALL of them. Launches take
 * ring slots in order and drains retire the
 * oldest, so the live set is always the
 * nlive slots ending at `cur` -- launch
 * order, which is coding order, which is
 * the order the NALs must leave in. */

    /* Not per-chain: this is a capability answer ("is the async machinery up
 * for this encoder"), not chain state. stair_async_ready brings every chain
 * up in one go, and stair_run_burst asks BEFORE the launch commits to a
 * slot -- a per-chain answer would make engagement depend on ring phase. */
    int      async_state;           /* 0 unresolved, 1 ready, -1 unavailable */
    /* Y264_STAIR_STAT: arrival-side blocked time (serial_wait + the launch's
 * ref-B commit wait), the v4 window metric. Deliberately NOT per-chain:
 * every one of these waits happens on the API thread, the report at
 * stair_free is an aggregate over anchors anyway, and splitting it by ring
 * slot would only mean summing it back at the end. */
    double   stat_swait_ms, stat_cwait_ms;
    int      stat_n;
    _Atomic double stat_prevp_ms;   /* chain time spent waiting for the PREVIOUS
 * anchor's full publish before its first
 * prep -- the width ceiling once the slot
 * recycle stops being one. Chain-side, so
 * atomic: K chains accumulate into it. */
    /* v6 diagnostic: row claims refused, split by which watermark refused them.
 * Atomic because every wavefront worker asks under the pool mutex, and the
 * question this answers -- is the reference B or its anchor the binding
 * producer? -- is the one that decides whether the gate can ever reach the
 * ceiling the wait's removal bounds. */
    _Atomic long stat_gate_anchor, stat_gate_refb;
    int      stat_slotwait;         /* slot recycles that had to wait for a
 * live burst still reading the picture */
    /* The launch's reference-B content wait, split by how many launches back
 * the burst waited on is (0 = the fly). Index > 0 is the whole point of
 * widening it: those are reads the fly-only test did not cover. `need` is
 * every burst the condition selected, `wait` the subset that actually
 * blocked -- the fired/covered pair, same as stat_hop2_slices/refs. */
    int      stat_refbneed[Y264_STAIR_K], stat_refbwait[Y264_STAIR_K];
    /* Measurement only (Y264_STAIR_STAT): of the bursts that wait SELECTED,
 * how many does a list-0-aware predicate still have to select? Counted
 * separately against the anchor's own list 0 and against the burst's
 * LEAVES' list 0s, because both read a live burst's reference-B recon and
 * only the union of the two is a sound predicate. */
    int      stat_l0sel[Y264_STAIR_K], stat_l0skip[Y264_STAIR_K];
    _Atomic int stat_l0leafsel, stat_l0leafskip;
    /* The ref-B content wait's OWN blocked time. stat_cwait_ms pools it with
 * the recycled-watermark (arow) wait below it, so attributing the pooled
 * total to this wait -- as the --ref round did -- overstates it. */
    double   stat_refb_ms;
    int      stat_wide, stat_nwide;  /* wide launches, and the high-water mark
 * of bursts genuinely in flight at once --
 * the one number that says the gate is
 * doing anything at all */
    /* The launch-split probe's own engagement, same discipline: stat_early is
 * the number of launches that reached the prologue with a burst still live
 * and therefore had a drain to defer, and stat_late the number that then
 * retired it after the anchor's jobs were registered. Both zero means the
 * probe changed no ordering and its wall clock says nothing. */
    int      stat_early, stat_late, stat_earlyfill;
    _Atomic int stat_flaunch;       /* Y264_STAIR_FREELAUNCH: chains whose
 * prev-anchor publish wait was deferred
 * past serial_fire (engagement counter;
 * bumped on chain drivers, hence atomic) */
    /* Y264_STAIR_STAT=2: the chain event trace. Every scheduling transition a
 * chain, an anchor runner or the API thread makes, timestamped, tagged with
 * the ring slot. Aggregates cannot answer "were two chains ENCODING at the
 * same time", and the wall clock cannot either; a trace answers it by
 * inspection. Preallocated and appended
 * with one relaxed fetch_add, so it perturbs the schedule about as much as
 * the STPROF timers already do; overflow just stops recording. */
    struct stair_ev *tr;
    _Atomic long tr_n;
    long     tr_cap;
    double   tr_t0;
};

enum {                              /* trace events; ..._E is the matching end */
    STE_LAUNCH, STE_LAUNCH_E, STE_PREVP, STE_PREVP_E, STE_PREP, STE_PREP_E,
    STE_FIRE, STE_RBGATE, STE_RBGATE_E, STE_B, STE_B_E, STE_PAIR, STE_PAIR_E,
    STE_CHAIN_E, STE_ANCH, STE_ANCH_E, STE_TRAIL, STE_TRAIL_E, STE_DRAIN,
    STE_DRAIN_E, STE_WAIT, STE_WAIT_E, STE_NEV
};
static const char *const stair_ev_name[STE_NEV] = {
    "launch", "launch_end", "prevp_wait", "prevp_wait_end", "prep", "prep_end",
    "serial_fire", "rbgate_wait", "rbgate_wait_end", "b", "b_end", "pair",
    "pair_end", "chain_end", "anchor", "anchor_end", "trailer", "trailer_end",
    "drain", "drain_end", "wait", "wait_end"
};
/* STE_WAIT sites, in stair_launch's order. */
enum { STW_REFB = 1, STW_GATER, STW_DPB, STW_PREP, STW_SERIAL, STW_EVICT,
       STW_SWAP };

struct stair_ev { float t; int8_t slot, ev; int16_t a, b; };

/* Append one trace record. Off unless Y264_STAIR_STAT >= 2. */
static void stair_tr(struct stair_ctx *st, int slot, int ev, int a, int b)
{
    if (!st || !st->tr)
        return;
    long i = atomic_fetch_add_explicit(&st->tr_n, 1, memory_order_relaxed);
    if (i < st->tr_cap)
        st->tr[i] = (struct stair_ev){ (float)(tprof_ms() - st->tr_t0),
                                       (int8_t)slot, (int8_t)ev,
                                       (int16_t)a, (int16_t)b };
}

/* The oldest live burst: the one a drain must retire, because output order is
 * coding order and coding order is launch order. */
static struct stair_burst *stair_oldest(struct stair_ctx *st)
{
    if (!st || st->nlive <= 0)
        return NULL;
    int k = (st->cur - st->nlive + 1) % Y264_STAIR_K;
    return &st->bur[k < 0 ? k + Y264_STAIR_K : k];
}

/* Register a launched burst as live. Called at the END of stair_launch, in the
 * same breath as B->live: `nlive` and `cur` together name the live window, so
 * they must never be updated apart -- with `cur` advanced and the burst not yet
 * counted, stair_oldest would name the burst that was just launched. */
static void stair_live_push(struct stair_ctx *st, struct stair_burst *B)
{
    st->nlive++;
    st->fly = B;                        /* the newest, by construction */
}

static void stair_col_apply(yah264_encoder_t *e, struct stair_burst *B,
                            size_t mvcount);
static int stair_bemit_drain(yah264_encoder_t *e, struct stair_burst *B);
static int stair_refb_join(yah264_encoder_t *e, struct stair_burst *B);

/* Does this list 0 reach any of burst F's reference-B pictures? F's reference-B
 * POCs are fixed at ITS launch (the flattened plan plus the captured B bank)
 * and no live burst spans an IDR -- the IDR path drains everything -- so within
 * the live window a POC names exactly one picture. */
static int stair_l0_reads_refb(const struct stair_burst *F, const int *l0poc, int l0n)
{
    for (int i = 0; i < F->nplan; i++) {
        if (!F->plan[i].is_ref)
            continue;
        int p = F->bpoc[F->plan[i].m];
        for (int k = 0; k < l0n; k++)
            if (l0poc[k] == p)
                return 1;
    }
    return 0;
}

/* The chain a burst executes on: its own ring slot, always. Derived from the
 * pointer rather than stored, so the two rings cannot drift apart. */
static struct stair_chain *stair_ch(struct stair_ctx *st, const struct stair_burst *B)
{
    return &st->chain[B - st->bur];
}

/* The slot the NEXT launch will take. stair_launch computes the same thing;
 * both go through here so the engagement test in stair_run_burst and the
 * machinery the launch actually uses can never name different chains. */
static int stair_next_slot(const struct stair_ctx *st)
{
    return (st->cur + 1) % Y264_STAIR_K;
}

/* The row-progress machinery, shared by the anchor and (BDEPTH) a reference B.
 * hp_scratch is the trailing thread's private hpel band buffer. */
static int stair_prog_init(struct stair_prog *P, yah264_encoder_t *e)
{
    P->enc = e;
    P->hmb = e->height_in_mbs;
    pthread_mutex_init(&P->amx, NULL); pthread_cond_init(&P->acv, NULL);
    pthread_mutex_init(&P->pmx, NULL); pthread_cond_init(&P->pcv, NULL);
    /* The trailing band runs from hp_done to 16*j+10, or to ph+LB on the last
 * row: at most LB+10 rows for the first band, 16+LB+6 for the last -- except
 * a frame of ONE macroblock row, whose single band spans the whole padded
 * plane, 2*LB+16 rows. Sized for that (found by ASan on 16x16 and 33x17
 * inputs, 2026-09-04); +5 for the 6-tap's source rows. */
    size_t rows = (size_t)2 * Y264_LUMA_BORDER + 16 + 5;
    P->hp_scratch = malloc(rows * (size_t)e->pstride[0] * sizeof(int32_t));
    return P->hp_scratch != NULL;
}

static void stair_prog_free(struct stair_prog *P)
{
    free(P->hp_scratch);
    P->hp_scratch = NULL;
    pthread_mutex_destroy(&P->amx); pthread_cond_destroy(&P->acv);
    pthread_mutex_destroy(&P->pmx); pthread_cond_destroy(&P->pcv);
}

/* Arm one picture's progress for a fresh dispatch. */
static void stair_prog_reset(struct stair_prog *P, y264_frame_t *f,
                             struct dpb_entry *slot, const int *l0poc, int l0n,
                             int l1poc0, struct stair_burst *refb_of)
{
    atomic_store_explicit(&P->arow, 0, memory_order_relaxed);
    atomic_store_explicit(&P->pub, -1, memory_order_relaxed);
    P->f = f;
    P->slot = slot;
    if (slot) P->bag = dpbp_bag_of(slot);
    P->l0poc = l0poc; P->l0n = l0n; P->l1poc0 = l1poc0;
    P->ext_done_l = 0; P->ext_done_c = 0;
    P->hp_done = -Y264_LUMA_BORDER;
    P->refb_of = refb_of;
}

static void stair_burst_free(yah264_encoder_t *e, struct stair_burst *B)
{
    int pw[3] = { e->padded_w, e->padded_w / e->sub_w, e->padded_w / e->sub_w };
    int pb[3] = { Y264_LUMA_BORDER, Y264_CHROMA_BORDER, Y264_CHROMA_BORDER };
    if (B->runner)  ntp_bg_destroy(B->runner);      /* bursts always join first */
    if (B->trailer) ntp_bg_destroy(B->trailer);
    struct w2_gen *G = &B->g;
    free(G->nnz[0]); free(G->nnz[1]); free(G->nnz[2]);
    free(G->i4mode); free(G->mbcbp);
    free(G->mvx); free(G->mvy); free(G->mvx1); free(G->mvy1);
    free(G->refidx); free(G->refidx1);
    free(G->mvdx); free(G->mvdy); free(G->mvdx1); free(G->mvdy1);
    free(G->mb_tr8); free(G->aq_off); free(G->mbtree_off); free(G->rbsp);
    free(B->mbqp);
    free(B->stash);
    for (int c = 0; c < 3; c++) plane_free(B->asrc[c], pw[c], pb[c]);
    free(B->lrs_mvx); free(B->lrs_mvy); free(B->lrs_cost);
    for (int i = 0; i < 8; i++) free(B->bmbtoff[i]);
    stair_prog_free(&B->P);
}

static void stair_free(yah264_encoder_t *e)
{
    struct stair_ctx *st = e->st;
    int pw[3] = { e->padded_w, e->padded_w / e->sub_w, e->padded_w / e->sub_w };
    int pb[3] = { Y264_LUMA_BORDER, Y264_CHROMA_BORDER, Y264_CHROMA_BORDER };
    if (!st)
        return;
    /* A close without a terminal flush can still hold live bursts (the API
 * allows it). ntp_bg_destroy joins the thread but drops a task that has not
 * started, so join every live burst's machinery before tearing any of it
 * down -- driver first, then the anchor pipeline it may still submit to. */
    for (int k = 0; k < Y264_STAIR_K; k++) {
        if (!atomic_load_explicit(&st->bur[k].live, memory_order_acquire))
            continue;
        if (st->chain[k].driver) ntp_bg_sync(st->chain[k].driver);
        if (st->bur[k].runner)   ntp_bg_sync(st->bur[k].runner);
        if (st->bur[k].trailer)  ntp_bg_sync(st->bur[k].trailer);
        atomic_store_explicit(&st->bur[k].live, 0, memory_order_release);
    }
    st->nlive = 0; st->fly = NULL;
    for (int k = 0; k < Y264_STAIR_K; k++) {        /* chains always complete */
        struct stair_chain *C = &st->chain[k];
        if (C->driver)   ntp_bg_destroy(C->driver);
        if (C->bemit)    ntp_bg_destroy(C->bemit);
        if (C->brunner)  ntp_bg_destroy(C->brunner);
        if (C->btrailer) ntp_bg_destroy(C->btrailer);
        if (C->bdepth_ok) stair_prog_free(&C->rprog);
    }
    if (stair_stat_on())
        fprintf(stderr, "stair-stat: stair_lag %d (floor %d), stair_mvy_max %d qpel "
                "-- pool %d threads, height %d MB rows\n",
                e->stair_lag, Y264_STAIR_LAG, e->stair_mvy_max,
                e->pool ? ntp_pool_nthreads(e->pool) : 0, e->height_in_mbs);
    if (stair_stat_on())
        fprintf(stderr, "stair-stat: wide launches %d, max concurrent bursts %d, "
                "slot-recycle waits %d\n",
                st->stat_wide, st->stat_nwide, st->stat_slotwait);
    if (stair_stat_on())
        fprintf(stderr, "stair-stat: launch-split probe mode %d -- %d launches had "
                "a drain to defer, %d retired it after the launch, %d took the "
                "anchor's actual early\n", e->abr_early, st->stat_early,
                st->stat_late, st->stat_earlyfill);
    if (stair_stat_on())
        fprintf(stderr, "stair-stat: hop-2 clamp on %d slices, %d list-0 "
                "references clamped by it\n",
                e->stat_hop2_slices, e->stat_hop2_refs);
    if (stair_stat_on())
        fprintf(stderr, "stair-stat: row claims refused -- by an anchor's "
                "watermark %ld, by a reference B's %ld\n",
                atomic_load_explicit(&st->stat_gate_anchor, memory_order_relaxed),
                atomic_load_explicit(&st->stat_gate_refb, memory_order_relaxed));
    if (stair_stat_on())
        fprintf(stderr, "stair-stat: ref-B row gate replaced the wait on %d "
                "launches; the wp arm still selected on %d\n",
                e->stat_refbgate, e->stat_refbblock);
    if (stair_stat_on()) {
        fprintf(stderr, "stair-stat: ref-B content wait by ring distance:");
        for (int k = 0; k < Y264_STAIR_K; k++)
            fprintf(stderr, " %d:%d/%d", k, st->stat_refbwait[k], st->stat_refbneed[k]);
        fprintf(stderr, "  (blocked/selected, 0 = the newest live burst)\n");
        fprintf(stderr, "stair-stat: of those, a list-0-aware predicate still selects:");
        for (int k = 0; k < Y264_STAIR_K; k++)
            fprintf(stderr, " %d:%d/%d", k, st->stat_l0sel[k],
                    st->stat_l0sel[k] + st->stat_l0skip[k]);
        fprintf(stderr, "  (anchor list 0); leaves %d/%d\n",
                atomic_load_explicit(&st->stat_l0leafsel, memory_order_relaxed),
                atomic_load_explicit(&st->stat_l0leafsel, memory_order_relaxed) +
                atomic_load_explicit(&st->stat_l0leafskip, memory_order_relaxed));
        fprintf(stderr, "stair-stat: ref-B content wait blocked %.2f ms of the "
                "%.2f ms commit_wait total\n", st->stat_refb_ms, st->stat_cwait_ms);
    }
    if (stair_stat_on())
        fprintf(stderr, "stair-stat: chain waited %.2f ms total for the previous "
                "anchor's publish (%d of those waits deferred past serial_fire)\n",
                atomic_load_explicit(&st->stat_prevp_ms, memory_order_relaxed),
                atomic_load_explicit(&st->stat_flaunch, memory_order_relaxed));
    if (stair_stat_on())
        fprintf(stderr, "stair-stat: dpb pool %d bags, %d recycles served, "
                "peak %d parked, %d exhausted\n",
                e->dpbp_n, e->dpbp_take, e->dpbp_hi, e->dpbp_exh);
    if (stair_stat_on() && st->stat_n > 0)
        fprintf(stderr, "stair-stat: %d anchors, serial_wait %.2f ms total "
                "(%.3f ms/anchor), commit_wait %.2f ms total (%.3f ms/anchor)\n",
                st->stat_n, st->stat_swait_ms, st->stat_swait_ms / st->stat_n,
                st->stat_cwait_ms, st->stat_cwait_ms / st->stat_n);
    if (st->tr) {
        long n = atomic_load_explicit(&st->tr_n, memory_order_relaxed);
        if (n > st->tr_cap) n = st->tr_cap;
        fprintf(stderr, "stair-trace: %ld events\n", n);
        for (long i = 0; i < n; i++)
            fprintf(stderr, "TR %8.3f %d %s %d %d\n", (double)st->tr[i].t,
                    st->tr[i].slot, stair_ev_name[st->tr[i].ev],
                    st->tr[i].a, st->tr[i].b);
        free(st->tr);
        st->tr = NULL;
    }
    for (int k = 0; k < Y264_STAIR_K; k++)
        stair_burst_free(e, &st->bur[k]);
    for (int c = 0; c < Y264_STAIR_K; c++)
        for (int k = 0; k < 8; k++)
            fleaf_free(e, st->chain[c].leaf[k]);
    for (int b = 0; b < Y264_STAIR_K; b++)
        for (int i = 0; i < 8; i++) {
            struct stair_chain *C = &st->chain[b];
            for (int c = 0; c < 3; c++) plane_free(C->bspare[i][c], pw[c], pb[c]);
            for (int k = 0; k < 4; k++) free(C->sspare[i][k]);
        }
    for (int k = 0; k < Y264_STAIR_K; k++) {
        pthread_mutex_destroy(&st->chain[k].smx);
        pthread_cond_destroy(&st->chain[k].scv);
    }
    free(st);
    e->st = NULL;
}

/* Pool shape (v2, x264's actual model): ONE shared worker set. The anchor's
 * wavefront and each B's wavefront register as concurrent JOBS on e->pool; a
 * worker claims the next ready row from the oldest job that has one, and a B
 * row whose gate (published anchor rows) is closed is simply never claimed --
 * its would-be worker runs anchor rows instead. This replaces both v1 shapes:
 * the core-partitioned split (0.66-0.82x -- the gated B chain collapsed to the
 * narrow anchor partition's row rate while its own partition idled) and the
 * oversubscribed second full-width pool (1.09x at t8 but 0.97x at t18 --
 * scheduling churn at saturation). Wavefront output is thread-count- and
 * schedule-invariant, so none of this can change bits. */
static int stair_burst_alloc(yah264_encoder_t *e, struct stair_burst *B)
{
    B->enc = e;
    int pok = stair_prog_init(&B->P, e);
    B->gate.n = 0;
    B->runner = ntp_bg_create_named("y264-anchor");
    B->trailer = ntp_bg_create_named("y264-trailer");
    /* anchor-private generation (w2_gen shape + mbqp) */
    size_t mvcount = (size_t)e->mv_stride * e->height_in_mbs * 4;
    size_t nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
    int cbh = 4 / e->sub_h;
    size_t nnz0 = (size_t)e->nnz_stride[0] * e->height_in_mbs * 4;
    size_t nnzc = (size_t)e->nnz_stride[1] * e->height_in_mbs * cbh;
    size_t i4sz = (size_t)e->i4mode_stride * e->height_in_mbs * 4;
    struct w2_gen *G = &B->g;
    G->nnz[0] = malloc(nnz0); G->nnz[1] = malloc(nnzc); G->nnz[2] = malloc(nnzc);
    G->i4mode = malloc(i4sz);
    G->mbcbp = malloc(nmb * sizeof(int));
    G->mvx = malloc(mvcount * sizeof(int16_t));
    G->mvy = malloc(mvcount * sizeof(int16_t));
    G->mvx1 = malloc(mvcount * sizeof(int16_t));
    G->mvy1 = malloc(mvcount * sizeof(int16_t));
    G->refidx = malloc(mvcount);
    G->refidx1 = malloc(mvcount);
    G->mvdx = malloc(mvcount * sizeof(int16_t));
    G->mvdy = malloc(mvcount * sizeof(int16_t));
    G->mvdx1 = malloc(mvcount * sizeof(int16_t));
    G->mvdy1 = malloc(mvcount * sizeof(int16_t));
    G->mb_tr8 = e->mb_tr8 ? malloc(nmb) : NULL;
    G->aq_off = e->aq_off ? malloc(nmb) : NULL;
    G->mbtree_off = e->mbtree_off ? malloc(nmb) : NULL;
    G->rbsp = malloc(e->rbsp_cap);
    B->mbqp = e->mbqp ? malloc(nmb) : NULL;
    return B->runner && B->trailer && pok &&
           G->nnz[0] && G->nnz[1] && G->nnz[2] && G->i4mode && G->mbcbp &&
           G->mvx && G->mvy && G->mvx1 && G->mvy1 && G->refidx && G->refidx1 &&
           G->mvdx && G->mvdy && G->mvdx1 && G->mvdy1 && G->rbsp &&
           (!e->mb_tr8 || G->mb_tr8) && (!e->aq_off || G->aq_off) &&
           (!e->mbtree_off || G->mbtree_off) && (!e->mbqp || B->mbqp);
}

static int stair_alloc(yah264_encoder_t *e)
{
    struct stair_ctx *st = calloc(1, sizeof *st);
    if (!st)
        return 0;
    e->st = st;
    st->pool = e->pool;
    int ok = 1;
    for (int c = 0; c < Y264_STAIR_K; c++) {
        struct stair_chain *C = &st->chain[c];
        pthread_mutex_init(&C->smx, NULL); pthread_cond_init(&C->scv, NULL);
        C->bemit = ntp_bg_create_named("y264-bemit");
        for (int k = 0; k < 2; k++) {
            C->leaf[k] = fleaf_new(e);
            if (C->leaf[k]) C->leaf[k]->pool = e->pool;
        }
        C->nleaf = 2;               /* async_ready extends to min(bframes, 7) */
        ok = C->bemit && C->leaf[0] && C->leaf[1] && ok;
    }
    for (int k = 0; k < Y264_STAIR_K; k++)
        ok = stair_burst_alloc(e, &st->bur[k]) && ok;
    if (stair_stat_on() >= 2) {
        st->tr_cap = 1 << 18;
        st->tr = malloc((size_t)st->tr_cap * sizeof *st->tr);
        st->tr_t0 = tprof_ms();
    }
    if (!ok) {
        stair_free(e);
        return 0;
    }
    return 1;
}

/* Clamp eligibility: every term is a machine-invariant parameter, so a stream
 * encoded with Y264_STAIR=1 carries the clamp at ANY thread count (including
 * 1), and engagement can never change bits. Temporal direct is excluded: its
 * derived list-1 MV is mvL0 - mvCol, which no closure bounds (and its
 * slice-header scan reads the whole col grid up front). Spatial direct needs
 * no per-mode check: the derived MV is a median of already-clamped coded L1
 * MVs, so the clamp closes over it. */
static int stair_clamp_on(const yah264_encoder_t *e)
{
    return stair_on_env() && e->b_pyramid && !stair_direct_blocks(e)
        && (e->rcp_on || (!e->abr_on && !e->vbv_on && !e->tp_pass));
}

static int stair_ready(yah264_encoder_t *e)
{
    if (!stair_clamp_on(e))
        return 0;
    if (vbv_bound_all(e))
        return 0;                       /* no retry window at a burst anchor:
 * its leaves are row-gated on the recon
 * a retry would change (vbv_bound_all) */
    if (e->rcp_on && (rcp_warm(e) || (e->vbv_on && e->rcp_vbv_tight)))
        return 0;                   /* serial-tight RC feedback while warm
 * or in a tight VBV burst */
    if (!e->pool || ntp_pool_nthreads(e->pool) < Y264_MT_POOL_MIN || !e->wf_warmed)
        return 0;
    if (e->st_state == 0)
        e->st_state = stair_alloc(e) ? 1 : -1;
    return e->st_state == 1;
}

/* Producer hook: fired by the wavefront worker that completes the LAST cell of
 * anchor row mby (rows complete in increasing order). Wakes the trailer. */
static void stair_row_done(void *ctx, int mby)
{
    struct stair_prog *P = ctx;
    pthread_mutex_lock(&P->amx);
    atomic_store_explicit(&P->arow, mby + 1, memory_order_release);
    pthread_cond_signal(&P->acv);
    pthread_mutex_unlock(&P->amx);
}

static void stair_arow_wait(struct stair_prog *P, int need)
{
    if (atomic_load_explicit(&P->arow, memory_order_acquire) >= need)
        return;
    pthread_mutex_lock(&P->amx);
    while (atomic_load_explicit(&P->arow, memory_order_acquire) < need)
        pthread_cond_wait(&P->acv, &P->amx);
    pthread_mutex_unlock(&P->amx);
}

/* A serial-fallback analyze (pool-slot OOM) never fires the per-row hooks, so
 * every runner drives the watermark to hmb once its analyze returns. */
static void stair_arow_finish(struct stair_prog *P, int hmb)
{
    pthread_mutex_lock(&P->amx);
    if (atomic_load_explicit(&P->arow, memory_order_relaxed) < hmb)
        atomic_store_explicit(&P->arow, hmb, memory_order_release);
    pthread_cond_broadcast(&P->acv);
    pthread_mutex_unlock(&P->amx);
}

/* The publish twin of stair_arow_finish: raise the watermark to hmb and wake
 * everyone, for a picture that will never publish the rest itself. A chain that
 * bails between arming a reference-B watermark (its prep) and launching its
 * trailer (its encode) would otherwise leave a row gater waiting on rows nobody
 * is going to write. Monotone raise under pmx, so it is safe even if the
 * trailer is publishing concurrently -- but callers only use it after the join,
 * where "no more rows are coming" is what makes the value honest. */
static void stair_pub_finish(struct stair_prog *P, int hmb)
{
    pthread_mutex_lock(&P->pmx);
    if (atomic_load_explicit(&P->pub, memory_order_relaxed) < hmb)
        atomic_store_explicit(&P->pub, hmb, memory_order_release);
    pthread_cond_broadcast(&P->pcv);
    pthread_mutex_unlock(&P->pmx);
}

/* Block until this picture's trailer has published every row (used by an emit
 * that would otherwise race the trailer's reads of the same grids). */
static void stair_pub_wait_all(struct stair_prog *P, int hmb)
{
    if (atomic_load_explicit(&P->pub, memory_order_acquire) >= hmb)
        return;
    pthread_mutex_lock(&P->pmx);
    while (atomic_load_explicit(&P->pub, memory_order_acquire) < hmb)
        pthread_cond_wait(&P->pcv, &P->pmx);
    pthread_mutex_unlock(&P->pmx);
}

/* Publish "trailing rows <= j are consumable" (j == hmb: fully done, bottom
 * borders extended). Publisher and waiters share pmx, so no lost wakeup; the
 * data written before the publish is ordered by the release store + the
 * waiter's acquire load (its fast path never takes the lock). The pool kick
 * makes parked workers re-evaluate the gated rows' claim gates
 * (stair_row_ready): the publish may have opened one. */
static void stair_publish(struct stair_prog *P, int j)
{
    pthread_mutex_lock(&P->pmx);
    atomic_store_explicit(&P->pub, j, memory_order_release);
    pthread_cond_broadcast(&P->pcv);
    pthread_mutex_unlock(&P->pmx);
    ntp_pool_kick(P->enc->st->pool);
}

/* Incremental border extension: identical ops to extend_plane, applied to the
 * row ranges as they become final. */
static void stair_extend_lr(pixel *p, int stride, int w, int b, int y0, int y1)
{
    for (int y = y0; y < y1; y++) {
        pixel *row = p + (size_t)y * stride;
        pixel lval = row[0], rval = row[w - 1];
        for (int x = 0; x < b; x++) { row[-b + x] = lval; row[w + x] = rval; }
    }
}
static void stair_extend_top(pixel *p, int stride, int w, int b)
{
    for (int y = 1; y <= b; y++)
        memcpy(p - (size_t)y * stride - b, p - b, ((size_t)w + 2 * b) * sizeof(pixel));
}
static void stair_extend_bottom(pixel *p, int stride, int w, int h, int b)
{
    for (int y = 1; y <= b; y++)
        memcpy(p + (size_t)(h - 1 + y) * stride - b,
               p + (size_t)(h - 1) * stride - b, ((size_t)w + 2 * b) * sizeof(pixel));
}

/* The trailing per-row consumability pipeline, on its own background thread.
 * Row j: wait for analysis rows <= j+1 (deblocking row j writes into every
 * pixel row of j near vertical edges, including row 15, which analysis row j+1
 * reads for intra prediction -- so wait until it has read them), deblock row j,
 * extend the newly-FINAL rows' left/right borders, build the hpel band, commit
 * the colmv rows into the DPB slot, publish. Byte-identical to the serial
 * deblock-frame + extend_borders + dpb_store hpel/colmv (same ops, same order
 * within each dependency chain). */
static void stair_trailer_task(void *arg)
{
    struct stair_prog *P = arg;
    yah264_encoder_t *e = P->enc;
    y264_frame_t *f = P->f;
    struct dpb_bag *g = &P->bag;      /* the picture's buffers, not the slot's */
    const int hmb = e->height_in_mbs;
    const int LB = Y264_LUMA_BORDER, CB = Y264_CHROMA_BORDER;
    const int sstride = e->pstride[0];
    const int pw = e->padded_w, ph = e->padded_h;
    const int cw = pw / e->sub_w, chh = ph / e->sub_h;
    const int CH = 16 / e->sub_h;               /* chroma rows per MB row */
    const int cmargin = e->cf_idc == 3 ? 3 : 2; /* rows deblock j+1 still touches */
    const size_t mrow = (size_t)e->mv_stride * 4;   /* mv-grid cells per MB row */

    for (int j = 0; j < hmb; j++) {
        int last = (j == hmb - 1);
        stair_arow_wait(P, last ? hmb : j + 2);
        y264_deblock_rows(f, j, j + 1);
        /* FINAL pixel rows after deblocking row j (deblock j+1's top edge still
 * rewrites the last 3 luma / cmargin chroma rows of row j). */
        int fin_l = last ? ph : 16 * j + 13;
        int fin_c = last ? chh : (j + 1) * CH - cmargin;
        stair_extend_lr(f->rec[0], sstride, pw, LB, P->ext_done_l, fin_l);
        for (int c = 1; c < 3; c++)
            stair_extend_lr(f->rec[c], e->pstride[c], cw, CB, P->ext_done_c, fin_c);
        P->ext_done_l = fin_l;
        P->ext_done_c = fin_c;
        if (j == 0) {                           /* row 0 final -> top borders */
            stair_extend_top(f->rec[0], sstride, pw, LB);
            for (int c = 1; c < 3; c++)
                stair_extend_top(f->rec[c], e->pstride[c], cw, CB);
        }
        if (last) {
            stair_extend_bottom(f->rec[0], sstride, pw, ph, LB);
            for (int c = 1; c < 3; c++)
                stair_extend_bottom(f->rec[c], e->pstride[c], cw, chh, CB);
        }
        /* hpel band over the final rows (6-tap needs source rows +-2/+3). */
        if (e->hpel_on && g->hpel[0]) {
            int hi = last ? ph + LB : 16 * j + 10;
            if (hi > P->hp_done) {
                double t0 = hpel_probe_on() ? tprof_ms() : 0;
                y264_hpel_census_built(g->hpel[0], P->hp_done, hi, sstride);
                for (int pass = hpel_double_on(); pass > 0; pass--)
                y264_mc_build_hpel_rows(g->hpel[0], g->hpel[1], g->hpel[2], sstride,
                                        f->rec[0], sstride, pw, ph, LB,
                                        P->hp_scratch, sstride, P->hp_done, hi);
                if (hpel_probe_on()) hpb_add(tprof_ms() - t0, hi - P->hp_done);
                P->hp_done = hi;
            }
        }
        /* colmv rows: final at analysis-row completion (deblock never changes
 * motion). Same resolution as dpb_store, over the anchor's PRIVATE
 * grids (f->), using the POC set captured at the anchor's prep. */
        for (size_t i = (size_t)j * mrow; i < (size_t)(j + 1) * mrow; i++) {
            if (f->refidx[i] >= 0) {
                g->mvx[i] = f->mvx[i]; g->mvy[i] = f->mvy[i]; g->refidx[i] = f->refidx[i];
                g->colpoc[i] = (int16_t)(f->refidx[i] < P->l0n
                                         ? P->l0poc[f->refidx[i]] : -1);
            } else if (f->refidx1[i] >= 0) {
                g->mvx[i] = f->mvx1[i]; g->mvy[i] = f->mvy1[i]; g->refidx[i] = f->refidx1[i];
                g->colpoc[i] = (int16_t)(P->l1poc0 >= 0 ? (P->l1poc0 | Y264_COLPOC_L1) : -1);
            } else {
                g->mvx[i] = 0; g->mvy[i] = 0;
                g->refidx[i] = -1;
                g->colpoc[i] = -1;
            }
        }
        stair_publish(P, last ? hmb : j);
    }
    /* Reference-B flavour: its DPB content (recon + borders + hpel + colmv) is
 * now complete -- release the arrival-side readers the next anchor's launch
 * holds on refb_done. */
    if (P->refb_of) {
        struct stair_burst *B = P->refb_of;
        pthread_mutex_lock(&B->P.pmx);
        atomic_fetch_add_explicit(&B->refb_done, 1, memory_order_release);
        pthread_cond_broadcast(&B->P.pcv);
        pthread_mutex_unlock(&B->P.pmx);
    }
}

/* Anchor runner: the wavefront analyze (on pool_a; the per-row hooks feed the
 * trailer) followed by the entropy emit -- which reads only the decision grids,
 * never recon, so it overlaps the B side freely. No TPROF here (the buckets are
 * unsynchronised main-thread accumulators). */
static void stair_runner_task(void *arg)
{
    struct stair_burst *B = arg;
    y264_frame_t *f = &B->f;
    struct stair_ctx *tst = B->enc->st;
    int tsl = (int)(B - tst->bur);
    stair_tr(tst, tsl, STE_ANCH, (int)atomic_load(&B->seq), B->poc);
    y264_me_set_hpel((const y264_hpel_ref_t *)f->hpel_ctx, f->hpel_n, f->hpel_stride);
    y264_emit_job_t *job = y264_frame_analyze(f);
    stair_tr(tst, tsl, STE_ANCH_E, (int)atomic_load(&B->seq), B->poc);
    stair_arow_finish(&B->P, f->hmb);
    /* The entropy emit walks the burst's decision grids -- and the CAVLC intra
 * path RE-writes the motion field it authored (set_mb_intra_motion, same
 * values), which would race the trailer's bS reads. Emit only after the
 * trailer has consumed everything; it finishes promptly (analyze is done,
 * the publish above unblocks its last rows), so this costs ~one row's
 * trailing work, not a pipeline stage. */
    stair_pub_wait_all(&B->P, f->hmb);
    y264_frame_emit(&B->bs, f, job);
    if (B->cabac) {
        if (!B->cb.overflow) cabac_zero_words(f, &B->cb, B->bs.end);
        B->size = B->cb.overflow ? 0 : (size_t)(B->cb.p - B->bs.start);
    } else {
        y264_bs_rbsp_trailing(&B->bs);
        B->size = B->bs.overflow ? 0 : (size_t)(B->bs.p - B->bs.start);
    }
}

/* Release every parked bag no live burst can still name.
 *
 * The test is one compare against launch order, not a scan of readsets: a bag
 * was parked with the seq of the newest burst then launched, and every holder
 * of a pointer into that picture -- a burst's readset, the colocated redirect
 * that reads a slot's motion grids in place, the recon replay a chain defers to
 * its drain -- belongs to a burst that was live at or before that moment. A
 * burst launched later cannot reach it: reference lists are built from the DPB,
 * and the picture left the DPB before its bag was parked. Bursts drain
 * oldest-first, so "the oldest live burst is newer than the park" means all of
 * them are gone.
 *
 * Called only from a recycle, which is also the only moment the answer is
 * needed, so it never blocks anyone and needs no promptness of its own.
 *
 * Threading: a recycle runs either on the API thread inside a launch or on the
 * one chain prep phase serial_done lets run at a time, and those two exclude
 * each other -- the same exclusion e->dpb itself relies on, so the pool needs
 * no lock. A DRAIN does not hold it: it retires an older burst while a newer
 * chain preps, which is exactly why `live` is atomic. DO NOT ALSO SWEEP AT
 * THE DRAIN: it races this one (TSan reports it), and the sweep at the take
 * already sees the freshest live set, so the drain-side sweep buys nothing but
 * the race. */
static void dpbp_sweep(yah264_encoder_t *e)
{
    struct stair_ctx *st = e->st;
    unsigned oldest = ~0u;
    if (st)
        for (int b = 0; b < Y264_STAIR_K; b++)
            if (atomic_load_explicit(&st->bur[b].live, memory_order_acquire) &&
                st->bur[b].seq < oldest)
                oldest = atomic_load_explicit(&st->bur[b].seq, memory_order_relaxed);
    for (int i = 0; i < e->dpbp_npend; ) {
        if (e->dpbp_pend[i].seq < oldest) {
            e->dpbp_free[e->dpbp_nfree++] = e->dpbp_pend[i].b;
            e->dpbp_pend[i] = e->dpbp_pend[--e->dpbp_npend];
        } else {
            i++;
        }
    }
}

/* Swap slot `d`'s buffers for a fresh bag and park the old ones, so the picture
 * leaving the slot keeps everything it lent out. `rec` is the incoming
 * picture's write target (already captured by its prep), which becomes the
 * slot's plane exactly as the in-place swap did -- what changes is where the
 * OLD plane goes: to the pool, not straight back out as the next writer's
 * target. Returns 0 when the pool is empty, and the caller takes the wait. */
static int dpbp_recycle(yah264_encoder_t *e, struct dpb_entry *d, pixel *rec[3])
{
    if (e->dpbp_n <= 0)
        return 0;                       /* pool off: the wait is the mechanism */
    dpbp_sweep(e);
    if (e->dpbp_nfree <= 0) {
        e->dpbp_exh++;
        return 0;
    }
    struct dpb_bag g = e->dpbp_free[--e->dpbp_nfree];
    struct dpb_bag old;
    for (int c = 0; c < 3; c++) {
        old.plane[c] = d->plane[c];
        old.hpel[c] = d->hpel[c];
        d->plane[c] = rec[c];
        rec[c] = g.plane[c];
        d->hpel[c] = g.hpel[c];
    }
    old.mvx = d->mvx; old.mvy = d->mvy;
    old.refidx = d->refidx; old.colpoc = d->colpoc;
    d->mvx = g.mvx; d->mvy = g.mvy; d->refidx = g.refidx; d->colpoc = g.colpoc;
    e->dpbp_pend[e->dpbp_npend].b = old;
    e->dpbp_pend[e->dpbp_npend].seq = e->st ? e->st->seq : 0;
    e->dpbp_npend++;
    if (e->dpbp_npend > e->dpbp_hi) e->dpbp_hi = e->dpbp_npend;
    e->dpbp_take++;
    return 1;
}

/* A DPB slot is about to be recycled: its plane buffer is handed to the new
 * picture's writer and its half-pel triple is rebuilt IN PLACE. Those two have
 * different lifetimes, and that is the trap. The PLANE travels with the picture
 * (the swap below moves it out), but the HPEL belongs to the SLOT -- it is
 * allocated once per slot and every occupant's trailer refills the same three
 * buffers. So a burst still searching that picture is reading half-pel planes
 * the next occupant overwrites, and it does not even have a stale pointer to
 * show for it. Wait for those readers.
 *
 * Keyed on d->plane[0], which is the picture's identity in every readset AND
 * the key build_slice_prep's hpel lookup matched on, so one test covers both.
 *
 * Acyclic, and enforced rather than argued: a burst only ever waits on one
 * launched EARLIER than itself. The caller should always be the newest anyway
 * (the launch on the API thread, or the one prep phase the serial_done
 * handshake lets run at a time), but "should" is how wait cycles get built, and
 * the compare is free. self == NULL means the API thread's own launch, which is
 * newer than everything live.
 *
 * The ordering key is B->seq, not the ring's `cur`: `cur` moves on the API
 * thread while a chain driver runs this, so reading it here would be one more
 * race to reason about for no benefit. */
static void stair_slot_readers_wait(yah264_encoder_t *e, const struct dpb_entry *d,
                                    const struct stair_burst *self)
{
    struct stair_ctx *st = e->st;
    if (!st || !d->plane[0])
        return;
    unsigned selfseq = self ? self->seq : ~0u;
    for (int b = 0; b < Y264_STAIR_K; b++) {
        struct stair_burst *F = &st->bur[b];
        if (!atomic_load_explicit(&F->live, memory_order_acquire) || F == self)
            continue;
        if (F->seq >= selfseq)
            continue;                       /* not older: never wait on it */
        int hit = F->nread < 0;             /* overflowed: reads everything */
        for (int k = 0; !hit && k < F->nread; k++)
            if (F->readset[k] == d->plane[0])
                hit = 1;
        if (!hit)
            continue;
        ntp_bg_sync(stair_ch(st, F)->driver);   /* its B chain */
        ntp_bg_sync(F->runner);                 /* its anchor's analyze+emit */
        ntp_bg_sync(F->trailer);                /* and the trailer that WRITES
 * this slot's hpel */
        st->stat_slotwait++;
    }
}

/* DPB bookkeeping for the in-flight anchor, done UP FRONT so the B preps can
 * resolve their list-1 reference to it; the slot's CONTENT (recon rows, hpel,
 * colmv) streams in via the trailer. Mirrors dpb_store's eviction + slot
 * selection EXACTLY (keep in sync). The anchor's write target (captured into
 * st->f.rec at prep, == pre-swap e->rec) becomes the slot's plane; the slot's
 * old buffer becomes the NEXT frame's e->rec, untouched until after the burst. */
static struct dpb_entry *stair_dpb_begin(yah264_encoder_t *e, int poc)
{
    int used = 0;
    for (int i = 0; i < e->dpb_size; i++) used += e->dpb[i].used;
    if (used >= e->sps.max_num_ref_frames) {
        int victim = -1, minfn = 0;
        for (int i = 0; i < e->dpb_size; i++)
            if (e->dpb[i].used && (victim < 0 || fn_wrap(e, e->dpb[i].frame_num) < minfn)) {
                victim = i; minfn = fn_wrap(e, e->dpb[i].frame_num);
            }
        /* v3 eviction safety: the victim's buffer becomes the NEXT coding
 * target (its planes swap with e->rec below) -- if an in-flight burst
 * still READS it (multi-ref lists reach old pictures), wait for those
 * readers first. Readsets are complete and stable here (the caller held
 * the serial_done handshake); nread < 0 = overflowed, treat as "reads
 * everything". Never fires at nref 1 (lists only reach the two newest
 * pictures).
 *
 * The guard is the UNION over every live ring slot, not over the fly
 * alone: with one chain in flight the two sets are the same (`live` is
 * raised at the end of a launch, after this function, and cleared at
 * the drain, so the burst being launched is never in its own union),
 * and with K chains it is the K-way readset the design doc calls for.
 * The driver is still one, so it is synced once for the whole union;
 * runners are per burst. */
        if (victim >= 0 && e->st && !stair_evictpool_on()) {
            struct stair_ctx *st = e->st;
            for (int b = 0; b < Y264_STAIR_K; b++) {
                struct stair_burst *F = &st->bur[b];
                if (!atomic_load_explicit(&F->live, memory_order_acquire))
                    continue;
                int hit = F->nread < 0;
                for (int k = 0; !hit && k < F->nread; k++)
                    if (F->readset[k] == e->dpb[victim].plane[0])
                        hit = 1;
                if (!hit)
                    continue;
                /* Per burst now, not once for the union: each live burst has
 * its own driver, and syncing one says nothing about another's
 * leaves. */
                stair_tr(st, (int)(F - st->bur), STE_WAIT, STW_EVICT,
                         (int)atomic_load(&F->seq));
                ntp_bg_sync(stair_ch(st, F)->driver);   /* B chain reads */
                ntp_bg_sync(F->runner);                 /* the anchor's own */
                stair_tr(st, (int)(F - st->bur), STE_WAIT_E, STW_EVICT,
                         (int)atomic_load(&F->seq));
            }
        }
        if (victim >= 0) e->dpb[victim].used = 0;
    }
    int slot = -1;
    for (int i = 0; i < e->dpb_size; i++) if (!e->dpb[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;
    struct dpb_entry *d = &e->dpb[slot];
    /* Pool first: the retiring picture keeps its buffers and nothing blocks.
 * The wait is the floor for an exhausted (or absent) pool, and it is the
 * path every non-wide encode still takes. */
    if (!dpbp_recycle(e, d, e->rec)) {
        if (e->st) stair_tr(e->st, slot, STE_WAIT, STW_SWAP, 0);
        stair_slot_readers_wait(e, d, NULL);  /* the launching burst is not live yet */
        if (e->st) stair_tr(e->st, slot, STE_WAIT_E, STW_SWAP, 0);
        for (int c = 0; c < 3; c++) {
            pixel *t = d->plane[c]; d->plane[c] = e->rec[c]; e->rec[c] = t;
        }
    }
    d->hpel_valid = (e->hpel_on && d->hpel[0]) ? 1 : 0;
    d->col_l0poc0 = e->cur_l0n > 0 ? e->cur_l0poc[0] : -1;
    d->poc = poc;
    d->frame_num = e->next_frame_num;
    d->used = 1;
    e->last_ref_fn = e->next_frame_num;
    int maxfn = 1 << (e->sps.log2_max_frame_num_minus4 + 4);
    e->next_frame_num = (e->next_frame_num + 1) % maxfn;
    return d;
}

/* B row gate, blocking form: block until the anchor's consumable rows cover
 * this B row's clamped reach (see the LAG budget above -- now
 * P->enc->stair_lag, computed once at open by stair_lag_for, not the fixed
 * Y264_STAIR_LAG constant). Used by the SERIAL fallback analyze (first cell of
 * each row); the trailer always reaches pub == hmb, so no deadlock. On the
 * pool path the non-blocking twin below gates the CLAIM instead, so this
 * check passes instantly there. */
static void stair_row_gate(void *ctx, int mby)
{
    const struct stair_gate *G = ctx;
    if (stair_unsafe_no_rowgate())
        return;                         /* ceiling probe: see the twin below */
    for (int k = 0; k < G->n; k++) {
        struct stair_prog *P = G->p[k];
        int need = mby + P->enc->stair_lag;
        if (need > P->hmb) need = P->hmb;   /* bottom rows: full completion */
        if (atomic_load_explicit(&P->pub, memory_order_acquire) >= need)
            continue;
        pthread_mutex_lock(&P->pmx);
        while (atomic_load_explicit(&P->pub, memory_order_acquire) < need)
            pthread_cond_wait(&P->pcv, &P->pmx);
        pthread_mutex_unlock(&P->pmx);
    }
}

/* Y264_UNSAFE_NO_ROWGATE=1: every consumer row is claimable the instant it
 * exists. Third of the delete-probe family (`_NO_REFBWAIT` deletes the
 * launch-side content wait, `_NO_PREVPWAIT` the chain-side publish wait) and the
 * one that covers the row-granular direction those two do not: it prices the
 * CEILING of every possible reference-publish scheme, including the row-banded
 * publish, in one run. It races by construction -- a consumer's ME reads
 * reference rows nobody has written -- so the output is not the encoder's and
 * this can never ship. Timing only, and two publish-scheme arms on this branch
 * have already measured null. */
static int stair_unsafe_no_rowgate(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_UNSAFE_NO_ROWGATE"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Non-blocking twin, the multi-frame pool's per-row claim gate: "may B row mby
 * start?". Called under the pool mutex -- one atomic watermark read, monotonic
 * (pub only advances within a burst). A closed row is never claimed, so its
 * would-be worker serves the anchor's job instead of sleeping in a cell. */
/* Y264_UNSAFE_GATE_AROW=1: gate on the reference's ANALYSIS watermark instead of
 * its PUBLISH watermark, i.e. pretend deblock + border extend + hpel are free
 * and instantaneous. Splits what the row gate costs into the half that is
 * waiting for the producer to decide its pixels (arow) and the half that is
 * waiting for the trailer to finish them (pub - arow) -- which is the number
 * that says whether parallelising the trailer is worth anything. Unsafe: a
 * consumer's ME then reads undeblocked, unextended pixels with no hpel planes,
 * so the output is not the encoder's. Timing only. */
static int stair_unsafe_gate_arow(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_UNSAFE_GATE_AROW"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

static int stair_row_ready(void *ctx, int mby)
{
    const struct stair_gate *G = ctx;
    if (stair_unsafe_no_rowgate())
        return 1;
    if (stair_unsafe_gate_arow()) {
        for (int k = 0; k < G->n; k++) {
            struct stair_prog *P = G->p[k];
            int need = mby + P->enc->stair_lag;
            if (need > P->hmb) need = P->hmb;
            if (atomic_load_explicit(&P->arow, memory_order_acquire) < need)
                return 0;
        }
        return 1;
    }
    for (int k = 0; k < G->n; k++) {
        struct stair_prog *P = G->p[k];
        int need = mby + P->enc->stair_lag;
        if (need > P->hmb) need = P->hmb;
        if (atomic_load_explicit(&P->pub, memory_order_acquire) < need) {
            if (stair_stat_on() && P->enc->st)
                atomic_fetch_add_explicit(k == G->rbidx
                                              ? &P->enc->st->stat_gate_refb
                                              : &P->enc->st->stat_gate_anchor,
                                          1, memory_order_relaxed);
            return 0;
        }
    }
    return 1;
}

/* Record one frame's recon replay event (rec_out + recon_cb payload) in coding
 * order. All replays FIRE at stair_drain, on the API thread -- never from the
 * chain, which may be running on the driver (v3). Buffers recorded here must
 * stay untouched until the drain (leaf pinning / DPB residency). */
static void stair_record_replay(struct stair_burst *B, pixel *const pl[3], int disp)
{
    if (B->nreplay >= (int)(sizeof B->replay / sizeof B->replay[0]))
        return;                             /* can't happen: anchor + <= 7 B's + 1 */
    for (int c = 0; c < 3; c++) B->replay[B->nreplay].pl[c] = pl[c];
    B->replay[B->nreplay].disp = disp;
    B->nreplay++;
}

/* Record the anchor's replay exactly once, in coding order: before the first
 * B's. The first B of a burst always list-1-references the anchor, so its
 * completion implies the trailer published everything (pixels+borders final). */
static void stair_record_anchor_out(struct stair_burst *B)
{
    if (B->anchor_out_rec)
        return;
    B->anchor_out_rec = 1;
    stair_record_replay(B, B->f.rec, B->disp);
}

/* Stash one coded B's RBSP until the drain appends it (after the anchor NAL). */
static int stair_stash_nal(struct stair_burst *B, int ref_idc,
                           const uint8_t *rbsp, size_t len)
{
    if (B->stash_n >= 8)
        return -1;
    if (B->stash_len + len > B->stash_cap) {
        size_t cap = (B->stash_len + len) * 2;
        uint8_t *n = realloc(B->stash, cap);
        if (!n) return -1;
        B->stash = n; B->stash_cap = cap;
    }
    memcpy(B->stash + B->stash_len, rbsp, len);
    B->stash_item[B->stash_n].off = B->stash_len;
    B->stash_item[B->stash_n].len = len;
    B->stash_item[B->stash_n].ref_idc = ref_idc;
    B->stash_n++;
    B->stash_len += len;
    return 0;
}

/* Join the in-flight anchor's COMPUTE: wait for runner + trailer (and retire
 * the trailing B emit). No output side effects -- the NAL assembly happens at
 * stair_drain. Idempotent; from here the burst is serial (post-join B's code
 * serially into the stash). */
static int stair_join_compute(yah264_encoder_t *e, struct stair_burst *B)
{
    if (B->joined)
        return 0;
    if (stair_refb_join(e, B) < 0)          /* v5: a pipelined reference B */
        return -1;
    g_ew_site = 8;
    if (stair_bemit_drain(e, B) < 0)        /* retire the trailing B emit */
        return -1;
    if (B->async) {
        ntp_bg_sync(B->runner); ntp_bg_sync(B->trailer);
    } else {
        TPROF(TP_ANALYZE, { ntp_bg_sync(B->runner); ntp_bg_sync(B->trailer); });
    }
    B->joined = 1;
    if (B->size == 0)
        return -1;                          /* CAVLC overflow (mirrors serial) */
    stair_record_anchor_out(B);             /* no-op if a B already forced it */
    return 0;
}

/* Retire the fly burst on the API thread: wait for its chain, then emit
 * everything in coding order -- any pending prior W2 emit, the anchor's NAL,
 * the stashed B NALs, and the recon replay events (rec_out + recon_cb). */
/* Y264_ABR_EARLY=2: retire the ANCHOR half of a burst without retiring the
 * burst. The decide that follows needs actuals, not a finished chain, and the
 * two are not the same wait: B->size is final when the RUNNER returns, while
 * the chain driver additionally holds the mini-GOP's B leaves -- which is where
 * most of the tail lives (analyze_Bcb 8.3 s against analyze_Pcb 5.3 s of pool
 * time on park_joy).
 *
 * So this syncs the runner alone and fills its bits, and leaves the burst live
 * for the launch to overlap. The following decide then sees its predecessor's
 * ANCHOR exactly and only that burst's B's predicted -- which is the point,
 * because the anchor is the volatile term. A cut, a fade or a pan lands on the
 * anchor, and that is the content the RC lag mispriced on every clip it lost.
 *
 * IT BILLS THE LEDGER, IT DOES NOT EMIT. That split is the whole of queue item
 * 4b. This runs on the burst that is ABOUT to be left flying, so its anchor is
 * NEWER than the B's of the burst still undrained ahead of it; appending the
 * NAL here put that anchor in the bitstream in front of them. FrameNum is
 * claimed at prep time in plan order, so the stream then carried 7, 9, 8, 11,
 * 10 -- a gap on every hoisted anchor. gaps_in_frame_num_value_allowed_flag is
 * 0, so a decoder fills each gap with non-existing frames, which displaces the
 * real short-term references; the B's colocated picture (RefPicList1[0] under
 * spatial direct) is then not the one the encoder predicted from. bus_cif t12
 * ABR 400 read 46 of 60 frames failing recon-match, first at display 13, with
 * 29 "co located POCs unavailable" from the decoder -- against 0 of 60 with the
 * append moved back to the drain. Anchors survived it because they predict from
 * a picture that is still there; only the B's drift, and the error accumulates
 * from 31 dB to 16.
 *
 * The ledger order is deliberately NOT coding order and does not change here.
 * Moving only the append leaves every rate-control decision identical, so this
 * costs no bits: the same slices go out, in the order the decoder needs.
 *
 * Still a probe, and one hazard is now gone with the append: a chain error
 * raised after the runner returns no longer arrives with the anchor already in
 * the bitstream. */
static int stair_drain_anchor(yah264_encoder_t *e, size_t *off,
                              struct stair_burst *B)
{
    if (!B || !B->async || B->anchor_billed)
        return 0;
    ntp_bg_sync(B->runner);
    if (B->err || B->size == 0) {
        if (!B->err) slice_overflow_warn();
        return -1;
    }
    g_ew_site = 4;
    w2_flush(e, off);                       /* keeps the ledger in fill order */
    rcp_fill(e, 8.0 * (double)B->size);
    B->anchor_billed = 1;
    if (stair_stat_on()) e->st->stat_earlyfill++;
    return 0;
}

static int stair_drain(yah264_encoder_t *e, size_t *off)
{
    struct stair_ctx *st = e->st;
    struct stair_burst *B = stair_oldest(st);
    if (!B)
        return 0;
    stair_tr(st, (int)(B - st->bur), STE_DRAIN, (int)atomic_load(&B->seq), st->nlive);
    /* TP_STAIRJOIN, not the emit bucket: the chain DRIVER holds this burst's B
 * analyze, so what is waited on here is compute. Naming it after the drain
 * reads as emission time that is not there. */
    if (B->async)
        TPROF(TP_STAIRJOIN, ntp_bg_sync(stair_ch(st, B)->driver));
    stair_tr(st, (int)(B - st->bur), STE_DRAIN_E, (int)atomic_load(&B->seq), st->nlive);
    if (getenv("Y264_DIRECT_WHY"))
        fprintf(stderr, "ddrain seq=%u counts=%ld,%ld\n", atomic_load(&B->seq), B->dauto[0], B->dauto[1]);
    if (--st->nlive == 0)
        st->fly = NULL;
    atomic_store_explicit(&B->live, 0, memory_order_release);
                                            /* chain joined: its reads are done */
    if (B->col_restore)                     /* wide: deferred to here (see below) */
        stair_col_apply(e, B, (size_t)e->mv_stride * e->height_in_mbs * 4);
    if (B->err || B->size == 0) {
        if (!B->err) slice_overflow_warn();
        return -1;
    }
    int r;
    g_ew_site = 5;
    w2_flush(e, off);                       /* a pending prior emit precedes us */
    TPROF(TP_NAL, r = append_nal(e, off, 2, YAH264_NAL_SLICE, B->g.rbsp, B->size));
    if (r < 0)
        return -1;
    if (e->rcp_on && !B->anchor_billed)     /* else stair_drain_anchor billed it */
        rcp_fill(e, 8.0 * (double)B->size); /* anchor actuals, coding order */
    for (int k = 0; k < B->stash_n; k++) {
        TPROF(TP_NAL, r = append_nal(e, off, B->stash_item[k].ref_idc,
                                     YAH264_NAL_SLICE,
                                     B->stash + B->stash_item[k].off,
                                     B->stash_item[k].len));
        if (r < 0)
            return -1;
        if (e->rcp_on)
            rcp_fill(e, 8.0 * (double)B->stash_item[k].len);
    }
    B->stash_n = 0;
    B->stash_len = 0;
    for (int k = 0; k < B->nreplay; k++) {
        for (int c = 0; c < 3; c++) e->rec_out[c] = B->replay[k].pl[c];
        y264_note_emit(e, B->replay[k].disp);
        if (e->recon_cb) {
            yah264_picture_t rp;
            rp.csp = e->param.csp; rp.width = e->width; rp.height = e->height;
            rp.pts = 0;
            for (int c = 0; c < 3; c++) {
                rp.plane[c] = B->replay[k].pl[c];
                rp.stride[c] = e->pstride[c];
            }
            e->recon_cb(e->recon_ud, &rp, B->replay[k].disp, Y264_BIT_DEPTH);
        }
    }
    B->nreplay = 0;
    return 0;
}

static void stair_serial_wait_all(yah264_encoder_t *e);

/* Retire every live burst, oldest first. The call sites that want this are the
 * ones whose meaning is "nothing may be in flight past here": an IDR, a flush,
 * a shape that falls off the pipelined path, the rcp/VBV serial points.
 *
 * Under width AND rcp this needs the serial-phase exclusion first, and TSan is
 * what said so: the flush path's drain retires the OLDEST burst -- syncing that
 * burst's driver and no other -- while a NEWER chain is still in its prep phase
 * deciding its B's. Both ends touch the rcp FIFO, the drain through rcp_fill
 * and the prep through rcp_decide, and neither holds anything against the
 * other.
 *
 * This is coupling no other drain site pays. Every other one is excluded by
 * the launch induction (a launch runs only after the fly's serial_done, and
 * every older burst fired its own at an earlier launch), and CRF has no FIFO
 * for two threads to share. A flush has no launch in front of it, so it
 * inherits neither.
 *
 * The fix is that same exclusion widened rather than a lock, because a lock
 * would order the FIFO by whoever arrived first -- and an rcp fill that lands
 * on one side or the other of a decide can move a pop, which moves the ledger,
 * which moves bits. Waiting cannot: it writes nothing and every chain's prep
 * runs to the same place regardless. Only when a NEWER burst exists to race
 * with; draining the sole live burst syncs its own driver first. */
static int stair_drain_all(yah264_encoder_t *e, size_t *off)
{
    if (e->rcp_on && e->st && e->st->nlive > 1)
        stair_serial_wait_all(e);
    while (e->st && e->st->nlive)
        if (stair_drain(e, off) < 0)
            return -1;
    return 0;
}

/* Reference-B DPB commit, split in two (v4). BEGIN is the bookkeeping half --
 * dpb_store's eviction + slot selection (keep in sync), the plane swap, and
 * the FrameNum sequence -- everything a later prep's dpb_find / frame_num
 * observes. The async chain runs it at PREP time (mirroring stair_dpb_begin:
 * bookkeeping up front, content trails), so serial_done can fire before any B
 * encode; the sync flavour runs begin+content back-to-back at the old commit
 * site, which observes the identical DPB-state sequence. hpel_valid is a
 * promise, like the anchor's: pointers register against it, content lands
 * before any read (chain order for the burst's own B's; the next anchor's
 * launch holds on refb_done). */
static struct dpb_entry *stair_dpb_commit_begin(yah264_encoder_t *e,
                                                struct fpipe_leaf *L, int poc,
                                                struct stair_burst *self)
{
    int used = 0;
    for (int i = 0; i < e->dpb_size; i++) used += e->dpb[i].used;
    if (used >= e->sps.max_num_ref_frames) {
        int victim = -1, minfn = 0;
        for (int i = 0; i < e->dpb_size; i++)
            if (e->dpb[i].used && (victim < 0 || fn_wrap(e, e->dpb[i].frame_num) < minfn)) {
                victim = i; minfn = fn_wrap(e, e->dpb[i].frame_num);
            }
        if (victim >= 0) e->dpb[victim].used = 0;
    }
    int slot = -1;
    for (int i = 0; i < e->dpb_size; i++) if (!e->dpb[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;
    struct dpb_entry *d = &e->dpb[slot];
    /* The leaf's write target (already captured into L->f.rec) becomes the
 * slot's plane. Pooled, the leaf's next-burst target comes from the pool
 * and the slot's old buffers are parked; unpooled it is the old in-place
 * swap behind the reader wait. */
    if (!dpbp_recycle(e, d, L->rec)) {
        stair_slot_readers_wait(e, d, self);
        for (int c = 0; c < 3; c++) {
            pixel *t = d->plane[c]; d->plane[c] = L->rec[c]; L->rec[c] = t;
        }
    }
    d->hpel_valid = (e->hpel_on && d->hpel[0]) ? 1 : 0;
    L->commit_bag = dpbp_bag_of(d);
    d->col_l0poc0 = L->l0n > 0 ? L->l0poc[0] : -1;
    d->poc = poc;
    d->frame_num = e->next_frame_num;
    d->used = 1;
    e->last_ref_fn = e->next_frame_num;
    int maxfn = 1 << (e->sps.log2_max_frame_num_minus4 + 4);
    e->next_frame_num = (e->next_frame_num + 1) % maxfn;
    return d;
}

/* CONTENT half: the hpel planes and the colocated-motion resolve, from the
 * leaf's private grids and its captured POC set. No shared e-> state. */
static void stair_dpb_commit_content(yah264_encoder_t *e, struct fpipe_leaf *L,
                                     const struct dpb_bag *d, size_t mvcount)
{
    if (e->hpel_on && d->hpel[0]) {
        /* A parallel-for job on the shared pool; the in-flight anchor's ready
 * rows (few at a time -- the wavefront front) outrank it, the rest of
 * the workers take the bands. */
        hpel_build_ref_on(e, e->pool,
                          d->hpel[0], d->hpel[1], d->hpel[2], d->plane[0]);
    }
    struct w2_gen *G = &L->g;
    for (size_t i = 0; i < mvcount; i++) {
        if (G->refidx[i] >= 0) {
            d->mvx[i] = G->mvx[i]; d->mvy[i] = G->mvy[i]; d->refidx[i] = G->refidx[i];
            d->colpoc[i] = (int16_t)(G->refidx[i] < L->l0n
                                     ? L->l0poc[G->refidx[i]] : -1);
        } else if (G->refidx1[i] >= 0) {
            d->mvx[i] = G->mvx1[i]; d->mvy[i] = G->mvy1[i]; d->refidx[i] = G->refidx1[i];
            d->colpoc[i] = (int16_t)(L->l1poc0 >= 0 ? (L->l1poc0 | Y264_COLPOC_L1) : -1);
        } else {
            d->mvx[i] = 0; d->mvy[i] = 0;
            d->refidx[i] = -1;
            d->colpoc[i] = -1;
        }
    }
}

/* Serial per-B setup into the stair leaf, in coding order -- fpipe_prep_leaf's
 * shape, with three staircase differences: the colocated field of an IN-FLIGHT
 * anchor is read in place (row-gated) instead of copied; e->col* is NEVER
 * written (the anchor's analyze is concurrently reading it as its temporal
 * seeds); and the analyze row-gate is armed when list-1 is the anchor.
 * Returns 1 ok, 0 = demand the serial fallback (shared hpel). */
static int stair_prep_b(yah264_encoder_t *e, struct stair_burst *B,
                        struct fpipe_leaf *L, int m,
                        int depth, int is_ref, size_t mvcount)
{
    struct stair_ctx *st = e->st;
    int l0 = dpb_find(e, B->bpoc[m], 0), l1 = dpb_find(e, B->bpoc[m], 1);
    for (int c = 0; c < 3; c++) e->ref[c] = e->dpb[l0].plane[c];
    for (int c = 0; c < 3; c++) e->cur_l1p[c] = e->dpb[l1].plane[c];
    e->ref0_poc = e->dpb[l0].poc;
    e->ref1_poc = e->dpb[l1].poc;
    e->colframepoc = e->dpb[l1].poc;
    int l1_inflight = (&e->dpb[l1] == B->slot);
    /* v4 async two-phase: all preps precede all encodes, so list-1 may be a
 * burst reference-B whose bookkeeping is done (commit_begin at its prep)
 * but whose CONTENT hasn't landed yet. Its colmv must then be read IN
 * PLACE at analyze time -- a prep-time copy would capture stale data. No
 * row gate needed: the chain runs this B strictly after that ref-B's
 * content commit. commit_slot is only ever non-NULL mid-prep-phase, so
 * the sync flavour (and post-content preps) take the copy as before. */
    int l1_pend = 0;
    struct stair_chain *C = stair_ch(st, B);         /* only THIS burst's leaves
 * can hold a pending
 * commit its own preps
 * reference; another
 * chain's are stale by K
 * launches */
    for (int k = 0; !l1_inflight && k < C->nleaf; k++)
        if (C->leaf[k] && C->leaf[k]->commit_slot == &e->dpb[l1])
            l1_pend = 1;
    if (!l1_inflight && !l1_pend) {
        memcpy(L->colmvx, e->dpb[l1].mvx, mvcount * sizeof(int16_t));
        memcpy(L->colmvy, e->dpb[l1].mvy, mvcount * sizeof(int16_t));
        memcpy(L->colref, e->dpb[l1].refidx, mvcount);
        memcpy(L->colpoc, e->dpb[l1].colpoc, mvcount * sizeof(int16_t));
        L->col_l0poc0 = e->dpb[l1].col_l0poc0;
    }
    /* No stair prep ever writes e->col* (the anchor's analyze reads it); the
 * burst end restores it from the LAST B's list-1 slot -- what the serial
 * path's final set_b_refs would have left. */
    B->col_restore = &e->dpb[l1];
    B->col_bag = dpbp_bag_of(&e->dpb[l1]);
    e->poc = B->bpoc[m];
    e->cur_disp = B->bdisp[m];
    e->cur_lr_motion = B->bmotion[m]; e->cur_lr_tdiff = B->btdiff[m];
    e->cur_bseed = m;
    e->cur_b_depth = depth;
    e->frame_num = is_ref ? e->next_frame_num : e->last_ref_fn;
    /* emit_frame's RC head (CQP/CRF, or ABR/2-pass under rcp; see
 * fpipe_prep_leaf). The ASYNC chain skips the CRF flavour: it runs on the
 * driver while new arrivals rewrite the lowres fields it reads, and in
 * CQP/CRF nothing it computes reaches the bitstream -- bits identical,
 * race removed. The rcp decide RUNS in async too: it reads only the
 * burst-captured complexity + rcp state, whose driver-side accesses the
 * serial_done / chain-submit handshakes order. */
    if (e->rcp_on) {
        e->rcp_cur_cme = B->bcplx[m];
        e->rcp_cur_cvi = B->bcvi[m];
        rcp_decide(e, 2, is_ref, NULL);
    } else if (e->crf_on && !B->async) {
        double C = frame_complexity(e, B->bplane[m], 0);
        e->rc_cplx = C;
        double Ccrf = frame_complexity_me(e);
        if (getenv("Y264_DBG_CPLX"))
            fprintf(stderr, "CPLX type=%d C=%.0f permb=%.1f  Cme=%.0f permb=%.1f\n", 2,
                    C, C / (e->width_in_mbs * e->height_in_mbs),
                    Ccrf, Ccrf / (e->width_in_mbs * e->height_in_mbs));
        rc_set_qp_crf(e, Ccrf, 2);
    }
    struct frame_work fw;
    fw_default(e, &fw);
    fw.mbtoff_b = (is_ref && m >= 0 && m < 8 && B->bmbtoff_valid[m])
                ? B->bmbtoff[m] : NULL;                 /* burst-owned */
    fw.refb_poc = stair_refb_poc(B->nbuf, B->bpoc);  /* the BURST's shape:
 * arrivals rewrite e->nbuf
 * while this chain preps */
    for (int c = 0; c < 3; c++) fw.rec[c] = L->rec[c];
    for (int c = 0; c < 3; c++) fw.ref1[c] = e->cur_l1p[c];
    if (l1_inflight) {                      /* read the streaming field in place */
        fw.colmvx = B->slot->mvx;  fw.colmvy = B->slot->mvy;
        fw.colref = B->slot->refidx;
        fw.colpoc = B->slot->colpoc;
        fw.col_l0poc0 = B->slot->col_l0poc0;
    } else if (l1_pend) {                   /* burst ref-B: in place, chain-ordered */
        fw.colmvx = e->dpb[l1].mvx;  fw.colmvy = e->dpb[l1].mvy;
        fw.colref = e->dpb[l1].refidx;
        fw.colpoc = e->dpb[l1].colpoc;
        fw.col_l0poc0 = e->dpb[l1].col_l0poc0;
    } else {
        fw.colmvx = L->colmvx; fw.colmvy = L->colmvy;
        fw.colref = L->colref; fw.colpoc = L->colpoc;
        fw.col_l0poc0 = L->col_l0poc0;
    }
    fw.col_l0poc0_valid = 1;
    fw.hpel_ctx = L->hpel_ctx;
    fw.bseed_cur = L->bseed_cur;
    fw.refidx = L->g.refidx; fw.refidx1 = L->g.refidx1;
    fw.bseed_src = (int16_t *const (*)[4])B->bseed;     /* the burst's captured bank */
    fw.bseedc_src = NULL;                               /* t1-only measurement */
    fw.bseed_valid = B->bseed_valid;
    fw.bseed_poc0 = B->bseed_poc0;
    fw.bseed_poc1 = B->bseed_poc1;
    fw.dauto_acc = B->dauto;                 /* the burst's counts, decided at launch */
    fw.dauto_valid = direct_auto_armed(e);
    fw.dauto_temporal = B->dauto_temporal;
    build_slice_prep(e, 2, 0, is_ref, B->bplane[m], L->g.rbsp, e->rbsp_cap, &fw,
                     &L->bs, &L->f, &L->fqp, &L->dblk);
    e->cur_bseed = -1;
    if (!fpipe_hpel_private(e, &L->f))
        return 0;
    L->l0n = e->cur_l0n;
    L->l1poc0 = e->cur_l1poc0;
    for (int i = 0; i < L->l0n; i++) L->l0poc[i] = e->cur_l0poc[i];
    /* Measurement (see stat_l0sel): a leaf's list 0 reaches back past its own
 * mini-GOP, so the bursts its LAUNCH waited on have to be asked the same
 * question here. Anything this counts as `sel` is a read the anchor's own
 * list 0 did not cover. */
    if (stair_stat_on())
        for (int i = 0; i < B->probe_nsel; i++)
            atomic_fetch_add_explicit(
                stair_l0_reads_refb(B->probe_sel[i], L->l0poc, L->l0n)
                    ? &st->stat_l0leafsel : &st->stat_l0leafskip,
                1, memory_order_relaxed);
    y264_frame_t *f = &L->f;
    struct w2_gen *G = &L->g;
    f->nnz[0] = G->nnz[0]; f->nnz[1] = G->nnz[1]; f->nnz[2] = G->nnz[2];
    f->i4mode = G->i4mode; f->mbcbp = G->mbcbp;
    f->mvx = G->mvx; f->mvy = G->mvy; f->mvx1 = G->mvx1; f->mvy1 = G->mvy1;
    f->refidx = G->refidx; f->refidx1 = G->refidx1;
    f->mvdx = G->mvdx; f->mvdy = G->mvdy; f->mvdx1 = G->mvdx1; f->mvdy1 = G->mvdy1;
    if (f->mb_tr8) f->mb_tr8 = G->mb_tr8;
    if (f->aq_off) f->aq_off = G->aq_off;
    if (f->mbqp)   f->mbqp = L->mbqp;
    f->pool = L->pool;
    L->gate.n = 0;
    L->gate.rbidx = -1;
    if (l1_inflight)                        /* gate rows on the anchor's publish */
        L->gate.p[L->gate.n++] = &B->P;
    /* v5: and on the burst reference B's, in whichever list resolves to it --
 * list 1 for the earlier leaf (its nearest future picture), list 0 for the
 * later one (its nearest past). Same LAG budget, same fixed clamp. */
    if (C->refb_slot && (&e->dpb[l1] == C->refb_slot || &e->dpb[l0] == C->refb_slot))
        L->gate.p[L->gate.n++] = &C->rprog;
    if (L->gate.n) {
        f->row_gate = stair_row_gate;       /* blocking (serial fallback) */
        f->row_ready = stair_row_ready;     /* non-blocking (pool claim gate) */
        f->row_gate_ctx = &L->gate;
    }
    /* DPB-eviction safety (v3): record every reference plane this B reads, so
 * a later anchor's slot selection can tell whether the victim's buffer is
 * still being read by this (possibly in-flight) burst. */
    for (int i = 0; i < f->nref && B->nread >= 0; i++)
        B->nread < (int)(sizeof B->readset / sizeof B->readset[0])
            ? (void)(B->readset[B->nread++] = f->refs[i][0]) : (void)(B->nread = -1);
    if (B->nread >= 0)
        B->nread < (int)(sizeof B->readset / sizeof B->readset[0])
            ? (void)(B->readset[B->nread++] = f->ref1[0]) : (void)(B->nread = -1);
    L->cabac = e->pps.entropy_coding_mode_flag ? 1 : 0;
    if (L->cabac) {
        while (y264_bs_pos_bits(&L->bs) & 7)
            y264_bs_write1(&L->bs, 1);      /* cabac_alignment_one_bit */
        y264_cabac_init_engine(&L->cb, L->bs.p); y264_cabac_set_end(&L->cb, L->bs.end - 64);
        y264_cabac_init_contexts(&L->cb, 2, 0, L->fqp);
        f->cabac = &L->cb;
    }
    L->disp = e->cur_disp;
    L->size = 0;
    return 1;
}

/* Chain-side TPROF: the profiling buckets are unsynchronised main-thread
 * accumulators, so the async chain (driver thread) skips them. */
#define STPROF(B, bucket, ...) do { \
    if ((B)->async) { __VA_ARGS__; } else { TPROF(bucket, __VA_ARGS__); } \
} while (0)

/* Post-join serial B emit, stash flavour: build_slice's serial encode (byte-
 * identical to emit_frame's core -- only CQP/CRF engage the staircase, so the
 * RC tail is empty), with the NAL stashed and the recon replay recorded
 * instead of appended/fired: the chain may be running on the driver (v3),
 * where e->out / e->nal / recon_cb belong to the API thread. e->rbsp and the
 * shared grids are free here (the anchor codes into its private generation). */
static int stair_emit_stash(yah264_encoder_t *e, struct stair_burst *B,
                            int is_ref, pixel *const src[3])
{
    if (e->crf_on && !B->async) {           /* emit_frame's RC head (see prep) */
        double C = frame_complexity(e, src, 0);
        e->rc_cplx = C;
        double Ccrf = frame_complexity_me(e);
        if (getenv("Y264_DBG_CPLX"))
            fprintf(stderr, "CPLX type=%d C=%.0f permb=%.1f  Cme=%.0f permb=%.1f\n", 2,
                    C, C / (e->width_in_mbs * e->height_in_mbs),
                    Ccrf, Ccrf / (e->width_in_mbs * e->height_in_mbs));
        rc_set_qp_crf(e, Ccrf, 2);
    }
    size_t sz = build_slice(e, 2, 0, is_ref, src);
    if (sz == 0)
        return -1;
    STPROF(B, TP_BORDERS, extend_borders(e, e->rec));
    if (stair_stash_nal(B, is_ref ? 2 : 0, e->rbsp, sz) < 0)
        return -1;
    stair_record_replay(B, e->rec, e->cur_disp);
    return 0;
}

/* One B of the burst, post-join serial flavour: code_b_hier's body, stashed. */
static int stair_serial_b(yah264_encoder_t *e, struct stair_burst *B,
                          int m, int depth, int is_ref, size_t mvcount)
{
    int l0, l1;
    set_b_refs(e, B->bpoc[m], mvcount, &l0, &l1);
    e->poc = B->bpoc[m];
    e->cur_disp = B->bdisp[m];
    e->cur_lr_motion = B->bmotion[m]; e->cur_lr_tdiff = B->btdiff[m];
    e->cur_bseed = m;
    e->cur_b_depth = depth;
    e->frame_num = is_ref ? e->next_frame_num : e->last_ref_fn;
    if (e->rcp_on) {                    /* fresh decide, or consume a bailed prep's */
        e->rcp_cur_cme = B->bcplx[m];
        e->rcp_cur_cvi = B->bcvi[m];
        rcp_head(e, 2, is_ref, NULL);
    }
    int r = stair_emit_stash(e, B, is_ref, B->bplane[m]);
    e->cur_bseed = -1;
    if (r < 0) return -1;
    if (is_ref)
        STPROF(B, TP_DPBSTORE, dpb_store(e, B->bpoc[m], mvcount));
    B->col_restore = NULL;          /* set_b_refs refreshed e->col* itself */
    return 0;
}

/* v4 handshake: the async chain fires this once its PREP PHASE -- every B's
 * serial prep plus each reference-B's DPB bookkeeping -- is done, BEFORE any B
 * encode. From then on the driver makes no shared e-> write the next anchor's
 * arrival phase observes (the DPB slot content it still streams is held off
 * arrival-side reads by the launch's refb_done wait). First signaller wins so
 * col_src stays stable once published; the chain end re-fires unconditionally
 * to cover the bail paths. */
static void stair_serial_fire(yah264_encoder_t *e, struct stair_burst *B)
{
    struct stair_chain *C = stair_ch(e->st, B);
    pthread_mutex_lock(&C->smx);
    if (!C->serial_done) {
        B->col_src = B->col_restore;
        C->serial_done = 1;
        pthread_cond_signal(&C->scv);
    }
    pthread_mutex_unlock(&C->smx);
}

/* Block until the running chain's serial (prep) phase is done: from then on
 * the driver makes no shared e-> write the next anchor's serial prep could
 * observe. No-op when nothing async is in flight. */
static void stair_serial_wait(yah264_encoder_t *e)
{
    struct stair_ctx *st = e->st;
    if (!st || !st->fly || !st->fly->async)
        return;
    struct stair_chain *C = stair_ch(st, st->fly);   /* the chain this waits ON */
    double t0 = stair_stat_on() ? tprof_ms() : 0.0;
    pthread_mutex_lock(&C->smx);
    while (!C->serial_done)
        pthread_cond_wait(&C->scv, &C->smx);
    pthread_mutex_unlock(&C->smx);
    if (stair_stat_on()) {
        st->stat_swait_ms += tprof_ms() - t0;
        st->stat_n++;
    }
}

/* The same wait over EVERY live burst, for a drain that has no launch in front
 * of it to have excluded them (see stair_drain_all). The fly is the newest, so
 * waiting on it alone is exactly what leaves a mid-prep predecessor uncovered
 * -- and under width the predecessor is the one still holding a chain.
 *
 * No cycle to worry about: a chain's prep only ever waits on bursts launched
 * BEFORE it, never on the API thread, so every prep this
 * blocks on can finish without anything from here. */
static void stair_serial_wait_all(yah264_encoder_t *e)
{
    struct stair_ctx *st = e->st;
    if (!st)
        return;
    for (int b = 0; b < Y264_STAIR_K; b++) {
        struct stair_burst *B = &st->bur[b];
        if (!atomic_load_explicit(&B->live, memory_order_acquire) || !B->async)
            continue;
        struct stair_chain *C = stair_ch(st, B);
        pthread_mutex_lock(&C->smx);
        while (!C->serial_done)
            pthread_cond_wait(&C->scv, &C->smx);
        pthread_mutex_unlock(&C->smx);
    }
}

/* Trailing B entropy emit (on bemit): W2's overlap inside the burst -- writes
 * only the leaf's own bitstream/engine (plus same-value grid rewrites on the
 * leaf's PRIVATE grids in the CAVLC intra path), so the next B's analyze on
 * the other leaf runs concurrently. */
static void stair_bemit_task(void *arg)
{
    struct fpipe_leaf *L = arg;
    y264_frame_emit(&L->bs, &L->f, L->job);
    L->job = NULL;
    if (L->cabac) {
        if (!L->cb.overflow) cabac_zero_words(&L->f, &L->cb, L->bs.end);
        L->size = L->cb.overflow ? 0 : (size_t)(L->cb.p - L->bs.start);
    } else {
        y264_bs_rbsp_trailing(&L->bs);
        L->size = L->bs.overflow ? 0 : (size_t)(L->bs.p - L->bs.start);
    }
}

/* Retire the in-flight B emit: wait, then stash its NAL (coding order). */
static int stair_bemit_drain(yah264_encoder_t *e, struct stair_burst *B)
{
    struct stair_chain *C = stair_ch(e->st, B);
    struct fpipe_leaf *L = C->pend;
    if (!L)
        return 0;
    C->pend = NULL;
    STPROF(B, TP_EMITWAIT, ntp_bg_sync(C->bemit));   /* no site accounting here: runs on the chain driver */
    if (L->size == 0)
        { slice_overflow_warn(); return -1; }                              /* CAVLC overflow */
    return stair_stash_nal(B, L->ref_idc, L->g.rbsp, L->size);
}

/* The ENCODE half of one burst B, from a prepped leaf: analyze as a job on the
 * shared pool (rows gated on the anchor when list-1 is the anchor -- workers
 * whose claim is gated serve the anchor's job) + deblock, recon replayed in
 * coding order, reference-B's committed; the entropy emit TRAILS on bemit into
 * the next B's analyze (W2's overlap, inside the burst), its NAL stashed at
 * the next drain in coding order. Reads only leaf-captured + burst state --
 * no shared e-> scalar the arrival phase could observe (the DPB content
 * commit is covered by the refb_done handshake). f->rec, not L->rec: for a
 * v4 ref-B the commit_begin at prep already swapped L->rec away. */
static int stair_run_b(yah264_encoder_t *e, struct stair_burst *B,
                       struct fpipe_leaf *L, int m,
                       int is_ref, size_t mvcount)
{
    struct stair_ctx *st = e->st;
    y264_frame_t *f = &L->f;
    y264_me_set_hpel((const y264_hpel_ref_t *)f->hpel_ctx, f->hpel_n, f->hpel_stride);
    y264_emit_job_t *job;
    STPROF(B, TP_ANALYZE, job = y264_frame_analyze(f));
    if (L->dblk)
        STPROF(B, TP_DEBLOCK, y264_deblock_frame(f));
    /* Recon replay, coding order (recorded here; FIRED at the drain, on the
 * API thread). The FIRST B of a burst always list-1-references the anchor,
 * so its completion implies the trailer published everything (its last row
 * gated on pub == hmb) -> the anchor's replay is due now. */
    if (stair_refb_join(e, B) < 0)          /* v5: its NAL precedes this leaf's */
        return -1;
    stair_record_anchor_out(B);
    stair_record_replay(B, f->rec, L->disp);
    g_ew_site = 9;
    if (stair_bemit_drain(e, B) < 0)        /* previous B's NAL, coding order */
        return -1;
    L->job = job;
    L->ref_idc = is_ref ? 2 : 0;
    struct stair_chain *C = stair_ch(st, B);
    ntp_bg_submit(C->bemit, stair_bemit_task, L);
    C->pend = L;
    if (is_ref) {
        /* A reference B is consumed by the burst's remaining frames: retire
 * its emit NOW (the CAVLC intra emit path rewrites the leaf's motion
 * grids -- same values -- which the commit below reads), extend its
 * recon borders (it serves as an MC reference; non-ref leaves skip
 * this, their buffer is never read again) and commit it to the DPB. */
        g_ew_site = 10;
        if (stair_bemit_drain(e, B) < 0)
            return -1;
        STPROF(B, TP_BORDERS, extend_borders(e, f->rec));
        STPROF(B, TP_DPBSTORE, {
            if (!L->commit_slot)                /* sync: bookkeeping now */
                stair_dpb_commit_begin(e, L, B->bpoc[m], B);
            stair_dpb_commit_content(e, L, &L->commit_bag, mvcount);
        });
        if (L->commit_slot) {
            /* v4: content landed -- release arrival-side readers (the next
 * anchor's launch waits on refb_done under pmx/pcv). */
            L->commit_slot = NULL;
            pthread_mutex_lock(&B->P.pmx);
            atomic_fetch_add_explicit(&B->refb_done, 1, memory_order_release);
            pthread_cond_broadcast(&B->P.pcv);
            pthread_mutex_unlock(&B->P.pmx);
        }
    }
    return 0;
}

/* v5 reference-B runner: the anchor runner's shape, on a leaf. Its analyze is a
 * gated job on the shared pool (its own list-1 is the in-flight anchor), the
 * per-row hooks feed its trailer, and the entropy emit waits for that trailer
 * to finish -- the CAVLC intra path rewrites the motion field the trailer's bS
 * and colmv reads walk. Everything it touches is leaf-private plus its DPB
 * slot's content, so it overlaps its own leaves freely. */
static void stair_refb_runner_task(void *arg)
{
    /* The CHAIN, not the encoder: finding the leaf through e->st->refb_pipe
 * does not work once the pipeline is per chain, because that pointer stops
 * naming one thing. Submitting the chain it belongs to leaves nothing to
 * resolve. */
    struct stair_chain *C = arg;
    struct fpipe_leaf *L = C->refb_pipe;
    y264_frame_t *f = &L->f;
    y264_me_set_hpel((const y264_hpel_ref_t *)f->hpel_ctx, f->hpel_n, f->hpel_stride);
    y264_emit_job_t *job = y264_frame_analyze(f);
    stair_arow_finish(&C->rprog, f->hmb);
    stair_pub_wait_all(&C->rprog, f->hmb);
    y264_frame_emit(&L->bs, f, job);
    if (L->cabac) {
        if (!L->cb.overflow) cabac_zero_words(&L->f, &L->cb, L->bs.end);
        L->size = L->cb.overflow ? 0 : (size_t)(L->cb.p - L->bs.start);
    } else {
        y264_bs_rbsp_trailing(&L->bs);
        L->size = L->bs.overflow ? 0 : (size_t)(L->bs.p - L->bs.start);
    }
}

/* Launch the burst's reference B into its own pipeline and RETURN: its rows
 * become consumable one at a time (deblock -> borders -> DPB hpel band ->
 * colmv -> publish), so the leaves that follow in coding order start against
 * its watermark instead of its completion. The DPB bookkeeping already ran at
 * its prep (v4's commit_begin); the trailer streams the content and fires the
 * burst's refb_done when it lands. */
static void stair_launch_refb(yah264_encoder_t *e, struct stair_burst *B,
                              struct fpipe_leaf *L)
{
    struct stair_chain *C = stair_ch(e->st, B);
    L->f.row_done = stair_row_done;
    L->f.row_done_ctx = &C->rprog;
    C->refb_pipe = L;
    ntp_bg_submit(C->btrailer, stair_trailer_task, &C->rprog);
    ntp_bg_submit(C->brunner, stair_refb_runner_task, C);
}

/* Retire the pipelined reference B, in coding order: it precedes every leaf of
 * its mini-GOP, so its NAL and recon replay are recorded before theirs (the
 * callers below join it just before their own stash). Its content commit and
 * the refb_done fire happened inside the trailer. */
static int stair_refb_join(yah264_encoder_t *e, struct stair_burst *B)
{
    struct stair_chain *C = stair_ch(e->st, B);
    struct fpipe_leaf *L = C->refb_pipe;
    if (!L)
        return 0;
    /* Clear only AFTER the syncs: the runner task reads C->refb_pipe when it
 * picks the task up, which may not have happened yet. */
    STPROF(B, TP_ANALYZE, {
        ntp_bg_sync(C->brunner);
        ntp_bg_sync(C->btrailer);
    });
    C->refb_pipe = NULL;
    C->refb_slot = NULL;
    L->commit_slot = NULL;                  /* the trailer committed the content */
    if (L->size == 0)
        { slice_overflow_warn(); return -1; }                          /* CAVLC overflow (mirrors serial) */
    /* The reference B's last row gated on the anchor's full publish, so the
 * anchor's replay is due now (coding order: anchor, ref B, leaves). */
    stair_record_anchor_out(B);
    if (stair_stash_nal(B, 2, L->g.rbsp, L->size) < 0)
        return -1;
    stair_record_replay(B, L->f.rec, L->disp);
    return 0;
}

/* One B of the burst, sync flavour: serial prep + encode, interleaved (the v3
 * shape -- the ping-pong leaf frees itself via the trailing-emit drain inside
 * the run). */
static int stair_encode_b(yah264_encoder_t *e, struct stair_burst *B,
                          int m, int depth, int is_ref, size_t mvcount)
{
    struct stair_ctx *st = e->st;
    if (B->joined)
        return stair_serial_b(e, B, m, depth, is_ref, mvcount);
    struct stair_chain *C = stair_ch(st, B);
    struct fpipe_leaf *L = C->leaf[C->lparity];
    if (!stair_prep_b(e, B, L, m, depth, is_ref, mvcount)) {
        /* shared-hpel bail (defensive; cannot fire for pyramid DPB refs): the
 * sync chain joins and codes this B serially (the aborted prep's e->
 * writes are all redone); the async chain fails loudly instead -- its
 * serial fallback would read the ARRIVAL-side seed bank. */
        if (B->async)
            return -1;
        if (stair_join_compute(e, B) < 0)
            return -1;
        if (e->rcp_on)
            e->rcp_predecided = 1;      /* the bailed prep already decided */
        return stair_serial_b(e, B, m, depth, is_ref, mvcount);
    }
    C->lparity ^= 1;
    return stair_run_b(e, B, L, m, is_ref, mvcount);
}

/* The sibling non-ref leaf pair, inside the burst (Lever 2 folded into the
 * staircase): both leaves encode concurrently as jobs on the shared pool --
 * the serial stair path would otherwise LOSE the pair overlap the default
 * (FPIPE) path has. Preps run serially in coding order; leaf 1's
 * analyze+emit+deblock task rides the (idle, just-drained) bemit thread while
 * leaf 0 runs inline. The leaves never read the in-flight anchor (their
 * list-1 is the mini-GOP's reference B, committed before this), so no row
 * gate arms; they overlap the anchor's trailing emit and each other. NALs are
 * stashed in coding order; recon replays likewise. By construction the pair
 * follows its reference-B parent, whose completion fired the anchor replay. */
/* The ENCODE half of a prepped sibling pair: leaf 1's analyze+emit+deblock
 * task rides the just-drained bemit thread while leaf 0 runs inline; both
 * analyzes are concurrent jobs on the shared pool. The pending prior B's NAL
 * is stashed first (coding order) -- that drain also frees the bemit thread. */
static int stair_run_pair(yah264_encoder_t *e, struct stair_burst *B,
                          struct fpipe_leaf *L0, struct fpipe_leaf *L1)
{
    struct stair_chain *C = stair_ch(e->st, B);
    g_ew_site = 11;
    if (stair_bemit_drain(e, B) < 0)
        return -1;
    ntp_bg_submit(C->bemit, fpipe_leaf_task, L1);
    STPROF(B, TP_ANALYZE, fpipe_leaf_task(L0));
    ntp_bg_sync(C->bemit);
    if (stair_refb_join(e, B) < 0)      /* v5: its NAL precedes the pair's */
        return -1;
    for (int k = 0; k < 2; k++) {
        struct fpipe_leaf *L = k ? L1 : L0;
        if (L->size == 0)
            { slice_overflow_warn(); return -1; }                  /* CAVLC overflow: mirrors serial ret 0 */
        if (stair_stash_nal(B, 0, L->g.rbsp, L->size) < 0)
            return -1;
        stair_record_replay(B, L->f.rec, L->disp);
    }
    return 0;
}

static int stair_encode_pair(yah264_encoder_t *e, struct stair_burst *B,
                             int m0, int m1, int depth, size_t mvcount)
{
    struct stair_chain *C = stair_ch(e->st, B);
    g_ew_site = 12;
    if (stair_bemit_drain(e, B) < 0)    /* both leaf contexts must be free */
        return -1;
    struct fpipe_leaf *L0 = C->leaf[0], *L1 = C->leaf[1];
    int p0 = stair_prep_b(e, B, L0, m0, depth, 0, mvcount);
    int p1 = p0 ? stair_prep_b(e, B, L1, m1, depth, 0, mvcount) : 0;
    if (!p0 || !p1) {
        /* shared-hpel bail (defensive; see stair_encode_b). */
        if (B->async)
            return -1;
        if (stair_join_compute(e, B) < 0)
            return -1;
        if (e->rcp_on)
            e->rcp_predecided = p0 ? 2 : 1;
        if (stair_serial_b(e, B, m0, depth, 0, mvcount) < 0)
            return -1;
        return stair_serial_b(e, B, m1, depth, 0, mvcount);
    }
    C->lparity = 0;
    return stair_run_pair(e, B, L0, L1);
}

/* code_b_hier's recursion, staircase flavour. b-a==3 (both children single
 * non-ref leaves) runs them as the concurrent pair. */
static int stair_b_hier(yah264_encoder_t *e, struct stair_burst *B,
                        int a, int b, int depth, size_t mvcount)
{
    if (a >= b) return 0;
    int m = (a + b) / 2;
    int is_ref = (m > a) || (m + 1 < b);
    if (stair_encode_b(e, B, m, depth, is_ref, mvcount) < 0) return -1;
    if (b - a == 3 && !B->joined)
        return stair_encode_pair(e, B, a, a + 2, depth + 1, mvcount);
    if (stair_b_hier(e, B, a, m, depth + 1, mvcount) < 0) return -1;
    if (stair_b_hier(e, B, m + 1, b, depth + 1, mvcount) < 0) return -1;
    return 0;
}

/* The same recursion flattened into coding order (keep in sync with
 * stair_b_hier / code_b_hier): the v4 async chain preps every entry first,
 * then encodes them. pair == 1 marks the first of a concurrent sibling pair
 * (the next entry is its sibling). */
static int stair_plan_hier(struct stair_bent *pl, int n, int a, int b, int depth)
{
    if (a >= b) return n;
    int m = (a + b) / 2;
    int is_ref = (m > a) || (m + 1 < b);
    pl[n++] = (struct stair_bent){ (int8_t)m, (int8_t)depth, (int8_t)is_ref, 0 };
    if (b - a == 3) {
        pl[n++] = (struct stair_bent){ (int8_t)a, (int8_t)(depth + 1), 0, 1 };
        pl[n++] = (struct stair_bent){ (int8_t)(a + 2), (int8_t)(depth + 1), 0, 2 };
        return n;
    }
    n = stair_plan_hier(pl, n, a, m, depth + 1);
    return stair_plan_hier(pl, n, m + 1, b, depth + 1);
}

/* How many of a full mini-GOP's B's the pyramid marks as references -- the DPB
 * window term (SPS sizing) and the bag-pool term (dpbp_open) both read the
 * coding plan so they cannot drift from it. */
static int stair_plan_nrefb(int bframes)
{
    if (bframes < 2) return 0;                    /* no pyramid (b_pyramid gate) */
    struct stair_bent plan[8];
    int np = stair_plan_hier(plan, 0, 0, bframes > 8 ? 8 : bframes, 1);
    int n = 0;
    for (int i = 0; i < np; i++) n += plan[i].is_ref;
    return n;
}

/* The serial path's last set_b_refs leaves e->col* holding the last B's list-1
 * colocated field, which the NEXT anchor's analyze reads as its temporal ME
 * seeds. The stair preps never write e->col* (the anchor was reading it), so
 * the burst restores it from the slot its last prep named, once that slot's
 * content is complete. (A v3 anchor overlapping this reads the SOURCE SLOT
 * instead -- see col_src.) */
static void stair_col_apply(yah264_encoder_t *e, struct stair_burst *B,
                            size_t mvcount)
{
    const struct dpb_bag *g = &B->col_bag;
    memcpy(e->colmvx, g->mvx, mvcount * sizeof(int16_t));
    memcpy(e->colmvy, g->mvy, mvcount * sizeof(int16_t));
    memcpy(e->colref, g->refidx, mvcount);
    memcpy(e->colpoc, g->colpoc, mvcount * sizeof(int16_t));
    B->col_restore = NULL;
}

/* Allocate the picture-buffer pool, once, from yah264_encoder_open.
 *
 * Only where Y264_STAIR_WIDE can engage: nothing else recycles a DPB slot under
 * a live reader, so every other shape keeps the wait it already had and pays no
 * memory. The condition is the one stair_run_burst gates `wide` on, read here
 * so it is one statement of the rule rather than two.
 *
 * SIZE. A parked bag is released once every burst live when it was parked has
 * drained, and at most K bursts are live at once, so the ceiling is the
 * retirements those K bursts can make: one anchor recycle at each launch plus
 * one per reference B the burst commits. The reference-B count comes from the
 * coding plan itself rather than from `bframes` -- the pyramid makes internal
 * nodes references and leaves not, so 7 B's commit 3 references, not 7, and
 * sizing on bframes would have over-allocated by a third at the widest shape.
 *
 * K*(1+nrefb) is 6 at bframes 2 and 3, 12 at bframes 7. Measured high-water
 * across 3 clips x bframes 2/3/7 x keyint 30/60/250 x t8/t18 x BDEPTH on/off is
 * 5, 5 and 11 -- one below the ceiling in every case, because the ceiling counts
 * the bag a recycle is about to take and the peak is read after it is parked.
 * The bound is tight, which is the reason to keep the exhaustion fallback
 * rather than to trust the arithmetic.
 *
 * Never grown afterwards. A realloc under concurrent chains is the defect this
 * codebase already tracks in hpel_ensure_ws, and one of those is enough. */
static void dpbp_open(yah264_encoder_t *e, size_t mvcount)
{
    if (!stair_wide_on() || !stair_wide_nref_ok(e) || !stair_wide_rc_ok(e))
        return;
    int nrefb = stair_plan_nrefb(e->param.bframes);
    int want = Y264_STAIR_K * (1 + nrefb);
    /* Y264_DPB_POOL caps the pool below its derived size. The exhaustion
 * fallback is otherwise unreachable -- 0 exhaustions on every shape
 * measured -- and an untested fallback is not a floor, it is a guess. At 1
 * nearly every recycle takes it. */
    { const char *v = getenv("Y264_DPB_POOL");
      if (v) { int n = atoi(v); if (n >= 0 && n < want) want = n; } }
    if (want > Y264_DPB_POOL_MAX) want = Y264_DPB_POOL_MAX;
    for (int i = 0; i < want; i++) {
        struct dpb_bag *g = &e->dpbp_free[i];
        g->plane[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
        g->plane[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                                  Y264_CHROMA_BORDER);
        g->plane[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                                  Y264_CHROMA_BORDER);
        g->mvx = malloc(mvcount * sizeof(int16_t));
        g->mvy = malloc(mvcount * sizeof(int16_t));
        g->refidx = malloc(mvcount);
        g->colpoc = malloc(mvcount * sizeof(int16_t));
        int ok = g->plane[0] && g->plane[1] && g->plane[2] &&
                 g->mvx && g->mvy && g->refidx && g->colpoc;
        for (int c = 0; c < 3; c++)
            g->hpel[c] = e->hpel_on
                ? plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER) : NULL;
        if (e->hpel_on) ok = ok && g->hpel[0] && g->hpel[1] && g->hpel[2];
        if (!ok)
            break;                  /* OOM: a shorter pool still works, the */
        e->dpbp_n = e->dpbp_nfree = i + 1;  /* fallback covers the shortfall */
    }
}

/* The burst's B chain + tail: every B of the burst (serial preps in coding
 * order, gated analyzes as jobs on the shared pool, trailing emits), then the
 * anchor join and the burst-end colmv restore. Runs on the API thread
 * (sync, interleaved prep/encode) or on the DRIVER (async) -- there the v4
 * two-phase shape front-loads EVERY B's serial prep (plus each reference-B's
 * DPB bookkeeping) and fires serial_done before any B encode, so the next
 * anchor's arrival phase overlaps the whole B chain instead of its tail. The
 * preps are pure functions of post-anchor bookkeeping state (a prep needs the
 * anchor's/ref-B's slot identity and scalars, never their recon); the encode
 * phase makes no shared e-> write the arrival phase observes -- the streamed
 * DPB slot CONTENT is held off arrival-side readers by refb_done, and the
 * burst-end colmv restore touches only state the next anchor no longer reads
 * (its grids are a private generation; its colocated field aims at the source
 * DPB slot). */
static void stair_chain(yah264_encoder_t *e, struct stair_burst *B)
{
    struct stair_chain *C = stair_ch(e->st, B);
    size_t mvcount = (size_t)e->mv_stride * e->height_in_mbs * 4;
    int rb = 0;
    const int tsl = (int)(B - e->st->bur), tsq = (int)atomic_load(&B->seq);
    int refb_launched = 0;          /* v7 (Y264_STAIR_REFBEARLY): set once the
 * reference B's runner+trailer are
 * submitted, whichever phase does it, so
 * phase 2 never launches it twice */
    /* WIDE: the one dependency the leaf machinery does NOT gate. A burst's B's
 * take the PREVIOUS anchor as their nearest past reference (list 0), and
 * unlike the anchor's own list-0 search that read is neither clamped nor
 * row-gated -- without width, the drain-before-submit makes the predecessor
 * complete by construction. Under width that guarantee is restored
 * explicitly: wait for its trailer to publish everything before the first
 * prep. Costs the chain nothing in steady state (the anchor's own wavefront
 * staircases against the same watermark and finishes later), and it still
 * leaves the predecessor's whole B chain -- the part width exists to overlap
 * -- running alongside.
 *
 * DO NOT TURN THIS INTO A ROW GATE. Two results, both reproducible from the
 * tree:
 *
 * (1) The row gate a replacement would install ALREADY EXISTS, so the only
 * missing piece would be a leaf-side list-0 clamp. A leaf claims row r
 * only when its own gate is open, i.e. some producer P (its burst's
 * anchor, or the burst's pipelined reference B) has pub >= min(r+LAG,
 * hmb). pub >= q implies arow >= q (stair_trailer_task waits arow >=
 * j+2 before publishing j), so analysis row q-1 was CLAIMED, which
 * required the previous anchor's pub >= min(q-1+LAG, hmb). Substituting
 * q = min(r+LAG, hmb) gives previous-anchor pub >= min(r+2*LAG-1, hmb)
 * when r+LAG <= hmb, and == hmb otherwise. Both dominate min(r+LAG,
 * hmb) for LAG >= 1 -- exactly the bound a direct row gate on prev_P
 * would have produced. The composition is EXACT, not merely sufficient,
 * and it is LAG-independent for the same reason the anchor gate is:
 * both sides of the publish/touch inequality scale by 16 per unit of LAG.
 * (Recycle-safe too, and that is why the composed form beats a direct
 * gate: a leaf never reads the predecessor's stair_prog at all, so the
 * ring may recycle that slot under it -- the hazard that rules out a
 * leaf-side gate -- while the CONTENT it reads stays pinned by
 * the readset. It also needs e->nref <= 1, width's shipped envelope: a
 * deeper list 0 reaches pictures this chain composes no gate onto.)
 * (2) It would buy nothing. Y264_UNSAFE_NO_PREVPWAIT (above) deletes the
 * wait outright -- the ceiling any safe version could reach -- and the
 * wall clock does not move: 0.990-1.004x over 19 shapes (CIF/720p/
 * 1080p, CQP/CRF, t8/t18, --ref 1..4), including every shape where this
 * wait is large (70 ms of a 272 ms CIF encode at --ref 4; 204 ms of a
 * 746 ms 1080p encode at t8). The blocked time is real and it is off the
 * critical path: it runs on the chain DRIVER while the pool is saturated
 * by the anchor's own wavefront, and deleting it leaves the row-claim
 * refusal counts unchanged to within 2% -- the leaves it delays could
 * not have been admitted earlier, because their own gates were shut for
 * the same reason the wait was blocking. scripts/stair_prevp_sweep.sh,
 * scripts/stair_prevp_1080.sh and scripts/stair_prevp_stat.sh reproduce
 * all of it. */
    /* FREELAUNCH (probe, default off): defer this wait past serial_fire so it
 * stops sitting between launch n and launch n+1. Wide async chains only;
 * phase 2 still runs behind the identical wait either way. */
    int defer_prevp = stair_freelaunch_on() && B->async && B->wide;
    if (B->prev_P && !defer_prevp && !stair_unsafe_no_prevpwait()) {
        double t0 = stair_stat_on() ? tprof_ms() : 0.0;
        stair_tr(e->st, tsl, STE_PREVP, tsq, 0);
        stair_pub_wait_all(B->prev_P, B->prev_P->hmb);
        stair_tr(e->st, tsl, STE_PREVP_E, tsq, 0);
        if (stair_stat_on()) {
            double d = tprof_ms() - t0;
            double o = atomic_load_explicit(&e->st->stat_prevp_ms, memory_order_relaxed);
            while (!atomic_compare_exchange_weak_explicit(
                       &e->st->stat_prevp_ms, &o, o + d,
                       memory_order_relaxed, memory_order_relaxed)) { }
        }
    }
    if (B->async) {
        /* Phase 1: the serial preps, coding order, one leaf per plan entry
 * (the ring outlives the burst: every leaf's recon is pinned by a
 * deferred replay until the drain). A prep bail fails loudly, as
 * before (the serial fallback would read the arrival-side seed bank,
 * and the completed commit_begins cannot be undone). */
        stair_tr(e->st, tsl, STE_PREP, tsq, B->nplan);
        for (int i = 0; i < B->nplan && rb == 0; i++) {
            const struct stair_bent *en = &B->plan[i];
            struct fpipe_leaf *L = C->leaf[i];
            if (!stair_prep_b(e, B, L, en->m, en->depth, en->is_ref, mvcount))
                rb = -1;
            else if (en->is_ref) {
                L->commit_slot = stair_dpb_commit_begin(e, L, B->bpoc[en->m], B);
                if (B->bdepth && i == 0) {  /* v5: the leaves' preps gate on it */
                    C->refb_slot = L->commit_slot;
                    /* v6: ARM the watermark here, not at stair_launch_refb in
 * phase 2. serial_fire below releases the next anchor's
 * launch, and that launch may install a row gate on this
 * rprog. Arming it in phase 2 would leave the PREVIOUS
 * occupant of this chain slot visible to the gater at
 * pub == hmb -- a gate that opens instantly on a picture
 * that has not started a row. It is the same
 * recycled-watermark hazard stair_launch already handles for
 * the anchor's own pub, arriving one ring deeper. */
                    stair_prog_reset(&C->rprog, &L->f, L->commit_slot,
                                     L->l0poc, L->l0n, L->l1poc0, B);
                    if (stair_refbearly_on()) {
                        /* v7: LAUNCH here too, not just arm -- start the
 * producer while this burst's own
 * remaining sibling preps are still running on the
 * driver, instead of after every one of them plus
 * serial_fire. See stair_refbearly_on for the safety
 * argument (no shared e-> write either task makes or
 * reads, and a later sibling prep bailing still
 * reaches stair_join_compute's unconditional
 * stair_refb_join). */
                        stair_launch_refb(e, B, L);
                        refb_launched = 1;
                    }
                }
            }
        }
        stair_tr(e->st, tsl, STE_PREP_E, tsq, B->nplan);
        if (rb == 0) {
            stair_serial_fire(e, B);   /* the arrival phase may start NOW */
            stair_tr(e->st, tsl, STE_FIRE, tsq, 0);
            /* FREELAUNCH: the deferred previous-anchor publish wait, verbatim.
 * Phase 2's B reads still see a fully-published predecessor; only
 * the next launch's serial_wait no longer sits behind it. */
            if (defer_prevp && B->prev_P && !stair_unsafe_no_prevpwait()) {
                double t0 = stair_stat_on() ? tprof_ms() : 0.0;
                if (stair_stat_on())
                    atomic_fetch_add_explicit(&e->st->stat_flaunch, 1,
                                              memory_order_relaxed);
                stair_tr(e->st, tsl, STE_PREVP, tsq, 0);
                stair_pub_wait_all(B->prev_P, B->prev_P->hmb);
                stair_tr(e->st, tsl, STE_PREVP_E, tsq, 0);
                if (stair_stat_on()) {
                    double d = tprof_ms() - t0;
                    double o = atomic_load_explicit(&e->st->stat_prevp_ms, memory_order_relaxed);
                    while (!atomic_compare_exchange_weak_explicit(
                               &e->st->stat_prevp_ms, &o, o + d,
                               memory_order_relaxed, memory_order_relaxed)) { }
                }
            }
            /* v6: the leaf half of what the launch-side wait holds. A
 * leaf's list 0 reaches past its own mini-GOP into an older live
 * burst's reference B -- 58 to 142 of 169 checks at --ref 2 to 4 --
 * and once this anchor's launch takes the row gate
 * instead of the wait, nothing else covers those reads.
 *
 * A WAIT and not a gate, deliberately. Row-gating a leaf against an
 * older burst's chain would let the ring recycle that chain's rprog
 * under a leaf still reading it: the next launch re-arms it to -1,
 * and the leaf would then be waiting on rows belonging to a
 * completely different picture. The gater wait at the launch bounds
 * that for the IMMEDIATE successor only, which is why the anchor
 * gate reaches exactly one burst back and this does not try to.
 *
 * It costs nothing where it matters. It is AFTER serial_fire, so
 * the next anchor's launch is already released and its analyze is
 * already running row-gated; blocking here delays only this burst's
 * own leaves. And it is nearly always already satisfied -- this
 * chain waited for the previous ANCHOR's full publish before phase
 * 1, and that burst's reference B trails its anchor by a row gate,
 * not by a frame.
 *
 * ONE burst, the one this launch skipped, identified by seq and not
 * by slot. That is what keeps the wait graph acyclic: a chain waits
 * only on a burst launched BEFORE it, so it can never wait on a
 * burst whose own chain is waiting on it. Sweeping the live ring
 * here instead would do exactly that -- the ring recycles, so the
 * slot that held the burst I meant can already hold a NEWER one,
 * whose phase 2 is at this same line waiting for me. */
            struct stair_burst *F = B->rbgate_of;
            if (F && atomic_load(&F->seq) == B->rbgate_seq &&
                atomic_load(&F->refb_done) < F->nrefb) {
                stair_tr(e->st, tsl, STE_RBGATE, tsq, 0);
                pthread_mutex_lock(&F->P.pmx);
                while (atomic_load(&F->refb_done) < F->nrefb &&
                       atomic_load(&F->seq) == B->rbgate_seq)
                    pthread_cond_wait(&F->P.pcv, &F->P.pmx);
                pthread_mutex_unlock(&F->P.pmx);
                stair_tr(e->st, tsl, STE_RBGATE_E, tsq, 0);
            }
            /* Phase 2: the encodes, coding order; sibling pairs concurrent. */
            for (int i = 0; i < B->nplan && rb == 0; i++) {
                if (B->bdepth && B->plan[i].is_ref) {
                    if (!refb_launched)          /* v5: streams; v7 may already
 * have done this in phase 1 */
                        stair_launch_refb(e, B, C->leaf[i]);
                } else if (stair_leafrun_on() != 2 &&
                           (B->plan[i].pair == 1 ||
                            (stair_leafrun_on() == 1 && !B->plan[i].is_ref &&
                             i + 1 < B->nplan && !B->plan[i + 1].is_ref))) {
                    stair_tr(e->st, tsl, STE_PAIR, tsq, B->plan[i].m);
                    rb = stair_run_pair(e, B, C->leaf[i], C->leaf[i + 1]);
                    stair_tr(e->st, tsl, STE_PAIR_E, tsq, B->plan[i + 1].m);
                    i++;
                } else {
                    stair_tr(e->st, tsl, STE_B, tsq,
                             B->plan[i].m | (B->plan[i].is_ref << 8));
                    rb = stair_run_b(e, B, C->leaf[i], B->plan[i].m,
                                     B->plan[i].is_ref, mvcount);
                    stair_tr(e->st, tsl, STE_B_E, tsq,
                             B->plan[i].m | (B->plan[i].is_ref << 8));
                }
            }
        }
    } else {
        rb = stair_b_hier(e, B, 0, B->nbuf, 1, mvcount);
    }
    if (stair_join_compute(e, B) < 0 || rb < 0) /* always join (even on error) */
        B->err = 1;
    {   /* the burst's auto-rule counts are final here (every analyze joined):
 * publish them for the launch fold (Y264_DAUTO_FOLD) */
        unsigned sq = atomic_load(&B->seq);
        long *c = e->st->dauto_fifo[sq % (2 * Y264_STAIR_K)];
        c[0] = B->dauto[0]; c[1] = B->dauto[1];
        atomic_store_explicit(&e->st->dauto_ready[sq % (2 * Y264_STAIR_K)], sq, memory_order_release);
    }
    if (B->col_restore && !B->wide)
        stair_col_apply(e, B, mvcount);
    /* WIDE runs it at the drain instead: K chains ending concurrently would
 * memcpy different pictures into the same four grids, and the value a later
 * reader needs is the LAST one in coding order. Drains are in launch order,
 * on the API thread, so doing it there is both race-free and correctly
 * ordered. */
    /* The decision grids need no such restore. The serial path's B's write
 * the SHARED ones, so after a burst those hold the LAST B's state, but a
 * burst-end copy of the last leaf's grids into e-> would restore nothing
 * anyone reads: every consumer of these grids (the CABAC/CAVLC nnz and mvd
 * contexts, the deblock boundary strengths, the intra mode predictors) is a
 * same-frame left/top neighbour read gated by frame-internal availability,
 * and every frame writes its own grids before reading them. Established by
 * poisoning, not by enumeration. */
    /* Chain end: fire the handshake unconditionally (covers sync chains and
 * every bail path; normally the prep phase already signalled -- first
 * signaller wins so col_src stays stable once published). Also release
 * any refb_done waiter: an errored chain may have skipped commits. */
    stair_serial_fire(e, B);
    if (B->async && atomic_load_explicit(&B->refb_done, memory_order_relaxed) < B->nrefb) {
        pthread_mutex_lock(&B->P.pmx);
        atomic_store_explicit(&B->refb_done, B->nrefb, memory_order_release);
        pthread_cond_broadcast(&B->P.pcv);
        pthread_mutex_unlock(&B->P.pmx);
    }
    /* And any ROW GATER on the reference-B watermark, for the same reason one
 * launch further out: the arm happens at prep, the trailer that drives it to
 * hmb in phase 2, and a bail between the two writes no rows at all.
 * stair_join_compute above has already synced that trailer (via
 * stair_refb_join) when there was one, so "no more rows are coming" holds
 * here whether or not it ever ran. */
    if (B->async && B->bdepth)
        stair_pub_finish(&C->rprog, e->height_in_mbs);
    stair_tr(e->st, tsl, STE_CHAIN_E, tsq, 0);
}

/* The task carries the BURST: a chain running concurrently with the arrival
 * side cannot find its burst through a shared "the one running" pointer.
 * The encoder comes back off the burst, and stair_ch off the ring position. */
static void stair_chain_task(void *arg)
{
    struct stair_burst *B = arg;
    stair_chain(B->enc, B);
}

/* v3 lazy bring-up: the driver thread, per-burst stable-source/seed copies,
 * and the spare B bank (async engagement swaps it with the arrival-side
 * buffers). v4: the leaf ring extends to min(bframes, 7) -- one context per
 * planned B, held until the drain -- so bursts up to 7 B's pipeline; with a
 * shorter ring anything past 3 falls back to the fully synchronous chain. */
static int stair_async_ready(yah264_encoder_t *e)
{
    struct stair_ctx *st = e->st;
    if (st->async_state == 0) {
        size_t nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
        int nspare = e->bframes < 7 ? e->bframes : 7;
        int ok = 1;
        /* Every chain is brought up together, not lazily on first use: the
 * engagement test in stair_run_burst reads a leaf count before the
 * launch commits to a slot, and a per-chain bring-up would make
 * engagement depend on the ring's phase rather than on the encode. */
        for (int c = 0; ok && c < Y264_STAIR_K; c++) {
            struct stair_chain *C = &st->chain[c];
            ok = (C->driver = ntp_bg_create_named("y264-driver")) != NULL;
            for (int k = C->nleaf; ok && k < nspare; k++) {
                C->leaf[k] = fleaf_new(e);
                if (C->leaf[k]) { C->leaf[k]->pool = e->pool; C->nleaf = k + 1; }
                else ok = 0;
            }
            for (int i = 0; ok && i < nspare; i++) {    /* the spare B bank */
                C->bspare[i][0] = plane_alloc(e->padded_w, e->padded_h,
                                              Y264_LUMA_BORDER);
                C->bspare[i][1] = plane_alloc(e->padded_w / e->sub_w,
                                              e->padded_h / e->sub_h,
                                              Y264_CHROMA_BORDER);
                C->bspare[i][2] = plane_alloc(e->padded_w / e->sub_w,
                                              e->padded_h / e->sub_h,
                                              Y264_CHROMA_BORDER);
                ok = C->bspare[i][0] && C->bspare[i][1] && C->bspare[i][2];
                if (ok && e->bseed[i][0])
                    for (int k = 0; k < 4 && ok; k++) {
                        C->sspare[i][k] = malloc(nmb * sizeof(int16_t));
                        ok = C->sspare[i][k] != NULL;
                    }
            }
        }
        for (int k = 0; ok && k < Y264_STAIR_K; k++) {
            struct stair_burst *B = &st->bur[k];
            B->asrc[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER);
            B->asrc[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                                     Y264_CHROMA_BORDER);
            B->asrc[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                                     Y264_CHROMA_BORDER);
            ok = B->asrc[0] && B->asrc[1] && B->asrc[2];
            if (ok && e->lr_seed_mvx) {
                B->lrs_mvx = malloc(nmb * sizeof(int16_t));
                B->lrs_mvy = malloc(nmb * sizeof(int16_t));
                B->lrs_cost = malloc(nmb * sizeof(int32_t));
                ok = B->lrs_mvx && B->lrs_mvy && B->lrs_cost;
            }
            for (int i = 0; ok && i < 8; i++) {
                B->bmbtoff[i] = malloc(nmb);      /* burst-owned ref-B field */
                ok = B->bmbtoff[i] != NULL;
            }
        }
        /* v5: each chain's reference-B pipeline -- its own runner/trailer + row
 * progress. Failure is not fatal (that burst falls back to the
 * run-to-completion reference B), so it never clears `ok`, and it is
 * per chain for the same reason as everything above it. */
        for (int c = 0; ok && stair_bdepth_on() && c < Y264_STAIR_K; c++) {
            struct stair_chain *C = &st->chain[c];
            C->brunner = ntp_bg_create_named("y264-brunner");
            C->btrailer = ntp_bg_create_named("y264-btrailer");
            C->bdepth_ok = C->brunner && C->btrailer && stair_prog_init(&C->rprog, e);
        }
        /* hpel_ensure_ws grows e->hpel_scratch_ws with an unlocked realloc, and
 * a leaf's DPB content commit calls hpel_build_ref_on from a chain
 * driver. One chain made that a single caller; K chains make it a
 * data race whose realloc can move a buffer a live band job is
 * writing. Warm it to the steady state HERE, on the API thread, with
 * the same arithmetic hpel_build_ref_on uses, so every later call
 * takes the early return and never writes. */
        if (ok && e->hpel_on && e->pool) {
            int nt = ntp_pool_nthreads(e->pool);
            int Bb = Y264_LUMA_BORDER, sstride = e->padded_w + 2 * Bb;
            int rows = e->padded_h + 2 * Bb;
            if (nt > 1) {
                int band = (rows + nt - 1) / nt;
                if (band < 32) band = 32;
                hpel_ensure_ws(e, nt, (size_t)sstride * (size_t)(band + 5)
                                      * sizeof(int32_t));
            }
        }
        st->async_state = ok ? 1 : -1;      /* partial allocs freed at stair_free */
    }
    return st->async_state == 1;
}

/* The colocated field a launching anchor must read instead of e->col*: the
 * last B of the immediately-preceding anchor's burst, published at that
 * burst's serial_fire and NOT yet copied into e->col* by its chain end.
 * Searched newest-first over the ring and keyed by the predecessor's POC, so
 * the answer is well defined at any K instead of meaning "the fly" -- the same
 * shape as anchor_srcsum_get. `live` is what bounds it: a drained burst
 * already ran its restore, and a serial frame may have overwritten e->col*
 * since, so its slot is not the authority any more. NULL = read e->col*.
 *
 * Returns the BURST, because the caller needs two different things from it: the
 * slot, purely to compare identities against the fly's, and the motion grids,
 * which it reads. Those stopped being the same answer when the pool made a
 * slot's grids move with the picture instead of staying with the slot. */
static const struct stair_burst *stair_col_src_get(const struct stair_ctx *st,
                                                   int poc)
{
    for (int i = 0; i < Y264_STAIR_K; i++) {
        int k = (st->cur - i + Y264_STAIR_K) % Y264_STAIR_K;
        const struct stair_burst *F = &st->bur[k];
        if (atomic_load_explicit(&F->live, memory_order_acquire) && F->poc == poc &&
            F->col_src)
            return F;
    }
    return NULL;
}

/* Serial launch of one burst, on the API thread: RC head, the anchor's slice
 * prep into its PRIVATE generation, the B-bank capture, DPB bookkeeping, and
 * the runner/trailer submission. Returns NULL = not engaged (caller runs the
 * serial path; every e-> side effect here is idempotently redone by it). */
static struct stair_burst *stair_launch(yah264_encoder_t *e, pixel *const src[3],
                                        int poc, int async, int wide)
{
    struct stair_ctx *st = e->st;
    int slot = stair_next_slot(st);             /* cur advances only on success:
 * a bail retries this slot */
    struct stair_burst *B = &st->bur[slot];
    size_t nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
    stair_tr(st, slot, STE_LAUNCH, (int)st->seq + 1, poc);
    /* RC head, mirroring emit_frame (CQP/CRF, or ABR/2-pass under rcp). The
 * async launch skips the zero-motion frame_complexity: e->ref may be the
 * still-streaming previous anchor, and rc_cplx feeds nothing in CQP/CRF.
 * The rcp decide never reads recon (arrival-captured lowres cost). */
    if (e->rcp_on) {
        e->rcp_cur_cme = e->rcp_arr_cme;    /* this arrival's lowres complexity */
        e->rcp_cur_cvi = e->rcp_arr_cvi;
        rcp_head(e, 1, 1, src);
    } else if (e->crf_on) {
        double C = 0.0;
        if (!async) {
            C = frame_complexity(e, src, 0);
            e->rc_cplx = C;
        }
        double Ccrf = frame_complexity_me(e);
        if (getenv("Y264_DBG_CPLX"))
            fprintf(stderr, "CPLX type=%d C=%.0f permb=%.1f  Cme=%.0f permb=%.1f\n", 1,
                    C, C / (e->width_in_mbs * e->height_in_mbs),
                    Ccrf, Ccrf / (e->width_in_mbs * e->height_in_mbs));
        e->crf_dc = crf_frame_dc(e, src);
        rc_set_qp_crf(e, Ccrf, 1);
    }
    B->async = async;
    B->wide = wide;
    B->prev_P = (wide && st->fly) ? &st->fly->P : NULL;
    B->err = 0;
    B->nreplay = 0;
    B->anchor_out_rec = 0;
    B->anchor_billed = 0;
    B->stash_n = 0;
    B->stash_len = 0;
    B->nread = 0;
    B->poc = poc;
    B->col_src = NULL;
    B->col_restore = NULL;
    /* v4: flatten the burst's coding plan now (the chain preps plan[i] into
 * leaf i) and reset the ref-B content-commit watermark the NEXT anchor's
 * launch may wait on. */
    B->nplan = stair_plan_hier(B->plan, 0, 0, e->nbuf, 1);
    B->nrefb = 0;
    for (int i = 0; i < B->nplan; i++) B->nrefb += B->plan[i].is_ref;
    B->probe_nsel = 0;
    B->rbgate_of = NULL;
    /* v5: pipeline this burst's reference B when the shape is one the leaf
 * clamp surface covers -- exactly one reference B, at plan entry 0, which
 * every leaf of the mini-GOP references. The clamp itself is keyed on the
 * same shape and applies whether or not this engages. */
    B->bdepth = async && stair_bdepth_on() && stair_ch(st, B)->bdepth_ok &&
                stair_refb_poc(e->nbuf, e->bpoc) >= 0 &&
                B->nrefb == 1 && B->nplan > 0 && B->plan[0].is_ref;
    /* Capture the burst's B bank. Sync bursts alias the arrival-side arrays
 * (nothing arrives mid-burst); an async burst swaps the plane/seed buffers
 * with the spare set so later arrivals fill fresh memory. */
    B->nbuf = e->nbuf;
    for (int i = 0; i < e->nbuf; i++) {
        B->bpoc[i] = e->bpoc[i];
        B->bdisp[i] = e->bdisp[i];
        B->bmotion[i] = e->bmotion[i];
        B->btdiff[i] = e->btdiff[i];
        B->bcplx[i] = e->rcp_bcplx[i];
        B->bcvi[i] = e->rcp_bcvi[i];
        B->bseed_valid[i] = e->bseed_valid[i];
        B->bseed_poc0[i] = e->bseed_poc0[i];
        B->bseed_poc1[i] = e->bseed_poc1[i];
        for (int c = 0; c < 3; c++) B->bplane[i][c] = e->bplane[i][c];
        for (int k = 0; k < 4; k++) B->bseed[i][k] = e->bseed[i][k];
        B->bmbtoff_valid[i] = 0;
        if (e->bmbtree_valid[i] && e->bmbtree_off[i] && B->bmbtoff[i]) {
            memcpy(B->bmbtoff[i], e->bmbtree_off[i],
                   (size_t)e->width_in_mbs * e->height_in_mbs);
            B->bmbtoff_valid[i] = 1;
        }
    }
    if (async) {
        /* Rotate EVERY spare slot, not just this burst's nbuf. The (K+1)-cycle
 * safety argument above needs slot i to rotate at every launch; a SMALL
 * burst that swaps only its own slots leaves the high slots parked on
 * planes a LARGER burst two launches back captured and may still be
 * reading, and a later deep burst's arrivals would write into them.
 * Closed on the invariant, NOT on a measurement: the 08-29 4:4:4
 * nondeterminism that pointed here turned out to be the mc_luma border
 * overflow (mc.h, y264_mc_luma_b) and reproduced with the stair OFF.
 * Slots >= nbuf carry no captured content, so they swap in place. */
        struct stair_chain *C = stair_ch(st, B);
        int nrot = e->bframes < 7 ? e->bframes : 7;     /* = the bank's nspare */
        for (int i = 0; i < nrot; i++) {
            if (i < e->nbuf) {
                for (int c = 0; c < 3; c++) e->bplane[i][c] = C->bspare[i][c];
                for (int k = 0; k < 4; k++)
                    if (e->bseed[i][k]) e->bseed[i][k] = C->sspare[i][k];
                for (int c = 0; c < 3; c++) C->bspare[i][c] = B->bplane[i][c];
                for (int k = 0; k < 4; k++)
                    if (B->bseed[i][k]) C->sspare[i][k] = B->bseed[i][k];
            } else {
                for (int c = 0; c < 3; c++) {
                    pixel *t = e->bplane[i][c];
                    e->bplane[i][c] = C->bspare[i][c];
                    C->bspare[i][c] = t;
                }
                for (int k = 0; k < 4; k++)
                    if (e->bseed[i][k] && C->sspare[i][k]) {
                        int16_t *t = e->bseed[i][k];
                        e->bseed[i][k] = C->sspare[i][k];
                        C->sspare[i][k] = t;
                    }
            }
        }
    }
    pixel *const *asrc = src;
    if (async) {
        /* Stable anchor source: the lookahead ring slot recycles while the
 * burst is in flight. Private lowres-seed copies for the same reason
 * (stash_lr_seed rewrites the shared arrays at every later pop). */
        TPROF(TP_BORDERS, copy_planes(e, B->asrc, src));
        asrc = B->asrc;
        if (e->lr_seed_valid && B->lrs_mvx) {
            memcpy(B->lrs_mvx, e->lr_seed_mvx, nmb * sizeof(int16_t));
            memcpy(B->lrs_mvy, e->lr_seed_mvy, nmb * sizeof(int16_t));
            memcpy(B->lrs_cost, e->lr_seed_cost, nmb * sizeof(int32_t));
        }
    }
    struct frame_work fw; fw_default(e, &fw);
    fw.hpel_ctx = B->hpel_ctx;
    fw.refidx = B->g.refidx;
    fw.refidx1 = B->g.refidx1;
    if (async && B->lrs_mvx) {
        fw.lr_seed_mvx = B->lrs_mvx;
        fw.lr_seed_mvy = B->lrs_mvy;
        fw.lr_seed_cost = B->lrs_cost;
    }
    /* The predecessor burst's e->col* restore may not have landed yet; read the
 * SOURCE slot in place (same values; rows final at publish, and this
 * anchor's rows gate on that publish when the slot streams). The slot can
 * never be this launch's own dpb_begin victim: it carries one of the two
 * highest FrameNums, the victim the lowest, and a pyramid DPB holds >= 4
 * pictures when eviction fires. e->prev_anchor_poc still names the
 * predecessor here (this launch assigns it below), and st->cur still names
 * its slot. */
    const struct stair_burst *cb = async ? stair_col_src_get(st, e->prev_anchor_poc)
                                         : NULL;
    const struct dpb_entry *csrc = cb ? cb->col_src : NULL;
    if (cb) {
        fw.colmvx = cb->col_bag.mvx; fw.colmvy = cb->col_bag.mvy;
        fw.colref = cb->col_bag.refidx; fw.colpoc = cb->col_bag.colpoc;
    }
    /* v4: serial_done fires before a burst's B encodes, so a reference-B's
 * slot CONTENT may still be streaming here. Two arrival-side readers need
 * it complete before this anchor's prep: estimate_wp_luma walks every
 * list-0 recon except the srcsum-substituted previous anchor (weightp is
 * always signalled; a live burst's ref-B enters list0 at nref > 1), and the
 * colocated field when the redirect's col_src is a ref-B slot (the
 * bframes-2/5/6 shapes -- the resolved csrc != cb->slot iff it is a burst
 * ref-B). Wait for the commits: scheduling-only, the values read are the
 * ones the serial order produced. In steady state the commits landed frames
 * ago; this binds only when arrivals outpace the chains (e.g. the flush
 * loop).
 *
 * The UNION over every live burst, not the fly alone -- the same shape as
 * stair_dpb_begin's eviction guard and stair_slot_readers_wait. At K=1 the
 * two sets are identical (the launching burst is never in its own union:
 * `live` is raised at the end of this function and cleared at the drain).
 * At width the fly is only the NEWEST of up to K-1 streaming predecessors,
 * and a deep list 0 fills with their reference B's newest-first -- so the
 * older burst's B's sit BEHIND the fly's in the list rather than out of
 * reach, and the fly-only test covers the near half of exactly the reads
 * that make `--ref > 1` unsafe under width.
 *
 * The two reasons do NOT reduce to one test applied K times:
 *
 * - the wp recon walk is uniform. Its reach is `--ref`, list 0 is built
 * from the DPB, and every live burst's committed reference B's are in
 * the DPB, so at nref > 1 any of them can be walked. Which ones actually
 * land in this slice's list 0 is not knowable here (build_list0 runs
 * inside the build_slice_prep below), and guessing would be a correctness
 * bet to save a wait that is free in steady state.
 * - the colocated redirect is NOT uniform. It resolves through one POC --
 * the previous anchor's -- so it names at most ONE burst however many
 * are live, and asks a question about that burst's own slot. Testing it
 * against every live burst would wait on older bursts for a read that
 * never touches them. So it is keyed on the burst the redirect actually
 * resolved to, which is what `cb` already is.
 *
 * `F->async` stays in the test per burst: a sync burst never increments
 * refb_done (its commit path takes the !commit_slot arm), so waiting on one
 * would hang. Liveness otherwise comes from the chain end, which forces
 * refb_done to nrefb even on a bail -- for every burst, not just this one.
 * Acyclic for the same reason stair_slot_readers_wait is: this runs on the
 * API thread, every burst it waits on launched earlier, and no chain ever
 * waits on the arrival side. */
    for (int b = 0; b < Y264_STAIR_K; b++) {
        struct stair_burst *F = &st->bur[b];
        if (!atomic_load_explicit(&F->live, memory_order_acquire))
            continue;
        if (!F->async || F->nrefb <= 0)
            continue;
        int wp = e->nref > 1;                       /* the recon walk / list-0 ME */
        int col = cb == F && csrc && csrc != F->slot;   /* the colocated redirect */
        /* v6: the wp arm's two reads both have a bounded form now, so for the
 * FLY it does not have to be a wait at all. The ME half becomes a row
 * gate on that reference B's publish watermark plus the fixed vertical
 * clamp (installed below, keyed on the same POC); the recon-walk half
 * becomes the cached source-luma DC, exactly as for a clamped anchor.
 *
 * The fly ALONE, and that is not a shortcut. The distance-1 arm is
 * measured blocking zero times out of every launch in every
 * configuration -- its obligation is discharged one launch earlier
 * -- so the fly is where all of the cost is. It is also
 * the only distance the ring's recycle protection already covers: the
 * gater wait below holds bur[slot+1]'s analyze rows before this launch
 * recycles slot's progs, and bur[slot+1] is exactly the burst that
 * gates on them. Reaching further would need that wait to grow too, for
 * a distance that never blocks.
 *
 * stair_refbgate_elig is the static half and build_slice_prep's clamp
 * reads the same function; F->bdepth is the runtime half, and it is
 * allowed to be runtime because it can only ever make us wait MORE.
 * Without it that reference B has no per-row publish at all -- it
 * commits its content in one step inside stair_run_b -- so there is no
 * watermark to gate on and the wait is the only correct mechanism.
 * That is every mini-GOP with more than one reference B (bframes 4-7). */
        int gated = wp && F == st->fly && F->bdepth &&
                    stair_refbgate_elig(e) &&
                    stair_refb_poc(F->nbuf, F->bpoc) >= 0 &&
                    stair_refb_poc(F->nbuf, F->bpoc) == e->refb_hist[0];
        if (gated) {
            wp = 0;
            B->rbgate_of = F;
            B->rbgate_seq = atomic_load(&F->seq);
            if (stair_stat_on()) e->stat_refbgate++;
        }
        if (!(wp || col))
            continue;
        if (stair_stat_on() && wp) e->stat_refbblock++;
        if (stair_unsafe_no_refbwait())
            continue;              /* measurement bound only; races by design */
        /* Ring distance from the newest live burst, in launches: st->seq still
 * names the fly here (this launch stamps B->seq below), so 0 is the fly
 * and anything above it is a burst the old fly-only test missed. */
        unsigned d = st->seq - atomic_load_explicit(&F->seq, memory_order_relaxed);
        if (d >= (unsigned)Y264_STAIR_K) d = Y264_STAIR_K - 1;
        if (stair_stat_on()) st->stat_refbneed[d]++;
        /* Only the wp arm is probed: it is the uniform one, and the only one a
 * list-0 test could narrow. The colocated arm already resolves through
 * a single POC and selects at most one burst. */
        if (wp && B->probe_nsel < Y264_STAIR_K) {
            B->probe_d[B->probe_nsel] = (int)d;
            B->probe_sel[B->probe_nsel++] = F;
        }
        if (atomic_load_explicit(&F->refb_done, memory_order_acquire) < F->nrefb) {
            double t0 = stair_stat_on() ? tprof_ms() : 0.0;
            stair_tr(st, slot, STE_WAIT, STW_REFB, (int)atomic_load(&F->seq));
            pthread_mutex_lock(&F->P.pmx);
            while (atomic_load_explicit(&F->refb_done, memory_order_acquire) < F->nrefb)
                pthread_cond_wait(&F->P.pcv, &F->P.pmx);
            pthread_mutex_unlock(&F->P.pmx);
            stair_tr(st, slot, STE_WAIT_E, STW_REFB, (int)atomic_load(&F->seq));
            if (stair_stat_on()) {
                double dt = tprof_ms() - t0;
                st->stat_cwait_ms += dt;
                st->stat_refb_ms += dt;
                st->stat_refbwait[d]++;
            }
        }
    }
    int deblock;
    /* Y264_DIRECT_AUTO: fold the bursts this launch is allowed to see and
 * decide the mode for every frame of this burst. Serial with the launches. */
    {
        unsigned n = st->seq + 1;               /* this burst's seq, stamped below */
        long mbs = (long)e->width_in_mbs * e->height_in_mbs;
        /* Bursts <= n - K - 1: the launches before this one each needed a free
 * slot, so those bursts drained (their counts recorded before `live`
 * dropped, which the slot pick acquires). n - K may still be draining
 * below this point; one more burst of lag keeps the fold ahead of it. */
        {
            int d = dauto_fold_env();
            unsigned lag = d > 0 ? (unsigned)d : (unsigned)Y264_STAIR_K + 1;
            while (st->dauto_folded + lag + 1 <= n) {           /* next s = folded+1 <= n-lag */
                unsigned sq = st->dauto_folded + 1;
                _Atomic unsigned *rd = &st->dauto_ready[sq % (2 * Y264_STAIR_K)];
                if (d > 0)                                      /* wait for that burst's chain end */
                    while (atomic_load_explicit(rd, memory_order_acquire) != sq) sched_yield();
                long *c = st->dauto_fifo[sq % (2 * Y264_STAIR_K)];
                st->dauto_folded = sq;
                e->direct_score[0] += c[0]; e->direct_score[1] += c[1];
                c[0] = c[1] = 0;
            }
        }
        while (e->direct_score[0] + e->direct_score[1] > mbs / y264_mb_dauto_stride()) {
            e->direct_score[0] = e->direct_score[0] * 9 / 10;
            e->direct_score[1] = e->direct_score[1] * 9 / 10;
        }
        B->dauto[0] = B->dauto[1] = 0;
        B->dauto_temporal = e->direct_score[0] > e->direct_score[1];
        if (getenv("Y264_DIRECT_WHY"))
            fprintf(stderr, "dfold n=%u folded=%u score=%ld,%ld\n", n, st->dauto_folded, e->direct_score[0], e->direct_score[1]);
        fw.dauto_acc = B->dauto;
        fw.dauto_valid = direct_auto_armed(e);
        fw.dauto_temporal = B->dauto_temporal;
    }
    stair_tr(st, slot, STE_WAIT, STW_PREP, 0);
    TPROF(TP_PREP, build_slice_prep(e, 1, 0, 1, asrc, B->g.rbsp, e->rbsp_cap, &fw,
                     &B->bs, &B->f, &B->fqp, &deblock));
    stair_tr(st, slot, STE_WAIT_E, STW_PREP, 0);
    (void)deblock;                          /* always on; the trailer runs it */
    /* Defensive: the anchor's write target must not alias any picture it
 * references (never true -- e->rec is always outside the DPB -- but a
 * violated assumption here would corrupt a reference mid-read). */
    for (int i = 0; i < B->f.nref; i++)
        if (B->f.refs[i][0] == B->f.rec[0]) {
            if (e->rcp_on)
                e->rcp_predecided = 1;  /* the serial fallback consumes the decide */
            return NULL;
        }
    /* Aim the anchor at its private generation: analyze writes / emit reads /
 * the trailer's colmv commit all use these, so the NEXT anchor's serial
 * prep + analyze never touch state this burst still reads. */
    {
        y264_frame_t *f = &B->f;
        struct w2_gen *G = &B->g;
        f->nnz[0] = G->nnz[0]; f->nnz[1] = G->nnz[1]; f->nnz[2] = G->nnz[2];
        f->i4mode = G->i4mode; f->mbcbp = G->mbcbp;
        f->mvx = G->mvx; f->mvy = G->mvy; f->mvx1 = G->mvx1; f->mvy1 = G->mvy1;
        f->refidx = G->refidx; f->refidx1 = G->refidx1;
        f->mvdx = G->mvdx; f->mvdy = G->mvdy; f->mvdx1 = G->mvdx1; f->mvdy1 = G->mvdy1;
        if (f->mb_tr8) f->mb_tr8 = G->mb_tr8;
        if (f->aq_off) f->aq_off = G->aq_off;
        if (f->mbqp)   f->mbqp = B->mbqp;
        if (f->mbtree_off && G->mbtree_off) {   /* content is ready pre-launch */
            memcpy(G->mbtree_off, f->mbtree_off, nmb);
            f->mbtree_off = G->mbtree_off;
        }
        for (int i = 0; i < f->nref && B->nread >= 0; i++)
            B->nread < (int)(sizeof B->readset / sizeof B->readset[0])
                ? (void)(B->readset[B->nread++] = f->refs[i][0])
                : (void)(B->nread = -1);
    }
    B->l0n = e->cur_l0n;
    B->l1poc0 = e->cur_l1poc0;
    for (int i = 0; i < B->l0n; i++) B->l0poc[i] = e->cur_l0poc[i];
    /* Measurement (see stat_l0sel): the anchor's list 0 is final HERE and its
 * inputs -- the DPB bookkeeping -- have not moved since the wait above, so
 * this is what a list-0-aware predicate would have decided at the wait. */
    if (stair_stat_on())
        for (int i = 0; i < B->probe_nsel; i++)
            (stair_l0_reads_refb(B->probe_sel[i], B->l0poc, B->l0n)
                 ? st->stat_l0sel : st->stat_l0skip)[B->probe_d[i]]++;
    B->disp = e->cur_disp;
    B->cabac = e->pps.entropy_coding_mode_flag ? 1 : 0;
    if (B->cabac) {
        while (y264_bs_pos_bits(&B->bs) & 7)
            y264_bs_write1(&B->bs, 1);      /* cabac_alignment_one_bit */
        y264_cabac_init_engine(&B->cb, B->bs.p); y264_cabac_set_end(&B->cb, B->bs.end - 64);
        y264_cabac_init_contexts(&B->cb, 1, 0, B->fqp);
        B->f.cabac = &B->cb;
    }
    B->f.pool = e->pool;        /* the anchor is one job on the shared pool;
 * the B's register alongside it (v2) */
    B->f.row_done = stair_row_done;
    B->f.row_done_ctx = &B->P;
    if (async && st->fly) {
        /* v3: this anchor's list-0 ME staircases against the still-in-flight
 * previous anchor -- rows claim once its publish covers the fixed
 * clamp's reach (the same LAG budget as the B list-1 gate). Gating
 * never changes bits (the clamp is applied by the env gate alone). */
        B->f.row_gate = stair_row_gate;
        B->f.row_ready = stair_row_ready;
        B->gate.p[0] = &st->fly->P;
        B->gate.n = 1;
        B->gate.rbidx = -1;
        /* v6: and the fly's REFERENCE B, when the wait above handed this launch
 * a gate instead. Same row_gate, same LAG budget, same MVY_MAX -- and
 * soundly so by identity rather than by analogy, because a reference B's
 * rows are published by stair_trailer_task, the same function with the
 * same per-row guarantee (final luma 16j+13, hpel 16j+10, chroma 8j+6)
 * that makes the bound hold against an anchor. */
        if (B->rbgate_of) {
            B->gate.rbidx = B->gate.n;
            B->gate.p[B->gate.n++] = &stair_ch(st, B->rbgate_of)->rprog;
        }
        B->f.row_gate_ctx = &B->gate;
    } else {
        B->gate.n = 0;
        B->gate.rbidx = -1;
    }
    /* This launch RECYCLES a burst slot -- including the pub watermark that
 * slot's previous occupant published and that the anchor launched right
 * AFTER it is still gating its analyze rows on (its gate ctx is that
 * struct; resetting pub to -1 would re-close its claim gates forever). So
 * wait until that gating anchor's analyze rows are all complete; after that
 * nothing reads the recycled watermark (the runner's safety net always
 * drives arow to hmb, even on a chain error).
 *
 * Depth 2 that anchor is the fly. At width K it is the burst one slot
 * PAST the one being recycled -- two launches back rather than one, which
 * is the whole point: the wait moves further from the arrival side.
 * Steady state it is long done; it binds only when arrivals outpace the
 * chains (e.g. the flush loop). */
    struct stair_burst *gater = wide ? &st->bur[(slot + 1) % Y264_STAIR_K]
                                     : st->fly;
    if (gater && (!wide ||
                  atomic_load_explicit(&gater->live, memory_order_acquire))) {
        double t0 = stair_stat_on() ? tprof_ms() : 0.0;
        stair_tr(st, slot, STE_WAIT, STW_GATER, (int)atomic_load(&gater->seq));
        stair_arow_wait(&gater->P, e->height_in_mbs);
        stair_tr(st, slot, STE_WAIT_E, STW_GATER, (int)atomic_load(&gater->seq));
        if (stair_stat_on())
            st->stat_cwait_ms += tprof_ms() - t0;
    }
    B->size = 0;
    B->joined = 0;
    stair_tr(st, slot, STE_WAIT, STW_DPB, 0);
    B->slot = stair_dpb_begin(e, poc);      /* bookkeeping now; content trails */
    stair_tr(st, slot, STE_WAIT_E, STW_DPB, 0);
    stair_prog_reset(&B->P, &B->f, B->slot, B->l0poc, B->l0n, B->l1poc0, NULL);
    e->prev_anchor_poc2 = e->prev_anchor_poc;   /* never an IDR on this path */
    e->prev_anchor_poc = poc;
    /* v6, and the ordering is the whole point: this anchor's own prep above read
 * refb_hist[0..K-2] as its live PREDECESSORS' reference B's, and the leaves
 * that prep after this read [1..K-1] as the same set with their own burst
 * pushed in front. One push, two windows. */
    refb_hist_push(e, stair_refb_poc(B->nbuf, B->bpoc));
    e->mbtree_apply = 0;                    /* B->f captured mbtree_off already */
    /* Launch order, before `live`. SEQ FIRST, THEN the reference-B watermark
 * reset, and seq_cst on both: a chain-side v6 waiter asks "is slot X still
 * the burst I meant?" by comparing seq, and if it could observe the reset
 * counter while the seq still read as the OLD burst's it would settle in to
 * wait for a picture belonging to a burst launched after it -- which is the
 * one direction that closes a cycle, since that burst's own chain waits on
 * this one. The total order seq_cst gives makes "saw refb_done == 0" imply
 * "sees the new seq". */
    atomic_store(&B->seq, ++st->seq);
    atomic_store(&B->refb_done, 0);
    atomic_store_explicit(&B->live, 1, memory_order_release);
                                            /* from here its readset pins refs */
    ntp_bg_submit(B->trailer, stair_trailer_task, &B->P);
    ntp_bg_submit(B->runner, stair_runner_task, B);
    st->cur = slot;
    stair_live_push(st, B);
    stair_tr(st, slot, STE_LAUNCH_E, (int)st->seq, st->nlive);
    if (wide) {
        st->stat_wide++;
        if (st->nlive > st->stat_nwide) st->stat_nwide = st->nlive;
    }
    return B;
}

/* Arm the chain B will execute on. Called once per launch, from both arms of
 * stair_run_burst's drain -- it was two hand-kept copies of the same six
 * assignments, which is one field away from a chain armed half by one slot's
 * rules and half by another's. */
static void stair_chain_arm(struct stair_ctx *st, struct stair_burst *B)
{
    struct stair_chain *C = stair_ch(st, B);
    C->pend = NULL;                     /* B->col_restore was reset at its launch */
    C->lparity = 0;
    C->serial_done = 0;
    C->refb_pipe = NULL;
    C->refb_slot = NULL;
    for (int k = 0; k < C->nleaf; k++)  /* defensive: no stale pending commit */
        if (C->leaf[k]) C->leaf[k]->commit_slot = NULL;
}

/* Encode one mini-GOP burst (P anchor + its buffered B's) via the staircase:
 * the anchor's analyze + trailing consumability pipeline + emit run on the
 * stair threads while the B's encode serially-among-themselves as concurrent
 * jobs on the ONE shared pool, each row's claim gated on the anchor's
 * published consumable rows. Returns 0 = done
 * (caller finishes the frame), -1 = not engaged (caller runs the serial path;
 * every e-> side effect so far is idempotently redone by it), -2 = hard error. */
static int stair_run_burst(yah264_encoder_t *e, size_t *off,
                           pixel *const src[3], int poc)
{
    struct stair_ctx *st = e->st;
    /* rcp: ALL prior actuals must stage before this anchor's decide (the
 * zero-lag-anchor schedule): retire the fly burst and any W2 pending now,
 * before stair_launch's RC head. Costs the anchor-vs-burst-tail overlap
 * only; the CRF path keeps the original late-drain order. */
    if (e->rcp_on) {
        /* Y264_RCP_LAG bursts may stay live across the decide (0 = zero-lag,
 * the shipped schedule and an exact stair_drain_all). The bound is
 * ALSO the rcp ring's: a pending entry per in-flight frame plus the
 * burst this launch is about to decide, and an entry that does not fit
 * is silently not pushed, which would slide every later fill onto the
 * wrong frame. Both terms are counts in coding order, so which drains
 * happen here is a function of the launch sequence and never of when a
 * chain finished. */
        /* The probe drops the first term only: the ring bound stays, because a
 * silently-unpushed rcp entry slides every later fill onto the wrong
 * frame and that would distort the wall clock this is measuring. */
        int keep = e->rcp_lag;
        if (stair_stat_on() && e->st->nlive > keep) st->stat_early++;
        /* Mode 2 retires the anchor half of everything it is about to leave
 * flying, so the decide below is short only the B's. Oldest first, the
 * order the ledger is filled in. */
        if (e->abr_early == 2)
            for (int n = st->nlive; n > keep; n--) {
                int k = (st->cur - n + 1) % Y264_STAIR_K;
                if (k < 0) k += Y264_STAIR_K;
                if (stair_drain_anchor(e, off, &st->bur[k]) < 0)
                    return -2;
            }
        while ((!e->abr_early && e->st->nlive > keep) ||
               (e->st->nlive > 0 && e->rcp_n + e->nbuf + 1 > RCP_MAX))
            if (stair_drain(e, off) < 0)
                return -2;
        if (!(rcp_nodrain_on() && e->rcp_lag > 0 && !e->vbv_on)) {
            g_ew_site = 6;
            w2_flush(e, off);           /* see rcp_nodrain_on */
        }
    }
    /* v3 depth: pipeline this burst across the API boundary when the gate is
 * on and the machinery is up. The caller already held the serial_done
 * handshake if a burst is in flight; a sync burst instead retires it fully
 * first (its chain shares the leaves and bemit the fly's tail holds). */
    int async = stair_depth_on() && e->nbuf <= 7 && stair_async_ready(e) &&
                e->nbuf <= st->chain[stair_next_slot(st)].nleaf;
    /* WIDTH (stage 3): stop draining the previous chain before submitting the
 * next one, and let the drain wait for the ring to need the slot back.
 *
 * The --ref bound is a CONDITION here, deliberately, and not a consequence
 * of anything downstream. At nref <= 1 a P anchor's list 0 is exactly
 * {previous anchor} and a burst leaf's nearest past reference is either
 * that same anchor or its own mini-GOP's reference B: one hop back, which
 * is exactly the reach the single clamp0_poc + the row gate cover. At
 * nref > 1 list 0 reaches further, and under real concurrency more than
 * one of those pictures can be live at once with only the newest clamped
 * -- that is the multi-hop clamp, BD-gated separately. Without it, a shape
 * with --ref > 1 keeps the fully-serialized path, and
 * it cannot be reached by a shape that merely LOOKS like ref 1: e->nref is
 * the encoder's own list-0 depth, set once from the parameters, and it is
 * the same value build_list0 sizes the list with.
 *
 * rcp is out unless it has been given a lag budget, and for a reason that
 * is the same statement twice: its zero-lag anchor schedule retires
 * everything in flight before each decide, and deferred retirement is what
 * width IS. stair_wide_rc_ok carries the rule; Y264_RCP_LAG buys the
 * budget, at a rate-accuracy price that is the whole point of measuring it.
 *
 * Y264_STAIR_WIDE_REF lifts the --ref half of this for the BD-rate round
 * that has to price it (stage 3 item 3). It is measurement scaffolding, off
 * by default, and the paragraph above stands unamended on purpose. */
    int wide = async && stair_wide_on() && stair_wide_nref_ok(e) &&
               stair_wide_rc_ok(e);
    if (!async && stair_drain_all(e, off) < 0)
        return -2;
    /* The ring is the throttle: a launch waits only when the slot it needs is
 * still held by an undrained burst, which is K launches back. */
    while (wide && st->nlive >= Y264_STAIR_K)
        if (stair_drain(e, off) < 0)
            return -2;
    struct stair_burst *B = stair_launch(e, src, poc, async, wide);
    if (!B) {
        /* Not engaged (alias defense; nothing was submitted). Retire any fly
 * so the serial fallback sees fully-restored shared state. */
        if (stair_drain_all(e, off) < 0)
            return -2;
        return -1;
    }
    /* Retire the previous burst NOW, after this anchor's jobs are registered:
 * its tail (leaf analyzes + emits + the anchor's entropy emit) overlaps
 * this anchor's wavefront on the shared pool. */
    if (!wide && st->nlive > 1) {       /* a predecessor is still in flight */
        if (stair_stat_on()) st->stat_late++;
        int dr = stair_drain(e, off);   /* retires the OLDEST, which is it */
        stair_chain_arm(st, B);
        if (dr < 0) {
            /* The previous burst failed. Our anchor jobs are already in
 * flight -- run + retire this burst so no thread dangles, then
 * report the hard error. */
            stair_chain(e, B);
            stair_drain_all(e, off);
            return -2;
        }
    } else {
        stair_chain_arm(st, B);
    }
    if (async) {
        ntp_bg_submit(stair_ch(st, B)->driver, stair_chain_task, B);
        return 0;
    }
    stair_chain(e, B);
    return stair_drain(e, off) < 0 ? -2 : 0;
}

static int encode_frame_core(yah264_encoder_t *e, pixel *const src_planes[3],
                             int have_flags, int flag_cut, int flag_anchor,
                             size_t *offp);

/* Stash the popped anchor's per-MB lowres ANCHOR-leg MV as the P-frame ME seed.
 * The leg holds the anchor-vs-previous-anchor lowres motion (integer lowres pel);
 * scale x8 (half-res -> full-res, pel -> quarter-pel). Only meaningful for an
 * anchor entry; the P search reads it (ref0 only). Cleared for non-anchors so a
 * stale MV never leaks. Single-threaded / same-call use (see lr_seed_valid). */
static void stash_lr_seed(yah264_encoder_t *e, const struct la_entry *en)
{
    e->lr_seed_valid = 0;
    e->bseed_pend_valid = 0;
    if (!e->lr_seed_mvx) return;
    int nmb = e->width_in_mbs * e->height_in_mbs;
    if (en->is_anchor && en->leg[LR_LEG_ANCHOR]) {
        const y264_lr_blk *a = en->leg[LR_LEG_ANCHOR];
        for (int i = 0; i < nmb; i++) {
            /* leg MVs are lowres qpel; full-res qpel = *2 (1 lowres px = 2 px) */
            e->lr_seed_mvx[i] = (int16_t)(a[i].mvx * 2);
            e->lr_seed_mvy[i] = (int16_t)(a[i].mvy * 2);
            e->lr_seed_cost[i] = a[i].d_inter;
        }
        e->lr_seed_valid = 1;
    } else if (!en->is_anchor && en->typed && en->bleg_have && e->bseed_pend[0]) {
        /* B entry: stash both lowres pair fields (full-res qpel) for the
 * reorder buffer; encode_frame_core moves them into the B's slot. */
        const y264_lr_blk *l0 = en->leg[LR_LEG_ANCHOR];
        const y264_lr_blk *l1 = en->leg[LR_LEG_NEXT];
        for (int i = 0; i < nmb; i++) {
            e->bseed_pend[0][i] = (int16_t)(l0[i].mvx * 2);
            e->bseed_pend[1][i] = (int16_t)(l0[i].mvy * 2);
            e->bseed_pend[2][i] = (int16_t)(l1[i].mvx * 2);
            e->bseed_pend[3][i] = (int16_t)(l1[i].mvy * 2);
        }
        if (e->bseedc_pend[0])          /* Y264_BLATE_STAT: carry the costs too */
            for (int i = 0; i < nmb; i++) {
                e->bseedc_pend[0][i] = l0[i].d_inter;
                e->bseedc_pend[1][i] = l1[i].d_inter;
                e->bseedc_pend[2][i] = en->d_intra ? en->d_intra[i] : -1;
            }
        e->bseed_pend_poc0 = en->bleg_poc0;
        e->bseed_pend_poc1 = en->bleg_poc1;
        e->bseed_pend_valid = 1;
    }
}


/* Take up to max display indices from the emitted-frame FIFO, in coding order,
 * and remove them. Returns how many were taken.
 *
 * A caller that pairs output packets with input timestamps in ARRIVAL order
 * gets every B-frame wrong, because B-frames are coded after the anchor that
 * follows them in display order. Without this the ffmpeg wrapper had no way to
 * know which frame a packet held, handed out timestamps in sorted order, and
 * produced files whose presentation timestamps ran backwards. They played, and
 * they stuttered.
 *
 * DRAIN BY PACKET COUNT, not per call: a frame's finalisation and its NAL are
 * decoupled, so one call can finalise more frames than it appends NALs for and
 * the remainder belongs to the next call's output. Take exactly as many as the
 * packets you split. Indices count input frames from zero, so a caller keeping
 * its own array of timestamps indexes straight into it. */
int yah264_encoder_frame_order(yah264_encoder_t *e, int *disp, int max)
{
    if (e && e->hw) return 0;
    int n, i;

    if (!e || !disp || max < 0)
        return -1;
    n = e->emit_count < max ? e->emit_count : max;
    for (i = 0; i < n; i++)
        disp[i] = e->emit_disp[i];
    e->emit_count -= n;
    for (i = 0; i < e->emit_count; i++)          /* keep what was not taken */
        e->emit_disp[i] = e->emit_disp[n + i];
    return n;
}

int yah264_encoder_frame_stats(yah264_encoder_t *e, yah264_frame_stats_t *out, int max)
{
    if (!e || !out || max < 0) return -1;
    if (e->hw) return 0;
    int n = e->fstats_count < max ? e->fstats_count : max;
    for (int i = 0; i < n; i++) out[i] = e->fstats[i];
    e->fstats_count -= n;
    for (int i = 0; i < e->fstats_count; i++) e->fstats[i] = e->fstats[n + i];
    return n;
}

int yah264_encoder_set_zones(yah264_encoder_t *e, const yah264_zone_t *zones, int n)
{
    if (!e || n < 0 || (n > 0 && !zones)) return -1;
    yah264_zone_t *z = n ? malloc((size_t)n * sizeof *z) : NULL;
    if (n && !z) return -1;
    for (int i = 0; i < n; i++) z[i] = zones[i];
    /* sorted by first, no overlaps, sane ranges */
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && z[k].first < z[k - 1].first; k--) { yah264_zone_t t = z[k]; z[k] = z[k - 1]; z[k - 1] = t; }
    for (int i = 0; i < n; i++)
        if (z[i].first < 0 || z[i].last < z[i].first || (i && z[i].first <= z[i - 1].last)) { free(z); return -1; }
    free(e->zones);
    e->zones = z; e->nzones = n;
    return 0;
}

int yah264_encoder_encode(yah264_encoder_t *e, yah264_nal_t **nal, int *count,
                           const yah264_picture_t *pic)
{
    if (e && e->hw) return y264_hw_encode(e->hw, nal, count, pic, hw_scenecut_probe(e, pic));
    if (g_tprof_on > 0) { g_enc_calls++; if (!pic) g_enc_null_calls++; }
    if (g_tprof_on > 0) {                     /* time inside the call vs the open-to-close wall */
        double t0 = tprof_ms();
        int r = yah264_encoder_encode_inner(e, nal, count, pic);
        g_enc_inside_ms += tprof_ms() - t0;
        return r;
    }
    return yah264_encoder_encode_inner(e, nal, count, pic);
}
static int yah264_encoder_encode_inner(yah264_encoder_t *e, yah264_nal_t **nal, int *count,
                                       const yah264_picture_t *pic)
{
    if (!e || !nal || !count)
        return -1;
    e->nal_count = 0;
    size_t off = 0;
    *nal = e->nal;
    /* Deferred-NAL contract : the previous call's last entropy emit
 * may still be in flight here. It is NOT drained at entry -- emit_frame_w2
 * retires it after this call's analyze (full overlap), appending its NAL to
 * this call's output ahead of the new frames', so coding order holds. A call
 * that emits nothing carries it forward; the flush path returns the tail. */
    if (!pic) {
        /* Flush: drain the lookahead window one anchor group at a time, then
 * the B reorder buffer. Callers loop until 0 bytes and 0 NALs. The
 * chain drains FIRST, so the tail finalizes below run consumer-side
 * with the la thread idle -- identical order to the serial path. */
        la_th_wait_all(e);
        while (e->la_n > 0 && off == 0) {
            struct la_entry *en = &e->la[e->la_head];
            e->la_head = (e->la_head + 1) % e->la_cap;
            e->la_n--;
            if (e->la_th)
                e->la_th->pop_seq++;        /* keep pop k == push k accounting */
            if (!en->typed)                 /* the pending tail: no future frame */
                la_finalize(e, en, NULL);
            stash_lr_seed(e, en);
            e->cur_la_en = en;
            if (encode_frame_core(e, en->plane, 1, en->is_cut, en->is_anchor, &off) < 0)
                return -1;
        }
        if (off == 0 && stair_drain_all(e, &off) < 0)   /* retire what is in flight */
            return -1;
        if (off == 0 && flush_buffered_p(e, &off) < 0)
            return -1;
        g_ew_site = 7;
        w2_flush(e, &off);              /* end of stream: nothing left to hide behind */
        /* Commit the tail's RC actuals only on the TERMINAL flush call (this
 * call emitted nothing => every NAL, and therefore every fill, has
 * landed on every path). Flushing on the earlier calls would pop
 * eagerly at t1 (bits available immediately) but not under an
 * in-flight burst -- a thread-count-dependent schedule, the exact
 * thing this design forbids. */
        if (e->rcp_on && off == 0)
            rcp_flush_all(e);
        *count = e->nal_count;
        return (int)off;
    }
    if (pic->csp != e->param.csp ||
        pic->width != e->width || pic->height != e->height)
        return -1;

    if (e->la_depth > 0) {
        /* Pad the input straight into the ring slot la_push will claim -- the
 * copy_planes(en->plane, e->plane) hop (a full padded-frame copy per
 * input frame) is gone; e->plane stays a legacy-path scratch. Engaged,
 * the recycled slot's plane may still be pending its own chain step
 * (its downscale is the only chain read of en->plane): slots are
 * claimed in strictly increasing ring order, so the slot of push s
 * last hosted push s-la_cap -- wait for that step before padding
 * (la_cap, not la_depth: the ring's actual rotation period, widened
 * by Y264_LA_BUF's extra capacity). */
        struct la_entry *dst = &e->la[(e->la_head + e->la_n) % e->la_cap];
        if (e->la_th)
            TPROF(TP_LAWAIT, la_th_wait(e, e->la_th->pushed + 1 - e->la_cap));
        TPROF(TP_PAD, pad_input_to(e, pic, dst->plane));
        la_push(e);
        if (e->la_n < e->la_cap) {   /* ring (window + Y264_LA_BUF) still filling */
            *count = 0;
            return 0;
        }
        struct la_entry *en = &e->la[e->la_head];
        e->la_head = (e->la_head + 1) % e->la_cap;
        e->la_n--;
        /* The popped entry's last chain writer is its future anchor's
 * finalize (B pair legs), at most bframes+1 pushes after its own
 * (+1 margin); its typed/is_cut/is_anchor landed even earlier. The
 * engage gate (la_depth >= bframes+3) makes this strictly older than
 * the newest push, so B arrivals never block on the chain's head;
 * la_cap >= la_depth keeps that true with Y264_LA_BUF engaged too. */
        if (e->la_th) {
            e->la_th->pop_seq++;
            TPROF(TP_LAWAIT, la_th_wait(e, e->la_th->pop_seq + e->bframes + 2));
        }
        if (!en->typed)
            la_finalize(e, en, NULL);
        stash_lr_seed(e, en);
        e->cur_la_en = en;
        if (encode_frame_core(e, en->plane, 1, en->is_cut, en->is_anchor, &off) < 0)
            return -1;
        *count = e->nal_count;
        return (int)off;
    }
    pad_input(e, pic);                  /* legacy path: input -> e->plane */
    e->cur_la_en = NULL;                /* legacy path: no ring entry, no memo */
    if (encode_frame_core(e, e->plane, 0, 0, 0, &off) < 0)
        return -1;
    *count = e->nal_count;
    return (int)off;
}

/* Encode one arrived frame (the whole existing frame-type / reorder / coding
 * flow). `src` is the padded input; with have_flags the scene-cut decision was
 * made at lookahead push time and flag_cut is obeyed instead of re-detecting. */
static int encode_frame_core(yah264_encoder_t *e, pixel *const src_planes[3],
                             int have_flags, int flag_cut, int flag_anchor,
                             size_t *offp)
{
    size_t off = *offp;
    int keyint = e->param.keyint > 0 ? e->param.keyint : 1;
    int period = e->bframes + 1;

    /* Lowres analysis (intra/inter costs + MVs vs prev) still runs per coded
 * frame: it feeds mb-tree's own-intra term and the legacy scene-cut. On the
 * lookahead path the popped ring entry already holds the SAME field: la_push
 * ran the identical blk8_intra_dispatch + zero-seeded blk8_inter on the same
 * plane contents against the same display-order predecessor (push order ==
 * pop order == display order), so d_intra + leg[PREV] substitute
 * byte-for-byte and the whole re-search (plus the duplicate downscale) is
 * skipped. Y264_LR_REUSE=0 restores the recompute. */
    TPROF(TP_LOWRES, {
        struct la_entry *lren = e->cur_la_en;
        if (lr_reuse_on() && lren && lren->d_intra && lren->leg[LR_LEG_PREV]) {
            memcpy(e->lowres_cur, lren->lowres, (size_t)e->lr_w * e->lr_h * sizeof(pixel));
            int nmb_lr = e->width_in_mbs * e->height_in_mbs;
            for (int i = 0; i < nmb_lr; i++) {
                const y264_lr_blk *L = &lren->leg[LR_LEG_PREV][i];
                e->lr_intra[i] = (int)lren->d_intra[i];
                e->lr_inter[i] = (int)L->d_inter;
                e->lr_mvx[i] = L->mvx == LR_MV_INVALID ? 0 : L->mvx;
                e->lr_mvy[i] = L->mvx == LR_MV_INVALID ? 0 : L->mvy;
            }
        } else {
            downscale(e->lowres_cur, e->lr_w, e->lr_h, src_planes[0], e->pstride[0]);
            lowres_analyse(e);
        }
    });
    int mscore = frame_motion_score(e);   /* adaptive-ME signal, this display frame */
    /* Psy calm gate feature, the second class rule:
 * UNCOMPENSATED lowres |tdiff| per pixel, x256, EWMA-chained in ARRIVAL
 * order on this (single) thread -- a pure function of the input sequence,
 * so it is deterministic at any width. Uncompensated is
 * the point: MV magnitude reads flat clips as fast (sintel) and clean
 * pans as slow (mobile), and both misclassify. */
    if (e->sc_have_prev) {
        size_t n = (size_t)e->lr_w * e->lr_h;
        long sd = 0;
        for (size_t i = 0; i < n; i++) {
            int d = (int)e->lowres_cur[i] - (int)e->lowres_prev[i];
            sd += d < 0 ? -d : d;
        }
        int td = (int)(sd * 256 / (long)n);
        /* first-sample seed: a zero start reads every clip's opening frames
 * as calm and fires the gate through the warmup. */
        e->lr_tdiff_ewma = e->lr_tdiff_ewma == 0 ? td
                         : (int)((9L * e->lr_tdiff_ewma + 1L * td + 5) / 10);
    }
    if (e->rcp_on) {                      /* rcp decide input: source-only, captured
 * here so no decide ever reads recon */
        e->rcp_arr_cme = frame_complexity_me(e);
        /* VBV-model complexity: cme floored at a quarter of the intra energy.
 * cme alone collapses on static/noise content (samsung's 3 Mbit
 * sparkle anchors read ~0); intra energy alone is flat within a scene
 * and misses motion-driven cost swings (sintel regressed on it). The
 * hybrid keeps cme's motion signal with a floor that cannot go blind. */
        double ilr = frame_complexity_ilr(e);
        e->rcp_arr_cvi = e->rcp_arr_cme > ilr * 0.25 ? e->rcp_arr_cme : ilr * 0.25;
    }

    /* Scene cut forces a keyframe here, but no closer than a minimum interval
 * (prevents pathological back-to-back IDRs on e.g. noise). */
    struct sc_cfg sc = sc_cfg_of(&e->param);
    /* x264 gates only the IDR PROMOTION on keyint_min, not the cut itself: a
 * detected cut closer than keyint_min still becomes a plain I frame (the
 * keyframe distance decides I-vs-IDR). With no non-IDR I at all,
 * `since_idr >= keyint_min` suppresses the cut entirely, which also makes
 * scenecut_decide's `gop_size <= keyint_min/4` bias ramp unreachable dead
 * code.
 *
 * Measured on sintel, whose cut lands at ~frame 20 inside keyint_min 25:
 * suppressed, that codes a rigid I B B B P... cadence straight through it
 * and spends 76% of the clip's bits on the 8 frames after the cut, where
 * x264 inserts an I there and spends 24%. Y264_SC_EARLY=1 lets the cut
 * through. */
    if (sc.off ? 0
                : (have_flags ? flag_cut
                   : (e->since_idr >= sc.keyint_min &&
                      detect_scenecut(e, e->since_idr))))
        e->since_idr = 0;
    { pixel *t = e->lowres_prev; e->lowres_prev = e->lowres_cur; e->lowres_cur = t; }
    e->sc_have_prev = 1;

    int since_idr = e->since_idr;
    e->since_idr = (e->since_idr + 1) % keyint;       /* advance for the next frame */
    int is_idr = (since_idr == 0);
    /* With window flags the anchor decision was made at push time (b-adapt);
 * the legacy path keeps the fixed cadence. */
    int is_anchor = have_flags ? flag_anchor
                               : (is_idr || (since_idr % period) == 0);
    int poc = since_idr * 2;                          /* POC follows display order */

    if (!is_anchor) {                                /* B: buffer until its anchor */
        TPROF(TP_BORDERS, copy_planes(e, e->bplane[e->nbuf], src_planes));
        /* Steal the ring entry's mb-tree Phase-A memo (pointer swap keeps both
 * sides' allocations balanced): the B keeps its memoized slice across
 * the ring->bplane move, so the anchor's mb-tree hits instead of
 * re-running the B's two-leg lowres ME. The ring slot's memo would be
 * invalidated on reuse anyway. */
        {
            struct la_entry *en = e->cur_la_en;
            struct mbt_bmemo *m = &e->bmbt[e->nbuf];
            m->valid = 0;
            if (en && en->mbt_pa_valid) {
                long *tpi = m->pi;         m->pi = en->mbt_pa_pi;   en->mbt_pa_pi = tpi;
                long *tpin = m->pin;       m->pin = en->mbt_pa_pin; en->mbt_pa_pin = tpin;
                signed char *tplu = m->plu; m->plu = en->mbt_pa_plu; en->mbt_pa_plu = tplu;
                int *tpmv = m->pmv;        m->pmv = en->mbt_pa_pmv; en->mbt_pa_pmv = tpmv;
                double *tpsw = m->psw;     m->psw = en->mbt_pa_psw; en->mbt_pa_psw = tpsw;
                m->past_poc = en->mbt_pa_past_poc;
                m->fut_poc = en->mbt_pa_fut_poc;
                m->valid = 1;
                en->mbt_pa_valid = 0;
            }
        }
        e->bpoc[e->nbuf] = poc;
        e->bdisp[e->nbuf] = (int)e->frame_count;
        e->bpush[e->nbuf] = e->cur_la_en ? e->cur_la_en->push_idx : 0;   /* gpq key */
        e->bmotion[e->nbuf] = mscore;
        e->btdiff[e->nbuf] = e->lr_tdiff_ewma;
        e->rcp_bcplx[e->nbuf] = e->rcp_arr_cme;
        e->rcp_bcvi[e->nbuf] = e->rcp_arr_cvi;
        e->bseed_valid[e->nbuf] = 0;
        if (e->bseed_pend_valid && e->bseed[e->nbuf][0] &&
            e->bseed_pend_poc0 < poc && poc < e->bseed_pend_poc1) {
            size_t nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
            for (int k = 0; k < 4; k++)
                memcpy(e->bseed[e->nbuf][k], e->bseed_pend[k], nmb * sizeof(int16_t));
            if (e->bseedc_pend[0] && e->bseedc[e->nbuf][0])
                for (int k = 0; k < 3; k++)
                    memcpy(e->bseedc[e->nbuf][k], e->bseedc_pend[k],
                           nmb * sizeof(int32_t));
            e->bseed_poc0[e->nbuf] = e->bseed_pend_poc0;
            e->bseed_poc1[e->nbuf] = e->bseed_pend_poc1;
            e->bseed_valid[e->nbuf] = 1;
        }
        e->nbuf++;
        e->frame_count++;
        /* Everything the next anchor's mb-tree reads is final iff that anchor
 * is the very next entry -- i.e. this was the last B of its mini-GOP.
 * mbt_pre_launch tests exactly that and declines otherwise. */
        mbt_pre_launch(e);
        *offp = off;
        return 0;
    }

    /* VBV burst gate (rcp): retire everything in flight so the trigger -- and
 * the anchor decide after it -- read the EXACT buffer (the zero-lag anchor
 * discipline, applied one step earlier; under rcp the fly burst was going
 * to be drained before the launch decide anyway). Then decide whether the
 * coming burst may ride predictions or must run the serial schedule. */
    if (e->rcp_on && e->vbv_on) {
        if (stair_drain_all(e, &off) < 0)
            return -1;
        w2_flush(e, &off);
        rcp_vbv_gate(e, is_idr);
    }

    /* v3: a burst may still be in flight from an earlier call. A pipelining
 * anchor (pyramid P over buffered B's, depth gate on) only waits for the
 * chain's SERIAL phase -- its own prep/registration then overlaps the
 * burst's tail, and the drain happens inside stair_run_burst after this
 * anchor's jobs are up. Everything else (IDR -- never overlap across it --
 * or a non-pipelining shape) retires the burst fully first: coding order
 * and the burst-end colmv restore precede any serial coding. */
    if (e->st && e->st->nlive) {
        int pipelining = e->b_pyramid && !is_idr && e->nbuf > 0 &&
                         stair_depth_on() && stair_ready(e);
        if (pipelining)
            stair_serial_wait(e);
        else if (stair_drain_all(e, &off) < 0)
            return -1;
    }

    /* Anchor (I or P). Buffered B's before an IDR belong to the closing GOP and
 * cannot reference across it, so flush them as non-reference P first. */
    if (is_idr && e->nbuf > 0 && flush_buffered_p(e, &off) < 0)
        return -1;

    if (e->b_pyramid) {
        size_t mc = (size_t)e->mv_stride * e->height_in_mbs * 4;
        if (is_idr) {
            e->code_panchor_have = 0;               /* no cross-IDR list0 dependency */
            dpb_reset(e);
            col_reset(e);                           /* break the temporal-MV chain */
            anchor_srcsum_reset(e);                 /* POC restarts: no stale key */
            refb_hist_reset(e);                     /* and the reference-B half */
            e->frame_num = 0;                       /* next_frame_num is 0 after reset */
            e->cur_ref_l0_fn = -1;                  /* IDR: no list0 reorder */
        } else {
            /* Pin the previous anchor (still in the DPB) as list0[0]. */
            int aslot = -1;
            for (int i = 0; i < e->dpb_size; i++)
                if (e->dpb[i].used && e->dpb[i].poc == e->prev_anchor_poc) { aslot = i; break; }
            if (aslot < 0) {                 /* the anchor was evicted: take any reference */
                for (int i = 0; i < e->dpb_size; i++) if (e->dpb[i].used) { aslot = i; break; }
                fprintf(stderr, "yah264: internal: previous anchor missing from the DPB\n");
                if (aslot < 0) aslot = 0;
            }
            e->cur_ref_l0_fn = e->dpb[aslot].frame_num;
            for (int c = 0; c < 3; c++) e->ref[c] = e->dpb[aslot].plane[c];
            e->ref0_poc = e->dpb[aslot].poc;
            e->frame_num = e->next_frame_num;
        }
        e->poc = poc;
        e->cur_b_depth = 0;                          /* anchor: base temporal layer */
        if (is_idr)
            e->idr_pic_id = (int)(e->frame_count & 0xffff);
        e->cur_disp = (int)e->frame_count;
        if (e->mbtree_on && !e->mbtree_skip && (e->nbuf > 0 || e->la_depth > 0)) {
            /* mb-tree walks the window up to the NEWEST push (its typed flags
 * and legs), exactly as the serial order does: full-chain wait.
 * On a replay-oracle HIT the walk never runs, so its wait is the
 * lead the probe exists to emulate -- skip it; a MISS takes the
 * untouched wait-then-compute path. Same for a LEAD prefetch, whose
 * thread is holding that wait already (mbt_pre_inflight). */
            if (!mbt_oracle_prepare(e) && !mbt_pre_inflight(e))
                TPROF(TP_MBTWAIT, la_th_wait_mbtree(e));
            TPROF(TP_MBTREE, mbt_resolve(e, src_planes));              /* lower QP where dependents lean on it */
            mbt_warm_launch(e);       /* the next anchor's Phase A, during this burst */
            e->mbtree_apply = 1;
        }
        e->cur_lr_motion = mscore;
        e->cur_lr_tdiff = e->lr_tdiff_ewma;
        e->rcp_cur_cme = e->rcp_arr_cme;
        e->rcp_cur_cvi = e->rcp_arr_cvi;
        /* v3 depth: cache this anchor's source-luma DC now (serial time); it
 * becomes the NEXT P's wp-estimate substitute for this anchor's recon.
 * Assigned after this frame's own prep consumed the previous value. */
        uint64_t newsum = 0, refbsum = 0;
        int have_newsum = 0, refbpoc = -1;
        if (stair_depth_on() && stair_clamp_on(e)) {
            newsum = src_luma_sum(e, src_planes[0]);
            have_newsum = 1;
            /* v6: and this burst's REFERENCE B, on exactly the same terms -- a
 * later anchor clamps it in list 0 and so needs the same substitute
 * for a recon it cannot finish reading. Computed HERE, before
 * stair_launch swaps the arrival-side B planes out to the chain's
 * spare bank: after that e->bplane names the next burst's memory.
 * Index 1 because stair_refb_poc's shape test is bpoc[1]. */
            if (stair_refbgate_on() && stair_bdepth_on() && !is_idr)
                refbpoc = stair_refb_poc(e->nbuf, e->bpoc);
            if (refbpoc >= 0)
                refbsum = src_luma_sum(e, e->bplane[1][0]);
        }
        /* Staircase: a P anchor with buffered B's is the mini-GOP shape the
 * pipeline overlaps. Not engaged (-1) falls through to the serial
 * path, whose prep idempotently redoes the aborted attempt's writes. */
        if (!is_idr && e->nbuf > 0 && stair_ready(e)) {
            int sr = stair_run_burst(e, &off, src_planes, poc);
            if (sr == -2)
                return -1;
            if (sr == 0) {
                if (have_newsum)
                    anchor_srcsum_put(e, poc, newsum);
                if (refbpoc >= 0)               /* coding order: anchor, then its ref B */
                    anchor_srcsum_put(e, refbpoc, refbsum);
                e->nbuf = 0;
                e->frame_count++;
                *offp = off;
                return 0;
            }
        }
        if (emit_frame(e, &off, is_idr ? 0 : 1, is_idr, 1, src_planes) < 0)
            return -1;
        e->mbtree_apply = 0;                          /* B's don't use it */
        TPROF(TP_DPBSTORE, dpb_store(e, poc, mc));
        /* Shift the anchor history, the clamp set's hop-2 producer. An IDR ends
 * it: POC restarts there, so the previous GOP's anchor would otherwise
 * be reachable as a same-numbered key -- the reason anchor_srcsum_reset
 * exists a few lines above. */
        e->prev_anchor_poc2 = is_idr ? -1 : e->prev_anchor_poc;
        e->prev_anchor_poc = poc;
        /* v6: the reference-B history moves with it, on this path as on the
 * staircase launch -- the clamp it keys has to be the same function of
 * coding order whichever path coded the frame, or a serial fallback
 * would produce different bits from an engaged run. */
        refb_hist_push(e, is_idr ? -1 : stair_refb_poc(e->nbuf, e->bpoc));
        if (have_newsum)
            anchor_srcsum_put(e, poc, newsum);
        if (refbpoc >= 0)
            anchor_srcsum_put(e, refbpoc, refbsum);
        if (code_b_hier(e, 0, e->nbuf, 1, &off, mc) < 0)
            return -1;
        e->nbuf = 0;
        e->frame_count++;
        /* Deferred-NAL: the burst's trailing emit stays in flight across the API
 * boundary and is drained at the next encode call's entry. */
        *offp = off;
        return 0;
    }

    int fnmask = (1 << (e->sps.log2_max_frame_num_minus4 + 4)) - 1;
    if (is_idr) {
        e->code_panchor_have = 0;                   /* no cross-IDR list0 dependency */
        e->nref_valid = 0;                          /* IDR flushes the reference ring */
        e->anchor_seq = 0;
        col_reset(e);                               /* break the temporal-MV chain */
    }
    /* FrameNum counts coded references since the IDR; with b-adapt the B runs
 * vary, so a division by the nominal period no longer works. */
    e->anchor_fn = e->anchor_seq & fnmask;
    e->anchor_seq++;
    e->frame_num = e->anchor_fn;
    e->poc = poc;
    if (is_idr)
        e->idr_pic_id = (int)(e->frame_count & 0xffff);
    e->cur_disp = (int)e->frame_count;
    if (e->mbtree_on && !e->mbtree_skip && (e->nbuf > 0 || e->la_depth > 0)) {
        if (!mbt_oracle_prepare(e) && !mbt_pre_inflight(e))  /* replay hit / lead
 * prefetch: the wait is not the driver's */
            TPROF(TP_MBTWAIT, la_th_wait_mbtree(e));
        TPROF(TP_MBTREE, mbt_resolve(e, src_planes));
        mbt_warm_launch(e);
        e->mbtree_apply = 1;
    }
    e->cur_lr_motion = mscore;
    e->cur_lr_tdiff = e->lr_tdiff_ewma;
    e->rcp_cur_cme = e->rcp_arr_cme;
    e->rcp_cur_cvi = e->rcp_arr_cvi;
    if (emit_frame(e, &off, is_idr ? 0 : 1, is_idr, 1, src_planes) < 0)
        return -1;
    e->mbtree_apply = 0;

    /* The anchor reconstruction is the future (list-1) reference for the B's, and
 * its motion field is the co-located source for their spatial-direct MVs.
 * Rotate by pointer: the anchor's rec buffer becomes ref1 (borders already
 * extended in emit_frame); ref1's old buffer becomes the next coding target.
 * rec_out still points at the anchor's pixels. */
    for (int c = 0; c < 3; c++) {
        pixel *t = e->ref1[c]; e->ref1[c] = e->rec[c]; e->rec[c] = t;
    }
    if (e->flat_hp_on) {
        for (int c = 0; c < 3; c++) {
            pixel *t = e->ref1_hp[c]; e->ref1_hp[c] = e->rec_hp[c]; e->rec_hp[c] = t;
        }
        e->rec_hpv = 0;                 /* rec's content diverges next frame */
        /* Filter the stored anchor recon ONCE, here; every slice that references
 * it (B list-1 now, list-0 after rotation into the ring) reuses the
 * planes by pointer instead of rebuilding them. */
        TPROF(TP_DPBSTORE, hpel_build_ref(e, e->ref1_hp[0], e->ref1_hp[1],
                                          e->ref1_hp[2], e->ref1[0]));
        e->ref1_hpv = 1;
    }
    e->ref1_poc = poc;                              /* future anchor POC, for implicit WP */
    if (e->nbuf > 0) {
        size_t mc = (size_t)e->mv_stride * e->height_in_mbs * 4;
        e->colframepoc = poc;
        memcpy(e->colmvx, e->mvx, mc * sizeof(int16_t));
        memcpy(e->colmvy, e->mvy, mc * sizeof(int16_t));
        memcpy(e->colref, e->refidx, mc);
        for (size_t i = 0; i < mc; i++)
            e->colpoc[i] = (int16_t)(e->refidx[i] >= 0 && e->refidx[i] < e->cur_l0n
                                     ? e->cur_l0poc[e->refidx[i]] : -1);
    }
    for (int i = 0; i < e->nbuf; i++) {              /* B's: list0=prev, list1=this */
        for (int c = 0; c < 3; c++) e->cur_l1p[c] = NULL;   /* flat B: list1 = ref1[] */
        e->frame_num = e->anchor_fn;
        e->poc = e->bpoc[i];
        e->cur_disp = e->bdisp[i];
        e->cur_lr_motion = e->bmotion[i]; e->cur_lr_tdiff = e->btdiff[i];
        e->rcp_cur_cme = e->rcp_bcplx[i];
        e->rcp_cur_cvi = e->rcp_bcvi[i];
        e->cur_bseed = i;                            /* lowres pair seeds for this B */
        if (emit_frame(e, &off, 2, 0, 0, e->bplane[i]) < 0)
            return -1;
        e->cur_bseed = -1;
    }
    e->nbuf = 0;
    /* This anchor becomes the most-recent list-0 reference for what follows. With
 * multi-ref it enters the ring (rotating out the oldest); otherwise it is the
 * single reference. */
    if (e->nref > 1) {
        refring_push(e, poc);   /* poc: the anchor's, not a B's */
    } else {
        /* Single ref: swap the labels; the anchor recon (in ref1 since the
 * post-emit swap) becomes ref, the old ref buffer becomes ref1 scratch
 * (rewritten at the next anchor's swap before any read). Keep
 * refring[0] aliasing ref -- close frees the ring slots. */
        for (int c = 0; c < 3; c++) {
            pixel *t = e->ref[c]; e->ref[c] = e->ref1[c]; e->ref1[c] = t;
            e->refring[0][c] = e->ref[c];
        }
        if (e->flat_hp_on) {            /* hpel travels with the buffers */
            for (int c = 0; c < 3; c++) {
                pixel *t = e->ring_hp[0][c];
                e->ring_hp[0][c] = e->ref1_hp[c]; e->ref1_hp[c] = t;
            }
            int t = e->ring_hpv[0]; e->ring_hpv[0] = e->ref1_hpv; e->ref1_hpv = t;
        }
    }
    e->ref0_poc = e->ref1_poc;

    e->frame_count++;
    /* Deferred-NAL: the burst's trailing emit stays in flight across the API
 * boundary and is drained at the next encode call's entry. */
    *offp = off;
    return 0;
}

int yah264_encoder_get_recon(yah264_encoder_t *e, yah264_picture_t *pic)
{
    if (e && e->hw) return -1;
    if (!e || !pic || e->frame_count == 0)
        return -1;
    pic->csp = e->param.csp;
    pic->width = e->width;
    pic->height = e->height;
    pic->pts = 0;
    for (int c = 0; c < 3; c++) {
        /* rec_out: the coded frame's buffer may have rotated into the DPB /
 * ref set; e->rec is already the NEXT frame's coding target then. */
        pic->plane[c] = e->rec_out[c] ? e->rec_out[c] : e->rec[c];
        pic->stride[c] = e->pstride[c];
    }
    return 0;
}

void yah264_encoder_set_recon_cb(yah264_encoder_t *e,
                                  void (*cb)(void *, const yah264_picture_t *, int, int),
                                  void *ud)
{
    if (e && e->hw) return;
    if (!e)
        return;
    e->recon_cb = cb;
    e->recon_ud = ud;
}

YAH264_EXPORT int yah264_encoder_set_video_signal(yah264_encoder_t *e, const yah264_video_signal_t *vs)
{
    if (!e || !vs) return -1;
    if (e->hw) return 0;                        /* the hardware backend writes its own VUI */
    if (e->frame_count > 0) return -1;          /* the SPS is already out */
    int p = vs->primaries > 0 && vs->primaries < 256 ? vs->primaries : 2;
    int t = vs->transfer > 0 && vs->transfer < 256 ? vs->transfer : 2;
    int m = vs->matrix >= 0 && vs->matrix < 256 ? vs->matrix : 2;
    e->sps.vs_present = 1;
    e->sps.vs_full_range = vs->full_range ? 1 : 0;
    e->sps.vs_primaries = p; e->sps.vs_transfer = t; e->sps.vs_matrix = m;
    e->sps.chroma_loc = vs->chroma_loc >= 0 && vs->chroma_loc <= 5 ? vs->chroma_loc : -1;
    return 0;
}

YAH264_EXPORT int yah264_encoder_rc_import(yah264_encoder_t *e, const yah264_rc_state_t *c, int frames_ahead)
{
    if (e && e->hw) return -1;
    if (!e || !c || !c->valid || !e->abr_on) return -1;
    if (e->abr_cum_actual > 0 || e->rcp_n > 0 || e->frame_count > 0) return -1;   /* only before the first frame */
    /* Continue a previous instance's controller instead of the seeds (see
 * yah264.h). Frames of instances in flight between the export and this
 * GOP are credited at the target rate on every ledger (the same in-flight
 * prediction a frame-threaded encoder makes, at GOP granularity). */
    double n = frames_ahead > 0 ? frames_ahead : 0;
    e->abr_cum_target = c->cum_target + n * e->abr_target_bpf;
    e->abr_cum_actual = c->cum_actual + n * e->abr_target_bpf;
    e->abr_qp = c->qp;
    for (int t = 0; t < 3; t++) {
        e->abr_scale[t] = c->scale[t]; e->abr_inited[t] = c->inited[t];
        e->rcp_cal[t] = c->cal[t]; e->rcp_abr_calqp[t] = c->calqp[t];
        e->last_qscale_type[t] = c->last_qscale_type[t];
    }
    double n_done = c->target_bpf > 0 ? c->rf_wanted_bits / c->target_bpf : 0.0;
    e->rf_cplx_sum = c->rf_cplx_sum + (n_done > 0 ? n * (c->rf_cplx_sum / n_done) : 0.0);
    e->rf_wanted_bits = c->rf_wanted_bits + n * e->abr_target_bpf;
    e->ptrack_qp = c->ptrack_qp; e->ptrack_norm = c->ptrack_norm;
    e->last_ref_qp[0] = c->last_ref_qp[0]; e->last_ref_qp[1] = c->last_ref_qp[1];
    e->st_cplxsum = c->st_cplxsum; e->st_cplxcount = c->st_cplxcount;
    e->last_nonb_type = c->last_nonb_type;
    for (int t = 0; t < 3; t++) { e->rf2_kc[t] = c->rf2_kc[t]; e->rf2_kc_cal[t] = c->rf2_kc_cal[t]; }
    e->rf2_opened = 1;                  /* the opening belongs to the first instance only */
    return 0;
}

YAH264_EXPORT int yah264_encoder_rc_state(const yah264_encoder_t *e, yah264_rc_state_t *out)
{
    if (e && e->hw) return -1;
    memset(out, 0, sizeof *out);
    if (!e || !e->abr_on) return -1;
    out->valid = 1;
    out->target_bpf = e->abr_target_bpf;
    out->cum_target = e->abr_cum_target; out->cum_actual = e->abr_cum_actual;
    out->qp = e->abr_qp;
    for (int t = 0; t < 3; t++) {
        out->scale[t] = e->abr_scale[t]; out->inited[t] = e->abr_inited[t];
        out->cal[t] = e->rcp_cal[t]; out->calqp[t] = e->rcp_abr_calqp[t];
        out->last_qscale_type[t] = e->last_qscale_type[t];
    }
    out->rf_cplx_sum = e->rf_cplx_sum; out->rf_wanted_bits = e->rf_wanted_bits;
    out->ptrack_qp = e->ptrack_qp; out->ptrack_norm = e->ptrack_norm;
    out->last_ref_qp[0] = e->last_ref_qp[0]; out->last_ref_qp[1] = e->last_ref_qp[1];
    out->st_cplxsum = e->st_cplxsum; out->st_cplxcount = e->st_cplxcount;
    out->last_nonb_type = e->last_nonb_type;
    for (int t = 0; t < 3; t++) { out->rf2_kc[t] = e->rf2_kc[t]; out->rf2_kc_cal[t] = e->rf2_kc_cal[t]; }
    return 0;
}

void yah264_encoder_close(yah264_encoder_t *e)
{
    if (e && e->hw) { y264_hw_close(e->hw); hw_scenecut_free(e); free(e); return; }
    if (!e)
        return;
    y264_me_stats_dump();           /* E2: prints only when Y264_ME_STATS is set */
    if (e->mbtp) {                  /* join before the pool: Phase A rides it */
        struct mbt_pre *mp = e->mbtp;
        pthread_mutex_lock(&mp->mx);
        while (mp->req && !mp->done)        /* let an unclaimed prefetch retire */
            pthread_cond_wait(&mp->cv_done, &mp->mx);
        mp->exit = 1;
        pthread_cond_broadcast(&mp->cv_req);
        pthread_mutex_unlock(&mp->mx);
        ntp_bg_sync(mp->bg);
        ntp_bg_destroy(mp->bg);
        if (getenv("Y264_MBT_PRE_DBG"))
            fprintf(stderr, "[mbt_pre] latched %ld, fell back %ld | launch->start "
                    "%.1f ms, chainwait %.1f ms, compute %.1f ms, driver latch %.1f ms\n",
                    mp->hits, mp->misses, mp->ms_gap, mp->ms_chainwait,
                    mp->ms_compute, mp->ms_latch);
        pthread_mutex_destroy(&mp->mx);
        pthread_cond_destroy(&mp->cv_req);
        pthread_cond_destroy(&mp->cv_done);
        free(mp->out_off);
        free(mp);
        e->mbtp = NULL;
    }
    if (e->la_th) {                 /* join the lookahead thread first: its pool
 * jobs must retire before pool destroy */
        struct la_thread *lt = e->la_th;
        pthread_mutex_lock(&lt->mx);
        lt->exit = 1;
        pthread_cond_broadcast(&lt->cv_push);
        pthread_mutex_unlock(&lt->mx);
        ntp_bg_sync(lt->bg);
        ntp_bg_destroy(lt->bg);
        pthread_mutex_destroy(&lt->mx);
        pthread_cond_destroy(&lt->cv_push);
        pthread_cond_destroy(&lt->cv_done);
        pthread_mutex_lock(&g_tprof_la_mx);
        g_tprof_la[0] += lt->ms[0];
        g_tprof_la[1] += lt->ms[1];
        pthread_mutex_unlock(&g_tprof_la_mx);
        free(lt);
        e->la_th = NULL;
        e->la_th_on = 0;
    }
    if (mbt_split_env())
        fprintf(stderr, "[mbt_split] %ld calls, %ld srcs (%ld miss) | invq %.1f "
                "bind %.1f phaseA %.1f phaseB %.1f finish %.1f ms\n",
                g_mbt_split.calls, g_mbt_split.srcs, g_mbt_split.misses,
                g_mbt_split.invq, g_mbt_split.bind, g_mbt_split.pa,
                g_mbt_split.pb, g_mbt_split.fin);
    if (mbt_split_env())
    {
        fprintf(stderr, "[mbt_warm] %ld passes, %ld srcs seen, %ld computed\n",
                g_mbt_split.wcalls, g_mbt_split.wsrcs, g_mbt_split.wdone);
        fprintf(stderr, "[mbt_pa] searched %ld: gpu-full %ld gpu-half %ld reuse %ld "
                "scaled %ld | no-reuse: anchor %ld not-in-ring %ld key-mismatch %ld\n",
                (long)(g_mbt_split.pa_gpu2 + g_mbt_split.pa_gpu1 +
                       g_mbt_split.pa_reuse + g_mbt_split.pa_scaled +
                       g_mbt_split.pa_anc + g_mbt_split.pa_noring +
                       g_mbt_split.pa_nokey),
                (long)g_mbt_split.pa_gpu2, (long)g_mbt_split.pa_gpu1,
                (long)g_mbt_split.pa_reuse, (long)g_mbt_split.pa_scaled,
                (long)g_mbt_split.pa_anc,
                (long)g_mbt_split.pa_noring, (long)g_mbt_split.pa_nokey);
        fprintf(stderr, "[mbt_pa_setup] downscale %.1f invq %.1f subpel-planes %.1f ms | shared sets built %ld\n", g_mbt_split.pa_ds_ms, g_mbt_split.pa_invq_ms, g_mbt_split.pa_sub_ms, g_mbt_split.sub_built);
        fprintf(stderr, "[mbt_pa_ms] setup %.1f | gpu2 %.1f/%ld gpu1 %.1f/%ld reuse %.1f/%ld scaled %.1f/%ld anchor %.1f/%ld noring %.1f/%ld nokey %.1f/%ld\n",
                g_mbt_split.pa_setup_ms, g_mbt_split.pa_ms[0], g_mbt_split.pa_n[0], g_mbt_split.pa_ms[1], g_mbt_split.pa_n[1],
                g_mbt_split.pa_ms[2], g_mbt_split.pa_n[2], g_mbt_split.pa_ms[3], g_mbt_split.pa_n[3], g_mbt_split.pa_ms[4], g_mbt_split.pa_n[4],
                g_mbt_split.pa_ms[5], g_mbt_split.pa_n[5], g_mbt_split.pa_ms[6], g_mbt_split.pa_n[6]);
        fprintf(stderr, "[mbt_nokey] unsettled %ld nobleg %ld norange %ld"
                " (of key-mismatch %ld)\n",
                (long)g_mbt_split.pa_unsettled, (long)g_mbt_split.pa_nobleg,
                (long)g_mbt_split.pa_norange, (long)g_mbt_split.pa_nokey);
        fprintf(stderr, "[mbt_miss] fresh %ld pastkey %ld futkey %ld | by window offset:",
                g_mbt_split.mfresh, g_mbt_split.mpast, g_mbt_split.mfut);
        for (int i = 0; i < 64; i++)
            if (g_mbt_split.mpos[i])
                fprintf(stderr, " %d:%ld", i, g_mbt_split.mpos[i]);
        fprintf(stderr, "\n");
    }
    tprof_dump();                   /* prints only when Y264_THREAD_PROF is set */
    if (e->bg) {
        ntp_bg_sync(e->bg);          /* drain any straggler emit before teardown */
        ntp_bg_destroy(e->bg);
    }
    fpipe_free(e);                   /* leaf pairs always join before returning */
    stair_free(e);                   /* stair bursts always join before returning */
    for (int g = 0; g < 2; g++) {
        struct w2_gen *G = &e->gen[g];
        free(G->nnz[0]); free(G->nnz[1]); free(G->nnz[2]);
        free(G->i4mode); free(G->mbcbp);
        free(G->mvx); free(G->mvy); free(G->mvx1); free(G->mvy1);
        free(G->refidx); free(G->refidx1);
        free(G->mvdx); free(G->mvdy); free(G->mvdx1); free(G->mvdy1);
        free(G->mb_tr8); free(G->aq_off); free(G->mbtree_off); free(G->rbsp);
    }
    y264_gpu_close(e->gpu);
    e->gpu = NULL;
    y264_gpu_close(e->gpu_warm);
    e->gpu_warm = NULL;
    y264_gpq_close(e->gpq);
    e->gpq = NULL;
    ntp_pool_destroy(e->pool);
    if (e->rcp_on)
        rcp_flush_all(e);           /* commit any filled tail (pass-1 stat lines) */
    if (e->rcp_on && e->vbv_on && vbv_stat_on())
        fprintf(stderr, "vbv-stat: %d bursts, %d tight (%.1f%%), %d underflow-clamps\n",
                e->rcp_vbv_nburst, e->rcp_vbv_ntight,
                e->rcp_vbv_nburst > 0
                    ? 100.0 * e->rcp_vbv_ntight / e->rcp_vbv_nburst : 0.0,
                e->rcp_vbv_nclamp);
    if (e->tp_fp)
        fclose(e->tp_fp);
    free(e->tp_stats);
    free(e->tp_cost);
    free(e->tp_qrec);
    free(e->tp_q);
    free(e->tp_ebits);
    int pw[3] = { e->padded_w, e->padded_w / e->sub_w, e->padded_w / e->sub_w };
    int pb[3] = { Y264_LUMA_BORDER, Y264_CHROMA_BORDER, Y264_CHROMA_BORDER };
    for (int c = 0; c < 3; c++) {
        plane_free(e->plane[c], pw[c], pb[c]);
        plane_free(e->rec[c], pw[c], pb[c]);
        free(e->nnz[c]);
    }
    /* e->ref aliases a refring slot (the buffers rotate), so free the ring.
 * rec/ring/ref1 labels are a permutation of the same allocations after the
 * pointer rotation; each label is freed exactly once (rec above). */
    for (int i = 0; i < (e->nref > 1 ? e->nref : 1); i++)
        for (int c = 0; c < 3; c++)
            plane_free(e->refring[i][c], pw[c], pb[c]);
    for (int c = 0; c < 3; c++)
        plane_free(e->ref1[c], pw[c], pb[c]);
    /* bplane <-> stair_chain::bspare is an exact swap (stair_launch), so the
 * two labels partition the same allocations; stair_free took its half. */
    for (int i = 0; i < e->bframes; i++)
        for (int c = 0; c < 3; c++)
            plane_free(e->bplane[i][c], pw[c], pb[c]);
    for (int i = 0; i < e->bframes && i < 8; i++) {
        free(e->blowres[i]); free(e->bmbtree_off[i]);
    }
    for (int i = 0; i < 17; i++)
        for (int c = 0; c < 3; c++)
            plane_free(e->hpel_buf[i][c], e->padded_w, Y264_LUMA_BORDER);
    if (e->flat_hp_on) {
        for (int c = 0; c < 3; c++) {
            plane_free(e->rec_hp[c], e->padded_w, Y264_LUMA_BORDER);
            plane_free(e->ref1_hp[c], e->padded_w, Y264_LUMA_BORDER);
        }
        for (int i = 0; i < e->nref; i++)
            for (int c = 0; c < 3; c++)
                plane_free(e->ring_hp[i][c], e->padded_w, Y264_LUMA_BORDER);
    }
    free(e->hpel_scratch);
    for (int w = 0; w < e->hpel_ws_n; w++) free(e->hpel_scratch_ws[w]);
    for (int i = 0; i < e->la_cap; i++) {
        for (int c = 0; c < 3; c++) plane_free(e->la[i].plane[c], pw[c], pb[c]);
        free(e->la[i].lowres);
        free(e->la[i].d_intra);
        for (int g = 0; g < LR_NLEGS; g++) free(e->la[i].leg[g]);
        free(e->la[i].mbt_pa_pi);  free(e->la[i].mbt_pa_pin); free(e->la[i].mbt_pa_plu);
        free(e->la[i].mbt_pa_pmv); free(e->la[i].mbt_pa_psw);
    }
    for (int i = 0; i < 8; i++) {
        free(e->bmbt[i].pi);  free(e->bmbt[i].pin); free(e->bmbt[i].plu);
        free(e->bmbt[i].pmv); free(e->bmbt[i].psw);
    }
    free(e->la_lr_prev); free(e->la_anchor_lr);
    free(e->la_anchor_mvx); free(e->la_anchor_mvy);
    for (int k = 0; k < 4; k++) {
        free(e->bseed_pend[k]); free(e->bseed_cur[k]);
        for (int b = 0; b < 8; b++) free(e->bseed[b][k]);
    }
    free(e->la_prop_a); free(e->la_prop_b);
    free(e->lr_seed_mvx); free(e->lr_seed_mvy); free(e->lr_seed_cost);
    free(e->i4mode);
    free(e->mbcbp);
    free(e->mvx);
    free(e->mvy);
    free(e->refidx);
    free(e->mvx1);
    free(e->mvy1);
    free(e->refidx1);
    free(e->mvdx);
    free(e->mvdy);
    free(e->mvdx1);
    free(e->mvdy1);
    free(e->colmvx);
    free(e->colmvy);
    free(e->colref);
    free(e->colpoc);
    free(e->aq_off);
    free(e->zones);
    free(e->mbqp);
    free(e->mb_tr8);
    free(e->lowres_cur);
    free(e->lowres_prev);
    free(e->lr_intra); free(e->lr_inter); free(e->lr_mvx); free(e->lr_mvy);
    free(e->mbtree_off); free(e->lowres_tmp); free(e->code_panchor_lr);
    for (int g = 0; g < 2; g++)
        for (int p = 1; p < 16; p++) free(e->lr_subpel[g][p]);
    for (int i = 0; i < e->mbt_sub_n; i++)
        for (int p = 1; p < 16; p++) free(e->mbt_sub[i][p]);
    for (int w = 0; w < e->mbt_nws; w++) {
        for (int p = 1; p < 16; p++) { free(e->mbt_subpel[w][0][p]); free(e->mbt_subpel[w][1][p]); }
        free(e->mbt_lrtmp[w]); free(e->mbt_invq[w]); free(e->mbt_aqoff[w]);
    }
    for (int i = 0; i < e->dpb_size; i++) {
        for (int c = 0; c < 3; c++) plane_free(e->dpb[i].plane[c], pw[c], pb[c]);
        for (int c = 0; c < 3; c++) if (e->dpb[i].hpel[c]) plane_free(e->dpb[i].hpel[c], e->padded_w, Y264_LUMA_BORDER);
        free(e->dpb[i].mvx); free(e->dpb[i].mvy); free(e->dpb[i].refidx);
        free(e->dpb[i].colpoc);
    }
    /* Pool bags. Buffers circulate between the slots, e->rec, the leaf recons
 * and the pool, so which allocation a given label holds at close is a
 * permutation -- every label is still freed exactly once, which is all the
 * accounting needs. Parked bags are freed too: the encoder is closing, so
 * whatever pinned them is joined. */
    for (int i = 0; i < e->dpbp_nfree; i++)
        dpbp_bag_free(e, &e->dpbp_free[i], pw, pb);
    for (int i = 0; i < e->dpbp_npend; i++)
        dpbp_bag_free(e, &e->dpbp_pend[i].b, pw, pb);
    free(e->rbsp);
    free(e->out);
    free(e);
}

/* ---------------------------------------------------------------------------
 * Scene-cut pre-scan -- the input side of the cut-aware GOP split.
 *
 * The CLI cuts its GOP-workers apart arithmetically (ceil(frames/keyint)), so a
 * real scene cut lands wherever it lands INSIDE some worker's slice: a hard
 * barrier (stair_drain_all + dpb_reset + a cold refill) that buys no GOP
 * parallelism, because the split never knew it was coming. That costs 17.4%
 * of samsung_720p's 18-thread wall. This scans the whole input up
 * front so the split can put a worker boundary ON each one.
 *
 * It reproduces the encoder's own decision exactly rather than approximating it.
 * la_finalize reads nothing but scenecut_decide(keyint, sum_icost,
 * sum_cost[LR_LEG_PREV], la_since_idr) plus the flash guard, and la_chain_step
 * computes both sums against the IMMEDIATELY PRECEDING display-order frame --
 * no propagation, no anchor chain, no rate state. So a whole-input scan is just
 * that arithmetic replayed, with la_since_idr carried as the state machine it
 * is. Exactness is what keeps the split honest: a boundary that is NOT where
 * the encoder would have put an IDR is a spurious IDR, and spurious IDRs cost
 * real bits.
 *
 * Cheapness comes from a one-sided screen, not from a looser test. The true
 * pcost -- a diamond search, and ~5x the cost of everything else here -- is
 * bounded above by its own zero-MV starting point, and scenecut_decide is
 * monotonic in pcost. So the screen IS the decision, run on that bound: a frame
 * the bound cannot cut is a frame no diamond could cut, and the diamond only
 * ever runs on the few frames that survive. Cuts are rare, so nearly every
 * frame costs a downscale, an intra pass and one SATD per block.
 *
 * Scheduling only. The encode still runs its own real-time detection; this
 * decides worker boundaries and nothing else. */


struct sc_scan {
    int lr_w, lr_h, wmb, hmb, w, h;
    pixel **lr;                  /* lowres plane, one per frame */
    int32_t **dintra;            /* per-block intra cost, one array per frame */
    const pixel *const *src;
    const int *sstride;
    long *icost, *pzero;
};

/* pad_plane then downscale, without materialising the padded plane: the
 * padded sample at column x is src[min(x, w-1)], so clamping the read indices
 * is the same arithmetic on the same values. */
static void sc_downscale(pixel *dst, int dw, int dh, const pixel *src, int ss,
                         int sw, int sh)
{
    for (int y = 0; y < dh; y++) {
        int y0 = 2 * y, y1 = y0 + 1;
        if (y0 > sh - 1) y0 = sh - 1;
        if (y1 > sh - 1) y1 = sh - 1;
        const pixel *a = src + (size_t)y0 * ss, *b = src + (size_t)y1 * ss;
        pixel *d = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int x0 = 2 * x, x1 = x0 + 1;
            if (x0 > sw - 1) x0 = sw - 1;
            if (x1 > sw - 1) x1 = sw - 1;
            d[x] = (pixel)((a[x0] + a[x1] + b[x0] + b[x1] + 2) >> 2);
        }
    }
}

static void sc_down_task(void *ctx, int tid, int i)
{
    struct sc_scan *c = ctx;
    (void)tid;
    sc_downscale(c->lr[i], c->lr_w, c->lr_h, c->src[i], c->sstride[i], c->w, c->h);
}

/* Per-frame intra sum and the ZERO-MV inter sum (the screen's upper bound).
 * Frame 0 has no predecessor and takes pp = ii, which is what la_lr_row does
 * when la_have_prev is 0. */
static void sc_cost_task(void *ctx, int tid, int i)
{
    struct sc_scan *c = ctx;
    (void)tid;
    const pixel *cur = c->lr[i], *prev = i > 0 ? c->lr[i - 1] : NULL;
    int32_t *di = c->dintra[i];
    long ic = 0, pz = 0;
    for (int my = 0; my < c->hmb; my++)
        for (int mx = 0; mx < c->wmb; mx++) {
            size_t off = (size_t)(my * 8) * c->lr_w + mx * 8;
            const pixel *sb = cur + off;
            long ii = blk8_intra_dispatch(sb, c->lr_w, mx, my);
            long pp = prev ? blk8_satd(sb, c->lr_w, prev + off, c->lr_w) : ii;
            di[my * c->wmb + mx] = (int32_t)ii;
            ic += ii;
            pz += ii < pp ? ii : pp;
        }
    c->icost[i] = ic;
    c->pzero[i] = pz;
}

/* The exact sum_cost[LR_LEG_PREV] of frame `cur` against frame `ref`: the same
 * diamond la_lr_row runs, a row per task, reduced in row order (integer
 * addition regroups exactly, so the total matches the encoder's). */
struct sc_exact { struct sc_scan *c; const pixel *cur, *ref;
                  const int32_t *di; long *rowpc; };

static void sc_exact_row(void *ctx, int tid, int my)
{
    struct sc_exact *x = ctx;
    struct sc_scan *c = x->c;
    (void)tid;
    long pc = 0;
    for (int mx = 0; mx < c->wmb; mx++) {
        int mvx, mvy;
        const pixel *sb = x->cur + (size_t)(my * 8) * c->lr_w + mx * 8;
        long ii = x->di[my * c->wmb + mx];
        long pp = blk8_inter(sb, c->lr_w, x->ref, c->lr_w,
                             c->lr_w, c->lr_h, mx * 8, my * 8, &mvx, &mvy);
        pc += ii < pp ? ii : pp;
    }
    x->rowpc[my] = pc;
}

static long sc_exact_pcost(struct sc_scan *c, ntp_pool_t *pool, long *rowpc,
                           int cur, int ref)
{
    struct sc_exact x = { c, c->lr[cur], c->lr[ref], c->dintra[cur], rowpc };
    if (pool && ntp_pool_nthreads(pool) > 1 && c->hmb > 1)
        ntp_parallel_for(pool, c->hmb, sc_exact_row, &x);
    else
        for (int my = 0; my < c->hmb; my++) sc_exact_row(&x, 0, my);
    long pc = 0;
    for (int my = 0; my < c->hmb; my++) pc += rowpc[my];
    return pc;
}

/* The pre-scan core: the IDR decision plus, when the caller wants them, the
 * per-frame lowres intra cost, the zero-MV inter bound and the cut flag
 * (a cut IDR as opposed to a keyint one). The shot table is built on these. */
static int scan_core(const yah264_param_t *param,
                     const pixel *const *luma, const int *stride,
                     int n, int nthreads, unsigned char *idr,
                     long *icost_out, long *pcost_out, unsigned char *cut_out)
{
    if (!param || !luma || !stride || !idr || n <= 0)
        return -1;

    /* This runs before any encoder is opened, so nothing has primed the shared
 * dispatch table or the lazy env statics yet -- and blk8_satd goes straight
 * through y264_dsp. Both are idempotent and both are done here on one
 * thread, before the pool exists. */
    y264_dsp_init();
    warm_lr_statics();

    struct sc_scan c;
    memset(&c, 0, sizeof(c));
    c.w = param->width; c.h = param->height;
    c.wmb = (param->width + 15) / 16;
    c.hmb = (param->height + 15) / 16;
    c.lr_w = c.wmb * 16 / 2;
    c.lr_h = c.hmb * 16 / 2;
    c.src = luma; c.sstride = stride;

    size_t lrsz = (size_t)c.lr_w * c.lr_h;
    int nmb = c.wmb * c.hmb;
    c.lr = calloc((size_t)n, sizeof(*c.lr));
    c.dintra = calloc((size_t)n, sizeof(*c.dintra));
    c.icost = calloc((size_t)n, sizeof(*c.icost));
    c.pzero = calloc((size_t)n, sizeof(*c.pzero));
    long *rowpc = calloc((size_t)c.hmb, sizeof(*rowpc));
    unsigned char *cleared = calloc((size_t)n, 1);
    int ok = c.lr && c.dintra && c.icost && c.pzero && rowpc && cleared;
    for (int i = 0; ok && i < n; i++) {
        c.lr[i] = malloc(lrsz * sizeof(pixel));
        c.dintra[i] = malloc((size_t)nmb * sizeof(int32_t));
        ok = c.lr[i] && c.dintra[i];
    }

    ntp_pool_t *pool = NULL;
    int rc = -1;
    if (ok) {
        if (nthreads > 1) pool = ntp_pool_create(nthreads);
        if (pool && ntp_pool_nthreads(pool) > 1) {
            ntp_parallel_for(pool, n, sc_down_task, &c);
            ntp_parallel_for(pool, n, sc_cost_task, &c);
        } else {
            for (int i = 0; i < n; i++) sc_down_task(&c, 0, i);
            for (int i = 0; i < n; i++) sc_cost_task(&c, 0, i);
        }

        /* Replay la_finalize's state machine over the whole input, off the
         * same resolved knobs la_finalize reads -- INCLUDING `off`: re-deriving
         * keyint_min here without asking whether the adaptive cut is disabled
         * predicts cut IDRs the encode never places. */
        struct sc_cfg sc = sc_cfg_of(param);
        int keyint = sc.keyint;
        int since_idr = 0, have_prev_fin = 0, nidr = 0;
        for (int i = 0; i < n; i++) {
            int raw = 0;
            /* The screen is the real decision run on the upper bound:
 * scenecut_decide is monotonic in pcost, and pcost_true <= pzero, so
 * a frame the bound cannot cut is a frame no diamond could cut. That
 * makes the screen exactly as tight as the test it guards -- it uses
 * this frame's own bias instead of a worst-case constant -- and
 * still cannot reject a real cut. */
            if (!sc.off && have_prev_fin && !cleared[i] &&
                since_idr >= sc.keyint_min &&
                scenecut_decide(&sc, c.icost[i], c.pzero[i], since_idr)) {
                long pc = sc_exact_pcost(&c, pool, rowpc, i, i - 1);
                raw = scenecut_decide(&sc, c.icost[i], pc, since_idr);
            }
            if (raw && param->bframes > 0 && i + 1 < n && i >= 1) {
                /* Flash guard: does the successor still predict from the
 * pre-cut frame? Then the candidate was a flash, not a cut. */
                long pc1 = sc_exact_pcost(&c, pool, rowpc, i + 1, i - 1);
                if (!scenecut_decide(&sc, c.icost[i + 1], pc1, since_idr + 1)) {
                    raw = 0;
                    cleared[i + 1] = 1;
                }
            }
            if (raw) since_idr = 0;
            int since = since_idr;
            since_idr = (since_idr + 1) % keyint;
            idr[i] = (unsigned char)(since == 0);
            nidr += idr[i];
            if (cut_out) cut_out[i] = (unsigned char)(raw || i == 0);
            if (icost_out) icost_out[i] = c.icost[i];
            if (pcost_out) pcost_out[i] = c.pzero[i];
            have_prev_fin = 1;
        }
        rc = nidr;
    }

    if (pool) ntp_pool_destroy(pool);
    for (int i = 0; i < n; i++) {
        if (c.lr) free(c.lr[i]);
        if (c.dintra) free(c.dintra[i]);
    }
    free(c.lr); free(c.dintra); free(c.icost); free(c.pzero);
    free(rowpc); free(cleared);
    return rc;
}

int yah264_scan_idr_frames(const yah264_param_t *param,
                            const void *const *luma, const int *stride,
                            int n, int nthreads, int depth, unsigned char *idr)
{
    if (depth != Y264_BIT_DEPTH) return -1;
    return scan_core(param, (const pixel *const *)luma, stride, n, nthreads,
                     idr, NULL, NULL, NULL);
}

/* The shot table (docs/shot-based-plan.md, stage S1): the pre-scan's
 * per-frame costs, aggregated per cut-delimited run. A keyint IDR inside a
 * shot does not split it -- a shot spanning several GOPs shares one entry.
 * Returns the number of shots written (at most max_shots), or -1. */
YAH264_EXPORT int yah264_scan_shots(const yah264_param_t *param,
                                 const void *const *luma_v, const int *stride,
                                 int n, int nthreads, int depth,
                                 unsigned char *idr,
                                 yah264_shot_t *shots, int max_shots)
{
    if (!shots || max_shots <= 0) return -1;
    /* A depth the planes are not is a scan of the wrong pixels that still
     * returns a plausible map, so it is refused rather than clamped. */
    if (depth != Y264_BIT_DEPTH) return -1;
    const pixel *const *luma = (const pixel *const *)luma_v;
    long *ic = malloc((size_t)n * sizeof(long)), *pc = malloc((size_t)n * sizeof(long));
    unsigned char *cut = malloc((size_t)n);
    unsigned char *idr_local = idr ? NULL : malloc((size_t)n);
    if (!ic || !pc || !cut || (!idr && !idr_local)) { free(ic); free(pc); free(cut); free(idr_local); return -1; }
    int rc = scan_core(param, luma, stride, n, nthreads, idr ? idr : idr_local, ic, pc, cut);
    int ns = 0;
    if (rc >= 0) {
        int start = 0;
        for (int i = 1; i <= n; i++) {
            if (i < n && !cut[i]) continue;
            if (ns < max_shots) {
                yah264_shot_t *sh = &shots[ns];
                memset(sh, 0, sizeof *sh);
                sh->first = start; sh->last = i - 1;
                double si = 0, sp = 0, peak = 0; int np = 0;
                for (int k = start; k < i; k++) {
                    si += (double)ic[k]; if ((double)ic[k] > peak) peak = (double)ic[k];
                    if (k > start) { sp += (double)pc[k]; np++; }
                }
                sh->icost_mean = si / (i - start);
                sh->icost_peak = peak;
                sh->pcost_mean = np ? sp / np : sh->icost_mean;
                sh->ratio = sh->icost_mean > 0 ? sh->pcost_mean / sh->icost_mean : 0.0;
            }
            ns++;
            start = i;
        }
    }
    free(ic); free(pc); free(cut); free(idr_local);
    return rc < 0 ? -1 : (ns < max_shots ? ns : max_shots);
}

/* The hardware mode (docs/videotoolbox-plan.md). Y264_HW: off | auto |
 * videotoolbox, the harnesses' override of the API's choice. */
static int hw_env(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("Y264_HW");
        v = !e || !*e || !strcmp(e, "off") || !strcmp(e, "0") ? 0
          : !strcmp(e, "auto") ? YAH264_HW_AUTO
          : !strcmp(e, "videotoolbox") || !strcmp(e, "vt") || !strcmp(e, "1") ? YAH264_HW_VIDEOTOOLBOX : 0;
    }
    return v;
}

yah264_encoder_t *yah264_encoder_open_hw(const yah264_param_t *param, int hw)
{
    if (!param) return NULL;
    if (hw != YAH264_HW_OFF) {
        char why[160] = "";
        y264_dsp_init();                /* the scene-cut probe runs lowres kernels */
        warm_lr_statics();
        struct y264_hw *h = y264_hw_open(param, why, sizeof why);
        if (h) {
            yah264_encoder_t *e = calloc(1, sizeof *e);
            if (!e) { y264_hw_close(h); return NULL; }
            e->param = *param;
            e->width = param->width; e->height = param->height;
            e->hw = h;
            return e;
        }
        if (hw == YAH264_HW_VIDEOTOOLBOX) {
            fprintf(stderr, "yah264: hardware encoder unavailable: %s\n", why);
            return NULL;
        }
        fprintf(stderr, "yah264: hardware encoder unavailable (%s); using the software encoder\n", why);
    }
    return encoder_open_sw(param);
}

yah264_encoder_t *yah264_encoder_open(const yah264_param_t *param)
{
    return yah264_encoder_open_hw(param, hw_env());
}

const char *yah264_encoder_backend(const yah264_encoder_t *e)
{
    return e && e->hw ? y264_hw_name(e->hw) : "yah264";
}

/* Our scene-cut, applied causally to the hardware path (docs/videotoolbox-plan.md
 * step 2, item 4; Y264_HW_SCENECUT=0 or --no-scenecut turns it off). The same
 * lowres intra / zero-motion / exact-diamond costs and the same decision rule
 * as the lookahead and the pre-scan, run on each frame as it arrives against
 * the previous one, so a cut at frame N forces an IDR at N with no added
 * latency. What it cannot do without a frame of lookahead is the flash guard
 * (the pre-scan checks that N+1 still predicts badly from N-1), so a one-frame
 * flash costs an extra IDR here; the hardware's own keyint remains the
 * backstop for the interval. */
struct hw_sc {
    struct sc_scan c;
    pixel   *lr[2];
    int32_t *dintra[2];
    long    *rowpc;
    int      have_prev, since_idr, cur;
    struct sc_cfg cfg;
};
static int hw_scenecut_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_HW_SCENECUT"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}
static int hw_scenecut_probe(yah264_encoder_t *e, const yah264_picture_t *pic)
{
    if (!pic || !hw_scenecut_env() || e->param.scenecut < 0) return 0;
    struct hw_sc *h = e->hw_sc;
    if (!h) {
        h = calloc(1, sizeof *h);
        if (!h) return 0;
        h->c.w = e->param.width; h->c.h = e->param.height;
        h->c.wmb = (h->c.w + 15) / 16; h->c.hmb = (h->c.h + 15) / 16;
        h->c.lr_w = h->c.wmb * 8; h->c.lr_h = h->c.hmb * 8;
        size_t lrsz = (size_t)h->c.lr_w * h->c.lr_h, nmb = (size_t)h->c.wmb * h->c.hmb;
        for (int i = 0; i < 2; i++) { h->lr[i] = malloc(lrsz * sizeof(pixel)); h->dintra[i] = malloc(nmb * sizeof(int32_t)); }
        h->rowpc = calloc((size_t)h->c.hmb, sizeof(long));
        if (!h->lr[0] || !h->lr[1] || !h->dintra[0] || !h->dintra[1] || !h->rowpc) { hw_scenecut_free(e); return 0; }
        h->c.lr = h->lr; h->c.dintra = h->dintra;
        h->cfg = sc_cfg_of(&e->param);
        e->hw_sc = h;
    }
    int cur = h->cur, prev = cur ^ 1;
    sc_downscale(h->lr[cur], h->c.lr_w, h->c.lr_h, pic->plane[0], pic->stride[0], h->c.w, h->c.h);
    int force = 0;
    if (!h->have_prev) {
        force = 0;                              /* the first frame is the hardware's IDR anyway */
    } else {
        long ic = 0, pz = 0;
        for (int my = 0; my < h->c.hmb; my++)
            for (int mx = 0; mx < h->c.wmb; mx++) {
                size_t off = (size_t)(my * 8) * h->c.lr_w + mx * 8;
                const pixel *sb = h->lr[cur] + off;
                long ii = blk8_intra_dispatch(sb, h->c.lr_w, mx, my);
                long pp = blk8_satd(sb, h->c.lr_w, h->lr[prev] + off, h->c.lr_w);
                h->dintra[cur][my * h->c.wmb + mx] = (int32_t)ii;
                ic += ii; pz += ii < pp ? ii : pp;
            }
        h->since_idr++;
        if (h->since_idr >= h->cfg.keyint) force = 1;
        else if (!h->cfg.off && h->since_idr >= h->cfg.keyint_min &&
                 scenecut_decide(&h->cfg, ic, pz, h->since_idr)) {
            long pc = sc_exact_pcost(&h->c, NULL, h->rowpc, cur, prev);
            force = scenecut_decide(&h->cfg, ic, pc, h->since_idr);
        }
    }
    if (force || !h->have_prev) h->since_idr = 0;
    h->have_prev = 1; h->cur = prev;           /* swap: this frame is the next one's previous */
    return force;
}
static void hw_scenecut_free(yah264_encoder_t *e)
{
    struct hw_sc *h = e->hw_sc;
    if (!h) return;
    free(h->lr[0]); free(h->lr[1]); free(h->dintra[0]); free(h->dintra[1]); free(h->rowpc); free(h);
    e->hw_sc = NULL;
}
