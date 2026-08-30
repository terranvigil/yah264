---
title: How video encoding works - yah264
description: The fundamentals every codec shares, with live figures. Part one, before any H.264.
toc_label: Part one, fundamentals
---

<p class="kicker">Part one, common to every codec</p>

# How video encoding works

<p class="standfirst">Most video codecs are built out of the same handful of
building blocks. This part covers those so that
<a href="how-h264-works.html">part two</a> can focus on the H.264 specific
coding tools.</p>

## The bit budget

A second of uncompressed 1080p60 video is about **187 megabytes**. A good quality stream
of the same second is about 6 Mbit. So the encoder discards **99.6%** of the bits without the viewer noticing. What follows are the methods for choosing what to discard.

  <figure>
    <svg viewBox="0 0 700 158" width="100%" role="img" aria-label="A bar for one second of raw video, with the delivered stream as a sliver at its left edge">
      <defs>
        <!-- The bar was an empty outline, which reads as absence when it is the
             thing the whole figure is about. Hatching it in the accent ties it
             to the solid sliver: same colour, one filled and one only ruled, so
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

All compression comes down to finding redundancy and not paying for it twice. There are four kinds worth finding. Every standardised codec chases the same four.

<ul>
<li><strong>Spatial.</strong> Neighbouring pixels look alike. Predict a block from pixels already
decoded above and to the left and then code only the difference.</li>
<li><strong>Temporal.</strong> Frame x+1 is mostly frame x but displaced. We record the motion
instead of the image.</li>
<li><strong>Statistical.</strong> After prediction, most of what's left are zeros. Common values are assigned short codes, rare ones get long codes.</li>
<li><strong>Perceptual.</strong> Vision weighs brightness over colour and gradients over texture
detail. Spend bits where they are seen.</li>
</ul>

<div class="aside">
<p class="aside-title">Where the loss actually happens</p>
<p>
The first three don't lose anything. A residual is exact, a motion vector is exact, and entropy coding is reversible. The step that actually throws data away is quantisation - the rounding off the residual before coding it.

So the fourth bullet is the interesting one. Its based on judgement about human vision. Two encoders at the same bitrate are partly disagreeing about it and partly just predicting better or worse than each other. The perceptual half can't be settled by argument. You settle it by asking viewers which is what a MOS panel does. We will get to MOS later.
</p>
</div>

## The encode loop

