/*
 * test_cabac.c - round-trip the CABAC arithmetic engine: encode a bin sequence
 * with adapting contexts + bypass + terminate, decode it back with a matching
 * decode engine, and check every bin. This validates the encoder engine and the
 * state transitions without needing the full slice binarization.
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "../src/encoder/cabac.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- reference decode engine (9.3.3.2), independent of the encoder --- */
static const uint8_t d_rangeTabLPS[64][4] = {
    {128,176,208,240},{128,167,197,227},{128,158,187,216},{123,150,178,205},
    {116,142,169,195},{111,135,160,185},{105,128,152,175},{100,122,144,166},
    { 95,116,137,158},{ 90,110,130,150},{ 85,104,123,142},{ 81, 99,117,135},
    { 77, 94,111,128},{ 73, 89,105,122},{ 69, 85,100,116},{ 66, 80, 95,110},
    { 62, 76, 90,104},{ 59, 72, 86, 99},{ 56, 69, 81, 94},{ 53, 65, 77, 89},
    { 51, 62, 73, 85},{ 48, 59, 69, 80},{ 46, 56, 66, 76},{ 43, 53, 63, 72},
    { 41, 50, 59, 69},{ 39, 48, 56, 65},{ 37, 45, 54, 62},{ 35, 43, 51, 59},
    { 33, 41, 48, 56},{ 32, 39, 46, 53},{ 30, 37, 43, 50},{ 29, 35, 41, 48},
    { 27, 33, 39, 45},{ 26, 31, 37, 43},{ 24, 30, 35, 41},{ 23, 28, 33, 39},
    { 22, 27, 32, 37},{ 21, 26, 30, 35},{ 20, 24, 29, 33},{ 19, 23, 27, 31},
    { 18, 22, 26, 30},{ 17, 21, 25, 28},{ 16, 20, 23, 27},{ 15, 19, 22, 25},
    { 14, 18, 21, 24},{ 14, 17, 20, 23},{ 13, 16, 19, 22},{ 12, 15, 18, 21},
    { 12, 14, 17, 20},{ 11, 14, 16, 19},{ 11, 13, 15, 18},{ 10, 12, 15, 17},
    { 10, 12, 14, 16},{  9, 11, 13, 15},{  9, 11, 12, 14},{  8, 10, 12, 14},
    {  8,  9, 11, 13},{  7,  9, 11, 12},{  7,  9, 10, 12},{  7,  8, 10, 11},
    {  6,  8,  9, 11},{  6,  7,  9, 10},{  6,  7,  8,  9},{  2,  2,  2,  2}
};
static const uint8_t d_transMPS[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
    25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63
};
static const uint8_t d_transLPS[64] = {
     0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,13,13,15,15,16,16,18,18,
    19,19,21,21,22,22,23,24,24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63
};

typedef struct { const uint8_t *buf; int bitpos; uint32_t range, offset; uint8_t ctx[Y264_CABAC_CTX]; } dec_t;

static int d_bit(dec_t *d) { int byte = d->buf[d->bitpos >> 3]; int b = (byte >> (7 - (d->bitpos & 7))) & 1; d->bitpos++; return b; }

static void d_init(dec_t *d, const uint8_t *buf) {
    d->buf = buf; d->bitpos = 0; d->range = 510; d->offset = 0;
    for (int i = 0; i < 9; i++) d->offset = (d->offset << 1) | d_bit(d);
}
static int d_decision(dec_t *d, int ctxIdx) {
    int state = d->ctx[ctxIdx] >> 1, mps = d->ctx[ctxIdx] & 1, bin;
    int q = (d->range >> 6) & 3;
    uint32_t lps = d_rangeTabLPS[state][q];
    d->range -= lps;
    if (d->offset >= d->range) {
        bin = 1 - mps; d->offset -= d->range; d->range = lps;
        if (state == 0) mps = 1 - mps;
        state = d_transLPS[state];
    } else { bin = mps; state = d_transMPS[state]; }
    d->ctx[ctxIdx] = (uint8_t)((state << 1) | mps);
    while (d->range < 256) { d->range <<= 1; d->offset = (d->offset << 1) | d_bit(d); }
    return bin;
}
static int d_bypass(dec_t *d) {
    d->offset = (d->offset << 1) | d_bit(d);
    if (d->offset >= d->range) { d->offset -= d->range; return 1; }
    return 0;
}
static int d_terminate(dec_t *d) {
    d->range -= 2;
    if (d->offset >= d->range) return 1;
    while (d->range < 256) { d->range <<= 1; d->offset = (d->offset << 1) | d_bit(d); }
    return 0;
}

