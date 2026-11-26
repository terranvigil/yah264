---
title: Architectures - yah264
description: Which processors and instruction sets yah264 is tuned for today, which come next, and where the speed comes from.
---

# Architectures

yah264 is at parity with x264 on Apple silicon and also runs on x86-64 and ARM
servers, which we check and time on rented machines. This page lists what runs
where and where an encoder's speed comes from.

## Where the speed comes from

An encoder spends most of its time in a few small loops that compare, filter,
transform and scan blocks of pixels. One SIMD (single instruction, multiple data)
add can serve sixteen pixels.

Because each processor family has its own SIMD set, we write each hot loop
several times. These kernels ship beside a plain C reference and must match its
bytes exactly. A test program checks that on each build and traps reads outside
a kernel's declared memory. At run time the encoder picks the fastest kernel the
processor supports.

The terms describe different levels:

| term | what it is | here |
|---|---|---|
| SIMD | one instruction doing the same arithmetic on many values | every kernel |
| instruction set | a processor family's SIMD instructions | NEON and SVE on ARM, SSE4.2, AVX2 and AVX-512 on x86 |
| intrinsics | C functions that each map onto one instruction, with the compiler choosing registers and order | how our kernels are written |
| hand assembly | the instructions written out directly, with the author choosing registers and order | how x264 writes its kernels |
| auto-vectorization | the compiler turning a plain C loop into SIMD on its own | fine on simple loops and unreliable on transforms. We don't count on it |

