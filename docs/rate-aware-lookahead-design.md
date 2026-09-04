# Rate-aware lookahead + RC unification: design spec

**Result so far: R0.1 (the byte-identical `y264_lr_blk` D/R-separate record) is
in the tree as the TPL-ready substrate. R1 (the rate-aware propfrac consumed
through mb-tree) measured a net VMAF-NEG regression and is reverted:** mean
~+0.08%, foreman +0.91%, akiyo +1.36%, i.e. the clips mb-tree helps most
regress hardest, and the result is robust to recalibrating
`LR_INTRA_PEN_BITS`. Pricing MV rate in the propagation cost *reduces*
propagation exactly where mb-tree wants to boost. That is "tightening mb-tree",
which dead-ends the same way three prior mb-tree tweaks did.

**Conclusion: the mb-tree propfrac is the wrong consumer of the rate record.
The right consumer is TPL.** R2 and R3 were downstream of a winning R1 and are
parked. The record layout below is the part that matters, because it is what
TPL will read.

Reference for the record's forward compatibility: SVT-AV1's published TPL
description and its `TplStats` fields. Everything is reimplemented from
understanding, not copied.

## 1. Problem statement

Four consumers make decisions off lowres analysis, and each sees a different,
rate-blind cost:

| consumer | cost it sees today |
|---|---|
| scene-cut | raw-SATD sums `sc_ic` / `sc_pc` captured at push |
| b-adapt demote | raw-SATD `pinter` / `aintra` plus fresh backward ME |
| mb-tree propfrac | raw SATD, except IPPP anchors where a magic `mbtree_mvlambda=8.0` is folded destructively into `ainter` |
| rate control | none of the above; a separate full-res zero-motion SATD `frame_complexity` |

x264 prices one cost (SATD + lambda*mvbits + fixed penalties, lambda =
its ME lambda at QP 12 is 1) and routes it to all four consumers, with a parallel
AQ-weighted accumulator for RC only. Its CRF then carries per-frame complexity
*through* mb-tree's offsets instead of a frame-level complexity equation. We
fight that architecture with `CRF_BASE_CPLX=9000` and an asymmetric `-2.0`
clamp.

Three prior dead ends (b-adapt over-demote, IPPP mb-tree chain saturation, the
mb-tree/AQ motion-alloc attempts) all trace to this: decisions taken on costs
that do not price rate.

x264 collapses distortion and rate into one SATD-like scalar per lowres
block. We deliberately do NOT: TPL's entire advantage over mb-tree is keeping
distortion and rate separate, as the AV1 encoders' TPL statistics do (source
and reconstruction distortion and rate per block, plus the propagated
dependent distortion and rate). The record below stores D and R apart and
every consumer composes its own scalar on read. That is the one-way door; get it right and TPL
is an additive change.

## 2. Design overview

1. **One record** per lookahead frame: per-MB lowres intra distortion, plus up
   to three reference "legs", each holding {inter distortion, rate proxy in
   bits, MV} as SEPARATE fields. Per-frame AQ offset and weight arrays computed
   once and stored alongside.
2. **One composition helper** turns (D, R) into an x264-style scalar
   `D + lambda_lr*R + penalties` at the consumer, so consumers can differ
   (b-adapt applies the B discount; RC applies AQ weights; TPL bypasses
   composition entirely).
3. **Routing**: scene-cut, b-adapt and mb-tree propfrac all switch to composed
   rate-aware costs; the IPPP-only `mbtree_mvlambda` hack and the destructive
   fold into `ainter` are deleted.
4. **RC**: `frame_complexity` is replaced, when the window exists, by the
   AQ-weighted lookahead frame cost of the actual frame and type being coded,
   for CRF, ABR, VBV and 2-pass alike.
5. **CRF**: once complexity flows through mb-tree, the CRF equation moves to a
   duration-based constant-qscale form with an mb-tree compensation offset;
   `CRF_BASE_CPLX`, `crf_cblur` and the `-2.0` clamp are deleted. The mb-tree
   centring special case for IPPP dies with them.
