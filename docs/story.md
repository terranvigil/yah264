# next264: the story

## What this is

next264 is a from-scratch H.264/AVC encoder written in modern C (C11 plus
hand-written assembly where it pays). It is built on a many-core pipeline
architecture and one hard rule: every frame the encoder reconstructs must be
bit-exact against an independent decoder. That recon-match gate runs on every
commit, so the encoder is never quietly wrong.

It is not a wrapper, a fork, or a teaching toy. It's a real encoder built tool by
tool (transforms, prediction, entropy coding, motion, in-loop filters, rate
control), each one gated against ffmpeg's decoder and measured against x264 on
real content.

## The goal

Match x264, then beat it. In that order, and in layers:

1. **Pure C first.** Reach x264's compression and get within reach of its speed
   using nothing but portable C. This is the honest baseline: the encoder has to
   be good on a machine with no SIMD at all, because SIMD is opt-in on detection,
   not assumed.
2. **Then SIMD.** NEON today, more later, enabled only when the CPU advertises
   it, so the C path is never allowed to rot.
3. **Then the GPU.** Mac/Metal and general GPU acceleration for the embarrassingly
   parallel stages (scoring, lookahead, filtering), behind the same correctness
   gate.
4. **Then past x264.** Shot-aware, lookahead-driven, per-title encoding: the
   third-generation techniques that a single-clip encoder can't reach.

The north star is stated plainly so it can be measured: **be the fastest
open-source H.264 encoder, at equal or better quality.** That's the goal, not a
claim about today. Today we're roughly 7–23% behind x264 on motion-heavy content
by BD-rate, and behind on single-thread speed before SIMD kicks in. The rest of
this document is honest about which line each piece sits on.

## Who it's for

Two audiences. First, engineers who want a readable, correct, modern-C reference
for how a competitive H.264 encoder actually works. Every non-obvious decision is
commented with *why*, not *what*. Second, the product side: a codec that can be
pointed at a catalog and encode it better, not just correctly, per-title,
per-shot, quality-targeted.

## How we build it

Four disciplines, applied without exception:

- **Recon-match or it doesn't ship.** Encode with next264, decode with ffmpeg,
  assert the encoder's own reconstruction equals the decoder's output, bit for
  bit, across a QP sweep and every chroma format. This is the phase-1 gate and it
  runs in CI.
- **Quality changes are gated on VMAF-NEG BD-rate**, on a real content corpus
  (grain, detail, motion, crowd, cadence), not on a hand-picked clip. Plain VMAF
  over-rewards spending bits on flat backgrounds; VMAF-NEG is the owner-chosen
  metric. **Never ship a regression.** A change that loses on the gate is
  reverted and logged, with the reason, so we don't re-try dead ends.
- **Behaviour, not text.** Other encoders are studied from the outside: what
  they emit, how fast, at what bitrate, and which decisions they appear to make
  on a given clip. That plus the standard and the published literature is enough
  to implement a technique here, and it is the only input we take from them.
  Their output is ground truth for *what good looks like*; their source is not a
  draft of ours. `CONTRIBUTING.md` draws the line in detail.
- **Measure both paths.** A C optimization that a NEON build masks on the dev
  machine still matters. It's a real win on every machine without that SIMD. We
  measure with SIMD forced off (`NEXT264_NO_ASM=1`) as well as on, and we keep C
  wins that SIMD hides.

## Where it stands, honestly

**Shipped and measured.** The full core toolchain is done and recon-matches
ffmpeg: CAVLC and CABAC entropy coding, 4×4 and 8×8 transforms, B-frames with
pyramid and a temporal-layer QP cascade, the full rate-control family
(CQP/ABR/CRF/VBV/2-pass), in-loop deblocking, variance adaptive quantization,
quarter-pel motion with sub-partitions and multiple references, weighted
prediction, mb-tree, trellis quantization (RDOQ), psychovisual RD, custom quant
matrices, and the complete chroma/bit-depth matrix: 4:2:0, 4:2:2, 4:4:4, at 8
and 10 bits. Two quality wins moved the BD number. A CABAC-accurate RD rate model
is worth −3.7 / −2.4 / −1.8% on three clips, replacing a CABAC-stream cost priced
with CAVLC. A motion-estimation lambda recalibration to x264's exponential table
is worth a mean ~−0.78%, concentrated exactly on the motion clips where the gap
was. On speed, the half-pel-plane and MV-cost-table work is +12% CIF / +23% 720p,
bit-exact. On breadth, everything above works at every chroma format and bit
depth.

**In progress.** The wavefront threading foundation is built: the encoder
splits analysis from bitstream emission so rows can eventually run in parallel
while staying bit-exact at any thread count, with CAVLC byte-identity as the
canary. NEON kernels cover the big hot loops (motion comp, SAD/SATD, quant) at
roughly 4.2× on the vectorized path; the scalar path stays first-class. A shared
GPU library (nextgpu) holds a full, validated port of VMAF v1 to the GPU
(matches libvmaf to ±0.006), banked, not yet in the scoring hot path.

**Planned, and honest about it.** The remaining motion-BD gap traces to the
lookahead: it isn't rate-aware, and it's decoupled from rate control. Bolting a
rate term onto mb-tree's propagation was built, measured net-negative on
VMAF-NEG, and is not in the encoder. Pricing MV rate reduces propagation exactly
where mb-tree wants to boost. The real fix is TPL (trajectory-aware propagation
with rate and distortion kept separate), which is a larger, owner-gated
architecture decision, not a tuning pass. Beyond that: lookahead shot detection
feeding shot-based and convex-hull encoding, per-title rate control, and
film-grain synthesis, the third-generation direction.

