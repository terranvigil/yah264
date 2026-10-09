// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Rate control: the frame-level complexity inputs, the bits models, the
// pipelined decide/account (rcp), the VBV clip, burst gate and plan, the CRF
// head, the RF2 opening and the row-brake budget note. Moved out of encoder.c
// on 2026-10-04 (v2 subsystems item 3, step 3c); ratecontrol.h is the
// interface both files share.

#include <limits.h>
#include "../common/iprof.h"
#include "rcreplay.h"
#include "encoder.h"
#include "ratecontrol.h"
#include "../dsp/mc.h"
#include "../common/threadpool.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

/* Does anything read the zero-motion complexity below this frame? The ABR
 * and VBV bits models, the 2-pass stats line and the debug print do; CRF on
 * its own reads only the lowres one, so it skips the full-resolution pass. */
static int rc_cplx_wanted(const yah264_encoder_t *e)
{
    return e->abr_on || e->vbv_on || e->tp_pass || e->tp_fp || e->rck.dbg_cplx;
}

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
 * motion with coding difficulty and reads foreman ~= ducks), this is what the reference encoder's
 * CRF uses -- it reflects post-motion-compensation cost, so hard and easy content
 * separate and the qcomp curve can actually spread QP the way the reference encoder does. */
double frame_complexity_me(const yah264_encoder_t *e)
{
    long total = 0;
    int n = e->width_in_mbs * e->height_in_mbs;
    for (int i = 0; i < n; i++) {
        int ic = e->lr_intra[i], pc = e->lr_inter[i];
        total += ic < pc ? ic : pc;
    }
    return (double)total + 1.0;
}

/* The arriving anchor's lowres min(intra, inter) cost against the previous
 * anchor, from the lookahead's anchor leg; 0 when there is none. */
double anchor_leg_cost(const yah264_encoder_t *e)
{
    const struct la_entry *ae = e->cur_la_en;
    if (!ae || !ae->is_anchor || !ae->aleg_have || !ae->leg[LR_LEG_ANCHOR] || !ae->d_intra)
        return 0.0;
    long ca = 0;
    int n = e->width_in_mbs * e->height_in_mbs;
    for (int i = 0; i < n; i++) {
        long ic = ae->d_intra[i], pc = ae->leg[LR_LEG_ANCHOR][i].d_inter;
        ca += ic < pc ? ic : pc;
    }
    return (double)ca;
}

/* Lowres INTRA cost sum: pure source energy, no recon, never collapses. The
 * rcp VBV bits model runs on this -- min(intra, inter) reads ~0 on static or
 * noise-vs-motion content (samsung's sparkle anchors: cme ~tiny, actual 3
 * Mbit) and a blind model is exactly what VBV cannot afford. Level
 * differences per type (a B at the same energy costs far less than a P) are
 * absorbed by the per-type scale calibration. */
double frame_complexity_ilr(const yah264_encoder_t *e)
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
 * sintel -0.36%, and the other eight clips byte-identical. Always on, at
 * 0.2 x cvi (the Y264_ABR_CFLOOR / _CFLOORF overrides are gone). */


/* The full reference-encoder ABR model: rate factor for P, P-track anchors for I and B.
 * All three go in together: the P-track anchors alone fail when layered on a
 * model with no rate factor under them. DEFAULT OFF until the corpus gates
 * it. */
int abr_cguard_on(void)
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

double abr_cguard_thresh(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_ABR_CGUARD_T"); v = s ? atof(s) : 4.0; }
    return v;
}

/* Env override of the public param.rc.abr_model. -1 = unset (use the param). */
int abr_rf_env(void)
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
 * byte-identical. Default inert. */
