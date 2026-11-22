// Copyright (c) 2026, the yah264 authors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Live control: per-frame events with a defined landing frame, and the
// backpressure status (docs/live-control.md).

#include "control.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the first version's struct ends at the resolution fields */
#define CTL_V1_SIZE   ((int)(offsetof(yah264_control_t, height) + sizeof(int)))
#define STAT_V1_SIZE  ((int)(offsetof(yah264_status_t, controls_applied) + sizeof(int64_t)))

static double ctl_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static double open_fps(const yah264_encoder_t *e)
{
    const yah264_param_t *p = &e->param;
    return p->timebase.fps_num > 0 && p->timebase.fps_den > 0
         ? (double)p->timebase.fps_num / p->timebase.fps_den : 25.0;
}

struct y264_ctl *ctl_new(const yah264_encoder_t *e)
{
    struct y264_ctl *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    /* deeper than anything between hand-in and coding (lookahead, reorder,
     * denoiser, frames in flight) */
    int need = e->la_cap + 4 * (e->bframes + 1) + 256;
    int r = 512;
    while (r < need) r <<= 1;
    c->ring = r;
    c->slot = malloc((size_t)r * sizeof *c->slot);
    if (!c->slot) { free(c); return NULL; }
    for (int i = 0; i < r; i++) { memset(&c->slot[i], 0, sizeof c->slot[i]); c->slot[i].disp = -1; }
    pthread_mutex_init(&c->mx, NULL);
    atomic_init(&c->nrq, 0);
    atomic_init(&c->used, 0);
    c->qp_min = e->qp_min;
    c->qp_max = e->qp_max;
    c->q_kbps = e->abr_on ? e->param.rc.bitrate : 0;
    c->q_maxrate = e->vbv_on ? e->param.rc.vbv_maxrate : 0;
    c->q_bufsize = e->vbv_on ? e->param.rc.vbv_bufsize : 0;
    c->fps = open_fps(e);
    c->s_kbps = c->q_kbps;
    c->s_maxrate = c->q_maxrate;
    c->s_bufsize = c->q_bufsize;
    c->s_fill = e->vbv_on ? e->vbv.fill : 0.0;
    c->s_have_fill = e->vbv_on;
    c->s_have_bal = e->abr_on;
    return c;
}

void ctl_free(struct y264_ctl *c)
{
    if (!c) return;
    pthread_mutex_destroy(&c->mx);
    free(c->slot);
    free(c->fq); free(c->fq_seq);
    free(c->rq); free(c->rq_seq);
    free(c->zones);
    free(c->done);
    free(c);
}

/* insert keeping (frame, seq) order; returns 0 or -1 */
static int q_insert(yah264_control_t **q, int64_t **qs, int *n, int *cap,
                    const yah264_control_t *ev, int64_t seq)
{
    if (*n == *cap) {
        int nc = *cap ? 2 * *cap : 16;
        yah264_control_t *a = realloc(*q, (size_t)nc * sizeof *a);
        if (!a) return -1;
        *q = a;
        int64_t *b = realloc(*qs, (size_t)nc * sizeof *b);
        if (!b) return -1;
        *qs = b;
        *cap = nc;
    }
    int i = *n;
    while (i > 0 && (*q)[i - 1].frame > ev->frame) {
        (*q)[i] = (*q)[i - 1];
        (*qs)[i] = (*qs)[i - 1];
        i--;
    }
    (*q)[i] = *ev;
    (*qs)[i] = seq;
    (*n)++;
    return 0;
}

static void q_pop_front(yah264_control_t *q, int64_t *qs, int *n)
{
    (*n)--;
    memmove(q, q + 1, (size_t)*n * sizeof *q);
    memmove(qs, qs + 1, (size_t)*n * sizeof *qs);
}

/* under mx */
static void done_push(struct y264_ctl *c, const yah264_control_t *ev, int64_t at)
{
    if (c->ndone == c->cdone) {
        int nc = c->cdone ? 2 * c->cdone : 16;
        yah264_control_t *a = realloc(c->done, (size_t)nc * sizeof *a);
        if (!a) return;             /* the event still applied; only its record is lost */
        c->done = a;
        c->cdone = nc;
    }
    yah264_control_t *d = &c->done[c->ndone++];
    *d = *ev;
    d->applied_frame = at;
    if (at >= 0) c->applied++;
}

