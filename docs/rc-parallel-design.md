# RC-parallel design: deterministic fixed-lag feedback (Y264_RC_PIPE)

> 2026-10-04: the pipeline is the only rate-control path now. The serial
> controller and its `Y264_RC_PIPE` / `Y264_RC_PIPE_VBV` escapes were removed;
> the A/B results below that use them are history.
>
> `Y264_RCP_LAG` has defaulted to 1 above one thread since commit 94bc84c, so
> ABR output at more than one thread varies with the thread count by design.
> The sections below that call the lag refused or default 0 predate that.


**Goal: the MT frame pipeline serves every rate-control mode, CRF/CQP and the
bitrate modes alike.** Without this design ABR/VBV/2-pass disengage all of it.
`emit_frame_w2`'s `rc_waits` drains the previous frame's coded bits before
setting this frame's QP. `fpipe_ready` / `stair_clamp_on` refuse those modes
outright. The bitrate-mode tiers therefore read flat while CRF single-GOP gains
1.32x.

This design runs ABR frame-parallel. It re-estimates the rate model against
PREDICTED bits for in-flight frames and corrects when actuals arrive (an
in-flight prediction, post-frame accounting and an overflow-buffer feedback).
**Its output must not vary with thread count.** The
feedback schedule below is fixed by CONFIG (env + params + input), never by
runtime timing, engagement, or thread count. RC_PIPE=1 output is therefore
identical at t1 and t18 and run-to-run. That was the design's core invariant
when it was written. It no longer holds above one thread (see the note at the
top), and the owner's rule no longer asks it to: two runs at `--threads 1` must
be byte-identical, and identity across thread counts is informational only.

## The model: decide with predictions, account on a fixed schedule

RC state splits into a **committed ledger** (`abr_scale[]`, `abr_cum_actual`,
`abr_cum_target`, `tp_rem_target`, `tp_rem_cq`, advanced ONLY by actual coded
bits, in coding order) and a **pending FIFO** of decided-but-unaccounted frames.
Each pending entry is created at the frame's QP decision and carries:

    { type, is_ref, fqp (coded QP at decide), base_qp (e->qp at decide),
      rceq, cplx, cq, pred (predicted bits), bits (actual; unfilled until
      the NAL is retired), seq (decide sequence number) }

Three operations, each at deterministic program points:

- **decide(i)**: serial, coding order (the "capture, don't synchronize"
  discipline: on the API thread at the arrival/launch/prep site, or on the stair
  driver strictly between the `serial_done` / chain-submit handshakes). Computes
  frame i's QP from the committed ledger PLUS the pending entries' predictions,
  then pushes its own entry.
- **fill(i)**: the frame's actual bits become known (W2 drain, stair drain,
  fpipe append, serial append). It always runs on the API thread in coding order
  and stores bits into the oldest unfilled entry. It never touches the ledger.
- **pop**: moves a filled entry's actuals into the committed ledger (model
  recalibration + budget). The schedule rule is the whole determinism argument:
  **an entry pops once it is filled AND a non-B frame has been decided after it**
  (entry.seq < seq of the newest type!=2 decide), oldest first.

### Zero-lag anchors

The burst-lagged anchor schedule applies to B frames only. Rate accuracy forces
it. **Every driver stages all prior actuals before a NON-B decide** (`rc_waits`
keeps the W2 drain for non-B frames; the staircase drains the fly burst before
the launch decide), and a non-B decide commits the whole ledger on entry.
Anchors therefore decide on FULL actuals (zero lag). Only B frames decide on
predictions, from their own burst, corrected at the next anchor.

With lag forced to zero the rcp equations match serial within 0.04pp, so every
accuracy loss was the anchor lag. The overlap given up is the
anchor-vs-burst-tail window, ~0.5-2 ms per anchor.

Three warm-up devices go with it:

- P/B seeds derived in the lowres-Cme domain. A SATD-domain seed reads 2.8x low
  for P. (2026-09-04: under the rate-factor controller the OPENING QP is no
  longer a resolution-only seed. `Y264_ABR_RF2_SEED=2` fits it at the first
  decide on the lookahead ring's median inter cost per macroblock and the target
  bits per macroblock. The cumulative rate factor never forgets its first
  second. The old seed opened high-rate cells at QP 6-17 against settling points
  of 27-34. See docs/knobs.md and the encoder's `rf2_open_qp` comment.)
- First calibration SNAPS over the seed. An EMA drags it across a whole burst.
- A 12-decide config-keyed WARM PHASE per encoder (serial-tight feedback,
  pipeline disengaged, pops at fill) so models calibrate in-domain before any
  lag. The CLI's per-GOP encoders restart the transient every keyint.

In-flight predictions re-evaluate against the CURRENT model at every decide. The re-calibration knobs are `Y264_RCP_WARM` (12),
`Y264_RCP_GAIN` (0.1) and `Y264_ABR_QCOMP` (0.6). All are warmed statics.

### Why the pop rule makes output engagement-invariant

The RC-visible sequence is purely decides and pops. Fills stage data but do not
change what a decide reads. Group frames into bursts: a non-B anchor plus the
B's coded after it, where an IDR or a bframes-0 P is a one-frame burst. The pop
rule yields this sequence on EVERY path:

    ... D(anchor_n), POP(all of burst n-1), D(B_n1), D(B_n2), ..., D(anchor_n+1), POP(burst n), ...

- Serial (pipeline off at t1, or plain emit path): each frame's bits fill at its
  own append. The pop still waits for the next anchor's decide, so the schedule
  is emulated exactly.
- W2 emit-overlap: the previous frame's bits fill mid-call after analyze. That
  is after D(anchor) but before any later decide. Same sequence.
- Staircase (sync, async, depth-2): `stair_launch` decides the anchor, THEN
  `stair_drain` retires the previous burst (fills), THEN the chain decides the
  B's. Same sequence. Fill timing differs across depth and engagement. The
  decide/pop order never does.

An anchor decide always sees actuals through burst n-2 plus predictions for
burst n-1. A B decide sees actuals through burst n-1 plus predictions for the
current burst's earlier frames. **Effective lag K = one burst** (up to
bframes+1 frames; bframes 3 gives 4). In the degenerate shapes, consecutive-P
streams give K = 1 frame. Env off gives K = 0 with the exact serial equations
and byte-identical behavior, because the whole machinery is bypassed.

Availability is guaranteed structurally. A pop needs burst n-1's bits only after
D(anchor_n), and every engaged path retires burst n-1 (stair_drain / W2 drain)
before any burst-n B decide. No pop ever blocks.

Mid-stream flush pops everything filled, because the flush path drains every
burst and pending emit first. That is a property of the caller's API call
sequence and is identical at every thread count.

## The prediction function (pure function of pre-dispatch state)

- **ABR**: `pred = S_type * rceq / qscale(fqp)` where `rceq = C^(1-qcomp)`,
  `S_type` is the committed per-type scale at decide time and `fqp` the coded QP
  the decision produced. This is the exact inverse of `rc_account`'s calibration
  (`s = bits * qscale / rceq`), so a perfectly-calibrated model predicts its own
  accounting. Prediction error is then exactly model error. The existing
  correction absorbs it: `err * 0.1` against the virtual cumulative ledger.
- **2-pass**: `pred = scaleterm / qscale(qp)` with `scaleterm = (bits1 + 1) *
  qscale(qp1)` from the pass-1 stat. `rc_set_qp_2pass` plans with the same model.
  The virtual remaining budget is `tp_rem_target - sum(pending pred)` and
  `tp_rem_cq - sum(pending cq)`.
- **Pass 1**: no feedback at all (fixed QP 26); entries exist only so stat lines
  append in coding order at pop time, regardless of completion order.

### The complexity input must not read recon

Serial ABR's `frame_complexity` compares the source against `e->ref[0]`. That is
the previous anchor's RECON, which is still mid-stream under the staircase.
Reading it would be a timing-dependent race. Under RC_PIPE the decision
complexity is:

