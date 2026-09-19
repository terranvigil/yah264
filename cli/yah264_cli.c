/*
 * yah264_cli.c - command-line front end: Y4M in, Annex-B out
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "yah264.h"
#include "hw/vt_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
/* ---------------------------------------------------------------------
 * Both libraries, one command line (item C3-10bit).
 *
 * The sample width is a compile-time type, so there are two encoder libraries
 * and there always will be; what there is no longer is two command lines.
 * yah264 links both archives and picks the encoder from the INPUT: a C420 y4m
 * goes to the 8-bit library, a C420p10 one to High 10, and --output-depth 10
 * puts 8-bit input through High 10 after an explicit upshift. The two are told
 * apart by name -- the 10-bit library's symbols carry a _10 suffix
 * (include/yah264.h's YAH264_API for the public entry points, a force-included
 * header for the internals) -- and by nothing else: same header, same structs,
 * same argument lists.
 *
 * This file is compiled at Y264_BIT_DEPTH 8, so the header hands it the 8-bit
 * names unsuffixed; the 10-bit ones it declares here. yah264_picture_t carries
 * its planes as void* for exactly this reason, so one struct and one dispatch
 * table serve both widths.
 * ------------------------------------------------------------------ */
const char *yah264_version_10(void);
int yah264_bit_depth_10(void);
const char *yah264_cpu_features_10(void);
void yah264_param_default_10(yah264_param_t *param);
int yah264_param_apply_preset_10(yah264_param_t *param, const char *preset);
yah264_encoder_t *yah264_encoder_open_10(const yah264_param_t *param);
yah264_encoder_t *yah264_encoder_open_hw_10(const yah264_param_t *param, int hw);
const char *yah264_encoder_backend_10(const yah264_encoder_t *enc);
int yah264_encoder_set_video_signal_10(yah264_encoder_t *enc, const yah264_video_signal_t *vs);
int yah264_encoder_headers_10(yah264_encoder_t *enc, yah264_nal_t **nal, int *count);
int yah264_encoder_encode_10(yah264_encoder_t *enc, yah264_nal_t **nal, int *count,
                             const yah264_picture_t *pic);
void yah264_encoder_set_recon_cb_10(yah264_encoder_t *enc,
                                    void (*cb)(void *, const yah264_picture_t *, int, int),
                                    void *ud);
int yah264_encoder_set_zones_10(yah264_encoder_t *enc, const yah264_zone_t *zones, int n);
int yah264_encoder_set_frame_forces_10(yah264_encoder_t *enc, const yah264_frame_force_t *forces, int n);
int yah264_encoder_frame_stats_10(yah264_encoder_t *enc, yah264_frame_stats_t *out, int max);
int yah264_encoder_frame_order_10(yah264_encoder_t *enc, int *disp, int max);
int yah264_frame_thread_cap_10(int width, int height);
int yah264_threads_auto_10(void);
int yah264_lookahead_delay_10(const yah264_param_t *param);
int yah264_scan_shots_10(const yah264_param_t *param, const void *const *luma,
                         const int *stride, int n, int nthreads, int depth,
                         unsigned char *idr, yah264_shot_t *shots, int max_shots);
int yah264_scan_idr_frames_10(const yah264_param_t *param, const void *const *luma,
                              const int *stride, int n, int nthreads, int depth,
                              unsigned char *idr);
void yah264_encoder_close_10(yah264_encoder_t *enc);
int yah264_encoder_rc_state_10(const yah264_encoder_t *enc, yah264_rc_state_t *out);
int yah264_encoder_rc_import_10(yah264_encoder_t *enc, const yah264_rc_state_t *state,
                                int frames_ahead);

typedef struct yah264_api {
    int depth;                  /* the samples this table's library takes */
    const char *(*version)(void);
    const char *(*cpu_features)(void);
    void (*param_default)(yah264_param_t *);
    int  (*param_apply_preset)(yah264_param_t *, const char *);
    yah264_encoder_t *(*encoder_open)(const yah264_param_t *);
    yah264_encoder_t *(*encoder_open_hw)(const yah264_param_t *, int);
    const char *(*encoder_backend)(const yah264_encoder_t *);
    int  (*set_video_signal)(yah264_encoder_t *, const yah264_video_signal_t *);
    int  (*encoder_headers)(yah264_encoder_t *, yah264_nal_t **, int *);
    int  (*encoder_encode)(yah264_encoder_t *, yah264_nal_t **, int *,
                           const yah264_picture_t *);
    void (*set_recon_cb)(yah264_encoder_t *,
                         void (*)(void *, const yah264_picture_t *, int, int), void *);
    int  (*set_zones)(yah264_encoder_t *, const yah264_zone_t *, int);
    int  (*set_frame_forces)(yah264_encoder_t *, const yah264_frame_force_t *, int);
    int  (*frame_stats)(yah264_encoder_t *, yah264_frame_stats_t *, int);
    int  (*frame_order)(yah264_encoder_t *, int *, int);
    int  (*frame_thread_cap)(int, int);
    int  (*threads_auto)(void);
    int  (*lookahead_delay)(const yah264_param_t *);
    int  (*scan_shots)(const yah264_param_t *, const void *const *, const int *,
                       int, int, int, unsigned char *, yah264_shot_t *, int);
    int  (*scan_idr_frames)(const yah264_param_t *, const void *const *,
                            const int *, int, int, int, unsigned char *);
    void (*encoder_close)(yah264_encoder_t *);
    int  (*rc_state)(const yah264_encoder_t *, yah264_rc_state_t *);
    int  (*rc_import)(yah264_encoder_t *, const yah264_rc_state_t *, int);
} yah264_api;

static const yah264_api api8 = {
    8, yah264_version, yah264_cpu_features, yah264_param_default,
    yah264_param_apply_preset, yah264_encoder_open, yah264_encoder_open_hw,
    yah264_encoder_backend,
    yah264_encoder_set_video_signal, yah264_encoder_headers,
    yah264_encoder_encode, yah264_encoder_set_recon_cb,
    yah264_encoder_set_zones, yah264_encoder_set_frame_forces,
    yah264_encoder_frame_stats,
    yah264_encoder_frame_order,
    yah264_frame_thread_cap, yah264_threads_auto, yah264_lookahead_delay,
    yah264_scan_shots, yah264_scan_idr_frames, yah264_encoder_close,
    yah264_encoder_rc_state, yah264_encoder_rc_import,
};

static const yah264_api api10 = {
    10, yah264_version_10, yah264_cpu_features_10, yah264_param_default_10,
    yah264_param_apply_preset_10, yah264_encoder_open_10,
    yah264_encoder_open_hw_10,
    yah264_encoder_backend_10, yah264_encoder_set_video_signal_10,
    yah264_encoder_headers_10, yah264_encoder_encode_10,
    yah264_encoder_set_recon_cb_10, yah264_encoder_set_zones_10,
    yah264_encoder_set_frame_forces_10,
    yah264_encoder_frame_stats_10, yah264_encoder_frame_order_10,
    yah264_frame_thread_cap_10,
    yah264_threads_auto_10, yah264_lookahead_delay_10, yah264_scan_shots_10,
    yah264_scan_idr_frames_10, yah264_encoder_close_10,
    yah264_encoder_rc_state_10, yah264_encoder_rc_import_10,
};

/* The selected library, and the width of one sample in its pictures. 8-bit
 * until the Y4M header says otherwise, so everything that runs before the
 * header is parsed reads the encoder it always did. */
static const yah264_api *g_api = &api8;
static int g_enc_sample_sz = 1;

/* Bytes per luma/chroma sample ON DISK. 1 at 8-bit; 2 (16-bit little-endian,
 * as ffmpeg writes yuv420p10le / Y4M "C420p10") at 10/12-bit. The library's
 * `pixel` type is uint16 at BD>8 and the host is little-endian, so at a
 * matched depth the on-disk layout and the in-memory buffer are the same bytes
 * and the read path copies nothing. They differ only under --output-depth 10
 * on 8-bit input, which is the one case that has a conversion in it. */
static int g_in_sample_sz = 1;
static int g_upshift;           /* 8-bit input into the 10-bit library */
#define Y264_STR_(x) #x
#define Y264_STR(x) Y264_STR_(x)

/* Chroma subsampling of the input/recon, parsed from the Y4M C tag. Default
 * 4:2:0. Set once after the header is read; the single-threaded encode path and
 * the recon dumper read them. */
static int g_sub_w = 2, g_sub_h = 2;
/* 1 = the input is headerless planar YUV, so every frame is exactly one
 * payload with no FRAME line in front of it. Read by the three places that
 * walk the input: the streaming reader, the serial loop and the frame-count
 * estimator. */
static int g_raw_input;

/* Field order the input Y4M header declared: 0 = progressive or unstated,
 * 1 = It (top field first), 2 = Ib (bottom field first). */
static int g_y4m_interlace;

/* Y4M chroma tag matching g_sub_w/g_sub_h and the depth of the library that
 * produced the recon, which is the selected one and no longer the build's. */
static const char *y264_y4m_ctag(void)
{
    int d = g_api->depth;
    if (g_sub_w == 1 && g_sub_h == 1) return d > 8 ? "C444p10" : "C444";
    if (g_sub_w == 2 && g_sub_h == 1) return d > 8 ? "C422p10" : "C422";
    return d > 8 ? "C420p10" : "C420jpeg";
}

/* Read one plane of `n` samples into a buffer of n * g_enc_sample_sz bytes.
 *
 * At a matched depth this is the fread it always was, into the whole buffer.
 * Under --output-depth 10 on 8-bit input the bytes are read into the SECOND
 * HALF of the destination and expanded in place, ascending: the write for
 * sample i lands on bytes 2i and 2i+1, the not-yet-read source bytes start at
 * n+i+1, and 2i+1 < n+i+1 for every i below n, so the expansion never passes
 * the read head. (Descending is the version that corrupts: it writes into the
 * tail before reading it.) */
static int read_plane(FILE *in, void *dst, size_t n)
{
    if (!g_upshift)
        return fread(dst, 1, n * (size_t)g_in_sample_sz, in) == n * (size_t)g_in_sample_sz;
    uint16_t *d16 = dst;
    uint8_t *tail = (uint8_t *)dst + n;         /* the 8-bit plane, right-aligned */
    if (fread(tail, 1, n, in) != n) return 0;
    for (size_t i = 0; i < n; i++)
        d16[i] = (uint16_t)tail[i] << 2;        /* 8 -> 10 bits, MSB-aligned */
    return 1;
}

/* The only caller of sysctlbyname() is y264_phys_mem(), and that one is under
 * its own __APPLE__ guard. So the #endif belongs one line down, and while it
 * stretched to the end of this block the CLI did not compile on Linux AT ALL,
 * on any architecture -- 60-odd undeclared identifiers, the log level and the
 * VUI state among them. The ubuntu CI job is manual-only, so nothing had asked
 * the question since. */
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

/* Video signal (VUI) from --range/--colorprim/--transfer/--colormatrix/
 * --chromaloc and the Y4M XCOLORRANGE tag. H.273 codes; names for the
 * common ones. Unset = nothing signalled. */
static yah264_video_signal_t g_vs = { 0, 2, 2, 2, -1 };
static int g_cut_split, g_shot_table, g_shot_crf;   /* --cut-split, --shot-table, --shot-crf (docs/shot-based-plan.md S1/S2) */
/* The engine hooks (docs/engine-interface.md, shot-based plan S4): a plan file
 * of zones, per-GOP frame-thread pinning, per-segment output and per-frame
 * stats. All opt-in; none changes the stream unless a zone says so. */
static yah264_zone_t *g_zones; static int g_nzones, g_plan_idr;
static yah264_frame_force_t *g_forces; static int g_nforces, g_force_idr;
/* A forced type or QP the encoder refused. Set from the GOP workers, read once
 * at the end: a refusal has to reach the exit status, or a run that coded the
 * wrong frames looks like a run that worked. */
static volatile int g_force_reject;
/* The first frame of the GOP instance that refused, so the frame number the
 * encoder printed (which counts from the instance's own first frame) can be
 * added back to a number in the file. -1 = the whole stream was one instance,
 * where the two already agree. */
static volatile int g_force_reject_base = -1;
static int g_gop_threads;                 /* --gop-threads K: every GOP instance's frame_threads */
static const char *g_segment_out;         /* --segment-out PATTERN (printf %d = GOP index) */
/* --open-gop. The GOP table is a table of IDRs: every boundary in it opens an
 * encoder instance of its own, with its own parameter sets, FrameNum and POC.
 * An open GOP's keyframe is none of those things, so with this set the keyint
 * cadence stops producing boundaries and the whole encode is one instance --
 * a cut (with --cut-split) or a plan IDR still splits, because both really are
 * IDRs. --threads then buys row wavefront inside that instance instead of
 * parallel GOPs, which is the price of the feature and not a bug to fix. */
static int g_open_gop;
static const char *g_frame_stats;         /* --frame-stats FILE (JSON lines) */

/* --- verbosity ------------------------------------------------------------
 * One dial, four steps, and every informational line on stderr goes through
 * it. Errors and warnings are NOT informational: --quiet still prints them,
 * because a run that fails silently is worse than a noisy one. */
enum { LOG_ERROR = 1, LOG_WARN = 2, LOG_INFO = 3, LOG_DEBUG = 4 };
static int g_log = LOG_INFO;
static int g_no_progress;                 /* --no-progress */
#define LOGF(lvl, ...) do { if (g_log >= (lvl)) fprintf(stderr, __VA_ARGS__); } while (0)

/* --- the progress line ----------------------------------------------------
 * Only on a terminal. A gate's stderr is a file or a pipe, so nothing here
 * ever reaches a log, and --no-progress turns it off for a terminal session
 * that wants clean output. It is the only stderr in the CLI that rewrites its
 * own line, which is exactly why it must not run into a file. */
static int g_prog_on = -1;      /* -1 = not resolved yet */
static double g_prog_t0, g_prog_last;

static double prog_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void progress(long done, long total)
{
    if (g_prog_on < 0) {
        g_prog_on = !g_no_progress && g_log >= LOG_INFO && isatty(2);
        g_prog_t0 = g_prog_last = prog_now();
    }
    if (!g_prog_on) return;
    double t = prog_now();
    if (done > 0 && t - g_prog_last < 0.25) return;      /* four a second is plenty */
    g_prog_last = t;
    double el = t - g_prog_t0;
    double fps = el > 0 ? done / el : 0;
    if (total > 0)
        fprintf(stderr, "\ryah264: %ld/%ld frames, %.1f fps   ", done, total, fps);
    else
        fprintf(stderr, "\ryah264: %ld frames, %.1f fps   ", done, fps);
    fflush(stderr);
}

static void progress_done(void)
{
    if (g_prog_on > 0) { fputc('\r', stderr); fprintf(stderr, "%60s\r", ""); fflush(stderr); }
}

/* --- raw input and cropping ----------------------------------------------
 * g_in_w/g_in_h are the geometry of a frame ON THE INPUT, which is what the
 * reader's buffers and the file-length frame count are sized by. The encoder's
 * width/height are the CODED geometry, which --crop-rect makes smaller. The
 * two were the same number until 2026-09-16 and several places still read the
 * coded one where they mean the input one, so the distinction is worth
 * keeping loud. */
static int g_in_w, g_in_h;
static int g_crop_l, g_crop_t, g_crop_r, g_crop_b;

/* Plan file: one zone per line, "first last [idr] [qp+N|qp-N]", '#' comments.
 * Frames count from zero in input order, inclusive. */
static int parse_plan(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "yah264: cannot read plan %s\n", path); return -1; }
    char line[256]; int cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        int first, last, nread = 0;
        if (sscanf(line, "%d %d%n", &first, &last, &nread) < 2) continue;
        if (g_nzones == cap) { cap = cap ? cap * 2 : 16; g_zones = realloc(g_zones, (size_t)cap * sizeof *g_zones); }
        yah264_zone_t *z = &g_zones[g_nzones++];
        z->first = first; z->last = last; z->flags = 0; z->qp_offset = 0;
        for (char *tok = strtok(line + nread, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
            if (!strcmp(tok, "idr")) { z->flags |= YAH264_ZONE_IDR; g_plan_idr = 1; }
            else if (!strncmp(tok, "qp", 2)) z->qp_offset = atof(tok + 2);
            else { fprintf(stderr, "yah264: plan %s: unknown token '%s'\n", path, tok); fclose(f); return -1; }
        }
    }
    fclose(f);
    return 0;
}

/* Frame file: one line per frame, "<frame> <type> <qp>", '#' comments. Frames
 * count from zero in input order. The type is one of I K i P B b and the QP is
 * absolute, or -1 for "the rate control decides"; a frame the file does not
 * name is left entirely to the encoder.
 *
 * `i` is accepted and refused rather than ignored: a non-IDR I frame needs an
 * open GOP, which this encoder does not have, and silently coding an IDR there
 * would answer a different question than the one the file asked. */
static int parse_qpfile(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "yah264: cannot read qpfile %s\n", path); return -1; }
    char line[256]; int cap = 0, lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *h = strchr(line, '#'); if (h) *h = 0;
        int frame, qp; char ty[16];
        int nf = sscanf(line, "%d %15s %d", &frame, ty, &qp);
        if (nf <= 0) continue;
        if (nf < 3) {
            fprintf(stderr, "yah264: qpfile %s:%d: expected \"<frame> <type> <qp>\"\n", path, lineno);
            fclose(f); return -1;
        }
        if (frame < 0) {
            fprintf(stderr, "yah264: qpfile %s:%d: frame %d is before the start of the stream\n",
                    path, lineno, frame);
            fclose(f); return -1;
        }
        if (qp < -1 || qp > 51) {
            fprintf(stderr, "yah264: qpfile %s:%d: QP %d is outside 0..51 (-1 = the rate control decides)\n",
                    path, lineno, qp);
            fclose(f); return -1;
        }
        int type;
        if (ty[1]) type = -1;
        else switch (ty[0]) {
            case 'I': case 'K': type = YAH264_FORCE_IDR; break;
            case 'P': case 'p': type = YAH264_FORCE_P;   break;
            case 'B': case 'b': type = YAH264_FORCE_B;   break;
            case '-':           type = YAH264_FORCE_NONE; break;
            case 'i':
                fprintf(stderr, "yah264: qpfile %s:%d: frame %d asks for a non-IDR I frame, "
                                "which needs an open GOP this encoder does not have\n",
                        path, lineno, frame);
                fclose(f); return -1;
            default: type = -1;
        }
        if (type < 0) {
            fprintf(stderr, "yah264: qpfile %s:%d: unknown frame type '%s' (I K i P B b, or - for none)\n",
                    path, lineno, ty);
            fclose(f); return -1;
        }
        if (frame == 0 && type == YAH264_FORCE_B) {
            fprintf(stderr, "yah264: qpfile %s:%d: frame 0 cannot be a B frame, it opens the stream\n",
                    path, lineno);
            fclose(f); return -1;
        }
        for (int i = 0; i < g_nforces; i++)
            if (g_forces[i].disp == frame) {
                fprintf(stderr, "yah264: qpfile %s:%d: frame %d is named twice\n", path, lineno, frame);
                fclose(f); return -1;
            }
        if (g_nforces == cap) { cap = cap ? cap * 2 : 64; g_forces = realloc(g_forces, (size_t)cap * sizeof *g_forces); }
        g_forces[g_nforces++] = (yah264_frame_force_t){ frame, type, qp };
        if (type == YAH264_FORCE_IDR) g_force_idr = 1;
    }
    fclose(f);
    return 0;
}

/* Rebase the frame forces onto one GOP instance, exactly as apply_zones does.
 * A force on the instance's own first frame keeps its QP and drops an IDR it
 * does not need to ask for. */
static void apply_forces(yah264_encoder_t *e, int start, int end)
{
    if (!g_nforces) return;
    yah264_frame_force_t *f = malloc((size_t)g_nforces * sizeof *f); int m = 0;
    for (int i = 0; i < g_nforces; i++) {
        int d = g_forces[i].disp;
        if (d < start || d >= end) continue;
        f[m] = g_forces[i];
        f[m].disp = d - start;
        if (f[m].disp == 0) {
            /* The instance opens on a key frame. An IDR asked for here is
 * already there and a P yields to it (the header says so); a B is the
 * one force a key frame cannot absorb, so it is refused by frame
 * number rather than quietly coded as an I. */
            if (f[m].type == YAH264_FORCE_B) {
                fprintf(stderr, "yah264: frame %d cannot be coded as a B frame "
                                "(a key frame is placed here)\n", d);
                g_force_reject = 1;
                free(f);
                return;
            }
            f[m].type = YAH264_FORCE_NONE;
        }
        m++;
    }
    if (m && g_api->set_frame_forces(e, f, m) < 0) {
        fprintf(stderr, "yah264: qpfile rejected on the GOP at %d\n", start);
        g_force_reject = 1;
    }
    free(f);
}

/* Rebase the plan onto one GOP instance (frames [start, end)) and hand it in;
 * the IDR flag is dropped for a zone starting at the instance's own first
 * frame (already an IDR) and kept elsewhere. */
static void apply_zones(yah264_encoder_t *e, int start, int end)
{
    if (!g_nzones) return;
    yah264_zone_t *z = malloc((size_t)g_nzones * sizeof *z); int m = 0;
    for (int i = 0; i < g_nzones; i++) {
        int a = g_zones[i].first, b = g_zones[i].last;
        if (b < start || a >= end) continue;
        z[m] = g_zones[i];
        z[m].first = a > start ? a - start : 0;
        z[m].last = (b < end - 1 ? b : end - 1) - start;
        if (a <= start) z[m].flags &= ~YAH264_ZONE_IDR;
        m++;
    }
    if (m && g_api->set_zones(e, z, m) < 0) fprintf(stderr, "yah264: zones rejected on GOP at %d\n", start);
    free(z);
}

/* Per-frame stats join: the NALs of one encode call, the display indices the
 * encoder emitted them for (coding order) and its decision records; one JSON
 * line per slice NAL. `st_by` is indexed by display index within the instance. */
