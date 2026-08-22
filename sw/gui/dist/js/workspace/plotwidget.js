// Plot widget: uPlot renders the traces; the chrome (head, DOM axes, gap
// ribbon, cursor line + readout) is ours so the design's classes and type
// land exactly. Series paths come from the gap-aware builders in interp.js
// (per the signal's appearance): they connect through alignment holes
// (undefined — a slower signal between its own samples) and BREAK at
// explicit gap markers (null), so a line never crosses a tick-count gap.
// [impl->app~views_001~1]

import { icon } from "../icons.js";
import { store, subscribe } from "../state.js";
import { histories } from "./history.js";
import { meta } from "./watchflow.js";
import { traceDashed } from "./colors.js";
import { appearanceOf, resolvedColor } from "./appearance.js";
import { PATH_BUILDERS } from "./interp.js";
import { cursor, setCursorTick, clearCursor } from "./cursor.js";
import { currentWindow, zoomAt, panBy, selectRange } from "./timeline.js";
import { toggleAxesConfig, closeAxesConfigFor } from "./axesconfig.js";

// Wheel-zoom feel (app~views_009 pins 1.0015 per wheel-delta unit): wheel-up
// zooms in (~×0.86 per notch). A taste constant — invert the exponent's sign
// or retune the base if the bench prefers the other direction or pace.
const WHEEL_ZOOM_BASE = 1.0015;

const esc = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const shortName = (path) => path.split(".").pop();

const BASE_STROKE_W = 1.5;
const POINTED_MAX_PX = 40; // app~views_012's pointing threshold

