/*
 * yah264.h - public API for the yah264 H.264/AVC encoder
 *
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This header is an independent work. It follows the general shape common to
 * C video-encoder APIs (a parameter struct, picture in, NAL units out) but its
 * text is original to this project. See CONTRIBUTING.md.
 */
#ifndef YAH264_H
#define YAH264_H

#include <stdint.h>
#include <stddef.h>
/* The sample width is a compile-time type INSIDE the encoder, so there are two
 * libraries -- an 8-bit one and a 10-bit one -- and a program may link both.
 * What this header no longer does is type the samples for the caller. There is
 * no public `pixel`: picture planes are void*, their stride is in samples, and
 * the width of a sample is whichever library you called. That is what lets one
 * yah264_picture_t serve both depths, and it is why the old failure mode -- a
 * 10-bit library loaded under a header that had typed `pixel` as uint8_t, so
 * every plane was read at half its stride -- cannot happen any more.
 *
 * WHICH library a call reaches is decided here, by the Y264_BIT_DEPTH this
 * header is compiled with: the 10-bit library's public names carry a `_10`
 * suffix and YAH264_API renames the calls. An external consumer sees no change
 * -- it writes yah264_encoder_open() either way -- and gets the right value
 * from pkg-config's Cflags (`yah264`, or `yah264_10`). A consumer that wants
 * BOTH libraries in one program is in the CLI's position: it compiles at 8 and
 * declares the suffixed entry points itself, which is what YAH264_API is
 * published for. yah264_bit_depth() still answers for a library linked by
 * hand. */
#ifndef Y264_BIT_DEPTH
#define Y264_BIT_DEPTH 8
#endif

#define YAH264_API_PASTE(a, b) a ## b
#define YAH264_API_EVAL(a, b)  YAH264_API_PASTE(a, b)
#if Y264_BIT_DEPTH == 8
#  define YAH264_API(name) name
#else
#  define YAH264_API(name) YAH264_API_EVAL(name, YAH264_API_EVAL(_, Y264_BIT_DEPTH))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if Y264_BIT_DEPTH != 8
#define yah264_version                  YAH264_API(yah264_version)
#define yah264_bit_depth                YAH264_API(yah264_bit_depth)
#define yah264_cpu_features             YAH264_API(yah264_cpu_features)
#define yah264_param_default            YAH264_API(yah264_param_default)
#define yah264_2pass_stat_weight        YAH264_API(yah264_2pass_stat_weight)
#define yah264_param_apply_preset       YAH264_API(yah264_param_apply_preset)
#define yah264_encoder_open             YAH264_API(yah264_encoder_open)
#define yah264_encoder_open_hw          YAH264_API(yah264_encoder_open_hw)
#define yah264_encoder_backend          YAH264_API(yah264_encoder_backend)
#define yah264_encoder_set_video_signal YAH264_API(yah264_encoder_set_video_signal)
#define yah264_encoder_headers          YAH264_API(yah264_encoder_headers)
#define yah264_encoder_encode           YAH264_API(yah264_encoder_encode)
#define yah264_encoder_get_recon        YAH264_API(yah264_encoder_get_recon)
#define yah264_encoder_set_recon_cb     YAH264_API(yah264_encoder_set_recon_cb)
#define yah264_encoder_frame_order      YAH264_API(yah264_encoder_frame_order)
#define yah264_encoder_set_zones        YAH264_API(yah264_encoder_set_zones)
#define yah264_encoder_set_frame_forces YAH264_API(yah264_encoder_set_frame_forces)
#define yah264_encoder_frame_stats      YAH264_API(yah264_encoder_frame_stats)
#define yah264_frame_thread_cap         YAH264_API(yah264_frame_thread_cap)
#define yah264_threads_auto             YAH264_API(yah264_threads_auto)
#define yah264_lookahead_delay          YAH264_API(yah264_lookahead_delay)
#define yah264_scan_shots               YAH264_API(yah264_scan_shots)
#define yah264_scan_idr_frames          YAH264_API(yah264_scan_idr_frames)
#define yah264_encoder_close            YAH264_API(yah264_encoder_close)
#define yah264_encoder_rc_state         YAH264_API(yah264_encoder_rc_state)
#define yah264_encoder_rc_import        YAH264_API(yah264_encoder_rc_import)
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

/* A raw input picture. Planes point at caller-owned memory.
 *
 * void*, not a typed pointer: the sample width is the LIBRARY's, uint8_t for
 * yah264 and uint16_t (native-endian) for yah264_10, and one struct serves
 * both so that a program linking both needs one picture type and one dispatch
 * table. stride is in SAMPLES, never bytes, at either depth. */