int abr_rfqp_trace(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_ABR_RFQP"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}
/* The RF2 model (plan step A2): ABR as the CRF path plus a slowly adapted
 * rate factor (plan work item A). The same
 * design the reference encoder documents for single-pass ABR under mb-tree, written
 * independently from that description, from no other encoder's code: the frame
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
 * No per-frame lstep clip (A3), no CFLOOR / CGUARD, no ABR scale. Needs
 * mb-tree on (bframes > 0 or the IPPPP mb-tree); otherwise it logs and
 * falls back to the previous model.
 * DEFAULT ON (2026-09-02, plan A5) on the 29-clip table
 * (local/records/a4-abr-table-2026-09-02.md): ABR vs the reference encoder's ABR at the reference encoder's
 * achieved rates, medians CIF -4.1% / 720p -8.2% / 1080p -1.4% where the
 * previous model read -0.5 / +14.7 / +8.8; ABR-vs-own-CRF tax 5.5 / 9.4 /
 * 6.9% against the reference encoder's 3.5 / 4.4 / 3.8 and the previous model's 10 / 33 / 18.
 * Behind the previous model only at the top of the rate range (riverbed,
 * crowd_run); the rate-keyed B cascade later shipped (abr_pbrate_scale). The
 * Y264_ABR_RF2 escape is gone; without mb-tree the previous model still runs. */


/* Y264_RC_SHARE_OVN (default on): each instance on a shared ledger takes its
 * share of the overflow window (see rcp_decide). Warmed static. */
int rc_share_ovn_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RC_SHARE_OVN"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

static int rc_shared_nlive(const yah264_encoder_t *e)
{
    struct yah264_rc_shared *s = e->rc_shared;
    pthread_mutex_lock(&s->mx);
    int n = s->nlive;
    pthread_mutex_unlock(&s->mx);
    return n > 1 ? n : 1;
}

static void rc_shared_add(yah264_encoder_t *e, double cplx, double wanted,
                          double actual, double target)
{
    struct yah264_rc_shared *s = e->rc_shared;
    pthread_mutex_lock(&s->mx);
    s->cplx_sum += cplx; s->wanted += wanted;
    s->actual += actual; s->target += target;
    pthread_mutex_unlock(&s->mx);
}

static void rc_shared_get(const yah264_encoder_t *e, double *cplx, double *wanted,
                          double *actual, double *target)
{
    struct yah264_rc_shared *s = e->rc_shared;
    pthread_mutex_lock(&s->mx);
    *cplx = s->cplx_sum; *wanted = s->wanted;
    *actual = s->actual; *target = s->target;
    pthread_mutex_unlock(&s->mx);
}

/* --ratetol: how far ABR may drift from its target before the correction term
 * answers, as a fraction of the target bitrate. Resolved at open (e->ratetol)
 * from param.ratetol, with Y264_ABR_TOL overriding it -- the env knob keeps
 * winning over the flag, as every other pair does. 1.0 is the value every ABR
 * encode used before the flag existed, so the default is byte-identical to it;
 * an infinite tolerance makes the correction factor exactly 1 and takes the
 * term out. */
static double abr_tol(const yah264_encoder_t *e)
{
    return e->ratetol > 0.0 ? e->ratetol : 1.0;
}

/* Bounded, sqrt(t)-damped correction. Only a nudge: the rate factor does the
 * converging, so this term alone undershoots. err and wanted are bits. */
static double abr_overflow(const yah264_encoder_t *e, double err, double wanted)
{
    double bps = e->abr_target_bpf * (e->abr_fps > 0 ? e->abr_fps : 25.0);
    if (bps <= 0) return 1.0;
    double buf = 2.0 * abr_tol(e) * bps;
    double t = wanted / bps;
    if (t > 1.0) buf *= sqrt(t);
    double ov = 1.0 + err / buf;
    /* the reference encoder clips symmetrically to [0.5, 2]. The DOWNWARD side is what hurts on
 * content that is free for a while: sintel's ~13 near-black opening frames
 * cost 38 bytes each while the time-based target accrues, so the overflow
 * reads a large surplus and spends it by lowering QP -- and the real content
 * then arrives at that lowered QP. Y264_ABR_OVLO tightens only that side. */
    /* 0.8: sintel-900 +12.3% -> +9.4% and the sweep saturates there (0.9
     * reads +9.2); bit-identical on five controls including the
     * undershooters (akiyo, stefan). Only the rf model calls this, so the
     * default path never sees it. */
    double lo = e->rck.abr_ovlo;
    if (ov < lo) ov = lo;
    if (ov > 2.0) ov = 2.0;   /* upside proven inert: raising it to 4 or 8
 * changes nothing, so the overflow is NOT
 * saturating and the overshoot is upstream. */
    return ov;
}

/* The decayed P-QP track (the reference encoder keeps the same), non-B only: our B QP comes from
 * last_ref_qp, so feeding B back in would drag the I anchor toward the B level.
 * An I contributes qp + ip_offset to stay in the P domain. */
static void abr_track_update(yah264_encoder_t *e, double qp, int type)
{
    if (type == 2) return;
    e->ptrack_qp *= 0.95;
    e->ptrack_norm *= 0.95;
    e->ptrack_norm += 1.0;
    e->ptrack_qp += qp + (type == 0 ? e->rck.abr_ipoff : 0.0);
    e->last_nonb_type = type;
    e->last_ref_qp[1] = e->last_ref_qp[0];
    e->last_ref_qp[0] = qp;
}

/* --- the bits model (struct rc_bmodel, encoder.h) ---------------------- */

/* The type's k, else P's, else I's, else B's (a type not yet calibrated
 * borrows the nearest one), else `none`. */
static double rcbm_k(const struct rc_bmodel *m, int type, double none)
{
    if (m->have[type]) return m->k[type];
    if (m->have[1]) return m->k[1];           /* prefer P */
    if (m->have[0]) return m->k[0];
    if (m->have[2]) return m->k[2];
    return none;
}

/* The update every model uses for a per-frame ratio: a warm value moves 30%
 * toward the new measurement, a cold one snaps to it. */
static inline double rcbm_ema(int warm, double old, double v)
{
    return warm ? 0.7 * old + 0.3 * v : v;
}

/* ...and for a decayed sum (the complexity-weighted form): the old sum keeps
 * 70% and the new term adds in whole. */
static inline double rcbm_sum(int warm, double old, double v)
{
    return warm ? 0.7 * old + v : v;
}

/* --- the buffer ledger (struct rc_buffer, encoder.h) ------------------- */

/* One picture's interval, netted: deliver b->rate, remove `bits`, clamp to
 * [0, size]. The committed account and the pipeline's virtual fill walk use
 * it. *empty (when not NULL) reports a drain the clamp at 0 absorbed. */
static inline double rcbuf_step(const struct rc_buffer *b, double fill, double bits,
                                int *empty)
{
    fill += b->rate - bits;
    if (fill > b->size) fill = b->size;
    if (fill < 0) { fill = 0; if (empty) *empty = 1; }
    return fill;
}

/* The second half of a forward walk that removes a picture's bits first (so
 * the caller can test the low point) and then refills: deliver b->rate and
 * clamp at full. The burst gate and the burst plan use it. */
static inline double rcbuf_refill(const struct rc_buffer *b, double fill)
{
    fill += b->rate;
    if (fill > b->size) fill = b->size;
    return fill;
}

/* What would overflow if a picture of no bits were removed now: delivery past
 * the capacity. CBR spends it; capped VBR lets the delivery pause. */
static inline double rcbuf_overflow(const struct rc_buffer *b, double fill)
{
    return fill + b->rate - b->size;
}

/* --- the step limiters ------------------------------------------------- */

/* Between consecutive frames of one type, in the qscale domain: at most one
 * lstep up or down from the type's last qscale, widened by one more step on
 * the side the rate error asks for (widen_hi when over budget, else
 * widen_lo when under). The rate-factor decides and the 2-pass plan use it. */
double rcstep_qscale(double q, double last, double lstep,
                                   int widen_hi, int widen_lo)
{
    double lo = last / lstep, hi = last * lstep;
    if (widen_hi) hi *= lstep;
    else if (widen_lo) lo /= lstep;
    if (q < lo) q = lo;
    if (q > hi) q = hi;
    return q;
}

/* In the QP domain, between bounds the caller derives from a reference: the
 * running ABR QP's swing, or the regime a bits model was calibrated at. */
static inline double rcstep_qp(double qp, double lo, double hi)
{
    if (qp < lo) qp = lo;
    if (qp > hi) qp = hi;
    return qp;
}

/* The lowest QP a decide may take against a model's calibrated regime: the
 * model cannot extrapolate far below the QP it was measured at. */
static inline int rcbm_qp_floor(const struct rc_bmodel *m, int type, int qpd)
{
    return (int)lround(m->qp[type]) - qpd;
}

static double abr_scale_for(yah264_encoder_t *e, int type)
{
    return rcbm_k(&e->bm_abr, type, 1.0);
}



/* Constant rate factor on absolute complexity: the base QP tracks the
 * blurred anchor complexity against a resolution-scaled constant,
 * qp = crf + SLOPE * log2(Cblur / (BASE * mbs)), so complex clips run a higher
 * average QP at the same CRF and easy clips a lower one (qcompress).
 *
 * Cblur is the ME-compensated lowres cost (frame_complexity_me: sum of per-MB
 * min(intra, best-inter)), the same signal the reference encoder's CRF uses. The earlier build
 * fed the zero-motion full-res SATD here, which conflated motion with coding
 * difficulty (foreman ~= ducks) and, with a base tuned to that ~9000/MB scale,
 * collapsed CRF to ~constant-QP-at-crf -- it spent 2-3x the reference encoder's bits, worse on
 * complex content. On the ME signal foreman P sits at ~820/MB and ducks ~1160.
 *
 * BASE (140/MB) and SLOPE (1.8) are calibrated so crf tracks the reference encoder's crf bitrate
 * across foreman/akiyo/mobile/ducks/park_joy (0.9-1.6x, VMAF within ~+/-2 on the
 * complex end). SLOPE is shallower than the reference encoder's qcompress (6*(1-0.6)=2.4) because
 * yah264's lowres metric spreads content ~1.3x wider than the reference encoder's, so 2.4
 * over-penalized complex clips (mobile lost ~5 VMAF). The blur track follows P
 * anchors only: B frames inherit the anchor base (frame_qp adds the type
 * cascade) and I frames read the track without polluting it with intra-domain
 * magnitudes. Base/slope/cap stay env-overridable for re-calibration.
 * ABR/VBV/2-pass still use the self-calibrating zero-motion frame_complexity. */
#define CRF_BASE_CPLX  140.0    /* per-MB ME-compensated cost that sits at qp=crf */
#define CRF_QP_SLOPE   1.8      /* QP per log2 of complexity (a 0.6 qcomp would be 2.4) */
#define CRF_QP_CAP     1.5      /* max QP discount below crf for easy content */

static double rc_tunable(const char *n, double def, int *set)
{
    const char *s = getenv(n);
    if (set) *set = s != NULL;
    return s ? atof(s) : def;
}

/* Every rate-control calibration knob the decides read (env-overridable so
 * the CRF base/cap/slope, the ABR steps and the opening fit can be swept
 * without a rebuild; the defaults are the baked-in calibration). Resolved
 * once at open: decides run on the stair driver thread and nothing on a
 * decide path calls getenv. */
void rc_knobs_resolve(yah264_encoder_t *e)
{
    struct rc_knobs *k = &e->rck;
    k->abr_ovlo       = rc_tunable("Y264_ABR_OVLO", 0.8, NULL);
    k->abr_ipoff      = rc_tunable("Y264_ABR_IPOFF", 2.92, NULL);
    k->abr_ipf        = rc_tunable("Y264_ABR_IPF", 1.4, NULL);
    k->abr_pbf        = rc_tunable("Y264_ABR_PBF", 1.3, NULL);
    k->abr_qpstep     = rc_tunable("Y264_ABR_QPSTEP", 0.0, &k->abr_qpstep_set);
    k->abr_vbvov_grow = rc_tunable("Y264_ABR_VBVOV_GROW", 6.0, NULL);
    k->abr_vbvov_pow  = rc_tunable("Y264_ABR_VBVOV_POW", 1.0, NULL);
    k->abr_vbvov_vbr  = rc_tunable("Y264_ABR_VBVOV_VBR", 0.75, NULL);
    k->abr_dc         = (int)rc_tunable("Y264_ABR_DC", 1.0, NULL);
    k->rf2_iacc       = rc_tunable("Y264_ABR_RF2_IACC", 1.0, NULL);
    k->rf2_seed       = rc_tunable("Y264_ABR_RF2_SEED", 2.0, NULL);
    k->rf2_floorfb    = rc_tunable("Y264_ABR_RF2_FLOORFB", 1.0, NULL) != 0.0;
    k->rf2_seed_a     = rc_tunable("Y264_ABR_RF2_SEED_A", 8.692, NULL);
    k->rf2_seed_b     = rc_tunable("Y264_ABR_RF2_SEED_B", 2.924, NULL);
    k->rf2_seed_c     = rc_tunable("Y264_ABR_RF2_SEED_C", -1.315, NULL);
    k->vbv_plan_h     = rc_tunable("Y264_VBV_PLAN_H", 32.0, NULL);
    k->vbv_plan_t     = rc_tunable("Y264_VBV_PLAN_T", 0.5, NULL);
    k->vbv_plan_pred  = (int)rc_tunable("Y264_VBV_PLAN_PRED", 2.0, NULL);
    k->crf_base       = rc_tunable("Y264_CRF_BASE", CRF_BASE_CPLX, NULL);
    k->crf_slope      = rc_tunable("Y264_CRF_SLOPE", CRF_QP_SLOPE, NULL);
    k->crf_cap        = rc_tunable("Y264_CRF_CAP", CRF_QP_CAP, NULL);
    k->crf_cl_shift   = rc_tunable("Y264_CRF_CL_SHIFT", 0.0, &k->crf_cl_shift_set);
    k->rc_trace       = getenv("Y264_RC_TRACE") != NULL;
    k->dbg_cplx       = getenv("Y264_DBG_CPLX") != NULL;
}

/* The AQ frame-mean (DC) term on the CRF base QP (2026-09-05, the across-shot
 * allocation term). The absolute-anchor AQ offset is strength x (log2 energy
 * - anchor); its per-MB part (the within-frame gradient) was calibrated at
 * strength 0.4 on single-shot clips, where its frame mean is a per-clip
 * constant the band cannot see. Across shots that mean IS the allocation:
 * the reference encoder runs it at strength 1.0 and moves ~4 QP between the shots of a
 * concatenation where we moved ~1.5, which turned a 6-14% per-clip lead
 * into a 5-13% deficit on multi-shot sequences. This adds the missing part,
 * (dcstr - acstr) x (frame mean - anchor), to the ANCHOR's base QP; B frames
 * inherit it through frame_qp's anchor base + cascade, so the P/B relation
 * is untouched and every base-keyed consumer (lookahead lambda, cascade,
 * psy, trellis) sees it -- the offset form of the same term lost 2.4% on
 * mobile for exactly that reason. Gate: ms_cif_30 / ms_720p_50 /
 * ms_1080p_25 -8.2 / -4.1 / -6.2% BD-VMAF-NEG vs flat (the reference encoder's gap +13.4 /
 * +4.9 / +7.3 -> +4.4 / +0.3 / +0.7); the 12-clip band at matched rate
 * median +0.30 / mean -0.26 / worst +0.94, sintel -5.73. Non-anchor paths
 * (mode-2 AQ on non-reference B) carry no DC of their own by design. */
/* Y264_AQ_DC_RQW=<w> (default 0.25; 0 or below = the plain mean, the
 * pre-2026-09-28 output): the frame DC as a RATE-EQUIVALENT
 * shift instead of the plain log-energy mean. The reference encoder applies its
 * absolute offset per macroblock, so a near-flat MB's large negative offset
 * costs almost nothing there, while the plain mean moves every MB of the frame
 * by it; a frame that is mostly flat (dark scenes, credits on black, 4K
 * animation) takes a base QP several points lower than the offset field's rate
 * justifies. With w set, each MB is weighted by a rate proxy 2^(w*l) + 1 and
 * the DC is the base shift that spends what the per-MB strength-1.0 field
 * would. Measured 2026-09-27 at w 0.25: multi-shot -0.92 / -3.16 / -0.38%, CRF
 * band median -0.20% (9 of 12, worst foreman +0.65%), chimera and meridian
 * 2160p and BBB 2160p windows neutral at matched size. w 0.5 overshoots
 * (multi-shot +1.15 / -1.85 / +1.73%). */
static double crf_frame_dc(const yah264_encoder_t *e, pixel *const src[3])
{
    if (!e->aq_abs || !src || !src[0]) return 0.0;
    int wmb = e->width_in_mbs, hmb = e->height_in_mbs, n = wmb * hmb;
    int stride = e->pstride[0], cstride = e->pstride[1];
    int cw = e->aq_chroma && src[1] && src[2] ? 16 / e->sub_w : 0, ch = cw ? 16 / e->sub_h : 0;
    double astr = (double)e->aq_strength * 1.0397;
    double rqw = e->aq_dc_rqw;
    double sum = 0, num = 0, den = 0;
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
            double l = log2(en < 1.0 ? 1.0 : en) - 8.0;
            sum += l;
            if (rqw > 0.0) {
                double c = pow(2.0, rqw * l) + 1.0, d = l - e->aq_anchor;
                num += c * pow(2.0, -e->aq_dcstr * d / 6.0);
                den += c * pow(2.0, -astr * d / 6.0);
            }
        }
    if (rqw > 0.0 && den > 0.0)
        return -6.0 * log2(num / den);
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
        /* Reference-encoder device 2: with mb-tree ON its frame rate equation becomes duration-based
 * (constant per frame) -- frame-level complexity modulation is DROPPED
 * because mb-tree's per-MB offsets already carry the complexity/importance.
 * Keeping the complexity term would double-count it and overspend. So the
 * base is a flat crf; the shift below supplies the operating-point bias. */
        qp = e->crf;
    } else {
        double base_permb = e->rck.crf_base;
        double slope = e->rck.crf_slope;
        double base = base_permb * e->width_in_mbs * e->height_in_mbs;
        double adj = slope * log2(e->crf_cblur / base);
        /* Asymmetric: complex content takes the full upward adjustment, but the
 * easy-content discount is capped -- yah264 already overspends flat
 * content at qp=crf (coding overhead), so letting it drop further is
 * pure VMAF-saturated waste. */
        double cap = e->rck.crf_cap;
        if (adj < -cap) adj = -cap;
        qp = e->crf + adj;
    }
    /* mb-tree operating-point shift (the reference encoder's mb-tree operating-point offset): a fixed
 * uniform +(1-qcomp)*13.5 QP bias. mb-tree's mean-negative per-MB offsets net it
 * back out on average, so the DIFFERENTIAL survives -- static/heavily-referenced
 * anchors (more negative mean_off) net lower QP, motion anchors net higher. Fixed
 * (not per-frame) so it never centres-away the cross-frame benefit; and it aligns
 * the CRF operating point with the reference encoder's constant-quality bitrate. */
    if (e->crf_cl && e->mbtree_on) {
        double shift = (e->rck.crf_cl_shift_set ? e->rck.crf_cl_shift : (1.0 - e->crf_qcomp) * 13.5);
        qp += shift;
    }
    /* Frame-duration term. Under mb-tree the reference encoder's rate equation is duration-only
 * (the rate equation:
 * rceq = (base frame duration / clipped frame duration)^(1 - qcomp)
 * which lands on the base QP as +6*(1-qcomp)*log2(0.04/dur). It is zero at
 * 25 fps, +0.63 at 30, +2.4 at 50, -0.14 at 24 -- measured straight off an
 * instrumented reference build (its rate-control mean QP: 31.03 on every CIF clip at 30 fps, 32.80
 * on 720p50, 30.26 on 720p24). Without it CRF N is a different operating
 * point at every frame rate, which is most of why yah264 overspends
 * park_joy and ducks (both 50 fps) at equal CRF. */
    if (crf_fps_env() && e->crf_cl && e->mbtree_on) {
        double fps = (e->param.timebase.fps_num > 0 && e->param.timebase.fps_den > 0)
                   ? (double)e->param.timebase.fps_num / e->param.timebase.fps_den : 25.0;
        double dur = fps > 0 ? 1.0 / fps : 0.04;
        if (dur < 0.01) dur = 0.01;                 /* the clipped duration, as the reference encoder does */
        if (dur > 1.00) dur = 1.00;
        qp += 6.0 * (1.0 - e->crf_qcomp) * log2(0.04 / dur);
    }
    if (crf_aqabs_env() && e->crf_cl && e->mbtree_on)
        qp += crf_ped_env();
    qp += e->crf_dc;                            /* Y264_AQ_DC_BASE, 0 otherwise */
    if (qp < 1) qp = 1;
    if (qp > 51) qp = 51;
    e->qp = (int)lround(qp);
    /* The base the RATE FACTOR chose, kept before the buffer gets to raise it.
 * --crf-max bounds the raise and not the mapping, so the bound needs both
 * numbers to tell them apart. */
    e->crf_pre_qp = e->qp;
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
 * the boundary sooner and spends more of the clip sitting on it). The reference encoder holds
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
 * Only without a bitrate target. ABR's integrator (the overflow term in the RF2 decide)
 * already enforces the average, and ABR+VBV measures clean with 62% of the
 * buffer still in hand; a second controller there would fight it and cost the
 * rate accuracy. Callers pass the fit-test limit for abr_on; capped VBR gets
 * its own trajectory from the burst plan (vbv_burst_plan_qp) instead.
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
    double half = 0.5 * e->vbv.size;
    double target = fill + 0.5 * e->vbv.rate;
    if (target > half) target = half;
    double allowed = fill + e->vbv.rate - target;
    return allowed > 0.0 ? allowed : 0.0;
}

