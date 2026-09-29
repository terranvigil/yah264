/*
 * iprof.c - see iprof.h.
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "iprof.h"

#ifdef Y264_IPROF
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* the kernel's per-thread counters: [0] instructions, [1] cycles, both
 * kernel and user, since the thread started */
extern int thread_selfcounts(int type, void *buf, size_t nbytes);

static const char *const ip_name[IP_N] = {
    "ME (integer)", "sub-pel", "RD trial", "RD trial bits", "commit re-encode",
    "intra", "entropy emit", "deblock", "lookahead", "mb-tree", "MB decision (rest)",
    "hpel + borders",
};
static _Atomic uint64_t ip_acc[IP_N], ip_calls[IP_N];
static uint64_t ip_cost;            /* instructions one counter read costs */
static pthread_once_t ip_once = PTHREAD_ONCE_INIT;

static inline uint64_t ip_read(void)
{
    uint64_t c[2];
    thread_selfcounts(1, c, sizeof c);
    return c[0];
}

static void ip_dump(void)
{
    uint64_t t = 0;
    for (int b = 0; b < IP_N; b++) t += ip_acc[b];
    fprintf(stderr, "IPROF: retired instructions by stage (read cost %llu subtracted)\n",
            (unsigned long long)ip_cost);
    for (int b = 0; b < IP_N; b++)
        fprintf(stderr, "IPROF %-18s %10.3f G  %11llu scopes\n", ip_name[b],
                ip_acc[b] / 1e9, (unsigned long long)ip_calls[b]);
    fprintf(stderr, "IPROF %-18s %10.3f G\n", "staged total", t / 1e9);
}

static void ip_init(void)
{
    uint64_t a = ip_read();
    for (int i = 0; i < 1000; i++) (void)ip_read();
    uint64_t b = ip_read();
    ip_cost = (b - a) / 1001;
    atexit(ip_dump);
}

static _Thread_local struct { int b; uint64_t t; } ip_stk[32];
static _Thread_local int ip_depth;

void y264_ip_beg(int bucket)
{
    pthread_once(&ip_once, ip_init);
    uint64_t now = ip_read();
    if (ip_depth > 0) {
        int p = ip_stk[ip_depth - 1].b;
        uint64_t d = now - ip_stk[ip_depth - 1].t;
        atomic_fetch_add_explicit(&ip_acc[p], d > ip_cost ? d - ip_cost : 0,
                                  memory_order_relaxed);
    }
    if (ip_depth < 32) {
        ip_stk[ip_depth].b = bucket;
        ip_stk[ip_depth].t = ip_read();
    }
    ip_depth++;
    atomic_fetch_add_explicit(&ip_calls[bucket], 1, memory_order_relaxed);
}

void y264_ip_end(void)
{
    uint64_t now = ip_read();
    ip_depth--;
    if (ip_depth < 32) {
        int b = ip_stk[ip_depth].b;
        uint64_t d = now - ip_stk[ip_depth].t;
        atomic_fetch_add_explicit(&ip_acc[b], d > ip_cost ? d - ip_cost : 0,
                                  memory_order_relaxed);
    }
    if (ip_depth > 0)
        ip_stk[ip_depth - 1].t = ip_read();
}

void y264_ip_scope_end(int *unused)
{
    (void)unused;
    y264_ip_end();
}
#endif