6. **AQ unification** rides along: `mbtree_invqscale` derives a mode-1
   flat-variance offset for the anchor fold while encode-time AQ defaults to
   mode 2, so anchors and B frames are quantized against two different AQ
   metrics. The record stores ONE per-frame offset array computed with the
   shipped `aq_analyze` metric, and every fold, weight and encode-time offset
   reads it.

## 3. The record layout (the irreversible part)

New types in `encoder.h`:

```c
/* One lowres 8x8 block analysed against one reference leg. Distortion and
 * rate stay in SEPARATE fields (TPL-ready). Consumers compose a scalar via
 * lr_cost; TPL reads the fields apart. d_inter is pure SATD, normalised to
 * the 8-bit domain (>> (BIT_DEPTH-8)); r_inter is a bit count, NOT
 * lambda-scaled. mvx==LR_MV_INVALID marks an unfilled leg. */
typedef struct y264_lr_blk {
    int32_t d_inter;   /* SATD8x8 of the best prediction, no penalties */
    int32_t r_inter;   /* rate proxy, bits: mv bits vs median predictor
                        * (0 when mvd==0) + NZ-MV flat penalty */
    int16_t mvx, mvy;  /* winning lowres MV, integer lowres pels */
} y264_lr_blk;

#define LR_MV_INVALID INT16_MIN

/* Reference legs of one lookahead entry. Fixed roles for now; a later step
 * generalises this to [list][dist] behind the same accessor, so no consumer
 * indexes the array literally. */
enum { LR_LEG_PREV   = 0,  /* vs previous display frame (push-time) */
       LR_LEG_NEXT   = 1,  /* vs next display frame / future anchor
                            * (finalize/mb-tree time, filled lazily) */
       LR_LEG_ANCHOR = 2,  /* vs previous typed anchor (anchor entries) */
       LR_NLEGS      = 3 };
```

`struct la_entry` gains or replaces:

```c
    int32_t      *d_intra;      /* per-MB lowres intra SATD, pure */
    y264_lr_blk  *leg[LR_NLEGS];/* nmb each */
    float        *qp_off_aq;    /* per-MB raw AQ offset (aq_analyze
                                 * metric, unclamped) */
    float        *inv_q;        /* 2^(-qp_off_aq/6), mean-normalised */
    /* frame-level sums, filled when the corresponding leg is analysed */
    int64_t       sum_icost, sum_icost_aq;
    int64_t       sum_cost[LR_NLEGS], sum_cost_aq[LR_NLEGS];
```

Deleted fields: `aintra`, `pinter`, `ainter`, `amvx`, `amvy`, `sc_ic`, `sc_pc`
(the sums move into `sum_icost` / `sum_cost[LR_LEG_PREV]`).

Composition helpers (encoder.c, near the blk8 functions):

```c
#define Y264_LR_QP        12  /* lambda_me(12) == 1; see section 4 */
#define LR_NZMV_PEN_BITS   5  /* nonzero-MV flat penalty */
#define LR_INTRA_PEN_BITS  5  /* inter-vs-intra bias */
#define LR_LOWRES_PEN      4  /* anti-zero-residual floor (VBV safety) */

static inline long lr_cost(const y264_lr_blk *b, int lambda)
{ return b->d_inter + (long)lambda * b->r_inter + LR_LOWRES_PEN; }

static inline long lr_icost(int32_t d_intra, int lambda)
{ return d_intra + (long)lambda * LR_INTRA_PEN_BITS + LR_LOWRES_PEN; }
```

Ownership and lifecycle:

- Filled by `la_push` (LR_LEG_PREV, d_intra, AQ arrays), `la_finalize`
  (LR_LEG_ANCHOR for anchors; LR_LEG_NEXT transiently for the flash test and
  the demote rule), and `compute_mbtree` (LR_LEG_NEXT of buffered B entries,
  against the anchor being coded).
- When a frame is typed B and its planes are buffered into `bplane[]`, the
  record pointers (`d_intra`, `leg[*]`, `qp_off_aq`, `inv_q`) are
  POINTER-SWAPPED into a parallel per-B-slot set on the encoder. The la ring
  reuses entries, so a swap, not a copy, keeps the B's analysis alive until it
  is coded. Anchors need no swap: `encode_frame_core` receives the popped
  `la_entry*` directly (signature change, NULL on the legacy no-window path).