void ctl_hand_in(yah264_encoder_t *e)
{
    struct y264_ctl *c = e->ctl;
    if (!c) return;
    int64_t d = c->in++;
    struct ctl_slot *s = &c->slot[d & (c->ring - 1)];
    s->disp = d;
    s->t_in = ctl_now();
    s->emitted = 0;
    s->key = 0;
    s->zone_on = 0;
    s->zone_off = 0.0;
    if (atomic_load_explicit(&c->used, memory_order_relaxed)) {
        while (c->nfq > 0 && c->fq[0].frame <= d) {
            const yah264_control_t *ev = &c->fq[0];
            if (ev->kind == YAH264_CTL_QP_LIMITS) {
                if (ev->qp_min >= 0) c->qp_min = ev->qp_min;
                if (ev->qp_max >= 0) c->qp_max = ev->qp_max;
                /* only an out-of-order pair of events gets here crossed */
                if (c->qp_min > c->qp_max) c->qp_min = c->qp_max;
            } else if (ev->kind == YAH264_CTL_KEYFRAME) {
                s->key = 1;
            }
            pthread_mutex_lock(&c->mx);
            done_push(c, ev, d);
            pthread_mutex_unlock(&c->mx);
            q_pop_front(c->fq, c->fq_seq, &c->nfq);
        }
        int w = 0;
        for (int i = 0; i < c->nz; i++) {
            if (c->zones[i].last < d) continue;     /* past: drop */
            if (c->zones[i].first <= d) { s->zone_on = 1; s->zone_off = c->zones[i].off; }
            c->zones[w++] = c->zones[i];
        }
        c->nz = w;
    }
    s->qp_min = c->qp_min;
    s->qp_max = c->qp_max;
}

void ctl_note_emit(yah264_encoder_t *e, int disp)
{
    struct y264_ctl *c = e->ctl;
    if (!c) return;
    struct ctl_slot *s = &c->slot[disp & (c->ring - 1)];
    if (s->disp == disp) s->emitted = 1;
    c->out++;
    while (c->oldest < c->in) {
        const struct ctl_slot *o = &c->slot[c->oldest & (c->ring - 1)];
        if (o->disp != c->oldest || !o->emitted) break;
        c->oldest++;
    }
}

void ctl_flush_end(yah264_encoder_t *e)
{
    struct y264_ctl *c = e->ctl;
    if (!c) return;
    pthread_mutex_lock(&c->mx);
    for (int i = 0; i < c->nfq; i++) done_push(c, &c->fq[i], -1);
    c->nfq = 0;
    int n = atomic_load_explicit(&c->nrq, memory_order_relaxed);
    for (int i = 0; i < n; i++) done_push(c, &c->rq[i], -1);
    atomic_store_explicit(&c->nrq, 0, memory_order_release);
    pthread_mutex_unlock(&c->mx);
}

void ctl_rc_land_slow(yah264_encoder_t *e)
{
    struct y264_ctl *c = e->ctl;
    int64_t at = e->cur_disp;
    pthread_mutex_lock(&c->mx);
    int n = atomic_load_explicit(&c->nrq, memory_order_relaxed);
    while (n > 0 && c->rq[0].frame <= at) {
        const yah264_control_t *ev = &c->rq[0];
        if (ev->kind == YAH264_CTL_RATE) {
            rc_ctl_retarget(e, ev->bitrate_kbps, ev->vbv_maxrate_kbps, ev->vbv_bufsize_kbit);
            if (ev->bitrate_kbps > 0) c->s_kbps = ev->bitrate_kbps;
            if (ev->vbv_maxrate_kbps > 0) c->s_maxrate = ev->vbv_maxrate_kbps;
            if (ev->vbv_bufsize_kbit > 0) c->s_bufsize = ev->vbv_bufsize_kbit;
        } else if (ev->kind == YAH264_CTL_CRF) {
            e->crf = ev->crf;
        }
        done_push(c, ev, at);
        q_pop_front(c->rq, c->rq_seq, &n);
    }
    atomic_store_explicit(&c->nrq, n, memory_order_release);
    pthread_mutex_unlock(&c->mx);
}

