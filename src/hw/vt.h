/* Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause */
#ifndef Y264_HW_VT_H
#define Y264_HW_VT_H
/* The hardware mode: Apple's fixed-function H.264 encoder behind the yah264
 * option surface (docs/videotoolbox-plan.md). The output is the hardware's
 * stream, not ours; none of the conformance, recon or determinism gates apply
 * to it. Off-Apple builds (or -Dvideotoolbox=disabled) compile vt_stub.c, whose
 * open returns NULL with a reason, so callers carry no ifdefs. */
#include "yah264.h"
#include <stddef.h>

struct y264_hw;

/* Try to open a hardware session for `param`. Returns NULL and writes a short
 * reason into `why` when the hardware is unavailable, refuses the format, or
 * the build has no VideoToolbox. Software VideoToolbox is never accepted. */
struct y264_hw *y264_hw_open(const yah264_param_t *param, char *why, size_t whylen);
int  y264_hw_headers(struct y264_hw *h, yah264_nal_t **nal, int *count);
/* pic == NULL flushes; returns bytes, or -1. Output NALs are Annex B, in the
 * hardware's decode order, owned by the session until the next call. */
int  y264_hw_encode(struct y264_hw *h, yah264_nal_t **nal, int *count, const yah264_picture_t *pic,
                    int force_key);   /* force_key: this picture starts a new IDR (our scene-cut) */
void y264_hw_close(struct y264_hw *h);
const char *y264_hw_name(const struct y264_hw *h);
/* The warnings block for options the hardware does not honour: one line per
 * entry, built from what the CLI saw the user set (docs/videotoolbox-plan.md
 * step 4). The library only knows the parameters; the CLI knows which were
 * set explicitly, so it passes the names. */
#endif
