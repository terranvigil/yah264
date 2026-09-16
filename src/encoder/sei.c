/*
 * sei.c - access unit delimiter and SEI message serialization (ITU-T H.264
 * clause 7.3.2.4 and Annex D)
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Each message is built into a small byte buffer first and then wrapped by
 * y264_sei_write, because an SEI's payloadSize is a byte count that has to be
 * known before the payload is emitted. The messages here are all short enough
 * that a stack buffer is the whole story; the one variable-length message this
 * encoder writes, the settings user_data_unregistered, stays in encoder.c
 * where the settings string lives.
 */
#include "sei.h"

#include <string.h>

void y264_aud_write(y264_bs_t *bs, int primary_pic_type)
{
    y264_bs_write(bs, 3, (uint32_t)(primary_pic_type & 7));
    y264_bs_rbsp_trailing(bs);
}

void y264_sei_write(y264_bs_t *bs, int payload_type,
                    const uint8_t *payload, size_t size)
{
    int t = payload_type;
    while (t >= 255) { y264_bs_write(bs, 8, 0xFF); t -= 255; }
    y264_bs_write(bs, 8, (uint32_t)t);
    size_t n = size;
    while (n >= 255) { y264_bs_write(bs, 8, 0xFF); n -= 255; }
    y264_bs_write(bs, 8, (uint32_t)n);
    for (size_t i = 0; i < size; i++)
        y264_bs_write(bs, 8, payload[i]);
    y264_bs_rbsp_trailing(bs);
}

/* The message writers below all return a byte count and all take the same
 * shape, so the wrapper never has to know which one it is holding. Each
 * pads to a byte boundary with the sei_payload alignment bits (D.1: a 1 then
 * zeroes), which is what makes payloadSize a whole number of bytes. */
static size_t sei_finish(y264_bs_t *bs)
{
    if (!y264_bs_aligned(bs)) {
        y264_bs_write1(bs, 1);                   /* bit_equal_to_one */
        while (!y264_bs_aligned(bs))
            y264_bs_write1(bs, 0);               /* bit_equal_to_zero */
    }
    y264_bs_flush(bs);
    return (size_t)(bs->p - bs->start);
}

size_t y264_sei_pic_timing(uint8_t *buf, size_t cap, int pic_struct)
{
    y264_bs_t bs;
    y264_bs_init(&bs, buf, cap);
    /* No CPB/DPB removal delays: those are present only when the VUI carries
 * HRD parameters, and this encoder writes none (--nal-hrd is a later
 * item). pic_struct_present_flag is what gates the rest. */
    y264_bs_write(&bs, 4, (uint32_t)(pic_struct & 15));
    /* One clock timestamp slot per Table D-1 for pic_struct 0..2, three for
 * the doubling/tripling values. Every one of them is written as absent,
 * which is the cheapest legal spelling and all a display needs from
 * pic_struct alone. */
    int n = pic_struct <= 2 ? 1 : pic_struct <= 4 ? 2 : pic_struct <= 6 ? 3
          : pic_struct == 7 ? 2 : pic_struct == 8 ? 3 : 1;
    for (int i = 0; i < n; i++)
        y264_bs_write1(&bs, 0);                  /* clock_timestamp_flag */
    return sei_finish(&bs);
}

size_t y264_sei_frame_packing(uint8_t *buf, size_t cap, int type)
{
    y264_bs_t bs;
    y264_bs_init(&bs, buf, cap);
    y264_bs_write_ue(&bs, 0);                    /* frame_packing_arrangement_id */
    y264_bs_write1(&bs, 0);                      /* ..._cancel_flag */
    y264_bs_write(&bs, 7, (uint32_t)(type & 0x7f));
    y264_bs_write1(&bs, 0);                      /* quincunx_sampling_flag */
    /* content_interpretation_type 1: frame 0 is the left/first view, which is
 * the ordering every packed stream this encoder can be handed already has,
 * since nothing here re-orders views. */
    y264_bs_write(&bs, 6, 1);
    y264_bs_write1(&bs, 0);                      /* spatial_flipping_flag */
    y264_bs_write1(&bs, 0);                      /* frame0_flipped_flag */
    y264_bs_write1(&bs, 0);                      /* field_views_flag */
    y264_bs_write1(&bs, 0);                      /* current_frame_is_frame0_flag */
    y264_bs_write1(&bs, 0);                      /* frame0_self_contained_flag */
    y264_bs_write1(&bs, 0);                      /* frame1_self_contained_flag */
    if (type != 5) {                             /* not frame alternation */
        y264_bs_write(&bs, 4, 0);                /* frame0_grid_position_x */
        y264_bs_write(&bs, 4, 0);                /* frame0_grid_position_y */
        y264_bs_write(&bs, 4, 0);                /* frame1_grid_position_x */
        y264_bs_write(&bs, 4, 0);                /* frame1_grid_position_y */
    }
    y264_bs_write(&bs, 8, 0);                    /* ..._reserved_byte */
    /* 0 = the arrangement persists until cancelled, which is what "this whole
 * stream is packed this way" means. */
    y264_bs_write_ue(&bs, 0);                    /* ..._repetition_period */
    y264_bs_write1(&bs, 0);                      /* ..._extension_flag */
    return sei_finish(&bs);
}

size_t y264_sei_cll(uint8_t *buf, size_t cap, int max_cll, int max_fall)
{
    y264_bs_t bs;
    y264_bs_init(&bs, buf, cap);
    y264_bs_write(&bs, 16, (uint32_t)(max_cll & 0xffff));
    y264_bs_write(&bs, 16, (uint32_t)(max_fall & 0xffff));
    return sei_finish(&bs);
}

size_t y264_sei_mastering(uint8_t *buf, size_t cap, const unsigned *prim,
                          const unsigned *wp, unsigned max_lum, unsigned min_lum)
{
    y264_bs_t bs;
    y264_bs_init(&bs, buf, cap);
    /* G, B, R -- the spec's order, which is NOT the R, G, B a person writes
 * the primaries in. The caller reorders; this writes what it is given. */
    for (int i = 0; i < 6; i++)
        y264_bs_write(&bs, 16, (uint32_t)(prim[i] & 0xffff));
    y264_bs_write(&bs, 16, (uint32_t)(wp[0] & 0xffff));
    y264_bs_write(&bs, 16, (uint32_t)(wp[1] & 0xffff));
    y264_bs_write(&bs, 32, max_lum);
    y264_bs_write(&bs, 32, min_lum);
    return sei_finish(&bs);
}

size_t y264_sei_alt_transfer(uint8_t *buf, size_t cap, int transfer)
{
    y264_bs_t bs;
    y264_bs_init(&bs, buf, cap);
    y264_bs_write(&bs, 8, (uint32_t)(transfer & 0xff));
    return sei_finish(&bs);
}
