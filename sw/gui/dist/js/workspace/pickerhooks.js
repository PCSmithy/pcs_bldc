// Picker interactivity the shell left to the workspace: clicking a row
// toggles it into the watch list (default 10 ms); the active row grows the
// 1/10/100 period segmented control; the budget meters (SOLE owner here)
// render the live preview, falling back to the device's trace status.

import { store, subscribe } from "../state.js";
import { addWatch, setPeriod, removeWatch } from "./watchflow.js";
import { WATCH_CAPACITY } from "./budget.js";
import { esc } from "../dom.js";

const HOT_FRACTION = 0.85; // sage while comfortable, accent near the limit
let activePath = null;

export function initPickerWatchControls() {
  const tree = document.querySelector(".picker-tree");

  tree.addEventListener("click", (ev) => {
    const row = ev.target.closest(".signal-row");
    if (!row) return;
    const path = row.dataset.path;

    const seg = ev.target.closest("[data-period]");
    if (seg) {
      setPeriod(path, +seg.dataset.period);
      return;
    }
    if (ev.target.closest("[data-unwatch]")) {
      removeWatch(path);
      activePath = null;
      return;
    }
    if (!store.watched.has(path)) {
      addWatch(path);
      activePath = path;
    } else {
      activePath = activePath === path ? null : path;
      decorate();
    }
  });

  // The shell re-renders rows on watched/signals changes; decorate after it.
  for (const topic of ["watched", "signals", "gate"]) subscribe(topic, () => queueMicrotask(decorate));
  for (const topic of ["budgetPreview", "traceStatus", "budgetVerdict"]) {
    subscribe(topic, renderPreviewMeters);
  }
  renderPreviewMeters();

  function decorate() {
    for (const row of tree.querySelectorAll(".signal-row")) {
      const path = row.dataset.path;
      const active = path === activePath && store.watched.has(path);
      row.classList.toggle("signal-row--active", active);
      const existing = row.querySelector(".period-seg");
      if (!active) { existing?.remove(); continue; }
      if (existing) continue;
      const w = store.watched.get(path);
      const seg = document.createElement("span");
      seg.className = "seg period-seg";
      seg.innerHTML =
        [1, 10, 100]
          .map((p) => `<button data-period="${p}" class="${w.period_ms === p ? "is-selected" : ""}">${p}</button>`)
          .join("") + `<button data-unwatch title="Stop watching">×</button>`;
      row.querySelector(".period-pill")?.replaceWith(seg);
    }
  }

  function renderPreviewMeters() {
    const host = document.querySelector(".budget-meters");
    if (!host) return;
    const s = store.traceStatus;
    const p =
      store.budgetPreview ??
      (s && {
        u: s.ram_worst_tick_bytes,
        ramMax: s.ram_budget_bytes,
        r: s.link_rate_bytes_per_s,
        linkMax: s.link_budget_bytes_per_s,
        count: store.watched.size,
        capacity: WATCH_CAPACITY,
      });
    const meter = (label, text, frac) => `
      <div class="budget-meter">
        <div class="budget-meter-row">
          <span class="budget-meter-label">${label}</span>
          <span class="budget-meter-value">${text}</span>
        </div>
        <div class="budget-track">
          <div class="budget-fill ${frac >= HOT_FRACTION ? "budget-fill--hot" : ""}"
               style="width:${Math.min(100, frac * 100).toFixed(1)}%"></div>
        </div>
      </div>`;
    const verdict = store.budgetVerdict;
    host.innerHTML = `
      <div class="budget-head">
        <span class="field-label">device budget</span>
        ${p ? `<span class="budget-count mono">${p.count}/${p.capacity} watched</span>` : ""}
        <span class="budget-status ${verdict !== "accepted" && verdict !== "—" ? "budget-status--rejected" : ""}">${esc(verdict)}</span>
      </div>
      ${meter("watch RAM", p ? `${p.u} / ${p.ramMax} B` : "— / —", p ? p.u / p.ramMax : 0)}
      ${meter("link bandwidth", p ? `${Math.round((p.r / p.linkMax) * 100)} / 100 %` : "— / —", p ? p.r / p.linkMax : 0)}`;
  }
}
