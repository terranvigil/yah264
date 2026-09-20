# yah264 on x86-64: SIMD parity, local validation, and the x264 cloud campaign

## Context

yah264 ships NEON intrinsics for 54 of its ~70 DSP entry points and nothing for
x86-64, while the plan of record promises SSE4.2 through AVX2 and calls AVX-512
first-class. The owner has no x86 machine. The goal is to do all the software work
and all the correctness validation locally (Docker on the M5 Max, free GitHub
runners), then rent x86 hardware for a short campaign that only measures: identity
on real silicon, AVX-512 correctness, and the x264 comparison. Every paid hour is
rehearsed in Docker first. yah264 goes first; the harness pieces are written so
yah265 reuses them.

Worktrees per item as in the parity programme (`../yah264-<id>`, branch
`item-<id>`), one gated merge each. Trailer `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
No hand assembly (CONTRIBUTING; hygiene_check enforces): x86 kernels are C11
intrinsics like the NEON ones.

## Decisions (owner, 2026-09-17)

- ISA tiers: SSE4.2 floor, AVX2 (+FMA3, BMI2) the shipped tier, AVX-512 a gated
  experiment (`-Davx512=false` default) for pixel metrics and MC only, decided
  after it is measured on both Intel and AMD.
- yah264 first; yah265 reuses the checkasm harness, the per-ISA build shape, the
  Docker kit and the cloud kit.
- GitHub Actions: the ubuntu job runs on push (free on a public repo,
  pass/fail only); the macOS job stays manual. **The runner model is not
  fixed.** It was an AMD EPYC 7763 when this was written and an Intel Xeon
  Platinum 8573C on the wave 2 run, and that second one reports avx512f,
  avx512bw, avx512vl and avxvnni. So the fleet can already execute the gated
  tier, and if a runner ever has to be relied on for AVX-512 the job must
  ASSERT the features it got rather than assume them -- the same rule the
  Docker kit follows in the other direction.
- Cloud provider: Google Cloud (c3 Sapphire Rapids + c3d Genoa), chosen after
  verifying Oracle, GCP, RunPod and AWS; table in "The cloud campaign".

## What the tree already has (facts the plan rests on)

- `src/common/cpu.c:16-61` `detect_x86()` is complete (SSE2..AVX2, FMA3, BMI2,
  AVX512F/BW/VL, VNNI, xgetbv checks), wired at `:148`, flag bits `cpu.h:16-30`,
  names `cpu.c:169-176`; nothing consumes them.
- The blocker: `y264_asm_on()` (`cpu.h:79-83`) tests `Y264_CPU_NEON` by name;
  `y264_pixel_init` (`pixel.c:465-503`) does the same at `:470`.
- Dispatch is two mechanisms: a function-pointer table for pixel metrics
  (`pixel.h:81-122`) and per-call `#if defined(__aarch64__) && Y264_BIT_DEPTH==8`
  + `y264_asm_on(CLASS)` blocks in mc.c, transform.c, predict.c,
  encoder/deblock.c and macroblock.c (SSD). NEON prototypes live in each
  dispatching .c.
- meson: `neon_sources` only on aarch64 (`meson.build:83-90`), linked into the
  8-bit library only (`:154-166`); no per-file c_args, no -march anywhere.
- Coverage: `docs/dsp-coverage-inventory.md:120-131` per family; the technique
  table and ranking `docs/asm-research-2026-09-02.md:52-105, :198-215`; the
  recorded rule that a checkasm multiple is necessary, not sufficient (`:203`,
  `:239-243`).
- checkasm: `tools/checkasm/checkasm.c`, 1747 lines, one main, 44 ad-hoc groups,
  no memory guards; several groups compare through the dispatcher. the sibling yah265's checkasm has the better shape: per-family files, a registration table,
  guard buffers, row-pad check, an mmap/mprotect page guard for over-reads
  (`checkasm.h:88-175`, `main.c:160-201`), and direct ref-vs-kernel comparison.
- Portability: every `__APPLE__`/aarch64 guard has a Linux arm; Metal is off by
  default; VideoToolbox has a stub; CI already builds ubuntu-latest.
