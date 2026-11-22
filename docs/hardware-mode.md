# Hardware mode

`--hw videotoolbox` encodes with the Mac's fixed-function H.264 engine through
the VideoToolbox API, driven by yah264's options. Those options map onto the
hardware's settings as far as they go. Our scene-cut decides the keyframes.
At open the CLI names each option the hardware can't honor, once.

The stream is Apple's. Recon-match, conformance, determinism, the quality
bands and the parity goals don't apply to it. It never appears in their rows
and has its own row on the site, labeled as hardware.

GPU compute over our own encoder is a separate route, covered in
[gpu.md](gpu.md).

## How it works

`--hw off|auto|videotoolbox` picks the mode. The default is `off`. The
library takes the mode at open (`yah264_encoder_open_hw`) instead of in the
parameter struct. `yah264_encoder_backend` names what actually opened.
`Y264_HW` sets the mode for the test scripts. The meson option `videotoolbox`
is on by default on macOS. Elsewhere a stub's open returns NULL with a reason.
Callers need no ifdefs.

The session requires the hardware encoder and never accepts VideoToolbox's
software encoder. If the hardware session fails or won't confirm it's
hardware, it's refused. Under `videotoolbox` a refusal fails the encode. Under `auto` it
prints one line naming why and falls back to yah264's own encoder.

Each input frame is copied into a buffer from the session's pool, because the
session holds frames past the call and the caller reuses its planes. On the
session's thread, the output callback converts each sample from
length-prefixed to Annex B. The encode call drains the samples in decode
order. SPS and PPS go out ahead of the first slice. At the end the stream
flushes. Since the session can drop a frame, the CLI counts the frames that
came back instead of the frames sent and flags a mismatch. The CLI's
GOP-parallel path is off in this mode because the hardware has its own
parallelism.

Our scene-cut runs causally on each frame as it arrives, using the same lowres
costs and decision rule as the lookahead, and forces an IDR on a cut with no
added latency. It has no frame of lookahead, so there's no flash guard: a
one-frame flash costs an extra IDR. `--no-scenecut`
turns it off. The hardware's keyframe interval stays the backstop.

## What maps and what doesn't

The map is one C table in `src/hw/vt_map.c`. Because the CLI's warnings read the
same table, the text and the behavior can't drift apart.

| yah264 | on the hardware | class |
|---|---|---|
| `--cabac` / `--cavlc` | entropy mode | maps |
| `--keyint` | maximum keyframe interval | maps |
| frame rate | expected frame rate | maps |
| `--bitrate` | average bitrate | maps |
| `--vbv-maxrate`, `--vbv-bufsize` | a data-rate limit of maxrate over a window of bufsize / maxrate seconds | approximated |
| `--crf` | the hardware's 0-to-1 quality key, through a fixed rate-matched table (below) | approximated |
| `--qp` | minimum and maximum frame QP both set to N. The hardware may still move QP inside a frame | approximated |
| `--bframes` | frame reordering on or off. The hardware picks the count | approximated |
| `--tune zerolatency` | the hardware's real-time mode (it arrives as no B frames and no lookahead) | approximated |
| `--scenecut`, `--no-scenecut` | our scene-cut forcing the hardware's keyframes | ours |
| `--no-sei` | the hardware writes none of our SEI anyway | maps |
| profile | always High, level chosen by the hardware | fixed |
| `--ref`, `--min-keyint`, `--b-adapt`, `--b-bias`, `--direct`, `--zones`, `--qpfile`, `--pass`, `--stats`, `--abr-model`, `--crf-max`, `--ratetol`, `--qcomp`, `--rc-lookahead`, `--sync-lookahead`, `--preset`, other tunes, `--threads`, `--aq-strength`, `--psy-rd`, `--psy-trellis`, `--trellis`, the deadzones, the `--cqm` family, `--me`, `--merange`, `--subme`, `--subpel`, `--partitions`, `--no-mixed-refs`, `--no-chroma-me`, `--transform-8x8`, `--cll`, `--mastering-display`, `--alternative-transfer`, `--frame-stats` | nothing | ignored |
| `--dump-recon`, `--slices` above 1, anything but 4:2:0, input above 8 bits | nothing | refused |

The CRF table is rate-matched. Each point is the quality at which the
hardware spends what yah264 spends at that CRF, as the median over six clips.
The per-clip spread is about 0.1. Between points the table is linear. At
crf 28 the hardware comes within 16% of our rate over the six clips.

