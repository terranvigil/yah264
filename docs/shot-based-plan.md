# Shot-based encoding: the codec/orchestrator split, and what to build first

Four questions: can yah264 support shot-based / context-aware encoding, does
that require container demuxing, what have other projects done, and would support
inside the codec be new. This doc answers each, then gives a staged plan where
every stage has a measurable gate and a kill threshold.

The conclusion up front: the QP axis of shot-based encoding belongs in the codec
and is nearly built already, the resolution/ladder axis belongs in an
orchestration layer, and the most valuable thing yah264 can ship is the hook set
that makes it the best engine under such a layer. That is a narrower ambition
than "Dynamic Optimizer inside the encoder", and it is the right one.

## What the encoder already has

- **Lookahead.** Lowres lookahead with mb-tree, adaptive scene cut
  (`scenecut_decide`), `--scenecut`/`--min-keyint`, decoupled `--sync-lookahead`.
- **Rate control.** CQP, CRF, ABR, CBR, capped VBR, 2-pass
  (`docs/rc-mode-matrix.md`).
- **A shot detector.** `yah264_scan_idr_frames` pre-scans the whole input and
  returns exactly the IDRs the real encode's lookahead would place, replaying
  `la_finalize`'s state machine with a flash guard. Exact by construction,
  parallel, analysis-only.
- **Per-shot encoder instances as the normal threaded path.** `gop_worker` opens
  a fresh encoder per GOP, and `Y264_CUT_SPLIT=1` moves those boundaries onto
  real cuts via the pre-scan.
- **A quality harness.** `scripts/bdcompare.py --vmaf` with the VMAF-NEG gate,
  corpus calibrated to the VMAF 88-94 band where deltas mean something.

Two structural facts matter most. The threaded CLI encodes GOPs on independent
instances, which is the skeleton of per-shot encoding, built for threading rather
than quality. And the per-GOP parameter mechanism already half exists: `gop_k[]`
gives each GOP its own `frame_threads`, so a per-GOP `rc` override is a small
extension of an existing pattern, a bitstream-affecting one this time.

Three rate-control defects bear on the design:

- **2-pass distributes bits badly**: right total, wrong per-frame split, 3.7 to
  34.4 VMAF worse than 1-pass ABR at matched rate. Nothing below builds on the
  2-pass stats path.
- **CRF's frame-level complexity term is dropped when mb-tree is on**
  (`rc_set_qp_crf`): the default path codes `qp = crf + (1-qcomp)*13.5 = crf +
  5.4`, flat, on the behaviour-matched theory that mb-tree's per-MB offsets carry
  the complexity. They only partly do: equal-CRF size vs x264 swings foreman
  -10%, bus +9%, park_joy +44%, sintel -55%. Shot-level modulation is the natural
  place to put the missing term, because mb-tree handles within-shot and nothing
  handles across-shot.
- **Capped VBR underflows on 5 of 6 clips.** Irrelevant to a quality-targeted
  ladder, blocking for any rung that promises a delivery cap. Noted in sequencing
  below.

## Survey: what shipped, and where

**Netflix Dynamic Optimizer** is the reference system and it is an orchestrator:
split at shots, trial-encode each shot at many (resolution, QP) points, VMAF
everything, run a constant-slope trellis across shots, stitch. Published numbers:
17% bitrate vs fixed QP on VMAF, over 50% vs two-pass VBR, roughly 28% vs a fixed
ladder for x264. Those include resolution switching and dozens of trial encodes
per shot, so they are a ceiling for any one-pass scheme, never a target. Their
generation history is the sharpest lesson in the survey: gen 2 encoded each shot
as its own distributed job and it hurt, with ~20 frames of rate-control warmup per
shot, 4-8% IDR overhead on short shots, and ~900 tasks per hour of content
overwhelming their messaging layer. Gen 3 collates shots into ~3-minute chunks:
shots stay the unit of quality decisions, chunks become the unit of work.
yah264's GOP-worker model is already the gen-3 shape inside one machine.

**av1an** is the open-source proof that the orchestration layer is commodity:
scene-split chunking, per-scene target-VMAF CRF search (probe encodes plus
interpolation), parallel workers, running x264/x265/SVT-AV1/rav1e. It ships today
and it is free. Any "we built an orchestrator" story competes with it. A "we are
the best engine under one" story benefits from it.

**In-encoder fragments** exist across the field, which settles the novelty
question:

- x264: `--zones` (manual per-frame-range ratefactor overrides) and, more deeply,
  CRF+qcomp+mb-tree itself. Constant-rate-factor coding IS in-codec
  constant-slope bit allocation at frame granularity, and qcomp is its exponent.
  The idea has been inside encoders for twenty years.
