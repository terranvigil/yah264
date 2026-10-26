<!-- Generated from the shared review rules; edit the shared copy, not this file. -->

# Code review instructions — yah264

Codebase: a from-scratch H.264/AVC encoder. C11 library (public header
`include/yah264.h`, C99-compatible), SIMD intrinsics for kernels (NEON, and
x86 tiers), no assembly, Python and bash tooling under `scripts/`, meson build.
Public API is `yah264_` / `YAH264_`, internals `y264_`, development env knobs
`Y264_*`. Layout: `cli/` the CLI, `src/encoder/` analysis, rate control and
entropy coding, `src/dsp/` kernels (C plus `_neon`, `x86/`), `src/common/`
bitstream, threadpool, CPU detection and the env helper (`y264_env()` in
`src/common/env.h`; a release build reads only the knobs the CLI carries),
`src/hw/` the hardware mode, `tests/` unit tests and checkasm. Gates: `make
test` (unit tests, symbol check, knob census, regress smoke), checkasm,
`scripts/conformance.sh` (recon match against an independent decoder), byte
identity at `--threads 1`, `scripts/hygiene_check.sh`.

Flag these; stay quiet on the rest. Sections are ordered by how often each class
of defect turned up in the project's fix commits and reviews.

---

## 1. Concurrency and determinism (the most frequent class by far)

A race in an encoder is a wrong stream on someone's machine, and the ones found
here were found late, by TSan or by a byte diff, never by the author.

- **Lazy statics read from worker threads.** An `if (!ready) init()` table, a
  cached env read or a CPU-feature cache resolved on first use races when the
  first use is on a worker. Resolve it at encoder open (the warm-up function)
  or with `pthread_once`. A warm path does read-compare-write, never a plain
  store that concurrent opens race on. Flag every new lazy static on a path a
  worker can reach.
- **Publish after fill.** A flag, counter or pointer that another thread polls
  must be set after the data it guards is complete, with release/acquire
  ordering. Flag a publish that comes first.
- **Shared state through the context.** A worker must not read a field another
  worker is still writing (a co-located MV grid, a ring head, a batch). Pass the
  captured state as a parameter, or fall back to the serial form above one
  thread.
- **Thread-count-dependent decisions.** A median over a ring whose length
  follows the thread count, or an estimate warmed from two different origins,
  makes output depend on scheduling.
- **`--threads 1` output is byte-identical across runs.** That is a hard gate.
  Above one thread identity isn't required, but a new diff there is a lead on a
  race or an uninitialised read and worth reporting.
- **Report latent races even when no current caller reaches them**, including
  ones that predate the PR. A lock or a warm-up that is provably unnecessary
  today is still wanted.

## 2. Stale text and published claims

- Help text, comments and docs must match the code: a `--help` default that
  isn't the default, "DEFAULT OFF" on a knob that defaults on, a count of
  knobs or scripts that is out of date. Counts should come from the tool that
  generates them, not be typed.
- A published number must be the one the script measures, at its real settings.
  Flag a flattered figure, a table pasted twice, or transposed columns.
- Public docs, the site and the README state the current status, briefly: no
  history, no before/after, no rejected variants, no per-clip lists, no dates,
  no commit IDs. Detail goes to the local, untracked notes.
- American spelling. Plain words. "Metrics", not "legs".

## 3. Measurement integrity: the arm isn't the arm it claims

An A/B that silently compares the same thing twice is the most expensive bug in
the harness, because it produces a confident wrong decision.

- A cache keyed on the command string, or on one binary when the result also
  depends on a library, serves stale encodes across a rebuild. Key on what the
  result depends on, and make hits visible.
- A knob the binary ignores (a release build, a config that can't engage the
  path) must make the harness refuse, not run.
- A switch that applies to one encoder of a pair (pure C on ours, SIMD on the
  reference) skews the comparison.
- Args built in a shell scalar and expanded bare don't word-split in zsh. Check
  that the args reached the binary when two arms come out identical.
- Flag claims not backed by evidence. A BD figure needs both arms, the same
  operating points, and a stated noise floor; a change inside the null spread
  is no change. Ask for the numbers.
