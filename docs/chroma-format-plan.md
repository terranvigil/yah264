# 4:2:2 / 4:4:4 chroma-format support, phased plan

10-bit lands first, then 4:2:2/4:4:4. Unlike 10-bit, chroma formats do **not**
touch the luma pixel type, so they do not block the luma SIMD/optimization pass.

## Non-negotiable invariant

**4:2:0 output stays byte-identical at every step.** New format code is gated
behind `cf_idc`/`sub_w`/`sub_h`; the 4:2:0 path produces identical `.264` and
passes conformance throughout. Each format is only *accepted at open* once its
whole chain is wired and recon-matches ffmpeg.

## Central representation (source of truth)

- `next264_csp_t`: `I420=0` (idc 1), `I422=1` (idc 2), `I444=2` (idc 3).
- Encoder context `cf_idc`, `sub_w` (SubWidthC), `sub_h` (SubHeightC): 4:2:0
  (2,2), 4:2:2 (2,1), 4:4:4 (1,1). Chroma plane dims = `padded_w/sub_w` x
  `padded_h/sub_h`; chroma MB is `16/sub_w` wide x `16/sub_h` tall.
- `n264_sps_t.chroma_format_idc` drives SPS signaling (+
  `separate_colour_plane_flag` = 0 for 4:4:4). profile_idc: 122 (High 4:2:2),
  244 (High 4:4:4).

## The three genuinely hard areas (rest is mechanical geometry)

1. **Chroma-DC transform.** 4:2:0 = 2x2 Hadamard (4 DC). 4:2:2 = 2x4 DC (8 DC,
   spec 8.5.11, a 2-point + 4-point transform with a `sqrt(2)`-scaled quant).
   4:4:4 has **no** chroma-DC: chroma AC/DC are coded like luma (Intra16 chroma
   uses a luma-style 4x4 DC only inside I_16x16; otherwise plain 4x4 residual).
2. **Entropy coding of chroma residual.** CABAC ctxBlockCat: 4:2:0/4:2:2 use
   cat 3 (ChromaDC) + cat 4 (ChromaAC), but ChromaDC coeff count differs (4 vs 8)
   so `CNT_M1`/scan differ. 4:4:4 uses the luma cats per component (cat 5/6-ish
   model: Cb/Cr get their own 4x4/8x8 residual, no ChromaDC). CAVLC: 4:2:2
   chroma-DC uses `nC == -2` (Table 9-9(b), 8 coeffs); 4:4:4 chroma uses the luma
   `nC` neighbour derivation. cbp semantics: 4:2:0/4:2:2 keep the 0/1/2
   chroma-cbp; 4:4:4 folds chroma into the luma cbp (no separate chroma cbp).
3. **Deblocking edge geometry.** 4:2:2 chroma MB is 8x16, so horizontal edges sit
   at chroma rows 0,4,8,12 (vs 0,4). 4:4:4 chroma deblocks exactly like luma
   (16x16, luma bS mapping). Vertical/horizontal edge counts become
   format-driven.

## Phase plan (each gates: 4:2:0 byte-identical + conformance green)

- **0. Foundation** (done): enum values, `n264_sps_t.chroma_format_idc`, encoder
  `cf_idc`/`sub_w`/`sub_h`, SPS write driven by the field (+4:4:4 extra bit),
  profile_idc 122/244. 4:2:2/4:4:4 rejected at open. 4:2:0 byte-identical.
- **1. Geometry backbone** (done): replace chroma `/2`/`>>1` with
  `/sub_w`,`/sub_h` (plane alloc/stride/pad/extend in encoder.c; nnz grid dims;
  CLI plane sizes + Y4M C422/C444 parse). Still 4:2:0-only accepted, so
  byte-identical. This is the mechanical bulk both new formats share.
- **2. 4:2:2** (done): 2x4 chroma-DC transform + quant; chroma block count 8 (2x4
  grid); intra chroma pred 8x16; CAVLC `nC==-2`; CABAC ChromaDC 8-coeff; deblock
  4 h-edges; chroma MV scaling (half-width eighth-pel x, full-height quarter-pel
  y). Accept I422 at open. Gate: recon-match vs ffmpeg High 4:2:2 (profile 122),
  CAVLC+CABAC. Covers intra + inter + B + multiref + 8x8.
- **3. 4:4:4** (done): chroma-as-luma coding model (no chroma-DC except I_16x16
  luma-style DC per component; luma cbp; luma ctxBlockCat per plane; 12 scaling
  lists when cqm); intra chroma pred 16x16 (reuse luma intra); deblock luma-like;
  chroma MV = luma MV (full-res both axes). Accept I444 at open. Gate:
  recon-match vs ffmpeg High 4:4:4 Predictive (profile 244), CAVLC + CABAC, I +
  P + B, multiref, spatial/temporal direct.
- **4. Corpus + BD**: add 4:2:2/4:4:4 test clips; direct BD vs x264 where useful.

**Deferred**: the 4:4:4 8x8 transform (I_8x8 chroma; needs c444 8x8 coders plus
CABAC cats 5/9/13 init values). `--transform-8x8` degrades to 4x4 for 4:4:4.
The GOP-parallel CLI reader is 4:2:0-only, so 4:2:2 uses the serial path.

## 4:4:4 execution reference (spec-confirmed, ITU-T H.264 04/2017)

