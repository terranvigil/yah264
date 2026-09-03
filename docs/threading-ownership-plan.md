# Threading ownership: the library resolves auto, the CLI splits budgets

**Requirement (owner):** calling ffmpeg with libx264 and libyah264 should encode
with the same number of threads by default, and meet the goal-3 speed
comparison. Fix it properly rather than patch it.

**One caveat first, because the requirement conflates two numbers.** 1.06x was
the goal-3 median at the time this plan ran: the six table clips of 08-26, now
`CLIPS_LEGACY` in `scripts/parity-clips.sh` since the 08-31 rebalance to ten,
CRF solved to a matched achieved bitrate per clip, both encoders at
`-threads 12` inside one ffmpeg process
(`scripts/ffboard.py`, RC=crf). It is not a universal constant. BBB 1080p at crf
25 reads ~1.55x on the fast loop and yah264 spends 10.1% fewer bytes there; that
is a different operating point with one side doing less work per bit, not a
regression. The form of the requirement this plan gates on: **`ffboard.py` with
no `-threads` must read a median no worse than what `-threads 12` reads on the
same build.** "On the same build" matters, because Stage 0 shows the fix moves
the `-threads 12` cell too. The reference is re-measured, never quoted from
history.

**Provenance.** Nothing here reads or transliterates x264's threading code. The
one behavioural fact used is public and observable from its own documentation:
`threads 0` means the library decides, so auto belongs in the library rather than
in every caller. Its specific formula is irrelevant to us regardless, since it
drives a frame pipeline and we drive a row wavefront, and our constant comes from
our own measurements.

## The defect, precisely

Three distinct problems are tangled under "threading":

1. **The header states a contract the library does not keep.** `yah264.h` says
   `threads: 0 = auto, 1 = serial; GOP-parallel above 1`. `param.threads` is read
   exactly once in the library (encoder.c ~2414), as the serial-mode predicate
   for stq. Auto resolves to nothing, so a library consumer gets serial unless it
   sets `frame_threads` itself.
2. **Every consumer reinvents the policy.** GOP parallelism, the g x k budget
   split, greedy redistribution, the longest-first queue and wavefront capping
   all live in `cli/yah264_cli.c` (~940-1420). The ffmpeg wrapper could use none
   of it and grew its own plan; the current stopgap resolves auto in the wrapper
   via `av_cpu_count()`, which is policy in the wrong layer.
3. **A live quality/speed inconsistency nobody has priced.** The wrapper sets
   `p->threads = 1` unconditionally, and stq keys on `param.threads == 1`. So
   every ffmpeg encode, including the ffboard runs that produced the 1.06 median,
   ran with the flip-first speed trades DISENGAGED, in the serial quality mode,
   while the CLI table at t12 runs them engaged. Fixing the `threads` semantics
   flips that cell: yah264 gets faster at t12 in ffmpeg, its rate curve shifts,
   and the CRF solve re-lands. Favourable for the gate, but it means the
   reference number must be re-measured, and it partly explains the fast loop's
   byte advantage.

## 1. Where the policy lives

**The library owns "what one instance does with a thread budget", including
resolving auto. The caller owns "how a budget is split across instances." GOP
parallelism stays a caller construct.** Do not move the CLI's GOP machinery into
the library.

- **The data points away from GOP splitting on big frames.** On 1080p the
  wrapper's crude single-instance 18-wide wavefront beats the CLI's tuned GOP
  plan on wall (1.03s vs 1.25s) and spends 4.6% fewer bits, because the
  arithmetic split forces IDR placement a continuous encoder would not choose.
  `yah264_frame_thread_cap(1920,1080)` is 32, well above 18: one 1080p wavefront
  can eat the whole machine, so splitting buys nothing there and costs bits.
  Moving that policy into the library would enshrine the losing plan.
- **GOP parallelism inside one handle breaks the library's contracts.** It needs
  whole-GOP input buffering (streaming input is an owner requirement),
  per-instance rate-control splitting, output reordering across instances, and it
  changes the bitstream. The one-handle API does not survive N hidden instances
  without either large latency or large memory.
- **The upstream wrapper shape demands it.** A libavcodec wrapper that passes
  `avctx->thread_count` through and declares `AV_CODEC_CAP_OTHER_THREADS` is the
  normal shape. One computing core counts and clamps is not.
- Where splitting still pays, narrow frames (CIF's wavefront cap is 12, so 18
  threads cannot be spent on one CIF frame) and long multi-shot content, the CLI
  keeps it, documented as a CLI feature.

