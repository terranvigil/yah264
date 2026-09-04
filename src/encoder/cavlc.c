/*
 * cavlc.c - CAVLC residual coding (ITU-T H.264 9.2)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The VLC tables are the normative code tables from ITU-T H.264 (Tables 9-5,
 * 9-7/9-8, 9-9, 9-10). They are stored here as their literal binary codeword
 * strings and parsed into (length, value) pairs at first use, which keeps the
 * transcription checkable by eye against the standard and avoids hand
 * conversion errors. Table structure is validated by tests/test_cavlc.c.
 */
#include "cavlc.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* coeff_token, Table 9-5. Indexed [column][TotalCoeff 0..16][TrailingOnes 0..3].
 * Columns: 0: 0<=nC<2, 1: 2<=nC<4, 2: 4<=nC<8, 3: nC==-1 (4:2:0 chroma DC),
 * 4: nC==-2 (4:2:2 chroma DC). */
static const char *const CT_STR[5][17][4] = {
{ /* 0 <= nC < 2 */
 {"1","","",""},
 {"000101","01","",""},
 {"00000111","000100","001",""},
 {"000000111","00000110","0000101","00011"},
 {"0000000111","000000110","00000101","000011"},
 {"00000000111","0000000110","000000101","0000100"},
 {"0000000001111","00000000110","0000000101","00000100"},
 {"0000000001011","0000000001110","00000000101","000000100"},
 {"0000000001000","0000000001010","0000000001101","0000000100"},
 {"00000000001111","00000000001110","0000000001001","00000000100"},
 {"00000000001011","00000000001010","00000000001101","0000000001100"},
 {"000000000001111","000000000001110","00000000001001","00000000001100"},
 {"000000000001011","000000000001010","000000000001101","00000000001000"},
 {"0000000000001111","000000000000001","000000000001001","000000000001100"},
 {"0000000000001011","0000000000001110","0000000000001101","000000000001000"},
 {"0000000000000111","0000000000001010","0000000000001001","0000000000001100"},
 {"0000000000000100","0000000000000110","0000000000000101","0000000000001000"},
},
{ /* 2 <= nC < 4 */
 {"11","","",""},
 {"001011","10","",""},
 {"000111","00111","011",""},
 {"0000111","001010","001001","0101"},
 {"00000111","000110","000101","0100"},
 {"00000100","0000110","0000101","00110"},
 {"000000111","00000110","00000101","001000"},
 {"00000001111","000000110","000000101","000100"},
 {"00000001011","00000001110","00000001101","0000100"},
 {"000000001111","00000001010","00000001001","000000100"},
 {"000000001011","000000001110","000000001101","00000001100"},
 {"000000001000","000000001010","000000001001","00000001000"},
 {"0000000001111","0000000001110","0000000001101","000000001100"},
 {"0000000001011","0000000001010","0000000001001","0000000001100"},
 {"0000000000111","00000000001011","0000000000110","0000000001000"},
 {"00000000001001","00000000001000","00000000001010","0000000000001"},
 {"00000000000111","00000000000110","00000000000101","00000000000100"},
},
{ /* 4 <= nC < 8 */
 {"1111","","",""},
 {"001111","1110","",""},
 {"001011","01111","1101",""},
 {"001000","01100","01110","1100"},
 {"0001111","01010","01011","1011"},
 {"0001011","01000","01001","1010"},
 {"0001001","001110","001101","1001"},
 {"0001000","001010","001001","1000"},
 {"00001111","0001110","0001101","01101"},
 {"00001011","00001110","0001010","001100"},
 {"000001111","00001010","00001101","0001100"},
 {"000001011","000001110","00001001","00001100"},
 {"000001000","000001010","000001101","00001000"},
 {"0000001101","000000111","000001001","000001100"},
 {"0000001001","0000001100","0000001011","0000001010"},
 {"0000000101","0000001000","0000000111","0000000110"},
 {"0000000001","0000000100","0000000011","0000000010"},
},
{ /* nC == -1 (chroma DC, 4:2:0) */
 {"01","","",""},
 {"000111","1","",""},
 {"000100","000110","001",""},
 {"000011","0000011","0000010","000101"},
 {"000010","00000011","00000010","0000000"},
 {"","","",""},{"","","",""},{"","","",""},{"","","",""},{"","","",""},
 {"","","",""},{"","","",""},{"","","",""},{"","","",""},{"","","",""},
 {"","","",""},{"","","",""},
},
{ /* nC == -2 (chroma DC, 4:2:2) */
 {"1","","",""},
 {"0001111","01","",""},
 {"0001110","0001101","001",""},
 {"000000111","0001100","0001011","00001"},
 {"000000110","000000101","0001010","000001"},
 {"0000000111","0000000110","000000100","0001001"},
 {"00000000111","00000000110","0000000101","0001000"},
 {"000000000111","000000000110","00000000101","0000000100"},
 {"0000000000111","000000000101","000000000100","00000000100"},
 {"","","",""},{"","","",""},{"","","",""},{"","","",""},{"","","",""},
 {"","","",""},{"","","",""},{"","","",""},
},
};

