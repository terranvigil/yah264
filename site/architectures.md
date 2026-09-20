---
title: Architectures, yah264
description: The processors and instruction sets yah264 is tuned for today, the ones planned next, where the speed comes from.
---

# Architectures

This page says which processors yah264 runs fast on today, which are next, and
what the fast paths actually are. It is written for an engineer who has not
worked on codecs before.

## Where the speed comes from

A video encoder spends most of its time on a few small loops that compare
blocks of pixels, filter them, transform them and scan the results. A normal
processor instruction handles one number at a time. A SIMD instruction handles
a whole row of them at once, so sixteen pixels fit in one register and one add
serves all sixteen. SIMD stands for single instruction, multiple data.

Every processor family has its own SIMD instruction set. We write a separate
version of each hot loop for each one. We call these versions kernels. A plain
C version of every kernel also ships. The C version is the reference. Every
kernel has to produce exactly the same bytes as its C twin. A test program
checks that and also traps any read outside the memory the kernel declared. The
encoder picks the fastest kernel the running processor supports.

On a low-bitrate 1080p encode at one thread, the kernels with SIMD on retire
54% fewer instructions than plain C. That reads as 1.54x faster. Pixel
comparison is the largest share by far. The table gives the rest.

| kernel family | share of a plain C encode |
|---|--:|
| pixel comparison (SAD, SATD, variance) | 32% |
| motion compensation | 9.5% |
| transforms | 4% |
| coefficient scans | 3% |
| deblocking | 3% |
| everything else with a kernel | 3% |

The other half of the encode is decision making. Which block size, which
motion vector, skip or code. SIMD does not touch that half. Threads do. Each
frame is split into rows and rows run in parallel, with a frame-level pipeline
on top. The [threading](threading.md) page covers it.

## Supported today

| processor | instruction set | kernels | status |
|---|---|--:|---|
| Apple silicon, other 64-bit ARM | NEON | 53 of 67 entry points | shipped, measured |
| ARM with the dot-product extension: Apple M1 and later, Graviton 2 and later | NEON + dot product | 2 extra | shipped, measured |
| x86-64, any since 2008 | SSE4.2 | 58 | shipped, correctness checked, no timing yet |
| x86-64 with AVX2 (Intel Haswell and later, AMD Zen and later) | AVX2 | 58 | shipped, correctness checked, no timing yet |
| x86-64 with AVX-512 | AVX-512 | 0 | build option, off by default, nothing written |

The ARM tier is the one every number on the [results](results.md) page comes
from. The two dot-product kernels are the 16x16 sum of absolute differences
and the block variance. They use a single instruction that multiplies and adds
sixteen byte pairs at once.

The x86 tiers exist and pass every correctness check we have. That includes
byte-identical output against the C build on ten clips under two different
emulators on the Mac and on a real AMD machine in GitHub's test fleet. What
they do not have is a speed number. An emulator's timing measures the
emulator. Timing waits for real x86 hardware, which we rent by the hour for
one short campaign on one Intel and one AMD machine. Those rows go on the
results page when it runs.

## Planned

**AVX-512** is a build option that is off by default. It doubles the register
width again over AVX2. On some Intel parts it also lowers the clock speed
while in use, so a kernel can do less work and still take longer. We will write
it for the pixel and motion compensation families only. It goes on by default
only if it wins on both an Intel and an AMD machine.

**Other ARM extensions.** SVE and SVE2 are ARM's scalable vector sets. Apple
silicon does not have them. Graviton 3 and 4 do, at 256 bits. We have not
written for them because no machine here can run them. SME on Apple M4 and
later is built for matrix maths and does not fit these loops.

**Kernels we chose not to write.** Fourteen of the 67 C entry points have no
SIMD twin on ARM. Most are small or rarely called. A few were tried and
measured slower than the C version, which happens when the loop is short and
the data has to be gathered from scattered addresses. Those refusals are
recorded in the tree with the numbers so nobody tries them twice.

## How ARM and x86 differ

The two families solve the same problems with different instructions. A kernel
written for one rarely maps one to one onto the other.