typedef struct {
    int       csp;          /* yah264_csp_t */
    int       width;
    int       height;
    int64_t   pts;
    void     *plane[3];     /* Y, Cb, Cr; samples of the library's own width */
    int       stride[3];    /* samples per row for each plane */
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
 * x264's -1 (its auto value) = auto. Porting
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
 * caller. 0 = the original numbering; 1 = x264-matched; 2 = runtime depth:
 * the public `pixel` typedef is gone, picture planes are void*, the recon
 * callback and the pre-scan carry a sample width, and the 10-bit library's
 * public names carry a _10 suffix. Source-compatible for a caller that never
 * named `pixel`; every caller must still recompile.
 *
 * 3 = the parameter struct declares its own size. `size` is the FIRST field of
 * yah264_param_t, yah264_param_default() fills it with sizeof(yah264_param_t),
 * and yah264_encoder_open() refuses a struct whose size is not the library's.
 * Source-compatible for every caller that goes through param_default, which is
 * every caller there is; the layout moved, so all of them must recompile.
 *
 * WHY THE FIELD EXISTS. A caller compiled against an older header hands the
 * library a struct in the older layout, and until ABI 3 the library read it as
 * the current one: every field after the first added one is some other field's
 * bytes. It opened, it encoded, and the only symptom was nonsense. The
 * in-process board on 2026-09-19 solved park_joy to CRF 42.7 at 55 Mbps
 * through an ffmpeg wrapper built one field behind the library, and timed the
 * encode at 32x slower than x264. Nothing in the loader catches this: the
 * soname matched and every symbol resolved. The refusal at open is the
 * tripwire that case had none of. */
#define YAH264_ABI_VERSION         3

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

/* partitions: the macroblock partition shapes the mode decision may try, as a
 * bitmask. These are OUR values, not another encoder's: a mask ported from
 * elsewhere selects the wrong shapes, so the CLI takes the names and a caller
 * ORs these constants. AUTO is negative because 0 is a real mask -- the empty
 * set, which codes every macroblock whole -- and so cannot double as unset. */
#define YAH264_PART_P8X8           0x01   /* P 16x8 / 8x16 / 8x8 split */
#define YAH264_PART_P4X4           0x02   /* P 8x4 / 4x8 / 4x4 sub-split (needs P8X8) */
#define YAH264_PART_B8X8           0x04   /* B 16x8 / 8x16 / 8x8 split */
#define YAH264_PART_I8X8           0x08   /* 8x8 intra prediction (needs transform8x8) */
#define YAH264_PART_I4X4           0x10   /* 4x4 intra prediction */
#define YAH264_PART_ALL            0x1f
#define YAH264_PART_NONE           0x00
#define YAH264_PART_AUTO           (-1)

/* Encoder parameters. Zero-initialise, then yah264_param_default. */
typedef struct {
    int size;               /* sizeof(yah264_param_t), written by
 * yah264_param_default(). Do not set it by hand and do not
 * copy it between builds: yah264_encoder_open() refuses a
 * value that is not this library's own sizeof, which is how
 * a caller built against a different include/yah264.h is
 * caught before it encodes rather than after (ABI 3). It
 * is first in the struct so the check reads a known offset
 * whatever else moves. */
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
    int open_gop;           /* 1 = the periodic keyframe is a non-IDR I picture
 * carrying a recovery_point SEI: the DPB is not
 * flushed, POC and FrameNum continue, and the B
 * frames that precede the key in output order keep
 * referencing the anchor before it. Only the first
 * picture of an encode, and a keyframe a zone forced,
 * stay IDR -- an ADAPTIVE SCENE-CUT key becomes an
 * open one like any other, which at the default keyint
 * is the only key most short encodes have. A stream coded this way is one sequence
 * with one POC line from end to end, which is why the
 * CLI stops splitting it into per-GOP encoders.
 * Refused with `stitchable`, whose whole promise is a
 * cut point at every keyframe. */
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
    int partitions;         /* which partition shapes the mode decision may try:
 * an OR of YAH264_PART_*, or YAH264_PART_AUTO (the
 * default) to derive the set from the rest of the
 * configuration. Resolve it with
 * yah264_partitions_resolved(). */
    /* Three low-rate decision gates. At low bitrate most of a frame is a skip
 * and most of the tournament is spent confirming it, so each of these ends
 * a search earlier when the evidence already in hand settles the verdict.
 * All three are speed-for-bits trades and all three default off; the
 * presets turn on whichever of them the corpus says is free. */
    int b_preme_skip;       /* B skip verdict before the motion search:
 * 0 = off, 1 = non-reference B slices, 2 = also
 * reference B's the propagation guard admits */
    int b_preme_bits;       /* the RD floor that gate reads, in bits (0 =
 * default) */
    int p_part_gate;        /* refuse the P sub-partition searches when the
 * 16x16 result is already this cheap in lambda
 * units and the neighbourhood is homogeneous
 * (0 = off) */
    int rd_surv_rank;       /* RD at most this many SATD survivors per B
 * candidate set, by rank (0 = off, RD every
 * candidate inside the score threshold) */
    int lr_settle;          /* end the lowres block search at its predictor
 * when that leaves under this much SAD per lowres
 * pixel (0 = off) */
    int lr_subgate;         /* skip the lowres subpel refine when the
 * whole-pel winner is under this much SATD per
 * lowres pixel (0 = off) */
    int mbt_depfloor;       /* refuse the mb-tree deposit where the block's
 * propagation fraction is under this many 256ths
 * (0 = off) */
    int cqm;              /* quant matrices: 0 = flat, 1 = JVT default (High) */
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
        /* The same composability question asked of the HRD clock. A coded
 * picture's cpb_removal_delay counts clock ticks since the start of the
 * buffering period in force, and a GOP-parallel caller's segments each
 * open one at their own first picture -- so the FIRST picture of a
 * segment has to count back into the segment before it, which is the one
 * number this encoder instance cannot know. The caller hands it over:
 * ticks from the previous segment's first picture to this one's, two per
 * input frame. 0 = this segment starts the stream. Every later picture
 * counts from this segment's own buffering period and needs nothing. */
        int hrd_bp_ticks;
        /* 1 = this encode is ONE OF SEVERAL segments concatenated into the
 * output. It suppresses the buffering period this encoder would otherwise
 * open at each of its own mid-stream IDRs, and the reason is arithmetic
 * rather than taste: hrd_bp_ticks above is measured from the previous
 * SEGMENT's first picture, so it is only the right answer when no other
 * buffering period was opened in between. The encoder places IDRs at scene
 * cuts that the caller splitting the work never sees, and one of those
 * inside the previous segment silently re-anchors the clock the next
 * segment is counting back to -- measured on a 300-frame clip whose last
 * cut landed at frame 139, which moved the removal time at the join by
 * 2.8 seconds and overflowed the buffer to 385% of its size.
 *
 * So in a segmented encode the buffering periods are exactly the segment
 * starts, one per GOP, and they are exact. The cost is that a mid-segment
 * IDR is still a random access point for a decoder but not one the HRD can
 * be initialised at. A single-segment encode leaves this 0 and gets a
 * buffering period at every IDR. */
        int hrd_segmented;
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
        /* Coded-QP bounds and the per-frame step limit the rate control works
 * inside. All three are zero-as-unset: 0 resolves to 0, 51 and 4, which
 * are the bounds and the step every mode already used. qp_min is the
 * one whose unset value and off value coincide. */
        int qp_min;         /* lowest coded QP the rate control may pick (0 = 0) */
        int qp_max;         /* highest (0 = 51) */
        int qp_step;        /* largest QP move between consecutive frames of one
 * type, in QP (0 = 4) */
        /* Initial VBV buffer occupancy. <= 1 is a fraction of vbv_bufsize,
 * above 1 is kbit. 0 = unset = start full, which is what a stream that
 * starts the output does. A segment that follows another in the output
 * spells its own occupancy with vbv_seg_join, not with this. */
        double vbv_init;
    } rc;

    int annexb;             /* 1 = emit Annex-B start codes (the only mode) */

    /* --- appended 2026-09-16 (A-plumb). Every one of these is a value the
     * encoder used to spell as a literal. Each default reproduces that
     * literal exactly, so a param struct filled by yah264_param_default and
     * left alone encodes what it always did.
     *
     * The three booleans are ON at their default and are written by
     * yah264_param_default, on the `sei` precedent: a caller that memsets and
     * skips param_default gets them off, which is why the struct's contract
     * has always been "zero-initialise, THEN yah264_param_default". */
    int deblock;            /* 1 = in-loop deblocking filter (the default) */
    int deblock_alpha;      /* slice_alpha_c0_offset_div2, -6..6 (0 = neutral) */
    int deblock_beta;       /* slice_beta_offset_div2, -6..6 (0 = neutral) */
    int b_pyramid;          /* 1 = hierarchical B when bframes >= 2 (the default),
 * 0 = a flat B run. There is no `strict` here yet. */
    int weightb;            /* 1 = implicit weighted biprediction on B slices
 * (the default), 0 = weighted_bipred_idc 0 */
    int constrained_intra;  /* 1 = PPS constrained_intra_pred_flag: intra
 * prediction in a P or B slice reads neither the
 * samples nor the intra mode of an inter-coded
 * neighbour, so an intra macroblock survives the
 * loss of the inter data around it. 0 = the
 * default, where every decoded neighbour is a
 * reference. Costs bits; buys error resilience. */
    int chroma_qp_index_offset;  /* PPS chroma_qp_index_offset, -12..12. Reaches
 * the quantiser AND the deblock chroma edge QP,
 * as the spec requires. 0 = the default. */
    int mvrange;            /* vertical motion-vector range in LUMA SAMPLES.
 * 0 = the level's own Table A-1 bound, which is
 * what the encoder always used. A value tighter
 * than the level's narrows the search; a wider one
 * is refused, because the level is a conformance
 * bound and not a suggestion. */
    int sps_id;             /* seq_parameter_set_id written in the SPS and named
 * by the PPS. 0..31; default 0. */
    int slices;             /* coded slices per picture, cut on macroblock-row
 * boundaries. 0 and 1 both mean one slice, which
 * is the default and what every picture was before.
 * Each slice is independently decodable -- no
 * prediction, no entropy state and no mb_qp_delta
 * chain crosses its first row -- so a decoder can
 * resynchronise at any of them, and each becomes
 * its own NAL unit. The in-loop filter is NOT cut:
 * every slice header says
 * disable_deblocking_filter_idc 0, so the picture
 * is deblocked whole and slice edges do not show.
 * More slices cost bits. Clamped to the picture's
 * macroblock row count. Slices are not a threading
 * vehicle here (the wavefront is), so this buys
 * loss resilience and decoder parallelism, not
 * encoder speed. */
    /* Forced profile_idc: 66 Baseline, 77 Main, 100 High, 110 High 10,
 * 122 High 4:2:2, 244 High 4:4:4. 0 = derive it from the tools and the
 * content, which is what the encoder has always done.
 *
 * It is a CONSTRAINT, not a label. encoder_open refuses a profile the
 * content or the tool-set cannot fit -- High for 4:4:4 input, anything
 * below High 10 for a 10-bit build, Baseline with CABAC or B frames --
 * rather than writing a profile_idc the stream does not obey. Narrowing
 * the tools to fit is the CALLER's job, and the CLI does it for the
 * tools the preset chose while refusing the ones you asked for by name. */
    int profile_idc;

    /* --- stream-level signalling (A-plumb). None of it moves a sample; all of
     * it changes the bytes, so each one is off by default. --- */
    int aud;                /* 1 = an access unit delimiter (NAL type 9) opens
 * every access unit */
    int pic_struct;         /* 1 = the VUI sets pic_struct_present_flag and
 * every picture carries a pic_timing SEI. The
 * value written is 0, a progressive frame, until
 * field coding exists to write anything else. */
    int frame_packing;      /* frame_packing_arrangement_type (Table D-8):
 * 0 checkerboard, 1 column, 2 row, 3 side-by-side,
 * 4 top-bottom, 5 frame alternation, 6 2D,
 * 7 tile. -1 = off, and param_default writes that,
 * because 0 is a real arrangement. */
    int cll_max, cll_avg;   /* content light level, cd/m^2. Both 0 = no SEI. */
    int mastering_set;      /* 1 = the mastering values below are meaningful */
    unsigned mastering_prim[6];  /* G.x G.y B.x B.y R.x R.y, 0.00002 units --
 * the SPEC's order, not the R,G,B a person
 * writes; the caller reorders */
    unsigned mastering_wp[2];    /* white point x, y, same units */
    unsigned mastering_max;      /* max display luminance, 0.0001 cd/m^2 */
    unsigned mastering_min;      /* min display luminance, same units */
    int alternative_transfer;    /* H.273 transfer code a display should prefer
 * over the VUI's. 0 = no SEI (0 is "reserved"
 * in H.273, so it is free to mean off). */
    int overscan;           /* 0 = not signalled, 1 = overscan_appropriate 0
 * (show the whole picture), 2 = 1 (crop is fine) */
    int video_format;       /* VUI video_format, 0..5. -1 and 0 differ: 0 is
 * "component" and 5 is "unspecified", so
 * param_default writes -1 for "leave it at 5". */
    int stitchable;         /* 1 = size the DPB from the LEVEL rather than from
 * this encode's ref/bframes, so two streams made
 * with different settings at the same geometry
 * carry the same SPS and concatenate. Costs
 * nothing in bits beyond the SPS itself; a bigger
 * declared DPB does not make the encoder keep
 * more pictures. */
    /* PAFF field coding: 0 = frame pictures (the default), 1 = top field first,
 * 2 = bottom field first. Each input frame is coded as two field pictures
 * of half the height, in the named order. An I frame becomes a pair of I
 * fields; a P frame a pair of P fields, each referencing the SAME-PARITY
 * field of the frames before it. Refused together with B frames, with the
 * hardware backend, and with 4:2:2 / 4:4:4 input (PAFF-1). MBAFF is not
 * implemented and will not be: docs/what-we-dont-do.md. */
    int interlaced;
    int fake_interlaced;    /* 1 = declare the sequence as one that MAY contain
 * field pictures (frame_mbs_only_flag 0) while
 * coding nothing but frame pictures. Every sample
 * is coded exactly as it would be without it; the
 * SPS geometry and one bit per slice header
 * change. For a downstream tool that refuses a
 * progressive-only sequence. */

    /* --- HRD signalling (B-hrd). The buffer model the rate control already
     * obeys, written into the stream so a receiver can verify it instead of
     * taking it on trust. Off by default: it costs an SEI per picture. --- */
    int nal_hrd;            /* YAH264_NAL_HRD_*. Both rc.vbv_maxrate and
 * rc.vbv_bufsize are required; encoder_open
 * refuses the setting without them, because there
 * is no buffer to declare. VBR declares cbr_flag
 * 0 and changes headers only, so the pictures are
 * the same samples the same VBV encode already
 * produced. CBR declares cbr_flag 1, which says
 * the channel runs at the declared rate for the
 * whole stream, so the access units are padded up
 * to it (see `filler`). */
    int filler;             /* Pad each access unit with a filler NAL (type 12)
 * so the constant delivery schedule is true.
 * ZERO IS UNSET, not off: it follows nal_hrd, so
 * CBR pads and everything else does not. Off is
 * YAH264_FILLER_OFF, by the same convention
 * scenecut and sync_lookahead use and for the
 * same reason -- a caller who memset the struct
 * and set nal_hrd by hand would otherwise get CBR
 * headers over an unpadded stream, which is a
 * non-conforming stream and no diagnostic. 1
 * forces it on, and is refused outside CBR:
 * under VBR the declared schedule already lets
 * the channel pause, so padding would buy nothing
 * and cost the bits. */

    /* --- rate-control bounds (B-rcbounds). Rate-control values, but appended
     * here rather than inside `rc` for the reason every other appended block
     * is: growing a nested struct moves every field after it. --- */
    /* Ceiling on the QP the VBV may raise a frame to in CRF mode. 0 = unset,
 * which is the ceiling the coded-QP bound already gives (rc.qp_max, or 51).
 * Needs both rc.method _CRF and a VBV; encoder_open refuses it otherwise,
 * because with no VBV nothing raises the QP for it to bound.
 *
 * It bounds the RAISE, never the mapping: a frame the rate factor already
 * puts above this keeps the QP it was given. What it buys is a quality
 * floor, and what it costs is the buffer model -- a frame that would have
 * been starved down to fit is coded at this QP instead and the buffer is
 * allowed to under-run. That is the whole point of the option and it is not
 * a defect; scripts/vbv_check.py reports the under-run rather than hiding
 * it. Do not set it on a stream that has to be HRD-conformant. */
    double crf_max;
    /* How far single-pass ABR may drift from its target before the correction
 * term answers, as a fraction of the target bitrate. 0 = unset = 1.0, which
 * is the tolerance every ABR encode has used and is byte-identical to it.
 * Smaller tightens the loop: the correction reacts to a smaller error, so
 * the rate tracks closer and the quality swings more. HUGE_VAL (the CLI's
 * `--ratetol inf`) disables the correction entirely and leaves the rate to
 * the allocator alone. Y264_ABR_TOL still overrides it. */
    double ratetol;
} yah264_param_t;

