/* Two canvas plots, no dependencies.
 *
 * StripChart  - time series, one panel, any number of traces.
 * PhasePlot   - the V-I plane, points fading over a persistence window.
 *
 * Both are told what to draw; neither knows a field name. The schema the
 * server sends decides how many of each exist and what goes in them.
 */

const AXIS = '#69737f';
const GRID = 'rgba(23, 27, 33, 0.085)';
const FRAME = '#c3cad2';
const TEXT = '#171b21';

/* Left and bottom rules. Gridlines alone leave a plot floating on the page;
 * the frame is what makes it read as an axis. */
function drawFrame(ctx, x0, y0, x1, y1) {
  ctx.strokeStyle = FRAME;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(Math.round(x0) + 0.5, y0);
  ctx.lineTo(Math.round(x0) + 0.5, Math.round(y1) + 0.5);
  ctx.lineTo(x1, Math.round(y1) + 0.5);
  ctx.stroke();
}

function niceTicks(lo, hi, count) {
  if (!(hi > lo)) hi = lo + 1;
  const raw = (hi - lo) / Math.max(count, 1);
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  const step = (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10) * mag;
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) {
    out.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  }
  return { ticks: out, step };
}

function tickLabel(value, step) {
  const decimals = Math.max(0, Math.min(4, -Math.floor(Math.log10(step))));
  return value.toFixed(decimals);
}

class Canvas2D {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.w = 0;
    this.h = 0;
    this.resize();
    this.observer = new ResizeObserver(() => this.resize());
    this.observer.observe(canvas);
  }

  /* Rebuilt plots share a canvas, so the old observer has to let go of it. */
  destroy() {
    this.observer.disconnect();
  }

  resize() {
    const rect = this.canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    this.w = Math.max(1, rect.width);
    this.h = Math.max(1, rect.height);
    this.canvas.width = Math.round(this.w * dpr);
    this.canvas.height = Math.round(this.h * dpr);
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  clear() {
    this.ctx.clearRect(0, 0, this.w, this.h);
  }
}

/* ------------------------------------------------------------------ */
class StripChart extends Canvas2D {
  /* traces: [{key, label, colour, scale, step, unit}] */
  constructor(canvas, opts) {
    super(canvas);
    this.title = opts.title || '';
    this.traces = opts.traces;
    this.window = opts.window || 30;
    this.t = [];
    this.values = new Map(this.traces.map((tr) => [tr.key, []]));
    this.pad = { l: 54, r: 10, t: 16, b: 20 };
  }

  push(times, series) {
    for (let n = 0; n < times.length; n++) {
      this.t.push(times[n]);
      for (const tr of this.traces) {
        const column = series[tr.key];
        const raw = column ? column[n] : null;
        this.values.get(tr.key).push(raw == null ? null : raw / tr.scale);
      }
    }
  }

  prune(now) {
    const cutoff = now - this.window;
    let drop = 0;
    while (drop < this.t.length && this.t[drop] < cutoff) drop++;
    if (!drop) return;
    // One splice, not shift() per sample: the arrays are scanned once a frame
    // either way, and leaving stale samples in makes every later scan longer.
    this.t.splice(0, drop);
    for (const list of this.values.values()) list.splice(0, drop);
  }

  clearData() {
    this.t.length = 0;
    for (const list of this.values.values()) list.length = 0;
  }

