# The B early-skip decision: the oracle's ceiling, and a staged design

The B tournament is where the remaining speed gap sits, and skip-verdict
macroblocks consume about half of it (~193 ms of 396 ms under `N264_BPROF`).
The question this page answers: how should the B skip decision use better
evidence, earlier?

The answer, after building the design in full: it should not. **The skip
DECISION is closed. The remaining prize belongs to making the tournament
cheaper.**

## 1. The ceiling, measured first

Oracle replay, t1, medians of 5, arms interleaved per round, **oracle
byte-identity ok on every cell** (a correct oracle replay reproduces the normal
run exactly, which is the check the instrument exists to pass):

| cell | perfect B decision, pre-ME | perfect B, post-ref0 searches | perfect both sides |
|---|--:|--:|--:|
| stefan hi / lo | 1.033 / 1.111 | 1.020 / 1.064 | 1.059 / 1.187 |
| bus hi / lo | 1.027 / 1.123 | 1.016 / 1.070 | 1.042 / 1.206 |
| pjoy hi / lo | 1.095 / 1.172 | 1.058 / 1.104 | 1.134 / 1.277 |
| samsung hi / lo | 1.133 / 1.143 | 1.086 / 1.088 | 1.225 / 1.260 |

**A perfect B early-skip decision is worth 11-17% at the low points.** Against
the samsung-lo cell, where we are 1.43x behind, perfection on this one decision
closes about a third of the gap, and perfection on both slice types about half.
So the premise holds and is bounded: this is the largest single addressable
compartment, and it cannot reach parity alone.

Two refinements matter more than the headline:

- **Shipped exits collect part of this ceiling as they land**, so a ceiling
  measured against an older binary overstates what is left (samsung-lo post-ref0
  reads 1.088 where an earlier measurement read 1.171).
- The pre/post split says **~40% of the B prize is the ref-0 ME itself**
  (samsung-lo 1.143 vs 1.088). Only a pre-ME commit reaches that, and section 2
  shows no safe pre-ME commit rule exists. **The realistic target is 6-10% on
  the low cells.**

And a repricing that changes the surface: the behaviour-matched confirmation
(`N264_BSKIP_PROBE` / `N264_BSKIP_CONFIRM`) nets a **loss** everywhere on the
current binary (0.975-0.998, probe cost alone 1.6-3.3%) against a
same-placement ceiling of 1.040-1.097, because the shipped exit already absorbs
its catches. What separates 0.98 from 1.09 is not probe cost, it is **decision
quality at the commit point**.

## 2. Two negative results that kill the obvious designs

Measured with the `N264_BLATE_STAT` evidence dump over 629k B macroblocks
across four regime cells.

**The fullres searched SATD is NOT predictable from the lowres cost per
macroblock.** `satd16min / (c0+c1)` has median 3.11 on samsung with p10 1.47
and p90 5.35, and the median itself swings from 1.38 (mobile) to 4.06 (akiyo).
Even self-calibrated per frame, predicting the exit's precondition catches
32-49% at 23-38% false positives. **Handing `bexit_ok` a scaled lowres stand-in
for satd16min is dead**, and that was the obvious design, avoided for the price
of one dump.

**No pre-ME signal in this set can safely COMMIT.** Every gate that catches
most late skips (lowres-static, mb-tree leaf, neighbour-skip) also admits
10-95% of macroblocks whose searches go on to win by 8-25% median SATD. This
reproduces the falsified pre-ME gate attempts with populations rather than BD
damage, and it is why the pre/post share should be written off for decision
work.

## 3. Three positive results

- **Neighbour context is an excellent ADMISSION signal.** Left+top-skipped
  catches 55-68% of late skips on three of four cells while shrinking the
  population that pays any speculative test by 2.5-4x. Useless to commit on,
  ideal for deciding *who pays the probe*.
- **The untested ref-B guard is the propagation field.** Ref-B late skips are
  26-35% propagation-important (mbtree offset < 0) against only 4-18% of coded
  ref-B macroblocks: **roughly two thirds of what the shipped exit's blanket
  ref-ban protects are propagation leaves.** Blanket non-ref, a lambda-scaled
  SSD bound and an absolute SSD bound were all tried; the propagation field,
  the quantity the damage actually travels through, was never the guard.
