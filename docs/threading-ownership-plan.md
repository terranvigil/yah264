# Threading ownership: the library resolves auto, the CLI splits budgets

**Requirement (owner):** calling ffmpeg with libx264 and libnext264 should encode
with the same number of threads by default, and meet the goal-3 speed
comparison. Fix it properly rather than patch it.

**One caveat first, because the requirement conflates two numbers.** 1.06x is the
goal-3 median: six board clips, CRF solved to a matched achieved bitrate per
clip, both encoders at `-threads 12` inside one ffmpeg process
(`scripts/ffboard.py`, RC=crf). It is not a universal constant. BBB 1080p at crf
25 reads ~1.55x on the fast loop and next264 spends 10.1% fewer bytes there; that
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

1. **The header states a contract the library does not keep.** `next264.h` says
   `threads: 0 = auto, 1 = serial; GOP-parallel above 1`. `param.threads` is read
   exactly once in the library (encoder.c ~2414), as the serial-mode predicate
   for stq. Auto resolves to nothing, so a library consumer gets serial unless it
   sets `frame_threads` itself.
2. **Every consumer reinvents the policy.** GOP parallelism, the g x k budget
   split, greedy redistribution, the longest-first queue and wavefront capping
   all live in `cli/next264_cli.c` (~940-1420). The ffmpeg wrapper could use none
   of it and grew its own plan; the current stopgap resolves auto in the wrapper
   via `av_cpu_count()`, which is policy in the wrong layer.
3. **A live quality/speed inconsistency nobody has priced.** The wrapper sets
   `p->threads = 1` unconditionally, and stq keys on `param.threads == 1`. So
   every ffmpeg encode, including the ffboard runs that produced the 1.06 median,
   ran with the flip-first speed trades DISENGAGED, in the serial quality mode,
   while the CLI board at t12 runs them engaged. Fixing the `threads` semantics
   flips that cell: next264 gets faster at t12 in ffmpeg, its rate curve shifts,
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
  `next264_frame_thread_cap(1920,1080)` is 32, well above 18: one 1080p wavefront
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
`next264_frame_thread_cap`. `frame_threads` stays the explicit low-level width;
when both are set, `frame_threads` wins, which the CLI's workers depend on.
`encoder_open` writes the resolved value back into its private copy so
`threads == 1` predicates key on truth: auto on a big machine disengages stq,
matching the CLI at t12, and an explicit `-threads 1` engages it, matching the
CLI at t1.

## 2. What auto resolves to

A new internal `n264_machine_threads()`, cached, resolved once.

- **Darwin:** `sysctlbyname("hw.nperflevels")` and `hw.perflevelN.logicalcpu`.
  This box reads 6 + 12 = 18. Note that the informal "12 P + 6 E" description and
  sysctl's level naming disagree about which tier is which, which is reason to
  enumerate levels rather than hardcode an interpretation.
- **Portable fallback:** `sysconf(_SC_NPROCESSORS_ONLN)`.

Two candidate policies, and the data in hand does not decide between them:

- **P1: all online cores** (18 here), clamped by the cap. On pure-y4m 1080p,
  18-wide beat 12 (1.03s vs 1.14s), so P1 may be right for the board, which feeds
  y4m.
- **P2: total minus the lowest perf level** (12 here). On the mp4 path, where the
  decoder competes in-process, 12 beat 18 (1.20s vs 1.56s), and x264's auto reads
  12.03 cores against our 13.91. The owner's real invocation is mp4-shaped.

Decision rule, fixed now so the measurement cannot be argued with later: choose
the policy that passes the ffboard gate; among those that pass, choose the one
faster on the mp4 fast loop; on a tie, fewer threads. Escape hatch
`N264_AUTO_THREADS=<n>` for measurement. The policy is a formula over the
topology, never the literal 12.

One observation that de-dramatises this: on the board only the 720p rows can
move. CIF's cap is 12, so auto and `-threads 12` resolve identically there. The
whole auto question on the gate is three cells wide.

## 3. Public header changes

- **No struct layout change, no field renumbering.** Only the `threads` comment is
  rewritten to the contract above.
- **One added export**, the 14th `NEXT264_API` symbol:
  `int next264_threads_auto(void);` returning what auto resolves to. Public so the
  CLI's split and the wrapper's log line consume the same number the library
  uses, and so the two policies cannot diverge again.
- **ABI:** adding a symbol is backward compatible, so **soversion stays 1** and
  `NEXT264_ABI_VERSION` stays 1. What changes is behaviour under existing
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

