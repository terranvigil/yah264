#!/usr/bin/env bash
# Copyright (c) 2026, the yah264 authors
# SPDX-License-Identifier: BSD-2-Clause
#
# parity-clips.sh -- the scoreboard's clip set and its per-clip operating
# points. SOURCED, not executed, by perf-comp-set.sh (ABR) and
# perf-comp-crf-set.sh (matched-point CRF).
#
# It lives in its own file for one reason: the two scoreboards must sit at the
# SAME operating points or their rows cannot be reasoned about together, and a
# table duplicated across two scripts drifts the first time someone recalibrates
# one of them. Sourcing it makes that impossible by construction.
#
# clip:bitrate(kbps) -- chosen PER CLIP from a measured rate/VMAF ladder
# (docs/rc-mode-matrix.md), not per resolution.
#
# 2026-08-11: these were CIF 2500 / 720p 12000, one rate per resolution. Both
# were wrong, and not in the way the resolution split suggests. Measured on this
# tree at 6s windows, medium preset, ABR:
#
#   - NOTHING SATURATES. The suspicion that the 720p rows sat at a QP floor does
#     not reproduce: ducks_720p at a 12000 target delivers 12447 kbit/s
#     (yah264, +3.7%) and 13005 (x264, +8.4%), and the same holds on the whole
#     500-frame clip (+2.0% / -1.1%). Sweeping 3000-20000 on all three 720p
#     clips and 500-8000 on all three CIF clips found no saturation anywhere.
#   - The real defect was the OPERATING POINT. At 2500 the CIF clips sat at VMAF
#     97.6-99.9 -- foreman 99.55 vs x264 99.61, bus 99.85 vs 99.86. That is
#     visually lossless, the metric has no headroom, and every dVMAF printed on a
#     CIF row was noise around zero rather than a quality result. At the other
#     end ducks_720p at 12000 sat at VMAF 75.5, far below any deployment point.
#     The scoreboard was miscalibrated in BOTH directions at once.
#
# Selection rule, applied per clip: the lowest ABR target at which both encoders
# track within a few percent AND yah264 lands in VMAF ~88-94, the band where the
# metric still discriminates and where the encoders are actually deployed. The
# resulting points: foreman 93.1, bus 94.3, stefan 92.2, samsung 88.3,
# park_joy 90.8, ducks 90.1.
#
# In the CRF scoreboard these numbers are a NOMINAL target rather than a
# contract: yah264's CRF reaches only a discrete ladder of rates (integer frame
# QP -- see scripts/crf-solve.py), so the run lands on the nearest reachable
# rung and reports the drift. The point of the number here is to pick the
# operating REGION, which it still does.
#
# uneven_720p is deliberately absent: it is mislabeled on disk (its container
# frame rate does not match its content), and every script here reads the frame
# rate off the clip.
# ---------------------------------------------------------------------------
# 2026-08-31: REBALANCED. The legacy six are preserved below as CLIPS_LEGACY,
# so any historical number stays reproducible; say which set a number came from.
#
# WHY. The old set was three CIF and three 720p, with NO 1080p at all, so every
# speed figure was a CIF-and-720p number. Twelve HD clips were calibrated the
# same day (docs/corpus-sources.md) and ten of them measured through this
# harness (local/records/board-hd-2026-08-31.log).
#
# WHAT THE REBALANCE DOES AND DOES NOT DO -- stated carefully, because the first
# draft of this comment got it wrong and the control caught it. Adding the four
# clips moves the median by 0.01 at one thread and not at all at twelve: the
# legacy six read 1.19x / 1.12x on the same box in the same session, against the
# rebalanced ten's 1.18x / 1.12x. **So this is a COVERAGE fix, not a correction
# to the level.** It buys honest reporting of where we stand across resolution
# and rate; it does not change what the number is.
#
# THE AXIS IS RATE, NOT RESOLUTION. Splitting those ten within one run:
#
#     under 3000 kbps (6 clips):  t1 1.44x   t12 1.15x
#     3000+ kbps      (4 clips):  t1 1.17x   t12 0.83x
#     720p            (4 clips):  t1 1.29x   t12 1.04x
#     1080p           (6 clips):  t1 1.36x   t12 1.13x
#
# Rate orders the table about four times as strongly as resolution, which is the
# operating-point result this tree already had, reproduced on content that never
# informed it. The legacy 720p clips sit at 12000 and 25000 kbps -- the regime
# where we are AHEAD -- so a set balanced on resolution alone would have left
# the rate blind spot in place. The additions are therefore chosen for RATE span
# within each resolution class:
#
#   shields_720p:2200      720p at a LOW rate, a regime the old board had only
#                          one clip in (samsung). Reads 1.48x / 1.15x.
#   sunflower_1080p:1500   the worst cell measured, 1.55x / 1.22x.
#   pedestrian_1080p:2800  1080p mid-rate, 1.31x / 1.10x.
#   riverbed_1080p:12500   1080p at a HIGH rate, and the one clip where we are
#                          FASTER: 0.78x / 0.79x. It is here precisely so the
#                          set is not stacked toward the regime we lose.
#
# Deliberately NOT added: blue_sky_1080p reads dVMAF -3.24 at its matched point
# (the B-direct failure, docs/b-direct-mode.md), so it would report a quality
# defect in the board's dVMAF column; crowd_run_1080p at 22000 kbps roughly
# doubles runtime for a cell riverbed already covers; in_to_tree, parkrun,
# fourpeople and station2 are calibrated and available in docs/corpus-sources.md
# for a wider sweep.
#
# AND A WARNING THAT COST A ROUND THE DAY THIS WAS WRITTEN: the numbers this
# harness prints are NOT the published goal figures. Those come from
# scripts/ffboard.py, both encoders as libraries in one ffmpeg process
# (docs/board-2026-08-27.md says so in its own header). This harness runs two
# CLIs with per-process setup inside the measurement and reads about 0.19x
# worse. Comparing one against the other reads as a regression that is not
# there. Say which board produced a number, every time.
CLIPS="${CLIPS:-foreman_cif:400 bus_cif:400 stefan_cif:400 ducks_720p:25000 park_joy_720p:12000 samsung_720p:1200 shields_720p:2200 sunflower_1080p:1500 pedestrian_1080p:2800 riverbed_1080p:12500}"

