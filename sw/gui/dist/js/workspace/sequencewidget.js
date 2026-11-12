// Sequence widget: a timed setpoint sequence under one mode, edited as
// points on a chart (time from start × setpoint, bounded by the mode's
// rating), run on the board as one Command ending in a stop at the
// duration, its progress marked from the board's reported elapsed time.
// The chart draws the written targets as a zero-order hold and the target
// in effect as the rate-limited slew the firmware applies (fw~mc_027), or
// under the position method each point's move profile (fw~mc_031).
// Named sequences live in the workspace.
// [impl->app~views_025~1] [impl->app~views_026~1] [impl->app~views_027~1]

import { icon } from "../icons.js";
import { store, subscribe, api, notify } from "../state.js";
import { toggleAxesConfig, closeAxesConfigFor } from "./axesconfig.js";
import { wireTitleEditor } from "./titlebar.js";
import {
  COMMAND_MODES, SETPOINT_KINDS, kindOf, clampToBound, stopCommand, boundPositionLimits, positionLimitFields,
} from "./commandwidget.js";
import { globalNewest, addRunMark } from "./timeline.js";
import { esc } from "../dom.js";
import { REMOVE_KEYS } from "../platform.js";

/** The modes a sequence runs under: every mode with a setpoint kind. */
export const SEQUENCE_MODES = COMMAND_MODES.filter((m) => m.kind !== null);

const MARGIN = { l: 48, r: 12, t: 10, b: 20 };
const HIT_PX = 8;
const DEFAULT_DURATION_MS = 2000;
// The vertical bound with no rating reported: fixed, so a drag cannot feed it.
const FALLBACK_BOUND = { velocity: 50, torque: 1, position: 62.83 };
const MIN_DURATION_MS = 10;
const MAX_DURATION_MS = 600_000;

const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

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

export function restoreNamed(entries) {
  named.clear();
  for (const [name, seq] of Object.entries(entries || {})) named.set(name, seq);
}

// ── the sequence model ────────────────────────────────────────────────────

/** A position's acceleration limit and velocity at the point, kept only when nonzero. */
const POSITION_LIMITS = ["accel", "at_point"];
function tidy(p) {
  for (const key of POSITION_LIMITS) if (!Number.isFinite(p[key]) || p[key] === 0) delete p[key];
  return p;
}

const clean = (points) =>
  points
    .filter((p) => Number.isFinite(p.t_ms) && Number.isFinite(p.value))
    .map((p) => tidy({
      t_ms: Math.max(0, Math.round(p.t_ms)), value: p.value, limit: Number.isFinite(p.limit) ? p.limit : 0,
      accel: p.accel, at_point: p.at_point,
    }))
    .sort((a, b) => a.t_ms - b.t_ms);

/** The target in effect over [0, duration]: each pass slews toward the
 *  last written target at its rate limit, a zero limit stepping; from zero
 *  before the first point. Returns [times_ms, values]. */
export function trajectory(points, duration_ms, step_ms = Math.max(1, duration_ms / 2000)) {
  const ts = [], vs = [];
  let effective = 0;
  let i = 0;
  let target = null;
  for (let t = 0; t <= duration_ms + 1e-9; t += step_ms) {
    // The pass slews toward the target written before it; a write at t
    // takes effect from the next pass (a step lands at once).
    if (target && target.limit > 0) {
      const maxStep = (target.limit * step_ms) / 1000;
      effective += Math.max(-maxStep, Math.min(maxStep, target.value - effective));
    }
    while (i < points.length && points[i].t_ms <= t) {
      target = points[i++];
      if (target.limit <= 0) effective = target.value;
    }
    ts.push(t);
    vs.push(effective);
  }
  return [ts, vs];
}

