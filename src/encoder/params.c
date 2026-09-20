/*
 * params.c - parameter defaults and presets
 * Copyright (c) 2026, the yah264 authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "yah264.h"
#include "../common/cpu.h"
#include <string.h>

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

/* The public enums carry the reference encoder's values so a ported constant
 * selects the tool it names. Nothing in a C compiler notices when someone
 * "tidies" one of these back into a dense 0,1,2, and the encode would keep
 * succeeding -- with the wrong tool, which is the entire bug this numbering
 * exists to prevent. Pin them here against the ABI contract spelled out in
 * include/yah264.h, checked at every build. */
_Static_assert(YAH264_RC_CQP == 0 && YAH264_RC_CRF == 1 && YAH264_RC_ABR == 2,
               "rc.method must keep the reference CQP/CRF/ABR values");
_Static_assert(YAH264_ME_DIA == 0 && YAH264_ME_HEX == 1 && YAH264_ME_UMH == 2,
               "me_method must keep the reference DIA/HEX/UMH values");
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
 * refused (deep median +0.24/+0.44). Y264_PSY_RD
 * overrides. */
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
        {"faster",    4,  2,  2, 20, 1, 1, 3, 0},
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