# The pre-rebalance set. Every G1/G2/G3 figure published before 2026-08-31 is
# this one; run with CLIPS="$CLIPS_LEGACY" to reproduce a historical number.
CLIPS_LEGACY="foreman_cif:400 bus_cif:400 stefan_cif:400 ducks_720p:25000 park_joy_720p:12000 samsung_720p:1200"

# The REVIEWER set: four clips that answer "how fast, how big, how good" without
# needing any of this project's context. It is a 2x2 rather than a list, because
# the two things that move a speed ratio most are resolution and how synthetic
# the content is, and one clip cannot separate them:
#
#              720p                     1080p
#   CGI        bbb_720p                 bbb10s_1080p_o120
#   camera     perseverance_720p        perseverance_1080p
#
# bbb is Big Buck Bunny, clean synthetic frames that compress unusually well.
# perseverance is NASA Mars rover footage, real sensor noise and real motion.
# Each appears at both resolutions as THE SAME CONTENT, so the 720p and 1080p
# rows differ by resolution alone. Reading only bbb overstates every encoder.
#
# The bitrates below are used only when this set is run in ABR; the reviewer
# board runs CRF, where the rate factor is the operating point and the bitrate
# field is ignored. They are order-of-magnitude points for these clips at
# preset medium, NOT calibrated in the parity-clip-calib.sh sense, and they are
# deliberately not on any scoreboard -- this set exists to be re-run by someone
# outside the project, not to produce a published goal figure.
REVIEW_CLIPS="bbb_720p:4000 perseverance_720p:6000 bbb10s_1080p_o120:8000 perseverance_1080p:12000"
REVIEW_CLIPS_720="bbb_720p:4000 perseverance_720p:6000"
REVIEW_CLIPS_1080="bbb10s_1080p_o120:8000 perseverance_1080p:12000"