- The legacy per-encoder arrays `lr_intra` / `lr_inter` / `lr_mvx` / `lr_mvy`
  and the `lowres_analyse` run in `encode_frame_core` feed the mb-tree FINISH
  step (the ratio denominator and the adaptive-strength ratio) and the
  `la_depth==0` scene-cut. **This is NOT a trivial duplicate of the window
  analysis.** Verified against the code order: the B path (`emit_frame`) does
  NOT swap `lowres_prev`, only `encode_frame_core` (anchors) does, so for an
  anchor `lr_inter` = anchor-vs-PREVIOUS-ANCHOR, matching `leg[ANCHOR]`, and
  `lr_intra == d_intra` always (intra is reference-free). `la_anchor_lr ==
  lowres_prev` (both are the prev anchor's downscale of the same
  `en->plane[0]`), so for **bframes>0** `lr_inter == leg[ANCHOR].d_inter`
  exactly, a byte-identical dedup. But for **IPPP (bframes==0)** the mvlambda
  fold lives INSIDE `leg[ANCHOR].d_inter` while `lr_inter` is UNFOLDED, so they
  differ; the clean dedup needs the unfolded `d_inter` plus fold-in-`r_inter`
  that arrives with the D/R split. **So the finish-step dedup is coupled to R1
  and merges into it; it is not a standalone byte-identical step.**
  `la_depth==0` keeps `lowres_analyse` wholesale, out of scope.

Memory: 3 legs * 12 B + 4 B intra + 8 B AQ = 48 B per MB per entry. 1080p
(8160 MB) at depth 40 is ~15.7 MB total, noise next to the per-entry plane
copies that already exist.

What mb-tree reads against what TPL will read:

| field | mb-tree (this design) | TPL (later) |
|---|---|---|
| `d_intra` | finish-ratio denominator, self-importance | intra srcrf_dist analog |
| `leg[].d_inter` | via lr_cost in propfrac | srcrf_dist |
| `leg[].r_inter` | via lr_cost in propfrac | srcrf_rate (already in bits) |
| `leg[].mv` | splat target | propagation target |
| `inv_q` | propagation weight + RC weight | lambda/beta baseline |
| `qp_off_aq` | the fold in the combined offset | same |
| (future) recon-domain dist/rate | -- | added fields; nothing else moves |

TPL's propagation grid needs a (dist, rate) PAIR per MB; the existing
`la_prop_a/b` doubles widen to a two-field struct when that lands. mb-tree uses
only the dist half. Reserve nothing now; it is additive.

## 4. The cost formula and its constants (re-derived, not copied)

**Lambda scale.** x264 prices the lookahead at an ME lambda of 1 (QP 12). Our
`lambda_me` IS that table, verified entry for entry, and our qscale convention
matches x264's (`qscale = 2^((QP-12)/6)`, used throughout `rc_account` and
`vbv_update`; the RD-domain `lambda_mode = 0.85*2^((QP-12)/3)` is a different,
SSD-domain object and is not used here). Our lowres SATD is the same half-res
8-bit 8x8 SATD magnitude (a sum of four satd4x4 against x264's 8x8 SATD: same
scale, slightly different low-frequency capture, which the calibration sweep
absorbs). So the independent derivation lands on the same point:
`lambda_lr = lambda_me(Y264_LR_QP)` with `Y264_LR_QP = 12`, i.e. lambda 1, so
the stored `r_inter` bits act at face value. The lookahead cost must be
QP-independent (computed once, consumed at every operating QP), so a fixed
low-QP lambda is correct by construction, and 12 is where our table crosses 1.
Calibration knob: env `Y264_LR_QP`, sweep {8, 12, 16, 20} once on the corpus;
expect a flat optimum at 12 and keep whatever wins.

**MV rate.** During the diamond in `blk8_inter` the candidate cost is

