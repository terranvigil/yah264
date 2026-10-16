# yah264 on x86-64 and on ARM servers with SVE

This plan covers the SIMD tiers the Mac can't run. Most of it is x86-64.
"The ARM SVE tier" covers SVE for AWS Graviton3, which was built with the same
machinery and is measured on rented machines the same way.

## Where it stands

The x86-64 build has SSE4.2 and AVX2 versions of every kernel family the NEON
build has: pixel metrics, motion compensation, transforms with quant and scan,
deblocking and intra prediction. They are C11 intrinsics, like the NEON
kernels, with no hand-written assembly. AVX-512 has its first kernels, pixel
metrics only, in a build that asks for them with `-Davx512=true`.

Every kernel matches its C reference bit for bit on two emulators and on
rented x86-64 machines. On those machines the one-thread output of every build
is byte-identical to the Mac's, pure C and AVX-512 included. The conformance
suite passes there too.

After two tuning rounds, a one-thread run on an AMD EPYC 9R14 (c7a.8xlarge) at
CRF 23 reads:

| | sunflower 1080p, 60 frames | crowd_run 4K, 20 frames |
|---|--:|--:|
| instructions vs x264, before the first round | 1.22x | 1.12x |
| after the first round | 0.91x | 0.88x |
| after the second round, default build | 0.88x | 0.83x |
| after the second round, AVX-512 build | 0.82x | 0.78x |
| CPU time vs x264, default build | 0.96x | 0.86x |
| CPU time vs x264, AVX-512 build | 0.93x | 0.84x |

At the same bitrate, ABR at 2855 kbit/s, sunflower reads 0.94x x264's
instructions and 1.02x its CPU time. crowd_run has no usable same-bitrate pair
because x264 overshot its target about 4.7 times.

Threading is the open item. At the default thread count on that 32-vCPU
machine, sunflower took longer than x264 and used more CPU time while retiring
fewer instructions. crowd_run came closer:

| default threads | wall, ours | wall, x264 | CPU ms, ours | CPU ms, x264 |
|---|--:|--:|--:|--:|
| sunflower | 0.76 s | 0.53 s | 5456 | 4641 |
| crowd_run | 1.17 s | 1.14 s | 9749 | 9728 |

Each is a single run that includes tool start-up. It shows the problem without
sizing it. The x86 goal board will size it.

## Decisions

- SSE4.2 is the floor and AVX2, with FMA3 and BMI2, is the main tier. AVX-512
  is opt-in (`-Davx512=true`) and limited to pixel metrics and motion
  compensation. Its first pixel kernels beat AVX2 on AMD by about 3% of
  one-thread time. It stays opt-in until it also wins on an Intel part,
  because AMD is the only x86 vendor we've measured.
- Kernels are intrinsics only. The hygiene check refuses inline assembly under
  `src/dsp/x86/` and any `-march=` flag.
- yah264 goes first. yah265 reuses the checkasm harness, the per-tier build
  layout, the Docker kit and the cloud kit.
- CI runs the Ubuntu job on every push, pass or fail only. The macOS job stays
  manual. GitHub's runner CPU model varies and some have AVX-512, so a job that
  ever relies on AVX-512 must check the features it actually got.
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
  `docker ps` for any container, including other sessions'.
- Use Rosetta, which runs close to native speed, for routine runs. Use QEMU for
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

The first validation run on a rented x86-64 machine found the x86 build retiring 1.22 times x264's
instructions on sunflower 1080p and 1.37 times our own arm64 build's, with a
third of them in vector kernels. The first round worked on the families of
waves 3a and 3b:

- **Quant** runs in 16-bit lanes wherever the shift and rounding allow, with
  batched entry points so the per-block loops no longer call the dispatcher
  one block at a time.
- **Scan** gained a kernel for the 4x4 absolute zigzag. The 8x8 scan mask now
  narrows each level to one byte before shuffling.
- **Forward transforms** run columns first, which saves a transpose. AVX2
  takes two neighboring blocks per pass.
- **Intra 8x8** filters the edge once and builds all six directional modes from
  that one pass. The lookahead's four base predictions share it too.
- **Deblock** gained the whole-edge forms the NEON side has: a sixteen-line
  luma edge and a chroma edge covering both planes.

That brought instructions under x264's on both clips, as the table at the top
shows.

## The second tuning round

The second round worked on the encoder's C, where the first had worked on
kernels, and wrote the first AVX-512 kernels. None of it changes output.

- **Trellis and entropy coding.** CAVLC lengths come from a nonzero mask and
  tables. 8x8 intra is priced in place from the quantizer's scan mask. A block
  whose only level is a single 1 has a closed form. RDOQ writes back only at
  nonzero positions. The 4x4 trellis reads its contexts in place, and the skip
  probe takes its operands from the scan kernels. Nonzero masks come from a
  vector compare in SSE2 or NEON, the baseline on each architecture. The
  lattice tracks its live nodes with a bitmask. The trellis and entropy rows of
  the x86 profile each fell by about a quarter. Since this is C, arm64 gains
  too. On the Mac it removes 2.0% of instructions on sunflower and 3.8% on a 4K
  clip.
