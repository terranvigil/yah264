# The orchestrator: per-shot convex-hull encoding above the engines

The orchestrator runs the per-shot search that the encoders don't. It encodes
each shot of a title at several operating points, measures them, and assembles
a ladder from the best point per shot. It will live in its own repository,
drive yah264, yah265 and yaav1 through [engine-interface.md](engine-interface.md),
and take this page as its founding design note.
[shot-based-plan.md](shot-based-plan.md) explains why this work sits above
the encoder and not inside it.

## Where it stands

The design is settled and a prototype has run against yah264's hooks. The
repository doesn't exist yet. yah264 has every hook the orchestrator needs,
and yah265 and yaav1 have none of them yet.

The prototype covered the first two phases below. On the five-shot 720p
sequence, joining one cell's segments reproduced that cell's encode byte for
byte. The measured hull, at one resolution over five CRF points, read in
BD-VMAF-NEG:

| window | against flat CRF | against `--shot-crf` |
|---|---:|---:|
| the five-shot 720p sequence | -11.7% | -3.4% |
| a 3000-frame Big Buck Bunny window, 36 shots in 38 GOPs | -2.2% | -4.6% |

Both clear the phase gate of 3% over `--shot-crf`. On the Big Buck Bunny
window `--shot-crf` itself loses 2.6% to flat CRF, because the pre-scan
misreads the animation's pans, so the hull's margin over it is larger there.

## Decisions

The owner made these:

1. The repository is `enc-orc`, and it's codec-agnostic.
2. It's written in Go, to match the service template the other projects use.
3. It optimizes VMAF-NEG and records plain VMAF alongside. Where a change
   needs plain VMAF optimized to move that number, it does that too, so the
   figure other people compare against improves as well.
4. Fixed-resolution rungs ship first. Per-shot resolution switching is queued
   and will be offered as an option.
5. yah265 and yaav1 get wired in after the loop is proven on yah264.

Two more came out of the prototype. The unit of allocation is the engine's
GOP, and each unit's objective is weighted by its frame count. Both are
explained under the stages below.

## What it does

The orchestrator cuts the title into shots. It encodes the whole title
several times, once per operating point, meaning a resolution and a quality
setting, with keyframes on the cuts and each shot written as its own segment.
It scores every shot of every encode against the source with one metric, and
for each shot keeps only the points on the upper convex hull of its bits and
quality.

From there it picks one point per shot so the whole title sits at one
quality-per-bit slope. Sweeping that slope traces the title's best possible
curve, and the ladder's rungs are read off it. Each rung is assembled by
joining the chosen segments. The engine's determinism guarantee says those are
the same bytes a standalone encode of each shot would produce. That holds
exactly at one thread and is best effort with more, so with threads an
assembled segment is a valid drop-in, though not always identical bytes.

That's Netflix's dynamic optimizer without the catalog-scale machinery. What
it adds is the engine's help: shot boundaries and per-frame decisions come
from the encoder, so the search never runs its own analysis pass.

## The stages

Five stages, each a command with files in and files out, so any stage can be
rerun or replaced:

```
analyze  ->  probe  ->  score  ->  hull+select  ->  assemble (+package)
```

**analyze.** Ask the engine for the shot table (`--cut-split --shot-table`),
take the source's frame count, resolution and frame rate, and write
`shots.json`. A user plan can add forced boundaries, such as chapter marks or
ad slots. The orchestrator does no scene detection of its own, because the
boundaries have to be the ones the engine cuts on or the segments won't line
up. One `--scan-out` file serves every probe of the title through `--scan-in`.

The unit of allocation is the GOP the engine produces, which isn't always a
shot. The engine splits a shot longer than the keyframe interval, and the
first probe's frame stats say exactly where. The prototype learned this on Big
Buck Bunny, where two shots ran past 250 frames and 36 shots became 38 GOPs.
Reading the unit map back from the stats, and checking that every cell agrees,
is part of the probe stage. To make the unit the shot itself, set the
keyframe interval at or above the longest shot.

