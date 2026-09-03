# The rate-control mode matrix

`scripts/perf-comp-modes.sh` runs yah264 against x264 in CRF, CQP, ABR, CBR,
capped VBR and 2-pass, one table per mode. It prints no cross-mode average on
purpose: the spread across modes is larger than the spread across clips, so a
single number would hide the interesting part.

This document explains how each mode is made comparable, what the matrix
measures, and where a fair comparison is not available at all.

## Operating points

A comparison-table row is only readable at an operating point where the quality metric
still has headroom. Measured VMAF at 2500 kbit/s for the CIF clips and 12000
kbit/s for the 720p ones, 6-second windows, medium preset:

| clip | target | yah264 VMAF | x264 VMAF |
|---|--:|--:|--:|
| foreman_cif | 2500 | 99.55 | 99.61 |
| bus_cif | 2500 | 99.85 | 99.86 |
| stefan_cif | 2500 | 99.41 | 98.99 |
| samsung_720p | 12000 | 97.55 | 98.04 |
| park_joy_720p | 12000 | 90.81 | 90.80 |
| ducks_720p | 12000 | 75.47 | 80.69 |

Four of those six rows sit at VMAF 97.5 or above, which is visually lossless, so
`dVMAF` on them is noise around zero. 2500 kbit/s for 352x288 is roughly eight
times what anyone would spend on it. At the other end ducks sits at VMAF 75, far
below any point a service would ship, and its -5.2 VMAF is a real rate-matched
deficit rather than a saturation artifact.

It also distorts the speed number, which is what the table is for. Coding a
CIF frame to VMAF 99.5 means residual work no real encode does, so the ratio
comes from an operating point neither encoder was designed around.

**Neither encoder saturates at these rates.** Sweeping 3000-20000 kbit/s on the
three 720p clips and 500-8000 on the three CIF clips, no cell hits a QP floor:
yah264's signed rate error stays inside ±4.2% everywhere and x264's inside
±14%. The worst reading in the sweep is x264 *undershooting* stefan by 13.8% at
500 kbit/s, at the low-rate end. At 12000 kbit/s for 720p50 the QPs land in the
twenties.

**Trap: a rate computed from the clip's full duration against a trimmed encode
reads as a large undershoot.** The harness trims 500-frame clips to 300, and
dividing 300 frames of bits by 500 frames of duration produces exactly the
16.7% and 50% "undershoots" that this class of error is known for. Check the
frame count the rate was divided by before believing any large rate deficit.

### The targets the matrix uses

Each clip is laddered on rate and VMAF together, and the target is the lowest
ABR rate where both encoders track and yah264 lands in VMAF 88-94: high enough
to be a deployment point, low enough that the metric still separates the two
encoders.

| clip | target | yah264 VMAF | x264 VMAF | y264 rate err | x264 rate err |
|---|--:|--:|--:|--:|--:|
| foreman_cif | **400** | 93.11 | 94.78 | +1.6% | -2.7% |
| bus_cif | **400** | 94.27 | 94.42 | +1.8% | -8.4% |
| stefan_cif | **400** | 92.22 | 88.63 | +3.3% | -15.6% |
| samsung_720p | **1200** | 88.30 | 92.44 | -1.4% | -1.2% |
| park_joy_720p | **12000** | 90.81 | 90.80 | +2.3% | +4.8% |
| ducks_720p | **25000** | 90.11 | 92.10 | +3.2% | +11.7% |

Two caveats. The rate error is not zero and cannot be made zero: x264's ABR
undershoots high-motion CIF badly (bus -8.4%, stefan -15.6%) and overshoots
ducks (+11.7%), which is a property of x264's rate control rather than anything
a target choice fixes. On stefan there is no target in the useful quality band
where the two agree within a few percent: at 5000 kbit/s they agree to 0.9%, but
5000 puts the clip back at VMAF 99.8. `perf-comp.sh` prints the signed error for
both sides on every targeted row, and it is meant to be read.

The second caveat is that x264's ABR error depends on how much of the clip you
encode. ducks reads +8.4% over the 300-frame window the harness uses and -1.1%
over the whole 500-frame clip, and samsung, which is short enough that the
window *is* the whole clip, tracks to within 1%. x264's ABR converges over a
clip and the harness truncates before it gets there. The 6-second window stays
because lengthening it slows `parity-status` by about 1.6x for a rate error the
table reports anyway.

Take that dependence seriously when reading the ducks cell: x264's error on it
appears three times in this file, +11.7% in the table above, +8.4% in the
paragraph just above, and +14.8% in the ABR block below. Those are separate
runs at different window lengths, so none of them is *the* value for the cell.

