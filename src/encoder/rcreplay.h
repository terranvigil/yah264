/*
 * rcreplay.h - record and replay the rate controller's inputs.
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Included by encoder.c only; everything here is static.
 *
 * A one-thread encode with Y264_RC_REC=<file> writes every value the rate
 * controller reads that comes from pixel work -- the lookahead's complexity
 * sums, the full-resolution complexity probes, the AQ frame-DC term, the
 * frame-type and cut decisions, and each coded slice's size -- in the order
 * the controller consumes them, plus the QP each slice was coded at.
 *
 * The same command with Y264_RC_PLAY=<file> instead runs the encoder with
 * the pixel work switched off (no lookahead analysis, no mb-tree, no
 * macroblock analysis or entropy coding, no reference planes) and feeds those
 * values back from the file, so only the frame flow and the rate controller
 * run. A replay of an unchanged controller must reproduce every recorded QP;
 * that is the harness's own first test (scripts/rc_replay.sh). When the
 * controller under test picks a different QP, the slice size is scaled from
 * the recorded one by the 1/qscale law (bits x 2^(dQP/6) for a QP dQP lower,
 * Y264_RC_PLAY_K sets the exponent's scale), which is an estimate and is
 * reported as one.
 *
 * File format: a 16-byte header ("Y264RCR1", then the frame width and height
 * as uint32), then fixed 16-byte records {uint32 tag, uint32 seq, double v},
 * host byte order (the file never leaves the machine that wrote it).
 */
#ifndef Y264_RCREPLAY_H
#define Y264_RCREPLAY_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum {
    RCR_FLAGS = 1,      /* per frame: effective cut*2 + anchor */
    RCR_ARR_CME,        /* arrival: frame_complexity_me */
    RCR_ARR_ILR,        /* arrival: frame_complexity_ilr */
    RCR_FC,             /* frame_complexity (full resolution) at a decide */
    RCR_FCME,           /* frame_complexity_me at a decide */
    RCR_CRFDC,          /* crf_frame_dc */
    RCR_RF2CME,         /* the opening fit's median lookahead cost, -1 = none */
    RCR_MBTMEAN,        /* the anchor's mb-tree mean offset */
    RCR_QP,             /* a coded slice's QP: base * 64 + frame QP (checked) */
    RCR_SIZE,           /* a coded slice's RBSP bytes */
    RCR_ARR_ACX,        /* arrival, VBV: an anchor's lowres cost vs the previous anchor (0 = none) */
    RCR_NTAGS
};

static const char *const rcr_tag_name[RCR_NTAGS] = {
    "?", "flags", "arr_cme", "arr_ilr", "fc", "fcme", "crf_dc", "rf2_cme",
    "mbt_mean", "qp", "size", "arr_acx",
};

typedef struct y264_rcr {
    FILE    *fp;            /* opened at the first record or read: the CLI opens
                             * encoders that never code a frame, and those must
                             * neither truncate a recording nor consume one */
    char     path[1024];
    int      width, height;
    int      broken;        /* the file could not be opened or is not ours */
    int      play;          /* 0 record, 1 replay */
    uint32_t seq;
    int      eof;
    /* replay bookkeeping */
    long     slices, qp_match, qp_diff, modelled;
    long     tag_err;       /* the file and the encoder disagree on the order */
    char     first_diff[192];
    double   bits_rec, bits_play;
    double   kscale;        /* Y264_RC_PLAY_K */
    int      last_qp_rec;   /* the recorded frame QP of the slice in hand */
    int      cur_qp_diff;   /* the slice in hand was coded at another QP */
    /* a leaky-bucket estimate of the VBV over the slices this run produced
     * (one slice = one picture interval; a summary, not the Annex C gate) */
    double   vbv_rate, vbv_buf, vbv_fill, vbv_min;
    long     vbv_under;
    double   vbv_pend;      /* the picture in hand's latest attempt, -1 = none:
                             * a measured-size retry replaces it, so only a
                             * picture's final attempt enters the bucket */
    double   vbv_pend_rec;  /* ...and the recorded size of that attempt */
    long     pictures;
    double   bits;          /* this run's total (recorded or replayed) */
} y264_rcr;

static void rcr_set_vbv(y264_rcr *r, int maxrate_kbps, int buf_kbit, double init,
                        int fps_num, int fps_den)
{
    if (!r || maxrate_kbps <= 0 || buf_kbit <= 0 || fps_num <= 0 || fps_den <= 0)
        return;
    if (r->vbv_buf > 0.0)
        return;                     /* a later GOP instance: the bucket runs on */
    r->vbv_rate = 1000.0 * maxrate_kbps * fps_den / fps_num;   /* bits per picture */
    r->vbv_buf = 1000.0 * buf_kbit;
    r->vbv_fill = init <= 0.0 ? r->vbv_buf : (init <= 1.0 ? init * r->vbv_buf : 1000.0 * init);
    if (r->vbv_fill > r->vbv_buf) r->vbv_fill = r->vbv_buf;
    r->vbv_min = r->vbv_fill;
}

