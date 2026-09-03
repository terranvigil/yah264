# Where yah264 beats x264 (measured)

All claims below are measured. Each says which harness produced it, and where the
harness is local rather than in this repository, it says so.

1. **Quality, and only in a stated rate range**: at low bitrate, BD-rate
   (VMAF-NEG) ahead of x264 `--preset medium` on 9 of the 10 board clips, median
   around -12%. The lead narrows as bitrate rises and is gone at the top of the
   range, so a compression claim from this project has to name the band it came
   from. Ten-clip board, 2026-09-02; the published figures are in
   `site/results.md`. The single undated "-2.76% over 7 clips" this line used to
   carry averaged across bands and is not quotable.
2. **CABAC throughput**: the byte-queue arithmetic engine runs 2.51x per bin over
   the engine it replaced, byte-identical. That figure is ours against our own
   previous engine. x264 does ship its own aarch64 assembly for the arithmetic
   coder, carrying about 1.75% of its self time, so whether our C beats it on
   this architecture is open and unmeasured. Listed here as a real speedup, not
   as a lead.
3. **Kernels level with hand-written asm on Apple Silicon.** A 40-kernel
   head-to-head on 2026-09-02 found our NEON intrinsics tie hand asm on every
   shape both sides implement, so the earlier "faster than" claim for sa8d,
   satd 16x16, batched sad_x4 and quant_8x8 overstated it. What separates the
   two is coverage, not the instruction level. The harness is local and the
   per-kernel nanosecond figures are not reproducible from this repository.
4. **Leaner motion-compensation architecture**: a pointer-rotated DPB whose
   half-pel planes travel with the buffer, so a reference is never copied. The
   op-ledger ratio this line used to quote against x264 needs a locally patched
   baseline build, which this repository cannot carry, so it is not reproducible
   here either.
5. **Trellis**: x264's placement plus a Viterbi that scales with the coded
   coefficient count rather than the block size, byte-identical, golden-gated.
6. **Verification as a feature**: a clean recon-match conformance run (the check
   count is built at run time, so quote what `scripts/conformance.sh` prints
   rather than a number written down here), TSan floor
   zero, per-feature byte-identity escapes, and a reproducibility (golden)
   gate x264 has no equivalent of. This is single-run reproducibility, the same
   input, config and thread count always producing the same output, not
   cross-thread-count bitstream identity.

Bitstream identity across thread counts is not on this list and is not a
guarantee yah264 makes. It cost more in unreachable multi-thread speed than it
was worth, so yah264 follows x264's model: output may legitimately vary by
thread count where that buys real throughput. That is what makes x264's
thread-scaled MV clamp available to build.
