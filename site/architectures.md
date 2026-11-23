---
title: Architectures - yah264
description: Which processors and instruction sets yah264 is tuned for today, which come next, and where the speed comes from.
---

# Architectures

yah264 is at parity with x264 on Apple silicon. It also runs on x86-64 and on
ARM servers. We check it for correctness and time it there on rented machines.
This page lists what runs where. It also explains where an encoder's speed
comes from, for readers who have never worked on a codec.

## Where the speed comes from

An encoder spends most of its time in a handful of small loops that compare,
filter, transform and scan blocks of pixels. A normal instruction handles one
number. A SIMD instruction (single instruction, multiple data) handles a whole
row of them, so sixteen pixels fit in one register and one add serves all
sixteen.

Because each processor family has its own SIMD instruction set, we write each
hot loop several times. We call those versions kernels. A plain C version always
ships beside them as the reference. A kernel has to produce exactly the same
bytes as its C twin. A test program checks that on each build and also
traps any read outside the memory the kernel said it would touch. At run time the
encoder picks the fastest kernel the processor supports.

These terms get mixed up because they describe different levels:

| term | what it is | here |
|---|---|---|
| SIMD | one instruction doing the same arithmetic on many values | every kernel |
| instruction set | a processor family's SIMD instructions | NEON and SVE on ARM, SSE4.2, AVX2 and AVX-512 on x86 |
| intrinsics | C functions that each map onto one instruction, with the compiler choosing registers and order | how our kernels are written |
| hand assembly | the instructions written out directly, with the author choosing registers and order | how x264 writes its kernels |
| auto-vectorization | the compiler turning a plain C loop into SIMD on its own | fine on simple loops and unreliable on transforms. We don't count on it |

