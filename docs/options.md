# yah264 options reference

Every option the encoder accepts, what it defaults to, and how it interacts with
the others. Read against `cli/yah264_cli.c`, `include/yah264.h` and
`src/encoder/params.c`. Where this file and the code disagree, the code wins and
this file is the bug.

Rate control has its own guide: [rate-control.md](rate-control.md). Read that
one before picking `--crf` or `--bitrate`; this file only says what the flags
are, not which to reach for.

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

Input is Y4M or headerless planar YUV. There is no container demuxer and no
format sniffing: `--input-y4m` takes a Y4M, whose header supplies width,
height, frame rate and chroma format, and `--input-raw` takes raw planes, where
those four have to be given. Pipe from ffmpeg for anything else:

```sh
ffmpeg -i input.mp4 -f yuv4mpegpipe - | yah264 --input-y4m - --crf 23 -o out.264
yah264 --input-raw in.yuv --input-res 1920x1080 --fps 24 --crf 23 -o out.264
```

**Raw input says nothing about itself**, so `--input-res` is required with it
and a wrong geometry does not fail, it encodes garbage. `--input-csp`, `--fps`
and `--input-range` are refused alongside `--input-y4m` rather than silently
disagreeing with the header they would contradict.

The one field a Y4M header carries that raw input cannot, and that nothing on
the command line can invent, is the **sample aspect ratio**: a Y4M with `A128:117`
writes it into the VUI and the same frames as raw do not. Pass `--sar` if you
want the two to agree byte for byte.

The depth travels with the chroma format (`--input-csp i420p10`) for the same
reason it does in the Y4M `C` tag, and it still picks the library: nothing else
on the command line says 10-bit. `--output-depth 10` over 8-bit raw input
upshifts exactly as it does over an 8-bit Y4M.

Output is an Annex-B elementary stream. `-` means stdin/stdout for either side.

The Y4M `C` tag is parsed for `420`, `422` and `444`, plus their `p10` form.
**The input's bit depth selects the encoder**: one binary carries both, an
8-bit library and a 10-bit one, and a `C420p10` file is coded as High 10 with
nothing said on the command line. `--output-depth 10` puts 8-bit content
through the 10-bit encoder after upshifting every sample by 2; `--output-depth
8` on 10-bit input is refused, because taking samples away is a conversion and
ffmpeg owns those.

## What the bare default is

`yah264 --input-y4m in.y4m -o out.264` with no other flags is deliberately
`x264 --preset medium`'s tool-set, so the two are comparable without an argument
list on either side. That means preset medium, CABAC, `--ref 3`, `--bframes 3`,
8x8 transform on, adaptive B on, `--rc-lookahead 40`, `--keyint 250`.

It also means **constant QP at 26**, because no rate-control flag was given.
That is the one part of the default that is not an x264 match: x264's bare
default is CRF 23. If you want constant quality you have to ask for it.

`--psy-rd` defaults to 2.0, this encoder's own swept value rather than an x264
match, and `--psy-trellis` to 0.0, which is x264 medium's.

## The option table

Defaults marked "preset" are set by the preset ladder and the bare default is
medium's row; see [Presets](#presets).

**A value outside an option's domain is refused, naming the domain.** A
non-numeric value and an out-of-range one both exit 2 with a message like
`yah264: --ref expects 1..2147483647 (got '0')`. Parsing is lenient about the
*form* of the number, so `--bitrate 800.0` works because that is what
`str(float)` produces, but a non-integral value for an integer option is refused
rather than truncated.

Enum-valued options refuse an unknown name the same way, so a typo and x264's
unimplemented `--direct none` are both rejected rather than silently read
as something else.

