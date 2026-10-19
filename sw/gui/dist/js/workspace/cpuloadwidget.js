// CPU load widget: a donut of how CPU time splits between the configured
// cumulative-µs counters over a 1 s window, the idle share as its headline,
// a legend, and a 30 s strip of past windows. Task and idle counters
// partition elapsed time; interrupt time is charged inside them, so each
// task/idle share is scaled by (1 − I/T).
// [impl->app~views_020~1]

import { icon } from "../icons.js";
import { store, subscribe } from "../state.js";
import { histories } from "./history.js";
import { addWatch } from "./watchflow.js";
import { currentWindow } from "./timeline.js";
import { cursor } from "./cursor.js";
import { toggleAxesConfig, closeAxesConfigFor } from "./axesconfig.js";
import { wireTitleEditor } from "./titlebar.js";
import { throttleTrailing } from "../perf.js";
import { esc, shortName } from "../dom.js";

export const CPU_LOAD_PERIOD = 200; // 10 ms, the slowest legal watch period
export const CPU_ROLES = ["task", "idle", "interrupt"];

const WINDOW_MS = 1000;
const STRIP_MS = 30_000;
const STRIP_STEP_MS = 100;
const STRIP_COLS = STRIP_MS / STRIP_STEP_MS;
const U32 = 2 ** 32;

// [impl->app~views_021~1]
export function cpuLoadPreset() {
  const tasks = ["other", "idle", "1 ms", "10 ms", "200 ms", "usb", "server"];
  return [
    ...tasks.map((label, i) => ({
      path: `app_cpuLoad_data.taskTime_us[${i}]`,
      label,
      role: label === "idle" ? "idle" : "task",
    })),
    { path: "HW_ADC_data.irqTime_us", label: "cycle ISR", role: "interrupt" },
  ];
}

/** Shares from per-wedge counter deltas; null when T is 0. */
export function cpuShares(roles, deltas) {
  let T = 0, I = 0;
  roles.forEach((r, i) => {
    if (r === "interrupt") I += deltas[i];
    else T += deltas[i];
  });
  if (!(T > 0)) return null;
  const scale = 1 - I / T;
  return roles.map((r, i) => (r === "interrupt" ? deltas[i] / T : (deltas[i] / T) * scale));
}

const wrapDelta = (a, b) => (((a - b) % U32) + U32) % U32;

function sampleAtOrBefore(h, t) {
  const i = h.indexAfter(t) - 1;
  if (i < 0) return null;
  const v = h.valueAtIndex(i);
  return v === null ? null : { t: h.tickAtIndex(i), v };
}

/** The window ending at tEnd: each counter's wrapping increase from its
 *  sample at-or-before tEnd − 1 s (else its oldest) to its sample
 *  at-or-before tEnd. */
function windowDeltas(hs, tEnd) {
  return hs.map((h) => {
    if (!h || !h.size) return 0;
    const b = sampleAtOrBefore(h, tEnd);
    if (!b) return 0;
    const a = sampleAtOrBefore(h, tEnd - WINDOW_MS) ?? (h.tickAtIndex(0) < b.t ? { v: h.valueAtIndex(0) } : null);
    return a && a.v !== null ? wrapDelta(b.v, a.v) : 0;
  });
}

const pct = (s) => `${(s * 100).toFixed(1)} %`;
const relTime = (dt) => `${dt < 0 ? "−" : ""}${(Math.abs(dt) / 1000).toFixed(1)} s`;

const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const TASK_COLORS = ["--trace-3", "--trace-4", "--trace-5", "--trace-6", "--color-accent-300", "--color-neutral-500"];

export class CpuLoadWidget {
  /** cfg: { id, wedges: [{ path, label, role }] } */
  constructor(cfg, hooks) {
    this.cfg = cfg;
    this.cfg.wedges ??= cpuLoadPreset();
    // Layout and the configuration popover read a widget's signal set.
    Object.defineProperty(this.cfg, "signals", {
      get: () => this.cfg.wedges.map((w) => w.path),
      configurable: true,
      enumerable: false,
    });
    this.hooks = hooks;
    this.shares = null;      // the shares on display: live, scrubbed, or at the cursor
    this.liveShares = null;
    this.liveEnd = null;
    this.strip = [];
    this.stripTicks = [];
    this.hoverIdx = null;    // hovered wedge or legend row
    this.scrubCol = null;    // hovered strip column, 0..STRIP_COLS-1
    this.el = document.createElement("div");
    this.el.className = "cpu-widget widget";
    this.el.dataset.widgetId = cfg.id;
    this.refreshBatch = throttleTrailing(() => this.refresh());
    this.render();
    this.ensureWatches();
    this.unsubs = [
      subscribe("gate", (g) => { if (g === "matched") this.ensureWatches(); }),
      subscribe("timeline", () => this.refresh()),
      subscribe("cursor", () => this.paint()),
    ];
  }

