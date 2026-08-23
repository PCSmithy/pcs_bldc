// Trace decimation: bound each trace's rendered points per pixel column of
// the plotted X range — a column holding more than two samples renders that
// column's minimum and maximum samples (real samples, real ticks — a
// single-sample spike survives), a column holding at most two renders its
// samples raw. Gap markers (null values injected by windowTable) pass
// through untouched and split the column's segments, so decimation never
// selects an extreme across a tick-count gap and the line still breaks.
// [impl->app~views_014~1]

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
