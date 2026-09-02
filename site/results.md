---
title: Results - yah264
description: The goal tables, the quality maps, the corpus, and how each number reproduces.
---

# Results

The measurements are current as of Aug 2026 on Apple Silicon. Speed ratios move
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

**CRF, matched achieved bitrate**, ten clips (three CIF, four 720p, three 1080p), 2026-09-02 (second board of the day, after the P-search deletions):

| goal | configuration | median | max | VMAF | size | status |
|---|---|--:|--:|--:|--:|---|
| 1 | pure C, single-threaded | **0.97x** | 1.16x | +0.32 | +0.2% | worst clip 0.01 over the bar |
| 2 | pure C, multi-threaded | **0.84x** | 1.12x | +0.22 | +0.2% | all metrics pass |
| 3 | as-shipped SIMD, multi-threaded | **0.95x** | 1.18x | +0.24 | +0.2% | worst clip 0.03 over the bar |

The worst two clips on every row are the same pair, low-bitrate HD (shields at
2.3 Mbit/s and sunflower at 1.5 Mbit/s); the high-bitrate 1080p rows are the
fastest cells on the board. Until 2026-09-02 this table was taken on six clips with no 1080p in
it and read 0.95x / 0.85x / 0.96x; the board itself is the change, not the
encoder. The full per-clip tables are in
[docs/board-2026-09-02.md](https://github.com/terranvigil/yah264/blob/main/docs/board-2026-09-02.md).

**ABR, same bitrate on both sides**, same clips:

| goal | configuration | median | max | VMAF | size |
|---|---|--:|--:|--:|--:|
| 1 | pure C, single-threaded | 1.02x | 1.20x | −0.27 | +2.8% |
| 2 | pure C, multi-threaded | 1.19x | 1.31x | +0.30 | +2.9% |
| 3 | as-shipped SIMD, multi-threaded | 1.40x | 1.52x | +0.31 | +2.9% |

The second table is not really a speed test, so no goal is set against it. Hand
both encoders the same bitrate and they still won't do the same amount of work,
and the one doing less finishes sooner. The size column gives it away: our files
come out about 2.9% bigger than x264's, almost three times what the goal allows.
Same bitrate does not mean same job.

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

## Against other encoders

| encoder | pure-C 1-thread | pure-C MT | SIMD MT | quality (VMAF) | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | 1.01x | **0.85x** | 0.97x | +0.23 | +0.1% | this repo, ten-clip board |
| x264 | 1.00x | 1.00x | 1.00x | ref | ref | the reference point |
| openh264 | 0.18x | 0.76x | 0.78x | −9.3 | +0.9% | not a matched point |

The yah264 row is the goal table above; the openh264 row is an older six-clip
measurement and is not on the same board.

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
