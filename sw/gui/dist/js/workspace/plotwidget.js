// Plot widget: glrender.js draws the traces, DOM chrome does the rest. Each
// signal renders from its OWN decimated table (nulls split runs at gaps), so
// no cross-signal alignment exists to punch holes through.
// [impl->app~views_001~1]

import { icon } from "../icons.js";
import { store, subscribe } from "../state.js";
import { histories, lowerBound } from "./history.js";
import { meta } from "./watchflow.js";
import { traceDashed } from "./colors.js";
import { appearanceOf, resolvedColor } from "./appearance.js";
import { GlTraces, buildTraceGeometry, arcAtX, parseColor, DOT_SIZE_PX } from "./glrender.js";
import { envelopeTable } from "./decimate.js";
import { cursor, setCursorTick, clearCursor } from "./cursor.js";
import { currentWindow, displayWindow, zoomAt, panBy, selectRange } from "./timeline.js";
import { toggleAxesConfig, closeAxesConfigFor } from "./axesconfig.js";
import { wireTitleEditor, updateTitle } from "./titlebar.js";
import { isAnchorModifier, ANCHOR_KEY } from "../platform.js";
import { esc, shortName } from "../dom.js";

// Wheel-zoom feel (app~views_009 pins 1.0015 per wheel-delta unit): a taste
// constant — retune if the bench prefers another direction or pace.
const WHEEL_ZOOM_BASE = 1.0015;

const BASE_STROKE_W = 1.5;
const POINTED_MAX_PX = 40; // app~views_012's pointing threshold
const CLICK_MAX_PX = 6; // press+release under this is a click; over, a select drag
const ANCHOR_HIT_PX = 8; // bare-click release distance to an anchor line

export class PlotWidget {
  /** cfg: { id, signals: [path], sides: {path:'L'|'R'}, scales: {L|R: {mode:'auto'|'manual', min, max}},
   *         x, y, w, h (px, lattice-snapped — owned by layout.js) } */
  constructor(cfg, hooks) {
    this.cfg = { sides: {}, scales: {}, ...cfg };
    // Pre-axes-config snapshots stored an axisMode: 'shared' meant one scale
    // (sides cleared), 'split' kept its sides. The mode is now derived from
    // the per-signal assignments.
    if (this.cfg.axisMode === "shared") this.cfg.sides = {};
    delete this.cfg.axisMode;
    this.hooks = hooks; // { onChange(), onRemove(id) }
    this.el = document.createElement("div");
    this.el.className = "plot-widget widget";
    this.el.dataset.widgetId = this.cfg.id;
    this.gl = null;          // GlTraces, created on first sized refresh
    this._ranges = {};       // last-applied Y range per scale group key
    this.window = currentWindow();
    this.pointed = null;    // transient pointed-trace path (app~views_012)
    this.pointedRaf = null;
    this.anchor = null;     // comparison anchor { path, tick, value } (app~views_017)
    this.preview = null;    // modifier-held anchor candidate { path, tick, value } (app~views_019)
    this._ptr = null;       // last pointer position over the canvas
    this._ctrl = false;     // anchor-modifier state (Ctrl; Command on macOS), tracked so a
    //                         keypress with a still pointer previews too
    this.render();
    // The modifier can change without a pointer event (press/release with
    // the mouse still, or released outside the window entirely — blur; on
    // macOS a Cmd-driven app switch never fires the keyup, so blur is the
    // release path there).
    this._onKey = (ev) => {
      if (ev.key !== ANCHOR_KEY) return;
      this._ctrl = ev.type === "keydown";
      this.refreshPreview();
    };
    this._onBlur = () => {
      this._ctrl = false;
      this.refreshPreview();
    };
    window.addEventListener("keydown", this._onKey);
    window.addEventListener("keyup", this._onKey);
    window.addEventListener("blur", this._onBlur);
    this.unsubs = [
      subscribe("cursor", () => this.renderCursor()),
      subscribe("timeline", () => {
        if (store.timeline.mode !== "paused") {
          this.applyPointed(null);
          this.releaseAnchor(); // Resume releases (app~views_017)
        }
        this.scheduleRefresh();
      }),
    ];
  }

  /** Leading-edge + trailing rAF coalesce: the first trigger renders
   *  synchronously (interaction reads stay coherent); same-frame repeats
   *  (wheel bursts, resize streams) fold into one trailing refresh. */
  scheduleRefresh() {
    if (this._refreshRaf) {
      this._refreshDirty = true;
      return;
    }
    this.refresh();
    this._refreshRaf = requestAnimationFrame(() => {
      this._refreshRaf = null;
      if (this._refreshDirty) {
        this._refreshDirty = false;
        this.refresh();
      }
    });
  }

  // [impl->app~views_016~1] unset title: earliest-added signal's leaf name,
  // `Plot` while empty (cfg.signals order is add order).
  title() {
    return this.cfg.title || (this.cfg.signals[0] ? shortName(this.cfg.signals[0]) : "Plot");
  }

