# yah264 options reference

Every option the encoder accepts, its default and how it interacts with the
others. Where this file and the code (`cli/yah264_cli.c`, `include/yah264.h`,
`src/encoder/params.c`) disagree, the code wins and this file is the bug.

The program is `y264`, installed with a `yah264` alias that `meson uninstall`
leaves behind, so remove `bin/yah264` by hand. Read the [rate control
guide](rate-control.md) before choosing between `--crf` and `--bitrate`.

## Contents

- [Invoking it](#invoking-it)
- [What the bare default is](#what-the-bare-default-is)
- [The option table](#the-option-table)
- [Presets](#presets)
- [Tunes](#tunes)
- [Which options change the bitstream
  shape](#which-options-change-the-bitstream-shape)
- [Threading, and what it does to your
  output](#threading-and-what-it-does-to-your-output)
- [x264 compatibility](#x264-compatibility)
- [Calling it from C](#calling-it-from-c)
- [Environment variables](#environment-variables)
- [Rough edges](#rough-edges)

## Invoking it

```sh
y264 --input-y4m <in.y4m|-> [-o <out.264|->] [options]
```

Input is Y4M or raw YUV, with no demuxer and no format sniffing. A Y4M header
supplies the width, height, frame rate and chroma format, and raw input takes
them from the command line. Pipe anything else through ffmpeg:

```sh
ffmpeg -i input.mp4 -f yuv4mpegpipe - | y264 --input-y4m - --crf 23 -o out.264
y264 --input-raw in.yuv --input-res 1920x1080 --fps 24 --crf 23 -o out.264
```

A wrong `--input-res` doesn't fail: it encodes garbage. `--input-csp`, `--fps`
and `--input-range` are refused with `--input-y4m` because they could silently
contradict the header.

Raw input can't carry the sample aspect ratio. A Y4M with `A128:117` writes it
into the VUI and the same frames as raw don't, so pass `--sar` to make them
agree byte for byte.

The Y4M `C` tag is parsed for `420`, `422` and `444`, plus `p10` forms. The
input's depth picks one of the binary's two libraries, 8-bit or 10-bit. That's
why a `C420p10` file is coded as High 10 unasked. Raw input names its depth
with the format (`--input-csp i420p10`) because nothing else on the command
line says 10-bit.

`--output-depth 10` shifts 8-bit samples up by 2 into the 10-bit encoder, from
Y4M or raw alike. `--output-depth 8` on 10-bit input is refused, because
dropping bits is a conversion and conversions are ffmpeg's job. A 16-bit sample
above the depth's maximum (1023 at 10 bits) ends the encode with exit status 1,
naming the value and frame.

Output is an Annex-B elementary stream, and `-` means stdin or stdout.

## What the bare default is

With no other flags, `y264 --input-y4m in.y4m -o out.264` runs the tool set of
`x264 --preset medium`, so that the two compare without argument lists: CABAC,
`--ref 3`, `--bframes 3`, the 8x8 transform, adaptive B, `--rc-lookahead 40`
and `--keyint 250`. Rate control differs: the default is constant QP 26, where
x264's bare default is CRF 23. Ask for `--crf` if you want constant quality.

`--psy-rd` defaults to 2.0, swept for this encoder, unlike x264's.
`--psy-trellis` defaults to x264 medium's 0.0.

## The option table

Defaults marked "preset" come from the [preset ladder](#presets). The bare
default is medium's row.

A value outside an option's domain, or a non-numeric one, exits 2 with a
message naming the domain, such as `y264: --ref expects 1..2147483647 (got
'0')`. The *form* of a number is lenient, so `--bitrate 800.0` (what
`str(float)` produces) works. But a non-integral value for an integer option is
refused, never truncated. An unknown enum name is refused the same way, whether
a typo or x264's unimplemented `--direct none`.

### Input and output

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--input-y4m` | path or `-` | this or `--input-raw` | Y4M input. `--input-raw` takes raw YUV instead. |
| `-o`, `--output` | path or `-` | `-` (stdout) | Annex-B output. It can't be the input, the recon file or the stats file under any name. See [Output and scratch files](#output-and-scratch-files). |
| `--input-raw` | path or `-` | | Raw YUV. Needs `--input-res`. |
| `--input-res` | `WxH` | | Raw input geometry. Required with `--input-raw`, refused with `--input-y4m`. |
| `--input-csp` | `i420`\|`i422`\|`i444`, each with an optional `p10` | `i420` | Raw input chroma format and sample depth, as in `i420p10`. The depth rides with the format as it does in a Y4M `C` tag, so there's no `--input-depth`. |
| `--fps` | `N`, `N/D` or a decimal | 25 | Raw input frame rate. `--fps 23.976` is read as 23976/1000. |
| `--input-range` | `full`\|`limited` | not signaled | VUI color range for raw input, the same field `--range` sets. |
| `--frames` | N | 0 = all | Stop after N input frames. |
| `--seek` | N | 0 | Drop the first N input frames. It seeks where the input allows and reads and discards where it doesn't, which lets a pipe work. |
| `--crop-rect` | `L,T,R,B` | off | Crop the input before encoding, in luma samples. Offsets must fall on a chroma sample and the result must be even in both axes. Anything else is refused, never rounded. |
| `-v`, `--verbose` | | | Log level debug, which adds one line naming what the command line resolved to. |
| `--quiet` | | | Log level warning: no informational lines. Warnings and errors still print. |
| `--log-level` | `error`\|`warning`\|`info`\|`debug` | `info` | The same dial. There's no `none`: an error prints at every level, because a run that fails in silence is worse than a noisy one. |
| `--no-progress` | | | No progress line. The line only appears when stderr is a terminal, never in a gate or a redirect. |
| `--output-depth` | 8 or 10 | the input's | Which encoder library codes the stream. 10 on 8-bit input shifts each sample up by 2 and writes High 10. 8 on 10-bit input is refused, never rounded. |
| `--dump-recon` | path | off | Write the encoder's reconstruction as Y4M, in display order, at the encode's depth. It forces the single-threaded path and warns if `--threads` asked for more. The same file checks as `-o` apply. |
| `--range` | `full` or `limited` | not signaled | VUI color range. The Y4M `XCOLORRANGE` tag sets it on its own. |
| `--colorprim`, `--transfer`, `--colormatrix` | H.273 code 0..255 or a name, below | not signaled | VUI color description. Signaling only: nothing in the encoder changes with them. |
| `--chromaloc` | 0..5 | not signaled | VUI chroma sample location. |
| `--version` | | | Print the version and exit. |
| `-h`, `--help` | | | Print usage and exit. `--longhelp` and `--fullhelp` print the same list, which already names every flag. |

The color names are the ones x264's CLI and ffmpeg use, and a value copied from
either works, in any case:

| Code | `--colorprim` | `--transfer` | `--colormatrix` |
| --- | --- | --- | --- |
| 0 | | | `GBR` |
| 1 | `bt709` | `bt709` | `bt709` |
| 2 | `undef` | `undef` | `undef` |
| 4 | `bt470m` | `bt470m`, `gamma22` | `fcc` |
| 5 | `bt470bg` | `bt470bg`, `gamma28` | `bt470bg` |
| 6 | `smpte170m` | `smpte170m` | `smpte170m` |
| 7 | `smpte240m` | `smpte240m` | `smpte240m` |
| 8 | `film` | `linear` | `YCgCo` |
| 9 | `bt2020` | `log100` | `bt2020nc` |
| 10 | `smpte428` | `log316` | `bt2020c` |
| 11 | `smpte431` | `iec61966-2-4` | `smpte2085` |
| 12 | `smpte432` | `bt1361e` | `chroma-derived-nc` |
| 13 | | `iec61966-2-1`, `srgb` | `chroma-derived-c` |
| 14 | | `bt2020-10` | `ICtCp` |
| 15 | | `bt2020-12` | `ipt-c2` |
| 16 | | `smpte2084`, `pq` | `ycgco-re` |
| 17 | | `smpte428` | `ycgco-ro` |
| 18 | | `arib-std-b67`, `hlg` | |
| 22 | `ebu3213`, `jedec-p22` | | |

`bt601` is accepted for all three as code 6. `bt2020` means `bt2020-10` as a
transfer and `bt2020nc` as a matrix. `unknown` and `unspecified` mean `undef`.
ffmpeg's own spellings (`bt2020_ncl`, `smpte428_1`, `iec61966_2_1`) work too.

A code that H.273 leaves reserved draws a warning. A reserved 0 for primaries
or transfer is written as 2 (unspecified), and any other is written as given.
`GBR` on 4:2:0 or 4:2:2 input is refused, because H.264 allows it only at
4:4:4. `YCgCo` there is legal but draws a warning, since it's rarely what was
meant. `--range` takes `full`, `pc` or `jpeg` and `limited`, `tv` or `mpeg`, in
any case.

### Output and scratch files

`-o`, `--dump-recon` and `--stats` refuse to name the input or each other by
any path: a symlink, another case, `/dev/stdout` or a redirected stream. The
check compares the open files. A terminal, socket or FIFO never matches, so
standard input and output may share a socket.

An existing output is truncated only once the encode starts. A descriptor's
file (`-`, `/dev/stdout` or `/dev/fd/N`, however spelled) never is: it's
written through the descriptor from where it stands, as `--input-y4m
/dev/stdin` reads. A run refused before its encode starts removes any output or
recon file it created and leaves an existing one untouched.

Pass 2 and 3 read the stats file more than once and refuse a FIFO or device.
Two-pass scratch files (pass 3's working copy, a threaded run's per-GOP files)
take the stats name plus a suffix of up to about 30 characters, which must fit
the system's path limit. They're removed when the run ends, and when one of
these signals ends it, including the ones a write to a closed pipe or past the
file-size limit raises:

- SIGINT, SIGTERM, SIGHUP, SIGQUIT
- SIGPIPE, SIGXFSZ (taken only by a two-pass run)
- SIGALRM, SIGXCPU, SIGVTALRM, SIGPROF
- SIGUSR1, SIGUSR2, SIGEMT, SIGSYS

The same signals remove an output or recon file a run made before its encode
started. SIGKILL and a crash (SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE,
SIGTRAP) leave the files, and so can a signal while a scratch file's create has
hung for over 3 seconds (a stuck network mount), because the run then ends
without waiting. A signal the run started with handled elsewhere (a profiler's
SIGPROF) or ignored is left alone.

### Rate control

The [rate control guide](rate-control.md) covers how to choose. The flags:

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `-q`, `--qp` | 0..51 | 26 | Constant QP. It's the mode you get if you name no other. |
| `-B`, `--bitrate` | kbit/s | off | Single-pass ABR to an average bitrate. |
| `--crf` | ~0..51 | off | Constant rate factor. Decimals are accepted, but see the staircase note in the rate control guide. |
| `--vbv-maxrate` | kbit/s | 0 = off | VBV peak rate. It needs `--vbv-bufsize`, and either alone does nothing. |
| `--vbv-bufsize` | kbit | 0 = off | VBV buffer size. |
| `--vbv-init` | float | full | Initial VBV occupancy: a fraction of `--vbv-bufsize` at or below 1, kbit above it. It matters only where the VBV actually limits the rate. |
| `--qpmin`, `--qpmax` | 0..51 | 0, 51 | Bounds on the coded QP, applied after the frame-type offsets. The base QP isn't bounded. `--qpmax` under 51 is refused with `--nal-hrd`, whose buffer may need the QPs above it. |
| `--crf-max` | 1..51 | off | Ceiling on the QP the VBV may raise a `--crf` frame to. It's refused with `--nal-hrd`. See [below](#--crf-max-buys-quality-with-the-buffer-model). |
| `--ratetol` | float, or `inf` | 1.0 | How far single-pass ABR may drift from its target before its correction answers, as a fraction of the bitrate. Smaller tracks the rate closer and swings the quality more, and `inf` takes the correction out. |
| `--qpstep` | 1..51 | 4 | Largest QP move between consecutive frames of one type, in single-pass ABR and in the two-pass allocator. |
| `-p`, `--pass` | 1, 2 or 3 | off | Multi-pass, paired with `--bitrate`. 1 writes stats, 2 reads them, and 3 reads them and writes them back for a further pass to refine against a real encode. |
| `--stats` | path | `yah264.stats` | Two-pass statistics file. See [Output and scratch files](#output-and-scratch-files). |
| `--aq-strength` | float | 0.4 rate-controlled, 0.0 at CQP | Variance adaptive quantization strength. 0 disables it. |
| `--aq-mode` | 1 or 2 | 2 | The AQ form: 1 is plain log2-variance and 2 is adaptive. `0` is refused, and AQ off is `--aq-strength 0`. |
| `--abr-model` | `default`, `rf` | `default` | ABR bit allocation without mb-tree (`--bframes 0 --rc-lookahead 0`). See [below](#--abr-model-rf-matters-only-without-mb-tree). |
| `--rc-lookahead` | frames | preset (40 at medium) | mb-tree propagation window, at most 64 frames. 0 turns the window off. |
| `--no-mbtree` | | mb-tree on | Skip mb-tree propagation entirely. Constant QP already skips it, as x264 does. |
| `--ipratio`, `--pbratio` | float | 1.4, 1.3 | Two-pass only: the offline allocator's I-to-P and P-to-B qscale factors, with x264's names, numbers and defaults. The single-pass modes anchor I and B their own way and never read them. |
| `--cplxblur` | float | 20 | Two-pass only: the width of the planner's complexity blur, a gaussian over neighboring frames' costs with sigma `--cplxblur`/2 frames. 0 turns it off. |

Under a VBV the buffer's own cap on the correction still applies, even at
`inf`. The default is byte-identical to no flag, and `Y264_ABR_TOL` still overrides it.

Run every pass at one thread or every pass at more, because one thread codes
with tools more threads don't use. A pass reading another kind's stats warns.

`--rc-lookahead` above 64 runs at 64 with a warning. With B frames and a
shorter window, the lookahead still buffers `bframes+2` frames, at most 9.

`--aq-mode 2` adds a per-frame gain from the spread of block energies, a raised
variance floor and a frame-level center on frames with no mb-tree field. `0` is
refused, unlike in x264, because the value only selects a metric and 0 would
encode as 1.

`--qblur` is accepted and ignored with a warning, since the planner has no
quantizer blur.

`--aq-strength` defaults to 0.4 under CRF, ABR and two-pass and 0.0 under
constant QP. An explicit value, `0` included, wins. 0.4 is this encoder's
measured value, where x264 medium uses 1.0. On seven CIF clips in VMAF v1 the
lowest value tried reads best:

| `--aq-strength` | BD-rate against 0.4 |
| --- | --- |
| 0.2 | -2.6% median, although stefan still loses +0.33% |
| 0.3 | -0.1% |
| 0.5, 0.6 | lose |

Why the best value sits so far below the reference's default is an open
question.

### `--crf-max` buys quality with the buffer model

Under `--crf` with a VBV, a frame the buffer can't afford has its QP raised
until it fits. `--crf-max` stops that raise at the QP you name, codes the frame
there anyway, and lets the buffer go negative: a quality floor, bought with the
thing protecting the buffer. It's refused without `--crf` and a VBV, since
nothing else raises the QP. It bounds only the raise: a frame the rate factor already put
above the ceiling keeps its QP.

The stream is then not VBV-conformant at the declared buffer. A receiver
enforcing the model will stall. Don't use it where the stream must conform, or
with `--nal-hrd`, whose declaration would be false.

`scripts/vbv_check.py --crf-max Q` simulates the buffer, reports the under-run
in kbit and as a share of the buffer, and exits 0, since the under-run is
expected. Without the flag it reads `UNDERFLOW` and exits 1, also correct. A
gate that skipped the checker here would miss an under-run that stopped being
bounded.

The ceiling applies to the coded QP after the frame-type offsets, as `--qpmax`
does. A ceiling on the base QP would let every B frame step over it: on foreman
at `--crf-max 34` that puts P frames at 34 and B frames at 37.

### `--abr-model rf` matters only without mb-tree

With mb-tree, which runs whenever there are B frames or a lookahead, ABR always
uses the rate-factor ledger: P frames get a self-normalizing rate factor. I and
B frames anchor to the running non-B QP track. `--abr-model` doesn't change
that. A one-thread foreman encode at 500 kbit/s is byte-identical under
`default` and `rf`, with or without `--no-mbtree`, which skips propagation but
keeps the ledger.

At `--bframes 0 --rc-lookahead 0` there's no mb-tree. There `default` uses the
per-frame model and `rf` keeps a rate factor on the blurred complexity. No
current measurement compares them. `rf` is also accepted under its older
spelling, `x264`.

### When the command line names two rate-control modes

`--qp`, `--bitrate` and `--crf` each name a mode. The last one wins, as in
x264. A warning names what the loser still does:

```
y264: warning: --crf 23 is dropped -- --bitrate 5000 came later and selects ABR (last rate-control flag wins, as in x264)
y264: warning: --qp 26 still sets only the base QP -- --crf 26 came later and selects CRF (last rate-control flag wins, as in x264)
```

A losing `--crf` or `--bitrate` is discarded, but `--qp` still seeds the base
QP (`rc.qp`) in any mode. `--bitrate X --crf Y` is CRF, while `--crf Y --qp Z`
and `--bitrate X --qp Z` are constant QP, each with the warning. Repeating the
*same* flag prints nothing.

`--pass` isn't in the contest. It's a stats round-trip targeting `--bitrate`.
Two-pass CRF isn't implemented. A `--crf` beside it is dropped with a warning.

### GOP structure

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `-I`, `--keyint` | N | 250 | Maximum frames between IDRs. |
| `-i`, `--min-keyint` | N or `auto` | 0 = auto (`keyint/10`) | A scene cut closer than this to the last keyframe isn't promoted to an IDR. It's clamped internally to `[1, keyint/2+1]`. |
| `--scenecut` | N | 40 | How eagerly scene cuts get keyframes. Higher inserts more, and `0` or any negative turns it off. |
| `--no-scenecut` | | | Same as `--scenecut 0`: only `--keyint` places IDRs. |
| `--open-gop` / `--no-open-gop` | | off | Keyframes after the first become non-IDR I pictures with a recovery_point SEI, which the B frames before them still reference across. See [the note below](#--open-gop). |
| `--intra-refresh` | | off | No keyframe after the first. A column of intra macroblocks sweeps the picture once every `--keyint` frames, in P frames only. See [the note below](#--intra-refresh). |
| `-b`, `--bframes` | N | preset (3) | Consecutive B frames between anchors, and 0 disables B entirely. Up to 16 are accepted. A run is capped at 7. |
| `--b-adapt` | N | 1 | Adaptive B placement. 0 codes a fixed cadence. |
| `--b-bias` | -100..100 | 0 | Leans adaptive B placement toward more B frames (positive) or fewer (negative). Ignored at `--b-adapt 0`. It's a tuning dial: see [`--b-bias`](#--b-bias-is-a-tuning-dial). |
| `--direct` | `auto`\|`spatial`\|`temporal` | `auto` | B direct MV derivation. `auto` lets each B slice pick by the running skippability score. |
| `-r`, `--ref` | N | preset (3) | P-frame list-0 reference count, clamped to 16. At subme 7 (medium) under rate control, a picture 3840 or more on its longer side gets 2 unless you pass it. |

`--bframes 2` or more turns on the B-pyramid, which changes
`max_num_ref_frames` and so the auto-selected level.

`--bframes 0` also turns mb-tree off unless the lookahead window is on. mb-tree
runs when there are B frames, or when `--rc-lookahead` is above 0 and the IPPP
path is enabled, as it is by default. So `--bframes 0 --rc-lookahead 0` changes
more than the frame types. It's a different rate-control workload from the
default.

#### `--open-gop`

Without it, every keyframe is an IDR: the decoded picture buffer is flushed,
the picture order count restarts. The B frames before the key in display order
are coded as non-reference P frames against the past alone, losing their
forward prediction.

With `--open-gop` the periodic keyframe is a non-IDR I picture with a
recovery_point SEI (`recovery_frame_cnt` 0, `exact_match_flag` 1). The DPB
survives, POC and FrameNum run straight through. The frames before the key stay
B frames referencing both the anchor behind it and the key. The first picture
is still an IDR, and so is a keyframe a `--plan` zone or a `--qpfile` I or K
forces, because the [engine interface](engine-interface.md) promises a forced
keyframe is addressable as its own segment.

Scene-cut keys become open too, and that's where the gain shows. At the default
`--keyint 250` the cadence rarely fires, so on most short encodes a cut is the
only key. Of the twelve band clips at 120 frames, nine are byte-identical with
and without the flag. The three with cuts move this much at the same CRF:

| Clip | Bytes moved |
| --- | --- |
| sintel_720p | 0.4% |
| coastguard_cif | 6-7% |
| samsung_720p | 11% |

To keep a cut as a hard boundary a packager can address as its own segment, use
`--cut-split`, which gives each shot its own encoder instance and keeps the cut
an IDR.

The recovery point's promise is exact and narrow. Decoding from that access
unit with only the parameter sets before it gives every picture from the key
on, in output order, exactly as a decode from the IDR would. The leading B
frames come out before the recovery point and aren't covered, so such a decoder
discards them. To keep the promise, no picture after the key references one
before it. The leading B frames are coded as non-reference pictures, since a
reference among them would head every later default list with the wrong picture
on a cold start. They wait in the DPB beside the reference window. The SPS
declares room for them. With no level forced, an encode needing more than
H.264's 16 frames is fitted to 16 as `--level` would fit it.

`scripts/recovery_check.py` tests this, and `make conformance` runs it. It asks
the decoder for every picture decoded and selects its own comparison set,
counting back from the last, because decoders differ about which pictures they
display after a mid-stream start. One suppresses the uncovered B frames, and
another also stops two short of the end. Neither moves a sample.

The cost is parallelism, not bits. With no GOP boundary to hand out, the encode
is one instance end to end and `--threads` goes to the row wavefront, though
under `--cut-split` the scene cuts still split, being IDRs. Without a readable
frame count, as from a pipe, it falls back to the serial path and says so when
`--threads` asked for more. `--open-gop` is refused with `--stitchable`, which
promises a cut point at every keyframe.

#### `--intra-refresh`

A keyframe is the biggest picture in a stream, often by an order of magnitude,
and on a low-latency link it arrives as a burst the buffer has to absorb.
`--intra-refresh` removes the keyframes after the first. In their place each P
frame codes a share of the picture's macroblock columns as intra, left to
right. The cost spreads evenly over the period. A full sweep refreshes every
column once.

- **The period is `--keyint`.** Sweep position *j*, from 0 to keyint-1, codes
  the columns from `j*W/keyint` up to `(j+1)*W/keyint`, W being the picture's
  width in macroblocks, so with keyint above W some frames refresh nothing. The
  first sweep starts on the frame after the opening IDR.
- **Each sweep opens with a recovery_point SEI**, with `exact_match_flag` 1
  and `recovery_frame_cnt` = keyint - 1. A decoder starting there with only
  the parameter sets has the encoder's reconstruction from the sweep's last
  frame on. FrameNum widens to hold the count, so keyint can be at most 65536.
- **Motion never reads what the sweep hasn't reached.** A macroblock left of
  the refresh column predicts only from references inside the current sweep,
  and only from their refreshed columns. Reads stop 3 luma samples short of a
  reference's refreshed edge, which the deblocking filter rewrites from the
  unrefreshed side. A fractional vector 3 samples earlier still, for the 6-tap
  filter. The bound is per block. It holds the skip vector and the quarter-pel
  refinement too. The last refreshed column doesn't use intra modes that read
  the unrefreshed above-right neighbor.
- **No picture references anything older than the frame before its sweep
  began**, the last one the previous sweep promised exact. A decoder that
  joined one sweep earlier stays exact.
- **Scene cuts don't place keyframes or restart the sweep**, which has already
  promised an exact picture at its end. `--scenecut` is ignored with a warning.
  A forced keyframe (a `--plan` or `--qpfile` IDR) is still an IDR, and the
  next sweep starts after it.
- **P frames only**, because direct prediction reads co-located motion that a
  mid-sweep decoder never decoded. Named B frames are refused. A preset's are
  dropped with a note.
- **One encoder instance**, with no GOP boundaries to split at. `--threads`
  runs the row wavefront and the lookahead. The CLI says so when it has threads
  to spare.
- **Refused** with `--open-gop`, `--cut-split`, `--shot-table`, `--shot-crf`,
  `--hw`, `--stitchable` and field coding (`--tff`/`--bff`).

Rate control and VBV run unchanged. `scripts/recovery_check.py` starts a decode
at each recovery point and compares the frames from the end of the sweep on
with the reconstruction. `make conformance` runs it on several streams, beside
the recon-match cells.

The table compares intra refresh with keyframes at the same period: `--keyint
60`, `--bframes 0`, a one-second VBV (`--bitrate R --vbv-maxrate R
--vbv-bufsize R`) and `--no-scenecut` on the keyframe side. Each encode is 180
frames at `--threads 1`, at four rates from half to 1.5 times the board rate.
Positive BD-rate means intra refresh needs more bits. Peak is the largest frame
after the IDR over the mean frame size, at the board rate.

| Clip | Board rate | BD-rate (VMAF v1) | Peak, intra refresh | Peak, keyframes |
| --- | --- | --- | --- | --- |
| foreman_cif | 400 | +15.4% | 2.5x | 6.6x |
| bus_cif | 400 | +3.7% | 2.1x | 6.7x |
| fourpeople_720p | 1200 | +54.5% | 3.1x | 19.2x |
| shields_720p | 2200 | +12.5% | 3.4x | 13.8x |
| park_joy_720p | 12000 | +3.6% | 1.4x | 5.0x |

The largest frame drops by a factor of 3 to 6, which is the point. The bit cost
tracks how static the picture is: about 4% on busy content, a lot on a static
video call like fourpeople, where a keyframe is coded once at a low QP and then
skipped through. Each refresh column is coded at the P frames' QP. The one
beside the sweep's edge can't predict its last 4 luma columns from where they
were. On fourpeople in CRF the cost is +40%, and removing the motion bound
(which breaks recovery) leaves most of it. Most of the cost is the intra
columns themselves. A lower QP on the refresh column made it worse at both
offsets tried, 3 and 6.

#### `--b-bias` is a tuning dial

Adaptive placement keeps a candidate frame as a B while its lowres cost (per
block, the cheapest of the forward leg, the backward leg and intra) stays under
0.77 of its intra cost. `--b-bias` moves that 0.77 linearly: +100 takes it to
1.0, where a frame becomes an anchor only if nothing beats intra, and -100 to
0, where every frame is an anchor. The B-run limit, the keyframe interval and
scene cuts don't move, and 0 is byte-identical to no flag.

Four board clips at 120 frames, `--threads 1`, CRF 23 to 35 in five rungs,
BD-rate on VMAF v1 against 0, B share read at CRF 29. At -50 four of the five
rungs overlap.

| clip | B share at 0 | at -50 | BD at -50 | at +50 and +100 |
|---|--:|--:|--:|---|
| stefan_cif | 73% | 26% | +12.8% | byte-identical to 0 |
| shields_720p | 74% | 1% | +51.0% | byte-identical to 0 |
| park_joy_720p | 74% | 0% | +24.5% | byte-identical to 0 |
| pedestrian_1080p | 74% | 0% | +10.1% | byte-identical to 0 |

shields' number is the least exact, because its top rung sits at VMAF v1 98.

The positive side does nothing here, because the default already fills every B
run to `--bframes` on all four clips, and at `--bframes 16` on stefan and
shields. The negative side is steep. stefan at `--bframes 16`:

| `--b-bias` | stefan B frames |
| --- | --- |
| 0 | 77 |
| -10 | 71 |
| -25 | 48 |
| -50 | nearly none |

Use it in small steps to trade B frames for decode simplicity or latency, and
expect -50 to cost 10 to 51%. It isn't a quality setting, and nothing here
suggests moving it from 0.

### Coding tools

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--preset` | name | `medium` | Speed/quality ladder. See [Presets](#presets). |
| `--tune` | name | off | Content tuning. See [Tunes](#tunes). |
| `--cabac` / `--cavlc` | | CABAC (preset) | Entropy coder. `ultrafast` sets CAVLC. |
| `--transform-8x8` / `--no-transform-8x8` | | on (preset) | 8x8 transform and 8x8 intra. On means High profile. |
| `--cqm` | `flat`\|`jvt` | `flat` | Quantization matrices. `jvt` writes the standard's default scaling lists into the SPS and forces High profile. |
| `--cqmfile` | file | off | Matrices from a JM-format file, overriding every other `--cqm*` option. Forces High. See [Custom quantization matrices](#custom-quantization-matrices). |
| `--cqm4`, `--cqm8` | 16 or 64 weights | flat | Set every 4x4 list, or both 8x8 lists, to one comma-separated list of weights 1..255 in raster order. |
| `--cqm4i`, `--cqm4p`, `--cqm8i`, `--cqm8p` | weights | flat | The intra or the inter lists, luma and chroma together. |
| `--cqm4iy`, `--cqm4ic`, `--cqm4py`, `--cqm4pc`, `--cqm8iy`, `--cqm8py` | weights | flat | One list each: `y` is luma and `c` both chroma planes. A list you don't name stays flat. |
| `-f`, `--deblock` | `A:B` | `-1:-1` when the longer side is 1280 or more, `0:0` below; `0:0` with psy off | Deblocking offsets, each -6..6, in the slice header's div2 units, so x264's numbers port unchanged. The decoder applies twice the value. |
| `--no-deblock` | | filter on | No in-loop deblocking at all: `disable_deblocking_filter_idc 1`. No filter runs. |
| `--b-pyramid` | `none`\|`normal` | `normal` | `none` codes a flat B run. `strict` isn't implemented and is refused. |
| `--no-weightb` / `--weightb` | | on | Clears (or restores) `weighted_bipred_idc`: plain averaging in B slices instead of implicit weights. |
| `--weightp` | 0\|1\|2 | 1 | Explicit P-slice weighted prediction: 0 off, 1 one weight per reference, 2 weighted or plain per macroblock. See [Weighted prediction](#weighted-prediction). |
| `--slices` | N | 1 | N independently decodable slices per picture, on macroblock rows. Costs bits; refused with `--hw`. See [Slices](#slices-and-constrained-intra). |
| `--constrained-intra` | | off | Intra macroblocks in P and B slices decode from intra data alone, buying error resilience with bits. |
| `--chroma-qp-offset` | -12..12 | by the longer side: -4 below 1280, -2 below 1920, 0 from 1920 up; 0 with psy off | PPS `chroma_qp_index_offset` and `second_chroma_qp_index_offset`, reaching the chroma quantizer and the deblock chroma edge QP. |
| `--mvrange` | luma samples | the level's | Vertical motion-vector range. Only a value tighter than the level's own Table A-1 bound has any effect, because the level is a conformance bound. |
| `--me` | `dia`\|`hex`\|`umh`\|`esa`\|`tesa` | auto from preset | Motion search: hex at medium and faster, UMH at slow and up. `esa` and `tesa` are exhaustive and [slow](#what-the-exhaustive-searches-cost). |
| `--no-mixed-refs` | | mixed | P partitions search only the reference the 16x16 search chose. Cheaper with several references, and costs bits. B macroblocks already take the 16x16's list-0 reference, so it changes P only. |
| `--no-chroma-me` | | | Accepted and ignored. The motion search here scores luma only. |
| `--psy-rd` | float | 2.0 | Psychovisual RD strength. 0 disables. |
| `--psy-trellis` | float | 0.0 | Psy-trellis strength. Around 1.0 suits grain. |
| `-t`, `--trellis` | 0..2 | 1 | RDOQ placement on x264's scale: 0 off, 1 the committed macroblock only, 2 every mode decision. |
| `-A`, `--partitions` | list | preset (see below) | Partition shapes the mode decision may try: `p8x8`, `p4x4`, `b8x8`, `i8x8`, `i4x4`, `none`, `all`. See [What `--partitions` gates](#what---partitions-gates). |
| `--p-part-gate` | lambdas | 400 | Skip the P splits where the 16x16 result costs under this many lambdas in a homogeneous neighborhood. See [the gates](#the-three-low-rate-decision-gates). |
| `--b-preme-skip` | 0..4 | 0 (off); 2 at medium when the longer side is 1920 or more | End a B macroblock at skip before its motion search when no coded mode could beat it. See [the gates](#the-three-low-rate-decision-gates). |
| `--rd-surv-rank` | count | 0 (off) | RD at most this many candidates per set in the B tournament, ranked by screening cost. 0 keeps the score threshold alone. |
| `--lr-settle` | SAD/pixel | preset (1 at `faster` and below, 0 from `fast` up) | End the lookahead's lowres block search at its predictor when that leaves under this much SAD per pixel. See [The lookahead's own fixed cost](#the-lookaheads-own-fixed-cost). |
| `--lr-subgate` | SATD/pixel | 0 (off) | Skip the lowres subpel refine when the whole-pel winner is already under this much SATD per lowres pixel. |
| `--mbt-depfloor` | 256ths | 0 (off) | Refuse the macroblock tree's deposit where the block's own propagation fraction is under this many 256ths. |
| `--b-intra-band` | 16ths | 0 (off) | Skip the B intra screen and trial in a row band whose lookahead intra cost exceeds this many 16ths of its inter cost. See [Band-level decisions](#band-level-decisions). |
| `--b8-band` | CoV^2 x100 | 0 (off) | Skip the B_8x8 quadrant gate and its eight searches in a row band whose lookahead pair-leg cost has a squared coefficient of variation under this many hundredths. |
| `-m`, `--subme` | 1..11 | preset (7 at medium) | Subpel and RD analysis level, on x264's scale. It also picks the search method. See [below](#where-a-same-named-flag-differs-from-x264). |
| `--subpel` | 0..2 | preset (2 at medium) | Refinement pattern: 0 square, 1 diamond, 2 capped diamond. No x264 equivalent. |
| `--merange` | pels | 16 | Search radius for `umh`, `esa` and `tesa`. `dia` and `hex` ignore it, so it does nothing at medium. |
| `--qcomp` | 0..1 | 0.6 | Rate-curve compression, reaching fewer curves than x264's. See [below](#where-a-same-named-flag-differs-from-x264). |
| `--cut-split` | | off | IDR on every scene cut the pre-scan finds, with the GOP workers split there. Each cut becomes a clean seek point. Read from a file, since a pipe is held in memory up to the limit ([Memory](#memory-the-parallel-path-streams-through-a-bounded-window)). |
| `--shot-table` | | off | Print the pre-scan's shot table as JSON on stderr. Implies `--cut-split`. |
| `--shot-crf` | | off | Each GOP takes its shot's own CRF. Implies `--cut-split`, ignored by `--pass 1`. See [Per-shot CRF](#per-shot-crf). |
| `--scan-out` | FILE | off | With `--cut-split` or `--shot-crf`, save the pre-scan for later encodes of the same input. See [the saved pre-scan](#the-saved-pre-scan---scan-out-and---scan-in). |
| `--scan-in` | FILE | off | With `--cut-split` or `--shot-crf`, use a saved pre-scan in place of running one, refused unless input and settings match. Output is byte-identical. See [the saved pre-scan](#the-saved-pre-scan---scan-out-and---scan-in). |
| `--plan` | FILE | off | The [engine interface](engine-interface.md)'s zones, one per line: `first last [idr] [qp+N\|qp-N]`, frames from 0, inclusive. `idr` forces a keyframe and GOP boundary at `first`. The offset applies on top of any rate control. |
| `--zones` | LIST | off | x264's inline zones, `q=` and `b=` only. See [`--zones`](#--zones). |
| `--qpfile` | FILE | off | Per-frame type and QP forces from a file. A force the encoder can't place is refused by frame number. See [below](#--qpfile-refuses-what-it-cannot-place). |
| `--gop-threads` | K | off | Fix every GOP instance's frame threads at K, so that a GOP re-encoded alone reproduces its bytes (`scripts/shot_determinism.sh`). |
| `--segment-out` | PATTERN | off | Also write each GOP, with its own parameter sets, to its own file (`%d` is the GOP index). Concatenated in order they equal the stream. Refused with `--nal-hrd`, and with `--keyint 1` under a VBV. |
| `--frame-stats` | FILE | off | One JSON line per coded frame, in coding order. Not under `--hw`. See [Per-frame stats and live control](#per-frame-stats-and-live-control). |
| `--control` | FILE | off | Live control events from a file, all checked before the first frame. See [live control](live-control.md). |
| `--control-fd` | N | off | The same events, read from descriptor N between frames as they arrive. |
| `--control-out` | FILE | off | Write every applied event as a `--control` log that replays the same stream at one thread. Needs `--control` or `--control-fd`. |
| `--deadzone-inter` | 0..32 | 21 | Inter luma deadzone, x264's flag and value. Passing it changes output even at 21. |
| `--deadzone-intra` | 0..32 | 11 | Intra luma quantization deadzone. |
| `--no-psy` | | psy on | `--psy-rd 0 --psy-trellis 0`, as x264 spells it. An explicit `--psy-rd` anywhere still wins, as over a `--tune`. |
| `--nr` / `--nr-intra` / `--nr-inter` | 0..2000 | 0 (off) | Coefficient noise reduction. `--nr` sets both block classes. The other two set one each. See [Noise reduction and the temporal pre-filter](#noise-reduction-and-the-temporal-pre-filter). |
| `--mcstf` / `--no-mcstf` | | off | Motion-compensated temporal pre-filter on the source, ahead of the lookahead. It's opt-in, for grain and sensor noise. |
| `--mcstf-range` | 1..4 | 1 | Neighbors per side that `--mcstf` averages over. 1 is the measured setting. |
| `--no-dct-decimate` | | decimation on | Never drop a block whose coefficients are all marginal. |
| `--no-fast-pskip` | | fast P-skip on | Drop the cheap P_Skip pre-test: every P macroblock is fully analyzed at `--subme` 8 and below. Slower. It moves bits. |
| `--no-asm` | | asm on | Force every scalar C path. Output is byte-identical, since every kernel is checkasm-equal to C. |

`--no-p-part-gate`, `--no-lr-settle`, `--no-b-intra-band` and `--no-b8-band`
set their option to 0.

#### Flags with an environment variable behind them

`--merange`, `--qcomp`, the deadzone pair, `--aq-mode`, `--no-mbtree`,
`--no-dct-decimate`, `--no-fast-pskip`, `--no-asm`, the ratio pair and the blur
pair set the `Y264_*` variable each was promoted from. If the environment
already holds a different value, it wins and stderr says so, so sweep scripts
that override arguments keep working and nothing beats a flag silently.
`--no-psy` has no variable and sets the psy fields directly.

#### Where a same-named flag differs from x264

- `--subme` also picks the search method when there's no `--me`: hex below 8,
  UMH from 8, so `--subme 8` changes the algorithm too. Pass `--me` to fix it.
  `--subme 0` is refused, because it's x264's fastest mode but this library's
  "unset". Unset means the slowest, 10.
- `--qcomp` reaches only the ABR curve and the mb-tree strength derived from
  it. The CRF and two-pass curves keep a fixed 0.6, where the reference
  documents one global curve compression.
- The deadzone flags change the output even at their defaults. Passing either
  swaps the exact expression (intra `step/3`, inter `step/6`) for its 1/64
  approximation and disables the NEON quant path, so `--deadzone-inter 21
  --deadzone-intra 11` measures +0.23% on foreman instead of 0. The 0-32 scale
  is the reference option's, so values carry over.

`--trellis` defaults to 1, as in x264. Level 0 quantizes with the plain
deadzone, 1 quantizes the trials with the deadzone and re-encodes the winner
with RDOQ, and 2 runs RDOQ in every mode decision.

### Custom quantization matrices

`--cqmfile` reads the JM text format: `INTRA4X4_LUMA = w,w,...`, and likewise
`INTRA4X4_CHROMAU`/`CHROMAV`, the three `INTER4X4_*` lists, `INTRA8X8_LUMA` and
`INTER8X8_LUMA`. Weights are 1..255 in raster order, separated by commas or
whitespace across any number of lines, and `#` starts a comment. A missing luma
list is flat; a missing Cb list takes that mode's luma list, and a missing Cr
list takes Cb. Every list is written into the SPS, so any decoder reproduces
it, and the quantizer, dequantizer and trellis all use them. The 4:4:4 chroma 8x8 keys are accepted and unused,
because 4:4:4 never picks the 8x8 transform here.

The per-list options start from flat, so that an unnamed list stays 16. They
refuse to combine with `--cqm jvt`. A set that's flat everywhere writes no
matrix and is byte-identical to `--cqm flat`. x264's `--cqm8ic`/`--cqm8pc`
(4:4:4 chroma 8x8) have no equivalent, for the same reason.

### What custom matrices cost

The standard's default lists are tuned for neither this encoder's rate control
nor VMAF. `--cqm jvt` against flat on five board clips: 120 frames, `--preset
medium`, CRF 23-35, one thread, BD-rate on VMAF v1 (negative is better).

| clip | BD-VMAF v1 | CPU time |
| --- | --- | --- |
| foreman_cif | +7.79% | 1.32x |
| bus_cif | +5.50% | 1.25x |
| stefan_cif | +8.68% | 1.21x |
| park_joy_720p | -2.47% | 1.20x |
| pedestrian_1080p | +11.05% (4 of 5 rungs) | 1.42x |

The CPU cost is the quantizer leaving its SIMD path, which takes only flat
lists. Any non-flat list runs the C weighted path.

### Weighted prediction

`--weightp` defaults to 1, so explicit P-slice weighted prediction is on unless
you turn it off.

- **0** clears `weighted_pred_flag`. Nothing weights. It's the one flag that
  can lower the declared profile, since Annex A.2.1 forbids the tool in
  Baseline: a CAVLC stream with no B frames and no 8x8 transform declares
  Baseline at `--weightp 0` and Main at 1.
- **1** codes one luma weight (a 1/64 step) and offset (a whole 8-bit code) per
  active list-0 reference, chosen together for the least expected error. Chroma
  weights stay identity.
- **2** adds a weighted second slot for a reference whose own slot stays plain,
  so the choice is per macroblock. It fires only where the frame-level estimate
  already did and the weight wins on most of the picture but not all. It's
  refused at `--ref 1` and with `--tff`/`--bff`.

The slope is the ratio of standard deviations on a textured picture and of
means on a flat one, measured from black when the picture has black in it, as
in a fade. Up to about 130/64 is sent as 127/64, and steeper isn't weighted. A
flat picture is weighted from a quarter-code mean shift, a textured one from
two codes.

Letterboxed or pillarboxed progressive pictures keep their bars' level through
a fade. When bars of about equal depth (within a macroblock row or so) lie on
opposite sides and reference 0's weight would move them, the list holds
reference 0 again, plain, behind the weighted copy, if it has room (under 16
entries). A macroblock whose bars predict better from the plain copy doesn't
take skip, and its mostly-bar partitions code against that copy. Unequal or
one-sided bars aren't covered. The slot is added at any `--ref`, 1 included,
and under `--weightp 2` only when that mode hasn't already duplicated another
reference. When it duplicated reference 0, the bars use that.

Mode 2 stays off because it gains nothing: within 0.05% of `--weightp 1` in
BD-VMAF v1 on two CIF fades (one to black, one a dissolve between scenes), and byte-identical on ten
of the twelve HD band clips.

### Slices and constrained intra

`--slices N` cuts each picture into N slices on macroblock-row boundaries,
clamped to the macroblock row count and to 256. Nothing predicts across a
slice's first row: the intra samples, the mode and motion-vector predictors,
the CAVLC nC, the CABAC contexts and the `mb_qp_delta` chain all stop there.
Because each slice is its own NAL, a decoder can resynchronize at any of them
and decode several at once. Every slice header writes
`disable_deblocking_filter_idc` 0, so that the picture is deblocked whole and
the edges don't show.

That first row codes without its neighbors above, and each slice pays a header
and an entropy-coder reset. At `--slices 4`, 120 frames, the same five CRF
rungs (23 to 35) on both sides (`scripts/hd_band.py`):

| clip | BD-VMAF v1 vs `--slices 1` |
| --- | --- |
| riverbed_1080p | +0.48% |
| touchdown_420 | +2.32% |
| pedestrian_1080p | +3.61% |
| sunflower_1080p | +4.00% |
| **median** | **+2.97%** |

The spread follows how much of the picture would have been skipped. Clause
8.4.1.1 gives P_Skip a zero vector wherever the neighbor above is unavailable.
A slice's first row then loses the skip whenever the picture moves. riverbed
(dense detail, little skip) pays least, and sunflower (a slow pan over a mostly
static frame) most. The cost is linear in the cuts. At CRF 32, in bytes:

| clip | encoder | 2 slices | 4 slices | 8 slices |
| --- | --- | --- | --- | --- |
| sunflower_1080p | yah264 | +2.96% | +6.81% | +14.84% |
| sunflower_1080p | an independent encoder | +0.51% | -0.36% | +2.09% |
| riverbed_1080p | yah264 | +0.26% | +0.70% | +1.47% |
| riverbed_1080p | an independent encoder | +0.18% | +0.61% | +1.52% |

On detailed clips the per-cut cost matches an independent encoder's. On
skip-heavy ones ours is several times that, because this encoder leans harder
on P_Skip than it has to. Closing that gap is separate work. Slices don't speed
up the encoder, since the row wavefront already parallelizes inside a picture,
so use them for loss resilience and decoder-side parallelism.

`--constrained-intra` sets the PPS `constrained_intra_pred_flag`, so intra
prediction in P and B slices treats inter neighbors as unavailable for both the
reference samples and the mode predictor. I_16x16 plane and the corner-reading
4x4 and 8x8 modes drop out wherever the above-left neighbor is inter. I slices
are unaffected.

### What the exhaustive searches cost

`esa` scores every whole-pel vector within `--merange` of the best starting
vector (zero, the predicted vector or a neighbor's) by SAD plus vector cost.
`tesa` then re-ranks its 16 cheapest by SATD (Hadamard) plus vector cost. Both
replace the pattern search for every partition and reference, and leave the
sub-pel stages alone. Against `--me umh` on the same five clips and ladder,
with the CPU time of one 120-frame encode at CRF 26:

| clip | esa BD-VMAF v1 | esa CPU | tesa BD-VMAF v1 | tesa CPU |
| --- | --- | --- | --- | --- |
| foreman_cif | -1.79% | 4.6x | -0.76% | 7.7x |
| bus_cif | +0.21% | 4.3x | -0.39% | 7.4x |
| stefan_cif | -0.47% | 4.4x | -0.62% | 7.9x |
| park_joy_720p | +1.90% | 3.8x | +2.83% | 6.2x |
| pedestrian_1080p | +5.30% | 2.9x | +5.59% | 4.5x |

Exhaustive isn't better, because the SAD-cheapest vector isn't the one that
codes best, and on the HD clips the pattern search's smoother field wins. The
modes exist for x264 compatibility and experiments, not quality.

### What `--partitions` gates

Each name gates a search, not a syntax element:

| Name | What the mode decision may try |
| --- | --- |
| `p8x8` | The P 16x8, 8x16 and 8x8 splits. Without it every P macroblock is 16x16 or skipped. |
| `p4x4` | The 8x4, 8x8 and 4x4 sub-splits inside a P 8x8 block. Needs `p8x8`, because without the 8x8 split there's no block to divide. |
| `b8x8` | The B 16x8, 8x16 and 8x8 splits, each quadrant with its own list choice. Without it every B macroblock is 16x16, bidirectional or direct. |
| `i8x8` | 8x8 intra prediction. Needs `--transform-8x8`, because only the 8x8 transform can code its residual. |
| `i4x4` | 4x4 intra prediction. Without it, and without `i8x8`, intra is I_16x16 only. |

A shape outside the list is never searched or coded, and the macroblock falls
back to its whole form (P and B 16x16, intra I_16x16). `none` and `all` replace
the list, so `p8x8,none` is empty. `p4x4` without `p8x8`, or `i8x8` without
`--transform-8x8`, is an error naming the rule. At subme 7 (medium) under rate control,
the default at 1920 or more on the longer side drops `p8x8` and `b8x8`, while a
list you give is kept.

Dropping a shape doesn't shrink the encode: the search gets cheaper and the
residual pays for the worse prediction. `none` is a floor to measure against,
not a setting to ship. Slow and faster (subme 8 and below) search no B 16x8 or
8x16, so `b8x8` gets the quadrant split only, while `--subme` 9 and up (slower,
veryslow, placebo) search both orientations.

### The three low-rate decision gates

At low bitrate most of a picture is skip. On a 1080p clip at its shipping rate,
38% of the B macroblocks that end as skip ran a full motion search first. Each
flag here ends one of those searches early, trading bits for speed. Each is
read in BD-VMAF v1 at five shared CRF rungs on twelve 720p and 1080p clips.
Only the cheapest is on.

`--p-part-gate` is that one. Where the 16x16 search already found an answer
cheaper than the threshold, it skips the splits. The threshold is in lambdas so
that it follows the operating point. Two lookahead interlocks must pass first:
the local 3x3 lowres-cost field isn't dispersed (dispersion marks a motion or
texture boundary, where one vector shouldn't be trusted), and the mb-tree
offset must not say other frames depend on the block. The gate saturates
against them, so raising 400 to 1200 buys almost nothing. It's inert at
`--subme` 9 and above. It's over medium's +0.2% per-clip bar on one or two
clips, and turning it off reads -0.32% raw on ducks and -0.46% on park_joy
against the control.

`--b-preme-skip` is the costliest one to leave off. No coded macroblock scores
below lambda times its own mb_type and cbp rate. A skip candidate whose
distortion is already under that floor is settled. The search and RD trials
after it only confirm it. The modes differ in which B slices may use it,
because a reference picture's errors travel:

| Mode | Where it runs |
| --- | --- |
| 0 | off |
| 1 | non-reference B slices |
| 2 | also the reference B slices a propagation guard admits |
| 3, 4 | modes 1 and 2 with the bound read as an absolute distortion instead of in lambdas |

It's off below a longer side of 1920, at any subme but 7 and under CQP, and
inert at `--subme` 9 and up. The bound is the problem below 1920: measured in
lambdas, it widens as the rate falls. The gate is then most generous where a
wrong skip costs most. In BD-VMAF v1 against off:

| Reading | Mode 1 | Mode 2 |
| --- | --- | --- |
| 720p clips, worst | +0.21% | +0.43% |
| 720p clips, median | -0.01% | -0.15% |
| sunflower_1080p, top of the ladder | no cost | no cost |
| sunflower_1080p, bottom rung | a third of a point lower on 3.4% more bytes | 0.42 points lower on 2.2% more bytes |
| sunflower_1080p, whole ladder | +0.92% | +0.86% |

Mode 1 sits at medium's +0.2% bar on 720p and mode 2 is over it. Sunflower is
the band's hardest 1080p clip. Modes 3 and 4 use an absolute bound that can't
widen. They move the damage between clips without removing it, and stay
reachable.

Medium runs mode 2 anyway at 1920 or more on the longer side, over the same bar
on sunflower_1080p (+0.86%), together with the partition default that drops the
8x8 inter sub-partitions there. On a 1440-frame window of a 1080p title at
`--threads 3`, the pair costs 1.1% at CRF and 1.3% at VBR in BD-VMAF v1.

`--rd-surv-rank` RDs only the top-ranked candidates of each B set and costs
+0.63% at the band median. Cutting by rank bounds what cutting by score could
buy, so that one number settles cutting by score as well.

### The lookahead's own fixed cost

Quality here is BD-VMAF v1 on the twelve-clip HD band at five shared CRF rungs.

The lookahead costs the same however cheap the residual, so at low-bitrate HD
it's a large share of the encode. Deleting the window and the tree takes 14.8%
of the instructions off one of two profiled 1080p cells at matched bytes, where
the reference encoder pays 0.6% for the same deletion.

`--lr-settle` came out of that. The lowres block search starts at a predictor
its searched neighbors and the previous field agree on. Where it leaves almost
no residual, the candidate list, hexagon, square refine and subpel diamonds add
nothing. The search is the inner loop of both the lookahead's field and the
macroblock tree's walk. Skipping those steps saves 3.5% of instructions at
threshold 2 and 1.2% at 1. Above 2 it saves nothing more.

Each arm is read per clip against its preset's own baseline, since half a
corpus can sit under a median. Medium and above keep a +0.2% per-clip bar,
presets below medium +0.5%. A near-null control arm shows the instrument's
resolution. At threshold 1:

| Preset | Bar | Worst clip | Median | Worst, referenced to the control | Control's own range |
| --- | --- | --- | --- | --- | --- |
| medium | +0.2% | +0.56% | +0.07% (mean +0.11%) | | -0.62% to +0.26% |
| fast | +0.5% | +0.73% | | | |
| faster | +0.5% | +0.65% | -0.02% | +0.81% | up to +0.69% |
| veryfast | +0.5% | +0.66% | +0.03% | +0.90% | up to +0.83% |

At medium six of twelve clips are ahead or level, although the worst, a 1080p
animation clip, is beyond anything the control does. That keeps it off there
and at `fast`. At `faster` and `veryfast` the control alone reaches about +0.7%
to +0.8% on single clips. The band can't resolve a 0.5% bar there. The setting
stays on until a tighter null can read it. `superfast` and `ultrafast` run no
lookahead window. At medium, a picture 3840 or more on its longer side gets 2
unless the option is given.

A cheaper lowres search doesn't pay: the plain from-zero diamond costs 9.4%
more instructions at matched bytes on both cells, because a worse field costs
more downstream than the search saves.

`--lr-subgate` reaches the same blocks as `--lr-settle` by another route, so on
top of it it saves two tenths of a percent of instructions, and alone it costs
+0.45% on its worst clip, over medium's bar.

`--mbt-depfloor` is a clean null. A tiny propagation fraction deposits too
little to survive rounding into a QP offset, so refusing it looked free. It is
free but saves nothing: at 8, 16 and 32 parts in 256 it reads within a quarter
of a percent of baseline on both cells. The tree's time goes to its motion
search, not its arithmetic.

### Band-level decisions

Quality here is BD-VMAF v1 at five shared CRF rungs.

A row band can decide too. When a band rule is armed, the frame builds a small
table at frame open from lookahead fields already final: each band's mean and
dispersion of lowres inter cost, mean lowres intra cost, and share of
macroblocks other frames depend on. That's one pass per frame, under a
hundredth of a percent of a 1080p encode. Unlike a lambda-scaled per-block
bound, which widens as the rate falls, a band verdict reads content, and the
same band is refused at every rate.

`--b-intra-band` has the most to gain. The B intra screen tries three or four
whole-macroblock intra predictions on every macroblock it reaches. The trial
behind it wins a few hundred in a million. Where the band's lookahead says
intra isn't competitive, neither runs. Refusing every band removes the whole
compartment, and on one low-rate 1080p cell changes the size by one byte in
1.31 MB. Quality is over six 720p and 1080p clips:

| Threshold | Instructions saved, two 1080p cells | Worst clip |
| --- | --- | --- |
| every band | 0.52%, 0.91% | +0.71%, a 1080p clip (median -0.04%) |
| 48 | 0.29%, 0.67% | +0.55% |
| 96 | 0.06%, 0.22% | +0.04% |

At 96 it stops firing on live action. With every band refused, the other two
1080p clips read +0.06% and +0.11%. A control arm that changed 0.02% of the
encode read -0.63% on one 720p clip. The corpus floor is that wide, and the
tighter thresholds sit inside it. It's off.

`--b8-band` is off too. The quadrant gate spends four 8x8 SATDs to decide eight
motion searches. A band rule deletes both where that gate doesn't already
decline. On a low-rate 1080p cell the gate refuses three quarters of the
macroblocks reaching it. The searches run on one in forty. A rule covering half
the paying macroblocks is worth 0.18% and 0.02% of instructions. It was the
control arm for the floor above.

The pre-macroblock skip verdict has no band flag. Priced offline from a
per-macroblock dump, its wrong skips at the bottom of a 1080p ladder run 11% to
17% across every decile of every band field. No band tracks the damage.

`Y264_MBT_DEPFLOOR` overrides its flag either way. `Y264_BAND_ROWS` sets the
band width in macroblock rows (2). `Y264_BAND_PRECOMP=1` moves the P
sub-partition gate's interlocks into the frame-open pass, byte-identical but
off, because the lambda test already screens out most macroblocks. All three
are read only in a dev_knobs build.

### Noise reduction and the temporal pre-filter

`--nr` moves every coefficient toward zero before quantization by an offset
learned per transform position from the running coefficient energy, except on
pictures that read clean on a spatial noise gate. `--nr-inter 100` is the
measured setting, neutral to better on the HD band and best on noisy live
action. It's off because it doesn't help on film grain.

`--mcstf` replaces every fourth picture (the anchor spacing at `--bframes 3`)
with a weighted average of itself and its motion-compensated neighbors, which
everything downstream sees. A temporal-noise gate skips clean pictures, so
animation and other clean content stay byte-identical. It costs 10-13% of the instructions. In BD-VMAF
v1:

| Content | `--mcstf` |
| --- | --- |
| grain excerpts | up to -8.0% |
| touchdown | -5.7% |
| foreman_cif | +4.1% |
| riverbed_1080p | +4.2% |

[Denoising](denoise.md) has the full numbers for both.

### Per-shot CRF

`--shot-crf` gives each shot the CRF `crf + 6(1-qcomp) log2(C_shot / C_title)`,
where C is the shot's mean lowres inter cost, clamped to +-4 QP. Shots under 24
frames merge into their predecessor. Each GOP takes its shot's CRF. On ten
1080p titles it wins on 9 in VMAF v1. PSNR-Y averaged over frames agrees, while
PSNR-Y pooled over the clip reads the other way and wins on one.

| Test | Against | BD-VMAF v1 |
| --- | --- | --- |
| ten 1080p titles (three short segments, seven 10-minute excerpts) | plain CRF | -3.6% median |
| three short multi-shot sequences (CIF, 720p, 1080p, `--threads 2`) | the default | -8 to -12% |
| the same three | `--cut-split` alone | -5 to -10% |

### `--zones`

`--zones` takes x264's inline zones, `start,end,option` joined by `/`, with
frames from 0 in input order, inclusive. One option per zone. `q=N` codes the
range at base QP N (0..51) like `--qp N`: I frames take N-3, B frames the
cascade on top, and neither the rate control nor the VBV moves it. `b=F`
multiplies the range's bitrate by F as a QP offset of -6 log2(F) on what the
rate control chose, so `b=0.5` is `qp+6` in a `--plan` file. Other options,
overlapping zones, and `--plan` on the same line are refused.

### `--qpfile` refuses what it cannot place

`--qpfile` takes one line per frame, `<frame> <type> <qp>`, frames from 0 in
input order. Type `I` or `K` forces an IDR (under `--open-gop` too), `P` an
anchor, `B` or `b` a B frame, and `-` leaves the type alone. The QP is absolute
(0..51), or `-1` for the rate control's. Unnamed frames are free.

A line the encoder can't honor fails the encode with its frame number, never
coded as something close that the caller can't detect. Under the GOP splitter
the number counts from the start of that GOP. A second line names the GOP.
Refused forces:

- `B` on frame 0, which is a keyframe.
- `B` on a frame the keyframe interval claimed, or on the first frame of a GOP
  split. A type force doesn't move a keyframe.
- `B` on the last frame, with nothing after it. A forced keyframe close to an
  existing one cuts a short GOP whose coder sees its last frame as the last
  frame. This comes up more than it looks.
- `B` past `--bframes`. A sparse force joins whatever B run the lookahead
  started. One four frames after a keyframe at `--bframes 3` finds the run
  full. Name the whole type sequence instead.
- `i`, a non-IDR I. `--open-gop` makes the other keyframes non-IDR itself, and
  `I` and `K` both mean IDR.

A forced `P` on a keyframe's slot yields to the keyframe, an anchor that gives
more than was asked. `b` codes a B slice. But the B-pyramid still decides
whether it's a reference, so `b` and `B` are the same request: there's no
non-reference slot to put one in.

The forced QP is final. The frame-type cascade, a `--plan` offset and the VBV
all come before it. The buffer can't answer for a forced frame. Per-macroblock
AQ offsets still apply, as on a `--qp` encode. `--frame-stats` reports the
slice QP, the number the file names.

### Per-frame stats and live control

`--frame-stats` writes one JSON line per coded frame in coding order: frame,
gop, type, idr, ref, qp, bytes and k, plus `control` where a live control event
took effect. `bytes` covers all the frame's slice NAL units, start codes
included. It's written on every path, `--dump-recon` included, where `gop` is
0. Under field coding (`--tff`/`--bff` or an interlaced Y4M), `bytes` is both
fields'; type, idr, ref and qp are the first field's. A `fields` array gives
each field's slice type and bytes. An IDR frame's second field shows as P.

`--control` takes one event per line, `<frame> <kind> [name=value ...]`, frames
from 0 in input order and not decreasing:

| Kind | Fields |
| --- | --- |
| `rate` | `bitrate=` `maxrate=` `bufsize=` |
| `crf` | `value=` |
| `qp` | `min=` `max=` |
| `key` | |
| `zone` | `last=` `offset=` |

The whole file is checked against the encoder before the first frame. A bad
line stops the run with its line number and the output untouched. Live control
runs one encoder instance, its threads on the row wavefront and the lookahead.
It refuses `--segment-out`, `--cut-split`, `--shot-table`, `--shot-crf`,
`--scan-in`, `--scan-out` and `--gop-threads`. It doesn't run under `--hw` or
`--pass`.

`--control-fd` reads the same lines from descriptor N between frames, up to 64
before each. A frame may be `next`. A line for a frame already handed in, or a
refused event, is skipped with a warning. A malformed line or an unreadable
descriptor stops the encode. `--control-out` logs every applied event under the
frame it named, in frame order and then in arrival order.

### Stream metadata

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--sar` | `W:H` | unspecified (square) | Sample aspect ratio, as `16:11` or `16/11`. |
| `--level` | e.g. `3.1` or `31` | auto | Force an H.264 level, 1.0 to 6.2. The DPB is fitted to it (see below). |
| `--no-sei` | | SEI on | Drop the user-data SEI at the start of the stream that names the encoder, its version and the options in force. |
| `--profile` | `baseline`\|`main`\|`high`\|`high10`\|`high422`\|`high444` | derived | Constrain the tool set to a profile and write its `profile_idc`. See below. |
| `--aud` | | off | An access unit delimiter (NAL type 9) opens every access unit, ahead of the parameter sets. |
| `--pic-struct` | | off | VUI `pic_struct_present_flag` plus a `pic_timing` SEI per picture: 0 for a frame, 1 for a top field, 2 for a bottom one. |
| `--nal-hrd` | `none`\|`vbr`\|`cbr` | `none` | Write the buffer model into the stream and hold the encode to it. Needs `--vbv-maxrate`, `--vbv-bufsize` and a frame rate. See [`--nal-hrd`](#--nal-hrd-holds-the-buffer-it-declares). |
| `--filler` / `--no-filler` | | follows `--nal-hrd` | Pad access units to the constant rate with filler NAL units (type 12). On under `--nal-hrd cbr` and refused anywhere else. |
| `--frame-packing` | 0..7 | off | `frame_packing_arrangement` SEI, written once, "until canceled": 0 checkerboard, 1 column, 2 row, 3 side-by-side, 4 top-bottom, 5 frame alternation, 6 2D, 7 tile. |
| `--cll` | `MAX,AVG` | off | Content light level SEI (MaxCLL, MaxFALL) in cd/m^2, each 0..65535. `0,0` writes no SEI. |
| `--mastering-display` | `G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)` | off | Mastering display color volume SEI, chromaticity in 0.00002 units and luminance in 0.0001 cd/m^2. The order is the spec's G, B, R, not the usual R, G, B. |
| `--alternative-transfer` | H.273 code or a `--transfer` name | off | `alternative_transfer_characteristics` SEI: the transfer a display should prefer over the VUI's. `undef`, `unspecified` and 2 write no SEI. |
| `--overscan` | `undef`\|`show`\|`crop` | `undef` | VUI `overscan_info`. `undef` writes nothing. |
| `--videoformat` | `component`\|`pal`\|`ntsc`\|`secam`\|`mac`\|`undef` | `undef` | VUI `video_format`. Naming it opens the `video_signal_type` block, with the three color codes at 2 (unspecified) if not given. |
| `--stitchable` | | off | Size the declared DPB from the level and fix the FrameNum width, so two encodes at the same geometry write the same SPS. It also moves bits; see below. |
| `--fake-interlaced` | | off | Declare a sequence that may hold field pictures (`frame_mbs_only_flag` 0) while coding only frame pictures. |
| `--tff` | | off (see below) | Code each frame as two field pictures, top field first. |
| `--bff` | | off (see below) | The same, bottom field first. |
| `--no-interlaced` | | | Code an interlaced source as frame pictures, ignoring the Y4M's field order. |
| `--sps-id` | 0..31 | 0 | `seq_parameter_set_id`, written by the SPS and named by the PPS. |

`--cll`, `--mastering-display` and `--alternative-transfer` are written in-band
ahead of every IDR and every recovery point (`--open-gop`, `--intra-refresh`),
because each holds only until the next. `--cll` refuses MaxFALL above MaxCLL
unless MaxCLL is 0 (unknown). `--mastering-display` takes `L(0,0)` as unknown
luminance. It refuses a chromaticity above 50000 (1.0), a luminance wider than
32 bits, or a min at or above a real peak. These draw a warning and are written
as given:

- a chromaticity outside the SEI's range (x 5..37000, y 5..42000)
- a peak above 10000 cd/m^2, or a min above 5 cd/m^2
- a nonzero peak under 5, which is usually nits typed in place of 0.0001 units

#### `--nal-hrd` holds the buffer it declares

`--nal-hrd` writes `hrd_parameters` in the VUI, a `buffering_period` SEI at
every IDR, and a `pic_timing` SEI with `cpb_removal_delay` and `dpb_output_delay`
on every picture. It's refused without `--vbv-maxrate`, `--vbv-bufsize` and a
frame rate.

Every IDR opens a buffering period, scene cuts included. The CLI therefore runs
one encoder instance with its threads on the row wavefront. That rules out
`--cut-split`, `--shot-table`, `--shot-crf` and `--segment-out`. It also
refuses `--crf-max`, `--qpmax` under 51, forced qpfile QPs and `--zones q=`,
which hold a QP under what the buffer may need.

`vbr` declares a schedule the channel may pause on. `cbr` declares one that
never pauses, so filler pads every access unit up to the rate. The buffer must
be larger than one picture period of arrival (a field's under `--tff`/`--bff`).
`--no-filler` under `cbr` writes the CBR headers over an unpadded stream, whose
buffer a conforming decoder may overflow. Use it only as a negative control.

Under either mode, each picture is re-coded up to the QP ceiling until it
arrives in time on the declared schedule, leaving room for the next key the
lookahead shows, and after a large raise the next frame of that type starts
from the raised QP. An inter picture at QP 51 skips every macroblock it can if
it's still late, or if it would make a coming key late and costs over twice the
per-picture arrival alone. The rate control's own model never skips one.
Pictures still late, keys included since their room is estimated, are counted
in a warning at the end.

Pass 1 of a two-pass encode isn't held. But its stream still has the HRD SEIs,
so send it to `/dev/null`. See [what VBV
guarantees](rate-control.md#what-vbv-actually-guarantees).

#### Profiles

With no `--profile`, the profile is derived from the tools and the content:

| Condition | profile_idc |
| --- | --- |
| CABAC or B frames, or neither | 77 (Main) |
| 8x8 transform on, or any quant matrix but flat | 100 (High) |
| 10-bit input, or `--output-depth 10` | 110 (High 10) |
| 4:2:2 input | 122 (High 4:2:2) |
| 4:4:4 input | 244 (High 4:4:4) |

The derivation never picks Baseline, even at `--cavlc --bframes 0
--no-transform-8x8`, because explicit P weighted prediction, on at the default `--weightp 1`, is
signaled in the PPS and A.2.1 forbids it in Baseline.

`--profile` is a constraint. What it does depends on where a conflicting tool
came from:

- A tool the preset chose is narrowed in silence, so `--preset medium --profile
  baseline` works.
- A tool you named is refused: `--profile main --transform-8x8` exits 2.
- A profile the content can't fit is refused: `--profile high` on 4:2:2 input,
  or anything below `high10` on a 10-bit build. `high10` is refused on an 8-bit
  build.

`--profile baseline` also clears `weighted_pred_flag` and
`weighted_bipred_idc`. Without that, 66 couldn't be reached at all.

### `--stitchable` pins the SPS, and charges for it

Two separately encoded clips can be concatenated only if the second SPS is
byte-for-byte the first, or the decoder re-initializes. This flag fixes the
three SPS fields that moved with the settings:

- `max_num_ref_frames`, `max_dec_frame_buffering` and `max_num_reorder_frames`
  become the level's maximum, capped at 15.
- `log2_max_frame_num_minus4` becomes 4. Without the flag it's 0 at `--ref 1
  --bframes 0`.
- The level is picked for that fixed 15-frame DPB. It then depends only on
  frame size, frame rate and rate cap.

That level is the charge. A higher level allows a wider `MaxVmvR`. The search
is clamped to the level, so `--stitchable` is a different encode, not only a
different header.

The PPS still moves: `weighted_bipred_idc` follows `--bframes` and
`num_ref_idx_l0_default_active_minus1` follows `--ref`. Fixing it would cost
`num_ref_idx_active_override_flag` bits on every slice, so streams to be
stitched should pass the same `--ref` and `--bframes`. Fixing the PPS belongs
with the rest of the stitching bundle.

### What `--fake-interlaced` actually changes

It sets `frame_mbs_only_flag` 0 while every picture is still coded as a frame,
with a `field_pic_flag` of 0 in each slice header. Only the SPS geometry and
one bit per slice header move. `height_in_map_units` halves, an
`mb_adaptive_frame_field_flag` of 0 appears. The vertical crop counts in double
units. A coded height of an odd number of macroblock rows, such as 720p's 45,
is padded by one row and cropped away. Because of the double-unit crop, the
flag refuses any height that isn't a multiple of 4.

A named profile the stream was checked against is asserted: profile_idc 66 sets
`constraint_set0_flag`, and `--profile main` adds `constraint_set1_flag`. A
derived profile asserts nothing new.

### Field coding: `--tff` and `--bff`

Interlaced sources are coded as field pictures (PAFF). MBAFF isn't supported
and won't be ([what we don't do](what-we-dont-do.md)).

An interlaced frame is two half-height pictures taken a field period apart, and
coding them as one frame asks the transform to model a comb. `--tff` and
`--bff` code two field pictures per input frame in the named order, each with
its own slice header, picture order count and reference list. The sequence
declares `frame_mbs_only_flag` 0 and every slice a `field_pic_flag` of 1.

A Y4M tagged `It` or `Ib` turns this on by itself. `--tff`/`--bff` override the
tag, and `--no-interlaced` refuses to field-code at all. Raw input needs the
flag.

Following the standard, a field picture uses the field scan, reads CABAC
significance contexts from the second model set, drops an intra horizontal
macroblock edge from deblocking strength 4 to 3 and halves the vertical motion
threshold. The declared vertical motion range halves too.

- **What's coded.** I, P and B fields, CAVLC and CABAC, 4:2:0, with
  `--bframes`, `--b-pyramid`, both `--direct` modes and `--slices`. A named
  `--tff`/`--bff` refuses 4:2:2, 4:4:4, `--hw` and a height that isn't a
  multiple of 4. When only the Y4M tag asked, the encode codes frames instead,
  with one line on stderr, since a tag describes the input and isn't a request.
- **Reference lists** name both parities in the standard's order: the nearest
  frame's same-parity field, then its opposite one, then the frame before's,
  and so on. `--ref` still counts frames. A list holds up to twice the entries
  and the search roughly doubles. The slice header names each entry explicitly.
- **An I frame's second field is a P field** predicting from the first. A key
  costs one intra field, still quantized as intra. It's the one place a field
  reads the opposite parity, which needs the cross-parity chroma correction of
  8.4.1.4, because the parities site chroma a quarter sample apart.
- **Rate control and the lookahead stay frame-based.** A frame's type applies
  to its pair. Each field gets half the frame's bit target and VBV credit.
- **The coded height** is an even number of macroblock rows, padded and cropped
  as under `--fake-interlaced`, and must be a multiple of 4: refused under
  `--tff`, frame-coded under a bare `It` tag, always refused under
  `--fake-interlaced`. Broadcast heights (480, 576, 1080) all fit.

Three speed gaps remain, none moving a sample:

- Field motion search runs without the cached half-pel planes, because those
  are built over the frame and describe no field.
- The residual RD model prices a field block in frame-scan order against the
  frame context models.
- The two features that overlap a mini-GOP's B frames with their anchor don't
  run for fields. One sizes its budget in rows of a single coded picture, and
  the other gives each leaf a private reconstruction that a field pair's two
  pictures share.

`--dump-recon` writes woven frames with the field order in the Y4M `I` tag,
which is what a decoder outputs and the conformance gate compares.

The only cost figures come from an older field coder (I and P fields, a
same-parity list, both I fields intra) and haven't been re-measured. At QP 26
with CABAC and the 8x8 transform, on two synthetic fixtures:

| Fixture | Field coding | Frame coding |
| --- | --- | --- |
| testsrc2 | 69.1 kB at 45.44 dB Y | 55.3 kB at 45.31 dB |
| interlaced foreman | 103.6 kB at 37.43 dB | 91.0 kB at 37.00 dB |

Both are progressive sources through `tinterlace`, whose fields correlate
vertically, which favors frame coding. Real interlaced capture hasn't been
measured. Use `--tff` for interlaced sources that need a field-coded stream,
not to save bits.

### `--level` fits the DPB

`--level` takes a Table A-1 level (`3.1`, `31` or `3`). Anything else,
including `1b`, is refused. When the frames the encode keeps would overflow the
level's DPB at this frame size (at most 15 frames), the encoder gives things up
in this order, only as far as it has to:

1. the extra B-frame slots the threaded pipeline reserves, which turns that
   overlap off
2. list-0 references, down to `--ref 1`
3. B frames under the pyramid, down to two
4. the pyramid itself, with `--bframes` back as asked
5. B frames, down to none

References then come back as far as the room allows. One line names everything
that changed, an explicit `--ref` or `--bframes` included. `--level 5.1` on a
4K encode keeps medium's three references and only narrows the window, while
`--level 4.1` on 1080p also drops to two. A frame larger than the level's whole
DPB, or field coding at a one-frame DPB, is refused. Under `--stitchable` the
fit runs the same way. The SPS declares the level's DPB, at most 15 frames.

Frame size, frame rate and bitrate aren't fitted. A level below their
conformant minimum is accepted with a warning. The auto level follows frame
size, frame rate and DPB size per Annex A.

Default 4K streams declare Level 6.0, because the threaded pipeline's wide
window keeps more frames than Level 5.1 holds at that size. For TV and other
consumer devices, pass `--level 5.1`, or 5.2 at 50 or 60 fps, at some speed
cost on threaded encodes.

### Threading

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--threads` | N or `auto` | 0 = auto: online cores, at most 16 | Total thread budget, split across GOP workers and in-frame wavefront threads. |
| `--max-memory` | MiB | half of physical RAM | Memory budget for the threaded encode. When the GOP workers wouldn't fit, fewer run at once, each with a wider wavefront. See [Memory](#memory-the-parallel-path-streams-through-a-bounded-window). |
| `--sync-lookahead` | frames or `auto` | auto | Input frames buffered so the lookahead runs ahead on its own thread. `0` disables. A negative count is refused. |

`--sync-lookahead` costs exactly that many frames of latency and changes no
bits, since every lookahead decision reads only the `--rc-lookahead` window.
Auto resolves to `bframes+1` once the frame-thread pool is wide enough to run a
lookahead chain against, and to 0 otherwise. With B frames and a window shorter
than `bframes+2`, even a lead of 0 buffers `bframes+2` frames, at most 9, so
that each B gets its anchor's motion. At 1080p with `--rc-lookahead 1` that's
about 21 MB at one thread and 47 MB at four. The CLI prints the resolved lead
and its latency in milliseconds. `--tune zerolatency` turns it off.

There's no `--frame-threads` flag. The wavefront share comes from `--threads`.

## Presets

The ladder sets the columns below. Everything else is preset-independent.

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

Every column but lr-settle buys quality with time. lr-settle gives a little
back for speed. It has to be argued against a bar: +0.2% per clip at medium and
above, +0.5% below. `superfast` and `ultrafast` run no lookahead window, so
their value changes no byte. It's written so the column never turns back on
down the ladder.

Medium (subme 7 under rate control) has four more defaults that follow the
picture's longer side. From 1920, auto partitions drop `p8x8` and `b8x8` and
`--b-preme-skip` runs mode 2. From 3840, `--ref` 3 becomes 2 and an lr-settle
of 0 becomes 2. An explicit value wins, and for partitions that means any list
you give.

subpel 2 is a capped diamond, and -1 an 8-neighbor square iterated to
convergence. subme 8 and below uses the fast SATD partition path, and 9 and
above full RD per partition.

The partition column follows the resolved configuration, and `--log-level
debug` prints it. The sub-8x8 P shapes come with the full-RD tier at subme 8,
so `--subme 9` on a fast preset searches them, and ultrafast drops 8x8 intra
with the 8x8 transform. `all` adds only `p4x4` at medium and nothing at slow
and above. At ultrafast it's an error, since `i8x8` has no transform: spell the
list or add `--transform-8x8`.

With no `--me`, subme below 8 runs hex and 8 and above runs UMH, so `--preset
slow` changes the search algorithm as well as its effort.

Explicit flags override the preset regardless of order: `--ref`, `--bframes`,
`--rc-lookahead`, `--cabac`/`--cavlc`, `--transform-8x8`/`--no-transform-8x8`,
`--me` and `--partitions`. An unknown preset name is an error.

## Tunes

`--tune` sets content-adaptive defaults. An explicit `--psy-rd` or
`--psy-trellis` overrides whatever the tune chose.

| Tune | Effect |
| --- | --- |
| `grain` | psy-trellis 1.0, and psy-rd 1.5 in place of the 2.0 default. |
| `film` | psy-trellis 0.5, with psy-rd at the 2.0 default. Not BD-measured, for want of a film clip. |
| `animation` | 4 more B frames, only when `--bframes` isn't given and the preset's count is under 8. psy-trellis and AQ stay at the preset's values, because changing them measured as a coin flip on two cartoon clips. |
| `psnr` | psy-rd 0, psy-trellis 0, aq-strength 0. With psy off, the size-based chroma QP and deblocking offsets stay at 0. |
| `ssim` | psy-rd 0, psy-trellis 0, AQ kept. With psy off, the size-based chroma QP and deblocking offsets stay at 0. |
| `zerolatency` | bframes 0, rc-lookahead 0, sync-lookahead off. |
| `stillimage` | psy-trellis 0.7, aq-strength 1.2, deblock -3:-3: the reference tune's constants, not measured here, because the corpus has no still-image clip. |
| `fastdecode` | No deblocking, CAVLC, no weighted biprediction and no explicit P weighted prediction, at a deliberate cost in quality. With `--bframes 0 --no-transform-8x8` it declares Baseline without naming a profile. |

`grain`'s psy-rd 1.5 reads a median -0.2% BD-VMAF v1 against 2.0 on four grain
excerpts and touchdown, ahead on four of the five. The tune as a whole reads
+1.2% to +2.2% against no tune on three of those clips and -1% on the two
Nocturne excerpts.

`zerolatency` and `animation` change frame types only if you didn't set
`--bframes`. An unknown tune name is an error.

## Which options change the bitstream shape

Worth knowing before you A/B anything:

- `--cqm flat` writes no scaling matrix, which is byte-identical to a decoder's
  default flat-16. The same holds for a custom set that is 16 in every list.
- `--sync-lookahead` never changes a bit. It trades latency for throughput.
- `--threads` can change bits. See the next section.
- `--no-sei` changes the stream but not the pictures.
- `--constrained-intra` changes P and B slices only. An all-intra encode
  (`--keyint 1`) differs by the one PPS bit and reconstructs identically.
- `--slices` above 1 changes the stream and the pictures, because the encoder
  has to replace the prediction a slice boundary withdraws. `--slices 1` is
  byte-identical to no flag.
- `--partitions` changes the pictures at every value except the one the preset
  already resolved to. Nothing about it is signaled: the stream just stops
  using the mb_type and sub_mb_type codes for the shapes taken away.
- The SPS declares the vertical motion-vector bound the encode is held to
  (`log2_max_mv_length_vertical`), rounded up to the next power of two so it's
  never narrower than a vector the search may return. A flat 16 would advertise
  +-16384 luma samples at every level. It costs the SPS 0 or 1 byte and moves
  no picture. A byte compare against a build that wrote the flat 16 differs
  only in the SPS NAL. Compare `--dump-recon`, or everything after the first
  NAL.

## Threading, and what it does to your output

`--threads N` is a budget, not a worker count. The CLI splits it into `g` GOP
workers times `k` in-frame row-wavefront threads, sizes each GOP's share by the
frames it owns, and prints what it chose:

```
y264: encoded 250 frame(s) in 2 GOP(s) on 2 GOP-worker(s) x 4 frame-thread(s)
```

Determinism is guaranteed only at `--threads 1`, where the test gates hold the
same input and config to the same bits. With more threads runs usually repeat,
but neither run-to-run nor cross-thread-count identity is promised, since that
would rule out optimizations, and x264 doesn't promise it either. The wavefront
prices predecessor context slightly differently from the serial path, so `k=1`
and `k>=2` can differ while all `k>=2` agree.

`--dump-recon` forces the serial path, since the recon has to come from one
continuous encode, and two-pass falls back to it in the cases below. An
explicit `--threads` above 1 that can't be honored names the condition:

```
y264: warning: --threads 8 cannot be honoured, encoding serially: --dump-recon needs one continuous self-consistent recon stream, which per-GOP encoders cannot produce
```

An unset `--threads` means "auto, every core", which isn't a request. It
doesn't warn on every `--dump-recon` run of the conformance gate. 4:2:2 and
4:4:4 thread too, the reader taking the subsampling from the Y4M `C` tag. A
`--keyint 1` encode with a VBV runs one instance on the row wavefront, because
a one-frame GOP has no frame left to correct its segment's assumed buffer
level.

Two-pass runs on the parallel path in both passes and splits the stats file
along GOP boundaries:

- Pass 2 needs stats from a threaded pass 1. A serial pass 1 writes no GOP
  markers, so pass 2 quietly stays serial for it.
- Pass 2 refuses, with a message, a stats file whose GOP split differs from the
  current run's, as happens if `--frames` or `--keyint` changes between passes.
  Pass 1 records its `--keyint`, and pass 2 refuses another.
- Under a VBV with `--keyint` under 30, each encoder takes whole GOPs, 30
  frames or more at a time, with keyframes still on the `--keyint` grid. Pass 2
  splits the way pass 1 did. A VBV on one pass and not the other is fine.
- Under a VBV, pass 2 codes one frame at a time in each GOP, its threads on the
  row wavefront.

### Memory: the parallel path streams through a bounded window

The parallel path reads the input on its own thread, and a GOP worker frees
each input frame once its encoder has taken it. How much the input store holds
depends on whether the input can seek.

From a file, the frame count and GOP table are known up front and every frame
is one seek away. The reader keeps each GOP in flight at most 16 frames ahead
of its worker, plus the first 16 frames of the next GOP to be handed out. The
store is `(GOPs in flight + 1) x 16` frames, whatever the title length or disk
speed: 272 frames (611 MiB) on a ten-minute 1920x818 title at default threads.
Because frames are the same bytes in any order, the stream matches an in-order
reader's at every thread count.

From a pipe, the reader must read in display order, so starting a GOP means
holding every frame back to the oldest GOP still coding. The window is
`(--threads + 1) x --keyint` frames, narrowed to read-ahead once the workers
start. Early dispatch hands GOPs out in display order as the reader reaches
them, which keeps the ones in flight contiguous (558 frames on the same title).
Above 1080p the store is capped at an eighth of physical RAM.

The rest is the GOP workers. Each is a whole encoder with its own lookahead
ring, reference and half-pel planes, mb-tree arrays and per-frame decision
records, and peak memory is close to the worker count times its size:

| Resolution | One frame thread per worker | Wide wavefront |
| --- | --- | --- |
| 1080p | about 0.5 GiB | up to 0.9 GiB |
| 2160p | 1.4 to 1.7 GiB | up to 3.0 GiB |

`--max-memory MIB` budgets the whole encode, half of physical RAM by default.
When the workers wouldn't fit, fewer GOPs run at once, each with a wider
wavefront. The thread budget doesn't change. On a 64 GiB machine the default
never limits 1080p, or 2160p titles at default threads. On a 16 GiB laptop it
keeps a 2160p encode out of swap. When it limits, stderr says so:

```
y264: 3 GOP worker(s) instead of 8 to stay inside the memory budget (about 2.6 GiB of 2.9 GiB; --max-memory sets it)
```

The table gives peak footprint (`/usr/bin/time -l`), the median of interleaved
runs. The title is
ten minutes of 1920x818 (13177 frames, `--crf 23`, default threads, 16
workers). The 2160p clip is the 600-frame `meridian` on 3 workers of 8 frame
threads.

| Input | In-order reader | Seek reader |
| --- | --- | --- |
| 1920x818 title: input store | 2946 frames, 6.6 GiB | 272 frames, 0.6 GiB |
| 1920x818 title: footprint / max RSS | 12.4 / 15.0 GiB | 8.3 / 10.9 GiB |
| 2160p clip: input store | 324 frames, 3.8 GiB | 48 frames, 0.6 GiB |
| 2160p clip: footprint | 10.8 GiB | 9.0 GiB |

Max RSS sits above the footprint because it also counts pages the allocator has
freed for reuse, which the system takes back on demand.

On a file, the in-order reader's window wasn't enforced: every worker starts at
once, the one with the sixteenth GOP waits for frame 3750. A starving worker lets the reader past its window, so
the store grows to everything in between, 10.9 to 19.6 GiB of RSS on that title depending on input
speed.

Windowing against holding the whole input, at 720p with `--threads 1 --keyint
50` (peak RSS):

| Input frames | Whole input resident | Windowed |
| --- | --- | --- |
| 180 | 503 MB | 378 MB |
| 450 | 930 MB | 384 MB |
| 900 | 1631 MB | 384 MB |
| 1800 | 3035 MB | 385 MB |

The window held 100 frames at each length. The cost per frame of clip drops
from 1.559 MB to 0.004.

On a pipe the worst case is large: at the default keyint, 500 frames on one
thread and 4750 at 18 threads, which is 14 GiB at 1080p. It's reached only when
the split waits for the GOP count before any worker starts, meaning one thread
on a pipe or a forced split. Lower `--keyint` or `--threads`, or set
`Y264_STREAM_WINDOW`.

The cut-aware split needs every cut before it can dispatch the first GOP. It
runs under `--cut-split`, `--shot-crf`, and `--plan` or `--qpfile` with a
forced IDR. From a file it reads the input twice. A scan pass hands each
frame's luma to the pre-scan and drops it, skips the chroma unread and keeps
one byte per frame. Then the ordinary windowed encode runs on the scan's GOP
table through the seek reader, byte-identical to a whole-input read. Peak RSS
on the same ten-minute title, default memory limit:

| Mode | Whole-input read | Scan pass, in-order reader | Scan pass, seek reader |
| --- | --- | --- | --- |
| plain | 10.9-19.6 GiB | the same path | 10.3-11.8 GiB, 27 s |
| `--cut-split` | refused: needs 34.1 GiB, limit 32.0 | 14.2 GiB, 30 s | 12.7 GiB |
| `--shot-crf` | refused: needs 34.1 GiB, limit 32.0 | 14.8 GiB, 31 s | 13.5 GiB |
| `--shot-crf --scan-in` | | 14.6 GiB, 29 s | |

Under the in-order reader the cut-aware split hands out GOPs longest-first only
within runs of half the `(--threads + 1) x --keyint` window. Sorted over the
whole title, the first workers took GOPs from end to end. The reader held 7877
frames (24.7 GiB) to feed them. The seek reader holds 272 frames however the
GOPs are ordered. The bounded runs stay because the rate carry chains on the
hand-out order.

A pipe can't be read twice, so on a pipe the cut-aware split still holds the
whole input and refuses once it reaches the memory limit. Encode from a file.
Two-pass also needs a seekable input, because it needs the GOP split before
pass 1 writes per-GOP stats.

Frame counts come from the file length: yah264 reads the first Y4M `FRAME`
header, seeks back and checks that the rest divides into fixed-size records.
Every scheduling decision depends on the count. Knowing it keeps the schedule
and bitstream identical to a whole-input read. A pipe gets the same schedule by
waiting for EOF or the `--threads + 1`'th GOP boundary before dispatching.

The memory refusal measures the window, not the clip. The limit is half of
physical RAM (`Y264_MAX_INPUT_MB` overrides it in MiB), counted on physical
rather than free memory because macOS reclaims most inactive, speculative or
purgeable pages on demand. A free-page gate would refuse encodes that could
run, and answer differently between runs.

```
y264: this encode needs 41.7 GiB of memory and the limit is 32.0 GiB
y264: the threaded path streams, but its window is (--threads + 1) x --keyint = 14250 frame(s); lower either, or set Y264_STREAM_WINDOW
y264: limit 32.0 GiB (50% of 64.0 GiB physical), 2.97 MiB resident per 1920x1080 frame, so 11046 frame(s) fit
y264: encode a segment with --frames, split the input, or raise Y264_MAX_INPUT_MB
```

The guard computes from the real geometry: a raw 8-bit frame is `w*h*1.5` bytes
at 4:2:0, `w*h*2` at 4:2:2 and `w*h*3` at 4:4:4. On top of the window sits a
fixed per-worker cost that doesn't scale with it, around 330 MiB for one 1080p
worker and another 50 for a second.

### The saved pre-scan: `--scan-out` and `--scan-in`

On a long title the scan pass is a real fraction of the encode's wall time. A
CRF ladder or a re-encode repeats it over the same frames. `--scan-out FILE`
saves its answer and `--scan-in FILE` replaces the pass with it. The answer
doesn't depend on the rate control. One scan serves every rung of a CRF ladder,
`--cut-split` and `--shot-crf` alike.

A scan reused on the wrong encode would put IDRs and shot CRFs on the wrong
frames without any error. The file records what it was computed from.
`--scan-in` refuses it unless all of it matches, and names the line that
differs:

- the frame count, after `--seek` and `--frames`;
- the input geometry: size, chroma format, bytes per sample, `--crop-rect`;
- the scan's settings as the library resolves them: coded size, sample depth,
  `--keyint` and the minimum keyint, `--scenecut`, whether B frames are on (the
  flash test reads nothing else of them), and the environment knobs that move
  the scan's costs or its cut test;
- a 64-bit FNV-1a hash of the on-disk luma of the first, middle and last
  frames.

The hash catches a different title, cut or start. It doesn't checksum the file
and misses an edit to a frame it doesn't read.

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
line. The map holds each cut and each keyint IDR, before `--open-gop`, `--plan`
and `--qpfile` adjust it. `shots` gives the count, then one shot per line with
the fields `--shot-table` prints: first and last frame, mean and peak lowres
intra cost, mean inter cost, and their ratio. The costs are written with enough
digits to read back as the same doubles, so `--shot-crf` stays byte-identical
through the file. A version this build doesn't read is refused, and so is a
truncated file.

## x264 compatibility

These options match x264 in name and meaning. A command line using only them
ports unchanged:

`--preset`, `--tune` (any of the eight except stillimage and fastdecode), `--bitrate`, `--qp`,
`--keyint`, `--min-keyint`, `--no-scenecut`, `--bframes`, `--b-adapt`, `--ref`,
`--psy-rd`, `--psy-trellis`, `--aq-strength`, `--rc-lookahead`,
`--sync-lookahead`, `--vbv-maxrate`, `--vbv-bufsize`, `--pass`, `--stats`,
`--threads`, `--frames`, `--sar`, `--level`, `--me`, `--direct`, `--cqm`,
`--aud`, `--pic-struct`, `--nal-hrd`, `--filler`, `--frame-packing`, `--cll`,
`--mastering-display`, `--alternative-transfer`, `--overscan`, `--videoformat`,
`--fake-interlaced`, `--open-gop`, `--tff`, `--bff`, `--no-interlaced`,
`--seek`, `--crop-rect`, `--input-res`, `--input-csp`, `--fps`, `--quiet`,
`--log-level`, `--no-progress`, `--no-psy`, `--no-dct-decimate`,
`--no-fast-pskip`, `--no-mbtree`, `--no-asm`, `--deblock`, `--no-deblock`,
`--no-weightb`, `--constrained-intra`, `--chroma-qp-offset`, `--qpmin`,
`--qpmax`, `--qpstep`, `--vbv-init`, `--sps-id`, `--slices`, `--crf-max`,
`--ratetol`, `--partitions`, `--b-bias`, `--longhelp`, `--fullhelp`, `-o`, and
the one-letter aliases `-I`, `-i`, `-b`, `-r`, `-f`, `-q`, `-B`, `-p`, `-A`,
`-m` and `-t`.

Options x264 spells differently:

| x264 option | Here | Notes |
| --- | --- | --- |
| `--no-cabac` | `--cavlc` | `--cabac` turns it back on. |
| `--no-8x8dct` | `--no-transform-8x8` | `--transform-8x8` turns it back on. |
| `--dump-yuv` | `--dump-recon` | Writes the reconstruction as Y4M. |
| `--psy-rd A:B` | `--psy-rd A --psy-trellis B` | `--psy-rd` takes one number here. The two-part form is refused. |

Options that differ, and how:

| Option | The difference |
| --- | --- |
| `--nr` | Same flag and range, different tool: one value there, while here `--nr-intra` and `--nr-inter` split it, and the offset is scaled by each transform position's gain. See [noise reduction](#noise-reduction-and-the-temporal-pre-filter). |
| `--qpfile` | `i` is refused, and `K` is always an IDR, where x264's is a non-IDR I under `--open-gop`. `b` leaves the pyramid to decide whether the B is a reference. |
| `--crf` | Not comparable to x264's: the same value measures -54.6% to +45.3% in size across the corpus. Compare at matched size or quality. |
| `--crf` | Fractions are accepted but largely inert, since the QP is an integer. |
| `--crf` | The frame-level AQ strength is 1.3 under uncapped CRF and 1.04 under capped CRF. |
| `--crf 0` | x264's lossless isn't implemented and is refused, because `rc.rf = 0` means "CRF unarmed". |
| `--direct` | `none` is refused. `auto` (the default), `spatial` and `temporal` behave as x264's. |
| `--qp` | Both encoders turn mb-tree and AQ off at constant QP, so `--qp N` is the same workload on each. |
| `--scenecut` | `0` is off on the CLI, as in x264. In the [C API](#calling-it-from-c), 0 means the default (40), and off is `YAH264_SCENECUT_OFF`. |
| `--sync-lookahead` | Same 0-means-off on the CLI. The API spells off `YAH264_SYNC_LOOKAHEAD_OFF`. |
| `--input-y4m` / `--input-raw` | x264 sniffs the input format. Here they're separate flags and nothing is guessed. |
| `--tune stillimage` | The reference's constants, not measured here. |
| `--tune fastdecode` | Also sets `--weightp 0`, which x264's doesn't. |
| bare default | x264 defaults to CRF 23, and yah264 to QP 26. |
| `--cqmfile` | Codes Cr with the file's `CHROMAV` lists, where x264 uses Cb's. The 4:4:4 chroma 8x8 lists and `--cqm8ic`/`--cqm8pc` have no effect. |
| `--partitions` | Same names, but each preset resolves to its own list here. A missing prerequisite is refused, never narrowed. |
| `--subme` | Also selects the search method, and `0` is refused. See [above](#where-a-same-named-flag-differs-from-x264). |
| `--merange` | UMH, esa and tesa read it. hex ignores it. |
| `--me tesa` | Re-ranks the 16 cheapest SAD candidates by SATD instead of scoring every position in SATD. |
| `--no-chroma-me` | A no-op. The search never reads chroma, which is x264's `--no-chroma-me` behavior, always on. |
| `--qcomp` | ABR curve and mb-tree strength only, where the reference documents one global setting. |
| `--deadzone-inter`/`-intra` | Same scale, but passing either swaps in an approximation, so x264's own defaults measure +0.23%. |
| `--profile` | Refuses a tool you named, where x264 narrows silently. `high10` needs a 10-bit build. |
| `--cll` | `0,0` writes no SEI, where x264 writes one saying both are unknown. The param struct can't tell `0,0` from unset. |
| `--level` | Fits the DPB by lowering an explicit `--ref`, the pyramid and `--bframes` too, where x264 keeps an explicit `--ref` with a warning. `1b` is refused. |
| `--stitchable` | Fixes the SPS only, and raises the declared level and MV range with it. The encode changes too. |
| `--pic-struct` | pic_struct by parity, with every clock timestamp absent, the cheapest legal spelling. |
| `--nal-hrd` | One SchedSelIdx and NAL HRD only, where x264 offers both models, because the encoder obeys one bucket. |
| `--b-pyramid` | `none` and `normal` only. `strict` is refused. |
| `--mvrange` | Narrows the level's bound and never widens it. x264 lets a larger `--mvrange` stand. |
| `--pass 3` | Same meaning, but it writes a copy and renames it over the file only on success. A failed or signaled run leaves the file as it was. A symlink is replaced at its target (a dangling one by a regular file), a hard link is split, and the new file belongs to whoever ran the pass. The directory must be writable at any thread count. Keep a copy if you want the pass-1 records. |
| `--open-gop` | Refused with `--stitchable`. One encoder instance (`--threads` goes to the row wavefront), serial from a pipe. |
| `--intra-refresh` | P frames only, where x264 also allows B. Scene cuts place no keyframe. It's one encoder instance. |
| `--aq-mode` | `0` is refused. AQ off is `--aq-strength 0`. |
| `--zones` | `q=` and `b=` only, which is all x264's help lists, and `b=` is a QP offset in every mode. |
| `--b-bias` | Same range and direction, with fixed ends: +100 puts a B wherever anything beats intra, and -100 means no B frames. |
| `--ipratio`/`--pbratio`/`--cplxblur`/`--qblur` | Two-pass only, where the reference documents its ratio pair as global. The single-pass modes anchor I and B through the CRF track. |

x264's `--muxer` and `--demuxer` have no equivalent. `--slice-max-size`,
`--slice-max-mbs`, `--slice-min-mbs` and `--slices-max` are refused, because
`--slices` takes a count with no size cap. x264's `--interlaced` is
`--tff`/`--bff` here, which code field pictures, not MBAFF. The census in the
[plan](plan.md) lists every x264 option with no flag here and what was decided
for each.

## Calling it from C

The C API in `include/yah264.h` matches x264's field values, with three
deliberate divergences: `scenecut`, `sync_lookahead` and `subme`. All three
come from the zero-as-unset convention. None of this applies to the CLI, which
maps each flag for you.

`yah264_param_default` fills the struct, and twenty of its fields treat zero as
"unset, pick the default". That's fine until a field's off switch is a value
x264 spells as zero:

| Field | x264 spells off as | Zero here means | Off here is |
| --- | --- | --- | --- |
| `scenecut` | `--scenecut 0` | the default, **40**, which is x264's own aggressiveness | `YAH264_SCENECUT_OFF` |
| `sync_lookahead` | `--sync-lookahead 0` | auto, a lead of `bframes+1` | `YAH264_SYNC_LOOKAHEAD_OFF` |
| `filler` | n/a (x264 has no off switch) | follow `nal_hrd`: CBR pads, nothing else does | `YAH264_FILLER_OFF` |

`filler` follows `nal_hrd` because a caller that memsets the struct and sets
only `nal_hrd = YAH264_NAL_HRD_CBR` would otherwise get CBR headers over an
unpadded stream, with no diagnostic.

So `param.scenecut = 0` asks for adaptive keyframes at full strength. Nothing
errors. Use the constants, both `-1` (any negative means off):

```c
yah264_param_t param;
yah264_param_default(&param);
param.scenecut = YAH264_SCENECUT_OFF; /* NOT 0 */
```

Eighteen other fields depend on the convention, so two that read zero
differently would trade a documented divergence for an inconsistency.

Three defaults follow the picture size at `subme` 7 under rate control:
`b_preme_skip`, `ref` and `lr_settle`. To keep a value you assigned to one of
them, set its `YAH264_SET_*` bit in `explicit_set`. The CLI sets the bit for
each of those flags you pass.

### The enumerated fields carry x264's values

Porting a constant gets you the tool it names. `yah264_encoder_open` refuses
what yah264 doesn't implement instead of narrowing it. No value quietly encodes
something else.

| Field | x264 | yah264 |
| --- | --- | --- |
| `rc.method` | CQP 0, CRF 1, ABR 2 | x264's, plus 2-pass at **100** |
| `rc.rf` | `float`, `23.0` | **`double`**, `23.0` |
| `me_method` | DIA 0, HEX 1, UMH 2, ESA 3, TESA 4 | x264's, plus auto at **-1** |
| `direct` | NONE 0, SPATIAL 1, TEMPORAL 2, AUTO 3 | the same values; NONE refused, AUTO is the default |
| `csp` | I400 1, I420 2, I422 6, I444 12 | x264's **I420 2, I422 6, I444 12** |
| `subme` | 0 = fastest | 0 = the library default 10, the slowest; see below |

`YAH264_ABI_VERSION` is 4. Assert on it for a build-time tripwire, and
`#ifndef` it if you also build against headers that predate it.

From ABI 3 on, the first field of `yah264_param_t` is `size`, which
`yah264_param_default()` sets to `sizeof(yah264_param_t)`.
`yah264_encoder_open()` refuses a struct whose `size` isn't the library's own,
which stops a caller built against a different header before it encodes from a
misread struct. Call `yah264_param_default()` first and the field is never
yours to set.

#### Where matching x264 exactly is not possible

There are four places, each deliberate:

- `YAH264_RC_2PASS` is 100. x264 spells 2-pass as ABR plus
  `b_stat_read`/`b_stat_write`, and 3 is where it would put a fourth method, so
  100 can't collide with a future `X264_RC_*`. With `_2PASS` and a `stats`
  path, leaving `rc.pass` at 0 runs pass 1, which opens the stats file for
  writing and truncates it.
- `YAH264_ME_AUTO` is -1, because `X264_ME_*` already uses 0..4. Negative is
  also how this struct spells auto for `subpel` and `sync_lookahead`. Since
  auto isn't 0, a struct that skips `yah264_param_default` is asking for dia,
  which is why `param_default` writes `direct` and `me_method` explicitly.
- `csp` takes x264's values but not its encoding. `X264_CSP_*` are bitflags
  with a mask (`X264_CSP_MASK`) and modifiers (`_VFLIP`, `_HIGH_DEPTH`), none
  of which yah264 implements. The depth is a property of the library you
  called, so `X264_CSP_I420 | X264_CSP_HIGH_DEPTH` is an unknown value and open
  fails. Pass one constant and don't mask. Everything in the gaps is refused
  too: I400, NV12/NV21, YV12/YV16, YUYV/UYVY/V210, YV24 and the RGB family.
- `rc.rf` is a `double` where x264's is a `float`. Assigning `f_rf_constant` to
  it is exact, so porting works. `0` leaves CRF unarmed instead of meaning
  x264's lossless `--crf 0`. The CLI rounds `--crf` to tenths, so ask through
  the API for finer.

#### `subme` follows x264's scale except at zero

The scale runs the same direction as x264's (higher is slower, more RD) and the
tiers line up, except at zero: `0` here is the library default 10, the slowest,
where x264's `0` is its fastest. It's the zero-as-unset convention shared with
nineteen other fields. Ask for `1` if you want speed.

### Fields that are genuinely fine

Everything else either agrees with x264 at zero or has no x264 equivalent.
`rc.qp`, `bframes`, `ref`, `keyint`, `aq_strength`, `psy_rd`, `psy_trellis`,
`cabac`, `transform8x8`, `sar_*` and the `vbv_*` pair all read zero the way an
x264 user expects, and `keyint_min` 0 is auto in both. These still surprise
people:

- `param.threads` is a budget for one encoder instance. 0 picks the machine's
  thread count, capped at 16, 1 is serial and N means up to N, so `threads = 8`
  gets a wavefront of 8. `frame_threads` is still the explicit low-level width
  and wins when set, which lets the CLI's GOP workers each take their own
  share.
- `param.annexb` is read by nothing. Annex-B is the only output mode.
- `badapt` 2 (trellis B-frame placement) is silently narrowed to fast b-adapt.
- `cqm` 2 is `YAH264_CQM_CUSTOM`. It reads `cqm_4x4[6][16]` (Intra Y, Cb, Cr,
  then Inter Y, Cb, Cr) and `cqm_8x8[2][64]` (Intra Y, Inter Y) at the end of
  the struct, in raster order, weights 1..255. A zero weight fails
  `yah264_encoder_open`.

## Environment variables

[The knob census](knobs.md) lists the encoder's `Y264_*` environment variables,
about 140, generated from the source. Only a `-Ddev_knobs=true` build reads
them all. A default build reads the 16 the CLI sets to pass its own options
down (`src/common/env.h`) and the two memory bounds. Its CLI refuses to encode
when any other is set. Most are measurement scaffolding. If a variable isn't in
the first table below, don't set it.

- Most are read once and cached at encoder open, so changing one mid-process
  does nothing. `Y264_MBTREE_STRENGTH` is read again on every mb-tree pass.
- `YAH264_NO_ASM` is presence-only, so `0` still turns it on.
- `Y264_TP_IPF` and `Y264_TP_PBF` are percentages divided by 100.

### Tier 1: worth knowing about

Every build reads the rows promoted to a CLI flag and the two memory bounds.
The rest need `-Ddev_knobs=true`.

| Variable | Default | What it is for |
| --- | --- | --- |
| `YAH264_NO_ASM` | asm on | Force every scalar C path, even at `=0`. Promoted to `--no-asm`, and still overrides it. |
| `Y264_SIMD_FORCE` | unset | Cap the binary at one SIMD tier (`none`, `sse4`, `avx2`, `avx512`) by clearing the CPU feature bits above it. A speed axis only: kernels are bit-exact with C. |
| `Y264_UMH_RANGE` | 16 | UMH search radius in integer pels. Promoted to `--merange`, and still overrides it. |
| `Y264_ABR_QCOMP` | 0.6 | The ABR rate curve's compression and the mb-tree strength derived from it. Promoted to `--qcomp`, and still overrides it. |
| `Y264_AQ_DC` | 1.3 under uncapped CRF, 1.04 otherwise (capped CRF included) | The AQ frame-level strength, fitted by sweep. Under CRF its rate-equivalent difference from the per-MB field's strength is added to the base QP. One-pass ABR and capped VBR feed it to the rate model, and CBR, two-pass and CQP don't use it. |
| `Y264_DZ_INTRA` / `Y264_DZ_INTER` | unset | Quantizer rounding bias in 1/64 units, the encoder's own scale; the CLI flag's scale is `32` minus this. Promoted to `--deadzone-intra`/`--deadzone-inter`, which do the inversion. Both still override. |
| `Y264_AQ_MODE` | 2 | 1 is plain log2-variance AQ, and 2 and above the adaptive form. Promoted to `--aq-mode`, and still overrides it. |
| `Y264_FAST_PSKIP` | 1 (on) | The cheap P_Skip pre-test at `--subme` 8 and below. Promoted to `--no-fast-pskip`, and still overrides it. |
| `Y264_DCTDEC` | 1 (on) | Coefficient decimation. Promoted to `--no-dct-decimate`, and still overrides it. |
| `Y264_2PASS_MT` | on | 0 forces two-pass onto the serial path, reproducing the serial output exactly. |
| `Y264_MBTREE_OFF` | 0 (off) | Skip mb-tree entirely, for comparing `--qp` runs against x264. Changes bits. Promoted to `--no-mbtree`, and still overrides it. |
| `Y264_MAX_INPUT_MB` | 50% of physical RAM | MiB the input window may take before the encode refuses instead of being OOM-killed. |
| `Y264_STREAM_WINDOW` | `(--threads + 1) x --keyint` | Input frames held on the in-order reader (a pipe), at least `2 x --keyint` so a whole GOP is always dispatchable. Scheduling only. |
| `Y264_LA_BUF` | auto = `bframes+1` (4 at medium) | Overrides `--sync-lookahead` either way. `=1` and `=0` cost 1-6% of wall and change no bits. |
| `Y264_CRF_CPLX` | 1 (on) | CRF content adaptation, which narrows the equal-CRF spread against x264 from 100 points to 41 (9 of 11 clips better at the tuned anchor, worst +1.54%). 0 turns it off. |
| `Y264_CRF_LEVEL` | 2.3 | The level in the CRF base under mb-tree, `crf + level + 2.9 * log2(fps / 30)`, fitted against the reference CLI's bytes at equal `--crf`. |

`Y264_SIMD_FORCE=none` is the twin of `YAH264_NO_ASM`. On aarch64 there's one
tier, where the x86 names do nothing. An unrecognized value changes nothing,
and a typo can't pass for a result. Meson's `-Dsimd=` is the build-time twin,
leaving the tier out of the binary.

Two you should know exist so you never set them:

| Variable | Why not |
| --- | --- |
| `Y264_UNSAFE_NO_REFBWAIT`, `Y264_UNSAFE_NO_PREVPWAIT` | Racy on purpose: they drop synchronization waits to measure a ceiling. The output isn't trustworthy. |
| `Y264_UNSAFE_NO_EMIT`, `Y264_UNSAFE_NO_NAL` | Broken on purpose: they delete the entropy emit and NAL assembly to price the emission path. The bitstream is invalid and the reconstruction unaffected. |

### Tier 2: calibration knobs

These let a constant be swept against x264 without a rebuild. Their defaults
were measured, and moving one moves quality in ways usually already tested and
rejected. They're listed so you recognize one in a script. Don't tune with
them.

Motion and mode decision: `Y264_PART_EARLYTERM`, `Y264_PROBE_TRELLIS`,
`Y264_P_INTRA_CABAC`.

Rate control: `Y264_TP_IPF`, `Y264_TP_PBF`, `Y264_TP_CPLXBLUR`.

mb-tree: `Y264_MBTREE_STRENGTH`.

Threading and scheduling (byte-identical unless noted): `Y264_W2`,
`Y264_STAIR_WIDE`.

[The knob census](knobs.md) lists the rest by tier, with each one's default and
reader.

### Tier 3: measurement scaffolding, not features

Profiling counters, trace dumps, racy ceiling probes, and switches for
experiments measured and rejected. Setting one gets you diagnostics or a
known-worse encode, and several change bits.

`Y264_THREAD_PROF`, `Y264_NTP_PROF`, `Y264_LA_STAT`, `Y264_SKIP_ORACLE`,
`Y264_ADME_LOG`, `Y264_HEX_ORACLE`, `Y264_STAIR_BDEPTH`, `Y264_RCP_LAG`,
`Y264_UNSAFE_NO_REFBWAIT`, `Y264_UNSAFE_NO_PREVPWAIT`, `Y264_UNSAFE_NO_EMIT`,
`Y264_UNSAFE_NO_NAL`, `Y264_GOP_FORCE_G`, `Y264_GOP_FORCE_K`.

`Y264_SKIP_ORACLE` takes a path, not a boolean.

### Not read by the encoder

These appear in scripts and tests only, never in C: `Y264_CRF`,
`Y264_CRF_CACHE`, `Y264_STRESS_ABR`, `Y264_TRELLIS_PRINT`, `YAH264_ARGS`,
`YAH264_DESC`, `YAH264_ENC`, `YAH264_CONF_*`, and `YAH264_VMAF_MODEL` (used by
`scripts/vmaf.sh` and `scripts/bdcompare.py`).

These look like environment variables but are compile-time macros that a shell
can't set: `Y264_STAIR_K`, `Y264_STAIR_HOPS`, `Y264_MT_POOL_MIN`,
`Y264_STAIR_LAG`, `Y264_LA_CAP_MAX`, `Y264_DPB_POOL_MAX`, `Y264_BIT_DEPTH`.

### Comments that lie about their defaults

The comments beside the readers in `src/encoder/encoder.c` agree with the code.
One struct-field comment in `src/encoder/encoder.h` is stale and still
describes the gate as off. Trust the reader or [the knob census](knobs.md) over
the header:

| Variable | Header comment claims | Code does |
| --- | --- | --- |
| `Y264_W2` | off by default | **on whenever there is a thread pool** |

## Rough edges

These will bite:

- A value-taking flag as the final argument (`yah264 ... --qp`) is reported as
  an unknown argument instead of a missing value.
- `--threads` does nothing under `--dump-recon` or the two-pass fallbacks
  above. It warns when you asked for more than one.
- The pipe window is `(--threads + 1) x --keyint` frames, whatever the clip
  length: 18 threads at `--keyint 250` is 4750 frames, 14 GiB at 1080p.
- `--level` warns about frame size, frame rate and bitrate and doesn't enforce
  them.
