---
name: writing-comments-in-code
description: ALWAYS invoke before writing or editing comments in any source file, and when reviewing a diff for comments — not optional, run it every time. How to comment: severely sparingly; a comment must be quicker to read than the code it labels, or say something the code can't. Default to no comment.
---

# Writing comments in code

The implementation is the source of truth. A comment must **not** re-explain in detail
what the code does — for detail, the reader reads the code. A comment earns its place
only if it is:

1. **quicker to read than the code it labels** — a glanceable (oneliner strongly prefered) 
   summary so the reader skims and dives in only where needed, or
2. **information that can't be deduced from the code** — a pitfall or rationale (and
   needing this is itself a sign the code could be clearer).

## A glanceable label (95% of all comments !)

Before a block that does one meaningful thing, put a **few-word** summary of *what* it
does — `/* count skip blocks */`, `/* pick the cheapest partition */`, `/* reset per-frame state */`.
One glance tells me the block's job; I skip it or read the lines for *how*. The
comment says *what*, never *how* — the code already says how.

## Pitfalls or rationale

Only if the code is confusing for some reason (decoder quirk, ordering hazard, why-not-the-obvious-way), then comment.
Keep the comment short : oneliner strongly preferred, max 3 lines.
If possible, make the code less confusing instead.

## Rules

- **DON'T paraphrase implementation.** 
  `/* count skip blocks */`, not three lines narrating the loop.
- **DON'T paraphrase function names nor arguments nor return type, nor variable names.** 
  Most variables/functions should be self-explanatory; if a
  name needs a comment to be understood, rename instead.
- **DON'T comment about call places.** 
  A tool's manual says what the tool does, how to use it, not who uses it
- **Quicker to read than the code, or delete it.** 
  A comment as long as the code it describes is noise.
- **A module-head block** (purpose, invariants, data flow) is acceptable.
- **Match what the site already had.** Look at it before your edit: no comment → none
  now; a few words → a few words. It was very likely enough and still is. Growing it
  needs a reason you can state in one clause.
- **Label after, not while.** A comment written in the same breath as the code comes
  out as your own reasoning *while* doing it. The reader may not need it. 
  Settle the code, then comment, for the reader, not for yourself.
- **State what's known, never speculate.** A rationale comment must assert only
  what's verified (observed behavior, a measured symptom, a documented spec). Do
  NOT state an unconfirmed *mechanism* or *cause* as fact — "X clears the next
  frame's buffer, so Y" when all you saw was "Y fails." Write the observation
  ("X path produced no copies; Y works") and, if the cause is a guess, either
  omit it or mark it ("likely", "suspected"). A confident wrong "why" is worse
  than no "why" — it misleads the next reader into trusting it.

Severe trimming is correct. When in doubt, delete the comment.
