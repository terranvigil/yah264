/*
 * yah264.h - public API for the yah264 H.264/AVC encoder
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This header is an independent work. It follows the general shape common to
 * C video-encoder APIs (a parameter struct, picture in, NAL units out) but its
 * text is original to this project. See CONTRIBUTING.md.
 */
#ifndef YAH264_H
#define YAH264_H

#include <stdint.h>
#include <stddef.h>
/* Sample type. The library is built for one bit depth and a caller must use a
 * header matching that build; YAH264_ABI_VERSION covers the mismatch. Defined
 * here rather than pulled from an internal header so this file stands alone --
 * it is the only header a consumer needs. A consumer that builds against the
 * installed library gets the right value from pkg-config's Cflags; one that
 * does not should check yah264_bit_depth() below, because the default here is
 * a guess and a wrong guess mistypes every sample. */
#ifndef Y264_BIT_DEPTH
#define Y264_BIT_DEPTH 8
#endif
#if Y264_BIT_DEPTH > 8
typedef uint16_t pixel;
#else
typedef uint8_t  pixel;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define YAH264_VERSION_MAJOR 0
#define YAH264_VERSION_MINOR 1
#define YAH264_VERSION_PATCH 0

/* Chroma sampling (planar Y, Cb, Cr).
 *
 * These are x264's X264_CSP_* values for the three planar YUV formats this
 * encoder supports, so porting X264_CSP_I420 / _I422 / _I444 gets you the
 * format you asked for. The numbering is deliberately SPARSE for that reason:
 * the gaps are x264's formats we do not implement (I400, NV12/NV21, YV12/YV16,
 * packed YUYV/UYVY/V210, YV24, and the RGB family), and every one of them is
 * REJECTED by yah264_encoder_open rather than approximated. A ported csp
 * therefore either encodes the format you named or fails to open. There is no
 * value that quietly encodes something else.
 *
 * What is NOT adopted is x264's flag machinery. X264_CSP_MASK, _VFLIP and
 * _HIGH_DEPTH are bits layered on top of these values, and this encoder
 * implements none of them -- bit depth is a compile-time property here
 * (Y264_BIT_DEPTH). `X264_CSP_I420 | X264_CSP_HIGH_DEPTH` is not 4:2:0 to us,
 * it is an unknown value, and open fails. Do not mask; pass one constant. */
typedef enum {
    YAH264_CSP_I420 = 2,   /* 4:2:0 — chroma half-width, half-height */
    YAH264_CSP_I422 = 6,   /* 4:2:2 — chroma half-width, full-height */
    YAH264_CSP_I444 = 12,  /* 4:4:4 — chroma full-resolution */
} yah264_csp_t;

/* NAL unit types we emit (subset of ITU-T H.264 Table 7-1). */
typedef enum {
    YAH264_NAL_UNKNOWN  = 0,
    YAH264_NAL_SLICE    = 1,   /* coded slice of a non-IDR picture */
    YAH264_NAL_SLICE_IDR = 5,  /* coded slice of an IDR picture */
    YAH264_NAL_SEI      = 6,   /* supplemental enhancement information */
    YAH264_NAL_SPS      = 7,   /* sequence parameter set */
    YAH264_NAL_PPS      = 8,   /* picture parameter set */
} yah264_nal_type_t;

/* nal_ref_idc values. */
typedef enum {
    YAH264_NAL_PRIORITY_DISPOSABLE = 0,
    YAH264_NAL_PRIORITY_HIGH       = 3,
} yah264_nal_priority_t;

/* One output NAL unit. The payload lives in an encoder-owned buffer that stays
 * valid until the next call to the encoder. It is a complete Annex-B unit: a
 * start code (00 00 00 01) followed by the emulation-prevented RBSP. */
typedef struct {
    int      type;          /* yah264_nal_type_t */
    int      ref_idc;       /* yah264_nal_priority_t */
    size_t   size;          /* bytes in payload, including the start code */
    uint8_t *payload;       /* Annex-B bytes */
} yah264_nal_t;

/* A raw input picture. Planes point at caller-owned memory. */
typedef struct {
    int       csp;          /* yah264_csp_t */
    int       width;
    int       height;
    int64_t   pts;
    pixel    *plane[3];     /* Y, Cb, Cr */
    int       stride[3];    /* samples (pixel elements) per row for each plane */
} yah264_picture_t;

