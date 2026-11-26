# The yah264 plan

yah264 is an H.264/AVC encoder written from scratch in C11. Its SIMD kernels
are intrinsics that it picks at run time: NEON and SVE on ARM, SSE4.2, AVX2 and
AVX-512 on x86-64. There's no hand-written assembly. The code is licensed
GPL-2.0-or-later, and products that can't meet the GPL can use a commercial
license instead. The CLI keeps x264's flag vocabulary so it drops into existing pipelines.
The [options we don't support](what-we-dont-do.md) are listed on their own
page. Input is Y4M or raw YUV, or frames from ffmpeg through the
[wrapper](ffmpeg.md). Output is an Annex-B elementary stream.

The aim is to be the fastest open-source H.264 encoder at equal or better
quality. Its sibling encoders are yah265 for HEVC and yaav1 for AV1. All three share
a pipeline design, a correctness gate and the parity spec.

## The goals and the bar

We measure against x264 `--preset medium` at its default CRF 23. Ours runs at
its own shipped default preset, `medium`. For each cell (one clip at one size)
x264 sets the file size and ours is solved onto it within 1%, so speed and
quality are compared at the same bytes. There are three goals:

| goal | our build | threads | x264 build |
|---|---|---|---|
| G1 | plain C | 1 | assembly off, compiler vectorization on |
| G2 | plain C | all cores | assembly off, compiler vectorization on |
| G3 | as shipped (SIMD) | all cores | as shipped |

Every group of the test set runs at CIF, 720p, 1080p and 2160p.

| group | what it holds |
|---|---|
| S | short clips |
| M | 2-minute segments of real titles |
| L | 10-minute excerpts of real titles |
| C | content types: grain, sports, screen content |

Real-title cells skip black openings and stop before the end credits.

A (class, group, goal) combination passes when:

- the median ratio of our wall time to x264's is at or under 1.00 and the
  worst cell is at or under 1.15
- the median VMAF v1 difference is no worse than -0.5 and no cell is more
  than 1.0 VMAF v1 or 1.0 dB PSNR-Y behind
- on real titles, BD-rate on VMAF v1 over a five-rung ladder (CRF 20 to 32)
  is at or under +1.0%, at CRF and at capped VBR, with lookahead depth swept
  and no shot-based encoding
- at VBR on real titles, the bitrate of each rung is within 10% of its target

In the content-type group only quality has to pass. Speed is shown there but
doesn't block. Full parity means every combination passes.

Correctness gates sit under all of this and never relax. An encode's
reconstruction must match an independent decoder: ffmpeg by default, or
openh264 and the JM on request. Each SIMD kernel must match its C reference
bit for bit. Two runs at `--threads 1` must be byte-identical. With more
threads, output may differ between runs and thread counts. The checks for that
warn and never fail.

## Where it stands

yah264 is at full parity with x264 on the Apple M5 Max. The
[results tables](https://terranvigil.github.io/yah264/results.html) show every
counted combination passing at all three goals, in every group from CIF to 4K.

The core is feature-complete for High profile and beyond: CAVLC and CABAC,
4x4 and 8x8 transforms, B-pyramid with adaptive placement, weighted
prediction, trellis quantization, psy-RD, mb-tree, variance AQ, the full rate
control family (CQP, CRF, ABR, VBV, two-pass) and custom quant matrices. It
also covers 8 and 10 bit, 4:2:0, 4:2:2 and 4:4:4, PAFF interlaced, slices and
intra refresh. Around the core sit per-shot CRF and the orchestrator hooks, a
VideoToolbox hardware mode and the ffmpeg wrapper. The
[shot-based encoding](shot-based.md), [engine interface](engine-interface.md)
and [hardware mode](hardware-mode.md) docs describe them.

On x86-64 every kernel family has SSE4.2 and AVX2 versions, and AVX-512
covers the pixel metrics. One thread [retires fewer instructions](simd.md)
than x264 on both AMD and Intel. An SVE tier serves 256-bit ARM parts such as
Graviton3.

## What's open

The groups are in priority order, and items are ranked within each. An item
leaves this list when it ships or when a measurement closes it. A closed idea
goes into its subsystem doc's "What didn't work" table.

### Speed on other machines

1. Threading at high core counts on x86-64. At default threads on a 32-vCPU
   Intel machine we're 4% behind x264 on wall time at 1080p and ahead at 4K
   (see [threading](threading.md) and [SIMD](simd.md)).
2. The goal board on AMD x86-64. Its first run was stopped by spot reclaims.

### Rate control ([rate-control.md](rate-control.md))

3. Threaded ABR output depends on when GOP workers start, because the shared
   rate factor reacts to start timing. On two short clips it moves by up to
   about 9% BD-rate. CRF and one-thread output aren't affected.
4. Quality-target rate control: hold a perceptual score instead of a CRF.
5. Better coded-size prediction: a fitted model of frame bits from lookahead
   features (intra cost, inter cost, motion spread, QP).

### Frame types and the lookahead ([b-frames.md](b-frames.md))

6. Whole-run B placement, off by default. It helps Cosmos Laundromat
   and some short clips and costs coastguard. It needs a real-title round
   before it can be the default.
7. Trajectory-aware propagation that keeps rate and distortion separate. No
   known gap waits on it.

### Coding tools ([motion-and-modes.md](motion-and-modes.md))

8. Intra refresh on near-static scenes costs about 50% more than keyframes,
   against 5 to 13% on moving content. The likely fix is having mb-tree
   treat the refresh column as intra.
9. Lossless coding. `--crf 0` is refused today.
10. The 8x8 transform in 4:4:4 (see [formats](formats.md)).
11. Chroma weights in explicit weighted prediction. Chroma is identity today.
12. Chroma QP by B-pyramid level, to steady quality across frames.
13. Saliency-weighted AQ: a cheap heuristic (center weighting, motion against
    global motion, skin tones) on top of variance AQ.
14. Learned mode pruning: small decision trees, exported as C arrays, to skip
    P8x8 sub-modes and 4x4 intra trials that won't win.

### Memory and input

15. GOP worker memory, about 0.5 GB each at 1080p. The lookahead ring, the
    lowres sets and the per-frame decision records are where to shrink it.
16. `--cut-split` and `--shot-crf` on piped input still hold the input up to
    the memory limit. A bounded version would rate shots against a running
    average. Its output would then differ from a file encode.

### Shots and ladders ([shot-based.md](shot-based.md))

17. A measured per-shot hull with a resolution ladder. It belongs to the
    orchestrator, which lives outside this repository. A prototype has run.
18. A predicted hull, trained on the measured one. It starts only after 17.
19. One analysis for a whole ladder: seed the lower rungs from the top one.
20. Content-adaptive scaler choice per shot, for the ladder's resizes.

### New features

21. A [live control plane](live-control.md). Built: events for bitrate and
    buffer, CRF, QP limits, keyframes and zones. Each takes effect on a named frame
    and shows in the stats. Status goes out and the log replays. Left:
    frame-rate, preset and resolution events, and the ffmpeg wrapper's
    mapping onto the plane.
22. Film grain synthesis via the H.264 film grain SEI, with `--mcstf` as the
    denoiser (see [denoise](denoise.md)). yah265 already has one to port.
23. Segment output (CMAF/fMP4), then MPEG-TS.
24. A prefilter trained against this encoder. It's the riskiest item here.

### Verification

25. A fuzz harness. There isn't one.
26. A flat-gradient banding test set, so AQ changes are checked for banding
    as well as VMAF v1.

### Decoding

27. A fast decoder. The in-tree decoder only verifies the encoder and has
    never been timed. The first steps are a `--decode` mode and a baseline
    against threaded ffmpeg.

### Outside the code

28. A position on the AVC patent pool before any commercial distribution.
