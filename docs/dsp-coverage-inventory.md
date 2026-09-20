# yah264 pixel-work inventory: NEON vs scalar C

Read-only survey of this tree at commit 7cd92bd (main, 2026-09-02).
Every line number below was read from the tree, not inferred.

## 0. Summary

- The dsp layer has **67 public C entry points** across pixel/mc/transform/predict/deblock. **53 have a NEON twin** (8 of those only on a sub-range of shapes or inputs), **14 do not**. `src/dsp/*_neon.c` export **61 NEON functions** in total.
- Dispatch is two mechanisms: a **function-pointer table** for the pixel metrics (`y264_dsp`, filled once in `y264_dsp_init`), and a **per-call flag check** (`y264_asm_on(class)`, an acquire-load plus a mask test) for everything else (mc, transform, predict, deblock, SSD). There is no x86 SIMD; the only SIMD path is `__aarch64__ && Y264_BIT_DEPTH == 8`.
- `tools/checkasm/checkasm.c` covered 44 kernel groups and did **not** test `y264_dequant_4x4_neon`, `y264_dequant_8x8_neon`, `y264_ssd_16xh_neon` or `y264_ssd_8xh_neon`. **Fixed on 2026-09-17** (item `x86-checkasm`, x86 plan W0a): the harness is now `tools/checkasm/{main.c,checkasm.h,pixel.c,transform.c,mc.c,predict.c,deblock.c}` with a `{name, family, cpu_mask, fn}` registration table, 47 groups, and all four of those kernels have one. Two more that were dark are covered too: `y264_pixel_init` overwrites `sad_16x16` and `var16x16` with their FEAT_DotProd twins on this box, so the plain-NEON forms -- which still ship, and still run wherever dotprod is absent -- had never been exercised by a run on this machine; the plain and dotprod forms are separate rows now. Every group compares the C reference against the kernel SYMBOL rather than through the dispatcher, so `Y264_ASM_OFF` can no longer flatten a group into a C-against-C tautology, and guard buffers, a row-padding check and an mmap/mprotect page guard at both tails ride on every comparison. The rest of this section's "checkasm" columns are pre-split group names; the live list is `checkasm --list`.
- Outside dsp, the remaining scalar pixel/coefficient loops are concentrated in `src/encoder/macroblock.c` (DC-only reconstruction, chroma DC transform + quant, weighted prediction, per-block 8x8 bipred average, scan-order level extraction, level nonzero counts, RD snapshot copies, greedy RDOQ residual rebuild), `src/encoder/encoder.c` (lowres downscale, 15 quarter-pel lowres phase planes, lowres intra cost, mb-tree propagate splat, plane pad/extend, WP estimate sums, chroma AC energy) and `src/encoder/deblock.c` (vertical chroma edges and all 4:4:4 chroma filtering).
- Data layout differs from the fixed-stride scratch convention other encoders use in three ways that matter for kernels: **no alignment anywhere** (planes are plain `malloc`, stride = width + 2*border + optional pad, interior pointer offset by border; MB scratch is stack `pixel[256]` with stride 16), coefficient blocks are **raster `int16_t[16]`/`[64]`** with scan-order copies made separately per consumer, and prediction blocks use **three different strides** (16 for luma, `cw` = 8 for 4:2:0 chroma, 4 or 8 for intra4x4/8x8 candidates).
- The latest profile that names uncovered symbols is `yah264old/docs/archive/goal3-coverage-ranking.md` (2026-08-17, committed 08-25 in the old tree; not present in this tree). It is reproduced in section 4.

---

## 1. `src/dsp`: C kernels, NEON twins, dispatch, checkasm

### 1a. Dispatch mechanisms

| class | mechanism | where | ablation bit |
|---|---|---|---|
| pixel metrics (sad, sad_x4, satd, sa8d, hadamard_ac, texture, var, intra4x4_x9, intra_satd_x3) | function-pointer table `y264_pixel_fn_t y264_dsp`, filled once by `y264_dsp_init` | `src/dsp/pixel.h:81-133`, `src/dsp/pixel.c:465-518` | `Y264_ASM_PIXEL` zeroes `cpu` before init (pixel.c:513) |
| mc (luma, chroma, avg_wt, pred_copy, pred_avg2) | per-call `y264_asm_on(Y264_ASM_MC)` inside the public wrapper, shape/bounds test, then NEON or C | `src/dsp/mc.c:249-299, 301-371, 551-590` | `Y264_ASM_MC` |
| hpel plane build | per-call `y264_asm_on(Y264_ASM_HPEL)` once per band, then per-row kernel calls | `src/dsp/mc.c:460-536` | `Y264_ASM_HPEL` |
| dct / sub-dct / add-idct | per-call `dct_have_neon()` = `y264_asm_on(Y264_ASM_DCT)` | `src/dsp/transform.c:43, 204-330, 415-430` | `Y264_ASM_DCT` |
| quant / dequant | per-call `qnt_have_neon()`, **flat CQM only** (`!w`) | `src/dsp/transform.c:45, 625-700, 892-960` | `Y264_ASM_QUANT` |
| zigzag / scan mask | per-call `scan_have_neon()` | `src/dsp/transform.c:44, 776-799` | `Y264_ASM_SCAN` |
| intra predict | per-call `pr_have_neon()`, per-mode switch for 8x8 | `src/dsp/predict.c:399-486` | `Y264_ASM_PRED` |
| deblock strength + filters | per-call `db_have_neon()` in the **encoder**, not dsp | `src/encoder/deblock.c:25, 189-192, 233-240, 258-265, 310-317` | `Y264_ASM_DEBLOCK` |
| SSD | per-call `y264_asm_on(Y264_ASM_SSD)` in the **encoder** | `src/encoder/macroblock.c:868-887` | `Y264_ASM_SSD` |

`y264_asm_on` (`src/common/cpu.h:79-83`) is `(y264_cpu_detect() & Y264_CPU_NEON) && !(y264_asm_off_ & cls)`; `y264_cpu_detect` is an inline acquire load of a cached word (cpu.h:38-49). The comment at cpu.h:34-37 records that the previous out-of-line `pthread_once` form measured ~1% of self-time. A caller-side consequence: every 4x4 transform/quant call pays a branch plus an atomic load, which is why `y264_sub_dct4_blocks` (transform.h:316-326) exists: it hoists the dispatch to once per 16-block grid.

The NEON kernels are declared in the C file that dispatches to them (e.g. pixel.c:430-463, mc.c:12-36, transform.c:13-46, predict.c:399-409, encoder/deblock.c:20-26 and :67-69), not in a shared header, except `y264_ssd_*_neon` (pixel.h:135-139) and `y264_deblock_strength_neon` (deblock.h:575-578).

### 1b. `pixel.c` / `pixel_neon.c` / `x86/pixel_{sse4,avx2}.c` (table-dispatched)

The SSE4.2 and AVX2 columns arrived with wave 1 of docs/x86-plan.md. They
carry the kernel and its checkasm row and NOT a multiple, and the reason is
recorded rather than left to be guessed: the only x86 this project can reach
before the rented box is an emulator, `x86-docker.sh` reads no clocks for that
reason, and a translated ratio for a 256-bit kernel measures the translator.
Run under Rosetta the AVX2 rows read below their own C reference while the
SSE4.2 rows read 1.2x to 4.9x; neither number is about silicon. The figures
land here from session A.

