# Bit depth and chroma formats

yah264 encodes 8-bit and 10-bit video at 4:2:0, 4:2:2 or 4:4:4. Each depth
and each chroma format recon-matches ffmpeg's decoder across I, P and B frames
on both entropy coders. One `yah264` binary handles both depths and picks the encoder
from the input. The 8-bit 4:2:0 path is the tuned one. The 10-bit path codes
correctly but is well behind x264 High 10 on coding efficiency, and that gap
is the main open item on this page.

## How it works

### Two libraries, one binary

The sample width is a compile-time type, so there are two encoder libraries.
`src/common/bitdepth.h` sets it from `Y264_BIT_DEPTH`:

| | 8-bit | 10-bit |
|---|---|---|
| `pixel` | `uint8_t` | `uint16_t` |
| `dctcoef` (coefficients, transform intermediates) | `int16_t` | `int32_t` |
| `PIXEL_MAX` | 255 | 1023 |
| `Y264_QP_BD_OFFSET` | 0 | 12 (QP range -12 to 51) |
| profile at 4:2:0 | per options (Baseline/Main/High) | 110, High 10 |
| SIMD | every tier ([simd.md](simd.md)) | none, C reference only |
| GPU offload | available, off ([gpu.md](gpu.md)) | stubbed out |
| library | `libyah264`, `yah264.pc` | `libyah264_10`, `yah264_10.pc` |

The coefficients widen at 10 bits because the residual reaches ±1023 and the
transform butterflies overflow 16 bits. Only true image samples use `pixel`.
Bitstream bytes, nonzero counts, mode ids and flags stay `uint8_t`.

Every build directory compiles both libraries. The 10-bit one force-includes
`src/common/symbols10.h`, which gives every symbol a `_10` suffix, so both
archives link into one program without a name resolving into the wrong one.
`scripts/symbol_check.sh` gates that in `make test`: a name defined in both
would resolve by link order, and wrong pixels would be the only symptom. The
public header has no `pixel` type. Picture planes are `void*` with a stride in
samples, and each `.pc` file sets `Y264_BIT_DEPTH` in its `Cflags`, so a
caller's `yah264_encoder_open()` resolves to the library it configured
against.

The meson `bit_depth` option (default 8) no longer decides what the CLI can
encode. It picks the depth that checkasm, the unit tests and the in-tree tools
are built at. CI builds `-Dbit_depth=10` and runs the unit tests there.

### Choosing the depth

- A Y4M input's `C` tag picks the library: `C420` goes to 8-bit, `C420p10` to
  High 10. The same holds for 4:2:2 and 4:4:4.
- Raw input has no tag, so `--input-csp` carries the depth with the format:
  `i420`, `i422`, `i444`, each optionally with `p10`.
- `--output-depth 10` on 8-bit input shifts each sample left by 2 and codes
  High 10.
- `--output-depth 8` on 10-bit input is refused. Dropping bits is a
  conversion, and that's ffmpeg's job.

The ffmpeg wrapper makes the same choice from the input `pix_fmt` (see
[ffmpeg.md](ffmpeg.md)).

### Chroma formats

The encoder context carries `cf_idc` with `sub_w` and `sub_h` (SubWidthC and
SubHeightC). Plane sizes, strides, padding, the nonzero grid and the chroma
macroblock size all derive from those two, so the shared geometry has no
4:2:0 constants left in it.

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
1024 contexts. The Cb and Cr context ranges initialize by copying luma's,
which the spec's init tables also give.

Things that stay 4:2:0 only, or are limited elsewhere:

- **4:4:4 never uses the 8x8 transform.** `--transform-8x8` drops to 4x4 at
  open, and the 4:4:4 chroma 8x8 quant-matrix keys are accepted and unused.
- **Field coding (`--tff`/`--bff`) is 4:2:0 only.** An explicit request on
  4:2:2 or 4:4:4 is refused. An interlaced Y4M tag is ignored with a warning.
- **Chroma SIMD.** The vertical chroma deblock edge, all 4:4:4 chroma
  deblocking and intra chroma at 4:4:4 run in C.
- **No I400, NV12, YV12 or packed formats.** Convert with ffmpeg first.

Threading is format-agnostic. The GOP-parallel reader reads the subsampling
from the Y4M `C` tag the same way the serial path does.

## What we chose and why

| choice | why |
|---|---|
| Bit depth as a compile-time type | Keeps the hot path branch-free and lets each depth get its own SIMD. A runtime-depth `pixel` would put a branch or a wider type in every 8-bit kernel. |
| Both libraries in every build, one binary | 10-bit became a property of the input instead of a rebuild. Over the ten board clips at CRF, CQP and ABR, at one and eight threads, 8-bit output is byte-identical to the single-library build. |
| `--output-depth 8` on 10-bit input refused | Rounding samples away is a conversion. Silent truncation would hide it. |
| The pixel-type change before any SIMD work | Widening `pixel` and `dctcoef` is the one change that would invalidate 8-bit kernels. Doing it first meant each kernel is written once per depth. |
| 10-bit runs C only | The 10-bit path's efficiency gap comes first. 16-bit SIMD would only make a weaker encoder faster. |
| Recon-match against ffmpeg as the gate for each format | A format is accepted at open only once its whole chain decodes bit-exact. 4:2:0 output stayed byte-identical while each format went in. |

