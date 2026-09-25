# Shot-based encoding: which layer owns what, and what to build first

This document answers four questions. Can yah264 support shot-based encoding?
Does that require container demuxing? What have other projects already
shipped? Would doing it inside the codec be new? Then it gives the stages,
each with a measurable gate and a kill threshold.

The conclusion, up front. The QP axis of shot-based encoding belongs in the
codec, the resolution and ladder axis belongs in an orchestration layer, and
and what yah264 can ship that is worth the most to a caller is the hook set
that makes it the best engine under such a layer. That is a narrower ambition than a dynamic
optimizer inside the encoder. We think it is the right one, and the stages
below are ordered on that belief.

**Where this stands.** The first five stages have shipped, except that S3
closed at a measured ceiling and shipped no feature. The last two have not
started, and the orchestrator that would run them lives in its own repository.

One caveat applies to every gain claimed below. The multi-shot sequences are
concatenations of six-second clips. On the only long-form window anybody has
measured, a 3000-frame Big Buck Bunny window, per-shot CRF loses 2.6% against
flat.

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

Three rate-control questions bear on the design. Two of them have since been
answered and the third is open.

- **Two-pass allocates fine.** It beats one-pass ABR at matched rate
  ([rc-mode-matrix.md](rc-mode-matrix.md)). *Retracted: an earlier version of
  this list had two-pass far behind one-pass ABR, taken against an allocator
  since replaced, with the sign backwards.* Nothing below builds on the
  two-pass stats path either way.
- **CRF's frame-level complexity term is back.** `rc_set_qp_crf` used to drop
  the term whenever mb-tree was on, leaving the base QP flat, on the theory
  that mb-tree's per-MB offsets account for the complexity. They only partly
  do.
  `Y264_CRF_CPLX` put the term back and ships on by default, and the CRF
  scale is now aligned: the median absolute offset against x264 stays under
  one CRF point at every rung, from 0.39 to 0.76, and the per-clip table is
  in [rate-control.md](rate-control.md). What that alignment does not fix is
  the spread, which runs 2.3 to 3.1 points across clips, because the residual
  changes sign between clips and no remapping can shrink it. What is left for
  shot-level modulation is the across-shot part, which mb-tree still does not
  handle and which `--shot-crf` adds.
- **Capped VBR still underflows.** The compliance gate reads 29 of its 36
  cells clean. The seven failures are encoder-side underflows: the VBV fill
  clamps at zero instead of preventing the excursion. The B-frame HRD item
  now in flight will name them. This is irrelevant to a quality-targeted
  ladder and blocking for any rung that promises a delivery cap, so it comes
  back in the sequencing section.

## What already shipped elsewhere

**Netflix Dynamic Optimizer** is the reference system and it is an orchestrator:
split at shots, trial-encode each shot at many (resolution, QP) points, VMAF
everything, run a constant-slope trellis across shots, stitch. Published
numbers, every one measured on their own catalogue against a named anchor: 17%
bitrate on VMAF against fixed-QP encoding, over 50% against two-pass VBR,
roughly 28% against a fixed ladder for x264. Those include resolution switching
and dozens of trial encodes per shot, so they are a ceiling for any one-pass
scheme, never a target, and they are not ours to quote without the anchor. Their
generation history is the sharpest lesson here. Their gen 2 encoded each shot
as its own distributed job and it hurt: about 20 frames of rate-control warmup
per shot, 4 to 8% IDR overhead on short shots, and roughly 900 tasks per hour
of content overwhelming their messaging layer. Gen 3 collates shots into
three-minute chunks. Shots stay the unit of quality decisions and chunks
become the unit of work.
yah264's GOP-worker model is already that arrangement inside one machine.

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

**Academic state of the art** has moved from searching the hull to predicting
it. RCN-Hull predicts hull membership from shot content and loses 0.26%
BD-rate at 62% fewer encodes. The ACM TOMM benchmark of Telili et al. covers
300 UHD shots across AVC, HEVC and VVC, and finds that handcrafted features
with ExtraTrees beat the deep models at around 88% accuracy, for about 1.8%
quality loss against exhaustive search.

VCA-style DCT-energy features are the cheap input that makes any of this work
at ingest speed. ARTEMIS does the live-streaming variant with no trial encodes
at all.

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
   byte-identical everywhere else" is structurally true here, guaranteed at
   `--threads 1` and best effort with more threads (owner, 2026-09-25). We know of no mainstream encoder that offers it, which
   is not the same as having checked them all. Orchestrators, and NLE-style
   partial re-encode, can build on that.
