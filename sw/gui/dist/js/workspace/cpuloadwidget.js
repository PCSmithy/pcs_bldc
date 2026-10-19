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
import { toggleAxesConfig, closeAxesConfigFor } from "./axesconfig.js";
import { wireTitleEditor } from "./titlebar.js";
import { throttleTrailing } from "../perf.js";
import { esc, shortName } from "../dom.js";

export const CPU_LOAD_PERIOD = 200; // 10 ms, the slowest legal watch period
export const CPU_ROLES = ["task", "idle", "interrupt"];

const WINDOW_MS = 1000;
const STRIP_MS = 30_000;
const STRIP_STEP_MS = 100;
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
    this.shares = null;
    this.strip = [];
    this.el = document.createElement("div");
    this.el.className = "cpu-widget widget";
    this.el.dataset.widgetId = cfg.id;
    this.refreshBatch = throttleTrailing(() => this.refresh());
    this.render();
    this.ensureWatches();
    this.unsubs = [
      subscribe("gate", (g) => { if (g === "matched") this.ensureWatches(); }),
      subscribe("timeline", () => this.refresh()),
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
          </div>
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
    this.refresh();
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

  evaluate() {
    const roles = this.cfg.wedges.map((w) => w.role);
    const hs = this.cfg.wedges.map((w) => histories.get(w.path));
    const end = this.endTick();
    this.shares = end === null ? null : cpuShares(roles, windowDeltas(hs, end));
    this.strip = [];
    if (end === null) return;
    const last = Math.floor(end / STRIP_STEP_MS) * STRIP_STEP_MS;
    for (let t = last - STRIP_MS + STRIP_STEP_MS; t <= last; t += STRIP_STEP_MS) {
      this.strip.push(cpuShares(roles, windowDeltas(hs, t)));
    }
  }

  refresh() {
    this.refreshBatch.touch();
    this.evaluate();
    const shares = this.shares;
    const colors = this.cfg.wedges.map((_, i) => this.colorOf(i));
    const pct = (s) => `${(s * 100).toFixed(1)} %`;
    const idle = shares
      ? shares.reduce((a, s, i) => a + (this.cfg.wedges[i].role === "idle" ? s : 0), 0)
      : null;
    this.el.classList.toggle("cpu-widget--empty", !shares);
    this.el.querySelector(".cpu-idle-pct").textContent = idle === null ? "—" : pct(idle);
    this.el.querySelector(".cpu-legend").innerHTML = this.cfg.wedges
      .map(
        (w, i) => `<div class="cpu-legend-row" data-path="${esc(w.path)}" data-role="${w.role}" title="${esc(w.path)}">
          <span class="cpu-swatch" style="background:${colors[i]}"></span>
          <span class="cpu-label">${esc(w.label)}</span>
          <span class="cpu-pct mono">${shares ? pct(shares[i]) : "—"}</span>
        </div>`,
      )
      .join("");
    this.drawDonut(shares, colors);
    this.drawStrip(colors);
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
    ctx.lineWidth = ring;
    if (!shares) {
      ctx.strokeStyle = `rgba(${cssVar("--hair-rgb")}, 0.08)`;
      ctx.beginPath();
      ctx.arc(cx, cy, r - ring / 2, 0, 2 * Math.PI);
      ctx.stroke();
      return;
    }
    let a = -Math.PI / 2;
    shares.forEach((s, i) => {
      if (!(s > 0)) return;
      const b = a + s * 2 * Math.PI;
      ctx.strokeStyle = colors[i];
      ctx.beginPath();
      ctx.arc(cx, cy, r - ring / 2, a, b);
      ctx.stroke();
      a = b;
    });
  }

  drawStrip(colors) {
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