Encoding happens with blocks of video. Every block goes around one loop. Let's step through it.

  <div class="fig bleed">
    <header>
      <h4>Stepping through the encode loop</h4>
      <p class="look">After entropy coding, the block isn't done. It goes back around.</p>
    </header>
    <div class="bd">
      <svg id="loopsvg" viewBox="0 0 700 262" width="100%" role="img" aria-label="The encode loop: predict, transform, quantise and entropy code, with a return path through inverse and reconstruct back to predict">
        <!-- forward path -->
        <g class="conn" fill="none" stroke="#868e96" stroke-width="2"><path d="M144.0,72.0L153.3,71.4L162.7,72.0L172.0,72.0"/> <path d="M298.0,72.0L307.3,72.5L316.7,72.6L326.0,72.0"/> <path d="M452.0,72.0L461.3,72.3L470.7,72.7L480.0,72.0"/></g>
        <g fill="#868e96">
          <path d="M172,67 L180,72 L172,77 Z"/><path d="M326,67 L334,72 L326,77 Z"/>
          <path d="M480,67 L488,72 L480,77 Z"/>
        </g>

        <!-- return path: down from quantise, left through reconstruct, back up
             into predict. It runs outside every box and under every label. -->
        <g class="conn" data-path="return" fill="none" stroke-width="2" stroke-dasharray="7 5"><path d="M392.0,99.0L392.4,116.7L392.5,134.3L392.0,152.0"/><path d="M332.0,185.0L322.7,185.1L313.3,185.4L304.0,185.0"/><path d="M178.0,185.0L146.7,185.5L115.3,185.1L84.0,185.0"/><path d="M84.0,185.0L84.2,158.7L84.0,132.3L84.0,106.0"/></g>
        <g data-path="return" class="arrow">
          <path d="M387,152 L392,160 L397,152 Z"/>
          <path d="M304,180 L296,185 L304,190 Z"/>
          <path d="M79,106 L84,98 L89,106 Z"/>
        </g>

        <g class="stage" data-stage="predict"><path class="box" d="M24.0,45.0L54.0,45.1L84.0,45.6L114.0,44.5L144.0,45.0L144.6,63.0L143.3,81.0L144.0,99.0L114.0,98.5L84.0,99.4L54.0,99.5L24.0,99.0L24.4,81.0L24.7,63.0L24.0,45.0Z"/><path class="box-sketch" d="M24.0,45.0L54.0,44.4L84.0,45.9L114.0,45.8L144.0,45.0L143.8,63.0L144.2,81.0L144.0,99.0L114.0,99.8L84.0,100.0L54.0,98.0L24.0,99.0L23.5,81.0L24.4,63.0L24.0,45.0Z"/><text x="84" y="77" text-anchor="middle">predict</text></g>
        <g class="stage" data-stage="transform"><path class="box" d="M178.0,45.0L208.0,44.6L238.0,45.5L268.0,45.2L298.0,45.0L298.3,63.0L298.5,81.0L298.0,99.0L268.0,98.6L238.0,99.7L208.0,99.4L178.0,99.0L178.1,81.0L178.6,63.0L178.0,45.0Z"/><path class="box-sketch" d="M178.0,45.0L208.0,45.2L238.0,44.5L268.0,45.9L298.0,45.0L298.6,63.0L299.0,81.0L298.0,99.0L268.0,99.5L238.0,99.1L208.0,99.9L178.0,99.0L177.3,81.0L177.7,63.0L178.0,45.0Z"/><text x="238" y="77" text-anchor="middle">transform</text></g>
        <g class="stage" data-stage="quantise"><path class="box" d="M332.0,45.0L362.0,45.1L392.0,44.4L422.0,44.8L452.0,45.0L451.4,63.0L451.2,81.0L452.0,99.0L422.0,98.7L392.0,98.7L362.0,98.9L332.0,99.0L331.4,81.0L331.3,63.0L332.0,45.0Z"/><path class="box-sketch" d="M332.0,45.0L362.0,44.0L392.0,45.9L422.0,45.4L452.0,45.0L451.0,63.0L453.0,81.0L452.0,99.0L422.0,98.7L392.0,99.0L362.0,98.5L332.0,99.0L331.6,81.0L333.0,63.0L332.0,45.0Z"/><text x="392" y="77" text-anchor="middle">quantise</text></g>
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

Temporal redundancy is the biggest single win. And we need to calculate it millions of times a second. *Where did this block go?* The encoder
takes a block from the frame it is coding, slides it around the previous frame,
and keeps the position where the pixels differ least. That difference measure is
a `SAD`, the sum of absolute differences.

  <div class="fig bleed">
    <header>
      <h4>Stepping through a motion search</h4>
      <p class="look">Three beats: the block and its window, then the search revealing itself position
      by position, then the winner. Press <b>Step</b> to take it one beat at a time.</p>
    </header>
    <div class="bd">
      <div class="me">
        <div><h5>Previous frame</h5><canvas id="meref" role="img" aria-label="The previous video frame, with the region the search may look in"></canvas>
          <p class="cap">Dashed violet is everywhere the search is allowed to look. Orange is the
          position under test, and then the winner.</p></div>
        <div><h5>This frame</h5><canvas id="mecur" role="img" aria-label="The current video frame, with the block being coded outlined"></canvas>
          <p class="cap">Solid violet is the block we have to code. On the last beat the dashed grey
          box shows where its content sat a frame ago.</p></div>
      </div>
      <div class="me-small">
        <div><h5>Cost at every position tried</h5><canvas id="mecost" role="img" aria-label="A heat map of the match cost at every position the search tried"></canvas></div>
        <div><h5>What is left to code</h5><canvas id="meres" role="img" aria-label="The residual left to code, before and after the motion search"></canvas>
          <p class="cap"><b id="meres0">—</b> per pixel with no motion,
          <b id="meres1">—</b> after the search. Darker red is more to code.</p></div>
        <div class="me-hint">The camera is panning across a still, so every block in the frame has
        moved by the same few pixels. That is the easiest case a search ever gets, and it is still
        hundreds of comparisons for one block.</div>
      </div>
      <p class="phase" id="mephase"></p>
      <div class="ctrls">
        <button id="meplay">Pause</button><button id="mestep">Step ›</button>
        <label class="sw"><input id="meslow" type="checkbox"> half speed</label>
        <label class="sw" for="merange">Search range</label> <input id="merange" type="range" min="6" max="22" value="14" style="width:110px" aria-label="Search range, in pixels"> <b id="merangeout">±14</b></span>
        <label class="sw"><input id="memode" type="checkbox"> hexagon pattern, seeded from the neighbours</label>
      </div>
      <div class="nums">
        <div><span>motion vector</span><b id="memv">—</b></div>
        <div><span>SAD per pixel</span><b id="mesad">—</b></div>
        <div><span>positions tested</span><b id="metested">—</b></div>
        <div><span>versus exhaustive</span><b id="mepen">—</b></div>
      </div>
    </div>
  </div>

