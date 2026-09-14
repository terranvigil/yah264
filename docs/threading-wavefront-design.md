# MB-row wavefront threading: the design

Architecture doc. The implementer starts from the "what this locks" section and
the W0-W3 stage list. W0, W1 and W2 are built and the wavefront is the default;
W3 is optional and gated on measured need.

Evidence base: a full dependency audit of the per-MB encode path (our macroblock.c,
deblock.c, cabac.c, cavlc.c, me.c and encoder.c) and a read of x264's threading
behaviour (concepts only, clean-room rules apply).

## The decision in short

Keep GOP-parallel across GOPs. Add a macroblock-row wavefront inside the encoder
library for analysis + reconstruction, with entropy coding split out as a serial
per-frame pass that trails the wavefront. The wavefront schedule is a pure
function of frame geometry (the 2-MB spatial lag), so output stays byte-identical
at any thread count. Do not adopt x264's frame-parallel model: its thread-count
dependence is structural (see below), not incidental, and our conformance gate
forbids it.

One consequence is accepted up front: splitting entropy from analysis cannot be
byte-identical with pre-split CABAC output, because the CABAC RD estimates
snapshot the live raster-serial context state. CAVLC output is preserved
bit-for-bit through the whole migration; CABAC output changes once (at stage W0,
single-threaded) and is gated on BD-neutrality, then never changes again as
threads are added.

## Thread-count-invariant `mb_qp_delta` pricing

The `mb_qp_delta` RD-trial pricing reads `f->prev_qp`, and its value depends on
which QP chain the pricing walks. The TRUE entropy chain (advance the chain per
MB in raster order) carries a serial dependency; `predict_prev_qp` (the QP-MAP
raster predecessor) does not. The two diverge whenever per-MB QP is NON-uniform,
which is to say whenever mb-tree, AQ, or CRF rate control make neighbouring QPs
differ, because the true chain only advances `prev_qp` when a predecessor
actually codes a delta. The qpd bit-cost then tips a few RD mode decisions
differently, and the effect is thread-count-dependent because which MBs run under
the prediction and which under the true chain changes with the split.

**The design therefore makes the position-independent `predict_prev_qp` pricing
the default for ALL thread counts, serial included**, so output is
thread-count-invariant by construction. `wf_predqp_env` defaults ON;
`Y264_WF_PREDQP=0` escapes to the true-chain serial pricing, which is
non-deterministic across threads and exists only as a byte-identity canary.

The trigger is NON-uniform QP, not CQP: `--qp` + `--rc-lookahead 0` + no-AQ is
byte-identical across threads either way; adding mb-tree/AQ/CRF is what breaks
the true-chain form. The top-right neighbour lag is correct (threadpool.c waits
`progress[r-1] >= c+2`), so this was never an availability race, and
`serial + Y264_WF_PREDQP=1 == wavefront` byte-for-byte proves the single
mechanism.

Verified: threads 1/2/8 byte-identical on the conformance syn_320x240 fixture
across baseline/cabac/bframes2; BD-neutral at +0.00% VMAF-NEG on the 6-clip CIF
corpus (`--no-cache`) against the true chain; full conformance green.

## Known limitation: VBV determinism is load-sensitive

The qp-delta pricing rule does NOT cover VBV rate control. The conformance
`--cabac --crf 16 --vbv-maxrate 600 --vbv-bufsize 600 --bframes 2` determinism
check fails INTERMITTENTLY (roughly 1 run in N) under heavy parallel conformance
load (`-P 18`); in isolation the same config is byte-identical across threads
1/2/8 in 6/6 runs. It is a genuine but load- and timing-sensitive VBV +
GOP-parallel race: VBV carries cumulative bit-budget state across GOP workers,
which the deterministic qp-map trick does not linearise.

Fix direction: make the VBV rate-controller state order-independent across the
GOP split, or serialise the VBV accounting. Low priority: VBV is not the BD gate
and `--threads 1` is deterministic.

