/*
 * params.c - parameter defaults and presets
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "yah264.h"
#include "../common/cpu.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* The two off-switches are negative, and the encoder tests them as `< 0` rather
 * than against these values. Anyone who "fixes" the constants to 0 turns both
 * off-switches into their defaults, silently, everywhere. Fail the build
 * instead. */
_Static_assert(YAH264_SCENECUT_OFF < 0,
               "scenecut off must stay negative; 0 is the default (40)");
_Static_assert(YAH264_SYNC_LOOKAHEAD_OFF < 0,
               "sync_lookahead off must stay negative; 0 is auto");
/* Same shape, third case: a zero `partitions` is the empty set (every
 * macroblock coded whole), so auto cannot be zero. "Tidying" AUTO to 0 turns
 * every default encode into a 16x16-only encode that still succeeds. */
_Static_assert(YAH264_PART_AUTO < 0,
               "partitions auto must stay negative; 0 is the empty set");
_Static_assert(YAH264_PART_ALL == (YAH264_PART_P8X8 | YAH264_PART_P4X4 |
                                   YAH264_PART_B8X8 | YAH264_PART_I8X8 |
                                   YAH264_PART_I4X4),
               "PART_ALL must name every shape; a new shape belongs in it");

/* The public enums carry the established H.264 encoder API's public values
 * (include/yah264.h) so a constant written against that API selects the tool
 * it names. Nothing in a C compiler notices when someone
 * "tidies" one of these back into a dense 0,1,2, and the encode would keep
 * succeeding -- with the wrong tool, which is the entire bug this numbering
 * exists to prevent. Pin them here against the ABI contract spelled out in
 * include/yah264.h, checked at every build. */
_Static_assert(YAH264_RC_CQP == 0 && YAH264_RC_CRF == 1 && YAH264_RC_ABR == 2,
               "rc.method must keep the reference CQP/CRF/ABR values");
_Static_assert(YAH264_ME_DIA == 0 && YAH264_ME_HEX == 1 && YAH264_ME_UMH == 2 &&
               YAH264_ME_ESA == 3 && YAH264_ME_TESA == 4,
               "me_method must keep the reference DIA/HEX/UMH/ESA/TESA values");
_Static_assert(YAH264_DIRECT_SPATIAL == 1 && YAH264_DIRECT_TEMPORAL == 2 && YAH264_DIRECT_AUTO == 3,
               "direct must keep the reference SPATIAL/TEMPORAL values");
_Static_assert(YAH264_CSP_I420 == 2 && YAH264_CSP_I422 == 6 && YAH264_CSP_I444 == 12,
               "csp must keep the reference I420/I422/I444 values");

/* The two values the reference encoder has no seat for. 2PASS must stay clear
 * of the reference rc-method range (0..2 today) so a fourth method there cannot
 * land on it, and ME AUTO must stay negative for the same reason -- plus the
 * CLI and encoder both treat a negative me_method as "no explicit method". */
_Static_assert(YAH264_RC_2PASS > 16,
               "2-pass must stay far above the reference rc-method range");
_Static_assert(YAH264_ME_AUTO < 0,
               "ME auto must stay negative; every non-negative value is a reference method");
/* And the CLI's `--direct` / `--me` overrides are applied under a `>= 0` test,
 * so a negative spatial or dia would silently drop the flag. */
_Static_assert(YAH264_DIRECT_SPATIAL >= 0 && YAH264_DIRECT_TEMPORAL >= 0,
               "direct modes must stay non-negative; the CLI gates on >= 0");

/* Built from the header's macros rather than spelled again. There were three
 * independent copies of this string (here, the header, meson.build) and a
 * version bump that moves only some of them is a version bump that lies. */
#define Y264_STR_(x) #x
#define Y264_STR(x)  Y264_STR_(x)
const char *yah264_version(void)
{
    return Y264_STR(YAH264_VERSION_MAJOR) "." Y264_STR(YAH264_VERSION_MINOR)
           "." Y264_STR(YAH264_VERSION_PATCH);
}

int yah264_bit_depth(void)
{
    return Y264_BIT_DEPTH;
}

const char *yah264_cpu_features(void)
{
    static char buf[128];
    static int done;
    if (!done) {
        y264_cpu_name(y264_cpu_detect(), buf, sizeof(buf));
        done = 1;
    }
    return buf;
}

