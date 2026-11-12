// Sequence widget: a timed setpoint sequence under one mode, edited as
// points on a chart (time from start × setpoint, bounded by the mode's
// rating), run on the board as one Command ending in a stop at the
// duration, its progress marked from the board's reported elapsed time.
// The chart draws the written targets as a zero-order hold and the kind's
// target in effect (setpoints.js); a mode of another setpoint kind clears
// the points. Named sequences live in the workspace.
// [impl->app~views_025~1] [impl->app~views_026~1] [impl->app~views_027~1]

import { icon } from "../icons.js";
import { store, subscribe, notify } from "../state.js";
import { toggleAxesConfig, closeAxesConfigFor } from "./axesconfig.js";
import { wireTitleEditor } from "./titlebar.js";
import {
  COMMAND_MODES, EXTRA_FIELDS, kindOf, kindFor, clampToBound, stopCommand, boundLimits, boundOf,
} from "./setpoints.js";
import { fmt, isLive, sendRowHtml, sendWithVerdict, renderSendRow } from "./cmdplane.js";
import { globalNewest, addRunMark } from "./timeline.js";
import { esc } from "../dom.js";
import { REMOVE_KEYS } from "../platform.js";

/** The modes a sequence runs under: every mode with a setpoint kind. */
export const SEQUENCE_MODES = COMMAND_MODES.filter((m) => kindOf(m.mode));

const MARGIN = { l: 48, r: 12, t: 10, b: 20 };
const HIT_PX = 8;
const DEFAULT_DURATION_MS = 2000;
const MIN_DURATION_MS = 10;
const MAX_DURATION_MS = 600_000;
const EXTRA_KEYS = EXTRA_FIELDS.map((e) => e.key);

const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const isSequenceMode = (mode) => SEQUENCE_MODES.some((m) => m.mode === mode);
const capitalized = (s) => s[0].toUpperCase() + s.slice(1);

// ── named sequences: one library per workspace, persisted with the layout ──

const named = new Map();

export function namedSequences() {
  return [...named.keys()].sort();
}

export function saveNamed(name, seq) {
  named.set(name, structuredClone(seq));
  notify("sequences", namedSequences());
}

export function loadNamed(name) {
  const seq = named.get(name);
  return seq ? structuredClone(seq) : null;
}

export function namedEntries() {
  return Object.fromEntries(named);
}

/** Restored as stored; a Load holds each to the editor's bounds. */
export function restoreNamed(entries) {
  named.clear();
  for (const [name, seq] of Object.entries(entries || {})) if (seq && typeof seq === "object") named.set(name, seq);
}

// ── the sequence model ────────────────────────────────────────────────────

/** A point's extra limit fields kept only when finite and nonzero. */
function tidy(p) {
  for (const key of EXTRA_KEYS) if (!Number.isFinite(p[key]) || p[key] === 0) delete p[key];
  return p;
}

/** Points with a finite time and value, time-ordered, their fields copied. */
const clean = (points) =>
  (Array.isArray(points) ? points : [])
    .filter((p) => Number.isFinite(p?.t_ms) && Number.isFinite(p.value))
    .map((p) => tidy({
      t_ms: Math.max(0, Math.round(p.t_ms)), value: p.value, limit: p.limit,
      ...Object.fromEntries(EXTRA_KEYS.map((key) => [key, p[key]])),
    }))
    .sort((a, b) => a.t_ms - b.t_ms);

const heldDuration = (ms) => Math.round(Math.max(MIN_DURATION_MS, Math.min(MAX_DURATION_MS, ms)));

/** The Command a sequence runs as: every point in time order, then a stop
 *  at the duration, so the board ends the run itself. */
export function buildSequenceCommand(cfg) {
  const k = kindFor(cfg.mode);
  const setpoints = cfg.points.map((p) => ({ t_us: p.t_ms * 1000, ...k.wireFields(p) }));
  setpoints.push({ t_us: cfg.duration_ms * 1000, stop: {} });
  return { mode: cfg.mode, setpoints };
}

