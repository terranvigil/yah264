# Adaptive-effort ME: evidence-gated escalation

**Nothing here shipped, and the conclusion is negative: every cut to yah264's
motion search costs BD-rate out of proportion to the speed it buys.** The
instrumentation (`Y264_ME_STATS`) is in the tree as a permanent diagnostic; the
gates are not. Read the conclusion before touching this again.

The problem the design set out to solve: restructure the ME and mode-decision
cost model so yah264 reaches x264-medium quality at x264-medium compute. ME
plus the SATD/SAD it drives is ~50% of pure-C compute.

## 1. The design decision

Two candidate architectures:

- **ARCH-1 "hex parity":** make yah264's hex-only search reach x264 quality,
  then drop UMH and full subpel outright, matching x264's *search list*.
- **ARCH-2 "adaptive effort":** keep the escalation ladder but gate every
  expensive stage on cheap per-block evidence that the cheaper stage failed,
  matching x264's *compute* rather than its search list.

**ARCH-2 is the buildable one. ARCH-1 is dead and must not be re-attempted.**

### Why ARCH-1 is dead

The asymmetry to explain: with UMH, yah264 is at corpus BD parity with x264
(-0.17%); without it, +5.99%. x264 runs hex-only at medium and stays at parity,
so on the hex-only rung x264 extracts ~6% more BD from the same search tier.
Every mechanism that could make that a fixable constant is ruled out by direct
verification or direct implementation:

| candidate mechanism | verdict | how |
|---|---|---|
| predictor set (spatial/temporal/lowres) | equal | source-verified |
| hex geometry / iteration | equal | radius-2 hex both; 8-pt square finish built, no help, reverted |
| lowres-ME seed reach | dead | exhaustive-search ceiling test (bus 0.37 -> 0.40) |
| ME lambda value | equal | `lambda_me` tab diffed against x264's ME lambda table, all 52 entries exact |
| MV bit model | ~equal | exact Golomb vs x264's smooth `2*log2(mv+1)+0.718`; <=~1 bit apart, ~0.3% of a 16x16 SATD, not a 6% mechanism |
| RC coupling (search cost -> CRF QP) | none | `frame_complexity_me` sums LOWRES `min(lr_intra,lr_inter)`; UMH never touches it, so the QP field is search-invariant like x264's |
| RDOQ / residual chain | at parity | audited (the PSNR-vs-VMAF artifact) |
| mb-tree structure | closed | the whole-buffer port closes its ~1pt |

With no constant left to fix, the hex-rung asymmetry is **emergent**: it lives
in the interplay of mode decision, partition fallback, and bit allocation when
the integer MV is mediocre. Repeated attempts to pin it failed. The correct
response is to stop needing the answer.

### Why ARCH-2 looked right

x264's per-compute efficiency IS adaptive gating: SATD screens before RD,
early-skip detection, threshold-gated escalation everywhere. yah264's
remaining ME fat is precisely the UNCONDITIONAL loops: the UMH cross+grid runs
in full on every 16x16/16x8/8x16 search regardless of how good the hex answer
already is, and subpel iterates to convergence on every shape. The blanket cuts
fail (+5.99% UMH-off, +0.53% subpel-cap) because they cut where the work was
earning; a gate cuts where it provably isn't. The in-tree proof of the pattern
is intra screening: x264's tight 9/8 threshold cost +0.48% BD, and the loosened
2x-margin variant was BD-neutral at ~12% speed. Gates have to be tuned to
yah264's own fallback balance, measured not copied.

### The gating signal: the lowres oracle

yah264 already computes, for every MB of every frame before it is coded, a
lowres 8x8 cost pair (`lr_intra[i]`, `lr_inter[i]`) and a lowres MV
(`lr_seed_mvx/y`); they feed CRF (`frame_complexity_me`) and the P ME seed
today. That is a per-block *expected cost and expected motion* prior that x264
does not wire into its escalation decisions. The gates condition on it:

- hex lands near the oracle MV at near the oracle cost -> the surface is
  understood, escalation cannot pay, skip;
- hex is far from the oracle or costs multiples of it -> suspicious surface
  (occlusion, zoom divergence, lowres blur) -> spend the UMH.

That concentrates BD damage exactly where the gate keeps spending, and makes
the skip rate content-adaptive for free: akiyo almost always skips, bus rarely
does (bus is WHERE UMH earns).

## 2. The gates as designed

**Instrumentation first (`Y264_ME_STATS`, no behavior change).** Per-search
counters, dumped per clip: stage reached; probes per stage; bcost after seeds /
after hex / after UMH; UMH improvement (delta, |mv| distance); subpel
iterations used vs gain per iteration; hex-vs-`lr_seed` MV distance and
bcost-vs-`lr_inter` ratio distributions. Thresholds come from these
distributions, not from x264.

**G1, UMH admission gate.** Skip the UMH cross+grid when BOTH
`hex_bcost <= alpha * oracle_cost(i)` (oracle scaled lowres->fullres, alpha
loose) and `|hex_mv - lr_seed_mv| <= r`. Fallbacks (partition split, intra
admit) stay intact. Env `Y264_ME_GATE` bit 0.

