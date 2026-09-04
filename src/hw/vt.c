/* Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause */
/* VideoToolbox H.264 behind the yah264 API (docs/videotoolbox-plan.md).
 *
 * A compression session that REQUIRES the hardware encoder; the software
 * VideoToolbox encoder is never accepted (if the hardware session fails we are
 * on our own encoder). Frames go in as planar 4:2:0 CVPixelBuffers wrapping
 * the caller's planes without a copy; the output callback runs on the
 * session's thread and converts each AVCC sample into Annex-B NAL units on a
 * mutex-guarded queue, which y264_hw_encode drains in decode order. SPS/PPS
 * come from the first sample's format description and are emitted once, ahead
 * of the first slice, as the headers call. */
#include "vt.h"
#include <VideoToolbox/VideoToolbox.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define VT_MAX_NALS 64

struct vt_out {                      /* one queued Annex-B buffer (a whole sample's NALs) */
    uint8_t *data;
    size_t   size;
    int      key;
    struct vt_out *next;
};

struct y264_hw {
    VTCompressionSessionRef sess;
    int      width, height;
    int      fps_num, fps_den;
    int64_t  pts_scale;              /* pts units per second, for CMTime */
    int      frames_in, frames_out;
    int      err;                    /* first callback error, sticky */
    int      hdr_done;               /* SPS/PPS emitted */
    uint8_t *sps, *pps; size_t sps_len, pps_len;
    pthread_mutex_t mx;
    pthread_cond_t  cv;
    struct vt_out *head, *tail;
    /* what the last encode call handed out (owned until the next call) */
    uint8_t *out_buf; size_t out_cap, out_len;
    yah264_nal_t nal[VT_MAX_NALS];
    /* header NALs (Annex B) */
    uint8_t *hdr_buf; size_t hdr_len;
    yah264_nal_t hdr_nal[2];
    char name[64];
};

static void out_push(struct y264_hw *h, struct vt_out *o)
{
    pthread_mutex_lock(&h->mx);
    if (h->tail) h->tail->next = o; else h->head = o;
    h->tail = o;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mx);
}

/* Copy the parameter sets out of the format description once. */
static int take_parameter_sets(struct y264_hw *h, CMFormatDescriptionRef fd)
{
    if (h->sps) return 0;
    const uint8_t *p = NULL; size_t n = 0; int hlen = 4;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, &p, &n, NULL, &hlen) != noErr) return -1;
    h->sps = malloc(n); if (!h->sps) return -1; memcpy(h->sps, p, n); h->sps_len = n;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 1, &p, &n, NULL, &hlen) != noErr) return -1;
    h->pps = malloc(n); if (!h->pps) return -1; memcpy(h->pps, p, n); h->pps_len = n;
    return hlen;
}

static void vt_output(void *ref, void *src_ref, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sb)
{
    struct y264_hw *h = ref;
    (void)src_ref;
    if (status != noErr || !sb) {
        pthread_mutex_lock(&h->mx); if (!h->err) h->err = status ? (int)status : -1; pthread_cond_broadcast(&h->cv); pthread_mutex_unlock(&h->mx);
        return;
    }
    if (flags & kVTEncodeInfo_FrameDropped) { pthread_mutex_lock(&h->mx); h->frames_out++; pthread_cond_broadcast(&h->cv); pthread_mutex_unlock(&h->mx); return; }
    CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
    int hlen = 4;
    if (fd) { int r = take_parameter_sets(h, fd); if (r > 0) hlen = r; }
    int key = 1;
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (att && CFArrayGetCount(att) > 0) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(att, 0);
        if (d && CFDictionaryGetValue(d, kCMSampleAttachmentKey_NotSync)) key = 0;
    }
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    size_t total = 0; char *base = NULL;
    if (!bb || CMBlockBufferGetDataPointer(bb, 0, NULL, &total, &base) != noErr || !base) {
        pthread_mutex_lock(&h->mx); if (!h->err) h->err = -2; pthread_cond_broadcast(&h->cv); pthread_mutex_unlock(&h->mx);
        return;
    }
    /* AVCC (length-prefixed) -> Annex B: same byte count per NAL (4-byte length
     * becomes a 4-byte start code); for other header lengths the buffer grows. */
    struct vt_out *o = calloc(1, sizeof *o);
    if (!o) return;
    o->data = malloc(total + 4 * 16);
    if (!o->data) { free(o); return; }
    size_t pos = 0, w = 0;
    const uint8_t *s = (const uint8_t *)base;
    while (pos + (size_t)hlen <= total) {
        uint32_t len = 0;
        for (int i = 0; i < hlen; i++) len = (len << 8) | s[pos + i];
        pos += hlen;
        if (len == 0 || pos + len > total) break;
        if (w + 4 + len > total + 4 * 16) {
            uint8_t *n = realloc(o->data, w + 4 + len + 4 * 16);
            if (!n) break;
            o->data = n;
        }
        o->data[w] = 0; o->data[w + 1] = 0; o->data[w + 2] = 0; o->data[w + 3] = 1;
        memcpy(o->data + w + 4, s + pos, len);
        w += 4 + len; pos += len;
    }
    o->size = w; o->key = key;
    pthread_mutex_lock(&h->mx); h->frames_out++; pthread_mutex_unlock(&h->mx);
    out_push(h, o);
}