- `nthreads` auto resolution moves from `sysconf` to `next264_threads_auto()`.
  One source of truth.
- **Add the missing plan: one instance, wide wavefront.** The CLI has no such mode
  today; every non-recon encode takes the GOP path even when the split buys
  nothing. Planner rule: when `next264_frame_thread_cap(W,H) >= nthreads`, route
  to the streaming serial loop with `param.threads = nthreads` and leave
  `frame_threads` at 0 for the library. CLI default output on 1080p then matches
  the wrapper's bits and keyframes, with no forced-IDR premium, and the CLI
  carries less policy rather than more.
- The GOP path survives untouched for the cases it wins. Whether it still wins
  even at CIF is a Stage 0 measurement, not an assumption;
  `N264_GOP_FORCE_G` / `N264_GOP_FORCE_K` already exist for that sweep.
- The boundary is then structural: **library = budget to one instance's width;
  CLI = budget to instances**, with the CLI consuming only exported primitives.

## 6. Stages, each with its gate

The fast loop (`bbb15.sh`, 450 frames of BBB as y4m, no `-threads`) is the inner
iteration for **thread behaviour only**: cores occupied, wall direction, no
hangs. next264's 13.91 cores should move toward x264's 12.03 under a correct
auto, which is readable in seconds. It is not a matched operating point, next264
spends 10.1% fewer bytes at equal CRF, so it **must not gate speed**. `ffboard.py`
at matched bitrates is the outer gate. Check `uptime` and `pgrep` for leaked
spinners before any timed session.

**S0. Settle the open questions before writing code.** All existing harnesses.

- *S0a*: price the stq inconsistency. Fast loop, wrapper as-is vs `N264_STQ=0`.
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
- *S0d*: is the CLI's GOP policy earning its keep at CIF? A `N264_GOP_FORCE_G`
  sweep on the three CIF clips at t12. If g=1 ties or wins, the planner rule
  extends and the GOP path shrinks toward cut-split content only.
- Gate: a per-clip table and a written decision on the auto formula and the
  planner rule. No code before it.

**S1. Library: implement `threads`.** Resolution in `encoder_open`, the
`next264_threads_auto` export, resolved value visible in `N264_LA_STAT`. Audit
every in-tree `next264_encoder_open` call site for the default flip.
Gate: `make test`; conformance byte-identical on the serial recon path; auto md5
equals explicit-width md5 at the resolved count; determinism under six spinners
at the new default width; thread stress; TSan clean; recon sweep with the
wavefront default armed; `N264_AUTO_THREADS=1` reproduces the old default md5
exactly.

**S2. Wrapper reduction.** The diff above, replacing the stopgap commit.
Gate: fast loop cores moving toward 12.03 with wall improving or holding;
`-threads N` matching library `frame_threads=N`; auto versus explicit-same-count
md5 identical; the integration plan's recon gate re-run.

**S3. CLI planner refactor.** Auto via `next264_threads_auto`, the
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
post-fix t12 cell moved the direction S0a predicted, so the board's history stays
interpretable.

## 7. Risks and their retirement

| risk | retirement |
|---|---|
| x264's auto beats its own t12, so default-vs-default reads worse than 1.06 through no fault of our threading | S0c reads it directly. If real: report both numbers, keep the bar, and the work becomes our auto beating our t12 |
| The stq flip moves the matched-rate solve enough to change the median | S0a prices it before any code; S4 confirms direction. The CRF re-solve absorbs the rate shift by construction |
| Auto right for y4m, wrong for mp4, or the reverse | The decision rule in section 2; S0b and S0c measure both shapes; the tie-break biases toward the shape the board cannot see |
| Default flip from serial to wavefront exposes a latent race at width 18 | Determinism under load, thread stress, TSan. The distinct-md5 count is the reading |
| Hidden pools from priming and probe opens after the flip | S1 call-site audit; RSS check in S3 |
| The CLI's GOP policy is simply wrong and this plan preserved it out of respect | S0d answers it with existing knobs. The planner rule is written to shrink the GOP path to exactly the cases that measure a win |
| Board noise swallows the three 720p cells that carry the question | The repeated-sample timer holds boards to +/-0.01 per clip; run S4 twice cross-day; the CIF rows are structurally identical and double as a null control |

## What this plan deliberately does not do

Move GOP parallelism into the library, hardcode 12 anywhere, read x264's
implementation, change `next264_param_t` layout, bump the soversion, or ship any
stage without its measurement. The stopgap wrapper commit is replaced, not built
upon.
