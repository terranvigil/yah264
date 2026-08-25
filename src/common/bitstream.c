/*
 * bitstream.c - MSB-first bit writer
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "bitstream.h"

void n264_bs_init(n264_bs_t *bs, uint8_t *buf, size_t size)
{
    bs->start = buf;
    bs->p = buf;
    bs->end = buf + size;
    bs->cur = 0;
    bs->bits = 0;
    bs->overflow = 0;
    bs->count_only = 0;
}

void n264_bs_init_count(n264_bs_t *bs)
{
    n264_bs_init(bs, NULL, 0);
    bs->count_only = 1;
}

/* Push whole buffered bytes out of the accumulator into the byte stream. */
static void flush_bytes(n264_bs_t *bs)
{
    while (bs->bits >= 8) {
        bs->bits -= 8;
        uint8_t byte = (uint8_t)(bs->cur >> bs->bits);
        if (bs->p < bs->end)
            *bs->p++ = byte;
        else
            bs->overflow = 1;
    }
    /* Keep only the still-buffered low bits so `cur` cannot grow without bound. */
    if (bs->bits > 0)
        bs->cur &= ((uint64_t)1 << bs->bits) - 1;
    else
        bs->cur = 0;
}

void n264_bs_write(n264_bs_t *bs, int count, uint32_t value)
{
    if (count <= 0)
        return;
    if (count > 32)
        count = 32;
    if (bs->count_only) { bs->bits += count; return; }
    /* Mask to `count` bits so stray high bits never corrupt the stream. */
    uint32_t masked = (count == 32) ? value : (value & (((uint32_t)1 << count) - 1));
    bs->cur = (bs->cur << count) | masked;
    bs->bits += count;
    flush_bytes(bs);
}

void n264_bs_write1(n264_bs_t *bs, int bit)
{
    if (bs->count_only) { bs->bits += 1; return; }
    bs->cur = (bs->cur << 1) | (uint32_t)(bit & 1);
    bs->bits += 1;
    flush_bytes(bs);
}

void n264_bs_write_zero(n264_bs_t *bs, int count)
{
    if (bs->count_only) { if (count > 0) bs->bits += count; return; }
    while (count >= 32) {
        n264_bs_write(bs, 32, 0);
        count -= 32;
    }
    if (count > 0)
        n264_bs_write(bs, count, 0);
}

void n264_bs_write_ue(n264_bs_t *bs, uint32_t value)
{
    /* codeNum = value; write floor(log2(value+1)) leading zeros, then value+1. */
    uint32_t x = value + 1;              /* value < UINT32_MAX in all our uses */
    int n = 31 - __builtin_clz(x);       /* x >= 1, so clz is defined */
    if (bs->count_only) { bs->bits += 2 * n + 1; return; }
    n264_bs_write_zero(bs, n);
    n264_bs_write(bs, n + 1, x);
}

void n264_bs_write_se(n264_bs_t *bs, int32_t value)
{
    /* Map signed to unsigned codeNum: 0,-> 0; 1,-1,2,-2 -> 1,2,3,4 ... */
    uint32_t code = (value > 0) ? (uint32_t)(2 * value - 1)
                                : (uint32_t)(-2 * value);
    n264_bs_write_ue(bs, code);
}

void n264_bs_flush(n264_bs_t *bs)
{
    if (bs->bits > 0) {
        int pad = 8 - bs->bits;
        bs->cur <<= pad;
        bs->bits += pad;
        flush_bytes(bs);
    }
}

void n264_bs_rbsp_trailing(n264_bs_t *bs)
{
    n264_bs_write1(bs, 1);   /* rbsp_stop_one_bit */
    while (bs->bits != 0)    /* rbsp_alignment_zero_bit(s) */
        n264_bs_write1(bs, 0);
}
