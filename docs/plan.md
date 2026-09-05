# yah264: project plan

Status header (2026-09-04): the plan of record. Decisions marked as locked in 2026-06 are
annotated below where the tree went another way; docs/knobs.md and the site carry the
measured state.

Goal: an H.264 encoder measurably faster than x264 at equal quality, built from scratch. Decisions below follow docs/research.md.

## Locked decisions

- Language: C11 core, SIMD kernels as NEON intrinsics on aarch64 (arm64-first; the tree has no hand-written assembly, and x86-64 SIMD has not started -- see the status note near the end), runtime CPU dispatch. Public header stays C99-compatible, matching FFmpeg's expectation.
- License: BSD-2-Clause. Clean-room policy: contributors do not port x264/x265 code or write from memory of their internals. Spec, papers, and public documentation only. Policy lives in CONTRIBUTING.md from the first commit.
- API: our own header in x264's shape (params struct, preset/tune/profile, picture in, NAL units out) with independently written text. CLI keeps x264's flag vocabulary (--preset, --crf, --tune, --bframes) so it drops into existing pipelines.
- I/O: Y4M and raw YUV on stdin, Annex-B on stdout, byte-compatible with x264 conventions. Native CMAF/fMP4 segment output later, MPEG-TS after that. No RTP/SRT in-process.
- Threading: SVT-style decoupled pipeline. Reproducible output for a given configuration is a hard requirement and a CI gate; bitstream identity across thread counts is not, because it costs more multi-thread speed than it buys.
- Quality metric: VMAF-NEG (`vmaf_v0.6.1neg`, libvmaf) is the primary quality gate in the harnesses, alongside plain VMAF, PSNR and SSIM; BD-rate against x264 is computed on it. The v1 model is optional and off unless `YAH264_VMAF_MODEL` points at it, in which case `scripts/vmaf.sh` and `scripts/bdcompare.py` report it as well.
- Build: Meson (no assembler in the build). clang as the reference compiler.

## Architecture

Pipeline stages, each a thread-pool consumer connected by queues:

1. Ingest and lookahead. Lowres downscale, intra/inter cost estimation, scene-cut segmentation, complexity stats, frame-type and minigop decision, mb-tree-style propagation, per-shot QP planning. The lookahead is itself parallel across frames, unlike x264's.
2. Mode decision. Per-frame, macroblock-row wavefront parallelism (rows proceed with a 2-MB horizontal lag). Multi-stage decision: cheap classifiers prune partition/mode candidates before full RD, SVT-style, giving a fine-grained speed ladder rather than on/off feature toggles.
3. Reconstruction. Transform, quant, dequant, deblock, interpolation for reference frames. Fused into stage 2's wavefront where dependencies allow.
4. Entropy coding. CABAC/CAVLC as a separate pipeline stage consuming finished MB rows, hiding H.264's per-slice serialization behind frame-level parallelism.
5. Rate control. Designed for many cores from day one: per-shot planning from the lookahead, per-frame allocation, row-level adaptation with linear (not quadratic) VBV accounting. x264's admitted 12-24 thread breakdown is a primary target.

Frame-level parallelism across this pipeline for throughput; segment-based mode for low-latency; slice threading and intra-refresh for the zerolatency tune.

Kernel layer: every hot function (SAD, SATD, subpel filters, transforms, quant, deblock, CABAC bit ops where possible) has a C reference and asm variants selected by dispatch table. Tiers: SSE2 baseline, SSSE3/SSE4.1, AVX2 primary, AVX-512 first-class (Zen 4/5 changed the throttling story), NEON+dotprod from the start. VNNI for SAD/SATD is a profiling-driven experiment, not a commitment. No GPU offload in the core; the x264 OpenCL record says no. (A hardware encode mode, `--hw videotoolbox`, exists as a separate backend that replaces the core rather than offloading part of it: docs/videotoolbox-plan.md.)

## Phases

Ordering is dependency-driven. Each phase has an exit gate; nothing advances past a red gate.

### Phase 0: skeleton and harnesses