/* total_zeros for 4x4 blocks, Tables 9-7/9-8. Indexed [total_zeros][TotalCoeff]. */
static const char *const TZ4_STR[16][16] = {
 {"","1","111","0101","00011","0101","000001","000001","000001","000001","00001","0000","0000","000","00","0"},
 {"","011","110","111","111","0100","00001","00001","0001","000000","00000","0001","0001","001","01","1"},
 {"","010","101","110","0101","0011","111","101","00001","0001","001","001","01","1","1",""},
 {"","0011","100","101","0100","111","110","100","011","11","11","010","1","01","",""},
 {"","0010","011","0100","110","110","101","011","11","10","10","1","001","","",""},
 {"","00011","0101","0011","101","101","100","11","10","001","01","011","","","",""},
 {"","00010","0100","100","100","100","011","010","010","01","0001","","","","",""},
 {"","000011","0011","011","0011","011","010","0001","001","00001","","","","","",""},
 {"","000010","0010","0010","011","0010","0001","001","000000","","","","","","",""},
 {"","0000011","00011","00011","0010","00001","001","000000","","","","","","","",""},
 {"","0000010","00010","00010","00010","0001","000000","","","","","","","","",""},
 {"","00000011","000011","000001","00001","00000","","","","","","","","","",""},
 {"","00000010","000010","00001","00000","","","","","","","","","","",""},
 {"","000000011","000001","000000","","","","","","","","","","","",""},
 {"","000000010","000000","","","","","","","","","","","","",""},
 {"","000000001","","","","","","","","","","","","","",""},
};

/* total_zeros for chroma DC (4 coeffs), Table 9-9(a). [total_zeros][TotalCoeff]. */
static const char *const TZC_STR[4][4] = {
 {"","1","1","1"},
 {"","01","01","0"},
 {"","001","00",""},
 {"","000","","",},
};

/* total_zeros for 4:2:2 chroma DC (8 coeffs), Table 9-9(b). [total_zeros 0..7]
 * [tzVlcIndex 0..7] (column 0 unused; tzVlcIndex == TotalCoeff 1..7). */
static const char *const TZC8_STR[8][8] = {
 {"","1",   "000","000","110","00","00","0"},
 {"","010", "01", "001","00", "01","01","1"},
 {"","011", "001","01", "01", "10","1", ""},
 {"","0010","100","10", "10", "11","",  ""},
 {"","0011","101","110","111","",  "",  ""},
 {"","0001","110","111","",   "",  "",  ""},
 {"","00001","111","",  "",   "",  "",  ""},
 {"","00000","",   "",  "",   "",  "",  ""},
};

/* run_before, Table 9-10. Indexed [run][min(zerosLeft,7)], zerosLeft 1..7(=>6+). */
static const char *const RB_STR[15][8] = {
 {"","1","1","11","11","11","11","111"},
 {"","0","01","10","10","10","000","110"},
 {"","","00","01","01","011","001","101"},
 {"","","","00","001","010","011","100"},
 {"","","","","000","001","010","011"},
 {"","","","","","000","101","010"},
 {"","","","","","","100","001"},
 {"","","","","","","","0001"},
 {"","","","","","","","00001"},
 {"","","","","","","","000001"},
 {"","","","","","","","0000001"},
 {"","","","","","","","00000001"},
 {"","","","","","","","000000001"},
 {"","","","","","","","0000000001"},
 {"","","","","","","","00000000001"},
};

typedef struct { uint8_t len; uint16_t code; } vlc_t;

static vlc_t g_ct[5][17][4];
static vlc_t g_tz4[16][16];
static vlc_t g_tzc[4][4];
static vlc_t g_tzc8[8][8];
static vlc_t g_rb[15][8];
static _Atomic int g_init;

