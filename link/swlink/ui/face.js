// Browser preview of the watch's Kiro face (linkfw/src/face.cpp).
// The ghost body is drawn as a path rather than shipping the 393 KB RGB565
// bitmap; eyes, motion, decorations and timings mirror the firmware so the
// dashboard preview matches what the device shows.
(function () {
  const MOODS = ['sleep', 'idle', 'thinking', 'working', 'waiting', 'question', 'error', 'celebrate', 'listening', 'dizzy'];
  const LABEL = { sleep: 'zzz', idle: '', thinking: 'thinking', working: 'working', waiting: 'waiting',
                  question: 'hmm?', error: 'error', celebrate: 'celebrate!', listening: 'listening...', dizzy: 'whoa...' };
  // Firmware geometry (212x258 body inside a 320x372 sprite) scaled to the canvas.
  const BODY_W = 213, BODY_H = 258, EYE = { lx: 118, ly: 92, rx: 162, w: 25, h: 41 };

  function ghostPath(ctx, x, y, w, h) {
    // Rounded dome with a pointed left tail and two bottom lobes, as in kiro-ghost.svg.
    ctx.beginPath();
    ctx.moveTo(x + w * 0.5, y);
    ctx.bezierCurveTo(x + w * 0.95, y, x + w, y + h * 0.42, x + w * 0.96, y + h * 0.66);
    ctx.bezierCurveTo(x + w * 0.94, y + h * 0.86, x + w * 0.84, y + h, x + w * 0.74, y + h * 0.97);
    ctx.bezierCurveTo(x + w * 0.68, y + h * 0.95, x + w * 0.64, y + h * 0.88, x + w * 0.58, y + h * 0.9);
    ctx.bezierCurveTo(x + w * 0.5, y + h * 0.93, x + w * 0.44, y + h, x + w * 0.36, y + h * 0.97);
    ctx.bezierCurveTo(x + w * 0.3, y + h * 0.94, x + w * 0.29, y + h * 0.86, x + w * 0.27, y + h * 0.8);
    ctx.bezierCurveTo(x + w * 0.18, y + h * 0.85, x + w * 0.02, y + h * 0.82, x + w * 0.05, y + h * 0.72);
    ctx.bezierCurveTo(x + w * 0.09, y + h * 0.62, x + w * 0.16, y + h * 0.6, x + w * 0.15, y + h * 0.45);
    ctx.bezierCurveTo(x + w * 0.14, y + h * 0.18, x + w * 0.3, y, x + w * 0.5, y);
    ctx.closePath();
  }

  function star(ctx, cx, cy, r, rot, color) {
    ctx.beginPath();
    for (let i = 0; i < 10; i++) {
      const a = rot + i * Math.PI / 5 - Math.PI / 2, rr = (i & 1) ? r * 0.42 : r;
      const x = cx + Math.cos(a) * rr, y = cy + Math.sin(a) * rr;
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    }
    ctx.closePath();
    ctx.fillStyle = color; ctx.fill();
    ctx.lineWidth = Math.max(1.5, r * 0.18); ctx.strokeStyle = '#222'; ctx.stroke();   // visible over the body
  }

  function spiralEye(ctx, cx, cy, rx, ry, phase) {
    ctx.save();
    ctx.beginPath(); ctx.ellipse(cx, cy, rx, ry, 0, 0, Math.PI * 2);
    ctx.fillStyle = '#fff'; ctx.fill(); ctx.lineWidth = 1.5; ctx.strokeStyle = '#000'; ctx.stroke();
    ctx.beginPath();
    for (let i = 0; i <= 26; i++) {
      const f = i / 26, a = phase + f * 3.2 * Math.PI, r = 0.12 + f * 0.85;
      const x = cx + Math.cos(a) * r * rx, y = cy + Math.sin(a) * r * ry;
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    }
    ctx.lineWidth = Math.max(1.5, rx * 0.22); ctx.strokeStyle = '#000'; ctx.lineCap = 'round'; ctx.stroke();
    ctx.restore();
  }

  function eyeShape(ctx, cx, cy, style, s) {
    ctx.save(); ctx.strokeStyle = '#000'; ctx.lineWidth = 6 * s; ctx.lineCap = 'round'; ctx.beginPath();
    if (style === 1) { ctx.moveTo(cx-12*s, cy-12*s); ctx.lineTo(cx+12*s, cy+12*s); ctx.moveTo(cx-12*s, cy+12*s); ctx.lineTo(cx+12*s, cy-12*s); }
    if (style === 2) { ctx.moveTo(cx-13*s, cy+7*s); ctx.lineTo(cx, cy-9*s); ctx.lineTo(cx+13*s, cy+7*s); }
    if (style === 3) { ctx.moveTo(cx-13*s, cy-4*s); ctx.lineTo(cx, cy+5*s); ctx.lineTo(cx+13*s, cy-4*s); }
    ctx.stroke(); ctx.restore();
  }

  /** Draw one frame. state: {mood, t (seconds), blink (0..1 open), level (0..1)} */
  function drawFace(ctx, W, H, state) {
    const mood = MOODS.includes(state.mood) ? state.mood : 'idle';
    const t = state.t;
    let bob = Math.sin(t * 2) * 6, jitter = 0, wsc = 1, hsc = 1, biasX = 0, biasY = 0, estyle = 0, frown = false;
    switch (mood) {
      case 'sleep': bob = Math.sin(t * 0.9) * 4; estyle = 3; break;
      case 'thinking': bob = Math.sin(t * 1.3) * 4; hsc = 0.9; biasX = -6; biasY = -8; break;
      case 'working': bob = Math.sin(t * 3) * 4; hsc = 0.8; wsc = 1.02; biasY = 4; frown = true; break;
      case 'waiting': bob = -Math.abs(Math.sin(t * 4)) * 12; hsc = 1.14; wsc = 1.08; break;
      case 'question': bob = Math.sin(t * 1.6) * 5; hsc = 1.05; biasX = -7; biasY = -6; break;
      case 'error': jitter = (Math.floor(t * 34) & 1) ? 3 : -3; estyle = 1; break;
      case 'celebrate': bob = -Math.abs(Math.sin(t * 5)) * 18; estyle = 2; break;
      case 'listening': bob = Math.sin(t * 1.5) * 3; wsc = 1.25; hsc = 1.2; biasY = -2; break;
      case 'dizzy': bob = Math.sin(t * 3.2) * 9; jitter = Math.sin(t * 6.5) * 7; estyle = 4; break;
    }
    const s = Math.min(W / 320, H / 400);                 // firmware sprite units -> canvas
    const bw = BODY_W * s, bh = BODY_H * s;
    const bx = (W - bw) / 2 + jitter * s, by = 40 * s + bob * s;

    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#000'; ctx.fillRect(0, 0, W, H);

    if (mood === 'listening') {                            // pulsing rings + level bars
      const gcx = bx + bw / 2, gcy = by + bh / 2;
      for (let i = 0; i < 3; i++) {
        const ph = (t * 0.8 + i / 3) % 1, r = bh / 2 + (8 + ph * 34) * s;
        ctx.beginPath(); ctx.ellipse(gcx, gcy, r, r, 0, 0, Math.PI * 2);
        ctx.strokeStyle = `rgba(0,229,255,${0.75 - ph * 0.65})`; ctx.lineWidth = 2; ctx.stroke();
      }
    }
    if (mood === 'dizzy') {                                // stars circling overhead, spinning as they go
      const hcx = bx + bw / 2, hcy = by - 12 * s;
      for (let i = 0; i < 4; i++) {
        const a = t * 3.4 + i * Math.PI / 2;
        const depth = 0.75 + 0.25 * Math.sin(a);
        star(ctx, hcx + Math.cos(a) * 74 * s, hcy + Math.sin(a) * 13 * s, 15 * depth * s, t * 2.2 + i,
             (i & 1) ? '#ffe94d' : '#fffbc8');
      }
    }

    ghostPath(ctx, bx, by, bw, bh);
    ctx.fillStyle = '#fff'; ctx.fill();
    ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(255,255,255,0.55)'; ctx.stroke();   // soften the rim like the device bitmap

    const open = state.blink ?? 1;
    const lx = bx + EYE.lx * s + biasX * s, rx = bx + EYE.rx * s + biasX * s, ey = by + EYE.ly * s + biasY * s;
    if (estyle === 0) {
      const ew = EYE.w * wsc * s / 2, eh = Math.max(1.5, EYE.h * hsc * open * s / 2);
      ctx.fillStyle = '#000';
      ctx.beginPath(); ctx.ellipse(lx, ey, ew, eh, 0, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.ellipse(rx, ey, ew, eh, 0, 0, Math.PI * 2); ctx.fill();
    } else if (estyle === 4) {
      spiralEye(ctx, lx, ey, 13 * s, 15 * s, t * 5);
      spiralEye(ctx, rx, ey, 13 * s, 15 * s, -t * 5);
    } else { eyeShape(ctx, lx, ey, estyle, s); eyeShape(ctx, rx, ey, estyle, s); }
    if (frown) {
      ctx.save(); ctx.strokeStyle = '#000'; ctx.lineWidth = 5 * s; ctx.lineCap = 'round'; ctx.beginPath();
      ctx.moveTo(lx-15*s, ey-20*s); ctx.lineTo(lx+11*s, ey-11*s);
      ctx.moveTo(rx-11*s, ey-11*s); ctx.lineTo(rx+15*s, ey-20*s);
      ctx.stroke(); ctx.restore();
    }
    if (mood === 'celebrate') {
      const gcx = bx + bw / 2, gcy = by + bh / 2;
      for (let i = 0; i < 7; i++) {
        const a = t * 3 + i * 0.9;
        ctx.beginPath();
        ctx.ellipse(gcx + Math.cos(a) * (bw / 2 + 12 * s), gcy + Math.sin(a) * (bh / 2 + 8 * s),
                    ((i + Math.floor(t * 4)) & 1 ? 5 : 3) * s, ((i + Math.floor(t * 4)) & 1 ? 5 : 3) * s, 0, 0, Math.PI * 2);
        ctx.fillStyle = (i & 1) ? '#ffd24b' : '#66ffc2'; ctx.fill();
      }
    }
    if (mood === 'listening') {
      const bars = 7, bwid = 10 * s, gap = 6 * s, x0 = W / 2 - (bars * bwid + (bars - 1) * gap) / 2, base = H - 70 * s;
      ctx.fillStyle = '#00e5ff';
      for (let i = 0; i < bars; i++) {
        const wave = 0.5 + 0.5 * Math.sin(t * 9 + i * 1.1);
        const h = (4 + (10 + 44 * (state.level ?? 0.35)) * wave) * s;
        ctx.beginPath(); ctx.roundRect(x0 + i * (bwid + gap), base - h, bwid, h, 3 * s); ctx.fill();
      }
    }
    const label = LABEL[mood];
    if (label) {
      ctx.fillStyle = '#d0d0d0'; ctx.textAlign = 'center';
      ctx.font = `${Math.round(15 * s)}px -apple-system, sans-serif`;
      ctx.fillText(label, W / 2, H - 40 * s);
    }
  }

  /** Animate a mood on a canvas; returns a handle with setMood()/stop(). */
  function animate(canvas, mood) {
    const ctx = canvas.getContext('2d');
    const st = { mood, t: 0, blink: 1, level: 0.35 };
    let raf = 0, t0 = performance.now(), nextBlink = 1400 + Math.random() * 2000, blinkStart = -1;
    const step = now => {
      const ms = now - t0;
      st.t = ms / 1000;
      const canBlink = ['idle', 'working', 'waiting', 'thinking', 'question'].includes(st.mood);
      if (canBlink && ms > nextBlink) { blinkStart = ms; nextBlink = ms + 2200 + Math.random() * 2600; }
      st.blink = (blinkStart >= 0 && ms - blinkStart < 150) ? Math.abs((ms - blinkStart) / 150 - 0.5) * 2 : 1;
      if (st.mood === 'listening') st.level = 0.25 + 0.35 * (0.5 + 0.5 * Math.sin(ms / 700));
      drawFace(ctx, canvas.width, canvas.height, st);
      raf = requestAnimationFrame(step);
    };
    raf = requestAnimationFrame(step);
    return { setMood: m => { st.mood = m; }, stop: () => cancelAnimationFrame(raf), state: st };
  }

  window.KiroFace = { MOODS, LABEL, drawFace, animate };
})();
