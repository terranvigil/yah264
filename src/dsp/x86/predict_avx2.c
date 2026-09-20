/*
 * predict_avx2.c - AVX2 kernels for intra prediction (ITU-T H.264 8.3)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3b of docs/x86-plan.md, the AVX2 twins of predict_sse4.c.
 *
 * WHICH SHAPES WIDEN. A 16x16 prediction ROW is sixteen bytes and a 16x16
 * plane row is sixteen 16-bit lanes, so two rows are one 256-bit register on
 * both sides of the pack and the whole builder runs at two rows a step. The
 * chroma builder's rows are eight bytes, so four of them fill a register for
 * the fills and two for the plane. Both are in this file.
 *
 * WHICH DO NOT, AND WHY. The 8x8 builders keep predict_x86.h's 128-bit
 * bodies. An 8x8 row is eight bytes; the filtered edge arrays are 24 and 32
 * bytes, so neither a 32-byte load nor a 32-byte store exists that stays
 * inside them, and the only widening left would be a cross-lane shuffle per
 * row pair -- a permute added to buy one merged store. That is not a wider
 * kernel, it is the same kernel with a lane fixup, so this tier takes the
 * shared bodies and gets VEX encodings of them: three-operand forms, so the
 * register copies the two-operand encodings need go away.
 *
 * Bit-exact with the SSE4.2 tier and with the C reference.
 *
 * C11 intrinsics only.
 */
#include "dsp/arch.h"

#if Y264_HAVE_AVX2

#include "dsp/x86/predict_x86.h"

/* Two rows of sixteen 16-bit lanes -> thirty-two bytes in row order. PACKUSWB
 * works inside each 128-bit half, so the four qwords come out interleaved as
 * (rowA lo, rowB lo, rowA hi, rowB hi) and one qword permute straightens them
 * -- and saturates at both ends on the way, which IS Clip1. */
static inline __m256i y264_pack2rows(__m256i a, __m256i b)
{
    return _mm256_permute4x64_epi64(_mm256_packus_epi16(a, b),
                                    _MM_SHUFFLE(3, 1, 2, 0));
}

void y264_intra16x16_avx2(pixel pred[256], const pixel *rec, int stride,
                          int mode, int have_top, int have_left)
{
    uint8_t left[16];
    for (int i = 0; i < 16; i++)
        left[i] = have_left ? rec[i * stride - 1] : 0;

    switch (mode) {
    case Y264_I16_VERT: {
        __m128i t = have_top ? y264_ld128(rec - stride) : _mm_setzero_si128();
        __m256i v = _mm256_broadcastsi128_si256(t);
        for (int y = 0; y < 16; y += 2)
            _mm256_storeu_si256((__m256i *)(void *)(pred + 16 * y), v);
        return;
    }
    case Y264_I16_HORIZ:
        for (int y = 0; y < 16; y += 2)
            _mm256_storeu_si256((__m256i *)(void *)(pred + 16 * y),
                _mm256_setr_m128i(_mm_set1_epi8((char)left[y]),
                                  _mm_set1_epi8((char)left[y + 1])));
        return;
    case Y264_I16_DC: {
        int st = 0, sl = 0;
        for (int i = 0; i < 16; i++) {
            st += have_top ? rec[-stride + i] : 0;
            sl += left[i];
        }
        int dc;
        if (have_top && have_left) dc = (st + sl + 16) >> 5;
        else if (have_top)         dc = (st + 8) >> 4;
        else if (have_left)        dc = (sl + 8) >> 4;
        else                       dc = 1 << (Y264_BIT_DEPTH - 1);
        __m256i v = _mm256_set1_epi8((char)dc);
        for (int y = 0; y < 16; y += 2)
            _mm256_storeu_si256((__m256i *)(void *)(pred + 16 * y), v);
        return;
    }
    case Y264_I16_PLANE: {
        const pixel *top = rec - stride;
        int corner = rec[-stride - 1];
        int H = 0, Vv = 0;
        for (int x = 0; x < 8; x++) {
            int tprev = (6 - x >= 0) ? top[6 - x] : corner;
            H += (x + 1) * (top[8 + x] - tprev);
        }
        for (int y = 0; y < 8; y++) {
            int lprev = (6 - y >= 0) ? left[6 - y] : corner;
            Vv += (y + 1) * (left[8 + y] - lprev);
        }
        int b = (5 * H + 32) >> 6;
        int c = (5 * Vv + 32) >> 6;
        int a = 16 * (left[15] + top[15]);
        int16_t bx[16];
        for (int x = 0; x < 16; x++) bx[x] = (int16_t)(b * (x - 7));
        __m256i bxv = _mm256_loadu_si256((const __m256i *)(const void *)bx);
        for (int y = 0; y < 16; y += 2) {
            __m256i r0 = _mm256_srai_epi16(_mm256_add_epi16(
                _mm256_set1_epi16((short)(a + c * (y - 7) + 16)), bxv), 5);
            __m256i r1 = _mm256_srai_epi16(_mm256_add_epi16(
                _mm256_set1_epi16((short)(a + c * (y - 6) + 16)), bxv), 5);
            _mm256_storeu_si256((__m256i *)(void *)(pred + 16 * y),
                                y264_pack2rows(r0, r1));
        }
        return;
    }
    default: break;
    }
}