```
cost(mv) = SATD8x8(src, ref+mv) + lambda_lr * (mvbits(4*(mvx-px)) + mvbits(4*(mvy-py)))
```

- Predictor `(px,py)`: median of left / top / top-right neighbours' winning MVs
  in the SAME leg, forward raster scan, bitstream-MVP geometry (degenerate
  cases as in 8.4.1.3.1: missing neighbours drop out). x264 scans in reverse
  and predicts from right and below because its lookahead MVs seed the main
  encode; that is a later step's business. The record does not encode scan
  order, so flipping later is free.
- The factor 4 prices the mvd at lowres quarter-pel scale, matching what x264's
  `p_cost_mv` indexing does on its lowres grid; our search stays integer-pel
  (subpel lowres refinement is later fidelity work, out of scope).
- `mvbits(d)` is the exact se(v) exp-Golomb length (the existing
  `lowres_mvbits`) minus the 1-bit baseline per component, so a
  predictor-perfect MV prices 0 bits: `r = sev_bits(dx)+sev_bits(dy)-2`.
- Zero-residual fast path, as x264 has: when the predictor is zero and SATD at
  mv 0 is < 64 (8-bit domain), accept mv 0 and skip the search. This is a speed
  guard AND it protects static content from spurious nonzero MVs.

Stored per block (composition happens at the consumer, never at store time):

```
d_inter = SATD of the winner (>> (BIT_DEPTH-8) for 10-bit)
r_inter = mvbits(4*dx)+mvbits(4*dy)-2 + (mv != 0 ? LR_NZMV_PEN_BITS : 0)
```

Fixed penalties (all in bits at lambda 1, mirroring x264's shape at our own
scale, sweepable only if a corpus class regresses):

- `LR_NZMV_PEN_BITS = 5`: nonzero-MV flat penalty. Rate side, so it lives in
  `r_inter`.
- `LR_INTRA_PEN_BITS = 5`: intra bias. Applied in `lr_icost`, not stored, so
  `d_intra` stays pure.
- `LR_LOWRES_PEN = 4`: flat floor on every composed cost, an anti-zero-cost
  guard for VBV and RC. It is neither D nor R, so it lives only in the
  composition helpers.

**AQ weighting.** One function (a refactor of `aq_analyze`, exposed as
`y264_aq_offsets(plane, stride, wmb, hmb, strength, out_off)`) computes the
per-MB offsets with the SHIPPED default metric (mode 2 auto-variance) at
`la_push`, once per frame. `inv_q[i] = 2^(-qp_off_aq[i]/6)`, mean-normalised to
1.0 exactly as `mbtree_invqscale` does today. `mbtree_invqscale` itself is
deleted; all its callers (the anchor fold, the per-B propagation weights, the
chain-prop self terms) read the stored arrays. This is both a correctness
unification (anchor fold and B-frame AQ finally use the same metric) and a
large CPU win, since the full-res 16x16 variance pass currently reruns per B
per anchor per chain step.

Frame sums, filled when a leg is analysed:

```
sum_icost      = sum_i lr_icost(d_intra[i], lambda_lr)
sum_icost_aq   = sum_i lr_icost(...) * inv_q[i]
sum_cost[l]    = sum_i min(lr_icost(...), lr_cost(&leg[l][i], lambda_lr))
sum_cost_aq[l] = same, * inv_q[i]
```

Unweighted sums feed slicetype decisions, AQ-weighted sums feed RC; never mix,
which is x264's explicit rule too.

## 5. Consumer diffs

### 5.1 Scene-cut

`la_push` stops capturing raw `sc_ic` / `sc_pc` and instead stores `sum_icost`
and `sum_cost[LR_LEG_PREV]`. `scenecut_from_sums` is unchanged: the ratio and
bias geometry was matched to measured behaviour whose sums are composed exactly
this way, so this ALIGNS the threshold semantics rather than shifting them.
Directionally, mv rate raises pcost on fast pans, making cuts slightly more
likely there; the existing flash guard already covers the failure mode. The
flash test's transient backward ME composes the same way. Gate: a cut-placement
diff over the corpus, all classes; every moved cut is inspected, and the count
of moved cuts at default settings is expected near zero.