/* Deterministic pseudo-random bin/ctx stream. */
static uint32_t rng = 0x2545F491;
static uint32_t nextr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* --- reference residual decoder, mirroring y264_cabac_residual --- */
static const uint16_t D_CBF[5]={85,89,93,97,101},D_SIG[5]={105,120,134,149,152},
    D_LAST[5]={166,181,195,210,213},D_LVLO[5]={227,237,247,257,266};
static const uint8_t D_CNT[5]={15,14,15,3,14},D_L1[8]={1,2,3,4,0,0,0,0},
    D_LG1[8]={5,5,5,5,6,7,8,9};
static const uint8_t D_TR[2][8]={{1,2,3,3,4,5,6,7},{4,4,4,4,5,6,7,7}};
static int d_ueg(dec_t *d,int k){int v=0;while(d_bypass(d)){v+=1<<k;k++;}while(k>0){k--;v+=d_bypass(d)<<k;}return v;}
static int d_residual(dec_t *d,int cat,dctcoef *out,int nza,int nzb){
    int cm1=D_CNT[cat],n=cm1+1;
    for(int i=0;i<n;i++)out[i]=0;
    int cbf=d_decision(d,D_CBF[cat]+2*(nzb?1:0)+(nza?1:0));
    if(!cbf)return 0;
    int sig=D_SIG[cat],last=D_LAST[cat],lvl=D_LVLO[cat];
    int pos[16],np=0,i=0;
    while(1){
        if(d_decision(d,sig+i)){ pos[np++]=i; if(d_decision(d,last+i))break; }
        if(++i==cm1){ pos[np++]=i; break; }
    }
    int node=0;
    for(int k=np-1;k>=0;k--){
        int a=1,c=D_L1[node]+lvl;
        if(d_decision(d,c)){
            c=D_LG1[node]+lvl; int t=0;
            while(t<13 && d_decision(d,c)) t++;
            a=2+t; if(t==13) a=15+d_ueg(d,0);
            node=D_TR[1][node];
        } else node=D_TR[0][node];
        int sign=d_bypass(d);
        out[pos[k]]=sign?-a:a;
    }
    return np;
}