## Why x264's model is off the table

x264's frame-parallel threading staggers N frames vertically and synchronizes on
reconstructed-row progress (a per-row wait on a completed-lines counter). Its
output depends on thread count through two structural mechanisms, both confirmed
in source:

- **The vertical MV clamp is a function of thread count.**
  `i_mv_range_thread ≈ (height / i_thread_frames) / 2`, applied per row. More
  threads means a tighter clamp, different MVs, different bitstream. With
  `--non-deterministic` it gets worse: the clamp tracks the racing reference
  thread's actual progress, so output is not even reproducible at a fixed count.
- **Rate control reads in-flight estimates.** VBV planning subtracts the planned
  sizes of the other `i_thread_frames - 1` frames, ABR sums peers' predicted
  bits, row-VBV tolerance divides by thread count, and the ABR complexity
  accumulator lags by exactly the pipeline depth. All of it is count-dependent by
  construction.

The lesson worth keeping is the inverse of the mechanisms: x264 hoists every
lookahead-ish decision (slicetype, scenecut, mb-tree, weightp analysis) into a
serial order-deterministic pre-stage, and only MV range and RC accounting leak
thread count. This design keeps the hoisting and closes the two leaks: MV range
stays unclamped within a frame (the wavefront never co-encodes a frame with its
reference), and RC state transitions stay strictly serial in coding order.

## 1. Topology

Two nested levels, both deterministic, so the split between them affects only
speed, never bytes:

- **Across GOPs: GOP-parallel.** Independent encoder instances on keyint
  boundaries. Embarrassingly parallel and already deterministic. Nothing about it
  changes.
- **Within a frame: MB-row wavefront, inside the library.** The encoder holds a
  worker pool (`param.threads`, created at `yah264_encoder_open`). Analysis +
  reconstruction of MB rows proceed with the 2-MB lag; a serial entropy task per
  frame trails the rows; deblock stays a post-frame pass at first (stage W1) and
  fuses into the wavefront later (W3).
- **Within a GOP, across frames: serial.** Frames in a GOP form a reference
  chain, and our ME has an effectively unrestricted vertical MV range
  (out-of-frame coords clamp to the padded reference), so a dependent frame needs
  its reference fully reconstructed, deblocked, and border-extended. A fixed-lag
  frame pipeline that relaxes this is optional stage W3, gated on measured need.
  It is not the core of the design.

The CLI's thread budget splits GOP-first: `g = min(n_gops, threads)` GOP workers,
each opening its encoder with `threads/g` (rounded up) wavefront threads.
GOP-parallel is the more efficient level (no ramp triangles, no serial entropy
tail), so it gets priority; the wavefront exists to fill cores when GOPs run out.
Single-GOP content becomes 1 worker x 8 wavefront threads instead of an
effectively single-threaded encode. The split heuristic is tunable at will
because neither level can affect output.

What fills 8 cores on single-GOP content: the shipped cap is
`frame_thread_cap_k` in `src/encoder/encoder.c`, which sizes the pool from how
fast a diagonal retires rather than from the grid alone. One frame's diagonal
retires in `2(R-1)+C` cell-times, and k frames overlapping at `lag` rows apart
retire in `2(R-1)+C+(k-1)*2*lag`, so the cap is `k*R*C` over that. Narrow frames
run k=2 (the staircase overlaps an anchor and one row-gated leaf), wider ones
k=1, and the floor is `Y264_MT_POOL_MIN`. That reads 12 workers at CIF, 21 at
720p and 32 at 1080p. 8 threads saturate on everything from CIF up.

## 2. Determinism mechanism

Four rules, enforced by construction:

- **R1.** Every value a wavefront task reads is a pure function of (input,
  params, MB position). No task may observe thread count, scheduling, or timing.