Repo layout, Meson build, CI (Linux x86-64 first, then macOS arm64). checkasm-style bench that validates every asm kernel against its C reference with cycle counts (x265's practice). Golden-stream test runner: encode fixed inputs, decode with FFmpeg's h264 decoder and openh264, verify bit-exact reconstruction against our own decoder-side reconstruction (encoder and its internal recon must match what a conformant decoder produces). Test corpus: Xiph/derf clips plus a handful of 1080p shots, fetched by `scripts/fetch_corpus.sh` rather than checked in. Only a few rows carry a sha256; the rest are marked `-` and skip verification until a first verified download pins them.

Gate: CI green, checkasm runs, a hello-world NAL (SPS/PPS + skip-frame) decodes in ffprobe.

Status: **done.** Built: Meson build, BSD-2 license, clean-room
CONTRIBUTING. Common layer (MSB-first bit writer with Exp-Golomb and RBSP, NAL
packaging with emulation prevention, runtime CPU detection for x86-64 and
aarch64). DSP layer with SAD C references plus dispatched NEON kernels
(1.29-1.41x on 16x16/8x16 on Apple Silicon). checkasm validates every dispatched
kernel against its C reference and benchmarks it. Phase 0 encoder emits SPS/PPS
plus IDR frames coded as I_PCM, a lossless raw-sample placeholder that exercises
the full slice/macroblock path. CLI does Y4M in, Annex-B out over pipes. Unit
tests for bitstream and NAL. Conformance runner encodes then decodes with ffmpeg
and asserts bit-exact reconstruction plus byte-identical output across runs; it
passes on six geometries including non-MB-aligned crop cases and odd chroma, and
runs clean under ASan+UBSan. GitHub CI covers Linux x86-64 and macOS arm64.
The gate was exceeded: rather than just a skip frame, the hello-world reproduces
the input bit-exactly (verified with framemd5), which is a stronger check.

### Phase 1: correct intra encoder

Bitstream writer (NAL, RBSP, SPS/PPS, slice headers). I-frame-only, 4:2:0 8-bit, Baseline-ish: 16x16 and 4x4 intra prediction, 4x4 integer transform, scalar quant, CAVLC, deblocking. C only, single-threaded, correctness over everything.

Gate: every encoded stream decodes bit-exact-recon in FFmpeg, openh264, and the JM reference decoder across the corpus and fuzzed QPs.

Status: **done.** Built: 4x4 core transform and Hadamard DC transforms with
quant/dequant per the spec MF/V tables and chroma-QP mapping (round-trip
unit-tested); intra prediction for I_16x16 (4 modes), chroma
8x8 (4 modes), and I_4x4 (all 9 modes); full CAVLC residual coding with the
normative VLC tables (validated prefix-free and by an independent round-trip
decoder in tests/test_cavlc.c); a closed-loop macroblock pipeline doing
SATD-based mode decision, forward transform/quant, reconstruction, and CAVLC,
with per-macroblock selection between I_16x16 and I_4x4. The encoder is lossy and
QP-controlled (constant QP). Verified: the encoder's own reconstruction is
bit-identical to FFmpeg's decode across QP 0..51 and varied content (detail,
bars, flat, incompressible noise, non-MB-aligned crops), for both luma modes,
and clean under ASan/UBSan. PSNR/size track QP as expected (55 dB at QP18, 41 dB
at QP34). The conformance gate checks recon-match (decode == encoder
reconstruction) rather than decode-equals-input, since coding is lossy.

The in-loop deblocking filter and the RD-based intra mode decision are Phase 3
and Phase 4 work; this phase signalled deblocking off via
disable_deblocking_filter_idc=1, which keeps reconstruction exactly
prediction+residual and recon-exact against the decoder, and decided
I_16x16-vs-I_4x4 on a SATD threshold.

### Phase 2: inter and the pipeline

P-frames: full-pel motion search (dia/hex/umh ladder), subpel refinement, multiple reference frames, P-skip, MV prediction. Stand up the real pipeline at the same time: lookahead stage, wavefront mode decision, entropy as its own stage, deterministic threading. Determinism gate enters CI here (same output at 1, 4, N threads).

Gate: conformance as phase 1, plus determinism, plus a first speed/quality point on the table vs x264 baseline-profile settings.

Status: **done.** Built: reference-frame management (IDR then all-P, single
reference), P-slice headers with frame_num/POC, bit-exact motion compensation
(luma quarter-pel 6-tap +
bilinear, chroma eighth-pel, unrestricted MVs via edge clamping), diamond
integer + half/quarter-pel motion search, median MV prediction and P_Skip MV
derivation (8.4.1.3/8.4.1.1), inter 4x4 CAVLC residual with the inter CBP
me(v) mapping, and a per-macroblock decision among P_Skip, P_16x16, and intra
(I_16x16/I_4x4). Verified: encoder reconstruction is bit-identical to FFmpeg's
decode over multi-frame IPPP sequences across QP 0..51 with real motion, noisy
content, and non-MB-aligned crops; clean under ASan/UBSan; deterministic output.
A drift bug (encoder/decoder MV-predictor disagreement once non-zero MVs appear)
was found and fixed. The benchmark harness (bench/bench.py) tracks VMAF and
encode speed against a matched x264 IPPP baseline.