- x265: `--zonefile` (per-frame reconfig), `--scenecut-aware-qp` and
  `--hist-scenecut` (scene-aware QP masking around cuts).
- SVT-AV1: `--enable-variance-boost`, `--luminance-qp-bias` (per-block and
  per-scene-brightness quality adaptation), per-frame QP via qpfile, which is what
  av1an's per-frame target quality drives.
- AWS MediaConvert QVBR: a shipped, fully in-encoder content-adaptive quality
  mode. Bitmovin and Mux sell per-title as a service, Mux's "instant per-title"
  predicting the ladder from features in milliseconds with no trial encodes.

**Academic state of the art** has moved from "search the hull" to "predict the
hull". RCN-Hull (IEEE TIP 2024) predicts hull membership from shot content and
loses 0.26% BD-rate with 62% fewer encodes. The 2025 ACM TOMM benchmark (Telili
et al., 300 UHD shots, AVC/HEVC/VVC) finds handcrafted features plus ExtraTrees
around 88% accuracy, beating the deep models, with about 1.8% quality loss vs
exhaustive search. VCA-style DCT-energy features are the cheap input that makes
this work at ingest speed. ARTEMIS (NSDI 2024) does the live-streaming variant
with no trial encodes at all.

**So is "support through the codec" new?** In kind, no: per-shot QP adaptation
inside the encoder is x264 zones done automatically, and CRF/qcomp/mb-tree
already perform the allocation at finer granularity than shots. Nobody should
claim the mechanism as novel. What genuinely does not ship anywhere today, and is
within this codebase's reach:

1. **Exact shot-granular allocation in one pass with a measured BD gate.** x264's
   closest is 2-pass qcomp over frames. A shot-level term driven by the pre-scan,
   gated on BD-VMAF-NEG, is a real (if modest) differentiator.
2. **A determinism contract for shots.** Because each GOP is coded by an
   independent instance, "re-encode shot k with changed parameters,
   byte-identical everywhere else" is structurally true here and false in every
   mainstream encoder. Orchestrators, and NLE-style partial re-encode, can build
   on that.
3. **One-invocation ladder with shared analysis.** Multi-rung output where the
   lookahead/shot analysis runs once. For H.264 this is a compute win, never a
   quality win, since rungs are separate streams regardless: AVC has no
   reference-picture resampling, so per-shot resolution inside one stream needs a
   new SPS+IDR and players that tolerate it, and segmented delivery makes the
   question moot anyway.

Point 3 is why the resolution axis of the convex hull cannot move into the codec
in any strong sense for this codec. The hull's QP axis can, and the prediction
models that pick hull points can consume our lookahead features.

## The split

What the codec should own, because it is better at it than any wrapper:

- Shot detection and per-shot features (exists, exact, parallel).
- Per-shot quality allocation at fixed resolution: the shot-level complexity term
  CRF currently lacks.
- Per-shot tool adaptation. This is quietly the strongest in-codec argument,
  because several measured wins are parked waiting for it: psy-trellis wins on
  grain and loses on clean (park_joy -0.29/-0.49/-0.64% VMAF-NEG at strengths
  0.6/0.9/1.2, ducks -0.35/-0.75%), shipped as manual `--tune grain` with
  shot-adaptive gating as the real ship path. mb-tree strength 2.5 halves the
  static-content gap but costs motion clips. dct-decimate thresholds want the same
  switch. No orchestrator can reach these knobs mid-title.
- Analysis export and override hooks (the hull-assist set), plus the determinism
  contract above.

What the orchestration layer should own, because moving it inside would re-learn
known lessons:

- Resolution ladders and hull construction. Measurement lives outside the encoder
  (VMAF wants decode, upscale to source, score), resolution switching is a
  packaging fact, and cross-codec comparisons are the point of a hull.
