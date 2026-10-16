# The ML track: what three independent sweeps agree on

This page is research. Nothing here is built and nothing is shipped. Its value
is the quality margin and the post-parity differentiator track. None of it
makes the encoder faster.

Three independent surveys with deliberately non-overlapping briefs fed this
page. The first is a census of the tree's own hand-chosen constants (~290
verified rows). The second is an external prior-art sweep. The third is a
layered opportunity map. What follows reads across them: where they converge,
where they conflict, and how the ranking looks once all three constraints apply
at once.

## 0. Found on the way: one constant that looked unrescaled and measured clean

yah264's SATD is the un-halved Hadamard sum. That is a deliberate, documented
project-wide convention, byte-identical to the naive kernel across 300k random
blocks, and nothing about it needs fixing.

The census flags something narrower. Because SATDs are ratios of themselves,
the scale cancels everywhere *except* where an ADDITIVE constant meets one of
our SATDs. An additive constant chosen for a halved SATD carries half its
intended weight here unless it is doubled.

One candidate, in the lowres intra cost:

```c
return ((best + lr_ipen()) >> (Y264_BIT_DEPTH - 8));   /* lr_ipen() = 5 */
```

`best` is `y264_dsp.satd8x8` (un-halved). If 5 was meant for a halved SATD, the
matching value would be 10. A flat penalty on both the intra and inter lowres
costs would cancel in the intra-vs-inter min but not in the propagate ratio
mb-tree consumes. We do not carry one.

**Settled 08-27: it is a wash and the shipped 5 stands.** The A/B ran as a
VMAF-NEG band at points 22-40 over 9 clips that include both animation kinds. It
read a median +0.02% for 10 against 5. The fixed penalty and its knob have
since been replaced by one tied to the rate factor (`lr_intra_penalty` in
`src/encoder/encoder.c`, which records the sweep). Per-clip spread exists (bus -8.07 and bbb +6.97, with both
rows saturation-flagged). That spread is M4 evidence and says nothing against the
default. This was M0, the census's first candidate. It went the same way as M1's
other staleness suspects: the constant measured clean.

## 1. The shape all three arrived at independently

None of the three recommends a neural network in the encode loop. All three
converged on the same delivery pattern:

**Offline-fit screens, compiled to C tables, over telemetry this tree already
emits.**

The precedent is production code. libaom ships offline-trained MLPs, CNNs and
SVMs for partition, mode and TX pruning as compiled-in weight tables. They
run as plain C with no ML runtime at build time or run time. That settles the
deployability question that usually sinks this conversation. It also matches
our constraints exactly: integer tables keep determinism and add no dependency.

The prior-art sweep also removes the reverence from the constants we would be
replacing. The lambda law every encoder uses is a 1998 regression fit
(Sullivan & Wiegand, lambda = 0.85*Q^2). x265's second (rate) lambda, exponential in QP,
is a curve fit to HM's curve fit. These are two-decade-old fits to corpora nobody has any more.
Per-clip lambda re-fitting is measured at ~1.9% median BD and up to 24% on
individual clips.

## 2. The ranking, all constraints applied

### Rank 1: re-sweep the stale constants. No ML at all.

The census found **22 open instances** of the pattern this project has already
caught five times: a constant measured as "the optimum" before the encoder
changed around it. psy-RD 1.0 -> 2.0, AQ 1.0 -> 0.4 and the CRF_CPLX anchor
5.5 -> 4.5 were all this. Every closed ledger was measured on a pre-flip
encoder.

The highest-confidence open instances:

- the RDOQ seed 32, swept while 4x4 inter RDOQ was *unreachable* at medium (the
  transform-size fix re-opened it)
- the intra admission margins 24/16, swept before four reshaping ships
- the B8_QGATE=10 knee, measured only on ship day
- mb-tree strength x0.7, fitted to compensate a coarse lowres ME that has since
  been upgraded. Nobody has re-tested the coupling.

This is pure harness work with zero design risk. The psy-2.0 precedent says at
least one number moves. It ranks first because it is the cheapest thing on the
page, and because **fitting a model on top of a stale constant inherits the
staleness**.

A machine-checkable version exists: compare each constant's last `git log -S`
touch against the reshaping-ship list. The blocker is that the worst offenders
have no knob at all (b-adapt 0.90, the hpel 7/8 ratio, the iteration caps, the
ABR five, the qcomp/blur set). That is itself the argument for knobbing them.

### Rank 2: the transform-size classifier

This is the only item where all three lists intersect exactly. It is also the
best-posed supervised problem in the tree:

- **Free exact labels.** `Y264_TR_PRE=0` is an oracle, not an approximation.
- **Measured headroom.** The oracle is -1.58% median; the shipped bias-105
  heuristic gets -1.30%. That 0.28% is sitting there.
- **Zero wall cost.** The decision is already being made. A better decision is
  not a slower one.
- **Features already dumping.** `Y264_RESPROF` emits the margin buckets.

The shipped heuristic is a recorded *cliff* with a near-random per-MB
predictor. That is the signature of a scalar standing in for a function. Fit
p(4x4 | c4, c8, margin, QP, energy).

### Rank 3: calibration debt in the bit estimate

