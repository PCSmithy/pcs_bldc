// Workspace layout: the widget grid — create at the drop position, join on
// drop-over-widget, reorder by header drag, resize by the corner handle —
// and its persistence (localStorage; WebView2 keeps it in the app's data
// dir, so the arrangement, signals, and trace colors survive restarts).
// [impl->app~views_004~1]

import { store, notify, subscribe, prefs } from "../state.js";
import { PlotWidget } from "./plotwidget.js";
import { TableWidget } from "./tablewidget.js";
import { addWatch, meta } from "./watchflow.js";
import { restoreColors, colorSlots } from "./colors.js";
import { appearanceEntries, restoreAppearance, resolvedColor } from "./appearance.js";

const LS_KEY = "cockpit.workspace.v1";
let grid = null;
let widgets = []; // ordered; index = grid order
let nextId = 1;

export function initLayout() {
  const workspace = document.querySelector(".workspace");
  grid = document.createElement("div");
  grid.className = "widget-grid";
  grid.hidden = true;
  workspace.appendChild(grid);

  wireDrops(workspace);
  wireDragReorder();
  wireResize();

  subscribe("workspace-create", (kind) => {
    if (kind === "new-plot") addWidget({ type: "plot", signals: [] });
    if (kind === "new-table") addWidget({ type: "table", signals: [] });
  });

  restore();

  // The snapshot carries the watch list (app~arch_002's reinstall source),
  // so watch edits that touch no widget — panel-only adds, period changes —
  // must persist too. Subscribed after restore(): its own notify is not a
  // reason to rewrite the snapshot mid-rebuild.
  subscribe("watched", persist);
}

function syncEmptyState() {
  const empty = document.querySelector(".workspace-empty");
  grid.hidden = widgets.length === 0;
  empty.hidden = widgets.length > 0;
}

const hooks = {
  onChange: persist,
  onRemove(id) {
    const i = widgets.findIndex((w) => w.cfg.id === id);
    if (i < 0) return;
    widgets[i].destroy();
    widgets.splice(i, 1);
    syncEmptyState();
    persist();
  },
};

export function addWidget(cfg, index = widgets.length) {
  cfg.id = cfg.id ?? `w${nextId++}`;
  const widget = cfg.type === "table" ? new TableWidget(cfg, hooks) : new PlotWidget(cfg, hooks);
  applySize(widget);
  widgets.splice(index, 0, widget);
  const before = grid.children[index] || null;
  grid.insertBefore(widget.el, before);
  syncEmptyState();
  persist();
  return widget;
}

const MIN_H_PX = 248;
const MAX_H_PX = 1400;
const DEFAULT_H_PX = 340;

/** Width is a column span (grid-aligned tiling); height is explicit pixels —
 *  row spans of stretchy 1fr rows change nothing visually (auto-fit also
 *  collapses the empty tracks a column span would grow into), which is how
 *  the corner handle came to mutate state without moving a pixel. Old
 *  snapshots stored spans in cfg.h; migrate them to pixels once. */
function applySize(widget) {
  const cfg = widget.cfg;
  if (!Number.isFinite(cfg.hpx)) {
    cfg.hpx = cfg.h ? Math.min(MAX_H_PX, Math.max(MIN_H_PX, cfg.h * 300)) : DEFAULT_H_PX;
  }
  delete cfg.h;
  widget.el.style.gridColumn = `span ${cfg.w || 1}`;
  widget.el.style.height = `${cfg.hpx}px`;
}

export function forEachWidget(fn) {
  widgets.forEach(fn);
}

/** How many widgets hold this signal — 0 means its watch has no consumer. */
export function holdersOf(path) {
  return widgets.reduce((n, w) => n + (w.cfg.signals.includes(path) ? 1 : 0), 0);
}

// ── drops: create at the drop position, or join the widget under the drop ──

function dropIndexAt(x, y) {
  for (let i = 0; i < grid.children.length; i++) {
    const r = grid.children[i].getBoundingClientRect();
    if (y < r.top || (y <= r.bottom && x < r.left + r.width / 2)) return i;
  }
  return grid.children.length;
}

function wireDrops(workspace) {
  workspace.addEventListener("dragover", (ev) => {
    if (ev.dataTransfer.types.includes("text/x-signal")) {
      ev.preventDefault();
      ev.dataTransfer.dropEffect = "copy";
    }
  });
  workspace.addEventListener("drop", (ev) => {
    const path = ev.dataTransfer.getData("text/x-signal");
    if (!path) return;
    ev.preventDefault();
    if (store.gate !== "matched") return;
    addWatch(path);
    const over = ev.target.closest(".widget");
    if (over) {
      widgets.find((w) => w.cfg.id === over.dataset.widgetId)?.addSignal(path);
      persist();
    } else if (ev.target.closest(".drop-target")) {
      ev.target.closest(".table-widget"); // handled above via .widget; kept for clarity
    } else {
      addWidget({ type: "plot", signals: [path] }, dropIndexAt(ev.clientX, ev.clientY));
    }
  });
}