void y264_intra_chroma_avx2(pixel *pred, const pixel *rec, int stride,
                            int mode, int have_top, int have_left,
                            int cw, int ch)
{
    uint8_t top[8], left[16];
    (void)cw;                                   /* dispatcher guarantees 8 */
    for (int i = 0; i < 8; i++) top[i] = have_top ? rec[-stride + i] : 0;
    for (int i = 0; i < ch; i++) left[i] = have_left ? rec[i * stride - 1] : 0;
    int corner = (have_top && have_left) ? rec[-stride - 1] : 0;

    switch (mode) {
    case Y264_IC_VERT: {
        __m256i v = _mm256_broadcastq_epi64(y264_ld64(top));
        for (int y = 0; y < ch; y += 4)
            _mm256_storeu_si256((__m256i *)(void *)(pred + 8 * y), v);
        return;
    }
    case Y264_IC_HORIZ:
        for (int y = 0; y < ch; y += 4) {
            __m128i a = _mm_unpacklo_epi64(_mm_set1_epi8((char)left[y]),
                                           _mm_set1_epi8((char)left[y + 1]));
            __m128i b = _mm_unpacklo_epi64(_mm_set1_epi8((char)left[y + 2]),
                                           _mm_set1_epi8((char)left[y + 3]));
            _mm256_storeu_si256((__m256i *)(void *)(pred + 8 * y),
                                _mm256_setr_m128i(a, b));
        }
        return;
    case Y264_IC_PLANE: {
        int yCF = (ch == 16) ? 4 : 0;
        int H = 0, Vv = 0;
        for (int x = 0; x <= 3; x++) {
            int i = 2 - x;
            int tprev = (i >= 0) ? top[i] : corner;
            H += (x + 1) * (top[4 + x] - tprev);
        }
        for (int y = 0; y <= 3 + yCF; y++) {
            int i = 2 + yCF - y;
            int lprev = (i >= 0) ? left[i] : corner;
            Vv += (y + 1) * (left[4 + yCF + y] - lprev);
        }
        int b = (34 * H + 32) >> 6;
        int c = ((34 - 29 * (ch == 16)) * Vv + 32) >> 6;
        int a = 16 * (left[ch - 1] + top[7]);
        int16_t bx[8];
        for (int x = 0; x < 8; x++) bx[x] = (int16_t)(b * (x - 3));
        __m256i bxv = _mm256_broadcastsi128_si256(
            _mm_loadu_si128((const __m128i *)(const void *)bx));
        for (int y = 0; y < ch; y += 2) {
            __m256i base = _mm256_setr_m128i(
                _mm_set1_epi16((short)(a + c * (y - 3 - yCF) + 16)),
                _mm_set1_epi16((short)(a + c * (y + 1 - 3 - yCF) + 16)));
            __m256i r = _mm256_srai_epi16(_mm256_add_epi16(base, bxv), 5);
            _mm_storeu_si128((__m128i *)(void *)(pred + 8 * y),
                             _mm256_castsi256_si128(y264_pack2rows(r, r)));
        }
        return;
    }
    default:
        /* IC_DC is per 4x4 block: eight scalar sums and four-byte fills, with
         * nothing for a wider register to do. */
        y264_ichroma_x86(pred, rec, stride, mode, have_top, have_left, cw, ch);
        return;
    }
}

void y264_intra8x8_from_edge_avx2(pixel pred[64], const pixel e[32], int mode)
{
    y264_i8_from_edge_x86(pred, e, mode);
}

void y264_intra8x8_avx2(pixel pred[64], const pixel *rec, int stride,
                        int mode, int have_top, int have_left,
                        int have_topleft, int have_topright)
{
    uint8_t e[32];
    y264_i8_edge_x86(e, rec, stride, have_top, have_left,
                     have_topleft, have_topright);
    y264_i8_from_edge_x86(pred, e, mode);
}

#endif /* Y264_HAVE_AVX2 */