  render() {
    const split = this.isSplit();
    this.el.classList.toggle("plot-widget--split", split);
    this.el.innerHTML = `
      <div class="widget-head" data-drag-handle>
        ${icon("grip-vertical")}
        <span class="widget-title display">${esc(this.title())}</span>
        <button class="axis-mode axis-mode--derived" data-axesconfig
          title="Axes configuration">${split ? "split" : "shared Y"}</button>
        <span class="widget-legend"></span>
        <button class="widget-menu" data-axesconfig aria-label="Widget menu">${icon("more-horizontal")}</button>
      </div>
      <div class="plot-body">
        <div class="y-axis y-axis--left"></div>
        <div class="plot-canvas">
          <div class="plot-grid"></div>
          <div class="trace-host"></div>
          <div class="gap-ribbon"></div>
          <span class="plot-watermark">canvas · webgl${split ? " · 2 scales" : ""}</span>
          <div class="plot-empty-hint" hidden>drop a signal to trace it</div>
          <div class="cursor-line" hidden></div>
          <div class="anchor-line-x" hidden></div>
          <div class="anchor-line-y" hidden></div>
          <div class="anchor-preview-y" hidden></div>
          <div class="cursor-readout" hidden></div>
        </div>
        <div class="y-axis y-axis--right" ${split ? "" : "hidden"}></div>
      </div>
      <div class="x-axis"></div>
      <div class="resize-handle" data-resize aria-hidden="true"></div>`;

    const $w = (sel) => this.el.querySelector(sel);
    this._els = {
      canvas: $w(".plot-canvas"),
      host: $w(".trace-host"),
      yL: $w(".y-axis--left"),
      yR: $w(".y-axis--right"),
      xAxis: $w(".x-axis"),
      ribbon: $w(".gap-ribbon"),
      legend: $w(".widget-legend"),
      emptyHint: $w(".plot-empty-hint"),
      cursorLine: $w(".cursor-line"),
      readout: $w(".cursor-readout"),
      anchorX: $w(".anchor-line-x"),
      anchorY: $w(".anchor-line-y"),
      previewY: $w(".anchor-preview-y"),
    };

    // Both the ⋯ menu and the derived axis chip open the axes configuration —
    // assignment, scale modes, and signal removal all live there.
    this.el.querySelectorAll("[data-axesconfig]").forEach((b) =>
      b.addEventListener("click", (ev) => {
        ev.stopPropagation();
        toggleAxesConfig(this, b);
      }),
    );

    const canvas = this._els.canvas;
    canvas.addEventListener("pointermove", (ev) => {
      // The shared cursor is pure window math — it must work (app-wide!)
      // even while this widget's renderer is unavailable.
      const rect = canvas.getBoundingClientRect();
      const tick = Math.round(this.tickAtPx(ev.clientX - rect.left, rect.width));
      setCursorTick(tick);
      this._ptr = { x: ev.clientX, y: ev.clientY };
      this._ctrl = isAnchorModifier(ev);
      this.trackSelect(canvas, ev);
      this.trackPointed(ev.clientX, ev.clientY, canvas);
    });
    canvas.addEventListener("pointerleave", () => {
      clearCursor();
      this._ptr = null;
      this.applyPointed(null);
      this.refreshPreview();
    });
    // Plots own their pointer gestures: macOS synthesizes a secondary
    // click from Ctrl+click and WKWebView pops its context menu over it —
    // scoped here so the rest of the app keeps the default menu.
    canvas.addEventListener("contextmenu", (ev) => ev.preventDefault());
    this.wireTimelineActions(canvas);

    wireTitleEditor(this);
    this.renderLegend();
    this.rebuild();
  }

  // ── paused-range controls: wheel zooms, horizontal/Shift+wheel pans,
  // horizontal drag zooms every plot to the selection. No-ops while live.
  // [impl->app~views_009~1]

  wireTimelineActions(canvas) {
    canvas.addEventListener(
      "wheel",
      (ev) => {
        if (store.timeline.mode !== "paused") return;
        ev.preventDefault();
        const rect = canvas.getBoundingClientRect();
        const msPerPx = (this.window[1] - this.window[0]) / Math.max(1, rect.width);
        const dx = ev.deltaX || (ev.shiftKey ? ev.deltaY : 0);
        if (dx && Math.abs(dx) >= Math.abs(ev.shiftKey ? 0 : ev.deltaY)) {
          panBy(dx * msPerPx);
        } else {
          zoomAt(cursor.tick, Math.pow(WHEEL_ZOOM_BASE, ev.deltaY));
        }
      },
      { passive: false },
    );

    canvas.addEventListener("pointerdown", (ev) => {
      if (store.timeline.mode !== "paused" || ev.button !== 0) return;
      this.click = { x0: ev.clientX, y0: ev.clientY, ctrl: isAnchorModifier(ev) };
      // The anchor modifier scopes the press to the comparison anchor
      // (app~views_017): no range select can start, so a modifier+drag is
      // inert and a modifier+click drops without ever fighting the
      // views_009 gesture.
      if (!isAnchorModifier(ev)) this.sel = { x0: ev.clientX, active: false, el: null };
      canvas.setPointerCapture?.(ev.pointerId);
    });
    const end = (ev) => {
      const sel = this.sel;
      const click = this.click;
      this.sel = null;
      this.click = null;
      sel?.el?.remove();
      if (sel?.active) {
        const rect = canvas.getBoundingClientRect();
        const [t0, t1] = this.window;
        const toTick = (x) => t0 + ((x - rect.left) / Math.max(1, rect.width)) * (t1 - t0);
        selectRange(toTick(Math.min(sel.x0, ev.clientX)), toTick(Math.max(sel.x0, ev.clientX)));
        return;
      }
      // A click is a press+release under the select-activation travel — the
      // x metric only, matching the horizontal select gesture it yields to,
      // so vertical wiggle never voids a click. [impl->app~views_017~1]
      if (!click) return;
      if (Math.abs(ev.clientX - click.x0) >= CLICK_MAX_PX) return;
      if (click.ctrl) {
        this.dropAnchor(ev.clientX, ev.clientY, canvas);
      } else {
        const rect = canvas.getBoundingClientRect();
        this.releaseAnchorNear(ev.clientX - rect.left, ev.clientY - rect.top, rect);
      }
    };
    canvas.addEventListener("pointerup", end);
    canvas.addEventListener("pointercancel", () => {
      this.sel?.el?.remove();
      this.sel = null;
      this.click = null;
    });
  }