/* ===========================================================================
 * PORTING x264 CODE? READ THIS FIRST.
 *
 * yah264_param_t does not share x264's numbering. Several fields take values
 * that are legal in both encoders and mean DIFFERENT THINGS in each. Because
 * the ported value is always in range, the assignment compiles, the encode
 * succeeds, and you get a different tool than you asked for -- no error, no
 * warning, nothing in the bitstream to say so. You find out from the file size.
 *
 * NONE OF THIS APPLIES TO THE CLI, which is x264-compatible and maps every
 * flag onto the values below for you. The trap is the C API alone.
 *
 * There are two shapes of it.
 *
 * (1) OFF IS A NEGATIVE, NOT ZERO.
 *
 * Twenty fields in this struct read zero as "unset, pick the default", so
 * zero was never available to mean off. x264 spells both of these off as
 * zero. Use the constants and you cannot get it backwards:
 *
 * scenecut 0 here = the default 40 (x264's own aggressiveness).
 * x264's --scenecut 0 = off. Off here:
 * YAH264_SCENECUT_OFF.
 *
 * sync_lookahead inverted in BOTH directions. 0 here = auto (a lead of
 * bframes+1); x264's 0 = off. Negative here = off;
 * x264's -1 (<reference-internal>) = auto. Porting
 * either value gets you the other behaviour. Off here:
 * YAH264_SYNC_LOOKAHEAD_OFF.
 *
 * Any negative value means off, so a bare negative works as well as the
 * constant.
 *
 * (2) THE ENUMS CARRY x264'S VALUES.
 *
 * rc.method, me_method, direct and csp use x264's numbering, and the cases
 * x264 has that this encoder does not are REJECTED by yah264_encoder_open
 * instead of being narrowed to something nearby. Every ported value either
 * does what it says or fails to open.
 *
 * A caller compiled against an ABI-version-0 header (the pre-x264 numbering)
 * and linked against this one passes the old numbers and gets the wrong tool
 * -- the exact failure this numbering exists to remove, pointed the other
 * way. RECOMPILING IS MANDATORY, not optional. See YAH264_ABI_VERSION below
 * and docs/options.md.
 *
 * Two values are ours alone and are deliberately parked where no future
 * x264 addition can reach them:
 *
 * YAH264_RC_2PASS (100) x264 has no 2-pass rc method; it spells 2-pass
 * as ABR plus b_stat_read/b_stat_write. Parked
 * far above X264_RC_*'s dense range so a fourth
 * x264 method cannot land on it.
 *
 * YAH264_ME_AUTO (-1) x264 has no auto; its me_method is always
 * explicit. Any non-negative home for auto is a
 * seat X264_ME_* might one day want (it already
 * uses 0..4), so auto is negative -- which is
 * also how this struct already spells auto for
 * subpel and sync_lookahead.
 *
 * What still does not line up, and cannot be fixed by renumbering:
 *
 * rc.rf x264's rate factor is a float; this is a `double`, so
 * porting a float CRF works. Double rather than float on
 * purpose: assigning x264's float to it is exact. 0 means
 * "CRF not armed" rather than x264's lossless --crf 0;
 * that is the zero-as-unset convention of class (1), not a
 * scale problem.
 *
 * subme NOT renumbered and NOT inverted. The scale runs the same
 * direction as x264's subpel level (higher = slower, more
 * RD) and the tiers line up. The only disagreement is at
 * zero: 0 here is the library default 10, the SLOWEST
 * setting, where x264's 0 is a real mode and its FASTEST.
 * That is class (1) above -- zero-as-unset -- shared with
 * nineteen other fields, so it is left alone deliberately.
 * Porting 0 for speed maximises effort. Ask for 1.
 *
 * Fields not listed agree with x264 at zero, or have no x264 equivalent.
 * =========================================================================== */
#define YAH264_SCENECUT_OFF        (-1)
#define YAH264_SYNC_LOOKAHEAD_OFF  (-1)

/* Bumped whenever the meaning of a value in yah264_param_t changes under a
 * caller. 0 = the original numbering; 1 = x264-matched (this header). */