void ctl_rc_snapshot(yah264_encoder_t *e)
{
    struct y264_ctl *c = e->ctl;
    if (!c) return;
    pthread_mutex_lock(&c->mx);
    if (e->vbv_on) c->s_fill = e->vbv.fill;
    if (e->abr_on) c->s_balance = e->abr_cum_target - e->abr_cum_actual;
    pthread_mutex_unlock(&c->mx);
}

void yah264_control_init(yah264_control_t *ev, int kind)
{
    if (!ev) return;
    memset(ev, 0, sizeof *ev);
    ev->size = (int)sizeof *ev;
    ev->kind = kind;
    ev->frame = YAH264_CTL_NEXT;
    ev->applied_frame = -1;
    ev->qp_min = ev->qp_max = -1;
    ev->last = -1;
}

const char *yah264_control_strerror(int err)
{
    switch (err) {
    case YAH264_CTL_OK:           return "accepted";
    case YAH264_CTL_EINVAL:       return "invalid event: bad size, field or value";
    case YAH264_CTL_EUNSUPPORTED: return "this event kind is not supported";
    case YAH264_CTL_ELATE:        return "the landing frame has already been handed to the encoder";
    case YAH264_CTL_EMODE:        return "the event does not fit this encode's rate-control mode";
    case YAH264_CTL_EHRD:         return "the change would break the stream's signalled HRD (--nal-hrd)";
    case YAH264_CTL_ELEVEL:       return "the change exceeds the stream's declared level";
    case YAH264_CTL_ECONFLICT:    return "the zone overlaps a zone already set";
    case YAH264_CTL_ENOMEM:       return "out of memory";
    default:                      return "unknown error";
    }
}

static int zone_overlaps(const yah264_encoder_t *e, int64_t first, int64_t last)
{
    const struct y264_ctl *c = e->ctl;
    for (int i = 0; i < c->nz; i++)
        if (first <= c->zones[i].last && c->zones[i].first <= last) return 1;
    for (int i = 0; i < e->nzones; i++)
        if (first <= e->zones[i].last && e->zones[i].first <= last) return 1;
    return 0;
}

