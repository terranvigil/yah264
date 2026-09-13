# Live control: adjusting the encoder from outside while it runs

A broadcast or live encoding service sits between a source and a network
that keeps changing. Bandwidth drops, the sender's queue backs up, a viewer
joins and needs a keyframe, the box runs hot and the encoder falls behind
real time. Today the service's only lever is to close the encoder and open
another one with different settings, and every established encoder answers
the same way: a reconfigure call that swaps the whole parameter set between
frames. That call is a blunt instrument. It does not say which frame took the
change, it does not tell the rate control anything about why the target moved,
it cannot express "drop to 15 fps until the queue drains", and it gives the
service nothing back. The service ends up polling the packet sizes it just
sent and guessing.

This plan gives yah264 (and yah265 and yaav1, same contract) a control plane
that is a first-class part of the encoder: settings the caller can change per
frame, applied at a defined boundary, echoed back in the frame stats, coupled
into the rate control instead of resetting it, and paired with a backpressure
signal in the other direction. The same contract on all three codecs means a
service writes its control loop once.

Status: `planned`. This is a differentiator (docs/innovations.md section 10)
and the first product-shaped feature after the hull-assist hooks
(docs/engine-interface.md), which it reuses.

## What the caller can change, and when it lands

A control is a (frame index, setting, value) event. The caller issues it any
time; the encoder applies it at the first frame boundary at or after the
index, and the change is visible in that frame's stats line. Controls never
require a close and reopen.

| control | what it does | lands at |
|---|---|---|
| target bitrate, VBV rate and buffer | the ABR/CBR target moves; the rate control keeps its complexity history and re-solves the rate factor for the new target instead of restarting from the opening | next frame |
| CRF value | the constant-quality point moves | next frame |
| QP ceiling and floor | hard bounds on the per-frame QP, so a bandwidth cliff cannot turn the picture to mud and a bandwidth windfall cannot spend bits nothing can see | next frame |
| frame rate | the declared rate changes, or the encoder is told to code every Nth input frame (decimation); the per-frame bit budget rescales with it and timestamps stay honest | next frame; decimation from the next keyframe when the stream's timing must not skew |
| keyframe request | an IDR at the next frame, for viewer join, recovery after loss, or a segment boundary | next frame |
| speed preset | the analysis effort moves up or down a tier; for a box falling behind real time | next frame (row-level state is per frame) |
| resolution | new parameter sets and a keyframe at the switch; the lookahead is drained | next keyframe |
| zone offsets | the existing plan zones (`qp+N`, `idr`) issued live instead of from a file | as the zone says |

Rules, in the spirit of the engine interface:

- **Ordered and deterministic.** A control log (the sequence of events with
  their frame indices) replayed against the same input produces the same
  bytes. That makes a live session reproducible offline, which is how the
  feature is tested and how a service debugs a bad night.
- **Echoed, not assumed.** Every frame's stats line carries the settings in
  force for that frame. The service never has to infer whether its change
  landed.
- **Coupled, not reset.** A bitrate change is a new target for the same rate
  control, with its learned complexity and frame-type scales intact. The
  opening refit that fixed our ABR start (fitted on the lookahead ring) is
  the mechanism to reuse: a target change is an opening at a known
  complexity.
- **Nothing in the default stream changes.** With no controls issued the
  output is byte-identical to the encoder without the control plane.

## What the encoder tells the caller

Backpressure runs both ways. The caller needs to know when the encoder is
the bottleneck, not the network.

- Per frame, already in the frame stats: bytes, slice QP, type. Added: the
  settings in force, wall time to encode, and the age of the frame (input
  time to output time) so the service sees latency creep before it becomes
  a stall.
- Queue depth: how many frames the encoder holds (lookahead plus in flight),
  and whether the input side is being made to wait. A service that feeds
  from a camera uses this to decide between decimating and buffering.
- A budget signal: the encoder's estimate of the bits the next N frames will
  need at the current settings, from the lookahead's costs. This is what
  lets a service act before the buffer overruns rather than after.

## What is deliberately out

- No network model inside the encoder. It does not know about RTP, SRT,
  WebRTC or HLS; it exposes levers and signals, and the service owns the
  policy.
- No resolution or frame-rate ladder decisions: the service picks, the
  encoder executes.
- No hidden retuning: the encoder never changes a setting the caller did not
  ask for. Adaptive behaviour belongs in the example app, where it can be
  read.

## API shape

Following the public-struct rule (never grow the parameter struct; add
functions), one entry point per direction:

```c
/* issue a control; applied at the first frame boundary >= frame */
int yah264_encoder_control(yah264_encoder_t *enc, int64_t frame,
                           yah264_control_t what, const yah264_control_value_t *v);

/* what the encoder is doing right now: queue depth, lateness, bit budget */
int yah264_encoder_status(const yah264_encoder_t *enc, yah264_status_t *out);
```

The settings in force ride on the existing frame-stats call. The CLI gets a
control channel for scripting and tests: `--control FILE` reads events
(`frame setting value`, one per line, the plan-file spelling extended), and
`--control-fd N` reads the same lines from a pipe while running, which is
how the example app drives it without linking the library. yah265 and yaav1
use the same spellings with their own prefixes.

## The example app: a live encoder that reacts

`examples/livecast/` is a small program that shows the feature doing the job
it exists for. It takes a camera (AVFoundation on macOS) or a Y4M pipe as
the source, encodes with the library, and pushes packets into a simulated
network: a token bucket whose rate follows a scripted trace (steady, then a
cliff, then a slow recovery, then a burst of loss). The app's control loop
is deliberately simple and printed as it runs:

- bucket draining faster than it fills: lower the bitrate target, then the
  frame rate, in that order; raise them back on a schedule when it refills
- encoder queue depth past a threshold: step the speed preset down
- simulated viewer join, or loss: request a keyframe
- every second: print the frame stats line, the bucket level, and the
  settings in force, so the reaction is readable in the terminal

Same app, same trace, on all three codecs: the point of the demo is that
the control code does not change when the codec does. A recorded run
(the control log and the stats) is the artefact; the replay test proves the
recorded log reproduces the recorded bytes.

## Gate

- Replay: a control log from a live run replays to byte-identical output.
- Coupling: a bitrate step down and back up on the board clips costs no
  more BD-rate than a fixed encode at the lower rate for the same span
  (the rate control does not lurch).
- Latency: keyframe and bitrate controls land at the next frame; measured
  in the stats.
- The default stream is byte-identical with the control plane compiled in.
