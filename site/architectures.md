---
title: Architectures, yah264
description: The processors and instruction sets yah264 is tuned for today, the ones planned next, where the speed comes from.
---

# Architectures

yah264 is fast on Apple silicon today. On x86 it runs correctly, but we
haven't timed it there yet. This page explains what that means and what comes
next. It also says where an encoder's speed actually comes from, for a reader
who has never worked on a codec.

## Where the speed comes from

An encoder spends most of its time in a handful of small loops. They compare
blocks of pixels, filter them, transform them and scan the results. A normal
instruction handles one number. A SIMD instruction handles a whole row of them
in one go, so sixteen pixels fit in a register and a single add serves all
sixteen. SIMD stands for single instruction, multiple data.

Each processor family has its own SIMD instruction set, so we write each hot
loop several times. We call those versions kernels. A plain C version always
ships beside them and it's the reference: every kernel has to produce exactly
the same bytes as its C twin. A test program checks that on every build. It
also traps any read outside the memory the kernel said it would touch. At run
time the encoder picks the fastest kernel the processor supports.

How much does that buy? On a low-bitrate 1080p encode at one thread, SIMD
makes the whole encode 1.54x faster than plain C. Pixel comparison is where
most of that comes from.

| kernel family | share of a plain C encode |
|---|--:|
| pixel comparison (SAD, SATD, variance) | 32% |
| motion compensation | 9.5% |
| transforms | 4% |
| coefficient scans | 3% |
| deblocking | 3% |
| everything else with a kernel | 3% |

The other half of an encode is decisions. Which block size, which motion
vector, skip or code. SIMD can't help there. Threads can: each frame is split
into rows that run in parallel, with a frame pipeline on top. The
[threading](threading.md) page covers that.

## Supported today

| processor | instruction set | kernels | status |
|---|---|--:|---|
| Apple silicon, other 64-bit ARM | NEON | 53 of 67 entry points | shipped, measured |
| ARM with the dot-product extension: Apple M1 and later, Graviton 2 and later | NEON + dot product | 2 extra | shipped, measured |
| x86-64, anything since 2008 | SSE4.2 | 58 | shipped, checked, not timed |
| x86-64 with AVX2: Intel Haswell and later, AMD Zen and later | AVX2 | 58 | shipped, checked, not timed |
| x86-64 with AVX-512 | AVX-512 | 0 | build option, off by default |

Every number on the [results](results.md) page comes from the ARM tier. The
two dot-product kernels are the 16x16 sum of absolute differences and the
block variance. Each uses one instruction that multiplies sixteen byte pairs
and adds them up.

The x86 tiers pass every correctness check we have. Their output is
byte-identical to the C build on ten clips under two different emulators on
the Mac, and again on a real AMD machine in GitHub's test fleet. What they
don't have is a speed number, because timing under an emulator measures the
emulator. We'll rent one Intel and one AMD machine by the hour for a short
campaign. The results page gets its x86 rows from that.

## Planned

**AVX-512** is a build option and stays off by default. It doubles the
register width again over AVX2. On some Intel parts it also drops the clock
while in use, so a kernel can do less work and still take longer. We'll write
it for pixel comparison and motion compensation only. It goes on by default
only once it has won on both an Intel and an AMD machine.

**Other ARM extensions.** SVE and SVE2 are ARM's scalable vector sets. Apple
silicon doesn't have them. Graviton 3 and 4 do, at 256 bits. We haven't written
for them because no machine here can run them. SME on Apple M4 and later is
built for matrix maths and doesn't fit these loops.

**Kernels we chose not to write.** Fourteen of the 67 C entry points have no
SIMD twin on ARM. Most are small or rarely called. A few were tried and came
out slower than C, which happens when the loop is short and its data has to be
gathered from scattered addresses. Each refusal is recorded in the tree with
its numbers so nobody tries it twice.

## How ARM and x86 differ

Same problems, different instructions. A kernel written for one family rarely
maps one to one onto the other. These are the reasons.