- **R2.** The dependency schedule is fixed by geometry: MB (x, r) may run once
  row r-1 has completed MB x+2 and MB (x-1, r) has completed. Threads race to
  *execute* tasks, never to decide what a task reads.
- **R3.** State with a raster total order lives in exactly one serial pass per
  frame (entropy). It consumes committed per-MB decisions; it never feeds
  analysis.
- **R4.** Cross-frame waits target fixed milestones (frame N analysis complete,
  frame N entropy complete), never "how far a thread happens to be". The
  substrate API enforces this by having no progress getters (see section 5).

### The dependency table

Audited exhaustively against the code. "Lag OK" means the 2-MB lag guarantees the
read is committed (left, top, top-left, top-right at MB granularity are all final
when (x, r) starts).

| Dependency | State read | Neighbours | Lag OK? |
|---|---|---|---|
| I16x16 / chroma intra pred | `f->rec` samples | top, left | yes |
| I4x4 / I8x8 intra pred incl. diagonal modes | `f->rec`; `topright_avail` | top, left, TL, TR | yes; TR never exceeds MB x+1 of row r-1 |
| MVP median (16x16, partitions, sub-8x8) | `mvx/mvy/refidx` grids via `nb_at` | A=left, B=top, C=top-right, D=TL | yes; the 16x8 bottom-partition C is already disabled |
| P_Skip MV | grids, A+B | left, top | yes |
| B spatial/temporal direct | spatial A/B/C + co-located `col*` grids | spatial: as MVP; co-located: another, completed frame | yes; col grids are whole-frame snapshots taken before the B starts |
| Implicit bipred weights | POCs only | none | yes (frame constants) |
| Motion search + MC | reference planes only, never current `rec` | cross-frame | yes within a frame; full-reference dependency across frames (noted in W3) |
| Per-MB QP | `aq_off` / `mbtree_off` maps, precomputed before the frame; `mb_qp_pre` is a position lookup | none | yes |
| Deblock | `rec`, `mbqp`, `mb_tr8`, `nnz`, motion grids | left, top edges | post-frame pass today; never feeds same-frame intra (unfiltered pred is normative) |
| CABAC/CAVLC context inputs | `nnz`, `mbcbp`, `mvd*`, `refidx`, `i4mode` grids | left, top | yes, but see the authorship fix below |
| CABAC engine + adaptive `ctx[1024]` | every bin mutates it | raster total order | **no**: serial pass |
| CAVLC bitstream + `mb_skip_run` | raster accumulator | raster total order | **no**: serial pass |
| `mb_qp_delta` chain (`prev_qp`, `last_qp_delta`) | most recent raster MB with a residual | raster total order | **no**: serial pass |
| RD/RDOQ CABAC bit estimates (`est_*_bits`, `scan_bits_4x4`) | the **live** engine `ctx[]` at that raster point | raster total order | **no**: context source must change (below) |
| Rate control accumulators (ABR/CRF/VBV/2-pass) | updated once per frame after `build_slice` | frame-serial | yes; no within-frame bit feedback exists |
| Scratch / globals | all per-MB buffers are stack-local; only `s_analyse_subme` and idempotent lazy tables (getenv caches) are file-scope | n/a | yes; move `s_analyse_subme` into `y264_frame_t`, pre-init the lazy tables before threading |

The audit found no hidden per-MB serialization in reconstruction: rows encoded in
any R2-legal order produce identical recon. Everything raster-serial is
entropy-layer state.

### Three fixes the table demands

**Grid authorship moves to analysis.** The `write_*` functions author `f->nnz`,
`f->mbcbp`, and finalize `i4mode`/`mvd*`, so analysis of a neighbour depends on
the neighbour's *write* having run. The wavefront pass must author every grid
itself at MB commit; the entropy pass becomes a pure reader. The values already
exist at commit time; this is a store-site move, not new computation.

