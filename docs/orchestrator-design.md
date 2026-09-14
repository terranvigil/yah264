# The orchestrator: per-shot convex-hull encoding above the engines

Status 2026-09-06: design, first draft, for the owner's review. **P0 and
P1 measured the same day on a prototype against the yah264 hooks**: on the
five-shot 720p sequence, concatenating one cell's segments reproduced that
cell's encode byte for byte (P0), and the measured hull at one resolution
over five CRF cells read -11.7% BD-VMAF-NEG against flat CRF and -3.4%
against per-shot CRF (`--shot-crf`); on the 36-shot Big Buck Bunny window
(38 GOP units) -2.2% against flat and -4.6% against per-shot CRF, which on
that window is itself 2.6% WORSE than flat (its pre-scan misreads the
animation's pans). Both clear the P1 ship gate (>= 3% over per-shot CRF).
Two lessons went straight into section 3: the unit is the engine's GOP,
and the per-unit objective must be weighted by frame count (unweighted, the
Big Buck Bunny assembly read +4.8% against flat). Decided by
the owner: the orchestrator lives in its own repository, is codec-agnostic,
and drives yah264, yah265 and yaav1 through the engine interface
(docs/engine-interface.md). The yah264 side of that interface shipped in
PR #143 and is itself still awaiting the owner's review, so everything here
that leans on it is provisional. This document is meant to move to the new
repository as its founding design note.

## 1. What it does, in one paragraph

Cut the title into shots. Encode the whole title several times, once per
operating point (a resolution and a quality setting), with keyframes on the
cuts and each shot written as its own segment. Score every shot of every
encode against the source with one metric. For each shot, keep only the
points on the upper convex hull of its (bits, quality) cloud. Pick one point
per shot so that the whole title sits at one quality-per-bit slope, sweep the
slope to trace the title's best possible curve, and read the ladder's rungs
off that curve. Assemble each rung by concatenating the chosen segments,
which the engine's determinism contract guarantees are the same bytes a
standalone re-encode of that shot would produce.

That is Netflix's dynamic optimizer, minus their catalog-scale machinery
and plus one thing they do not have: the engine hands over shot boundaries
and per-frame decisions, so the search never runs its own analysis pass.

## 2. Prior art, and what each one settles

**Netflix dynamic optimizer (2018).** Shot-level encodes at multiple
resolutions and QPs, VMAF on decodes upscaled to the display resolution,
per-shot rate-quality hull, one point per shot at a common slope (a
Lagrangian choice: maximise quality minus lambda times bits, per shot),
lambda swept to build the title's curve, rungs read off it. Reported 17%
bitrate savings against fixed-QP per-title encoding on VMAF and more against
two-pass VBR. Settles the method; the cost is dozens of encodes per shot.

**RCN-Hull (Paul, Norkin, Bovik, IEEE TIP 2024).** The measurement
protocol we adopt: 7 resolutions from 1080p to 216p, 9 QPs from 16 to 48 in
steps of 4, decodes upscaled to the source resolution with Lanczos-3, VMAF,
hull by quickhull over the (bitrate, quality) points. Their statistics show
most grid cells never land on any hull (63 candidates pruned to 50 for VMAF)
and a learned predictor gets within 0.26% BD-rate of the exhaustive hull at
54% less encode time. Settles that a sparse grid loses little and that the
hull is predictable from the shot's pixels.

**The 2025 ACM TOMM benchmark** (300 UHD shots, AVC/HEVC/VVC): handcrafted
features suffice for a usable predictor; the learned models win by a small
margin. Settles that the prediction stage can start simple.

**Av1an.** The open-source per-scene orchestrator most people actually run:
scene detection, chunk-parallel encodes, a target-quality mode that probes
CRFs per chunk to hit a VMAF target, concatenation. Settles the ergonomics
(a chunk queue, encoder plugins, a probe loop) and shows the gap: it targets
a quality per chunk rather than a common slope, has no hull and no
resolution dimension, and re-runs scene detection outside the encoder.

**Our own S1-S4.** The shot table and cut-aligned GOPs; per-shot CRF from a
closed-form curve (worth 6-11% against cut-split on the multi-shot
sequences, and 1-4% over the now-default across-shot term); the hooks. The
per-shot CRF result is the floor the orchestrator has to beat: it is what a
prediction without measurement already buys.

## 3. Architecture

Five stages, each a command with files in and files out, so any stage can
be re-run or replaced:

```
analyze  ->  probe  ->  score  ->  hull+select  ->  assemble (+package)
```

**analyze.** Ask the engine for the shot table (`--shot-table`), take the
source's frame count, resolution and frame rate, and write `shots.json`.
Optionally merge a user plan (forced boundaries at chapter marks, ad slots).
No orchestrator-side scene detection: the boundaries must be the ones the
engine will cut on, or the segments will not line up.

The unit of allocation is the GOP the engine actually produces, not the
shot: a shot longer than the keyframe interval is split by the engine, and
the per-frame stats of the first probe say exactly where. The prototype
learned this on the Big Buck Bunny window (two shots over 250 frames, 38
GOPs for 36 shots): reading the unit map back from the stats, and checking
every cell agrees, is part of the probe stage. Set the interval at or above
the longest shot when the unit is meant to be the shot itself.

**probe.** For each cell of the grid (resolution r, quality setting q),
one full-title encode: the source downscaled to r (Lanczos-3, the same
scaler for every engine), `--plan` with an `idr` at every shot start,
`--gop-threads K` pinned, `--segment-out` and `--frame-stats`. Cells are
independent, so they run in parallel across machines; within a cell the
engine's own GOP workers run the shots in parallel. Output: per cell, one
segment per shot and one stats line per frame.

Why whole-title encodes rather than per-shot jobs: the same bits come out
(determinism contract), the engine schedules the shots across its workers
better than an external queue would, and the rate control inside a cell sees
the whole title, which matters for ABR-style cells later.

**score.** Decode each segment, upscale to the source resolution with
Lanczos-3, run libvmaf against the source frames of that shot, keep the
per-frame scores. Per-shot quality = mean VMAF-NEG by default, with the
per-frame vector kept so a floor (p10, min) can be applied at selection.
Bits per shot come from the segment file. Output: `cells/<r>_<q>/shot_k.json`
with (bits, frames, mean, p10, min, per-frame).

The metric is VMAF-NEG (v0.6.1neg) because it is what the encoders are
gated on and because plain VMAF rewards sharpening that a per-shot search
would otherwise learn to exploit. PSNR is recorded alongside as a sanity
column, never optimised.

**hull + select.** Per unit, the upper convex hull of its cell points in
(bits, frames x quality), by monotone chain; points below the hull are
discarded. The title's quality is the frame-weighted mean, so the per-unit
objective is `frames * quality - lambda * bits`: for a slope lambda each
unit takes the hull point that maximises it, and the sum over units is one
title-level point. (Unweighted, units of unequal length get the wrong points;
the prototype measured +4.8% against flat that way and -2.2% weighted.)
Sweeping lambda over the hulls' segment slopes yields the title curve. A
rung is the assembly at the lambda that meets its target (a bitrate, a mean
quality, or a quality floor). Optional constraints at this stage: a per-shot
minimum quality (a shot may not fall below X), a maximum resolution change
between neighbouring shots (a switching cost added to the objective, solved
by dynamic programming over the shot sequence rather than per shot).

**assemble.** For each rung, concatenate the chosen segments in shot order.
Each segment carries its own parameter sets, so a rung whose shots come from
different cells is a valid stream as long as the container and the player
accept a parameter-set change at an IDR, which is the case for DASH/HLS
segments and for most players on a raw stream. A rung at one resolution
throughout needs nothing more. **package** wraps rungs as CMAF/fMP4 or TS
segments aligned on the shots, with a manifest; it is the last stage and
the only one that knows about players.

## 4. What the engine must provide, and what the orchestrator owns

From docs/engine-interface.md: shot table, plan (forced IDR, QP offset),
pinned threads with the determinism contract, per-frame stats, segment
output. The orchestrator owns: the scaler, the metric, the hull, the
selection, the assembly, the packaging, and every trial encode. It never
asks the engine to switch resolution inside an encode, never asks it for a
quality score, and never tunes an engine default.

Per-engine adapters map the contract onto each CLI. yah264's flags are the
shared spelling; yah265 and yaav1 need the same five things before they
can be driven (neither has them yet, 2026-09-06).

## 5. The grid, and its cost

Start (AVC, 1080p source): resolutions {1080, 720, 540, 360}, quality
settings {CRF 20, 24, 28, 32, 36}: 20 cells. Each cell is one normal encode
of the title plus one VMAF pass. For a 12-minute title at 24 fps that is 20
encodes of 17,000 frames; at the current yah264 medium speed on the dev box
roughly 3-6 minutes per 1080p cell and less for the smaller ones, so an
exhaustive grid is under two hours on one machine and embarrassingly
parallel beyond it. RCN-Hull's pruning statistics say a third of the cells
can be dropped before any prediction; the S6 predictor targets 60% fewer.

The floor to beat is per-shot CRF (S2), which costs one encode. The gate for
the measured hull is therefore "worth its extra encodes": at least 3%
BD-VMAF-NEG over per-shot CRF at fixed resolution, and a further gain from
resolution switching at the low rungs, on the long-form windows.

## 6. Phases and gates

- **P0, scaffold.** The repository, the stage commands, the yah264 adapter,
  `shots.json` and the cell layout, a smoke run on the 3000-frame Big Buck
  Bunny window. Gate: the assembled single-cell rung is byte-identical to
  the cell's own encode (the concatenation is a no-op when every shot comes
  from one cell).
