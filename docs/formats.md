# Bit depth and chroma formats

yah264 encodes 8-bit and 10-bit video at 4:2:0, 4:2:2 or 4:4:4. Each depth
and each chroma format recon-matches ffmpeg's decoder across I, P and B frames
on both entropy coders. One `yah264` binary handles both depths and picks the
encoder from the input.

The tuned path is 8-bit 4:2:0. The 10-bit path keeps the 8-bit
rate-distortion trades, and when 8-bit content is coded at 10 bits it gets
the frame types of its 8-bit encode. On the HDR10 test clips at 1080p the
10-bit path is ahead of x264 High 10 on coding efficiency, by the HDR ladder
in "Measuring it".

## How it works

### Both depths in one binary

Because the sample width is a compile-time type, there are two encoder
libraries. `src/common/bitdepth.h` sets the width from `Y264_BIT_DEPTH`:

| | 8-bit | 10-bit |
|---|---|---|
| `pixel` | `uint8_t` | `uint16_t` |
| `dctcoef` (coefficients, transform intermediates) | `int16_t` | `int32_t` |
| `PIXEL_MAX` | 255 | 1023 |
| `Y264_QP_BD_OFFSET` | 0 | 12 (QP range -12 to 51) |
| profile at 4:2:0 | per options (Baseline/Main/High) | 110, High 10 |
| SIMD | every tier ([simd.md](simd.md)) | NEON on aarch64; SSE4.2 and AVX2 on x86-64 ([simd.md](simd.md)) |
| GPU offload | available, off ([gpu.md](gpu.md)) | stubbed out |
| library | `libyah264`, `yah264.pc` | `libyah264_10`, `yah264_10.pc` |

The coefficients widen at 10 bits because the residual reaches ±1023 and the
transform butterflies overflow 16 bits. Only true image samples use `pixel`.
Bitstream bytes, nonzero counts, mode ids and flags stay `uint8_t`.

Every build directory compiles both libraries. The 10-bit one force-includes
`src/common/symbols10.h`, which gives every symbol a `_10` suffix. Both
archives then link into one program without a name resolving into the wrong
one. `scripts/symbol_check.sh` checks this in `make test`, because a name
defined in both would resolve by link order and the only symptom would be
wrong pixels.

The public header has no `pixel` type. Picture planes are `void*` with a
stride in samples. Each `.pc` file sets `Y264_BIT_DEPTH` in its `Cflags`, so a
caller's `yah264_encoder_open()` resolves to the library it configured
against.

The meson `bit_depth` option doesn't decide what the CLI can encode. It
defaults to 8 and picks the depth that checkasm, the unit tests and the
in-tree tools are built at. CI also builds `-Dbit_depth=10` and runs the unit
tests there.

### Choosing the depth

- A Y4M input's `C` tag picks the library: `C420` goes to 8-bit, `C420p10` to
  High 10. The same holds for 4:2:2 and 4:4:4.
- Raw input has no tag. Its `--input-csp` names the depth along with the
  format: `i420`, `i422` or `i444`, optionally with `p10`.
- `--output-depth 10` on 8-bit input shifts each sample left by 2 and codes
  High 10.
- `--output-depth 8` on 10-bit input is refused. Dropping bits is a
  conversion, and that's ffmpeg's job.
- The encode is refused with exit status 1 when a 16-bit input sample is
  above the depth's maximum. Such a file isn't 10-bit video.

The [ffmpeg wrapper](ffmpeg.md) makes the same choice from the input
`pix_fmt`.

### Chroma formats

The encoder context holds `cf_idc` along with `sub_w` and `sub_h`, the
spec's SubWidthC and SubHeightC. Plane sizes, strides, padding, the nonzero
grid and the chroma macroblock size all derive from those two. That leaves no
4:2:0 constants in the shared geometry.

