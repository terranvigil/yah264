/*
 * cabacbench.c - replay a recorded CABAC op trace through the real engine.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The bin coder is a serial state machine, so the only honest way to price a
 * rewrite of it is to feed it the bin sequence a real encode produces. Traces
 * come from a `-DY264_CABAC_TRACE` build (src/encoder/cabac.c), which records
 * one slice engine's real-coding ops at the API boundary: decision, bypass,
 * UEGk, terminate, and the two residual writers with their coefficient blocks.
 * Replaying those entry points in order reproduces the identical bin sequence,
 * context evolution and output bytes -- the FNV hash printed below is the
 * check, and it must not move when the engine is rewritten.
 *
 * usage: cabacbench <trace> [reps]
 */
#include "../../src/encoder/cabac.h"
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Monotonic nanoseconds. clock_gettime rather than mach_absolute_time so this
 * builds everywhere; checkasm uses the same call. */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

enum { TR_DEC = 0, TR_BYP, TR_UEG, TR_TERM, TR_RES, TR_RES8, TR_ENG, TR_CTX };

/* Pre-decoded op list: parsing must not be inside the timed region. */
typedef struct {
    uint8_t  op, cat;
    uint8_t  nza, nzb;
    int16_t  n;
    int32_t  a, b;
    uint32_t coff;              /* index into the coefficient pool */
} op_t;

static op_t *ops;
static size_t nops;
static dctcoef *pool;
static size_t npool;

static uint64_t counts[8];
static uint64_t n_bins_dec, n_bins_byp;

static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    int32_t *raw = malloc((size_t)sz);
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); exit(1); }
    fclose(f);
    size_t nw = (size_t)sz / 4;

    ops = malloc(nw * sizeof(op_t));
    pool = malloc(nw * sizeof(dctcoef));
    size_t i = 0;
    while (i < nw) {
        op_t o = {0};
        o.op = (uint8_t)raw[i];
        counts[o.op]++;
        switch (raw[i]) {
        case TR_DEC:  o.a = raw[i+1]; o.b = raw[i+2]; i += 3; n_bins_dec++; break;
        case TR_BYP:  o.a = raw[i+1]; i += 2; n_bins_byp++; break;
        case TR_UEG:  o.a = raw[i+1]; o.b = raw[i+2]; i += 3; break;
        case TR_TERM: o.a = raw[i+1]; i += 2; break;
        case TR_ENG:  i += 1; break;
        case TR_CTX:  o.a = raw[i+1]; o.b = raw[i+2]; o.n = (int16_t)raw[i+3]; i += 4; break;
        case TR_RES:
        case TR_RES8:
            o.cat = (uint8_t)raw[i+1];
            o.nza = (uint8_t)raw[i+2];
            o.nzb = (uint8_t)raw[i+3];
            o.n   = (int16_t)raw[i+4];
            o.coff = (uint32_t)npool;
            for (int k = 0; k < o.n; k++) pool[npool++] = (dctcoef)raw[i+5+k];
            i += 5 + (size_t)o.n;
            break;
        default:
            fprintf(stderr, "bad trace op %d at word %zu\n", raw[i], i);
            exit(1);
        }
        ops[nops++] = o;
    }
    free(raw);
}

/* 64 MB is more than any recorded slice needs; +16 keeps the first putbyte's
 * p[-1] carry slot (which provably receives +0) inside the allocation. */
#define BUFSZ (64u << 20)
static uint8_t *buf;

static uint64_t replay(y264_cabac_t *c)
{
    uint8_t *base = buf + 16;
    uint8_t *end = base;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < nops; i++) {
        const op_t *o = &ops[i];
        switch (o->op) {
        case TR_DEC:  y264_cabac_encode_decision(c, o->a, o->b); break;
        case TR_BYP:  y264_cabac_encode_bypass(c, o->a); break;
        case TR_UEG:  y264_cabac_encode_ueg_bypass(c, o->a, o->b); break;
        case TR_TERM: y264_cabac_encode_terminate(c, o->a); break;
        case TR_RES:  y264_cabac_residual(c, o->cat, pool + o->coff, o->nza, o->nzb); break;
        case TR_RES8: y264_cabac_residual_8x8(c, pool + o->coff); break;
        case TR_ENG:
            /* fold the finished slice into the hash, then restart the engine */
            for (uint8_t *p = base; p < end; p++) { h ^= *p; h *= 1099511628211ull; }
            y264_cabac_init_engine(c, base);
            end = base;
            break;
        case TR_CTX:  y264_cabac_init_contexts(c, o->a, o->b, o->n); break;
        }
        if (c->p > end) end = c->p;
    }
    for (uint8_t *p = base; p < end; p++) { h ^= *p; h *= 1099511628211ull; }
    return h;
}