- **P1, measured hull at one resolution.** Full CRF grid at 720p on the two
  long-form windows and the three multi-shot sequences; hull, lambda sweep,
  assembly. Gate: BD-VMAF-NEG against flat CRF and against `--shot-crf` at
  matched bits; ship value if >= 3% over per-shot CRF, kill below 1%. This
  also measures the real prize of per-shot allocation on this codec, which
  S2 could only estimate.
- **P2, resolution.** Add 540 and 360 cells; per-shot resolution in the
  selection; a switching-cost DP. Gate: the low rungs (the 0.3-1 Mbit/s
  band at 720p source) against the fixed-resolution ladder, and a visual
  check that resolution switches at cuts are not seen.
- **P3, pruning and prediction.** RCN-Hull-style statistics over our cells,
  then a predictor from the engine's shot features (S6). Gate: encode count
  down >= 50% at < 1% BD loss against the full grid.
- **P4, packaging.** CMAF output and manifests, a player check on the
  parameter-set change at shot-aligned segments.
- **Later.** ABR-style cells (the engine's rate control across the title
  inside a cell), audio-aware shot merging for very short shots, and the
  two sibling engines once their contracts land.

## 7. Repository shape (proposal)

```
<repo>/
  README.md, LICENSE (GPL-2.0-or-later, as the encoders)
  docs/design.md            (this document)
  orch/                     (Python package)
    engine/  yah264.py next265.py nextav1.py   (adapters: contract -> CLI)
    analyze.py probe.py score.py hull.py select.py assemble.py package.py
    cli.py                  (`orch <stage> ...`)
  tests/                    (hull and selection on synthetic clouds; the
                             concatenation identity)
  corpus/                   (pointers only: docs/corpus-sources.md URLs)
```