/** The position target under the position method over [0, duration],
 *  replayed in the board's 1 ms passes (fw~mc_031): from each point's time,
 *  from the previous move's value and velocity, velocity slews at a toward
 *  ±v (v zero: the maximum velocity; a zero: a step), braking to arrive at
 *  the point at |ω_s|, then holds ω_s; from 0 at rest before the first
 *  point. Returns [times_ms, values, velocities]. */
export function positionTrajectory(points, duration_ms, motorInfo, step_ms = Math.max(1, duration_ms / 2000)) {
  const T = 1e-3;
  const maxVelocity = motorInfo?.max_velocity_radps ?? FALLBACK_BOUND.velocity;
  const maxPosition = motorInfo?.max_position_rad ?? FALLBACK_BOUND.position;
  let theta = 0, omega = 0, sp = null, arrived = false, held = 0, i = 0, pass = 0;
  const runPass = () => {
    while (i < points.length && points[i].t_ms <= pass) {
      const p = points[i++];
      const v = p.limit > 0 ? Math.min(p.limit, maxVelocity) : maxVelocity;
      sp = {
        position: clampToBound(p.value, maxPosition), v, a: Math.max(0, p.accel ?? 0),
        ws: Math.abs(clampToBound(p.at_point ?? 0, v)),
      };
      arrived = false;
    }
    let w = 0;
    if (sp && arrived) {
      w = held;
    } else if (sp) {
      const distance = sp.position - theta;
      const dir = distance < 0 ? -1 : 1;
      const remaining = Math.abs(distance);
      if (sp.a > 0) {
        // The speed from which braking at a reaches ω_s exactly on the point.
        const aT = sp.a * T, h = aT / 2;
        const speed = Math.min(sp.v, Math.sqrt(Math.max(0, h * h + sp.ws * sp.ws - aT * sp.ws + 2 * sp.a * remaining)) - h);
        w = omega + Math.max(-aT, Math.min(aT, dir * speed - omega));
      } else {
        w = dir * sp.v;
      }
      if (w * dir >= 0 && remaining <= Math.abs(w) * T) {
        arrived = true;
        held = w = dir * sp.ws;
        if (sp.ws === 0) theta = sp.position;
      }
    }
    let next = theta + w * T;
    if (Math.abs(next) > maxPosition) {
      next = Math.sign(next) * maxPosition;
      w = 0;
    }
    theta = next;
    omega = w;
    pass += 1;
  };
  const ts = [], vs = [], ws = [];
  for (let t = 0; t <= duration_ms + 1e-9; t += step_ms) {
    while (pass < t - 1e-9) runPass();
    ts.push(t);
    vs.push(theta);
    ws.push(omega);
  }
  return [ts, vs, ws];
}

/** The Command a sequence runs as: every point in time order, then a stop
 *  at the duration, so the board ends the run itself. */
export function buildSequenceCommand(cfg) {
  const kind = kindOf(cfg.mode);
  const k = SETPOINT_KINDS[kind];
  const setpoints = clean(cfg.points).map((p) => {
    const sp = { t_us: p.t_ms * 1000, rate_limit: p.limit };
    sp[k.field] = p.value;
    if (kind === "position") Object.assign(sp, positionLimitFields(p));
    return sp;
  });
  setpoints.push({ t_us: cfg.duration_ms * 1000, stop: {} });
  return { mode: cfg.mode, setpoints };
}

const fmt = (v) => (Number.isFinite(v) ? String(+v.toFixed(3)) : "");

