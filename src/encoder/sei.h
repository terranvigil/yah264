/*
 * sei.h - access unit delimiter and SEI message serialization
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef YAH264_SEI_H
#define YAH264_SEI_H

#include "../common/bitstream.h"

/* Every writer here fills an RBSP: no NAL header, no start code and no
 * emulation prevention, which the caller's NAL writer adds. Each one ends with
 * rbsp_trailing_bits, so `bs->p - bs->start` after the call is the payload the
 * caller hands to append_nal. */

/* Access unit delimiter (NAL type 9, 7.3.2.4). primary_pic_type is Table 7-5:
 * 0 = I only, 1 = I or P, 2 = I, P or B. Values above 2 widen it further; 7
 * asserts nothing, which is what an unknown picture type gets. */
void y264_aud_write(y264_bs_t *bs, int primary_pic_type);

/* One SEI NAL carrying one message. `payload` holds `size` bytes of the
 * message's own syntax, already byte-aligned. */
void y264_sei_write(y264_bs_t *bs, int payload_type,
                    const uint8_t *payload, size_t size);

/* pic_timing (payloadType 1), in the shape this encoder's VUI permits: no
 * CPB/DPB delays (no HRD is written), pic_struct present. `pic_struct` is
 * Table D-1; 0 = a progressive frame. */
size_t y264_sei_pic_timing(uint8_t *buf, size_t cap, int pic_struct);

/* frame_packing_arrangement (payloadType 45, D.2.26). `type` is
 * frame_packing_arrangement_type (3 = side-by-side, 4 = top-bottom,
 * 5 = frame alternation, and the rest of Table D-8). Written with the
 * repetition period "until cancelled", which is what a constant arrangement
 * over a whole stream means. */
size_t y264_sei_frame_packing(uint8_t *buf, size_t cap, int type);

/* content_light_level_info (payloadType 144): MaxCLL and MaxFALL in cd/m^2. */
size_t y264_sei_cll(uint8_t *buf, size_t cap, int max_cll, int max_fall);

/* mastering_display_colour_volume (payloadType 137). `prim` is G, B, R
 * chromaticity in 0.00002 units (x then y, six values), `wp` the white point
 * in the same units, and the luminance pair in 0.0001 cd/m^2. */
size_t y264_sei_mastering(uint8_t *buf, size_t cap, const unsigned *prim,
                          const unsigned *wp, unsigned max_lum, unsigned min_lum);

/* alternative_transfer_characteristics (payloadType 147): the H.273 transfer
 * code a display should prefer over the VUI's. */
size_t y264_sei_alt_transfer(uint8_t *buf, size_t cap, int transfer);

#endif /* YAH264_SEI_H */