static int ctl_check(yah264_encoder_t *e, yah264_control_t *v)
{
    struct y264_ctl *c = e->ctl;
    switch (v->kind) {
    case YAH264_CTL_RATE: {
        if (v->bitrate_kbps < 0 || v->vbv_maxrate_kbps < 0 || v->vbv_bufsize_kbit < 0)
            return YAH264_CTL_EINVAL;
        if (!v->bitrate_kbps && !v->vbv_maxrate_kbps && !v->vbv_bufsize_kbit)
            return YAH264_CTL_EINVAL;
        if (e->tp_pass || e->rc_shared) return YAH264_CTL_EMODE;
        if (v->bitrate_kbps && !e->abr_on) return YAH264_CTL_EMODE;
        if ((v->vbv_maxrate_kbps || v->vbv_bufsize_kbit) && !e->vbv_on) return YAH264_CTL_EMODE;
        /* a CBR declaration pads the stream back up to its rate */
        if (e->hrd_on == YAH264_NAL_HRD_CBR) return YAH264_CTL_EHRD;
        int nk = v->bitrate_kbps ? v->bitrate_kbps : c->q_kbps;
        int nm = v->vbv_maxrate_kbps ? v->vbv_maxrate_kbps : c->q_maxrate;
        int nb = v->vbv_bufsize_kbit ? v->vbv_bufsize_kbit : c->q_bufsize;
        if (e->vbv_cbr && v->bitrate_kbps && !v->vbv_maxrate_kbps)
            nm = nk;                            /* CBR: the cap follows the target */
        if (e->abr_on && e->vbv_on) {
            if (e->vbv_cbr ? nm != nk : nm <= nk) return YAH264_CTL_EMODE;
        }
        /* under a VBR declaration the stream may spend less than declared, never more */
        if (e->hrd_on && ((double)nm * 1000.0 > e->hrd_bitrate || (double)nb * 1000.0 > e->hrd_cpb))
            return YAH264_CTL_EHRD;
        long br = nk > nm ? nk : nm;
        if (br > y264_level_max_br(e)) return YAH264_CTL_ELEVEL;
        /* the event carries what it resolves to, so the echo replays it */
        if (e->vbv_cbr && v->bitrate_kbps && !v->vbv_maxrate_kbps) v->vbv_maxrate_kbps = nm;
        return YAH264_CTL_OK;
    }
    case YAH264_CTL_CRF:
        if (!e->crf_on || e->tp_pass) return YAH264_CTL_EMODE;
        if (!(v->crf > 0.0 && v->crf <= 51.0)) return YAH264_CTL_EINVAL;
        return YAH264_CTL_OK;
    case YAH264_CTL_QP_LIMITS: {
        if (!e->abr_on && !e->crf_on && !e->tp_pass) return YAH264_CTL_EMODE;   /* constant QP */
        if (v->qp_min < -1 || v->qp_min > 51 || v->qp_max < -1 || v->qp_max > 51)
            return YAH264_CTL_EINVAL;
        if (v->qp_min < 0 && v->qp_max < 0) return YAH264_CTL_EINVAL;
        int lo = v->qp_min >= 0 ? v->qp_min : c->qp_min;
        int hi = v->qp_max >= 0 ? v->qp_max : c->qp_max;
        for (int i = 0; i < c->nfq; i++)        /* what the queue leaves in force */
            if (c->fq[i].kind == YAH264_CTL_QP_LIMITS && c->fq[i].frame <= v->frame) {
                if (v->qp_min < 0 && c->fq[i].qp_min >= 0) lo = c->fq[i].qp_min;
                if (v->qp_max < 0 && c->fq[i].qp_max >= 0) hi = c->fq[i].qp_max;
            }
        if (lo > hi) return YAH264_CTL_EINVAL;
        /* the buffer may need the QPs above a ceiling, as at open */
        if (e->hrd_on && v->qp_max >= 0 && v->qp_max < 51) return YAH264_CTL_EHRD;
        return YAH264_CTL_OK;
    }
    case YAH264_CTL_KEYFRAME:
        if (e->tp_pass) return YAH264_CTL_EMODE;
        return YAH264_CTL_OK;
    case YAH264_CTL_ZONE:
        if (v->last < v->frame) return YAH264_CTL_EINVAL;
        if (!(v->qp_offset >= -51.0 && v->qp_offset <= 51.0)) return YAH264_CTL_EINVAL;
        if (zone_overlaps(e, v->frame, v->last)) return YAH264_CTL_ECONFLICT;
        return YAH264_CTL_OK;
    default:
        return YAH264_CTL_EUNSUPPORTED;
    }
}

