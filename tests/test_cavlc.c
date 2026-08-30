/*
 * test_cavlc.c - structural and round-trip validation of CAVLC residual coding
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Two independent checks:
 * 1. Every VLC table is a valid prefix code with no duplicate codewords. This
 * catches most transcription errors (wrong length, collision) with no
 * external oracle.
 * 2. Random coefficient blocks are encoded and then decoded by an independent
 * in-test decoder, verifying the encoder's level/total_zeros/run_before
 * logic and that the whole thing is self-consistent.
 */
#include "encoder/cavlc.h"
#include "common/bitstream.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } \
} while (0)

typedef struct { int len, code, a, b; } cw_t;   /* codeword + two labels */

static int is_prefix(cw_t x, cw_t y)
{
    if (x.len == 0 || y.len == 0 || x.len > y.len) return 0;
    return (y.code >> (y.len - x.len)) == x.code;
}

static void check_prefix_free(cw_t *cw, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (i != j && is_prefix(cw[i], cw[j]))
                CHECK(0, "%s: codeword (%d,%d) [%d,%d] is a prefix of (%d,%d) [%d,%d]",
                      name, cw[i].a, cw[i].b, cw[i].len, cw[i].code,
                      cw[j].a, cw[j].b, cw[j].len, cw[j].code);
}

static void test_tables_prefix_free(void)
{
    /* coeff_token: each column independently prefix-free. */
    for (int col = 0; col < 4; col++) {
        cw_t cw[68]; int n = 0;
        for (int tc = 0; tc < 17; tc++)
            for (int t1 = 0; t1 < 4; t1++) {
                int len, code;
                y264_cavlc_coeff_token(col, tc, t1, &len, &code);
                if (len) cw[n++] = (cw_t){ len, code, tc, t1 };
            }
        char nm[32]; snprintf(nm, sizeof(nm), "coeff_token[col %d]", col);
        check_prefix_free(cw, n, nm);
    }
    /* total_zeros 4x4: prefix-free per TotalCoeff. */
    for (int tc = 1; tc <= 15; tc++) {
        cw_t cw[16]; int n = 0;
        for (int tz = 0; tz < 16; tz++) {
            int len, code;
            y264_cavlc_total_zeros(16, tc, tz, &len, &code);
            if (len) cw[n++] = (cw_t){ len, code, tc, tz };
        }
        char nm[32]; snprintf(nm, sizeof(nm), "total_zeros4[tc %d]", tc);
        check_prefix_free(cw, n, nm);
    }
    /* total_zeros chroma DC: prefix-free per TotalCoeff. */
    for (int tc = 1; tc <= 3; tc++) {
        cw_t cw[4]; int n = 0;
        for (int tz = 0; tz < 4; tz++) {
            int len, code;
            y264_cavlc_total_zeros(4, tc, tz, &len, &code);
            if (len) cw[n++] = (cw_t){ len, code, tc, tz };
        }
        char nm[32]; snprintf(nm, sizeof(nm), "total_zerosC[tc %d]", tc);
        check_prefix_free(cw, n, nm);
    }
    /* run_before: prefix-free per zerosLeft context. */
    for (int zl = 1; zl <= 7; zl++) {
        cw_t cw[15]; int n = 0;
        for (int run = 0; run < 15; run++) {
            int len, code;
            y264_cavlc_run_before(zl, run, &len, &code);
            if (len) cw[n++] = (cw_t){ len, code, zl, run };
        }
        char nm[32]; snprintf(nm, sizeof(nm), "run_before[zl %d]", zl);
        check_prefix_free(cw, n, nm);
    }
}

/* --- independent decoder for round-trip --- */

typedef struct { const uint8_t *p; int bit; int end_bits; int pos; } rdr_t;

