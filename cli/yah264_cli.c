/*
 * yah264_cli.c - command-line front end: Y4M in, Annex-B out
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "yah264.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

/* Bytes per luma/chroma sample on disk. 1 at 8-bit; 2 (16-bit little-endian,
 * as ffmpeg writes yuv420p10le / Y4M "C420p10") at 10/12-bit. The internal
 * `pixel` type is uint16 at BD>8 and the host is little-endian, so the on-disk
 * layout and the in-memory buffer match byte-for-byte. */
#define Y264_SAMPLE_SZ ((int)sizeof(pixel))
#define Y264_STR_(x) #x
#define Y264_STR(x) Y264_STR_(x)

/* Chroma subsampling of the input/recon, parsed from the Y4M C tag. Default
 * 4:2:0. Set once after the header is read; the single-threaded encode path and
 * the recon dumper read them. */
static int g_sub_w = 2, g_sub_h = 2;

/* Y4M chroma tag matching g_sub_w/g_sub_h and the build bit depth. */
static const char *y264_y4m_ctag(void)
{
    int d = Y264_BIT_DEPTH;
    if (g_sub_w == 1 && g_sub_h == 1) return d > 8 ? "C444p" Y264_STR(Y264_BIT_DEPTH) : "C444";
    if (g_sub_w == 2 && g_sub_h == 1) return d > 8 ? "C422p" Y264_STR(Y264_BIT_DEPTH) : "C422";
    return d > 8 ? "C420p" Y264_STR(Y264_BIT_DEPTH) : "C420jpeg";
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "yah264 %s - H.264 encoder (Phase 0: I_PCM)\n"
        "usage: %s --input-y4m <in.y4m|-> [-o <out.264|->] [options]\n"
        "  --input-y4m PATH   Y4M input, '-' for stdin\n"
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
        "  --pass N           2-pass: 1 = analysis, 2 = final (with --bitrate)\n"
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
        "                     IDRs. Also makes the cut-aware GOP split a no-op\n",
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
        "  --tune NAME        grain, film, animation, psnr, ssim, zerolatency\n"
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
        "                     the NEON quant path.\n"
        "  (--subme/--subpel override the preset; --merange, --qcomp and the\n"
        "   deadzone pair reach the encoder through the Y264_* variable they were\n"
        "   promoted from, which still wins if it is set in the environment.)\n"
        "  --cabac / --cavlc  entropy coder (default CABAC = x264 medium)\n"
        "  --ref N            reference frames (default 3 = x264 medium)\n"
        "  --bframes N        consecutive B frames (default 3 = x264 medium)\n"
        "  --transform-8x8 / --no-transform-8x8   8x8 transform+intra (default on,\n"
        "                     High profile; --no- for Baseline/Main)\n"
        "  --cqm MODE         quant matrices: flat (default) or jvt (High profile)\n"
        "  --no-sei           suppress the settings SEI (emitted by default, x264-style)\n"
        "  --sar W:H          sample aspect ratio (e.g. 16:11; default square/unspecified)\n"
        "  --level L          force H.264 level (e.g. 3.1 or 40; default = auto from res/fps/DPB)\n"
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
static void recon_dump_cb(void *ud, const yah264_picture_t *rec, int disp)
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
    uint8_t *buf = malloc((ys + 2 * cs) * Y264_SAMPLE_SZ), *dst = buf;
    for (int p = 0; p < 3; p++) {
        int pw = p ? rd->w / g_sub_w : rd->w, ph = p ? rd->h / g_sub_h : rd->h;
        for (int y = 0; y < ph; y++) {
            memcpy(dst, rec->plane[p] + (size_t)y * rec->stride[p],
                   (size_t)pw * Y264_SAMPLE_SZ);
            dst += (size_t)pw * Y264_SAMPLE_SZ;
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
        if (!gs || !gd || !gz || !gk || !rs || !rr || !pg) return -1;
        if (gs) j->gop_start = gs;
        if (gd) j->gop_data = gd;
        if (gz) j->gop_size = gz;
        if (gk) j->gop_done = gk;
        j->rc_state = rs; j->rc_ready = rr; j->pull_gop = pg;
        for (int i = j->gops_cap; i < nc; i++) {
            j->gop_data[i] = NULL; j->gop_size[i] = 0; j->gop_done[i] = 0;
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
    int W = j->width, H = j->height;
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
        yah264_encoder_t *e = yah264_encoder_open(&p);
        if (e && carry.valid) yah264_encoder_rc_import(e, &carry, carry_ahead);
        size_t cap = 1 << 16, sz = 0;
        uint8_t *buf = malloc(cap);
        yah264_nal_t *nal;
        int cnt;

        if (yah264_encoder_headers(e, &nal, &cnt) == 0)
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
            yah264_picture_t pic;
            memset(&pic, 0, sizeof(pic));
            pic.csp = j->csp;
            pic.width = W; pic.height = H; pic.pts = i - start;
            pic.plane[0] = (pixel *)f->y; pic.stride[0] = W;
            pic.plane[1] = (pixel *)f->u; pic.stride[1] = W / j->sub_w;
            pic.plane[2] = (pixel *)f->v; pic.stride[2] = W / j->sub_w;
            if (yah264_encoder_encode(e, &nal, &cnt, &pic) >= 0)
                for (int k = 0; k < cnt; k++)
                    buf_append(&buf, &sz, &cap, nal[k].payload, nal[k].size);

            pthread_mutex_lock(&j->lock);
            fs_retire(j, i, i + 1);
            pthread_cond_broadcast(&j->cv_space);
            pthread_mutex_unlock(&j->lock);
        }
        for (;;) {                              /* flush (window + B reorder) */
            int fb = yah264_encoder_encode(e, &nal, &cnt, NULL);
            if (fb < 0 || (fb == 0 && cnt == 0))
                break;
            for (int k = 0; k < cnt; k++)
                buf_append(&buf, &sz, &cap, nal[k].payload, nal[k].size);
        }
        yah264_rc_state_t st;
        int have_st = yah264_encoder_rc_state(e, &st) == 0;
        yah264_encoder_close(e);
        /* Publish. Frames are retired one at a time as they are fed, so
 * there is nothing left of this GOP to free here. */
        pthread_mutex_lock(&j->lock);
        if (j->rc_state) { if (have_st) j->rc_state[g] = st; j->rc_ready[g] = 1; }
        j->gop_data[g] = buf;
        j->gop_size[g] = sz;
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
    size_t yb, cb;
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
        int len = read_line(r->in, line, sizeof(line));
        if (len < 0) break;                             /* EOF */
        if (strncmp(line, "FRAME", 5) != 0) {
            fprintf(stderr, "yah264: expected FRAME header\n");
            reader_fail(j); return NULL;
        }
        if (r->hdr_len >= 0 && len != r->hdr_len) {
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
        f.y = malloc(r->yb); f.u = malloc(r->cb); f.v = malloc(r->cb);
        if (!f.y || !f.u || !f.v) {
            fprintf(stderr, "yah264: out of memory at frame %d\n", n);
            free(f.y); free(f.u); free(f.v); reader_fail(j); return NULL;
        }
        if (fread(f.y, 1, r->yb, r->in) != r->yb ||
            fread(f.u, 1, r->cb, r->in) != r->cb ||
            fread(f.v, 1, r->cb, r->in) != r->cb) {
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

    if (r->verify_eof && read_line(r->in, line, sizeof(line)) >= 0) {
        fprintf(stderr, "yah264: input has more frames than its length implied "
                "(read %d)\n", n);
        reader_fail(j); return NULL;
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
static int encode_threaded(const yah264_param_t *param, FILE *in, FILE *out,
                           long max_frames, int nthreads)
{
    int W = param->width, H = param->height;
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
    uint64_t payload = (uint64_t)(y_size + 2 * c_size) * (uint64_t)Y264_SAMPLE_SZ;
    uint64_t per_frame = payload;
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
    int cut_split = getenv("Y264_CUT_SPLIT") && atoi(getenv("Y264_CUT_SPLIT")) &&
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
        n_gops = (n + keyint - 1) / keyint;
        job.gops_cap = n_gops;
        job_rc_alloc(&job, n_gops);
        job.gop_start = malloc((size_t)(n_gops + 1) * sizeof(int));
        job.gop_data  = calloc((size_t)n_gops, sizeof(uint8_t *));
        job.gop_size  = calloc((size_t)n_gops, sizeof(size_t));
        job.gop_done  = calloc((size_t)n_gops, 1);
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
        .yb = y_size * Y264_SAMPLE_SZ, .cb = c_size * Y264_SAMPLE_SZ,
        .keyint = keyint, .hdr_len = nknown > 0 ? hdr_len : -1,
        .verify_eof = nknown > 0 && !cut_split &&
                      (max_frames <= 0 || nknown < max_frames),
    };
    pthread_t rtid;
    if (pthread_create(&rtid, NULL, y4m_reader, &ra) != 0) {
        fprintf(stderr, "yah264: cannot start the input reader\n");
        free(job.seg); free(job.gop_start);
        return 1;
    }

    if (cut_split) {
        /* Whole-input mode: the reader fills unbounded and we wait it out, so
 * the pre-scan sees every frame. */
        pthread_join(rtid, NULL);
        n = job.n_read;
        if (job.rerr) { n = 0; }
        if (n > 0) {
            n_gops = (n + keyint - 1) / keyint;
            job.gop_start = realloc(job.gop_start, (size_t)(n_gops + 1) * sizeof(int));
            for (int i = 0; i < n_gops; i++) job.gop_start[i] = i * keyint;
            job.gop_start[n_gops] = n;

            const pixel **luma = malloc((size_t)n * sizeof(*luma));
            int *lstride = malloc((size_t)n * sizeof(*lstride));
            unsigned char *idr = malloc((size_t)n);
            for (int i = 0; i < n; i++) {
                luma[i] = (const pixel *)job.seg[i >> FS_SEG_SH][i & (FS_SEG_N - 1)].y;
                lstride[i] = W;
            }
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int nidr = yah264_scan_idr_frames(param, luma, lstride, n, nthreads, idr);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (getenv("Y264_CUT_SPLIT_STAT") && atoi(getenv("Y264_CUT_SPLIT_STAT"))) {
                double ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
                            (t1.tv_nsec - t0.tv_nsec) / 1e6;
                fprintf(stderr, "yah264: cut pre-scan %.1f ms, %d IDR(s) at", ms, nidr);
                for (int i = 0; i < n; i++) if (idr[i]) fprintf(stderr, " %d", i);
                fprintf(stderr, "\n");
            }
            if (nidr > 0) {
                int *ns = malloc((size_t)(nidr + 1) * sizeof(int));
                int m = 0;
                for (int i = 0; i < n; i++) if (idr[i]) ns[m++] = i;
                ns[m] = n;
                free(job.gop_start);
                job.gop_start = ns; n_gops = m;
            }
            free(luma); free(lstride); free(idr);
            job.gop_data = calloc((size_t)n_gops, sizeof(uint8_t *));
            job.gop_size = calloc((size_t)n_gops, sizeof(size_t));
            job.gop_done = calloc((size_t)n_gops, 1);
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
        pthread_join(rtid, NULL);
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
        pthread_join(rtid, NULL);
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
                                       param->rc.pass == 2 ? "p2gop" : "p1gop", i);
        if (param->rc.pass == 2) {
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
                pthread_join(rtid, NULL);
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
    int wfcap = yah264_frame_thread_cap(W, H);
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
        int lead = yah264_lookahead_delay(&lp);
        if (lead > 0) {
            int fn = param->timebase.fps_num > 0 ? param->timebase.fps_num : 25;
            int fd = param->timebase.fps_den > 0 ? param->timebase.fps_den : 1;
            double fps = (double)fn / fd;
            fprintf(stderr, "yah264: lookahead lead %d frame(s) buffered "
                    "(+%.1f ms of latency at %.4g fps); --sync-lookahead 0 or "
                    "--tune zerolatency for none\n", lead, lead * 1000.0 / fps, fps);
        }
    }

    /* Prime the shared dispatch table single-threaded before the workers run. */
    yah264_encoder_t *prime = yah264_encoder_open(param);
    if (prime) yah264_encoder_close(prime);

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
        free(buf);
        emitted++;
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
    pthread_join(rtid, NULL);
    free(wa); free(wp); free(owner); free(qk); free(qorder);

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

    fprintf(stderr, "yah264: encoded %d frame(s) in %d GOP(s) on %d GOP-worker(s)"
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
        fprintf(stderr, "yah264: warning: %s=%s in the environment overrides "
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
    int cabac = -1;                                 /* -1 = unset -> CABAC (x264 medium default) */
    int transform8x8 = -1;                          /* -1 = unset -> on (x264 medium default) */
    int no_sei = 0;                                 /* --no-sei suppresses the settings SEI */
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
            pass = (int)opt_int("--pass", argv[++i], 1, 2);
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
        else if (!strcmp(argv[i], "--no-sei"))
            no_sei = 1;
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
    if (read_line(in, line, sizeof(line)) < 0 || strncmp(line, "YUV4MPEG2", 9) != 0) {
        fprintf(stderr, "yah264: input is not a Y4M stream\n");
        return 1;
    }

    int width = 0, height = 0, fps_num = 25, fps_den = 1;
    int y4m_sar_n = 0, y4m_sar_d = 0;               /* 'A' tag; 0:0 = unspecified */
    int csp_ok = 1, csp = YAH264_CSP_I420;         /* default C420 if unspecified */
    int in_depth = 8;                               /* sample bit depth from C tag */
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
        default: break;                             /* ignore I, X params */
        }
    }
    if (width <= 0 || height <= 0 || !csp_ok) {
        fprintf(stderr, "yah264: unsupported Y4M geometry/colorspace "
                        "(got %dx%d)\n", width, height);
        return 1;
    }
    if (in_depth != Y264_BIT_DEPTH) {
        fprintf(stderr, "yah264: input is %d-bit but this build is %d-bit "
                        "(rebuild with -Dbit_depth=%d)\n",
                in_depth, Y264_BIT_DEPTH, in_depth);
        return 1;
    }

    yah264_param_t param;
    yah264_param_default(&param);
    param.width = width;
    param.height = height;
    param.csp = csp;
    param.timebase.fps_num = fps_num;
    param.timebase.fps_den = fps_den;
    /* Mirror x264's CLI: with no --preset, the bare default IS medium, AND the
 * medium tool-set (CABAC, ref 3, bframes 3, 8x8dct, aq 1.0) -- so that
 * `yah264 in.y4m` is apples-to-apples with `x264 in.y4m` instead of silently
 * comparing yah264's subme-10/CAVLC/ref-1 against x264 medium. The preset is a
 * real ladder over (subme, subpel, ref, lookahead) -- it SETS those, and an
 * explicit CLI flag (--ref/--rc-lookahead/--bframes) overrides the preset below.
 * Escapes: --preset, --cavlc, --no-transform-8x8, --ref N, --bframes N,
 * --aq-strength F. bframes defaults to 3 (medium); the ladder doesn't scale it yet. */
    if (!preset) preset = "medium";
    if (yah264_param_apply_preset(&param, preset) < 0) {   /* owns subme/subpel/ref/lookahead/cabac/tr8/bframes */
        fprintf(stderr, "yah264: unknown preset '%s'\n", preset);
        return 1;
    }
    /* Explicit CLI tool flags override the preset (all default to -1 = unset). */
    if (cabac >= 0) param.cabac = cabac;
    if (transform8x8 >= 0) param.transform8x8 = transform8x8;
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
            fprintf(stderr, "yah264: warning: Y264_SUBPEL=%s in the environment "
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
        } else {
            fprintf(stderr, "yah264: unknown --tune '%s' "
                    "(grain, film, animation, psnr, ssim, zerolatency)\n", tune);
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
                fprintf(stderr, "yah264: warning: %s %s %s -- --pass %d targets "
                        "--bitrate (2-pass CRF is not implemented)\n",
                        rc_arg[a].flag, rc_arg[a].val, effect, pass);
            else
                fprintf(stderr, "yah264: warning: %s %s %s -- %s %s came later and "
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

    /* GOP-parallel path (unless a recon dump is requested, which is serial). */
    int nthreads = threads >= 0 ? threads : param.threads;
    /* Ask the library what auto means rather than the OS. Both used to answer
     * "every online core" independently, which is the shape of a constant that
     * drifts: the moment one side learns something about asymmetric cores the
     * other is silently a different encoder. */
    if (nthreads <= 0) nthreads = yah264_threads_auto();
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
             && (pass != 2 || tp_stats_have_markers(param.rc.stats));
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
          :                             "this pass-2 stats file has no GOP markers to "
                                        "split on, so a serial pass 1 wrote it";
        fprintf(stderr, "yah264: warning: --threads %d cannot be honoured, "
                "encoding serially: %s\n", threads, why);
    }
    if (hw != YAH264_HW_OFF) {
        /* docs/videotoolbox-plan.md step 4: one block at open naming the options
 * the user set that the hardware does not honour; nothing on defaults. */
        static const char *const ignored[] = {
            "--me", "--merange", "--subme", "--subpel", "--aq-strength", "--psy-rd",
            "--psy-trellis", "--trellis", "--deadzone-inter", "--deadzone-intra", "--cqm",
            "--b-adapt", "--direct", "--min-keyint", "--ref", "--rc-lookahead",
            "--sync-lookahead", "--qcomp", "--abr-model", "--transform-8x8",
            "--no-transform-8x8", "--pass", "--stats", "--threads", "--tune", "--no-scenecut",
            "--scenecut", "--preset", NULL };
        int n = 0;
        for (int i = 1; i < argc; i++)
            for (int k = 0; ignored[k]; k++)
                if (!strcmp(argv[i], ignored[k])) n++;
        if (n) {
            fprintf(stderr, "yah264: hw: %d option(s) not honoured by the hardware encoder:\n", n);
            for (int i = 1; i < argc; i++)
                for (int k = 0; ignored[k]; k++)
                    if (!strcmp(argv[i], ignored[k]))
                        fprintf(stderr, "yah264: hw:   %s (ignored)\n", argv[i]);
            if (hw_strict) { fprintf(stderr, "yah264: hw: --hw-strict: refusing\n"); return 2; }
        }
        if (recon_path) {
            fprintf(stderr, "yah264: hw: --dump-recon has no reconstruction to dump in hardware mode\n");
            return 2;
        }
        tp_mt = 0;                              /* the hardware has its own parallelism */
    }
    if (!recon_path && tp_mt) {
        fprintf(stderr, "yah264: cpu features: %s\n", yah264_cpu_features());
        int rc = encode_threaded(&param, in, out, max_frames, nthreads);
        if (in != stdin) fclose(in);
        if (out != stdout) fclose(out);
        return rc;
    }

    yah264_encoder_t *enc = yah264_encoder_open_hw(&param, hw);
    if (!enc) {
        fprintf(stderr, "yah264: encoder_open failed\n");
        return 1;
    }
    if (strcmp(yah264_encoder_backend(enc), "yah264"))
        fprintf(stderr, "yah264: encoder: %s\n", yah264_encoder_backend(enc));
    else
        fprintf(stderr, "yah264: cpu features: %s\n", yah264_cpu_features());

    struct recon_dump rdump = { width, height, NULL, 0, 0 };
    if (recon)
        yah264_encoder_set_recon_cb(enc, recon_dump_cb, &rdump);

    size_t y_size = (size_t)width * height;
    size_t c_size = (size_t)(width / g_sub_w) * (height / g_sub_h);
    size_t y_bytes = y_size * Y264_SAMPLE_SZ, c_bytes = c_size * Y264_SAMPLE_SZ;
    uint8_t *y = malloc(y_bytes), *u = malloc(c_bytes), *v = malloc(c_bytes);
    if (!y || !u || !v) {
        fprintf(stderr, "yah264: out of memory\n");
        return 1;
    }

    yah264_nal_t *nal;
    int count;
    int rc = 0;

    if (yah264_encoder_headers(enc, &nal, &count) < 0) {
        fprintf(stderr, "yah264: headers failed\n");
        rc = 1;
        goto done;
    }
    for (int i = 0; i < count; i++)
        fwrite(nal[i].payload, 1, nal[i].size, out);

    long frame = 0;
    while (max_frames == 0 || frame < max_frames) {
        int len = read_line(in, line, sizeof(line));
        if (len < 0)
            break;                                  /* clean EOF */
        if (strncmp(line, "FRAME", 5) != 0) {
            fprintf(stderr, "yah264: expected FRAME header, got '%s'\n", line);
            rc = 1;
            goto done;
        }
        if (fread(y, 1, y_bytes, in) != y_bytes ||
            fread(u, 1, c_bytes, in) != c_bytes ||
            fread(v, 1, c_bytes, in) != c_bytes) {
            fprintf(stderr, "yah264: short read on frame %ld\n", frame);
            rc = 1;
            goto done;
        }

        yah264_picture_t pic;
        memset(&pic, 0, sizeof(pic));
        pic.csp = csp;
        pic.width = width;
        pic.height = height;
        pic.pts = frame;
        pic.plane[0] = (pixel *)y; pic.stride[0] = width;
        pic.plane[1] = (pixel *)u; pic.stride[1] = width / g_sub_w;
        pic.plane[2] = (pixel *)v; pic.stride[2] = width / g_sub_w;

        int bytes = yah264_encoder_encode(enc, &nal, &count, &pic);
        if (bytes < 0) {
            fprintf(stderr, "yah264: encode failed on frame %ld\n", frame);
            rc = 1;
            goto done;
        }
        for (int i = 0; i < count; i++)
            fwrite(nal[i].payload, 1, nal[i].size, out);
        frame++;
    }

    for (;;) {                                      /* flush (window + B reorder) */
        int fb = yah264_encoder_encode(enc, &nal, &count, NULL);
        if (fb < 0 || (fb == 0 && count == 0))
            break;
        for (int i = 0; i < count; i++)
            fwrite(nal[i].payload, 1, nal[i].size, out);
    }
    fprintf(stderr, "yah264: encoded %ld frame(s)\n", frame);

    if (recon) {                                    /* write recons in display order */
        size_t ys = (size_t)width * height, cs = (size_t)(width / g_sub_w) * (height / g_sub_h);
        fprintf(recon, "YUV4MPEG2 W%d H%d F%d:%d Ip A1:1 %s\n",
                width, height, fps_num, fps_den, y264_y4m_ctag());
        for (int i = 0; i < rdump.count; i++) {
            if (!rdump.frames[i]) continue;
            fprintf(recon, "FRAME\n");
            fwrite(rdump.frames[i], 1, (ys + 2 * cs) * Y264_SAMPLE_SZ, recon);
        }
    }

done:
    for (int i = 0; i < rdump.cap; i++) free(rdump.frames[i]);
    free(rdump.frames);
    free(y); free(u); free(v);
    yah264_encoder_close(enc);
    if (in != stdin) fclose(in);
    if (out != stdout) fclose(out);
    if (recon) fclose(recon);
    return rc;
}