/* The partition mask the encoder will actually run, with AUTO resolved.
 *
 * AUTO is the set this encoder searched before `partitions` existed, spelled
 * out: every preset splits P macroblocks and B macroblocks and tries 4x4
 * intra; the sub-8x8 P shapes are an effort tier that arrives with the full-RD
 * analysis path at subme 8; and 8x8 intra needs the 8x8 transform to code it,
 * so a preset or a profile that turns the transform off drops the shape with
 * it. Deriving the default rather than tabulating it per preset is what keeps
 * `--subme 9` on a fast preset searching exactly what it searched before.
 *
 * Inline so the CLI can print the resolved set without a second copy of the
 * rule, and without a new exported symbol on both depth libraries. */
static inline int yah264_partitions_resolved(const yah264_param_t *param)
{
    int subme;
    int mask;
    if (!param)
        return YAH264_PART_NONE;
    if (param->partitions != YAH264_PART_AUTO)
        return param->partitions;
    subme = param->subme > 0 ? param->subme : 10;
    mask = YAH264_PART_P8X8 | YAH264_PART_B8X8 | YAH264_PART_I4X4;
    if (subme >= 8)
        mask |= YAH264_PART_P4X4;
    if (param->transform8x8)
        mask |= YAH264_PART_I8X8;
    return mask;
}