**Register width.** NEON and SSE4.2 registers are the same width. AVX2
doubles it. AVX-512 doubles it again. Wider is better only when the block is
wide enough to fill the register. A 4x4 block fills a NEON register exactly.
An AVX2 register would be half empty.

| tier | register width |
|---|--:|
| NEON, SSE4.2 | 128 bits |
| AVX2 | 256 bits |
| AVX-512 | 512 bits |

**Register count.** 64-bit ARM has 32 vector registers. SSE4.2 and AVX2 have
16. AVX-512 brings x86 to 32. An 8x8 inverse transform in 128-bit registers
wants about sixteen live values, which fits on ARM and spills on SSE4.2.

**Lanes within lanes.** An AVX2 register is two 128-bit halves that do not
talk to each other cheaply. Shuffles and packs work within a half. Moving data
between halves costs an extra instruction. NEON has one flat register and no
such seam. This is why the AVX2 twin of a kernel is not simply the SSE4.2
twin at double width.

**Table lookups.** Both families have an instruction that rearranges bytes by
a table. NEON's reaches across 64 bytes in one go. The x86 one reaches 16
bytes. The coefficient scans are byte permutations, and on x86 they take
several lookups merged together where NEON takes one.

**Multiply and add.** ARM's dot-product instruction multiplies sixteen byte
pairs and sums them into four lanes in one step. x86 has a byte multiply-add
that pairs neighbours, which suits filters. Neither is a superset of the
other.

**Alignment and loads.** Both families load from any address today. Older
x86 tiers wanted 16-byte alignment. yah264 keeps no alignment guarantee on
its frame buffers, so every kernel uses unaligned loads on both.

## Why we write intrinsics instead of assembly

Our kernels are C code that calls compiler intrinsics. An intrinsic is a
function that maps onto one SIMD instruction. The compiler picks the registers
and orders the instructions. x264 writes its kernels as hand assembly. Its
author chooses every register and every instruction order.

We chose intrinsics for three reasons. The code reads as C and any contributor
can review it. One source compiles for SSE4.2 and AVX2 with different flags,
which halves the kernel count we maintain. And the project's clean-room rule
forbids copying any x264 assembly, so writing our own assembly from scratch
would buy nothing over intrinsics until we have measured where the compiler
does badly.

What hand assembly could still win is real. The compiler's instruction
schedule is rarely the best one for a given core. It sometimes spills a
register the author would have kept. A loop that does one block per call can
be unrolled by hand to do four. We expect those gains to be worth taking in a
few hot kernels once the x86 timing exists to point at them.

## What would make it faster still

In rough order of what we expect it to buy:

1. **Do less work.** At matched output bytes we retire about 1.4x the
   instructions x264 does on low-bitrate 1080p. Most of that excess is the
   block-decision search. It is the largest saving available. It is not a
   SIMD change.
2. **Measure on x86.** Every kernel there was written blind. The first real
   timing will show which twins are slow and why.
3. **Fuse kernels.** Several hot paths call three kernels back to back on the
   same block. One call that keeps the block in registers between steps saves
   the loads and stores in between.
4. **Widen where the block is wide.** The AVX2 twins that only re-encode the
   SSE4.2 body could use the whole register on 16-wide blocks.
5. **AVX-512** on the two families where the block width can use it, if the
   clock penalty does not eat the gain.
6. **Hand assembly** for the few kernels where measurement shows the compiler
   losing.

## Checking a kernel yourself

The test program is `tools/checkasm`. It compares every kernel against its C twin
on adversarial inputs, with a poisoned border around every output and a
guard page at both ends of every input. Run it after any build.

```sh
./build/tools/checkasm/checkasm --list
./build/tools/checkasm/checkasm --isa avx2
```

On a Mac the x86 tiers run in Docker through Rosetta or QEMU with
[x86-docker.sh](https://github.com/terranvigil/yah264/blob/main/scripts/x86-docker.sh).
One run covers the C tier, SSE4.2 and AVX2, with the identity check on the
board clips.
