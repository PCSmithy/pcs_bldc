// Table widget: one row per signal — name, value, type, period. With no
// cursor time held the value is the latest streamed sample, live; with a
// cursor held every row reads at that time, absence stated.
// [impl->app~views_006~1]

import { icon } from "../icons.js";
import { store, subscribe } from "../state.js";
import { histories } from "./history.js";
import { meta } from "./watchflow.js";
import { traceColor } from "./colors.js";
import { cursor } from "./cursor.js";
import { formatValue } from "./plotwidget.js";
import { toggleAxesConfig, closeAxesConfigFor } from "./axesconfig.js";

const esc = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

export class TableWidget {
  /** cfg: { id, signals: [path] } */
  constructor(cfg, hooks) {
    this.cfg = cfg;
    this.hooks = hooks;
    this.el = document.createElement("div");
    this.el.className = "table-widget widget";
    this.el.dataset.widgetId = cfg.id;
    this.render();
    this.unsubs = [subscribe("cursor", () => this.refresh())];
  }

  render() {
    this.el.innerHTML = `
      <div class="widget-head" data-drag-handle>
        ${icon("grip-vertical")}
        <span class="widget-title display">Live values</span>
        <span class="widget-tag table-mode-tag"></span>
        <span class="widget-legend"></span>
        <button class="widget-menu" data-axesconfig aria-label="Widget menu">${icon("more-horizontal")}</button>
      </div>
      <div class="table-body">
        <table class="value-table">
          <thead><tr><th>signal</th><th class="col-value">value</th><th>type</th><th class="col-period">period</th></tr></thead>
          <tbody></tbody>
        </table>
        <div class="drop-target">drop a signal to add a row</div>
      </div>
      <div class="resize-handle" data-resize aria-hidden="true"></div>`;
    // The ⋯ opens the shared configuration popover — signal rows + Remove
    // widget; a table has no axes, so no assignment or scale surface.
    const menu = this.el.querySelector("[data-axesconfig]");
    menu.addEventListener("click", (ev) => {
      ev.stopPropagation();
      toggleAxesConfig(this, menu);
    });
    this.refresh();
  }

  addSignal(path) {
    if (this.cfg.signals.includes(path)) return;
    this.cfg.signals.push(path);
    this.refresh();
    this.hooks.onChange();
  }

  /** Deselect: the row leaves the table; the widget remains. */
  removeSignal(path) {
    const i = this.cfg.signals.indexOf(path);
    if (i < 0) return;
    this.cfg.signals.splice(i, 1);
    this.refresh();
    this.hooks.onChange();
  }

  rowValue(path) {
    const h = histories.get(path);
    if (!h) return null;
    if (cursor.tick === null) return h.latest();
    const t = h.tickAtOrBefore(cursor.tick);
    if (t === null || cursor.tick - t >= h.period) return null; // in a tick-count gap
    return h.valueAt(t);
  }

  refresh() {
    const atCursor = cursor.tick !== null;
    this.el.querySelector(".table-mode-tag").textContent = atCursor
      ? `at cursor · ${cursor.tick.toLocaleString("en-US").replace(/,/g, " ")} ms`
      : "latest";
    this.el.querySelector("tbody").innerHTML = this.cfg.signals
      .map((path) => {
        const m = meta.get(path);
        const w = store.watched.get(path);
        const v = this.rowValue(path);
        const absent = v === null;
        return `<tr>
          <td><span class="legend-bar" style="background:${absent ? "var(--ink-hint)" : w?.color || traceColor(path)}"></span><span class="mono">${esc(path)}</span></td>
          <td class="col-value mono ${absent ? "readout-value--absent" : ""} ${m?.kind === "enum" ? "value--enum" : ""} ${m?.kind === "bool" && v === 0 ? "value--ok" : ""}">${formatValue(v, m?.kind)}</td>
          <td class="col-type">${esc(m?.kind ?? "—")}</td>
          <td class="col-period mono">${w ? `${w.period_ms} ms` : "—"}</td>
        </tr>`;
      })
      .join("");
  }

  destroy() {
    closeAxesConfigFor(this);
    this.unsubs.forEach((u) => u());
    this.el.remove();
  }

  toJSON() {
    const { id, signals, w, hpx } = this.cfg;
    return { type: "table", id, signals, w, hpx };
  }
}