#define YAH264_ABI_VERSION         1

/* rc.method. CQP/CRF/ABR are X264_RC_*'s values. 2PASS is ours; see above. */
#define YAH264_RC_CQP              0
#define YAH264_RC_CRF              1
#define YAH264_RC_ABR              2
#define YAH264_RC_2PASS            100

/* me_method. DIA/HEX/UMH are X264_ME_*'s values. AUTO is ours; see above.
 * X264_ME_ESA (3) and X264_ME_TESA (4) have no equivalent and are refused by
 * yah264_encoder_open rather than rounded down to UMH. */
#define YAH264_ME_AUTO             (-1)
#define YAH264_ME_DIA              0
#define YAH264_ME_HEX              1
#define YAH264_ME_UMH              2

/* direct. X264_DIRECT_PRED_*'s values. X264_DIRECT_PRED_NONE (0) and
 * X264_DIRECT_PRED_AUTO (3) are not implemented and are refused by
 * yah264_encoder_open rather than read as spatial. */
#define YAH264_DIRECT_SPATIAL      1
#define YAH264_DIRECT_TEMPORAL     2
#define YAH264_DIRECT_AUTO         3   /* per slice, by the running skippability score (the default) */

/* Encoder parameters. Zero-initialise, then yah264_param_default. */
typedef struct {
    int width;
    int height;
    int csp;                /* yah264_csp_t */

    struct {
        int fps_num;        /* frame rate numerator */
        int fps_den;        /* frame rate denominator */
    } timebase;

    int threads;            /* THREAD BUDGET FOR THIS ENCODER INSTANCE.
 * 0 = auto (the library resolves it from the machine,
 * then clamps to what the picture can use), 1 = serial,
 * N = up to N. This is the only threading value a caller
 * needs to set. GOP-parallelism is NOT this: it is a
 * caller construct, several encoder instances fed
 * separate GOPs, and the CLI implements it.
 * yah264_threads_auto() reports what 0 resolves to. */
    int frame_threads;      /* in-frame row-wavefront threads (0/1 = serial). >1
 * runs pass-1 analysis on the wavefront (deterministic,
 * ~4-5x); the CLI budget-splits threads across GOP x
 * frame. Its output differs from serial by the
 * BD-neutral predecessor/WPP pricing (standard threading
 * trade), but is identical at any frame_threads >= 2. */
    int sync_lookahead;     /* decoupled-lookahead lead, x264's --sync-lookahead:
 * how many extra input frames the encoder buffers so
 * the lookahead chain can run AHEAD of the encode on
 * its own thread. Costs exactly that many frames of
 * latency (encode returns no NAL for the first
 * sync_lookahead calls) and never changes a bit.
 * 0 = auto: bframes+1 (x264's own magnitude) once the
 * frame wavefront pool is wide enough to run a
 * lookahead chain against, else 0.
 * OFF = YAH264_SYNC_LOOKAHEAD_OFF (any negative):
 * zero added latency, chain inline. NOT 0 -- this
 * is inverted against x264 in BOTH directions; see
 * the porting warning above the struct. */
    int keyint;             /* max frames between IDR keyframes (>= 1) */
    int keyint_min;         /* min frames between IDRs: a scene cut closer than
 * this to the last keyframe is not promoted. 0 =
 * auto (keyint/10). Clamped to [1, keyint/2+1].
 * See the note on x264's auto rule in encoder.c. */
    int scenecut;           /* adaptive-I aggressiveness, x264's --scenecut:
 * higher inserts more extra keyframes. 0 = library
 * default (40, x264's).
 * OFF = YAH264_SCENECUT_OFF (any negative): no
 * adaptive cuts at all, only keyint places IDRs.
 * NOT 0 -- x264's --scenecut 0 is off, ours is the
 * default 40. See the warning above the struct. */
    int bframes;            /* consecutive B frames between anchors (0 = none) */
    int ref;                /* P-frame list-0 reference count (1 = single ref) */
    int cabac;              /* 1 = CABAC entropy coding, 0 = CAVLC */
    int subme;              /* subpel/analysis level (x264-style): higher = more
 * exhaustive RD, slower. <=8 enables the fast
 * SATD-partition path; >=9 does full RD per partition.
 * 0 = library default (max quality, i.e. 10).
 * The scale itself matches x264's subpel level
 * (same direction, tiers line up); only 0 differs,
 * and it is not renumberable -- x264's 0 is its
 * FASTEST mode, ours is the library default 10.
 * Porting 0 for speed gets you maximum effort. Ask
 * for 1. See the porting warning above. */
    int subpel;             /* subpel refinement pattern (speed/quality knob,
 * independent of subme): -1 = auto (8-neighbour square
 * iterated to convergence, the max-quality default),
 * 1 = 4-point diamond, 2 = capped diamond (x264 subme-7
 * style, cheapest). Presets set this; Y264_SUBPEL env
 * overrides. */
    int me_method;          /* ME search method (x264-style --me), decoupled from
 * --preset: YAH264_ME_AUTO follows subme (hex at
 * medium/fast, UMH at slow+), else _DIA/_HEX/_UMH.
 * _DIA/_HEX/_UMH are X264_ME_*'s values. There is
 * no ESA/TESA here, so X264_ME_ESA (3) and
 * X264_ME_TESA (4) fail encoder_open rather than
 * quietly running UMH. AUTO is negative because
 * x264 has no auto and every non-negative seat
 * belongs to X264_ME_*. NOTE that auto is
 * therefore NOT zero: a param struct that skips
 * yah264_param_default asks for _DIA. */
    int badapt;             /* adaptive B placement (needs bframes + lookahead) */
    int direct;             /* B direct MV derivation: YAH264_DIRECT_AUTO (the
 * default: each B slice picks spatial or temporal by
 * the running count of macroblocks each derivation
 * would have made skippable), _SPATIAL or _TEMPORAL
 * to pin one. The values are x264's; its NONE (0) is
 * not accepted. */
    int transform8x8;       /* 1 = allow 8x8 transform + intra (High profile) */
    int cqm;                /* quant matrices: 0 = flat, 1 = JVT default (High) */
    float aq_strength;      /* variance-AQ strength (0 = off, ~1.0 typical).
                             * Default 0.4, which is what every shipped non-CQP
                             * encode runs. encoder_open forces 0 at CQP. */
    int sei;                /* 1 = emit a settings SEI (x264-style user data) */
    int sar_num;            /* sample aspect ratio W:H (0 = unspecified/square) */
    int sar_den;
    int level_idc;          /* forced H.264 level*10 (e.g. 31 = 3.1); 0 = auto */
    float psy_rd;           /* psy-RD strength (0 = off, ~1.0 typical) */
    float psy_trellis;      /* psy-trellis strength (0 = off; ~1.0-1.2 for grain) */
    int trellis;            /* RDOQ placement, x264-compatible: 0 = off (plain
 * deadzone quantiser everywhere), 1 = on the final
 * macroblock only, 2 = in every mode decision.
 * Default 1. */

    struct {
        int method;         /* YAH264_RC_CQP / _CRF / _ABR / _2PASS. The first
 * three are X264_RC_*'s values; _2PASS is ours and
 * is parked at 100. Any other value fails
 * encoder_open. */
        int qp;             /* constant QP, 0..51 (_CQP). A real value: 0
 * means QP 0, not "unset". */
        double rf;          /* CRF target (_CRF), e.g. 23.0. Double rather than
 * x264's float so assigning its rate factor to it is
 * exact. 0 leaves CRF unarmed (zero-as-unset, see
 * the porting warning) where x264's --crf 0 is
 * lossless. */
        int bitrate;        /* target bitrate in kbit/s (_ABR) */
        /* ABR allocation model (_ABR only), default 0 = the shipped one:
 * since 2026-09-02 the CRF path plus a rate factor (the single-pass
 * design x264 documents for mb-tree; Y264_ABR_RF2=0 selects the older per-frame
 * complexity model). 1 selects the earlier rate-factor experiment
 * described below, kept for measurement.
 *
 * 1 selects x264's: a self-normalising rate factor for P, with I and B
 * anchored to the running non-B QP track. It is markedly better at
 * SPENDING a given bitrate -- the I/P/B split lands within a few percent
 * of x264's own where the default's I frames are 3.4x too small, and the
 * ABR band measures a median -5.65% BD-rate -- and markedly worse at
 * HITTING it on one class of content: where the complexity signal
 * collapses (a near-black opening), the rate factor runs away and the
 * default's cruder swing limit is what absorbs that today. sintel reads
 * +24.5% over target at 900 frames.
 *
 * So it is opt-in rather than default: choose it when quality per bit
 * matters more than landing the target exactly. */
        int abr_model;
        int vbv_maxrate;    /* VBV peak bitrate in kbit/s (0 = off) */
        int vbv_bufsize;    /* VBV buffer size in kbit (0 = off) */
        /* --- composable VBV segments -------------------------------------
 * A caller that codes one continuous stream through one encoder leaves
 * this at 0: the buffer starts full.
 *
 * A GOP-parallel caller does NOT get that. It opens one encoder per GOP
 * and concatenates the bitstreams, so every segment would start its
 * buffer full while a decoder's is wherever the previous segment left
 * it. Setting this says "something precedes me in the output", and the
 * encoder starts the buffer at the handoff occupancy instead of full.
 *
 * Concatenation is then safe by induction, and the reason it needs only
 * one flag is that the handoff level is chosen to be vbv_fill_budget's
 * own fixed point. That budget solves for a frame that lands the
 * occupancy back on vbv_size/2 and allows a climb when it is below, so
 * a segment starting there is a segment starting at the level its own
 * rate loop returns to. It exits there without being told to.
 *
 * The level is therefore NOT a parameter. A caller that picked one
 * independently of the encoder could silently disagree with it, and a
 * disagreement is exactly the bug this exists to prevent. */
        int vbv_seg_join;       /* 1 = this segment follows another in the
 * output stream, so do not assume a full
 * buffer. 0 = starts the stream, or is the
 * whole stream. */
        int pass;           /* 2-pass: 1 = analysis (write stats), 2 = final (read) */
        const char *stats;  /* 2-pass stats file path */
        /* Pass-2 bit budget for THIS encoder instance, in bits. 0 = derive it
 * from bitrate x (frames in the stats file) / fps, which is right only
 * when the instance codes the whole stream. A GOP-parallel caller hands
 * each per-GOP encoder its own slice of the stats file, so it must also
 * hand it that slice's share of the global budget -- otherwise every GOP
 * gets the stream-average rate and 2-pass stops moving bits between
 * hard and easy GOPs, which is the whole point of the mode. */
        double tp_target_bits;
        int lookahead;      /* lookahead window in frames (mb-tree propagation depth) */
    } rc;

    int annexb;             /* 1 = emit Annex-B start codes (the only mode) */
} yah264_param_t;