export class SequenceWidget {
  /** cfg: { id, mode, duration_ms, points: [{ t_ms, value, limit, ...extra limit fields }] } */
  constructor(cfg, hooks) {
    this.cfg = cfg;
    this.cfg.mode = isSequenceMode(cfg.mode) ? cfg.mode : "MODE_FOC_VELOCITY";
    this.cfg.duration_ms = Number.isFinite(cfg.duration_ms) ? heldDuration(cfg.duration_ms) : DEFAULT_DURATION_MS;
    this.cfg.points = this.held(cfg.points);
    Object.defineProperty(this.cfg, "signals", { get: () => [], configurable: true, enumerable: false });
    this.hooks = hooks;
    this.selected = null;    // index into cfg.points
    this.drag = null;
    this.verdict = null;     // the latest send's outcome (cmdplane.js)
    this.sending = false;
    this.run = null;         // { marker_ms, running, seen, duration_ms, started } after an accepted run
    this.pendingMode = null; // a mode of another kind, awaiting Clear or Keep
    this.series = null;      // the drawn target in effect (trajectorySeries)
    this.el = document.createElement("div");
    this.el.className = "sequence-widget widget";
    this.el.dataset.widgetId = cfg.id;
    this.render();
    this.unsubs = [
      subscribe("connection", () => {
        if (!isLive()) this.holdRun();
        this.refresh();
      }),
      subscribe("motorInfo", () => this.rebound()),
      subscribe("sequences", () => this.renderLibrary()),
      subscribe("telemetry", (t) => this.onTelemetry(t)),
      subscribe("command-accepted", () => this.holdRun()),
    ];
  }

  // [impl->app~views_016~1] unset title: a Sequence widget reads `Sequence`.
  title() {
    return this.cfg.title || "Sequence";
  }

  kind() {
    return kindFor(this.cfg.mode);
  }

  /** The vertical bound: the mode's rating, else the kind's fixed fallback. */
  yBound() {
    return boundOf(this.kind(), store.motorInfo);
  }

  render() {
    const modes = SEQUENCE_MODES.map(
      (m) => `<button data-mode="${m.mode}" class="${m.mode === this.cfg.mode ? "is-selected" : ""}">${esc(m.label)}</button>`,
    ).join("");
    const extras = EXTRA_FIELDS.map((e) => `
            <label class="seq-tip-field" data-extra="${e.key}" hidden>${esc(e.short)}
              <input class="mono" data-tip-${e.attr} inputmode="decimal" placeholder="${e.zero}" aria-label="${e.aria}">
              <span>${e.unit}</span>
            </label>`).join("");
    this.el.innerHTML = `
      <div class="widget-head" data-drag-handle>
        ${icon("grip-vertical")}
        <span class="widget-title display">${esc(this.title())}</span>
        <span class="widget-tag seq-unit" data-unit></span>
        <span class="widget-legend"></span>
        <button class="widget-menu" data-axesconfig aria-label="Widget menu">${icon("more-horizontal")}</button>
      </div>
      <div class="seq-body">
        <div class="cmd-row seq-toolbar">
          <span class="seg cmd-modes" role="group" aria-label="Mode">${modes}</span>
          <span class="seq-confirm" data-confirm hidden>
            <span data-confirm-text></span>
            <button class="btn btn-secondary" data-confirm-clear>Clear</button>
            <button class="btn btn-secondary" data-confirm-keep>Keep</button>
          </span>
          <span class="cmd-input">
            <span class="field-label">duration</span>
            <input class="mono" data-duration inputmode="numeric" aria-label="Duration">
            <span class="cmd-unit">ms</span>
            <span class="cmd-hint" title="The run ends with a stop at the duration">then stop</span>
          </span>
          <span class="seq-library">
            <input class="mono" data-seqname placeholder="name" aria-label="Sequence name">
            <button class="btn btn-secondary" data-saveas>Save as</button>
            <select data-load aria-label="Load a named sequence"></select>
          </span>
        </div>
        <div class="seq-chart">
          <canvas class="seq-canvas" tabindex="0" aria-label="Sequence editor"></canvas>
          <div class="seq-marker" hidden></div>
          <div class="seq-tip" hidden>
            <label class="seq-tip-field">t
              <input class="mono" data-tip-t inputmode="numeric" aria-label="Time">
              <span>ms</span>
            </label>
            <label class="seq-tip-field">
              <input class="mono" data-tip-value inputmode="decimal" aria-label="Setpoint">
              <span data-tip-valueunit></span>
            </label>
            <label class="seq-tip-field">limit
              <input class="mono" data-tip-limit inputmode="decimal" placeholder="step" aria-label="Rate limit">
              <span data-tip-unit></span>
            </label>${extras}
          </div>
          <div class="seq-hint" data-hint>click to add a point · drag to move · Delete removes</div>
        </div>
        ${sendRowHtml("run", "Run")}
      </div>
      <div class="resize-handle" data-resize aria-hidden="true"></div>`;
    const menu = this.el.querySelector("[data-axesconfig]");
    menu.addEventListener("click", (ev) => {
      ev.stopPropagation();
      toggleAxesConfig(this, menu);
    });
    wireTitleEditor(this);
    this.canvas = this.el.querySelector(".seq-canvas");
    this.wireToolbar();
    this.wireChart();
    this.renderLibrary();
    this.refresh();
  }