static void set_i32(VTCompressionSessionRef s, CFStringRef key, int v)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    if (n) { VTSessionSetProperty(s, key, n); CFRelease(n); }
}
static void set_f64(VTCompressionSessionRef s, CFStringRef key, double v)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &v);
    if (n) { VTSessionSetProperty(s, key, n); CFRelease(n); }
}

/* --crf onto the hardware's 0..1 quality key (docs/videotoolbox-plan.md step
 * 0 item 1): a fixed monotone table, RATE-matched, not quality-matched. Each
 * point is the quality at which the hardware spends what yah264 spends at
 * that CRF, the median over six clips (foreman, samsung, park_joy, sunflower,
 * pedestrian, riverbed; 150 frames; 2026-09-04): crf 20 -> 0.69, 24 -> 0.59,
 * 28 -> 0.50, 32 -> 0.41, 36 -> 0.30 (per-clip spread about +-0.1); linear
 * between, extrapolated at the same slope outside. Y264_HW_QUALITY overrides. */
static double crf_to_quality(double crf)
{
    static const double t[][2] = { {8, 0.98}, {12, 0.88}, {16, 0.79}, {20, 0.69}, {24, 0.59}, {28, 0.50}, {32, 0.41}, {36, 0.30}, {40, 0.20}, {44, 0.12}, {48, 0.06}, {51, 0.03} };
    int n = (int)(sizeof t / sizeof t[0]);
    if (crf <= t[0][0]) return t[0][1];
    for (int i = 1; i < n; i++)
        if (crf <= t[i][0]) {
            double f = (crf - t[i - 1][0]) / (t[i][0] - t[i - 1][0]);
            return t[i - 1][1] + f * (t[i][1] - t[i - 1][1]);
        }
    return t[n - 1][1];
}

struct y264_hw *y264_hw_open(const yah264_param_t *p, char *why, size_t whylen)
{
#define FAIL(msg) do { if (why && whylen) snprintf(why, whylen, "%s", msg); goto fail; } while (0)
    struct y264_hw *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    pthread_mutex_init(&h->mx, NULL); pthread_cond_init(&h->cv, NULL);
    h->width = p->width; h->height = p->height;
    h->fps_num = p->timebase.fps_num > 0 ? p->timebase.fps_num : 25;
    h->fps_den = p->timebase.fps_den > 0 ? p->timebase.fps_den : 1;
    h->pts_scale = h->fps_num;              /* pts counts frames: CMTime(pts * fps_den, fps_num) */
    if (p->csp != YAH264_CSP_I420) FAIL("the hardware encoder takes 4:2:0 8-bit only");
#if Y264_BIT_DEPTH != 8
    FAIL("the hardware encoder takes 8-bit input only");
#endif
    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
    CFMutableDictionaryRef src = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int fmt = kCVPixelFormatType_420YpCbCr8Planar;
    CFNumberRef fmtn = CFNumberCreate(NULL, kCFNumberIntType, &fmt);
    CFDictionarySetValue(src, kCVPixelBufferPixelFormatTypeKey, fmtn); CFRelease(fmtn);
    OSStatus st = VTCompressionSessionCreate(NULL, p->width, p->height, kCMVideoCodecType_H264, spec, src, NULL,
                                             vt_output, h, &h->sess);
    CFRelease(spec); CFRelease(src);
    if (st != noErr || !h->sess) FAIL("no hardware H.264 encoder session (VTCompressionSessionCreate failed)");
    CFBooleanRef hwacc = NULL;
    if (VTSessionCopyProperty(h->sess, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, NULL, &hwacc) == noErr && hwacc) {
        int ok = CFBooleanGetValue(hwacc); CFRelease(hwacc);
        if (!ok) FAIL("VideoToolbox offered only its software encoder; refused");
    } else
        FAIL("VideoToolbox will not say whether the session is hardware; refused");