static void frame_stats_emit(FILE *out, yah264_encoder_t *e, const yah264_nal_t *nal, int cnt,
                             yah264_frame_stats_t *st_by, int nst, int base, int gop, int k)
{
    yah264_frame_stats_t st[128];
    int m = g_api->frame_stats(e, st, 128);
    for (int i = 0; i < m; i++) if (st[i].disp >= 0 && st[i].disp < nst) st_by[st[i].disp] = st[i];
    int packets = 0;
    for (int i = 0; i < cnt; i++) packets += nal[i].type == YAH264_NAL_SLICE || nal[i].type == YAH264_NAL_SLICE_IDR;
    int disp[128]; int got = g_api->frame_order(e, disp, packets < 128 ? packets : 128);
    int pi = 0;
    for (int i = 0; i < cnt && pi < got; i++) {
        if (!(nal[i].type == YAH264_NAL_SLICE || nal[i].type == YAH264_NAL_SLICE_IDR)) continue;
        int d = disp[pi++];
        const yah264_frame_stats_t *r = d >= 0 && d < nst ? &st_by[d] : NULL;
        fprintf(out, "{\"frame\": %d, \"gop\": %d, \"type\": \"%c\", \"idr\": %d, \"ref\": %d, \"qp\": %d, \"bytes\": %d, \"k\": %d}\n",
                base + d, gop, r ? "IPB"[r->type] : '?', r ? r->is_idr : (nal[i].type == YAH264_NAL_SLICE_IDR),
                r ? r->is_ref : -1, r ? r->qp : -1, (int)nal[i].size, k);
    }
}
static double shot_tunable(const char *n, double def) { const char *v = getenv(n); return v ? atof(v) : def; }
static int g_vs_set, g_y4m_full_range = -1;
static int colour_code(const char *opt, const char *v)
{
    if (v[0] >= '0' && v[0] <= '9') return atoi(v);
    int prim = !strcmp(opt, "--colorprim"), tr = !strcmp(opt, "--transfer"), mat = !strcmp(opt, "--colormatrix");
    if (!strcmp(v, "bt709")) return 1;
    if (!strcmp(v, "bt2020")) return prim ? 9 : (tr ? 14 : 9);
    if (!strcmp(v, "bt601") || !strcmp(v, "smpte170m")) return prim ? 6 : (tr ? 6 : 6);
    if (!strcmp(v, "bt470bg")) return prim ? 5 : (tr ? 5 : 5);
    if (!strcmp(v, "srgb") || !strcmp(v, "iec61966-2-1")) return tr ? 13 : (mat ? 0 : 1);
    if (!strcmp(v, "smpte2084") || !strcmp(v, "pq")) return tr ? 16 : -1;
    if (!strcmp(v, "arib-std-b67") || !strcmp(v, "hlg")) return tr ? 18 : -1;
    if (!strcmp(v, "unspecified")) return 2;
    return -1;
}
static void apply_video_signal(yah264_encoder_t *e)
{
    if (!e) return;
    yah264_video_signal_t vs = g_vs;
    if (!g_vs_set && g_y4m_full_range < 0) return;
    if (g_y4m_full_range >= 0 && !g_vs_set) vs.full_range = g_y4m_full_range;
    else if (g_y4m_full_range >= 0 && g_vs.full_range == 0 && g_y4m_full_range == 1) vs.full_range = 1;
    g_api->set_video_signal(e, &vs);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "yah264 %s - H.264 encoder (Phase 0: I_PCM)\n"
        "usage: %s --input-y4m <in.y4m|-> [-o <out.264|->] [options]\n"
        "  --input-y4m PATH   Y4M input, '-' for stdin\n"
        "  --input-raw PATH   headerless planar YUV, '-' for stdin. Needs\n"
        "                     --input-res; --input-csp, --fps and --input-range\n"
        "                     supply what a Y4M header would have said.\n"
        "  --input-res WxH    raw input geometry (required with --input-raw)\n"
        "  --input-csp NAME   i420 (default), i422, i444, each optionally with a\n"
        "                     p10 suffix -- raw input has no C tag, so the depth\n"
        "                     travels with the format and there is no --input-depth\n"
        "  --fps N[/D]        raw input frame rate (default 25; decimals ok)\n"
        "  --input-range full|limited   VUI colour range of the raw input\n"
        "  --seek N           drop the first N input frames\n"
        "  --crop-rect L,T,R,B  crop the input before encoding, in luma samples\n"
        "  -v, --verbose / --quiet / --log-level error|warning|info|debug\n"
        "                     how much this prints on stderr. Errors always\n"
        "                     print. -v adds the resolved settings line.\n"
        "  --no-progress      no progress line (it only appears on a terminal)\n"
        "  -o, --output PATH  Annex-B output, '-' for stdout (default: -)\n"
        "  (bare default mirrors x264 medium: --preset medium --cabac --ref 3\n"
        "   --bframes 3 --transform-8x8 --aq-strength 0.4, so `yah264 in.y4m` is\n"
        "   directly comparable to `x264 in.y4m`.)\n"
        "  --preset NAME      ultrafast..medium..veryslow..placebo; sets subme +\n"
        "                     subpel tier. Default = medium (x264-match). Omit for\n"
        "                     medium; pass veryslow/placebo for the max-quality tier.\n"
        "  --qp N             constant QP, 0..51 (default 26)\n"
        "  --bitrate N        target average bitrate in kbit/s (single-pass ABR)\n"
        "  --crf N            constant rate factor, ~0..51 (constant quality)\n"
        "  --vbv-maxrate N    VBV peak bitrate in kbit/s (with --vbv-bufsize)\n"
        "  --vbv-bufsize N    VBV buffer size in kbit\n"
        "  --pass N           multi-pass: 1 = analysis (writes stats), 2 = final\n"
        "                     (reads them), 3 = read AND write, so a further pass\n"
        "                     refines against a real encode. Pair with --bitrate.\n"
        "  --stats PATH       2-pass stats file (default yah264.stats)\n"
        "  (--qp, --bitrate and --crf each select a rate-control mode. Give more\n"
        "   than one and the LAST on the command line wins, as in x264; what the\n"
        "   others still do, or no longer do, is named on stderr. --pass is not a\n"
        "   mode of its own: it targets --bitrate.)\n"
        "  --keyint N         max frames between IDR keyframes (default 250)\n"
        "  --min-keyint N     min frames between IDRs; a scene cut closer than\n"
        "                     this is not promoted (0/auto = keyint/10)\n"
        "  --scenecut N       how aggressively to insert extra keyframes\n"
        "                     (default 40; 0 = off, same as --no-scenecut)\n"
        "  --no-scenecut      never insert extra keyframes; only --keyint places\n"
        "                     IDRs. Also makes the cut-aware GOP split a no-op\n"
        "  --open-gop         every keyframe but the first is a non-IDR I picture\n"
        "                     with a recovery_point SEI -- the --keyint cadence and\n"
        "                     the adaptive scene cuts alike: the B frames before it\n"
        "                     keep referencing the anchor behind it, the DPB is not\n"
        "                     flushed and POC runs on. One sequence end to end, so\n"
        "                     the encode is ONE GOP instance and --threads spends\n"
        "                     its budget on the row wavefront instead. Needs a\n"
        "                     known frame count above one thread; refused with\n"
        "                     --stitchable. --no-open-gop is the default.\n",
        yah264_version(), argv0);
    /* Split here only because one literal for the whole thing runs past the
 * 4095 bytes ISO C99 guarantees a compiler will take. */
    fprintf(stderr,
        "  --threads N        GOP-parallel worker threads (0/auto = cores). The\n"
        "                     threaded path streams the input through a bounded\n"
        "                     window of (--threads + 1) x --keyint frames at\n"
        "                     w*h*1.5 bytes each (Y264_STREAM_WINDOW overrides the\n"
        "                     count); a window needing more than half of physical\n"
        "                     RAM is refused rather than OOM-killed, and\n"
        "                     Y264_MAX_INPUT_MB sets the limit directly.\n"
        "                     --dump-recon forces the serial path (4:2:0, 4:2:2 and\n"
        "                     4:4:4 all thread). A --threads that cannot be honoured\n"
        "                     says so on stderr instead of silently encoding on one.\n"
        "  --hw MODE          off (default) | auto | videotoolbox: encode with the\n"
        "                     Mac's hardware H.264 encoder through VideoToolbox.\n"
        "                     auto falls back to yah264 with one warning when the\n"
        "                     hardware refuses; videotoolbox fails instead. The\n"
        "                     stream is the hardware's; options it cannot honour\n"
        "                     are listed once at open (--hw-strict makes that an\n"
        "                     error). 4:2:0 8-bit only.\n"
        "  --frames N         stop after N frames (0 = all)\n"
        "  --aq-strength F    variance adaptive quantization strength (0 = off;\n"
        "                     default 0.4 for CRF/ABR/2-pass, off for pure CQP;\n"
        "                     x264's default is 1.0)\n"
        "  --rc-lookahead N   mb-tree lookahead window in frames (default 40, 0 = off)\n"
        "  --sync-lookahead N frames of input buffered so the lookahead runs on\n"
        "                     its own thread ahead of the encode. Costs exactly N\n"
        "                     frames of latency and never changes a bit. Default\n"
        "                     auto = bframes+1 once the wavefront pool is wide\n"
        "                     enough to run a chain against, else 0. N=0 disables\n"
        "                     (also set by --tune zerolatency)\n"
        "  --b-adapt N        adaptive B placement, 0 = fixed cadence (default 1)\n"
        "  --psy-rd F         psychovisual RD strength (0 = off)\n"
        "  --psy-trellis F    psy-trellis strength (0 = off; ~1.0 for grain/detail)\n"
        "  --trellis N        RDOQ placement (x264-compatible): 0 = off, 1 = final\n"
        "                     macroblock only (default), 2 = every mode decision\n"
        "  --tune NAME        grain, film, animation, psnr, ssim, zerolatency,\n"
        "                     stillimage, fastdecode\n"
        "  --direct MODE      B direct MV mode: auto (default: each B slice picks\n"
        "                     spatial or temporal by skippability), spatial, temporal\n"
        "  --me METHOD        motion search: dia, hex, umh (default: follow --preset;\n"
        "                     medium+faster = hex, slow+ = umh)\n");
    /* Third chunk, same 4095-byte reason as the split above. */
    fprintf(stderr,
        "  --subme N          subpel/RD analysis level 1..11, x264's scale (default\n"
        "                     from --preset; medium = 7). NOTE: with no --me, the\n"
        "                     search method follows it -- below 8 hex, 8 and above\n"
        "                     umh -- which x264 does not do. Pass --me to pin it.\n"
        "                     N=0 is refused: x264's fastest, our 'unset' (= 10).\n"
        "  --partitions LIST  which partition shapes the mode decision may try,\n"
        "                     comma-separated: p8x8, p4x4, b8x8, i8x8, i4x4, plus\n"
        "                     none and all. Default follows --preset (medium =\n"
        "                     p8x8,b8x8,i8x8,i4x4; slow and above = all). p4x4\n"
        "                     needs p8x8 and i8x8 needs --transform-8x8; either\n"
        "                     missing is an error, not a silent drop.\n"
        "  --p-part-gate N    refuse the P 16x8, 8x16 and 8x8 searches when the\n"
        "                     16x16 result already costs under N lambda and the\n"
        "                     neighbourhood is homogeneous (default 400, 0 = off,\n"
        "                     same as --no-p-part-gate). Inert at --subme 9+.\n"
        "  --lr-settle N      end the lookahead's lowres block search at its own\n"
        "                     predictor when that already leaves under N of SAD\n"
        "                     per lowres pixel (default 0 = off). Reaches both\n"
        "                     the lookahead's field and the mb-tree walk.\n"
        "  --lr-subgate N     skip the lowres subpel refine when the whole-pel\n"
        "                     winner is under N of SATD per lowres pixel\n"
        "                     (default 0 = off).\n"
        "  --mbt-depfloor N   refuse the mb-tree deposit where the block's own\n"
        "                     propagation fraction is under N/256 (default 0 =\n"
        "                     off, deposit everything).\n"
        "  --b-preme-skip N   end a B macroblock at skip before its motion search\n"
        "                     when the skip residual is already under the cheapest\n"
        "                     rate any coded mode could pay: 0 off (the default),\n"
        "                     1 non-reference B slices, 2 also reference B's a\n"
        "                     propagation guard admits, 3 and 4 the same two with\n"
        "                     the bound read as an absolute distortion. Off\n"
        "                     because it costs up to 2.3% on a detailed 1080p\n"
        "                     clip at low rate. Inert at --subme 9+.\n"
        "  --rd-surv-rank N   RD at most N candidates per set in the B tournament,\n"
        "                     ranked by their screening cost (0 = off, the\n"
        "                     default: RD everything the score threshold admits).\n"
        "  --subpel N         refinement PATTERN, no x264 equivalent: 0 square,\n"
        "                     1 diamond, 2 capped diamond (default from --preset;\n"
        "                     medium = 2). Set by the preset separately from --subme.\n"
        "  --merange N        UMH search radius in integer pels (default 16, x264's\n"
        "                     --merange). Only UMH reads it; dia and hex do not.\n"
        "  --qcomp F          rate-curve compression 0..1 (default 0.6, x264's\n"
        "                     --qcomp). Sets the ABR curve and the mb-tree strength\n"
        "                     derived from it; the CRF and 2-pass curves carry their\n"
        "                     own and are NOT affected, unlike x264's.\n"
        "  --deadzone-inter N / --deadzone-intra N   luma quantisation deadzone,\n"
        "                     0..32, x264's flags and x264's inversion (defaults 21\n"
        "                     and 11). Passing either leaves the exact shipped\n"
        "                     expression for the 1/64 approximation of it, so even\n"
        "                     x264's defaults are not a no-op here, and it turns off\n"
        "                     the NEON quant path.\n");
    /* Fourth chunk, same 4095-byte reason as the splits above. */
    fprintf(stderr,
        "  --aq-mode N        AQ metric: 1 = log2-variance, 2 = autovariance\n"
        "                     (default; 1 under mb-tree's derived shape). N=0 is\n"
        "                     refused -- AQ off is --aq-strength 0 here.\n"
        "  --no-mbtree        skip mb-tree propagation entirely (x264's own policy\n"
        "                     at constant QP, where it is already the default here)\n"
        "  --no-dct-decimate  never drop a block of marginal coefficients\n"
        "  --no-fast-pskip    no cheap P_Skip pre-test; every P macroblock takes\n"
        "                     the full analysis path (slower, and it moves bits)\n"
        "  --no-psy           --psy-rd 0 --psy-trellis 0, as x264 spells it\n"
        "  --no-asm           force every scalar C path (no NEON)\n"
        "  --ipratio F / --pbratio F   2-PASS ONLY: the I-to-P and P-to-B qscale\n"
        "                     factors of the offline allocator (x264's numbers and\n"
        "                     x264's defaults, 1.4 and 1.3). The single-pass modes\n"
        "                     carry their own I/P/B anchoring and do not read these.\n"
        "  --cplxblur F / --qblur F    2-PASS ONLY: the allocator's complexity and\n"
        "                     qscale blur radii in frames (defaults 20 and 0)\n"
        "  (--subme/--subpel override the preset; --merange, --qcomp, --aq-mode,\n"
        "   the --no-* switches above, the ratio pair, the blur pair and the\n"
        "   deadzone pair reach the encoder through the Y264_* variable they were\n"
        "   promoted from, which still wins if it is set in the environment.)\n"
        "  --cabac / --cavlc  entropy coder (default CABAC = x264 medium)\n"
        "  --ref N            reference frames (default 3 = x264 medium)\n"
        "  --bframes N        consecutive B frames (default 3 = x264 medium)\n"
        "  --transform-8x8 / --no-transform-8x8   8x8 transform+intra (default on,\n"
        "                     High profile; --no- for Baseline/Main)\n"
        "  --cqm MODE         quant matrices: flat (default) or jvt (High profile)\n"
        "  --no-sei           suppress the settings SEI (emitted by default, x264-style)\n");
    /* Fifth chunk, same 4095-byte reason as the splits above. */
    fprintf(stderr,
        "  --deblock A:B      in-loop deblocking filter offsets, each -6..6, in the\n"
        "                     slice header's own div2 units (default 0:0)\n"
        "  --no-deblock       no in-loop deblocking at all\n"
        "  --b-pyramid MODE   none | normal (default). none codes a flat B run.\n"
        "                     strict is not implemented and is refused.\n"
        "  --no-weightb / --weightb   implicit weighted biprediction on B slices\n"
        "                     (on by default)\n"
        "  --weightp N        explicit weighted prediction on P slices: 0 off,\n"
        "                     1 one weight per reference from the frame's DC ratio\n"
        "                     (default), 2 the refined weight plus a duplicate\n"
        "                     reference slot, so the choice is per macroblock.\n"
        "                     2 needs --ref 2 or more and is refused with --tff/--bff.\n"
        "  --constrained-intra  intra prediction in P and B slices reads no\n"
        "                     inter-coded neighbour, so an intra macroblock decodes\n"
        "                     from intra data alone. Error resilience; costs bits.\n"
        "                     Off by default.\n"
        "  --chroma-qp-offset N   PPS chroma_qp_index_offset, -12..12 (default 0).\n"
        "                     Reaches the quantiser and the deblock chroma edge QP.\n"
        "  --qpmin N / --qpmax N  bounds on the coded QP the rate control may pick\n"
        "                     (defaults 0 and 51)\n"
        "  --qpstep N         largest QP move between consecutive frames of one\n"
        "                     type (default 4)\n"
        "  --vbv-init F       initial VBV occupancy: <=1 a fraction of\n"
        "                     --vbv-bufsize, above 1 kbit. Default full.\n"
        "  --crf-max Q        ceiling on the QP the VBV may raise a --crf frame\n"
        "                     to. Needs --crf and a VBV. The buffer is allowed to\n"
        "                     under-run rather than starve the frame: a quality\n"
        "                     floor, bought with the buffer model.\n"
        "  --ratetol F        how far ABR may drift from its target before the\n"
        "                     correction answers, as a fraction of the bitrate\n"
        "                     (default 1.0). Smaller tracks the rate closer and\n"
        "                     swings the quality more; inf takes the term out.\n"
        "  --mvrange N        vertical motion-vector range in luma samples. Default\n"
        "                     is the level's Table A-1 bound; only a value TIGHTER\n"
        "                     than the level's has any effect.\n"
        "  --sps-id N         seq_parameter_set_id, 0..31 (default 0)\n"
        "  --slices N         cut each picture into N independently decodable\n"
        "                     slices on macroblock-row boundaries (default 1).\n"
        "                     A decoder can resynchronise at any of them, and each\n"
        "                     is its own NAL. The in-loop filter is NOT cut: the\n"
        "                     picture is deblocked whole, so slice edges do not\n"
        "                     show. Costs bits, buys loss resilience and decoder\n"
        "                     parallelism; it is not a threading knob here.\n"
        "  --profile NAME     baseline | main | high | high10 | high422 | high444.\n"
        "                     A CONSTRAINT, not a label: a tool the PRESET chose is\n"
        "                     narrowed to fit, a tool you NAMED is refused, and a\n"
        "                     profile the content cannot fit is refused. Default is\n"
        "                     derived from the tools and the content.\n"
        "  (the flags below add signalling only: no sample moves.)\n");
    /* Sixth chunk, same 4095-byte reason as the splits above. */
    fprintf(stderr,
        "  --aud              an access unit delimiter opens every access unit\n"
        "  --pic-struct       VUI pic_struct_present_flag and a pic_timing SEI\n"
        "                     per picture (value 0, a progressive frame)\n"
        "  --frame-packing N  frame_packing_arrangement_type 0..7: 0 checkerboard,\n"
        "                     1 column, 2 row, 3 side-by-side, 4 top-bottom,\n"
        "                     5 frame alternation, 6 2D, 7 tile\n"
        "  --cll MAX,AVG      content light level SEI, cd/m^2\n"
        "  --mastering-display G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)\n"
        "                     mastering display colour volume SEI. Chromaticity in\n"
        "                     0.00002 units, luminance in 0.0001 cd/m^2. G,B,R is\n"
        "                     the spec order, not the R,G,B you would write.\n"
        "  --alternative-transfer <code|name>   the transfer a display should\n"
        "                     prefer over the VUI's (the names --transfer takes)\n"
        "  --overscan undef|show|crop           VUI overscan_info\n"
        "  --videoformat component|pal|ntsc|secam|mac|undef   VUI video_format\n"
        "  --nal-hrd vbr|cbr  write the buffer model INTO the stream: hrd_parameters\n"
        "                     in the VUI, a buffering_period SEI at every IDR and a\n"
        "                     pic_timing SEI per picture, so a receiver can verify\n"
        "                     the buffer instead of trusting it. Needs --vbv-maxrate\n"
        "                     and --vbv-bufsize. vbr declares a schedule the channel\n"
        "                     may pause on, and moves no sample and no slice bit;\n"
        "                     cbr declares a channel that never pauses, so each\n"
        "                     access unit is padded up to the rate with filler.\n"
        "  --filler / --no-filler   pad (or refuse to pad) access units to the\n"
        "                     constant rate. Only under --nal-hrd cbr, where it is\n"
        "                     already on; --no-filler there gives the cbr headers\n"
        "                     over an unpadded stream, whose buffer a conforming\n"
        "                     decoder may overflow. For measuring, not for shipping.\n"
        "  --stitchable       size the DECLARED DPB from the level rather than\n"
        "                     from --ref/--bframes, so two encodes at the same\n"
        "                     geometry carry the same SPS. It raises the declared\n"
        "                     level, and with it the MV range, so it does move bits.\n"
        "  --fake-interlaced  declare a sequence that may carry fields\n"
        "                     (frame_mbs_only_flag 0) while coding frames only\n"
        "  --tff / --bff      code each frame as two FIELD pictures, top field\n"
        "                     first or bottom field first (PAFF). 4:2:0 only;\n"
        "                     an interlaced Y4M (It/Ib) picks\n"
        "                     the order on its own unless one of these says\n"
        "                     otherwise. --no-interlaced codes fields as frames.\n"
        "  --sar W:H          sample aspect ratio (e.g. 16:11; default square/unspecified)\n"
        "  --level L          force H.264 level (e.g. 3.1 or 40; default = auto from res/fps/DPB)\n"
        "  --cut-split             pre-scan the input and put GOP boundaries on scene cuts (changes the stream)\n"
        "  --shot-crf              per-shot CRF offsets from the shot table (implies --cut-split; Y264_SHOT_QCOMP 0.6, _CLAMP 4, _MIN 24)\n"
        "  --shot-table            with --cut-split: print the shot table (per-shot costs) as JSON on stderr\n"
        "  --plan FILE             zones, one per line: \"first last [idr] [qp+N|qp-N]\" (frames from 0, inclusive);\n"
        "                          idr forces a keyframe at first (and a GOP boundary), qp offsets every frame in range\n"
        "  --qpfile FILE           per-frame forces, one line per frame: \"<frame> <type> <qp>\" (frames from 0).\n"
        "                          Type I or K = IDR, P = anchor, B or b = B frame, - = leave the type alone;\n"
        "                          QP is ABSOLUTE 0..51, or -1 to leave it to the rate control. i (non-IDR I)\n"
        "                          is refused: it needs an open GOP. Unlisted frames are free.\n"
        "  --gop-threads K         pin every GOP instance's frame threads to K (a shot then re-encodes byte-identically alone)\n"
        "  --segment-out PATTERN   also write each GOP to its own file, PATTERN with a %%d for the GOP index\n"
        "  --frame-stats FILE      one JSON line per coded frame: frame, gop, type, idr, ref, qp, bytes, k\n"
        "  --range full|limited   VUI colour range (the Y4M XCOLORRANGE tag sets it too)\n"
        "  --colorprim, --transfer, --colormatrix <code|name>  VUI colour description (H.273 codes or\n"
        "                          bt709 bt2020 bt601 smpte170m bt470bg srgb smpte2084 arib-std-b67)\n"
        "  --chromaloc 0..5        VUI chroma sample location\n"
        "  --output-depth N   code at 8 or 10 bits (default: the input's). 10 on\n"
        "                     8-bit input upshifts every sample by 2 and codes\n"
        "                     High 10; 8 on 10-bit input is refused, since that\n"
        "                     is a conversion and ffmpeg owns those.\n"
        "  --dump-recon PATH  write the encoder's reconstruction as Y4M\n"
        "  --version          print version and exit\n");
}

static int read_line(FILE *f, char *buf, int cap);

/* Collect per-frame reconstructions in display order for --dump-recon. B-frames
 * reorder coding vs display, so one encode emits several frames out of order;
 * the callback stashes each by its display index and we write them in order. */
struct recon_dump {
    int w, h;
    uint8_t **frames;           /* frames[disp] = tight YUV, or NULL */
    int cap, count;
};
static void recon_dump_cb(void *ud, const yah264_picture_t *rec, int disp, int depth)
{
    struct recon_dump *rd = ud;
    if (disp >= rd->cap) {
        int nc = rd->cap ? rd->cap : 16;
        while (nc <= disp) nc *= 2;
        rd->frames = realloc(rd->frames, (size_t)nc * sizeof(uint8_t *));
        for (int i = rd->cap; i < nc; i++) rd->frames[i] = NULL;
        rd->cap = nc;
    }
    size_t ys = (size_t)rd->w * rd->h, cs = (size_t)(rd->w / g_sub_w) * (rd->h / g_sub_h);
    /* The library that produced this recon names its own sample width, so a
     * dumper registered once serves either encoder. */
    size_t sz = depth > 8 ? 2 : 1;
    uint8_t *buf = malloc((ys + 2 * cs) * sz), *dst = buf;
    for (int p = 0; p < 3; p++) {
        int pw = p ? rd->w / g_sub_w : rd->w, ph = p ? rd->h / g_sub_h : rd->h;
        const uint8_t *src = rec->plane[p];
        for (int y = 0; y < ph; y++) {
            memcpy(dst, src + (size_t)y * (size_t)rec->stride[p] * sz,
                   (size_t)pw * sz);
            dst += (size_t)pw * sz;
        }
    }
    free(rd->frames[disp]);
    rd->frames[disp] = buf;
    if (disp + 1 > rd->count) rd->count = disp + 1;
}

/* --- GOP-parallel multithreaded encoding ---
 *
 * Each GOP (an IDR followed by keyint-1 P frames) is independent, so it can be
 * encoded on its own core by its own encoder instance, and GOP outputs are
 * written in order.
 *
 * A GOP's bytes do not depend on WHICH thread encodes it. They CAN depend on
 * how many: --threads sets the per-worker frame-thread share k, and k=1 vs
 * k>=2 take different in-frame paths. Thread-count invariance is NOT offered
 * here (docs/advantages.md), and x264 does not offer it either. What holds is
 * that the same input,
 * config AND thread count always give the same output -- anything else would
 * be a race. */

typedef struct { uint8_t *y, *u, *v; } frame_t;

/* --- the streaming frame window ------------------------------------------
 *
 * Reading the input whole before a worker starts makes the resident set the
 * whole decoded clip -- 501 GiB for a two-hour 1080p title, and a measured
 * 447 MB against x264's 158 MB on 180 frames of samsung_720p. What a
 * worker set can actually see at one moment is g GOPs of at most keyint frames,
 * so the window the path needs is g * keyint plus the read-ahead that keeps the
 * next worker fed. Frames past that are retired: a GOP's frames are dead once
 * its worker publishes, which needs no per-frame refcount.
 *
 * Slots live in fixed segments, never in one realloc'd array: the reader grows
 * the store while workers index it, and a realloc would move the array out from
 * under a worker mid-GOP. A segment is 512 slots (12 KiB of pointers), and the
 * segment table is allocated once -- 33.5M frames, 387 hours at 24 fps. The
 * pixel buffers inside a segment are freed on retire; the segments themselves
 * are not, because at 12 KiB each even a two-hour title's worth is 4 MB. */
#define FS_SEG_SH  9
#define FS_SEG_N   (1 << FS_SEG_SH)
#define FS_SEG_MAX 65536

typedef struct {
    const yah264_param_t *param;
    int keyint, width, height;
    /* Chroma geometry of the frames above, from the Y4M C tag. The store
 * carries it rather than assuming 4:2:0 -- that assumption was the one
 * thing forcing non-4:2:0 input onto the serial encoder. */
    int csp, sub_w, sub_h;
    uint8_t **gop_data;
    size_t   *gop_size;
    /* gop_start[g] is the first frame of GOP g, gop_start[n_gops] is n_frames.
 * Arithmetic by default (g*keyint); cut-aware
 * under Y264_CUT_SPLIT. Worker slicing reads only this, so the two paths
 * differ in the array's contents and nowhere else. */
    int *gop_start;
    int n_gops;
    int next_gop;
    pthread_mutex_t lock;
    /* Non-NULL = static assignment: gop_owner[i] names the worker that codes
 * GOP i, and wparam[w] carries that worker's own frame_threads share. */
    const yah264_param_t *wparam;
    int *gop_owner;
    /* Pull-queue case (more GOPs than workers). gop_order[] is the order the
 * queue hands GOPs out in -- longest first -- and gop_k[i] is GOP i's own
 * wavefront share, since the worker opens a fresh encoder per GOP and a
 * 250-frame GOP and a 25-frame one do not want the same width. Both NULL
 * selects the flat pull queue in GOP order. */
    const int *gop_order;
    const int *gop_k;
    double *gop_rf;               /* per-GOP CRF from the shot plan (--shot-crf), or NULL */
    char   **gop_fst;             /* --frame-stats: each GOP's JSON lines (published with gop_data) */
    int      want_fst;            /* --frame-stats was asked for, so gop_fst is grown with the table */
    /* 2-pass. Every worker is its own encoder, so both halves of the stats
 * round-trip are split along the GOP boundaries: in pass 1 each GOP writes
 * its own file (they cannot share one -- they would truncate each other),
 * and in pass 2 each GOP reads back only its own records. gop_stats[g] is
 * that per-GOP path, NULL for a GOP with no stats. gop_target[g] is the
 * pass-2 budget in bits; 0 leaves the encoder to derive it. */
    char **gop_stats;
    const double *gop_target;

    /* --- streaming state, all of it under `lock` ---
 *
 * seg[i >> FS_SEG_SH][i & (FS_SEG_N-1)] is frame i. n_read is how many
 * frames the reader has published; live is how many are allocated and not
 * yet retired, and the reader blocks while live >= window. gops_final says
 * the GOP table has stopped growing, which is true from the start whenever
 * the frame count was known up front. */
    frame_t **seg;
    int n_read, live, window;
    /* Workers currently blocked on a frame the reader has not published. The
 * reader ignores the window while this is non-zero, which is what makes the
 * window a READ-AHEAD budget rather than a correctness bound: whatever it is
 * set to, a starving worker always gets its frame. */
    int waiting;
    int max_live;                   /* high-water mark, for Y264_STREAM_STAT */
    size_t held, max_held;          /* published-but-unwritten output bytes */
    int eof, rerr, abort_;
    int gops_final, gops_cap;
    unsigned char *gop_done;
    /* ABR carry (Y264_RC_CARRY, default on): each GOP imports the controller
 * state exported by the GOP handed out W places before it (W = workers),
 * which is always complete by then, so the chain is a function of the
 * hand-out order alone and the bits stay run-to-run deterministic for a
 * given thread count. rc_state[g] / rc_ready[g] are per GOP; pull_gop[k]
 * is the k-th GOP handed out (pull-queue mode). */
    int nworkers;
    yah264_rc_state_t *rc_state;
    unsigned char *rc_ready;
    int *pull_gop;
    pthread_cond_t cv_space;        /* reader waits: a GOP retired */
    pthread_cond_t cv_ready;        /* workers wait: frames or GOPs arrived */
    pthread_cond_t cv_emit;         /* writer waits: the next GOP published */
} gop_job_t;

typedef struct { gop_job_t *j; int wid; } gop_arg_t;

/* Frames of read-ahead the window carries per in-flight GOP. It is read-ahead
 * and nothing else -- a worker retires each frame as it feeds it -- so the
 * figure answers "how far ahead of a consumer should the reader be allowed to
 * run", not "how much does a worker hold". 16 covers a page-cache read (0.3 ms
 * at 720p) against a frame's encode (tens of ms) with two orders to spare, and
 * the starvation valve means an underestimate costs latency, never progress. */
static long stream_readahead(void)
{
    const char *v = getenv("Y264_STREAM_READAHEAD");
    long ra = v ? atol(v) : 0;
    return ra > 0 ? ra : 16;
}

/* Frame i's slot, allocating its segment on first touch. Caller holds the lock
 * whenever this can allocate (the reader); a worker only ever reads a slot the
 * reader published before releasing the lock, so the mutex carries the
 * ordering and the pixel pointers need no atomics of their own. */
static frame_t *fs_slot(gop_job_t *j, int i)
{
    int s = i >> FS_SEG_SH;
    if (!j->seg[s]) j->seg[s] = calloc(FS_SEG_N, sizeof(frame_t));
    return j->seg[s] ? &j->seg[s][i & (FS_SEG_N - 1)] : NULL;
}

/* Retire [start, end): the frames of a GOP whose worker has published. Under
 * the lock, so the count the reader blocks on moves with the free. */