Dark violet on the cost map is a good match, and the basin around it is why the
shortcut works. The surface is smooth enough that a pattern which walks downhill
usually finds the same minimum as testing everything. Usually. The percentage in
the last box is what the shortcut costs you when it does not, and choosing that
trade is most of what a preset is.

## Quantisation

Prediction, transform and entropy coding are all reversible. Exactly one step
destroys information, and that is **quantisation**. It divides every transform
coefficient by a step size and rounds. `QP` selects that step, and the step grows
geometrically with it: in H.264 it doubles every six QP.

  <div class="fig bleed">
    <header>
      <h4>Quantising a real frame</h4>
      <p class="look">Drag QP up until you can see the 8×8 grid. Watch the sky band before the hair
      blurs, because flat areas give way first.</p>
    </header>
    <div class="bd">
      <div class="quant">
        <div><canvas id="qcv" role="img" aria-label="A video frame, quantised live at the QP you choose"></canvas>
          <p class="credit">Sintel © Blender Foundation, CC BY 3.0, one frame from
          <code>tests/corpus/sintel_720p.y4m</code>. Luma and chroma are transformed, quantised and
          inverted in your browser. Chroma gets a coarser step, as in a real encoder.</p>
        </div>
        <div><h5>4× detail</h5><canvas id="qzoom" role="img" aria-label="A four times detail crop of the quantised frame"></canvas></div>
      </div>
      <div class="ctrls"><span>QP <input id="qp" type="range" min="4" max="50" value="26" style="width:220px" aria-label="Quantiser, QP"> <b id="qpv">26</b></span></div>
      <div class="nums">
        <div><span>coefficients kept</span><b id="kept">—</b></div>
        <div><span>size</span><b id="bits">—</b></div>
        <div><span>compression</span><b id="ratio">—</b></div>
        <div><span>luma PSNR</span><b id="psnr">—</b></div>
      </div>
    </div>
  </div>

It fails in squares, on whatever grid the transform uses. This figure runs an
8&times;8 transform, H.264 codes 4&times;4 by default with 8&times;8 available in
High profile, and later formats use several sizes. Blocking is quantisation
showing through the transform's seams, which is why every codec since puts a
deblocking filter inside the loop.

## The decision

So far every stage has had one obvious way to do it. Real encoding is a choice.
This block could be skipped, predicted with one motion vector, split into four
with four vectors, or coded from scratch. Cheaper to describe usually means
worse to look at, so the encoder prices both together as `cost = D + lambda x R`,
distortion plus lambda times rate, and takes the smallest.

**Lambda is the exchange rate between quality and bits**, and moving it moves
every decision in the encoder at once. Drag it.

  <div class="fig bleed">
    <header>
      <h4>Costing four candidate modes</h4>
      <p class="look">D is measured on a real block; R is what each mode costs to describe. The winner
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
<p>Every encoder computes that same cost. What separates them is which
candidates they bother to try, how honestly they estimate R before the entropy
coder has run, and whether lambda is a single number for the frame or varies
with what the lookahead has seen. That last one is where the differences between encoders
mostly live.</p>
</div>

## Rate control

Quantisation hands you one knob per block. **Rate control is the policy that
turns it**, thousands of times a second, without being able to see the future,
against a target it is not allowed to miss. It is the part of an encoder users
feel most directly and understand least. Its failures do not look like
rate-control failures. They look like a blurry face, or a stall.

The difficulty is that the two things you might want to hold constant, quality
and bitrate, cannot both be held. Content varies, and a static interview and an
explosion do not cost the same to code well. Fix the quality and the bitrate must
move. Fix the bitrate and the quality must move. Every mode below is a different
answer to which of those you are willing to let go.

### Constant QP

Set one QP and never change it. Quality is even across the whole clip and the
bitrate goes wherever the content puts it, which for a mixed sequence can be a
factor of five between the calm and the action. Nobody ships this. It is the only
mode that isolates a coding change from the rate controller's reaction to it, so
it is what almost every encoder experiment is measured in.

