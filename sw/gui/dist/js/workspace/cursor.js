// The shared cursor: AT MOST one app-level cursor time. Pointing at any
// plot sets it; leaving the plots clears it. Every plot marks a held time
// and reads its own signals at it; the table follows the same time.
// [impl->app~views_005~1]

import { notify } from "../state.js";

export const cursor = { tick: null };

// Leading-edge sync + trailing rAF coalesce: the first change notifies
// immediately; same-frame repeats fold into one follow-up notify.
let rafId = null;
let notifiedTick = null;

function emitCursor() {
  if (cursor.tick !== notifiedTick) {
    notifiedTick = cursor.tick;
    notify("cursor", cursor.tick);
    rafId = requestAnimationFrame(() => {
      rafId = null;
      emitCursor();
    });
  } else {
    rafId = null;
  }
}

export function setCursorTick(tick) {
  if (cursor.tick === tick) return;
  cursor.tick = tick;
  if (!rafId) emitCursor();
}

export function clearCursor() {
  if (cursor.tick === null) return;
  cursor.tick = null;
  if (!rafId) emitCursor();
}
