# High bit depth (10-bit / High 10), architecture & phased plan

10-bit ships before 4:2:2/4:4:4. It is the only breadth feature that changes
the *pixel type* across the hot path. It must therefore ship before the
SIMD/speed optimization pass, or that SIMD gets rewritten. The recon-match gate
is the safety net.

## Non-negotiable invariant

**8-bit output stays byte-identical at every step.** The abstraction goes in
first with `pixel = uint8_t`. Conformance (recon-match + thread determinism +
`cmp` of .264) stays green and identical throughout the refactor. 10-bit is only
*enabled* once the abstraction is complete. That lets the large mechanical
change go in safely, one subsystem at a time.

## Status, 2026-09-16 (item C3-10bit)

Phases A-D are done. E is still deferred. The depth is a compile-time type
inside the encoder and always will be. It is no longer a BUILD choice for the
user, though. Every build directory compiles BOTH libraries. One binary
links both and picks the encoder from the input's Y4M `C` tag. `--output-depth 10`
puts 8-bit content through High 10; `--output-depth 8` on 10-bit input is
refused. 8-bit output is byte-identical to the single-library build it
replaced, over the ten board clips at CRF, CQP and ABR at one and eight
threads. 10-bit output is byte-identical to the old `-Dbit_depth=10` build.

Phase D read a first 10-bit board (docs/data/board10-2026-09-16.md). It says
the thing worth knowing about the 10-bit path: **it is far behind its own
8-bit sibling in coding efficiency.** Against x264 High 10 on eight BVI-AOM
clips the median is about +21% BD-rate, with the worst clip at +59.6%. The
SAME two clips at 8 bits, with the same harness and rate points, read +3.5% and
+0.3%. PSNR confirms it independently (2.3 dB behind at 8% fewer bytes on one
cell), so it is not a metric artifact. The place to look is the
depth-dependent constants: QP domain, lambda, the RC constants and the quant
tables. Nothing in the coding tools is depth-specific, which is why the gap is
a defect and not a property. It is not a regression either, since that path
was never measured before. It is recorded here as the first baseline and the
next 10-bit item's brief.

## Architecture: compile-time `pixel` typedef (x264-style)

- `src/common/bitdepth.h`: `typedef uint8_t pixel` at `Y264_BIT_DEPTH 8`,
  `uint16_t` at 10/12. `PIXEL_MAX = (1<<BD)-1`; `clip_pixel(x)`. The header is
  named `bitdepth.h` to avoid a guard collision with the DSP `pixel.h`.
- Bit depth is **compile-time** inside the library. That keeps the hot path
  branch-free and lets SIMD specialize. Two library variants are compiled
  ALWAYS (item C3-10bit) instead of being selected by a meson option. Both
  install. The
  public header renames a caller's entry points to the suffixed ones at
  `Y264_BIT_DEPTH` 10. The CLI and the ffmpeg wrapper link both and dispatch on
  the input's sample width. The meson option still selects the depth the tests
  and in-tree tools are built at.
- **Only true pixels become `pixel`.** `uint8_t` is also used for bitstream
  bytes, nnz counts, mode ids, and flags. Those stay `uint8_t`. Every
  conversion is a judgment call, NOT a blind sed. The surface is ~440 uses
  across 27 files, a large fraction of them non-pixel.

## The two genuinely hard spots (everything else is mechanical)

1. **Transform intermediates.** `y264_fdct*/idct*` use `int16_t` intermediates.
   At 10-bit the residual is ±1023 and the butterfly sums overflow int16. The
   coefficient arrays (`dctcoef`) and DCT intermediates widen to `int32_t` for
   BD>8 (x264 does exactly this). Quant/dequant clamp ranges widen too.
2. **NEON kernels.** SAD/SATD/MC/quant/dequant/(future deblock) NEON is
   8-bit-specific (`uint8x16`). 10-bit needs either 16-bit NEON variants or
   a scalar fallback selected by bit depth. Start with **scalar fallback for
   10-bit** to get correctness and conformance first. Add 16-bit NEON in the
   optimization phase.

## Other 10-bit specifics (mechanical once the above are in)

- **QP**: `QpBdOffsetY = 6*(BD-8)` = 12 at 10-bit; QP range shifts to [-12, 51].
  lambda tables, chroma-QP mapping, and deblock indexA/B all key off the
  BD-adjusted QP.
- **SPS**: `profile_idc = 110` (High 10); `bit_depth_luma/chroma_minus8 = 2`;
  `qpprime_y_zero_transform_bypass_flag` for lossless. PPS unchanged.
- **Clip** everywhere pixels are reconstructed: [0, PIXEL_MAX].
- **I/O**: Y4M/YUV read/write handles 16-bit (little-endian) samples;
  `--dump-recon` and the conformance decode/compare go 16-bit.
- **CABAC/CAVLC residual**: coeff magnitude ranges widen (larger `coeff_abs`).
  The coding logic is bit-depth-agnostic, so this is mostly free.

## Phase plan (each phase gates: 8-bit byte-identical + conformance green)

Phases A-C are complete. `bit_depth=10` builds and produces conformant High 10
(yuv420p10le) streams, and 8-bit stays byte-identical. Recon-match vs ffmpeg
High 10 passes across CAVLC/CABAC/8x8/bframes plus a QP sweep 18-44. It is also
thread-deterministic. D and E are deferred to the optimization pass.

- **A. Foundation** (done): `src/common/bitdepth.h` with the typedef,
  `PIXEL_MAX`, `clip_pixel`, `Y264_BIT_DEPTH`. No behavior change.
- **B. Pixel-type the hot path** (done): convert genuine-pixel `uint8_t` →
  `pixel` in DSP (mc, pixel, predict), then encoder (macroblock, deblock, me,
  encoder). Widen transform intermediates to a `dctcoef`/`dctsum` type (int16 at
  8-bit, int32 at BD>8). Gate byte-identical at 8-bit after each subsystem.
- **C. Enable 10-bit build** (done): meson `bit_depth` option; SPS High 10
  signaling; QP-bd-offset; clip ranges; scalar DSP for 10-bit. Gate: recon-match
  vs ffmpeg High 10 on a 10-bit corpus.
- **D. 10-bit corpus + BD** (done, 2026-09-16): eight BVI-AOM clips staged as
  `yuv420p10le` Y4M (`CLIPS_P10` in scripts/parity-clips.sh), scored by
  `scripts/board10.sh` against x264 High 10. The median reads about +21%
  BD-rate on VMAF-NEG. An 8-bit control on the same clips reads +3.5% / +0.3%,
  which says the gap belongs to the depth and not the content.
- **E. 16-bit NEON** for the 10-bit hot path, in the optimization phase. Still
  deferred. The board above says the efficiency gap should be closed first,
  since NEON would only make a worse encoder faster.

Then comes 4:2:2 / 4:4:4 as a separate effort: chroma block geometry (8x16 /
16x16), chroma DC transforms (2x4 / 4x4), CABAC ctxBlockCat for 4:4:4, and
deblock. None of those touch the luma pixel type, so they do not block luma
SIMD.

## Why this order serves the optimization goal

The pixel-type and transform-intermediate widening (Phase B) is the ONLY change
that would invalidate 8-bit SIMD. With it in first (byte-identical), the
subsequent speed pass writes SIMD once per depth, against a stable type layer.
