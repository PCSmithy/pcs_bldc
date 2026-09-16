// The watch-period vocabulary and the local budget preview — the
// fw~conn_trace_002 admission formulas, computed BEFORE any request so the
// meters answer while the user edits. Entries group by period c (cycles):
//   S_g = Σ size, f_g = 20000/c, n_g = max(1, 20/c)
//   u = Σ n_g·(4 + S_g)                          vs ram_budget_bytes_per_ms
//   r = Σ (S_g·f_g + W·min(f_g, max(1000, ⌊f_g·S_g/256⌋))) vs the link budget
// W, the 32-watch capacity, and the legal periods mirror the board's spec'd
// constants.

export const WIRE_OVERHEAD_W = 27;
export const WATCH_CAPACITY = 32;

const CYCLES_PER_S = 20_000;
const CYCLES_PER_MS = 20;
const SAMPLES_DATA_CAPACITY = 256;

/** The legal watch periods in PWM cycles: one cycle, 1 ms, 10 ms. */
export const PERIOD_CYCLES = [1, 20, 200];

/** The period as the user reads it — a rate at one cycle, a time otherwise. */
export function periodLabel(period_cycles) {
  return period_cycles === 1 ? "20 kHz" : `${period_cycles / CYCLES_PER_MS} ms`;
}

/** entries: [{ size, period_cycles }] → { u, r, count } */
export function preview(entries) {
  const groups = new Map(); // period_cycles -> Σ size
  for (const e of entries) {
    groups.set(e.period_cycles, (groups.get(e.period_cycles) || 0) + e.size);
  }
  let u = 0, r = 0;
  for (const [c, size] of groups) {
    const f = CYCLES_PER_S / c;
    u += Math.max(1, CYCLES_PER_MS / c) * (4 + size);
    // Integer message count, as the device computes it — a fractional term
    // would preview a link rate above the board's own TraceStatus.
    r += size * f + WIRE_OVERHEAD_W * Math.min(f, Math.max(1000, Math.floor((f * size) / SAMPLES_DATA_CAPACITY)));
  }
  return { u, r, count: entries.length };
}
