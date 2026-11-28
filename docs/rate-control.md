# Rate control

Rate control picks the base QP of every frame. Per-macroblock AQ offsets,
mb-tree's propagation offsets and the I/P/B cascade are all finer adjustments
on top of that base. yah264 has six modes: CRF, CQP, one-pass ABR, ABR under a
VBV (CBR and capped VBR), capped CRF and two-pass. Each mode has to keep working
when frames are coded out of order on many threads. Most of this doc is about
the model each mode uses to pick a QP and when that model gets to see the bits
it actually spent.

The models, the VBV and the pipelined decide and account live in
`src/encoder/rate.c`. Mode setup (`yah264_encoder_open`), the frame-type
cascade (`frame_qp`) and the lookahead are in `src/encoder/encoder.c`.
[options.md](options.md) lists the flags, and [knobs.md](knobs.md) has each
`Y264_*` default quoted here. Quality figures on this page are on VMAF v1,
except rows marked "earlier metric", which were read on VMAF v0.6.1. Rows
marked "older build" were read on a build that still had the arm's code,
before later default changes.

## How it works

### Picking a mode

| You are delivering to | Mode | Flags |
| --- | --- | --- |
| nothing that constrains the rate (masters, archives, quality work) | CRF | `--crf N` |
| streaming VOD or ABR ladder rungs | capped CRF | `--crf N --vbv-maxrate M --vbv-bufsize B` |
| a file-size target in one pass | ABR | `--bitrate N` |
| a file-size target with the whole clip available | two-pass | `--pass 1`, then `--pass 2 --bitrate N` |
| an average rate under a peak ceiling | capped VBR | `--bitrate N --vbv-maxrate M --vbv-bufsize B`, M > N |
| a fixed-bandwidth link | CBR | `--bitrate N --vbv-maxrate N --vbv-bufsize B` |
| measurement or debugging | CQP | `--qp N` |

Each of `--qp`, `--bitrate` and `--crf` names a mode. When more than one is
given, the last on the command line wins. A warning on stderr names the flag
that lost. `--pass` stays outside that contest because it's a mode plus a stats
round trip whose target is `--bitrate`. A `--crf` beside it is dropped with a
message. A VBV needs both `--vbv-maxrate` and `--vbv-bufsize`.

The base QP each mode picks then goes through `frame_qp`: I frames take
base - 3, B frames take base plus a depth cascade, and `--qpmin`/`--qpmax`
bound the coded QP. Two-pass sets the coded QP directly and skips the cascade.

### CQP

Every frame gets the QP you named, through the I/P/B cascade and nothing else.
CQP turns off both AQ and mb-tree, so `--qp 26` really is constant QP. The
library forces `aq_strength` to 0 itself, as well as the CLI, and
`mbtree_skip` switches mb-tree off. CQP is an instrument for measurement. You
can't deliver to a rate with it because integer QP can't get closer than about
+-7% of a rate, and real clips miss by more.

### CRF

CRF is open loop: no coded bit ever feeds back into the QP. With mb-tree on (the
default whenever there are B frames or a lookahead), `rc_set_qp_crf` gives every
anchor the same base:

    qp = crf + level + 2.9 * log2(fps / 30) + frame_dc

mb-tree's offsets are negative on average because they lend QP to the blocks
other frames reference. A flat base at `crf` would overspend. That's why
`level` (`Y264_CRF_LEVEL`, 2.3) and the fixed fps slope of 2.9 exist. The two
constants put our bytes at `--crf N` on top of x264's at the same `--crf` on 22
clips from 24 to 60 fps. The median miss is 5.2% and the worst cell 27.7%.

