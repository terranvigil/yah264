# Contributing to yah264

## Clean-room policy

yah264 is an independent implementation. It is not derived from x264, x265, or
any other GPL-licensed encoder, and it must stay that way so the project can
ship under BSD-2-Clause.

Rules:

1. Do not copy code from x264, x265, or any other encoder into this repository,
   in any form, including transliteration into another style or language.
2. Do not port the internal architecture of another encoder file by file, and do
   not reproduce its expression: its statement order, its decomposition into
   functions, its variable naming, or its file structure. Renaming as you go does
   not change this. If your version would read as a diff against theirs, it is a
   derivative work whatever language it is in.
3. The line, stated plainly:

   | allowed | not allowed |
   |---|---|
   | observing behaviour from the outside: output, bitrate, timings, decisions | reading a source file and reproducing what it says |
   | reading published papers and algorithm descriptions | transcribing an implementation, in any language or style |
   | building and measuring another encoder as a baseline | carrying its structure, naming or line order across |
   | implementing a technique the literature describes | copying the code the of the implementation  |

4. What you may use freely: the H.264/AVC specification (ITU-T H.264), published
   papers, the JM reference software's behavior as a conformance oracle (run it,
   compare output; do not copy it), public documentation, and your own knowledge
   of how video coding works.

If you are unsure whether something crosses the line, ask in a PR before writing.

## Engineering rules

- Language: C11 for the library, SIMD intrinsics (NEON today) for kernels, Python
  for tooling. Public headers stay C99-compatible. There is no assembly in the
  tree and none is assumed: `scripts/hygiene_check.sh` fails on any `.S` or
  `.asm` until someone has said where it came from and who will maintain it
  (`ASM_OK=1`). Hand-written asm is a conversation, not a default.
- Every DSP kernel ships with a C reference and a checkasm test that validates the
  optimized path against the reference and benchmarks it. No kernel merges without
  checkasm coverage.
- Encoded output must be bit-exact reproducible across runs, and threaded output
  must equal serial output at the same settings. Single-thread output may differ
  from t2+ where a flip-first trade disengages at `--threads 1`; the conformance
  script pins that case explicitly rather than pretending it does not exist.
- Every encode the conformance gate produces is decoded by an independent decoder
  (FFmpeg) and checked. Conformance is not optional, but it is not automatic
  either: `.github/workflows/ci.yml` is `workflow_dispatch` only, deliberately,
  so run `make conformance` locally before you open a PR and fire the workflow
  when a change wants a second platform.
- Match the surrounding code style. No trailing whitespace. Tabs are not used;
  indent with four spaces.