**probe.** Each grid cell, a resolution and a quality setting, is one
full-title encode. The source is downscaled with Lanczos-3, the same scaler
for every engine, and encoded with `--plan` putting a keyframe at every shot
start, `--gop-threads` pinned, `--segment-out` and `--frame-stats`. Cells are
independent, so they run in parallel across machines. Within a cell the
engine's own GOP workers run the shots in parallel. Each cell produces one
segment per shot and one stats line per frame.

Whole-title encodes beat per-shot jobs here. The same bits come out, the
engine schedules shots across its workers better than an outside queue would,
and the rate control inside a cell sees the whole title, which matters for
the ABR-style cells planned for later.

**score.** Decode each segment, upscale it to the source resolution with
Lanczos-3, and run libvmaf against that shot's source frames, keeping the
per-frame scores. A shot's quality is its mean VMAF-NEG by default. The
per-frame scores are kept so selection can apply a floor, such as the 10th
percentile or the minimum. Bits per shot come from the segment file. Each
cell writes one JSON file per shot with bits, frames, mean, 10th percentile,
minimum and the per-frame vector.

VMAF-NEG, the v0.6.1neg model, is the metric because the encoders are judged on it, and because
plain VMAF rewards sharpening that a per-shot search would learn to exploit.
PSNR is recorded as a sanity column and never optimized.

**hull and select.** Each unit keeps the upper convex hull of its cell
points, with bits on one axis and frames times quality on the other, found by
a monotone chain. Points below the hull are dropped. Since the title's
quality is the frame-weighted mean, each unit's objective is
`frames * quality - lambda * bits`. For a given slope lambda every unit takes
the hull point that maximizes it, and the sum over units is one point on the
title's curve.

The frame weighting matters. Without it units of unequal length get the wrong
points, which is how the prototype's Big Buck Bunny assembly first came out
4.8% worse than flat CRF.

Sweeping lambda over the hulls' segment slopes yields the title curve. A rung
is the assembly at the lambda that meets its target, whether that's a
bitrate, a mean quality or a quality floor. Two optional constraints fit here.
A per-shot minimum quality keeps any shot from falling below a set score. A
limit on resolution changes between neighboring shots adds a switching cost to
the objective, which is then solved by dynamic programming over the shot
sequence instead of shot by shot.

**assemble and package.** For each rung, join the chosen segments in shot
order. Each segment carries its own parameter sets, so a rung whose shots come
from different cells is a valid stream as long as the container and the player
accept a parameter-set change at a keyframe. DASH and HLS segments do, and so
do most players on a raw stream. A rung at one resolution throughout needs
nothing more. Packaging wraps rungs as CMAF, fMP4 or TS segments aligned on
the shots, with a manifest. It's the last stage and the only one that knows
about players.

## What the engine provides and what the orchestrator owns

The engine provides what [engine-interface.md](engine-interface.md) lists: the
shot table, a plan of forced keyframes and QP offsets, per-frame forces,
pinned threads with the determinism guarantee, per-frame stats and segment
output. The orchestrator owns the scaler, the metric, the hull, the selection,
the assembly, the packaging and every trial encode. It never asks the engine
to switch resolution inside an encode, never asks it for a quality score, and
never tunes an engine default.

A small adapter per engine maps the contract onto that engine's CLI. yah264's
flags are the shared spelling.

## The grid and its cost

The starting grid for an AVC title from a 1080p source:

| axis | values |
|---|---|
| resolution | 1080, 720, 540, 360 |
| CRF | 20, 24, 28, 32, 36 |

That makes 20 cells, each one normal encode of the title plus one VMAF pass.
At yah264's medium preset on the development Mac, a 1080p cell of a 12-minute
title takes roughly 3 to 6 minutes, with the smaller sizes faster. The
exhaustive grid therefore fits in under two hours on one machine before it's
split across more. RCN-Hull's statistics suggest a third of the cells can go
before any prediction. The prediction phase aims for 60% fewer.

The floor to beat is `--shot-crf`, which costs one encode. So the measured
hull has to be worth its extra encodes: at least 3% BD-VMAF-NEG over
`--shot-crf` at fixed resolution, plus a further gain from resolution
switching at the low rungs, on long-form content.

## Phases