  // ── pixel <-> data mapping (the renderer, cursor, and hit-tests share it) ──

  /** The window on SCREEN this frame. Live, the display clock's smoothly
   *  scrolled window (what the translated geometry shows — up to a lead
   *  ahead of this.window, the batch-time geometry window); paused, the
   *  frozen range. Every interaction mapping routes through this so the
   *  cursor and hit-tests agree with the drawn translation. */
  viewWindow() {
    return store.timeline.mode === "paused" ? this.window : displayWindow();
  }

  /** Tick at a canvas-relative x (css px). */
  tickAtPx(px, width) {
    const [t0, t1] = this.viewWindow();
    return t0 + (px / Math.max(1, width)) * (t1 - t0);
  }

  /** Canvas-relative y (css px) of a value on the given scale group. */
  yPxOf(v, side, height) {
    const [min, max] = this._ranges[side] || [0, 1];
    return (1 - (v - min) / (max - min || 1)) * height;
  }

  /** Window fraction of a tick (tickAtPx's inverse, width-free), against
   *  the on-screen window. */
  xFracOf(t) {
    const [t0, t1] = this.viewWindow();
    return (t - t0) / (t1 - t0 || 1);
  }

  /** views_012 emphasis: the pointed trace renders at twice the base width. */
  strokeWidthFor(path) {
    return path === this.pointed ? BASE_STROKE_W * 2 : BASE_STROKE_W;
  }

  /** The translation (css px) that maps the batch-window geometry onto the
   *  display window. Negative while the display now leads the newest data:
   *  the honest trailing strip at the right edge, at most a batch wide. */
  scrollOffsetPx() {
    if (store.timeline.mode === "paused") return 0;
    return (this.window[1] - displayWindow()[1]) * (this._pxPerMs || 0);
  }

  /** Display-rate live scroll: redraw cached geometry at the new
   *  translation and glide the cursor overlays with it. No allocation, no
   *  layout read — geometry, ranges, labels, and ribbons stay at batch
   *  rate (refresh). */
  scrollTick() {
    if (!this.gl?.alive() || !this._built?.length) return;
    const off = this.scrollOffsetPx();
    if (off === this._scrollOff) return;
    this._scrollOff = off;
    this.gl.scroll(off);
    if (cursor.tick !== null) this.positionCursor();
  }

  // ── pointed-trace emphasis (paused only): nearest rendered line ≤ 40 px
  // doubles its stroke via a cached-geometry redraw. Transient — persisted
  // appearance and cfg are never touched. [impl->app~views_012~1]

  trackPointed(clientX, clientY, canvas) {
    if (this.pointedRaf) return;
    this.pointedRaf = requestAnimationFrame(() => {
      this.pointedRaf = null;
      this.applyPointed(this.computePointed(clientX, clientY, canvas));
      this.refreshPreview(); // the candidate rides the same resolve (app~views_019)
    });
  }

  /** The nearest pointable trace at the pointer, or null. A series is
   *  pointable at x only where a sample-bearing segment exists: inside a
   *  tick-count gap (a null marker on either side) it does not participate. */
  computePointed(clientX, clientY, canvas) {
    if (store.timeline.mode !== "paused" || !this.gl) return null;
    const rect = canvas.getBoundingClientRect();
    const px = clientX - rect.left;
    const py = clientY - rect.top;
    const t = this.tickAtPx(px, rect.width);
    let best = null;
    for (const path of this.cfg.signals) {
      const [xs, ys] = this.tableFor(path);
      if (!xs.length) continue;
      const iR = lowerBound(xs, t);
      const iL = iR - 1;
      const vL = iL >= 0 ? ys[iL] : undefined;
      const vR = iR < xs.length ? ys[iR] : undefined;
      // A null neighbor is a gap marker: no segment here. Off either end of
      // the data, clamp to the nearest run endpoint instead.
      let yVal;
      if (vL != null && vR != null) {
        yVal = appearanceOf(path).interp === "zoh"
          ? vL // the rendered step holds the left value until the next sample
          : vL + ((vR - vL) * (t - xs[iL])) / (xs[iR] - xs[iL] || 1);
      } else if (vL != null && vR === undefined) {
        yVal = vL;
      } else if (vR != null && vL === undefined) {
        yVal = vR;
      } else {
        continue; // inside a gap (or empty series)
      }
      const yPx = this.yPxOf(yVal, this.sideOf(path), rect.height);
      const dy = Math.abs(py - yPx);
      if (dy <= POINTED_MAX_PX && (!best || dy < best.dy)) best = { path, dy };
    }
    return best ? best.path : null;
  }

