/*
 * deblock_sse4.c - SSE4.2 kernels for the deblock family (ITU-T H.264 8.7)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Wave 3b of docs/x86-plan.md. Four kernels: the two luma edge shapes, the
 * eight-line chroma horizontal edge, and the whole-macroblock strength
 * derivation. The filters themselves live in deblock_x86.h, written once at
 * 128 bits and shared with the AVX2 tier, because a four-line luma segment and
 * an eight-line chroma edge have nothing for a wider register to hold. Only
 * the strength derivation widens, and its AVX2 twin is in deblock_avx2.c.
 *
 * What is NOT here, and why:
 *
 *   - the VERTICAL chroma edge. It is eight rows of four bytes, so a kernel
 *     has to gather and scatter across the stride, and the chroma filter only
 *     touches p0 and q0 -- too little arithmetic to amortize a transpose. The
 *     NEON twin measured 0.86x and 0.87x for the two forms of that and refused
 *     it; the x86 idiom is the same idiom (four-byte loads, inserts, extracts,
 *     four-byte stores), so the refusal carries over rather than being
 *     re-argued. docs/dsp-coverage-inventory.md carries the row.
 *   - 4:4:4 chroma, which takes the LUMA-style filter on chroma planes and is
 *     therefore the scalar path's business either way.
 *   - the sixteen-line whole-edge form. The scalar loop feeds four lines at a
 *     time because that is the unit one bS value covers, and a kernel taking
 *     sixteen would have to carry four parameter sets through its lanes. The
 *     inventory names it as the next shape, not this wave's.
 *
 * C11 intrinsics only.
 */
#include "dsp/arch.h"

#if Y264_HAVE_SSE4

#include "dsp/x86/deblock_x86.h"

void y264_deblock_luma_v4_sse4(pixel *q0p, int stride, int bs, int alpha,
                               int beta, int tc0)
{
    y264_db_luma_v4_x86(q0p, stride, bs, alpha, beta, tc0);
}

void y264_deblock_luma_h4_sse4(pixel *q0p, int stride, int bs, int alpha,
                               int beta, int tc0)
{
    y264_db_luma_h4_x86(q0p, stride, bs, alpha, beta, tc0);
}

void y264_deblock_chroma8_h_sse4(pixel *q0p, int stride, int alpha, int beta,
                                 const uint8_t bs[4], const uint8_t tc0tab[3],
                                 int span, int g)
{
    y264_db_chroma8_h_x86(q0p, stride, alpha, beta, bs, tc0tab, span, g);
}

/* Sixteen vertical and sixteen horizontal strengths out of one pass over a 5x5
 * window of the motion and coefficient grids. Every test in 8.7.2.1 is a
 * lane-wise compare, so an axis is two eight-lane passes here -- each pass two
 * macroblock rows of four edges -- and each pass narrows straight into one
 * store.
 *
 * The kernel bakes the FRAME rules in (bS 4 on every macroblock edge, motion
 * threshold 4 on both axes) and compares refIdx lanes directly, so the caller
 * keeps a field picture and a `--weightp 2` duplicate on the C reference. Both
 * paths produce the same strengths; that is what the checkasm row covers, and
 * the dispatcher only chooses which one runs. */
void y264_deblock_strength_sse4(const struct y264_bs_ctx *c,
                                uint8_t bsv[4][4], uint8_t bsh[4][4])
{
    const int8_t *nnz = c->nnz;
    const int ns = c->nnz_stride;

    /* Columns 0..3 of rows -1..3; hi[0] is the top neighbour's row. */
    __m128i co[5];
    for (int y = 0; y < 4; y++)
        co[y + 1] = y264_db_co_row(nnz, ns, y, c->tr8_cur);
    co[0] = c->have_top ? y264_db_co_row(nnz, ns, -1, c->tr8_top) : co[1];

    struct y264_db_side hi[5];
    hi[0] = y264_db_load4(c, c->have_top ? -1 : 0, 0, co[0]);
    for (int y = 0; y < 4; y++)
        hi[y + 1] = y264_db_load4(c, y, 0, co[y + 1]);

    /* Vertical edges: the p side is the same row shifted one column left, and
     * its coefficient lane comes from the LEFT macroblock's fold. With no left
     * macroblock that column is outside the frame and must not be read, so the
     * p side is rotated out of the row itself -- lane 0 then holds column 3's
     * value, and the xb == 0 edge it feeds is one the caller does not filter
     * and is zeroed below. */
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
        const __m128i ev = _mm_setr_epi16(-1, 0, 0, 0, -1, 0, 0, 0);
        struct y264_db_side p = y264_db_pair(lo[0], lo[1]);
        struct y264_db_side q = y264_db_pair(hi[1], hi[2]);
        __m128i r01 = y264_db_bs8(&p, &q, ev);
        p = y264_db_pair(lo[2], lo[3]);
        q = y264_db_pair(hi[3], hi[4]);
        __m128i r23 = y264_db_bs8(&p, &q, ev);

        __m128i rows = _mm_unpacklo_epi64(r01, r23);
        if (!c->have_left)
            rows = _mm_and_si128(rows, _mm_set1_epi32((int)0xffffff00u));
        _mm_storeu_si128((__m128i *)(void *)&bsv[0][0],
                         _mm_shuffle_epi8(rows, Y264_DB_TRV));
    }

    /* Horizontal edges: the p side is the row above, so no shift is needed. */
    {
        const __m128i eh = _mm_setr_epi16(-1, -1, -1, -1, 0, 0, 0, 0);
        struct y264_db_side p = y264_db_pair(hi[0], hi[1]);
        struct y264_db_side q = y264_db_pair(hi[1], hi[2]);
        __m128i r = y264_db_bs8(&p, &q, eh);
        if (!c->have_top)
            r = _mm_and_si128(r, _mm_set_epi32(0, 0, -1, 0));
        _mm_storel_epi64((__m128i *)(void *)&bsh[0][0], r);

        p = y264_db_pair(hi[2], hi[3]);
        q = y264_db_pair(hi[3], hi[4]);
        _mm_storel_epi64((__m128i *)(void *)&bsh[2][0],
                         y264_db_bs8(&p, &q, _mm_setzero_si128()));
    }
}

#endif /* Y264_HAVE_SSE4 */