  draw(now) {
    this.prune(now);
    this.clear();
    const { ctx, pad } = this;
    const x0 = pad.l;
    const x1 = this.w - pad.r;
    const y0 = pad.t;
    const y1 = this.h - pad.b;
    if (x1 <= x0 || y1 <= y0) return;

    const tMax = Math.max(now, this.window);
    const tMin = tMax - this.window;

    let lo = Infinity;
    let hi = -Infinity;
    for (const list of this.values.values()) {
      for (let n = 0; n < list.length; n++) {
        const v = list[n];
        if (v == null || !Number.isFinite(v)) continue;
        if (this.t[n] < tMin) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
    }
    if (!Number.isFinite(lo)) { lo = 0; hi = 1; }
    if (hi - lo < 1e-9) { hi = lo + 1; }
    const margin = (hi - lo) * 0.12;
    lo = Math.min(lo - margin, 0);
    hi += margin;

    const px = (t) => x0 + ((t - tMin) / (tMax - tMin)) * (x1 - x0);
    const py = (v) => y1 - ((v - lo) / (hi - lo)) * (y1 - y0);

    const yTicks = niceTicks(lo, hi, Math.max(2, Math.floor((y1 - y0) / 34)));
    ctx.lineWidth = 1;
    ctx.font = '10px ui-monospace, Consolas, monospace';
    ctx.fillStyle = AXIS;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const v of yTicks.ticks) {
      const y = py(v);
      if (y < y0 - 1 || y > y1 + 1) continue;
      ctx.strokeStyle = GRID;
      ctx.beginPath();
      ctx.moveTo(x0, Math.round(y) + 0.5);
      ctx.lineTo(x1, Math.round(y) + 0.5);
      ctx.stroke();
      ctx.fillText(tickLabel(v, yTicks.step), x0 - 6, y);
    }

    const xTicks = niceTicks(tMin, tMax, Math.max(2, Math.floor((x1 - x0) / 90)));
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (const t of xTicks.ticks) {
      const x = px(t);
      if (x < x0 - 1 || x > x1 + 1) continue;
      ctx.strokeStyle = GRID;
      ctx.beginPath();
      ctx.moveTo(Math.round(x) + 0.5, y0);
      ctx.lineTo(Math.round(x) + 0.5, y1);
      ctx.stroke();
      ctx.fillText(tickLabel(t, xTicks.step) + 's', x, y1 + 4);
    }

    drawFrame(ctx, x0, y0, x1, y1);

    for (const tr of this.traces) {
      const list = this.values.get(tr.key);
      ctx.strokeStyle = tr.colour;
      ctx.lineWidth = 1.4;
      ctx.beginPath();
      let open = false;
      let lastY = 0;
      for (let n = 0; n < list.length; n++) {
        const v = list[n];
        if (v == null || this.t[n] < tMin) { open = false; continue; }
        const x = px(this.t[n]);
        const y = py(v);
        if (!open) { ctx.moveTo(x, y); open = true; }
        else if (tr.step) { ctx.lineTo(x, lastY); ctx.lineTo(x, y); }
        else ctx.lineTo(x, y);
        lastY = y;
      }
      ctx.stroke();
    }

    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    let legendX = x0 + 4;
    for (const tr of this.traces) {
      ctx.fillStyle = tr.colour;
      ctx.fillRect(legendX, y0 + 3, 8, 2);
      legendX += 12;
      ctx.fillStyle = TEXT;
      ctx.fillText(tr.label, legendX, y0 - 2);
      legendX += ctx.measureText(tr.label).width + 12;
    }
  }
}

/* ------------------------------------------------------------------ */
class PhasePlot extends Canvas2D {
  /* The V-I plane. Each sample is a point that fades out over `persist`
   * seconds, so an MPPT run draws its own search: a spiral onto the knee, or a
   * limit cycle hunting around it. */
  constructor(canvas, opts) {
    super(canvas);
    this.xLabel = opts.xLabel;
    this.yLabel = opts.yLabel;
    this.colour = opts.colour || '#ff7f0e';
    this.persist = opts.persist || 60;
    this.pointSize = 3;
    this.showTrail = true;
    this.showBest = true;
    this.showIso = true;
    this.fade = 'linear';
    this.lockAxes = false;
    this.xMaxLock = 1;
    this.yMaxLock = 1;
    this.points = [];
    this.pad = { l: 52, r: 12, t: 12, b: 30 };
  }

  push(t, x, y, p) {
    this.points.push({ t, x, y, p });
  }

  clearData() {
    this.points.length = 0;
  }

  prune(now) {
    const cutoff = now - this.persist;
    let drop = 0;
    while (drop < this.points.length && this.points[drop].t < cutoff) drop++;
    if (drop) this.points.splice(0, drop);
  }

