/*
 * gpu.c - see gpu.h.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gpu.h"
#include "../common/env.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The library is 8-bit today (NGC_DEPTH_8), and our `pixel` is uint16 above
 * 8-bit, so the offload is confined to the 8-bit build rather than silently
 * reinterpreting samples. */
#if defined(Y264_HAVE_GPU) && Y264_BIT_DEPTH == 8

#include <ngc.h>
#include <pthread.h>

/* ONE Metal context per process, not per handle. ngc_open creates the device
 * and loads (or compiles) the shader library -- tens of milliseconds -- and
 * the CLI opens a probe encoder plus one encoder per GOP worker, so
 * per-handle contexts would pay that init on every one. Metal devices,
 * libraries and pipelines are thread-safe and immutable; per-handle state is
 * the STREAM (its own command queue) and the buffers. */
static pthread_once_t g_ngc_once = PTHREAD_ONCE_INIT;
static ngc_ctx *g_ngc_shared;
static int g_ngc_lw, g_ngc_lh;
static void ngc_open_once(void)
{
    ngc_config cfg = { .max_width = (uint32_t)g_ngc_lw,
                       .max_height = (uint32_t)g_ngc_lh,
                       .depth = NGC_DEPTH_8, .families = NGC_FAMILY_SEARCH,
                       .vmaf_model_path = NULL };
    g_ngc_shared = ngc_open(&cfg);
}

/* ------------------------------- gpq (per-push Phase-A legs; see gpu.h) --- */

int y264_gpq_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *s = y264_env("Y264_GPU_PHASEA");
        v = s ? (atoi(s) ? 1 : 0) : 0;
    }
    return v;
}

#define GPQ_RANGE 8

struct gpq_round {
    long       key;                 /* boundary push this round submits at (0 = unused) */
    ngc_fence *fence;               /* non-NULL = submitted, not yet waited */
    int        waiting;             /* a consumer is fence-waiting OUTSIDE the lock */
    int        failed;              /* fence wait failed (round unusable) */
    int16_t   *legof;               /* [rel*reach*2 + (d-1)*2 + dir] -> leg index, -1 = absent */
    ngc_buf   *out;                 /* batch*2*reach legs, nblk records each */
};

struct y264_gpq {
    ngc_ctx    *ctx;
    ngc_stream *stream;
    int   lw, lh, nblk, nslots, reach, batch;
    int   nrounds;                  /* round-table entries (rounds are batch pushes apart) */
    int   npast;                    /* slot_of table size, in pushes */
    int   dead;                     /* sticky: no coverage from here on */
    pthread_cond_t cv;              /* field: waiters for a fence someone else holds */
    pthread_t init_th;              /* background init: Metal open + ALL buffer
 * mappings, spawned at open so the ~16 ms
 * per-process cost overlaps CLI setup and
 * the ring's lead instead of the encode.
 * The first push JOINS it, so coverage
 * never depends on init timing. */
    int   init_started, init_joined, init_ok;
    pthread_mutex_t mx;             /* guards fence wait + round publication */
    ngc_buf         **plane;        /* one per la ring slot */
    long             *plane_push;   /* push each slot currently holds (0 = none) */
    int              *slot_of;      /* [push % npast] -> ring slot of that push */
    struct gpq_round *round;
    ngc_job          *job;          /* pending batch: up to batch*2*reach jobs */
    int               njob;
    long              cur_key;      /* boundary the pending batch will submit at */
};

static void gpq_die(y264_gpq *g, const char *why)
{
    if (!g->dead)
        fprintf(stderr, "yah264: Y264_GPU_PHASEA disabled mid-encode (%s); "
                        "output may differ from a healthy run\n", why);
    g->dead = 1;
}

#define GPQ_BATCH 4

/* Pushes 1..W get no coverage BY ARITHMETIC, so the background init runs
 * behind the first W frames' encode instead of stalling the chain at push 1
 * (at startup the ring's lead does not exist in TIME -- input arrives
 * instantly and the driver's first pops wait on the chain). Deterministic:
 * timing decides only how long push W+1's join waits, never what is covered. */
static int gpq_warmup_env(void)
{
    static int v = -1;
    if (v < 0) { const char *s = y264_env("Y264_GPQ_WARMUP"); v = s ? atoi(s) : 24;
                 if (v < 0) v = 0; }
    return v;
}

