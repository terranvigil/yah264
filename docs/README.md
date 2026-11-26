# yah264 docs

Each subsystem doc covers what yah264 does and why, what didn't work, and
what's open. Current numbers are on the [results page](https://terranvigil.github.io/yah264/results.html).

## Start here

- [Plan](plan.md): what yah264 is, its goals, status and open work.
- [How H.264 works](how-h264-works.html): H.264 encoding for newcomers.
- [What we don't do](what-we-dont-do.md): x264 options we refuse, and why.

## Subsystems

- [Rate control](rate-control.md): CQP, CRF, ABR, VBV and two-pass.
- [Lookahead and AQ](lookahead-and-aq.md): lookahead, mb-tree, adaptive quantization, scene cuts.
- [B-frames](b-frames.md): B-frame placement, the pyramid, direct modes.
- [Motion and modes](motion-and-modes.md): motion search, partitions, mode decision.
- [Threading](threading.md): splitting work across cores.
- [Threading, illustrated](threading.html): the threading model, with diagrams.
- [SIMD](simd.md): the NEON, SVE, SSE4.2, AVX2 and AVX-512 kernels and their checks.
- [GPU](gpu.md): why the encoder stays on the CPU.
- [Formats](formats.md): bit depths and chroma formats.
- [Animation](animation.md): animated content.
- [Denoising](denoise.md): `--nr` and `--mcstf`, the two opt-in denoisers.
- [Content selection](content-selection.md): settings per content type.
- [Shot-based encoding](shot-based.md): the shot table, cut-aware keyframes, per-shot CRF.
- [Hardware mode](hardware-mode.md): `--hw videotoolbox`, the Mac's hardware encoder.
- [ffmpeg](ffmpeg.md): using yah264 from ffmpeg.
- [Live control](live-control.md): per-frame control events for live encoding.

## Reference

- [Options](options.md): each CLI option, its default and how it differs from x264's.
- [Knobs](knobs.md): the `Y264_*` environment knobs (generated).
- [Engine interface](engine-interface.md): the orchestrator contract shared with yah265 and yaav1.

## Measuring

- [Instruments](instruments.md): existing measurement tools, by question. Read it before building one.
- [Corpus sources](corpus-sources.md): where test and tuning clips come from.
- [VMAF](vmaf.md): how we score quality with VMAF v1.
- [VMAF pipeline audit](vmaf-pipeline-audit.md): the check that nothing in our output depresses VMAF.