Python with numpy for the maths, ffmpeg (with libzimg) for scaling and
decoding, libvmaf for scoring, subprocess to the engines. Nothing in it is
hot; the encodes are.

## 8. Owner decisions (2026-09-06)

1. **Repository**: `enc-orc`.
2. **Language**: Go (matches the service template under ~/src).
3. **Metric**: optimize VMAF-NEG (the strict one) and record plain VMAF
   alongside; where a change needs plain VMAF optimized to move that number,
   do that too, so the industry-comparable figure improves as well.
4. **Resolution**: ship fixed-resolution rungs first, but queue the
   per-shot resolution-switching work and expose it as an option.
5. **Siblings**: wire yah265 and yaav1 in after the loop is proven on
   yah264.

## 9. Open questions (resolved above)

1. The repository's name and home.
2. Python for the orchestrator (proposed), or Go to match the service
   template in ~/src.
3. VMAF-NEG as the optimised metric (proposed), or plain VMAF for
   comparability with Netflix's published numbers (record both, optimise one).
4. Per-shot resolution changes inside one rung (Netflix does this; it needs
   the packaging stage and player checks) versus fixed-resolution rungs with
   per-shot quality only, as the first shipped form.
5. Whether the two sibling encoders should implement the contract now, or
   after P1 proves the loop on yah264.
