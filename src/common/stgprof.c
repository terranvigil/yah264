/*
 * stgprof.c - stage-profiler storage + exit dump (see stgprof.h). Empty TU
 * unless built with -DY264_STAGE_PROF.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "stgprof.h"

#ifdef Y264_STAGE_PROF
#include <stdio.h>
#include <time.h>

static const char *const g_stg_name[STG_N] = {
    "skip_cand", "skip_probe", "me+satd", "inter_rd", "intra",
    "snapshot", "decide", "deblock", "slice_prep", "emit",
    "b_skip", "b_me", "b_rd", "other"
};

static double g_stg_ms[STG_N];
static unsigned long long g_stg_calls[STG_N];

/* nesting stack: time is billed to the innermost open region. Depth is tiny
 * (per-MB regions nest at most a few deep); 64 is generous. */
#define STG_STACK 64
static struct { int bucket; double t0; } g_stack[STG_STACK];
static int g_depth;
static double g_wall0;
static int g_inited;

static double stg_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

void y264_stg_beg(int bucket)
{
    double now = stg_ms();
    if (!g_inited) { g_wall0 = now; g_inited = 1; }
    if (g_depth > 0)                                   /* pause the parent */
        g_stg_ms[g_stack[g_depth - 1].bucket] += now - g_stack[g_depth - 1].t0;
    if (g_depth < STG_STACK) {
        g_stack[g_depth].bucket = bucket;
        g_stack[g_depth].t0 = now;
    }
    g_depth++;
    g_stg_calls[bucket]++;
}

void y264_stg_end(void)
{
    double now = stg_ms();
    if (g_depth <= 0) return;
    g_depth--;
    if (g_depth < STG_STACK)
        g_stg_ms[g_stack[g_depth].bucket] += now - g_stack[g_depth].t0;
    if (g_depth > 0)                                   /* resume the parent */
        g_stack[g_depth - 1].t0 = now;
}

void y264_stg_count(int bucket) { g_stg_calls[bucket]++; }

__attribute__((destructor)) static void y264_stg_dump(void)
{
    if (!g_inited) return;
    double wall = stg_ms() - g_wall0, tot = 0;
    for (int i = 0; i < STG_N; i++) tot += g_stg_ms[i];
    fprintf(stderr, "\n=== Y264_STAGE_PROF (attributed %.1f ms) ===\n", tot);
    for (int i = 0; i < STG_N; i++)
        if (g_stg_ms[i] > 0 || g_stg_calls[i])
            fprintf(stderr, "  %-12s %10.1f ms  %5.1f%%  (%llu calls)\n",
                    g_stg_name[i], g_stg_ms[i],
                    tot > 0 ? 100.0 * g_stg_ms[i] / tot : 0.0, g_stg_calls[i]);
    fprintf(stderr, "  %-12s %10.1f ms\n", "wall_since_1st", wall);
}
#endif
