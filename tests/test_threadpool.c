/*
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * test_threadpool -- verifies the W1 wavefront substrate: (1) the neighbour
 * ordering guarantee (a cell only runs after its left + top + top-right cells),
 * (2) exactly-once cell invocation, and (3) determinism -- an identical result
 * grid at every thread count. If the ordering guarantee holds, a recurrence that
 * reads those neighbours produces a thread-count-independent grid, which is the
 * property the byte-identical threaded encode relies on.
 */
#include "common/threadpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#define NR 37
#define NC 53

struct grid {
    long v[NR][NC];
    _Atomic int done[NR][NC];
    _Atomic long calls;
    _Atomic int order_fail;
};

static void cell(void *ctx, int thread_idx, int r, int c)
{
    struct grid *g = ctx;
    (void)thread_idx;
    atomic_fetch_add(&g->calls, 1);

    /* every neighbour the H.264 raster scan would have finished must be done */
    int ok = 1;
    if (r > 0 && !atomic_load(&g->done[r - 1][c])) ok = 0;              /* top */
    if (r > 0 && c + 1 < NC && !atomic_load(&g->done[r - 1][c + 1])) ok = 0; /* top-right */
    if (r > 0 && c > 0 && !atomic_load(&g->done[r - 1][c - 1])) ok = 0; /* top-left */
    if (c > 0 && !atomic_load(&g->done[r][c - 1])) ok = 0;             /* left */
    if (!ok)
        atomic_store(&g->order_fail, 1);

    /* a recurrence over those neighbours -- deterministic iff ordering holds */
    long up   = (r > 0) ? g->v[r - 1][c] : 1;
    long left = (c > 0) ? g->v[r][c - 1] : 1;
    long tr   = (r > 0 && c + 1 < NC) ? g->v[r - 1][c + 1] : 0;
    g->v[r][c] = (up * 1000003L + left * 31L + tr * 7L + r * 13L + c) & 0x3fffffff;

    atomic_store(&g->done[r][c], 1);
}

static int run(int nthreads, long v_out[NR][NC])
{
    ntp_pool_t *p = ntp_pool_create(nthreads);
    if (!p) { fprintf(stderr, "pool create failed (nthreads=%d)\n", nthreads); return 1; }
    if (ntp_pool_nthreads(p) != nthreads) { fprintf(stderr, "nthreads mismatch\n"); return 1; }

    struct grid *g = calloc(1, sizeof *g);
    ntp_wavefront(p, NR, NC, NULL, cell, g);

    int fail = 0;
    if (atomic_load(&g->order_fail)) { fprintf(stderr, "ORDER violation (nthreads=%d)\n", nthreads); fail = 1; }
    if (atomic_load(&g->calls) != (long)NR * NC) {
        fprintf(stderr, "call count %ld != %d (nthreads=%d)\n",
                (long)atomic_load(&g->calls), NR * NC, nthreads); fail = 1;
    }
    for (int r = 0; r < NR; r++)
        for (int c = 0; c < NC; c++)
            if (!atomic_load(&g->done[r][c])) { fprintf(stderr, "cell (%d,%d) never ran\n", r, c); fail = 1; }

    memcpy(v_out, g->v, sizeof g->v);
    free(g);
    ntp_pool_destroy(p);
    return fail;
}

/* W2 background worker: submitted tasks run in order, exactly once, and their
 * writes are visible after ntp_bg_sync (the pipeline relies on this handoff). */
struct bgjob { long in, out; };
static void bg_task(void *arg)
{
    struct bgjob *j = arg;
    j->out = j->in * 2654435761u + 12345;   /* some work touching the arg */
}
static int test_bg(void)
{
    ntp_bg_t *b = ntp_bg_create();
    if (!b) { fprintf(stderr, "bg create failed\n"); return 1; }
    int fail = 0;
    for (int k = 0; k < 5000; k++) {
        struct bgjob j = { k, -1 };
        ntp_bg_submit(b, bg_task, &j);
        ntp_bg_sync(b);                      /* after sync, j.out must be written */
        long want = (long)((unsigned long)k * 2654435761u + 12345);
        if (j.out != want) { fprintf(stderr, "bg iter %d: %ld != %ld\n", k, j.out, want); fail = 1; break; }
    }
    ntp_bg_destroy(b);
    /* a create/destroy with no task submitted must not hang */
    ntp_bg_t *b2 = ntp_bg_create(); ntp_bg_destroy(b2);
    return fail;
}

int main(void)
{
    static long ref[NR][NC];
    int fail = run(1, ref);                        /* 1-thread reference */

    int counts[] = { 2, 3, 4, 8, 16 };
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        static long cur[NR][NC];
        fail |= run(counts[i], cur);
        if (memcmp(ref, cur, sizeof ref) != 0) {
            fprintf(stderr, "NONDETERMINISTIC: grid at %d threads != 1-thread reference\n", counts[i]);
            fail = 1;
        }
    }

    /* run the same pool through many wavefronts to exercise job reuse */
    ntp_pool_t *p = ntp_pool_create(8);
    for (int k = 0; k < 200; k++) {
        struct grid *g = calloc(1, sizeof *g);
        ntp_wavefront(p, NR, NC, NULL, cell, g);
        if (atomic_load(&g->order_fail) || atomic_load(&g->calls) != (long)NR * NC) {
            fprintf(stderr, "reuse iter %d failed\n", k); fail = 1;
        }
        if (memcmp(g->v, ref, sizeof ref) != 0) { fprintf(stderr, "reuse iter %d nondeterministic\n", k); fail = 1; }
        free(g);
    }
    ntp_pool_destroy(p);

    fail |= test_bg();

    printf(fail ? "test_threadpool: FAIL\n" : "test_threadpool: OK\n");
    return fail;
}