  title() {
    return this.cfg.title || "CPU load";
  }

  ensureWatches() {
    for (const { path } of this.cfg.wedges) {
      if (!store.watched.has(path)) addWatch(path, CPU_LOAD_PERIOD);
    }
  }

  render() {
    this.el.innerHTML = `
      <div class="widget-head" data-drag-handle>
        ${icon("grip-vertical")}
        <span class="widget-title display">${esc(this.title())}</span>
        <span class="widget-tag">1 s window</span>
        <span class="widget-legend"></span>
        <button class="widget-menu" data-axesconfig aria-label="Widget menu">${icon("more-horizontal")}</button>
      </div>
      <div class="cpu-body">
        <div class="cpu-donut">
          <canvas class="cpu-donut-canvas"></canvas>
          <div class="cpu-centre">
            <span class="cpu-idle-pct display"></span>
            <span class="cpu-idle-caption">idle</span>
            <span class="cpu-when mono" hidden></span>
          </div>
          <div class="cpu-tooltip" hidden></div>
        </div>
        <div class="cpu-side">
          <div class="cpu-legend"></div>
          <div class="cpu-empty">waiting for samples</div>
        </div>
      </div>
      <canvas class="cpu-strip" aria-label="Last 30 s of shares"></canvas>
      <div class="resize-handle" data-resize aria-hidden="true"></div>`;
    const menu = this.el.querySelector("[data-axesconfig]");
    menu.addEventListener("click", (ev) => {
      ev.stopPropagation();
      toggleAxesConfig(this, menu);
    });
    wireTitleEditor(this);
    this.wireHover();
    this.refresh();
  }

  // [impl->app~views_022~1] wedge and legend hover, strip scrub.
  wireHover() {
    const donut = this.el.querySelector(".cpu-donut-canvas");
    donut.addEventListener("pointermove", (ev) => this.setHover(this.wedgeAt(ev)));
    donut.addEventListener("pointerleave", () => this.setHover(null));
    const legend = this.el.querySelector(".cpu-legend");
    legend.addEventListener("pointerover", (ev) => {
      const row = ev.target.closest(".cpu-legend-row");
      if (row) this.setHover(this.cfg.wedges.findIndex((w) => w.path === row.dataset.path));
    });
    legend.addEventListener("pointerleave", () => this.setHover(null));
    const strip = this.el.querySelector(".cpu-strip");
    strip.addEventListener("pointermove", (ev) => {
      const r = strip.getBoundingClientRect();
      const col = Math.floor(((ev.clientX - r.left) / r.width) * STRIP_COLS);
      const k = Math.min(STRIP_COLS - 1, Math.max(0, col));
      if (k !== this.scrubCol) {
        this.scrubCol = k;
        this.paint();
      }
    });
    strip.addEventListener("pointerleave", () => {
      this.scrubCol = null;
      this.paint();
    });
  }

  setHover(i) {
    const idx = i === null || i < 0 ? null : i;
    if (idx === this.hoverIdx) return;
    this.hoverIdx = idx;
    this.paint();
  }

  /** The wedge under the pointer on the displayed donut, or null. */
  wedgeAt(ev) {
    const g = this._donutGeo;
    if (!g || !this.shares) return null;
    const r = ev.currentTarget.getBoundingClientRect();
    const dx = ev.clientX - r.left - g.cx, dy = ev.clientY - r.top - g.cy;
    const d = Math.hypot(dx, dy);
    if (d < g.rIn || d > g.rOut) return null;
    const a = (Math.atan2(dy, dx) + Math.PI / 2 + 2 * Math.PI) % (2 * Math.PI);
    let acc = 0;
    for (let i = 0; i < this.shares.length; i++) {
      acc += this.shares[i] * 2 * Math.PI;
      if (a < acc) return i;
    }
    return null;
  }

  /** A dropped signal joins as a task wedge. */
  addSignal(path) {
    if (this.cfg.wedges.some((w) => w.path === path)) return;
    this.cfg.wedges.push({ path, label: shortName(path), role: "task" });
    this.refresh();
    this.hooks.onChange();
  }

  removeSignal(path) {
    const i = this.cfg.wedges.findIndex((w) => w.path === path);
    if (i < 0) return;
    this.cfg.wedges.splice(i, 1);
    this.refresh();
    this.hooks.onChange();
  }

