---
title: How video encoding works - yah264
description: The fundamentals every codec shares, with live figures. This is part one, before any H.264.
toc_label: Part one: fundamentals
---

<p class="kicker">Part one: what every codec shares</p>

# How video encoding works

<p class="standfirst">Most video codecs are built from the same handful of
parts. Part one covers them so that
<a href="how-h264-works.html">part two</a> can focus on the coding tools
H.264 adds.</p>

## The bit budget

A second of uncompressed 1080p60 video in 8-bit 4:2:0 is about 187 megabytes.
A good-quality stream of the same second is about 6 Mbit. That's 99.6% of the
bits thrown away. Nobody notices. The rest of this page is about how the
encoder chooses what to discard.

  <figure>
    <svg viewBox="0 0 700 158" width="100%" role="img" aria-label="A bar for one second of raw video, with the delivered stream as a sliver at its left edge">
      <defs>
        <!-- The bar was an empty outline, which reads as absence when it is the
             thing the whole figure is about. Hatching it in the accent ties it
             to the solid sliver: same color, one filled and one only ruled, so
             the eye reads whole against survivor rather than two objects. Kept
             faint -- at full strength it competes with the 3px sliver, which is
             the one mark that has to be seen. -->
        <pattern id="raw-hatch" width="9" height="9" patternUnits="userSpaceOnUse"
                 patternTransform="rotate(45)">
          <line x1="0" y1="0" x2="0" y2="9" stroke="#6741d9" stroke-width="1.4"
                opacity="0.28"/>
        </pattern>
      </defs>
      <!-- only the big outline is hand-drawn; anything thinner than a few px
           gets chewed up by the displacement filter, so it stays crisp -->
      <path fill="url(#raw-hatch)" d="M20.0,46.0L46.4,45.9L72.8,46.1L99.2,46.8L125.6,45.9L152.0,46.0L178.4,46.2L204.8,45.4L231.2,46.0L257.6,46.3L284.0,46.6L310.4,45.2L336.8,45.6L363.2,45.2L389.6,46.6L416.0,46.4L442.4,45.1L468.8,47.0L495.2,46.9L521.6,46.3L548.0,46.2L574.4,45.3L600.8,45.0L627.2,46.1L653.6,45.1L680.0,46.0L680.6,58.7L680.5,71.3L680.0,84.0L653.6,84.9L627.2,84.1L600.8,84.1L574.4,83.3L548.0,84.0L521.6,83.7L495.2,84.0L468.8,83.7L442.4,84.1L416.0,84.4L389.6,83.0L363.2,83.0L336.8,83.3L310.4,83.6L284.0,84.4L257.6,84.5L231.2,84.4L204.8,84.9L178.4,83.5L152.0,84.2L125.6,83.3L99.2,84.2L72.8,83.1L46.4,83.3L20.0,84.0L19.0,71.3L19.4,58.7L20.0,46.0Z"/>
      <path class="sketch" d="M20.0,46.0L46.4,47.1L72.8,45.9L99.2,47.2L125.6,45.7L152.0,44.9L178.4,46.3L204.8,46.7L231.2,45.4L257.6,44.9L284.0,45.6L310.4,47.2L336.8,46.7L363.2,45.0L389.6,45.3L416.0,45.0L442.4,44.9L468.8,46.8L495.2,45.2L521.6,46.2L548.0,45.9L574.4,45.2L600.8,46.6L627.2,45.0L653.6,46.4L680.0,46.0L681.0,58.7L680.2,71.3L680.0,84.0L653.6,84.7L627.2,84.6L600.8,82.8L574.4,83.2L548.0,84.5L521.6,83.0L495.2,84.8L468.8,84.3L442.4,83.1L416.0,83.6L389.6,85.0L363.2,82.7L336.8,84.7L310.4,84.6L284.0,83.3L257.6,84.4L231.2,84.5L204.8,85.1L178.4,85.1L152.0,83.8L125.6,84.7L99.2,83.7L72.8,84.3L46.4,84.1L20.0,84.0L21.2,71.3L20.0,58.7L20.0,46.0Z"/>
      <rect x="21" y="47" width="3.4" height="36" fill="#6741d9"/>
      <path d="M23,104 V94" stroke="#6741d9" stroke-width="1.5" fill="none"/>
      <path d="M23,86 L19,94 L27,94 Z" fill="#6741d9"/>

      <text class="hand-lg" x="20" y="34">one second of raw 1080p60 is 1.5 Gbit</text>
      <text class="hand" x="32" y="120" fill="#6741d9">6 Mbit, the whole delivered stream</text>
      <text class="hand" x="352" y="71" fill="#868e96" text-anchor="middle">video reconstructed by the decoder</text>
      <text class="hand" x="32" y="146" fill="#868e96">250 times smaller</text>
    </svg>
    <figcaption>Drawn to scale. The violet sliver at the left edge is a 6 Mbit/s stream against one
    second of its own uncompressed source.</figcaption>
  </figure>

