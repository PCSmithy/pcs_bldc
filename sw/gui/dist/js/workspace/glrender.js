// WebGL2 trace renderer: each segment is one instanced screen-space quad,
// antialiased against a capsule SDF; dash windows key on cumulative arc
// length, so caps/joins/dashes all share the round AA falloff. Geometry is
// CSS px (the backing store carries the DPR). Vertex buffers upload once
// per draw(); cached redraws (pointed width, live scroll) bind + draw only.
// [impl->app~views_015~1]

import { monotoneTangents } from "./interp.js";
import { markDraw, markScroll } from "../perf.js";

const FEATHER_PX = 1.0; // AA falloff half-width
export const DOT_SIZE_PX = 7; // sample-dot diameter, css px

// Browsers cap live WebGL contexts (~16/page) and evict the oldest with no
// restore event; our LRU stays under that — beyond it, plots thrash.
const MAX_LIVE_CONTEXTS = 12;
const liveContexts = new Set(); // GlTraces instances with an alive context
let drawSeq = 0;

const LINE_VS = `#version 300 es
layout(location=0) in vec3 aA;   // x, y (css px), cumulative arc (px)
layout(location=1) in vec3 aB;
uniform vec2 uView;              // css px
uniform float uWidth;            // stroke width, css px
uniform float uFeather;
uniform float uOffsetX;          // live-scroll translation, css px
out vec2 vPos;
flat out vec2 vA;
flat out vec2 vB;
flat out float vArcA;
void main() {
  vec2 a = aA.xy, b = aB.xy;
  vec2 d = b - a;
  float len = max(length(d), 1e-6);
  vec2 dir = d / len;
  vec2 nrm = vec2(-dir.y, dir.x);
  float along = float(gl_VertexID >> 1);            // 0 | 1
  float across = float((gl_VertexID & 1) * 2 - 1);  // -1 | 1
  float pad = uWidth * 0.5 + uFeather + 0.5;
  vec2 pos = a + dir * (along * len + (along * 2.0 - 1.0) * pad) + nrm * across * pad;
  // SDF space (vPos/vA/vB) stays un-offset — the capsule distance and dash
  // arc are translation-invariant; only the clip position shifts.
  vPos = pos; vA = a; vB = b; vArcA = aA.z;
  gl_Position = vec4((pos.x + uOffsetX) / uView.x * 2.0 - 1.0, 1.0 - pos.y / uView.y * 2.0, 0.0, 1.0);
}`;

const LINE_FS = `#version 300 es
precision highp float;
uniform vec4 uColor;   // straight alpha; premultiplied on output
uniform float uWidth;
uniform float uFeather;
uniform vec2 uDash;    // (on px, period px); period <= 0 = solid
in vec2 vPos;
flat in vec2 vA;
flat in vec2 vB;
flat in float vArcA;
out vec4 outColor;
void main() {
  vec2 ab = vB - vA;
  float len = max(length(ab), 1e-6);
  vec2 dir = ab / len;
  float t = clamp(dot(vPos - vA, dir), 0.0, len);
  float dist = length(vPos - (vA + dir * t));
  // Dash windows are capsules: the distance beyond the on-window along the
  // arc combines with the across-line distance, so every dash/dot gets the
  // same round cap and AA falloff as a run end.
  float dArc = 0.0;
  if (uDash.y > 0.0) {
    float s = mod(vArcA + t, uDash.y);
    if (s > uDash.x) dArc = min(s - uDash.x, uDash.y - s);
  }
  float d = length(vec2(dist, dArc));
  float a = 1.0 - smoothstep(uWidth * 0.5 - uFeather * 0.35, uWidth * 0.5 + uFeather, d);
  if (a <= 0.004) discard;
  float pa = uColor.a * a;
  outColor = vec4(uColor.rgb * pa, pa);
}`;

const DOT_VS = `#version 300 es
layout(location=0) in vec2 aP;   // css px
uniform vec2 uView;
uniform float uSizePx;           // dot diameter, css px
uniform float uDpr;
uniform float uOffsetX;          // live-scroll translation, css px
void main() {
  gl_PointSize = (uSizePx + 2.0) * uDpr;
  gl_Position = vec4((aP.x + uOffsetX) / uView.x * 2.0 - 1.0, 1.0 - aP.y / uView.y * 2.0, 0.0, 1.0);
}`;

const DOT_FS = `#version 300 es
precision highp float;
uniform vec4 uColor;
uniform float uSizePx;
out vec4 outColor;
void main() {
  vec2 c = gl_PointCoord * 2.0 - 1.0;                 // [-1,1] across the sprite
  float r = length(c) * (uSizePx + 2.0) * 0.5;        // css px from center
  float a = 1.0 - smoothstep(uSizePx * 0.5 - 0.8, uSizePx * 0.5 + 0.8, r);
  if (a <= 0.004) discard;
  float pa = uColor.a * a;
  outColor = vec4(uColor.rgb * pa, pa);
}`;

