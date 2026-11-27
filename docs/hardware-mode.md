# Hardware mode

`--hw videotoolbox` encodes with the Mac's fixed-function H.264 engine through
the VideoToolbox API. yah264's options map onto the hardware's settings where
they can. Our scene-cut decides the keyframes. At open the CLI names each
option the hardware can't honor, once.

The stream is Apple's, so recon-match, conformance, determinism, the quality
bands and the parity goals don't apply. It has its own row on the site,
labeled as hardware. GPU compute inside our own encoder is a separate route
([gpu.md](gpu.md)).

## How it works

| control | what it does |
|---|---|
| `--hw off\|auto\|videotoolbox` | picks the mode. The default is `off` |
| `yah264_encoder_open_hw` | the library takes the mode at open, not in the parameter struct |
| `yah264_encoder_backend` | names what actually opened |
| `Y264_HW` | sets the mode for the test scripts |
| meson `videotoolbox` | on by default on macOS. Elsewhere a stub's open returns NULL with a reason, so callers need no ifdefs |

Only the hardware encoder is accepted, never VideoToolbox's software one. A
session that fails or won't confirm it's hardware is refused. Under
`videotoolbox` that fails the encode. Under `auto` the CLI prints one line
naming why and falls back to yah264's own encoder.

Each input frame is copied into a buffer from the session's pool, because the
session holds frames past the call while the caller reuses its planes. The
output callback converts samples to Annex B on the session's thread. The
encode call drains them in decode order, SPS and PPS first. Because the
session can drop frames, the CLI counts the frames that came back and flags a
mismatch. The GOP-parallel path is off because the hardware has its own
parallelism.

Our scene-cut runs causally on each arriving frame with the lookahead's lowres
costs and decision rule. It forces an IDR on a cut with no added latency.
Because it has no lookahead, there's no flash guard: a one-frame flash costs an
extra IDR. `--no-scenecut` turns it off. The hardware's keyframe interval is
the backstop.

## What maps and what doesn't

The map is one C table in `src/hw/vt_map.c`. The CLI's warnings read the same
table, so text and behavior can't drift apart.

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

At each CRF point the hardware spends what yah264 spends, as a median over
six clips. The table is linear between points.

| CRF | 20 | 24 | 28 | 32 | 36 |
|---|--:|--:|--:|--:|--:|
| quality | 0.69 | 0.59 | 0.50 | 0.41 | 0.30 |

The per-clip spread is about 0.1, and at CRF 28 the hardware comes within 16%
of our rate.

At open the CLI prints one block naming each option the user set that's ignored
or approximated, with its note. Defaults print nothing. `--hw-strict` makes
that block an error.

## The trade

The hardware saves CPU and pays in quality, scoring below us at the same
bitrate on all ten board clips. Use it when CPU matters more than bits.

| clip | wall, hardware / ours | CPU, ours / hardware | VMAF v1, hardware minus ours |
|---|--:|--:|--:|
| foreman_cif | 1.88 | 12 | -6.1 |
| bus_cif | 1.78 | 11 | -11.9 |
| stefan_cif | 2.29 | 8 | -8.7 |
| ducks_720p | 0.62 | 48 | -9.9 |
| park_joy_720p | 0.65 | 32 | -9.2 |
| fourpeople_720p | 1.45 | 16 | -6.4 |
| shields_720p | 1.02 | 18 | -7.0 |
| sunflower_1080p | 0.93 | 18 | -3.9 |
| pedestrian_1080p | 0.80 | 27 | -5.3 |
| riverbed_1080p | 0.32 | 46 | -12.6 |

| at the median | hardware against ours |
|---|---|
| CPU | about an eighteenth |
| wall time | about the same |
| VMAF v1 | 7.8 points lower |

The hardware is faster on high-rate clips and most HD. It's slower on CIF and
fourpeople, where opening the session is a large share of a short run.

| about the table | |
|---|---|
| rate | clips run at their ABR targets. The hardware comes within about 6% of target on all ten clips |
| wall and CPU | median of five timed runs per side on a quiet machine, ours at auto threads. CPU counts the encoder process only, not work the hardware does elsewhere |
| VMAF v1 | read with our encoder at three threads. The hardware spends more bits than we do on seven of ten clips, which favors it here |

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

- CRF emulation (not built): run our lookahead and set the average bitrate
  per shot so the hardware tracks what our CRF would spend. Only this would
  make two-pass mean anything on the hardware.
- `--level` and `--sar` are accepted silently though the session sets
  neither. The map should apply them or list them as ignored.

## Measuring it

- The hardware isn't byte-stable. Across five runs on the board its VMAF
  moved by up to 0.6 points, where ours repeated to within about 0.2.
- `scripts/regress.py` draws `--hw auto` in a quarter of its cells and checks
  only "encodes, decodes, frame count matches", not determinism.
- A raw `.264` from the hardware has no VUI timing. Align frames by index
  when scoring, or the PSNR filter pairs the wrong frames.
- `tools/vtprobe` prints the hardware's supported H.264 properties. Check the
  map against it on a new macOS.