typedef struct yah264_encoder yah264_encoder_t;

/* Library version string, e.g. "0.0.0". */
/* Everything this header declares is exported; everything else in the library
 * is not. The shared build compiles with hidden visibility so the dylib's own
 * internal calls stay direct rather than routing through the export table on
 * every kernel dispatch, which means the public entry points have to say so
 * explicitly or the shared object ships with nothing in it. */
#if defined(__GNUC__) || defined(__clang__)
#  define YAH264_API __attribute__((visibility("default")))
#else
/* MSVC would want dllexport when building and dllimport when consuming, which
 * needs a build-time define this project does not yet set. Windows is not a
 * tested target, so rather than ship a half-right guess, export nothing
 * special and leave it for whoever ports it. */
#  define YAH264_API
#endif

YAH264_API const char *yah264_version(void);

/* Bit depth this library was BUILT for, which a caller cannot infer from the
 * header alone: Y264_BIT_DEPTH defaults to 8 above when nothing defines it, and
 * the 8-bit and 10-bit builds install under the same soname, so a 10-bit dylib
 * loads happily under a header that has typed `pixel` as uint8_t and every
 * plane is then read at half its stride. pkg-config's Cflags carry the right
 * -DY264_BIT_DEPTH, so the compile-time path is covered; this is for the caller
 * that linked by hand or had the dylib swapped underneath it. Compare it
 * against Y264_BIT_DEPTH at open and refuse the mismatch. */