## Four kinds of redundancy

All compression comes down to finding redundancy and not paying for it twice.
Every standardized codec chases the same four kinds.

<ul>
<li><strong>Spatial.</strong> Neighboring pixels look alike. Predict a block from pixels already
decoded above and to the left, then code only the difference.</li>
<li><strong>Temporal.</strong> Frame x+1 is mostly frame x but displaced. We record the motion
instead of the image.</li>
<li><strong>Statistical.</strong> After prediction, transform and quantization, most of
what's left is zeros. Common values get short codes and rare ones get long codes.</li>
<li><strong>Perceptual.</strong> Vision is far more sensitive to brightness than color, and to
gradients than to fine texture. Spend bits where they are seen.</li>
</ul>

<div class="aside">
<p class="aside-title">Where the loss actually happens</p>
<p>
The first three lose nothing. A motion vector is exact, the prediction error
left after it (the residual) is exact, and entropy coding is reversible.
Quantization is where data gets thrown away: it rounds the residual before
coding it.
</p>
<p>
The fourth kind is the interesting one because it rests on a judgment about
human vision. When two encoders differ at the same bitrate, part of the gap is
one predicting better than the other. The rest is that they disagree about
vision. Viewers settle that part. A
<a href="https://en.wikipedia.org/wiki/Mean_opinion_score">MOS</a> panel is how
you ask them, and <a href="#measuring-it">Measuring it</a> below covers MOS.
</p>
</div>

## The encode loop

