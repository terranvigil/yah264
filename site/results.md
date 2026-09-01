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
different questions, so both are here.

Each CRF run is solved per clip so it lands on the same bitrate as x264.

**CRF, matched achieved bitrate**:

| goal | configuration | median | max | VMAF | size | status |
|---|---|--:|--:|--:|--:|---|
| 1 | pure C, single-threaded | **0.95x** | 1.04x | +0.00 | +0.1% | all metrics pass |
| 2 | pure C, multi-threaded | **0.85x** | 0.96x | −0.08 | +0.2% | all metrics pass |
| 3 | as-shipped SIMD, multi-threaded | 0.96x | 1.14x | −0.07 | +0.2% | all metrics pass 33% of runs |

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

Goals 1 and 2 have passed all four metrics. Goal 3 is the one to read carefully.
It cleared every metric on the run above, which is one of three runs at the
current rate tolerance: the other two read 1.00x and 1.02x,
and on both the worst-clip metric sat exactly at its 1.15x bar rather than under
it. Every one of those gaps is smaller than the machine's own spread. One
favorable draw is a draw, so goal 3 stays open until the result repeats across
separate sessions.

Goal 3 is also scored on CIF and 720p only. On larger frames the same
as-shipped tier reads 1.28x to 1.47x across four clips, three of them 1080p and
one 720p.
That is not a contradiction, it is the scope. The small clips flatter us: on
foreman_cif we keep around 8.5 cores busy where x264 keeps 5.9, so we finish
first by using more of the machine, not by doing the work faster. A larger frame
has enough work in it for both encoders to consume every core, that advantage goes
away, and what is left is the per-unit efficiency gap on its own.

## How to reproduce them

Both encoders run as libraries inside one ffmpeg process, off the same demuxer,
each choosing its own thread count the way it would for any caller. Neither is
told how many threads to use, because pinning one number pins it for both
encoders and they do not want the same one.

That matters more than it sounds. Measured through two CLIs instead, each
encoder's own Y4M reader falls inside the timing, and goal 3's median rises by
roughly a tenth. The difference is almost entirely in the short clips, which is
what a fixed per-process cost looks like as the encode gets faster.

The pure-C rows compare auto-vectorized C on both sides. x264 suppresses
vectorization in its own build, because its C sits behind hand-written assembly
and exists as a fallback, so the reference for those rows is built with that
flag stripped. Leaving it in reads goal 2 around 0.73x, worth roughly a third of
the goal, which measures a compiler flag instead of an encoder.

The in-process wrapper lives in an ffmpeg fork rather than in this repository,
for licence reasons. `make parity-status-crf` runs the two-CLI version and needs
no fork.

## Quality across the rate range

Quality is full-frame VMAF in NEG mode at matched bitrates. The result is
band-specific, so it is quoted as a map.

Low bitrate is where yah264 does best. At the deep band it leads x264 on 9 of 10
clips, with a median BD-rate advantage around 12%. Two cautions come with that,
both from the same working notes: ducks and park_joy cannot reach the regime at
all, and the per-clip noise floor is around 1.2 BD-rate points, which three of
the nine leads do not clear. The lead narrows further up the range and does not
survive at the high band. Anyone quoting a
compression result from this project should say which band it came from.

BD-rate is also half a comparison. It says nothing about what the quality cost
in time, which is why every quality claim here is published next to a speed row.

## Against other encoders

| encoder | pure-C 1-thread | pure-C MT | SIMD MT | quality (VMAF) | size | notes |
|---|--:|--:|--:|--:|--:|---|
| yah264 | **0.95x** | **0.85x** | 1.01x | −0.07 | +0.2% | this repo |
| x264 | 1.00x | 1.00x | 1.00x | ref | ref | the reference point |
| openh264 | 0.18x | 0.76x | 0.78x | −9.3 | +0.9% | not a matched point |

The first two speed columns are the same measurements as the goal table above.
The SIMD MT column is an earlier read of it, taken before the rate match was
tightened, so 1.01x is not one of the three runs counted there. Both are draws
from the same ~0.07 spread, which is wider than the margin goal 3 turns on.

openh264 cannot be read against the other two. It exposes no quality knob
through ffmpeg, only a bitrate, so there is nothing to solve onto a common
operating point. At a matched bitrate it sits more than 9 VMAF below both, which
is most of why it looks fast. Its comparable number is BD-rate, which normalizes
for quality, and there it costs +63.7%. It also has no B-frames.

GPU-vendor encoders are fixed-function silicon with different quality and
latency trade-offs, so they stay out of scope here.

## The corpus

The clips in the goal tables are natural video, three CIF and three 720p. The
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
