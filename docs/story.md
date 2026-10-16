# yah264: the story

## What this is

yah264 is a from-scratch H.264/AVC encoder written in modern C (C11 plus SIMD
intrinsics where they pay; no assembly in the tree today). It rests on a
many-core pipeline architecture and one hard rule: every frame the encoder
reconstructs must be bit-exact against an independent decoder. That recon-match
gate runs before anything merges. The encoder is never quietly wrong.

It isn't a wrapper, a fork or a teaching toy. We built it tool by tool
(transforms, prediction, entropy coding, motion, in-loop filters, rate control).
Each tool is gated against ffmpeg's decoder and measured against x264 on real
content.

## The goal

Match x264, then beat it. In that order, and in layers:

1. **Pure C first.** Reach x264's compression and get within reach of its speed
   using nothing but portable C. This is the baseline. SIMD is opt-in on
   detection, so the encoder has to be good on a machine with no SIMD at all.
2. **Then SIMD.** NEON, SVE and x86 (SSE4.2, AVX2, AVX-512) intrinsics today.
   It turns on only when the CPU
   advertises it. That keeps the C path from rotting.
3. **Then the GPU.** Mac/Metal and general GPU acceleration for the embarrassingly
   parallel stages (scoring, lookahead, filtering), behind the same correctness
   gate.
4. **Then past x264.** Shot-aware, lookahead-driven, per-title encoding: the
   third-generation techniques that a single-clip encoder can't reach.

The north star is to **be the fastest open-source H.264 encoder, at equal or
better quality.** It's measurable. It's also a goal, not a claim about today.

On a ten-clip board measured 2026-09-02, multi-threaded pure C runs at 0.84x of
x264's time. The shipped NEON build runs at 0.95x and single-threaded pure C at
0.97x, with quality 0.2 to 0.3 VMAF ahead at the same size. Compression depends
on the rate. At low bitrates we lead on 9 of 10 clips by around 12% BD-rate, and
the lead fades to nothing at the top of the range. The open item is low-bitrate
HD, where the worst clip runs 1.16 to 1.18x. The rest of this document says which
line each piece sits on.

Since then yah264 has reached parity with x264 (2026-10-12): faster at the same
file size with the same or better quality, from CIF to 4K and across short
clips, 2-minute segments and 10-minute excerpts. The current figures are in the
README and `site/results.md`.

## Who it's for

Two audiences. The first is engineers who want a readable, correct, modern-C
reference for how a competitive H.264 encoder works. Every non-obvious decision
has a comment that explains *why*. The second is the product side, which
wants a codec it can point at a catalog to encode it better than a correct
encoder would: per-title, per-shot and quality-targeted.

## How we build it

Four disciplines, applied without exception:

- **Recon-match or it doesn't ship.** Encode with yah264, decode with ffmpeg and
  assert that the encoder's own reconstruction equals the decoder's output bit
  for bit, across a QP sweep and every chroma format. This is the phase-1 gate.
  `make conformance` runs it before anything merges. The GitHub workflow's Linux
  job runs the same gate on every push that touches code. The macOS job runs
  only when fired by hand.
- **Quality changes are gated on VMAF-NEG BD-rate** over a real content corpus
  (grain, detail, motion, crowd, cadence). A hand-picked clip doesn't count.
  Plain VMAF over-rewards bits spent on flat backgrounds, so the owner chose
  VMAF-NEG. **Never ship a regression.** A change that loses on the gate is
  reverted and logged with the reason, so we don't re-try dead ends.
- **Behavior, not text.** We study other encoders from the outside: what they
  emit, how fast, at what bitrate, and which decisions they appear to make on a
  given clip. With the standard and the published literature, that's enough to
  implement a technique here. It's the only input we take from them. Their
  output is ground truth for *what good looks like*. Their source is not a draft
  of ours, and `CONTRIBUTING.md` draws the line in detail.
- **Measure both paths.** A C optimization that a NEON build masks on the dev
  machine still wins on every machine without that SIMD. We measure with SIMD
  forced off (`YAH264_NO_ASM=1`) as well as on. C wins that SIMD hides stay in.

## Where it stands, honestly