The two-library dispatch costs the 8-bit path +0.3% to 0.6% wall time at one
thread (foreman CIF and park_joy 720p). That's inside same-binary run-to-run
spread, and it doesn't scale with the CLI's share of the work, so binary
layout is the likelier cause than the dispatch itself.

## What didn't work

| idea | why not |
|---|---|
| A blind `uint8_t` to `pixel` replace | Most `uint8_t` uses aren't pixels. Every conversion was a judgment call across about 440 uses in 27 files. |
| 4:2:0-sized scratch buffers at 4:2:2 and 4:4:4 | Stack overflows: a 24-entry nonzero buffer took the 32-entry 4:2:2 save and corrupted luma recon in RD trials. Only AddressSanitizer caught it. |
| The general CABAC Gt1 context table for the 4:2:2 chroma DC | The 8-coefficient block reaches node 7, where the general table's ctxIdxInc of 9 spills into the chroma AC contexts. The 4:2:2 DC row caps it at 8. |

## Open questions

### The 10-bit efficiency gap

We scored eight BVI-AOM clips at 480x272, 64 frames each, by VMAF-NEG BD-rate
at matched bytes against x264 High 10. The median is +21% and one clip is
ahead. An 8-bit control shows where the gap comes from: the two worst clips,
downconverted to 8 bits, read +3.5% and +0.3% against 8-bit x264 on the same
scripts and byte targets. PSNR agrees. At the middle rung on
BusyHongKongStreetVidevo we're about 1.9 dB behind after correcting for
bytes.

| clip | fps | BD-rate (VMAF-NEG) vs x264 High 10 |
|---|--:|--:|
| VeniceS1Harmonics | 60 | -6.66% |
| AmericanFootballS2Harmonics | 60 | +0.35% |
| SmokeSauna | 50 | +14.63% |
| ChristmasRoomDareful | 29 | +17.11% |
| CalmingWaterBVITexture | 120 | +25.53% |
| BasketballS1YonseiUniversity | 30 | +29.73% |
| BusyHongKongStreetVidevo | 25 | +45.61% |
| JockeyHarmonics | 120 | +59.60% |

Nothing in the coding tools is depth-specific. What is: anything that indexes
a table with a QP that can now go to -12, the lambda tables and RD scale, the
rate-control constants fitted on 8-bit, and the quant deadzone and rounding. A
fixed-QP cell is the cheap first cut. If bytes against quality look sane at a
pinned QP, the defect is in rate control. If not, it's in lambda or quant.

### Other open items

- **4:4:4 with the 8x8 transform.** Needs the 4:4:4 8x8 residual coders and
  the CABAC 8x8 chroma context init values.
- **4:2:2 and 4:4:4 against x264.** Both recon-match, but no BD board exists
  for them yet.
- **16-bit SIMD** for the 10-bit library, after the efficiency gap closes.

## Measuring it

- **The 10-bit board:** `scripts/board10.sh`. The clip list and rate targets
  are `CLIPS_P10` in `scripts/parity-clips.sh`. Both encoders run at their
  default preset and `--threads 4`, solved onto five shared byte targets per
  clip by `scripts/bd_at_rate.py`. x264 needs both `--profile high10` and
  `--output-depth 10`. The profile alone still writes 8-bit samples.
- **Decode with `-fps_mode passthrough`.** An Annex-B stream carries its rate
  in the VUI, and when the Y4M muxer picks a different one ffmpeg drops frames
  to fit. The 120 fps clips once decoded 15 of 64 frames, and VMAF scored
  those against the first 15 of the reference.
- **The clips are short.** 64 frames at 480x272 is below the project's
  120-frame rule for a BD sweep. The 8-bit control shares that shape, which is
  why the comparison still holds.
- **Recon-match:** `make conformance` covers every format. By hand, decode the
  `.264` to a Y4M (`ffmpeg -i x.264 dec.y4m`) and compare `framemd5` hash
  columns Y4M against Y4M. Don't compare a Y4M's framemd5 with one taken
  straight off the `.264`, since ffmpeg treats them differently.
  `--dump-recon` keeps the source frame rate where the decode defaults to 25,
  so only the hash column matters.
- **Check output size first.** A crashed encode gives `decode_errors=0` on a
  26-byte file.
- **ffmpeg conceals desync.** Grepping for "error while decoding" undercounts.
  Look for "concealing N DC/AC/MV errors", or use `-v trace`.