/* The whole non-ABR bits ceiling for one frame at a given occupancy: the fit
 * test and the half-full budget, whichever is tighter. One function so the
 * serial clamp, the pipelined clamp and the first-frame re-encode bound cannot
 * drift apart.
 *
 * There is deliberately no segment-exit term here for CRF (the corridor below
 * runs only when a caller asks for it, which the CLI does for capped ABR,
 * whose buffer has no budget pulling it back to the handoff). One was built -- a budget
 * that tightened as a segment's last frame approached, so the exit occupancy
 * became a constraint rather than a tendency -- and the ablation said to drop
 * it. It never bound on 30 of the gate's 36 cells, because vbv_fill_budget
 * already holds the occupancy at the handoff. Where it did bind it made things
 * worse on both axes at once, and the mechanism is not subtle: a tighter
 * ceiling raises this frame's QP, the bits it saves stay in the buffer, and the
 * NEXT frame's ceiling is looser by exactly that much. Total spend does not
 * fall, it just moves, and the frame that paid for it is worse. It cost a
 * compliance cell (ducks/10000 serial) and spent 600 kbit/s MORE. */
double vbv_limit_at(const yah264_encoder_t *e, double fill)
{
    /* Fit test: don't drain it dry. `fill` is the occupancy the frame is
 * removed FROM -- the buffer law credits a frame's refill after its
 * removal (fill += rate - bits), so the refill of the frame's own interval
 * is already in it -- and the frame has to fit in what is there. This read
 * 0.9 * (fill + rate) until 2026-10-03, one frame of refill more than the
 * buffer holds at the removal, which let the model sit at "empty" while the
 * stream underflowed by up to a frame's worth of the cap. */
    double limit = 0.9 * (vbv_fitlaw_env() ? fill : fill + e->vbv.rate);
    /* Never zero: every caller reads "limit <= 0" as "no limit", and an empty
 * buffer is the case that wants the HIGHEST QP, not none. */
    if (limit < 0.01 * e->vbv.rate) limit = 0.01 * e->vbv.rate;
    if (!e->abr_on) {                               /* CRF has no integrator */
        double b = vbv_fill_budget(e, fill);
        if (b < limit) limit = b;
    }
    if (e->vbv_exit_n > 0) {
        /* The exit corridor (yah264_encoder_vbv_handoff): leave enough in the
 * buffer that the frames still to come, each keeping vbv_exit_k of its
 * refill, bring it back to the handoff level by the last frame. Wide open
 * for most of a segment, it narrows over the final stretch and requires
 * the level itself of the last frame. */
        int rem = e->vbv_exit_n - e->vbv_ndec;      /* frames after this one */
        if (rem < 0) rem = 0;
        double need = e->vbv_seg_h - rem * e->vbv_exit_k * e->vbv.rate;
        double x = fill + e->vbv.rate - need;
        if (x < 0.02 * e->vbv.rate) x = 0.02 * e->vbv.rate;
        if (x < limit) limit = x;
    }
    return limit;
}

/* --crf-max: the ceiling on what the VBV may raise a frame to. `base` is the
 * QP the rate control had already chosen and `raised` the one the buffer wants;
 * the answer is the raise, bounded, and never below the base -- a frame the
 * rate factor already put above the ceiling keeps the QP it was given, because
 * this bounds the raise and not the mapping.
 *
 * The bound is read on the CODED QP, after the frame-type cascade and any
 * zone offset, for the same reason --qpmax is: a bound on the base QP is not a
 * bound a viewer can see. `off` is the coded QP minus the base at entry, which
 * is what turns one into the other.
 *
 * The buffer is then allowed to under-run, and that is the option, not a
 * defect: the caller asked for the quality floor and the model is what pays.
 * scripts/vbv_check.py reports the under-run. */
double crf_max_clip(const yah264_encoder_t *e, double base, double raised, int off)
{
    if (e->crf_max <= 0.0 || raised <= base)
        return raised;
    double ceil_base = e->crf_max - off;
    if (ceil_base < base) ceil_base = base;
    return raised > ceil_base ? ceil_base : raised;
}





/* The CPLX debug line (Y264_DBG_CPLX) of a frame's rate-control head. */
static void rc_dbg_cplx(const yah264_encoder_t *e, int type, double C, double Ccrf)
{
    if (e->rck.dbg_cplx)
        fprintf(stderr, "CPLX type=%d C=%.0f permb=%.1f  Cme=%.0f permb=%.1f\n", type,
                C, C / (e->width_in_mbs * e->height_in_mbs),
                Ccrf, Ccrf / (e->width_in_mbs * e->height_in_mbs));
}

/* The serial rate-control head, for every frame that does not ride rcp: the
 * base QP of the frame about to be coded, from the previous frames' actuals
 * (pure CRF or CQP at the defaults; ABR, 2-pass and VBV only with the pipeline
 * off). CRF reads the ME-compensated lowres cost; ABR, VBV and 2-pass keep
 * the full-resolution zero-motion metric their bits models calibrate on. The
 * replayed values are consumed in a fixed order: C, Ccrf, then the frame DC. */
void rc_head_serial(yah264_encoder_t *e, int type, int is_ref, pixel *const src[3])
{
    (void)is_ref;
    if (!e->crf_on) return;             /* CQP: the base QP is fixed */
    double C = 0.0;
    if (rc_cplx_wanted(e)) { C = RCR_VAL(e->rcr, RCR_FC, frame_complexity(e, src, type == 0)); e->rc_cplx = C; }
    double Ccrf = RCR_VAL(e->rcr, RCR_FCME, frame_complexity_me(e));
    rc_dbg_cplx(e, type, C, Ccrf);
    if (type != 2) e->crf_dc = RCR_VAL(e->rcr, RCR_CRFDC, crf_frame_dc(e, src));
    rc_set_qp_crf(e, Ccrf, type);
}

/* The CRF head of a frame decided outside emit_frame (a buffered B, a
 * staircase anchor): the same calls in the same order as rc_head_serial's
 * CRF branch, so rc_cplx, the debug line and the replay stream match the
 * serial path. A B frame's decide is a no-op (it inherits the anchor base);
 * want_c = 0 skips the full-resolution probe where it would race (async). */
void rc_head_crf(yah264_encoder_t *e, int type, pixel *const src[3], int want_c)
{
    double C = 0.0;
    if (want_c && rc_cplx_wanted(e)) { C = RCR_VAL(e->rcr, RCR_FC, frame_complexity(e, src, 0)); e->rc_cplx = C; }
    double Ccrf = RCR_VAL(e->rcr, RCR_FCME, frame_complexity_me(e));
    rc_dbg_cplx(e, type, C, Ccrf);
    if (type != 2) e->crf_dc = RCR_VAL(e->rcr, RCR_CRFDC, crf_frame_dc(e, src));
    rc_set_qp_crf(e, Ccrf, type);
}


/* Warm phase: the first RCP_WARM decides run with SERIAL-TIGHT feedback (the
 * pipeline stays disengaged and pops are immediate), so the per-type models
 * calibrate in the rcp complexity domain before any lag begins. Keyed on the
 * DECIDE COUNT -- pure config/sequence state, identical at every thread
 * count. Warm-up was the whole measured accuracy gap: a seed error decides an
 * entire mini-GOP under lag, and the CLI's per-GOP encoders restart the
 * transient every keyint. */

int rcp_warm(const yah264_encoder_t *e)
{
    return (int)atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) < RCP_WARM;
}

/* Per-type VBV bits model in the rcp decide-complexity domain, falling back
 * across types until calibrated (abr_scale_for's shape). 0.0 = nothing
 * calibrated yet: predictions unavailable, the clip stays off and the burst
 * gate stays tight. */
