# High bit depth (10-bit / High 10), architecture & phased plan

10-bit lands before 4:2:2/4:4:4. 10-bit is the only breadth feature that changes
the *pixel type* across the hot path, so it must land before the SIMD/speed
optimization pass, or that SIMD gets rewritten. The recon-match gate is the
safety net.

## Non-negotiable invariant

**8-bit output stays byte-identical at every step.** The abstraction goes in
first with `pixel = uint8_t`; conformance (recon-match + thread determinism +
`cmp` of .264) stays green and identical throughout the refactor. 10-bit is only
*enabled* once the abstraction is complete. That lets the large mechanical
change land safely, one subsystem at a time.

## Architecture: compile-time `pixel` typedef (x264-style)

- `src/common/bitdepth.h`: `typedef uint8_t pixel` at `N264_BIT_DEPTH 8`,
  `uint16_t` at 10/12. `PIXEL_MAX = (1<<BD)-1`; `clip_pixel(x)`. The header is
  named `bitdepth.h` rather than `pixel.h` to avoid a guard collision with the
  DSP `pixel.h`.
- Bit depth is **compile-time** (like libx264's per-depth build), not runtime.
  That keeps the hot path branch-free and lets SIMD specialize. Two library
  variants (8-bit default; 10-bit) selected by a meson option; the CLI
  dispatches.
- **Only true pixels become `pixel`.** `uint8_t` is also used for bitstream
  bytes, nnz counts, mode ids, and flags; those stay `uint8_t`. Every conversion
  is a judgment call, NOT a blind sed. (Surface: ~440 uses across 27 files, but a
  large fraction are non-pixel.)

## The two genuinely hard spots (everything else is mechanical)

1. **Transform intermediates.** `n264_fdct*/idct*` use `int16_t` intermediates.
   At 10-bit the residual is ±1023 and the butterfly sums overflow int16. The
   coefficient arrays (`dctcoef`) and DCT intermediates widen to `int32_t` for
   BD>8 (x264 does exactly this). Quant/dequant clamp ranges widen too.
2. **NEON kernels.** SAD/SATD/MC/quant/dequant/(future deblock) NEON is
   8-bit-specific (`uint8x16`). For 10-bit: 16-bit NEON variants, or scalar
   fallback gated by bit depth. Start with **scalar fallback for 10-bit** for
   correctness and conformance first, and add 16-bit NEON in the optimization
   phase.

## Other 10-bit specifics (mechanical once the above are in)

- **QP**: `QpBdOffsetY = 6*(BD-8)` = 12 at 10-bit; QP range shifts to [-12, 51].
  lambda tables, chroma-QP mapping, and deblock indexA/B all key off the
  BD-adjusted QP.
- **SPS**: `profile_idc = 110` (High 10); `bit_depth_luma/chroma_minus8 = 2`;
  `qpprime_y_zero_transform_bypass_flag` for lossless. PPS unchanged.
- **Clip** everywhere pixels are reconstructed: [0, PIXEL_MAX].
- **I/O**: Y4M/YUV read/write handles 16-bit (little-endian) samples;
  `--dump-recon` and the conformance decode/compare go 16-bit.
- **CABAC/CAVLC residual**: coeff magnitude ranges widen (larger `coeff_abs`),
  but the coding logic is bit-depth-agnostic, so mostly free.

## Phase plan (each phase gates: 8-bit byte-identical + conformance green)

Phases A-C are complete. `bit_depth=10` builds and produces conformant High 10
(yuv420p10le) streams; 8-bit stays byte-identical. Recon-match vs ffmpeg High 10
passes across CAVLC/CABAC/8x8/bframes plus a QP sweep 18-44, and is
thread-deterministic. D and E are deferred to the optimization pass.

- **A. Foundation** (done): `src/common/bitdepth.h` with the typedef,
  `PIXEL_MAX`, `clip_pixel`, `N264_BIT_DEPTH`. No behavior change.
- **B. Pixel-type the hot path** (done): convert genuine-pixel `uint8_t` →
  `pixel` in DSP (mc, pixel, predict), then encoder (macroblock, deblock, me,
  encoder). Widen transform intermediates to a `dctcoef`/`dctsum` type (int16 at
  8-bit, int32 at BD>8). Gate byte-identical at 8-bit after each subsystem.
- **C. Enable 10-bit build** (done): meson `bit_depth` option; SPS High 10
  signaling; QP-bd-offset; clip ranges; scalar DSP for 10-bit. Gate: recon-match
  vs ffmpeg High 10 on a 10-bit corpus.
- **D. 10-bit corpus + BD**: add 10-bit test clips; direct BD vs x264 High 10.
- **E. 16-bit NEON** for the 10-bit hot path, in the optimization phase.

Then 4:2:2 / 4:4:4 (separate effort): chroma block geometry (8x16 / 16x16),
chroma DC transforms (2x4 / 4x4), CABAC ctxBlockCat for 4:4:4, deblock. None of
which touch the luma pixel type, so they do not block luma SIMD.

## Why this order serves the optimization goal

The pixel-type and transform-intermediate widening (Phase B) is the ONLY change
that would invalidate 8-bit SIMD. Landing it first (byte-identical) means the
subsequent speed pass writes SIMD once, per depth, against a stable type layer.