static void rcr_vbv_commit(y264_rcr *r)
{
    if (r->vbv_pend < 0.0)
        return;
    double bits = r->vbv_pend;
    r->vbv_pend = -1.0;
    r->pictures++;
    r->bits += bits;
    if (r->play) {                  /* totals over final attempts only */
        r->bits_play += bits;
        r->bits_rec += r->vbv_pend_rec;
    }
    if (r->vbv_buf <= 0.0)
        return;
    r->vbv_fill -= bits;
    if (r->vbv_fill < r->vbv_min) r->vbv_min = r->vbv_fill;
    if (r->vbv_fill < 0.0) r->vbv_under++;
    r->vbv_fill += r->vbv_rate;
    if (r->vbv_fill > r->vbv_buf) r->vbv_fill = r->vbv_buf;
}

/* A coded attempt of a picture: a retry replaces the picture's previous
 * attempt, a first attempt commits the previous picture. */
static void rcr_vbv_take(y264_rcr *r, double bits, double rec_bits, int retry)
{
    if (!retry)
        rcr_vbv_commit(r);
    r->vbv_pend = bits;
    r->vbv_pend_rec = rec_bits;
}

/* Open from the environment. Recording and replay both need the one-thread
 * path: the order the controller consumes its inputs in is fixed there. */
/* One recorder per process, shared by every encoder instance the process
 * opens. At one thread the CLI codes a stream longer than its keyint as a
 * SEQUENCE of GOP instances, each its own encoder, so the instances append to
 * one recording in coding order and the replay's instances read it in the
 * same order. The file is finished, and the summary printed, at exit. */
static struct y264_rcr *rcr_shared;
static int rcr_users;
static void rcr_finish(void);

static y264_rcr *rcr_open_env(int threads, int width, int height)
{
    if (rcr_shared) {
        if (rcr_shared->width != width || rcr_shared->height != height) {
            fprintf(stderr, "yah264: rate-control record/replay: a second encoder of another "
                            "size in one process; ignored\n");
            return NULL;
        }
        rcr_users++;
        return rcr_shared;
    }
    const char *rec = getenv("Y264_RC_REC"), *play = getenv("Y264_RC_PLAY");
    if (!rec && !play)
        return NULL;
    if (rec && play) {
        fprintf(stderr, "yah264: Y264_RC_REC and Y264_RC_PLAY are exclusive; ignoring both\n");
        return NULL;
    }
    if (threads != 1) {
        fprintf(stderr, "yah264: rate-control record/replay needs --threads 1; ignored\n");
        return NULL;
    }
    y264_rcr *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->play = play != NULL;
    r->vbv_pend = -1.0;
    snprintf(r->path, sizeof r->path, "%s", r->play ? play : rec);
    r->width = width;
    r->height = height;
    if (r->play) {
        const char *k = getenv("Y264_RC_PLAY_K");
        r->kscale = k ? atof(k) : 1.0;
        if (!(r->kscale > 0.0)) r->kscale = 1.0;
    }
    rcr_shared = r;
    rcr_users = 1;
    atexit(rcr_finish);
    return r;
}

struct rcr_rec { uint32_t tag, seq; double v; };

/* Open the file on first use; 0 on success. */
static int rcr_lazy(y264_rcr *r)
{
    if (r->fp)
        return 0;
    if (r->broken)
        return -1;
    uint32_t hdr[4];
    r->fp = fopen(r->path, r->play ? "rb" : "wb");
    if (!r->fp) {
        fprintf(stderr, "yah264: rate-control %s: cannot open %s\n",
                r->play ? "replay" : "record", r->path);
        r->broken = 1;
        return -1;
    }
    if (r->play) {
        if (fread(hdr, sizeof hdr, 1, r->fp) != 1 || memcmp(hdr, "Y264RCR1", 8) != 0 ||
            hdr[2] != (uint32_t)r->width || hdr[3] != (uint32_t)r->height) {
            fprintf(stderr, "yah264: rate-control replay: %s is not a recording of a "
                            "%dx%d encode\n", r->path, r->width, r->height);
            fclose(r->fp);
            r->fp = NULL;
            r->broken = 1;
            return -1;
        }
    } else {
        memcpy(hdr, "Y264RCR1", 8);
        hdr[2] = (uint32_t)r->width;
        hdr[3] = (uint32_t)r->height;
        fwrite(hdr, sizeof hdr, 1, r->fp);
    }
    return 0;
}

static void rcr_put(y264_rcr *r, int tag, double v)
{
    if (rcr_lazy(r) < 0)
        return;
    struct rcr_rec x = { (uint32_t)tag, r->seq++, v };
    fwrite(&x, sizeof x, 1, r->fp);
}

/* The next recorded value, which must carry `tag`. A mismatch means the
 * encoder under replay consumed its inputs in another order than the
 * recording did -- the flow itself changed -- and nothing after it can be
 * trusted; it is counted and reported, and 0 is returned. */
