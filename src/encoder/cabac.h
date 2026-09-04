/*
 * cabac.h - CABAC binary arithmetic encoder (ITU-T H.264 section 9.3)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef YAH264_CABAC_H
#define YAH264_CABAC_H

#include <stdint.h>
#include "../common/bitdepth.h"

/* Number of context models. ctxIdx 0..459 is the whole standard set for
 * 4:2:0 and 4:2:2 (all block categories, both transform sizes); 460..1023 is
 * used only by the 4:4:4 Cb/Cr categories. Arrays are the full 1024, but the
 * RD trial snapshots only have to save and restore what the format can touch,
 * which is 2.2x less on everything but 4:4:4. */
#define Y264_CABAC_CTX 1024
#define Y264_CABAC_CTX_BASE 460

/* Contexts a frame of this chroma format can touch (see above). */
static inline int y264_cabac_ctx_n(int cf_idc)
{
    return cf_idc == 3 ? Y264_CABAC_CTX : Y264_CABAC_CTX_BASE;
}

typedef struct {
    /* Byte-queue arithmetic engine (the x264 shape, proven byte-identical to
 * the spec's bit-wise PutBit/bitsOutstanding formulation): `low` holds the
 * 10-bit active interval base in its low bits plus `queue+9` settled bits
 * above, so carries into already-settled bits resolve by ordinary addition;
 * only fully-emitted 0xff bytes need deferral (`obytes`). Renormalisation
 * is a single clz shift instead of a per-bit loop. */
    uint32_t low;               /* active codILow + settled bits above bit 9 */
    uint32_t range;             /* codIRange */
    int      queue;             /* settled bits - 9 (extraction at >= 0) */
    int      obytes;            /* pending 0xff bytes awaiting a carry */

    uint8_t *start;             /* output byte buffer (into the RBSP) */
    uint8_t *p;                 /* next byte */
    uint8_t *end;               /* capacity backstop: no byte is written at or
 * past it; NULL = unbounded. Set with
 * y264_cabac_set_end after init_engine. */
    int      overflow;          /* a byte was dropped at `end`: the slice is
 * invalid and must be retried, as CAVLC's
 * bs.overflow (review 2026-09-04) */
    uint32_t nbins;             /* bins coded by the real engine (regular +
 * bypass), for cabac_zero_words (9.3.4.6) */

    /* Per-context state, packed as (pStateIdx << 1) | valMPS.
 *
 * A POINTER, not the array, and the reason is measured: the RD estimator
 * runs over a private copy of the contexts, and swapping that copy in and
 * out used to be four 460-byte memcpys per macroblock and three more per
 * priced trial. Deleting those copies outright (an unsafe ceiling probe)
 * was worth 5.0% of samsung's t1 wall. With a pointer the swap is an
 * assignment: est_commit_* points ctx straight at est_ctx, and the pricing
 * paths point it at a stack scratch they filled once.
 *
 * INVARIANT: ctx points at ctxbuf except inside such a swap, and every
 * struct copy of an y264_cabac_t must call y264_cabac_rebind -- a raw
 * copy leaves the destination sharing the SOURCE's buffer, which under the
 * wavefront is one shared context array across workers. */
    uint8_t *ctx;
    uint8_t  ctxbuf[Y264_CABAC_CTX];

    /* W0 step 6: private RD-estimator context. The RD cost trials adapt from this
 * (not the live `ctx`, which pass 2 / the real engine owns), so the analysis
 * pass is independent of the serial arithmetic engine. In step 6a it tracks
 * `ctx` exactly (byte-identical); step 6b re-inits it per row (WPP / slice). */
    uint8_t  est_ctx[Y264_CABAC_CTX];

    /* RD bit-estimate mode: when est_mode, encode_decision/bypass accumulate
 * fractional bits (x256) into est_bits over the (scratch) context states
 * instead of arithmetic-coding, so the real coders double as CABAC rate
 * estimators for mode decision. See est_decision. */
    int      est_mode;
    long     est_bits;
} y264_cabac_t;

/* Initialise the arithmetic engine to write into `buf`. */
void y264_cabac_init_engine(y264_cabac_t *c, uint8_t *buf);
void y264_cabac_set_end(y264_cabac_t *c, uint8_t *end);   /* capacity backstop (see `end`) */

/* Point ctx back at this engine's own buffer. Required after every struct copy
 * (see the ctx comment); init_engine calls it for you. */
static inline void y264_cabac_rebind(y264_cabac_t *c) { c->ctx = c->ctxbuf; }

