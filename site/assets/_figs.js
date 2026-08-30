// Interactive figures for the yah264 fundamentals page.
// Every number on the page is computed here, in the reader's browser.
//
// Pacing rules, after reading the research on explanatory animation:
//   - one beat is 1.5-3s, never 30 frames a second
//   - reveal a process progressively; never show only its answer
//   - the reader can always pause, step, or slow it down
(function () {

  // ================= shared =================
  const D = id => document.getElementById(id);
  const set = (id, v) => { const e = D(id); if (e) e.textContent = v; };

  const C = [];
  for (let u = 0; u < 8; u++) {
    C[u] = [];
    for (let x = 0; x < 8; x++)
      C[u][x] = (u === 0 ? Math.SQRT1_2 : 1) * Math.cos(((2 * x + 1) * u * Math.PI) / 16) / 2;
  }
  function dct8(b, o) {
    const t = new Float32Array(64);
    for (let y = 0; y < 8; y++) for (let u = 0; u < 8; u++) {
      let s = 0; for (let x = 0; x < 8; x++) s += C[u][x] * b[y * 8 + x]; t[y * 8 + u] = s;
    }
    for (let u = 0; u < 8; u++) for (let v = 0; v < 8; v++) {
      let s = 0; for (let y = 0; y < 8; y++) s += C[v][y] * t[y * 8 + u]; o[v * 8 + u] = s;
    }
  }
  function idct8(c, o) {
    const t = new Float32Array(64);
    for (let v = 0; v < 8; v++) for (let y = 0; y < 8; y++) {
      let s = 0; for (let u = 0; u < 8; u++) s += C[u][y] * c[v * 8 + u]; t[v * 8 + y] = s;
    }
    for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) {
      let s = 0; for (let v = 0; v < 8; v++) s += C[v][x] * t[v * 8 + y]; o[x * 8 + y] = s;
    }
  }
  const qstep = qp => 0.85 * Math.pow(2, (qp - 4) / 6);

  // synthetic scene for the motion figures: one object crossing a textured field
  const scene = (t, w, h, buf) => {
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++)
      buf[y * w + x] = 52 + 90 * (x / w) + 26 * (y / h) + 5 * Math.sin(x * .55) * Math.cos(y * .42);
    const disc = (cx, cy, r, val) => {
      for (let y = Math.max(0, cy - r | 0); y < Math.min(h, cy + r + 1); y++)
        for (let x = Math.max(0, cx - r | 0); x < Math.min(w, cx + r + 1); x++) {
          const d = Math.hypot(x - cx, y - cy);
          if (d < r) buf[y * w + x] = val - 26 * (d / r);
        }
    };
    const box = (bx, by, bw, bh, val, grain) => {
      for (let y = Math.max(0, by | 0); y < Math.min(h, by + bh); y++)
        for (let x = Math.max(0, bx | 0); x < Math.min(w, bx + bw); x++)
          buf[y * w + x] = val + (grain ? ((x * 7 + y * 13) % 11) * 4 : 0);
    };
    const swing = w - 48, ph = (t * 7.5) % (2 * swing);
    disc(18 + (ph < swing ? ph : 2 * swing - ph), h * .30, 10, 232);
    box(w * .10 + (t * 2.6) % (w * .40), h * .60, 20, 16, 34, false);
    box(w * .64, h * .66 + Math.sin(t * .08) * 2, 22, 18, 150, true);
    for (let i = 0; i < w * h; i++) buf[i] = Math.max(0, Math.min(255, buf[i]));
  };
  const discX = (t, w) => {
    const swing = w - 48, ph = (t * 7.5) % (2 * swing);
    return 18 + (ph < swing ? ph : 2 * swing - ph);
  };

  function grayPaint(ctx, buf, w, h, scale) {
    const off = document.createElement('canvas');
    off.width = w; off.height = h;
    const octx = off.getContext('2d'), img = octx.createImageData(w, h);
    for (let i = 0; i < w * h; i++) {
      const v = buf[i] | 0;
      img.data[i * 4] = v; img.data[i * 4 + 1] = v; img.data[i * 4 + 2] = v; img.data[i * 4 + 3] = 255;
    }
    octx.putImageData(img, 0, 0);
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(off, 0, 0, w * scale, h * scale);
  }
  const sad = (cur, ref, w, bx, by, dx, dy, n) => {
    let s = 0;
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++)
      s += Math.abs(cur[(by + y) * w + bx + x] - ref[(by + y + dy) * w + bx + x + dx]);
    return s;
  };

  // ============================================================
  // 1. The loop, one stage at a time
  // ============================================================
  const STAGES = [
    ['predict', 'Guess the block from what the decoder already knows: nearby pixels, or a matching patch in an earlier frame. Good guesses do most of the work.'],
    ['transform', 'Subtract the guess and transform what is left. The residual concentrates into a few low-frequency coefficients. A flat block collapses almost entirely into one.'],
    ['quantise', "Divide each coefficient by a step and round. It's the only lossy step. The QP value controls how much information we can throw away during compression."],
    ['entropy', 'Code the surviving numbers against an adaptive probability model. Common values cost fractions of a bit.'],
    ['inverse', 'Now undo it. Rescale and invert the transform, so you see what the decoder will actually receive.'],
    ['reconstruct', "Add the prediction back to what's left, smooth out the seams between blocks, and keep the result. Later frames get predicted from this reconstructed copy, not the original, because that copy is all the decoder will ever have. Which means every encoder is quietly running a decoder inside itself."],
  ];
  window.initLoopFig = function (o) {
    const svg = D(o.svg), dots = D(o.dots);
    let i = 0;
    dots.innerHTML = STAGES.map((s, k) => `<button data-k="${k}" aria-label="${s[0]}"></button>`).join('');
    function draw() {
      STAGES.forEach((s, k) => {
        const g = svg.querySelector('[data-stage="' + s[0] + '"]');
        if (g) g.classList.toggle('lit', k === i);
      });
      // both the dashed segments and their arrowheads carry data-path=return
      svg.querySelectorAll('[data-path="return"]').forEach(g => g.classList.toggle('lit', i >= 4));
      set(o.name, STAGES[i][0]); set(o.caption, STAGES[i][1]);
      [...dots.children].forEach((b, k) => b.classList.toggle('on', k === i));
    }
    const go = k => { i = (k + STAGES.length) % STAGES.length; draw(); };
    D(o.prev).onclick = () => go(i - 1);
    D(o.next).onclick = () => go(i + 1);
    dots.onclick = e => { if (e.target.dataset.k) go(+e.target.dataset.k); };
    svg.querySelectorAll('[data-stage]').forEach(g =>
      g.addEventListener('click', () => go(STAGES.findIndex(s => s[0] === g.dataset.stage))));
    draw();
  };

  // ============================================================
  // 2. Motion estimation, on the real frame.
  //    A camera pan over the Sintel still: the reference and current frames
  //    are two windows onto the same picture, a few pixels apart. Real
  //    detail, real texture, and a search that has to find a real vector.
  //    Labels live in the HTML, never on top of the picture.
  // ============================================================
  window.initMeFig = function (o) {
    const VW = 168, VH = 120, S = 2.6, N = 16, BX = 70, BY = 40;
    const cA = D(o.refCanvas), cB = D(o.curCanvas), cK = D(o.costCanvas), cR = D(o.resCanvas);
    [cA, cB].forEach(c => { c.width = VW * S; c.height = VH * S; });
    cK.width = 200; cK.height = 200;
    if (cR) { cR.width = N * 7 * 2 + 22; cR.height = N * 7; }
    const xA = cA.getContext('2d'), xB = cB.getContext('2d'), xK = cK.getContext('2d');
    const xR = cR ? cR.getContext('2d') : null;

    let SRC = null, SW = 0, SH = 0, luma = null, RGB = null;
    let t = 8, playing = true, speed = 1, phase = 0, phaseT = 0, last = 0;
    let cand = [], shown = 0, best = null, R = 12, hex = false, panx = 0, pany = 0;
    let seed = [0, 0];   // last frame's winner, standing in for the neighbour predictor
    const BEAT = [1600, 3000, 2800];

    // where the camera is looking on frame t
    // aimed at the character: hair, face and shoulder give the search real
    // texture to lock onto. A block on flat sky has no minimum to find.
    // The pan is deliberately fractional. A whole-pixel pan of a still frame
    // matches exactly, and a figure that shows a zero residual teaches that
    // motion estimation is free. It is not: what integer search cannot reach
    // is exactly what quarter-pixel interpolation exists for.
    const camXf = t => 40 + 34 * Math.sin(t * 0.36);
    const camYf = t => 100 + 11 * Math.sin(t * 0.27);
    const camX = t => Math.round(camXf(t));
    const camY = t => Math.round(camYf(t));

    // the current frame is resampled at the fractional position
    let curL = null, curCv = null;
    function buildCurrent(fx, fy) {
      if (!curCv) {
        curCv = document.createElement('canvas');
        curCv.width = VW; curCv.height = VH;
        curL = new Float32Array(VW * VH);
      }
      const ctx = curCv.getContext('2d');
      const img = ctx.createImageData(VW, VH);
      const x0 = Math.floor(fx), y0 = Math.floor(fy);
      const ax = fx - x0, ay = fy - y0;
      for (let y = 0; y < VH; y++) for (let x = 0; x < VW; x++) {
        const sx = Math.min(SW - 2, Math.max(0, x0 + x)), sy = Math.min(SH - 2, Math.max(0, y0 + y));
        const i = (sy * SW + sx) * 4;
        const o = (y * VW + x) * 4;
        for (let ch = 0; ch < 3; ch++) {
          const a = RGB[i + ch], b = RGB[i + 4 + ch];
          const c = RGB[i + SW * 4 + ch], d = RGB[i + SW * 4 + 4 + ch];
          img.data[o + ch] = (a * (1 - ax) + b * ax) * (1 - ay) + (c * (1 - ax) + d * ax) * ay;
        }
        img.data[o + 3] = 255;
        curL[y * VW + x] = .299 * img.data[o] + .587 * img.data[o + 1] + .114 * img.data[o + 2];
      }
      ctx.putImageData(img, 0, 0);
    }

    const win = (ctx, ox, oy) => {
      ctx.imageSmoothingEnabled = false;
      ctx.drawImage(SRC, ox, oy, VW, VH, 0, 0, VW * S, VH * S);
    };
    const lum = (x, y) => luma[y * SW + x];
    const cost = (rx, ry, dx, dy, n) => {
      let s = 0;
      for (let y = 0; y < n; y++) for (let x = 0; x < n; x++)
        s += Math.abs(curL[(BY + y) * VW + BX + x] - lum(rx + BX + x + dx, ry + BY + y + dy));
      return s;
    };

    function buildFrame() {
      R = +D(o.range).value; hex = D(o.mode).checked;
      const rx = camX(t), ry = camY(t);
      const fx = camXf(t + 1), fy = camYf(t + 1);
      buildCurrent(fx, fy);
      panx = fx - rx; pany = fy - ry;
      const ok = (dx, dy) => Math.abs(dx) <= R && Math.abs(dy) <= R
        && rx + BX + dx >= 0 && ry + BY + dy >= 0
        && rx + BX + dx + N <= SW && ry + BY + dy + N <= SH;
      cand = [];
      if (hex) {
        const HEX = [[-2, 0], [-1, -2], [1, -2], [2, 0], [1, 2], [-1, 2]];
        const SM = [[-1, 0], [0, -1], [1, 0], [0, 1]];
        const test = (dx, dy) => {
          if (!ok(dx, dy)) return Infinity;
          const c = cost(rx, ry, dx, dy, N);
          cand.push({ dx, dy, c }); return c;
        };
        // start from the predictor, not from zero. A hexagon walk launched at
        // (0,0) falls into the first local minimum it meets and reports a
        // vector that is visibly wrong; every real encoder seeds the search
        // with the motion of neighbouring blocks for exactly this reason.
        let px = Math.max(-R, Math.min(R, seed[0])), py = Math.max(-R, Math.min(R, seed[1]));
        let bc = test(px, py);
        if (!isFinite(bc)) { px = 0; py = 0; bc = test(0, 0); }
        for (let it = 0; it < 24; it++) {
          let moved = false;
          for (const [ax, ay] of HEX) {
            const c = test(px + ax, py + ay);
            if (c < bc) { bc = c; px += ax; py += ay; moved = true; break; }
          }
          if (!moved) break;
        }
        for (const [ax, ay] of SM) {
          const c = test(px + ax, py + ay);
          if (c < bc) { bc = c; px += ax; py += ay; }
        }
      } else {
        for (let dy = -R; dy <= R; dy++) for (let dx = -R; dx <= R; dx++)
          if (ok(dx, dy)) cand.push({ dx, dy, c: cost(rx, ry, dx, dy, N) });
      }
      best = cand.reduce((a, b) => (b.c < a.c ? b : a), { c: Infinity, dx: 0, dy: 0 });
      let full = Infinity, ftested = 0;
      for (let dy = -R; dy <= R; dy++) for (let dx = -R; dx <= R; dx++)
        if (ok(dx, dy)) { ftested++; full = Math.min(full, cost(rx, ry, dx, dy, N)); }
      best.full = full; best.ftested = ftested;
      best.zero = ok(0, 0) ? cost(rx, ry, 0, 0, N) : NaN;
      best.rx = rx; best.ry = ry; best.seed = seed.slice();
      seed = [best.dx, best.dy];
      shown = 0;
    }

    function drawFrames() {
      const { rx, ry } = best;
      win(xA, rx, ry);
      // beat 1 blinks the right panel between the two frames: a few pixels of
      // pan is invisible side by side and obvious when it flips
      const blink = phase === 0 && playing && (Math.floor(phaseT / 460) % 2 === 1);
      xB.imageSmoothingEnabled = false;
      if (blink) win(xB, rx, ry);
      else xB.drawImage(curCv, 0, 0, VW, VH, 0, 0, VW * S, VH * S);

      xB.strokeStyle = '#6741d9'; xB.lineWidth = 3;
      xB.strokeRect(BX * S, BY * S, N * S, N * S);
      if (phase === 2) {
        xB.strokeStyle = 'rgba(35,35,38,.5)'; xB.setLineDash([5, 4]); xB.lineWidth = 2;
        xB.strokeRect((BX + best.dx) * S, (BY + best.dy) * S, N * S, N * S);
        xB.setLineDash([]);
      }

      xA.strokeStyle = 'rgba(103,65,217,.85)'; xA.setLineDash([6, 4]); xA.lineWidth = 2;
      xA.strokeRect((BX - R) * S, (BY - R) * S, (N + 2 * R) * S, (N + 2 * R) * S);
      xA.setLineDash([]);

      if (phase === 1) {
        xA.strokeStyle = 'rgba(232,89,12,.30)'; xA.lineWidth = 1;
        for (let i = 0; i < shown; i++)
          xA.strokeRect((BX + cand[i].dx) * S, (BY + cand[i].dy) * S, N * S, N * S);
        const c = cand[Math.min(shown, cand.length - 1)];
        if (c) {
          xA.strokeStyle = '#e8590c'; xA.lineWidth = 3;
          xA.strokeRect((BX + c.dx) * S, (BY + c.dy) * S, N * S, N * S);
        }
      }
      if (phase === 2) {
        xA.strokeStyle = '#e8590c'; xA.lineWidth = 3;
        xA.strokeRect((BX + best.dx) * S, (BY + best.dy) * S, N * S, N * S);
        xA.beginPath();
        xA.moveTo((BX + N / 2) * S, (BY + N / 2) * S);
        xA.lineTo((BX + best.dx + N / 2) * S, (BY + best.dy + N / 2) * S);
        xA.lineWidth = 2.5; xA.stroke();
        xA.fillStyle = '#e8590c';
        xA.beginPath(); xA.arc((BX + N / 2) * S, (BY + N / 2) * S, 4, 0, 7); xA.fill();
      }
    }

    function drawCost() {
      const side = 2 * R + 1, px = 200 / side;
      xK.fillStyle = '#f4f2ee'; xK.fillRect(0, 0, 200, 200);
      const upto = phase === 0 ? 0 : (phase === 1 ? shown : cand.length);
      const vals = [];
      for (let i = 0; i < upto; i++) vals.push(cand[i].c);
      vals.sort((a, b) => a - b);
      const lo = vals[0] || 0;
      const hi = vals[Math.floor(vals.length * 0.75)] || lo + 1;   // clamp the tail
      for (let i = 0; i < upto; i++) {
        const c = cand[i];
        const n = Math.max(0, Math.min(1, (c.c - lo) / Math.max(1, hi - lo)));
        xK.fillStyle = `hsl(262 ${(78 - 40 * n).toFixed(0)}% ${(30 + 62 * n).toFixed(0)}%)`;
        xK.fillRect((c.dx + R) * px, (c.dy + R) * px, Math.ceil(px), Math.ceil(px));
      }
      if (hex && upto > 1) {
        xK.strokeStyle = 'rgba(35,35,38,.6)'; xK.lineWidth = 1.5; xK.beginPath();
        for (let i = 0; i < upto; i++) {
          const c = cand[i], X = (c.dx + R + .5) * px, Y = (c.dy + R + .5) * px;
          i ? xK.lineTo(X, Y) : xK.moveTo(X, Y);
        }
        xK.stroke();
      }
      xK.strokeStyle = 'rgba(35,35,38,.22)'; xK.lineWidth = 1;
      xK.beginPath(); xK.moveTo(100, 0); xK.lineTo(100, 200);
      xK.moveTo(0, 100); xK.lineTo(200, 100); xK.stroke();
      if (phase === 2) {
        const bxp = (best.dx + R) * px, byp = (best.dy + R) * px, sz = Math.max(px, 6);
        xK.strokeStyle = '#fff'; xK.lineWidth = 4;
        xK.strokeRect(bxp - 2, byp - 2, sz + 4, sz + 4);
        xK.strokeStyle = '#e8590c'; xK.lineWidth = 2.5;
        xK.strokeRect(bxp - 2, byp - 2, sz + 4, sz + 4);
      }
    }

    function drawResidual() {
      if (!xR) return;
      const s = 7, { rx, ry } = best;
      xR.fillStyle = '#fff'; xR.fillRect(0, 0, cR.width, cR.height);
      let means = [];
      const panel = (ox, dx, dy) => {
        const img = xR.createImageData(N, N);
        let sum = 0;
        for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) {
          const d = Math.abs(curL[(BY + y) * VW + BX + x] - lum(rx + BX + x + dx, ry + BY + y + dy));
          sum += d;
          const v = 255 - Math.min(255, d * 12);
          const i = (y * N + x) * 4;
          img.data[i] = 255; img.data[i + 1] = v; img.data[i + 2] = v; img.data[i + 3] = 255;
        }
        const off = document.createElement('canvas');
        off.width = N; off.height = N; off.getContext('2d').putImageData(img, 0, 0);
        xR.imageSmoothingEnabled = false;
        xR.drawImage(off, ox, 0, N * s, N * s);
        means.push(sum / (N * N));
      };
      panel(0, 0, 0);
      if (phase === 2) panel(N * s + 22, best.dx, best.dy);
      set(o.res0Out, means[0] === undefined ? '—' : means[0].toFixed(1));
      set(o.res1Out, means[1] === undefined ? '—' : means[1].toFixed(1));
    }

    function readouts() {
      set(o.rangeOut, '±' + R);
      if (phase === 0) {
        set(o.mvOut, '—'); set(o.sadOut, (best.zero / (N * N)).toFixed(1));
        set(o.testedOut, '0 of ' + best.ftested); set(o.penaltyOut, '—');
        set(o.phaseOut, 'Beat 1. The camera has panned. The right panel blinks between the two frames so you can see by how much.');
      } else if (phase === 1) {
        set(o.mvOut, '…'); set(o.sadOut, '…');
        set(o.testedOut, shown + ' of ' + best.ftested);
        set(o.penaltyOut, 'searching');
        set(o.phaseOut, hex
          ? `Beat 2. The hexagon pattern steps downhill from the predictor at (${best.seed[0]}, ${best.seed[1]}), which is where the neighbouring blocks ended up. Each filled square is one position actually tested.`
          : 'Beat 2. Every position in the window, one at a time. Dark violet on the cost map is a close match.');
      } else {
        const pinned = Math.abs(best.dx) >= R || Math.abs(best.dy) >= R;
        set(o.mvOut, `(${best.dx}, ${best.dy})`);
        set(o.sadOut, (best.c / (N * N)).toFixed(1));
        set(o.testedOut, cand.length + ' of ' + best.ftested);
        set(o.penaltyOut, pinned ? 'hit the window edge' : hex
          ? (best.c <= best.full * 1.02
            ? 'same match, ' + Math.round(cand.length / best.ftested * 100) + '% of the work'
            : '+' + Math.round((best.c - best.full) / Math.max(1, best.full) * 100) + '% worse')
          : 'exhaustive');
        set(o.phaseOut, `Beat 3. The camera panned by (${panx.toFixed(1)}, ${pany.toFixed(1)}) pixels; the best whole-pixel match is (${best.dx}, ${best.dy}). What that fraction of a pixel leaves behind is the residual, and it is why H.264 searches quarter-pixel positions too.`);
      }
    }

    const render = () => { if (!SRC) return; drawFrames(); drawCost(); drawResidual(); readouts(); };

    function tick(now) {
      const dt = last ? Math.min(64, now - last) : 16; last = now;
      if (playing && SRC) {
        phaseT += dt * speed;
        if (phase === 1) shown = Math.max(1, Math.round(Math.min(1, phaseT / BEAT[1]) * cand.length));
        if (phaseT >= BEAT[phase]) {
          phaseT = 0;
          if (phase === 2) { t += 1; buildFrame(); phase = 0; }
          else { phase++; if (phase === 1) shown = 1; }
        }
        render();
      }
      requestAnimationFrame(tick);
    }

    D(o.play).onclick = e => { playing = !playing; e.target.textContent = playing ? 'Pause' : 'Play'; };
    D(o.step).onclick = () => {
      playing = false; D(o.play).textContent = 'Play'; phaseT = 0;
      if (phase === 2) { t += 1; buildFrame(); phase = 0; }
      else { phase++; if (phase === 1) shown = cand.length; }
      render();
    };
    D(o.slow).onchange = e => { speed = e.target.checked ? 0.4 : 1; };
    D(o.range).oninput = () => { buildFrame(); phase = 0; phaseT = 0; render(); };
    D(o.mode).onchange = () => { buildFrame(); phase = 0; phaseT = 0; render(); };

    const img = new Image();
    img.onload = () => {
      SRC = img; SW = img.width; SH = img.height;
      const off = document.createElement('canvas');
      off.width = SW; off.height = SH;
      const oc = off.getContext('2d');
      oc.drawImage(img, 0, 0);
      const d = oc.getImageData(0, 0, SW, SH).data;
      RGB = d;
      luma = new Float32Array(SW * SH);
      for (let i = 0; i < SW * SH; i++)
        luma[i] = .299 * d[i * 4] + .587 * d[i * 4 + 1] + .114 * d[i * 4 + 2];
      buildFrame(); render(); requestAnimationFrame(tick);
    };
    img.src = window.SAMPLE_FRAME;
  };

  // ============================================================
  // 3. Quantisation, on a real frame
  // ============================================================
  window.initQuantReal = function (o) {
    const cv = D(o.canvas), cz = D(o.zoom);
    const img = new Image();
    img.onload = () => {
      const W = img.width, H = img.height;
      cv.width = W; cv.height = H;
      const ctx = cv.getContext('2d');
      const off = document.createElement('canvas');
      off.width = W; off.height = H;
      const octx = off.getContext('2d');
      octx.drawImage(img, 0, 0);
      const src = octx.getImageData(0, 0, W, H);

      const Y = new Float32Array(W * H), Cb = new Float32Array(W * H), Cr = new Float32Array(W * H);
      for (let i = 0; i < W * H; i++) {
        const r = src.data[i * 4], g = src.data[i * 4 + 1], b = src.data[i * 4 + 2];
        Y[i] = .299 * r + .587 * g + .114 * b;
        Cb[i] = -.168736 * r - .331264 * g + .5 * b + 128;
        Cr[i] = .5 * r - .418688 * g - .081312 * b + 128;
      }
      const rY = new Float32Array(W * H), rCb = new Float32Array(W * H), rCr = new Float32Array(W * H);
      const blk = new Float32Array(64), coef = new Float32Array(64), out = new Float32Array(64);
      const dst = ctx.createImageData(W, H);
      let pending = 0;

      function plane(sp, dp, q) {
        let kept = 0, total = 0, bits = 0;
        for (let by = 0; by < H; by += 8) for (let bx = 0; bx < W; bx += 8) {
          for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++)
            blk[y * 8 + x] = sp[(by + y) * W + bx + x] - 128;
          dct8(blk, coef);
          for (let i = 0; i < 64; i++) {
            const lvl = Math.round(coef[i] / q);
            total++;
            if (lvl) { kept++; bits += 2 + 2 * Math.log2(1 + Math.abs(lvl)); }
            coef[i] = lvl * q;
          }
          idct8(coef, out);
          for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++)
            dp[(by + y) * W + bx + x] = out[y * 8 + x] + 128;
        }
        return { kept, total, bits };
      }

      function render(qp) {
        const q = qstep(qp);
        const a = plane(Y, rY, q);
        const b = plane(Cb, rCb, q * 1.25);
        const c = plane(Cr, rCr, q * 1.25);
        let se = 0;
        for (let i = 0; i < W * H; i++) {
          const y = rY[i], cb = rCb[i] - 128, cr = rCr[i] - 128;
          dst.data[i * 4] = Math.max(0, Math.min(255, y + 1.402 * cr));
          dst.data[i * 4 + 1] = Math.max(0, Math.min(255, y - .344136 * cb - .714136 * cr));
          dst.data[i * 4 + 2] = Math.max(0, Math.min(255, y + 1.772 * cb));
          dst.data[i * 4 + 3] = 255;
          const d = Y[i] - rY[i]; se += d * d;
        }
        ctx.putImageData(dst, 0, 0);

        const kept = a.kept + b.kept + c.kept, total = a.total + b.total + c.total;
        const bits = a.bits + b.bits + c.bits;
        set(o.qpOut, qp);
        set(o.keptOut, (kept / total * 100).toFixed(1) + '%');
        set(o.bitsOut, (bits / 8192).toFixed(0) + ' kB');
        set(o.psnrOut, (10 * Math.log10(65025 / Math.max(1e-6, se / (W * H)))).toFixed(1) + ' dB');
        set(o.ratioOut, Math.round(W * H * 3 / (bits / 8)) + ':1');

        if (cz) {
          const zw = 128, zh = 96, zx = o.zoomX, zy = o.zoomY;
          cz.width = zw * 4; cz.height = zh * 4;
          const zc = cz.getContext('2d');
          zc.imageSmoothingEnabled = false;
          zc.drawImage(cv, zx, zy, zw, zh, 0, 0, zw * 4, zh * 4);
          ctx.strokeStyle = '#6741d9'; ctx.lineWidth = 2;
          ctx.strokeRect(zx, zy, zw, zh);
        }
      }

      const slider = D(o.slider);
      const schedule = () => {
        if (pending) return;
        pending = requestAnimationFrame(() => { pending = 0; render(+slider.value); });
      };
      slider.addEventListener('input', schedule);
      render(+slider.value);
    };
    img.src = window.SAMPLE_FRAME;
  };

  // ============================================================
  // 4. Rate control: one shot, four policies
  // ============================================================
  window.initRcFig = function (o) {
    const NF = 160, CUTS = [55, 105];
    const cx = new Float32Array(NF);
    for (let f = 0; f < NF; f++) {
      let c = 1 + .25 * Math.sin(f * .09);
      if (f >= 55 && f < 105) c = 4.4 + .8 * Math.sin(f * .5) + (f < 62 ? 1.6 : 0);
      if (f >= 105) c = 1.15 + .3 * Math.sin(f * .12);
      cx[f] = c;
    }
    const BASE = 190000;          // bits per unit complexity at qstep 1, calibrated so
                                 // QP 27 on the calm section lands near the target
    const bitsFor = (c, qp) => BASE * c / qstep(qp);
    const qpFor = (c, bits) =>
      Math.max(8, Math.min(51, 4 + 6 * Math.log2(BASE * c / Math.max(1, bits) / .85)));

    function simulate(mode, target, lookahead) {
      const qp = new Float32Array(NF), bits = new Float32Array(NF), buf = new Float32Array(NF);
      const BUFSZ = target * 20;   // about 0.8s of buffer at 25fps
      let fill = BUFSZ * .8, acc = 0, under = 0;
      for (let f = 0; f < NF; f++) {
        let q;
        if (mode === 'cqp') q = 27;
        else if (mode === 'crf' || mode === 'ccrf') q = 27 + 6 * Math.log2(Math.pow(cx[f], .4));
        else {
          const err = acc - target * f;
          q = 27 + 6 * Math.log2(Math.max(.35, 1 + err / (target * 22)));
          if (lookahead) {
            let ahead = 0, n = 0;
            for (let k = f; k < Math.min(NF, f + 40); k++) { ahead += cx[k]; n++; }
            q += 6 * Math.log2(Math.max(.6, Math.min(1.9, Math.pow((ahead / n) / 1.6, .35))));
          }
        }
        q = Math.max(10, Math.min(51, q));
        let b = bitsFor(cx[f], q);
        // vbv and capped CRF share one clip. The difference is what sets the
        // quality in the first place: a bitrate controller, or a rate factor.
        if ((mode === 'vbv' || mode === 'ccrf') && b > fill * .5) {
          b = fill * .5; q = qpFor(cx[f], b);
        }
        fill = Math.min(BUFSZ, fill - b + target);
        if (fill < 0) { under++; fill = 0; }
        qp[f] = q; bits[f] = b; buf[f] = fill / BUFSZ; acc += b;
      }
      return { qp, bits, buf, total: acc, under };
    }

    const VERDICT = {
      cqp: () => 'One QP for the whole shot. The picture quality is even, but the bitrate does whatever the content tells it to — the action section costs several times the calm one, and nobody asked the network.',
      crf: () => 'Quality is held roughly steady and the bitrate is allowed to move. The action still costs more, but only as much more as it perceptually needs. The right default whenever the file size is negotiable.',
      abr: la => la
        ? 'The lookahead sees the hard section coming and starts tightening before the cut, so the average is met without a spike at the boundary and without a lurch afterwards.'
        : 'With no lookahead the controller can only react after the fact: it overspends into the cut, then over-corrects, and the quality visibly lurches for a second either side. Switch the lookahead on and watch the QP trace settle.',
      ccrf: (la, r) => r.under
        ? 'Even with the cap the buffer ran dry, which means the rate factor is too generous for this ceiling.'
        : 'Quality drives, and the cap only ever subtracts. Until the hard section arrives the trace is identical to plain CRF, bit for bit, because the ceiling is slack there. It starts binding partway into the action and keeps binding through the recovery while the buffer refills. This is what a VOD library and the rungs of an adaptive ladder actually run.',
      vbv: (la, r) => r.under
        ? 'The buffer still ran dry. At this target and this buffer size the content simply cannot be carried — the encoder must either drop quality further or you must raise the target.'
        : 'The peak is clipped to whatever the buffer can carry, so quality dips through the action section. That dip is the price of never stalling a decoder, and it is not optional for broadcast or adaptive streaming.',
    };

    const svg = D(o.chart), state = { mode: 'crf', lookahead: true };

    function draw() {
      const target = +D(o.target).value * 1000;
      const r = simulate(state.mode, target, state.lookahead);

      // three stacked panels, each with its own scale, its own baseline and a
      // clip so a curve can never escape its box
      const VW = 660, L = 66, RGUT = 58, PW = VW - L - RGUT;
      const P = [
        { y: 34, h: 96, name: 'bits/frame' },
        { y: 158, h: 74, name: 'QP' },
        { y: 260, h: 56, name: 'buffer' },
      ];
      const X = f => L + (f / (NF - 1)) * PW;
      const maxB = Math.max(Math.max(...r.bits), target * 1.2) * 1.12;   // 12% headroom
      const Yb = b => P[0].y + P[0].h - Math.max(0, Math.min(1, b / maxB)) * P[0].h;
      const Yq = q => P[1].y + ((Math.max(10, Math.min(51, q)) - 10) / 41) * P[1].h;
      const Yf = v => P[2].y + P[2].h - Math.max(0, Math.min(1, v)) * P[2].h;
      const path = (arr, Y) => arr.map((v, f) => `${f ? 'L' : 'M'}${X(f).toFixed(1)},${Y(v).toFixed(1)}`).join('');
      const band = i => `<rect x="${X(CUTS[0]).toFixed(1)}" y="${P[i].y}" width="${(X(CUTS[1]) - X(CUTS[0])).toFixed(1)}" height="${P[i].h}" fill="#fff4e6"/>`;
      const cuts = i => CUTS.map(c =>
        `<line x1="${X(c).toFixed(1)}" y1="${P[i].y}" x2="${X(c).toFixed(1)}" y2="${P[i].y + P[i].h}" stroke="#e8b88a" stroke-dasharray="3 4"/>`).join('');
      const frame = i =>
        `<rect x="${L}" y="${P[i].y}" width="${PW}" height="${P[i].h}" fill="none" stroke="#e9ecef"/>` +
        `<text x="${L - 10}" y="${P[i].y + 11}" font-size="10.5" fill="#868e96" text-anchor="end">${P[i].name}</text>`;
      const kb = v => (v / 1000).toFixed(0) + 'k';

      svg.innerHTML = `
        <defs>
          ${P.map((p, i) => `<clipPath id="rcclip${i}"><rect x="${L}" y="${p.y}" width="${PW}" height="${p.h}"/></clipPath>`).join('')}
        </defs>

        <text x="${L}" y="18" font-size="11" fill="#8f310c">hard action section between two cuts</text>
        <rect x="${(X(CUTS[0])).toFixed(1)}" y="22" width="${(X(CUTS[1]) - X(CUTS[0])).toFixed(1)}" height="4" fill="#ffd8a8"/>

        ${band(0)}${cuts(0)}${frame(0)}
        <text x="${L - 10}" y="${P[0].y + P[0].h}" font-size="9.5" fill="#adb5bd" text-anchor="end">0</text>
        <text x="${L - 10}" y="${P[0].y + 24}" font-size="9.5" fill="#adb5bd" text-anchor="end">${kb(maxB)}</text>
        <g clip-path="url(#rcclip0)">
          <line x1="${L}" y1="${Yb(target).toFixed(1)}" x2="${L + PW}" y2="${Yb(target).toFixed(1)}" stroke="#0b7285" stroke-dasharray="5 4"/>
          <path d="${path([...r.bits], Yb)}" fill="none" stroke="#6741d9" stroke-width="2"/>
        </g>
        <text x="${L + PW + 6}" y="${Yb(target) + 3.5}" font-size="10" fill="#0b7285">target</text>

        ${band(1)}${cuts(1)}${frame(1)}
        <text x="${L - 10}" y="${P[1].y + 11}" font-size="9.5" fill="#adb5bd" text-anchor="end" dy="12">better</text>
        <text x="${L - 10}" y="${P[1].y + P[1].h}" font-size="9.5" fill="#adb5bd" text-anchor="end">worse</text>
        <g clip-path="url(#rcclip1)">
          <path d="${path([...r.qp], Yq)}" fill="none" stroke="#e8590c" stroke-width="2"/>
        </g>
        <text x="${L + PW + 6}" y="${Yq(r.qp[NF - 1]) + 3.5}" font-size="10" fill="#e8590c">QP</text>

        ${band(2)}${cuts(2)}${frame(2)}
        <text x="${L - 10}" y="${P[2].y + 24}" font-size="9.5" fill="#adb5bd" text-anchor="end">full</text>
        <text x="${L - 10}" y="${P[2].y + P[2].h}" font-size="9.5" fill="#adb5bd" text-anchor="end">empty</text>
        <g clip-path="url(#rcclip2)">
          <path d="${path([...r.buf], Yf)}" fill="none" stroke="#0b7285" stroke-width="2"/>
        </g>
        ${r.under
          ? `<text x="${L}" y="${P[2].y + P[2].h + 16}" font-size="10.5" fill="#c92a2a">buffer empty on ${r.under} frames, so a real decoder stalls</text>`
          : `<text x="${L}" y="${P[2].y + P[2].h + 16}" font-size="10.5" fill="#0b7285">buffer never empties, safe to stream</text>`}
        <text x="${L + PW}" y="${P[2].y + P[2].h + 16}" font-size="10" fill="#adb5bd" text-anchor="end">160 frames</text>`;

      let vmin = 99, vmax = 0;
      r.qp.forEach(q => { vmin = Math.min(vmin, q); vmax = Math.max(vmax, q); });
      set(o.sizeOut, Math.round(r.total / 8192) + ' kB');
      set(o.qpMeanOut, ([...r.qp].reduce((a, b) => a + b, 0) / NF).toFixed(1));
      set(o.qpSwingOut, (vmax - vmin).toFixed(1));
      set(o.vbvOut, r.under ? r.under + ' stalls' : 'none');
      set(o.verdictOut, VERDICT[state.mode](state.lookahead, r));
      const live = state.mode === 'abr' || state.mode === 'vbv';
      D(o.look).disabled = !live;                    // inert here, so say so
      D(o.look).parentElement.style.opacity = live ? 1 : .5;
    }

    D(o.modes).onclick = e => {
      if (!e.target.dataset.mode) return;
      state.mode = e.target.dataset.mode;
      [...D(o.modes).children].forEach(b => b.classList.toggle('on', b.dataset.mode === state.mode));
      draw();
    };
    D(o.look).onchange = e => { state.lookahead = e.target.checked; draw(); };
    D(o.target).oninput = e => { set(o.targetOut, e.target.value + ' kbit'); draw(); };
    set(o.targetOut, D(o.target).value + ' kbit');
    [...D(o.modes).children].forEach(b => b.classList.toggle('on', b.dataset.mode === state.mode));
    draw();
  };

  // ============================================================
  // 5. The decision: cost = D + lambda x R
  // ============================================================
  window.initRdFig = function (o) {
    const W = 132, H = 92, N = 16, t = 20;
    const ref = new Float32Array(W * H), cur = new Float32Array(W * H);
    scene(t, W, H, ref); scene(t + 1, W, H, cur);
    const bx = Math.max(0, Math.min(W - N, Math.round(discX(t + 1, W)) - N / 2)) | 0;
    const by = Math.max(0, Math.min(H - N, Math.round(H * .30) - N / 2)) | 0;
    const search = (x, y, n, r) => {
      let b = Infinity;
      for (let dy = -r; dy <= r; dy++) for (let dx = -r; dx <= r; dx++) {
        if (x + dx < 0 || y + dy < 0 || x + dx + n > W || y + dy + n > H) continue;
        b = Math.min(b, sad(cur, ref, W, x, y, dx, dy, n));
      }
      return b;
    };
    const skipD = sad(cur, ref, W, bx, by, 0, 0, N);
    const d16 = search(bx, by, N, 8);
    let d8 = 0;
    for (let q = 0; q < 4; q++) d8 += search(bx + (q % 2) * 8, by + ((q / 2) | 0) * 8, 8, 8);
    let mean = 0;
    for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) mean += cur[(by + y) * W + bx + x];
    mean /= N * N;
    let dI = 0;
    for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) dI += Math.abs(cur[(by + y) * W + bx + x] - mean);

    const MODES = [
      { k: 'SKIP', d: skipD, r: 1, why: 'copy the co-located block, send nothing' },
      { k: 'Inter 16×16', d: d16, r: 14, why: 'one motion vector for the whole block' },
      { k: 'Inter 8×8', d: d8, r: 46, why: 'four vectors — fits better, costs more to say' },
      { k: 'Intra 16×16', d: dI, r: 74, why: 'ignore the past, predict from this frame' },
    ];
    const lam = D(o.lambda), rows = D(o.rows), svg = D(o.chart);

    function draw() {
      const L = +lam.value;
      const costs = MODES.map(m => ({ ...m, c: m.d + L * m.r }));
      const win = costs.reduce((a, b) => (b.c < a.c ? b : a));
      rows.innerHTML = costs.map(m => `
        <div class="rd-row${m === win ? ' win' : ''}">
          <div class="rd-k">${m.k}${m === win ? '<span class="rd-tag">chosen</span>' : ''}</div>
          <div class="rd-why">${m.why}</div>
          <div class="rd-n"><span>D</span>${Math.round(m.d)}</div>
          <div class="rd-n"><span>R</span>${m.r}</div>
          <div class="rd-n rd-c"><span>D + λR</span>${Math.round(m.c)}</div>
        </div>`).join('');
      set(o.lambdaOut, L);
      const maxL = +lam.max, maxC = Math.max(...MODES.map(m => m.d + maxL * m.r));
      const X = l => 46 + (l / maxL) * 396, Yc = c => 168 - (c / maxC) * 138;
      const cols = ['#6741d9', '#0b7285', '#e8590c', '#8f3d3d'];
      svg.innerHTML = `
        <line x1="46" y1="168" x2="450" y2="168" stroke="#ced4da"/>
        <line x1="46" y1="18" x2="46" y2="168" stroke="#ced4da"/>
        <text x="46" y="188" font-size="10.5" fill="#868e96">λ = 0 · bits are free</text>
        <text x="450" y="188" font-size="10.5" fill="#868e96" text-anchor="end">λ = ${maxL} · bits are expensive</text>
        <text x="4" y="16" font-size="10.5" fill="#868e96">cost</text>
        ${MODES.map((m, i) => `
          <line x1="${X(0)}" y1="${Yc(m.d)}" x2="${X(maxL)}" y2="${Yc(m.d + maxL * m.r)}"
                stroke="${cols[i]}" stroke-width="${m === win ? 3 : 1.4}" opacity="${m === win ? 1 : .45}"/>
          <text x="${X(maxL) + 5}" y="${Yc(m.d + maxL * m.r) + 4}" font-size="10" fill="${cols[i]}">${m.k}</text>`).join('')}
        <line x1="${X(L)}" y1="14" x2="${X(L)}" y2="172" stroke="#232326" stroke-dasharray="3 3"/>
        <circle cx="${X(L)}" cy="${Yc(win.c)}" r="4.5" fill="#232326"/>`;
    }
    lam.oninput = draw; draw();
  };
})();