| | 4:2:0 | 4:2:2 | 4:4:4 |
|---|---|---|---|
| `chroma_format_idc` | 1 | 2 | 3 |
| profile_idc | per options | 122 (High 4:2:2) | 244 (High 4:4:4 Predictive) |
| chroma macroblock | 8x8 | 8x16 | 16x16 |
| chroma DC | 2x2 Hadamard, 4 coefficients | 2x4 transform, 8 coefficients | none, except a luma-style 4x4 DC per plane inside I_16x16 |
| intra chroma prediction | chroma modes, 8x8 | chroma modes, 8x16 | inherited from the co-located luma block, not signaled |
| chroma MC | eighth-pel bilinear | eighth-pel x, quarter-pel y | luma 6-tap, quarter-pel (chroma MV equals luma MV) |
| CAVLC chroma DC | `nC = -1` | `nC = -2` | luma-style `nC` per plane |
| coded block pattern | separate chroma cbp | separate chroma cbp | chroma folded into the luma cbp |
| deblock | chroma filter, edges 0 and 4 | chroma filter, horizontal edges 0, 4, 8, 12 | luma-strength filter, edges like luma |
| scaling lists in the SPS | 8 | 8 | 12 |

At 4:4:4 the Cb and Cr planes are coded exactly like luma: three residual
passes, the luma context categories per plane, and CABAC extended from 460 to
1024 contexts. The Cb and Cr context ranges initialize by copying luma's.
The spec's init tables give the same values.

A few things stay 4:2:0 only or are limited at the other formats:

- 4:4:4 never uses the 8x8 transform. `--transform-8x8` drops to 4x4 at
  open. The 4:4:4 chroma 8x8 quant-matrix keys are accepted and unused.
- Field coding (`--tff`/`--bff`) is 4:2:0 only. An explicit request on
  4:2:2 or 4:4:4 is refused. An interlaced Y4M tag is ignored with a warning.
- Some chroma work has no SIMD: the vertical chroma deblock edge, all 4:4:4
  chroma deblocking and intra chroma at 4:4:4 run in C.
- There's no I400, NV12, YV12 or packed input. Convert with ffmpeg first.

Threading is format-agnostic. The GOP-parallel reader reads the subsampling
from the Y4M `C` tag the same way the serial path does.

### Keeping 10-bit on the 8-bit trades

We do all the tuning at 8 bits. To keep the 8-bit trades, the 10-bit path
puts its costs on the 8-bit scale in five places:

- **Lambdas.** The mode, trellis and motion lambdas are read at the QP plus
  the depth's QP offset. Because a 10-bit sample's error is four times the
  8-bit one, SATD grows 4x and SSD 16x. An offset of six QP per extra bit
  scales the lambdas by the same factors. The psy terms scale with the
  sample scale for the same reason.
- **The lookahead.** Its lowres planes hold 8-bit samples at every depth.
  B placement, scene cuts, mb-tree and CRF's complexity therefore read the
  costs their constants were fitted on, and 8-bit content coded at 10 bits
  gets the frame types of its 8-bit encode.
- **Rate-control complexity.** The full-resolution intra SATD that opens a
  bitrate-target encode and prices its I frames is divided by 4 at 10
  bits, so the opening fits read it on the scale they were fitted on.
- **Adaptive quantization.** Every AQ path reads a macroblock's energy as
  `log2(variance + floor)` after dividing the variance by 4 per extra bit.
  The floors, the anchor and B-frame levels and the frame DC then see the
  values they were fitted on. The flat psy-trellis check's variance limit is
  raised by the same factor.
- **Fixed thresholds.** The I_16x16 screen counts a P or B macroblock as flat
  under a variance limit that is raised by the same factor, so 8-bit content
  coded at 10 bits gets the verdict of its 8-bit encode. The motion search's
  per-pixel early-out bound doubles per extra bit, like the SAD it's
  compared with.

## What we chose and why

| choice | why |
|---|---|
| Bit depth as a compile-time type | Keeps the hot path branch-free and lets each depth get its own SIMD. A runtime-depth `pixel` would put a branch or a wider type in every 8-bit kernel. |
| Both libraries in every build, one binary | The input picks 10-bit, with no rebuild. 8-bit output is byte-identical to the single-library build on the ten board clips at CRF, CQP and ABR, at one and eight threads. |
| `--output-depth 8` on 10-bit input refused | Rounding samples away is a conversion. Silent truncation would hide it. |
| The pixel-type change before any SIMD work | Widening `pixel` and `dctcoef` is the one change that would invalidate 8-bit kernels. Doing it first meant each kernel is written once per depth. |
| 10-bit SIMD as its own tiers | Each kernel is written for the depth's lanes and checked against the 10-bit C reference. The 8-bit kernels are untouched. |
| Recon-match against ffmpeg as the gate for each format | A format is accepted at open only once its whole chain decodes bit-exact. 4:2:0 output stayed byte-identical while each format went in. |

