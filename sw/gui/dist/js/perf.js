// Render-loop performance monitor (header chrome readout; unspecced).
// A permanent rAF loop timestamps real frames, so the readout reports
// what the user's eye gets, not a timer's opinion. The samples-batch
// handler reports its main-thread cost through markBatch(); both feed
// the strip's "render" cell.

const FPS_WINDOW_MS = 1000;   // rolling FPS window
const WORST_WINDOW_MS = 3000; // worst-frame / slow-frame lookback
const SLOW_FRAME_MS = 32;     // two missed 60 Hz frames
// A delta above this is a discontinuity (hidden/occluded webview — rAF
// stops entirely), not a slow frame; it is discarded, never reported.
const DISCONTINUITY_MS = 1000;

const frames = [];  // rAF timestamps (ms, performance.now domain)
const batches = []; // [t, durMs] per samples batch
let draws = 0;      // cumulative renderer draws (glrender reports each one)
let scrolls = 0;    // cumulative live-scroll redraws (cached geometry)
let rafId = null;

function loop(now) {
  frames.push(now);
  const cut = now - WORST_WINDOW_MS;
  while (frames.length && frames[0] < cut) frames.shift();
  rafId = requestAnimationFrame(loop);
}

export function initPerf() {
  if (rafId === null) rafId = requestAnimationFrame(loop);
  // A hidden webview stops rAF; the first delta after refocus would span
  // the whole hidden interval and read as a monster "worst" frame. Drop
  // the history at both edges of the visibility change.
  document.addEventListener("visibilitychange", () => {
    frames.length = 0;
  });
  setInterval(renderCell, 500);
}

/** Record one trace-renderer draw (the snapshot's `draws` — lets a check
 *  tell "frames ticked" apart from "traces actually drew"). */
export function markDraw() {
  draws++;
}

/** Record one live-scroll redraw (cached geometry at a new translation).
 *  Counted apart from `draws` so the views_015 draws-per-batch check keeps
 *  meaning "geometry rebuilt and drew" — a scroll is a visual update, not
 *  fresh data. */
export function markScroll() {
  scrolls++;
}

/** Record one samples-batch handler run of `ms` main-thread milliseconds. */
export function markBatch(ms) {
  const now = performance.now();
  batches.push([now, ms]);
  while (batches.length && batches[0][0] < now - WORST_WINDOW_MS) batches.shift();
}

export function perfSnapshot() {
  const now = performance.now();
  let fps = 0;
  let worst = 0;
  let slow = 0;
  let prev = null;
  for (const t of frames) {
    if (prev !== null) {
      const d = t - prev;
      if (d < DISCONTINUITY_MS) {
        if (d > worst) worst = d;
        if (d > SLOW_FRAME_MS) slow++;
      }
      if (t >= now - FPS_WINDOW_MS) fps++;
    }
    prev = t;
  }
  let batchAvg = 0;
  let batchMax = 0;
  if (batches.length) {
    for (const [, ms] of batches) {
      batchAvg += ms;
      if (ms > batchMax) batchMax = ms;
    }
    batchAvg /= batches.length;
  }
  return { fps, worst_ms: worst, slow_frames: slow, batch_avg_ms: batchAvg, batch_max_ms: batchMax, draws, scrolls };
}

/** The strip cell's machine half (chrome.js owns the cell markup; this
 *  fills it in place between telemetry rebuilds). */
export function perfCellText(s = perfSnapshot()) {
  if (!s.fps) return "—";
  return `${s.fps} fps · worst ${s.worst_ms.toFixed(0)} ms`;
}

function renderCell() {
  const el = document.querySelector("[data-perf-cell]");
  if (!el) return;
  const s = perfSnapshot();
  el.textContent = perfCellText(s);
  el.title = `slow frames (3 s): ${s.slow_frames} · batch avg ${s.batch_avg_ms.toFixed(1)} ms · max ${s.batch_max_ms.toFixed(1)} ms`;
}

/** A trailing-edge throttle: at most one `fn` per `ms`, a pending trailing
 *  call landing the newest state. `touch()` re-arms the window (a direct
 *  render elsewhere counts as a run); `cancel()` drops a pending trail on
 *  owner teardown. */
export function throttleTrailing(fn, ms = 100, trailMs = 110) {
  let last = 0;
  let trail = null;
  const run = () => {
    if (performance.now() - last < ms) {
      if (!trail) trail = setTimeout(() => { trail = null; run(); }, trailMs);
      return;
    }
    last = performance.now();
    fn();
  };
  run.touch = () => { last = performance.now(); };
  run.cancel = () => { if (trail) { clearTimeout(trail); trail = null; } };
  return run;
}