  applyPointed(path) {
    if (path === this.pointed) return;
    this.pointed = path;
    if (this.gl && this._built) {
      let restroke = false;
      this._built.forEach((tr, i) => {
        const want = this.strokeWidthFor(this.cfg.signals[i]);
        if (tr.widthPx !== want) {
          tr.widthPx = want;
          restroke = true;
        }
      });
      if (restroke) this.gl.drawCached();
    }
    this.el.querySelectorAll(".cursor-readout .readout-row").forEach((r) =>
      r.classList.toggle("readout-row--pointed", r.dataset.path === path),
    );
    // The pointed row carries the value delta (app~views_018): a pointer move
    // that transfers the emphasis without changing the cursor tick fires no
    // "cursor" notify, so the readout re-renders here.
    if (this.anchor) this.renderCursor();
  }

  /** Grow the drag-select region under the pointer (called from pointermove). */
  trackSelect(canvas, ev) {
    const sel = this.sel;
    if (!sel) return;
    const dx = ev.clientX - sel.x0;
    if (!sel.active && Math.abs(dx) > CLICK_MAX_PX) {
      sel.active = true;
      sel.el = document.createElement("div");
      sel.el.className = "select-region";
      canvas.appendChild(sel.el);
    }
    if (sel.active) {
      const rect = canvas.getBoundingClientRect();
      sel.el.style.left = `${Math.min(sel.x0, ev.clientX) - rect.left}px`;
      sel.el.style.width = `${Math.abs(dx)}px`;
    }
  }

  // ── comparison anchor: a sample marked by time + value lines against its
  // signal's own axis. Paused-only by lifecycle, never persisted.
  // [impl->app~views_017~1]

  /** The sample a Ctrl+click at this pointer x anchors: the pointed
   *  signal's sample nearest in time, the earlier of two equidistant.
   *  Shared by the drop (app~views_017) and its preview (app~views_019). */
  snapSample(path, clientX, canvas) {
    const h = histories.get(path);
    if (!h || !h.size) return null;
    const rect = canvas.getBoundingClientRect();
    const t = this.tickAtPx(clientX - rect.left, rect.width);
    const iR = h.indexAtOrAfter(t);
    const iL = iR - 1;
    let i;
    if (iL < 0) i = iR;
    else if (iR >= h.size) i = iL;
    else i = t - h.tickAtIndex(iL) <= h.tickAtIndex(iR) - t ? iL : iR; // earlier wins a tie
    return { path, tick: h.tickAtIndex(i), value: h.valueAtIndex(i) };
  }

  dropAnchor(clientX, clientY, canvas) {
    // Recompute the pointed trace synchronously: the rAF-tracked this.pointed
    // may trail the pointer by a frame at click time.
    const path = this.computePointed(clientX, clientY, canvas);
    if (!path) return; // the set action requires a pointed trace
    const snapped = this.snapSample(path, clientX, canvas);
    if (!snapped) return;
    this.anchor = snapped;
    this.renderAnchor();
    this.renderCursor();
  }

  // ── anchor preview: the candidate mark a modifier+click would anchor. Not
  // an anchor line — the release hit-test never sees it.
  // [impl->app~views_019~1]

  refreshPreview() {
    const canvas = this._els?.canvas;
    let next = null;
    if (
      this._ctrl && this._ptr && canvas &&
      store.timeline.mode === "paused"
    ) {
      // The candidate follows the pointed trace; resolve at the stored
      // pointer so a Ctrl press with a still mouse previews immediately.
      const path = this.computePointed(this._ptr.x, this._ptr.y, canvas);
      if (path) next = this.snapSample(path, this._ptr.x, canvas);
    }
    this.preview = next;
    const ly = this._els?.previewY;
    if (!ly) return;
    if (!next) {
      ly.hidden = true;
      return;
    }
    const [min, max] = this._ranges[this.sideOf(next.path)] || [0, 1];
    const inY = next.value >= min && next.value <= max;
    ly.hidden = !inY;
    if (inY) ly.style.top = `${this.yPxOf(next.value, this.sideOf(next.path), 100).toFixed(2)}%`;
  }

  releaseAnchor() {
    if (!this.anchor) return;
    this.anchor = null;
    this.renderAnchor();
    this.renderCursor();
  }

  /** The bare-click release. Only a VISIBLE line is a target — a hidden
   *  line's projection must not keep a phantom click strip at the edge. */
  releaseAnchorNear(px, py, rect) {
    const a = this.anchor;
    if (!a) return;
    const hitX =
      !this._els.anchorX.hidden &&
      Math.abs(px - this.xFracOf(a.tick) * rect.width) <= ANCHOR_HIT_PX;
    const hitY =
      !this._els.anchorY.hidden &&
      Math.abs(py - this.yPxOf(a.value, this.sideOf(a.path), rect.height)) <= ANCHOR_HIT_PX;
    if (hitX || hitY) this.releaseAnchor();
  }

