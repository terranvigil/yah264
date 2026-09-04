/*
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * threadpool.h -- a persistent worker pool plus a deterministic MB-row wavefront
 * (W1). The wavefront is the substrate for in-frame parallelism: rows run
 * concurrently but each cell only starts once its H.264 neighbours (top-right and
 * left) are complete, so the per-cell work sees the SAME neighbour state at any
 * thread count. That determinism is what lets the threaded encode stay
 * byte-identical to the serial one (the W1 gate).
 */
#ifndef Y264_THREADPOOL_H
#define Y264_THREADPOOL_H

#include <stddef.h>

typedef struct ntp_pool ntp_pool_t;

/* Create a pool with `nthreads` persistent workers (nthreads >= 1). Returns NULL
 * on allocation/thread-spawn failure. A 1-thread pool is valid (it runs the
 * wavefront on a single worker, useful as the byte-identity reference). */
ntp_pool_t *ntp_pool_create(int nthreads);
void        ntp_pool_destroy(ntp_pool_t *p);
/* Grow-only scratch slot (i in [0,8)) of the CALLING thread's lane; persists
 * across frames so the wavefront run functions can reuse it instead of
 * malloc/free per frame. Each submitting thread gets its own lane (up to 4), so
 * concurrent frames on the shared pool never carve the same buffer. NULL when
 * the lanes are exhausted -- fall back to the serial path. */
void       *ntp_pool_slot(ntp_pool_t *p, int i, size_t bytes);
int         ntp_pool_nthreads(const ntp_pool_t *p);
/* Milliseconds the pool has spent with no live job, sampled lock-free so a
 * driver stage can be charged for the idleness it causes. 0 unless the pool
 * profiler (Y264_NTP_PROF) is on. */
double      ntp_pool_empty_ms(const ntp_pool_t *p);

/*
 * Deterministic wavefront over an nrows x ncols grid. `cell_fn(ctx, row, col)` is
 * invoked exactly once per cell. When it runs, the runtime guarantees:
 * - every cell (row-1, col') with col' <= col+1 has already returned, and
 * - every cell (row, col') with col' < col has already returned.
 * That is the H.264 raster dependency (left + top + top-right neighbours), so a
 * cell observes identical neighbour state regardless of nthreads -> the result is
 * byte-identical to a serial raster scan. The call blocks until every cell is done.
 *
 * `thread_init(ctx, thread_idx)`, if non-NULL, runs once on each participating
 * worker before it processes any cell of THIS wavefront -- use it to install
 * thread-local per-worker state (e.g. the ME half-pel context). thread_idx is in
 * [0, nthreads).
 *
 * `cell_fn` also receives the worker's thread_idx so it can select per-worker
 * scratch (e.g. a private copy of the frame struct) without thread-locals.
 *
 * Deadlock-free: workers claim rows in increasing order, and a row only ever waits
 * on the immediately-preceding (lower-numbered, already-claimed) row.
 */
void ntp_wavefront(ntp_pool_t *p, int nrows, int ncols,
                   void (*thread_init)(void *ctx, int thread_idx),
                   void (*cell_fn)(void *ctx, int thread_idx, int row, int col),
                   void *ctx);

/*
 * v2 (multi-frame): the pool holds a small table of concurrent jobs -- every
 * ntp_wavefront / ntp_parallel_for call registers one and blocks until it
 * completes, and calls may overlap from different threads. ONE worker set
 * serves them all: an idle worker claims the next READY row from the OLDEST
 * job that has one (drain order bounds latency), so a stalled frame never
 * parks a worker another frame could use. With a single registered job this
 * reduces exactly to the classic wavefront above.
 *
 * When a claimed row stalls mid-row and other work is runnable, the worker
 * PARKS the row (a resumable continuation) and serves the other frame; only
 * the same worker later resumes it, so row-carried per-worker state survives.
 * Re-entering a job after running another one calls `thread_attach` (re-
 * install cheap thread-locals, e.g. the ME hpel registry) -- thread_init runs
 * only before FRESH rows, where all row state re-seeds. A job that has a
 * thread_init but no thread_attach is never parked (its rows stall in place,
 * v1 behaviour).
 *
 * ntp_wavefront_gated additionally takes an external per-row claim gate: row
 * `r` is only claimed once row_ready(gate_ctx, r) returns nonzero. The
 * predicate is called under the pool's internal mutex, so it must be cheap,
 * non-blocking and MONOTONIC (once true for a row it must stay true -- e.g.
 * an atomic watermark read). Whoever advances the gate's source must call
 * ntp_pool_kick so parked workers re-evaluate. Used by the staircase: a B
 * frame's rows gate on the in-flight anchor's published consumable rows.
 */