# ---------------------------------------------------------------------------
# 2026-08-13: calibrated, but NOT on the scoreboard.
#
# sintel_720p and touchdown_420 appear in BD rounds (abr_lag_corpus.py and
# everything built on it) and had no calibrated point anywhere in the repo, so
# each round picked one by hand. They now have one, measured the same way as the
# six above -- scripts/parity-clip-calib.sh, which prints the ladder the rule is
# applied to. They live here rather than in CLIPS because neither belongs on the
# yah264-vs-x264 scoreboard, for two different reasons, both recorded below.
#
#   sintel_720p:700       6s window, 144 frames.  yah264 VMAF 90.84 (v0.6.1),
#                         89.56 NEG; rate error -0.3%.
#   touchdown_420:8000    FULL clip, 150 frames.  yah264 VMAF 88.84 / 86.48
#                         NEG; rate error -0.3%.
#
# The rate span of the 88-94 band matters as much as the point, because a BD
# ladder has to fit inside it and the usual one (0.625x-1.6x, a 2.56x span) does
# not always. Measured spans:
#
#   sintel_720p      88-94 spans ~600-1100 kbit/s -- a 1.8x span. The standard
#                    ladder does NOT fit; narrow it or accept rungs off the band.
#   touchdown_420    88-94 spans ~7500 kbit/s upward -- wide enough, but see below.
#
# sintel is NOT scoreboard material: x264's own ABR runs +19.5% to +26% hot on
# it at every rate from 450 to 4000 while yah264 tracks within 0.4%, so a dsize
# column comparing them is measuring x264's rate control, not either encoder's
# efficiency. docs/archive/abr-undershoot-investigation.md found the same thing.
#
# touchdown_420 is not usable in a BD round AT ALL, and calibration does not fix
# it. At the calibrated 8000 with three different in-band ladders it reads
# +4.27%, +61.01% and +1.31% for the same encode pair. Two causes, neither of
# them the target: it is 150 frames -- 5.0 seconds, ONE GOP at the default
# keyint -- so nothing averages out a single rate-control trajectory; and its
# VMAF/rate curve is nearly flat (~4.5 points over a 2.56x rate span) and
# NON-MONOTONIC in both arms, so the slope the BD fit inverts is sometimes the
# wrong sign. Note this is the opposite of saturation: the band is 85-93, the
# metric simply stops responding to rate. Do not average it into a corpus mean.
#
# touchdown_1080p is the 4:2:2 ORIGINAL. It must never enter a 4:2:0 VMAF sweep
# -- its Y4M header says C422, and a 4:2:0 sweep of it measures nothing. It is
# NOT dead weight though, and must not be deleted: scripts/conformance.sh globs
# tests/corpus/*.y4m, so that file is the suite's ONLY 4:2:2 recon-match
# coverage. Removing it would silently drop a real correctness test to tidy a
# benchmark. touchdown_420 is the converted clip and the one every speed and BD
# round actually uses.
#
# Beyond the format, touchdown_420 is UNUSABLE IN BD ROUNDS regardless: three
# in-band ladders of the same encode pair at its calibrated 8000 read +4.27%,
# +61.01% and +1.31%. That is not saturation -- it is 150 frames with a flat,
# non-monotonic rate/VMAF curve in both arms, so no operating point fixes it.
# Keep it for speed and conformance; do not quote a BD number from it.
CLIPS_CALIB="${CLIPS_CALIB:-sintel_720p:700 touchdown_420:8000}"