Prior art says to regression-fit the estimate against true CABAC bits. The
oracle is free and there is no runtime cost. The census independently found the
specific defects. **P_Skip is priced at a flat 1 bit** and the intra mode bonus
at a flat 4, both lambda-blind. B_Skip on the est path is *already* priced
context-accurately. A lambda-scaled bonus is the obvious alternative; ours stays a flat 4.

This is arguably not ML at all. It is a correctness gap in the rate model. It
points straight at the standing finding that the last 8% of the RD gap is the
bit estimate.

### Rank 4: per-content parameter selection

All three named this under different labels: a per-clip knob selector,
per-shot lambda prediction and per-shot CRF_PBSCALE. It is one item.

The argument is unusually strong here. **The constituent wins are already
measured and were refused only because they are narrow.** They include the AC
gain 1.0-vs-1.7 conflict, the psy-flat gate (sintel -7.68, refused globally on
samsung's +30% wall), CRF_CPLX's flat-content losers and the ME_ET rescue rows.
CRF_PBSCALE alone shows the largest per-content spread in the census. One
scalar moves ducks -8.92% and sintel +7.00%. A selector converts the standing
portfolio of narrow positives into a single win.

**The first move costs nothing:** compute, from band results already on disk,
what a *perfect* per-clip selector over the five recorded conditional arms
would have been worth. That is arithmetic on existing files. It sets the ceiling
before any code is written.

The delivery path exists in the shot plan's per-GOP rc hook. The model must be
tiny and dependency-free: a decision tree compiled to C.

### Rank 5: joint knob fit (CMA-ES / TPE)

The case is a signature in our own record: "each null alone, large together"
recurs across subsystems. That is what a coupled parameter space looks like
when coordinate descent is the only tool that has been used on it. ~15-25
shipped constants, leave-one-clip-out, noise-aware evaluation against the
recorded floors.

### Research spikes, not near-term

The early-skip surrogate has a measured ceiling of 6-10% reachable wall.
Three dead hand-gate generations are behind it, and `Y264_BLATE_STAT` already
holds ~629k labeled rows. Kill-test it offline before touching the encoder.
Learned mb-tree consumption would be one joint re-parameterization of the
most-refused subsystem in the tree, worth exactly one shot. Saliency AQ is
blocked on a metric the NEG gate cannot see.

### Dead on constraints

Per-MB conv nets are out. The budget is 100-300 ns/MB, derived from the
measured 1.9-11 us/MB tournament cost. Pixel CNNs are 100-1000x over it, and
two independent estimates agree. Also out: anything decoder-side (learned
entropy coding, transforms, interpolation, CABAC adaptation past
`cabac_init_idc`), RL for VBV before an oracle exists, and GPU-resident
per-frame models on the default path. Those pay the measured 12-17 ms Metal
per-process floor per table cell.

## 3. Where the surveys conflict

**Film grain.** The opportunity map proposes learned denoise plus grain
re-synthesis via FGC SEI as the largest BD ceiling available. The prior-art
sweep reports that film-grain SEI is effectively dead in H.264 deployment
because decoders do not honor it. Both cannot be acted on. The resolution is
that the denoise half stands on its own (fewer bits spent on grain the viewer
partly loses anyway). Without re-synthesis it is a *quality-tradeoff* feature
for a VOD tier. It is not a free BD win, and it must only ship described that
way.

**What ranks first.** The opportunity map put the per-content selector at the
top, and prior art put the tournament/skip screen there. The census
arbitrates. The transform-size classifier beats both on posedness because its
labels are a free *exact* oracle and its wall cost is zero. Neither of the
others can claim that.

## 4. The constraint that binds everything

Bristol's BVI-DVC work found that training-corpus choice moved results by up to
10.3%, more than most architecture changes did. **The 12-clip gate corpus must
stay test-only.** Training data comes from external sources that the gate never
sees. Any result produced by training on the gate corpus is uninterpretable.
That is the single most likely way for the whole track to produce a confident
wrong answer.

Also binding: determinism (models ship as integer tables), no runtime
dependency, the recorded noise floors (deep +/-1.2 per clip, ABR 0.2-11, table
dVMAF < 0.05), and the t1-only restriction on the per-decision emitters.

## 5. How anything here gets trained

The protocol is written before any fitting starts. That keeps the
cheap-to-prevent failures prevented. It covers train/dev/gate separation with a checked-in data
manifest; labels ranked by fidelity, exact oracles first; causal features only;
integer tables that compile to C with no runtime dependency; the ceiling
measured before fitting; an offline kill-test before the encoder is touched;
clip-level cross-validation; per-clip reporting against the recorded noise
floors; and an unchanged gate. Every fitted table also needs a provenance
header whose `REFIT IF:` line names the encoder changes that invalidate the fit.
A fitted table goes stale exactly as a swept constant does. It is also harder
to eyeball.

## 6. Public documentation: after, not before

This gets written up as a feature once the work is done. Nothing here is
trained yet, so we could not back a "trained coefficients" claim today. The rule
is that published rows carry our own measurements.

The write-up therefore belongs to the shipping items instead of being a doc
task of its own. Whichever of ranks 1-4 ships first carries it, with the measured
numbers in hand. It is worth saying plainly then, because the substrate is
unusual for an encoder tree. It has per-decision telemetry, working oracles,
and constants fit to a corpus instead of guessed. `Y264_BLATE_STAT` alone is a
purpose-built per-B-MB feature-vector dump that holds ~629k labeled rows.