### Input and output

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--input-y4m` | path or `-` | required | Y4M input. No other input format exists. |
| `-o`, `--output` | path or `-` | `-` (stdout) | Annex-B output. |
| `--input-raw` | path or `-` | | Headerless planar YUV. Needs `--input-res`. |
| `--input-res` | `WxH` | | Raw input geometry. Required with `--input-raw`, refused with `--input-y4m`. |
| `--input-csp` | `i420`\|`i422`\|`i444`, each with an optional `p10` | `i420` | Raw input chroma format **and sample depth**. A raw file has no Y4M `C` tag, and the depth is what that tag carries beside the format, so it travels with the format here too: `--input-csp i420p10`. That is why there is no `--input-depth`. |
| `--fps` | `N`, `N/D` or a decimal | 25 | Raw input frame rate. `--fps 23.976` is read as 23976/1000. |
| `--input-range` | `full`\|`limited` | not signalled | VUI colour range for raw input; the same field `--range` sets. |
| `--frames` | N | 0 = all | Stop after N input frames. |
| `--seek` | N | 0 | Drop the first N input frames. Seeks where the input allows it and reads-and-discards where it does not, so a pipe still works. |
| `--crop-rect` | `L,T,R,B` | off | Crop the input before encoding, in luma samples. The offsets must land on a chroma sample and the result must be even in both axes; anything else is refused rather than rounded. |
| `-v`, `--verbose` | | | Log level debug: adds one line naming what the command line resolved to. |
| `--quiet` | | | Log level warning: no informational lines. Warnings and errors still print. |
| `--log-level` | `error`\|`warning`\|`info`\|`debug` | `info` | The same dial. There is no `none`: an error prints at every level, because a run that fails in silence is worse than a noisy one. |
| `--no-progress` | | | No progress line. It only appears when stderr is a terminal, so a gate or a redirect never sees one either way. |
| `--output-depth` | 8 or 10 | the input's | Which of the two encoder libraries codes the stream. 10 on 8-bit input upshifts each sample by 2 and writes High 10; 8 on 10-bit input is refused rather than rounded. |
| `--dump-recon` | path | off | Write the encoder's own reconstruction as Y4M, in display order, at the depth the encode ran at. **Forces the single-threaded path** (see below), and an explicit `--threads` above 1 is warned about rather than dropped. |
| `--range` | `full` or `limited` | not signalled | VUI colour range. The Y4M `XCOLORRANGE` tag sets it on its own. |
| `--colorprim`, `--transfer`, `--colormatrix` | H.273 code or a name (`bt709`, `bt2020`, `bt601`, `smpte170m`, `bt470bg`, `srgb`, `smpte2084`, `arib-std-b67`) | not signalled | VUI colour description. Signalling only: nothing in the encoder changes with them. |
| `--chromaloc` | 0..5 | not signalled | VUI chroma sample location. |
| `--version` | | | Print version, exit. |
| `-h`, `--help` | | | Print usage, exit. |

### Rate control

Covered properly in [rate-control.md](rate-control.md). The flags:

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--qp` | 0..51 | 26 | Constant QP. This is the mode you get if you name no other. |
| `--bitrate` | kbit/s | off | Single-pass ABR average bitrate. |
| `--crf` | ~0..51 | off | Constant rate factor. Accepts a decimal but see the staircase note in the RC guide. |
| `--vbv-maxrate` | kbit/s | 0 = off | VBV peak rate. Needs `--vbv-bufsize` too; either alone does nothing. |
| `--vbv-bufsize` | kbit | 0 = off | VBV buffer size. |
| `--vbv-init` | float | full | Initial VBV occupancy: a value at or below 1 is a fraction of `--vbv-bufsize`, above 1 is kbit. Only does anything where the VBV actually binds. |
| `--qpmin`, `--qpmax` | 0..51 | 0, 51 | Bounds on the coded QP the rate control may pick. Applied after the frame-type offsets, so they bound what is coded rather than the base QP the offsets came from. |
| `--qpstep` | 1..51 | 4 | Largest QP move between consecutive frames of one type. Reaches the single-pass ABR step clip and the two-pass allocator's. `Y264_ABR_QPSTEP` still overrides the ABR half. |
| `--pass` | 1, 2 or 3 | off | Multi-pass: 1 writes stats, 2 reads them, 3 reads them **and writes them back**, so a further pass refines against a real encode instead of the fixed-QP pass 1. Pair with `--bitrate`. |
| `--stats` | path | `yah264.stats` | Two-pass statistics file. |
| `--aq-strength` | float | 0.4 rate-controlled, 0.0 at CQP | Variance adaptive quantisation. 0 disables. |
| `--aq-mode` | 1 or 2 | 2 (1 under mb-tree's derived shape) | The AQ metric: 1 log2-variance, 2 autovariance. **`0` is refused**, unlike x264's: the value is a metric selector with no off seat here and 0 would encode as 1. AQ off is `--aq-strength 0`. |
| `--abr-model` | `default`, `rf` | `default` | ABR bit allocation. `rf` (spelled `x264` in older scripts, still accepted) spends a given bitrate markedly better and hits it less reliably; see below. |
| `--rc-lookahead` | frames | preset (40 at medium) | mb-tree propagation window. 0 turns the window off. |
| `--no-mbtree` | | mb-tree on | Skip mb-tree propagation entirely. Already the policy at constant QP, where x264 forces it too. |
| `--ipratio`, `--pbratio` | float | 1.4, 1.3 | **Two-pass only.** The I-to-P and P-to-B qscale factors of the offline allocator. x264's names, numbers and defaults; the single-pass modes anchor I and B their own way and never read these. |
| `--cplxblur`, `--qblur` | frames | 20, 0 | **Two-pass only.** The allocator's complexity and qscale blur radii. |

### `--abr-model rf`, and why it is not the default

`default` is the shipped allocator. `rf` selects the rate-factor one: a self-normalising
rate factor for P frames, with I and B anchored to the running non-B QP track.
It is opt-in because the two things an ABR encoder does, spending a bitrate well
and hitting it exactly, come apart here: it is better at the first and worse at
the second.

**Spending it.** At 1200 kbit/s on samsung_720p the default's I frames are 3.4x
smaller than x264's and its B frames 2.7x larger; `--abr-model rf` puts both
within a few percent of x264's own sizes. Across the 12-clip band that is a
median **-5.65% BD-rate**, 9 clips better (samsung -26.6%, akiyo -49.2%), 3
worse (ducks +5.8%).

**Hitting it.** On content whose complexity signal collapses, a near-black
opening being the usual way, the rate factor runs away and this model has no
swing limit to absorb it: sintel reads **+24.5% over target** at 900 frames.
Elsewhere the error is a warm-up transient that decays with length (ducks
+11.4% at 180 frames, +4.6% at 480).

So: choose it when quality per bit matters more than landing the target
exactly, and do not choose it for a hard bitrate budget on unknown content.

**`--qp`, `--bitrate` and `--crf` each name a mode, and the last one on the
command line wins**, which is x264's rule. A warning on stderr names what the
loser still does:

```
yah264: warning: --crf 23 is dropped -- --bitrate 5000 came later and selects ABR (last rate-control flag wins, as in x264)
yah264: warning: --qp 26 still sets only the base QP -- --crf 26 came later and selects CRF (last rate-control flag wins, as in x264)
```

The two messages differ because `--qp` is not thrown away: `rc.qp` seeds the
encoder's base QP whatever the mode. `--crf` and `--bitrate` really are
discarded.

So `--bitrate X --crf Y` is CRF, and `--crf Y --qp Z` and `--bitrate X --qp Z`
are constant QP. Every one of them prints the warning above, so the bits move
only where you are told they moved. Repeating the *same* flag is not a clash and
says nothing.

`--pass` is not in the contest. It is a mode plus a stats round-trip whose
target is `--bitrate`; two-pass CRF is not implemented, so a `--crf` passed
alongside it is dropped and says so.

`--aq-strength`'s default is a two-way branch: **0.4** under CRF/ABR/two-pass,
0.0 under constant QP. An explicit value, including an explicit `0`, always
wins. 0.4 is this encoder's own measured value, not x264 medium's 1.0: a
seven-clip VMAF-NEG sweep put the optimum near 0.3, and the shipped default sits
just above it. Why it lands so far below x264's is a known open question about
how this AQ differs from x264's.

### GOP structure

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--keyint` | N | 250 | Maximum frames between IDRs. |
| `--min-keyint` | N | 0 = auto (`keyint/10`) | A scene cut closer than this to the last keyframe is not promoted to an IDR. Clamped internally to `[1, keyint/2+1]`. |
| `--scenecut` | N | 40 | Adaptive-keyframe aggressiveness; higher inserts more. `0` or any negative turns it off. |
| `--no-scenecut` | | | Same as `--scenecut 0`. Only `--keyint` places IDRs. |
| `--bframes` | N | preset (3) | Consecutive B frames between anchors. 0 disables B entirely. |
| `--b-adapt` | N | 1 | Adaptive B placement. 0 codes a fixed cadence. |
| `--direct` | `auto`\|`spatial`\|`temporal` | `auto` | B direct MV derivation; `auto` lets each B slice pick by the running skippability score (since 2026-09-03). |
| `--ref` | N | preset (3) | P-frame list-0 reference count, clamped to 16. |

`--bframes 2` or higher enables the B-pyramid, which changes `max_num_ref_frames`
and therefore the auto-selected level.

**`--bframes 0` also turns mb-tree off unless the lookahead window is on.**
mb-tree runs when `bframes > 0`, or when `rc-lookahead > 0` and the IPPP path is
enabled (it is by default). So `--bframes 0 --rc-lookahead 0` is a genuinely
different rate-control workload from the default, not just fewer frame types.

### Coding tools

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--preset` | name | `medium` | Speed/quality ladder. See below. |
| `--tune` | name | off | Content tuning. See below. |
| `--cabac` / `--cavlc` | | CABAC (preset) | Entropy coder. `ultrafast` sets CAVLC. |
| `--transform-8x8` / `--no-transform-8x8` | | on (preset) | 8x8 transform and 8x8 intra. On means High profile. |
| `--cqm` | `flat`\|`jvt` | `flat` | Quantisation matrices. `jvt` writes scaling lists into the SPS and forces High profile. |
| `--deblock` | `A:B` | `0:0` | In-loop deblocking filter offsets, each -6..6. These are the slice header's own div2 values, so x264's numbers port unchanged and the offset the decoder applies is twice what you type. |
| `--no-deblock` | | filter on | No in-loop deblocking at all: `disable_deblocking_filter_idc 1`, and no filter runs. |
| `--b-pyramid` | `none`\|`normal` | `normal` | `none` codes a flat B run instead of a hierarchy. `strict` is not implemented and is refused rather than read as `normal`. |
| `--no-weightb` / `--weightb` | | on | Clears (or restores) `weighted_bipred_idc`, so B slices use plain averaging instead of implicit weights. |
| `--slices` | N | 1 | Cut each picture into N independently decodable slices on macroblock-row boundaries. Nothing crosses a slice's first row: not the intra reference samples, not the mode or motion-vector predictors, not the CAVLC nC or the CABAC contexts, not the `mb_qp_delta` chain. Each slice is its own NAL, so a decoder can resynchronise at any of them and several can be decoded at once. **The in-loop filter is not cut**: every slice header writes `disable_deblocking_filter_idc` 0, so the picture is deblocked whole and the slice edges do not show. It costs bits (see below). Clamped to the picture's macroblock row count, and to 256. Refused with `--hw`. |
| `--constrained-intra` | | off | Sets the PPS `constrained_intra_pred_flag`. Intra prediction in a P or B slice then treats an inter-coded neighbour as unavailable, for the reference samples and for the intra mode predictor alike, so an intra macroblock decodes from intra data alone. Error resilience, and the price is bits: the prediction has less to work with and I_16x16 plane and the corner-reading 4x4/8x8 modes drop out wherever the above-left neighbour is inter. No effect on I slices, where every neighbour is intra already. |
| `--chroma-qp-offset` | -12..12 | 0 | PPS `chroma_qp_index_offset`. Reaches the chroma quantiser **and** the deblock filter's chroma edge QP, as the spec requires. Written to `second_chroma_qp_index_offset` too. |
| `--mvrange` | luma samples | the level's | Vertical motion-vector range. The default is the level's own Table A-1 bound; only a value **tighter** than the level's has any effect, because a level is a conformance bound and not a suggestion. The SPS's `log2_max_mv_length_vertical` follows whichever bound is in force, rounded up to the next power of two so the declaration is never narrower than a vector the search may return. |
| `--me` | `dia`\|`hex`\|`umh` | auto from preset | Motion search. Auto is hex at medium and faster, UMH at slow and above. |
| `--psy-rd` | float | 2.0 | Psychovisual RD strength. 0 disables. |
| `--psy-trellis` | float | 0.0 | Psy-trellis strength. Around 1.0 for grain. |
| `--trellis` | 0..2 | 1 | RDOQ placement, x264's scale: 0 off, 1 the committed macroblock only, 2 every mode decision. |
| `--subme` | 1..11 | preset (7 at medium) | Subpel/RD analysis level, x264's scale. See below. |
| `--subpel` | 0..2 | preset (2 at medium) | Refinement *pattern*: 0 square, 1 diamond, 2 capped diamond. No x264 equivalent. |
| `--merange` | pels | 16 | UMH search radius, x264's `--merange`. **Only UMH reads it**; `dia` and `hex` ignore it, so it does nothing at medium. |
| `--qcomp` | 0..1 | 0.6 | Rate-curve compression, x264's `--qcomp`. See the divergence below. |
| `--cut-split` | off | File input only: put an IDR on every scene cut the pre-scan finds and split the GOP workers there, instead of at arithmetic keyint boundaries. Clean seek points at the cuts; each shot is coded as its own unit. Needs the whole file, so it does nothing on piped input. |
| `--shot-table` | off | Print the pre-scan's shot table as JSON on stderr (first/last frame, mean and peak lowres intra cost, mean inter cost, ratio). Implies `--cut-split`. |
| `--shot-crf` | off | Per-shot CRF from the shot table: `crf + 6(1-qcomp) log2(C_shot / C_title)`, C the shot's mean lowres inter cost, clamped to +-4 QP, shots under `Y264_SHOT_MIN` frames merged into their predecessor; each GOP takes its shot's CRF. Implies `--cut-split`. On the multi-shot sequences it is worth -6 to -11% BD-VMAF-NEG against `--cut-split` alone and 1-4% over the default's across-shot term, the difference being the whole-file title reference; on a 3000-frame Big Buck Bunny window it LOSES 2.6% against flat (the pre-scan's inter cost misreads the animation's pans), so measure before trusting it on a title. The measured-hull orchestrator (docs/orchestrator-design.md) beats it by 3-5% on both. Ignored by `--pass 1`. |
| `--plan FILE` | off | Zones, one per line: `first last [idr] [qp+N\|qp-N]`, frames from 0 in input order, inclusive. `idr` forces a keyframe and a GOP boundary at `first`; the QP offset applies to every frame in the range on top of the rate control (CRF, CQP, ABR). The engine interface's plan (docs/engine-interface.md). |
| `--gop-threads K` | off | Pin every GOP instance's frame threads to K, so a GOP re-encoded alone reproduces its bytes (`scripts/shot_determinism.sh`). |
| `--segment-out PATTERN` | off | Also write each GOP to its own file (`%d` = GOP index), each with its own parameter sets; concatenated in order they equal the stream. |
| `--frame-stats FILE` | off | One JSON line per coded frame in coding order: frame, gop, type, idr, ref, qp, bytes, k. |
| `--deadzone-inter` | 0..32 | 21 | Inter luma quantisation deadzone, x264's flag and x264's value. |
| `--deadzone-intra` | 0..32 | 11 | Intra luma quantisation deadzone. |
| `--no-psy` | | psy on | `--psy-rd 0 --psy-trellis 0`, which is how x264 spells it. An explicit `--psy-rd` anywhere on the line still wins, exactly as it wins over a `--tune`. |
| `--no-dct-decimate` | | decimation on | Never drop a block whose coefficients are all marginal. |
| `--no-fast-pskip` | | fast P-skip on | Drop the cheap P_Skip pre-test, so every P macroblock takes the full analysis path. Only reachable at `--subme` 8 and below, where that test runs. Slower, and it moves bits. |
| `--no-asm` | | asm on | Force every scalar C path. Byte-identical output: every kernel is checkasm-equal to its C reference. |

`--merange`, `--qcomp`, the deadzone pair, `--aq-mode`, `--no-mbtree`,
`--no-dct-decimate`, `--no-fast-pskip`, `--no-asm`, the ratio pair and the blur
pair all also have a `Y264_*` variable, **which still works and still wins**:
the flag sets the variable the encoder reads, and if the environment disagrees
with the flag the environment takes it and says so on stderr. That keeps sweep
scripts that override a binary's arguments from the environment working,
without an env var quietly beating an explicit flag. (`--no-psy` is the one
exception in that group: it is the psy pair set to zero, two ordinary param
fields, not a variable.)

Three of them do not mean quite what the x264 flag of the same name means:

- **`--subme` also picks the search method.** With no `--me`, this encoder gates
 hex against UMH on subme (below 8 hex, 8 and above UMH), so `--subme 8` changes
 the algorithm and not just the effort. x264 keeps the two independent. Pass
 `--me` to pin it. `--subme 0` is refused rather than obeyed: it is x264's
 *fastest* mode and this library's "unset", which is the slowest (10), so
 accepting it would do the opposite of what an x264 user meant.
- **`--qcomp` reaches the ABR curve and the mb-tree strength derived from it, not
 the CRF or two-pass curves**, which carry their own (`Y264_TP_QCOMP`). x264's
 applies to all of them.
- **The deadzone flags are not a no-op at their own defaults.** Passing either
 swaps the exact shipped expression, intra `step/3` and inter `step/6`, for the
 1/64 approximation, so `--deadzone-inter 21 --deadzone-intra 11` measures
 +0.23% on foreman rather than 0. It also disables the NEON quant path. The
 value is inverted on the way in exactly as x264 inverts it
 (x264 inverts the deadzone on the way in, so its internal bias is `32 - flag`), so the numbers port.

**`--trellis` takes all three of x264's levels**, and it defaults to 1 as x264
does: 0 is RDOQ off everywhere, plain deadzone; 1 quantises the trials with the
deadzone and re-encodes only the winner with RDOQ; 2 runs RDOQ in every mode
decision. `Y264_TRELLIS_COMMIT=0` is the separate escape that puts RDOQ back in
every trial at level 1. There is no `--weightp` or `--open-gop`. Explicit
P-slice weighted prediction is signalled in the PPS unconditionally.
Interlaced sources are coded as FIELD PICTURES (PAFF): see `--tff`/`--bff`
below. There is no MBAFF and there will not be
([what-we-dont-do.md](what-we-dont-do.md)).

**What `--slices` costs, and what decides it.** Nothing predicts across a
slice's first row, so that row codes without its neighbours above, and each
slice pays a header and an entropy-coder reset. Measured at `--slices 4`, 120
frames, five CRF rungs inside the VMAF-NEG 55-95 band, at matched achieved
bitrate (`scripts/bd_at_rate.py`):

| clip | BD-VMAF-NEG vs `--slices 1` |
| --- | --- |
| riverbed_1080p | +0.71% |
| touchdown_420 | +4.01% |
| pedestrian_1080p | +4.08% |
| sunflower_1080p | +8.53% |
| **median** | **+4.05%** |

The spread is not resolution, it is **how much of the picture was going to be
skipped**. 8.4.1.1 gives P_Skip a zero motion vector wherever the neighbour
above is unavailable, so every macroblock in a slice's first row loses the
skip whenever the picture is moving at all, and it loses it against a baseline
that is almost entirely skip. riverbed (dense detail, little skip) pays +0.71%;
sunflower (a slow pan over a mostly static frame) pays +8.53% for the same cut.
The cost is linear in the number of cuts: at CRF 32 on sunflower, `--slices`
2/4/8 costs +2.96% / +6.81% / +14.84% of the bytes, and on riverbed
+0.26% / +0.70% / +1.47%.

On the detailed clips that per-cut cost is what an independent encoder pays
(riverbed, same measurement: +0.18% / +0.61% / +1.52%); on the skip-heavy ones
it is several times that (sunflower: +0.51% / -0.36% / +2.09%). So this
encoder leans harder on P_Skip than it has to, and a slice cut is where that
shows. Recorded as measured; closing it is its own piece of work, not part of
shipping the flag.

Slices are not a threading vehicle here -- the row wavefront already
parallelises inside a picture -- so what this buys is loss resilience and
decoder-side parallelism, not encoder speed.

### Stream metadata

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--sar` | `W:H` | unspecified (square) | Sample aspect ratio. Accepts `16:11` or `16/11`. |
| `--level` | e.g. `3.1` or `31` | auto | Force an H.264 level. Range 1.0 to 6.2. |
| `--no-sei` | | SEI on | Suppress the x264-style settings SEI. |
| `--profile` | `baseline`\|`main`\|`high`\|`high10`\|`high422`\|`high444` | derived | Constrain the tool-set to a profile and write its `profile_idc`. See below. |
| `--aud` | | off | An access unit delimiter (NAL type 9) opens every access unit, ahead of the parameter sets. |
| `--pic-struct` | | off | VUI `pic_struct_present_flag` plus a `pic_timing` SEI per picture. The value written is 0, a progressive frame; there is nothing else to write until field coding exists. |
| `--frame-packing` | 0..7 | off | `frame_packing_arrangement` SEI: 0 checkerboard, 1 column, 2 row, 3 side-by-side, 4 top-bottom, 5 frame alternation, 6 2D, 7 tile. Written once, "until cancelled". |
| `--cll` | `MAX,AVG` | off | Content light level SEI (MaxCLL, MaxFALL) in cd/m^2. |
| `--mastering-display` | `G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)` | off | Mastering display colour volume SEI. Chromaticity in 0.00002 units, luminance in 0.0001 cd/m^2. **G, B, R is the spec's order**, not the R, G, B a person writes. |
| `--alternative-transfer` | H.273 code or a `--transfer` name | off | `alternative_transfer_characteristics` SEI: the transfer a display should prefer over the VUI's. |
| `--overscan` | `undef`\|`show`\|`crop` | `undef` | VUI `overscan_info`. `undef` writes nothing, which is the default. |
| `--videoformat` | `component`\|`pal`\|`ntsc`\|`secam`\|`mac`\|`undef` | `undef` | VUI `video_format`. Naming it opens the `video_signal_type` block even with no colour description; the three colour codes then stay at 2 (unspecified). |
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
| 8x8 transform on, or `--cqm jvt` | 100 (High) |
| 10-bit input, or `--output-depth 10` | 110 (High 10) |
| 4:2:2 input | 122 (High 4:2:2) |
| 4:4:4 input | 244 (High 4:4:4) |

**The derivation never lands on Baseline**, even at `--cavlc --bframes 0
--no-transform-8x8`, because explicit P-slice weighted prediction is signalled
in the PPS unconditionally and A.2.1 forbids it in Baseline. Claiming 66 with
that flag set would be a header the stream does not obey.

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

The flag exists so two clips encoded separately can be concatenated: a decoder
that meets a second SPS has to re-initialise, and it only skips that when the
second SPS is byte-for-byte the first. Three SPS fields moved with the
settings, and this pins all three:

- `max_num_ref_frames`, `max_dec_frame_buffering` and `max_num_reorder_frames`
  become the level's own maximum (capped at 15), not what this encode needs;
- `log2_max_frame_num_minus4` becomes 4 unconditionally, rather than 0 at
  `--ref 1 --bframes 0`;
- the level is picked for that fixed 15-frame DPB, so it stops moving with
  `--ref` too, leaving it a function of frame size, frame rate and rate cap.

That last one is the charge. A higher declared level carries a wider `MaxVmvR`,
the search is clamped to the level, so **`--stitchable` really is a different
encode and not only different bytes**. It is worth saying plainly because every
other flag in this group changes only the header.

It pins the **SPS**. The PPS still moves: `weighted_bipred_idc` follows
`--bframes` and `num_ref_idx_l0_default_active_minus1` follows `--ref`, and
pinning the second would mean writing `num_ref_idx_active_override_flag` on
every P and B slice, which is bits on every picture rather than bytes once.
Two streams to be stitched should pass the same `--ref` and `--bframes`;
pinning the PPS belongs with the rest of the stitching bundle.

### What `--fake-interlaced` actually changes

`frame_mbs_only_flag` becomes 0, so the sequence *may* contain field pictures.
It never does: every picture is coded exactly as it would be without the flag,
and every slice header says so with a `field_pic_flag` of 0. What moves is the
SPS geometry -- `height_in_map_units` halves, an `mb_adaptive_frame_field_flag`
of 0 appears, and the vertical crop counts in double units -- plus one bit per
slice header. A coded height that is an odd number of macroblock rows is padded
by one row and cropped away, because `FrameHeightInMbs` has to be even once the
flag is clear; 720p is 45 rows, so that is the common case rather than the
corner. The crop then counts in double units, which is why the flag needs a
height that is a multiple of 4 and refuses any other.

A named profile the stream was then checked against is an assertion, so the SPS
carries the matching constraint_set flag: `constraint_set0_flag` comes with
profile_idc 66 on its own, and `--profile main` adds `constraint_set1_flag`. A
derived profile asserts nothing it did not assert before.

### Field coding: `--tff` and `--bff`

An interlaced source is two half-height pictures per frame taken a field period
apart, and coding them as one frame asks the transform to model a comb. `--tff`
and `--bff` code them as what they are: two **field pictures** per input frame,
each half the height, in the named order, each with its own slice header, its
own picture order count and its own reference list. The sequence declares
`frame_mbs_only_flag` 0 and every slice a `field_pic_flag` of 1.

**A Y4M that says `It` or `Ib` turns this on by itself.** The header knows the
field order and nothing on the command line has to repeat it; `--tff` / `--bff`
override the header and `--no-interlaced` refuses to field-code at all. Raw
input says nothing about itself, so there a flag is the only way in.

What the standard changes for a field picture, and this encoder with it: the
residual scan is the field scan rather than the zig-zag, CABAC reads its
significance contexts out of a second set of models, the deblocking filter drops
an intra horizontal macroblock edge from strength 4 to 3 and halves the vertical
motion threshold, and the declared vertical motion range halves.

What this release codes, and what it does not:

- **I and P fields, CAVLC and CABAC, 4:2:0.** B fields are the next item.
  Where the field order came from decides what a conflict with it means: with
  `--tff`/`--bff` on the command line, a named `--bframes` above 0, 4:2:2,
  4:4:4 or `--hw` is a refusal, because this encode was told to field-code.
  When the Y4M's tag is all that asked, the named flag wins and one line on
  stderr says the field order went unused -- the tag is a property of the
  input, not a request. A B count the PRESET chose is narrowed in silence
  either way, as with `--profile`.
- **Each field references the same-parity field of the frames before it**, out
  to `--ref` frames, named by a reordering command in the slice header. The
  same-parity restriction is deliberate: it is what keeps the cross-parity
  chroma motion offset of 8.4.1.4 out of the encoder entirely.
- **An I frame codes both of its fields intra**, rather than predicting the
  second from the first. Predicting it would be the one place a field reads the
  opposite parity, so it waits for the item that builds that path. It costs one
  intra field per key frame.
- **Rate control and the lookahead stay frame-based.** A frame's type applies to
  its pair, and the per-picture bit target and VBV credit are each half a
  frame's, because two pictures are coded per frame.
- The coded height is an even number of macroblock rows, padded and cropped
  away exactly as `--fake-interlaced` does, because each field is half of it.
  That makes the vertical crop count in double units, so **the height has to
  be a multiple of 4**: any other could not be cropped back to itself, and a
  stream whose declared height is not the one you handed in is worse than a
  refusal. It answers to the same named-versus-inferred rule as the rest --
  refused under `--tff`, narrowed to frame coding under a bare `It` tag.
  Every broadcast interlaced height (480, 576, 1080) already is one.
  `--fake-interlaced` is always named, so there it is always a refusal; it
  did not check at all before this item.

Two speed-side gaps, both of them named rather than measured away: a field
picture runs motion search without the cached half-pel planes (they are built
over the frame and describe no field), and the residual RD model prices a field
block in frame-scan order against the frame context models. Neither moves a
sample; both are worth closing when field coding gets a speed leg.

`--dump-recon` writes **woven frames**, one per input frame, with the field
order in the Y4M's own `I` tag, because that is the picture a decoder outputs
and the picture the conformance gate compares.

`--level` below the computed conformant minimum is accepted, but prints a
warning that the stream may be non-conformant. It does not clamp the encode to
fit the level you asked for. The auto level is derived from frame size, frame
rate and DPB size per Annex A.

### Threading

| Option | Argument | Default | What it does |
| --- | --- | --- | --- |
| `--threads` | N | 0 = auto (online cores) | Total thread budget, split across GOP workers and in-frame wavefront threads. |
| `--sync-lookahead` | frames | auto | Input frames buffered so the lookahead runs ahead of the encode on its own thread. `0` or negative disables. |

`--sync-lookahead` costs exactly that many frames of latency and changes no
bits. Auto resolves to `bframes+1` once the frame-thread pool is wide enough to
run a lookahead chain against, and 0 otherwise. The CLI prints the resolved lead
and its millisecond cost at your frame rate on stderr. `--tune zerolatency` sets
it off.

There is no `--frame-threads` flag; the in-frame wavefront share is derived from
`--threads` by the budget split.

## Presets

The ladder sets five things. Everything else is preset-independent.

| Preset | subme | subpel | ref | rc-lookahead | cabac | 8x8 | bframes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ultrafast | 1 | 2 | 1 | 0 | off | off | 0 |
| superfast | 1 | 2 | 1 | 0 | on | on | 3 |
| veryfast | 2 | 2 | 1 | 10 | on | on | 3 |
| faster | 4 | 2 | 2 | 20 | on | on | 3 |
| fast | 6 | 2 | 2 | 30 | on | on | 3 |
| **medium** | 7 | 2 | 3 | 40 | on | on | 3 |
| slow | 8 | -1 | 5 | 50 | on | on | 3 |
| slower | 9 | -1 | 8 | 60 | on | on | 3 |
| veryslow | 10 | -1 | 16 | 60 | on | on | 3 |
| placebo | 11 | -1 | 16 | 60 | on | on | 3 |

subpel 2 is a capped diamond (x264's subme-7 shape); -1 is an 8-neighbour square
iterated to convergence. subme at or below 8 uses the fast SATD partition path,
9 and above does full RD per partition.

The preset also gates motion search when `--me` is not given: subme below 8 runs
hex, 8 and above runs UMH. So `--preset slow` changes the search algorithm, not
just its effort.

Explicit flags override the preset regardless of order on the command line:
`--ref`, `--bframes`, `--rc-lookahead`, `--cabac`/`--cavlc`,
`--transform-8x8`/`--no-transform-8x8`, `--me`.

An unknown preset name is an error, not a warning.

## Tunes

`--tune` sets content-adaptive defaults. Explicit `--psy-rd` and
`--psy-trellis` override whatever the tune chose.

| Tune | Effect |
| --- | --- |
| `grain` | psy-trellis 1.0, psy-rd 1.5. The 1.5 was a measured VMAF-NEG win on heavy grain against a 1.0 default; the default has since moved to 2.0, so the tune now lowers psy-rd and has not been re-measured against it. |
| `film` | psy-trellis 0.5. psy-rd left at the 2.0 default; the film value has not been BD-measured for want of a film clip. |
| `animation` | psy-trellis 0.5, aq-strength 0.6, and bframes +2 (capped at 8). |
| `psnr` | psy-rd 0, psy-trellis 0, aq-strength 0. |
| `ssim` | psy-rd 0, psy-trellis 0, AQ kept. |
| `zerolatency` | bframes 0, rc-lookahead 0, sync-lookahead off. |
| `stillimage` | psy-trellis 0.7, aq-strength 1.2, deblock -3:-3. The reference tune's constants, **borrowed and not measured here**: there is no still-image clip in the corpus, and inventing one to fit a tune would measure the clip. |
| `fastdecode` | No deblocking filter, CAVLC, no weighted biprediction. Everything that costs the decoder, off; it is a real quality loss on purpose. Explicit P weighted prediction also belongs in it and is not there yet, because `--weightp` does not exist. |

`zerolatency` and `animation` only apply their frame-type changes if you did not
set `--bframes` yourself. An unknown tune name is an error.

## Which options change the bitstream shape

Worth knowing before you A/B anything:

- `--cqm flat` writes no scaling matrix at all, so it is byte-identical to a
 decoder using the default flat-16.
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
- Since 2026-09-16 the SPS declares the vertical motion-vector bound it is
  actually held to (`log2_max_mv_length_vertical`) instead of a flat 16, which
  advertised +-16384 luma samples at every level. It costs the SPS 0 or 1 byte
  and moves no picture, but it does mean **a bitstream cmp against a binary
  older than that differs in the SPS NAL**; compare `--dump-recon`, or compare
  everything after the first NAL.

## Threading, and what it does to your output

`--threads N` is a budget, not a worker count. The CLI splits it into `g` GOP
workers times `k` in-frame row-wavefront threads, sizing each GOP's share by the
frames it actually owns, and prints what it chose:

```
yah264: encoded 250 frame(s) in 2 GOP(s) on 2 GOP-worker(s) x 4 frame-thread(s)
```

**The determinism guarantee is: same input, same config, same thread count gives
the same output, bit for bit.** Output may differ across *different* thread
counts. That is deliberate, and x264 does not offer the stronger
thread-count-invariant guarantee either. The mechanism is that the in-frame
wavefront prices predecessor context slightly differently from the serial path,
so `k=1` and `k>=2` can differ (all `k>=2` agree with each other). If you need
reproducibility across machines with different core counts, pin `--threads`.

`--dump-recon` forces the fully serial path, because the recon stream has to be
a single continuous self-consistent encode. Two-pass falls back to it as well in
the cases below. **A `--threads N` greater than 1 that cannot be honoured says
so on stderr and names the condition** rather than silently encoding on one
thread:

```
yah264: warning: --threads 8 cannot be honoured, encoding serially: --dump-recon needs one continuous self-consistent recon stream, which per-GOP encoders cannot produce
```

It fires only on an explicit `--threads` above 1. Leaving it unset means "auto,
every core", which is not a request, and warning on it would fire on all 252
`--dump-recon` runs of the conformance gate.

**4:2:2 and 4:4:4 thread.** The encoder codes all three chroma formats through
I/P/B on both entropy coders, and the GOP-parallel reader's frame store reads
the subsampling off the Y4M `C` tag, the same as the serial path and the recon
dumper.

Two-pass runs on the parallel path in both passes, splitting the stats file
along GOP boundaries. Pass 2 needs a stats file that a *threaded* pass 1 wrote:
one from a serial pass 1 has no GOP markers to split on, so pass 2 quietly stays
serial for it. Pass 2 also refuses outright, with a message, if the stats file
describes a different GOP split than the current run computed, which is what
happens if you change `--frames` or `--keyint` between passes.

### Memory: the parallel path streams through a bounded window

The parallel path reads the input on its own thread and keeps only a window of
it resident. A GOP worker owns its frames and frees them when it publishes, so
what stays in memory is however many GOPs are in flight plus one GOP of
read-ahead: `(--threads + 1) x --keyint` frames. Clip length is not the ceiling.
The window is, and you set it with the two flags you were already setting.

Measured on 720p at `--threads 1 --keyint 50`, peak RSS windowed against holding
the whole input resident:

| Input frames | Whole input resident | Windowed |
| --- | --- | --- |
| 180 | 503 MB | 378 MB |
| 450 | 930 MB | 384 MB |
| 900 | 1631 MB | 384 MB |
| 1800 | 3035 MB | 385 MB |

The windowed column is flat: 1.559 MB per frame of clip becomes 0.004. Set
`Y264_STREAM_STAT=1` and the encoder reports what the window actually held,
which is 100 frames of 100 at every one of those lengths.

The window is generous, though, and it is worth sizing before you are surprised
by it. On one thread at the default keyint it is 500 frames, so 720p settles
around 975 MB where x264 sits at 167 flat; at 18 threads it is 4750 frames, 14
GiB at 1080p. Lower `--keyint`, lower `--threads`, or set `Y264_STREAM_WINDOW`.

Two things still read the whole input. `Y264_CUT_SPLIT=1` pre-scans for scene
cuts, and since the boundaries it finds are what the dispatcher schedules on,
there is nothing to dispatch until the scan has run, so it keeps the whole-input
read and a whole-clip memory ceiling. Two-pass needs the GOP split before pass 1 writes its
per-GOP stats, so it needs a seekable input, and it has to read the input twice
anyway.

Frame counts come from the file length rather than from reading frames. A Y4M
frame is a fixed-size record once the stream header is past, so yah264 reads
the first `FRAME` header, seeks back, and checks that the remainder divides
exactly. That matters for more than the memory guard: every scheduling decision
on this path is a function of the frame count, so knowing it up front keeps the
schedule, and the bitstream, identical to what the whole-input read produced. A
pipe has no length. It gets the same schedule anyway, by waiting for either EOF
or the `--threads + 1`'th GOP boundary before it dispatches, whichever comes
first; past that point the frame count no longer changes the answer.

The memory refusal measures the window rather than the clip, so it fires on a
machine too small for the parallelism you asked for rather than on a clip too
long for the box. The limit is half of **physical** RAM, overridden by
`Y264_MAX_INPUT_MB` in MiB. Physical rather than free: on macOS most of what
`vm_stat` calls inactive, speculative or purgeable is reclaimable on demand, so
a free-page gate refuses encodes that would have run and answers differently
between two runs of the same command.

```
yah264: this encode needs 41.7 GiB of memory and the limit is 32.0 GiB
yah264: the threaded path streams, but its window is (--threads + 1) x --keyint = 14250 frame(s); lower either, or set Y264_STREAM_WINDOW
yah264: limit 32.0 GiB (50% of 64.0 GiB physical), 2.97 MiB resident per 1920x1080 frame, so 11046 frame(s) fit
yah264: encode a segment with --frames, split the input, or raise Y264_MAX_INPUT_MB
```

A raw 4:2:0 8-bit frame is `w*h*1.5` bytes; 4:2:2 is `w*h*2` and 4:4:4 is
`w*h*3`, and the guard computes from the real geometry. Riding above the window
without scaling with it is a fixed per-worker encoder cost, around 330 MiB for
one 1080p worker and another 50 for a second.

## x264 compatibility

Options that match x264 in **name and semantics**, so a command line ports
across unchanged:

`--preset`, `--tune` (the six names listed above), `--bitrate`, `--qp`,
`--keyint`, `--min-keyint`, `--no-scenecut`, `--bframes`, `--b-adapt`, `--ref`,
`--cabac`/`--cavlc`, `--no-transform-8x8`, `--psy-rd`, `--psy-trellis`,
`--aq-strength`, `--rc-lookahead`, `--sync-lookahead`, `--vbv-maxrate`,
`--vbv-bufsize`, `--pass`, `--stats`, `--threads`, `--frames`, `--sar`,
`--level`, `--me`, `--direct`, `--cqm`, `--aud`, `--pic-struct`,
`--frame-packing`, `--cll`, `--mastering-display`, `--alternative-transfer`,
`--overscan`, `--videoformat`, `--fake-interlaced`, `--tff`, `--bff`,
`--no-interlaced`, `--seek`, `--crop-rect`,
`--input-res`, `--input-csp`, `--fps`, `--quiet`, `--log-level`,
`--no-progress`, `--no-psy`, `--no-dct-decimate`,
`--no-fast-pskip`, `--no-mbtree`, `--no-asm`, `--deblock`, `--no-deblock`,
`--no-weightb`, `--constrained-intra`, `--chroma-qp-offset`, `--qpmin`,
`--qpmax`, `--qpstep`,
`--vbv-init`, `--sps-id`, `--slices`, `-o`.

Options that differ, and how:

| Option | The difference |
| --- | --- |
| `--crf` | **The number is not comparable to x264's.** Same CRF value has measured a size difference from -54.6% to +45.3% against x264 across the corpus. Do not port a CRF setting across. See [rate-control.md](rate-control.md). |
| `--crf` | Fractional values are accepted but largely inert; the quantiser rounds to an integer QP. |
| `--crf 0` | x264's lossless. Not implemented here, and refused rather than accepted, because `rc.rf = 0` means "CRF unarmed" in the param struct. |
| `--direct` | x264's `none` is not implemented and is refused. `auto` (the default, as in x264) picks per slice by skippability; `spatial` and `temporal` behave the same as x264's. |
| `--qp` | x264 forces mb-tree and AQ off at constant QP. yah264 forces AQ off *at the CLI* but leaves mb-tree running. `--qp 26` is not the same workload on both encoders. |
| `--scenecut` | On the CLI, `--scenecut 0` means off, same as x264. In the C API, `param.scenecut = 0` means **default (40)** and off is spelled with a negative. Assign `YAH264_SCENECUT_OFF`; see [Calling it from C](#calling-it-from-c). |
| `--sync-lookahead` | Same 0-means-off spelling on the CLI, same negative-means-off idiom in the API. Assign `YAH264_SYNC_LOOKAHEAD_OFF`. |
| `--input-y4m` / `--input-raw` | x264 sniffs the input format; here the two are separate flags and nothing is guessed. |
| `--tune stillimage` | The reference's constants, not measured here. |
| `--tune fastdecode` | Does not yet clear explicit P weighted prediction, because there is no `--weightp` to clear it with. |
| bare default | x264 defaults to CRF 23; yah264 defaults to QP 26. |
| `--cqm` | x264 takes `flat`/`jvt` plus custom file forms; only `flat` and `jvt` here. |
| `--subme` | Also selects the search method here: with no `--me`, below 8 is hex and 8 or above is UMH. x264 keeps effort and method independent. `--subme 0` is refused; see the coding-tools section. |
| `--merange` | Only UMH reads it. x264's applies to hex and esa too. |
| `--qcomp` | Reaches the ABR curve and mb-tree strength; the CRF and two-pass curves carry their own. x264's applies to all. |
| `--deadzone-inter`/`-intra` | Same values, same inversion, but passing either also swaps the exact shipped quant expression for its 1/64 approximation, so x264's own defaults measure +0.23% rather than 0. |
| `--profile` | Refuses a tool you named rather than dropping it, where x264 narrows silently. `high10` needs a 10-bit build. |
| `--stitchable` | Pins the SPS only, and raises the declared level (and with it the MV range), so it changes the encode and not only the header. |
| `--pic-struct` | Always writes pic_struct 0, a progressive frame, because there is no field coding to write anything else for. |
| `--b-pyramid` | `none` and `normal` only; x264's `strict` is refused rather than read as `normal`. |
| `--mvrange` | Narrows the level's bound and never widens it. x264 lets a `--mvrange` above the level's stand. |
| `--pass 3` | Same meaning as x264's -- read the stats and write them back -- but it rewrites the file **in place**, so keep a copy if you want the pass-1 records afterwards. |
| `--aq-mode` | `0` is refused here. x264's 0 turns AQ off; this value is a metric selector with no off seat, so 0 would encode as 1. Spell AQ off `--aq-strength 0`. |
| `--ipratio`/`--pbratio`/`--cplxblur`/`--qblur` | Two-pass only. x264 applies its ratio pair to every mode; the single-pass modes here anchor I and B through the CRF track and never read them. |

x264 options with **no equivalent at all**: `--weightp`,
`--open-gop`, `--nal-hrd`, `--muxer`/`--demuxer`. `--slice-max-size` and
`--slice-max-mbs` are not here either: `--slices` takes a count, not a size
cap. x264's `--interlaced` is `--tff`/`--bff` here, and it codes field
pictures rather than MBAFF.

The absence of `--nal-hrd` matters for delivery: **yah264 writes no HRD
parameters into the SPS**, even when VBV is active. See the guarantee discussion
in [rate-control.md](rate-control.md).

## Calling it from C

Everything above describes the CLI, which is x264-compatible where it claims to
be. The C API in `include/yah264.h` matches x264's field values, with three
deliberate divergences: `scenecut`, `sync_lookahead` and `subme`, all of them
the zero-as-unset convention rather than a numbering choice. Read this before
you port anything.

None of it applies if you use the CLI. The CLI maps each flag onto the values
below for you.

**Zero means the default, not off.** `yah264_param_t` fills itself
from `yah264_param_default`, and twenty of its fields treat zero as "unset,
pick the default". That convention is fine until a field's off switch is a value
x264 spells as zero. Two fields are in that position:

| Field | x264 spells off as | Zero here means | Off here is |
| --- | --- | --- | --- |
| `scenecut` | `--scenecut 0` | the default, **40**, which is x264's own aggressiveness | `YAH264_SCENECUT_OFF` |
| `sync_lookahead` | `--sync-lookahead 0` | auto, a lead of `bframes+1` | `YAH264_SYNC_LOOKAHEAD_OFF` |

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
| `me_method` | DIA 0, HEX 1, UMH 2 | x264's, plus auto at **-1** |
| `direct` | NONE 0, SPATIAL 1, TEMPORAL 2, AUTO 3 | the same values; NONE refused, AUTO is the default |
| `csp` | I400 1, I420 2, I422 6, I444 12 | x264's **I420 2, I422 6, I444 12** |
| `subme` | 0 = fastest | 0 = the library default 10, the slowest; see below |

`YAH264_ABI_VERSION` is 1. Assert on it if you want a build-time tripwire, and
`#ifndef` it if you also build against headers that predate it.