  wireToolbar() {
    this.el.querySelector(".cmd-modes").addEventListener("click", (ev) => {
      const b = ev.target.closest("[data-mode]");
      if (b) this.setMode(b.dataset.mode);
    });
    this.el.querySelector("[data-confirm-clear]").addEventListener("click", () => this.confirmMode(true));
    this.el.querySelector("[data-confirm-keep]").addEventListener("click", () => this.confirmMode(false));
    const duration = this.el.querySelector("[data-duration]");
    const commitDuration = () => this.setDuration(parseFloat(duration.value));
    duration.addEventListener("change", commitDuration);
    duration.addEventListener("keydown", (ev) => {
      if (ev.key === "Enter") commitDuration();
      else if (ev.key === "Escape") this.refresh(true);
    });
    const name = this.el.querySelector("[data-seqname]");
    const saveAs = () => {
      const n = name.value.trim();
      if (!n) return;
      saveNamed(n, this.sequence());
      name.value = "";
    };
    this.el.querySelector("[data-saveas]").addEventListener("click", saveAs);
    name.addEventListener("keydown", (ev) => { if (ev.key === "Enter") saveAs(); });
    const load = this.el.querySelector("[data-load]");
    load.addEventListener("change", () => {
      if (load.value) this.load(load.value);
      load.value = "";
    });
    this.el.querySelector("[data-run]").addEventListener("click", () => this.runSequence());
    this.el.querySelector("[data-stop]").addEventListener("click", () => this.stop());
  }

  // ── the editor gestures [impl->app~views_025~1] ──