| table slot (pixel.h:81-122) | C ref (pixel.c) | NEON twin (pixel_neon.c) | SSE4.2 | AVX2 | checkasm group |
|---|---|---|---|---|---|
| `sad[16x16]` | `sad_c_16x16` | `y264_sad_16x16_neon` :62, `_neon_dotprod` (DOTPROD cpus) | `y264_sad_16x16_sse4` | `_avx2` | `sad_16x16` (:341-363) |
| `sad[16x8]` | `sad_c_16x8` | `y264_sad_16x8_neon` :67 | `y264_sad_16x8_sse4` | `_avx2` | `sad_16x8` |
| `sad[8x16]` | `sad_c_8x16` | `y264_sad_8x16_neon` :90, `_neon_dotprod` | `y264_sad_8x16_sse4` | `_avx2` | `sad_8x16` |
| `sad[8x8]` | `sad_c_8x8` | `y264_sad_8x8_neon` :95 | `y264_sad_8x8_sse4` | `_avx2` | `sad_8x8` |
| `sad[8x4]` | `sad_c_8x4` | **none** (C) | **none** | **none** | `sad_8x4` (C vs C) |
| `sad[4x8]` | `sad_c_4x8` | **none** | **none** | **none** | `sad_4x8` |
| `sad[4x4]` | `sad_c_4x4` | **none** | **none** | **none** | `sad_4x4` |
| `sad_x4[16x16..8x4]` (5) | `sad_x4_c_*` | `y264_sad_x4_{16x16,16x8,8x16,8x8,8x4}_neon` :148-168 | `y264_sad_x4_{16x16,16x8,8x16,8x8,8x4}_sse4` | `_avx2` | `sad_x4_*` (:365-390) |
| `sad_x4[4x8]`, `sad_x4[4x4]` | `sad_x4_c_*` | **none** | **none** | **none** | `sad_x4_*` (C vs C) |
| `satd4x4` | `satd_c_4x4` | `y264_satd_4x4_neon` :234 | `y264_satd_4x4_sse4` | `_avx2` (the shared 128-bit form, VEX) | `satd4x4` (:150-170) |
| `satd8x8` | `satd_c_8x8` | `y264_satd_8x8_neon` :308 | `y264_satd_8x8_sse4` | `_avx2` (rows 0-3 low lane, 4-7 high) | `satd8x8` (:177) |
| `satd_x4_8x8` | `satd_x4_c_8x8` | `y264_satd_x4_8x8_neon` :331 | `y264_satd_x4_8x8_sse4` | `_avx2` | `satd_x4_8x8` (:395-418) |
| `satd16x16` | `satd_c_16x16` | `y264_satd_16x16_neon_ded` :358 | `y264_satd_16x16_sse4` | `_avx2` (four tiles per pass) | `satd16x16` |
| `sa8d8x8` | `sa8d_c_8x8` | `y264_sa8d_8x8_neon` :436 | `y264_sa8d_8x8_sse4` | `_avx2` (the shared 128-bit form, VEX) | `sa8d8x8` |
| `sa8d16x16` | `sa8d_c_16x16` | `y264_sa8d_16x16_neon` :450 | `y264_sa8d_16x16_sse4` | `_avx2` (two 8x8 per pass) | `sa8d16x16` |
| `hadamard_ac8x8` | `hadamard_ac_c_8x8` | `y264_hadamard_ac_8x8_neon` :460 | `y264_hadamard_ac_8x8_sse4` | `_avx2` | `hadamard_ac8x8` (:198-211) |
| `texture_ac4_16x16` | `texture_ac4_c_16x16` | `y264_texture_ac4_16x16_neon` :487 | `y264_texture_ac4_16x16_sse4` | `_avx2` | `texture_ac4` (:215-228) |
| `texture_ac48_16x16` | `texture_ac48_c_16x16` | `y264_texture_ac48_16x16_neon` :593 | `y264_texture_ac48_16x16_sse4` | `_avx2` | `texture_ac48` (:232-258) |
| `var16x16` | `var_c_16x16` | `y264_var_16x16_neon` :646, `_neon_dotprod` :663 | `y264_var_16x16_sse4` | `_avx2` | `var16x16` (:261-277) |
| `intra4x4_x9` | `intra4x4_x9_c` | `y264_intra4x4_x9_neon` :747 | `y264_intra4x4_x9_sse4` | `_avx2` (modes in pairs) | `intra4x4_x9` (:280-310) |
| `intra_satd_x3_16` | `intra_satd_x3_16_c` | `y264_intra_satd_x3_16x16_neon` :815 | `y264_intra_satd_x3_16x16_sse4` | `_avx2` | `intra_satd_x3_16` (:313-339) |
| (not in table) | scalar SSD loop in `ssd_block`, macroblock.c:879-885 | `y264_ssd_16xh_neon` :18, `y264_ssd_8xh_neon` :30, and the FEAT_DotProd twins `y264_ssd_16xh_neon_dotprod` / `y264_ssd_8xh_neon_dotprod` (HD stage 4) | `y264_ssd_{16xh,8xh}_sse4` | `_avx2` | `ssd`, `ssd_dotprod` (added 2026-09-17, split 2026-09-19) |

Pixel table: 26 slots, 21 with a NEON twin, 5 without (the 4-wide and 8x4 SADs, `sad_x4` 4x8/4x4). Plus SSD, dispatched from the encoder with a NEON twin, which gained its checkasm group on 2026-09-17.

The x86 tiers cover the same 21 slots plus SSD, in both SSE4.2 and AVX2: 24 kernels each. The five slots NEON refused are refused here too, and for the same reason rather than by inheritance -- a 4-wide SAD is four bytes against four, which no vector instruction improves on, and `sad_x4` at those widths would batch four of them.

### 1c. `mc.c` / `mc_neon.c` / `x86/mc_{sse4,avx2}.c` (per-call dispatch)

The SSE4.2 and AVX2 columns arrived with wave 2 of docs/x86-plan.md. They
carry the kernel and its checkasm row and NOT a multiple, for the reason
recorded above the pixel table: the only x86 this project can reach before the
rented box is an emulator, and a translated ratio measures the translator.

The x86 column is named by WIDTH where the NEON one is named by block. There
is no x86 twin of `y264_mc_chroma_neon8`, the fixed 8x8 form, because the
8-wide kernel is straight-line at every even height and a second entry point
for one of them would be a second thing to keep bit-exact for no instruction
saved. Decided here rather than inherited: the shape NEON specialised is a
shape x86 does not have to.

| public entry (mc.h) | C ref | NEON twin (mc_neon.c) | SSE4.2 | AVX2 | condition (mc.c) | checkasm |
|---|---|---|---|---|---|---|
| `y264_mc_luma` / `y264_mc_luma_b` | `y264_mc_luma_c` | `y264_mc_luma_neon16`, `y264_mc_luma_neon8` | `y264_mc_luma16_sse4`, `y264_mc_luma8_sse4` | `_avx2` (16-wide: one 256-bit register per row; 8-wide: the shared 128-bit body) | `w<=16 && h<=16`; in-border window reads the plane directly, else `mc_luma_tile` gathers clamped samples; a narrower block computes 16- or 8-wide into `tmp` and copies out. One body, instantiated per tier by `Y264_MC_LUMA_TIER` | `mc_luma_win{,_sse4,_avx2}` (kernel, page-guarded per phase), `mc_luma` (oracle, 7 shapes x 16 phases x 144 positions) |
| `y264_mc_chroma` | `y264_mc_chroma_c` | `y264_mc_chroma_neon8` (8x8), `_w8h`, `_w4h` | `y264_mc_chroma_w8h_sse4`, `_w4h_sse4` | `_avx2` (two output rows per pass at w8, four at w4) | `(w==8 \|\| w==4) && h even && h>=2` and the window inside `Y264_CHROMA_BORDER`; **w==2 and odd h stay C** (w2 is 0 of 2.75M calls at the samsung point) | `mc_chroma_win{,_sse4,_avx2}`, `mc_chroma` (oracle, 8 shapes x 3 formats) |
| `y264_pixel_avg_wt` | `y264_pixel_avg_wt_c` | `y264_pixel_avg_wt_neon` | `y264_pixel_avg_wt_sse4` | `_avx2` (16 samples per register; 32 on the unweighted PAVGB path) | always when the class is on | `pixel_avg_wt{,_sse4,_avx2}` |
| `y264_pred_copy` | `_c` | `y264_pred_copy_neon` | `y264_pred_copy_sse4` | `_avx2` = the shared 128-bit body, VEX-encoded | `w in {4,8,16}` | `pred_copy{,_sse4,_avx2}` |
| `y264_pred_avg2` | `_c` | `y264_pred_avg2_neon` | `y264_pred_avg2_sse4` | `_avx2` = the shared 128-bit body, VEX-encoded | `w in {4,8,16}` | `pred_avg2{,_sse4,_avx2}` |
| `y264_mc_build_hpel_rows` / `_build_hpel` | scalar body in mc.c | `y264_hpel_hrow_neon`, `y264_hpel_outrow_neon` | `y264_hpel_hrow_sse4`, `y264_hpel_outrow_sse4` | `_avx2` (16 columns a pass, the 128-bit form as the step back onto the span) | interior columns only (`xin1-xin0 >= 8`, `v1-v0 >= 8`); border columns and clamped rows stay scalar | `hpel_rows{,_sse4,_avx2}` (six spans: 8, 9, 15, 16, 17, 59), `hpel_build` (oracle, all positions) |

All 6 mc jobs have twins at all three tiers; 3 are partial by shape (chroma
w2/odd h, pred_copy/avg2 odd widths, hpel borders), and the partials are the
same at every tier because the shape is what refuses them, not the ISA.

A row the x86 tiers do NOT carry: a prediction row of 16 bytes has no second
half for a 256-bit register, so both plane fetches are the same 128-bit body
at both tiers. Written down because "AVX2 kernel" there means VEX encoding
and nothing else.

### 1d. `transform.c` / `transform_neon.c` (per-call dispatch)