- **Scaffold.** Done in the prototype. It covers the stage commands, the
  yah264 adapter, `shots.json` and the cell layout, smoke-tested on the Big
  Buck Bunny window. Gate: a rung assembled from one cell is byte-identical to
  that cell's own encode.
- **Measured hull at one resolution.** Done in the prototype, with the results
  above. A full CRF grid at 720p, the hull, the lambda sweep and assembly.
  Gate: BD-VMAF-NEG against flat CRF and against `--shot-crf` at matched bits.
  It ships at 3% or more over `--shot-crf` and is killed below 1%. The
  prototype ran it on two windows. The rest of the long-form corpus and the
  other two multi-shot sequences are still to run.
- **Resolution.** Add 540 and 360 cells, per-shot resolution in the selection,
  and the switching-cost search. Gate: the low rungs, 0.3 to 1 Mbit/s from a
  720p source, against the fixed-resolution ladder, plus a visual check that
  resolution switches at cuts aren't visible.
- **Pruning and prediction.** RCN-Hull-style statistics over our cells, then a
  predictor built on the engine's shot features. Gate: at least 50% fewer
  encodes at under 1% BD loss against the full grid.
- **Packaging.** CMAF output and manifests, and a player check on the
  parameter-set change at shot-aligned segments.
- **Later.** ABR-style cells, where the engine's rate control spans the title
  inside a cell. Audio-aware merging of very short shots. yah265 and yaav1,
  once they implement the contract.

## How it's built

Go, with ffmpeg built with libzimg for scaling and decoding, libvmaf for
scoring, and the engines run as subprocesses. Nothing in the orchestrator is
hot. The encodes are. The repository holds one adapter per engine, the stage
commands behind a single CLI, tests of the hull and selection on synthetic
point clouds and of the segment-join identity, and pointers to the corpus in
[corpus-sources.md](corpus-sources.md) rather than the media itself. It's
licensed GPL-2.0-or-later, like the encoders.

## Prior art

**Netflix's dynamic optimizer**, published in 2018, encodes each shot at several resolutions and
QPs, scores VMAF on decodes upscaled to the display size, builds each shot's
rate-quality hull, and picks one point per shot at a common slope. That's a
Lagrangian choice: maximize quality minus lambda times bits, shot by shot.
Sweeping lambda builds the title's curve, and rungs are read off it. Netflix
reports 17% bitrate savings on VMAF against fixed-QP per-title encoding, and
more against two-pass VBR. It settles the method. The cost is dozens of
encodes per shot.

**RCN-Hull**, by Paul, Norkin and Bovik in IEEE TIP 2024, gives the
measurement protocol we adopt. Decodes are upscaled to the source size with
Lanczos-3 and scored with VMAF, and the hull is built with quickhull over the
bitrate and quality points. The grid is:

| axis | values |
|---|---|
| resolution | seven, from 1080p down to 216p |
| QP | nine, 16 to 48 in steps of 4 |

Its statistics show most grid cells never reach any hull, with 50 of 63
candidates pruned for VMAF. A learned predictor gets within 0.26% BD-rate of
the exhaustive hull with 54% less encode time. So a sparse grid loses little,
and the hull is predictable from a shot's pixels.

**The 2025 ACM TOMM benchmark**, on 300 UHD shots across AVC, HEVC and VVC,
finds handcrafted features good enough for a usable predictor, with learned
models winning by a small margin. The prediction stage can start simple.

**av1an** is the open-source per-scene orchestrator most people run. It
detects scenes, encodes chunks in parallel, probes CRFs per chunk to hit a
VMAF target, and joins the result. It settles the ergonomics: a chunk queue,
encoder plugins and a probe loop. It also shows the gap. It targets a quality
per chunk instead of a common slope, has no hull and no resolution dimension,
and runs its own scene detection outside the encoder.

**yah264's own shot work** supplies the shot table, cut-aligned GOPs,
per-shot CRF and the hooks. `--shot-crf` is the floor the orchestrator has to
beat, since it's what a prediction without measurement already buys. On ten
real titles at 1080p it reads -1.0% median BD-VMAF-NEG against plain CRF, and
on the short multi-shot sequences 1 to 4% over the default.
