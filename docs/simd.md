# SIMD

Every hot pixel loop in yah264 has a plain C reference and one or more SIMD
kernels written in C11 intrinsics, with no hand-written assembly. At run time
the encoder reads the CPU's feature word once and picks the widest kernel the
chip supports. Each kernel matches its C twin byte for byte, so the tier never
changes the output at one thread.

The [Architectures](https://terranvigil.github.io/yah264/architectures.html)
page lists the supported processors and each tier's kernel count and speed
against x264. This doc covers how the tiers are built and tested.

## How it works

### Tiers

| tier | architecture | what it covers | runs when |
|---|---|---|---|
| NEON | aarch64 | every kernel family | always on aarch64 |
| NEON + dot product | aarch64 | SAD 16x16 and 8x16, 16x16 variance, SSD | the CPU reports dotprod: Apple M1 and later, Graviton2 and later |
| SVE | aarch64 Linux | some pixel metrics and the forward quant, listed below | the CPU reports SVE at 256 bits or wider, as Graviton3 does |
| SSE4.2 | x86-64 | every kernel family | the x86 floor |
| AVX2 with FMA3 and BMI2 | x86-64 | every kernel family | the main x86 tier |
| AVX-512 F, BW and VL | x86-64 | pixel metrics only | the CPU reports all three |

These tiers work on 8-bit lanes. The 10-bit library has its own NEON, SSE4.2
and AVX2 tiers on 16-bit lanes, covered in [The 10-bit
tiers](#the-10-bit-tiers).

Dot product and SVE live inside the NEON tier: `y264_cpu_tier()` answers NEON
on a Graviton3. SVE needs no dispatch sites of its own because its entries
overwrite the NEON ones in the pixel table and behind the NEON quant's call
sites. The SVE kernels are vector-length agnostic and exact at any length.
They're installed only at 256 bits or more. Graviton4's SVE2 is 128 bits wide,
and Graviton4 runs NEON.

### Which families each tier vectorizes

| family | NEON | SVE | SSE4.2 | AVX2 | AVX-512 |
|---|---|---|---|---|---|
| pixel metrics: SAD, SAD x4, SATD, SA8D, texture AC, variance, intra 4x4 search, intra 16x16 SATD, SSD | yes | part | yes | yes | part |
| motion compensation: luma quarter/half-pel, chroma bilinear, copy, average, weighted average, half-pel plane build | yes | | yes | yes | |
| transform, quant, scan: 4x4 and 8x8 forward and inverse, batched 4x4 grid, quant, dequant, zigzag | yes | forward quant | yes | yes | |
| intra prediction: 8x8, 16x16, chroma | yes | | yes | yes | |
| deblock: strength, luma edges, chroma horizontal edges | yes | | yes | yes | |

The partial tiers have these kernels:

| kernel | SVE | AVX-512 |
|---|---|---|
| SATD | 16x16, 16x8, 8x8 | 16x16, 16x8 |
| averaged SATD | 16x16, 16x8 | 16x16, 16x8 |
| SA8D | 16x16 | 16x16 |
| texture/AC | 16x16 | 16x16 |
| SATD x4 | 8x8 | 8x8 |
| batched SAD, three or four candidates | | 16x16, 16x8, 8x16, 8x8 |
| batched SAD, two candidates | | 16x16, 16x8, 8x16 |
| forward quant | yes | |

Because AVX-512 has no horizontal add, its Hadamard's last stage uses
`|x+y| + |x-y| = 2·max(|x|, |y|)`. The single 8x8 SATD, the 8x8 SAD pair and
motion compensation stay on AVX2. The AVX-512 MC file
(`src/dsp/x86/interp_avx512.c`) stays empty until an MC kernel beats AVX2 on
both AMD and Intel.

These stay C on every tier:

| code | note |
|---|---|
| SAD at 8x4, 4x8 and 4x4; `sad_quad` at 4x8 and 4x4 | |
| chroma DC transforms and their quant | 2x2, and 2x4 for 4:2:2 |
| quant and dequant with a custom quant matrix | the kernels take the flat matrix only |
| vertical chroma deblock edge, all 4:4:4 chroma deblocking | |
| intra 4x4 prediction; intra 8x8 horizontal, DC, diagonal-down-left and horizontal-up | the compiler vectorizes these well enough |
| chroma MC at width 2 or odd heights; intra chroma at 4:4:4 | |
| outside `src/dsp`: lowres downscale, lowres sub-pel planes, plane padding, mb-tree propagate | |
| entropy coders, trellis, RDOQ | not vectorizable in any useful sense |

### The 10-bit tiers

The 10-bit kernels build into the 10-bit library only and take the same
`YAH264_NO_ASM`, `Y264_ASM_OFF` and `Y264_SIMD_FORCE` escapes as 8 bits.

| | NEON | SSE4.2 and AVX2 |
|---|---|---|
| source | `src/dsp/arm/<family>_neon10.c` | `src/dsp/x86/<family>_sse4_10.c` and `<family>_avx2_10.c`, with shared 128-bit bodies in `<family>_x86_10.h` |
| build macro | `Y264_HAVE_NEON10` | `Y264_HAVE_SSE4_10`, `Y264_HAVE_AVX2_10` |
| tier it answers | `Y264_TIER_NEON` | `Y264_TIER_SSE4`, `Y264_TIER_AVX2` |
| build | inside the 10-bit library | one static library per tier with the 10-bit library's arguments plus the tier's `-m` flags; the tier libraries fold into `libyah264_10` |

Each NEON kernel takes its 8-bit twin's name, and `src/common/symbols10.h`
adds a `_10` suffix. That lets `arch.h` declare both depths with one set of
prototypes while `symbol_check.sh` keeps them apart. A translation unit sets
at most one of `Y264_HAVE_NEON` and `Y264_HAVE_NEON10`. The 8-bit build
preprocesses as if the 10-bit tier didn't exist.

| family | 10-bit NEON | stays C |
|---|---|---|
| pixel metrics | SAD and its batches, SATD at every shape and its average form, SA8D, both texture terms, variance, intra 4x4 search, intra 16x16 SATD, SSD and its per-4x4 maps | none |
| motion compensation | six-tap luma, plane fetch, weighted average, chroma at widths 8 and 4, the half-pel plane build (whole band and rows), the lowres half-pel rows, the fused bi-prediction forms | as on 8 bits |
| transform, quant, scan | every NEON dispatch site | as on 8 bits |
| deblock | strength, 16-line luma edges, 4:2:0 chroma edges, horizontal chroma edge | as on 8 bits |
| intra prediction | 16x16, 8-wide chroma, the routed 8x8 modes, the six-mode 8x8 form, the lowres four-mode builder | as on 8 bits |

The half-pel row kernel exists only at 10 bits, for a band too short for the
whole-band kernel. At 8 bits every band fits.

The wider samples change the lane arithmetic:

| kernels | lanes | how they stay in range |
|---|---|---|
| transforms | int32 | a 10-bit coefficient is an int32 |
| pixel, intra, deblock | int16 | widen where 1023 pushes a sum past int16, as in the intra plane modes; each kernel states its bounds |
| six-tap luma, whole-band half-pel | u16 | an offset makes each filter sum non-negative and comes off after the shift |
| chroma bilinear and its fused bi-prediction form | u16 | with weights that sum to 64, no sum of 10-bit samples reaches 65536 |
| lowres half-pel row | 8-bit scale | rounds each pair mean to the lowres planes' 8-bit scale and saturates at 255 |
| directional intra modes | | split the 16-bit table into low and high bytes, because a table lookup reaches 64 bytes |

Both libraries build the boundary-strength kernel from one source
(`src/dsp/arm/loopfilter_bs_neon.c`) because it reads no samples.

The x86 kernels take the 8-bit x86 names. Both depths therefore share the
text of `arch.h`'s x86 declarations and most dispatch sites. With no 10-bit
AVX-512 form, an AVX-512 machine runs AVX2.

At one thread on rented Intel and AMD machines, against x264's 10-bit build:

| yah264 10-bit | against x264 | result |
|---|---|---|
| AVX2 | with its assembly | less CPU time, and fewer instructions where the machine exposes the counters |
| C | its fair C build | less CPU time, and fewer instructions where the machine exposes the counters |
| SSE4.2 | with its assembly | ahead on most clips, within about 10% on the rest |

| family | 10-bit SSE4.2 and AVX2 | stays C |
|---|---|---|
| pixel metrics | every kernel the 8-bit x86 tiers have | none |
| motion compensation | six-tap luma, chroma at widths 8 and 4, plane fetch, average, weighted average, the half-pel plane build (whole band and rows), the lowres half-pel row, the fused 4:2:0 average and two-list chroma | as on 8 bits |
| transform, quant, scan | every x86 dispatch site | as on 8 bits |
| deblock | strength, 16-line luma edges, 4:2:0 chroma edges, horizontal chroma edge | as on 8 bits |
| intra prediction | 16x16, 8-wide chroma, the routed 8x8 modes and their edge, the six-mode 8x8 forms, the lowres four-mode builder | as on 8 bits |

A 16-wide row of 10-bit samples fills one AVX2 register. The AVX2 files use
the whole register for 16-wide blocks, pack 8-wide blocks two rows to a
register and run narrower blocks on the VEX-encoded 128-bit bodies. Bounds
follow the NEON tier's except here:

| kernel | x86 handling |
|---|---|
| SAD | with no 16-bit absolute-difference sum, it accumulates `abs(a - b)` in 16-bit lanes and widens once |
| SATD | the four magnitudes meeting in a lane are coefficients of one tile, whose energy bounds them to 32736; they add in int16 before one widening multiply-add |
| 8x8 Hadamard | because a coefficient reaches 65472, the row transform's last stage is 2 * max(\|x\|, \|y\|) |
| quant | forms `\|c\| * mf + f` in 32 bits and hands any block with a coefficient at or above 2^17 to the C loop |
| reconstruction | clips to 1023 through `packus_epi32` and a min, never the byte pack |

Both depths' x86 files include one header for the boundary-strength and
per-lane deblock bodies (`src/dsp/x86/loopfilter_x86_core.h`).

### Dispatch

Detection lives in `src/common/cpu.c`. It reads CPUID on x86-64 for SSE2
through AVX2, FMA3, BMI2, AVX-512 and VNNI. On aarch64 Linux it reads HWCAP
once, including the SVE vector length. The result is cached in one word and
read with an inlined acquire load.

Pixel metrics go through `y264_dsp`, a function-pointer table that
`y264_dsp_init` fills once in tier order, so a wider tier overwrites a
narrower one's entry. Everything else (MC, half-pel build, transforms, quant,
scan, intra prediction, deblock, SSD) checks `y264_asm_on(class)` or
`y264_cpu_tier()` at the call site. Batched entry points such as
`y264_sub_dct4_blocks` check once per 16-block grid. All kernel prototypes
are in `src/dsp/arch.h`.

### Build options

The options in `meson_options.txt` decide what's compiled in. The CPU decides
what runs.

| option | values | default | effect |
|---|---|---|---|
| `simd` | `auto`, `none`, `sse4`, `avx2`, `avx512` | `auto` | Highest tier compiled. `auto` builds every tier the architecture has kernels for, and `none` builds a pure-C binary. The x86 names are a cap: `-Dsimd=sse4` on an AVX2 box builds and runs SSE4.2 alone. aarch64 accepts only `auto` and `none`. |
| `avx512` | `true`, `false` | `true` | Compile the AVX-512 tier. `-Dsimd=avx512 -Davx512=false` is an error. |
| `sve` | `auto`, `enabled`, `disabled` | `auto` | Build the SVE tier. `auto` means aarch64 Linux with a compiler that has the SVE intrinsics. Never on macOS, since no Apple part has SVE. |

Each x86 tier's files (`src/dsp/x86/<family>_<tier>.c`) compile into their own
static library and fold into the 8-bit library as objects.

| tier | flags |
|---|---|
| SSE4.2 | `-msse4.2` |
| AVX2 | `-mavx2 -mfma -mbmi2` |
| AVX-512 | `-mavx512f -mavx512bw -mavx512vl` |

The SVE files (`src/dsp/arm/*_sve.c`) select SVE with a target pragma and
leave the dispatching files at baseline armv8-a. `scripts/hygiene_check.sh`
refuses inline assembly under `src/dsp/x86/` and any `-march=` in a meson
file.

### Runtime overrides

Only `-Ddev_knobs=true` builds read these, except `YAH264_NO_ASM`.

| variable | effect |
|---|---|
| `Y264_SIMD_FORCE` | Caps one binary at a lower tier. On x86-64 the values are `none`, `sse4`, `avx2` and `avx512`. On aarch64 they're `neon` (no SVE), `sve128` (dispatch as a 128-bit part would), `sve` and `sve2`. Names for the other architecture are inert. An unknown value changes nothing. A typo can't silently drop to C. |
| `YAH264_NO_ASM` | Any value forces the C reference everywhere. |
| `Y264_ASM_OFF` | Comma list of classes to turn off: `pixel`, `dct`, `quant`, `mc`, `hpel`, `pred`, `deblock`, `ssd`, `scan`, `all`. |

Tests walk one build through its tiers with `Y264_SIMD_FORCE`, and the output
must not change between tiers.

### checkasm

`tools/checkasm` runs every kernel against its C reference on random and
adversarial inputs. It calls kernels by symbol, not through the dispatcher,
so turning SIMD off can't make a test compare C with C. Outputs get a
poisoned border and inputs a guard page at both ends. A load or store sized
to the register instead of the block then faults.

```sh
./build/tools/checkasm/checkasm --list          # every group and the ISA it needs
./build/tools/checkasm/checkasm --isa avx2      # one tier (includes the tiers below it)
./build/tools/checkasm/checkasm --bench mc      # correctness, then timings for a family
```

`--isa` takes `neon`, `sve`, `sve2`, `sse4`, `avx2`, `avx512` or `all`. The
registration table is per architecture. One file per family (`metrics.c`,
`interp.c`, `transform.c`, `intrapred.c`, `loopfilter.c`) writes each group
body once and takes the kernel as an argument. checkasm runs in `make test`.

A kernel ships when it's bit-exact under checkasm, the SIMD build matches the
plain-C build on the board clips, and fast conformance and CI are green. The
`--bench` multiple is recorded but decides nothing. Encoder-level speed on
real hardware decides.

### Testing tiers the Mac can't run

| script | runs | covers |
|---|---|---|
| `scripts/x86-docker.sh` (image in `docker/x86/`) | linux/amd64 on an Apple Silicon Mac, under Rosetta and QEMU | tiers none, SSE4.2 and AVX2, with legs for `make test` with checkasm, fast conformance and an identity check against `none` on the board clips; a 10-bit build whose unit leg runs checkasm over the 10-bit tiers |
| `scripts/sve-docker.sh` (image in `docker/sve/`) | a native arm64 container, SVE legs under QEMU user mode | Neoverse V1 with SVE at 256 bits like Graviton3; Neoverse N2 with SVE2 at 128 bits like Graviton4; `max` at 512 bits; a native leg with no SVE as the negative control |

Neither emulator has AVX-512. The x86 script says so out loud so nobody
reads an emulated pass as AVX-512 coverage.

```sh
scripts/x86-docker.sh                       # Rosetta, all three tiers
scripts/x86-docker.sh --backend qemu        # the final gate
scripts/x86-docker.sh --build-image         # rebuild the image first
```

For real hardware, `scripts/cloud/` holds the rented-machine kit. Cloud runs
use spot instances under a price cap.

| script | does |
|---|---|
| `bootstrap.sh` | builds a bare instance into a measurable state and writes `bootstrap.ok` with the CPU name, flags, kernel and compiler |
| `campaign.sh` | reads that file and runs the boards at one pinned core and at full threads, per-tier checkasm timings, instruction counts, identity md5s and the AVX-512 against AVX2 A/B on one binary |
| `lib.sh` | what the other two share: the PASS/SKIP/FAIL stage table, the bucket location, when a clock may be read and the wall-time budget |

CI builds `-Dsimd=auto` and `-Dsimd=sse4` on Ubuntu on every push that touches
code, then runs checkasm per tier and checks tier identity. The macOS job is
manual.

## What we chose and why

| choice | why |
|---|---|
| Intrinsics, no assembly | Reviewable as C. One source compiles for SSE4.2 and AVX2 with different flags. Hand assembly may still win on a few kernels once timings point at them. |
| SSE4.2 as the x86 floor, AVX2 as the main tier | AVX2 pays only where the data is at least 16 lanes wide. Short rows leave half the register idle, so those kernels match SSE4.2. |
| AVX-512 built by default | On one binary with identical output, it takes 0.966x AVX2's one-thread CPU time on an AMD EPYC 9R14 and 0.964x on an Intel Xeon 8488C. |
| AVX-512 limited to pixel metrics | The pixel kernels win on both vendors. MC has no AVX-512 kernel yet. |
| SVE only at 256 bits or wider | At 128 bits it did no better than NEON. On Graviton3 it takes 0.976x NEON's CPU time on one binary. |
| Unaligned loads everywhere | Frame planes are plain `malloc` with a border offset. Nothing promises alignment. |
| Per-class `Y264_ASM_OFF` bits | A profile in a dev_knobs build can price one kernel family without a rebuild. |
| Tier cap at build time and run time | `-Dsimd=none` gives a truly pure-C binary. In a dev_knobs build, `Y264_SIMD_FORCE` lets one binary on one machine test every tier. |

One-thread ratios against x264 at medium on rented machines, at CRF 23. The
ranges span sunflower 1080p and crowd_run 4K.

| machine | tier | instructions | CPU time |
|---|---|--:|--:|
| AMD EPYC 9R14 | AVX2 | 0.83 to 0.88 | 0.86 to 0.96 |
| AMD EPYC 9R14 | AVX-512 | 0.78 to 0.82 | 0.84 to 0.93 |
| Intel Xeon 8488C | AVX2 | 0.83 to 0.88 | 0.88 to 0.99 |
| Intel Xeon 8488C | AVX-512 | 0.78 to 0.82 | 0.84 to 0.97 |
| AWS Graviton3 | SVE | 0.62 to 0.67 | 0.65 to 0.74 |

## What didn't work

| idea | why not |
|---|---|
| SME/SME2, 512-bit streaming vectors for transforms and SATD | 1.08x over NEON. Closed on every form. |
| SME/SME2 per block | Impossible: `smstart` plus `smstop` costs 9.6 ns against a 3.7 ns SATD 8x8. Closed on every form. |
| SME/SME2 whole-frame SATD batched through the ZA engine | 0.21x NEON. Closed on every form. |
| SVE at 128 bits | No better than NEON. |
| SVE batched 16-wide SAD | More instructions than NEON, because NEON's loads post-increment and SVE's don't. |
| Batched 4x4 inverse transform | The whole 4x4 inverse is 0.047% of an encode. All-zero and DC-only blocks already shortcut it. A coded macroblock runs 3.5 to 11.5 of its 16. |
| NEON intra 4x4 prediction builder | Slower than C by measurement. |
| Kernel for the lowres sub-pel planes | Measured null twice. The compiler already vectorizes the C. |
| Wider SAD x4 and the vertical chroma deblock edge | The data's shape doesn't fill a register, on x86 as on NEON. |
| Rounding average in the intra 8x8 edge filter on x86 | Wrong result. The filter needs a truncating halving add. |

## Open questions

- x86 threading at high core counts loses in thread coordination, not
  kernels. At default threads on 32-vCPU machines:

  | machine | wall time against x264 |
  |---|---|
  | Intel Xeon, 1080p | 4% behind |
  | Intel Xeon, 4K | 10% ahead |
  | AMD EPYC, 1080p | behind, despite fewer instructions retired |

- Several AVX2 kernels re-encode the SSE4.2 body and could use the whole
  register on 16-wide blocks.
- Would an AVX-512 form pay at 10 bits? x264's 10-bit build retires the same
  instructions with its AVX-512 paths as without them.

## Measuring it

- **Emulators check correctness only.** Wall time or instruction counts under
  Rosetta or QEMU measure the emulator. All x86 and SVE speed comes from
  rented machines.
- **Use Rosetta for routine Docker runs and QEMU for an item's final gate.**
  QEMU is several times slower. Emulated runs are real CPU load, so keep them
  away from timed runs.
- **The Docker kit leaves the VM's interpreter alone.** Because its QEMU legs
  name the image's own `qemu-x86_64` for each binary, one session's run can't
  change what another's containers run under. A lock keeps two runs in one
  worktree apart.
- **Compare the encoders on the same quiet machine.** Build x264 twice: with
  its assembly, and with assembly off but its C free to vectorize. The second
  is the fair match for `-Dsimd=none`.
- **Pin one-thread runs to a core and log the clock.** The cloud kit logs MHz
  beside each table and repeats a run that drifts more than 3%.
- **perf counters need permission.** Stock Linux ships
  `kernel.perf_event_paranoid` at 2 or higher, where `scripts/instr-ratio.sh`
  prints `NO-COUNTER`. Set it to 1 or use a privileged container.
- **gcc's ThreadSanitizer needs `vm.mmap_rnd_bits=28`** on recent Linux
  kernels. That's machine setup, not a code bug.
- **GitHub's runner CPUs vary,** and some have AVX-512. A CI job that relies
  on AVX-512 must check the features it got.
- `scripts/instr-ratio.sh N` gives the instruction ratio against x264 beside
  the wall ratio. For other tools, see the [instruments
  catalog](instruments.md).