| public entry (transform.h) | C ref | NEON twin (transform_neon.c) | condition | checkasm |
|---|---|---|---|---|
| `y264_fdct4x4` / `y264_idct4x4` :289-290 | `_c` :152/:178 | `y264_fdct4x4_neon` :168, `y264_idct4x4_neon` :291 | DCT flag | `fdct4x4`, `idct4x4` (:430-468) |
| `y264_sub4x4_dct` / `y264_add4x4_idct` :301-304 | `_c` :229/:239 | `_neon` :188/:313 | DCT | `sub4x4_dct`, `add4x4_idct` (:476-563) |
| `y264_sub8x8_dct8` / `y264_add8x8_idct8` :305-308 | `_c` :249/:259 | `_neon` :395/:496 | DCT | `sub8x8_dct8`, `add8x8_idct8` |
| `y264_sub_dct4_blocks` :323 | `_c` :279 | `y264_sub_dct4_blocks_neon` :244 | DCT, nbw even | `sub_dct4_blocks` |
| `y264_fdct8x8` / `y264_idct8x8` :379-380 | `_c` :380/:397 | `_neon` :385/:486 | DCT | `fdct8x8`, `idct8x8` |
| `y264_quant_4x4` :403, `_f64` :408 | `quant4_flat` / matrix loop :892-932 | `y264_quant_4x4_neon` :41, `y264_quant_4x4_fneon` :24 | QUANT **and `w == NULL`** (flat CQM) | `quant_4x4`, `quant_4x4_f64` (dispatched-flat vs dispatched-w16 scalar) |
| `y264_quant_8x8` :386, `_f64` :410 | `quant8_flat` / matrix loop :625-670 | `y264_quant_8x8_fneon` :49 | QUANT and flat | `quant_8x8`, `quant_8x8_f64` |
| `y264_dequant_4x4` :419 | scalar :934-961 | `y264_dequant_4x4_neon` :67 | QUANT and flat | `dequant` (added 2026-09-17) |
| `y264_dequant_8x8` :388 | scalar :676-699 | `y264_dequant_8x8_neon` :91 | QUANT and flat | `dequant` (added 2026-09-17) |
| `y264_zigzag_abs_8x8` :366 | `_c` :740 | `_neon` :554 | SCAN | `zigzag_abs_8x8` |
| `y264_scan_mask_8x8` :369 | `_c` :748 | `_neon` :578 | SCAN | `scan_mask_8x8` |
| `y264_zigzag_scan_4x4` :372 | `_c` :761 | `_neon` :593 | SCAN | `zigzag_scan_4x4` |
| `y264_zigzag_abs_4x4` :365 | C only :732 (comment: TBL form measured 0.87x) | **none** | | |
| `y264_hadamard4x4` :393 | C :701 (int tmp[16], two passes) | **none** | | |
| `y264_hadamard2x2` :394 | C :801 | **none** | | |
| `y264_chroma422_dc` :440 | C :817 | **none** | | |
| `y264_quant_dc_luma` / `y264_dequant_dc_luma` :424-426 | C :963/:979 (16 int64 mul + shift) | **none** | | |
| `y264_quant_dc_chroma` / `y264_dequant_dc_chroma` :430-432 | C :996/:1012 | **none** | | |
| `y264_quant_dc_chroma422` / `y264_dequant_dc_chroma422` :441-443 | C :860/:840 | **none** | | |

Transform: 28 entries, 18 with twins (all quant/dequant twins are flat-CQM only), 10 without (the whole DC family, the 4x4 abs-zigzag).

### 1e. `predict.c` / `predict_neon.c`

| public entry (predict.h) | C ref | NEON twin | SSE4.2 | AVX2 | condition (predict.c) | checkasm |
|---|---|---|---|---|---|---|
| `y264_intra16x16` | `_c` :13 | `y264_intra16x16_neon` predict_neon.c:152 | `y264_intra16x16_sse4` | `_avx2` (two rows a step, all four modes) | all modes | `intra16x16{,_sse4,_avx2}` |
| `y264_intra_chroma` | `_c` :74 | `y264_intra_chroma_neon` :217 | `y264_intra_chroma_sse4` | `_avx2` (four rows a step on the fills, two on the plane; DC stays 128-bit) | **`cw == 8` only** (4:2:0/4:2:2); 4:4:4 chroma stays C | `intra_chroma{,_sse4,_avx2}` |
| `y264_intra4x4` | `_c` :151 | **none by decision** ("the builder measured a net loss") | **none, same decision** | **none** | | `intra4x4` (C vs C) |
| `y264_intra8x8` | `_c` :379 | `y264_intra8x8_neon` :100 | `y264_intra8x8_sse4` | `_avx2` = the shared 128-bit body, VEX-encoded | modes VERT/DDR/VR/HD/VL only; HORIZ/DC/DDL/HU take auto-vectorised C | `intra8x8{,_sse4,_avx2}` |
| `y264_intra8x8_from_edge` | `_c` :294 | `y264_intra8x8_from_edge_neon` :51 | `y264_intra8x8_from_edge_sse4` | `_avx2` = the shared 128-bit body, VEX-encoded | same 5 modes | inside `intra8x8{,_sse4,_avx2}` |
| `y264_intra8x8_edge_c` | C :243 (the 8.3.2.2.1 low-pass, `int t[16], l[8], tl` plus a `pixel f[32]` flat copy) | **none** | folded into the x86 8x8 builder, which derives the flat edge itself | same | | inside `intra8x8_edge` |

Predict: 6 entries, 4 with twins (2 partial), 2 without.

The x86 tiers cover the same 4 slots at both tiers: 4 kernels each. The
ROUTING is NEON's, and inherited rather than re-derived, because both refusals
are facts about the SHAPE and not about the instruction set -- sixteen samples
do not amortize an edge-filter precompute on any ISA, and a flat fill
auto-vectorises about as well as anything written by hand. Nothing in wave 3b
is timed, so a new route would have been a claim with no number behind it.

Two rows where the x86 idiom differs from NEON's and the difference is
load-bearing. The 121 reference filter needs the TRUNCATING halving add,
`(a & c) + ((a ^ c) >> 1)` over bytes, because PAVGB rounds and NEON's `vhadd`
does not -- the one place in the whole DSP layer where x86's rounding average
is the wrong instruction rather than the right one. And VR and HD, which NEON
builds with a scalar gather per sample, are one PSHUFB pair per row here:
every index those two modes need falls inside the first sixteen entries of F
and of H, so a row is two shuffles OR-ed together with the control's high bit
zeroing the lanes the other source owns. Note the per-mode indirect structure: the 4x4 decision path is already fused (`intra4x4_x9` costs all 9 modes in one NEON pass, macroblock.c:2027), but the chroma decision (macroblock.c:2300-2315) and the 8x8 decision (macroblock.c:2117-2136) still build each mode into a stack block and SATD it separately.

### 1f. `deblock.c` / `deblock_neon.c` (dsp) and the filter loops in `src/encoder/deblock.c`

| job | C | NEON | SSE4.2 | AVX2 | condition | checkasm |
|---|---|---|---|---|---|---|
| bS derivation, whole MB | `y264_deblock_strength_c` dsp/deblock.c:70-121 | `y264_deblock_strength_neon` deblock_neon.c:397 | `y264_deblock_strength_sse4` (two eight-lane passes an axis) | `_avx2` (**one sixteen-lane pass an axis**) | DEBLOCK, non-I slices (I slices use a constant grid); progressive only, and not under a `--weightp 2` duplicate | `deblock_strength{,_sse4,_avx2}` |
| luma vertical edge, 4 lines | `filter_line(step=1)` encoder/deblock.c, called per line | `y264_deblock_luma_v4_neon` deblock_neon.c:124 | `y264_deblock_luma_v4_sse4` | `_avx2` = the shared 128-bit body, VEX-encoded | per 4-line segment with bs != 0 | `deblock_luma{,_sse4,_avx2}` |
| luma horizontal edge, 4 lines | `filter_line(step=rs)` | `y264_deblock_luma_h4_neon` :158 | `y264_deblock_luma_h4_sse4` | `_avx2` = the shared 128-bit body, VEX-encoded | same | `deblock_luma{,_sse4,_avx2}` |
| chroma horizontal edge, 8 wide | `filter_line` | `y264_deblock_chroma8_h_neon` :249 | `y264_deblock_chroma8_h_sse4` | `_avx2` = the shared 128-bit body, VEX-encoded | `cstyle` (not 4:4:4) | `deblock_chroma8_h{,_sse4,_avx2}` |
| chroma **vertical** edge | `filter_line`, scalar per line | **none** (comment: "needs a gather/scatter across the stride and measured 0.87x") | **none, same refusal** | **none** | | |
| 4:4:4 chroma (luma-style filter on chroma) | `filter_line` with `cstyle=0` | **none** | **none** | **none** | | |

