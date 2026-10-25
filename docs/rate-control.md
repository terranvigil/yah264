# Rate control

Rate control picks the base QP of every frame. Everything finer (per-macroblock
AQ offsets, mb-tree's propagation offsets, the I/P/B cascade) sits on top of
that base. yah264 has six modes: CRF, CQP, one-pass ABR, ABR under a VBV (CBR
and capped VBR), capped CRF and two-pass. Each one has to keep working when
frames are coded out of order on many threads, so most of this doc is about two
things: the model each mode uses to pick a QP, and when that model gets to see
the bits it actually spent.

The code lives in `src/encoder/rate.c` (the models, the VBV, the pipelined
decide and account) and `src/encoder/encoder.c` (mode setup in
`yah264_encoder_open`, the frame-type cascade in `frame_qp`, the lookahead).
Flags are in [options.md](options.md). Every `Y264_*` default quoted here is in
[knobs.md](knobs.md).

## How it works

### Picking a mode

| You are delivering to | Mode | Flags |
| --- | --- | --- |
| nothing constrains the rate: masters, archives, quality work | CRF | `--crf N` |
| streaming VOD, ABR ladder rungs | capped CRF | `--crf N --vbv-maxrate M --vbv-bufsize B` |
| a file-size target, one pass | ABR | `--bitrate N` |
| a file-size target, whole clip available | two-pass | `--pass 1`, then `--pass 2 --bitrate N` |
| average rate under a peak ceiling | capped VBR | `--bitrate N --vbv-maxrate M --vbv-bufsize B`, M > N |
| a fixed-bandwidth link | CBR | `--bitrate N --vbv-maxrate N --vbv-bufsize B` |
| measurement, debugging | CQP | `--qp N` |

`--qp`, `--bitrate` and `--crf` each name a mode, and the last one on the
command line wins. A warning on stderr names the flag that lost. `--pass` sits
outside that contest: it's a mode plus a stats round trip whose target is
`--bitrate`, and a `--crf` beside it is dropped with a message. A VBV needs both
`--vbv-maxrate` and `--vbv-bufsize`.

The base QP each mode picks then goes through `frame_qp`: I frames take
base - 3, B frames take base plus a depth cascade, and `--qpmin`/`--qpmax`
bound the coded QP. Two-pass sets the coded QP directly and skips the cascade.

### CQP

Every frame gets the QP you named, through the I/P/B cascade and nothing else.
CQP turns off both AQ (`aq_strength` is forced to 0 in the library, not just the
CLI) and mb-tree (`mbtree_skip`), so `--qp 26` really is constant QP. It's an
instrument, not a delivery mode: integer QP can't land closer than about
+-7% of a rate, and real clips miss by more.

### CRF

CRF is open loop: no coded bit ever feeds back into the QP. With mb-tree on (the
default whenever there are B frames or a lookahead), `rc_set_qp_crf` gives every
anchor the same base:

    qp = crf + level + 2.9 * log2(fps / 30) + frame_dc

`level` (`Y264_CRF_LEVEL`, 2.3) and the fixed fps slope of 2.9 exist because mb-tree's offsets are negative on average: they lend QP to the
blocks other frames reference, so a flat base at `crf` overspends. The two
constants put our bytes at `--crf N` on top of x264's at the same `--crf` on
22 clips from 24 to 60 fps (median miss 5.2%, worst cell 27.7%). Per-frame
importance comes from the mb-tree field. The frame-level content term is
`frame_dc`, the frame DC of the AQ field (see
[the AQ strength table](#frame-level-aq-strength-by-mode)). B frames inherit their
anchor's base and add the cascade.

The base is then rounded: `e->qp = (int)lround(qp)`. That's the integer
staircase. Fractional `--crf` is parsed and stored, but it only matters where it
crosses a rounding boundary, so reachable rates come in steps of one QP, roughly
12-13% apart. CRF can't be solved to an arbitrary bitrate. Search on a grid of
1.0, since a 0.1 grid oscillates between plateaus.

Without mb-tree (`--bframes 0 --rc-lookahead 0`), CRF falls back to a
complexity equation on the ME-compensated lowres cost, `CRF_BASE_CPLX` and a
capped downward adjustment.

### Capped CRF

Capped CRF is CRF with a VBV, and the VBV can only take bits away. Where the cap
is slack the encode is the CRF encode, bit for bit. Where it's tight,
`vbv_fill_budget` steers the buffer toward half full.
Above half full a frame may spend its arrival plus the whole surplus, so content
the cap doesn't need keeps its CRF quality. Below it a frame spends its arrival
less half the deficit, never less than half an arrival. The budget reads only the declared buffer and rate,
and the occupancy advances on actual bits, so a misprediction shows up as lower
fill on the next decide.

This budget is what makes the mode work. Without it nothing under CRF looks at
bits until the fit test fires at a nearly empty buffer, and a rate factor whose
natural rate sits above the cap drains the buffer unopposed and then runs pinned
at empty. `--crf-max` bounds how far the buffer may raise a frame's coded QP,
and the buffer is then allowed to under-run. That's the trade the flag asks for.

### One-pass ABR

With mb-tree on, ABR is the RF2 model: CRF's flat base plus a slowly adapted
rate factor. Two terms move the anchor base QP.

- **The ledger.** Each coded frame adds bits x qscale(its QP) to a work sum and
  one frame of target to an owed sum. The ratio is the qscale at which the
  frames so far would have spent exactly their target. It's self-normalizing
  and has no gain. It moves at most 0.25 x `--qpstep` per anchor decide.
- **The feedback.** Bits spent beyond bits owed, over a window of `--ratetol`
  x 2 seconds of target that widens with the square root of the coded
  duration, map to a QP offset bounded at +-6. Gains are 16 under and 12 over
  (10 and 4 under CBR, whose window is a quarter of the buffer).

The ledger opens on one frame of prior at a fitted opening QP:
`8.69 + 2.92 log2(cme/MB) - 1.32 log2(bits/MB)`,
clamped to 15..45, read off the median lowres inter cost of the frames already
in the lookahead. The cumulative ledger never forgets its first second, so the
opening matters. Mid-stream I frames take a decayed track of the P base QPs
(decay 0.9). B frames take the anchor's base plus the cascade, as under CRF.
Frames whose decide complexity per MB is below 4 (a black opening) stay out of
the ledger but still count in the feedback.

With no B frames and no lookahead (`--bframes 0 --rc-lookahead 0`), ABR uses a
per-frame model: QP from
`bits ~ scale x C^(1-qcomp) / qscale` with per-type scales, `Y264_ABR_QCOMP`
0.6. `--abr-model rf` swaps in a per-frame rate-factor variant there. Anywhere
else it has no effect, `--no-mbtree` included.

### CBR and capped VBR

Both are ABR plus a VBV, and the encoder calls it CBR when `--vbv-maxrate` is at
or below `--bitrate`. On top of the ABR decide:

- **The fit test** (`vbv_limit_at`): a frame must fit in 0.9 of the occupancy it
  is removed from.
- **The burst plan** (capped VBR only, `Y264_VBV_PLAN` 1): at each anchor the
  whole burst (anchor plus buffered B frames) is walked through the buffer law at
  the model's predictions. The base is the lowest QP at or above the rate
  factor's that keeps every frame inside the fit test and puts the buffer on a
  trajectory toward half full over 32 frames. Without it, a cheap opening banks
  a surplus, the hard section drains the buffer inside a second, and the encode
  runs pinned near empty with a +-4 QP sawtooth.
- **Serial re-coding** for bursts the model can't price (see
  [under frame threading](#rate-control-under-frame-threading)).

The buffer is charged each picture's slice payload plus its NAL framing. The
payload alone undercounts by about 60 bits a picture, which drifted to 30 kbit
of phantom occupancy over a few hundred CBR pictures.

`--nal-hrd vbr|cbr` writes the buffer model into the stream: `hrd_parameters`
in the SPS, a `buffering_period` SEI at every IDR, `pic_timing` on every
picture. `BitRate` and `CpbSize` are `--vbv-maxrate` and `--vbv-bufsize`
rounded up to the syntax grid, and the initial removal delay is the occupancy
the encoder started from. `vbr` moves no bit. `cbr` adds filler and nothing else.
`scripts/hrd_check.py` reads the declaration back and simulates Annex C.

### Two-pass

Pass 1 codes at a fixed base QP of 26 with full analysis (there's no reduced
first pass) and writes one record per frame: type, complexity, bits, QP,
is-reference. Pass 2 turns each record into a QP-invariant cost,
`(bits + 1) x qscale(qp)`, blurs it over neighboring frames (`--cplxblur` 20),
and solves an offline plan for the whole clip with I and B tied to P by
`--ipratio` 1.4 and `--pbratio` 1.3. A bounded runtime correction tracks the
actual spend. `--pass 3` reads the records and writes them back from a real
encode, so a further pass refines against it. Pass 2 needs the same `--frames`
and `--keyint` as pass 1. A missing stats file isn't an error: pass 2 then codes
every frame at the base QP.

### Frame-level AQ strength by mode

Per-MB AQ is `--aq-strength` (0.4 in every rate-controlled mode, 0 at CQP)
times a per-frame gain. Under CRF and ABR the AQ offset is anchored to an
absolute energy level instead of the frame mean, so the field has a frame DC.
`crf_frame_dc` carries that DC onto the anchor's base QP at its own strength,
`Y264_AQ_DC`, weighted per MB by a rate proxy at 0.25 so a mostly
flat frame doesn't take a base far below what its blocks cost. B frames with no
mb-tree field get their frame-level term from a pull toward a fixed level
instead, with strength 1.3 around a level of 6.0.

| Mode | Frame DC strength | Where it goes |
| --- | --- | --- |
| CRF, no VBV | 1.3 | anchor base QP |
| capped CRF | 1.04 | anchor base QP |
| one-pass ABR, capped VBR (mb-tree on) | 1.04 | coded base QP only; the ledger and opening stay DC-free |
| CBR | none | |
| two-pass | none; non-reference B frames get the pull directly, at 1.3 | |
| CQP | none, AQ off | |

Under a bitrate target the DC reads a much lower variance floor
of 1/256, so a black frame reads as black.

### Rate tracking

ABR's accuracy comes from the ledger plus feedback, and it's held to the shared
parity bar: every rung of a real-title VBR cell lands within +-10% of target
over the whole cell. CBR tracks to a median 0.52%, worst -2.5%, on the band at
six threads. Delivered rate barely moves with thread count, even where the
bitstream does.

Across GOP workers (the CLI codes GOPs in parallel, one encoder per GOP), ABR
keeps one rate problem:

- **Carry** (`Y264_RC_CARRY` 1): a GOP opens from its predecessor's exported
  state, with the GOPs in between credited at target.
- **Shared ledger**: concurrent ABR GOPs read and write one
  rate factor. Chained through the carry alone, the first W GOPs each solved
  their own rate problem cold and every later one read a ledger W GOPs stale:
  6 parallel GOPs of a 1080p title at 12.5 Mbit/s capped VBR read VMAF-NEG 87.4
  against 89.8 with one worker. Not used under two-pass (its budget is already
  per GOP) or `--gop-threads`.
- **Two-pass** splits the stats file at the GOP markers a threaded pass 1
  writes, and each GOP takes the share of the budget its complexity earns. A
  serial pass 1 writes no markers, so its pass 2 runs serially too.

### What VBV actually guarantees

**Per segment, from the assumed occupancy, the guarantee is strict.** The first
GOP starts full (or at `--vbv-init`). Every later GOP assumes only the handoff
level, half the buffer, which is the CRF fill goal's fixed point. That's what
chunked delivery needs: each segment decodes legally on its own.

**Capped ABR joins segments explicitly.** A GOP coded right after its
predecessor by the same worker (every GOP at `--threads 1`) continues that
buffer at its exact final occupancy, so a one-worker encode is one buffer end to
end. With more workers, a GOP followed by another's is held to an exit corridor
(`yah264_encoder_vbv_handoff`) that narrows over its last frames until the final
one leaves the buffer at the handoff level. Capped CRF needs no corridor because
its fill budget already returns to that level.

**Per stream, concatenated end to end, it's designed to hold and measured to
hold, not proved.** Each segment exiting at or above the handoff rests on the
rate loop returning to its fixed point, and the loop runs on a predictor whose
error tail is unbounded (actual over predicted: p50 about 0.85, p99.9 in the
hundreds). The serial re-coding below closes most of that gap. A cut the buffer
can't hold at QP 51 still gets through.

### Rate control under frame threading

Frames are coded in a staircase: an anchor and its B frames form a burst, and
bursts overlap (see [threading.md](threading.md)). A QP decided before the
previous frames' bits are known is the whole problem.

**CRF and CQP** need no bits, so they decide serially at arrival from signals
captured on the API thread (the lowres cost, the source for the frame DC) and
take full pipeline width.

**ABR, two-pass and anything with a VBV** ride the pipelined decide
(`rcp_decide`). State splits into a committed ledger, advanced only by actual
bits in coding order, and a FIFO of decided-but-unaccounted frames, each carrying
its predicted bits. A decide reads the ledger plus the pending predictions. A
fill stores a frame's actual bits, and a pop moves them into the ledger once a
later non-B frame has been decided. Decide and pop order is fixed by
configuration and input, never by timing.

- **Anchors decide at zero lag by default.** Every driver drains prior actuals
  before a non-B decide. Only B frames decide on their own burst's predictions,
  corrected at the next anchor. Burst-lagged anchors doubled the rate deviation.
- **`Y264_RCP_LAG` 1** lets an anchor decide one burst ahead of its actuals, so
  bursts can overlap under ABR the way they do under CRF. It's granted only where
  the wide staircase can engage (B-pyramid, no VBV, a pool of at least
  `Y264_MT_POOL_MIN`, 2) and is 0 at `--threads 1`. In-flight frames are
  predicted from their own complexity, not their type's mean. On the ABR board at
  twelve threads it bought 7-10% of wall on 720p and 1080p cells, with
  BD-VMAF-NEG against lag 0 inside the instrument's +-1.7 (sintel -1.9,
  stockholm +1.4).
- **Split anchor drain.** The anchor waits for the predecessor
  anchor's bits (which the decide needs) but leaves that burst's B frames
  flying. It buys 1.04-1.11x where it engages and costs about +1.3% BD-VMAF-NEG
  at `--ref 3` on cut-heavy content (sintel, over 48 seconds). 8-9 of 12 clips
  are byte-identical. It engages only when each GOP worker gets a pool of at
  least `Y264_MT_POOL_MIN` threads.
- **Warm-up.** The first 12 decides of each encoder run
  serial-tight so the models calibrate before any lag. P/B scales are seeded in
  the lowres-cost domain.

Under a VBV the buffer law is the safety-critical part, and the pipeline adds a
fallback gate. At each anchor's arrival, `rcp_vbv_gate` drains everything in
flight, then walks the coming burst through the buffer at 2 times
the predictions, or a decaying shock multiplier of recent overshoot when that's
larger. If any prefix underflows, the burst takes the serial schedule, where each
frame is coded, measured and re-coded at a higher QP if it doesn't fit. These
also go serial:

- IDR bursts and the warm phase, always.
- A content jump past 4x of the calibrated regime, in either
  direction (a missed cut, or a collapse like samsung's 12k-vs-1.5M anchor).
- Under capped VBR, the anchor after an intra frame or after one the buffer
  raised 3+ QP (`Y264_VBV_POSTI`), and any burst starting under a quarter full
  (`Y264_VBV_LOW`).
- Under CBR, a burst whose rate factor sits more than 2 QP under the regime the
  model was calibrated at. Below there the model
  under-predicts an anchor by 2-5x. About 20% of CBR frames take this path at
  six threads.

Within a pipelined burst, a B may not decide more than 6 QP below
the QP regime its model was calibrated at. The VBV model runs on
`max(lowres inter cost, lowres intra / 4)` in one domain for all types, so a
shock on one type propagates to the others.

**Determinism.** Two runs at `--threads 1` are byte-identical, and the lag is 0
there, so the serial schedule is exact. Above one thread the output may vary with
the thread count. Rate accuracy doesn't.

### The lookahead's role

The lookahead (40 frames at medium) feeds rate control four ways:

- **mb-tree** carries per-frame and per-block importance, which is why the CRF
  and RF2 bases can be flat.
- **The decide complexity.** P and B frames use the lowres min(intra, inter)
  cost captured at arrival, never the reconstruction, which may still be in
  flight. Under ABR it's floored at 0.2 x the VBV metric, because the raw cost
  collapses on static or noisy content (a 21x step between adjacent samsung
  frames, about 10 QP of oscillation). I frames use a full-resolution intra SATD.
- **The ABR opening QP**, fitted on the frames already in the window.
- **The VBV burst gate and plan**, which price buffered B frames from their
  lowres costs before they're coded.

The lowres analysis stores distortion and rate apart per block and per
reference leg (`y264_lr_blk`). Only the distortion half is read today. The rate
half is there for a future TPL-style consumer.

### Sync-lookahead

`--sync-lookahead N` lets the lookahead thread run ahead of the encode. It grows
the lookahead ring's capacity to `rc-lookahead + N` while every mb-tree and
scene-cut walk stays capped at the original window, so it changes no bits. The
only visible effect is N more `encode` calls returning nothing before the first
frame comes out. `yah264_lookahead_delay` reports it.

Auto resolves to `bframes + 1` when the frame-thread pool reaches
2 threads and 0 otherwise. `--sync-lookahead 0` and
`--tune zerolatency` turn it off, and the lookahead thread follows it: no lead,
no thread. The ring's array is `Y264_LA_CAP_MAX` (80) entries. With a lead of 4
at 18 threads, samsung_720p ran 15.2% faster, sintel_720p 12.3% and foreman_cif
4.2%. park_joy_720p was the one clip where it didn't pay (+0.8%).

The walks are bounded at `la_depth - 2`, not `- 1`. With no lead, the entry at
`la_depth - 2` is always the frame pushed in the same call and still untyped, so
it's never folded in. With a lead it's typed by the time it's read, and a `- 1`
bound would admit it. Byte-identity caught that at every lead on `--bframes 0`
and `--bframes 3`.

## What we chose and why

| Choice | Why |
| --- | --- |
| Flat CRF base under mb-tree, plus level 2.3 and fps slope 2.9 | mb-tree carries the per-frame allocation; the constants match bytes at equal `--crf` against x264 medium to a median 5.2% on 22 clips |
| Frame DC 1.3 under uncapped CRF | lowest worst clip on the CRF band, and it recovers long multi-shot titles (a 1080p animated feature: -1.81% at 1.04, -5.60% at 1.4); 1.4 starts to cost the static deep-band clip |
| Frame DC 1.04 under capped CRF and bitrate targets | at 1.3 the capped-VBR compliance gate with CBR HRD read 68/72; 72/72 at 1.04 |
| `--aq-strength` 0.4 | a seven-clip VMAF-NEG sweep put the optimum near 0.3 |
| RF2 ledger for ABR | the CRF path plus a rate factor; every constant was read on the ABR band and the capped-VBR tracking cells |
| Fitted ABR opening QP | a resolution-only seed opened 720p at QP 6 against a settling point of 27; the fit scores out of sample at rms 2.5 QP |
| Half-full handoff for segments | it's the CRF fill budget's fixed point, so segments compose without extra machinery |
| Zero-lag anchors | burst-lagged anchors doubled the rate deviation |
| Lag 1 above one thread | 7-10% of wall on 720p/1080p ABR at twelve threads, BD inside the noise |
| Two-pass offline plan always on | without it the I frame got the highest QP in its GOP (2.7x a B frame's bytes, against 18x with the plan); with it two-pass beats one-pass ABR on all seven corpus clips (-1.61% to -35.66% BD-VMAF-NEG) |
| Shared ABR ledger across GOP workers | 87.4 against 89.8 VMAF-NEG with six workers on a 1080p title |
| Buffer charged slice payload plus framing | payload alone drifted 30 kbit over a few hundred CBR pictures |
| CBR dive rule at 2 QP | 3 was the largest with no underflow on sintel's four CBR rungs at four and six threads; 2 keeps a QP of margin |
| Sync-lookahead as ring capacity | fencing the window instead would change mb-tree's math; capacity is pure latency |

## What didn't work

| Idea | Why not |
| --- | --- |
| Burst-lagged anchor decides | twice the rate deviation of zero-lag anchors |
| Lag with in-flight frames predicted at their type's mean | sintel +14.4% BD: a black opening credited each in-flight frame at the mean |
| Lag granted at `--ref 3` without width | 7 of 11 clips lose (akiyo +20.66%) and sintel's rate control fails (+212% over target) |
| Launching the anchor before its predecessor drains | the decide prices the burst off missing actuals; samsung +3.31% for the least speed |
| A bit-neutral launch split | analysis consumes the QP from its first statement; at most 1.4% of wall is QP-independent |
| Lowres intra energy as ABR decide complexity | park_joy -1.67%, akiyo +13.03%: near-zero inter cost on static content is a correct read |
| Geometric blend of intra and inter cost | akiyo +22.06% |
| A content term on the flat CRF base | +0.6 costs sintel 10%, -0.6 costs akiyo 1.6% |
| Dropping the B cascade under CRF | ducks -8.92%, sintel +7.00%, no net |
| An exit constraint on capped-CRF segments | never bound on 30 of 36 cells; where it did, it cost a compliance cell and spent 600 kbit/s more |
| Fill horizon 1 for capped CRF | underflows a cell at four threads |
| `Y264_ABR_QCOMP` 0.7 | +45% BD |
| Rate-aware lookahead cost consumed by mb-tree | mean +0.08%, foreman +0.91%, akiyo +1.36%; pricing MV rate cuts propagation where mb-tree wants to boost |
| Lowres SATD candidates in full-res decisions | +0.28% BD; the lookahead feeds frame-level decisions only |
| A lookahead thread with no lead | 1.00-1.02x, and +1.5% to +10.6% wall on the clips the lead wins on |
| Gating the lead on frame size | clips sharing one wavefront knee read -15.2%, -12.3% and +0.8% |

## Open questions

- **In-burst B lag.** B frames still decide on their own burst's predictions.
  Letting an early B's actual bits feed a later B's decide needs the burst's B
  preps privatized, which is a pipeline change, not a rate-control one.
- **The split drain's cost rests on one clip.** Only sintel both moves under
  the split and is long enough to measure over several GOPs.
- **No cheap first pass.** Pass 1 is close to half of two-pass wall.
- **Concatenated VBV compliance isn't proved**, only measured, while the
  predictor's error tail stays unbounded.
- **A TPL-style consumer** of the stored distortion and rate record hasn't been
  built.

## Measuring it

- **Equal CRF isn't a matched operating point.** The two encoders sit at
  different places on their RD curves. Match on achieved bitrate:
  `scripts/crf-solve.py` solves yah264 first (it can only reach whole-QP rungs)
  and then x264 onto that rate. `make parity-status-crf` drives it.
- **`make parity-modes`** runs the six-mode matrix against x264 (slow;
  `MODES="cbr 2pass"` restricts it). Read the signed rate error on both sides.
  x264's ABR converges over a clip, so its error depends on the window length.
- **`scripts/cvbr_compliance.sh` drives CRF with a cap**, never a bitrate, so it
  doesn't exercise ABR's VBV. `scripts/vbv_check.py` checks any stream against
  the encoder's buffer, `scripts/hrd_check.py` against a declared HRD.
- **A rate divided by the wrong duration** reads as a large undershoot. Check the
  frame count before believing one.
- **Check engagement before pricing a threading knob.** `Y264_STAIR_STAT=1`
  shows wide launches and concurrent bursts. An unengaged arm is byte-identical
  and reads as a 0.00% BD. Engagement depends on threads per GOP worker, so a
  long clip can be inert at 18 threads and engaged at 36.
- **Short single-GOP windows aren't quality measurements.** sintel's drain-split
  cost read +11.70% at 144 frames and +1.32% over 1152.
- **`touchdown_420` can't be used in a BD round.** Three in-band ladders of the
  same pair read +4.27%, +61.01% and +1.31%.
- **Read the VMAF band before believing a BD cell**, and pass `--no-cache` to
  bdcompare. It keys on the command string, and a rebuild doesn't invalidate it.
- **`Y264_LA_BUF` overrides downward too.** Print the resolved lead before
  pricing it.
- **Lag changes need decode gates**: `scripts/recon_thread_gate.sh` and
  `scripts/abr_decode_gate.sh`. Conformance forces the serial path and never
  reaches the lagged one.
