// The plot timeline — ONE per app: a display span every plot shares, and a
// live/paused mode. Live follows the stream's trailing span; Pause freezes
// the span-window preceding the pause, and wheel-zoom / pan / drag-select
// adjust the visible sub-range, bounded by that window. Resume returns to
// live. The bar UI follows the handoff's extension rules: pill family,
// tokens only, mono for machine-authored values, no animation.
// [impl->app~views_008~1]

import { store, notify, prefs } from "../state.js";
import { histories } from "./history.js";

export const SPANS_MS = [5_000, 10_000, 30_000, 60_000];
const MIN_RANGE_MS = 50; // zoom-in floor across the plot
const LS_KEY = "cockpit.timeline.span.v1";

const tl = () => store.timeline;

/** Newest tick across every history — the shared live edge (every plot
 *  renders the same X range, so the edge is global, not per widget). */
export function globalNewest() {
  let newest = 0;
  for (const h of histories.values()) {
    const t = h.newestTick();
    if (t != null && t > newest) newest = t;
  }
  return newest;
}

/** The X range every plot renders right now. */
export function currentWindow() {
  const t = tl();
  if (t.mode === "paused" && t.window) return t.window;
  const newest = Math.max(globalNewest(), t.span_ms);
  return [newest - t.span_ms, newest];
}

function apply(patch) {
  store.timeline = { ...tl(), ...patch };
  notify("timeline", store.timeline);
  renderBar();
}

/** Span selection is a live-mode action; the pills are disabled paused. */
export function setSpan(span_ms) {
  if (!SPANS_MS.includes(span_ms) || tl().mode === "paused") return;
  prefs.set(LS_KEY, span_ms);
  apply({ span_ms });
}

export function pause() {
  if (tl().mode === "paused") return;
  const newest = Math.max(globalNewest(), tl().span_ms);
  const win = [newest - tl().span_ms, newest];
  apply({ mode: "paused", pausedSpan: win, window: [...win] });
}

export function resume() {
  if (tl().mode !== "paused") return;
  apply({ mode: "live", pausedSpan: null, window: null });
}

/** Clamp a candidate window into the paused span, preserving its width. */
function clampWindow([a, b]) {
  const [s0, s1] = tl().pausedSpan;
  const w = Math.min(Math.max(b - a, MIN_RANGE_MS), s1 - s0);
  b = a + w;
  if (a < s0) { a = s0; b = a + w; }
  if (b > s1) { b = s1; a = b - w; }
  return [a, b];
}

// ── paused range control (the range mutations behind the canvas actions) ──
// [impl->app~views_009~1]

/** Zoom about a tick — the held cursor time, else the range's center. */
export function zoomAt(center, factor) {
  if (tl().mode !== "paused") return;
  const [a, b] = tl().window;
  if (center == null || center < a || center > b) center = (a + b) / 2;
  apply({ window: clampWindow([center - (center - a) * factor, center + (b - center) * factor]) });
}

/** Pan at the current scale, bounded by the paused span's edges. */
export function panBy(delta_ms) {
  if (tl().mode !== "paused") return;
  const [a, b] = tl().window;
  apply({ window: clampWindow([a + delta_ms, b + delta_ms]) });
}

/** A dragged X section: every plot zooms to it. */
export function selectRange(a, b) {
  if (tl().mode !== "paused") return;
  if (b < a) [a, b] = [b, a];
  apply({ window: clampWindow([a, b]) });
}

// ── the bar ────────────────────────────────────────────────────────────────

let bar = null;

export function initTimeline() {
  const saved = Number(prefs.get(LS_KEY));
  if (SPANS_MS.includes(saved)) store.timeline = { ...tl(), span_ms: saved };

  const workspace = document.querySelector(".workspace");
  bar = document.createElement("div");
  bar.className = "timeline-bar";
  workspace.insertBefore(bar, workspace.firstChild);
  bar.addEventListener("click", (ev) => {
    const span = ev.target.closest("[data-span]");
    if (span && !span.disabled) setSpan(+span.dataset.span);
    if (ev.target.closest("[data-pause]")) pause();
    if (ev.target.closest("[data-resume]")) resume();
  });
  renderBar();
}

const fmtS = (ms) => `${(ms / 1000).toFixed(ms % 1000 ? 2 : 0)} s`;

function renderBar() {
  if (!bar) return;
  const t = tl();
  const paused = t.mode === "paused";
  bar.innerHTML = `
    <span class="timeline-label">plot duration</span>
    <span class="span-group" role="group" aria-label="Plot duration">
      ${SPANS_MS.map(
        (s) =>
          `<button class="span-pill mono ${s === t.span_ms ? "is-selected" : ""}"
             data-span="${s}" ${paused ? "disabled" : ""}>${s / 1000} s</button>`,
      ).join("")}
    </span>
    ${paused
      ? `<button class="btn btn-primary" data-resume>Resume</button>
         <span class="timeline-readout mono">paused · showing ${fmtS(t.window[0])} – ${fmtS(t.window[1])}</span>`
      : `<button class="btn btn-secondary" data-pause>Pause</button>`}`;
}