The x86 tiers cover the same 4 slots at both tiers: 4 kernels each. The two
refusals carry rather than being re-argued: a vertical chroma edge is eight
rows of four bytes, so any kernel gathers and scatters across the stride, and
the chroma filter moves only p0 and q0 -- too little arithmetic to amortize a
transpose on any instruction set. NEON measured both forms of that at 0.86x
and 0.87x; the x86 idiom is the same idiom with different spellings.

Three of the four do NOT widen at AVX2, and the reason is the shape rather
than the ISA: a luma edge segment is four lines because four lines is the unit
one bS value covers, and a chroma edge is eight. Four and eight 16-bit lanes
leave a 256-bit register with nothing in its upper half. The strength
derivation is the one that widens exactly, because sixteen edges of an axis
are sixteen lanes. Written down because "AVX2 kernel" for the three filters
means VEX encoding and nothing else.

Still refused at every tier, and named here so the next wave does not have to
rediscover it: the sixteen-line whole-edge form. The scalar loop feeds four
lines at a time because that is the bS unit, so a kernel taking sixteen would
have to carry four parameter sets through its lanes.

Filter shapes: 5 jobs, 3 with twins, 2 without. Note the filters run per **four-line** segment with a scalar bS per segment (the kernels take one `bs`, one `tc0`); the census in the archived doc says only 15% of half-edges are live, which is the recorded reason the whole-edge form was refused.

### 1g. Counts

| file pair | C public entries | with NEON twin | of which partial | without twin | NEON functions exported |
|---|--:|--:|--:|--:|--:|
| pixel | 26 (+SSD in encoder) | 21 (+SSD) | 0 | 5 | 26 |
| mc | 6 | 6 | 3 | 0 | 10 (9 per x86 tier) |
| transform | 28 | 18 | 5 (quant/dequant: flat CQM only) | 10 | 17 |
| predict | 6 | 4 | 2 | 2 | 4 (4 per x86 tier) |
| deblock (dsp + encoder filter shapes) | 1 + 5 | 1 + 3 | 0 | 2 | 4 (4 per x86 tier) |
| **total** | **72** | **54** | **10** | **19** | **61** |

checkasm is `tools/checkasm/{main.c,checkasm.h,pixel.c,transform.c,mc.c,predict.c,deblock.c}` since 2026-09-17, a registration table of **49 groups** -- 41 behind a cpu mask and 8 portable. pixel: sad, sad_dotprod, sad_x4, satd4x4, satd8x8, satd16x16, satd_x4_8x8, sa8d8x8, sa8d16x16, hadamard_ac8x8, texture_ac4, texture_ac48, var16x16, var16x16_dotprod, intra4x4_x9, intra_satd_x3_16, ssd, texture_ac48_c. transform: fdct4x4, idct4x4, fdct8x8, idct8x8, quant_4x4, quant_f64, dequant, sub_dct, add_idct, sub_dct4_blocks, scan, trellis_bench. mc: pred_copy, pred_avg2, pixel_avg_wt, mc_luma_win, mc_chroma_win, hpel_rows, mc_luma, mc_luma_hp, mc_chroma, hpel_build. predict: intra16x16, intra_chroma, intra8x8, intra4x4, intra8x8_edge. deblock: deblock_luma, deblock_chroma8_h, deblock_strength. `checkasm --list` is the live list with the ISA each row needs.

On x86-64 that table is a different length, because the rows are per
ARCHITECTURE: an x86 build runs the 8 portable rows plus the x86 ones, which
is **62 groups** since wave 3b -- 15 pixel, 6 mc, 3 predict and 3 deblock per
tier, at two tiers -- and 35 of them at `--isa sse4`. The mc rows are
`pred_copy_<tier>`, `pred_avg2_<tier>`, `pixel_avg_wt_<tier>`,
`mc_luma_win_<tier>`, `mc_chroma_win_<tier>` and `hpel_rows_<tier>`; the
predict rows are `intra16x16_<tier>`, `intra_chroma_<tier>` and
`intra8x8_<tier>`, and the deblock rows `deblock_luma_<tier>`,
`deblock_chroma8_h_<tier>` and `deblock_strength_<tier>`.

Wave 3b put those two families on wave 1's shape as well, so every family's
group body is now written once and takes its kernel as an argument. Four page
guards came with the move that the NEON rows had never had: the two luma edge
windows, the chroma edge's four rows, every prediction DESTINATION block --
256, 64 or 8*ch packed bytes with no padding anywhere, so a store sized to the
register rather than to the block runs straight into the guard page -- and the
32-byte flat edge array the from-edge builder reads in two overlapping
pieces.

**HD stage 4 (2026-09-19)** added `ssd_dotprod` -- 48 groups -- and a bench
row for eleven kernels that had a correctness group and no timing: ssd,
dequant 4x4 and 8x8, quant_8x8, mc_luma, mc_chroma, the two half-pel row
kernels, deblock_strength and the whole sixteen-line luma edge. The last
is the shape the reference filter takes in ONE call and we take in four,
so the head-to-head column now compares a whole edge with a whole edge.

### Multiples moved by HD stage 4

ns per call, `checkasm --bench`, this box. The reference column is the
same shape in the reference encoder's own harness.

| kernel | before | after | reference | note |
|---|--:|--:|--:|---|
| ssd 16x16 | 6.96 ns, 1.23x | **3.07 ns, 2.86x** | 3.72 ns | DotProd, four chains, straight-line at h=16 |
| ssd 8x8 | 2.36 ns, **0.83x** | **1.71 ns, 1.18x** | 1.93 ns | was slower than its own C |
| deblock_strength | 18.25 ns, 2.63x | **15.6 ns, 2.49x** | 5.91 ns | algebraic reduction; a NULL on the encode |
| intra 8x8 from edge | 2.44 / 2.68 ns | (unchanged) | 1.28 ns | bench row added; the whole-entry row times a different job |

The deblock row is the item's own reminder that the multiple is not the
answer: the kernel runs once per macroblock and is about 0.28% of a 1080p
encode, so a sixth off it is 0.04% -- inside the instruction counter's own
spread on both cells, and not consistent in sign.

### The batched inverse transform, repriced and refused (2026-09-19)

Stage 4's head-to-head has two rows for a batched 4x4 inverse -- four blocks
at 2.25x and sixteen at 2.71x against the reference encoder -- and ranked the
shape third on the strength of the `dct` class share, 4.16% and 4.50%. **That
is the wrong denominator and the rows carry the correction here.** The class
is 89% forward transform by cost, measured by calling each entry point twice
in a temporary build and reading the difference:

| shape, whole call set | sunflower_1080p | bbb10s_1080p_o120 |
|---|--:|--:|
| forward 4x4 (`sub_dct4_blocks` + `sub4x4_dct`) | 1.82% | 2.22% |
| forward 8x8 | 0.93% | 0.82% |
| **inverse 4x4** | **0.047%** | **0.046%** |
| inverse 8x8 | 0.30% | 0.37% |

So deleting the 4x4 inverse outright is 0.047% on the stage-0 cells and 0.131%
at the median board clip; a restructure collecting the full 2.71x takes 63% of
that. The op ledger says why. A coded macroblock runs **3.5 to 11.5** inverse
4x4 calls, not sixteen: all-zero blocks are a row copy and DC-only blocks a
flat add, and the recon loop already shortcuts both. Sixteen-of-sixteen nonzero
is the benchmark's shape, not the encoder's.

The live transform item is the **batched forward**, which we lose at 1.29x on
1.8-2.2% of the encode. `local/records/resid-batch-2026-09-19.md` has the
census, the ceiling probe and the per-clip tables.


What the split changed, beyond the four kernels that had no group: the quant and dequant groups used to flip `Y264_ASM_OFF` and compare the dispatcher against itself, because the flat-CQM multiplier row is a table the dispatcher owns and nothing exported it. The kernels take that row as an ARGUMENT, so the harness now builds it from the specification's normAdjust tables and hands it to the kernel while the reference derives its own from the library's tables through the weighted path with a flat matrix of 16 -- identical multipliers by construction, `(16*mf + 8)/16 == mf` -- and the 4x4 forward row is cross-checked against the public `y264_mf4_at()`. The two sides agree only if both transcriptions are right.

Groups that were C-against-C tautologies are gone rather than renamed: the old `sad` and `sad_x4` rows ran all seven partition shapes, but 8x4/4x8/4x4 SAD and 4x8/4x4 `sad_x4` have no NEON twin, so those cells compared `ref.sad[pu]` with `opt.sad[pu]` where the two were the same function pointer. The NEON rows cover the shapes that have kernels; the shapes that do not are recorded here instead.

Three kernels became reachable for the first time as SYMBOLS rather than through a whole-plane build: `mc_luma_win` and `mc_chroma_win` drive the interpolation window kernels at the contract the dispatcher promises them, and `hpel_rows` drives `y264_hpel_hrow_neon` / `y264_hpel_outrow_neon` against the definitional six-tap at every column of the span. The whole-plane `hpel_build`, `mc_luma` and `mc_chroma` rows stay as portable ORACLES over the public entry point, because the border body and the clamped tile gather are C on every architecture and are where a coordinate bug actually lives.