  /** Position the anchor lines; each hides alone when its coordinate leaves
   *  the current window or its axis's range (paused zoom/pan/scrub). */
  renderAnchor() {
    const lx = this._els?.anchorX;
    const ly = this._els?.anchorY;
    if (!lx) return;
    const a = this.anchor;
    if (!a) {
      lx.hidden = true;
      ly.hidden = true;
      return;
    }
    const [t0, t1] = this.window;
    const inX = a.tick >= t0 && a.tick <= t1;
    lx.hidden = !inX;
    if (inX) lx.style.left = `${(this.xFracOf(a.tick) * 100).toFixed(2)}%`;
    const [min, max] = this._ranges[this.sideOf(a.path)] || [0, 1];
    const inY = a.value >= min && a.value <= max;
    ly.hidden = !inY;
    if (inY) ly.style.top = `${this.yPxOf(a.value, this.sideOf(a.path), 100).toFixed(2)}%`;
  }

  addSignal(path) {
    if (this.cfg.signals.includes(path)) return;
    this.cfg.signals.push(path);
    this.renderLegend();
    this.rebuild();
    this.hooks.onChange();
  }

  /** Deselect: the signal leaves this widget; the widget remains (an emptied
   *  plot shows its drop hint). The caller owns the watch-level cleanup. */
  removeSignal(path) {
    const i = this.cfg.signals.indexOf(path);
    if (i < 0) return;
    this.cfg.signals.splice(i, 1);
    delete this.cfg.sides[path];
    if (this.anchor?.path === path) this.releaseAnchor(); // its signal left (app~views_017)
    this.rebuild();
    this.hooks.onChange();
  }

  setSide(path, side) {
    if (!this.cfg.signals.includes(path) || this.sideOf(path) === side) return;
    this.cfg.sides[path] = side;
    this.rebuild();
    this.hooks.onChange();
  }

  /** Per-side scale config; auto unless configured manual. */
  scaleConfig(side) {
    return this.cfg.scales[side] || { mode: "auto" };
  }

  setScaleMode(side, mode) {
    if (mode === "manual") {
      // Seed the manual bounds from the range currently on screen, so the
      // switch is a freeze, not a jump.
      const group = this.scaleGroups().find((g) => g.key === side);
      const [min, max] = this.scaleConfig(side).mode === "manual"
        ? [this.cfg.scales[side].min, this.cfg.scales[side].max]
        : this.scaleRange(group ? group.signals : []);
      this.cfg.scales[side] = { mode: "manual", min: round4(min), max: round4(max) };
    } else {
      this.cfg.scales[side] = { mode: "auto" };
    }
    this.refresh();
    this.hooks.onChange();
  }

  setManualRange(side, min, max) {
    this.cfg.scales[side] = { mode: "manual", min, max };
    this.refresh();
    this.hooks.onChange();
  }

  renderLegend() {
    this._els.legend.innerHTML = this.cfg.signals
      .map((p) => {
        const w = store.watched.get(p);
        return `<span class="legend-entry" title="${esc(p)}">
          <span class="legend-bar" style="background:${resolvedColor(p)}"></span>
          <span class="legend-name mono">${esc(shortName(p))}</span>
          <span class="legend-period">${w ? `${w.period_ms}ms` : ""}</span>
        </span>`;
      })
      .join("");
  }

  sideOf(path) {
    return this.cfg.sides[path] || "L";
  }

  /** Scale groups: one Y axis per assigned side in use — the readout
   *  sections by these, and "shared vs split" is derived from them. */
  // [impl->app~views_007~1]
  scaleGroups() {
    const L = this.cfg.signals.filter((p) => this.sideOf(p) === "L");
    const R = this.cfg.signals.filter((p) => this.sideOf(p) === "R");
    const both = L.length > 0 && R.length > 0;
    return [
      { key: "L", label: both ? "left · scale 1" : null, signals: L },
      { key: "R", label: both ? "right · scale 2" : null, signals: R },
    ].filter((g) => g.signals.length);
  }

  isSplit() {
    return this.scaleGroups().length === 2;
  }

  /** The group's Y range per its scale mode: manual bounds hold while values
   *  clip beyond them; auto follows the rendered extents. */
  axisRange(group) {
    const sc = this.scaleConfig(group.key);
    if (sc.mode === "manual" && Number.isFinite(sc.min) && Number.isFinite(sc.max)) {
      return [sc.min, sc.max];
    }
    return this.scaleRange(group.signals);
  }

  /** The signal's rendered trace style. An explicit line style — "solid"
   *  included — always wins; the 6-color overflow dash applies only while
   *  the style is auto (null). */
  // [impl->app~views_011~1]
  traceStyle(path) {
    const a = appearanceOf(path);
    const dash =
      a.style === "dotted" ? [2, 3]
      : a.style === "dashed" ? [8, 5]
      : a.style === "solid" ? null
      : traceDashed(path) ? [6, 4]
      : null;
    return {
      color: resolvedColor(path),
      dash,
      dots: a.dots,
      interp: a.interp,
    };
  }

