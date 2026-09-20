// The watch flow: edits to the watched set recompute the budget preview
// locally, then a debounced commit sends ONE install_watches with the full
// list. Acceptance updates the meters; a refusal quotes the firmware cause
// verbatim in the reject dialog, rolls the watched set back to the list the
// device is still streaming, and offers that refused list's fixes.
// [impl->app~obs_003~1] (UI half: the install/reject presentation)

import { api, store, set, notify, subscribe } from "../state.js";
import { icon } from "../icons.js";
import { preview, WATCH_CAPACITY } from "./budget.js";
import { releaseColor } from "./colors.js";
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
    period_cycles: w.period_cycles,
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
      ramMax: s?.ram_budget_bytes_per_ms ?? 2048,
      r: p.r,
      linkMax: s?.link_budget_bytes_per_s ?? 480_000,
      count: p.count,
      capacity: WATCH_CAPACITY,
    },
  });
}

export function addWatch(path, period_cycles = 200) {
  if (store.gate !== "matched") return; // gated: never fire while mismatched
  if (!store.watched.has(path) && store.watched.size >= WATCH_CAPACITY) return;
  const sig = store.signals.find((s) => s.path === path);
  if (sig) meta.set(path, { size: sig.size, kind: sig.kind, enums: sig.enums });
  // An already-watched signal keeps its entry untouched (the drop handler
  // re-adds before joining a widget — a join must not clobber the period).
  if (store.watched.has(path)) return;
  store.watched.set(path, { period_cycles });
  historyFor(path, period_cycles);
  afterEdit();
}

export function setPeriod(path, period_cycles) {
  const w = store.watched.get(path);
  if (!w) return;
  store.watched.set(path, { ...w, period_cycles });
  historyFor(path, period_cycles); // resets that signal's history to the new rate
  afterEdit();
}

export function removeWatch(path) {
  if (!store.watched.delete(path)) return;
  releaseColor(path);
  histories.delete(path);
  afterEdit();
}

/** Make `list` ([{path, period_cycles}]) the watched set, rebuilding each
 *  history at its period and dropping what the list omits. Colors survive:
 *  a dropped path may still sit in a widget, and a released slot would be
 *  handed to another signal. `andCommit` sends the new list; a rollback
 *  (the device already runs `list`) only restores the app's state. */
function applyList(list, andCommit) {
  const keep = new Set(list.map((e) => e.path));
  for (const path of [...store.watched.keys()]) {
    if (keep.has(path)) continue;
    store.watched.delete(path);
    histories.delete(path);
  }
  for (const { path, period_cycles } of list) {
    store.watched.set(path, { ...store.watched.get(path), period_cycles });
    historyFor(path, period_cycles); // re-rates the history when the period moved
  }
  if (andCommit) afterEdit();
  else {
    notify("watched", store.watched);
    renderPreview();
  }
}

function afterEdit() {
  notify("watched", store.watched);
  renderPreview();
  clearTimeout(debounceTimer);
  debounceTimer = setTimeout(commit, COMMIT_DEBOUNCE_MS);
}

/** Send the full list; the stream restarts from tick 0 on acceptance. */
let pausedDeferral = false;
let clearOnStatus = false; // an install is awaiting its trace-status event
export async function commit() {
  clearTimeout(debounceTimer);
  if (store.gate !== "matched") return;
  // views_008's sacred span: an accepted install clears every history, so
  // any commit landing while paused — a reconnect's recommit included —
  // defers to resume; the frozen inspection stays consistent.
  if (store.timeline.mode === "paused") {
    pausedDeferral = true;
    return;
  }
  pausedDeferral = false;
  const list = entries().map(({ path, period_cycles }) => ({ path, period_cycles }));
  const key = JSON.stringify(list);
  if (key === committed) return;
  try {
    // The clear rides the trace-status EVENT (see initWatchflow): the core
    // queues it behind the prior list's final batch, so clearing on this
    // command's resolution could wipe histories the batch then refills.
    clearOnStatus = true;
    await api.installWatches(list);
    committed = key;
  } catch (cause) {
    clearOnStatus = false;
    // The device still runs the previous list, so the app goes back to it:
    // a refused period edit left behind would label the signal at a rate
    // nothing streams, and its history would read as one long gap.
    const fixes = computeFixes(entries());
    applyList(JSON.parse(committed || "[]"), false);
    set({ budgetVerdict: String(cause) });
    showRejectDialog(String(cause), fixes);
  }
}

