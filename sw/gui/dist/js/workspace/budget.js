// Local budget preview — the fw~conn_trace_002 admission formulas, computed
// BEFORE any request so the meters answer while the user edits:
//   u = 4 + Σ size_i                      vs ram_budget_bytes
//   r = Σ(size_i · 1000/period_i) + W·max(1000/period_i)  vs link budget
// W and the 32-watch capacity mirror the board's spec'd constants.

export const WIRE_OVERHEAD_W = 21;
export const WATCH_CAPACITY = 32;

/** entries: [{ size, period_ms }] → { u, r, count } */
export function preview(entries) {
  let sum = 0, rate = 0, maxF = 0;
  for (const e of entries) {
    const f = 1000 / e.period_ms;
    sum += e.size;
    rate += e.size * f;
    if (f > maxF) maxF = f;
  }
  return {
    u: entries.length ? 4 + sum : 0,
    r: entries.length ? rate + WIRE_OVERHEAD_W * maxF : 0,
    count: entries.length,
  };
}
