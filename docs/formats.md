# Bit depth and chroma formats

yah264 encodes 8-bit and 10-bit video at 4:2:0, 4:2:2 or 4:4:4. All of them
recon-match ffmpeg's decoder across I, P and B frames on both entropy coders.
One `y264` binary handles both depths and picks the encoder from the input.

The tuned path is 8-bit 4:2:0. The 10-bit path keeps the 8-bit
rate-distortion trades, and 8-bit content coded at 10 bits gets the frame types
of its 8-bit encode. On the 1080p HDR10 test clips the 10-bit path beats x264
High 10 on coding efficiency (see the HDR ladder under "Measuring it").

## How it works

### Both depths in one binary

The sample width is a compile-time type that `src/common/bitdepth.h` sets
from `Y264_BIT_DEPTH`. That makes two encoder libraries:

| | 8-bit | 10-bit |
|---|---|---|
| `pixel` | `uint8_t` | `uint16_t` |
| `dctcoef` (coefficients, transform intermediates) | `int16_t` | `int32_t`, because the residual reaches ±1023 and the transform butterflies overflow 16 bits |
| `PIXEL_MAX` | 255 | 1023 |
| `Y264_QP_BD_OFFSET` | 0 | 12 |
| QP_Y range (spec) | 0 to 51 | -12 to 51 |
| profile at 4:2:0 | Baseline, Main or High, per options | High 10, profile_idc 110 |
| SIMD | every tier ([simd.md](simd.md)) | NEON on aarch64; SSE4.2 and AVX2 on x86-64 ([simd.md](simd.md)) |
| GPU offload | available, off ([gpu.md](gpu.md)) | stubbed out |
| library | `libyah264`, `yah264.pc` | `libyah264_10`, `yah264_10.pc` |

Only true image samples use `pixel`. Bitstream bytes, nonzero counts, mode ids
and flags stay `uint8_t`.

Every build compiles both libraries. The 10-bit one force-includes
`src/common/symbols10.h`, whose `_10` suffix on its symbols lets both link into
one program. `scripts/symbol_check.sh` checks this in `make test`, because a
name defined in both would resolve by link order and show up only as wrong
pixels.

The public header has no `pixel` type. Picture planes are `void*` with a
stride in samples. Because each `.pc` file sets `Y264_BIT_DEPTH` in its
`Cflags`, `yah264_encoder_open()` resolves to the library the caller
configured against.

The meson `bit_depth` option defaults to 8. It only sets the depth that
checkasm, the unit tests and the in-tree tools build at. CI also runs the
unit tests under `-Dbit_depth=10`.

### Choosing the depth

| input | result |
|---|---|
| Y4M `C` tag | `C420` codes 8-bit, `C420p10` codes High 10. Same for 4:2:2 and 4:4:4 |
| raw input | `--input-csp` names format and depth: `i420`, `i422` or `i444`, optionally with `p10` |
| `--output-depth 10` on 8-bit input | each sample shifts left by 2 and codes High 10 |
| `--output-depth 8` on 10-bit input | refused. Dropping bits is a conversion, and that's ffmpeg's job |
| a 16-bit sample above the depth's maximum | exit status 1. Such a file isn't 10-bit video |

The [ffmpeg wrapper](ffmpeg.md) chooses the same way from the input
`pix_fmt`.

### Chroma formats

The encoder context holds `cf_idc` and the spec's SubWidthC and SubHeightC
(`sub_w`, `sub_h`). Plane sizes, strides, padding, the nonzero grid and the
chroma macroblock size all derive from those two. The shared geometry has no
4:2:0 constants.

| | 4:2:0 | 4:2:2 | 4:4:4 |
|---|---|---|---|
| `chroma_format_idc` | 1 | 2 | 3 |
| profile | per options | High 4:2:2 | High 4:4:4 Predictive |
| profile_idc | per options | 122 | 244 |
| chroma macroblock | 8x8 | 8x16 | 16x16 |
| chroma DC | 2x2 Hadamard, 4 coefficients | 2x4 transform, 8 coefficients | none, except a luma-style 4x4 DC per plane inside I_16x16 |
| intra chroma prediction | chroma modes, 8x8 | chroma modes, 8x16 | inherited from the co-located luma block, not signaled |
| chroma MC | eighth-pel bilinear | eighth-pel x, quarter-pel y | luma 6-tap, quarter-pel (chroma MV equals luma MV) |
| CAVLC chroma DC | `nC = -1` | `nC = -2` | luma-style `nC` per plane |
| coded block pattern | separate chroma cbp | separate chroma cbp | chroma folded into the luma cbp |
| deblock | chroma filter, edges 0 and 4 | chroma filter, horizontal edges 0, 4, 8, 12 | luma-strength filter, edges like luma |
| scaling lists in the SPS | 8 | 8 | 12 |
| CABAC contexts | 460 | 460 | 1024 |

