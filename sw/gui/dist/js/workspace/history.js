// Per-signal sample history: the plot/table/cursor read side of the
// "samples" batches. Ticks are ms; a signal's samples land only at
// multiples of its period, so an exact-tick lookup either hits or the
// sample is absent — never nearest-neighbor across a tick-count gap.

import { store } from "../state.js";

/** First index with xs[i] >= t (xs ascending). */
export function lowerBound(xs, t) {
  let lo = 0, hi = xs.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (xs[mid] < t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

const CAP_MS = 120_000; // live retention horizon per signal

// While paused, the frozen span is sacred: nothing at or after the paused
// window's start is trimmed, no matter how long the pause holds. Appends
// continue for up to this much stream time past the pause (so Resume lands
// on continuous data), then stop — the session keeps draining the wire and
// only history skips the samples, so the resume-time tick jump renders as
// an honest gap through the existing gap machinery. Bounded so a marathon
// pause cannot grow memory without limit (max retained while paused:
// span + PAUSE_CATCHUP_MS of samples).
const PAUSE_CATCHUP_MS = 120_000;

// Trimming the horizon by splicing the array head is a memmove of the whole
// retained history (120 k samples at 1 ms) — done per 50 ms batch it was a
// steady tax. The horizon still bounds what readers SEE (windowTable starts
// at a binary-searched index), so physical trimming only needs to be
// occasional: let this many stale samples pool, then splice once.
const TRIM_SLACK = 4096;

export class SignalHistory {
  constructor(period_ms) {
    this.period = period_ms;
    this.ticks = [];
    this.values = [];
    // Detected gaps as [fromTickExclusive, toTickExclusive] spans, capped
    // with the same horizon as the samples.
    this.gaps = [];
  }

  append(points) {
    const tl = store.timeline;
    const paused = tl?.mode === "paused" && tl.pausedSpan;
    const appendCutoff = paused ? tl.pausedSpan[1] + PAUSE_CATCHUP_MS : Infinity;
    for (const [tick, value] of points) {
      if (tick > appendCutoff) continue; // past the paused catch-up cap
      const last = this.ticks.length ? this.ticks[this.ticks.length - 1] : null;
      if (last !== null && tick <= last) continue; // stale/duplicate batch tail
      if (last !== null && tick - last > this.period) this.gaps.push([last, tick]);
      this.ticks.push(tick);
      this.values.push(value);
    }
    const newest = this.ticks[this.ticks.length - 1] ?? 0;
    let horizon = newest - CAP_MS;
    if (paused) horizon = Math.min(horizon, tl.pausedSpan[0]);
    const drop = this.indexAtOrAfter(horizon);
    if (drop > TRIM_SLACK) {
      this.ticks.splice(0, drop);
      this.values.splice(0, drop);
    }
    while (this.gaps.length && this.gaps[0][1] < horizon) this.gaps.shift();
  }

  /** First index with ticks[i] >= t. */
  indexAtOrAfter(t) {
    return lowerBound(this.ticks, t);
  }

  newestTick() {
    return this.ticks.length ? this.ticks[this.ticks.length - 1] : null;
  }

  latest() {
    return this.values.length ? this.values[this.values.length - 1] : null;
  }

  /** The sample at-or-before `tick`, accepted only within one period — a
   *  tick-count gap reads as absent, never as a stale bridged number. */
  valueNear(tick) {
    const t = this.tickAtOrBefore(tick);
    if (t === null || tick - t >= this.period) return null;
    return this.valueAt(t);
  }

  /** Exact-tick lookup; null when the tick has no sample (gap or off-phase). */
  valueAt(tick) {
    let lo = 0, hi = this.ticks.length - 1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      const t = this.ticks[mid];
      if (t === tick) return this.values[mid];
      if (t < tick) lo = mid + 1;
      else hi = mid - 1;
    }
    return null;
  }

  /** Nearest sample tick at or below `tick` (for snapping the cursor to the
   *  signal's own grid); null when history is empty or tick precedes it. */
  tickAtOrBefore(tick) {
    let lo = 0, hi = this.ticks.length - 1, best = null;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (this.ticks[mid] <= tick) { best = this.ticks[mid]; lo = mid + 1; }
      else hi = mid - 1;
    }
    return best;
  }

  /** [ticks, values] within [t0, t1], with an explicit null sample injected
   *  after each real gap so the plot breaks the line there (the renderer
   *  splits a trace into runs at null markers). */
  windowTable(t0, t1) {
    const xs = [], ys = [];
    let gi = 0;
    for (let i = this.indexAtOrAfter(t0); i < this.ticks.length; i++) {
      const t = this.ticks[i];
      if (t > t1) break;
      while (gi < this.gaps.length && this.gaps[gi][1] <= t) {
        const [from] = this.gaps[gi];
        if (from >= t0) { xs.push(from + this.period); ys.push(null); }
        gi++;
      }
      xs.push(t);
      ys.push(this.values[i]);
    }
    return [xs, ys];
  }

  /** Gap spans clipped to [t0, t1]. */
  gapsIn(t0, t1) {
    return this.gaps
      .filter(([a, b]) => b > t0 && a < t1)
      .map(([a, b]) => [Math.max(a, t0), Math.min(b, t1)]);
  }
}

/** The app-wide history set, keyed by signal path. */
export const histories = new Map();

export function historyFor(path, period_ms) {
  let h = histories.get(path);
  if (!h || h.period !== period_ms) {
    h = new SignalHistory(period_ms);
    histories.set(path, h);
  }
  return h;
}
