/* Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause */
#ifndef Y264_HW_VT_MAP_H
#define Y264_HW_VT_MAP_H
/* The option map for the hardware mode (docs/videotoolbox-plan.md step 3), in
 * one place so the warnings, the docs and the behaviour cannot drift: what
 * each yah264 CLI option becomes on the hardware. Built on every platform
 * (it is data), consulted by the CLI's warnings block. */
enum y264_hw_class {
    Y264_HW_MAPS = 0,       /* same meaning on the hardware */
    Y264_HW_APPROX,         /* the closest thing the hardware has; warned once */
    Y264_HW_OURS,           /* ours, fed into the hardware (scene-cut) */
    Y264_HW_IGNORED,        /* no equivalent; warned once when set explicitly */
    Y264_HW_FATAL,          /* cannot encode: error in forced mode, fall back in auto */
};
struct y264_hw_opt { const char *name; enum y264_hw_class cls; const char *note; };
/* The class of a CLI option name ("--me"); Y264_HW_MAPS for anything not listed. */
enum y264_hw_class y264_hw_option_class(const char *name, const char **note);
/* The whole table, terminated by a NULL name (for docs and tests). */
const struct y264_hw_opt *y264_hw_option_table(void);
#endif