The encoder codes a frame one block at a time. Every block goes around the
same loop.

  <div class="fig bleed">
    <header>
      <h4>Stepping through the encode loop</h4>
      <p class="look">After entropy coding, the block goes back around the loop.</p>
    </header>
    <div class="bd">
      <svg id="loopsvg" viewBox="0 0 700 262" width="100%" role="img" aria-label="The encode loop: predict, transform, quantize and entropy code, with a return path through inverse and reconstruct back to predict">
        <!-- forward path -->
        <g class="conn" fill="none" stroke="#868e96" stroke-width="2"><path d="M144.0,72.0L153.3,71.4L162.7,72.0L172.0,72.0"/> <path d="M298.0,72.0L307.3,72.5L316.7,72.6L326.0,72.0"/> <path d="M452.0,72.0L461.3,72.3L470.7,72.7L480.0,72.0"/></g>
        <g fill="#868e96">
          <path d="M172,67 L180,72 L172,77 Z"/><path d="M326,67 L334,72 L326,77 Z"/>
          <path d="M480,67 L488,72 L480,77 Z"/>
        </g>

        <!-- return path: down from quantize, left through reconstruct, back up
             into predict. It runs outside every box and under every label. -->
        <g class="conn" data-path="return" fill="none" stroke-width="2" stroke-dasharray="7 5"><path d="M392.0,99.0L392.4,116.7L392.5,134.3L392.0,152.0"/><path d="M332.0,185.0L322.7,185.1L313.3,185.4L304.0,185.0"/><path d="M178.0,185.0L146.7,185.5L115.3,185.1L84.0,185.0"/><path d="M84.0,185.0L84.2,158.7L84.0,132.3L84.0,106.0"/></g>
        <g data-path="return" class="arrow">
          <path d="M387,152 L392,160 L397,152 Z"/>
          <path d="M304,180 L296,185 L304,190 Z"/>
          <path d="M79,106 L84,98 L89,106 Z"/>
        </g>

        <g class="stage" data-stage="predict"><path class="box" d="M24.0,45.0L54.0,45.1L84.0,45.6L114.0,44.5L144.0,45.0L144.6,63.0L143.3,81.0L144.0,99.0L114.0,98.5L84.0,99.4L54.0,99.5L24.0,99.0L24.4,81.0L24.7,63.0L24.0,45.0Z"/><path class="box-sketch" d="M24.0,45.0L54.0,44.4L84.0,45.9L114.0,45.8L144.0,45.0L143.8,63.0L144.2,81.0L144.0,99.0L114.0,99.8L84.0,100.0L54.0,98.0L24.0,99.0L23.5,81.0L24.4,63.0L24.0,45.0Z"/><text x="84" y="77" text-anchor="middle">predict</text></g>
        <g class="stage" data-stage="transform"><path class="box" d="M178.0,45.0L208.0,44.6L238.0,45.5L268.0,45.2L298.0,45.0L298.3,63.0L298.5,81.0L298.0,99.0L268.0,98.6L238.0,99.7L208.0,99.4L178.0,99.0L178.1,81.0L178.6,63.0L178.0,45.0Z"/><path class="box-sketch" d="M178.0,45.0L208.0,45.2L238.0,44.5L268.0,45.9L298.0,45.0L298.6,63.0L299.0,81.0L298.0,99.0L268.0,99.5L238.0,99.1L208.0,99.9L178.0,99.0L177.3,81.0L177.7,63.0L178.0,45.0Z"/><text x="238" y="77" text-anchor="middle">transform</text></g>
        <g class="stage" data-stage="quantize"><path class="box" d="M332.0,45.0L362.0,45.1L392.0,44.4L422.0,44.8L452.0,45.0L451.4,63.0L451.2,81.0L452.0,99.0L422.0,98.7L392.0,98.7L362.0,98.9L332.0,99.0L331.4,81.0L331.3,63.0L332.0,45.0Z"/><path class="box-sketch" d="M332.0,45.0L362.0,44.0L392.0,45.9L422.0,45.4L452.0,45.0L451.0,63.0L453.0,81.0L452.0,99.0L422.0,98.7L392.0,99.0L362.0,98.5L332.0,99.0L331.6,81.0L333.0,63.0L332.0,45.0Z"/><text x="392" y="77" text-anchor="middle">quantize</text></g>
        <g class="stage" data-stage="entropy"><path class="box" d="M486.0,45.0L516.0,44.3L546.0,45.1L576.0,45.4L606.0,45.0L605.4,63.0L605.6,81.0L606.0,99.0L576.0,98.7L546.0,98.5L516.0,98.3L486.0,99.0L485.8,81.0L486.3,63.0L486.0,45.0Z"/><path class="box-sketch" d="M486.0,45.0L516.0,45.8L546.0,45.8L576.0,44.8L606.0,45.0L605.4,63.0L605.2,81.0L606.0,99.0L576.0,98.8L546.0,98.7L516.0,99.2L486.0,99.0L486.2,81.0L486.2,63.0L486.0,45.0Z"/><text x="546" y="70" text-anchor="middle">entropy</text><text x="546" y="86" text-anchor="middle">code</text></g>
        <g class="stage" data-stage="inverse"><path class="box" d="M332.0,158.0L362.0,157.3L392.0,158.2L422.0,158.8L452.0,158.0L451.4,176.0L451.6,194.0L452.0,212.0L422.0,212.2L392.0,211.6L362.0,211.9L332.0,212.0L331.9,194.0L332.5,176.0L332.0,158.0Z"/><path class="box-sketch" d="M332.0,158.0L362.0,157.1L392.0,158.5L422.0,157.0L452.0,158.0L451.8,176.0L452.0,194.0L452.0,212.0L422.0,212.6L392.0,211.6L362.0,212.0L332.0,212.0L332.2,194.0L332.9,176.0L332.0,158.0Z"/><text x="392" y="190" text-anchor="middle">inverse</text></g>
        <g class="stage" data-stage="reconstruct"><path class="box" d="M178.0,158.0L208.0,157.6L238.0,157.2L268.0,157.7L298.0,158.0L297.7,176.0L298.5,194.0L298.0,212.0L268.0,212.5L238.0,211.4L208.0,211.7L178.0,212.0L177.9,194.0L178.6,176.0L178.0,158.0Z"/><path class="box-sketch" d="M178.0,158.0L208.0,157.6L238.0,158.3L268.0,157.4L298.0,158.0L298.1,176.0L297.4,194.0L298.0,212.0L268.0,211.1L238.0,211.2L208.0,212.2L178.0,212.0L178.2,194.0L177.6,176.0L178.0,158.0Z"/><text x="238" y="183" text-anchor="middle">reconstruct</text><text x="238" y="199" text-anchor="middle">+ deblock</text></g>

        <text class="hand" x="26" y="34">source frame</text>
        <text class="hand" x="622" y="78">→ bits</text>
        <text class="hand" x="104" y="146">what the next frame predicts from</text>
        <text class="hand" x="104" y="240" fill="#868e96">an encoder contains a whole decoder</text>
      </svg>
      <div class="ctrls">
        <button id="lprev">‹ Back</button><button id="lnext">Next ›</button>
        <div class="dots" id="ldots"></div>
      </div>
      <p class="stagecap"><b id="lname"></b><span id="lcap"></span></p>
    </div>
  </div>