### 5.2 b-adapt demote

The demote comparison switches to composed costs, and the backward ME result is
written into the candidate's `leg[LR_LEG_NEXT]` instead of being discarded (a
later Viterbi step will want it). Two adjustments, because rate-pricing raises
exactly the long-distance term that already drives the known over-demote bias:

- Apply the B discount when composing the continue-run side:
  `cb_scaled = cb * 100 / (120 + bframe_bias)` with bias 0. x264's costs are
  only comparable across B/P choices WITH this discount; importing the rate
  term without it would demote more.
- Re-A/B the 0.90 threshold under the new composition (sweep 0.88-0.94 once).

If the b-adapt BD gate regresses anyway, this consumer alone reverts to
composing without the rate term (`Y264_LR_RATE_BADAPT=0`), which the
split-field record makes a one-line consumer choice. Nothing else is coupled to
it.

### 5.3 mb-tree

`la_finalize`'s anchor ME stores pure (D, R, MV) into `leg[LR_LEG_ANCHOR]`; the
`bframes == 0` block that folds `mvlambda * lowres_mvbits` into the stored cost
is DELETED along with `mbtree_mvlambda` and the `Y264_MBTREE_MVLAMBDA` knob.
The propfrac in `la_chain_prop` and both `compute_mbtree` loops becomes

```
frac = 1 - min(lr_cost(blk, lambda_lr), icost) / icost, icost = lr_icost(d_intra[i], lambda_lr)
```

which is the principled version of the hack, now on EVERY path (B-vs-anchor and
next-mini-GOP loops included, which were raw SATD). This is what makes the IPPP
chain converge without special-casing: a tracked pan pays lambda*mvbits, so
frac stays below 1 and the geometric series is finite. The +/-51 IPPP bound and
+/-8 bframes bound, the bothlist split, adaptive strength and centring policy
are untouched in this phase (centring changes in R3).

`compute_mbtree`'s per-B ME writes into the B's swapped `leg[LR_LEG_NEXT]`
record and, as a byproduct, accumulates the B's RC cost (section 6). The finish
ratio's intra term reads the entry's `d_intra` and `inv_q` instead of
`e->lr_intra` and a fresh invqscale pass.

**This is the consumer that measured negative.** See the result at the top: the
rate term reduces propagation exactly where mb-tree wants to boost.

### 5.4 What must NOT consume this cost

The main-encode ME and mode decision. Feeding extra SATD-domain candidates into
full-res decisions the bitstream does not price measured +0.28% BD: SATD must
own the field. The lookahead cost feeds frame-level decisions only: typing,
cuts, frame QP, per-MB QP offsets. Seeding full-res ME from `leg[].mv` is
explicitly out of scope until something prices it properly.

## 6. RC reads the lookahead cost (R2, parked)

### 6.1 The complexity value

`emit_frame` currently computes `C = frame_complexity(e, src, type==0)` for
every RC mode. New rule, when the window path is active (an `la_entry*` or
B-slot record accompanies the frame):

```
I frame:  C = sum_icost_aq
P anchor: C = sum_cost_aq[LR_LEG_ANCHOR]     /* ME vs its actual list-0 ref */
B frame:  C = sum_i min(icost_i, cost(leg PREV), cost(leg NEXT)) * inv_q_i
```

The B sum is accumulated inside `compute_mbtree`'s existing per-B loop (it
already computes intra and the vs-anchor ME there) into a per-B-slot scalar
`e->b_lacost[b]`, using the swapped-in PREV leg for the list-0 side. This is
x264's AQ-adjusted cost-estimate shape built from our own
machinery.

`frame_complexity` survives ONLY as the `la_depth == 0` fallback. The
`Y264_DBG_CPLX` debug print reports both during bring-up.

### 6.2 Per-path effects

