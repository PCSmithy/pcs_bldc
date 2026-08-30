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
const MIN_RANGE_MS = 10; // zoom-in floor across the plot (app~views_009)
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

// ── display clock: the smoothly estimated live "now" ──────────────────────
// Batches land at ~20 Hz, so a window pinned to the newest tick jumps ~1% of
// a 5 s span per redraw — visible stutter beside the 144 Hz cursor. The
// clock advances an estimated device-time now at display rate: newest tick
// plus wall-clock elapsed since it landed, lead-clamped so a stalled stream
// freezes honestly instead of scrolling into emptiness. Monotonic — never
// backwards. Under steady streaming, forward snaps (a batch outrunning the
// estimate) are bounded by clock skew over one batch: invisible. A stall
// LONGER than the lead clamp ends differently by design: the view freezes
// at the clamp, and the batch that finally lands snaps it forward by the
// stall's full excess — a hard, honest catch-up to reality. The one
// non-monotonic case: a stream restart resets the tick domain to zero, and
// the clock snaps down with it.

const LEAD_MAX_MS = 75;   // ~1.5 batch intervals of extrapolation, no more
const CATCHUP_GAIN = 0.12; // fraction of the estimate error closed per frame
const RESET_MS = 1000;    // a target this far below the estimate is a restart

const liveEdge = { tick: 0, wall: 0 };
const disp = { now: 0, wall: 0 };

/** Note fresh appends (called after each samples batch lands). */
export function noteLiveEdge() {
  const t = globalNewest();
  const now = performance.now();
  if (t > liveEdge.tick || t < liveEdge.tick - RESET_MS) {
    liveEdge.tick = t;
    liveEdge.wall = now;
    // Reality caught up past (or reset below) the estimate: snap forward
    // (or re-seed) — the displayed now must never trail the drawn data.
    if (t > disp.now || t < disp.now - RESET_MS) disp.now = t;
  }
}

export function resetDisplayClock() {
  liveEdge.tick = 0;
  liveEdge.wall = 0;
  disp.now = 0;
  disp.wall = 0;
}

/** Advance the displayed now toward the lead-clamped estimate; true when it
 *  moved (the scroll pass parks itself once the clock stops changing). */
export function advanceDisplayClock(nowWall) {
  if (tl().mode === "paused" || !liveEdge.tick) return false;
  const target = liveEdge.tick + Math.min(nowWall - liveEdge.wall, LEAD_MAX_MS);
  let next;
  if (!disp.now || target < disp.now - RESET_MS) {
    next = target; // first light, or a restarted tick domain
  } else {
    const dt = disp.wall ? Math.min(nowWall - disp.wall, 100) : 16;
    const step = dt + (target - disp.now) * CATCHUP_GAIN;
    next = Math.min(disp.now + Math.max(0, step), liveEdge.tick + LEAD_MAX_MS);
  }
  const changed = next > disp.now;
  disp.now = next;
  disp.wall = nowWall;
  return changed;
}

/** The window on SCREEN this frame: paused → the frozen range; live → the
 *  display clock's smoothly advancing trailing span. Interaction mapping
 *  (cursor, hit-tests) and the drawn translation share this so they agree
 *  every frame. */
export function displayWindow() {
  const t = tl();
  if (t.mode === "paused" && t.window) return t.window;
  if (!disp.now) return currentWindow();
  const end = Math.max(disp.now, t.span_ms);
  return [end - t.span_ms, end];
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
  // Freeze the DISPLAYED window, not the newest sample: the display clock
  // leads the newest tick by up to LEAD_MAX_MS, and freezing to the sample
  // edge would pop the view left by that lead at the pause press. The
  // frozen span is exactly what was on screen (views_008's span preceding
  // the pause); its right edge honestly holds the ≤ LEAD_MAX_MS strip no
  // samples reached yet.
  const newest = Math.max(globalNewest(), tl().span_ms);
  const end = Math.max(newest, Math.min(disp.now || 0, newest + LEAD_MAX_MS));
  const win = [end - tl().span_ms, end];
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

/** Zoom about a tick — the held cursor time, else the range's center. A step
 *  INTO a bound the range already sits at is a no-op (the width clamp would
 *  otherwise keep re-centering about the anchor, sliding the pinned window —
 *  zoom must never turn into pan at the floor). */
export function zoomAt(center, factor) {
  if (tl().mode !== "paused") return;
  const [a, b] = tl().window;
  const [s0, s1] = tl().pausedSpan;
  const w = b - a;
  const bounded = Math.min(Math.max(w * factor, MIN_RANGE_MS), s1 - s0);
  if (bounded === w && w !== w * factor) return; // already flush at that bound
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