## Motion estimation

Repetition from frame to frame is the biggest saving on offer. The encoder asks
*where did this block go?* millions of times a second. It takes a block from
the frame it's coding, slides it around the previous frame and keeps the
position where the pixels differ least. The difference measure is the sum of
absolute differences, or SAD.

  <div class="fig bleed">
    <header>
      <h4>Stepping through a motion search</h4>
      <p class="look">The figure runs in three steps: the block and the window it may
      search, the search working through that window one position at a time,
      and the winner. Press <b>Step</b> to go one step at a time.</p>
    </header>
    <div class="bd">
      <div class="me">
        <div><h5>Previous frame</h5><canvas id="meref" role="img" aria-label="The previous video frame, with the region the search may look in"></canvas>
          <p class="cap">Dashed violet is everywhere the search is allowed to look. Orange is the
          position under test, then the winner.</p></div>
        <div><h5>This frame</h5><canvas id="mecur" role="img" aria-label="The current video frame, with the block being coded outlined"></canvas>
          <p class="cap">Solid violet is the block we have to code. On the last step the dashed gray
          box shows where its content sat a frame ago.</p></div>
      </div>
      <div class="me-small">
        <div><h5>Cost at every position tried</h5><canvas id="mecost" role="img" aria-label="A heat map of the match cost at every position the search tried"></canvas></div>
        <div><h5>What is left to code</h5><canvas id="meres" role="img" aria-label="The residual left to code, before and after the motion search"></canvas>
          <p class="cap"><b id="meres0">—</b> per pixel with no motion,
          <b id="meres1">—</b> after the search. Darker red is more to code.</p></div>
        <div class="me-hint">Every block in the frame moves by the same few pixels because the
        camera pans across a still image. That's the easiest case a search ever gets. Even so,
        it takes hundreds of comparisons for one block.</div>
      </div>
      <p class="phase" id="mephase"></p>
      <div class="ctrls">
        <button id="meplay">Pause</button><button id="mestep">Step ›</button>
        <label class="sw"><input id="meslow" type="checkbox"> half speed</label>
        <label class="sw" for="merange">Search range</label> <input id="merange" type="range" min="6" max="22" value="14" style="width:110px" aria-label="Search range, in pixels"> <b id="merangeout">±14</b>
        <label class="sw"><input id="memode" type="checkbox"> hexagon pattern, seeded from the neighbors</label>
      </div>
      <div class="nums">
        <div><span>motion vector</span><b id="memv">—</b></div>
        <div><span>SAD per pixel</span><b id="mesad">—</b></div>
        <div><span>positions tested</span><b id="metested">—</b></div>
        <div><span>versus exhaustive</span><b id="mepen">—</b></div>
      </div>
    </div>
  </div>

