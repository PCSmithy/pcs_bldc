// Per-signal trace appearance (app~views_011): color override, line style,
// sample dots, interpolation. Appearance is GLOBAL per signal — one visual
// identity everywhere it renders — set from any plot widget's configuration
// menu and persisted with the layout.
//
// Color rule (the theme observer in index.js applies it): an un-overridden
// signal takes its slot's --trace-N token and RETINTS on theme switch; a
// user-overridden color is an absolute value and survives theme switches
// exactly as chosen.

import { notify } from "../state.js";
import { traceColor, traceDashed } from "./colors.js";

// style null = auto: solid, except the 6-color overflow rule dashes. Any
// EXPLICIT style — "solid" included — beats the overflow rule; storing the
// default as null is what keeps "user chose solid" distinguishable from
// "never touched" through the override trim below.
const DEFAULTS = { color: null, style: null, dots: false, interp: "linear" };

const overrides = new Map(); // path -> the non-default subset only

export function appearanceOf(path) {
  return { ...DEFAULTS, ...(overrides.get(path) || {}) };
}

export function setAppearance(path, patch) {
  const next = { ...appearanceOf(path), ...patch };
  const trimmed = {};
  for (const key of Object.keys(DEFAULTS)) {
    if (next[key] !== DEFAULTS[key]) trimmed[key] = next[key];
  }
  if (Object.keys(trimmed).length) overrides.set(path, trimmed);
  else overrides.delete(path);
  notify("appearance", path);
}

/** The signal's rendered color: the override, else its cycle slot's token. */
export function resolvedColor(path) {
  return appearanceOf(path).color || traceColor(path);
}

/** The style as rendered: the explicit choice, else the auto rule. */
export function effectiveStyle(path) {
  return appearanceOf(path).style ?? (traceDashed(path) ? "dashed" : "solid");
}

// An unwatched signal keeps its appearance override (identity, like the
// name); only the auto color slot is recycled (colors.releaseColor).
export function appearanceEntries() {
  return [...overrides.entries()];
}

export function restoreAppearance(entries) {
  overrides.clear();
  for (const [path, a] of entries || []) overrides.set(path, a);
}
