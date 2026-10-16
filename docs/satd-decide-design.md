# SATD-decide restructure: pure-C speed parity with x264 medium

This plan closes the pure-C speed gap by matching x264's *candidate
discipline*: decide by SATD, run RD only on survivors, screen intra instead of
encoding it, and keep trellis off the trial path. Each workstream is gated per
section 7. A workstream that fails is reverted.

## 1. Objective and bars

**Goal:** pure-C `yah264 --preset medium` matches pure-C `x264 --preset medium`
(matched `--ref 1 --bframes 2 --cabac --transform-8x8`, threads 1) on
wall-clock, **without lower quality than x264**.

Two bars, decoupled:

- **Speed target:** corpus geomean wall-clock ratio (yah264/x264, both scalar,
  `--crf`, `scripts/perf-comp-purec.sh` protocol) driven to **<= 1.0x**.
- **Quality invariant (per step):** for any shipped step, 7-clip VMAF-NEG BD vs
  x264 medium must not *increase* from the baseline by more than **+0.10%
  mean**. No single clip may rise by more than **+0.5%**. Every step also needs
  recon-match green, subme>=9 byte-identical to HEAD, the threaded==serial
  canary green and the unit suite green.

The decoupling matters. If the baseline shows BD margin (mean below 0), that
margin may be spent on the two known speed-for-quality trades (A3, A5) as long
as the mean stays <= 0. If it shows a deficit, those trades are frozen and only
the quality-neutral workstreams run. The deficit itself is mb-tree and motion
territory. It's outside this program's scope and must not be double-counted
against speed work.

**Preset semantics:** the target is `--preset medium` itself (the subme 7
band). There's no new preset. The subme>=9 default (max quality) stays
byte-identical throughout. Every behavior change is `subme<=8`-gated with an
env escape.

## 2. x264-medium anatomy (subme 7 -> RD level 1, trellis 1)

Every claim source-checked:

1. **Decide by SATD, RD the survivors.** P: SATD analysis ranks
   {16x16(+refs), 8x8, 16x8, 8x16} plus SATD-screened intra. Then the P RD
   stage runs the full-MB RD cost (a full MB encode: transform, **deadzone**
   quant, recon, CABAC size) only on candidates with SATD < `its internal state*5/4+1`.
   16x16 is always admitted. The transform-size decision follows on the winner
   only, then `intra_rd` at the same `5/4+1` threshold, which usually means
   zero intra RDs. Typical: **2-4 RD encodes per MB.** B has the same
   structure via its B RD stage with threshold `*17/16+1`. Direct is gated by
   direct cost <= best cost * 33/32.
2. **No trellis in RD trials.** Trellis runs in RD trials only above trellis
   level 1 at its RD level. So at medium (trellis=1) **every RD trial quantizes
   with the plain deadzone**. Trellis runs exactly once, on the committed
   winner.
3. **Bounded subpel.** The subme-7 tier allows 2 hpel and 2 qpel diamond
   iterations on every candidate. There's **no** extra winner refinement, and
   it never iterates to convergence. Integer search is hexagon (radius-2,
   follow-direction, me_range 16) plus one square refine. There's **no UMH at
   medium** (UMH starts at slower / subme 9).
4. **16x8/8x16 are derived, not searched fresh.** Their costs are estimated
   from the 8x8 SATDs (a per-orientation rectangular estimate). The search is
   seeded from the 8x8 MVs, and partition 1 is skipped when
   `part0_cost + est(part1) > i_best_satd * 5/4`.
5. **Intra in inter frames is screened, not encoded.** The intra analysis is
   predict-plus-SATD per mode only. i8x8 entry is gated at
   `min(cost_so_far, i16satd) > (i_satd_inter*6)>>2` (1.5x inter). i4x4 uses a
   `min3*10/8` threshold with a per-mode bail. A neighborhood heuristic
   (`b_fast_intra`) tightens i16 with a LUT. Intra is RD-coded only if it
   survives to `intra_rd`. Profile: **~70 self-samples**.
6. **Chroma in RD trials** is encoded but var2-pre-decimated. yah264's
   equivalent early-out measured at parity.

## 3. yah264 medium anatomy, and the delta