- **The money is in the middle of the tournament.** Mid-exit skips (paid ME,
  SATD, direct RD and 16x16 RD, then left) are 12-24% of all B macroblocks per
  cell, and full-tournament skips another 11-18%. The commit point that matters
  is x264's post-ref0 position, which the confirmation machinery already
  occupies with a decision that is too weak there.

## 4. The design: staged, evidence quality rising as cost is sunk

- **Stage A, pre-ME admission.** Admit when (left AND top neighbours skipped)
  OR (lowres pair MVs within 2 qpel of the direct MVs). A few compares; both
  inputs are already on the frame. **A never skips anything**, so its false
  positives cost time only.
- **Stage B, the graded probe, on admitted macroblocks only.** `probe_skip_g`'s
  tolerant form, whose measured defect is running on every macroblock (1.6-3.3%
  of wall). Under A it runs on about a third of entrants while keeping 55-68%
  of catches.
- **Stage C, the commit, hoisted to x264's post-ref0 point.** At the existing
  `r == 0` confirmation site, commit B_SKIP when the probe held, direct is
  SATD-competitive with the **ref-0** costs (the `bexit_ok` shape without
  needing refs 1..n or the SATD phase), the skip J still leads, and for
  reference B's the propagation guard holds. Committing here abandons refs 1-2,
  the SATD builds, direct RD, the 16x16 RD trials, subpartitions and intra:
  exactly what the mid-exit population pays in full.
- **Stage D, the shipped exit stays** as the backstop, with the same
  propagation-guarded ref-B readmission.

Why this is better evidence than SATD-vs-searched-16x16: the current
precondition needs the searches it is trying to save; it is **rate-blind**
(SATD cannot say whether residual survives the quantizer, and surviving the
quantizer is what skip IS); and it is **future-blind**, which is why the
ref-slice ban is a blanket. The staged form gets rate-awareness from the
coder-consistent probe at the real QP, future-awareness from the propagation
field, competitiveness from the two searches x264 itself considers sufficient,
and pays the speculative cost only where the neighbourhood already says skip is
likely.

## 5. E1: propagation-guarded ref-B readmission

`N264_B_SKIP_EXIT=3` is in the tree, **default OFF**. The implementation is
`bx_ref_admit` in `src/encoder/macroblock.c`, which replaces the inlined
`>= 2 && bdist_x <= ssd` test at both exit sites so `=2` keeps its old meaning
exactly and `=3` selects the guard.

E1 exists to find out whether propagation is a safe currency for stage C's
ref-B guard. It is:

| gate | E1 (`=3`) | the lambda-scaled predecessor that failed |
|---|---|---|
| ABR band, akiyo / park_joy | +0.96% / **-0.00%** | +2.22% / +1.68% |
| CRF low band 32-41, six clips | +0.00 akiyo, +0.12 pjoy, +0.07 samsung, -0.03 bus, -0.03 foreman, -0.00 stefan | -- |
| CRF high band 20-29, six clips | +0.01 foreman, +0.00 akiyo/bus, -0.02 stefan, -0.05 pjoy, -0.07 samsung | -- |
| CRF band, twelve clips | worst +0.07% (samsung); eight clips exactly +0.00% | -- |

**akiyo's ABR +0.96% is not a result, and the disproof is worth keeping.** The
same arm on the same clip reads **+0.96%, +0.25% and -5.75%** on three ABR
ladders 13% apart in rate. The ABR band's own per-clip noise floor for akiyo is
1.09-2.87 points for a 2-5% inert field perturbation (park_joy 0.51-1.39), so
the ladder sweep is measuring the band, not the arm. The settling evidence is
the CRF low band: it reaches the same QP regime without the rate controller's
feedback loop, and akiyo reads **exactly +0.00%** there.

**The arm engaged, and that is what makes the nulls evidence.** `BXSTAT` on the
CRF band's own command line, `=1` against `=3`:

| clip | `taken` at `=1` | at `=3` | ref-B readmitted | md5 |
|---|--:|--:|--:|---|
| akiyo crf32 | 37.2% | 58.7% | 98.9% of blocked | SAME |
| bus crf28 | 22.7% | 30.7% | 97.0% | SAME |
| samsung crf28 | 21.2% | 28.7% | 70.7% | SAME |
| samsung crf22 | 21.2% | 28.4% | 69.1% | differs |