static double rcr_get(y264_rcr *r, int tag)
{
    struct rcr_rec x;
    if (rcr_lazy(r) < 0 || r->eof || fread(&x, sizeof x, 1, r->fp) != 1) {
        if (!r->eof && !r->first_diff[0])
            snprintf(r->first_diff, sizeof r->first_diff,
                     "recording ended at seq %u, wanted %s", r->seq, rcr_tag_name[tag]);
        r->eof = 1;
        r->tag_err++;
        return 0.0;
    }
    if ((int)x.tag != tag || x.seq != r->seq) {
        if (!r->first_diff[0])
            snprintf(r->first_diff, sizeof r->first_diff,
                     "order differs at seq %u: recorded %s, replay wants %s", r->seq,
                     x.tag < RCR_NTAGS ? rcr_tag_name[x.tag] : "?", rcr_tag_name[tag]);
        r->tag_err++;
    }
    r->seq++;
    return x.v;
}

/* RCR_VAL(e, tag, expr): the value of expr, recorded; under replay the recorded
 * value, and expr is not evaluated. */
#define RCR_VAL(r, tag, expr) \
    ((r) ? ((r)->play ? rcr_get((r), (tag)) : rcr_val_put((r), (tag), (expr))) : (expr))

static double rcr_val_put(y264_rcr *r, int tag, double v)
{
    rcr_put(r, tag, v);
    return v;
}

/* A slice is about to be coded at (base, frame QP). Records the pair, or
 * under replay compares it with the recording. */
static void rcr_slice_qp(y264_rcr *r, int base, int fqp)
{
    double v = (double)(base * 64 + fqp);
    if (!r->play) {
        rcr_put(r, RCR_QP, v);
        return;
    }
    double rv = rcr_get(r, RCR_QP);
    int rbase = (int)rv / 64, rfqp = (int)rv % 64;
    r->slices++;
    r->last_qp_rec = rfqp;
    r->cur_qp_diff = rfqp != fqp;
    if (r->cur_qp_diff) {
        r->qp_diff++;
        if (!r->first_diff[0])
            snprintf(r->first_diff, sizeof r->first_diff,
                     "slice %ld: recorded QP %d (base %d), replay %d (base %d)",
                     r->slices, rfqp, rbase, fqp, base);
    } else {
        r->qp_match++;
    }
}

/* The slice's size in RBSP bytes: recorded, or under replay the recorded
 * size, scaled when the QP differs. */
static size_t rcr_slice_size(y264_rcr *r, size_t sz, int fqp, int retry)
{
    if (!r->play) {
        rcr_put(r, RCR_SIZE, (double)sz);
        rcr_vbv_take(r, 8.0 * (double)sz, 8.0 * (double)sz, retry);
        r->slices++;
        return sz;
    }
    double rs = rcr_get(r, RCR_SIZE);
    double s = rs;
    if (r->cur_qp_diff) {
        s = rs * pow(2.0, r->kscale * (double)(r->last_qp_rec - fqp) / 6.0);
        r->modelled++;
    }
    if (s < 1.0) s = 1.0;
    size_t out = (size_t)llround(s);
    rcr_vbv_take(r, 8.0 * (double)out, 8.0 * rs, retry);
    return out;
}

/* An encoder instance is done with the recorder; the last one out leaves
 * it open for the next instance, and rcr_finish closes it at exit. */
static void rcr_close(y264_rcr *r)
{
    if (r && r == rcr_shared && rcr_users > 0)
        rcr_users--;
    if (r && r->fp)
        fflush(r->fp);
}

static void rcr_finish(void)
{
    y264_rcr *r = rcr_shared;
    if (!r)
        return;
    rcr_shared = NULL;
    if (!r->fp) {                   /* this encoder never coded anything */
        free(r);
        return;
    }
    rcr_vbv_commit(r);
    if (r->vbv_buf > 0.0)
        fprintf(stderr, "RCREPLAY vbv: underflows=%ld min_fill=%.0f of %.0f bits over %ld "
                        "pictures (leaky-bucket estimate)\n", r->vbv_under, r->vbv_min,
                r->vbv_buf, r->pictures);
    if (!r->play)
        fprintf(stderr, "RCREPLAY recorded slices=%ld pictures=%ld bits=%.0f\n",
                r->slices, r->pictures, r->bits);
    if (r->play) {
        struct rcr_rec x;
        long left = 0;
        while (fread(&x, sizeof x, 1, r->fp) == 1) left++;
        fprintf(stderr, "RCREPLAY slices=%ld qp_match=%ld qp_diff=%ld modelled=%ld "
                        "order_errors=%ld unread=%ld bits_rec=%.0f bits_play=%.0f %s\n",
                r->slices, r->qp_match, r->qp_diff, r->modelled, r->tag_err, left,
                r->bits_rec, r->bits_play,
                (r->qp_diff == 0 && r->tag_err == 0 && left == 0) ? "EXACT" : "DIFFERS");
        if (r->first_diff[0])
            fprintf(stderr, "RCREPLAY first difference: %s\n", r->first_diff);
    }
    fclose(r->fp);
    free(r);
}

#endif
