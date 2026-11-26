# Live control

A live service can change an encode's settings mid-stream without closing
it. The caller hands the encoder control events between frames. Each event
names the input frame it lands on, takes effect there and shows up on that
frame's `--frame-stats` line. A recorded log of events replays to the same
bytes. A status call reports what is queued inside the encoder and what the
rate control has to spend, so a service sees backpressure before a stall.

yah265 and yaav1 share this contract. Event kinds, error codes, log grammar and
the replay guarantee are the same in all three. Only the name prefix differs.
All three build the same log parser file (`cli/ctl_log.c`). Live control builds
on the zones and frame stats in the [engine interface](engine-interface.md).

## What is supported

| event | log line | lands on | in which modes |
|---|---|---|---|
| bitrate and buffer | `F rate bitrate=K maxrate=K bufsize=K` | the rate decision of the first frame, in coding order, numbered F or later | bitrate in one-pass ABR; maxrate and bufsize wherever a buffer was set at open, capped CRF included |
| CRF | `F crf value=X`, 0 < X <= 51 | the same as a rate change | CRF |
| QP floor and ceiling | `F qp min=N max=N` | every frame numbered F or later | every mode but constant QP |
| keyframe | `F key` | frame F is an IDR that starts a closed GOP | every mode but two-pass |
| zone offset | `F zone last=L offset=X` | every frame F to L, offset (whole QPs) added to the rate control's QP | every mode |

Frame rate, preset and resolution changes are in the API's list of kinds and
are refused as not supported. None of the three is clean and cheap yet:

| change | what it would need |
|---|---|
| frame rate | the rate control to learn a new frame interval, and the stream's timing to follow it |
| preset | new analysis settings under a lookahead and per-frame state built for the old ones |
| resolution | new parameter sets and buffers at a keyframe |

### Where an event lands

An event may name a frame only while that frame hasn't been handed to the
encoder. After that it's refused as late, however deep the pipeline is,
because the lookahead may already have typed and costed the frame. Threads,
lookahead depth and B-frame reordering never move a landing frame. The echo
reads the same at any thread count.

QP limits, keyframes and zones land on exact input frames. Rate and CRF
changes land at a rate decision: the first frame in coding order whose number
is F or later. Every frame from F on is coded under the change, and so is a
B frame numbered below F that is coded after that decision.

A frame force from `--qpfile` or the frame-force API is absolute. It wins
over a zone offset and over the QP limits on its frame.

### How a rate change is applied

The rate control's ledger is retargeted, not reset. At the landing decision,
with r the new per-frame target over the old one:

| part | what happens |
|---|---|
| bits spent and bits owed so far | both scaled by r, so an over- or under-spend carries over as the same fraction of the new rate. Frames already decided when the change lands, coded or still in flight, count the same way, at r times their size |
| what the rate control has learned about the content | kept |
| operating point | moves by 6 log2(1/r) QP, the move that spends the new target if a frame's bits go as 1/qscale |
| slew reference and intra-frame track | move by the same QP step as the operating point, so the slew still limits what the content does, not the step the caller asked for |
| buffer | keeps its fill as a fraction of its size and refills at the new maxrate from the next frame |
| balance carried into the new rate (bits owed minus bits spent) | held to at most one second of the new target; the rest is forgiven |
| QP movement | for two seconds after the change, may move a whole step per anchor, as after the opening, while the content's cost at the new rate is unproven |

#### Large steps in the rate-factor model

In the rate-factor model (B frames or a lookahead on, the default), a step of
more than 3x either way is handled differently, because on real content a
frame's bits fall faster than 1/qscale: a tenth of the bits is often 9 to 12
QP away, not 20.

1. The operating point moves by 6 log2(1/r) / 1.5 QP.
2. One run coded before the step and the first run coded after it give the
   exponent a in bits ~ qscale^-a. The operating point moves again to where
   that run would have spent the new target. A slope outside 1 to 3 means
   the content changed between the two runs, and the last slope (1.5 at
   first) is used instead.
3. One more run, nearer the target, refits once.

