/*
 * intrapred_avx2_10.c - AVX2 intra prediction kernels for the 10-bit library
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * At 16 bits a sample, a 16x16 prediction row is exactly one 256-bit register
 * and two eight-wide chroma rows are another, so those two builders are
 * written out here: fills a row (or two) to a store, and the plane modes'
 * int32 sums eight to a register, packed and put back in order with one
 * qword permute.
 *
 * The 8x8 forms keep the shared 128-bit bodies, VEX-encoded: an 8x8 row is
 * one xmm, and the byte-plane shuffles that build the directional modes work
 * on sixteen-entry windows, which a 256-bit shuffle would only duplicate
 * across lanes. The edge derivation widens its integer view.
 */
#include "dsp/x86/intrapred_x86_10.h"

#if Y264_HAVE_AVX2_10

static inline void ip10_st16(pixel *p, __m256i v)
{
    _mm256_storeu_si256((__m256i *)(void *)p, v);
}

/* sixteen int32 sums (in order, eight per register) to sixteen clipped
 * samples. packus works per 128-bit lane, so the qwords come out as
 * (lo 0-3, hi 0-3, lo 4-7, hi 4-7) and the permute straightens them. */
static inline __m256i ip10_clip16(__m256i lo, __m256i hi)
{
    __m256i p = _mm256_packus_epi32(_mm256_srai_epi32(lo, 5),
                                    _mm256_srai_epi32(hi, 5));
    return _mm256_min_epu16(_mm256_permute4x64_epi64(p, _MM_SHUFFLE(3, 1, 2, 0)),
                            _mm256_set1_epi16(PIXEL_MAX));
}

