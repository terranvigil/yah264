# Denoising for grainy and noisy sources: `--nr` and `--mcstf` (2026-09-27)

Branch `item-denoise`, off main at `7cd4c0d`. Two tools, both **off by
default**, both reached through the environment (the CLI flags write it) so
`yah264_param_t` and `include/yah264.h` keep their layout.

**Outcome.** `--nr` (inter 100, noise-gated) is clean on the standard CRF
band (7 of 10 better, none worse) and threaded, costs 1.2-2.3% of the
instructions, and is held back from a medium flip by one HD clip at the
per-clip bar. `--mcstf` (range 1, temporal-noise gated) is the bigger tool on
grain and sensor noise (up to -2.8% on the grain titles' excerpts, -17% on
touchdown) but loses on foreman_cif and one grain excerpt and costs 10-13%;
opt-in. Neither is on by default.

All quality numbers are BD-VMAF-NEG of the arm against the shipped default on
the same binary and the same CRF ladder (negative = the arm is better), read
against the ORIGINAL source: the harnesses score the decoded stream against
the y4m they fed in, never against a filtered buffer. Release `-O3` builds
only. Grain clips are 120-frame 720p excerpts of the two tier C grain titles
(`scripts` do not fetch them; see "Clips" below).

## Clips

| name | source | offset | size |
|---|---|---|---|
| noct_a_720p | Nocturne (film-grain version), longform/nocturne_filmgrain | 50 s | 1280x720, 60p |
| noct_b_720p | the same | 200 s | 1280x720 |
| lash_a_720p | Lash of the Whip (1924 scan), longform/lash_of_the_whip_1924 | 40 s | 1280x960, 24p (pillarboxed 4:3) |
| lash_b_720p | the same | 600 s | 1280x960 |

`ffmpeg -ss <off> -i <mov> -frames:v 120 -vf scale=W:H:flags=lanczos,format=yuv420p`.

## 1. `--nr`: coefficient-domain noise reduction

Nothing like it existed in yah264 (docs/x264-parity-plan.md listed it as B-nr,
unbuilt). Adapted from the sibling HEVC encoder's module (same project) to
H.264's two transform sizes:

- per coefficient POSITION, per category (intra/inter x luma/chroma x 4x4/8x8),
  a running mean of |coef| over the blocks already quantised; the offset
  `strength * w / (mean + 1)` is taken toward zero off every coefficient before
  either quantiser sees it, bounded by the position's own mean;
- **the H.264 change**: the integer transforms are not orthonormal, so a
  position's raw magnitude carries its basis gain (1 to 6.25 across a 4x4).
  The offset is scaled by that squared gain (`w`), measured once by pushing
  impulses through the C transform. The unweighted form lost on both clips
  it was read on (park_joy -0.47 / -0.06 at 200 / 600 against -0.75 weighted
  at 100; sunflower -0.18 / +0.34) and was removed on 2026-10-04;
- statistics fold on the driver in coding order from per-row tally lines, so
  `--threads 1` repeats byte for byte; one block in four is tallied;
- **a noise gate** (`Y264_NR_GATE`, 0.75 levels by default when armed): a
  spatial Laplacian-percentile reading of the source picture, with near-black
  and near-white blocks excluded. Pictures that read clean are coded with
  zero offsets.

Knobs: `Y264_NR_INTRA`, `Y264_NR_INTER` (0-2000), `Y264_NR_GATE`,
`Y264_NR_TRACE`. CLI: `--nr N` (both), `--nr-intra`,
`--nr-inter`.

### Strength sweep (HD + grain, 60 frames, t1, medium)

| clip | w25 | w50 | w100 | inter-only 100 |
|---|--:|--:|--:|--:|
| lash_a_720p | -0.35 | -0.76 | -0.90 | -1.07 |
| lash_b_720p | -0.03 | -0.24 | +0.17 | +0.51 |
| noct_a_720p | -0.25 | +0.47 | +1.93 | +0.27 |
| noct_b_720p | -0.10 | +0.14 | -0.28 | -0.29 |
| bbb_720p | -0.14 | -0.24 | -0.47 | -0.81 |
| fourpeople_720p | -0.10 | -0.34 | -0.23 | -0.32 |
| shields_720p | -0.26 | +0.24 | +0.37 | -0.80 |
| crowd_run_1080p | -0.21 | -0.20 | -0.35 | -0.30 |
| ducks_720p | -0.12 | +0.34 | -0.02 | +0.10 |
| foreman_cif | +0.49 | -0.06 | -0.71 | -0.66 |
| median | -0.13 | -0.13 | -0.25 | **-0.31** |