/** First index with xs[i] >= t (xs ascending). */
function lowerBound(xs, t) {
  let lo = 0, hi = xs.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (xs[mid] < t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

export class PlotWidget {
  /** cfg: { id, signals: [path], sides: {path:'L'|'R'}, scales: {L|R: {mode:'auto'|'manual', min, max}} } */
  constructor(cfg, hooks) {
    this.cfg = { sides: {}, scales: {}, ...cfg };
    // Pre-axes-config snapshots stored an axisMode: 'shared' meant one scale
    // (sides cleared), 'split' kept its sides. The mode is now derived from
    // the per-signal assignments.
    if (this.cfg.axisMode === "shared") this.cfg.sides = {};
    delete this.cfg.axisMode;
    this.hooks = hooks; // { onChange(), onRemove(id) }
    this.page = 0;      // readout page (one per Y scale)
    this.el = document.createElement("div");
    this.el.className = "plot-widget widget";
    this.el.dataset.widgetId = this.cfg.id;
    this.uplot = null;
    this.window = currentWindow();
    this.pointed = null;    // transient pointed-trace path (app~views_012)
    this.pointedRaf = null;
    this.render();
    this.unsubs = [
      subscribe("cursor", () => this.renderCursor()),
      subscribe("timeline", () => {
        if (store.timeline.mode !== "paused") this.applyPointed(null);
        this.refresh();
      }),
    ];
  }

  title() {
    return this.cfg.title || (this.cfg.signals[0] ? shortName(this.cfg.signals[0]) : "Plot");
  }

  render() {
    const split = this.isSplit();
    this.el.classList.toggle("plot-widget--split", split);
    this.el.innerHTML = `
      <div class="widget-head" data-drag-handle>
        ${icon("grip-vertical")}
        <span class="widget-title display">${esc(this.title())}</span>
        <button class="axis-mode axis-mode--derived" data-axesconfig
          title="Axes configuration">${split ? "split" : "shared Y"}</button>
        <span class="widget-legend"></span>
        <button class="widget-menu" data-axesconfig aria-label="Widget menu">${icon("more-horizontal")}</button>
      </div>
      <div class="plot-body">
        <div class="y-axis y-axis--left"></div>
        <div class="plot-canvas">
          <div class="plot-grid"></div>
          <div class="uplot-host"></div>
          <div class="gap-ribbon"></div>
          <span class="plot-watermark">canvas · uPlot${split ? " · 2 scales" : ""}</span>
          <div class="plot-empty-hint" hidden>drop a signal to trace it</div>
          <div class="cursor-line" hidden></div>
          <div class="cursor-readout" hidden></div>
        </div>
        <div class="y-axis y-axis--right" ${split ? "" : "hidden"}></div>
      </div>
      <div class="x-axis"></div>
      <div class="resize-handle" data-resize aria-hidden="true"></div>`;

    // Both the ⋯ menu and the derived axis chip open the axes configuration —
    // assignment, scale modes, and signal removal all live there.
    this.el.querySelectorAll("[data-axesconfig]").forEach((b) =>
      b.addEventListener("click", (ev) => {
        ev.stopPropagation();
        toggleAxesConfig(this, b);
      }),
    );

    const canvas = this.el.querySelector(".plot-canvas");
    canvas.addEventListener("pointermove", (ev) => {
      if (!this.uplot) return;
      const rect = canvas.getBoundingClientRect();
      const tick = Math.round(this.uplot.posToVal(ev.clientX - rect.left, "x"));
      setCursorTick(tick);
      this.trackSelect(canvas, ev);
      this.trackPointed(ev.clientX, ev.clientY, canvas);
    });
    canvas.addEventListener("pointerleave", () => {
      clearCursor();
      this.applyPointed(null);
    });
    this.wireTimelineActions(canvas);

    this.renderLegend();
    this.rebuild();
  }

  // ── paused-range controls on the canvas ──────────────────────────────────
  // Wheel zooms about the held cursor time (range center without one);
  // horizontal scroll / Shift+wheel pans at the current scale; dragging a
  // horizontal section zooms every plot to it. All no-ops while live.
  // [impl->app~views_009~1]

  wireTimelineActions(canvas) {
    canvas.addEventListener(
      "wheel",
      (ev) => {
        if (store.timeline.mode !== "paused") return;
        ev.preventDefault();
        const rect = canvas.getBoundingClientRect();
        const msPerPx = (this.window[1] - this.window[0]) / Math.max(1, rect.width);
        const dx = ev.deltaX || (ev.shiftKey ? ev.deltaY : 0);
        if (dx && Math.abs(dx) >= Math.abs(ev.shiftKey ? 0 : ev.deltaY)) {
          panBy(dx * msPerPx);
        } else {
          zoomAt(cursor.tick, Math.pow(WHEEL_ZOOM_BASE, ev.deltaY));
        }
      },
      { passive: false },
    );

    canvas.addEventListener("pointerdown", (ev) => {
      if (store.timeline.mode !== "paused" || ev.button !== 0) return;
      this.sel = { x0: ev.clientX, active: false, el: null };
      canvas.setPointerCapture?.(ev.pointerId);
    });
    const end = (ev) => {
      const sel = this.sel;
      if (!sel) return;
      this.sel = null;
      sel.el?.remove();
      if (!sel.active) return;
      const rect = canvas.getBoundingClientRect();
      const [t0, t1] = this.window;
      const toTick = (x) => t0 + ((x - rect.left) / Math.max(1, rect.width)) * (t1 - t0);
      selectRange(toTick(Math.min(sel.x0, ev.clientX)), toTick(Math.max(sel.x0, ev.clientX)));
    };
    canvas.addEventListener("pointerup", end);
    canvas.addEventListener("pointercancel", () => {
      this.sel?.el?.remove();
      this.sel = null;
    });
  }

  // ── pointed-trace emphasis (paused only, app~views_012) ──────────────────
  // The trace whose rendered line lies nearest the pointer (≤ 40 px, pixel
  // space — split axes handled inherently since each series converts through
  // its own scale) doubles its stroke and its readout row is marked.
  // Mechanism: mutate series.width and redraw(false) — restroke only, no
  // data or path rebuild — then toggle row classes in place. Transient: the
  // persisted appearance and cfg are never touched.
  // [impl->app~views_012~1]

  trackPointed(clientX, clientY, canvas) {
    if (this.pointedRaf) return;
    this.pointedRaf = requestAnimationFrame(() => {
      this.pointedRaf = null;
      this.applyPointed(this.computePointed(clientX, clientY, canvas));
    });
  }

  /** The nearest pointable trace at the pointer, or null. A series is
   *  pointable at x only where a sample-bearing segment exists: inside a
   *  tick-count gap (a null marker on either side) it does not participate. */
  computePointed(clientX, clientY, canvas) {
    if (store.timeline.mode !== "paused" || !this.uplot) return null;
    const rect = canvas.getBoundingClientRect();
    const px = clientX - rect.left;
    const py = clientY - rect.top;
    const t = this.uplot.posToVal(px, "x");
    const xs = this.uplot.data[0];
    if (!xs.length) return null;
    let best = null;
    for (let s = 1; s < this.uplot.data.length; s++) {
      const path = this.cfg.signals[s - 1];
      const ys = this.uplot.data[s];
      let iR = lowerBound(xs, t);
      let iL = iR - 1;
      while (iL >= 0 && ys[iL] === undefined) iL--;      // alignment holes
      while (iR < xs.length && ys[iR] === undefined) iR++;
      const vL = iL >= 0 ? ys[iL] : undefined;
      const vR = iR < xs.length ? ys[iR] : undefined;
      // A null neighbor is a gap marker: no segment here. Off either end of
      // the data, clamp to the nearest run endpoint instead.
      let yVal;
      if (vL != null && vR != null) {
        yVal = appearanceOf(path).interp === "zoh"
          ? vL // the rendered step holds the left value until the next sample
          : vL + ((vR - vL) * (t - xs[iL])) / (xs[iR] - xs[iL] || 1);
      } else if (vL != null && vR === undefined) {
        yVal = vL;
      } else if (vR != null && vL === undefined) {
        yVal = vR;
      } else {
        continue; // inside a gap (or empty series)
      }
      const yPx = this.uplot.valToPos(yVal, this.sideOf(path), false);
      const dy = Math.abs(py - yPx);
      if (dy <= POINTED_MAX_PX && (!best || dy < best.dy)) best = { path, dy };
    }
    return best ? best.path : null;
  }

  applyPointed(path) {
    if (path === this.pointed) return;
    this.pointed = path;
    if (this.uplot) {
      let restroke = false;
      this.uplot.series.forEach((s, i) => {
        if (i === 0) return;
        const want = this.cfg.signals[i - 1] === path ? BASE_STROKE_W * 2 : BASE_STROKE_W;
        if (s.width !== want) {
          s.width = want;
          restroke = true;
        }
      });
      if (restroke) this.uplot.redraw(false);
    }
    this.el.querySelectorAll(".cursor-readout .readout-row").forEach((r) =>
      r.classList.toggle("readout-row--pointed", r.dataset.path === path),
    );
  }

  /** Grow the drag-select region under the pointer (called from pointermove). */
  trackSelect(canvas, ev) {
    const sel = this.sel;
    if (!sel) return;
    const dx = ev.clientX - sel.x0;
    if (!sel.active && Math.abs(dx) > 6) {
      sel.active = true;
      sel.el = document.createElement("div");
      sel.el.className = "select-region";
      canvas.appendChild(sel.el);
    }
    if (sel.active) {
      const rect = canvas.getBoundingClientRect();
      sel.el.style.left = `${Math.min(sel.x0, ev.clientX) - rect.left}px`;
      sel.el.style.width = `${Math.abs(dx)}px`;
    }
  }

  addSignal(path) {
    if (this.cfg.signals.includes(path)) return;
    this.cfg.signals.push(path);
    this.renderLegend();
    this.rebuild();
    this.hooks.onChange();
  }

  /** Deselect: the signal leaves this widget; the widget remains (an emptied
   *  plot shows its drop hint). The caller owns the watch-level cleanup. */
  removeSignal(path) {
    const i = this.cfg.signals.indexOf(path);
    if (i < 0) return;
    this.cfg.signals.splice(i, 1);
    delete this.cfg.sides[path];
    this.page = 0;
    this.rebuild();
    this.hooks.onChange();
  }

  setSide(path, side) {
    if (!this.cfg.signals.includes(path) || this.sideOf(path) === side) return;
    this.cfg.sides[path] = side;
    this.page = 0;
    this.rebuild();
    this.hooks.onChange();
  }

  /** Per-side scale config; auto unless configured manual. */
  scaleConfig(side) {
    return this.cfg.scales[side] || { mode: "auto" };
  }

  setScaleMode(side, mode) {
    if (mode === "manual") {
      // Seed the manual bounds from the range currently on screen, so the
      // switch is a freeze, not a jump.
      const group = this.scaleGroups().find((g) => g.key === side);
      const [min, max] = this.scaleConfig(side).mode === "manual"
        ? [this.cfg.scales[side].min, this.cfg.scales[side].max]
        : this.scaleRange(group ? group.signals : []);
      this.cfg.scales[side] = { mode: "manual", min: round4(min), max: round4(max) };
    } else {
      this.cfg.scales[side] = { mode: "auto" };
    }
    this.refresh();
    this.hooks.onChange();
  }

  setManualRange(side, min, max) {
    this.cfg.scales[side] = { mode: "manual", min, max };
    this.refresh();
    this.hooks.onChange();
  }

  renderLegend() {
    this.el.querySelector(".widget-legend").innerHTML = this.cfg.signals
      .map((p) => {
        const w = store.watched.get(p);
        return `<span class="legend-entry" data-legend="${esc(p)}" title="${esc(p)}">
          <span class="legend-bar" style="background:${w?.color || resolvedColor(p)}"></span>
          <span class="legend-name mono">${esc(shortName(p))}</span>
          <span class="legend-period">${w ? `${w.period_ms}ms` : ""}</span>
        </span>`;
      })
      .join("");
  }

  sideOf(path) {
    return this.cfg.sides[path] || "L";
  }

  /** Scale groups: one Y axis per assigned side in use — the readout
   *  paginates by these, and "shared vs split" is derived from them. */
  // [impl->app~views_007~1]
  scaleGroups() {
    const L = this.cfg.signals.filter((p) => this.sideOf(p) === "L");
    const R = this.cfg.signals.filter((p) => this.sideOf(p) === "R");
    const both = L.length > 0 && R.length > 0;
    return [
      { key: "L", label: both ? "left · scale 1" : null, signals: L },
      { key: "R", label: both ? "right · scale 2" : null, signals: R },
    ].filter((g) => g.signals.length);
  }

  isSplit() {
    return this.scaleGroups().length === 2;
  }

  /** The group's Y range per its scale mode: manual bounds hold while values
   *  clip beyond them; auto follows the rendered extents. */
  axisRange(group) {
    const sc = this.scaleConfig(group.key);
    if (sc.mode === "manual" && Number.isFinite(sc.min) && Number.isFinite(sc.max)) {
      return [sc.min, sc.max];
    }
    return this.scaleRange(group.signals);
  }

  /** uPlot series options realizing the signal's appearance. An explicit
   *  line style — "solid" included — always wins; the 6-color overflow
   *  dash applies only while the style is auto (null). */
  // [impl->app~views_011~1]
  seriesOpts(path) {
    const a = appearanceOf(path);
    const dash =
      a.style === "dotted" ? [2, 3]
      : a.style === "dashed" ? [8, 5]
      : a.style === "solid" ? undefined
      : traceDashed(path) ? [6, 4]
      : undefined;
    return {
      scale: this.sideOf(path),
      stroke: store.watched.get(path)?.color || resolvedColor(path),
      width: BASE_STROKE_W,
      dash,
      points: { show: a.dots, size: 5 },
      paths: PATH_BUILDERS[a.interp] || PATH_BUILDERS.linear,
    };
  }

  /** Tear down and re-create the uPlot instance (assignment/signal changes). */
  rebuild() {
    this.pointed = null; // the new instance strokes at base width
    const split = this.isSplit();
    this.el.classList.toggle("plot-widget--split", split);
    const chip = this.el.querySelector(".axis-mode--derived");
    if (chip) chip.textContent = split ? "split" : "shared Y";
    const mark = this.el.querySelector(".plot-watermark");
    if (mark) mark.textContent = `canvas · uPlot${split ? " · 2 scales" : ""}`;
    this.el.querySelector(".plot-empty-hint").hidden = this.cfg.signals.length > 0;
    this.uplot?.destroy();
    this.uplot = null;
    this.el.querySelector(".uplot-host").innerHTML = "";
    // One axis per assigned side IN USE: an all-right plot shows only the
    // right axis, not a vacant left column's mirror.
    this.el.querySelector(".y-axis--right").hidden = !this.scaleGroups().some((g) => g.key === "R");
    this.renderLegend();
    this.refresh();
  }

  buildData() {
    const tables = this.cfg.signals.map((p) => {
      const h = histories.get(p);
      return h ? h.windowTable(this.window[0], this.window[1]) : [[], []];
    });
    const union = [...new Set(tables.flatMap(([xs]) => xs))].sort((a, b) => a - b);
    const index = new Map(union.map((t, i) => [t, i]));
    const data = [union];
    for (const [xs, ys] of tables) {
      const col = new Array(union.length).fill(undefined);
      xs.forEach((t, i) => (col[index.get(t)] = ys[i]));
      data.push(col);
    }
    return data;
  }

  scaleRange(signals) {
    let min = Infinity, max = -Infinity;
    for (const p of signals) {
      const h = histories.get(p);
      if (!h) continue;
      const [xs, ys] = h.windowTable(this.window[0], this.window[1]);
      for (const v of ys) if (v != null) { if (v < min) min = v; if (v > max) max = v; }
    }
    if (min === Infinity) return [0, 1];
    if (min === max) { min -= 1; max += 1; }
    const pad = (max - min) * 0.08;
    return [min - pad, max + pad];
  }

  ensurePlot(width, height) {
    if (this.uplot) {
      this.uplot.setSize({ width, height });
      return;
    }
    const groups = this.scaleGroups();
    const scales = { x: { time: false } };
    for (const g of groups) scales[g.key] = { auto: false };
    const series = [{}, ...this.cfg.signals.map((p) => this.seriesOpts(p))];
    this.uplot = new uPlot(
      { width, height, scales, series, axes: [{ show: false }, ...groups.map(() => ({ show: false }))],
        legend: { show: false }, cursor: { show: false } },
      this.buildData(),
      this.el.querySelector(".uplot-host"),
    );
  }

  /** Called at batch rate: re-window, re-data, re-label, re-ribbon. The X
   *  range is the app-level timeline's — identical on every plot. */
  refresh() {
    this.window = currentWindow();

    const host = this.el.querySelector(".uplot-host");
    const { width, height } = host.getBoundingClientRect();
    if (width < 20 || height < 20) return;
    this.ensurePlot(Math.floor(width), Math.floor(height));

    const groups = this.scaleGroups();
    const batch = () => {
      // Data BEFORE scales: setData(…, false) re-pends the x scale from its
      // current value, so a setScale("x") queued earlier in the same batch
      // would be clobbered (a null x scale then never recovers, and nothing
      // draws — found the hard way).
      this.uplot.setData(this.buildData(), false);
      this.uplot.setScale("x", { min: this.window[0], max: this.window[1] });
      for (const g of groups) {
        const [min, max] = this.axisRange(g);
        this.uplot.setScale(g.key, { min, max });
        this.renderYAxis(g.key, min, max);
      }
    };
    this.uplot.batch(batch);

    // An axis with no assigned signals shows nothing, not stale labels.
    const live = new Set(groups.map((g) => g.key));
    if (!live.has("L")) this.el.querySelector(".y-axis--left").innerHTML = "";
    if (!live.has("R")) this.el.querySelector(".y-axis--right").innerHTML = "";

    this.renderXAxis();
    this.renderGapRibbon();
    this.renderCursor();
  }

  renderYAxis(key, min, max) {
    const el = this.el.querySelector(key === "R" ? ".y-axis--right" : ".y-axis--left");
    if (!el || el.hidden) { if (key === "L") return; }
    const fmt = (v) => (Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(2));
    const ticks = [...Array(5)].map((_, i) => max - ((max - min) * i) / 4);
    el.innerHTML = ticks.map((v) => `<span class="mono ${key === "R" ? "y-tick--r" : ""}">${fmt(v)}</span>`).join("");
    el.classList.toggle("y-axis--accent", this.isSplit() && key === "L");
  }

  renderXAxis() {
    const [t0, t1] = this.window;
    // More decimals as the paused zoom narrows, so labels stay distinct.
    const dp = t1 - t0 < 2000 ? 3 : 1;
    const fmt = (t) => `${(t / 1000).toFixed(dp)} s`;
    this.el.querySelector(".x-axis").innerHTML =
      `<span class="mono">${fmt(t0)}</span><span class="mono">${fmt((t0 + t1) / 2)}</span><span class="mono">${fmt(t1)}</span>`;
  }

  renderGapRibbon() {
    const [t0, t1] = this.window;
    const spans = new Map(); // merged accent spans across this plot's signals
    for (const p of this.cfg.signals) {
      const h = histories.get(p);
      if (!h) continue;
      for (const [a, b] of h.gapsIn(t0, t1)) spans.set(`${a}:${b}`, [a, b]);
    }
    const pct = (t) => (((t - t0) / (t1 - t0)) * 100).toFixed(2);
    this.el.querySelector(".gap-ribbon").innerHTML = [...spans.values()]
      .map(([a, b]) => `<span class="gap-span" style="left:${pct(a)}%;width:${Math.max(0.4, pct(b) - pct(a))}%"></span>`)
      .join("");
  }

  /** Per-signal cursor value: the signal's own sample at-or-before the
   *  cursor, accepted only within one period — a tick-count gap therefore
   *  reads as absent, never as a stale bridged number. */
  cursorValueFor(path) {
    const h = histories.get(path);
    if (!h) return null;
    const t = h.tickAtOrBefore(cursor.tick);
    if (t === null || cursor.tick - t >= h.period) return null;
    return h.valueAt(t);
  }

  renderCursor() {
    const line = this.el.querySelector(".cursor-line");
    const readout = this.el.querySelector(".cursor-readout");
    const [t0, t1] = this.window;
    if (cursor.tick === null || cursor.tick < t0 || cursor.tick > t1) {
      line.hidden = true;
      readout.hidden = true;
      return;
    }
    const frac = (cursor.tick - t0) / (t1 - t0);
    line.hidden = false;
    line.style.left = `${(frac * 100).toFixed(2)}%`;

    const groups = this.scaleGroups();
    // A signal-less plot has no scale groups — and no readout to render
    // (indexing groups[0] here threw on every shared-cursor move).
    if (!groups.length) {
      readout.hidden = true;
      return;
    }
    if (this.page >= groups.length) this.page = 0;
    const g = groups[this.page];
    const flipped = frac > 0.5;
    readout.hidden = false;
    readout.classList.toggle("cursor-readout--flipped", flipped);
    readout.style.left = flipped ? "" : `calc(${(frac * 100).toFixed(2)}% + 10px)`;
    readout.style.right = flipped ? `calc(${((1 - frac) * 100).toFixed(2)}% + 10px)` : "";
    const rows = g.signals
      .map((p) => {
        const v = this.cursorValueFor(p);
        const w = store.watched.get(p);
        const absent = v === null;
        return `<div class="readout-row ${p === this.pointed ? "readout-row--pointed" : ""}" data-path="${esc(p)}">
          <span class="legend-bar" style="background:${absent ? "var(--ink-hint)" : w?.color || resolvedColor(p)}"></span>
          <span class="readout-name mono">${esc(shortName(p))}</span>
          <span class="readout-value mono ${absent ? "readout-value--absent" : ""}">${absent ? "no sample" : formatValue(v, meta.get(p)?.kind)}</span>
        </div>`;
      })
      .join("");
    readout.innerHTML = `
      <div class="readout-head">
        <span class="readout-time mono">${icon("crosshair")}t = ${cursor.tick.toLocaleString("en-US").replace(/,/g, " ")} ms</span>
        <span class="readout-pager">
          ${groups.length > 1 ? `<button data-page="-1">${icon("chevron-left")}</button>` : ""}
          <span class="mono">${this.page + 1} / ${groups.length}</span>
          ${groups.length > 1 ? `<button data-page="1">${icon("chevron-right")}</button>` : ""}
        </span>
      </div>
      ${g.label ? `<div class="readout-group-label">${esc(g.label)}</div>` : ""}
      ${rows}`;
    readout.querySelectorAll("[data-page]").forEach((b) =>
      b.addEventListener("click", (ev) => {
        ev.stopPropagation();
        this.page = (this.page + +b.dataset.page + groups.length) % groups.length;
        this.renderCursor();
      }),
    );
  }

  destroy() {
    closeAxesConfigFor(this);
    if (this.pointedRaf) cancelAnimationFrame(this.pointedRaf);
    this.unsubs.forEach((u) => u());
    this.uplot?.destroy();
    this.el.remove();
  }

  toJSON() {
    const { id, signals, sides, scales, w, hpx } = this.cfg;
    return { type: "plot", id, signals, sides, scales, w, hpx };
  }
}

const round4 = (v) => (Number.isFinite(v) ? Math.round(v * 10000) / 10000 : v);

// [impl->app~views_006~1] value formatting is shared with the table
export function formatValue(v, kind) {
  if (v === null || v === undefined) return "no sample";
  if (kind === "bool") return v ? "true" : "false";
  if (kind && /^[iu]/.test(kind)) return String(Math.round(v));
  if (kind === "enum") return String(Math.round(v));
  return Math.abs(v) >= 1000 ? v.toFixed(1) : v.toFixed(4);
}