void ntp_wavefront_gated(ntp_pool_t *p, int nrows, int ncols,
                         void (*thread_init)(void *ctx, int thread_idx),
                         void (*thread_attach)(void *ctx, int thread_idx),
                         void (*cell_fn)(void *ctx, int thread_idx, int row, int col),
                         void *ctx,
                         int (*row_ready)(void *gate_ctx, int row), void *gate_ctx);

/* Wake parked workers to re-evaluate gated claims (call after advancing any
 * row_ready gate's source). Cheap; safe from any thread. */
void ntp_pool_kick(ntp_pool_t *p);

/*
 * Batch: register n INDEPENDENT wavefronts at once and block until all
 * complete. Each spec is one ntp_wavefront (ungated); the jobs run
 * concurrently on the shared worker set, so a batch of small grids reaches
 * the parallelism a single small wavefront cannot (its ~ncols/2 dependency
 * ceiling). The caller guarantees the wavefronts are mutually independent
 * (disjoint outputs, shared inputs read-only) -- each one's own dependency
 * order is still enforced, so per-job results are schedule-invariant and the
 * batch cannot change bits. */
typedef struct ntp_wf_spec {
    int nrows, ncols;
    void (*thread_init)(void *ctx, int thread_idx);
    void (*cell_fn)(void *ctx, int thread_idx, int row, int col);
    void *ctx;
} ntp_wf_spec_t;
void ntp_wavefront_batch(ntp_pool_t *p, int n, const ntp_wf_spec_t *spec);

/* Y264_NTP_PROF=1 wait accounting: label the NEXT job this thread registers
 * (one-shot; a string literal). With the profiler off this is a single
 * thread-local store. Per-worker busy/idle time split by wait site, per-tag
 * busy/join time and claim/wake counts are printed at pool destroy. */
void ntp_prof_tag(const char *tag);

/* Mark the NEXT job this thread registers as LATENCY-CRITICAL (one-shot):
 * non-affine workers pick the oldest ready high-priority job before any
 * normal one. For short jobs a blocked submitter is waiting on (the
 * lookahead's small grids) -- without this the oldest-first drain order lets
 * a long analyze wavefront starve them (the t18 budget's 0.7x lookahead_lr).
 * Scheduling-only: WHO runs a cell never reaches the bitstream. */
void ntp_prio_hint(void);

/*
 * Deterministic dynamic parallel-for over [0, n). `for_fn(ctx, thread_idx, i)` is
 * invoked exactly once per index i, on some worker, in NO guaranteed order and with
 * NO cross-index ordering. Use only when the per-index work is independent (writes
 * disjoint output). thread_idx is in [0, nthreads) so the body can pick per-worker
 * scratch. Blocks until every index is done. Determinism of the overall result is
 * the caller's responsibility: the bodies must not depend on each other's effects.
 */
void ntp_parallel_for(ntp_pool_t *p, int n,
                      void (*for_fn)(void *ctx, int thread_idx, int i),
                      void *ctx);

/*
 * Single background worker (W2): a dedicated thread that runs one submitted task
 * at a time, so the frame loop can hand off frame N's serial entropy emit and get
 * on with frame N+1's wavefront. At most one task is outstanding: call ntp_bg_sync
 * before the next ntp_bg_submit (and before touching the task's outputs).
 */
typedef struct ntp_bg ntp_bg_t;
ntp_bg_t *ntp_bg_create(void);
ntp_bg_t *ntp_bg_create_named(const char *name);   /* the thread's name in a profile (<= 15 chars) */
void      ntp_bg_destroy(ntp_bg_t *b);
void      ntp_bg_submit(ntp_bg_t *b, void (*fn)(void *arg), void *arg);
void      ntp_bg_sync(ntp_bg_t *b);   /* block until the outstanding task finishes */

#endif /* Y264_THREADPOOL_H */