## Six modes, and what it takes to compare each one

### Equal CRF is not a matched operating point

The two encoders' rate factors are not the same scale, and how far apart they
sit is content luck: at equal CRF, foreman measures -10%, bus +9%, park_joy +44%
and sintel -55%. Comparing at equal rate-factor numbers produces a confident
dVMAF over two encodes that answered different questions.

The matrix matches on achieved bitrate instead, by bisecting each encoder's CRF
separately against the clip's ABR target. BD-rate would also normalise bits, and
it stays the right instrument for an absolute quality anchor, but it gives no
speed number, and speed is what a comparison-table row is for. Matching the rate gives
both at once.

The calibration is a result in itself:

| clip | yah264 CRF | x264 CRF | gap |
|---|--:|--:|--:|
| foreman_cif | 21.4 | 22.7 | 1.3 |
| bus_cif | 28.6 | 27.9 | -0.7 |
| stefan_cif | 26.2 | 27.4 | 1.2 |
| samsung_720p | 21.4 | 25.5 | **4.1** |
| park_joy_720p | 26.2 | 25.0 | -1.2 |
| ducks_720p | 22.6 | 20.8 | -1.8 |

The gap swings from -1.8 to +4.1 points across six clips and changes sign. Any
row comparing these two encoders at the same CRF number is comparing whatever
that swing happens to be on that clip.

Bisection hits a limit worth recording: **yah264's CRF is quantised to whole QP
steps.** `rc_set_qp_crf` ends in `e->qp = (int)lround(qp)`, so the fractional
part of `--crf` only matters when it crosses a rounding boundary, and the
achievable bitrates come in jumps of roughly 12%. bus_cif cannot be brought
closer than -4.8% of its target and stefan than +5.3% for that reason alone.
x264's qscale is continuous and lands inside 0.1%. This bounds how well any CRF
row can ever be rate-matched here.

### CQP is not matched work

x264's parameter validation forces mb-tree and aq off whenever
the RC method is CQP. yah264's `e->mbtree_on` has no `rc.method` term at all,
so it runs mb-tree at every mode, and a bare `--qp` head-to-head compares
different workloads.

The CQP rows set `Y264_MBTREE_OFF=1`, which is x264's policy applied to yah264,
and every such row is labelled `mbtree-off` so it is never silent. AQ needs
nothing: the CLI already zeroes `aq_strength` when `rc.method == 0`.

Equal QP has the same operating-point problem as equal CRF, so the QP is
calibrated per encoder per clip against the same targets. Integer QP means ±7%
is the best achievable, and one cell (bus, +12%) misses badly enough that the
harness marks it UNMATCHED.

### Capped VBR needs a cap that binds

A realistic delivery cap does nothing on this corpus. foreman at its calibrated
CRF runs 416 kbit/s, and caps of 480 and 420 with a one-second buffer both
return a byte-identical stream, on both encoders. Six-second single-scene clips
do not produce the bursts a capped-VBR ceiling exists to catch, so such a row
would silently duplicate the CRF row and exercise none of the VBV path.

The cap is set to 0.8x the ABR target instead, which forces the limiter to work.
That makes the row a measurement of the limiter rather than of a deployment
configuration, which is the right trade for a defect-finding matrix, but it is a
choice, and the un-bound behaviour above is the reason for it.

Compliance is checked stream-side with `scripts/vbv_check.py` on both encoders'
output, which is a property of the bitstream and settles in one decode. A capped
row whose cap was violated is an invalid encode, and reporting its speed would
be reporting the speed of cheating.

One limitation: yah264 writes no HRD parameters into the SPS, so an external
conformance checker has nothing to verify against. `vbv_check.py` simulates the
encoder's own leaky bucket, which catches a broken limiter but would not catch a
disagreement about what the bucket should be.

## The matrix

pure-C, 18 threads, 6-second windows, median of 5 runs, `Y264_REFENC_CACHE=0`.
`x264 x` above 1.00 means x264 is faster. UNMATCHED marks a cell whose two
encodes differ by more than 5% in size, where the VMAF delta is an
operating-point difference rather than a quality verdict.

