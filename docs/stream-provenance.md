# Stream provenance: the identification string and the fingerprint

Two ways a stream says it was ours, on all three codecs. The first is a
readable string any tool can print; the second survives having the string
stripped.

## 1. The identification string

x264 and x265 write a "user data unregistered" SEI at the start of the
stream: a 16-byte UUID, then text naming the encoder, its version, a
copyright line, a URL and the options in force. `strings` and ffprobe show
it. It is how a stream's configuration is checked after the fact and how
support knows what made a file.

| codec | carrier | state |
|---|---|---|
| yah264 | user data unregistered SEI, on by default, `--no-sei` to drop it | done; the payload lacks the copyright and URL line and the UUID still spells the project's old name |
| yah265 | the same SEI, built but off by default because turning it on changes every stream by one message and the identity harness reads that as a diff on every cell | todo: on by default with a re-baselined harness, plus the options list |
| yaav1 | AV1 has no unregistered user data; a padding OBU carries arbitrary bytes and decoders must ignore it; an ITU-T T.35 metadata OBU is the registered route and needs a manufacturer code | todo: the padding OBU form first |

One format on all three: `<name> <version> - <codec> - Copyright <years> the
<name> authors - <url> - options: k=v ...`, one UUID per codec fixed for the
life of the format, on by default, one flag to drop it.

## 2. The fingerprint

The string is the first thing a remuxer or a one-line filter drops. A
fingerprint lives in choices the syntax leaves open: places where two
encodings are equally legal, decode to the same pixels and cost the same
bits, so a fixed pattern in those choices names the encoder without
spending anything. It survives remuxing and message stripping. Nothing
survives a re-encode.

Choice points, by cost:

- **Zero cost, per frame.** Ties in the search. Among motion vectors with
  equal cost, among equal-cost intra modes, among equal merge candidates,
  the encoder picks by a rule instead of by search order. A 64-bit
  signature spread over the ties of one GOP, repeated every GOP, is
  invisible to the rate and the picture and readable by a parser that
  knows the rule.
- **A few bits, per stream.** Parameter-set fields that admit several
  legal values with no effect on the picture: the log2 sizes of frame
  number and picture order count in H.264, the short-term reference set
  signalling form in HEVC, the OBU header options and reference signalling
  forms in AV1. Cheap to read, few bits of identity, stripped only by a
  re-serialisation.

The design lands with a verifier, `scripts/provenance_check.py`, that parses
a stream and prints which of the two marks it finds. The fingerprint is a
change to the default stream, so it lands as one flip with the identity
harness re-baselined, and it must keep the pipeline deterministic across
thread counts, which is the same test the encoder already runs.

Status: todo on all three. yah264 ideas backlog, yah265 ideas log, yaav1
plan phase 7.