  /** Edit one wedge's label or role. */
  setWedge(path, patch) {
    const w = this.cfg.wedges.find((x) => x.path === path);
    if (!w) return;
    Object.assign(w, patch);
    this.refresh();
    this.hooks.onChange();
  }

  colorOf(i) {
    const role = this.cfg.wedges[i].role;
    if (role === "idle") return `rgb(${cssVar("--ok-rgb")})`;
    if (role === "interrupt") return cssVar("--color-accent");
    const k = this.cfg.wedges.slice(0, i).filter((w) => w.role === "task").length;
    return cssVar(TASK_COLORS[k % TASK_COLORS.length]);
  }

  /** The newest time every counter with samples has reached. */
  endTick() {
    let end = Infinity;
    for (const { path } of this.cfg.wedges) {
      const t = histories.get(path)?.newestTick();
      if (t !== null && t !== undefined) end = Math.min(end, t);
    }
    if (store.timeline.mode === "paused") end = Math.min(end, currentWindow()[1]);
    return Number.isFinite(end) ? end : null;
  }

  sharesAt(end) {
    const roles = this.cfg.wedges.map((w) => w.role);
    const hs = this.cfg.wedges.map((w) => histories.get(w.path));
    return cpuShares(roles, windowDeltas(hs, end));
  }

  /** The live window and the strip, recomputed per batch. */
  evaluate() {
    const end = this.endTick();
    this.liveEnd = end;
    this.liveShares = end === null ? null : this.sharesAt(end);
    this.strip = [];
    this.stripTicks = [];
    if (end === null) return;
    const last = Math.floor(end / STRIP_STEP_MS) * STRIP_STEP_MS;
    for (let t = last - STRIP_MS + STRIP_STEP_MS; t <= last; t += STRIP_STEP_MS) {
      this.strip.push(this.sharesAt(t));
      this.stripTicks.push(t);
    }
  }

  /** The window on display: a hovered strip column wins, then the shared
   *  cursor, then live. */
  displayed() {
    if (this.liveEnd === null) return { mode: "live", end: null };
    const k = this.scrubCol === null ? -1 : this.scrubCol - (STRIP_COLS - this.strip.length);
    if (k >= 0) return { mode: "scrub", end: this.stripTicks[k], col: this.scrubCol };
    if (cursor.tick !== null) return { mode: "cursor", end: Math.min(cursor.tick, this.liveEnd) };
    return { mode: "live", end: this.liveEnd };
  }

  refresh() {
    this.refreshBatch.touch();
    this.evaluate();
    this.paint();
  }

  // [impl->app~views_022~1] the donut follows a scrubbed column or the cursor.
  paint() {
    const shown = this.displayed();
    const shares = shown.mode === "live" ? this.liveShares : this.sharesAt(shown.end);
    this.shares = shares;
    const colors = this.cfg.wedges.map((_, i) => this.colorOf(i));
    const idle = shares
      ? shares.reduce((a, s, i) => a + (this.cfg.wedges[i].role === "idle" ? s : 0), 0)
      : null;
    this.el.classList.toggle("cpu-widget--empty", !shares);
    this.el.querySelector(".cpu-idle-pct").textContent = idle === null ? "—" : pct(idle);
    const when = this.el.querySelector(".cpu-when");
    when.hidden = shown.mode === "live";
    when.dataset.mode = shown.mode;
    if (!when.hidden) {
      const rel = relTime(shown.end - this.liveEnd);
      when.innerHTML = shown.mode === "cursor" ? `${icon("crosshair")}${rel}` : rel;
    }
    this.el.querySelector(".cpu-legend").innerHTML = this.cfg.wedges
      .map(
        (w, i) => `<div class="cpu-legend-row ${i === this.hoverIdx ? "cpu-legend-row--hover" : ""}" data-path="${esc(w.path)}" data-role="${w.role}" title="${esc(w.path)}">
          <span class="cpu-swatch" style="background:${colors[i]}"></span>
          <span class="cpu-label">${esc(w.label)}</span>
          <span class="cpu-pct mono">${shares ? pct(shares[i]) : "—"}</span>
        </div>`,
      )
      .join("");
    this.drawDonut(shares, colors);
    this.drawTooltip(shares, colors);
    let markCol = null;
    if (shown.mode === "scrub") markCol = shown.col;
    else if (shown.mode === "cursor" && this.stripTicks.length) {
      const k = Math.round((shown.end - this.stripTicks[0]) / STRIP_STEP_MS);
      if (k >= 0 && k < this.stripTicks.length) markCol = STRIP_COLS - this.stripTicks.length + k;
    }
    this.drawStrip(colors, markCol);
  }