- **CRF** (`rc_set_qp_crf`): same equation in this phase, new C. The per-MB
  magnitude changes (motion-compensated lowres SATD instead of full-res
  zero-motion SATD), so `CRF_BASE_CPLX` is recalibrated ONCE, mechanically:
  encode the calibration probe set, take `BASE' = BASE * median(C_new/C_old)`
  over P anchors, then BD-gate. The code comment already claims this scale
  ("~820 per MB on the lookahead's ME-compensated lowres cost"), so `BASE'`
  should land near 9000; measure, do not trust. The `-2.0` clamp STAYS in this
  phase and dies in R3.
- **ABR** (`rc_set_qp`): drop-in. `abr_scale[type]` is learned online as
  `bits*qscale/C`, so it self-recalibrates within a few frames; the win is that
  a motion-compensated C stops mispredicting pans (today a pan reads as a huge
  zero-motion residual and ABR over-reacts).
- **VBV** (`vbv_clip_qp` / `vbv_update`): same self-calibrating structure, same
  drop-in. Better C means fewer false underflow clamps on motion. Per-row SATD
  for mid-frame adaptation (x264 keeps per-row SATD sums for this) is NOT built now, but the
  per-MB min-cost values needed to build row sums later are all in the record,
  so row-QP work can add it without touching the layout.
- **2-pass**: pass 1 logs the new C in the stats line; the pass-2 allocator is
  C-agnostic (it uses measured bits). Pass-1/pass-2 consistency is covered by
  the existing varying-QP canary, which must gate on CRF/ABR/AQ configs, not
  just `--qp`.

### 6.3 Plumbing

`encode_frame_core` gains the popped `la_entry*` parameter and forwards it to
`emit_frame` (anchor) or swaps its record into the B slot (section 3). The
flush path already pops typed entries and works unchanged. `code_b_hier` and
`flush_buffered_p` pass the B slot index through so `emit_frame` can find
`b_lacost[b]`; tail Bs coded as P at flush use their B-slot cost with the PREV
leg only, since no future anchor exists, which is exactly why they are being
P-coded.

## 7. The CRF equation (R3, parked)

x264 with mb-tree on keeps per-frame qscale essentially CONSTANT:
`q = (base_duration/duration)^(1-qcomp) / rate_factor`, with
`rate_factor = base_cplx^(1-qcomp) / qscale(crf + 13.5*(1-qcomp))`. ALL
content adaptation happens through the per-MB combined offsets, whose uncentred
mean (mostly negative boosts) the `13.5*(1-qcomp)` term pre-compensates.
Complexity enters RC only as the combined-offset-weighted cost, used for
VBV/ABR size prediction, never in the CRF QP equation itself.

The R3 change, one atomic flip:

1. `rc_set_qp_crf` becomes
   `qp = crf + MBTREE_QP_OFFSET + 6*(1-qcomp)*log2(BASE_DUR/duration_clamped)`
   for P anchors when mb-tree is on (I keeps the frame_qp -3, B keeps the
   cascade; revisiting the I/P/B relation against x264's ip/pb qscale factors
   is a separate, later A/B, since churning two equations in one flip is how
   this goes wrong). `BASE_DUR = 0.04 s`, duration clamped. At 25 fps CFR the
   duration term is exactly 0.
2. Delete `CRF_BASE_CPLX`, `crf_cblur`, `crf_cblur_init`, the asymmetric `-2.0`
   clamp, and the complexity argument to the CRF path entirely.
3. mb-tree boosts go UNCENTRED under CRF and ABR on every path: the
   `e->bframes == 0` centring special case is deleted. The equation's
   `MBTREE_QP_OFFSET` is what absorbs the uncentred mean, which is the whole
   point of the coupling.
4. RC complexity weighting for mb-tree frames switches from AQ-only `inv_q` to
   the FULL combined offset: `C = sum_i mincost_i * 2^(-mbtree_off[i]/6)` (our
   analog of x264's lowres frame-cost recalculation; the offsets exist
   before `emit_frame` runs). This is the guard against double-counting: the
   frame QP equation no longer sees complexity, so the only complexity paths
   are the per-MB offsets (spend) and the offset-weighted C (VBV/ABR
   prediction), which is internally consistent.
