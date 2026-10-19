# Denoise

yah264 has two denoising tools, both off by default. `--nr` takes a learned
offset off transform coefficients before quantization. `--mcstf` filters the
source in time before the lookahead sees it. Both exist for grainy and noisy
sources, and both carry a noise gate so clean content (animation above all)
comes out byte-identical to an encode without them.

Every quality number here is BD-rate on VMAF-NEG of the tool against the
shipped default on the same binary and CRF ladder (negative is better), scored
against the original source, never the filtered one.

## When to use which

| source | reach for | default |
|---|---|---|
| noisy live action, sensor noise (shields, touchdown) | `--mcstf`, or `--nr-inter 100` for a cheaper, smaller gain | off |
| film grain | `--mcstf` | off |
| clean digital, animation | neither (both gate themselves off anyway) | off |

Don't combine them. `--mcstf` plus `--nr-inter 100` reads a median of -1.85 on
the grain and noise clips against -2.20 for `--mcstf` alone.

## How it works

### `--nr`: coefficient noise reduction

`--nr N` sets both block classes; `--nr-intra` and `--nr-inter` set one each
(0 to 2000, default 0). The encoder keeps a running mean of |coefficient| per
transform position and per category (intra/inter, luma/chroma, 4x4/8x8) over
blocks already quantized. Each coefficient is pulled toward zero by
`strength * w / (mean + 1)`, bounded by that mean. H.264's integer transforms
aren't orthonormal, so `w` is the position's squared basis gain, measured once
by pushing impulses through the C transform. One block in four is tallied, and
the statistics fold in coding order, so `--threads 1` repeats byte for byte.
The subtraction runs in NEON.

A spatial noise gate, always at 0.75 levels, reads a Laplacian
percentile of each source picture, skipping near-black and near-white blocks.
Pictures under the bar get zero offsets. The code lives in
`src/encoder/nr.c`.

### `--mcstf`: motion-compensated temporal filter

A queue in front of the lookahead. Every fourth picture (`Y264_MCSTF_PERIOD`,
the anchor spacing at the default `--bframes 3`) is replaced by a weighted
average of itself and its motion-compensated neighbors, `--mcstf-range` per
side (1 to 4, default 1). Three weights combine: temporal distance, block match
error, and a per-sample photometric weight (width 6 levels). Everything
downstream, the lookahead included, sees the filtered picture.

The gate reads **temporal** noise: the 25th percentile of the nearest
neighbors' block match errors, skipping near-black and near-white blocks,
taken from the quieter side so a cut doesn't trip it. Pictures under 0.5
levels (`Y264_MCSTF_CLEAN`) pass through untouched. Animation reads 0.00-0.25
and every natural clip in the corpus reads above 0.75. The code lives in
`src/encoder/mcstf.c`.

## What we chose and why

| choice | why |
|---|---|
| both off by default | neither is neutral everywhere. `--nr` doesn't help film grain; `--mcstf` loses on foreman_cif (+1.2) and costs 10-13% of the instructions |
| `--nr-inter 100` as the setting to use | neutral to better on the HD band (median -0.06; shields -1.07, park_joy -0.43, crowd_run -0.32), with every clean clip byte-identical. `--nr 100` with intra on fails the per-clip bar on pedestrian_1080p (+0.50) |
| inter only | intra attenuation removes the added-in-post grain on Nocturne |
| per-position basis-gain weight in `--nr` | the unweighted form lost on both clips it was read on |
| spatial gate at 0.75 for `--nr` | ungated, it strips structure from clean sources: sintel +7.58, akiyo +1.19, samsung +1.03. All three read under 0.65 |
| `--mcstf` range 1 | the only setting with no clip worse than +0.00 at 60 frames, at half the search of range 2 |
| `--mcstf` temporal gate, not spatial | a spatial reading can't separate the grain titles (1.38-1.44 at 720p) from foreman_cif (1.19-1.44), and reads a pillarboxed 1924 scan at 0.50 |

`--mcstf` on the content it's for, 120 frames:

| clip | one thread | 12 threads |
|---|--:|--:|
| touchdown_420 | -17.1 | |
| lash_a_720p (1924 scan) | -2.84 | -2.81 |
| fourpeople_720p | -2.20 | -2.37 |
| shields_720p | -2.24 | -1.73 |
| foreman_cif | +1.20 (standard band) | +0.33 |
| lash_b_720p | +1.43 | |

touchdown's gain is real: 11-24% fewer bits per rung for 0.05-0.34 VMAF-NEG.

Cost in instructions retired, one thread, medium: `--nr` +1.2% to +2.3%,
`--mcstf` +10% to +13%. With every picture gated out, `--mcstf` still costs
+5.6%, because the gate's own neighbor searches run.

## What didn't work

| idea | why not |
|---|---|
| `--nr` with intra on | costs the added-in-post grain on Nocturne |
| `--nr` unweighted by basis gain | lost on park_joy and sunflower |
| `--mcstf` subpel refinement on poor matches (`Y264_MCSTF_SUBPEL`) | worse on 9 of 12; it finds better matches on the noise itself and averages less |
| `-1` QP on filtered pictures (`Y264_MCSTF_QP`) | median -0.85 shrinks to -0.04 |
| `--mcstf` every picture (period 1) | lash_b +5.0 |
| per-block adaptive weights (`Y264_MCSTF_ADAPT`) | fixes foreman on the HD band but not the standard band, moves the loss to riverbed (+1.17) and gives back most of lash_a's win. Knob, off |
| QP-adaptive photometric width (`Y264_MCSTF_QADAPT`) | mixed: lash_a better, noct_a worse. Knob, off |
| cut-side neighbor selection (`Y264_MCSTF_CUT`) | inert: no cut on the measured set gets past the gate. Knob, off |

## Open questions

- **A gate that tells grain from detail for `--nr`.** The spatial gate gives up
  the grain win on lash_a (-1.07 ungated) because the 1924 scan reads clean.
  Any gate that arms there also has to stay off sintel_720p.
- **Noise versus untracked motion for `--mcstf`.** foreman's temporal reading
  (1.7-1.9) sits beside Nocturne's (2.0). The gate needs to separate "the match
  error is noise" from "the match error is motion the search couldn't follow".
  Riverbed (water) is the same problem.
- **`--mcstf` wall share at high thread counts.** The filter runs serially on
  the driver ahead of the lookahead, so its share of wall grows with threads.
  It hasn't been measured; read it before putting `--mcstf` in any preset.

## Measuring it

The grain clips are 120-frame excerpts the scripts don't fetch:

| name | source in `/Volumes/T7/media/longform/` | offset | size |
|---|---|---|---|
| noct_a_720p | nocturne_filmgrain | 50 s | 1280x720 |
| noct_b_720p | nocturne_filmgrain | 200 s | 1280x720 |
| lash_a_720p | lash_of_the_whip_1924 | 40 s | 1280x960 (pillarboxed 4:3) |
| lash_b_720p | lash_of_the_whip_1924 | 600 s | 1280x960 |

    ffmpeg -ss <off> -i <mov> -frames:v 120 -vf scale=W:H:flags=lanczos,format=yuv420p

Read `scripts/hd_band.py` with a near-null control arm (`Y264_MBT_DEPFLOOR=16`)
beside the tool: on the grain clips the control's own spread reaches +0.5,
which is as large as several of the tool's per-clip readings.
`Y264_NR_TRACE=1` and `Y264_MCSTF_TRACE=1` print the gate readings.