/* param.nal_hrd: which hypothetical reference decoder model the SPS declares. */
#define YAH264_NAL_HRD_NONE 0
#define YAH264_NAL_HRD_VBR  1
#define YAH264_NAL_HRD_CBR  2
/* param.filler: 0 follows nal_hrd, 1 forces padding on, this forces it off. */
#define YAH264_FILLER_OFF          (-1)

typedef struct yah264_encoder yah264_encoder_t;

/* Library version string, e.g. "0.0.0". */
/* Everything this header declares is exported; everything else in the library
 * is not. The shared build compiles with hidden visibility so the dylib's own
 * internal calls stay direct rather than routing through the export table on
 * every kernel dispatch, which means the public entry points have to say so
 * explicitly or the shared object ships with nothing in it. */
#if defined(__GNUC__) || defined(__clang__)
#  define YAH264_EXPORT __attribute__((visibility("default")))
#else
/* MSVC would want dllexport when building and dllimport when consuming, which
 * needs a build-time define this project does not yet set. Windows is not a
 * tested target, so rather than ship a half-right guess, export nothing
 * special and leave it for whoever ports it. */
#  define YAH264_EXPORT
#endif

YAH264_EXPORT const char *yah264_version(void);

/* Bit depth this library was BUILT for, which a caller cannot infer from the
 * header alone: Y264_BIT_DEPTH defaults to 8 above when nothing defines it, and
 * the 8-bit and 10-bit builds install under the same soname, so a 10-bit dylib
 * loads happily under a header that has typed `pixel` as uint8_t and every
 * plane is then read at half its stride. pkg-config's Cflags carry the right
 * -DY264_BIT_DEPTH, so the compile-time path is covered; this is for the caller
 * that linked by hand or had the dylib swapped underneath it. Compare it
 * against Y264_BIT_DEPTH at open and refuse the mismatch. */
