# yah264 docs

Each subsystem doc says what yah264 does now and why, what we tried that
didn't work, and what's still open. Current speed and quality numbers are on
the [results page](https://terranvigil.github.io/yah264/results.html).

## Start here

- [Plan](plan.md): what yah264 is, the goals and bar it's measured against, where it stands and what's open.
- [How H.264 works](how-h264-works.html): a walk through H.264 encoding for readers new to the codec.
- [What we don't do](what-we-dont-do.md): x264 options and features we refuse and the reason for each.

## Subsystems

- [Rate control](rate-control.md): CQP, CRF, ABR, VBV and two-pass, and which one fits which job.
- [Lookahead and AQ](lookahead-and-aq.md): the lookahead, mb-tree, adaptive quantization and scene cuts.
- [B-frames](b-frames.md): B-frame placement, the pyramid and direct modes.
- [Motion and modes](motion-and-modes.md): motion search, partitions and the mode decision.
- [Threading](threading.md): how the encoder splits work across cores and picks a thread count.
- [Threading, illustrated](threading.html): the threading model as a site page with diagrams.
- [SIMD](simd.md): the NEON, SVE, SSE4.2, AVX2 and AVX-512 kernels and how they're checked.
- [GPU](gpu.md): the GPU and matrix-unit offload we measured and why the encoder stays on the CPU.
- [Formats](formats.md): bit depths and chroma formats.
- [Animation](animation.md): how animated content behaves and what we do about it.
- [Denoising](denoise.md): `--nr` and `--mcstf`, the two opt-in denoisers.
- [Content selection](content-selection.md): choosing settings per content type and why that isn't automatic.
- [Shot-based encoding](shot-based.md): the shot table, cut-aware keyframes and per-shot CRF.
- [Hardware mode](hardware-mode.md): `--hw videotoolbox`, the Mac's hardware encoder behind our options.
- [ffmpeg](ffmpeg.md): using yah264 from ffmpeg.
- [Live control](live-control.md): per-frame control events for live encoding (rate, CRF, QP limits, keyframes, zones), the status call and the replayable control log.

## Reference

- [Options](options.md): every CLI option, its default and how it differs from x264's.
- [Knobs](knobs.md): every `Y264_*` environment knob with its default and where the code reads it (generated).
- [Engine interface](engine-interface.md): the contract an orchestrator builds on, shared with yah265 and yaav1.

## Measuring

- [Instruments](instruments.md): the profilers, oracles and A/B harnesses that exist, by the question they answer. Read it before building a new one.
- [Corpus sources](corpus-sources.md): where the test and tuning clips come from and why the two corpora stay apart.
- [VMAF](vmaf.md): how we score quality with VMAF v1.
- [VMAF pipeline audit](vmaf-pipeline-audit.md): the check that nothing about our output depresses the VMAF score.
