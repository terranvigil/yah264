# Where yah264 beats x264 (measured)

Every claim below is measured. Each names the harness that produced it and says
so when that harness is local rather than in this repository.

1. **Quality, and only in a stated rate range.** At low bitrate we're ahead of
   x264 `--preset medium` in BD-rate (VMAF-NEG) on 9 of the 10 board clips, with
   a median around -12%. The lead narrows as bitrate rises and is gone at the
   top of the range. A compression claim from this project therefore has to
   name the band it came from. Source: the ten-clip board, 2026-09-02, with the
   published figures in `site/results.md`. This line used to quote an undated
   "-2.76% over 7 clips". It averaged across bands and isn't quotable.
2. **CABAC throughput.** The byte-queue arithmetic engine runs 2.51x per bin
   over the engine it replaced, byte-identical. That figure is ours against our
   own previous engine. x264 ships its own aarch64 assembly for the arithmetic
   coder. That code takes about 1.75% of its self time. Whether our C beats it on
   this architecture is open and unmeasured. We list this as a real speedup and
   don't claim it as a lead.
3. **Kernels level with hand-written asm on Apple Silicon.** A 40-kernel
   head-to-head on 2026-09-02 found that our NEON intrinsics tie hand asm on
   every shape both sides implement. The earlier "faster than" claim for sa8d,
   satd 16x16, batched sad_x4 and quant_8x8 overstated it. What separates the
   two is coverage, not the instruction level. The harness is local, so the
   per-kernel nanosecond figures can't be reproduced from this repository.
4. **Leaner motion-compensation architecture.** The DPB is pointer-rotated and
   its half-pel planes travel with the buffer, so a reference is never copied.
   This line used to quote an op-ledger ratio against x264. That ratio needs a
   locally patched baseline build this repository can't carry, so it isn't
   reproducible here either.
5. **Trellis**: x264's placement plus a Viterbi that scales with the coded
   coefficient count instead of the block size. It is byte-identical and
   golden-gated.
6. **Verification as a feature.** We have a clean recon-match conformance run,
   a TSan floor of zero and per-feature byte-identity escapes. The check count
   is built at run time, so quote what `scripts/conformance.sh` prints instead
   of a number written down here. Output determinism is claimed only at one
   thread. Since 2026-09-25 (owner), repeatable byte-exact output isn't required
   for threaded encodes, and their determinism checks report differences
   without failing on them.

Bitstream identity across thread counts isn't on this list, and yah264 doesn't
guarantee it. It cost more in unreachable multi-thread speed than it was worth.
yah264 follows x264's model instead: output may legitimately vary by thread
count where that buys real throughput. That is what makes x264's thread-scaled
MV clamp available to build.
