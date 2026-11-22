# Live control

A live service can change an encode's settings mid-stream without closing
it. The caller hands the encoder control events between frames. Each event
names the input frame it lands on, takes effect there, and shows up on that
frame's `--frame-stats` line. A recorded log of events replays to the same
bytes. In the other direction, a status call reports how much is queued
inside the encoder and what the rate control has to spend, so a service sees
backpressure before a stall.

The contract is shared with yah265 and yaav1: the same event kinds, error
codes, log grammar and replay guarantee, with only the name prefix changed.
The log parser (`cli/ctl_log.c`) is one file all three carry. It builds on the
zones and frame stats in [engine-interface.md](engine-interface.md).

## What is supported

| event | log line | lands on | in which modes |
|---|---|---|---|
| bitrate and buffer | `F rate bitrate=K maxrate=K bufsize=K` | the rate decision of the first frame, in coding order, numbered F or later | bitrate in one-pass ABR; maxrate and bufsize wherever a buffer was set at open, capped CRF included |
| CRF | `F crf value=X`, 0 < X <= 51 | the same as a rate change | CRF |
| QP floor and ceiling | `F qp min=N max=N` | every frame numbered F or later | every mode but constant QP |
| keyframe | `F key` | frame F is an IDR that starts a closed GOP | every mode but two-pass |
| zone offset | `F zone last=L offset=X` | every frame F to L, offset (whole QPs) added to the rate control's QP | every mode |

Frame rate, preset and resolution changes are in the API's list of kinds and
are refused as not supported. A frame-rate change needs the rate control to
learn a new frame interval and the stream's timing to follow it; a preset
change swaps analysis settings the lookahead and the per-frame state were
built for; a resolution change needs new parameter sets and buffers at a
keyframe. None of the three is clean and cheap yet.

### Where an event lands

An event may name a frame only while that frame hasn't been handed to the
encoder. After that it's refused as late, however deep the pipeline is,
because the frame may already be typed and costed in the lookahead. So
threads, lookahead depth and B-frame reordering never move a landing frame,
and the echo reads the same at every thread count.

QP limits, keyframes and zones land on exact input frames. Rate and CRF
changes land at a rate decision: the first frame in coding order whose number
is F or later. Every frame from F on is coded under the change, and a B frame
numbered below F that is coded after that decision takes it too.

A frame force from `--qpfile` or the frame-force API is absolute and wins
over a zone offset and over the QP limits on its frame.

### How a rate change is applied

The rate control's ledger is retargeted, not reset. At the landing decision,
with r the new per-frame target over the old one:

- the bits spent and the bits owed so far are both scaled by r, so an over-
  or under-spend carries over as the same fraction of the new rate;
- what the rate control has learned about the content is kept, so the
  operating point moves straight to the one that spends the new target;
- frames already decided but not yet coded keep their predicted size and owe
  the new target from then on;
- the slew reference and the intra-frame track move by the same QP step as
  the operating point, so the slew still limits what the content does and not
  the step the caller asked for;
- a buffer keeps its fill as a fraction of its size and refills at the new
  maxrate from the next frame;
- the balance carried into the new rate, bits owed minus bits spent with the
  frames already decided counted at their predicted size, is held to at most
  one second of the new target, and the rest is forgiven.

For two seconds after a change the rate control may move its QP a whole step
per anchor, as it does after the opening, while the content's cost at the
new rate is still unproven.

With a buffer of about a second, a step lands on its new rate within the
first half second. With no buffer, a step down settles more slowly: what the
rate control learned about the content at the old rate prices it at the new
one, and that estimate is off by a few QP after a large step until frames at
the new rate outweigh it.

### What is refused, and why

- An encode opened as CBR (maxrate equal to the bitrate) stays CBR: a bitrate
  change without a maxrate moves the maxrate with it, and one that would part
  the two is refused. An encode opened as capped VBR keeps maxrate above the
  bitrate.
- A buffer can't be switched on or off mid-stream.
- A rate past what the stream's declared level allows is refused. The level
  is chosen at open from the opening rate, so a stream that will step up
  should open at its top rate or name a `--level`.
- Under `--nal-hrd vbr` the declaration in the stream never changes. A rate
  change that keeps the bitrate, maxrate and buffer at or below what was
  declared is accepted: a buffer fed at the declared rate that the stream now
  spends more slowly can't underflow. Anything above it is refused. Under
  `--nal-hrd cbr` every rate change is refused, because the declared constant
  rate would be padded back with filler. A QP ceiling under 51 is refused
  under either, as it is at open.
- Two-pass encodes refuse rate, CRF and keyframe events: pass 2 follows its
  stats file. The CLI refuses `--control` and `--control-fd` with `--pass`
  outright.
- An event is judged against the state it would land in: the settings in
  force at its frame once every event queued before it has landed. It's
  refused if it doesn't fit there, or if any event already queued after it
  would no longer fit.
- The encoder holds at most 4096 events naming frames after the next one to
  be handed in, and at most 1 << 20 events waiting to land and applied
  records waiting to be drained together. Events for the next frame count
  only toward the second bound, so what is due now can always be queued, and
  every queued event has room for its record. An event past a bound is
  refused with `YAH264_CTL_EFULL`; draining
  `yah264_encoder_controls_applied()` frees room.
- The hardware encoder (`--hw`) takes no events.

## From the command line

```
yah264 --input-y4m in.y4m --bitrate 3000 --vbv-maxrate 3000 --vbv-bufsize 3000 \
       --control steps.ctl --frame-stats stats.jsonl -o out.264
```

`--control FILE` reads a log, one event per line:

```
#livectl 1
# step down at 4 s, back up at 12 s (30 fps)
120 rate bitrate=1000 maxrate=1000
120 key
360 rate bitrate=3000 maxrate=3000
400 zone last=459 offset=-3
```