3. **One-invocation ladder with shared analysis.** Multi-rung output where the
   lookahead/shot analysis runs once. For H.264 this is a compute win, never a
   quality win, since rungs are separate streams regardless: AVC has no
   reference-picture resampling, so per-shot resolution inside one stream needs a
   new SPS+IDR and players that tolerate it, and segmented delivery makes the
   question moot anyway.

Point 3 is why the resolution axis of the convex hull cannot move into the codec
in any strong sense for this codec. The hull's QP axis can, and the prediction
models that pick hull points can consume our lookahead features.

## Which layer owns what

What the codec should own, because it is better at it than any wrapper:

- Shot detection and per-shot features (exists, exact, parallel).
- Per-shot quality allocation at fixed resolution: the shot-level complexity term
  CRF currently lacks.
- Per-shot tool adaptation. This is quietly the strongest in-codec argument,
  because several measured wins are parked waiting for it. Psy-trellis wins on
  grain and loses on clean content, its strength curve measured on park_joy and
  ducks, and it ships as the manual `--tune grain` until a shot-adaptive switch
  can set it. mb-tree at strength 2.5 halves the static-content gap and costs
  motion clips. dct-decimate thresholds want the same switch. No orchestrator
  can reach any of these knobs mid-title.
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

## Why there is no demuxer here

Do not build demuxing into yah264, in the library or the CLI. Real shot-based
inputs are compressed mezzanines, so "mp4 support" is actually "decoding
support". That means adopting libavformat and libavcodec, and a from-scratch
encoder should not absorb either. Other projects do the same. SVT-AV1 takes
y4m and raw input, x264 treats lavf input as an optional build, and av1an uses
ffmpeg or vapoursynth for all of its IO.

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
two passes over the file. First a streaming analysis pass that keeps the lowres
costs and the shot table and discards the planes as it goes, then a streaming
encode pass consuming the plan. An orchestrator does exactly that, which is one
more reason the two layers should meet at a stats file instead of sharing an
address space.

## The stages and where each one stands

Every stage ships something, every stage is measured, and a stage that misses
its number is killed. All gates are
BD-VMAF-NEG via `scripts/bdcompare.py --vmaf --no-cache`, 5-point sweeps, against
**our own** flat-CRF encode. The x264 equal-CRF divergence makes cross-encoder
CRF comparisons meaningless (`docs/rc-mode-matrix.md`).

**S0. A corpus that can see shots** (prereq, no encoder code, effort S).
**Shipped.** The calibrated corpus is ten single-scene six-second windows from
`scripts/parity-clips.sh`, so a shot-based gain on it is zero by construction.
The multi-shot set concatenates those clips into sequences of mixed complexity
with known cut positions, targets picked into the VMAF 88-94 band the same way
`docs/rc-mode-matrix.md` did. `scripts/make_multishot.py` builds them and
`scripts/multishot_bd.py` scores them. The harness also reports a per-shot
floor, the minimum per-shot VMAF delta against the flat encode, because a mode
that wins the mean by starving one shot is a regression viewers will see.

The set that shipped is three sequences of five shots each, at CIF, 720p and
1080p. That is also its limit, because five six-second shots is not a title. The long-form windows
used later came from elsewhere, and they are where the per-shot gain stops
holding.

**S1. Promote the pre-scan to an analysis API** (library, effort S).
**Shipped.** `yah264_scan_idr_frames` computed lowres intra and inter costs for
every frame and threw them away. It now returns them as a shot table: first and
last frame, mean and peak intra cost, the inter-over-intra ratio. `--shot-table`
prints it as JSON on stderr and `--cut-split` is a real CLI flag. Both stay
opt-in, because `--cut-split` moves GOP boundaries and therefore changes the
bitstream. The gate was that the shot table matches the cuts the encode
actually places, which it does by construction: it replays the same arithmetic,
so this was a test and never a tuning exercise.

**S2. Per-shot CRF** (CLI + one small library hook, effort S-M). **Shipped as
`--shot-crf`.** The smallest version that can beat flat CRF. Per-shot offsets
come from S1's features through the standard qcomp-exponent form, applied at
shot granularity instead of per frame:

    qp_shot = crf + 6*(1-qcomp) * log2(C_shot / C_title)

with qcomp starting at 0.6, clamped to +-4 QP, shots shorter than min-shot
merged, and a shot spanning several GOPs sharing one offset. Implementation is a
per-GOP `rc.rf` in the GOP job, the `gop_k[]` pattern extended to rate control.
The plan derives from input and params only, so determinism at fixed thread count
holds by the same argument as the cut split. mb-tree keeps working within each
shot, and this supplies the across-shot term the flat CRF path deliberately
dropped. The gate was 2% BD-VMAF-NEG on the S0 multi-shot set with no shot
below the floor, and a kill below 1%. We expected low single digits, because
qcomp and mb-tree already capture much of what shot allocation buys, and the
published Netflix figures had resolution switching and trial encodes in them.