### Constant quality, or CRF

Hold *perceived* quality roughly steady, the MOS a viewer would give it, and let
the bitrate go where it must. The QP still moves with content, just far less than
the complexity does, because spending proportionally on a busy frame is wasted:
the eye cannot audit detail that is moving quickly.

### Capped CRF, which is what most streaming actually runs

Put a buffer ceiling on top of a quality target and you get the mode a VOD
library or an adaptive ladder is almost certainly using. Quality drives the
encode, so an easy title codes cheaply and comes out small. The cap is
one-sided: it can only take bits away, never add them, so wherever the ceiling is
slack the output is exactly the CRF encode you asked for, bit for bit, and where
the content runs into the ceiling it gets bounded instead of becoming
undeliverable.

It gets you the cheapness of constant quality on the easy half of a catalogue
and the safety of a buffer constraint on the hard half, which is why it displaced
plain ABR for most on-demand work.

### Average bitrate, and why lookahead matters

ABR must hit a number over the whole file, so it runs a feedback controller.
Overspent so far, tighten. Underspent, relax. The trouble with feedback alone is
that it only learns about a hard section *after* paying for the first frames of
it. It overshoots into the cut, over-corrects afterwards, and the quality lurches
either side of a scene change. It is the most visible rate-control artefact
there is.

A **lookahead** fixes this by buffering the next few dozen frames, measuring
roughly what they will cost, and adjusting *before* the cut. Everything good
downstream depends on having that window: sensible frame types, a bit budget that
anticipates, per-block lambda that knows which blocks later frames will predict
from.

### Buffer-constrained, or VBV

A decoder reads from a buffer that fills at the channel rate and drains one frame
at a time. If the encoder ever produces a frame larger than what is in the
buffer, playback stalls. VBV makes that constraint explicit and caps every frame
to what the buffer can carry, so quality dips through hard sections instead of
the stream breaking. It is mandatory for broadcast and for adaptive streaming,
and it is why a live encode of a hard scene looks worse than the same scene
encoded offline.

  <div class="fig bleed">
    <header>
      <h4>Rate control comparison</h4>
      <p class="look">Same 160 frames, same complexity curve, with a hard action section between two
      cuts. Watch what each policy chooses to let move.</p>
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

<div class="aside">
<p class="aside-title">Where our own work sits</p>
<p>Every encoder implements these modes. What differs is how well the lookahead
spends, and whether it can tell that a quiet block will be predicted from for the
next fifty frames and is therefore worth protecting. That is where most of
yah264's quality difference has come from, and the
<a href="design.html">design page</a> has the mechanism.</p>
</div>

## Measuring it

The only real measure of video quality is a person watching it. That is not a
figure of speech. The ground truth in this field is a **MOS**, a mean opinion
score. You seat a panel of viewers in a controlled room, show them clips in a
randomised order, and ask each to rate what they saw from 1 (bad) to 5
(excellent). Average the scores and you have the MOS for that clip at that
bitrate. The procedure is standardised, down to the room lighting and the viewing
distance, by [ITU-R BT.500](https://www.itu.int/rec/R-REC-BT.500) and ITU-T P.910.

MOS is also slow, expensive, and impossible to put in a build. So every metric we
actually use is an attempt to *predict* a MOS without convening the panel.
`PSNR` measures squared error, which is cheap and correlates only loosely with
what viewers say. `SSIM` compares local structure and does better. [`VMAF`](https://netflixtechblog.com/toward-a-practical-perceptual-video-quality-metric-653f208b9652) is a
model trained directly on MOS data to predict those scores, which makes it the
closest thing to useful. It also inherits the biases of the content its panels
were shown. When a metric and your eyes disagree, your eyes are the appeal court,
because they are what the metric was built to imitate.

Comparing two encoders takes a curve, because rate and quality trade against each
other. **BD-rate** integrates the gap between two rate/quality curves, with
quality measured by a MOS predictor such as VMAF, so &minus;5% means the same
predicted quality for 5% fewer bits *across the measured range*. Two cautions go
with it: a BD-rate figure is only as good as the metric underneath it, and it is
half a comparison. The other half is what it cost in wall-clock time, and the two
belong together.

## Next

[Part two covers what H.264 adds](how-h264-works.html): macroblocks, intra
modes, transforms, CABAC, deblocking, and the rate-control knobs.

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