At 4:4:4, Cb and Cr are coded exactly like luma, in three residual passes,
each plane with its own copy of the luma context categories. Their CABAC
context ranges initialize by copying luma's, which matches the spec's init
tables.

| limit outside 4:2:0 | behavior |
|---|---|
| 8x8 transform at 4:4:4 | never used. `--transform-8x8` drops to 4x4 at open. The 4:4:4 chroma 8x8 quant-matrix keys are accepted and unused |
| field coding (`--tff`/`--bff`) | 4:2:0 only. An explicit request on 4:2:2 or 4:4:4 is refused. An interlaced Y4M tag is ignored with a warning |
| SIMD | none for the vertical chroma deblock edge, all 4:4:4 chroma deblocking, or intra chroma at 4:4:4. These run in C |
| input layouts | no I400, NV12, YV12 or packed input. Convert with ffmpeg first |

Threading doesn't depend on the format. The GOP-parallel reader reads the
subsampling from the Y4M `C` tag like the serial path.

### Keeping 10-bit on the 8-bit trades

We tune at 8 bits. A 10-bit sample's error is four times the 8-bit one, so
SATD grows 4x and SSD 16x. The 10-bit path puts its costs back on the 8-bit
scale in five places:

| place | at 10 bits |
|---|---|
| lambdas | mode, trellis and motion lambdas are read at the QP plus the depth's QP offset. Six QP per extra bit scales them by the same factors as SATD and SSD. The psy terms scale with the sample scale |
| lookahead | lowres planes hold 8-bit samples at every depth. B placement, scene cuts, mb-tree and CRF's complexity read the costs their constants were fitted on |
| rate-control complexity | the full-resolution intra SATD that opens a bitrate-target encode and prices its I frames is divided by 4 |
| adaptive quantization | every AQ path divides a macroblock's variance by 4 per extra bit before reading its energy as `log2(variance + floor)`. The floors, the anchor and B-frame levels and the frame DC see 8-bit values. The flat psy-trellis gate's variance limit rises by the same factor |
| fixed thresholds | the I_16x16 screen's flatness limit for P and B macroblocks rises by the same factor, so 8-bit content coded at 10 bits gets its 8-bit verdict. The motion search's per-pixel early-out bound doubles per extra bit, like the SAD it's compared with |

## What we chose and why

| choice | why |
|---|---|
| Bit depth as a compile-time type | Keeps the hot path branch-free and gives each depth its own SIMD. A runtime-depth `pixel` would put a branch or a wider type in every 8-bit kernel. |
| Both libraries in every build, one binary | The input picks 10-bit with no rebuild. 8-bit output is byte-identical to the single-library build on the ten board clips at CRF, CQP and ABR, at one and eight threads. |
| `--output-depth 8` on 10-bit input refused | Rounding samples away is a conversion. Silent truncation would hide it. |
| The pixel-type change before any SIMD work | Widening `pixel` and `dctcoef` is the one change that would invalidate 8-bit kernels. Doing it first meant each kernel is written once per depth. |
| 10-bit SIMD as its own tiers | Each kernel is written for the depth's lanes and checked against the 10-bit C reference. The 8-bit kernels are untouched. |
| Recon-match against ffmpeg as the gate for each format | A format opens only once its whole chain decodes bit-exact. 4:2:0 output stayed byte-identical while each format went in. |
| Accepting the two-library dispatch cost | It's +0.3% to 0.6% wall time at one thread on foreman CIF and park_joy 720p, inside same-binary run-to-run spread. It doesn't scale with the CLI's share of the work, so binary layout is a likelier cause than the dispatch. |

## What didn't work

| idea | why not |
|---|---|
| A blind `uint8_t` to `pixel` replace | Most `uint8_t` uses aren't pixels. Each of about 440 uses in 27 files needed a judgment call. |
| 4:2:0-sized scratch buffers at 4:2:2 and 4:4:4 | They overflowed the stack. A 24-entry nonzero buffer took the 32-entry 4:2:2 save and corrupted luma recon in RD trials. Only AddressSanitizer caught it. |
| The general CABAC Gt1 context table for the 4:2:2 chroma DC | The 8-coefficient block reaches node 7, where the general table's ctxIdxInc of 9 spills into the chroma AC contexts. The 4:2:2 DC row caps it at 8. |