static vlc_t parse(const char *s)
{
    vlc_t v = { 0, 0 };
    for (; *s; s++) {
        v.code = (uint16_t)((v.code << 1) | (*s - '0'));
        v.len++;
    }
    return v;
}

/* Parsed once, under pthread_once: the plain `if (g_init) return;` guard raced
 * from every worker that reached CAVLC first (same values written, but a real
 * data race, and it drowned the TSan gate). y264_cavlc_warm primes it at
 * encoder open, before any worker exists. */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void table_init(void)
{
    for (int c = 0; c < 5; c++)
        for (int i = 0; i < 17; i++)
            for (int t = 0; t < 4; t++)
                g_ct[c][i][t] = parse(CT_STR[c][i][t]);
    for (int z = 0; z < 16; z++)
        for (int t = 0; t < 16; t++)
            g_tz4[z][t] = parse(TZ4_STR[z][t]);
    for (int z = 0; z < 4; z++)
        for (int t = 0; t < 4; t++)
            g_tzc[z][t] = parse(TZC_STR[z][t]);
    for (int z = 0; z < 8; z++)
        for (int t = 0; t < 8; t++)
            g_tzc8[z][t] = parse(TZC8_STR[z][t]);
    for (int r = 0; r < 15; r++)
        for (int z = 0; z < 8; z++)
            g_rb[r][z] = parse(RB_STR[r][z]);
    atomic_store_explicit(&g_init, 1, memory_order_release);
}
/* Fast path: one acquire load (pairs with table_init's release store); the
 * per-call pthread_once walk was visible in the profile. */
static inline void ensure_init(void)
{
    if (!atomic_load_explicit(&g_init, memory_order_acquire))
        pthread_once(&g_once, table_init);
}
static _Atomic int g_prefix15;   /* profile < 100: level_prefix must stay <= 15 */
void y264_cavlc_set_prefix15(int on) { g_prefix15 = on; }
void y264_cavlc_warm(void) { ensure_init(); }

static void put_vlc(y264_bs_t *bs, vlc_t v)
{
    y264_bs_write(bs, v.len, v.code);
}

/* level_prefix (unary: `prefix` zeros then a 1) plus a `size`-bit suffix. */
static void write_level(y264_bs_t *bs, int level_code, int suffix_length)
{
    int prefix, size, suffix;
    if (suffix_length == 0) {
        if (level_code < 14) {
            prefix = level_code; size = 0; suffix = 0;
        } else if (level_code < 30) {
            prefix = 14; size = 4; suffix = level_code - 14;
        } else {
            int r = level_code - 30, base = 0;
            prefix = 15; size = 12;
            while (r - base >= (1 << size)) { base += (1 << size); prefix++; size++; }
            suffix = r - base;
        }
    } else {
        if ((level_code >> suffix_length) < 15) {
            prefix = level_code >> suffix_length;
            size = suffix_length;
            suffix = level_code & ((1 << suffix_length) - 1);
        } else {
            int r = level_code - (15 << suffix_length), base = 0;
            prefix = 15; size = 12;
            while (r - base >= (1 << size)) { base += (1 << size); prefix++; size++; }
            suffix = r - base;
        }
    }
    if (prefix > 15 && g_prefix15) {           /* the quantiser cap should make this unreachable */
        static _Atomic int said;
        if (!atomic_exchange(&said, 1))
            fprintf(stderr, "yah264: internal: CAVLC level_prefix %d > 15 at a profile that forbids it\n", prefix);
    }
    y264_bs_write_zero(bs, prefix);
    y264_bs_write1(bs, 1);
    if (size > 0)
        y264_bs_write(bs, size, suffix);
}

void y264_cavlc_coeff_token(int col, int tc, int t1, int *len, int *code)
{
    ensure_init();
    vlc_t v = g_ct[col][tc][t1];
    *len = v.len; *code = v.code;
}

void y264_cavlc_total_zeros(int max_num_coeff, int tc, int tz, int *len, int *code)
{
    ensure_init();
    vlc_t v = { 0, 0 };
    if (max_num_coeff == 4) {
        if (tz >= 0 && tz < 4 && tc >= 0 && tc < 4) v = g_tzc[tz][tc];
    } else if (max_num_coeff == 8) {
        if (tz >= 0 && tz < 8 && tc >= 0 && tc < 8) v = g_tzc8[tz][tc];
    } else {
        if (tz >= 0 && tz < 16 && tc >= 0 && tc < 16) v = g_tz4[tz][tc];
    }
    *len = v.len; *code = v.code;
}