**The RD context source becomes deterministic.** CABAC RD and RDOQ estimate bits
against the live engine contexts, which encode the raster history of the whole
frame. Replacement: each row's analysis holds a private estimator context array,
initialized WPP-style from the snapshot MB (1, r-1) published at its commit
(available under the lag by construction), and advanced left-to-right by
est-coding each committed MB's decisions. Row 0 initializes from the slice-init
state. This is the closest deterministic approximation to the true state, costs
about one extra est-mode pass per MB (RD already runs several per MB), and it is
the *only* place the design changes CABAC output. Fallback if BD says the
sophistication is not needed: slice-init contexts everywhere, which is cheaper
and simpler. Decide by measurement at W0. RD's `mb_qp_delta` pricing uses the QP
map's raster predecessor as an approximation; the entropy pass codes the real
chain.

Build each row's private estimator context as the **~460-byte touched subset**
(bit counter plus the contexts a 4:2:0 MB can actually reach), *not* a full
`ctx[1024]` copy. x264's RD snapshot is exactly this narrow: a
single aligned memcpy of the bit counter plus 460 states, priced by a
`{transition[], entropy[]}` table walk (2 loads + 1 add per bin), no
arithmetic-coder path. Since W0 rebuilds these contexts as row-private anyway,
born them at the narrow width; it is materially cheaper to author small here than
to shrink a full-copy version afterward. 4:4:4 keeps the wider 1024+12 range (as
in x264).

**The QPY chain resolves serially, cheaply.** The decoder infers a no-residual
MB's QPY from the chain, and deblock reads that inferred value (`f->mbqp`). Chain
resolution needs only the committed decisions (cbp, cur_qp), not bits, so a
trivial raster scan (riding with the entropy pass, or standalone) fills `mbqp`
before deblock runs. Recon-match is preserved exactly.

**NOTE:** CAVLC RD prices bits from static tables, so the CAVLC path has no
live-context dependency. Every stage of this migration keeps CAVLC output
byte-identical, which makes CAVLC the canary: any CAVLC byte diff during
implementation is a bug, full stop.

## 3. The entropy stage

The wavefront produces one MB record per macroblock: mb_type, prediction modes,
ref indices, mvd (grids), cbp/mbcbp, transform-8x8 flag, cur_qp, and the
quantized levels. Worst case ~1 KB/MB for 4:2:0 8-bit; a full in-flight frame of
records is ~0.4 MB at CIF, ~8 MB at 1080p, ~33 MB at 4K. Negligible next to the
plane buffers.

A single entropy task per frame walks the records in raster order and owns all
the serial state: the CABAC engine and its `ctx[]`, cabac_alignment, the CAVLC
bitstream and `skip_run`, and the `prev_qp`/`last_qp_delta` chain. Rows publish on
completion, so the coder trails inside the frame rather than waiting for it; it
typically finishes shortly after the last row.

It does not serialize the frame because nothing downstream needs bits:

- Reconstruction never reads entropy output. The next frame's ME needs frame N's
  recon (wavefront + deblock product), not its bitstream. So from stage W2 on,
  frame N's entropy coder runs concurrently with frame N+1's wavefront, hiding
  CABAC's serialization behind frame-level overlap.