---

## 2. Scalar pixel / coefficient loops outside `src/dsp`

Layout key used below: `src` = source plane pointer with stride `ss` (`f->src_stride[c]`), `rec` = recon plane with stride `rs`, `pred` = stack block stride 16 (luma) or `cw` (chroma, 8 at 4:2:0), coefficients are `dctcoef` = `int16_t` at 8-bit (`src/common/bitdepth.h:20-25`), raster order unless stated.

### 2a. `src/encoder/macroblock.c`

| # | file:line | function | what it computes | layout | caller-side structure / awkwardness |
|---|---|---|---|---|---|
| 1 | :337-349 | `mb_ac_energy` | sum and sum-of-squares of a w x h block (chroma AQ energy) | uint8 strided, w=8/16 | Called only for chroma at `aq_chroma`; luma already uses `y264_dsp.var16x16` (:378). Returns double. Same loop duplicated as `blk_ac_energy` in encoder.c:5304. |
| 2 | :816-828 | `apply_wp_luma` | explicit weighted prediction `clip8(((p*w + rnd) >> D) + o)` in place | pred block stride 16, bw x bh in {16x16,16x8,8x16,8x8} | Per partition, P slices with `wp_luma[r]` set only. No NEON. (`bipred_avg` = `y264_pixel_avg_wt` is NEON; explicit uni-pred WP is not.) |
| 3 | :868-887 | `ssd_block` | SSD | strided uint8 | NEON for w=16 and w=8 only; scalar for w=4 (rare) and, notably, chroma at 4:2:2/4:4:4 goes through w=8/16 fine. Called from `ssd_mb` (:1701), `inter_res_4x4_plane` (:3553), `inter_res_8x8` (:3603). |
| 4 | :986-1008 | `block_J` | dequant + idct + sum (res-diff)^2 in the transform domain, plus optional psy fdct of clip8(pred+res) | int16[16] raster, pred strided | Greedy RDOQ only (subme>=9 or CAVLC or psy-trellis); the medium tier uses the Viterbi lattice instead. Per-trial dequant + idct calls. |
| 5 | :1144-1310 | `rdoq_4x4_ctx` | quant, all-zero scan (:1170), scan-order gather `ZIGZAG[i+base]` (:1207-1215 with CQM; :1197-1204 via `y264_zigzag_abs_4x4` when flat), sign restore back to raster (:1224-1228) | int16 raster in, int[16] scan arrays for the lattice | Called **once per 4x4 block**, 16-24 times per MB, from 5 sites (:1888, :2039, :2333, :3209, :3527). Each call: dispatch branch for quant, TLS-free but 11 args. The residual rebuild `diff[]` at :1236-1239 is a 4x4 subtract duplicated from the fused `sub4x4_dct` (greedy path only). |
| 6 | :1367-1489 | `rdoq_8x8` | same at 8x8: quant, all-zero scan (:1385), `zigzag_abs_8x8` (NEON) x2, `trellis_8x8`, sign restore + mask/big fold (:1425-1432) | int16[64] raster, int[64] scan | Per 8x8, 4 per MB. Sign restore loop is 64 scalar gathers. |
| 7 | :1867-1893 | `encode_luma16` block loop | after the batched `sub_dct4_blocks`, per block: copy `lev` to `ac_lev[b][k]` and count nonzeros (:1889-1890) | int16[16] | Per-4x4 indirect: `rdoq_4x4_ctx` call then a 16-element copy+count. |
| 8 | :1897-1899, :1915-1917 | I16 DC path | `y264_hadamard4x4` (C) then `y264_quant_dc_luma` (C), inverse `hadamard4x4` + `dequant_dc_luma` (C) | int16[16] | No NEON on any of the four DC steps. |
| 9 | :1927-1937 | I16 DC-only reconstruction | `rec = clip8(pred + (rdc+32)>>6)` for 4x4 blocks whose AC is zero | pred stride 16, rec stride rs | This is the DC-only add shape; scalar here, per block, inside a per-block loop that alternates with the NEON `add4x4_idct`. |
| 10 | :1943-1957 | I16 scan extraction | `dc_scan[k] = dclev[ZIGZAG[k]]`; per block `ac_scan[i][k] = ac_lev[b][ZIGZAG[k+1]]` + nz count | int16 raster to scan | 16 blocks x 15 gathers, blkIdx-ordered via `BLK_X/BLK_Y`. |
| 11 | :2027-2069 | `encode_luma4x4` per block | fused NEON `intra4x4_x9`, C `y264_intra4x4` builder, NEON `sub4x4_dct`, `rdoq_4x4_ctx`, scan gather `lev[ZIGZAG[k]]` + nz (:2050-2056), NEON dequant+add or scalar `rec = pred` copy (:2064-2067) | pred stride **4**, src/rec strided | Closed-loop per 4x4 (each block's prediction reads the previous block's recon), so no batching across blocks is possible; only the inner per-block ops can be kernels. |
| 12 | :2117-2136, :2153-2166 | `encode_luma8x8` | 9-mode loop: C edge derivation once, then per mode a builder (5 NEON / 4 C) + `satd_block` + `memcpy` of the winner; sub-dct8 NEON; rdoq_8x8; dequant+add NEON; else scalar 8x8 `rec = pred` copy | pred stride **8** | Per-mode indirect builder + SATD; the `memcpy(best_pred, pred, 64)` on every improvement. |
| 13 | :2251-2267, :2271-2282 | `chroma_dc_fwd`, `chroma_dc_inv` | `y264_hadamard2x2` + `y264_quant_dc_chroma` (C), scan gather; 4:2:2: `chroma422_dc` + its quant (C) | int16[4] or [8] | Tiny; no NEON. |
| 14 | :2300-2315 | `encode_chroma` mode loop | up to 4 modes x 2 planes: builder (NEON for cw==8) + `satd_block` + scalar copy of the winner (`for k < cw*ch`) | pred stride cw | Per-mode indirect; no fused chroma-x4 cost kernel. |
| 15 | :2340-2345, :2382-2391 | `encode_chroma` per block | `ac_lev[blk][k] = lev[k]` + nz; scan gather `ac_lev[blk][ZIGZAG[k+1]]` | int16 | Same shape as #7/#10. |
| 16 | :2363-2373 | chroma DC-only recon | `rec = clip8(pred + flat)` per 4x4 | pred stride cw | Same as #9. |
| 17 | :3154-3162 | `add_dc_4x4` | 4x4 `clip8(pred + dc)` via a 4-byte temp + memcpy | pred stride ps, rec stride rs | The inter-chroma DC-only recon (:3216-3222). Scalar. |
| 18 | :3164-3282 | `encode_chroma_inter` | as `encode_chroma` without the mode loop; `memcpy(ac_lev..)` :3197, branchless nz :3199, DC-only or `add4x4_idct`, then scan gather (:3255-3263) | | |
| 19 | :3297-3438 | `build_inter_pred` | luma via `y264_me_mc_luma` (plane read: NEON copy/avg2), `apply_wp_luma` (scalar), chroma via `y264_mc_chroma` (NEON w4/w8) | pred stride 16 / cw | Sub-partition loops at :3316-3327 issue per-4x4 `y264_me_mc_luma` calls (each with a linear search of the hpel registry, me.c:563-569). |
| 20 | :3484-3494 | `decimate_mask` | run-weighted decimate score from a scan-order bitmask | uint32/64 mask | Bit-serial `ctz` loop; input mask comes from the NEON `zigzag_scan_4x4` / `scan_mask_8x8`. Cheap. |
| 21 | :3496-3556 | `inter_res_4x4_plane` | batched NEON `sub_dct4_blocks`, then per block: rdoq, NEON `zigzag_scan_4x4`, decimate, popcount, NEON dequant + add-idct, **or** 4 row memcpys `rec4 = pred` (:3549-3550); final `ssd_block` 16x16 (NEON) | pred stride 16, rec4 stack stride 16 | Per-block indirect (16 x rdoq + 16 x dequant/idct). The all-zero `rec = pred` copy and the DC-only add are the two scalar shapes left in the residual path. |
| 22 | :3562-3606 | `inter_res_8x8` | per quadrant: NEON sub-dct8, rdoq_8x8, decimate, `memcpy(lev8)` 128 B, NEON dequant8 + add-idct8 or 8 row memcpys | | |
| 23 | :3951-4110 | `encode_inter_res_tp` | tr-pre decision (`satd16x16` + `sa8d16x16`, both NEON), 4x4 and/or 8x8 paths, winner copy `rec <- win` 16 rows of memcpy (:4067-4068), 4:4:4 chroma via `inter_res_4x4_plane` then a **scalar 16x16 element copy** (:4094-4095) | | The 4:4:4 chroma copy at :4094 is a per-element loop where the luma path uses memcpy. |
| 24 | :4148-4179 | `save_mb_rec` / `load_mb_rec` | snapshot/restore of the MB's recon: luma 16 rows x 16 B, chroma 2 planes x (16/sub_h) rows x (16/sub_w) B = **384 B at 4:2:0** per snapshot | strided plane to packed `pixel *buf` | 26 call sites (`grep -c`). The `MB_REC_ROWS` macro (:4136-4146) splits on w in {16, 8} so clang inlines the copy; the doc comment says the runtime-length form cost 3.02% of wall (`_platform_memmove`). Stage-profiled as `STG_SNAP`. |
| 25 | :1644-1672, :4183-4190 | `save_mb_nnz` / `load_mb_nnz` / `clear_mb_nnz` | nnz grid rows: 4 x 4 B luma + 2 planes x cbh rows x cbw B chroma (16+8 B at 4:2:0) | int8 grid, stride `nnz_stride[c]` | Row memcpys with compile-time lengths behind a `cbw` branch. |
| 26 | :4444, :4472-4473, :6236-6238, :7800-7801 | `struct bpred_cache` | stashes uni-pred luma (256 B) + chroma (2 x 256 B) per list for the Bi average: 768 B x 2 memcpy per B candidate | packed stride 16 / cw | Bi = `bipred_avg` (NEON `pixel_avg_wt`) over the cached blocks (:4477-4479). |
| 27 | :4481-4494 | `build_bpred` no-cache path | two full MC builds + `bipred_avg` | | |
| 28 | :5509-5568 | `build_direct_pred` | per 8x8 quadrant: 2 x `y264_me_mc_luma` into 256-B stride-16 temps, then a **scalar 8x8 weighted average** `clip8((p0*w0 + p1*w1 + 32) >> 6)` (:5524-5525); chroma quadrants (4x4 at 4:2:0) the same scalar loop (:5557-5558) | p0/p1 stride 16, dst stride 16; chroma qw stride | The weighted average exists as a NEON kernel (`y264_pixel_avg_wt`, packed n) but is not used here because the quadrant is a strided sub-block of a 16-wide buffer. Same scalar loop repeated in `build_b8_pred` :5613-5614, :5647-5648, and the 4:4:4 branches :5535, :5626. |
| 29 | :5703-5720 | `store_pred_rec` | B_Skip recon: 16x16 luma + chroma element-wise copy `rec = pred` | strided | Scalar per-element (not memcpy). |
| 30 | :2783-2815 (`analyze_intra_g`) | I16 / I8 recon snapshots | `tmp16`/`tmp8` 256-B row memcpys of the luma recon, restored on the winner | stride rs to packed | Another snapshot shape, luma-only. |
| 31 | :9461-9487 | `coef_signif` | strict: `|coef|*mf >= thr` scan over 16; loose: quant + nonzero scan | int16 raster + `y264_mf4_row` | Skip probe; strict path could be a 16-lane compare. |
| 32 | :9529-9598 | `probe_signif_rdoq` | quant (NEON), nonzero scan, scan-order gather with `unmf`/`w2` rows, `trellis_4x4`, then a scan-order mask fold | int16 raster to int[16] scan | The scan gather at :9569-9578 is the CQM (`w != NULL`) form of #5's gather, run even when flat because it also fills `unmf` from `ur[r]`. |
| 33 | :9636-9700 | `probe_skip_g` | per 4x4 NEON `sub4x4_dct` **straight from the recon plane** (`pred = f->rec`, stride rs) then #31/#32; chroma likewise with a DC gather | src/rec strided | 16 + 8 per-block dispatched calls; uses `y264_sub4x4_dct` rather than the batched `sub_dct4_blocks`. |
| 34 | :9781-9848, :10054-10097, :10221-10255 | `est_ctx_snap`, `est_p_save/restore`, `est_b_save/restore`, `est_snap_save/restore` | CABAC context copy: 460 B (`Y264_CABAC_CTX_BASE`, cabac.h:18) or 1024 B (4:4:4); mvd grid rows 4 x 8 B x 2 or 4 lists; nnz (#25); i4mode 4 x 4 B | packed struct fields | The est-ctx swap is a pointer swap (`c->ctx = s->ctx_s`) plus one 460-B memcpy per trial. Also 35 more `memcpy(.., Y264_CABAC_CTX)` sites (1024 B each) for wavefront/slice context seeding (:8093-8215, :10895-11371) at row/frame granularity, not per MB. |

Not in the table but present: `satd_block` (:847-865) and me.c `satd_blk` (:792-808) loop the NEON satd4x4/8x8 for non-16x16 rectangles (8x4, 4x8, 4x4 remainders call `satd4x4` per tile); `i16_costs_x3` (:1822-1846) gathers the 16 top/left samples scalar before the fused NEON cost.

### 2b. `src/encoder/encoder.c`

| # | file:line | function | what | layout | notes |
|---|---|---|---|---|---|
| 35 | :1623-1637 | `pad_plane` | copy the input picture into the padded plane and replicate the right edge | src stride from the API picture to `pstride` | Per input frame, 3 planes. Row copy is an element loop, not memcpy. |
| 36 | :1749-1761 | `extend_plane` | replicate left/right borders per row (element loop), then top/bottom rows by memcpy | bordered plane | Once per stored recon (luma border 32, chroma 16). |
| 37 | :1832-1848, :1872-1880 | `estimate_wp_luma`, `src_luma_sum` | full-plane sums of src and ref (`uint64 += pixel`) | W x H interior of the padded plane | Two full-frame passes per P frame when WP estimation runs; `var16x16`'s sum half could serve. |
| 38 | :4867-4877 | `downscale` | 2x2 box filter `(a+b+c+d+2)>>2` to the lowres plane | src stride ss, dst packed `lw` | Per frame (and per buffered B at :7455); `sc_downscale` :15929 is the clamped variant. x264 does this plus the half-pel planes in one vector pass. |
| 39 | :4880-4886 | `blk8_satd` | `y264_dsp.satd8x8` (NEON) | | |
| 40 | :4889-4904 | `blk8_intra` | per 4x4: scalar 16-sum, flat block fill, NEON `satd4x4` | | Legacy path, gated off by default (`Y264_LR_INTRA_NEIGHBOUR=1`). |
| 41 | :4934-4986 | `blk8_intra_neighbour` | 8x8 DC/V/H/plane predictions built scalar into `pred[64]`, each scored with NEON `satd8x8` | packed stride 8 | Default lowres intra cost, per 8x8 lowres block per frame (and per B). The plane build at :4966-4977 is 64 scalar mul-adds. |
| 42 | :5304-5316 | `blk_ac_energy` | duplicate of #1 for the mb-tree AQ weights (chroma only; luma uses `var16x16`) | | |
| 43 | :5268-5290, :5462+ | `splat_prop`, `splat_prop_qp` | bilinear 4-way deposit into the `double` propagate grid, clamped | `double grid[wmb*hmb]` | This is the mb-tree propagate kernel. Scalar, one MB at a time, with an order dependence: the comment at :5273-5279 says the four adds must not be reordered (clamped targets can alias). Callers: `la_chain_prop` :5445-5452, buffered-B loop :7466-7475, and the wholebuf walk. |
| 44 | :5334-5396 | `mbtree_invqscale` | `var16x16` (NEON) per MB, then per-MB `log2`, `pow(2, -x/6)` and a normalisation pass | double arrays | Transcendental per MB; x264 uses a table. |
| 45 | :5588-5618 | `build_lr_subpel_1` | **15 quarter-pel bilinear phase planes** of the lowres plane: `(w00*r0[X] + w10*r0[X+ax] + w01*r1[X] + w11*r1[X+ax] + 8) >> 4` | packed `lw` stride, 16 planes of lw x lh | The archived doc records a hand kernel at 1.07x and a specialised one at 0.94x (auto-vectorised already), and that the cost is the 15-plane WORK (x264 builds 3 half-pel planes). 3.0% of t1 wall on samsung in that profile. Per anchor per lowres build. |
| 46 | :5666-5672, :8729-8735 | `blk8_sad_qp`, `blk8_sad_fpel` | `y264_dsp.sad[8x8]` (NEON) against a phase plane | | Search ring is not routed through `sad_x4` (`Y264_SATDX4` batched SATD is off: bench 1.01x). |
| 47 | :6234-6248 | `lr_bipred` | 8x8 `(a+b+1)>>1` average of two lowres refs | packed stride 8 out, `lw` in | Scalar; `y264_pred_avg2` would fit if both inputs shared a stride (they do: `lw`). |
| 48 | :9544 | `frame_complexity` | (not read in full) per-frame cost sums driving ABR | | |

Lowres ME itself (`lr_me_block` :8737, `lr_fme_block` :8948, `blk8_inter_coh`) is control flow over NEON SAD/SATD probes.

### 2c. `src/encoder/me.c`

| # | file:line | function | notes |
|---|---|---|---|
| 49 | :608-616 | `sad` | scalar SAD fallback for odd shapes; used by `sad_int` when `c->sad_fn` is NULL (never for the 7 PU shapes) and by `sad_blk` for non-PU shapes |
| 50 | :693-711 | `sad_int` | in-window: `c->sad_fn` (table); out-of-window: **per-pixel clamped scalar SAD** (:703-709). The archived doc says bus takes the clamped path on 55.6% of luma MC calls; ME probes out of window are the same class. |
| 51 | `src/dsp/mc.h` | `y264_mc_luma_hp_i` | the plane read itself: one table lookup per position, then NEON `pred_copy` or `pred_avg2` at the caller's destination stride. Inline in the header on purpose -- out of line it cost +0.22% to +0.46% instructions across the board. `y264_mc_luma_hp` in mc.c is the same body as a symbol, for checkasm |
| 52 | :550-583 | `y264_me_mc_luma_s` | linear scan of the hpel registry (`s_met.hpel[i].ref == ref`, up to 17 entries) on **every** MC call, then plane read or 6-tap fallback. Every luma MC in analysis routes here, the P_Skip prediction included; the six-tap runs only out of window |
| 53 | :730-783 | `probe_int_list` | `sad_x4` batching of integer probes |
| 54 | :890-926 | `probe_hpel_x4` | `sad_x4` batching of pure half-pel probes off the planes |

### 2d. Entropy coders

| # | file:line | function | what | notes |
|---|---|---|---|---|
| 55 | `src/encoder/cabac.c` :554-566, :568-577, :579-618 | `y264_cabac_encode_decision`, `_bypass`, `_terminate` | one bin each; `E_DEC` macro at cabac.c (the `#define E_DEC` block): rangeTabLPS lookup, branch on MPS, `ctx_trans` state update, `clz` renorm, `E_PUTBYTE` carry handling | Scalar, one bin per call, engine state loaded/stored per call (`E_LOAD`/`E_STORE`). `est_mode` branch first. Bypass runs are batched (`E_BYPASS_N`). x264 has aarch64 asm for the same engine; the archived doc measured ours at 1.07-1.10x of it per bin. |
| 56 | `cabac.c` :1477-1590 | `y264_cabac_residual` | nonzero mask fold over n levels (:1485-1487), sig/last bins, then levels via `E_LEVELS` | int16 scan-order in | Two walks: an est-mode walk with `est_tab` fused lookups and a real walk. 8x8 form at :1592. |
| 57 | `cabac.c` :867-902 | `y264_cabac_residual_bits` | rate estimate for the greedy RDOQ: last-nonzero scan + per-position `est_decision` | | Greedy RDOQ only. |
| 58 | `cabac.c` :958, :1246, :1351 | `trellis_core`, `y264_cabac_trellis_4x4`, `_8x8` | the Viterbi RDOQ lattice (8 nodes, reverse scan) | int arrays in scan order | Benchmarked in checkasm (:1512-1556). Not pixel work. |
| 59 | `src/encoder/cavlc.c` :320-400, :408+ | `cavlc_len_tc`, `y264_cavlc_residual` | total_coeff/trailing-ones scan, level codes, total_zeros, run_before | int16 scan order | The archived doc notes `cavlc_len_tc` at 1.9% of wall on the intra decision (CAVLC builds only); it was made branchless (mask + clz). |
| 60 | `src/common/bitstream.c` | `y264_bs_write*` | bit writer | | not pixel work |

---

## 3. Data layout

**Planes.** `plane_alloc` (encoder.c:1705-1710): `malloc(stride * (h + 2b))` with `stride = w + 2*b + plane_pad()`, returning the interior pointer `base + b*stride + b`. Luma border `Y264_LUMA_BORDER = 32`, chroma `Y264_CHROMA_BORDER = 16` (mc.h:164-165). `pstride[0] = padded_w + 64 + pad`, `pstride[1] = padded_w/sub_w + 32 + pad` (encoder.c:4266-4268). `padded_w/h` are MB-aligned. **No alignment attribute or aligned allocator exists anywhere in `src/`** (grep for `aligned_alloc|posix_memalign|alignas|__attribute__((aligned` returns nothing). So a luma row start is `malloc(16-aligned) + 32*stride + 32`, which is 16-aligned only when `stride` is a multiple of 16, i.e. when `padded_w` is (it is, since padded_w is a multiple of 16 and `plane_pad()` defaults to 0). Chroma: `padded_w/2 + 32`, a multiple of 8, so chroma rows are 8-aligned, not 16. MB-offset pointers (`+ mbx*16`) keep luma 16-alignment; chroma MB origins are 8-aligned. All NEON kernels use unaligned loads, so this is a performance question, not correctness.

**Half-pel planes** share the reference's geometry (same stride, interior origin) and are built over the full bordered extent (mc.h:227-236). The hpel build scratch is `int32_t` with `sstride = padded_w + 64` and `padded_h + 64 + 5` rows (encoder.c:4306-4309).

**Lowres planes** are packed `lw x lh` with `lw = padded_w/2` (encoder.c:4323-4326), no border; the 15 phase planes are each a separate `malloc(lw*lh)` (encoder.c:4383). The lowres search clamps MVs to the frame instead of reading a border (`lr_me_block` :8743-8744).

**Macroblock scratch.** There is no persistent fenc/fdec cache: every analysis function reads `f->src[c] + (mby*16)*ss + mbx*16` and writes `f->rec[c] + ...` **directly in the frame planes** (e.g. macroblock.c:1853-1855, 2004-2005, 3972-3973). Prediction and trial reconstruction live in stack arrays:

| buffer | type | stride | where |
|---|---|---|---|
| luma pred | `pixel pred[256]` | 16 | build_inter_pred :3297, encode_inter_mb :4118, build_bpred :4452 |
| chroma pred | `pixel cpred[2][256]` | `cw` (8 at 4:2:0, 16 at 4:4:4) | same; `y264_mc_chroma(cpred[c], cw, ...)` :3403 |
| intra candidates | `pixel best_pred[256]` (16), `[16]` (stride 4), `[64]` (stride 8), chroma `pred[2][256]` (stride cw) | 16 / 4 / 8 / cw | :1862, :2024, :2114, :2296 |
| trial recon | `pixel rec4[256], rec8[256]` | 16 | encode_inter_res_tp :4022 |
| bi-pred cache | `struct bpred_cache { pixel l[2][256]; pixel c[2][2][256]; }` | 16 / cw | :4444 |
| direct/B8 quadrant temps | `pixel p0[256], p1[256]` (stride 16, 8x8 used), `q0[64], q1[64]` (stride qw) | mixed | :5520, :5551 |
| recon snapshot | `pixel buf[...]` packed rows, 384 B at 4:2:0 | packed | save_mb_rec :4148 |

Because the recon is written straight into the frame plane, every RD candidate that touches `f->rec` needs `save_mb_rec`/`load_mb_rec` around it (26 call sites), and I16/I8/I4 candidates are compared by snapshotting the luma recon into `tmp16`/`tmp8` (:2783-2815). That snapshot traffic is what the archived profile attributed to `_platform_memmove` (3.02% before the compile-time-length fix).

**Coefficients.** `dctcoef` is `int16_t` at 8-bit, `int32_t` above (bitdepth.h:20-25). Transform/quant kernels take and return **raster** `[16]`/`[64]` blocks (transform.h:259). Scan order is produced separately, three times over: (a) `y264_zigzag_scan_4x4` (NEON, inter luma) into `ir->lev[blk][16]`; (b) scalar `lev[ZIGZAG[k]]` gathers into `struct luma_result.ac_scan[16][15]`, `i4_result.lev[16][16]`, `chroma_result.ac_scan[2][..][15]` for the intra and chroma paths (:1943-1957, :2050-2056, :2382-2391, :3255-3263); (c) `int[16]`/`int[64]` scan-order magnitude arrays for the trellis (`qn`, `absc`, plus `unmf`/`w2` rows pre-permuted by `y264_unquant4_row_zz`). 8x8 levels are kept raster (`i8_result.lev[4][64]`, `inter_result.lev8[4][64]`) and the CABAC 8x8 writer scans them itself. The entropy coder reads int16 scan-order arrays; CAVLC reads 4x4 sub-scans of the 8x8 with a stride-4 `scan8 + j` trick (:1312-1321).

**Per-4x4 grids** (`f->mvx/mvy/mvx1/mvy1` int16, `refidx` int8, `nnz[3]` int8, `i4mode` int8, `mvd*` int16) have their own strides (`mv_stride`, `nnz_stride[c]`, `i4mode_stride`); the bS kernel reads them through `struct y264_bs_ctx` with a 5x5 window (deblock.h:554-563).

---

## 4. Latest profile naming uncovered C symbols

This tree's `docs/` has no `archive/` directory and no per-symbol coverage table (grep for `uncovered|self-time|encode_inter_res|analyze_b_mb|_platform_memmove` hits only the parity review's prose). The table lives in the old tree:

**`docs/archive/goal3-coverage-ranking.md` in the old tree (yah264old), dated 2026-08-17 in its heading, last committed 2026-08-25 (3702c5a).** Method: `bench/lowrate/coverage.sh` on both shipped binaries, `--threads 1`, samsung_720p at 1200 kbps and bus_cif looped x6 at 400 kbps. Both scripts still exist here (`bench/coverage_gap.sh`, `bench/lowrate/coverage.sh`).

Coverage on those runs:

| | ours NEON | ours uncovered | x264 NEON | x264 uncovered |
|---|--:|--:|--:|--:|
| samsung | 24.6% | 72.9% | 54.2% | 45.9% |
| bus | 21.8% | 76.8% | 48.2% | 51.8% |

Per-area milliseconds (ours vs x264, same clip and point):

| area | sams ours | sams x264 | x | bus ours | bus x264 | x |
|---|--:|--:|--:|--:|--:|--:|
| ME search core | 382 | 248 | 1.54 | 403 | 267 | 1.51 |
| subpel refine + probe | 301 | 485 | 0.62 | 354 | 560 | 0.63 |
| SATD / SA8D kernels | 423 | 361 | 1.17 | 310 | 280 | 1.11 |
| SAD kernels | 139 | 203 | 0.69 | 132 | 185 | 0.71 |
| residual encode | 394 | 158 | 2.50 | 340 | 180 | 1.88 |
| SSD / dist | 93 | -- | -- | 51 | -- | -- |
| RDOQ / trellis | 137 | 70 | 1.95 | 201 | 166 | 1.21 |
| entropy emit | 228 | 68 | 3.35 | 310 | 150 | 2.07 |
| intra analysis | 112 | 65 | 1.74 | 45 | 63 | 0.72 |
| mode orchestration | 329 | 94 | 3.49 | 271 | 109 | 2.50 |
| chroma MC | 43 | 75 | 0.57 | 23 | 88 | 0.26 |
| deblock | 91 | 10 | 9.30 | 32 | 11 | 2.85 |
| lookahead / mb-tree | 156 | 65 | 2.40 | 123 | 37 | 3.34 |
| hpel plane build | 23 | 25 | 0.90 | 31 | 13 | 2.38 |
| memmove / memset / nnz bookkeeping | 113 | 12 | 9.28 | 81 | 16 | 4.91 |