void y264_cavlc_run_before(int zeros_left, int run, int *len, int *code)
{
    ensure_init();
    int z = zeros_left > 7 ? 7 : zeros_left;
    vlc_t v = g_rb[run][z];
    *len = v.len; *code = v.code;
}

/* level_prefix + suffix LENGTH, without writing: the exact bit count
 * write_level above emits for the same (level_code, suffix_length). Kept
 * adjacent to it -- the two must move together. */
static inline int level_len(int level_code, int suffix_length)
{
    int prefix, size;
    if (suffix_length == 0) {
        if (level_code < 14) {
            prefix = level_code; size = 0;
        } else if (level_code < 30) {
            prefix = 14; size = 4;
        } else {
            int r = level_code - 30, base = 0;
            prefix = 15; size = 12;
            while (r - base >= (1 << size)) { base += (1 << size); prefix++; size++; }
        }
    } else {
        if ((level_code >> suffix_length) < 15) {
            prefix = level_code >> suffix_length;
            size = suffix_length;
        } else {
            int r = level_code - (15 << suffix_length), base = 0;
            prefix = 15; size = 12;
            while (r - base >= (1 << size)) { base += (1 << size); prefix++; size++; }
        }
    }
    return prefix + 1 + size;
}

/* Bit LENGTH of the block y264_cavlc_residual would write, computed without
 * a bitstream. The RD/est callers only ever wanted this number, and were paying
 * a bs_init plus the whole accumulator/flush path per priced block (the CAVLC
 * est bucket was ~4-6% of single-thread self time). Every codeword length comes
 * from the same tables the writer indexes, in the same order, so the two agree
 * by construction; tests/test_cavlc.c asserts it over random blocks.
 * `stride` prices an interleaved sub-block in place (the 8x8 CAVLC split reads
 * scan8[4*i+j] as sub-block j: base scan8+j, stride 4). */
static int cavlc_len_tc(const dctcoef *coeff, int max_num_coeff, int nC,
                        int stride, int *out_tc)
{
    ensure_init();

    /* Everything below reads the block only through the POSITIONS of its
 * non-zero coefficients, so take those once as a bitmask. The two scans
 * this replaces walked max_num_coeff and then last_nz+1 positions with a
 * data-dependent branch each; the mask build is branchless and the gather
 * runs total_coeff times. This is the RDOQ and intra-decision rate model
 * and it is 1.9% of the shipped wall on samsung, so the branches are the
 * expensive part rather than the table walks. (x264 vectorises the same
 * step as coeff_last / coeff_level_run.) */
    uint32_t msk = 0;
    for (int i = 0; i < max_num_coeff; i++)
        msk |= (uint32_t)(coeff[i * stride] != 0) << i;
    int total_coeff = __builtin_popcount(msk);
    int last_nz = msk ? 31 - __builtin_clz(msk) : -1;
    int idx[16], m = 0;
    for (uint32_t t = msk; t; ) {                     /* high -> low frequency */
        int i = 31 - __builtin_clz(t);
        idx[m++] = i;
        t ^= 1u << i;
    }

    int t1 = 0;
    for (int k = 0; k < total_coeff && k < 3; k++) {
        int a = coeff[idx[k] * stride];
        if (a == 1 || a == -1) t1++;
        else break;
    }

    if (out_tc) *out_tc = total_coeff;

    int bits;
    if (nC >= 8)
        bits = 6;
    else {
        int col = (nC == -2) ? 4 : (nC == -1) ? 3 : (nC < 2) ? 0 : (nC < 4) ? 1 : 2;
        bits = g_ct[col][total_coeff][t1].len;
    }

    if (total_coeff == 0)
        return bits;

    bits += t1;                                       /* trailing-one signs */

    int suffix_length = (total_coeff > 10 && t1 < 3) ? 1 : 0;
    for (int k = t1; k < total_coeff; k++) {
        int level = coeff[idx[k] * stride];
        int level_code = (level > 0) ? (level << 1) - 2 : (-level << 1) - 1;
        if (k == t1 && t1 < 3)
            level_code -= 2;
        bits += level_len(level_code, suffix_length);
        if (suffix_length == 0)
            suffix_length = 1;
        int a = level < 0 ? -level : level;
        if (a > (3 << (suffix_length - 1)) && suffix_length < 6)
            suffix_length++;
    }

    int total_zeros = (last_nz + 1) - total_coeff;
    if (total_coeff < max_num_coeff) {
        if (max_num_coeff == 4)
            bits += g_tzc[total_zeros][total_coeff].len;
        else if (max_num_coeff == 8)
            bits += g_tzc8[total_zeros][total_coeff].len;
        else
            bits += g_tz4[total_zeros][total_coeff].len;
    }

    int zeros_left = total_zeros;
    for (int k = 0; k < total_coeff - 1 && zeros_left > 0; k++) {
        int run = idx[k] - idx[k + 1] - 1;
        int z = zeros_left > 7 ? 7 : zeros_left;
        bits += g_rb[run][z].len;
        zeros_left -= run;
    }

    return bits;
}