// ── reorder: the widget head is the drag handle ────────────────────────────

let dragging = null;

function wireDragReorder() {
  grid.addEventListener("pointerdown", (ev) => {
    const handle = ev.target.closest("[data-drag-handle]");
    if (!handle || ev.target.closest("button") || ev.target.closest(".axis-mode")) return;
    const el = handle.closest(".widget");
    dragging = { el, moved: false };
    el.setPointerCapture?.(ev.pointerId);
  });
  grid.addEventListener("pointermove", (ev) => {
    if (!dragging) return;
    dragging.moved = true;
    dragging.el.classList.add("widget--dragging");
    const index = dropIndexAt(ev.clientX, ev.clientY);
    const before = grid.children[index] || null;
    if (before !== dragging.el && before !== dragging.el.nextSibling) {
      grid.insertBefore(dragging.el, before);
    }
  });
  const end = () => {
    if (!dragging) return;
    dragging.el.classList.remove("widget--dragging");
    if (dragging.moved) {
      widgets.sort(
        (a, b) => [...grid.children].indexOf(a.el) - [...grid.children].indexOf(b.el),
      );
      persist();
    }
    dragging = null;
  };
  grid.addEventListener("pointerup", end);
  grid.addEventListener("pointercancel", end);
}

// ── resize: the corner handle — column span horizontally, pixels vertically ──

/** One column track's resolved width (auto-fill keeps empty tracks, so the
 *  computed template always lists them). */
function trackWidth() {
  const first = getComputedStyle(grid).gridTemplateColumns.split(" ")[0];
  return parseFloat(first) || 420;
}

function wireResize() {
  let resizing = null;
  grid.addEventListener("pointerdown", (ev) => {
    const handle = ev.target.closest("[data-resize]");
    if (!handle) return;
    const el = handle.closest(".widget");
    const widget = widgets.find((w) => w.cfg.id === el.dataset.widgetId);
    resizing = { widget, startX: ev.clientX, startY: ev.clientY, w: widget.cfg.w || 1, hpx: widget.cfg.hpx };
    handle.setPointerCapture(ev.pointerId);
    ev.stopPropagation();
  });
  grid.addEventListener("pointermove", (ev) => {
    if (!resizing) return;
    resizing.widget.cfg.w = Math.max(
      1, Math.min(4, resizing.w + Math.round((ev.clientX - resizing.startX) / trackWidth())),
    );
    resizing.widget.cfg.hpx = Math.max(
      MIN_H_PX, Math.min(MAX_H_PX, resizing.hpx + (ev.clientY - resizing.startY)),
    );
    applySize(resizing.widget);
    resizing.widget.refresh?.();
  });
  const end = () => {
    if (resizing) persist();
    resizing = null;
  };
  grid.addEventListener("pointerup", end);
  grid.addEventListener("pointercancel", end);
}

// ── persistence ────────────────────────────────────────────────────────────

export function persist() {
  const snapshot = {
    widgets: widgets.map((w) => w.toJSON()),
    watched: [...store.watched.entries()].map(([path, w]) => ({
      path,
      period_ms: w.period_ms,
      ...meta.get(path),
    })),
    colors: colorSlots(),
    appearance: appearanceEntries(),
  };
  prefs.set(LS_KEY, snapshot);
}

function restore() {
  // Clone: widget cfgs must not alias the prefs object (a live mutation
  // would corrupt the persisted snapshot between saves).
  const snap = structuredClone(prefs.get(LS_KEY));
  if (!snap) { syncEmptyState(); return; }

  restoreColors(snap.colors);
  restoreAppearance(snap.appearance); // before colors resolve: overrides win
  for (const w of snap.watched || []) {
    meta.set(w.path, { size: w.size ?? 4, kind: w.kind ?? "f32" });
    store.watched.set(w.path, { period_ms: w.period_ms, color: resolvedColor(w.path) });
  }
  notify("watched", store.watched);
  for (const cfg of snap.widgets || []) {
    nextId = Math.max(nextId, parseInt(String(cfg.id).slice(1), 10) + 1 || nextId);
    addWidget(cfg);
  }
  syncEmptyState();
}