**Shipped and measured.** The full core toolchain is done. It recon-matches
ffmpeg: CAVLC and CABAC entropy coding, 4×4 and 8×8 transforms, B-frames with
pyramid and a temporal-layer QP cascade, the full rate-control family
(CQP/ABR/CRF/VBV/2-pass), in-loop deblocking, variance adaptive quantization,
quarter-pel motion with sub-partitions and multiple references, weighted
prediction, mb-tree, trellis quantization (RDOQ), psychovisual RD, custom quant
matrices, and the complete chroma/bit-depth matrix: 4:2:0, 4:2:2, 4:4:4, at 8
and 10 bits. Two quality wins moved the BD number. A CABAC-accurate RD rate model
replaced a CABAC-stream cost priced with CAVLC and is worth −3.7 / −2.4 / −1.8%
on three clips. Recalibrating the motion-estimation lambda to an exponential curve in QP is worth a mean ~−0.78%, all of it on the motion clips where the gap was.
The half-pel-plane and MV-cost-table work buys +12% CIF / +23% 720p in speed and
stays bit-exact. Everything above works at every chroma format and bit depth.

**Also shipped.** Wavefront threading: the encoder splits analysis from
bitstream emission, and rows run in parallel on top of the GOP-parallel
workers. We dropped bit-exactness at any thread count as a goal along the way
because it cost more multi-thread speed than it bought. Two runs at
`--threads 1` must be byte-identical, and that's a hard gate. With more threads,
output may differ between runs and thread counts, and the comparison in
`scripts/conformance.sh` only warns. NEON kernels cover the big hot loops (motion
comp, SAD/SATD, quant) at roughly 4.2× on the vectorized path. The scalar path
stays first-class. A shared GPU library holds a full, validated port of VMAF v1
to the GPU that matches libvmaf to ±0.006. It's banked and not yet in the
scoring hot path.

**Planned, and honest about it.** For a long time we read the motion-BD gap as a
lookahead deficit. The mb-tree half of it has since closed, worth about −0.85%
median. What is left is clip-specific, with no single structural hole behind it.
We built a rate term bolted onto mb-tree's propagation and measured it
net-negative on VMAF-NEG, so it isn't in the encoder. Pricing MV rate reduces
propagation exactly where mb-tree wants to boost. TPL (trajectory-aware
propagation with rate and distortion kept separate) is still on the list. It's a
larger, owner-gated architecture decision than a tuning pass. No known gap is
waiting on it any more. Beyond that comes the third-generation direction:
lookahead shot detection feeding shot-based and convex-hull encoding, per-title
rate control and film-grain synthesis.

**The assembly frontier.** The VideoLAN and ffmpeg developers hand-write
assembly for the kernels that dominate a real encode (motion compensation,
SAD/SATD, the transforms, CABAC bit I/O). Those loops run billions of times, and
a compiler leaves throughput on the table. We follow that model, though only
where it pays. Profile, find the loop that owns the time, and convert it to
intrinsics or hand asm only when the measured benefit justifies the maintenance
cost. The portable C stays beside it as the reference and the fallback. NEON is
the first target, and the same discipline extends to the rest.

---

# Optimizations beyond a vanilla H.264 encoder

A textbook H.264 encoder does mode decision by SAD, quantizes with a fixed
deadzone and stops there. Everything below is what yah264 does past that
baseline. Status tags: **[shipped]**, **[in progress]**, **[planned]**.

## Rate-distortion

- **[shipped] Trellis quantization (RDOQ)** on all block types, with a
  CABAC-accurate cost. The real arithmetic-coder bit estimate prices the level
  decision. No lookup table stands in for it.
- **[shipped] CABAC-accurate MB-level RD rate model.** Mode decision on a CABAC
  stream walks the actual CABAC bin costs (header + residual) where it used to
  scratch-code in CAVLC. It corrected both speed and quality (−3.7 / −2.4 /
  −1.8% BD on the eval clips).
- **[shipped] Psychovisual RD** in mode decision. An energy-preservation term
  keeps texture the eye notices even when SSD would smooth it away.
- **[shipped] Rate-aware partition decision.** The mb_type/partition SATD
  comparison carries the real ue(v) bit cost.
- **[in progress] Cheaper accurate cost model.** The CABAC estimate becomes a
  pure table walk over a small snapshot. Per-candidate pricing gets cheap without
  dropping to a wrong proxy, and that's what lets fan-out reduction pay.

## Motion

- **[shipped] Quarter-pel motion** with hex and UMH search, median MV prediction,
  P_Skip, and the full partition set down to P_8×8 sub-partitions (8×8/8×4/4×8/4×4).