Inter-only is the best shape; intra attenuation is what costs the
added-in-post grain of Nocturne.

### Standard CRF band (run_band.py, 120 frames, t1, ladder calibrated on the reference encoder)

| clip | inter 100, ungated | inter 100, gate 0.75 |
|---|--:|--:|
| foreman_cif | -0.20 | -0.20 |
| bus_cif | -0.23 | -0.23 |
| stefan_cif | -0.22 | -0.22 |
| akiyo_cif | **+1.19** | 0.00 (gated) |
| mobile_cif | +0.12 | +0.12 |
| ducks_720p | -1.40 | -1.40 |
| park_joy_720p | -0.96 | -0.96 |
| samsung_720p | **+1.03** | 0.00 (gated) |
| sintel_720p | **+7.58** | 0.00 (gated) |
| touchdown_420 | -10.91 | -10.91 |

Ungated, a coefficient-domain denoise on a clean digital source removes
structure, which is the sibling encoder's finding too (its sintel lost 1.44
at strength 100). All three losers read under 0.65 on the gate's scale
(sintel 0.00, samsung 0.44, akiyo 0.62) and come back byte-identical to the
default behind it.

### HD + grain band (hd_band.py, 120 frames, t1, one control arm)

inter 100 with the 0.75 gate (`g100`) and inter 200 (`g200`); the control
column is `Y264_MBT_DEPFLOOR=16`, a near-null arm, so its spread is the
instrument's own per-clip floor.

| clip | g100 | g200 | control |
|---|--:|--:|--:|
| shields_720p | -1.12 | -1.39 | -0.12 |
| park_joy_720p | -0.61 | -0.82 | -0.23 |
| bus_cif | -0.52 | -0.81 | -0.67 |
| fourpeople_720p | -0.43 | -0.25 | -0.09 |
| crowd_run_1080p | -0.29 | -0.61 | +0.16 |
| stefan_cif | -0.26 | -0.59 | -0.39 |
| ducks_720p | -0.16 | -0.16 | +0.11 |
| foreman_cif | -0.12 | -0.53 | -0.55 |
| noct_b_720p | -0.01 | +0.67 | +0.47 |
| sintel, lash_a, lash_b, perseverance, bbb, sunflower | 0.00 (gated) | 0.00 | |
| riverbed_1080p | +0.14 | +0.25 | -0.21 |
| noct_a_720p | +0.64 | +1.12 | +0.47 |
| pedestrian_1080p | **+0.69** | **+1.31** | +0.03 |
| median | -0.01 | +0.00 | -0.08 |

Re-read on the final binary with both controls: pedestrian_1080p +0.48 raw
(+0.45 / +0.58 against the two controls), noct_a_720p +0.80 raw but +0.33 /
+0.17 against controls that themselves read +0.47 / +0.64 there, riverbed
+0.13. The gate costs the grain wins the ungated sweep found on
lash_a_720p (-1.07): the 1924 scan reads 0.50-0.62 on the spatial scale.

### Threaded and cost

`--threads 12` (hd_band.py THREADS=12, 120 frames), inter 100 gated:
shields -1.08, foreman -0.87, park_joy -0.73, noct_b -0.32, fourpeople -0.15,
lash_a 0.00 (gated), pedestrian +0.28; median -0.32.

Instructions retired at `--threads 1`, medium, CRF 26, 60 frames, release
build, `/usr/bin/time -l` (load-immune): park_joy_720p +1.5%, foreman_cif
+2.3%, lash_a_720p +1.2% (gated there, so that one is the pure overhead). The
first build cost +5.6-6.4%; the subtraction is now NEON (saturating subtract
of |v|, then the sign back by a select; bit-exact with the C loop) and one
block in four is tallied.

### Verdict