A guard that moves 70-99% of the blocked population while the bitstream does
not change at all is the strongest shape this gate has: a byte-identical output
cannot cost BD, so the +0.00% rows are work removed, not an inert knob.

**The wall is real but small.** Interleaved A/B, t1, medians of 11, arm minus
its own duplicate-base control:

| cell | pure-C | as-shipped |
|---|--:|--:|
| samsung | **+0.45%** | +0.15% |
| park_joy | **+0.53%** | +0.36% |
| bus | +0.29% | VOID (ctrl spread 1.43) |
| foreman | -0.10% (null) | +0.56%, spreads 1.08-1.12 |

At medians of 5 the controls drift -1.21% to +0.76% and the reading is
uninterpretable; only the eleven-run form has controls inside 0.26%. This is
the same size as `=2`'s +0.45% even though `=3` readmits two to four times as
many macroblocks, which says the population `=2` was already catching is where
the time is.

**Verdict: E1 passes the gate it was built to answer and stays default OFF on
its own merits.** Half a percent is not a default flip. What it buys is E2's
licence: stage C may guard reference B's on propagation. `determ_repeat.sh`
under six spinners, 2/2 configs reproducible over 12 runs each with the arm
armed.

### What the guard actually reads

`f->mbtree_off` is **the COMBINED x264-style offset, not the propagation term.**
It is built as `aq_fold[i] - strength*ratio + boost_mean`, and `mb_qp_pre`
treats it as combined precisely so the standalone AQ offset is never added
twice. So `mbtree_off >= 0` does not say "this macroblock propagates little";
it says **`aq_fold >= strength*ratio`**, which a flat, heavily-depended-on block
can satisfy on AQ's contribution alone.

Two things bound the damage, and neither is a design: centring is off under CRF
and ABR with B-frames, so `boost_mean` is 0; and `aq_strength` is 0.4 rather
than the 1.0 it was calibrated at, which shrinks `aq_fold`. **The guard is
therefore approximately right for the wrong reason, and it will move if either
changes.**

This is the trap `docs/instruments.md` records in another form: check what a
substituted field is interpreted AS. Section 3's 26-35% statistic reads the
same combined field out of `N264_BLATE_STAT`, so E1 as built is faithful to
what was measured, but it is not a guard in propagation units.

Separating the pure term (`strength*ratio`, or equivalently
`mbtree_off - aq_fold`) would cost one more per-frame `int8` array plumbed
through the same `G->` copy paths as `mbtree_off`; it is not currently
published per macroblock. `f->aq_off` is not the missing half: it is built by
`aq_analyze` on the coded frame's FULL-RES source, where `aq_fold` comes from
`mbtree_invqscale` on the lowres anchor plane, so subtracting one from the
other is not the identity it looks like.

## 6. E2: the staged design, built whole, and the kill fired

Stages A, B and C are all in the tree, default inert: `N264_BSKIP_ADMIT=<tol>`
(stage A: probe only where left+top are skipped OR the lowres pair MVs land
within tol of the direct MVs; `_NB=0` / `_MV=0` isolate the clauses), the
existing `N264_BSKIP_PROBE` / `N264_BSKIP_CONFIRM` as stage B, and
`N264_BSKIP_CGUARD=<mask>` (stage C at the post-ref0 commit: bit0 direct
SATD-competitive with the two ref-0 searches, since `n264_me_search` returns
SATD plus mv-rate after qpel refinement so the currencies match; bit1
lambda-cheap skip recon; bit2 E1's propagation guard for reference B's).
`BXASTAT` prints the whole funnel. The default path is md5-identical;
`determ_repeat.sh` under six spinners reads 2/2 with the full arm.

**The funnel, at the bar's own cells** (`--bitrate`, t1, 60-120 frames):