#define GPQ_KEY(g, p) ((((p) + (g)->batch - 1) / (g)->batch) * (g)->batch)
#define GPQ_RIDX(g, k) ((int)(((k) / (g)->batch) % (g)->nrounds))

static void *gpq_init_main(void *arg);
static int   gpq_init_join(y264_gpq *g);

y264_gpq *y264_gpq_open(int lw, int lh, int nslots, int reach)
{
    if (!y264_gpq_mode() || lw < 16 || lh < 16 || nslots <= 0 || reach <= 0)
        return NULL;
    y264_gpq *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->lw = lw; g->lh = lh; g->nblk = (lw / 8) * (lh / 8);
    g->nslots = nslots; g->reach = reach; g->batch = GPQ_BATCH;
    /* Strictly more table capacity than any consumer can reach back (gpu.h):
 * consumers live within nslots pushes of the newest. */
    g->npast   = nslots + 2 * reach + 4;
    g->nrounds = (nslots + 2 * reach) / g->batch + 4;
    g->plane      = calloc((size_t)nslots, sizeof *g->plane);
    g->plane_push = calloc((size_t)nslots, sizeof *g->plane_push);
    g->slot_of    = calloc((size_t)g->npast, sizeof *g->slot_of);
    g->round      = calloc((size_t)g->nrounds, sizeof *g->round);
    g->job        = calloc((size_t)g->batch * 2 * reach, sizeof *g->job);
    if (!g->plane || !g->plane_push || !g->slot_of || !g->round || !g->job) {
        y264_gpq_close(g); return NULL;
    }
    for (int i = 0; i < g->nrounds; i++) {
        g->round[i].legof = malloc((size_t)g->batch * reach * 2 * sizeof(int16_t));
        if (!g->round[i].legof) { y264_gpq_close(g); return NULL; }
    }
    pthread_mutex_init(&g->mx, NULL);
    pthread_cond_init(&g->cv, NULL);
    /* Init on a BACKGROUND thread, not lazily at the first push and not here:
 * the Metal open plus ~50 MTLBuffer mappings measure ~16 ms per process,
 * which is 14% of a CIF board cell if the encode pays it. Spawned now, it
 * overlaps the CLI's own setup and the lookahead ring's lead; the first
 * push joins it. (The gpu.c lazy-init lesson still holds for handles that
 * never push: y264_gpq_open only exists on the la ring, which the CLI's
 * probe encoder does not run.) */
    g->init_started = pthread_create(&g->init_th, NULL, gpq_init_main, g) == 0;
    if (!g->init_started) { y264_gpq_close(g); return NULL; }
    return g;
}

void y264_gpq_close(y264_gpq *g)
{
    if (!g) return;
    gpq_init_join(g);
    for (int i = 0; g->round && i < g->nrounds; i++) {
        if (g->round[i].fence) { ngc_fence_wait(g->round[i].fence);
                                 ngc_fence_release(g->round[i].fence); }
        if (g->round[i].out) ngc_buf_free(g->round[i].out);
        free(g->round[i].legof);
    }
    for (int i = 0; g->plane && i < g->nslots; i++)
        if (g->plane[i]) ngc_buf_free(g->plane[i]);
    if (g->stream) ngc_stream_close(g->stream);
    free(g->plane); free(g->plane_push); free(g->slot_of);
    free(g->round); free(g->job);
    pthread_mutex_destroy(&g->mx);
    pthread_cond_destroy(&g->cv);
    free(g);
}

static int gpq_ensure(y264_gpq *g)
{
    if (g->stream) return 1;
    if (!g->ctx) {
        g_ngc_lw = g->lw; g_ngc_lh = g->lh;
        pthread_once(&g_ngc_once, ngc_open_once);
        g->ctx = g_ngc_shared;
        if (!g->ctx) return 0;
    }
    if (!(g->stream = ngc_stream_open(g->ctx))) return 0;
    return 1;
}


/* Name the calling thread so a profile attributes by role (E4: unnamed
 * threads made the per-thread counter split useless). Best effort, no
 * error path: a name is a diagnostic, never a dependency. */