  drawTooltip(shares, colors) {
    const tip = this.el.querySelector(".cpu-tooltip");
    const i = this.hoverIdx;
    const g = this._donutGeo;
    if (i === null || i >= this.cfg.wedges.length || !shares || !g) {
      tip.hidden = true;
      return;
    }
    const w = this.cfg.wedges[i];
    tip.innerHTML = `<span class="cpu-swatch" style="background:${colors[i]}"></span><span class="cpu-tip-label">${esc(w.label)}</span> <span class="cpu-tip-value mono">${pct(shares[i])}</span>`;
    tip.hidden = false;
    // Anchored just outside the wedge's mid-angle, on the side it points to.
    const before = shares.slice(0, i).reduce((a, s) => a + s, 0);
    const mid = (before + shares[i] / 2) * 2 * Math.PI - Math.PI / 2;
    const x = g.cx + Math.cos(mid) * g.rOut;
    const y = g.cy + Math.sin(mid) * g.rOut;
    const right = Math.cos(mid) >= 0;
    tip.style.left = right ? `${x + 8}px` : "";
    tip.style.right = right ? "" : `${g.width - x + 8}px`;
    tip.style.top = `${Math.max(0, y - 12)}px`;
  }

  fitCanvas(canvas, width, height) {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(width * dpr));
    const h = Math.max(1, Math.round(height * dpr));
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);
    return ctx;
  }

  drawDonut(shares, colors) {
    const canvas = this.el.querySelector(".cpu-donut-canvas");
    const { width, height } = canvas.getBoundingClientRect();
    if (width < 20 || height < 20) return;
    const ctx = this.fitCanvas(canvas, width, height);
    const r = Math.min(width, height) / 2 - 4;
    const ring = Math.max(10, r * 0.28);
    const cx = width / 2, cy = height / 2;
    this._donutGeo = { cx, cy, rIn: r - ring, rOut: r, width };
    ctx.lineWidth = ring;
    if (!shares) {
      ctx.strokeStyle = `rgba(${cssVar("--hair-rgb")}, 0.08)`;
      ctx.beginPath();
      ctx.arc(cx, cy, r - ring / 2, 0, 2 * Math.PI);
      ctx.stroke();
      return;
    }
    const hover = this.hoverIdx;
    let a = -Math.PI / 2;
    shares.forEach((s, i) => {
      if (!(s > 0)) return;
      const b = a + s * 2 * Math.PI;
      ctx.globalAlpha = hover === null || hover === i ? 1 : 0.4;
      ctx.lineWidth = hover === i ? ring + 6 : ring;
      ctx.strokeStyle = colors[i];
      ctx.beginPath();
      ctx.arc(cx, cy, r - ring / 2, a, b);
      ctx.stroke();
      a = b;
    });
    ctx.globalAlpha = 1;
  }

  drawStrip(colors, markCol) {
    const canvas = this.el.querySelector(".cpu-strip");
    const { width, height } = canvas.getBoundingClientRect();
    if (width < 20 || height < 2) return;
    const ctx = this.fitCanvas(canvas, width, height);
    const n = STRIP_MS / STRIP_STEP_MS;
    const colW = width / n;
    const offset = n - this.strip.length;
    this.strip.forEach((shares, k) => {
      if (!shares) return;
      const x0 = Math.round((offset + k) * colW);
      const x1 = Math.round((offset + k + 1) * colW);
      let acc = 0;
      shares.forEach((s, i) => {
        if (!(s > 0)) return;
        const y0 = Math.round(acc * height);
        acc += s;
        ctx.fillStyle = colors[i];
        ctx.fillRect(x0, y0, x1 - x0, Math.round(acc * height) - y0);
      });
    });
    if (markCol !== null) {
      const x = Math.round((markCol + 0.5) * colW);
      ctx.fillStyle = `rgba(${cssVar("--lit-rgb")}, 0.3)`;
      ctx.fillRect(x - 3, 0, 6, height);
      ctx.fillStyle = cssVar("--color-accent-300");
      ctx.fillRect(x - 1, 0, 2, height);
    }
  }

  destroy() {
    closeAxesConfigFor(this);
    this.refreshBatch.cancel();
    this.unsubs.forEach((u) => u());
    this.el.remove();
  }

  toJSON() {
    const { id, x, y, w, h, title } = this.cfg;
    const wedges = this.cfg.wedges.map(({ path, label, role }) => ({ path, label, role }));
    return { type: "cpuload", id, wedges, x, y, w, h, ...(title ? { title } : {}) };
  }
}