- Bit accounting for RC stays deterministic because it stays sequential:
  `rc_account`/`vbv_update`/2-pass records consume frame N's actual bits in coding
  order, and modes that need them before setting frame N+1's QP (ABR, VBV,
  2-pass) wait on the frame-N entropy milestone. That is a stall, not a
  determinism hole, and it is short because the coder trails by little. CQP and
  CRF do not wait (CRF's complexity input is available before entropy finishes).

We never read a peer's in-progress size estimate the way x264 does. If row-level
VBV is added later, its QP adjustments must be a function of *committed
estimated* bits from the analysis pass in a fixed row order, never of actual bits
mid-frame; the record already holds an estimate per MB, so the mechanism is
available and deterministic. Deferred until VBV work resumes.

## 4. Lookahead and mb-tree

No interaction with the intra-frame wavefront, by existing construction:
`la_push`/`la_finalize` run serially in input order on source-side lowres data,
and `compute_mbtree` fills its per-MB offset map before `emit_frame` starts the
frame. The wavefront consumes the finished maps read-only. This mirrors the x264
hoisting principle and it already holds in our code.

Two notes for later stages:

- The lookahead window legitimately spans GOP boundaries; `la_chain_prop` stops
  at IDRs, which is what keeps GOP workers independent. Unchanged.
- mb-tree's own cost (lowres ME per anchor) can be parallelized on the same pool
  if profiles demand: the per-MB cost maps are data-parallel, but `splat_prop`
  scatters, so a parallel version must partition by target row and merge in fixed
  order to stay deterministic. Optional, not scheduled.

Frame pipelining (W3) coexists with the lookahead trivially: typing decisions for
a frame complete at window pop, before the frame enters the wavefront, so the
pipeline adds no new ordering constraint on the window.

## 5. The substrate boundary

The scheduler is a codec-agnostic substrate, with a scope line in the spirit of
the shared GPU library's: **the substrate sees geometry, counters, and opaque tasks. It never
sees codec state.** No MB records, no contexts, no planes cross the boundary.
What varies per codec (block size 16 vs CTU vs superblock, the lag value, what a
"row task" does) is parameters and callbacks; the pool, the wavefront progress
table, and the milestone primitive are codec-free.

Prefix `ntp_` (next thread pool). The locked API:

- `ntp_pool`: `ntp_pool_create(nthreads)` / `ntp_pool_destroy`. One per encoder
  instance. Workers are anonymous; tasks never learn which worker runs them or how
  many exist.
- The wavefront: a per-frame grid of (rows, cols). It shipped as one blocking
  call, `ntp_wavefront(pool, nrows, ncols, thread_init, cell_fn, ctx)`, with the
  runtime rather than the caller enforcing the dependency: a cell runs only once
  its left, top and top-right neighbours have returned. The design's caller-side
  wait/publish pair was folded into that call, so a row task cannot get the
  order wrong. Row claiming order is by row index; claiming order cannot matter
  (R2), but fixed is tidy.
- `ntp_milestone`: named frame-level events (analysis-done, entropy-done,
  rows-deblocked >= k) with wait/signal, for the entropy task, deblock, and the W3
  pipeline.

One deliberate omission enforces R4 at the ABI level: **no progress getters.**
Nothing in the header hands a caller the progress a row has actually reached.
x264's
non-deterministic mode exists precisely because its cond_wait returns the raced
completion count and analysis consumes it; making that value unobservable makes
the whole class of bug unwritable.

Location: `src/common/threadpool.c` and `src/common/threadpool.h` (it includes
its own header and libc only, no encoder headers, GPL-2.0-or-later), promotable to its own
repo consumed by meson wrap if a second codec adopts it, that library's layout
exactly. The irreversible commitment is the ABI and the no-getters rule,
both locked here; the repo move is mechanics. A CTU-row consumer gets WPP-shaped
reuse for free (lag 1 or 2 per its entropy sync); a superblock consumer uses the
same pool for rows within tiles plus tile-level tasks.

## 6. Migration and gates

Every stage ships with conformance green (recon-match, byte-identical across runs
and across threads 1/2/8, the RC checks) and a bench/bench.py A/B. The
GOP-parallel fallback survives untouched throughout: `threads/g = 1` per instance
reproduces the pre-wavefront behaviour. The planned `Y264_WAVEFRONT=0` kill
switch was never built and no reader for it exists; a pool is created only when
the encoder's resolved wavefront width exceeds 1, so `--threads 1` is the way
back to the serial path.

| Stage | Work | Output vs previous stage | Gate |
|---|---|---|---|
| W0 | Split analysis/commit from bit emission (records + serial entropy pass, still one thread); grids authored at commit; RD context source switched to the row-private estimator; QPY chain resolver; `s_analyse_subme` into `y264_frame_t`; pre-init lazy tables | CAVLC byte-identical; CABAC changes once | conformance 227/227; CAVLC cmp-identical vs pre-W0 across the feature matrix; CABAC BD-neutral-or-better on the 7-clip corpus (VMAF-NEG, 5-pt, --no-cache); est-vs-real bits self-check; speed within noise |
| W1 | Pool in the library (`param.threads`); rows as wavefront tasks; entropy task trails within the frame; deblock stays the post-frame serial pass; CLI budget split g x (threads/g) | byte-identical to W0 at every thread count | threads 1/2/8 cmp-identical to W0-serial (the strongest form: threaded == serial bytes); TSan + ASan clean; scaling curve published |
| W2 | Frame-N entropy overlaps frame-N+1 wavefront inside a GOP; ABR/VBV/2-pass wait on the entropy milestone, CQP/CRF do not | byte-identical to W1 | same gates; single-GOP 8-thread speedup target ~6x |
| W3 (optional, measured need) | Fixed-lag frame pipeline within a GOP: deblock + border-extend fused as trailing row tasks with an intra-border backup of pre-deblock edge pixels; per-row reference publish; a fixed vertical MV clamp, a param constant applied identically at every thread count, unlike x264's; weightp estimation moved to source planes; CRF complexity from lookahead lowres. Also here if profiles justify it: flat-B siblings encoded frame-parallel (non-reference B's in a minigop are mutually independent) | output changes (clamp, weightp, CRF inputs); still thread-count invariant | each toggle BD-gated independently; determinism gates unchanged; ship only what pays |