- Harness: `scripts/perf-comp.sh` (fair x264 recipe `:72-84`: `x264-asm` =
  `configure --disable-lavf --disable-ffms --disable-avs --disable-swscale`;
  `x264-noasm-autovec` = `--disable-asm` then strip `-fno-tree-vectorize` from
  config.mak; `x264 --asm 0` on a stock build does not substitute), timing by
  `perf_counter` with medians and `PERF_REPEAT_FLOOR`; `scripts/ffboard.py`
  needs an ffmpeg with both libraries (the fork's `yah264` branch);
  `scripts/parity-clips.sh`; `scripts/bd_at_rate.py`; only
  `scripts/instr-ratio.sh` is macOS-only.
- Local emulation: Docker Desktop with Rosetta exposes sse4_2/avx2/fma/bmi2 (no
  AVX-512; known Illegal-instruction bugs); QEMU TCG does SSE4.2/AVX2/FMA (no
  AVX-512) and needs `QEMU_CPU=max`; Intel SDE runs on x86 hosts only (use it on
  the rented box). Verify first:
  `docker run --rm --platform linux/amd64 alpine grep -o 'avx[0-9_]*' /proc/cpuinfo`.

## Wave 0: readiness (local, no x86 kernels yet)

**W0a checkasm harness** (item `x86-checkasm`): split `tools/checkasm/checkasm.c`
into `tools/checkasm/{main.c,checkasm.h,pixel.c,mc.c,transform.c,predict.c,deblock.c}`
on yah265's shape: a registration table `{name, family, cpu_mask, fn}`, guard
buffers, row-pad check, the page guard (both tails), `--isa neon|sse4|avx2|avx512|all`,
`--list`, `--bench` kept (Apple QoS pin under `#ifdef __APPLE__`; Linux gets
`sched_setaffinity` via `CHECKASM_CPU`). Every test compares the C reference
against the kernel symbol directly, never through `y264_pixel_init` or
`y264_asm_on`. The 44 NEON groups re-register with `cpu_mask = Y264_CPU_NEON`
(or `|DOTPROD`). Gate: all groups pass on the M5 with page guards on; aarch64
output byte-identical (the parity clips md5) before and after; `make test`
unchanged.

**W0b dispatch generalisation** (item `x86-dispatch`): `Y264_CPU_SIMD_ANY` computed
in `cpu_detect_once` as the OR of compiled-in tiers masked by detection;
`y264_asm_on(cls)` becomes `(detect & SIMD_ANY) && !(asm_off & cls)`;
`YAH264_NO_ASM` and `Y264_ASM_OFF` keep their meaning; new `Y264_SIMD_FORCE=
none|sse4|avx2|avx512` clears higher bits so one binary runs per tier. New
`src/dsp/arch.h` holds every NEON prototype (moved out of the dispatching .c
files) and the x86 ones (`_sse4`, `_avx2`, `_avx512` suffixes) under
`#if defined(__x86_64__)`. `y264_pixel_init` overwrites in tier order after the
C fill (the DOTPROD override at `pixel.c:490-497` is the model); per-call sites
use a `y264_cpu_tier()` helper. Gate: aarch64 byte-identical; x86 C-only build
green in Docker.

**W0c meson** (same item as W0b): options `simd` (auto|none|sse4|avx2|avx512, a
build-time cap) and `avx512` (bool, default false). On x86_64 three
`static_library` objects: `src/dsp/x86/*_sse4.c` with `-msse4.2`, `*_avx2.c`
with `-mavx2 -mfma -mbmi2`, `*_avx512.c` with `-mavx512f -mavx512bw -mavx512vl`
only when `avx512`; linked into the 8-bit library only; per-family per-depth
opt-in lists with `-U/-DY264_SIMD_<FAMILY>` as yah265 does
(the sibling yah265's meson.build, per-family lists); `-DY264_HAVE_SSE4/AVX2/AVX512` project args
that `SIMD_ANY` reads; dispatching .c files stay baseline x86-64.
`hygiene_check.sh` also refuses `__asm__` under `src/dsp/x86/` and any `-march=`.

**W0d Docker kit** (item `x86-docker`): `docker/x86/Dockerfile` (ubuntu 24.04,
meson, ninja, gcc, clang, python3, ffmpeg, nasm, libvmaf, perf, git, curl) and
`scripts/x86-docker.sh` running `{rosetta, qemu QEMU_CPU=max}` x
`{Y264_SIMD_FORCE=none, sse4, avx2}` x `{make test incl. checkasm,
conformance.sh --fast, identity cmp}`. The identity cmp encodes the ten
parity-clips at their rates per tier and compares md5s to the `none` tier; the
10-bit build too (C only). The script asserts avx512 is absent under both
emulators so an emulated pass is never read as AVX-512 coverage; a Rosetta SIGILL
reruns that case under QEMU and marks it "emulator".

**W0e CI**: ubuntu job gains `-Dsimd=auto` build, `checkasm --isa avx2`, the
identity cmp on the synthetic conformance clips, a `-Dsimd=sse4` build +
checkasm; `push` trigger for the ubuntu job only, path-filtered to `src/**`,
`tools/**`, `meson*`, `scripts/conformance.sh`; timing steps stay informational.

**W0f** `scripts/instr-ratio.sh` gains a Linux branch on `perf stat -e
instructions,cycles,task-clock` with the same columns.

**The binfmt registry is VM-GLOBAL, so two sessions cannot run this kit at
once.** Selecting QEMU means disabling the Rosetta entry for the duration, and
that entry belongs to the Docker VM rather than to a container: a second
session starting its own run will restore or re-disable it underneath the
first, and the first session's already-running encoders change interpreter
mid-flight. Wave 2 saw exactly one failure from this -- a two-pass cell
producing a zero-byte file with exit status 0, at one tier, in a window when
another worktree's container was up -- and it passed on a re-run at both
tiers with the box to itself. So the coordination this needs is not only the
CPU share: check `docker ps` for ANY container before a run, not just your
own, and treat the whole kit as exclusive.

**Box rule for every Docker run (owner, 2026-09-17):** emulated x86 runs are
CPU load like any encode (QEMU TCG is several times slower than native, so a
conformance sweep can occupy the box for an hour). Every `x86-docker.sh`
invocation is coordinated the same way as the timed legs: announce it to the
other sessions, keep it inside the jobs x threads cap, never run it beside a
timed leg or a band, and prefer the Rosetta backend (near-native speed) for the
routine matrix with the QEMU leg reserved for the final gate of each item.

Wave 0 exit: aarch64 output byte-identical to today; x86 C-only build green in
Docker under both emulators and on GH ubuntu; checkasm ported with page guards;
hygiene green.

## Waves 1 to 3: kernels

Order from the inventory's share and the h2h ranking. Each family gets SSE4.2 and
AVX2 twins of every kernel the NEON twin covers, in
`src/dsp/x86/<family>_<tier>.c` (AVX2 files may include the SSE4 file's static
helpers for tails), registered in the checkasm table with the tier's cpu mask.

| wave | family | kernels | classes |
|---|---|---|---|
| 1 **shipped** | pixel | sad 16x16/16x8/8x16/8x8, sad_x4 (+8x4), satd 4x4/8x8/x4_8x8/16x16, sa8d 8x8/16x16, hadamard_ac 8x8, texture ac, var 16x16, intra4x4_x9, intra_satd_x3_16, SSD | PIXEL, SSD |
| 2 **shipped** | mc, hpel | luma qpel/hpel taps, chroma bilinear, pred_copy, pred_avg2, weighted average, hpel plane build | MC, HPEL |
| 3a | transform, quant, scan | sub_dct4/8, add_idct4/8, dc-only recon, quant/dequant 4x4+8x8, zigzag/RDOQ marshal | DCT, QUANT, SCAN |
| 3b | deblock, predict | deblock strength, luma v4/h4, chroma8 h; intra 4x4/8x8/16x16/chroma builders | DEBLOCK, PRED |

**Wave 1 shipped**: 24 kernels per tier in `src/dsp/x86/pixel_{sse4,avx2}.c`
over a shared `pixel_x86.h`, 30 new checkasm groups, dispatched by the
tier-ordered overwrite in `y264_pixel_init` and by `y264_cpu_tier()` at the SSD
site. checkasm's pixel groups were restructured so a group's body is written
once and takes its kernel as an argument, which is what puts the x86 twins
under the NEON rows' own adversarial fills and page guards. No multiple is
recorded: see the note above the inventory's new columns.

**Wave 2 shipped**: 9 kernels per tier in `src/dsp/x86/mc_{sse4,avx2}.c` over a
shared `mc_x86.h`, 12 new checkasm groups, dispatched by `y264_cpu_tier()` at
every mc and hpel call site. Two bodies are written once and instantiated per
tier by macro -- the 16-wide luma plane build, which takes its tier's three row
filters, and the dispatcher's own window/tile body, which takes the two kernel
names -- because only those differ and an indirect call per prediction block is
not free. The chroma twins are named by WIDTH rather than by block: there is no
x86 form of the fixed 8x8 kernel, since the 8-wide one is straight-line at
every even height. checkasm's mc groups moved onto wave 1's shape and gained
three checks in the process: a per-phase page guard on the luma window kernels
whose horizontal reach is a per-row argument (the tiers declare different
windows), page guards on pred_avg2 and chroma, and six spans rather than one on
the half-pel row groups. No multiple is recorded; see the note above the
inventory's columns.

Ship criterion per kernel: bit-exact to the C reference under checkasm with page
guards on Rosetta AND QEMU; identity cmp x86-SIMD vs x86-C on the ten board clips;
`conformance.sh --fast` green in Docker; GH ubuntu green. The checkasm multiple is
recorded in the coverage inventory beside the NEON figure and is not a ship
criterion; the encoder-level A/B on real x86 (perf-comp with a control column) is
session B's. AVX-512 (`*_avx512.c`) only for pixel and MC, only after their AVX2
twins ship; compile-only gate in Docker, first run on real silicon in session A.
Do not port what NEON refused for a recorded reason (chroma vertical deblock,
sad_x4 widening) unless x86 has a different idiom; note the decision in the
inventory. Three worktrees at a time, no timed legs anywhere in these waves.

## Wave 4: the cloud kit, rehearsed in Docker

`scripts/cloud/` runs identically in Docker (Rosetta AVX2) and on the instance;
`CLOUD=1` enables AVX-512 and SDE, and the bucket URL is the only other
difference.

- `scripts/cloud/bootstrap.sh`: apt deps (as the Dockerfile); clone yah264 at
  `YAH264_COMMIT`; fetch corpus + JM + openh264 + SDE tarballs from an in-region
  bucket and verify with the sha256 list from `scripts/fetch_corpus.sh`; build
  yah264 (release, both depths); build the two x264 arms by the fair recipe on
  Linux/gcc; build the ffmpeg fork with `--enable-libyah264 --enable-libx264`;
  run `fetch_jm.sh` and `fetch_openh264.sh`; then `make test`, `conformance.sh`,
  `checkasm --isa all` (AVX-512 on real silicon), and under SDE
  (`sde64 -spr`, `-gnr`) when `CLOUD=1`; write `bootstrap.ok` with
  `y264_cpu_name`.
- `scripts/cloud/campaign.sh`: `taskset -c 2` for t1 legs; `perf-comp.sh` at
  threads 1 and N (N = vCPU with SMT off) for yah264 vs both x264 arms;
  `ffboard.py` through the fork; `bd_at_rate.py`; `checkasm --bench --isa
  sse4|avx2|avx512`; the CPU-seconds column via the instr-ratio Linux twin;
  identity md5s per arm and per tier; an MHz drift log (`/proc/cpuinfo` every
  30 s); the AVX-512 vs AVX2 A/B on one binary via `Y264_SIMD_FORCE` with a
  `YAH264_NO_ASM=1` control column; results under `results/<sku>/<date>/`,
  tarred and pushed; a `WALL_BUDGET_H` guard that pushes partial results before
  the budget is exceeded.
- `scripts/cloud/estimate.md`: the section-6 table with the actual spend
  appended after each session.
- Corpus staging: one-time upload of the ~12 GB corpus, the JM/openh264/SDE
  tarballs and the sha256 list to a private bucket in the campaign region
  (ingress free).
- Rehearsal criterion: `bootstrap.sh && campaign.sh` runs unattended in a clean
  Docker container to a results tarball, with a local directory standing in for
  the bucket. Nothing is launched before that passes.

## The cloud campaign

Prices read 2026-09-17 (all per-second billing after a 1-minute minimum):

| provider | Intel AVX-512 box | AMD AVX-512 box | notes |
|---|---|---|---|
| **Google Cloud** (recommended) | c3-highcpu-8 Sapphire Rapids $0.34 on-demand / $0.11 spot; c3-highcpu-22 $0.94 / $0.31 | c3d-highcpu-16 Genoa $0.60 / $0.13; c3d-highcpu-30 $1.12 / $0.25 | fixed CPU model per family, SMT can be switched off (billed at full vCPU), egress $0.12/GB after 1 GiB, the $300 trial credit applies (its vCPU quota unconfirmed) |
| AWS | c7i.4xlarge $0.71 / $0.28 | c7a.4xlarge $0.82 / $0.34 | verified, 100 GB/month egress free, ~2x GCP's on-demand |
| Oracle | Standard3 Ice Lake only (no Sapphire Rapids), $0.74 for 32 vCPU | E5/E6 Genoa/Turin $0.61 for 32 vCPU + 64 GB, true dedicated cores | cheapest AMD by far, egress free, but the E5 rate is blog-derived and oracle.com blocks price checks; keep as the AMD-only extended-run option |
| RunPod | pooled, model not selectable | same | no AVX-512 or dedicated-core guarantee; out |
| Hetzner / DO / Vultr / Scaleway | model not pinned | | out for timing |

Choice: **Google Cloud**, both vendors, fixed models, spot for correctness and
the trial credit likely covering the whole programme. AWS is the fallback with
the same shape at about twice the price; Oracle E6 is the cheap AMD-only box if
a longer AMD-only series is ever wanted.

| session | boxes | mode | hours | $ (both) | purpose |
|---|---|---|---|---|---|
| A | c3-highcpu-8 + c3d-highcpu-16 | spot | ~4 each | ~$1.75 (on-demand ~$3.80) | bootstrap, checkasm incl. AVX-512 on real silicon, SDE, conformance, identity |
| B | c3-highcpu-22 + c3d-highcpu-16 | on-demand | up to 3x8 each | ~$37 ceiling | the board: perf-comp t1 (pinned) and tN vs both x264 arms, ffboard, bd_at_rate, AVX-512 vs AVX2 A/B |

The tN leg runs with SMT on at the SKU's full vCPU count, which is how users run
x264 too, and says so beside the table; if a physical-core reading is wanted,
c3d-highcpu-30 and c3-highcpu-44 with SMT off (15 and 22 cores) cost about
$72 for the same campaign. Never spot a timing series. B runs only after A is
clean on both vendors. Total programme ceiling on GCP: about $40 on-demand, and
$0 if the trial credit's quota admits these shapes (check before session A).

Hygiene: t1 legs pinned with `taskset` on an SMT-off box or an idle sibling
thread; tN as stated above; MHz drift logged beside every table and any run over 3% drift repeated;
corpus in-region; medians and `PERF_REPEAT_FLOOR` as perf-comp.sh does; results
pushed before the instance is terminated.

## Verification

Readiness gate before any paid hour: `scripts/x86-docker.sh` green under Rosetta
and QEMU for none, sse4 and avx2; GH ubuntu green; aarch64 byte-identical to
pre-wave-0; hygiene green; the cloud-kit rehearsal produces a tarball unattended.

Cloud exit: identity md5s clean on both real boxes for every tier including
AVX-512; `checkasm --isa all` clean on both vendors and under SDE; the board table
with both x264 arms and yah264 at t1 and t16 with the CPU-seconds column and MHz
logs; the goal legs against stock x264 at t1 and tN recorded with medians and
repeat counts; the AVX-512 decision made from both vendors' A/B.

Written back: `docs/what-shipped.md` section 6 (x86 subsection, per-tier kernel
counts, checkasm multiples); `docs/plan.md` phase 5 status; `site/results.md`
x86 rows (both vendors, both tiers, both x264 arms); `docs/dsp-coverage-inventory.md`
gains SSE4/AVX2/AVX512 columns; `scripts/cloud/estimate.md` gets the actual spend.
