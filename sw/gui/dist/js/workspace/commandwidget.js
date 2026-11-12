// Command widget: one command to the board — a mode, a setpoint in the
// mode's unit bounded by the reported rating, and its limits (a rate limit,
// zero a step; a position's velocity limit, acceleration limit and velocity
// at the point) — sent as a Command with one setpoint at time zero; Stop
// sends MODE_OFF with no setpoints, the selection kept. The verdict stands
// beside the controls until the next send; the fields, Send, and Stop follow
// the connection. Mode, setpoint, and limits persist with the workspace.
// [impl->app~views_024~1]

import { icon } from "../icons.js";
import { store, subscribe, api } from "../state.js";
import { toggleAxesConfig, closeAxesConfigFor } from "./axesconfig.js";
import { wireTitleEditor } from "./titlebar.js";
import { esc } from "../dom.js";

/** The board's modes and the setpoint kind each takes (null: none). */
export const COMMAND_MODES = [
  { mode: "MODE_OFF", label: "off", kind: null },
  { mode: "MODE_SIX_STEP_TRAP", label: "six-step", kind: "velocity" },
  { mode: "MODE_SINUSOIDAL", label: "sinusoidal", kind: "velocity" },
  { mode: "MODE_FOC_TORQUE", label: "FOC torque", kind: "torque" },
  { mode: "MODE_FOC_VELOCITY", label: "FOC velocity", kind: "velocity" },
  { mode: "MODE_FOC_POSITION", label: "FOC position", kind: "position" },
];

/** One row per setpoint kind: units, the wire field, its rating, and what
 *  its rate limit is (a position's is its velocity limit, zero the maximum). */
export const SETPOINT_KINDS = {
  velocity: { unit: "rad/s", limitUnit: "rad/s²", limitLabel: "rate limit", limitZero: "step", field: "velocity_radps", bound: (m) => m?.max_velocity_radps },
  torque: { unit: "Nm", limitUnit: "Nm/s", limitLabel: "rate limit", limitZero: "step", field: "torque_nm", bound: (m) => m?.max_torque_nm },
  position: { unit: "rad", limitUnit: "rad/s", limitLabel: "velocity limit", limitZero: "max", field: "position_rad", bound: (m) => m?.max_position_rad },
};

/** The velocity at the point's bound: the velocity limit, the maximum velocity for zero. */
export const atPointBound = (limit, motorInfo) => (limit > 0 ? limit : motorInfo?.max_velocity_radps);

/** A position's limits held in place: the velocity limit at the maximum
 *  velocity, the velocity at the point at the velocity limit. */
export function boundPositionLimits(p, motorInfo) {
  p.limit = clampToBound(p.limit, motorInfo?.max_velocity_radps);
  p.at_point = clampToBound(p.at_point ?? 0, atPointBound(p.limit, motorInfo));
}

/** A position setpoint's wire fields beside its rate limit (its velocity limit). */
export function positionLimitFields(p) {
  return { accel_limit_radps2: p.accel ?? 0, velocity_at_point_radps: p.at_point ?? 0 };
}

export const kindOf = (mode) => COMMAND_MODES.find((m) => m.mode === mode)?.kind ?? null;
const isMode = (mode) => COMMAND_MODES.some((m) => m.mode === mode);

/** |value| held within the rating; no rating known leaves it as written. */
export function clampToBound(value, bound) {
  return Number.isFinite(bound) ? Math.max(-bound, Math.min(bound, value)) : value;
}

/** The stop: MODE_OFF with no setpoints, accepted in every state. */
export const stopCommand = () => ({ mode: "MODE_OFF", setpoints: [] });

/** The Command for a widget's configuration: off sends no setpoint. */
export function buildCommand(cfg, motorInfo) {
  const kind = kindOf(cfg.mode);
  if (!kind) return { mode: cfg.mode, setpoints: [] };
  const k = SETPOINT_KINDS[kind];
  const setpoint = { t_us: 0, rate_limit: cfg.limit };
  setpoint[k.field] = clampToBound(cfg.setpoint, k.bound(motorInfo));
  if (kind === "position") Object.assign(setpoint, positionLimitFields(cfg));
  return { mode: cfg.mode, setpoints: [setpoint] };
}

const fmt = (v) => (Number.isFinite(v) ? String(+v.toFixed(4)) : "");

export class CommandWidget {
  /** cfg: { id, mode, setpoint, limit, accel, at_point } */
  constructor(cfg, hooks) {
    this.cfg = cfg;
    this.cfg.mode = isMode(cfg.mode) ? cfg.mode : "MODE_OFF";
    this.cfg.setpoint = Number.isFinite(cfg.setpoint) ? cfg.setpoint : 0;
    this.cfg.limit = Number.isFinite(cfg.limit) ? cfg.limit : 0;
    this.cfg.accel = Number.isFinite(cfg.accel) ? cfg.accel : 0;
    this.cfg.at_point = Number.isFinite(cfg.at_point) ? cfg.at_point : 0;
    // Layout and the configuration popover read a widget's signal set.
    Object.defineProperty(this.cfg, "signals", { get: () => [], configurable: true, enumerable: false });
    this.hooks = hooks;
    this.verdict = null; // { text, rejected } | null — the last send's outcome
    this.sending = false;
    this.el = document.createElement("div");
    this.el.className = "command-widget widget";
    this.el.dataset.widgetId = cfg.id;
    this.render();
    this.unsubs = [
      subscribe("connection", () => this.refresh()),
      subscribe("motorInfo", () => this.rebound()),
    ];
  }

