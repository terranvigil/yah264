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
    c->a_kbps = e->abr_on ? e->param.rc.bitrate : 0;
    c->a_maxrate = e->vbv_on ? e->param.rc.vbv_maxrate : 0;
    c->a_bufsize = e->vbv_on ? e->param.rc.vbv_bufsize : 0;
    c->fps = open_fps(e);
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
    free(c->fq.ev);
    free(c->rq.ev);
    free(c->done.ev);
    free(c->zones);
    free(c);
}

static yah264_control_t *q_at(const struct ctl_queue *q, int i)
{
    return &q->ev[q->head + i];
}

/* insert after every event at or before its frame (it is the newest);
 * returns 0 or -1 */
static int q_insert(struct ctl_queue *q, const yah264_control_t *ev)
{
    if (q->head + q->n == q->cap) {
        if (q->head > 0) {
            memmove(q->ev, q->ev + q->head, (size_t)q->n * sizeof *q->ev);
            q->head = 0;
        }
        if (q->n == q->cap) {
            int nc = q->cap ? 2 * q->cap : 16;
            yah264_control_t *a = realloc(q->ev, (size_t)nc * sizeof *a);
            if (!a) return -1;
            q->ev = a;
            q->cap = nc;
        }
    }
    int i = q->n;
    while (i > 0 && q_at(q, i - 1)->frame > ev->frame) {
        *q_at(q, i) = *q_at(q, i - 1);
        i--;
    }
    *q_at(q, i) = *ev;
    q->n++;
    return 0;
}

/* where q_insert would put an event naming `frame` */
static int q_slot_for(const struct ctl_queue *q, int64_t frame)
{
    int i = q->n;
    while (i > 0 && q_at(q, i - 1)->frame > frame) i--;
    return i;
}

/* append in arrival order; returns 0 or -1 */
static int q_append(struct ctl_queue *q, const yah264_control_t *ev)
{
    if (q->head + q->n == q->cap) {
        if (q->head > 0) {
            memmove(q->ev, q->ev + q->head, (size_t)q->n * sizeof *q->ev);
            q->head = 0;
        }
        if (q->n == q->cap) {
            int nc = q->cap ? 2 * q->cap : 16;
            yah264_control_t *a = realloc(q->ev, (size_t)nc * sizeof *a);
            if (!a) return -1;
            q->ev = a;
            q->cap = nc;
        }
    }
    *q_at(q, q->n++) = *ev;
    return 0;
}

/* a buffer past this many entries is given back as its queue drains */
#define CTL_Q_KEEP 4096

static void q_pop(struct ctl_queue *q)
{
    q->head++;
    if (--q->n == 0) {
        q->head = 0;
        if (q->cap > CTL_Q_KEEP) { free(q->ev); q->ev = NULL; q->cap = 0; }
    } else if (q->cap > CTL_Q_KEEP && q->n < q->cap / 4) {
        /* landed slots stay allocated until the queue is compacted */
        memmove(q->ev, q->ev + q->head, (size_t)q->n * sizeof *q->ev);
        q->head = 0;
        yah264_control_t *a = realloc(q->ev, (size_t)(q->cap / 2) * sizeof *a);
        if (a) { q->ev = a; q->cap /= 2; }
    }
}

/* under mx */
static void done_push(struct y264_ctl *c, const yah264_control_t *ev, int64_t at)
{
    if (at >= 0) c->applied++;
    yah264_control_t d = *ev;
    d.applied_frame = at;
    if (c->done.n >= CTL_DONE_MAX || q_append(&c->done, &d) != 0)
        c->err_lost = 1;            /* the event still applied; its record is gone */
}

