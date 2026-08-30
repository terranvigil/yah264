---
title: Results - yah264
description: The goal boards, the quality maps, the corpus, and how each number reproduces.
---

# Results

The board figures come from the two dated board files named below. Several of
the surrounding numbers, marked where they appear, are recorded in the working
notes rather than in a board file, and they are due a re-measure before anyone
quotes them. The measurements are a snapshot from August 2026 on Apple Silicon,
and there is no x86-64 SIMD tier yet. Speed ratios move a few points between machines and
between runs on the same machine, so treat the third decimal as noise.

## Reading a board

A board scores yah264 against x264 as a ratio of wall time at a matched
operating point. Parity is one and lower is faster. A ratio travels between
machines far better than a frame rate does, which is why the goals are set on
one, but it is not invariant: expect a few points of movement on hardware that
is not this Apple Silicon box.

Four metrics decide a goal, and all four are read at the same matched point:

| metric | bar |
|---|---|
| median speed | 1.00x or faster |
| worst-clip speed | under 1.15x |
| quality | within 0.5 [VMAF](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion) |
| compression | within 1.0% size |

Two cautions come with reading one. A rate-matched board cannot decide a quality
margin smaller than its own rate tolerance, so a difference in the second
decimal of VMAF is below the instrument. The machine's own run-to-run spread on
a median is wider than some of the margins in the table below, which is why a
single favourable run does not close a goal.

## Why the rate control mode changes the answer

The headline board runs at CRF, solved per clip onto a matched achieved
bitrate. That choice does most of the work in the table, so the other reading
is published beside it.

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

The second table is not a speed measurement, and that is why the goals are not
set against it. At a matched bitrate the two encoders are not doing the same
work, so the ratio largely reports which one spent fewer bits. Across the six clips the
ABR speed ratio correlates 0.87 with the bits each encoder spent, a figure from
the working notes rather than a board file.
x264's rate control undershoots high-motion CIF and overshoots ducks, so a
matched-bitrate ratio scores whichever encoder happened to spend less. The size
column says the same thing at the summary level, sitting almost three times its
bar. Matching the rate is not the same as matching the work.

## The three speed goals

Goals 1 and 2 have passed all four metrics. Goal 3 is the one to read carefully.
It cleared every metric on the run above, which is one of the three runs the
board has had at its current rate tolerance: the other two read 1.00x and 1.02x,
and on both the worst-clip metric sat exactly at its 1.15x bar rather than under
it. Every one of those gaps is smaller than the machine's own spread. One
favourable draw is a draw, so goal 3 stays open until the board repeats across
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

The first two speed columns are the same board as the goal table above. The
SIMD MT column carries an earlier read of it, taken before the board's rate
match was tightened, which is why 1.01x is not one of the three runs counted
there. It and the 0.96x are draws from the same ~0.07 run-to-run spread, and
that spread is wider than the margin goal 3 turns on, which is why the goal is
still open rather than met.

openh264's row is a separate measurement again, and cannot be read against
the other two. It exposes no quality knob
through ffmpeg, only a bitrate, so there is nothing to solve onto a common
operating point. Boarded at a matched bitrate it sits more than 9 VMAF below
both other encoders, and that deficit is most of why it looks fast. Its
comparable number is BD-rate, which normalises for quality, and there it costs
+63.7%. It also has no B-frames.

GPU-vendor encoders are fixed-function silicon driven through vendor APIs, with
different quality and latency trade-offs, so they stay out of scope for the
software boards.

## The corpus

The board clips are natural video, three CIF and three 720p. The wider corpus
adds animation and high-motion sport, and every clip is recorded with its source
and licence.

The training set and the gate set are separate, and the gate set is test-only.
Any fitted coefficient is calibrated on the training half and reported on the
gate half, because a number fitted and reported on the same clips is not a
measurement.

## Off-corpus content

Content outside the board behaves differently enough that the honest summary is
a range.

Two clips both fairly called animation land 33 BD-rate points apart. On 3D CGI
yah264 runs 25% ahead of x264 at 1.34x the time; held at equal quality instead,
`veryfast` reaches x264 medium at 1.07x for a fifth fewer bits. On hand-drawn 2D
it runs 8% behind.

No animation result here is claimed as a single number.
`docs/animation-content.md` has the measurements and the preset-ladder rows. It is also why a preset-for-preset speed comparison stops
meaning much once content leaves the set the presets were tuned on.
