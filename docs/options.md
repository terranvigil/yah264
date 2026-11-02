# yah264 options reference

Every option the encoder accepts, what it defaults to, and how it interacts with
the others. Read against `cli/yah264_cli.c`, `include/yah264.h` and
`src/encoder/params.c`. Where this file and the code disagree, the code wins and
this file is the bug.

Rate control has its own guide: [rate-control.md](rate-control.md). Read that
one before picking `--crf` or `--bitrate`. This file only says what the flags
are and leaves the choice to that guide.

## Contents

- [Invoking it](#invoking-it)
- [What the bare default is](#what-the-bare-default-is)
- [The option table](#the-option-table)
- [Presets](#presets)
- [Tunes](#tunes)
- [Which options change the bitstream shape](#which-options-change-the-bitstream-shape)
- [Threading, and what it does to your output](#threading-and-what-it-does-to-your-output)
- [x264 compatibility](#x264-compatibility)
- [Calling it from C](#calling-it-from-c)
- [Environment variables](#environment-variables)
- [Rough edges](#rough-edges)

## Invoking it

```sh
yah264 --input-y4m <in.y4m|-> [-o <out.264|->] [options]
```

Input is Y4M or raw YUV, with no container demuxer and no format sniffing.
`--input-y4m` takes a Y4M, and its header supplies width, height, frame rate and
chroma format. `--input-raw` takes raw planes, so those four have to be given.
Pipe from ffmpeg for anything else:

```sh
ffmpeg -i input.mp4 -f yuv4mpegpipe - | yah264 --input-y4m - --crf 23 -o out.264
yah264 --input-raw in.yuv --input-res 1920x1080 --fps 24 --crf 23 -o out.264
```

**Raw input says nothing about itself.** `--input-res` is required with it, and
a wrong geometry does not fail: it encodes garbage. `--input-csp`, `--fps` and
`--input-range` are refused alongside `--input-y4m`, so they can never silently
contradict the header.

A Y4M header carries one field that raw input cannot and that nothing on the
command line invents for it: the **sample aspect ratio**. A Y4M with `A128:117`
writes it into the VUI, while the same frames as raw do not. Pass `--sar` if you
want the two to agree byte for byte.

The depth travels with the chroma format (`--input-csp i420p10`) for the same
reason it does in the Y4M `C` tag. It still picks the library, since nothing
else on the command line says 10-bit. `--output-depth 10` over 8-bit raw input
upshifts exactly as it does over an 8-bit Y4M.

Output is an Annex-B elementary stream. `-` means stdin/stdout for either side.

The Y4M `C` tag is parsed for `420`, `422` and `444`, plus their `p10` form.
**The input's bit depth selects the encoder.** One binary carries an 8-bit
library and a 10-bit one, so a `C420p10` file is coded as High 10 with nothing
said on the command line. `--output-depth 10` upshifts every sample of 8-bit
content by 2 and puts it through the 10-bit encoder. `--output-depth 8` on
10-bit input is refused: taking samples away is a conversion, and ffmpeg owns
conversions.

## What the bare default is

`yah264 --input-y4m in.y4m -o out.264` with no other flags is `x264 --preset
medium`'s tool-set on purpose, so the two are comparable without an argument
list on either side. That means preset medium, CABAC, `--ref 3`, `--bframes 3`,
8x8 transform on, adaptive B on, `--rc-lookahead 40`, `--keyint 250`.

It also means **constant QP at 26**, because no rate-control flag was given.
That is the one part of the default that is not an x264 match: x264's bare
default is CRF 23. If you want constant quality you have to ask for it.

`--psy-rd` defaults to 2.0, a value this encoder swept for itself that does not
match x264. `--psy-trellis` defaults to 0.0, which is x264 medium's value.

## The option table

Defaults marked "preset" are set by the preset ladder and the bare default is
medium's row. See [Presets](#presets).

**A value outside an option's domain is refused, naming the domain.** A
non-numeric value and an out-of-range one both exit 2 with a message like
`yah264: --ref expects 1..2147483647 (got '0')`. Parsing is lenient about the
*form* of the number: `--bitrate 800.0` works, because that is what `str(float)`
produces. A non-integral value for an integer option is still refused instead of
truncated.

Enum-valued options refuse an unknown name the same way. A typo and x264's
unimplemented `--direct none` are both rejected instead of being silently read
as something else.

### Input and output

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--input-y4m` | path or `-` | required | Y4M input. No other input format exists. |
| `-o`, `--output` | path or `-` | `-` (stdout) | Annex-B output. |
| `--input-raw` | path or `-` | | Raw YUV. Needs `--input-res`. |
| `--input-res` | `WxH` | | Raw input geometry. Required with `--input-raw`, refused with `--input-y4m`. |
| `--input-csp` | `i420`\|`i422`\|`i444`, each with an optional `p10` | `i420` | Raw input chroma format **and sample depth**. A raw file has no Y4M `C` tag. That tag carries the depth beside the format, so the depth travels with the format here too: `--input-csp i420p10`. That is why there is no `--input-depth`. |
| `--fps` | `N`, `N/D` or a decimal | 25 | Raw input frame rate. `--fps 23.976` is read as 23976/1000. |
| `--input-range` | `full`\|`limited` | not signaled | VUI color range for raw input. It is the same field `--range` sets. |
| `--frames` | N | 0 = all | Stop after N input frames. |
| `--seek` | N | 0 | Drop the first N input frames. Seeks where the input allows it and reads-and-discards where it does not, so a pipe still works. |
| `--crop-rect` | `L,T,R,B` | off | Crop the input before encoding, in luma samples. The offsets must land on a chroma sample and the result must be even in both axes. Anything else is refused, not rounded. |
| `-v`, `--verbose` | | | Log level debug: adds one line naming what the command line resolved to. |
| `--quiet` | | | Log level warning: no informational lines. Warnings and errors still print. |
| `--log-level` | `error`\|`warning`\|`info`\|`debug` | `info` | The same dial. There is no `none`: an error prints at every level, because a run that fails in silence is worse than a noisy one. |
| `--no-progress` | | | No progress line. It only appears when stderr is a terminal, so a gate or a redirect never sees one either way. |
| `--output-depth` | 8 or 10 | the input's | Which of the two encoder libraries codes the stream. 10 on 8-bit input upshifts each sample by 2 and writes High 10. 8 on 10-bit input is refused, not rounded. |
| `--dump-recon` | path | off | Write the encoder's own reconstruction as Y4M, in display order, at the depth the encode ran at. **Forces the single-threaded path** (see below). An explicit `--threads` above 1 is warned about rather than dropped. |
| `--range` | `full` or `limited` | not signaled | VUI color range. The Y4M `XCOLORRANGE` tag sets it on its own. |
| `--colorprim`, `--transfer`, `--colormatrix` | H.273 code or a name (`bt709`, `bt2020`, `bt601`, `smpte170m`, `bt470bg`, `srgb`, `smpte2084`, `arib-std-b67`) | not signaled | VUI color description. Signaling only: nothing in the encoder changes with them. |
| `--chromaloc` | 0..5 | not signaled | VUI chroma sample location. |
| `--version` | | | Print version, exit. |
| `-h`, `--help` | | | Print usage, exit. `--longhelp` and `--fullhelp` print the same list, since it already names every flag. |

### Rate control

Covered properly in [rate-control.md](rate-control.md). The flags:

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `-q`, `--qp` | 0..51 | 26 | Constant QP. This is the mode you get if you name no other. |
| `-B`, `--bitrate` | kbit/s | off | Single-pass ABR average bitrate. |
| `--crf` | ~0..51 | off | Constant rate factor. Accepts a decimal but see the staircase note in the RC guide. |
| `--vbv-maxrate` | kbit/s | 0 = off | VBV peak rate. Needs `--vbv-bufsize` too. Either alone does nothing. |
| `--vbv-bufsize` | kbit | 0 = off | VBV buffer size. |
| `--vbv-init` | float | full | Initial VBV occupancy: a value at or below 1 is a fraction of `--vbv-bufsize`, above 1 is kbit. Only does anything where the VBV actually binds. |
| `--qpmin`, `--qpmax` | 0..51 | 0, 51 | Bounds on the coded QP the rate control may pick. Applied after the frame-type offsets, so they bound what is coded rather than the base QP the offsets came from. |
| `--crf-max` | 1..51 | off | Ceiling on the QP the VBV may raise a `--crf` frame to. Needs `--crf` and a VBV. Without either it is refused, because nothing else raises the QP for it to bound. It bounds the raise and not the mapping: a frame the rate factor already put above the ceiling keeps the QP it was given. The buffer is then allowed to under-run. That is the trade, not a defect. See below. |
| `--ratetol` | float, or `inf` | 1.0 | How far single-pass ABR may drift from its target before the correction answers, as a fraction of the target bitrate. Smaller tracks the rate closer and swings the quality more. `inf` takes the correction out and leaves the rate to the allocator. Under a VBV the buffer's own cap on the correction still applies. The default is today's ABR, byte for byte. `Y264_ABR_TOL` still overrides it. |
| `--qpstep` | 1..51 | 4 | Largest QP move between consecutive frames of one type. Reaches the single-pass ABR step clip and the two-pass allocator's. |
| `-p`, `--pass` | 1, 2 or 3 | off | Multi-pass: 1 writes stats, 2 reads them, 3 reads them **and writes them back**, so a further pass refines against a real encode instead of the fixed-QP pass 1. Pair with `--bitrate`. |
| `--stats` | path | `yah264.stats` | Two-pass statistics file. |
| `--aq-strength` | float | 0.4 rate-controlled, 0.0 at CQP | Variance adaptive quantization. 0 disables. |
| `--aq-mode` | 1 or 2 | 2 | The AQ form: 1 plain log2-variance, 2 adaptive (a per-frame gain from the spread of block energies, plus a raised variance floor and a frame-level center on frames with no mb-tree field). **`0` is refused**, unlike in x264. The value only selects a metric here and has no off seat: 0 would encode as 1. AQ off is `--aq-strength 0`. |
| `--abr-model` | `default`, `rf` | `default` | ABR bit allocation without mb-tree (`--bframes 0 --rc-lookahead 0`). With mb-tree, both use the rate-factor ledger. `rf` is also accepted under its older spelling `x264`. See below. |
| `--rc-lookahead` | frames | preset (40 at medium) | mb-tree propagation window. 0 turns the window off. |
| `--no-mbtree` | | mb-tree on | Skip mb-tree propagation entirely. Already the policy at constant QP, where x264 forces it too. |
| `--ipratio`, `--pbratio` | float | 1.4, 1.3 | **Two-pass only.** The I-to-P and P-to-B qscale factors of the offline allocator. They keep x264's names, numbers and defaults. The single-pass modes anchor I and B their own way and never read these. |
| `--cplxblur` | | 20 | **Two-pass only.** Width of the planner's complexity blur: a gaussian over neighboring frames' costs with sigma `--cplxblur`/2 frames. 0 turns it off. `--qblur` is accepted for compatibility and ignored, with a warning: the planner has no quantiser blur. |

### `--crf-max` buys quality with the buffer model

The option is a floor on quality bought with the thing that was protecting the
buffer. Under `--crf` with a VBV, a frame the buffer cannot afford has its QP
raised until the prediction fits. `--crf-max` stops that raise at a QP you name.
The frame is then coded at that QP. It does not fit, so the buffer goes
negative.

That is the whole option. It is intended, and nothing softens it. The stream is
no longer VBV-conformant at the declared buffer, and a receiver that enforces
the model will stall on those frames. Do not set it on a stream that has to be
conformant. Do not set it with `--nal-hrd` either, because the declaration in
the stream would then be false.

`scripts/vbv_check.py --crf-max Q` gives the true reading. It simulates the same
buffer and reports the under-run in kbit and as a percentage of the buffer. It
then exits 0 instead of failing the stream, because an under-run is the expected
outcome of a `--crf-max` encode. Without the flag the same stream reads
`UNDERFLOW` and exits 1. That is also correct. The two modes measure the same
thing and differ only in what was asked for. A gate that simply skipped the
checker on those cells would stop noticing the day the under-run stopped being
bounded.

The ceiling is read on the CODED QP after the frame-type cascade, where
`--qpmax` is read too. Every B frame steps over a ceiling on the base QP alone.
At `--crf-max 34` on foreman the P frames landed on 34 and the B frames on 37, a
bound nobody can see in the picture.

### `--abr-model rf` matters only without mb-tree

ABR with mb-tree, which runs whenever there are B frames or a lookahead (the
default), always uses the rate-factor ledger: P frames get a self-normalizing
rate factor, and I and B frames are anchored to the running non-B QP track.
`--abr-model` doesn't change that path. A one-thread foreman encode at
500 kbit/s is byte-identical under `default` and `rf`, with or without
`--no-mbtree`, because `--no-mbtree` skips propagation but leaves the ledger in
place.

The flag matters only at `--bframes 0 --rc-lookahead 0`, where there is no
mb-tree at all. There `default` falls back to the per-frame model, and `rf`
keeps a rate factor on the blurred complexity instead. The older measurements
that compared the two came from before the ledger was the default, and they
haven't been re-run on this path.

**`--qp`, `--bitrate` and `--crf` each name a mode. The last one on the command
line wins**, as in x264. A warning on stderr names what the loser still does:

```
yah264: warning: --crf 23 is dropped -- --bitrate 5000 came later and selects ABR (last rate-control flag wins, as in x264)
yah264: warning: --qp 26 still sets only the base QP -- --crf 26 came later and selects CRF (last rate-control flag wins, as in x264)
```

The two messages differ because `--qp` is not thrown away: `rc.qp` seeds the
encoder's base QP whatever the mode. `--crf` and `--bitrate` really are
discarded.

So `--bitrate X --crf Y` is CRF. `--crf Y --qp Z` and `--bitrate X --qp Z` are
constant QP. Every one of them prints the warning above, so the bits move only
where you are told they moved. Repeating the *same* flag is not a clash and says
nothing.

`--pass` is not in the contest. It is a mode plus a stats round-trip whose
target is `--bitrate`. Two-pass CRF is not implemented, so a `--crf` passed
alongside it is dropped with a warning.

`--aq-strength`'s default is a two-way branch: **0.4** under CRF/ABR/two-pass,
0.0 under constant QP. An explicit value, including an explicit `0`, always
wins. 0.4 is this encoder's own measured value and differs from x264 medium's
1.0. A seven-clip VMAF-NEG sweep put the optimum near 0.3, and the shipped
default sits just above it. Why it lands so far below the reference's default is an open question.

### GOP structure

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `-I`, `--keyint` | N | 250 | Maximum frames between IDRs. |
| `-i`, `--min-keyint` | N or `auto` | 0 = auto (`keyint/10`) | A scene cut closer than this to the last keyframe is not promoted to an IDR. Clamped internally to `[1, keyint/2+1]`. |
| `--scenecut` | N | 40 | Adaptive-keyframe aggressiveness; higher inserts more. `0` or any negative turns it off. |
| `--no-scenecut` | | | Same as `--scenecut 0`. Only `--keyint` places IDRs. |
| `--open-gop` | | off | The periodic keyframe is a non-IDR I picture with a recovery_point SEI instead of an IDR: the B frames before it keep referencing the anchor behind it, the DPB is not flushed and POC runs on. See the note below. |
| `--intra-refresh` | | off | No keyframe after the first: a column of intra macroblocks sweeps the picture once every `--keyint` frames, and each sweep opens with a recovery_point SEI. P frames only. See the note below. |
| `-b`, `--bframes` | N | preset (3) | Consecutive B frames between anchors. 0 disables B entirely. |
| `--b-adapt` | N | 1 | Adaptive B placement. 0 codes a fixed cadence. |
| `--b-bias` | -100..100 | 0 | Leans the adaptive B placement toward more B frames (positive) or fewer (negative). A tuning dial, not a quality setting: see [`--b-bias`](#--b-bias-is-a-tuning-dial) below. Ignored at `--b-adapt 0`. |
| `--direct` | `auto`\|`spatial`\|`temporal` | `auto` | B direct MV derivation. `auto` lets each B slice pick by the running skippability score (since 2026-09-03). |
| `-r`, `--ref` | N | preset (3) | P-frame list-0 reference count, clamped to 16. |

`--bframes 2` or higher enables the B-pyramid, which changes `max_num_ref_frames`
and therefore the auto-selected level.

**`--bframes 0` also turns mb-tree off unless the lookahead window is on.**
mb-tree runs when `bframes > 0`, or when `rc-lookahead > 0` and the IPPP path is
enabled (it is by default). So `--bframes 0 --rc-lookahead 0` changes more than
the frame types: it is a different rate-control workload from the default.

#### `--open-gop`

Without it, every keyframe is an IDR. The buffer of decoded pictures is thrown
away and the picture order count restarts. The B frames that sit before the key
in display order cannot see across it, so they are coded as non-reference P
frames against the past alone. That is a closed GOP, and it costs those frames
their forward prediction.

With `--open-gop` the periodic keyframe is an I picture that is **not** an IDR.
It carries a recovery_point SEI (`recovery_frame_cnt` 0, `exact_match_flag` 1).
The decoded picture buffer survives it, and POC and FrameNum run straight
through. The frames before it in display order stay B frames that reference
backward to the anchor behind the key and forward to the key itself. The first
picture of an encode is still an IDR, and so is a keyframe a `--plan` zone asked
for, because the engine interface promises a plan keyframe is addressable as a
segment on its own.

**Every other keyframe becomes an open one, including the adaptive scene-cut
keys** as well as the `--keyint` cadence. That matters because at the default
`--keyint 250` the cadence rarely fires at all. A 120-frame clip has one
keyframe, its first, and that one is an IDR either way. A cut is the only key
most short encodes have, so it is where the whole gain of the flag shows up on
them. Measured over the twelve band clips at 120 frames and the default keyint,
nine are byte-identical with the flag and without it. The three whose content
cuts move by 0.4% (sintel_720p), 6-7% (coastguard_cif) and 11% (samsung_720p) of
the bytes at the same CRF. If you want a cut to stay a hard boundary, a segment
a packager can address on its own, use `--cut-split`. It gives each shot its own
encoder instance, and the cut stays an IDR.

What the recovery point promises is exact and narrow, and the encoder is built
to keep it. Start decoding at that access unit with nothing before it but the
parameter sets, and **every picture from the key onward in output order is the
picture a decode from the stream's IDR would have produced.** The leading B
frames are output before the recovery point and are not covered: a decoder that
started there discards them. Two things follow inside the encoder. No picture
after the key may reference one from before it, so the reference lists are cut
at the key. The leading B frames are coded as non-reference pictures, because a
reference among them would head the default list of everything after the key
while holding the wrong picture on a cold start. `scripts/recovery_check.py` is
the test, and `make conformance` runs it. It asks the decoder for **every**
picture the decoding process produced and makes the selection itself out of the
tail. Which pictures a decoder hands to a display after a mid-stream start is
its own policy, and decoders differ about it on the same bytes. One suppresses
the uncovered leading B frames. Another suppresses those and then stops two
short of the end of the bitstream. Neither reading moves a sample, so the
checker takes all of them and counts back from the last.

The cost is parallelism, not bits. A GOP boundary is what lets the CLI hand a
whole GOP to its own encoder instance. An open GOP has none, so the encode is
one instance from end to end and `--threads` spends its budget on the row
wavefront instead of on parallel GOPs. With `--cut-split` the scene cuts still
split, because those really are IDRs. On an input whose frame count cannot be
read (a pipe), the schedule cannot be planned at all and the encode falls back
to the serial path. It says so on stderr when `--threads` asked for more.
`--open-gop` is refused with `--stitchable`, because that flag promises a cut
point at every keyframe.

#### `--intra-refresh`

A keyframe is the biggest picture in a stream, often by an order of magnitude.
On a low-latency link that size arrives as a burst the buffer has to absorb.
`--intra-refresh` removes keyframes after the first one. Instead, each P frame
codes a share of the picture's macroblock columns as intra, left to right. A
full sweep refreshes every column once, and the cost spreads evenly over the
period.

- **The period is `--keyint`.** Sweep position *j* (0 to keyint-1) codes the
  columns from `j*W/keyint` up to `(j+1)*W/keyint`, where W is the picture's
  width in macroblocks. When keyint is larger than W, some frames refresh no
  column at all. The first sweep starts on the frame after the opening IDR.
- **Each sweep opens with a recovery_point SEI** on its first frame, with
  `recovery_frame_cnt` = keyint - 1 and `exact_match_flag` 1. A decoder that
  starts at that frame, with nothing before it but the parameter sets, has the
  encoder's own reconstruction from the sweep's last frame onward. FrameNum
  widens as needed to hold the count, so keyint can be at most 65536.
- **Motion never reads what the sweep hasn't reached.** A macroblock left of the
  refresh column predicts only from references inside the current sweep, and
  only from the columns those references had already refreshed. Each block's
  reads stop 3 luma samples short of a reference's refreshed edge, because the
  deblocking filter rewrites that many refreshed samples from the unrefreshed
  side. A fractional vector stops 3 samples earlier still, for the 6-tap
  interpolation. The bound is per block, so the left half of a macroblock beside
  the edge can still use a vector its right half can't. The skip vector and the
  quarter-pel refinement are held to the same bound. In the last refreshed
  column, the intra prediction modes that read the above-right neighbor are not
  used, because that neighbor hasn't been refreshed yet.
- **No picture references anything older than the frame before its sweep
  began.** That frame is the last one the previous sweep promised exact, so a
  decoder that joined one sweep earlier stays exact through this one.
- **Scene cuts don't place keyframes and don't restart the sweep.** The sweep
  in progress has already promised an exact picture at its end, and a restart
  would have to keep that promise as well as open a new one. `--scenecut` is
  ignored with a warning. A keyframe the caller forces (a `--plan` or
  `--qpfile` IDR) is still an IDR, and the next sweep starts after it.
- **P frames only.** B frames are refused when named with `--bframes`, and a
  preset's own B frames are dropped with a note on stderr. Direct prediction
  reads the co-located picture's motion, which a decoder that joined
  mid-sweep never decoded.
- **One encoder instance.** With no keyframes there are no GOP boundaries to
  split the encode at, so `--threads` runs the row wavefront and the lookahead
  inside one instance instead of parallel GOPs. The CLI says so on stderr
  when it has more than one thread to spend.
- **Refused** with `--open-gop`, `--cut-split`, `--shot-table`, `--shot-crf`,
  `--hw`, `--stitchable` and field coding (`--tff`/`--bff`).

Rate control and VBV run unchanged. `scripts/recovery_check.py` tests the
promise: it reads `recovery_frame_cnt` out of the SEI, starts a decode at the
recovery point and compares every frame from the end of the sweep onward with
the reconstruction. `make conformance` runs it at every recovery point of
several streams, beside the recon-match cells.

**What it costs.** Measured against plain keyframes at the same period
(`--keyint 60`, `--no-scenecut` on the keyframe side), both at `--bframes 0`
with a one-second VBV (`--bitrate R --vbv-maxrate R --vbv-bufsize R`), 180
frames, `--threads 1`, four rates from half to one and a half times the board
rate. BD-rate is on VMAF-NEG, where positive means intra refresh needs more bits
for the same quality. The peak is the largest frame after the opening IDR over
the mean frame size, at the board rate.

| Clip | Board rate | BD-rate (VMAF-NEG) | Peak, intra refresh | Peak, keyframes |
| --- | --- | --- | --- | --- |
| foreman_cif | 400 | +12.6% | 2.5x | 6.6x |
| bus_cif | 400 | +4.6% | 2.1x | 6.7x |
| fourpeople_720p | 1200 | +51.2% | 3.1x | 19.2x |
| shields_720p | 2200 | +10.2% | 3.4x | 13.8x |
| park_joy_720p | 12000 | +4.8% | 1.4x | 5.0x |

The peak is the point: the largest frame drops by a factor of 3 to 6. The bits
it costs depend on how static the picture is. On moving content it's about 5%.
On a static scene (fourpeople, a video call) it's large. A keyframe there is
coded once at a low QP and then skipped through, while a refresh column is coded
at the P frames' QP. The column beside the sweep's edge also can't predict its
last 4 luma columns from where they were. In CRF on fourpeople the cost is +42%.
With the motion bound removed (which breaks recovery) it is still +27%, so most
of it is the intra columns themselves. A lower QP on the refresh column made it
worse at both offsets tried (3 and 6).

#### `--b-bias` is a tuning dial

The adaptive placement keeps a candidate frame as a B while its lowres cost
stays under 0.77 of its intra cost. That cost is, per block, the cheapest of
the forward leg, the backward leg and intra. Otherwise the frame becomes an
anchor. `--b-bias` moves that 0.77 in a straight line: up to
1.0 at +100, where a frame becomes an anchor only if nothing predicts it
better than intra, and down to 0 at -100, where every frame is an anchor. The
B-run limit (`--bframes`), the key-frame interval and scene cuts don't move. At
0 the threshold is untouched and the output is byte-identical to an encode
without the flag.

Measured on four board clips, 120 frames, `--threads 1`, CRF 23 to 35 in five
rungs, BD-rate on VMAF-NEG against `--b-bias 0`. The B share is read at CRF 29.

| clip | B share at 0 | at -50 | BD at -50 | at +50 and +100 |
|---|--:|--:|--:|---|
| stefan_cif | 73% | 26% | +14.3% | byte-identical to 0 |
| shields_720p | 74% | 1% | +54.2% | byte-identical to 0 |
| park_joy_720p | 74% | 0% | +30.8% | byte-identical to 0 |
| pedestrian_1080p | 74% | 0% | +11.3% | byte-identical to 0 |

shields' top rung sits at VMAF-NEG 98, so its number is the least exact of the
four. Two things follow. The positive side does nothing on this content: the
default already fills every B run to `--bframes` on all four clips, so there
is no anchor left for a looser test to turn back into a B. The same holds at
`--bframes 16` on stefan and shields. The negative side is steep. -10 already
takes stefan from 77 to 71 B frames at `--bframes 16`, -25 to 48, and -50
removes nearly all of them, at a cost of 11 to 54%. Use it to trade B frames
away for decode simplicity or latency, in small steps, and expect it to cost
bits. It is not a quality setting, and nothing here suggests moving it from 0.

### Coding tools

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--preset` | name | `medium` | Speed/quality ladder. See below. |
| `--tune` | name | off | Content tuning. See below. |
| `--cabac` / `--cavlc` | | CABAC (preset) | Entropy coder. `ultrafast` sets CAVLC. |
| `--transform-8x8` / `--no-transform-8x8` | | on (preset) | 8x8 transform and 8x8 intra. On means High profile. |
| `--cqm` | `flat`\|`jvt` | `flat` | Quantization matrices. `jvt` writes the standard's default scaling lists into the SPS and forces High profile. |
| `--cqmfile` | file | off | Custom quantization matrices from a file in the JM text format: `INTRA4X4_LUMA = w,w,...`, and likewise `INTRA4X4_CHROMAU`/`CHROMAV`, the three `INTER4X4_*` lists, `INTRA8X8_LUMA` and `INTER8X8_LUMA`. Weights are 1..255 in raster order (row by row), separated by commas or whitespace, and may span lines. `#` starts a comment. A luma list the file leaves out is flat, a missing Cb list takes that mode's luma list, and a missing Cr list takes Cb. Every list is written into the SPS, so any decoder reproduces them. The quantizer, the dequantizer and the trellis all read them. Forces High profile. Overrides every other `--cqm*` option. The 4:4:4 chroma 8x8 keys are accepted and unused: 4:4:4 never picks the 8x8 transform here. |
| `--cqm4`, `--cqm8` | 16 or 64 weights | flat | Set every 4x4 list, or both 8x8 lists, to one comma-separated list of weights 1..255 in raster order. |
| `--cqm4i`, `--cqm4p`, `--cqm8i`, `--cqm8p` | weights | flat | The intra or the inter lists, luma and chroma together. |
| `--cqm4iy`, `--cqm4ic`, `--cqm4py`, `--cqm4pc`, `--cqm8iy`, `--cqm8py` | weights | flat | One list each: `y` luma, `c` both chroma planes. The per-list options build a custom set starting from flat, so any list you do not name stays 16. They refuse to combine with `--cqm jvt`. A set that comes out flat in every list writes no scaling matrix at all and is byte-identical to `--cqm flat`. x264's `--cqm8ic`/`--cqm8pc` (4:4:4 chroma 8x8) have no seat, for the same reason as above. |
| `-f`, `--deblock` | `A:B` | `-1:-1` when the longer side is 1280 or more, `0:0` below; `0:0` with psy off | In-loop deblocking filter offsets, each -6..6. These are the slice header's own div2 values, so x264's numbers port unchanged and the offset the decoder applies is twice what you type. |
| `--no-deblock` | | filter on | No in-loop deblocking at all: `disable_deblocking_filter_idc 1`, and no filter runs. |
| `--b-pyramid` | `none`\|`normal` | `normal` | `none` codes a flat B run instead of a hierarchy. `strict` is not implemented and is refused rather than read as `normal`. |
| `--no-weightb` / `--weightb` | | on | Clears (or restores) `weighted_bipred_idc`, so B slices use plain averaging instead of implicit weights. |
| `--weightp` | 0\|1\|2 | 1 | Explicit weighted prediction on P slices. **0** clears `weighted_pred_flag`: no `pred_weight_table` in any P slice header, and the weight leaves the prediction and the search alike. It is the one flag that can LOWER the declared profile. Annex A.2.1 forbids the tool in Baseline and nothing else here asserted it, so a CAVLC stream with no B frames and no 8x8 transform declares Baseline at `--weightp 0` and Main at 1. **1** is the default and is what every encode before this flag did. It codes one luma weight and offset per active list-0 reference. The weight is estimated from the frame's DC ratio against that reference, because that ratio is what a fade looks like from outside. Chroma weights stay identity. **2** adds a second list-0 slot carrying that weight while the reference's own slot stays plain, so the mode decision picks weighted or plain per macroblock instead of per frame. It fires only where the frame-level estimate already did, and only where the weight wins on most of the picture but not all of it. It needs a slot to put the duplicate in, so it is **refused at `--ref 1`**. It is also refused with `--tff`/`--bff`. It is OFF by default. It costs +0.29% BD-VMAF-NEG on a CIF fade clip, and on ten of the twelve HD band clips it is inert and byte-identical to `--weightp 1`. [plan.md](plan.md) has the measurement. |
| `--slices` | N | 1 | Cut each picture into N independently decodable slices on macroblock-row boundaries. Nothing crosses a slice's first row: not the intra reference samples, not the mode or motion-vector predictors, not the CAVLC nC or the CABAC contexts, not the `mb_qp_delta` chain. Each slice is its own NAL. A decoder can resynchronize at any of them and decode several at once. **The in-loop filter is not cut**: every slice header writes `disable_deblocking_filter_idc` 0, so the picture is deblocked whole and the slice edges do not show. It costs bits (see below). Clamped to the picture's macroblock row count, and to 256. Refused with `--hw`. |
| `--constrained-intra` | | off | Sets the PPS `constrained_intra_pred_flag`. Intra prediction in a P or B slice then treats an inter-coded neighbor as unavailable, for the reference samples and for the intra mode predictor alike. An intra macroblock then decodes from intra data alone. It buys error resilience, and the price is bits. The prediction has less to work with, and I_16x16 plane and the corner-reading 4x4/8x8 modes drop out wherever the above-left neighbor is inter. No effect on I slices, where every neighbor is intra already. |
| `--chroma-qp-offset` | -12..12 | by the longer side: -4 below 1280, -2 below 1920, 0 from 1920 up; 0 with psy off | PPS `chroma_qp_index_offset`. Reaches the chroma quantizer **and** the deblock filter's chroma edge QP, as the spec requires. Written to `second_chroma_qp_index_offset` too. |
| `--mvrange` | luma samples | the level's | Vertical motion-vector range. The default is the level's own Table A-1 bound. Only a value **tighter** than the level's has any effect, because a level is a conformance bound and not a suggestion. The SPS's `log2_max_mv_length_vertical` follows whichever bound is in force, rounded up to the next power of two so the declaration is never narrower than a vector the search may return. |
| `--me` | `dia`\|`hex`\|`umh`\|`esa`\|`tesa` | auto from preset | Motion search. Auto is hex at medium and faster, UMH at slow and above. `esa` scores every whole-pel vector within `--merange` of the best starting vector (zero, the predicted vector or a neighbor's) by SAD plus vector cost and keeps the cheapest. `tesa` runs the same scan, then re-ranks its 16 cheapest candidates by SATD (Hadamard) plus vector cost and keeps the best of those. Both replace the pattern search for every partition shape and reference. The sub-pel stages after them are unchanged. Slow. The cost against `umh` is below the table. |
| `--no-mixed-refs` | | mixed | One list-0 reference per P macroblock: the 16x8, 8x16 and 8x8 partitions search only the reference the 16x16 search chose, instead of each picking its own. Cheaper with several references, and it costs bits. B macroblocks already take the 16x16's list-0 reference for their rectangles and quadrants, so it changes P macroblocks only. |
| `--no-chroma-me` | | | Accepted and changes nothing: the motion search here scores luma only, so there is no chroma term to drop. |
| `--psy-rd` | float | 2.0 | Psychovisual RD strength. 0 disables. |
| `--psy-trellis` | float | 0.0 | Psy-trellis strength. Around 1.0 for grain. |
| `-t`, `--trellis` | 0..2 | 1 | RDOQ placement, x264's scale: 0 off, 1 the committed macroblock only, 2 every mode decision. |
| `-A`, `--partitions` | list | preset (see below) | Which macroblock partition shapes the mode decision may try, comma-separated from `p8x8`, `p4x4`, `b8x8`, `i8x8`, `i4x4`, plus the two words `none` and `all`. A shape outside the list is never searched and never coded, so the macroblock falls back to its whole form: P and B stay 16x16, intra stays I_16x16. `none` and `all` replace the list instead of adding to it, so `p8x8,none` is the empty set. `p4x4` needs `p8x8`, and `i8x8` needs `--transform-8x8`. Either one missing is an error that names the rule, not a silent drop. See below. |
| `--p-part-gate` | lambdas | 400 | Refuse the P 16x8, 8x16 and 8x8 searches when the 16x16 result already costs less than this many lambdas **and** the neighborhood is homogeneous and nothing downstream leans on the block. `--no-p-part-gate` is 0. Inert at `--subme` 9 and above, which run the exhaustive tournament. See below. |
| `--b-preme-skip` | 0..4 | 0 (off) | End a B macroblock at skip **before** its motion search when the skip candidate's own distortion is already under the cheapest rate any coded mode could pay. 0 off, 1 non-reference B slices, 2 also reference B's a propagation guard admits. 3 and 4 are the same two with the bound read as an absolute distortion instead of in lambdas. Off, because the largest speed prize left costs up to 2.3% BD on a detailed 1080p clip. Inert at `--subme` 9 and above. See below. |
| `--rd-surv-rank` | count | 0 (off) | RD at most this many candidates per set in the B tournament, ranked by screening cost. 0 keeps the score threshold alone. Off, with the number below. |
| `--lr-settle` | SAD/pixel | preset (0 at medium, 1 below it) | End the lookahead's lowres block search at its own predictor when that predictor already leaves under this much SAD per lowres pixel. The search is the inner loop of both the lookahead's motion field and the macroblock tree's walk, so the threshold reaches the whole lookahead. `--no-lr-settle` is 0. Off at medium and above, where it misses the bar. On at `veryfast` and below, and inert at `superfast` and `ultrafast`, which run no lookahead window. See below. |
| `--lr-subgate` | SATD/pixel | 0 (off) | Skip the lowres subpel refine when the whole-pel winner is already under this much SATD per lowres pixel. Off, with the number below. |
| `--mbt-depfloor` | 256ths | 0 (off) | Refuse the macroblock tree's deposit where the block's own propagation fraction is under this many 256ths. Off, with the number below. |
| `--b-intra-band` | 16ths | 0 (off) | Skip the B intra SATD screen **and** the intra trial in a row band whose lookahead intra cost is more than this many 16ths of its lookahead inter cost. The verdict is per band, taken once per frame from fields the lookahead already built, instead of per macroblock. Off, with the number below. |
| `--b8-band` | CoV^2 x100 | 0 (off) | Decline the B_8x8 quadrant gate and its eight motion searches in a row band whose lookahead pair-leg cost field has a squared coefficient of variation under this many hundredths. Off, with the number below. |
| `-m`, `--subme` | 1..11 | preset (7 at medium) | Subpel/RD analysis level, x264's scale. See below. |
| `--subpel` | 0..2 | preset (2 at medium) | Refinement *pattern*: 0 square, 1 diamond, 2 capped diamond. No x264 equivalent. |
| `--merange` | pels | 16 | Search radius for `umh`, `esa` and `tesa`, x264's `--merange`. `dia` and `hex` ignore it, so it does nothing at medium. |
| `--qcomp` | 0..1 | 0.6 | Rate-curve compression, x264's `--qcomp`. See the divergence below. |
| `--cut-split` | off | File input only: put an IDR on every scene cut the pre-scan finds and split the GOP workers there, instead of at arithmetic keyint boundaries. Clean seek points at the cuts, and each shot is coded as its own unit. From a file the pre-scan is a pass of its own. It reads the file once for the scan, keeping one byte per frame, then rewinds and encodes through the ordinary bounded window. It needs about the memory a plain encode does (see [Memory](#memory-the-parallel-path-streams-through-a-bounded-window)). A pipe cannot be read twice, so there the input is held in memory until it reaches the limit, where the encode is refused. |
| `--shot-table` | off | Print the pre-scan's shot table as JSON on stderr (first/last frame, mean and peak lowres intra cost, mean inter cost, ratio). Implies `--cut-split`. |
| `--shot-crf` | off | Per-shot CRF from the shot table: `crf + 6(1-qcomp) log2(C_shot / C_title)`, where C is the shot's mean lowres inter cost, clamped to +-4 QP. Shots under 24 frames merge into their predecessor, and each GOP takes its shot's CRF. Implies `--cut-split`. On ten real titles at 1080p (three 2-minute segments, seven 10-minute excerpts) it reads a median -1.0% BD-rate on VMAF-NEG (7 of 10 better) and -5.3% on PSNR-Y (all 10 better) against plain CRF. On the short multi-shot sequences it reads -1 to -4% against the default and -6 to -11% against `--cut-split` alone. Ignored by `--pass 1`. |
| `--scan-out FILE` | off | With `--cut-split` or `--shot-crf`: also save the pre-scan's result (the IDR map and the shot table) to FILE, so a later encode of the same input can skip the pre-scan. See [the saved pre-scan](#the-saved-pre-scan---scan-out-and---scan-in). |
| `--scan-in FILE` | off | With `--cut-split` or `--shot-crf`: use a saved pre-scan instead of running one. Refused, never applied, unless the frame count, the input's geometry, the scan's settings and a hash of three frames' luma all match this encode. Output is byte-identical to running the scan. |
| `--plan FILE` | off | Zones, one per line: `first last [idr] [qp+N\|qp-N]`, frames from 0 in input order, inclusive. `idr` forces a keyframe and a GOP boundary at `first`. The QP offset applies to every frame in the range on top of the rate control (CRF, CQP, ABR). The engine interface's plan (docs/engine-interface.md). |
| `--zones LIST` | off | x264's inline zones: `start,end,option` joined by `/`, frames from 0 in input order, inclusive. `q=N` codes every frame in the range at base QP N, 0..51, the way `--qp N` would: I frames take N-3 and B frames the cascade on top, and neither the rate control nor the VBV moves it. `b=F` multiplies the range's bitrate by F, applied as a QP offset of -6 log2(F) on what the rate control chose, so `b=0.5` is `qp+6` in a `--plan` file. One option per zone. Anything else, overlapping zones and `--plan` on the same command line are refused with a message. |
| `--qpfile FILE` | off | Per-frame forces, one line per frame: `<frame> <type> <qp>`, frames from 0 in input order. Type `I` or `K` forces an IDR, `P` an anchor, `B` or `b` a B frame, `-` leaves the type alone. QP is **absolute** 0..51, or `-1` to leave it to the rate control, so a line can force a type, a QP, or both. Frames the file does not name are free. A force is refused by frame number rather than approximated. See below. |
| `--gop-threads K` | off | Pin every GOP instance's frame threads to K, so a GOP re-encoded alone reproduces its bytes (`scripts/shot_determinism.sh`). |
| `--segment-out PATTERN` | off | Also write each GOP to its own file (`%d` = GOP index), each with its own parameter sets. Concatenated in order they equal the stream. |
| `--frame-stats FILE` | off | One JSON line per coded frame in coding order: frame, gop, type, idr, ref, qp, bytes, k. |
| `--deadzone-inter` | 0..32 | 21 | Inter luma quantization deadzone, x264's flag and x264's value. |
| `--deadzone-intra` | 0..32 | 11 | Intra luma quantization deadzone. |
| `--no-psy` | | psy on | `--psy-rd 0 --psy-trellis 0`, which is how x264 spells it. An explicit `--psy-rd` anywhere on the line still wins, exactly as it wins over a `--tune`. |
| `--nr` / `--nr-intra` / `--nr-inter` | 0..2000 | 0 (off) | Coefficient noise reduction: an offset toward zero, learned per transform position from the running coefficient energy, taken off every coefficient before quantization. `--nr` sets both block classes; the other two set one each. Pictures that read clean on a spatial noise gate get no offset. `--nr-inter 100` is the measured setting: neutral to better on the HD band, best on noisy live action. It stays off because it does not help on film grain. [denoise.md](denoise.md) has the numbers. |
| `--mcstf` / `--no-mcstf` | | off | Motion-compensated temporal pre-filter on the source, in front of the lookahead. Every fourth picture, the anchor spacing at `--bframes 3`, is replaced by a weighted average of itself and its motion-compensated neighbors, so everything downstream sees the filtered picture. A temporal-noise gate skips pictures that read clean, which keeps animation and other clean content byte-identical. Up to -2.8% BD-VMAF-NEG on grain excerpts and -17% on touchdown, but foreman_cif reads +1.2 and it costs 10-13% of the instructions, so it stays opt-in for grain and sensor noise. [denoise.md](denoise.md) has the numbers. |
| `--mcstf-range` | 1..4 | 1 | Neighbors per side that `--mcstf` averages over. 1 is the measured setting. |
| `--no-dct-decimate` | | decimation on | Never drop a block whose coefficients are all marginal. |
| `--no-fast-pskip` | | fast P-skip on | Drop the cheap P_Skip pre-test, so every P macroblock takes the full analysis path. Only reachable at `--subme` 8 and below, where that test runs. Slower, and it moves bits. |
| `--no-asm` | | asm on | Force every scalar C path. Byte-identical output: every kernel is checkasm-equal to its C reference. |

### What custom matrices cost

A matrix is a quality choice, not a speed one. The default lists are tuned for
neither this encoder's rate control nor VMAF. `--cqm jvt` against flat on five
board clips, 120 frames, `--preset medium`, CRF 23-35, one thread, BD-rate on
VMAF-NEG (negative is better):

| clip | BD-VMAF-NEG | CPU time |
| --- | --- | --- |
| foreman_cif | +7.39% | 1.32x |
| bus_cif | +0.59% | 1.25x |
| stefan_cif | +0.38% | 1.21x |
| park_joy_720p | -0.91% | 1.20x |
| pedestrian_1080p | +19.51% (4 of 5 rungs) | 1.42x |

The CPU cost is the quantizer leaving its SIMD path: the kernels only take the
flat lists, and any non-flat list runs the C weighted path.

### What the exhaustive searches cost

`esa` and `tesa` against `--me umh` on the same five clips and ladder
(negative is better), with the CPU time of one 120-frame encode at CRF 26:

| clip | esa BD-VMAF-NEG | esa CPU | tesa BD-VMAF-NEG | tesa CPU |
| --- | --- | --- | --- | --- |
| foreman_cif | -2.74% | 4.6x | -1.81% | 7.7x |
| bus_cif | -0.42% | 4.3x | -0.19% | 7.4x |
| stefan_cif | -0.15% | 4.4x | -1.00% | 7.9x |
| park_joy_720p | +1.43% | 3.8x | +1.93% | 6.2x |
| pedestrian_1080p | +3.67% | 2.9x | +4.30% | 4.5x |

Exhaustive does not mean better here. The SAD-cheapest vector in the window is
not the one that codes best. On the two HD clips the pattern search's smoother
field wins. They are there for x264 compatibility and for experiments, not as a
quality setting.

### What `--partitions` gates

There is one name per shape family. Each one gates a search, not a syntax
element:

| Name | What the mode decision may try |
| --- | --- |
| `p8x8` | The P 16x8, 8x16 and 8x8 splits. Without it every P macroblock is 16x16 or skipped. |
| `p4x4` | The 8x4, 8x8 and 4x4 sub-splits inside a P 8x8 block. Needs `p8x8`: these shapes divide an 8x8 block, and without the 8x8 split there is no block to divide. |
| `b8x8` | The B 16x8, 8x16 and 8x8 splits, each quadrant with its own list choice. Without it every B macroblock is 16x16, bidirectional or direct. |
| `i8x8` | 8x8 intra prediction. Needs `--transform-8x8`: the 8x8 transform is what carries the residual, and nothing else can. |
| `i4x4` | 4x4 intra prediction. Without it, and without `i8x8`, intra is I_16x16 only. |

Dropping a shape does not make the encode smaller. It makes the search cheaper
and the prediction worse, and the residual pays for it. `none` is the extreme:
every macroblock is coded whole. That makes a useful floor to measure against
but not a setting to ship.

Slow and faster (subme 8 and below) search no B 16x8 or 8x16, so there
`--partitions b8x8` gets the quadrant split; `--subme 9` and up (slower,
veryslow, placebo) search both orientations too.

### The three low-rate decision gates

At low bitrate most of a picture is a skip. Most of the tournament is spent
confirming that, not deciding it. On a 1080p clip at the rate it ships at, 38%
of the B macroblocks that end as skip have run a full motion search first. These
three flags each end one of those searches earlier. Each is a speed-for-bits
trade, so each is read against BD-VMAF-NEG at matched achieved bitrate on a band
of twelve 720p and 1080p clips. One of them holds its quality on every clip of
that band. It is the one that is on.

`--p-part-gate` is that one. Where the 16x16 search has already found a cheap
answer, none of the splits is searched at all. Cheap is measured in lambdas, so
the threshold follows the operating point instead of the content. Two interlocks
read the lookahead's own fields before the gate may fire. First, the local 3x3
lowres-cost field must not be dispersed. Dispersion marks a motion or texture
boundary, which is exactly where one vector for the whole macroblock is the call
not to trust. Second, the mb-tree offset must not say that other frames lean on
this one. The gate saturates against those interlocks: raising the threshold
from 400 to 1200 buys almost nothing. That is the same fact as its quality
holding.

`--b-preme-skip` is off, and it is the expensive refusal. A coded macroblock
pays at least its own mb_type and cbp syntax, so no coded mode can score below
lambda times that rate. Where the skip candidate's distortion already sits under
that floor, the verdict is settled. The motion search, the RD trials and the
subpartitions after it only confirm it. Modes 1 and 2 differ in whether
reference B slices may use it, because a reference picture's errors travel into
the pictures that read it.

What refuses it is the shape of the bound, not the idea. A distortion measured
in lambdas widens as lambda grows. Lambda grows as the rate falls, so the gate
is most generous exactly where a wrong skip costs most. On the band's hardest
1080p clip it takes nothing at the top of the ladder and 2.7 VMAF-NEG points at
the bottom. Modes 3 and 4 read the same distortion as an absolute bound, which
cannot widen. They move the damage between clips without removing it. Both stay
reachable with their numbers recorded.

`--rd-surv-rank` is off too. It RDs only the top-ranked candidates of each B set
instead of everything the score threshold admits. It costs +0.63% of band
median. Cutting the list by rank bounds what cutting it by score could buy, so
that number closes both directions at once.

### The lookahead's own fixed cost

The lookahead costs the same whether the residual is cheap or not. At
low-bitrate HD that is where a large share of the encode goes. Deleting it
outright, the window and the tree together, takes 14.8% of the instructions off
one of the two profiled 1080p cells at matched bytes. The reference encoder pays
0.6% for the same deletion. Its lookahead earns its keep downstream, and ours
did not entirely.

`--lr-settle` is what came back from that. It is off at medium and on below it.
The lowres block search starts at a predictor its already-searched neighbors and
the previous field agree on. Where that predictor leaves almost no residual, the
candidate list, the hexagon, the square refine and the subpel diamonds are all
bought for a block whose answer was in hand before any of them ran. Deleting
them is worth 3.5% of instructions at a threshold of 2 and 1.2% at a threshold
of 1. The threshold saturates above 2.

It is the best band any candidate of this stage or the last one has produced. At
1 the median is -0.12%, the mean **-0.21%**, and ten of twelve clips are ahead
of or level with the default. It still costs +0.51% on one detailed 1080p clip.
The bar is read per clip, because half a corpus can sit under a median. A
near-inert control arm reads -0.09% to +0.13% on the same clips, so that +0.51%
comes from the arm and not from the band. Shifting the ladder keeps its sign. It
is refused at medium on that clip.

Below medium the same threshold ships where that tier's own band admits it. The
bar there is a different number, and the arm is read against that preset's own
baseline instead of medium's. At `veryfast` the worst clip is +0.24% and the
median -0.04%, inside the +0.5% the tier allows, so it is on. At `faster` the
band re-read at shared rungs gives a worst clip of +0.46% raw and +0.54%
referenced to the controls, with a median of +0.12%. The owner took it. At
`fast` the worst clip is +0.98%, so it refuses. `superfast` and `ultrafast` run
no lookahead window and are byte-identical either way.

The same band also shows how little the instrument can resolve at these tiers. A
deliberately near-null control arm reads a worst clip of +0.61% at `fast`,
against the +/-0.15 it reads at medium. Two independent controls reproduce each
other clip by clip, so those are systematic per-clip offsets and not scatter.
Every arm is read with the control's own number on the same clip beside it.

One constraint makes a cheaper lowres search hard instead of free. Taking the
field down to the plain from-zero diamond reads 9.4% MORE instructions at
matched bytes on both cells: a worse field costs more downstream than the search
saves. So the saving has to come out of blocks that were never in question, and
anything coarser pays twice.

`--lr-subgate` is off too. It skips the subpel refine on a whole-pel winner that
is already cheap. That is the same population `--lr-settle` reaches by a
different door, so on top of the settle exit it adds two tenths of a percent of
instructions. Alone it costs +1.10% on its own worst clip. Refused twice over.

`--mbt-depfloor` is off, and it is the clean null. Where a block's propagation
fraction is tiny the tree deposits an amount that cannot survive rounding into a
QP offset, so refusing the deposit looked free. It is: free of saving. At 8, 16
and 32 parts in 256 it reads inside a quarter of a percent of the baseline on
both cells, in both directions. The tree's time is in its motion search, not in
its arithmetic. This number is what says so.

### Band-level decisions

The gates above decide per macroblock. A row band can decide too. For a frame
with a band rule armed, a small table is built once at frame open out of the
lookahead fields that are already final by then: the band's mean and dispersion
of the lowres inter cost, its mean lowres intra cost, and its share of
macroblocks other frames lean on. The table costs one pass over those arrays per
frame, which measures under a hundredth of a percent of a 1080p encode.

The point is the axis. A per-block bound scaled by lambda widens as the rate
falls, so it is most generous exactly where a wrong verdict costs most. That is
what refused the pre-macroblock skip verdict a stage ago. A band verdict reads
content instead: the same band is refused at every rate on the same clip.

`--b-intra-band` is the one with a prize, and it is off. The B intra screen
prices three or four whole-macroblock intra predictions for every macroblock
that reaches it. The trial behind it wins a few hundred macroblocks in a
million. Where the band's own lookahead says intra is nowhere near competitive,
neither the screen nor the trial runs.

Its ceiling is the number to keep. Refusing every band, so that the B intra
screen and the trial never run on any macroblock, is worth **0.52% and 0.91%**
of instructions on the two low-rate 1080p cells. On one of them it changes the
byte count by a single byte in 1.31 MB. That is the whole compartment, measured
from the top. At 48 sixteenths it is 0.29% and 0.67%. At 96, where it stops
firing on live action altogether, it is 0.06% and 0.22%.

And the ceiling is where the quality is. Over six 720p and 1080p clips at five
rungs and matched achieved rate, refusing every band costs a median of +0.31%
and +0.95% on its worst clip. Three 1080p clips sit between +0.27% and +0.56%.
The tighter thresholds sit inside the corpus's own floor, where they buy two
tenths of a percent. A control arm that changed 0.02% of the encode measured
that floor at +0.19% on two 1080p clips and +0.67% on one 720p clip. It is off
on both counts.

`--b8-band` is off on a smaller number. The quadrant gate spends four 8x8 SATDs
to decide eight motion searches. Declining a uniform band deletes both. What it
cannot delete is what the existing gate already declines. On a low-rate 1080p
cell that gate refuses three quarters of the macroblocks reaching it, and the
eight searches run on one in forty. A band rule covering half the payers is
worth 0.18% and 0.02% of instructions. It is also the control this item measured
its corpus with. An arm that changes 0.02% of the encode and reads +0.67% of BD
on one clip is a floor, not a result.

The pre-macroblock skip verdict was the third candidate and it never got a gate.
Its band rule was priced offline first, from a per-macroblock dump of the
verdict and the lookahead evidence behind it. Every band field is orthogonal to
the damage: at the bottom of a 1080p ladder the verdict's wrong skips run
between 11% and 17% across every decile of every band field there is. There is
no band to arm it in.

`Y264_MBT_DEPFLOOR` overrides its flag in either direction. `Y264_BAND_ROWS`
sets the band width in macroblock rows (2). `Y264_BAND_PRECOMP=1` moves the P sub-partition gate's two interlocks
into the frame-open pass. It is byte-identical but off, because the gate's
lambda test already screens most macroblocks out before either interlock is
consulted.

### `--qpfile` refuses what it cannot place

The file is a decision, not a preference, so a line the encoder cannot honor
fails the encode with the frame number on stderr instead of being coded as
something close to it. A caller that asked for a placement and quietly got a
different one has no way to find out. Under the GOP splitter that frame number
counts from the first frame of the GOP the encoder was working on, and a second
line names it.

What is refused, and why:

- `B` on frame 0. The first frame of a stream is a key frame.
- `B` on a frame the key-frame interval has claimed, or on the first frame of
  a GOP the encoder split the work at. Moving the key frame is not what a type
  force means.
- `B` on the last frame. There is nothing after it to predict backwards from.
  The GOP splitter makes this sharper than it looks: a forced key frame close
  to an existing one cuts a short GOP, and the last frame of THAT is the last
  frame as far as the encoder coding it is concerned.
- `B` past `--bframes`. A sparse force lands in whatever B run the lookahead had
  already started, so a forced B needs room in the run at that point: one placed
  four frames after a key frame at `--bframes 3` arrives with the run already
  full. Naming the whole type sequence instead of a few frames of it takes the
  question away.
- `i`, a non-IDR I frame. It needs an open GOP, which this encoder does not
  have. `I` and `K` both mean IDR here, which is what the same file asks for
  on a closed-GOP encoder anyway.

One case is not refused. A forced `P` on a key frame's own slot yields to the
key frame, because an IDR is an anchor and the answer is a superset of what
was asked for.

`b` is accepted and codes a B slice. Whether that B is itself a reference stays
the B-pyramid's choice, so `b` and `B` are the same request here. There is no
non-reference seat to put one in.

The forced QP is absolute and it is the last word. The frame-type cascade, a
`--plan` zone's offset and the VBV are all upstream of it, and none of them
moves it. That also means the buffer cannot answer for a forced frame.
Per-macroblock AQ offsets still sit on top, exactly as they do on a `--qp`
encode. `--frame-stats` reports the slice QP, which is the number the file
names.

`--merange`, `--qcomp`, the deadzone pair, `--aq-mode`, `--no-mbtree`,
`--no-dct-decimate`, `--no-fast-pskip`, `--no-asm`, the ratio pair and the blur
pair all also have a `Y264_*` variable, **which still works and still wins**.
The flag sets the variable the encoder reads. If the environment disagrees with
the flag, the environment takes it and says so on stderr. That keeps sweep
scripts working when they override a binary's arguments from the environment. No
env var quietly beats an explicit flag. (`--no-psy` is the one exception in that
group. It sets the psy pair, two ordinary param fields, to zero and has no
variable.)

Three of them do not mean quite what the x264 flag of the same name means:

- **`--subme` also picks the search method.** With no `--me`, this encoder gates
 hex against UMH on subme (below 8 hex, 8 and above UMH), so `--subme 8` changes
 the algorithm and not just the effort. x264 keeps the two independent. Pass
 `--me` to pin it. `--subme 0` is refused rather than obeyed. It is x264's
 *fastest* mode but this library's "unset", which means the slowest (10), so
 accepting it would do the opposite of what an x264 user meant.
- **`--qcomp` reaches the ABR curve and the mb-tree strength derived from it,
 not the CRF or two-pass curves**, which carry their own fixed 0.6.
 The reference documents `--qcomp` as one global curve compression.
- **The deadzone flags are not a no-op at their own defaults.** Passing either
 swaps the exact shipped expression (intra `step/3`, inter `step/6`) for the
 1/64 approximation, so `--deadzone-inter 21 --deadzone-intra 11` measures
 +0.23% on foreman instead of 0. It also disables the NEON quant path. The flag uses the same 0-32 scale as the reference's option, so values carry
 over.

**`--trellis` takes all three of x264's levels** and defaults to 1, as x264
does. 0 is RDOQ off everywhere, with the plain deadzone. 1 quantizes the trials
with the deadzone and re-encodes only the winner with RDOQ. 2 runs RDOQ in every
mode decision.

**`--weightp` takes all three of x264's levels** and defaults to 1, so explicit
P-slice weighted prediction is on unless you turn it off. See the row above for
what each level does and for which of them is measured.

Interlaced sources are coded as FIELD PICTURES (PAFF): see `--tff`/`--bff`
below. There is no MBAFF and there will not be
([what-we-dont-do.md](what-we-dont-do.md)).

**What `--slices` costs, and what decides it.** Nothing predicts across a
slice's first row, so that row codes without its neighbors above. Each slice
also pays a header and an entropy-coder reset. Measured at `--slices 4`, 120
frames, five CRF rungs inside the VMAF-NEG 55-95 band, at matched achieved
bitrate (`scripts/bd_at_rate.py`):

| clip | BD-VMAF-NEG vs `--slices 1` |
| --- | --- |
| riverbed_1080p | +0.71% |
| touchdown_420 | +4.01% |
| pedestrian_1080p | +4.08% |
| sunflower_1080p | +8.53% |
| **median** | **+4.05%** |

The spread comes from **how much of the picture was going to be skipped**, not
from resolution. 8.4.1.1 gives P_Skip a zero motion vector wherever the neighbor
above is unavailable. Every macroblock in a slice's first row then loses the
skip whenever the picture is moving at all, against a baseline that is almost
entirely skip. riverbed (dense detail, little skip) pays +0.71%, while sunflower
(a slow pan over a mostly static frame) pays +8.53% for the same cut. The cost
is linear in the number of cuts: at CRF 32 on sunflower, `--slices` 2/4/8 costs
+2.96% / +6.81% / +14.84% of the bytes, and on riverbed +0.26% / +0.70% /
+1.47%.

On the detailed clips that per-cut cost is what an independent encoder pays
(riverbed, same measurement: +0.18% / +0.61% / +1.52%). On the skip-heavy ones
ours is several times that (sunflower: +0.51% / -0.36% / +2.09%). So this
encoder leans harder on P_Skip than it has to, and a slice cut is where that
shows. This is recorded as measured. Closing it is its own piece of work and was
not part of shipping the flag.

Slices are not a threading vehicle here, because the row wavefront already
parallelizes inside a picture. What they buy is loss resilience and decoder-side
parallelism, not encoder speed.

### Stream metadata

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--sar` | `W:H` | unspecified (square) | Sample aspect ratio. Accepts `16:11` or `16/11`. |
| `--level` | e.g. `3.1` or `31` | auto | Force an H.264 level. Range 1.0 to 6.2. |
| `--no-sei` | | SEI on | Drop the identification message at the start of the stream: a user-data SEI naming the encoder, its version and the options in force. |
| `--profile` | `baseline`\|`main`\|`high`\|`high10`\|`high422`\|`high444` | derived | Constrain the tool-set to a profile and write its `profile_idc`. See below. |
| `--aud` | | off | An access unit delimiter (NAL type 9) opens every access unit, ahead of the parameter sets. |
| `--pic-struct` | | off | VUI `pic_struct_present_flag` plus a `pic_timing` SEI per picture. A frame picture writes 0. Under `--tff`/`--bff` each field writes its own parity, 1 for a top field and 2 for a bottom one. |
| `--nal-hrd` | `none`\|`vbr`\|`cbr` | `none` | Write the buffer model into the stream: `hrd_parameters` in the VUI, a `buffering_period` SEI at every IDR, a `pic_timing` SEI carrying `cpb_removal_delay` and `dpb_output_delay` on every picture. **Needs `--vbv-maxrate` and `--vbv-bufsize`**, and a frame rate. Refused without them. `vbr` declares a schedule the channel may pause on and moves no sample and no slice bit. `cbr` declares a channel that never pauses, so every access unit is padded up to the rate with filler. See [rate-control.md](rate-control.md#what-vbv-actually-guarantees). |
| `--filler` / `--no-filler` | | follows `--nal-hrd` | Pad access units to the constant rate with filler NAL units (type 12). On under `--nal-hrd cbr` and refused anywhere else, because padding to a constant rate only means something where a constant rate is declared. `--no-filler` under `cbr` gives the CBR headers over an unpadded stream, whose buffer a conforming decoder may overflow. It is the negative control, not a shipping mode. |
| `--frame-packing` | 0..7 | off | `frame_packing_arrangement` SEI: 0 checkerboard, 1 column, 2 row, 3 side-by-side, 4 top-bottom, 5 frame alternation, 6 2D, 7 tile. Written once, "until canceled". |
| `--cll` | `MAX,AVG` | off | Content light level SEI (MaxCLL, MaxFALL) in cd/m^2, each a plain number 0..65535. MaxFALL above MaxCLL is refused unless MaxCLL is 0 (unknown). `0,0` writes no SEI. |
| `--mastering-display` | `G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)` | off | Mastering display color volume SEI. Chromaticity in 0.00002 units, luminance in 0.0001 cd/m^2. **G, B, R is the spec's order**, not the R, G, B a person writes. `L(0,0)` means unknown luminance. A chromaticity above 50000 (1.0), a luminance wider than 32 bits, or a min at or above a real peak is refused. A chromaticity outside the SEI's range (x 5..37000, y 5..42000), a peak above 10000 cd/m^2 or under 5 (other than 0), or a min above 5 cd/m^2 draws a warning and is written as given; a peak under 5 is usually nits typed in place of 0.0001 units. |
| `--alternative-transfer` | H.273 code or a `--transfer` name | off | `alternative_transfer_characteristics` SEI: the transfer a display should prefer over the VUI's. |
| `--overscan` | `undef`\|`show`\|`crop` | `undef` | VUI `overscan_info`. `undef` writes nothing, which is the default. |
| `--videoformat` | `component`\|`pal`\|`ntsc`\|`secam`\|`mac`\|`undef` | `undef` | VUI `video_format`. Naming it opens the `video_signal_type` block even with no color description. The three color codes then stay at 2 (unspecified). |
| `--stitchable` | | off | Size the **declared** DPB from the level rather than from `--ref`/`--bframes`, and pin the FrameNum width, so two encodes at the same geometry carry the same SPS. **It does move bits**: see below. |
| `--fake-interlaced` | | off | Declare a sequence that may carry field pictures (`frame_mbs_only_flag` 0) while coding nothing but frame pictures. |
| `--tff` | | off (see below) | Code each frame as two field pictures, top field first. |
| `--bff` | | off (see below) | The same, bottom field first. |
| `--no-interlaced` | | | Code an interlaced source as frame pictures anyway, ignoring the Y4M's field order. |
| `--sps-id` | 0..31 | 0 | `seq_parameter_set_id`, written in the SPS and named by the PPS. |

`--profile` takes `baseline`, `main`, `high`, `high10`, `high422` or
`high444`. With no `--profile` the profile is derived from the tools and the
content:

| Condition | profile_idc |
| --- | --- |
| CABAC or B frames, or neither | 77 (Main) |
| 8x8 transform on, or any quant matrix but flat | 100 (High) |
| 10-bit input, or `--output-depth 10` | 110 (High 10) |
| 4:2:2 input | 122 (High 4:2:2) |
| 4:4:4 input | 244 (High 4:4:4) |

**The derivation never lands on Baseline**, even at `--cavlc --bframes 0
--no-transform-8x8`, because explicit P-slice weighted prediction is signaled in
the PPS unconditionally and A.2.1 forbids it in Baseline. Claiming 66 with that
flag set would be a header the stream does not obey.

`--profile` is a **constraint, not a label**, and which of its two jobs it does
depends on where the conflicting tool came from:

- a tool the **preset** chose is narrowed in silence, because `--preset medium
  --profile baseline` is a reasonable thing to type and a preset is a default
  rather than a request;
- a tool **you named** is refused, because narrowing it would encode something
  other than what the command line says: `--profile main --transform-8x8` exits
  2 rather than dropping the transform;
- a profile the **content** cannot fit is refused: `--profile high` on 4:2:2
  input, or anything below `high10` on a 10-bit build.

`--profile baseline` is the one that also turns a tool off: it clears
`weighted_pred_flag` and `weighted_bipred_idc`, which is what makes 66 reachable
at all.

`--profile high10` is refused on an 8-bit build, which is every build today.

### `--stitchable` pins the SPS, and charges for it

The flag exists so two clips encoded separately can be concatenated. A decoder
that meets a second SPS has to re-initialize unless the second SPS is
byte-for-byte the first. Three SPS fields moved with the settings, and this flag
pins all three:

- `max_num_ref_frames`, `max_dec_frame_buffering` and `max_num_reorder_frames`
  become the level's own maximum (capped at 15), not what this encode needs;
- `log2_max_frame_num_minus4` becomes 4 unconditionally, rather than 0 at
  `--ref 1 --bframes 0`;
- the level is picked for that fixed 15-frame DPB, so it stops moving with
  `--ref` too, leaving it a function of frame size, frame rate and rate cap.

That last one is the charge. A higher declared level carries a wider `MaxVmvR`.
The search is clamped to the level, so **`--stitchable` really is a different
encode and not only different bytes**. That needs saying, because every other
flag in this group changes only the header.

It pins the **SPS**. The PPS still moves: `weighted_bipred_idc` follows
`--bframes` and `num_ref_idx_l0_default_active_minus1` follows `--ref`. Pinning
the second would mean writing `num_ref_idx_active_override_flag` on every P and
B slice, which costs bits on every picture instead of bytes once. Two streams to
be stitched should pass the same `--ref` and `--bframes`. Pinning the PPS
belongs with the rest of the stitching bundle.

### What `--fake-interlaced` actually changes

`frame_mbs_only_flag` becomes 0, so the sequence *may* contain field pictures.
It never does: every picture is coded exactly as it would be without the flag,
and every slice header says so with a `field_pic_flag` of 0. What moves is the
SPS geometry, plus one bit per slice header. `height_in_map_units` halves, an
`mb_adaptive_frame_field_flag` of 0 appears, and the vertical crop counts in
double units. A coded height that is an odd number of macroblock rows is padded
by one row and cropped away, because `FrameHeightInMbs` has to be even once the
flag is clear. 720p is 45 rows, so that is the common case, not a corner case.
The crop then counts in double units, which is why the flag needs a height that
is a multiple of 4 and refuses any other.

A named profile the stream was then checked against is an assertion, so the SPS
carries the matching constraint_set flag: `constraint_set0_flag` comes with
profile_idc 66 on its own, and `--profile main` adds `constraint_set1_flag`. A
derived profile asserts nothing it did not assert before.

### Field coding: `--tff` and `--bff`

An interlaced source is two half-height pictures per frame taken a field period
apart. Coding them as one frame asks the transform to model a comb. `--tff` and
`--bff` code them as what they are: two **field pictures** per input frame, each
half the height, in the named order. Each has its own slice header, its own
picture order count and its own reference list. The sequence declares
`frame_mbs_only_flag` 0 and every slice a `field_pic_flag` of 1.

**A Y4M that says `It` or `Ib` turns this on by itself.** The header knows the
field order, so nothing on the command line has to repeat it. `--tff` / `--bff`
override the header, and `--no-interlaced` refuses to field-code at all. Raw
input says nothing about itself, so there a flag is the only way in.

What the standard changes for a field picture, and this encoder with it: the
residual scan is the field scan instead of the zig-zag, CABAC reads its
significance contexts out of a second set of models, the deblocking filter drops
an intra horizontal macroblock edge from strength 4 to 3 and halves the vertical
motion threshold, and the declared vertical motion range halves.

What this release codes:

- **I, P and B fields, CAVLC and CABAC, 4:2:0.** `--bframes`, `--b-pyramid`,
  both `--direct` derivations and `--slices` all compose with field coding.
  What is refused under `--tff`/`--bff` is 4:2:2, 4:4:4, `--hw`, and a height
  that is not a multiple of 4. Those are refusals because this encode was told
  to field-code. When the Y4M's tag is all that asked, the named flag wins
  instead. One line on stderr then says the field order went unused. The tag is
  a property of the input. It is not a request.
- **A field's reference list names both parities.** The order is the standard's
  own: the same-parity field of the nearest frame, then the opposite-parity
  field of that frame, then the same-parity field of the one before it, and so
  on down the list. `--ref` counts reference FRAMES here as it does everywhere
  else. A field list therefore holds up to twice as many entries as a frame
  picture's, and reaches the same distance back. **The references a field
  macroblock searches roughly double with it**, which is the price of that
  reach. The slice header names every entry outright by its picture number
  rather than leaving it to the default derivation.
- **The second field of an I frame is a P field.** It predicts from the first
  field of its own pair, so a key frame costs one intra field rather than two.
  This is the one place a field reads the opposite parity. It is what the
  cross-parity chroma correction of 8.4.1.4 is for: the two parities site their
  chroma rows a quarter of a chroma sample apart inside their own fields, so a
  prediction that crosses parity has to move chroma by that quarter where luma
  moves by nothing. The field is still quantized as the intra field it
  replaces, because it is still half of a key picture.
- **Rate control and the lookahead stay frame-based.** A frame's type applies to
  its pair. The per-picture bit target and VBV credit are each half a frame's,
  because two pictures are coded per frame.
- The coded height is an even number of macroblock rows, padded and cropped
  away exactly as `--fake-interlaced` does, because each field is half of it.
  That makes the vertical crop count in double units, so **the height has to
  be a multiple of 4**. Any other could not be cropped back to itself, and a
  stream whose declared height is not the one you handed in is worse than a
  refusal. It answers to the same named-versus-inferred rule as the rest:
  refused under `--tff`, narrowed to frame coding under a bare `It` tag.
  Every broadcast interlaced height (480, 576, 1080) already is one.
  `--fake-interlaced` is always named, so there it is always a refusal. It
  did not check at all before this item.

Three speed-side gaps remain. All of them are named here rather than measured
away. A field picture runs motion search without the cached half-pel planes,
because those are built over the frame and describe no field. The residual RD
model prices a field block in frame-scan order against the frame context models.
The two levers that overlap a mini-GOP's B frames with their anchor do not run
for field pictures. One of them sizes its budget in rows of a single coded
picture, and a field pair is two pictures interleaved in one buffer. The other
gives each concurrent leaf a private reconstruction, and a field pair's two
pictures share one. None of the three moves a sample. All are worth closing when
field coding gets a speed leg.

`--dump-recon` writes **woven frames**, one per input frame, with the field
order in the Y4M's own `I` tag, because that is the picture a decoder outputs
and the picture the conformance gate compares.

**What it costs, measured rather than claimed, and measured BEFORE B fields.**
The figures below were taken when field coding was I and P only, with a
same-parity reference list and both fields of an I frame coded intra. All three
have since changed. Each of them moves the number. Read this as the last reading
rather than the current one. The re-measurement is a named follow-up in
docs/plan.md. On the two synthetic interlaced fixtures, at QP 26 with
CABAC and the 8x8 transform, field coding cost bytes for very little PSNR: 69.1
kB at 45.44 dB Y against frame coding's 55.3 kB at 45.31 dB on the testsrc2
clip, and 103.6 kB at 37.43 dB against 91.0 kB at 37.00 dB on an interlaced
foreman. Both fixtures are a PROGRESSIVE source run through `tinterlace`, so
their two fields are adjacent frames of a 50 Hz sequence and correlate
vertically. That is the content frame coding is best at. Field coding earns its
keep on real interlaced capture, where the fields are half a frame apart in
time. This project has not measured that yet. The rate-distortion leg is an
interlaced board, scheduled with the B-field item instead of guessed at here.
Until then, reach for `--tff` when the source really is interlaced and you need
a field-coded stream, not as a compression win.

`--level` below the computed conformant minimum is accepted, but prints a
warning that the stream may be non-conformant. It does not clamp the encode to
fit the level you asked for. The auto level is derived from frame size, frame
rate and DPB size per Annex A.

### Threading

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--threads` | N or `auto` | 0 = auto: online cores, at most 16 | Total thread budget, split across GOP workers and in-frame wavefront threads. |
| `--max-memory` | MiB | half of physical RAM | Memory budget for the threaded encode. When the GOP workers would not fit, fewer run at once, each with a wider wavefront. See [Memory](#memory-the-parallel-path-streams-through-a-bounded-window). |
| `--sync-lookahead` | frames or `auto` | auto | Input frames buffered so the lookahead runs ahead of the encode on its own thread. `0` disables. A negative count is refused. |

`--sync-lookahead` costs exactly that many frames of latency and changes no
bits. Auto resolves to `bframes+1` once the frame-thread pool is wide enough to
run a lookahead chain against, and 0 otherwise. The CLI prints the resolved lead
and its millisecond cost at your frame rate on stderr. `--tune zerolatency` sets
it off.

There is no `--frame-threads` flag. The in-frame wavefront share is derived from
`--threads` by the budget split.

## Presets

The ladder sets six things. Everything else is preset-independent.

| Preset | subme | subpel | ref | rc-lookahead | cabac | 8x8 | bframes | lr-settle | partitions |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ultrafast | 1 | 2 | 1 | 0 | off | off | 0 | 1 (inert) | p8x8,b8x8,i4x4 |
| superfast | 1 | 2 | 1 | 0 | on | on | 3 | 1 (inert) | p8x8,b8x8,i8x8,i4x4 |
| veryfast | 2 | 2 | 1 | 10 | on | on | 3 | 1 | p8x8,b8x8,i8x8,i4x4 |
| faster | 4 | 2 | 2 | 20 | on | on | 3 | 1 | p8x8,b8x8,i8x8,i4x4 |
| fast | 6 | 2 | 2 | 30 | on | on | 3 | 0 | p8x8,b8x8,i8x8,i4x4 |
| **medium** | 7 | 2 | 3 | 40 | on | on | 3 | 0 | p8x8,b8x8,i8x8,i4x4 |
| slow | 8 | -1 | 5 | 50 | on | on | 3 | 0 | all |
| slower | 9 | -1 | 8 | 60 | on | on | 3 | 0 | all |
| veryslow | 10 | -1 | 16 | 60 | on | on | 3 | 0 | all |
| placebo | 11 | -1 | 16 | 60 | on | on | 3 | 0 | all |

The lr-settle column is the one entry here that trades quality instead of
effort. Every other column buys picture quality with time, so a slower preset is
better everywhere. This one gives a little quality back for speed, so it runs
the other way down the ladder. It is the only column that has to be argued
against a bar. Presets at medium and above keep the +0.2%-per-clip bar the
parity claim is read on and carry none of these arms. The presets below medium
are allowed +0.5% per clip. `superfast` and `ultrafast` run no lookahead window,
so the value they name changes no byte. It is written there so the column never
turns back on as the ladder gets faster.

subpel 2 is a capped diamond. -1 is an 8-neighbor square
iterated to convergence. subme at or below 8 uses the fast SATD partition path,
and 9 and above does full RD per partition.

The partition column is **derived, not tabulated**. Every preset splits P and B
macroblocks and tries 4x4 intra. The sub-8x8 P shapes arrive with the full-RD
analysis tier at subme 8. 8x8 intra needs the 8x8 transform to code it, so
ultrafast drops the shape along with the transform. That is why `--subme 9` on a
fast preset still searches the sub-8x8 shapes: the set follows the resolved
configuration, not the preset's name. `--log-level debug` prints the list the
run resolved to.

`all` therefore adds exactly one shape at medium, `p4x4`, and nothing at slow
and above. It is an error at ultrafast, where `i8x8` has no transform to code
it. Spell the list instead, or add `--transform-8x8`.

The preset also gates motion search when `--me` is not given: subme below 8 runs
hex, 8 and above runs UMH. So `--preset slow` changes the search algorithm, not
just its effort.

Explicit flags override the preset regardless of order on the command line:
`--ref`, `--bframes`, `--rc-lookahead`, `--cabac`/`--cavlc`,
`--transform-8x8`/`--no-transform-8x8`, `--me`, `--partitions`.

An unknown preset name is an error, not a warning.

## Tunes

`--tune` sets content-adaptive defaults. Explicit `--psy-rd` and
`--psy-trellis` override whatever the tune chose.

| Tune | Effect |
| --- | --- |
| `grain` | psy-trellis 1.0, psy-rd 1.5. The 1.5 was a measured VMAF-NEG win on heavy grain against a 1.0 default. The default has since moved to 2.0, so the tune now lowers psy-rd and has not been re-measured against it. |
| `film` | psy-trellis 0.5. psy-rd left at the 2.0 default. The film value has not been BD-measured for want of a film clip. |
| `animation` | 4 more B frames, only when `--bframes` isn't given and the preset's count is under 8. psy-trellis and AQ stay at the preset's values: raising one and lowering the other measured as a coin flip across two cartoon clips. |
| `psnr` | psy-rd 0, psy-trellis 0, aq-strength 0. With psy off, the size-based chroma QP and deblocking offsets stay at 0. |
| `ssim` | psy-rd 0, psy-trellis 0, AQ kept. With psy off, the size-based chroma QP and deblocking offsets stay at 0. |
| `zerolatency` | bframes 0, rc-lookahead 0, sync-lookahead off. |
| `stillimage` | psy-trellis 0.7, aq-strength 1.2, deblock -3:-3. The reference tune's constants, **borrowed and not measured here**: there is no still-image clip in the corpus, and inventing one to fit a tune would measure the clip. |
| `fastdecode` | No deblocking filter, CAVLC, no weighted biprediction, no explicit P weighted prediction. Everything that costs the decoder is off. That is a real quality loss, on purpose. With `--bframes 0 --no-transform-8x8` it is the one arrangement that declares Baseline without naming a profile. |

`zerolatency` and `animation` only apply their frame-type changes if you did not
set `--bframes` yourself. An unknown tune name is an error.

## Which options change the bitstream shape

Worth knowing before you A/B anything:

- `--cqm flat` writes no scaling matrix at all, so it is byte-identical to a
 decoder using the default flat-16. So does a custom set that is 16 in every
 list, however it was spelled.
- `--sync-lookahead` never changes a bit. It is pure latency-for-throughput.
- `--threads` **can** change bits. See the next section.
- `--no-sei` changes the stream but not the pictures.
- `--constrained-intra` changes the stream in P and B slices only. An
  all-intra encode (`--keyint 1`) differs by the one PPS bit and reconstructs
  identically.
- `--slices` changes the stream at every slice count above 1, and changes the
  pictures with it: the prediction a slice boundary withdraws is prediction the
  encoder then has to replace. `--slices 1` is the default and byte-identical
  to no flag at all.
- `--partitions` changes the pictures at every value except the one the preset
  already resolved to. It is a search restriction, so nothing about it is
  signaled: the stream simply stops carrying the mb_type and sub_mb_type codes
  for the shapes that were taken away. The default is the derived set, and it is
  byte-identical to the flag being absent.
- Since 2026-09-16 the SPS declares the vertical motion-vector bound it is
  actually held to (`log2_max_mv_length_vertical`) instead of a flat 16, which
  advertised +-16384 luma samples at every level. It costs the SPS 0 or 1 byte
  and moves no picture, but it does mean **a bitstream cmp against a binary
  older than that differs in the SPS NAL**. Compare `--dump-recon` instead, or
  compare everything after the first NAL.

## Threading, and what it does to your output

`--threads N` is a budget, not a worker count. The CLI splits it into `g` GOP
workers times `k` in-frame row-wavefront threads, sizing each GOP's share by the
frames it actually owns, and prints what it chose:

```
yah264: encoded 250 frame(s) in 2 GOP(s) on 2 GOP-worker(s) x 4 frame-thread(s)
```

**Determinism is guaranteed only at one thread.** At
`--threads 1` the same input and config give the same output bit for bit. The
test gates hold it. With more threads the same input, config and thread count
usually repeat too. Neither run-to-run nor cross-thread-count identity is
promised, though, because holding it would rule out optimizations. x264 does not
promise it either. Output differs across *different* thread counts by design.
The in-frame wavefront prices predecessor context slightly differently from the
serial path, so `k=1` and `k>=2` can differ (all `k>=2` agree with each other).
Pinning `--threads` removes that source of difference, but not every other.

`--dump-recon` forces the fully serial path, because the recon stream has to be
a single continuous self-consistent encode. Two-pass falls back to it as well in
the cases below. **A `--threads N` greater than 1 that cannot be honored says
so on stderr and names the condition** rather than silently encoding on one
thread:

```
yah264: warning: --threads 8 cannot be honoured, encoding serially: --dump-recon needs one continuous self-consistent recon stream, which per-GOP encoders cannot produce
```

It fires only on an explicit `--threads` above 1. Leaving it unset means "auto,
every core", which is not a request, and warning on it would fire on all 252
`--dump-recon` runs of the conformance gate.

**4:2:2 and 4:4:4 thread.** The encoder codes all three chroma formats through
I/P/B on both entropy coders. The GOP-parallel reader's frame store reads the
subsampling off the Y4M `C` tag, as the serial path and the recon dumper do.

Two-pass runs on the parallel path in both passes, splitting the stats file
along GOP boundaries. Pass 2 needs a stats file that a *threaded* pass 1 wrote:
one from a serial pass 1 has no GOP markers to split on, so pass 2 quietly stays
serial for it. Pass 2 also refuses outright, with a message, if the stats file
describes a different GOP split than the current run computed. That happens if
you change `--frames` or `--keyint` between passes.

### Memory: the parallel path streams through a bounded window

The parallel path reads the input on its own thread and keeps only part of it
resident. A GOP worker frees each input frame as soon as its encoder has taken
it, so what the input store holds is read-ahead. How much it holds depends on
whether the input can seek.

**From a file**, the frame count and the GOP table are known before the first
read, and every frame is a fixed-size record one seek away. The reader serves
the GOPs in flight in the order their workers consume them, keeping each one at
most 16 frames ahead of its worker, plus the first 16 frames of the next GOP the
queue will hand out. The store is `(GOPs in flight + 1) x 16` frames whatever
the title length and however fast the disk delivers: 272 frames (611 MiB) on a
ten-minute 1920x818 title at default threads. The frames are the same bytes in
any order, so the stream is the one an in-order reader writes, at every thread
count.

**From a pipe**, the reader has to read in display order, so to start a GOP it
must hold every frame between the oldest GOP still coding and the new one. The
window is `(--threads + 1) x --keyint` frames, narrowed to read-ahead once the
workers start. Early dispatch hands GOPs out in display order as the reader
reaches them, which keeps the GOPs in flight contiguous (558 frames on the same
title). Above 1080p a store ceiling of an eighth of physical RAM applies.

What remains is the GOP workers themselves. Each one is a whole encoder, with
its lookahead ring, reference and half-pel planes, mb-tree arrays and per-frame
decision records: about 0.5 GiB at 1080p and 1.4 to 1.7 GiB at 2160p with one
frame thread each, and up to 0.9 and 3.0 GiB when a worker runs a wide
wavefront. With the input bounded, peak memory is close to the worker count
times that. `--max-memory MIB` sets a budget for the whole encode (default: half
of physical RAM). When the workers would not fit, fewer GOPs run at once, each
with a wider wavefront, so the thread budget is unchanged. On a 64 GiB machine
the default budget does not bind at 1080p or on 2160p titles at default threads.
On a 16 GiB laptop it keeps a 2160p encode out of swap. It says so on stderr
when it binds:

```
yah264: 3 GOP worker(s) instead of 8 to stay inside the memory budget (about 2.6 GiB of 2.9 GiB; --max-memory sets it)
```

Peak footprint (`/usr/bin/time -l`) on a ten-minute 1920x818 title (13177
frames, `--crf 23`, default threads, 16 workers) and on the 600-frame 2160p
`meridian` clip (3 workers of 8 frame threads), median of interleaved runs:

| Input | In-order reader | Seek reader |
| --- | --- | --- |
| 1920x818 title: input store | 2946 frames, 6.6 GiB | 272 frames, 0.6 GiB |
| 1920x818 title: footprint / max RSS | 12.4 / 15.0 GiB | 8.3 / 10.9 GiB |
| 2160p clip: input store | 324 frames, 3.8 GiB | 48 frames, 0.6 GiB |
| 2160p clip: footprint | 10.8 GiB | 9.0 GiB |

Max RSS sits above the footprint because it also counts pages the allocator
has freed and marked reusable, which the system takes back on demand.

The in-order reader's store on a file was a budget and not a bound. With the
frame count known, every worker starts at once. The worker given the sixteenth
GOP waits for a frame 3750 frames into the title. A starving worker lets the
reader past its window, so the store grew to everything in between. How far it
grew followed how fast the input arrived (10.9 to 19.6 GiB of RSS on that
title).

The history of the window, measured on 720p at `--threads 1 --keyint 50`, peak
RSS windowed against holding the whole input resident:

| Input frames | Whole input resident | Windowed |
| --- | --- | --- |
| 180 | 503 MB | 378 MB |
| 450 | 930 MB | 384 MB |
| 900 | 1631 MB | 384 MB |
| 1800 | 3035 MB | 385 MB |

The windowed column is flat: 1.559 MB per frame of clip becomes 0.004. The
window held 100 frames of 100 at every one of those lengths.

On a pipe the window's worst case is generous, so size it before it surprises
you. At the default keyint it is 500 frames on one thread and 4750 frames (14
GiB at 1080p) at 18 threads. It is reached only when the split waits for the GOP
count before any worker starts (one thread on a pipe, or a forced split). Lower
`--keyint`, lower `--threads`, or set `Y264_STREAM_WINDOW`.

The cut-aware split (`--cut-split`, `--shot-crf`, and `--plan` or `--qpfile`
with a forced IDR) needs every cut before the first GOP can be dispatched,
because the boundaries it finds are what the dispatcher schedules on. From a
file it reads the input twice. First a scan pass walks the frames, hands each
one's luma to the pre-scan and drops it (the chroma is skipped, not read),
keeping one byte per frame. Then the ordinary windowed encode runs on the GOP
table the scan produced. It takes the seek reader like any other file. The
stream is byte-identical to the one the old whole-input read wrote. Peak RSS on
a ten-minute 1920x818 title (13177 frames, `--crf 23`, default threads, default
memory limit):

| Mode | Whole-input read | Scan pass, in-order reader | Scan pass, seek reader |
| --- | --- | --- | --- |
| plain | 10.9-19.6 GiB | the same path | 10.3-11.8 GiB, 27 s |
| `--cut-split` | refused: needs 34.1 GiB, limit 32.0 | 14.2 GiB, 30 s | 12.7 GiB |
| `--shot-crf` | refused: needs 34.1 GiB, limit 32.0 | 14.8 GiB, 31 s | 13.5 GiB |
| `--shot-crf --scan-in` | | 14.6 GiB, 29 s | |

Under the in-order reader the cut-aware split hands its GOPs out longest-first
only within runs of half the `(--threads + 1) x --keyint` window. Sorted over
the whole title, the first workers took GOPs from one end to the other, and the
in-order reader held 7877 frames (24.7 GiB) to feed them. The seek reader holds
272 frames however the GOPs are ordered. The bounded runs stay, because the
hand-out order is part of what the rate carry chains on.

A pipe cannot be read twice, so on a pipe the cut-aware split still holds the
whole input and refuses once it reaches the memory limit. Encode from a file.
Two-pass needs the GOP split before pass 1 writes its per-GOP stats, so it needs
a seekable input. It has to read the input twice anyway.

Frame counts come from the file length instead of from reading frames. A Y4M
frame is a fixed-size record once the stream header is past, so yah264 reads the
first `FRAME` header, seeks back, and checks that the remainder divides exactly.
That matters for more than the memory guard. Every scheduling decision on this
path is a function of the frame count, so knowing it up front keeps the schedule
and the bitstream identical to what the whole-input read produced. A pipe has no
length. It gets the same schedule anyway by waiting, before it dispatches, for
EOF or the `--threads + 1`'th GOP boundary, whichever comes first. Past that
point the frame count no longer changes the answer.

The memory refusal measures the window, not the clip, so it fires on a machine
too small for the parallelism you asked for. The limit is half of **physical**
RAM, overridden by `Y264_MAX_INPUT_MB` in MiB. It counts physical memory and not
free memory, because on macOS most of what `vm_stat` calls inactive, speculative
or purgeable is reclaimable on demand. A free-page gate refuses encodes that
would have run and answers differently between two runs of the same command.

```
yah264: this encode needs 41.7 GiB of memory and the limit is 32.0 GiB
yah264: the threaded path streams, but its window is (--threads + 1) x --keyint = 14250 frame(s); lower either, or set Y264_STREAM_WINDOW
yah264: limit 32.0 GiB (50% of 64.0 GiB physical), 2.97 MiB resident per 1920x1080 frame, so 11046 frame(s) fit
yah264: encode a segment with --frames, split the input, or raise Y264_MAX_INPUT_MB
```

A raw 4:2:0 8-bit frame is `w*h*1.5` bytes, 4:2:2 is `w*h*2` and 4:4:4 is
`w*h*3`. The guard computes from the real geometry. On top of the window sits a
fixed per-worker encoder cost that does not scale with it: around 330 MiB for
one 1080p worker and another 50 for a second.

### The saved pre-scan: `--scan-out` and `--scan-in`

The scan pass reads the whole file, which on a long title is a real fraction of
the encode's wall time. A CRF ladder or a re-encode runs the same scan over the
same frames every time. `--scan-out FILE` saves its answer and `--scan-in FILE`
replaces the pass with it. The answer depends on the frames and on a handful of
settings but not on the rate control, so one scan serves every rung of a CRF
ladder, `--cut-split` and `--shot-crf` alike.

A reused scan that does not belong to the encode would put IDRs and shot CRFs on
the wrong frames without any error. So the file records what it was computed
from, and `--scan-in` refuses it unless all of it matches, naming the line that
differs:

- the frame count, after `--seek` and `--frames`;
- the input geometry: size, chroma format, bytes per sample, `--crop-rect`;
- the scan's settings, as the library resolves them: coded size, sample depth,
  `--keyint` and the minimum keyint, `--scenecut`, whether B frames are on (the
  flash test reads nothing else of them), and the environment knobs that move
  the scan's costs or its cut test;
- a 64-bit FNV-1a hash of the on-disk luma of the first, middle and last frames.

The hash is a cheap identity check, not a checksum of the file: it catches a
different title, a different cut or a different start. It does not catch an edit
to a frame it does not read.

The format is plain text, one item per line, and versioned:

```
yah264-scan 1
frames 690
input 352x288 chroma=420 sample-bytes=1 crop=0,0,0,0
scan depth=8 size=352x288 keyint=250 min-keyint=25 cuts=on threshold=0.4 flash-guard=1 jump=0.1 floor=0.9 lr-intra=1/3/5 lr-prev-shape=1
fingerprint 8907dbe241b271f3
idr 5
0 150 300 390 540
shots 5
0 149 851292.82666666666 1017188 408168.55033557047 0.47946903527168272
150 299 1331626.1866666668 1472690 1159031.7516778524 0.87038822402490168
300 389 1525360.3555555556 1600258 1128916.6404494382 0.74009832256212826
390 539 553767.28 570692 58226.05369127517 0.10514534858627828
540 689 2065633.2533333334 2162706 1061194.3221476511 0.51373801251272022
end
```

`idr` gives the count, then the frame numbers of the scan's IDR map, 16 to a
line: every cut and every keyint IDR, before `--open-gop`, `--plan` and
`--qpfile` adjust it. `shots` gives the count, then one shot per line: first
and last frame, mean and peak lowres intra cost, mean inter cost, and their
ratio, the same fields `--shot-table` prints. The costs are written with enough
digits to read back as the same doubles, which is what keeps `--shot-crf`
byte-identical through the file. A version this build does not read is refused,
and so is a truncated file.

## x264 compatibility

Options that match x264 in **name and semantics**, so a command line ports
across unchanged:

`--preset`, `--tune` (the six names listed above), `--bitrate`, `--qp`,
`--keyint`, `--min-keyint`, `--no-scenecut`, `--bframes`, `--b-adapt`, `--ref`,
`--psy-rd`, `--psy-trellis`,
`--aq-strength`, `--rc-lookahead`, `--sync-lookahead`, `--vbv-maxrate`,
`--vbv-bufsize`, `--pass`, `--stats`, `--threads`, `--frames`, `--sar`,
`--level`, `--me`, `--direct`, `--cqm`, `--aud`, `--pic-struct`, `--nal-hrd`,
`--filler`,
`--frame-packing`, `--cll`, `--mastering-display`, `--alternative-transfer`,
`--overscan`, `--videoformat`, `--fake-interlaced`, `--open-gop`, `--tff`, `--bff`,
`--no-interlaced`, `--seek`, `--crop-rect`,
`--input-res`, `--input-csp`, `--fps`, `--quiet`, `--log-level`,
`--no-progress`, `--no-psy`, `--no-dct-decimate`,
`--no-fast-pskip`, `--no-mbtree`, `--no-asm`, `--deblock`, `--no-deblock`,
`--no-weightb`, `--constrained-intra`, `--chroma-qp-offset`, `--qpmin`,
`--qpmax`, `--qpstep`,
`--vbv-init`, `--sps-id`, `--slices`, `--crf-max`, `--ratetol`, `--partitions`,
`--b-bias`, `--longhelp`, `--fullhelp`, `-o`, and the one-letter aliases `-I`,
`-i`, `-b`, `-r`, `-f`, `-q`, `-B`, `-p`, `-A`, `-m` and `-t`.

Options x264 spells differently. Each does the same job under the name on
the right:

| x264 option | Here | Notes |
| --- | --- | --- |
| `--no-cabac` | `--cavlc` | `--cabac` turns it back on. |
| `--no-8x8dct` | `--no-transform-8x8` | `--transform-8x8` turns it back on. |
| `--dump-yuv` | `--dump-recon` | Writes the reconstruction as Y4M. |
| `--psy-rd A:B` | `--psy-rd A --psy-trellis B` | `--psy-rd` takes one number here. The two-part form is refused. |

Options that differ, and how:

| Option | The difference |
| --- | --- |
| `--nr` | Same flag and range, but not the same tool. The reference's `--nr` takes one value. Here `--nr N` sets the intra and inter strengths together, `--nr-intra` and `--nr-inter` set one each, the offset is scaled by each transform position's gain, and pictures that read clean on a spatial noise gate are coded with no offset at all. See [denoise.md](denoise.md). |
| `--qpfile` | Same file format and the same frame-type letters, with two differences. `i` (a non-IDR I frame) is refused, because that needs an open GOP. `I` and `K` both force an IDR. `b` codes a B slice but does not force it to be a non-reference one: whether a B is a reference stays the B-pyramid's. |
| `--crf` | **The number is not comparable to x264's.** The same CRF value has measured a size difference from -54.6% to +45.3% against x264 across the corpus. Do not port a CRF setting across. See [rate-control.md](rate-control.md). |
| `--crf` | Fractional values are accepted but largely inert. The quantizer rounds to an integer QP. |
| `--crf` | The CRF scale moved with this release. The frame-level AQ strength under uncapped CRF went from 1.04 to 1.3 (capped CRF keeps 1.04), so at the same CRF value busy content now codes smaller than it did before: 5% on foreman, 13% on stefan and 19% on bus at CRF 23. Quality per bit is better. If you tuned a CRF value on an earlier yah264, check your sizes again, and compare encoders at matched size or quality, never at matched CRF. |
| `--crf 0` | x264's lossless. Not implemented here, and refused rather than accepted, because `rc.rf = 0` means "CRF unarmed" in the param struct. |
| `--direct` | x264's `none` is not implemented and is refused. `auto` (the default, as in x264) picks per slice by skippability. `spatial` and `temporal` behave the same as x264's. |
| `--qp` | Both encoders turn mb-tree and AQ off at constant QP, so `--qp N` is the same workload on each. |
| `--scenecut` | On the CLI, `--scenecut 0` means off, same as x264. In the C API, `param.scenecut = 0` means **default (40)** and off is spelled with a negative. Assign `YAH264_SCENECUT_OFF`. See [Calling it from C](#calling-it-from-c). |
| `--sync-lookahead` | Same 0-means-off spelling on the CLI. The API spells off with a negative: assign `YAH264_SYNC_LOOKAHEAD_OFF`. |
| `--input-y4m` / `--input-raw` | x264 sniffs the input format. Here the two are separate flags and nothing is guessed. |
| `--tune stillimage` | The reference's constants, not measured here. |
| `--tune fastdecode` | Clears explicit P weighted prediction as well, which x264's does not: `--weightp 0` is part of the tune here. |
| bare default | x264 defaults to CRF 23, and yah264 to QP 26. |
| `--cqmfile` | Honors a file's `CHROMAV` lists as the Cr lists, where x264 codes Cr with the Cb list. The 4:4:4 chroma 8x8 lists, and x264's `--cqm8ic`/`--cqm8pc`, have no effect: 4:4:4 never picks the 8x8 transform here. |
| `--partitions` | Same names, same meaning, same two words for a whole set. The default set follows **this** encoder's preset ladder, so the list a preset name resolves to here is not the list the same name resolves to there. The table above is the one that applies. `p4x4` without `p8x8`, and `i8x8` without the 8x8 transform, are refused with the rule rather than narrowed. |
| `--subme` | Also selects the search method here: with no `--me`, below 8 is hex and 8 or above is UMH. x264 keeps effort and method independent. `--subme 0` is refused. See the coding-tools section. |
| `--merange` | UMH, esa and tesa read it; hex ignores it. |
| `--me tesa` | Re-ranks the 16 cheapest SAD candidates of the exhaustive scan by SATD, rather than scoring every position in SATD. |
| `--no-chroma-me` | A no-op: the search here never reads chroma, which is x264's `--no-chroma-me` behavior all the time. |
| `--qcomp` | Reaches the ABR curve and mb-tree strength. The CRF and two-pass curves carry their own. The reference documents it as one global setting. |
| `--deadzone-inter`/`-intra` | Same values and scale, but passing either also swaps the exact shipped quant expression for its 1/64 approximation, so x264's own defaults measure +0.23% rather than 0. |
| `--profile` | Refuses a tool you named rather than dropping it, where x264 narrows silently. `high10` needs a 10-bit build. |
| `--stitchable` | Pins the SPS only, and raises the declared level (and with it the MV range), so it changes the encode and not only the header. |
| `--pic-struct` | Writes pic_struct by parity: 0 for a frame picture, 1 or 2 for the two halves of a field pair. The clock timestamps are all written absent, which is the cheapest legal spelling. |
| `--nal-hrd` | One SchedSelIdx and NAL HRD only, where x264 offers the choice of both models. The encoder obeys one bucket, so declaring a second would be declaring a schedule nothing enforces. |
| `--b-pyramid` | `none` and `normal` only. x264's `strict` is refused rather than read as `normal`. |
| `--mvrange` | Narrows the level's bound and never widens it. x264 lets a `--mvrange` above the level's stand. |
| `--pass 3` | Same meaning as x264's (read the stats and write them back), but it rewrites the file **in place**. Keep a copy if you want the pass-1 records afterwards. |
| `--open-gop` | Refused with `--stitchable`. It makes the whole encode ONE encoder instance, so `--threads` buys row wavefront rather than parallel GOPs. On an input with no readable frame count (a pipe) it falls back to the serial path. |
| `--intra-refresh` | P frames only. x264 also allows B frames with it, but here they are refused (or dropped when only a preset set them). A scene cut is ignored rather than placing a keyframe. The encode is one encoder instance, so `--threads` buys row wavefront rather than parallel GOPs. |
| `--aq-mode` | `0` is refused here. x264's 0 turns AQ off. Here the value only selects a metric and has no off seat, so 0 would encode as 1. Spell AQ off `--aq-strength 0`. |
| `--zones` | `q=` and `b=` only, which is all x264's help lists. `b=` is applied as a QP offset of -6 log2(F) in every rate-control mode, so it moves the quantizer and the rate follows. Refused alongside `--plan`, which writes the same zone list. |
| `--b-bias` | Same range and direction. Here it moves one cost ratio in the placement test, so +100 and -100 have fixed meanings: a B wherever anything predicts better than intra, and no B frames at all. |
| `--ipratio`/`--pbratio`/`--cplxblur`/`--qblur` | Two-pass only. The reference documents its ratio pair as global. The single-pass modes here anchor I and B through the CRF track and never read them. |

x264 options with **no equivalent at all**:
`--muxer`/`--demuxer`. `--slice-max-size`, `--slice-max-mbs`,
`--slice-min-mbs` and `--slices-max` are refused: `--slices` takes a count,
not a size cap. x264's `--interlaced` is `--tff`/`--bff` here, and it
codes field pictures rather than MBAFF. The full list of x264 options with no
flag here, and what was decided for each, is the census in
[plan.md](plan.md).

## Calling it from C

Everything above describes the CLI, which is x264-compatible where it claims to
be. The C API in `include/yah264.h` matches x264's field values, with three
deliberate divergences: `scenecut`, `sync_lookahead` and `subme`. All three come
from the zero-as-unset convention, not from a numbering choice. Read this before
you port anything.

None of it applies if you use the CLI. The CLI maps each flag onto the values
below for you.

**Zero means the default, not off.** `yah264_param_t` fills itself
from `yah264_param_default`, and twenty of its fields treat zero as "unset,
pick the default". That convention is fine until a field's off switch is a value
x264 spells as zero. Three fields are in that position:

| Field | x264 spells off as | Zero here means | Off here is |
| --- | --- | --- | --- |
| `scenecut` | `--scenecut 0` | the default, **40**, which is x264's own aggressiveness | `YAH264_SCENECUT_OFF` |
| `sync_lookahead` | `--sync-lookahead 0` | auto, a lead of `bframes+1` | `YAH264_SYNC_LOOKAHEAD_OFF` |
| `filler` | n/a (x264 has no off switch) | follow `nal_hrd`: CBR pads, nothing else does | `YAH264_FILLER_OFF` |

`filler` is here for a sharper reason than the other two. A caller that memset
the struct, set `nal_hrd = YAH264_NAL_HRD_CBR` and left `filler` alone would get
CBR headers over an unpadded stream on an off-is-zero reading. That is a
non-conforming stream with no diagnostic, so zero follows `nal_hrd` instead.

So `param.scenecut = 0` does not disable adaptive keyframes. It requests them at
full strength. Nothing errors, the encode succeeds, and you find out from the
keyframe count and the file size. Use the constants:

```c
yah264_param_t param;
yah264_param_default(&param);
param.scenecut = YAH264_SCENECUT_OFF; /* NOT 0 */
```

Both constants are `-1`, and any negative value means off.

The convention is deliberate for these two. Nineteen other fields depend on
zero-as-unset, and making two of them read zero differently would swap a
divergence you can document for an inconsistency you cannot.

### The enumerated fields carry x264's values

Porting a constant gets you the tool it names. What yah264 does not implement
is refused by `yah264_encoder_open` rather than narrowed to the nearest thing
available, so there is no value that quietly encodes something else.

| Field | x264 | yah264 |
| --- | --- | --- |
| `rc.method` | CQP 0, CRF 1, ABR 2 | x264's, plus 2-pass at **100** |
| `rc.rf` | `float`, `23.0` | **`double`**, `23.0` |
| `me_method` | DIA 0, HEX 1, UMH 2, ESA 3, TESA 4 | x264's, plus auto at **-1** |
| `direct` | NONE 0, SPATIAL 1, TEMPORAL 2, AUTO 3 | the same values; NONE refused, AUTO is the default |
| `csp` | I400 1, I420 2, I422 6, I444 12 | x264's **I420 2, I422 6, I444 12** |
| `subme` | 0 = fastest | 0 = the library default 10, the slowest; see below |

`YAH264_ABI_VERSION` is 3. Assert on it if you want a build-time tripwire, and
`#ifndef` it if you also build against headers that predate it.

At ABI 3 the first field of `yah264_param_t` is `size`.
`yah264_param_default()` writes it with `sizeof(yah264_param_t)`.
`yah264_encoder_open()` refuses a struct whose `size` is not the library's own.
A caller built against a different copy of the header is therefore stopped at
open, instead of encoding from a struct it has misread. Call
`yah264_param_default()` first and the field is never yours to set.

#### Where matching x264 exactly is not possible

Four places, each deliberate and none a half-match:

- **`YAH264_RC_2PASS` is 100.** x264 has no 2-pass rc method. It spells 2-pass
 as ABR plus `b_stat_read`/`b_stat_write`. 3 is exactly where x264 would put a
 fourth method, so 2-pass sits where a future `X264_RC_*` cannot collide with
 it. With `_2PASS` and a `stats` path, leaving `rc.pass` at 0 runs pass 1, which
 opens the stats file for writing and **truncates it**.
- **`YAH264_ME_AUTO` is -1.** x264's `me_method` is always explicit, so every
 non-negative seat belongs to `X264_ME_*` (which already uses 0..4). Negative
 is also how this struct spells auto for `subpel` and `sync_lookahead`. The
 consequence worth knowing: **auto is not 0**, so a param struct that skips
 `yah264_param_default` is asking for dia. `param_default` writes `direct` and
 `me_method` explicitly for exactly this reason.
- **`csp` takes x264's values but not its encoding.** `X264_CSP_*` are bitflags
 with a mask (`X264_CSP_MASK`) and modifiers (`_VFLIP`, `_HIGH_DEPTH`) layered
 on top. yah264 implements none of that. The depth is a property of the LIBRARY
 you called, not a bit in the csp, so `X264_CSP_I420 | X264_CSP_HIGH_DEPTH` is
 not 4:2:0 to us but an unknown value, and open fails. Pass one constant and do
 not mask. Everything in the gaps (I400, NV12/NV21, YV12/YV16, YUYV/UYVY/V210,
 YV24, the RGB family) is likewise refused rather than approximated.
- **`rc.rf` is a `double`, not x264's `float`.** Assigning `f_rf_constant` to a
 double is exact, so porting works. `0` leaves CRF unarmed instead of meaning
 x264's lossless `--crf 0`. That comes from the zero-as-unset convention above,
 not from a scale problem. The CLI rounds `--crf` to tenths. Ask through the API
 for finer.

#### `subme` follows x264's scale except at zero

The scale is **not inverted and not offset**. It runs the same direction as
x264's subpel level (higher = slower, more RD) and the tiers line up. The
only disagreement is at zero: `0` here is the library default **10**, the
slowest setting, where x264's `0` is a real mode and its fastest. That is the
zero-as-unset convention shared with nineteen other fields, and renumbering one
of them would swap a divergence you can document for an inconsistency you
cannot. Porting `0` for speed maximizes effort instead. **Ask for `1`.**

### Fields that are genuinely fine

Everything else either agrees with x264 at zero or has no x264 equivalent.
`rc.qp`, `bframes`, `ref`, `keyint`, `aq_strength`, `psy_rd`, `psy_trellis`,
`cabac`, `transform8x8`, `sar_*` and the `vbv_*` pair all read zero the way an
x264 user expects. `keyint_min` 0 is auto in both. Three notes that are not
traps but will still surprise:

- **`param.threads` is a budget for one encoder instance**, not a width: 0 asks
 the library to pick (the machine's thread count, capped at 16), 1 is serial and
 N means up to N. A library caller who sets `threads = 8` gets a wavefront of 8.
 `frame_threads` is still the explicit low-level width and wins when set.
 That is what lets the CLI's GOP workers carry their own share.
- **`param.annexb` is read by nothing at all.** Annex-B is the only output mode.
- **`badapt` 2** (trellis B-frame placement) is silently narrowed rather than
 rejected: it becomes fast b-adapt.
- **`cqm` 2 is `YAH264_CQM_CUSTOM`** and reads the lists from `cqm_4x4[6][16]`
 (Intra Y, Cb, Cr, then Inter Y, Cb, Cr) and `cqm_8x8[2][64]` (Intra Y, Inter
 Y) at the end of the struct, in raster order, weights 1..255. A zero weight
 fails `yah264_encoder_open`.

## Environment variables

The encoder has about 120 `Y264_*` environment variables, listed in
[knobs.md](knobs.md), which is generated from the source. They are read only in
a build configured with `-Ddev_knobs=true`. A default build reads only the 16
the CLI uses to carry its own options (`src/common/env.h`) and the two memory
bounds below; no other variable changes its output, and the CLI refuses to
encode when one is set. Almost none of them are features. They are how this project tries
an experiment without a rebuild, and most are measurement scaffolding.

They are tiered below by whether you have any business setting them. A rule of
thumb: if it is not in the first table, the answer is no.

Shared semantics worth knowing before you set any of them:

- **Most are read once and cached** in a lazy static, warmed on the main thread
 at encoder open. Changing one mid-process does nothing. `Y264_MBTREE_STRENGTH`
 is read again on every mb-tree pass.
- **`YAH264_NO_ASM` is presence-only**, so setting it to `0` still turns it
 *on*.
- **Some are scaled.** `Y264_TP_IPF` and `Y264_TP_PBF` are percentages divided
 by 100.

### Tier 1: worth knowing about

Escape hatches, and knobs with no CLI equivalent. The rows promoted to a CLI
flag and the two memory bounds are read in every build; the rest need
`-Ddev_knobs=true`.

| Variable | Default | What it is for |
| --- | --- | --- |
| `YAH264_NO_ASM` | asm on | Force every scalar C path. Presence-only, so `=0` also disables asm. Promoted to `--no-asm`, and still overrides it. |
| `Y264_SIMD_FORCE` | unset | Cap the binary at one SIMD tier: `none`, `sse4`, `avx2`, `avx512`. It clears the CPU feature bits above the tier it names, so one binary runs the way the lower-tier build would have. Output does not move, because every kernel is bit-exact with its C reference. This is a speed axis and never a quality one. `none` is the twin of `YAH264_NO_ASM`. On aarch64 there is one tier and nothing above it to cap, so the three x86 names do nothing there. An unrecognized value changes nothing rather than dropping to C, so a typo cannot be read as a result. The build-time twin is meson's `-Dsimd=`, which leaves the tier out of the binary instead of routing past it. |
| `Y264_UMH_RANGE` | 16 | UMH search radius in integer pels. Promoted to `--merange`, and still overrides it. |
| `Y264_ABR_QCOMP` | 0.6 | The ABR rate curve's compression, and the mb-tree strength derived from it. Promoted to `--qcomp`, and still overrides it. |
| `Y264_AQ_DC` | 1.3 under uncapped CRF, 1.04 otherwise (capped CRF included) | The AQ frame-level strength, added to the CRF base QP as its rate-equivalent difference from the per-MB field's strength: the across-shot allocation term. Under one-pass ABR and capped VBR it feeds the rate model instead. CBR, two-pass and CQP don't use it. Fitted by sweep. |
| `Y264_DZ_INTRA` / `Y264_DZ_INTER` | unset | Quantizer rounding bias in 1/64 units. This is the encoder's own scale, **not** the CLI flag's, which is `32` minus this. Promoted to `--deadzone-intra`/`--deadzone-inter`, which do the inversion. Both still override. |
| `Y264_AQ_MODE` | 2 | 1 is plain log2-variance AQ, 2 and above is yah264's adaptive form (see `--aq-mode`). Promoted to `--aq-mode`, and still overrides it. |
| `Y264_FAST_PSKIP` | 1 (on) | The cheap P_Skip pre-test at `--subme` 8 and below. Promoted to `--no-fast-pskip`, and still overrides it. |
| `Y264_DCTDEC` | 1 (on) | Coefficient decimation. Promoted to `--no-dct-decimate`, and still overrides it. |
| `Y264_2PASS_MT` | on | 0 forces two-pass onto the serial path, reproducing the serial output exactly. |
| `Y264_MBTREE_OFF` | 0 (off) | Skip mb-tree entirely. Set it when comparing `--qp` runs against x264. Changes bits. Promoted to `--no-mbtree`, and still overrides it. |
| `Y264_MAX_INPUT_MB` | 50% of physical RAM | How much memory the input window may take, in MiB. It refuses rather than being OOM-killed. Raise it if you know the box can take it; lower it to test the refusal. |
| `Y264_STREAM_WINDOW` | `(--threads + 1) x --keyint` | Input frames held resident on the in-order reader (a pipe), overriding the default window. Below `2 x --keyint` the reader could block before a whole GOP is dispatchable, so that is the floor. Scheduling-only: the bitstream does not move with it. |
| `Y264_LA_BUF` | auto = `bframes+1` (4 at medium) | Overrides `--sync-lookahead`, **in both directions**: the auto default resolves a lead of `bframes+1` whenever there is a pool, so `=1` and `=0` REDUCE it and cost 1-6% of wall. Costs latency, and changes no bits **while the decoupled chain is on**, which is whenever the knob has any effect. |
| `Y264_CRF_CPLX` | 1 (on) | Behavior-matched CRF content adaptation, on by default. Narrows the equal-CRF spread against x264 from 100 points to 41, and improves 9 of 11 corpus clips at the tuned anchor, worst +1.54%. 0 turns it off. |
| `Y264_CRF_LEVEL` | 2.3 | The CRF base under mb-tree is `crf + level + 2.9 * log2(fps / 30)`, fitted against the reference CLI's bytes at equal `--crf`. This sets the level. |

Two you should know exist so you never set them:

| Variable | Why not |
| --- | --- |
| `Y264_UNSAFE_NO_REFBWAIT`, `Y264_UNSAFE_NO_PREVPWAIT` | Deliberately racy. They drop synchronization waits to measure a ceiling. The output is not trustworthy. |
| `Y264_UNSAFE_NO_EMIT`, `Y264_UNSAFE_NO_NAL` | Deliberately broken. They delete the entropy emit and the NAL assembly to price the emission path. The bitstream is invalid. The reconstruction is unaffected, which is the point. |

### Tier 2: calibration knobs

These exist so a constant can be swept against x264 without a rebuild. Their
defaults were measured. Moving one moves quality in ways that have usually
already been tested and rejected. They are listed so you know what you are
looking at if you find one in a script, not as a tuning surface.

**Motion and mode decision:** `Y264_DCTDEC`, `Y264_PART_EARLYTERM`,
`Y264_PROBE_TRELLIS`.

**Rate control:** `Y264_TP_IPF`, `Y264_TP_PBF`, `Y264_TP_CPLXBLUR`.

**mb-tree:** `Y264_MBTREE_STRENGTH`.

**Threading and scheduling** (all byte-identical unless noted): `Y264_W2`,
`Y264_STAIR_WIDE`.

[knobs.md](knobs.md) lists the rest by tier, with each one's default and
reader.

### Tier 3: measurement scaffolding, not features

Profiling counters, trace dumps, deliberately-racy ceiling probes, and gates on
experiments that were measured and rejected. Setting any of these gets you
diagnostics or a known-worse encode. Several change bits.

`Y264_THREAD_PROF`, `Y264_NTP_PROF`,
`Y264_LA_STAT`, `Y264_SKIP_ORACLE`, `Y264_ADME_LOG`,
`Y264_HEX_ORACLE`, `Y264_STAIR_BDEPTH`, `Y264_RCP_LAG`,
`Y264_UNSAFE_NO_REFBWAIT`, `Y264_UNSAFE_NO_PREVPWAIT`, `Y264_UNSAFE_NO_EMIT`,
`Y264_UNSAFE_NO_NAL`, `Y264_GOP_FORCE_G`, `Y264_GOP_FORCE_K`.

`Y264_SKIP_ORACLE` takes a **path**, not a boolean.

### Not read by the encoder

These appear in scripts and tests only, never in C: `Y264_CRF`,
`Y264_CRF_CACHE`, `Y264_STRESS_ABR`, `Y264_TRELLIS_PRINT`,
`YAH264_ARGS`, `YAH264_DESC`, `YAH264_ENC`, `YAH264_CONF_*`, and
`YAH264_VMAF_MODEL` (used by `scripts/vmaf.sh` and `scripts/bdcompare.py`).

Some names that look like environment variables are compile-time macros and
cannot be set from a shell: `Y264_STAIR_K`, `Y264_STAIR_HOPS`,
`Y264_MT_POOL_MIN`, `Y264_STAIR_LAG`, `Y264_LA_CAP_MAX`, `Y264_DPB_POOL_MAX`,
`Y264_BIT_DEPTH`.

### Comments that lie about their defaults

The comments beside the readers in `src/encoder/encoder.c` agree with the
code. One struct-field comment in `src/encoder/encoder.h` is still stale and
describes an era when the gate was off. Trust the reader, or the census in
[knobs.md](knobs.md), and not the header:

| Variable | Header comment claims | Code does |
| --- | --- | --- |
| `Y264_W2` | off by default | **on whenever there is a thread pool** |

## Rough edges

Things that will bite, listed because a reference that only lists what works is
not a reference.

- A value-taking flag as the final argument (`yah264 ... --qp`) is reported as
 an unknown argument rather than a missing value.
- `--threads` does nothing under `--dump-recon`, or under the two-pass fallbacks
 above. It warns when you asked for more than one.
- The parallel path's window is `(--threads + 1) x --keyint` frames, so a high
 thread count at the default keyint asks for a lot of memory on a short clip's
 behalf: 18 threads at `--keyint 250` is 4750 frames, 14 GiB at 1080p. Clip
 length does not matter, but that product does.
- `--level` warns rather than enforces.