**What S1 and S2 are, against the Netflix method.** No trial encodes and no
hull. One pass, the lookahead's costs, a closed-form offset per shot, one
resolution. The dynamic optimizer's savings come from a search this does not
run, and what the two share is only the direction of the allocation. The
stages that need the search are S4 and after: predicted hull, per-shot
resolution, slope matching across the title.

The cost that "no trial encodes" does not cover is the pre-scan. It reads the
whole input before the encode starts, so this is one encode pass over two
reads of the file, and it needs a seekable file. On a pipe `--cut-split` and
everything above it do nothing. The two facts belong together in any claim
made about the mode: nothing is encoded twice, and the input is read twice.

**S2 result: shipped opt-in as `--shot-crf`, and it found a bug in the flat
path.** It clears its gate against `--cut-split` alone on all three S0
sequences. The x264 control is what reframed that number. The flat CRF path
trailed x264 on multi-shot content while leading on the same clips taken
singly, so the shot arm was catching up and not pulling ahead. The mechanism
was the AQ frame-mean term running at the within-frame strength of 0.4 against
the reference's 1.0. Non-reference B frames got no such term at all.

The library fix is `Y264_AQ_DC` on the CRF base QP, and it ships on by
default. Every reading here is BD-VMAF-NEG on the three sequences, CIF then
720p then 1080p.

| arm | anchor | CIF | 720p | 1080p |
|---|---|---:|---:|---:|
| `--shot-crf` | `--cut-split` alone | -10.8% | -7.6% | -5.9% |
| the `Y264_AQ_DC` fix, no pre-scan | the old default | -8.2% | -4.1% | -6.2% |

Single-shot clips moved by a band median of +0.3% under that fix, which is
the price it charges where there is no across-shot allocation to get right.
Over the new default the shot arm keeps 1 to 4% on CIF and 720p, and the
whole-file title reference is the whole of the difference.

**The long-window caveat, which applies to every gain on this page.** On a
3000-frame Big Buck Bunny window `--shot-crf` reads +2.6% against flat, so it
loses. The pre-scan's inter cost misreads that animation's pans and the
offsets go the wrong way. Five concatenated six-second clips are not a title,
and nobody has measured what the mode is worth over a feature-length film.
Until that measurement exists, the claim `--shot-crf` can support is narrow:
it wins on short multi-shot sequences, and it has one long-form
counterexample.

**NOTE:** CRF is quantised to whole QP, so offsets arrive in integer steps and
bitrates move in jumps of about 12%. That is acceptable at a four-QP clamp.
Fractional QP is its own item.

**S3. Shot-class tool gating** (encoder, effort S per tool). **Closed by
measurement.** The idea was to classify shots from S1's features as grainy,
flat, high-motion or static, then switch the parked wins per class:
psy-trellis strength on grain shots, mb-tree strength on static ones,
dct-decimate thresholds, AQ strength. Each tool would gate separately under
the BD discipline, never bundled. Grain looked like the first win, since its
strength curve was already measured and only the switch was missing.

**S3 result: closed.** A per-shot oracle on long-form windows put a *perfect*
selector at about 1%. A perfect selector is the ceiling, so no real classifier
can beat it, and 1% does not pay for the machinery. It reopens only if a tool
with a steeper content curve than the ones measured turns up.

**S4. Hull-assist hooks** (API, effort S, high strategic value). **Shipped.**
The wedge that makes yah264 the preferred engine under av1an-class
orchestrators. Three pieces, plus the thing they are for:

- Shot-table export, JSON from the CLI and a struct from the library,
  including per-shot complexity so an orchestrator can seed hull prediction
  without running its own analysis pass.
- Per-shot overrides in one encode: a forced IDR at given display indices,
  plus per-frame-range QP offsets, driven by a plan file. This is the
  equivalent of the manual zones other encoders expose, issued from a plan.
- The determinism contract, held by a test. Re-encoding shot k alone, with the
  same parameters and the same pinned frame-thread count, reproduces its byte
  range in the full encode. The pinning is part of the contract. The
  GOP-instance model already made this true, and a test turns it into a
  promise an orchestrator can build convex-hull probing and partial re-encode
  on.

These hooks exist to have a real orchestrator wired to them, one that already
has a monotone-chain hull, a Bjontegaard fit and a lambda-searched
constant-slope allocator. Reusing those against yah264's hooks is weeks
cheaper than rebuilding any of it in C, and it exercises the hooks the way a
real customer would.