static void fs_retire(gop_job_t *j, int start, int end)
{
    for (int i = start; i < end; i++) {
        frame_t *f = &j->seg[i >> FS_SEG_SH][i & (FS_SEG_N - 1)];
        free(f->y); free(f->u); free(f->v);
        f->y = f->u = f->v = NULL;
        j->live--;
    }
}

/* Append a GOP boundary and grow the per-GOP output arrays with it. Only the
 * unknown-frame-count path calls this; when n is known the table is built whole
 * before the reader starts. Under the lock. */
static int gop_push(gop_job_t *j, int end)
{
    if (j->n_gops + 2 > j->gops_cap) {
        int nc = j->gops_cap ? j->gops_cap * 2 : 64;
        int *gs = realloc(j->gop_start, (size_t)(nc + 1) * sizeof(int));
        uint8_t **gd = realloc(j->gop_data, (size_t)nc * sizeof(uint8_t *));
        size_t *gz = realloc(j->gop_size, (size_t)nc * sizeof(size_t));
        unsigned char *gk = realloc(j->gop_done, (size_t)nc);
        yah264_rc_state_t *rs = realloc(j->rc_state, (size_t)nc * sizeof(*rs));
        unsigned char *rr = realloc(j->rc_ready, (size_t)nc);
        int *pg = realloc(j->pull_gop, (size_t)nc * sizeof(int));
        /* Grown with the rest rather than beside it: --frame-stats reached this
 * path with its array never allocated, because the array is built only
 * where the whole-input scan builds the GOP table. The symptom was an
 * empty stats file and no diagnostic. */
        char **gf = j->want_fst ? realloc(j->gop_fst, (size_t)nc * sizeof(char *)) : NULL;
        if (!gs || !gd || !gz || !gk || !rs || !rr || !pg || (j->want_fst && !gf)) return -1;
        if (gs) j->gop_start = gs;
        if (gd) j->gop_data = gd;
        if (gz) j->gop_size = gz;
        if (gk) j->gop_done = gk;
        if (gf) j->gop_fst = gf;
        j->rc_state = rs; j->rc_ready = rr; j->pull_gop = pg;
        for (int i = j->gops_cap; i < nc; i++) {
            j->gop_data[i] = NULL; j->gop_size[i] = 0; j->gop_done[i] = 0;
            if (j->gop_fst) j->gop_fst[i] = NULL;
            memset(&j->rc_state[i], 0, sizeof j->rc_state[i]); j->rc_ready[i] = 0; j->pull_gop[i] = -1;
        }
        j->gops_cap = nc;
    }
    j->gop_start[++j->n_gops] = end;
    return 0;
}

/* Longest GOP first, index breaking ties -- a total order, so the queue hands
 * work out in the same sequence on every run and at every thread count. */
typedef struct { int len, idx; } gop_len_t;

static int gop_len_cmp(const void *a, const void *b)
{
    const gop_len_t *x = a, *y = b;
    if (x->len != y->len) return x->len < y->len ? 1 : -1;
    return x->idx - y->idx;
}

static void buf_append(uint8_t **buf, size_t *sz, size_t *cap,
                       const uint8_t *data, size_t n)
{
    if (*sz + n > *cap) {
        while (*sz + n > *cap) *cap *= 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *sz, data, n);
    *sz += n;
}

/* Y264_RC_CARRY=0 disables the cross-GOP ABR carry (each GOP re-solves its
 * own rate problem from the seeds, the pre-2026-09-02 behaviour). */
static int rc_carry_on(void)
{
    static int v = -1;
    if (v < 0) { const char *s = getenv("Y264_RC_CARRY"); v = s ? (atoi(s) ? 1 : 0) : 1; }
    return v;
}

/* The carry arrays for a GOP count known up front (the growth path in
 * gop_push handles the streamed case). */
static void job_rc_alloc(gop_job_t *j, int n)
{
    j->rc_state = calloc((size_t)n, sizeof(*j->rc_state));
    j->rc_ready = calloc((size_t)n, 1);
    j->pull_gop = malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) j->pull_gop[i] = -1;
}


/* Name the calling thread so a profile attributes by role (E4: unnamed
 * threads made the per-thread counter split useless). Best effort, no
 * error path: a name is a diagnostic, never a dependency. */
static void y264_thread_name(const char *name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

static void *gop_worker(void *arg)
{
    gop_arg_t *a = arg;
    { char nm[24]; snprintf(nm, sizeof nm, "y264-gop%d", a->wid); y264_thread_name(nm); }
    gop_job_t *j = a->j;
    int W = j->width;                   /* the INPUT frame width; the coded one is p.width */
    int scan = 0;
    for (;;) {
        int g, start, end;
        pthread_mutex_lock(&j->lock);
        int k = -1;                             /* pull index (pull-queue mode) */
        if (j->gop_owner) {                     /* static: walk my own GOPs */
            while (scan < j->n_gops && j->gop_owner[scan] != a->wid) scan++;
            g = scan++;
        } else {
            k = j->next_gop++;
            g = k;
            if (g < j->n_gops && j->gop_order)
                g = j->gop_order[g];
            if (g < j->n_gops && j->pull_gop) j->pull_gop[k] = g;
        }
        /* Wait for the GOP to exist. It may not yet: a streamed input with no
 * length to read the count off publishes boundaries as it reads them,
 * so "past the end" and "not read yet" are the same index until EOF
 * settles it. Cannot deadlock -- the reader only blocks once the window
 * is full, and a full window is more than g complete GOPs, so a waiting
 * worker's index is always one the reader has already passed. */
        while (!j->abort_ && g >= j->n_gops && !j->gops_final)
            pthread_cond_wait(&j->cv_ready, &j->lock);
        if (j->abort_ || g >= j->n_gops) { pthread_mutex_unlock(&j->lock); break; }
        start = j->gop_start[g];
        end   = j->gop_start[g + 1];
        /* Wait for the GOP's FIRST frame, not for all of it. Blocking until
 * the whole slice is resident is what would force the
 * window to hold g whole GOPs; a worker consumes in display order and the
 * encoder copies each frame into its own lookahead ring before
 * encoder_encode returns, so one frame at a time is all it ever needs. */
        while (!j->abort_ && j->n_read <= start && !j->eof)
            pthread_cond_wait(&j->cv_ready, &j->lock);
        if (j->abort_) { pthread_mutex_unlock(&j->lock); break; }
        if (end > j->n_read && j->eof) end = j->n_read;  /* input ended short */
        pthread_mutex_unlock(&j->lock);
        if (end <= start) {                     /* nothing to code; still publish */
            pthread_mutex_lock(&j->lock);
            j->gop_done[g] = 1;
            if (j->rc_ready) j->rc_ready[g] = 1;   /* nothing exported; waiters move on */
            pthread_cond_broadcast(&j->cv_emit);
            pthread_mutex_unlock(&j->lock);
            continue;
        }

        yah264_param_t p = j->wparam ? j->wparam[a->wid] : *j->param;
        if (j->gop_k)
            p.frame_threads = j->gop_k[g];
        if (g_gop_threads > 0)
            p.frame_threads = g_gop_threads;  /* --gop-threads: pinned, so a shot re-encodes byte-identically */
        if (j->gop_rf)
            p.rc.rf = j->gop_rf[g];           /* --shot-crf: this GOP's shot CRF */
        if (j->gop_stats) {
            p.rc.stats = j->gop_stats[g];
            p.rc.tp_target_bits = j->gop_target ? j->gop_target[g] : 0.0;
        }
        /* Every GOP but the first is preceded by another in the output, so its
 * VBV buffer does not start full -- it starts wherever its predecessor
 * left one, and the encoder assumes the handoff level rather than a
 * buffer it has already spent. GOP 0 says nothing because a decoder's
 * buffer really is full there. Static in the GOP split, so it does not
 * depend on which worker coded the GOP or on how many threads ran. */
        if (g > 0 && p.rc.vbv_maxrate > 0 && p.rc.vbv_bufsize > 0)
            p.rc.vbv_seg_join = 1;
        /* The same handoff, asked of the HRD clock instead of the buffer. This
 * GOP opens a buffering period at its first picture, and that picture's
 * cpb_removal_delay counts ticks back to the PREVIOUS period, which is
 * the previous GOP's first picture -- two ticks per input frame between
 * them. Static in the GOP split, like the flag above, so it does not
 * depend on which worker coded which GOP. */
        if (g > 0 && p.nal_hrd)
            p.rc.hrd_bp_ticks = 2 * (j->gop_start[g] - j->gop_start[g - 1]);
        /* ...and tell the encoder that the count above is only meaningful if it
 * opens no buffering period of its own in between. Assume segmented
 * unless the split is FINAL and holds exactly one GOP: a streamed input
 * publishes boundaries as it reads them, so "one so far" is not "one". */
        if (p.nal_hrd)
            p.rc.hrd_segmented = (j->gops_final && j->n_gops == 1) ? 0 : 1;
        /* ABR carry: the predecessor is the GOP handed out W places earlier
 * (pull mode: pull index k - W; static mode: this worker's previous
 * GOP). Wait for its export, then credit the GOPs between at target. */
        yah264_rc_state_t carry;
        int carry_ahead = 0;
        memset(&carry, 0, sizeof carry);
        if (p.rc.bitrate > 0 && rc_carry_on() && j->rc_state) {
            int pred = -1, ahead = 0;
            if (k >= 0) {
                if (k >= j->nworkers) {
                    pred = j->pull_gop[k - j->nworkers];
                    for (int i = k - j->nworkers + 1; i < k; i++) {
                        int gi = j->pull_gop[i];
                        if (gi >= 0) ahead += j->gop_start[gi + 1] - j->gop_start[gi];
                    }
                }
            } else {
                for (int i = g - 1; i >= 0; i--)
                    if (j->gop_owner[i] == a->wid) { pred = i; break; }
                for (int i = pred + 1; i < g && pred >= 0; i++)
                    ahead += j->gop_start[i + 1] - j->gop_start[i];
            }
            if (pred >= 0) {
                pthread_mutex_lock(&j->lock);
                while (!j->abort_ && !j->rc_ready[pred])
                    pthread_cond_wait(&j->cv_emit, &j->lock);
                carry = j->rc_state[pred];
                pthread_mutex_unlock(&j->lock);
                carry_ahead = ahead;
            }
        }
        yah264_encoder_t *e = g_api->encoder_open(&p);
        if (e && carry.valid) g_api->rc_import(e, &carry, carry_ahead);
        apply_video_signal(e);
        if (e) apply_zones(e, start, end);
        if (e) apply_forces(e, start, end);
        FILE *fst = NULL; char *fst_buf = NULL; size_t fst_len = 0;
        yah264_frame_stats_t *st_by = NULL;
        if (g_frame_stats && e) {
            fst = open_memstream(&fst_buf, &fst_len);
            st_by = calloc((size_t)(end - start), sizeof *st_by);
        }
        size_t cap = 1 << 16, sz = 0;
        uint8_t *buf = malloc(cap);
        yah264_nal_t *nal;
        int cnt;

        if (g_api->encoder_headers(e, &nal, &cnt) == 0)
            for (int i = 0; i < cnt; i++)
                buf_append(&buf, &sz, &cap, nal[i].payload, nal[i].size);

        for (int i = start; i < end; i++) {
            /* Wait for this frame, then free it as soon as the encoder has
 * taken it. yah264_encoder_encode pads the picture into the
 * lookahead ring slot (or into e->plane with the window off) before
 * it returns, so the frame is dead on return -- the same ownership
 * rule as a GOP-wide retire, applied at the granularity the
 * consumer actually works at. */
            pthread_mutex_lock(&j->lock);
            while (!j->abort_ && j->n_read <= i && !j->eof) {
                j->waiting++;
                pthread_cond_broadcast(&j->cv_space);   /* the reader may be parked */
                pthread_cond_wait(&j->cv_ready, &j->lock);
                j->waiting--;
            }
            int have = !j->abort_ && i < j->n_read;
            pthread_mutex_unlock(&j->lock);
            if (!have) { end = i; break; }              /* aborted, or short input */

            const frame_t *f = &j->seg[i >> FS_SEG_SH][i & (FS_SEG_N - 1)];
            /* W/H are the INPUT frame; --crop-rect shows the encoder a window
 * into it at the same stride (see the serial path's copy). */
            yah264_picture_t pic;
            memset(&pic, 0, sizeof(pic));
            pic.csp = j->csp;
            /* The crop offset is in SAMPLES and the planes are void*, so the
 * pointer arithmetic is in BYTES and scales with the library's
 * sample size -- 2 at 10-bit. Strides stay in samples, which is
 * what yah264_picture_t documents them as. */
            pic.width = p.width; pic.height = p.height; pic.pts = i - start;
            int cstride = W / j->sub_w;
            size_t yoff = ((size_t)g_crop_t * W + g_crop_l) * (size_t)g_enc_sample_sz;
            size_t coff = ((size_t)(g_crop_t / j->sub_h) * cstride + g_crop_l / j->sub_w)
                        * (size_t)g_enc_sample_sz;
            pic.plane[0] = (uint8_t *)f->y + yoff; pic.stride[0] = W;
            pic.plane[1] = (uint8_t *)f->u + coff; pic.stride[1] = cstride;
            pic.plane[2] = (uint8_t *)f->v + coff; pic.stride[2] = cstride;
            if (g_api->encoder_encode(e, &nal, &cnt, &pic) >= 0) {
                for (int k = 0; k < cnt; k++)
                    buf_append(&buf, &sz, &cap, nal[k].payload, nal[k].size);
                if (fst) frame_stats_emit(fst, e, nal, cnt, st_by, end - start, start, g, p.frame_threads);
            } else if (g_nforces) {
                g_force_reject = 1;             /* the qpfile named something unplaceable */
                g_force_reject_base = start;
            }

            pthread_mutex_lock(&j->lock);
            fs_retire(j, i, i + 1);
            pthread_cond_broadcast(&j->cv_space);
            pthread_mutex_unlock(&j->lock);
        }
        for (;;) {                              /* flush (window + B reorder) */
            int fb = g_api->encoder_encode(e, &nal, &cnt, NULL);
            if (fb < 0 || (fb == 0 && cnt == 0))
                break;
            for (int k = 0; k < cnt; k++)
                buf_append(&buf, &sz, &cap, nal[k].payload, nal[k].size);
            if (fst) frame_stats_emit(fst, e, nal, cnt, st_by, end - start, start, g, p.frame_threads);
        }
        if (fst) { fclose(fst); free(st_by); }
        yah264_rc_state_t st;
        int have_st = g_api->rc_state(e, &st) == 0;
        g_api->encoder_close(e);
        /* Publish. Frames are retired one at a time as they are fed, so
 * there is nothing left of this GOP to free here. */
        pthread_mutex_lock(&j->lock);
        if (j->rc_state) { if (have_st) j->rc_state[g] = st; j->rc_ready[g] = 1; }
        j->gop_data[g] = buf;
        j->gop_size[g] = sz;
        if (j->gop_fst) j->gop_fst[g] = fst_buf; else free(fst_buf);
        j->gop_done[g] = 1;
        j->held += sz;
        if (j->held > j->max_held) j->max_held = j->held;
        pthread_cond_broadcast(&j->cv_space);
        pthread_cond_broadcast(&j->cv_emit);
        pthread_mutex_unlock(&j->lock);
    }
    /* A worker leaving may be the last one the writer was waiting on. */
    pthread_mutex_lock(&j->lock);
    pthread_cond_broadcast(&j->cv_emit);
    pthread_mutex_unlock(&j->lock);
    return NULL;
}

/* --- the reader thread ---------------------------------------------------
 *
 * Parses Y4M frames into the window and blocks when it is full. `stop_at` is
 * the frame count to read (0 = until EOF); `hdr_len` is the FRAME header length
 * the caller measured, or -1 if it did not, and a stream that changes it is an
 * error rather than a silent miscount -- the frame count was read off the file
 * length on the assumption that it does not change. */
typedef struct {
    gop_job_t *j;
    FILE *in;
    long stop_at;
    size_t ys, cs;              /* SAMPLES per plane, not bytes: the disk width
                                 * and the encoder's can differ (--output-depth). */
    int keyint, hdr_len, verify_eof;
} reader_arg_t;

static void reader_fail(gop_job_t *j)
{
    pthread_mutex_lock(&j->lock);
    j->rerr = 1; j->abort_ = 1; j->eof = 1; j->gops_final = 1;
    pthread_cond_broadcast(&j->cv_ready);
    pthread_cond_broadcast(&j->cv_emit);
    pthread_cond_broadcast(&j->cv_space);
    pthread_mutex_unlock(&j->lock);
}

static void *y4m_reader(void *arg)
{
    y264_thread_name("y264-y4m");
    reader_arg_t *r = arg;
    gop_job_t *j = r->j;
    char line[512];
    int n = 0;

    for (;;) {
        if (r->stop_at > 0 && n >= r->stop_at) break;
        int len = 0;
        if (!g_raw_input) {
            len = read_line(r->in, line, sizeof(line));
            if (len < 0) break;                         /* EOF */
            if (strncmp(line, "FRAME", 5) != 0) {
                fprintf(stderr, "yah264: expected FRAME header\n");
                reader_fail(j); return NULL;
            }
        } else {
            /* Raw input has no per-frame header, so EOF is only visible by
 * trying to read: peek one byte and put it back. */
            int c = fgetc(r->in);
            if (c == EOF) break;
            ungetc(c, r->in);
        }
        if (!g_raw_input && r->hdr_len >= 0 && len != r->hdr_len) {
            fprintf(stderr, "yah264: frame %d has a %d-byte FRAME header where "
                    "frame 0 had %d -- per-frame parameters make the frame count "
                    "unreadable from the file length\n", n, len, r->hdr_len);
            reader_fail(j); return NULL;
        }
        pthread_mutex_lock(&j->lock);
        while (j->live >= j->window && !j->waiting && !j->abort_)
            pthread_cond_wait(&j->cv_space, &j->lock);
        int stop = j->abort_;
        frame_t *slot = stop ? NULL : fs_slot(j, n);
        pthread_mutex_unlock(&j->lock);
        if (stop) break;
        if (!slot) { fprintf(stderr, "yah264: out of memory at frame %d\n", n);
                     reader_fail(j); return NULL; }

        frame_t f;
        f.y = malloc(r->ys * (size_t)g_enc_sample_sz);
        f.u = malloc(r->cs * (size_t)g_enc_sample_sz);
        f.v = malloc(r->cs * (size_t)g_enc_sample_sz);
        if (!f.y || !f.u || !f.v) {
            fprintf(stderr, "yah264: out of memory at frame %d\n", n);
            free(f.y); free(f.u); free(f.v); reader_fail(j); return NULL;
        }
        if (!read_plane(r->in, f.y, r->ys) ||
            !read_plane(r->in, f.u, r->cs) ||
            !read_plane(r->in, f.v, r->cs)) {
            fprintf(stderr, "yah264: short read on frame %d\n", n);
            free(f.y); free(f.u); free(f.v); reader_fail(j); return NULL;
        }

        pthread_mutex_lock(&j->lock);
        *slot = f;
        j->n_read = ++n;
        if (++j->live > j->max_live) j->max_live = j->live;
        /* Without a known frame count the GOP table is built as the frames
 * arrive: a GOP becomes dispatchable exactly when its last frame is
 * read, which is also what keeps the queue running forward. */
        if (!j->gops_final && n % r->keyint == 0 && gop_push(j, n) != 0) {
            pthread_mutex_unlock(&j->lock); reader_fail(j); return NULL;
        }
        pthread_cond_broadcast(&j->cv_ready);
        pthread_mutex_unlock(&j->lock);
    }

    if (r->verify_eof) {
        int more = g_raw_input ? (fgetc(r->in) != EOF)
                               : (read_line(r->in, line, sizeof(line)) >= 0);
        if (more) {
            fprintf(stderr, "yah264: input has more frames than its length implied "
                    "(read %d)\n", n);
            reader_fail(j); return NULL;
        }
    }
    pthread_mutex_lock(&j->lock);
    j->eof = 1;
    if (!j->gops_final) {
        if (n > (j->n_gops ? j->gop_start[j->n_gops] : 0)) gop_push(j, n);
        j->gops_final = 1;
    }
    pthread_cond_broadcast(&j->cv_ready);
    pthread_cond_broadcast(&j->cv_emit);
    pthread_mutex_unlock(&j->lock);
    return NULL;
}

/* --- 2-pass across GOP workers ---------------------------------------------
 *
 * Every other rate-control mode reaches the GOP-parallel path because none of
 * them carries state over a GOP boundary: ABR re-seeds abr_cum_target per
 * encoder, VBV re-primes its buffer full, CRF re-seeds its complexity
 * reference. Each GOP re-solves its own rate problem against the same target,
 * which is exactly why a GOP's bytes do not depend on which thread coded it.
 *
 * 2-pass is the one mode that does carry state, in both directions:
 *
 * pass 1 every worker would fopen(rc.stats, "w") on the SAME path and
 * truncate the others' lines -- last finisher wins.
 * pass 2 the reader slurps the whole file at encoder_open, sizes tp_target
 * from the whole stream's record count, and walks the records with a
 * bare tp_stats[tp_idx++] cursor that starts at 0. Every GOP would
 * plan itself against GOP 0's records and the entire clip's budget.
 *
 * So the exclusion at the call site is load-bearing, and the file format is
 * why: `type cplx bits qp` per record, matched to frames by POSITION alone,
 * with no frame index to seek on. Splitting it needs boundaries the file does
 * not have -- and they cannot be inferred by counting, because a frame dropped
 * on CAVLC overflow writes no record at all (rcp_drop, and the size == 0 early
 * return in w2_drain), so "records per GOP == frames per GOP" is not an
 * identity.
 *
 * The boundaries therefore get written down. Pass 1 gives each GOP its own
 * file and the merge stamps a marker ahead of each section:
 *
 * #gop <index> <first frame> <end frame> <records>
 *
 * Pass 2 splits on those markers, hands each worker its own section, and sizes
 * each worker's budget by that section's share of the global complexity sum --
 * NOT by its frame count. That distinction is the whole mode: an equal
 * per-frame budget per GOP would hit the target rate just as precisely while
 * refusing to move bits from an easy GOP to a hard one, which is the only
 * reason to run two passes.
 *
 * The markers are comments to the encoder's own reader, which skips them, so
 * one file still feeds the serial whole-stream solve unchanged -- that is what
 * keeps the conformance gate's 2-pass recon-match (serial pass 2, --dump-recon)
 * reading a stats file that a threaded pass 1 wrote. A file with no markers is
 * a serial pass 1's, and pass 2 stays on the serial path for it. */

#define TP_MARKER "#gop"

static char *tp_gop_path(const char *base, const char *tag, int g)
{
    size_t n = strlen(base) + 64;
    char *s = malloc(n);
    snprintf(s, n, "%s.%s%d.%ld", base, tag, g, (long)getpid());
    return s;
}

static void tp_free_paths(char **paths, int n, int unlink_them)
{
    if (!paths) return;
    for (int i = 0; i < n; i++) {
        if (!paths[i]) continue;
        if (unlink_them) unlink(paths[i]);
        free(paths[i]);
    }
    free(paths);
}

/* True if the stats file was written by a threaded pass 1, i.e. carries the GOP
 * markers pass 2 needs to split it. Called before the input is read, so it can
 * only look at the file. */
static int tp_stats_have_markers(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char ln[256];
    int found = 0;
    while (fgets(ln, sizeof ln, f)) {
        if (ln[0] == '\n' || ln[0] == '\0') continue;
        found = !strncmp(ln, TP_MARKER, strlen(TP_MARKER));
        break;                                  /* first real line decides */
    }
    fclose(f);
    return found;
}

/* Concatenate the per-GOP pass-1 files into the user's stats path, in GOP
 * order, each section preceded by its marker. Returns 0 on success. */
static int tp_merge_pass1(const char *out_path, char *const *gop_stats,
                          const int *gstart, int n_gops)
{
    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "yah264: cannot write stats file '%s'\n", out_path);
        return -1;
    }
    for (int g = 0; g < n_gops; g++) {
        FILE *in = gop_stats[g] ? fopen(gop_stats[g], "r") : NULL;
        char ln[256];
        int nrec = 0;
        if (in) {                               /* count first, to stamp it */
            while (fgets(ln, sizeof ln, in))
                if (ln[0] != '#' && ln[0] != '\n' && ln[0] != '\0') nrec++;
            rewind(in);
        }
        fprintf(out, TP_MARKER " %d %d %d %d\n", g, gstart[g], gstart[g + 1], nrec);
        if (!in) continue;
        while (fgets(ln, sizeof ln, in))
            if (ln[0] != '#' && ln[0] != '\n' && ln[0] != '\0') fputs(ln, out);
        fclose(in);
    }
    if (fclose(out) != 0) {
        fprintf(stderr, "yah264: error writing stats file '%s'\n", out_path);
        return -1;
    }
    return 0;
}

/* Split a marked stats file into one file per GOP and size each GOP's pass-2
 * budget by its share of the global complexity sum. Returns 0 on success, -1 if
 * the file's sections do not describe the GOP split pass 2 computed (different
 * --frames, --keyint or cut-split between the passes), which is unrecoverable
 * and must not be papered over -- positional stats applied to the wrong frames
 * is precisely the silent-garbage case this format change exists to stop. */
static int tp_split_pass2(const char *in_path, char **gop_stats, double *gop_target,
                          const int *gstart, int n_gops, int bitrate, double fps)
{
    FILE *in = fopen(in_path, "r");
    if (!in) {
        fprintf(stderr, "yah264: cannot read stats file '%s'\n", in_path);
        return -1;
    }
    double *w = calloc((size_t)n_gops, sizeof(double));
    long *rec = calloc((size_t)n_gops, sizeof(long));
    FILE **out = calloc((size_t)n_gops, sizeof(FILE *));
    long total_rec = 0;
    double total_w = 0.0;
    int cur = -1, bad = 0;
    char ln[256];

    while (fgets(ln, sizeof ln, in)) {
        if (ln[0] == '\n' || ln[0] == '\0') continue;
        if (!strncmp(ln, TP_MARKER, strlen(TP_MARKER))) {
            int g, s, e, nr;
            if (sscanf(ln + strlen(TP_MARKER), "%d %d %d %d", &g, &s, &e, &nr) != 4) {
                fprintf(stderr, "yah264: malformed GOP marker in '%s'\n", in_path);
                bad = 1; break;
            }
            if (g < 0 || g >= n_gops || s != gstart[g] || e != gstart[g + 1]) {
                fprintf(stderr, "yah264: stats file '%s' describes a different GOP "
                        "split than this pass (marker %d = frames %d..%d)%s\n",
                        in_path, g, s, e,
                        g >= 0 && g < n_gops ? "" : " -- GOP count differs");
                bad = 1; break;
            }
            cur = g;
            out[g] = fopen(gop_stats[g], "w");
            if (!out[g]) {
                fprintf(stderr, "yah264: cannot write '%s'\n", gop_stats[g]);
                bad = 1; break;
            }
            continue;
        }
        if (cur < 0) {                          /* records before any marker */
            fprintf(stderr, "yah264: stats file '%s' has records outside any GOP\n",
                    in_path);
            bad = 1; break;
        }
        int type, qp;
        double cplx, bits;
        if (sscanf(ln, "%d %lf %lf %d", &type, &cplx, &bits, &qp) != 4)
            break;                              /* malformed: stop, as the encoder does */
        fputs(ln, out[cur]);
        /* Not on the dispatch table on purpose: this is a pure function of a
         * stats record's (bits, qp) with the same constants in both libraries,
         * so the 8-bit copy is the 10-bit answer. It stays off the table so
         * the table only carries calls where the depth can matter. */
        double q = yah264_2pass_stat_weight(bits, qp);
        w[cur] += q; total_w += q;
        rec[cur]++; total_rec++;
    }
    fclose(in);
    for (int g = 0; g < n_gops; g++)
        if (out[g]) fclose(out[g]);

    /* A section that opened but got no records leaves an empty file, which the
 * encoder reads as "no stats" and codes at base QP. Say so rather than let
 * it look like a plausible encode. */
    if (!bad)
        for (int g = 0; g < n_gops; g++)
            if (!rec[g]) {
                fprintf(stderr, "yah264: stats file '%s' has no records for GOP %d "
                        "(frames %d..%d)\n", in_path, g, gstart[g], gstart[g + 1]);
                bad = 1;
            }

    if (bad || total_rec == 0 || total_w <= 0.0) {
        free(w); free(rec); free(out);
        return -1;
    }

    /* The whole clip's budget, computed exactly as the serial reader computes
 * it -- from the RECORD count, not the frame count, so a dropped frame
 * moves both paths' targets the same way. Each GOP then takes the share its
 * own complexity earns. */
    double total_target = (double)(bitrate > 0 ? bitrate : 1000) * 1000.0
                        * (double)total_rec / fps;
    for (int g = 0; g < n_gops; g++)
        gop_target[g] = total_target * w[g] / total_w;

    free(w); free(rec); free(out);
    return 0;
}