    /* the option map (docs/videotoolbox-plan.md step 3), the part that maps */
    /* --tune zerolatency arrives as bframes 0 + no lookahead: the hardware's
     * real-time mode, which emits each frame as it is encoded. The frame
     * delay count is read-only on this hardware, so that is the whole map. */
    int low_latency = p->bframes == 0 && p->rc.lookahead == 0;
    VTSessionSetProperty(h->sess, kVTCompressionPropertyKey_RealTime, low_latency ? kCFBooleanTrue : kCFBooleanFalse);
    VTSessionSetProperty(h->sess, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    VTSessionSetProperty(h->sess, kVTCompressionPropertyKey_H264EntropyMode,
                         p->cabac ? kVTH264EntropyMode_CABAC : kVTH264EntropyMode_CAVLC);
    VTSessionSetProperty(h->sess, kVTCompressionPropertyKey_AllowFrameReordering,
                         p->bframes > 0 ? kCFBooleanTrue : kCFBooleanFalse);
    if (p->keyint > 0) set_i32(h->sess, kVTCompressionPropertyKey_MaxKeyFrameInterval, p->keyint);
    set_f64(h->sess, kVTCompressionPropertyKey_ExpectedFrameRate, (double)h->fps_num / h->fps_den);
    if (p->rc.bitrate > 0) {
        set_i32(h->sess, kVTCompressionPropertyKey_AverageBitRate, p->rc.bitrate * 1000);
        if (p->rc.vbv_maxrate > 0 && p->rc.vbv_bufsize > 0) {
            /* DataRateLimits: bytes per window, window = bufsize / maxrate seconds */
            double win = (double)p->rc.vbv_bufsize / p->rc.vbv_maxrate;
            double bytes = (double)p->rc.vbv_maxrate * 1000.0 / 8.0 * win;
            CFNumberRef b = CFNumberCreate(NULL, kCFNumberDoubleType, &bytes);
            CFNumberRef w = CFNumberCreate(NULL, kCFNumberDoubleType, &win);
            const void *vals[2] = { b, w };
            CFArrayRef arr = CFArrayCreate(NULL, vals, 2, &kCFTypeArrayCallBacks);
            if (arr) { VTSessionSetProperty(h->sess, kVTCompressionPropertyKey_DataRateLimits, arr); CFRelease(arr); }
            if (b) CFRelease(b); if (w) CFRelease(w);
        }
    } else if (p->rc.rf > 0) {
        /* Y264_HW_QUALITY: the calibration harness's override of the table */
        const char *qs = getenv("Y264_HW_QUALITY");
        double q = qs && *qs ? atof(qs) : crf_to_quality(p->rc.rf);
        if (q < 0.0) q = 0.0; if (q > 1.0) q = 1.0;
        set_f64(h->sess, kVTCompressionPropertyKey_Quality, q);
    } else if (p->rc.qp > 0) {
        /* CQP: pin the frame QP range; the hardware may still move QP inside a frame */
        set_i32(h->sess, CFSTR("MinAllowedFrameQP"), p->rc.qp);
        set_i32(h->sess, CFSTR("MaxAllowedFrameQP"), p->rc.qp);
    }
    if (VTCompressionSessionPrepareToEncodeFrames(h->sess) != noErr) FAIL("the hardware session refused to prepare");
    snprintf(h->name, sizeof h->name, "VideoToolbox H.264 (hardware)");
    return h;
fail:
    y264_hw_close(h);
    return NULL;
#undef FAIL
}

const char *y264_hw_name(const struct y264_hw *h) { return h ? h->name : "none"; }

/* The headers are known only once the first sample has come back. Callers that
 * ask before that get zero NALs and the first encode call emits them ahead of
 * the first slice instead. */
static int build_headers(struct y264_hw *h)
{
    if (h->hdr_done || !h->sps) return 0;
    size_t n = 4 + h->sps_len + 4 + h->pps_len;
    h->hdr_buf = malloc(n); if (!h->hdr_buf) return -1;
    uint8_t *w = h->hdr_buf;
    w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 1; memcpy(w + 4, h->sps, h->sps_len);
    h->hdr_nal[0].type = YAH264_NAL_SPS; h->hdr_nal[0].ref_idc = YAH264_NAL_PRIORITY_HIGH;
    h->hdr_nal[0].size = 4 + h->sps_len; h->hdr_nal[0].payload = w;
    w += 4 + h->sps_len;
    w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 1; memcpy(w + 4, h->pps, h->pps_len);
    h->hdr_nal[1].type = YAH264_NAL_PPS; h->hdr_nal[1].ref_idc = YAH264_NAL_PRIORITY_HIGH;
    h->hdr_nal[1].size = 4 + h->pps_len; h->hdr_nal[1].payload = w;
    h->hdr_len = n;
    return 2;
}

int y264_hw_headers(struct y264_hw *h, yah264_nal_t **nal, int *count)
{
    if (!h) return -1;
    if (build_headers(h) == 2) { h->hdr_done = 1; *nal = h->hdr_nal; *count = 2; return 0; }
    *nal = NULL; *count = 0;                /* not known yet: emitted with the first slice */
    return 0;
}

/* Drain the queue into the caller-visible buffer: every queued sample, split
 * back into NAL units by start code so each entry carries its type. */
static int drain(struct y264_hw *h, yah264_nal_t **nal, int *count, int wait_for)
{
    pthread_mutex_lock(&h->mx);
    while (wait_for && !h->err && h->frames_out < wait_for) pthread_cond_wait(&h->cv, &h->mx);
    struct vt_out *o = h->head; h->head = h->tail = NULL;
    int err = h->err;
    pthread_mutex_unlock(&h->mx);
    size_t need = 0; int prepend = 0;
    if (!h->hdr_done && h->sps && o) { build_headers(h); prepend = 1; need += h->hdr_len; }
    for (struct vt_out *q = o; q; q = q->next) need += q->size;
    if (need > h->out_cap) {
        uint8_t *n = realloc(h->out_buf, need); if (!n) { err = -3; need = 0; }
        else { h->out_buf = n; h->out_cap = need; }
    }
    size_t w = 0; int n = 0;
    if (prepend && need) {
        memcpy(h->out_buf, h->hdr_buf, h->hdr_len);
        h->nal[n] = h->hdr_nal[0]; h->nal[n].payload = h->out_buf; n++;
        h->nal[n] = h->hdr_nal[1]; h->nal[n].payload = h->out_buf + h->hdr_nal[0].size; n++;
        w = h->hdr_len; h->hdr_done = 1;
    }
    while (o) {
        struct vt_out *nx = o->next;
        if (need && w + o->size <= need) {
            memcpy(h->out_buf + w, o->data, o->size);
            /* split by start code */
            size_t s = w, end = w + o->size;
            for (size_t i = w + 4; i + 4 <= end; i++)
                if (h->out_buf[i] == 0 && h->out_buf[i + 1] == 0 && h->out_buf[i + 2] == 0 && h->out_buf[i + 3] == 1) {
                    if (n < VT_MAX_NALS) { h->nal[n].payload = h->out_buf + s; h->nal[n].size = i - s; h->nal[n].type = h->out_buf[s + 4] & 0x1f; h->nal[n].ref_idc = (h->out_buf[s + 4] >> 5) & 3; n++; }
                    s = i;
                }
            if (n < VT_MAX_NALS && end > s) { h->nal[n].payload = h->out_buf + s; h->nal[n].size = end - s; h->nal[n].type = h->out_buf[s + 4] & 0x1f; h->nal[n].ref_idc = (h->out_buf[s + 4] >> 5) & 3; n++; }
            w = end;
        }
        free(o->data); free(o); o = nx;
    }
    h->out_len = w; *nal = h->nal; *count = n;
    if (err) { pthread_mutex_lock(&h->mx); h->err = err; pthread_mutex_unlock(&h->mx); return -1; }
    return (int)w;
}

int y264_hw_encode(struct y264_hw *h, yah264_nal_t **nal, int *count, const yah264_picture_t *pic, int force_key)
{
    if (!h) return -1;
    if (!pic) {                                 /* flush */
        if (h->frames_in > h->frames_out || h->head) {
            VTCompressionSessionCompleteFrames(h->sess, kCMTimeInvalid);
            return drain(h, nal, count, h->frames_in);
        }
        *nal = NULL; *count = 0; return 0;
    }
    if (pic->csp != YAH264_CSP_I420 || pic->width != h->width || pic->height != h->height) return -1;
    /* The session keeps a frame in flight past this call (its frame delay,
     * B-frame reordering), and the caller reuses its planes on the next read,
     * so the frame is copied into a buffer from the session's pool. */
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(h->sess);
    CVPixelBufferRef pb = NULL;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess || !pb) {
        if (CVPixelBufferCreate(NULL, h->width, h->height, kCVPixelFormatType_420YpCbCr8Planar, NULL, &pb) != kCVReturnSuccess || !pb)
            return -1;
    }
    if (CVPixelBufferLockBaseAddress(pb, 0) != kCVReturnSuccess) { CVPixelBufferRelease(pb); return -1; }
    for (int c = 0; c < 3; c++) {
        uint8_t *dst = CVPixelBufferGetBaseAddressOfPlane(pb, c);
        size_t ds = CVPixelBufferGetBytesPerRowOfPlane(pb, c);
        int w = c ? (h->width + 1) / 2 : h->width, hh = c ? (h->height + 1) / 2 : h->height;
        const uint8_t *src = (const uint8_t *)pic->plane[c];
        for (int y = 0; y < hh; y++) memcpy(dst + (size_t)y * ds, src + (size_t)y * pic->stride[c], (size_t)w);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    CMTime pts = CMTimeMake(pic->pts * h->fps_den, h->fps_num);
    CMTime dur = CMTimeMake(h->fps_den, h->fps_num);
    CFDictionaryRef fopts = NULL;
    {
        /* per-frame options: our scene-cut's ForceKeyFrame, and the QP-hint
         * PROBE (Y264_HW_QPHINT=N sets a base frame QP on every frame; whether
         * the H.264 path honours it is what the probe measures, step 2) */
        const void *k[2]; const void *v[2]; int n = 0;
        CFNumberRef qn = NULL;
        if (force_key) { k[n] = kVTEncodeFrameOptionKey_ForceKeyFrame; v[n] = kCFBooleanTrue; n++; }
        const char *qh = getenv("Y264_HW_QPHINT");
        if (qh && *qh) { int q = atoi(qh); qn = CFNumberCreate(NULL, kCFNumberIntType, &q); if (qn) { k[n] = CFSTR("BaseFrameQP"); v[n] = qn; n++; } }
        if (n) fopts = CFDictionaryCreate(NULL, k, v, n, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (qn) CFRelease(qn);
    }
    OSStatus st = VTCompressionSessionEncodeFrame(h->sess, pb, pts, dur, fopts, NULL, NULL);
    if (fopts) CFRelease(fopts);
    CVPixelBufferRelease(pb);       /* the session retains it while in flight */
    if (st != noErr) return -1;
    h->frames_in++;
    return drain(h, nal, count, 0);  /* whatever has come back so far, no wait */
}

void y264_hw_close(struct y264_hw *h)
{
    if (!h) return;
    if (h->sess) { VTCompressionSessionInvalidate(h->sess); CFRelease(h->sess); }
    struct vt_out *o = h->head;
    while (o) { struct vt_out *n = o->next; free(o->data); free(o); o = n; }
    free(h->sps); free(h->pps); free(h->out_buf); free(h->hdr_buf);
    pthread_mutex_destroy(&h->mx); pthread_cond_destroy(&h->cv);
    free(h);
}
