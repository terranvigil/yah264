// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Live control (yah264_encoder_control), the encoder side (internal).
//
// Three kinds of state, split by who touches them:
//   - a ring of per-frame slots, indexed by input frame, written on the API
//     thread when a picture is handed in and read when that frame is typed or
//     coded. It carries what lands on exact input frames (QP limits, zone
//     offsets, keyframe requests) and the status bookkeeping. A slot is
//     tagged with its frame, so a reader never takes another frame's values.
//   - events that land at a rate-control decision (RATE, CRF), queued under
//     a mutex and applied by the decide hook, which may run off the API
//     thread.
//   - events that land on exact frames, queued on the API thread only and
//     folded into the slots at hand-in.

#ifndef Y264_CONTROL_H
#define Y264_CONTROL_H

#include "encoder.h"
#include <pthread.h>
#include <stdatomic.h>

struct ctl_slot {
    int64_t disp;           /* the frame this slot holds, -1 = none */
    int     qp_min, qp_max; /* in force on this frame */
    int     zone_on;
    double  zone_off;
    int     key;            /* the caller asked for an IDR here */
    int     emitted;
    double  t_in;           /* monotonic seconds at hand-in */
};

struct ctl_zone { int64_t first, last; double off; };

struct y264_ctl {
    int              ring;      /* power of two, past the deepest pipeline */
    struct ctl_slot *slot;
    atomic_int       used;      /* an event was ever accepted: slots carry controls */

    /* API thread */
    int64_t          in, out, oldest;
    int64_t          seq;
    yah264_control_t *fq;       /* frame-exact events, sorted by (frame, seq) */
    int64_t         *fq_seq;
    int              nfq, cfq;
    struct ctl_zone *zones;
    int              nz, cz;
    int              qp_min, qp_max;        /* in force at the next hand-in */
    int              q_kbps, q_maxrate, q_bufsize;  /* after every queued RATE */
    double           fps;

    /* shared with the decide hook, under mx */
    pthread_mutex_t  mx;
    atomic_int       nrq;       /* pending decision events (lock-free peek) */
    yah264_control_t *rq;       /* sorted by (frame, seq) */
    int64_t         *rq_seq;
    int              crq;
    yah264_control_t *done;     /* applied, not yet drained */
    int              ndone, cdone;
    int64_t          applied;
    double           s_kbps, s_maxrate, s_bufsize, s_fill, s_balance;
    int              s_have_fill, s_have_bal;
};

struct y264_ctl *ctl_new(const yah264_encoder_t *e);
void ctl_free(struct y264_ctl *c);

/* API thread, once per picture handed in, before it enters the coding flow */
void ctl_hand_in(yah264_encoder_t *e);
/* API thread, once per coded frame returned */
void ctl_note_emit(yah264_encoder_t *e, int disp);
/* API thread, on the flush call that returns nothing */
void ctl_flush_end(yah264_encoder_t *e);
/* the decide hook: apply RATE/CRF events due at e->cur_disp */
void ctl_rc_land_slow(yah264_encoder_t *e);
/* the status snapshot, at the end of a rate decision */
void ctl_rc_snapshot(yah264_encoder_t *e);

static inline void ctl_rc_land(yah264_encoder_t *e)
{
    if (e->ctl && atomic_load_explicit(&e->ctl->nrq, memory_order_acquire) > 0)
        ctl_rc_land_slow(e);
}

/* The slot of input frame `disp` when it carries controls, else NULL. */
static inline const struct ctl_slot *ctl_slot_at(const yah264_encoder_t *e, int disp)
{
    const struct y264_ctl *c = e->ctl;
    if (!c || disp < 0 || !atomic_load_explicit(&c->used, memory_order_relaxed)) return NULL;
    const struct ctl_slot *s = &c->slot[disp & (c->ring - 1)];
    return s->disp == disp ? s : NULL;
}

static inline int ctl_key_at(const yah264_encoder_t *e, int disp)
{
    const struct ctl_slot *s = ctl_slot_at(e, disp);
    return s && s->key;
}

/* rate.c: retarget the ledger and the buffer in place (0 = unchanged) */
void rc_ctl_retarget(yah264_encoder_t *e, int kbps, int maxrate, int bufsize);
/* encoder.c: the highest rate (kbit/s) the declared level allows */
long y264_level_max_br(const yah264_encoder_t *e);

#endif