YAH264_API int yah264_bit_depth(void);

/* Space-separated list of CPU features the encoder auto-detected and will use
 * for kernel dispatch on this machine (e.g. "neon dotprod i8mm"), or "scalar".
 * The returned string is owned by the library. */
YAH264_API const char *yah264_cpu_features(void);

/* Fill param with defaults. Safe to call on a zeroed struct. */
YAH264_API void yah264_param_default(yah264_param_t *param);

/* 2-pass: the weight one pass-1 stats record contributes to the pass-2 bit
 * allocation (the QP-invariant coding cost, complexity-compressed). Exposed so
 * a caller that splits a stats file along GOP boundaries can size each GOP's
 * share of the budget with the encoder's own formula instead of a copy of it. */
double yah264_2pass_stat_weight(double bits, int qp);

/* Apply a named speed preset ("ultrafast".."placebo"). Returns 0 on success,
 * -1 on an unknown name. */
YAH264_API int yah264_param_apply_preset(yah264_param_t *param, const char *preset);

/* Open an encoder for the given parameters. Returns NULL on error. */
YAH264_API yah264_encoder_t *yah264_encoder_open(const yah264_param_t *param);

/* Retrieve the sequence headers (SPS, PPS). On return *nal points at an array of
 * *count NAL units owned by the encoder. Returns 0 on success. */
