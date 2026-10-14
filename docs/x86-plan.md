# yah264 on x86-64

## Where it stands

The x86-64 build has SSE4.2 and AVX2 versions of every kernel family the NEON
build has: pixel metrics, motion compensation, transforms with quant and scan,
deblocking and intra prediction. They are C11 intrinsics, like the NEON
kernels, with no hand-written assembly.

Every kernel matches its C reference bit for bit, on two emulators and on a
rented AMD EPYC 9R14. On that machine the one-thread output is byte-identical
to the Mac's, and the conformance suite passes.

After the first tuning round, one thread with SIMD on the AMD machine reads:

| clip | instructions vs x264 | wall time vs x264 |
|---|--:|--:|
| sunflower 1080p, same bitrate | 0.91x | 1.02x |
| crowd_run 4K, CRF 23 | 0.88x | 0.89x |

A second tuning round is under way. So far only AMD has been timed. Intel chips
run the same x86 code but haven't been measured yet, and AVX-512 is a build
option with no kernels.

## Decisions

- SSE4.2 is the floor and AVX2, with FMA3 and BMI2, is the main tier. AVX-512
  is an opt-in experiment, limited to pixel metrics and motion compensation,
  and it stays or goes once it's measured on both Intel and AMD.
- Kernels are intrinsics only. The hygiene check refuses inline assembly under
  `src/dsp/x86/` and any `-march=` flag.
- yah264 goes first. yah265 reuses the checkasm harness, the per-tier build
  layout, the Docker kit and the cloud kit.
- CI runs the Ubuntu job on every push, pass or fail only. The macOS job stays
  manual. GitHub's runner CPU varies: some runners are AMD EPYC 7763 and some
  are Intel Xeon 8573C with AVX-512. A job that ever relies on AVX-512 must
  check the features it actually got.
- Cloud work uses AWS spot instances under a price cap, never on-demand. Speed
  comparisons run both encoders on the same quiet machine and are reported
  separately from the Mac's parity table.

## How the build is organized

- **Detection.** `src/common/cpu.c` detects SSE2 through AVX2, FMA3, BMI2,
  AVX-512 and VNNI.
- **Dispatch.** Pixel metrics go through a function-pointer table filled in
  tier order. Everything else checks `y264_cpu_tier()` at the call site. All
  kernel prototypes live in `src/dsp/arch.h`.
- **Build.** The meson option `simd` caps the tier at build time (`auto`,
  `none`, `sse4`, `avx2` or `avx512`), and `avx512` turns that tier on, off by
  default. Each tier's files, `src/dsp/x86/*_<tier>.c`, compile with that
  tier's flags and link into the 8-bit library only.
- **Runtime.** `Y264_SIMD_FORCE=none|sse4|avx2|avx512` limits one binary to a
  tier, so a single build tests every tier. `YAH264_NO_ASM` and `Y264_ASM_OFF`
  work as on arm64.

## Testing without x86 hardware

- **checkasm** has a file per kernel family and a registration table. It runs
  each kernel against its C reference directly, with guard buffers and page
  guards on both ends of every read. `--isa` picks the tier.
- **The Docker kit**, `scripts/x86-docker.sh`, runs Rosetta and QEMU, each at
  none, SSE4.2 and AVX2. Each run covers `make test`, a fast conformance pass
  and an output check on the ten board clips against the plain-C tier. It
  fails if either emulator reports AVX-512, so an emulated pass is never read
  as AVX-512 coverage.
- **Emulators check correctness only.** Timing under an emulator measures the
  emulator, so all x86 speed numbers come from rented machines.

Rules for the Docker kit:

- Emulated runs are real CPU load, and QEMU is several times slower than
  native. Take `/Volumes/T7/staging/yah264/x86opt/docker.lock` first and never
  run beside a timed run.
- Only one session can use the kit at a time. Switching to QEMU changes the
  Docker VM's binary-format registry for every container, so a second
  session's run can swap the interpreter under the first one's encoders. Check
  `docker ps` for any container, not just your own.
- Use Rosetta for routine runs, which is close to native speed, and QEMU for
  each item's final gate.

## Kernel waves

All four waves have shipped at SSE4.2 and AVX2.

| wave | family | kernels |
|---|---|---|
| 1 | pixel metrics | SAD, SAD x4, SATD, SA8D, Hadamard AC, texture AC, variance, intra 4x4 search, intra 16x16 SATD, SSD |
| 2 | motion compensation | luma quarter- and half-pel filters, chroma bilinear, copy and average, weighted average, half-pel plane build |
| 3a | transform, quant, scan | 4x4 and 8x8 forward and inverse transforms, batched 4x4 grid, quant and dequant, zigzag |
| 3b | deblock, intra prediction | deblock strength, luma and chroma edges, intra 8x8, 16x16 and chroma predictors |

What carried over from NEON and what didn't:

- Where NEON deliberately leaves a kernel out because of the data's shape,
  such as the vertical chroma deblock edge or a wider SAD x4, x86 leaves it out
  too unless x86 has a better idiom. The decision is noted in
  `docs/dsp-coverage-inventory.md`.
- AVX2 helps only where the data is at least sixteen lanes wide, such as a
  deblock pass over sixteen edges or a 16x16 prediction row. Short rows leave
  the upper half of a 256-bit register idle, so those kernels gain nothing
  over SSE4.2.
- The intra 8x8 reference filter needs a truncating halving add on x86, not
  the rounding average instruction.

A kernel ships when it is bit-exact under checkasm on both emulators, the x86
SIMD build's output matches the x86 plain-C build on the board clips, the fast
conformance pass is green in Docker, and CI is green. Its checkasm speedup is
recorded in the coverage inventory but doesn't decide anything. Encoder-level
speed on real hardware does.

## The first tuning round

The AMD validation run found the x86 build retiring 1.22 times x264's
instructions on sunflower 1080p and 1.37 times our own arm64 build's, with a
third of them in vector kernels. The first round worked on the families of
waves 3a and 3b:

- **Quant** runs in 16-bit lanes wherever the shift and rounding allow, with
  batched entry points so the per-block loops no longer call the dispatcher
  one block at a time.
- **Scan** gained a kernel for the 4x4 absolute zigzag, and the 8x8 scan mask
  narrows each level to one byte before shuffling.
- **Forward transforms** run columns first, which saves a transpose. AVX2
  takes two neighboring blocks per pass.
- **Intra 8x8** filters the edge once and builds all six directional modes from
  that one pass. The lookahead's four base predictions share it too.
- **Deblock** gained the whole-edge forms the NEON side has: a sixteen-line
  luma edge and a chroma edge covering both planes.

That brought instructions under x264's on both clips, as the table at the top
shows.

## Measuring on rented machines

- Both encoders run on the same quiet machine. x264 is built two ways: with
  its assembly, and with assembly off but its C free to vectorize, which is the
  fair match for our plain-C build.
- One-thread runs are pinned to a single core. The kit logs the clock speed
  beside every table and repeats any run that drifts more than 3%.
- Instruction counts come from `perf`. Results are copied off before the
  instance is shut down.
- Still to do: an Intel run, the AVX-512 decision from an Intel and AMD
  comparison, and the full goal board on x86.

## When this is done

Update `docs/what-shipped.md`, the phase 5 status in `docs/plan.md`, the x86
rows on the site's results page, and the SSE4.2, AVX2 and AVX-512 columns in
`docs/dsp-coverage-inventory.md`.
