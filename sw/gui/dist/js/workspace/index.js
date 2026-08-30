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
  displayWindow, noteLiveEdge, advanceDisplayClock, resetDisplayClock,
} from "./timeline.js";
import { markBatch, perfSnapshot } from "../perf.js";
import { ANCHOR_KEY, anchorModifierKeyFor } from "../platform.js";

export function initWorkspace() {
  initWatchflow();
  initTimeline();
  initLayout();
  initPickerWatchControls();
  initWatchPanel();

  // One perf measurement per batch CYCLE: the append half (possibly several
  // coalesced "samples" events) accumulates here and the rAF refresh half
  // closes the measurement — two markBatch calls would halve the apparent
  // per-cycle cost.
  let pendingAppendMs = 0;
  let refreshQueued = false;
  const refreshAll = () => {
    refreshQueued = false;
    const t0 = performance.now();
    // Read pass before write pass: measuring every host first means the
    // cycle forces one layout flush, not one per widget.
    forEachWidget((w) => w.measureHost?.());
    forEachWidget((w) => (w.refreshBatch ?? w.refresh).call(w));
    markBatch(pendingAppendMs + (performance.now() - t0));
    pendingAppendMs = 0;
  };

  subscribe("samples", (batch) => {
    const t0 = performance.now();
    for (const sig of batch.signals || []) {
      const w = store.watched.get(sig.path);
      if (!w) continue; // late batch for a signal just removed
      const h = historyFor(sig.path, w.period_ms);
      h.append(sig.points);
    }
    noteLiveEdge();
    pendingAppendMs += performance.now() - t0;
    if (!refreshQueued) {
      refreshQueued = true;
      requestAnimationFrame(refreshAll);
    }
    armScroll();
  });

  // ── live smooth scroll ────────────────────────────────────────────────
  // Geometry rebuilds at batch rate (refreshAll above), but the window
  // GLIDES at display rate: each frame advances the display clock and
  // redraws every plot's cached geometry at the new X translation —
  // uniforms + draw calls, no rebuild. The loop parks itself when the
  // clock stops moving (paused, stalled stream, disconnect — the lead
  // clamp freezes the estimate) and any samples batch or timeline change
  // re-arms it.
  let scrollRaf = null;
  let idleFrames = 0;
  const scrollPass = (now) => {
    scrollRaf = null;
    if (store.timeline.mode === "paused") return; // parked; notify re-arms
    if (advanceDisplayClock(now)) {
      idleFrames = 0;
      forEachWidget((w) => w.scrollTick?.());
    } else if (++idleFrames > 30) {
      return; // frozen: park until the next batch
    }
    scrollRaf = requestAnimationFrame(scrollPass);
  };
  const armScroll = () => {
    if (scrollRaf === null && store.timeline.mode !== "paused") {
      idleFrames = 0;
      scrollRaf = requestAnimationFrame(scrollPass);
    }
  };
  subscribe("timeline", armScroll);
  // A stream restart resets the device tick domain to zero: the display
  // clock resets with it (the defensive reset in noteLiveEdge would also
  // catch it, one batch later).
  subscribe("stream-restart", resetDisplayClock);

  subscribe("watched", () => forEachWidget((w) => w.renderLegend?.() || w.refresh()));
  window.addEventListener("resize", () => forEachWidget((w) => w.refresh()));

  // An appearance edit propagates everywhere the signal renders: refresh the
  // watched map's resolved color (legend/picker/watch-panel/table all read
  // it), rebuild the plots so legends and trace styles re-derive, and
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
  // slot-stable per signal; re-resolve and rebuild so legends and traces
  // retint. The appearance rule applies here: an un-overridden signal
  // re-resolves to its slot's new token, while a user-overridden color is
  // absolute and comes back from resolvedColor exactly as chosen.
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
    timeline: { get: () => store.timeline, setSpan, pause, resume, zoomAt, panBy, selectRange, currentWindow, displayWindow },
    appearance: { of: appearanceOf, set: setAppearance, resolvedColor },
    interp: { evalMonotoneRun, monotoneTangents },
    perf: { snapshot: perfSnapshot },
    platform: { ANCHOR_KEY, anchorModifierKeyFor },
  };
}
