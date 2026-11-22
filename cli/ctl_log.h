/*
 * ctl_log.h - the live-control log grammar, shared verbatim by the yah264,
 * yah265 and yaav1 trees (the contract's section 9). It depends on nothing
 * but the C library and names no encoder: each tree converts the neutral
 * event below to its own control struct. Change the home copy and copy it;
 * never edit a tree's copy alone.
 *
 * Copyright (c) 2026, the yah264, yah265 and yaav1 authors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CTL_LOG_H
#define CTL_LOG_H

#include <stddef.h>
#include <stdint.h>

#define CTL_LOG_VERSION 1

/* the contract's kind values (section 5) */
#define CTL_KIND_RATE        1
#define CTL_KIND_CRF         2
#define CTL_KIND_QP_LIMITS   3
#define CTL_KIND_KEYFRAME    4
#define CTL_KIND_ZONE        5
#define CTL_KIND_FPS         6
#define CTL_KIND_PRESET      7
#define CTL_KIND_RESOLUTION  8

#define CTL_FRAME_NEXT (-1)

/* one log line; unset fields hold the values ctl_log_parse starts from:
 * 0 for the rates, -1 for qp_min / qp_max / last */
struct ctl_log_event {
    int     kind;
    int64_t frame;                  /* CTL_FRAME_NEXT for "next" */
    int     bitrate_kbps, vbv_maxrate_kbps, vbv_bufsize_kbit;
    double  crf;
    int     qp_min, qp_max;
    int64_t last;
    double  qp_offset;
    int     fps_num, fps_den;
    char    preset[32];
    int     width, height;
};

/* Parse one line. Returns 1 for an event (written to *ev), 0 for a blank or
 * comment line, -1 for a malformed one with the reason in err. `allow_next`
 * admits "next" as the frame (live input only). */
int ctl_log_parse(const char *line, int allow_next, struct ctl_log_event *ev, char *err, size_t errlen);

/* A "#livectl N" header: returns N, 0 when the line isn't one, -1 when it is
 * one with a bad number. */
int ctl_log_header(const char *line);

/* The event as a log line, without a newline. */
void ctl_log_format(const struct ctl_log_event *ev, char *buf, size_t n);

#endif