YAH264_EXPORT int yah264_bit_depth(void);

/* Space-separated list of CPU features the encoder auto-detected and will use
 * for kernel dispatch on this machine (e.g. "neon dotprod i8mm"), or "scalar".
 * The returned string is owned by the library. */
YAH264_EXPORT const char *yah264_cpu_features(void);

/* Fill param with defaults. Safe to call on a zeroed struct.
 *
 * It also writes `size`, and it is the only supported way to get that field
 * right. Every caller starts here, so a struct that reaches open without it
 * is a struct from some other build of this header. */
YAH264_EXPORT void yah264_param_default(yah264_param_t *param);

/* 2-pass: the weight one pass-1 stats record contributes to the pass-2 bit
 * allocation (the QP-invariant coding cost, complexity-compressed). Exposed so
 * a caller that splits a stats file along GOP boundaries can size each GOP's
 * share of the budget with the encoder's own formula instead of a copy of it. */
double yah264_2pass_stat_weight(double bits, int qp);

/* Apply a named speed preset ("ultrafast".."placebo"). Returns 0 on success,
 * -1 on an unknown name. */
YAH264_EXPORT int yah264_param_apply_preset(yah264_param_t *param, const char *preset);

/* Open an encoder for the given parameters. Returns NULL on error.
 *
 * `param->size` must equal the library's own sizeof(yah264_param_t) or the
 * open is refused with a line on stderr naming both numbers. That is a caller
 * built against a different include/yah264.h, and the encode it would have
 * produced is not worth having. */
