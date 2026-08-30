/*
 * nal.c - NAL unit packaging
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "nal.h"

size_t y264_nal_write(uint8_t *out, size_t out_size,
                      int nal_ref_idc, int nal_unit_type,
                      const uint8_t *rbsp, size_t rbsp_size)
{
    /* Conservative capacity check: start code (4) + header (1) + every RBSP byte
 * plus a possible emulation byte, plus one trailing guard. */
    if (out_size < 5 + rbsp_size + rbsp_size / 2 + 1)
        return 0;

    uint8_t *o = out;
    *o++ = 0x00;
    *o++ = 0x00;
    *o++ = 0x00;
    *o++ = 0x01;
    *o++ = (uint8_t)(((nal_ref_idc & 3) << 5) | (nal_unit_type & 0x1f));

    /* Emulation prevention: inside the RBSP, any 00 00 00, 00 00 01, 00 00 02,
 * or 00 00 03 sequence must have a 03 byte inserted after the two zeros so
 * decoders never mistake payload for a start code. `zeros` counts the run of
 * consecutive 0x00 bytes already written to the output. */
    int zeros = 0;
    for (size_t i = 0; i < rbsp_size; i++) {
        uint8_t b = rbsp[i];
        if (zeros >= 2 && b <= 0x03) {
            *o++ = 0x03;
            zeros = 0;
        }
        *o++ = b;
        zeros = (b == 0x00) ? zeros + 1 : 0;
    }

    return (size_t)(o - out);
}