- type 0 (IDR): `frame_complexity(src, intra=1)`, source-only, unchanged.
- type 1/2: the ME-compensated lowres cost `frame_complexity_me` **captured at
  the frame's arrival** on the API thread. CRF uses the same signal, captured
  per frame into `arr_cme` / `bcplx[]` / the burst's `bcplx[]`. It is a pure
  function of source content: deterministic, race-free, and cheaper than the
  full-res SATD walk. The per-type scales self-calibrate to the new signal's
  magnitude. Only the first frame of each type runs on the seed, bounded by the
  existing +-4 QP swing limit.

## Per-mode treatment

- **ABR.** Decide/account as above. Rate accuracy is a first-class gate. At 3
  bitrates x 4 clips, |achieved - target| for RC_PIPE=1 must be within 1.25x of
  serial ABR's own deviation, per clip.
- **2-pass.** Pass 2 QPs derive mostly from stats; only the re-planning term takes
  the lagged treatment. Pass-1 stat lines append at pop time in coding order (the
  deferred-drain machinery orders the NALs; the FIFO orders the stats
  identically).
- **CRF/CQP.** Unchanged; RC_PIPE is inert there.
- **VBV** (`Y264_RC_PIPE_VBV`, a sub-gate that defaulted ON until it was
  removed with the serial path). The buffer law
  `fill += rate - bits` is safety-critical. Instrumentation confirmed the
  design's fear: the per-frame actual/predicted ratio has an UNBOUNDED tail
  (p50 ~0.85, p90 ~2, p99.9 in the hundreds). The tail comes from near-zero
  predictions on static content and from missed-cut regime shifts. No fixed
  r_max alone is sound, so what ships is hardened by four devices. Each is a pure
  function of the decided ledger (see the VBV section below).

## Implementation shape

- Env gate `Y264_RC_PIPE` (default ON; warmed static; removed since, with
  `Y264_RC_PIPE_VBV`, so the pipeline is now unconditional). Effective:
  `e->rcp_on = env && (abr_on || tp_pass || vbv_on) && (!vbv_on ||
  Y264_RC_PIPE_VBV)`. The VBV sub-gate defaults ON, so VBV runs through the
  pipeline unless you set it to 0.
- `emit_frame_w2`: `rc_waits` becomes `(abr_on||vbv_on||tp_pass) && !rcp_on`, so
  the drains stop forcing serialization. The RC head decides from the lagged
  ledger, and `w2_drain`'s RC tail becomes a fill.
- `fpipe_ready` / `stair_clamp_on` admit ABR/2-pass when `rcp_on` (VBV never).
  The stair MV clamp stays env-keyed, so it applies at every thread count and
  bits stay thread-invariant. Its BD cost (measured ~zero) covers ABR/2-pass
  too.
- Decide sites mirror the CRF heads: `stair_launch` (the serial arrival phase),
  `stair_prep_b` (sync on the API thread; async on the driver, where the
  `serial_done` / chain-submit handshakes already order every RC access),
  `stair_serial_b`, `fpipe_prep_leaf`, and the emit_frame heads. Fill sites:
  `w2_drain`, `stair_drain` (anchor NAL + stash, coding order), `code_b_pair`
  appends, the serial append. A defensive prep-bail (shared-hpel fallback,
  unreachable for pyramid DPB refs) re-codes an already-decided frame serially.
  A `predecided` counter then consumes the pushed entry instead of deciding
  twice and restores `base_qp`.
- CAVLC overflow (W2 drop path) marks the entry dropped. Pop removes it without
  accounting, mirroring the serial skip.
- Ring capacity 24 (structural max in flight: one burst <= 8 + next anchor + W2
  pending, with headroom).

## Measured results (on the default-off build)

- **Determinism**: t1==t8==t18 across 9 ABR configs (bf0/2/3/7, CAVLC, keyint40,
  ref1, AQ, cut-heavy sintel) plus two-pass (stats, pass-1, pass-2); x5 t18
  repeats; flush-torture 10/10 cut points. Engagement-invariance was measured
  directly: identical bits with the pipeline envs on and off.
- **Default identity**: w2_canary 26/26 vs fresh HEAD; 32/32 default + NO_ASM
  matrix including ABR and two-pass; make test 9/9; conformance --fast 249/249
  with AND without the env; TSan 0 warnings x8 engaged configs; stress 0 hangs.
  TSan found one real race, the arrival-side warm check racing chain decides.
  Making `rcp_seq` atomic fixed it.