YAH264_EXPORT yah264_encoder_t *yah264_encoder_open(const yah264_param_t *param);

/* The hardware mode (docs/videotoolbox-plan.md): the same handle and calls,
 * backed by Apple's fixed-function H.264 encoder through VideoToolbox. The
 * output is the hardware's stream, not this encoder's, so reconstruction and
 * the rate-control state calls return -1 on it. YAH264_HW_AUTO falls back to
 * the software encoder with one warning line on stderr when the hardware
 * refuses the session (no hardware, another process holding it, a format it
 * does not take); YAH264_HW_VIDEOTOOLBOX returns NULL instead. Passed at open
 * rather than carried in yah264_param_t so the parameter struct's layout is
 * unchanged. yah264_encoder_open is open_hw with YAH264_HW_OFF (the Y264_HW
 * environment knob, off | auto | videotoolbox, overrides it for the harnesses). */
#define YAH264_HW_OFF          0
#define YAH264_HW_AUTO         1
#define YAH264_HW_VIDEOTOOLBOX 2
YAH264_EXPORT yah264_encoder_t *yah264_encoder_open_hw(const yah264_param_t *param, int hw);
/* The name of the encoder behind a handle: "yah264" or the hardware's. */
YAH264_EXPORT const char *yah264_encoder_backend(const yah264_encoder_t *enc);

