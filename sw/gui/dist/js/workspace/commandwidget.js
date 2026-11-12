// Command widget: one command to the board — a mode, a setpoint in the
// mode's unit bounded by the reported rating, and its kind's limits — sent
// as a Command with one setpoint at time zero; Stop sends MODE_OFF with no
// setpoints, the selection kept. A mode of another setpoint kind starts
// from zero. The verdict stands beside the controls until the next send;
// the fields, Send, and Stop follow the connection. Mode, setpoint, and
// limits persist with the workspace.
// [impl->app~views_024~1]

import { icon } from "../icons.js";
import { store, subscribe } from "../state.js";
import { toggleAxesConfig, closeAxesConfigFor } from "./axesconfig.js";
import { wireTitleEditor } from "./titlebar.js";
import { esc } from "../dom.js";
import { COMMAND_MODES, EXTRA_FIELDS, kindFor, clampToBound, stopCommand, boundLimits } from "./setpoints.js";
import { fmt, isLive, sendRowHtml, sendWithVerdict, renderSendRow } from "./cmdplane.js";

const isMode = (mode) => COMMAND_MODES.some((m) => m.mode === mode);
const LIMIT_KEYS = ["limit", ...EXTRA_FIELDS.map((e) => e.key)];
const capitalized = (s) => s[0].toUpperCase() + s.slice(1);

/** The Command for a widget's configuration: off sends no setpoint. */
export function buildCommand(cfg, info) {
  const k = kindFor(cfg.mode);
  if (!k) return { mode: cfg.mode, setpoints: [] };
  const p = { ...cfg, value: clampToBound(cfg.setpoint, k.rating(info)) };
  return { mode: cfg.mode, setpoints: [{ t_us: 0, ...k.wireFields(p) }] };
}

export class CommandWidget {
  /** cfg: { id, mode, setpoint, limit, and each extra limit field } */
  constructor(cfg, hooks) {
    this.cfg = cfg;
    this.cfg.mode = isMode(cfg.mode) ? cfg.mode : "MODE_OFF";
    for (const key of ["setpoint", ...LIMIT_KEYS]) this.cfg[key] = Number.isFinite(cfg[key]) ? cfg[key] : 0;
    // Layout and the configuration popover read a widget's signal set.
    Object.defineProperty(this.cfg, "signals", { get: () => [], configurable: true, enumerable: false });
    this.hooks = hooks;
    this.verdict = null; // the latest send's outcome (cmdplane.js)
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
    const extras = EXTRA_FIELDS.map((e) => `
          <label class="cmd-field" data-extra="${e.key}" hidden>
            <span class="field-label">${esc(e.label)}</span>
            <span class="cmd-input">
              <input class="mono" data-${e.attr} inputmode="decimal" placeholder="${e.zero}" aria-label="${e.aria}">
              <span class="cmd-unit">${e.unit}</span>
            </span>
            <span class="cmd-hint" data-hint="${e.key}"></span>
          </label>`).join("");
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
            <span class="cmd-hint" data-limithint></span>
          </label>${extras}
        </div>
        ${sendRowHtml("send", "Send")}
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
    const q = (sel) => this.el.querySelector(sel);
    this.inputs = {
      setpoint: q("[data-setpoint]"),
      limit: q("[data-limit]"),
      ...Object.fromEntries(EXTRA_FIELDS.map((e) => [e.key, q(`[data-${e.attr}]`)])),
    };
    for (const [key, input] of Object.entries(this.inputs)) {
      const commit = () => {
        if (key === "setpoint") this.setSetpoint(parseFloat(input.value));
        else this.setLimit(key, input.value.trim() === "" ? 0 : parseFloat(input.value));
      };
      input.addEventListener("change", commit);
      input.addEventListener("keydown", (ev) => {
        if (ev.key === "Enter") commit();
        else if (ev.key === "Escape") this.refresh(true);
      });
    }
    q("[data-send]").addEventListener("click", () => this.send());
    q("[data-stop]").addEventListener("click", () => this.send(stopCommand()));
  }