Frames count from 0 in input order and may not decrease down the file. The
optional first line names the log's version. A line is `FRAME KIND` and then
`name=value` arguments; a line whose first character after any spaces is `#`
is a comment, and a `#` after an event (`5 key # note`) is refused. Numbers
are plain decimals: no hex, exponent or `inf`; a sign only on the offsets and
the CRF, which take at most 8 digits before the point and 6 after. Before the first frame the whole file is parsed and
every event is checked against the encoder in order, as if all were queued:
an unknown kind or argument, a bad value, a NUL byte, a frame that goes
backwards, a file that can't be read, or an event the encoder would refuse
stops the run with the file name, the line number and the reason, and the
output is left as it was. So does a log the encoder's queue couldn't hold:
more than 65536 events on one frame, or more than 262144 rate and CRF events
within 4096 frames. The events then reach the encoder as frames go by: each
before its frame is handed in, and up to 1024 ahead of that. Events for a
frame the stream never reaches are reported at the end.

`--control-fd N` reads the same lines from an open descriptor while the
encode runs, taking up to 64 of the lines that have arrived before each
frame is handed in. The frame may be `next`, the frame about to be handed in.
A numbered line for a frame that has already gone in, or an event the
encoder refuses, is skipped with a warning. With `--control` as well, the
file's events up to a line's frame are queued first, and the line is
skipped (queue full) if that would take the file more than 1024 events
ahead; a line that would make one of the file's later events fail is
skipped too, so the file's events are never refused mid-run. That check
costs each line time in proportion to the file's events of its kind not yet
queued; checking a log in frame order up front is linear. A malformed line, a NUL byte or a
descriptor that can't be read stops the encode; one that isn't open or is
write-only is refused at startup. An unterminated last line counts. The
descriptor's flags are put back at exit.

`--control-out FILE` writes every event that took effect, with the frame it
named, as a `--control` log. It's the record of a live run.

Live control runs one encoder instance, so `--threads` goes to its row
wavefront and lookahead rather than to parallel GOPs: an event names a frame
of the whole stream, and the ledger it retargets is one encoder's.

### The echo

A frame where an event took effect carries a `control` key on its
`--frame-stats` line, the events as log lines with the frame each named:

```
{"frame": 122, "gop": 0, "type": "P", "idr": 0, "ref": 1, "qp": 31, "bytes": 4210, "k": 6, "control": ["120 rate bitrate=1000 maxrate=1000"]}
```

The line's `frame` is where the event took effect, and the string's number is
the frame it named. A CBR rate change without a maxrate is echoed with the
maxrate it moved to. Lines with no event have no `control` key.

## From C

```c
yah264_control_t ev;
yah264_control_init(&ev, YAH264_CTL_RATE);       /* lands on the next frame */
ev.bitrate_kbps = 1000;
ev.vbv_maxrate_kbps = 1000;
int r = yah264_encoder_control(enc, &ev);        /* ev.frame now holds the landing frame */
if (r != YAH264_CTL_OK)
    fprintf(stderr, "%s\n", yah264_control_strerror(r));
```

`yah264_encoder_control()` validates and queues an event, or returns one of
the codes below and queues nothing. `yah264_encoder_control_check()` runs the
same checks over a list of events, each as if the ones before it had been
queued, and queues nothing; it returns the first refusal's code and index. `yah264_encoder_controls_applied()`
drains the events that took effect, in the order they were applied, each with
`applied_frame` set. After the flush, an event whose frame never arrived
comes back with `applied_frame` -1. It returns -1 once if records were lost,
which only an allocation failure causes; the events themselves still took
effect. Both are called between encode calls,
from the thread that calls encode.

| code | meaning |
|---|---|
| `YAH264_CTL_EINVAL` | bad pointer, struct size or value |
| `YAH264_CTL_EUNSUPPORTED` | the kind isn't implemented (frame rate, preset, resolution, or a kind this library doesn't know) |
| `YAH264_CTL_ELATE` | the landing frame has already been handed in |
| `YAH264_CTL_EMODE` | the kind doesn't fit the encode's rate-control mode |
| `YAH264_CTL_EHRD` | it would break the signalled HRD |
| `YAH264_CTL_ELEVEL` | it exceeds the declared level |
| `YAH264_CTL_ECONFLICT` | the zone overlaps one already set |
| `YAH264_CTL_ENOMEM` | out of memory |
| `YAH264_CTL_EFULL` | the control queue is full; resend once events have landed |

`yah264_encoder_status()` fills a `yah264_status_t`: pictures in and frames
out, the queue depth in frames and in stream time, the next frame an event
can name, the wall time the oldest queued frame has spent inside, the current
bitrate and buffer, the buffer's fill after the last frame whose bits the
rate control has counted (the whole stream once it's flushed), the rate
ledger's balance (target bits minus bits spent) and the bits the next frame
may spend. Every field but the
wall time is a function of the input and the events, so it reads the same on
a replay at one thread.

Both structs carry their size as their first field and only ever grow at the
end, so a caller built against an older header keeps working. Neither touches
`yah264_param_t`.

## What is guaranteed

- With no events, the stream is byte-identical to an encode without live
  control, in every configuration.
- The same input, parameters and events with the same landing frames give the
  same bytes at one thread, whenever each event was handed in, as long as it
  was handed in before its frame. A live run's `--control-out` log, fed back
  through `--control`, reproduces the live run's stream and stats. With more
  threads the landing frames and the echo are the same, and byte identity is
  best effort, as everywhere else.
- A stream with events decodes to the encoder's own reconstruction under the
  reference decoders, and under `--nal-hrd vbr` passes the stream's own HRD
  check.