5. The duration and fps terms land as a PAIR: this CRF duration term AND the
   mb-tree propagation `fps_factor` analog (scale propagated amounts by
   `duration/avg_duration` and the finish ratio by its inverse). At CFR both
   are no-ops at 25 fps and mild constants otherwise; importing one without the
   other is the documented "fps_factor red herring" dead end. Our windows are
   CFR today, so implement both as clamped constants derived from the
   configured fps and add 50/60 fps clips to the gate explicitly.

`MBTREE_QP_OFFSET` derivation: closed-form start
`13.5 * (1-qcomp) * (strength_eff/2.0)` where `strength_eff` is the corpus mean
of our content-adaptive strength (the adaptive multiplier means x264's constant
does not transfer verbatim). Then one calibration sweep: pick the offset (grid
0.5 QP) that matches the corpus-median bitrate at CRF 32 to the pre-R3 build,
so "CRF x" keeps meaning roughly the same rate across the flip. That is the
no-broken-CRF sequencing: R3 is one commit, calibrated before merge, with
`Y264_RC_LEGACY=1` keeping the old equation alive for one cycle of A/Bs.

## 8. TPL forward-compatibility contract

What TPL needs the substrate to provide, and where this design puts it:

- Per-block dist/rate kept separate: `y264_lr_blk` (src rate is already in
  bits, the domain `delta_rate_cost` works in).
- Recon-domain distortion and rate: future additive fields on
  `y264_lr_blk` once a lowres transform+quant sim exists; no consumer moves.
- MV plus reference identity: `mv` plus the leg index (the [list][dist]
  generalisation subsumes it).
- A propagation grid carrying (dist, rate) pairs: widen `la_prop_a/b` when TPL
  lands; mb-tree keeps reading the dist half.
- A frame base-QP hook: after R3, the mb-tree contribution to the frame QP
  lives in exactly one place (`MBTREE_QP_OFFSET` inside `rc_set_qp_crf`). TPL
  replaces that term with `f(r0)`, where r0 is the frame's own distortion over
  its own plus its propagated distortion, consumed as a boost of roughly `k/r0`
  (an AV1 encoder derives its frame boost the same way; the H.264 constant is
  our own re-derivation). No consumer moves.
- Per-block lambda modulation: `mb_qp_pre` already re-derives `cur_qp` per MB
  and every RD/ME lambda derives from it, so TPL's beta arrives as a QP-offset
  field through the existing combined-offset slot. No new plumbing.
- mb-tree stays the default; TPL builds behind `Y264_TPL=1` on this record and
  flips only on a corpus-wide NEG win with no class regression.

## 9. Phases, gates, canaries

Every phase is a separate commit series with its own gate; a failed gate
reverts the phase, not the design. BD gates are VMAF-NEG, 5-point sweeps in the
discriminating band, `bdcompare --no-cache`, on the broadened corpus
(`--full`; report `--class` grain/cadence/crowd separately).

**R0, record migration (byte-identical).** Introduce the structs, migrate
`aintra` / `pinter` / `ainter` / `amv*` / `sc_*` onto them, with NO formula
changes: composition reproduces today's raw values bit for bit and
`mbtree_mvlambda` is still applied where it is today. Gate: byte-identity on
the full canary matrix.

- **R0.1 is in the tree**: the per-entry array migration (`aintra` -> `d_intra`,
  `pinter` -> `leg[PREV]`, `ainter`/`amv` -> `leg[ANCHOR]`, `sc_*` ->
  `sum_icost` / `sum_cost[PREV]`), with `r_inter = 0` and the mvlambda fold
  kept inside `leg[ANCHOR].d_inter`. Byte-identical: 25 configs, 0 diffs,
  conformance equal to HEAD.
- **R0.2 is only plumbing**: thread the popped `la_entry*` into
  `encode_frame_core`. It is byte-identical and strictly needed only by R2, so
  it can ride with R2. The finish-step `lr_*` dedup is NOT a standalone
  byte-identical step (see section 3) and belongs to R1.