#### Where matching x264 exactly is not possible

Four places, each deliberate rather than a half-match:

- **`YAH264_RC_2PASS` is 100.** x264 has no 2-pass rc method; it spells 2-pass
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
 on top. yah264 implements none of that: the depth is a property of the LIBRARY you
 called, not a bit in the csp, so `X264_CSP_I420 | X264_CSP_HIGH_DEPTH` is not
 4:2:0 to us, it is an unknown value and open fails. Pass one constant, do not
 mask.
 Everything in the gaps (I400, NV12/NV21, YV12/YV16, YUYV/UYVY/V210, YV24, the
 RGB family) is likewise refused rather than approximated.
- **`rc.rf` is a `double`, not x264's `float`.** Assigning `f_rf_constant` to a
 double is exact, so porting works. `0` leaves CRF unarmed rather than meaning
 x264's lossless `--crf 0`, which is the zero-as-unset convention above rather
 than a scale problem. The CLI rounds `--crf` to tenths; ask through the API for
 finer.

#### `subme` follows x264's scale except at zero

The scale is **not inverted and not offset**. It runs the same direction as
x264's subpel level (higher = slower, more RD) and the tiers line up. The
only disagreement is at zero: `0` here is the library default **10**, the
slowest setting, where x264's `0` is a real mode and its fastest. That is the
zero-as-unset convention shared with nineteen other fields, and renumbering one
of them would swap a divergence you can document for an inconsistency you
cannot. Porting `0` for speed maximises effort instead. **Ask for `1`.**