| cell | config | admitted | probe-held | committed |
|---|---|--:|--:|--:|
| samsung-lo | CONFIRM=1 CGUARD=5 | 100% | 20.0% | 3.8% |
| samsung-lo | CONFIRM=2 CGUARD=5 | 100% | 20.5% | 5.1% |
| samsung-lo | CONFIRM=2,3 CGUARD=5 | 100% | 25.7% | **6.1%** |
| samsung-lo | + ADMIT=2 | 65.4% | 19.5% | 3.8% |
| pjoy-lo | CONFIRM=2,3 CGUARD=5 | 100% | 38.9% | **15.7%** |
| pjoy-lo | + ADMIT=2 | 53.6% | 32.2% | 9.1% |

The throat is not where the design thought. Stage A's shrink is 1.3-1.9x on
these cells, not the 2.5-4x the dump promised: on content where skips are
common the left+top clause admits nearly everyone. And the commit rate
saturates, with tolerance 3 adding 0.2 points over tolerance 2 on samsung. What
remains probe-held but uncommitted (~14-23% of B MBs) are macroblocks whose
searches genuinely land off the direct MV, and committing those is precisely
the over-skip the guards exist to prevent.

**The wall**, as-shipped t1, 7 interleaved rounds, base / ctrl / 3 arms in one
batch:

| cell | CONFIRM=2 | CONFIRM=2,3 | +ADMIT=2 | ctrl |
|---|--:|--:|--:|--:|
| pjoy hi | 0.983 | 0.990 | 0.999 | 0.998 |
| pjoy lo | 0.999 | 0.993 | 0.989 | 0.998 |
| samsung hi | 0.981 | 0.978 | 0.984 | 1.002 |
| samsung lo | 0.973 | 0.994 | 0.956 | 0.991 |

**Best case is a wash; the bar was >= +3% on both lo cells.** The miss is not
noise: the arithmetic agrees from the other side. 6.1% of B MBs committed, each
saving ~60% of its tournament share of ~40% of wall, is +1.5% gross against a
1.6-3.3% probe tax. To make the bar, the funnel would need 3-4x the conversion
at a fraction of the tax, and the conversion is capped by the population whose
motion genuinely disagrees with direct.

## 7. Conclusion

Every evidence class that exists before and inside the tournament (lookahead
costs, lookahead MVs, propagation, neighbours, the coder-consistent probe at
the real QP, the real ref-0 searches) has been measured, singly and jointly, at
the commit point x264 itself uses. **The remaining prize belongs to making the
tournament cheaper, not to a better skip decision. The post-ref0 ceiling
(1.088 / 1.104 on the lo cells) is a documented floor, not a target.**

Two follow-ups are answered by the same sweep and should not be run: wider
probe tolerance under the full guard set (the tolerance axis saturates two
points short of the bar), and a cheap confirmation-shaped search for admitted
macroblocks (the commit rate, not the search cost, is the binding constraint).
Frame-level arming is moot, since nothing reached a BD round.

Two things survive:

- **Stage A is the fix for the confirmation machinery's loss.** The all-MB
  probe costs 1.7-2.2% at the high points; under ADMIT it reads 0.999 / 0.984.
  That upgrades `BSKIP_PROBE` / `BSKIP_CONFIRM` from "nets a loss everywhere"
  to a wash, worth knowing if that machinery is ever revived, not worth a
  default.
- **The guard set and the funnel counters** are permanent instruments: any
  future claim about the B skip decision has `BXASTAT`'s
  admitted/probed/held/committed columns to pass through, and the declined-by
  attribution says which guard is binding.

The currency question (combined offset vs pure propagation term) is moot for
stage C, since there is no stage C worth guarding. It stays open only in E1's
own narrow form, where the combined reading has the defensible semantics (AQ's
masking allowance covering the propagation debt) and passed its gates as-is.

Wall readings for anything in this area come from interleaved harnesses, never
board deltas, and `determ_repeat.sh` runs **under load** for anything touching
the reorder: the conditional L1-first reorder is known to change bits when
live.

## The instrument

**`N264_BLATE_STAT=<path>`** (default inert) plumbs the lookahead's per-MB
lowres pair costs through the B reorder buffer (they were dropped at pop, with
only the MVs stashed) and dumps, per B macroblock, the final verdict and every
pre-ME signal a better decision could consult: direct SATD, skip-recon SSD,
lowres pair costs, lowres-vs-direct MV disagreement, mb-tree offset, QP, and
the path taken.