**R1, rate-aware cost plus slicetype/mb-tree consumers.** Sections 4 and 5,
including the AQ unification and the deletion of `mbtree_mvlambda` and
`mbtree_invqscale`. Default output CHANGES for every config with a lookahead
(including CQP with bframes>0, since mb-tree offsets shift); the only
byte-identity holdout is `la_depth==0` paths. Gates: corpus BD, expecting the
motion classes to move and static to hold (the akiyo -6.9% win must survive); a
scene-cut placement diff with near-zero moved cuts; a b-adapt demote-rate
report against HEAD with the 5.2 escape hatch pre-wired; a speed check,
expecting a WIN from the invqscale dedup. Risk watch: IPPP long-chain offsets
(the +/-51 path) on a 300-frame IPPP run, since the rate term is the
convergence mechanism, so verify offsets stabilise.

**This phase measured negative and is reverted** (see the result at the top).

**R2, RC routing.** Section 6. Gates: BD on CRF; BD on ABR at 2 rates per clip
plus delivered-vs-target rate accuracy, which must not regress; VBV runs with
violation counts, which must not regress; the 2-pass canary on CRF/ABR/AQ; and
a recorded `CRF_BASE_CPLX` recalibration sweep. `--qp` CQP without VBV never
calls RC and stays byte-identical through R2.

**R3, CRF/mb-tree coupling.** Section 7, one atomic flip, calibrated pre-merge.
Gates: corpus BD against R2; absolute-rate tracking, with bitrate at CRF
{26..44} within a sane band of R2 at the calibration midpoint (the CRF-meaning
contract); 50/60 fps clips explicitly (park_joy and ducks are 50 fps, the
duration-term trap); an ABR/VBV unaffected re-run. Keep `Y264_RC_LEGACY` one
cycle, then delete.

**R4, backlog:** the I/P/B qscale relation A/B against the frame_qp cascade;
the b-adapt threshold retune; row-SATD for VBV; [list][dist] legs.

Why this design dodges the three recorded dead ends:

1. **IPPP chain saturation** was caused by propfrac -> 1 on rate-free tracked
   motion. The rate term is structural on every propagation path, and the D/R
   split means tuning lambda can never again corrupt stored distortions (the
   old `ainter` fold was destructive).
2. **The fps_factor red herring**: duration terms ship only in R3, only as the
   matched pair (propagation scaling plus CRF term), with fps-diverse gate
   clips. No half-import can recur.
3. **The RD-owns-the-field trap**: section 5.4 forbids lookahead cost and MV
   leakage into full-res decisions. The lookahead influences WHAT a frame is
   and HOW MANY bits it gets, never which mode wins inside it.

## 10. Knobs

None of the knobs below is wired. R1 stopped short of them, so each row is the
default this design intended, not one the tree reads. Setting any of them today
does nothing.

| env | intended default | purpose | state |
|---|---|---|---|
| `Y264_LR_QP` | 12 | lookahead lambda QP (lambda_me table index) | no reader |
| `Y264_LR_RATE` | 1 | 0 = compose all consumers without rate terms (R1 A/B) | no reader |
| `Y264_LR_RATE_BADAPT` | 1 | b-adapt-only rate opt-out (5.2 escape hatch) | no reader |
| `Y264_RC_LACOST` | 1 | 0 = RC keeps frame_complexity (R2 A/B) | no reader |
| `Y264_RC_LEGACY` | 0 | 1 = pre-R3 CRF equation (one release cycle) | no reader |
| `Y264_TPL` | 0 | section 8's TPL build gate | no reader |

The three knobs this design planned to retire are all still live in
`src/encoder/encoder.c`. `Y264_MBTREE_MVLAMBDA` sets the lambda on the
propagation mvcost term (default 8.0), `Y264_MBTREE_PROP_INVQ` toggles the
inverse-qscale weighting of a propagation pass (default 1), and
`Y264_MBTREE_CENTER` (default 0) centres the boost term on the frame mean.
Nothing subsumed or removed them, because R3 never landed.

Existing mb-tree knobs (`Y264_MBTREE_STRENGTH/ADAPT/AINT/ASLOPE/ALO/AHI`,
`Y264_MBTREE_BOTHLIST`, `Y264_MBTREE_IPPP`) all still read their env.