/* ---- input memory guard -------------------------------------------------
 *
 * encode_threaded holds the WHOLE clip resident: the GOP workers index frames[]
 * by display order, so no frame can be retired until every worker is past it.
 * At 1080p 4:2:0 8-bit that is 3.0 MiB a frame, i.e. 537 GiB for a two-hour
 * 24 fps title and 45 GiB for ten minutes -- and the failure mode is the OOM
 * killer, which tells the user nothing. Refuse first, and name the number.
 *
 * Measured on this tree (sintel_720p, --threads 1 --keyint 10000, peak RSS at
 * 100 vs 400 frames): 1.442 MiB resident per 1280x720 frame against 1.3824 MiB
 * of payload, so the array itself costs 1.04x its arithmetic -- three mallocs a
 * frame, page-rounded. Sitting on top of it, and NOT in the arithmetic below:
 * a fixed per-worker encoder cost (~330 MiB for one 1080p worker at --threads 1,
 * ~+50 MiB for a second) and the entire compressed output, which this path also
 * buffers to the end. Both live inside the headroom the share leaves. */
#define Y264_INPUT_MEM_SHARE 0.50

/* Total physical RAM, or 0 if it cannot be determined. Deliberately NOT a
 * free-page count: on macOS most of what vm_stat calls inactive, speculative or
 * purgeable is reclaimable on demand, so gating on free pages would refuse
 * encodes that run fine, and the number moves under you between two runs of the
 * same command. Physical RAM is stable, reproducible, and the figure a user can
 * check; the share above is what keeps the encode off the pager. */
static uint64_t y264_phys_mem(void)
{
#if defined(__APPLE__)
    uint64_t v = 0; size_t sz = sizeof v;
    if (sysctlbyname("hw.memsize", &v, &sz, NULL, 0) == 0) return v;
    return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) return (uint64_t)pages * (uint64_t)psz;
    return 0;
#endif
}

/* Bytes the frame array is allowed to occupy. 0 means no guard, which happens
 * only when the platform will not say how much RAM it has. */
static uint64_t y264_input_budget(int *from_env)
{
    const char *ev = getenv("Y264_MAX_INPUT_MB");
    *from_env = 0;
    if (ev) {
        double mb = atof(ev);
        if (mb > 0) { *from_env = 1; return (uint64_t)(mb * 1048576.0); }
    }
    uint64_t phys = y264_phys_mem();
    return phys ? (uint64_t)((double)phys * Y264_INPUT_MEM_SHARE) : 0;
}

static double y264_gib(uint64_t b) { return (double)b / (1024.0 * 1024.0 * 1024.0); }

/* Sizes here span megabytes to hundreds of gigabytes, and a limit that reads
 * "0.1 GiB" is a limit nobody can check against. Scale the unit. */
static const char *y264_hsize(char *buf, size_t cap, uint64_t bytes)
{
    double gib = y264_gib(bytes);
    if (gib >= 1.0) snprintf(buf, cap, "%.1f GiB", gib);
    else            snprintf(buf, cap, "%.0f MiB", (double)bytes / 1048576.0);
    return buf;
}

/* The two lines every refusal ends with: what the limit is, and what to do. */
static void y264_mem_advice(uint64_t budget, int from_env, uint64_t per_frame,
                            int W, int H)
{
    char lim[32], ph[32];
    y264_hsize(lim, sizeof lim, budget);
    y264_hsize(ph, sizeof ph, y264_phys_mem());
    if (from_env)
        fprintf(stderr, "yah264:   limit %s (Y264_MAX_INPUT_MB), "
                "%.2f MiB resident per %dx%d frame, so %llu frame(s) fit\n", lim,
                (double)per_frame / 1048576.0, W, H,
                (unsigned long long)(budget / per_frame));
    else
        fprintf(stderr, "yah264:   limit %s (%.0f%% of %s physical), "
                "%.2f MiB resident per %dx%d frame, so %llu frame(s) fit\n", lim,
                Y264_INPUT_MEM_SHARE * 100.0, ph,
                (double)per_frame / 1048576.0, W, H,
                (unsigned long long)(budget / per_frame));
    fprintf(stderr, "yah264:   encode a segment with --frames, split the input, "
            "or raise Y264_MAX_INPUT_MB\n");
}

/* Frames still ahead of us in a regular file. Y4M frames are fixed size once the
 * stream header is past, so a seekable input's length IS its frame count and we
 * can refuse before allocating anything. Returns -1 for a pipe or a stream whose
 * length we cannot know, which is the honest answer -- that case is caught by the
 * running check in the read loop instead. Per-frame divisor uses the shortest
 * legal frame header ("FRAME\n"), so a stream with per-frame parameters projects
 * a few frames high out of thousands; the share leaves room for that. */
static long y264_projected_frames(FILE *in, uint64_t frame_stream_bytes)
{
    struct stat st;
    int fd = fileno(in);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    off_t pos = ftello(in);
    if (pos < 0 || st.st_size <= pos || frame_stream_bytes == 0) return -1;
    return (long)((uint64_t)(st.st_size - pos) / frame_stream_bytes);
}

/* The EXACT frame count of a seekable Y4M, or -1 if it cannot be had.
 *
 * This is what keeps streaming from costing anything. Every scheduling decision
 * on this path -- the GOP split, g, k, the frame-weighted and longest-first
 * heuristics -- is a function of the frame count n, and a reader that discovers
 * n only at EOF loses all of them. But a Y4M frame is a fixed-size record once
 * the stream header is past, so for a regular file the LENGTH is the count: read
 * the first FRAME header, seek back, and check that what remains divides by
 * header+payload exactly. When it does, n is known before a byte of pixel data
 * is read and the schedule is the one a buffered path would compute. When it does
 * not (a pipe, or per-frame parameters), the caller falls back to discovering
 * the split as it reads -- see the note in encode_threaded on why that fallback
 * lands on the same schedule anyway.
 *
 * The divisibility check is not a proof that every header is 6 bytes, so the
 * reader also checks each one against *hdr_len and fails loudly on a mismatch
 * rather than encoding a miscounted clip. */
static long y264_exact_frames(FILE *in, uint64_t payload, int *hdr_len)
{
    struct stat st;
    char line[512];
    int fd = fileno(in);
    *hdr_len = -1;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || payload == 0)
        return -1;
    off_t pos = ftello(in);
    if (pos < 0 || st.st_size <= pos) return -1;
    /* Raw input has no header to measure and no header to divide out, so the
 * count is just the length: cleaner than the Y4M case, and exact for the
 * same reason. A trailing partial frame is a truncated file; refuse the
 * count rather than encoding whatever is there. */
    if (g_raw_input) {
        uint64_t span = (uint64_t)(st.st_size - pos);
        if (span % payload) return -1;
        *hdr_len = 0;
        return (long)(span / payload);
    }
    int len = read_line(in, line, sizeof(line));
    if (fseeko(in, pos, SEEK_SET) != 0) return -1;
    if (len < 5 || strncmp(line, "FRAME", 5) != 0) return -1;
    uint64_t rec = (uint64_t)len + 1 + payload;
    uint64_t span = (uint64_t)(st.st_size - pos);
    if (span % rec) return -1;
    *hdr_len = len;
    return (long)(span / rec);
}

/* Stream a Y4M through a bounded window, encoding GOPs in parallel across
 * `nthreads` cores and writing the stream in order. Returns 0 on success. */
/* encode_threaded's "this shape is not mine" return, handled by its one caller
 * before a frame has been read. Not an error and not a frame count, so any
 * value outside both is free; -2 keeps -1 meaning failure. */
#define Y264_GO_SERIAL (-2)