int main(void) {
    enum { N = 20000 };
    static int ctxs[N], bins[N], kinds[N];   /* kind: 0 decision, 1 bypass, 2 terminate(0) */
    y264_cabac_t c;
    static uint8_t buf[1 << 20];
    y264_cabac_init_engine(&c, buf);
    y264_cabac_init_contexts(&c, 1, 0, 26);

    for (int i = 0; i < N; i++) {
        int k = nextr() % 10;
        if (k == 0) { kinds[i] = 2; bins[i] = 0; y264_cabac_encode_terminate(&c, 0); }
        else if (k <= 2) { kinds[i] = 1; bins[i] = nextr() & 1; y264_cabac_encode_bypass(&c, bins[i]); }
        else { kinds[i] = 0; ctxs[i] = nextr() % 400; bins[i] = nextr() & 1;
               y264_cabac_encode_decision(&c, ctxs[i], bins[i]); }
    }
    y264_cabac_encode_terminate(&c, 1);      /* flush */

    dec_t d;
    /* initialise the decoder's contexts the same way the encoder did */
    { y264_cabac_t tmp; y264_cabac_init_contexts(&tmp, 1, 0, 26); memcpy(d.ctx, tmp.ctx, sizeof(d.ctx)); }
    d_init(&d, buf);

    for (int i = 0; i < N; i++) {
        int got;
        if (kinds[i] == 2) got = d_terminate(&d);
        else if (kinds[i] == 1) got = d_bypass(&d);
        else got = d_decision(&d, ctxs[i]);
        if (got != bins[i]) {
            printf("FAIL at bin %d: kind %d ctx %d expected %d got %d\n",
                   i, kinds[i], kinds[i] == 0 ? ctxs[i] : -1, bins[i], got);
            return 1;
        }
    }
    printf("cabac round-trip: %d bins OK (%d bytes)\n", N, y264_cabac_bytes(&c));

    /* serial-vs-batched bypass differential: y264_cabac_encode_ueg_bypass now
 * emits the whole UEGk suffix as one batched run; it must produce byte
 * output identical to the old per-bin bypass loop, across k orders, value
 * magnitudes (multi-chunk runs), and interleaved context bins (so the run
 * hits every settled-bit alignment and pending-0xff state). */
    {
        enum { NU = 30000 };
        y264_cabac_t eS, eB;
        static uint8_t bufS[1 << 20], bufB[1 << 20];
        y264_cabac_init_engine(&eS, bufS); y264_cabac_init_contexts(&eS, 1, 0, 30);
        y264_cabac_init_engine(&eB, bufB); y264_cabac_init_contexts(&eB, 1, 0, 30);
        for (int i = 0; i < NU; i++) {
            int op = nextr() % 4;
            if (op == 0) {
                int ctx = nextr() % 400, b = nextr() & 1;
                y264_cabac_encode_decision(&eS, ctx, b);
                y264_cabac_encode_decision(&eB, ctx, b);
            } else {
                int k = nextr() % 6;
                int val = (int)(op == 1 ? nextr() % 8 :
                                op == 2 ? nextr() % 600 : nextr() % 200000);
                int kk = k, v = val;                  /* per-bin reference */
                while (v >= (1 << kk)) { y264_cabac_encode_bypass(&eS, 1); v -= 1 << kk; kk++; }
                y264_cabac_encode_bypass(&eS, 0);
                while (kk > 0) { kk--; y264_cabac_encode_bypass(&eS, (v >> kk) & 1); }
                y264_cabac_encode_ueg_bypass(&eB, k, val);
            }
        }
        y264_cabac_encode_terminate(&eS, 1);
        y264_cabac_encode_terminate(&eB, 1);
        int nS = y264_cabac_bytes(&eS), nB = y264_cabac_bytes(&eB);
        if (nS != nB || memcmp(bufS, bufB, (size_t)nS) != 0) {
            printf("FAIL ueg differential: serial %d bytes vs batched %d bytes\n", nS, nB);
            return 1;
        }
        printf("cabac ueg serial-vs-batched differential: OK (%d bytes)\n", nS);
    }

    /* residual block round-trip: encode random coefficient blocks for each cat,
 * decode them back, and compare. */
    enum { M = 4000 };
    static int rc_cat[M], rc_nza[M], rc_nzb[M];
    static dctcoef rc_blk[M][16];
    static const int catN[5] = { 16, 15, 16, 4, 15 };
    y264_cabac_t rc;
    static uint8_t buf2[1 << 20];
    y264_cabac_init_engine(&rc, buf2);
    y264_cabac_init_contexts(&rc, 1, 0, 26);
    for (int m = 0; m < M; m++) {
        int cat = nextr() % 5, n = catN[cat];
        rc_cat[m] = cat; rc_nza[m] = nextr() & 1; rc_nzb[m] = nextr() & 1;
        for (int i = 0; i < 16; i++) rc_blk[m][i] = 0;
        for (int i = 0; i < n; i++)
            if ((nextr() % 3) == 0) {              /* ~1/3 significant */
                int mag = (nextr() % 25) == 0 ? 16 + nextr() % 3000 :   /* long UEG0 tails */
                          (nextr() % 10) == 0 ? 1 + nextr() % 40 : 1 + nextr() % 3;
                rc_blk[m][i] = (nextr() & 1) ? -mag : mag;
            }
        y264_cabac_residual(&rc, cat, rc_blk[m], rc_nza[m], rc_nzb[m]);
    }
    y264_cabac_encode_terminate(&rc, 1);

    dec_t d2;
    { y264_cabac_t tmp; y264_cabac_init_contexts(&tmp, 1, 0, 26); memcpy(d2.ctx, tmp.ctx, sizeof(d2.ctx)); }
    d_init(&d2, buf2);
    for (int m = 0; m < M; m++) {
        dctcoef got[16];
        d_residual(&d2, rc_cat[m], got, rc_nza[m], rc_nzb[m]);
        for (int i = 0; i < catN[rc_cat[m]]; i++)
            if (got[i] != rc_blk[m][i]) {
                printf("FAIL residual block %d cat %d pos %d: enc %d dec %d\n",
                       m, rc_cat[m], i, rc_blk[m][i], got[i]);
                return 1;
            }
    }
    printf("cabac residual round-trip: %d blocks OK (%d bytes)\n", M, y264_cabac_bytes(&rc));

    /* 8x8 residual round-trip (ctxBlockCat 5, no coded_block_flag). */
    {
        static const uint8_t S8[63] = {
            0,1,2,3,4,5,5,4,4,3,3,4,4,4,5,5,4,4,4,4,3,3,6,7,7,7,8,9,10,9,8,7,
            7,6,11,12,13,11,6,7,8,9,14,10,9,8,6,11,12,13,11,6,9,14,10,9,11,12,13,11,14,10,12 };
        static const uint8_t L8[63] = {
            0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
            3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,6,6,6,6,7,7,7,7,8,8,8 };
        enum { M8 = 3000 };
        static dctcoef b8[M8][64], g8[64];
        y264_cabac_t e8;
        static uint8_t buf8[1 << 21];
        y264_cabac_init_engine(&e8, buf8);
        y264_cabac_init_contexts(&e8, 1, 0, 26);
        for (int m = 0; m < M8; m++) {
            int any = 0;
            for (int i = 0; i < 64; i++) b8[m][i] = 0;
            for (int i = 0; i < 64; i++)
                if ((nextr() % 4) == 0) {
                    int mag = (nextr() % 30) == 0 ? 16 + nextr() % 3000 :  /* long UEG0 tails */
                              (nextr() % 12) == 0 ? 1 + nextr() % 40 : 1 + nextr() % 3;
                    b8[m][i] = (nextr() & 1) ? -mag : mag; any = 1;
                }
            if (!any) b8[m][nextr() % 64] = 1;      /* cat5 is only coded when non-empty */
            y264_cabac_residual_8x8(&e8, b8[m]);
        }
        y264_cabac_encode_terminate(&e8, 1);

        dec_t d8;
        { y264_cabac_t tmp; y264_cabac_init_contexts(&tmp, 1, 0, 26); memcpy(d8.ctx, tmp.ctx, sizeof(d8.ctx)); }
        d_init(&d8, buf8);
        for (int m = 0; m < M8; m++) {
            for (int i = 0; i < 64; i++) g8[i] = 0;
            int last = -1;
            for (int i = 0; i < 64; i++) if (b8[m][i]) last = i;   /* known non-empty */
            (void)last;
            int pos[64], np = 0, i = 0;
            while (1) {
                if (d_decision(&d8, 402 + S8[i])) { pos[np++] = i; if (d_decision(&d8, 417 + L8[i])) break; }
                if (++i == 63) { pos[np++] = i; break; }
            }
            int node = 0;
            for (int k = np - 1; k >= 0; k--) {
                int a = 1, cx = D_L1[node] + 426;
                if (d_decision(&d8, cx)) {
                    cx = D_LG1[node] + 426; int t = 0;
                    while (t < 13 && d_decision(&d8, cx)) t++;
                    a = 2 + t; if (t == 13) a = 15 + d_ueg(&d8, 0);
                    node = D_TR[1][node];
                } else node = D_TR[0][node];
                int sign = d_bypass(&d8);
                g8[pos[k]] = (dctcoef)(sign ? -a : a);
            }
            for (int j = 0; j < 64; j++)
                if (g8[j] != b8[m][j]) {
                    printf("FAIL 8x8 block %d pos %d: enc %d dec %d\n", m, j, b8[m][j], g8[j]);
                    return 1;
                }
        }
        printf("cabac 8x8 residual round-trip: %d blocks OK\n", M8);
    }
    return 0;
}