export class SequenceWidget {
  /** cfg: { id, mode, duration_ms, points: [{ t_ms, value, limit, accel?, at_point? }] } */
  constructor(cfg, hooks) {
    this.cfg = cfg;
    this.cfg.mode = SEQUENCE_MODES.some((m) => m.mode === cfg.mode) ? cfg.mode : "MODE_FOC_VELOCITY";
    this.cfg.duration_ms = Number.isFinite(cfg.duration_ms)
      ? Math.round(Math.max(MIN_DURATION_MS, Math.min(MAX_DURATION_MS, cfg.duration_ms)))
      : DEFAULT_DURATION_MS;
    this.cfg.points = clean(cfg.points || []);
    Object.defineProperty(this.cfg, "signals", { get: () => [], configurable: true, enumerable: false });
    this.hooks = hooks;
    this.selected = null;   // index into cfg.points
    this.drag = null;
    this.verdict = null;    // { text, rejected } | null
    this.sending = false;
    this.run = null;        // { marker_ms, running, seen } after an accepted run
    this.el = document.createElement("div");
    this.el.className = "sequence-widget widget";
    this.el.dataset.widgetId = cfg.id;
    this.render();
    this.unsubs = [
      subscribe("connection", () => this.refresh()),
      subscribe("motorInfo", () => this.rebound()),
      subscribe("sequences", () => this.renderLibrary()),
      subscribe("telemetry", (t) => this.onTelemetry(t)),
    ];
  }

  // [impl->app~views_016~1] unset title: a Sequence widget reads `Sequence`.
  title() {
    return this.cfg.title || "Sequence";
  }

  kind() {
    return SETPOINT_KINDS[kindOf(this.cfg.mode)];
  }

  /** The vertical bound: the mode's rating, else the kind's fixed fallback. */
  yBound() {
    const rated = this.kind().bound(store.motorInfo);
    return Number.isFinite(rated) ? rated : FALLBACK_BOUND[kindOf(this.cfg.mode)];
  }

  connected() {
    return store.connection.state === "connected";
  }

