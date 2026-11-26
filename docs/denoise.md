# Denoise

yah264's two denoising tools, for grainy and noisy sources, are off by
default. `--nr` takes a learned offset off transform coefficients. `--mcstf`
filters the source in time before the lookahead. A noise gate keeps clean
content, animation above all, byte-identical.

Numbers are BD-rate on VMAF v1 against the shipped default on the same binary
and ladder, scored on the original source. Negative is better. The
`--mcstf` range row is on VMAF-NEG (earlier metric).

## When to use which

| source | reach for | default |
|---|---|---|
| noisy live action, sensor noise (shields, touchdown) | `--mcstf`, or `--nr-inter 100` for a cheaper, smaller gain | off |
| film grain | `--mcstf` | off |
| clean digital, animation | neither, since both gate themselves off anyway | off |

Don't combine them. On the grain excerpts plus shields and fourpeople, the
pair reads a median of -1.26 against -1.44 for `--mcstf` alone.

## How it works

### Coefficient noise reduction with `--nr`

`--nr N` (0 to 2000, default 0) sets intra and inter blocks. `--nr-intra` and
`--nr-inter` set one. The encoder keeps a running mean
of |coefficient| per transform position and category (intra or inter, luma or
chroma, 4x4 or 8x8). Each coefficient is pulled toward zero by
`strength * w / (mean + 1)`, bounded by that mean. `w` is the position's
squared basis gain, since H.264's integer transforms aren't orthonormal. We
measured it with impulses through the C transform. One block in
four is tallied, in coding order, so `--threads 1` repeats byte for byte. The
subtraction runs in NEON.

The gate reads a Laplacian percentile of each picture and skips near-black
and near-white blocks. Pictures under its bar get no offset. The code is
`src/encoder/nr.c`.

### Motion-compensated temporal filter with `--mcstf`

Every fourth picture, the anchor spacing at the default `--bframes 3`, is
replaced by a weighted average of itself and its motion-compensated neighbors.
`--mcstf-range` sets the neighbors per side, 1 to 4, default 1. The weight
combines temporal distance, block match error and a photometric weight 6
levels wide.

The gate takes the 25th percentile of the nearest neighbors' block match
errors and skips near-black and near-white blocks. It reads the quieter side
so a cut doesn't trip it. Pictures under its bar pass through. The code is
`src/encoder/mcstf.c`.

Gate readings, in levels:

| gate | bar | readings |
|---|--:|---|
| `--nr` spatial | 0.75 | akiyo, sintel and samsung all under 0.65. On a spatial reading the grain titles read 1.38-1.44 at 720p, foreman_cif 1.19-1.44 and the pillarboxed 1924 scan 0.50 |
| `--mcstf` temporal | 0.5 | animation 0.00-0.25, every natural clip in the corpus above 0.75, foreman 1.7-1.9, Nocturne 2.0 |

## What we chose and why

| choice | why |
|---|---|
| both off by default | neither is neutral everywhere. `--nr` doesn't help film grain; `--mcstf` loses on foreman_cif (+4.1) and riverbed (+4.2) and costs 10-13% of the instructions |
| `--nr-inter 100` as the setting to use | neutral on the HD band (median -0.04; ducks -0.60, crowd_run -0.56, shields -0.30), with every clean clip byte-identical. Its worst clip, pedestrian_1080p at +0.45, stays under the per-clip bar. `--nr 100` with intra on fails it there (+0.81) |
| inter only | intra attenuation removes the added-in-post grain on Nocturne: noct_a reads +1.20 with intra on, against +0.76 inter only |
| per-position basis-gain weight in `--nr` | the unweighted form lost on both clips it was read on |
| spatial gate at 0.75 for `--nr` | ungated, it strips structure from clean sources: akiyo +15.7, sintel +11.1, samsung +2.1 on the standard band |
| `--mcstf` range 1 | on VMAF-NEG, the only setting with no clip worse than +0.00 at 60 frames, at half the search of range 2 |
| `--mcstf` temporal gate, not spatial | a spatial reading can't separate the grain titles from foreman_cif. It also reads the pillarboxed 1924 scan as clean |

`--mcstf` on grain and noise, 120 frames on the HD band's CRF 23-35 ladder:

| clip | one thread | three threads |
|---|--:|--:|
| noct_a_720p | -8.01 | |
| lash_a_720p (1924 scan) | -4.52 | -2.59 |
| lash_b_720p | -1.52 | |
| noct_b_720p | -0.63 | |
| fourpeople_720p | -1.35 | -1.47 |
| shields_720p | -1.31 | -1.53 |
| foreman_cif | +4.11 (standard band) | +3.07 |

touchdown_420 [can't take a BD reading](corpus-sources.md). At matched CRF on
the standard band, `--mcstf` takes up to 16% fewer bits there and scores
slightly higher on every rung.

Instructions retired at one thread and medium. The gate's searches run even
when every picture passes through:

| tool | cost |
|---|--:|
| `--nr` | +1.2% to +2.3% |
| `--mcstf` | +10% to +13% |
| `--mcstf`, every picture gated out | +5.6% |

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

`--nr` needs a gate that tells grain from detail. The spatial gate reads the
1924 scan as clean and gives up lash_a's ungated -1.49. A gate that arms there
must stay off sintel_720p.

`--mcstf` needs a gate that tells noise from motion the search couldn't
follow. foreman reads just below Nocturne, and riverbed's water is the same
case.

The filter runs serially ahead of the lookahead, so its share of wall grows
with threads. Measure it before putting `--mcstf` in a preset.

## Measuring it

The grain clips are 120-frame excerpts the scripts don't fetch:

| name | source in `/Volumes/T7/media/longform/` | offset | size |
|---|---|---|---|
| noct_a_720p | nocturne_filmgrain | 50 s | 1280x720 |
| noct_b_720p | nocturne_filmgrain | 200 s | 1280x720 |
| lash_a_720p | lash_of_the_whip_1924 | 40 s | 1280x960 (pillarboxed 4:3) |
| lash_b_720p | lash_of_the_whip_1924 | 600 s | 1280x960 |

    ffmpeg -ss <off> -i <mov> -frames:v 120 -vf scale=W:H:flags=lanczos,format=yuv420p

Run `scripts/hd_band.py` with a near-null control arm beside the tool:
`Y264_MBT_DEPFLOOR=16`, in a dev_knobs build. On the grain clips the
control's spread reaches 0.9 (lash_a -0.87), as large as several tool
readings.
