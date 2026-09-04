/* Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause */
#include "vt.h"
#include <stdio.h>

struct y264_hw *y264_hw_open(const yah264_param_t *param, char *why, size_t whylen)
{
    (void)param;
    if (why && whylen) snprintf(why, whylen, "this build has no VideoToolbox (not macOS, or -Dvideotoolbox=disabled)");
    return NULL;
}
int y264_hw_headers(struct y264_hw *h, yah264_nal_t **nal, int *count) { (void)h; (void)nal; (void)count; return -1; }
int y264_hw_encode(struct y264_hw *h, yah264_nal_t **nal, int *count, const yah264_picture_t *pic, int force_key)
{ (void)h; (void)nal; (void)count; (void)pic; (void)force_key; return -1; }
void y264_hw_close(struct y264_hw *h) { (void)h; }
const char *y264_hw_name(const struct y264_hw *h) { (void)h; return "none"; }