void yah264_param_default(yah264_param_t *param)
{
    if (!param)
        return;
    memset(param, 0, sizeof(*param));
    /* First field, and the only place it is ever written. encoder_open reads
 * it to catch a caller compiled against a different header (ABI 3). */
    param->size = (int)sizeof(*param);
    param->csp = YAH264_CSP_I420;
    param->timebase.fps_num = 25;
    param->timebase.fps_den = 1;
    param->threads = 0;
    param->keyint = 250;
    param->ref = 1;
    param->rc.qp = 26;
    param->rc.rf = 23.0;        /* CRF target, used when rc.method is _CRF */
    param->rc.lookahead = 40;   /* mb-tree propagation window */
    param->badapt = 1;          /* adaptive B placement (b-adapt) */
    param->psy_rd = 2.0f;       /* psychovisual RD. Swept against the CRF band:
 * knee basin 2.0-3.0, 2.0 the most uniform -- corpus
 * median -0.17% VMAF-NEG, 7/12 negative, worst +0.74;
 * deep band a wash. The QP-ramp form is measured and
 * refused (deep median +0.24/+0.44). */
    param->subme = 10;          /* default: full RD per partition (max quality) */
    param->trellis = 1;         /* RDOQ on the committed MB only */
    param->subpel = -1;         /* -1 = auto (square-to-convergence, max quality) */
    /* Both of these have to be written even though the struct was just zeroed:
 * under the ABI numbering 0 is the "no direct prediction" value (which we refuse) and
 * YAH264_ME_DIA (a real method, not auto), so memset alone spells neither
 * default. */
    param->direct = YAH264_DIRECT_AUTO;    /* per-slice choice; see the header */
    param->me_method = YAH264_ME_AUTO;   /* follow the subme ME gate (--me overrides) */
    param->sei = 1;             /* emit a settings SEI by default */
    /* Variance AQ, on by default because every shipped non-CQP encode runs it.
     * It used to be left at zero here and resolved by the CLI, which meant a
     * library caller got AQ off and no indication of it: the ffmpeg wrapper
     * emitted 32% more bits than the CLI at the same CRF before this moved.
     * encoder_open still forces 0 at CQP, so this is the non-CQP value. */
    param->aq_strength = 0.4f;
    param->annexb = 1;
    /* The three that the encoder spelled as literals until 2026-09-16. Written
 * explicitly for the same reason `sei` and `annexb` are: their default is ON
 * and a zeroed struct would spell the opposite. */
    param->deblock = 1;
    /* Deblocking and chroma QP offsets: resolved from the picture size at
 * open (see deblock_auto_resolve in encoder.c). */
    param->deblock_alpha = YAH264_DEBLOCK_AUTO;
    param->deblock_beta = YAH264_DEBLOCK_AUTO;
    param->chroma_qp_index_offset = YAH264_CQP_AUTO;
    param->b_pyramid = 1;
    param->weightb = 1;
    /* Explicit P weighted prediction, at the level every encode before the flag
 * ran at. 0 reads the same (unset), so a memset spells it correctly; off is
 * YAH264_WEIGHTP_OFF. */
    param->weightp = 1;
    param->slices = 1;          /* one slice per picture (0 reads the same) */
    /* Three more that a memset cannot spell, for the same reason `direct` and
 * `me_method` are written above: 0 is a real value for all of them. A zeroed
 * `partitions` is the empty set, which codes every macroblock whole. */
    param->partitions = YAH264_PART_AUTO;
    /* The P sub-partition gate, in lambdas, and a fourth of the same kind: 0
 * is off, which is what a zeroed struct spells, and off is not what this
 * ships at. The gate holds its quality on every clip of the HD band and the
 * searches it deletes are the largest count in the P tournament. Inert at
 * subme >= 9, which runs the exhaustive tournament by contract. Its two
 * siblings, b_preme_skip and rd_surv_rank, ARE off at every preset, and a
 * memset spells both of those correctly. lr_settle is off HERE and the
 * preset ladder turns it on below medium, so this zero is the value a
 * caller who never applies a preset gets. */
    param->p_part_gate = 400;
    param->frame_packing = -1;   /* off; 0 is checkerboard packing */
    param->video_format = -1;    /* leave the VUI's 5 (unspecified); 0 is component */
}

