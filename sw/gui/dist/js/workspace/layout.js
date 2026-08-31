// Workspace layout: a free canvas. Every widget carries {x, y, w, h} in CSS
// pixels, all four snapped to the 50 px layout lattice — header drag moves
// (vertical stacking included), the corner handle resizes with both extents
// snapped, pointerdown raises to the front (overlap is allowed; the widgets
// array's order IS the stacking order). Persistence via prefs (the config
// file under Tauri; localStorage under the devmock).
// [impl->app~views_004~1]

import { store, notify, subscribe, prefs } from "../state.js";
import { PlotWidget } from "./plotwidget.js";
import { TableWidget } from "./tablewidget.js";
import { addWatch, meta } from "./watchflow.js";
import { restoreColors, colorSlots } from "./colors.js";
import { appearanceEntries, restoreAppearance } from "./appearance.js";

const LS_KEY = "cockpit.workspace.v1";
const SNAP = 50;
const MIN_W = 400;
const MIN_H = 250;
const DEFAULT_W = 800;
const DEFAULT_H = 350;
const MAX_PX = 20000;

let canvas = null;
let spacer = null;
let widgets = []; // stacking order: last = frontmost
let nextId = 1;

const snap = (v) => Math.round(v / SNAP) * SNAP;

export function initLayout() {
  const workspace = document.querySelector(".workspace");
  canvas = document.createElement("div");
  canvas.className = "widget-grid";
  canvas.hidden = true;
  // The spacer pins the scroll range one cell past the furthest widget, so
  // content dragged to the edge stays reachable.
  spacer = document.createElement("div");
  spacer.className = "grid-extent";
  canvas.appendChild(spacer);
  workspace.appendChild(canvas);

  wireRaise();
  wireDrops(workspace);
  wireMove();
  wireResize();
  wireLauncher(workspace);

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
  canvas.hidden = widgets.length === 0;
  empty.hidden = widgets.length > 0;
}

/** The widget launcher (app~views_004): a floating "+" menu whose items
 *  carry data-workspace, routing exactly like the empty-state buttons. */
function wireLauncher(workspace) {
  const launcher = document.createElement("div");
  launcher.className = "widget-launcher";
  launcher.innerHTML = `
    <div class="launcher-menu" role="menu" hidden>
      <button role="menuitem" data-workspace="new-plot">Add plot</button>
      <button role="menuitem" data-workspace="new-table">Add table</button>
    </div>
    <button class="launcher-fab" aria-label="Add a widget" aria-haspopup="menu"
      aria-expanded="false">+</button>`;
  workspace.appendChild(launcher);
  const menu = launcher.querySelector(".launcher-menu");
  const fab = launcher.querySelector(".launcher-fab");
  const setOpen = (open) => {
    menu.hidden = !open;
    fab.setAttribute("aria-expanded", String(open));
  };
  fab.addEventListener("click", () => setOpen(menu.hidden));
  // A selection bubbles to the workspace-create router; the menu closes on
  // it, on any outside press, and on Escape.
  menu.addEventListener("click", (ev) => {
    if (ev.target.closest("[data-workspace]")) setOpen(false);
  });
  document.addEventListener("pointerdown", (ev) => {
    if (!menu.hidden && !ev.target.closest(".widget-launcher")) setOpen(false);
  });
  document.addEventListener("keydown", (ev) => {
    if (ev.key === "Escape" && !menu.hidden) setOpen(false);
  });
}

const hooks = {
  onChange: persist,
  onRemove(id) {
    const i = widgets.findIndex((w) => w.cfg.id === id);
    if (i < 0) return;
    widgets[i].destroy();
    widgets.splice(i, 1);
    applyStacking();
    syncExtent(); // the scroll range releases with the furthest widget
    syncEmptyState();
    persist();
  },
};

/** Clamp + snap a geometry onto the lattice; a widget arriving without a
 *  position (add-plot/add-table actions) joins the vertical stack: one cell
 *  in from the left, one cell below the furthest widget. */
function normalizeGeometry(cfg) {
  if (!Number.isFinite(cfg.x)) cfg.x = SNAP;
  if (!Number.isFinite(cfg.y)) {
    cfg.y = SNAP + widgets.reduce((m, w) => Math.max(m, w.cfg.y + w.cfg.h), 0);
  }
  if (!Number.isFinite(cfg.w)) cfg.w = DEFAULT_W;
  if (!Number.isFinite(cfg.h)) cfg.h = DEFAULT_H;
  // MAX_PX bounds a corrupt/hand-edited snapshot: without it, one absurd
  // coordinate persists an absurd scroll range.
  cfg.x = Math.min(MAX_PX, Math.max(0, snap(cfg.x)));
  cfg.y = Math.min(MAX_PX, Math.max(0, snap(cfg.y)));
  cfg.w = Math.min(MAX_PX, Math.max(MIN_W, snap(cfg.w)));
  cfg.h = Math.min(MAX_PX, Math.max(MIN_H, snap(cfg.h)));
}