Cb/Cr are coded exactly like luma (three `residual_luma` passes: Y, Cb, Cr). No
chroma-DC/AC branch, no `intra_chroma_pred_mode`, no separate chroma predictor.
Reuse the luma coding path on the chroma planes.

**Coding model.** The 4 `CodedBlockPatternLuma` bits gate the 8x8 groups of ALL
three planes; `CodedBlockPatternChroma` is 0. Intra: each Cb/Cr block reuses the
co-located luma block's Intra4x4/8x8/16x16 mode (inherited, not signalled). I16
chroma runs the luma-style 4x4 DC Hadamard per component. MC: chroma uses the
**luma 6-tap** interpolation, quarter-pel (`xIntC=(mvx>>2)`, `xFracC=mvx&3`; same
for y), because chroma MV == luma MV. Deblock: `chromaStyleFilteringFlag=0`, so
chroma uses the **luma-strength** filter; edges at 0,4,8,12 both axes (or 0,8
with 8x8), 16 samples, luma bS, chroma-QP thresholds.

**CABAC, total 1024 contexts** (the 460-entry state array extends to 1024). Base
ctxIdx (frame-coded) per ctxBlockCat:

| cat | block      | cbf  | sig | last | abs |
|-----|------------|------|-----|------|-----|
| 6   | Cb I16 DC  | 460  | 484 | 572  | 952 |
| 7   | Cb I16 AC  | 464  | 499 | 587  | 962 |
| 8   | Cb 4x4     | 468  | 513 | 601  | 972 |
| 9   | Cb 8x8     | 1016 | 660 | 690  | 708 |
| 10  | Cr I16 DC  | 472  | 528 | 616  | 982 |
| 11  | Cr I16 AC  | 476  | 543 | 631  | 992 |
| 12  | Cr 4x4     | 480  | 557 | 645  | 1002|
| 13  | Cr 8x8     | 1020 | 748 | 748  | 766 |

cabac_init (m,n) for ctxIdx 460..1023: spec Tables 9-25..9-33 (spec.txt lines
14514..15254); values identical across I and P/B slice columns except the idc
sets. `cabac_context_init_I[1024][2]` and `_PB[3][1024][2]` carry them. The Cb/Cr
ranges initialize by copying the luma contexts, which are identical and are the
spec-correct init for 4:4:4 chroma-as-luma.

**CAVLC.** Cb/Cr use luma-style nC (per-component neighbour, `(nA+nB+1)>>1`),
normal 0<=nC coeff_token columns, and the 4x4 total_zeros tables (NOT nC==-1/-2,
NOT chroma-DC tables).

**SPS/PPS, 12 scaling lists** (chroma_format_idc==3): 4x4 order i0..5 =
{IntraY,IntraCb,IntraCr,InterY,InterCb,InterCr}; 8x8 order i6..11 =
{IntraY,InterY,IntraCb,InterCb,IntraCr,InterCr} (note the 8x8 interleave differs
from 4x4). Fallback rule set A.

## Verification

Recon-match uses the same harness as 10-bit: `--dump-recon` writes Y4M
(C422p/C444p), decode the `.264` with ffmpeg, compare via `framemd5` (not raw
`cmp`, the Y4M header/FRAME markers differ). Thread determinism (1 vs 4) and a
QP sweep per format, as with High 10.

**Reliable recon-match test:** decode the .264 to a Y4M (`ffmpeg -i x.264
dec.y4m`) and compare `framemd5` hash columns Y4M-to-Y4M. Do NOT compare
framemd5 of a Y4M against framemd5 taken directly off the .264 (ffmpeg treats
those asymmetrically), and beware that `--dump-recon` carries the source
framerate while the decode defaults to 25fps (only the hash column matters).

## Diagnostic traps

These each cost real time while bringing up 4:4:4.

- **Check OUTPUT SIZE first.** An empty or crashed encode gives a meaningless
  `decode_errors=0`. A 26-byte `.264` reads as a clean pass.
- **ffmpeg CONCEALS desync.** `-v error | grep 'error while decoding'`
  UNDERCOUNTS; the frame reports "concealing NNN DC/AC/MV errors" instead. Use
  the concealment message (or `-v trace`) as the real desync signal.
- **Use explicit CLI args, not shell vars.** Expansion silently broke args into
  a usage error and an empty output file.
- **Rebuild cleanly** (touch the .c). Checkout dances for comparison binaries
  leave stale object files.
- **`decode_errors` on a FULL-SIZE stream is the trustworthy signal**, not
  framemd5 mismatch, for 4:4:4.

Two classes of bug turned up during bring-up and both are worth watching for in
any new format:

- **Stack buffer overflows** from 4:2:0-sized scratch: `nzbuf[16+8]` was too
  small for the 4:2:2 32-entry nnz save and corrupted the luma recon in RD
  trials; the 4:4:4 nz/nzbuf and `snap_*` recon buffers needed 16+32 and
  `16*16+2*16*16`. AddressSanitizer catches these; nothing else did.
- **Context-table edge cases**: the CABAC chroma-DC `coeff_abs_level` Gt1
  context. The 8-coefficient 4:2:2 chroma DC block reaches node 7, where the
  general table gives ctxIdxInc 9, which spills into the chroma-AC context. The
  4:2:2-DC variant caps it at 8 (the `<reference-internal>` 422-DC row).