  /** Re-derive the widget chrome after assignment/signal changes; trace
   *  geometry rebuilds on the following refresh (nothing instance-bound
   *  survives — the GL renderer redraws whole frames from built tables). */
  rebuild() {
    this.pointed = null; // strokes return to base width
    updateTitle(this); // an unset title follows the earliest-added signal
    const split = this.isSplit();
    this.el.classList.toggle("plot-widget--split", split);
    const chip = this.el.querySelector(".axis-mode--derived");
    if (chip) chip.textContent = split ? "split" : "shared Y";
    const mark = this.el.querySelector(".plot-watermark");
    if (mark) mark.textContent = `canvas · webgl${split ? " · 2 scales" : ""}`;
    this._els.emptyHint.hidden = this.cfg.signals.length > 0;
    // One axis per assigned side IN USE: an all-right plot shows only the
    // right axis, not a vacant left column's mirror.
    this._els.yR.hidden = !this.scaleGroups().some((g) => g.key === "R");
    this.renderLegend();
    this.refresh();
  }

  /** One decimated window table per signal, computed once per refresh and
   *  shared by the plot data and the auto scale ranges. The decimation
   *  (app~views_014) keeps every pixel column's extent, so ranges computed
   *  from the decimated table equal the raw window's. Queries the history
   *  ring directly (no materialized window): O(pixel columns) per trace,
   *  span- and density-independent. */
  computeTables(widthPx) {
    const cols = Math.max(1, Math.round(widthPx * (window.devicePixelRatio || 1)));
    this._tables = new Map(
      this.cfg.signals.map((p) => {
        const h = histories.get(p);
        return [p, h ? envelopeTable(h, this.window[0], this.window[1], cols) : [[], []]];
      }),
    );
  }

  tableFor(path) {
    const cached = this._tables?.get(path);
    if (cached) return cached;
    const h = histories.get(path);
    return h ? h.windowTable(this.window[0], this.window[1]) : [[], []];
  }

  /** Flush a trailing coalesced refresh so surface reads are current. */
  _flushRefresh() {
    if (this._refreshDirty) {
      this._refreshDirty = false;
      this.refresh();
    }
  }

  /** The per-signal decimated window tables as rendered (test surface). */
  renderedTables() {
    this._flushRefresh();
    return new Map(this.cfg.signals.map((p) => [p, this.tableFor(p)]));
  }

  /** Last-applied Y ranges per scale group key (test surface). */
  ranges() {
    this._flushRefresh();
    return { ...this._ranges };
  }

  /** Resolved render state per trace (test surface): width reflects the
   *  transient pointed emphasis; style/dots/interp/color per appearance. */
  traceInfo(path) {
    return { ...this.traceStyle(path), widthPx: this.strokeWidthFor(path) };
  }

  scaleRange(signals) {
    let min = Infinity, max = -Infinity;
    for (const p of signals) {
      const [, ys] = this.tableFor(p);
      for (const v of ys) if (v != null) { if (v < min) min = v; if (v > max) max = v; }
    }
    if (min === Infinity) return [0, 1];
    if (min === max) { min -= 1; max += 1; }
    const pad = (max - min) * 0.08;
    return [min - pad, max + pad];
  }

  ensureGl(width, height) {
    // A dead renderer (evicted context, failed creation) is replaced whole,
    // canvas included: eviction fires no restore event on the old canvas.
    if (this.gl && !this.gl.alive()) {
      this.gl.destroy();
      this.gl = null;
    }
    if (!this.gl) this.gl = new GlTraces(this._els.host);
    this.gl.resize(width, height);
  }

  /** Batch-rate entry point: paused with a live renderer, streaming batches
   *  change nothing on screen — zero redraws. */
  refreshBatch() {
    if (store.timeline.mode === "paused" && this.gl?.alive()) {
      this._hostSize = null; // never let a skipped cycle's measure go stale
      return;
    }
    this.refresh();
  }

  /** Batch-cycle read pass: measure every widget BEFORE any widget writes
   *  DOM — one layout flush per cycle. One-shot; refresh() consumes it, so
   *  direct refresh() callers still measure live. */
  measureHost() {
    const { width, height } = this._els.host.getBoundingClientRect();
    this._hostSize = { width, height };
  }

