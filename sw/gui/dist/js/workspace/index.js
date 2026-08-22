// Workspace orchestrator: mounts the grid, fans "samples" batches into the
// per-signal histories, and refreshes widgets at batch rate (~20 Hz),
// never per sample.

import { store, subscribe, api, notify } from "../state.js";
import { historyFor, histories } from "./history.js";
import { initWatchflow, addWatch, setPeriod, removeWatch, commit } from "./watchflow.js";
import { initLayout, forEachWidget, addWidget, persist } from "./layout.js";
import { initPickerWatchControls } from "./pickerhooks.js";
import { initWatchPanel } from "./watchpanel.js";
import { setCursorTick, clearCursor, cursor } from "./cursor.js";
import { appearanceOf, setAppearance, resolvedColor } from "./appearance.js";
import { evalMonotoneRun, monotoneTangents } from "./interp.js";
import {
  initTimeline, setSpan, pause, resume, zoomAt, panBy, selectRange, currentWindow,
} from "./timeline.js";

export function initWorkspace() {
  initWatchflow();
  initTimeline();
  initLayout();
  initPickerWatchControls();
  initWatchPanel();

  let refreshQueued = false;
  const refreshAll = () => {
    refreshQueued = false;
    forEachWidget((w) => w.refresh());
  };

  subscribe("samples", (batch) => {
    for (const sig of batch.signals || []) {
      const w = store.watched.get(sig.path);
      if (!w) continue; // late batch for a signal just removed
      const h = historyFor(sig.path, w.period_ms);
      h.append(sig.points);
    }
    if (!refreshQueued) {
      refreshQueued = true;
      requestAnimationFrame(refreshAll);
    }
  });

  subscribe("watched", () => forEachWidget((w) => w.renderLegend?.() || w.refresh()));
  window.addEventListener("resize", () => forEachWidget((w) => w.refresh()));

  // An appearance edit propagates everywhere the signal renders: refresh the
  // watched map's resolved color (legend/picker/watch-panel/table all read
  // it), rebuild the plots (series opts are fixed at instance creation), and
  // persist — appearance restores with the layout.
  subscribe("appearance", (path) => {
    const w = store.watched.get(path);
    if (w) {
      store.watched.set(path, { ...w, color: resolvedColor(path) });
      notify("watched", store.watched);
    }
    forEachWidget((wd) => wd.rebuild?.() ?? wd.refresh());
    persist();
  });

  // A theme is a token block — the trace cycle retints with it. Colors are
  // slot-stable per signal; re-resolve and rebuild (series strokes are fixed
  // at instance creation). The appearance rule applies here: an un-overridden
  // signal re-resolves to its slot's new token, while a user-overridden color
  // is absolute and comes back from resolvedColor exactly as chosen.
  new MutationObserver(() => {
    for (const [path, w] of store.watched) {
      store.watched.set(path, { ...w, color: resolvedColor(path) });
    }
    notify("watched", store.watched);
    forEachWidget((w) => w.rebuild?.() ?? w.refresh());
  }).observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });

  // Test/debug surface (the playwright suite drives the app through this;
  // harmless under Tauri).
  window.__cockpit = {
    store, api, notify, addWatch, setPeriod, removeWatch, commit,
    addWidget, forEachWidget, setCursorTick, clearCursor, cursor, histories,
    timeline: { get: () => store.timeline, setSpan, pause, resume, zoomAt, panBy, selectRange, currentWindow },
    appearance: { of: appearanceOf, set: setAppearance, resolvedColor },
    interp: { evalMonotoneRun, monotoneTangents },
  };
}
