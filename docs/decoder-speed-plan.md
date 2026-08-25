# Decoder speed track

Make the decoder as fast as the best. This file is the plan; the track is not
started.

## Current state

The in-tree decoder exists for VERIFICATION, not speed: `n264_cabac_dec_*`
(cabac.c) plus the recon-match path decode the encoder's own output to prove
conformance. It has never been benchmarked as a decoder and was not built to be
one. There is no standalone decode CLI.

## The bar

"As fast as the best" means measured against the leaders on their own turf:

- **ffmpeg's h264 decoder** (the deployed default everywhere), threaded.
- **dav1d-style engineering** as the quality reference for what a hand-tuned
  decoder looks like. dav1d is AV1, but its architecture (per-frame and
  per-tile threading, tight asm coverage) is the model, and there is a local
  dav1d tree to study.

A decoder board mirrors the encoder's: multiple = leader-speed / ours over a
clip set, single- and multi-threaded, pure-C and SIMD tiers, at several
bitrates (decode cost scales with bitrate, unlike encode).

## Plan sketch (to be sized before any build)

1. **Standalone decode path**: a `--decode` CLI mode (Annex-B in, Y4M out)
   over the existing verification decoder, so it can be timed at all.
2. **Baseline board**: ours vs `ffmpeg -threads N` on the corpus at 3 rates.
   Expect to be far off; the verification decoder is bit-accurate and naive.
3. **Attribution before building**: where does decode time go, in the CABAC
   decode loop, MC interpolation, deblock, or reconstruction?
4. **The known big rocks, in likely order**: CABAC decode is serial per slice
   (the encoder's bit-estimate work does not transfer; decode is the
   arithmetic, not the pricing); MC and deblock SIMD can reuse the encoder's
   NEON kernels where the operation is shared; then the frame/slice threading
   model.
5. **Gates**: conformance stays bit-exact (framemd5 vs ffmpeg's decode of the
   same streams); the determinism battery extends to decode.