  // [impl->app~views_016~1] unset title: a Command widget reads `Command`.
  title() {
    return this.cfg.title || "Command";
  }

  render() {
    const modes = COMMAND_MODES.map(
      (m) => `<button data-mode="${m.mode}" class="${m.mode === this.cfg.mode ? "is-selected" : ""}">${esc(m.label)}</button>`,
    ).join("");
    this.el.innerHTML = `
      <div class="widget-head" data-drag-handle>
        ${icon("grip-vertical")}
        <span class="widget-title display">${esc(this.title())}</span>
        <span class="widget-legend"></span>
        <button class="widget-menu" data-axesconfig aria-label="Widget menu">${icon("more-horizontal")}</button>
      </div>
      <div class="cmd-body">
        <div class="cmd-row">
          <span class="field-label">mode</span>
          <span class="seg cmd-modes" role="group" aria-label="Mode">${modes}</span>
        </div>
        <div class="cmd-row cmd-fields">
          <label class="cmd-field">
            <span class="field-label">setpoint</span>
            <span class="cmd-input">
              <input class="mono" data-setpoint inputmode="decimal" aria-label="Setpoint">
              <span class="cmd-unit" data-unit></span>
            </span>
            <span class="cmd-hint mono" data-bound></span>
          </label>
          <label class="cmd-field">
            <span class="field-label" data-limitlabel>rate limit</span>
            <span class="cmd-input">
              <input class="mono" data-limit inputmode="decimal" placeholder="step" aria-label="Rate limit">
              <span class="cmd-unit" data-limitunit></span>
            </span>
            <span class="cmd-hint" data-limithint>per second · empty or 0 steps</span>
          </label>
          <label class="cmd-field" data-poslimit hidden>
            <span class="field-label">acceleration limit</span>
            <span class="cmd-input">
              <input class="mono" data-accel inputmode="decimal" placeholder="step" aria-label="Acceleration limit">
              <span class="cmd-unit">rad/s²</span>
            </span>
            <span class="cmd-hint">empty or 0 steps</span>
          </label>
          <label class="cmd-field" data-poslimit hidden>
            <span class="field-label">velocity at the point</span>
            <span class="cmd-input">
              <input class="mono" data-atpoint inputmode="decimal" placeholder="stop" aria-label="Velocity at the point">
              <span class="cmd-unit">rad/s</span>
            </span>
            <span class="cmd-hint" data-atpointhint></span>
          </label>
        </div>
        <div class="cmd-row cmd-send">
          <button class="btn btn-primary" data-send>Send</button>
          <button class="btn btn-secondary" data-stop title="MODE_OFF, in one press">Stop</button>
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
    this.wireControls();
    this.refresh();
  }

  wireControls() {
    this.el.querySelector(".cmd-modes").addEventListener("click", (ev) => {
      const b = ev.target.closest("[data-mode]");
      if (b) this.setMode(b.dataset.mode);
    });
    const setpoint = this.el.querySelector("[data-setpoint]");
    const limits = {
      limit: this.el.querySelector("[data-limit]"),
      accel: this.el.querySelector("[data-accel]"),
      at_point: this.el.querySelector("[data-atpoint]"),
    };
    const commit = (input) => {
      if (input === setpoint) this.setSetpoint(parseFloat(input.value));
      else {
        const which = Object.keys(limits).find((key) => limits[key] === input);
        this.setLimit(which, input.value.trim() === "" ? 0 : parseFloat(input.value));
      }
    };
    for (const input of [setpoint, ...Object.values(limits)]) {
      input.addEventListener("change", () => commit(input));
      input.addEventListener("keydown", (ev) => {
        if (ev.key === "Enter") commit(input);
        else if (ev.key === "Escape") this.refresh(true);
      });
    }
    this.el.querySelector("[data-send]").addEventListener("click", () => this.send());
    this.el.querySelector("[data-stop]").addEventListener("click", () => this.send(stopCommand()));
  }

  /** A new setpoint kind starts from zero: a number in rad/s is not one in Nm. */
  setMode(mode) {
    if (!isMode(mode)) return;
    const kindChanged = kindOf(mode) !== kindOf(this.cfg.mode);
    this.cfg.mode = mode;
    if (kindChanged) this.cfg.limit = this.cfg.accel = this.cfg.at_point = 0;
    this.setSetpoint(kindChanged ? 0 : this.cfg.setpoint);
  }

  /** A rating arrived: the setpoint and a position's limits held within it. */
  rebound() {
    if (kindOf(this.cfg.mode) === "position") boundPositionLimits(this.cfg, store.motorInfo);
    this.setSetpoint(this.cfg.setpoint);
  }

  /** Setpoint held within the mode's rating; a non-number leaves it. */
  setSetpoint(value) {
    if (Number.isFinite(value)) {
      const kind = kindOf(this.cfg.mode);
      const bound = kind ? SETPOINT_KINDS[kind].bound(store.motorInfo) : undefined;
      this.cfg.setpoint = clampToBound(value, bound);
      this.hooks.onChange();
    }
    this.refresh(true);
  }

  /** which: limit, accel, or at_point; a position's held within their bounds. */
  setLimit(which, value) {
    if (Number.isFinite(value)) {
      this.cfg[which] = value;
      if (kindOf(this.cfg.mode) === "position") boundPositionLimits(this.cfg, store.motorInfo);
      this.hooks.onChange();
    }
    this.refresh(true);
  }

  connected() {
    return store.connection.state === "connected";
  }

  /** Send: the configured Command, or the one given (Stop). */
  async send(command = buildCommand(this.cfg, store.motorInfo)) {
    if (this.sending || !this.connected()) return;
    this.sending = true;
    this.verdict = { text: "sending…", rejected: false };
    this.refresh();
    try {
      const v = await api.sendCommand(command);
      this.verdict = v.accepted ? { text: "accepted", rejected: false } : { text: v.cause || "rejected", rejected: true };
    } catch (e) {
      this.verdict = { text: String(e), rejected: true };
    }
    this.sending = false;
    this.refresh();
  }

  /** Nothing here is sample-driven: the batch refresh is a no-op. */
  refreshBatch() {}

  /** `resync` rewrites a focused field too: a commit or Escape, never a
   *  store change under someone's typing. */
  refresh(resync = false) {
    const kind = kindOf(this.cfg.mode);
    const k = kind ? SETPOINT_KINDS[kind] : null;
    const bound = k?.bound(store.motorInfo);
    const live = this.connected();
    for (const b of this.el.querySelectorAll("[data-mode]")) {
      b.classList.toggle("is-selected", b.dataset.mode === this.cfg.mode);
      b.disabled = !live;
    }
    const setpoint = this.el.querySelector("[data-setpoint]");
    const limit = this.el.querySelector("[data-limit]");
    const accel = this.el.querySelector("[data-accel]");
    const atPoint = this.el.querySelector("[data-atpoint]");
    const position = kind === "position";
    const shown = (v) => (k && v !== 0 ? fmt(v) : "");
    if (resync || document.activeElement !== setpoint) setpoint.value = k ? fmt(this.cfg.setpoint) : "";
    if (resync || document.activeElement !== limit) limit.value = shown(this.cfg.limit);
    if (resync || document.activeElement !== accel) accel.value = position ? shown(this.cfg.accel) : "";
    if (resync || document.activeElement !== atPoint) atPoint.value = position ? shown(this.cfg.at_point) : "";
    setpoint.disabled = limit.disabled = !live || !k;
    accel.disabled = atPoint.disabled = !live || !position;
    for (const f of this.el.querySelectorAll("[data-poslimit]")) f.hidden = !position;
    limit.placeholder = k?.limitZero ?? "step";
    limit.setAttribute("aria-label", k?.limitLabel === "velocity limit" ? "Velocity limit" : "Rate limit");
    this.el.querySelector("[data-limitlabel]").textContent = k?.limitLabel ?? "rate limit";
    const maxVelocity = store.motorInfo?.max_velocity_radps;
    const atMost = (b) => (Number.isFinite(b) ? `≤ ${fmt(b)} rad/s · ` : "");
    this.el.querySelector("[data-limithint]").textContent = position
      ? `${atMost(maxVelocity)}empty or 0 is the maximum`
      : "per second · empty or 0 steps";
    this.el.querySelector("[data-atpointhint]").textContent = `${atMost(atPointBound(this.cfg.limit, store.motorInfo))}empty or 0 stops`;
    this.el.querySelector("[data-unit]").textContent = k?.unit ?? "—";
    this.el.querySelector("[data-limitunit]").textContent = k?.limitUnit ?? "—";
    this.el.querySelector("[data-bound]").textContent = !k
      ? "no setpoint"
      : Number.isFinite(bound)
        ? `≤ ${fmt(bound)} ${k.unit}`
        : "rating unknown";
    this.el.querySelector("[data-send]").disabled = !live || this.sending;
    this.el.querySelector("[data-stop]").disabled = !live || this.sending;
    this.el.querySelector("[data-offline]").hidden = live;
    const verdict = this.el.querySelector("[data-verdict]");
    verdict.textContent = this.verdict?.text ?? "";
    verdict.classList.toggle("cmd-verdict--rejected", Boolean(this.verdict?.rejected));
    verdict.classList.toggle("cmd-verdict--accepted", Boolean(this.verdict) && !this.verdict.rejected && !this.sending);
  }

  destroy() {
    closeAxesConfigFor(this);
    this.unsubs.forEach((u) => u());
    this.el.remove();
  }

  toJSON() {
    const { id, mode, setpoint, limit, accel, at_point, x, y, w, h, title } = this.cfg;
    return { type: "command", id, mode, setpoint, limit, accel, at_point, x, y, w, h, ...(title ? { title } : {}) };
  }
}