**S4 result: built.** `--plan` takes the zones, with
`yah264_encoder_set_zones` behind it in the library. `--gop-threads` pins the
frame threads, `--segment-out` writes a file per GOP, and `--frame-stats`
emits one JSON line per coded frame in coding order, with
`yah264_encoder_frame_stats` as its library form. The determinism contract is
a test, `scripts/shot_determinism.sh`, and it reads 5 of 5 GOPs byte-identical
alone at a pinned frame-thread count. The contract itself is written
codec-agnostically in docs/engine-interface.md, so the sibling encoders can
implement it. The orchestrator lives in a separate repository, which the owner
decided.

**A prototype ran against these hooks, and it beat `--shot-crf` on both
windows.** It measured a hull over five CRF cells at one resolution, and
BD-VMAF-NEG came out like this:

| window | against flat CRF | against `--shot-crf` |
|---|---:|---:|
| the five-shot 720p sequence | -11.7% | -3.4% |
| the Big Buck Bunny window | -2.2% | -4.6% |

On that second window `--shot-crf` is itself the losing arm. That is why the
hull's margin over it is the larger of the two. Concatenating one cell's
segments reproduced that cell's encode byte for byte, so the determinism
contract did its job. Two lessons went into the orchestrator's design. The
unit is the engine's GOP. The per-unit objective has to be weighted by frame
count, because unweighted the Big Buck Bunny assembly read +4.8% against flat.
docs/orchestrator-design.md has the rest.

**S5. Measured-hull ladder** (tool layer, effort M, decision point). **Not
started.** Sparse per-shot grids, three resolutions by four QPs at a fast
preset, PCHIP interpolation, per-shot hulls, a trellis across shots, and final
encodes only at the chosen points. The orchestrator invoking yah264 runs it
first. A `tools/ladder/` comes into this repo only if shared analysis shows a
compute win worth owning. This stage owns the first scaler the project needs,
Lanczos-3 for an output-quality downscale, and the decode-upscale-VMAF
methodology that matches the published work. Gate: within 1% BD of the
exhaustive hull at half the encodes or fewer, plus an ffmpeg round-trip
packaging check that rungs switch cleanly at shot-aligned segments.

**S6. Predicted hull, the research stage** (effort L). **Not started, and it
may never start.** Train a GBDT on S5's ground truth to predict hull
membership from S1's features, export the trees as C arrays, and keep a
convexity check after encoding so a misprediction costs one extra encode
instead of a broken ladder. The falsifiable target comes from the literature:
within about 1% BD of the measured hull at 60% fewer encodes, which is where
RCN-Hull came out. The TOMM benchmark says handcrafted features are enough to
get there. Start only once S5 has generated the truth data, and only if
S5's economics say trial encodes are the cost that matters.

## Sequencing against the one defect still open

Two of the three rate-control items above are closed, and neither ever sat in
this plan's dependency chain. Per-shot CRF is open-loop by construction, so
the two-pass allocator's state was never its problem. Whoever works on
two-pass should know the shot allocator may be part of the answer there,
allocating across shots by constant slope and letting the CRF machinery handle
frames within a shot. The flat CRF path was likewise a feature here: S2 added
the missing across-shot term at the granularity where it belongs, and left the
within-window design alone.

Capped VBR's underflow is the one that is still open, and it only bites when a
ladder rung promises a delivery cap, which is S5's packaging check. It wants
fixing on its own track before then, along with the per-GOP VBV buffer reset
that the fresh-instance model causes, which a capped per-shot stream would
inherit.

## What would falsify this plan

Two of these have already happened, and they are marked.

- **Happened, partly.** S3's shot classes fail to reproduce the parked
  psy-trellis and mb-tree wins, so the content-adaptive story loses its best
  evidence. A perfect selector measured about 1%, S3 closed, and the
  differentiator is now S2 plus S4.
- **Happening on long-form.** S2 reads under 1% on real content. On the three
  short multi-shot sequences it clears its gate, and on the one long-form
  window measured it is negative. If a proper long-form corpus confirms the
  second reading, per-shot QP is not worth its complexity, title-level CRF
  plus mb-tree already suffice, and what survives is S4: best engine under an
  orchestrator, which is still worth shipping.
- S5 shows the sparse measured hull is already cheap enough, on short shots at
  fast presets with few points. Then S6's prediction saves compute nobody is
  paying and should not be built. This is a real possibility at short-form
  scale.
- Packaging reality, meaning SSAI ad points and strict segment ladders, forces
  re-encodes that erase the one-invocation savings. Then the ladder work stays
  outside the codec permanently and the codec keeps only S4.

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
