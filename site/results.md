---
title: Results - yah264
description: The goal tables, the quality maps, the corpus, and how each number reproduces.
---

# Results

The measurements are current as of 2026-09-04 on Apple Silicon. Speed ratios move
a few points between runs on the same machine.

## Reading the tables

The tables show how fast yah264 is compared to x264. Both encode the same clip
at the same quality. 1.0 means a tie. Lower is faster. These numbers come from
Apple Silicon. On other hardware they may shift a little.

The goal is made up of four metrics:

| metric | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | under 1.15x |
| quality | within 0.5 [VMAF](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion) |
| compression | within 1.0% size |

Two things to keep in mind when reading these tables. First, the rates are only
matched approximately, so tiny quality differences - hundredths of a VMAF
point - are smaller than the test can measure. Second, the same machine gives
slightly different times on every run and that jitter is bigger than some of
the gaps. This is why we run the measurement multiple times for confirmation.

## Why the rate control mode changes the answer

You can ask an encoder for a quality level and let the bitrate land where it
lands. That's CRF, and it's the main table. Or you can ask for a bitrate and let
quality land where it lands. That's ABR, in the second table. The two answer
different questions so both are here.

The main table is CRF. It's solved per clip so it lands on the same bitrate
as x264.

**CRF, matched achieved bitrate**, ten clips (three CIF, four 720p, three 1080p), 2026-09-04 (after a lookahead cache fix on top of the 09-03 changes):

| goal | configuration | median | max | VMAF | size | status |
|---|---|--:|--:|--:|--:|---|
| 1 | pure C, single-threaded | **0.95x** | 1.14x | +0.26 | −0.1% | all metrics pass |
| 2 | pure C, multi-threaded | **0.83x** | 1.06x | +0.20 | +0.1% | all metrics pass |
| 3 | as-shipped SIMD, multi-threaded | **0.95x** | 1.15x | +0.22 | +0.1% | worst clip at the bar |

The worst clip on every row is the same one, low-bitrate 1080p (sunflower at
1.5 Mbit/s), with shields at 2.3 Mbit/s next; the high-bitrate 1080p rows are
the fastest cells on the board. The pure C rows now meet all four metrics;
the shipped build's worst clip sits exactly at the 1.15x bar, which is inside
the board's own day-to-day spread. Until 2026-09-02 this table was taken on
six clips with no 1080p in it and read 0.95x / 0.85x / 0.96x. The full
per-clip tables are kept in our local board notes.

