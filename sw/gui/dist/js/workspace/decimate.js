// Trace decimation: bound each trace's rendered points per pixel column of
// the plotted X range — a column holding more than two samples renders that
// column's minimum and maximum samples (real samples, real ticks — a
// single-sample spike survives), a column holding at most two renders its
// samples raw. Gap markers (null values injected by windowTable) pass
// through untouched and split the column's segments, so decimation never
// selects an extreme across a tick-count gap and the line still breaks.
// [impl->app~views_014~1]
//
// Two entry points share these semantics: envelopeTable() queries a
// SignalHistory ring directly (binary-searched column bounds + the min/max
// pyramid — O(pixel columns), the live-refresh hot path), and
// decimateTable() transforms an already-materialized [xs, ys] pair (the
// fallback path and the in-page spec checks). Their outputs are identical
// for the same window; the suite pins envelopeTable via the rendered-path
// checks and decimateTable via direct import.

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
  let gi = 0;
  // Next null-valued sample at/after i (a cursor, re-sought only when
  // passed — no per-column probe when the null list is empty).
  let bk = h.nextNullIndex(i, end);
  while (i < end) {
    const t = tt[start + i];
    // Inject the gap markers windowTable would have placed before this
    // sample (marker at from+period, only for gaps starting inside the
    // window) — same output positions, same conditions.
    while (gi < gaps.length && gaps[gi][1] <= t) {
      if (gaps[gi][0] >= t0) {
        oxs.push(gaps[gi][0] + h.period);
        oys.push(null);
      }
      gi++;
    }
    if (bk !== -1 && bk < i) bk = h.nextNullIndex(i, end);
    // A null-valued sample is its own emitted element and a segment
    // break, exactly as the array path treats a stored null.
    if (bk === i) {
      oxs.push(t);
      oys.push(null);
      i++;
      continue;
    }
    // One gap-free segment within one pixel column, bounds found by a
    // galloping search from the previous group's end instead of a scan.
    // The max(i+1, …) is the same ulp guard as the scan path's
    // unconditional first admission. The gap clamp is the index of the
    // first post-gap sample (== first index past the pre-gap segment,
    // since no samples exist inside a gap).
    const colEnd = t0 + (Math.floor((t - t0) / colMs) + 1) * colMs;
    let j = Math.min(h.indexAtOrAfterFrom(colEnd, i), end);
    if (gi < gaps.length) j = Math.min(j, h.indexAtOrAfterFrom(gaps[gi][1], i));
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
  while (i < n) {
    if (ys[i] === null) {
      oxs.push(xs[i]);
      oys.push(null);
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