```
== CRF (each encoder at its own calibrated rate factor) ==
clip             x264 x   y264 rate   x264 rate   dVMAF   dsize   notes
foreman_cif        1.4x       416k        401k    -0.30    +4%
bus_cif            1.4x       381k        399k    -2.00    -5%
stefan_cif         1.6x       421k        402k    +1.30    +5%
samsung_720p       1.9x      1172k       1194k    +0.13    -2%
park_joy_720p      1.1x     11971k      11929k    -1.78    +0%
ducks_720p         0.8x     24738k      25156k    -0.73    -2%

== CQP (yah264 Y264_MBTREE_OFF=1 to match x264's CQP policy) ==
foreman_cif        1.4x       404k        423k    -0.43    -5%   mbtree-off
bus_cif            1.4x       430k        383k    +1.73   +12%   mbtree-off UNMATCHED
stefan_cif         1.3x       412k        395k    +1.58    +4%   mbtree-off
samsung_720p       1.8x      1161k       1202k    -0.27    -3%   mbtree-off
park_joy_720p      1.0x     11675k      12884k    -0.47    -9%   mbtree-off UNMATCHED
ducks_720p         0.8x     23953k      23216k    +1.58    +3%   mbtree-off

== ABR/VBR ==
foreman_cif        1.7x      +1.6%       -2.5%    -1.59    +4%
bus_cif            1.7x      +1.8%       -8.9%    +0.11   +12%   UNMATCHED
stefan_cif         1.7x      +3.3%      -14.4%    +3.08   +21%   UNMATCHED
samsung_720p       1.9x      -1.4%       -1.2%    -3.97    -0%
park_joy_720p      1.2x      +2.3%       +7.5%    -1.26    -5%
ducks_720p         0.9x      +3.2%      +14.8%    -0.25   -10%   UNMATCHED

== CBR (bitrate == VBV maxrate, 1s buffer) ==
foreman_cif        1.8x      +1.8%       -6.1%    -1.45    +8%   both VBV ok UNMATCHED
bus_cif            1.8x      +1.8%      -11.6%    +0.44   +15%   both VBV ok UNMATCHED
stefan_cif         1.9x      +3.4%      -23.2%    +6.27   +35%   both VBV ok UNMATCHED
samsung_720p       2.1x      -0.3%       -7.2%    -4.85    +7%   both VBV ok UNMATCHED
park_joy_720p      1.4x      +2.3%       -8.1%    -1.11   +11%   both VBV ok UNMATCHED
ducks_720p         1.0x      +2.8%       +2.5%    -1.53    +0%   both VBV ok

== capped VBR (calibrated CRF + binding VBV cap at 0.8x the ABR target) ==
foreman_cif        1.7x       343k        336k    -1.08    +2%   both VBV ok
bus_cif            1.7x       343k        298k    +1.42   +15%   both VBV ok UNMATCHED
stefan_cif         1.6x       363k        253k    +8.36   +44%   both VBV ok UNMATCHED
samsung_720p       2.0x       972k        923k    -1.01    +5%   both VBV ok UNMATCHED
park_joy_720p      1.3x     10856k       9267k    +0.59   +17%   both VBV ok UNMATCHED
ducks_720p         1.0x     21911k      19611k    -0.31   +12%   both VBV ok UNMATCHED

== 2-pass ABR (wall = pass 1 + pass 2; both passes threaded) ==
foreman_cif        2.7x      +0.5%       -2.1%             +3%
bus_cif            2.8x      +0.2%       -1.9%             +2%
stefan_cif         3.0x      +0.2%       +0.9%             -1%
samsung_720p       3.0x      +0.6%       +1.7%             -1%
park_joy_720p      2.2x      -0.2%       +0.1%             -0%
ducks_720p         1.8x      -0.2%       +3.5%             -4%
```

The 2-pass block carries no dVMAF column because the one it used to carry was
measured against a two-pass allocator that has since been replaced, and it was
wrong by a wide margin: it read two-pass as far worse than one-pass, and a
head-to-head re-run says the opposite. On samsung_720p at a 1200k target,
one-pass ABR scores 87.51 VMAF-NEG in 773,595 bytes and two-pass scores 91.25
in 750,885 -- better quality in fewer bits. That direction matches
[rate-control.md](rate-control.md), which has two-pass beating one-pass ABR on
all seven corpus clips. The speed and rate-accuracy columns above were never in
question.

## What the matrix finds

**The spread across modes is larger than the spread across clips.** The same
binary on the same clip is 1.4x off x264 in CRF and 2.7x off in 2-pass. The
modes range 0.8x to 3.0x, so quoting an ABR number as "the" gap misses the worst
mode by more than 1.5x.

**yah264 wins the CRF row on ducks and ties on park_joy** (0.8x and 1.1x), and
loses CIF by 1.3-1.4x. The pure-C gap is clip-dependent, and at these operating
points it is much smaller than a saturated table suggests.

**yah264's rate control is the more accurate of the two in every targeted
mode.** ABR: yah264 within +3.3%, x264 out to -14.4%. CBR: yah264 within
+3.4%, x264 out to -23.2%. 2-pass: yah264 within 0.6%. That accuracy is why so
many cells are marked UNMATCHED: the two encoders genuinely spent different
bits, and x264 is the one that missed.