static double rcp_vbv_scale_for(const yah264_encoder_t *e, int type)
{
    return rcbm_k(&e->bm_rvbv, type, 0.0);
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
    if (e->bm_rvbvs.have[0] && e->bm_rvbvs.k[0] > vs)
        vs = e->bm_rvbvs.k[0];
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
        /* Y264_VBV_CW (default on): the scale is a ratio of decayed SUMS,
 * bits*qscale over complexity, so each frame weighs in by its complexity.
 * An average of per-frame ratios lets a run of near-empty frames -- a
 * black stretch codes ~400 bits of headers at any complexity -- set the
 * scale, and after the cut the first real frame moves it by 0.3 of the
 * way: chimera's post-cut P predicted 252 kbit at the QP its predecessor
 * had just coded 747 kbit at. Weighted, the empty frames carry almost no
 * weight and the first real frame takes the scale. */
        double qs_bits = p->bits * qscale, cx = p->vcplx > 0 ? p->vcplx : 1.0;
        {
            struct rc_bmodel *m = &e->bm_rvbv, *sh = &e->bm_rvbvs;
            int t = p->type, w = m->have[t], ws = sh->have[0];
            if (vbv_cw_env()) {
                m->num[t]   = rcbm_sum(w, m->num[t], qs_bits);
                m->den[t]   = rcbm_sum(w, m->den[t], cx);
                sh->num[0]  = rcbm_sum(ws, sh->num[0], qs_bits);
                sh->den[0]  = rcbm_sum(ws, sh->den[0], cx);
            }
            m->k[t]    = rcbm_ema(w, m->k[t], s);
            m->qp[t]   = rcbm_ema(w, m->qp[t], p->fqp);
            m->cplx[t] = rcbm_ema(w, m->cplx[t], p->vcplx);
            m->have[t] = 1;
            sh->k[0]   = rcbm_ema(ws, sh->k[0], s);
            sh->have[0] = 1;
        }
        if (vbv_cw_env()) {
            e->bm_rvbv.k[p->type] = e->bm_rvbv.num[p->type] / e->bm_rvbv.den[p->type];
            e->bm_rvbvs.k[0] = e->bm_rvbvs.num[0] / e->bm_rvbvs.den[0];
        }
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
        /* The buffer law, actuals only. */
        {
            int empty = 0;
            e->vbv.fill = rcbuf_step(&e->vbv, e->vbv.fill, p->bits, &empty);
            if (empty) e->rcp_vbv_nclamp++;
        }
        if (rcp_dbg_on())
            fprintf(stderr, "RCPB seq=%u type=%d bits=%.0f fill=%.0f\n",
                    p->seq, p->type, p->bits, e->vbv.fill);
    }
    if (e->abr_on) {
        double qscale = pow(2.0, (p->fqp - 12) / 6.0);
        double s = p->bits * qscale / p->rceq;
        if (e->abr_rf || e->abr_rf2 || abr_rfqp_trace()) {     /* the trace shadows the rf accumulators */
            /* B divides by pb_factor so a B's cheapness cannot drag the whole
 * rate factor down. Under mb-tree the P/B ratio is 1, which is the RF2
 * form. The accumulators never decay: with a VBV the overflow term's
 * window is tied to the buffer instead (abr_vbvov_frac). */
            double pb = p->type == 2 && !e->abr_rf2 ? e->rck.abr_pbf : 1.0;
            /* Accumulate at the frame's BASE QP (the frame-level QP before the
 * per-MB offsets), which keeps the
 * prediction "bits at base q" self-consistent with frames coded at
 * base + offset. */
            double accqp = e->abr_rf2 ? p->acc_qp : p->fqp;   /* the base-domain accounting above */
            double qacc = e->abr_rf2 ? pow(2.0, (accqp - 12) / 6.0) : qscale;
            double contrib = p->bits * qacc / (p->rceq * pb);
            if (e->abr_rf2 && p->cplx > 0) {
                double k = contrib / p->cplx;
                e->bm_rf2kc.k[p->type] = rcbm_ema(e->bm_rf2kc.have[p->type], e->bm_rf2kc.k[p->type], k);
                e->bm_rf2kc.have[p->type] = 1;
            }
            double n_before = e->rf_wanted_bits / e->abr_target_bpf;
            double mean_before = n_before > 0 ? e->rf_cplx_sum / n_before : 0.0;
            e->rf_cplx_sum += contrib;
            e->rf_wanted_bits += e->abr_target_bpf;
            if (e->rc_shared && e->abr_rf2)
                rc_shared_add(e, contrib, e->abr_target_bpf, p->bits, e->abr_target_bpf);
            abr_track_update(e, p->fqp - p->dcq, p->type);
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
        e->bm_abr.k[p->type] = rcbm_ema(e->bm_abr.meas[p->type], e->bm_abr.k[p->type], s);
        /* The calibrated QP regime per type, for the lag guard in rcp_decide.
 * Travels with the scale for the same reason the VBV one does: it is
 * the statement of where this model has actually been measured. */
        e->bm_abr.qp[p->type] = rcbm_ema(e->bm_abr.meas[p->type], e->bm_abr.qp[p->type], p->fqp);
        e->bm_abr.meas[p->type] = 1;
        e->bm_abr.have[p->type] = 1;
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

/* Retire the oldest ledger entry: account it unless it was dropped, then pop
 * it. Every commit goes through here (the pop bound, the anchor prologues,
 * the burst gate, flush); tag names the caller in the Y264_RCP_DBG trace. */
static void rcp_retire_head(yah264_encoder_t *e, char tag)
{
    struct rcp_pend *p = &e->rcp[e->rcp_head];
    if (rcp_dbg_on())
        fprintf(stderr, "RCP%c seq=%u bits=%.0f\n", tag, p->seq, p->bits);
    if (!p->dropped)
        rcp_account(e, p);
    e->rcp_head = (e->rcp_head + 1) % RCP_MAX;
    e->rcp_n--;
}

/* Retire every entry that has its bits, oldest first, regardless of the pop
 * bound: the zero-lag anchor prologue, the burst gate and flush. */
static void rcp_commit_filled(yah264_encoder_t *e, char tag)
{
    while (e->rcp_n > 0 && e->rcp[e->rcp_head].filled)
        rcp_retire_head(e, tag);
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
        rcp_retire_head(e, 'P');
    }
}

/* Actual coded bits arrive (coding order == push order). */
void rcp_fill(yah264_encoder_t *e, double bits)
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
void rcp_reqp(yah264_encoder_t *e, int type, int is_ref)
{
    if (e->rcp_n <= 0)
        return;
    struct rcp_pend *p = &e->rcp[(e->rcp_head + e->rcp_n - 1) % RCP_MAX];
    if (p->filled)
        return;                             /* already accounted; not ours */
    int nf = frame_qp(e, type, is_ref);
    double r = pow(2.0, (double)(p->fqp - nf) / 6.0);   /* bits track 1/qscale */
    p->acc_qp += e->qp - p->base_qp;    /* the rate factor credits the QP coded */
    p->base_qp = e->qp;
    p->fqp = nf;
    p->pred *= r;
    p->vpred *= r;
}

/* CAVLC-overflow drop: remove without accounting (mirrors the serial skip). */
void rcp_drop(yah264_encoder_t *e)
{
    for (int i = 0; i < e->rcp_n; i++) {
        struct rcp_pend *p = &e->rcp[(e->rcp_head + i) % RCP_MAX];
        if (!p->filled) { p->filled = 1; p->dropped = 1; p->bits = 0; break; }
    }
    rcp_pop_ready(e);
}

/* Flush/close: commit everything that has bits. The caller's flush points are
 * part of the API call sequence, identical at every thread count. */
void rcp_flush_all(yah264_encoder_t *e)
{
    rcp_commit_filled(e, 'X');
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
double rcp_vbv_vfill(const yah264_encoder_t *e, int drop_tail)
{
    double rhi = vbv_rhi_env();
    double fill = e->vbv.fill;
    int n = e->rcp_n - drop_tail;
    for (int i = 0; i < n; i++) {
        const struct rcp_pend *p = &e->rcp[(e->rcp_head + i) % RCP_MAX];
        fill = rcbuf_step(&e->vbv, fill, rhi * p->vpred, NULL);
    }
    return fill;
}

/* Capped VBR: plan the whole burst at the anchor (Y264_VBV_PLAN, default on).
 *
 * The per-frame fit test only fires when the buffer is nearly empty, and under
 * a bitrate target nothing else holds it up. On a cheap opening the rate
 * factor banks a surplus and codes low; when the hard section arrives the
 * buffer drains from full to the floor inside a second, and the encode then
 * runs pinned there, every frame clipped against 0.9 of a near-empty buffer --
 * a QP sawtooth of +-4 within each mini-GOP (sollevante 1080p at 12.5 Mbit/s
 * spent 400 frames at 5% occupancy). The B frames inherit the anchor's base, so
 * the anchor is where the burst's spend is decided, and this prices all of it:
 * the anchor plus every buffered B, each at the model's prediction, walked
 * through the buffer law. The base is the lowest QP at or above the rate
 * factor's whose walk keeps every frame inside 0.9 of what it is removed from
 * and ends the burst on a trajectory toward the target level -- half the
 * buffer -- closing the gap over Y264_VBV_PLAN_H frames rather than at once.
 * Above the target the burst may spend down to it, so a full buffer is spent
 * on hard content exactly as before; below it the raise arrives one burst at a
 * time instead of as a wall.
 *
 * Capped VBR only: under CBR the buffer has no headroom to plan around and the
 * spend-the-spare branch owns the top. A pure function of the ledger, the
 * pending B complexities and the decided QP, so it moves with nothing else. */
static int vbv_plan_on(const yah264_encoder_t *e)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_PLAN"); v = s ? atoi(s) : 1; }
    return v && e->abr_on && e->vbv_on && !e->vbv_cbr && !e->tp_pass;
}

static double vbv_burst_plan_qp(const yah264_encoder_t *e, double C, int type, int is_ref,
                                double fill)
{
    double va = rcp_vbv_scale_eff(e, type);
    double vb = rcp_vbv_scale_eff(e, 2);
    double shock = e->rcp_vbv_shock > 1.0 ? e->rcp_vbv_shock : 1.0;
    if (e->rck.vbv_plan_pred) {
        /* The plan is a trajectory, not the underflow guard, so it prices
 * the burst at each type's own measured scale: no shared-scale maximum
 * and no shock multiplier. Those two are the fit test's margin, and they
 * stay there (rcp_vbv_clip). In the plan they priced the P after every
 * intra frame at three to four times what it coded, and the plan answered
 * with a base 10 to 20 QP above the rate factor's for the next few
 * anchors: on a 10-minute 720p title, one spike per cut. */
        /* ...and only at scales this instance has measured for the types
 * it is pricing. A GOP instance opens with none; the P after its first I
 * would be priced at the I frame's scale (the cross-type fallback), and
 * that was a one-anchor spike of 10 to 20 QP at every GOP start. */
        if (!e->bm_rvbv.have[type] || (e->nbuf > 0 && !e->bm_rvbv.have[2]))
            return 0.0;
        va = rcp_vbv_scale_for(e, type);
        vb = rcp_vbv_scale_for(e, 2);
        if (e->rck.vbv_plan_pred > 1) shock = 1.0;
    }
    if (va <= 0.0)
        return 0.0;
    if (vb <= 0.0) vb = va;
    double H = e->rck.vbv_plan_h;
    double T = e->rck.vbv_plan_t * e->vbv.size;
    int nb = e->nbuf;
    int n = nb + 1;
    double end_need = fill >= T ? T : fill + (T - fill) * (n / (H > 1 ? H : 1));
    /* An intra frame is one frame's spend, not a rate: asking its burst to
 * end above where it started squeezes exactly the frame the whole shot
 * predicts from. It may take the buffer down to half the target level;
 * below that the ordinary rule applies again. */
    if (e->rck.vbv_plan_pred && type == 0 && fill > 0.5 * T && end_need > 0.5 * T)
        end_need = 0.5 * T;
    int aoff = frame_qp(e, type, is_ref) - e->qp;   /* I takes its offset */
    for (int base = e->qp; base <= 51; base++) {
        double f = fill;
        int ok = 1;
        for (int j = 0; j < n && ok; j++) {
            int cq = j == 0 ? base + aoff : base + 1;   /* B: the smallest cascade step */
            if (cq > 51) cq = 51;
            double c = j == 0 ? C : e->rcp_bcvi[j - 1];
            double bits = (j == 0 ? va : vb) * c / pow(2.0, (cq - 12) / 6.0) * shock;
            if (bits > 0.9 * f) ok = 0;
            f = rcbuf_refill(&e->vbv, f - bits);
        }
        if (ok && f >= end_need)
            return base;
    }
    return 51.0;
}

/* VBV clip against the virtual buffer. At an
 * anchor decide the pending set is empty (zero-lag) and this IS the serial
 * computation; only in-burst B decides see the conservative bound, and only in
 * pipelined (non-tight) bursts -- a tight burst's pending set is empty too
 * (pops at fill). */
static void rcp_vbv_clip(yah264_encoder_t *e, double C, int type, int is_ref)
{
    e->vbv_ndec++;
    double vs = rcp_vbv_scale_eff(e, type);
    if (vs <= 0.0)
        return;                             /* nothing calibrated yet */
    double fill = rcp_vbv_vfill(e, 0);      /* entry not pushed yet: walk them all */
    /* The extrapolation guard the pipelined B decides already have, for the
 * anchor of a burst that is not tight: that anchor is coded where no
 * measured-size retry can reach it (the stair publishes its rows to the B
 * leaves as it goes), so it must not dive far below the QP regime its
 * scale was measured at. samsung at a 480 kbit cap: a static run calibrated
 * the P scale at QP 46, the cut's anchor decided 27, predicted 21 kbit and
 * coded 464 kbit. */
    if (type != 2 && !e->rcp_vbv_tight && e->bm_rvbv.have[type]) {
        int lo = rcbm_qp_floor(&e->bm_rvbv, type, vbv_qpd_env());
        if (e->qp < lo) e->qp = lo > 51 ? 51 : lo;
    }
    int coded = frame_qp(e, type, is_ref);
    double qscale = pow(2.0, (coded - 12) / 6.0);
    double pred = vs * C / qscale;
    if (e->rcp_vbv_shock > 1.0)
        pred *= e->rcp_vbv_shock;   /* recent overshoot: distrust the model */
    /* The budgets on the VIRTUAL fill -- the buffer after the in-flight frames
 * are charged. That is this path's version of the reference encoder's VBV plan: an
 * anticipated occupancy rather than a committed one, so a drain already in
 * flight is answered now instead of one burst later. */
    double limit = vbv_limit_at(e, fill);
    double newqp = e->qp;
    if (limit > 0.0 && pred > limit) {      /* over budget: shrink the frame */
        newqp = e->qp + 6.0 * log2(pred / limit);
    } else if (e->vbv_cbr) {
        /* Spend spare buffer only at CBR (serial rule -- see vbv_cbr's init:
 * capped VBR fills at maxrate and this branch would chase it). */
        double room = rcbuf_overflow(&e->vbv, fill);
        if (room > 0 && pred < room)
            newqp = e->qp + 6.0 * log2(pred / room);
    }
    double newqp_lim = newqp;
    if (type != 2 && vbv_plan_on(e)) {
        double pq = vbv_burst_plan_qp(e, C, type, is_ref, fill);
        if (pq > newqp) newqp = pq;
    }
    if (rcp_dbg_on())
        fprintf(stderr, "RCPC type=%d qp_in=%d pred=%.0f shock=%.2f limit=%.0f fill=%.0f/%.0f lim_qp=%.2f plan_qp=%.2f tight=%d vs=%.3f C=%.0f\n",
                type, e->qp, pred, e->rcp_vbv_shock, limit, fill, e->vbv.size, newqp_lim, newqp, e->rcp_vbv_tight, vs, C);
    newqp = crf_max_clip(e, e->qp, newqp, coded - e->qp);
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
void rcp_vbv_gate(yah264_encoder_t *e, int is_idr)
{
    rcp_commit_filled(e, 'A');
    e->rcp_vbv_nburst++;
    /* The anchor codes against the previous anchor, across every B of its
 * burst, so it costs at least what the costliest of them does against its
 * neighbour; its own cost against the previous display frame can collapse
 * where theirs does not (samsung, frame 33: 12k against 1.5M, 89 kbit
 * predicted, 448 kbit coded into a 720 kbit buffer). Floor the anchor's VBV
 * complexity there before anything prices it. */
    for (int i = 0; i < e->nbuf; i++)
        if (e->rcp_bcvi[i] > e->rcp_arr_cvi)
            e->rcp_arr_cvi = e->rcp_bcvi[i];
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
        double fill = e->vbv.fill;
        tight = 0;
        /* Content-jump guard: the scale is calibrated on the regime it has
 * SEEN; a coming frame whose complexity jumps far past it (missed
 * cut, fade-end flash) breaks the model by more than any fixed
 * multiplier. Take the serial schedule for that burst. */
        if (e->bm_rvbv.cplx[1] > 0 && e->rcp_arr_cvi > cj * e->bm_rvbv.cplx[1])
            tight = 1;
        if (e->bm_rvbv.have[2] && e->bm_rvbv.cplx[2] > 0)
            for (int i = 0; i < e->nbuf; i++)
                if (e->rcp_bcvi[i] > cj * e->bm_rvbv.cplx[2])
                    tight = 1;
        /* ...and a COLLAPSE the same distance the other way. A complexity
 * far below the calibrated regime is no safer: the anchor of samsung's
 * burst at frame 33 read a complexity of 12k against 1.5M on its own
 * B frames, predicted 89 kbit and coded 500 kbit into a 720 kbit
 * buffer. Tight sends it down the serial schedule, where the
 * measured-size bound catches what the prediction cannot. */
        if (e->bm_rvbv.cplx[1] > 0 && e->rcp_arr_cvi * cj < e->bm_rvbv.cplx[1])
            tight = 1;
        /* The anchor at the current base QP (type-1 frame_qp offset is 0)... */
        double q = pow(2.0, (e->qp - 12) / 6.0);
        fill -= rhi * vs1 * e->rcp_arr_cvi / q;     /* removed, then refilled */
        if (fill < 0) tight = 1;
        fill = rcbuf_refill(&e->vbv, fill);
        /* ...then the B's at the smallest cascade offset (+1) over the QPD
 * floor the pipelined decide will enforce: the most-bits bound over
 * every pyramid depth, in the regime the model can speak to. */
        double vs2 = rcp_vbv_scale_eff(e, 2);
        double bqp = e->qp + 1;
        if (e->bm_rvbv.have[2]) {
            int lo = rcbm_qp_floor(&e->bm_rvbv, 2, vbv_qpd_env());
            if (bqp < lo) bqp = lo;
        }
        double qb = pow(2.0, (bqp - 12) / 6.0);
        for (int i = 0; i < e->nbuf && !tight; i++) {
            fill -= rhi * vs2 * e->rcp_bcvi[i] / qb;
            if (fill < 0) tight = 1;
            fill = rcbuf_refill(&e->vbv, fill);
        }
    }
    if (vbv_force_env())
        tight = 0;                          /* measurement-only override */
    e->rcp_vbv_tight = tight;
    e->rcp_vbv_ntight += tight;
    if (rcp_dbg_on())
        fprintf(stderr, "RCPG tight=%d fill=%.0f nbuf=%d\n",
                tight, e->vbv.fill, e->nbuf);
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
 *   mode 1: was a fixed QP (Y264_ABR_RF2_SEEDQ, refused and removed); now
 *           the same as mode 0;
 *   mode 2: fitted on the lookahead window: the median lowres inter cost per
 *           macroblock of the frames that arrived before the first decide and
 *           the target bits per macroblock,
 *             qp = 8.69 + 2.92 log2(cme/mb) - 1.32 log2(bits/mb), clamped 15..45,
 *           least squares on 17 non-board cells (fit rms 3.3 QP), scoring the
 *           ten board clips out of sample at rms 2.5 / max 4.1 QP
 *           (local record abr-undershoot-2026-09-04). */
/* The opening fit's input: the median lowres cost of the oldest 32 buffered
 * frames, -1 when fewer than two are usable. Its own function so that rate-
 * control replay can supply it (rcreplay.h). */
static double rf2_open_cme(const yah264_encoder_t *e, int *nusable)
{
    double v[32]; int n = 0;
    for (int i = 0; i < e->la_n && n < (int)(sizeof v / sizeof v[0]); i++) {
        const struct la_entry *en = &e->la[(e->la_head + i) % e->la_cap];
        if (en->sum_cost[LR_LEG_PREV] > 0) v[n++] = (double)en->sum_cost[LR_LEG_PREV];
    }
    *nusable = n;
    if (n < 2) return -1.0;
    for (int i = 1; i < n; i++) { double t = v[i]; int j = i; while (j > 0 && v[j-1] > t) { v[j] = v[j-1]; j--; } v[j] = t; }
    return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static double rf2_open_qp(yah264_encoder_t *e)
{
    int mode = (int)e->rck.rf2_seed;
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
    int n = 0;
    double cme = RCR_VAL(e->rcr, RCR_RF2CME, rf2_open_cme(e, &n));
    if (abr_rfqp_trace())
        fprintf(stderr, "RFOPEN mode=%d la_n=%d usable=%d\n", mode, e->la_n, n);
    if (cme < 0.0) return 0.0;
    double nmb = (double)(e->width_in_mbs * e->height_in_mbs);
    if (nmb <= 0 || cme < 1.0 || e->abr_target_bpf <= 0) return 0.0;
    double qp = e->rck.rf2_seed_a
              + e->rck.rf2_seed_b * log2(cme / nmb)
              + e->rck.rf2_seed_c * log2(e->abr_target_bpf / nmb);
    /* Y264_ABR_RF2_FLOORFB (default 1): a fit below its floor is a fit outside
 * the range it was fitted on, not a QP. The fit's cells were all 1080p or
 * below and none animation; flat 4K animation reads a lowres cost per
 * macroblock so small that it lands far below the floor (bbb 2160p: the
 * fit hit 15 at every rung where CRF settles near 34), and clamping it to
 * 15 coded the first I frame at QP 15 with half the encode's bits (ABR
 * +35%, VBR +14% BD-VMAF-NEG against the reference). Under the floor the
 * opening falls back to the resolution-only seed instead. A fit inside
 * [15, 45] is untouched. 0 restores the clamp. */
    if (qp < 15 && e->rck.rf2_floorfb) {
        if (abr_rfqp_trace())
            fprintf(stderr, "RFOPEN n=%d cme/mb=%.0f bits/mb=%.1f -> fit %.2f under the floor: resolution seed\n",
                    n, cme / nmb, e->abr_target_bpf / nmb, qp);
        return 0.0;
    }
    if (qp < 15) qp = 15;
    if (qp > 45) qp = 45;
    if (abr_rfqp_trace())
        fprintf(stderr, "RFOPEN n=%d cme/mb=%.0f bits/mb=%.1f -> qp %.2f\n", n, cme / nmb, e->abr_target_bpf / nmb, qp);
    return qp;
}

/* Y264_VBV_ROWBRAKE_WALK (default 0 = the clip's own budget): see rb_note_budget. */
double rb_walk_env(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_VBV_ROWBRAKE_WALK"); v = s ? atof(s) : 0.0; if (v < 0) v = 0; }
    return v;
}

/* The row brake's inputs, noted after the VBV clip: the budget the clip
 * tested the frame against (the same call on the same virtual fill, the
 * entry not pushed yet) and the frame's predicted size at its coded QP,
 * shock included. Read only: nothing here moves the decide. */
static void rb_note_budget(yah264_encoder_t *e, struct rcp_pend *en)
{
    double bud = 0.0;
    if (rcp_vbv_scale_eff(e, en->type) > 0.0) {
        double fill;
        if (rb_walk_env() > 0.0) {
            /* Y264_VBV_ROWBRAKE_WALK=x: the in-flight frames charged at x times
 * their prediction instead of the clip's conservative r_hi (2.0) --
 * the budget the frame most likely has, not the worst case */
            fill = e->vbv.fill;
            for (int i = 0; i < e->rcp_n; i++) {
                const struct rcp_pend *p = &e->rcp[(e->rcp_head + i) % RCP_MAX];
                fill = rcbuf_step(&e->vbv, fill, rb_walk_env() * p->vpred, NULL);
            }
        } else
            fill = rcp_vbv_vfill(e, 0);
        bud = vbv_limit_at(e, fill);
    }
    double shock = e->rcp_vbv_shock > 1.0 ? e->rcp_vbv_shock : 1.0;
    en->rb_bud = bud;
    en->rb_pred = en->vpred * shock;
    e->rb_bud = en->rb_bud;
    e->rb_pred = en->rb_pred;
}

/* Y264_ABR_DC (default 1; 0 = the output before 2026-10-09): the rate-factor
 * controller runs on the CRF path's field. Until now the absolute AQ anchor
 * and the anchor's frame-DC term (crf_frame_dc: the across-shot allocation,
 * worth 4-8% on multi-shot content under CRF) were CRF-only, so one-pass ABR
 * and capped VBR coded every shot at one base QP where our own CRF moves
 * several QP between a dark or flat shot and a busy one. On a 10-minute
 * 720p title at six threads plain ABR read 4.0% behind our own CRF curve
 * in BD-VMAF-NEG and reads 1.1% with it; capped VBR gains 0.9 points. Off
 * under CBR (2 arms it there), which was not measured. With it the anchor's base QP is the
 * rate factor's QP plus the frame DC; the ledger credits each frame at its
 * coded QP minus that DC, so the factor converges on the constant-quality
 * level that spends the target, exactly a slowly adapted CRF. */
static int abr_dc_on(const yah264_encoder_t *e)
{
    return e->abr_rf2 && e->aq_abs && e->rck.abr_dc > 0 && (!e->vbv_cbr || e->rck.abr_dc > 1);
}

void rcp_decide(yah264_encoder_t *e, int type, int is_ref,
                       pixel *const src[3])
{
    double C = type == 0 ? RCR_VAL(e->rcr, RCR_FC, frame_complexity(e, src, 1)) : e->rcp_cur_cme;
    if (e->abr_on && type != 0) {
        double f = e->rcp_cur_cvi * 0.2;
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
        rcp_commit_filled(e, 'A');
    double pend_pred = 0.0, pend_cq = 0.0, pend_cplxr = 0.0;
    int np = e->rcp_n;
    /* A5b (always on): under the rate-factor controller
     * an in-flight frame (decided, not yet coded) is predicted from ITS OWN
     * decide complexity times a per-type EMA of contribution per unit of
     * complexity, instead of the type's mean contribution. Only reachable with
     * Y264_RCP_LAG >= 1: at lag 0 every anchor drains the ring before it
     * decides, so the default schedule is byte-identical either way. The mean
     * form is what made the lag fail on sintel (+14.4% BD): a lagged burst over
     * the near-black opening credits each in-flight frame at the mean cost, far
     * above what a black frame spends, and the factor drifts before the actuals
     * land. */
    int rf2_inflight = e->abr_on && e->abr_rf2;
    double rf2_mean = e->rf_wanted_bits > 0 ? e->rf_cplx_sum / (e->rf_wanted_bits / e->abr_target_bpf) : 0.0;
    for (int i = 0; i < e->rcp_n; i++) {
        const struct rcp_pend *p = &e->rcp[(e->rcp_head + i) % RCP_MAX];
        if (rf2_inflight) {
            /* A5b: the frame's own complexity when its type is calibrated,
 * the type mean until then. Bits at its decided QP follow from the
 * same contribution (contrib = bits * qscale / rceq). */
            double c = e->bm_rf2kc.have[p->type] && p->cplx > 0 ? e->bm_rf2kc.k[p->type] * p->cplx : rf2_mean;
            pend_cplxr += c;
            pend_pred += c * p->rceq / pow(2.0, (p->acc_qp - 12) / 6.0);
            pend_cq += p->cq;
            continue;
        }
        /* Re-evaluate the in-flight prediction against the CURRENT model
 * (the reference encoder recomputes predicted bits continuously): when a pop just
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
        /* The old per-frame ABR equations against the VIRTUAL ledger: committed actuals
 * plus in-flight predictions, each pending frame owing one target_bpf. */
        /* The ledger this decide reads: the instance's own, or the totals of
 * every instance sharing one (yah264_encoder_rc_share). */
        double L_cplx = e->rf_cplx_sum, L_wanted = e->rf_wanted_bits;
        double L_act = e->abr_cum_actual, L_tgt = e->abr_cum_target;
        /* The anchor's frame-DC term (see abr_dc_on): the rate factor and every
 * piece of its state (the ledger, the step clip's last qscale, the P
 * track, the opening) stay in the DC-free domain, and the DC lands on
 * the coded base QP only. */
        double dc = 0.0;
        if (abr_dc_on(e) && type != 2) {
            dc = RCR_VAL(e->rcr, RCR_CRFDC, crf_frame_dc(e, src));
            e->crf_dc = dc;
        }
        int shared = e->rc_shared && e->abr_rf2;
        if (shared) {
            if (!e->rc_shared_seeded) {
                /* The first decide of an instance that did not continue
 * another (rc_import) adds its opening -- one frame of prior at the
 * QP fitted on its own lookahead -- so the ledger's prior is the
 * mean opening of every GOP that started cold. */
                if (!e->rf2_opened && type != 2) {
                    double oq = rf2_open_qp(e);
                    if (oq > 0)
                        rc_shared_add(e, pow(2.0, (oq - dc - 12.0) / 6.0) * e->abr_target_bpf / e->rf2_rceq,
                                      e->abr_target_bpf, 0.0, 0.0);
                }
                e->rf2_opened = 1;
                e->rc_shared_seeded = 1;
            }
            rc_shared_get(e, &L_cplx, &L_wanted, &L_act, &L_tgt);
            if (L_wanted <= 0.0) {          /* nothing in it yet: the open-time seed */
                L_cplx = e->rf_cplx_sum; L_wanted = e->rf_wanted_bits;
            }
        }
        double err = (L_act + pend_pred)
                   - (L_tgt + np * e->abr_target_bpf);
        if (rcp_dbg_on())
            fprintf(stderr, "RCPE seq=%u err=%.0f\n",
                    atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) + 1, err);
        double target = e->abr_target_bpf - err * rcp_gain();
        if (target < 200) target = 200;
        double scale = abr_scale_for(e, type);
        double rceq = pow(C, 1.0 - abr_qcomp_env());
        if (e->abr_rf2) {
            /* A2: the CRF path plus a rate factor (the RF2 model). */
            rceq = e->rf2_rceq;
            if (type != 2) {                   /* B: anchor base + frame_qp cascade, as CRF */
                /* First is this INSTANCE's first decide (its unclipped
 * opening frame), even when a shared ledger already holds other
 * instances' frames: the step clip below is relative to this
 * instance's own last QP, which does not exist yet. */
                int first = e->abr_cum_actual <= 0 && np == 0;
                if (first && !e->rf2_opened && !shared) {
                    double oq = rf2_open_qp(e);
                    if (oq > 0)
                        e->rf_cplx_sum = pow(2.0, (oq - dc - 12.0) / 6.0) * e->abr_target_bpf / e->rf2_rceq;
                    e->rf2_opened = 1;
                    L_cplx = e->rf_cplx_sum;    /* the ledger read above predates the opening */
                }
                double n_done = L_wanted / e->abr_target_bpf;
                double wanted = L_wanted + np * e->abr_target_bpf;
                double cplxr = L_cplx;
                if (rf2_inflight) cplxr += pend_cplxr;
                else if (np > 0 && n_done > 0) cplxr += np * (L_cplx / n_done);
                double q = rceq * cplxr / (wanted > 0 ? wanted : 1.0);
                double ov = 1.0;
                if (!first) {
                    double bps = e->abr_target_bpf * (e->abr_fps > 0 ? e->abr_fps : 25.0);
                    double t = bps > 0 ? wanted / bps : 0.0;
                    double buf = 2.0 * abr_tol(e) * bps * (t > 1.0 ? sqrt(t) : 1.0);
                    if (e->vbv_on && e->vbv.size > 0 && abr_vbvov_frac() > 0 &&
                        (e->vbv_cbr || e->rck.abr_vbvov_vbr > 0)) {
                        /* The window is tied to the buffer under CBR (A6).
 * Capped VBR takes a wider one, Y264_ABR_VBVOV_VBR (0.75) of the
 * buffer, against its own bar (owner decision 2026-10-05: about +-6%
 * over a 6 s window; CBR keeps the tight one). 0.75 is the widest
 * that holds A6 inside it (stefan -4.8%, ducks +5.3% at the 1.5x
 * cap; 1.0 reads -6.6 / +6.1) and buys 0.7-2.7 BD points on tier S
 * VBR at one thread (foreman -8.0 -> -10.7%, stefan -7.9 -> -10.2%).
 * Both widen with the coded duration past Y264_ABR_VBVOV_GROW
 * seconds as (t / grow)^Y264_ABR_VBVOV_POW. */
                        double cap = (e->vbv_cbr ? abr_vbvov_frac() : e->rck.abr_vbvov_vbr) * e->vbv.size;
                        double g = e->rck.abr_vbvov_grow;
                        if (!e->vbv_cbr && g > 0 && t > g)
                            cap *= pow(t / g, e->rck.abr_vbvov_pow);
                        if (buf > cap) buf = cap;
                    }
                    /* Under a shared ledger every live instance reads the
 * same err and answers it at once, so the loop gain is the instance
 * count times one instance's. Each takes its share of the window
 * instead: sollevante 1080p capped VBR read BD +1.15% against the
 * reference with the whole window per instance, -1.5 / -1.4% with
 * its share, chimera 1080p -4.0 -> -5.9%. */
                    if (shared && rc_share_ovn_env())
                        buf *= rc_shared_nlive(e);
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
                    double lstep = pow(2.0, (e->rck.abr_qpstep_set ? e->rck.abr_qpstep : e->qp_step) / 6.0);
                    q = rcstep_qscale(q, e->last_qscale_type[type], lstep,
                                      ov > 1.1 && n_done > 3,   /* widen only after the first frames */
                                      ov < 0.9);
                }
                e->last_qscale_type[type] = q;
                if (first) e->last_qscale_type[1] = q * e->rck.abr_ipf;
                double qp = 12.0 + 6.0 * log2(q > 0 ? q : 1e-9);
                /* The first I is coded at the seed QP itself; frame_qp takes
 * 3 off every I, so hand it the seed plus 3. */
                if (type == 0 && first) qp += 3.0;
                if (qp < 1) qp = 1;
                if (qp > 51) qp = 51;
                e->abr_qp = qp;
                qp += dc;
                if (qp < 1) qp = 1;
                if (qp > 51) qp = 51;
                e->qp = qp_bound(e, (int)lround(qp));
                e->chroma_qp = y264_chroma_qp(e->qp, e->param.chroma_qp_index_offset);
                if (abr_rfqp_trace())
                    fprintf(stderr, "RFQP seq=%u type=%d C=%.0f rceq=%.3f np=%d | rf2 cplxr=%.0f wanted=%.0f "
                            "err=%.0f ov=%.3f qp_eq=%.2f anch=%d qp=%.2f dc=%.2f moff=%.2f | live=rf2 qp=%d\n",
                            atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) + 1, type, C, rceq,
                            np, cplxr, wanted, err, ov, 12.0 + 6.0 * log2(q_eq > 0 ? q_eq : 1e-9),
                            anch, qp, dc, e->mbtree_mean_off, e->qp);
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
                /* the reference encoder: the rate factor runs on the
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
                /* the reference encoder's asymmetric per-type clip .
 * WITHOUT THIS the rate factor runs away on content whose
 * complexity signal legitimately collapses: sintel opens on
 * near-black frames with C ~ 1, so cplxr stops growing while
 * wanted grows linearly, rf goes 14 -> 163 in fourteen frames
 * and qp goes NEGATIVE. HEAD survives that only because its
 * +/-4 swing limit absorbs it. Asymmetric because symmetric
 * would block overflow control in rapidly oscillating
 * complexity, which is the reference encoder's own stated reason. */
                double lstep = pow(2.0, (e->rck.abr_qpstep_set ? e->rck.abr_qpstep : e->qp_step) / 6.0);
                double ovf = abr_overflow(e, err, wanted);
                q = rcstep_qscale(q, e->last_qscale_type[type], lstep, ovf > 1.1, ovf < 0.9);
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
 * top of these, which is the shape the reference encoder's ip/pb offsets give. */
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
                qp = rcstep_qp(qp, e->abr_qp - (e->bm_abr.meas[type] ? 4 : 2), e->abr_qp + 4);
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
            if (qpd > 0 && e->bm_abr.meas[type] && !anch) {
                double c = e->bm_abr.qp[type];
                qp = rcstep_qp(qp, c - qpd, c + qpd);
            }
            if (qp < 1) qp = 1;
            if (qp > 51) qp = 51;
            /* DEGENERATE-COMPLEXITY GUARD (Y264_ABR_CGUARD). A frame whose
 * complexity sits at the floor -- sintel opens on ~13 near-black
 * frames, C=1258 against a clip mean two orders larger -- carries no
 * information about the rate model, but it still ratchets abr_qp
 * down to the clamp. The +/-4 swing limit then needs ~9 frames to
 * climb back, and on sintel those are exactly the frames after the
 * scene cut: we spend 76% of the clip's bits there where the reference encoder spends
 * 24%. Under CRF, which has no cumulative state, we concentrate 9.8%
 * against the reference encoder's 17.6% -- so this is purely an RC-state defect, not
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
                /* The plan-less 2-pass share against the virtual remainder: in-flight frames
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
        if (type != 2) e->crf_dc = RCR_VAL(e->rcr, RCR_CRFDC, crf_frame_dc(e, src));
        rc_set_qp_crf(e, C, type);
    }
    /* else pass 1 / CQP: e->qp is the fixed base */
    if (e->vbv_on && type == 2 && !e->rcp_vbv_tight && e->bm_rvbv.have[2]) {
        /* Extrapolation guard: a pipelined B decides before any of its
 * burst's actuals land, and the bits model under-predicts savagely
 * below its calibrated QP regime (static content: skip-all frames
 * calibrate the scale at high QP; an ABR ladder chasing a full
 * buffer then dives 4 QP per DECIDE -- per frame serially, but per
 * burst-slot here -- and the first actual lands a 40x overshoot).
 * Floor the flying decide at calqp - QPD; the next anchor re-syncs
 * on actuals and the serial equations take it from there. Tight
 * bursts have per-frame actuals (the serial schedule) -- no floor. */
        int lo = rcbm_qp_floor(&e->bm_rvbv, 2, vbv_qpd_env());
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
        rb_note_budget(e, &en);
    }
    if (rcp_dbg_on())
        fprintf(stderr, "RCPD seq=%u type=%d ref=%d C=%.0f qp=%d np=%d pp=%.0f ca=%.0f s0=%.0f s1=%.0f s2=%.0f\n",
                atomic_load_explicit(&e->rcp_seq, memory_order_relaxed) + 1,
                type, is_ref, C, e->qp, np, pend_pred, e->abr_cum_actual,
                e->bm_abr.k[0], e->bm_abr.k[1], e->bm_abr.k[2]);
    en.base_qp = e->qp;
    /* A B frame codes on its anchor's base, DC included, so it is credited
 * in the same DC-free domain as the anchor. */
    en.dcq = abr_dc_on(e) ? e->crf_dc : 0.0;
    en.acc_qp = e->qp - en.dcq;
    if (e->abr_rf2 && type == 0) {
        /* Y264_ABR_RF2_IACC: 0 = credit an I at the P base current at its
 * decide (unbiased: bits at base = wanted at equilibrium whatever the
 * type mix); 1 = at the I's own coded QP (three below), which
 * under-credits intra bits by the I/P ratio and so biases the loop
 * toward spending on cut-heavy content. Default 1 (2026-09-04): +1..4%
 * closer to target and -0.8..-1.6% BD on foreman/bus/stefan/samsung. */
        if (e->rck.rf2_iacc > 0)
            en.acc_qp = en.fqp - en.dcq;
        else if (!(e->abr_cum_actual <= 0 && e->rcp_n == 0) && e->last_qscale_type[1] > 0)
            en.acc_qp = lround(12.0 + 6.0 * log2(e->last_qscale_type[1]));
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
void rcp_head(yah264_encoder_t *e, int type, int is_ref,
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
        e->rb_bud = e->rcp[idx].rb_bud; e->rb_pred = e->rcp[idx].rb_pred;
        return;
    }
    rcp_decide(e, type, is_ref, src);
}

/* The weight one pass-1 stats record contributes to the pass-2 allocation: the
 * QP-invariant coding cost, complexity-compressed. Exported because a caller
 * that splits a stats file along GOP boundaries has to size each GOP's budget
 * with the encoder's own formula -- a copy of it in the CLI would drift the
 * first time qcomp or the cost model moved, and the symptom (a slow rate drift
 * between the passes) would look like a rate-control bug rather than a stale
 * constant. Must stay in step with the tp_sum_cq accumulation in
 * yah264_encoder_open and with the 2-pass decide. */
double yah264_2pass_stat_weight(double bits, int qp)
{
    return pow((bits + 1) * pow(2.0, (qp - 12) / 6.0), Y264_TP_QCOMP);
}

/* --- Offline pass-2 plan ------------------------------------------------------
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
 * planned QPs came out I 32 / P 27 / B 21, exactly upside down. The reference encoder does
 * not let the curve decide this at all -- its cross-type limiter OVERWRITES
 * the I and B qscales as fixed ratios off the P qscale, and the complexity
 * term only ever moves the P frames.
 * - NO BOUND. Spending against a remaining budget is a closed loop with unit
 * gain and a biased plant: every frame that overshoots its model takes the
 * overshoot out of the frames after it. B frames overshot ~2x (dropping a
 * B from QP 28 to 21 buys far more bits than the 1/qscale model predicts,
 * because it un-skips blocks), so the tail hit the QP 51 rail. The reference encoder solves
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
double tp_qscale2qp(double q)   { return 12.0 + 6.0 * log2(q); }

/* Modelled bits for record i at qscale q. tp_bexp 1.0 reproduces the model the
 * greedy loop used (bits strictly inverse to qscale); the reference encoder uses 1.1 on the
 * texture term because skip blocks make texture bits fall off faster than
 * 1/qscale. With one lumped total the exponent is the only handle we have. */
double tp_bits_at(const yah264_encoder_t *e, int i, double q)
{
    if (q < 0.1) q = 0.1;
    if (e->tp_bexp == 1.0)
        return e->tp_cost[i] / q;
    return e->tp_cost[i] * pow(e->tp_qrec[i] / q, e->tp_bexp) / e->tp_qrec[i];
}

/* The reference's cross-type qscale limiter. Forces the I and B qscales
 * off the P qscale and limits step between consecutive frames of one type.
 *
 * Run in REVERSE record order, as the reference encoder does, and that is load-bearing rather
 * than a quirk: an I frame's quantiser should relate to the P frames it is
 * about to feed, not to the ones behind it, and reverse order is what puts the
 * following GOP's P average in ptrack_qp when the I frame is reached. Forward
 * order leaves the FIRST I frame of a slice with ptrack_norm == 0, which is
 * the fallback branch -- the I frame keeps its own complexity-derived qscale,
 * which is the whole defect. Under the threaded splitter every GOP section
 * starts with an I frame, so forward order would miss every one of them.
 *
 * the reference encoder's accum mask is 1 - (intra MBs / MBs)^2, which down-weights a P frame
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
        /* the reference encoder charges its pbratio only to a B that is NOT kept as a reference:
 * a pyramid ref-B carries the next frames and is held at its anchor's
 * qscale. is_ref comes off the stats record (5th field); a 4-field
 * record from an older pass 1 reads 0, i.e. the flat-B assumption. */
        if (!s->is_ref)
            q *= e->tp_pbf;
    }
    /* Step limit between consecutive frames of the same type. */
    if (d->last_non_b == type && (type != 0 || d->last_accum_p_norm < 1)) {
        q = rcstep_qscale(q, d->lastq[type], lstep, 0, 0);
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
void tp_build_plan(yah264_encoder_t *e)
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
            /* Plain two-sided gaussian. The reference encoder additionally decays the running
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
    double base_cplx = (double)mbs * 120.0;      /* the reference encoder, with B frames */

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


double tp_plan_qscale(yah264_encoder_t *e, int idx, double spent, double pend)
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
 * It replaces the reference encoder's two correction terms, which both failed here for
 * the same reason: the reference encoder measures its bias against a bits model that
 * splits texture, motion and header, and ours lumps them (pass 1
 * records one total). Our bias is therefore large -- 0.88 on
 * stefan_cif -- and the reference encoder's cumulative-average term chases it without
 * ever arriving: the ratio walked 0.96 -> 0.88 over 90 frames while
 * the encode finished 4.3% under target. A global re-solve does not
 * have to converge, because it reads the remaining budget directly. */
        double rem_m = e->tp_ebits[e->tp_n] - e->tp_ebits[idx];
        double rem_t = e->tp_target - spent - pend;
        if (rem_m > 1 && rem_t > 1 && k > 0) {
            double s = rem_t / (k * rem_m);
            /* Same band the reference encoder bounds its drift term with: one frame can move at
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
    /* the reference encoder gates this on being one second into the STREAM. Under the GOP
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

/* CRF content adaptation, Y264_CRF_CPLX, DEFAULT ON. MEASUREMENT TRAP: a
 * matched-CRF-NUMBER sweep (`bdcompare --points`) reads this gate as a large
 * LOSS (samsung +9.30%, touchdown +10.71%) because translating the CRF axis is
 * most of what it does; that is ladder placement, not quality. Gated with
 * `scripts/bd_at_rate.py` at matched achieved BITRATE, 12 clips, four targets
 * each: median -5.17%, 11 of 12 negative, worst +1.24% at the anchor below.
 * ducks -20.69, park_joy -11.86, tempete -8.32, bus -7.83. It is the largest
 * quality arm here by about a factor of three.
 *
 * Why it is that big: the whole quality gap to the reference encoder is mb-tree, and the tell is
 * the intra frame's share of the bits -- theirs takes half of it away on the
 * clips we lose and ours does not move the split. This gate is exactly the DC
 * that moves it.
 *
 * Y264_CRF_CPLX=0 disables it.
 *
 * the reference encoder's CRF has no frame-level complexity term at all once mb-tree is on:
 * the rate equation drops the blurred complexity and the rate
 * equation becomes duration-only, so the base QP is the fixed pedestal
 * crf + 13.5*(1-qcomp). ALL of its content adaptation is the DC of the per-MB
 * offset field, and that DC exists because the reference encoder's variance AQ is anchored to an
 * ABSOLUTE constant, strength*(log2(ac_energy) - 14.427))
 * rather than to the frame mean. yah264 centres on the frame mean, which
 * removes exactly that DC -- which is why our CRF resolves to a flat crf+5.4.
 *
 * This gate turns on the two terms that differ, both behaviour-matched:
 * - the absolute AQ anchor (mbtree_invqscale), worth up to ~7 QP of spread;
 * - the frame-duration term in the base QP (rc_set_qp_crf), worth 0 at
 * 25 fps and +2.4 at 50.
 * Resolved in warm_lr_statics: read by the lookahead/mb-tree worker threads. */
int crf_cplx_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_CRF_CPLX"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Per-term overrides, so the bundle can be attributed. Unset = follow the
 * master gate; 0/1 forces the term on its own. The mb-tree strength has
 * Y264_MBTREE_STRENGTH already and the B cascade has Y264_CRF_PBSCALE, so
 * these two complete the set. */
int crf_aqabs_env(void)
{
    static int v = -3;
    if (v == -3) { const char *s = getenv("Y264_CRF_AQABS"); v = s ? (atoi(s) ? 1 : 0) : -1; }
    return v < 0 ? crf_cplx_env() : v;
}

/* True only when the knob was set EXPLICITLY, so the ABR round can arm the
 * absolute anchor outside CRF without the default flip doing it silently. */
int crf_aqabs_forced(void)
{
    const char *s = getenv("Y264_CRF_AQABS");
    return s && atoi(s);
}


int crf_fps_env(void)
{
    static int v = -3;
    if (v == -3) { const char *s = getenv("Y264_CRF_FPS"); v = s ? (atoi(s) ? 1 : 0) : -1; }
    return v < 0 ? crf_cplx_env() : v;
}

/* The CRF pedestal calibration, in QP. The absolute anchor replaces a
 * per-frame mean with a constant, so it shifts the whole corpus's operating
 * point as well as spreading it; this puts that level back where the reference encoder's is.
 * Separate from the anchor on purpose -- the anchor also feeds the mb-tree
 * intra-cost weight, so moving the level with it distorts the boost, while the
 * pedestal is linear and BD-neutral by construction.
 *
 * -3.0, and it is PAIRED WITH THE ANCHOR: the anchor sets how far the offsets
 * sit from zero, so re-tuning one re-levels the other. At anchor 7.5 the
 * pedestal wants -2.0 (mean +0.9% vs the reference encoder's size at equal CRF); at anchor 5.5
 * that same -2.0 runs 5-24% under and -3.0 restores it (7 clips: mean -1.1%,
 * spread -14.0%..+8.7%). The knob is coarse because the base QP is rounded to an
 * integer per frame where the reference encoder keeps a float, so -2.0 and -2.5 land on the same
 * QP for most clips -- there is no rung between them.
 *
 * It is also the only part of the gate the BD table cannot see: a pure 1-QP
 * level move measures 0.00% BD-VMAF-NEG on ducks, samsung and foreman. */
/* Zero under the derived mb-tree mode: the pedestal exists to put OUR fitted anchor's
 * level back where the reference encoder's is, and the mode uses the reference encoder's anchor and the reference encoder's
 * strength, so there is nothing left to correct -- the reference encoder has no such term. The
 * mode's level therefore lands wherever the faithful constants put it, which is
 * why its gate has to be a matched-BITRATE one (scripts/bd_at_rate.py) and not
 * a matched-CRF-number sweep. */
double crf_ped_env(void)
{
    static double v = -1e9;
    if (v < -1e8) { const char *s = getenv("Y264_CRF_PED");
                    v = s ? atof(s) : (y264_mbt_derived() ? 0.0 : -3.0); }
    return v;
}


int crf_pb0_env(void)
{
    static int v = -3;
    if (v == -3) { const char *s = getenv("Y264_CRF_PB0"); v = s ? (atoi(s) ? 1 : 0) : -1; }
    return v < 0 ? crf_cplx_env() : v;
}

/* B-frame QP cascade scale under the gate. The reference encoder forces the pbratio to 1
 * whenever mb-tree is on, so its pb_offset is 0 and a B
 * codes at the anchor's base QP: its whole B economy is that non-reference B's
 * miss the mb-tree offsets, which yah264 already reproduces. Our extra 1..d+1
 * cascade therefore double-counts, and since B's are ~3/4 of the frames it is
 * most of the residual under-spend at equal CRF. 0 = the reference encoder's value -- but that value
 * is not ours to take: dropping the cascade is worth -8.92% BD on ducks and
 * +7.00% on sintel, a bidirectional trade with no net, so the default keeps
 * yah264's cascade and this stays a probe.
 *
 * The derived mb-tree mode takes the 0. The bidirectional trade above is measured against
 * OUR mb-tree field; under the reference encoder's field and the reference encoder's strength the B's inherit a
 * different offset distribution, so the cascade's compensation is priced
 * against a different thing. Whether it still costs ducks 8.9% is one of the
 * questions the mode's gate answers, and Y264_CRF_PBSCALE=1 splits it out. */
double crf_pbscale_env(void)
{
    static double v = -1e9;
    if (v < -1e8) { const char *s = getenv("Y264_CRF_PBSCALE");
                    v = s ? atof(s) : (y264_mbt_derived() ? 0.0 : 1.0); }
    return v;
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
int rcp_qpd_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RCP_QPD"); v = s ? atoi(s) : 0; }
    return v;
}

/* rcp warm length (serial-tight decides per encoder before the lag begins)
 * and the ABR error-correction gain; tunables for re-calibration. Warmed
 * statics: decides can run on the stair driver thread. */
int rcp_warm_n(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RCP_WARM"); v = s ? atoi(s) : 12; }
    return v;
}


double rcp_gain(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_RCP_GAIN"); v = s ? atof(s) : 0.1; }
    return v;
}

/* ABR qcompress for the pipelined decide. Warmed static: async-chain decides
 * run on the stair driver thread. */
double abr_qcomp_env(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_ABR_QCOMP"); v = s ? atof(s) : 0.6; }
    return v;
}

/* Y264_RCP_DBG=1: trace rcp decides/fills/pops. Warmed static: decides can
 * run on the stair driver thread. */
int rcp_dbg_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RCP_DBG"); v = s ? (atoi(s) ? 1 : 0) : 0; }
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
double vbv_rhi_env(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_VBV_RHI"); v = s ? atof(s) : 2.0; }
    return v;
}

/* Y264_VBV_FORCE=1: measurement-only -- skip the serial fallback so every
 * burst pipelines (prediction-error instrumentation needs in-flight entries).
 * Config-deterministic like every rcp input; never set in production. */
int vbv_force_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_FORCE"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
}

/* Y264_VBV_BOUND=1: bound EVERY frame against its measured coded size, not
 * just the instance's first. Default off -- it costs the stair, for the reason
 * vbv_bound_all sets out. Resolved in warm_lr_statics. */
int vbv_bound_env(void)
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
int vbv_qpd_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_QPD"); v = s ? atoi(s) : 6; }
    return v;
}


double vbv_cjump_env(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_VBV_CJUMP"); v = s ? atof(s) : 4.0; }
    return v;
}

/* Y264_VBV_CW: complexity-weighted VBV scale (see rcp_account). Default on.
 * Warmed static. */
int vbv_cw_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_CW"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Y264_VBV_FITLAW (default 1): 1 = the fit test reads the occupancy the frame
 * is removed from; 0 = the pre-2026-10-03 law that also counted the frame's own
 * refill (measurement arm: it underflows). Warmed static. */
int vbv_fitlaw_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_FITLAW"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* Y264_VBV_SEGH (default 0.5): the handoff occupancy a joined segment assumes
 * and the exit corridor steers to, as a fraction of the buffer. Warmed static. */
double vbv_segh_env(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_VBV_SEGH"); v = s ? atof(s) : 0.5;
                 if (v < 0) v = 0; if (v > 1) v = 1; }
    return v;
}

/* Y264_VBV_STAT=1: report burst/fallback/clamp counts at encoder close. */
int vbv_stat_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_STAT"); v = s ? (atoi(s) ? 1 : 0) : 0; }
    return v;
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
double abr_vbvov_frac(void)
{
    static double v = -1.0;
    if (v < 0) { const char *s = getenv("Y264_ABR_VBVOV"); v = s ? atof(s) : 0.25; if (v < 0) v = 0; }
    return v;
}

/* Under the rate-factor ABR controller, scale the B QP
 * cascade by the TARGET rate. On the 29-clip table the controller lost to
 * the previous model only at the top of the rate range (riverbed 12.5 and
 * crowd_run 24 Mbit/s), and the cascade at 0 recovered ducks (-7.4) while
 * costing sintel (+6.7): the two ends want opposite cascades, keyed by bits
 * per pixel per frame. Scale 1 at or below 0.10 bpp,
 * 0 at or above 0.40, linear between. A rate-keyed rule, not a
 * content-feature selector (that class is closed).
 * DEFAULT ON (2026-09-03). ABR ladders vs the unscaled controller, t1,
 * 120f: ducks -7.50%, riverbed -1.52, crowd_run -1.17, park_joy -0.24;
 * samsung, sintel, foreman, stockholm +0.00 (scale 1 there, byte-identical).
 * Always on; the Y264_ABR_PBRATE switch and its _LO/_HI overrides are gone. */
double abr_pbrate_scale(const yah264_encoder_t *e)
{
    const double lo = 0.10, hi = 0.40;   /* bits per pixel per frame */
    if (!e->abr_rf2 || !e->mbtree_on || e->width <= 0 || e->height <= 0) return 1.0;
    double bpp = e->abr_target_bpf / ((double)e->width * e->height);
    if (bpp <= lo) return 1.0;
    if (bpp >= hi || hi <= lo) return 0.0;
    return (hi - bpp) / (hi - lo);
}

/* The bytes of a versioned rc_state the caller declared, or 0 when its size
 * is outside [YAH264_RC_STATE_MIN_SIZE, sizeof]. A field is present when it
 * ends inside that size. */
static size_t rc_state_size(int size)
{
    if (size < (int)YAH264_RC_STATE_MIN_SIZE || size > (int)sizeof(yah264_rc_state_t)) return 0;
    return (size_t)size;
}
#define RC_STATE_HAS(n, f) (offsetof(yah264_rc_state_t, f) + sizeof(((yah264_rc_state_t *)0)->f) <= (n))

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
int vbv_bound_all(const yah264_encoder_t *e)
{
    return e->vbv_on && e->vbv_bound_on && !e->abr_on && !e->tp_pass;
}

/* Capped ABR: the measured-size bound on every frame of a TIGHT burst.
 *
 * The burst gate already sends a burst down the serial schedule when its
 * predicted trajectory comes near the bottom of the buffer, after an IDR, while
 * warm and on a content jump; there the stair and fpipe are off and each frame
 * reaches emit_frame's serial body, so the retry window costs nothing but W2's
 * overlap on those frames. What it buys is the one thing the prediction cannot:
 * the post-cut P after a black run predicts at 1.1 Mbit from a scale trained on
 * skip-only frames and codes 7.7 Mbit against a 2 Mbit buffer (chimera 1080p,
 * 988 kbit/s, 1.5x cap). Coding it, reading the size and re-coding at a higher
 * QP is exact where every model is a guess.
 *
 * The gate is a pure function of decided state, so the same frames are bounded
 * at every thread count. Y264_VBV_ABOUND=0 restores the unbounded emit. */
int vbv_abound_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_VBV_ABOUND"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}


static int vbv_abr_bounded(const yah264_encoder_t *e)
{
    /* A VBV always rides the pipeline now, so the !rcp_on term (the old
 * serial-VBV case) never fires; it costs nothing and keeps the predicate
 * honest if the pipeline is ever made optional again. */
    /* ...and every frame of an instance with no worker pool. Without one there
 * is no stair and no W2 (both need the pool), so every frame already takes
 * the serial emit and the retry window costs nothing but the re-codes that
 * fire. Non-tight bursts there used to escape the bound for consistency
 * with the threaded path, which threaded output no longer needs (owner,
 * 2026-09-25), and an unpromoted cut in one of them is exactly what the
 * prediction cannot see: samsung 720p, CRF with a 480 kbit cap, one
 * thread, a 307 kbit anchor into a 277 kbit buffer once weighted P stopped
 * weighting the brightness drift before the cut. */
    return e->vbv_on && !e->tp_pass && (!e->rcp_on || e->rcp_vbv_tight || !e->pool)
        && vbv_abound_env();
}

/* Either bound wants the serial emit: W2 does not know a frame's size until
 * w2_drain, by which point the NAL is appended and the RC has consumed it. */
int vbv_bounded_now(const yah264_encoder_t *e)
{
    return vbv_first_bounded(e) || vbv_bound_all(e) || vbv_abr_bounded(e);
}



YAH264_EXPORT int yah264_encoder_rc_import(yah264_encoder_t *e, const yah264_rc_state_t *c, int frames_ahead)
{
    if (e && e->hw) return -1;
    if (!e || !c || !c->valid || !e->abr_on) return -1;
    size_t csz = rc_state_size(c->size);
    if (!csz) return -1;
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
        e->bm_abr.k[t] = c->scale[t]; e->bm_abr.have[t] = c->inited[t];
        e->bm_abr.meas[t] = c->cal[t]; e->bm_abr.qp[t] = c->calqp[t];
        e->last_qscale_type[t] = c->last_qscale_type[t];
    }
    double n_done = c->target_bpf > 0 ? c->rf_wanted_bits / c->target_bpf : 0.0;
    e->rf_cplx_sum = c->rf_cplx_sum + (n_done > 0 ? n * (c->rf_cplx_sum / n_done) : 0.0);
    e->rf_wanted_bits = c->rf_wanted_bits + n * e->abr_target_bpf;
    e->ptrack_qp = c->ptrack_qp; e->ptrack_norm = c->ptrack_norm;
    e->last_ref_qp[0] = c->last_ref_qp[0]; e->last_ref_qp[1] = c->last_ref_qp[1];
    e->st_cplxsum = c->st_cplxsum; e->st_cplxcount = c->st_cplxcount;
    e->last_nonb_type = c->last_nonb_type;
    if (RC_STATE_HAS(csz, rf2_kc_cal))
        for (int t = 0; t < 3; t++) { e->bm_rf2kc.k[t] = c->rf2_kc[t]; e->bm_rf2kc.have[t] = c->rf2_kc_cal[t]; }
    e->rf2_opened = 1;                  /* the opening belongs to the first instance only */
    /* The exporter coded the segment right before this one: its final
 * occupancy is this segment's first, exactly, so it replaces the handoff
 * assumption. With frames of other instances in between it is not, and
 * the assumption stands. */
    if (e->vbv_on && RC_STATE_HAS(csz, vbv_have) && c->vbv_have && n == 0) {
        double f = c->vbv_fill;
        if (f > e->vbv.size) f = e->vbv.size;
        if (f < 0) f = 0;
        e->vbv.fill = f;
    }
    return 0;
}


YAH264_EXPORT yah264_rc_shared_t *yah264_rc_shared_new(void)
{
    struct yah264_rc_shared *s = calloc(1, sizeof *s);
    if (s) pthread_mutex_init(&s->mx, NULL);
    return s;
}


YAH264_EXPORT void yah264_rc_shared_free(yah264_rc_shared_t *s)
{
    if (!s) return;
    pthread_mutex_destroy(&s->mx);
    free(s);
}


YAH264_EXPORT int yah264_encoder_rc_share(yah264_encoder_t *e, yah264_rc_shared_t *s)
{
    if (!e || e->hw || !s || !e->abr_on || !e->abr_rf2 || e->tp_pass) return -1;
    if (e->frame_count > 0 || e->rcp_n > 0) return -1;
    e->rc_shared = s;
    pthread_mutex_lock(&s->mx);
    s->nlive++;
    pthread_mutex_unlock(&s->mx);
    return 0;
}


YAH264_EXPORT int yah264_encoder_vbv_handoff(yah264_encoder_t *e, int nframes)
{
    if (!e || e->hw || !e->vbv_on || nframes <= 0) return -1;
    if (e->frame_count > 0 || e->vbv_ndec > 0) return -1;
    e->vbv_exit_n = nframes;
    /* Half of the refill a frame leaves at the average rate (the target
 * under a bitrate, the cap otherwise), floored so a CBR-like pair, which
 * leaves none on average, still gets a corridor rather than a wall. */
    double keep = e->abr_on && e->param.rc.vbv_maxrate > 0
                ? 1.0 - (double)e->param.rc.bitrate / e->param.rc.vbv_maxrate : 0.5;
    double k = 0.5 * keep;
    if (k < 0.1) k = 0.1;
    e->vbv_exit_k = k;
    return 0;
}


YAH264_EXPORT int yah264_encoder_rc_state(const yah264_encoder_t *e, yah264_rc_state_t *dst)
{
    size_t dsz = dst ? rc_state_size(dst->size) : 0;
    if (!dsz) return -1;
    yah264_rc_state_t full, *out = &full;
    memset(out, 0, sizeof *out);
    out->size = dst->size;
    if (!e || e->hw || !e->abr_on) { memcpy(dst, out, dsz); return -1; }
    out->valid = 1;
    out->target_bpf = e->abr_target_bpf;
    out->cum_target = e->abr_cum_target; out->cum_actual = e->abr_cum_actual;
    out->qp = e->abr_qp;
    for (int t = 0; t < 3; t++) {
        out->scale[t] = e->bm_abr.k[t]; out->inited[t] = e->bm_abr.have[t];
        out->cal[t] = e->bm_abr.meas[t]; out->calqp[t] = e->bm_abr.qp[t];
        out->last_qscale_type[t] = e->last_qscale_type[t];
    }
    out->rf_cplx_sum = e->rf_cplx_sum; out->rf_wanted_bits = e->rf_wanted_bits;
    out->ptrack_qp = e->ptrack_qp; out->ptrack_norm = e->ptrack_norm;
    out->last_ref_qp[0] = e->last_ref_qp[0]; out->last_ref_qp[1] = e->last_ref_qp[1];
    out->st_cplxsum = e->st_cplxsum; out->st_cplxcount = e->st_cplxcount;
    out->last_nonb_type = e->last_nonb_type;
    for (int t = 0; t < 3; t++) { out->rf2_kc[t] = e->bm_rf2kc.k[t]; out->rf2_kc_cal[t] = e->bm_rf2kc.have[t]; }
    if (e->vbv_on) { out->vbv_fill = e->vbv.fill; out->vbv_have = 1; }
    memcpy(dst, out, dsz);
    return 0;
}
