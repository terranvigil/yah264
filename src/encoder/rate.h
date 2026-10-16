// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The interface between encoder.c and rate.c (internal).

#ifndef Y264_RATECONTROL_H
#define Y264_RATECONTROL_H

#include "encoder.h"
#include <pthread.h>

/* The shared rate ledger (yah264_encoder_rc_share): the four running sums the
 * RF2 decide reads, summed over every attached instance. */
struct yah264_rc_shared {
    pthread_mutex_t mx;
    double cplx_sum, wanted, actual, target;
    int nlive;              /* instances attached and not yet closed */
};

/* --- Deterministic fixed-lag RC feedback -----------------------------------
 * ABR/2-pass decisions read the committed ledger
 * plus PREDICTIONS for the in-flight frames; actuals commit on a schedule
 * keyed purely to DECIDE order -- a burst pops right after the next anchor's
 * decision -- so the RC-visible op sequence, and therefore the bitstream, is
 * identical at every thread count and pipeline engagement level (unlike
 * the reference encoder's threaded ABR). Fills stage actual bits in coding order on the API
 * thread and never touch the ledger; decides run serially in coding order
 * (API thread, or the stair driver between the serial_done / chain-submit
 * handshakes, which order every rcp access). */
#define RCP_MAX ((int)(sizeof ((yah264_encoder_t *)0)->rcp \
                     / sizeof ((yah264_encoder_t *)0)->rcp[0]))
#define RCP_WARM rcp_warm_n()

/* encoder.c -> rate.c and back: every function the other file calls. */
double abr_qcomp_env(void);
double abr_vbvov_frac(void);
double blk_ac_energy(const pixel *p, int stride, int w, int h);
int crf_aqabs_env(void);
int frame_qp(const yah264_encoder_t *e, int type, int is_ref);
int qp_bound(const yah264_encoder_t *e, int q);
int rcp_dbg_on(void);
double rcp_gain(void);
int rcp_qpd_env(void);
int rcp_warm_n(void);
double tp_bits_at(const yah264_encoder_t *e, int i, double q);
double tp_plan_qscale(yah264_encoder_t *e, int idx, double spent, double pend);
double tp_qscale2qp(double q);
double vbv_cjump_env(void);
int vbv_cw_env(void);
int vbv_fitlaw_env(void);
int vbv_force_env(void);
int vbv_qpd_env(void);
double vbv_rhi_env(void);
int abr_cguard_on(void);
double abr_cguard_thresh(void);
int abr_rf_env(void);
int abr_rfqp_trace(void);
double anchor_leg_cost(const yah264_encoder_t *e);
double crf_max_clip(const yah264_encoder_t *e, double base, double raised, int off);
double frame_complexity_ilr(const yah264_encoder_t *e);
double frame_complexity_me(const yah264_encoder_t *e);
double rb_walk_env(void);
void rc_head_crf(yah264_encoder_t *e, int type, pixel *const src[3], int want_c);
void rc_head_serial(yah264_encoder_t *e, int type, int is_ref, pixel *const src[3]);
void rc_knobs_resolve(yah264_encoder_t *e);
int rc_share_ovn_env(void);
void rcp_decide(yah264_encoder_t *e, int type, int is_ref, pixel *const src[3]);
void rcp_drop(yah264_encoder_t *e);
void rcp_fill(yah264_encoder_t *e, double bits);
void rcp_flush_all(yah264_encoder_t *e);
void rcp_head(yah264_encoder_t *e, int type, int is_ref, pixel *const src[3]);
void rcp_reqp(yah264_encoder_t *e, int type, int is_ref);
void rcp_vbv_gate(yah264_encoder_t *e, int is_idr);
double rcp_vbv_vfill(const yah264_encoder_t *e, int drop_tail);
int rcp_warm(const yah264_encoder_t *e);
double rcstep_qscale(double q, double last, double lstep, int widen_hi, int widen_lo);
double vbv_limit_at(const yah264_encoder_t *e, double fill);
void tp_build_plan(yah264_encoder_t *e);
double abr_pbrate_scale(const yah264_encoder_t *e);
int crf_aqabs_forced(void);
int crf_cplx_env(void);
int crf_pb0_env(void);
double crf_pbscale_env(void);
int vbv_bound_env(void);
double vbv_segh_env(void);
int vbv_stat_on(void);
int vbv_bound_all(const yah264_encoder_t *e);
int vbv_bounded_now(const yah264_encoder_t *e);
int vbv_abound_env(void);

#endif
