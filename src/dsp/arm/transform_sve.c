/*
 * transform_sve.c - SVE forward quant (docs/simd.md)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The flat-matrix forward quant in the coefficient's own 16-bit lanes, a 4x4
 * block to a 256-bit register. SVE has no widening multiply before SVE2, and
 * the 256-bit part is SVE only, so the 32-bit product |c| * mf is kept as its
 * two halves (MUL and UMULH) and the bias added to the pair with the carry out
 * of the low half carried by hand:
 *
 *   (|c| * mf + f) >> 16 == hi + (f >> 16) + carry(lo + (f & 0xffff))
 *
 * which is exact wherever the NEON form is (|c| <= 32768, mf < 65536,
 * f < 2^24, so the sum's high half stays under 2^15). The 4x4 quantiser
 * shifts by 15 + qp/6, so it runs on 2 * mf and 2 * f as the NEON twin does.
 *
 * Every kernel is exact at any vector length; the dispatch sites in
 * transform.c take them on Y264_CPU_SVE256 only. The feature is selected for
 * this file alone by the target pragma.
 */
#include <stddef.h>
#include <stdint.h>

#include "../arch.h"

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("sve"))), apply_to = function)
#else
#pragma GCC target("+sve")
#endif
#include <arm_sve.h>

static inline svint16_t quant16(svbool_t pt, svint16_t c, svuint16_t mf,
                                uint16_t flo, uint16_t fhi, uint16_t k)
{
    svuint16_t a = svreinterpret_u16_s16(svabs_s16_x(pt, c));
    svuint16_t lo = svmul_u16_x(pt, a, mf);
    svuint16_t q = svadd_n_u16_x(pt, svmulh_u16_x(pt, a, mf), fhi);
    /* lo + flo carries out of 16 bits exactly when lo > 0xffff - flo */
    q = svadd_n_u16_m(svcmpgt_n_u16(pt, lo, (uint16_t)~flo), q, 1);
    svint16_t qs = svreinterpret_s16_u16(svlsr_n_u16_x(pt, q, k));
    return svneg_s16_m(qs, svcmplt_n_s16(pt, c, 0), qs);
}

/* Multiplier entries i.. of an int32 row as u16 (every flat-matrix multiplier
 * fits), doubled for the 4x4 shift: a two-way structure load whose first
 * register is the even halfwords, the entries' low halves. */
static inline svuint16_t mf_u16(const int32_t *m, int i, int n, int dbl)
{
    svuint16_t v = svget2_u16(svld2_u16(svwhilelt_b16_s32(i, n),
                                        (const uint16_t *)(m + i)), 0);
    return dbl ? svlsl_n_u16_x(svptrue_b16(), v, 1) : v;
}

/* n consecutive 4x4 blocks. One register holds a block at 256 bits; narrower
 * vectors take a block in chunks. */
void y264_quant_4x4_f_n_sve(const int16_t *coef, int16_t *lev, int n, int qp,
                            int f, const int32_t mfrow[16])
{
    const svbool_t pt = svptrue_b16();
    const uint32_t f2 = 2u * (uint32_t)f;
    const uint16_t flo = (uint16_t)f2, fhi = (uint16_t)(f2 >> 16), k = (uint16_t)(qp / 6);
    if (svcnth() >= 16) {
        const svbool_t pg = svwhilelt_b16_s32(0, 16);
        const svuint16_t mf = mf_u16(mfrow, 0, 16, 1);
        for (int b = 0; b < n; b++, coef += 16, lev += 16)
            svst1_s16(pg, lev, quant16(pt, svld1_s16(pg, coef), mf, flo, fhi, k));
        return;
    }
    for (int b = 0; b < n; b++, coef += 16, lev += 16)
        for (int i = 0; i < 16; i += (int)svcnth()) {
            const svbool_t pg = svwhilelt_b16_s32(i, 16);
            svst1_s16(pg, lev + i, quant16(pt, svld1_s16(pg, coef + i),
                                           mf_u16(mfrow, i, 16, 1), flo, fhi, k));
        }
}

void y264_quant_4x4_f_sve(const int16_t coef[16], int16_t lev[16], int qp, int f,
                          const int32_t mfrow[16])
{
    y264_quant_4x4_f_n_sve(coef, lev, 1, qp, f, mfrow);
}