/* Retrieve the sequence headers (SPS, PPS). On return *nal points at an array of
 * *count NAL units owned by the encoder. Returns 0 on success. */
/* Video signal description for the VUI (colour primaries, transfer
 * characteristics, matrix coefficients: the H.273 codes, 2 = unspecified;
 * full_range 0/1; chroma_loc 0..5, -1 = not signalled). Set once after open
 * and before the headers or the first encode; the parameter struct is not
 * grown for it (ABI). Unset = nothing signalled, as before. */
typedef struct {
    int full_range;
    int primaries, transfer, matrix;
    int chroma_loc;
} yah264_video_signal_t;
YAH264_EXPORT int yah264_encoder_set_video_signal(yah264_encoder_t *enc, const yah264_video_signal_t *vs);

YAH264_EXPORT int yah264_encoder_headers(yah264_encoder_t *enc,
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
YAH264_EXPORT int yah264_encoder_encode(yah264_encoder_t *enc,
                           yah264_nal_t **nal, int *count,
                           const yah264_picture_t *pic);

/* Point *pic at the encoder's reconstruction of the most recently encoded
 * picture (cropped to the coded width/height). The planes are owned by the
 * encoder and valid until the next encode call. Returns 0 on success, -1 if no
 * frame has been encoded yet. Used to verify that the encoder's internal
 * reconstruction matches an independent decoder's output. */
YAH264_EXPORT int yah264_encoder_get_recon(yah264_encoder_t *enc, yah264_picture_t *pic);

/* Register a callback invoked once per emitted frame, in coding order, with the
 * frame's reconstruction (cropped) and its display index (input order). This is
 * the only way to capture every frame's recon when B-frames reorder coding vs
 * display order: a single encode call can emit an anchor plus several B's.
 * The picture planes are valid only for the duration of the callback. Pass
 * cb = NULL to clear.
 *
 * `depth` is the sample width of rec->plane, 8 or 10: the picture carries
 * void* planes now, and a caller that registers ONE callback across both
 * libraries (the CLI's recon dumper does) would otherwise have to remember
 * which encoder each call came from. */
YAH264_EXPORT void yah264_encoder_set_recon_cb(yah264_encoder_t *enc,
                                  void (*cb)(void *ud, const yah264_picture_t *rec,
                                             int disp_index, int depth),
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
YAH264_EXPORT int yah264_encoder_frame_order(yah264_encoder_t *enc, int *disp, int max);

/* Zones: per-frame-range overrides an orchestrator hands the encoder, the
 * engine side of a shot-based plan (docs/engine-interface.md). Frames are
 * indexed in input order from zero, [first, last] inclusive. YAH264_ZONE_IDR
 * forces an IDR at `first` (the lookahead treats it as a cut: an anchor, a new
 * GOP, keyint restarts); qp_offset is added to every frame's QP inside the
 * range, on top of whatever the rate control chose (CRF, CQP and ABR alike),
 * clamped to the QP range. Zones may not overlap. The array is copied; call
 * before the frames it names are pushed (the safe order is before the first
 * encode). Returns 0, or -1 on bad arguments. */
typedef struct {
    int    first, last;
    int    flags;               /* YAH264_ZONE_* */
    double qp_offset;
} yah264_zone_t;
#define YAH264_ZONE_IDR 1
YAH264_EXPORT int yah264_encoder_set_zones(yah264_encoder_t *enc, const yah264_zone_t *zones, int n);

/* Per-frame forces: the same plan, at its finest grain. A zone names a RANGE
 * and an offset; this names ONE frame and the decision itself. Frames are
 * indexed in input order from zero, one record per frame, and a frame no
 * record names is left entirely to the encoder.
 *
 * `type` replaces the lookahead's choice for that frame. `qp` replaces the
 * rate control's, and it is ABSOLUTE: 0..51 is the coded slice QP, not an
 * offset, and neither the frame-type cascade nor the VBV nor a zone's offset
 * moves it afterwards. -1 leaves the QP to the rate control, so a record can
 * force a type alone, a QP alone, or both.
 *
 * A force the encoder cannot honour is refused rather than approximated, and
 * the refusal names the frame: set_frame_forces rejects what it can see from
 * the array alone (a B at frame 0, a B with no B frames configured, an
 * out-of-range QP, a repeated frame index), and a B the lookahead cannot place
 * when it gets there -- the last frame of the stream, a frame the key-frame
 * interval has already claimed, one past the B-run limit -- fails the encode
 * call with the frame number on stderr.
 *
 * The array is copied and may be in any order. Call before the frames it names
 * are pushed. Returns 0, or -1 on bad arguments. */
typedef struct {
    int disp;               /* input frame index, from zero */
    int type;               /* YAH264_FORCE_* */
    int qp;                 /* absolute coded QP 0..51, or -1 for "rate control decides" */
} yah264_frame_force_t;
#define YAH264_FORCE_NONE 0     /* type is not forced (a QP-only record) */
#define YAH264_FORCE_IDR  1     /* an IDR: a new GOP, the key-frame interval restarts */
#define YAH264_FORCE_P    2     /* an anchor: a P frame (the first frame of a GOP is an IDR whatever this says) */
#define YAH264_FORCE_B    3     /* a leaf: a B frame. Whether it is itself a reference stays
 * the B-pyramid's choice, which is why there is no separate
 * non-reference seat here. */
YAH264_EXPORT int yah264_encoder_set_frame_forces(yah264_encoder_t *enc,
                                                  const yah264_frame_force_t *forces, int n);

/* Per-frame coding decisions, in CODING order, drained like frame_order: one
 * record per coded frame with its input index, slice type (0 I, 1 P, 2 B),
 * whether it is an IDR and a reference, and the slice QP the rate control
 * chose (per-macroblock offsets sit on top of it). Bytes are not here: the
 * caller has the NALs, and frame_order pairs them with these records. */
typedef struct {
    int disp;
    int type;
    int is_idr, is_ref;
    int qp;
} yah264_frame_stats_t;
YAH264_EXPORT int yah264_encoder_frame_stats(yah264_encoder_t *enc, yah264_frame_stats_t *out, int max);

YAH264_EXPORT int yah264_frame_thread_cap(int width, int height);

/* What param.threads = 0 resolves to on this machine: every online core, cached.
 * Exported so a caller splitting work across several encoder instances sizes its
 * budget from the same number the library would have used, instead of asking the
 * OS separately and drifting. Clamp per instance with yah264_frame_thread_cap:
 * this is how much machine exists, not how much one picture can use. */
YAH264_EXPORT int yah264_threads_auto(void);

/* Frames of input latency these parameters add through the decoupled
 * lookahead's lead -- i.e. param.sync_lookahead resolved (auto, explicit, or
 * off) and clamped the way encoder_open clamps it. encode returns no NAL for
 * that many calls beyond the B-frame reorder delay, which this does not include
 * and does not change. The lead never changes a bit, so this number is the
 * whole cost of it and a latency-sensitive caller should be shown it. */
YAH264_EXPORT int yah264_lookahead_delay(const yah264_param_t *param);

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
/* A shot, from the pre-scan: frames [first, last] between two scene cuts,
 * with the lowres intra cost (mean, peak) and the zero-motion inter cost
 * (mean, from the second frame) over it, and their ratio (low = static,
 * high = motion). Costs are in the lookahead's own lowres SATD units; compare
 * them across shots of one input, not across inputs. */
typedef struct {
    int    first, last;
    double icost_mean, icost_peak;
    double pcost_mean;
    double ratio;
} yah264_shot_t;
/* Pre-scan the input and return its shot table (plus the IDR map in `idr`
 * when non-NULL, as yah264_scan_idr_frames). A keyint IDR does not split a
 * shot. Returns the number of shots written, capped at max_shots, or -1. */
/* `depth` is the sample width of the planes in `luma`, 8 or 10, and it must
 * equal this library's own: the planes are void* and a mismatch would be read
 * silently at half or twice its stride, which is a scan that returns a plausible
 * IDR map for the wrong pixels. Mismatch returns -1 and scans nothing. */
YAH264_EXPORT int yah264_scan_shots(const yah264_param_t *param,
                                 const void *const *luma, const int *stride,
                                 int n, int nthreads, int depth,
                                 unsigned char *idr,
                                 yah264_shot_t *shots, int max_shots);

YAH264_EXPORT int yah264_scan_idr_frames(const yah264_param_t *param,
                            const void *const *luma, const int *stride,
                            int n, int nthreads, int depth,
                            unsigned char *idr);

/* Close the encoder and free all resources. */
YAH264_EXPORT void yah264_encoder_close(yah264_encoder_t *enc);

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
    double   rf2_kc[3];                        /* the in-flight predictor's per-type */
    int      rf2_kc_cal[3];                    /* calibration (appended 2026-09-04) */
} yah264_rc_state_t;

/* Export the ABR state after the flush (all frames accounted). Returns 0 and
 * out->valid = 1 for an ABR instance; -1 (out->valid = 0) otherwise. */
YAH264_EXPORT int yah264_encoder_rc_state(const yah264_encoder_t *enc, yah264_rc_state_t *out);

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
YAH264_EXPORT int yah264_encoder_rc_import(yah264_encoder_t *enc, const yah264_rc_state_t *state,
                                        int frames_ahead);

#ifdef __cplusplus
}
#endif

#endif /* YAH264_H */