**Not flipped on at medium.** The standard band is the clean shape a flip
wants (7 of 10 better, the three clean sources byte-identical behind the
gate, none worse), and threaded agrees, but the HD band has pedestrian_1080p
at the per-clip bar (+0.48 to +0.69 across three reads, controls at +0.03 and
-0.09), and the portfolio rule still needs the rest of the corpus neutral. It
ships as an opt-in: `--nr-inter 100` is the measured setting; `--nr N` sets
both classes and intra attenuation is what costs the added-in-post grain.
The next move is a gate that sees what pedestrian's residual is (a
detail-rich, low-noise street scene at 1.0-1.06 on the spatial scale, beside
park_joy at 1.06, which wins by a point).

## 2. `--mcstf`: motion-compensated temporal pre-filter

Ported from the sibling HEVC encoder's filter (same project): a queue in front
of the lookahead; every `Y264_MCSTF_PERIOD`-th picture (4, the anchor spacing
at `--bframes 3`) is replaced by a weighted average of itself and its
motion-compensated neighbours, with a temporal weight by distance, a
block-match-error weight and a per-sample photometric (Gaussian) weight.
Everything downstream, the lookahead included, sees the filtered picture;
the harness still scores against the original. A still sequence comes out
byte-identical (unit test), and the pictures out equal the pictures in at
1, 2, 5 and 23 frames.

**What changed from the port, and why.** The sibling gates on a spatial
noise reading at 1.5 levels. On these clips that reading cannot separate the
grain titles (Nocturne 1.38-1.44 at 720p; the 1924 scan 0.50, because a
quarter of its picture is black pillarbox and the scan's grain spans several
samples) from foreman_cif (1.19-1.44). The gate here reads **temporal**
noise: the 25th percentile of the nearest neighbours' block match errors,
blocks near black or white excluded, the quieter of the two sides (a cut sits
on at most one). Animation reads 0.00-0.25 and every natural clip above 0.75;
the bar is 0.5 (`Y264_MCSTF_CLEAN`).

Refinements, each behind its own knob and each read alone against the port
(`m0`: range 2, gate 0.5):

- (a) `Y264_MCSTF_ADAPT` (+ `_ADAPT_K`): per-block weight full up to K/4 x the
  noise's own match error, falling with the square of the excess, and the
  photometric width narrowing in proportion.
- (b) `Y264_MCSTF_SUBPEL`: half- and quarter-sample refinement only where the
  integer match error is above 2x the noise error (6 levels without ADAPT).
- (c) `Y264_MCSTF_CUT`: a neighbour whose mean match error is past
  3 x noise + 12 levels, and everything beyond it on that side, is dropped, so
  a cut picture filters from its own shot only. (An IDR at the start of a GOP
  instance has no past neighbours at all, since the CLI opens one encoder per
  GOP.)
- (d) `Y264_MCSTF_QP`: a QP offset for filtered pictures (frame_qp). The
  stronger mb-tree strength for filtered pictures was not built.
- (e) `Y264_MCSTF_QADAPT`: photometric width x 2^((rf-26)/12), clamped
  0.6-1.8.

### Sweep 1: each refinement alone (60 frames, t1)

| clip | m0 port | (a) adapt | (b) subpel | (c) cut | (d) QP -1 | (e) q-adapt |
|---|--:|--:|--:|--:|--:|--:|
| lash_a_720p | -5.84 | -1.90 | -5.35 | -5.84 | -3.32 | -6.36 |
| shields_720p | -2.48 | -2.17 | -1.63 | -2.48 | -3.59 | -2.53 |
| fourpeople_720p | -1.84 | -1.86 | -1.46 | -1.84 | -1.57 | -1.75 |
| noct_a_720p | -1.64 | -1.42 | -1.28 | -1.70 | -1.09 | -1.00 |
| noct_b_720p | -1.37 | -1.25 | -0.70 | -1.37 | -2.06 | -1.13 |
| lash_b_720p | -1.16 | -1.14 | -0.53 | -1.16 | +1.25 | -0.67 |
| crowd_run_1080p | -0.53 | -0.34 | +0.03 | -0.53 | -0.07 | -0.44 |
| ducks_720p | +0.04 | -0.37 | +0.51 | +0.04 | +1.22 | -0.18 |
| sintel_720p | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| bbb_720p | +0.09 | -0.00 | +0.03 | +0.09 | +0.23 | +0.16 |
| park_joy_720p | +0.35 | +0.55 | +1.05 | +0.35 | +0.87 | +0.43 |
| foreman_cif | **+1.25** | +0.30 | +1.13 | +1.25 | +0.82 | +0.86 |
| median | -0.85 | -0.76 | -0.27 | -0.85 | -0.04 | -0.56 |

