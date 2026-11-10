// Command widget: one command to the board — a mode, a setpoint in the
// mode's unit bounded by the reported rating, and a rate limit (zero: a
// step) — sent as a Command with one setpoint at time zero; Stop sends
// MODE_OFF with no setpoints, the selection kept. The verdict stands beside
// the controls until the next send; the fields, Send, and Stop follow the
// connection. Mode, setpoint, and limit persist with the workspace.
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
];

/** One row per setpoint kind: units, the wire field, and its rating. */
export const SETPOINT_KINDS = {
  velocity: { unit: "rad/s", limitUnit: "rad/s²", field: "velocity_radps", bound: (m) => m?.max_velocity_radps },
  torque: { unit: "Nm", limitUnit: "Nm/s", field: "torque_nm", bound: (m) => m?.max_torque_nm },
};

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
  return { mode: cfg.mode, setpoints: [setpoint] };
}

const fmt = (v) => (Number.isFinite(v) ? String(+v.toFixed(4)) : "");

export class CommandWidget {
  /** cfg: { id, mode, setpoint, limit } */
  constructor(cfg, hooks) {
    this.cfg = cfg;
    this.cfg.mode = isMode(cfg.mode) ? cfg.mode : "MODE_OFF";
    this.cfg.setpoint = Number.isFinite(cfg.setpoint) ? cfg.setpoint : 0;
    this.cfg.limit = Number.isFinite(cfg.limit) ? cfg.limit : 0;
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
      subscribe("motorInfo", () => this.setSetpoint(this.cfg.setpoint)), // re-bounded
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
            <span class="field-label">rate limit</span>
            <span class="cmd-input">
              <input class="mono" data-limit inputmode="decimal" placeholder="step" aria-label="Rate limit">
              <span class="cmd-unit" data-limitunit></span>
            </span>
            <span class="cmd-hint">per second · empty or 0 steps</span>
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
    const limit = this.el.querySelector("[data-limit]");
    const commit = (input) => {
      if (input === setpoint) this.setSetpoint(parseFloat(input.value));
      else this.setLimit(input.value.trim() === "" ? 0 : parseFloat(input.value));
    };
    for (const input of [setpoint, limit]) {
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
    if (kindChanged) this.cfg.limit = 0;
    this.setSetpoint(kindChanged ? 0 : this.cfg.setpoint);
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

  setLimit(value) {
    if (Number.isFinite(value)) {
      this.cfg.limit = value;
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
    if (resync || document.activeElement !== setpoint) setpoint.value = k ? fmt(this.cfg.setpoint) : "";
    if (resync || document.activeElement !== limit) limit.value = k && this.cfg.limit !== 0 ? fmt(this.cfg.limit) : "";
    setpoint.disabled = limit.disabled = !live || !k;
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
    const { id, mode, setpoint, limit, x, y, w, h, title } = this.cfg;
    return { type: "command", id, mode, setpoint, limit, x, y, w, h, ...(title ? { title } : {}) };
  }
}