### Fields that are genuinely fine

Everything else either agrees with x264 at zero or has no x264 equivalent.
`rc.qp`, `bframes`, `ref`, `keyint`, `aq_strength`, `psy_rd`, `psy_trellis`,
`cabac`, `transform8x8`, `sar_*` and the `vbv_*` pair all read zero the way an
x264 user expects; `keyint_min` 0 is auto in both. Three notes that are not
traps but will still surprise:

- **`param.threads` is a budget for one encoder instance**, not a width: 0 asks
 the library to pick (the machine's thread count, capped at 16), 1 is serial and
 N means up to N. A library caller who sets `threads = 8` gets a wavefront of 8.
 `frame_threads` is still the explicit low-level width, and it wins when set,
 which is what lets the CLI's GOP workers carry their own share.
- **`param.annexb` is read by nothing at all.** Annex-B is the only output mode.
- **`badapt` 2 and `cqm` 2** are silently narrowed rather than rejected:
 `X264_B_ADAPT_TRELLIS` becomes fast b-adapt, `X264_CQM_CUSTOM` becomes JVT.

## Environment variables

The encoder reads **over 300 distinct `Y264_*` environment variables** in C
code; [knobs.md](knobs.md) is generated from the source and carries the count.
Almost none of them are features. They are how this project ships an experiment
without a rebuild, and the great majority are measurement scaffolding that
happens to be reachable from your shell.

They are tiered below by whether you have any business setting them. A rule of
thumb: if it is not in the first table, the answer is no.

Shared semantics worth knowing before you set any of them:

- **Most are read once and cached** in a lazy static, warmed on the main thread
 at encoder open. Changing one mid-process does nothing. Three re-read per
 call: `Y264_MBTREE_MVLAMBDA`, `Y264_ABR_QCOMP` on the serial path, and
 `Y264_DBG_CPLX`.
- **Some are presence-only**, so setting them to `0` still turns them *on*:
 `YAH264_NO_ASM`, `Y264_RC_TRACE`, `Y264_TP_DBG`, `Y264_MBTREE_DBG`,
 `Y264_MBT_PRE_DBG`, `Y264_DBG_CPLX`. `Y264_WF_THREADS` is half-presence-only:
 its mere presence disables the thread-cap clamp, separately from its value.
- **Some test one literal character.** `Y264_ME_LAMBDA` and `Y264_WF_PREDQP`
 only act on exactly `0`; `Y264_VITERBI` acts on anything that is not `0`;
 `Y264_NO_SCENECUT` only acts on exactly `1`, so `=2` does nothing.
- **Some are scaled.** `Y264_TP_BEXP`, `Y264_TP_IPF` and `Y264_TP_PBF` are
 percentages divided by 100. The four `Y264_NTP_SPIN*` are microseconds.

### Tier 1: worth knowing about

Escape hatches, and knobs with no CLI equivalent.

| Variable | Default | What it is for |
| --- | --- | --- |
| `YAH264_NO_ASM` | asm on | Force every scalar C path. Presence-only, so `=0` also disables asm. Promoted to `--no-asm`, and still overrides it. |
| `Y264_SUBPEL` | preset | The subpel pattern: 0 square, 1 diamond, 2 capped diamond. Promoted to `--subpel`, and still overrides it. |
| `Y264_UMH_RANGE` | 16 | UMH search radius in integer pels. Promoted to `--merange`, and still overrides it. |
| `Y264_NO_UMH` | unset | Overrides `--me` and the preset gate entirely. 1 forces hex, 0 forces UMH. |
| `Y264_ABR_QCOMP` | 0.6 | The ABR rate curve's compression, and the mb-tree strength derived from it. Promoted to `--qcomp`, and still overrides it. |
| `Y264_AQ_DC` | 1.0 | The AQ frame-mean term's strength (x1.0397 like the per-MB AQ), added to the CRF base QP as its difference from the per-MB strength: the across-shot allocation term, at x264's AQ strength. `0.4` (= `--aq-strength`) reproduces the pre-2026-09-05 output. CRF only. |
| `Y264_DZ_INTRA` / `Y264_DZ_INTER` | unset | Quantiser rounding bias in 1/64 units, the encoder's own scale, **not** x264's flag value, which is `32` minus this. Promoted to `--deadzone-intra`/`--deadzone-inter`, which do the inversion; both still override. |
| `Y264_AQ_MODE` | 2 | 1 is log2-variance AQ, 2 and above is x264 aq-mode 2. Promoted to `--aq-mode`, and still overrides it. |
| `Y264_FAST_PSKIP` | 1 (on) | The cheap P_Skip pre-test at `--subme` 8 and below. Promoted to `--no-fast-pskip`, and still overrides it. |
| `Y264_DCTDEC` | 1 (on) | Coefficient decimation. Promoted to `--no-dct-decimate`, and still overrides it. |
| `Y264_AQ_DARK` | 0 (off) | Dark-region AQ bias, around 0.5 to 1.0. |
| `Y264_TP_PLAN` | **1 (on)** | The two-pass offline allocator. 0 selects the ranking allocator instead, which is far worse. |
| `Y264_2PASS_MT` | on | 0 forces two-pass onto the serial path, reproducing the serial output exactly. |
| `Y264_MBTREE_OFF` | 0 (off) | Skip mb-tree entirely, which is x264's own CQP policy. Set it when comparing `--qp` runs against x264. Changes bits. Promoted to `--no-mbtree`, and still overrides it. |
| `Y264_CUT_SPLIT` | 0 (off) | Split GOP workers on real scene cuts instead of arithmetic boundaries. Worth up to 17% of wall on multi-shot clips. Bitstream unchanged. Its pre-scan needs every frame at once, so it turns streaming off and brings back the whole-clip ceiling, plus a second whole-clip array on top (~+18%). |
| `Y264_SHOT_QCOMP` | 0.6 | `--shot-crf`: the compression of the per-shot curve, `6(1-qcomp)` QP per doubling of the shot's cost against the title's. |
| `Y264_SHOT_CLAMP` | 4 | `--shot-crf`: the per-shot CRF offset's bound, in QP. |
| `Y264_SHOT_MIN` | 24 | `--shot-crf`: shots shorter than this many frames take their predecessor's offset. |
| `Y264_MAX_INPUT_MB` | 50% of physical RAM | How much memory the input window may take, in MiB. It refuses rather than being OOM-killed. Raise it if you know the box can take it; lower it to test the refusal. |
| `Y264_STREAM_WINDOW` | `(--threads + 1) x --keyint` | Input frames held resident, overriding the default window. Below `2 x --keyint` the reader could block before a whole GOP is dispatchable, so that is the floor. Scheduling-only: the bitstream does not move with it. |
| `Y264_STREAM_STAT` | 0 (off) | Report the window's high-water mark at the end of the encode: frames actually resident, and compressed bytes published but not yet written. |
| `Y264_WF_THREADS` | from `--threads` | Sets the wavefront width directly **and bypasses the critical-path cap**. The only way to probe above the knee. |
| `Y264_LA_BUF` | auto = `bframes+1` (4 at medium) | Overrides `--sync-lookahead`, **in both directions**: the auto default resolves a lead of `bframes+1` whenever there is a pool, so `=1` and `=0` REDUCE it and cost 1-6% of wall. Costs latency, and changes no bits **while the decoupled chain is on**, which is whenever the knob has any effect; forced together with `Y264_LA_THREAD=0` it changes the bitstream deterministically. |
| `Y264_NTP_SPIN` | 25 (us) | Thread-pool spin budget before sleeping. Worth tuning on unusual core counts. |
| `Y264_CRF_CPLX` | 1 (on) | Behaviour-matched CRF content adaptation, on by default. Narrows the equal-CRF spread against x264 from 100 points to 41, and improves 9 of 11 corpus clips at the tuned anchor, worst +1.54%. 0 turns it off. |
| `Y264_CRF_FPS` | follows `CRF_CPLX`, so on | Frame-duration term, so CRF N is the same operating point at 24 and 50 fps. A correctness fix rather than a tuning one. |
| `Y264_AQ_CHROMA` | 0 (off) | Sums chroma into the AQ energy, as x264 does when it measures a macroblock's energy. Faithful, and measured neutral: it costs 9% of the mb-tree bucket and buys +0.35..-0.29% BD. Off even under `CRF_CPLX`. |

Two you should know exist so you never set them:

| Variable | Why not |
| --- | --- |
| `Y264_WF_PREDQP=0` | Escapes to the true raster QP chain, which makes analysis **non-deterministic across thread counts**. The default of 1 is what holds the determinism guarantee up. |
| `Y264_UNSAFE_NO_REFBWAIT`, `Y264_UNSAFE_NO_PREVPWAIT` | Deliberately racy. They drop synchronisation waits to measure a ceiling. The output is not trustworthy. |
| `Y264_UNSAFE_NO_EMIT`, `Y264_UNSAFE_NO_NAL` | Deliberately broken. They delete the entropy emit and the NAL assembly to price the emission path. The bitstream is invalid; the reconstruction is unaffected, which is the point. |

### Tier 2: calibration knobs

These exist so a constant can be swept against x264 without a rebuild. They have
defaults that were measured, and moving one moves quality in ways that have
usually already been tested and rejected. Listed so you know what you are
looking at if you find one in a script, not as a tuning surface.

**Motion and mode decision:** `Y264_HPEL_SAD`, `Y264_HPEL_THRESH`,
`Y264_ME_SMALL_NOUMH`, `Y264_ME_LAMBDA`, `Y264_TEMPORAL_SEED`,
`Y264_RICH_SEEDS`, `Y264_LR_SEED`, `Y264_B_SEEDS`, `Y264_LR_ME`, `Y264_TR_PRE`,
`Y264_TRELLIS_COMMIT`, `Y264_VITERBI`, `Y264_RDOQ_SEED64`, `Y264_PSY_TRELLIS`,
`Y264_CABAC_RD`, `Y264_EST_CTX`, `Y264_DCTDEC`, `Y264_DCTDEC_T4`,
`Y264_DCTDEC_T8`, `Y264_QPELRD`, `Y264_QPELRD_HYST`, `Y264_QPELRD_LUMA`,
`Y264_INTRA_SKIP`, `Y264_INTRA_FINE_M`, `Y264_INTRA_SCREEN`,
`Y264_INTRA_ADMIT_M`, `Y264_INTRA_SCREEN_PURE`, `Y264_P_RECT`,
`Y264_PART_EARLYTERM`, `Y264_PART_THRESH`, `Y264_PART_IMPORTANT`,
`Y264_PART_HETERO`, `Y264_B_THRESH`, `Y264_B_RECT`, `Y264_BPO`,
`Y264_PROBE_TRELLIS`, `Y264_RD_ADMIT`, `Y264_RD_ADMIT_MARGIN`, `Y264_MIDSKIP`,
`Y264_MIDSKIP_MARGIN`, `Y264_ADME`.

**AQ and rate control:** `Y264_AQ2_BIAS`, `Y264_AQ_BOOST`, `Y264_AQ_OCTILE`,
`Y264_AQ_ANCHOR`, `Y264_CRF_CL`, `Y264_CRF_AQABS`, `Y264_CRF_PB0`,
`Y264_CRF_PED`, `Y264_CRF_PBSCALE`, `Y264_CRF_BASE`, `Y264_CRF_SLOPE`,
`Y264_CRF_CAP`, `Y264_CRF_CL_SHIFT`, `Y264_RCP_GAIN`, `Y264_RCP_WARM`,
`Y264_RCP_QPD`, `Y264_VBV_RHI`, `Y264_VBV_QPD`, `Y264_VBV_CJUMP`,
`Y264_TP_DIFFLIM`, `Y264_TP_CORR`, `Y264_TP_RESOLVE`, `Y264_TP_BEXP`,
`Y264_TP_IPF`, `Y264_TP_PBF`, `Y264_TP_CPLXBLUR`, `Y264_TP_QBLUR`,
`Y264_TP_CWARM`.

**mb-tree and lookahead:** `Y264_MBTREE_WHOLEBUF`, `Y264_MBTREE_IPPP`,
`Y264_MBTREE_STRENGTH`, `Y264_MBTREE_BFIX`, `Y264_MBTREE_MVLAMBDA`,
`Y264_MBTREE_CENTER`, `Y264_MBTREE_PROP_INVQ`, `Y264_MBTREE_BOTHLIST`,
`Y264_MBTREE_ADAPT`, `Y264_MBTREE_AINT`, `Y264_MBTREE_ASLOPE`,
`Y264_MBTREE_ALO`, `Y264_MBTREE_AHI`, `Y264_LOWRES_COH`, `Y264_LR_INTRA_NEIGHBOUR`,
`Y264_LR_REUSE`, `Y264_LA_THREAD`, `Y264_LA_INLINE`, `Y264_LA_POOL_MIN`.

**Threading and scheduling** (all byte-identical unless noted): `Y264_W2`,
`Y264_FPIPE`, `Y264_STAIR`, `Y264_STAIR_DEPTH`, `Y264_STAIR_WIDE`,
`Y264_STAIR_EVICTPOOL`, `Y264_RC_PIPE`, `Y264_RC_PIPE_VBV`, `Y264_HPEL`,
`Y264_GOP_EVEN`, `Y264_NTP_SPIN_ROW`, `Y264_NTP_SPIN_JOIN`,
`Y264_NTP_SPIN_IDLE`, `Y264_F1`, `Y264_F3`, `Y264_F3C`.

`Y264_MBTREE_CENTER` deserves a warning: it has two read sites with *different*
defaults, so its effective default depends on which mb-tree path is live.

`Y264_VBV_BOUND` (default **0**) belongs to both of the lists above and to
neither cleanly, which is why it is called out here. It bounds every frame
against its measured coded size instead of only the instance's first, and it
reaches the concurrent routes by disengaging the stair and fpipe, so it is a
rate-control change *and* a scheduling one. It only does anything under a VBV
cap with no bitrate target and no pass-1 record (capped VBR and CQP+VBV); every
other mode is byte-identical with it on. On: `scripts/cvbr_compliance.sh` goes
from 34/36 to 36/36 at both windows, quality is neutral-to-positive in the mode,
and wall clock is ~1.5-1.6x.

### Tier 3: measurement scaffolding, not features

Profiling counters, trace dumps, deliberately-racy ceiling probes, and gates on
experiments that were measured and rejected. Setting any of these gets you
diagnostics or a known-worse encode. Several change bits.

`Y264_THREAD_PROF`, `Y264_NTP_PROF`, `Y264_NTP_STATS`, `Y264_NTP_PARK`,
`Y264_STAIR_STAT`, `Y264_VBV_STAT`, `Y264_RCP_DBG`, `Y264_RC_TRACE`,
`Y264_TP_DBG`, `Y264_MBTREE_DBG`, `Y264_MBT_PRE_DBG`, `Y264_DBG_CPLX`,
`Y264_LA_STAT`, `Y264_ME_STATS`, `Y264_MB_LOG`, `Y264_SKIP_ORACLE`,
`Y264_EST_CHECK`, `Y264_CUT_SPLIT_STAT`, `Y264_ADME_LOG`,
`Y264_PROBE_DEADZONE`, `Y264_HEX_ORACLE`, `Y264_STAIR_BDEPTH`,
`Y264_STAIR_MULTIHOP`, `Y264_STAIR_WIDE_REF`, `Y264_STAIR_REFBGATE`,
`Y264_STAIR_REFBEARLY`, `Y264_STAIR_LEAFRUN`, `Y264_STAIR_LAG_FORCE`,
`Y264_RCP_LAG`, `Y264_RCP_LAG_NOWIDE`, `Y264_VBV_FORCE`,
`Y264_UNSAFE_NO_REFBWAIT`, `Y264_UNSAFE_NO_PREVPWAIT`, `Y264_UNSAFE_NO_EMIT`,
`Y264_UNSAFE_NO_NAL`, `Y264_NO_SCENECUT`,
`Y264_MBT_PRE`, `Y264_DPB_POOL`, `Y264_GOP_FORCE_G`, `Y264_GOP_FORCE_K`.

Note `Y264_NO_SCENECUT` is **not** a user-facing way to disable scene cuts; it
is a diagnostic that isolates a threading barrier. Use `--no-scenecut`.

`Y264_MB_LOG` and `Y264_SKIP_ORACLE` take a **path**, not a boolean.

### Not read by the encoder

These appear in scripts and tests only, never in C: `Y264_CRF`,
`Y264_CRF_CACHE`, `Y264_REFENC_CACHE`, `Y264_STRESS_ABR`, `Y264_TRELLIS_PRINT`,
`YAH264_ARGS`, `YAH264_DESC`, `YAH264_ENC`, `YAH264_CONF_*`, and
`YAH264_VMAF_MODEL` (used by `scripts/vmaf.sh` and `scripts/bdcompare.py`).

Some names that look like environment variables are compile-time macros and
cannot be set from a shell: `Y264_STAIR_K`, `Y264_STAIR_HOPS`,
`Y264_MT_POOL_MIN`, `Y264_STAIR_LAG`, `Y264_LA_CAP_MAX`, `Y264_DPB_POOL_MAX`,
`Y264_BIT_DEPTH`.

### Comments that lie about their defaults

The comments beside the readers in `src/encoder/encoder.c` now agree with the
code for all four gates this section used to list. What is still stale is the
struct-field comments in `src/encoder/encoder.h`, which describe an era when
these were off. Trust the reader, or the census in
[knobs.md](knobs.md), and not the header:

| Variable | Header comment claims | Code does |
| --- | --- | --- |
| `Y264_RC_PIPE` | default off | defaults to **1** |
| `Y264_RC_PIPE_VBV` | default off | defaults to **1** |
| `Y264_STAIR` | default off | defaults to **1** |
| `Y264_W2` | default off | **on whenever there is a thread pool** |
| `Y264_ABR_EARLY` | default 0, a probe | defaults to **2**, the shipped drain split |

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