ARM kernels don't run on x86. We write ours as intrinsics for the reasons
[further down](#why-we-write-intrinsics-instead-of-assembly).

On a low-bitrate 1080p encode at one thread, SIMD makes the whole encode 1.54x
faster than plain C, mostly in pixel comparison:

| kernel family | share of a plain C encode |
|---|--:|
| pixel comparison (SAD, SATD, variance) | 32% |
| motion compensation | 9.5% |
| transforms | 4% |
| coefficient scans | 3% |
| deblocking | 3% |
| everything else with a kernel | 3% |

The other half of an encode is decisions (block size, motion vector, skip or
code), where SIMD can't help but threads can. Rows of each frame run in parallel
under a frame pipeline, as the [threading](threading.md) page explains.

## Supported today

| processor | instruction set | kernels | status |
|---|---|--:|---|
| Apple silicon, other 64-bit ARM | NEON | 53 of 67 entry points | shipped. At parity on Apple M5 Max and checked on AWS Graviton4 |
| ARM with SVE at 256 bits: AWS Graviton3 | SVE | 10, plus quant | shipped, checked and timed on a cloud machine |
| ARM with the dot-product extension: Apple M1 and later, Graviton2 and later | NEON + dot product | 3 extra | shipped, measured |
| x86-64, anything since 2008 | SSE4.2 | 58 | shipped, checked, not timed |
| x86-64 with AVX2, most chips from the last decade | AVX2 | 58 | shipped, checked, timed on a cloud machine |
| x86-64 with AVX-512 | AVX-512 | 18, pixel comparison only | shipped, on by default where the chip has it; timed on AMD and Intel |

The [results](results.md) page's parity tables come from the ARM tier on the
Mac, with separate cloud readings for arm64 and x86-64. The three dot-product
kernels are the 16x16 and 8x16 sums of absolute differences and the block
variance.

The x86 tiers pass every correctness check we have. Their output matches the C
build byte for byte on ten clips under two emulators on the Mac, on a GitHub
x86-64 runner, and on rented AMD and Intel machines. Emulators can't time, so
x86 speed comes from the rented machines. AMD and Intel ran the same binary,
hence matching instruction counts:

| machine | one thread, CRF 23, against x264 | instructions | CPU time |
|---|---|--:|--:|
| AMD EPYC, x86-64 | AVX2 | 0.83 to 0.88 | 0.86 to 0.96 |
| AMD EPYC, x86-64 | AVX-512 | 0.78 to 0.82 | 0.84 to 0.93 |
| Intel Xeon, x86-64 | AVX2 | 0.83 to 0.88 | 0.88 to 0.99 |
| Intel Xeon, x86-64 | AVX-512 | 0.78 to 0.82 | 0.84 to 0.97 |
| AWS Graviton3, arm64 | SVE | 0.62 to 0.67 | 0.65 to 0.74 |

With every core busy on the Intel machine, we take 4% longer than x264 on a
1080p clip and 10% less time on a 4K one. Threading at high core counts is the
[open x86 item](results.md#by-processor).

**AVX-512** is in every x86-64 build (`-Davx512=false` leaves it out) and runs
only on chips that report it. Some chips lower the clock while it runs, which
can make a kernel slower despite doing less work. Ours cover pixel comparison
only and save about 2% to 4% CPU time over AVX2 on AMD EPYC and Intel Xeon, at
one thread and on all cores. Motion compensation and the transforms still run
on AVX2.

## What the tiers leave out

**Other ARM extensions.** Apple silicon lacks SVE and SVE2, ARM's scalable
vector sets. Our SVE kernels run on Graviton3's 256-bit SVE and turn on only at
that width or wider, since at 128 bits they do no better than NEON. Graviton4
has 128-bit SVE2 and runs NEON. SME on Apple M4 and later is for matrix math
and doesn't fit these loops.

**Kernels we chose not to write.** Fourteen of the 67 C entry points have no ARM
SIMD twin. Most are small or rarely called. A few came out slower than C, as
short loops over scattered data do. Each refusal is recorded in the tree with
its numbers.

## How ARM and x86 differ

**Register width.** Wider only helps when the block fills the register. A 4x4
block fills a NEON register exactly and leaves an AVX2 register half empty.

| tier | register width |
|---|--:|
| NEON, SSE4.2 | 128 bits |
| AVX2 | 256 bits |
| AVX-512 | 512 bits |

**Register count.** 64-bit ARM has 32 vector registers. SSE4.2 and AVX2 have
16, AVX-512 has 32. An 8x8 inverse transform wants about sixteen live values,
which fit on ARM and spill to memory on SSE4.2.

**Two halves that don't talk.** An AVX2 register is two 128-bit halves.
Shuffles and packs stay inside a half, and crossing the seam costs an
instruction. NEON has no seam. A good AVX2 kernel is rarely just SSE4.2 at
double width, though many of ours still are (see below).

**Table lookups.** NEON's byte-table lookup reaches 64 bytes, x86's 16. The
coefficient scans are byte permutations, so x86 merges several lookups where
NEON uses one.

**Multiply and add.** ARM's dot product multiplies sixteen byte pairs into four
lane sums in one step. x86's byte multiply-add pairs neighbors instead, which
suits filters. Neither is a superset.

**Alignment.** Both families load from any address, though older x86 tiers
wanted 16-byte alignment. Every kernel uses unaligned loads because yah264
promises no frame-buffer alignment.

## Why we write intrinsics instead of assembly

With intrinsics the compiler picks registers and instruction order. x264's hand
assembly leaves both to the author. We chose intrinsics because:

- The code reads as C, so any contributor can review it.
- One source compiles for both SSE4.2 and AVX2, halving the kernels we
  maintain.
- Our clean-room rule forbids copying x264's assembly. Writing our own buys
  nothing until we've measured where the compiler does badly.

I expect hand assembly to win in a few places: a better order for a given core,
a register the compiler spilled, a one-block loop unrolled to do four. The x86
timings point at the kernels where that's worth doing.

## What would make it faster still

In the order I'd expect them to pay:

1. Fix threading at high core counts on x86, where we still lose to x264 at
   1080p. The loss is in thread coordination, not kernels.
2. Do less work. The block-decision search (motion search and mode decision) is
   still the largest share of our instructions.
3. Fuse kernels. Several hot paths call three kernels back to back on one
   block. One call that keeps it in registers saves the loads and stores.
4. Widen the AVX2 twins that only re-encode the SSE4.2 body, to use the whole
   register on 16-wide blocks.
5. More AVX-512, starting with motion compensation, since the pixel kernels
   already win on AMD and Intel.
6. Hand assembly where measurement shows the compiler losing.

## Checking a kernel yourself

`tools/checkasm` runs each kernel against its C twin on adversarial inputs,
with a poisoned border around each output and a guard page at both ends of each
input. Run it after any build.

```sh
./build/tools/checkasm/checkasm --list
./build/tools/checkasm/checkasm --isa avx2
```

On a Mac,
[x86-docker.sh](https://github.com/terranvigil/yah264/blob/main/scripts/x86-docker.sh)
runs the C, SSE4.2 and AVX2 tiers in Docker through Rosetta or QEMU, including
the identity check on the board clips.
[sve-docker.sh](https://github.com/terranvigil/yah264/blob/main/scripts/sve-docker.sh)
does the same for SVE under QEMU.
