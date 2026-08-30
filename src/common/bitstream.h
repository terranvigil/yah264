/*
 * bitstream.h - MSB-first bit writer with Exp-Golomb and RBSP helpers
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef YAH264_BITSTREAM_H
#define YAH264_BITSTREAM_H

#include <stdint.h>
#include <stddef.h>

/* A bit writer over a caller-provided byte buffer. Bits are packed MSB-first,
 * which is the order H.264 RBSP uses. Writes are bounds-checked against `end`;
 * on overflow the writer sets `overflow` and drops further output. */
typedef struct {
    uint8_t *start;
    uint8_t *p;         /* next byte to write */
    uint8_t *end;       /* one past the last usable byte */
    uint64_t cur;       /* bit accumulator, valid bits held in the low `bits` */
    int      bits;      /* number of buffered bits not yet flushed to bytes */
    int      overflow;  /* set to 1 once a write would exceed `end` */
    int      count_only;/* 1 = price only: accumulate lengths, emit nothing */
} y264_bs_t;

void y264_bs_init(y264_bs_t *bs, uint8_t *buf, size_t size);

/* A writer that only counts. The RD pricing sites write a macroblock's syntax
 * to a scratch buffer and keep nothing but y264_bs_pos_bits; in this mode the
 * accumulator/flush path is skipped and every write adds its length, which
 * y264_bs_pos_bits reports unchanged (`p` never leaves `start`). Do not
 * combine with y264_bs_flush / y264_bs_aligned / reading the buffer. */
void y264_bs_init_count(y264_bs_t *bs);

/* Number of whole bytes already emitted plus buffered bits. */
static inline size_t y264_bs_pos_bits(const y264_bs_t *bs)
{
    return (size_t)(bs->p - bs->start) * 8 + (size_t)bs->bits;
}

/* Write the low `count` bits of `value` (0 <= count <= 32). */
void y264_bs_write(y264_bs_t *bs, int count, uint32_t value);

/* Write a single bit. */
void y264_bs_write1(y264_bs_t *bs, int bit);

/* Write `count` zero bits (count >= 0). */
void y264_bs_write_zero(y264_bs_t *bs, int count);

/* Unsigned Exp-Golomb, ue(v). */
void y264_bs_write_ue(y264_bs_t *bs, uint32_t value);

/* Signed Exp-Golomb, se(v). */
void y264_bs_write_se(y264_bs_t *bs, int32_t value);

/* 1 if the writer sits on a byte boundary (no buffered bits). */
static inline int y264_bs_aligned(const y264_bs_t *bs)
{
    return bs->bits == 0;
}

/* RBSP trailing bits: a stop bit then zero padding to the next byte boundary. */
void y264_bs_rbsp_trailing(y264_bs_t *bs);

/* Flush any buffered bits to a byte boundary, zero-padding the final byte.
 * After this, (bs->p - bs->start) is the exact byte length written. */
void y264_bs_flush(y264_bs_t *bs);

#endif /* YAH264_BITSTREAM_H */
