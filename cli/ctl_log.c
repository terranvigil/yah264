/*
 * ctl_log.c - the live-control log grammar, shared verbatim by the encoder
 * trees; see ctl_log.h.
 *
 * Copyright (c) 2026, the yah264, yah265 and yaav1 authors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ctl_log.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kind_spec {
    const char *name;
    int kind;
    const char *args[4];        /* names this kind takes */
    unsigned required;          /* bit i: args[i] must be present */
    int any_of;                 /* at least one arg must be present */
};

static const struct kind_spec kinds[] = {
    { "rate",   CTL_KIND_RATE,       { "bitrate", "maxrate", "bufsize" }, 0, 1 },
    { "crf",    CTL_KIND_CRF,        { "value" }, 1, 0 },
    { "qp",     CTL_KIND_QP_LIMITS,  { "min", "max" }, 0, 1 },
    { "key",    CTL_KIND_KEYFRAME,   { 0 }, 0, 0 },
    { "zone",   CTL_KIND_ZONE,       { "last", "offset" }, 3, 0 },
    { "fps",    CTL_KIND_FPS,        { "num", "den" }, 3, 0 },
    { "preset", CTL_KIND_PRESET,     { "name" }, 1, 0 },
    { "size",   CTL_KIND_RESOLUTION, { "width", "height" }, 3, 0 },
};

static int fail(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
    return -1;
}

/* a decimal integer: an optional minus and digits */
static int parse_i64(const char *s, int64_t lo, int64_t hi, int64_t *out)
{
    const char *d = *s == '-' ? s + 1 : s;
    if (!*d) return -1;
    for (const char *p = d; *p; p++)
        if (!isdigit((unsigned char)*p)) return -1;
    errno = 0;
    char *end;
    long long v = strtoll(s, &end, 10);
    if (errno || *end || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

/* a decimal real: an optional sign, 1 to 8 digits, and an optional point
 * followed by 1 to 6 digits; no exponent, hex, inf or nan. At most 14
 * significant digits, so the writer's shortest form always reads back. */
#define CTL_REAL_INT_DIGITS  8
#define CTL_REAL_FRAC_DIGITS 6
static int parse_dbl(const char *s, double *out)
{
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    const char *d = p;
    while (isdigit((unsigned char)*p)) p++;
    if (p == d || p - d > CTL_REAL_INT_DIGITS) return -1;
    if (*p == '.') {
        d = ++p;
        while (isdigit((unsigned char)*p)) p++;
        if (p == d || p - d > CTL_REAL_FRAC_DIGITS) return -1;
    }
    if (*p) return -1;
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || *end || !isfinite(v)) return -1;
    *out = v;
    return 0;
}

int ctl_log_header(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (strncmp(line, "#livectl", 8) != 0 || (line[8] != ' ' && line[8] != '\t')) return 0;
    const char *p = line + 8;
    while (*p == ' ' || *p == '\t') p++;
    long v = 0;
    const char *d = p;
    while (isdigit((unsigned char)*p) && v <= 1000000) v = v * 10 + (*p++ - '0');
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p == d || *p || v <= 0 || v > 1000000) return -1;
    return (int)v;
}