A run is whole groups (an anchor and the B frames coded against it, a
frame's two fields together) holding at least a quarter second of frames.
Each run stands as many frames from a QP move as the encoder has in flight.
Its means use the frame QP the rate control chose, before the
adaptive-quantization and mb-tree offsets, and leave out I frames.

A scene cut in a run or between the two runs stops the refit and keeps the
moves already made. So does a run whose frames are mostly held at a QP bound
(`--qpmin`, `--qpmax`, a live QP-limit event, or the rate control's own
range), because its bits answer the clamp. Any later rate change, a
buffer-only one included, ends a refit in progress. For two seconds after
refitting ends, the ledger runs on a memory of about a second, so frames at
the new rate soon outweigh those coded at the old one.

With a buffer, only a step down is handled this way; a step up is left to
the buffer, whose fill already paces it. The per-frame model
(`--tune zerolatency`, or `--bframes 0` with no lookahead) moves straight by
6 log2(1/r) and never refits.

#### How quickly a step settles

| case | in the first second | after that |
|---|---|---|
| buffer of about a second | lands near its new rate | |
| no buffer, step down | lands near its new rate | follows the content as plain ABR does at a steady rate. On content whose cost moves, that can be a quarter or more off for a second or two |
| no buffer, step up | can still land well off its target, over or under | |

### What is refused, and why

| case | rule |
|---|---|
| encode opened as CBR (maxrate equal to the bitrate) | stays CBR. A bitrate change without a maxrate moves the maxrate with it; one that would part the two is refused |
| encode opened as capped VBR | keeps maxrate above the bitrate |
| buffer | can't be switched on or off mid-stream |
| level | a rate past what the stream's declared level allows is refused. The level is chosen at open from the opening rate, so a stream that will step up should open at its top rate or name a `--level` |
| `--nal-hrd vbr` | the declaration in the stream never changes. A rate change that keeps the bitrate, maxrate and buffer at or below what was declared is accepted (a buffer fed at the declared rate that the stream now spends more slowly can't underflow). Anything above it is refused |
| `--nal-hrd cbr` | every rate change is refused, because the declared constant rate would be padded back with filler |
| QP ceiling under 51 | refused under either `--nal-hrd` mode, as it is at open |
| two-pass | rate, CRF and keyframe events are refused (pass 2 follows its stats file). The CLI refuses `--control` and `--control-fd` with `--pass` outright |
| hardware encoder (`--hw`) | takes no events |

An event is judged against the state it would land in: the settings in force
at its frame once every event queued before it has landed. It's refused if it
doesn't fit there, or if any event already queued after it would no longer
fit.

The control queue has two bounds. An event past either is refused with
`YAH264_CTL_EFULL`, and draining `yah264_encoder_controls_applied()` frees
room.

| bound | counts |
|---|---|
| 4096 | events naming frames after the next one to be handed in |
| 1 << 20 | events waiting to land and applied records waiting to be drained, together |

Events for the next frame count only toward the second bound, so an event
due now can always be queued, and every queued event has room for its
record.

## From the command line

```
y264 --input-y4m in.y4m --bitrate 3000 --vbv-maxrate 3000 --vbv-bufsize 3000 \
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

| log rule | |
|---|---|
| frames | count from 0 in input order and may not decrease down the file |
| first line | optional; names the log's version |
| event line | `FRAME KIND`, then `name=value` arguments |
| comments | a line whose first character after any spaces is `#`. A `#` after an event (`5 key # note`) is refused |
| numbers | plain decimals: no hex, exponent or `inf`. A sign only on the offsets and the CRF, which take at most 8 digits before the point and 6 after |

Before the first frame, the whole file is parsed and each event is checked
against the encoder in order, as if all were queued. These stop the run with
the file name, the line number and the reason, and leave the output as it
was:

| problem | |
|---|---|
| bad input | an unknown kind or argument, a bad value, a NUL byte, a frame that goes backwards, a file that can't be read |
| refusal | an event the encoder would refuse |
| queue overflow | a log the encoder's queue couldn't hold: more than 65536 events on one frame, or more than 262144 rate and CRF events within 4096 frames |

The events then reach the encoder as frames go by. Each is queued before
its frame is handed in, and up to 1024 ahead of that. Events for a frame the
stream never reaches are reported at the end.

### Reading events while the encode runs

`--control-fd N` reads the same lines from an open descriptor while the
encode runs, taking up to 64 of the lines that have arrived before each
frame is handed in. The descriptor's flags are put back at exit.

| input | result |
|---|---|
| frame `next` | the frame about to be handed in |
| a numbered line for a frame already handed in, or an event the encoder refuses | skipped with a warning |
| a malformed line, a NUL byte, or a descriptor that can't be read | stops the encode |
| a descriptor that isn't open or is write-only | refused at startup |
| an unterminated last line | counts |

With `--control` as well, the file's events up to a line's frame are queued
first. The line is skipped (queue full) if that would take the file more
than 1024 events ahead. A line that would make one of the file's later
events fail is skipped too, so the file's events are never refused mid-run.
That check costs each line time in proportion to the file's events of its
kind not yet queued. Checking a log in frame order up front is linear.

`--control-out FILE` writes every event that took effect, with the frame it
named, as a `--control` log: the record of a live run.

Live control runs one encoder instance, so `--threads` goes to its row
wavefront and lookahead rather than to parallel GOPs: an event names a frame
of the whole stream, and the ledger it retargets is one encoder's.

### The echo

A frame where an event took effect has a `control` key on its
`--frame-stats` line, holding the events as log lines with the frame each
named:

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

| call | what it does |
|---|---|
| `yah264_encoder_control()` | validates and queues an event, or returns one of the codes below and queues nothing |
| `yah264_encoder_control_check()` | runs the same checks over a list of events, each as if the ones before it had been queued. Queues nothing; returns the first refusal's code and index |
| `yah264_encoder_controls_applied()` | drains the events that took effect, in the order they were applied, each with `applied_frame` set. After the flush, an event whose frame never arrived comes back with `applied_frame` -1. Returns -1 once if records were lost, which only an allocation failure causes; the events themselves still took effect |

These and `yah264_encoder_status()` are called between encode calls, from
the thread that calls encode.

| code | meaning |
|---|---|
| `YAH264_CTL_EINVAL` | bad pointer, struct size or value |
| `YAH264_CTL_EUNSUPPORTED` | the kind isn't implemented (frame rate, preset, resolution, or a kind this library doesn't know) |
| `YAH264_CTL_ELATE` | the landing frame has already been handed in |
| `YAH264_CTL_EMODE` | the kind doesn't fit the encode's rate-control mode |
| `YAH264_CTL_EHRD` | it would break the signaled HRD |
| `YAH264_CTL_ELEVEL` | it exceeds the declared level |
| `YAH264_CTL_ECONFLICT` | the zone overlaps one already set |
| `YAH264_CTL_ENOMEM` | out of memory |
| `YAH264_CTL_EFULL` | the control queue is full; resend once events have landed |

`yah264_encoder_status()` fills a `yah264_status_t`:

| field | |
|---|---|
| progress | pictures in and frames out |
| queue depth | in frames and in stream time |
| next frame an event can name | |
| wall time | the time the oldest queued frame has spent inside |
| rate | the current bitrate and buffer |
| buffer fill | after the last frame whose bits the rate control has counted (the whole stream once it's flushed) |
| ledger balance | target bits minus bits spent |
| next frame's budget | the bits the next frame may spend |

Every field but the wall time is a function of the input and the events. A
replay at one thread reads the same values. Both structs record their size
in their first field and only ever grow at the end. A caller built against
an older header keeps working. Neither touches `yah264_param_t`.

## What is guaranteed

- With no events, the stream is byte-identical to an encode without live
  control, in every configuration.
- The same input, parameters and events with the same landing frames give the
  same bytes at one thread, whenever each event was handed in, as long as it
  was handed in before its frame. A live run's `--control-out` log, fed back
  through `--control`, reproduces its stream and stats.
- With more threads the landing frames and the echo are the same, and byte
  identity is best effort.
- A log replays byte for byte only on a build that applies rate changes the
  same way. Its landing frames and echo are the same on any build.
- A stream with events decodes to the encoder's own reconstruction under the
  reference decoders, and under `--nal-hrd vbr` passes the stream's own HRD
  check.