YAH264_API int yah264_encoder_headers(yah264_encoder_t *enc,
                            yah264_nal_t **nal, int *count);

/* Encode one picture. On success returns the total number of bytes across the
 * emitted NAL units (>= 0) and sets *nal / *count to the encoder-owned output,
 * valid until the next call into the encoder.
 *
 * A call returns ZERO OR MORE NAL units, and a frame's NAL may be returned by a
 * LATER call than the one that submitted the picture : frames are
 * reordered/buffered, and with threading the final entropy emit of a burst can
 * stay in flight across the API boundary so the next call's analysis hides it.
 * NAL units are always returned in coding order. pic == NULL flushes: call it
 * repeatedly at end of stream until it returns 0 bytes with *count == 0. */
YAH264_API int yah264_encoder_encode(yah264_encoder_t *enc,
                           yah264_nal_t **nal, int *count,
                           const yah264_picture_t *pic);

/* Point *pic at the encoder's reconstruction of the most recently encoded
 * picture (cropped to the coded width/height). The planes are owned by the
 * encoder and valid until the next encode call. Returns 0 on success, -1 if no
 * frame has been encoded yet. Used to verify that the encoder's internal
 * reconstruction matches an independent decoder's output. */
YAH264_API int yah264_encoder_get_recon(yah264_encoder_t *enc, yah264_picture_t *pic);

/* Register a callback invoked once per emitted frame, in coding order, with the
 * frame's reconstruction (cropped) and its display index (input order). This is
 * the only way to capture every frame's recon when B-frames reorder coding vs
 * display order: a single encode call can emit an anchor plus several B's.
 * The picture planes are valid only for the duration of the callback. Pass
 * cb = NULL to clear. */
YAH264_API void yah264_encoder_set_recon_cb(yah264_encoder_t *enc,
                                  void (*cb)(void *ud, const yah264_picture_t *rec,
                                             int disp_index),
                                  void *ud);

/* The widest in-frame (frame_threads) row-wavefront a picture of this size can
 * use. The wavefront's cell (r,c) waits on (r-1,c+1), which fixes a critical
 * path of 2*(rows-1)+cols cell-times whatever the worker count, so past
 * work/critical-path workers a frame has no row left to hand out and the extra
 * threads buy only wake traffic. The encoder applies this to param.frame_threads
 * itself; it is public so a caller doing its own decomposition (as the CLI's
 * GOP splitter does) can spend the refused share somewhere it will be used
 * rather than handing out threads the encoder will decline.
 *
 * Depends on nothing but the picture size -- same answer on every machine, and
 * it never changes a bitstream. */
/* Take up to max display indices from the encoder's emitted-frame FIFO, in
 * CODING order, removing them. Returns how many were taken.
 *
 * A muxer needs this. B-frames are coded after the anchor that follows them in
 * display order, so one call can emit an anchor plus several B's and a caller
 * pairing packets with input timestamps in arrival order gets every B wrong:
 * the file plays with its presentation timestamps running backwards, which
 * looks like stutter rather than like an error. Drain it by the number of
 * packets you split, not once per encode call: a frame's finalisation and its
 * NAL are decoupled, so a call can finalise more frames than it appends NALs
 * for. Indices count input frames from zero, so a caller holding its own array
 * of timestamps indexes into it. */