function compile(gl, type, src) {
  const s = gl.createShader(type);
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
    throw new Error(`shader: ${gl.getShaderInfoLog(s)}`);
  }
  return s;
}

function link(gl, vs, fs) {
  const p = gl.createProgram();
  gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, vs));
  gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
    throw new Error(`link: ${gl.getProgramInfoLog(p)}`);
  }
  return p;
}

/** "#rrggbb" (or #rgb / rgb(a)(...)) -> [r,g,b,a] floats. */
export function parseColor(css) {
  const s = String(css).trim();
  let m = /^#([0-9a-f]{6})$/i.exec(s);
  if (m) {
    const v = parseInt(m[1], 16);
    return [((v >> 16) & 255) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255, 1];
  }
  m = /^#([0-9a-f]{3})$/i.exec(s);
  if (m) {
    const [r, g, b] = m[1].split("").map((c) => parseInt(c + c, 16) / 255);
    return [r, g, b, 1];
  }
  m = /^rgba?\(([^)]+)\)$/i.exec(s);
  if (m) {
    const p = m[1].split(",").map(parseFloat);
    return [p[0] / 255, p[1] / 255, p[2] / 255, p.length > 3 ? p[3] : 1];
  }
  return [0.9, 0.9, 0.9, 1];
}

export class GlTraces {
  constructor(host) {
    this.canvas = document.createElement("canvas");
    this.canvas.className = "gl-trace-canvas";
    this.canvas.style.cssText = "position:absolute;inset:0;width:100%;height:100%;";
    host.appendChild(this.canvas);
    this.drawCount = 0;
    this.contextLosses = 0;
    this.w = 0;
    this.h = 0;
    this.dpr = 1;
    this._traces = [];
    this._offsetX = 0;
    this.lastDrawSeq = 0;
    this.canvas.addEventListener("webglcontextlost", (ev) => {
      ev.preventDefault();
      this.gl = null;
      liveContexts.delete(this);
    });
    this.canvas.addEventListener("webglcontextrestored", () => {
      this.contextLosses++;
      this._init();
      this._size();
      if (this.gl) this.draw(this._traces, this._offsetX);
    });
    this._init();
  }

  alive() {
    return Boolean(this.gl) && !this.gl.isContextLost();
  }

  /** Give this renderer's context back to the pool (the owner re-creates a
   *  fresh renderer on its next refresh — a lost canvas may never restore). */
  _release() {
    this.gl?.getExtension("WEBGL_lose_context")?.loseContext();
    this.gl = null;
    liveContexts.delete(this);
  }