## Open questions

- 4:4:4 with the 8x8 transform needs the 4:4:4 8x8 residual coders and the
  CABAC 8x8 chroma context init values.
- There's no BD board against x264 for 4:2:2 or 4:4:4 yet. Both recon-match.
- AVX-512 at 10 bits. The x86 tiers stop at AVX2 (see [SIMD](simd.md)).

## Measuring it

### The 10-bit board

`scripts/board10.sh` runs both encoders at their default preset with
`--threads 4`. Its clip list and rate targets are `CLIPS_P10` in
`scripts/parity-clips.sh`.
`scripts/bd_at_rate.py` solves each clip onto five shared byte targets. x264
needs `--output-depth 10` as well as `--profile high10`, because the profile
alone still writes 8-bit samples.

The board is a smoke check. Its clips are 64 frames at 480x272, below the
project's 120-frame rule for a BD sweep.

Decode with `-fps_mode passthrough`. Otherwise ffmpeg drops frames when the
Y4M muxer picks a rate other than the one in the stream's VUI. On the 120 fps
clips only 15 of 64 frames survived. VMAF paired them with the first 15
reference frames.

### Scoring HDR

HDR clips score on VMAF v1's HDR models (`hdr_vmaf_v1.1.8`). The SDR models
score PQ pictures as if they were SDR, with no warning.

| picture | HDR model |
|---|---|
| height up to 1080 | 3d0h |
| height above 1080, such as 3840x1600 and 4096x2160 | 1d5h_2160 |
| 50 fps and up | the hfr variant of either |

Tag both sides limited range, BT.2020 primaries and matrix, and PQ transfer.
By the project's rule, a picture smaller than 1920x1080 in both dimensions is
scaled up on both sides to fit 1920x1080, aspect kept. Anything else scores
at its own size, 1080p and 2160p included. Read PSNR-Y on the 10-bit code
values too. Check that the two ladders overlap in quality before reading a BD
figure.

### The HDR ladder

`scripts/hdr_ladder.py SRC.y4m` runs CRF 18-34 on yah264 and on x264 High 10
at medium. Both streams are tagged HDR10, meaning BT.2020, PQ and limited
range. The source is a progressive, limited-range C420p10 HDR10 Y4M of any
even size. The 1080p test spans are 120 frames. Rungs score as in "Scoring
HDR". CAMBI is told the coded size when a source is scaled up. The script
prints BD-rate by HDR VMAF, PSNR-Y and PSNR-YUV over the shared quality range,
with each rung's CAMBI.

Set `VMAF_HDR` and `VMAF_HDR_MODELS` first. Each tool path must name a binary,
since a wrapper script won't do.

| the script keeps | keyed on |
|---|---|
| a rung's stream | the encoder binary, its libraries, the command line, the `Y264_*` and `YAH264_*` environment and the source file |
| a rung's score | the scorer chain and the script. A scorer change rescores the kept stream and prints "rescored". A reused rung prints "cached" |

A run started while another holds the same work directory stops at once.

| option | effect |
|---|---|
| `--max-crf` | drops the top rungs |
| `--fresh` | ignores the cache |
| `--vbr` | a VBR ladder at the listed targets. Both encoders get maxrate 1.5x the target, a buffer of 2x the target and an initial fill of 0.9. Each rung prints its rate error and the VBV checker's verdict. An error outside the parity spec's +-10% bar is flagged |
| `--encoders x264` | runs one side alone, such as the reference's CRF ladder that sets the VBR targets |
| `--yah-lookahead`, `--x264-lookahead` | each side's depth: at most 64 for yah264, 0 to 250 for x264. Every reading prints the depth each side ran with |

### Recon-match

`make conformance` runs recon-match for every format. By hand, decode the
`.264` to a Y4M (`ffmpeg -i x.264 dec.y4m`) and compare `framemd5` hash
columns, Y4M against Y4M. Don't compare a Y4M's framemd5 with one taken
straight off the `.264`, because ffmpeg treats them differently. Only the hash
column matters, because `--dump-recon` keeps the source frame rate where the
decode defaults to 25.

Check output size first. A crashed encode gives `decode_errors=0` on a
26-byte file. ffmpeg conceals desync, so grepping for "error while decoding"
undercounts. Look for "concealing N DC/AC/MV errors", or use `-v trace`.