/** Re-commit when the gate opens with a restored list pending. */
export function initWatchflow() {
  subscribe("gate", (gate) => {
    if (gate === "matched" && store.watched.size) commit();
  });
  // [impl->app~conn_001~1] The board's watch list lives on the CDC session
  // (a dropped DTR clears it device-side), so any departure from
  // "connected" invalidates the committed-list cache — the next gate open
  // recommits in full even though the list itself never changed.
  subscribe("connection", (c) => {
    if (c.state !== "connected") committed = "";
  });
  // A commit deferred by a pause (see commit()) fires on resume.
  subscribe("timeline", () => {
    if (store.timeline.mode === "live" && pausedDeferral) commit();
  });
  subscribe("traceStatus", renderPreview);
  // views_008's clear: only an install we sent clears histories — a plain
  // status query must leave them alone.
  subscribe("trace-status", () => {
    if (!clearOnStatus) return;
    clearOnStatus = false;
    for (const h of histories.values()) h.clear();
    notify("stream-restart");
  });
  // Snapshot-restored meta is only as fresh as the last session (an old
  // snapshot has no enums; a reloaded ELF may rename them) — every arriving
  // signal list re-resolves the watched paths' meta.
  subscribe("signals", (signals) => {
    let changed = false;
    for (const s of signals) {
      if (store.watched.has(s.path)) {
        meta.set(s.path, { size: s.size, kind: s.kind, enums: s.enums });
        changed = true;
      }
    }
    if (changed) {
      renderPreview();
      notify("watched", store.watched);
    }
  });
  renderPreview();
}

// ── reject dialog: quote the firmware verbatim, suggest app-computed fixes ──

/** Fixes for the REFUSED list (the rollback has since restored the running
 *  one), each applying that list with the fix in place. */
function computeFixes(list) {
  const fixes = [];
  // One-cycle entries dominate both budgets, so they get two fixes: slow
  // them, or drop them.
  const fastest = list.filter((e) => e.period_cycles === 1);
  if (fastest.length) {
    const plural = fastest.length > 1 ? "s" : "";
    const moved = list.map((e) => (e.period_cycles === 1 ? { ...e, period_cycles: 200 } : e));
    fixes.push({
      label: `Move the ${fastest.length} 20 kHz signal${plural} to 10 ms → ${pctOfLink(preview(moved).r)} of link`,
      apply: () => applyList(moved, true),
    });
    const kept = list.filter((e) => e.period_cycles !== 1);
    fixes.push({
      label: `Drop the ${fastest.length} 20 kHz signal${plural} → ${pctOfLink(preview(kept).r)} of link`,
      apply: () => applyList(kept, true),
    });
  }
  if (list.length > 1) {
    const heaviest = [...list].sort((a, b) => b.size / b.period_cycles - a.size / a.period_cycles)[0];
    const lighter = list.filter((e) => e.path !== heaviest.path);
    fixes.push({
      label: `Drop ${heaviest.path.split(".").pop()} → ${pctOfLink(preview(lighter).r)}`,
      apply: () => applyList(lighter, true),
    });
  }
  return fixes;
}

function pctOfLink(r) {
  const max = store.traceStatus?.link_budget_bytes_per_s ?? 480_000;
  return `${Math.round((r / max) * 100)} %`;
}

let closeRejectDialog = null; // replacing a dialog must also release its listener

function showRejectDialog(cause, fixes) {
  closeRejectDialog?.();
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
  const close = () => {
    scrim.remove();
    window.removeEventListener("keydown", onKey, true);
    closeRejectDialog = null;
  };
  closeRejectDialog = close;
  // Modal keyboard contract: Escape dismisses (capture — the dialog is
  // topmost), and focus starts on the dismiss action.
  const onKey = (ev) => {
    if (ev.key === "Escape") {
      ev.stopImmediatePropagation();
      close();
    }
  };
  window.addEventListener("keydown", onKey, true);
  scrim.addEventListener("click", (ev) => {
    if (ev.target.dataset.fix !== undefined) { fixes[+ev.target.dataset.fix].apply(); close(); }
    else if (ev.target.dataset.dismiss !== undefined || ev.target === scrim) close();
  });
  document.querySelector(".workspace").appendChild(scrim);
  scrim.querySelector("[data-dismiss]").focus();
}