  wireChart() {
    const c = this.canvas;
    c.addEventListener("pointerdown", (ev) => {
      if (ev.button !== 0) return;
      c.focus({ preventScroll: true });
      const { x, y } = this.chartPx(ev);
      const hit = this.pointAt(x, y);
      if (hit === null) {
        const t = Math.round(this.tAtPx(x));
        if (t < 0 || t > this.cfg.duration_ms) return;
        this.insertPoint({ t_ms: t, value: clampToBound(this.vAtPx(y), this.yBound()), limit: 0 });
      } else {
        this.select(hit);
      }
      this.drag = { pointerId: ev.pointerId, moved: false };
      c.setPointerCapture(ev.pointerId);
      ev.preventDefault();
    });
    c.addEventListener("pointermove", (ev) => {
      if (!this.drag || ev.pointerId !== this.drag.pointerId || this.selected === null) return;
      const { x, y } = this.chartPx(ev);
      this.drag.moved = true;
      this.movePoint(this.selected, this.tAtPx(x), this.vAtPx(y));
    });
    const end = (ev) => {
      if (!this.drag || ev.pointerId !== this.drag.pointerId) return;
      if (this.drag.moved) this.hooks.onChange();
      this.drag = null;
    };
    c.addEventListener("pointerup", end);
    c.addEventListener("pointercancel", end);
    c.addEventListener("keydown", (ev) => {
      if (REMOVE_KEYS.includes(ev.key)) {
        // Always consumed: the layout's Delete removes the widget otherwise,
        // and a held key's repeats remove nothing more.
        ev.preventDefault();
        ev.stopPropagation();
        if (!ev.repeat && this.selected !== null) this.removePoint(this.selected);
      } else if (ev.key === "Escape") {
        this.select(null);
      }
    });
    // The tooltip's fields: a typed time or value moves the point as a drag
    // does (clamped to the bound and its neighbours); an empty limit is zero.
    const q = (sel) => this.el.querySelector(sel);
    const fields = {
      t: q("[data-tip-t]"),
      value: q("[data-tip-value]"),
      limit: q("[data-tip-limit]"),
      ...Object.fromEntries(EXTRA_FIELDS.map((e) => [e.key, q(`[data-tip-${e.attr}]`)])),
    };
    const commitField = (which) => {
      const i = this.selected;
      if (i === null) return;
      const text = fields[which].value.trim();
      const v = which !== "t" && which !== "value" && text === "" ? 0 : parseFloat(text);
      if (Number.isFinite(v)) {
        const pt = this.cfg.points[i];
        if (which === "t") this.movePoint(i, v, pt.value);
        else if (which === "value") this.movePoint(i, pt.t_ms, v);
        else {
          pt[which] = v;
          this.boundPoint(pt);
        }
        this.hooks.onChange();
      }
      this.refresh(true);
    };
    for (const [which, input] of Object.entries(fields)) {
      input.addEventListener("change", () => commitField(which));
      input.addEventListener("keydown", (ev) => {
        if (ev.key === "Enter") commitField(which);
        else if (ev.key === "Escape") this.refresh(true);
        ev.stopPropagation();
      });
    }
    this.tipFields = fields;
  }

  // ── geometry: chart px (css, canvas-relative) ↔ (t_ms, value) ──

  plotRect() {
    const r = this.canvas.getBoundingClientRect();
    return { x: MARGIN.l, y: MARGIN.t, w: Math.max(1, r.width - MARGIN.l - MARGIN.r), h: Math.max(1, r.height - MARGIN.t - MARGIN.b) };
  }

  chartPx(ev) {
    const r = this.canvas.getBoundingClientRect();
    return { x: ev.clientX - r.left, y: ev.clientY - r.top };
  }

  tAtPx(x) {
    const p = this.plotRect();
    return ((x - p.x) / p.w) * this.cfg.duration_ms;
  }

  vAtPx(y) {
    const p = this.plotRect();
    const b = this.yBound();
    return b - ((y - p.y) / p.h) * 2 * b;
  }

  /** Canvas-relative px of a (t_ms, value). */
  pxOf(t_ms, value) {
    const p = this.plotRect();
    const b = this.yBound();
    return [p.x + (t_ms / this.cfg.duration_ms) * p.w, p.y + ((b - value) / (2 * b)) * p.h];
  }

  /** Client px of a (t_ms, value) — the test surface for pointer gestures. */
  clientPxOf(t_ms, value) {
    const r = this.canvas.getBoundingClientRect();
    const [x, y] = this.pxOf(t_ms, value);
    return [r.left + x, r.top + y];
  }

  pointAt(x, y) {
    let best = null, bestD = HIT_PX;
    this.cfg.points.forEach((pt, i) => {
      const [px, py] = this.pxOf(pt.t_ms, pt.value);
      const d = Math.hypot(px - x, py - y);
      if (d <= bestD) { best = i; bestD = d; }
    });
    return best;
  }

  // ── model edits ──

  sequence() {
    const { mode, duration_ms, points } = this.cfg;
    return { mode, duration_ms, points };
  }

  /** A point's limits held for the mode's kind: its extras only, each within bounds. */
  boundPoint(p) {
    const k = this.kind();
    for (const key of EXTRA_KEYS) if (!k.extras.some((e) => e.key === key)) delete p[key];
    boundLimits(k, p, store.motorInfo);
    return tidy(p);
  }

  /** Points as an edit leaves them: clean, within the duration, the bound, and their limits. */
  held(points) {
    const b = this.yBound();
    return clean(points).map((p) => {
      p.t_ms = Math.min(p.t_ms, this.cfg.duration_ms);
      p.value = clampToBound(p.value, b);
      return this.boundPoint(p);
    });
  }