The first data point, taken when P-frames landed (matched settings, VMAF
v0.6.1): at equal QP we sat within ~1 VMAF of x264 but spent 25-85% more bits,
an efficiency gap from the RD tools not yet built, and ran 30-90x slower, mostly
on a naive MC-based motion search.

### Phase 3: High profile quality tools

CABAC. 8x8 transform and 8x8 intra. B-frames with pyramid and adaptive placement (target: better than x264's B-adapt 1, which its developers call substantially improvable). Weighted prediction. Direct modes.

Gate: BD-rate (VMAF v1 and PSNR) within striking distance of x264 --preset medium on the corpus; all conformance and determinism gates.

Status: **CABAC done for I, P, and B slices** (opt-in --cabac), verified two ways: recon-match against ffmpeg and an encoder-oplog-vs-ffmpeg bin trace, across QP 0-51, CAVLC/CABAC, bframes 1-3, varied content. Direct modes and B-slice coding came with Phase 2. A latent B mv-predictor bug (non-list neighbour must contribute mv 0 to the median), exposed by --dump-recon-for-B, is fixed.

**8x8 transform + Intra_8x8 done, High profile** (opt-in
--transform-8x8). Built: the normative 8x8 inverse transform (8.5.13.2) with a
matched forward and six-category quant/dequant (normAdjust8x8); Intra_8x8
prediction (9 modes with the 8.3.2.2.1 reference-sample low-pass filter);
High-profile SPS/PPS (profile_idc 100, transform_8x8_mode_flag + tail); the
I_16x16/I_4x4/I_8x8 mode decision; transform_size_8x8_flag coding for I_NxN and
(as 0) for inter MBs with luma coefficients; the 8x8 luma residual for both
entropy coders (CAVLC as four interleaved 4x4 sub-blocks per 8.5.6; CABAC as one
ctxBlockCat-5 block, no coded_block_flag, with the SIG8/LAST8 ctxIdxMaps); and
deblocking that skips the internal 4x4 edges of an 8x8-transform MB (per-MB
mb_tr8 flag). Recon matches ffmpeg across QP 0-51, CAVLC/CABAC, I/P/B, B-frames,
crops, at 176x144 and 320x240; conformance is 87/87. Four bugs were found via
recon-match, three CABAC-subtle: the VR/HD prediction else-branch needed the
general y-2x/x-2y index; the CABAC LAST8 map had wrong runs (a self-consistent
round-trip can't catch a spec-value error, so the whole table was diffed against
ffmpeg's own 8x8 last-coefficient context map); the intra trial's scratch write and the
I-slice CABAC path both mishandled mb_tr8, so deblock touched internal edges.

**Implicit weighted biprediction done**, weighted_bipred_idc 2,
auto-enabled with B-frames. B bi-prediction (explicit L0/L1/Bi and direct/skip)
uses POC-distance weights (8.4.2.3.2) instead of a flat average; the weights are
fully decoder-derivable so nothing extra is signalled. Recon-matches ffmpeg at
bframes 1-3, CAVLC/CABAC, QP 0-51. Measured -8.4% (bframes 2) / -5.9% (bframes 3)
on a fade at equal QP, neutral on normal motion.

**Explicit P-slice weighted prediction done**, weighted_pred_flag 1.
A per-frame luma weight+offset is estimated from the source/reference DC ratio
(a fade shows as a global luma scale), signalled in pred_weight_table, and
applied in the P luma MC (all partitions + P_Skip). Chroma stays identity. It
activates only on a detected fade, else signals luma_weight_l0_flag 0 (bit-exact
with unweighted). Recon-matches ffmpeg, QP 0-51, CAVLC/CABAC. Measured -12.2%
(CAVLC) / -10.9% (CABAC) on a P-frame fade, neutral otherwise.

**B-pyramid (hierarchical referenced B) done**, auto-enabled at
bframes >= 2. A general decoded-picture buffer (per-reference recon + resolved
co-located motion) replaces the fixed past/future-anchor pair; each mini-GOP's
B's are coded middle-first as a temporal pyramid, the middle B a reference the
others predict from. FrameNum is a running reference counter (wider FrameNum to
avoid wrap); B lists use the default POC order, P lists pin the previous anchor
via ref_pic_list_modification; the sliding window mirrors the decoder. Recon
matches ffmpeg across bframes 2-3, CAVLC/CABAC, keyint/IDR resets, 8x8, and
non-MB-aligned crops (conformance 138/138); deterministic across thread counts.
Measured ~1.3-1.4% smaller at equal QP from the shorter prediction distances.

**B-frame deblocking done.** The in-loop filter runs on B slices
too: the boundary-strength derivation handles
B's dual-list motion (different reference-picture set or MV count -> bS 1, else a
per-list MV-difference test; list0[0] and list1[0] are distinct pictures so the
mixed-case MV pairing is unique). Reference B's store their filtered recon into the
DPB. Recon-matches ffmpeg across bframes 1-3, CAVLC/CABAC, 8x8, and crops; B was
the last slice type skipping the filter, so every slice type is deblocked.

Per-temporal-layer QP offsets (deeper B's take a larger QP penalty) are in
frame_qp.

Some Phase 4 work landed early since it composes cleanly: variance AQ (--aq-strength) with the full per-MB QP machinery (real mb_qp_delta coding + prediction chain, per-edge deblock QP), and frame-type QP differentiation (I lower, B higher). A 64-case cross-feature matrix (entropy x bframes x QP x AQ x content) recon-matches with zero failures.

### Phase 4: RD machinery and rate control

Trellis quantization (principled for both CABAC and CAVLC; x264's CAVLC trellis is an admitted hack). Psy-RD tuned against VMAF v1: NEG-default means sharpening-style enhancement buys nothing, and the new banding and chroma features mean adaptive quantization must actively prevent banding in flat regions and chroma cannot be starved. Variance AQ, mb-tree propagation from lookahead. Rate control modes: CQP, CRF, ABR, 2-pass, VBV with linear-complexity accounting, per-shot planning.

Gate: BD-rate parity or better vs x264 medium at equal settings; VBV compliance verified by a stream analyzer; no banding regressions on a flat-gradient test set scored with VMAF v1's banding feature.

Status: **partial.** RD machinery landed incrementally: CAVLC RDOQ
(4x4), RD-based inter mode/partition decision (J = SSD + lambda*bits via scratch
coding), and an **RD-based intra mode decision** (I_16x16/I_4x4/I_8x8 by
J = SSD + lambda*estimated_bits) that replaced the SATD-plus-bias heuristic and
unlocked the 8x8/4x4 tools, measuring a clean BD-rate gain (smaller *and* higher
PSNR, +0.41 dB at qp20 on CIF I-frames). Variance AQ (--aq-strength) with the
full per-MB QP machinery; a latent bug where the RD trials perturbed the
mb_qp_delta chain (breaking AQ recon on P/B) was found and fixed, and AQ is in
the conformance gate. **Scene-cut detection** (x264-style) landed: per-MB intra cost vs the best inter
cost from an integer diamond motion search against the previous frame, cut when
pcost >= (1-bias)*icost with a GOP-length-adaptive bias and a min-keyint floor.
The real motion search is what makes it work (a zero-MV first attempt conflated
motion with cuts and was reverted). Fires exactly on a mid-GOP cut, no false
positives on continuous motion; +0.44-0.52 dB at ~+2% size across a cut. It is
the first piece of the lookahead; a lowres pass (for speed) and mb-tree build on
the same complexity/ME machinery.

**The rate-control family is in: CQP, ABR, CRF, VBV, and 2-pass.**
- ABR (--bitrate): proactive, complexity-driven per-frame QP from a smoothed
 complexity estimate plus buffer-error correction; accurate to ~1% on a single GOP.
- CRF (--crf): constant quality; qp = crf + 6(1-qcomp)log2(C/Cref) with a per-type
 running complexity reference. Monotonic rate ladder.
- VBV (--vbv-maxrate/--vbv-bufsize): leaky-bucket cap on top of any base QP;
 one-sided (never inflates an under-budget CRF stream). Converges to ~maxrate.
- 2-pass (--pass 1/2 --stats): pass 1 records per-frame cost, pass 2 re-plans the
 remaining budget each frame from the QP-invariant cost. Accurate to ~0.3% and
 +2.25 dB over ABR at equal bitrate on cut-heavy content.

All recon-match and are ASan-clean.

### Phase 5: assembly waves

Profile-driven. Wave 1: SAD/SATD/pixel ops, SSE2+AVX2. Wave 2: transforms, quant, deblock, interpolation, AVX2. Wave 3: AVX-512 across the same kernels. Wave 4: NEON/dotprod parity on aarch64. Every kernel lands with checkasm coverage. Interleave with phases 3-4 as profiles demand; this phase is a workstream more than a stage.

Gate: ≥2x over our own C baseline (x264's asm multiplier) and target speed vs x264 met at three ladder points (fast, medium, slow equivalents).

Status (2026-09-03): **partial, and not as written.** Wave 4 came first because development is arm64-first: NEON kernels ship for pixel (SAD/SATD/SA8D), motion compensation, transforms, quant and deblock (`src/dsp/*_neon.c`), dispatched at run time and covered by checkasm. They are intrinsics, not hand asm, and the tree has no assembly at all; a 2026-09-02 head-to-head found the intrinsics tie hand-written asm on every shape both sides implement, so the open item is kernel coverage rather than the instruction level. The x86-64 waves (SSE2 through AVX-512) have not started.

### Phase 6: presets, tunes, benchmark honesty

Define the preset ladder as schedules over ref count, search method/range, subme-equivalent, partition depth, B-adapt, trellis, lookahead depth. Tunes: film, animation, grain, zerolatency, psnr/ssim/vmaf. Public benchmark harness: BD-rate (VMAF v1, PSNR) and speed vs x264 and x265 across the corpus, reproducible by anyone.

Gate: at matched VMAF-NEG, faster than x264 at equivalent ladder points, with the margin published per preset. This is the project's main claim.

Status (2026-09-03): **partial.** The preset ladder (ultrafast through placebo) and the tunes (grain, film, animation, psnr, ssim, zerolatency) ship in the CLI. The benchmark harness ships and its results are published on the site; the margin is quoted at `--preset medium` rather than per preset, and there is no CI job behind it, since CI is manual.

### Phase 7: differentiators

Shot-aware single-pass encoding: scene segmentation and per-shot complexity classification in the lookahead, per-shot rate/QP planning (Dynamic-Optimizer thinking collapsed into one pass). Hull-assist mode: machine-readable shot boundaries, per-shot RD stats side channel, deterministic per-shot re-encode with parameter overrides, making yah264 the best engine under a Netflix-style orchestration layer. CMAF/fMP4 native segment output with chunked flushing for low-latency HLS/DASH; MPEG-TS second.

Gate: measured bitrate savings from shot-aware mode on long-form content vs our own fixed-CRF; a demo DASH/HLS pipeline with no external packager.

Status (2026-09-05): **started.** The shot table, cut-split IDRs and per-shot CRF shipped (`--shot-table`, `--cut-split`, `--shot-crf`; docs/shot-based-plan.md S1-S2), and the across-shot allocation term they exposed is now in the default CRF path for every input mode (`Y264_AQ_DC`). Hull assist, per-shot tool selection and segment output are not started.

### Phase 8: breadth

High 10 (10-bit), which has almost no hardware competition (NVENC only from Blackwell, Intel never). 4:2:2 and 4:4:4. Lossless. Quality-target rate control (constant perceptual quality toward a VMAF proxy score; the convex-hull literature shows millisecond-class proxies work). Interlace/MBAFF only if a broadcast customer demands it.

Status (2026-09-03): **partial, and out of order.** 10-bit (`-Dbit_depth=10`), 4:2:2 and 4:4:4 all landed early and recon-match across I/P/B and both entropy coders; only 4:4:4 with the 8x8 transform is deferred. Lossless, quality-target rate control and interlace are not started.

## Validation, always-on

As built, not as planned: `.github/workflows/ci.yml` is `workflow_dispatch` only, so none of the gates below run per commit. They run locally (`make test`, `make conformance`) and in CI when the workflow is fired.

- Conformance: decode of every encode the gate produces, recon-match required. One decoder today, FFmpeg. openh264 and JM were planned and are not wired in.
- Determinism: bit-identical across runs for a given configuration.
- checkasm: every dispatched kernel vs its C reference, correctness and cycles; `tools/checkasm`, run by `meson test` and by CI when fired.
- Fuzzing: planned. There is no fuzz harness in the tree.
- Quality regression: BD-rate (VMAF v1 NEG, PSNR) vs pinned baselines of ourselves and x264; any regression beyond noise blocks merge.
- VBV/HRD compliance checks on rate-controlled streams.

## Risks

- Effort scale: x264 embodies roughly two decades of tuning; the asm layer alone is person-years. Mitigation: strict profiling discipline, kernels in measured-impact order, and the SVT-style architecture doing the heavy lifting for the speed claim rather than out-tuning x264 kernel by kernel.
- CABAC serialization caps single-frame speedups; the pipeline hides it at throughput but not at latency-1. Zerolatency will lean on slices, costing compression, same as x264.
- Rate control is the least-parallelizable, most-tuned part of x264; matching its CRF behavior perceptually will take longer than the math suggests.
- Patents: AVC pool coverage is thinning as patents expire, but a Via-LA posture decision is required before commercial distribution. Track expiries; the tools we ship first (Baseline/High-vintage) are the oldest.
- VMAF v1 is new; industry baselines are still v0.6.1. Publish both scores during the transition so our numbers are comparable to everyone else's.