`threads` is redefined in documentation, not layout: **the thread budget for this
one encoder instance.** 0 = auto, 1 = serial, N = up to N, clamped at
`yah264_frame_thread_cap`. `frame_threads` stays the explicit low-level width;
when both are set, `frame_threads` wins, which the CLI's workers depend on.
`encoder_open` writes the resolved value back into its private copy so
`threads == 1` predicates key on truth: auto on a big machine disengages stq,
matching the CLI at t12, and an explicit `-threads 1` engages it, matching the
CLI at t1.

## 2. What auto resolves to

A new internal `y264_machine_threads()`, cached, resolved once.

- **Darwin:** `sysctlbyname("hw.nperflevels")` and `hw.perflevelN.logicalcpu`.
  This box reads 6 + 12 = 18. Note that the informal "12 P + 6 E" description and
  sysctl's level naming disagree about which tier is which, which is reason to
  enumerate levels rather than hardcode an interpretation.
- **Portable fallback:** `sysconf(_SC_NPROCESSORS_ONLN)`.

Two candidate policies, and the data in hand does not decide between them:

- **P1: all online cores** (18 here), clamped by the cap. On pure-y4m 1080p,
  18-wide beat 12 (1.03s vs 1.14s), so P1 may be right for the table, which feeds
  y4m.
- **P2: total minus the lowest perf level** (12 here). On the mp4 path, where the
  decoder competes in-process, 12 beat 18 (1.20s vs 1.56s), and x264's auto reads
  12.03 cores against our 13.91. The owner's real invocation is mp4-shaped.

Decision rule, fixed now so the measurement cannot be argued with later: choose
the policy that passes the ffboard gate; among those that pass, choose the one
faster on the mp4 fast loop; on a tie, fewer threads. Escape hatch
`Y264_AUTO_THREADS=<n>` for measurement. The policy is a formula over the
topology, never the literal 12.

One observation that de-dramatises this: on the table only the 720p rows can
move. CIF's cap is 12, so auto and `-threads 12` resolve identically there. The
whole auto question on the gate is three cells wide.

## 3. Public header changes

- **No struct layout change, no field renumbering.** Only the `threads` comment is
  rewritten to the contract above.
- **One added export**, a `YAH264_API` symbol:
  `int yah264_threads_auto(void);` returning what auto resolves to. It was the
  14th; the header declares 18 today, so count them rather than trusting that
  ordinal. Public so the
  CLI's split and the wrapper's log line consume the same number the library
  uses, and so the two policies cannot diverge again.
- **ABI:** adding a symbol is backward compatible, so **soversion stays 1** and
  `YAH264_ABI_VERSION` stays 1. What changes is behaviour under existing
  callers: a default-initialised param that previously encoded serial now engages
  the wavefront. That is thread-variant output, which the owner accepts, and the
  wavefront is identical at any width >= 2. It is a release note and a 0.2.0
  version bump, not an ABI event. Stage 1 audits call sites that need serial,
  notably the CLI's dispatch-priming open and its pool-less probe encoder, which
  must not each build an 18-thread pool as a side effect.

## 4. What the wrapper reduces to

```c
p->threads = avctx->thread_count;   /* 0 = auto; the library resolves it */
```

Delete the `av_cpu_count()` resolution, the cap clamp, and the `p->threads = 1`
line, which also fixes the stq inconsistency above. Keep
`AV_CODEC_CAP_OTHER_THREADS`. The init log prints the resolved width so a slow
encode is diagnosable. That is the wrapper's entire threading surface, and it
contains no encoder policy to defend in review.

## 5. How the CLI stops diverging

- `nthreads` auto resolution moves from `sysconf` to `yah264_threads_auto()`.
  One source of truth.
- **Add the missing plan: one instance, wide wavefront.** The CLI has no such mode
  today; every non-recon encode takes the GOP path even when the split buys
  nothing. Planner rule: when `yah264_frame_thread_cap(W,H) >= nthreads`, route
  to the streaming serial loop with `param.threads = nthreads` and leave
  `frame_threads` at 0 for the library. CLI default output on 1080p then matches
  the wrapper's bits and keyframes, with no forced-IDR premium, and the CLI
  carries less policy rather than more.
- The GOP path survives untouched for the cases it wins. Whether it still wins
  even at CIF is a Stage 0 measurement, not an assumption;
  `Y264_GOP_FORCE_G` / `Y264_GOP_FORCE_K` already exist for that sweep.
- The boundary is then structural: **library = budget to one instance's width;
  CLI = budget to instances**, with the CLI consuming only exported primitives.