- Flag thresholds and constants fitted to a handful of clips with no stated
  basis.

## 4. Gates and harnesses that pass on nothing

- A gate that finds zero cases, a missing binary, a broken symlink or an empty
  corpus must fail, not report "passed". Compare the case count with the
  expected count.
- Check every subprocess's exit code, and floor the output size where an empty
  file would read as a result.
- An aggregator must read every summary, not the last one.
- A parser must be updated with the log format it parses; one that falls back
  and passes everything is worse than none.
- A determinism gate run on an idle box proves little; the threaded gates need
  load.
- Scripts have no side effects at import (no writes before argument parsing),
  clean their scratch on EXIT/INT/TERM, and don't wait on `pgrep -f` patterns
  that match themselves.

## 5. Codec legality that a recon match can't see

A stream can decode to the encoder's own reconstruction and still be illegal.

- Ordering and numbering: NAL order, FrameNum gaps, reference marking and
  eviction by the wrapped value, POC.
- Ranges and limits: QP-delta range, MV range per level, level limits (MaxBR,
  MaxDpb), entropy-coder capacity, prefix lengths.
- Profile and flag consistency: a tool signalled that the profile forbids.
- Cite the spec clause in the PR, and add a conformance cell that reaches the
  case (a wrap needs a clip long enough to wrap).

## 6. Comments and clean-room wording

Full rules: `.claude/skills/writing-comments-in-code/SKILL.md`. Read it before
commenting on any comment. Summary: a comment earns its place only if it is
quicker to read than the code it labels, or states something the code cannot.
Default is no comment; severe trimming is correct.

- **Never name another encoder's internals** (function, variable, struct field,
  file:line) in code, comments, commit messages, docs or PR text. Describe the
  behavior; at most use its public option names. Naming the comparison encoder
  itself is fine. This is a hard rule; flag every instance.
- **Long comment blocks are a defect.** If the rationale is long, it belongs in
  the subsystem's doc, linked in one line.
- **No PR notes or history in code**: "measured at +4.7%", "changed from X to
  Y", a date, a sweep's results, a review round. Those go in the commit message
  or the PR.