YAH264_API int yah264_encoder_frame_order(yah264_encoder_t *enc, int *disp, int max);

YAH264_API int yah264_frame_thread_cap(int width, int height);

/* What param.threads = 0 resolves to on this machine: every online core, cached.
 * Exported so a caller splitting work across several encoder instances sizes its
 * budget from the same number the library would have used, instead of asking the
 * OS separately and drifting. Clamp per instance with yah264_frame_thread_cap:
 * this is how much machine exists, not how much one picture can use. */
YAH264_API int yah264_threads_auto(void);

/* Frames of input latency these parameters add through the decoupled
 * lookahead's lead -- i.e. param.sync_lookahead resolved (auto, explicit, or
 * off) and clamped the way encoder_open clamps it. encode returns no NAL for
 * that many calls beyond the B-frame reorder delay, which this does not include
 * and does not change. The lead never changes a bit, so this number is the
 * whole cost of it and a latency-sensitive caller should be shown it. */
YAH264_API int yah264_lookahead_delay(const yah264_param_t *param);

/* Scene-cut pre-scan, for callers that split an input into independent GOP
 * encodes and want their boundaries to land on the real cuts instead of on
 * ceil(frames/keyint) arithmetic.
 *
 * Given the whole input's luma planes in display order, writes into idr[0..n) a
 * 1 for every frame this encoder's own lookahead would code as an IDR -- a
 * scene cut, or keyint frames since the last one -- and returns how many.
 * Returns -1 on bad arguments or allocation failure.
 *
 * Analysis only: it opens no encoder and emits no bits. The answer depends on
 * the input, the width/height and keyint/bframes alone, so it is the same at
 * any `nthreads`; nthreads only says how much of the machine to scan with. */
YAH264_API int yah264_scan_idr_frames(const yah264_param_t *param,
                            const pixel *const *luma, const int *stride,
                            int n, int nthreads, unsigned char *idr);

/* Close the encoder and free all resources. */
YAH264_API void yah264_encoder_close(yah264_encoder_t *enc);

/* The ABR rate-controller state of an instance, for rc.carry. Fields are the
 * encoder's own; treat as opaque and pass through unchanged. */
typedef struct yah264_rc_state {
    int      valid;
    double   target_bpf;
    double   cum_target, cum_actual;           /* the overflow ledger */
    double   qp, scale[3], calqp[3];           /* the default model */
    int      inited[3], cal[3];
    double   rf_cplx_sum, rf_wanted_bits;    /* the rate-factor models */
    double   ptrack_qp, ptrack_norm, last_ref_qp[2], last_qscale_type[3];
    double   st_cplxsum, st_cplxcount;
    int      last_nonb_type;
} yah264_rc_state_t;

/* Export the ABR state after the flush (all frames accounted). Returns 0 and
 * out->valid = 1 for an ABR instance; -1 (out->valid = 0) otherwise. */
YAH264_API int yah264_encoder_rc_state(const yah264_encoder_t *enc, yah264_rc_state_t *out);

/* --- ABR state carried across GOP-parallel encoder instances ---------------
 * A caller that opens one encoder per GOP would otherwise restart the rate
 * controller at every keyint and re-pay its startup transient (the first I
 * at the seed QP, the ramp after it) once per GOP, which a single-instance
 * encoder such as x264 never does. Call right after yah264_encoder_open and
 * before the first frame: `state` is an export from an earlier instance;
 * `frames_ahead` is how many frames of OTHER instances lie between that
 * export and this instance's first frame (GOPs still in flight), which the
 * import credits at the target rate. Returns 0, or -1 if the instance is not
 * ABR, has already coded a frame, or the state is not valid (nothing
 * changes). Functions only, so the parameter struct's layout is unchanged. */
YAH264_API int yah264_encoder_rc_import(yah264_encoder_t *enc, const yah264_rc_state_t *state,
                                        int frames_ahead);

#ifdef __cplusplus
}
#endif

#endif /* YAH264_H */