**Register width.** NEON and SSE4.2 registers are the same width. AVX2
doubles it. AVX-512 doubles it again. Wider only helps when the block is wide
enough to fill the register. A 4x4 block fills a NEON register exactly and
leaves an AVX2 register half empty.

| tier | register width |
|---|--:|
| NEON, SSE4.2 | 128 bits |
| AVX2 | 256 bits |
| AVX-512 | 512 bits |

**Register count.** 64-bit ARM has 32 vector registers. SSE4.2 and AVX2 have
16. AVX-512 brings x86 up to 32. An 8x8 inverse transform wants about sixteen
live values at once, which fits on ARM and spills to memory on SSE4.2.

**Two halves that don't talk.** An AVX2 register is really two 128-bit halves.
Shuffles and packs work inside a half. Moving data across the seam costs an
extra instruction. NEON has one flat register and no seam. That's why the AVX2
twin of a kernel is never just the SSE4.2 twin at double width.

**Table lookups.** Both families can rearrange bytes by a table. NEON's
instruction reaches across 64 bytes in one go. The x86 one reaches 16. The
coefficient scans are byte permutations, so on x86 they take several lookups
merged together where NEON takes one.

**Multiply and add.** ARM's dot-product instruction multiplies sixteen byte
pairs and sums them into four lanes in one step. x86 has a byte multiply-add
that pairs neighbours instead, which suits filters. Neither is a superset of
the other.

**Alignment.** Both families load from any address now. Older x86 tiers wanted
16-byte alignment. yah264 makes no alignment promise about its frame buffers,
so every kernel uses unaligned loads on both.

## Why we write intrinsics instead of assembly

Our kernels are C that calls compiler intrinsics. An intrinsic is a function
that maps onto one SIMD instruction. The compiler picks the registers and
orders the instructions. x264 writes its kernels as hand assembly, where the
author chooses every register and every instruction order.

We went with intrinsics for three reasons. The code reads as C, so any
contributor can review it. One source compiles for SSE4.2 and AVX2 with
different flags, which halves the kernel count we maintain. And our
clean-room rule forbids copying x264's assembly, so writing our own from
scratch buys nothing over intrinsics until we've measured where the compiler
does badly.

Hand assembly could still win. I expect it will in a few places. The
compiler's instruction order is rarely the best one for a given core. It
sometimes spills a register a person would have kept. A loop that handles one
block per call can be unrolled by hand to do four. We'll take those gains in
the hottest kernels once x86 timing exists to point at them.

## What would make it faster still

In the order I'd expect them to pay:

1. **Do less work.** At matched output bytes we retire about 1.4x the
   instructions x264 does on low-bitrate 1080p. Most of the excess is the
   block-decision search. It's the biggest saving on the table. It isn't a
   SIMD change at all.
2. **Measure on x86.** Every kernel there was written blind. The first real
   timing will say which twins are slow and why.
3. **Fuse kernels.** Several hot paths call three kernels back to back on the
   same block. One call that keeps the block in registers between steps saves
   every load and store in between.
4. **Widen where the block is wide.** The AVX2 twins that only re-encode the
   SSE4.2 body could use the whole register on 16-wide blocks.
5. **AVX-512** for the two families whose blocks can fill it, if the clock
   penalty doesn't eat the gain.
6. **Hand assembly** for the few kernels where measurement shows the compiler
   losing.

## Checking a kernel yourself

The test program is `tools/checkasm`. It runs every kernel against its C twin
on adversarial inputs, with a poisoned border around every output and a guard
page at both ends of every input. Run it after any build.

```sh
./build/tools/checkasm/checkasm --list
./build/tools/checkasm/checkasm --isa avx2
```

On a Mac the x86 tiers run in Docker through Rosetta or QEMU with
[x86-docker.sh](https://github.com/terranvigil/yah264/blob/main/scripts/x86-docker.sh).
One run covers the C tier, SSE4.2 and AVX2, with the identity check on the
board clips.
