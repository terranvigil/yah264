# Where next264 beats x264 (measured)

All claims below are measured on this repo's harnesses.

1. **Quality**: -2.76% BD-rate (VMAF-NEG) ahead of x264 `--preset medium` at
   matched settings over the 7-clip corpus (akiyo -13.6%, foreman -8.4%,
   coastguard -6.2%; bus +10.5% is the one open deficit).
2. **CABAC throughput**: the byte-queue arithmetic engine runs 2.51x per bin over
   the engine it replaced, byte-identical. That figure is ours against our own
   previous engine. x264 does ship aarch64 CABAC assembly
   (`cabac_encode_decision_asm` and two siblings, selected at compile time,
   carrying about 1.75% of its self time), so whether our C beats their asm on
   this architecture is open and unmeasured. Listed here as a real speedup, not
   as a lead.
3. **Kernels faster than x264's hand asm on Apple Silicon** (same pinned
   harness): sa8d 8x8/16x16, satd 16x16, batched sad_x4, quant_8x8.
4. **Leaner motion-compensation architecture**: 0.1-0.37x x264's pixel volume
   on reference fetch / averaging / MC (op-ledger), with a pointer-rotated
   DPB whose half-pel planes travel with the buffer (zero reference copies).
5. **Trellis**: x264's placement plus a Viterbi that scales with the coded
   coefficient count rather than the block size, byte-identical, golden-gated.
6. **Verification as a feature**: 468/468 recon-match conformance, TSan floor
   zero, per-feature byte-identity escapes, and a reproducibility (golden)
   gate x264 has no equivalent of. This is single-run reproducibility, the same
   input, config and thread count always producing the same output, not
   cross-thread-count bitstream identity.

Bitstream identity across thread counts is not on this list and is not a
guarantee next264 makes. It cost more in unreachable multi-thread speed than it
was worth, so next264 follows x264's model: output may legitimately vary by
thread count where that buys real throughput. That is what makes x264's
thread-scaled MV clamp available to build.