static int encode_threaded(const yah264_param_t *param, FILE *in, FILE *out,
                           long max_frames, int nthreads)
{
    /* The INPUT geometry, which is what a frame on disk occupies and what the
 * window is sized in. It equals param->width/height unless --crop-rect
 * makes the coded picture smaller than the one being read. */
    int W = g_in_w, H = g_in_h;
    /* g_sub_w/g_sub_h come from the Y4M C tag, the same source the serial path
 * and the recon dumper read. A 4:2:0-only store here is what would keep
 * 4:2:2 and 4:4:4 off the threaded path -- a CLI-side limit, since the
 * encoder codes all three formats. */
    size_t y_size = (size_t)W * H,
           c_size = (size_t)(W / g_sub_w) * (H / g_sub_h);
    int keyint = param->keyint > 0 ? param->keyint : 1;

    /* `payload` is what a frame occupies ON DISK, which is what the file length
 * divides by; `per_frame` is what it costs in memory, which is what the
 * limit compares. */
    uint64_t payload = (uint64_t)(y_size + 2 * c_size) * (uint64_t)g_in_sample_sz;
    /* What a frame costs in MEMORY is the ENCODER's width, which is wider
     * than the disk's under --output-depth 10 on 8-bit input. */
    uint64_t per_frame = (uint64_t)(y_size + 2 * c_size) * (uint64_t)g_enc_sample_sz;
    /* The cut-aware split pre-scans the whole clip, and the pre-scan builds a
 * SECOND whole-clip array: half-resolution luma (w*h/4 samples) plus one
 * int32 intra cost per macroblock, both resident for every frame at once.
 * That is 18% of the payload by arithmetic and measured +242 MiB of peak RSS
 * on 1253 frames of 720p (2058 -> 2300 MB), i.e. +14% of the array -- and
 * because it scales with the clip while the encoder's own cost does not, it
 * is what sets the peak on any clip long enough for this limit to matter
 * (past ~600 frames at 1080p). Price it in when the pre-scan is on. */
    /* The cut-aware split is the one mode that still reads the clip whole: its
 * pre-scan takes luma[0..n) and needs every frame at once, and the boundaries
 * it produces are the dispatcher's input, so there is nothing to dispatch
 * until it has run. It is off by default and it buys 17.4% of samsung_720p's
 * t18 wall when it is on, so it keeps the whole-input read (and the refusal
 * that goes with it) rather than being downgraded to arithmetic boundaries
 * to fit the window. The scan could be made incremental: it is per-frame
 * work carrying one previous lowres frame, not inherently whole-clip
 * (docs/streaming-input-plan.md). */
    int cut_split = (g_cut_split || g_plan_idr || g_force_idr ||
                     (getenv("Y264_CUT_SPLIT") && atoi(getenv("Y264_CUT_SPLIT")))) &&
                    keyint > 1;
    if (cut_split)
        per_frame += (uint64_t)((double)per_frame * 0.18);
    int budget_from_env = 0;
    uint64_t budget = y264_input_budget(&budget_from_env);

    /* The frame count, read off the file length rather than by reading frames.
 * Everything the schedule depends on is a function of it. */
    int hdr_len = -1;
    long nknown = y264_exact_frames(in, payload, &hdr_len);
    if (nknown > 0 && max_frames > 0 && nknown > max_frames) nknown = max_frames;

    /* --open-gop is one instance for the whole encode, so the GOP table is
 * {0, n} and nothing here works without n. Without a length (a pipe) the
 * table would have to be a sentinel the worker discovers at EOF, which
 * takes the window sizing, the two-pass split and the stats array with it;
 * the serial streaming path already encodes exactly that shape, so hand it
 * over instead. Nothing has been read yet -- y264_exact_frames restores the
 * position it probed from -- so the handover costs no frames. */
    if (g_open_gop && nknown <= 0 && !cut_split)
        return Y264_GO_SERIAL;
    /* Named here rather than at the call, so the handover above does not print
 * it twice -- once for the path it declined and once for the serial path
 * that takes over. Nothing has been printed before this point. */
    LOGF(LOG_INFO, "yah264: cpu features: %s\n", g_api->cpu_features());

    /* The WORST CASE the window can reach, which is what the refusal below has
 * to price: g GOPs of at most keyint frames in flight plus one of
 * read-ahead, g at most nthreads. The steady state is far below it -- a
 * worker retires each frame as it feeds it, so what is resident is
 * read-ahead, not slices (job.window is narrowed to that once g is known,
 * below). The valve in the reader means the difference is a budget and not
 * a bound, so the refusal keeps quoting the worst case. */
    long need = (long)(nthreads + 1) * keyint;
    if (nknown > 0 && nknown < need) need = nknown;
    int win_forced = 0;
    if (getenv("Y264_STREAM_WINDOW")) {
        long w = atol(getenv("Y264_STREAM_WINDOW"));
        if (w > 0) { need = w; win_forced = 1; }
                                        /* no 2*keyint floor: a worker consumes
 * one frame at a time, so a window
 * shorter than a GOP is dispatchable */
    }
    if (need < 1) need = 1;

    /* Refuse what will not fit -- but what has to fit is the window, not the
 * clip, so this fires on a machine too small for the requested parallelism
 * rather than on a clip too long for the box. */
    if (budget && per_frame) {
        long hold = cut_split ? y264_projected_frames(in, payload + 6) : need;
        if (cut_split && max_frames > 0 && (hold < 0 || hold > max_frames))
            hold = max_frames;
        if (hold > 0 && (uint64_t)hold * per_frame > budget) {
            char nd[32], lim[32];
            fprintf(stderr, "yah264: this encode needs %s of memory and the "
                    "limit is %s\n",
                    y264_hsize(nd, sizeof nd, (uint64_t)hold * per_frame),
                    y264_hsize(lim, sizeof lim, budget));
            if (cut_split)
                fprintf(stderr, "yah264:   Y264_CUT_SPLIT pre-scans the whole "
                        "input, so all %ld frame(s) are resident at once\n", hold);
            else
                fprintf(stderr, "yah264:   the threaded path streams, but its "
                        "window is (--threads + 1) x --keyint = %ld frame(s); "
                        "lower either, or set Y264_STREAM_WINDOW\n", hold);
            y264_mem_advice(budget, budget_from_env, per_frame, W, H);
            return 1;
        }
    }

    gop_job_t job;
    memset(&job, 0, sizeof job);
    job.param = param;                              /* replaced below with `p` */
    job.keyint = keyint; job.width = W; job.height = H;
    job.csp = param->csp; job.sub_w = g_sub_w; job.sub_h = g_sub_h;
    job.window = cut_split ? INT_MAX : (int)need;
    job.want_fst = g_frame_stats != NULL;
    job.seg = calloc(FS_SEG_MAX, sizeof(frame_t *));
    if (!job.seg) { fprintf(stderr, "yah264: out of memory\n"); return 1; }
    pthread_mutex_init(&job.lock, NULL);
    pthread_cond_init(&job.cv_space, NULL);
    pthread_cond_init(&job.cv_ready, NULL);
    pthread_cond_init(&job.cv_emit, NULL);

    /* With n in hand the GOP table is built whole before a frame is read, and
 * every scheduling decision below is the one a whole-input read would make. */
    int n = -1, n_gops = 0;
    if (nknown > 0 && !cut_split) {
        n = (int)nknown;
        /* --open-gop: one sequence, so one instance. See g_open_gop. */
        n_gops = g_open_gop ? 1 : (n + keyint - 1) / keyint;
        job.gops_cap = n_gops;
        job_rc_alloc(&job, n_gops);
        job.gop_start = malloc((size_t)(n_gops + 1) * sizeof(int));
        job.gop_data  = calloc((size_t)n_gops, sizeof(uint8_t *));
        job.gop_size  = calloc((size_t)n_gops, sizeof(size_t));
        job.gop_done  = calloc((size_t)n_gops, 1);
        job.gop_fst   = job.want_fst ? calloc((size_t)n_gops, sizeof(char *)) : NULL;
        for (int i = 0; i < n_gops; i++) job.gop_start[i] = i * keyint;
        job.gop_start[n_gops] = n;
        job.n_gops = n_gops;
        job.gops_final = 1;
    } else {
        job.gop_start = malloc(sizeof(int));
        job.gop_start[0] = 0;
    }

    /* Narrow the window before the reader starts, not after g is planned: the
 * planning below (and the priming encoder_open) is long enough for a reader
 * running against the worst-case budget to have filled it, and a peak set
 * once is a peak. g is not known yet, but it is at most one worker per GOP,
 * and the real g lowers this again where it is smaller.
 *
 * Only with the frame count in hand. A pipe's dispatcher blocks until either
 * EOF or the (nthreads+1)'th GOP boundary arrives, so it needs that much
 * READ, and no worker exists yet to open the starvation valve: narrowing
 * here deadlocks the two threads against each other (reproduced at
 * --threads 8 --keyint 25 on a pipe). That path narrows below instead, once
 * the split is decided. */
    if (job.window != INT_MAX && !win_forced && n_gops > 0) {
        int gmax = n_gops < nthreads ? n_gops : nthreads;
        long w = (long)(gmax + 1) * stream_readahead();
        if (w < job.window) job.window = (int)w;
    }

    reader_arg_t ra = {
        .j = &job, .in = in,
        .stop_at = nknown > 0 && !cut_split ? nknown : max_frames,
        .ys = y_size, .cs = c_size,
        .keyint = keyint, .hdr_len = nknown > 0 ? hdr_len : -1,
        .verify_eof = nknown > 0 && !cut_split &&
                      (max_frames <= 0 || nknown < max_frames),
    };
    pthread_t rtid;
    /* The reader is joined on several paths and one of them runs BEFORE the
 * common teardown: --cut-split waits the reader out to get the whole input,
 * and the exit below then joined the same thread again. Joining a pthread_t
 * twice is undefined, and the sanitiser says so ("Joining already joined
 * thread") on every --cut-split, --plan idr and --qpfile-with-a-key-frame
 * run. One flag, every join site guarded. */
    int rd_joined = 0;
    if (pthread_create(&rtid, NULL, y4m_reader, &ra) != 0) {
        fprintf(stderr, "yah264: cannot start the input reader\n");
        free(job.seg); free(job.gop_start);
        return 1;
    }

    if (cut_split) {
        /* Whole-input mode: the reader fills unbounded and we wait it out, so
 * the pre-scan sees every frame. */
        if (!rd_joined) { pthread_join(rtid, NULL); rd_joined = 1; }
        n = job.n_read;
        if (job.rerr) { n = 0; }
        if (n > 0) {
            n_gops = (n + keyint - 1) / keyint;
            job.gop_start = realloc(job.gop_start, (size_t)(n_gops + 1) * sizeof(int));
            for (int i = 0; i < n_gops; i++) job.gop_start[i] = i * keyint;
            job.gop_start[n_gops] = n;

            const void **luma = malloc((size_t)n * sizeof(*luma));
            int *lstride = malloc((size_t)n * sizeof(*lstride));
            unsigned char *idr = malloc((size_t)n);
            for (int i = 0; i < n; i++) {
                luma[i] = job.seg[i >> FS_SEG_SH][i & (FS_SEG_N - 1)].y;
                lstride[i] = W;
            }
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int nidr;
            if (g_shot_table || g_shot_crf || g_open_gop) {
                /* The shot table (S1): the same scan, aggregated per cut.
 * --open-gop needs the same aggregation for a different reason: the
 * scan's IDR map holds the keyint cadence as well as the cuts, and
 * only the cuts may split. */
                yah264_shot_t *shots = malloc((size_t)n * sizeof(*shots));
                int ns = shots ? g_api->scan_shots(param, luma, lstride, n, nthreads,
                                                   g_api->depth, idr, shots, n) : -1;
                nidr = 0; for (int i = 0; i < n; i++) nidr += idr[i];
                if (ns >= 0 && g_shot_crf && param->rc.method == YAH264_RC_CRF) {
                    /* S2: per-shot CRF. The reference's rate equation at shot
 * granularity: qp = crf + 6(1-qcomp) log2(C_shot / C_title), C the
 * shot's mean inter cost (its frames' actual price), clamped, shots
 * shorter than min-shot merged into their predecessor. A GOP takes
 * its shot's CRF; a shot over several GOPs shares one. */
                    double crf0 = param->rc.rf;
                    double qcomp = shot_tunable("Y264_SHOT_QCOMP", 0.6), clamp = shot_tunable("Y264_SHOT_CLAMP", 4.0);
                    int minshot = (int)shot_tunable("Y264_SHOT_MIN", 24);
                    double ctitle = 0; long nf = 0;
                    for (int k = 0; k < ns; k++) { int L = shots[k].last - shots[k].first + 1; ctitle += shots[k].pcost_mean * L; nf += L; }
                    ctitle = nf ? ctitle / nf : 1.0;
                    double *soff = calloc((size_t)ns, sizeof *soff);
                    for (int k = 0; k < ns; k++) {
                        double c = shots[k].pcost_mean > 0 ? shots[k].pcost_mean : ctitle;
                        double off = 6.0 * (1.0 - qcomp) * log2(c / ctitle);
                        if (off > clamp) off = clamp; else if (off < -clamp) off = -clamp;
                        int L = shots[k].last - shots[k].first + 1;
                        if (L < minshot && k > 0) off = soff[k - 1];
                        soff[k] = off;
                    }
                    job.gop_rf = calloc((size_t)n, sizeof *job.gop_rf);   /* per frame here, per GOP below */
                    for (int k = 0; k < ns; k++)
                        for (int f = shots[k].first; f <= shots[k].last; f++) job.gop_rf[f] = crf0 + soff[k];
                    if (g_shot_table)
                        for (int k = 0; k < ns; k++) fprintf(stderr, "yah264: shot %d frames %d-%d: CRF %+.2f -> %.2f\n", k, shots[k].first, shots[k].last, soff[k], crf0 + soff[k]);
                    free(soff);
                }
                if (ns >= 0 && g_open_gop) {
                    /* Keep the cuts, drop the cadence. Every remaining boundary
 * opens an instance whose first picture is an IDR, and the
 * keyframes between them are the recovery points. */
                    memset(idr, 0, (size_t)n);
                    for (int k = 0; k < ns; k++) idr[shots[k].first] = 1;
                    nidr = ns;
                }
                if (ns >= 0 && g_shot_table) {
                    fprintf(stderr, "yah264: shot table (%d shots): [\n", ns);
                    for (int k = 0; k < ns; k++)
                        fprintf(stderr, "  {\"shot\": %d, \"first\": %d, \"last\": %d, \"frames\": %d, \"icost_mean\": %.0f, \"icost_peak\": %.0f, \"pcost_mean\": %.0f, \"ratio\": %.3f}%s\n",
                                k, shots[k].first, shots[k].last, shots[k].last - shots[k].first + 1,
                                shots[k].icost_mean, shots[k].icost_peak, shots[k].pcost_mean, shots[k].ratio, k + 1 < ns ? "," : "");
                    fprintf(stderr, "]\n");
                }
                free(shots);
            } else
                nidr = g_api->scan_idr_frames(param, luma, lstride, n, nthreads,
                                              g_api->depth, idr);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (getenv("Y264_CUT_SPLIT_STAT") && atoi(getenv("Y264_CUT_SPLIT_STAT"))) {
                double ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
                            (t1.tv_nsec - t0.tv_nsec) / 1e6;
                fprintf(stderr, "yah264: cut pre-scan %.1f ms, %d IDR(s) at", ms, nidr);
                for (int i = 0; i < n; i++) if (idr[i]) fprintf(stderr, " %d", i);
                fprintf(stderr, "\n");
            }
            for (int z = 0; z < g_nzones; z++)          /* the plan's forced IDRs split GOPs too */
                if ((g_zones[z].flags & YAH264_ZONE_IDR) && g_zones[z].first > 0 && g_zones[z].first < n && !idr[g_zones[z].first]) {
                    idr[g_zones[z].first] = 1; nidr++;
                }
            for (int z = 0; z < g_nforces; z++)        /* and so do the frame file's */
                if (g_forces[z].type == YAH264_FORCE_IDR && g_forces[z].disp > 0 &&
                    g_forces[z].disp < n && !idr[g_forces[z].disp]) {
                    idr[g_forces[z].disp] = 1; nidr++;
                }
            if (nidr > 0) {
                int *ns = malloc((size_t)(nidr + 1) * sizeof(int));
                int m = 0;
                for (int i = 0; i < n; i++) if (idr[i]) ns[m++] = i;
                ns[m] = n;
                free(job.gop_start);
                job.gop_start = ns; n_gops = m;
            }
            if (job.gop_rf) {                     /* per-frame -> per-GOP (the GOP's first frame's shot) */
                double *g_rf = calloc((size_t)n_gops, sizeof *g_rf);
                for (int g = 0; g < n_gops; g++) g_rf[g] = job.gop_rf[job.gop_start[g]];
                free(job.gop_rf); job.gop_rf = g_rf;
            }
            free(luma); free(lstride); free(idr);
            job.gop_data = calloc((size_t)n_gops, sizeof(uint8_t *));
            job.gop_size = calloc((size_t)n_gops, sizeof(size_t));
            job.gop_done = calloc((size_t)n_gops, 1);
            job.gop_fst = job.want_fst ? calloc((size_t)n_gops, sizeof(char *)) : NULL;
            job.gops_cap = n_gops;
            job_rc_alloc(&job, n_gops);
            job.n_gops = n_gops;
        }
        job.gops_final = 1;
    } else if (n < 0) {
        /* No length to read the count off (a pipe). Wait for the split to be
 * decidable rather than guessing, which costs nothing: either EOF
 * arrives first and n is exact, or a (nthreads+1)'th GOP arrives and the
 * schedule is pinned WITHOUT n. The second case is why this fallback
 * costs no quality (docs/streaming-input-plan.md): once n_gops > nthreads
 * the code below takes g = nthreads,
 * k = 1, and the longest-first queue's own per-GOP share
 * ceil(len*nthreads/n) is exactly 1 for every GOP too (n > nthreads*keyint
 * there), with its order identity because all full GOPs tie and the short
 * tail sorts last. So the streamed schedule equals the buffered one, and
 * the ABR drain split that moves bits at k=8 never sees a different k.
 * The bounded wait is at most the window, which is already resident. */
        pthread_mutex_lock(&job.lock);
        while (!job.abort_ && !job.gops_final && job.n_gops <= nthreads)
            pthread_cond_wait(&job.cv_ready, &job.lock);
        if (job.gops_final) { n = job.n_read; n_gops = job.n_gops; }
        pthread_mutex_unlock(&job.lock);
    }

    if (job.rerr || (job.gops_final && job.n_gops == 0)) {
        pthread_mutex_lock(&job.lock);
        job.abort_ = 1;
        pthread_cond_broadcast(&job.cv_space);
        pthread_mutex_unlock(&job.lock);
        if (!rd_joined) { pthread_join(rtid, NULL); rd_joined = 1; }
        int bad = job.rerr;
        for (int i = 0; i < job.n_read; i++) fs_retire(&job, i, i + 1);
        for (int s = 0; s < FS_SEG_MAX; s++) free(job.seg[s]);
        free(job.seg); free(job.gop_start);
        free(job.gop_data); free(job.gop_size); free(job.gop_done);
        return bad;
    }
    /* When the split is still growing, job.gop_start is the reader's to realloc,
 * so nothing outside the lock may hold a pointer into it -- and nothing
 * below needs to, because the only consumers of a whole-clip GOP table are
 * the two heuristics that this branch does not run. n_gops here is a
 * placeholder that makes g and k come out at nthreads and 1. */
    int gstart_known = n >= 0;
    int *gstart = gstart_known ? job.gop_start : NULL;
    if (!gstart_known) n_gops = nthreads + 1;

    /* 2-pass: hand each GOP its own slice of the stats round-trip. The GOP
 * split is settled by here and depends on nothing but the input and the
 * params, so both passes derive the same boundaries -- and pass 2 checks
 * that against the ones pass 1 recorded rather than trusting it. */
    char **gop_stats = NULL;
    double *gop_target = NULL;
    if (param->rc.method == YAH264_RC_2PASS && param->rc.stats && !gstart_known) {
        /* Both halves of the round-trip are indexed by the GOP split, so a split
 * that is still being discovered has nothing to hand out. Two-pass needs
 * to read the input twice anyway, which a pipe cannot do. */
        fprintf(stderr, "yah264: two-pass needs a seekable input on the threaded "
                "path (the GOP split has to be known before pass 1 writes its "
                "per-GOP stats); encode from a file, or add --threads 1\n");
        pthread_mutex_lock(&job.lock);
        job.abort_ = 1;
        pthread_cond_broadcast(&job.cv_space);
        pthread_mutex_unlock(&job.lock);
        if (!rd_joined) { pthread_join(rtid, NULL); rd_joined = 1; }
        for (int i = 0; i < job.n_read; i++) fs_retire(&job, i, i + 1);
        for (int s = 0; s < FS_SEG_MAX; s++) free(job.seg[s]);
        free(job.seg); free(job.gop_start);
        free(job.gop_data); free(job.gop_size); free(job.gop_done);
        return 1;
    }
    if (param->rc.method == YAH264_RC_2PASS && param->rc.stats) {
        gop_stats = calloc((size_t)n_gops, sizeof(char *));
        for (int i = 0; i < n_gops; i++)
            gop_stats[i] = tp_gop_path(param->rc.stats,
                                       param->rc.pass == 2 ? "p2gop" :
                                       param->rc.pass == 3 ? "p3gop" : "p1gop", i);
        /* pass 3 needs BOTH halves: the split, because it reads, and the merge,
 * because it writes. The worker rewrites its own slice file in place, so
 * one path per GOP still serves both directions. */
        if (param->rc.pass >= 2) {
            int fn = param->timebase.fps_num > 0 ? param->timebase.fps_num : 25;
            int fd = param->timebase.fps_den > 0 ? param->timebase.fps_den : 1;
            gop_target = calloc((size_t)n_gops, sizeof(double));
            if (tp_split_pass2(param->rc.stats, gop_stats, gop_target, gstart,
                               n_gops, param->rc.bitrate, (double)fn / fd) != 0) {
                fprintf(stderr, "yah264: pass 2 cannot use '%s' -- re-run pass 1 "
                        "with the same --frames/--keyint and this thread count\n",
                        param->rc.stats);
                tp_free_paths(gop_stats, n_gops, 1);
                free(gop_target);
                pthread_mutex_lock(&job.lock);
                job.abort_ = 1;
                pthread_cond_broadcast(&job.cv_space);
                pthread_mutex_unlock(&job.lock);
                if (!rd_joined) { pthread_join(rtid, NULL); rd_joined = 1; }
                for (int i = 0; i < job.n_read; i++) fs_retire(&job, i, i + 1);
                for (int s = 0; s < FS_SEG_MAX; s++) free(job.seg[s]);
                free(job.seg); free(job.gop_start);
                free(job.gop_data); free(job.gop_size); free(job.gop_done);
                return 1;
            }
        }
    }

    /* Budget split: g GOP-workers x k in-frame (row-wavefront) threads. GOP
 * parallelism is capped at the GOP count, so the wavefront fills the cores
 * GOP-parallel can't (e.g. a 2-GOP clip on 8 threads = 2 x 4 instead of 2). */
    int g = nthreads < n_gops ? nthreads : n_gops;
    if (g < 1) g = 1;
    int k = (nthreads + g - 1) / g;                 /* in-frame threads per GOP-worker */
    /* DIAG (not a feature): force the split so the (g,k) landscape can be swept
 * on a clip whose GOP count exceeds the thread count, where the rule above
 * pins k at 1. Scheduling-only, so the bitstream is unchanged either way. */
    if (getenv("Y264_GOP_FORCE_G")) {
        int fg = atoi(getenv("Y264_GOP_FORCE_G"));
        if (fg > 0) g = fg < n_gops ? fg : n_gops;
    }
    if (getenv("Y264_GOP_FORCE_K")) {
        int fk = atoi(getenv("Y264_GOP_FORCE_K"));
        if (fk > 0) k = fk;
    }
    /* A wavefront refuses threads past its grid's critical-path knee (the
 * encoder clamps to it
 * regardless), so a share above the cap is not a share -- it is a thread
 * that will never be created. Cap what a worker is offered and, below,
 * hand the refused threads to a worker that will still use them. */
    int wfcap = g_api->frame_thread_cap(W, H);
    yah264_param_t p = *param;
    p.frame_threads = k < wfcap ? k : wfcap;

    /* When GOP-parallelism is NOT thread-capped (g == n_gops) every worker owns
 * exactly one GOP, so the pull queue balances nothing -- and a trailing
 * partial GOP then gets an equal thread share it cannot use, while the
 * worker holding a full GOP runs the critical path on a fraction of the
 * machine. Size each worker's wavefront to the frames it will actually
 * code instead (greedy: hand the next thread to the worker with the worst
 * frames-per-thread). The budget REDISTRIBUTED is the one the uniform rule
 * would have handed out, g*k, not nthreads: when g does not divide nthreads
 * the uniform rule oversubscribes, and that oversubscription is load-bearing
 * (a wavefront's gate/ramp gaps let a sleeping worker free its core, the
 * documented staircase result) -- shrinking it to nthreads measured 0.89x on
 * an 8-GOP clip. Byte-identical either way: a GOP's bits are thread-
 * invariant. Y264_GOP_EVEN=1 selects the uniform split.
 *
 * The greedy skips a worker already at the wavefront cap, so the budget
 * flows to workers that can still spend it instead of piling onto the
 * longest GOP past the point its grid can feed. That budget is still the
 * uniform rule's g*k, for the oversubscription reason above -- the cap only
 * REDIRECTS threads, it never shrinks the total handed out, and the leftover
 * when every worker is saturated is unspendable anyway (the encoder would
 * refuse it). */
    yah264_param_t *wp = NULL; int *owner = NULL;
    /* Weight by the frames each worker actually owns whenever the GOPs are not
 * all the same length. Arithmetically that is exactly `n % keyint != 0`; a
 * cut-aware split makes it the common case rather than the trailing-partial
 * one. */
    int ragged = 0;
    for (int i = 1; gstart_known && i < n_gops; i++)
        if (gstart[i + 1] - gstart[i] != gstart[1] - gstart[0]) { ragged = 1; break; }
    int uneven = gstart_known && g > 1 && g == n_gops && ragged &&
                 !(getenv("Y264_GOP_EVEN") && atoi(getenv("Y264_GOP_EVEN")));
    if (uneven) {
        int *len = malloc((size_t)g * sizeof(int));
        int *kw  = malloc((size_t)g * sizeof(int));
        owner = malloc((size_t)n_gops * sizeof(int));
        for (int i = 0; i < g; i++) {
            len[i] = gstart[i + 1] - gstart[i];     /* worker i owns GOP i */
            owner[i] = i; kw[i] = 1;
        }
        for (int left = g * k - g; left > 0; left--) {
            int b = -1;                             /* worst frames-per-thread */
            for (int w = 0; w < g; w++) {
                if (kw[w] >= wfcap) continue;       /* its grid is already full */
                if (b < 0 || (long)len[w] * kw[b] > (long)len[b] * kw[w]) b = w;
            }
            if (b < 0) break;                       /* every worker at its knee */
            kw[b]++;
        }
        wp = malloc((size_t)g * sizeof(*wp));
        for (int w = 0; w < g; w++) { wp[w] = *param; wp[w].frame_threads = kw[w]; }
        k = kw[0];                                  /* the critical GOP's share */
        free(len); free(kw);
    }

    /* The other side of that test: MORE GOPs than workers, which a cut-aware
 * split makes ordinary (cuts do not arrive one per core). Here g == nthreads,
 * so k = ceil(nthreads/g) collapses to exactly 1 and every GOP is coded by a
 * lone serial encoder -- including the longest, which IS the wall. On a 25-GOP
 * 720p clip built as one 250-frame shot against 24 of 25 frames, t18 measured
 * 11.07 s, and the pole alone at one thread is 10.22 s of it: the other 71% of
 * the frames finish inside the pole and 14 of 18 cores idle (CPU/wall 3.8x,
 * against 13.9x for the arithmetic split of the same clip).
 *
 * Two scheduling-only measures answer that, and neither touches the
 * g == n_gops path above:
 *
 * - Hand the queue out longest-first. A straggler cannot recruit a sibling's
 * thread once it is already running, so the only cheap defence is to start
 * it first; display order is otherwise free to schedule the long pole last.
 *
 * - Size the wavefront to the GOP rather than to the worker. The worker opens
 * a fresh encoder per GOP, so this is free, and a 250-frame GOP and a
 * 25-frame one plainly do not want the same width. The share that stops GOP
 * i being the floor is the one that finishes it inside the throughput bound
 * total/nthreads -- i.e. len_i * nthreads / total -- which needs no constant
 * of its own. On the pole that is 6 threads, and 6 threads codes it in
 * 2.38 s against a 2.43 s bound, so the pole lands exactly on the bound.
 *
 * A minimum share under that formula is REJECTED by measurement. The theory
 * is that a lone GOP still stalls on its own gates, ramps and drain, so a
 * share of 1 leaves nothing to cover them -- and the oversubscription note above
 * says exactly that. Swept at t18 (best of 3, ms, floor applied after the
 * formula):
 *
 * floor 1 2 3 4 6
 * uneven_720p 3444 4102 3951 5098 5085
 * sintel_720p 2739 2875 3001 2569 2673
 *
 * uneven_720p wants no floor at all, and not marginally: a floor is threads
 * handed to 24 short GOPs that cannot use them, taken from the pole that can.
 * sintel_720p does not order its column at all -- 1 beats 2 beats 3, then 4
 * beats all three -- and a knob whose response is not monotonic is not being
 * measured on this box (an identical-arms null reads 1.013x on medians with a
 * 38% spread between the fastest and slowest of 14 runs). So sintel cannot pay
 * for the constant either, and uneven actively refuses it: no floor.
 * Y264_GOP_EVEN=1 selects the flat queue. Byte-identical throughout -- a
 * GOP's bits are thread-invariant, and the output is written in GOP order
 * however the queue ran. */
    int *qk = NULL, *qorder = NULL;
    /* Skipped when the count is still being discovered, and skipping it costs
 * nothing there: that branch is reached only with n_gops > nthreads, where
 * ceil(len*nthreads/n) is 1 for every GOP and the sort is the identity, so
 * the flat queue below IS this schedule. */
    int queued = gstart_known && g > 1 && g < n_gops &&
                 !(getenv("Y264_GOP_EVEN") && atoi(getenv("Y264_GOP_EVEN")));
    if (queued) {
        gop_len_t *ord = malloc((size_t)n_gops * sizeof(*ord));
        qk = malloc((size_t)n_gops * sizeof(int));
        qorder = malloc((size_t)n_gops * sizeof(int));
        for (int i = 0; i < n_gops; i++) {
            int len = gstart[i + 1] - gstart[i];
            long want = ((long)len * nthreads + n - 1) / n;      /* ceil */
            if (want < 1) want = 1;
            if (want > wfcap) want = wfcap;
            qk[i] = (int)want;
            ord[i].len = len; ord[i].idx = i;
        }
        qsort(ord, (size_t)n_gops, sizeof(*ord), gop_len_cmp);
        for (int i = 0; i < n_gops; i++) qorder[i] = ord[i].idx;
        k = qk[qorder[0]];                          /* the longest GOP's share */
        free(ord);
    }

    /* Say what the lookahead lead costs, in the units a latency-sensitive user
 * budgets in. k is the critical worker's share whichever split ran above,
 * so this is the delay on the path that decides the wall. */
    {
        yah264_param_t lp = p;
        lp.frame_threads = k;
        int lead = g_api->lookahead_delay(&lp);
        if (lead > 0) {
            int fn = param->timebase.fps_num > 0 ? param->timebase.fps_num : 25;
            int fd = param->timebase.fps_den > 0 ? param->timebase.fps_den : 1;
            double fps = (double)fn / fd;
            LOGF(LOG_INFO, "yah264: lookahead lead %d frame(s) buffered "
                    "(+%.1f ms of latency at %.4g fps); --sync-lookahead 0 or "
                    "--tune zerolatency for none\n", lead, lead * 1000.0 / fps, fps);
        }
    }

    /* Prime the shared dispatch table single-threaded before the workers run.
 * It is also the one place a PARAMETER refusal can be caught on this path:
 * a worker whose encoder_open returns NULL emits no NAL and the run used to
 * finish with status 0 and an empty file. Every worker opens with the same
 * parameters, so if this one is refused they all are. */
    yah264_encoder_t *prime = g_api->encoder_open(param);
    if (!prime) {
        fprintf(stderr, "yah264: encoder_open failed -- these parameters were refused\n");
        tp_free_paths(gop_stats, n_gops, 1);
        free(gop_target);
        pthread_mutex_lock(&job.lock);
        job.abort_ = 1;
        pthread_cond_broadcast(&job.cv_space);
        pthread_mutex_unlock(&job.lock);
        if (!rd_joined) { pthread_join(rtid, NULL); rd_joined = 1; }
        for (int i = 0; i < job.n_read; i++) fs_retire(&job, i, i + 1);
        for (int s = 0; s < FS_SEG_MAX; s++) free(job.seg[s]);
        free(job.seg); free(job.gop_start);
        free(job.gop_data); free(job.gop_size); free(job.gop_done);
        return 1;
    }
    g_api->encoder_close(prime);

    job.param = &p;
    job.wparam = wp; job.gop_owner = owner;
    job.gop_order = qorder; job.gop_k = qk;
    job.gop_stats = gop_stats; job.gop_target = gop_target;
    /* Narrow the window to what the workers actually hold. Because frames are
 * retired per frame, g x keyint is the wrong size by two orders: a worker's
 * encoder copies each frame into its own lookahead ring before
 * encoder_encode returns, so a worker holds ONE input frame at a time and
 * everything else the window carries is read-ahead. g + 1 lots of it is
 * generous at 16 frames each (720p: 44 MB against 249 MB pinned by a
 * 180-frame clip under the g x keyint sizing), and it is a budget rather
 * than a bound -- a worker that starves overrides it, so a forced g needs
 * no widening of its own.
 *
 * Y264_STREAM_READAHEAD sets the per-GOP figure; Y264_STREAM_WINDOW
 * overrides the whole thing, and both are clamped by the worst case above so
 * this can only ever lower it. */
    if (job.window != INT_MAX && !win_forced) {
        long w = (long)(g + 1) * stream_readahead();
        if (w < job.window) {
            pthread_mutex_lock(&job.lock);
            job.window = (int)w;
            pthread_cond_broadcast(&job.cv_space);
            pthread_mutex_unlock(&job.lock);
        }
    }

    job.nworkers = g;
    (void)rc_carry_on();    /* warm the lazy static before the workers race on it */
    pthread_t *tid = malloc((size_t)g * sizeof(pthread_t));
    gop_arg_t *wa = malloc((size_t)g * sizeof(*wa));
    for (int t = 0; t < g; t++) {
        wa[t].j = &job; wa[t].wid = t;
        pthread_create(&tid[t], NULL, gop_worker, &wa[t]);
    }

    /* Write each GOP as it finishes, lowest un-emitted first, instead of holding
 * every GOP's bytes to the join. The compressed side is small next to the
 * input -- 9 GB for a two-hour title at 10 Mbit/s -- but holding it grows
 * without bound in exactly the same way, so a bounded input window with an
 * unbounded output buffer would still be a clip-length ceiling.
 *
 * The cost of streaming the output is that a read error past the first
 * published GOP finds bytes already written. That is the honest
 * behaviour for a streaming encoder and it is what the exit code is for;
 * the alternative is holding the whole stream to be able to withdraw it. */
    int emitted = 0, wr_err = 0;
    for (;;) {
        uint8_t *buf = NULL;
        size_t sz = 0;
        pthread_mutex_lock(&job.lock);
        for (;;) {
            if (job.abort_) break;
            if (job.gops_final && emitted >= job.n_gops) break;
            if (emitted < job.n_gops && job.gop_done[emitted]) break;
            pthread_cond_wait(&job.cv_emit, &job.lock);
        }
        int fin = job.abort_ || (job.gops_final && emitted >= job.n_gops);
        if (!fin) {
            buf = job.gop_data[emitted]; sz = job.gop_size[emitted];
            job.gop_data[emitted] = NULL;
            job.held -= sz;
        }
        pthread_mutex_unlock(&job.lock);
        if (fin) break;
        if (buf && sz && fwrite(buf, 1, sz, out) != sz) {
            fprintf(stderr, "yah264: error writing the output stream\n");
            wr_err = 1;
        }
        if (g_segment_out && buf && sz) {               /* one file per GOP, the segment-aligned output */
            char path[1024]; const char *pc = strstr(g_segment_out, "%d");
            if (pc) snprintf(path, sizeof path, "%.*s%d%s", (int)(pc - g_segment_out), g_segment_out, emitted, pc + 2);
            else    snprintf(path, sizeof path, "%s.%d", g_segment_out, emitted);
            FILE *sf = fopen(path, "wb");
            if (!sf || fwrite(buf, 1, sz, sf) != sz) { fprintf(stderr, "yah264: cannot write segment %s\n", path); wr_err = 1; }
            if (sf) fclose(sf);
        }
        if (job.gop_fst && job.gop_fst[emitted]) {
            FILE *ff = fopen(g_frame_stats, emitted ? "a" : "w");
            if (ff) { fputs(job.gop_fst[emitted], ff); fclose(ff); }
            free(job.gop_fst[emitted]); job.gop_fst[emitted] = NULL;
        }
        free(buf);
        emitted++;
        /* Frames written, not frames read: the progress a viewer cares about is
 * output that exists. gop_start[emitted] is the first frame of the next
 * GOP, i.e. the count already written. */
        progress(job.gop_start && emitted <= job.n_gops ? job.gop_start[emitted] : 0,
                 job.gops_final ? (long)job.gop_start[job.n_gops] : 0);
        if (wr_err) {
            pthread_mutex_lock(&job.lock);
            job.abort_ = 1;
            pthread_cond_broadcast(&job.cv_space);
            pthread_cond_broadcast(&job.cv_ready);
            pthread_mutex_unlock(&job.lock);
            break;
        }
    }

    for (int t = 0; t < g; t++)
        pthread_join(tid[t], NULL);
    if (!rd_joined) { pthread_join(rtid, NULL); rd_joined = 1; }
    free(wa); free(wp); free(owner); free(qk); free(qorder);

    progress_done();
    n = job.n_read;
    n_gops = job.n_gops;

    /* Pass 1's per-GOP files are complete only now: the encoder flushes its
 * lagged RC tail and fcloses the stats FILE inside encoder_close, which the
 * worker calls before it publishes the GOP. */
    int tp_rc = 0;
    if (gop_stats && param->rc.pass != 2)
        tp_rc = tp_merge_pass1(param->rc.stats, gop_stats, job.gop_start, n_gops);
    tp_free_paths(gop_stats, n_gops, 1);
    free(gop_target);

    LOGF(LOG_INFO, "yah264: encoded %d frame(s) in %d GOP(s) on %d GOP-worker(s)"
            " x %d frame-thread(s)%s, window %d frame(s)\n", n, n_gops, g, k,
            uneven ? " (frame-weighted)" :
            queued ? " (longest-first, per-GOP)" :
            gstart_known ? "" : " (streamed split)",
            job.window == INT_MAX ? n : job.window);
    /* What the window ACTUALLY held, which is the claim worth checking: peak RSS
 * moves for reasons that are not this store, so measure the store. */
    if (getenv("Y264_STREAM_STAT") && atoi(getenv("Y264_STREAM_STAT")))
        fprintf(stderr, "yah264: stream peak %d frame(s) resident of %d "
                "(%.1f MiB), output held %.1f MiB\n", job.max_live,
                job.window == INT_MAX ? n : job.window,
                (double)job.max_live * (double)per_frame / 1048576.0,
                (double)job.max_held / 1048576.0);

    for (int i = 0; i < n_gops; i++) free(job.gop_data[i]);
    for (int i = 0; i < job.n_read; i++) {
        frame_t *f = &job.seg[i >> FS_SEG_SH][i & (FS_SEG_N - 1)];
        free(f->y); free(f->u); free(f->v);
    }
    for (int s = 0; s < FS_SEG_MAX; s++) free(job.seg[s]);
    free(job.seg); free(tid);
    free(job.gop_data); free(job.gop_size); free(job.gop_done); free(job.gop_start);
    pthread_mutex_destroy(&job.lock);
    pthread_cond_destroy(&job.cv_space);
    pthread_cond_destroy(&job.cv_ready);
    pthread_cond_destroy(&job.cv_emit);
    return tp_rc ? tp_rc : (wr_err || job.rerr);
}

/* Read one newline-terminated header line (Y4M stream or frame header) into buf.
 * Returns line length excluding the newline, or -1 on EOF/error. */
static int read_line(FILE *f, char *buf, int cap)
{
    int n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            buf[n] = '\0';
            return n;
        }
        if (n < cap - 1)
            buf[n++] = (char)c;
    }
    return -1;
}

/* ---- option values ------------------------------------------------------
 *
 * Numeric options are validated at the flag, naming the domain. atoi/atof has
 * no failure value: "--qp abc" reads as 0 and encodes at QP 0, and an
 * out-of-range number parses, is found out of range at apply time and dropped
 * WITHOUT a message, so "--ref 0" and "--keyint 0" encode at the default and
 * look like they worked.
 *
 * Parsing is via strtod rather than strtol even for the integer options, so a
 * caller that formats a computed number ("800.0", which every Python str(float)
 * produces) works the way atoi allowed. A non-integral value for an integer
 * option is refused rather than truncated. */
static double opt_num(const char *flag, const char *val, double lo, double hi)
{
    char *end = NULL;
    double v = strtod(val, &end);
    if (end == val || *end || !isfinite(v) || v < lo || v > hi) {
        /* %g renders an INT_MAX bound as "2.14748e+09", which reads as a
 * typo rather than a limit. An open-ended range is clearer stated as
 * one. */
        if (hi >= (double)INT_MAX)
            fprintf(stderr, "yah264: %s expects %g or more (got '%s')\n",
                    flag, lo, val);
        else
            fprintf(stderr, "yah264: %s expects %g..%g (got '%s')\n",
                    flag, lo, hi, val);
        exit(2);
    }
    return v;
}

static long opt_int(const char *flag, const char *val, long lo, long hi)
{
    double v = opt_num(flag, val, (double)lo, (double)hi);
    if (v != (double)(long)v) {
        fprintf(stderr, "yah264: %s expects a whole number %ld..%ld (got '%s')\n",
                flag, lo, hi, val);
        exit(2);
    }
    return (long)v;
}

/* ---- --partitions ------------------------------------------------------
 *
 * A comma-separated list of shape names, plus the two words that stand for a
 * whole set. The names are the reference encoder's, so the habit ports; the
 * values behind them are ours (YAH264_PART_* in the public header).
 *
 * `none` and `all` are assignments, not additions: each replaces whatever the
 * list held so far, so `all,p4x4` is `all` and `p8x8,none` is the empty set.
 * Reading them as bits to OR in would make `none` a no-op, which is the one
 * misreading that encodes something nobody asked for and still succeeds. */
static const struct { const char *name; int bit; } PART_NAMES[] = {
    { "p8x8", YAH264_PART_P8X8 },
    { "p4x4", YAH264_PART_P4X4 },
    { "b8x8", YAH264_PART_B8X8 },
    { "i8x8", YAH264_PART_I8X8 },
    { "i4x4", YAH264_PART_I4X4 },
};
#define PART_NAMES_N (sizeof PART_NAMES / sizeof PART_NAMES[0])

static int opt_partitions(const char *val)
{
    int mask = YAH264_PART_NONE;
    const char *p = val;
    if (!*p) {
        fprintf(stderr, "yah264: --partitions takes a list, not an empty string; "
                        "'none' is how you ask for no splits at all\n");
        exit(2);
    }
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        int hit = 0;
        if (n == 4 && !strncmp(p, "none", 4)) { mask = YAH264_PART_NONE; hit = 1; }
        else if (n == 3 && !strncmp(p, "all", 3)) { mask = YAH264_PART_ALL; hit = 1; }
        for (size_t k = 0; !hit && k < PART_NAMES_N; k++)
            if (n == strlen(PART_NAMES[k].name) && !strncmp(p, PART_NAMES[k].name, n)) {
                mask |= PART_NAMES[k].bit;
                hit = 1;
            }
        if (!hit) {
            fprintf(stderr, "yah264: --partitions: unknown shape '%.*s'; the list is "
                            "p8x8, p4x4, b8x8, i8x8, i4x4, plus none and all\n",
                    (int)n, p);
            exit(2);
        }
        p = comma ? comma + 1 : p + n;
    }
    return mask;
}

/* The resolved set, spelled the way --partitions takes it, for the resolved
 * line. 32 bytes is enough for every mask. */
static const char *part_str(int mask, char *buf, size_t cap)
{
    size_t len = 0;
    if (mask == YAH264_PART_NONE) return "none";
    if (mask == YAH264_PART_ALL)  return "all";
    buf[0] = '\0';
    for (size_t k = 0; k < PART_NAMES_N; k++) {
        if (!(mask & PART_NAMES[k].bit)) continue;
        len += (size_t)snprintf(buf + len, cap - len, "%s%s",
                                len ? "," : "", PART_NAMES[k].name);
        if (len >= cap) { len = cap - 1; break; }
    }
    buf[len] = '\0';
    return buf;
}

/* ---- flags that reach the encoder through an environment variable -------
 *
 * Some knobs a user would reasonably expect as flags have no param field, only
 * an Y264_* variable read deep in the encoder. Promoting one does not need a
 * field: the CLI can set the variable it already reads, before the lazy statics
 * warm at encoder open.
 *
 * The variable stays the final say. That is this repo's escape-hatch
 * convention, scripts and sweeps depend on being able to override a binary's
 * arguments from the environment, and an A/B that stopped working because the
 * command line grew a flag would be a nasty way to find out. But a
 * disagreement is announced rather than resolved in silence -- an env var
 * quietly beating an explicit flag is a silent-wrong-tool bug aimed the other
 * way. */
