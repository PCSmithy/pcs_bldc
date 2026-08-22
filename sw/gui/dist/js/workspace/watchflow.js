// The watch flow: edits to the watched set recompute the budget preview
// locally, then a debounced commit sends ONE install_watches with the full
// list. Acceptance updates the meters; a refusal quotes the firmware cause
// verbatim in the reject dialog and leaves the previous list streaming —
// nothing is cleared on rejection.
// [impl->app~obs_003~1] (UI half: the install/reject presentation)

import { api, store, set, notify, subscribe } from "../state.js";
import { icon } from "../icons.js";
import { preview, WATCH_CAPACITY } from "./budget.js";
import { releaseColor } from "./colors.js";
import { resolvedColor } from "./appearance.js";
import { historyFor, histories } from "./history.js";

const COMMIT_DEBOUNCE_MS = 600;

// path -> { size, kind } captured at add time (the filtered signal list may
// no longer contain a watched signal later).
export const meta = new Map();

let committed = ""; // JSON of the last accepted list, to skip no-op commits
let debounceTimer = null;

function entries() {
  return [...store.watched.entries()].map(([path, w]) => ({
    path,
    period_ms: w.period_ms,
    size: meta.get(path)?.size ?? 4,
  }));
}

/** Live preview → meters + "n/32 watched" chip, before any request. */
function renderPreview() {
  const p = preview(entries());
  const s = store.traceStatus;
  set({
    budgetPreview: {
      u: p.u,
      ramMax: s?.ram_budget_bytes ?? 2048,
      r: p.r,
      linkMax: s?.link_budget_bytes_per_s ?? 1_100_000,
      count: p.count,
      capacity: WATCH_CAPACITY,
    },
  });
}

export function addWatch(path, period_ms = 10) {
  if (store.gate !== "matched") return; // gated: never fire while mismatched
  if (!store.watched.has(path) && store.watched.size >= WATCH_CAPACITY) return;
  const sig = store.signals.find((s) => s.path === path);
  if (sig) meta.set(path, { size: sig.size, kind: sig.kind });
  const color = resolvedColor(path);
  store.watched.set(path, { period_ms, color });
  historyFor(path, period_ms);
  afterEdit();
}

export function setPeriod(path, period_ms) {
  const w = store.watched.get(path);
  if (!w) return;
  store.watched.set(path, { ...w, period_ms });
  historyFor(path, period_ms); // resets that signal's history to the new rate
  afterEdit();
}

export function removeWatch(path) {
  if (!store.watched.delete(path)) return;
  releaseColor(path);
  histories.delete(path);
  afterEdit();
}

function afterEdit() {
  notify("watched", store.watched);
  renderPreview();
  clearTimeout(debounceTimer);
  debounceTimer = setTimeout(commit, COMMIT_DEBOUNCE_MS);
}

/** Send the full list; the stream restarts from tick 0 on acceptance. */
export async function commit() {
  clearTimeout(debounceTimer);
  if (store.gate !== "matched") return;
  const list = entries().map(({ path, period_ms }) => ({ path, period_ms }));
  const key = JSON.stringify(list);
  if (key === committed) return;
  try {
    await api.installWatches(list);
    committed = key;
    for (const h of histories.values()) { h.ticks.length = 0; h.values.length = 0; h.gaps.length = 0; }
    notify("stream-restart");
  } catch (cause) {
    set({ budgetVerdict: String(cause) });
    showRejectDialog(String(cause));
  }
}

/** Re-commit when the gate opens with a restored list pending. */
export function initWatchflow() {
  subscribe("gate", (gate) => {
    if (gate === "matched" && store.watched.size) commit();
  });
  subscribe("trace-status", renderPreview);
  renderPreview();
}

// ── reject dialog: quote the firmware verbatim, suggest app-computed fixes ──

function computeFixes() {
  const list = entries();
  const fixes = [];
  const fastest = list.filter((e) => e.period_ms === 1);
  if (fastest.length) {
    const moved = list.map((e) => (e.period_ms === 1 ? { ...e, period_ms: 10 } : e));
    const p = preview(moved);
    fixes.push({
      label: `Move the ${fastest.length} fastest signal${fastest.length > 1 ? "s" : ""} to 10 ms → ${pctOfLink(p.r)} of link`,
      apply: () => { for (const e of fastest) setPeriod(e.path, 10); },
    });
  }
  if (list.length > 1) {
    const heaviest = [...list].sort((a, b) => b.size / b.period_ms - a.size / a.period_ms)[0];
    const p = preview(list.filter((e) => e.path !== heaviest.path));
    fixes.push({
      label: `Drop ${heaviest.path.split(".").pop()} → ${pctOfLink(p.r)}`,
      apply: () => removeWatch(heaviest.path),
    });
  }
  return fixes;
}

function pctOfLink(r) {
  const max = store.traceStatus?.link_budget_bytes_per_s ?? 1_100_000;
  return `${Math.round((r / max) * 100)} %`;
}

function showRejectDialog(cause) {
  document.querySelector(".reject-scrim")?.remove();
  const fixes = computeFixes();
  const scrim = document.createElement("div");
  scrim.className = "reject-scrim";
  scrim.innerHTML = `
    <div class="reject-dialog" role="alertdialog" aria-modal="true">
      <h2 class="reject-title display">Device refused the watch list</h2>
      <div class="reject-reason">
        <span class="reject-reason-label">reason from firmware</span>
        <span class="reject-reason-text mono"></span>
      </div>
      <p class="reject-body">The previous list is still running — nothing was
        lost. Drop a signal, or move some to a slower period.</p>
      <div class="reject-fixes"></div>
      <div class="reject-actions">
        ${fixes.length ? `<button class="btn btn-primary" data-fix="0">Apply first fix</button>` : ""}
        <button class="btn btn-secondary" data-dismiss>Edit watch list</button>
      </div>
    </div>`;
  scrim.querySelector(".reject-reason-text").textContent = cause; // verbatim
  scrim.querySelector(".reject-fixes").innerHTML = fixes
    .map((f, i) => `<div class="reject-fix">${icon("plus")}<span data-fixlabel="${i}"></span></div>`)
    .join("");
  fixes.forEach((f, i) => (scrim.querySelector(`[data-fixlabel="${i}"]`).textContent = f.label));
  scrim.addEventListener("click", (ev) => {
    if (ev.target.dataset.fix !== undefined) { fixes[+ev.target.dataset.fix].apply(); scrim.remove(); }
    else if (ev.target.dataset.dismiss !== undefined || ev.target === scrim) scrim.remove();
  });
  document.querySelector(".workspace").appendChild(scrim);
}