**G2, subpel iteration budget.** Replace fixed-convergence subpel loops with
improvement-driven continuation (stop when an iteration's gain is below a
lambda-scaled epsilon); full budget for the winning shape, capped for losers.
Env bit 1.

**G3, partition UMH inherits G1.** 16x8/8x16 run UMH only when the 16x16
outcome was NOT oracle-adequate. Env bit 2.

## 3. What the instrumentation measured, and why the fork is parked

`Y264_ME_STATS` instrumented the real search over the 5-clip corpus (replicated
independently on 7 clips plus 720p at medium and tf8). The design's ~15-20%
estimate was wrong in both directions, and the net is a NEGATIVE result.

1. **UMH is bigger than the plan assumed: 28-37% of pure-C.** `Y264_NO_UMH=1`
   reads bus -27.7%, mobile -36.8%. The earlier "~13%" was measured after
   `me_small_noumh`, which already skips 8x8 UMH by default; the
   16x16 + 16x8 + 8x16 UMH is the 28-37%. So the prize LOOKS large. It is not
   capturable BD-safely.

2. **The lowres oracle is a weak UMH-value predictor, so G1 is dead.** The
   joint histogram (hex_bcost/oracle x |hex_mv - oracle_mv|, per-cell
   UMH-improved rate) shows UMH improving bcost on 40-58% of motion-clip
   searches, spread across cells, NOT concentrated where the oracle flags
   inadequacy. There is no BD-safe high-skip region: >40% skip needs
   ratio<=1.25 at any distance (55% skip), and 41% of those are blocks UMH
   would improve, mean gain ~290 SATD. The clean 16x16-ref0 scope is only ~4%
   of all UMH runs (60k of ~1.36M); the bulk is partitions and B.

3. **Subpel already self-limits, so G2 is dead.** hpel and qpel converge in <=2
   iterations 96-99% of the time, so a convergence cap saves ~0.3%, not 4-6%.

4. **Behaviour-matched partition early-termination costs BD, so G3 is dead.**
   Implemented exactly as x264 does it (8x8 first, search 16x8/8x16 only when
   8x8 cost < 16x16 cost + threshold; env `Y264_PART_ET`, subme<=8). Speed is
   only ~1-2% corpus-wide (foreman 2.8% best; akiyo 0%, since its MBs are
   P_SKIP so no partition search runs; bus/mobile ~1%, since 8x8 beats 16x16 on
   detail so the gate rarely fires). BD-rate VMAF-NEG on a 5-point sweep:
   foreman +0.16, mobile +0.54, stefan +0.47, coastguard +0.36, mean +0.38%,
   with mobile breaching the +0.5%/clip invariant. Retuning the slack trades BD
   for speed 1:1 (bigger slack = the gate fires less = less BD AND less speed),
   so there is no BD-safe point worth having. Reverted; the default is
   byte-identical.

5. **Diamond subpel is BD-dead.** `Y264_SUBPEL=1` (diamond to convergence)
   against the default square, 5-point crf30-46, 120f: foreman **+1.67%
   VMAF-NEG**, mobile +0.51%. The 8-neighbour square earns its probes. **Do not
   revisit diamond in any form.**

**The unified finding, measured four ways:** yah264's UMH plus full partition
search is load-bearing for its coding efficiency. Every cut (blanket UMH-off
+6% BD, blanket 16x8/8x16-off +0.62%, behaviour-matched early-term +0.38%,
oracle-gated with a weak signal) costs BD out of proportion to the speed,
because yah264 gets less quality per ME candidate than x264 and compensates
with search. This is the same emergent per-compute inefficiency the ARCH-1
audit named, and it is not gateable away.

The op-count ledger says the same thing from the other side: at medium against
medium, yah264's residual and entropy chain is at COUNT PARITY with x264 (dct
0.8x, quant 0.7x, est-bins 1.2x), and the entire count gap is ME probes (SAD-pix
3.8-4.5x, SATD-pix 5.5x). That probe volume is quality-buying, and the ledger
is the progress meter for whatever attacks it next.

**One variant was never tested:** in-search dead-ring termination, stopping the
multi-hex grid after k consecutive gainless rings (42-56% of UMH runs gain
nothing from all ~120 probes). The unified finding is a strong prior against it
(the partition early-term's slack traded BD 1:1). If ever attempted, build ONE
bounded version under the standard gates and expect to park it.

## 4. Honest arithmetic, and what this fork is NOT

The plan's own estimate was: ME plus its driven metric ~50% of compute,
realistic gated recovery ~15-20% of total pure-C (G1 8-11%, G2 4-6%, G3 2-4%,
overlapping), taking the gap from ~3.3x to ~2.5-2.7x quality-neutral. The
blanket-mimic floor (2.1x, +8% BD) is the ceiling of perfect gating. The
measurements above retire that estimate: the recoverable share is near zero at
constant quality.

This fork was never going to reach 1.0x on its own. The residual ~2.1-2.5x is
the "remains even at cheap work" bucket: per-MB structure, cache layout (fenc
stride-16), est-path overheads. That is a separate, mechanical, BD-neutral
program, and it is the better next investment.

Related design: `docs/satd-decide-design.md` (candidate discipline).
