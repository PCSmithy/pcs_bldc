// Gap-aware uPlot path builders (app~views_011): undefined = alignment hole
// (a slower signal between its own samples — connect through), null = a real
// tick-count gap (hard break). Every interpolation works per gap-free run,
// so no mode ever draws across a gap.
//
// The vendored uPlot min build bundles only the points builder, so the
// stepped (ZOH) and cubic builders live here. The cubic is Fritsch–Carlson
// MONOTONE cubic: its limited tangents keep every Hermite segment within its
// endpoints' range, so the curve never overshoots the samples (uPlot's stock
// spline is Catmull-Rom-flavored and can).

/** Collect the series' gap-free runs as canvas-pixel point lists. */
export function splitRuns(u, sidx, i0, i1) {
  const xs = u.data[0];
  const ys = u.data[sidx];
  const scaleKey = u.series[sidx].scale;
  const runs = [];
  let run = null;
  for (let i = i0; i <= i1; i++) {
    const v = ys[i];
    if (v === undefined) continue;
    if (v === null) {
      run = null;
      continue;
    }
    if (!run) {
      run = { x: [], y: [] };
      runs.push(run);
    }
    run.x.push(u.valToPos(xs[i], "x", true));
    run.y.push(u.valToPos(v, scaleKey, true));
  }
  return runs;
}

export function linearPaths(u, sidx, i0, i1) {
  const path = new Path2D();
  for (const r of splitRuns(u, sidx, i0, i1)) {
    path.moveTo(r.x[0], r.y[0]);
    for (let i = 1; i < r.x.length; i++) path.lineTo(r.x[i], r.y[i]);
  }
  return { stroke: path, fill: null, clip: null };
}

/** Zero-order hold: each value holds as a step until the next sample. */
export function steppedPaths(u, sidx, i0, i1) {
  const path = new Path2D();
  for (const r of splitRuns(u, sidx, i0, i1)) {
    path.moveTo(r.x[0], r.y[0]);
    for (let i = 1; i < r.x.length; i++) {
      path.lineTo(r.x[i], r.y[i - 1]);
      path.lineTo(r.x[i], r.y[i]);
    }
  }
  return { stroke: path, fill: null, clip: null };
}

/** Fritsch–Carlson tangents for one run: the limiter caps α²+β² at 9, which
 *  is what makes each segment monotone (hence overshoot-free). */
export function monotoneTangents(x, y) {
  const n = x.length;
  if (n === 1) return [0];
  const d = [];
  const m = new Array(n);
  for (let i = 0; i < n - 1; i++) d.push((y[i + 1] - y[i]) / (x[i + 1] - x[i] || 1));
  m[0] = d[0];
  m[n - 1] = d[n - 2];
  for (let i = 1; i < n - 1; i++) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
  for (let i = 0; i < n - 1; i++) {
    if (d[i] === 0) {
      m[i] = 0;
      m[i + 1] = 0;
      continue;
    }
    const a = m[i] / d[i];
    const b = m[i + 1] / d[i];
    const s = a * a + b * b;
    if (s > 9) {
      const t = 3 / Math.sqrt(s);
      m[i] = t * a * d[i];
      m[i + 1] = t * b * d[i];
    }
  }
  return m;
}

export function monotonePaths(u, sidx, i0, i1) {
  const path = new Path2D();
  for (const r of splitRuns(u, sidx, i0, i1)) {
    const { x, y } = r;
    path.moveTo(x[0], y[0]);
    if (x.length === 1) continue;
    const m = monotoneTangents(x, y);
    for (let i = 0; i < x.length - 1; i++) {
      const h = x[i + 1] - x[i];
      path.bezierCurveTo(
        x[i] + h / 3,
        y[i] + (m[i] * h) / 3,
        x[i + 1] - h / 3,
        y[i + 1] - (m[i + 1] * h) / 3,
        x[i + 1],
        y[i + 1],
      );
    }
  }
  return { stroke: path, fill: null, clip: null };
}

/** Hermite evaluation from the same tangents — the test oracle for the
 *  no-overshoot property (the path's beziers derive from these tangents). */
export function evalMonotoneRun(x, y, sx) {
  const m = monotoneTangents(x, y);
  let i = 0;
  while (i < x.length - 2 && sx > x[i + 1]) i++;
  const h = x[i + 1] - x[i];
  const t = (sx - x[i]) / h;
  const t2 = t * t;
  const t3 = t2 * t;
  return (
    (2 * t3 - 3 * t2 + 1) * y[i] +
    (t3 - 2 * t2 + t) * h * m[i] +
    (-2 * t3 + 3 * t2) * y[i + 1] +
    (t3 - t2) * h * m[i + 1]
  );
}

export const PATH_BUILDERS = { linear: linearPaths, zoh: steppedPaths, cubic: monotonePaths };
