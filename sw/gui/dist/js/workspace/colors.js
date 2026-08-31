// The six-color trace cycle: assigned in add order, remembered per signal
// (persisted with the layout), dashed strokes beyond six instead of new hues.

const CYCLE = 6;
const assigned = new Map(); // path -> slot index (0-based, may exceed CYCLE)

export function restoreColors(entries) {
  assigned.clear();
  for (const [path, slot] of entries || []) assigned.set(path, slot);
}

export function colorSlots() {
  return [...assigned.entries()];
}

export function traceSlot(path) {
  if (!assigned.has(path)) {
    const used = new Set(assigned.values());
    let slot = 0;
    while (used.has(slot)) slot++;
    assigned.set(path, slot);
  }
  return assigned.get(path);
}

export function traceColor(path) {
  const slot = traceSlot(path);
  return getComputedStyle(document.documentElement)
    .getPropertyValue(`--trace-${(slot % CYCLE) + 1}`)
    .trim();
}

export function traceDashed(path) {
  return traceSlot(path) >= CYCLE;
}

export function releaseColor(path) {
  assigned.delete(path);
}
