# Decoder speed track

Make the decoder as fast as the best. This file is the plan. The track hasn't
started.

## Current state

The in-tree decoder exists for VERIFICATION, not speed. `y264_cabac_dec_*`
(cabac.c) and the recon-match path decode the encoder's own output to prove
conformance. It wasn't built to be a decoder and nobody has benchmarked it as
one. There is no standalone decode CLI.

## The bar

"As fast as the best" means measured against the leaders on their own turf:

- **ffmpeg's h264 decoder**, threaded. It's the deployed default everywhere.
- **dav1d-style engineering** as the reference for what a hand-tuned decoder
  looks like. dav1d is AV1. Its architecture is still the model: per-frame and
  per-tile threading with tight asm coverage. There is a local dav1d tree to
  study.

A decoder table mirrors the encoder's. The multiple is leader-speed / ours over
a clip set, single- and multi-threaded, in pure-C and SIMD tiers. It runs at
several bitrates because decode cost scales with bitrate, unlike encode.

## Plan sketch (to be sized before any build)

1. **Standalone decode path**: a `--decode` CLI mode (Annex-B in, Y4M out)
   over the existing verification decoder, so it can be timed at all.
2. **Baseline table**: ours vs `ffmpeg -threads N` on the corpus at 3 rates.
   We expect to be far off, since the verification decoder is bit-accurate and
   naive.
3. **Attribution before building**: where does decode time go? The candidates
   are the CABAC decode loop, MC interpolation, deblock and reconstruction.
4. **The known big rocks, in likely order.** CABAC decode is serial per slice.
   The encoder's bit-estimate work doesn't transfer, because decode runs the
   arithmetic where the encoder only prices it. MC and deblock SIMD can reuse
   the encoder's NEON kernels where the operation is shared. The frame/slice
   threading model comes last.
5. **Gates**: conformance stays bit-exact, checked by framemd5 against ffmpeg's
   decode of the same streams. The determinism battery extends to decode.