int yah264_param_apply_preset(yah264_param_t *param, const char *preset)
{
    /* Preset ladder: a real schedule over (subme, subpel, ref, lookahead), mirroring
 * the conventional preset ladder. Each tier scales:
 * subme RD path: <=8 fast SATD-partition, >=9 full RD per partition
 * subpel subpel pattern: 2 = capped diamond (fast), -1 = square (medium+)
 * ref P-frame list-0 references (the reference encoder: 1..16 across the ladder)
 * lookahead mb-tree/rc lookahead depth (the reference encoder: 0..60)
 * ME METHOD: UMH is gated on subme>=8 (slow+), so MEDIUM (subme 7) runs
 * the parity HEX path -- rich MV seeds + the behaviour-matched lowres MV
 * field + terminal square refine -- matching the reference encoder's
 * medium=hex. Measured -0.48% VMAF-NEG against it. --me umh selects the wide-grid search at
 * any tier; --me hex/dia force hex regardless of subme.
 * bframes/b-adapt/trellis-level/direct-auto are not tiered.
 * -1 lookahead = "leave default"; explicit CLI --ref/--rc-lookahead override. */
    /* The preset OWNS the full tool-set so it is self-contained: cabac/tr8/bframes
 * are set here (not just search knobs), matching the reference encoder's
 * ultrafast tool-strip (CAVLC, no B frames, no 8x8 transform). superfast+ keep
 * the full tools (it strips only at ultrafast). An explicit CLI flag (--cabac/--cavlc/
 * --transform-8x8/--no-transform-8x8/--bframes) overrides the preset in the CLI. */
    /* The last column is the LOW-RATE ARM the ladder carries, and it is the only
 * one here that is a quality trade rather than an effort level. medium and
 * above run 0: medium is the preset the parity claim is read on, it keeps the
 * +0.2%-per-clip bar, and every arm of the stage-2 and stage-3 rounds failed
 * that bar. The presets below it are allowed +0.5% per clip, because a small
 * per-clip cost is what a fast preset IS. Of the five arms read against that
 * bar on the HD band, one holds it: the lowres settle exit at 1. It is inert
 * where the lookahead window is off (superfast, ultrafast) and the column
 * still names it there, so an arm that ships at one tier ships at every
 * faster one. local/records/lowrate-presets-2026-09-20.md. */
    static const struct {
        const char *name; int subme, subpel, ref, lookahead, cabac, tr8, bframes,
                           lr_settle;
    } P[] = {
        {"ultrafast", 1,  2,  1,  0, 0, 0, 0, 1},  /* stripped: CAVLC, no-8x8, no-B */
        {"superfast", 1,  2,  1,  0, 1, 1, 3, 1},
        {"veryfast",  2,  2,  1, 10, 1, 1, 3, 1},
        {"faster",    4,  2,  2, 20, 1, 1, 3, 1},
        {"fast",      6,  2,  2, 30, 1, 1, 3, 0},
        {"medium",    7,  2,  3, 40, 1, 1, 3, 0},
        {"slow",      8, -1,  5, 50, 1, 1, 3, 0},
        {"slower",    9, -1,  8, 60, 1, 1, 3, 0},
        {"veryslow", 10, -1, 16, 60, 1, 1, 3, 0},
        {"placebo",  11, -1, 16, 60, 1, 1, 3, 0},
        {NULL, 0, 0, 0, 0, 0, 0, 0, 0}
    };
    if (!param || !preset)
        return -1;
    for (int i = 0; P[i].name; i++)
        if (strcmp(P[i].name, preset) == 0) {
            param->subme = P[i].subme;
            param->subpel = P[i].subpel;
            param->ref = P[i].ref;
            param->rc.lookahead = P[i].lookahead;
            param->cabac = P[i].cabac;
            param->transform8x8 = P[i].tr8;
            param->bframes = P[i].bframes;
            param->lr_settle = P[i].lr_settle;
            return 0;
        }
    return -1;
}

/* A digits-only unsigned number at *s, up to `max`; -1 when there isn't one.
 * strtoul takes a sign and skips blanks, so neither reaches it. */