Dark violet on the cost map marks a good match. The basin around it is why a
shortcut like the hexagon pattern works. The cost changes smoothly enough that a
pattern walking downhill usually finds the same minimum as testing every
position. The percentage in the last box is what the shortcut costs you when it
doesn't. Choosing that trade is most of what a preset is.

## Quantization

Prediction, transform and entropy coding are all reversible. Quantization is the
one step that destroys information. [Rate control](#rate-control) below covers
how it works and the knob that sets it.

  <div class="fig bleed">
    <header>
      <h4>Quantizing a real frame</h4>
      <p class="look">Drag QP up until you can see the 8×8 grid. Watch the sky band:
      flat areas give way before the hair blurs.</p>
    </header>
    <div class="bd">
      <div class="quant">
        <div><canvas id="qcv" role="img" aria-label="A video frame, quantized live at the QP you choose"></canvas>
          <p class="credit">Sintel © Blender Foundation, CC BY 3.0, one frame from
          <code>tests/corpus/sintel_720p.y4m</code>. Your browser transforms, quantizes and inverts both
          luma and chroma. Chroma gets a coarser step, as in a real encoder. The
          transform here is the generic 8&times;8 DCT. Part two covers H.264's own
          4&times;4 and 8&times;8 transforms.</p>
        </div>
        <div><h5>4× detail</h5><canvas id="qzoom" role="img" aria-label="A four times detail crop of the quantized frame"></canvas></div>
      </div>
      <div class="ctrls"><span>QP <input id="qp" type="range" min="4" max="50" value="26" style="width:220px" aria-label="Quantizer, QP"> <b id="qpv">26</b></span></div>
      <div class="nums">
        <div><span>coefficients kept</span><b id="kept">—</b></div>
        <div><span>size</span><b id="bits">—</b></div>
        <div><span>compression</span><b id="ratio">—</b></div>
        <div><span>luma PSNR</span><b id="psnr">—</b></div>
      </div>
    </div>
  </div>

When compression is pushed too hard, the image breaks up into visible squares
along the transform's own grid. "Blocking" is one of the more common
artifacts viewers notice and complain about. Every codec adds a deblocking
filter inside the decoding loop because of it.

## The decision

So far every stage has had one obvious way to work. Past this point, encoding
is a choice. A block can be skipped, predicted with one motion vector, split into
four blocks with four vectors, or coded from scratch. The cheaper a choice is to
describe, the worse it usually looks. So the encoder prices both sides together
as distortion plus lambda times rate, `cost = D + lambda x R`. It picks the
option with the lowest cost.

Lambda is the price of a bit in that trade. When lambda is small, bits are cheap
and the encoder spends them to cut error. When it's large, bits are dear and the
encoder accepts more error to save them. Because the quantizer sets lambda,
that one number steers every decision in the encoder at once. Drag λ in the
figure to watch the trade move.

  <div class="fig bleed">
    <header>
      <h4>Costing four candidate modes</h4>
      <p class="look">D is measured on a real block. R is what each mode costs to describe. The winner
      changes under you as λ moves.</p>
    </header>
    <div class="bd rd">
      <div id="rdrows"></div>
      <div class="ctrls"><span>λ <input id="rdlam" type="range" min="0" max="120" value="30" style="width:220px" aria-label="Lambda, the exchange rate between quality and bits"> <b id="rdlamout">30</b></span></div>
      <svg id="rdchart" viewBox="0 0 580 200" width="100%" style="display:block;overflow:visible;margin-top:10px" role="img" aria-label="Cost against lambda for the four candidate modes, with the winner highlighted"></svg>
    </div>
  </div>

<div class="aside">
<p class="aside-title">Where the encoders differ</p>
<p>Every encoder computes that same cost. Three things separate them.</p>
<ul>
<li>Which candidates they decide to try.</li>
<li>How accurately they guess what a block will cost in bits, before they have
actually coded it.</li>
<li>Whether the quality-against-bits trade stays fixed for a whole frame or
shifts with what is coming next.</li>
</ul>
</div>

## The constants

Every encoder has constants that someone tuned by hand. The lambda law
behind the decision above traces to the Lagrange-multiplier work of the late
1990s and early 2000s. Its authors swept lambda against QP on the test clips of
the day and fit a curve through the pairs that won. Every H.264-era encoder,
ours included, inherited that fit. The constants around it were swept once on
small sets of clips and then frozen.

The usual assumption is that one setting suits all content. To test it, we
flipped six settings we already ship one at a time across the whole test set
and read the results clip by clip. Each setting was best for some clips and
wrong for others.

The average over the test set hides this because a talking head and a handheld
street scene want different settings. Picking the right setting for each clip
would save about 1.3% on our twelve clips.

The encoder doesn't pick per clip yet. The closest it comes is a check that
spots flat, cartoon-like frames and raises the psychovisual strength for them.
It fires on only one clip in the test set.

A cheap measurement that predicts the right setting is hard to find. We labeled
239 clips from an outside set and tested four features. None of them predicts.
Most clips probably have nothing to win anyway. On that set, a tenth of the
clips held most of the gain.

<div class="aside">
<p class="aside-title">Two ways to fool yourself</p>
<p>Two mistakes make an encoder measure better than it is. One is tuning on
the clips you test with. Tune on a different set of clips and a result can move
by ten percent. That's why anything we fit is trained on other people's video
and tested on ours. The other is comparing at the same setting instead of the
same size. A change that only shifts how a quality setting maps to bits produces
a bigger file with better scores and passes for an improvement. For that reason
every comparison here is made at the same achieved bitrate.</p>
</div>

## Rate control

Quantization divides each transform coefficient by a step size and rounds it.
A bigger step means fewer bits and more loss.

The step can change per block. Rate control picks it thousands of times a
second. It has to hit a bitrate target without seeing far enough ahead to plan
for it. Viewers never see rate control working, only its failures: a
blurry face or a video that stalls.

Because an encoder can't fix quality and bitrate at the same time, you pick one
and let the other adjust. A talking head sitting still is cheap to compress. A detailed
action scene is expensive. Lock in the quality and the bitrate jumps around with
what is on screen. Lock in the bitrate and the quality takes the hit instead.
Each mode below chooses which one to let go.

### Constant QP

QP sets the quantization step. In H.264 it runs from 0 to 51. Each rise of six
doubles the step size.

Constant QP sets one QP and never changes it. Quality stays even. The bitrate
follows the content and runs up to five times higher in the action than in the
calm. It's rarely used for delivery, although encoder experiments get measured
with it.

### CRF

CRF holds *perceived* quality roughly steady and lets the bitrate go where it
must. QP still moves with the content. It moves much less than the complexity
does because the eye can't follow detail in fast motion.

That's why CRF is the right default whenever file size isn't capped. Because
constant QP holds the quantizer steady instead of the look, it overspends on
frames nobody is scrutinizing and starves the still ones where errors show.
Single-pass ABR chases a bitrate number through a feedback loop. Its quality
wobbles most at the scene changes a viewer is likeliest to notice. CRF chases
nothing and pays what each scene actually costs. At the same bits, that buys a
steadier picture than single-pass ABR. Two-pass also reaches a steady picture
for the price of a second pass. Use it when you need a known output size.

CRF gives you no say over how big the file comes out. The next mode adds that.

### Capped CRF

Capped CRF puts a buffer ceiling on top of a quality target. It's a common
choice for VOD ladders. Quality leads: an easy title codes cheaply and comes out
small. The cap only ever takes bits away. Below it you get exactly the CRF
encode you asked for, bit for bit. Above it the frame is bounded and stays
deliverable.

You get constant quality's cheapness on the easy half of a catalog and a buffer
constraint's safety on the hard half. Single-pass ABR is now rare in
on-demand work.

### Average bitrate

ABR must hit a number over the whole file. It runs a feedback controller that
tightens when it has overspent so far and relaxes when it has underspent.
Feedback alone only learns about a hard section *after* paying for its first
frames. The controller overshoots into the cut and over-corrects afterward.
Quality lurches on either side of the scene change. That lurch is the
rate-control failure viewers notice.

A lookahead fixes this. The encoder keeps a queue of the next few dozen frames
and studies them before coding. It learns where the scene cuts are, which
stretches will be expensive and which blocks later frames will copy from. With
that it picks frame types, holds bits back for a spike and spends extra where it
pays off.

Two-pass fixes it another way. A first pass encodes the whole file only to learn
how hard each part is. Because the second pass plans the budget knowing the
whole file, the total comes out exact and no scene catches it off guard.
Because two-pass needs a second encode and an input you can seek in, it can't
run live.

### VBV

A decoder reads from a buffer that fills at the channel rate and empties by one
frame's worth each time a frame is decoded. If a frame needs more bits than the
buffer holds at that moment, playback stalls. VBV makes that constraint explicit:
no frame may take more than the buffer holds when its turn comes. A big frame is
allowed only if the frames before it left room. The frames after it then have to
stay small until the buffer refills. So the cap is on a run of frames as long as
the buffer, usually one to two seconds, and not on each frame alone.

Through a hard section the quality dips instead of the stream breaking.
Broadcast profiles require VBV. Most adaptive-streaming authoring specs ask for
it too. That's why a live encode of a hard scene looks worse than the same scene
encoded offline.

  <div class="fig bleed">
    <header>
      <h4>Rate control comparison</h4>
      <p class="look">Every mode codes the same 160 frames, the same complexity in
      each: an action scene between two calm ones. Watch what each policy
      chooses to let move.</p>
    </header>
    <div class="bd">
      <div class="rc-modes" id="rcmodes">
        <button data-mode="cqp">Constant QP</button>
        <button data-mode="crf">CRF</button>
        <button data-mode="abr">ABR</button>
        <button data-mode="ccrf">Capped CRF</button>
        <button data-mode="vbv">VBV-capped</button>
      </div>
      <svg id="rcchart" viewBox="0 0 660 340" width="100%" style="display:block;overflow:visible" role="img" aria-label="Bits per frame, QP and buffer fullness across 160 frames for the selected rate control mode"></svg>
      <div class="ctrls">
        <span>Target <input id="rctarget" type="range" min="8" max="40" value="18" style="width:150px" aria-label="Target bits per frame"> <b id="rctargetout"></b> per frame</span>
        <label class="sw"><input id="rclook" type="checkbox" checked> lookahead (40 frames)</label>
      </div>
      <p class="verdict" id="rcverdict"></p>
      <div class="nums">
        <div><span>total size</span><b id="rcsize">—</b></div>
        <div><span>mean QP</span><b id="rcqp">—</b></div>
        <div><span>QP swing</span><b id="rcswing">—</b></div>
        <div><span>buffer</span><b id="rcvbv">—</b></div>
      </div>
    </div>
  </div>


### VBV and HRD

People use the two names for each other, although they mean different things.

The hypothetical reference decoder (HRD) is the model in the H.264
specification: an idealized decoder with a buffer in front of it. Conformance
is defined against that model. A stream conforms when it never overflows or
underflows the buffer. The model's parameters can also be written into the
sequence header to tell a real decoder what buffering to expect.

The video buffering verifier (VBV) is the encoder side of the same constraint.
It's the buffer simulation a rate controller runs while encoding so that it
never emits a frame the model couldn't decode. The name is MPEG-1 and MPEG-2's
own term for their buffer model. H.264 specifies its own model instead: the
HRD, with a coded picture buffer at its front. Encoders kept the older name for
the option.

The two describe one constraint from opposite ends: VBV is what you configure
and HRD is what the decoder cares about.

yah264 and x264 both write HRD parameters into the sequence header only when you
ask for them with `--nal-hrd`. A stream constrained by `--vbv-maxrate` and
`--vbv-bufsize` alone has no signaled buffer. For most delivery nobody notices.
Broadcast profiles that require signaled buffering, such as Blu-ray and ATSC,
reject a stream that has none even when the encode itself was properly
constrained.

## Measuring it

The only real measure of video quality is a person watching it. The ground
truth in this field is the mean opinion score, or MOS. A panel of viewers is seated
in a controlled room and shown clips in a randomized order. Each viewer rates
what they saw from 1 (bad) to 5 (excellent). The average of those scores is the
MOS for that clip at that bitrate.
[ITU-R BT.500](https://www.itu.int/rec/R-REC-BT.500) and ITU-T P.910
standardize the procedure down to the room lighting and the viewing distance.

MOS is also slow, expensive, and impossible to put in a build. Every metric we
use stands in for it by predicting what viewers would say without asking them.
PSNR measures squared error. It's cheap and only loosely tracks what viewers
report. SSIM compares local structure and does better.
[VMAF](https://en.wikipedia.org/wiki/Video_Multimethod_Assessment_Fusion) is a
model trained on MOS scores to predict them. It has become the standard the
streaming industry judges quality by.

VMAF is the metric this project runs on. We read every comparison against x264,
every tuning decision and every quality gate on it. We use its NEG variant so
that sharpening tricks can't inflate the score. PSNR stays underneath as a
floor. A change can't buy VMAF by making the picture less accurate.

<div class="aside" id="bd-rate">
<p class="aside-title">BD-rate</p>
<p>BD-rate compares two encoders in one number. It's the average difference in
the bits they need for the same quality over a range of bitrates. A BD-rate of
&minus;5% means an encoder reached the same quality as the one it's measured
against while spending 5% fewer bits.</p>
</div>

## On to H.264

[Part two](how-h264-works.html) covers what H.264 adds: macroblocks, intra
modes, transforms, CABAC, deblocking and the rate-control knobs.

<script src="assets/_frame.js"></script>
<script src="assets/_figs.js"></script>
<script>
initLoopFig({svg:'loopsvg',caption:'lcap',name:'lname',dots:'ldots',prev:'lprev',next:'lnext'});
initMeFig({refCanvas:'meref',curCanvas:'mecur',costCanvas:'mecost',resCanvas:'meres',
  range:'merange',play:'meplay',step:'mestep',slow:'meslow',mode:'memode',
  mvOut:'memv',sadOut:'mesad',testedOut:'metested',rangeOut:'merangeout',
  penaltyOut:'mepen',phaseOut:'mephase',res0Out:'meres0',res1Out:'meres1'});
initQuantReal({canvas:'qcv',zoom:'qzoom',zoomX:96,zoomY:60,slider:'qp',qpOut:'qpv',
  keptOut:'kept',bitsOut:'bits',psnrOut:'psnr',ratioOut:'ratio'});
initRdFig({lambda:'rdlam',lambdaOut:'rdlamout',rows:'rdrows',chart:'rdchart'});
initRcFig({chart:'rcchart',modes:'rcmodes',target:'rctarget',targetOut:'rctargetout',look:'rclook',
  sizeOut:'rcsize',qpMeanOut:'rcqp',qpSwingOut:'rcswing',vbvOut:'rcvbv',verdictOut:'rcverdict'});
</script>
