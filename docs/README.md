# yah264 docs

Each subsystem doc says what yah264 does now and why, what we tried that
didn't work, and what's still open. Current speed and quality numbers live on
the [results page](https://terranvigil.github.io/yah264/results.html).

## Start here

- [plan.md](plan.md): what yah264 is, the goals and bar it's measured against, where it stands and what's open.
- [how-h264-works.html](how-h264-works.html): a walk through H.264 encoding for readers new to the codec.
- [what-we-dont-do.md](what-we-dont-do.md): x264 options and features we refuse, with the reason for each.

## How it works

- [rate-control.md](rate-control.md): CQP, CRF, ABR, VBV and two-pass, and which one fits which job.
- [lookahead-and-aq.md](lookahead-and-aq.md): the lookahead, mb-tree, adaptive quantization and scene cuts.
- [b-frames.md](b-frames.md): B-frame placement, the pyramid and direct modes.
- [motion-and-modes.md](motion-and-modes.md): motion search, partitions and the mode decision.
- [threading.md](threading.md): how the encoder splits work across cores and picks a thread count.
- [threading.html](threading.html): the threading model as an illustrated site page.
- [simd.md](simd.md): the NEON, SVE, SSE4.2, AVX2 and AVX-512 kernels and how they're checked.
- [gpu.md](gpu.md): what GPU and matrix-unit offload was measured, and why the encoder stays on the CPU.
- [formats.md](formats.md): bit depths and chroma formats.
- [animation.md](animation.md): how animated content behaves and what we do about it.
- [denoise.md](denoise.md): `--nr` and `--mcstf`, the two opt-in denoisers.
- [content-selection.md](content-selection.md): choosing settings per content type, and why it isn't automatic.
- [shot-based.md](shot-based.md): the shot table, cut-aware keyframes and per-shot CRF.
- [hardware-mode.md](hardware-mode.md): `--hw videotoolbox`, the Mac's hardware encoder behind our options.
- [ffmpeg.md](ffmpeg.md): using yah264 from ffmpeg.
- [live-control.md](live-control.md): the planned per-frame control plane for live encoding.
- [stream-provenance.md](stream-provenance.md): the identification string and the stream fingerprint.

## Reference

- [options.md](options.md): every CLI option, its default and how it differs from x264's.
- [knobs.md](knobs.md): every `Y264_*` environment knob with its default and where the code reads it (generated).
- [engine-interface.md](engine-interface.md): the contract an orchestrator builds on, shared with yah265 and yaav1.

## Measuring

- [instruments.md](instruments.md): the profilers, oracles and A/B harnesses that exist, by the question they answer. Read it before building a new one.
- [corpus-sources.md](corpus-sources.md): where the test and tuning clips come from, and why the two corpora stay apart.
- [vmaf-pipeline-audit.md](vmaf-pipeline-audit.md): the check that nothing about our output depresses the VMAF score.