## 6. Stages, each with its gate

The fast loop (`bbb15.sh`, 450 frames of BBB as y4m, no `-threads`) is the inner
iteration for **thread behaviour only**: cores occupied, wall direction, no
hangs. yah264's 13.91 cores should move toward x264's 12.03 under a correct
auto, which is readable in seconds. It is not a matched operating point, yah264
spends 10.1% fewer bytes at equal CRF, so it **must not gate speed**. `ffboard.py`
at matched bitrates is the outer gate. Check `uptime` and `pgrep` for leaked
spinners before any timed session.

**S0. Settle the open questions before writing code.** All existing harnesses.

- *S0a*: price the stq inconsistency. Fast loop, wrapper as-is vs `Y264_STQ=0`.
  Predicts how far the post-fix `-threads 12` reference moves.
- *S0b*: auto-candidate sweep. Fast loop and its mp4 variant, arms {12, 18},
  interleaved, medians of 5, reading cores and wall. Locates the oversubscription
  knee on both input shapes.
- *S0c*: the outer truth. `RC=crf THREADS=0 ffboard.py` against `THREADS=12` on
  the stopgap build, per clip. Only the 720p rows and x264's own auto response
  are unknown. This one run decides the auto policy and says whether the owner's
  expectation is already met, at risk, or moved by x264's auto. If x264's auto
  beats its own t12, say so plainly: the default-vs-default median is a new
  number, the bar does not move, and the remedy is making our auto faster.
- *S0d*: is the CLI's GOP policy earning its keep at CIF? A `Y264_GOP_FORCE_G`
  sweep on the three CIF clips at t12. If g=1 ties or wins, the planner rule
  extends and the GOP path shrinks toward cut-split content only.
- Gate: a per-clip table and a written decision on the auto formula and the
  planner rule. No code before it.

**S1. Library: implement `threads`.** Resolution in `encoder_open`, the
`yah264_threads_auto` export, resolved value visible in `Y264_LA_STAT`. Audit
every in-tree `yah264_encoder_open` call site for the default flip.
Gate: `make test`; conformance byte-identical on the serial recon path; auto md5
equals explicit-width md5 at the resolved count; determinism under six spinners
at the new default width; thread stress; TSan clean; recon sweep with the
wavefront default armed; `Y264_AUTO_THREADS=1` reproduces the old default md5
exactly.

**S2. Wrapper reduction.** The diff above, replacing the stopgap commit.
Gate: fast loop cores moving toward 12.03 with wall improving or holding;
`-threads N` matching library `frame_threads=N`; auto versus explicit-same-count
md5 identical; the integration plan's recon gate re-run.

**S3. CLI planner refactor.** Auto via `yah264_threads_auto`, the
single-instance route, the planner rule from S0d.
Gate: `--threads 1` md5 unchanged; `parity-status-crf` unchanged within noise on
clips whose plan did not change; BBB 1080p default matching the wrapper's bytes
and keyframes and its wall; RSS bounded on a long clip; the memory-limit refusal
re-checked, since window sizing keys on nthreads.

**S4. Final gate.** `RC=crf ffboard.py` with `THREADS=0` against `THREADS=12` on
the same post-fix build: auto median no worse than t12 within noise. Quote per
clip, quote x264's auto shift explicitly, run on two days given the known
cross-day spread, spinner-checked. Plus one mp4 end-to-end run, the owner's
actual invocation shape. Then re-run S0a's pricing backwards to confirm the
post-fix t12 cell moved the direction S0a predicted, so the table's history stays
interpretable.

## 7. Risks and their retirement

| risk | retirement |
|---|---|
| x264's auto beats its own t12, so default-vs-default reads worse than 1.06 through no fault of our threading | S0c reads it directly. If real: report both numbers, keep the bar, and the work becomes our auto beating our t12 |
| The stq flip moves the matched-rate solve enough to change the median | S0a prices it before any code; S4 confirms direction. The CRF re-solve absorbs the rate shift by construction |
| Auto right for y4m, wrong for mp4, or the reverse | The decision rule in section 2; S0b and S0c measure both shapes; the tie-break biases toward the shape the table cannot see |
| Default flip from serial to wavefront exposes a latent race at width 18 | Determinism under load, thread stress, TSan. The distinct-md5 count is the reading |
| Hidden pools from priming and probe opens after the flip | S1 call-site audit; RSS check in S3 |
| The CLI's GOP policy is simply wrong and this plan preserved it out of respect | S0d answers it with existing knobs. The planner rule is written to shrink the GOP path to exactly the cases that measure a win |
| Table noise swallows the three 720p cells that carry the question | The repeated-sample timer holds tables to +/-0.01 per clip; run S4 twice cross-day; the CIF rows are structurally identical and double as a null control |

