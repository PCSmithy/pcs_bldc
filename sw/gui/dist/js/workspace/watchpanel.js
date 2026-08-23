// Watch panel: the picker-docked master list of watched signals — trace
// color, name, period seg, latest value, remove. Period edits and removals
// flow through the normal watchflow commit path (debounced recommit, budget
// preview, reject dialog untouched).
// [impl->app~views_010~1]

import { icon } from "../icons.js";
import { store, subscribe } from "../state.js";
import { histories } from "./history.js";
import { meta, setPeriod, removeWatch } from "./watchflow.js";
import { WATCH_CAPACITY } from "./budget.js";
import { forEachWidget, persist } from "./layout.js";
import { formatValue } from "./plotwidget.js";
import { throttleTrailing } from "../perf.js";

const PERIODS = [1, 10, 100];
const esc = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

let host = null;

/** Panel removal is MASTER removal: the signal leaves the watch list AND
 *  every widget holding it — a signal with no stream is a dead row. (The
 *  axes-config deselect is the per-widget path: it unwatches only when no
 *  other widget still holds the signal.) */
function removeEverywhere(path) {
  forEachWidget((w) => w.removeSignal?.(path));
  removeWatch(path);
  persist();
}

function renderRows() {
  const rows = [...store.watched.entries()]
    .map(([path, w]) => {
      const m = meta.get(path);
      return `
      <div class="watch-row" data-path="${esc(path)}" title="${esc(path)}">
        <span class="legend-bar" style="background:${w.color}"></span>
        <span class="watch-name mono">${esc(path)}</span>
        <span class="watch-seg" role="group" aria-label="Sample period">
          ${PERIODS.map((p) => `<button class="watch-seg-opt ${p === w.period_ms ? "watch-seg-opt--on" : ""}"
              data-period="${p}">${p}</button>`).join("")}
        </span>
        <span class="watch-value mono" data-value>${esc(formatValue(histories.get(path)?.latest() ?? null, m?.kind, m?.enums))}</span>
        <button class="watch-remove" aria-label="Remove ${esc(path)} from the watch list">×</button>
      </div>`;
    })
    .join("");
  host.innerHTML = store.watched.size
    ? `<div class="watch-panel-head">
         <span class="field-label">watched · ${store.watched.size}/${WATCH_CAPACITY}</span>
       </div>
       <div class="watch-rows">${rows}</div>`
    : "";
}

/** Value cells refresh in place — throttled to 10 Hz with a trailing edge
 *  (batches arrive at ~20 Hz; the eye can't use more, the DOM writes cost).
 *  Rows only re-render on membership/period changes. */
function refreshValues() {
  if (!host || !store.watched.size) return;
  for (const row of host.querySelectorAll(".watch-row")) {
    const path = row.dataset.path;
    const cell = row.querySelector("[data-value]");
    if (cell) cell.textContent = formatValue(histories.get(path)?.latest() ?? null, meta.get(path)?.kind, meta.get(path)?.enums);
  }
}

const throttledRefreshValues = throttleTrailing(refreshValues);

export function initWatchPanel() {
  host = document.querySelector(".watch-panel");
  if (!host) return;

  host.addEventListener("click", (ev) => {
    const row = ev.target.closest(".watch-row");
    if (!row) return;
    const path = row.dataset.path;
    if (ev.target.closest(".watch-remove")) removeEverywhere(path);
    else if (ev.target.dataset.period) setPeriod(path, +ev.target.dataset.period);
  });

  subscribe("watched", renderRows);
  subscribe("samples", throttledRefreshValues);
  subscribe("stream-restart", refreshValues);
  renderRows();
}