/* n inter 4x4 AC blocks: the quant above, then each block's DC coefficient to
 * dc[k] with its level zeroed and its nonzero AC count to nz[k]. Returns
 * nonzero if any block has an AC level. The 256-bit form only; narrower
 * vectors run the chunked quant and count in scalar. */
int y264_quant_4x4_ac_f_sve(const int16_t *coef, int16_t *lev, int n, int qp,
                            int f, const int32_t mfrow[16], int16_t *dc, int *nz)
{
    const svbool_t pt = svptrue_b16();
    int any = 0;
    if (svcnth() >= 16) {
        const uint32_t f2 = 2u * (uint32_t)f;
        const uint16_t flo = (uint16_t)f2, fhi = (uint16_t)(f2 >> 16), k = (uint16_t)(qp / 6);
        const svbool_t pg = svwhilelt_b16_s32(0, 16);
        const svbool_t pac = svbic_b_z(pt, pg, svwhilelt_b16_s32(0, 1));
        const svuint16_t mf = mf_u16(mfrow, 0, 16, 1);
        for (int b = 0; b < n; b++, coef += 16, lev += 16) {
            svint16_t l = quant16(pt, svld1_s16(pg, coef), mf, flo, fhi, k);
            dc[b] = coef[0];
            /* the AC lanes only: the DC level is stored as zero */
            l = svsel_s16(pac, l, svdup_n_s16(0));
            svst1_s16(pg, lev, l);
            int c = (int)svcntp_b16(pac, svcmpne_n_s16(pac, l, 0));
            nz[b] = c;
            any |= c;
        }
        return any;
    }
    y264_quant_4x4_f_n_sve(coef, lev, n, qp, f, mfrow);
    for (int b = 0; b < n; b++, coef += 16, lev += 16) {
        dc[b] = coef[0];
        lev[0] = 0;
        int c = 0;
        for (int i = 1; i < 16; i++)
            c += lev[i] != 0;
        nz[b] = c;
        any |= c;
    }
    return any;
}

/* n consecutive 8x8 blocks: four registers a block at 256 bits. */
void y264_quant_8x8_f_n_sve(const int16_t *coef, int16_t *lev, int n, int qp,
                            int f, const int32_t mfrow[64])
{
    const svbool_t pt = svptrue_b16();
    const uint16_t flo = (uint16_t)f, fhi = (uint16_t)((uint32_t)f >> 16), k = (uint16_t)(qp / 6);
    if (svcnth() >= 16) {
        const svbool_t pg = svwhilelt_b16_s32(0, 16);
        const svuint16_t m0 = mf_u16(mfrow, 0, 64, 0), m1 = mf_u16(mfrow, 16, 64, 0);
        const svuint16_t m2 = mf_u16(mfrow, 32, 64, 0), m3 = mf_u16(mfrow, 48, 64, 0);
        for (int b = 0; b < n; b++, coef += 64, lev += 64) {
            svst1_s16(pg, lev,      quant16(pt, svld1_s16(pg, coef),      m0, flo, fhi, k));
            svst1_s16(pg, lev + 16, quant16(pt, svld1_s16(pg, coef + 16), m1, flo, fhi, k));
            svst1_s16(pg, lev + 32, quant16(pt, svld1_s16(pg, coef + 32), m2, flo, fhi, k));
            svst1_s16(pg, lev + 48, quant16(pt, svld1_s16(pg, coef + 48), m3, flo, fhi, k));
        }
        return;
    }
    for (int b = 0; b < n; b++, coef += 64, lev += 64)
        for (int i = 0; i < 64; i += (int)svcnth()) {
            const svbool_t pg = svwhilelt_b16_s32(i, 64);
            svst1_s16(pg, lev + i, quant16(pt, svld1_s16(pg, coef + i),
                                           mf_u16(mfrow, i, 64, 0), flo, fhi, k));
        }
}

void y264_quant_8x8_f_sve(const int16_t coef[64], int16_t lev[64], int qp, int f,
                          const int32_t mfrow[64])
{
    y264_quant_8x8_f_n_sve(coef, lev, 1, qp, f, mfrow);
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
