# How yah264 decides how many threads to use

Ask an encoder to use "all your cores" and you will often get a slower encode
than if you had asked for fewer. That is not a paradox, and understanding why is
most of what there is to know about threading a video encoder.

## One picture is not infinitely divisible

yah264 parallelises inside a frame, on a row wavefront. Macroblocks are
coded in raster order, and each one depends on its left, above, above-left and
above-right neighbours being finished first. A second thread can start row 1
once row 0 is two macroblocks ahead of it, a third can start row 2 behind that,
and so on, so the work advances as a diagonal wave rolling down the picture.

The wave has a shape, and the shape has a limit. Threads fill in behind the
diagonal, so the useful number of them is bounded by how many rows the diagonal
can span before the first thread runs out of picture. A wide frame has a long
diagonal and room for many threads. A narrow one does not. Past that width the
extra threads are real threads doing real synchronisation, waking each other and
waiting, without shortening the critical path they are all queued behind.

`yah264_frame_thread_cap(width, height)` returns that limit. It is a property of the picture
alone:

| resolution | usable threads |
|---|--:|
| QCIF 176x144 | 5 |
| CIF 352x288 | 12 |
| 720p 1280x720 | 21 |
| 1080p 1920x1080 | 32 |
| 4K 3840x2160 | 63 |

A 64-core server encoding CIF gets 12 threads on one frame, and there is nothing
to be done about that at the frame level. The picture cannot absorb more.

## The ceiling, and why a default should be conservative

The cap is what a picture *can* use. It is not always what an encoder *should*
ask for.

Scaling in a wavefront is sublinear well before it reaches the cap, because
every added thread joins the same dependency chain. On an asymmetric machine it
gets worse: the last few threads get scheduled onto efficiency cores, which run the same
work slower, and since the wavefront advances at the pace of its slowest
participant those threads can lengthen the critical path rather than shorten it.

So we cap the automatic budget at 16. We measured what that ceiling costs on an
18-core Apple Silicon machine, encoding 1080p, and the answer was nothing: the
capped run and the uncapped one finished within a hundredth of a second of each
other. What did change was occupancy, which fell from 13.9 cores to 12.1. Those
last cores had been spinning rather than working.

The ceiling applies to the automatic budget only. Ask for a specific number and
you get it.

## Putting both together

The resolved width is `min(request or auto, frame_thread_cap(width, height))`,
where auto is `min(online cores, 16)`:

| resolution | cap | 4 | 8 | 12 | 16 | 32 | 64 cores |
|---|--:|--:|--:|--:|--:|--:|--:|
| QCIF | 5 | 4 | 5 | 5 | 5 | 5 | 5 |
| CIF | 12 | 4 | 8 | 12 | 12 | 12 | 12 |
| 720p | 21 | 4 | 8 | 12 | 16 | 16 | 16 |
| 1080p | 32 | 4 | 8 | 12 | 16 | 16 | 16 |
| 4K | 63 | 4 | 8 | 12 | 16 | 16 | 16 |

Read it in two directions. Down a column, small machines use everything they
have, since a ceiling of 16 never applies to them. Along a row, small pictures
clamp below the ceiling on their own, because the cap is reached first.

On Apple Silicon the core count comes from enumerating `hw.nperflevels` and
summing each level rather than assuming a fixed split of performance and
efficiency cores. The informal description of a chip and the order sysctl
reports its levels in do not always agree about which tier is which, and a
policy that hardcoded one interpretation would invert silently on a different
part. Elsewhere it is `sysconf(_SC_NPROCESSORS_ONLN)`.

## Filling a bigger machine

A 64-core server encoding 1080p will use 16 cores for one encode. That is
deliberate, and the remaining 48 are not wasted so much as unaddressed by this
layer.

Frame-level threading is one of two kinds of parallelism an H.264 encoder has.
The other is running several encoders at once over different parts of the
sequence, which scales with the machine rather than with the picture. We keep
that one in the caller: the library gives you one encoder instance and one
budget, and an application that wants a whole server busy runs several
instances, or simply several jobs. The command-line tool does exactly this, and
so would a transcoding farm.

We put it there for a measured reason. Splitting one
sequence across instances means each split point becomes a keyframe, and
keyframes cost bits. On 1080p a single instance with a wide wavefront finished
faster than a split across instances *and* spent 4.6% fewer bits, because the
split forced a keyframe the continuous encoder would not have chosen.

## Setting it yourself

Through ffmpeg, `-threads N` does what it says, and omitting it gets you auto:

```
ffmpeg -i in.mp4 -c:v libyah264 -crf 25 out.mp4            # auto
ffmpeg -i in.mp4 -c:v libyah264 -crf 25 -threads 8 out.mp4 # eight
```

Through the library, `param.threads` is the budget for one encoder instance. 0
asks it to decide, 1 is serial, N means up to N. `yah264_threads_auto()` reports
what 0 would resolve to, so an application splitting work across instances can
size its own budget from the same number.

`--threads 1` is worth knowing about for its own sake. At one thread the encoder
turns on a quality mode that a threaded encode cannot use, so a serial encode is
not merely a slow parallel one: it spends its bits slightly better.

## A bug worth remembering

For a while, every ffmpeg encode of yah264 ran on a single core.

ffmpeg tells an encoder `thread_count = 0` when the user has not asked for a
specific number, meaning "you decide". The wrapper only configured threading
when the count was greater than zero, so the ordinary case, the one with no
`-threads` on the command line, fell through and got the encoder's own default,
which was serial.

Nothing failed. The output was correct, no error appeared, and an encoder
occupying 1.05 cores looks exactly like an encoder that is simply slow. It
survived an entire benchmarking campaign because the benchmark always passed
`-threads` explicitly, so every measurement took the working path and every real
invocation took the broken one.

Two lessons stuck. A default that is only exercised by real users and never by
your harness is a default you are not testing. And "how many cores are you
using" is worth printing next to "how long did it take", because the first
explains the second and only one of them is usually recorded.