| per-MB operation | x264 medium | yah264 medium | delta driver |
|---|---|---|---|
| P integer ME | hex+square, seeded; 16x8/8x16 seeded from 8x8 plus early-term | hex-to-convergence (32 iters cap) plus **UMH grid on 16x16 AND 16x8 x2 AND 8x16 x2**; every shape searched fresh from its own median | ME probe ~23% of self-time |
| P subpel | 2 hpel + 2 qpel diamond iters, all candidates | 8-neighbour square **iterated to convergence**, hpel+qpel, every search | probe_sub ~10% |
| P RD encodes | 2-4 (deadzone quant) | 1-2 (16x16 insurance always admitted, `rd_admit_16` default-open) but **each runs Viterbi RDOQ and full chroma** | RDOQ ~12% |
| trellis | once, at commit | inside **every** RD trial and intra trial | -- |
| intra in P/B | SATD screen (~70 samples) | `analyze_intra_g` **fully encodes i16** (fdct, RDOQ, recon, chroma, rate) on every MB; i4/i8 get only a 2x-margin early-out | intra ~625 samples (~9x) |
| B modes | SATD-rank all, RD survivors (17/16), direct 33/32-gated | threshold-survivor is near-parity; **intra still fully encoded**; direct always RD'd | -- |

So the fan-out story is precise. **yah264 does not run more RD encodes than
x264. It runs a heavier ME per shape, a trellis-and-chroma-loaded encode per
trial, and a full intra encode per MB that x264 screens away.** Those three
asymmetries plus bounded-subpel discipline make up the whole remaining
algorithmic gap.

## 4. The design: five workstreams

Everything is `subme<=8`-gated and env-escaped, with subme>=9 byte-identical.

### W-A: ME discipline

- **A1. Parent/sibling seed chaining.** Thread the 16x16 winner MV (per ref)
  and the co-located 8x8 winner MVs into the 16x8/8x16/8x8 searches as seeds.
  yah264 restarts each shape from its own spatial median.
- **A2. 16x8/8x16 cost-estimation early-termination.** Compute a
  per-orientation rectangular estimate from the 8x8 SATD phase, and skip the
  partition-1 search when `part0 + est > best_so_far * 5/4`. This requires
  ordering the SATD phase 16x16 -> 8x8 -> {16x8, 8x16}. Today all four run
  unconditionally.
- **A3. UMH scope narrowing.** UMH-16only measured 19% speed at +0.62% BD, so
  it's margin-bound. Keep UMH on 16x16 regardless. It's yah264's compensation
  for hex reach on zoom content. We measured it as real basin-escape work.
- **A4. Hex iteration cap** 32 -> ~8 with A1 seeds. Byte-inequivalent, BD-gated.
- **A5. Subpel iteration discipline** = x264's `{0,0,2,2}`, a 2+2 capped
  diamond on all candidates. It measured +1.36% BD mean. This is the canonical
  margin-spend lever, to use only if the baseline shows at least that much
  margin.

### W-B: intra screening

Replace yah264's always-encode-i16 in P/B analysis with x264's structure:

- **B1.** SATD mode screen. Predict each i16 mode, `satd16x16`, and keep the
  best, with **no transform, quant or recon**. Compare it against the inter
  SATD winner. Run the full encode only when the intra cost is competitive,
  which lets it into the RD tournament. Otherwise intra is out with no encode
  at all.
- **B2.** Keep the i4/i8 2x early-out inside the survivor path. Add x264's
  i8x8 entry gate (1.5x inter) plus the i4x4 per-mode bail.
- **B3.** The same screen in `analyze_b_mb`. Its intra trial is the same dead
  weight on B MBs, and B MBs are the majority at bframes 2.
- Risk: intra-cheap content (akiyo) leans on intra in P, so the screen must not
  starve it. Sentinel akiyo and foreman in the BD gate.

### W-C: trellis/RDOQ placement (x264 trellis=1 semantics)

- **C1.** RD trials (`inter_rd_score`, `eval_b_*`, the intra trial) quantize
  with the plain deadzone (`y264_quant_*`) and no Viterbi. J comes from that.
  The **commit** path re-encodes the winner once with the full Viterbi RDOQ.
- **C2.** If C1's BD holds, drop the per-trial chroma Viterbi to deadzone too.

### W-D: B-path completion

- **D1.** Gate the always-RD'd B_Direct with x264's `direct <= best*33/32` SATD
  admission. It needs one satd16x16 on the already-built `dp`, which is nearly
  free.
- **D2.** B intra screen, which is W-B3.

### W-E: quality-neutral constant-factor experiments

The candidate-discipline arithmetic doesn't reach 1.0x alone. The rest is
per-op constants. Each is a bounded experiment with a wall-clock gate (>= 2% on
2 clips or revert, byte-identical required):

- **E1. Stride-16 fenc/analysis cache.** Copy the MB's source (and hot pred)
  into contiguous stride-16 buffers once per MB. Every SATD/SAD probe then
  reads fully-utilized cache lines instead of frame-stride planes. It's x264's
  single biggest structural cache advantage, and it's entirely
  output-invariant. It touches probe plumbing widely, so prototype on the P
  16x16 path first.
