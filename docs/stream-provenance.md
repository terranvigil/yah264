# Stream provenance: the identification string and the fingerprint

A stream can say it was ours in two ways, on all three codecs. The first is a
readable string any tool can print. The second survives having the string
stripped.

## 1. The identification string

x264 and x265 write a "user data unregistered" SEI at the start of the
stream. It holds a 16-byte UUID followed by text naming the encoder, its
version, a copyright line, a URL and the options in force. `strings` and
ffprobe show it. People use it to check a stream's configuration after the
fact, and support uses it to learn what made a file.

| codec | carrier | state |
|---|---|---|
| yah264 | user data unregistered SEI, on by default, `--no-sei` to drop it | done. The payload lacks the copyright and URL line. The UUID still spells the project's old name |
| yah265 | the same SEI, built but off by default. Turning it on changes every stream by one message. The identity harness reads that as a diff on every cell | todo: on by default with a re-baselined harness, plus the options list |
| yaav1 | AV1 has no unregistered user data. A padding OBU carries arbitrary bytes that decoders must ignore. An ITU-T T.35 metadata OBU is the registered route and needs a manufacturer code | todo: the padding OBU form first |

All three use one format: `<name> <version> - <codec> - Copyright <years> the
<name> authors - <url> - options: k=v ...`. Each codec gets one UUID, fixed
for the life of the format. The string is on by default and one flag drops it.

## 2. The fingerprint

A remuxer or a one-line filter drops the string first. A fingerprint lives in
choices the syntax leaves open: places where two encodings are equally legal,
decode to the same pixels and cost the same bits. A fixed pattern in those
choices names the encoder without spending anything. It survives remuxing and
message stripping. Nothing survives a re-encode.

Choice points, by cost:

- **Zero cost, per frame.** Ties in the search. Among motion vectors with
  equal cost, equal-cost intra modes or equal merge candidates, the encoder
  picks by a rule instead of by search order. A 64-bit signature spread over
  the ties of one GOP and repeated every GOP is invisible to the rate and the
  picture. A parser that knows the rule can read it.
- **A few bits, per stream.** Parameter-set fields that admit several
  legal values with no effect on the picture: the log2 sizes of frame
  number and picture order count in H.264, the short-term reference set
  signaling form in HEVC, the OBU header options and reference signaling
  forms in AV1. They're cheap to read and carry few bits of identity. Only a
  re-serialization strips them.

The design will land with a verifier, `scripts/provenance_check.py` (not written yet), that parses
a stream and prints which of the two marks it finds. The fingerprint changes
the default stream, so it lands as one flip with the identity harness
re-baselined. It must keep two `--threads 1` runs byte-identical, the hard
gate the encoder already runs. Threaded output doesn't have to match across
runs or thread counts.

Status: todo on all three. yah264 ideas backlog, yah265 ideas log, yaav1
plan phase 7.