(b) and (d) are refused: subpel finds better SAD on the noise itself and
averages less; spending a QP on filtered pictures buys less than it costs.
(c) is inert on this set (no clip carries a cut the gate lets through) and
free. (a) fixes foreman and gives back most of lash_a; (e) is mixed.

### Sweep 2 and 3: range, period, adapt threshold (60 frames)

| arm | median | mean | worst |
|---|--:|--:|--:|
| adapt K=8 | -0.77 | -0.92 | park_joy +0.58 |
| adapt K=12 | -0.77 | -0.86 | foreman +1.46 |
| period 1 | -0.04 | +0.15 | lash_b +5.00 |
| period 8 | -0.53 | -0.66 | park_joy +0.56 |
| **range 1** | -0.37 | -0.85 | **bbb +0.00** |
| range 1 + adapt K=8 | -0.70 | -0.75 | foreman +0.16 |
| range 1 + adapt K=5 | -0.61 | -0.60 | foreman +0.52 |
| range 1 + q-adapt | -0.34 | -0.97 | bbb +0.02 |
| range 1 + period 8 | -0.28 | -0.48 | foreman +0.64 |

Range 1 is the one arm with nothing worse, for half the search: it is the
armed default.

### Confirmation at 120 frames (18 clips, one control)

Range 1 (`r1`, the armed default), plus q-adapt (`r1e`) and plus adapt K=8
with q-adapt (`r1a8e`), control `Y264_MBT_DEPFLOOR=16`.

| clip | r1 | r1e | r1a8e | control |
|---|--:|--:|--:|--:|
| lash_a_720p | **-2.84** | -3.30 | -1.56 | +0.01 |
| shields_720p | -2.24 | -1.43 | -1.62 | -0.12 |
| fourpeople_720p | -2.20 | -2.55 | -1.98 | -0.09 |
| pedestrian_1080p | -0.58 | -0.30 | -0.65 | +0.03 |
| stefan_cif | -0.53 | -0.31 | -0.62 | -0.39 |
| crowd_run_1080p | -0.50 | -0.43 | -0.39 | +0.16 |
| park_joy_720p | -0.50 | -0.41 | -0.37 | -0.23 |
| bus_cif | -0.48 | -0.56 | -0.55 | -0.67 |
| noct_b_720p | -0.33 | -0.37 | -0.31 | +0.47 |
| ducks_720p | -0.20 | -0.10 | +0.13 | +0.11 |
| sintel_720p | 0.00 | 0.00 | 0.00 | -0.56 |
| bbb_720p | +0.04 | +0.13 | +0.12 | +0.25 |
| foreman_cif | +0.39 | +0.79 | +0.53 | -0.55 |
| riverbed_1080p | +0.49 | +0.78 | +1.57 | -0.21 |
| sunflower_1080p | +0.50 | +0.58 | +0.87 | -0.08 |
| perseverance_720p | +0.55 | +0.47 | +0.31 | -0.09 |
| noct_a_720p | +0.58 | +0.31 | +0.31 | +0.47 |
| lash_b_720p | **+1.43** | +1.60 | +0.79 | -0.03 |
| median | -0.26 | -0.20 | -0.15 | -0.08 |

Range 1 with adapt K=8 and no q-adapt, same 18 clips: median -0.19, mean
-0.27; foreman_cif +0.03, lash_b_720p +0.86, sunflower_1080p +0.70,
riverbed_1080p +1.17 (water: the match error there is motion the search
cannot follow, which ADAPT reads as noise to keep), lash_a_720p only -0.84.