static long long hdr_number(const char **s, unsigned long long max)
{
    const char *p = *s;
    unsigned long long v = 0;
    if (!isdigit((unsigned char)*p))
        return -1;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (unsigned)(*p++ - '0');
        if (v > max)
            return -1;
    }
    *s = p;
    return (long long)v;
}

int yah264_param_parse_cll(yah264_param_t *p, const char *s, char *err, size_t errlen)
{
    const char *q = s;
    long long a = hdr_number(&q, 65535), b = -1;
    if (a >= 0 && *q == ',') { q++; b = hdr_number(&q, 65535); }
    if (a < 0 || b < 0 || *q) {
        if (err) snprintf(err, errlen, "expects MaxCLL,MaxFALL, each 0..65535 (got '%s')", s);
        return -1;
    }
    /* a frame's average light can't pass the brightest pixel; the usual
     * cause is the two values swapped. A MaxCLL of 0 means unknown, so it
     * leaves MaxFALL unchecked. */
    if (a > 0 && b > a) {
        if (err) snprintf(err, errlen, "MaxFALL %lld is above MaxCLL %lld; the order is "
                          "MaxCLL,MaxFALL", b, a);
        return -1;
    }
    p->cll_max = (int)a;
    p->cll_avg = (int)b;
    return 0;
}

int yah264_param_parse_mastering(yah264_param_t *p, const char *s, char *err, size_t errlen)
{
    static const char *const lead[10] = { "G(", ",", ")B(", ",", ")R(", ",", ")WP(", ",", ")L(", "," };
    long long g[10] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    const char *q = s;
    for (int k = 0; k < 10; k++) {
        size_t n = strlen(lead[k]);
        if (strncmp(q, lead[k], n)) break;
        q += n;
        g[k] = hdr_number(&q, k < 8 ? 50000ull : 0xFFFFFFFFull);
        if (g[k] < 0) break;
    }
    if (g[9] < 0 || strcmp(q, ")")) {
        if (err) snprintf(err, errlen, "expects G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min) -- "
                          "chromaticity 0..50000 in 0.00002 units, luminance in 0.0001 "
                          "cd/m^2 within 32 bits (got '%s')", s);
        return -1;
    }
    /* min below max binds only on a real peak: one under 5 cd/m^2 isn't (0
     * means unknown, as in L(0,0)), so the min isn't held below it */
    if (g[8] >= 50000 && g[9] >= g[8]) {
        if (err) snprintf(err, errlen, "L(max,min) needs min below max, in 0.0001 cd/m^2");
        return -1;
    }
    /* the SEI's semantics keep x in 5..37000, y in 5..42000 and the peak at
     * 10000 cd/m^2 or below; a player may ignore a value outside them, so
     * say so but write what was asked */
    int warn = 0;
    if (err && errlen) err[0] = 0;
    for (int k = 0; k < 8; k++)
        if (g[k] < 5 || g[k] > ((k & 1) ? 42000 : 37000)) {
            if (err) snprintf(err, errlen, "chromaticity %lld is outside the SEI's range "
                              "(x 5..37000, y 5..42000)", g[k]);
            warn = 1;
            break;
        }
    if (!warn && g[8] > 100000000) {
        if (err) snprintf(err, errlen, "peak is above 10000 cd/m^2");
        warn = 1;
    }
    if (!warn && g[9] > 50000) {
        if (err) snprintf(err, errlen, "min is above 5 cd/m^2; luminance is in 0.0001 cd/m^2 units");
        warn = 1;
    }
    /* a peak under 5 cd/m^2, other than 0 for unknown, is almost always nits
     * typed where 0.0001 units belong */
    if (!warn && g[8] > 0 && g[8] < 50000) {
        if (err) snprintf(err, errlen, "peak L(%lld,...) is %.4f cd/m^2; luminance is in "
                          "0.0001 cd/m^2 units", g[8], g[8] / 10000.0);
        warn = 1;
    }
    for (int k = 0; k < 6; k++) p->mastering_prim[k] = (unsigned)g[k];
    p->mastering_wp[0] = (unsigned)g[6];
    p->mastering_wp[1] = (unsigned)g[7];
    p->mastering_max = (unsigned)g[8];
    p->mastering_min = (unsigned)g[9];
    p->mastering_set = 1;
    return warn;
}