Per-frame importance comes from the mb-tree field. `frame_dc` is the
frame-level content term: the frame DC of the AQ field, whose strength by mode
is in [the AQ strength table](#frame-level-aq-strength-by-mode). B frames
inherit their anchor's base and add the cascade.

Rounding the base to an integer (`e->qp = (int)lround(qp)`) turns CRF's rates
into a staircase. Fractional `--crf` is parsed and stored, although it only
matters where it crosses a rounding boundary. Reachable rates come in steps of
one QP, roughly 12-13% apart, and CRF can't be solved to an arbitrary bitrate.
Search on a grid of 1.0 because a 0.1 grid oscillates between plateaus.

Without mb-tree (`--bframes 0 --rc-lookahead 0`), CRF falls back to a
complexity equation on the ME-compensated lowres cost, `CRF_BASE_CPLX` and a
capped downward adjustment.

### Capped CRF

Capped CRF is CRF with a VBV that can only take bits away. Where the cap is
slack, the encode is the CRF encode bit for bit. Where it's tight,
`vbv_fill_budget` steers the buffer toward half full. Above half full a frame
may spend its arrival plus the whole surplus so that content the cap doesn't
need keeps its CRF quality. Below half full a frame spends its arrival less
half the deficit. It never spends less than half an arrival. The budget reads
only the declared buffer and rate. Because the occupancy advances on actual
bits, a misprediction shows up as lower fill on the next decide.

The mode depends on this budget. Without it nothing under CRF looks at bits
until the fit test fires at a nearly empty buffer. A rate factor whose natural
rate is above the cap then drains the buffer unopposed and runs pinned at empty.
`--crf-max` bounds how far the buffer may raise a frame's coded QP. The buffer
is then allowed to under-run. That's the trade the flag asks for.

### One-pass ABR

With mb-tree on, ABR is the RF2 model: CRF's flat base plus a slowly adapted
rate factor. Two terms move the anchor base QP.

- **The ledger.** Each coded frame adds bits x qscale(its QP) to a work sum and
  one frame of target to an owed sum. The ratio is the qscale at which the
  frames so far would have spent exactly their target. It's self-normalizing
  and has no gain. It moves at most 0.25 x `--qpstep` per anchor decide.
- **The feedback.** Bits spent beyond bits owed map to a QP offset bounded at
  +-6. The window is `--ratetol` x 2 seconds of target and widens with the
  square root of the coded duration. Gains are 16 under and 12 over. CBR uses
  gains of 10 and 4 over a window a quarter of the buffer long.

The ledger opens on one frame of prior at a fitted opening QP, clamped to
15..45: `24.63 + 2.42 log2(cme/MB) - 4.12 log2(bits/MB)`. It reads the median
lowres inter cost of the frames already in the lookahead. Because we fitted it
on fixed-QP encodes of 32 clips across QP 16-44, its rate term is read within
each clip. The opening QP is DC-free like the rate factor: each frame codes at
it plus its own frame DC.

An older fit read near each clip's own operating point,
`8.69 + 2.92 log2(cme/MB) - 1.32 log2(bits/MB)`, does two jobs beside it.
First, the ledger opens at the lower of the two. That keeps static content at a
low QP, because its opening I and first P frames are what the GOP lives on.
Second, where the older fit falls under 15, the lowres cost is too small for
either fit. Flat 4K animation does this, and so do dark or black openings. The
opening then comes from the intra fit, 8 QP lower. The opening matters because
the cumulative ledger never forgets its first second.

Unpredictable content opens at the older fit instead. That's a window whose
median lowres inter cost is over 0.9 of its intra cost and whose opening isn't
dark. Full-swing noise is one example. A dark fade at 1080p also reads near 1,
which is why a dark opening doesn't count. Such content keeps its opening I
whole and gets no slew window because every frame there costs about what an I
does. None of the opening rules below apply to it.

Two more rules cover the first seconds of an encoder that opened cold instead
of from a carried ledger. The first is a weight on the opening I. After a
lowres-fit opening on an unshared ledger, the opening I counts at half its work
when it cost up to 10 frames of target, rising to its whole work at 16. A cheap
I's bits are spent once. A young ledger would read them as the cost of every
frame to come. An I that cost many frames' target was opened low. A short
encode has to repay it. Its bits always count in full in the feedback.

The second rule is a faster slew. For the first 2 seconds the ledger may fall a
whole `--qpstep` per anchor decide. The exception is a dark frame (mean luma
within 16 of coded black, on the 8-bit scale) while predictions are in flight.
Because a fade from black prices those predictions at the near-black frames'
cost, the ledger falls at the normal slew then. After a lowres-fit opening on
an unshared ledger it may also rise a whole step. A ledger shared by GOP
workers rises at 0.35 of a step. It keeps every I whole because those recur
once a GOP. After an intra-fit opening a rise is mostly the opening I's own
cost. The ledger rises at 0.35 there, or at the normal 0.25 when the opening I
was set under the fit's level. On any opening route a dark opening I never
codes under QP 15 with its frame DC included.

With no lookahead (`--rc-lookahead 0`, B frames on) two more rules apply to
an encoder with its own ledger, outside CBR and two-pass. In the first 2
seconds after an intra-fit opening that isn't dark, once 4 frames have been
coded after the opening I, a ledger whose coded frames alone read more than
12 QP over the slew reference is followed whole instead of at the opening's
slew (0.35 of a step, or 0.25 when the opening I was set under the fit's
level): on noise every frame costs what an I does, so the rise isn't the I's.
And past the first 4 seconds, when the ledger reads more than 4 QP over what
the last second of frames would read on their own, its history is scaled down
to sit 4 QP over them. Nothing else tells a ledger with no lookahead that the
content changed, and after an expensive run (noise before a static scene) it
would otherwise hold that run's QP for the rest of the encode.

The history set aside is kept, with the ledger's QP before the first cut. The
restore needs an earlier cut and happens at most once: when the last second
reads no more than 6 QP under that level and 2 QP over the ledger, the
expensive content has come back, so the history is restored, that anchor
follows the ledger whole (the next one, when that decide is a mid-stream I),
and the rule turns off for the rest of the encode, since content that
alternates is what the whole history is for. A scene that is only costlier
than the cheap stretch reads well under that level and leaves the history
alone. Capped VBR keeps cutting and never restores, since its buffer already
bounds each burst. Near-black frames, which the ledger doesn't count, don't
count toward either time.

CBR keeps the older fit as its opening because its buffer already holds the
rate. The refit's opening also ran short CBR buffers a few points over. After
an intra-fit opening CBR keeps its first-two-second slew: a full-step fall
whether or not frames are in flight, and a rise at 0.35 of a step (0.25 when
the opening I was set under the fit's level). None of the other rules above
apply to CBR. It gets no floor on the coded opening, no weighted I and no slew
window after a lowres-fit opening.

Mid-stream I frames take a decayed track of the P base QPs (decay 0.9). B
frames take the anchor's base plus the cascade, as under CRF. Frames whose
decide complexity per MB is below 4, such as a black opening, stay out of the
ledger but still count in the feedback.

With no B frames and no lookahead (`--bframes 0 --rc-lookahead 0`), ABR uses a
per-frame model: QP from
`bits ~ scale x C^(1-qcomp) / qscale` with per-type scales, `Y264_ABR_QCOMP`
0.6. `--abr-model rf` swaps in a per-frame rate-factor variant there. Anywhere
else it has no effect, `--no-mbtree` included.

### CBR and capped VBR

Both are ABR plus a VBV. The encoder calls it CBR when `--vbv-maxrate` is at or
below `--bitrate`. On top of the ABR decide:

- **The fit test** (`vbv_limit_at`): a frame must fit in 0.9 of the occupancy it
  is removed from.
- **The burst plan** (capped VBR only, `Y264_VBV_PLAN` 1): at each anchor the
  plan walks the whole burst (anchor plus buffered B frames) through the buffer
  law at the model's predictions. The base is the lowest QP at or above the
  rate factor's that keeps every frame inside the fit test and puts the buffer
  on a trajectory toward half full over 32 frames. A GOP coded in parallel
  starts from an assumed half-full buffer. There the target is half a buffer
  below the highest occupancy the segment has reached, clamped between a
  quarter full and half full. Without the plan a cheap opening banks a surplus.
  The hard section then drains the buffer inside a second. From there the
  encode runs pinned near empty with a +-4 QP sawtooth.
- **Serial re-coding** for bursts the model can't price (see
  [under frame threading](#rate-control-under-frame-threading)).

The buffer is charged each picture's slice payload plus its NAL framing. The
payload alone undercounts by about 60 bits a picture. Over a few hundred CBR
pictures that drifted to 30 kbit of phantom occupancy.

`--nal-hrd vbr|cbr` writes the buffer model into the stream: `hrd_parameters`
in the SPS, a `buffering_period` SEI at every IDR, `pic_timing` on every
picture. `BitRate` and `CpbSize` are `--vbv-maxrate` and `--vbv-bufsize`
rounded up to the syntax grid. The first buffering period's initial removal
delay is the occupancy the encoder started from. Each later one is the gap
between the previous picture's final arrival and this one's removal. That gap
is at most a full buffer, as C.3 requires. `cbr` also pads every access unit up
to the rate with filler.

The encoder holds the declared buffer as well as describing it. Each picture's
slices are measured, SEIs and emulation bytes included, against the room left
before the picture's removal time on the declared schedule. They're re-coded
at a higher QP until they fit or the coded QP reaches 51. At 51 the picture is
coded once more without its per-macroblock offsets.

An inter picture is then coded once more with every macroblock that can be
skipped skipped when it's still late on the declared schedule, or when the
reserve below says it would make a coming key late and the picture by itself
costs over twice what arrives per picture. A picture that's all prediction
beats one that stalls the decoder. The rate control's own buffer model alone
never skips a picture.

The room left after a picture is a reserve for the next key the encoder can see
coming: the keyint key or a scene cut the lookahead flagged. The reserve is
what the last intra picture cost at QP 51, less what arrives before that key.
When the coming key is a scene cut ahead of the keyint key, or a key the
lookahead has already released, that cost is scaled up by the two keys' lowres
intra costs if the coming one's is higher (at most 16 times, and not under
field coding). Under field coding the reserve also covers
the second field of an intra first field. Because the rate control never sees
these raises, a frame raised by `--qpstep` or more makes the next frame of its
type start from the raised QP. A frame with room to spare lowers that start. A
key, a cut or a lookahead window without use drops it.

The encoder counts a key still late at QP 51, or an inter picture still late
once skipped, and ends with one warning naming the count. The reserve is an
estimate, so a key can still arrive late after a cut into content far harder
than the lookahead's lowres costs show. The warning says so when that happens.
Pass 1 of a two-pass encode is not held. Its stream still has the HRD SEIs but
answers to nothing. Send it to `/dev/null`. Under `--nal-hrd` the encoder
refuses `--crf-max`, `--qpmax` under 51, forced QPs and `--zones q=` because
each holds a QP under what the buffer may need. `scripts/hrd_check.py` reads
the declaration back and simulates Annex C.

### Two-pass

Pass 1 codes at a fixed base QP of 26 with full analysis (there's no reduced
first pass) and writes one record per picture (two a frame under
`--tff`/`--bff`): type, complexity, bits, QP, is-reference. Pass 2 turns each
record into a QP-invariant cost, `(bits + 1) x qscale(qp)`, blurs it over
neighboring frames (`--cplxblur` 20), and solves an offline plan for the whole
clip with I and B tied to P by `--ipratio` 1.4 and `--pbratio` 1.3. A bounded
runtime correction tracks the actual spend. Frames pass 1 coded at almost no
bits (grain or texture under its quantiser) say nothing about what a finer QP
costs, so when enough of them would be planned far under pass 1 the plan holds
them at most 15 QP under it, and the runtime prices them at their own measured
cost as they land. When the floor stays off, such frames code no finer than 4 QP
under pass 1 until two of them have landed, on any clip. Once they cost 4 times
the model or more, they stay held there and the correction's band widens by
what they cost. On every picture of a plan the floor left off, and on those
after a floored plan's last floored frame, the correction lowers the QP by at
most 2, and an overspent budget holds the picture at the top of its band.
`--pass 3` reads the records and writes them back from a
real encode, so a further pass refines against it. Pass 2 needs the same
`--frames`, `--keyint` and `--tff`/`--bff` as pass 1. It refuses a file from
the other field mode, from another `--keyint`, with a malformed record, or
whose sections don't hold the records their markers count, or that was cut
short (a file one encoder wrote, and a threaded pass 1's merged file, end
in a line that counts its records; the GOP markers count each section's),
and warns of any pictures it codes past the file's last record. A VBV on one pass and not the
other is fine: pass 2 splits as pass 1 recorded. Pass 2 refuses a missing
stats file. Pass 3 can start one: it warns that it runs without a plan, codes
every frame at the base QP and writes what it coded. One that is there but
can't be read is refused.

**With a VBV**, pass 1 codes as capped ABR at the target with its base QP never
under 26. Frames are then recorded at QPs that say what a finer QP costs.
Because each of pass 1's GOPs starts its rate control afresh, like GOPs record
at like QPs.

Pass 2 codes one frame at a time and puts its frame threads on the row
wavefront. Like the single-pass modes, it holds every frame to the buffer by
its measured size. A frame that comes out over the fit test, the GOP's exit
corridor or the whole buffer is re-coded at the QP the overshoot asks for. At
QP 51 it's coded once more without its per-macroblock offsets. A raise of 3 QP
or more that comes out under half its limit steps back. It bisects toward the
last QP that failed for at most four more builds and keeps the lowest QP that
fits.

A picture still underflows where it reaches QP 51. With GOPs coded in parallel
it can also underflow within a GOP coded beside one that reached QP 51 and left
the buffer below its planned level, because that GOP plans on the level it
assumed. Those pictures are counted over the stream the GOPs make together and
named in one warning at the end. Each frame also leaves the next frames what
they cost at QP 51, priced from their pass-1 records. It leaves the next GOP's
key what that key costs at its planned QP.

The splitter caps each GOP's budget at what the cap delivers over the GOP plus
half the buffer and hands the rest to the other GOPs. With the cap at or under
the target it also floors each budget at what arrives over the GOP less half
the buffer. The shares are then one water level between floor and cap that
sums to the whole. The splitter also plans the level each GOP joins at by
walking the buffer over the GOP budgets, never below the next key's cost. With
the cap at or under the target, the last GOP is held by its measured sizes to
the level the walk ends at.

A GOP coded right after its predecessor continues its buffer at the exact
occupancy. Any other GOP starts at the planned level. The GOP before it leaves
at least that much but is held to the level only where what its remaining
frames give back at QP 51 couldn't make it up. A GOP takes over the spend
against budget of the GOP it continues from, even when a different worker
coded that GOP. Under a VBV the split's unit is at least 30 frames because
every split point costs a join. GOPs shorter than that go to each encoder
several at a time. Their keyframes stay at `--keyint`.

When the cap is at or under the target, pass 2 steers to half the next key's
planned size, or half the buffer where that is less. A frame the buffer has
room for spends it, up to what its budget allows. `--nal-hrd` always runs one
encoder. Under `--nal-hrd cbr` that encoder ends with the buffer where it
began.

### Frame-level AQ strength by mode

Per-MB AQ is `--aq-strength` times a per-frame gain. The strength is 0.25 in
every rate-controlled mode and 0 at CQP. Because the AQ offset under CRF and
ABR is anchored to an absolute energy level instead of the frame mean, the
field has a frame DC. `crf_frame_dc` adds that DC to the anchor's base QP at its
own strength, `Y264_AQ_DC`. It weights each MB by a rate proxy at 0.25 so that
a mostly flat frame doesn't take a base far below what its blocks cost. B
frames with no mb-tree field get their frame-level term from a pull toward a
fixed level instead. The pull has strength 1.3 around a level of 6.0.

| Mode | Frame DC strength | Where it goes |
| --- | --- | --- |
| CRF, no VBV | 1.3 | anchor base QP |
| capped CRF | 1.04 | anchor base QP |
| one-pass ABR, capped VBR (mb-tree on) | 1.04 | coded base QP only, while the ledger and opening stay DC-free |
| CBR | none | |
| two-pass | none, but non-reference B frames get the pull directly at 1.3 | |
| CQP | none, AQ off | |

Under one-pass ABR the DC reads a much lower variance floor of 1/256 so that a
black frame reads as black. Capped VBR keeps the field's floor like CRF because
its cap already bounds a black frame's overshoot.

### Rate tracking

ABR's accuracy comes from the ledger plus feedback. It's held to the shared
parity bar: every rung of a real-title VBR cell comes in within +-10% of target
over the whole cell. On the band at six threads, CBR tracks to a median 0.52%
and a worst of -2.5%. Delivered rate barely moves with thread count, even where
the bitstream does.

The CLI runs one encoder per GOP and codes the GOPs in parallel. Across those
GOP workers ABR keeps one rate problem:

- **Carry** (`Y264_RC_CARRY` 1): a GOP opens from its predecessor's exported
  state. The GOPs in between are credited at target.
- **Shared ledger**: concurrent ABR GOPs read and write one rate factor.
  Chained through the carry alone, the first W GOPs each solved their own rate
  problem cold. Every later one read a ledger W GOPs stale. In the table below,
  six unshared workers undershoot the target by three times as much as one
  worker and score lower. The shared ledger isn't used under two-pass, whose
  budget is already per GOP, or under `--gop-threads`.
- **Two-pass** splits the stats file at the GOP markers a threaded pass 1
  writes. Each GOP takes the share of the budget its complexity earns. Because
  a serial pass 1 writes no markers, its pass 2 runs serially too. Under a VBV
  the GOPs join at levels the splitter plans (see [Two-pass](#two-pass)).
- **`--keyint 1` with a VBV** runs one encoder: a one-frame GOP has no frame
  of its own to correct the buffer level its segment assumed.

Sol Levante at 1080p, 2494 frames, capped VBR at 12.5 Mbit/s, `--threads 6`:

| GOP workers | build | rate vs target | VMAF v1 |
|---|---|--:|--:|
| 6, shared ledger | current | -3.5% | 95.11 |
| 1 | current | -3.0% | 94.39 |
| 6, unshared | older build, AQ 0.25 | -9.5% | 92.96 |
| 6, shared ledger | older build, AQ 0.25 | -3.4% | 94.36 |
| 1 | older build, AQ 0.25 | -2.8% | 93.71 |

### What VBV actually guarantees

**The guarantee is strict per segment, from the assumed occupancy.** The first
GOP starts full (or at `--vbv-init`). Each later GOP assumes only the handoff
level of half the buffer, which is the CRF fill goal's fixed point. A two-pass
pass 2 instead assumes the level its GOP split planned (see
[Two-pass](#two-pass)). That's what chunked delivery needs: each segment
decodes legally on its own.

**Capped ABR joins segments explicitly.** A GOP coded right after its
predecessor by the same worker (every GOP at `--threads 1`) continues that
buffer at its exact final occupancy. A one-worker encode is therefore one
buffer end to end. With more workers, a GOP followed by another's is held to an
exit corridor (`yah264_encoder_vbv_handoff`) that narrows over its last frames
until the final one leaves the buffer at the handoff level. Capped CRF needs no
corridor because its fill budget already returns to that level.

**For the stream concatenated end to end, the guarantee is designed and
measured to hold but not proved.** Whether each segment exits at or above the
handoff depends on the rate loop returning to its fixed point. That loop runs
on a predictor whose error tail is unbounded: actual over predicted bits has a
p50 about 0.85 and a p99.9 in the hundreds. The serial re-coding below closes
most of that gap. A cut the buffer can't hold at QP 51 still gets through.

### Rate control under frame threading

Frames are coded in a staircase of overlapping bursts. A burst is an anchor and
its B frames (see [threading.md](threading.md)). A QP decided before the
previous frames' bits are known is the whole problem.

**CRF and CQP** need no bits. They decide serially at arrival and take full
pipeline width. Their inputs (the lowres cost and the source for the frame DC)
are captured on the API thread.

**ABR, two-pass and anything with a VBV** use the pipelined decide
(`rcp_decide`). Their state splits into a committed ledger that advances only on
actual bits in coding order and a FIFO of decided-but-unaccounted frames with
their predicted bits. A decide reads the ledger plus those pending predictions.
When a frame is coded, a fill stores its actual bits. A pop moves them into the
ledger once a later non-B frame has been decided. Configuration and input fix
the decide and pop order, never timing.

- **Anchors decide at zero lag by default.** Every driver drains prior actuals
  before a non-B decide. Only B frames decide on their own burst's predictions,
  corrected at the next anchor. Burst-lagged anchors doubled the rate deviation.
- **`Y264_RCP_LAG` 1** lets an anchor decide one burst ahead of its actuals.
  It's read only in a dev_knobs build. Bursts can then overlap under ABR the way
  they do under CRF. It's granted only where the wide staircase can engage:
  B-pyramid, no VBV and a pool of at least `Y264_MT_POOL_MIN` threads. That
  minimum is 2. The lag is 0 at `--threads 1`. In-flight frames are predicted
  from their own complexity instead of their type's mean. On the ABR board at
  twelve threads it bought 7-10% of wall on 720p and 1080p cells. Its quality
  cost on VMAF v1 is in the table after this list.
- **Split anchor drain.** The anchor waits for the predecessor anchor's bits
  because the decide needs them. It leaves that burst's B frames flying. It buys
  1.04-1.11x where it engages. It costs nothing we can measure. On the older
  build, against the prologue drain it replaced, sintel at `--ref 3` reads
  -0.2% BD-VMAF v1 over 48 seconds. It engages only when each GOP worker gets a pool of
  at least `Y264_MT_POOL_MIN` threads.
- **Warm-up.** The first 12 decides of each encoder run serial-tight so the
  models calibrate before any lag. P/B scales are seeded in the lowres-cost
  domain.

The table prices lag 1 against lag 0 under one-pass ABR at three threads. Each
clip ran four rates over 120 frames. The null column is a near-inert arm on
the same rungs. A lag reading inside it says nothing. Negative favors the lag.

| clip | lag 1 | null |
|---|---:|---:|
| sintel_720p | -1.5% | -0.2% |
| foreman_cif | -1.0% | +0.4% |
| riverbed_1080p | 0.0% | -0.1% |
| park_joy_720p | +0.1% | 0.0% |
| ducks_720p | +0.2% | 0.0% |
| crowd_run_1080p | +0.5% | -0.3% |
| samsung_720p | +1.3% | 0.0% |
| stockholm_720p | +2.1% | +1.3% |

Under a VBV the buffer law is the safety-critical part. The pipeline adds a
fallback gate for it. At each anchor's arrival, `rcp_vbv_gate` drains
everything in flight. It then walks the coming burst through the buffer at 2
times the predictions, or at a decaying shock multiplier of recent overshoot
when that's larger. If any prefix underflows, the burst takes the serial
schedule, where each frame is coded, measured and re-coded at a higher QP if it
doesn't fit.

Under capped CRF on a frame pool, a burst that passes is walked a second time.
That walk uses how far the first builds of bounded frames recently came out
over their predictions, by a tenth of the buffer or more. The accounted shock
never sees that overshoot because the re-encode re-prices those frames. A burst
sent serial only by the second walk answers to the fit test alone, as it would
have pipelined, and not to the fill budget. These bursts also go serial:

- IDR bursts and the warm phase, always.
- A content jump past 4x of the calibrated regime, in either
  direction (a missed cut, or a collapse like samsung's 12k-vs-1.5M anchor).
- Under capped VBR, the anchor after an intra frame or after one the buffer
  raised 3+ QP (`Y264_VBV_POSTI`), and any burst starting under a quarter full
  (`Y264_VBV_LOW`).
- Under capped VBR with a buffer shorter than 8 coded pictures at the
  maxrate, or than two B runs plus their anchors (a run is at most 7 B
  frames), every burst coded on a frame pool. One anchor a few times its
  prediction is the whole buffer there. Under PAFF both counts are fields.
- Under capped CRF, the next two anchors after an intra frame the buffer
  raised 3+ QP over the rate factor.
- Under CBR and under `--nal-hrd` (pass 1 of a two-pass aside), every burst
  coded on a frame pool.
- Every frame of a two-pass pass 2, which still decides as one thread does.

Within a pipelined burst, a B may not decide more than 6 QP below the QP regime
its model was calibrated at. The VBV model runs on
`max(lowres inter cost, lowres intra / 4)` in one domain for all types. A shock
on one type therefore propagates to the others.

**Determinism.** Two runs at `--threads 1` are byte-identical. Because the lag
is 0 there, the serial schedule is exact. Above one thread the output may vary
with the thread count. Rate accuracy doesn't.

### The lookahead's role

The lookahead is 40 frames at medium. It feeds rate control in four ways:

- **mb-tree** supplies per-frame and per-block importance. The CRF and RF2
  bases can be flat because of it.
- **The decide complexity.** P and B frames use the lowres min(intra, inter)
  cost captured at arrival. They never read the reconstruction because it may
  still be in flight. Under ABR the cost is floored at 0.2 x the VBV metric
  because the raw cost collapses on static or noisy content. Two adjacent
  samsung frames showed a 21x step, about 10 QP of oscillation. I frames use a
  full-resolution intra SATD.
- **The ABR opening QP**, fitted on the frames already in the window.
- **The VBV burst gate and plan**, which price buffered B frames from their
  lowres costs before they're coded.

The lowres analysis stores distortion and rate apart per block and per
reference leg (`y264_lr_blk`). Only the distortion half is read today. The rate
half is there for a future TPL-style consumer.

### Sync-lookahead

`--sync-lookahead N` lets the lookahead thread run ahead of the encode. It grows
the lookahead ring's capacity to `rc-lookahead + N`. Because every mb-tree and
scene-cut walk stays capped at the original window, it changes no bits. The
only visible effect is N more `encode` calls returning nothing before the first
frame comes out. `yah264_lookahead_delay` reports it.

Auto resolves to `bframes + 1` when the frame-thread pool reaches 2 threads and
0 otherwise. `--sync-lookahead 0` and `--tune zerolatency` turn it off. The
lookahead thread exists only when there's a lead. The ring's array holds 80
entries (`Y264_LA_CAP_MAX`).

With a lead of 4 at 18 threads, samsung_720p ran 15.2% faster, sintel_720p
12.3% and foreman_cif 4.2%. park_joy_720p was the one clip where the lead
didn't pay, at +0.8%.

The walks are bounded at `la_depth - 2`, not `- 1`. With no lead, the entry at
`la_depth - 2` is always the frame pushed in the same call. Because it's still
untyped, it's never folded in. With a lead it's typed by the time it's read.
A `- 1` bound would then admit it. Byte-identity caught that at every lead on
`--bframes 0` and `--bframes 3`.

## What we chose and why

| Choice | Why |
| --- | --- |
| Flat CRF base under mb-tree, plus level 2.3 and fps slope 2.9 | mb-tree handles the per-frame allocation. The constants match bytes at equal `--crf` against x264 medium to a median 5.2% on 22 clips |
| Frame DC 1.3 under uncapped CRF | On the earlier metric it had the lowest worst clip on the CRF band and recovered long multi-shot titles. VMAF v1 sees little either way. On a 2-minute window of a 1080p animated feature, 1.04, 1.3 and 1.4 sit within 0.25% of each other against x264 medium. On the deep band, 1.4's cost on the static clips is inside the band's noise |
| Frame DC 1.04 under capped CRF and bitrate targets | the capped-VBR compliance gate with CBR HRD read 68/72 at 1.3 and 72/72 at 1.04 |
| `--aq-strength` 0.25 | against 0.4 on the real-title ladders it reads a median -1.0% at CRF and -0.6% under capped VBR; dark titles under VBR give back up to 0.6% |
| RF2 ledger for ABR | it's the CRF path plus a rate factor. We read each constant on the ABR band and the capped-VBR tracking cells |
| Fitted ABR opening QP | a resolution-only seed opened 720p at QP 6 against a settling point of 27. The fit reads rms 3.2 QP over QP 16-44 on 32 clips |
| Half-full handoff for segments | it's the CRF fill budget's fixed point, so segments compose without extra machinery |
| Zero-lag anchors | burst-lagged anchors doubled the rate deviation |
| Lag 1 above one thread | 7-10% of wall on 720p/1080p ABR at twelve threads. On VMAF v1 it's within 1% of lag 0 on five of eight clips and sintel favors it. samsung and stockholm go the other way (see the table under frame threading) |
| Two-pass offline plan always on | without the plan the I frame got the highest QP in its GOP, at 2.7x a B frame's bytes against 18x with it. On VMAF v1 over 120 frames, two-pass beats one-pass ABR on only three of seven corpus clips, from -2.8% (samsung) to +7.3% (sintel) |
| Shared ABR ledger across GOP workers | without it, six workers on a 1080p title came in 9.5% under target and read VMAF v1 92.96 against 93.71 with one worker (older build, see the ledger table above) |
| Buffer charged slice payload plus framing | payload alone drifted 30 kbit over a few hundred CBR pictures |
| CBR dive rule at 2 QP | under CBR, a burst whose rate factor sits more than 2 QP below the regime the bits model was calibrated at takes the serial schedule, because the model under-predicts there. 3 was the largest with no underflow on sintel's four CBR rungs at four and six threads. 2 keeps a QP of margin |
| Sync-lookahead as ring capacity | fencing the window instead would change mb-tree's math. Capacity is pure latency |

## What didn't work

| Idea | Why not |
| --- | --- |
| Burst-lagged anchor decides | twice the rate deviation of zero-lag anchors |
| Lag with in-flight frames predicted at their type's mean | a large loss on sintel, because a black opening credited each in-flight frame at the mean |
| Lag granted at `--ref 3` without width | 7 of 11 clips lose, akiyo worst, and sintel's rate control fails (+212% over target) (earlier metric) |
| Launching the anchor before its predecessor drains | the decide prices the burst off missing actuals. samsung pays the most bits for the smallest speed gain |
| A bit-neutral launch split | analysis consumes the QP from its first statement. At most 1.4% of wall is QP-independent |
| Lowres intra energy as ABR decide complexity | a small gain on park_joy and a large loss on akiyo: near-zero inter cost on static content is a correct read (earlier metric) |
| Geometric blend of intra and inter cost | a large loss on akiyo (earlier metric) |
| A content term on the flat CRF base | older build. Either sign hurts some clip on VMAF v1. +0.6 costs sintel 10.8%. -0.6 gains sintel 9.3% but costs akiyo 1.0% |
| Dropping the B cascade under CRF | ducks -3.1%, sintel +5.0% on VMAF v1, no net |
| An exit constraint on capped-CRF segments | never bound on 30 of 36 cells. Where it did, it cost a compliance cell and spent 600 kbit/s more |
| Fill horizon 1 for capped CRF | underflows a cell at four threads |
| `Y264_ABR_QCOMP` 0.7 | it loses on 9 of the 13 ABR band clips on VMAF v1, median +1.6%, akiyo +7.2% |
| Rate-aware lookahead cost consumed by mb-tree | a loss on the mean, largest on foreman and akiyo. Pricing MV rate cuts propagation where mb-tree wants to boost (earlier metric) |
| Lowres SATD candidates in full-res decisions | a small BD loss. The lookahead feeds frame-level decisions only (earlier metric) |
| A lookahead thread with no lead | 1.00-1.02x, and +1.5% to +10.6% wall on the clips the lead wins on |
| Gating the lead on frame size | frame size doesn't predict the lead's gain. Three 720p clips of one size moved -15.2%, -12.3% and +0.8% in wall with the lead |

## Open questions

- **In-burst B lag.** B frames still decide on their own burst's predictions.
  Letting an early B's actual bits feed a later B's decide needs the burst's B
  preps privatized. That's a pipeline change, not a rate-control one.
- **The split drain's cost rests on one clip.** Only sintel both moves under
  the split and is long enough to measure over several GOPs.
- **No cheap first pass.** Pass 1 is close to half of two-pass wall.
- **Concatenated VBV compliance is measured but not proved** while the
  predictor's error tail stays unbounded.
- **A TPL-style consumer** of the stored distortion and rate record hasn't been
  built.

## Measuring it

- **Equal CRF isn't a matched operating point.** The two encoders sit at
  different places on their RD curves. Match on achieved bitrate instead.
  `scripts/crf-solve.py` solves yah264 first because yah264 can only reach
  whole-QP rungs. It then solves x264 onto that rate. `make parity-status-crf`
  drives it.
- **`make parity-modes`** runs the six-mode matrix against x264. It's slow, and
  `MODES="cbr 2pass"` restricts it. Read the signed rate error on both sides.
  Because x264's ABR converges over a clip, its error depends on the window
  length.
- **`scripts/cvbr_compliance.sh` drives CRF with a cap and never a bitrate.**
  It doesn't exercise ABR's VBV. `scripts/vbv_check.py` checks any stream
  against the encoder's buffer. `scripts/hrd_check.py` checks it against a
  declared HRD.
- **A rate divided by the wrong duration** reads as a large undershoot. Check the
  frame count before believing one.
- **A clip that ends on credits reads under target over the whole clip.** A
  title card and credits on black code to almost nothing at any QP a one-pass
  controller reaches. sintel_720p's closing ten seconds are one such case. The
  reference encoder misses the same way. The parity cells stop before the
  credits. Read a corpus clip the same way.
- **Check engagement before pricing a threading knob.** An unengaged arm is
  byte-identical and reads as a 0.00% BD. Because engagement depends on threads
  per GOP worker, a long clip can be inert at 18 threads and engaged at 36.
- **Short single-GOP windows aren't quality measurements.** A rate-control
  arm read over one GOP is read on the controller's settling alone.
- **Read the VMAF band before believing a BD cell.** Pass `--no-cache` to
  bdcompare too, because its cache keys on the command string and a rebuild
  doesn't invalidate it.
- **`Y264_LA_BUF` overrides downward too.** Print the resolved lead before
  pricing it. It's read only in a dev_knobs build.
- **Lag changes need decode gates**: `scripts/recon_thread_gate.sh` and
  `scripts/abr_decode_gate.sh`. Conformance forces the serial path and never
  reaches the lagged one.