  select(i) {
    this.selected = i;
    this.refresh(true);
  }

  insertPoint(pt) {
    this.cfg.points.push(pt);
    this.cfg.points.sort((a, b) => a.t_ms - b.t_ms);
    this.select(this.cfg.points.indexOf(pt));
    this.hooks.onChange();
  }

  /** Move a point: time held between its neighbours', value within the bound. */
  movePoint(i, t_ms, value) {
    const pts = this.cfg.points;
    const lo = i > 0 ? pts[i - 1].t_ms : 0;
    const hi = i < pts.length - 1 ? pts[i + 1].t_ms : this.cfg.duration_ms;
    pts[i].t_ms = Math.round(Math.max(lo, Math.min(hi, t_ms)));
    pts[i].value = clampToBound(value, this.yBound());
    this.refresh(true);
  }

  removePoint(i) {
    this.cfg.points.splice(i, 1);
    this.selected = null;
    this.hooks.onChange();
    this.refresh(true);
  }

  /** A rating or a same-kind mode arrived: every point held within it, the selection kept. */
  rebound() {
    const b = this.yBound();
    for (const p of this.cfg.points) {
      p.value = clampToBound(p.value, b);
      this.boundPoint(p);
    }
    this.hooks.onChange();
    this.refresh();
  }

  /** Replace every point (a load, a test). */
  setPoints(points) {
    this.cfg.points = this.held(points);
    this.selected = null;
    this.pendingMode = null;
    this.hooks.onChange();
    this.refresh(true);
  }

  /** A mode of the same setpoint kind keeps the points, held within its
   *  bound; another kind clears them, confirmed first when there are any. */
  setMode(mode) {
    if (!isSequenceMode(mode)) return;
    const kindChanged = kindOf(mode) !== kindOf(this.cfg.mode);
    this.pendingMode = kindChanged && this.cfg.points.length ? mode : null;
    if (this.pendingMode) {
      this.refresh();
    } else {
      this.cfg.mode = mode;
      this.rebound();
    }
  }

  /** The pending kind change: Clear empties the points and takes the mode. */
  confirmMode(clear) {
    const mode = this.pendingMode;
    this.pendingMode = null;
    if (clear && mode) {
      this.cfg.points = [];
      this.selected = null;
      this.setMode(mode);
    } else {
      this.refresh();
    }
  }

  setDuration(ms) {
    if (Number.isFinite(ms)) {
      this.cfg.duration_ms = heldDuration(ms);
      for (const p of this.cfg.points) p.t_ms = Math.min(p.t_ms, this.cfg.duration_ms);
      this.hooks.onChange();
    }
    this.refresh(true);
  }

  /** A named sequence through the editor's own validation. */
  load(name) {
    const seq = loadNamed(name);
    if (!seq) return;
    if (isSequenceMode(seq.mode)) this.cfg.mode = seq.mode;
    this.setDuration(seq.duration_ms);
    this.setPoints(seq.points);
  }

  // ── run / stop [impl->app~views_026~1] ──

  runSequence() {
    const duration_ms = this.cfg.duration_ms;
    return sendWithVerdict(
      this,
      buildSequenceCommand(this.cfg),
      () => {
        // The run starts now on the board: mark the sample time current on
        // the shared timeline and start the chart marker at zero.
        this.run = { marker_ms: 0, running: true, seen: false, duration_ms, started: performance.now() };
        const tick = globalNewest();
        if (tick > 0) addRunMark(tick, this.title());
      },
      () => { this.run = null; }, // a rejected run leaves no marker
    );
  }

  stop() {
    return sendWithVerdict(this, stopCommand());
  }

  /** The marker holds: another Command accepted, or the session gone. */
  holdRun() {
    if (!this.run?.running) return;
    this.run.running = false;
    this.placeMarker();
  }

  /** The marker follows the reported elapsed time while the board reports
   *  the sequence running and holds once it does not; a run never seen
   *  running (shorter than a telemetry period) ends at its duration. */
  onTelemetry(t) {
    if (!this.run?.running || !t) return;
    if (t.sequence_running) {
      this.run.marker_ms = t.sequence_elapsed_ms;
      this.run.seen = true;
      this.placeMarker();
    } else if (this.run.seen) {
      this.holdRun();
    } else if (performance.now() - this.run.started >= this.run.duration_ms) {
      this.run.marker_ms = this.run.duration_ms;
      this.holdRun();
    }
  }

