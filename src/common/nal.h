/*
 * nal.h - NAL unit packaging (Annex-B start codes + emulation prevention)
 * Copyright (c) 2026, the next264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef NEXT264_NAL_H
#define NEXT264_NAL_H

#include <stdint.h>
#include <stddef.h>

/* Write one Annex-B NAL unit into `out` from the raw RBSP payload `rbsp`.
 *
 * Emits: 00 00 00 01 (4-byte start code)
 * 1-byte NAL header (forbidden_zero_bit=0, nal_ref_idc, nal_unit_type)
 * the RBSP with emulation-prevention 0x03 bytes inserted.
 *
 * `out_size` is the capacity of `out`. Returns the number of bytes written, or
 * 0 if the output buffer is too small.
 *
 * Worst-case output size for an n-byte RBSP is 5 + n + n/2 + 1; callers should
 * size `out` at least that large.
 */
size_t n264_nal_write(uint8_t *out, size_t out_size,
                      int nal_ref_idc, int nal_unit_type,
                      const uint8_t *rbsp, size_t rbsp_size);

#endif /* NEXT264_NAL_H */