Single symbols that doc and the companion memory (`goal3-route-is-neon-coverage`, 2026-08-17) priced on the t1 samsung profile: `_platform_memmove` 3.02% (RD snapshot traffic), `build_lr_subpel_1` 2.34-3.0%, `deblock_strength` 1.45% (since kernelised), `rdoq_4x4_ctx` 1.32%, `rdoq_8x8` 1.29%, `cavlc_len_tc` 1.9% (intra decision), `y264_mc_luma_c` 1.33% on bus (since tiled), `mbtree` walk 4.4-6.7%. The doc's own arithmetic: the byte-identical coverage list reaches 6-9% of t1 wall against a 17% requirement, and the two larger items (15 lowres phase planes, RD trial count) are quality-gated.

Two later readings supersede its ranking: memory `coverage-list-re-ranked` (2026-08-18) says the Intra8x8 nine-mode fusion targets a path the medium preset skips (`analyze_Icb` = 1.0% of bus t12 pool CPU) and moves batched inverse DCT + DC transforms to the top; and `docs/parity-review-2026-09-01.md:89` states the split of uncovered C time (1733 ms vs x264's 755 on the coverage profile) into pixel work versus control flow "was named on 08-20 and never run", with :235 asking for it on sunflower and shields at board CRFs.

Older ground truth for the pure-C tier (memory `purec-1x-profile`, 2026-07-22, macOS `sample`, samsung CRF30 t1 pure-C): flat self-time is distributed, SATD/SA8D ~1363 samples, ME ~1035, transform/quant ~734, trellis ~723, MC ~579 (`mc_build_hpel` 334), intra ~397, `encode_inter_res` 256.

---

## 5. What is scalar and vectorisable, condensed

Grouped by the kind of kernel a later round would write, with the call shape that constrains it.

**Already have a NEON kernel, wrong call shape at the site**
- 8x8 weighted bipred average in `build_direct_pred` / `build_b8_pred` (macroblock.c:5524, 5557, 5613, 5647): strided 8x8 sub-blocks of a 16-wide buffer; `y264_pixel_avg_wt` is packed-only. A strided 8x8 variant, or writing the two MC temps at stride 8, removes 4 scalar loops per B MB.
- `lr_bipred` (encoder.c:6244-6247): 8x8 `(a+b+1)>>1` with equal input strides; `y264_pred_avg2` fits as is.
- 4x4 SATD / SAD remainders in `satd_block`, `satd_blk`, `sad_blk`: the 8x4/4x8/4x4 SAD table slots have no NEON, and rectangular SATD is looped per 4x4.
- `probe_skip_g` (:9636-9700) calls `y264_sub4x4_dct` 20 times per MB (16 luma, 4 chroma at 4:2:0) where the batched `y264_sub_dct4_blocks` exists. At about 34 forward 4x4 blocks per MB on both stage-0 cells it is the largest single contributor to `dct4_blk`, and the forward 4x4 is 1.8% to 2.2% of the encode. It is **not** a straight batching candidate: the loop returns early the moment a block codes to something, so hoisting all twenty transforms in front of the decision does work the probe currently skips. Sizing that trade needs a census of where the loop exits; not yet run (2026-09-19).

**No kernel; per-block, fixed size**
- DC-only reconstruction `clip8(pred + dc)` for 4x4 (three copies: :1927-1937, :2363-2373, `add_dc_4x4` :3154) and the `rec = pred` copies (:2064, :2163, :3549, :3596, `store_pred_rec` :5703).
- DC transform family: `hadamard4x4` (:701), `hadamard2x2` (:801), `chroma422_dc` (:817), and all six `quant_dc_*`/`dequant_dc_*` (transform.c:963-1024, 840-875).
- Explicit WP `apply_wp_luma` (:816).
- Scan-order gathers and nonzero counts on the intra and chroma paths (#7, #10, #15, and rdoq's sign restore #5/#6); the inter luma path already uses the NEON `zigzag_scan_4x4`.
- `coef_signif` strict compare (:9479-9486).
- Intra chroma mode decision and I16 PLANE / I8x8 per-mode SATD loops (fused-cost form like `intra4x4_x9`; the memory says price at t12 on an inter cell first).

**No kernel; per-frame / per-plane**
- `downscale` / `sc_downscale` (encoder.c:4867, 15929).
- `build_lr_subpel_1` (:5588): auto-vectorised already, 15 planes of work; a kernel was measured null twice.
- `blk8_intra_neighbour` predictors (:4934) and `blk_ac_energy`/`mb_ac_energy` (chroma AQ).
- `pad_plane` row copy (:1623), `extend_plane` side borders (:1749), `estimate_wp_luma` / `src_luma_sum` plane sums (:1832, :1872).
- `splat_prop` / `splat_prop_qp` mb-tree deposit (:5268, :5462): order-dependent double adds; x264 vectorises the matching deposit by splitting it into a compute pass and a scatter.
- Deblock: vertical chroma edges and 4:4:4 chroma (encoder/deblock.c:294-300, cstyle=0), plus the four-line-with-scalar-bS filter shape (8-line half-edge form was the recommended next shape).
- `y264_mc_chroma` w=2 / odd h, `y264_intra_chroma` at 4:4:4 (cw=16), `y264_intra4x4` builder (refused by measurement), `intra8x8` HORIZ/DC/DDL/HU (auto-vectorised C by measurement), `intra8x8_edge_c`.

**Copy traffic rather than arithmetic**
- `save_mb_rec`/`load_mb_rec` 384 B per snapshot x 26 sites, `analyze_intra_g` tmp16/tmp8, `bpred_cache` 768 B x 2, est-context 460 B per trial, `encode_inter_res_tp` winner copy and 4:4:4 element copy (:4094). These are the `_platform_memmove` group in the profile; the fix already taken was compile-time lengths, and the structural fix (an fdec scratch like x264's so candidates never touch the frame plane) is a layout change, not a kernel.

**Not vectorisable, listed for completeness**
- CABAC bin engine (`E_DEC`), residual writers, CAVLC length walk, greedy RDOQ, Viterbi trellis, `decimate_mask`, `y264_me_mc_luma`'s registry scan.