void y264_intra16x16_avx2(pixel pred[256], const pixel *rec, int stride,
                          int mode, int have_top, int have_left)
{
    switch (mode) {
    case Y264_I16_VERT: {
        __m256i t = have_top
            ? _mm256_loadu_si256((const __m256i *)(const void *)(rec - stride))
            : _mm256_setzero_si256();
        for (int y = 0; y < 16; y++) ip10_st16(pred + 16 * y, t);
        return;
    }
    case Y264_I16_HORIZ:
        for (int y = 0; y < 16; y++)
            ip10_st16(pred + 16 * y,
                      _mm256_set1_epi16((short)(have_left ? rec[y * stride - 1] : 0)));
        return;
    case Y264_I16_DC: {
        __m256i v = _mm256_set1_epi16((short)ip10_i16_dc(rec, stride, have_top,
                                                         have_left));
        for (int y = 0; y < 16; y++) ip10_st16(pred + 16 * y, v);
        return;
    }
    case Y264_I16_PLANE: {
        int a, b, c;
        ip10_i16_plane_abc(rec, stride, &a, &b, &c);
        __m256i vb = _mm256_set1_epi32(b), vc = _mm256_set1_epi32(c);
        __m256i bx0 = _mm256_mullo_epi32(_mm256_setr_epi32(-7, -6, -5, -4,
                                                           -3, -2, -1, 0), vb);
        __m256i bx1 = _mm256_mullo_epi32(_mm256_setr_epi32(1, 2, 3, 4,
                                                           5, 6, 7, 8), vb);
        __m256i base = _mm256_set1_epi32(a - 7 * c + 16);
        for (int y = 0; y < 16; y++) {
            ip10_st16(pred + 16 * y, ip10_clip16(_mm256_add_epi32(base, bx0),
                                                 _mm256_add_epi32(base, bx1)));
            base = _mm256_add_epi32(base, vc);
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
    (void)cw;                                   /* dispatcher guarantees 8 */
    switch (mode) {
    case Y264_IC_VERT: {
        __m128i t = have_top ? ip10_ld8(rec - stride) : _mm_setzero_si128();
        __m256i v = _mm256_broadcastsi128_si256(t);
        for (int y = 0; y < ch; y += 2) ip10_st16(pred + 8 * y, v);
        return;
    }
    case Y264_IC_HORIZ:
        for (int y = 0; y < ch; y += 2) {
            int l0 = have_left ? rec[y * stride - 1] : 0;
            int l1 = have_left ? rec[(y + 1) * stride - 1] : 0;
            ip10_st16(pred + 8 * y, _mm256_setr_m128i(_mm_set1_epi16((short)l0),
                                                      _mm_set1_epi16((short)l1)));
        }
        return;
    case Y264_IC_PLANE: {
        /* rows y and y + 1 meet in one pack: each is one register of eight
         * int32 sums, and the permute lays the two rows out in order */
        pixel top[8], left[16];
        memcpy(top, rec - stride, sizeof(top));
        for (int i = 0; i < ch; i++) left[i] = rec[i * stride - 1];
        int a, b, c, yCF = (ch == 16) ? 4 : 0;
        ip10_ichroma_abc(top, left, rec[-stride - 1], ch, &a, &b, &c);
        __m256i bx = _mm256_mullo_epi32(_mm256_setr_epi32(-3, -2, -1, 0, 1, 2, 3, 4),
                                        _mm256_set1_epi32(b));
        __m256i r0 = _mm256_add_epi32(_mm256_set1_epi32(a + c * (-3 - yCF) + 16), bx);
        __m256i vc = _mm256_set1_epi32(c), vc2 = _mm256_set1_epi32(2 * c);
        for (int y = 0; y < ch; y += 2) {
            ip10_st16(pred + 8 * y, ip10_clip16(r0, _mm256_add_epi32(r0, vc)));
            r0 = _mm256_add_epi32(r0, vc2);
        }
        return;
    }
    default:
        /* the per-4x4 DC is eight scalar sums and half-row fills, with nothing
         * for a wider register to do */
        ip10_ichroma(pred, rec, stride, mode, have_top, have_left, ch);
        return;
    }
}

void y264_intra8x8_from_edge_avx2(pixel pred[64], const pixel e[32], int mode)
{ ip10_i8_from_edge(pred, e, mode); }

void y264_intra8x8_avx2(pixel pred[64], const pixel *rec, int stride,
                        int mode, int have_top, int have_left,
                        int have_topleft, int have_topright)
{
    pixel e[32];
    __m128i t0, t1, lf;
    int tl = ip10_edge_regs(rec, stride, have_top, have_left, have_topleft,
                            have_topright, &t0, &t1, &lf);
    ip10_edge_flat(e, t0, t1, lf, tl);
    ip10_i8_from_edge(pred, e, mode);
}

/* both views of y264_i8_edge_t; the integer view eight entries to a store */
void y264_intra8x8_edge_avx2(y264_i8_edge_t *e, const pixel *rec, int stride,
                             int have_top, int have_left,
                             int have_topleft, int have_topright)
{
    __m128i t0, t1, lf;
    int tl = ip10_edge_regs(rec, stride, have_top, have_left, have_topleft,
                            have_topright, &t0, &t1, &lf);
    ip10_edge_flat(e->f, t0, t1, lf, tl);
    _mm256_storeu_si256((__m256i *)(void *)(e->t + 0), _mm256_cvtepu16_epi32(t0));
    _mm256_storeu_si256((__m256i *)(void *)(e->t + 8), _mm256_cvtepu16_epi32(t1));
    _mm256_storeu_si256((__m256i *)(void *)e->l, _mm256_cvtepu16_epi32(lf));
    e->tl = tl;
}

void y264_intra8x8_dir6_avx2(pixel out[6][64], const pixel e[32])
{ ip10_i8_dir6(out, e); }

void y264_intra8x8_dir6_rec_avx2(pixel out[6][64], const pixel *rec, int stride)
{
    pixel e[32];
    __m128i t0, t1, lf;
    int tl = ip10_edge_regs(rec, stride, 1, 1, 1, 0, &t0, &t1, &lf);
    ip10_edge_flat(e, t0, t1, lf, tl);
    ip10_i8_dir6(out, e);
}

void y264_intra8_dvhp_avx2(pixel out[4][64], const pixel top[8],
                           const pixel left[8], int dc, int a, int b, int c)
{ ip10_i8_dvhp(out, top, left, dc, a, b, c); }

#endif /* Y264_HAVE_AVX2_10 */