  /** A new setpoint kind starts from zero: a number in rad/s is not one in Nm. */
  setMode(mode) {
    if (!isMode(mode)) return;
    if (kindFor(mode) !== kindFor(this.cfg.mode)) {
      this.cfg.setpoint = 0;
      for (const key of LIMIT_KEYS) this.cfg[key] = 0;
    }
    this.cfg.mode = mode;
    this.rebound();
  }

  /** The setpoint and limits held within the mode's rating. */
  rebound() {
    const k = kindFor(this.cfg.mode);
    if (k) boundLimits(k, this.cfg, store.motorInfo);
    this.setSetpoint(this.cfg.setpoint);
  }

  /** Setpoint held within the mode's rating; a non-number leaves it. */
  setSetpoint(value) {
    if (Number.isFinite(value)) {
      this.cfg.setpoint = clampToBound(value, kindFor(this.cfg.mode)?.rating(store.motorInfo));
      this.hooks.onChange();
    }
    this.refresh(true);
  }

  /** which: the limit or an extra limit field, held within its bounds. */
  setLimit(which, value) {
    const k = kindFor(this.cfg.mode);
    if (Number.isFinite(value) && k) {
      this.cfg[which] = value;
      boundLimits(k, this.cfg, store.motorInfo);
      this.hooks.onChange();
    }
    this.refresh(true);
  }

  /** Send: the configured Command, or the one given (Stop). */
  send(command = buildCommand(this.cfg, store.motorInfo)) {
    return sendWithVerdict(this, command);
  }

  /** Nothing here is sample-driven: the batch refresh is a no-op. */
  refreshBatch() {}

  /** `resync` rewrites a focused field too: a commit or Escape, never a
   *  store change under someone's typing. */
  refresh(resync = false) {
    const k = kindFor(this.cfg.mode);
    const info = store.motorInfo;
    const live = isLive();
    for (const b of this.el.querySelectorAll("[data-mode]")) {
      b.classList.toggle("is-selected", b.dataset.mode === this.cfg.mode);
      b.disabled = !live;
    }
    const show = (input, text) => {
      if (resync || document.activeElement !== input) input.value = text;
    };
    const shown = (v) => (v ? fmt(v) : "");
    const { setpoint, limit } = this.inputs;
    show(setpoint, k ? fmt(this.cfg.setpoint) : "");
    show(limit, k ? shown(this.cfg.limit) : "");
    setpoint.disabled = limit.disabled = !live || !k;
    for (const e of EXTRA_FIELDS) {
      const on = Boolean(k?.extras.includes(e));
      this.el.querySelector(`[data-extra="${e.key}"]`).hidden = !on;
      this.inputs[e.key].disabled = !live || !on;
      show(this.inputs[e.key], on ? shown(this.cfg[e.key]) : "");
      this.el.querySelector(`[data-hint="${e.key}"]`).textContent = on ? e.hint(this.cfg, info) : "";
    }
    const limitLabel = k?.limitLabel ?? "rate limit";
    limit.placeholder = k?.limitZero ?? "step";
    limit.setAttribute("aria-label", capitalized(limitLabel));
    this.el.querySelector("[data-limitlabel]").textContent = limitLabel;
    this.el.querySelector("[data-limithint]").textContent = k ? k.limitHint(info) : "";
    this.el.querySelector("[data-unit]").textContent = k?.unit ?? "—";
    this.el.querySelector("[data-limitunit]").textContent = k?.limitUnit ?? "—";
    const bound = k?.rating(info);
    this.el.querySelector("[data-bound]").textContent = !k
      ? "no setpoint"
      : Number.isFinite(bound) ? `≤ ${fmt(bound)} ${k.unit}` : "rating unknown";
    renderSendRow(this, "[data-send]");
  }

  destroy() {
    closeAxesConfigFor(this);
    this.unsubs.forEach((u) => u());
    this.el.remove();
  }

  toJSON() {
    const { id, mode, setpoint, x, y, w, h, title } = this.cfg;
    const limits = Object.fromEntries(LIMIT_KEYS.map((key) => [key, this.cfg[key]]));
    return { type: "command", id, mode, setpoint, ...limits, x, y, w, h, ...(title ? { title } : {}) };
  }
}