- **ABR rate accuracy** (3 bitrates x 4 clips, |achieved-target| vs serial): 9/12
  cells BEAT serial (mobile 0.4-0.5x, park_joy 0.5-0.9x, samsung ~1.0x).
  Foreman's 3 cells read 1.33-2.04x of serial's unusually tight deviation but
  stay <= 2.9% absolute. Matrix mean |dev| ex-samsung is 1.58% vs serial's 1.91%
  (samsung's ~25% undershoot is pre-existing serial behavior, ratio ~1.0). The
  1.25x-per-clip bar reads 9/12. The miss comes from foreman's serial luck, with
  no rcp bias behind it.
- **2-pass pass-2 accuracy**: 11/12 cells 0.87-1.11x of serial. The one "1.40x"
  is 0.31% vs 0.22% absolute, a ratio artifact at stats precision.
- **BD at matched target bitrates** (5-pt VMAF-NEG, 120f, discriminating band):
  akiyo -8.04, bus -1.42 (saturation-flagged), coastguard -1.05, foreman -3.16,
  mobile -3.36, stefan -0.41, tempete -0.60, park_joy +2.72; mean -1.92% (rcp
  FAVORED; negative = fewer bits at equal quality). park's +2.72 is allocation
  (clamp-off measures WORSE, +5.13). An `Y264_ABR_QCOMP=0.7` probe detonated
  (+45%), so qcomp stays 0.6.
- **Speed** (interleaved best-of-3, single-GOP 500f, RC_PIPE=1 vs 0): park_joy
  t18 **1.66x** / t8 1.38x; samsung t18 **1.61x** / t8 1.50x; park_joy keyint 250
  t18 1.31x. Bar was >= 1.15x.

## Gates (all must hold before any default flip is discussed)

Conformance case counts appear three times in this file (249, 210 and 252).
None of them is a fixed total. `conformance.sh` builds its case list from the
corpus present and from `--fast`, so quote the run instead of the number.

1. Env off: byte-identical to fresh HEAD (full config matrix including ABR and
   two-pass, default + YAH264_NO_ASM=1), w2_canary 26/26, make test 9/9,
   conformance --fast 249/249.
2. RC_PIPE=1 (with the stair/fpipe envs): t1 == t8 == t18 md5 and x5 t18 repeats
   on ABR and two-pass configs; flush-torture across cut points; keyint 40 plus a
   cut-heavy clip (IDR boundaries never overlap; the schedule still holds through
   them).
3. Rate accuracy per the ABR bullet above; same for 2-pass pass-2.
4. BD at matched target bitrates, 7 CIF clips + park_joy: mean within +-0.3%
   VMAF-NEG, per-clip reported.
5. TSan t18 clean with ABR and two-pass engaged; stress 0 hangs; conformance
   --fast WITH the env.
6. Speed: ABR park_joy + samsung, t8/t18, RC_PIPE=1 vs 0 under the stair envs:
   >= 1.15x t18 single-GOP.

## VBV under the pipeline (Y264_RC_PIPE_VBV, default ON)

The knob is gone now and VBV always runs through the pipeline. The
`RC_PIPE_VBV=0` comparisons below are history.

This covers every VBV combo: ABR/CRF/CQP/2-pass with a vbv constraint. CRF/CQP
enter rcp only when VBV is on, and pure CRF/CQP are untouched. The contract is
harder than ABR's because the CPB must never underflow on any frame. The design
goal is **pipelined compliance == serial compliance**, verified by the checker.

### Mechanism

- **Buffer ledger**: `e->vbv_fill` advances ONLY on actuals, at pops
  (rcp_account), like the abr ledger. The serial single `vbv_scale` model is
  untouched (RC_PIPE_VBV=0 is byte-identical to HEAD).
- **Virtual buffer at decides**: a decide sees committed fill plus, per in-flight
  entry, `rate in - r_hi * vpred out` (rim-clamped per step, `Y264_VBV_RHI` = 2).
  Anchors are zero-lag: the pending set is empty and the computation is exactly
  serial. Only in-burst B decides see the conservative bound.
- **Per-burst fallback gate** (`rcp_vbv_gate`): at anchor ARRIVAL on the API
  thread, every in-flight frame is drained first (the rcp zero-lag discipline
  applied one step earlier). The gate then simulates the coming burst at r_hi
  (or the shock multiplier, if larger) times the model predictions. If any
  prefix underflows or a content jump trips, the burst runs the serial K=0
  schedule. Stair/fpipe disengage, rc_waits is forced and entries pop at fill.
  That is the warm-phase schedule, byte-for-byte the serial VBV information
  flow. IDR bursts and the warm phase are unconditionally tight. The gate is a
  pure function of decided state (config + content), never of timing or thread
  count.

### The tail devices and what forced each

A forced-pipelined corpus run (`Y264_VBV_FORCE=1`, 12 runs) showed the
actual/predicted ratio tail is unbounded. The compliance A/B produced four
concrete failures, and their fixes are the scheme:

1. **Robust model domain**: the VBV bits model runs per type on `max(cme,
   lowres_intra/4)`, one domain for all types. cme alone reads ~0 on static or
   noisy content (samsung's 3 Mbit sparkle anchors). Intra energy alone is flat
   across motion swings (sintel regressed on it).
2. **Extrapolation guard** (`Y264_VBV_QPD` = 6): a pipelined B decide may not dive
   more than QPD below the model's calibrated QP regime (EMA of accounted coded QP
   per type). On static content the ABR ladder chased a full buffer down 4 QP per
   DECIDE. Serially that is per actual frame, but in flight it is per burst slot,
   and the first actual came in at a 40x overshoot (akiyo).
3. **Shared shock-propagation scale**: predictions use max(per-type scale, shared
   cross-type scale). Serial survives regime shocks because its single scale snaps
   up on a mispredicted 1 Mbit IDR and inflates the very next B's clip
   prediction. Isolated per-type scales stayed blind for a whole burst.
4. **Shock multiplier**: a decaying max of recent accounted overshoot
   (bits/vpred, cap 16, decay 0.7/account) scales the clip prediction and the
   gate margin. The model is trusted only as far as it has recently earned
   (sintel's cut runs). It sits at ~1 in steady state. Alongside it, the
   **content-jump gate** (`Y264_VBV_CJUMP` = 4) sends a coming frame whose
   complexity jumps past the calibrated regime to the serial schedule.

### Measured results (6 clips x tight bufsize=maxrate/2 + typical =maxrate)

- **Compliance** (scripts/vbv_check.py: Annex-B AU split + leaky-bucket CPB, exit
  1 on underflow): pipelined underflows NOWHERE serial passes. 11/12 cells are
  OK in both. sintel-tight fails BOTH (serial 12 underflows / min-fill -354 kbit;
  pipelined 4 / -178, fewer on both counts). That config is infeasible
  for this encoder's VBV, serial included.
- **Rate accuracy**: |achieved - target| BEATS serial in 12/12 cells (0.02x-0.81x
  of serial's deviation). Serial VBV overshoots targets 11-30% toward maxrate,
  while pipelined tracks 0.1-10.6%.
- **Fallback rate**: 6.6-15% of bursts on normal content and 17-31% on the
  adversarial clips (samsung fades, sintel cuts). No clip reads 0, so the gate
  is live.
- **Determinism**: t1==t8==t18, x5 t18 repeats, across ABR+VBV (bf2/bf3,
  CAVLC+CABAC, keyint40, tight+typical), CRF+VBV, CQP+VBV, 2-pass+VBV;
  flush-torture (mid-stream flush at 8 points including mid-burst, foreman +
  cut-heavy sintel) identical across thread counts.
- **BD at matched configs** (capped-VBR maxrate=bufsize=bitrate, 5-pt, VMAF-NEG,
  pipelined vs serial): mobile -1.12, akiyo -12.55, tempete -1.94, samsung +0.21,
  park_joy +6.33 (the park allocation residue: ABR's +2.72 amplified under VBV),
  foreman -2.37 and sintel -51 saturation-flagged. The unsaturated mean is -1.81%
  (pipelined favored).
- **Speed** (single-GOP 500f/180f, best-of-3, vs serial VBV): park_joy t18
  1.34-1.39x / t8 1.16-1.17x; samsung t18 1.30-1.33x / t8 1.19x. That is smaller
  than ABR's 1.61-1.66x, as expected. Fallback bursts serialize, and the VBV gate
  drains the fly burst at every anchor arrival.
- **Battery**: default byte-identical vs fresh HEAD (w2_canary 26/26 + VBV configs
  t1/t8 + NO_ASM spots); `Y264_RC_PIPE=0` escape == HEAD; make test 9/9;
  conformance --fast 249/249 with AND without the env; TSan t18 0 warnings x4
  engaged VBV configs; stress 0 hangs / 120 concurrent encodes.

VBV without the sub-gate keeps the fully-serial path, byte-identical.

## What the in-burst B lag costs

In-burst B's decide on their own burst's predicted bits, corrected only at the
next anchor. Every clip's bursts show real reallocation noise from this. A
burst-by-burst frame-complexity trace on park_joy and foreman shows anchor bits
swinging ~2x between the serial and pipelined schedules at matched total budget.
The mechanism is universal. Whether it costs quality depends on the clip.

park_joy_720p is the corpus clip that loses in both modes (ABR +2.72, VBV +6.33
above), while the rest are neutral-to-favored. Two content properties turn the
noise into a net loss there. Heavy film grain and water/foliage detail make
frame-to-frame complexity swing faster and less smoothly than in a typical CIF
scene, so the burst's own predictions (extrapolated from the anchor's regime)
miss by more. VMAF-NEG is also specifically sensitive to texture and grain
retention, so bits misallocated AWAY from a grain-heavy frame cost more NEG than
the same misallocation on a clean clip.

**Attribution method, worth reusing.** `Y264_RC_PIPE` toggles the schedule but
not the thread count, so a clean A/B needs one thread count. `RC_PIPE=0`
(serial) against the default at `--threads 1` isolates the RC mechanism alone.
To split the mechanism further, set `Y264_RCP_WARM` to a huge value. That forces
every decide through the warm-phase branch (pops at fill, pending set always
empty, via `rcp_pop_ready`'s `p->seq <= RCP_WARM` disjunct). Every OTHER rcp
device stays exactly as shipped: the arrival-captured lowres complexity domain,
the seed/snap calibration and the VBV virtual-buffer bookkeeping. Only the burst
lag goes. The three configs share the domain machinery and differ only in lag:
serial (K=0, old domain) / `RCP_WARM=huge` (K=0, rcp domain) / default (K=one
burst, rcp domain).

**The lag dominates.** The sweeps were a 3-pt narrow, a 5-pt and a
7-pt/240-frame wide sweep spanning ~1.5-11 Mbit. In every one, huge-vs-default
(lag only, domain held fixed) showed pipelined LOSING to its own zero-lag twin by
a clear margin of the same sign: ABR -0.82% (3-pt) / -5.05% (7-pt wide), VBV
-6.26% (5-pt) VMAF-NEG. That alone accounts for essentially all of the measured
total (7-pt ABR: total -5.11%, lag-only -5.01%). The domain-only split (serial vs
huge) was NOT reliable: it flip-flopped sign between the narrow 3-pt sweep
(appeared to cost ~1.8-1.9%) and the 7-pt wide sweep (shrank to -0.10%, noise
level). akiyo's domain-only read ranged +8.4% to +18.9% favoring the ME domain
across two sweeps. Its TOTAL read flipped sign entirely between a narrow and a
wide sweep. The lag reads never flipped sign in either mode, and the domain reads
did. **Treat the lag as confirmed and the domain question as open. Don't count
the domain as a second contributor.**

### Calibration fixes tried and rejected

All env-gated probes, removed after measurement; none shipped.

1. Feed ABR's `C` the VBV robust domain that already exists (`rcp_cur_cvi =
   max(cme, ilr*0.25)`) instead of raw `cme`: **zero effect**, byte-identical
   output. Both terms live in the same lowres (half-res) domain, and the floor
   never takes effect at park_joy's motion levels. This rules out the cheapest
   possible calibration fix.
2. Replace `C` with `frame_complexity_ilr` (lowres intra energy) alone. This
   helps park_joy (-1.67% NEG) and every CIF clip tested (foreman -1.49, mobile
   -1.30) but **wrecks akiyo (+13.03% NEG)**. akiyo's near-zero `cme` on
   well-matched near-static content is a CORRECT read. Pure intra energy cannot
   tell that case from a blind spot.
3. Geometric-mean blend `sqrt(cme * ilr)`: the same failure mode and worse, akiyo
   +22.06% NEG. The smaller term dominates a geomean. akiyo's cme is ~20-40x
   smaller than its ilr, so the blend inflates its predicted complexity far more
   than intended.
4. A race-free FULL-RESOLUTION texture-energy term (`frame_texture_full`,
   source-only 4x4 SATD vs local flat DC, same domain as the IDR path's
   `frame_complexity(e,src,1)` but never touching `e->ref`). Its magnitude does
   NOT cleanly separate "grain the lowres plane cannot see" from "motion the
   lowres ME correctly compensated". Individual foreman P-frames showed cme:ftex
   ratios (~10x) as extreme as park_joy's. A floor or blend built on it risks
   inflating predicted bits on ordinary well-predicted motion corpus-wide.

All four point at the domain axis, and the wide-sweep measurement shows the
domain is not the confirmed driver. **Do not re-chase this as a "domain" bug. It
is a lag bug with no cheap fix.**

**Partial feedback is the architecturally correct fix.** It has an early
in-burst B's actual bits feed a later B's decide before the next anchor. That
needs the burst chain's B preps fully privatized: B preps whose shared-scalar
writes are fully privatized would let `serial_done` fire at launch. This is a new
pipeline-structure project, not a rate-control parameter change. It also touches
exactly the byte-queue ordering that the whole RCP determinism argument rests on.

## `Y264_RCP_LAG`: the lagged schedule, measured and refused

This refusal was later reversed. Lag 1 became the default above one thread in
commit 94bc84c, once each in-flight frame was predicted from its own
complexity (`Y264_ABR_RF2_INFLIGHT`). At one thread it's byte-identical to lag
0 (`rcp_lag_env` and `rcp_lag_for` in `src/encoder/encoder.c`). The section
below is the record from before that.

Zero-lag anchors trade overlap for accuracy. That trade is what keeps a wide
staircase ring from engaging under ABR at all. `stair_run_burst` retires
everything before each anchor decide, and width is deferred retirement.

**The drain cannot be narrowed, only lagged.** Retirement is oldest-first, so a
wide ring holds the NEWEST bursts and the immediate predecessor is the last thing
that can still be flying. Any drain that leaves a burst live is a lagged decide.
`Y264_RCP_LAG` (default 0 when this ran) buys that lag back in coding order. The lag has to
live in `rcp_pop_ready`'s seq bound as well as in the drain. Deferring only the
drain makes bits move with the thread count, because the serial path has
everything filled and takes it all.

**The cost of zero lag is small per anchor and large per encode.** foreman 300f
bframes 3 goes from 402 ms to 282 ms at lag 1. That is 1.6 ms per anchor over 75
anchors. A CIF anchor's whole budget is about 5.4 ms, so the "small" number is
30% of the encode.

**Plain ABR has VBV's windup and needs the same guard.** One burst of anchor lag
makes the ladder step the full swing limit for several decides running before
any actual arrives. Per-decide QP sd goes from 4.52 to 11.66, pinning at 51 and
walking back. `Y264_VBV_QPD` only applies when `vbv_on`. ABR never needed the
guard because it never had anchor lag. `Y264_RCP_QPD` (default 0, applies at
every lag including 0) is the same device and cuts the lag's BD cost from +44.8%
to +5.5%. At QPD 6 rate accuracy holds: ex-samsung 1.04% against the shipped
1.09%, with 9/12 cells inside this document's 1.25x bar. The residual lag cost
there is +3.49% BD-NEG. At QPD 3 the BD looks better but rate accuracy collapses
to 10.3%, because a ladder clamped that hard stops tracking the target.

**The guard is no quality win on the zero-lag path.** Measured on its own, the
ABR ladder misses its target by 64% at QPD 2 and by 11% at QPD 3 (mean |achieved
- target| ex-samsung, against the shipped 1.09%). Any BD comparison at those
settings is between arms running at different bitrates. At QPD 4 and 6 rate
tracking holds, and BD-NEG against QPD 0 is +7.61% and +0.00%. `Y264_RCP_QPD`
stays default 0 and stays what it was built as: a guard the lagged schedule
needs. The shipped schedule gains nothing from it.

**The lag budget is granted only where it can be used.** `e->rcp_lag` is
`stair_wide_capable(e) ? rcp_lag_env : 0`. `encoder_open` decides it from static
configuration alone: env gates, `b_pyramid`/`direct`, the `--ref` bound, no VBV,
and a pool that reaches `Y264_MT_POOL_MIN`. `rcp_pop_bound`, `rcp_decide` and
`stair_run_burst` read `e->rcp_lag` instead of the env. With the env set, these
stay byte-identical to the zero-lag path: a `--threads 1` encode, a 2-GOP encode
at `--threads 8` (whose per-worker pool is 4), and every CRF/CQP/VBV config.
The 2-GOP case held while `Y264_MT_POOL_MIN` was 8. It's 2 now
(`src/encoder/encoder.h`), so a pool of 4 can engage the lag.

### Where the lag engages and what it costs

Wall clock, medians of 5 (9 for the last two rows). Arms ran round-robin with the
order rotated and a duplicate control in each batch:

| shape | clip | lag 0 | lag 1 | lag 2 | control |
|---|---|--:|--:|--:|--:|
| `--ref 1` | foreman_cif | 203.9 ms | **-21.3%** | -24.3% | -0.2% |
| `--ref 1` | park_joy_720p | 1850.1 ms | **-19.7%** | -20.3% | +1.4% |
| `--ref 3` + `WIDE_REF` | foreman_cif | 225.5 ms | **-10.8%** | -12.9% | +0.2% |
| `--ref 3` + `WIDE_REF` | park_joy_720p | 2079.1 ms | **-13.8%** | -12.4% | +1.0% |
| `--ref 3` + `NOWIDE` | foreman_cif | 227.7 ms | **-10.4%** | -- | -1.3% |
| `--ref 3` + `NOWIDE` | park_joy_720p | 2086.2 ms | **-13.4%** | -- | -0.5% |

So 1.25-1.27x at `--ref 1` and 1.12-1.16x at `--ref 3`. Lag 2 buys almost nothing
over lag 1. At the default `Y264_RCP_QPD` 0 the lag detonates on park_joy at
+48.3% BD-NEG, so it cannot ship without moving the guard's default in the same
change.

**The corpus answer, at `--ref 3` under `Y264_RCP_LAG_NOWIDE`, lag 1 + QPD 6
against the shipped default:**

| clip | BD-NEG | VMAF band |
|---|--:|---|
| akiyo_cif | **+20.66%** | 86.6-94.9 |
| touchdown_420 (1080p) | **+14.23%** | 74.7-87.1 |
| samsung_720p | +6.34% | 77.8-92.1 |
| stefan_cif | +5.09% | 65.4-90.7 |
| tempete_cif | +3.61% | 79.2-91.3 |
| bus_cif | +2.74% | 59.5-88.6 |
| foreman_cif | +2.10% | 75.6-92.7 |
| mobile_cif | -1.00% | 80.1-92.1 |
| park_joy_720p | -2.08% | 81.4-95.4 |
| ducks_720p | -4.78% | 73.9-91.9 |
| coastguard_cif | **-5.77%** | 75.0-89.3 |
| sintel_720p | rate control fails | -- |

Seven clips get worse, four get better, one breaks. No mean is quoted, because
the decision turns on the distribution. A mean of these reads about +3.7% and
hides both the +20.66% and the failure.

**The losers have something in common.** akiyo, samsung, touchdown and sintel are
the corpus's non-stationary clips: near-static content interrupted by motion,
fades and sparkle, sports cuts and pans, hard cuts. The winners (coastguard,
ducks, park_joy, mobile) all have roughly stationary frame-to-frame statistics. A
burst-lagged predictor extrapolates the current burst from the last anchor's
regime, so it mispredicts exactly where the regime moves inside a burst. That is
the park_joy mechanism generalized. park_joy is not the worst case.

**sintel has no BD number because it has a rate-control failure.** The lagged arm
floors at about 2340 kbit/s while the ladder asks for 750, a +212% overshoot.
The shipped arm tracks every point to within a percent. Rate accuracy is
otherwise clean: worst case +8.4% on touchdown, under 3% on nine of the other
ten. The failure therefore belongs to cut-heavy content specifically. It also worsens
as the target tightens: +19% at the higher ladder against +212% at the lower.
It fails this document's own bar outright, which asks for a per-clip deviation
within 1.25x of serial's.

**Verdict at the time: `Y264_RCP_LAG` stays default 0.** The speed is real and the mechanism
is understood. But seven of eleven measurable clips lose, the worst by 20%, and
the twelfth loses rate control entirely. Tuning QPD does not rescue it. The guard
already moved from 0 to 6. These losses are what survived it.

### Isolating drain placement from width

At `--ref 3` ABR does not need width, because CRF does not get width there
either. What it needs is the drain one launch later.

`stair_wide_capable`'s `--ref` term exists for width. A wide ring recycles DPB
slots through the bag pool, and the pool needs `nref <= 1`. That same predicate
hands out the lag budget, so a `--ref 3` encode is refused a lag for a reason
that has nothing to do with what the lag would buy it. On the non-wide path
`stair_run_burst` retires the predecessor after the new anchor's jobs are
registered. A lag of 1 lets one burst survive the prologue to reach that code.

`Y264_RCP_LAG_NOWIDE=1` (default off, measurement only) grants the budget on
async capability instead, through a `stair_lag_capable` that is
`stair_wide_capable` minus the `--ref` term. `stair_wide_nref_ok` still refuses
`wide` and still refuses `dpbp_open`, so no bag pool is allocated. `stair-stat`
reports 0 wide launches and 0 concurrent bursts (confirmed by measurement). The
arm recovers -10.4% and -13.4%, against -10.8% and -13.8% for the same shape with
width also switched on. **Width is worth roughly nothing at `--ref 3`. The drain
placement is the entire mechanism.** That is the finding to build on: some other
way of getting the drain one launch later, without a rate-control lag paying for
it.

## Width and ABR: the two gates

Two independent predicates refuse a wide staircase ring under ABR at `--ref 3`.
`stair-stat` on foreman_cif 180f, t18:

| mode | `--ref` | wide launches | max concurrent bursts | DPB recycles |
|---|--:|--:|--:|--:|
| ABR | 3 | 0 | 0 | 0 |
| ABR | 1 | 0 | 0 | 0 |
| CRF | 3 | 0 | 0 | 0 |
| CRF | 1 | **45** | **3** | 90 |

`stair_wide_nref_ok` wants `nref <= 1` unless `Y264_STAIR_WIDE_REF` lifts it.
That knob now defaults ON, so `--ref 3` no longer refuses width on that clause.
The grid above was measured when it defaulted off. `stair_wide_rc_ok` wants
`rcp_lag > 0`. `Y264_RCP_LAG` defaulted 0 then, so ABR still refused width at
every `--ref`. With `Y264_RCP_LAG=1`, ABR at `--ref 1` reaches 42 launches and 2
concurrent bursts, and `--ref 3` reaches the same 42 and 2. The machinery runs
fine. The rate-control predicate is what refuses to let it start.

**TRAP: a knob measured on a config that cannot engage it reads exactly like a
knob that does not work.** Check engagement before pricing anything on this arm.
`Y264_STAIR_STAT` answers it in one run.

### The ABR-vs-CRF wall gap

Both encoders ran at the same shape, pure-C, t18, medians of 5 with the encoder
order swapped between rounds. Every spread is inside 1.08x. CRF is calibrated per
clip and per encoder to reproduce that encoder's own ABR output size, within 2.5%
on every cell. That calibration is required. At equal CRF the two encoders'
sizes diverge by -55% to +44% on this corpus, so an uncalibrated CRF arm compares
two different amounts of work and its timing delta means nothing.

`--ref 3`:

| clip | ABR | CRF | yah264's own ABR/CRF |
|---|--:|--:|--:|
| foreman_cif | 1.65x | 1.41x | 1.15x |
| park_joy_720p | 1.24x | 1.05x | 1.13x |
| samsung_720p | 1.95x | 1.84x | 1.09x |
| **mean** | **1.61x** | **1.43x** | |

`--ref 1`:

| clip | ABR | CRF | yah264's own ABR/CRF |
|---|--:|--:|--:|
| foreman_cif | 2.25x | 1.60x | 1.40x |
| park_joy_720p | 1.43x | 1.10x | 1.23x |
| samsung_720p | 2.04x | 2.00x | 1.07x |
| **mean** | **1.91x** | **1.57x** | |

x264 is close to mode-neutral on all six rows, within 5% either way, so the whole
spread is ours. `--bframes` 2 against 3 is worth roughly 1.6x on this encoder by
itself. Any ABR-vs-CRF contrast that also moves `--bframes` is mostly measuring
that instead.

### What binds ABR: where the drain sits

At `--ref 3` width is off for CRF too, so width cannot be the difference there.
The difference is when the predecessor gets retired. `stair_run_burst`'s prologue
drains to `nlive == 0` before the anchor decides. That is what zero lag means.
CRF skips that block and retires its predecessor further down, once the new
anchor's jobs are already registered. The predecessor's tail then overlaps the
new anchor's wavefront ramp. It is the same retirement, one launch later.

`Y264_NTP_PROF`, ABR against CRF:

| clip | `--ref` | mode | busy% | ramp% | tail% | bg-sync wait |
|---|--:|---|--:|--:|--:|--:|
| foreman_cif | 3 | ABR | 56.4 | 13.0 | 9.4 | 149.9 ms |
| foreman_cif | 3 | CRF | 63.7 | 9.6 | 8.7 | 84.0 ms |
| park_joy_720p | 3 | ABR | 73.8 | 1.5 | 15.2 | 785.2 ms |
| park_joy_720p | 3 | CRF | 75.9 | 1.5 | 12.9 | 597.2 ms |
| foreman_cif | 1 | ABR | 54.1 | 14.6 | 10.1 | 131.8 ms |
| foreman_cif | 1 | CRF | **74.8** | **0.5** | 4.9 | 65.7 ms |
| park_joy_720p | 1 | ABR | 71.7 | 1.4 | 16.5 | 693.9 ms |
| park_joy_720p | 1 | CRF | 72.6 | 1.9 | 13.7 | **272.4 ms** |

The bg-sync line is the drain itself, since `stair_drain` syncs each chain
driver. It runs 1.3-2.5x longer under ABR on every row. Frames in flight
separates the two gates cleanly. At `--ref 3`, where neither mode gets width, ABR
and CRF run the same number of frames at once: 0.39 B frames against 0.38 on
foreman. At `--ref 1`, where only CRF gets it, CRF roughly doubles: 0.36 against
0.74 on foreman, 0.19 against 0.44 on park_joy. Drain placement costs about 13%
at `--ref 3`. Forfeited width costs a further 10-25% at `--ref 1`.

Two smaller ABR-only serializers come along too and were not priced apart.
`rc_waits` forces a W2 join on every anchor. `rcp_warm` holds `stair_ready` and
`fpipe_ready` off for the first 12 decides.

Opening both gates puts ABR within 2.6% and 1.4% of CRF's own wall (foreman
227.9 -> 202.4 ms against CRF's 197.4; park_joy 2071.9 -> 1799.5 against 1774.9;
batch control ±1.3%). The two gates account for essentially all of the gap.
Everything else in the rate-control path costs a couple of percent between them.

**TRAP: this tree drifts up to 7% between batches**, so a wall compared across
two of them is not evidence. Run every arm in one batch with the order rotated,
and include a duplicate of the first arm as a control.

## `Y264_ABR_EARLY`: drain placement without a rate-control lag

The bit-neutral launch split is structurally absent. The idea was to start the
anchor's ME and analyze against the still-draining predecessor and let only the
decide wait. But the anchor's analyze opens with the rate decision instead of
deferring its use. `y264_frame_analyze`'s P branch computes `int mlam =
lambda_me(f->qp)` and `long lam = lambda_mode(f->qp)` as its first two statements
and threads both by value into every `y264_me_search` and every mode comparison;
`mb_qp_pre` then folds `f->qp` into the per-MB quantizer that drives all of RDOQ.
Motion search, mode decision and quantization each consume the QP, and the QP is
what the drain exists to produce.

So the only work a split could start early is work that never reads `f->qp`.
Almost none of it is left on the pool. The big candidate, half-pel interpolation
of the references, is already built by *earlier* frames. `dpb_store` builds it
for the general case. In the predecessor's own `stair_trailer_task` it is built
per row while that frame's analyze is still running. `build_slice_prep` only
looks the cached triples up. `Y264_THREAD_PROF` prices the whole of `slice_prep`
at 19.1 ms of a 1783.8 ms park_joy wall (1.1%) and 11.2 ms of 821.1 ms on samsung
(1.4%). The drain holds the API thread for 67.1% and 47.4% of those same walls.
That bounds a bit-neutral launch split at about 1.4% of the wall, below this
tree's batch-to-batch noise. Re-opening it means first changing what analyze
consumes.

What can be built instead is moving the drain, in two modes. `Y264_ABR_EARLY`
defaults 2, the shipped drain split. 1 is the unsafe probe, and 0 restores the
zero-lag prologue drain.

**Mode 1** drops the zero-lag prologue drain so the launch happens first. The
late drain then retires the predecessor after this anchor's jobs are registered.
That is the CRF ordering under ABR. Unlike `Y264_RCP_LAG` it leaves
`rcp_pop_bound`'s seq alone, so it isolates placement from the accounting change.

**Mode 2** splits the drain from the decide, because they are not waiting for the
same thing. `B->size` is final when the anchor's RUNNER returns
(`stair_runner_task`). The chain driver also holds the mini-GOP's B leaves.
That is where the tail actually lives. `Y264_NTP_PROF` puts `analyze_Bcb` at
8266 ms of pool time against `analyze_Pcb`'s 5327 ms on park_joy. So
`stair_drain`'s single `ntp_bg_sync(driver)` bundles a cheap wait the decide
needs with an expensive one it does not. `stair_drain_anchor` syncs the runner
alone, appends the anchor's NAL and fills its actual. It leaves the burst flying
for the launch to overlap. The full drain then owes only the B's and skips the
half already done. Coding order holds on both paths, because the anchor's NAL and
fill precede its own B's either way. The following decide is exact on the
volatile term. A cut, a fade or a pan shows up on the anchor, and the anchor is
what mode 2 gives back.

Wall clock, pure-C, t18, medians of 7. All arms ran round-robin in one batch with
a duplicate baseline:

| clip | base | early (mode 1) | **early2 (mode 2)** | lagnowide (ceiling) | control |
|---|--:|--:|--:|--:|--:|
| foreman_cif | 209.8 ms | 1.116x | **1.107x** | 1.116x | 1.002x |
| park_joy_720p | 1921.2 ms | 1.088x | **1.096x** | 1.179x | 1.011x |
| samsung_720p | 862.3 ms | 1.028x | **1.042x** | 1.070x | 0.992x |

Placement alone is most of the lag's ceiling on foreman and about half of it on
the two 720p clips.

Quality. Mode 1's decide runs before its predecessor's `rcp_fill`, so it prices
the burst off a ledger missing actuals. Over the twelve-clip corpus at five
ladder points each, eight clips come back byte-identical under mode 1 and nine
under mode 2. The movers are the same non-stationary set the RC lag lost on, for
the same reason.

| clip | mode 1 BD-NEG | **mode 2 BD-NEG** | VMAF band |
|---|--:|--:|---|
| samsung_720p | +3.31% | **-0.66%** | 78.1-92.1 |
| samsung_720p, 1.4x ladder | +5.01% | **-0.29%** | 85.3-93.5 |
| sintel_720p, 0.30x ladder | +27.52% | **+2.63%** | 89.6-96.0 |
| sintel_720p, 0.35x ladder | -- | **+2.46%** | 91.8-96.3 |
| sintel_720p, 0.45x ladder | +11.58% | +9.69% | 93.5-96.8 |
| touchdown_420 | -11.48% | +5.67% | 86.3-91.1 |
| nine others | -- | 0.00% | byte-identical |

Mode 1's samsung result settles against it. samsung is the worst cell on the
table and its one low-bitrate 720p clip. Mode 1 pays +3.31% BD there for 1.038x,
the least speed on the corpus for the most quality. Its speed goes to the clips
already doing fine and its cost falls on the clip carrying the deficit. That is
the wrong trade whatever its mean says. Mode 2 crosses samsung from a loss to a
small win on two independent ladders.

Rate accuracy under mode 2 is equal to or better than the shipped path on every
clip measured. Zero-lag anchors exist for that property:

| clip | shipped worst | mode 2 worst |
|---|--:|--:|
| sintel_720p | +2.2% | +2.1% |
| samsung_720p | +7.8% | +7.6% |
| touchdown_420 | +5.9% | **+4.6%** |
| akiyo_cif | +3.8% | +3.8% |
| park_joy_720p | +3.6% | +3.6% |

Mode 1 had put touchdown at +11.6% on one point; splitting the drain removed that
too.

### Mode 2's remaining cost over a long window

sintel's cost depends heavily on the measurement window. Holding the VMAF band
fixed while lengthening the window gives a clean series at the calibrated
operating point (target 700 kbit/s):

| window | ladder | band | BD-NEG (ref 3) |
|---|---|---|--:|
| 144f (6s) | 600-1060 | 87.6-94.0 | **+11.70%** |
| 288f (12s) | 600-1060 | 89.6-94.6 | **+4.94%** |
| 576f (24s) | 3200-5300 | 89.2-95.4 | **-0.03%** |

Each is a legitimately calibrated read. The 5-second one is the least
representative window in the set. **A short single-GOP window is not a quality
measurement.**

Measured over 48 seconds (1152 frames), the number does not depend on which
engaged regime it is taken in:

| config | ref 3 | ref 1 |
|---|--:|--:|
| 1152f, 1 GOP forced, t18, ladder 1800-3200 | +1.63% | +0.67% |
| 1152f, 1 GOP forced, t18, ladder 2000-3500 | +1.18% | -- |
| 1152f, 5 GOPs, t36, ladder 1900-3200 | **+1.32%** | **+0.14%** |

So sintel's cost is about +1.3% BD-NEG at `--ref 3` and under +0.7% at `--ref 1`.

`--ref 1` is not identical, because the mode-2 gate is `if (e->rcp_on)` with no
reference-count term. At `--ref 1` bits move on akiyo (one point), samsung (three
of five), sintel (all five) and touchdown (all five). Eight of twelve clips are
identical, where `--ref 3` had nine.

| clip | ref 3 | ref 1 |
|---|--:|--:|
| sintel_720p, 48s, 1 GOP forced, t18 | +1.63% | **+0.67%** |
| sintel_720p, 48s, 5 GOPs, t36 | +1.32% | **+0.14%** |
| samsung_720p, 1000-2100 | -0.56% | **+1.40%** |
| samsung_720p, 750-1920 | -1.91% | **-0.46%** |
| touchdown_420, 5000-12800 | +4.27% | **+4.34%** |
| akiyo_cif, 250-640 | -- | **-1.82%** |
| eight others | 0.00% | 0.00% |

samsung swings across roughly three points between ladders and reference counts
without settling on a sign. That is the 180-frame single-GOP instability in
milder form, so read it as no effect. akiyo's -1.82% comes from a single moved
rung out of five in a band topping 96.5. It is one 0.22 VMAF wobble amplified by
a near-saturated fit.

**The trade.** Mode 2 costs about +1.3% BD-NEG on cut-heavy content and nothing
measurable on eight of twelve clips. In exchange it buys 1.04-1.11x on most
encodes on a wide box and on none on a narrow one. It is not flipped, because
this is an owner decision. The speed claim must be stated as 1.04-1.11x when
`--threads` is at least 8 per GOP, and exactly 1.00x below that.

Three gaps stay open. Only one corpus clip both moves under the split and is long
enough to measure over multiple GOPs, so the +1.3% rests on sintel alone. ducks
and park_joy are byte-identical and cannot corroborate it. The 1.04-1.11x was not
re-timed in the t36 multi-GOP regime. And the determinism battery covers
t1/t8/t18, which sit at or below the engagement boundary for the clips in it.

## Engagement is bounded by threads per GOP

`stair_ready` is the gate. One clause in it decides everything:

```c
if (!e->pool || ntp_pool_nthreads(e->pool) < Y264_MT_POOL_MIN || !e->wf_warmed)
        return 0;
```

`Y264_MT_POOL_MIN` is 2 (`src/encoder/encoder.h`). The CLI hands each GOP worker
a `frame_threads` share of `--threads`, split across workers in proportion to GOP
length. A worker whose share falls below that floor never allocates a staircase,
so `e->st` stays NULL. `Y264_ABR_EARLY` lives inside `stair_run_burst`, which
that worker never reaches. The rule is roughly
`--threads >= Y264_MT_POOL_MIN * ceil(frames / keyint)`.

This was measured on sintel by reading the output hash instead of any counter.
The floor was 8 when this ran. The boundaries below, and the
576-versus-720 example under them, are where a floor of 8 puts them:

| frames | GOPs | t4 | t8 | t18 | t36 | t72 |
|---|--:|---|---|---|---|---|
| 144 | 1 | identical | **moves** | **moves** | | |
| 240 | 1 | | **moves** | **moves** | | |
| 300 | 2 | | identical | **moves** | | |
| 600 | 3 | | identical | **moves** | | |
| 660-1152 | 3-5 | | identical | identical | **moves** | **moves** |

GOP count is not the controlling quantity. 576 and 720 frames have the same three
GOPs and behave differently, because the ragged split gives the two 250-frame
GOPs 8 threads each out of 18 at 576 and only 7 at 720. Length enters only
through how many workers the budget has to cover.

Short table clips (120-180 frames, one GOP) engage at any thread count from 8
up. A table run therefore measures the engaged case and cannot show the inert
one. Long content can go either way: it is inert at 18 threads
and engaged at 36. On a wide machine most encodes engage. Engagement and speedup
come from the same staircase, so an encode that gets no speedup is byte-identical
and takes no quality risk at all.

**TRAP: `stat_early` is not an engagement signal.** It counts launches that had a
drain to defer, and that tracks encoder speed. The same configuration reports 54
launches under `YAH264_NO_ASM=1` and 2 with asm while producing identical
output. What IS structural is whether the `stair-stat` lines appear at all
(`stair_free` returns early on `!e->st`). The output hash agrees with that in
every cell of the grid above.

## Measurement traps on this arm

- **Check engagement before pricing any knob.** A knob measured where it cannot
  engage reads exactly like a knob that does not work. An unengaged arm also
  reports a byte-identical 0.00% BD that reads exactly like a quality result.
- **Use `--no-cache` on every bdcompare invocation.** It keys on the command
  string, and a rebuild does not invalidate it.
- **Read the VMAF band before believing any BD cell.** Five clips were first
  measured with their top point above VMAF 96, where the metric has no headroom.
  Re-running them on a lowered ladder moved the answers materially: akiyo
  +11.23% to +20.66%, stefan +5.58% to +5.09%, bus +1.87% to +2.74%. touchdown
  moved from -25.23% to +14.23%. That sign flip is the cubic misbehaving at the
  old span.
- **The band's rate SPAN is a real constraint.** sintel's 88-94 band covers
  600-1100 kbit/s, a 1.8x span. The standard BD ladder (0.625x-1.6x) is 2.56x
  wide, so no single target can put all five rungs in the discriminating band.
  Record the span next to the calibrated target.
- **touchdown_420 cannot be used in a BD round.** At its calibrated target of
  8000, three in-band ladders of the same encode pair read +4.27% (5000-12800,
  band 83.7-91.4), +61.01% (6500-11000, band 85.5-90.2) and +1.31% (8000-15500,
  band 85.9-92.9). That is a 60-point swing from ladder choice alone, with no
  saturation flag anywhere. There are two causes. The clip is 150 frames, which is
  5.0 seconds and one GOP at the default keyint, so each rate point is a single
  rate-control trajectory with nothing to average against. Its VMAF/rate curve
  is also nearly flat: about 4.5 points across a 2.56x rate span, with a plateau
  at 8000-11000 where 37% more rate buys 0.42 VMAF. Both arms go non-monotonic
  inside the ladder (the shipped arm scores 88.58 at 4045k and 85.50 at 4710k).
  This is the opposite of saturation. The band sits at 85-93, low enough that
  VMAF should discriminate, and the curve stops responding to rate.
- **`touchdown_1080p` is the 4:2:2 original and cannot enter a 4:2:0 VMAF sweep at
  all.** `touchdown_420` is the conversion.
- **x264's ABR runs 19.5% to 26% hot on sintel** at every rate from 450 to 4000
  while yah264 tracks within 0.4%. sintel can never satisfy an operating-point
  rule whose first clause is that both encoders track. Keep sintel off any table
  with a dsize column, because that column would be reporting x264's rate control.
- **`conformance.sh --fast` needs the corpus symlinked into the worktree.**
  Without it the script silently runs 210 cases instead of 252. 210 is not the
  gate.
- **Sanitizer sweeps must match the config under test.** `stair_san.sh` runs
  `--ref 1` with no rate flag, so every one of its runs is constant-quality and
  both lag envs are inert in all of them, so it does not cover a lag change.
  `abr_san.sh` exists for that reason and sweeps ABR at `--ref` 1/3 x lag 1/2 x
  nowide 0/1 x t8/t18 across 5 shapes. A floor above zero hides the next real report, so run the shipped mode
  beside the probe modes instead of quoting a floor from a different config. One
  such gap hid a real race. The SHIPPED path raced at ABR `--ref 3`, t18, roughly
  one run in four, on `intra_screen_pure.env`. That is a function-local static
  read from `analyze_b_mb` that was never registered in `y264_mb_warm_statics`.
  The race was a same-value init from two GOP workers' pool threads, so
  determinism was never at risk. It did keep the TSan floor above zero.

## Probe gates

`Y264_RCP_LAG_NOWIDE` defaults off and writes nothing when unset. Byte-identity
was therefore the gate for the default path when these ran. `Y264_ABR_EARLY`
has since shipped at 2, so its mode-2 rows below describe the default path.

Four of the scripts named here and in the trap above are not in this tree:
`stair_san.sh`, `abr_san.sh`, `split_san.sh` and `stair_flush.sh`. They stayed
behind in the pre-rename repo. These rows record runs that cannot be repeated
here as written.

- `w2_canary` against a fresh build 26/26, and 26/26 again under
  `YAH264_NO_ASM=1`.
- Default byte-identity over 2 clips x {ABR, CRF, CQP} x `--ref` 1/3 x
  `--bframes` 2/3 x threads 1/8/18 x CABAC/CAVLC x asm/no-asm: **288/288**
  by comparing md5s across the arms.
- `meson test` 9/9. `conformance.sh --fast` 252/252.
- Run-to-run determinism at a FIXED thread count, 3 reps per cell: 42/42 identical
  over clips x `--ref` 1/3 x threads 1/8/18 x {shipped, lag1, lag2, lag1+wref,
  lag1+wref+qpd6, lag1+nowide, lag1+nowide+qpd6}; and 0 nondeterministic cells
  over 120 encodes for five clips x t1/t4/t8/t18 x {shipped, probe}. Mode 2 at
  `--ref 1`: 0 nondeterministic cells over 54 encodes. Mode 2 at t1 reproduces the
  shipped hash, and t8 and t18 agree with each other. Thread-count invariance is
  NOT claimed and is not the property in question. touchdown's own shipped output
  already differs between t8 and t18.
- TSan: 0 reports over 80 runs (modes 1 and 2 x `--ref` 1/3
  x t8/t18 across 5 shapes); `stair_san.sh tsan 3` 0 over 72; `abr_san.sh tsan 2`
  0 over 160.
- ASan+UBSan: `split_san.sh asan 2` 0 reports over 80 runs.
- `stair_determ.sh` 96/96, `stair_flush.sh` 160/160, `stress_threads.py` 0 hangs
  in up to 360 concurrent 8-thread encodes.
- `rcp_lag_nowide_on` and `abr_early_env` are registered in `warm_lr_statics`.
  `e->abr_early` resolves once at `encoder_open`, so no decide path calls
  `getenv`.

One item is not gated and is required before any flip. The mode-2 BD round
covers `--ref 3` only. Mode 2 applies at `--ref 1` too, since it keys on
`rcp_on` and ignores the reference count.

## Reproduction

```
W=build/cli/yah264
A="--input-y4m tests/corpus/foreman_cif.y4m --frames 180 --preset medium \
   --cabac --transform-8x8 --bitrate 400 --threads 18 -o /dev/null"

# engagement first, always
env Y264_STAIR_STAT=1 $W $A                                     # wide launches 0, max cc 0
env Y264_STAIR_STAT=1 Y264_RCP_LAG=1 $W $A                      # still 0 -- the --ref gate
env Y264_STAIR_STAT=1 Y264_RCP_LAG=1 Y264_STAIR_WIDE_REF=1 $W $A  # 42, 2

# encode the same clip at each lag and compare md5s: the lag is inert on this
# shape, so all three match, and it goes live only once the budget is granted
# on async capability (Y264_RCP_LAG_NOWIDE=1)
MODE=2 python3 bench/lowrate/split_bd.py samsung_720p touchdown_420
MODE=2 python3 bench/lowrate/split_bd2.py sintel_720p=0.30 sintel_720p=0.35
MODE=2 python3 bench/lowrate/split_rate.py sintel_720p samsung_720p touchdown_420
MODE=2 REF=1 REPS=3 THREADS=1,8,18 python3 bench/lowrate/split_determ.py \
    sintel_720p samsung_720p touchdown_420

# the ladder an operating-point rule is applied to
scripts/parity-clip-calib.sh sintel_720p 600,800,1000,1200,1600,2000
SECONDS_PER=5 scripts/parity-clip-calib.sh touchdown_420 4000,6000,8000,11000,14000,18000

# BD on written-out ladders, not a scale factor on a provisional target
EXTRA="--keyint 1200" bench/lowrate/split_bd_calib.sh sintel_720p 3 1152 1800,2100,2400,2800,3200
THREADS=36 bench/lowrate/split_bd_calib.sh sintel_720p 3 1152 1900,2200,2500,2850,3200
THREADS=36 bench/lowrate/split_bd_calib.sh sintel_720p 1 1152 1900,2200,2500,2850,3200
```

## 2026-09-01: sizing the ABR serialization target (it is WIDTH, not the drain)

The 2026-09-01 board sized ABR's cost against CRF at matched bits and found the
cost is not in the bits: +0.04 of the ratio at one thread, **+0.33 and +0.41 at
auto threads**. Cost that appears only when there are threads to hold up is
serialization. This is where it lives.

**The gap, reproduced and rate-matched to 0.00%.** park_joy_720p, 300 frames
(the board's 6-second window at 50 fps), auto threads, run through the CLI
instead of the in-process board. These are attribution numbers, not goal
numbers. Medians of 5, with arms interleaved so all three see the same load.

| arm | size | wall |
|---|--:|--:|
| ABR 12000 kbps | 9163640 B | **1.315 s** |
| CRF 27 | 9164114 B | **1.153 s** |

**ABR is 14% slower on identical bits.** `Y264_THREAD_PROF` + `Y264_NTP_PROF`
say **59% of the gap is measured pool-idle** (167.7 ms against CRF's 69.0 ms).
The machine is standing still, which is what serialization has to mean if it
means anything.

### The cause (the first answer here was wrong)

`emit_sync_wait` reads 88.9 ms under ABR against CRF's 1.0 ms. That points
straight at `w2_drain` and at the `type != 2` term in `emit_frame_w2`'s
`rc_waits`, since every non-B frame drains (one in four at `--bframes 3`).
**That attribution was wrong and is retracted.** Putting that term behind a knob
and turning it off moves **7 ms of the 167** and changes **no bits at all** on
this clip. The ledger an anchor reads was already the same. The knob was removed
afterwards.

The corroboration that seemed to confirm it was conflated. `Y264_RC_PIPE=0`
grows `emit_sync_wait` nine-fold. Besides adding drains, it also disengages the
stair and fpipe chains entirely, because `stair_clamp_on` needs `rcp_on` under
ABR. One knob made two changes, and the nine-fold scaling was mostly the second
one.

### What it actually is: width never launches under ABR

`Y264_STAIR_STAT` answers it in one run, as the section above already says to
do:

| | ABR | CRF |
|---|--:|--:|
| wide launches | **0** | **62** |
| max concurrent bursts | **0** | **3** |
| slot-recycle waits | **56** | **0** |
| launches with a drain to defer | 57 | 0 |

ABR runs **one burst at a time**. CRF runs **three**. Each burst waits for its
predecessor's slot to recycle, and those 56 waits are the serialization. They
also explain the rest of the bucket table. `analyze(WAVEFRONT)` reads +266 ms
while `stair_chain_join` reads -78 ms. That is work moving off the chain path
onto the driver path, exactly what losing concurrency looks like. The flushes
that inflate `emit_sync_wait` are the serialized bursts' ordering flushes, not RC.

"Width and ABR: the two gates" above already named the predicate.
`stair_wide_rc_ok` wants `rcp_lag > 0`, and `Y264_RCP_LAG` defaulted 0 then, so
ABR refused width at every `--ref`. This round walked into that section's own TRAP
line from the other side: *a knob measured on a config that cannot engage it
reads exactly like a knob that does not work.* Read that section before pricing
anything here.

### The existing arm is worth 46% of the gap

`Y264_RCP_LAG=1` already existed, shipped but off and already fixed
(`rcp-lag-shipped`). Same clip, same target, medians of 5 interleaved:

| arm | wall | size | ABR/CRF |
|---|--:|--:|--:|
| ABR, `Y264_RCP_LAG=0` (default then) | 1.315 s | 9163640 B | 1.141 |
| ABR, `Y264_RCP_LAG=1` | **1.241 s** | 9137443 B (-0.29%) | **1.076** |
| CRF 27 | 1.153 s | 9164114 B | 1.000 |

Engagement flips with it: 0 wide launches to 59, 0 concurrent bursts to 3, 56
slot-recycle waits to 0. **46% of the ABR-vs-CRF wall gap, at matched bits, from
a knob that is already in the tree.**

What it still needs before it could be a default is the accuracy half. The wall
side needs nothing more.
The lag changes when an anchor's ledger is current, so it wants the ABR rate
error across the corpus and a band read with `bench/lowrate/abr_noise.py`
alongside (`abr-band-noise-floor`: an ABR row smaller than that clip's floor is
not evidence). It belongs to the same predicate family as `Y264_RC_PIPE_VBV`,
which took the same decision and defaulted on before it was removed. The lag
took it too and has been the default above one thread since commit 94bc84c.
