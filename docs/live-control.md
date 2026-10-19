# Live control

Not built. Today a live service that wants different settings has to close the
encoder and open a new one; `yah264_encoder_open` takes its parameters once.

The idea is a control plane for live encoding: the caller issues per-frame
events (target bitrate and VBV, CRF, QP floor and ceiling, frame rate, a
keyframe request, preset, resolution, zone offsets), each lands at a defined
frame boundary, is echoed in that frame's `--frame-stats` line, and feeds the
rate control as a new target instead of resetting it. In the other direction
the encoder would report queue depth, frame age and a bit-budget estimate so
the service sees backpressure before a stall. A recorded control log would
replay to the same bytes, and with no controls issued the stream would be
unchanged. The contract would be shared with yah265 and yaav1, and it builds on
the hooks in [engine-interface.md](engine-interface.md).