static void y264_thread_name(const char *name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

static void *gpq_init_main(void *arg)
{
    y264_thread_name("y264-gpu");
    y264_gpq *g = arg;
    if (!gpq_ensure(g)) return NULL;
    size_t legsz = (size_t)g->nblk * sizeof(y264_gpq_blk);
    for (int i = 0; i < g->nslots; i++)
        if (!g->plane[i] &&
            !(g->plane[i] = ngc_buf_alloc(g->ctx, (size_t)g->lw * g->lh)))
            return NULL;
    for (int i = 0; i < g->nrounds; i++)
        if (!g->round[i].out &&
            !(g->round[i].out = ngc_buf_alloc(g->ctx,
                  legsz * 2 * (size_t)g->reach * g->batch)))
            return NULL;
    g->init_ok = 1;
    return NULL;
}

static int gpq_init_join(y264_gpq *g)
{
    if (!g->init_started) return 0;
    if (!g->init_joined) { pthread_join(g->init_th, NULL); g->init_joined = 1; }
    return g->init_ok;
}

void y264_gpq_push(y264_gpq *g, long push, int slot, const pixel *lowres)
{
    if (!g || g->dead || push <= gpq_warmup_env() || slot < 0 || slot >= g->nslots
        || !lowres)
        return;
    if (!gpq_init_join(g)) { gpq_die(g, "init"); return; }

    /* Upload this push's lowres into its ring slot's device buffer. Metal
 * reads buffers at EXECUTION time, so first fence every round that could
 * still read the slot's previous plane (the rounds covering pushes
 * Q..Q+reach for the push Q it last held). Those are la_cap pushes old by
 * now, so the waits are no-ops in any healthy run -- this is the
 * structural guarantee, not a cost. */
    if (g->plane_push[slot] > 0) {
        long q0w = GPQ_KEY(g, g->plane_push[slot]);
        long q1w = GPQ_KEY(g, g->plane_push[slot] + g->reach);
        pthread_mutex_lock(&g->mx);
        for (long k = q0w; k <= q1w; k += g->batch) {
            struct gpq_round *rw = &g->round[GPQ_RIDX(g, k)];
            while (rw->key == k && rw->waiting)     /* a consumer holds the wait */
                pthread_cond_wait(&g->cv, &g->mx);
            if (rw->key == k && rw->fence) {
                ngc_fence_wait(rw->fence);
                ngc_fence_release(rw->fence);
                rw->fence = NULL;
            }
        }
        pthread_mutex_unlock(&g->mx);
    }
    memcpy(ngc_buf_ptr(g->plane[slot]), lowres, (size_t)g->lw * g->lh);
    g->plane_push[slot] = push;
    g->slot_of[push % g->npast] = slot;

    /* Start a new batch at its first push: retire whatever round the table
 * position last held (consumers can never reach a round that old). The
 * round stays UNPUBLISHED (key 0) until its submit, so field cannot
 * match it half-built. */
    long K = GPQ_KEY(g, push);
    struct gpq_round *r = &g->round[GPQ_RIDX(g, K)];
    size_t legsz = (size_t)g->nblk * sizeof(y264_gpq_blk);
    if (g->cur_key != K) {
        pthread_mutex_lock(&g->mx);
        while (r->waiting)                          /* a consumer holds the wait */
            pthread_cond_wait(&g->cv, &g->mx);
        if (r->fence) { ngc_fence_wait(r->fence); ngc_fence_release(r->fence); }
        r->fence = NULL; r->key = 0; r->failed = 0;
        pthread_mutex_unlock(&g->mx);
        memset(r->legof, -1, (size_t)g->batch * g->reach * 2 * sizeof(int16_t));
        g->cur_key = K;
        g->njob = 0;
    }

    /* Accumulate this push's legs into the pending batch. */
    int rel = (int)(K - push);
    for (int d = 1; d <= g->reach; d++) {
        long rp = push - d;
        if (rp <= 0) break;
        int rslot = g->slot_of[rp % g->npast];
        if (g->plane_push[rslot] != rp) continue;      /* not resident */
        for (int dir = 0; dir < 2; dir++) {
            ngc_buf *cur = dir ? g->plane[rslot] : g->plane[slot];
            ngc_buf *ref = dir ? g->plane[slot] : g->plane[rslot];
            ngc_job *j = &g->job[g->njob];
            memset(j, 0, sizeof *j);
            j->kind = NGC_JOB_SEARCHQ;
            j->src = (ngc_plane){ cur, 0, (uint32_t)g->lw, (uint32_t)g->lh, (uint32_t)g->lw };
            j->ref = (ngc_plane){ ref, 0, (uint32_t)g->lw, (uint32_t)g->lh, (uint32_t)g->lw };
            j->out = r->out;
            j->out_offset = (uint32_t)((size_t)g->njob * legsz);
            j->u.searchq.range = GPQ_RANGE;
            r->legof[(rel * g->reach + (d - 1)) * 2 + dir] = (int16_t)g->njob;
            g->njob++;
        }
    }

    if (push != K || !g->njob)
        return;                     /* mid-batch, or nothing to submit */
    ngc_fence *f = ngc_submit(g->stream, g->job, (uint32_t)g->njob);
    if (!f) { gpq_die(g, "submit"); return; }
    pthread_mutex_lock(&g->mx);
    r->fence = f;
    r->key = K;                     /* publish */
    pthread_mutex_unlock(&g->mx);
    g->cur_key = 0;
}

const y264_gpq_blk *y264_gpq_field(y264_gpq *g, long src_push, long ref_push,
                                   long max_push)
{
    if (!g || g->dead || src_push <= 0 || ref_push <= 0)
        return NULL;
    /* Round owner = the LATER push. dir 0 = the round's own frame as source
 * (src newer than ref); dir 1 = an earlier frame against it. */
    long owner, d;
    int dir;
    if (src_push > ref_push) { owner = src_push; d = src_push - ref_push; dir = 0; }
    else                     { owner = ref_push; d = ref_push - src_push; dir = 1; }
    if (d < 1 || d > g->reach)
        return NULL;
    /* THE ARITHMETIC COVERAGE BOUND (the determinism contract): the leg's
 * round submits at boundary K; the consumer's chain wait guarantees
 * pushes up to max_push happened, so K <= max_push means submitted and
 * K > max_push means not-yet BY ARITHMETIC, never by racing the chain. */
    long K = GPQ_KEY(g, owner);
    if (max_push > 0 && K > max_push)
        return NULL;
    struct gpq_round *r = &g->round[GPQ_RIDX(g, K)];
    int rel = (int)(K - owner);
    int leg = r->legof[(rel * g->reach + (int)(d - 1)) * 2 + dir];
    pthread_mutex_lock(&g->mx);
    if (r->key != K || leg < 0 || r->failed) {
        pthread_mutex_unlock(&g->mx);
        return NULL;
    }
    /* Fence-wait OUTSIDE the lock: Phase A runs field from every pool
 * worker, and a wait held under the handle mutex serializes the whole
 * parallel_for behind one round's completion. First toucher claims the
 * wait; the rest sleep on the condvar. */
    while (r->fence || r->waiting) {
        if (!r->waiting) {
            ngc_fence *f = r->fence;
            r->waiting = 1;
            pthread_mutex_unlock(&g->mx);
            int rc = ngc_fence_wait(f);
            pthread_mutex_lock(&g->mx);
            ngc_fence_release(f);
            r->fence = NULL;
            r->waiting = 0;
            if (rc) { r->failed = 1; gpq_die(g, "fence wait"); }
            pthread_cond_broadcast(&g->cv);
        } else
            pthread_cond_wait(&g->cv, &g->mx);
    }
    int failed = r->failed;
    pthread_mutex_unlock(&g->mx);
    if (failed)
        return NULL;
    return (const y264_gpq_blk *)((const char *)ngc_buf_ptr(r->out)
           + (size_t)leg * g->nblk * sizeof(y264_gpq_blk));
}

/* Every env gate in this TU, resolved on the main thread. The Phase-A path
 * reads them from pool workers, so first touch would otherwise race between
 * them -- benign same-value init, but a nonzero TSan floor hides the next
 * real report. */
void y264_gpu_warm_statics(void)
{
    (void)y264_gpq_mode();
    (void)gpq_warmup_env();
}

#else   /* library not linked, or a bit depth it does not serve */

int y264_gpq_mode(void) { return 0; }
y264_gpq *y264_gpq_open(int lw, int lh, int nslots, int reach)
{ (void)lw; (void)lh; (void)nslots; (void)reach; return NULL; }
void y264_gpq_close(y264_gpq *g) { (void)g; }
void y264_gpq_push(y264_gpq *g, long push, int slot, const pixel *lowres)
{ (void)g; (void)push; (void)slot; (void)lowres; }
const y264_gpq_blk *y264_gpq_field(y264_gpq *g, long src_push, long ref_push,
                                   long max_push)
{ (void)g; (void)src_push; (void)ref_push; (void)max_push; return NULL; }
void y264_gpu_warm_statics(void) {}

#endif