  // ── rendering ──

  renderLibrary() {
    const load = this.el.querySelector("[data-load]");
    const names = namedSequences();
    load.innerHTML = `<option value="">load…</option>${names.map((n) => `<option value="${esc(n)}">${esc(n)}</option>`).join("")}`;
    load.disabled = names.length === 0;
  }

  refreshBatch() {}

  /** `resync` rewrites a focused field too (a commit or Escape). */
  refresh(resync = false) {
    const k = this.kind();
    for (const b of this.el.querySelectorAll("[data-mode]")) {
      b.classList.toggle("is-selected", b.dataset.mode === this.cfg.mode);
    }
    const n = this.cfg.points.length;
    this.el.querySelector("[data-confirm]").hidden = !this.pendingMode;
    this.el.querySelector("[data-confirm-text]").textContent = `a new setpoint kind clears ${n} point${n === 1 ? "" : "s"}`;
    const duration = this.el.querySelector("[data-duration]");
    if (resync || document.activeElement !== duration) duration.value = String(this.cfg.duration_ms);
    const rated = Number.isFinite(k.rating(store.motorInfo));
    this.el.querySelector("[data-unit]").textContent = `${k.unit} · ±${fmt(this.yBound())}${rated ? "" : " · rating unknown"}`;
    renderSendRow(this, "[data-run]");
    this.el.querySelector("[data-hint]").hidden = n > 0;
    this.renderTip(resync);
    this.paint();
  }

  renderTip(resync) {
    const tip = this.el.querySelector(".seq-tip");
    const pt = this.selected === null ? null : this.cfg.points[this.selected];
    tip.hidden = !pt;
    if (!pt) return;
    const k = this.kind();
    this.el.querySelector("[data-tip-valueunit]").textContent = k.unit;
    this.el.querySelector("[data-tip-unit]").textContent = k.limitUnit;
    const { t, value, limit } = this.tipFields;
    limit.placeholder = k.limitZero;
    limit.setAttribute("aria-label", capitalized(k.limitLabel));
    const shown = (v) => (v ? fmt(v) : "");
    const texts = [[t, String(pt.t_ms)], [value, fmt(pt.value)], [limit, shown(pt.limit)]];
    for (const e of EXTRA_FIELDS) {
      const on = k.extras.includes(e);
      this.el.querySelector(`.seq-tip [data-extra="${e.key}"]`).hidden = !on;
      texts.push([this.tipFields[e.key], on ? shown(pt[e.key]) : ""]);
    }
    for (const [input, text] of texts) {
      if (resync || document.activeElement !== input) input.value = text;
    }
    const [x, y] = this.pxOf(pt.t_ms, pt.value);
    const r = this.canvas.getBoundingClientRect();
    const w = tip.offsetWidth || 300;
    tip.style.left = `${Math.min(Math.max(0, x + 10), Math.max(0, r.width - w))}px`;
    tip.style.top = `${Math.max(0, y - 34)}px`;
  }

  /** The kind's target in effect as drawn: recomputed only when the mode,
   *  duration, points, or ratings it follows change. */
  trajectorySeries() {
    const k = this.kind();
    const key = JSON.stringify([this.cfg.mode, this.cfg.duration_ms, this.cfg.points, store.motorInfo]);
    if (this.series?.key !== key) {
      const step_ms = Math.max(1, this.cfg.duration_ms / 2000);
      const values = k.trajectory(this.cfg.points, this.cfg.duration_ms, store.motorInfo, step_ms);
      this.series = { key, fn: k.trajectory, step_ms, values };
    }
    return this.series;
  }

  /** The run marker: the board's reported progress, held once not running. */
  placeMarker() {
    const m = this.el.querySelector(".seq-marker");
    m.hidden = !this.run;
    if (!this.run) return;
    const p = this.plotRect();
    m.style.left = `${Math.round(this.pxOf(Math.min(this.run.marker_ms, this.cfg.duration_ms), 0)[0])}px`;
    m.style.top = `${p.y}px`;
    m.style.height = `${p.h}px`;
    m.classList.toggle("seq-marker--running", this.run.running);
  }

