/*
 * deblock_avx2.c - AVX2 kernels for the deblock family (ITU-T H.264 8.7)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3b of docs/x86-plan.md, the AVX2 twins of deblock_sse4.c.
 *
 * WHICH OF THE FOUR ACTUALLY WIDENS, AND WHY THE OTHER THREE DO NOT. A luma
 * edge segment is FOUR lines and a chroma edge is EIGHT, because four lines is
 * the unit one bS value covers and eight is a whole chroma edge. Those are
 * four and eight lanes of 16-bit arithmetic, so a 256-bit register has nothing
 * to put in its upper half and a widened kernel would be the same work plus a
 * cross-lane fixup. Those three take deblock_x86.h's bodies whole and differ
 * from the SSE4.2 tier only in that this translation unit's flags make the
 * compiler emit VEX encodings of them -- three-operand forms, so the register
 * copies the two-operand encodings need go away, which is the honest part of
 * what this tier buys at these widths.
 *
 * The strength derivation is the one that widens, and it widens exactly:
 * sixteen edges of an axis are sixteen 16-bit lanes, so a whole axis is ONE
 * pass here where the SSE4.2 twin takes two. The five grid rows are read once
 * as 128-bit quarters and assembled into 256-bit sides, and the sixteen
 * strengths narrow into one store.
 *
 * Bit-exact with the SSE4.2 tier and with the C reference, which is what the
 * checkasm rows and the kit's identity leg both assert.
 *
 * C11 intrinsics only.
 */
#include "dsp/arch.h"

#if Y264_HAVE_AVX2

#include "dsp/x86/deblock_x86.h"

void y264_deblock_luma_v4_avx2(pixel *q0p, int stride, int bs, int alpha,
                               int beta, int tc0)
{
    y264_db_luma_v4_x86(q0p, stride, bs, alpha, beta, tc0);
}

void y264_deblock_luma_h4_avx2(pixel *q0p, int stride, int bs, int alpha,
                               int beta, int tc0)
{
    y264_db_luma_h4_x86(q0p, stride, bs, alpha, beta, tc0);
}

void y264_deblock_chroma8_h_avx2(pixel *q0p, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g)
{
    y264_db_chroma8_h_x86(q0p, stride, alpha, beta, bs, tc0tab, span, g);
}

/* ---- sixteen edges of an axis, one pass ---------------------------------- */

struct y264_db_side16 {
    __m256i r0, r1, x0, y0, x1, y1, co;
};

/* Four rows of four blocks into sixteen lanes, row-major -- which is the order
 * bsh wants directly and the order the vertical transpose expects. */
static inline __m256i y264_db_q4(__m128i a, __m128i b, __m128i c, __m128i d)
{
    return _mm256_setr_m128i(_mm_unpacklo_epi64(a, b), _mm_unpacklo_epi64(c, d));
}

static inline struct y264_db_side16 y264_db_quad(const struct y264_db_side *a,
                                                 const struct y264_db_side *b,
                                                 const struct y264_db_side *c,
                                                 const struct y264_db_side *d)
{
    struct y264_db_side16 s;
    s.r0 = y264_db_q4(a->r0, b->r0, c->r0, d->r0);
    s.r1 = y264_db_q4(a->r1, b->r1, c->r1, d->r1);
    s.x0 = y264_db_q4(a->x0, b->x0, c->x0, d->x0);
    s.y0 = y264_db_q4(a->y0, b->y0, c->y0, d->y0);
    s.x1 = y264_db_q4(a->x1, b->x1, c->x1, d->x1);
    s.y1 = y264_db_q4(a->y1, b->y1, c->y1, d->y1);
    s.co = y264_db_q4(a->co, b->co, c->co, d->co);
    return s;
}

static inline __m256i y264_abd16_256(__m256i a, __m256i b)
{
    return _mm256_sub_epi16(_mm256_max_epi16(a, b), _mm256_min_epi16(a, b));
}

/* 8.7.2.1 over sixteen edges. Same algebra as the 128-bit body in
 * deblock_x86.h: one compare per side per list answers both "used in list n"
 * and the intra test, and the motion test folds the two components with a max
 * before its single threshold. */