  /** Full re-render: re-window, rebuild trace geometry, re-label, re-ribbon.
   *  The X range is the app-level timeline's — identical on every plot. */
  refresh() {
    this.window = currentWindow();

    const { width, height } = this._hostSize ?? this._els.host.getBoundingClientRect();
    this._hostSize = null;
    if (width < 20 || height < 20) return;
    this.computeTables(width);
    this.ensureGl(width, height);

    const groups = this.scaleGroups();
    this._ranges = {};
    for (const g of groups) {
      const [min, max] = this.axisRange(g);
      this._ranges[g.key] = [min, max];
      this.renderYAxis(g.key, min, max);
    }

    const [t0, t1] = this.window;
    const pxPerMs = width / Math.max(1, t1 - t0);
    this._pxPerMs = pxPerMs;
    // Geometry maps the BATCH window (this.window) to pixels; the live
    // scroll pass translates it to the display window per frame.
    const mapX = (t) => ((t - t0) / (t1 - t0 || 1)) * width;
    // Dash-phase continuity: a per-signal arc datum carries across rebuilds
    // (advanced along the previous build's polyline to the new first
    // sample), else the clipped left edge would jump every dash per batch.
    const prevDatum = this._dashDatum || new Map();
    this._dashDatum = new Map();
    this._built = this.cfg.signals.map((p) => {
      const st = this.traceStyle(p);
      const mapY = (v) => this.yPxOf(v, this.sideOf(p), height);
      const [xs, ys] = this.tableFor(p);
      let arcStart = 0;
      if (st.dash) {
        const period = st.dash[0] + st.dash[1];
        let f = 0;
        while (f < xs.length && (ys[f] === null || !Number.isFinite(ys[f]))) f++;
        if (f < xs.length) {
          const tFirst = xs[f];
          // The y-range is deliberately NOT in the fingerprint: live auto
          // ranges breathe every few batches, and re-anchoring on each
          // breath would echo the pop this datum exists to kill.
          const fp = `${pxPerMs}|${width}|${height}|${st.interp}|${st.dash[0]}:${st.dash[1]}`;
          const prev = prevDatum.get(p);
          if (prev && prev.fp === fp && tFirst >= prev.tick) {
            const arc = tFirst === prev.tick
              ? prev.arc
              : arcAtX(prev.run0, (tFirst - prev.t0) * pxPerMs);
            arcStart = arc === null ? 0 : ((arc % period) + period) % period;
          }
          this._dashDatum.set(p, { tick: tFirst, arc: arcStart, fp, t0, run0: null });
        }
      }
      // Sample dots auto-hide when denser than they are wide — counted from
      // the table BEFORE building, so hidden dots cost no geometry.
      let wantDots = st.dots;
      if (wantDots) {
        let n = 0;
        for (const v of ys) if (v != null && Number.isFinite(v)) n++;
        wantDots = n > 0 && width / n >= DOT_SIZE_PX * 1.5;
      }
      const geo = buildTraceGeometry(xs, ys, mapX, mapY, st.interp, pxPerMs, wantDots, arcStart);
      const datum = this._dashDatum.get(p);
      if (datum) datum.run0 = geo.runs[0]?.verts || null;
      return {
        runs: geo.runs,
        dots: geo.dots,
        color: parseColor(st.color),
        widthPx: this.strokeWidthFor(p),
        dash: st.dash,
        dotSizePx: DOT_SIZE_PX,
      };
    });
    this._scrollOff = this.scrollOffsetPx();
    this.gl.draw(this._built, this._scrollOff);

    // An axis with no assigned signals shows nothing, not stale labels.
    const live = new Set(groups.map((g) => g.key));
    for (const key of ["L", "R"]) {
      if (!live.has(key)) {
        const el = key === "R" ? this._els.yR : this._els.yL;
        if (this._axisHtml?.[key]) el.innerHTML = "";
        if (this._axisHtml) this._axisHtml[key] = "";
      }
    }

    this.renderXAxis();
    this.renderGapRibbon();
    this.renderAnchor();
    this.refreshPreview(); // ranges may have moved the candidate's y
    this.renderCursor();
  }

  renderYAxis(key, min, max) {
    const el = key === "R" ? this._els.yR : this._els.yL;
    const fmt = (v) => (Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(2));
    const ticks = [...Array(5)].map((_, i) => max - ((max - min) * i) / 4);
    const html = ticks.map((v) => `<span class="mono">${fmt(v)}</span>`).join("");
    this._axisHtml ??= {};
    if (this._axisHtml[key] !== html) {
      this._axisHtml[key] = html;
      el.innerHTML = html;
    }
    el.classList.toggle("y-axis--accent", this.isSplit() && key === "L");
  }

  renderXAxis() {
    const [t0, t1] = this.window;
    // More decimals as the paused zoom narrows, so labels stay distinct.
    const dp = t1 - t0 < 2000 ? 3 : 1;
    const fmt = (t) => `${(t / 1000).toFixed(dp)} s`;
    const html =
      `<span class="mono">${fmt(t0)}</span><span class="mono">${fmt((t0 + t1) / 2)}</span><span class="mono">${fmt(t1)}</span>`;
    if (this._xAxisHtml !== html) {
      this._xAxisHtml = html;
      this._els.xAxis.innerHTML = html;
    }
  }

  renderGapRibbon() {
    const [t0, t1] = this.window;
    const spans = new Map(); // merged accent spans across this plot's signals
    for (const p of this.cfg.signals) {
      const h = histories.get(p);
      if (!h) continue;
      for (const [a, b] of h.gapsIn(t0, t1)) spans.set(`${a}:${b}`, [a, b]);
    }
    const pct = (t) => (((t - t0) / (t1 - t0)) * 100).toFixed(2);
    const html = [...spans.values()]
      .map(([a, b]) => `<span class="gap-span" style="left:${pct(a)}%;width:${Math.max(0.4, pct(b) - pct(a))}%"></span>`)
      .join("");
    if (this._ribbonHtml !== html) {
      this._ribbonHtml = html;
      this._els.ribbon.innerHTML = html;
    }
  }

  /** Per-signal cursor value, read from raw history (never decimated). */
  cursorValueFor(path) {
    return histories.get(path)?.valueNear(cursor.tick) ?? null;
  }