**The assembly frontier.** The model here is explicit. The VideoLAN and ffmpeg
developers hand-write assembly for the kernels that dominate a real encode (motion
compensation, SAD/SATD, the transforms, CABAC bit I/O) because those loops are
run billions of times and a compiler leaves throughput on the table. We follow
that model deliberately, not everywhere: profile, find the loop that owns the time,
convert it to intrinsics or hand asm only when the measured benefit justifies the
maintenance cost, and keep the portable C beside it as the reference and the
fallback. NEON is the first target; the same discipline extends to the rest.

---

# Optimizations beyond a vanilla H.264 encoder

A textbook H.264 encoder does mode decision by SAD, quantizes with a fixed
deadzone, and calls it done. Everything below is what next264 does past that
baseline. Status tags: **[shipped]**, **[in progress]**, **[planned]**.

## Rate-distortion

- **[shipped] Trellis quantization (RDOQ)** on all block types, with a
  CABAC-accurate cost. The level decision is priced by the real arithmetic-coder
  bit estimate, not a table.
- **[shipped] CABAC-accurate MB-level RD rate model.** Mode decision on a CABAC
  stream is priced by walking the actual CABAC bin costs (header + residual), not
  by scratch-coding in CAVLC. This was both a speed and a quality correction
  (−3.7 / −2.4 / −1.8% BD on the eval clips).
- **[shipped] Psychovisual RD** in mode decision, an energy-preservation term
  that keeps texture the eye notices even when SSD would smooth it away.
- **[shipped] Rate-aware partition decision.** The mb_type/partition SATD
  comparison carries the real ue(v) bit cost, x264-style.
- **[in progress] Cheaper accurate cost model.** The CABAC estimate as a
  pure table walk over a small snapshot, so per-candidate pricing is cheap without
  dropping to a wrong proxy. This is the enabler that makes fan-out reduction pay.

## Motion

- **[shipped] Quarter-pel motion** with hex and UMH search, median MV prediction,
  P_Skip, and the full partition set down to P_8×8 sub-partitions (8×8/8×4/4×8/4×4).
- **[shipped] Multiple references** across IPPP, flat-B and B-pyramid structures,
  with mixed references and per-reference weighted prediction.
- **[shipped] Bounded qpel-RD refinement**, a hysteresis-gated ±1 quarter-pel
  nudge of the SATD winner, scored by true RD.
- **[shipped] ME lambda calibration** to x264's exponential lambda table,
  concentrated on motion content.
- **[shipped] Half-pel plane reuse.** Motion comp reads frame-wide half-pel
  planes built once per reference instead of re-running the 6-tap filter per RD
  candidate. Bit-exact; a real win on the scalar path.
- **[planned] ME refinement on erratic motion**, stronger predictors and search
  where the current gap is worst.

## Rate control

- **[shipped] The full family:** constant-QP, average bitrate, constant rate
  factor, VBV-constrained, and two-pass.
- **[shipped] mb-tree**, per-MB QP offsets from lookahead propagation, combined
  with AQ into one offset, with content-adaptive strength.
- **[shipped] Variance adaptive quantization**, per-MB QP from local variance,
  calibrated (strength 0.3, the VMAF-NEG optimum for CRF, not x264's 1.0).
- **[planned] TPL and rate-aware lookahead**, the structural fix for the motion
  gap, kept rate/distortion-separate from day one.
- **[planned] Per-title and per-shot rate control.**

## Threading and parallelism

- **[shipped] GOP-parallel encoding**, independent GOPs across worker threads.
- **[in progress] Row-wavefront threading**, a hybrid GOP-plus-row model
  designed to stay bit-exact at any thread count, with a trailing serial entropy
  pass. The analysis/emit split is built; CAVLC byte-identity is the canary.

## SIMD and GPU

- **[shipped] NEON kernels** for motion comp, SAD/SATD and quant (~4.2× on the
  vectorized path), opt-in on CPU detection so the C path stays first-class.
- **[in progress/banked] nextgpu**, a shared GPU library with a full VMAF v1 port
  validated against libvmaf (±0.006), ready for the scoring hot path.
- **[planned] Mac/Metal acceleration** for lookahead, scoring and filtering.
- **[planned] Wider SIMD and targeted hand-assembly** for the loops that own the
  profile, following the x264/ffmpeg/dav1d model.

## Tooling and methodology (the part that makes the rest trustworthy)

- **[shipped] Recon-match conformance gate** against ffmpeg, across QPs and every
  chroma format, in CI.
- **[shipped] VMAF-NEG BD-rate harness** with a content-classed corpus
  (`bdcompare --class`, `fetch_corpus --full`) and a persistent encode cache.
- **[shipped] Fast dev-loop gates**, a parallel byte-identity canary and a
  3-clip smoke BD for tight iteration, reserving the full corpus for sign-off.
- **[shipped] Never-ship-a-regression discipline.** Negative results are reverted
  and logged with the reason, so dead ends aren't re-explored.

## Beyond x264: the differentiators (planned)

These go past what x264 does at all, and are the reason the project exists past
"parity":

- **[planned] Lookahead shot detection feeding shot-based / convex-hull encoding.**
  Detect scene structure in the lookahead, then encode each shot on its own
  operating point.
- **[planned] Per-title constant-slope rate control.**
- **[planned] VMAF-targeted rate control**, per-frame QP driven to a quality
  target.
- **[planned] Content-adaptive lambda and film-grain synthesis via FGC SEI.**

---

*This document tracks intent and status. A planned item moves to shipped here
when it ships and passes its gate, not before.*
