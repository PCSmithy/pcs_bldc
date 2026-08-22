// Picker interactivity the shell left to the workspace: clicking a row
// toggles it into the watch list (default 10 ms); the active row grows the
// 1/10/100 period segmented control; the budget meters render the LIVE
// preview (before any request) with the sage→accent crossover, plus the
// "n/32 watched" count.

import { store, subscribe } from "../state.js";
import { addWatch, setPeriod, removeWatch } from "./watchflow.js";

const HOT_FRACTION = 0.8;
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
  subscribe("budgetPreview", renderPreviewMeters);

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
      seg.className = "period-seg";
      seg.innerHTML =
        [1, 10, 100]
          .map((p) => `<button data-period="${p}" class="${w.period_ms === p ? "is-selected" : ""}">${p}</button>`)
          .join("") + `<button data-unwatch title="Stop watching">×</button>`;
      row.querySelector(".period-pill")?.replaceWith(seg);
    }
  }

  function renderPreviewMeters(p) {
    const host = document.querySelector(".budget-meters");
    if (!host || !p) return;
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
        <span class="budget-count mono">${p.count}/${p.capacity} watched</span>
        <span class="budget-status ${verdict !== "accepted" && verdict !== "—" ? "budget-status--rejected" : ""}">${verdict}</span>
      </div>
      ${meter("watch RAM", `${p.u} / ${p.ramMax} B`, p.u / p.ramMax)}
      ${meter("link bandwidth", `${Math.round((p.r / p.linkMax) * 100)} / 100 %`, p.r / p.linkMax)}`;
  }
}