## What this plan deliberately does not do

Move GOP parallelism into the library, hardcode 12 anywhere, read x264's
implementation, change `yah264_param_t` layout, bump the soversion, or ship any
stage without its measurement. The stopgap wrapper commit is replaced, not built
upon.

---

# Stage 0 results, 2026-08-26

Run on the stopgap build (wrapper resolves auto via `av_cpu_count()` = 18 and
pins `threads = 1`). Box carried only UI processes, no competing encoders.

## S0a. What stq has been costing the ffmpeg path

15s BBB y4m, no `-threads`, best of 3:

| | wall | bytes |
|---|--:|--:|
| stq engaged (as shipped) | 1.73s | 4,074,145 |
| stq off (`Y264_STQ=0`) | 1.63s | 4,089,830 |

**5.8% wall for 0.4% fewer bits.** Every ffmpeg encode this project has measured
paid it, including the ffboard runs behind the published median, because the
wrapper pins `threads = 1` and stq keys on that. Correcting the semantics
disengages stq at auto and hands that 5.8% back on our side of the table.

## S0b. Where the oversubscription knee is

450 frames, best of 5, `-threads` forced:

| input | 12 threads | 18 threads |
|---|--:|--:|
| y4m, no decode | 1.98s, 11.37 cores | **1.81s, 13.30 cores** |
| mp4, decode in-process | 2.09s, 11.82 cores | **1.96s, 13.59 cores** |
| x264 auto, for reference | | y4m 1.21s / 12.05 cores, mp4 1.24s / 12.79 |

18 wins on both shapes. This **overturns an earlier reading** that 12 beat 18 on
mp4: that measurement streamed its source from an external drive, so I/O was the
constraint rather than the thread count. The drive later unmounted, the source
was rebuilt on local disk, and the result flipped. Treat any speed number taken
against that drive as suspect.

## S0c. The outer truth, and the finding that reframes the gate

Goal-3 table, CRF at matched achieved bitrate, `THREADS=12` against `THREADS=0`.
Measured 08-26 on the six clips that are now `CLIPS_LEGACY` in
`scripts/parity-clips.sh`. The table was rebalanced to ten clips on 08-31, four
of them HD, so both medians below are historical: reproduce them with
`CLIPS="$CLIPS_LEGACY"`, and take a current median off the ten-clip table. The
finding the section rests on, that the `-threads 12` convention handicaps x264
more than it handicaps us, is what survives here, not the numbers.

| clip | t12 | auto | yah264 wall | x264 wall |
|---|--:|--:|---|---|
| foreman_cif | 1.18x | 1.04x | 0.11 -> 0.11 | 0.09 -> 0.11 |
| bus_cif | 1.16x | 1.09x | 0.10 -> 0.10 | 0.09 -> 0.09 |
| stefan_cif | 1.04x | 1.00x | 0.06 -> 0.06 | 0.06 -> 0.06 |
| ducks_720p | 0.61x | 0.83x | 1.32 -> 1.07 | 2.17 -> **1.28** |
| park_joy_720p | 0.83x | 1.11x | 1.17 -> 0.98 | 1.41 -> **0.88** |
| samsung_720p | 1.05x | 1.26x | 0.40 -> 0.39 | 0.38 -> **0.31** |
| **median** | **1.04x** | **1.06x** | | |
| max | 1.18x | 1.26x | | |

Both encoders speed up at auto. **x264 speeds up far more**: 41% on ducks, 38% on
park_joy, against our 15-19%. So the table's `-threads 12` convention has been
handicapping x264 more than it handicaps us.

**This invalidates the gate as originally written.** "Auto median no worse than
t12 median" compares our auto against a *handicapped x264*, which is not a
meaningful bar. The two runs are different comparisons, not better and worse
versions of one. The honest number is default against default, which read
1.06x on the legacy six. The bar does not move (standing owner rule); what
changes is which measurement we quote against it.

## S0d. Does the CLI's GOP split earn its keep at CIF?

Three CIF clips, t12, `Y264_GOP_FORCE_G` swept:

| forced g | median |
|---|--:|
| default | 1.03x |
| 1 (single instance) | 1.04x |
| 2 | 1.04x |
| 4 | 1.04x |

