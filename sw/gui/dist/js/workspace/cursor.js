// The shared cursor: AT MOST one app-level cursor time. Pointing at any
// plot sets it; leaving the plots clears it. Every plot marks a held time
// and reads its own signals at it; the table follows the same time.
// [impl->app~views_005~1]

import { notify } from "../state.js";

export const cursor = { tick: null };

export function setCursorTick(tick) {
  if (cursor.tick === tick) return;
  cursor.tick = tick;
  notify("cursor", tick);
}

export function clearCursor() {
  if (cursor.tick === null) return;
  cursor.tick = null;
  notify("cursor", null);
}