**ABR, matched achieved bitrate**, same clips, 2026-09-04 (after the staircase's reference-B row gate was allowed under rate control):

| goal | configuration | median | max | VMAF | size |
|---|---|--:|--:|--:|--:|
| 1 | pure C, single-threaded | 0.94x | 1.15x | +0.03 | +0.1% |
| 2 | pure C, multi-threaded | 0.94x | 1.08x | +1.11 | −0.1% |
| 3 | as-shipped SIMD, multi-threaded | 1.11x | 1.24x | +1.09 | −0.1% |

Here x264's target is solved so it lands on the bitrate we achieved, which is
what the size column shows. No goal is set against this table, but it is now a
speed reading rather than a bit-spending contest. Single-threaded, ABR costs us
nothing over CRF. Multi-threaded it now costs about 0.1 to 0.15 on the ratio,
down from 0.3 to 0.4: the rate-control decide was allowed to run one burst
ahead (2026-09-03), and then a staircase device that lets the next frame start
against a reference still being coded, which rate control had been refusing,
was allowed under that lag (2026-09-04). That second change alone took the
multi-threaded rows from 1.06x and 1.26x to 0.94x and 1.11x. Until 2026-09-03
this table handed both encoders the same target and let the sizes differ by
about 3%, which made it unreadable as a speed number.

**By resolution class**, median ratio on the same two boards (three CIF, four
720p, three 1080p clips):

| goal | CRF CIF | CRF 720p | CRF 1080p | ABR CIF | ABR 720p | ABR 1080p |
|---|--:|--:|--:|--:|--:|--:|
| 1 | 0.92x | 0.96x | 1.07x | 0.90x | 0.96x | 1.08x |
| 2 | 0.78x | 0.86x | 0.99x | 0.93x | 0.91x | 1.06x |
| 3 | 0.91x | 0.99x | 1.08x | 1.11x | 1.07x | 1.18x |

Resolution is not what orders these rows. Bitrate is: the slow cells are the
low-bitrate HD ones and the high-bitrate 1080p cells are the fastest on the
board. The 1080p column reads high because two of its three clips are
low-bitrate.

## The three speed goals

Goals 1 and 2 have passed all four metrics. Goal 3 has been passing but not
consistently, so we are leaving it open.

## How to reproduce them

Both encoders run inside a single ffmpeg process and each decides how many CPU
cores to use.

We don't set the core count by hand, even though that sounds like the right
move for repeatable results. In reality, each encoder picks its thread count
differently, based on its own implementation and optimizations.

The pure-C rows need one adjustment to be fair. x264 turns off auto-vectorization
in its own C, because that C is only a fallback behind its hand-written assembly.
We strip that flag so the compiler treats both sides the same. Leave it in and
goal 2 reads about 0.73x, which measures a compiler flag rather than an encoder.

The wrapper that runs both encoders in one process lives in an ffmpeg fork, not
in this repository, for licence reasons. `make parity-status-crf` runs the
two-CLI version instead and needs no fork.

## Quality across the rate range

Quality is scored with VMAF, which predicts how good video subjectively looks to
people. We compare both encoders at the same file size.

yah264 does best at low bitrates: it beats x264 on 9 of 10 clips there, by around
12% on the standard compression score. That score is an average across a range of
settings. The lead fades as bitrate rises and is gone at the top. Any
compression claim from this project should say which bitrate range it came from.

And file size is half the story. It says nothing about encoding time. Every
quality number here sits next to a speed number.

## The hardware mode

`--hw videotoolbox` encodes through the Mac's fixed-function H.264 engine
with yah264's options mapped onto it and our scene-cut driving its keyframes.
The stream is the hardware's, not ours, so nothing above applies to it; this
is its own row, measured on the same ten clips at the same bitrates,
2026-09-04, on an Apple M-series machine. VMAF is the NEG variant.

| clip | kbit/s target | yah264 wall | yah264 CPU | hardware wall | hardware CPU | yah264 VMAF | hardware VMAF |
|---|--:|--:|--:|--:|--:|--:|--:|
| foreman_cif | 400 | 0.11 s | 0.81 s | 0.16 s | 0.04 s | 93.1 | 93.3 |
| bus_cif | 400 | 0.11 | 0.68 | 0.15 | 0.04 | 92.2 | 86.5 |
| stefan_cif | 400 | 0.07 | 0.39 | 0.12 | 0.03 | 89.1 | 85.5 |
| ducks_720p | 25000 | 1.46 | 15.25 | 0.53 | 0.25 | 91.6 | 83.7 |
| park_joy_720p | 12000 | 1.16 | 11.85 | 0.43 | 0.27 | 90.2 | 83.5 |
| samsung_720p | 1200 | 0.39 | 4.01 | 0.29 | 0.12 | 89.6 | 89.2 |
| shields_720p | 2200 | 0.69 | 7.25 | 0.42 | 0.27 | 94.4 | 89.0 |
| sunflower_1080p | 1500 | 0.68 | 8.41 | 0.42 | 0.28 | 89.8 | 87.5 |
| pedestrian_1080p | 2800 | 0.79 | 10.45 | 0.41 | 0.22 | 86.1 | 85.1 |
| riverbed_1080p | 12500 | 1.34 | 16.44 | 0.42 | 0.23 | 82.8 | 83.0 |

Six-second windows, our encoder at auto threads. The hardware uses 30 to 70
times less CPU and is two to three times faster in wall time on HD (slower
on CIF, where the session's setup is most of the run), and it lands 1 to 8
VMAF points below our encoder at the same bitrate on eight of the ten clips,
level on the other two. It is not byte-stable run to run. Sizes are within
a few percent of target on both sides; the full per-clip figures are in our
local records.

## Against other encoders

| encoder | pure-C 1-thread | pure-C MT | SIMD MT | quality (VMAF) | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | 0.95x | **0.83x** | 0.95x | +0.22 | +0.1% | this repo, ten-clip board, 2026-09-04 |
| x264 | 1.00x | 1.00x | 1.00x | ref | ref | the reference point |
| openh264 | 0.18x | 0.76x | 0.78x | −9.3 | +0.9% | not a matched point |

The yah264 row is the same ten-clip board as the goal table above but a separate
run of it, which is why it reads a few hundredths apart; the goal figures are the
ones in that table. The openh264 row is an older six-clip measurement and is not
on the same board at all.

openh264 cannot be read against the other two. It exposes no quality knob
through ffmpeg, only a bitrate, so there is nothing to solve onto a common
operating point. At a matched bitrate it sits more than 9 VMAF below both, which
is most of why it looks fast. Its comparable number is BD-rate, which normalizes
for quality, and there it costs +63.7%. It also has no B-frames.

GPU-vendor encoders are fixed-function silicon with different quality and
latency trade-offs, so they stay out of scope here.

## The corpus

The clips in the goal tables are natural video, three CIF, four 720p and three 1080p. The
wider corpus adds animation and high-motion sport, and every clip is recorded with its source
and licence.

The training set and the gate set are separate, and the gate set is test-only.
Any fitted coefficient is calibrated on the training half and reported on the
gate half, because a number fitted and reported on the same clips is not a
measurement.

## Off-corpus content

Content outside that set behaves differently enough that the only summary that
holds is a range.

Two clips both fairly called animation sit 33 BD-rate points apart. On 3D CGI
yah264 runs 25% ahead of x264 at 1.34x the time. Held at equal quality instead,
`veryfast` reaches x264 medium at 1.07x for a fifth fewer bits. On hand-drawn 2D
it runs 8% behind.

No animation result here is claimed as a single number.
`docs/animation-content.md` has the measurements and the preset-ladder rows. It is also why a preset-for-preset speed comparison stops
meaning much once content leaves the set the presets were tuned on.