  render() {
    const modes = SEQUENCE_MODES.map(
      (m) => `<button data-mode="${m.mode}" class="${m.mode === this.cfg.mode ? "is-selected" : ""}">${esc(m.label)}</button>`,
    ).join("");
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
            </label>
            <label class="seq-tip-field" data-tip-poslimit hidden>accel
              <input class="mono" data-tip-accel inputmode="decimal" placeholder="step" aria-label="Acceleration limit">
              <span>rad/s²</span>
            </label>
            <label class="seq-tip-field" data-tip-poslimit hidden>at point
              <input class="mono" data-tip-atpoint inputmode="decimal" placeholder="stop" aria-label="Velocity at the point">
              <span>rad/s</span>
            </label>
          </div>
          <div class="seq-hint" data-hint>click to add a point · drag to move · Delete removes</div>
        </div>
        <div class="cmd-row cmd-send">
          <button class="btn btn-primary" data-run>Run</button>
          <button class="btn btn-secondary" data-stop>Stop</button>
          <span class="cmd-verdict" data-verdict></span>
          <span class="cmd-offline" data-offline hidden>no session</span>
        </div>
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
      if (REMOVE_KEYS.includes(ev.key) && this.selected !== null) {
        ev.preventDefault(); // the layout's Delete removes the widget otherwise
        ev.stopPropagation();
        this.removePoint(this.selected);
      } else if (ev.key === "Escape") {
        this.select(null);
      }
    });
    // The tooltip's fields: a typed time or value moves the point as a drag
    // does (clamped to the bound and its neighbours); an empty limit is zero.
    const fields = {
      t: this.el.querySelector("[data-tip-t]"),
      value: this.el.querySelector("[data-tip-value]"),
      limit: this.el.querySelector("[data-tip-limit]"),
      accel: this.el.querySelector("[data-tip-accel]"),
      at_point: this.el.querySelector("[data-tip-atpoint]"),
    };
    const commitField = (which) => {
      const i = this.selected;
      if (i === null) return;
      const text = fields[which].value.trim();
      const isLimit = which !== "t" && which !== "value";
      const v = isLimit && text === "" ? 0 : parseFloat(text);
      if (Number.isFinite(v)) {
        const pt = this.cfg.points[i];
        if (which === "t") this.movePoint(i, v, pt.value);
        else if (which === "value") this.movePoint(i, pt.t_ms, v);
        else {
          pt[which] = v;
          this.boundLimits(pt);
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

  /** A position's limits held within their bounds; other kinds carry none. */
  boundLimits(p) {
    if (kindOf(this.cfg.mode) === "position") boundPositionLimits(p, store.motorInfo);
    else for (const key of POSITION_LIMITS) delete p[key];
    tidy(p);
  }

  /** A rating arrived: hold every value within it, the selection kept. */
  rebound() {
    const b = this.yBound();
    for (const p of this.cfg.points) {
      p.value = clampToBound(p.value, b);
      this.boundLimits(p);
    }
    this.hooks.onChange();
    this.refresh();
  }

  /** Replace every point (a load, a test). */
  setPoints(points) {
    const b = this.yBound();
    this.cfg.points = clean(points).map((p) => ({ ...p, value: clampToBound(p.value, b), t_ms: Math.min(p.t_ms, this.cfg.duration_ms) }));
    for (const p of this.cfg.points) this.boundLimits(p);
    this.selected = null;
    this.hooks.onChange();
    this.refresh(true);
  }

  /** The points stay through a mode change, held within the new bound. */
  setMode(mode) {
    if (!SEQUENCE_MODES.some((m) => m.mode === mode)) return;
    this.cfg.mode = mode;
    this.rebound();
  }

  setDuration(ms) {
    if (Number.isFinite(ms)) {
      this.cfg.duration_ms = Math.round(Math.max(MIN_DURATION_MS, Math.min(MAX_DURATION_MS, ms)));
      for (const p of this.cfg.points) p.t_ms = Math.min(p.t_ms, this.cfg.duration_ms);
      this.hooks.onChange();
    }
    this.refresh(true);
  }

  load(name) {
    const seq = loadNamed(name);
    if (!seq) return;
    this.cfg.mode = SEQUENCE_MODES.some((m) => m.mode === seq.mode) ? seq.mode : this.cfg.mode;
    this.cfg.duration_ms = Number.isFinite(seq.duration_ms) ? seq.duration_ms : this.cfg.duration_ms;
    this.setPoints(seq.points || []);
  }

  // ── run / stop [impl->app~views_026~1] ──

  async send(command, onAccepted, onRejected) {
    if (this.sending || !this.connected()) return;
    this.sending = true;
    this.verdict = { text: "sending…", rejected: false };
    this.refresh();
    try {
      const v = await api.sendCommand(command);
      this.verdict = v.accepted ? { text: "accepted", rejected: false } : { text: v.cause || "rejected", rejected: true };
      if (v.accepted) onAccepted?.();
      else onRejected?.();
    } catch (e) {
      this.verdict = { text: String(e), rejected: true };
      onRejected?.();
    }
    this.sending = false;
    this.refresh();
  }

  runSequence() {
    this.send(
      buildSequenceCommand(this.cfg),
      () => {
        // The run starts now on the board: mark the sample time current on
        // the shared timeline and start the chart marker at zero.
        this.run = { marker_ms: 0, running: true, seen: false };
        const tick = globalNewest();
        if (tick > 0) addRunMark(tick, this.title());
      },
      () => { this.run = null; }, // a rejected run leaves no marker
    );
  }

  stop() {
    this.send(stopCommand());
  }

  /** The marker follows the reported elapsed time while the board reports
   *  the sequence running, and holds once it does not: a later run on the
   *  board (another widget's) never moves it again. */
  onTelemetry(t) {
    if (!this.run || !t || !this.run.running) return;
    if (t.sequence_running) {
      this.run.marker_ms = t.sequence_elapsed_ms;
      this.run.seen = true;
      this.paint();
    } else if (this.run.seen) {
      this.run.running = false;
      this.paint();
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
    const live = this.connected();
    for (const b of this.el.querySelectorAll("[data-mode]")) {
      b.classList.toggle("is-selected", b.dataset.mode === this.cfg.mode);
    }
    const duration = this.el.querySelector("[data-duration]");
    if (resync || document.activeElement !== duration) duration.value = String(this.cfg.duration_ms);
    this.el.querySelector("[data-unit]").textContent = `${k.unit} · ±${fmt(this.yBound())}`;
    this.el.querySelector("[data-run]").disabled = !live || this.sending;
    this.el.querySelector("[data-stop]").disabled = !live || this.sending;
    this.el.querySelector("[data-offline]").hidden = live;
    const verdict = this.el.querySelector("[data-verdict]");
    verdict.textContent = this.verdict?.text ?? "";
    verdict.classList.toggle("cmd-verdict--rejected", Boolean(this.verdict?.rejected));
    verdict.classList.toggle("cmd-verdict--accepted", Boolean(this.verdict) && !this.verdict.rejected && !this.sending);
    this.el.querySelector("[data-hint]").hidden = this.cfg.points.length > 0;
    this.renderTip(resync);
    this.paint();
  }

  renderTip(resync) {
    const tip = this.el.querySelector(".seq-tip");
    const pt = this.selected === null ? null : this.cfg.points[this.selected];
    tip.hidden = !pt;
    if (!pt) return;
    const k = this.kind();
    const position = kindOf(this.cfg.mode) === "position";
    this.el.querySelector("[data-tip-valueunit]").textContent = k.unit;
    this.el.querySelector("[data-tip-unit]").textContent = k.limitUnit;
    const limit = this.el.querySelector("[data-tip-limit]");
    limit.placeholder = k.limitZero;
    limit.setAttribute("aria-label", position ? "Velocity limit" : "Rate limit");
    for (const f of this.el.querySelectorAll("[data-tip-poslimit]")) f.hidden = !position;
    const shown = (v) => (v ? fmt(v) : "");
    const fields = [
      [this.el.querySelector("[data-tip-t]"), String(pt.t_ms)],
      [this.el.querySelector("[data-tip-value]"), fmt(pt.value)],
      [limit, shown(pt.limit)],
      [this.el.querySelector("[data-tip-accel]"), shown(pt.accel)],
      [this.el.querySelector("[data-tip-atpoint]"), shown(pt.at_point)],
    ];
    for (const [input, text] of fields) {
      if (resync || document.activeElement !== input) input.value = text;
    }
    const [x, y] = this.pxOf(pt.t_ms, pt.value);
    const r = this.canvas.getBoundingClientRect();
    const w = tip.offsetWidth || 300;
    tip.style.left = `${Math.min(Math.max(0, x + 10), Math.max(0, r.width - w))}px`;
    tip.style.top = `${Math.max(0, y - 34)}px`;
  }

  paint() {
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
    if (pts.length) {
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

      // The target in effect: the rate-limited slew or the move profile, over the hold.
      const [ts, vs] = kindOf(this.cfg.mode) === "position"
        ? positionTrajectory(pts, this.cfg.duration_ms, store.motorInfo)
        : trajectory(pts, this.cfg.duration_ms);
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

    // The run marker: the board's reported progress, held once not running.
    if (this.run) {
      const x = Math.round(this.pxOf(Math.min(this.run.marker_ms, this.cfg.duration_ms), 0)[0]) + 0.5;
      g.strokeStyle = this.run.running ? cssVar("--color-accent-400") : cssVar("--color-neutral-500");
      g.lineWidth = 1;
      g.beginPath(); g.moveTo(x, p.y); g.lineTo(x, p.y + p.h); g.stroke();
    }
  }

  destroy() {
    closeAxesConfigFor(this);
    this.unsubs.forEach((u) => u());
    this.el.remove();
  }

  toJSON() {
    const { id, mode, duration_ms, x, y, w, h, title } = this.cfg;
    const points = this.cfg.points.map(({ t_ms, value, limit, accel, at_point }) => tidy({ t_ms, value, limit, accel, at_point }));
    return { type: "sequence", id, mode, duration_ms, points, x, y, w, h, ...(title ? { title } : {}) };
  }
}
