# Where yah264 beats x264 (measured)

Every claim below is measured. Each names the harness that produced it and says
so when that harness is local rather than in this repository.

1. **Speed and compression at parity and past it.** On the Apple M5 Max, yah264
   finishes faster than x264 `--preset medium` at the same file size in all 27
   gating groups (CIF to 4K; short clips, 2-minute segments and 10-minute
   excerpts) at all three goals, with the same or better quality. Over 52 rate
   ladders on real titles, at CRF and capped VBR, it needs a median 13% fewer
   bits for the same VMAF-NEG, and only one ladder is behind. Source: the parity
   board and ladders, with every cell in `site/results.md`. The lead still
   depends on the content and the rate, so a compression claim names its ladder
   or band.
2. **CABAC throughput.** The byte-queue arithmetic engine runs 2.51x per bin
   over the engine it replaced, byte-identical. That figure is ours against our own previous engine. We list this as a real
speedup and don't claim it as a lead over anyone.
3. **Intrinsics, not assembly.** Every SIMD kernel is C intrinsics, held
   bit-exact to its C reference by checkasm. The whole-encode effect of SIMD
   is on the results page, measured with assembly on and off on both sides.
4. **Leaner motion-compensation architecture.** The DPB is pointer-rotated and
   its half-pel planes travel with the buffer, so a reference is never copied.
5. **Trellis**: on the committed winner only (the documented `--trellis 1` meaning), with a Viterbi that scales with the coded
   coefficient count instead of the block size. It is byte-identical and
   golden-gated.
6. **Verification as a feature.** We have a clean recon-match conformance run,
   a TSan floor of zero and per-feature byte-identity escapes. The check count
   is built at run time, so quote what `scripts/conformance.sh` prints instead
   of a number written down here. Output determinism is claimed only at one
   thread. Repeatable byte-exact output isn't required
   for threaded encodes, and their determinism checks report differences
   without failing on them.

Bitstream identity across thread counts isn't on this list, and yah264 doesn't
guarantee it. It cost more in unreachable multi-thread speed than it was worth.
Output may legitimately vary by thread count where that buys real throughput.