  _init() {
    // Stay under the browser's context cap: release the least-recently-drawn
    // live renderer before claiming a new context.
    if (liveContexts.size >= MAX_LIVE_CONTEXTS) {
      let lru = null;
      for (const r of liveContexts) {
        if (r !== this && (!lru || r.lastDrawSeq < lru.lastDrawSeq)) lru = r;
      }
      lru?._release();
    }
    const gl = this.canvas.getContext("webgl2", {
      alpha: true,
      antialias: false, // the SDF feather is the AA
      premultipliedAlpha: true,
      preserveDrawingBuffer: false,
      depth: false,
      stencil: false,
    });
    if (!gl || gl.isContextLost()) {
      // Creation can fail under page-global context pressure; the owner
      // retries with a fresh renderer on its next refresh.
      this.gl = null;
      return;
    }
    this.gl = gl;
    liveContexts.add(this);
    this.lineProg = link(gl, LINE_VS, LINE_FS);
    this.dotProg = link(gl, DOT_VS, DOT_FS);
    // Buffer pool, reused across draw() uploads; a lost context invalidates
    // every handle, so the pool resets with the context.
    this._pool = [];
    this._used = 0;
    this.lineU = {
      view: gl.getUniformLocation(this.lineProg, "uView"),
      width: gl.getUniformLocation(this.lineProg, "uWidth"),
      feather: gl.getUniformLocation(this.lineProg, "uFeather"),
      color: gl.getUniformLocation(this.lineProg, "uColor"),
      dash: gl.getUniformLocation(this.lineProg, "uDash"),
      offx: gl.getUniformLocation(this.lineProg, "uOffsetX"),
    };
    this.dotU = {
      view: gl.getUniformLocation(this.dotProg, "uView"),
      size: gl.getUniformLocation(this.dotProg, "uSizePx"),
      dpr: gl.getUniformLocation(this.dotProg, "uDpr"),
      color: gl.getUniformLocation(this.dotProg, "uColor"),
      offx: gl.getUniformLocation(this.dotProg, "uOffsetX"),
    };
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA); // premultiplied over
  }

  resize(wCss, hCss) {
    const dpr = window.devicePixelRatio || 1;
    if (wCss === this.w && hCss === this.h && dpr === this.dpr) return;
    this.w = wCss;
    this.h = hCss;
    this.dpr = dpr;
    this._size();
  }

  _size() {
    this.canvas.width = Math.max(1, Math.round(this.w * this.dpr));
    this.canvas.height = Math.max(1, Math.round(this.h * this.dpr));
  }

  /** traces: [{ runs: [{ verts: Float32Array (x,y,arc)* }], dots: Float32Array|null,
   *             color: [r,g,b,a], widthPx, dash: [on,off]|null, dotSizePx }].
   *  Uploads every buffer ONCE; cached redraws (drawCached / scroll) then
   *  bind + set uniforms + draw. offsetX (css px) is the scroll translation. */
  draw(traces, offsetX = 0) {
    this._traces = traces;
    this._offsetX = offsetX;
    this._uploadAll();
    this.drawCached();
  }

  _upload(data) {
    const gl = this.gl;
    const buf = this._pool[this._used] ?? (this._pool[this._used] = gl.createBuffer());
    this._used++;
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW);
    return buf;
  }

  _uploadAll() {
    const gl = this.gl;
    if (!gl || gl.isContextLost()) return;
    this._used = 0;
    for (const tr of this._traces) {
      for (const run of tr.runs) run._glBuf = this._upload(run.verts);
      tr._glDots = tr.dots && tr.dots.length >= 2 ? this._upload(tr.dots) : null;
    }
  }

  /** Geometry-rate redraw: counts toward the draws honesty metric. */
  drawCached() {
    this.drawCount++;
    markDraw();
    this._render();
  }

  /** Display-rate redraw at a new translation (live smooth scroll): cached
   *  geometry, uniforms + draw calls only. Accounted as a scroll, not a
   *  draw — the views_015 draws-per-batch check must keep meaning
   *  "geometry actually rebuilt and drew". */
  scroll(offsetX) {
    this._offsetX = offsetX;
    markScroll();
    this._render();
  }

  _render() {
    const gl = this.gl;
    if (!gl || gl.isContextLost() || this.w < 1) return;
    this.lastDrawSeq = ++drawSeq;
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    gl.useProgram(this.lineProg);
    gl.uniform2f(this.lineU.view, this.w, this.h);
    gl.uniform1f(this.lineU.feather, FEATHER_PX);
    gl.uniform1f(this.lineU.offx, this._offsetX || 0);
    for (const tr of this._traces) {
      gl.uniform4fv(this.lineU.color, tr.color);
      gl.uniform1f(this.lineU.width, tr.widthPx);
      gl.uniform2f(
        this.lineU.dash,
        tr.dash ? tr.dash[0] : 0,
        tr.dash ? tr.dash[0] + tr.dash[1] : 0,
      );
      for (const run of tr.runs) {
        const n = run.verts.length / 3;
        if (n < 2 || !run._glBuf) continue;
        gl.bindBuffer(gl.ARRAY_BUFFER, run._glBuf);
        gl.enableVertexAttribArray(0);
        gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 12, 0);
        gl.vertexAttribDivisor(0, 1);
        gl.enableVertexAttribArray(1);
        gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 12, 12);
        gl.vertexAttribDivisor(1, 1);
        gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, n - 1);
      }
    }

    gl.useProgram(this.dotProg);
    gl.uniform2f(this.dotU.view, this.w, this.h);
    gl.uniform1f(this.dotU.dpr, this.dpr);
    gl.uniform1f(this.dotU.offx, this._offsetX || 0);
    for (const tr of this._traces) {
      if (!tr._glDots || !tr.dots || tr.dots.length < 2) continue;
      gl.uniform4fv(this.dotU.color, tr.color);
      gl.uniform1f(this.dotU.size, tr.dotSizePx || DOT_SIZE_PX);
      gl.bindBuffer(gl.ARRAY_BUFFER, tr._glDots);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, 0);
      gl.vertexAttribDivisor(0, 0);
      gl.disableVertexAttribArray(1);
      gl.drawArrays(gl.POINTS, 0, tr.dots.length / 2);
    }
  }

  destroy() {
    this._release();
    this.canvas.remove();
  }
}