function applyGeometry(widget) {
  const { x, y, w, h } = widget.cfg;
  widget.el.style.left = `${x}px`;
  widget.el.style.top = `${y}px`;
  widget.el.style.width = `${w}px`;
  widget.el.style.height = `${h}px`;
  syncExtent();
}

/** Stacking is the array order; the spacer keeps the scroll range covering
 *  the furthest widget plus one cell. */
function applyStacking() {
  widgets.forEach((w, i) => { w.el.style.zIndex = i + 1; });
}

function syncExtent() {
  let mx = 0, my = 0;
  for (const w of widgets) {
    mx = Math.max(mx, w.cfg.x + w.cfg.w);
    my = Math.max(my, w.cfg.y + w.cfg.h);
  }
  spacer.style.left = `${mx + SNAP}px`;
  spacer.style.top = `${my + SNAP}px`;
}

function raise(el) {
  const i = widgets.findIndex((w) => w.el === el);
  if (i < 0 || i === widgets.length - 1) return;
  widgets.push(...widgets.splice(i, 1));
  applyStacking();
  persist();
}

export function addWidget(cfg, at = null) {
  cfg.id = cfg.id ?? `w${nextId++}`;
  if (at) { cfg.x = at.x; cfg.y = at.y; }
  normalizeGeometry(cfg);
  const widget = cfg.type === "table" ? new TableWidget(cfg, hooks) : new PlotWidget(cfg, hooks);
  widgets.push(widget);
  canvas.appendChild(widget.el);
  applyGeometry(widget);
  applyStacking();
  syncEmptyState();
  // Attach-time refresh: the constructor's own refresh ran on a detached
  // element (zero rect) and drew nothing; paused mode would never heal it
  // (refreshBatch skips), so the first sized render happens here.
  widget.refresh?.();
  persist();
  return widget;
}

export function forEachWidget(fn) {
  widgets.forEach(fn);
}

/** How many widgets hold this signal — 0 means its watch has no consumer. */
export function holdersOf(path) {
  return widgets.reduce((n, w) => n + (w.cfg.signals.includes(path) ? 1 : 0), 0);
}

// ── raise to front: any pointerdown on a widget (capture: a handle's
//    stopPropagation must not exempt it) ─────────────────────────────────────

function wireRaise() {
  canvas.addEventListener(
    "pointerdown",
    (ev) => {
      const el = ev.target.closest(".widget");
      if (el) raise(el);
    },
    { capture: true },
  );
}

// ── drops: create at the snapped drop point, or join the widget under it ──

function wireDrops(workspace) {
  const overGlow = (on) =>
    document.querySelector(".empty-drop")?.classList.toggle("empty-drop--over", on);
  workspace.addEventListener("dragover", (ev) => {
    if (ev.dataTransfer.types.includes("text/x-signal")) {
      ev.preventDefault();
      ev.dataTransfer.dropEffect = "copy";
      overGlow(Boolean(ev.target.closest?.(".empty-drop")));
    }
  });
  workspace.addEventListener("dragleave", (ev) => {
    if (!workspace.contains(ev.relatedTarget)) overGlow(false);
  });
  workspace.addEventListener("drop", (ev) => {
    overGlow(false);
    const path = ev.dataTransfer.getData("text/x-signal");
    if (!path) return;
    ev.preventDefault();
    if (store.gate !== "matched") return;
    addWatch(path);
    const over = ev.target.closest(".widget");
    if (over) {
      widgets.find((w) => w.cfg.id === over.dataset.widgetId)?.addSignal(path);
      persist();
    } else {
      // Reveal the canvas BEFORE measuring (first drop, empty state up): a
      // hidden canvas has a zero rect, and the workspace box misses the
      // timeline bar that appears with the canvas and shifts its origin.
      canvas.hidden = false;
      document.querySelector(".workspace-empty").hidden = true;
      const r = canvas.getBoundingClientRect();
      addWidget({ type: "plot", signals: [path] }, {
        x: ev.clientX - r.left + canvas.scrollLeft,
        y: ev.clientY - r.top + canvas.scrollTop,
      });
    }
  });
}

// ── move: the widget head is the drag handle; the widget rides the lattice ──

let dragging = null;