- **E2. Snapshot/ctx slimming.** `save_mb_rec` copies and est-ctx 1KB memcpys
  shrink proportionally once W-A..C cut the trial count. Re-profile first.

## 5. What each workstream measured

**Baseline.** 2.65x geomean pure-C, and BD **-3.95%** at the unflagged crf30-46
sweep (+1.47% at crf24-40, where bus saturates). yah264 is ahead on quality,
so the margin-bound levers A3 and A5 are live. Fresh both-encoder profiles
also invert the plan's premise. **ME and SATD are at PARITY with x264** (6.6 vs
7.0; 17.3 vs 20.4), so W-A's "largest bucket" is already spent. The one
large asymmetry left is trellis placement, **31% vs 6.3%**.

> **Note, 2026-09-27.** The 31% figure predates trellis-at-commit (the `--trellis 1`
> placement: RD trials quantize with the plain deadzone and only the winning mode
> is re-encoded with the full RDOQ, `Y264_TRELLIS_COMMIT`). The latest profile has
> trellis at about 4.2% of wall, so this asymmetry is largely spent. The paragraph
> above stays as the record of what the baseline measured.

**W-B (intra screening) shipped: ~12% speed** (2.84x -> 2.51x) at BD +0.009%,
which is neutral. x264's own 9/8 threshold failed the BD gate here at +0.48%,
while the retuned 2x margin is neutral. That's the standing evidence that
x264's thresholds must be re-tuned against yah264's fallback balance. Copying
them doesn't work.

**W-A is tapped out.**

- A1 (parent-seed chaining) reverted. It bought 0% speed because the ME seeds
  are already saturated, and it breached BD (bus +0.96).
- A2 (est early-term) deferred. It needs the invasive x264 cost-estimate plus a
  SATD reorder.
- A4 (hex cap) near-zero, since the hex already early-exits on convergence.
- D1 (direct gate) low value, since direct usually wins.

**W-C does not transfer.** Trellis-at-commit was built P-first with all
correctness gates green. It measured **BD +2.35% for ~2% speed**. The
deadzone-J decisions are worse, yah264 was leaning on trellis-in-trials, and
the winner re-encode eats the savings. Closed.

So the quality-neutral half of the program is spent. What remains is E1
(byte-identical, uncertain) plus the two margin-bound levers A3 (0-19%) and A5
(0-15%), which the baseline's BD margin now permits.

Abort criterion per step: if the BD gate fails after one bounded retune, revert
and log the root cause, then move on. No step ships disabled-by-default "for
later". Ship it on at medium, or revert.

## 6. Honest arithmetic to parity

Multiplying the mid-range expectations: B 1.06 x A1/A2 1.10 x A4/D1 1.04 x C
1.07 x A3 1.10 x A5 1.08 ~= **1.55x**, from ~2.4x to **~1.55x remaining**. The
candidate-discipline program alone does NOT reach parity. Getting to <= 1.0x
needs E1-class constant-factor wins and/or fuller margin-spend on A3/A5. Their
maximum readings, 19% and 15%, are another ~1.4x if the margin allows both at
full strength, for ~1.1x. Both routes are measured, not assumed. If
the gap floors above 1.0x with the invariant binding, the remainder is by
construction the price of quality-above-x264. Surface that number and let the
owner decide whether BD-parity-exact, spending ALL margin, is the ship point.

## 7. Measurement protocol (binding)

- Profile and BD **at `--crf`, never `--qp`** (the `--qp` path shifts mb-tree by
  ~30%). Wall-clock best-of-3 or better on a cool box.
- BD: `scripts/bdcompare.py --vmaf --no-cache`, a robust multi-point sweep on
  the 7-clip corpus, **against x264 medium directly**. Never against
  yah264-self only.
- Every step: recon-match (medium band QPs), subme>=9 byte-identity against a
  fresh HEAD build, threaded==serial canary, ASan on one clip after any
  buffer-touching change.
- Sentinels: bus and stefan (motion), akiyo (static/intra), mobile (detail),
  foreman (mixed). A3/A5 margin decisions additionally check the broadened
  `--class` corpus.

## 8. Non-goals and standing dead ends

Do not re-open inside this program: batched sad_x3/x4 (SIMD tier), chroma
pre-decimate, UMH-off-everywhere, the 16x16-Bi/uni top-1 SATD gate (+3.2%),
winner-only-trellis without a re-encode, the mb-tree lowres cache (~0 at
`--crf`), lowres-ME motion levers, and any SIMD/NEON work (separate tier).