- **No test-clip names in shared code** ("needed for sintel"). They read as a
  constraint, go stale silently, and usually mean a per-clip value is
  hardcoded. Describe the content condition instead ("a letterboxed night
  scene").
- **Delete comments that restate the code**, and dead comments describing
  removed behavior or a default that changed.
- Doc goes on the definition, once.
- A comment explaining a badly named variable is a naming bug.

These apply to lines a PR adds or edits. The tree carries older comments that
break them; don't flag those unless the PR touches them.

## 7. Error paths and memory

- Check every result: `fopen`, `malloc`, a grow, an append. A failure must
  report or fall back (to serial, to a smaller buffer), never segfault or
  orphan a ring entry.
- No silent truncation: a dropped slice, a truncated sample, a parse failure
  defaulting quietly.
- Free on every path, including the OOM exit and pool teardown.
- Compare a count before the grow that changes it, not after.

## 8. Uninitialised state and buffer sizing

- State that feeds a decision must be fully written on every path. A struct
  left half-written by an early return, or a `malloc` where `calloc` was
  needed, seeds the search with garbage. Recon match, TSan and ASan are all
  blind to this class; a `--threads 1` repeat on a varied config catches it.
- Size buffers by chroma format and `sizeof(pixel)`, not for 4:2:0 at 8 bits,
  and for one-macroblock-row frames as well as typical ones. Chroma reads are
  guarded by the chroma border.
- Offsets that can go negative are `ptrdiff_t`, not unsigned. Don't left-shift
  a negative value; multiply. Clamp a ring index before use.

## 9. Portability

The tree builds on gcc and clang, Linux and macOS, arm64 and x86-64.

- Flag clang-only or Mac-only constructs, `_Atomic` uses gcc rejects, shell that
  assumes GNU or BSD tools without a fallback (`sha256sum`), and globs that
  catch more archives than meant.
- Values written to a sourced key=value file are shell-quoted.

## 10. Public API and shared state

- Extend the public API with functions, not new fields in a public struct that
  prebuilt callers allocate; growing it broke callers before.
- No process-wide globals for per-encoder state; two encoders in one process
  must not share a cap, a calibration or a cache.

## 11. Ownership and placement

- The CLI maps flags to parameters and does I/O. It holds no encoding decision.
- Kernels live with a C reference, and every optimized kernel has a test that
  compares it with C byte for byte (checkasm, where the repo has it).
- Environment reads go through the env helper (see the layout above), never a
  bare `getenv`; a new knob is registered with the knob census in the same PR.
- Codec-wide code must not carry a per-clip value. A behavior that must differ
  by content gets a parameter or a detected condition.
- A new extern gets its high-bit-depth symbol rename in the same PR.

## 12. Naming

Names follow Go's rule: the length of a name follows its scope.

- A loop counter is `i`, a short-lived local can be `n`, `buf`, `sum`, `doc`.
  Codec and domain abbreviations are fine (`mb`, `qp`, `mv`, `sad`, `cbp`).
- Exported functions, struct fields and names that live long or travel far stay
  descriptive. Flag a cryptic invented abbreviation at wide scope, not a short
  local.
- **The name must not lie.** A function named for one effect that has another,
  a flag that isn't a boolean, a variable holding the opposite of its name.
- Functions are verbs, and the verb matches what the code does. Reuse the verb
  already used for the same thing elsewhere in the tree.
- Types end in `_t`. Files and directories follow what they hold.

## 13. Configuration

- New shipped behavior gets a public parameter (and its CLI flag), not a
  development env knob, a hardcoded constant or an `#ifdef`. Env knobs are
  development tools.
- Defaults keep the current output byte-identical unless the PR means to move
  it, and then it says so and shows the measurement.
- Extend the existing parameter instead of adding a parallel one.
- Flag a new knob whose only purpose is to work around a bug elsewhere.

## 14. Duplication and divergence

- Flag copy-pasted blocks and near-identical functions; prefer one function
  with a parameter, so the variants can't drift apart.
- The 8-bit and high-bit-depth paths, CABAC and CAVLC, and the serial and
  threaded paths drift; treat drift as a bug.
- A new feature that copies an existing one factors the shared part in the
  same PR.
- Check that the helper doesn't already exist before adding one.

## 15. Tests

- A test must reach the rule it names. A case whose input already fails an
  earlier check passes whether or not the rule holds.
- Test observable behavior; don't widen a header's surface for a test.
- Odd sizes, every chroma format and both bit depths: the ASan/UBSan matrix
  exists for this; a new path gets a cell in it.

## 16. PR and commit hygiene

- Small PRs. One feature per commit, so a clean part can merge while a doubtful
  part is discussed.
- The commit message matches the diff. Commits carry the author's name only: no
  Co-Authored-By or tool trailer.
- No scope creep; unrelated changes are removed or named in the description.
- Removals and renames update every reference in the same PR: build targets,
  scripts, the knob catalog, docs.
- The PR lists the gates it ran and their results.

---

## What not to flag

### Project standards: the opposite of the usual review reflex

These are house rules. Suggesting the opposite is a defect in the review, because
a maintainer then has to undo the change:

- **Never ask for more comments.** No "add a comment explaining this", no
  docblocks on every function, no narration of a loop. A comment must be
  quicker to read than the code it labels, or state something the code cannot.
  Flagging a missing comment is almost always wrong here; flagging a bloated
  one is almost always right.
- **Never accept a comment as the fix for an unclear name.** Propose the rename.
- **Never ask to lengthen a short local** that reads clearly in its scope.
- **Never propose a rationale comment that states an unverified mechanism as
  fact.** State the observation, mark the cause as suspected, or leave it out.
- **Never propose a defensive guard that masks a bug** (a null check that
  swallows, a default substituted for a parse failure, a retry around a
  symptom). Report the underlying bug; an assert that surfaces it is fine.

### Noise control

- Style the compiler or the surrounding file's convention already settles.
- Restating what the diff does, or praising it.

## Review output shape

Group findings as **Blockers** / **Major** / **Worth fixing (minor)**, each with
`path:line`, one sentence on the concrete failure (what input or state leads to
what wrong behavior), and the fix in one line. Prefer few, high-confidence
findings over exhaustive lists.