- **Lookahead.** Only a per-frame cached weight, which is worth very little.
  What the lookahead still costs is search volume, and cutting that would change
  output. That makes it a quality trade, so this round left it alone.
- **AVX-512 kernels, pixel metrics only.** They cover texture/AC 16x16, SATD
  16x16 and 16x8, averaged SATD 16x16 and 16x8, SA8D 16x16, SATD x4 8x8, and
  the batched SADs with four or three candidates at 16x16, 16x8, 8x16 and 8x8,
  and with two at all of those but 8x8. AVX-512 has no horizontal add, so the
  Hadamard's last stage uses |x+y| + |x-y| = 2·max(|x|, |y|). The single 8x8 SATD, the
  8x8 SAD pair and motion compensation stay on AVX2.

The AVX-512 reading comes from one binary, the two tiers interleaved, seven
runs each on sunflower. At one thread AVX-512 takes 0.966x AVX2's CPU time and
wall time. At the default thread count it takes 0.982x the CPU time and 0.990x
the wall time. Those are AMD readings only. That's why the tier stays opt-in.

The machine found two defects. gcc's UBSan caught a misaligned double store in
the deblock row helper, now fixed. gcc's ThreadSanitizer needs
`vm.mmap_rnd_bits=28` on these kernels, which is box setup, not code.

## The ARM SVE tier

AWS Graviton3, a Neoverse V1 part, has SVE at 256 bits and no SVE2. x264 runs
its own SVE code there. We ran NEON only. Graviton4 has SVE2 at 128 bits and
Graviton2 has neither. Apple parts have no SVE, so the Mac never sees this
tier.

One thread on a c7g.8xlarge at CRF 23 reads:

| | sunflower 1080p, 60 frames | crowd_run 4K, 20 frames |
|---|--:|--:|
| instructions vs x264 | 0.67x | 0.62x |
| CPU time vs x264 | 0.74x | 0.65x |
| our instructions: main before the second round | 31.29 G | 59.93 G |
| with the second round's C, on NEON | 30.40 G | 56.60 G |
| with SVE | 28.01 G | 52.35 G |

On one binary, interleaved, SVE takes 0.976x NEON's CPU time on both clips.
x264 turns on its SVE code on this chip, which adds about 0.6% to its
instructions and no measurable time.

How the tier is built:

- **Detection.** Linux only, from HWCAP. The vector length is read once.
  `Y264_CPU_SVE256` marks vectors of 256 bits or more.
- **Dispatch.** The tier rides inside NEON the way DotProd does:
  `y264_cpu_tier()` still answers NEON, so no dispatch site moved. The SVE
  entries overwrite NEON ones in the pixel table and behind the NEON quant's
  call sites.
- **Build.** meson `-Dsve=auto|enabled|disabled`, where auto means aarch64
  Linux with the SVE intrinsics. The kernels live in `src/dsp/arm/*_sve.c` and
  select SVE with a target pragma, so no `-march=` reaches a meson file and
  the dispatching files stay baseline armv8-a.
- **Runtime.** `Y264_SIMD_FORCE` gains `neon` (no SVE), `sve128` (dispatched as
  a 128-bit part would be), `sve` and `sve2`. checkasm gains `--isa sve` and
  `--isa sve2`.

The pixel kernels are SATD at 16x16, 16x8 and 8x8, averaged SATD at 16x16 and
16x8, SA8D 16x16, texture/AC 16x16 and SATD x4 8x8. The forward quant has SVE
forms for 8x8, for a batch of 4x4 blocks and for a single 4x4. Every one is
vector-length agnostic and exact at any length. What they buy is width, so
they're installed only at 256 bits or more. At 128 bits they did no better than
NEON. For the same reason Graviton4 runs the NEON code unchanged.

We refused the batched 16-wide SAD. NEON's loads post-increment and SVE's
don't, so the SVE form executes more instructions than NEON.

No Mac can run SVE, so `scripts/sve-docker.sh` (image in `docker/sve/`) runs the
tier in an arm64 container with the SVE legs under QEMU at 128, 256 and 512
bits. On the Graviton3 itself, 24 identity arms are byte-identical and checkasm
passes at 256 and at 128 bits on the same chip. The same lane fixed an unaligned
four-byte load in the NEON 4x4 SATD that gcc's UBSan flagged.

## Measuring on rented machines

- Both encoders run on the same quiet machine. x264 is built two ways: with
  its assembly, and with assembly off but its C free to vectorize, which is the
  fair match for our plain-C build.
- One-thread runs are pinned to a single core. The kit logs the clock speed
  beside every table and repeats any run that drifts more than 3%.
- Instruction counts come from `perf`. Results are copied off before the
  instance is shut down.
- Still to do:
  - AVX-512 on an Intel part. It decides whether the tier stays opt-in.
  - Threading at high core counts. On the 32-vCPU AMD machine we lose to x264
    at the default thread count while retiring fewer instructions. This is the
    open x86 item.
  - The x86 goal board, G1, G2 and G3. The first pass is planned. It measures
    the threading gap properly.

## When this is done

Update `docs/what-shipped.md`, the phase 5 status in `docs/plan.md`, the x86
rows on the site's results page, and the SSE4.2, AVX2 and AVX-512 columns in
`docs/dsp-coverage-inventory.md`.