void ctl_hand_in(yah264_encoder_t *e)
{
    struct y264_ctl *c = e->ctl;
    if (!c) return;
    int64_t d = c->in++;
    struct ctl_slot *s = &c->slot[d & (c->ring - 1)];
    if (s->disp >= 0 && !s->emitted)
        c->err_ring = 1;
    s->disp = d;
    s->t_in = ctl_now();
    s->emitted = 0;
    s->key = 0;
    s->zone_on = 0;
    s->zone_off = 0.0;
    if (atomic_load_explicit(&c->used, memory_order_relaxed)) {
        while (c->fq.n > 0 && q_at(&c->fq, 0)->frame <= d) {
            const yah264_control_t *ev = q_at(&c->fq, 0);
            if (ev->kind == YAH264_CTL_QP_LIMITS) {
                if (ev->qp_min >= 0) c->qp_min = ev->qp_min;
                if (ev->qp_max >= 0) c->qp_max = ev->qp_max;
            } else if (ev->kind == YAH264_CTL_KEYFRAME) {
                s->key = 1;
            }
            pthread_mutex_lock(&c->mx);
            done_push(c, ev, d);
            pthread_mutex_unlock(&c->mx);
            q_pop(&c->fq);
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

int ctl_hand_in_failed(const yah264_encoder_t *e)
{
    return e->ctl && e->ctl->err_ring;
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
    while (c->fq.n > 0) { done_push(c, q_at(&c->fq, 0), -1); q_pop(&c->fq); }
    while (c->rq.n > 0) { done_push(c, q_at(&c->rq, 0), -1); q_pop(&c->rq); }
    atomic_store_explicit(&c->nrq, 0, memory_order_release);
    pthread_mutex_unlock(&c->mx);
}

void ctl_rc_land_slow(yah264_encoder_t *e)
{
    struct y264_ctl *c = e->ctl;
    int64_t at = e->cur_disp;
    pthread_mutex_lock(&c->mx);
    while (c->rq.n > 0 && q_at(&c->rq, 0)->frame <= at) {
        const yah264_control_t *ev = q_at(&c->rq, 0);
        if (ev->kind == YAH264_CTL_RATE) {
            rc_ctl_retarget(e, ev->bitrate_kbps, ev->vbv_maxrate_kbps, ev->vbv_bufsize_kbit);
            if (ev->bitrate_kbps > 0) c->a_kbps = ev->bitrate_kbps;
            if (ev->vbv_maxrate_kbps > 0) c->a_maxrate = ev->vbv_maxrate_kbps;
            if (ev->vbv_bufsize_kbit > 0) c->a_bufsize = ev->vbv_bufsize_kbit;
        } else if (ev->kind == YAH264_CTL_CRF) {
            e->crf = ev->crf;
        }
        done_push(c, ev, at);
        q_pop(&c->rq);
    }
    atomic_store_explicit(&c->nrq, c->rq.n, memory_order_release);
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
    case YAH264_CTL_EFULL:        return "the control queue is full; resend once events have landed";
    default:                      return "unknown error";
    }
}

static int zone_overlaps(const yah264_encoder_t *e, const struct y264_ctl *c, int64_t first, int64_t last)
{
    for (int i = 0; i < c->nz; i++)
        if (first <= c->zones[i].last && c->zones[i].first <= last) return 1;
    for (int i = 0; i < e->nzones; i++)
        if (first <= e->zones[i].last && e->zones[i].first <= last) return 1;
    return 0;
}

/* a rate state the encode can be in */
static int rate_ok(const yah264_encoder_t *e, int k, int m, int b)
{
    if (e->abr_on && e->vbv_on && (e->vbv_cbr ? m != k : m <= k)) return YAH264_CTL_EMODE;
    if (e->hrd_on && ((double)m * 1000.0 > e->hrd_bitrate || (double)b * 1000.0 > e->hrd_cpb))
        return YAH264_CTL_EHRD;
    if ((k > m ? k : m) > y264_level_max_br(e)) return YAH264_CTL_ELEVEL;
    return YAH264_CTL_OK;
}

static void rate_apply(const yah264_control_t *ev, int *k, int *m, int *b)
{
    if (ev->bitrate_kbps > 0) *k = ev->bitrate_kbps;
    if (ev->vbv_maxrate_kbps > 0) *m = ev->vbv_maxrate_kbps;
    if (ev->vbv_bufsize_kbit > 0) *b = ev->vbv_bufsize_kbit;
}

/* Replay the applied state and every queued RATE in landing order with v in
 * its place: v must leave a valid state, and so must every later event.
 * Under mx. */
static int rate_check_queue(const yah264_encoder_t *e, const struct y264_ctl *c, const yah264_control_t *v)
{
    int k = c->a_kbps, m = c->a_maxrate, b = c->a_bufsize;
    int at = q_slot_for(&c->rq, v->frame), r;
    for (int i = 0; i <= c->rq.n; i++) {
        const yah264_control_t *ev = i < at ? q_at(&c->rq, i) : i == at ? v : q_at(&c->rq, i - 1);
        if (ev->kind != YAH264_CTL_RATE) continue;
        rate_apply(ev, &k, &m, &b);
        if ((r = rate_ok(e, k, m, b)) != YAH264_CTL_OK) return r;
    }
    return YAH264_CTL_OK;
}

/* the same for QP_LIMITS against the queued frame-exact events */
static int qp_check_queue(const struct y264_ctl *c, const yah264_control_t *v)
{
    int lo = c->qp_min, hi = c->qp_max;
    int at = q_slot_for(&c->fq, v->frame);
    for (int i = 0; i <= c->fq.n; i++) {
        const yah264_control_t *ev = i < at ? q_at(&c->fq, i) : i == at ? v : q_at(&c->fq, i - 1);
        if (ev->kind != YAH264_CTL_QP_LIMITS) continue;
        if (ev->qp_min >= 0) lo = ev->qp_min;
        if (ev->qp_max >= 0) hi = ev->qp_max;
        if (lo > hi) return YAH264_CTL_EINVAL;
    }
    return YAH264_CTL_OK;
}

/* the checks against the state at the event's frame (c NULL: the event
 * alone); RATE's queue replay is left to the caller, which holds mx for it */
static int ctl_check(const yah264_encoder_t *e, const struct y264_ctl *c, yah264_control_t *v)
{
    switch (v->kind) {
    case YAH264_CTL_RATE:
        if (v->bitrate_kbps < 0 || v->vbv_maxrate_kbps < 0 || v->vbv_bufsize_kbit < 0)
            return YAH264_CTL_EINVAL;
        if (!v->bitrate_kbps && !v->vbv_maxrate_kbps && !v->vbv_bufsize_kbit)
            return YAH264_CTL_EINVAL;
        if (e->tp_pass || e->rc_shared) return YAH264_CTL_EMODE;
        if (v->bitrate_kbps && !e->abr_on) return YAH264_CTL_EMODE;
        if ((v->vbv_maxrate_kbps || v->vbv_bufsize_kbit) && !e->vbv_on) return YAH264_CTL_EMODE;
        /* a CBR declaration pads the stream back up to its rate */
        if (e->hrd_on == YAH264_NAL_HRD_CBR) return YAH264_CTL_EHRD;
        /* CBR: the cap follows the target, and the echo carries it */
        if (e->vbv_cbr && v->bitrate_kbps && !v->vbv_maxrate_kbps) v->vbv_maxrate_kbps = v->bitrate_kbps;
        return YAH264_CTL_OK;
    case YAH264_CTL_CRF:
        if (!e->crf_on || e->tp_pass) return YAH264_CTL_EMODE;
        if (!(v->crf > 0.0 && v->crf <= 51.0)) return YAH264_CTL_EINVAL;
        return YAH264_CTL_OK;
    case YAH264_CTL_QP_LIMITS:
        if (!e->abr_on && !e->crf_on && !e->tp_pass) return YAH264_CTL_EMODE;   /* constant QP */
        if (v->qp_min < -1 || v->qp_min > 51 || v->qp_max < -1 || v->qp_max > 51)
            return YAH264_CTL_EINVAL;
        if (v->qp_min < 0 && v->qp_max < 0) return YAH264_CTL_EINVAL;
        /* the buffer may need the QPs above a ceiling, as at open */
        if (e->hrd_on && v->qp_max >= 0 && v->qp_max < 51) return YAH264_CTL_EHRD;
        return c ? qp_check_queue(c, v) : YAH264_CTL_OK;
    case YAH264_CTL_KEYFRAME:
        if (e->tp_pass) return YAH264_CTL_EMODE;
        return YAH264_CTL_OK;
    case YAH264_CTL_ZONE:
        if (v->last < v->frame) return YAH264_CTL_EINVAL;
        if (!(v->qp_offset >= -51.0 && v->qp_offset <= 51.0)) return YAH264_CTL_EINVAL;
        if (c && zone_overlaps(e, c, v->frame, v->last)) return YAH264_CTL_ECONFLICT;
        return YAH264_CTL_OK;
    case 0:
        return YAH264_CTL_EINVAL;
    default:
        return YAH264_CTL_EUNSUPPORTED;
    }
}

/* the caller's event in this library's layout, its frame resolved */
static int ctl_take(const struct y264_ctl *c, const yah264_control_t *ev, yah264_control_t *v)
{
    if (!ev || ev->size < CTL_V1_SIZE || ev->size > (int)sizeof(yah264_control_t))
        return YAH264_CTL_EINVAL;
    yah264_control_init(v, 0);
    memcpy(v, ev, (size_t)ev->size);
    v->size = (int)sizeof *v;
    v->applied_frame = -1;
    if (v->frame == YAH264_CTL_NEXT) v->frame = c->in;
    else if (v->frame < 0) return YAH264_CTL_EINVAL;
    else if (v->frame < c->in) return YAH264_CTL_ELATE;
    return YAH264_CTL_OK;
}

static int zone_add(struct y264_ctl *c, const yah264_control_t *v)
{
    if (c->nz == c->cz) {
        int nc = c->cz ? 2 * c->cz : 8;
        struct ctl_zone *z = realloc(c->zones, (size_t)nc * sizeof *z);
        if (!z) return -1;
        c->zones = z;
        c->cz = nc;
    }
    /* whole QPs, as a plan zone's offset is applied */
    c->zones[c->nz++] = (struct ctl_zone){ v->frame, v->last, (double)lround(v->qp_offset) };
    return 0;
}

static int q_copy(struct ctl_queue *d, const struct ctl_queue *q)
{
    memset(d, 0, sizeof *d);
    if (!q->n) return 0;
    /* room for one more, so the first insert doesn't double it */
    d->ev = malloc((size_t)(q->n + 1) * sizeof *d->ev);
    if (!d->ev) return -1;
    memcpy(d->ev, q->ev + q->head, (size_t)q->n * sizeof *d->ev);
    d->n = q->n;
    d->cap = q->n + 1;
    return 0;
}

int yah264_encoder_control_check(yah264_encoder_t *e, const yah264_control_t *ev, int n, int *bad_index)
{
    if (!e || (n > 0 && !ev) || n < 0 || !bad_index) {
        if (bad_index) *bad_index = -1;
        return YAH264_CTL_EINVAL;
    }
    *bad_index = -1;
    if (e->hw || !e->ctl) {
        if (n > 0) *bad_index = 0;
        return n > 0 ? YAH264_CTL_EUNSUPPORTED : YAH264_CTL_OK;
    }
    struct y264_ctl *c = e->ctl;
    /* a private copy of everything the checks read, which the events fold into */
    struct y264_ctl sh;
    memset(&sh, 0, sizeof sh);
    sh.in = c->in;
    sh.qp_min = c->qp_min;
    sh.qp_max = c->qp_max;
    int r = YAH264_CTL_OK;
    pthread_mutex_lock(&c->mx);
    sh.a_kbps = c->a_kbps; sh.a_maxrate = c->a_maxrate; sh.a_bufsize = c->a_bufsize;
    if (q_copy(&sh.rq, &c->rq) != 0) r = YAH264_CTL_ENOMEM;
    pthread_mutex_unlock(&c->mx);
    if (r == YAH264_CTL_OK && q_copy(&sh.fq, &c->fq) != 0) r = YAH264_CTL_ENOMEM;
    for (int i = 0; r == YAH264_CTL_OK && i < c->nz; i++)
        if (zone_add(&sh, &(yah264_control_t){ .frame = c->zones[i].first, .last = c->zones[i].last,
                                               .qp_offset = c->zones[i].off }) != 0)
            r = YAH264_CTL_ENOMEM;
    if (r != YAH264_CTL_OK) {
        free(sh.rq.ev); free(sh.fq.ev); free(sh.zones);
        return r;
    }
    /* The state after every queued event of each kind, so an event appended
     * at the tail (a log in frame order) is checked in constant time; one that
     * lands among the queued ones replays them. */
    int tk = sh.a_kbps, tm = sh.a_maxrate, tb = sh.a_bufsize, tlo = sh.qp_min, thi = sh.qp_max;
    for (int i = 0; i < sh.rq.n; i++)
        if (q_at(&sh.rq, i)->kind == YAH264_CTL_RATE) rate_apply(q_at(&sh.rq, i), &tk, &tm, &tb);
    for (int i = 0; i < sh.fq.n; i++) {
        const yah264_control_t *q = q_at(&sh.fq, i);
        if (q->kind != YAH264_CTL_QP_LIMITS) continue;
        if (q->qp_min >= 0) tlo = q->qp_min;
        if (q->qp_max >= 0) thi = q->qp_max;
    }
    int64_t zfirst = -1, zlast = -1;           /* the queued zones' latest start and end */
    for (int i = 0; i < sh.nz; i++) {
        if (sh.zones[i].first > zfirst) zfirst = sh.zones[i].first;
        if (sh.zones[i].last > zlast) zlast = sh.zones[i].last;
    }
    for (int i = 0; i < n; i++) {
        yah264_control_t v;
        r = ctl_take(&sh, &ev[i], &v);
        int tail = 0;
        if (r == YAH264_CTL_OK) {
            struct ctl_queue *q = v.kind == YAH264_CTL_RATE || v.kind == YAH264_CTL_CRF ? &sh.rq : &sh.fq;
            tail = q_slot_for(q, v.frame) == q->n;
        }
        if (r == YAH264_CTL_OK && tail && v.kind == YAH264_CTL_QP_LIMITS) {
            r = ctl_check(e, NULL, &v);         /* the event alone, then the tail state */
            int lo = v.qp_min >= 0 ? v.qp_min : tlo, hi = v.qp_max >= 0 ? v.qp_max : thi;
            if (r == YAH264_CTL_OK && lo > hi) r = YAH264_CTL_EINVAL;
        } else if (r == YAH264_CTL_OK && tail && v.kind == YAH264_CTL_ZONE && v.frame >= zfirst) {
            r = ctl_check(e, NULL, &v);
            if (r == YAH264_CTL_OK && zlast >= v.frame) r = YAH264_CTL_ECONFLICT;
            for (int z = 0; r == YAH264_CTL_OK && z < e->nzones; z++)
                if (v.frame <= e->zones[z].last && e->zones[z].first <= v.last) r = YAH264_CTL_ECONFLICT;
        } else if (r == YAH264_CTL_OK) {
            r = ctl_check(e, &sh, &v);
        }
        if (r == YAH264_CTL_OK && v.kind == YAH264_CTL_RATE) {
            if (tail) {
                int k = tk, m = tm, b = tb;
                rate_apply(&v, &k, &m, &b);
                r = rate_ok(e, k, m, b);
            } else {
                r = rate_check_queue(e, &sh, &v);
            }
        }
        if (r == YAH264_CTL_OK) {
            int k = v.kind == YAH264_CTL_RATE || v.kind == YAH264_CTL_CRF
                  ? q_insert(&sh.rq, &v) : q_insert(&sh.fq, &v);
            if (k == 0 && v.kind == YAH264_CTL_ZONE) k = zone_add(&sh, &v);
            if (k != 0) r = YAH264_CTL_ENOMEM;
        }
        if (r != YAH264_CTL_OK) { *bad_index = i; break; }
        /* fold it into the tail states */
        if (v.kind == YAH264_CTL_RATE) {
            if (tail) {
                rate_apply(&v, &tk, &tm, &tb);
            } else {
                tk = sh.a_kbps; tm = sh.a_maxrate; tb = sh.a_bufsize;
                for (int j = 0; j < sh.rq.n; j++)
                    if (q_at(&sh.rq, j)->kind == YAH264_CTL_RATE) rate_apply(q_at(&sh.rq, j), &tk, &tm, &tb);
            }
        } else if (v.kind == YAH264_CTL_QP_LIMITS) {
            if (tail) {
                if (v.qp_min >= 0) tlo = v.qp_min;
                if (v.qp_max >= 0) thi = v.qp_max;
            } else {
                tlo = sh.qp_min; thi = sh.qp_max;
                for (int j = 0; j < sh.fq.n; j++) {
                    const yah264_control_t *q = q_at(&sh.fq, j);
                    if (q->kind != YAH264_CTL_QP_LIMITS) continue;
                    if (q->qp_min >= 0) tlo = q->qp_min;
                    if (q->qp_max >= 0) thi = q->qp_max;
                }
            }
        } else if (v.kind == YAH264_CTL_ZONE) {
            if (v.frame > zfirst) zfirst = v.frame;
            if (v.last > zlast) zlast = v.last;
        }
    }
    free(sh.rq.ev); free(sh.fq.ev); free(sh.zones);
    return r;
}

/* events in the queues naming a frame after the next one; under mx */
static int ctl_future(const struct y264_ctl *c)
{
    return (c->rq.n - q_slot_for(&c->rq, c->in)) + (c->fq.n - q_slot_for(&c->fq, c->in));
}

/* undrained records count too, so every queued event has a record slot
 * waiting for it */
static int ctl_full(const struct y264_ctl *c, const yah264_control_t *v)
{
    return c->rq.n + c->fq.n + c->done.n >= CTL_DONE_MAX
        || (v->frame > c->in && ctl_future(c) >= CTL_QUEUE_MAX);
}

int yah264_encoder_control(yah264_encoder_t *e, yah264_control_t *ev)
{
    if (!e || !ev) return YAH264_CTL_EINVAL;
    if (e->hw || !e->ctl) return YAH264_CTL_EUNSUPPORTED;
    struct y264_ctl *c = e->ctl;
    yah264_control_t v;
    int r0 = ctl_take(c, ev, &v);
    if (r0 != YAH264_CTL_OK) return r0;
    int r = ctl_check(e, c, &v);
    if (r != YAH264_CTL_OK) return r;
    if (v.kind == YAH264_CTL_RATE || v.kind == YAH264_CTL_CRF) {
        pthread_mutex_lock(&c->mx);
        r = ctl_full(c, &v) ? YAH264_CTL_EFULL
          : v.kind == YAH264_CTL_RATE ? rate_check_queue(e, c, &v) : YAH264_CTL_OK;
        if (r == YAH264_CTL_OK && q_insert(&c->rq, &v) != 0) r = YAH264_CTL_ENOMEM;
        atomic_store_explicit(&c->nrq, c->rq.n, memory_order_release);
        pthread_mutex_unlock(&c->mx);
        if (r != YAH264_CTL_OK) return r;
    } else {
        pthread_mutex_lock(&c->mx);
        int full = ctl_full(c, &v);
        pthread_mutex_unlock(&c->mx);
        if (full) return YAH264_CTL_EFULL;
        if (v.kind == YAH264_CTL_ZONE && zone_add(c, &v) != 0) return YAH264_CTL_ENOMEM;
        if (q_insert(&c->fq, &v) != 0) {
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
    if (c->err_lost) {
        c->err_lost = 0;
        pthread_mutex_unlock(&c->mx);
        return -1;
    }
    int n = c->done.n < max ? c->done.n : max;
    for (int i = 0; i < n; i++) {
        int sz = out[i].size;
        memcpy(&out[i], q_at(&c->done, 0), (size_t)sz);
        out[i].size = sz;
        q_pop(&c->done);
    }
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
        s.target_kbps = c->a_kbps;
        s.vbv_maxrate_kbps = c->a_maxrate;
        s.vbv_bufsize_kbit = c->a_bufsize;
        s.vbv_fill_bits = c->s_have_fill ? c->s_fill : 0.0;
        s.rate_balance_bits = c->s_have_bal ? c->s_balance : 0.0;
        s.budget_bits = c->s_have_fill ? c->s_fill : c->s_have_bal ? c->s_balance : 0.0;
        s.controls_pending = c->fq.n + c->rq.n;
        s.controls_applied = c->applied;
        pthread_mutex_unlock(&c->mx);
    }
    int sz = st->size;
    memcpy(st, &s, (size_t)sz);
    st->size = sz;
    return 0;
}