static inline __m128i y264_db_bs16(const struct y264_db_side16 *p,
                                   const struct y264_db_side16 *q, __m256i edge)
{
    const __m256i z = _mm256_setzero_si256();
    const __m256i ones = _mm256_cmpeq_epi16(z, z);
    const __m256i three = _mm256_set1_epi16(3);

    __m256i pn0 = _mm256_cmpgt_epi16(z, p->r0), pn1 = _mm256_cmpgt_epi16(z, p->r1);
    __m256i qn0 = _mm256_cmpgt_epi16(z, q->r0), qn1 = _mm256_cmpgt_epi16(z, q->r1);
    __m256i intra = _mm256_or_si256(_mm256_and_si256(pn0, pn1),
                                    _mm256_and_si256(qn0, qn1));
    __m256i coeff = _mm256_or_si256(p->co, q->co);

    __m256i u0 = _mm256_xor_si256(pn0, ones);
    __m256i u1 = _mm256_xor_si256(pn1, ones);

    __m256i one = _mm256_xor_si256(pn0, qn0);
    one = _mm256_or_si256(one, _mm256_xor_si256(pn1, qn1));
    one = _mm256_or_si256(one, _mm256_and_si256(u0,
              _mm256_xor_si256(_mm256_cmpeq_epi16(p->r0, q->r0), ones)));
    one = _mm256_or_si256(one, _mm256_and_si256(u1,
              _mm256_xor_si256(_mm256_cmpeq_epi16(p->r1, q->r1), ones)));
    __m256i d0 = _mm256_cmpgt_epi16(
        _mm256_max_epi16(y264_abd16_256(p->x0, q->x0),
                         y264_abd16_256(p->y0, q->y0)), three);
    one = _mm256_or_si256(one, _mm256_and_si256(u0, d0));
    __m256i d1 = _mm256_cmpgt_epi16(
        _mm256_max_epi16(y264_abd16_256(p->x1, q->x1),
                         y264_abd16_256(p->y1, q->y1)), three);
    one = _mm256_or_si256(one, _mm256_and_si256(u1, d1));

    __m256i bs = _mm256_and_si256(one, _mm256_set1_epi16(1));
    bs = _mm256_blendv_epi8(bs, _mm256_set1_epi16(2), coeff);
    bs = _mm256_blendv_epi8(bs, _mm256_blendv_epi8(_mm256_set1_epi16(3),
                                                   _mm256_set1_epi16(4), edge),
                            intra);
    /* PACKUSWB works inside each 128-bit half, so the sixteen strengths land
     * as two eight-byte groups a whole lane apart; one qword permute brings
     * them together in the low half. */
    __m256i pk = _mm256_packus_epi16(bs, bs);
    return _mm256_castsi256_si128(
        _mm256_permute4x64_epi64(pk, _MM_SHUFFLE(3, 1, 2, 0)));
}

void y264_deblock_strength_avx2(const struct y264_bs_ctx *c,
                                uint8_t bsv[4][4], uint8_t bsh[4][4])
{
    const int8_t *nnz = c->nnz;
    const int ns = c->nnz_stride;

    __m128i co[5];
    for (int y = 0; y < 4; y++)
        co[y + 1] = y264_db_co_row(nnz, ns, y, c->tr8_cur);
    co[0] = c->have_top ? y264_db_co_row(nnz, ns, -1, c->tr8_top) : co[1];

    struct y264_db_side hi[5];
    hi[0] = y264_db_load4(c, c->have_top ? -1 : 0, 0, co[0]);
    for (int y = 0; y < 4; y++)
        hi[y + 1] = y264_db_load4(c, y, 0, co[y + 1]);

    /* With no left macroblock the p column is outside the frame and must not
     * be read, so the p side is rotated out of the row itself; lane 0 then
     * holds column 3's value and feeds an edge the caller does not filter. */
    struct y264_db_side lo[4];
    for (int y = 0; y < 4; y++) {
        if (c->have_left) {
            int l = y264_db_co_at(nnz, ns, -1, y, c->tr8_left);
            lo[y] = y264_db_load4(c, y, -1, y264_db_co_shift(co[y + 1], l));
        } else {
            lo[y] = y264_db_rot4(hi[y + 1]);
        }
    }
    {
        const __m256i ev = _mm256_setr_epi16(-1, 0, 0, 0, -1, 0, 0, 0,
                                             -1, 0, 0, 0, -1, 0, 0, 0);
        struct y264_db_side16 p = y264_db_quad(&lo[0], &lo[1], &lo[2], &lo[3]);
        struct y264_db_side16 q = y264_db_quad(&hi[1], &hi[2], &hi[3], &hi[4]);
        __m128i rows = y264_db_bs16(&p, &q, ev);
        if (!c->have_left)
            rows = _mm_and_si128(rows, _mm_set1_epi32((int)0xffffff00u));
        _mm_storeu_si128((__m128i *)(void *)&bsv[0][0],
                         _mm_shuffle_epi8(rows, Y264_DB_TRV));
    }
    {
        const __m256i eh = _mm256_setr_epi16(-1, -1, -1, -1, 0, 0, 0, 0,
                                             0, 0, 0, 0, 0, 0, 0, 0);
        struct y264_db_side16 p = y264_db_quad(&hi[0], &hi[1], &hi[2], &hi[3]);
        struct y264_db_side16 q = y264_db_quad(&hi[1], &hi[2], &hi[3], &hi[4]);
        __m128i r = y264_db_bs16(&p, &q, eh);
        if (!c->have_top)
            r = _mm_and_si128(r, _mm_set_epi32(-1, -1, -1, 0));
        _mm_storeu_si128((__m128i *)(void *)&bsh[0][0], r);
    }
}

#endif /* Y264_HAVE_AVX2 */