  paint() {
    this.placeMarker();
    const c = this.canvas;
    const r = c.getBoundingClientRect();
    if (r.width < 20 || r.height < 20) return;
    const dpr = window.devicePixelRatio || 1;
    if (c.width !== Math.round(r.width * dpr) || c.height !== Math.round(r.height * dpr)) {
      c.width = Math.round(r.width * dpr);
      c.height = Math.round(r.height * dpr);
    }
    const g = c.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, r.width, r.height);
    const p = this.plotRect();
    const b = this.yBound();
    const hair = cssVar("--hair-rgb");

    // Grid: the bound, zero, and quarter lines; time at four divisions.
    g.strokeStyle = `rgba(${hair}, 0.08)`;
    g.lineWidth = 1;
    g.font = "10.5px " + cssVar("--font-mono");
    g.fillStyle = cssVar("--color-neutral-600");
    g.textAlign = "right";
    g.textBaseline = "middle";
    for (let i = 0; i <= 4; i++) {
      const v = b - (i / 4) * 2 * b;
      const y = Math.round(this.pxOf(0, v)[1]) + 0.5;
      g.beginPath(); g.moveTo(p.x, y); g.lineTo(p.x + p.w, y); g.stroke();
      if (i % 2 === 0) g.fillText(fmt(v), p.x - 6, y);
    }
    g.textAlign = "center";
    g.textBaseline = "top";
    for (let i = 0; i <= 4; i++) {
      const t = (i / 4) * this.cfg.duration_ms;
      const x = Math.round(this.pxOf(t, 0)[0]) + 0.5;
      g.beginPath(); g.moveTo(x, p.y); g.lineTo(x, p.y + p.h); g.stroke();
      g.fillText(`${Math.round(t)} ms`, Math.min(Math.max(x, p.x + 16), p.x + p.w - 20), p.y + p.h + 5);
    }

    const pts = this.cfg.points;
    if (!pts.length) return;
    // The written targets: a zero-order hold from each point to the next.
    g.strokeStyle = cssVar("--trace-1");
    g.lineWidth = 1.5;
    g.beginPath();
    pts.forEach((pt, i) => {
      const [x, y] = this.pxOf(pt.t_ms, pt.value);
      if (i === 0) g.moveTo(x, y);
      else { g.lineTo(x, this.pxOf(0, pts[i - 1].value)[1]); g.lineTo(x, y); }
    });
    g.lineTo(p.x + p.w, this.pxOf(0, pts.at(-1).value)[1]);
    g.stroke();

    // The target in effect, over the hold.
    const [ts, vs] = this.trajectorySeries().values;
    g.strokeStyle = cssVar("--color-accent-2-400");
    g.lineWidth = 1.5;
    g.setLineDash([4, 3]);
    g.beginPath();
    ts.forEach((t, i) => {
      const [x, y] = this.pxOf(t, vs[i]);
      if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
    });
    g.stroke();
    g.setLineDash([]);

    pts.forEach((pt, i) => {
      const [x, y] = this.pxOf(pt.t_ms, pt.value);
      g.beginPath();
      g.arc(x, y, i === this.selected ? 5.5 : 4, 0, 2 * Math.PI);
      g.fillStyle = cssVar("--trace-1");
      g.fill();
      if (i === this.selected) {
        g.strokeStyle = cssVar("--color-neutral-200");
        g.lineWidth = 1.5;
        g.stroke();
      }
    });
  }

  destroy() {
    closeAxesConfigFor(this);
    this.unsubs.forEach((u) => u());
    this.el.remove();
  }

  toJSON() {
    const { id, mode, duration_ms, x, y, w, h, title } = this.cfg;
    const points = this.cfg.points.map((p) => tidy({
      t_ms: p.t_ms, value: p.value, limit: p.limit, ...Object.fromEntries(EXTRA_KEYS.map((key) => [key, p[key]])),
    }));
    return { type: "sequence", id, mode, duration_ms, points, x, y, w, h, ...(title ? { title } : {}) };
  }
}