int ctl_log_parse(const char *line, int allow_next, struct ctl_log_event *ev, char *err, size_t errlen)
{
    char buf[1024];
    size_t len = strlen(line);
    if (len >= sizeof buf) return fail(err, errlen, "line too long");
    memcpy(buf, line, len + 1);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = 0;
    char *tok[16];
    int nt = 0;
    for (char *p = buf; *p;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (nt == 0 && *p == '#') return 0;                 /* comment */
        if (nt == (int)(sizeof tok / sizeof tok[0])) return fail(err, errlen, "too many fields");
        tok[nt++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    if (nt == 0) return 0;                                  /* blank */
    if (nt < 2) return fail(err, errlen, "expected a frame number and an event kind");

    memset(ev, 0, sizeof *ev);
    ev->qp_min = ev->qp_max = -1;
    ev->last = -1;
    if (!strcmp(tok[0], "next")) {
        if (!allow_next) return fail(err, errlen, "'next' is only accepted on --control-fd");
        ev->frame = CTL_FRAME_NEXT;
    } else {
        int64_t f;
        if (tok[0][0] == '-' || parse_i64(tok[0], 0, (int64_t)1 << 53, &f) < 0)
            return fail(err, errlen, "bad frame number '%s'", tok[0]);
        ev->frame = f;
    }
    const struct kind_spec *k = NULL;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (!strcmp(tok[1], kinds[i].name)) k = &kinds[i];
    if (!k) return fail(err, errlen, "unknown event kind '%s'", tok[1]);
    ev->kind = k->kind;

    unsigned seen = 0;
    for (int t = 2; t < nt; t++) {
        char *eq = strchr(tok[t], '=');
        if (!eq || eq == tok[t]) return fail(err, errlen, "'%s' is not name=value", tok[t]);
        *eq = 0;
        const char *name = tok[t], *val = eq + 1;
        int ai = -1;
        for (int a = 0; a < 4 && k->args[a]; a++)
            if (!strcmp(name, k->args[a])) ai = a;
        if (ai < 0) return fail(err, errlen, "'%s' takes no argument '%s'", k->name, name);
        if (seen & (1u << ai)) return fail(err, errlen, "argument '%s' given twice", name);
        if (!*val) return fail(err, errlen, "argument '%s' has no value", name);
        seen |= 1u << ai;
        int64_t iv;
        double dv;
        int bad = 0;
        switch (k->kind) {
        case CTL_KIND_RATE:
            bad = parse_i64(val, 1, 100000000, &iv) < 0;
            if (!bad) {
                if (ai == 0) ev->bitrate_kbps = (int)iv;
                else if (ai == 1) ev->vbv_maxrate_kbps = (int)iv;
                else ev->vbv_bufsize_kbit = (int)iv;
            }
            break;
        case CTL_KIND_CRF:
            bad = parse_dbl(val, &dv) < 0;
            if (!bad) ev->crf = dv;
            break;
        case CTL_KIND_QP_LIMITS:
            bad = parse_i64(val, 0, 51, &iv) < 0;
            if (!bad) { if (ai == 0) ev->qp_min = (int)iv; else ev->qp_max = (int)iv; }
            break;
        case CTL_KIND_ZONE:
            if (ai == 0) { bad = parse_i64(val, 0, (int64_t)1 << 53, &iv) < 0; if (!bad) ev->last = iv; }
            else { bad = parse_dbl(val, &dv) < 0; if (!bad) ev->qp_offset = dv; }
            break;
        case CTL_KIND_FPS:
            bad = parse_i64(val, 1, 1000000, &iv) < 0;
            if (!bad) { if (ai == 0) ev->fps_num = (int)iv; else ev->fps_den = (int)iv; }
            break;
        case CTL_KIND_PRESET:
            bad = strlen(val) >= sizeof ev->preset;
            if (!bad) snprintf(ev->preset, sizeof ev->preset, "%s", val);
            break;
        case CTL_KIND_RESOLUTION:
            bad = parse_i64(val, 1, 65536, &iv) < 0;
            if (!bad) { if (ai == 0) ev->width = (int)iv; else ev->height = (int)iv; }
            break;
        }
        if (bad) return fail(err, errlen, "bad value '%s' for '%s'", val, name);
    }
    if ((seen & k->required) != k->required) {
        for (int a = 0; a < 4 && k->args[a]; a++)
            if ((k->required & (1u << a)) && !(seen & (1u << a)))
                return fail(err, errlen, "'%s' needs '%s='", k->name, k->args[a]);
    }
    if (k->any_of && !seen) {
        char want[96] = "";
        for (int a = 0; a < 4 && k->args[a]; a++) {
            strncat(want, a ? ", " : "", sizeof want - strlen(want) - 1);
            strncat(want, k->args[a], sizeof want - strlen(want) - 1);
        }
        return fail(err, errlen, "'%s' needs at least one of %s", k->name, want);
    }
    return 1;
}

/* the shortest plain decimal (no exponent, which the grammar refuses) that
 * reads back to the same double; every value parse_dbl accepts has one */
static void fmt_dbl(char *out, size_t n, double v)
{
    for (int p = 0; p <= CTL_REAL_FRAC_DIGITS; p++) {
        snprintf(out, n, "%.*f", p, v);
        if (strtod(out, NULL) == v) return;
    }
}

void ctl_log_format(const struct ctl_log_event *ev, char *buf, size_t n)
{
    const char *name = "?";
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (kinds[i].kind == ev->kind) name = kinds[i].name;
    int w = snprintf(buf, n, "%lld %s", (long long)ev->frame, name);
    if (w < 0 || (size_t)w >= n) return;
    char d[40];
    size_t o = (size_t)w;
#define ADD(...) do { if (o < n) { int k_ = snprintf(buf + o, n - o, __VA_ARGS__); if (k_ > 0) o += (size_t)k_; } } while (0)
    switch (ev->kind) {
    case CTL_KIND_RATE:
        if (ev->bitrate_kbps > 0) ADD(" bitrate=%d", ev->bitrate_kbps);
        if (ev->vbv_maxrate_kbps > 0) ADD(" maxrate=%d", ev->vbv_maxrate_kbps);
        if (ev->vbv_bufsize_kbit > 0) ADD(" bufsize=%d", ev->vbv_bufsize_kbit);
        break;
    case CTL_KIND_CRF:
        fmt_dbl(d, sizeof d, ev->crf);
        ADD(" value=%s", d);
        break;
    case CTL_KIND_QP_LIMITS:
        if (ev->qp_min >= 0) ADD(" min=%d", ev->qp_min);
        if (ev->qp_max >= 0) ADD(" max=%d", ev->qp_max);
        break;
    case CTL_KIND_ZONE:
        fmt_dbl(d, sizeof d, ev->qp_offset);
        ADD(" last=%lld offset=%s", (long long)ev->last, d);
        break;
    case CTL_KIND_FPS:
        ADD(" num=%d den=%d", ev->fps_num, ev->fps_den);
        break;
    case CTL_KIND_PRESET:
        ADD(" name=%s", ev->preset);
        break;
    case CTL_KIND_RESOLUTION:
        ADD(" width=%d height=%d", ev->width, ev->height);
        break;
    }
#undef ADD
}