- VMAF-in-the-loop CRF search (av1an's target quality). An encoder that scored its
  own output with the metric it is gated on would be grading its own homework.
- Distribution across machines, stitching, manifests, containers.

`docs/plan.md` names "hull-assist mode ... making yah264 the best engine under a
Netflix-style orchestration layer", and that is the strategic center of this doc.
The in-codec maximalist version, emitting a whole ABR ladder per shot in one
pass, survives as a research stage at the end, behind ground truth it needs
anyway.

## Containers

Do not build demuxing into yah264, in the library or the CLI. Real shot-based
inputs are compressed mezzanines, so "mp4 support" is actually "decoding
support", which means adopting libavformat/libavcodec. A from-scratch encoder
should not absorb that. The precedents agree: SVT-AV1 takes y4m/raw, x264 treats
lavf input as an optional build, av1an uses ffmpeg/vapoursynth for all IO.

Consumption model instead:

- Humans: `ffmpeg -i src.mp4 -f yuv4mpegpipe - | yah264 --input-y4m -`. The CLI
  reads y4m from stdin and the threaded path streams frames through a bounded
  window (`docs/streaming-input-plan.md`), so a pipe loses nothing.
- Orchestrators: the library API plus the S4 hooks below. Orchestrators speak
  "run a CLI per chunk" natively.
- Output: Annex-B now, `ffmpeg -c copy` for mp4. CMAF/fMP4 segment output stays a
  later product feature, not part of this work.

**NOTE:** the cut-aware split (`Y264_CUT_SPLIT`) still reads the whole input,
because `yah264_scan_idr_frames` needs every frame at once and its boundaries
are the dispatcher's input. Long-form with cut-aware boundaries therefore wants
the two-decode shape: a streaming analysis pass (lowres costs and the shot table,
discarding planes as it goes), then a streaming encode pass consuming the plan.
That is the same shape an orchestrator uses, one more reason the layers should
meet at a stats file rather than a shared address space.

## Staged plan

Ship value at each stage, gate everything, kill what misses. All gates are
BD-VMAF-NEG via `scripts/bdcompare.py --vmaf --no-cache`, 5-point sweeps, against
**our own** flat-CRF encode. The x264 equal-CRF divergence makes cross-encoder
CRF comparisons meaningless (`docs/rc-mode-matrix.md`).

**S0. A corpus that can see shots** (prereq, no encoder code, effort S). The
calibrated corpus is six single-scene 6-second windows: shot-based gains are
definitionally ~0 on it. Build the multi-shot set: concatenations of the
calibrated clips (mixed complexity by construction, known cut positions), plus
sintel and 2-3 other multi-shot 720p sequences, targets picked into the VMAF
88-94 band the same way `docs/rc-mode-matrix.md` did. Add a per-shot floor report
to the harness: min per-shot VMAF delta vs the flat encode, because a mode that
wins the mean by starving one shot is a regression viewers will see. Nothing else
proceeds until S0 exists, or every later number is noise.

**S1. Promote the pre-scan to an analysis API** (library, effort S).
`yah264_scan_idr_frames` already computes lowres intra and inter costs for every
frame and throws them away. Return them: a shot table (first/last frame,
mean/peak `icost`, `pcost/icost` ratio, luma/variance aggregates) behind a new
`yah264_analyze` or a widened scan call. Promote `Y264_CUT_SPLIT` to a real CLI
flag. It changes GOP boundaries, hence the bitstream, so it stays opt-in rather
than default. Gate: the shot table on the S0 corpus matches the cuts the encode
actually places. It replays the same arithmetic, so this is a test, not a tuning
exercise.

**S2. Per-shot CRF** (CLI + one small library hook, effort S-M). The smallest
version that can beat flat CRF. Compute per-shot offsets from S1 features, x264's
own curve applied at shot granularity:

    qp_shot = crf + 6*(1-qcomp) * log2(C_shot / C_title)

with qcomp starting at 0.6, clamped to +-4 QP, shots shorter than min-shot
merged, and a shot spanning several GOPs sharing one offset. Implementation is a
per-GOP `rc.rf` in the GOP job, the `gop_k[]` pattern extended to rate control.
The plan derives from input and params only, so determinism at fixed thread count
holds by the same argument as the cut split. mb-tree keeps working within each
shot, and this supplies the across-shot term the flat CRF path deliberately
dropped. Gate: ship at >= 2% BD-VMAF-NEG on the S0 multi-shot set with no shot
below the floor, kill below 1%. Expect low single digits: qcomp and mb-tree
already capture much of what shot allocation buys, and the published 17%/28%
numbers had resolution switching and trial encodes in them.

**NOTE:** CRF is quantised to whole QP, so offsets land in integer steps and
bitrates move in ~12% jumps. Acceptable at a +-4 clamp. Fractional QP is its own
item if S2 ships.

**S3. Shot-class tool gating** (encoder, effort S per tool). Classify shots from
S1 features (grainy, flat/animation, high-motion, static) and gate the parked
wins: psy-trellis strength on grain shots, mb-tree strength on static,
dct-decimate thresholds, AQ strength. Each tool gates separately under the BD
discipline; never bundle. The grain case is the most likely first win since the
strength curve is already measured and only the switch is missing.

**S4. Hull-assist hooks** (API, effort S, high strategic value). The wedge that
makes yah264 the preferred engine under av1an-class orchestrators:

- Shot-table export: JSON from the CLI, struct from the library, including
  per-shot complexity so an orchestrator can seed hull prediction without its own
  analysis pass.
- Per-shot overrides in one encode: forced IDR at given display indices plus
  per-frame-range rf/QP offsets. An x264-zones equivalent, driven by a plan file.
- The determinism contract, held by a test: re-encoding shot k alone with the
  same parameters reproduces its byte range in the full encode. The GOP-instance
  model makes this true today, and a test makes it a promise an orchestrator can
  build convex-hull probing and partial re-encode on.

Then wire a real orchestrator to it, one that already has a monotone-chain hull,
a Bjontegaard fit, and a lambda-searched constant-slope allocator. Reusing those
against yah264's hooks is weeks cheaper than rebuilding any of it in C, and it
exercises the hooks the way a real customer would.

**S5. Measured-hull ladder** (tool layer, effort M, decision point). Sparse
per-shot grids (3 resolutions x 4 QPs at a fast preset), PCHIP interpolation,
per-shot hulls, a trellis across shots, final encodes only at chosen points. Run
it as the orchestrator invoking yah264 first. Bring a `tools/ladder/` into this
repo only if shared analysis shows a compute win worth owning. This stage owns
the first scaler the project needs (Lanczos-3 for output-quality downscale, plus
decode-upscale-VMAF methodology matching Netflix/RCN-Hull). Gate: within 1% BD of
the exhaustive hull with >= 50% fewer encodes, and an ffmpeg round-trip packaging
check that rungs switch cleanly at shot-aligned segments.

**S6. Predicted hull, the research stage** (effort L). Train a GBDT on S5's
ground truth to predict hull membership from S1 features, export the trees as C
arrays, keep a convexity check after encoding so a misprediction costs one extra
encode rather than a broken ladder. Falsifiable target from the literature:
within ~1% BD of the measured hull at >= 60% fewer encodes (RCN-Hull reached
0.26% at 62%, and the TOMM benchmark says handcrafted features suffice). Only
start once S5 has generated the truth data, and only if S5's economics say trial
encodes are the cost that matters.

## Sequencing against the open defects

- The 2-pass allocation defect stays out of this plan's dependency chain: per-shot
  CRF is open-loop by construction. Whoever fixes 2-pass should know the shot
  allocator may BE the fix, allocating across shots by constant slope and letting
  CRF machinery handle frames within a shot, but the first shipping stage must not
  wait on it.
- The flat CRF default path is a feature here rather than a blocker: S2 adds the
  missing across-shot term at the granularity where it belongs, without touching
  the behaviour-matched within-window design.
- Capped VBR's underflow only matters when a ladder rung promises a delivery cap
  (S5's packaging checks). Fix it on its own track before then, along with the
  per-GOP VBV buffer reset the fresh-instance model causes, which a capped
  per-shot stream would inherit.

## What would falsify this plan

- S2 lands under 1% on the multi-shot corpus: per-shot QP is not worth its
  complexity, title-level CRF plus mb-tree already suffice. The differentiator
  then collapses to S3 (tool gating) and S4 (hooks), and that narrower product,
  best engine under an orchestrator, is still worth shipping.
- S3's shot classes fail to reproduce the parked psy-trellis/mb-tree wins: the
  content-adaptive story loses its best evidence, and S3 stops at whatever
  individual tools did gate.
- S5 shows the sparse measured hull is already cheap enough (short shots, fast
  presets, few points): S6's prediction saves compute nobody is paying, and it
  should not be built. This is a real possibility at short-form scale.
- Packaging reality (SSAI ad points, strict segment ladders) forces re-encodes
  that erase one-invocation savings: the ladder work stays outside the codec
  permanently and the codec keeps only S4.

## Sources

- Netflix: [Dynamic Optimizer](https://netflixtechblog.com/dynamic-optimizer-a-perceptual-video-encoding-optimization-framework-e19f1e3a277f),
  [Optimized shot-based encodes](https://netflixtechblog.com/optimized-shot-based-encodes-now-streaming-4b9464204830)
- [RCN-Hull, IEEE TIP 2024](https://arxiv.org/abs/2206.04877); [Telili et al., ACM TOMM 2025 hull-prediction benchmark](https://dl.acm.org/doi/full/10.1145/3723006);
  [VCA](https://github.com/cd-athena/VCA); [ARTEMIS, NSDI 2024](https://www.usenix.org/system/files/nsdi24-tashtarian.pdf)
- [av1an target quality](https://rust-av.github.io/Av1an/Features/TargetQuality.html);
  [SVT-AV1 parameters (variance boost, luminance-qp-bias)](https://github.com/psy-ex/svt-av1-psy/blob/master/Docs/Parameters.md);
  x265 `--zonefile` / `--scenecut-aware-qp` (x265 CLI docs); x264 `--zones`
- [Mux instant per-title](https://www.mux.com/blog/instant-per-title-encoding);
  [Bitmovin split-and-stitch](https://bitmovin.com/blog/split-and-stitch-encoding/)
- In-repo: `docs/rc-mode-matrix.md`, `docs/streaming-input-plan.md`