/* Second arm: the same op stream in RD-estimate mode. The profile says the
 * "cabac" area is mostly this -- est bins outnumber real bins 3.5:1 at the
 * cabac-heaviest cell -- and it runs the same residual writers down their
 * est branch, so the recorded coefficient blocks price it directly. Slice
 * boundaries re-init the contexts only; there is no arithmetic state. */
static long replay_est(y264_cabac_t *c)
{
    long acc = 0;
    c->est_mode = 1;
    for (size_t i = 0; i < nops; i++) {
        const op_t *o = &ops[i];
        switch (o->op) {
        case TR_DEC:  y264_cabac_encode_decision(c, o->a, o->b); break;
        case TR_BYP:  y264_cabac_encode_bypass(c, o->a); break;
        case TR_UEG:  y264_cabac_encode_ueg_bypass(c, o->a, o->b); break;
        case TR_RES:  y264_cabac_residual(c, o->cat, pool + o->coff, o->nza, o->nzb); break;
        case TR_RES8: y264_cabac_residual_8x8(c, pool + o->coff); break;
        case TR_ENG:  acc += c->est_bits; c->est_bits = 0; break;
        case TR_CTX:  y264_cabac_init_contexts(c, o->a, o->b, o->n); c->est_mode = 1; break;
        default: break;
        }
    }
    acc += c->est_bits;
    c->est_mode = 0;
    return acc;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <trace> [reps]\n", argv[0]); return 1; }
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    load(argv[1]);
    buf = calloc(BUFSZ, 1);
    y264_cabac_warm();
    y264_cabac_t *c = calloc(1, sizeof(*c));

    uint64_t bins = n_bins_dec + n_bins_byp;
    printf("trace %s: %zu ops (dec %llu, byp %llu, ueg %llu, term %llu, res %llu, res8 %llu, "
           "slices %llu), %zu coeffs\n",
           argv[1], nops, (unsigned long long)counts[TR_DEC], (unsigned long long)counts[TR_BYP],
           (unsigned long long)counts[TR_UEG], (unsigned long long)counts[TR_TERM],
           (unsigned long long)counts[TR_RES], (unsigned long long)counts[TR_RES8],
           (unsigned long long)counts[TR_ENG], npool);

    double best = 1e30;
    uint64_t h = 0, h0 = 0;
    for (int r = 0; r < reps; r++) {
        uint64_t t0 = now_ns();
        h = replay(c);
        uint64_t t1 = now_ns();
        double ns = (double)(t1 - t0);
        if (r == 0) h0 = h;
        else if (h != h0) { fprintf(stderr, "REPLAY HASH UNSTABLE\n"); return 1; }
        if (ns < best) best = ns;
        printf("  rep %d: %.3f ms\n", r, ns / 1e6);
    }
    printf("cabacbench: real  best %.3f ms, %.3f ns/op, hash %016llx\n",
           best / 1e6, best / (double)nops, (unsigned long long)h);

    double beste = 1e30;
    long e = 0, e0 = 0;
    for (int r = 0; r < reps; r++) {
        y264_cabac_init_engine(c, buf + 16);
        uint64_t t0 = now_ns();
        e = replay_est(c);
        uint64_t t1 = now_ns();
        double ns = (double)(t1 - t0);
        if (r == 0) e0 = e;
        else if (e != e0) { fprintf(stderr, "EST REPLAY UNSTABLE\n"); return 1; }
        if (ns < beste) beste = ns;
    }
    printf("cabacbench: est   best %.3f ms, %.3f ns/op, bits %ld\n",
           beste / 1e6, beste / (double)nops, e);
    (void)bins;
    return 0;
}