function wireMove() {
  canvas.addEventListener("pointerdown", (ev) => {
    if (ev.button !== 0) return;
    const handle = ev.target.closest("[data-drag-handle]");
    // Interactive head elements — buttons, the axis chip, the click-to-edit
    // title and its editor — never start a move gesture.
    if (
      !handle ||
      ev.target.closest("button") ||
      ev.target.closest(".axis-mode") ||
      ev.target.closest(".widget-title, .widget-title-edit")
    ) return;
    const el = handle.closest(".widget");
    const widget = widgets.find((w) => w.cfg.id === el.dataset.widgetId);
    dragging = {
      widget, pointerId: ev.pointerId, startX: ev.clientX, startY: ev.clientY,
      // Scroll can move under the gesture (wheel, edge bump): the client
      // delta alone would then park the widget short of the pointer.
      scrollX: canvas.scrollLeft, scrollY: canvas.scrollTop,
      x: widget.cfg.x, y: widget.cfg.y, moved: false,
    };
    el.setPointerCapture?.(ev.pointerId);
  });
  canvas.addEventListener("pointermove", (ev) => {
    if (!dragging || ev.pointerId !== dragging.pointerId) return;
    dragging.moved = true;
    dragging.widget.el.classList.add("widget--dragging");
    const dx = (ev.clientX - dragging.startX) + (canvas.scrollLeft - dragging.scrollX);
    const dy = (ev.clientY - dragging.startY) + (canvas.scrollTop - dragging.scrollY);
    dragging.widget.cfg.x = Math.max(0, snap(dragging.x + dx));
    dragging.widget.cfg.y = Math.max(0, snap(dragging.y + dy));
    applyGeometry(dragging.widget);
  });
  const end = (ev) => {
    if (!dragging || ev.pointerId !== dragging.pointerId) return;
    dragging.widget.el.classList.remove("widget--dragging");
    if (dragging.moved) persist();
    dragging = null;
  };
  canvas.addEventListener("pointerup", end);
  canvas.addEventListener("pointercancel", end);
}

// ── resize: the corner handle — both extents snapped to the lattice ──

function wireResize() {
  let resizing = null;
  canvas.addEventListener("pointerdown", (ev) => {
    if (ev.button !== 0) return;
    const handle = ev.target.closest("[data-resize]");
    if (!handle) return;
    const el = handle.closest(".widget");
    const widget = widgets.find((w) => w.cfg.id === el.dataset.widgetId);
    resizing = {
      widget, pointerId: ev.pointerId, startX: ev.clientX, startY: ev.clientY,
      scrollX: canvas.scrollLeft, scrollY: canvas.scrollTop,
      w: widget.cfg.w, h: widget.cfg.h,
    };
    handle.setPointerCapture(ev.pointerId);
    ev.stopPropagation();
  });
  canvas.addEventListener("pointermove", (ev) => {
    if (!resizing || ev.pointerId !== resizing.pointerId) return;
    const dx = (ev.clientX - resizing.startX) + (canvas.scrollLeft - resizing.scrollX);
    const dy = (ev.clientY - resizing.startY) + (canvas.scrollTop - resizing.scrollY);
    resizing.widget.cfg.w = Math.max(MIN_W, snap(resizing.w + dx));
    resizing.widget.cfg.h = Math.max(MIN_H, snap(resizing.h + dy));
    applyGeometry(resizing.widget);
    (resizing.widget.scheduleRefresh ?? resizing.widget.refresh)?.call(resizing.widget);
  });
  const end = (ev) => {
    if (!resizing || ev.pointerId !== resizing.pointerId) return;
    persist();
    resizing = null;
  };
  canvas.addEventListener("pointerup", end);
  canvas.addEventListener("pointercancel", end);
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

/** A flow-grid cfg (column span in w, pixel height in hpx — span height in
 *  h before that) predates positions: no finite x, and its w/h are span-
 *  scale. Detection is PER CFG — a valid canvas-shape cfg in the same
 *  snapshot must pass through untouched, and a cfg with no geometry at all
 *  is not flow-era either (normalizeGeometry gives it the stack default). */
function isFlowCfg(cfg) {
  return (
    !Number.isFinite(cfg.x) &&
    (Number.isFinite(cfg.hpx) ||
      (Number.isFinite(cfg.w) && cfg.w <= 4) ||
      (Number.isFinite(cfg.h) && cfg.h <= 4))
  );
}

/** Map a flow cfg's spans to lattice sizes; the position stays unset, so
 *  the widget joins the vertical stack like any positionless arrival. */
function migrateFlowCfg(cfg) {
  const hpx = cfg.hpx ?? (cfg.h ? Math.min(1400, Math.max(248, cfg.h * 300)) : 340);
  cfg.w = (cfg.w || 1) * 450;
  cfg.h = Math.max(MIN_H, snap(hpx));
  delete cfg.hpx;
}

function restore() {
  // Clone: widget cfgs must not alias the prefs object (a live mutation
  // would corrupt the persisted snapshot between saves).
  const snap_ = structuredClone(prefs.get(LS_KEY));
  if (!snap_) { syncEmptyState(); return; }

  restoreColors(snap_.colors);
  restoreAppearance(snap_.appearance); // before colors resolve: overrides win
  for (const w of snap_.watched || []) {
    meta.set(w.path, { size: w.size ?? 4, kind: w.kind ?? "f32", enums: w.enums });
    store.watched.set(w.path, { period_ms: w.period_ms });
  }
  notify("watched", store.watched);
  for (const cfg of snap_.widgets || []) {
    if (isFlowCfg(cfg)) migrateFlowCfg(cfg);
    nextId = Math.max(nextId, parseInt(String(cfg.id).slice(1), 10) + 1 || nextId);
    addWidget(cfg);
  }
  syncEmptyState();
}