/** Build one trace's geometry from its decimated window table.
 *  xs/ys: data-space table (null = gap break); mapX/mapY: data -> css px;
 *  interp: 'linear' | 'zoh' | 'cubic'; pxPerMs for the cubic tessellation
 *  density (~1 vertex per px). firstRunArcStart seeds the FIRST run's
 *  cumulative arc: the live window clips that run's left edge, so its
 *  first vertex is a different sample every rebuild — the caller carries
 *  the dropped prefix's arc across rebuilds (plotwidget's dash datum) so a
 *  fixed sample's dash phase holds. Later runs are gap-anchored (their
 *  start is a fixed sample) and stay at 0. Returns { runs, dots }. */
export function buildTraceGeometry(xs, ys, mapX, mapY, interp, pxPerMs, wantDots, firstRunArcStart = 0) {
  const runs = [];
  const dotPts = wantDots ? [] : null;
  let i = 0;
  const n = xs.length;
  // A non-finite sample is a run-breaker like a gap marker: it never enters
  // a vertex buffer (one NaN in the cumulative arc would poison the dash
  // window for the rest of the run), and the segments beside it drop.
  const breaker = (v) => v === null || !Number.isFinite(v);
  while (i < n) {
    if (breaker(ys[i])) { i++; continue; }
    let j = i;
    while (j < n && !breaker(ys[j])) j++;
    if (dotPts) for (let k = i; k < j; k++) dotPts.push(mapX(xs[k]), mapY(ys[k]));
    runs.push(buildRun(xs, ys, i, j, mapX, mapY, interp, pxPerMs, runs.length ? 0 : firstRunArcStart));
    i = j;
  }
  return { runs, dots: dotPts ? new Float32Array(dotPts) : null };
}

/** Arc value at pixel x along a run's (x, y, arc) vertex strip, or null
 *  when x falls outside the run (the caller re-anchors). Vertex x ascends
 *  (a ZOH riser repeats one x); within a straight segment arc is exactly
 *  linear in distance, so the bracketing segment interpolates it. */
export function arcAtX(verts, x) {
  if (!verts || verts.length < 3 || x < verts[0] - 1e-6) return null;
  for (let k = 0; k + 3 < verts.length; k += 3) {
    if (verts[k + 3] >= x) {
      const xA = verts[k], yA = verts[k + 1], aA = verts[k + 2];
      const dx = verts[k + 3] - xA;
      if (dx <= 1e-9) return verts[k + 5]; // riser: the far vertex's arc
      const t = (x - xA) / dx;
      const y = yA + t * (verts[k + 4] - yA);
      return aA + Math.hypot(x - xA, y - yA);
    }
  }
  const lastX = verts[verts.length - 3];
  return Math.abs(x - lastX) <= 1e-6 ? verts[verts.length - 1] : null;
}

/** Builds the [i0, i1) run of xs/ys — index ranges, no per-run copies. */
function buildRun(xs, ys, i0, i1, mapX, mapY, interp, pxPerMs, arcStart = 0) {
  let px = [], py = [];
  if (interp === "zoh") {
    for (let k = i0; k < i1; k++) {
      if (k > i0) { px.push(mapX(xs[k])); py.push(py[py.length - 1]); }
      px.push(mapX(xs[k]));
      py.push(mapY(ys[k]));
    }
  } else if (interp === "cubic" && i1 - i0 > 2) {
    // Hermite evaluation from the Fritsch–Carlson tangents, ~1 vertex/px.
    const runX = xs.slice(i0, i1), runY = ys.slice(i0, i1);
    const m = monotoneTangents(runX, runY);
    for (let k = 0; k < runX.length - 1; k++) {
      const x0 = runX[k], x1 = runX[k + 1];
      const steps = Math.max(1, Math.min(256, Math.ceil((x1 - x0) * pxPerMs)));
      for (let s = 0; s < steps; s++) {
        const t = s / steps;
        const h = x1 - x0;
        const t2 = t * t, t3 = t2 * t;
        const v =
          (2 * t3 - 3 * t2 + 1) * runY[k] +
          (t3 - 2 * t2 + t) * h * m[k] +
          (-2 * t3 + 3 * t2) * runY[k + 1] +
          (t3 - t2) * h * m[k + 1];
        px.push(mapX(x0 + t * h));
        py.push(mapY(v));
      }
    }
    px.push(mapX(runX[runX.length - 1]));
    py.push(mapY(runY[runY.length - 1]));
  } else {
    for (let k = i0; k < i1; k++) {
      px.push(mapX(xs[k]));
      py.push(mapY(ys[k]));
    }
  }
  const verts = new Float32Array(px.length * 3);
  let arc = arcStart;
  for (let k = 0; k < px.length; k++) {
    if (k > 0) arc += Math.hypot(px[k] - px[k - 1], py[k] - py[k - 1]);
    verts[k * 3] = px[k];
    verts[k * 3 + 1] = py[k];
    verts[k * 3 + 2] = arc;
  }
  return { verts };
}