The two-library dispatch costs the 8-bit path +0.3% to 0.6% wall time at one
thread on foreman CIF and park_joy 720p. That's inside same-binary run-to-run
spread. Because the cost doesn't scale with the CLI's share of the work,
binary layout is probably the cause more than the dispatch itself.

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

The 10-bit board is `scripts/board10.sh`. Its clip list and rate targets are
`CLIPS_P10` in `scripts/parity-clips.sh`. Both encoders run at their default
preset and `--threads 4`. `scripts/bd_at_rate.py` solves each clip onto five
shared byte targets. x264 needs both `--profile high10` and
`--output-depth 10`, because the profile alone still writes 8-bit samples.

The board's clips are short. 64 frames at 480x272 is below the project's
120-frame rule for a BD sweep, so read the board as a smoke check.

Decode with `-fps_mode passthrough`. An Annex-B stream records its rate in the
VUI. When the Y4M muxer picks a different one, ffmpeg drops frames to fit.
The 120 fps clips once decoded 15 of 64 frames. VMAF scored those against
the first 15 of the reference.

HDR clips score on VMAF v1's HDR models, `hdr_vmaf_v1.1.8`, and never on its
SDR models. The SDR models score PQ pictures as if they were SDR without a
warning. The HDR model follows the picture:

- 3d0h up to a height of 1080
- 1d5h_2160 above that, so 3840x1600 and 4096x2160 score on it
- the hfr variant at 50 fps and up

Tag both sides limited range, BT.2020 primaries and matrix, and PQ transfer.
Under the project's rule, a picture smaller than 1920x1080 in both dimensions
is scaled up on both sides to fit 1920x1080 with its aspect kept, and anything
else scores at its own size, 1080p and 2160p included. Read PSNR-Y on the
10-bit code values too. Before reading a BD figure, check that the two ladders
overlap in quality.

The HDR ladder is `scripts/hdr_ladder.py SRC.y4m`. It runs CRF 18-34 on
yah264 and on x264 High 10 at medium. Both streams are tagged HDR10: BT.2020,
PQ and limited range. The source is a progressive, limited range C420p10
HDR10 Y4M of any even size, and the 1080p test spans are 120 frames. Each
rung scores as above. When a source is scaled up, CAMBI is told the coded
size. The script prints BD-rate by HDR VMAF, PSNR-Y and PSNR-YUV over the
quality range both ladders cover. Each rung's CAMBI prints beside it.

Before running it, set `VMAF_HDR` and `VMAF_HDR_MODELS` and make sure each
tool path names a binary, since a wrapper script won't do. The script keeps
each rung's stream and score. It keys the stream on the encoder binary, its
libraries, the command line, the `Y264_*` and `YAH264_*` environment and the
source file. It keys the score on the scorer chain and the script. When the
scorer changes, the kept stream is rescored and the rung prints "rescored".
A reused rung prints "cached". A run started while another holds the same
work directory stops at once. The options:

- `--max-crf` drops the top rungs.
- `--fresh` ignores the cache.
- `--vbr` runs a VBR ladder at the listed targets. Both encoders get a
  maxrate of 1.5x the target, a buffer of 2x the target and an initial fill
  of 0.9. Each rung prints its rate error and the VBV checker's verdict. A
  rate error outside the parity spec's +-10% bar is flagged.
- `--encoders x264` runs one side alone, e.g. the reference's CRF ladder that
  sets the VBR targets.
- `--yah-lookahead` (at most 64) and `--x264-lookahead` (0 to 250) set each
  side's depth. Every reading prints the depth each side ran with.

Recon-match runs for every format under `make conformance`. To check by
hand, decode the `.264` to a Y4M (`ffmpeg -i x.264 dec.y4m`) and compare the
`framemd5` hash columns, Y4M against Y4M. Don't compare a Y4M's framemd5 with
one taken straight off the `.264`, because ffmpeg treats them differently.
Because `--dump-recon` keeps the source frame rate where the decode defaults
to 25, only the hash column matters.

Check output size first. A crashed encode gives `decode_errors=0` on a
26-byte file.

ffmpeg conceals desync, so grepping for "error while decoding" undercounts.
Look for "concealing N DC/AC/MV errors", or use `-v trace`.