static int rbit(rdr_t *r)
{
    int b = (r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
    r->pos++;
    return b;
}

/* Match the next bits against a codeword list; returns index or -1. */
static int dec_vlc(rdr_t *r, const cw_t *cw, int n)
{
    int len = 0, code = 0;
    while (len < 24) {
        code = (code << 1) | rbit(r);
        len++;
        for (int i = 0; i < n; i++)
            if (cw[i].len == len && cw[i].code == code)
                return i;
    }
    return -1;
}

static int dec_level(rdr_t *r, int suffix_length)
{
    int level_prefix = 0;
    while (rbit(r) == 0) level_prefix++;
    int size;
    if (level_prefix == 14 && suffix_length == 0) size = 4;
    else if (level_prefix >= 15) size = level_prefix - 3;
    else size = suffix_length;
    int suffix = 0;
    for (int i = 0; i < size; i++) suffix = (suffix << 1) | rbit(r);
    int lc = (level_prefix < 15 ? level_prefix : 15) << suffix_length;
    lc += suffix;
    if (level_prefix >= 15 && suffix_length == 0) lc += 15;
    if (level_prefix >= 16) lc += (1 << (level_prefix - 3)) - 4096;
    return lc;   /* caller converts to signed level */
}

static void roundtrip_one(const dctcoef *coeff, int maxc, int nC)
{
    uint8_t buf[512];
    y264_bs_t bs;
    y264_bs_init(&bs, buf, sizeof(buf));
    int tc_enc = y264_cavlc_residual(&bs, coeff, maxc, nC);
    /* mark stream end so the reader can't run past it */
    int end = (int)y264_bs_pos_bits(&bs);

    /* the writer-free pricing path must agree exactly, including strided
 * (interleaved 8x8 sub-block) access, which the RD callers use */
    CHECK(y264_cavlc_residual_len(coeff, maxc, nC, 1) == end,
          "residual_len %d != written %d (maxc=%d nC=%d)",
          y264_cavlc_residual_len(coeff, maxc, nC, 1), end, maxc, nC);
    {
        dctcoef spread[16 * 4];
        for (int i = 0; i < 16 * 4; i++) spread[i] = (dctcoef)(i * 7 + 1);
        for (int i = 0; i < maxc; i++) spread[i * 4 + 2] = coeff[i];
        CHECK(y264_cavlc_residual_len(spread + 2, maxc, nC, 4) == end,
              "strided residual_len mismatch (maxc=%d nC=%d)", maxc, nC);
    }

    y264_bs_flush(&bs);

    rdr_t r = { buf, 0, end, 0 };

    /* decode coeff_token */
    int total_coeff, t1;
    if (nC >= 8) {
        int code = 0;
        for (int i = 0; i < 6; i++) code = (code << 1) | rbit(&r);
        if (code == 3) { total_coeff = 0; t1 = 0; }
        else { total_coeff = (code >> 2) + 1; t1 = code & 3; }
    } else {
        int col = (nC == -1) ? 3 : (nC < 2) ? 0 : (nC < 4) ? 1 : 2;
        cw_t cw[68]; int n = 0;
        for (int tc = 0; tc < 17; tc++)
            for (int tt = 0; tt < 4; tt++) {
                int len, code;
                y264_cavlc_coeff_token(col, tc, tt, &len, &code);
                if (len) cw[n++] = (cw_t){ len, code, tc, tt };
            }
        int mi = dec_vlc(&r, cw, n);
        CHECK(mi >= 0, "coeff_token decode failed (nC=%d)", nC);
        if (mi < 0) return;
        total_coeff = cw[mi].a; t1 = cw[mi].b;
    }

    dctcoef out[16] = {0};
    CHECK(total_coeff == tc_enc, "TotalCoeff enc %d dec %d", tc_enc, total_coeff);
    if (total_coeff == 0) {
        for (int i = 0; i < maxc; i++)
            CHECK(coeff[i] == 0, "expected all-zero block");
        return;
    }

    int levels[16], li = 0;
    int signs[3];
    for (int k = 0; k < t1; k++) signs[k] = rbit(&r) ? -1 : 1;

    int suffix_length = (total_coeff > 10 && t1 < 3) ? 1 : 0;
    for (int k = t1; k < total_coeff; k++) {
        int lc = dec_level(&r, suffix_length);
        if (k == t1 && t1 < 3) lc += 2;
        int level = (lc & 1) ? -((lc + 1) >> 1) : ((lc + 2) >> 1);
        levels[li++] = level;
        if (suffix_length == 0) suffix_length = 1;
        int a = level < 0 ? -level : level;
        if (a > (3 << (suffix_length - 1)) && suffix_length < 6) suffix_length++;
    }

    /* total_zeros */
    int total_zeros = 0;
    if (total_coeff < maxc) {
        cw_t cw[16]; int n = 0;
        for (int tz = 0; tz < 16; tz++) {
            int len, code;
            y264_cavlc_total_zeros(maxc, total_coeff, tz, &len, &code);
            if (len) cw[n++] = (cw_t){ len, code, tz, 0 };
        }
        int mi = dec_vlc(&r, cw, n);
        CHECK(mi >= 0, "total_zeros decode failed");
        if (mi < 0) return;
        total_zeros = cw[mi].a;
    }

    /* run_before */
    int runs[16] = {0};
    int zeros_left = total_zeros;
    for (int k = 0; k < total_coeff - 1 && zeros_left > 0; k++) {
        cw_t cw[15]; int n = 0;
        for (int run = 0; run < 15; run++) {
            int len, code;
            y264_cavlc_run_before(zeros_left, run, &len, &code);
            if (len) cw[n++] = (cw_t){ len, code, run, 0 };
        }
        int mi = dec_vlc(&r, cw, n);
        CHECK(mi >= 0, "run_before decode failed");
        if (mi < 0) return;
        runs[k] = cw[mi].a;
        zeros_left -= runs[k];
    }
    runs[total_coeff - 1] += zeros_left;   /* remaining zeros go before lowest coeff */

    /* rebuild scan array from high frequency down */
    int pos = total_zeros + total_coeff - 1;   /* last_nz index */
    int coeff_vals[16];
    for (int k = 0; k < t1; k++) coeff_vals[k] = signs[k];
    for (int k = t1; k < total_coeff; k++) coeff_vals[k] = levels[k - t1];
    for (int k = 0; k < total_coeff; k++) {
        out[pos] = (dctcoef)coeff_vals[k];
        pos -= (runs[k] + 1);
    }

    for (int i = 0; i < maxc; i++)
        CHECK(out[i] == coeff[i], "coeff[%d] enc %d dec %d (nC=%d,maxc=%d)",
              i, coeff[i], out[i], nC, maxc);
}

static uint64_t rng = 0xf00dcafebabeULL;
static int rr(int lo, int hi)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return lo + (int)((rng >> 33) % (uint64_t)(hi - lo + 1));
}

static void test_roundtrip(void)
{
    struct { int maxc, nC; } ctx[] = {
        {16, 0}, {16, 2}, {16, 5}, {16, 9}, {15, 0}, {15, 3},
        {15, 6}, {4, -1}, {16, 8}, {16, 20},
    };
    for (size_t c = 0; c < sizeof(ctx) / sizeof(ctx[0]); c++) {
        for (int trial = 0; trial < 4000; trial++) {
            dctcoef coeff[16] = {0};
            int density = rr(0, 3);       /* vary sparsity */
            for (int i = 0; i < ctx[c].maxc; i++) {
                if (rr(0, 3) <= density) {
                    int mag = (rr(0, 9) == 0) ? rr(2, 40) : rr(1, 2);
                    coeff[i] = (dctcoef)(rr(0, 1) ? mag : -mag);
                }
            }
            roundtrip_one(coeff, ctx[c].maxc, ctx[c].nC);
            if (fails > 20) return;       /* stop spamming on systemic break */
        }
    }
}

int main(void)
{
    test_tables_prefix_free();
    test_roundtrip();
    if (fails) {
        printf("test_cavlc: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_cavlc: all passed\n");
    return 0;
}