W3 exists because CIF-class frames have only 18 rows and a per-frame ramp; 1080p
does not need it to saturate 8 threads. Decide from the W2 scaling curve on the
gate corpus, not in advance.

Expected wins. These are the design's projections and none of them carries a
measurement; the shipped scaling numbers live with the boards and the MT
records, not here.

- Single-GOP CIF, 8 threads: pre-wavefront ~1x (single-threaded in practice). W1
  ~4.5-5x (wavefront efficiency ~65-75% at 11-row concurrency plus the serial
  entropy tail). W2 ~6x (tail hidden).
- The benchmark clip (foreman 912 frames, 4 GOPs, 8 threads): 3.5x over 1-thread
  (98 vs 28 fps) pre-wavefront. Hybrid 4 GOP workers x 2 wavefront threads ~7x,
  around 190-200 fps, pulling the x264-medium gap at 8 threads from ~17x back
  toward the ~12.7x single-thread ratio.
- 1080p single GOP: near-linear to 8 threads at W1 already (60-row concurrency).

Conformance additions that ship with W1: extend `check_threading` to the feature
matrix (cabac, 8x8, bframes, aq, each RC mode) rather than one flag set; add a
threaded-vs-serial byte-identity check against the same binary; add a TSan job to
CI.

## What this locks and what it leaves open

Locked: the hybrid topology (GOP-parallel outer, in-library row wavefront inner,
frames serial within a GOP until W3); the four determinism rules R1-R4 and the
dependency table's serial/parallel split; entropy as a trailing serial per-frame
pass over MB records; grid authorship at analysis commit; the row-private
WPP-style RD context model (with slice-init as the measured fallback); the `ntp_`
substrate ABI including the no-progress-getters rule; CAVLC byte-identity as a
standing invariant through every stage; the W0-W3 gate ladder.

Open, by measurement: the CLI budget-split heuristic; whether W3 ships at all and
which of its toggles pay; the fixed MV clamp value; row-level VBV mechanics
(specified deterministic-by-estimate, not scheduled); mb-tree internal
parallelization; the exact MB record layout and pool internals (implementer's, so
long as R1-R4 and the gates hold).
