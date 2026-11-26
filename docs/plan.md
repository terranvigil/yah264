# The yah264 plan

yah264 is an H.264/AVC encoder written from scratch in C11, aiming to be the
fastest open-source H.264 encoder at equal or better quality.

| | |
|---|---|
| SIMD | intrinsics picked at run time: NEON and SVE on ARM, SSE4.2, AVX2 and AVX-512 on x86-64; no hand-written assembly |
| license | GPL-2.0-or-later, or a commercial license for products that can't meet the GPL |
| CLI | x264's flag vocabulary, to drop into existing pipelines; see [options we don't support](what-we-dont-do.md) |
| input | Y4M, raw YUV, or frames from ffmpeg through the [wrapper](ffmpeg.md) |
| output | an Annex-B elementary stream |
| siblings | yah265 for HEVC and yaav1 for AV1, sharing a pipeline design, a correctness gate and the parity spec |

## The goals and the bar

We measure against x264 `--preset medium` at its default CRF 23, with ours at
its own default preset, `medium`. In each cell (one clip at one size) x264 sets
the file size and ours is solved onto it within 1%, so speed and quality are
compared at the same bytes.

| goal | our build | threads | x264 build |
|---|---|---|---|
| G1 | plain C | 1 | assembly off, compiler vectorization on |
| G2 | plain C | all cores | assembly off, compiler vectorization on |
| G3 | as shipped (SIMD) | all cores | as shipped |

Each test group runs at CIF, 720p, 1080p and 2160p.

| group | what it holds |
|---|---|
| S | short clips |
| M | 2-minute segments of real titles |
| L | 10-minute excerpts of real titles |
| C | content types: grain, sports, screen content |

Real-title cells skip black openings and stop before the end credits.

A (class, group, goal) combination passes when it meets every row that
applies. Full parity means every combination passes.

| check | bar | applies to |
|---|---|---|
| wall time, ours over x264 | median at or under 1.00, worst cell at or under 1.15 | S, M, L (shown but not blocking in C) |
| VMAF v1 difference | median no worse than -0.5; no cell more than 1.0 VMAF v1 or 1.0 dB PSNR-Y behind | all groups |
| BD-rate on VMAF v1, five-rung ladder (CRF 20 to 32) | at or under +1.0%, at CRF and at capped VBR, lookahead depth swept, no shot-based encoding | real titles |
| VBR rate tracking | each rung within 10% of its target | real titles |

Correctness gates never relax. An encode's reconstruction must match an
independent decoder: ffmpeg by default, or openh264 and the JM on request.
Each SIMD kernel must match its C reference bit for bit. Two runs at
`--threads 1` must be byte-identical. With more threads, output may differ
between runs and thread counts; those checks only warn.

## Where it stands

yah264 is at full parity with x264 on the Apple M5 Max. The
[results tables](https://terranvigil.github.io/yah264/results.html) show every
counted combination passing at all three goals, in every group from CIF to 4K.

| area | what's built |
|---|---|
| core (High profile and beyond) | CAVLC and CABAC, 4x4 and 8x8 transforms, B-pyramid with adaptive placement, weighted prediction, trellis quantization, psy-RD, mb-tree, variance AQ, CQP, CRF, ABR, VBV, two-pass, custom quant matrices |
| formats | 8 and 10 bit; 4:2:0, 4:2:2 and 4:4:4; PAFF interlaced; slices; intra refresh |
| around the core | [per-shot CRF](shot-based.md), the [orchestrator hooks](engine-interface.md), a VideoToolbox [hardware mode](hardware-mode.md), the ffmpeg wrapper |
| x86-64 | SSE4.2 and AVX2 for every kernel family, AVX-512 for the pixel metrics; one thread [retires fewer instructions](simd.md) than x264 on AMD and Intel |
| ARM | an SVE tier for 256-bit parts such as Graviton3 |

## What's open

Groups are in priority order, and items are ranked within each. An item
leaves the list when it ships or a measurement closes it. A closed idea goes
into its subsystem doc's "What didn't work" table.

### Speed on other machines

1. Threading at high core counts on x86-64. At default threads on a 32-vCPU
   Intel machine we're 4% behind x264 on wall time at 1080p and ahead at 4K
   (see [threading](threading.md) and [SIMD](simd.md)).
2. The goal board on AMD x86-64. Spot reclaims stopped its first run.

### Rate control ([rate-control.md](rate-control.md))

3. Threaded ABR output depends on when GOP workers start, because the shared
   rate factor reacts to start timing: up to about 9% BD-rate on two short
   clips. CRF and one-thread output aren't affected.
4. Quality-target rate control: hold a perceptual score instead of a CRF.
5. Coded-size prediction fitted from lookahead features (intra cost, inter
   cost, motion spread, QP).

### Frame types and the lookahead ([b-frames.md](b-frames.md))

6. Whole-run B placement, off by default. It helps Cosmos Laundromat and some
   short clips and costs coastguard. It needs a real-title round first.
7. Trajectory-aware propagation that keeps rate and distortion separate. No
   known gap waits on it.

### Coding tools ([motion-and-modes.md](motion-and-modes.md))

8. Intra refresh costs about 50% more than keyframes on near-static scenes
   against 5 to 13% on moving content. Likely fix: mb-tree treats the refresh
   column as intra.
9. Lossless coding. `--crf 0` is refused today.
10. The 8x8 transform in 4:4:4 (see [formats](formats.md)).
11. Chroma weights in explicit weighted prediction (identity today).
12. Chroma QP by B-pyramid level, to steady quality across frames.
13. Saliency-weighted AQ: a cheap heuristic (center weighting, motion against
    global motion, skin tones) on top of variance AQ.
14. Learned mode pruning: small decision trees, exported as C arrays, that
    skip losing P8x8 sub-modes and 4x4 intra trials.

### Memory and input

15. GOP worker memory, about 0.5 GB each at 1080p. Shrink the lookahead ring,
    the lowres sets and the per-frame decision records.
16. `--cut-split` and `--shot-crf` on piped input hold the input up to the
    memory limit. A bounded version would rate shots against a running
    average. Its output would then differ from a file encode.

### Shots and ladders ([shot-based.md](shot-based.md))

17. A measured per-shot hull with a resolution ladder, in the orchestrator
    outside this repository. A prototype has run.
18. A predicted hull trained on the measured one, after 17.
19. One analysis for a whole ladder: seed the lower rungs from the top one.
20. Content-adaptive scaler choice per shot, for the ladder's resizes.

### New features

21. A [live control plane](live-control.md). Built: per-frame events for
    bitrate and buffer, CRF, QP limits, keyframes and zones, echoed in the
    stats, plus status and a replayable log. Left: frame-rate, preset and
    resolution events, and the ffmpeg wrapper's mapping.
22. Film grain synthesis via the H.264 film grain SEI, with `--mcstf` as the
    denoiser (see [denoise](denoise.md)). yah265 has one to port.
23. Segment output (CMAF/fMP4), then MPEG-TS.
24. A prefilter trained against this encoder, the riskiest item here.

### Verification

25. A fuzz harness. There isn't one.
26. A flat-gradient banding test set, so AQ changes are checked for banding
    as well as VMAF v1.

### Decoding

27. A fast decoder. The in-tree decoder only verifies the encoder and has
    never been timed. First steps: a `--decode` mode and a baseline against
    threaded ffmpeg.

### Outside the code

28. A position on the AVC patent pool before any commercial distribution.
