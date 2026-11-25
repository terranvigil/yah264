# Denoise

yah264 has two denoising tools, both off by default. `--nr` takes a learned
offset off transform coefficients before quantization. `--mcstf` filters the
source in time before the lookahead sees it. Both exist for grainy and noisy
sources. Each has a noise gate that keeps clean content byte-identical to an
encode without the tool, and animation is the case that matters most.

The quality numbers here are BD-rate on VMAF v1 of the tool against the
shipped default on the same binary and CRF ladder, where negative is better.
The `--mcstf` range row is the exception: it's still on VMAF-NEG (earlier
metric). Every reading is scored against the original source, never the
filtered one.

## When to use which

| source | reach for | default |
|---|---|---|
| noisy live action, sensor noise (shields, touchdown) | `--mcstf`, or `--nr-inter 100` for a cheaper, smaller gain | off |
| film grain | `--mcstf` | off |
| clean digital, animation | neither, since both gate themselves off anyway | off |

Don't combine them. On the four grain excerpts plus shields and fourpeople,
`--mcstf` plus `--nr-inter 100` reads a median of -1.26 against -1.44 for
`--mcstf` alone.

## How it works

### Coefficient noise reduction with `--nr`

`--nr N` sets both block classes, and `--nr-intra` and `--nr-inter` set one
each. Each takes 0 to 2000 and defaults to 0. Over blocks already quantized,
the encoder keeps a running mean of |coefficient| per transform position and
per category: intra or inter, luma or chroma, 4x4 or 8x8. Each coefficient is
pulled toward zero by `strength * w / (mean + 1)`, bounded by that mean.
Because H.264's integer transforms aren't orthonormal, `w` is the position's
squared basis gain, which we measured once by pushing impulses through the C
transform. One block in four is tallied and the statistics fold in coding
order, so `--threads 1` repeats byte for byte. The subtraction runs in NEON.

A spatial noise gate, always at 0.75 levels, reads a Laplacian percentile of
each source picture and skips near-black and near-white blocks. Pictures
under the bar get zero offsets. The code lives in `src/encoder/nr.c`.

### Motion-compensated temporal filter with `--mcstf`

The filter is a queue in front of the lookahead. Every fourth picture is
replaced by a weighted average of itself and its motion-compensated neighbors.
Four is the anchor spacing at the default `--bframes 3`. `--mcstf-range` sets
how many neighbors it uses per side. It takes 1 to 4 and defaults to 1. The weight
combines temporal distance, block match error and a per-sample photometric
weight 6 levels wide. Everything downstream, the lookahead included, sees the
filtered picture.

This gate reads temporal noise. It takes the 25th percentile of the nearest
neighbors' block match errors and skips near-black and near-white blocks. It
reads from the quieter side so a cut doesn't trip it. Pictures under 0.5
levels pass through untouched. Animation reads 0.00-0.25. Every natural clip
in the corpus reads above 0.75. The code lives in `src/encoder/mcstf.c`.

## What we chose and why

| choice | why |
|---|---|
| both off by default | neither is neutral everywhere. `--nr` doesn't help film grain; `--mcstf` loses on foreman_cif (+4.1) and riverbed (+4.2) and costs 10-13% of the instructions |
| `--nr-inter 100` as the setting to use | neutral on the HD band (median -0.04; ducks -0.60, crowd_run -0.56, shields -0.30), with every clean clip byte-identical. Its worst clip, pedestrian_1080p at +0.45, stays under the per-clip bar. `--nr 100` with intra on fails it there (+0.81) |
| inter only | intra attenuation removes the added-in-post grain on Nocturne: noct_a reads +1.20 with intra on, against +0.76 inter only |
| per-position basis-gain weight in `--nr` | the unweighted form lost on both clips it was read on |
| spatial gate at 0.75 for `--nr` | ungated, it strips structure from clean sources: akiyo +15.7, sintel +11.1, samsung +2.1 on the standard band. All three read under 0.65 |
| `--mcstf` range 1 | on VMAF-NEG, the only setting with no clip worse than +0.00 at 60 frames, at half the search of range 2 |
| `--mcstf` temporal gate, not spatial | a spatial reading can't separate the grain titles (1.38-1.44 at 720p) from foreman_cif (1.19-1.44), and reads a pillarboxed 1924 scan at 0.50 |

`--mcstf` on the content it's for, 120 frames on the HD band's CRF 23-35 ladder:

| clip | one thread | three threads |
|---|--:|--:|
| noct_a_720p | -8.01 | |
| lash_a_720p (1924 scan) | -4.52 | -2.59 |
| lash_b_720p | -1.52 | |
| noct_b_720p | -0.63 | |
| fourpeople_720p | -1.35 | -1.47 |
| shields_720p | -1.31 | -1.53 |
| foreman_cif | +4.11 (standard band) | +3.07 |

touchdown_420 isn't fit for a BD reading ([corpus-sources.md](corpus-sources.md)),
so we read it per rung. At matched CRF on the standard band, `--mcstf` takes up
to 16% fewer bits there and scores slightly higher on every rung.

In instructions retired at one thread and medium, `--nr` costs +1.2% to +2.3%
and `--mcstf` costs +10% to +13%. With every picture gated out, `--mcstf` still
costs +5.6% because the gate's own neighbor searches run.

## What didn't work

| idea | why not |
|---|---|
| `--nr` with intra on | costs the added-in-post grain on Nocturne |
| `--nr` unweighted by basis gain | lost on park_joy and sunflower |
| `--mcstf` subpel refinement on poor matches | worse on 9 of 12; it finds better matches on the noise itself and averages less |
| `-1` QP on filtered pictures | the median gain all but disappears |
| `--mcstf` every picture (period 1) | lash_b loses |
| per-block adaptive weights | fixes foreman on the HD band but not the standard band, moves the loss to riverbed and gives back most of lash_a's win |
| QP-adaptive photometric width | mixed: lash_a better, noct_a worse |
| cut-side neighbor selection | inert: no cut on the measured set gets past the gate |

## Open questions

`--nr` needs a gate that tells grain from detail. The spatial gate gives up
the grain win on lash_a, which reads -1.49 ungated, because the 1924 scan reads
clean. Any gate that arms there also has to stay off sintel_720p.

`--mcstf` needs to tell noise from untracked motion. foreman's temporal reading
of 1.7-1.9 sits beside Nocturne's 2.0. The gate has to separate "the match
error is noise" from "the match error is motion the search couldn't follow".
Riverbed, which is water, is the same problem.

Nobody has measured the `--mcstf` wall share at high thread counts. The filter
runs serially on the driver ahead of the lookahead, so its share of wall grows
with threads. Read it before putting `--mcstf` in any preset.

## Measuring it

The grain clips are 120-frame excerpts the scripts don't fetch:

| name | source in `/Volumes/T7/media/longform/` | offset | size |
|---|---|---|---|
| noct_a_720p | nocturne_filmgrain | 50 s | 1280x720 |
| noct_b_720p | nocturne_filmgrain | 200 s | 1280x720 |
| lash_a_720p | lash_of_the_whip_1924 | 40 s | 1280x960 (pillarboxed 4:3) |
| lash_b_720p | lash_of_the_whip_1924 | 600 s | 1280x960 |

    ffmpeg -ss <off> -i <mov> -frames:v 120 -vf scale=W:H:flags=lanczos,format=yuv420p

Run `scripts/hd_band.py` with a near-null control arm, `Y264_MBT_DEPFLOOR=16`,
beside the tool. The control needs a dev_knobs build. On the grain clips the
control's own spread reaches 0.9 (lash_a -0.87), as large as several of the
tool's per-clip readings.
