/*
 * skiporacle.c - see skiporacle.h. Measurement scaffolding only.
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "skiporacle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Flat bitmap over (poc, is_b, mb index). POC is dense and small for the clips
 * this is used on; a miss just means "not skip", which degrades to today's
 * behaviour rather than to a wrong answer. */
#define SKOR_POC   1024
#define SKOR_MB    16384        /* 1080p is 120x68 = 8160 */

static unsigned char *g_bits;
static int g_mode;                       /* 0 until skor_init runs; see skor_once */
static int g_side_b = 1, g_side_p = 1;   /* N264_SKIP_ORACLE_SIDE */
static int g_at_post;                    /* N264_SKIP_ORACLE_AT=post */
static const char *g_path;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static size_t idx(int poc, int is_b, int mbx, int mby, int wmb)
{
    long mb = (long)mby * wmb + mbx;
    if (poc < 0 || poc >= SKOR_POC || mb < 0 || mb >= SKOR_MB) return (size_t)-1;
    return ((size_t)poc * 2 + (size_t)!!is_b) * SKOR_MB + (size_t)mb;
}

static void skor_init(void)
{
    g_path = getenv("N264_SKIP_ORACLE");
    if (!g_path || !*g_path) { g_mode = 0; return; }
    const char *s = getenv("N264_SKIP_ORACLE_SIDE");
    if (s && *s) { g_side_b = (*s == 'b' || *s == 'B'); g_side_p = (*s == 'p' || *s == 'P');
                   if (!g_side_b && !g_side_p) { g_side_b = g_side_p = 1; } }
    /* post = exit after the searches, order UNCHANGED. Byte-identical, so the
 * oracle's own correctness check still holds -- and a conservative
 * reachable bound, since it does not claim list0's refs 1..n-1.
 * postr = the same exit with x264's list1-first reorder, which does claim
 * them. Changes the halfpel-threshold order and hence the output, so
 * it reports CHANGED by construction; its value is the DELTA over
 * post, which is what the reorder is worth. */
    const char *a = getenv("N264_SKIP_ORACLE_AT");
    if (a && a[0] == 'p' && a[1] == 'o') g_at_post = a[4] == 'r' ? 2 : 1;
    /* The RECORDING pass must see every verdict whatever the replay filters say,
 * or the bitmap it writes is not the thing being replayed. Filters apply to
 * the ask, never to the put. */
    g_bits = calloc((size_t)SKOR_POC * 2 * SKOR_MB, 1);
    if (!g_bits) { g_mode = 0; return; }
    FILE *f = fopen(g_path, "rb");
    if (f) {                                   /* pass 2: replay */
        if (fread(g_bits, 1, (size_t)SKOR_POC * 2 * SKOR_MB, f) == 0) { /* empty */ }
        fclose(f);
        g_mode = 2;
    } else {
        g_mode = 1;                            /* pass 1: record */
    }
}

/* pthread_once, not a double-checked int: this repo already paid for ~23
 * lazy-static init races once (see the TSan sweep), and a measurement hook is
 * not a good reason to add the 24th. After the once, g_mode is read-only. */
int n264_skor_mode(void)
{
    pthread_once(&g_once, skor_init);
    return g_mode;
}

int n264_skor_ask(int poc, int is_b, int mbx, int mby, int wmb)
{
    if (n264_skor_mode() != 2) return 0;
    if (!(is_b ? g_side_b : g_side_p)) return 0;
    size_t i = idx(poc, is_b, mbx, mby, wmb);
    return i == (size_t)-1 ? 0 : g_bits[i];
}

int n264_skor_side_b(void) { pthread_once(&g_once, skor_init); return g_side_b; }
int n264_skor_side_p(void) { pthread_once(&g_once, skor_init); return g_side_p; }
int n264_skor_at_post(void) { pthread_once(&g_once, skor_init); return g_at_post; }

void n264_skor_put(int poc, int is_b, int mbx, int mby, int wmb, int skip)
{
    if (n264_skor_mode() != 1 || !skip) return;
    size_t i = idx(poc, is_b, mbx, mby, wmb);
    if (i != (size_t)-1) g_bits[i] = 1;
}

__attribute__((destructor)) static void skor_flush(void)
{
    if (g_mode != 1 || !g_bits || !g_path) return;
    FILE *f = fopen(g_path, "wb");
    if (!f) return;
    fwrite(g_bits, 1, (size_t)SKOR_POC * 2 * SKOR_MB, f);
    fclose(f);
}
