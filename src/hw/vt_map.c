/* Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause */
#include "vt_map.h"
#include <string.h>
#include <stddef.h>

static const struct y264_hw_opt table[] = {
    { "--level",            Y264_HW_MAPS,    "ProfileLevel" },
    { "--cabac",            Y264_HW_MAPS,    "entropy mode" },
    { "--cavlc",            Y264_HW_MAPS,    "entropy mode" },
    { "--keyint",           Y264_HW_MAPS,    "max keyframe interval" },
    { "--bitrate",          Y264_HW_MAPS,    "average bitrate" },
    { "--vbv-maxrate",      Y264_HW_APPROX,  "data-rate limit over a window of bufsize / maxrate" },
    { "--vbv-bufsize",      Y264_HW_APPROX,  "data-rate limit over a window of bufsize / maxrate" },
    { "--crf",              Y264_HW_APPROX,  "the hardware's quality key through a rate-matched table, not our scale" },
    { "--qp",               Y264_HW_APPROX,  "pins the frame QP range; the hardware may still move QP inside a frame" },
    { "--bframes",          Y264_HW_APPROX,  "frame reordering on or off; the count is the hardware's" },
    { "--scenecut",         Y264_HW_OURS,    "our scene-cut forces the hardware's keyframes" },
    { "--no-scenecut",      Y264_HW_OURS,    "turns that off" },
    { "--sar",              Y264_HW_MAPS,    "pixel aspect ratio" },
    { "--min-keyint",       Y264_HW_IGNORED, NULL },
    { "--b-adapt",          Y264_HW_IGNORED, NULL },
    { "--direct",           Y264_HW_IGNORED, NULL },
    { "--ref",              Y264_HW_IGNORED, "reference count is the hardware's" },
    { "--pass",             Y264_HW_IGNORED, "no two-pass on the hardware" },
    { "--stats",            Y264_HW_IGNORED, "no two-pass on the hardware" },
    { "--abr-model",        Y264_HW_IGNORED, NULL },
    { "--qcomp",            Y264_HW_IGNORED, NULL },
    { "--rc-lookahead",     Y264_HW_IGNORED, NULL },
    { "--sync-lookahead",   Y264_HW_IGNORED, NULL },
    { "--preset",           Y264_HW_IGNORED, "the hardware has no presets" },
    { "--tune",             Y264_HW_IGNORED, "zerolatency excepted in a later step" },
    { "--threads",          Y264_HW_IGNORED, "the hardware has its own parallelism" },
    { "--aq-strength",      Y264_HW_IGNORED, NULL },
    { "--psy-rd",           Y264_HW_IGNORED, NULL },
    { "--psy-trellis",      Y264_HW_IGNORED, NULL },
    { "--trellis",          Y264_HW_IGNORED, NULL },
    { "--deadzone-inter",   Y264_HW_IGNORED, NULL },
    { "--deadzone-intra",   Y264_HW_IGNORED, NULL },
    { "--cqm",              Y264_HW_IGNORED, NULL },
    { "--me",               Y264_HW_IGNORED, NULL },
    { "--merange",          Y264_HW_IGNORED, NULL },
    { "--subme",            Y264_HW_IGNORED, NULL },
    { "--subpel",           Y264_HW_IGNORED, NULL },
    { "--transform-8x8",    Y264_HW_IGNORED, "implied by the profile" },
    { "--no-transform-8x8", Y264_HW_IGNORED, "implied by the profile" },
    { "--no-sei",           Y264_HW_MAPS,    "the hardware writes none of ours" },
    { "--dump-recon",       Y264_HW_FATAL,   "no reconstruction to dump" },
    { NULL, Y264_HW_MAPS, NULL }
};

enum y264_hw_class y264_hw_option_class(const char *name, const char **note)
{
    for (const struct y264_hw_opt *o = table; o->name; o++)
        if (!strcmp(o->name, name)) { if (note) *note = o->note; return o->cls; }
    if (note) *note = NULL;
    return Y264_HW_MAPS;
}

const struct y264_hw_opt *y264_hw_option_table(void) { return table; }