int yah264_encoder_control(yah264_encoder_t *e, yah264_control_t *ev)
{
    if (!e || !ev || ev->size < CTL_V1_SIZE || ev->size > (int)sizeof(yah264_control_t))
        return YAH264_CTL_EINVAL;
    if (e->hw || !e->ctl) return YAH264_CTL_EUNSUPPORTED;
    struct y264_ctl *c = e->ctl;
    yah264_control_t v;
    yah264_control_init(&v, 0);
    memcpy(&v, ev, (size_t)ev->size);
    v.size = (int)sizeof v;
    if (v.frame == YAH264_CTL_NEXT) v.frame = c->in;
    else if (v.frame < 0) return YAH264_CTL_EINVAL;
    else if (v.frame < c->in) return YAH264_CTL_ELATE;
    v.applied_frame = -1;
    int r = ctl_check(e, &v);
    if (r != YAH264_CTL_OK) return r;
    int64_t seq = c->seq++;
    if (v.kind == YAH264_CTL_RATE || v.kind == YAH264_CTL_CRF) {
        pthread_mutex_lock(&c->mx);
        int n = atomic_load_explicit(&c->nrq, memory_order_relaxed);
        int ok = q_insert(&c->rq, &c->rq_seq, &n, &c->crq, &v, seq) == 0;
        atomic_store_explicit(&c->nrq, n, memory_order_release);
        pthread_mutex_unlock(&c->mx);
        if (!ok) return YAH264_CTL_ENOMEM;
        if (v.kind == YAH264_CTL_RATE) {
            if (v.bitrate_kbps) c->q_kbps = v.bitrate_kbps;
            if (v.vbv_maxrate_kbps) c->q_maxrate = v.vbv_maxrate_kbps;
            if (v.vbv_bufsize_kbit) c->q_bufsize = v.vbv_bufsize_kbit;
        }
    } else {
        if (v.kind == YAH264_CTL_ZONE) {
            if (c->nz == c->cz) {
                int nc = c->cz ? 2 * c->cz : 8;
                struct ctl_zone *z = realloc(c->zones, (size_t)nc * sizeof *z);
                if (!z) return YAH264_CTL_ENOMEM;
                c->zones = z;
                c->cz = nc;
            }
            /* whole QPs, as a plan zone's offset is applied */
            c->zones[c->nz++] = (struct ctl_zone){ v.frame, v.last, (double)lround(v.qp_offset) };
        }
        if (q_insert(&c->fq, &c->fq_seq, &c->nfq, &c->cfq, &v, seq) != 0) {
            if (v.kind == YAH264_CTL_ZONE) c->nz--;
            return YAH264_CTL_ENOMEM;
        }
    }
    atomic_store_explicit(&c->used, 1, memory_order_relaxed);
    ev->frame = v.frame;
    if (v.kind == YAH264_CTL_RATE && ev->size > (int)offsetof(yah264_control_t, vbv_maxrate_kbps))
        ev->vbv_maxrate_kbps = v.vbv_maxrate_kbps;
    return YAH264_CTL_OK;
}

int yah264_encoder_controls_applied(yah264_encoder_t *e, yah264_control_t *out, int max)
{
    if (!e || !out || max < 0) return -1;
    struct y264_ctl *c = e->ctl;
    if (!c) return 0;
    for (int i = 0; i < max; i++)
        if (out[i].size < CTL_V1_SIZE || out[i].size > (int)sizeof(yah264_control_t)) return -1;
    pthread_mutex_lock(&c->mx);
    int n = c->ndone < max ? c->ndone : max;
    for (int i = 0; i < n; i++) {
        int sz = out[i].size;
        memcpy(&out[i], &c->done[i], (size_t)sz);
        out[i].size = sz;
    }
    c->ndone -= n;
    memmove(c->done, c->done + n, (size_t)c->ndone * sizeof *c->done);
    pthread_mutex_unlock(&c->mx);
    return n;
}

int yah264_encoder_status(yah264_encoder_t *e, yah264_status_t *st)
{
    if (!e || !st || st->size < STAT_V1_SIZE || st->size > (int)sizeof(yah264_status_t)) return -1;
    yah264_status_t s;
    memset(&s, 0, sizeof s);
    struct y264_ctl *c = e->ctl;
    if (c) {
        s.frames_in = c->in;
        s.frames_out = c->out;
        s.queue_depth = (int)(c->in - c->out);
        s.next_frame = c->in;
        s.queue_ms = c->fps > 0 ? s.queue_depth * 1000.0 / c->fps : 0.0;
        if (c->oldest < c->in) {
            const struct ctl_slot *o = &c->slot[c->oldest & (c->ring - 1)];
            if (o->disp == c->oldest) s.oldest_age_ms = (ctl_now() - o->t_in) * 1000.0;
        }
        pthread_mutex_lock(&c->mx);
        s.target_kbps = c->s_kbps;
        s.vbv_maxrate_kbps = c->s_maxrate;
        s.vbv_bufsize_kbit = c->s_bufsize;
        s.vbv_fill_bits = c->s_have_fill ? c->s_fill : 0.0;
        s.rate_balance_bits = c->s_have_bal ? c->s_balance : 0.0;
        s.budget_bits = c->s_have_fill ? c->s_fill : c->s_have_bal ? c->s_balance : 0.0;
        s.controls_pending = c->nfq + atomic_load_explicit(&c->nrq, memory_order_relaxed);
        s.controls_applied = c->applied;
        pthread_mutex_unlock(&c->mx);
    }
    int sz = st->size;
    memcpy(st, &s, (size_t)sz);
    st->size = sz;
    return 0;
}