At 120 frames the 60-frame picture holds on the big winners and loses its
clean tail: five clips sit at +0.4 to +0.6, inside or at the controls'
spread, and lash_b_720p is a real +1.43. perseverance_720p is armed on 6 of
30 candidate pictures (temporal readings 0.50-0.81 late in the clip).

### Standard CRF band, threaded, cost

**Standard CRF band** (run_band.py, 120 frames, t1):

| clip | r1 (armed default) | r1 + adapt K=8 |
|---|--:|--:|
| foreman_cif | **+1.20** | **+1.17** |
| bus_cif | -0.91 | -0.67 |
| stefan_cif | -0.25 | +0.30 |
| akiyo_cif | 0.00 (gated) | 0.00 |
| mobile_cif | -0.01 | +0.27 |
| ducks_720p | -0.41 | +0.10 |
| park_joy_720p | -0.34 | -0.01 |
| samsung_720p | 0.00 (gated) | 0.00 |
| sintel_720p | 0.00 (gated) | 0.00 |
| touchdown_420 | -17.12 | -19.43 |

touchdown_420 is real: 11-24% fewer bits per rung for 0.05-0.34 VMAF-NEG.
foreman_cif is the loser the sibling encoder found too, and at 120 frames no
per-block form here fixes it; ADAPT at K=8 costs more elsewhere than it buys
there.

**Threaded** (`--threads 12`, 120 frames), r1: lash_a -2.81, fourpeople
-2.37, shields -1.73, noct_b -0.99, pedestrian -0.80, park_joy -0.36,
foreman +0.33; median -0.99.

**Cost**, instructions retired, t1, medium, CRF 26, 60 frames: park_joy_720p
+11.5%, foreman_cif +12.6%, lash_a_720p +10.1%. With every picture gated out
the reading alone costs +5.6% (the two nearest neighbours' searches run to
take it). The first build cost +31-34%; the fine search now uses the
dispatched 16x16 SAD, the coarse 4x4 SAD is inlined, and the average runs
block by block; all three are bit-exact with what was measured. The filter
runs on the DRIVER, serially in front of the lookahead, so its share of the
wall grows with the thread count; that is unmeasured here (daytime rule), and
it is the first thing to read before any preset placement.

### Verdict

**Default off.** It is the strongest tool of the two on the content it is
for -- lash_a_720p -2.8, fourpeople_720p -2.2/-2.4, shields_720p -1.7/-2.2,
touchdown_420 -17 -- but it is not neutral elsewhere: foreman_cif +1.2 on the
standard band and lash_b_720p +1.4 on the HD band, with four more clips at
+0.4-0.6, and it costs 10-13% of the instructions. Recommended as an opt-in
for grainy and sensor-noise sources: `--mcstf` (range 1, period 4, temporal
gate 0.5).

Refinements: (a) per-block adaptive weights -- fixes foreman on the HD band
(+0.39 -> +0.03) but not on the standard band (+1.17), moves the loss to
riverbed (+1.17), and gives back most of the lash_a win; kept as a knob,
off. (b) subpel
only on poor matches -- refused, worse on 9 of 12. (c) cut-side selection --
inert: forced past the gate (`Y264_MCSTF_CLEAN=0`, 120 frames) it fires at
three of samsung_720p's cuts and moves that clip +0.44 -> +0.41; sintel's cut
never lands on a filtered picture. Kept as a knob, off. The same forced read
is the gate's own justification: ungated, sintel_720p +1.15, samsung_720p
+0.44, bbb_720p +0.12; gated all three are byte-identical to the default. (d) filtered pictures at -1 QP --
refused (median -0.85 -> -0.04); the mb-tree strength half was not built.
(e) QP-adaptive width -- mixed (lash_a better, noct_a worse), knob, off.

**Both together** (`--mcstf` plus `--nr-inter 100`, gated, 120 frames) on
the grain and noise clips: median -1.85 against -2.20 for `--mcstf` alone;
touchdown_420 gains 1.2 more, noct_a_720p loses 0.3 more. The two do not
stack usefully; `--mcstf` alone is the recipe.

The next step is the foreman question: a gate that separates "the match
error is noise" from "the match error is motion the block search could not
follow" before averaging, since foreman's temporal reading (1.7-1.9) sits
beside Nocturne's (2.0).