Flat within noise. The split buys nothing at CIF, and on 1080p single-instance
wins on wall and spends 4.6% fewer bits. **The GOP path earns nothing anywhere on
the table.**

## Decisions

1. **Auto resolves to all online cores** (P1), clamped by
   `yah264_frame_thread_cap`. S0b measures it faster on both input shapes. The
   table median worsening from 1.04 to 1.06 is x264 improving, not us regressing:
   our own wall improved on every 720p clip.
2. **The gate is restated.** Default against default is the comparison, measured
   with both encoders at their own auto. The pre-fix reading is 1.06x median /
   1.26x max. The fix must improve it, and S0a says roughly 5.8% is available on
   our side from disengaging stq alone.
3. **The CLI planner rule extends to CIF.** Route to single instance whenever the
   wavefront cap can absorb the budget, which on the table is everywhere. The GOP
   path stays only for cases that measure a win, and none is currently known;
   Stage 3 should look for one in cut-split long content or drop the path.
4. **Do not quote the t12 number as the main one any more.** It flatters us.

---

# Stages 1-4 results, 2026-08-26

## What shipped

- `param.threads` resolved in `encoder_open` and written back, so predicates
  keyed on it see what the instance got. `frame_threads` still wins when set.
- `y264_machine_threads()` counts the machine through `hw.nperflevels`, summing
  every performance level, with `sysconf` as the portable fallback.
- The auto budget is capped at **16** (owner call), binding auto only. Explicit
  requests pass through, and everything is clamped by
  `yah264_frame_thread_cap`. `Y264_AUTO_THREADS` pins the budget,
  `Y264_AUTO_THREADS_MAX` moves the ceiling.
- `yah264_threads_auto()` exported, soversion unchanged at 1.
- The ffmpeg wrapper reduced to `p->threads = avctx->thread_count`.
- The CLI asks `yah264_threads_auto()` instead of `sysconf`.

## The gate

Goal-3 table, CRF at matched achieved bitrate, both encoders at their own
default (`THREADS=0`, no `-threads` on either side):

| | median | max | dVMAF | dSIZE |
|---|--:|--:|--:|--:|
| pre-fix | 1.06x | 1.26x | -0.17 | +0.1% |
| post-fix, ceiling 18 | 1.01x | 1.20x | -0.17 | +0.5% |
| **post-fix, ceiling 16** | **1.01x** | **1.16x** | **-0.17** | **+0.5%** |

At the table's old `-threads 12` convention the same build reads **1.00x median,
1.11x max**, which clears all four metrics. That number is not the one to quote:
S0c showed t12 handicaps x264 more than it handicaps us, so it flatters. Default
against default is the honest comparison and it sits 0.01 outside both speed
metrics, which is inside the table's own per-clip noise.

## Scaling, which was the second half of the requirement

The chain is `min(request or auto, frame_thread_cap(W,H))`:

| resolution | cap | 4 | 8 | 12 | 16 | 32 | 64 cores |
|---|--:|--:|--:|--:|--:|--:|--:|
| QCIF | 5 | 4 | 5 | 5 | 5 | 5 | 5 |
| CIF | 12 | 4 | 8 | 12 | 12 | 12 | 12 |
| 720p | 21 | 4 | 8 | 12 | 16 | 16 | 16 |
| 1080p | 32 | 4 | 8 | 12 | 16 | 16 | 16 |
| 4K | 63 | 4 | 8 | 12 | 16 | 16 | 16 |

Small machines use everything. Small pictures clamp below the ceiling on their
own. A 64-core box encoding 1080p uses 16; filling the rest of such a machine is
several encoder instances, which is the caller's construct by design.

The ceiling measured free: 1.01s against 18's 1.00s at 300 frames of 1080p,
while occupancy fell from 13.9 cores to 12.07, where libx264's own auto sits.

## Gates run

`make test` 9/9. Conformance green, 476/476 on that run: the denominator is a
function of the corpus and QP list present, so it moves between checkouts and is
not a fixed target. `determ_repeat.sh` 16/16 configs
reproducible over 12 runs each under six spinners at load 10.09, which is the
gate the default flip from serial to wavefront needed. Through ffmpeg,
`auto == -threads 18 == -threads 12` byte-identical while `-threads 1` differs
and is smaller, stq engaging as it should. CLI output byte-identical.

## Not done

S3's second half: the CLI still routes every non-recon encode through the GOP
path even where S0d showed it earns nothing. That is a CLI-only speed and bits
question, it does not touch the ffmpeg default, and it wants its own table.
