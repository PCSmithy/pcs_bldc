// Trace decimation: a pixel column holding more than two samples renders
// its min and max samples (real samples, real ticks — a spike survives);
// breaks (gap markers and null-valued samples) split segments, at most one
// per pixel column — each break is one draw call, and a burst of drops
// must not cost thousands. Two entry points with identical outputs:
// envelopeTable() queries the ring via the pyramid (O(pixel columns), the
// hot path); decimateTable() transforms a materialized [xs, ys] pair.
// [impl->app~views_014~1]

/** Ring-native envelope query: SignalHistory -> decimated [xs, ys] over
 *  [t0, t1] at `cols` pixel columns, without materializing the window. */
export function envelopeTable(h, t0, t1, cols) {
  const n = h.size;
  if (!n) return [[], []];
  if (cols < 1 || t1 <= t0) return h.windowTable(t0, t1);
  const colMs = (t1 - t0) / cols;
  const oxs = [];
  const oys = [];
  const { t: tt, start } = h.raw();
  let i = h.indexAtOrAfter(t0);
  const end = h.indexAfter(t1);
  const gaps = h.gaps;
  // Bisect to the first gap that can still matter for this window: a
  // burst-of-drops gap list must not cost a scan per refresh.
  let gi = h.gapIndexFrom(t0);
  // Next null-valued sample at/after i (a cursor, re-sought only when
  // passed — no per-column probe when the null list is empty).
  let bk = h.nextNullIndex(i, end);
  let markCol = -1; // pixel column of the last emitted break
  /** Emit a break at x unless this column already carries one. */
  const pushBreak = (x) => {
    const col = Math.floor((x - t0) / colMs);
    if (col === markCol) return;
    markCol = col;
    oxs.push(x);
    oys.push(null);
  };
  while (i < end) {
    const t = tt[start + i];
    // Inject the gap markers windowTable would have placed: every span
    // closed at or before this sample gets its break, one period past the
    // span's start (only for gaps starting inside the window).
    while (gi < gaps.length && gaps[gi][1] <= t) {
      if (gaps[gi][0] >= t0) pushBreak(gaps[gi][0] + h.period);
      gi++;
    }
    // A span still open here covers this sample — only a coalesced span
    // covers any — and its interior is not rendered (app~views_001).
    if (gi < gaps.length && gaps[gi][0] < t) {
      i = Math.min(h.indexAtOrAfterFrom(gaps[gi][1], i), end);
      continue;
    }
    if (bk !== -1 && bk < i) bk = h.nextNullIndex(i, end);
    // A null-valued sample is a break at its own tick, exactly as the array
    // path treats a stored null.
    if (bk === i) {
      pushBreak(t);
      i++;
      continue;
    }
    // One gap-free segment within one pixel column, bounds found by a
    // galloping search from the previous group's end instead of a scan.
    // The max(i+1, …) is the same ulp guard as the scan path's
    // unconditional first admission. The gap clamp is the first sample past
    // the next span's START — that start sample is the segment's last.
    const colEnd = t0 + (Math.floor((t - t0) / colMs) + 1) * colMs;
    let j = Math.min(h.indexAtOrAfterFrom(colEnd, i), end);
    if (gi < gaps.length) j = Math.min(j, h.indexAfter(gaps[gi][0]));
    if (bk >= 0) j = Math.min(j, bk);
    j = Math.max(j, i + 1);
    if (j - i <= 2) {
      for (let k = i; k < j; k++) {
        oxs.push(h.tickAtIndex(k));
        oys.push(h.valueAtIndex(k));
      }
    } else {
      const { amin, amax } = h.rangeMinMax(i, j);
      if (amin >= 0) {
        const a = Math.min(amin, amax);
        const b = Math.max(amin, amax);
        oxs.push(h.tickAtIndex(a));
        oys.push(h.valueAtIndex(a));
        if (b !== a) {
          oxs.push(h.tickAtIndex(b));
          oys.push(h.valueAtIndex(b));
        }
      }
      // else: a dense span of only non-finite values carries no extent.
    }
    i = j;
  }
  return [oxs, oys];
}

/** [xs, ys] -> decimated [xs, ys]; `cols` pixel columns across [t0, t1]. */
export function decimateTable(xs, ys, t0, t1, cols) {
  const n = xs.length;
  if (!n || cols < 1 || t1 <= t0) return [xs, ys];
  const colMs = (t1 - t0) / cols;
  const oxs = [];
  const oys = [];
  let i = 0;
  let markCol = -1; // pixel column of the last emitted break
  while (i < n) {
    if (ys[i] === null) {
      const col = Math.floor((xs[i] - t0) / colMs);
      if (col !== markCol) {
        markCol = col;
        oxs.push(xs[i]);
        oys.push(null);
      }
      i++;
      continue;
    }
    // One gap-free segment within one pixel column. The first sample is
    // admitted unconditionally (j === i): with a fractional t0 (paused
    // zoom windows come from pixel math) colEnd can land exactly on —
    // or one ulp below — xs[i], and an empty column here would loop
    // forever without the guaranteed step.
    const colEnd = t0 + (Math.floor((xs[i] - t0) / colMs) + 1) * colMs;
    let j = i;
    // Min/max range over the segment's FINITE samples only: a NaN seeded
    // here would poison every comparison and the column would render the
    // NaN instead of the real extent. -1 = no finite sample seen yet.
    let minI = -1;
    let maxI = -1;
    while (j < n && ys[j] !== null && (j === i || xs[j] < colEnd)) {
      if (Number.isFinite(ys[j])) {
        if (minI < 0 || ys[j] < ys[minI]) minI = j;
        if (maxI < 0 || ys[j] > ys[maxI]) maxI = j;
      }
      j++;
    }
    if (j - i <= 2) {
      // Sparse column: samples pass through verbatim (non-finite included,
      // matching the raw-samples contract).
      for (let k = i; k < j; k++) {
        oxs.push(xs[k]);
        oys.push(ys[k]);
      }
    } else if (minI >= 0) {
      const a = Math.min(minI, maxI);
      const b = Math.max(minI, maxI);
      oxs.push(xs[a]);
      oys.push(ys[a]);
      if (b !== a) {
        oxs.push(xs[b]);
        oys.push(ys[b]);
      }
    }
    // else: a dense column of only non-finite values carries no extent —
    // nothing to render there.
    i = j;
  }
  return [oxs, oys];
}