A kernel written for ARM won't run on x86. We write ours as intrinsics, for the
reasons [further down](#why-we-write-intrinsics-instead-of-assembly).

On a low-bitrate 1080p encode at one thread, SIMD makes the whole encode 1.54x
faster than plain C, mostly through pixel comparison:

| kernel family | share of a plain C encode |
|---|--:|
| pixel comparison (SAD, SATD, variance) | 32% |
| motion compensation | 9.5% |
| transforms | 4% |
| coefficient scans | 3% |
| deblocking | 3% |
| everything else with a kernel | 3% |

The other half of an encode is decisions: which block size, which motion
vector, skip or code. SIMD can't help there, but threads can. Each frame is
split into rows that run in parallel, with a frame pipeline on top, as the
[threading](threading.md) page explains.

## Supported today

| processor | instruction set | kernels | status |
|---|---|--:|---|
| Apple silicon, other 64-bit ARM | NEON | 53 of 67 entry points | shipped. At parity on Apple M5 Max and checked on AWS Graviton4 |
| ARM with SVE at 256 bits: AWS Graviton3 | SVE | 10, plus quant | shipped, checked and timed on a cloud machine |
| ARM with the dot-product extension: Apple M1 and later, Graviton2 and later | NEON + dot product | 3 extra | shipped, measured |
| x86-64, anything since 2008 | SSE4.2 | 58 | shipped, checked, not timed |
| x86-64 with AVX2, most chips from the last decade | AVX2 | 58 | shipped, checked, timed on a cloud machine |
| x86-64 with AVX-512 | AVX-512 | 18, pixel comparison only | shipped, on by default where the chip has it; timed on AMD and Intel |

The parity tables on the [results](results.md) page come from the ARM tier on
the Mac. The same page has separate cloud readings for arm64 and x86-64. The
three dot-product kernels are the 16x16 and 8x16 sums of absolute differences
and the block variance. Each uses one instruction that multiplies sixteen byte pairs
and adds them up.

The x86 tiers pass every correctness check we have. Their output is
byte-identical to the C build on ten clips under two emulators on the Mac, on a
real x86-64 machine in GitHub's test fleet, and on rented AMD and Intel
machines. Timing under an emulator measures the emulator, so x86 speed comes
from rented machines. The AMD and Intel rows ran the same binary, so their
instruction counts match:

| machine | one thread, CRF 23, against x264 | instructions | CPU time |
|---|---|--:|--:|
| AMD EPYC, x86-64 | AVX2 | 0.83 to 0.88 | 0.86 to 0.96 |
| AMD EPYC, x86-64 | AVX-512 | 0.78 to 0.82 | 0.84 to 0.93 |
| Intel Xeon, x86-64 | AVX2 | 0.83 to 0.88 | 0.88 to 0.99 |
| Intel Xeon, x86-64 | AVX-512 | 0.78 to 0.82 | 0.84 to 0.97 |
| AWS Graviton3, arm64 | SVE | 0.62 to 0.67 | 0.65 to 0.74 |

With every core busy on the Intel machine, we take 4% longer than x264 on a
1080p clip and 10% less time on a 4K one. Threading at high core counts is the
open x86 item. The results page has the details.

**AVX-512** is built into each x86-64 build by default and runs only on chips
that report it. `-Davx512=false` leaves it out. It doubles the register width
again over AVX2. Some chips also lower the clock while it's in use, so a kernel
can do less work and still take longer. The AVX-512 kernels so far cover pixel
comparison. On one binary they take about 2% to 4% less CPU time than AVX2 on
both an AMD EPYC and an Intel Xeon, at one thread and with all cores busy. Motion compensation and the transforms still run on AVX2.

## What the tiers leave out

**Other ARM extensions.** SVE and SVE2 are ARM's scalable vector sets. Apple
silicon doesn't have them. Our SVE kernels run on Graviton3, which has SVE at
256 bits. They turn on only at that width or wider because at 128 bits they do
no better than NEON. Graviton4 has SVE2 at 128 bits and runs the NEON code. SME on
Apple M4 and later is built for matrix math and doesn't fit these loops.

**Kernels we chose not to write.** Fourteen of the 67 C entry points have no
SIMD twin on ARM. Most are small or rarely called. A few were tried and came
out slower than C, which happens when the loop is short and its data has to be
gathered from scattered addresses. Each refusal is recorded in the tree with
its numbers so nobody tries it twice.

## How ARM and x86 differ

The problems are the same on both. Even so, a kernel written for one family
rarely maps one to one onto the other.

**Register width.** NEON and SSE4.2 registers are the same width, AVX2 doubles
it and AVX-512 doubles it again. Wider only helps when the block fills the
register: a 4x4 block fills a NEON register exactly and leaves an AVX2 register
half empty.

| tier | register width |
|---|--:|
| NEON, SSE4.2 | 128 bits |
| AVX2 | 256 bits |
| AVX-512 | 512 bits |

**Register count.** 64-bit ARM has 32 vector registers. SSE4.2 and AVX2 have
16, and AVX-512 brings x86 up to 32. An 8x8 inverse transform wants about sixteen
live values at once, which fits on ARM and spills to memory on SSE4.2.

**Two halves that don't talk.** An AVX2 register is really two 128-bit halves.
Shuffles and packs work inside a half. Moving data across the seam costs an
extra instruction. NEON registers have no seam. That's why a good AVX2 kernel is
rarely just the SSE4.2 one at double width, though many of ours still are (see below).

**Table lookups.** Both families can rearrange bytes by a table, but NEON's
instruction reaches across 64 bytes in one go and the x86 one reaches 16. The
coefficient scans are byte permutations, so on x86 they take several lookups
merged together where NEON takes one.

**Multiply and add.** ARM's dot-product instruction multiplies sixteen byte
pairs and sums them into four lanes in one step. x86 has a byte multiply-add
that pairs neighbors instead, which suits filters. Neither is a superset of
the other.

**Alignment.** Both families load from any address now, though older x86 tiers
wanted 16-byte alignment. Because yah264 makes no alignment promise about its
frame buffers, the kernels all use unaligned loads.

## Why we write intrinsics instead of assembly

Our kernels are C that calls compiler intrinsics. The compiler picks the
registers and orders the instructions. x264 writes hand assembly, where the
author chooses every register and every instruction order.

We chose intrinsics for three reasons:

- The code reads as C, so any contributor can review it.
- One source compiles for SSE4.2 and AVX2 with different flags, which halves the
  kernels we maintain.
- Our clean-room rule forbids copying x264's assembly, so writing our own
  assembly from scratch buys nothing until we've measured where the compiler
  does badly.

I expect hand assembly to win in a few places. The compiler's instruction order
is rarely the best one for a given core. It sometimes spills a register a
person would have kept. And a loop that handles one block per call can be
unrolled by hand to do four. The x86 timings will show which kernels are worth
it.

## What would make it faster still

In the order I'd expect them to pay:

1. Fix threading at high core counts on x86. That's where we still lose to
   x264, at 1080p. No kernel fixes it, because the loss is in how the threads
   coordinate.
2. Do less work. The block-decision search (motion search and mode decision)
   is still the largest share of our instructions. Saving there isn't a SIMD
   change at all.
3. Fuse kernels. Several hot paths call three kernels back to back on the same
   block. One call that keeps the block in registers saves every load and store
   in between.
4. Widen where the block is wide. The AVX2 twins that only re-encode the
   SSE4.2 body could use the whole register on 16-wide blocks.
5. More AVX-512, starting with motion compensation. The pixel kernels already
   win on both AMD and Intel.
6. Hand assembly for the few kernels where measurement shows the compiler
   losing.

## Checking a kernel yourself

The test program is `tools/checkasm`. It runs each kernel against its C twin
on adversarial inputs. It puts a poisoned border around each output and a guard
page at both ends of each input. Run it after any build.

```sh
./build/tools/checkasm/checkasm --list
./build/tools/checkasm/checkasm --isa avx2
```

On a Mac the x86 tiers run in Docker through Rosetta or QEMU with
[x86-docker.sh](https://github.com/terranvigil/yah264/blob/main/scripts/x86-docker.sh).
One run covers the C tier, SSE4.2 and AVX2, and runs the identity check on
the board clips. The SVE tier runs the same way under QEMU with
[sve-docker.sh](https://github.com/terranvigil/yah264/blob/main/scripts/sve-docker.sh).