- **[shipped] Multiple references** across IPPP, flat-B and B-pyramid structures,
  with mixed references and per-reference weighted prediction.
- **[shipped] Bounded qpel-RD refinement.** A hysteresis-gated ±1 quarter-pel
  nudge of the SATD winner, scored by true RD.
- **[shipped] ME lambda calibration** to an exponential lambda curve in QP,
  concentrated on motion content.
- **[shipped] Half-pel plane reuse.** Motion comp reads frame-wide half-pel
  planes built once per reference instead of re-running the 6-tap filter per RD
  candidate. It's bit-exact and a real win on the scalar path.
- **[planned] ME refinement on erratic motion.** Stronger predictors and search
  where the current gap is worst.

## Rate control

- **[shipped] The full family:** constant-QP, average bitrate, constant rate
  factor, VBV-constrained, and two-pass.
- **[shipped] mb-tree.** Per-MB QP offsets from lookahead propagation, combined
  with AQ into one offset at a content-adaptive strength.
- **[shipped] Variance adaptive quantization.** Per-MB QP from local variance,
  calibrated to strength 0.4. That's the VMAF-NEG optimum for CRF. x264 uses 1.0.
- **[planned] TPL and rate-aware lookahead**, with rate and distortion kept
  separate from day one.
- **[planned] Per-title and per-shot rate control.**

## Threading and parallelism

- **[shipped] GOP-parallel encoding.** Independent GOPs across worker threads.
- **[shipped] Row-wavefront threading.** A hybrid GOP-plus-row model with a
  trailing serial entropy pass. We dropped bit-exactness across thread counts as
  a goal. Two `--threads 1` runs must be byte-identical. Threaded output may
  vary, and the thread-count comparison only warns.

## SIMD and GPU

- **[shipped] NEON kernels** for motion comp, SAD/SATD and quant (~4.2× on the
  vectorized path). They're opt-in on CPU detection, so the C path stays
  first-class.
- **[in progress/banked] A shared GPU library** with a full VMAF v1 port
  validated against libvmaf (±0.006). It's ready for the scoring hot path.
- **[planned] Mac/Metal acceleration** for lookahead, scoring and filtering.
- **[planned] Wider SIMD and targeted hand-assembly** for the loops that own the
  profile, following the x264/ffmpeg/dav1d model.

## Tooling and methodology (the part that makes the rest trustworthy)

- **[shipped] Recon-match conformance gate** against ffmpeg across QPs and every
  chroma format. It runs locally before merge, and in CI on every push that
  touches code.
- **[shipped] VMAF-NEG BD-rate harness** with a content-classed corpus
  (`bdcompare --class`, `fetch_corpus --full`) and a persistent encode cache.
- **[shipped] Fast dev-loop gates.** A parallel byte-identity canary and a 3-clip
  smoke BD keep iteration tight. The full corpus is for sign-off.
- **[shipped] Never-ship-a-regression discipline.** Negative results are reverted
  and logged with the reason, so dead ends aren't re-explored.

## Beyond x264: the differentiators

These go past anything x264 does. They're why the project continues past
"parity":

- **[shipped 2026-09-05] Across-shot allocation in single-pass CRF.** A
  multi-shot sequence exposed what the single-shot board could not. The flat
  CRF path trailed x264 by 5-13% on concatenations of clips it beats one at a
  time. The AQ frame-mean term ran at 40% strength while most B frames
  carried none. The term now applies to the CRF base QP on every path, streaming
  included. That's -4 to -8% against the old default and level with x264, with
  single-shot clips unchanged.
- **[shipped 2026-09-05] Shot table and per-shot CRF** (`--shot-table`,
  `--cut-split`, `--shot-crf`, file input). The pre-scan supplies shot
  boundaries and costs. Every cut gets an IDR. Each shot gets a CRF on
  x264's curve at shot granularity. It's -6 to -11% against cut-split alone. It
  beats the default's across-shot term by 1-4% because it sees the whole file.
- **[planned] Per-shot tool selection and convex-hull ladder output.** Shot
  features drive the psy/AQ/direct choices, then per-shot resolution decisions
  for segmented streaming.
- **[planned] Per-title constant-slope rate control.**
- **[planned] VMAF-targeted rate control**, per-frame QP driven to a quality
  target.
- **[planned] Content-adaptive lambda and film-grain synthesis via FGC SEI.**

---

*This document tracks intent and status. A planned item moves to shipped here
when it ships and passes its gate, not before.*
