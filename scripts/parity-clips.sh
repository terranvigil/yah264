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
CLIPS="${CLIPS:-foreman_cif:400 bus_cif:400 stefan_cif:400 ducks_720p:25000 park_joy_720p:12000 samsung_720p:1200}"

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