  /** Position-only pass for the cursor overlays against the on-screen
   *  window — the scroll pass calls this per frame so the line and readout
   *  glide with the translated traces (their CONTENT is a fixed tick's
   *  values: only positions move between content re-renders). Returns
   *  false when the tick left the window (overlays hidden). */
  positionCursor() {
    const line = this._els.cursorLine;
    const readout = this._els.readout;
    const [t0, t1] = this.viewWindow();
    if (cursor.tick === null || cursor.tick < t0 || cursor.tick > t1) {
      line.hidden = true;
      readout.hidden = true;
      return false;
    }
    const frac = (cursor.tick - t0) / (t1 - t0 || 1);
    line.hidden = false;
    line.style.left = `${(frac * 100).toFixed(2)}%`;
    // A tick that re-enters the window mid-scroll restores the readout too
    // (a signal-less plot has none to restore — renderCursor keeps it
    // hidden, and signals imply scale groups).
    if (readout.hidden && this.cfg.signals.length) readout.hidden = false;
    const flipped = frac > 0.5;
    readout.style.left = flipped ? "" : `calc(${(frac * 100).toFixed(2)}% + 10px)`;
    readout.style.right = flipped ? `calc(${((1 - frac) * 100).toFixed(2)}% + 10px)` : "";
    return true;
  }

  renderCursor() {
    const readout = this._els.readout;
    if (!this.positionCursor()) return;

    const groups = this.scaleGroups();
    // A signal-less plot has no scale groups — and no readout to render.
    if (!groups.length) {
      readout.hidden = true;
      return;
    }
    readout.hidden = false;
    // Comparison deltas (app~views_018): with an anchor held, the readout
    // head carries the time delta at every held cursor time; the pointed
    // row carries the value delta only on the anchor's axis, both signals
    // numeric (integer/float — bool and enum excluded), sample present.
    // [impl->app~views_018~1]
    const anchor = this.anchor;
    const numeric = (p) => /^[iuf]/.test(meta.get(p)?.kind || "");
    const fmtMs = (n) => n.toLocaleString("en-US").replace(/,/g, " ");
    const row = (p) => {
      const v = this.cursorValueFor(p);
      const w = store.watched.get(p);
      const absent = v === null;
      let delta = "";
      if (
        anchor && p === this.pointed && !absent &&
        this.sideOf(p) === this.sideOf(anchor.path) &&
        numeric(p) && numeric(anchor.path)
      ) {
        const dv = v - anchor.value;
        const ints = /^[iu]/.test(meta.get(p)?.kind || "") && /^[iu]/.test(meta.get(anchor.path)?.kind || "");
        delta = `<span class="readout-delta mono" data-delta-v>Δ ${dv < 0 ? "-" : "+"}${formatValue(Math.abs(dv), ints ? "i32" : "f32")}</span>`;
      }
      return `<div class="readout-row ${p === this.pointed ? "readout-row--pointed" : ""}" data-path="${esc(p)}">
        <span class="legend-bar" style="background:${absent ? "var(--ink-hint)" : resolvedColor(p)}"></span>
        <span class="readout-name mono">${esc(shortName(p))}</span>
        <span class="readout-value mono ${absent ? "readout-value--absent" : ""}">${absent ? "no sample" : esc(formatValue(v, meta.get(p)?.kind, meta.get(p)?.enums))}</span>
        ${delta}
      </div>`;
    };
    // Every scale group renders, stacked under its label (labels are null
    // while shared, leaving a single unlabeled section).
    const sections = groups
      .map((g) => `${g.label ? `<div class="field-label readout-group-label">${esc(g.label)}</div>` : ""}${g.signals.map(row).join("")}`)
      .join("");
    const dt = anchor ? cursor.tick - anchor.tick : null;
    readout.innerHTML = `
      <div class="readout-head">
        <span class="readout-time mono">${icon("crosshair")}t = ${fmtMs(cursor.tick)} ms</span>
        ${anchor ? `<span class="readout-delta mono" data-delta-t>Δ ${dt < 0 ? "-" : "+"}${fmtMs(Math.abs(dt))} ms</span>` : ""}
      </div>
      ${sections}`;
  }

  destroy() {
    closeAxesConfigFor(this);
    if (this.pointedRaf) cancelAnimationFrame(this.pointedRaf);
    if (this._refreshRaf) cancelAnimationFrame(this._refreshRaf);
    window.removeEventListener("keydown", this._onKey);
    window.removeEventListener("keyup", this._onKey);
    window.removeEventListener("blur", this._onBlur);
    this.unsubs.forEach((u) => u());
    this.gl?.destroy();
    this.gl = null;
    this.el.remove();
  }

  toJSON() {
    const { id, signals, sides, scales, x, y, w, h, title } = this.cfg;
    // Derived titles are never persisted — only a user-set name survives.
    return { type: "plot", id, signals, sides, scales, x, y, w, h, ...(title ? { title } : {}) };
  }
}

const round4 = (v) => (Number.isFinite(v) ? Math.round(v * 10000) / 10000 : v);

// [impl->app~views_013~1] the value rendering table, shared by the value
// table, the cursor readout, and the watch panel. `enums` is the signal's
// (value, name) enumerator pairs from resolution; an enum value with no
// matching enumerator renders the whole number.
export function formatValue(v, kind, enums) {
  if (v === null || v === undefined) return "no sample";
  if (kind === "bool") return v ? "true" : "false";
  if (kind === "enum") {
    const value = Math.round(v);
    const name = enums?.find((e) => e[0] === value)?.[1];
    return name ?? String(value);
  }
  if (kind && /^[iu]/.test(kind)) return String(Math.round(v));
  return Math.abs(v) >= 1000 ? v.toFixed(1) : v.toFixed(4);
}