  draw(now) {
    this.prune(now);
    this.clear();
    const { ctx, pad } = this;
    const x0 = pad.l;
    const x1 = this.w - pad.r;
    const y0 = pad.t;
    const y1 = this.h - pad.b;
    if (x1 <= x0 || y1 <= y0) return;

    const live = this.points;
    let xMax = 0.1;
    let yMax = 0.1;
    for (const p of live) {
      if (p.x > xMax) xMax = p.x;
      if (p.y > yMax) yMax = p.y;
    }
    if (this.lockAxes) { xMax = this.xMaxLock; yMax = this.yMaxLock; }
    else { xMax *= 1.15; yMax *= 1.15; }

    const px = (v) => x0 + (v / xMax) * (x1 - x0);
    const py = (v) => y1 - (v / yMax) * (y1 - y0);

    ctx.font = '10px ui-monospace, Consolas, monospace';
    const yTicks = niceTicks(0, yMax, Math.max(2, Math.floor((y1 - y0) / 40)));
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const v of yTicks.ticks) {
      const y = py(v);
      if (y < y0 - 1 || y > y1 + 1) continue;
      ctx.strokeStyle = GRID;
      ctx.beginPath();
      ctx.moveTo(x0, Math.round(y) + 0.5);
      ctx.lineTo(x1, Math.round(y) + 0.5);
      ctx.stroke();
      ctx.fillStyle = AXIS;
      ctx.fillText(tickLabel(v, yTicks.step), x0 - 6, y);
    }
    const xTicks = niceTicks(0, xMax, Math.max(2, Math.floor((x1 - x0) / 70)));
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (const v of xTicks.ticks) {
      const x = px(v);
      if (x < x0 - 1 || x > x1 + 1) continue;
      ctx.strokeStyle = GRID;
      ctx.beginPath();
      ctx.moveTo(Math.round(x) + 0.5, y0);
      ctx.lineTo(Math.round(x) + 0.5, y1);
      ctx.stroke();
      ctx.fillStyle = AXIS;
      ctx.fillText(tickLabel(v, xTicks.step), x, y1 + 5);
    }

    drawFrame(ctx, x0, y0, x1, y1);

    ctx.fillStyle = AXIS;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'bottom';
    ctx.fillText(this.xLabel, x1, this.h - 2);
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    ctx.fillText(this.yLabel, x0 - 46, y0);

    if (!live.length) {
      ctx.fillStyle = AXIS;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText('waiting for telemetry', (x0 + x1) / 2, (y0 + y1) / 2);
      return;
    }

    // Fade to a floor rather than to zero: the oldest points are the shape of
    // the curve and should stay legible while the newest are obviously live.
    const base = this.colour;
    for (const p of live) {
      const age = (now - p.t) / this.persist;
      let alpha = this.fade === 'flat' ? 0.5 : Math.max(0.1, 1 - age);
      if (this.fade === 'sharp') alpha = Math.max(0.07, Math.pow(1 - age, 3));
      ctx.globalAlpha = alpha;
      ctx.fillStyle = base;
      ctx.beginPath();
      ctx.arc(px(p.x), py(p.y), this.pointSize, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.globalAlpha = 1;

    if (this.showTrail && live.length > 1) {
      const tail = live.slice(-60);
      ctx.strokeStyle = base;
      ctx.globalAlpha = 0.45;
      ctx.lineWidth = 1;
      ctx.beginPath();
      tail.forEach((p, n) => (n ? ctx.lineTo(px(p.x), py(p.y)) : ctx.moveTo(px(p.x), py(p.y))));
      ctx.stroke();
      ctx.globalAlpha = 1;
    }

    const last = live[live.length - 1];
    ctx.fillStyle = base;
    ctx.strokeStyle = '#171b21';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.arc(px(last.x), py(last.y), 5.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.stroke();

    if (this.showBest || this.showIso) {
      let best = live[0];
      for (const p of live) if (p.p > best.p) best = p;
      if (this.showIso && best.p > 0) {
        // Constant-power hyperbola through the best point: y = P/x. Anything
        // above this line is a better operating point than has been found,
        // which is the whole question an MPPT run is asking.
        ctx.strokeStyle = '#2ca02c';
        ctx.globalAlpha = 0.55;
        ctx.setLineDash([4, 4]);
        ctx.beginPath();
        const from = Math.max(best.p / yMax, xMax / 400);
        for (let n = 0; n <= 100; n++) {
          const x = from + (xMax - from) * (n / 100);
          const y = best.p / x;
          if (n === 0) ctx.moveTo(px(x), py(y));
          else ctx.lineTo(px(x), py(y));
        }
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.globalAlpha = 1;
      }
      if (this.showBest) {
        star(ctx, px(best.x), py(best.y), 9, '#2ca02c');
        ctx.fillStyle = '#2ca02c';
        ctx.textAlign = 'left';
        ctx.textBaseline = 'bottom';
        ctx.fillText(`${best.p.toFixed(2)} W`, px(best.x) + 10, py(best.y) - 4);
      }
    }
  }
}

function star(ctx, cx, cy, r, colour) {
  ctx.fillStyle = colour;
  ctx.beginPath();
  for (let n = 0; n < 10; n++) {
    const radius = n % 2 ? r * 0.45 : r;
    const angle = (Math.PI / 5) * n - Math.PI / 2;
    const x = cx + Math.cos(angle) * radius;
    const y = cy + Math.sin(angle) * radius;
    if (n === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.closePath();
  ctx.fill();
}