**Capped VBR is compliant on all six clips at this cap**, where the limiter has
a per-frame buffer target to work against. It is not clean everywhere: at
tighter caps and across a keyint boundary the 36-cell gate in
`scripts/cvbr_compliance.sh` does not pass clean, and the residue has a
different mechanism, an unclamped I frame at every GOP start.

## What could not be made comparable

Three things resist, and saying so is more useful than a number that looks fine.

**stefan_cif in every targeted mode.** x264 undershoots it by 14% in ABR and 23%
in CBR at the operating point where the quality metric has headroom. There is no
target that fixes this: at 5000 kbit/s the two agree to 0.9%, but that puts the
clip at VMAF 99.8 where nothing is measurable. The cell is marked and excluded
from the mean.

**The CQP row on bus and park_joy.** Integer QP is a coarse instrument; the
nearest QP on either side of the target lands 12% and 9% away. Matching the two
encoders more tightly would need fractional QP, which CQP does not have.

**Five of the six capped-VBR cells.** They are UNMATCHED for the ordinary reason
the rest of this table has UNMATCHED cells: the two encoders' CRF scales put
them at different operating points, x264 lands 8-44% lower, and stefan is its
usual self at +44%. That is a calibration limit rather than a compliance one.
foreman is a matched cell with a real speed number (1.7x). Recalibrating the
cvbr rows against the achieved rates would close most of the rest, and the
constants at the top of `perf-comp-modes.sh` are stale for this row
specifically: they were bisected against a limiter that let 11-30% through.

## Findings around the edges

1. **Two-pass threads both passes, and the exclusion it replaced was doing real
 work.** The stats file matches records to frames by position and has no frame
 index, so N per-GOP encoders would truncate each other in pass 1 and all
 replay GOP 0's records in pass 2. Threading it needs GOP boundaries written
 into the file and a per-GOP budget drawn from each GOP's share of the global
 complexity sum, and that is what the format carries.

2. **Pass 1 is a full-effort encode.** x264 cuts subme and partitions on its
 first pass unless you ask for `--slow-firstpass`; yah264 has no equivalent,
 and grep for `tp_pass` turns up no writes to any effort knob. Pass 1 is CQP 26
 at full medium analysis, so 2-pass costs about twice a single pass.

3. **Two silent fallbacks.** `--vbv-maxrate` without `--vbv-bufsize` turns VBV
 off with no diagnostic. `--pass 2` with a missing or short stats file codes
 every frame at base QP 26 and looks like a CQP encode. Each produces a
 plausible-looking row for a mode that isn't running. Naming two rate-control
 modes is not in this class: the last flag on the command line wins and the
 loser is named on stderr.

4. **Stale comments in the source.** The comments beside the readers for
 `Y264_RC_PIPE` and `Y264_RC_PIPE_VBV` now say what the code does, which is that
 both default on. The struct-field comments in `src/encoder/encoder.h` still say
 "default off" for both. VBV is enforced either way, since the gate only chooses
 the pipelined path over the serial one, but the header says the opposite of the
 code.

## Re-running this

`make parity-status` is ABR-only and labelled as such in its own output. It is
quick enough to run routinely.

`make parity-modes` runs the matrix and takes tens of minutes. `MODES="cbr
2pass"` restricts it; `MODE=asm` and `THREADS=n` pick the tier. One mode at a
time is the practical way to run it.

`scripts/cvbr_compliance.sh` is the capped-VBR compliance gate that the cvbr row
only samples: 36 cells over three caps and both VBV paths, against x264's 18.

**Frame rates are never passed in by hand.** `perf-comp-modes.sh` passes a bare
number to `perf-comp.sh` that is its **CRF** positional (`<clip> [crf]
[seconds]`), not a frame rate. `perf-comp.sh` does not accept a frame rate from
a caller: it ffprobes the clip and hands `vbv_check.py` the source Y4M itself,
and `vbv_check.py --y4m` reads each clip's Y4M header and prints the rate it
resolved on every line. This matters for a corpus that mixes 29.97, 30 and 50.

The per-clip CRF and QP constants live at the top of
`scripts/perf-comp-modes.sh`. They are a property of the two encoders and go
stale when either changes. Regenerate by bisecting each encoder's CRF (or
integer QP) against the clip's ABR target until the achieved rate converges. Use
eleven iterations over [6.0, 44.0], keeping the closest probe rather than the
last one, which matters because yah264's whole-QP quantisation stalls the
search near the target. Staleness is not silent: every row prints both achieved
rates, and any cell whose two encodes differ by more than 5% in size is marked
UNMATCHED.