static void opt_env(const char *var, const char *flag, const char *flagval,
                    const char *envval)
{
    const char *cur = getenv(var);
    if (cur && strcmp(cur, envval)) {
        LOGF(LOG_WARN, "yah264: warning: %s=%s in the environment overrides "
                "%s %s\n", var, cur, flag, flagval);
        return;
    }
    setenv(var, envval, 1);
}

/* Which rate-control mode a flag selects. --pass is not one of these: it is not
 * a mode of its own on the command line, it composes with --bitrate, which is
 * its target. */
enum { RC_ARG_NONE = 0, RC_ARG_QP, RC_ARG_BITRATE, RC_ARG_CRF };

static const char *rc_arg_mode_name(int m)
{
    switch (m) {
    case RC_ARG_QP:      return "constant QP";
    case RC_ARG_BITRATE: return "ABR";
    case RC_ARG_CRF:     return "CRF";
    default:             return "";
    }
}

int main(int argc, char **argv)
{
    const char *in_path = NULL;
    const char *out_path = "-";
    const char *preset = NULL;
    const char *recon_path = NULL;
    int out_depth = -1;                 /* --output-depth; -1 = follow the input */
    int qp = -1;
    int keyint = -1;
    int keyint_min = -1;
    int scenecut = 0;               /* 0 = leave the param default alone */
    int threads = -1;
    int hw = 0, hw_strict = 0;              /* --hw off|auto|videotoolbox, --hw-strict */
    int bframes = -1;
    int nref = -1;
    int rc_lookahead = -1;
    int sync_lookahead = 0;         /* 0 = leave the param default (auto) alone */
    int badapt = -1;
    float psy_rd = -1.f;
    float psy_trellis = -1.f;
    int trellis_opt = -1;
    const char *tune = NULL;
    int sar_num = 0, sar_den = 0, level_idc = 0;
    int direct = -1;
    int me_method = -1;                             /* -1 = unset -> follow preset (i.e. YAH264_ME_AUTO) */
    int subme = -1, subpel = -2;                    /* unset -> the preset's values */
    int partitions = YAH264_PART_AUTO;              /* unset -> derived; see the header */
    int b_preme_skip = -1, p_part_gate = -1, rd_surv_rank = -1;   /* -1 = unset -> the preset's */
    int lr_settle = -1, lr_subgate = -1, mbt_depfloor = -1;       /* same convention */
    int cabac = -1;                                 /* -1 = unset -> CABAC (x264 medium default) */
    int transform8x8 = -1;                          /* -1 = unset -> on (x264 medium default) */
    int no_sei = 0;                                 /* --no-sei suppresses the settings SEI */
    /* The literals that became parameters (A-plumb). -1/-999 = unset, so the
 * param defaults stand; every other value is a real one the user asked for. */
    int deblock_on = -1, deblock_a = 0, deblock_b = 0;
    int b_pyramid = -1, weightb = -1, weightp = -2;   /* -2 = unset, -1 = off */
    int constrained_intra = 0;
    int chroma_qp_offset = 0;
    int qp_min = 0, qp_max = 0, qp_step = 0;
    double vbv_init = 0.0;
    double crf_max = 0.0, ratetol = 0.0;        /* both zero-as-unset (see yah264_param_t) */
    int mvrange = 0, sps_id = 0, slices = 1;
    const char *profile = NULL;     /* --profile: constrains AND validates */
    int aud = 0, pic_struct = 0, frame_packing = -1, alt_transfer = 0;
    int cll_max = 0, cll_avg = 0, overscan = 0, video_format = -1;
    int stitchable = 0, fake_interlaced = 0, open_gop = 0;
    int nal_hrd = 0, filler = 0;    /* 0 = follow --nal-hrd (cbr pads, else not) */
    /* PAFF: 0 = not asked for, 1 = --tff, 2 = --bff, -1 = --no-interlaced (an
     * interlaced Y4M is coded as frames anyway). g_y4m_interlace carries what
     * the input header said, which is what decides when nothing was asked. */
    int interlaced = 0;
    int mastering_set = 0;
    int raw_in = 0, raw_w = 0, raw_h = 0, raw_csp = YAH264_CSP_I420;
    int raw_fps_n = 0, raw_fps_d = 0, raw_depth = 8;
    long seek_frames = 0;
    unsigned mast_prim[6] = {0}, mast_wp[2] = {0}, mast_max = 0, mast_min = 0;
    int cqm = 0;                                    /* 0 = flat, 1 = JVT default */
    int bitrate = 0;
    double crf = 0.0;                               /* rate factor, 0 = unset */
    int vbv_maxrate = 0, vbv_bufsize = 0;
    int pass = 0;                                   /* 2-pass: 1 or 2, 0 = off */
    const char *stats_path = "yah264.stats";
    float aq_strength = -1.f;   /* -1 = unset: auto-default per RC mode below */
    int abr_model = 0;          /* --abr-model rf opts into the rate-factor allocator */
    long max_frames = 0;
    /* The rate-control mode flags in the order they were given, so the last one
 * can win and the ones it beat can be named. */
    struct { const char *flag, *val; int mode; } rc_arg[8];
    int n_rc_arg = 0, rc_mode = RC_ARG_NONE;
    /* Called with i already advanced onto the value, so argv[i-1] is the flag. */
#define RC_SEEN(m) do {                                                       \
        rc_mode = (m);                                                        \
        if (n_rc_arg < (int)(sizeof rc_arg / sizeof *rc_arg)) {               \
            rc_arg[n_rc_arg].flag = argv[i - 1];                              \
            rc_arg[n_rc_arg].val  = argv[i];                                  \
            rc_arg[n_rc_arg++].mode = (m);                                    \
        }                                                                     \
    } while (0)

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--input-y4m") && i + 1 < argc)
            in_path = argv[++i];
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc)
            out_path = argv[++i];
        else if (!strcmp(argv[i], "--preset") && i + 1 < argc)
            preset = argv[++i];
        else if (!strcmp(argv[i], "--range") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "full") || !strcmp(v, "pc") || !strcmp(v, "jpeg")) g_vs.full_range = 1;
            else if (!strcmp(v, "limited") || !strcmp(v, "tv") || !strcmp(v, "mpeg")) g_vs.full_range = 0;
            else { fprintf(stderr, "yah264: --range expects full or limited\n"); return 2; }
            g_vs_set = 1;
        }
        else if ((!strcmp(argv[i], "--colorprim") || !strcmp(argv[i], "--transfer") ||
                  !strcmp(argv[i], "--colormatrix") || !strcmp(argv[i], "--chromaloc")) && i + 1 < argc) {
            const char *opt = argv[i], *v = argv[++i];
            int code = colour_code(opt, v);
            if (code < 0) { fprintf(stderr, "yah264: %s: unknown value '%s' (use the H.273 code or bt709/bt2020/bt601/smpte170m/bt470bg/srgb/smpte2084/arib-std-b67)\n", opt, v); return 2; }
            if (!strcmp(opt, "--colorprim")) g_vs.primaries = code;
            else if (!strcmp(opt, "--transfer")) g_vs.transfer = code;
            else if (!strcmp(opt, "--colormatrix")) g_vs.matrix = code;
            else g_vs.chroma_loc = code;
            g_vs_set = 1;
        }
        else if (!strcmp(argv[i], "--qp") && i + 1 < argc) {
            qp = (int)opt_int("--qp", argv[++i], 0, 51);
            RC_SEEN(RC_ARG_QP);
        }
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) {
            bitrate = (int)opt_int("--bitrate", argv[++i], 1, INT_MAX);
            RC_SEEN(RC_ARG_BITRATE);
        }
        else if (!strcmp(argv[i], "--crf") && i + 1 < argc) {
            /* rc.rf is a real rate factor, so this could pass the parsed value
 * straight through. It rounds to tenths first on purpose: that is
 * the CLI's documented granularity, and dropping it would move the
 * bits for any --crf carrying more than one decimal. Ask via the API
 * for finer.
 *
 * The domain starts above 0 because rf = 0 is "CRF unarmed" in the
 * param struct, not x264's lossless --crf 0, which is not
 * implemented here. Accepting it would silently encode at CQP. */
            crf = lround(opt_num("--crf", argv[++i], 0.1, 51.0) * 10.0) / 10.0;
            RC_SEEN(RC_ARG_CRF);
        }
        else if (!strcmp(argv[i], "--vbv-maxrate") && i + 1 < argc)
            vbv_maxrate = (int)opt_int("--vbv-maxrate", argv[++i], 0, INT_MAX);
        else if (!strcmp(argv[i], "--vbv-bufsize") && i + 1 < argc)
            vbv_bufsize = (int)opt_int("--vbv-bufsize", argv[++i], 0, INT_MAX);
        else if (!strcmp(argv[i], "--pass") && i + 1 < argc)
            pass = (int)opt_int("--pass", argv[++i], 1, 3);
        else if (!strcmp(argv[i], "--stats") && i + 1 < argc)
            stats_path = argv[++i];
        else if (!strcmp(argv[i], "--keyint") && i + 1 < argc)
            keyint = (int)opt_int("--keyint", argv[++i], 1, INT_MAX);
        else if (!strcmp(argv[i], "--min-keyint") && i + 1 < argc)
            keyint_min = (int)opt_int("--min-keyint", argv[++i], 0, INT_MAX);
        /* x264 spells "off" as --scenecut 0; the param struct spells it with a
 * negative, because 0 is "unset" for every knob in it. Fold here. */
        else if (!strcmp(argv[i], "--scenecut") && i + 1 < argc) {
            scenecut = (int)opt_int("--scenecut", argv[++i], INT_MIN, INT_MAX);
            if (scenecut <= 0) scenecut = YAH264_SCENECUT_OFF;
        }
        else if (!strcmp(argv[i], "--cut-split")) g_cut_split = 1;
        else if (!strcmp(argv[i], "--shot-table")) { g_shot_table = 1; g_cut_split = 1; }
        else if (!strcmp(argv[i], "--shot-crf")) { g_shot_crf = 1; g_cut_split = 1; }
        else if (!strcmp(argv[i], "--plan") && i + 1 < argc) { if (parse_plan(argv[++i]) < 0) return 2; }
        else if (!strcmp(argv[i], "--qpfile") && i + 1 < argc) { if (parse_qpfile(argv[++i]) < 0) return 2; }
        else if (!strcmp(argv[i], "--gop-threads") && i + 1 < argc) g_gop_threads = (int)opt_int("--gop-threads", argv[++i], 1, 64);
        else if (!strcmp(argv[i], "--segment-out") && i + 1 < argc) g_segment_out = argv[++i];
        else if (!strcmp(argv[i], "--frame-stats") && i + 1 < argc) g_frame_stats = argv[++i];
        else if (!strcmp(argv[i], "--no-scenecut"))
            scenecut = YAH264_SCENECUT_OFF;
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = (int)opt_int("--threads", argv[++i], 0, INT_MAX);
        else if (!strcmp(argv[i], "--hw") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "off")) hw = YAH264_HW_OFF;
            else if (!strcmp(v, "auto")) hw = YAH264_HW_AUTO;
            else if (!strcmp(v, "videotoolbox") || !strcmp(v, "vt")) hw = YAH264_HW_VIDEOTOOLBOX;
            else { fprintf(stderr, "yah264: unknown --hw '%s' (off, auto, videotoolbox)\n", v); return 2; }
        }
        else if (!strcmp(argv[i], "--hw-strict")) hw_strict = 1;
        else if (!strcmp(argv[i], "--bframes") && i + 1 < argc)
            bframes = (int)opt_int("--bframes", argv[++i], 0, INT_MAX);
        else if (!strcmp(argv[i], "--ref") && i + 1 < argc)
            /* 0 references is not a picture-coding structure. Refused here:
 * accepted, it would fail the `>= 1` test at apply time and leave the
 * preset's value standing, so --ref 0 would encode at ref 3 and say
 * nothing. */
            nref = (int)opt_int("--ref", argv[++i], 1, INT_MAX);
        else if (!strcmp(argv[i], "--cabac"))
            cabac = 1;
        else if (!strcmp(argv[i], "--cavlc"))
            cabac = 0;
        else if (!strcmp(argv[i], "--transform-8x8"))
            transform8x8 = 1;
        else if (!strcmp(argv[i], "--no-transform-8x8"))
            transform8x8 = 0;
        else if (!strcmp(argv[i], "--partitions") && i + 1 < argc)
            partitions = opt_partitions(argv[++i]);
        else if (!strcmp(argv[i], "--b-preme-skip") && i + 1 < argc)
            b_preme_skip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-b-preme-skip"))
            b_preme_skip = 0;
        else if (!strcmp(argv[i], "--p-part-gate") && i + 1 < argc)
            p_part_gate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-p-part-gate"))
            p_part_gate = 0;
        else if (!strcmp(argv[i], "--rd-surv-rank") && i + 1 < argc)
            rd_surv_rank = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lr-settle") && i + 1 < argc)
            lr_settle = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-lr-settle"))
            lr_settle = 0;
        else if (!strcmp(argv[i], "--lr-subgate") && i + 1 < argc)
            lr_subgate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-lr-subgate"))
            lr_subgate = 0;
        else if (!strcmp(argv[i], "--mbt-depfloor") && i + 1 < argc)
            mbt_depfloor = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-mbt-depfloor"))
            mbt_depfloor = 0;
        else if (!strcmp(argv[i], "--no-sei"))
            no_sei = 1;
        /* --- the literals that became flags (A-plumb) --- */
        /* x264's spelling: two se(v) offsets, each -6..6, separated by a colon.
 * They are the DIV2 values the slice header carries, so the filter offset
 * the decoder applies is twice what is typed -- x264's numbers port. */
        else if (!strcmp(argv[i], "--deblock") && i + 1 < argc) {
            const char *v = argv[++i]; char *sep = NULL;
            long a = strtol(v, &sep, 10);
            long b = (sep && *sep == ':') ? strtol(sep + 1, &sep, 10) : a;
            if (v == sep || (sep && *sep) || a < -6 || a > 6 || b < -6 || b > 6) {
                fprintf(stderr, "yah264: --deblock expects A:B, each -6..6 (got '%s')\n", v);
                return 2;
            }
            deblock_on = 1; deblock_a = (int)a; deblock_b = (int)b;
        }
        else if (!strcmp(argv[i], "--no-deblock"))
            deblock_on = 0;
        else if (!strcmp(argv[i], "--b-pyramid") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "none")) b_pyramid = 0;
            else if (!strcmp(v, "normal")) b_pyramid = 1;
            else if (!strcmp(v, "strict")) {
                fprintf(stderr, "yah264: --b-pyramid strict is not implemented "
                        "(none, normal)\n");
                return 2;
            } else {
                fprintf(stderr, "yah264: unknown --b-pyramid '%s' (none, normal)\n", v);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--weightb")) weightb = 1;
        else if (!strcmp(argv[i], "--no-weightb")) weightb = 0;
        /* 0 is a real value here and the struct spells unset as 0, so the CLI
 * carries the reference's number and maps 0 onto the off value. */
        else if (!strcmp(argv[i], "--weightp") && i + 1 < argc) {
            int v = (int)opt_int("--weightp", argv[++i], 0, 2);
            weightp = v ? v : YAH264_WEIGHTP_OFF;
        }
        else if (!strcmp(argv[i], "--constrained-intra")) constrained_intra = 1;
        else if (!strcmp(argv[i], "--chroma-qp-offset") && i + 1 < argc)
            chroma_qp_offset = (int)opt_int("--chroma-qp-offset", argv[++i], -12, 12);
        else if (!strcmp(argv[i], "--qpmin") && i + 1 < argc)
            qp_min = (int)opt_int("--qpmin", argv[++i], 0, 51);
        else if (!strcmp(argv[i], "--qpmax") && i + 1 < argc)
            qp_max = (int)opt_int("--qpmax", argv[++i], 1, 51);
        else if (!strcmp(argv[i], "--qpstep") && i + 1 < argc)
            qp_step = (int)opt_int("--qpstep", argv[++i], 1, 51);
        else if (!strcmp(argv[i], "--vbv-init") && i + 1 < argc)
            vbv_init = opt_num("--vbv-init", argv[++i], 0.0, 1000000.0);
        else if (!strcmp(argv[i], "--crf-max") && i + 1 < argc)
            crf_max = opt_num("--crf-max", argv[++i], 1.0, 51.0);
        /* `inf` spelled out, because it is the value that means "no correction"
 * and a number large enough to mean the same thing is a guess about the
 * arithmetic rather than a statement about the encode. */
        else if (!strcmp(argv[i], "--ratetol") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "inf") || !strcmp(v, "INF") || !strcmp(v, "infinite")) ratetol = HUGE_VAL;
            else ratetol = opt_num("--ratetol", v, 0.001, 1000.0);
        }
        else if (!strcmp(argv[i], "--mvrange") && i + 1 < argc)
            mvrange = (int)opt_int("--mvrange", argv[++i], 32, 8192);
        else if (!strcmp(argv[i], "--sps-id") && i + 1 < argc)
            sps_id = (int)opt_int("--sps-id", argv[++i], 0, 31);
        else if (!strcmp(argv[i], "--slices") && i + 1 < argc)
            slices = (int)opt_int("--slices", argv[++i], 1, 256);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc)
            profile = argv[++i];
        /* --- stream-level signalling. None of it moves a sample. --- */
        else if (!strcmp(argv[i], "--aud")) aud = 1;
        else if (!strcmp(argv[i], "--pic-struct")) pic_struct = 1;
        else if (!strcmp(argv[i], "--fake-interlaced")) fake_interlaced = 1;
        else if (!strcmp(argv[i], "--tff")) interlaced = 1;
        else if (!strcmp(argv[i], "--bff")) interlaced = 2;
        else if (!strcmp(argv[i], "--no-interlaced")) interlaced = -1;
        else if (!strcmp(argv[i], "--stitchable")) stitchable = 1;
        else if (!strcmp(argv[i], "--nal-hrd") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "none")) nal_hrd = 0;
            else if (!strcmp(v, "vbr")) nal_hrd = YAH264_NAL_HRD_VBR;
            else if (!strcmp(v, "cbr")) nal_hrd = YAH264_NAL_HRD_CBR;
            else {
                fprintf(stderr, "yah264: --nal-hrd expects none, vbr or cbr\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--filler")) filler = 1;
        else if (!strcmp(argv[i], "--no-filler")) filler = YAH264_FILLER_OFF;
        else if (!strcmp(argv[i], "--open-gop")) open_gop = g_open_gop = 1;
        else if (!strcmp(argv[i], "--no-open-gop")) open_gop = g_open_gop = 0;
        /* --- verbosity, input geometry and the input-side window --- */
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
            g_log = LOG_DEBUG;
        else if (!strcmp(argv[i], "--quiet")) g_log = LOG_WARN;
        else if (!strcmp(argv[i], "--no-progress")) g_no_progress = 1;
        else if (!strcmp(argv[i], "--log-level") && i + 1 < argc) {
            /* No "none": an error still prints at every level, because a run
 * that fails in silence is worse than a noisy one, and a level that
 * promised silence and did not deliver it would be a lie. */
            static const char *L[] = { "", "error", "warning", "info", "debug" };
            const char *v = argv[++i];
            int lv = -1;
            for (int k = 1; k < 5; k++) if (!strcmp(v, L[k])) lv = k;
            if (lv < 0 && v[0] >= '1' && v[0] <= '4' && !v[1]) lv = v[0] - '0';
            if (lv < 0) {
                fprintf(stderr, "yah264: --log-level expects error, warning, info "
                        "or debug (or 1..4)\n");
                return 2;
            }
            g_log = lv;
        }
        else if (!strcmp(argv[i], "--input-raw") && i + 1 < argc) {
            in_path = argv[++i];
            raw_in = 1;
        }
        else if (!strcmp(argv[i], "--input-res") && i + 1 < argc) {
            const char *v = argv[++i]; char *sep = NULL;
            long w = strtol(v, &sep, 10);
            long h = (sep && (*sep == 'x' || *sep == 'X')) ? strtol(sep + 1, &sep, 10) : 0;
            if (w <= 0 || h <= 0 || (sep && *sep)) {
                fprintf(stderr, "yah264: --input-res expects WxH (e.g. 352x288)\n");
                return 2;
            }
            raw_w = (int)w; raw_h = (int)h;
        }
        /* The chroma format AND the sample depth, because raw input has no Y4M
 * C tag to carry either and there is deliberately no --input-depth: the
 * depth belongs with the format, exactly as the C tag spells it. The
 * i-prefixed and bare forms are both taken, and so is the p10 suffix. */
        else if (!strcmp(argv[i], "--input-csp") && i + 1 < argc) {
            const char *v = argv[++i];
            if (*v == 'i') v++;
            const char *suf = strstr(v, "p1");
            if (!strncmp(v, "420", 3)) raw_csp = YAH264_CSP_I420;
            else if (!strncmp(v, "422", 3)) raw_csp = YAH264_CSP_I422;
            else if (!strncmp(v, "444", 3)) raw_csp = YAH264_CSP_I444;
            else {
                fprintf(stderr, "yah264: --input-csp expects i420, i422 or i444, "
                        "each optionally with a p10 suffix (got '%s')\n", v);
                return 2;
            }
            raw_depth = suf ? atoi(suf + 1) : 8;
            if (raw_depth != 8 && raw_depth != 10) {
                fprintf(stderr, "yah264: --input-csp: only 8-bit and p10 are built "
                        "(got %d-bit)\n", raw_depth);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--input-range") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "full") || !strcmp(v, "pc")) { g_vs.full_range = 1; g_vs_set = 1; }
            else if (!strcmp(v, "limited") || !strcmp(v, "tv")) { g_vs.full_range = 0; g_vs_set = 1; }
            else { fprintf(stderr, "yah264: --input-range expects full or limited\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) {
            const char *v = argv[++i]; char *sep = NULL;
            long n = strtol(v, &sep, 10);
            long d = 1;
            if (sep && (*sep == '/' || *sep == ':')) d = strtol(sep + 1, &sep, 10);
            else if (sep && *sep == '.') {           /* 23.976 and friends */
                double f = atof(v);
                n = lround(f * 1000.0); d = 1000; sep = NULL;
            }
            if (n <= 0 || d <= 0 || (sep && *sep)) {
                fprintf(stderr, "yah264: --fps expects N, N/D or a decimal (got '%s')\n", v);
                return 2;
            }
            raw_fps_n = (int)n; raw_fps_d = (int)d;
        }
        else if (!strcmp(argv[i], "--seek") && i + 1 < argc)
            seek_frames = opt_int("--seek", argv[++i], 0, LONG_MAX);
        /* x264's spelling: left,top,right,bottom in LUMA samples. */
        else if (!strcmp(argv[i], "--crop-rect") && i + 1 < argc) {
            const char *v = argv[++i];
            int l, t, r, b;
            char tail;
            if (sscanf(v, "%d,%d,%d,%d%c", &l, &t, &r, &b, &tail) != 4 ||
                l < 0 || t < 0 || r < 0 || b < 0) {
                fprintf(stderr, "yah264: --crop-rect expects left,top,right,bottom "
                        "in luma samples (got '%s')\n", v);
                return 2;
            }
            g_crop_l = l; g_crop_t = t; g_crop_r = r; g_crop_b = b;
        }
        else if (!strcmp(argv[i], "--frame-packing") && i + 1 < argc)
            frame_packing = (int)opt_int("--frame-packing", argv[++i], 0, 7);
        else if (!strcmp(argv[i], "--alternative-transfer") && i + 1 < argc) {
            const char *v = argv[++i];
            int code = colour_code("--transfer", v);
            if (code < 0) {
                fprintf(stderr, "yah264: --alternative-transfer: unknown value '%s' "
                        "(an H.273 code, or the names --transfer takes)\n", v);
                return 2;
            }
            alt_transfer = code;
        }
        else if (!strcmp(argv[i], "--overscan") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "undef")) overscan = 0;
            else if (!strcmp(v, "show")) overscan = 1;
            else if (!strcmp(v, "crop")) overscan = 2;
            else { fprintf(stderr, "yah264: --overscan expects undef, show or crop\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--videoformat") && i + 1 < argc) {
            static const char *VF[] = { "component", "pal", "ntsc", "secam", "mac", "undef" };
            const char *v = argv[++i];
            video_format = -1;
            for (int k = 0; k < 6; k++) if (!strcmp(v, VF[k])) video_format = k;
            if (video_format < 0) {
                fprintf(stderr, "yah264: --videoformat expects component, pal, ntsc, "
                        "secam, mac or undef\n");
                return 2;
            }
        }
        /* x264's spelling: "MaxCLL,MaxFALL" in cd/m^2. */
        else if (!strcmp(argv[i], "--cll") && i + 1 < argc) {
            const char *v = argv[++i]; char *sep = NULL;
            long a = strtol(v, &sep, 10);
            long b = (sep && *sep == ',') ? strtol(sep + 1, &sep, 10) : -1;
            if (v == sep || !sep || *sep || a < 0 || a > 65535 || b < 0 || b > 65535) {
                fprintf(stderr, "yah264: --cll expects MaxCLL,MaxFALL, each 0..65535 "
                        "(got '%s')\n", v);
                return 2;
            }
            cll_max = (int)a; cll_avg = (int)b;
        }
        /* x264's spelling: G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min). The chromaticity
 * pairs are in 0.00002 units and the luminance pair in 0.0001 cd/m^2, and
 * the G,B,R order is the SPEC's, not the R,G,B a person writes. */
        else if (!strcmp(argv[i], "--mastering-display") && i + 1 < argc) {
            const char *v = argv[++i];
            unsigned long g[10];
            int n = sscanf(v, "G(%lu,%lu)B(%lu,%lu)R(%lu,%lu)WP(%lu,%lu)L(%lu,%lu)",
                           &g[0], &g[1], &g[2], &g[3], &g[4],
                           &g[5], &g[6], &g[7], &g[8], &g[9]);
            if (n != 10) {
                fprintf(stderr, "yah264: --mastering-display expects "
                        "G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min) -- chromaticity in "
                        "0.00002 units, luminance in 0.0001 cd/m^2 (got '%s')\n", v);
                return 2;
            }
            for (int k = 0; k < 6; k++) {
                if (g[k] > 50000) {
                    fprintf(stderr, "yah264: --mastering-display: chromaticity %lu is "
                            "above 1.0 in 0.00002 units\n", g[k]);
                    return 2;
                }
                mast_prim[k] = (unsigned)g[k];
            }
            mast_wp[0] = (unsigned)g[6]; mast_wp[1] = (unsigned)g[7];
            mast_max = (unsigned)g[8];   mast_min = (unsigned)g[9];
            mastering_set = 1;
        }
        else if (!strcmp(argv[i], "--cqm") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "jvt")) cqm = 1;
            else if (!strcmp(v, "flat")) cqm = 0;
            else { fprintf(stderr, "yah264: --cqm expects flat|jvt\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--abr-model") && i + 1 < argc) {
            const char *m = argv[++i];
            /* "x264" is an accepted alias of "rf", kept working for existing
 * scripts; it is not advertised. */
            if (!strcmp(m, "rf") || !strcmp(m, "x264")) abr_model = 1;
            else if (!strcmp(m, "default")) abr_model = 0;
            else { fprintf(stderr, "yah264: --abr-model expects 'default' or 'rf'\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--aq-strength") && i + 1 < argc)
            aq_strength = (float)opt_num("--aq-strength", argv[++i], 0.0, 100.0);
        else if (!strcmp(argv[i], "--rc-lookahead") && i + 1 < argc)
            rc_lookahead = (int)opt_int("--rc-lookahead", argv[++i], 0, INT_MAX);
        /* x264 spells "no lead" as --sync-lookahead 0; the param struct spells
 * it with a negative, the same split as --scenecut above. */
        else if (!strcmp(argv[i], "--sync-lookahead") && i + 1 < argc) {
            sync_lookahead = (int)opt_int("--sync-lookahead", argv[++i], INT_MIN, INT_MAX);
            if (sync_lookahead <= 0) sync_lookahead = YAH264_SYNC_LOOKAHEAD_OFF;
        }
        else if (!strcmp(argv[i], "--b-adapt") && i + 1 < argc)
            badapt = (int)opt_int("--b-adapt", argv[++i], 0, 2);
        else if (!strcmp(argv[i], "--psy-rd") && i + 1 < argc)
            psy_rd = (float)opt_num("--psy-rd", argv[++i], 0.0, 100.0);
        else if (!strcmp(argv[i], "--psy-trellis") && i + 1 < argc)
            psy_trellis = (float)opt_num("--psy-trellis", argv[++i], 0.0, 100.0);
        else if (!strcmp(argv[i], "--trellis") && i + 1 < argc)
            trellis_opt = (int)opt_num("--trellis", argv[++i], 0, 2);
        else if (!strcmp(argv[i], "--tune") && i + 1 < argc)
            tune = argv[++i];
        /* Every value is named explicitly: falling back to spatial for
 * anything but the literal "temporal" would let a typo pick the default
 * silently and make a mode we do not implement look accepted. */
        else if (!strcmp(argv[i], "--direct") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "spatial")) direct = YAH264_DIRECT_SPATIAL;
            else if (!strcmp(v, "temporal")) direct = YAH264_DIRECT_TEMPORAL;
            else if (!strcmp(v, "auto")) direct = YAH264_DIRECT_AUTO;
            else {
                fprintf(stderr, "yah264: unknown --direct '%s' (spatial, temporal, auto)\n", v);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--me") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "dia")) me_method = YAH264_ME_DIA;
            else if (!strcmp(v, "hex")) me_method = YAH264_ME_HEX;
            else if (!strcmp(v, "umh")) me_method = YAH264_ME_UMH;
            else { fprintf(stderr, "yah264: unknown --me '%s' (dia, hex, umh)\n", v); return 2; }
        }
        /* The preset sets subme and a user could not override it, which is the
 * gap most likely to bite: x264 users pin --subme independently of
 * --preset as a matter of routine. The scale is x264's (higher = more
 * RD, tiers line up) with one hole at 0, where x264 means its FASTEST
 * mode and this library means "unset, use the default 10". Refusing 0
 * is the only reading that cannot silently do the opposite of what was
 * asked. */
        else if (!strcmp(argv[i], "--subme") && i + 1 < argc) {
            if (!strcmp(argv[i + 1], "0")) {
                fprintf(stderr, "yah264: --subme 0 is x264's fastest mode but this "
                        "library's \"unset\" (= the slowest, 10); ask for 1\n");
                return 2;
            }
            subme = (int)opt_int("--subme", argv[++i], 1, 11);
        }
        /* No x264 equivalent: the refinement PATTERN, which the preset ladder
 * moves separately from subme. Y264_SUBPEL overrides this. */
        else if (!strcmp(argv[i], "--subpel") && i + 1 < argc)
            subpel = (int)opt_int("--subpel", argv[++i], 0, 2);
        else if (!strcmp(argv[i], "--merange") && i + 1 < argc) {
            opt_int("--merange", argv[i + 1], 1, 1024);
            opt_env("Y264_UMH_RANGE", argv[i], argv[i + 1], argv[i + 1]);
            i++;
        }
        /* --- the promoted Y264_* knobs, on the --subpel convention: the flag
 * sets the variable the encoder already reads, and the variable still
 * wins if the environment disagrees with the flag. None of these moves
 * a default; each one only makes an existing behaviour reachable
 * without a shell. --- */
        /* x264's --aq-mode 0 is "AQ off"; the variable behind this flag is a
 * metric selector with no off seat, and 0 resolves to the same encode as
 * 1. Refused rather than accepted, on the --subme 0 precedent: AQ off is
 * spelled --aq-strength 0 here and always has been. */
        else if (!strcmp(argv[i], "--aq-mode") && i + 1 < argc) {
            if (!strcmp(argv[i + 1], "0")) {
                fprintf(stderr, "yah264: --aq-mode 0 is x264's \"AQ off\"; here the mode is a "
                        "metric selector and 0 encodes as 1. Spell it --aq-strength 0\n");
                return 2;
            }
            opt_int("--aq-mode", argv[i + 1], 1, 2);
            opt_env("Y264_AQ_MODE", argv[i], argv[i + 1], argv[i + 1]);
            i++;
        }
        else if (!strcmp(argv[i], "--no-mbtree"))
            opt_env("Y264_MBTREE_OFF", argv[i], "", "1");
        else if (!strcmp(argv[i], "--no-dct-decimate"))
            opt_env("Y264_DCTDEC", argv[i], "", "0");
        else if (!strcmp(argv[i], "--no-fast-pskip"))
            opt_env("Y264_FAST_PSKIP", argv[i], "", "0");
        else if (!strcmp(argv[i], "--no-asm"))
            opt_env("YAH264_NO_ASM", argv[i], "", "1");
        /* x264 spells this pair as ratios; the allocator behind it reads
 * percentages, so the flag carries x264's number and converts here. */
        else if ((!strcmp(argv[i], "--ipratio") || !strcmp(argv[i], "--pbratio")) && i + 1 < argc) {
            const char *flag = argv[i];
            double v = opt_num(flag, argv[++i], 1.0, 10.0);
            char b[32];
            snprintf(b, sizeof b, "%g", v * 100.0);
            opt_env(!strcmp(flag, "--ipratio") ? "Y264_TP_IPF" : "Y264_TP_PBF", flag, argv[i], b);
        }
        else if (!strcmp(argv[i], "--cplxblur") && i + 1 < argc) {
            opt_num("--cplxblur", argv[i + 1], 0.0, 999.0);
            opt_env("Y264_TP_CPLXBLUR", argv[i], argv[i + 1], argv[i + 1]);
            i++;
        }
        else if (!strcmp(argv[i], "--qblur") && i + 1 < argc) {
            opt_num("--qblur", argv[i + 1], 0.0, 999.0);
            opt_env("Y264_TP_QBLUR", argv[i], argv[i + 1], argv[i + 1]);
            i++;
        }
        /* x264's --no-psy is the psy pair set to zero, not a third knob.
 * Spelled as the pair here too, so an explicit --psy-rd anywhere on the
 * line still wins over it exactly as it wins over a --tune. */
        else if (!strcmp(argv[i], "--no-psy")) {
            psy_rd = 0.f;
            psy_trellis = 0.f;
        }
        else if (!strcmp(argv[i], "--qcomp") && i + 1 < argc) {
            opt_num("--qcomp", argv[i + 1], 0.0, 1.0);
            opt_env("Y264_ABR_QCOMP", argv[i], argv[i + 1], argv[i + 1]);
            i++;
        }
        /* x264 inverts its deadzone on the way in: the internal
 * bias is 32 - the flag), so a flag that carries x264's name has to
 * invert too or it would mean the opposite at the same number. With the
 * inversion, x264's own defaults -- inter 21, intra 11 -- land on the
 * biases this encoder already ships, 10.67 and 21.33. */
        else if ((!strcmp(argv[i], "--deadzone-inter") ||
                  !strcmp(argv[i], "--deadzone-intra")) && i + 1 < argc) {
            const char *flag = argv[i];
            int inter = !strcmp(flag, "--deadzone-inter");
            char b[16];
            long v = opt_int(flag, argv[++i], 0, 32);
            snprintf(b, sizeof b, "%ld", 32 - v);
            opt_env(inter ? "Y264_DZ_INTER" : "Y264_DZ_INTRA", flag, argv[i], b);
        }
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            const char *v = argv[++i];
            /* accept "3.1" or "31"; store as level*10 */
            if (strchr(v, '.')) { double d = atof(v); level_idc = (int)(d * 10 + 0.5); }
            else { int n = atoi(v); level_idc = n < 10 ? n * 10 : n; }
            if (level_idc < 10 || level_idc > 62) {
                fprintf(stderr, "yah264: --level expects e.g. 3.1 or 4.0 (10..62)\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--sar") && i + 1 < argc) {
            const char *v = argv[++i]; char *sep = NULL;
            long w = strtol(v, &sep, 10);
            long h = (sep && (*sep == ':' || *sep == '/')) ? strtol(sep + 1, NULL, 10) : 0;
            if (w > 0 && h > 0) { sar_num = (int)w; sar_den = (int)h; }
            else { fprintf(stderr, "yah264: --sar expects W:H (e.g. 16:11)\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            max_frames = opt_int("--frames", argv[++i], 0, LONG_MAX);
        else if (!strcmp(argv[i], "--dump-recon") && i + 1 < argc)
            recon_path = argv[++i];
        else if (!strcmp(argv[i], "--output-depth") && i + 1 < argc)
            out_depth = opt_int("--output-depth", argv[++i], 8, 10);
        else if (!strcmp(argv[i], "--version")) {
            printf("yah264 %s\n", yah264_version());
            return 0;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "yah264: unknown argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (!in_path) {
        usage(argv[0]);
        return 2;
    }

    FILE *in = (!strcmp(in_path, "-")) ? stdin : fopen(in_path, "rb");
    if (!in) {
        fprintf(stderr, "yah264: cannot open input '%s'\n", in_path);
        return 1;
    }
    FILE *out = (!strcmp(out_path, "-")) ? stdout : fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "yah264: cannot open output '%s'\n", out_path);
        return 1;
    }
    FILE *recon = NULL;
    if (recon_path) {
        recon = fopen(recon_path, "wb");
        if (!recon) {
            fprintf(stderr, "yah264: cannot open recon '%s'\n", recon_path);
            return 1;
        }
    }

    char line[512];
    int width = 0, height = 0, fps_num = 25, fps_den = 1;
    int y4m_sar_n = 0, y4m_sar_d = 0;               /* 'A' tag; 0:0 = unspecified */
    int csp_ok = 1, csp = YAH264_CSP_I420;         /* default C420 if unspecified */
    int in_depth = 8;                               /* sample bit depth from C tag */

    /* Raw input carries nothing about itself, so every field the Y4M header
 * would have supplied has to be given. There is no sniffing and no
 * guessing: a wrong geometry on raw input does not fail, it encodes
 * garbage, which is exactly the failure a default would cause. */
    if (raw_in) {
        if (raw_w <= 0 || raw_h <= 0) {
            fprintf(stderr, "yah264: --input-raw needs --input-res WxH -- raw input "
                    "says nothing about its own geometry\n");
            return 1;
        }
        width = raw_w; height = raw_h; csp = raw_csp;
        g_sub_w = csp == YAH264_CSP_I444 ? 1 : 2;
        g_sub_h = csp == YAH264_CSP_I420 ? 2 : 1;
        fps_num = raw_fps_n > 0 ? raw_fps_n : 25;
        fps_den = raw_fps_d > 0 ? raw_fps_d : 1;
        in_depth = raw_depth;               /* from --input-csp's p10 suffix */
        g_raw_input = 1;
    } else {
    if (raw_w > 0 || raw_fps_n > 0 || raw_csp != YAH264_CSP_I420 || raw_depth != 8) {
        fprintf(stderr, "yah264: --input-res, --input-csp and --fps describe RAW "
                "input; a Y4M stream carries its own geometry and this one would "
                "disagree with it. Use --input-raw, or drop them.\n");
        return 1;
    }
    if (read_line(in, line, sizeof(line)) < 0 || strncmp(line, "YUV4MPEG2", 9) != 0) {
        fprintf(stderr, "yah264: input is not a Y4M stream\n");
        return 1;
    }
    for (char *tok = strtok(line + 9, " "); tok; tok = strtok(NULL, " ")) {
        switch (tok[0]) {
        case 'W': width = atoi(tok + 1); break;
        case 'H': height = atoi(tok + 1); break;
        case 'F': sscanf(tok + 1, "%d:%d", &fps_num, &fps_den); break;
        case 'A': sscanf(tok + 1, "%d:%d", &y4m_sar_n, &y4m_sar_d); break;
        case 'C':
            /* Accept 420/422/444 (8-bit variants) and their p10/p12 (16-bit LE)
 * forms; the trailing "p<N>" gives the sample depth. */
            if (!strncmp(tok + 1, "420", 3))      { csp = YAH264_CSP_I420; g_sub_w = 2; g_sub_h = 2; }
            else if (!strncmp(tok + 1, "422", 3)) { csp = YAH264_CSP_I422; g_sub_w = 2; g_sub_h = 1; }
            else if (!strncmp(tok + 1, "444", 3)) { csp = YAH264_CSP_I444; g_sub_w = 1; g_sub_h = 1; }
            else csp_ok = 0;
            if (csp_ok) {
                const char *p = strstr(tok + 1, "p1");
                if (p) in_depth = atoi(p + 1);      /* "p10" -> 10, "p12" -> 12 */
            }
            break;
        case 'I':                                   /* interlacing */
            /* It / Ib name a field order, which is exactly what --tff / --bff
             * mean, so the header selects PAFF unless the command line says
             * otherwise. Im is mixed (per-frame) and there is nothing in the
             * header to code it from; It / Ib is the whole supported set. */
            if (tok[1] == 't') g_y4m_interlace = 1;
            else if (tok[1] == 'b') g_y4m_interlace = 2;
            else if (tok[1] != 'p' && tok[1] != '?' && tok[1] != '\0') {
                fprintf(stderr, "yah264: Y4M interlace mode I%c is not supported "
                        "(It, Ib and Ip are)\n", tok[1]);
                csp_ok = 0;
            }
            break;
        case 'X':                                   /* extension tags: only the colour range matters */
            if (!strncmp(tok + 1, "COLORRANGE=", 11)) {
                if (!strcmp(tok + 12, "FULL")) g_y4m_full_range = 1;
                else if (!strcmp(tok + 12, "LIMITED")) g_y4m_full_range = 0;
            }
            break;
        default: break;
        }
    }
    }
    if (width <= 0 || height <= 0 || !csp_ok) {
        fprintf(stderr, "yah264: unsupported Y4M geometry/colorspace "
                        "(got %dx%d)\n", width, height);
        return 1;
    }
    /* The input's sample width SELECTS the encoder; it no longer refuses one.
     * --output-depth overrides it upward only: 8-bit content can be coded as
     * High 10 (samples upshifted by 2 on the way in), but 10-bit content is
     * not squeezed into an 8-bit encoder, because that is a conversion with a
     * quality decision in it and this CLI has no conversion stage. */
    if (in_depth != 8 && in_depth != 10) {
        fprintf(stderr, "yah264: %d-bit input is not supported (8 and 10 are)\n",
                in_depth);
        return 1;
    }
    if (out_depth < 0) out_depth = in_depth;
    if (out_depth != 8 && out_depth != 10) {
        fprintf(stderr, "yah264: --output-depth takes 8 or 10, not %d\n", out_depth);
        return 1;
    }
    if (out_depth < in_depth) {
        fprintf(stderr, "yah264: --output-depth %d on %d-bit input would have to "
                        "discard samples; convert with ffmpeg first\n",
                out_depth, in_depth);
        return 1;
    }
    g_api = out_depth > 8 ? &api10 : &api8;
    g_in_sample_sz = in_depth > 8 ? 2 : 1;
    g_enc_sample_sz = g_api->depth > 8 ? 2 : 1;
    g_upshift = in_depth == 8 && g_api->depth == 10;

    g_in_w = width;
    g_in_h = height;
    /* --crop-rect narrows the coded picture without narrowing the frame that is
 * read. The offsets have to land on a chroma sample or the chroma plane
 * would be cropped somewhere the luma is not, and the result has to be an
 * even-sized picture because the encoder refuses odd dimensions. Refused
 * rather than rounded: a silently-moved crop window is a crop that did not
 * do what the command line said. */
    if (g_crop_l | g_crop_t | g_crop_r | g_crop_b) {
        int cw = width - g_crop_l - g_crop_r, ch = height - g_crop_t - g_crop_b;
        if (cw <= 0 || ch <= 0) {
            fprintf(stderr, "yah264: --crop-rect leaves nothing of a %dx%d input\n",
                    width, height);
            return 1;
        }
        if ((g_crop_l % g_sub_w) || (g_crop_t % g_sub_h) ||
            (g_crop_r % g_sub_w) || (g_crop_b % g_sub_h)) {
            fprintf(stderr, "yah264: --crop-rect must be a multiple of %d horizontally "
                    "and %d vertically for this chroma format\n", g_sub_w, g_sub_h);
            return 1;
        }
        if ((cw & 1) || (ch & 1)) {
            fprintf(stderr, "yah264: --crop-rect leaves %dx%d; both must be even\n", cw, ch);
            return 1;
        }
        width = cw; height = ch;
    }
    /* --seek drops whole frames off the front of the input. Seek where the
 * input allows it and read-and-discard where it does not, so a pipe still
 * works; the cost of the second form is reading the bytes, which is what a
 * pipe charges for any skip. */
    if (seek_frames > 0) {
        /* Bytes ON DISK, so the input's sample size and not the library's:
 * a skip over an 8-bit file feeding the 10-bit encoder skips one byte
 * per sample, whatever the encoder will expand it to. */
        size_t fy = (size_t)g_in_w * g_in_h * (size_t)g_in_sample_sz;
        size_t fc = (size_t)(g_in_w / g_sub_w) * (g_in_h / g_sub_h) * (size_t)g_in_sample_sz;
        for (long k = 0; k < seek_frames; k++) {
            if (!g_raw_input) {
                if (read_line(in, line, sizeof(line)) < 0 ||
                    strncmp(line, "FRAME", 5) != 0) {
                    fprintf(stderr, "yah264: --seek %ld is past the end of the input "
                            "(%ld frame(s))\n", seek_frames, k);
                    return 1;
                }
            }
            if (fseeko(in, (off_t)(fy + 2 * fc), SEEK_CUR) != 0) {
                char skip[65536];
                size_t left = fy + 2 * fc;
                while (left) {
                    size_t want = left < sizeof skip ? left : sizeof skip;
                    if (fread(skip, 1, want, in) != want) {
                        fprintf(stderr, "yah264: --seek %ld is past the end of the "
                                "input\n", seek_frames);
                        return 1;
                    }
                    left -= want;
                }
            } else if (g_raw_input) {
                int c = fgetc(in);
                if (c == EOF) {
                    fprintf(stderr, "yah264: --seek %ld is past the end of the input "
                            "(%ld frame(s))\n", seek_frames, k + 1);
                    return 1;
                }
                ungetc(c, in);
            }
        }
        LOGF(LOG_INFO, "yah264: skipped %ld input frame(s)\n", seek_frames);
    }

    yah264_param_t param;
    g_api->param_default(&param);
    param.width = width;
    param.height = height;
    param.csp = csp;
    param.timebase.fps_num = fps_num;
    param.timebase.fps_den = fps_den;
    /* with no --preset the bare default is medium with its tool-set (CABAC,
     * ref 3, bframes 3, 8x8dct), close to the reference's medium; aq_strength
     * 0.4 and psy_rd 2.0 differ from it on purpose (params.c). the preset sets
     * subme, subpel, ref and lookahead; an explicit flag overrides it. */
    if (!preset) preset = "medium";
    if (g_api->param_apply_preset(&param, preset) < 0) {   /* owns subme/subpel/ref/lookahead/cabac/tr8/bframes */
        fprintf(stderr, "yah264: unknown preset '%s'\n", preset);
        return 1;
    }
    /* Explicit CLI tool flags override the preset (all default to -1 = unset). */
    if (cabac >= 0) param.cabac = cabac;
    if (transform8x8 >= 0) param.transform8x8 = transform8x8;
    /* Overrides the preset regardless of where it sits on the line, like every
 * other tool flag here. The derived default is left alone when it is not
 * given, so a preset keeps searching exactly what it searched before. */
    if (partitions != YAH264_PART_AUTO) param.partitions = partitions;
    if (b_preme_skip >= 0) param.b_preme_skip = b_preme_skip;
    if (p_part_gate >= 0) param.p_part_gate = p_part_gate;
    if (rd_surv_rank >= 0) param.rd_surv_rank = rd_surv_rank;
    if (lr_settle >= 0) param.lr_settle = lr_settle;
    if (lr_subgate >= 0) param.lr_subgate = lr_subgate;
    if (mbt_depfloor >= 0) param.mbt_depfloor = mbt_depfloor;
    if (qp >= 0)
        param.rc.qp = qp;
    if (keyint > 0)
        param.keyint = keyint;
    if (keyint_min >= 0)
        param.keyint_min = keyint_min;
    if (scenecut != 0)
        param.scenecut = scenecut;
    if (bframes >= 0)
        param.bframes = bframes;
    if (nref >= 1)
        param.ref = nref;
    if (me_method >= 0)                             /* --me overrides the preset's ME gate */
        param.me_method = me_method;
    /* Both override the preset, like every other explicit tool flag. Note that
 * subme is not only an effort dial here: with no --me, the search method
 * follows it (hex below 8, UMH at 8 and above, me.c:umh_allowed), so
 * --subme 8 changes the algorithm as well as the effort. x264 keeps the two
 * fully independent. Pass --me to pin it. */
    if (subme > 0)
        param.subme = subme;
    if (subpel >= 0) {
        param.subpel = subpel;
        /* me.c reads the env FIRST and the param only if it is unset, so this
 * one disagreement has to be reported at the same place as the rest. */
        const char *sp = getenv("Y264_SUBPEL");
        if (sp && atoi(sp) >= 0 && atoi(sp) != subpel)
            LOGF(LOG_WARN, "yah264: warning: Y264_SUBPEL=%s in the environment "
                    "overrides --subpel %d\n", sp, subpel);
    }
    param.cqm = cqm;
    if (rc_lookahead >= 0)
        param.rc.lookahead = rc_lookahead;
    if (badapt >= 0)
        param.badapt = badapt;
    /* --tune sets content-adaptive defaults; explicit --psy-rd/--psy-trellis
 * override it (applied after this block). grain/film enable psy-trellis
 * (AC-energy retention); grain sets psy-rd 1.5, measured as a VMAF-NEG win
 * on heavy grain (park_joy -0.15% / ducks -0.17%) when the default was 1.0;
 * the default is 2.0 now, so for grain it is a cut and has not been
 * re-measured. film leaves psy-rd at the default (lighter grain, not
 * BD-measured; needs a film clip). */
    if (tune) {
        if (!strcmp(tune, "grain")) {
            param.psy_trellis = 1.0f;
            param.psy_rd = 1.5f;
        } else if (!strcmp(tune, "film")) {
            param.psy_trellis = 0.5f;
        } else if (!strcmp(tune, "psnr")) {
            /* optimise for PSNR: no psy shaping, no AQ (both trade PSNR for
 * perceptual quality). Matches x264 --tune psnr. */
            param.psy_rd = 0.f;
            param.psy_trellis = 0.f;
            if (aq_strength < 0.f) aq_strength = 0.f;
        } else if (!strcmp(tune, "ssim")) {
            /* optimise for SSIM: no psy, keep variance-AQ (SSIM rewards the AQ
 * bit redistribution). Matches x264 --tune ssim. */
            param.psy_rd = 0.f;
            param.psy_trellis = 0.f;
        } else if (!strcmp(tune, "zerolatency")) {
            /* low latency: no B-frame reordering, no lookahead, and no
 * lookahead LEAD -- the last one is the only added latency the
 * encoder buffers by default, so a zerolatency tune that left it
 * standing would not be one. x264's --tune zerolatency sets
 * --sync-lookahead 0 for exactly this reason. */
            if (bframes < 0) param.bframes = 0;
            if (rc_lookahead < 0) param.rc.lookahead = 0;
            if (sync_lookahead == 0) sync_lookahead = YAH264_SYNC_LOOKAHEAD_OFF;
        } else if (!strcmp(tune, "animation")) {
            /* Flat cartoon content is temporally cheap, so it wants B frames and
 * nothing else. This tune used to also raise psy-trellis and lower AQ,
 * copying another encoder's published animation tune, and measured as a
 * coin flip: -3.69% BD-NEG on bbb_720p and +4.47% on sintel_720p, two
 * clips of the same class. Decomposed, the three constants pull in
 * different directions and the sum was content luck:
 *
 *   psy-trellis 0.5   bbb +0.57   sintel +0.00   superseded: the shipped
 *                                                psy lattice already sets
 *                                                more than 0.5 on sintel
 *   aq 0.6            bbb +1.00   sintel +5.93   hurts BOTH
 *   bframes           bbb -3.81   sintel -1.14   the only coherent element
 *
 * So only the B frames survive, and more of them is better: at bframes 7,
 * bbb reads -6.80% and sintel -1.01%, at neutral wall on bbb and 0.945x
 * on sintel. AQ is the axis this "class" actually splits on (aq 0 reads
 * sintel -6.86 against bbb +6.81, a symmetric 13.7-point split), which is
 * a per-content selector question and not a tune constant. */
            if (bframes < 0 && param.bframes < 8) param.bframes += 4;
        } else if (!strcmp(tune, "stillimage")) {
            /* One frame, or a slide: the deblocking filter is smoothing detail
 * that no later picture will predict from, and psy-trellis is what
 * keeps the texture. The three constants are the reference tune's, not
 * measured here -- there is no still-image clip in the corpus and
 * inventing one to fit a tune would measure the clip. Named as
 * borrowed, so nobody reads them as a swept result. */
            if (psy_trellis < 0.f) param.psy_trellis = 0.7f;
            if (aq_strength < 0.f) aq_strength = 1.2f;   /* the local, like psnr/ssim:
                                                         * param.aq_strength is
                                                         * written after this block */
            if (deblock_on < 0) { deblock_on = 1; deblock_a = -3; deblock_b = -3; }
        } else if (!strcmp(tune, "fastdecode")) {
            /* Everything that costs the DECODER, off: no deblocking filter, no
 * CABAC, no weighted prediction. It is a real quality loss on purpose
 * -- the point is a stream a weak decoder can keep up with. Each of
 * the three is an explicit flag now, so this tune is spelled in the
 * same vocabulary a user would type. */
            if (deblock_on < 0) deblock_on = 0;
            if (cabac < 0) param.cabac = 0;
            if (weightb < 0) weightb = 0;
            /* Explicit P weighted prediction is the fourth: it costs the
 * decoder a multiply and an add on every predicted sample, and the
 * tune turns it off with the rest. It also lets the derivation claim
 * Baseline, which is the profile a weak decoder is likeliest to
 * have, so this tune is now the one arrangement that reaches it
 * without naming a profile. */
            if (weightp == -2) weightp = YAH264_WEIGHTP_OFF;
        } else {
            fprintf(stderr, "yah264: unknown --tune '%s' "
                    "(grain, film, animation, psnr, ssim, zerolatency, "
                    "stillimage, fastdecode)\n", tune);
            return 2;
        }
    }
    if (sync_lookahead != 0)                    /* explicit flag, or zerolatency */
        param.sync_lookahead = sync_lookahead;
    if (psy_rd >= 0.f)
        param.psy_rd = psy_rd;                  /* explicit knob wins over --tune */
    if (psy_trellis >= 0.f)
        param.psy_trellis = psy_trellis;        /* explicit knob wins over --tune */
    if (trellis_opt >= 0)
        param.trellis = trellis_opt;
    if (direct >= 0)
        param.direct = direct;
    /* --- which rate-control mode, when more than one was asked for -----------
 *
 * --qp, --bitrate and --crf each name a mode, and a command line carrying
 * two of them names two. A fixed precedence -- bitrate beats crf beats qp,
 * whatever the order -- applied without a word would make
 * `--crf 23 --bitrate 5000` produce a file byte-identical to plain
 * `--bitrate 5000`, with nothing saying the CRF had been dropped. That trap
 * has cost this project once: a CRF-mode run with no bitrate target was
 * read by our own harness as an ABR undershoot.
 *
 * Two things are wrong there and they are separable. The silence is the
 * bug; the precedence is a disagreement. x264 resolves the same clash by
 * letting the LAST flag on the command line win (each of its rc options
 * assigns i_rc_method as it is parsed), so under a fixed precedence
 * identical command lines would choose different modes on the two encoders
 * with no diagnostic on either.
 *
 * Both are handled the same way: x264's last-flag-wins, and say what was
 * dropped. Matching rather than erroring is the deliberate half:
 *
 * - It is the direction the API enums take too.
 * - Appending a flag to override an earlier one is how last-wins is used
 * on purpose, by every wrapper script that has a base argument list and
 * a per-run tail. An error would break that pattern, and it would break
 * this repo's own scripts/w2_canary.sh, which appends --crf/--bitrate to
 * a base line carrying --qp 26.
 * - Nothing that runs stops running, so exit codes are unchanged.
 *
 * So `--bitrate X --crf Y` is CRF, and `--crf Y --qp Z` and
 * `--bitrate X --qp Z` are constant QP. Each of those prints the warning
 * below, so the bits move only where the user is told they moved.
 *
 * --pass is not in the contest. It is not a mode on the command line, it is
 * a mode plus a stats round-trip whose target is --bitrate; 2-pass CRF is
 * not implemented here, so a --crf given alongside it is genuinely dropped
 * and says so. */
    {
        /* Under --pass the target IS --bitrate, so a --bitrate is not a loser. */
        int winner = pass > 0 ? RC_ARG_BITRATE : rc_mode;
        for (int a = 0; a < n_rc_arg; a++) {
            if (rc_arg[a].mode == winner) continue;
            /* A repeat of the same flag is not a clash; last-wins is the whole
 * point of writing it twice. Only a DIFFERENT mode is dropped. */
            int shadowed = 0;
            for (int b = a + 1; b < n_rc_arg; b++)
                if (rc_arg[b].mode == rc_arg[a].mode) shadowed = 1;
            if (shadowed) continue;
            /* --qp is the one loser that is not thrown away: rc.qp seeds the
 * encoder's base QP in every mode. Saying "dropped" there would be
 * a different silent-behaviour bug, in a warning. */
            const char *effect = rc_arg[a].mode == RC_ARG_QP
                               ? "still sets only the base QP" : "is dropped";
            if (pass > 0)
                LOGF(LOG_WARN, "yah264: warning: %s %s %s -- --pass %d targets "
                        "--bitrate (2-pass CRF is not implemented)\n",
                        rc_arg[a].flag, rc_arg[a].val, effect, pass);
            else
                LOGF(LOG_WARN, "yah264: warning: %s %s %s -- %s %s came later and "
                        "selects %s (last rate-control flag wins, as in x264)\n",
                        rc_arg[a].flag, rc_arg[a].val, effect,
                        rc_arg[n_rc_arg - 1].flag, rc_arg[n_rc_arg - 1].val,
                        rc_arg_mode_name(winner));
        }
    }
    if (pass > 0) {
        param.rc.method = YAH264_RC_2PASS;
        param.rc.pass = pass;
        param.rc.stats = stats_path;
        if (bitrate > 0) param.rc.bitrate = bitrate;   /* target for pass 2 */
    } else if (rc_mode == RC_ARG_BITRATE) {
        param.rc.method = YAH264_RC_ABR;
        param.rc.bitrate = bitrate;
        param.rc.abr_model = abr_model;
    } else if (rc_mode == RC_ARG_CRF) {
        param.rc.method = YAH264_RC_CRF;
        param.rc.rf = crf;
    }
    /* AQ default: 0.4 for rate-controlled modes (ABR/CRF/2-pass); off for
 * pure CQP (byte-identity); explicit --aq-strength (incl 0) always
 * overrides.
 *
 * It is deliberately NOT x264 medium's 1.0. `Y264_CRF_CPLX` is default on,
 * and under its absolute AQ anchor this knob does a DIFFERENT job -- it
 * scales the offsets' distance from the anchor, not just their spread
 * around a frame mean -- so matching x264's number does not match x264's
 * behaviour. Swept in that regime with `scripts/bd_at_rate.py` at matched
 * achieved bitrate (0.2 / 0.3 / 0.4 / 0.5 / 0.7 / 1.0 on bus, mobile and
 * samsung), the turn is at 0.4 and it is a clear one: 0.3 and 0.2 fall back
 * off it, 0.5 and 0.7 degrade.
 *
 * Corpus, 0.4 against 1.0, twelve clips: **CRF median -3.81%, 11 of 12
 * negative, worst +1.07%** (touchdown -27.03, sintel -15.58, coastguard
 * -9.96). And unlike the absolute anchor it holds on the other band too --
 * **ABR median -3.21%, 9 of 12**, with the two positives inside that band's
 * own noise floor (bench/lowrate/abr_noise.py: 0.1-3.4 points on these
 * clips). So this one is not scoped to CRF. */
    /* Under the whole-system x264 mb-tree mode the default is x264's 1.0. It
 * belongs to that unit and not to this line's own calibration: the 0.4
 * above wins -3.81% median on its own terms, but it does so by trading away
 * 7-10 points of mb-tree TERM value on the board's leg clips (samsung,
 * park_joy, bus) -- a cost that only comes back with x264's anchor,
 * strength and gain restored alongside it. An explicit --aq-strength still
 * wins, for attribution. The gate is the env resolver in macroblock.c
 * (y264_mbt_derived); read directly here because the CLI links only the
 * public header. */
    if (aq_strength < 0.f) {
        const char *xm = getenv("Y264_MBT_DERIVED");
        float def = (xm && atoi(xm)) ? 1.0f : 0.4f;
        aq_strength = (param.rc.method != YAH264_RC_CQP) ? def : 0.f;
    }
    param.aq_strength = aq_strength;
    if (no_sei) param.sei = 0;
    /* SAR: an explicit --sar wins; else carry the Y4M 'A' tag through to the
 * VUI (x264's y4m reader does the same). A0:0 stays "unspecified". */
    param.sar_num = sar_num > 0 ? sar_num : y4m_sar_n;
    param.sar_den = sar_num > 0 ? sar_den : y4m_sar_d;
    param.level_idc = level_idc;
    param.rc.vbv_maxrate = vbv_maxrate;
    param.rc.vbv_bufsize = vbv_bufsize;
    param.rc.vbv_init = vbv_init;
    param.crf_max = crf_max;
    param.ratetol = ratetol;
    param.rc.qp_min = qp_min;
    param.rc.qp_max = qp_max;
    param.rc.qp_step = qp_step;
    if (deblock_on >= 0) param.deblock = deblock_on;
    param.deblock_alpha = deblock_a;
    param.deblock_beta  = deblock_b;
    if (b_pyramid >= 0) param.b_pyramid = b_pyramid;
    if (weightb >= 0) param.weightb = weightb;
    if (weightp != -2) param.weightp = weightp;
    param.constrained_intra = constrained_intra;
    param.chroma_qp_index_offset = chroma_qp_offset;
    param.mvrange = mvrange;
    param.sps_id = sps_id;
    param.slices = slices;
    param.aud = aud;
    param.pic_struct = pic_struct;
    param.nal_hrd = nal_hrd;
    param.filler = filler;
    param.frame_packing = frame_packing;
    param.cll_max = cll_max;
    param.cll_avg = cll_avg;
    param.mastering_set = mastering_set;
    for (int k = 0; k < 6; k++) param.mastering_prim[k] = mast_prim[k];
    param.mastering_wp[0] = mast_wp[0]; param.mastering_wp[1] = mast_wp[1];
    param.mastering_max = mast_max; param.mastering_min = mast_min;
    param.alternative_transfer = alt_transfer;
    param.overscan = overscan;
    param.video_format = video_format;
    param.stitchable = stitchable;
    param.open_gop = open_gop;
    /* Named here as well as refused in the library, because a library refusal
 * reaches the user as "encoder_open failed" and this one has a reason worth
 * printing. */
    if (open_gop && stitchable) {
        fprintf(stderr, "yah264: --open-gop and --stitchable contradict each other: "
                "--stitchable promises a cut point at every keyframe, and open GOP "
                "is the decision that a keyframe is not one\n");
        return 2;
    }
    param.fake_interlaced = fake_interlaced;
    /* An interlaced Y4M says which field comes first; --tff / --bff override it,
     * --no-interlaced refuses to field-code at all. */
    /* Where the field order came from decides what a conflict with it means.
     * ASKED means --tff / --bff on the command line: this encode was told to
     * field-code, so anything field coding cannot do with it is a refusal.
     * The Y4M's own It / Ib tag is not a request, it is a property of the
     * input, so a command line that names something field coding cannot do
     * keeps what it named and says out loud that the field order went unused.
     * Same rule --profile follows for a preset's tools against a named one. */
    int fld_asked = interlaced > 0;
    if (interlaced == 0) interlaced = g_y4m_interlace;
    param.interlaced = interlaced > 0 ? interlaced : 0;
    if (param.interlaced) {
        char hbuf[128];
        const char *why = NULL;
        if (param.csp != YAH264_CSP_I420) why = "is 4:2:0 only";
        else if (hw) why = "has no path through the hardware backend (--hw)";
        else if (param.height % 4) {
            /* The vertical crop counts in double units once the sequence may
 * carry fields, so a height that is not a multiple of 4 cannot be
 * cropped back to itself. */
            snprintf(hbuf, sizeof hbuf, "needs a height that is a multiple "
                     "of 4, and this one is %d", param.height);
            why = hbuf;
        }
        if (why && fld_asked) {
            fprintf(stderr, "yah264: field coding refused: it %s. Drop that "
                    "or drop --tff/--bff\n", why);
            return 2;
        }
        if (why) {
            fprintf(stderr, "yah264: the input declares a field order, but "
                    "field coding %s -- coding frames instead. --tff or --bff "
                    "makes this a refusal; --no-interlaced silences it\n", why);
            param.interlaced = 0;
        }
    }
    if (fake_interlaced && !param.interlaced && (param.height % 4)) {
        fprintf(stderr, "yah264: --fake-interlaced needs a height that is a "
                "multiple of 4 (got %d), for the same reason --tff does\n",
                param.height);
        return 2;
    }
    /* --profile does two things, and which one it does depends on where the
 * conflicting tool came from. A tool the PRESET chose is narrowed in
 * silence, because "--preset medium --profile baseline" is a reasonable
 * thing to type and the preset is a default, not a request. A tool YOU
 * named is refused, because narrowing it would encode something other than
 * what the command line says. The library then re-checks the result
 * against the content, which the CLI cannot see the whole of here. */
    if (profile) {
        static const struct {
            const char *name; int idc, cabac_ok, b_ok, t8_ok, cqm_ok, maxcf;
        } PROF[] = {
            /*                        cabac  B   8x8  cqm  max chroma_format_idc */
            { "baseline", 66,            0,  0,   0,   0,  1 },
            { "main",     77,            1,  1,   0,   0,  1 },
            { "high",    100,            1,  1,   1,   1,  1 },
            { "high10",  110,            1,  1,   1,   1,  1 },
            { "high422", 122,            1,  1,   1,   1,  2 },
            { "high444", 244,            1,  1,   1,   1,  3 },
            { NULL, 0, 0, 0, 0, 0, 0 }
        };
        int pi = -1;
        for (int i = 0; PROF[i].name; i++)
            if (!strcmp(profile, PROF[i].name)) pi = i;
        if (pi < 0) {
            fprintf(stderr, "yah264: unknown --profile '%s' (baseline, main, high, "
                    "high10, high422, high444)\n", profile);
            return 2;
        }
        int cf = csp == YAH264_CSP_I444 ? 3 : csp == YAH264_CSP_I422 ? 2 : 1;
        if (cf > PROF[pi].maxcf) {
            fprintf(stderr, "yah264: --profile %s cannot code %s input\n",
                    profile, cf == 3 ? "4:4:4" : "4:2:2");
            return 2;
        }
#define PROF_REFUSE(what) do {                                               \
            fprintf(stderr, "yah264: --profile %s forbids %s; drop the flag " \
                    "or raise the profile\n", profile, (what));               \
            return 2;                                                        \
        } while (0)
        if (!PROF[pi].cabac_ok) {
            if (cabac == 1) PROF_REFUSE("CABAC");
            param.cabac = 0;
        }
        if (!PROF[pi].b_ok) {
            if (bframes > 0) PROF_REFUSE("B frames");
            param.bframes = 0;
        }
        if (!PROF[pi].t8_ok) {
            if (transform8x8 == 1) PROF_REFUSE("the 8x8 transform");
            param.transform8x8 = 0;
        }
        if (!PROF[pi].cqm_ok && cqm) PROF_REFUSE("--cqm jvt");
        param.profile_idc = PROF[pi].idc;
    }
#undef PROF_REFUSE
    if (qp_min && qp_max && qp_min > qp_max) {
        fprintf(stderr, "yah264: --qpmin %d is above --qpmax %d\n", qp_min, qp_max);
        return 2;
    }

    /* -v / --log-level debug: what the command line actually resolved to, in
 * one line, before anything is encoded. This is the level's whole content
 * today, and it is the useful part -- most "why is it doing that" questions
 * are answered by seeing the resolved preset, mode and geometry rather than
 * by more output during the encode. */
    char partbuf[32];
    LOGF(LOG_DEBUG,
         "yah264: resolved: %dx%d %s in, %dx%d coded%s, %d/%d fps, preset %s%s%s, "
         "subme %d ref %d bframes %d %s, partitions %s, %s, keyint %d, threads %d\n",
         g_in_w, g_in_h,
         param.csp == YAH264_CSP_I444 ? "4:4:4" : param.csp == YAH264_CSP_I422 ? "4:2:2" : "4:2:0",
         param.width, param.height,
         (g_crop_l | g_crop_t | g_crop_r | g_crop_b) ? " (cropped)" : "",
         param.timebase.fps_num, param.timebase.fps_den, preset,
         tune ? " tune " : "", tune ? tune : "",
         param.subme > 0 ? param.subme : 10, param.ref, param.bframes,
         param.cabac ? "cabac" : "cavlc",
         part_str(yah264_partitions_resolved(&param), partbuf, sizeof partbuf),
         param.rc.method == YAH264_RC_CRF   ? "crf"
       : param.rc.method == YAH264_RC_ABR   ? "abr"
       : param.rc.method == YAH264_RC_2PASS ? "multi-pass" : "cqp",
         param.keyint, threads >= 0 ? threads : 0);

    /* GOP-parallel path (unless a recon dump is requested, which is serial). */
    int nthreads = threads >= 0 ? threads : param.threads;
    /* Ask the library what auto means rather than the OS. Both used to answer
     * "every online core" independently, which is the shape of a constant that
     * drifts: the moment one side learns something about asymmetric cores the
     * other is silently a different encoder. */
    if (nthreads <= 0) nthreads = g_api->threads_auto();
    if (nthreads < 1) nthreads = 1;
    param.threads = nthreads;   /* let the library see the RESOLVED request --
 * stq keys on threads==1 (worker params inherit
 * this; only frame_threads is per-worker) */
    /* Route every non-recon encode through the GOP-parallel path, even at one
 * thread: it encodes each GOP with its own encoder (per-GOP frame_num/POC).
 * Byte-identity across thread counts is NOT claimed; see the note on
 * encode_threaded above. The streaming serial
 * path stays only for --dump-recon, whose continuous stream is self-consistent
 * with its own recon and drives the conformance gate. */
    /* 4:2:2 and 4:4:4 ride it too. The only thing that ever excluded them was
 * the reader's frame store -- a hardcoded y + 2*(W/2)*(H/2) allocation and a
 * hardcoded YAH264_CSP_I420 handed to the encoder -- while the encoder
 * itself codes all three formats through I/P/B on both entropy coders. So
 * the limit was the CLI's, and professional/mezzanine 4:2:2 work paid for it
 * with a single-threaded encode of correct output. */
    /* 2-pass rides it too (Y264_2PASS_MT=0 to opt out). The reason it would
 * otherwise be excluded is real -- see the round-trip note above
 * encode_threaded -- and it is answered by splitting the stats file per GOP,
 * not by deleting the term. Pass 2 needs a stats file that a threaded pass 1 wrote: without
 * the GOP markers there is nothing to split on, so it stays serial, which is
 * also what keeps a serial pass 1 feeding a serial pass 2 unchanged. */
    int tp_mt = param.rc.method != YAH264_RC_2PASS;
    if (param.rc.method == YAH264_RC_2PASS) {
        const char *ev = getenv("Y264_2PASS_MT");
        tp_mt = (!ev || atoi(ev)) && param.rc.stats
             && (pass < 2 || tp_stats_have_markers(param.rc.stats));
    }
    /* Whatever is left that forces the serial path is a --threads the encode
 * cannot honour. Dropped in silence that means asking for 18 and getting 1,
 * with nothing to tell it apart from a slow machine. Name the condition.
 *
 * Only when the user asked explicitly for more than one thread. Unset means
 * "auto, every core", which is not a user asking for anything, and warning on
 * it would fire on all 252 --dump-recon runs of the conformance gate. */
    if (threads > 1 && (recon_path || !tp_mt)) {
        /* Ordered by what actually decided it, not by what is also true: with
 * Y264_2PASS_MT=0 set on a pass 2 whose stats file IS marked, blaming
 * the markers would be a confident wrong answer. */
        const char *mt_ev = getenv("Y264_2PASS_MT");
        const char *why =
            recon_path                ? "--dump-recon needs one continuous "
                                        "self-consistent recon stream, which per-GOP "
                                        "encoders cannot produce"
          : (mt_ev && !atoi(mt_ev))   ? "Y264_2PASS_MT=0 pins two-pass to the serial path"
          : !param.rc.stats           ? "two-pass has no --stats path to split per GOP"
          :                             "this pass's stats file has no GOP markers to "
                                        "split on, so a serial pass wrote it";
        LOGF(LOG_WARN, "yah264: warning: --threads %d cannot be honoured, "
                "encoding serially: %s\n", threads, why);
    }
    if (hw != YAH264_HW_OFF) {
        /* docs/videotoolbox-plan.md step 4: one block at open naming the options
 * the user set that the hardware does not honour, from the one table in
 * src/hw/vt_map.c; nothing on defaults. */
        int n = 0;
        for (int i = 1; i < argc; i++) {
            enum y264_hw_class c = y264_hw_option_class(argv[i], NULL);
            n += c == Y264_HW_IGNORED || c == Y264_HW_APPROX;
        }
        if (n) {
            fprintf(stderr, "yah264: hw: %d option(s) not honoured as given by the hardware encoder:\n", n);
            for (int i = 1; i < argc; i++) {
                const char *note = NULL;
                enum y264_hw_class c = y264_hw_option_class(argv[i], &note);
                if (c == Y264_HW_IGNORED || c == Y264_HW_APPROX)
                    fprintf(stderr, "yah264: hw:   %s (%s%s%s)\n", argv[i],
                            c == Y264_HW_IGNORED ? "ignored" : "approximated",
                            note ? ": " : "", note ? note : "");
            }
            if (hw_strict) { fprintf(stderr, "yah264: hw: --hw-strict: refusing\n"); return 2; }
        }
        if (recon_path) {
            fprintf(stderr, "yah264: hw: --dump-recon has no reconstruction to dump in hardware mode\n");
            return 2;
        }
        tp_mt = 0;                              /* the hardware has its own parallelism */
    }
    if (!recon_path && tp_mt) {
        int rc = encode_threaded(&param, in, out, max_frames, nthreads);
        if (rc != Y264_GO_SERIAL) {
            if (g_force_reject) {
                if (g_force_reject_base > 0)
                    fprintf(stderr, "yah264: --qpfile: refused inside the GOP starting at input frame %d; "
                                    "add that to the frame number above\n", g_force_reject_base);
                fprintf(stderr, "yah264: --qpfile: the encode was refused, the output is incomplete\n");
                rc = 1;
            }
            if (in != stdin) fclose(in);
            if (out != stdout) fclose(out);
            return rc;
        }
        if (threads > 1)
            LOGF(LOG_WARN, "yah264: warning: --threads %d cannot be honoured, "
                 "encoding serially: --open-gop is one encoder instance for the "
                 "whole stream and the GOP schedule needs a frame count this "
                 "input does not carry; encode from a file\n", threads);
    }

    yah264_encoder_t *enc = g_api->encoder_open_hw(&param, hw);
    apply_video_signal(enc);
    if (!enc) {
        fprintf(stderr, "yah264: encoder_open failed\n");
        return 1;
    }
    /* The serial path is one instance over the whole stream, so the plan and
 * the frame file reach it unrebased. They were reaching it not at all: this
 * path is what --dump-recon and every unsplittable input take, and a --plan
 * on it was silently dropped. */
    apply_zones(enc, 0, INT_MAX);
    apply_forces(enc, 0, INT_MAX);
    if (g_force_reject)
        return 1;
    if (strcmp(g_api->encoder_backend(enc), "yah264"))
        LOGF(LOG_INFO, "yah264: encoder: %s\n", g_api->encoder_backend(enc));
    else
        LOGF(LOG_INFO, "yah264: cpu features: %s\n", g_api->cpu_features());

    /* The recon is the CODED picture, so the dump carries the cropped geometry
 * and not the input's. */
    struct recon_dump rdump = { param.width, param.height, NULL, 0, 0 };
    if (recon)
        g_api->set_recon_cb(enc, recon_dump_cb, &rdump);

    /* The INPUT frame in SAMPLES, which is what a read consumes. --crop-rect
 * makes the CODED picture smaller than this, and the encoder is shown a
 * window into the buffer rather than a smaller buffer. The buffer is sized
 * by the LIBRARY's sample size, which is what an upshifting read expands
 * into; read_plane consumes g_in_sample_sz bytes per sample. */
    size_t y_size = (size_t)g_in_w * g_in_h;
    size_t c_size = (size_t)(g_in_w / g_sub_w) * (g_in_h / g_sub_h);
    size_t y_bytes = y_size * (size_t)g_enc_sample_sz,
           c_bytes = c_size * (size_t)g_enc_sample_sz;
    uint8_t *y = malloc(y_bytes), *u = malloc(c_bytes), *v = malloc(c_bytes);
    if (!y || !u || !v) {
        fprintf(stderr, "yah264: out of memory\n");
        return 1;
    }

    yah264_nal_t *nal;
    int count;
    int rc = 0;

    if (g_api->encoder_headers(enc, &nal, &count) < 0) {
        fprintf(stderr, "yah264: headers failed\n");
        rc = 1;
        goto done;
    }
    for (int i = 0; i < count; i++)
        fwrite(nal[i].payload, 1, nal[i].size, out);

    long frame = 0, returned = 0;
    while (max_frames == 0 || frame < max_frames) {
        if (g_raw_input) {
            int c = fgetc(in);
            if (c == EOF) break;                    /* clean EOF */
            ungetc(c, in);
        } else {
        int len = read_line(in, line, sizeof(line));
        if (len < 0)
            break;                                  /* clean EOF */
        if (strncmp(line, "FRAME", 5) != 0) {
            fprintf(stderr, "yah264: expected FRAME header, got '%s'\n", line);
            rc = 1;
            goto done;
        }
        }
        if (!read_plane(in, y, y_size) ||
            !read_plane(in, u, c_size) ||
            !read_plane(in, v, c_size)) {
            fprintf(stderr, "yah264: short read on frame %ld\n", frame);
            rc = 1;
            goto done;
        }

        /* --crop-rect is a pointer offset and a smaller width/height over the
 * SAME stride: the frame that was read is the whole input frame, and
 * what the encoder is shown is a window into it. */
        yah264_picture_t pic;
        memset(&pic, 0, sizeof(pic));
        pic.csp = csp;
        pic.width = param.width;
        pic.height = param.height;
        pic.pts = frame;
        /* The crop offset is in SAMPLES and the planes are void*, so the
 * pointer arithmetic is in BYTES and scales with the library's sample
 * size. Strides stay in samples. */
        int cstride = g_in_w / g_sub_w;
        size_t yoff = ((size_t)g_crop_t * g_in_w + g_crop_l) * (size_t)g_enc_sample_sz;
        size_t coff = ((size_t)(g_crop_t / g_sub_h) * cstride + g_crop_l / g_sub_w)
                    * (size_t)g_enc_sample_sz;
        pic.plane[0] = y + yoff; pic.stride[0] = g_in_w;
        pic.plane[1] = u + coff; pic.stride[1] = cstride;
        pic.plane[2] = v + coff; pic.stride[2] = cstride;

        int bytes = g_api->encoder_encode(enc, &nal, &count, &pic);
        if (bytes < 0) {
            fprintf(stderr, "yah264: encode failed on frame %ld\n", frame);
            rc = 1;
            goto done;
        }
        progress(frame, max_frames);
        for (int i = 0; i < count; i++) {
            fwrite(nal[i].payload, 1, nal[i].size, out);
            returned += nal[i].type == YAH264_NAL_SLICE || nal[i].type == YAH264_NAL_SLICE_IDR;
        }
        frame++;
    }

    for (;;) {                                      /* flush (window + B reorder) */
        int fb = g_api->encoder_encode(enc, &nal, &count, NULL);
        if (fb < 0 || (fb == 0 && count == 0))
            break;
        for (int i = 0; i < count; i++) {
            fwrite(nal[i].payload, 1, nal[i].size, out);
            returned += nal[i].type == YAH264_NAL_SLICE || nal[i].type == YAH264_NAL_SLICE_IDR;
        }
    }
    /* The hardware mode counts what came back, not what went in: the session
 * may drop a frame (docs/videotoolbox-plan.md step 2). One slice per frame
 * there; our own streams can carry several NALs per frame, so the count is
 * only reported for the hardware. */
    if (hw != YAH264_HW_OFF && strcmp(g_api->encoder_backend(enc), "yah264"))
        LOGF(LOG_INFO, "yah264: encoded %ld frame(s), %ld returned by the hardware%s\n", frame, returned,
                returned == frame ? "" : " (MISMATCH)");
    else
        LOGF(LOG_INFO, "yah264: encoded %ld frame(s)\n", frame);

    if (recon) {                                    /* write recons in display order */
        /* The CODED geometry, which --crop-rect makes smaller than the input's:
 * the recon is the picture the encoder built, not the frame it was
 * handed a window into. */
        int rw = param.width, rh = param.height;
        size_t ys = (size_t)rw * rh, cs = (size_t)(rw / g_sub_w) * (rh / g_sub_h);
        /* The recon is written as woven FRAMES whatever the picture structure
 * was, so the interlace tag describes the field order inside them. */
        fprintf(recon, "YUV4MPEG2 W%d H%d F%d:%d I%c A1:1 %s\n",
                rw, rh, fps_num, fps_den,
                param.interlaced == 1 ? 't' : param.interlaced == 2 ? 'b' : 'p',
                y264_y4m_ctag());
        for (int i = 0; i < rdump.count; i++) {
            if (!rdump.frames[i]) continue;
            fprintf(recon, "FRAME\n");
            fwrite(rdump.frames[i], 1, (ys + 2 * cs) * (size_t)g_enc_sample_sz, recon);
        }
    }

done:
    progress_done();
    for (int i = 0; i < rdump.cap; i++) free(rdump.frames[i]);
    free(rdump.frames);
    free(y); free(u); free(v);
    g_api->encoder_close(enc);
    if (in != stdin) fclose(in);
    if (out != stdout) fclose(out);
    if (recon) fclose(recon);
    return rc;
}
