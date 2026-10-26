# SIMD

Every hot pixel loop in yah264 has a plain C reference and one or more SIMD
kernels beside it. The kernels are C11 intrinsics on every architecture, with
no hand-written assembly. At run time the encoder reads the CPU's feature word
once and picks the widest kernel the chip supports. Each kernel must produce
exactly the bytes its C twin produces, so changing tier never changes the
output at one thread.

The published page, [Architectures](https://terranvigil.github.io/yah264/architectures.html),
covers the same ground for a general reader: which processors are supported,
how many kernels each tier has and the speed readings against x264. This doc
is the developer's view: how the tiers are built, dispatched and tested.

## How it works

### Tiers

| tier | architecture | what it covers | runs when |
|---|---|---|---|
| NEON | aarch64 | every kernel family | always on aarch64 |
| NEON + dot product | aarch64 | SAD 16x16 and 8x16, 16x16 variance, SSD | the CPU reports dotprod (Apple M1 and later, Graviton 2 and later) |
| SVE | aarch64 Linux | SATD at 16x16, 16x8 and 8x8, averaged SATD at 16x16 and 16x8, SA8D and texture/AC at 16x16, SATD x4 at 8x8, and the forward quant | the CPU reports SVE with vectors of 256 bits or wider (Graviton3) |
| SSE4.2 | x86-64 | every kernel family | the x86 floor |
| AVX2 (with FMA3, BMI2) | x86-64 | every kernel family | the main x86 tier |
| AVX-512 (F, BW, VL) | x86-64 | pixel metrics only | the CPU reports all three |

The tiers are 8-bit only. The 10-bit library is built with no SIMD at all and
runs the C reference everywhere (see [formats.md](formats.md)).

SVE rides inside the NEON tier the way dot product does. `y264_cpu_tier()`
still answers NEON on a Graviton3, and the SVE entries overwrite the NEON
ones in the pixel table and behind the NEON quant's call sites, so no dispatch
site had to change. The SVE kernels are vector-length agnostic and exact at
any length, but they're installed only at 256 bits or more. Graviton4 has
SVE2 at 128 bits and runs the NEON code.

### Which families each tier vectorizes

| family | NEON | SVE | SSE4.2 | AVX2 | AVX-512 |
|---|---|---|---|---|---|
| pixel metrics: SAD, SAD x4, SATD, SA8D, Hadamard AC, texture AC, variance, intra 4x4 search, intra 16x16 SATD, SSD | yes | part | yes | yes | part |
| motion compensation: luma quarter/half-pel, chroma bilinear, copy, average, weighted average, half-pel plane build | yes | | yes | yes | |
| transform, quant, scan: 4x4 and 8x8 forward and inverse, batched 4x4 grid, quant, dequant, zigzag | yes | forward quant | yes | yes | |
| intra prediction: 8x8, 16x16, chroma | yes | | yes | yes | |
| deblock: strength, luma edges, chroma horizontal edges | yes | | yes | yes | |

The AVX-512 kernels are:

- texture/AC 16x16
- SATD and averaged SATD at 16x16 and 16x8, SA8D 16x16, and SATD x4 at 8x8
- batched SAD with three or four candidates at 16x16, 16x8, 8x16 and 8x8, and
  with two at all of those but 8x8

AVX-512 has no horizontal add, so the Hadamard's last stage uses
`|x+y| + |x-y| = 2·max(|x|, |y|)`. The single 8x8 SATD, the 8x8 SAD pair and
motion compensation stay on AVX2. `src/dsp/x86/interp_avx512.c` exists and is
empty, waiting for an MC kernel that measures faster on both vendors.

What stays C on every tier:

- SAD at 8x4, 4x8 and 4x4, and `sad_quad` at 4x8 and 4x4
- the chroma DC transforms (2x2, 2x4 for 4:2:2) and their quant
- quant and dequant with a custom quant matrix (the kernels take the flat
  matrix only)
- the vertical chroma deblock edge and all 4:4:4 chroma deblocking
- intra 4x4 prediction, and intra 8x8 horizontal, DC, diagonal-down-left and
  horizontal-up (the compiler vectorizes those well enough)
- chroma MC at width 2 or odd heights, and intra chroma at 4:4:4

Outside `src/dsp`, the lowres downscale, the lowres sub-pel planes, plane
padding, the mb-tree propagate and the entropy coders are C. The entropy
coders, trellis and RDOQ aren't vectorizable in any useful sense.

### Dispatch

Detection lives in `src/common/cpu.c`. On x86-64 it reads CPUID for SSE2
through AVX2, FMA3, BMI2, AVX-512 and VNNI. On aarch64 Linux it reads HWCAP,
including the SVE vector length, once. The result is cached in one word and
read with an inlined acquire load.

There are two dispatch mechanisms:

- **Pixel metrics** go through a function-pointer table, `y264_dsp`, filled
  once by `y264_dsp_init` in tier order. A wider tier overwrites a narrower
  one's entry.
- **Everything else** (MC, half-pel build, transforms, quant, scan, intra
  prediction, deblock, SSD) checks `y264_asm_on(class)` or `y264_cpu_tier()`
  at the call site. Hot per-block loops have batched entry points, such as
  `y264_sub_dct4_blocks`, so the check runs once per 16-block grid rather than
  once per block.

All kernel prototypes are in `src/dsp/arch.h`.

### Build options

The SIMD options in `meson_options.txt` decide what gets compiled into the
binary. The CPU decides what runs.

| option | values | default | effect |
|---|---|---|---|
| `simd` | `auto`, `none`, `sse4`, `avx2`, `avx512` | `auto` | Highest tier compiled. `auto` builds every tier the architecture has kernels for. `none` builds a pure-C binary. The x86 names are a cap, so `-Dsimd=sse4` on an AVX2 box builds and runs SSE4.2 alone. On aarch64 only `auto` and `none` are accepted. |
| `avx512` | `true`, `false` | `true` | Compile the AVX-512 tier. `-Dsimd=avx512 -Davx512=false` is an error. |
| `sve` | `auto`, `enabled`, `disabled` | `auto` | Build the SVE tier. `auto` means aarch64 Linux with a compiler that has the SVE intrinsics. Never on macOS, since no Apple part has SVE. |

Each x86 tier's files (`src/dsp/x86/<family>_<tier>.c`) compile into their own
static library with that tier's `-m` flags (`-msse4.2`; `-mavx2 -mfma
-mbmi2`; `-mavx512f -mavx512bw -mavx512vl`) and fold into the 8-bit library as
objects. The SVE files (`src/dsp/arm/*_sve.c`) select SVE with a target pragma,
so the dispatching files stay baseline armv8-a. `scripts/hygiene_check.sh`
refuses inline assembly under `src/dsp/x86/` and any `-march=` in a meson
file.

### Runtime overrides

Read only in a build configured with `-Ddev_knobs=true`, except `YAH264_NO_ASM`.

| variable | effect |
|---|---|
| `Y264_SIMD_FORCE` | Caps one binary at a lower tier: `none`, `sse4`, `avx2`, `avx512` on x86-64; `neon` (no SVE), `sve128` (dispatch as a 128-bit part would), `sve`, `sve2` on aarch64. Names for the other architecture are inert. An unknown value changes nothing, so a typo can't silently drop to C. |
| `YAH264_NO_ASM` | Any value forces the C reference everywhere. |
| `Y264_ASM_OFF` | Comma list of classes to turn off: `pixel`, `dct`, `quant`, `mc`, `hpel`, `pred`, `deblock`, `ssd`, `scan`, `all`. |

`Y264_SIMD_FORCE` is how one build tests every tier: the output must not move
across it.

### checkasm

`tools/checkasm` runs every kernel against its C reference on random and
adversarial inputs. It calls each kernel by symbol, not through the
dispatcher, so turning SIMD off can't make a test compare C against C. Every
output has a poisoned border, and every input has a guard page at both ends,
so a load or store sized to the register instead of the block faults.

```sh
./build/tools/checkasm/checkasm --list          # every group and the ISA it needs
./build/tools/checkasm/checkasm --isa avx2      # one tier (includes the tiers below it)
./build/tools/checkasm/checkasm --bench mc      # correctness, then timings for a family
```

`--isa` takes `neon`, `sve`, `sve2`, `sse4`, `avx2`, `avx512` or `all`. The
registration table is per architecture, so an x86 build and an arm64 build
list different groups. One file per family (`metrics.c`, `interp.c`,
`transform.c`, `intrapred.c`, `loopfilter.c`) writes each group body once and
takes the kernel as an argument. checkasm runs in `make test`.

A kernel ships when it's bit-exact under checkasm, the SIMD build's output
matches the plain-C build on the board clips, the fast conformance pass is
green, and CI is green. The `--bench` multiple is recorded but decides
nothing. Encoder-level speed on real hardware does.

### Testing tiers the Mac can't run

`scripts/x86-docker.sh` (image in `docker/x86/`) runs the x86 battery as
linux/amd64 on an Apple Silicon Mac. The matrix is {Rosetta, QEMU} x {none,
SSE4.2, AVX2} x {`make test` with checkasm, fast conformance, an identity
check against the `none` tier on the board clips}, plus a C-only 10-bit build.
Neither emulator has AVX-512, and the script asserts that out loud so an
emulated pass is never read as AVX-512 coverage.

```sh
scripts/x86-docker.sh                       # Rosetta, all three tiers
scripts/x86-docker.sh --backend qemu        # the final gate
scripts/x86-docker.sh --build-image         # rebuild the image first
```

`scripts/sve-docker.sh` (image in `docker/sve/`) runs a native arm64 container
and executes the SVE legs under QEMU user mode at three CPU models: Neoverse V1
(SVE at 256 bits, the Graviton3 shape), Neoverse N2 (SVE2 at 128 bits, the
Graviton4 shape) and `max` at 512 bits. The native leg detects no SVE and is
the negative control.

For real hardware, `scripts/cloud/` holds the rented-machine kit.
`bootstrap.sh` builds a bare instance into a measurable state and writes
`bootstrap.ok` with the CPU name, flags, kernel and compiler. `campaign.sh`
reads that file and runs the boards at one pinned core and at full threads,
per-tier checkasm timings, instruction counts, identity md5s and the AVX-512
against AVX2 A/B on one binary. `lib.sh` holds what both share: the
PASS/SKIP/FAIL stage table, the bucket location, when a clock may be read and
the wall-time budget. Cloud runs use spot instances under a price cap.

CI builds `-Dsimd=auto` and `-Dsimd=sse4` on Ubuntu on every push that touches
code, runs checkasm per tier and checks tier identity. The macOS job is manual.

## What we chose and why

| choice | why |
|---|---|
| Intrinsics, no assembly | Reviewable as C, and one source compiles for SSE4.2 and AVX2 with different flags. Hand assembly may still win on a few kernels once timings point at them. |
| SSE4.2 as the x86 floor, AVX2 as the main tier | AVX2 only pays where the data is at least 16 lanes wide. Short rows leave half the register idle, so those kernels match SSE4.2. |
| AVX-512 built by default | On one binary it takes 0.966x AVX2's one-thread CPU time on an AMD EPYC 9R14 and 0.964x on an Intel Xeon 8488C, with identical output. |
| AVX-512 limited to pixel metrics and MC | The pixel kernels win on both vendors. MC has no AVX-512 kernel yet. |
| SVE only at 256 bits or wider | At 128 bits it did no better than NEON. On Graviton3 it takes 0.976x NEON's CPU time on one binary. |
| Unaligned loads everywhere | Frame planes are plain `malloc` with a border offset, and nothing promises alignment. |
| Per-class `Y264_ASM_OFF` bits | Lets a profile price one kernel family without a rebuild. |
| Tier cap at build time and run time | `-Dsimd=none` gives a truly pure-C binary. `Y264_SIMD_FORCE` lets one binary on one machine test every tier. |

One-thread readings on rented machines, CRF 23, against x264 at medium:

| machine | tier | instructions | CPU time |
|---|---|--:|--:|
| AMD EPYC 9R14 | AVX2 | 0.83 to 0.88 | 0.86 to 0.96 |
| AMD EPYC 9R14 | AVX-512 | 0.78 to 0.82 | 0.84 to 0.93 |
| Intel Xeon 8488C | AVX2 | 0.83 to 0.88 | 0.88 to 0.99 |
| Intel Xeon 8488C | AVX-512 | 0.78 to 0.82 | 0.84 to 0.97 |
| AWS Graviton3 | SVE | 0.62 to 0.67 | 0.65 to 0.74 |

The ranges span sunflower 1080p and crowd_run 4K.

## What didn't work

| idea | why not |
|---|---|
| SME/SME2 for transforms and SATD | 512-bit streaming vectors buy 1.08x over NEON. `smstart` plus `smstop` costs 9.6 ns against a 3.7 ns SATD 8x8, so per-block use is impossible. Whole-frame SATD batched through the ZA engine reads 0.21x NEON. Closed on every form. |
| SVE at 128 bits | No better than NEON. |
| SVE batched 16-wide SAD | NEON's loads post-increment and SVE's don't, so it executes more instructions than NEON. |
| Batched 4x4 inverse transform | The whole 4x4 inverse is 0.047% of an encode. All-zero and DC-only blocks already shortcut it, and a coded macroblock runs 3.5 to 11.5 of them, not 16. |
| NEON intra 4x4 prediction builder | Slower than C by measurement. |
| Kernel for the lowres sub-pel planes | Measured null twice. The compiler already vectorizes the C. |
| Wider SAD x4 and the vertical chroma deblock edge | The data's shape doesn't fill a register. x86 leaves them out for the same reason as NEON. |
| Rounding average in the intra 8x8 edge filter on x86 | Wrong result. The filter needs a truncating halving add. |

## Open questions

- **x86 threading at high core counts.** At the default thread count on a
  32-vCPU Intel Xeon we're 4% behind x264 on wall time at 1080p and 10% ahead
  at 4K. On a 32-vCPU AMD EPYC we lose at 1080p while retiring fewer
  instructions. The loss is in thread coordination, not kernels.
- **AVX-512 motion compensation.** The MC file is empty. It needs a kernel that
  beats AVX2 on both AMD and Intel.
- **Wider AVX2.** Several AVX2 kernels re-encode the SSE4.2 body and could use
  the whole register on 16-wide blocks.
- **10-bit SIMD.** None exists. The 10-bit path's coding efficiency gap comes
  first (see [formats.md](formats.md)).

## Measuring it

- **Emulators check correctness only.** A wall time or instruction count under
  Rosetta or QEMU measures the emulator. All x86 and SVE speed comes from
  rented machines.
- **Use Rosetta for routine Docker runs and QEMU for an item's final gate.**
  QEMU is several times slower, and emulated runs are real CPU load, so don't
  run them beside a timed run.
- **One Docker kit session at a time.** Switching to QEMU changes the Docker
  VM's binary-format registry for every container, so a second session's run
  can swap the interpreter under the first one's encoders. Check `docker ps`
  first.
- **Compare the two encoders on the same quiet machine.** Build x264 twice:
  with its assembly, and with assembly off but its C free to vectorize. The
  second is the fair match for `-Dsimd=none`.
- **Pin one-thread runs to a core and log the clock.** The cloud kit logs
  MHz beside every table and repeats a run that drifts more than 3%.
- **perf needs to be readable.** A stock Linux kernel ships
  `kernel.perf_event_paranoid` at 2 or higher, and `scripts/instr-ratio.sh`
  then prints `NO-COUNTER`. Set it to 1 or use a privileged container.
- **gcc's ThreadSanitizer needs `vm.mmap_rnd_bits=28`** on recent Linux
  kernels. That's machine setup, not a code bug.
- **GitHub's runner CPUs vary,** and some have AVX-512. A CI job that relies
  on AVX-512 must check the features it actually got.
- `scripts/instr-ratio.sh N` gives the instruction ratio against x264 beside
  the wall ratio (see [instruments.md](instruments.md)).