/* Build the RDOQ bit-estimation table now, on the calling thread. Called at
 * encoder open so worker threads never build it concurrently. Idempotent. */
void y264_cabac_warm(void);

/* Initialise all context models for a slice (slice_type: 0=I,1=P,2=B;
 * cabac_init_idc 0..2, ignored for I). */
void y264_cabac_init_contexts(y264_cabac_t *c, int slice_type, int cabac_init_idc, int qp);

/* Code one regular bin with context `ctxIdx`. */
void y264_cabac_encode_decision(y264_cabac_t *c, int ctxIdx, int bin);

/* Code one bypass (equiprobable) bin. */
void y264_cabac_encode_bypass(y264_cabac_t *c, int bin);

/* Code the end_of_slice / termination bin; bin==1 flushes the engine. */
void y264_cabac_encode_terminate(y264_cabac_t *c, int bin);

/* Exp-Golomb order-k value in bypass (level tails, mvd tails). */
void y264_cabac_encode_ueg_bypass(y264_cabac_t *c, int k, int val);

/* Code a residual block: coded_block_flag + significance map + levels. `cat` is
 * the ctxBlockCat (0=LumaDC,1=LumaAC,2=Luma4x4,3=ChromaDC,4=ChromaAC), `l` holds
 * scan-order coefficients, `nza`/`nzb` the neighbour coded_block_flag terms.
 * Returns the coefficient count (0 = all zero). */
int y264_cabac_residual(y264_cabac_t *c, int cat, const dctcoef *l, int nza, int nzb);

/* Code an 8x8 luma residual (ctxBlockCat 5): no coded_block_flag, 64 scan-order
 * coefficients. Returns the coefficient count. */
int y264_cabac_residual_8x8(y264_cabac_t *c, const dctcoef *l);
/* est_mode only: same estimate priced straight from the RASTER-order block,
 * fusing the caller's zigzag gather into the walk (bit-exact, faster). */
int y264_cabac_residual_8x8_est(y264_cabac_t *c, const dctcoef *lr);
/* est_mode only: one mvd component's bins (UEG3 prefix + suffix + sign) in a
 * single call, bit-exact with the per-bin emit path. */
void y264_cabac_est_mvd(y264_cabac_t *c, int ctxbase, int ctx, int mvd);
/* RDOQ cost estimates (bits x256) from the engine's current context states. */
long y264_cabac_residual_bits(const y264_cabac_t *c, int cat, const dctcoef *l,
                              int nza, int nzb);
long y264_cabac_residual_8x8_bits(const y264_cabac_t *c, const dctcoef *l);

/* Single-forward-Viterbi RDOQ for one 4x4-family block (cat 1/2/4). See cabac.c
 * for the per-scan-position input contract; writes final abs levels to absout. */
void y264_cabac_trellis_4x4(const y264_cabac_t *c, int cat, int nza, int nzb,
                            long lambda, int n, const int *qn, const int *abscoef,
                            const long *unmf, const int *w2,
                            int psy256, const int *psyp, int psy_lo, int *absout);
/* Single-forward-Viterbi RDOQ for one 8x8 luma block (ctxBlockCat 5). */
void y264_cabac_trellis_8x8(const y264_cabac_t *c, long lambda, const int *qn,
                            const int *abscoef, const long *unmf, const int *w2,
                            int psy256, const int *psyp, int psy_lo, int *absout);

/* Bit position (diagnostics only): settled bits produced so far, including
 * bytes still pending carry resolution. */
int y264_cabac_pos_bits(const y264_cabac_t *c);

/* Number of bytes written after a terminate(1) flush. */
int y264_cabac_bytes(const y264_cabac_t *c);

/* --- decode engine (9.3.3.2), for testing/verification --- */
typedef struct {
    const uint8_t *buf;
    int      bitpos;
    uint32_t range, offset;
    uint8_t  ctx[Y264_CABAC_CTX];
} y264_cabac_dec_t;

void y264_cabac_dec_init(y264_cabac_dec_t *d, const uint8_t *buf);
void y264_cabac_dec_contexts(y264_cabac_dec_t *d, int slice_type, int cabac_init_idc, int qp);
int  y264_cabac_dec_decision(y264_cabac_dec_t *d, int ctxIdx);
int  y264_cabac_dec_bypass(y264_cabac_dec_t *d);
int  y264_cabac_dec_terminate(y264_cabac_dec_t *d);

#endif /* YAH264_CABAC_H */
