// Capture export: the samples of every watched signal over the range on
// screen — paused, the held range (a drag-selected one included); live,
// the visible span — as a comma-separated file the operator names: a
// header of time_ms and each path, one row per distinct sample time, a
// cell empty where that signal has no sample at that time.
// [impl->app~views_028~1]

import { store } from "../state.js";
import { saveFile, invoke } from "../bridge.js";
import { histories } from "./history.js";
import { meta } from "./watchflow.js";
import { displayWindow } from "./timeline.js";

/** A sample's cell: f32 at its own precision, a non-finite reading as NaN. */
function cell(v, kind) {
  if (v === null) return "NaN";
  return kind === "f32" ? String(Number(v.toPrecision(9))) : String(v);
}

/** The CSV text for `paths` over [t0, t1]: a merge of the histories'
 *  time-ordered slices, one row per distinct tick. */
export function buildCsv(paths, [t0, t1]) {
  const cursors = paths.map((path) => {
    const h = histories.get(path);
    return { h, i: h ? h.indexAtOrAfter(t0) : 0, kind: meta.get(path)?.kind };
  });
  const tickOf = (c) => (c.h && c.i < c.h.size ? c.h.tickAtIndex(c.i) : Infinity);
  const lines = [["time_ms", ...paths].join(",")];
  for (;;) {
    const tick = Math.min(...cursors.map(tickOf));
    if (tick > t1) break;
    const cells = cursors.map((c) => {
      if (tickOf(c) !== tick) return "";
      return cell(c.h.valueAtIndex(c.i++), c.kind);
    });
    lines.push([String(tick), ...cells].join(","));
  }
  return lines.join("\n") + "\n";
}

let exporting = false;

/** Export: the save dialog names the file; a cancelled dialog writes nothing. */
export async function exportCapture() {
  const paths = [...store.watched.keys()];
  if (!paths.length || exporting) return null;
  exporting = true;
  try {
    const range = displayWindow();
    const path = await saveFile("capture.csv", [{ name: "CSV", extensions: ["csv"] }]);
    if (!path) return null;
    await invoke("write_text_file", { path, text: buildCsv(paths, range) });
    return path;
  } finally {
    exporting = false;
  }
}