| CRF | 20 | 24 | 28 | 32 | 36 |
|---|--:|--:|--:|--:|--:|
| quality | 0.69 | 0.59 | 0.50 | 0.41 | 0.30 |

At open, the CLI prints one block listing each option the user set that's
ignored or approximated, with its note. Defaults print nothing. `--hw-strict`
turns that block into an error, for scripted use.

## The trade

The table covers the ten board clips at their ABR targets, scored with
VMAF-NEG. Our encoder ran at auto threads. Each cell is the median of
five runs per side on a quiet machine.

| clip | wall, hardware / ours | CPU, ours / hardware | VMAF, hardware minus ours |
|---|--:|--:|--:|
| foreman_cif | 1.88 | 12 | -0.7 |
| bus_cif | 1.78 | 11 | -6.0 |
| stefan_cif | 2.29 | 8 | -3.9 |
| ducks_720p | 0.62 | 48 | -8.1 |
| park_joy_720p | 0.65 | 32 | -7.4 |
| fourpeople_720p | 1.45 | 16 | -4.2 |
| shields_720p | 1.02 | 18 | -4.7 |
| sunflower_1080p | 0.93 | 18 | -3.9 |
| pedestrian_1080p | 0.80 | 27 | -4.7 |
| riverbed_1080p | 0.32 | 46 | -13.9 |

At the median the hardware uses about an eighteenth of our CPU and about the
same wall time. It's faster on high-rate clips and most HD. On CIF and
fourpeople it's slower, because opening the session is a large share of a
short run.

Quality pays for the savings. The hardware scores below us at the same bitrate
on all ten clips, by 4.7 points at the median. It comes within about 6% of its
rate target on eight clips. On bus and stefan every encoder undershoots the
400 kbit/s target. The hardware spends more bits than we do on eight of ten,
which favors it in the quality column. CPU counts the encoder process only and
leaves out work the hardware does elsewhere. Use hardware mode when CPU
matters more than bits.

## What we chose and why

| choice | why |
|---|---|
| default `--hw off` | `auto` silently swaps in a different encoder. Every gate in the tree assumes ours |
| hardware only, never VideoToolbox's software encoder | the mode exists for the hardware's CPU savings. Without the hardware, our own encoder is the software path |
| `--crf` onto the quality key through a rate-matched table, with a warning | the hardware has no CRF. A fixed monotone table is cheap and comes within about 16% of our rate at CRF 28 over six clips |
| warn once on options the user set, silent on defaults, and `--hw-strict` to refuse | a default run shouldn't print noise. Scripts need a way to fail loudly |
| `auto` falls back with one line, and `videotoolbox` never falls back | a forced mode that quietly ran something else would lie |
| `--slices` refused instead of ignored | the session slices its own way and takes no count. Accepting it would mean saying yes and doing something else |
| CLI and library only, not in the ffmpeg wrapper | ffmpeg users already have `h264_videotoolbox`. The only thing we'd add is our scene-cut |

## What didn't work

| idea | why not |
|---|---|
| passing mb-tree's per-frame offsets as a base-QP hint | the H.264 path ignores the hint: output is byte-identical with it at 20, 40 or absent, under CRF and ABR |
| setting the frame delay count for zero latency | it's read-only on this hardware. Real-time mode is the whole map |

## Open questions

- CRF emulation: run our lookahead and set the average bitrate per shot so the
  hardware tracks what our CRF would spend. It's the only form that would make
  two-pass mean anything on the hardware. Not built.
- `--level` and `--sar` are accepted without a warning even though the session
  sets neither. The map table should either apply them or list them as ignored.

## Measuring it

- The hardware isn't byte-stable run to run. Its VMAF moved by up to 0.6
  points across five runs on the board. Ours repeated to within about 0.2.
- `scripts/regress.py` draws `--hw auto` in a quarter of its cells and checks
  only "encodes, decodes, frame count matches". It doesn't assert determinism there.
- A raw `.264` from the hardware has no VUI timing. A decoder guesses the
  rate. Align frames by index when scoring, or the PSNR filter pairs the wrong
  frames.
- `tools/vtprobe` prints the hardware's supported property dictionary for
  H.264 on the machine it runs on. Check the map against it on a new macOS.
