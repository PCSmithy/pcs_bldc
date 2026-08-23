// The monotone-cubic math behind the trace interpolation (app~views_011).
// The renderer (glrender.js) tessellates cubic runs from these tangents;
// ZOH and linear are generated directly as step/point vertices there. The
// cubic is Fritsch–Carlson MONOTONE cubic: its limited tangents keep every
// Hermite segment within its endpoints' range, so the curve never
// overshoots the samples.

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

/** Hermite evaluation from the same tangents — the test oracle for the
 *  no-overshoot property (the renderer's tessellation derives from these
 *  tangents). */
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

