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
#include "../common/iprof.h"
#include "rcreplay.h"
/* Under rate-control replay the pixel work is switched off (rcreplay.h). */
#define RCR_PLAY(e) ((e)->rcr && (e)->rcr->play)
#include "encoder.h"
#include "rate.h"
#include "../hw/vt.h"
#include "../dsp/interp.h"
#include "motion.h"
#include "cabac.h"
#include "gpu.h"
#include "cavlc.h"
#include "loopfilter.h"
#include "sei.h"
#include "nr.h"
#include "mcstf.h"
#include "../common/nal.h"
#include "../common/cpu.h"
#include "../common/threadpool.h"
#include "../common/ledger.h"
#include "../dsp/transform.h"
#include "../dsp/intrapred.h"
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
 * 8-12 the staircase's B emit drains. Thread-local because the staircase
 * chain drivers tag their drains too, on pool threads, while the API thread
 * tags its own: a shared tag was a TSan report on every threaded encode, and
 * only the thread that set a tag ever consumes it (w2_drain, same thread). */
static _Thread_local int g_ew_site; static double g_ew[16]; static long g_ewn[16];
static double ew_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static long g_enc_calls, g_enc_null_calls;   /* Y264_THREAD_PROF: how the API is driven */
static double g_enc_inside_ms;
static int yah264_encoder_encode_inner(yah264_encoder_t *e, yah264_nal_t **nal, int *count, const yah264_picture_t *pic);
static int encode_push(yah264_encoder_t *e, const yah264_picture_t *pic, size_t *offp);
static int mcstf_pop_push(yah264_encoder_t *e, size_t *offp);
static int g_cfg[11];                         /* Y264_THREAD_PROF: the resolved configuration at open */
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
 * with the la thread engaged several workers fold into this pair at once --
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
 * run on the dedicated la thread. Their time is then NOT
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

/* --- decoupled lookahead thread ---
 *
 * THE PURITY CLAIM: the per-frame lookahead chain --
 * la_chain_step's push analysis (downscale + la_lr_row) and la_finalize
 * (deferred scene-cut + flash test, b-adapt typing, lowres_anchor_me /
 * lowres_bleg_me) -- is a pure function of (the padded input frames, its own
 * prior chain state). Its inputs are the ring entries' plane/lowres/d_intra/
 * leg fields plus the chain-owned e->la_{lr_prev, have_prev, fin_prev_lr,
 * have_prev_fin, since_idr, brun (the B-run length), anchor_lr/have/poc, anchor_mvx/mvy/
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

/* Everything compute_mbtree needs that is NOT config, filled at the anchor's
 * own pop on the GOP driver (head = la_head, anchor already popped). Passing
 * these explicitly keeps the computation off the API-thread-owned ring
 * cursors. */
struct mbt_req {
    pixel *const  *anchor_planes;   /* anchor's padded source planes (Y,Cb,Cr) */
    const pixel   *anchor_lr;       /* anchor's lowres */
    int            anchor_poc;
    long           anchor_push;     /* anchor's lookahead push index (gpq key) */
    int            head;            /* ring index of the FIRST window entry */
    int            navail;          /* window entries available forward from head */
    int8_t        *out_off;         /* nmb per-MB QP offsets */
    double        *out_mean;        /* mean offset (drives the CRF reclaim) */
};

/* mb-tree Phase-A warm thread state; the argument lives at mbt_warm_window. */
struct mbt_pre {
    ntp_bg_t       *bg;
    pthread_mutex_t mx;
    pthread_cond_t  cv_req;         /* worker waits for a request */
    pthread_cond_t  cv_done;        /* driver waits for a result */
    int             req;            /* a request is pending or running */
    int             done;           /* the warm is complete */
    int             exit;
    int             warm_head, warm_navail;
    long            warm_popseq, warm_pushed;
    int             warm_d;         /* offset of the next anchor at the launch, -1 = none typed yet */
    long            hits;           /* Y264_MBT_PRE_DBG accounting */
    double          ms_compute;
};
static void mbt_pre_main(void *arg);
static void la_th_wait_step(yah264_encoder_t *e, long need);
static void la_th_wait_mbtree(yah264_encoder_t *e);

/* Y264_MBT_BREF: reference B frames as propagation TARGETS with their own
 * offset field, and mb-tree offsets applied to them. Without it no B frame
 * carries an offset, including the B frames other frames predict from.
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

/* mb-tree AC gain (1.0 = the unscaled field). At 1.0
 * the boost term's per-MB spread is narrow (sd 1.15), so the allocation points
 * the right way and is simply too flat. Raising `strength` was already refuted on 10 of 12 clips,
 * for this reason: strength scales the term's MEAN too, and the mean is the
 * anchor-versus-B split (B frames carry no mb-tree offsets at all). This
 * scales only the deviation from the frame mean, so it is a pure
 * redistribution -- 1.7 widens the sd to 1.926 with the mean held to two
 * decimals.
 *
 * Default 1.7: CRF band median -0.70% BD-NEG, 10 of 12 clips better, worst bus
 * +1.10. The sweep optimum sits at 1.7. The ABR band's two objections are
 * inside those clips' own perturbation floors while its one trustworthy row is
 * samsung -2.10%. */
#define MBT_AC_GAIN 1.7

/* The absolute AQ anchor in yah264's metric (log2 of the luma AC energy, minus
 * 8), plus 2 per extra bit of depth. It is fitted, not derived: a
 * first-principles all-plane anchor assumes AQ strength 1.0, and since the
 * anchor only ever appears as strength*(l - anchor), at our 0.4 the same
 * constant produces a fraction of the offset it was meant to.
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
static double aq_anchor_default(void)
{
    return 4.5 + 2.0 * (Y264_BIT_DEPTH - 8);
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
    fprintf(stderr, "  cfg: threads %d wf_width %d la_depth %d la_buf %d sync_lookahead %d rcp_lag %d bframes %d keyint %d rc.lookahead %d fps %d/%d\n",
            g_cfg[0], g_cfg[1], g_cfg[2], g_cfg[3], g_cfg[4], g_cfg[5], g_cfg[6], g_cfg[7], g_cfg[8], g_cfg[9], g_cfg[10]);
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

/* MT Lever 3: the reference-frame staircase keeps the previous burst in
 * flight across the encode API boundary and starts the next anchor against
 * its per-row publish, adding the fixed P list-0 clamp (+ the wp source-sum
 * and P_Skip guard that make its reads bounded). */

/* MT Lever 3 v5: staircase B DEPTH. With it on, a mini-GOP's non-reference
 * leaves staircase against the still-in-flight REFERENCE B (which grows the
 * anchor's trailing consumability pipeline of its own) instead of waiting for
 * it to complete -- the measured binding constraint of the t18 wall, where the
 * burst span is the B chain serial-among-itself. It adds a fixed vertical
 * clamp on every leaf read of that reference B, in whichever list resolves to
 * it. DEFAULT ON; the async chain is what carries it. Resolved in
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
 * the escape hatch. The async chain is what carries it. Resolved in
 * warm_lr_statics (function-local static, warmed there).
 *
 * At --ref > 1 a P anchor's list 0 can name more than one anchor that is
 * still streaming, so width there rides the MULTI-HOP list-0 clamp: the clamp
 * set reaches Y264_STAIR_HOPS anchors back instead of one. That clamp CHANGES
 * BITS (a clamped search is a smaller search); its measured CRF band cost is
 * zero on all six ledger clips. */
static int stair_wide_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_WIDE"); v = s ? (atoi(s) ? 1 : 0) : 1; }
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
static int rcp_lag_for(const yah264_encoder_t *e)
{
    return e->param.threads == 1 ? 0 : rcp_lag_env();
}



static int wf_narrow_frame(int width, int height);   /* defined with the cap */

/* Y264_DIRECT_PERMB: the temporal-direct legality check is per macroblock
 * (default) rather than per slice. See docs/b-frames.md, third round. */
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
 * analyze_b_mb and docs/b-frames.md.
 *
 * Stack garbage is reproducible for as long as the worker's call history is,
 * which is why it only showed above one thread and only with two sibling B
 * leaves sharing the pool's workers (the leaf pipeline): --bframes 1 or 2 never form a
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
 * staircase left armed, and 0 of 10 with it off.
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
/* How far behind its own seq a launch folds the auto-rule counts: bursts
 * <= n - 2, WAITING at launch for burst n - 2's chain end, where its counts
 * are final; the wait makes the fold set a function of n alone. The wait
 * blocks on a condition variable (a sched_yield spin cost 6-8% of the
 * instructions at three threads, since the launch waited on every burst).
 * The structural bound alone, bursts <= n - K - 1, is four bursts stale at
 * K = 3: blue_sky decided temporal on 6 slices at twelve threads against 24
 * at one. */
#define DAUTO_FOLD_LAG 2u
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
 * sequences for the next anchor and nothing sequences for the leaves.
 *
 * That was the state on 2026-09-01. Y264_STAIR_TDIR has defaulted ON since
 * 2026-09-03 (see its comment above): the 14/32 reading was the uninitialised
 * direct-MV seed, not the staircase, and with that fixed the determinism gate
 * reads 32/32. So this returns 0 on the shipped default, and
 * Y264_STAIR_TDIR=0 restores the exclusion. */
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
 * the bits equal wanted, whatever the type mix. Always on; its
 * Y264_ABR_RF2_BASEDOM escape is gone. */


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
 * != 1), since width is deferred retirement of an ASYNC chain. */
static int stair_wide_capable(const yah264_encoder_t *e)
{
    return stair_wide_on()
        && e->b_pyramid && !stair_direct_blocks(e)
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
 * DEFAULT ON, and it CHANGES BITS, like the multi-hop clamp: two more clamped
 * list-0 references, and unlike hop 2 these bind often (a burst's reference B
 * sits at index 0 or 1 of every later P list 0). */

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
 * Byte-identical at bframes 2/3/4 x t1/t18, so it cannot move a
 * bitstream; it pays where a plan actually holds two adjacent non-reference
 * leaves, which is bframes 4 (-6.4% foreman, -5.0% park_joy_720p, controls
 * within 0.7%), and is a measured wash at bframes 2 and 3 where the plan has no
 * such pair to generalize. The rule lives in stair_chain's phase 2. */

/* The DPB EVICTION GUARD is redundant once the bag pool
 * serves the recycle, and it is the launch-side wait that serializes the chains
 * at bframes 2/4/6 (82% of the bframes-4 encode). So the guard runs only
 * where the pool cannot cover it.
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
 * Byte-identical at bframes 2/3/4/6 x t1/t18, so it cannot move a
 * bitstream. It pays only where the wide path engages, i.e. --ref <= 1:
 * foreman_cif t18 bframes 4 -34.6%, bframes 2 -15.3%, duplicate-build control
 * within 0.3%. At --ref 3 it is a measured no-op (+0.2%/-0.1%) because the
 * guard is not on the critical path there, so it does not move the parity
 * scoreboard either (that runs at the preset default --ref 3). */

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

/* The lowres intra mode set: the six directional Intra8x8 modes only where
 * motion leaves inter close to intra (see lr_hpreg_set). */
#define LR_I10_MODE 3
static void lr_hpreg_set(const pixel *lr, pixel *const *hp);

/* Single-thread quality mode (see mbcode.h stq). Y264_STQ: unset = engage
 * at wf_width==1; 0 = never (restores the pre-stq t1 encoder); 1 = force on at
 * any width (diagnostic). Resolved in warm_lr_statics. */
static int stq_env(void)
{
    static int v = -2;
    if (v == -2) { const char *s = getenv("Y264_STQ"); v = s ? atoi(s) : -1; }
    return v;
}

/* Y264_STAIR_STAT=1: report the arrival-side blocked time (the v4 overlap
 * window metric) at encoder close. 2 additionally dumps the CHAIN EVENT TRACE
 * -- one line per scheduling transition per ring slot -- which is what turns
 * "the bursts do not overlap" from a timing inference into a structural read
 * (scripts/stair_tl.py renders it). A LEVEL and not a flag so the trace needs
 * no lazy static of its own; every existing caller is a boolean test and 2 is
 * still true. Resolved in warm_lr_statics. */
/* Y264_WEIGHTP_STAT: the weighted-prediction census, printed at close. */
extern _Atomic long y264_wp_esc;    /* mbcode.c, the escape counter */
static int wp_stat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_WEIGHTP_STAT"); v = s ? atoi(s) : 0;
                 if (v < 0) v = 0; }
    return v;
}

static int stair_stat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_STAIR_STAT"); v = s ? atoi(s) : 0;
                 if (v < 0) v = 0; }
    return v;
}

/* Frame area, in MBs, at and above which a lookahead-side grid is big enough
 * to be worth handing to the pool. Shared by the chain's fan-out default
 * (la_inline, resolved in open) and the lowres field-ME wavefront
 * (lowres_field_me_prep). */
#define LA_FANOUT_MBS 512

/* --sync-lookahead: extra RING CAPACITY
 * ahead of the window, not a smaller window. Every mb-tree/scene-cut window
 * walk stays capped at la_depth regardless of this value -- the ONLY visible
 * effect of k>0 is k more encode calls returning 0 NALs before the first
 * frame emits, and (combined with the lookahead thread) more wall-clock slack for
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
 * the constant's consumers reach the bitstream (stair_wide_capable's rcp_lag
 * grant, the pool-failure withdrawal, and the
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
 * la_lead_for itself.
 *
 * 2, NOT Y264_MT_POOL_MIN: a pool of 2 with its own lookahead thread is worth
 * having, and min2 beats min3 in the sweep. Byte-identical by construction (it
 * moves when work starts, not what is computed), and verified so on the
 * multi-GOP shape it unblocks. Below 2 a lead has no pool to lead. */
#define LA_POOL_MIN 2

static void slice_overflow_warn(void);
static size_t cabac_zero_words(const y264_frame_t *f, y264_cabac_t *cb, const uint8_t *end,
                               int nmb);















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
        /* memcpy, not a byte loop: the loop compiled to a vector copy with
 * runtime alias checks per row, and at 2160p this copy is ~2% of the
 * encode's CPU (it is the one full-resolution pass over every input). */
        memcpy(d, s, (size_t)sw * sizeof(pixel));
        pixel edge = s[sw - 1];
        for (int x = sw; x < dw; x++)
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
 * allocated with an edge border (interp.h constants) and handed around by its
 * interior pointer; strides include the borders, so all row arithmetic is
 * unchanged. extend_borders replicates a recon's edges outward once per
 * stored frame, after which out-of-frame MC positions read the borders
 * directly -- bit-identical to the spec's coordinate clamping. */
static void plane_free(pixel *interior, int w, int b, int vmul);

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
        plane_free(g->plane[c], pw[c], pb[c], e->pvmul);
        plane_free(g->hpel[c], e->padded_w, Y264_LUMA_BORDER, e->pvmul);
    }
    free(g->mvx); free(g->mvy); free(g->refidx); free(g->colpoc);
}


/* `vmul` scales the VERTICAL border only. It is 1 for frame coding and 2 under
 * PAFF: a field is a stride-doubled view of these planes, so one row of field
 * border eats two rows of frame border, and a field extended to the same
 * Y264_LUMA_BORDER reach that motion compensation assumes needs twice the rows
 * to put it in. The stride is untouched -- the horizontal border is per row and
 * a row belongs to exactly one field -- so every offset computed from the
 * stride, and every plane copy's width, is the same in both modes. */
static pixel *plane_alloc(int w, int h, int b, int vmul)
{
    /* e->pvmul is resolved with the geometry, before any caller here runs; the
 * clamp is so that a future site that allocates EARLIER gets the frame
 * layout rather than an origin at row zero and a heap it writes behind. */
    if (vmul < 1) vmul = 1;
    size_t stride = (size_t)w + 2 * b;
    pixel *base = malloc(stride * (h + 2 * b * vmul) * sizeof(pixel));
    return base ? base + (size_t)b * vmul * stride + b : NULL;
}

static void plane_free(pixel *interior, int w, int b, int vmul)
{
    if (vmul < 1) vmul = 1;             /* the same clamp plane_alloc applied */
    if (interior)
        free(interior - (size_t)b * vmul * ((size_t)w + 2 * b) - b);
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
            if (!(p[c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul))) {
                while (c-- > 0) plane_free(p[c], e->padded_w, Y264_LUMA_BORDER, e->pvmul);
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
    IP_SCOPE_B(IP_HPEL);
    /* PAFF: motion compensation reads a FIELD, so the replication has to be a
 * field's. Each parity is extended as its own picture -- stride doubled,
 * half the rows, starting at that parity's first row -- which writes the
 * same left/right columns a frame extension would (a row belongs to one
 * field) and different rows above and below, which is the whole point: the
 * top field's row above row 0 must be a copy of frame row 0, the bottom
 * field's a copy of frame row 1. Reaching Y264_LUMA_BORDER FIELD rows out
 * consumes twice that many frame rows, which is what pvmul allocated. */
    if (e->fields) {
        /* Only the parity just coded: the other half of this buffer is either
 * the field before it, already extended, or not written yet. */
        int p = e->fld_pic ? e->fld_parity : -1;
        for (int q = 0; q < 2; q++) {
            if (p >= 0 && q != p) continue;
            extend_plane(planes[0] + (size_t)q * e->pstride[0], 2 * e->pstride[0],
                         e->padded_w, e->padded_h / 2, Y264_LUMA_BORDER);
            for (int c = 1; c < 3; c++)
                extend_plane(planes[c] + (size_t)q * e->pstride[c], 2 * e->pstride[c],
                             e->padded_w / e->sub_w, e->padded_h / e->sub_h / 2,
                             Y264_CHROMA_BORDER);
        }
        return;
    }
    extend_plane(planes[0], e->pstride[0], e->padded_w, e->padded_h,
                 Y264_LUMA_BORDER);
    for (int c = 1; c < 3; c++)
        extend_plane(planes[c], e->pstride[c], e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                     Y264_CHROMA_BORDER);
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

/* A zone's base QP for a frame the rate control put at `q`: the offset on top
 * of it, or with YAH264_ZONE_QP the zone's own QP in its place, so a fixed-QP
 * zone takes the same frame-type cascade --qp does. */
static int zone_base_qp(const yah264_zone_t *z, int q)
{
    if (z->flags & YAH264_ZONE_QP) return (int)lround(z->qp_offset);
    return q + (int)lround(z->qp_offset);
}

/* The per-frame force (yah264_encoder_set_frame_forces) naming input frame
 * `disp`, or NULL. Binary search; the array is sorted and one record per
 * frame, exactly as zone_at's is. */
static const yah264_frame_force_t *force_at(const yah264_encoder_t *e, int disp)
{
    int lo = 0, hi = e->nforces - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const yah264_frame_force_t *f = &e->forces[mid];
        if (disp < f->disp) hi = mid - 1;
        else if (disp > f->disp) lo = mid + 1;
        else return f;
    }
    return NULL;
}

/* The absolute QP forced on the frame being coded, or -1 for "the rate control
 * decides". */
static int forced_qp(const yah264_encoder_t *e)
{
    if (!e->nforces) return -1;
    const yah264_frame_force_t *f = force_at(e, e->cur_disp);
    return f ? f->qp : -1;
}

/* The coded-QP bounds, applied at the one place every path's coded QP goes
 * through. e->qp_min / e->qp_max resolve to 0 and 51 with no --qpmin/--qpmax,
 * which is exactly the clamp this replaces. The frame-TYPE offsets are applied
 * first and clamped after, so a bound is a bound on what is actually coded and
 * not on the base QP the offsets are taken from. */
int qp_bound(const yah264_encoder_t *e, int q)
{
    if (q < e->qp_min) q = e->qp_min;
    if (q > e->qp_max) q = e->qp_max;
    return q;
}

int frame_qp(const yah264_encoder_t *e, int type, int is_ref)
{
    /* PAFF: a frame's type applies to its pair, and the QP cascade is the
 * frame's. The one place the two disagree is the second field of an I
 * frame, coded as a P field and quantised as an I one -- see fld_rc_type. */
    if (e->fld_pic && e->fld_rc_type >= 0) type = e->fld_rc_type;
    /* A forced QP is ABSOLUTE and it is the last word: it lands here, ahead of
 * the zone offset, the frame-type cascade and the coded-QP bounds, because
 * the caller asked for a number and not for a starting point. The VBV is
 * upstream of this and moves e->qp, which this ignores -- that is the same
 * one-way relation --qp already has with it. */
    int fq = forced_qp(e);
    if (fq >= 0)
        return fq;
    int q = e->qp;
    if (e->nzones) {                                /* the orchestrator's per-range offset */
        const yah264_zone_t *z = zone_at(e, e->cur_disp);
        if (z) q = zone_base_qp(z, q);
    }
    /* Y264_MCSTF_QP: a filtered picture has less noise to pay for, so the
     * same bits buy a finer quantiser there; and it is the pictures others
     * predict from that the filter picks */
    if (e->mcstf && e->mcstf_qp && e->mcstf_filt[e->cur_disp & 1023])
        q += e->mcstf_qp;
    if (e->tp_pass >= 2)                            /* 2-pass sets the coded QP directly */
        return qp_bound(e, q);
    if (type == 0) q -= 3;
    else if (type == 2) q += frame_b_casc(e, is_ref);
    /* --crf-max, read on the CODED QP. The buffer raises the BASE, and the
 * frame-type cascade then sits on top of it, so a ceiling applied to the
 * base alone is a ceiling every B frame steps over -- measured at 37 on a
 * --crf-max 34 encode before this moved here. What the ceiling may not do
 * is lower a frame the rate factor itself put above it, so the un-raised
 * base is carried alongside and the same cascade is taken off it: the
 * answer is the higher of the two. */
    if (e->crf_max > 0.0 && e->crf_pre_qp > 0 && q > e->crf_max && e->qp > e->crf_pre_qp) {
        int qpre = e->crf_pre_qp;
        if (e->nzones) {
            const yah264_zone_t *z = zone_at(e, e->cur_disp);
            if (z) qpre = zone_base_qp(z, qpre);
        }
        if (type == 0) qpre -= 3;
        else if (type == 2) qpre += frame_b_casc(e, is_ref);
        int ceiling = (int)e->crf_max;
        q = qpre > ceiling ? qpre : ceiling;
    }
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
static int adme_log(void);
/* The psy-trellis pays only on
 * flat/dark content (sintel -7.68, samsung -0.94 at matched rate; grain and
 * high-motion clips flat-to-positive), and the separating source feature is
 * the share of near-flat macroblocks (sintel 92%, samsung 56% vs <=18% on
 * every measured loser). When >= PSY_FLAT_SHARE percent of a frame's 16x16
 * luma MBs have variance < PSY_FLAT_VAR, that frame's psy-trellis strength
 * gets floor PSY_FLAT_S/256. Frame-level and input-derived only, so
 * deterministic at any thread count. At 75,307,25 it is sintel-only by
 * engagement (min share 84 vs samsung's max 71 -- fires 120/120 on sintel,
 * 0/120 on every other corpus clip), through the psy lattice: sintel -5.57
 * high / -8.99 low band at matched rate, +0.8% wall on sintel alone,
 * everything else byte-identical. */
#define PSY_FLAT_SHARE 75
#define PSY_FLAT_S     307
#define PSY_FLAT_VAR   25
static int psy4k_bits(const yah264_param_t *p);
static int uhd_lite_on(const yah264_param_t *p);
static int psy_flat_log(void);

/* sumref_ovr >= 0 substitutes for the reference-plane sum -- the v3 staircase
 * passes the previous anchor's SOURCE-plane sum there (a serial-time constant),
 * because the anchor's RECON may still be streaming when this P slice preps.
 * Fixed by the env gate, so bits stay thread-count-invariant. */
/* `H` and `stride` are the CODED PICTURE's, not the frame's. They are the same
 * thing for a frame picture and they are not for a field one: a field is half
 * the rows at twice the stride, and walking a frame's worth of rows down a
 * field view reads the OTHER parity's rows interleaved with it -- including,
 * when the reference is the current pair's first field, the rows this very
 * picture is about to reconstruct into. Those rows hold the previous frame's
 * recon in a reused encoder and zeroed pages in a fresh one, which is a
 * bitstream that depends on how the GOPs were handed out. Found by filling
 * every plane allocation with a constant: the output moved with the constant
 * (32672 bytes at 0, 26644 at 128), and t1 and t8 agreed as soon as it was
 * filled at all. */
/* Sum of a plane's samples. A row fits a 32-bit accumulator (width times
 * the largest sample), which lets the row loop vectorise. */
static uint64_t plane_sum(const pixel *p, int W, int H, int stride)
{
    uint64_t t = 0;
    for (int y = 0; y < H; y++) {
        const pixel *r = p + (size_t)y * stride;
        uint32_t s = 0;
        for (int x = 0; x < W; x++) s += r[x];
        t += s;
    }
    return t;
}

/* Explicit weights are kept only where the lookahead measured the frame-DC
 * weight saving at least WP_GATE_PCT% of the lowres SATD against the previous
 * anchor (lr_wp_vote). A fade is global, so the answer applies to every
 * list-0 reference of the slice; no measurement, no weight. */
#define WP_GATE_PCT 8

/* `sumsrc` is the coded picture's plane_sum, taken once for every reference. */
static int estimate_wp_luma(yah264_encoder_t *e, uint64_t sumsrc,
                            const pixel *ref, int denom, int64_t sumref_ovr,
                            int H, int stride, int *w, int *o)
{
    int W = e->width;
    uint64_t sumref = sumref_ovr >= 0 ? (uint64_t)sumref_ovr
                                      : plane_sum(ref, W, H, stride);
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

/* One weighted sample, exactly as 8.4.2.3.1 spells it (and as apply_wp_luma
 * applies it): the offset is signalled in the 8-bit domain and the decoder
 * scales it up. */
static inline int wp_sample(int r, int w, int o, int denom)
{
    int rnd = denom ? (1 << (denom - 1)) : 0;
    int v = ((r * w + rnd) >> denom) + o * (1 << (Y264_BIT_DEPTH - 8));
    return v < 0 ? 0 : (v > PIXEL_MAX ? PIXEL_MAX : v);
}

/* --weightp 2: refine one reference's pair against the pixels, and ask whether
 * the reference is worth carrying twice.
 *
 * The estimate above is a DC ratio. It says what the fade did to the frame's
 * average, and it cannot say whether the frame as a whole is better predicted
 * with it. This walks a decimated grid -- one sample in each 4x4, so the whole
 * picture at a sixteenth of its samples, which is the resolution the lookahead
 * already analyses at -- scores the candidate pairs around the seed by SAD at
 * zero motion, and then scores the winner TILE BY TILE, one tile per
 * macroblock, against the plain reference.
 *
 * The tile count is what separates the two modes. A weight that wins on every
 * tile is a whole-frame fade, and one weight on the reference's own slot
 * carries it. A weight that wins on some tiles and loses on others is the case
 * a second slot is for, because there the per-macroblock choice is a real one.
 * Zero-motion SAD is a cheap oracle and does not have to be the encoder's own
 * metric: it only has to rank the two slots the way the mode decision will,
 * and the mode decision then answers for itself on every macroblock.
 *
 * Reads the reference plane, so the caller owes the same readability rule the
 * estimate above owes: a reference the staircase may still be writing is not
 * a candidate. */
static void refine_wp_luma(const pixel *src, const pixel *ref, int denom,
                           int W, int H, int stride, int *w, int *o,
                           int *nwin, int *ntile, long *gain, long *plain)
{
    const int STEP = 4;
    int bw = *w, bo = *o;
    int nw = 0, nt = 0;
    long g = 0, pl = 0;
    for (int y0 = 0; y0 < H; y0 += 16)
        for (int x0 = 0; x0 < W; x0 += 16) {
            long cp = 0, cw = 0;
            for (int y = y0; y < y0 + 16 && y < H; y += STEP)
                for (int x = x0; x < x0 + 16 && x < W; x += STEP) {
                    int sv = src[y * stride + x], rv = ref[y * stride + x];
                    cp += labs((long)sv - rv);
                    cw += labs((long)sv - wp_sample(rv, bw, bo, denom));
                }
            nt++;
            pl += cp;
            if (cw < cp) { nw++; g += cp - cw; }
        }
    *w = bw; *o = bo; *nwin = nw; *ntile = nt; *gain = g; *plain = pl;
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

/* The POC no reference of the picture being coded may sit below, or -1 for no
 * bound at all. With --open-gop it is the most recent recovery point at or
 * below this picture's own POC: everything after the key answers to the key,
 * and the key's leading B frames -- output BEFORE it, and so outside what its
 * recovery_point promises -- answer to the key before that one instead. */
static int rp_bound(const yah264_encoder_t *e)
{
    /* --intra-refresh: the frame before this picture's sweep began, the last
 * one a decoder that joined at the previous sweep is promised exact. */
    if (e->ir_on)
        return e->ir_bound_poc;
    return e->poc > e->rp_poc ? e->rp_poc : e->rp_poc_prev;
}

/* Build this slice's RefPicList0 as plane pointers + POCs, mirroring the
 * decoder's list exactly. Non-pyramid: the refring (anchors most-recent-first,
 * which is both the P default PicNum-descending order and the B past-refs
 * POC-descending order). Pyramid: from the DPB — P sorts by PicNum descending
 * and pins cur_ref_l0_fn first (matching the emitted reorder command); B takes
 * past references by POC descending (the default initialisation, list 0 is
 * bounded to past refs so list 1's future anchor never aliases it). Returns
 * num_ref_idx_l0_active.
 *
 * `fn` takes each entry's frame_num. A frame list never needed it -- the one
 * reorder command the pyramid writes is derived from cur_ref_l0_fn -- but a
 * FIELD list names every entry by a PicNum built out of it, so the field
 * builder below reads it back rather than re-deriving it per path. */
static int build_list0(yah264_encoder_t *e, int type,
                       const pixel *pl[16][3], int poc[16], int fn[16])
{
    if (!e->b_pyramid) {
        if (e->nref <= 1) {
            for (int c = 0; c < 3; c++) pl[0][c] = e->ref[c];
            poc[0] = e->ref0_poc;
            fn[0] = e->ref0_fn;
            return 1;
        }
        int n = e->nref_valid < 1 ? 1 : (e->nref_valid > e->nref ? e->nref : e->nref_valid);
        /* --open-gop: the ring is most-recent-first, so cutting it at the first
 * entry below the bound is the whole of the restriction. Inert at bound
 * -1, which is every encode that is not an open GOP. */
        int bnd = rp_bound(e);
        if (bnd >= 0)
            for (int i = 0; i < n; i++)
                if (e->refring_poc[i] < bnd) { n = i < 1 ? 1 : i; break; }
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 3; c++) pl[i][c] = e->refring[i][c];
            poc[i] = e->refring_poc[i];
            fn[i] = e->refring_fn[i];
        }
        return n;
    }
    int cand[16], nc = 0;
    for (int i = 0; i < e->dpb_size && nc < 16; i++)
        if (e->dpb[i].used) cand[nc++] = i;
    /* --open-gop: drop everything from before the recovery point this picture
 * is answerable to. */
    {
        int bnd = rp_bound(e);
        if (bnd >= 0) {
            int keep = 0;
            for (int i = 0; i < nc; i++)
                if (e->dpb[cand[i]].poc >= bnd) cand[keep++] = cand[i];
            if (keep) nc = keep;
        }
    }
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
        fn[i] = e->dpb[cand[i]].frame_num;
    }
    if (nc == 0) {                                 /* should not happen; fall back */
        for (int c = 0; c < 3; c++) pl[0][c] = e->ref[c];
        poc[0] = e->ref0_poc;
        fn[0] = e->ref0_fn;
    }
    return n;
}

/* PAFF: how many reference FRAMES stand behind the current picture. The
 * pyramid can count its DPB; the ring path cannot, because at --ref 1 there is
 * no ring to count and e->ref is simply whatever was coded last -- including,
 * right after an IDR, a picture from the GOP before it. A frame list never had
 * to ask, since a P or B frame always has an anchor in front of it. A FIELD
 * list does: the second field of an IDR frame has nothing behind it but its
 * own first field, and that is the whole point of the case. */
static int fld_navail(const yah264_encoder_t *e)
{
    if (e->b_pyramid) {
        int n = 0;
        for (int i = 0; i < e->dpb_size; i++) n += e->dpb[i].used ? 1 : 0;
        return n;
    }
    return e->ref_navail;
}

/* PAFF (8.2.4.2.5): rewrite a list of reference FRAMES as the list of reference
 * FIELDS this field picture predicts from.
 *
 * The standard orders fields out of an ordered list of frames by taking them
 * alternately -- one of the CURRENT parity, one of the other, one of the
 * current, and so on down the frame list, skipping any frame that has no
 * reference field of the parity being asked for, and finishing with whatever
 * one scan has left when the other runs out. That is the order built here.
 *
 * The current pair's FIRST field joins it as the newest entry as soon as the
 * pair is a reference pair. It is the newest by both of the orderings a frame
 * list can carry (its frame_num is the current one; its POC is the largest at
 * or below this field's), and it offers only the OTHER parity, so it is
 * skipped by the same-parity scan and taken first by the opposite one. That
 * single entry is what lets an I frame's second field be a P field instead of
 * a second intra field, and it is the only reason 8.4.1.4's chroma correction
 * had to come in: every other entry here can be, and usually is, same-parity.
 *
 * --ref counts FIELDS here, because num_ref_idx_l0_active does. --ref 1
 * therefore still names exactly the nearest same-parity field, which is the
 * list PAFF-1 built, and only a deeper list reaches the other parity. */
static int build_field_list0(yah264_encoder_t *e, int is_ref,
                             const pixel *pl[16][3], int poc[16], int fn[16],
                             int par[16], int nframes)
{
    /* Candidate FRAMES, newest first, with the parities of each that are
 * reference fields. The planes stay frame planes: a field view is that
 * pointer plus one row per parity, with the stride doubled by the caller. */
    const pixel *cpl[17][3];
    int cpoc[17], cfn[17], chas[17][2], nc = 0;
    int cur = e->fld_parity, opp = !cur;
    if (e->fld_second && is_ref) {
        /* e->rec is where the pair's FIRST field reconstructed, and still is:
 * the buffer rotates only after the pair. That holds on the serial path
 * and on the W2 emit-overlap, which both aim fw->rec at e->rec; it would
 * NOT hold for a pipelined leaf, which reconstructs into a private
 * buffer, and those levers decline under field coding for that among
 * other reasons (fpipe_ready, stair_clamp_on). Whoever re-arms one has
 * to hand the pair's recon base in rather than read it here. */
        for (int c = 0; c < 3; c++) cpl[nc][c] = e->rec[c];
        cpoc[nc] = e->poc - cur;            /* the pair's frame POC (its top field's) */
        cfn[nc] = e->frame_num;             /* a pair shares one frame_num */
        chas[nc][cur] = 0;                  /* this field is the one being coded */
        chas[nc][opp] = 1;
        nc++;
    }
    for (int i = 0; i < nframes && nc < 17; i++) {
        for (int c = 0; c < 3; c++) cpl[nc][c] = pl[i][c];
        cpoc[nc] = poc[i];
        cfn[nc] = fn[i];
        chas[nc][0] = chas[nc][1] = 1;      /* a stored pair carries both fields */
        nc++;
    }
    /* --ref counts reference FRAMES, here as everywhere else, and a frame
 * offers two fields -- so the list is up to twice as long as a frame
 * picture's and reaches the same distance back. Reading --ref as a field
 * count instead was measured and lost: it halves the reach, and on the
 * three camera clips it cost 0.3 dB at equal bits where this keeps the
 * reach and takes 10-15% off the bits. 16 is what the frame contract and
 * the weight table carry. */
    int cap = 2 * e->nref;
    if (cap > 16) cap = 16;
    int n = 0, idx[2] = { 0, 0 }, turn = cur, dry = 0;
    while (n < cap && dry < 2) {
        int t = (turn == cur) ? 0 : 1;      /* 0 = the same-parity scan, 1 = the other */
        while (idx[t] < nc && !chas[idx[t]][turn]) idx[t]++;
        if (idx[t] >= nc) { dry++; turn = !turn; continue; }
        dry = 0;
        int k = idx[t]++;
        for (int c = 0; c < 3; c++)
            pl[n][c] = cpl[k][c] + (size_t)turn * e->pstride[c];
        poc[n] = cpoc[k] + turn;
        fn[n] = cfn[k];
        par[n] = turn;
        n++;
        turn = !turn;
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
/* --- lazy-hpel probe, dead by default and on no hot path (a few calls per
 * frame): Y264_HPEL_PROBE=1 accumulates the wall time of every half-pel build
 * and prints it at close. Single-threaded measurement.
 * The census (which bands are ever read) lives in motion.c. */
static int hpel_probe_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_HPEL_PROBE"); v = s && atoi(s) ? 1 : 0; }
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
    /* The SHIPPED band-aggregate bank (default: e->bseedb). A staircase burst
 * points this at its own captured copy for the same reason bseed_src is
 * captured: an async chain preps while arrivals refill the encoder's. */
    int32_t *const *bseedb_src;
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
/* refb_poc is a PARAMETER and not read here because of who calls this: an
 * async staircase chain builds its frame_work on a pool thread while the
 * arrival side rewrites e->nbuf and e->bpoc for the next mini-GOP. The chain
 * has its own captured shape and passes that; a caller on the arrival side
 * passes the encoder's through fw_default below. */
static void fw_default_shape(const yah264_encoder_t *e, struct frame_work *fw, int refb_poc)
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
    fw->bseedb_src = (int32_t *const *)e->bseedb;
    fw->bseed_valid = e->bseed_valid;
    fw->bseed_poc0 = e->bseed_poc0;
    fw->bseed_poc1 = e->bseed_poc1;
    fw->lr_seed_mvx = e->lr_seed_mvx;
    fw->lr_seed_mvy = e->lr_seed_mvy;
    fw->lr_seed_cost = e->lr_seed_cost;
    fw->refb_poc = refb_poc;
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
static void fw_default(const yah264_encoder_t *e, struct frame_work *fw)
{
    fw_default_shape(e, fw, stair_refb_poc(e->nbuf, e->bpoc));
}

static int stair_clamp_on(const yah264_encoder_t *e);

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
 * and 12 ms under CRF (2026-09-04, Y264_STAIR_STAT). */
    return stair_bdepth_on() && stair_wide_on() &&
           stair_clamp_on(e) && e->nref >= 2 &&
           (!e->rcp_on || e->rcp_lag > 0);
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
 * the scan stops at the first empty slot. The twin of mbcode.c's
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
int y264_mb_dauto_stride(void);   /* the auto score's sample stride (mbcode.c); its decay bound scales with it */
int y264_band_rows(void);        /* macroblock rows per band (mbcode.c): the lookahead
 * stash and the frame-open pass must partition rows alike */
    extern long y264_tdir_mb[2];

static void write_slice_header(y264_bs_t *bs, const struct slice_hdr *h, int first_mb)
{
    y264_bs_write_ue(bs, first_mb);                 /* first_mb_in_slice */
    y264_bs_write_ue(bs, h->slice_type_ue);
    y264_bs_write_ue(bs, h->pps_id);
    y264_bs_write(bs, h->frame_num_bits, h->frame_num);
    /* A sequence that may carry fields has every slice say which it is, and a
 * field picture then says which parity. */
    if (h->field_flag) {
        y264_bs_write1(bs, h->field_pic);           /* field_pic_flag */
        if (h->field_pic)
            y264_bs_write1(bs, h->bottom_field);    /* bottom_field_flag */
    }
    if (h->is_idr)
        y264_bs_write_ue(bs, h->idr_pic_id);        /* idr_pic_id */
    if (h->poc_type == 0)
        y264_bs_write(bs, h->poc_bits, h->poc_lsb);
    if (h->type == 2)
        y264_bs_write1(bs, h->direct_spatial);      /* direct_spatial_mv_pred_flag */
    if (h->type == 1 || h->type == 2) {
        if (h->active_ref > 1) {
            y264_bs_write1(bs, 1);                  /* num_ref_idx_active_override */
            y264_bs_write_ue(bs, h->active_ref - 1);/* num_ref_idx_l0_active_minus1 */
            if (h->type == 2)
                y264_bs_write_ue(bs, 0);            /* num_ref_idx_l1_active_minus1 */
        } else {
            y264_bs_write1(bs, 0);                  /* num_ref_idx_active_override */
        }
        if (h->l0_mod_n > 0) {
            /* See build_slice_prep: one command per entry of the list, each
 * naming that reference by its PicNum as a modulo subtraction from
 * the running predecessor. Every field picture writes it, and so
 * does a frame carrying a `--weightp 2` duplicate slot. */
            y264_bs_write1(bs, 1);                  /* ref_pic_list_modification_flag_l0 */
            for (int i = 0; i < h->l0_mod_n; i++) {
                y264_bs_write_ue(bs, 0);            /* idc 0: abs_diff subtract */
                y264_bs_write_ue(bs, h->l0_mod_absm1[i]);
            }
            y264_bs_write_ue(bs, 3);                /* idc 3: end */
        } else if (h->l0_reorder_diff > 0) {
            y264_bs_write1(bs, 1);                  /* ref_pic_list_modification_flag_l0 */
            y264_bs_write_ue(bs, 0);                /* idc 0: abs_diff subtract */
            y264_bs_write_ue(bs, h->l0_reorder_diff - 1);   /* abs_diff_pic_num_minus1 */
            y264_bs_write_ue(bs, 3);                /* idc 3: end */
        } else {
            y264_bs_write1(bs, 0);                  /* ref_pic_list_modification_l0 */
        }
        if (h->type == 2)
            y264_bs_write1(bs, 0);                  /* ref_pic_list_modification_l1 */
    }
    /* pred_weight_table for explicit P-slice weighted prediction: one luma
 * weight/offset per active list-0 reference (chroma stays identity). */
    if (h->wp_on) {
        y264_bs_write_ue(bs, h->wp_denom);          /* luma_log2_weight_denom */
        y264_bs_write_ue(bs, 0);                    /* chroma_log2_weight_denom */
        for (int i = 0; i < h->active_ref; i++) {
            y264_bs_write1(bs, h->wp_luma[i]);      /* luma_weight_l0_flag */
            if (h->wp_luma[i]) {
                y264_bs_write_se(bs, h->wp_w[i]);   /* luma_weight_l0 */
                y264_bs_write_se(bs, h->wp_o[i]);   /* luma_offset_l0 */
            }
            y264_bs_write1(bs, 0);                  /* chroma_weight_l0_flag (identity) */
        }
    }
    /* dec_ref_pic_marking only for reference pictures. */
    if (h->is_ref) {
        if (h->is_idr) {
            y264_bs_write1(bs, 0);                  /* no_output_of_prior_pics_flag */
            y264_bs_write1(bs, 0);                  /* long_term_reference_flag */
        } else {
            y264_bs_write1(bs, 0);                  /* adaptive_ref_pic_marking_mode */
        }
    }
    if (h->cabac_init)
        y264_bs_write_ue(bs, 0);                    /* cabac_init_idc */
    y264_bs_write_se(bs, h->qp_delta);              /* slice_qp_delta */
    /* In-loop deblocking on every slice type; the bS derivation handles B's
 * dual-list motion (loopfilter.c strength). Reference B's in the b-pyramid
 * store their filtered recon into the DPB (dpb_store runs after build_slice).
 * idc 0 and not 2: the filter runs across slice edges, so cutting a picture
 * into slices never shows as a seam. */
    y264_bs_write_ue(bs, h->deblock ? 0 : 1);       /* disable_deblocking_filter_idc */
    if (h->deblock) {
        y264_bs_write_se(bs, h->deblock_a);         /* slice_alpha_c0_offset_div2 */
        y264_bs_write_se(bs, h->deblock_b);         /* slice_beta_offset_div2 */
    }
}

/* --nr, the driver half (nr.h, encoder.h). */
#define Y264_NR_RING 64
struct y264_nr_slot {
    y264_nr_tally *rows;        /* one line per macroblock row */
    uint16_t off[Y264_NR_SLOTS];
    int state;                  /* 0 free, 1 analysing, 2 done (atomic) */
    int seq;
};

/* Resolved once at open, on the opening thread: Y264_NR_INTRA / Y264_NR_INTER
 * (the CLI's --nr sets both), else 0, off. Environment rather than a param
 * field, so yah264_param_t and the public header keep their layout. */
static void nr_open(yah264_encoder_t *e)
{
    const char *v;
    e->nr_intra = (v = getenv("Y264_NR_INTRA")) ? atoi(v) : 0;
    e->nr_inter = (v = getenv("Y264_NR_INTER")) ? atoi(v) : 0;
    /* the spatial noise reading (nr.c), in 8-bit levels, below which a
     * picture is coded without the filter. 0.75 disarms the clean sources, where the band measured the filter taking
     * structure rather than noise (sintel_720p +7.58% ungated, akiyo_cif
     * +1.19%, samsung_720p +1.03%; all three read under 0.65). */
    e->nr_gate_q8 = (int)(0.75 * 256.0 + 0.5);
    e->nr_trace = (v = getenv("Y264_NR_TRACE")) ? atoi(v) : 0;
    if (e->nr_intra < 0) e->nr_intra = 0;
    if (e->nr_inter < 0) e->nr_inter = 0;
    if (e->nr_intra > Y264_NR_MAX) e->nr_intra = Y264_NR_MAX;
    if (e->nr_inter > Y264_NR_MAX) e->nr_inter = Y264_NR_MAX;
    if (!e->nr_intra && !e->nr_inter)
        return;
    e->nr = calloc(1, sizeof(*e->nr));
    e->nr_ring = calloc(Y264_NR_RING, sizeof(*e->nr_ring));
    int ok = e->nr && e->nr_ring;
    for (int i = 0; ok && i < Y264_NR_RING; i++) {
        e->nr_ring[i].rows = calloc((size_t)e->height_in_mbs, sizeof(y264_nr_tally));
        e->nr_ring[i].seq = -1;
        ok = e->nr_ring[i].rows != NULL;
    }
    if (!ok) {
        fprintf(stderr, "yah264: --nr: out of memory; off\n");
        for (int i = 0; e->nr_ring && i < Y264_NR_RING; i++) free(e->nr_ring[i].rows);
        free(e->nr_ring); free(e->nr);
        e->nr_ring = NULL; e->nr = NULL;
        return;
    }
    pthread_mutex_init(&e->nr_mx, NULL);
}

static void nr_close(yah264_encoder_t *e)
{
    if (!e->nr)
        return;
    for (int i = 0; i < Y264_NR_RING; i++) free(e->nr_ring[i].rows);
    free(e->nr_ring);
    free(e->nr);
    pthread_mutex_destroy(&e->nr_mx);
}

/* Fold every finished slot, in coding order, then hand this frame a slot:
 * a frozen copy of the re-solved offsets and zeroed tally lines. A frame
 * that finds the ring full codes without the filter rather than waiting. */
static void nr_prep(yah264_encoder_t *e, y264_frame_t *f)
{
    f->nr_off = NULL; f->nr_rows = NULL; f->nr_cur = NULL; f->nr_done = NULL;
    if (!e->nr)
        return;
    int hmb = e->height_in_mbs;
    pthread_mutex_lock(&e->nr_mx);
    while (e->nr_fold_next < e->nr_seq) {
        struct y264_nr_slot *sl = &e->nr_ring[e->nr_fold_next % Y264_NR_RING];
        if (sl->seq != e->nr_fold_next ||
            __atomic_load_n(&sl->state, __ATOMIC_ACQUIRE) != 2)
            break;
        y264_nr_tally t;
        memset(&t, 0, sizeof t);
        for (int r = 0; r < hmb; r++) {
            for (int i = 0; i < Y264_NR_SLOTS; i++) t.sum[i] += sl->rows[r].sum[i];
            for (int c = 0; c < Y264_NR_CATS; c++) t.cnt[c] += sl->rows[r].cnt[c];
        }
        y264_nr_fold(e->nr, &t);
        memset(sl->rows, 0, (size_t)hmb * sizeof(*sl->rows));
        __atomic_store_n(&sl->state, 0, __ATOMIC_RELAXED);
        e->nr_fold_next++;
    }
    y264_nr_offsets(e->nr, e->nr_intra, e->nr_inter);
    int gated = 0;
    if (e->nr_gate_q8 > 0 || e->nr_trace) {
        int rd = y264_nr_noise_q8(f->src[0], f->src_stride[0], e->width,
                                  e->fld_pic ? e->height / 2 : e->height);
        gated = rd < e->nr_gate_q8;
        if (e->nr_trace)
            fprintf(stderr, "nr poc=%d noise=%.2f gated=%d\n", e->poc, rd / 256.0, gated);
    }
    struct y264_nr_slot *sl = &e->nr_ring[e->nr_seq % Y264_NR_RING];
    if (__atomic_load_n(&sl->state, __ATOMIC_ACQUIRE) == 0) {
        sl->seq = e->nr_seq++;
        sl->state = 1;
        if (gated)
            memset(sl->off, 0, sizeof sl->off);     /* still tallied: the estimate
                                                    * keeps tracking the clip */
        else
            memcpy(sl->off, e->nr->off, sizeof sl->off);
        f->nr_off = sl->off;
        f->nr_rows = sl->rows;
        f->nr_done = &sl->state;
    }
    pthread_mutex_unlock(&e->nr_mx);
}

static void build_slice_prep(yah264_encoder_t *e, int type, int is_idr, int is_ref,
                             pixel *const src[3], uint8_t *rbsp, size_t rbsp_cap,
                             const struct frame_work *fw,
                             y264_bs_t *bs_out, y264_frame_t *f_out,
                             int *fqp_out, int *deblock_out, struct slice_hdr *hdr_out)
{
    y264_bs_t bs;
    struct slice_hdr h;
    memset(&h, 0, sizeof h);
    y264_bs_init(&bs, rbsp, rbsp_cap);
    /* PAFF: the per-4x4 motion grids stay frame-sized and each field owns half
 * of one -- a field macroblock row covers two frame ones, so fld_hmb rows
 * of cells is exactly half the grid, and the two parities sit at offsets 0
 * and half. Everything that walks a grid by index rather than through the
 * frame contract walks [fld_mv0, fld_mv0 + fld_mvn) instead of the whole of
 * it. A frame picture owns all of it, which is what these two read as. */
    size_t fld_mvn = (size_t)e->mv_stride * e->height_in_mbs * 4, fld_mv0 = 0;
    if (e->fld_pic) { fld_mvn /= 2; fld_mv0 = (size_t)e->fld_parity * fld_mvn; }
    int fqp = frame_qp(e, type, is_ref);
    int fcqp = y264_chroma_qp(fqp, e->param.chroma_qp_index_offset);
    if (!(e->fld_pic && e->fld_second) &&
        e->fstats_count < (int)(sizeof e->fstats / sizeof e->fstats[0])) {
        yah264_frame_stats_t *st = &e->fstats[e->fstats_count++];
        st->disp = e->cur_disp; st->type = type; st->is_idr = is_idr; st->is_ref = is_ref; st->qp = fqp;
    }
    /* num_ref_idx_l0_active for this slice (list 1 stays single-ref for B). */
    const pixel *l0p[16][3];
    int l0poc[16], l0fn[16], l0par[16];
    for (int i = 0; i < 16; i++) l0par[i] = e->fld_parity;
    int active_ref = (type != 0) ? build_list0(e, type, l0p, l0poc, l0fn) : 1;
    /* PAFF: build_list0 hands back FRAMES, most recent first, and a field
 * picture predicts from FIELDS. build_field_list0 turns the one into the
 * other in the order 8.2.4.2.5 specifies, and hands back the parity each
 * entry was taken at -- which is what decides both the chroma correction
 * below and the PicNum the slice header names it by. */
    if (e->fld_pic && type != 0) {
        /* The frame set is what build_list0 actually FILLED, never more: it
 * stops at --ref entries and at what the ring or the DPB holds, and
 * reading past that is reading uninitialised pointers. Clamped again by
 * what stands behind this picture at all, which is 0 right after an
 * IDR. */
        int nfr = active_ref, av = fld_navail(e);
        if (nfr > av) nfr = av;
        int nf = build_field_list0(e, is_ref, l0p, l0poc, l0fn, l0par, nfr);
        if (nf > 0)
            active_ref = nf;
        else
            for (int i = 0; i < active_ref; i++) {   /* nothing to draw on: keep
 * this parity's view of what
 * build_list0 already gave */
                for (int c = 0; c < 3; c++)
                    l0p[i][c] += (size_t)e->fld_parity * e->pstride[c];
                l0poc[i] += e->fld_parity;
            }
    }
    /* v3 depth clamp eligibility for this slice: a P's list-0 searches against
 * the previous anchor take the fixed vertical clamp (its recon may still
 * be streaming), keyed by POC. Machine-invariant: env gate + parameters
 * only, so bits never depend on threads or engagement. */
    int clamp0_poc = (type == 1 && stair_clamp_on(e))
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
 * the condition width engages on. The two must never disagree: a wide burst
 * at --ref > 1 with the hop-2 clamp disarmed would search a reference that
 * is still streaming. */
    int clamp0_hop2 = (clamp0_poc >= 0 && stair_wide_on() &&
                       e->nref >= 2 && !e->rcp_on)
                    ? e->prev_anchor_poc2 : -1;
    /* v6: and the live predecessors' REFERENCE B's, the pictures that actually
 * sit at the front of a deep list 0 while they stream. Same shape of test as
 * hop 2 -- width, plus a list deep enough to reach past the pinned previous
 * anchor (at --ref 1 a P list 0 is exactly {previous anchor}, so nothing
 * here is reachable) -- and gated on BDEPTH as well, because that is what
 * decides whether a reference B publishes per row at all. Without it the
 * clamp would cost search reach and buy no overlap, so the two move
 * together. NOT conditioned on the multi-hop clamp: a different picture and
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
    int *wp_luma = h.wp_luma, *wp_w = h.wp_w, *wp_o = h.wp_o, wp_denom = 5;
    h.wp_denom = wp_denom;
    h.wp_on = (type == 1 && e->pps.weighted_pred_flag);
    /* The picture the estimate walks: this field, or the whole frame. */
    const pixel *wp_src = src[0] + (size_t)(e->fld_pic ? e->fld_parity : 0) * e->pstride[0];
    int wp_h = e->fld_pic ? e->height / 2 : e->height;
    int wp_stride = e->pstride[0] * (e->fld_pic ? 2 : 1);
    if (h.wp_on) {
        uint64_t wp_sumsrc = plane_sum(wp_src, e->width, wp_h, wp_stride);
        for (int i = 0; i < active_ref; i++) {
            /* Clamped ref: estimate against that anchor's SOURCE DC (cached at
 * its arrival) -- its recon is not fully readable yet. Generalizes
 * to the set for free, since the srcsum ring is POC-keyed and
 * Y264_STAIR_K deep: when this anchor preps, the ring still holds
 * every anchor the set can name. */
            /* The cached source sum is a whole FRAME's; a field picture has
 * no such cache and takes the direct walk. (The clamp set is the
 * staircase's, which field coding does not run, so this is belt
 * and braces rather than a live case.) */
            int64_t sro = (!e->fld_pic && clamp_set_has(clamp_set, l0poc[i]))
                        ? anchor_srcsum_get(e, l0poc[i]) : -1;
            wp_luma[i] = estimate_wp_luma(e, wp_sumsrc, l0p[i][0], wp_denom, sro,
                                          wp_h, wp_stride, &wp_w[i], &wp_o[i]);
            if (wp_luma[i] && e->cur_wp_vote < WP_GATE_PCT)
                wp_luma[i] = 0;
        }
    }
    /* --weightp 2: score each readable reference's weight against the picture
 * tile by tile, and where it wins on MOST of the picture but not all of it,
 * carry that reference TWICE -- once weighted, once plain -- so the mode
 * decision answers per macroblock instead of per frame. The weighted copy
 * goes immediately in FRONT of the plain one, so every other entry keeps its
 * order and only the twin moves down a slot; the header then names every
 * entry outright (see l0_mod_absm1), which is the only spelling of a list
 * that can say the same picture twice.
 *
 * One duplicate per slice at most: a second would cost a wider ref_idx on
 * every partition of the picture to buy a second answer to the same
 * question. Field pictures and `--ref 1` are refused at open, so neither is
 * a live case here. */
    int wp_dup = -1, wp_dup_of = -1;
    if (h.wp_on && e->weightp == 2 && !e->fld_pic && e->nref > 1 && active_ref < 16) {
        for (int i = 0; i < active_ref && wp_dup < 0; i++) {   /* one, at most */
            /* Same readability rule the estimate above obeys: a reference the
 * staircase may still be writing cannot be walked here, so it keeps
 * its frame-level pair and takes no duplicate. */
            if (clamp_set_has(clamp_set, l0poc[i]))
                continue;
            /* Only where the frame-level estimate already fired. That test is
 * a global luma shift of at least one level, which is what a fade
 * looks like and what a weight is FOR; zero-motion SAD on content
 * that simply moves says yes to almost any weight, because the
 * motion dominates the error it is scored against. Without this the
 * duplicate fired on half the board's P slices with no fade in
 * sight. It also makes mode 2 a strict refinement of mode 1: the
 * two answer the same question and differ only in how. */
            if (!wp_luma[i])
                continue;
            int w = wp_w[i], o = wp_o[i];
            int nwin = 0, ntile = 0;
            long gain = 0, plain = 0;
            refine_wp_luma(wp_src, l0p[i][0], wp_denom, e->width, wp_h, wp_stride,
                           &w, &o, &nwin, &ntile, &gain, &plain);
            if (ntile == 0 || (w == (1 << wp_denom) && o == 0))
                continue;                       /* identity: nothing to carry */
            /* What the weight is WORTH, against what it costs. The gain is the
 * error the per-macroblock choice actually removes -- the tiles the
 * weight loses on keep the plain slot and contribute nothing -- and
 * the slot is paid for by a wider ref_idx on every partition of the
 * picture, so a gain under a fortieth of the frame's own prediction
 * error does not cover it. Without this floor a weight one step off
 * identity flips half the tiles by rounding alone, and the duplicate
 * fires on three quarters of ordinary content (measured: 218 of 295
 * P slices over the ten board clips) for nothing. */
            if (gain * 40 < plain)
                continue;
            /* Where the weight wins on EVERY tile there is nothing to choose
 * and the reference's own slot carries it, exactly as mode 1 would:
 * a second slot would cost a ref_idx on every partition of the
 * picture to offer an escape nothing takes. Where it wins on too few
 * tiles it is not a fade at all. The duplicate is for the frame in
 * between -- the weight is right for most of the picture and wrong
 * for a part of it -- and the majority is what makes it affordable,
 * because the majority is what decides the ref_idx the frame mostly
 * codes and the motion predictors mostly agree on. */
            if (nwin == ntile) {                /* whole-frame: one slot carries it */
                wp_luma[i] = 1; wp_w[i] = w; wp_o[i] = o;
                continue;
            }
            if (nwin * 8 < ntile * 6)
                continue;
            /* The weighted copy takes the twin's own slot and the twin moves
 * down one; everything behind them shifts with it and everything in
 * front is untouched. Two things were tried before this and both
 * cost bits. Appending the copy to the END of the list gives the
 * majority the WIDEST ref_idx and leaves the escape the cheapest,
 * which is backwards. Putting it at the front of the list instead
 * reorders the whole list when the reference it copies is not the
 * first one -- the nearest reference, which most of the picture
 * predicts from, loses index 0 to a distant one -- and that cost
 * 13% of the rate on the CIF fade clip at a fixed CRF. In front of
 * its own twin is the placement that moves nothing else. */
            for (int k = active_ref; k > i; k--) {
                for (int c = 0; c < 3; c++) l0p[k][c] = l0p[k - 1][c];
                l0poc[k] = l0poc[k - 1]; l0fn[k] = l0fn[k - 1]; l0par[k] = l0par[k - 1];
                wp_luma[k] = wp_luma[k - 1]; wp_w[k] = wp_w[k - 1]; wp_o[k] = wp_o[k - 1];
            }
            wp_luma[i] = 1; wp_w[i] = w; wp_o[i] = o;
            wp_luma[i + 1] = 0;                 /* the plain half of the pair */
            active_ref++;
            wp_dup = i; wp_dup_of = i + 1;
        }
    }
    if (wp_stat_on() && type == 1) {            /* Y264_WEIGHTP_STAT: how often 2 fires */
        e->stat_wp_p++;
        if (wp_dup >= 0) e->stat_wp_dup++;
        for (int i = 0; i < active_ref; i++)
            if (i != wp_dup && wp_luma[i]) { e->stat_wp_frame++; break; }
    }
    /* Capture this frame's list POCs: when its recon later serves as the
 * co-located picture, colpoc resolves each block's refIdx to a POC. */
    e->cur_l0n = (type != 0) ? active_ref : 0;
    for (int i = 0; i < e->cur_l0n; i++) e->cur_l0poc[i] = l0poc[i];
    e->cur_l1poc0 = (type == 2) ? (e->ref1_poc + e->fld_parity) : -1;
    if (e->fld_pic) {                   /* ...and keep this field's, by parity */
        int p = e->fld_parity;
        e->fld_l0n[p] = e->cur_l0n;
        for (int i = 0; i < e->cur_l0n; i++) e->fld_l0poc[p][i] = l0poc[i];
        e->fld_l1poc0[p] = e->cur_l1poc0;
    }

    h.type = type; h.is_idr = is_idr; h.is_ref = is_ref;
    h.slice_type_ue = type == 0 ? 7 : (type == 1 ? 5 : 6);  /* I=7 P=5 B=6 */
    h.pps_id = e->pps.pps_id;
    h.frame_num_bits = e->sps.log2_max_frame_num_minus4 + 4;
    h.frame_num = e->frame_num;
    h.field_flag = !e->sps.frame_mbs_only_flag;
    h.field_pic = e->fld_pic;               /* PAFF: this picture IS one field */
    h.bottom_field = e->fld_parity;
    h.idr_pic_id = e->idr_pic_id;
    h.poc_type = e->sps.pic_order_cnt_type;
    if (h.poc_type == 0) {
        h.poc_bits = e->sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
        h.poc_lsb = e->poc & ((1 << h.poc_bits) - 1);
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
    /* Y264_DIRECT_AUTO: a per-slice vote. Fold the previous B frame's
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
    /* Slice-level temporal-direct legality (plan C2), see the slice-header
 * comment. Reaches only slices that would code temporal
 * direct (--direct temporal, or auto choosing it); the spatial default is
 * untouched. Under the default B-pyramid it sends a structural 37 of 111
 * B slices per 150 frames to spatial on every clip.
 * DEFAULT ON (2026-09-03). Rate-anchored BD vs the reference encoder's --preset medium, temporal arm,
 * five 1080p clips, 150f (spatial arm in brackets, unchanged): sunflower
 * +31.2 -> +18.5% (-7.7), pedestrian +7.9 -> +5.0 (-0.4), riverbed +4.9 ->
 * +4.0 (+2.4), station2 -32.1 -> -26.5 (-12.5), blue_sky -8.1 -> -3.0
 * (+16.5): the swing narrows at both ends, which is what a per-slice auto
 * rule needs before its score can be trusted. Always on; the env escape to
 * the unguarded temporal mode was removed on 2026-10-04. */
    int tl_illegal = active_ref > 0 && colp0 >= 0 && colp0 != l0poc[0];
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
        for (size_t i = fld_mv0; i < fld_mv0 + fld_mvn && temporal_legal; i++) {
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
        long nintra = 0;
        for (size_t i = fld_mv0; i < fld_mv0 + fld_mvn; i++) {
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
        for (size_t i = fld_mv0; i < fld_mv0 + fld_mvn; i++) {
            unsigned v = (unsigned)(uint16_t)fw->colpoc[i]
                       ^ ((unsigned)(uint16_t)fw->colmvx[i] << 8)
                       ^ ((unsigned)(uint16_t)fw->colmvy[i] << 16);
            h = (h ^ v) * 1099511628211UL;
        }
        fprintf(stderr, "] colhash=%016lx tdir_mb=%ld/%ld\n", h,
                y264_tdir_mb[0], y264_tdir_mb[1]);
        y264_tdir_mb[0] = y264_tdir_mb[1] = 0;
    }
    h.direct_spatial = !direct_temporal;
    h.active_ref = active_ref;
    /* In b-pyramid, reference B's outrank the previous anchor by FrameNum, so
 * the default P list0 would pick a B. Reorder to pin the anchor (its
 * FrameNum in cur_ref_l0_fn) at index 0. */
    {
        int maxfn = 1 << (e->sps.log2_max_frame_num_minus4 + 4);
        int diff = e->cur_ref_l0_fn >= 0
                 ? (e->frame_num - e->cur_ref_l0_fn + maxfn) % maxfn : 0;
        h.l0_reorder_diff = (type == 1 && e->b_pyramid && e->cur_ref_l0_fn >= 0)
                          ? diff : 0;
        /* PAFF: name every entry of the field list outright.
 *
 * A field's PicNum is 2*FrameNumWrap plus one when its parity is this
 * picture's, and reordering walks it relative to a running predecessor
 * that starts at CurrPicNum = 2*frame_num + 1. Writing one command per
 * entry costs a handful of bits per slice and buys the property that
 * matters: the list the decoder ends up with is the list built above,
 * and not whatever 8.2.4.2.5's default derivation would have produced
 * out of a DPB whose contents this encoder is only modelling.
 *
 * Every command is idc 0, a SUBTRACTION modulo MaxPicNum, because that
 * reaches any target from any predecessor -- including a target equal
 * to the predecessor, which is a real case: a non-reference picture
 * carries the frame_num of the reference pair in front of it, so that
 * pair's same-parity field has exactly CurrPicNum. A full turn of the
 * modulus expresses it. */
        h.l0_mod_n = 0;
        if (e->fld_pic && type != 0) {
            int maxpn = 2 * maxfn, pred = 2 * e->frame_num + 1;
            for (int i = 0; i < active_ref; i++) {
                int fnw = l0fn[i] > e->frame_num ? l0fn[i] - maxfn : l0fn[i];
                int pn = ((2 * fnw + (l0par[i] == e->fld_parity)) % maxpn + maxpn) % maxpn;
                int d = ((pred - pn) % maxpn + maxpn) % maxpn;
                if (d == 0) d = maxpn;
                h.l0_mod_absm1[i] = d - 1;
                pred = pn;
            }
            h.l0_mod_n = active_ref;
        } else if (wp_dup >= 0) {
            /* --weightp 2: a FRAME list that names the same picture twice. The
 * default derivation cannot produce it and a partial reorder cannot
 * either -- 8.2.4.3.1 drops every other copy of the picture it just
 * placed, from the slot after it onward -- so the only spelling of a
 * duplicate is to name every entry in turn, which leaves each earlier
 * copy standing behind the insertion point. A frame's PicNum is its
 * FrameNumWrap and the predecessor starts at CurrPicNum = frame_num,
 * so the duplicate's own command is a full turn of the modulus: the
 * one case the field path above already had to express. */
            int pred = e->frame_num;
            for (int i = 0; i < active_ref; i++) {
                int pn = ((l0fn[i] % maxfn) + maxfn) % maxfn;
                int d = ((pred - pn) % maxfn + maxfn) % maxfn;
                if (d == 0) d = maxfn;
                h.l0_mod_absm1[i] = d - 1;
                pred = pn;
            }
            h.l0_mod_n = active_ref;
        }
    }
    h.cabac_init = (e->pps.entropy_coding_mode_flag && type != 0);
    h.qp_delta = fqp - 26;
    int deblock = e->param.deblock;
    h.deblock = deblock;
    h.deblock_a = e->param.deblock_alpha;
    h.deblock_b = e->param.deblock_beta;
    /* The picture's first slice starts at macroblock 0; the rest are written by
 * emit_picture, which knows where each one's bitstream begins. */
    write_slice_header(&bs, &h, 0);

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
    /* --intra-refresh: this picture's refresh columns, and how far into each
 * reference the refreshed area reaches. Sweep position j codes columns
 * [j*W/period, (j+1)*W/period), so a reference at position i was refreshed
 * up to column (i+1)*W/period; a reference from before the sweep reaches
 * nothing. */
    f.ir_on = e->ir_on && type == 1 && e->ir_j >= 0;
    if (f.ir_on) {
        long w = e->width_in_mbs, per = e->ir_period, j = e->ir_j;
        f.ir_lo = (int)(j * w / per);
        f.ir_hi = (int)((j + 1) * w / per);
        for (int i = 0; i < 16; i++) {
            int r = 0;
            if (i < active_ref && f.refs_poc[i] >= e->ir_start_poc) {
                long pos = (f.refs_poc[i] - e->ir_start_poc) / 2;
                r = (int)((pos + 1) * w / per) * 16;
            }
            f.ir_refw[i] = r;
        }
    }
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
    f.direct_auto = auto_eng;
    f.dauto_acc = fw->dauto_acc;
    f.mv_stride = e->mv_stride;
    f.slice_type = type;
    f.field_pic = e->fld_pic;
    f.field_parity = e->fld_parity;
    /* 8.4.1.4: what each reference view costs chroma, read off the parity the
 * list entry was taken at. Zero for a frame picture and for every
 * same-parity field, so a progressive encode adds nothing anywhere. List 1
 * names the future anchor's SAME-parity field (the default derivation puts
 * it at index 0 and num_ref_idx_l1_active is 1), so it is always zero. */
    for (int i = 0; i < 16; i++)
        f.cmv_l0[i] = (int8_t)(e->fld_pic && type != 0
                               ? 2 * (e->fld_parity - l0par[i]) : 0);
    f.cmv_l1 = 0;
    f.deblock_on = deblock;
    f.deblock_a = e->param.deblock_alpha;
    f.deblock_b = e->param.deblock_beta;
    f.chroma_qp_off = e->param.chroma_qp_index_offset;
    f.cqm = e->cqm_on ? &e->cqm : NULL;
    f.no_mixed_refs = e->param.no_mixed_refs ? 1 : 0;
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
    f.wp_dup = wp_dup;
    f.wp_dup_of = wp_dup_of;
    /* The map 8.7.2.1 reads: the duplicate and its twin are ONE picture, and
 * the deblocking filter is the one derivation that asks about pictures
 * rather than indices. Identity everywhere else. */
    for (int i = 0; i < 16; i++) f.wp_refmap[i] = (int8_t)i;
    if (wp_dup >= 0) f.wp_refmap[wp_dup_of] = (int8_t)wp_dup;
    f.padded_w = e->padded_w;
    f.padded_h = e->padded_h;
    f.wmb = e->width_in_mbs;
    f.hmb = e->height_in_mbs;
    /* --slices: the row cuts, resolved once at open. One slice per picture
 * leaves both grids NULL, so mb_ytop stays 0 and every neighbour test in
 * mbcode.c reads as the frame-edge test it always was. */
    f.nslices = e->nslices;
    f.slice_row0 = e->nslices > 1 ? e->slice_row0 : NULL;
    f.slice_y0 = e->nslices > 1 ? e->slice_y0 : NULL;
    f.mb_ytop = 0;
    if (e->fld_pic) {
        /* Everything that says "picture" now means this FIELD: half the rows,
 * every plane starting at the parity's first row, every stride doubled.
 * The per-macroblock grids stay frame-sized and are simply used down to
 * fld_hmb rows -- they are rewritten per coded picture and nothing reads
 * past the picture. */
        f.padded_h = e->padded_h / 2;
        f.hmb = e->fld_hmb;
        for (int c = 0; c < 3; c++) {
            size_t off = (size_t)e->fld_parity * e->pstride[c];
            f.src[c] += off; f.src_stride[c] *= 2;
            f.rec[c] += off; f.rec_stride[c] *= 2;
            f.ref[c] = (type != 0) ? l0p[0][c] : e->ref[c] + off;
            f.ref_stride[c] *= 2;
            /* List 1 is the future anchor's SAME-parity field: the default
 * derivation puts it at index 0 and num_ref_idx_l1_active is 1, so
 * there is nothing to reorder and nothing for the chroma correction
 * to do. A P or I field leaves it null, as a frame one does. */
            f.ref1[c] = (type == 2 && f.ref1[c]) ? f.ref1[c] + off : NULL;
            f.ref1_stride[c] *= 2;
        }
        f.poc_l0 = (type != 0) ? l0poc[0] : e->poc;
        f.poc_l1 = (type == 2) ? e->ref1_poc + e->fld_parity : e->poc;
        if (type == 0)
            for (int c = 0; c < 3; c++)
                f.refs[0][c] = f.ref[c];
        /* The motion grids stay frame-sized and each field owns half of one:
 * a field macroblock row covers two frame ones, so fld_hmb rows of
 * 4x4 cells is exactly half the grid, and the two parities sit at
 * offsets 0 and half. That is what lets the single dpb_store after
 * the pair carry BOTH fields' motion, which is what a B field's
 * co-located read then picks its own parity out of. */
        size_t mvhalf = (size_t)e->mv_stride * e->fld_hmb * 4;
        size_t mo = (size_t)e->fld_parity * mvhalf;
        f.mvx += mo; f.mvy += mo; f.refidx += mo;
        f.mvx1 += mo; f.mvy1 += mo; f.refidx1 += mo;
        f.mvdx += mo; f.mvdy += mo; f.mvdx1 += mo; f.mvdy1 += mo;
        if (f.colmvx) { f.colmvx += mo; f.colmvy += mo; f.colref += mo; f.colpoc += mo; }
    }
    f.cf_idc = e->cf_idc;
    f.sub_w = e->sub_w;
    f.sub_h = e->sub_h;
    f.cbw = 4 / e->sub_w;
    f.cbh = 4 / e->sub_h;
    f.subme = e->param.subme > 0 ? e->param.subme : 10;
    f.lev_max = e->lev_max;
    f.partitions = yah264_partitions_resolved(&e->param);
    f.slice_is_ref = is_ref;
    f.bskip_probe = e->bskip_probe;
    f.bskip_screen = e->bskip_screen;
    f.b_preme_skip = e->b_preme_skip;
    f.b_preme_bits = e->b_preme_bits;
    f.p_part_gate = e->p_part_gate;
    f.rd_surv_rank = e->rd_surv_rank;
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
    f.ts_mbx = -1; f.ts_mby = -1;   /* temporal-seed memo: empty */
    f.te_skip = 0;
    f.i16s_mbx = -1;                /* intra screen hand-off: empty */
    f.te_rec_mbx = -1; f.te_rec_mby = -1;
    f.aq_off = e->aq_off;
    /* PROBE (Y264_MBT_BREF): apply mb-tree offsets to a B frame that is kept
 * as a reference; without it no B carries one. This tests the APPLY half
 * with the last anchor's field as a stand-in -- the real build gives the
 * reference B its own field. If a proxy field helps, the apply
 * is worth building; if it hurts, the field itself has to be the B's own. */
    /* offsets apply to any frame kept as a reference, keyed on
 * is_ref rather than on B-ness. A reference B
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
    /* PAFF: average each pair of frame macroblock rows down onto the field row
 * that covers them. A field macroblock's sixteen lines are frame lines
 * 32r..32r+31 of its parity, i.e. exactly the two frame macroblock rows
 * 2r and 2r+1, so the two offsets are the two halves of one answer. */
    if (e->fld_pic) {
        int wmb = e->width_in_mbs, hf = e->fld_hmb;
        if (f.aq_off && e->fld_aq) {
            for (int r = 0; r < hf; r++)
                for (int x = 0; x < wmb; x++)
                    e->fld_aq[r * wmb + x] = (int8_t)((f.aq_off[(2 * r) * wmb + x]
                                                     + f.aq_off[(2 * r + 1) * wmb + x]) >> 1);
            f.aq_off = e->fld_aq;
        }
        if (f.mbtree_off && e->fld_mbt) {
            for (int r = 0; r < hf; r++)
                for (int x = 0; x < wmb; x++)
                    e->fld_mbt[r * wmb + x] = (int8_t)((f.mbtree_off[(2 * r) * wmb + x]
                                                      + f.mbtree_off[(2 * r + 1) * wmb + x]) >> 1);
            f.mbtree_off = e->fld_mbt;
        }
    }
    f.mbt_frac = 0;              /* mbtree_off is in whole-QP units */
    /* P-frame lowres ME seed (ref0). Gated to the medium tier so the subme-10
 * default stays byte-identical; only for P (the anchor leg is P-vs-anchor). */
    /* type FIRST: only the anchor's serial prep may touch e->lr_seed_valid --
 * a v3 driver-side B prep runs concurrent with the API thread's pops,
 * which rewrite it (stash_lr_seed). */
    if (type == 1 && !e->fld_pic && e->lr_seed_valid &&
        (e->param.subme <= 0 || e->param.subme <= 8)) {
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
    /* The shipped band aggregates, on both the serial and the burst paths.
 * Frame geometry, so a field picture (whose macroblock rows are half the
 * frame's) does not read them. */
    f.band_c = NULL;
    if (type == 2 && !e->fld_pic && fw->bseedb_src && e->cur_bseed >= 0 &&
        e->cur_bseed < 8 && fw->bseed_valid[e->cur_bseed])
        f.band_c = fw->bseedb_src[e->cur_bseed];
    f.b_intra_band = e->b_intra_band;
    f.b8_band = e->b8_band;
    /* Y264_BLATE_STAT: attach the pair legs' lowres costs, unscaled (the
 * serial bank only -- a t1 measurement; MT paths see NULL). */
    f.lr_bseed_c0 = f.lr_bseed_c1 = f.lr_bseed_ci = NULL;
    if (type == 2 && fw->bseedc_src && e->cur_bseed >= 0 && e->cur_bseed < 8 &&
        fw->bseedc_src[e->cur_bseed][0] && fw->bseed_valid[e->cur_bseed]) {
        f.lr_bseed_c0 = fw->bseedc_src[e->cur_bseed][0];
        f.lr_bseed_c1 = fw->bseedc_src[e->cur_bseed][1];
        f.lr_bseed_ci = fw->bseedc_src[e->cur_bseed][2];
    }
    /* Content-adaptive ME (frame_motion_score): off. */
    f.me_cheap = 0;
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
    f.aq_abr = e->aq_rabr;
    f.aq_pull = (float)e->aq_pull;
    f.aq_anchor = (float)e->aq_bref;
    f.psy_rd = e->param.psy_rd;
    f.psy4k = psy4k_bits(&e->param);
    f.size_class = y264_size_class(e->param.width, e->param.height);
    f.uhdlite = uhd_lite_on(&e->param);
    /* Keyed on the USER'S total thread request, not this worker's pool width:
 * a multi-threaded encode with short GOPs hands its GOP workers 1-wide
 * pools, and stq engaging there would change MT output and cost MT speed
 * (caught by the conformance thread-canary). --threads 1 is the only
 * shape that gets the quality mode. */
    f.stq = stq_env() < 0 ? (e->param.threads == 1) : stq_env();
    f.psy_lattice = 0;
    f.psy_trellis = e->param.psy_trellis;
    f.mbvar = NULL;
    {
        int wmb = e->width_in_mbs, hmb = e->height_in_mbs, flat = 0, tex = 0;
        int64_t t = (int64_t)PSY_FLAT_VAR << 16;         /* var*256^2 domain */
        int64_t tv = (int64_t)64 << 16;                  /* textured, for the log */
        /* a frame picture's blocks are the AQ pass's blocks: keep the sums
         * (one encoding thread, so no other prep can overwrite them mid-read) */
        uint32_t *keep = (e->mbvar && !e->fld_pic && e->param.threads == 1)
                       ? e->mbvar : NULL;
        for (int mby = 0; mby < hmb; mby++)
            for (int mbx = 0; mbx < wmb; mbx++) {
                const pixel *s = src[0] + (mby * 16) * e->pstride[0] + mbx * 16;
                uint32_t v2[2];
                y264_dsp.var16x16(s, e->pstride[0], v2);
                if (keep) {
                    keep[2 * (mby * wmb + mbx)] = v2[0];
                    keep[2 * (mby * wmb + mbx) + 1] = v2[1];
                }
                int64_t var256sq = (int64_t)v2[1] * 256 - (int64_t)v2[0] * v2[0];
                if (var256sq < t) flat++;
                if (var256sq >= tv) tex++;
            }
        int share = flat * 100 / (wmb * hmb);
        int texshare = tex * 100 / (wmb * hmb);
        if (share >= PSY_FLAT_SHARE) {
            float st = PSY_FLAT_S / 256.0f;
            if (st > f.psy_trellis) { f.psy_trellis = st; f.psy_lattice = 1; }
        }
        int calm = 0;                  /* the calm-gate column: that gate is removed */
        if (psy_flat_log())
            fprintf(stderr, "psyflat: poc=%d type=%d share=%d tex=%d tdiff=%d calm=%d on=%d\n",
                    e->poc, type, share, texshare, e->cur_lr_tdiff, calm,
                    share >= PSY_FLAT_SHARE);
        f.mbvar = keep;
        f.mbvar_src = &e->mbvar_src;
        if (keep) e->mbvar_src = src[0];
    }
    f.trellis = e->param.trellis;
    nr_prep(e, &f);
    f.c4_pred = NULL; f.c4_val = 0;
    f.rd_abort = -1.0; f.rd_hdr_lb = 0; f.rd_aborted = 0;
    /* The first frame runs on the pool too: y264_mb_warm_statics resolves the
 * analyze path's lazy statics at open, so a serial warm frame would buy
 * nothing. wf_warmed still gates the staircase, which wants one completed
 * frame for its own reasons; the guarded store keeps a v3 driver-side prep
 * from writing what stair_ready reads. */
    f.pool = e->pool;
    if (!e->wf_warmed)
        e->wf_warmed = 1;
    /* Staircase hooks: off by default; the stair driver overrides after prep. */
    f.row_done = NULL; f.row_done_ctx = NULL;
    f.row_gate = NULL; f.row_gate_ctx = NULL;
    f.row_ready = NULL;
    f.brk = NULL;
    {
        int K, dmax;
        double margin, arm;
        int m = y264_rowbrake_cfg(&K, &dmax, &margin, &arm);
        /* the rule reads the per-row estimate, so it keeps the trace array */
        f.rb = (y264_rb_trace_on() || m == 1)
             ? calloc((size_t)2 * f.hmb, sizeof(int64_t)) : NULL;
        struct y264_rowbrake *b = m ? calloc(1, sizeof *b) : NULL;
        if (b && (b->q = calloc((size_t)f.hmb, 1)) != NULL) {
            b->mode = m; b->K = K; b->dmax = dmax; b->hmb = f.hmb;
            b->margin = margin; b->arm = arm; b->est = f.rb;
            b->bud = e->vbv_on && e->rcp_on ? e->rb_bud : 0.0;
            b->pred = e->rb_pred;
            f.brk = b;
        } else free(b);
    }
    /* The list-1 vertical clamp applies to every B that references the current
 * mini-GOP ANCHOR (poc_l1 == prev_anchor_poc), on EVERY code path (stair
 * driver, serial code_b_hier, fpipe pair) and at every thread count, purely
 * under the env gate -- so the bitstream changes once and stays invariant
 * to how (or whether) the concurrency engages. */
    /* v5 (Y264_STAIR_BDEPTH): the same clamp for a leaf whose list-1 is the
 * mini-GOP's still-in-flight REFERENCE B. List 1 is single-ref, so every
 * other list-1 MV producer (spatial direct/skip medians) closes over it,
 * exactly as for the anchor. */
    int refb_clamp = type == 2 && stair_bdepth_on() &&
                     stair_clamp_on(e) && fw->refb_poc >= 0;
    f.stair_clamp = type == 2 && stair_clamp_on(e) &&
                    (f.poc_l1 == e->prev_anchor_poc ||
                     (refb_clamp && f.poc_l1 == fw->refb_poc));
    /* v3 depth: the P list-0 twin (see clamp_set above) -- mbcode.c tests
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
 * Via fw so a per-leaf prep resets ITS slot's grids, never a live frame's.
 * PAFF: only THIS field's half, because the other half is the pair's first
 * field and its motion has to survive until the one dpb_store after the
 * pair carries both. */
    memset(fw->refidx + fld_mv0, -1, fld_mvn);       /* refIdx -1: int8 0xff */
    memset(fw->refidx1 + fld_mv0, -1, fld_mvn);

    /* A2: build the half-pel planes for every reference ME will search this
 * slice, and register them (thread-local) for the ME sub-pel probes. Order
 * matches f.refs / f.ref1 so ME resolves each by reference pointer. Each
 * reference's build is an independent pure filter, so they run on the pool
 * (per-worker scratch); byte-identical (no cross-ref state). */
    /* PAFF: every cached half-pel plane was filtered over the FRAME, so none of
 * them describes a field. Motion search interpolates on the fly instead,
 * which is the no-plane path and bit-identical to it -- a speed gap for
 * field coding, named in docs/options.md, not a correctness one. */
    if (e->hpel_on && type != 0 && !e->fld_pic) {
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
 * which is the no-plane path and bit-identical to it. */
                n = k;
                break;
            }
            if (!h0) {
                double t0 = hpel_probe_on() ? tprof_ms() : 0;
                y264_hpel_census_built(e->hpel_buf[k][0], -B, e->padded_h + B,
                                       e->pstride[0]);
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
    *hdr_out = h;
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
static int qp_trace_on(void)          /* warmed in warm_lr_statics */
{
    static int on = -1;
    if (on < 0) { const char *s = getenv("Y264_QP_TRACE"); on = s ? atoi(s) : 0; }
    return on;
}
static void qp_trace_frame(const yah264_encoder_t *e, const y264_frame_t *f, int type)
{
    if (!qp_trace_on() || !f->mbqp) return;
    long n = (long)e->width_in_mbs * e->height_in_mbs, sum = 0, isum = 0, nskip = 0;
    for (long i = 0; i < n; i++) {
        int q = f->qp + (f->mbtree_off ? f->mbtree_off[i]
                                       : (f->aq_off ? (int)f->aq_off[i] : 0));
        if (q < 0) q = 0; else if (q > 51) q = 51;
        sum += f->mbqp[i]; isum += q; nskip += f->mbqp[i] != q;
    }
    fprintf(stderr, "QPT type=%d poc=%d slice_qp=%d mean_qp=%.2f intended=%.2f nodelta=%.2f moff=%.2f crf_dc=%.2f map=%s\n",
            type, e->poc, f->qp, (double)sum / n, (double)isum / n, (double)nskip / n, e->mbtree_mean_off, e->crf_dc,
            f->mbtree_off ? (f->mbtree_off == e->mbtree_off ? "anchor" : "own") : (f->aq_off ? "aq" : "none"));
}

/* Close out this picture's bitstream and record what each slice cost.
 *
 * `bs` already carries slice 0's header (build_slice_prep wrote it) and, under
 * CABAC, `cb` is the engine that header was aligned for and that analyze
 * priced against. Every further slice gets its own header, its own entropy
 * coder started from the same slice-init state -- same SliceQPY, same slice
 * type, so the CABAC contexts are the same table -- and its own byte range,
 * packed straight after its predecessor in the same RBSP buffer so the caller
 * can hand each range to append_nal in coding order.
 *
 * At one slice per picture this is exactly the five open-coded tails it
 * replaces: one emit, one trailing/zero-word finish, one length. */
static void emit_picture(y264_frame_t *f, y264_emit_job_t *job,
                         const struct slice_hdr *h, y264_bs_t *bs,
                         y264_cabac_t *cb, int cabac, struct slice_map *sm)
{
    int n = f->nslices > 0 ? f->nslices : 1;
    uint8_t *end = bs->end;
    sm->n = 0;
    for (int s = 0; s < n; s++) {
        if (s > 0) {
            uint8_t *next = cabac ? cb->p : bs->p;
            y264_bs_init(bs, next, (size_t)(end - next));
            write_slice_header(bs, h, f->slice_row0[s] * f->wmb);
            if (cabac) {
                while (y264_bs_pos_bits(bs) & 7)
                    y264_bs_write1(bs, 1);          /* cabac_alignment_one_bit */
                y264_cabac_init_engine(cb, bs->p);
                y264_cabac_set_end(cb, bs->end - 64);
                /* ...which cleared the engine's FIELD flag, and a field
 * picture's second slice would then have written its residual
 * blocks in the frame scan out of the frame half of the context
 * set. The flag is a property of the PICTURE, not of one slice's
 * engine, so it is re-applied at every re-init. The first
 * --slices 4 field encode decoded as garbage from slice 1 on. */
                cb->field = f->field_pic;
                /* The contexts are restored from the job's slice-init copy by
 * the emit itself, which is where the single-slice path got them
 * too -- every slice of a picture inits from the same SliceQPY. */
            }
        }
        y264_frame_emit_slice(bs, f, job, s);
        int nmb = f->wmb * (f->slice_row0 ? f->slice_row0[s + 1] - f->slice_row0[s]
                                          : f->hmb);
        size_t sz;
        if (cabac) {
            if (!cb->overflow) cabac_zero_words(f, cb, bs->end, nmb);
            sz = cb->overflow ? 0 : (size_t)(cb->p - bs->start);
        } else {
            y264_bs_rbsp_trailing(bs);
            sz = bs->overflow ? 0 : (size_t)(bs->p - bs->start);
        }
        if (sz == 0) {          /* the buffer ran out: the whole picture is lost */
            sm->n = 0;
            y264_frame_emit_free(job);
            return;
        }
        sm->len[sm->n++] = (uint32_t)sz;
    }
    y264_frame_emit_free(job);
}

/* The picture's coded bytes: what the single-`size` tails reported, summed over
 * the slices, which is what the rate controller and the stats want. */
static size_t slice_map_bytes(const struct slice_map *sm)
{
    size_t t = 0;
    for (int s = 0; s < sm->n; s++) t += sm->len[s];
    return t;
}

/* A picture's bits on the wire for the measured-size bound: the slices'
 * payload and the framing append_nal_raw charges each slice NAL (its start
 * code and header, Y264_NAL_START_BYTES). The buffer ledger takes the same
 * framing through rcp_fill's p->framing; this is the picture's own share of
 * it, read before the NALs are written. */
static double sm_wire_bits(const struct slice_map *sm)
{
    size_t n = 0;
    for (int s = 0; s < sm->n; s++)
        n += sm->len[s] + Y264_NAL_START_BYTES;
    return 8.0 * (double)n;
}

static int append_nal(yah264_encoder_t *e, size_t *off, int ref_idc, int type,
                      const uint8_t *rbsp, size_t rbsp_size);
static int append_slice_nal(yah264_encoder_t *e, size_t *off, int ref_idc, int type,
                            const uint8_t *rbsp, size_t rbsp_size,
                            int first_slice, int disp, int recovery);

/* Append every slice of one picture as its own NAL, in coding order. Only slice
 * 0 opens the access unit: the delimiter and the timing SEIs describe a
 * PICTURE, and a picture cut into four slices is still one of them. The
 * --open-gop key's recovery_point rides the same rule, and `recovery` is
 * carried as an argument rather than read off the encoder for the reason `disp`
 * is: under the deferred-NAL emit a picture's slices reach the output buffer at
 * the NEXT frame's drain, so anything the access unit needs has to travel with
 * the picture rather than be looked up when it lands. */
static int append_picture_nals(yah264_encoder_t *e, size_t *off, int ref_idc,
                               int nal_type, const uint8_t *rbsp,
                               const struct slice_map *sm, int disp, int recovery)
{
    size_t at = 0;
    for (int s = 0; s < sm->n; s++) {
        int r = append_slice_nal(e, off, ref_idc, nal_type, rbsp + at, sm->len[s],
                                 s == 0, disp, recovery);
        if (r < 0)
            return r;
        at += sm->len[s];
    }
    return 0;
}

static size_t build_slice(yah264_encoder_t *e, int type, int is_idr, int is_ref,
                          pixel *const src[3], struct slice_map *sm)
{
    y264_bs_t bs; y264_frame_t f; int fqp, deblock;
    struct slice_hdr hdr;
    struct frame_work fw; fw_default(e, &fw);
    TPROF(TP_PREP, build_slice_prep(e, type, is_idr, is_ref, src, e->rbsp, e->rbsp_cap,
                     &fw, &bs, &f, &fqp, &deblock, &hdr));
    int cabac = e->pps.entropy_coding_mode_flag ? 1 : 0;
    y264_cabac_t cb;
    if (cabac) {
        /* cabac_alignment_one_bit: pad the header to a byte boundary with 1s,
 * then the arithmetic engine writes the slice data from there. */
        while (y264_bs_pos_bits(&bs) & 7)
            y264_bs_write1(&bs, 1);
        y264_cabac_init_engine(&cb, bs.p); y264_cabac_set_end(&cb, bs.end - 64);
        y264_cabac_init_contexts(&cb, type, 0, fqp);   /* contexts init from SliceQPY */
        cb.field = f.field_pic;         /* field scan + the field context half */
        f.cabac = &cb;
    }
    y264_emit_job_t *job;
    TPROF(TP_ANALYZE, job = y264_frame_analyze(&f));
    TPROF(TP_EMIT, emit_picture(&f, job, &hdr, &bs, &cb, cabac, sm));
    qp_trace_frame(e, &f, type);
    if (deblock)
        TPROF(TP_DEBLOCK, y264_deblock_frame(&f));
    ident_stat(&f, type);
    return slice_map_bytes(sm);
}

/* build_slice under rate-control record/replay: records the QP and the coded
 * size, or under replay codes nothing and takes the size from the recording
 * (one slice of that many bytes; the RBSP buffer's content is not read by
 * anything the rate controller depends on). */
static size_t rcr_build_slice(yah264_encoder_t *e, int type, int is_idr, int is_ref,
                              pixel *const src[3], struct slice_map *sm, int retry)
{
    if (!e->rcr)
        return build_slice(e, type, is_idr, is_ref, src, sm);
    int fqp = frame_qp(e, type, is_ref);
    rcr_slice_qp(e->rcr, e->qp, fqp);
    size_t sz = 0;
    if (!e->rcr->play)
        sz = build_slice(e, type, is_idr, is_ref, src, sm);
    sz = rcr_slice_size(e->rcr, sz, fqp, retry);
    if (e->rcr->play) {
        if (sz > e->rbsp_cap) sz = e->rbsp_cap;
        sm->n = 1;
        sm->len[0] = (uint32_t)sz;
    }
    return sz;
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
 * the SEI. Called immediately before the FIRST slice of a picture: a picture
 * cut into --slices pieces is still one access unit, and opening it once per
 * slice would put a delimiter and a set of timing messages in the middle of it.
 *
 * primary_pic_type is derived from the NAL type rather than threaded from the
 * five emit sites: an IDR picture is I and nothing else, so it asserts 0, and
 * everything else asserts Table 7-5's 2, "I, P or B may be present", which is
 * true of every non-IDR picture this encoder codes. A tighter value would have
 * to be carried through four concurrent emit paths to say something no decoder
 * acts on. */
static int hrd_close_au(yah264_encoder_t *e, size_t *off);

static int au_open(yah264_encoder_t *e, size_t *off, int nal_type, int disp,
                   int recovery)
{
    uint8_t buf[64];
    y264_bs_t bs;
    /* Close the PREVIOUS access unit first. Under CBR that is where its filler
 * goes, and filler data belongs to the access unit it follows (7.4.1.2.3),
 * so writing it here -- after that picture's last slice and before anything
 * of this one -- puts it exactly where a receiver attributes it. The last
 * access unit of a stream is never padded, deliberately: nothing is removed
 * after it, so there is no later instant for its buffer to overflow at. */
    if (e->hrd_on && hrd_close_au(e, off) < 0)
        return -1;
    /* The delimiter opens the access unit, so for the FIRST one it was already
 * written ahead of the parameter sets; the timing SEIs are not, because
 * 7.4.1.2.3 puts SEI after the parameter sets and an SEI written before the
 * active SPS is a message with nothing to be active for. So the halves are
 * suppressed separately, and only the delimiter is ever pre-written. */
    if (e->param.aud && !e->au_opened) {
        y264_bs_init(&bs, buf, sizeof buf);
        y264_aud_write(&bs, nal_type == YAH264_NAL_SLICE_IDR ? 0 : 2);
        if (append_nal_raw(e, off, YAH264_NAL_PRIORITY_DISPOSABLE, 9,
                           buf, (size_t)(bs.p - bs.start)) < 0)
            return -1;
    }
    e->au_opened = 0;

    long tick = e->hrd_pic * e->hrd_tpp;           /* ticks into this segment */
    unsigned cpb_delay = 0;
    if (e->hrd_on) {
        if (e->hrd_disp_base < 0)
            e->hrd_disp_base = disp;
        /* cpb_removal_delay counts ticks since the removal of the first picture
 * of the buffering period THIS PICTURE ARRIVES UNDER -- which, for a
 * picture that itself opens a new period, is the period BEFORE it. So it
 * is computed here, ahead of the message that moves the anchor, and not
 * after. Computing it after writes 0 on every IDR but the first, which
 * declares that picture removed at the same instant as the stream's
 * opening one; every later removal time then inherits the collapse. That
 * read as an underflow at the first mid-stream IDR of every clip that has
 * one, at every bitrate, and as nothing at all on the clips that do not.
 *
 * For this segment's own first picture the period belongs to the segment
 * BEFORE it in the output, which only the caller splitting the work
 * knows about; rc.hrd_bp_ticks is it saying so. A single-segment encode
 * leaves it 0, and the value is unused there anyway -- nothing precedes
 * the first access unit. */
        cpb_delay = e->hrd_pic == 0 ? (unsigned)e->param.rc.hrd_bp_ticks
                                    : (unsigned)(tick - e->hrd_anchor_tick);
        /* A buffering period at every IDR, which is what makes a mid-stream
 * random access point one a receiver can start its buffer from. The
 * first picture of this segment already has one: the header writer put
 * it beside the parameter sets, because a buffering period message has
 * to be the first SEI of its access unit and the settings message is
 * written there too. */
        if (nal_type == YAH264_NAL_SLICE_IDR && e->hrd_pic > 0 &&
            !e->param.rc.hrd_segmented) {
            uint8_t msg[16];
            /* A decoder joining HERE is told the buffer may be full, which is
 * the widest start the model allows and the one the encoder's own
 * loop assumes it can reach: the buffer law clamps the occupancy at the
 * buffer size and nowhere below it. The stream's own schedule is
 * unaffected either way -- a removal time is counted in ticks from
 * the period's start, never derived from this delay. */
            size_t n = y264_sei_buffering_period(msg, sizeof msg, e->sps.sps_id,
                                                 e->hrd_full_delay, 0);
            y264_bs_init(&bs, buf, sizeof buf);
            y264_sei_write(&bs, 0, msg, n);
            if (append_nal_raw(e, off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                               buf, (size_t)(bs.p - bs.start)) < 0)
                return -1;
            e->hrd_anchor_tick = tick;
        }
    }
    if (e->param.pic_struct || e->hrd_on) {
        uint8_t msg[16];
        /* pic_struct: a frame picture is 0. A field picture is 1 or 2 by its
 * PARITY, and the parity of the k-th coded picture of a field-coded
 * segment is k's own: the pair is always coded first field then second,
 * and --tff/--bff says which of the two the first one is. */
        int ps = 0;
        if (e->fields)
            ps = ((e->hrd_pic & 1) == 0) == (e->param.interlaced != 2) ? 1 : 2;
        /* dpb_output_delay carries the reorder lag: removal runs in DECODE
 * order at a fixed cadence and output runs in DISPLAY order, so the
 * delay is the gap between the two plus the depth the SPS already
 * declared as max_num_reorder_frames. Fields add their own parity back,
 * so the two halves of a pair are output one tick apart rather than
 * together. */
        long out = 2 * ((long)disp - e->hrd_disp_base + e->sps.max_num_reorder_frames)
                 + (e->fields ? (e->hrd_pic & 1) : 0);
        long dpb = out - tick;
        if (dpb < 0) dpb = 0;
        size_t n = y264_sei_pic_timing(msg, sizeof msg, e->hrd_on, cpb_delay,
                                       (unsigned)dpb, e->param.pic_struct, ps);
        y264_bs_init(&bs, buf, sizeof buf);
        y264_sei_write(&bs, 1, msg, n);
        if (append_nal_raw(e, off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                           buf, (size_t)(bs.p - bs.start)) < 0)
            return -1;
    }
    /* The recovery_point, last of the messages because it is an SEI and the
 * delimiter is not, and after buffering_period because that one has to be
 * the first SEI of its access unit. `recovery` is recovery_frame_cnt plus
 * one: an --open-gop key is exact at once, an --intra-refresh sweep once
 * its last column has been coded. */
    if (recovery) {
        uint8_t msg[16];
        size_t n = y264_sei_recovery_point(msg, sizeof msg, recovery - 1, 1, 0);
        y264_bs_init(&bs, buf, sizeof buf);
        y264_sei_write(&bs, 6, msg, n);
        if (append_nal_raw(e, off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                           buf, (size_t)(bs.p - bs.start)) < 0)
            return -1;
    }
    e->hrd_pic++;
    return 0;
}

static int append_nal(yah264_encoder_t *e, size_t *off, int ref_idc, int type,
                      const uint8_t *rbsp, size_t rbsp_size)
{
    if (rbsp_size == 0) { slice_overflow_warn(); }
    if (rbsp_size == 0)
        return -1;
    return append_nal_raw(e, off, ref_idc, type, rbsp, rbsp_size);
}

/* One slice NAL of a picture. `first_slice` is what opens the access unit,
 * `disp` is the picture's display index, which the timing SEI needs and which
 * every emit site already holds (the drain paths code out of order, so e's own
 * cur_disp has moved on by the time their NALs are appended), and `recovery`
 * says this picture is an --open-gop key and travels for the same reason. */
static int append_slice_nal(yah264_encoder_t *e, size_t *off, int ref_idc, int type,
                            const uint8_t *rbsp, size_t rbsp_size,
                            int first_slice, int disp, int recovery)
{
    if (rbsp_size == 0) { slice_overflow_warn(); }
    if (rbsp_size == 0)
        return -1;
    if (first_slice &&
        (e->param.aud || e->param.pic_struct || e->hrd_on || recovery) &&
        au_open(e, off, type, disp, recovery) < 0)
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
    if (e->nal_count >= e->nal_cap)
        return -1;                      /* never write past the array: it is
 * immediately followed by nal_count */
    yah264_nal_t *nl = &e->nal[e->nal_count++];
    nl->type = type;
    nl->ref_idc = ref_idc;
    nl->size = n;
    nl->payload = e->out + *off;
    *off += n;
    /* A slice NAL's payload is what the rate ledger fills with; everything
 * else that reaches the stream is framing, which the two-pass ledger
 * charges separately (rcp_fill). A slice's own framing is counted as its
 * start code and header only: its emulation-prevention bytes depend on
 * the payload's content, which a rate-control replay does not have, and
 * they are a few bytes a clip on CABAC streams. */
    /* Filler (type 12, --nal-hrd cbr) is left out: it pads an access unit
 * up to bytes the buffer would otherwise discard at its rim, so charging
 * it moves no ledger, and its size follows the slices' emulation
 * prevention, which a rate-control replay does not have. */
    if (type != 12)
        e->nal_framing += (type == YAH264_NAL_SLICE || type == YAH264_NAL_SLICE_IDR)
                        ? (double)Y264_NAL_START_BYTES : (double)n;
    /* Every byte that crosses the wire, start code included, because that is
 * what a receiver's buffer holds. The count runs across encode calls, so it
 * cannot be read off `off`, which restarts at every one of them. */
    e->hrd_au_bytes += n;
    return 0;
}

/* Close the access unit just finished: charge its bytes to the declared buffer
 * and, under CBR, pad it up to the constant rate.
 *
 * The condition being maintained is the Annex C one, C.3.2: at the removal
 * instant of every access unit the buffer holds no more than CpbSize. Under
 * cbr_flag 1 the channel never pauses, so the occupancy at the next removal is
 * this one's, plus a picture period of arrival, minus what this picture spent.
 * Where that would run past the buffer, the difference is exactly the padding
 * the picture owes -- and a filler NAL cannot grow under emulation prevention
 * (its payload is all 0xFF), so its coded size is known before it is written.
 *
 * Under cbr_flag 0 the delivery schedule is allowed to pause when the buffer is
 * full, so there is nothing to pad and the ledger is kept for the diagnostic
 * alone. The ledger is deliberately NOT clamped at either end: a receiver's
 * buffer does not clamp, and a ledger that did would decide the next picture's
 * padding against a schedule the stream is no longer on. */
static int hrd_close_au(yah264_encoder_t *e, size_t *off)
{
    if (e->hrd_pic == 0)                /* nothing coded yet: the headers are
 * part of the access unit still being built */
        return 0;
    double bits = 8.0 * (double)e->hrd_au_bytes;
    if (e->hrd_filler) {
        double want = e->hrd_fill + e->hrd_rate_pic - e->hrd_cpb - bits;
        if (want > 0.0) {
            /* floor+1, not ceil: land strictly INSIDE the bound rather than on
 * it. Rounding up to the nearest byte makes the occupancy come out
 * exactly CpbSize whenever the shortfall is a whole number of bytes,
 * and a checker that recomputes the same quantity its own way --
 * BitRate * t minus the bits removed, rather than our running sum --
 * lands a few ULP to the other side of it. At a 20 Mbit buffer that
 * is about 1e-9 bits, which is precisely the epsilon such a checker
 * compares with, so the cell reads OVERFLOW by nought point nothing.
 * One more byte of filler puts 1 to 8 BITS of daylight between the
 * two and costs one byte per padded access unit. */
            size_t need = (size_t)(want / 8.0) + 1;
            /* A filler NAL costs its start code and header byte (5) and the
 * rbsp_trailing 0x80 (1) on top of its ff_bytes, so the payload it
 * needs is the shortfall less those six -- and a shortfall smaller
 * than six bytes is already covered by the shortest filler there is.
 * Getting this by one costs the stream one byte per padded access
 * unit, which reads as an overflow of 3 to 8 BITS: the padding lands
 * a byte short of a bound it was meant to land exactly on. */
            size_t over = Y264_NAL_START_BYTES + 1;
            size_t ff = need > over ? need - over : 0;
            if (ff + 1 > e->hrd_pad_cap)
                ff = e->hrd_pad_cap > 1 ? e->hrd_pad_cap - 1 : 0;
            size_t n = y264_filler_write(e->hrd_pad, e->hrd_pad_cap, ff);
            if (n == 0 ||
                append_nal_raw(e, off, YAH264_NAL_PRIORITY_DISPOSABLE, 12,
                               e->hrd_pad, n) < 0)
                return -1;
            e->hrd_pad_bytes += (long)(ff + over);
            bits = 8.0 * (double)e->hrd_au_bytes;
        }
    }
    e->hrd_fill += e->hrd_rate_pic - bits;
    e->hrd_au_bytes = 0;
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
        fprintf(stderr, "yah264: a slice outgrew its buffer and was dropped (a pathological input)\n");
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
 * The constraint is over the picture's VCL NAL units TOGETHER, so it is
 * enforced slice by slice against that slice's OWN macroblock share (`nmb`):
 * summing those per-slice inequalities reproduces the picture's exactly, while
 * charging every slice the whole picture's macroblock allowance would let the
 * sum drift past the bound once there is more than one of them. With one slice
 * per picture nmb is the picture's and this is the expression it always was.
 * When bins outrun the bound, 0x0000 words appended after the trailing bits
 * (each one adds three NAL bytes once emulation prevention lands, i.e. 32 to
 * the bound) restore it. Returns the bytes appended.
 * Y264_CZW_STAT=1 prints the ratio per slice. */
static size_t cabac_zero_words(const y264_frame_t *f, y264_cabac_t *cb, const uint8_t *end,
                               int nmb)
{
    size_t bytes = (size_t)(cb->p - cb->start);
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
 * Y264_STAIR_LAG constant. The frame height is split evenly among the pool
 * threads: max_range = (height + MARGIN) / pool_threads - MARGIN, and half of
 * that is the wanted reach, converted back into whole LAG rows, floored at
 * Y264_STAIR_LAG and rounded up to a whole unit. MARGIN_PX is 24 px, three macroblock rows, which is
 * already the same "-24" baked into Y264_STAIR_MVY_MAX's 16*LAG-24 -- not a
 * coincidence, both mark the same producer-side trailer margin
 * (stair_trailer_task's fin_l = 16j+13 vs a consumer touching up to 16j-6:
 * the two constants are the two sides of one inequality).
 *
 * SOUNDNESS: the floor (never returning less than Y264_STAIR_LAG) is the only
 * thing this function has to get right, and it is enforced unconditionally
 * below, not tuned per shape. The reason it is safe to let this return MORE
 * than the floor (which is the entire point -- taller frames at lower pool
 * widths get a wider clamp) is in motion.h's Y264_STAIR_MVY_MAX comment: the
 * row-gate bound and the MV clamp bound both scale by exactly 16 per unit of
 * LAG, so the slack between producer and consumer is a LAG-independent
 * constant. Raising LAG can only ever add margin, never remove it. Lowering
 * the FLOOR itself is the unsound direction and this function never does
 * that: LAG 3 is measured UNSOUND at the current margin arithmetic (motion.h) and
 * worth only 1.01x on CIF, so this deliberately does not attempt it.
 *
 * On CIF/720p at 8-18 pool threads this returns exactly Y264_STAIR_LAG -- the
 * formula's natural value is already below the floor there. It stops being a no-op on taller frames run at a
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
    long want_px = max_range / 2;                   /* half reserved, half free */
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
 * threads, capped at the grid's critical-path knee. >1 means a pool.
 *
 * Split out of the ntp_pool_create site because two things now need the answer
 * and they run at different points in encoder_open: the pool itself, and
 * (earlier) dpbp_open's sizing rule, which reads the width-engagement
 * predicate. One function so the two can never disagree. */
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
    int wt = param->frame_threads > 0 ? param->frame_threads
                                      : resolve_threads(param);
    int cap = yah264_frame_thread_cap(param->width, param->height);
    return wt > cap ? cap : wt;
}

/* The lookahead window this open will build. Split out of encoder_open for the
 * same reason wf_width_for was: two callers now need the answer -- open itself
 * and yah264_lookahead_delay, which the CLI asks before it opens anything --
 * and a second copy of the rule would eventually disagree with the first. */
static int la_depth_for(const yah264_param_t *param)
{
    int want_la = (param->bframes > 0) || param->rc.lookahead > 0;
    int d = want_la ? param->rc.lookahead : 0;
    if (d < 0) d = 0;
    if (d > 64) d = 64;
    return d;
}

/* How many frames of lead the decoupled lookahead gets: param.sync_lookahead
 * resolved (auto = bframes+1, forced to 0 where a lookahead thread cannot pay).
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
 * why this returns the lead and la_th_on follows it (--sync-lookahead 0
 * therefore runs threadless). The
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
    if (wf_width_for(param) < LA_POOL_MIN)      /* auto: no pool, no thread to feed */
        return 0;
    int bf = param->bframes;                    /* as open will clamp it */
    if (bf < 0) bf = 0;
    if (bf > 7) bf = 7;
    return bf + 1;                              /* one mini-GOP plus one */
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

/* Above ~1080p the phases with a fraction on BOTH axes are not built as
 * planes. The census puts about 12% of the lowres subpel reads on them (1% on
 * the quarter-quarter ones) against 9 of the 15 full-frame builds, so their
 * few blocks are interpolated where they are read; at 4K that is 9 lowres
 * passes per reference that no longer run, and 9 planes per set that are
 * never allocated. At 1080p and below the planes are small enough to stay in
 * cache and building them is the cheaper side (+0.2% instructions measured
 * with the gate removed), so they are built as before. The signal to every
 * reader is a NULL plane pointer; either way the SATDs are identical. */
#define LR_ONFLY_MIN_PIXELS (1000 * 1000)
static int lr_qpel_avg_on(void);
static inline int lr_plane_built(const yah264_encoder_t *e, int phase)
{
    if (lr_qpel_avg_on())                   /* half-pel planes only: H, V, C */
        return phase == 2 || phase == 8 || phase == 10;
    int diag = (phase & 3) && (phase >> 2);
    return !diag || (size_t)e->lr_w * e->lr_h < LR_ONFLY_MIN_PIXELS;
}

/* --- the phase-set pool (struct y264_lrset) ------------------------------
 * The lookahead builds the new anchor's fifteen phases from its ring lowres;
 * a little later mb-tree's cache misses on that anchor and builds the same
 * phases from the same pointer again. A set is a pure function of the plane
 * it is built from and of the construction (the source half-pel planes
 * registered for that pointer, or bilinear), so the cache takes a reference
 * to the lookahead's set instead whenever its build would read the same
 * pointer the same way. Taken only on an exact (push index, pointer) match:
 * the cache's sets for an anchor's other buffers (the coded anchor's copy,
 * the ref-B's re-downscale) are built as before. Lookahead builds always go
 * to a set nobody else holds, so a referenced set is never written. */
/* A set nobody holds stays published, so a cache miss a few walks after the
 * lookahead moved on can still take it. A new set comes from an unpublished
 * free one, else a fresh allocation (up to the pool's size: the lookahead's
 * two sets and a full cache, the residency the pool replaced, plus two), else
 * the free published set of the oldest push. */
#define LRSET_RESIDENT (Y264_MBT_SUB_MAX + 4)
static int lrset_acquire(yah264_encoder_t *e)  /* under lrset_mx; -1 = none */
{
    int i = -1;
    for (int k = 0; k < e->lrset_n && i < 0; k++)
        if (e->lrset[k].refs == 0 && e->lrset[k].push < 0) i = k;
    if (i < 0 && e->lrset_n >= LRSET_RESIDENT)
        for (int k = 0; k < e->lrset_n; k++)
            if (e->lrset[k].refs == 0 && (i < 0 || e->lrset[k].push < e->lrset[i].push)) i = k;
    if (i >= 0) {
        e->lrset[i].refs = 1; e->lrset[i].push = -1; e->lrset[i].key = NULL;
        return i;
    }
    if (e->lrset_n >= Y264_LRSET_MAX) return -1;
    i = e->lrset_n;
    size_t lrsz = (size_t)e->lr_w * e->lr_h;
    e->lrset[i].pl[0] = NULL;
    for (int p = 1; p < 16; p++) {
        if (!lr_plane_built(e, p)) { e->lrset[i].pl[p] = NULL; continue; }
        if (!(e->lrset[i].pl[p] = malloc(lrsz * sizeof(pixel)))) {
            for (int q = 1; q < p; q++) { free(e->lrset[i].pl[q]); e->lrset[i].pl[q] = NULL; }
            return -1;
        }
    }
    e->lrset[i].refs = 1; e->lrset[i].push = -1; e->lrset[i].key = NULL;
    e->lrset_n = i + 1;
    return i;
}
static void lrset_release(yah264_encoder_t *e, int i)  /* under lrset_mx */
{
    if (i >= 0) e->lrset[i].refs--;             /* a published set keeps its key */
}

static int hw_env(void);
static int hw_scenecut_probe(yah264_encoder_t *e, const yah264_picture_t *pic);
static void hw_scenecut_free(yah264_encoder_t *e);
static void warm_lr_statics(void);

/* Size-gated effort at the medium tier. At 2160p the medium tier holds a quality margin over the
 * reference at matched size (VMAF-NEG +0.5 to +2.2 on the tier S and tier M
 * cells) and spends 1.3x its instructions. On pictures of at least
 * 3840x1600 luma samples this trades effort the cells do not need: two
 * references instead of three, no 8x8 inter sub-partitions, the B
 * pre-search skip at 2 and the lowres settle exit at 2. psy-RD stays on: it
 * read as a free win on the tier S clips, but over chimera_2160p's long
 * window turning it off costs +3.2% BD-VMAF-NEG, where each of the four kept
 * arms costs 0.35% or less. (One reference and one RD survivor
 * per B set save another 5-7% but leave chimera only +0.09 over the
 * reference.) From 1920x1080 up to that size only the two arms that cost
 * real 1080p titles nothing measurable run: no 8x8 inter sub-partitions and
 * the B pre-search skip. The third reference, the lowres settle exit and
 * psy-RD stay: on the chimera 1080p window (1440 frames) the full set cost
 * +5.1% BD-VMAF-NEG at CRF and +4.0% at VBR, nearly all of it the
 * reference cut, where these two cost +0.8% / +1.0%. Each change
 * applies only where the parameter still holds the medium preset's value,
 * so an explicit choice that differs from it is kept; an explicit choice
 * EQUAL to it (--ref 3 at medium, say) is not distinguishable and is
 * traded too. */
/* Cheaper psy-RD forms on the 2160p size tier, as bits (mbcode.h psy4k):
 * frames of at least 3840x1600 at the medium tier with psy-RD on carry the
 * bits, everything else carries 0. 4: the pre-ME skip bound keeps half of
 * the psy term. With the whole term in it, about a third more B
 * macroblocks escape the bound and run the full search only to end as skips
 * (meridian 2160p: psy-RD costs +12.9% instructions, nearly all of it there).
 * Half the term: -3.7..-4.2% instructions on meridian 2160p, -0.6% on chimera
 * and bbb 2160p, BD-VMAF-NEG +0.23 chimera (720 frames) / -0.10 meridian (899
 * frames). The bound on the SSD alone (2) costs chimera +0.56, the survivor
 * screen without the psy widening (1) about +0.3 more. 0 is the full term. */
#define PSY4K_BITS 4
/* An effort cut on the 2160p size tier only: the P intra admission at 1.0x
 * the inter SATD instead of 1.5x and the B intra admission at 0.5x instead of
 * 0.75x. -2.8% instructions on meridian 2160p, -1.8% on bbb 2160p,
 * BD-VMAF-NEG +0.22 chimera (720 frames) / +0.17 meridian (899 frames),
 * PSNR-Y +0.39 / +0.27. (The four-mode lowres intra estimate there as well
 * adds -1.2% but costs +1.45 NEG on chimera and raises bbb's instructions.) */
static int uhd_lite_on(const yah264_param_t *p)
{
    return p->subme == 7 && p->rc.method != YAH264_RC_CQP &&
           y264_size_class(p->width, p->height) >= 3;
}
static int psy4k_bits(const yah264_param_t *p)
{
    if (p->subme != 7 || p->rc.method == YAH264_RC_CQP ||
        p->psy_rd <= 0.0f || y264_size_class(p->width, p->height) < 3)
        return 0;
    return PSY4K_BITS;
}
static void size_trades(yah264_param_t *p)
{
    if (p->subme != 7 || p->rc.method == YAH264_RC_CQP)
        return;
    int cls = y264_size_class(p->width, p->height);
    if (cls < 2)
        return;
    if (p->partitions == YAH264_PART_AUTO)
        p->partitions = YAH264_PART_I4X4 | (p->transform8x8 ? YAH264_PART_I8X8 : 0);
    if (p->b_preme_skip == 0) p->b_preme_skip = 2;
    if (cls < 3)
        return;                                 /* the 1080p set ends here */
    if (p->ref == 3) p->ref = 2;
    if (p->lr_settle == 0) p->lr_settle = 2;
}

/* The default deblocking offsets, by picture size: -1:-1 (slice header div2
 * units) at 1280x720 and above, the spec-neutral 0:0 below. At HD sizes our
 * RD and psy keep edge and texture energy that the neutral filter smooths
 * back out. Measured 2026-10-04/05 at matched achieved rate, BD-VMAF-NEG of
 * -1:-1 against 0:0: HD band median -0.31%, worst sunflower +0.05%; sintel
 * -1.74, riverbed -0.96, pedestrian -0.94; Sol Levante 720p -0.59, 1080p
 * -0.70; 2160p chimera -0.65, meridian -0.48; multi-shot 720p -0.21, 1080p
 * -0.78. It costs PSNR (about +3% BD-PSNR-Y at 2160p) and nothing in
 * instructions. Below 720p the CIF clips read up to +0.72 (mobile), over
 * the per-clip bar, so small pictures keep 0:0. An explicit --deblock A:B
 * always wins. */
static void deblock_auto_resolve(yah264_param_t *p)
{
    int off = y264_size_class(p->width, p->height) >= 1 ? -1 : 0;
    if (p->deblock_alpha == YAH264_DEBLOCK_AUTO) p->deblock_alpha = off;
    if (p->deblock_beta == YAH264_DEBLOCK_AUTO) p->deblock_beta = off;
}

static int lr_intra_penalty(const yah264_param_t *p);
static yah264_encoder_t *encoder_open_sw(const yah264_param_t *param)
{
    warm_lr_statics();          /* resolve env-gated lowres statics on this thread
 * (CLI primes on the main thread before workers) */
    (void)tprof_on();           /* profiling gate: warm before any bg thread reads it */
    y264_cabac_warm();          /* build the RDOQ bit table before any worker runs */
    y264_deblock_warm();
    y264_cavlc_warm();          /* likewise the VLC tables */
    if (!param)
        return NULL;
    /* The public enums follow the reference encoder's public API numbering,
 * which has cases this encoder does not implement. Refuse them here. Accepting one and
 * narrowing it to the nearest thing we do have is exactly the
 * silent-wrong-tool failure the numbering was changed to remove, so every
 * one of these is a hard no: an unsupported csp is not 4:2:0, an exhaustive
 * search method is not UMH, and "no direct prediction" is not spatial. Do NOT go back to a contiguous
 * range test or a switch default: both admit values that then encode as
 * something else. */
    if (param->csp != YAH264_CSP_I420 && param->csp != YAH264_CSP_I422 &&
        param->csp != YAH264_CSP_I444)
        return NULL;
    if (param->rc.method != YAH264_RC_CQP && param->rc.method != YAH264_RC_CRF &&
        param->rc.method != YAH264_RC_ABR && param->rc.method != YAH264_RC_2PASS)
        return NULL;
    if (param->me_method != YAH264_ME_AUTO && param->me_method != YAH264_ME_DIA &&
        param->me_method != YAH264_ME_HEX && param->me_method != YAH264_ME_UMH &&
        param->me_method != YAH264_ME_ESA && param->me_method != YAH264_ME_TESA)
        return NULL;
    if (param->direct != YAH264_DIRECT_SPATIAL && param->direct != YAH264_DIRECT_TEMPORAL
        && param->direct != YAH264_DIRECT_AUTO)
        return NULL;
    if (param->width <= 0 || param->height <= 0)
        return NULL;
    if ((param->width & 1) || (param->height & 1))
        return NULL;                                /* 4:2:0 needs even dims */
    /* The two partition sets that cannot be coded as asked. Both are refused
 * with the rule rather than silently dropped: the derived default never
 * reaches either, so a mask that does is one somebody typed, and a shape
 * that goes missing without a word is the silent-wrong-tool failure every
 * other check above exists to prevent. A preset or a profile that turns the
 * 8x8 transform off narrows the DERIVED set in silence, as it does for
 * every other tool the preset chose. */
    {
        int pmask = yah264_partitions_resolved(param);
        if (pmask & ~YAH264_PART_ALL) {
            fprintf(stderr, "yah264: --partitions: unknown shape bits 0x%x\n",
                    (unsigned)(pmask & ~YAH264_PART_ALL));
            return NULL;
        }
        if ((pmask & YAH264_PART_P4X4) && !(pmask & YAH264_PART_P8X8)) {
            fprintf(stderr, "yah264: --partitions p4x4 needs p8x8: the sub-8x8 "
                            "shapes divide an 8x8 block, and without the 8x8 "
                            "split there is no 8x8 block to divide\n");
            return NULL;
        }
        if ((pmask & YAH264_PART_I8X8) && !param->transform8x8) {
            fprintf(stderr, "yah264: --partitions i8x8 needs --transform-8x8: "
                            "an 8x8 intra block is coded by the 8x8 transform, "
                            "and nothing else can carry its residual\n");
            return NULL;
        }
    }
    /* The syntax elements appended in 2026-09-16 carry the spec's own domains.
 * Out of range is refused rather than clamped: a clamped offset writes a
 * legal-looking header that does not say what the caller asked for. */
    if ((param->deblock_alpha != YAH264_DEBLOCK_AUTO &&
         (param->deblock_alpha < -6 || param->deblock_alpha > 6)) ||
        (param->deblock_beta != YAH264_DEBLOCK_AUTO &&
         (param->deblock_beta  < -6 || param->deblock_beta  > 6)))
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
    /* --crf-max bounds a raise that only CRF-with-a-VBV ever makes, so it is
 * refused where nothing would raise the QP for it to bound rather than
 * accepted and left inert. An inert bound on a rate-control flag is the
 * worst of the three outcomes: the encode runs, the ceiling is not there,
 * and nothing says so. */
    if (param->crf_max != 0.0) {
        if (param->crf_max < 1.0 || param->crf_max > 51.0) {
            fprintf(stderr, "yah264: --crf-max takes a QP in 1..51\n");
            return NULL;
        }
        if (param->rc.method != YAH264_RC_CRF) {
            fprintf(stderr, "yah264: --crf-max needs --crf: it bounds what the buffer "
                            "raises a rate-factor encode to, and no other mode has that raise\n");
            return NULL;
        }
        if (param->rc.vbv_maxrate <= 0 || param->rc.vbv_bufsize <= 0) {
            fprintf(stderr, "yah264: --crf-max needs --vbv-maxrate and --vbv-bufsize: "
                            "with no buffer nothing raises the QP for it to bound\n");
            return NULL;
        }
    }
    /* --weightp 2 puts the weighted copy of a reference in a list-0 slot of its
 * own, so it needs a slot to put it in: at --ref 1 there is none, and the
 * mode would silently be mode 1 on every frame. Refused rather than
 * accepted and left inert, on the --crf-max precedent. Field coding is
 * refused for a different reason: the duplicate's tile test walks the
 * picture at prep, a field pair preps twice into one frame buffer, and the
 * combination has never been measured.
 *
 * Read the parameter, not the resolved value. */
    {
        int wp = param->weightp;
        if (wp < -1 || wp > 2) {
            fprintf(stderr, "yah264: --weightp takes 0, 1 or 2\n");
            return NULL;
        }
        if (wp == 2) {
            if (param->ref <= 1) {          /* 0 is unset, and resolves to 1 */
                fprintf(stderr, "yah264: --weightp 2 needs --ref 2 or more: the weighted "
                                "copy of a reference is coded as a second list-0 entry, "
                                "and at --ref 1 there is no room for one\n");
                return NULL;
            }
            if (param->interlaced) {
                fprintf(stderr, "yah264: --weightp 2 is not available with field coding; "
                                "--weightp 1 is\n");
                return NULL;
            }
        }
    }
    if (param->b_bias < -100 || param->b_bias > 100) {
        fprintf(stderr, "yah264: --b-bias takes -100..100\n");
        return NULL;
    }
    /* A tolerance of zero is a divide by zero in the correction term, not a
 * tight loop; negative is meaningless. Infinity is the documented way to
 * take the term out. */
    if (param->ratetol < 0.0 || (param->ratetol > 0.0 && param->ratetol < 1e-6)) {
        fprintf(stderr, "yah264: --ratetol takes a positive fraction, or inf\n");
        return NULL;
    }
    /* --nal-hrd. The HRD declares a bucket and a clock, so it refuses where
 * either is missing rather than inventing one: with no VBV there is no
 * bucket to declare, and with no VUI timing there are no clock ticks for a
 * removal time to be counted in. Filler is refused outside CBR because
 * under VBR the declared schedule already lets the channel pause, so
 * padding there would spend bits to satisfy nothing. */
    if (param->nal_hrd) {
        if (param->nal_hrd != YAH264_NAL_HRD_VBR && param->nal_hrd != YAH264_NAL_HRD_CBR) {
            fprintf(stderr, "yah264: --nal-hrd takes vbr or cbr\n");
            return NULL;
        }
        if (param->rc.vbv_maxrate <= 0 || param->rc.vbv_bufsize <= 0) {
            fprintf(stderr, "yah264: --nal-hrd needs --vbv-maxrate and --vbv-bufsize: "
                            "there is no buffer model to declare without them\n");
            return NULL;
        }
        if (param->timebase.fps_num <= 0 || param->timebase.fps_den <= 0) {
            fprintf(stderr, "yah264: --nal-hrd needs a frame rate: removal times are "
                            "clock ticks and the VUI would carry none\n");
            return NULL;
        }
    }
    if (param->filler > 0 && param->nal_hrd != YAH264_NAL_HRD_CBR) {
        fprintf(stderr, "yah264: --filler needs --nal-hrd cbr: padding to a constant "
                        "rate only means something where a constant rate is declared\n");
        return NULL;
    }
    if (param->rc.hrd_bp_ticks < 0)
        return NULL;
    /* PAFF. What ships is I, P and B field pictures, both entropy coders,
 * 4:2:0. Everything named here is refused rather than narrowed, for the
 * reason the enum refusals above give: a request quietly answered with a
 * different tool is the failure mode this file refuses to have.
 * - 4:2:2 and 4:4:4: the field halves of the 4:4:4 residual context set
 * are not exercised by any cell here, and an ungated conformance
 * surface is not a shipped one.
 * - the hardware backend has no field path at all (src/hw/vt.c). */
    if (param->interlaced) {
        if (param->interlaced != 1 && param->interlaced != 2)
            return NULL;
        if (param->csp != YAH264_CSP_I420)
            return NULL;
        /* The vertical crop counts in CropUnitY = SubHeightC * 2 samples once
 * the sequence may carry fields, and the coded height is padded to an
 * even number of macroblock rows, i.e. a multiple of 32. A height that
 * is not a multiple of 4 therefore cannot be cropped back to exactly
 * itself: the SPS would declare a picture of a different size than the
 * one handed in. Refused rather than rounded -- a stream whose declared
 * height is not the caller's is the silent-wrong-answer this file does
 * not ship. */
        if (param->height % 4)
            return NULL;
    }
    /* --fake-interlaced declares the same geometry and has the same arithmetic
 * under it, so it has the same constraint: measured on main, height 98 with
 * the flag set declares a 100-line picture, because 100 - 98 is not a whole
 * number of crop units. Found while gating PAFF, fixed here because this
 * item owns frame_mbs_only_flag now. */
    if (param->fake_interlaced && !param->interlaced && (param->height % 4))
        return NULL;
    /* --open-gop against --stitchable. Stitchable exists so two encodes of the
 * same geometry carry the same parameter sets and can be cut and joined at
 * any keyframe; open GOP is the decision that a keyframe is NOT a cut point,
 * because the pictures around it reference across it. Honouring both means
 * honouring neither, so the pair is refused rather than silently ranked. */
    if (param->open_gop && param->stitchable)
        return NULL;
    /* --intra-refresh against what it cannot keep its promise with. B frames:
 * direct prediction reads the co-located picture's motion, which a decoder
 * that joined mid-sweep never decoded, and the sweep is defined in coding
 * order, which B frames take away from display order. Open GOP is a
 * different random-access scheme over the same keyframes this one removes.
 * Field coding would need the sweep defined per field. Stitchable promises
 * a cut point at every keyframe, and there are none. */
    if (param->intra_refresh) {
        const char *why = param->bframes > 0 ? "B frames (use --bframes 0)"
                        : param->open_gop    ? "--open-gop"
                        : param->interlaced  ? "field coding"
                        : param->stitchable  ? "--stitchable"
                        : param->keyint > 65536 ? "a --keyint above 65536 "
                                                  "(a sweep has to fit in FrameNum)"
                        : NULL;
        if (why) {
            fprintf(stderr, "yah264: --intra-refresh cannot be combined with %s\n", why);
            return NULL;
        }
    }

    yah264_encoder_t *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->param = *param;
    deblock_auto_resolve(&e->param);
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
    /* Real field coding needs the same even macroblock height, and for the
 * stronger reason: each field IS half of it. */
    e->fields = e->param.interlaced;
    /* --open-gop, resolved once per instance. */
    e->open_gop = param->open_gop ? 1 : 0;
    e->rp_poc = e->rp_poc_prev = -1;   /* calloc's 0 is a real POC; -1 is "none" */
    /* --intra-refresh: keyint becomes the sweep length, and the frame-type
 * decision stops placing keyframes at all -- neither the keyint cadence nor
 * an adaptive cut. A cut does not restart the sweep either: the sweep in
 * progress already promised, in its recovery_point, an exact picture at its
 * end, and a restart would have to keep that promise as well as open a new
 * one. Only the first picture and a keyframe the caller forces (a zone's or
 * a per-frame force) are IDRs, and the sweep restarts after one. */
    e->ir_on = param->intra_refresh ? 1 : 0;
    e->ir_j = -1;
    e->ir_bound_poc = -1;
    if (e->ir_on) {
        e->ir_period = param->keyint > 0 ? param->keyint : 1;
        e->param.keyint = 1 << 30;
        e->param.scenecut = YAH264_SCENECUT_OFF;
    }
    if ((param->fake_interlaced || e->fields) && (e->height_in_mbs & 1))
        e->height_in_mbs++;
    e->padded_w = e->width_in_mbs * 16;
    e->padded_h = e->height_in_mbs * 16;
    e->fld_hmb = e->height_in_mbs / 2;
    /* A field view doubles the stride, so one row of field border costs two
 * rows of frame border: allocate twice the vertical border (plane_alloc). */
    e->pvmul = e->fields ? 2 : 1;

    /* --slices: cut the picture into N independently decodable slices on
 * macroblock-row boundaries, N clamped to the row count (a slice owns at
 * least one row) and to Y264_SLICES_MAX. Rows divide as evenly as the count
 * allows, so slice s starts at s * hmb / N -- a pure function of the
 * parameters and the frame size, decided once, so every frame and every
 * thread cuts the picture in the same place. Allocation failure falls back to one
 * slice rather than failing the open: the picture still codes, it just
 * codes as one slice, and the caller sees that in the NAL count. */
    {
        int ns = e->param.slices > 0 ? e->param.slices : 1;
        /* PAFF: the cuts divide the CODED PICTURE, and under field coding that
 * is the field -- half the macroblock rows. Still a pure function of the
 * parameters and the geometry, so it is still decided once and every
 * picture cuts in the same place; it is just that the picture is a field.
 * Sizing it by the frame instead let the emit walk rows the per-picture
 * record array never had, which is a heap overflow the first --slices 4
 * field encode found. */
        int pic_hmb = e->fields ? e->fld_hmb : e->height_in_mbs;
        if (ns > pic_hmb) ns = pic_hmb;
        if (ns > Y264_SLICES_MAX) ns = Y264_SLICES_MAX;
        e->nslices = ns;
        /* A field pair is two coded pictures, so a frame's worth of NALs is
 * twice a frame picture's. */
        e->nal_cap = 48 * ns * (e->fields ? 2 : 1) + 8;
        e->nal = calloc((size_t)e->nal_cap, sizeof *e->nal);
        if (!e->nal) { yah264_encoder_close(e); return NULL; }
        if (ns > 1) {
            e->slice_row0 = malloc((size_t)(ns + 1) * sizeof(*e->slice_row0));
            e->slice_y0 = malloc((size_t)pic_hmb * sizeof(*e->slice_y0));
            if (!e->slice_row0 || !e->slice_y0) {
                free(e->slice_row0); free(e->slice_y0);
                e->slice_row0 = NULL; e->slice_y0 = NULL;
                e->nslices = 1;
            } else {
                for (int s = 0; s <= ns; s++)
                    e->slice_row0[s] = (int)((long)s * pic_hmb / ns);
                for (int s = 0; s < ns; s++)
                    for (int r = e->slice_row0[s]; r < e->slice_row0[s + 1]; r++)
                        e->slice_y0[r] = (int16_t)e->slice_row0[s];
            }
        }
    }

    /* Early-skip probe settings, resolved here so the analyze workers only
 * ever read frame fields. The tolerant acceptance modes, the lookahead MV
 * agreement guard, the cost gate and the deferred B confirmation were
 * refuted in August and removed on 2026-10-04 (local refused index). */
    {
        /* Y264_BSKIP_PROBE=1: the single graded probe, term 1 on its own. It
         * replaces the B round-to-nearest test with the deadzone+trellis one
         * the coder actually runs and commits only where NOTHING survives, so
         * it spends no tolerance and owes no confirmation. */
        const char *v = getenv("Y264_BSKIP_PROBE");
        e->bskip_probe = (v && *v) ? atoi(v) : 0;
        /* Y264_BSKIP_SCREEN: an exact screen in front of the strict B skip
 * probe. It proves the probe's answer from the skip candidate's SSD, which
 * the RD cost has already paid for, and runs the transforms only where the
 * proof fails, so the output cannot move. Default ON; 0 is the escape and
 * reproduces the unscreened path, 2 = cross-check mode. */
        v = getenv("Y264_BSKIP_SCREEN");
        e->bskip_screen = (v && *v) ? atoi(v) : 1;
        /* The three HD-parity stage 2 gates, read from their params (the env
 * overrides were removed on 2026-10-04; the flags are the A/B route).
 * Resolved once here, like every other member of this block.
 *
 * --b-preme-skip <mode> -- the pre-ME B skip verdict.
 * 0 off, 1 non-reference B slices, 2 also reference B's the propagation
 * guard admits. b_preme_bits is the RD floor the verdict reads, default 24:
 * a coded macroblock pays at least its mb_type and cbp syntax, so no
 * coded candidate can score below lambda times that rate, and a skip
 * whose own distortion is already under it has won. 24 is not the
 * syntax minimum -- that is nearer 6 -- it is where the census puts the
 * knee, and the census is what chose it (local/records/). */
        e->b_preme_skip = param->b_preme_skip;
        e->b_preme_bits = param->b_preme_bits;
        if (e->b_preme_skip < 0) e->b_preme_skip = 0;
        /* Modes 3 and 4 read the same field as an ABSOLUTE distortion rather
 * than a bit count, so they cannot share 24 as a default: the two numbers
 * are not in the same units and 24 of SSD selects almost nothing. 512 is
 * the absolute bound the mid-tournament exit already carries. */
        if (e->b_preme_bits <= 0) e->b_preme_bits = e->b_preme_skip >= 3 ? 512 : 24;
        /* --p-part-gate <k> -- refuse the P sub-partition searches when the
 * 16x16 cost is under k*lambda and the neighbourhood is homogeneous. */
        e->p_part_gate = param->p_part_gate;
        if (e->p_part_gate < 0) e->p_part_gate = 0;
        /* --rd-surv-rank <n> -- RD at most n survivors per B candidate set,
 * by rank. 0 keeps the score threshold alone, which is byte-identical. */
        e->rd_surv_rank = param->rd_surv_rank;
        if (e->rd_surv_rank < 0) e->rd_surv_rank = 0;
        /* the two lowres-search thresholds, in SAD and SATD per lowres pixel */
        e->lr_settle = param->lr_settle;
        if (e->lr_settle < 0) e->lr_settle = 0;
        e->lr_subgate = param->lr_subgate;
        if (e->lr_subgate < 0) e->lr_subgate = 0;
        /* Y264_MBT_DEPFLOOR=<n> -- refuse the propagation deposit under n/256
 * of propagation fraction. */
        v = getenv("Y264_MBT_DEPFLOOR");
        e->mbt_depfloor = (v && *v) ? atoi(v) : param->mbt_depfloor;
        if (e->mbt_depfloor < 0) e->mbt_depfloor = 0;
        /* --b-intra-band <m> -- skip the B intra screen and the intra trial
 * in a row band whose lookahead intra cost is more than m/16 of its
 * lookahead inter cost. 0 = off. */
        e->b_intra_band = param->b_intra_band;
        if (e->b_intra_band < 0) e->b_intra_band = 0;
        /* --b8-band <d> -- decline the B_8x8 quadrant gate and its eight
 * searches in a row band whose lookahead pair-leg cost field has a CoV^2
 * under d hundredths. 0 = off. */
        e->b8_band = param->b8_band;
        if (e->b8_band < 0) e->b8_band = 0;
    }

    e->cpu = y264_cpu_detect();
    y264_dsp_init();                                /* auto-select SIMD kernels */
    y264_me_set_subme(param->subme);                /* ME effort (hex vs UMH) */
    y264_me_set_method(param->me_method);           /* --me override (0 = follow subme) */
    y264_me_set_subpel(param->subpel);              /* subpel pattern (preset-scaled) */

    /* --weightp. Zero in the struct is UNSET and resolves to 1, the level
 * every encode before the flag ran at; off is the negative value. */
    {
        int wp = param->weightp;
        if (wp == 0) wp = 1;
        e->weightp = wp < 0 ? 0 : (wp > 2 ? 2 : wp);
    }
    int has_b = e->param.bframes > 0;
    e->sps.entropy_coding_mode_flag = e->param.cabac ? 1 : 0;
    e->sps.profile_idc = y264_profile_idc(e->param.cabac, e->param.bframes); /* Main for CABAC/B, else Baseline */
    /* Annex A.2.1 forbids weighted prediction in Baseline, and until --weightp
 * existed this encoder signalled it unconditionally, so the derivation
 * could never honestly claim Baseline. --weightp 0 turns the signalling
 * off and Baseline becomes the honest answer for a CAVLC stream with no B
 * frames. A.2.1 also requires frame_mbs_only_flag 1, so a sequence that
 * may carry fields stays off it whatever the weight does. */
    if (e->sps.profile_idc == 66 &&
        (e->weightp || e->param.interlaced || e->param.fake_interlaced))
        e->sps.profile_idc = 77;
    /* CAVLC at Main: 9.2.2.1 forbids level_prefix > 15 below High, so the
 * quantiser caps |level| at 2063 (the prefix-15 maximum) and the writer
 * reports if a larger one ever reaches it. High profiles have no cap. */
    e->lev_max = (!e->param.cabac && e->sps.profile_idc < 100) ? 2063 : 0;
    if (e->param.transform8x8)
        e->sps.profile_idc = 100;                   /* 8x8 transform is High profile */
    /* Custom quant matrices require High profile and carry the scaling lists in
 * the SPS. Flat (cqm 0) leaves the sequence scaling matrix absent, so the
 * decoder uses the flat-16 default and output is byte-identical. */
    e->cqm_on = 0;
    if (param->cqm == YAH264_CQM_JVT) {
        y264_cqm_jvt(&e->cqm);
        e->cqm_on = 1;
    } else if (param->cqm == YAH264_CQM_CUSTOM) {
        for (int i = 0; i < 6; i++)
            for (int k = 0; k < 16; k++)
                if (!param->cqm_4x4[i][k]) {
                    fprintf(stderr, "yah264: custom quant matrix 4x4 list %d has a zero weight (1..255)\n", i);
                    yah264_encoder_close(e);
                    return NULL;
                }
        for (int i = 0; i < 2; i++)
            for (int k = 0; k < 64; k++)
                if (!param->cqm_8x8[i][k]) {
                    fprintf(stderr, "yah264: custom quant matrix 8x8 list %d has a zero weight (1..255)\n", i);
                    yah264_encoder_close(e);
                    return NULL;
                }
        memcpy(e->cqm.w4, param->cqm_4x4, sizeof e->cqm.w4);
        memcpy(e->cqm.w8, param->cqm_8x8, sizeof e->cqm.w8);
        /* flat in every list is no matrix at all: the stream stays the flat one */
        e->cqm_on = !y264_cqm_mark_flat(&e->cqm);
    } else if (param->cqm) {
        fprintf(stderr, "yah264: unknown cqm %d (0 flat, 1 jvt, 2 custom)\n", param->cqm);
        yah264_encoder_close(e);
        return NULL;
    }
    if (e->cqm_on) {
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
    /* --intra-refresh: recovery_frame_cnt counts FrameNum and has to stay
 * below MaxFrameNum, so the field widens until a whole sweep fits. */
    while (e->ir_on && e->sps.log2_max_frame_num_minus4 < 12 &&
           (1 << (e->sps.log2_max_frame_num_minus4 + 4)) < e->ir_period)
        e->sps.log2_max_frame_num_minus4++;
    e->sps.pic_order_cnt_type = 0;                  /* explicit POC (enables reordering) */
    e->sps.log2_max_pic_order_cnt_lsb_minus4 = 4;  /* 8-bit poc_lsb */
    /* Sliding window: nref anchors, plus the future anchor for B, plus the
 * pyramid's MARKED reference B's. Charging `bframes` here OVER-CHARGES: the
 * pyramid marks only its internal nodes (1 for bframes 2-3, 2 at 4, 3 at
 * 5-7 -- stair_plan_nrefb reads it off the coding plan itself, the same way
 * dpbp_open sizes its bag pool), and the sliding window never holds more
 * marked ref-Bs than one mini-GOP emits. The over-charge costs two whole DPB
 * frames at medium, enough to push a 720p stream from level 3.1 (what the
 * reference encoder's output declares) to 4.0. The window's SIZE times decoder-side eviction, so the tight
 * window changes which references the pyramid's lists can offer (bits moved
 * when it shipped); its Y264_DPB_TIGHT escape is gone. */
    {
        int nrefb = e->b_pyramid ? e->param.bframes : 0;
        /* The tight window is NOT applied where the wide staircase can engage
         * (ref <= 1 shapes): its slot recycling depends on the wide window's
         * slack -- determ_repeat under load reads 4/16 nondeterministic
         * configs with the tight window there (all ref1) and 16/16 with the
         * wide one. The serial path is proven either way (recon_sweep 300/300
         * tight).
         *
         * THIS IS A CORRECTNESS GUARD, NOT A DETERMINISM ONE, so the
         * 2026-09-25 rule (threaded output need not repeat) does not lift it.
         * Re-checked that day by forcing the tight window into this branch:
         * at --bframes 7 --ref 1 --threads 3 the encoder's --dump-recon
         * disagrees with ffmpeg's decode of the same stream on foreman, bus,
         * stefan and fourpeople (8 of 8 runs, a different stream each run),
         * while --threads 1 with the same forced window matches and the wide
         * window matches at 3 threads. The likely mechanism: a threaded chain
         * still predicts from a reference the tight window has evicted on the
         * decoder's side, so the stream it writes is not the one it
         * reconstructs. Not bisected further. bframes 3 and
         * --direct temporal read clean (56 runs), and TSan reports nothing new
         * at 3 threads, so the only detector is recon-match on bframes 7. */
        /* Plus the rc half: under ABR/2-pass the zero-lag decide keeps width
         * disengaged, so the tight window is still safe there and the SPS
         * (hence the auto level) must not move. stair_wide_rc_ok CANNOT be
         * called here -- e->rcp_on/rcp_lag resolve further down in open, so
         * reading them here sees zeros, passes ABR through and moves level_idc
         * 3.1 -> 4.0. Mirror it from the params this early code does have. */
        int sps_rc_decide = (param->rc.method == YAH264_RC_ABR && param->rc.bitrate > 0)
                || param->rc.pass > 0
                || (param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0);
        int sps_vbv = param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0;
        int sps_wide_rc = !sps_rc_decide || (rcp_lag_for(e) > 0 && !sps_vbv);
        if (e->b_pyramid && !(stair_wide_on() && sps_wide_rc)) {
            nrefb = stair_plan_nrefb(e->param.bframes);
        }
        int mrf = e->nref + (has_b ? 1 : 0) + nrefb;
        /* A field pair's FIRST field is marked for reference before the second
 * one is decoded, and the sliding window runs at the first field, not the
 * second: the current frame therefore occupies a DPB slot of its own for
 * the whole pair. Without the +1 a --ref N field stream would reach only
 * N-1 previous frames. */
        if (e->fields) mrf++;
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
        /* Table A-1's MaxVmvR is in luma FRAME samples; a field picture gets
 * half of it, because a field sample spans two of them. */
        e->mv_ylim_q = e->fields ? 2 * vr : 4 * vr;
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
        int lag_rc_decide = (param->rc.method == YAH264_RC_ABR && param->rc.bitrate > 0)
                || param->rc.pass > 0
                || (param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0);
        int lag_vbv = param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0;
        if (has_b && lag_rc_decide && rcp_lag_for(e) > 0 && !lag_vbv
            && stair_wide_on())
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
    e->sps.frame_mbs_only_flag = (e->param.fake_interlaced || e->fields) ? 0 : 1;
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
 * the one profile that turns a tool off rather than only asserting one --
 * and so, since the flag exists, is --weightp 0. */
    e->pps.weighted_pred_flag = (e->sps.profile_idc == 66 || !e->weightp) ? 0 : 1;
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
    e->crf_max = param->crf_max;
    /* The env knob keeps overriding the flag, as it does for every other pair
 * (the --subpel convention), and it is resolved once here so a getenv does
 * not sit in the per-frame correction. */
    {
        const char *tolenv = getenv("Y264_ABR_TOL");
        double tol = tolenv ? atof(tolenv) : param->ratetol;
        e->ratetol = tol > 0.0 ? tol : 1.0;
    }

    e->qp = param->rc.qp;
    if (e->qp < 0) e->qp = 0;
    if (e->qp > 51) e->qp = 51;
    e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);

    e->abr_on = (param->rc.method == YAH264_RC_ABR && param->rc.bitrate > 0);
    if (e->abr_on) {
        double fps = (param->timebase.fps_num > 0 && param->timebase.fps_den > 0)
                   ? (double)param->timebase.fps_num / param->timebase.fps_den : 25.0;
        /* Every coded picture is charged against this, and PAFF codes two of
 * them per input frame, so the per-picture target is per FIELD. */
        e->abr_target_bpf = (double)param->rc.bitrate * 1000.0 / fps
                          / (e->fields ? 2.0 : 1.0);
        e->abr_qp = 26.0;                           /* initial guess; converges fast */
        /* The per-frame model's scales (bits x qscale / C^(1-qcomp)) and its
 * rf ledger have no value until a frame is coded: the opening I frame sets
 * the I scale and the ledger from the intra-fit opening QP (rcp_decide),
 * and P and B are seeded in their lowres domain where the rcp is set up.
 * RF2 opens its own ledger at its first decide. No slew reference and no P
 * track until a frame is decided. */
        for (int t = 0; t < 3; t++) { e->bm_abr.k[t] = 0.0; e->bm_abr.have[t] = 0; }
        e->abr_fps = fps;
        e->rfm_work = 0.0;
        e->rfm_owed = 0.0;
        e->itrk_qp = 0.0; e->itrk_wt = 0.0;
        e->last_anchor_type = -1;
        e->last_ref_qp[0] = e->last_ref_qp[1] = -1.0;
        for (int t = 0; t < 3; t++) e->slew_ref[t] = -1.0;
        e->blur_csum = 0.0; e->blur_cwt = 0.0;
    }
    e->crf_on = (param->rc.method == YAH264_RC_CRF && param->rc.rf > 0);
    if (e->crf_on) {
        e->crf = param->rc.rf;
        e->crf_qcomp = 0.6;                         /* the --qcomp default */
        e->crf_cl = 1;                              /* mb-tree operating-point devices */
    }
    e->vbv_on = (param->rc.vbv_maxrate > 0 && param->rc.vbv_bufsize > 0);
    if (e->vbv_on) {
        double fps = (param->timebase.fps_num > 0 && param->timebase.fps_den > 0)
                   ? (double)param->timebase.fps_num / param->timebase.fps_den : 25.0;
        /* The buffer is credited once per CODED PICTURE, and PAFF codes two of
 * them per input frame, so a field's share of the rate is half a
 * frame's -- exactly as abr_target_bpf above. Without this the model
 * fills twice as fast as the channel does and the cap reads as double. */
        e->vbv.rate = (double)param->rc.vbv_maxrate * 1000.0 / fps
                    / (e->fields ? 2.0 : 1.0);
        e->vbv.size = (double)param->rc.vbv_bufsize * 1000.0;
        /* Start full unless rc.vbv_init names another occupancy: <= 1 is a
 * fraction of the buffer, above 1 is kbit. 0 = unset = full, the
 * behaviour every stream had before the parameter existed. */
        e->vbv.fill = e->vbv.size;
        if (param->rc.vbv_init > 0) {
            double init = param->rc.vbv_init <= 1.0
                        ? param->rc.vbv_init * e->vbv.size
                        : param->rc.vbv_init * 1000.0;
            if (init > e->vbv.size) init = e->vbv.size;
            e->vbv.fill = init;
        }
        /* The handoff level is the CRF+VBV fill goal (vbv_fill_budget) and not
 * a tunable of its own, because the goal is this loop's fixed point: a
 * segment that starts there starts where its own rate loop returns to, and
 * exits there without being told to, which is why composability needs no
 * machinery beyond this line.
 *
 * An explicit exit constraint is deliberately absent under CRF: it was built,
 * measured, and removed on the measurement. Under a bitrate target the budget
 * does not run, so nothing returns the occupancy to the handoff level by
 * itself; a caller that splits a capped ABR encode asks for the exit corridor
 * through yah264_encoder_vbv_handoff instead. */
        rc_knobs_resolve(e);                    /* the fill goal; resolved again below */
        e->vbv_seg_h = (vbv_segh_env() >= 0 ? vbv_segh_env() : e->rck.vbv_fill_frac) * e->vbv.size;
        /* Under a bitrate target too. The integrator owns the AVERAGE, but
 * nothing about it keeps a GOP from ending with the buffer low, and a
 * per-GOP instance that assumed a full buffer at its start then spent
 * one it did not have: chimera 1080p at 988 kbit/s capped VBR underflowed
 * at the joins. A sequential caller replaces the assumption with the exact
 * occupancy through yah264_encoder_rc_import; a parallel one pairs it with
 * yah264_encoder_vbv_handoff on the segment before. 2-pass plans against
 * real pass-1 bits and keeps what it had. */
        if (param->rc.vbv_seg_join && param->rc.method != YAH264_RC_2PASS)
            e->vbv.fill = e->vbv_seg_h;             /* assume only the handoff */
        e->vbv_opt_join = param->rc.vbv_seg_join && param->rc.method != YAH264_RC_2PASS;
        /* The first coded frame of ANY instance escapes the VBV clip -- the bits
 * model needs one coded frame before a prediction exists. Under a
 * bitrate target the seeded ABR scale covers it; under CRF/CQP nothing
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
 * harmless: the buffer law's clamp is the whole story. */
        e->vbv_cbr = e->abr_on && param->rc.vbv_maxrate <= param->rc.bitrate;
        /* The HRD declaration, derived from the same two numbers the loop above
 * was built from, and from the occupancy it just resolved. Both coded
 * values round UP onto their scale's grid -- by under 64 bit/s and under
 * 16 bits -- because a declaration a shade WIDER than the bucket the rate
 * control enforced is one the stream cannot fail against, while a shade
 * narrower is one it can fail against for no reason but arithmetic. */
        if (param->nal_hrd) {
            e->hrd_on = param->nal_hrd;
            e->hrd_filler = param->filler == YAH264_FILLER_OFF ? 0
                          : param->filler > 0 ? 1
                          : param->nal_hrd == YAH264_NAL_HRD_CBR;
            double br = (double)param->rc.vbv_maxrate * 1000.0;
            e->hrd_bitrate = ceil(br / 64.0) * 64.0;
            e->hrd_cpb = ceil((double)param->rc.vbv_bufsize * 1000.0 / 16.0) * 16.0;
            e->hrd_tpp = e->fields ? 1 : 2;
            /* One picture period of arrival. tc is num_units_in_tick/time_scale
 * and the VUI writes time_scale = 2*fps_num, so a frame is two ticks
 * and a field one -- the same halving vbv_rate does above, for the
 * same reason. */
            e->hrd_rate_pic = e->hrd_bitrate * (double)e->hrd_tpp
                            * (double)param->timebase.fps_den
                            / (2.0 * (double)param->timebase.fps_num);
            e->hrd_full_delay = (unsigned)(90000.0 * e->hrd_cpb / e->hrd_bitrate);
            if (e->hrd_full_delay < 1) e->hrd_full_delay = 1;
            double d0 = 90000.0 * e->vbv.fill / e->hrd_bitrate;
            unsigned i0 = (unsigned)(d0 < 1.0 ? 1.0 : d0);
            if (i0 > e->hrd_full_delay) i0 = e->hrd_full_delay;
            e->hrd_init_delay = i0;
            /* The receiver's ledger starts where the declaration says it does,
 * not where the rate control's does: the two differ by the rounding
 * above and they are allowed to. */
            e->hrd_fill = e->hrd_bitrate * (double)i0 / 90000.0;
            e->hrd_anchor_tick = 0;
            e->hrd_disp_base = -1;
            e->sps.hrd = param->nal_hrd;
            e->sps.hrd_bit_rate = (unsigned)e->hrd_bitrate;
            e->sps.hrd_cpb_size = (unsigned)e->hrd_cpb;
            if (e->hrd_filler) {
                e->hrd_pad_cap = (size_t)(e->hrd_rate_pic / 8.0) + 64;
                e->hrd_pad = malloc(e->hrd_pad_cap);
                if (!e->hrd_pad) { yah264_encoder_close(e); return NULL; }
            }
        }
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
             * unchanged within 0.8%. The plan, its cross-type limiter, the
             * runtime correction and the re-solve are always on; tp_plan_on
             * still drops to 0 when a plan cannot be built. */
            e->tp_plan_on  = 1;
            e->tp_corr     = 1;
            e->tp_bexp     = 1.0;
            /* complexity blur, the --cplxblur default of 20; the planner maps
             * it to a gaussian sigma (tp_blur_cost in rate.c). */
            e->tp_cplxblur = (ev = getenv("Y264_TP_CPLXBLUR")) ? atof(ev)
                           : (e->tp_plan_on ? 20.0 : 0.0);
            e->tp_cwarm    = 4;
            e->tp_resolve  = 1;
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
    /* Deterministic fixed-lag RC feedback: every mode with a bitrate, a pass or
     * a VBV rides the frame pipeline (the serial controller and its
     * Y264_RC_PIPE / Y264_RC_PIPE_VBV escapes were removed 2026-10-04); VBV
     * uses the conservative virtual buffer + per-burst serial fallback. Pure
     * CRF and CQP decide in the serial head. Was: sub-gate off keeps VBV fully
     * serial. */
    /* Resolved here, not in the per-frame path: rcp_account runs on the stair
 * driver under the pipeline. */
    e->tp_rctrace = getenv("Y264_RC_TRACE") != NULL;
    rc_knobs_resolve(e);
    if (e->vbv_opt_join && e->vbv_on && e->abr_on && !e->vbv_cbr && !e->tp_pass
        && e->rck.vbv_entry_opt * e->vbv.size > e->vbv.fill)
        e->vbv_opt = e->rck.vbv_entry_opt * e->vbv.size - e->vbv.fill;
    /* Opt-in ABR model: the public parameter, with the env var as an override
     * for A/B runs. */
    e->abr_rf = abr_rf_env() >= 0 ? abr_rf_env() : (param->rc.abr_model ? 1 : 0);
    e->rcp_on = e->abr_on || e->tp_pass || e->vbv_on;
    e->rcp_vbv_tight = 1;               /* serial until the first burst gate */
    if (e->rcp_on && e->abr_on) {
        /* The rcp P/B decision complexity is the ME-compensated LOWRES cost
 * (never recon), a different domain from the I frame's full-res SATD.
 * Seed P/B with the corpus geometric mean of the CONVERGED Cme-domain
 * scales (foreman/mobile/akiyo/park_joy/samsung, ~90 and ~13 per
 * sqrt(mbs)); the I scale is set by the opening decide. */
        int nmbc = e->width_in_mbs * e->height_in_mbs;
        e->bm_abr.k[1] = 90.0 * sqrt((double)(nmbc > 0 ? nmbc : 1));
        e->bm_abr.k[2] = 13.0 * sqrt((double)(nmbc > 0 ? nmbc : 1));
        e->bm_abr.have[1] = e->bm_abr.have[2] = 1;
    }

    e->pstride[0] = e->padded_w + 2 * Y264_LUMA_BORDER;
    e->pstride[1] = e->padded_w / e->sub_w + 2 * Y264_CHROMA_BORDER;
    e->pstride[2] = e->pstride[1];
    /* Full bordered buffer sizes: plane copies move whole buffers so the
 * borders travel with the pixels (extended once per stored recon). */
    size_t luma = (size_t)e->pstride[0] * (e->padded_h + 2 * Y264_LUMA_BORDER);
    size_t chroma = (size_t)e->pstride[1] * (e->padded_h / e->sub_h + 2 * Y264_CHROMA_BORDER);
    e->plane[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
    e->plane[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
    e->plane[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
    e->rec[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
    e->rec[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
    e->rec[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
    e->ref[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
    e->ref[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
    e->ref[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
    /* Multi-ref ring: slot 0 aliases ref[]; slots 1..nref-1 get their own buffers. */
    e->refring[0][0] = e->ref[0];
    e->refring[0][1] = e->ref[1];
    e->refring[0][2] = e->ref[2];
    for (int i = 1; i < e->nref; i++) {
        e->refring[i][0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
        e->refring[i][1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
        e->refring[i][2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
    }
    e->nref_valid = 0;
    e->ref_navail = 0;
    e->fld_rc_type = -1;
    /* A2: half-pel plane pool. One H/V/C triple per list-0 reference plus one for
 * the list-1 anchor (B slices). Off only on OOM (falls back to on-the-fly
 * interpolation, bit-identical). */
    e->hpel_on = 1;
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
    nr_open(e);
    {
        /* --mcstf: the CLI flag writes Y264_MCSTF; read here, on the opening
         * thread, like every other knob this encoder resolves at open */
        y264_mcstf_cfg mc;
        if (y264_mcstf_cfg_env(&mc)) {
            double rf = param->rc.method == YAH264_RC_CQP ? param->rc.qp
                      : param->rc.rf > 0 ? param->rc.rf : 23.0;
            int sx = e->sub_w == 2 ? 1 : 0, sy = e->sub_h == 2 ? 1 : 0;
            e->mcstf = y264_mcstf_open(&mc, e->width, e->height, sx, sy, rf);
            e->mcstf_qp = mc.qp_off;
            if (!e->mcstf)
                fprintf(stderr, "yah264: --mcstf: out of memory; off\n");
        }
    }
    /* Flat-path owned hpel triples: one per rotating buffer (rec + ref1 + ring),
 * so a stored recon's half-pel build travels with it instead of being redone
 * per referencing slice. Pyramid mode uses the DPB's cached hpel instead. */
    e->flat_hp_on = e->hpel_on && !e->b_pyramid && !e->fields;
    if (e->flat_hp_on) {
        for (int c = 0; c < 3; c++) {
            e->rec_hp[c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
            e->ref1_hp[c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
        }
        for (int i = 0; i < e->nref; i++)
            for (int c = 0; c < 3; c++)
                e->ring_hp[i][c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
    }
    e->lr_w = e->padded_w / 2;
    e->lr_h = e->padded_h / 2;
    e->lowres_cur = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
    e->lowres_prev = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
    e->lr_buf0 = e->lowres_cur;
    {
        size_t lrsz_h = (size_t)e->lr_w * e->lr_h;
        for (int k = 0; k < 3; k++) {
            e->lr_bufhp[0][k] = malloc(lrsz_h * sizeof(pixel));
            e->lr_bufhp[1][k] = malloc(lrsz_h * sizeof(pixel));
        }
        lr_hpreg_set(e->lowres_cur, e->lr_bufhp[0]);
        lr_hpreg_set(e->lowres_prev, e->lr_bufhp[1]);
    }
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
         * the +/-8 clamp. */
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
        e->mbtree_on = (e->param.bframes > 0) || e->la_depth > 0;
        e->abr_rf2 = 0;
        if (e->abr_on) {
            if (!e->mbtree_on) {
                fprintf(stderr, "yah264: the rate-factor ABR model needs mb-tree (bframes > 0); using the per-frame model\n");
            } else {
                e->abr_rf2 = 1;          /* the ledger opens at the first decide */
            }
        }
        /* mb-tree AND AQ are off at constant QP:
         * otherwise --qp N is not constant QP at all but mb-tree- and
         * AQ-modulated QP, which is not what the flag says, and the reference
         * encoder's --qp output carries no per-MB QP variation either. The band
         * deficit is localised to the adaptive-QP subsystem
         * rather than the base coder, so a CQP user would be paying for the
         * bad offset field with no way to ask for what they wanted. */

        /* Y264_MBTREE_OFF=1: measurement probe -- the CQP policy
 * (mb-tree off) applied here, at every RC mode. Skips the per-anchor
 * compute_mbtree walk and the per-MB offset apply; the chain (legs,
 * typing, seeds) and the mbtree_on allocations stay, because the
 * lookahead ME reads lr_subpel whatever mbtree does. Changes bits
 * wherever mbtree ran; exists to price the driver-side mbtree wall
 * share. DEFAULT OFF. */
        { const char *v = getenv("Y264_MBTREE_OFF");
          e->mbtree_skip = v && atoi(v); }
        /* CQP turns mb-tree off via mbtree_skip, NOT mbtree_on: clearing
         * mbtree_on frees lowres_tmp, which the lookahead ME dereferences
         * whatever mb-tree does (see the comment just above, and
         * build_lr_subpel). */
        if (e->param.rc.method == YAH264_RC_CQP) e->mbtree_skip = 1;
        if (e->mbtree_on) {
            e->mbtree_off = malloc(nmb);
            for (int k = 0; k < 64; k++) {
                e->invq_cache[k].key = 0;
                e->invq_cache[k].invq = NULL; e->invq_cache[k].aqoff = NULL;
                e->invq_cache[k].psw = NULL; e->invq_cache[k].psw_ok = 0;
                pthread_mutex_init(&e->invq_cache[k].mx, NULL);
            }
            e->invq_cache_init = 1;
            e->lowres_tmp = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
            e->code_panchor_lr = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
            {
                for (int k = 0; k < 3; k++)
                    e->panchor_hp[k] = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
                lr_hpreg_set(e->code_panchor_lr, e->panchor_hp);
            }
            e->code_panchor_have = 0;
            pthread_mutex_init(&e->lrset_mx, NULL);
            e->lrset_init = 1;
            for (int i = 0; i < Y264_MBT_SUB_MAX; i++) e->mbt_sub_obj[i] = -1;
            for (int g = 0; g < 2; g++) {
                int o = lrset_acquire(e);
                e->lr_sub_obj[g] = o;
                for (int p = 1; p < 16; p++)
                    e->lr_subpel[g][p] = o >= 0 ? e->lrset[o].pl[p]
                        : lr_plane_built(e, p)
                        ? malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel)) : NULL;
            }
        }
        if (e->la_depth > 0) {
            e->lr_seed_mvx = malloc(nmb * sizeof(int16_t));
            e->lr_seed_mvy = malloc(nmb * sizeof(int16_t));
            e->lr_seed_cost = malloc(nmb * sizeof(int32_t));
            size_t lrsz = (size_t)e->lr_w * e->lr_h;
            for (int i = 0; i < e->la_cap; i++) {
                e->la[i].plane[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
                e->la[i].plane[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
                e->la[i].plane[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
                e->la[i].lowres = malloc(lrsz * sizeof(pixel));
                for (int k = 0; k < 3; k++)
                    e->la[i].lr_hp[k] = malloc(lrsz * sizeof(pixel));
                lr_hpreg_set(e->la[i].lowres, e->la[i].lr_hp);
                e->la[i].d_intra = malloc(nmb * sizeof(int32_t));
                e->la[i].hint_lr = NULL; e->la[i].hint_di = NULL; e->la[i].hint = 0;
                for (int g = 0; g < LR_NLEGS; g++)
                    e->la[i].leg[g] = malloc(nmb * sizeof(y264_lr_blk));
            }
            e->la_lr_prev = malloc(lrsz * sizeof(pixel));
            e->la_anchor_lr = malloc(lrsz * sizeof(pixel));
            for (int k = 0; k < 3; k++)
                e->la_anchor_hp[k] = malloc(lrsz * sizeof(pixel));
            lr_hpreg_set(e->la_anchor_lr, e->la_anchor_hp);
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
                /* Band aggregates of the same legs, three int32 per row band.
 * Unlike the per-macroblock cost bank above these ship, so they are
 * allocated always and carried on the async path too. */
                e->bseedb_bytes = 3 * e->height_in_mbs * (int)sizeof(int32_t);
                e->bseedb_pend = malloc((size_t)e->bseedb_bytes);
                for (int b = 0; b < 8; b++)
                    e->bseedb[b] = malloc((size_t)e->bseedb_bytes);
            }
            e->badapt_on = param->badapt && e->param.bframes > 0 && e->la_depth >= 2;
            e->la_prop_a = malloc(nmb * sizeof(double));
            e->la_prop_b = malloc(nmb * sizeof(double));
        }
    }

    e->bframes = e->param.bframes;
    if (e->bframes < 0) e->bframes = 0;
    e->lr_intra_pen = lr_intra_penalty(&e->param);
    if (e->bframes > 7) e->bframes = 7;
    e->nbuf = 0;
    e->ref1[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);                      /* always allocated: uniform */
    e->ref1[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);                    /* reorder logic uses it even */
    e->ref1[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);                    /* with 0 B frames */
    for (int i = 0; i < e->bframes && i < 8; i++) {
        e->blowres[i] = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
        e->bdintra[i] = malloc((size_t)e->width_in_mbs * e->height_in_mbs * sizeof(int32_t));
        e->bdintra_valid[i] = 0;
        {
            for (int k = 0; k < 3; k++)
                e->blowres_hp[i][k] = malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel));
            lr_hpreg_set(e->blowres[i], e->blowres_hp[i]);
        }
        e->bmbtree_off[i] = malloc((size_t)e->width_in_mbs * e->height_in_mbs);
        e->bmbtree_valid[i] = 0;
    }
    for (int i = 0; i < e->bframes; i++) {
        e->bplane[i][0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
        e->bplane[i][1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
        e->bplane[i][2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
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
    e->rcp_lag = stair_wide_capable(e) ? rcp_lag_for(e) : 0;
    if (g_tprof_on > 0) {
        g_cfg[0] = e->param.threads; g_cfg[1] = e->wf_width; g_cfg[2] = e->la_depth; g_cfg[3] = e->la_buf;
        g_cfg[4] = e->param.sync_lookahead; g_cfg[5] = e->rcp_lag; g_cfg[6] = e->param.bframes;
        g_cfg[7] = e->param.keyint; g_cfg[8] = e->param.rc.lookahead; g_cfg[9] = e->param.timebase.fps_num; g_cfg[10] = e->param.timebase.fps_den;
    }

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
            e->dpb[i].plane[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
            e->dpb[i].plane[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
            e->dpb[i].plane[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
            e->dpb[i].mvx = malloc(mvcount * sizeof(int16_t));
            e->dpb[i].mvy = malloc(mvcount * sizeof(int16_t));
            e->dpb[i].refidx = malloc(mvcount);
            e->dpb[i].colpoc = malloc(mvcount * sizeof(int16_t));
            e->dpb[i].used = 0;
            e->dpb[i].hpel_valid = 0;
            if (e->hpel_on)
                for (int c = 0; c < 3; c++)
                    e->dpb[i].hpel[c] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
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
    if (param->rc.method == YAH264_RC_CQP) e->aq_strength = 0.f;   /* constant QP means no AQ */
    /* Resolved here, not in the AQ kernels: they run on the lookahead/mb-tree
 * pool and an unwarmed lazy static there is a race. */
    /* SCOPED TO CRF. The absolute anchor reaches the per-MB AQ field in EVERY
 * rate mode, unlike the two base-QP terms beside it, which carry their own
 * `e->crf_cl` gate -- so arming Y264_CRF_CPLX unscoped moves ABR's output too.
 * ABR is not measured for it, and its band's per-clip noise floor spans 0.2 to
 * 11 points, so measuring it is a round of its own. The -5.17% median is a CRF
 * number. Y264_CRF_AQABS=1 arms it everywhere for that measurement. */
    e->aq_abs    = crf_aqabs_env()
                && (e->crf_on || crf_aqabs_forced()
                    || (e->abr_rf2 && e->rck.abr_dc > 0 && (!e->vbv_cbr || e->rck.abr_dc > 1)));
    /* AQ's rate-mode constants: 1 = a bitrate target, 3 = two-pass (the same
 * path, with its own strength scale and B-frame floor; y264_aq_gain). */
    e->aq_rabr = e->crf_on ? 0 : (e->tp_pass ? 3 : 1);
    e->aq_chroma = 0;               /* luma-only AQ energy */
    e->aq_anchor = aq_anchor_default();
    /* The frame-level (DC) AQ strength: see crf_frame_dc. Fitted by sweep at
 * matched rate (Y264_AQ_DC). The 1.3 sweep was uncapped CRF only. A bitrate
 * target keeps the 1.04 its own bands and rate tracking were measured at, and
 * so does capped CRF: at 1.3 its CBR-declared HRD read 68/72 on the capped-VBR
 * compliance gate (park_joy and ducks overflow at the loosest cap), 72/72 at
 * 1.04. Under a bitrate target only one-pass ABR and capped VBR read it
 * (abr_dc_on); CBR by default, two-pass and CQP never do. */
    { const char *v = getenv("Y264_AQ_DC");
      e->aq_dcstr = v ? atof(v) : (e->crf_on && !e->vbv_on ? 1.3 : 1.04); }
    e->aq_dcfloor = 0.00390625;
    /* Frames with no mb-tree field (non-reference B) inherit the anchor's
 * base QP, frame DC included, and centre their own AQ on their frame mean.
 * aq_pull moves that centre toward a fixed level (aq_bref), so a B whose
 * blocks are busier than that level codes coarser and a flatter one finer:
 * a frame-level term of strength x pull x (mean - bref). Only where the
 * absolute anchor is armed. Swept at matched rate and on the HD band:
 * pull 0 / 0.5 / 0.7 / 1.0 / 1.2 / 1.3 / 1.4 / 1.6 (0 loses 1-2.7% on stefan,
 * sintel and mobile; 1.3 over 1.0 moves stefan's CRF reads from +0.17 / -0.11
 * / +0.10 to +0.15 / -0.58 / -0.09 at three ladder shifts, and its ABR reads
 * from +0.17 / +0.63 at two shifts to -0.21 / +0.18, with mobile's ABR from
 * +0.43 to -0.59; 1.6 costs akiyo under ABR), bref 5.5 / 6.0 / 6.5 / 7.0 / 8.0 (5.5
 * costs fourpeople_720p, 6.5 and up give back ducks_720p's 1%). Pulling
 * only frames above bref (0 or 0.5 below it) bought nothing on
 * perseverance_720p and cost sintel 0.3-0.5%. */
    e->aq_pull = e->aq_abs ? 1.3 : 0.0;
    /* Two-pass runs without the absolute anchor, so its non-reference B frames
 * get the pull directly; see the two-pass note at y264_aq_gain. */
    if (e->tp_pass) e->aq_pull = 1.3;
    e->aq_bref = 6.0;
    e->aq_dc_rqw = 0.25;
    e->crf_dc = 0.0;
    size_t nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
    if (e->aq_strength > 0.f) {
        e->aq_off = malloc(nmb);
        e->mbvar = malloc(nmb * 2 * sizeof(uint32_t));
    }
    if (e->fields) {                     /* the per-field halves (encoder.h) */
        e->fld_aq = malloc(nmb);
        e->fld_mbt = malloc(nmb);
    }
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

    /* W1: in-frame row-wavefront pool from param.frame_threads. >1 => pass-1
 * analysis runs on the wavefront.
 *
 * Capped at the grid's critical-path knee, not at the row count: rows was
 * always the wrong bound (a 22-column row cannot pay for 18 claims however
 * many rows there are), and the knee is never above the row count anyway.
 * Worker count cannot reach the bitstream -- the W1 determinism guarantee --
 * so this is byte-identical by construction. */
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
 * staircase encode's bitstream identical whether the
 * staircase's wavefront/pool machinery actually runs or falls back to the
 * serial path (stair_row_gate's blocking form is exactly what the serial
 * analyze uses). So --threads 1, and every --threads below the pool
 * engagement floor, kept computing the FIXED floor value, or
 * w2_canary's default-path byte-identity broke. (Since 2026-09-25 that
 * identity is no longer required and those widths run uncapped; see
 * below. The row-gate margin itself is unchanged.) Only a pool that has
 * actually reached Y264_MT_POOL_MIN -- the same bar stair_ready and every
 * other stage-3 gate uses -- feeds the real formula; that is
 * exactly the set of configurations whose bits are allowed to vary with
 * --threads. */
    {
        int pt = e->pool ? ntp_pool_nthreads(e->pool) : 0;
        e->stair_lag = (pt >= Y264_MT_POOL_MIN) ? stair_lag_for(e->height_in_mbs, pt)
                                                 : Y264_STAIR_LAG;
        e->stair_mvy_max = 4 * (16 * e->stair_lag - 24);
        /* Below the pool floor no staircase can run, so nothing streams and
 * the bound protects no reader. It stayed at the floor value (40 px)
 * until 2026-09-25 only so that the one-thread bits matched the
 * threaded ones; threaded output no longer has to (owner, that day),
 * so the bound goes to INT_MAX here, which every compare against it
 * treats as uncapped. The clamp SET is untouched (stair_clamp_on).
 * "No pool" is not only --threads 1: the CLI's streamed split runs N GOP
 * workers at one frame-thread each, so every such encode sat on the 40 px
 * bound too, and at 4K it binds on ordinary motion: a credit roll
 * at 10 px a frame is 41 px between P anchors, and the P frames coded as
 * near-intra at eight times the bits (Big Buck Bunny 2160p, 2026-09-26). */
        if (pt < Y264_MT_POOL_MIN)
            e->stair_mvy_max = INT_MAX;
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

    /* Decoupled lookahead thread (see struct la_thread): the per-frame
 * lookahead chain runs on a dedicated thread behind the ring, ahead of the
 * encoder in push order. Engage gates: a resolved lead to run into
 * (e->la_buf > 0, so --sync-lookahead 0 runs threadless, and a chain with
 * zero lead measured a wash here from the day it landed); a pool of >=
 * LA_POOL_MIN workers (below that there is no pool for the wavefront
 * either, so nothing to run ahead of); and
 * la_depth >= bframes+3 so every
 * popped entry's chain writers are strictly older than the newest push. On
 * any failure fall back silently to the in-line chain (byte-identical
 * either way). */
    if (e->la_buf > 0 && e->la_depth >= e->bframes + 3 &&
        e->pool && ntp_pool_nthreads(e->pool) >= LA_POOL_MIN) {
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

    /* The warm needs a window to warm (its lead comes from the ring, not from
 * spare ring capacity) and an off-driver chain to lead: mbt_warm_window
 * returns at its first line when there is no la thread, so without one the
 * thread would only be a cond-var round trip per anchor -- which is the
 * whole cost the warm would otherwise add at --threads 1. */
    if (e->la_th_on && e->la_depth > 1 && e->mbtree_on) {
        struct mbt_pre *mp = calloc(1, sizeof *mp);
        if (mp) {
            pthread_mutex_init(&mp->mx, NULL);
            pthread_cond_init(&mp->cv_req, NULL);
            pthread_cond_init(&mp->cv_done, NULL);
            mp->bg = ntp_bg_create_named("y264-mbtree");
            if (mp->bg) {
                e->mbtp = mp;
                ntp_bg_submit(mp->bg, mbt_pre_main, e);
            } else {
                pthread_mutex_destroy(&mp->mx);
                pthread_cond_destroy(&mp->cv_req);
                pthread_cond_destroy(&mp->cv_done);
                free(mp);
            }
        }
    }
    y264_recs_cache_ref(+1);
    e->rcr = rcr_open_env(e->param.threads, e->width, e->height);
    rcr_set_vbv(e->rcr, e->param.rc.vbv_maxrate, e->param.rc.vbv_bufsize, e->param.rc.vbv_init,
                e->param.timebase.fps_num, e->param.timebase.fps_den);
    return e;
}

/* SEI user_data_unregistered (payloadType 5): a fixed yah264 UUID plus a human
 * settings string, so a stream's config is verifiable from
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
        "aq=%.2f mbtree=%d keyint=%d %s",
        yah264_version(), p->cabac, p->ref, p->bframes, p->transform8x8,
        p->subme > 0 ? p->subme : 10, e->aq_strength,
        p->rc.lookahead > 0 ? 1 : 0, p->keyint, rc);
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

    /* The buffering period goes FIRST among an access unit's SEI messages
 * (7.4.1.2.3), which is why this segment's own is written here beside the
 * parameter sets rather than by the slice-side opener that writes every
 * other one: the settings message below would otherwise precede it. The
 * delay it declares is the occupancy the rate control was initialised with
 * -- full for a stream that starts the output, the handoff level for a
 * segment that follows one -- so a receiver is told the buffer this encode
 * actually assumed rather than the widest one the syntax allows. */
    if (e->sps.hrd) {
        uint8_t msg[16];
        size_t n = y264_sei_buffering_period(msg, sizeof msg, e->sps.sps_id,
                                             e->hrd_init_delay,
                                             e->hrd_full_delay - e->hrd_init_delay);
        y264_bs_init(&bs, e->rbsp, e->rbsp_cap);
        y264_sei_write(&bs, 0, msg, n);
        if (append_nal(e, &off, YAH264_NAL_PRIORITY_DISPOSABLE, YAH264_NAL_SEI,
                       e->rbsp, (size_t)(bs.p - bs.start)) < 0)
            return -1;
    }

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

/* The lowres half-pel samples taken from the source at one-pixel offsets (right,
 * down, both) with the same 2x2 average as the whole-pel plane, instead of
 * interpolated from the lowres plane. A bilinear lowres phase is a low-pass of
 * a low-pass, so on fine texture under small motion it reads a residual the
 * true half-pel sample does not have, and the error grows with the distance
 * the vector spans. The rows and columns past the frame repeat the last one. */
static void lr_hp_build(pixel *const hp[3], const pixel *src, int ss, int pw, int ph,
                        int lw, int lh)
{
#define LRHF(a, b, c, d) (((((a) + (b) + 1) >> 1) + (((c) + (d) + 1) >> 1) + 1) >> 1)
    /* the clamps only ever bite on the last column (x2 = 2*lw) and the last
     * row (y2 = 2*lh), so the body runs clamp-free and vectorises */
    int xe = 2 * lw <= pw - 1 ? lw : lw - 1;
    for (int y = 0; y < lh; y++) {
        int y0 = 2 * y, y1 = y0 + 1 < ph ? y0 + 1 : ph - 1, y2 = y0 + 2 < ph ? y0 + 2 : ph - 1;
        const pixel *r0 = src + y0 * ss, *r1 = src + y1 * ss, *r2 = src + y2 * ss;
        pixel *restrict oh = hp[0] + y * lw, *restrict ov = hp[1] + y * lw, *restrict oc = hp[2] + y * lw;
#if Y264_HAVE_SSE4
        /* the x86 tiers take the clamp-free span as one dispatched row */
        y264_lr_hp_row(oh, ov, oc, r0, r1, r2, xe);
#else
        for (int x = 0; x < xe; x++) {
            int x0 = 2 * x, x1 = x0 + 1, x2 = x0 + 2;
            oh[x] = (pixel)LRHF(r0[x1], r1[x1], r0[x2], r1[x2]);
            ov[x] = (pixel)LRHF(r1[x0], r2[x0], r1[x1], r2[x1]);
            oc[x] = (pixel)LRHF(r1[x1], r2[x1], r1[x2], r2[x2]);
        }
#endif
        for (int x = xe; x < lw; x++) {
            int x0 = 2 * x, x1 = x0 + 1 < pw ? x0 + 1 : pw - 1, x2 = x0 + 2 < pw ? x0 + 2 : pw - 1;
            oh[x] = (pixel)LRHF(r0[x1], r1[x1], r0[x2], r1[x2]);
            ov[x] = (pixel)LRHF(r1[x0], r2[x0], r1[x1], r2[x1]);
            oc[x] = (pixel)LRHF(r1[x1], r2[x1], r1[x2], r2[x2]);
        }
    }
#undef LRHF
}

/* The fifteen quarter-pel phase planes (the layout build_lr_subpel writes)
 * from the whole-pel plane and the three half-pel planes. Along each axis a
 * sample is this whole pel (0), the half pel after it (2) or the next whole
 * pel (1). A half-pel phase is one sample; a quarter phase is the rounded
 * mean of the two samples that straddle it, and a diagonal quarter takes the
 * two nearest half-pel lines, the way the standard's own luma quarter
 * positions are formed from its half-pel ones. */
static void lr_phases_from_hp(pixel *const sp[16], const pixel *lr, pixel *const hp[3],
                              int lw, int lh)
{
    const pixel *pl[4] = { lr, hp[0], hp[1], hp[2] };   /* (col half) + 2 * (row half) */
    for (int phs = 1; phs < 16; phs++) {
        if (!sp[phs]) continue;             /* a sparse phase set, as build_lr_subpel_1 allows */
        int fx = phs & 3, fy = phs >> 2;
        int cx[2], cy[2];
        if (!(fx & 1) && !(fy & 1)) {
            cx[0] = cx[1] = fx ? 2 : 0; cy[0] = cy[1] = fy ? 2 : 0;
        } else if ((fx & 1) && (fy & 1)) {
            cx[0] = 2; cy[0] = fy == 1 ? 0 : 1;
            cx[1] = fx == 1 ? 0 : 1; cy[1] = 2;
        } else if (fx & 1) {
            cx[0] = fx == 1 ? 0 : 2; cx[1] = fx == 1 ? 2 : 1;
            cy[0] = cy[1] = fy ? 2 : 0;
        } else {
            cy[0] = fy == 1 ? 0 : 2; cy[1] = fy == 1 ? 2 : 1;
            cx[0] = cx[1] = fx ? 2 : 0;
        }
        const pixel *a = pl[(cx[0] == 2) + 2 * (cy[0] == 2)];
        const pixel *b = pl[(cx[1] == 2) + 2 * (cy[1] == 2)];
        int ax = cx[0] == 1, ay = cy[0] == 1, bx = cx[1] == 1, by = cy[1] == 1;
        int two = (fx & 1) || (fy & 1);
        for (int y = 0; y < lh; y++) {
            int ya = y + ay < lh ? y + ay : lh - 1, yb = y + by < lh ? y + by : lh - 1;
            const pixel *ra = a + ya * lw, *rb = b + yb * lw;
            pixel *restrict o = sp[phs] + y * lw;
            /* only the last column can clamp (an offset of one past lw-1) */
            int xe = lw - 1;
            if (two) {
                for (int x = 0; x < xe; x++) o[x] = (pixel)((ra[x + ax] + rb[x + bx] + 1) >> 1);
            } else {
                for (int x = 0; x < xe; x++) o[x] = ra[x + ax];
            }
            for (int x = xe; x < lw; x++) {
                int xa = x + ax < lw ? x + ax : lw - 1, xb = x + bx < lw ? x + bx : lw - 1;
                int v = ra[xa];
                if (two) v = (v + rb[xb] + 1) >> 1;
                o[x] = (pixel)v;
            }
        }
    }
}

/* Downscale a padded luma plane to half resolution by 2x2 averaging. */
static void downscale(pixel *dst, int dw, int dh, const pixel *src, int ss)
{
    IP_SCOPE_B(IP_LA_DS);
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

/* What a lowres intra block pays on top of its neighbour-predicted SATD
 * before it competes with an inter vector (the mb-tree propagate share, B
 * placement, scene cuts). It stands for the side cost an intra block carries
 * that the estimate leaves out, so it grows with the quantiser the way a rate
 * term does: 24 at rate factor 29, doubling every 4 steps. Units are this
 * tree's doubled SATD at 8 bits.
 *
 * Fit by sweep on the HD band (CRF 23-35) and the CRF band at matched rate
 * (rate factors ~16-30). A fixed penalty can't serve both: 20 read HD median
 * -0.20% but CRF +0.08%, 64 read CRF +0.18% (worst +1.06%), 192 read HD
 * -1.00% but riverbed +0.35%. Tied to the rate factor, larger or steeper
 * forms (128-192 at 29, doubling every 3-6) broke riverbed at the top rungs
 * (+1.4% to +1.9%). A grid of 16-24 at 29, doubling every 3-5, was screened
 * on BD-PSNR over tempete (three windows) and akiyo, then on the CRF band:
 * 16/3, 18/3 and 24/5 lost tempete's PSNR; 18/4 read CRF median +0.02%; 20/5
 * put sintel at +0.52%. Of the two finalists, 20 doubling every 3 read HD
 * median -0.14%, CRF -0.04% (worst +0.42%), akiyo +0.28% in one of five
 * windows; 24 doubling every 4 read HD median -0.18% (worst bbb +0.40%,
 * inside that clip's null), CRF -0.03% (worst +0.23%), BD-PSNR median -0.15%
 * over 22 clips, tempete -0.22% and akiyo -0.35% averaged over their
 * windows. The CQP offset is the CRF-to-QP gap the medium preset shows on the
 * band ladders (crf 23.45 runs at qp ~28.9).
 *
 * ABR and 2-pass have no target quantiser ahead of the lookahead, so they read
 * the curve at a fixed rate factor. The readings below depend only on the
 * penalty an anchor yields, so they are stated as penalties. ABR band
 * medians against 5: 4 read -0.06% (worst +0.16%), 6 read +0.06% (worst
 * +0.56%), 7 read +0.02% (worst +0.48%). A 2-pass spot check (foreman, bus,
 * park_joy, sintel; four rates each) went the other way: 4 put sintel at
 * +0.55%, 6 read +0.31% there, 7 read -0.06% to +0.08%. Rate error moved by
 * under 0.1 point at every value. 5 is the one that reads worse in neither
 * mode, and on this curve it falls at rate factor 20. */
static int lr_intra_penalty(const yah264_param_t *p)
{
    double rf = 20.0;
    if (p->rc.method == YAH264_RC_CRF) rf = p->rc.rf;
    else if (p->rc.method == YAH264_RC_CQP) rf = p->rc.qp - 5.5;
    long v = lround(24.0 * pow(2.0, (rf - 29.0) / 4.0));
    return v > 4096 ? 4096 : (int)v;
}

/* lowres intra cost: a
 * NEIGHBOUR-predicted 8x8 (DC/H/V/plane, the chroma prediction set),
 * measured with the SAME satd8x8 as the inter metric (blk8_satd), plus the
 * intra penalty `pen` (see lr_intra_penalty). This replaces the
 * legacy per-4x4-vs-own-mean cost, whose own-DC + per-subblock mean systematically
 * UNDER-reads intra on smooth/gradient motion content (bus/stefan) and is metric-
 * inconsistent with the DC-inclusive inter satd8x8 -- an underread denominator that
 * clamps the mb-tree propagate fraction regardless of inter quality.
 * The lowres plane has no border, so edge blocks use
 * available-neighbour fallbacks instead of a padded border.
 * Producers dispatch via blk8_intra_dispatch. */
static long blk8_intra_neighbour(const pixel *s, int ss, int have_top, int have_left,
                                 const pixel *pb, int i10, int pen)
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
    if (have_top && have_left && y264_dsp.satd_quad_8x8) {
        /* both edges: the four predictions built first and scored in one
         * batched call, then the same ordered strict-< pick (DC, V, H,
         * plane) the per-mode code below makes */
        pixel p4[4][64];
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
        y264_intra8_dvhp(p4, top, left, dc, a, b, c2);
        int sc[4];
        y264_dsp.satd_quad_8x8(s, ss, p4[0], p4[1], p4[2], p4[3], 8, sc);
        best = sc[0];
        for (int k = 1; k < 4; k++) if (sc[k] < best) best = sc[k];
        goto dir_modes;
    }
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
    /* i10 1: also the six directional 8x8 modes (the standard's
     * Intra8x8 modes 3-8 over its low-passed edge), so the lowres intra
     * estimate scores ten modes rather than four. The estimate read 4-18% high
     * against a ten-mode one; corrected inter costs (the half-pel planes) against an
     * over-read intra raise every block's propagated share. The lowres plane
     * is a source plane, so the samples above-right exist wherever the block
     * is not on the right edge; they are left out (replicated), as at the
     * edge. */
dir_modes:;
    /* i10: the caller's mode (LR_I10_MODE) */
    /* =3: the six only where the block's zero-motion residual against the
     * previous frame (pb, same stride) reaches a quarter of the four-mode
     * estimate. The propagated share 1 - inter/intra moves with the intra
     * error times inter/intra, so where even standing still predicts the
     * block to within a quarter of its intra cost, an exact intra changes
     * the share by little; with no previous frame the six always run. */
    if (i10 == 3) i10 = (!pb || 4 * blk8_satd(s, ss, pb, ss) >= best) ? 1 : 0;
    if (i10 == 1 && have_top && have_left) {
        /* the six built from one filter pass and scored four to a batched
         * call, then compared in mode order as the per-mode loop does */
        pixel mp[6][64];
        int sc[6], k = 0;
        y264_intra8x8_dir6_rec(mp, s, ss);
        if (y264_dsp.satd_quad_8x8) {
            y264_dsp.satd_quad_8x8(s, ss, mp[0], mp[1], mp[2], mp[3], 8, sc);
            k = 4;
        }
        for (; k < 6; k++) sc[k] = y264_dsp.satd8x8(s, ss, mp[k], 8);
        for (k = 0; k < 6; k++) if (sc[k] < best) best = sc[k];
    } else if (i10 == 2 && have_top && have_left) {
        /* =2: the six directional modes screened by SAD, the SAD winner
         * re-scored in SATD (one SATD instead of six) */
        y264_i8_edge_t ed;
        y264_intra8x8_edge(&ed, s, ss, 1, 1, 1, 0);
        pixel bp[64];
        int bsad = INT_MAX;
        for (int m = 3; m <= 8; m++) {
            y264_intra8x8_from_edge(pred, &ed, m, 1, 1);
            int c = y264_dsp.sad[Y264_PU_8x8](s, ss, pred, 8);
            if (c < bsad) { bsad = c; memcpy(bp, pred, sizeof bp); }
        }
        long c = y264_dsp.satd8x8(s, ss, bp, 8); if (c < best) best = c;
    }
    return ((best + pen) >> (Y264_BIT_DEPTH - 8));
}

static long blk8_intra_dispatch_p(const pixel *s, int ss, int mx, int my, const pixel *pb,
                                  int i10, int pen);
static long blk8_intra_dispatch(const pixel *s, int ss, int mx, int my, int i10, int pen)
{
    return blk8_intra_dispatch_p(s, ss, mx, my, NULL, i10, pen);
}
/* pb: the co-located block of the previous frame (stride ss), or NULL */
static long blk8_intra_dispatch_p(const pixel *s, int ss, int mx, int my, const pixel *pb,
                                  int i10, int pen)
{
    return blk8_intra_neighbour(s, ss, my > 0, mx > 0, pb, i10, pen);
}

/* Best integer-pel inter cost of an 8x8 lowres block via a shrinking-step diamond
 * search; returns the cost and writes the winning lowres MV. */
/* Score this search's probes with SAD and pay SATD once
 * for the winner (the cheap compare during the walk,
 * the full metric for the winner). This is the
 * push-time cost of every frame against its previous frame, and it scored
 * a five-level diamond with SATD at every probe: ~60 SATDs per block per
 * frame, 4.8G SATD pixels on sunflower_1080p -- a quarter of ALL our SATD.
 *
 * Together with the pair-leg search's and Phase A's integer levels' own SAD
 * probes, the three take our SATD
 * volume on sunflower_1080p from 18.1G pixels to 10.0G and are
 * priced as one change. Wall at --threads 12, medians of 5: sunflower -4.3%,
 * shields -2.9%, samsung -3.5%, pedestrian -3.3%, foreman -3.4%, park_joy
 * -2.2%; at t1 sunflower -3.2%, shields -2.0%. CRF band, 16 clips at 150f:
 * median -0.02%, range -1.26 (foreman) .. +1.24 (sintel). */
static long blk8_inter(const pixel *sb, int ss, const pixel *ref, int rs,
                       int lw, int lh, int bx, int by, int *outmvx, int *outmvy)
{
    int cx = 0, cy = 0;
#define PREV_PROBE(p) ((long)y264_dsp.sad[Y264_PU_8x8](sb, ss, (p), rs))
    long best = PREV_PROBE(ref + by * rs + bx);
    for (int step = 16; step >= 1; step >>= 1) {
        for (int iter = 0; iter < 8; iter++) {
            int cand[4][2] = { {cx + step, cy}, {cx - step, cy},
                               {cx, cy + step}, {cx, cy - step} };
            int moved = 0;
            int x0 = bx + cx, y0 = by + cy;
            if (x0 - step >= 0 && y0 - step >= 0 &&
                x0 + step <= lw - 8 && y0 + step <= lh - 8) {
                /* all four inside: one batched SAD call, then the same
                 * in-order strict-< pick the single probes make */
                int sc[4];
                const pixel *p0 = ref + y0 * rs + x0;
                y264_dsp.sad_quad[Y264_PU_8x8](sb, ss, p0 + step, p0 - step,
                                             p0 + step * rs, p0 - step * rs, rs, sc);
                for (int k = 0; k < 4; k++)
                    if (sc[k] < best) { best = sc[k]; cx = cand[k][0]; cy = cand[k][1]; moved = 1; }
            } else {
                for (int k = 0; k < 4; k++) {
                    int rx = bx + cand[k][0], ry = by + cand[k][1];
                    if (rx < 0 || ry < 0 || rx > lw - 8 || ry > lh - 8) continue;
                    long c = PREV_PROBE(ref + ry * rs + rx);
                    if (c < best) { best = c; cx = cand[k][0]; cy = cand[k][1]; moved = 1; }
                }
            }
            if (!moved || best == 0) break;
        }
    }
#undef PREV_PROBE
    /* back into the SATD domain the consumers read */
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
 * Predictor is the raster left neighbour, so coherent pans stay cheap. */
#define MBTREE_MVLAMBDA 8.0

/* The IPPP anti-saturation treatment (price the lowres MV into the chain
 * propfrac + the wide QP-offset clamp) to the bframes>0 path too. It was
 * originally IPPP-only, leaving the medium (B-frame) chain to over-propagate over
 * ~13 anchor hops and saturate the +/-8 clamp -- a +13-20% BD gap against the
 * reference encoder on motion clips. So every path takes it. */

/* Analyse the current lowres frame against the previous one, caching per-MB intra
 * and best-inter costs and the winning lowres MV. */
/* One macroblock row of the lowres analysis. Every block reads only the
 * (read-only) lowres planes and writes its own slot, so rows are independent. */

static void lowres_row(yah264_encoder_t *e, int my)
{
    NLED_SITE(Y264_LED_SITE_LOWRES);
    int lw = e->lr_w, wmb = e->width_in_mbs;
    const pixel *cur = e->lowres_cur, *prev = e->lowres_prev;
    for (int mx = 0; mx < wmb; mx++) {
        const pixel *sb = cur + (my * 8) * lw + mx * 8;
        int idx = my * wmb + mx, mvx = 0, mvy = 0;
        e->lr_intra[idx] = (int)blk8_intra_dispatch_p(sb, lw, mx, my,
                                 e->sc_have_prev ? prev + (my * 8) * lw + mx * 8 : NULL, LR_I10_MODE, e->lr_intra_pen);
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
 * only blows up on motion. No threshold is BD-gated, so no frame takes it
 * (me_cheap stays 0); Y264_ADME_LOG=1 dumps per-frame scores for calibration. */

static int psy_flat_log(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_PSY_FLAT_LOG"); v = e ? atoi(e) : 0; }
    return v;
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
 * offset, lower QP) weigh MORE, so they propagate more importance. Weights
 * are normalised to mean 1 so the global propagation
 * magnitude is unchanged (the ratio in the finish step is scale-invariant).
 * When aq_off is non-NULL the raw qp_offset_aq (unclamped, float) is returned
 * there for folding into the combined offset. */
/* Sum-of-squared-deviations of a w x h block, the per-plane
 * AC-energy variance for a plane
 * whose block is not 16x16 (chroma at 4:2:0 / 4:2:2). Returns npix*var, the
 * same units as the luma term. */
double blk_ac_energy(const pixel *p, int stride, int w, int h)
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

static void mbtree_invqscale(const yah264_encoder_t *e, pixel *const *pl,
                             int stride, int wmb, int hmb,
                             float strength, double *invq, float *aq_off)
{
    IP_SCOPE_B(IP_MBT_INVQ);
    int n = wmb * hmb;
    double sum = 0, sum2 = 0;
    for (int mby = 0; mby < hmb; mby++)
        for (int mbx = 0; mbx < wmb; mbx++) {
            const pixel *s = pl[0] + (mby * 16) * stride + mbx * 16;
            uint32_t v2[2];
            y264_dsp.var16x16(s, stride, v2);
            uint32_t s1 = v2[0], s2 = v2[1];
            double mean = s1 / 256.0, var = s2 / 256.0 - mean * mean;
            if (var < 0) var = 0;
            double l = log2(var + y264_aq_afloor());
            invq[mby * wmb + mbx] = l;
            sum += l; sum2 += l * l;
        }
    /* Under the absolute anchor (Y264_CRF_CPLX) the field is centred on a
 * constant instead of the frame mean, so its DC is a frame-level complexity
 * term. The propagation weights are renormalised to mean 1 below, so a DC
 * shift leaves them untouched; it reaches the bitstream through aq_off --
 * and through the unnormalised intra-cost weight in the mb-tree finish
 * step, which is why a flat clip's AQ discount damps its own mb-tree boost
 * there. aq-mode 2 scales the strength by the frame's gain. */
    int abs_aq = e->aq_abs;
    double fmean = sum / n, fsd2 = sum2 / n - fmean * fmean;
    double gain = y264_aq_gain(fmean, fsd2 > 0 ? sqrt(fsd2) : 0, 0, e->aq_rabr);
    double avg = abs_aq ? e->aq_anchor : fmean, wsum = 0;
    double astr = strength * gain;
    for (int i = 0; i < n; i++) {
        double base = invq[i] - avg;
        if (aq_off) aq_off[i] = (float)(astr * base);
        invq[i] = pow(2.0, -(astr * base) / 6.0);
        wsum += invq[i];
    }
    double norm = n / wsum;
    for (int i = 0; i < n; i++) invq[i] *= norm;
}

/* mbtree_invqscale through the per-encoder cache. The field is a pure
 * function of the frame's pixels and the encoder's AQ constants, and every
 * buffer one frame lives in (its ring slot, a buffered-B plane, the anchor's
 * stable copy) holds the same pixels, so the frame's lookahead push index is
 * a sufficient key. key <= 0 (unknown) or a strength other than the
 * encoder's computes directly. */

static void mbtree_invqscale_k(yah264_encoder_t *e, long key, pixel *const *pl,
                               int stride, int wmb, int hmb, float strength,
                               double *invq, float *aq_off)
{
    if (key <= 0 || strength != e->aq_strength || !e->invq_cache_init) {
        mbtree_invqscale(e, pl, stride, wmb, hmb, strength, invq, aq_off);
        return;
    }
    size_t n = (size_t)wmb * hmb;
    struct invq_slot *sl = &e->invq_cache[key % 64];
    pthread_mutex_lock(&sl->mx);
    if (sl->key != key) {
        if (!sl->invq) sl->invq = malloc(n * sizeof(double));
        if (!sl->aqoff) sl->aqoff = malloc(n * sizeof(float));
        if (!sl->invq || !sl->aqoff) {
            pthread_mutex_unlock(&sl->mx);
            mbtree_invqscale(e, pl, stride, wmb, hmb, strength, invq, aq_off);
            return;
        }
        mbtree_invqscale(e, pl, stride, wmb, hmb, strength, sl->invq, sl->aqoff);
        sl->key = key; sl->psw_ok = 0;
    }
    memcpy(invq, sl->invq, n * sizeof(double));
    if (aq_off) memcpy(aq_off, sl->aqoff, n * sizeof(float));
    pthread_mutex_unlock(&sl->mx);
}

/* The Phase A source weight 2^(-aq_off/6) of every macroblock, through the
 * same cache. It is a pure function of the frame's coded AQ field, and a frame
 * is a Phase A source in several walks (each memo miss), so the exp2 per block
 * runs once per frame instead of once per miss. Same expression on the same
 * floats, so the values are identical. */
static void mbtree_psw_k(yah264_encoder_t *e, long key, pixel *const *pl,
                         int stride, int wmb, int hmb, double *psw, float *aqtmp)
{
    size_t n = (size_t)wmb * hmb;
    struct invq_slot *sl = (key > 0 && e->invq_cache_init) ? &e->invq_cache[key % 64] : NULL;
    if (sl) {
        pthread_mutex_lock(&sl->mx);
        if (sl->key == key && sl->psw_ok) {
            memcpy(psw, sl->psw, n * sizeof(double));
            pthread_mutex_unlock(&sl->mx);
            return;
        }
        pthread_mutex_unlock(&sl->mx);
    }
    mbtree_invqscale_k(e, key, pl, stride, wmb, hmb, e->aq_strength, psw, aqtmp);
    for (size_t i = 0; i < n; i++) psw[i] = pow(2.0, -(double)aqtmp[i] / 6.0);
    if (sl) {
        pthread_mutex_lock(&sl->mx);
        if (sl->key == key && !sl->psw_ok) {
            if (!sl->psw) sl->psw = malloc(n * sizeof(double));
            if (sl->psw) { memcpy(sl->psw, psw, n * sizeof(double)); sl->psw_ok = 1; }
        }
        pthread_mutex_unlock(&sl->mx);
    }
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
    for (int i = 0; i < nmb; i++) in[i] = 0;
    for (int k = na - 1; k >= 0; k--) {
        struct la_entry *en = &e->la[idx[k]];
        double *dst = (k == 0) ? prop : out;
        if (k != 0)
            for (int i = 0; i < nmb; i++) out[i] = 0;
        /* AQ-aware self-term: this entry's intra importance weighs by its own
 * inv_qscale (flat blocks propagate more), matching the anchor/B path. */
        if (invq)
            mbtree_invqscale_k(e, en->push_idx, en->plane, e->pstride[0], wmb, hmb,
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

/* Quarter-pel splat (8-lowres-px MB cell = 32 quarter-units,
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

/* Precompute the quarter-pel subpel phase-planes of one lr_w*lr_h reference
 * (stride lw): the six single-axis phases (the others are interpolated where
 * they are read above ~1080p, see lr_plane_built). plane[phase], phase=(fy<<2)|fx, holds the SAME bilinear
 * blk8_satd_qp computes on the fly -- byte-identical, just hoisted out of the
 * per-candidate ME loop so every block in the frame reuses one interpolation.
 * The bilinear at (Y,X) reads integer (Y..Y+ay, X..X+ax); the last ay rows / ax
 * cols are never addressed by a valid lowres search (bounds guarantee it) so they
 * are left unfilled. */
/* Lowres-subpel probe, off by default and byte-identical. It prices the one
 * otherwise unmeasured plane region: the 15 quarter-pel phase-planes this
 * function builds per source frame.
 *
 * Y264_LRSUB_CENSUS=1 count, per phase, how many blk8_satd_qp reads land on
 * it and which plane rows any of them touch. The build is
 * skippable only where nothing reads.
 *
 * The census marks in blk8_satd_qp, which is on the lowres ME's inner loop, so
 * it is a plain non-atomic store behind one read-once flag: run at --threads 1.
 */
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
 * read of what one build costs. */
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
    fprintf(stderr, "  cache sets taken from the lookahead (not built): %llu\n",
            (unsigned long long)g_lrs_plan[15]);
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

/* Every lowres phase build (mb-tree Phase A, the anchor and B pair legs)
 * takes the source half-pel planes when the reference lowres plane has them,
 * instead of interpolating the lowres plane. The planes are found by the
 * lowres plane's address in a small process-wide registry (lookahead ring
 * slots and the retained anchor; a plane with none, e.g. mb-tree's
 * re-downscaled coded frames, keeps the bilinear build). The lookahead's own
 * field-ME legs (the anchor field and the B pair legs) also try the predictor
 * at quarter-pel before the subpel refine (lr_me_block's psub), and the lowres
 * intra estimate is LR_I10_MODE.
 *
 * Those three are the corrected lowres costs. Against the earlier default,
 * BD-VMAF-NEG at matched rate: t1 CRF band median -0.43, worst akiyo +0.68
 * (coastguard -1.68, mobile -1.55, foreman -1.49); --threads 4 median -0.48,
 * worst akiyo +0.72; multi-shot cif -0.65, 720p -2.46, 1080p -0.41; HD band
 * sunflower +0.01, pedestrian -0.85, riverbed -0.68. Instructions at
 * --threads 12: +0.7% at 1080p, +1.7% at 2160p. CPU (meridian 2160p 600f,
 * --threads 12, instructions retired): the half-pel planes alone -0.5% (their
 * builders run clamp-free and beat the bilinear build), the quarter-pel
 * predictor +1.2% (one more SATD per block per leg). */
#define LR_HPREG_N 2048
static struct { const pixel *lr; pixel *const *hp; } g_lr_hpreg[LR_HPREG_N];
static pthread_mutex_t g_lr_hpreg_mx = PTHREAD_MUTEX_INITIALIZER;
static void lr_hpreg_set(const pixel *lr, pixel *const *hp)
{
    pthread_mutex_lock(&g_lr_hpreg_mx);
    int free_i = -1;
    for (int i = 0; i < LR_HPREG_N; i++) {
        if (g_lr_hpreg[i].lr == lr) { free_i = i; break; }
        if (free_i < 0 && !g_lr_hpreg[i].lr) free_i = i;
    }
    if (free_i >= 0) {
        g_lr_hpreg[free_i].hp = hp;
        __atomic_store_n(&g_lr_hpreg[free_i].lr, hp ? lr : NULL, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_lr_hpreg_mx);
}
static pixel *const *lr_hpreg_get(const pixel *lr)
{
    for (int i = 0; i < LR_HPREG_N; i++)
        if (__atomic_load_n(&g_lr_hpreg[i].lr, __ATOMIC_ACQUIRE) == lr)
            return g_lr_hpreg[i].hp;
    return NULL;
}

/* dst gets the half-pel planes registered for src (a copy of src's lowres
 * is being taken); left as they were if src has none */
static void lr_copy_hp(yah264_encoder_t *e, pixel *const dst[3], const pixel *src)
{
    if (!dst[0]) return;
    pixel *const *hp = lr_hpreg_get(src);
    if (hp && hp[0])
        for (int k = 0; k < 3; k++) memcpy(dst[k], hp[k], (size_t)e->lr_w * e->lr_h * sizeof(pixel));
}
/* Every site takes the source half-pel planes when ref has them. */
static void build_lr_subpel(pixel *const plane[16], const pixel *ref, int lw, int lh)
{
    IP_SCOPE_B(IP_LA_SUBPEL);
    {
        pixel *const *hp = lr_hpreg_get(ref);
        if (hp && hp[0]) { lr_phases_from_hp(plane, ref, hp, lw, lh); return; }
    }
    (void)lrsub_probe();
    if (lrsub_census()) {
        g_lrs_builds++;
        g_lrs_lw = lw; g_lrs_lh = lh;
        for (int p = 1; p < 16; p++)
            if (!g_lrs_row[p]) g_lrs_row[p] = calloc((size_t)lh, 1);
    }
    double t0 = g_lrs_probe > 0 ? tprof_ms() : 0;
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
            if (!phase || !plane[phase]) continue;
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
#if Y264_HAVE_MC_FUSED
                if (y264_mc_fused_on())
                    y264_bilin4_row_k(o, r0, r1, lw - ax, ax, w00, w10, w01, w11);
                else
#endif
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
 * the matching precomputed phase-plane (build_lr_subpel), a cheap stand-in for
 * full interpolation; quarter precision keeps a subpel pan matching tightly so
 * the propagation fraction stays high and importance accumulates over the chain. */
/* The GPU's exhaustive half-window in LOWRES whole pels. 16 is what our own
 * diamond covers (step 64 qpel = 16 whole pels), so it is the like-for-like
 * setting; 8 is the cheaper operating point (0.53 ms vs 1.52 ms per leg at
 * 720p) and is used because several legs run per frame. */
#define GPU_RANGE 8

/* The whole-pel SAD probes of the two lowres searches (the mb-tree coherent
 * diamond's integer levels, and the lookahead field ME's candidate list,
 * hexagon and square) are scored four at a time through sad_quad before the
 * ordered strict-< comparisons. A probe's SAD does not depend on the running
 * best, so the comparisons see the same numbers in the same order:
 * byte-identical. Unlike SATD, SAD is load-bound and the batch shares the
 * source loads (checkasm: 8x8 x4 5.8 ns against 4 x 2.9 ns singles). The
 * settle lead acts only above one thread, with a lookahead thread; see the
 * wait in compute_mbtree_wholebuf. */
/* Y264_LR_QPEL_AVG=1 (default 0; changes bits): build only the three
 * half-pel lowres planes and form a quarter-pel block where it is read, as the
 * average of its two nearest half-pel samples, instead of building all
 * fifteen bilinear phase planes (nine of them already formed on the fly
 * above ~1080p). Plane building then costs 3/15 of the phases and the
 * per-read cost moves to a 64-pixel average. */
static int lr_qpel_avg_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_LR_QPEL_AVG"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* The lowres sub-pel diamonds score their four points through satd_quad_8x8
 * when all four are in bounds on built planes (byte-identical). Phase B skips
 * deposits onto an accumulator nobody reads afterwards (output unchanged). */
/* The lowres search's INTEGER-pel levels (step >= 4 quarter-pel) are scored
 * with SAD and only the half/quarter-pel levels with SATD (the cheap compare
 * for the integer walk, the full metric for the subpel refinement). Scoring
 * every probe of the seven-level diamond with SATD against the phase planes
 * cost 9.5G SATD pixels on sunflower_1080p. Priced with the pair-leg search's
 * and the previous-frame search's SAD probes as one change; see blk8_inter. */
/* The 8x8 block of one on-the-fly phase at integer position (x,y), into a
 * stride-8 scratch block: the same bilinear the plane builder writes. The
 * builder's edge replication never enters -- every caller keeps the vector
 * inside [0, lw-8] x [0, lh-8] in quarter-pel, so a fractional read stops one
 * column and one row short of the last. */
static inline void lr_qp_block8(pixel *restrict out, const pixel *restrict ref, int rs,
                                int x, int y, int fx, int fy)
{
    /* the row is widened into a small array first: read straight off the
     * source the compiler kept the eight taps scalar */
#if Y264_BIT_DEPTH > 8
    typedef uint32_t qacc_t;
#else
    typedef uint16_t qacc_t;                  /* 16 * 255 + 8 fits */
#endif
    qacc_t w00 = (qacc_t)((4 - fx) * (4 - fy)), w10 = (qacc_t)(fx * (4 - fy));
    qacc_t w01 = (qacc_t)((4 - fx) * fy),       w11 = (qacc_t)(fx * fy);
    const pixel *r0 = ref + y * rs + x;
#if Y264_BIT_DEPTH == 8 && defined(__clang__)
    /* 8-bit input in explicit 16-bit lanes: the sum is at most 16 * 255 + 8,
 * so the arithmetic is exact, and the plain loop below was widened to 32-bit
 * lanes by the vectoriser (twice the multiplies for the same bytes). */
    {
        typedef uint8_t  u8x8  __attribute__((ext_vector_type(8)));
        typedef uint16_t u16x8 __attribute__((ext_vector_type(8)));
        u16x8 v00 = (u16x8)w00, v10 = (u16x8)w10, v01 = (u16x8)w01, v11 = (u16x8)w11;
        for (int r = 0; r < 8; r++, r0 += rs, out += 8) {
            const pixel *r1 = r0 + rs;
            u8x8 a0, a1, b0, b1;
            memcpy(&a0, r0, 8); memcpy(&a1, r0 + 1, 8);
            memcpy(&b0, r1, 8); memcpy(&b1, r1 + 1, 8);
            u16x8 t = v00 * __builtin_convertvector(a0, u16x8)
                    + v10 * __builtin_convertvector(a1, u16x8)
                    + v01 * __builtin_convertvector(b0, u16x8)
                    + v11 * __builtin_convertvector(b1, u16x8) + (u16x8)8;
            u8x8 o = __builtin_convertvector(t >> 4, u8x8);
            memcpy(out, &o, 8);
        }
        return;
    }
#endif
    for (int r = 0; r < 8; r++, r0 += rs, out += 8) {
        const pixel *r1 = r0 + rs;
        qacc_t a[9], b[9];
        for (int c = 0; c < 9; c++) { a[c] = r0[c]; b[c] = r1[c]; }
        /* Truncating the sum to qacc_t BEFORE the shift is what lets the
 * vectoriser keep 8-bit input in 16-bit lanes (the sum is at most
 * 16 * 255 + 8, so the truncation is exact); with the shift applied to the
 * promoted int it widened every product to 32 bits. */
        for (int c = 0; c < 8; c++) {
            qacc_t t = (qacc_t)(w00 * a[c] + w10 * a[c + 1]
                              + w01 * b[c] + w11 * b[c + 1] + 8);
            out[c] = (pixel)(t >> 4);
        }
    }
}
/* Out of line so the plane path of the two readers below stays small
 * enough to inline into the searches. */
/* The two blocks whose rounded average is the quarter-pel block at integer
 * (x,y) + (fx,fy)/4, off the whole-pel plane and the three half-pel planes
 * (subpel[2] H, [8] V, [10] C) by the rule the full-res motion compensation
 * uses (y264_qpel_plane_a/b). Returns 0 when a half-pel plane is missing. */
static inline int lr_qp_pair(const pixel *ref, pixel *const *subpel, int rs,
                             int x, int y, int fx, int fy,
                             const pixel **pa, const pixel **pb)
{
    if (!subpel || !subpel[2] || !subpel[8] || !subpel[10])
        return 0;
    const pixel *pl[4] = { ref, subpel[2], subpel[8], subpel[10] };
    int qi = fy * 4 + fx;
    const pixel *a = pl[y264_qpel_plane_a[qi]] + (size_t)y * rs + x + (fy == 3 ? rs : 0);
    *pa = a;
    *pb = (qi & 5) ? pl[y264_qpel_plane_b[qi]] + (size_t)y * rs + x + (fx == 3 ? 1 : 0) : a;
    return 1;
}
static __attribute__((noinline)) long blk8_qp_onfly(const pixel *sb, int ss,
                                                    const pixel *ref, int rs,
                                                    pixel *const *subpel, int x, int y,
                                                    int fx, int fy, int sad)
{
    pixel blk[64];
    const pixel *qa, *qb;
    if (lr_qpel_avg_on() && lr_qp_pair(ref, subpel, rs, x, y, fx, fy, &qa, &qb))
        /* Y264_LR_QPEL_AVG: a quarter-pel block is the rounded average of
 * the two nearest of {whole, H, V, C} -- the same rule the full-res
 * motion compensation uses -- scored without materialising it. */
        return sad ? y264_dsp.avg_sad8x8(sb, ss, qa, qb, rs)
                   : y264_dsp.avg_satd8x8(sb, ss, qa, qb, rs);
    lr_qp_block8(blk, ref, rs, x, y, fx, fy);
    return sad ? (long)y264_dsp.sad[Y264_PU_8x8](sb, ss, blk, 8) : blk8_satd(sb, ss, blk, 8);
}
static inline long blk8_sad_qp(const pixel *sb, int ss, const pixel *ref, int rs,
                               pixel *const subpel[16], int bx, int by, int qmx, int qmy)
{
    int ix = qmx >> 2, iy = qmy >> 2, fx = qmx & 3, fy = qmy & 3;
    int phase = (fy << 2) | fx;
    if (phase && !subpel[phase])
        return blk8_qp_onfly(sb, ss, ref, rs, subpel, bx + ix, by + iy, fx, fy, 1);
    const pixel *base = phase ? subpel[phase] : ref;
    return y264_dsp.sad[Y264_PU_8x8](sb, ss, base + (by + iy) * rs + bx + ix, rs);
}
static inline long blk8_satd_qp(const pixel *sb, int ss, const pixel *ref, int rs,
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
    if (phase && !subpel[phase])
        return blk8_qp_onfly(sb, ss, ref, rs, subpel, bx + ix, by + iy, fx, fy, 0);
    return blk8_satd(sb, ss, base + (by + iy) * rs + bx + ix, rs);
}

/* Coherent + quarter-pel lowres ME for mb-tree. The SEARCH minimises
 * SATD + lambda*mvbits(mv - predictor) seeded from the predictor (coherent MV
 * field on pans), and refines to QUARTER-pel so a subpel
 * pan matches tightly -- else the residual inflates inter cost, cuts the
 * propagation fraction, and mb-tree importance decays over the chain instead of
 * accumulating (why mb-tree once gained little on motion clips). All MVs in
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
#define CLQ(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
    const long *ctab = coh_tab(mvlambda);                /* TLS resolved once */
    int sadint = !gvalid;                                 /* integer levels by SAD */
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
            /* Batched ring on the SAD levels: score every live candidate
 * through sad_quad BEFORE the ordered strict-< comparisons below. A
 * candidate's SAD does not depend on the running best, so the
 * comparisons see the identical numbers in the identical order. */
            long ring[4];
            int batch = sadnow;
            /* A SAD level with the whole ring inside the bounds: the four
             * candidates sit a whole-pel step from the centre, so they share
             * its quarter-pel phase and read one plane at fixed offsets. If
             * that plane is built, score them in one batched call and price
             * them with the rate terms the centre's components share; the
             * ordered comparisons are the ones below. */
            if (sadnow && step >= 4 &&
                cx - step >= xmin && cx + step <= xmax && cy - step >= ymin && cy + step <= ymax) {
                int phase = ((cy & 3) << 2) | (cx & 3);
                const pixel *base = phase ? subpel[phase] : ref;
                if (base) {
                    const pixel *c0 = base + (by + (cy >> 2)) * rs + bx + (cx >> 2);
                    int o = step >> 2, sc[4];
                    y264_dsp.sad_quad[Y264_PU_8x8](sb, ss, c0 + o, c0 - o, c0 + o * rs, c0 - o * rs, rs, sc);
                    int mbx0 = lowres_mvbits((cx - predx) >> 2), mby0 = lowres_mvbits((cy - predy) >> 2);
                    int rb[4] = { lowres_mvbits((cx + step - predx) >> 2) + mby0,
                                  lowres_mvbits((cx - step - predx) >> 2) + mby0,
                                  mbx0 + lowres_mvbits((cy + step - predy) >> 2),
                                  mbx0 + lowres_mvbits((cy - step - predy) >> 2) };
                    for (int k = 0; k < 4; k++) {
                        if (k == skipk) continue;
                        long c = sc[k] + coh_rate(ctab, mvlambda, rb[k]);
                        if (c < best) { best = c; cx = cand[k][0]; cy = cand[k][1]; moved = 1; acc = k; }
                    }
                    if (!moved) break;
                    skipk = acc ^ 1;
                    continue;
                }
            }
            /* A SATD level with the whole ring inside the bounds and every
             * candidate's plane built: the four reads at computed pointers,
             * scored in one batched call (each the single kernel's value),
             * priced with the shared rate terms, compared as below. */
            if (!sadnow && g_lrs_census <= 0 && y264_dsp.satd_quad_8x8 &&
                cx - step >= xmin && cx + step <= xmax && cy - step >= ymin && cy + step <= ymax) {
                const pixel *rp[4];
                int ok = 1;
                for (int k = 0; k < 4 && ok; k++) {
                    int mx = cand[k][0], my = cand[k][1];
                    int phase = ((my & 3) << 2) | (mx & 3);
                    const pixel *base = phase ? subpel[phase] : ref;
                    if (!base) ok = 0;
                    else rp[k] = base + (by + (my >> 2)) * rs + bx + (mx >> 2);
                }
                if (ok) {
                    int sc[4];
                    y264_dsp.satd_quad_8x8(sb, ss, rp[0], rp[1], rp[2], rp[3], rs, sc);
                    int mbx0 = lowres_mvbits((cx - predx) >> 2), mby0 = lowres_mvbits((cy - predy) >> 2);
                    int rb[4] = { lowres_mvbits((cx + step - predx) >> 2) + mby0,
                                  lowres_mvbits((cx - step - predx) >> 2) + mby0,
                                  mbx0 + lowres_mvbits((cy + step - predy) >> 2),
                                  mbx0 + lowres_mvbits((cy - step - predy) >> 2) };
                    for (int k = 0; k < 4; k++) {
                        if (k == skipk) continue;
                        long c = sc[k] + coh_rate(ctab, mvlambda, rb[k]);
                        if (c < best) { best = c; cx = cand[k][0]; cy = cand[k][1]; moved = 1; acc = k; }
                    }
                    if (!moved) break;
                    skipk = acc ^ 1;
                    continue;
                }
            }
            if (batch) {
                const pixel *rp[4]; int rk[4], nr = 0;
                for (int k = 0; k < 4; k++) {
                    ring[k] = LONG_MAX;
                    if (k == skipk) continue;
                    int mx = cand[k][0], my = cand[k][1];
                    if (mx < xmin || mx > xmax || my < ymin || my > ymax) continue;
                    int ix = mx >> 2, iy = my >> 2, fx = mx & 3, fy = my & 3;
                    int phase = (fy << 2) | fx;
                    const pixel *base = phase ? subpel[phase] : ref;
                    if (phase && !subpel[phase]) {
                        ring[k] = blk8_qp_onfly(sb, ss, ref, rs, subpel, bx + ix, by + iy, fx, fy, 1);
                        continue;
                    }
                    rp[nr] = base + (by + iy) * rs + bx + ix;
                    rk[nr] = k; nr++;
                }
                int j = 0;
                if (nr == 4) {
                    int sc[4];
                    y264_dsp.sad_quad[Y264_PU_8x8](sb, ss, rp[0], rp[1], rp[2], rp[3], rs, sc);
                    for (; j < 4; j++) ring[rk[j]] = sc[j];
                }
                for (; j < nr; j++)
                    ring[rk[j]] = (long)y264_dsp.sad[Y264_PU_8x8](sb, ss, rp[j], rs);
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
                long c = batch ? ring[k] : COH_COST(mx, my);
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
    int nmb, wmb, hmb, lw, lh;
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
    /* A slot's planes are a pool set, taken when the slot is assigned
     * (mbt_sub_plan); a new slot holds none. */
    while (e->mbt_sub_n < n) {
        int i = e->mbt_sub_n;
        for (int p = 0; p < 16; p++) e->mbt_sub[i][p] = NULL;
        e->mbt_sub_obj[i] = -1;
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
/* When a call's legs do not all fit the cache, the slots that were assigned
 * are kept and built, and only the legs left over take the per-worker path.
 * The all-or-nothing form threw the whole call to the private path on
 * overflow: sunflower t12 read 238 of 527 legs without a slot
 * (Y264_LRSUB_CENSUS "planning" line) with the cache never refused, so
 * every private build was an overflow. Same planes either way.
 *
 * The cache also takes the lookahead's phase set on an exact (push, pointer)
 * match instead of building it again (see the pool, struct y264_lrset). */
static int mbt_sub_plan(yah264_encoder_t *e, int ns, const unsigned char *need,
                        const pixel *const *pastlr, const pixel *const *futlr,
                        const long *pastpush, const long *futpush,
                        int *sub0, int *sub1)
{
    int cap = Y264_MBT_SUB_MAX;
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
            /* A known push index names the frame's lowres whichever buffer
 * holds it: the ring entry, the coded anchor's copy (code_panchor_lr) and
 * the walk's own anchor copy all carry the same pixels for one push, and
 * matching on the pointer too built the same anchor's planes up to three
 * times. */
            int bypush = stamp[h] > 0;
            for (int i = 0; i < e->mbt_sub_n; i++)
                if (e->mbt_sub_stamp[i] == stamp[h] &&
                    (bypush || e->mbt_sub_key[i] == want[h])) {
                    *slot[h] = i; e->mbt_sub_use[i] = call; break;
                }
            if (*slot[h] >= 0)
                continue;
            /* Assign: an unallocated slot first, else the least recently used
             * set no source of THIS call has claimed. */
            int pick = -1;
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
                    goto partial;
                }
            }
            /* The slot's set: the lookahead's own, when it built this very
 * set (same push, same pointer), else a pool set to build into. */
            int got = -1, shared = 0;
            pthread_mutex_lock(&e->lrset_mx);
            lrset_release(e, e->mbt_sub_obj[pick]);
            e->mbt_sub_obj[pick] = -1;
            if (stamp[h] > 0)
                for (int o = 0; o < e->lrset_n; o++)
                    if (e->lrset[o].push == stamp[h] && e->lrset[o].key == want[h]) {
                        e->lrset[o].refs++; got = o; shared = 1;
                        break;
                    }
            if (got < 0) got = lrset_acquire(e);
            pthread_mutex_unlock(&e->lrset_mx);
            if (got < 0) {                      /* OOM: the cache steps aside */
                e->mbt_sub_stamp[pick] = -1;
                if (g_lrs_census > 0) g_lrs_plan[10 + 1]++;
                goto none;
            }
            e->mbt_sub_obj[pick] = got;
            for (int p = 0; p < 16; p++) e->mbt_sub[pick][p] = e->lrset[got].pl[p];
            e->mbt_sub_key[pick] = want[h];
            e->mbt_sub_stamp[pick] = stamp[h];
            e->mbt_sub_use[pick] = call;
            if (!shared) fresh[nfresh++] = pick;
            else if (g_lrs_census > 0) g_lrs_plan[15]++;
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
    if (g_lrs_census > 0) { g_lrs_plan[6]++; g_lrs_plan[7] += 1ULL; g_lrs_plan[8] += (unsigned long long)cap; g_lrs_plan[9] += (unsigned long long)e->mbt_sub_n; }
    for (int s = 0; s < ns; s++) sub0[s] = sub1[s] = -1;
    /* Slots assigned this call before the overflow keep their keys but were
     * not built: make sure a later call cannot take them for a hit. */
    for (int i = 0; i < nfresh; i++) e->mbt_sub_stamp[fresh[i]] = -1;
    return 0;
}

/* Worker tid's own phase set h, allocated on its first use. Only that worker
 * reads or writes it. A phase that cannot be allocated stays NULL, which the
 * readers already serve on the fly (blk8_qp_onfly). */
static pixel *const *mbt_ws_sub(yah264_encoder_t *e, int tid, int h)
{
    pixel **sp = e->mbt_subpel[tid][h];
    if (!sp[2] && lr_plane_built(e, 2)) {
        size_t lrsz = (size_t)e->lr_w * e->lr_h;
        for (int p = 1; p < 16; p++)
            sp[p] = lr_plane_built(e, p) ? malloc(lrsz * sizeof(pixel)) : NULL;
    }
    return sp;
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
        /* The worker's own phase sets are the fallback for a leg the shared
 * cache did not cover, which the cache now almost never leaves: they are
 * allocated by the worker the first time it needs one (mbt_ws_sub), not for
 * every worker up front (two 1080p sets are 15.7 MB a worker). */
        for (int p = 0; p < 16; p++)
            e->mbt_subpel[w][0][p] = e->mbt_subpel[w][1][p] = NULL;
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
                long sub_built, sub_hit;
                /* Y264_MBT_MISSWHY: the nobleg bucket split by the FIELD that
 * was missing, so "no pair legs" is attributable. nb_anchor = the ring
 * entry is an anchor (anchors never grow a B pair), nb_untyped = the
 * entry is not typed yet, nb_nulleg = bleg_have is set but a leg
 * pointer is NULL, nb_nohave = typed leaf, legs allocated, flag
 * clear. */
                _Atomic long nb_anchor, nb_untyped, nb_nulleg, nb_nohave;
                } g_mbt_split;
static int mbt_split_env(void);
static int mbt_misswhy_env(void);
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
 * Scaling is the distance-scaling geometry of temporal direct: a leg is proportional to the
 * temporal distance it spans, so leaf->target = leaf->stored * (distance to
 * target) / (distance to stored). Only a target BETWEEN the source and its
 * stored anchor on the same side is derivable -- extrapolating past the anchor
 * would amplify the error rather than shrink it -- and num == den reproduces the
 * stored MV exactly, which is what keeps the already-covered sources identical.
 * Returns 1 if either leg came out usable. */
static int mbt_unsafe_nosettle(void);
static void la_th_wait(yah264_encoder_t *e, long need);
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
            if (mbt_misswhy_env()) {
                _Atomic long *w = ce->is_anchor ? &g_mbt_split.nb_anchor
                                : !ce->typed    ? &g_mbt_split.nb_untyped
                                : ce->bleg_have ? &g_mbt_split.nb_nulleg
                                                : &g_mbt_split.nb_nohave;
                atomic_fetch_add_explicit(w, 1, memory_order_relaxed);
                if (mbt_misswhy_env() > 1)
                    fprintf(stderr, "[mbt_misswhy] poc %d laoff %d settled %d anchor %d"
                            " typed %d have %d legs %d/%d bpoc %d/%d want %d/%d push %ld\n",
                            c->src[s].poc, c->src[s].laoff, c->settled_off,
                            ce->is_anchor, ce->typed, ce->bleg_have,
                            ce->leg[LR_LEG_ANCHOR] != NULL, ce->leg[LR_LEG_NEXT] != NULL,
                            ce->bleg_poc0, ce->bleg_poc1,
                            c->s_pastpoc[s], c->s_futpoc[s], c->src[s].push);
            }
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
/* Plan item E1: on an EXACT-key reuse (the lookahead
 * searched this very pair), take the stored leg's SATD and MV as the leg's
 * cost, plus the walk's own MV rate against its chained predictor, instead
 * of re-evaluating three candidates ({leg MV, chained predictor, zero})
 * with fresh SATDs. The cached-cost model a single-lookahead encoder runs:
 * every motion-search cost computed once, the propagation walk only reads.
 * Drops the rescue the 3-candidate eval gives mispriced legs on chaotic
 * motion, so it is output-changing and band-gated. Not for the pair-scaled
 * seeds (a derived seed still needs its SATD) or the GPU fields (which
 * already carry theirs).
 * MBT_SPLIT on sunflower_1080p t12: the reuse
 * group's evaluation 108 -> 8 ms. Walls, medians of 5 round-robin with a
 * control inside 0.6%: t12 sunflower -1.6%, pedestrian -0.8%, samsung /
 * park_joy / foreman -0.6%, shields 0.0%; t1 -0.2..-1.0%. CRF band, 14
 * clips, 150f, t1: median +0.04% (stq order) / +0.07% (threaded), worst
 * coastguard +0.50 / stockholm +0.63, best station2 -0.52 / coastguard
 * -0.59. */
/* Y264_DIRECT_LRVOTE: lowres simulation of the two B direct derivations, scored
 * against the lookahead's own motion result. Measurement only -- see the Phase B
 * block that reads these. LRVOTE_CZ is the colZeroFlag threshold, in whatever
 * units lowres MV field carries (quarter-pel under the coherent search,
 * integer otherwise), which is why it is not simply the spec's 1. */
static int direct_lrvote_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("Y264_DIRECT_LRVOTE"); v = e ? atoi(e) : 0; }
    return v;
}
#define DIRECT_LRVOTE_CZ 1
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
/* The hexagon's move cap, 16 lowres pels (32 full-res) of travel from the
 * best seed: half the 16-pel full-res search range. Swept on the HD band
 * against 8 as a share of that range: x0.25 (4) was refused, sunflower
 * +0.57%; 6 read median -0.05%, worst +0.31%; x0.75 (12) +0.01%, worst
 * +0.29%; x1 (16) -0.02%, worst +0.14%. A near-inert control in the same run
 * read median +0.06%, worst +0.29%, so 6-16 is one flat region inside the
 * band's null, with instruction counts within 0.2% of each other. 8 sits in
 * the middle of that region, clear of the cliff at 4. The range is fixed at
 * 16 for the hexagon searches that read this field, so the cap is a constant
 * rather than a share of a setting. */
#define LR_HEX_MOVES 8
static inline __attribute__((always_inline))
long lr_me_block(const pixel *sb, int ss, const pixel *ref, int rs,
                        pixel *const subpel[16], int lw, int lh, int bx, int by,
                        int predx, int predy, const int (*cand)[2], int ncand,
                        int stage, int settle, int subgate, int psub,
                        int *outmvx, int *outmvy, long *outsatd);
static int lr_me_stage(void);
/* The Phase A hexagon (changes bits): a Phase A leg with nothing to
 * reuse or seed from (a frontier leaf, or a source the threaded walk reached
 * before its legs settled) is searched with the lookahead's own block search
 * -- whole-pel SAD hexagon and square, then the sub-pel diamonds by SATD --
 * instead of the coherent diamond that starts at a step of 64. Candidates are
 * the same three (the chained left predictor, the block above, zero). The
 * leg's cost keeps its meaning: the SATD at the chosen vector plus the walk's
 * own MV rate against the chained predictor, as the reuse path prices a
 * stored leg. Only the search effort that picks the vector changes. */
/* Default ON at every thread count; 0/1 force either way (-1 is kept as
 * "unset" for the escape's sake). At --threads 8 the CRF band read median
 * +0.06% for 1-2% of the encode's instructions. At --threads 1 it was off
 * until 2026-10-04: a 09-27 read put ducks at +0.54% and the CIF multi-shot
 * at +0.36% for 0.6% of instructions. At --threads 1 a single-thread walk
 * searches every frontier leaf, and on 2160p that search had grown to 11.6 G
 * of a 101 G encode (meridian 120 frames), which the hexagon takes to 3.2 G:
 * -8.8% of the encode's instructions there, -1.2% on sunflower 1080p. Read
 * again at --threads 1 against the shipped default: CRF band median +0.00%,
 * worst foreman_cif +0.19% (ducks -0.08%, touchdown +0.06%); multi-shot
 * cif -0.04%, 720p +0.04%, 1080p +0.02%; HD/4K band (eight 1080p clips,
 * meridian and bbb 2160p, 120 frames) median -0.05%, 2160p -0.24%/-0.17%,
 * worst station2_1080p +0.53%. At one thread the 1080p class also runs the
 * frontier trim (below), and the pair reads +0.40% there. */
static long mbt_pa_hex_leg(yah264_encoder_t *e, const pixel *sb, const pixel *ref,
                           pixel *const *subpel, int lw, int lh, int mx, int my,
                           int pmvx, int pmvy, int ax, int ay, double mvlambda,
                           int *outmvx, int *outmvy)
{
    int cand[2][2] = { { ax, ay }, { 0, 0 } };
    int mvx, mvy; long satd;
    lr_me_block(sb, lw, ref, lw, subpel, lw, lh, mx * 8, my * 8, pmvx, pmvy,
                (const int (*)[2])cand, 2, lr_me_stage(), e->lr_settle, e->lr_subgate,
                1, &mvx, &mvy, &satd);
    *outmvx = mvx; *outmvy = mvy;
    return satd + coh_rate(coh_tab(mvlambda), mvlambda,
                           lowres_mvbits((mvx - pmvx) >> 2) + lowres_mvbits((mvy - pmvy) >> 2));
}

static void mbt_pa_source(void *ctx, int tid, int s)
{
    IP_SCOPE_B(IP_MBT_PA);
    struct mbt_pa_ctx *c = ctx;
    yah264_encoder_t *e = c->e;
    NLED_SITE(Y264_LED_SITE_LRPA);
    if (c->need[s]) NLED(leg_pa, 1 + (c->s_futlr[s] != NULL));
    int nmb = c->nmb, wmb = c->wmb, hmb = c->hmb, lw = c->lw, lh = c->lh;
    double mvlambda = c->mvlambda;
    pixel *const *subpel0 = e->mbt_subpel[tid][0];  /* allocated by mbt_ws_sub when used */
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
    double *pswf = invq_s;          /* this worker's scratch now holds 2^(-aq_off/6) */
    mbtree_psw_k(e, c->src[s].push, c->src[s].full, e->pstride[0], wmb, hmb, pswf, aqoff_s);
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
    if (e->gpq && gpq_consume && c->s_pastpush && c->src[s].push > 0) {
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
    if (pastlr && !q0) {
        int i = c->s_sub0 ? c->s_sub0[s] : -1;
        if (i >= 0)
            subpel0 = e->mbt_sub[i];
        else      { subpel0 = mbt_ws_sub(e, tid, 0); if (g_lrs_census > 0) g_lrs_site[1]++; build_lr_subpel(subpel0, pastlr, lw, lh); }
    }
    if (futlr && !q1) {
        int i = c->s_sub1 ? c->s_sub1[s] : -1;
        if (i >= 0)
            subpel1 = e->mbt_sub[i];
        else      { subpel1 = mbt_ws_sub(e, tid, 1); if (g_lrs_census > 0) g_lrs_site[2]++; build_lr_subpel(subpel1, futlr, lw, lh); }
    }

    if (pa_t0) g_mbt_split.pa_sub_ms += tprof_ms() - pa_tc;
    const y264_lr_blk *bleg0 = NULL, *bleg1 = NULL;
    /* Same settled bound as mbt_pair_seed, and for the same reason. This reader
 * predates A1 and looked safe because it fails closed on a pair that does
 * not match -- but "usually decides the same way" is not the same as
 * ordered, TSan flags the read against lowres_bleg_me's write, and a stored
 * pair that DOES match the walk's brackets makes it decide differently
 * depending on when the flag lands. */
    if (bleg_reuse_on() && c->src[s].laidx >= 0 && !c->src[s].is_anchor &&
        (c->src[s].laoff <= c->settled_off || mbt_unsafe_nosettle())) {
        struct la_entry *ce = &e->la[c->src[s].laidx];
        if (ce->bleg_have && ce->bleg_poc0 == c->s_pastpoc[s] &&
            ce->bleg_poc1 == c->s_futpoc[s] && ce->leg[LR_LEG_ANCHOR] &&
            ce->leg[LR_LEG_NEXT])
        { bleg0 = ce->leg[LR_LEG_ANCHOR]; bleg1 = ce->leg[LR_LEG_NEXT]; }
    }
    /* An ANCHOR source's Phase A
 * search is its field against the previous anchor, which lowres_anchor_me
 * already computed into leg[LR_LEG_ANCHOR] with the previous anchor's POC
 * in aleg_poc0. Same settled bound and the same three-candidate eval as
 * the leaf legs; anchors have no future leg. On sunflower_1080p this was
 * 65 full searches per encode, 118 ms of the 590 ms single-thread Phase A. */
    if (!bleg0 && bleg_reuse_on() && c->src[s].laidx >= 0 &&
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
    if (pair_scale_on() && !bleg0 && !c->src[s].is_anchor) {
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
                                          : blk8_intra_dispatch(sb, lw, mx, my, LR_I10_MODE, e->lr_intra_pen);
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
                } else if (bleg0) {
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
                } else if (!g0)
                    cost0 = mbt_pa_hex_leg(e, sb, pastlr, subpel0, lw, lh, mx, my,
                                              pmvx0, pmvy0, rowmv[mx*4+0], rowmv[mx*4+1],
                                              mvlambda, &mvx0, &mvy0);
                else
                    cost0 = blk8_inter_coh(sb, lw, pastlr, lw, subpel0, lw, lh, mx * 8, my * 8, pmvx0, pmvy0,
                                           rowmv[mx*4+0], rowmv[mx*4+1], mvlambda,
                                           g0 ? g0[i * 2] : 0, g0 ? g0[i * 2 + 1] : 0, g0 != NULL,
                                           &mvx0, &mvy0);
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
                } else if (bleg1) {
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
                } else if (!g1)
                    cost1 = mbt_pa_hex_leg(e, sb, futlr, subpel1, lw, lh, mx, my,
                                              pmvx1, pmvy1, rowmv[mx*4+2], rowmv[mx*4+3],
                                              mvlambda, &mvx1, &mvy1);
                else
                    cost1 = blk8_inter_coh(sb, lw, futlr, lw, subpel1, lw, lh, mx * 8, my * 8, pmvx1, pmvy1,
                                           rowmv[mx*4+2], rowmv[mx*4+3], mvlambda,
                                           g1 ? g1[i * 2] : 0, g1 ? g1[i * 2 + 1] : 0, g1 != NULL,
                                           &mvx1, &mvy1);
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
            psw[i] = pswf[i];
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

/* whole-buffer mb-tree. Replaces
 * the per-anchor propagation with a single backward pass. At anchor F's code
 * time the display-order dependency buffer [prev_anchor, buffered B's, F, future
 * window frames up to the next IDR] is reconstructed; every frame after F (future
 * B's, future anchors) and every buffered B deposits importance onto the anchor(s)
 * it references, chained anchor->anchor and terminating at F. F's accumulated
 * propagate_cost then finishes into its per-MB offset, in double precision
 * throughout. Writes e->mbtree_off
 * and returns 1, or 0 to fall back to the legacy path.
 * The propagation follows the published MB-tree paper (Garrett-Glaser, 2009):
 * propagate amount = (in + intra*w) * (intra - inter)/intra, split between the
 * lists a bipred block uses; finish off = aq - strength*log2((ic+prop)/ic),
 * strength=5*(1-qcomp), NOT centred; w = 2^(-aq_off/6). */
/* Y264_MBT_SPLIT=1: where compute_mbtree's time actually goes, so the question
 * "which part of this can be led?" is answered with a number. Phase A is
 * memoized and therefore prefetchable; Phase B's float accumulation order is
 * the bits, so it is pinned to the anchor. Probe only, printed at close. */
static pthread_mutex_t g_mbt_split_mx = PTHREAD_MUTEX_INITIALIZER;
/* Y264_MBT_BLEG_REUSE: in Phase A, when a leaf source's bleg pair fields
 * (lowres_bleg_me) exist for the SAME bracketing anchors, price the bleg MV
 * (satd at the MV + mv-rate, phase A's exact cost form) instead of running
 * the coherent search again -- one memoized lowres-ME store serves both
 * frame-typing and propagation, where running the search again is two
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

/* Y264_MBT_MISSWHY=1: split MBT_SPLIT's `nobleg` bucket by the field that was
 * missing; =2 adds one line per miss (t1 only -- the lines interleave on the
 * pool). Counting only, no encoder state read that the walk does not already
 * read, so it is inert by construction. */
static int mbt_misswhy_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_MBT_MISSWHY"); v = s ? atoi(s) : 0; }
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
        b->inflight = y264_gpu_lowres_submit(g, GPU_RANGE);
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
 * graph is leaf -> ref B -> anchor, instead of leaves depositing
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
            if (e->blowres_hp[refb_buf][0])
                lr_hp_build(e->blowres_hp[refb_buf], e->bplane[refb_buf][0], e->pstride[0],
                            e->padded_w, e->padded_h, lw, lh);
            refb_anc = na;
            anc[na].poc = e->bpoc[refb_buf]; anc[na].lr = e->blowres[refb_buf];
            anc[na].dintra = NULL; anc[na].full = e->bplane[refb_buf];
            anc[na].push = e->bpush[refb_buf]; na++;
        } else refb_buf = -1;
    }
    double *prop = calloc((size_t)na * nmb, sizeof(double));  /* per-anchor accumulators */
    double *Fw   = malloc(nmb * sizeof(double));              /* F's weight 2^(-aqoff/6) */
    if (!prop || !Fw) { free(prop); free(Fw); return 0; }
    for (int i = 0; i < nmb; i++) Fw[i] = pow(2.0, -(double)aq_fold[i] / 6.0);

    double mvlambda = MBTREE_MVLAMBDA;
    int bipw = 32;                                   /* bipred weight 0.5 (32/64) */
    /* Coherent (predictor-seeded, MV-cost) + quarter-pel + multi-predictor lowres ME:
 * tracks subpel/divergent motion so mb-tree propagation accumulates along the true
 * motion instead of scattering. DEFAULT ON (paired with the CRF operating-point
 * devices below). */
    int depfloor = e->mbt_depfloor;      /* HD parity stage 3, candidate 3 */
    double qcomp = abr_qcomp_env();
    /* the unscaled strength is 5*(1-qcomp)=2.0. yah264's coarser whole-pel lowres ME makes that
 * redistribution too aggressive on motion (BD-measured); 0.7x = 1.4 is the
 * VMAF-NEG optimum across the corpus. */
    /* The 0.7 stands under the absolute anchor too. Restoring the full
 * 5*(1-qcomp) = 2.0 does close the bitrate gap at equal CRF, but it costs
 * BD on 10 of 12 corpus clips (+7.04% ducks, +6.2% bus, +6.15% tempete),
 * so the scale match has to come from the pedestal instead. */
    double strength = 5.0 * (1.0 - qcomp) * 0.7;
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
        src[ns].dintra = e->bdintra_valid[b] ? e->bdintra[b] : NULL;
        src[ns].full = e->bplane[b]; src[ns].bbuf = b;
        src[ns].push = e->bpush[b];
        src[ns].laidx = -1; src[ns].laoff = -1; ns++;
    }
    for (int i = 1; i < ns; i++) {                   /* insertion sort, poc descending */
        struct mbt_source t = src[i]; int j = i - 1;
        while (j >= 0 && src[j].poc < t.poc) { src[j+1] = src[j]; j--; }
        src[j+1] = t;
    }
    /* The frontier trim (on above one thread and at 1080p, changes bits): end propagation at the
 * last typed anchor. A window leaf past every tracked anchor has no future
 * bracket yet, so each walk searched it one-sided against its past anchor
 * and the next walk, once its anchor was typed, searched it again; at 1080p
 * that was ~40% of Phase A's single-thread time. Such leaves are dropped
 * from this walk; they propagate in the later walk that brackets them. This
 * is about WHEN the search runs, not what a lowres cost means. */
    /* Default ON above one thread (keyed on the user's thread request, like
 * the other flip-first trades): at t8 the CRF band reads median -0.12%,
 * worst +0.39%. At --threads 1 it is on for the 1080p class only (at least
 * 1920x1080, under the 3840x1600 size-trade tier). Read at t1 with the Phase A hexagon
 * on, 2026-10-04: CIF band +0.25% median and akiyo_cif +1.26%, so below
 * 1080p stays off; eight 1080p clips median +0.03%, worst station2 +0.40%
 * (the hexagon alone reads +0.53% there), multi-shot 1080p +0.07%, -1.4% of the
 * encode's instructions on sunflower; at 2160p bbb reads +0.60%, so 4K
 * stays off. */
    int frontier = e->param.threads != 1 ||
                   y264_size_class(e->param.width, e->param.height) == 2;
    if (frontier) {
        int maxanc = INT_MIN;
        for (int a = 0; a < na; a++) if (anc[a].poc > maxanc) maxanc = anc[a].poc;
        int k = 0;
        for (int s2 = 0; s2 < ns; s2++)
            if (src[s2].is_anchor || src[s2].bbuf >= 0 || src[s2].poc < maxanc)
                src[k++] = src[s2];
        ns = k;
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

    /* Wait for the lookahead chain through
 * the step that finalizes the whole window before Phase A, so the settled
 * bound can drop the wide pipeline's fail-closed reach. The wait is to a
 * fixed step (pop_seq + la_depth - 1, clamped to what was pushed), so which
 * sources reuse their pair legs stays a function of the launch sequence and
 * never of chain timing. At 12 threads on sunflower_1080p the reach
 * declined 150 of 306 sources into full searches whose legs already existed;
 * the chain is normally far ahead, so the wait itself is cheap. */
    /* The settle lead (on above one thread): wait further, into
 * the lookahead's lead (the la_buf entries pushed beyond the window), so the
 * last la_buf window entries' legs are written too and the walk reuses them
 * instead of searching them fresh -- at 12 threads that was 86 full searches
 * per 240 frames of 1080p that the single-thread walk reuses. The extension
 * is clamped to what the API thread has pushed, which the launch sequence
 * fixes, so which sources reuse stays independent of chain timing. ON at
 * --threads != 1 from 2026-10-04: in the quiet window at default threads the
 * wall read meridian 2160p 35.8 -> 35.5 s, chimera 2160p 24.3 -> 24.3 s,
 * meridian 1080p 9.4 -> 9.0 s, CPU -1.5..-4%; the CRF band at --threads 8
 * read median -0.14% when it was added. */
    int settle_ext = 0;
    int settle_lead = e->param.threads != 1;
    if (e->la_th) {
        long need = e->la_th->pop_seq + e->la_depth - 1;
        if (settle_lead) {
            long more = e->la_th->pushed - need;
            if (more > e->la_buf) more = e->la_buf;
            if (more > 0) { settle_ext = (int)more; need += more; }
        }
        TPROF(TP_LAWAIT, la_th_wait(e, need));
    }
    /* Phase A: the per-source lowres ME (the expensive part) -- parallel over the
 * pool when available (each source is independent, writes its own pa_* slice),
 * else serial. Byte-identical either way: no prop reads/writes here. */
    struct mbt_pa_ctx pac = {
        .e = e, .src = src, .nmb = nmb, .wmb = wmb, .hmb = hmb, .lw = lw, .lh = lh,
        .mvlambda = mvlambda, .cost_inf = COST_INF,
        .s_past = s_past, .s_fut = s_fut, .s_pastlr = s_pastlr, .s_futlr = s_futlr,
        /* Under WIDE chains the walk can run further behind the chain's
 * head than the serial pop cadence assumed, so the settled bound
 * tightens by the wide pipeline's reach -- fail-closed: a declined
 * source searches fresh instead of reusing a pair that may still be
 * mid-write (the TSan pair: mbt_pa_source's bleg reads against
 * la_finalize's leg writes). */
        .settled_off = e->la_th
            ? e->la_depth - 4 - e->bframes + settle_ext
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
    if (e->gpu)
        mbt_gpu_submit(e->gpu, &gb, ns, src, need, s_pastlr, s_futlr, lw, lh);

    int sub0[MAXS], sub1[MAXS], subheld = 0;
    if ((subheld = mbt_sub_claim(e))) {
        { int nb = mbt_sub_plan(e, ns, need, s_pastlr, s_futlr, s_pastpush, s_futpush, sub0, sub1);
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
        g_lrs_plan[1]++;
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
    /* An anchor's accumulator is read once: when its own source deposits
     * (sources run newest first) or, for F and the reference B, by the finish
     * below. A deposit landing after that read reaches nothing -- the usual
     * case is a B's list-1 share onto its future anchor -- so it is not made.
     * The deposits that are read keep their order, so the offsets do not
     * change. */
    int anc_read_at[MAXA];
    for (int a = 0; a < na; a++) anc_read_at[a] = -1;
    for (int s = 0; s < ns; s++) if (s_self[s] >= 0) anc_read_at[s_self[s]] = s;
    anc_read_at[0] = INT_MAX;
    if (refb_anc >= 0) anc_read_at[refb_anc] = INT_MAX;
    for (int s = 0; s < ns; s++) {
        const double *inh = s_self[s] >= 0 ? prop + (size_t)s_self[s] * nmb : NULL;
        double *dst0 = s_past[s] >= 0 && anc_read_at[s_past[s]] > s ? prop + (size_t)s_past[s] * nmb : NULL;
        double *dst1 = s_fut[s]  >= 0 && anc_read_at[s_fut[s]]  > s ? prop + (size_t)s_fut[s]  * nmb : NULL;
        if (!dst0 && !dst1) continue;
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
                /* HD parity stage 3, candidate 3: the deposit floor. The
 * propagation fraction is (intra - inter) / intra, and below depfloor/256 of
 * it this block is telling us it is nearly as dear to predict as to code, so
 * the four bilinear accumulates it is about to pay for carry an amount that
 * cannot survive the finish step's rounding into an 8-bit QP offset. Refusing
 * the deposit outright is cheaper than computing it, and the comment above
 * this loop is the standing warning about what it is: the deposit IS the bits,
 * so this is a quality arm, gated on the band, never a kernel. */
                if (depfloor > 0 &&
                    (intra - inter) * 256 < (long)depfloor * intra) continue;
                double amount = (inh ? inh[i] : 0.0) + (double)intra * psw[i];
                double d = amount * (double)(intra - inter) / (double)intra;
                if ((lu & 1) && dst0) {
                    double a0 = lu == 3 ? d * bipw / 64.0 : d;
                    splat_prop_qp(dst0, wmb, hmb, mx * 32 + pmv[i*4+0], my * 32 + pmv[i*4+1], a0);
                }
                if ((lu & 2) && dst1) {
                    double a1 = lu == 3 ? d * (64 - bipw) / 64.0 : d;
                    splat_prop_qp(dst1, wmb, hmb, mx * 32 + pmv[i*4+2], my * 32 + pmv[i*4+3], a1);
                }
            }
    }
    /* --- Y264_DIRECT_LRVOTE: does the LOOKAHEAD already know which B direct mode
 * this content wants? -------------------------------------------------------
 *
 * The per-clip direct choice is worth up to 23 BD points and the signal that
 * picks it (Y264_DIRECT_SCORE=2, a skippability count) is computed during
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
        int cz = DIRECT_LRVOTE_CZ;
        int qshift = 2;                 /* the lowres MV field is quarter-pel;
 * SATD here is integer-pel */
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
 * derived BI-predictions on pixels is the full-resolution vote's question and has no such
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

    /* finish F (slot 0): the mb-tree finish pass, uncentred. Centring the
 * boost term to the frame mean destroys the akiyo cross-frame benefit; the
 * CRF-reclaim fix belongs in rate control, not here. */
    /* The boost term's frame mean: the pivot the AC gain scales around. It needs
 * a pass before the offsets can be written, so that pass caches the ratios
 * rather than paying nmb log2 twice -- mb-tree sits on the serial floor and
 * this runs per anchor. */
    const double ac_gain = MBT_AC_GAIN;
    double term_mean = 0;
    double *ratio_c = malloc((size_t)nmb * sizeof(double));
    if (ratio_c) {
        for (int i = 0; i < nmb; i++) {
            long li = (long)e->lr_intra[i];
            double intra = li < 1 ? 1.0 : (double)li;
            double ic = intra * Fw[i];
            ratio_c[i] = log2((ic + prop[i]) / ic);
            term_mean += ratio_c[i];
        }
        term_mean = term_mean * strength / nmb;
    }
    double dbg_maxr = 0, dbg_sumr = 0; int omin = 100, omax = -100;
    long osum = 0;                                    /* for the mean mb-tree offset */
    /* MEASUREMENT ONLY: dump the per-MB propagation ratio, the quantity
 * the offset carries as -strength*log2_ratio. */
    double *rq_dbg_ratio = NULL;
    const char *rq_dbg_path = getenv("Y264_MBT_RATIO_DUMP");
    if (rq_dbg_path) rq_dbg_ratio = malloc((size_t)nmb * sizeof(double));
    for (int i = 0; i < nmb; i++) {
        long li = (long)e->lr_intra[i];
        double intra = li < 1 ? 1.0 : (double)li;
        double ic = intra * Fw[i];                    /* intra cost times the AQ weight */
        double ratio = ratio_c ? ratio_c[i]
                               : log2((ic + prop[i]) / ic);   /* prop[0..nmb] = F's accumulator */
        if (rq_dbg_ratio) rq_dbg_ratio[i] = (double)aq_fold[i];   /* AQ term, not ratio */
        double term = strength * ratio;
        if (ratio_c) term = term_mean + (term - term_mean) * ac_gain;
        double offd = (double)aq_fold[i] - term;
        int off = (int)lround(offd);
        if (off < -51) off = -51; else if (off > 51) off = 51;
        rq->out_off[i] = (int8_t)off;
        osum += off;
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
    if (omax - omin > 25) {
        int lo = (int)lround((double)osum / nmb) - 12;
        if (lo < -26) lo = -26;
        if (lo > 0) lo = 0;
        int hi = lo + 25;
        osum = 0; omin = INT_MAX; omax = INT_MIN;
        for (int i = 0; i < nmb; i++) {
            int off = rq->out_off[i];
            if (off < lo) off = lo; else if (off > hi) off = hi;
            rq->out_off[i] = (int8_t)off;
            osum += off;
            if (off < omin) omin = off; if (off > omax) omax = off;
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
 * combined offset and does NOT add aq_off on top;
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
            mbtree_invqscale_k(e, e->bpush[refb_buf], e->bplane[refb_buf], e->pstride[0], wmb, hmb,
                               e->aq_strength, b_invq, b_aq);
            int lim = 51;
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
                        long li = e->bdintra_valid[refb_buf] ? (long)e->bdintra[refb_buf][i]
                                                             : blk8_intra_dispatch(sb, lw, mx, my, LR_I10_MODE, e->lr_intra_pen);
                        double intra = li < 1 ? 1.0 : (double)li;
                        double ic = intra * pow(2.0, -(double)b_aq[i] / 6.0);
                        bratio[i] = log2((ic + rp[i]) / ic);
                        bmean += strength * bratio[i];
                    }
                bmean /= nmb;
                /* THE REFERENCE B IS CENTRED AND ITS ANCHOR IS NOT. F's finish
 * above runs uncentred (since the akiyo result), so the `+ bmean` here does not give
 * the B "the same mean-centring F gets" -- it gives it a
 * systematic +bmean QP, which at strength 2.0 and a mean ratio
 * of ~1.75 is about 3.5 QP of frame-level shift relative to the
 * anchor it is a reference for. Uncentred, the offsets
 * are mean-negative, the frame rate controller absorbs the
 * shift, and that IS the cross-frame bit transfer.
 *
 * It stays: the reference-B field was gated WITH this term
 * measured in it (-2.09% median). */
                double bcen = bmean;
                for (int i = 0; i < nmb; i++) {
                    /* The path pivots on its own mean (the +bmean is a
 * centring), so the gain scales what is left after it. */
                    double bterm = strength * bratio[i];
                    bterm = bmean + (bterm - bmean) * ac_gain;
                    double offd = (double)b_aq[i] - bterm + bcen;
                    int off = (int)lround(offd);
                    if (off < -lim) off = -lim;
                    else if (off > lim) off = lim;
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
static int compute_mbtree(yah264_encoder_t *e, const struct mbt_req *rq)
{
    IP_SCOPE_B(IP_MBTREE);
    pixel *const *anchor_src = rq->anchor_planes;
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs, nmb = wmb * hmb;
    int lw = e->lr_w, lh = e->lr_h;
    const pixel *anchor_lr = rq->anchor_lr;       /* anchor's lowres (post-swap) */
    *rq->out_mean = 0.0;                          /* derived path sets the real value */
    double *prop = calloc(nmb, sizeof(double));
    double *anchor_invq = malloc(nmb * sizeof(double));
    double *bq = malloc(nmb * sizeof(double));
    float  *aq_fold = malloc(nmb * sizeof(float));
    if (!prop || !anchor_invq || !bq || !aq_fold) {
        free(prop); free(anchor_invq); free(bq); free(aq_fold);
        for (int i = 0; i < nmb; i++) rq->out_off[i] = 0;
        return 1;
    }
    /* Anchor's own AQ offset (to fold in) and inv_qscale (to weight its
 * self-importance in the finish ratio), from the SAME full-res 16x16
 * variance aq_analyze uses at encode time. */
    double t_iq = mbt_split_env() ? tprof_ms() : 0;
    mbtree_invqscale_k(e, rq->anchor_push, anchor_src, e->pstride[0], wmb, hmb, e->aq_strength,
                       anchor_invq, aq_fold);
    if (mbt_split_env()) g_mbt_split.invq += tprof_ms() - t_iq;

    /* whole-buffer path: a BD-measured net win over the per-anchor heuristics
 * below, which stay as its fallback (helps motion/detail and 720p, at a small
 * accepted akiyo regression). */
    if (compute_mbtree_wholebuf(e, rq, aq_fold)) {
        memcpy(e->code_panchor_lr, anchor_lr, (size_t)lw * lh * sizeof(pixel));
        lr_copy_hp(e, e->panchor_hp, anchor_lr);
        e->code_panchor_have = 1;
        e->code_panchor_poc = rq->anchor_poc;
        e->code_panchor_push = rq->anchor_push;
        free(prop); free(anchor_invq); free(bq); free(aq_fold);
        return 1;
    }

    if (e->la_depth > 0)
        la_chain_prop(e, prop);

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
    int td = e->poc - e->prev_anchor_poc;
    for (int b = 0; b < e->nbuf; b++) {
        downscale(e->lowres_tmp, lw, lh, e->bplane[b][0], e->pstride[0]);
        /* B's AQ-aware inv_qscale weights its propagated importance. */
        mbtree_invqscale_k(e, e->bpush[b], e->bplane[b], e->pstride[0], wmb, hmb,
                           e->aq_strength, bq, NULL);
        double w1 = 1.0;                                /* list-1 (future) weight */
        if (td > 0) {
            int tb = e->bpoc[b] - e->prev_anchor_poc;
            w1 = (double)tb / (double)td;
            if (w1 < 0) w1 = 0; else if (w1 > 1) w1 = 1;
        }
        for (int my = 0; my < hmb; my++)
            for (int mx = 0; mx < wmb; mx++) {
                const pixel *sb = e->lowres_tmp + (my * 8) * lw + mx * 8;
                int i = my * wmb + mx, mvx = 0, mvy = 0;
                long inter = blk8_inter(sb, lw, anchor_lr, lw, lw, lh, mx * 8, my * 8, &mvx, &mvy);
                long intra = blk8_intra_dispatch(sb, lw, mx, my, LR_I10_MODE, e->lr_intra_pen);
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
    if (e->la_depth > 0) {
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
                mbtree_invqscale_k(e, en->push_idx, en->plane, e->pstride[0], wmb, hmb,
                                   e->aq_strength, bq, NULL);
                for (int my = 0; my < hmb; my++)
                    for (int mx = 0; mx < wmb; mx++) {
                        const pixel *sb = en->lowres + (my * 8) * lw + mx * 8;
                        int i = my * wmb + mx, mvx = 0, mvy = 0;
                        long inter = blk8_inter(sb, lw, anchor_lr, lw, lw, lh,
                                                mx * 8, my * 8, &mvx, &mvy);
                        long intra = blk8_intra_dispatch(sb, lw, mx, my, LR_I10_MODE, e->lr_intra_pen);
                        if (intra <= 0) continue;
                        double pfrac = 1.0 - (double)inter / (double)intra;
                        if (pfrac <= 0) continue;
                        splat_prop(prop, wmb, hmb, mx * 8 + mvx, my * 8 + mvy,
                                   (double)intra * bq[i] * pfrac * w0);
                    }
            }
        }
    }

    /* COMBINED offset: the mb-tree boost is a log ratio whose intra
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
    /* Under CRF/ABR the boost is non-centred (rate controller
 * absorbs the shift). CQP centres only the boost term so it redistributes
 * at roughly constant rate; the folded AQ offset is left intact. For the
 * IPPP path (bframes 0) our CRF controller does not compensate for the
 * mb-tree mean, so an uncentred long-chain boost is a pure uncompensated
 * bit add; centre it so it redistributes at ~constant rate like CQP. */
    int centered = !(e->crf_on || e->abr_on) || e->bframes == 0;
    double mean = centered ? sum / nmb : 0.0;
    double strength = 2.0;                            /* 5*(1-qcomp), qcomp 0.6 */
    { const char *v = getenv("Y264_MBTREE_STRENGTH"); if (v) strength = atof(v); }
    /* (B) CONTENT-ADAPTIVE strength. A flat 2.0 is the unscaled value, but our fixed
 * allocation is already good on static content (akiyo at parity) and short
 * on high-motion/detail (stefan +21%). Modulate the mb-tree redistribution
 * amplitude per-frame by a cheap motion signal already computed for the
 * anchor: R = Sum min(inter,intra) / Sum intra over the frame's lowres MBs
 * (the scene-cut ratio). R is small when the anchor tracks its predecessor
 * cheaply (static/low-motion) and near 1 when prediction is as costly as
 * intra (high motion / fine detail). Lower strength where fixed allocation
 * already wins, raise it where motion needs sharper redistribution:
 * strength *= clamp(aint + aslope*R, lo, hi). The AQ fold is untouched. */
    {
        const double aint = 0.57, aslope = 1.0, lo = 0.5, hi = 1.6;
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
 * bound stays inside int8_t. The B-frame path takes the same bound. */
    int lim = 51;
    for (int i = 0; i < nmb; i++) {
        int off = (int)lround(aq_fold[i] - strength * (prop[i] - mean));
        if (off < -lim) off = -lim; else if (off > lim) off = lim;
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
 * WHY PHASE A CAN BE LED. A whole-anchor walk needs the mini-GOP's buffered B
 * pixels, which are copied at their pops, so it can start at most one encode
 * call early -- ~0.3 ms of driver work, measured, nothing to hide behind. Phase
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
                            long pop_seq, long pushed, int d)
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
    /* d comes from the launch (mbt_warm_launch), not from a scan here. The
 * entries before the next anchor are the ones the driver pops while this
 * warm runs, and with the ring full each freed slot takes the next push:
 * a scan from offset 0 on this thread read `typed` and `is_anchor` of
 * slots the chain was already retyping for new frames (TSan, capped VBR
 * at six threads, on every run). The anchor's own slot is not popped
 * before the join, so everything from d on is still what the launch saw. */
    if (d < 0 || d >= lim)
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
            .mvlambda = MBTREE_MVLAMBDA, .cost_inf = 1L << 40,
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
        if ((subheld = mbt_sub_claim(e))) {
            { int nb = mbt_sub_plan(e, ns, need, s_pastlr, s_futlr, s_pastpush, s_futpush, sub0, sub1);
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
            g_lrs_plan[1]++;
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

/* The mb-tree warm thread: it runs each posted Phase-A warm (mbt_warm_window)
 * and signals completion; mbt_warm_join retires it. */
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
        int wh = mp->warm_head, wn = mp->warm_navail;
        long wps = mp->warm_popseq, wpu = mp->warm_pushed;
        int wd = mp->warm_d;
        pthread_mutex_unlock(&mp->mx);
        double t0 = tprof_ms();
        mbt_warm_window(e, wh, wn, wps, wpu, wd);
        mp->ms_compute += tprof_ms() - t0;
        pthread_mutex_lock(&mp->mx);
        mp->done = 1;
        pthread_cond_broadcast(&mp->cv_done);
    }
    pthread_mutex_unlock(&mp->mx);
}

/* --- mb-tree oracle probe (Y264_MBT_REC / Y264_MBT_PLAY, default OFF) ---
 *
 * PROBE, not a feature: bounds the prize of computing mb-tree off the
 * driver's critical path with unbounded lead (our MBT_PRE launches one call early and the
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
        lr_copy_hp(e, e->panchor_hp, e->lowres_prev);
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
    /* Did this resolve's compute take the derived path (whose panchor update the
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
    if (!mp || !e->mbtree_on || e->mbtree_skip || e->la_depth <= 1)
        return;
    /* Under WIDE-capable shapes the warm's step<->offset wait arithmetic
     * keeps losing to the pipeline: TSan catches it reading an entry the chain
     * types a beat later, and widening the bound twice does not fix it. The
     * warm is pure memo prefill -- disabling it can never change bits (a missed
     * key is a fresh compute) and it is worth close to nothing -- so where
     * width can engage, it stands down instead of racing. */
    if (stair_wide_engaged_cfg(e))
        return;
    /* The next anchor's offset, found HERE, on the thread that owns the pops.
 * Entry at offset i is typed by chain step pop_seq+i+2 and neither written
 * again nor popped before this returns, so an acquire load of the chain's
 * progress licenses each read; the scan stops at the first entry whose step
 * has not landed. */
    int d = -1;
    if (e->la_th) {
        long done = atomic_load_explicit(&e->la_th->done_atom, memory_order_acquire);
        for (int i = 0; i < e->la_n; i++) {
            if (done < e->la_th->pop_seq + i + 2) break;
            const struct la_entry *en = &e->la[(e->la_head + i) % e->la_cap];
            if (!en->typed) break;
            if (en->is_anchor) { d = i; break; }
        }
    }
    pthread_mutex_lock(&mp->mx);
    while (mp->req && !mp->done)                 /* previous warm still running */
        pthread_cond_wait(&mp->cv_done, &mp->mx);
    mp->req = 0; mp->done = 0;
    mp->warm_head   = e->la_head;
    mp->warm_navail = e->la_n;
    mp->warm_popseq = e->la_th ? e->la_th->pop_seq : 0;
    mp->warm_pushed = e->la_th ? e->la_th->pushed : 0;
    mp->warm_d      = d;
    mp->req = 1;
    pthread_cond_signal(&mp->cv_req);
    pthread_mutex_unlock(&mp->mx);
}

/* Retire an in-flight warm before anything touches the memos it writes. */
static void mbt_warm_join(yah264_encoder_t *e)
{
    struct mbt_pre *mp = e->mbtp;
    if (!mp)
        return;
    pthread_mutex_lock(&mp->mx);
    while (mp->req && !mp->done)
        pthread_cond_wait(&mp->cv_done, &mp->mx);
    if (mp->req) { mp->req = 0; mp->done = 0; mp->hits++; }
    pthread_mutex_unlock(&mp->mx);
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
    TPROF_MOVE(TP_MBTREE, TP_MBTWARM, mbt_warm_join(e));   /* a WAIT, not compute */
    struct mbt_req rq = {
        .anchor_planes = anchor_src, .anchor_lr = e->lowres_prev,
        .anchor_poc = e->poc,
        /* The coded anchor's push index keys the walk's per-frame caches
 * (subpel sets, AQ fields); 0 left them unkeyed, so every call rebuilt the
 * current anchor's sets and, through code_panchor_push, the buffered B's
 * past-anchor sets as well. Same planes either way. */
        .anchor_push = e->cur_la_en ? e->cur_la_en->push_idx : 0,
        .head = e->la_head,        .navail = e->la_n,
        .out_off = e->mbtree_off,  .out_mean = &e->mbtree_mean_off,
    };
    compute_mbtree(e, &rq);
    mbt_oracle_record(e);
}

/* Scene-cut decision from lowres cost sums: the P cost is
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

    /* keyint_min 0 = auto, which is keyint/10. Capping it at one second of
 * frames would only bite above a 10-second GOP -- but that includes 24 fps at
 * the default keyint 250, so adopting it is a real output change and wants
 * its own BD round, not a silent ride-along here. The [1, keyint/2+1] clamp
 * applies to explicit and auto alike; on auto it can never bind (keyint/10 is always
 * below keyint/2+1), so it costs today's path nothing. */
    int km = p->keyint_min > 0 ? p->keyint_min : s.keyint / 10;
    int hi = s.keyint / 2 + 1;
    s.keyint_min = km < 1 ? 1 : (km > hi ? hi : km);

    /* Negative = off, 0 = the --scenecut default 40. keyint <= 1 is off too:
 * every frame is an IDR, so no cut can move one. */
    s.thresh_max = (p->scenecut > 0 ? p->scenecut : 40) / 100.0;
    s.off = p->scenecut < 0 || s.keyint <= 1;
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
/* A raw scene cut must also be a
 * JUMP -- its previous-frame-to-intra cost ratio at least SC_JUMP above the
 * largest of the three frames before it. The cut threshold falls as the GOP
 * ages, so on continuous content whose ratio is high or climbing (a tilt, fog,
 * water, a dense crowd) the threshold walks down onto the ratio and places an
 * IDR where nothing changed: coastguard 68 (ratio 0.80 -> 0.86), sintel 85
 * (0.81 -> 0.82), perseverance 79 (0.83 after 0.36/0.83, a duplicated-frame
 * cadence, hence the max over three). Every real cut on the corpus reads
 * 0.93-1.00 after at most 0.77, or (out of content already near intra)
 * halves the remaining headroom. Reads only sums the lookahead already has;
 * the cut pre-scan applies the same test. Against the plain threshold, BD-VMAF-NEG: t1 CRF
 * band coastguard -2.66, sintel -1.78, the other ten byte-identical; HD band
 * perseverance 720p -3.01, 1080p -1.31, riverbed -0.09 (its IDR every 25
 * frames goes), the other nine byte-identical; --threads 8 coastguard -3.46,
 * sintel -1.40, rest identical; multi-shot 0.00 / 0.00 / -0.37 with every
 * shot boundary kept. Instructions within +-0.4%. */
#define SC_JUMP 0.10
/* A raw cut must also read a previous-frame-to-intra ratio of at least
 * SC_FLOOR. Every verified real cut on
 * the corpus, the multi-shot sequences and tier M reads 0.93 or more; the
 * false cuts the falling threshold places on continuous content read 0.62-0.89
 * (park_joy 0.77, sunflower 0.76, chimera 2160p 0.78, coastguard 0.86). The
 * steady near-intra textures above it (riverbed 0.93) are the jump test's.
 * Not when the frame's intra cost is over twice its predecessor's: a scene
 * rising out of black (sintel 285 and 525) reads low against a black frame
 * and is a real cut; continuous content does not double its texture in a
 * frame. Not either with no predecessor to compare (iprev < 0): when in
 * doubt, cut. Against the jump test alone, BD-VMAF-NEG: 300 frames, park_joy -1.16, parkrun
 * -1.42, sunflower -3.66, sintel and samsung byte-identical; chimera 2160p's
 * first 480 frames (the frame-160 false cut) -1.75; multi-shot 0.00 / 0.00 /
 * -0.41, where the false cut had pushed out the 1080p sequence's shot
 * boundary at 150; the 120-frame t1/t8 and HD bands, tier M's windows and
 * cvbr byte-identical or unchanged. */
#define SC_FLOOR 0.90
static int sc_jump_ok(double r, const double *prev, int np, long icur, long iprev)
{
    if (r < SC_FLOOR && iprev >= 0 && 2 * iprev >= icur) return 0;
    const double j = SC_JUMP;
    if (np <= 0) return 1;
    double m = prev[0];
    for (int k = 1; k < np; k++) if (prev[k] > m) m = prev[k];
    /* or the frame's unpredictable share at most half of theirs: a real cut
     * out of content that already read near intra (multi-shot 1080p, water
     * at 0.93 into sky at 0.97) cannot climb far, but its headroom collapses */
    return r - m >= j || 1.0 - m >= 2.0 * (1.0 - r);
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

/* --- anchor-pair lowres ME (the lowres MB cost).
 *
 * The old anchor-leg ME (blk8_inter) started every 8x8 block from (0,0) with a
 * step-halving diamond: no predictor, no motion propagation, no subpel, no MV
 * cost. On divergent/accelerating motion (zoom) the true MV grows frame to
 * frame and varies across the frame, so the from-zero search lands in the
 * wrong basin -- the MV field was noise exactly where the full-res ME needs its
 * lowres seed (the hex-vs-UMH bus deficiency, localized by the oracle test).
 *
 * This one walks the field bottom-up and right-to-left, so every block can
 * be predicted from neighbours already searched (lr_field_pred). A block whose
 * predictor is zero and whose zero-vector residual is near nothing stops
 * there. The rest take a candidate-seeded hexagon walk priced with a motion
 * rate term (lr_me_block), a one-pel square around its winner, and a half- then
 * quarter-pel diamond. Legs with an earlier field also get the colocated
 * vector from it as a candidate, which follows zoom growth from pair to pair.
 *
 * Stages:
 * 0 = legacy from-zero diamond 1 = 0 + colocated temporal seed
 * 2 = the full predictor set + hex/square + mvcost (fpel) 3 = 2 + subpel refine
 * All MVs handled and stored in lowres QUARTER-pel (stages 0-2 store fpel
 * values *4); consumers convert once (full-res qpel = lowres qpel * 2).
 *
 * Default stage: 3 when hex is the active search and a full-res consumer will
 * read the field (y264_me_hex_features: --me hex or the auto medium+fast
 * tiers, or --me hex / the seed env overrides), else 0. The UMH path reads
 * nothing from this field, so it keeps the legacy compute (byte-identical AND
 * wall-clock-identical; the richer field costs ~2.5% single-thread CIF and the
 * lookahead is serial under the wavefront). */
static int lr_me_stage(void)
{
    return y264_me_hex_features() ? 3 : 0;
}

/* Resolve every env-gated lowres/mb-tree lazy static ONCE on the main thread.
 * The CLI opens (and closes) a prime encoder before spawning GOP workers, so
 * this runs first and single-threaded; afterwards the statics are read-only, so
 * the concurrent lowres paths (per-GOP-worker lookahead + the mb-tree Phase-A
 * parallel_for) never write them -> no data race. */


static int s_lrrow_prof = -1;
static long long s_lrrow_ns[3];
/* profile clock for Y264_LRROW_PROF only; it never feeds a decision */
#if defined(__APPLE__)
static long long lrrow_now(void) { return (long long)clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
#else
static long long lrrow_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}
#endif
static void warm_lr_statics(void)
{
    (void)gpq_consume_on();
    (void)lr_me_stage(); (void)adme_log();
    (void)psy_flat_log();
    
    /* Read by Phase A on pool workers, so they must resolve here. The wide
     * config changes WHICH thread first-touches several of these; each one
     * missing from this list is a TSan report. */
    (void)bleg_reuse_on(); (void)pair_scale_on();
    (void)direct_lrvote_on();   /* read in Phase B */
    (void)mbt_unsafe_nosettle();
    (void)lrsub_census();
    (void)stair_stat_on();
    (void)mbt_split_env();
    (void)mbt_misswhy_env();         /* read on the pool beside mbt_split_env */
    (void)stair_bdepth_on(); (void)la_buf_env();
    (void)stair_wide_on();
    (void)stair_unsafe_no_refbwait();
    (void)stair_unsafe_no_rowgate(); (void)stair_unsafe_gate_arow();
    (void)stair_unsafe_no_prevpwait();
    (void)stq_env();
    if (s_lrrow_prof < 0) { const char *q = getenv("Y264_LRROW_PROF"); s_lrrow_prof = q ? atoi(q) : 0; }
    (void)abr_qcomp_env(); (void)rcp_dbg_on(); (void)rb_walk_env();
    (void)mbt_bref_probe();
    (void)rcp_warm_n(); (void)rcp_gain(); (void)rcp_lag_env(); (void)hw_env(); (void)rcp_qpd_env();
    (void)abr_rf_env(); (void)abr_rfqp_trace(); (void)abr_vbvov_frac();
    (void)czw_stat_on();
    (void)direct_permb_env(); (void)y264_direct_auto_env();
    (void)vbv_rhi_env(); (void)vbv_force_env();
    (void)vbv_stat_on(); (void)vbv_qpd_env(); (void)vbv_cjump_env();
    /* vbv_bound_env is read by encoder_open, and cli/yah264_cli.c opens one
     * encoder PER GOP from concurrent workers, so open is not a
     * single-threaded context. Primed here on the main thread before any
     * worker. */
    (void)vbv_bound_env();
    (void)vbv_abound_env();
    (void)vbv_cw_env();
    (void)rc_share_ovn_env();
    (void)vbv_fitlaw_env();
    (void)vbv_segh_env();
    
    (void)crf_cplx_env(); (void)crf_pbscale_env();
    (void)crf_aqabs_env(); (void)crf_pb0_env();
    (void)unsafe_no_nal();
    /* Resolved inline inside worker-called functions until 09-26, so no warm
 * could reach them: hoisted into accessors and warmed here. */
    (void)qp_trace_on();
    (void)stair_tdir_on(); (void)wp_stat_on();
    /* 08-29 TSan sweep of the GOP-parallel repro: the three lazy env caches
 * it still flagged. All benign same-value init races, warmed here so the
 * NEXT TSan hunt is not reading four reports to find the real one. wf_capk
 * caches only its env; the dims are dummies. */
    (void)fqp_trace_on(); (void)ident_stat_env(); (void)wf_capk(64, 64);
    /* 08-29 sweep, second pass: the rest of this TU's env gates. Audited
 * mechanically (scripts/env_gate_audit.py) rather than by eye -- the tail
 * is long enough that reading for it is how four of them survived the
 * first pass. */
    (void)tprof_on();
    (void)hpel_probe_on();
    (void)mbt_unsafe_nombt(); (void)lrsub_probe(); (void)lr_qpel_avg_on();
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


/* The predictor of block (mx, my) in a field searched in reverse raster order.
 * Its already-searched neighbours are the one to the right and the three on
 * the row below; they go into mvc[] in that order (right, below, below-left,
 * below-right) as search candidates, and the predictor is the componentwise
 * median of the first three present, missing ones counting as zero (one
 * present: that one). Returns how many went in.
 *
 * Swept on the HD band against three other sets: right/below/below-right
 * (median +0.02%, worst +0.70%), the same three with below-left kept as a
 * candidate but out of the median (-0.03%, +0.52%), and right/below alone
 * (+0.12%, +0.72%). None beat this one. A temporal candidate is already in the
 * legs that have a live earlier field (the colocated vector the callers pass);
 * the one leg without it, the previous-frame field, is off by default. */
static int lr_field_pred(const y264_lr_blk *f, int wmb, int hmb, int mx, int my,
                         int mvc[4][2], int *px, int *py)
{
    const int i = my * wmb + mx;
    const int right = mx + 1 < wmb, below = my + 1 < hmb;
    int nb[4], n = 0;
    if (right) nb[n++] = i + 1;
    if (below) {
        nb[n++] = i + wmb;
        if (mx > 0) nb[n++] = i + wmb - 1;
        if (right) nb[n++] = i + wmb + 1;
    }
    for (int k = 0; k < n; k++) { mvc[k][0] = f[nb[k]].mvx; mvc[k][1] = f[nb[k]].mvy; }
    if (n < 2) {
        *px = n ? mvc[0][0] : 0;
        *py = n ? mvc[0][1] : 0;
    } else {
        int x2 = n > 2 ? mvc[2][0] : 0, y2 = n > 2 ? mvc[2][1] : 0;
        *px = median3(mvc[0][0], mvc[1][0], x2);
        *py = median3(mvc[0][1], mvc[1][1], y2);
    }
    return n;
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

/* One block of the stage-2/3 search. All MVs lowres qpel; the predictor
 * (predx,predy) prices the motion rate as the exp-Golomb length of the
 * difference, one doubled-SATD unit per bit. That weight was swept on the HD
 * band at 0.5 (median -0.01%, worst +0.54%) and 2 (median -0.09%, worst
 * +0.65%): flat in the median and worse at the worst clip either way;
 * cand[] are raw qpel candidates (spatial mvc + colocated + zero), each
 * fpel-rounded before probing, as any predictor-seeded search must be.
 * Returns the best TOTAL cost (satd + mvcost) and the pure SATD via *outsatd. */
/* LOWRES SEARCH SHAPE. 0 = the shipped coherent quarter-pel SATD search;
 * 1 = whole-pel SAD during the search, SATD only at the end.
 *
 * the local measurement records (08-14) measured our lookahead at
 * 13.7-13.8% of single-thread CPU on the 720p board clips
 * -- roughly 11-12% of t1 wall in ONE subsystem, and it is more work
 * rather than the same work done slower (instructions retired confirmed it is
 * volume, not CPI). The volume is per-probe metric: every candidate here calls
 * blk8_satd_qp, which is a quarter-pel SATD, where shape 1 probes whole-pel SAD
 * and pays SATD once. The whole-pel search was already measured and recovers at
 * most 1.7% -- it prices the search, not the shape, so it does not close this.
 *
 * MEASURED 08-26 late, AND IT DOES NOT PAY. Instruction counts (time -l),
 * 120 frames, CRF 25, arm verified live (md5s differ, sizes +0.04-0.08%):
 * foreman -0.5% t1 / -0.9% t8, samsung -1.1% / -1.3%, bbb -0.9% / -0.8%, with
 * wall flat (+0.0% to -0.8%). About 1%, not the 11-12% the audit projected.
 *
 * The reason is the counterfactual the audit never ran. The stage-0 field
 * (no stage 3 at all), and instructions go UP: samsung +4.8% t1 /
 * +3.8% t8, bbb +3.6% t1, pure-C samsung +5.3% t1. The lowres field already
 * pays for itself -- it seeds the full-res search, and a worse field costs
 * more downstream than the field costs to build. So this subsystem is not a
 * cost centre to be drained; cheapening the probe metric collects ~1% and
 * cheapening it harder risks paying for it twice in full-res ME.
 *
 * That was the legacy clips. On the low-rate HD cells that carry the board's
 * worst-clip leg the pair-leg search is 3.5G of 18.1G SATD pixels, and the
 * cheap-probe shape takes it to 1.2G. Priced as one change with the
 * previous-frame search's SAD probes and Phase A's integer levels; the band
 * and wall are recorded at blk8_inter. */

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

/* HD parity stage 3, candidates 1 and 2. Both are thresholds on the lowres
 * block's own residual, in units of SAD (settle) or SATD (subgate) PER LOWRES
 * PIXEL, and 0 turns each off. This search is the inner loop of the lookahead's
 * own field ME and of mb-tree's Phase A alike, which is why it is the one place
 * both halves of the lookahead's fixed cost can be reached at once.
 *
 * The governing constraint, measured before either was written: taking the
 * lowres field down to the legacy diamond (stage 0) reads +9.4% of
 * instructions at MATCHED RATE on both cells, because a worse field costs more
 * downstream than the search saves. So a threshold here has to buy its
 * reduction out of blocks whose vector was never in doubt, and the band is what
 * says whether it did. */
/* Whole-pel SADs for n probe points (qpel units, already whole-pel and in
 * bounds), four at a time through sad_quad. Exactly blk8_sad_fpel per point. */
static inline __attribute__((always_inline))
void lr_sad_fpel_n(const pixel *sb, int ss, const pixel *ref, int rs,
                   int bx, int by, const int (*pt)[2], int n, long *out)
{
    ref += (size_t)by * rs + bx;            /* the block's origin, once */
    bx = by = 0;
    int k = 0;
    for (; k + 4 <= n; k += 4) {
        const pixel *r[4];
        for (int j = 0; j < 4; j++)
            r[j] = ref + (by + (pt[k + j][1] >> 2)) * rs + bx + (pt[k + j][0] >> 2);
        int sc[4];
        y264_dsp.sad_quad[Y264_PU_8x8](sb, ss, r[0], r[1], r[2], r[3], rs, sc);
        for (int j = 0; j < 4; j++) out[k + j] = sc[j];
    }
    const pixel *r[3];
    for (int j = 0; k + j < n && j < 3; j++)
        r[j] = ref + (by + (pt[k + j][1] >> 2)) * rs + bx + (pt[k + j][0] >> 2);
    int sc[3];
    switch (n - k) {                /* the hexagon's 3-point steps land here */
    case 3:
        if (y264_dsp.sad_trio[Y264_PU_8x8]) {
            y264_dsp.sad_trio[Y264_PU_8x8](sb, ss, r[0], r[1], r[2], rs, sc);
            out[k] = sc[0]; out[k + 1] = sc[1]; out[k + 2] = sc[2];
            return;
        }
        break;
    case 2:
        if (y264_dsp.sad_pair[Y264_PU_8x8]) {
            y264_dsp.sad_pair[Y264_PU_8x8](sb, ss, r[0], r[1], rs, sc);
            out[k] = sc[0]; out[k + 1] = sc[1];
            return;
        }
        break;
    }
    for (; k < n; k++)
        out[k] = blk8_sad_fpel(sb, ss, ref, rs, bx, by, pt[k][0], pt[k][1]);
}

static inline __attribute__((always_inline))
long lr_me_block(const pixel *sb, int ss, const pixel *ref, int rs,
                        pixel *const subpel[16], int lw, int lh, int bx, int by,
                        int predx, int predy, const int (*cand)[2], int ncand,
                        int stage, int settle, int subgate, int psub,
                        int *outmvx, int *outmvy, long *outsatd)
{
    int xmin = -4 * bx, xmax = 4 * (lw - 8 - bx);
    int ymin = -4 * by, ymax = 4 * (lh - 8 - by);
#define LRCL(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define LRFPEL(v) ((((v) + 2) >> 2) * 4)   /* *4 not <<2: v can be negative (UB) */
#define LRMVCOST(mx, my) (lowres_mvbits((mx) - predx) + lowres_mvbits((my) - predy))
    /* Cheap-probe shape: probe whole-pel SAD, pay SATD once at the end. The MV-rate
 * term is halved for the SAD stage because this tree's SATD is deliberately
 * on a 2x scale (documented in src/dsp/, verified over 300k blocks) while
 * SAD is 1x, so an unscaled lambda would over-price motion by about two
 * against a SAD distortion and bias the search toward the predictor. The
 * factor is the domain ratio, not a tuned constant; the BD round is what
 * judges it. */
    long bsatd, best;
    int cx = LRCL(LRFPEL(predx), xmin, xmax), cy = LRCL(LRFPEL(predy), ymin, ymax);
#define LRPROBE(qx, qy) blk8_sad_fpel(sb, ss, ref, rs, bx, by, (qx), (qy))
#define LRCOST(qx, qy) (LRMVCOST((qx), (qy)) >> 1)
    /* With the settled-block exit off, the start probe needs nothing before
 * the candidates do, so it rides in their batch (scored first, below). */
    int ctr_in_batch = settle <= 0;
    if (!ctr_in_batch) {
        bsatd = LRPROBE(cx, cy);
        best = bsatd + LRCOST(cx, cy);
    } else { bsatd = 0; best = 0; }
    /* Candidate 1, the settled-block exit. The neighbours and the previous
 * field already agree on this vector and it leaves under `settle` of residual
 * per lowres pixel, so the candidate list, the hexagon, the square refine and
 * the subpel diamonds are all bought for a block whose answer is in hand. One
 * SATD still has to be paid to leave the returned cost in the domain the walk
 * reads, which is why the exit is worth having at all only well below the
 * probe's own noise. */
    if (settle > 0 &&
        bsatd < (long)settle * 64 * (1 << (Y264_BIT_DEPTH - 8))) {
        bsatd = blk8_satd_qp(sb, ss, ref, rs, subpel, bx, by, cx, cy);
        best = bsatd + LRMVCOST(cx, cy);
        *outmvx = cx; *outmvy = cy; *outsatd = bsatd;
        return best;
    }
    /* Candidate dedup vs EVERY probed start, not just a duplicate of the
 * current best: an exact duplicate returns the identical
 * (s, c), which cannot pass the strict-< acceptance -- skipping it changes
 * neither the winner nor a tie-break. Spatial/colocated/zero candidates
 * frequently coincide after fpel rounding + clamping. */
    int pts[10][2] = { { cx, cy } }, npts = 1;   /* 1 + ncand <= 10 */
    {
        /* The probes gathered first and scored in fours; the strict-< walk
 * below then runs over them in the original order. */
        for (int k = 0; k < ncand; k++) {
            int tx = LRCL(LRFPEL(cand[k][0]), xmin, xmax);
            int ty = LRCL(LRFPEL(cand[k][1]), ymin, ymax);
            int dup = 0;
            for (int q = 0; q < npts; q++)
                if (pts[q][0] == tx && pts[q][1] == ty) { dup = 1; break; }
            if (dup) continue;
            if (npts < 10) { pts[npts][0] = tx; pts[npts][1] = ty; npts++; }
        }
        long sv[10];
        if (ctr_in_batch) {
            lr_sad_fpel_n(sb, ss, ref, rs, bx, by, (const int (*)[2])pts, npts, sv);
            bsatd = sv[0]; best = bsatd + LRCOST(cx, cy);
            for (int q = 1; q < npts; q++) {
                long c = sv[q] + LRCOST(pts[q][0], pts[q][1]);
                if (c < best) { best = c; bsatd = sv[q]; cx = pts[q][0]; cy = pts[q][1]; }
            }
        } else {
            lr_sad_fpel_n(sb, ss, ref, rs, bx, by, (const int (*)[2])&pts[1], npts - 1, sv);
            for (int q = 1; q < npts; q++) {
                long c = sv[q - 1] + LRCOST(pts[q][0], pts[q][1]);
                if (c < best) { best = c; bsatd = sv[q - 1]; cx = pts[q][0]; cy = pts[q][1]; }
            }
        }
    }
    /* hexagon walk: six whole-pel points two pels out, re-centred on the best
 * until it stops moving or has moved LR_HEX_MOVES times, then a one-pel
 * 8-point square. */
    static const int hexp[6][2] = { {-8,0}, {-4,-8}, {4,-8}, {8,0}, {4,8}, {-4,8} };
    /* Direction-tracked dedup (the same hexagon-revisit shape, same proof as the fullres hex
 * in motion.c): after a move along hexp[d] only vertices {d-1, d, d+1} are new;
 * the rest were evaluated on an earlier iteration, and evaluated costs are
 * >= best under strict-< acceptance -- exact re-probe removal only. */
    int hdir = -1;
    /* the ring's vertices as pointer offsets from its centre, and per arrival
     * direction the new ones in ascending order (all six on the first step) */
    static const int8_t hex_new[7][6] = {
        { 0, 1, 2, 3, 4, 5 },
        { 0, 1, 5, -1 }, { 0, 1, 2, -1 }, { 1, 2, 3, -1 },
        { 2, 3, 4, -1 }, { 3, 4, 5, -1 }, { 0, 4, 5, -1 },
    };
    const int hfast = y264_dsp.sad_trio[Y264_PU_8x8] && y264_dsp.sad_pair[Y264_PU_8x8];
    const pixel *rorg = ref + (size_t)by * rs + bx;
    for (int iter = 0; iter < LR_HEX_MOVES; iter++) {
        int nx = cx, ny = cy; long ns = bsatd, nb = best;
        int nd = hdir;
        /* The whole ring inside the plane: every vertex is a read at a fixed
         * offset from the centre, scored in one or two batched calls and
         * compared in the order the general gather below would visit them. */
        if (hfast && cx - 8 >= xmin && cx + 8 <= xmax && cy - 8 >= ymin && cy + 8 <= ymax) {
            const int8_t *hv = hex_new[hdir + 1];
            int nv = hdir < 0 ? 6 : 3;
            const pixel *c0 = rorg + (cy >> 2) * rs + (cx >> 2);
            const pixel *rp[6];
            for (int q = 0; q < nv; q++)
                rp[q] = c0 + (hexp[hv[q]][1] >> 2) * rs + (hexp[hv[q]][0] >> 2);
            int sc[6];
            if (nv == 6) {
                y264_dsp.sad_quad[Y264_PU_8x8](sb, ss, rp[0], rp[1], rp[2], rp[3], rs, sc);
                y264_dsp.sad_pair[Y264_PU_8x8](sb, ss, rp[4], rp[5], rs, sc + 4);
            } else
                y264_dsp.sad_trio[Y264_PU_8x8](sb, ss, rp[0], rp[1], rp[2], rs, sc);
            for (int q = 0; q < nv; q++) {
                int k = hv[q];
                int tx = cx + hexp[k][0], ty = cy + hexp[k][1];
                long c = sc[q] + LRCOST(tx, ty);
                if (c < nb) { nb = c; ns = sc[q]; nx = tx; ny = ty; nd = k; }
            }
            if (nx == cx && ny == cy) break;
            cx = nx; cy = ny; best = nb; bsatd = ns; hdir = nd;
            continue;
        }
        int hp[6][2], hk[6], nh = 0;
        for (int k = 0; k < 6; k++) {
            if (hdir >= 0) {
                int rel = (k - hdir + 6) % 6;
                if (rel != 0 && rel != 1 && rel != 5) continue;
            }
            int tx = cx + hexp[k][0], ty = cy + hexp[k][1];
            if (tx < xmin || tx > xmax || ty < ymin || ty > ymax) continue;
            hp[nh][0] = tx; hp[nh][1] = ty; hk[nh] = k; nh++;
        }
        long hs[6];
        lr_sad_fpel_n(sb, ss, ref, rs, bx, by, (const int (*)[2])hp, nh, hs);
        for (int q = 0; q < nh; q++) {
            int tx = hp[q][0], ty = hp[q][1];
            long s = hs[q];
            long c = s + LRCOST(tx, ty);
            if (c < nb) { nb = c; ns = s; nx = tx; ny = ty; nd = hk[q]; }
        }
        if (nx == cx && ny == cy) break;
        cx = nx; cy = ny; best = nb; bsatd = ns; hdir = nd;
    }
    static const int sqp[8][2] = { {-4,-4}, {0,-4}, {4,-4}, {-4,0},
                                   {4,0}, {-4,4}, {0,4}, {4,4} };
    { int nx = cx, ny = cy; long ns = bsatd, nb = best;
      int sp[8][2], nsq = 0;
      if (cx - 4 >= xmin && cx + 4 <= xmax && cy - 4 >= ymin && cy + 4 <= ymax) {
          /* all eight inside: two batched calls at fixed offsets, the same
           * ordered comparisons */
          const pixel *c0 = rorg + (cy >> 2) * rs + (cx >> 2);
          int sc[8];
          y264_dsp.sad_quad[Y264_PU_8x8](sb, ss, c0 - rs - 1, c0 - rs, c0 - rs + 1, c0 - 1, rs, sc);
          y264_dsp.sad_quad[Y264_PU_8x8](sb, ss, c0 + 1, c0 + rs - 1, c0 + rs, c0 + rs + 1, rs, sc + 4);
          for (int k = 0; k < 8; k++) {
              int tx = cx + sqp[k][0], ty = cy + sqp[k][1];
              long c = sc[k] + LRCOST(tx, ty);
              if (c < nb) { nb = c; ns = sc[k]; nx = tx; ny = ty; }
          }
          nsq = -1;
      }
      for (int k = 0; nsq >= 0 && k < 8; k++) {
          int tx = cx + sqp[k][0], ty = cy + sqp[k][1];
          if (tx < xmin || tx > xmax || ty < ymin || ty > ymax) continue;
          sp[nsq][0] = tx; sp[nsq][1] = ty; nsq++;
      }
      long ss8[8];
      if (nsq < 0) nsq = 0;
      else lr_sad_fpel_n(sb, ss, ref, rs, bx, by, (const int (*)[2])sp, nsq, ss8);
      for (int q = 0; q < nsq; q++) {
          int tx = sp[q][0], ty = sp[q][1];
          long s = ss8[q];
          long c = s + LRCOST(tx, ty);
          if (c < nb) { nb = c; ns = s; nx = tx; ny = ty; }
      }
      cx = nx; cy = ny; best = nb; bsatd = ns; }
    /* Back into the SATD domain before anything downstream reads this. The
 * whole-pel stage above left bsatd holding a SAD, and the returned value is
 * the block's INTER COST -- it feeds mb-tree's propagation fraction and the
 * B seeds, which are calibrated in SATD. Re-score once at the winner and
 * restore the full MV-rate term; the subpel refine below then runs in the
 * same domain it always did. This single call is the "satd only at the
 * end" step, and it is what makes the cheap search safe rather than merely fast. */
    bsatd = blk8_satd_qp(sb, ss, ref, rs, subpel, bx, by, cx, cy);
    best = bsatd + LRMVCOST(cx, cy);
    /* Candidate 2, the subpel gate. The whole-pel winner is already under
 * `subgate` of SATD per lowres pixel, so a quarter of a lowres pixel -- half a
 * full-resolution one -- is below what the propagation fraction and the B seed
 * can resolve. Eight more interpolated SATDs buy a vector nobody downstream
 * can tell from this one. Separate from the settle exit because it fires on
 * the SEARCHED winner rather than on the predictor, so a block that moved can
 * still take it. */
    /* psub: the predictor at its own quarter-pel precision as a rival to the
     * whole-pel winner, before the subpel refine. The candidates above are rounded to whole pels and one
     * half- plus one quarter-pel diamond step cannot reach every quarter
     * position, so on coherent small motion -- the distance-one leg -- the
     * neighbours' exact vector is often the better start. Without it the
     * distance-one P costs read 3-8% high against the longer ones on Cosmos. */
    if (psub && stage >= 3) {
        int tx = LRCL(predx, xmin, xmax), ty = LRCL(predy, ymin, ymax);
        if (tx != cx || ty != cy) {
            long s2 = blk8_satd_qp(sb, ss, ref, rs, subpel, bx, by, tx, ty);
            long c2 = s2 + LRMVCOST(tx, ty);
            if (c2 < best) { best = c2; bsatd = s2; cx = tx; cy = ty; }
        }
    }
    if (stage >= 3 && subgate > 0 &&
        bsatd < (long)subgate * 64 * (1 << (Y264_BIT_DEPTH - 8))) {
        *outmvx = cx; *outmvy = cy; *outsatd = bsatd;
        return best;
    }
    if (stage >= 3) {
        /* Subpel: one 4-point diamond at half-pel, then one at quarter-pel.
         * Swept on the HD band: half-pel alone was refused (shields +1.35%)
         * for -0.5% to -2.2% of instructions; a second quarter-pel step read
         * median +0.00% for +1.7% to +2.6% of instructions. */
        for (int step = 2; step >= 1; step--) {
            int nx = cx, ny = cy; long ns = bsatd, nb = best;
            int dia[4][2] = { {step, 0}, {-step, 0}, {0, step}, {0, -step} };
            /* All four diamond points in bounds and on built planes: one
 * satd_quad call instead of four dispatched singles. The scores are
 * satd8x8's exactly, compared below in the same order. */
            long sv[4]; int have = 0;
            if (g_lrs_census <= 0 && y264_dsp.satd_quad_8x8) {
                const pixel *rp[4];
                for (have = 0; have < 4; have++) {
                    int tx = cx + dia[have][0], ty = cy + dia[have][1];
                    if (tx < xmin || tx > xmax || ty < ymin || ty > ymax) break;
                    int ph = ((ty & 3) << 2) | (tx & 3);
                    const pixel *base = ph ? subpel[ph] : ref;
                    if (!base) break;
                    rp[have] = base + (by + (ty >> 2)) * rs + bx + (tx >> 2);
                }
                if (have == 4) {
                    int sc[4];
                    y264_dsp.satd_quad_8x8(sb, ss, rp[0], rp[1], rp[2], rp[3], rs, sc);
                    for (int k = 0; k < 4; k++) sv[k] = sc[k];
                }
            }
            for (int k = 0; k < 4; k++) {
                int tx = cx + dia[k][0], ty = cy + dia[k][1];
                if (tx < xmin || tx > xmax || ty < ymin || ty > ymax) continue;
                long s = have == 4 ? sv[k] : blk8_satd_qp(sb, ss, ref, rs, subpel, bx, by, tx, ty);
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

/* One lowres MV field, cur vs ref (stages >= 2): reverse-order
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
    int settle, subgate;        /* HD parity stage 3, candidates 1 and 2 */
    int psub;                   /* subpel predictor rival (lr_me_block) */
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
                               lr_me_stage(), e->lr_settle, e->lr_subgate, 1 };
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
 * built from its right neighbour and the three below it (lr_field_pred),
 * so this is a wavefront, not a parallel-for -- but a wavefront with
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
    int pooled = lowres_field_me_prep(e, cur, d_intra, ref, subpel, colx, coly,
                                      colsf256, colsub, mvlambda, price, leg,
                                      &fc, &sp);
    if (pooled) {
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
    IP_SCOPE_B(IP_LA_FME);
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
            int mvc[4][2], predx, predy;
            int nmvc = lr_field_pred(leg, wmb, hmb, mx, my, mvc, &predx, &predy);
            /* Fast skip: near-zero residual on a zero predictor. */
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
                        fc->settle, fc->subgate, fc->psub,
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
/* Point the lookahead's set g at a pool set nobody else references before a
 * build writes it, and take it out of the published table while it is being
 * written. A set the cache also holds stays with the cache as it is. */
static void lr_sub_fresh(yah264_encoder_t *e, int g)
{
    int o = e->lr_sub_obj[g];
    if (!e->lrset_init || o < 0)
        return;                                 /* privately owned (pool short at open) */
    pthread_mutex_lock(&e->lrset_mx);
    if (e->lrset[o].push >= 0 || e->lrset[o].refs > 1) {
        /* published or held: it stays as it is, the build goes elsewhere */
        int n = lrset_acquire(e);
        lrset_release(e, o);
        o = n;
    }
    pthread_mutex_unlock(&e->lrset_mx);
    e->lr_sub_obj[g] = o;
    for (int p = 1; p < 16; p++)                /* o < 0: OOM, a private set */
        e->lr_subpel[g][p] = o >= 0 ? e->lrset[o].pl[p]
            : lr_plane_built(e, p) ? malloc((size_t)e->lr_w * e->lr_h * sizeof(pixel)) : NULL;
}
/* After the lookahead built set g from the ring lowres `key` of push `push`. */
static void lr_sub_publish(yah264_encoder_t *e, int g, long push, const pixel *key)
{
    int o = e->lr_sub_obj[g];
    if (!e->lrset_init || o < 0 || push <= 0)
        return;
    pthread_mutex_lock(&e->lrset_mx);
    e->lrset[o].push = push; e->lrset[o].key = key;
    pthread_mutex_unlock(&e->lrset_mx);
}

/* The weight gate's measurement (WP_GATE_PCT), made once this anchor's field against the
 * previous anchor exists. Where the lowres frame means differ by two or more,
 * a checkerboard of blocks is searched twice against the previous anchor,
 * seeded with the field's vector and zero: once as it is, once mapped through
 * the inverse of the frame-DC weight (the residual a weighted prediction
 * leaves, scaled by 1/w). Returns the percent of SATD the weight saves, with
 * the weighted SATD scaled back by w, floored at 0; -1 with no shift to
 * weight. The second search is what makes it work: vectors found without the
 * weight already absorb part of the brightness change on textured content,
 * so scoring the weight at them reads a fade as a loss. A fade saves 15-75%;
 * motion that moves the frame mean saves nothing or loses. */
static int lr_wp_vote(const yah264_encoder_t *e, const struct la_entry *en, const pixel *ref)
{
    int lw = e->lr_w, lh = e->lr_h, wmb = e->width_in_mbs, hmb = e->height_in_mbs;
    const pixel *cur = en->lowres;
    uint64_t sc = 0, sr = 0;
    for (int i = 0; i < lw * lh; i++) { sc += cur[i]; sr += ref[i]; }
    int64_t N = (int64_t)lw * lh;
    if (!sr || llabs((int64_t)(sc / N) - (int64_t)(sr / N)) < 2) return -1;
    const int D = 5, scale = 1 << D;
    int w = (int)((sc * scale + sr / 2) / sr);
    if (w < 1) w = 1; else if (w > 127) w = 127;
    int64_t num = (int64_t)sc * scale - (int64_t)sr * w;
    int o = (int)((num + (num >= 0 ? N * scale / 2 : -(N * scale / 2))) / (N * scale));
    const y264_lr_blk *leg = en->leg[LR_LEG_ANCHOR];
    int stage = lr_me_stage();
    long sp = 0, sw = 0;
    for (int my = 0; my < hmb; my++)
        for (int mx = (my & 1); mx < wmb; mx += 2) {
            const y264_lr_blk *b = &leg[my * wmb + mx];
            const pixel *sb = cur + (my * 8) * lw + mx * 8;
            pixel ib[64];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int v = ((sb[y * lw + x] - o) * scale + w / 2) / w;
                    ib[y * 8 + x] = (pixel)(v < 0 ? 0 : v > PIXEL_MAX ? PIXEL_MAX : v);
                }
            int cand[2][2] = { { b->mvx, b->mvy }, { 0, 0 } };
            int vx, vy; long satd_p, satd_w;
            lr_me_block(sb, lw, ref, lw, (pixel *const *)e->lr_subpel[0], lw, lh, mx * 8, my * 8,
                        b->mvx, b->mvy, (const int (*)[2])cand, 2, stage, 0, 0, 0, &vx, &vy, &satd_p);
            lr_me_block(ib, 8, ref, lw, (pixel *const *)e->lr_subpel[0], lw, lh, mx * 8, my * 8,
                        b->mvx, b->mvy, (const int (*)[2])cand, 2, stage, 0, 0, 0, &vx, &vy, &satd_w);
            sp += satd_p; sw += satd_w * w / scale;
        }
    if (!sp) return -1;
    int r = (int)(100 - 100 * sw / sp);
    return r < 0 ? 0 : r;
}

static void lowres_anchor_me(yah264_encoder_t *e, struct la_entry *en)
{
    NLED_SITE(Y264_LED_SITE_LOWRES);
    NLED(leg_anchor, 1);
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs, nmb = wmb * hmb;
    int lw = e->lr_w, lh = e->lr_h;
    int stage = lr_me_stage();
    double mvlambda = MBTREE_MVLAMBDA;
    const int price = 1;            /* the MV rate is priced into the chain */
    y264_lr_blk *leg = en->leg[LR_LEG_ANCHOR];
    const pixel *ref = e->la_anchor_lr;
    int thave = e->la_anchor_mv_have && e->la_anchor_mvx && e->la_anchor_mvy;

    if (stage >= 3) {
        /* The previous anchor's planes were built one anchor ago, as the
 * future reference of its own B pair legs (lowres_bleg_me writes them to
 * lr_subpel[1] from the lowres that became la_anchor_lr). Take them over
 * instead of building the same fifteen phases again. */
        if (e->lr_sub1_is_anchor) {
            for (int p = 1; p < 16; p++) {
                pixel *t = e->lr_subpel[0][p];
                e->lr_subpel[0][p] = e->lr_subpel[1][p];
                e->lr_subpel[1][p] = t;
            }
            int to = e->lr_sub_obj[0];
            e->lr_sub_obj[0] = e->lr_sub_obj[1]; e->lr_sub_obj[1] = to;
        } else {
            if (g_lrs_census > 0) g_lrs_site[3]++;
            lr_sub_fresh(e, 0);
            build_lr_subpel(e->lr_subpel[0], ref, lw, lh);
        }
    }
    e->lr_sub1_is_anchor = 0;

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

/* Per-B lowres pair fields: for each typed B
 * between the previous anchor (POC prev_poc, lowres still in e->la_anchor_lr)
 * and the just-typed anchor `en`, compute its list-0 pair field (B vs prev
 * anchor -> leg[LR_LEG_ANCHOR]) and list-1 pair field (B vs en ->
 * leg[LR_LEG_NEXT]). Colocated candidates come from the anchor pair's field
 * scaled by the B's temporal position (the same distance-scaling geometry:
 * dmv0 = mvr*d0/D, dmv1 = dmv0 - mvr). Runs after lowres_anchor_me, which
 * left that field in e->la_anchor_mvx/mvy and (stage 3) the prev anchor's
 * subpel planes in lr_subpel[0]. Stashed at pop as full-res B ME seeds. */
/* NLED_SITE(LOWRES) is set by both lowres ME entries; the analyze_* entries in
 * mbcode.c re-stamp the site, so no restore is needed here. */
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
        lr_sub_fresh(e, 1);
        build_lr_subpel(e->lr_subpel[1], en->lowres, lw, lh);
        lr_sub_publish(e, 1, en->push_idx, en->lowres);
        e->lr_sub1_is_anchor = 1;       /* en->lowres becomes la_anchor_lr below */
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
    const int16_t *bcx = e->la_anchor_mvx, *bcy = e->la_anchor_mvy;
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
                                  e->lr_subpel[0], bcx,
                                  bcy, sf0, 0, 0.0, 0,
                                  bn->leg[LR_LEG_ANCHOR], &fcs[ns], &sps[ns]))
            lowres_field_me(e, bn->lowres, bn->d_intra, e->la_anchor_lr,
                            e->lr_subpel[0], bcx, bcy,
                            sf0, 0, 0.0, 0, bn->leg[LR_LEG_ANCHOR]);
        else
            ns++;
        if (!lowres_field_me_prep(e, bn->lowres, bn->d_intra, en->lowres,
                                  e->lr_subpel[1], bcx,
                                  bcy, sf0, 1, 0.0, 0,
                                  bn->leg[LR_LEG_NEXT], &fcs[ns], &sps[ns]))
            lowres_field_me(e, bn->lowres, bn->d_intra, en->lowres,
                            e->lr_subpel[1], bcx, bcy,
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

/* The b-adapt demote threshold: a candidate B becomes an anchor when even
 * its best of bidirectional, forward and intra lowres cost is at least this
 * fraction of its intra cost. Until 2026-10-03 it was 0.90. On a texture
 * that motion cannot predict -- riverbed_1080p's water, where the reference
 * codes 234 of 250 frames as P and over half their macroblocks as intra --
 * every candidate reads 0.79-0.81 (0.84-0.85 before the backward cost was
 * folded from the successor's own leg), so nothing was demoted and the clip
 * coded three B frames in four, each predicted from anchors four frames
 * apart: -0.49 VMAF-NEG / -0.15 dB behind the reference at matched size. At
 * 0.77 it codes P frames throughout. No other clip measured comes near it:
 * the largest single candidate elsewhere reads 0.74 (stefan), the 90th
 * percentiles 0.68 at most, and the CRF band is byte-identical. */
#define BADAPT_DEMOTE_THR 0.77

/* --b-bias moves the demotion line linearly: to the whole intra cost at +100,
 * which cb (a per-block minimum that includes intra) can only reach where
 * nothing predicts better than intra, and to zero at -100, where every frame
 * demotes. 0 returns the threshold untouched, so the default stays bit-exact. */
static double badapt_bias_thr(int bias)
{
    double t = BADAPT_DEMOTE_THR;
    if (bias > 0) return t + (1.0 - t) * bias / 100.0;
    if (bias < 0) return t + t * bias / 100.0;
    return t;
}

static void la_finalize(yah264_encoder_t *e, struct la_entry *en,
                        const struct la_entry *nen)
{
    IP_SCOPE_B(IP_LA);
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
 * the pre-cut frame -- a flash guard one frame past the candidate. With no cut this is identical to a
 * push-time decision, so no-flash content stays byte-for-byte unchanged. */
    int raw_cut = 0;
    /* keyint_min gates only the IDR PROMOTION, not the cut itself: a
 * detected cut closer than keyint_min still becomes a plain I frame (the
 * keyframe distance decides I-vs-IDR). With no non-IDR I at all, gating on
 * keyint_min suppresses the cut ENTIRELY, which also makes scenecut_decide's
 * own `gop_size <= keyint_min/4` bias ramp unreachable dead code.
 *
 * sintel's cut lands at ~frame 20 inside keyint_min 25: suppressed, that
 * codes a rigid I B B B P... cadence straight through it and spends 76% of
 * the clip's bits on the 8 frames after it, where the reference encoder's
 * output has an I there and spends 24%. */
    int sc_min = sc.keyint_min;
    if (!sc.off && e->la_have_prev_fin && !en->sc_cleared &&
        e->la_since_idr >= sc_min)
        raw_cut = scenecut_decide(&sc, en->sum_icost, en->sum_cost[LR_LEG_PREV], e->la_since_idr);
    if (raw_cut && !sc_jump_ok((double)en->sum_cost[LR_LEG_PREV] / (double)en->sum_icost,
                               e->sc_rhist, e->sc_rn, en->sum_icost, e->sc_prev_ic))
        raw_cut = 0;
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
 * sequence (e.g. the reference encoder's, parsed from its first-pass stats
 * file) through this one, so
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
    /* A per-frame force reaches the type decision here, where the plan's forced
 * IDR already does: an IDR is a cut, and any other force clears one, so a
 * frame the caller named is the type the caller named and not a scene cut's.
 * The B leg is finished below, after the anchor rule has run, because
 * whether a B can be placed at all is the anchor rule's answer. */
    int force_disp = (int)(en->push_idx - 1);
    const yah264_frame_force_t *ff = e->nforces ? force_at(e, force_disp) : NULL;
    if (ff && ff->type == YAH264_FORCE_IDR) raw_cut = 1;
    else if (ff && ff->type != YAH264_FORCE_NONE) raw_cut = 0;
    en->is_cut = raw_cut;
    if (raw_cut)
        e->la_since_idr = 0;
    int since = e->la_since_idr;
    e->la_since_idr = (e->la_since_idr + 1) % keyint;
    en->is_idr = (since == 0);
    en->since_val = since;
    e->sc_prev_ic = e->la_have_prev_fin ? en->sum_icost : -1;
    if (e->la_have_prev_fin && en->sum_icost > 0) {
        e->sc_rhist[2] = e->sc_rhist[1]; e->sc_rhist[1] = e->sc_rhist[0];
        e->sc_rhist[0] = (double)en->sum_cost[LR_LEG_PREV] / (double)en->sum_icost;
        if (e->sc_rn < 3) e->sc_rn++;
    }
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
 * conservative: three cost-driven formulations (pairwise, a
 * path-cost geometry at integer-pel, the same with half-pel refined
 * costs) all over-demoted and measured +8-12% BD. The bias is the
 * zero-start lowres diamond overestimating exactly the longest-
 * distance term, which always sits on the continue-run path; fixing
 * it needs seeded per-frame-pair lowres MV caches (a full
 * lookahead-ME subsystem). */
        long cb = 0, ci = 0;
        int fold = nen->leg[LR_LEG_PREV][0].mvx != LR_MV_INVALID;
        long long tq = s_lrrow_prof > 0 ? lrrow_now() : 0;
        for (int my = 0; my < hmb; my++)
            for (int mx = 0; mx < wmb; mx++) {
                int i = my * wmb + mx;
                const pixel *sb = en->lowres + (my * 8) * e->lr_w + mx * 8;
                int mvx, mvy;
                /* the successor's own previous-frame leg (nen against en,
                 * already searched at nen's push) stands in for a fresh
                 * en-against-nen search: the same pair of frames read the
                 * other way round, block for block. The fresh search was a
                 * full-frame diamond per B candidate, serial on the chain
                 * (1.3-1.4% of instructions at 1080p and 2160p); frame types
                 * are identical on 36 of 38 corpus and multi-shot clips. */
                long bwd = fold ? nen->leg[LR_LEG_PREV][i].d_inter
                                : blk8_inter(sb, e->lr_w, nen->lowres, e->lr_w,
                                             e->lr_w, e->lr_h, mx * 8, my * 8, &mvx, &mvy);
                int32_t pv = en->leg[LR_LEG_PREV][i].d_inter;
                long m = pv < bwd ? pv : bwd;
                if (en->d_intra[i] < m) m = en->d_intra[i];
                cb += m;
                ci += en->d_intra[i];
            }
        if (s_lrrow_prof > 0) __atomic_add_fetch(&s_lrrow_ns[2], lrrow_now() - tq, __ATOMIC_RELAXED);
        anchor = (double)cb >= badapt_bias_thr(e->param.b_bias) * (double)ci;
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
    if (ff && ff->type == YAH264_FORCE_P && !en->is_idr)
        anchor = 1;
    if (ff && ff->type == YAH264_FORCE_B) {
        /* The three ways a B cannot go here, all of them structural: the frame
 * is a key frame (the interval claimed it, and moving the key frame is
 * not what a type force means), there is no successor to predict
 * backwards from, or the B run is already at --bframes. Refused with
 * the frame number rather than silently coded as an anchor, because a
 * caller that asked for a placement and got a different one has no way
 * to tell. */
        if (en->is_idr || !nen || e->la_brun >= e->bframes) {
            if (!e->force_fail) {
                e->force_fail = force_disp + 1;
                fprintf(stderr, "yah264: frame %d cannot be coded as a B frame (%s)\n",
                        force_disp,
                        en->is_idr ? "the key-frame interval places a key frame here"
                                   : !nen ? "no later frame to predict backwards from"
                                          : "the B run is already at --bframes");
            }
        } else
            anchor = 0;
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
    en->wp_vote = -1;
    if (en->is_idr || !e->la_anchor_have) {
        for (int i = 0; i < wmb * hmb; i++)
            en->leg[LR_LEG_ANCHOR][i] = (y264_lr_blk){ en->d_intra[i], 0, 0, 0 };
        e->la_anchor_mv_have = 0;
        e->lr_sub1_is_anchor = 0;           /* la_anchor_lr becomes this frame's */
    } else {
        TPROF_LA(e, TP_LOOKME, 1, {
            lowres_anchor_me(e, en);
            lowres_bleg_me(e, en, nb_run, e->la_anchor_poc);
        });
        en->aleg_poc0 = e->la_anchor_poc;    /* the walk's past anchor for this source */
        en->aleg_have = 1;
        en->wp_vote = e->pps.weighted_pred_flag
                    ? lr_wp_vote(e, en, e->la_anchor_lr) : -1;
    }
    memcpy(e->la_anchor_lr, en->lowres, lrsz * sizeof(pixel));
    if (e->la_anchor_hp[0] && en->lr_hp[0])
        for (int k = 0; k < 3; k++) memcpy(e->la_anchor_hp[k], en->lr_hp[k], lrsz * sizeof(pixel));
    e->la_anchor_have = 1;
    e->la_anchor_poc = en->since_val * 2;
}

/* measurement (local): Y264_LRROW_PROF=1 splits la_lr_row CPU into the
 * intra estimate and the previous-frame search; printed at close. */
/* One macroblock row of la_push's lowres pass (see the call site). */
struct la_lr_ctx { yah264_encoder_t *e; struct la_entry *en; int wmb;
                   long *ic, *pc; const int32_t *hint_di; };
static void la_lr_row(struct la_lr_ctx *c, int my, long *ic_out, long *pc_out)
{
    IP_SCOPE_B(IP_LA_LRROW);
    NLED_SITE(Y264_LED_SITE_LOWRES);
    yah264_encoder_t *e = c->e;
    struct la_entry *en = c->en;
    long ic = 0, pc = 0;
    long long tA = 0, tB = 0;
    for (int mx = 0; mx < c->wmb; mx++) {
        int i = my * c->wmb + mx;
        const pixel *sb = en->lowres + (my * 8) * e->lr_w + mx * 8;
        int mvx, mvy;
        long long t0 = s_lrrow_prof > 0 ? lrrow_now() : 0;
        long ii = c->hint_di ? c->hint_di[i]
                : blk8_intra_dispatch_p(sb, e->lr_w, mx, my,
                      e->la_have_prev ? e->la_lr_prev + (my * 8) * e->lr_w + mx * 8 : NULL, LR_I10_MODE, e->lr_intra_pen);
        long long t1 = s_lrrow_prof > 0 ? lrrow_now() : 0;
        long pp = e->la_have_prev
                ? blk8_inter(sb, e->lr_w, e->la_lr_prev, e->lr_w,
                             e->lr_w, e->lr_h, mx * 8, my * 8, &mvx, &mvy)
                : ii;
        if (s_lrrow_prof > 0) { long long t2 = lrrow_now(); tA += t1 - t0; tB += t2 - t1; }
        en->d_intra[i] = (int32_t)ii;
        en->leg[LR_LEG_PREV][i] = (y264_lr_blk){
            (int32_t)pp, 0,                              /* r_inter=0 until R1 */
            e->la_have_prev ? (int16_t)mvx : LR_MV_INVALID,
            e->la_have_prev ? (int16_t)mvy : 0 };
        ic += ii;
        pc += ii < pp ? ii : pp;
    }
    if (s_lrrow_prof > 0) { __atomic_add_fetch(&s_lrrow_ns[0], tA, __ATOMIC_RELAXED);
                            __atomic_add_fetch(&s_lrrow_ns[1], tB, __ATOMIC_RELAXED); }
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
 * -- on the dedicated la thread; see the purity note at
 * struct la_thread. The frame's B/anchor type lags one push (la_finalize). */
static void la_chain_step(yah264_encoder_t *e, struct la_entry *en)
{
    size_t lrsz = (size_t)e->lr_w * e->lr_h;
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs;
    /* en->plane was padded in place by the caller (pad_input_to) -- no copy. */
    long ic = 0, pc = 0;
    TPROF_LA(e, TP_LOWRES, 0, {
    /* A staged hint (the CLI's streamed cut scan computed this frame's lowres
     * plane and its intra costs against the frame before it) stands in for
     * the downscale and the intra pass, byte for byte -- but only when this
     * instance's previous frame is that frame, so never on a GOP's first. */
    const int32_t *hdi = en->hint && e->la_have_prev ? en->hint_di : NULL;
    if (hdi)
        memcpy(en->lowres, en->hint_lr, lrsz * sizeof(pixel));
    else
        downscale(en->lowres, e->lr_w, e->lr_h, en->plane[0], e->pstride[0]);
    if (en->lr_hp[0])
        lr_hp_build(en->lr_hp, en->plane[0], e->pstride[0], e->padded_w, e->padded_h, e->lr_w, e->lr_h);

    /* Per-MB independent (disjoint writes, read-only source), so it runs a row
 * per task. The two cost sums are accumulated per ROW and reduced in row
 * order after the join: integer addition regroups exactly, so the totals --
 * and every scene-cut decision downstream of them -- are unchanged. */
    struct la_lr_ctx lc = { e, en, wmb, NULL, NULL, hdi };
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
 * successor frame is visible and a one-frame flash can be suppressed (the
 * reference encoder at --preset medium does not cut on a one-frame flash).
 * Only the raw frame-vs-prev sums are captured here. */
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
 * chain lag the driver by k steps, which is the lead --sync-lookahead
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
    IP_SCOPE_B(IP_LA);
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
    /* PAFF: the copy above carries the whole frame-sized grid, so both fields
 * of the pair travel -- but the pointers this picture reads are into ITS
 * half of it, and repointing at the base hands the trailing emit the other
 * field's motion. It cost a round: the analyze pass had the offset, the
 * emit pass did not, and only CABAC noticed, because only CABAC reads a
 * neighbour's mvd for a context. The offset is build_slice_prep's. */
    size_t mo = e->fld_pic ? (size_t)e->fld_parity * (mvc / 2) : 0;
    f->mvx = G->mvx + mo; f->mvy = G->mvy + mo;
    f->mvx1 = G->mvx1 + mo; f->mvy1 = G->mvy1 + mo;
    f->refidx = G->refidx + mo; f->refidx1 = G->refidx1 + mo;
    f->mvdx = G->mvdx + mo; f->mvdy = G->mvdy + mo;
    f->mvdx1 = G->mvdx1 + mo; f->mvdy1 = G->mvdy1 + mo;
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
    emit_picture(&p->f, p->job, &p->hdr, &p->bs, &p->cb, p->cabac, &p->sm);
}

/* W2 drain: block until the pending emit finishes, then append its NAL (in coding
 * order) and run the deferred RC accounting. A no-op when nothing is pending.
 * `off` is the DRAINING call's output cursor, not the submitting call's: a
 * pending emit may outlive the encode call that submitted it (deferred-NAL API
 * contract -- a frame's NAL can be returned by a later call), so the
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
    size_t size = slice_map_bytes(&p->sm);
    if (size == 0) {
        slice_overflow_warn();
        if (e->rcp_on)
            rcp_drop(e);                /* pending entry retired without accounting */
        return;                         /* overflow: drop (mirrors serial ret 0) */
    }
    int r; TPROF(TP_NAL, r = append_picture_nals(e, off, p->ref_idc, p->nal_type,
                                                p->rbsp, &p->sm, p->disp, p->rp));
    if (r < 0) {
        if (e->rcp_on)
            rcp_drop(e);                /* retire the entry, or every later fill lands one frame late */
        return;
    }
    if (e->rcp_on)                      /* stage the actuals; the schedule pops */
        rcp_fill(e, 8.0 * (double)size);
    /* CQP and pure CRF have no rate-control tail. */
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
                     || (type != 2 && !(e->rcp_lag > 0 && !e->vbv_on))
                     || (e->vbv_on && e->rcp_vbv_tight));
    if (rc_waits) {
        g_ew_site = 0;
        w2_drain(e, off);               /* prev frame's bits feed this frame's RC */
    }
    if (e->rcp_on) {
        rcp_head(e, type, is_ref, src); /* lagged decide; no drain forced */
    } else {
        rc_head_serial(e, type, is_ref, src);
    }

    int g = e->w2_gen;
    struct w2_gen *G = &e->gen[g];
    struct w2_pending *p = &e->pipe;
    int cabac = e->pps.entropy_coding_mode_flag ? 1 : 0;
    y264_bs_t bs; y264_frame_t f; int fqp, deblock;
    y264_cabac_t cb;                    /* analyze's own engine, NOT the pending's */
    struct slice_hdr hdr;
    struct frame_work fw; fw_default(e, &fw);
    TPROF(TP_PREP, build_slice_prep(e, type, is_idr, is_ref, src, G->rbsp, e->rbsp_cap,
                     &fw, &bs, &f, &fqp, &deblock, &hdr));
    if (cabac) {
        while (y264_bs_pos_bits(&bs) & 7)
            y264_bs_write1(&bs, 1);
        y264_cabac_init_engine(&cb, bs.p); y264_cabac_set_end(&cb, bs.end - 64);
        y264_cabac_init_contexts(&cb, type, 0, fqp);
        cb.field = f.field_pic;         /* field scan + the field context half */
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
    /* Both of these are per DISPLAY FRAME, so a field pair reports itself once,
 * on its second field. The serial path always said so; this one did not,
 * which was invisible until a field pair could reach it. */
    if (!(e->fld_pic && !e->fld_second))
        y264_note_emit(e, e->cur_disp);
    if (e->recon_cb && !(e->fld_pic && !e->fld_second)) {
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
    p->hdr = hdr;
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
    p->disp = e->cur_disp;
    p->rp = e->cur_open_key;
    p->rc_type = type;
    p->is_ref = is_ref;
    p->active = 1;
    e->w2_gen ^= 1;
    ntp_bg_submit(e->bg, w2_emit_task, p);
    /* Under the ring the next decide drains this emit itself (the pre-decide
 * drain above), so waiting here only forfeits the overlap: 12 warm anchors
 * x one 1080p CABAC emit = 84 ms on riverbed ABR t12 (2026-09-04). */
    if (rc_waits && !e->rcp_on) {
        g_ew_site = 2;
        w2_drain(e, off);               /* serialise: RC tail needs this frame's bits now */
    }
    return 0;
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
        /* The serial emit under rcp: the SAME lagged schedule (decide with the
 * previous frames pending, account at the next anchor's decide), so
 * output matches the engaged path at any thread count. */
        rcp_head(e, type, is_ref, src);
    } else {
        rc_head_serial(e, type, is_ref, src);
    }
    struct slice_map sm;
    size_t rbsp_size = rcr_build_slice(e, type, is_idr, is_ref, src, &sm, 0);
    if (rbsp_size == 0) { slice_overflow_warn(); }
    if (rbsp_size == 0)
        return -1;
    /* --- bounded against the MEASURED size ---------------------------------
 *
 * The VBV clip prices a PREDICTION, and the prediction has no bound worth
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
 * before rcp_fill. Under rcp the decide already pushed a
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
        double fill = e->rcp_on ? rcp_vbv_vfill(e, 1) : e->vbv.fill;
        double limit = vbv_limit_at(e, fill);
        /* Capped VBR, an intra frame: it also leaves the reserve the burst
 * plan asks of an intra frame it can price, half the plan's target level
 * (or half of what is there, below that). A GOP instance's first frame is
 * the one frame the plan cannot price -- the instance has measured no
 * scale yet -- and 0.9 of the buffer let an IDR three frames after a cut
 * take 45% of it into a stretch that does not fit at QP 51 (Big Buck
 * Bunny 720p at 503 kbit/s, one thread: the cell's one underflow). */
        if (type == 0 && e->abr_on && !e->vbv_cbr && !e->tp_pass && e->rck.vbv_plan_pred > 0) {
            double l2 = fill - 0.5 * e->rck.vbv_plan_t * e->vbv.size;
            /* Y264_VBV_ENTRY_OPT: while the instance believes the buffer is
 * fuller than what it was promised, the plan's reserve is taken off the
 * believed occupancy, and the frame still leaves Y264_VBV_ENTRY_RSV of the
 * buffer of the promised one. */
            if (e->vbv_opt > 0.0) {
                double bf = fill + e->vbv_opt;
                if (bf > e->vbv.size) bf = e->vbv.size;
                double lo = bf - 0.5 * e->rck.vbv_plan_t * e->vbv.size;
                double lc = fill - e->rck.vbv_entry_rsv * e->vbv.size;
                if (lo > lc) lo = lc;
                if (lo > l2) l2 = lo;
            }
            if (l2 < 0.5 * fill) l2 = 0.5 * fill;
            if (l2 < limit) limit = l2;
        }
        e->vbv_first_bound = 0;         /* the one-shot half, spent */
        /* --crf-max bounds THIS raise too. It is the same raise the predicted
 * clamp makes, arrived at from the measured size instead, so a ceiling
 * that did not hold here would be a ceiling the instance's first frame
 * escapes and nothing else does. */
        int fqp0 = frame_qp(e, type, is_ref), qp0 = e->qp;
        /* A forced QP is not a QP the buffer may move, so there is nothing
 * here to try: the re-encode would code the same frame four times. */
        for (int k = 0; k < 4 && limit > 0.0 && forced_qp(e) < 0; k++) {
            double bits = sm_wire_bits(&sm);   /* what the buffer is charged */
            if (bits <= limit)
                break;
            int bump = (int)ceil(6.0 * log2(bits / limit));
            if (bump < 1) bump = 1;
            int nq = e->qp + bump;
            if (nq > 51) nq = 51;
            nq = (int)crf_max_clip(e, qp0, nq, fqp0 - qp0);
            if (nq == e->qp)
                break;                  /* already at the ceiling */
            e->qp = nq;
            e->chroma_qp = y264_chroma_qp(nq, e->param.chroma_qp_index_offset);
            if (e->rcp_on)
                rcp_reqp(e, type, is_ref);
            struct slice_map sm2;
            size_t sz = rcr_build_slice(e, type, is_idr, is_ref, src, &sm2, 1);
            if (sz == 0)
                break;                  /* keep the last good slice */
            rbsp_size = sz;
            sm = sm2;
        }
    }
    /* The recon may serve as a reference (copied into the ring / DPB / ref1):
 * replicate its edges outward so out-of-frame MC reads the borders. */
    if (!RCR_PLAY(e))
        TPROF(TP_BORDERS, extend_borders(e, e->rec));
    for (int c = 0; c < 3; c++) e->rec_out[c] = e->rec[c];
    int nal_type = is_idr ? YAH264_NAL_SLICE_IDR : YAH264_NAL_SLICE;
    int ref_idc = is_idr ? YAH264_NAL_PRIORITY_HIGH : (is_ref ? 2 : 0);
    int r; TPROF(TP_NAL, r = append_picture_nals(e, off, ref_idc, nal_type, e->rbsp, &sm,
                                                e->cur_disp, e->cur_open_key));
    /* Coding order is reported per DISPLAY FRAME, so a field pair notes itself
 * once -- on its second field, when the frame is complete. */
    if (r >= 0 && !(e->fld_pic && !e->fld_second))
        y264_note_emit(e, e->cur_disp);
    /* PAFF: the callback (and the CLI's --dump-recon through it) is per
 * DISPLAY FRAME, so it fires once, after the second field has written its
 * half of the same buffer. The two fields are woven by construction --
 * they reconstruct into alternating rows of one plane. */
    if (r >= 0 && e->recon_cb && !RCR_PLAY(e) && !(e->fld_pic && !e->fld_second)) {
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
 * coded QP, the mb-tree mean offset and the bits, so an operating-point
 * divergence against another encoder's per-frame QP is readable side by
 * side. */
    if (r >= 0 && e->rck.rc_trace)
        fprintf(stderr, "rct poc=%d type=%d ref=%d base=%d coded=%d moff=%.3f bits=%.0f\n",
                e->poc, type, is_ref, e->qp, frame_qp(e, type, is_ref),
                e->mbtree_mean_off, 8.0 * (double)rbsp_size);
    return r;
}

/* One DISPLAY FRAME's worth of coded pictures: one under frame coding, two
 * under PAFF. Every caller that used to call emit_frame for a whole frame
 * calls this instead, so the anchor path, the B pipeline and the tail flush all
 * field-code by the same rule instead of one of them carrying a loop.
 *
 * What belongs to the FRAME stays outside: its type, its lookahead entry, its
 * mb-tree field, its place in coding order, and the DPB store that follows the
 * pair. What each field gets of its own is a parity, a POC and a reference
 * list. The two share one frame_num -- they are a complementary field pair --
 * and split the frame's POC 2n into 2n and 2n+1, so the top field precedes the
 * bottom in output order whichever is coded first.
 *
 * The second field of an I frame is coded as a P field predicting from the
 * first, which is the one place a field reads the OTHER parity and the whole
 * reason 8.4.1.4's chroma correction is in this encoder. It saves a second
 * intra field per key frame. The second field of an IDR frame is a picture of
 * its own and carries nal_unit_type 1: an IDR there would reset frame_num and
 * POC in the middle of the pair. */
static int emit_pair(yah264_encoder_t *e, size_t *off, int type, int is_idr,
                     int is_ref, pixel *const src[3])
{
    if (!e->fields)
        return emit_frame(e, off, type, is_idr, is_ref, src);
    int poc = e->poc;
    int first_par = (e->fields == 1) ? 0 : 1;   /* --tff codes the top first */
    /* Captured before the loop clears the flag for the second field: the SEI
 * belongs to the first field alone, but the TYPE decision below is about
 * the pair and has to see the same answer twice. */
    int rp = e->cur_open_key;
    int r = 0;
    e->fld_pic = 1;
    e->fld_rc_type = type;
    for (int k = 0; k < 2 && r >= 0; k++) {
        e->fld_second = k;
        e->fld_parity = k ? !first_par : first_par;
        e->poc = poc + e->fld_parity;
        /* A second field with a reference in front of it is a P field; one
 * whose pair is not a reference pair has nothing of its own to predict
 * from and stays whatever the frame is.
 *
 * Not at an --open-gop key. That pair is a random access point and its
 * recovery_point promises an exact decode from a cold start, and a P
 * second field cannot keep the promise: it names its reference by
 * picNum, which is derived from FrameNumWrap, which is derived from a
 * frame_num history the cut threw away. At an IDR the same field is a
 * P field and recovers, because an IDR resets frame_num and the decoder
 * knows it. So the key pair codes both fields intra, which is what
 * PAFF-1 did for every I frame, and it costs one intra field per
 * keyframe. */
        int t = (type == 0 && k == 1 && is_ref && !rp) ? 1 : type;
        r = emit_frame(e, off, t, is_idr && k == 0, is_ref, src);
        /* The recovery point marks the FIRST field of the pair; the second is
 * the same picture's other half and needs no second copy of it. The
 * caller sets the flag and clears it after; clearing it here as well is
 * what keeps the pair to one message. */
        e->cur_open_key = 0;
    }
    e->fld_pic = e->fld_second = e->fld_parity = 0;
    e->fld_rc_type = -1;
    e->poc = poc;
    return r;
}

/* A popped lookahead entry's source planes
 * move into the B buffer or the anchor's stable copy by exchanging buffers
 * instead of copying the frame. All three are allocated with one geometry
 * (plane_alloc with the same sizes and borders), the buffer handed back to
 * the ring slot is one its owner was about to overwrite anyway, and the
 * slot is rewritten whole (pad_input_to) before anything reads it again.
 * Only the plane borders differ, and nothing reads a SOURCE plane's
 * borders. */
static int ring_swap_ok(const yah264_encoder_t *e, pixel *const src[3])
{
    return e->cur_la_en && src == e->cur_la_en->plane;
}
static void ring_swap(pixel *ring[3], pixel *dst[3])
{
    for (int c = 0; c < 3; c++) { pixel *t = ring[c]; ring[c] = dst[c]; dst[c] = t; }
}

static void copy_planes(const yah264_encoder_t *e, pixel *dst[3],
                        pixel *const src[3])
{
    /* Interior pointers in, full bordered buffers copied: the borders travel
 * with the pixels, so a plane extended once stays extended through every
 * copy into the reference ring / DPB / list buffers. */
    int vm = e->pvmul;                  /* the vertical border multiplier (plane_alloc) */
    size_t ol = (size_t)Y264_LUMA_BORDER * vm * e->pstride[0] + Y264_LUMA_BORDER;
    size_t oc = (size_t)Y264_CHROMA_BORDER * vm * e->pstride[1] + Y264_CHROMA_BORDER;
    size_t szl = (size_t)e->pstride[0] * (e->padded_h + 2 * Y264_LUMA_BORDER * vm);
    size_t szc = (size_t)e->pstride[1] * (e->padded_h / e->sub_h + 2 * Y264_CHROMA_BORDER * vm);
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
            e->ref0_fn = e->dpb[slot].frame_num;
            e->cur_ref_l0_fn = e->dpb[slot].frame_num;
            e->frame_num = e->last_ref_fn;          /* non-ref: last reference's FrameNum */
            e->poc = e->bpoc[i];
            e->cur_disp = e->bdisp[i];
            e->cur_lr_motion = e->bmotion[i]; e->cur_lr_tdiff = e->btdiff[i];
            e->rcp_cur_cme = e->rcp_bcplx[i];
            e->rcp_cur_cvi = e->rcp_bcvi[i];
            if (emit_pair(e, off, 1, 0, 0, e->bplane[i]) < 0)
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
        if (emit_pair(e, off, 1, 0, 0, e->bplane[i]) < 0)
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
    if (e->hpel_on && d->hpel[0] && !RCR_PLAY(e)) {
        hpel_build_ref(e, d->hpel[0], d->hpel[1], d->hpel[2], d->plane[0]);
        d->hpel_valid = 1;
    }
    /* Store the resolved co-located motion (8.4.1.2.1): MvL0/RefIdxL0 when this
 * picture used list 0, else MvL1/RefIdxL1 (a B reference may be list-1 only).
 * For a P reference list 1 is empty, so this is just its list-0 motion. */
    /* PAFF: the grid holds BOTH fields of the pair, each in its own half, and a
 * block's refIdx resolves against the list of the field that wrote it. The
 * halves are ordered by parity, not by coding order, so the index decides
 * which list to read and the code below reads the same values a frame
 * picture would (fld_l0* is only consulted under fields). */
    size_t fhalf = mvcount / 2;
    if (!e->fields) {
        /* frame picture: one list for every block, so the refIdx -> POC map
         * is a table and the loop is the three-way select alone */
        int16_t c0[128];
        int l0n = e->cur_l0n;
        for (int r = 0; r < 128; r++)
            c0[r] = (int16_t)(r < l0n ? e->cur_l0poc[r] : -1);
        int16_t c1 = (int16_t)(e->cur_l1poc0 >= 0 ? (e->cur_l1poc0 | Y264_COLPOC_L1) : -1);
        const int8_t *r0 = e->refidx, *r1 = e->refidx1;
        for (size_t i = 0; i < mvcount; i++) {
            if (r0[i] >= 0) {
                d->mvx[i] = e->mvx[i]; d->mvy[i] = e->mvy[i]; d->refidx[i] = r0[i];
                d->colpoc[i] = c0[r0[i]];
            } else if (r1[i] >= 0) {
                d->mvx[i] = e->mvx1[i]; d->mvy[i] = e->mvy1[i]; d->refidx[i] = r1[i];
                d->colpoc[i] = c1;
            } else {
                d->mvx[i] = 0; d->mvy[i] = 0;
                d->refidx[i] = -1;
                d->colpoc[i] = -1;
            }
        }
    } else
    for (size_t i = 0; i < mvcount; i++) {
        int fp = e->fields ? (i >= fhalf) : 0;
        const int *l0poc = e->fields ? e->fld_l0poc[fp] : e->cur_l0poc;
        int l0n = e->fields ? e->fld_l0n[fp] : e->cur_l0n;
        int l1poc0 = e->fields ? e->fld_l1poc0[fp] : e->cur_l1poc0;
        if (e->refidx[i] >= 0) {
            d->mvx[i] = e->mvx[i]; d->mvy[i] = e->mvy[i]; d->refidx[i] = e->refidx[i];
            d->colpoc[i] = (int16_t)(e->refidx[i] < l0n ? l0poc[e->refidx[i]] : -1);
        } else if (e->refidx1[i] >= 0) {
            d->mvx[i] = e->mvx1[i]; d->mvy[i] = e->mvy1[i]; d->refidx[i] = e->refidx1[i];
            d->colpoc[i] = (int16_t)(l1poc0 >= 0 ? (l1poc0 | Y264_COLPOC_L1) : -1);
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
    e->ref0_fn = e->dpb[*l0].frame_num;
    e->ref1_poc = e->dpb[*l1].poc;
    e->colframepoc = e->dpb[*l1].poc;
    e->col_l0poc0 = e->dpb[*l1].col_l0poc0;
    memcpy(e->colmvx, e->dpb[*l1].mvx, mvcount * sizeof(int16_t));
    memcpy(e->colmvy, e->dpb[*l1].mvy, mvcount * sizeof(int16_t));
    memcpy(e->colref, e->dpb[*l1].refidx, mvcount);
    memcpy(e->colpoc, e->dpb[*l1].colpoc, mvcount * sizeof(int16_t));
}

/* --- MT Lever 2: non-reference B leaves in parallel ---
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
    struct slice_hdr hdr;           /* this picture's header, written per slice */
    struct slice_map sm;            /* what each slice cost, filled by the task */
    int             cabac, dblk, fqp, disp;
    size_t          size;           /* RBSP bytes over every slice (0 = overflow) */
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
    for (int c = 0; c < 3; c++) plane_free(L->rec[c], pw[c], pb[c], e->pvmul);
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
    L->rec[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
    L->rec[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
    L->rec[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h, Y264_CHROMA_BORDER, e->pvmul);
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
    /* PAFF: a leaf carries a PRIVATE recon and private decision grids, and a
 * field pair's two pictures share one of each -- the second field predicts
 * from the first field's rows of the same buffer. Two sibling leaves would
 * therefore be two field PAIRS interleaved on one bank. Field coding takes
 * the serial leaf path; the pair lever is a scheduling choice and declining
 * it changes no bits. */
    if (e->fields)
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
        rc_head_crf(e, 2, e->bplane[m], 1);
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
                     &L->bs, &L->f, &L->fqp, &L->dblk, &L->hdr);
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
        L->cb.field = f->field_pic;     /* a picture property; the re-init clears it */
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
    emit_picture(f, job, &L->hdr, &L->bs, &L->cb, L->cabac, &L->sm);
    L->size = slice_map_bytes(&L->sm);
    if (L->dblk)
        y264_deblock_frame(f);
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
    int r = emit_pair(e, off, 2, 0, 0, e->bplane[m]);
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
        /* A leaf B, never the open-GOP key: recovery 0. */
        int r; TPROF(TP_NAL, r = append_slice_nal(e, off, 0, YAH264_NAL_SLICE,
                                                  L->g.rbsp, L->size, 1, L->disp, 0));
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
 * (temporal-pyramid) order: the middle first as a reference, then each half.
 *
 * `flat` codes the whole run as NON-REFERENCE B's, hierarchy and all.
 *
 * It exists for the --open-gop key's leading B frames, and the reason is the
 * decoder's list initialisation and not our own. A reference B among them sits
 * in the decoded picture buffer with a FrameNum ABOVE the key's, so it heads
 * the PicNum-descending default list of every picture coded after it -- and a
 * decoder that started at the key has decoded it from a reference the cut threw
 * away, so it holds the wrong picture under the right index. Keeping it out of
 * OUR list does not help: the index still names it on the decoder's side. Not
 * marking it at all is what makes the two lists agree, and it costs one
 * mini-GOP's temporal layer per keyframe, not a reference the rest of the
 * stream wanted. */
static int code_b_hier(yah264_encoder_t *e, int a, int b, int depth, size_t *off,
                       size_t mvcount, int flat)
{
    if (a >= b) return 0;
    int m = (a + b) / 2;
    int is_ref = !flat && ((m > a) || (m + 1 < b));  /* has children to reference it */
    int l0, l1;
    set_b_refs(e, e->bpoc[m], mvcount, &l0, &l1);
    e->poc = e->bpoc[m];
    e->cur_disp = e->bdisp[m];
    e->cur_lr_motion = e->bmotion[m]; e->cur_lr_tdiff = e->btdiff[m];
    e->rcp_cur_cme = e->rcp_bcplx[m];
    e->rcp_cur_cvi = e->rcp_bcvi[m];
    e->cur_bseed = m;                               /* lowres pair seeds for this B */
    /* Temporal layer, for the QP cascade. A flat run has no layers: every
 * picture in it is a leaf, and 0 is what the non-pyramid B run prices its
 * leaves at, so the two flat shapes cost the same. */
    e->cur_b_depth = flat ? 0 : depth;
    /* A reference B takes the next FrameNum (dpb_store advances it); a
 * non-reference B reuses the most recently coded reference's FrameNum. */
    e->frame_num = is_ref ? e->next_frame_num : e->last_ref_fn;
    if (emit_pair(e, off, 2, 0, is_ref, e->bplane[m]) < 0) return -1;
    e->cur_bseed = -1;
    if (is_ref)
        TPROF(TP_DPBSTORE, dpb_store(e, e->bpoc[m], mvcount));
    /* Lever 2: b-a==3 means both children are single non-ref leaves ([a,a+1) and
 * [a+2,a+3)) -- the one shape with two independent siblings to run at once. */
    if (!flat && b - a == 3 && fpipe_ready(e))
        return code_b_pair(e, a, a + 2, depth + 1, off, mvcount);
    if (code_b_hier(e, a, m, depth + 1, off, mvcount, flat) < 0) return -1;
    if (code_b_hier(e, m + 1, b, depth + 1, off, mvcount, flat) < 0) return -1;
    return 0;
}

/* --- MT Lever 3: the reference-frame staircase -------------------------------
 *
 * Without it, a mini-GOP's B frames wait for their future anchor to FULLY
 * finish (analyze + deblock + borders + hpel + colmv commit) -- the dominant
 * serial dependency in single-GOP encoding. The staircase (deterministic
 * frame threading) overlaps them: the anchor encodes
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
 * parameters (the clamp applies even at --threads 1); the CONCURRENCY engages
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
    struct slice_hdr hdr;           /* this picture's header, written per slice */
    struct slice_map sm;            /* what each slice cost, filled by the runner */
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
    /* One entry per stashed NAL, which is one per SLICE and no longer one per
 * picture: a burst holds at most 8 B pictures and each is cut into
 * param.slices of them. */
    struct { size_t off, len; int ref_idc, first, disp; } stash_item[8 * Y264_SLICES_MAX];
    struct { pixel *pl[3]; int disp; } replay[9];
    int      nreplay;
    int      anchor_out_rec;        /* anchor replay event recorded */
    int      anchor_billed;         /* the drain split: this burst's ANCHOR
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
    int32_t *bseedb[8];             /* burst-owned copy of the band aggregates */
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
    /* The launch fold BLOCKS on these rather than spinning: a sched_yield
 * loop here burned a core for every launch (35 of 35 launches waited,
 * 0.9 s on a 150-frame 1080p encode at three threads, 1.9 s at 4K). */
    pthread_mutex_t dauto_mx; pthread_cond_t dauto_cv;
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
    /* the direct-auto fold: launches that blocked on an earlier burst's chain
 * end, and their total blocked time. */
    int      stat_dfold_waits;
    double   stat_dfold_ms;
    int      stat_wide, stat_nwide;  /* wide launches, and the high-water mark
 * of bursts genuinely in flight at once --
 * the one number that says the gate is
 * doing anything at all */
    /* The drain split's own engagement, same discipline: stat_early is the
 * number of launches that reached the prologue with a burst still live and
 * therefore had a drain to defer, and stat_late the number that then
 * retired it after the anchor's jobs were registered. */
    int      stat_early, stat_late, stat_earlyfill;
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
    for (int c = 0; c < 3; c++) plane_free(B->asrc[c], pw[c], pb[c], e->pvmul);
    free(B->lrs_mvx); free(B->lrs_mvy); free(B->lrs_cost);
    for (int i = 0; i < 8; i++) { free(B->bmbtoff[i]); free(B->bseedb[i]); }
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
        fprintf(stderr, "stair-stat: direct-auto fold: %d launches waited "
                "%.2f ms for a chain end\n",
                st->stat_dfold_waits, st->stat_dfold_ms);
    if (stair_stat_on())
        fprintf(stderr, "stair-stat: launch split -- %d launches had "
                "a drain to defer, %d retired it after the launch, %d took the "
                "anchor's actual early\n", st->stat_early,
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
                "anchor's publish\n",
                atomic_load_explicit(&st->stat_prevp_ms, memory_order_relaxed));
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
            for (int c = 0; c < 3; c++) plane_free(C->bspare[i][c], pw[c], pb[c], e->pvmul);
            for (int k = 0; k < 4; k++) free(C->sspare[i][k]);
        }
    for (int k = 0; k < Y264_STAIR_K; k++) {
        pthread_mutex_destroy(&st->chain[k].smx);
        pthread_cond_destroy(&st->chain[k].scv);
    }
    pthread_mutex_destroy(&st->dauto_mx);
    pthread_cond_destroy(&st->dauto_cv);
    free(st);
    e->st = NULL;
}

/* Pool shape (v2): ONE shared worker set. The anchor's
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
    pthread_mutex_init(&st->dauto_mx, NULL);
    pthread_cond_init(&st->dauto_cv, NULL);
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

/* Clamp eligibility: every term is a machine-invariant parameter, so the
 * staircase's clamp SET (which references are clamped, the P list-0 pin, the
 * source-DC weighted-prediction substitute) is the same at any thread count.
 * The clamp's BOUND is not: see the stair_mvy_max resolution in encoder_open.
 * Temporal direct needs no exclusion here on the shipped default:
 * analyze_b_mb drops direct for a macroblock whose derived mvL1 exceeds the
 * bound (see Y264_STAIR_TDIR). Spatial direct needs no per-mode check: the
 * derived MV is a median of already-clamped coded L1 MVs, so the clamp closes
 * over it. */
static int stair_clamp_on(const yah264_encoder_t *e)
{
    /* PAFF: not under field coding. The staircase overlaps a mini-GOP's B
 * frames with their future anchor by publishing that anchor's rows as they
 * become consumable, and its whole budget -- the LAG constant, the vertical
 * MV clamp derived from it, the row gate -- is expressed in rows of ONE
 * coded picture. A field pair is two pictures interleaved in one buffer, so
 * a published row of the anchor is half the rows a B field's clamp was
 * sized against, and the two fields of the pair publish into the same
 * watermark. Re-deriving that budget in field rows is its own piece of
 * work; until then field coding runs the serial B path, which is a speed
 * gap and not a correctness one (the clamp is an env-gated function of the
 * parameters, so declining it cannot change a bit). */
    return e->b_pyramid && !e->fields && !stair_direct_blocks(e)
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
    emit_picture(f, job, &B->hdr, &B->bs, &B->cb, B->cabac, &B->sm);
    B->size = slice_map_bytes(&B->sm);
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
        /* The victim's buffer is never handed to a writer while a live burst
 * reads it: dpbp_recycle (below) gives the slot a fresh bag, and where
 * the pool cannot serve, stair_slot_readers_wait syncs every reader of
 * the entry actually handed out. */
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
                           const uint8_t *rbsp, size_t len, int first, int disp)
{
    if (B->stash_n >= (int)(sizeof B->stash_item / sizeof B->stash_item[0]))
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
    /* Which stashed NAL opens an access unit, and which picture it belongs to.
 * A stashed entry is one SLICE, so only the first of a picture's slices
 * carries the delimiter and the timing SEIs, and the drain cannot work that
 * out from the entry alone -- the pictures in a burst have no marker
 * separating them. */
    B->stash_item[B->stash_n].first = first;
    B->stash_item[B->stash_n].disp = disp;
    B->stash_n++;
    B->stash_len += len;
    return 0;
}

/* The same for a whole picture: one stashed NAL per slice, in coding order. */
static int stair_stash_picture(struct stair_burst *B, int ref_idc,
                               const uint8_t *rbsp, const struct slice_map *sm,
                               int disp)
{
    size_t at = 0;
    for (int s = 0; s < sm->n; s++) {
        if (stair_stash_nal(B, ref_idc, rbsp + at, sm->len[s], s == 0, disp) < 0)
            return -1;
        at += sm->len[s];
    }
    return sm->n ? 0 : -1;
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
/* The drain split: retire the ANCHOR half of a burst without retiring the
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
    /* The staircase never codes the open-GOP key (the burst's anchor prep is
 * a P prep), so this picture is never a recovery point: 0, not B->rp. */
    TPROF(TP_NAL, r = append_picture_nals(e, off, 2, YAH264_NAL_SLICE, B->g.rbsp, &B->sm,
                                          B->disp, 0));
    if (r < 0)
        return -1;
    if (e->rcp_on && !B->anchor_billed)     /* else stair_drain_anchor billed it */
        rcp_fill(e, 8.0 * (double)B->size); /* anchor actuals, coding order */
    for (int k = 0; k < B->stash_n; k++) {
        TPROF(TP_NAL, r = append_slice_nal(e, off, B->stash_item[k].ref_idc,
                                           YAH264_NAL_SLICE,
                                           B->stash + B->stash_item[k].off,
                                           B->stash_item[k].len,
                                           B->stash_item[k].first,
                                           B->stash_item[k].disp, 0));
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
    e->ref0_fn = e->dpb[l0].frame_num;
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
        rc_head_crf(e, 2, B->bplane[m], 1);
    }
    struct frame_work fw;
    /* the BURST's shape: arrivals rewrite e->nbuf and e->bpoc while this
 * chain preps, so the encoder's must not even be read from here */
    fw_default_shape(e, &fw, stair_refb_poc(B->nbuf, B->bpoc));
    fw.mbtoff_b = (is_ref && m >= 0 && m < 8 && B->bmbtoff_valid[m])
                ? B->bmbtoff[m] : NULL;                 /* burst-owned */
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
    fw.bseedb_src = (int32_t *const *)B->bseedb;        /* the burst's own copy */
    fw.bseed_valid = B->bseed_valid;
    fw.bseed_poc0 = B->bseed_poc0;
    fw.bseed_poc1 = B->bseed_poc1;
    fw.dauto_acc = B->dauto;                 /* the burst's counts, decided at launch */
    fw.dauto_valid = direct_auto_armed(e);
    fw.dauto_temporal = B->dauto_temporal;
    build_slice_prep(e, 2, 0, is_ref, B->bplane[m], L->g.rbsp, e->rbsp_cap, &fw,
                     &L->bs, &L->f, &L->fqp, &L->dblk, &L->hdr);
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
        L->cb.field = f->field_pic;     /* a picture property; the re-init clears it */
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
        rc_head_crf(e, 2, src, 1);
    }
    struct slice_map sm;
    size_t sz = build_slice(e, 2, 0, is_ref, src, &sm);
    if (sz == 0)
        return -1;
    STPROF(B, TP_BORDERS, extend_borders(e, e->rec));
    if (stair_stash_picture(B, is_ref ? 2 : 0, e->rbsp, &sm, e->cur_disp) < 0)
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
    emit_picture(&L->f, L->job, &L->hdr, &L->bs, &L->cb, L->cabac, &L->sm);
    L->job = NULL;
    L->size = slice_map_bytes(&L->sm);
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
    return stair_stash_picture(B, L->ref_idc, L->g.rbsp, &L->sm, L->disp);
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
    emit_picture(f, job, &L->hdr, &L->bs, &L->cb, L->cabac, &L->sm);
    L->size = slice_map_bytes(&L->sm);
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
    if (stair_stash_picture(B, 2, L->g.rbsp, &L->sm, L->disp) < 0)
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
        if (stair_stash_picture(B, 0, L->g.rbsp, &L->sm, L->disp) < 0)
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
    if (!stair_wide_on() || !stair_wide_rc_ok(e))
        return;
    int nrefb = stair_plan_nrefb(e->param.bframes);
    int want = Y264_STAIR_K * (1 + nrefb);
    if (want > Y264_DPB_POOL_MAX) want = Y264_DPB_POOL_MAX;
    for (int i = 0; i < want; i++) {
        struct dpb_bag *g = &e->dpbp_free[i];
        g->plane[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
        g->plane[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                                  Y264_CHROMA_BORDER, e->pvmul);
        g->plane[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                                  Y264_CHROMA_BORDER, e->pvmul);
        g->mvx = malloc(mvcount * sizeof(int16_t));
        g->mvy = malloc(mvcount * sizeof(int16_t));
        g->refidx = malloc(mvcount);
        g->colpoc = malloc(mvcount * sizeof(int16_t));
        int ok = g->plane[0] && g->plane[1] && g->plane[2] &&
                 g->mvx && g->mvy && g->refidx && g->colpoc;
        for (int c = 0; c < 3; c++)
            g->hpel[c] = e->hpel_on
                ? plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul) : NULL;
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
    if (B->prev_P && !stair_unsafe_no_prevpwait()) {
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
                }
            }
        }
        stair_tr(e->st, tsl, STE_PREP_E, tsq, B->nplan);
        if (rb == 0) {
            stair_serial_fire(e, B);   /* the arrival phase may start NOW */
            stair_tr(e->st, tsl, STE_FIRE, tsq, 0);
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
                    stair_launch_refb(e, B, C->leaf[i]);   /* v5: streams */
                } else if (B->plan[i].pair == 1 ||
                           (!B->plan[i].is_ref &&
                            i + 1 < B->nplan && !B->plan[i + 1].is_ref)) {
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
 * publish them for the launch fold (DAUTO_FOLD_LAG) */
        unsigned sq = atomic_load(&B->seq);
        long *c = e->st->dauto_fifo[sq % (2 * Y264_STAIR_K)];
        c[0] = B->dauto[0]; c[1] = B->dauto[1];
        pthread_mutex_lock(&e->st->dauto_mx);
        atomic_store_explicit(&e->st->dauto_ready[sq % (2 * Y264_STAIR_K)], sq, memory_order_release);
        pthread_cond_broadcast(&e->st->dauto_cv);
        pthread_mutex_unlock(&e->st->dauto_mx);
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
                                              Y264_LUMA_BORDER, e->pvmul);
                C->bspare[i][1] = plane_alloc(e->padded_w / e->sub_w,
                                              e->padded_h / e->sub_h,
                                              Y264_CHROMA_BORDER, e->pvmul);
                C->bspare[i][2] = plane_alloc(e->padded_w / e->sub_w,
                                              e->padded_h / e->sub_h,
                                              Y264_CHROMA_BORDER, e->pvmul);
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
            B->asrc[0] = plane_alloc(e->padded_w, e->padded_h, Y264_LUMA_BORDER, e->pvmul);
            B->asrc[1] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                                     Y264_CHROMA_BORDER, e->pvmul);
            B->asrc[2] = plane_alloc(e->padded_w / e->sub_w, e->padded_h / e->sub_h,
                                     Y264_CHROMA_BORDER, e->pvmul);
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
                if (ok && e->bseedb_bytes) {
                    B->bseedb[i] = malloc((size_t)e->bseedb_bytes);
                    ok = B->bseedb[i] != NULL;
                }
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
        rc_head_crf(e, 1, src, !async);
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
        if (e->bseedb[i] && B->bseedb[i])
            memcpy(B->bseedb[i], e->bseedb[i], (size_t)e->bseedb_bytes);
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
 * overflow (interp.h, y264_mc_luma_b) and reproduced with the stair OFF.
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
        if (ring_swap_ok(e, src))
            ring_swap(e->cur_la_en->plane, B->asrc);
        else
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
            const unsigned lag = DAUTO_FOLD_LAG;
            while (st->dauto_folded + 1 < n) {                  /* never this burst */
                unsigned sq = st->dauto_folded + 1;
                _Atomic unsigned *rd = &st->dauto_ready[sq % (2 * Y264_STAIR_K)];
                if (sq + lag > n)                               /* past the bound */
                    break;
                if (atomic_load_explicit(rd, memory_order_acquire) != sq) {   /* wait for its chain end */
                    double t0 = stair_stat_on() ? tprof_ms() : 0.0;
                    pthread_mutex_lock(&st->dauto_mx);
                    while (atomic_load_explicit(rd, memory_order_acquire) != sq)
                        pthread_cond_wait(&st->dauto_cv, &st->dauto_mx);
                    pthread_mutex_unlock(&st->dauto_mx);
                    if (stair_stat_on()) {
                        st->stat_dfold_waits++;
                        st->stat_dfold_ms += tprof_ms() - t0;
                    }
                }
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
                     &B->bs, &B->f, &B->fqp, &deblock, &B->hdr));
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
        B->cb.field = B->f.field_pic;   /* a picture property; the re-init clears it */
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
        /* The DRAIN SPLIT: rather than retire every burst it is about to leave
 * flying, retire only their ANCHOR halves (oldest first, the order the
 * ledger is filled in), so the decide below is short only the B leaves,
 * where the tail lives. B->size is final when a burst's runner returns;
 * the chain driver holds the leaves. Measured: ~+1.3% BD-VMAF-NEG at
 * --ref 3 and +0.14-0.7% at --ref 1 on cut-heavy content (9 of 12 clips
 * byte-identical), for 1.04-1.11x on the ABR board, with rate accuracy
 * equal-or-better on every clip. The ring bound stays: a silently-unpushed
 * rcp entry slides every later fill onto the wrong frame. */
        int keep = e->rcp_lag;
        if (stair_stat_on() && e->st->nlive > keep) st->stat_early++;
        for (int n = st->nlive; n > keep; n--) {
            int k = (st->cur - n + 1) % Y264_STAIR_K;
            if (k < 0) k += Y264_STAIR_K;
            if (stair_drain_anchor(e, off, &st->bur[k]) < 0)
                return -2;
        }
        while (e->st->nlive > 0 && e->rcp_n + e->nbuf + 1 > RCP_MAX)
            if (stair_drain(e, off) < 0)
                return -2;
        if (!(e->rcp_lag > 0 && !e->vbv_on)) {
            g_ew_site = 6;
            w2_flush(e, off);           /* no-drain only under a lag budget (emit_frame_w2) */
        }
    }
    /* v3 depth: pipeline this burst across the API boundary when the
 * machinery is up. The caller already held the serial_done
 * handshake if a burst is in flight; a sync burst instead retires it fully
 * first (its chain shares the leaves and bemit the fly's tail holds). */
    int async = e->nbuf <= 7 && stair_async_ready(e) &&
                e->nbuf <= st->chain[stair_next_slot(st)].nleaf;
    /* WIDTH (stage 3): stop draining the previous chain before submitting the
 * next one, and let the drain wait for the ring to need the slot back.
 *
 * At nref <= 1 a P anchor's list 0 is exactly {previous anchor} and a burst
 * leaf's nearest past reference is either that same anchor or its own
 * mini-GOP's reference B: one hop back, which is exactly the reach the single
 * clamp0_poc + the row gate cover. At nref > 1 list 0 reaches further, and
 * under real concurrency more than one of those pictures can be live at
 * once; the multi-hop clamp (clamp0_hop2) covers them.
 *
 * rcp is out unless it has been given a lag budget, and for a reason that
 * is the same statement twice: its zero-lag anchor schedule retires
 * everything in flight before each decide, and deferred retirement is what
 * width IS. stair_wide_rc_ok carries the rule; Y264_RCP_LAG buys the
 * budget, at a rate-accuracy price that is the whole point of measuring it. */
    int wide = async && stair_wide_on() && stair_wide_rc_ok(e);
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
        /* And the shipped band summary of the same three fields. Computed here
 * because the legs are ring-owned and freed before the macroblock loop
 * ever runs; the row partition is the one the frame-open pass uses. */
        if (e->bseedb_pend) {
            int rows = y264_band_rows(), wmb = e->width_in_mbs, hmb = e->height_in_mbs;
            int nb = (hmb + rows - 1) / rows;
            for (int b = 0; b < nb; b++) {
                int y0 = b * rows, y1 = y0 + rows;
                if (y1 > hmb) y1 = hmb;
                long long sp = 0, sq = 0, si = 0; int n = 0;
                for (int y = y0; y < y1; y++)
                    for (int x = 0; x < wmb; x++) {
                        int i = y * wmb + x;
                        long long cp = ((long long)l0[i].d_inter + l1[i].d_inter) / 2;
                        sp += cp; sq += cp * cp;
                        si += en->d_intra ? en->d_intra[i] : 0;
                        n++;
                    }
                long long mean = n ? sp / n : 0, cov2 = 0;
                if (mean > 0) {
                    long long var = sq / n - mean * mean;
                    cov2 = var * 100 / (mean * mean);
                    if (cov2 > INT32_MAX) cov2 = INT32_MAX;
                }
                e->bseedb_pend[3 * b + 0] = (int32_t)mean;
                e->bseedb_pend[3 * b + 1] = (int32_t)cov2;
                e->bseedb_pend[3 * b + 2] = (int32_t)(n ? si / n : 0);
            }
            for (int b = nb; b < hmb; b++)
                e->bseedb_pend[3 * b + 0] = e->bseedb_pend[3 * b + 1] =
                e->bseedb_pend[3 * b + 2] = 0;
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
        if (z[i].first < 0 || z[i].last < z[i].first || (i && z[i].first <= z[i - 1].last) ||
            ((z[i].flags & YAH264_ZONE_QP) && !(z[i].qp_offset >= 0.0 && z[i].qp_offset <= 51.0))) { free(z); return -1; }
    free(e->zones);
    e->zones = z; e->nzones = n;
    return 0;
}

int yah264_encoder_set_frame_forces(yah264_encoder_t *e, const yah264_frame_force_t *forces, int n)
{
    if (!e || n < 0 || (n > 0 && !forces)) return -1;
    if (n > 0 && e->hw) {
        fprintf(stderr, "yah264: the hardware encoder takes no per-frame type or QP force\n");
        return -1;
    }
    yah264_frame_force_t *f = n ? malloc((size_t)n * sizeof *f) : NULL;
    if (n && !f) return -1;
    for (int i = 0; i < n; i++) f[i] = forces[i];
    for (int i = 1; i < n; i++)                 /* sorted by frame, as zone_at's array is */
        for (int k = i; k > 0 && f[k].disp < f[k - 1].disp; k--) {
            yah264_frame_force_t t = f[k]; f[k] = f[k - 1]; f[k - 1] = t;
        }
    for (int i = 0; i < n; i++) {
        if (f[i].disp < 0 || (i && f[i].disp == f[i - 1].disp)) { free(f); return -1; }
        if (f[i].qp < -1 || f[i].qp > 51) { free(f); return -1; }
        if (f[i].type < YAH264_FORCE_NONE || f[i].type > YAH264_FORCE_B) { free(f); return -1; }
        /* The two B refusals that are readable from the array alone. Frame 0
 * opens the stream, so it is a key frame whatever anyone asks; and with
 * no B frames configured there is no B seat to put one in. The rest --
 * a key frame's own slot, the last frame, the run limit -- depend on
 * where the lookahead has got to and are refused there. */
        if (f[i].type == YAH264_FORCE_B && (f[i].disp == 0 || e->bframes <= 0)) {
            fprintf(stderr, "yah264: frame %d cannot be coded as a B frame (%s)\n", f[i].disp,
                    f[i].disp == 0 ? "the first frame of a stream is a key frame"
                                   : "this encode has no B frames (--bframes 0)");
            free(f);
            return -1;
        }
    }
    free(e->forces);
    e->forces = f; e->nforces = n;
    return 0;
}

/* The CLI's streamed cut scan has already computed the next picture's lowres
 * plane and lowres intra costs (against the picture before it); hand them in
 * and the lookahead copies them instead of computing them again. Valid for
 * the next yah264_encoder_encode call only, copied during it. Used only when
 * that picture reaches the lookahead unfiltered and this instance has coded
 * the picture before it, so the output is the same with or without it. */
int yah264_encoder_lowres_hint(yah264_encoder_t *e, const void *lowres, const int32_t *intra)
{
    if (!e || e->hw) return -1;
    e->lrh_lr = lowres; e->lrh_di = intra;
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
        if (e) { e->lrh_lr = NULL; e->lrh_di = NULL; }
        g_enc_inside_ms += tprof_ms() - t0;
        return r;
    }
    int r = yah264_encoder_encode_inner(e, nal, count, pic);
    if (e) { e->lrh_lr = NULL; e->lrh_di = NULL; }  /* one call's worth, whichever path took it */
    return r;
}
static int yah264_encoder_encode_inner(yah264_encoder_t *e, yah264_nal_t **nal, int *count,
                                       const yah264_picture_t *pic)
{
    if (!e || !nal || !count)
        return -1;
    /* A forced type the lookahead could not place. It is found one frame at a
 * time, inside the lookahead, which has no return value to fail with; this
 * is the first call after it that has one. */
    if (e->force_fail)
        return -1;
    e->nal_count = 0;
    size_t off = 0;
    *nal = e->nal;
    /* Deferred-NAL contract : the previous call's last entropy emit
 * may still be in flight here. It is NOT drained at entry -- emit_frame_w2
 * retires it after this call's analyze (full overlap), appending its NAL to
 * this call's output ahead of the new frames', so coding order holds. A call
 * that emits nothing carries it forward; the flush path returns the tail. */
    if (!pic && e->mcstf) {
        /* --mcstf's tail first: the pictures still inside the filter go into
         * the coding flow before the lookahead drains, so an encode codes
         * exactly the pictures it was handed */
        while (off == 0 && y264_mcstf_ready(e->mcstf, 1))
            if (mcstf_pop_push(e, &off) < 0)
                return -1;
        if (off) {
            *count = e->nal_count;
            return (int)off;
        }
    }
    if (!pic) {
        /* Flush: drain the lookahead window one anchor group at a time, then
 * the B reorder buffer. Callers loop until 0 bytes and 0 NALs. The
 * chain drains FIRST, so the tail finalizes below run consumer-side
 * with the la thread idle -- identical order to the serial path. */
        la_th_wait_all(e);
        while (e->la_n > 0 && off == 0 && !RCR_PLAY(e)) {
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
    if (e->mcstf) {
        const pixel *pl[3];
        for (int c = 0; c < 3; c++) pl[c] = (const pixel *)pic->plane[c];
        y264_mcstf_push(e->mcstf, pl, pic->stride, pic->pts);
        while (y264_mcstf_ready(e->mcstf, 0))
            if (mcstf_pop_push(e, &off) < 0)
                return -1;
    } else if (encode_push(e, pic, &off) < 0) {
        return -1;
    }
    *count = e->nal_count;
    return (int)off;
}

/* One picture into the coding flow: the lookahead ring, or the legacy path.
 * NALs append at *offp. */
static int encode_push(yah264_encoder_t *e, const yah264_picture_t *pic, size_t *offp)
{
    size_t off = *offp;
    if (RCR_PLAY(e)) {
        /* rate-control replay: no lookahead; every picture goes straight into
         * the coding flow, with its cut and anchor flags from the recording.
         * The rate controller sees the same sequence of decides either way:
         * the ring only delays the calls, and its one read by the controller
         * (the opening fit) is replayed too. */
        e->cur_la_en = NULL;
        if (encode_frame_core(e, e->plane, 1, 0, 0, &off) < 0)
            return -1;
        *offp = off;
        return 0;
    }
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
        dst->hint = 0;
        if (e->lrh_lr && e->lrh_di && !e->mcstf) {
            size_t lrsz = (size_t)e->lr_w * e->lr_h, nmb = (size_t)e->width_in_mbs * e->height_in_mbs;
            if (!dst->hint_lr) dst->hint_lr = malloc(lrsz * sizeof(pixel));
            if (!dst->hint_di) dst->hint_di = malloc(nmb * sizeof(int32_t));
            if (dst->hint_lr && dst->hint_di) {
                memcpy(dst->hint_lr, e->lrh_lr, lrsz * sizeof(pixel));
                memcpy(dst->hint_di, e->lrh_di, nmb * sizeof(int32_t));
                dst->hint = 1;
            }
        }
        la_push(e);
        if (e->la_n < e->la_cap)     /* ring (window + Y264_LA_BUF) still filling */
            return 0;
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
        *offp = off;
        return 0;
    }
    pad_input(e, pic);                  /* legacy path: input -> e->plane */
    e->cur_la_en = NULL;                /* legacy path: no ring entry, no memo */
    if (encode_frame_core(e, e->plane, 0, 0, 0, &off) < 0)
        return -1;
    *offp = off;
    return 0;
}

/* --mcstf: take the next filtered picture out of the queue and code it. */
static int mcstf_pop_push(yah264_encoder_t *e, size_t *offp)
{
    const pixel *pl[3];
    int st[3], filt;
    yah264_picture_t fp;
    memset(&fp, 0, sizeof fp);
    y264_mcstf_pop(e->mcstf, pl, st, &fp.pts, &filt);
    e->mcstf_filt[e->mcstf_pushed++ & 1023] = (uint8_t)filt;
    fp.csp = e->param.csp;
    fp.width = e->width;
    fp.height = e->height;
    for (int c = 0; c < 3; c++) {
        fp.plane[c] = (void *)pl[c];
        fp.stride[c] = st[c];
    }
    return encode_push(e, &fp, offp);
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
 * skipped. */
    e->cur_wp_vote = e->cur_la_en && e->cur_la_en->aleg_have ? e->cur_la_en->wp_vote : -1;
    if (!RCR_PLAY(e)) TPROF(TP_LOWRES, {
        struct la_entry *lren = e->cur_la_en;
        if (lren && lren->d_intra && lren->leg[LR_LEG_PREV]) {
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
        if (e->lr_bufhp[0][0])
            lr_hp_build(e->lr_bufhp[e->lowres_cur == e->lr_buf0 ? 0 : 1], src_planes[0], e->pstride[0],
                        e->padded_w, e->padded_h, e->lr_w, e->lr_h);
    });
    int mscore = frame_motion_score(e);   /* adaptive-ME signal, this display frame */
    /* The psy-flat log's tdiff column (the calm gate it was built for was
 * removed on 2026-10-04): UNCOMPENSATED lowres |tdiff| per pixel, x256,
 * EWMA-chained in ARRIVAL
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
        e->rcp_arr_cme = RCR_VAL(e->rcr, RCR_ARR_CME, frame_complexity_me(e));
        /* VBV-model complexity: cme floored at a quarter of the intra energy.
 * cme alone collapses on static/noise content (samsung's 3 Mbit
 * sparkle anchors read ~0); intra energy alone is flat within a scene
 * and misses motion-driven cost swings (sintel regressed on it). The
 * hybrid keeps cme's motion signal with a floor that cannot go blind. */
        double ilr = RCR_VAL(e->rcr, RCR_ARR_ILR, frame_complexity_ilr(e));
        e->rcp_arr_cvi = e->rcp_arr_cme > ilr * 0.25 ? e->rcp_arr_cme : ilr * 0.25;
        /* An anchor predicts from the previous ANCHOR, not from the frame
 * before it, so its cost against the previous display frame can read
 * near zero while the frame it actually codes against is a mini-GOP
 * away: samsung's anchor at frame 33 read 12k against 1.5M on its own
 * B frames and coded 500 kbit into a 720 kbit buffer. Where the
 * lookahead already has the anchor-to-anchor field, the VBV model
 * takes the larger of the two. VBV only: the rate decides keep cme. */
        if (e->vbv_on) {
            double ca = RCR_VAL(e->rcr, RCR_ARR_ACX, anchor_leg_cost(e));
            if (ca > e->rcp_arr_cvi) e->rcp_arr_cvi = ca;
        }
    }

    /* Scene cut forces a keyframe here, but no closer than a minimum interval
 * (prevents pathological back-to-back IDRs on e.g. noise). */
    struct sc_cfg sc = sc_cfg_of(&e->param);
    /* keyint_min gates only the IDR PROMOTION, not the cut itself: a
 * detected cut closer than keyint_min still becomes a plain I frame (the
 * keyframe distance decides I-vs-IDR). With no non-IDR I at all,
 * `since_idr >= keyint_min` suppresses the cut entirely, which also makes
 * scenecut_decide's `gop_size <= keyint_min/4` bias ramp unreachable dead
 * code.
 *
 * Measured on sintel, whose cut lands at ~frame 20 inside keyint_min 25:
 * suppressed, that codes a rigid I B B B P... cadence straight through it
 * and spends 76% of the clip's bits on the 8 frames after the cut, where
 * the reference encoder inserts an I there and spends 24%. */
    /* rate-control replay: the cut and anchor decisions come from the recording */
    int rcr_flags = RCR_PLAY(e) ? (int)rcr_get(e->rcr, RCR_FLAGS) : -1;
    /* --intra-refresh turns the detector off, but a keyframe the caller forced
 * (a zone's, a per-frame force) still arrives as a lookahead cut and is
 * still an IDR. Elsewhere sc.off drops it here as before. */
    int is_cut = rcr_flags >= 0 ? (rcr_flags >> 1) & 1
               : (sc.off ? (e->ir_on && have_flags ? flag_cut : 0)
                  : (have_flags ? flag_cut
                     : (e->since_idr >= sc.keyint_min &&
                        detect_scenecut(e, e->since_idr))));
    if (is_cut)
        e->since_idr = 0;
    { pixel *t = e->lowres_prev; e->lowres_prev = e->lowres_cur; e->lowres_cur = t; }
    e->sc_have_prev = 1;

    int since_idr = e->since_idr;
    e->since_idr = (e->since_idr + 1) % keyint;       /* advance for the next frame */
    int is_key = (since_idr == 0);
    /* --open-gop: a key is still an I picture and still an anchor, but it stops
 * being an IDR. Two keys keep the reset anyway. The FIRST picture of the
 * instance has nothing to continue from -- no DPB, no POC line -- and a
 * keyframe the caller's plan asked for is an IDR by the engine-interface
 * contract, because the orchestrator addresses the segment starting there as
 * a file and re-encodes it alone. Everything else, keyint and adaptive cut
 * alike, becomes a recovery point. */
    int zone_idr = 0;
    if (is_key && e->nzones) {
        const yah264_zone_t *z = zone_at(e, (int)e->frame_count);
        zone_idr = z && (z->flags & YAH264_ZONE_IDR) && (int)e->frame_count == z->first;
    }
    int is_idr = is_key && (!e->open_gop || e->frame_count == 0 || zone_idr);
    int open_key = is_key && !is_idr;
    /* With window flags the anchor decision was made at push time (b-adapt);
 * the legacy path keeps the fixed cadence. */
    int is_anchor = have_flags ? flag_anchor
                               : (is_key || (since_idr % period) == 0);
    if (rcr_flags >= 0)
        is_anchor = rcr_flags & 1;
    else if (e->rcr)
        rcr_put(e->rcr, RCR_FLAGS, (double)(is_cut * 2 + (is_anchor ? 1 : 0)));
    /* POC follows display order, counted from the last IDR rather than the last
 * key, so it runs unbroken across an open one. Identical to since_idr * 2
 * whenever every key is an IDR, which is every default encode -- poc_base
 * and frame_count move together at an IDR, which is also why the base can
 * be read here and only updated further down. */
    int poc = is_idr ? 0 : ((int)e->frame_count - e->poc_base) * 2;

    if (!is_anchor) {                                /* B: buffer until its anchor */
        if (RCR_PLAY(e))
            ;                                   /* replay: no pixels to keep */
        else if (ring_swap_ok(e, src_planes))
            ring_swap(e->cur_la_en->plane, e->bplane[e->nbuf]);
        else
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
        /* the frame's lowres intra estimate, which encode_frame_core just
         * produced for this frame (the ring's d_intra, or the same estimate
         * recomputed from the same plane): mb-tree's walks read it for this
         * buffered B instead of estimating it again on every walk */
        e->bdintra_valid[e->nbuf] = 0;
        if (e->bdintra[e->nbuf] && e->lr_intra) {
            int nmb_b = e->width_in_mbs * e->height_in_mbs;
            for (int i = 0; i < nmb_b; i++) e->bdintra[e->nbuf][i] = e->lr_intra[i];
            e->bdintra_valid[e->nbuf] = 1;
        }
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
            if (e->bseedb_pend && e->bseedb[e->nbuf])
                memcpy(e->bseedb[e->nbuf], e->bseedb_pend, (size_t)e->bseedb_bytes);
            e->bseed_poc0[e->nbuf] = e->bseed_pend_poc0;
            e->bseed_poc1[e->nbuf] = e->bseed_pend_poc1;
            e->bseed_valid[e->nbuf] = 1;
        }
        e->nbuf++;
        e->frame_count++;
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
        rcp_vbv_gate(e, is_key);
    }

    /* v3: a burst may still be in flight from an earlier call. A pipelining
 * anchor (pyramid P over buffered B's, depth gate on) only waits for the
 * chain's SERIAL phase -- its own prep/registration then overlaps the
 * burst's tail, and the drain happens inside stair_run_burst after this
 * anchor's jobs are up. Everything else (IDR -- never overlap across it --
 * or a non-pipelining shape) retires the burst fully first: coding order
 * and the burst-end colmv restore precede any serial coding. */
    if (e->st && e->st->nlive) {
        int pipelining = e->b_pyramid && !is_key && e->nbuf > 0 &&
                         stair_ready(e);
        if (pipelining)
            stair_serial_wait(e);
        else if (stair_drain_all(e, &off) < 0)
            return -1;
    }

    /* Anchor (I or P). Buffered B's before an IDR belong to the closing GOP and
 * cannot reference across it, so flush them as non-reference P first. An open
 * key is not a boundary of that kind: those same B's stay B's, coded after
 * the key against the anchor before it and the key itself, which is the whole
 * of what --open-gop buys. */
    if (is_idr && e->nbuf > 0 && flush_buffered_p(e, &off) < 0)
        return -1;

    /* The POC base and the recovery-point bounds are read by build_list0, which
 * the staircase runs on its chain threads, so they are written HERE -- after
 * the burst has been retired and after the closing GOP's own B frames have
 * been coded against the old values -- and not where is_key was decided,
 * which is upstream of the drain. */
    if (is_idr) {
        e->poc_base = (int)e->frame_count;            /* the only thing that restarts POC */
        e->rp_poc = e->rp_poc_prev = -1;              /* an IDR is a harder boundary */
    } else if (open_key) {
        e->rp_poc_prev = e->rp_poc;
        e->rp_poc = poc;
    }
    /* --intra-refresh: where this picture sits in its sweep. The sweeps start
 * on the picture after an IDR and run ir_period pictures each; the IDR
 * itself refreshes everything and is in none. */
    int ir_rp = 0;
    if (e->ir_on) {
        if (is_idr) {
            e->ir_idr = (int)e->frame_count;
            e->ir_j = -1;
            e->ir_bound_poc = -1;
        } else {
            e->ir_j = ((int)e->frame_count - e->ir_idr - 1) % e->ir_period;
            e->ir_start_poc = poc - 2 * e->ir_j;
            e->ir_bound_poc = e->ir_start_poc - 2;
            if (e->ir_j == 0)
                ir_rp = e->ir_period;     /* recovery_frame_cnt + 1 */
        }
    }

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
            e->ref0_fn = e->dpb[aslot].frame_num;
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
 * untouched wait-then-compute path. */
            if (!RCR_PLAY(e)) {
                if (!mbt_oracle_prepare(e))
                    TPROF(TP_MBTWAIT, la_th_wait_mbtree(e));
                TPROF(TP_MBTREE, mbt_resolve(e, src_planes));          /* lower QP where dependents lean on it */
                mbt_warm_launch(e);   /* the next anchor's Phase A, during this burst */
            }
            e->mbtree_mean_off = RCR_VAL(e->rcr, RCR_MBTMEAN, e->mbtree_mean_off);
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
        if (stair_clamp_on(e)) {
            newsum = src_luma_sum(e, src_planes[0]);
            have_newsum = 1;
            /* v6: and this burst's REFERENCE B, on exactly the same terms -- a
 * later anchor clamps it in list 0 and so needs the same substitute
 * for a recon it cannot finish reading. Computed HERE, before
 * stair_launch swaps the arrival-side B planes out to the chain's
 * spare bank: after that e->bplane names the next burst's memory.
 * Index 1 because stair_refb_poc's shape test is bpoc[1]. */
            if (stair_bdepth_on() && !is_idr)
                refbpoc = stair_refb_poc(e->nbuf, e->bpoc);
            if (refbpoc >= 0)
                refbsum = src_luma_sum(e, e->bplane[1][0]);
        }
        /* Staircase: a P anchor with buffered B's is the mini-GOP shape the
 * pipeline overlaps. Not engaged (-1) falls through to the serial
 * path, whose prep idempotently redoes the aborted attempt's writes. */
        /* The burst's anchor prep is a P prep, so the open key takes the
 * serial path below -- one picture per keyint, and the B's behind it
 * are still coded by code_b_hier. */
        if (!is_key && e->nbuf > 0 && stair_ready(e)) {
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
        e->cur_open_key = open_key ? 1 : ir_rp;
        int er = emit_pair(e, &off, is_key ? 0 : 1, is_idr, 1, src_planes);
        e->cur_open_key = 0;
        if (er < 0)
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
        if (code_b_hier(e, 0, e->nbuf, 1, &off, mc, open_key) < 0)
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
        e->ref_navail = 0;                          /* ...and with it the field list's frame set */
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
        if (!RCR_PLAY(e)) {
            if (!mbt_oracle_prepare(e))  /* replay hit: the wait is not the driver's */
                TPROF(TP_MBTWAIT, la_th_wait_mbtree(e));
            TPROF(TP_MBTREE, mbt_resolve(e, src_planes));
            mbt_warm_launch(e);
        }
        e->mbtree_mean_off = RCR_VAL(e->rcr, RCR_MBTMEAN, e->mbtree_mean_off);
        e->mbtree_apply = 1;
    }
    e->cur_lr_motion = mscore;
    e->cur_lr_tdiff = e->lr_tdiff_ewma;
    e->rcp_cur_cme = e->rcp_arr_cme;
    e->rcp_cur_cvi = e->rcp_arr_cvi;
    e->cur_open_key = open_key ? 1 : ir_rp;
    int er = emit_pair(e, &off, is_key ? 0 : 1, is_idr, 1, src_planes);
    e->cur_open_key = 0;
    if (er < 0)
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
        if (!RCR_PLAY(e))
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
        size_t chalf = mc / 2;
        for (size_t i = 0; i < mc; i++) {
            int fp = e->fields ? (i >= chalf) : 0;
            const int *l0poc = e->fields ? e->fld_l0poc[fp] : e->cur_l0poc;
            int l0n = e->fields ? e->fld_l0n[fp] : e->cur_l0n;
            e->colpoc[i] = (int16_t)(e->refidx[i] >= 0 && e->refidx[i] < l0n
                                     ? l0poc[e->refidx[i]] : -1);
        }
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
        if (emit_pair(e, &off, 2, 0, 0, e->bplane[i]) < 0)
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
    e->ref0_fn = e->anchor_fn;
    if (e->ref_navail < 16) e->ref_navail++;

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

void yah264_encoder_close(yah264_encoder_t *e)
{
    if (s_lrrow_prof > 0 && e && e->la_depth > 0)
        fprintf(stderr, "lrrow_prof intra_ms=%.1f prev_ms=%.1f bwd_ms=%.1f (cumulative)\n",
                s_lrrow_ns[0] / 1e6, s_lrrow_ns[1] / 1e6, s_lrrow_ns[2] / 1e6);
    if (e && e->hw) { y264_hw_close(e->hw); hw_scenecut_free(e); free(e); return; }
    if (!e)
        return;
    rcr_close(e->rcr);
    e->rcr = NULL;
    if (e->rc_shared) {
        pthread_mutex_lock(&e->rc_shared->mx);
        e->rc_shared->nlive--;
        pthread_mutex_unlock(&e->rc_shared->mx);
        e->rc_shared = NULL;
    }
    y264_recs_cache_ref(-1);
    y264_me_stats_dump();           /* E2: prints only when Y264_ME_STATS is set */
    if (wp_stat_on())
        fprintf(stderr, "weightp-stat: %d P slices, %d carried a weight, "
                "%d spent a duplicate slot, %ld partitions took the escape\n",
                e->stat_wp_p, e->stat_wp_frame, e->stat_wp_dup,
                atomic_load_explicit(&y264_wp_esc, memory_order_relaxed));
    if (e->mbtp) {                  /* join before the pool: Phase A rides it */
        struct mbt_pre *mp = e->mbtp;
        pthread_mutex_lock(&mp->mx);
        while (mp->req && !mp->done)        /* let an unclaimed warm retire */
            pthread_cond_wait(&mp->cv_done, &mp->mx);
        mp->exit = 1;
        pthread_cond_broadcast(&mp->cv_req);
        pthread_mutex_unlock(&mp->mx);
        ntp_bg_sync(mp->bg);
        ntp_bg_destroy(mp->bg);
        if (getenv("Y264_MBT_PRE_DBG"))
            fprintf(stderr, "[mbt_pre] warms %ld, compute %.1f ms\n",
                    mp->hits, mp->ms_compute);
        pthread_mutex_destroy(&mp->mx);
        pthread_cond_destroy(&mp->cv_req);
        pthread_cond_destroy(&mp->cv_done);
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
        if (mbt_misswhy_env())
            fprintf(stderr, "[mbt_nobleg] anchor %ld untyped %ld null-leg %ld"
                    " no-have %ld\n",
                    (long)g_mbt_split.nb_anchor, (long)g_mbt_split.nb_untyped,
                    (long)g_mbt_split.nb_nulleg, (long)g_mbt_split.nb_nohave);
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
        plane_free(e->plane[c], pw[c], pb[c], e->pvmul);
        plane_free(e->rec[c], pw[c], pb[c], e->pvmul);
        free(e->nnz[c]);
    }
    /* e->ref aliases a refring slot (the buffers rotate), so free the ring.
 * rec/ring/ref1 labels are a permutation of the same allocations after the
 * pointer rotation; each label is freed exactly once (rec above). */
    for (int i = 0; i < (e->nref > 1 ? e->nref : 1); i++)
        for (int c = 0; c < 3; c++)
            plane_free(e->refring[i][c], pw[c], pb[c], e->pvmul);
    for (int c = 0; c < 3; c++)
        plane_free(e->ref1[c], pw[c], pb[c], e->pvmul);
    /* bplane <-> stair_chain::bspare is an exact swap (stair_launch), so the
 * two labels partition the same allocations; stair_free took its half. */
    for (int i = 0; i < e->bframes; i++)
        for (int c = 0; c < 3; c++)
            plane_free(e->bplane[i][c], pw[c], pb[c], e->pvmul);
    for (int i = 0; i < e->bframes && i < 8; i++) {
        free(e->blowres[i]); free(e->bmbtree_off[i]); free(e->bdintra[i]);
    }
    for (int i = 0; i < 17; i++)
        for (int c = 0; c < 3; c++)
            plane_free(e->hpel_buf[i][c], e->padded_w, Y264_LUMA_BORDER, e->pvmul);
    if (e->flat_hp_on) {
        for (int c = 0; c < 3; c++) {
            plane_free(e->rec_hp[c], e->padded_w, Y264_LUMA_BORDER, e->pvmul);
            plane_free(e->ref1_hp[c], e->padded_w, Y264_LUMA_BORDER, e->pvmul);
        }
        for (int i = 0; i < e->nref; i++)
            for (int c = 0; c < 3; c++)
                plane_free(e->ring_hp[i][c], e->padded_w, Y264_LUMA_BORDER, e->pvmul);
    }
    free(e->hpel_scratch);
    nr_close(e);
    y264_mcstf_close(e->mcstf);
    for (int w = 0; w < e->hpel_ws_n; w++) free(e->hpel_scratch_ws[w]);
    for (int i = 0; i < e->la_cap; i++) {
        for (int c = 0; c < 3; c++) plane_free(e->la[i].plane[c], pw[c], pb[c], e->pvmul);
        lr_hpreg_set(e->la[i].lowres, NULL);
        free(e->la[i].lowres);
        for (int k = 0; k < 3; k++) free(e->la[i].lr_hp[k]);
        free(e->la[i].d_intra);
        free(e->la[i].hint_lr); free(e->la[i].hint_di);
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
    free(e->bseedb_pend);
    for (int b = 0; b < 8; b++) free(e->bseedb[b]);
    free(e->la_prop_a); free(e->la_prop_b);
    free(e->lr_seed_mvx); free(e->lr_seed_mvy); free(e->lr_seed_cost);
    free(e->nal);
    free(e->slice_row0);
    free(e->slice_y0);
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
    free(e->mbvar);
    free(e->fld_aq);
    free(e->fld_mbt);
    free(e->zones);
    free(e->forces);
    free(e->mbqp);
    free(e->mb_tr8);
    if (e->lr_bufhp[0][0]) { lr_hpreg_set(e->lowres_cur, NULL); lr_hpreg_set(e->lowres_prev, NULL); }
    if (e->panchor_hp[0] && e->code_panchor_lr) lr_hpreg_set(e->code_panchor_lr, NULL);
    for (int i = 0; i < 8; i++) if (e->blowres_hp[i][0]) lr_hpreg_set(e->blowres[i], NULL);
    for (int k = 0; k < 3; k++) {
        free(e->lr_bufhp[0][k]); free(e->lr_bufhp[1][k]); free(e->panchor_hp[k]);
        for (int i = 0; i < 8; i++) free(e->blowres_hp[i][k]);
    }
    free(e->lowres_cur);
    free(e->lowres_prev);
    free(e->lr_intra); free(e->lr_inter); free(e->lr_mvx); free(e->lr_mvy);
    free(e->mbtree_off); free(e->lowres_tmp); free(e->code_panchor_lr);
    for (int g = 0; g < 2; g++)
        if (!e->lrset_init || e->lr_sub_obj[g] < 0)      /* a privately owned set */
            for (int p = 1; p < 16; p++) free(e->lr_subpel[g][p]);
    if (e->la_anchor_lr) lr_hpreg_set(e->la_anchor_lr, NULL);
    for (int k = 0; k < 3; k++) free(e->la_anchor_hp[k]);
    for (int i = 0; i < e->lrset_n; i++)                 /* the slots point into these */
        for (int p = 1; p < 16; p++) free(e->lrset[i].pl[p]);
    if (e->lrset_init) pthread_mutex_destroy(&e->lrset_mx);
    for (int w = 0; w < e->mbt_nws; w++) {
        for (int p = 1; p < 16; p++) { free(e->mbt_subpel[w][0][p]); free(e->mbt_subpel[w][1][p]); }
        free(e->mbt_lrtmp[w]); free(e->mbt_invq[w]); free(e->mbt_aqoff[w]);
    }
    if (e->invq_cache_init)
        for (int k = 0; k < 64; k++) {
            free(e->invq_cache[k].invq); free(e->invq_cache[k].aqoff);
            free(e->invq_cache[k].psw);
            pthread_mutex_destroy(&e->invq_cache[k].mx);
        }
    for (int i = 0; i < e->dpb_size; i++) {
        for (int c = 0; c < 3; c++) plane_free(e->dpb[i].plane[c], pw[c], pb[c], e->pvmul);
        for (int c = 0; c < 3; c++) if (e->dpb[i].hpel[c]) plane_free(e->dpb[i].hpel[c], e->padded_w, Y264_LUMA_BORDER, e->pvmul);
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
    free(e->hrd_pad);
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
    long *pex;                   /* exact previous-frame cost by slot, -1 = not computed (NULL = no cache) */
    int ipen;                    /* the lowres intra penalty (lr_intra_penalty) */
    int mask;                    /* frame i lives in slot i & mask: -1 = one slot per frame
                                  * (the whole-input scan), 2^k-1 = a ring (the streamed one) */
};
#define SCX(c, i) ((i) & (c)->mask)

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
        /* columns whose pair lies inside the plane need no clamp, and the
         * plain loop vectorises; the clamped tail is the padding */
        int xin = sw / 2 < dw ? sw / 2 : dw;
        for (int x = 0; x < xin; x++)
            d[x] = (pixel)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
        for (int x = xin; x < dw; x++) {
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
    sc_downscale(c->lr[SCX(c, i)], c->lr_w, c->lr_h, c->src[i], c->sstride[i], c->w, c->h);
}

/* Per-frame intra sum and the ZERO-MV inter sum (the screen's upper bound).
 * Frame 0 has no predecessor and takes pp = ii, which is what la_lr_row does
 * when la_have_prev is 0. */
static void sc_cost_task(void *ctx, int tid, int i)
{
    struct sc_scan *c = ctx;
    (void)tid;
    const pixel *cur = c->lr[SCX(c, i)], *prev = i > 0 ? c->lr[SCX(c, i - 1)] : NULL;
    int32_t *di = c->dintra[SCX(c, i)];
    long ic = 0, pz = 0;
    for (int my = 0; my < c->hmb; my++)
        for (int mx = 0; mx < c->wmb; mx++) {
            size_t off = (size_t)(my * 8) * c->lr_w + mx * 8;
            const pixel *sb = cur + off;
            long ii = blk8_intra_dispatch_p(sb, c->lr_w, mx, my, prev ? prev + off : NULL, LR_I10_MODE, c->ipen);
            long pp = prev ? blk8_satd(sb, c->lr_w, prev + off, c->lr_w) : ii;
            di[my * c->wmb + mx] = (int32_t)ii;
            ic += ii;
            pz += ii < pp ? ii : pp;
        }
    c->icost[SCX(c, i)] = ic;
    c->pzero[SCX(c, i)] = pz;
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
    struct sc_exact x = { c, c->lr[SCX(c, cur)], c->lr[SCX(c, ref)], c->dintra[SCX(c, cur)], rowpc };
    if (pool && ntp_pool_nthreads(pool) > 1 && c->hmb > 1)
        ntp_parallel_for(pool, c->hmb, sc_exact_row, &x);
    else
        for (int my = 0; my < c->hmb; my++) sc_exact_row(&x, 0, my);
    long pc = 0;
    for (int my = 0; my < c->hmb; my++) pc += rowpc[my];
    return pc;
}

/* Frame i's exact cost against frame i-1, computed once: the jump test reads
 * it again for each of the next three frames that reach the full test. */
static long sc_prev_pcost(struct sc_scan *c, ntp_pool_t *pool, long *rowpc, int i)
{
    if (!c->pex) return sc_exact_pcost(c, pool, rowpc, i, i - 1);
    long *v = &c->pex[SCX(c, i)];
    if (*v < 0) *v = sc_exact_pcost(c, pool, rowpc, i, i - 1);
    return *v;
}

/* One frame of la_finalize's state machine, replayed: frame i's IDR decision
 * (returned) and whether it was a cut (*raw_out). Reads frames i-4..i+1 of the
 * scan (i+1 only when has_next), and cleared[] by slot, so the whole-input
 * scan and the streamed one run the same arithmetic on the same values. The
 * screen is the real decision run on the upper bound: scenecut_decide is
 * monotonic in pcost, and pcost_true <= pzero, so a frame the bound cannot
 * cut is a frame no diamond could cut. That makes the screen exactly as tight
 * as the test it guards -- it uses this frame's own bias instead of a
 * worst-case constant -- and still cannot reject a real cut. */
struct sc_state { int since_idr, have_prev_fin; };
static int sc_step(struct sc_scan *c, ntp_pool_t *pool, long *rowpc,
                   const struct sc_cfg *sc, int bframes, int i, int has_next,
                   struct sc_state *st, unsigned char *cleared, int *raw_out)
{
    int raw = 0;
    long ic = c->icost[SCX(c, i)];
    if (!sc->off && st->have_prev_fin && !cleared[SCX(c, i)] &&
        st->since_idr >= sc->keyint_min &&
        scenecut_decide(sc, ic, c->pzero[SCX(c, i)], st->since_idr)) {
        long pc = sc_prev_pcost(c, pool, rowpc, i);
        raw = scenecut_decide(sc, ic, pc, st->since_idr);
        if (raw) {
            double rh[3]; int nh = 0;
            for (int j = 1; j <= 3 && i - j >= 1; j++)
                if (c->icost[SCX(c, i - j)] > 0)
                    rh[nh++] = (double)sc_prev_pcost(c, pool, rowpc, i - j)
                             / (double)c->icost[SCX(c, i - j)];
            if (!sc_jump_ok((double)pc / (double)ic, rh, nh, ic,
                            i - 1 >= 1 ? c->icost[SCX(c, i - 1)] : -1)) raw = 0;
        }
    }
    if (raw && bframes > 0 && has_next && i >= 1) {
        /* Flash guard: does the successor still predict from the pre-cut
         * frame? Then the candidate was a flash, not a cut. */
        long pc1 = sc_exact_pcost(c, pool, rowpc, i + 1, i - 1);
        if (!scenecut_decide(sc, c->icost[SCX(c, i + 1)], pc1, st->since_idr + 1)) {
            raw = 0;
            cleared[SCX(c, i + 1)] = 1;
        }
    }
    if (raw) st->since_idr = 0;
    int since = st->since_idr;
    st->since_idr = (st->since_idr + 1) % sc->keyint;
    st->have_prev_fin = 1;
    *raw_out = raw;
    return since == 0;
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
    c.ipen = lr_intra_penalty(param);
    c.src = luma; c.sstride = stride;
    c.mask = -1;

    size_t lrsz = (size_t)c.lr_w * c.lr_h;
    int nmb = c.wmb * c.hmb;
    c.lr = calloc((size_t)n, sizeof(*c.lr));
    c.dintra = calloc((size_t)n, sizeof(*c.dintra));
    c.icost = calloc((size_t)n, sizeof(*c.icost));
    c.pzero = calloc((size_t)n, sizeof(*c.pzero));
    c.pex = malloc((size_t)n * sizeof(*c.pex));
    if (c.pex) for (int i = 0; i < n; i++) c.pex[i] = -1;
    long *rowpc = calloc((size_t)c.hmb, sizeof(*rowpc));
    unsigned char *cleared = calloc((size_t)n, 1);
    int ok = c.lr && c.dintra && c.icost && c.pzero && c.pex && rowpc && cleared;
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
        struct sc_state st = { 0, 0 };
        int nidr = 0;
        for (int i = 0; i < n; i++) {
            int raw;
            idr[i] = (unsigned char)sc_step(&c, pool, rowpc, &sc, param->bframes, i, i + 1 < n,
                                            &st, cleared, &raw);
            nidr += idr[i];
            if (cut_out) cut_out[i] = (unsigned char)(raw || i == 0);
            if (icost_out) icost_out[i] = c.icost[i];
            if (pcost_out) pcost_out[i] = c.pzero[i];
        }
        rc = nidr;
    }

    if (pool) ntp_pool_destroy(pool);
    for (int i = 0; i < n; i++) {
        if (c.lr) free(c.lr[i]);
        if (c.dintra) free(c.dintra[i]);
    }
    free(c.lr); free(c.dintra); free(c.icost); free(c.pzero); free(c.pex);
    free(rowpc); free(cleared);
    return rc;
}

/* The streamed pre-scan (docs/threading.md): the same per-frame
 * step over a ring of eight frames instead of the whole clip, so a GOP split
 * on the cuts needs neither the input's length nor all of it resident. A
 * frame's decision needs its successor (the flash guard), so it lands one
 * push late; flush settles the last frame. The decisions are the whole-input
 * scan's, frame for frame: same costs, same step, and the step never reads
 * further back than frame i-4 or further on than i+1. */
#define SCS_RING 8
struct yah264_cutscan {
    struct sc_scan c;
    struct sc_cfg cfg;
    struct sc_state st;
    int bframes, n;
    pixel *lr[SCS_RING];
    int32_t *dintra[SCS_RING];
    long icost[SCS_RING], pzero[SCS_RING], pex[SCS_RING];
    unsigned char cleared[SCS_RING];
    long *rowpc;
};

void yah264_cutscan_close(yah264_cutscan_t *s)
{
    if (!s) return;
    for (int i = 0; i < SCS_RING; i++) { free(s->lr[i]); free(s->dintra[i]); }
    free(s->rowpc);
    free(s);
}

yah264_cutscan_t *yah264_cutscan_open(const yah264_param_t *param, int depth)
{
    if (!param || depth != Y264_BIT_DEPTH || param->width <= 0 || param->height <= 0)
        return NULL;
    y264_dsp_init();
    warm_lr_statics();
    yah264_cutscan_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    struct sc_scan *c = &s->c;
    c->w = param->width; c->h = param->height;
    c->wmb = (param->width + 15) / 16;
    c->hmb = (param->height + 15) / 16;
    c->lr_w = c->wmb * 16 / 2;
    c->lr_h = c->hmb * 16 / 2;
    c->ipen = lr_intra_penalty(param);
    c->mask = SCS_RING - 1;
    c->lr = s->lr; c->dintra = s->dintra;
    c->icost = s->icost; c->pzero = s->pzero; c->pex = s->pex;
    size_t lrsz = (size_t)c->lr_w * c->lr_h, nmb = (size_t)c->wmb * c->hmb;
    int ok = (s->rowpc = calloc((size_t)c->hmb, sizeof(long))) != NULL;
    for (int i = 0; ok && i < SCS_RING; i++) {
        s->lr[i] = malloc(lrsz * sizeof(pixel));
        s->dintra[i] = malloc(nmb * sizeof(int32_t));
        ok = s->lr[i] && s->dintra[i];
    }
    if (!ok) { yah264_cutscan_close(s); return NULL; }
    s->cfg = sc_cfg_of(param);
    s->bframes = param->bframes;
    return s;
}

int yah264_cutscan_push(yah264_cutscan_t *s, const void *luma, int stride)
{
    if (!s || !luma) return -2;
    int i = s->n;
    sc_downscale(s->lr[SCX(&s->c, i)], s->c.lr_w, s->c.lr_h, (const pixel *)luma, stride,
                 s->c.w, s->c.h);
    sc_cost_task(&s->c, 0, i);
    s->cleared[SCX(&s->c, i)] = 0;
    s->pex[SCX(&s->c, i)] = -1;
    s->n = i + 1;
    if (i == 0) return -1;
    int raw;
    return sc_step(&s->c, NULL, s->rowpc, &s->cfg, s->bframes, i - 1, 1,
                   &s->st, s->cleared, &raw);
}

/* The lowres plane and per-block intra costs of the frame pushed last, for
 * yah264_encoder_lowres_hint. Valid until the eighth push after it. */
int yah264_cutscan_lowres(const yah264_cutscan_t *s, const void **lowres, const int32_t **intra)
{
    if (!s || s->n == 0 || !lowres || !intra) return -1;
    int k = SCX(&s->c, s->n - 1);
    *lowres = s->lr[k];
    *intra = s->dintra[k];
    return 0;
}

int yah264_cutscan_flush(yah264_cutscan_t *s)
{
    if (!s || s->n == 0) return -1;
    int raw;
    return sc_step(&s->c, NULL, s->rowpc, &s->cfg, s->bframes, s->n - 1, 0,
                   &s->st, s->cleared, &raw);
}

/* The whole-input scan's answer, IDR map and shot table, from frames pushed
 * one batch at a time (docs/options.md, "Memory"). The same step on the same
 * costs as scan_core, over a ring sized to the batch, and the shot sums
 * accumulate in frame order as yah264_scan_shots adds them, so every field
 * comes out bit for bit equal. What it keeps per frame is one
 * byte of IDR map, so a title costs its frame count in bytes, not its pixels. */
struct yah264_shotscan {
    struct sc_scan c;
    struct sc_cfg cfg;
    struct sc_state st;
    int bframes, n, batch, finished;
    long *rowpc;
    unsigned char *cleared;
    ntp_pool_t *pool;
    const pixel *const *bsrc;       /* the batch being pushed, frames n..n+k-1 */
    const int *bstride;
    unsigned char *idr;
    int idr_cap, nidr;
    yah264_shot_t *shots;
    int nshots, shots_cap;
    int shot_first;                 /* the open shot's first frame */
    double si, sp, peak;
    int np;
};

void yah264_shotscan_close(yah264_shotscan_t *s)
{
    if (!s) return;
    if (s->pool) ntp_pool_destroy(s->pool);
    int ring = s->c.mask + 1;
    for (int i = 0; i < ring; i++) {
        if (s->c.lr) free(s->c.lr[i]);
        if (s->c.dintra) free(s->c.dintra[i]);
    }
    free(s->c.lr); free(s->c.dintra); free(s->c.icost); free(s->c.pzero); free(s->c.pex);
    free(s->rowpc); free(s->cleared); free(s->idr); free(s->shots);
    free(s);
}

yah264_shotscan_t *yah264_shotscan_open(const yah264_param_t *param, int depth,
                                        int nthreads, int max_batch)
{
    if (!param || depth != Y264_BIT_DEPTH || param->width <= 0 || param->height <= 0)
        return NULL;
    if (max_batch < 1) max_batch = 1;
    y264_dsp_init();
    warm_lr_statics();
    yah264_shotscan_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    struct sc_scan *c = &s->c;
    c->w = param->width; c->h = param->height;
    c->wmb = (param->width + 15) / 16;
    c->hmb = (param->height + 15) / 16;
    c->lr_w = c->wmb * 16 / 2;
    c->lr_h = c->hmb * 16 / 2;
    c->ipen = lr_intra_penalty(param);
    /* the step reads back to frame i-4 and on to i+1, and a push lands a
     * whole batch before stepping the frame ahead of it */
    int ring = 8;
    while (ring < max_batch + 8) ring <<= 1;
    c->mask = ring - 1;
    s->batch = max_batch;
    size_t lrsz = (size_t)c->lr_w * c->lr_h, nmb = (size_t)c->wmb * c->hmb;
    c->lr = calloc((size_t)ring, sizeof(*c->lr));
    c->dintra = calloc((size_t)ring, sizeof(*c->dintra));
    c->icost = calloc((size_t)ring, sizeof(long));
    c->pzero = calloc((size_t)ring, sizeof(long));
    c->pex = calloc((size_t)ring, sizeof(long));
    s->cleared = calloc((size_t)ring, 1);
    s->rowpc = calloc((size_t)c->hmb, sizeof(long));
    int ok = c->lr && c->dintra && c->icost && c->pzero && c->pex && s->cleared && s->rowpc;
    for (int i = 0; ok && i < ring; i++) {
        c->lr[i] = malloc(lrsz * sizeof(pixel));
        c->dintra[i] = malloc(nmb * sizeof(int32_t));
        ok = c->lr[i] && c->dintra[i];
    }
    if (ok && nthreads > 1) s->pool = ntp_pool_create(nthreads);
    if (!ok) { yah264_shotscan_close(s); return NULL; }
    s->cfg = sc_cfg_of(param);
    s->bframes = param->bframes;
    return s;
}

static void ss_down_task(void *ctx, int tid, int k)
{
    yah264_shotscan_t *s = ctx;
    struct sc_scan *c = &s->c;
    (void)tid;
    sc_downscale(c->lr[SCX(c, s->n + k)], c->lr_w, c->lr_h, s->bsrc[k], s->bstride[k], c->w, c->h);
}

static void ss_cost_task(void *ctx, int tid, int k)
{
    yah264_shotscan_t *s = ctx;
    sc_cost_task(&s->c, tid, s->n + k);
}

/* yah264_scan_shots' per-shot arithmetic, one frame at a time in the same
 * order, so the doubles round the same way. */
static void ss_close_shot(yah264_shotscan_t *s, int end)
{
    if (s->nshots == s->shots_cap) {
        int cap = s->shots_cap ? 2 * s->shots_cap : 64;
        yah264_shot_t *p = realloc(s->shots, (size_t)cap * sizeof *p);
        if (!p) { s->nshots = -1; return; }
        s->shots = p; s->shots_cap = cap;
    }
    yah264_shot_t *sh = &s->shots[s->nshots++];
    memset(sh, 0, sizeof *sh);
    sh->first = s->shot_first; sh->last = end - 1;
    sh->icost_mean = s->si / (end - s->shot_first);
    sh->icost_peak = s->peak;
    sh->pcost_mean = s->np ? s->sp / s->np : sh->icost_mean;
    sh->ratio = sh->icost_mean > 0 ? sh->pcost_mean / sh->icost_mean : 0.0;
}

static int ss_step(yah264_shotscan_t *s, int i, int has_next)
{
    if (i >= s->idr_cap) {
        int cap = s->idr_cap ? 2 * s->idr_cap : 4096;
        while (cap <= i) cap *= 2;
        unsigned char *p = realloc(s->idr, (size_t)cap);
        if (!p) return -1;
        s->idr = p; s->idr_cap = cap;
    }
    int raw;
    int d = sc_step(&s->c, s->pool, s->rowpc, &s->cfg, s->bframes, i, has_next,
                    &s->st, s->cleared, &raw);
    s->idr[i] = (unsigned char)d;
    s->nidr += d;
    if (i > 0 && raw) {
        ss_close_shot(s, i);
        if (s->nshots < 0) return -1;
        s->shot_first = i;
        s->si = s->sp = s->peak = 0; s->np = 0;
    }
    double ic = (double)s->c.icost[SCX(&s->c, i)];
    s->si += ic; if (ic > s->peak) s->peak = ic;
    if (i > s->shot_first) { s->sp += (double)s->c.pzero[SCX(&s->c, i)]; s->np++; }
    return 0;
}

int yah264_shotscan_push(yah264_shotscan_t *s, const void *const *luma,
                         const int *stride, int k)
{
    if (!s || s->finished || !luma || !stride || k < 1 || k > s->batch) return -1;
    for (int j = 0; j < k; j++) if (!luma[j]) return -1;
    s->bsrc = (const pixel *const *)luma;
    s->bstride = stride;
    if (s->pool && ntp_pool_nthreads(s->pool) > 1 && k > 1) {
        ntp_parallel_for(s->pool, k, ss_down_task, s);
        ntp_parallel_for(s->pool, k, ss_cost_task, s);
    } else {
        for (int j = 0; j < k; j++) ss_down_task(s, 0, j);
        for (int j = 0; j < k; j++) ss_cost_task(s, 0, j);
    }
    s->bsrc = NULL; s->bstride = NULL;
    int base = s->n;
    for (int j = 0; j < k; j++) {
        s->cleared[SCX(&s->c, base + j)] = 0;
        s->c.pex[SCX(&s->c, base + j)] = -1;
    }
    s->n = base + k;
    /* every frame with a successor in hand is decidable now */
    for (int i = base > 0 ? base - 1 : 0; i < s->n - 1; i++)
        if (ss_step(s, i, 1) != 0) return -1;
    return 0;
}

int yah264_shotscan_finish(yah264_shotscan_t *s)
{
    if (!s || s->n == 0) return -1;
    if (!s->finished) {
        if (ss_step(s, s->n - 1, 0) != 0) return -1;
        ss_close_shot(s, s->n);
        if (s->nshots < 0) return -1;
        s->finished = 1;
    }
    return s->n;
}

int yah264_shotscan_idr(const yah264_shotscan_t *s, unsigned char *idr, int n)
{
    if (!s || !s->finished || !idr || n != s->n) return -1;
    memcpy(idr, s->idr, (size_t)n);
    return s->nidr;
}

int yah264_shotscan_shots(const yah264_shotscan_t *s, yah264_shot_t *shots, int max_shots)
{
    if (!s || !s->finished) return -1;
    if (shots && max_shots > 0)
        memcpy(shots, s->shots, (size_t)(max_shots < s->nshots ? max_shots : s->nshots) * sizeof *shots);
    return s->nshots;
}

/* Everything the scan's answer depends on besides the pixels, resolved the
 * way the scan resolves it, so a saved scan can be refused when any of it
 * moved. B frames enter only as on/off: the flash guard is all that reads them. */
/* the shortest of %.15g and %.17g that reads back as the same double */
static const char *ss_dbl(char *b, size_t cap, double v)
{
    snprintf(b, cap, "%.15g", v);
    if (strtod(b, NULL) != v) snprintf(b, cap, "%.17g", v);
    return b;
}

int yah264_shotscan_signature(const yah264_param_t *param, char *buf, int cap)
{
    if (!param || !buf || cap <= 0) return -1;
    warm_lr_statics();
    struct sc_cfg sc = sc_cfg_of(param);
    char th[32], ju[32], fl[32];
    int n = snprintf(buf, (size_t)cap,
                     "depth=%d size=%dx%d keyint=%d min-keyint=%d cuts=%s threshold=%s "
                     "flash-guard=%d jump=%s floor=%s lr-intra=%d/%d/%d lr-prev-shape=%d",
                     Y264_BIT_DEPTH, param->width, param->height, sc.keyint, sc.keyint_min,
                     sc.off ? "off" : "on", ss_dbl(th, sizeof th, sc.thresh_max),
                     param->bframes > 0, ss_dbl(ju, sizeof ju, SC_JUMP),
                     ss_dbl(fl, sizeof fl, SC_FLOOR), 1,
                     LR_I10_MODE, lr_intra_penalty(param), 1);
    return n < 0 || n >= cap ? -1 : n;
}

int yah264_scan_idr_frames(const yah264_param_t *param,
                            const void *const *luma, const int *stride,
                            int n, int nthreads, int depth, unsigned char *idr)
{
    if (depth != Y264_BIT_DEPTH) return -1;
    return scan_core(param, (const pixel *const *)luma, stride, n, nthreads,
                     idr, NULL, NULL, NULL);
}

/* The shot table (docs/shot-based.md): the pre-scan's
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

/* The hardware mode (docs/hardware-mode.md). Y264_HW: off | auto |
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

/* The caller's struct must be this build's struct (ABI 3).
 *
 * Checked here rather than in encoder_open_sw so that the hardware path and
 * every entry point above it are covered by the one test, and before anything
 * reads a second field: past `size`, nothing in a stale layout is what it
 * says it is. A caller that ran yah264_param_default() from this library
 * cannot fail it; one that did not is reading some other header's struct, and
 * the 2026-09-19 board is what that costs when it is allowed to proceed. */
static int param_size_ok(const yah264_param_t *param)
{
    if (param->size == (int)sizeof(*param))
        return 1;
    fprintf(stderr,
            "yah264: parameter struct mismatch -- the caller passed size %d, "
            "this library was built with %d (yah264.h ABI %d).\n"
            "yah264: the caller was compiled against a different "
            "include/yah264.h. Rebuild it against this one, and make sure it "
            "calls yah264_param_default() on the struct before open.\n",
            param->size, (int)sizeof(*param), YAH264_ABI_VERSION);
    return 0;
}

yah264_encoder_t *yah264_encoder_open_hw(const yah264_param_t *param, int hw)
{
    if (!param) return NULL;
    if (!param_size_ok(param)) return NULL;
    if (hw != YAH264_HW_OFF) {
        char why[160] = "";
        y264_dsp_init();                /* the scene-cut probe runs lowres kernels */
        warm_lr_statics();
        struct y264_hw *h = y264_hw_open(param, why, sizeof why);
        if (h) {
            yah264_encoder_t *e = calloc(1, sizeof *e);
            if (!e) { y264_hw_close(h); return NULL; }
            e->param = *param;
            deblock_auto_resolve(&e->param);
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
    yah264_param_t sized = *param;
    size_trades(&sized);
    return encoder_open_sw(&sized);
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
 * step 2, item 4; --no-scenecut turns it off). The same
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
static int hw_scenecut_probe(yah264_encoder_t *e, const yah264_picture_t *pic)
{
    if (!pic || e->param.scenecut < 0) return 0;
    struct hw_sc *h = e->hw_sc;
    if (!h) {
        h = calloc(1, sizeof *h);
        if (!h) return 0;
        h->c.w = e->param.width; h->c.h = e->param.height;
        h->c.wmb = (h->c.w + 15) / 16; h->c.hmb = (h->c.h + 15) / 16;
        h->c.lr_w = h->c.wmb * 8; h->c.lr_h = h->c.hmb * 8; h->c.ipen = e->lr_intra_pen;
        size_t lrsz = (size_t)h->c.lr_w * h->c.lr_h, nmb = (size_t)h->c.wmb * h->c.hmb;
        for (int i = 0; i < 2; i++) { h->lr[i] = malloc(lrsz * sizeof(pixel)); h->dintra[i] = malloc(nmb * sizeof(int32_t)); }
        h->rowpc = calloc((size_t)h->c.hmb, sizeof(long));
        if (!h->lr[0] || !h->lr[1] || !h->dintra[0] || !h->dintra[1] || !h->rowpc) { hw_scenecut_free(e); return 0; }
        h->c.lr = h->lr; h->c.dintra = h->dintra; h->c.mask = -1;
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
                long ii = blk8_intra_dispatch_p(sb, h->c.lr_w, mx, my, h->lr[prev] + off, LR_I10_MODE, e->lr_intra_pen);
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