int y264_cavlc_residual_len(const dctcoef *coeff, int max_num_coeff, int nC,
                            int stride)
{
    return cavlc_len_tc(coeff, max_num_coeff, nC, stride, NULL);
}

int y264_cavlc_residual(y264_bs_t *bs, const dctcoef *coeff,
                        int max_num_coeff, int nC)
{
    /* Pricing writer: the caller wants only the length, so take it from the
 * counting path instead of walking the coefficients a second time through
 * the bit accumulator. */
    if (bs->count_only) {
        int tc;
        bs->bits += cavlc_len_tc(coeff, max_num_coeff, nC, 1, &tc);
        return tc;
    }
    ensure_init();

    int total_coeff = 0, last_nz = -1;
    int idx[16], m = 0;
    for (int i = 0; i < max_num_coeff; i++)
        if (coeff[i]) { total_coeff++; last_nz = i; }
    for (int i = last_nz; i >= 0; i--)
        if (coeff[i]) idx[m++] = i;               /* high -> low frequency */

    int t1 = 0;
    for (int k = 0; k < total_coeff && k < 3; k++) {
        int a = coeff[idx[k]];
        if (a == 1 || a == -1) t1++;
        else break;
    }

    /* coeff_token */
    if (nC >= 8) {
        int code = (total_coeff == 0) ? 3 : (((total_coeff - 1) << 2) + t1);
        y264_bs_write(bs, 6, code);
    } else {
        int col = (nC == -2) ? 4 : (nC == -1) ? 3 : (nC < 2) ? 0 : (nC < 4) ? 1 : 2;
        put_vlc(bs, g_ct[col][total_coeff][t1]);
    }

    if (total_coeff == 0)
        return 0;

    /* trailing_ones_sign_flag, high -> low frequency */
    for (int k = 0; k < t1; k++)
        y264_bs_write1(bs, coeff[idx[k]] < 0 ? 1 : 0);

    /* levels */
    int suffix_length = (total_coeff > 10 && t1 < 3) ? 1 : 0;
    for (int k = t1; k < total_coeff; k++) {
        int level = coeff[idx[k]];
        int level_code = (level > 0) ? (level << 1) - 2 : (-level << 1) - 1;
        if (k == t1 && t1 < 3)
            level_code -= 2;
        write_level(bs, level_code, suffix_length);
        if (suffix_length == 0)
            suffix_length = 1;
        int a = level < 0 ? -level : level;
        if (a > (3 << (suffix_length - 1)) && suffix_length < 6)
            suffix_length++;
    }

    /* total_zeros */
    int total_zeros = (last_nz + 1) - total_coeff;
    if (total_coeff < max_num_coeff) {
        if (max_num_coeff == 4)
            put_vlc(bs, g_tzc[total_zeros][total_coeff]);
        else if (max_num_coeff == 8)
            put_vlc(bs, g_tzc8[total_zeros][total_coeff]);
        else
            put_vlc(bs, g_tz4[total_zeros][total_coeff]);
    }

    /* run_before */
    int zeros_left = total_zeros;
    for (int k = 0; k < total_coeff - 1 && zeros_left > 0; k++) {
        int run = idx[k] - idx[k + 1] - 1;
        int z = zeros_left > 7 ? 7 : zeros_left;
        put_vlc(bs, g_rb[run][z]);
        zeros_left -= run;
    }

    return total_coeff;
}
