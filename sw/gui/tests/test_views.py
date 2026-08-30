"""Playwright suite for the cockpit's views specs, run against dist/ over
file:// with the browser devmock (no board, no Tauri).

Run:  .venv/Scripts/python sw/gui/tests/test_views.py
"""

import os
import sys
import threading
from functools import partial
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

from playwright.sync_api import sync_playwright

# ES modules are CORS-blocked over file://, so serve dist/ on localhost.
DIST_DIR = Path(__file__).resolve().parent.parent / "dist"
_server = HTTPServer(
    ("127.0.0.1", 0),
    partial(SimpleHTTPRequestHandler, directory=str(DIST_DIR)),
)
threading.Thread(target=_server.serve_forever, daemon=True).start()
URL = f"http://127.0.0.1:{_server.server_port}/index.html?state=matched"

FAILURES = []


def check(name, cond, detail=""):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {name}{'' if cond else '  — ' + str(detail)}")
    if not cond:
        FAILURES.append(name)


def boot(page):
    page.goto(URL)
    page.wait_for_function("() => window.__cockpit !== undefined")
    # Load the (mock) ELF so the gate opens, then pull the signal list.
    page.evaluate("() => __cockpit.api.loadElf('mock.elf')")
    page.evaluate("() => __cockpit.api.listSignals('')")
    page.wait_for_function("() => __cockpit.store.gate === 'matched'")


def add_plot_with(page, paths, period=1):
    for p in paths:
        page.evaluate(f"() => __cockpit.addWatch({p!r}, {period})")
    page.evaluate(
        "(paths) => { const w = __cockpit.addWidget({ type: 'plot', signals: [] });"
        "  for (const p of paths) w.addSignal(p); return w.cfg.id; }",
        paths,
    )
    # Commit is debounced 600 ms; then the mock streams 50 ms batches.
    page.wait_for_function(
        "() => __cockpit.store.traceStatus && __cockpit.store.traceStatus.link_rate_bytes_per_s > 0",
        timeout=5000,
    )


def wait_for_samples(page, path, min_points=50):
    page.wait_for_function(
        f"() => (__cockpit.histories.get({path!r})?.ticks.length || 0) >= {min_points}",
        timeout=8000,
    )


def fresh_boot(page):
    """Wipe persistence, then a full boot — each bench batch starts clean."""
    page.evaluate("() => localStorage.clear()")
    boot(page)


def restart(page, load_elf=False, wait_js=None, timeout=5000):
    """Reload and wait for the app shell; optionally reload the (mock) ELF
    and wait for a page condition (widget counts, the gate) to hold."""
    page.reload()
    page.wait_for_function("() => window.__cockpit !== undefined")
    if load_elf:
        page.evaluate("() => __cockpit.api.loadElf('mock.elf')")
    if wait_js:
        page.wait_for_function(wait_js, timeout=timeout)


def widget_eval(page, wid, expr, arg=None):
    """Evaluate `expr` as (w, arg) => ... against the widget with id `wid`."""
    return page.evaluate(
        "([id, arg]) => { let w = null; __cockpit.forEachWidget(x => { if (x.cfg.id === id) w = x; });"
        f" return ({expr})(w, arg); }}",
        [wid, arg],
    )


# Nearest sample-y of `p` around window-time `t` (px space) — the shared
# probe idiom for placing real-mouse targets on a rendered trace.
NEAREST_Y_JS = (
    "const nearestY = (w, p, t, rect) => {"
    " const [txs, tys] = w.renderedTables().get(p);"
    " let bi = -1, bd = 1e18;"
    " for (let i = 0; i < txs.length; i++) {"
    "   if (tys[i] == null) continue;"
    "   const d = Math.abs(txs[i] - t);"
    "   if (d < bd) { bd = d; bi = i; }"
    " }"
    " return bi < 0 ? null : w.yPxOf(tys[bi], w.sideOf(p), rect.height);"
    " };"
)


def run(page):
    print("suite: views")

    boot(page)

    # ── [test->app~views_002~1] telemetry cell shows the latest value ──
    page.wait_for_function("() => __cockpit.store.telemetry !== null")
    vbus = page.locator(".telemetry-strip").inner_text()
    check("views_002 telemetry renders vbus", "19.8" in vbus, vbus[:120])

    # ── [test->app~views_003~1] log lines render in order ──
    page.wait_for_function(
        "() => document.querySelectorAll('.log-lines > div').length >= 2", timeout=8000
    )
    lines = page.eval_on_selector_all(".log-lines > div", "els => els.map(e => e.textContent)")
    check("views_003 log lines in order", "heartbeat" in "".join(lines), lines[:2])

    # ── [test->app~views_004~1] drop-on-empty creates a plot widget ──
    page.evaluate(
        """() => {
          const dt = new DataTransfer();
          dt.setData('text/x-signal', 'task1msRuns');
          const ws = document.querySelector('.workspace');
          ws.dispatchEvent(new DragEvent('dragover', { dataTransfer: dt, bubbles: true }));
          ws.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true,
            clientX: ws.getBoundingClientRect().left + 200,
            clientY: ws.getBoundingClientRect().top + 100 }));
        }"""
    )
    check(
        "views_004 drop creates plot widget",
        page.locator(".plot-widget").count() == 1
        and page.evaluate("() => __cockpit.store.watched.has('task1msRuns')"),
    )
    # The first drop was dispatched at workspace-relative (200, 100); the
    # RENDERED top-left must sit at that point snapped into CANVAS
    # coordinates (the canvas origin sits below the timeline bar that
    # appears with it).
    placed = page.evaluate(
        """() => { const g = document.querySelector('.widget-grid');
          const gr = g.getBoundingClientRect();
          const ws = document.querySelector('.workspace').getBoundingClientRect();
          const wr = document.querySelector('.widget').getBoundingClientRect();
          const snap = (v) => Math.round(v / 50) * 50;
          let c = null; __cockpit.forEachWidget(w => c = w.cfg);
          return {
            rendered: { x: wr.left - gr.left + g.scrollLeft, y: wr.top - gr.top + g.scrollTop },
            expected: { x: snap(200 - (gr.left - ws.left)), y: snap(100 - (gr.top - ws.top)) },
            cfg: { x: c.x, y: c.y, w: c.w, h: c.h } }; }"""
    )
    check(
        "views_004 drop lands the widget at the snapped drop point",
        placed["rendered"] == placed["expected"]
        and placed["cfg"]["x"] == placed["expected"]["x"]
        and placed["cfg"]["y"] == placed["expected"]["y"]
        and all(v % 50 == 0 for v in placed["cfg"].values()),
        placed,
    )

    # Two plots for the shared-cursor assertions.
    add_plot_with(page, ["app_motorControl_data.channels[0].velocityMeasured_radPerSec"], 1)
    wait_for_samples(page, "task1msRuns")
    wait_for_samples(page, "app_motorControl_data.channels[0].velocityMeasured_radPerSec")

    # ── [test->app~views_001~1] traces render; a tick-count gap breaks them ──
    plot_ready = page.evaluate(
        "() => { let n = 0; __cockpit.forEachWidget(w => {"
        "  if (w.renderedTables && [...w.renderedTables().values()].some(([xs]) => xs.length > 40)) n++; });"
        " return n; }"
    )
    check("views_001 both plots hold trace data", plot_ready == 2, plot_ready)
    # Wait past a mock gap window (every 5 s), then assert: an explicit null
    # marker in the plot data (the line break) AND an accent gap-ribbon span.
    page.wait_for_function(
        "() => __cockpit.histories.get('task1msRuns')?.gaps.length >= 1", timeout=9000
    )
    has_null = page.evaluate(
        """() => { let found = false, drew = true;
          __cockpit.forEachWidget(w => { if (!w.renderedTables) return;
            if (!(w.gl && w.gl.drawCount > 0)) drew = false;
            for (const [, ys] of w.renderedTables().values())
              if (ys.some(v => v === null)) found = true; });
          return { found, drew }; }"""
    )
    check(
        "views_001 gap yields a broken trace (explicit null, actually drawn)",
        has_null["found"] and has_null["drew"],
        has_null,
    )
    check("views_001 gap ribbon span shown", page.locator(".gap-span").count() >= 1)

    # ── [test->app~views_005~1] one shared cursor across every plot ──
    newest = page.evaluate("() => __cockpit.histories.get('task1msRuns').newestTick()")
    cursor_tick = newest - 1000
    page.evaluate(f"() => __cockpit.setCursorTick({cursor_tick})")
    visible_lines = page.eval_on_selector_all(
        ".plot-widget .cursor-line", "els => els.filter(e => !e.hidden).length"
    )
    check("views_005 cursor line on every plot", visible_lines == 2, visible_lines)
    readout = page.locator(".plot-widget .cursor-readout").first.inner_text()
    check("views_005 readout carries the time", f"{cursor_tick:,}".replace(",", " ") in readout, readout[:80])
    # An in-gap tick reads "no sample" (mock gap: t % 5000 in [4880, 5000)).
    gap_tick = (newest // 5000) * 5000 - 60
    page.evaluate(f"() => __cockpit.setCursorTick({gap_tick})")
    readout = page.locator(".plot-widget .cursor-readout").first.inner_text()
    check("views_005 in-gap tick reads 'no sample'", "no sample" in readout, readout[:80])
    # Pointer-leave clears every mark.
    page.evaluate("() => __cockpit.clearCursor()")
    visible_lines = page.eval_on_selector_all(
        ".plot-widget .cursor-line", "els => els.filter(e => !e.hidden).length"
    )
    check("views_005 leaving clears every mark", visible_lines == 0, visible_lines)

    # ── [test->app~views_006~1] table latest vs at-cursor + formatting ──
    page.evaluate(
        """() => { const t = __cockpit.addWidget({ type: 'table', signals: [] });
          t.addSignal('task1msRuns');
          __cockpit.addWatch('app_motorControl_data.channels[0].faultLatched', 10);
          t.addSignal('app_motorControl_data.channels[0].faultLatched'); }"""
    )
    wait_for_samples(page, "app_motorControl_data.channels[0].faultLatched", 5)
    page.evaluate("() => __cockpit.forEachWidget(w => w.refresh())")
    body = page.locator(".value-table").inner_text()
    check("views_006 integer renders whole", "." not in body.split("task1msRuns")[1].split("\n")[0], body[:120])
    check("views_006 bool renders true/false", ("true" in body) or ("false" in body), body[:160])
    # The rendered value may lag latest() by one rAF batch on a fast stream,
    # so assert it is a *recent* history value, read in the same JS turn.
    live = page.evaluate(
        """() => { const row = [...document.querySelectorAll('.value-table tr')]
             .find(r => r.textContent.includes('task1msRuns'));
           const m = row && row.textContent.match(/task1msRuns\\s*(\\d+)/);
           const shown = m ? parseInt(m[1], 10) : NaN;
           const recent = __cockpit.histories.get('task1msRuns').values.slice(-40);
           return { shown, ok: recent.includes(shown) }; }"""
    )
    check("views_006 latest value live", live["ok"], live["shown"])
    page.evaluate(f"() => __cockpit.setCursorTick({cursor_tick})")
    at_cursor = page.locator(".table-widget .table-mode-tag").inner_text()
    check("views_006 at-cursor mode tag", "at cursor" in at_cursor, at_cursor)
    page.evaluate("() => __cockpit.clearCursor()")

    # ── [test->app~views_007~1] axes configuration: per-signal L/R assignment
    #    renders one vs two labeled axes; auto follows; manual holds ──
    wid = page.evaluate(
        "() => { let id = null; __cockpit.forEachWidget(w => { if (!id && w.setSide) id = w.cfg.id; }); return id; }"
    )
    widget_eval(
        page, wid,
        "(w) => { if (w.cfg.signals.length < 2)"
        " w.addSignal('app_motorControl_data.channels[0].velocityMeasured_radPerSec'); }",
    )
    check("views_007 a multi-signal plot exists", wid is not None, wid)
    page.click(f"[data-widget-id='{wid}'] .widget-menu")
    page.wait_for_selector(".axes-popover")
    page.locator(".axes-popover .axes-row").first.locator("[data-side='R']").click()
    right_labels = page.eval_on_selector_all(
        ".plot-widget--split .y-axis--right span", "els => els.map(e => e.textContent)"
    )
    left_labels = page.eval_on_selector_all(
        ".plot-widget--split .y-axis--left span", "els => els.map(e => e.textContent)"
    )
    check("views_007 assignment renders right-scale labels", len(right_labels) == 5, right_labels)
    check("views_007 scales are distinct", right_labels != left_labels or len(left_labels) == 0)
    derived = page.locator(f"[data-widget-id='{wid}'] .axis-mode--derived").inner_text()
    check("views_007 derived chip reads split", derived == "split", derived)

    # ── [test->app~views_005~1] split axes: the readout lists BOTH scale
    #    groups' values at once (regression: the paged readout dropped the
    #    right-axis signal from the hover popup) ──
    newest = page.evaluate("() => __cockpit.histories.get('task1msRuns').newestTick()")
    page.evaluate(f"() => __cockpit.setCursorTick({newest - 500})")
    ro = page.locator(f"[data-widget-id='{wid}'] .cursor-readout")
    nsig = widget_eval(page, wid, "(w) => w.cfg.signals.length")
    rows = ro.locator(".readout-row").count()
    labels = ro.locator(".readout-group-label").all_inner_texts()
    check(
        "views_005 split readout lists both groups' values at once",
        rows == nsig and nsig >= 2 and len(labels) == 2,
        (rows, nsig, labels),
    )
    page.evaluate("() => __cockpit.clearCursor()")

    # Auto vs manual on a scratch plot fed a monotonic (ramp-kind) signal.
    ramp = page.evaluate(
        "() => (__cockpit.store.signals.find(s => (s.kind === 'u16' || s.kind === 'u32')"
        " && !__cockpit.store.watched.has(s.path)) || {}).path || null"
    )
    check("views_007 a ramp-kind signal exists for the auto check", ramp is not None, ramp)
    page.evaluate(f"() => __cockpit.addWatch({ramp!r}, 1)")
    scratch = page.evaluate(
        f"() => {{ const w = __cockpit.addWidget({{ type: 'plot', signals: [] }}); w.addSignal({ramp!r}); return w.cfg.id; }}"
    )
    wait_for_samples(page, ramp, 100)

    def scale_l(widget_id):
        return widget_eval(page, widget_id, "(w) => w.ranges().L ?? null")

    a1 = scale_l(scratch)
    page.wait_for_timeout(400)
    a2 = scale_l(scratch)
    check(
        "views_007 auto axis follows the signal's extents",
        a1 is not None and a2 is not None and (a2[1] > a1[1] or a2[0] < a1[0]),
        (a1, a2),
    )

    page.click(f"[data-widget-id='{scratch}'] .widget-menu")
    page.wait_for_selector(".axes-popover .axes-scale")
    page.click(".axes-popover [data-mode='manual']")
    page.fill(".axes-popover [data-rmin]", "0")
    page.fill(".axes-popover [data-rmax]", "100")
    page.locator(".axes-popover [data-rmax]").press("Tab")
    m1 = scale_l(scratch)
    page.wait_for_timeout(400)
    m2 = scale_l(scratch)
    check("views_007 manual bounds hold while values clip", m1 == [0, 100] and m2 == [0, 100], (m1, m2))
    page.fill(".axes-popover [data-rmin]", "500")
    page.locator(".axes-popover [data-rmin]").press("Tab")
    invalid = page.evaluate("() => !!document.querySelector('.axes-popover .scale-range.is-invalid')")
    m3 = scale_l(scratch)
    check("views_007 invalid manual pair rejected, bounds kept", invalid and m3 == [0, 100], (invalid, m3))

    # ── [test->app~views_004~1] deselect via the configuration menu ──
    page.click(".axes-popover .axes-row [data-deselect]")
    left_state = widget_eval(
        page, scratch,
        "(w) => ({ n: w.cfg.signals.length, hint: !w.el.querySelector('.plot-empty-hint').hidden })",
    )
    check(
        "views_004 deselect leaves the widget with its drop hint",
        left_state == {"n": 0, "hint": True},
        left_state,
    )
    check(
        "views_004 last-holder deselect removes the watch",
        page.evaluate(f"() => !__cockpit.store.watched.has({ramp!r})"),
    )
    page.keyboard.press("Escape")
    popover_gone = page.evaluate("() => !document.querySelector('.axes-popover')")
    check("views_007 Escape dismisses the popover", popover_gone)
    widget_eval(page, scratch, "(w) => w.hooks.onRemove(w.cfg.id)")
    # Flush the deselect's debounced re-commit NOW: the mock restarts its
    # stream on acceptance, and the timeline block must not race that.
    page.evaluate("() => __cockpit.commit()")
    wait_for_samples(page, "task1msRuns", 200)

    # ── [test->app~views_011~1] trace colors consistent across legends ──
    # (color identity is Trace appearance's requirement since the 011 split)
    color_ok = page.evaluate(
        """() => { const norm = (c) => { const d = document.createElement('i');
             d.style.color = c; document.body.appendChild(d);
             const v = getComputedStyle(d).color; d.remove(); return v; };
           let ok = true, seen = 0;
           document.querySelectorAll('.widget-legend [data-legend]').forEach(e => {
             const w = __cockpit.store.watched.get(e.dataset.legend);
             const bar = e.querySelector('.legend-bar');
             if (!w || !bar) return;
             seen++;
             if (getComputedStyle(bar).backgroundColor !== norm(w.color)) ok = false;
           });
           return ok && seen > 0; }"""
    )
    check("views_011 trace color consistent across legends", color_ok)

    # ── [test->app~views_008~1] timeline time base: span, pause, resume ──
    page.click(".timeline-bar [data-span='30000']")
    widths = page.evaluate(
        "() => { const ws = []; __cockpit.forEachWidget(w => { if (w.window) ws.push(Math.round(w.window[1] - w.window[0])); }); return ws; }"
    )
    check("views_008 span renders 30 s on every plot", len(widths) == 2 and all(w == 30000 for w in widths), widths)

    page.click(".timeline-bar [data-pause]")
    frozen1 = page.evaluate("() => JSON.stringify(__cockpit.timeline.get().window)")
    newest_before = page.evaluate("() => __cockpit.histories.get('task1msRuns').newestTick()")
    page.wait_for_timeout(300)  # several 50 ms mock batches
    page.evaluate("() => __cockpit.forEachWidget(w => w.refresh())")
    frozen2 = page.evaluate("() => JSON.stringify(__cockpit.timeline.get().window)")
    newest_after = page.evaluate("() => __cockpit.histories.get('task1msRuns').newestTick()")
    check("views_008 pause freezes the window", frozen1 == frozen2, (frozen1, frozen2))
    check("views_008 acquisition continues while paused", newest_after > newest_before, (newest_before, newest_after))

    # ── [test->app~views_009~1] paused range control: zoom, select, pan ──
    win0 = page.evaluate("() => __cockpit.timeline.get().window")
    cursor_t = int(win0[0] + (win0[1] - win0[0]) * 0.25)
    page.evaluate(f"() => __cockpit.setCursorTick({cursor_t})")
    canvas = page.locator(".plot-widget .plot-canvas").first
    canvas.dispatch_event("wheel", {"deltaY": -300, "bubbles": True, "cancelable": True})
    win1 = page.evaluate("() => __cockpit.timeline.get().window")
    check(
        "views_009 wheel zooms in about the cursor",
        (win1[1] - win1[0]) < (win0[1] - win0[0]) and win1[0] <= cursor_t <= win1[1],
        (win0, win1, cursor_t),
    )
    synced = page.evaluate(
        "() => { const t = __cockpit.timeline.get().window; let ok = true;"
        " __cockpit.forEachWidget(w => { if (w.window && (Math.abs(w.window[0] - t[0]) > 1 || Math.abs(w.window[1] - t[1]) > 1)) ok = false; });"
        " return ok; }"
    )
    check("views_009 zoom applies to every plot", synced)

    # The free canvas scrolls; earlier clicks may have scrolled this plot out
    # of the viewport, and real mouse events land on whatever is at the
    # coordinates. Park it center-screen before the drag work.
    canvas.evaluate("el => el.scrollIntoView({ block: 'center' })")
    box = canvas.bounding_box()
    y = box["y"] + box["height"] / 2
    page.mouse.move(box["x"] + box["width"] * 0.3, y)
    page.mouse.down()
    page.mouse.move(box["x"] + box["width"] * 0.6, y, steps=5)
    page.mouse.up()
    win2 = page.evaluate("() => __cockpit.timeline.get().window")
    exp0 = win1[0] + (win1[1] - win1[0]) * 0.3
    exp1 = win1[0] + (win1[1] - win1[0]) * 0.6
    tol = (win1[1] - win1[0]) * 0.06
    check(
        "views_009 drag-select zooms every plot to the range",
        abs(win2[0] - exp0) < tol and abs(win2[1] - exp1) < tol,
        (win2, exp0, exp1),
    )

    page.evaluate("() => __cockpit.timeline.panBy(10_000_000)")
    win3 = page.evaluate("() => __cockpit.timeline.get().window")
    pspan = page.evaluate("() => __cockpit.timeline.get().pausedSpan")
    check(
        "views_009 pan clamps at the span edge, scale kept",
        abs(win3[1] - pspan[1]) < 1 and abs((win3[1] - win3[0]) - (win2[1] - win2[0])) < 1,
        (win3, pspan),
    )
    canvas.dispatch_event("wheel", {"deltaY": -240, "deltaX": 0, "shiftKey": True, "bubbles": True, "cancelable": True})
    win4 = page.evaluate("() => __cockpit.timeline.get().window")
    check(
        "views_009 shift-wheel pans at fixed scale",
        win4[0] < win3[0] and abs((win4[1] - win4[0]) - (win3[1] - win3[0])) < 1,
        (win3, win4),
    )

    # zoom-in floor: the spec's 10 ms bound
    page.evaluate(
        "() => { const w = __cockpit.timeline.get().window;"
        " __cockpit.timeline.zoomAt((w[0] + w[1]) / 2, 1e-9); }"
    )
    floor_w = page.evaluate("() => { const w = __cockpit.timeline.get().window; return w[1] - w[0]; }")
    check("views_009 zoom-in stops at 10 ms", abs(floor_w - 10) < 1, floor_w)

    # ── [test->app~views_009~1] a step into the bound leaves the range
    #    unchanged: with the width pinned, the old re-centering slid the
    #    window toward the wheel's anchor — zoom must never read as pan.
    #    The cursor sits OFF-center (25%), the drift repro's shape. ──
    page.evaluate(
        "() => { const w = __cockpit.timeline.get().window;"
        " __cockpit.setCursorTick(Math.round(w[0] + (w[1] - w[0]) * 0.25)); }"
    )
    at_floor = page.evaluate("() => [...__cockpit.timeline.get().window]")
    for _ in range(3):
        canvas.dispatch_event("wheel", {"deltaY": -300, "bubbles": True, "cancelable": True})
    still = page.evaluate("() => [...__cockpit.timeline.get().window]")
    check(
        "views_009 zoom-in at the floor is a dead stop, both edges pinned",
        still == at_floor,
        (at_floor, still),
    )

    # a sub-6 px drag is a click, not a selection
    wf0 = page.evaluate("() => [...__cockpit.timeline.get().window]")
    page.mouse.move(box["x"] + box["width"] * 0.5, y)
    page.mouse.down()
    page.mouse.move(box["x"] + box["width"] * 0.5 + 3, y)
    page.mouse.up()
    wf1 = page.evaluate("() => [...__cockpit.timeline.get().window]")
    check("views_009 sub-6px drag does not zoom", wf0 == wf1, (wf0, wf1))

    # wheel outside any plot canvas leaves the range alone
    page.locator(".workspace").dispatch_event(
        "wheel", {"deltaY": -300, "bubbles": True, "cancelable": True}
    )
    wf2 = page.evaluate("() => [...__cockpit.timeline.get().window]")
    check("views_009 wheel outside plots leaves the range", wf1 == wf2, (wf1, wf2))

    # theme switch while paused keeps the window
    page.evaluate("() => document.documentElement.setAttribute('data-theme', 'neon')")
    wf3 = page.evaluate("() => [...__cockpit.timeline.get().window]")
    page.evaluate("() => document.documentElement.setAttribute('data-theme', 'warm')")
    check("views_008 theme switch keeps the paused window", wf2 == wf3, (wf2, wf3))

    # resume mid-drag: the pending selection dies with the pause
    page.mouse.move(box["x"] + box["width"] * 0.2, y)
    page.mouse.down()
    page.mouse.move(box["x"] + box["width"] * 0.5, y, steps=3)
    page.evaluate("() => __cockpit.timeline.resume()")
    page.mouse.up()
    mid_drag = page.evaluate(
        "() => ({ mode: __cockpit.timeline.get().mode, win: __cockpit.timeline.get().window })"
    )
    check(
        "views_009 resume mid-drag cancels the selection",
        mid_drag["mode"] == "live" and mid_drag["win"] is None,
        mid_drag,
    )
    page.evaluate("() => __cockpit.timeline.pause()")

    page.click(".timeline-bar [data-resume]")
    page.wait_for_timeout(150)
    page.evaluate("() => __cockpit.forEachWidget(w => w.refresh())")
    resumed = page.evaluate(
        "() => { const t = __cockpit.timeline.get(); const n = __cockpit.histories.get('task1msRuns').newestTick();"
        " let w0 = null; __cockpit.forEachWidget(w => { if (!w0 && w.window) w0 = w.window; });"
        " return { mode: t.mode, span: t.span_ms, w0, n }; }"
    )
    check(
        "views_008 resume returns every plot to the live span",
        resumed["mode"] == "live" and abs((resumed["w0"][1] - resumed["w0"][0]) - resumed["span"]) < 1,
        resumed,
    )
    check(
        "views_008 live edge includes paused-interval samples",
        abs(resumed["w0"][1] - resumed["n"]) < 200 and resumed["n"] > newest_after,
        resumed,
    )

    page.click(".timeline-bar [data-span='5000']")
    w5 = page.evaluate(
        "() => { let w0 = null; __cockpit.forEachWidget(w => { if (!w0 && w.window) w0 = w.window; });"
        " return Math.round(w0[1] - w0[0]); }"
    )
    check("views_008 span change after resume applies immediately", w5 == 5000, w5)
    page.click(".timeline-bar [data-span='30000']")

    for _ in range(10):
        page.evaluate("() => { __cockpit.timeline.pause(); __cockpit.timeline.resume(); }")
    rapid = page.evaluate("() => __cockpit.timeline.get().mode")
    check("views_008 rapid pause/resume ends live", rapid == "live", rapid)

    page.evaluate("() => __cockpit.clearCursor()")

    # ── [test->app~views_004~1] restart restores layout, signals, colors —
    #    and the axes configuration (sides + scale modes + manual bounds) ──
    page.evaluate(
        "() => { let done = false; __cockpit.forEachWidget(w => {"
        " if (!done && w.setScaleMode && w.cfg.signals.length) { w.setScaleMode('L', 'manual'); done = true; } }); }"
    )
    snapshot_js = (
        "() => JSON.stringify({ n: document.querySelectorAll('.widget').length,"
        " watched: [...__cockpit.store.watched.keys()].sort(),"
        " colors: [...__cockpit.store.watched.values()].map(w => w.color).sort(),"
        " axes: (() => { const c = []; __cockpit.forEachWidget(w =>"
        "   c.push({ id: w.cfg.id, sides: w.cfg.sides || null, scales: w.cfg.scales || null })); return c; })() })"
    )
    before = page.evaluate(snapshot_js)
    restart(page, load_elf=True, wait_js="() => document.querySelectorAll('.widget').length > 0")
    after = page.evaluate(snapshot_js)
    check(
        "views_004 restart restores arrangement/signals/colors/axes config",
        before == after,
        f"{before} != {after}",
    )

    # ── [test->app~views_008~1] sacred pause: span kept, catch-up capped, honest gap ──
    boot(page)
    add_plot_with(page, ["task1msRuns"])
    wait_for_samples(page, "task1msRuns", 200)
    page.click(".timeline-bar [data-pause]")
    ps = page.evaluate("() => [...__cockpit.timeline.get().pausedSpan]")
    # A synthetic stream far ahead of real time, through the real append path:
    # one burst inside the 120 s catch-up cap, one far past it.
    page.evaluate(
        "(ps) => { const h = __cockpit.histories.get('task1msRuns');"
        " const a = []; for (let t = 1000; t <= 1100; t++) a.push([Math.round(ps[1]) + t, 1]);"
        " h.append(a);"
        " const b = []; for (let t = 130000; t <= 130100; t++) b.push([Math.round(ps[1]) + t, 2]);"
        " h.append(b); }",
        ps,
    )
    kept = page.evaluate(
        "() => { const h = __cockpit.histories.get('task1msRuns');"
        " return { newest: h.newestTick(), oldest: h.ticks[0] }; }"
    )
    check(
        "views_008 paused catch-up caps appends at 120 s",
        kept["newest"] <= ps[1] + 120000,
        (kept, ps),
    )
    check(
        "views_008 paused span start is never trimmed",
        kept["oldest"] <= ps[0] + 1,
        (kept, ps),
    )
    page.click(".timeline-bar [data-resume]")
    page.evaluate(
        "(ps) => { const h = __cockpit.histories.get('task1msRuns');"
        " const c = []; for (let t = 130200; t <= 130260; t++) c.push([Math.round(ps[1]) + t, 3]);"
        " h.append(c); }",
        ps,
    )
    gap_ok = page.evaluate(
        "(ps) => { const h = __cockpit.histories.get('task1msRuns');"
        " return h.gaps.some(([a, b]) => a <= Math.round(ps[1]) + 1101 && b >= Math.round(ps[1]) + 130000); }",
        ps,
    )
    check("views_008 resume after a marathon pause shows an honest gap", gap_ok, ps)

    # ═══ bench batch 2: resize fix, filter, watch panel ═══

    # ── [test->app~views_004~1] the corner handle resizes the widget, and
    #    the size survives the restart round trip ──
    fresh_boot(page)
    add_plot_with(page, ["task1msRuns"])
    page.wait_for_selector(".plot-widget")
    hb = page.locator(".resize-handle").first.bounding_box()
    before_h = page.locator(".plot-widget").first.bounding_box()["height"]
    page.mouse.move(hb["x"] + 6, hb["y"] + 6)
    page.mouse.down()
    for i in range(1, 9):
        page.mouse.move(hb["x"] + 6, hb["y"] + 6 + 25 * i)
    page.mouse.up()
    after_h = page.locator(".plot-widget").first.bounding_box()["height"]
    check(
        "views_004 corner drag grows the widget",
        after_h >= before_h + 150,
        (before_h, after_h),
    )
    restart(page, load_elf=True, wait_js="() => document.querySelectorAll('.plot-widget').length > 0")
    restored_h = page.locator(".plot-widget").first.bounding_box()["height"]
    check(
        "views_004 resized height survives restart",
        abs(restored_h - after_h) <= 2,
        (after_h, restored_h),
    )

    # ── [test->app~obs_005~1] filter: substring / glob / regex / invalid ──
    boot(page)

    def filter_paths(q, until=None):
        # The filter applies after a 120 ms debounce; on a loaded host the
        # debounce + re-render can outlive any flat sleep (CI flake: the
        # read saw the previous query's list). Callers pass `until` — a JS
        # predicate over the presented list stating THIS query's end-state
        # — so the read never races the debounce. The flat wait remains
        # only for the cache-poll loop, whose result legitimately may not
        # change between retries.
        page.fill(".picker-search input", q)
        if until:
            page.wait_for_function(until, timeout=8000)
        else:
            page.wait_for_timeout(250)  # filter debounce is 120 ms
        return page.evaluate("() => __cockpit.store.signals.map(s => s.path)")

    # The picker's full-namespace cache loads async after the ELF; poll with
    # the match-all glob (an empty fill fires no input event) until a filter
    # pass sees the whole 704.
    full = 0
    for _ in range(20):
        full = len(filter_paths("*"))
        if full > 600:
            break
        page.wait_for_timeout(200)
    check("obs_005 full namespace cached for filtering", full > 600, full)

    sub = filter_paths(
        "velocity",
        until=f"""() => {{ const l = __cockpit.store.signals.map(s => s.path);
            return l.length > 0 && l.length < {full}
                && l.every(p => p.toLowerCase().includes('velocity')); }}""",
    )
    check(
        "obs_005 substring narrows to matches",
        len(sub) > 0 and all("velocity" in p.lower() for p in sub) and len(sub) < full,
        (len(sub), full),
    )
    glob = filter_paths(
        "chan*velocity",
        until="""() => { const l = __cockpit.store.signals.map(s => s.path);
            return l.length > 0 && l.every(p => p.toLowerCase().includes('chan')
                && p.toLowerCase().includes('velocity')); }""",
    )
    check(
        "obs_005 glob * matches any run",
        len(glob) > 0 and all("chan" in p.lower() and "velocity" in p.lower() for p in glob),
        glob[:3],
    )
    rex = filter_paths(
        r"buf\[1[0-2]\]",
        until="""() => { const l = __cockpit.store.signals.map(s => s.path);
            return l.length === 3 && l.every(p => p.startsWith('est_flux_data.buf[1')); }""",
    )
    check(
        "obs_005 regex matches exactly",
        sorted(rex) == [f"est_flux_data.buf[{i}]" for i in (10, 11, 12)],
        rex[:5],
    )
    bad = filter_paths(
        "velocity(",
        until="() => __cockpit.store.signals.length === 0",
    )
    check("obs_005 invalid regex falls back to substring", bad == [], bad[:3])
    restored = filter_paths(
        "",
        until=f"() => __cockpit.store.signals.length === {full}",
    )
    check("obs_005 clearing the filter restores the namespace", len(restored) == full, (len(restored), full))

    # ── [test->app~views_010~1] the watch panel ──
    add_plot_with(page, ["task1msRuns"])
    page.wait_for_selector(".watch-row[data-path='task1msRuns']")
    check("views_010 watching adds a panel row", True)
    v1 = page.locator(".watch-row [data-value]").first.inner_text()
    page.wait_for_function(
        f"() => document.querySelector('.watch-row [data-value]').textContent !== {v1!r}",
        timeout=5000,
    )
    check("views_010 value cell tracks the newest sample", True)
    installs_before = page.evaluate("() => (window.__devmockInstalls || []).length")
    page.click(".watch-row[data-path='task1msRuns'] [data-period='100']")
    page.wait_for_function(
        f"() => (window.__devmockInstalls || []).length === {installs_before} + 1",
        timeout=5000,
    )
    last = page.evaluate("() => window.__devmockInstalls.at(-1)")
    check(
        "views_010 period edit reaches the device as one recommitted list",
        len(last) == 1 and last[0]["path"] == "task1msRuns" and last[0]["period_ms"] == 100,
        last,
    )
    page.click(".watch-row[data-path='task1msRuns'] .watch-remove")
    gone = page.evaluate(
        """() => ({
          rows: document.querySelectorAll('.watch-row').length,
          watched: __cockpit.store.watched.size,
          held: (() => { let n = 0; __cockpit.forEachWidget(w => n += w.cfg.signals.length); return n; })(),
          hint: !document.querySelector('.plot-empty-hint')?.hidden,
        })"""
    )
    check(
        "views_010 panel remove unwatches everywhere, widgets remain",
        gone["rows"] == 0 and gone["watched"] == 0 and gone["held"] == 0 and gone["hint"],
        gone,
    )

    # ── [test->app~obs_003~1] a drop-join of an already-watched signal keeps
    #    its period (regression: addWatch reset a 1 ms watch back to 10 ms) ──
    page.evaluate("() => __cockpit.addWatch('task1msRuns', 10)")
    page.evaluate("() => __cockpit.setPeriod('task1msRuns', 1)")
    page.wait_for_selector(
        ".watch-row[data-path='task1msRuns'] [data-period='1'].watch-seg-opt--on"
    )
    page.evaluate(
        """() => {
          const dt = new DataTransfer();
          dt.setData('text/x-signal', 'task1msRuns');
          const el = document.querySelector('.plot-widget');
          const r = el.getBoundingClientRect();
          el.dispatchEvent(new DragEvent('dragover', { dataTransfer: dt, bubbles: true }));
          el.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true,
            clientX: r.left + r.width / 2, clientY: r.top + r.height / 2 }));
        }"""
    )
    after_drop = page.evaluate(
        """() => ({
          period: __cockpit.store.watched.get('task1msRuns')?.period_ms,
          joined: (() => { let j = false; __cockpit.forEachWidget(w =>
            { if (w.cfg.signals.includes('task1msRuns')) j = true; }); return j; })(),
          seg: !!document.querySelector(
            ".watch-row[data-path='task1msRuns'] [data-period='1'].watch-seg-opt--on"),
        })"""
    )
    check(
        "obs_003 drop-join keeps the 1 ms period and joins the widget",
        after_drop["period"] == 1 and after_drop["joined"] and after_drop["seg"],
        after_drop,
    )
    # Leave the workspace as views_010 left it (unwatched, emptied plot).
    page.click(".watch-row[data-path='task1msRuns'] .watch-remove")

    # ── picker column resize + collapse (chrome ergonomics; no spec) ──
    rz = page.locator(".picker-resizer").bounding_box()
    w0 = page.locator(".signal-picker").bounding_box()["width"]
    page.mouse.move(rz["x"] + 3, rz["y"] + 200)
    page.mouse.down()
    page.mouse.move(rz["x"] + 143, rz["y"] + 200, steps=5)
    page.mouse.up()
    w1 = page.locator(".signal-picker").bounding_box()["width"]
    check("picker column resizes by its edge", w1 >= w0 + 100, (w0, w1))
    page.click(".picker-collapse")
    wc = page.locator(".signal-picker").bounding_box()["width"]
    check("picker collapses to a rail", wc < 50, wc)
    restart(page)
    wr = page.locator(".signal-picker").bounding_box()["width"]
    check("collapsed state persists", wr < 50, wr)
    page.click(".picker-expand")
    we = page.locator(".signal-picker").bounding_box()["width"]
    check("picker expands to its saved width", abs(we - w1) <= 2, (w1, we))

    # ═══ batch 3: trace appearance + pointed-trace emphasis ═══════════════

    # Re-boot after the picker block's reload (empty watched from views_010).
    page.evaluate("() => __cockpit.api.loadElf('mock.elf')")
    page.evaluate("() => __cockpit.api.listSignals('')")
    page.wait_for_function("() => __cockpit.store.gate === 'matched'")
    sig_a = "app_motorControl_data.channels[0].phaseCurrent_a[0]"
    sig_b = "app_motorControl_data.channels[0].phaseCurrent_a[1]"
    add_plot_with(page, [sig_a, sig_b], 1)
    # A second plot holding sig_a: appearance must render on EVERY widget.
    page.evaluate(
        "(p) => { const w = __cockpit.addWidget({ type: 'plot', signals: [] }); w.addSignal(p); }",
        sig_a,
    )
    page.evaluate("() => __cockpit.commit()")
    wait_for_samples(page, sig_a)
    wait_for_samples(page, sig_b)

    # ── [test->app~views_011~1] style/dots/interp render on every widget ──
    page.evaluate(f"() => __cockpit.appearance.set({sig_a!r}, {{ style: 'dashed', dots: true }})")
    opts = page.evaluate(
        f"""() => {{ const out = [];
          __cockpit.forEachWidget(w => {{ if (!w.traceInfo) return;
            if (!w.cfg.signals.includes({sig_a!r})) return;
            const t = w.traceInfo({sig_a!r});
            out.push({{ dash: t.dash, dots: t.dots }}); }});
          return out; }}"""
    )
    check(
        "views_011 style+dots render on every widget holding the signal",
        len(opts) == 2 and all(o["dash"] == [8, 5] and o["dots"] for o in opts),
        opts,
    )

    # Interpolation: builder identity per mode, with the gap markers (null)
    # still present in the plotted data — no mode bridges a tick-count gap.
    page.wait_for_function(
        f"() => (__cockpit.histories.get({sig_a!r})?.gaps.length || 0) >= 1", timeout=9000
    )
    for mode in ("zoh", "cubic", "linear"):
        page.evaluate(f"() => __cockpit.appearance.set({sig_a!r}, {{ interp: {mode!r} }})")
        state = page.evaluate(
            f"""() => {{ const out = {{ modes: [], hasNull: false }};
              __cockpit.forEachWidget(w => {{ if (!w.traceInfo) return;
                if (!w.cfg.signals.includes({sig_a!r})) return;
                out.modes.push(w.traceInfo({sig_a!r}).interp);
                const [, ys] = w.renderedTables().get({sig_a!r});
                if (ys.some(v => v === null)) out.hasNull = true; }});
              return out; }}"""
        )
        check(
            f"views_011 {mode} interpolation applies and keeps gap markers",
            state["modes"] and all(n == mode for n in state["modes"]) and state["hasNull"],
            state,
        )

    # Monotone cubic never overshoots: the tangent oracle that drives the
    # path's beziers, sampled densely over a step-like run.
    ext = page.evaluate(
        """() => { const x = [0, 1, 2, 3, 4], y = [0, 0, 100, 100, 100];
          let mx = -1e9, mn = 1e9;
          for (let s = 0; s <= 400; s++) {
            const v = __cockpit.interp.evalMonotoneRun(x, y, s / 100);
            mx = Math.max(mx, v); mn = Math.min(mn, v);
          }
          return { mx, mn }; }"""
    )
    check(
        "views_011 monotone cubic never overshoots the samples",
        ext["mx"] <= 100 + 1e-9 and ext["mn"] >= -1e-9,
        ext,
    )

    # Color override: renders wherever the trace color renders; survives a
    # theme switch exactly as chosen while auto colors retint.
    auto_b = page.evaluate(f"() => __cockpit.store.watched.get({sig_b!r}).color")
    page.evaluate(f"() => __cockpit.appearance.set({sig_a!r}, {{ color: '#ff0000' }})")
    spread = page.evaluate(
        f"""() => {{ const red = (el) => getComputedStyle(el).backgroundColor === 'rgb(255, 0, 0)';
          const legends = [...document.querySelectorAll(`.widget-legend [data-legend="{sig_a}"] .legend-bar`)];
          const row = document.querySelector(`.watch-row[data-path="{sig_a}"] .legend-bar`);
          return {{ legends: legends.length, legendsRed: legends.every(red), rowRed: row ? red(row) : false }}; }}"""
    )
    check(
        "views_011 color override renders in legends and the watch panel",
        spread["legends"] == 2 and spread["legendsRed"] and spread["rowRed"],
        spread,
    )
    page.evaluate("() => document.documentElement.setAttribute('data-theme', 'graphite')")
    themed = page.evaluate(
        f"""() => ({{ a: __cockpit.store.watched.get({sig_a!r}).color,
                      b: __cockpit.store.watched.get({sig_b!r}).color }})"""
    )
    check(
        "views_011 override survives the theme switch; auto retints",
        themed["a"] == "#ff0000" and themed["b"] != auto_b,
        (themed, auto_b),
    )
    page.evaluate("() => document.documentElement.removeAttribute('data-theme')")

    # ── [test->app~views_012~1] pointed-trace emphasis, paused only ──
    page.evaluate("() => __cockpit.timeline.pause()")
    targets = page.evaluate(
        f"""() => {{ let res = null;
          __cockpit.forEachWidget(w => {{
            if (res || !w.renderedTables || w.cfg.signals.length !== 2) return;
            // The grid scrolls: the widget must be truly on screen, or the
            // real mouse events land on whatever covers those coordinates.
            w.el.scrollIntoView({{ block: 'center' }});
            w.refresh();
            const canvas = w.el.querySelector('.plot-canvas');
            const inCanvas = ([x, y]) =>
              document.elementFromPoint(x, y)?.closest('.plot-canvas') === canvas;
            const rect = canvas.getBoundingClientRect();
            {NEAREST_Y_JS}
            for (const frac of [0.5, 0.4, 0.6, 0.3, 0.7]) {{
              const px = rect.width * frac;
              const t = w.tickAtPx(px, rect.width);
              const ys = w.cfg.signals.map((p) => nearestY(w, p, t, rect));
              if (ys.some(v => v === null)) continue;
              const [yA, yB] = ys;
              if (Math.abs(yA - yB) < 30) continue;
              let far = null, farD = -1;
              for (let y = 4; y < rect.height - 4; y += 4) {{
                const d = Math.min(Math.abs(y - yA), Math.abs(y - yB));
                if (d > farD) {{ farD = d; far = y; }}
              }}
              if (farD < 50) continue;
              const clampY = (y) => Math.min(rect.height - 3, Math.max(3, y));
              const cand = {{ id: w.cfg.id,
                nearA: [rect.left + px, rect.top + clampY(yA + 6)],
                nearB: [rect.left + px, rect.top + clampY(yB + 6)],
                far: [rect.left + px, rect.top + far] }};
              if (!inCanvas(cand.nearA) || !inCanvas(cand.nearB) || !inCanvas(cand.far)) continue;
              res = cand;
            }}
          }});
          return res; }}"""
    )
    check("views_012 geometry probe found separable targets", targets is not None, targets)
    pointed_of = (
        "() => { let p = null; __cockpit.forEachWidget(w => { if (w.pointed) p = w.pointed; }); return p; }"
    )

    def wait_pointed(value):
        expr = "null" if value is None else repr(value)
        page.wait_for_function(
            f"() => {{ let p = null; __cockpit.forEachWidget(w => {{ if (w.pointed) p = w.pointed; }});"
            f" return p === {expr}; }}",
            timeout=3000,
        )

    widths_of = (
        """(id) => { let out = null;
          __cockpit.forEachWidget(w => { if (w.cfg.id === id && w.traceInfo)
            out = w.cfg.signals.map(p => w.traceInfo(p).widthPx); });
          return out; }"""
    )
    page.mouse.move(*targets["nearA"])
    wait_pointed(sig_a)
    widths = page.evaluate(widths_of, targets["id"])
    marked = page.evaluate(
        f"""() => {{ const r = document.querySelector('.readout-row--pointed');
          return r ? r.dataset.path : null; }}"""
    )
    check(
        "views_012 pointing thickens the trace and marks its readout row",
        widths == [3.0, 1.5] and marked == sig_a,
        (widths, marked),
    )
    page.mouse.move(*targets["nearB"])
    wait_pointed(sig_b)
    widths = page.evaluate(widths_of, targets["id"])
    marked = page.evaluate(
        "() => document.querySelector('.readout-row--pointed')?.dataset.path || null"
    )
    check(
        "views_012 moving nearer another trace transfers the emphasis",
        widths == [1.5, 3.0] and marked == sig_b,
        (widths, marked),
    )
    page.mouse.move(*targets["far"])
    wait_pointed(None)
    widths = page.evaluate(widths_of, targets["id"])
    check("views_012 nothing within 40 px clears the emphasis", widths == [1.5, 1.5], widths)
    page.evaluate("() => __cockpit.timeline.resume()")
    page.mouse.move(*targets["nearA"])
    page.wait_for_timeout(120)  # a few rAFs: live mode must stay unemphasized
    live_pointed = page.evaluate(pointed_of)
    check("views_012 live mode never emphasizes", live_pointed is None, live_pointed)
    page.mouse.move(10, 10)

    # ── persistence: appearance restores, emphasis leaves no trace ──
    restart(page)
    restored = page.evaluate(f"() => __cockpit.appearance.of({sig_a!r})")
    check(
        "views_011 appearance persists across restart",
        restored["color"] == "#ff0000" and restored["style"] == "dashed" and restored["dots"],
        restored,
    )
    base_widths = page.evaluate(
        """() => { const out = [];
          __cockpit.forEachWidget(w => { if (w.traceInfo)
            out.push(...w.cfg.signals.map(p => w.traceInfo(p).widthPx)); });
          return out; }"""
    )
    check(
        "views_012 emphasis is transient — base stroke widths after restart",
        all(w == 1.5 for w in base_widths),
        base_widths,
    )

    # ═══ batch 4: hide-const, group collapse, session restore, prefs ═══

    # ── [test->app~obs_006~1] the exclusion narrows to writable ∩ matches ──
    fresh_boot(page)
    page.wait_for_function("() => document.querySelectorAll('.signal-row').length > 0")
    page.fill(".picker-search input", "phaseCurrent")
    page.wait_for_function(
        "() => __cockpit.store.signals.length > 0"
        " && __cockpit.store.signals.every(s => s.path.includes('phaseCurrent'))"
    )
    both = page.evaluate("() => __cockpit.store.signals.length")
    page.click("[data-hideconst]")
    page.wait_for_function("() => __cockpit.store.signals.every(s => !s.readonly)")
    writable_only = page.evaluate("() => __cockpit.store.signals.length")
    check(
        "obs_006 exclusion presents exactly the writable matches",
        both == 6 and writable_only == 3,
        (both, writable_only),
    )
    page.click("[data-hideconst]")
    page.wait_for_function(f"() => __cockpit.store.signals.length === {both}")
    check("obs_006 disabling restores the read-only matches", True)

    # ── [test->app~obs_006~1] a watched signal hidden by the exclusion keeps
    #    its watch-panel row ──
    page.fill(".picker-search input", "*")  # match-all (an empty fill fires no event)
    page.wait_for_function("() => __cockpit.store.signals.length > 20")
    page.evaluate("() => __cockpit.addWatch('IO_bridge_channelConfig.deadtime_ns', 100)")
    page.wait_for_function(
        "() => document.querySelector('.watch-panel')?.textContent.includes('deadtime_ns')"
    )
    page.click("[data-hideconst]")
    page.wait_for_function(
        "() => !document.querySelector('.signal-row[data-path=\"IO_bridge_channelConfig.deadtime_ns\"]')"
    )
    check(
        "obs_006 watched hidden signal stays watched with its panel row",
        page.evaluate("() => __cockpit.store.watched.has('IO_bridge_channelConfig.deadtime_ns')")
        and page.evaluate(
            "() => document.querySelector('.watch-panel').textContent.includes('deadtime_ns')"
        ),
    )
    page.click("[data-hideconst]")
    page.evaluate("() => __cockpit.removeWatch('IO_bridge_channelConfig.deadtime_ns')")

    # ── picker group collapse (chrome ergonomics; persisted; search-stable) ──
    page.wait_for_selector('.picker-group[data-group="IO_bridge_channelConfig"]')
    page.click('.picker-group[data-group="IO_bridge_channelConfig"]')
    collapsed_now = page.evaluate(
        "() => !document.querySelector('.signal-row[data-path^=\"IO_bridge_channelConfig\"]')"
        " && document.querySelector('.picker-group--collapsed .picker-group-count')?.textContent === '5'"
    )
    check("picker group collapses on header click with a hidden-row count", collapsed_now)
    page.fill(".picker-search input", "phase")
    page.wait_for_function("() => __cockpit.store.signals.some(s => s.path.includes('phaseCurrent_a'))")
    check(
        "picker collapsed group stays collapsed during search",
        page.evaluate(
            "() => !document.querySelector('.signal-row[data-path^=\"IO_bridge_channelConfig\"]')"
            " && !!document.querySelector('.picker-group--collapsed[data-group=\"IO_bridge_channelConfig\"]')"
        ),
    )
    page.wait_for_timeout(600)  # prefs debounce
    restart(page, wait_js="() => __cockpit.store.gate === 'matched'", timeout=8000)
    page.wait_for_selector('.picker-group[data-group="IO_bridge_channelConfig"]')
    check(
        "picker collapsed set persists across restart",
        page.evaluate(
            "() => !!document.querySelector('.picker-group--collapsed[data-group=\"IO_bridge_channelConfig\"]')"
        ),
    )
    page.click('.picker-group[data-group="IO_bridge_channelConfig"]')  # expand again

    # ── collapse-all / expand-all (chrome ergonomics, unspecced — untagged) ──
    page.fill(".picker-search input", "phase")
    page.wait_for_function("() => document.querySelector('.picker-results').textContent !== ''")
    page.click("[data-collapseall]")
    check(
        "picker collapse-all collapses every presented group (during search)",
        page.evaluate(
            "() => !document.querySelector('.signal-row')"
            " && document.querySelectorAll('.picker-group--collapsed').length"
            "    === document.querySelectorAll('.picker-group').length"
            " && document.querySelector('[data-collapseall]').textContent === 'expand all'"
        ),
    )
    page.fill(".picker-search input", "*")  # match-all glob clears the narrowing
    page.wait_for_function("() => document.querySelectorAll('.picker-group').length > 2")
    page.click("[data-collapseall]")  # not everything collapsed yet → collapse all
    page.wait_for_function(
        "() => document.querySelector('[data-collapseall]').textContent === 'expand all'"
    )
    page.click('.picker-group[data-group="IO_bridge_channelConfig"]')
    check(
        "picker per-group expand composes; the button returns to collapse all",
        page.evaluate(
            "() => document.querySelectorAll('.signal-row').length === 5"
            " && document.querySelector('[data-collapseall]').textContent === 'collapse all'"
        ),
    )
    page.click("[data-collapseall]")  # collapse the reopened group again
    page.click("[data-collapseall]")  # all collapsed now → expand-all restores
    check(
        "picker expand-all restores every group",
        page.evaluate(
            "() => document.querySelectorAll('.picker-group--collapsed').length === 0"
            " && document.querySelectorAll('.signal-row').length > 20"
            " && document.querySelector('[data-collapseall]').textContent === 'collapse all'"
        ),
    )

    # ── [test->app~views_011~1] explicit solid beats the overflow dash ──
    fresh_boot(page)
    seven = [
        "app_motorControl_data.channels[0].velocityMeasured_radPerSec",
        "app_motorControl_data.channels[0].velocitySetpointCurrent_radPerSec",
        "app_motorControl_data.channels[0].mechanicalAngle_rad",
        "app_motorControl_data.channels[0].busCurrent",
        "app_motorControl_data.channels[0].duty[0]",
        "app_motorControl_data.channels[0].duty[1]",
        "task1msRuns",
    ]
    add_plot_with(page, seven, 100)
    dash_state = (
        "() => { let d = null; __cockpit.forEachWidget(w => { if (!w.traceInfo) return;"
        "  if (!w.cfg.signals.includes('task1msRuns')) return;"
        "  const t = w.traceInfo('task1msRuns');"
        "  d = Boolean(t.dash && t.dash.length); }); return d; }"
    )
    check("views_011 the 7th slot dashes by default (overflow)", page.evaluate(dash_state) is True)
    page.click(".plot-widget [data-axesconfig]")
    # The appearance editor renders per-row, expanded via the row's swatch.
    page.click('.axes-popover .axes-row[data-path="task1msRuns"] [data-appearance]')
    page.wait_for_selector(".axes-appearance")
    seg_selected = page.evaluate(
        """() => { const ed = [...document.querySelectorAll('.axes-appearance')]
             .find(e => e.dataset.path === 'task1msRuns');
           return ed?.querySelector('[data-style].is-selected')?.dataset.style; }"""
    )
    check("views_011 popover style seg shows the effective (dashed) style", seg_selected == "dashed", seg_selected)
    page.keyboard.press("Escape")
    page.evaluate("() => __cockpit.appearance.set('task1msRuns', { style: 'solid' })")
    page.wait_for_function(f"() => ({dash_state})() === false")
    check("views_011 explicit solid overrides the overflow dash", True)
    page.wait_for_timeout(600)
    restart(page, load_elf=True, wait_js="() => document.querySelectorAll('.plot-widget').length > 0")
    check("views_011 explicit solid survives restart", page.evaluate(dash_state) is False)

    # ── [test->app~arch_002~1] session restore: cold boot, full return ──
    page.evaluate("() => localStorage.clear()")
    coldboot = URL.replace("state=matched", "state=coldboot")
    page.goto(coldboot)
    page.wait_for_function("() => window.__cockpit !== undefined")
    page.wait_for_function("() => __cockpit.store.ports.length > 0")
    check(
        "arch_002 cold boot without saved context lands in the manual state",
        page.evaluate("() => __cockpit.store.connection.state") == "disconnected"
        and page.locator(".conn-picker").count() == 1,
    )
    page.evaluate("() => __cockpit.api.connect('COM8')")
    page.wait_for_function("() => __cockpit.store.connection.state === 'connected'")
    page.evaluate("() => __cockpit.api.loadElf('mock.elf')")
    page.wait_for_function("() => __cockpit.store.gate === 'matched'")
    page.evaluate("() => __cockpit.api.listSignals('')")
    page.wait_for_function("() => __cockpit.store.signals.length > 0")
    page.evaluate("() => __cockpit.addWatch('task1msRuns', 10)")
    page.wait_for_function("() => (window.__devmockInstalls || []).length >= 1", timeout=5000)
    page.wait_for_timeout(600)  # prefs debounce
    page.goto(coldboot)
    page.wait_for_function("() => window.__cockpit !== undefined")
    page.wait_for_function(
        "() => __cockpit.store.gate === 'matched'"
        " && __cockpit.store.watched.has('task1msRuns')"
        " && (window.__devmockInstalls || []).length >= 1",
        timeout=8000,
    )
    check(
        "arch_002 relaunch reconnects, reloads, and reinstalls without user action",
        page.evaluate("() => __cockpit.store.connection.port") == "COM8"
        and page.evaluate("() => Boolean(__cockpit.store.elf.buildId)"),
    )

    # ── [test->app~arch_002~1] fallbacks: absent port / unreadable ELF ──
    page.goto(URL.replace("state=matched", "state=coldboot-noport"))
    page.wait_for_function("() => window.__cockpit !== undefined")
    page.wait_for_function("() => Boolean(__cockpit.store.elf.buildId)", timeout=8000)
    check(
        "arch_002 saved port absent: port selection offered, ELF restored",
        page.evaluate("() => __cockpit.store.connection.state") == "disconnected"
        and page.locator(".conn-picker").count() == 1,
    )
    page.goto(URL.replace("state=matched", "state=badelf"))
    page.wait_for_function("() => window.__cockpit !== undefined")
    page.wait_for_function("() => __cockpit.store.connection.state === 'connected'", timeout=8000)
    check(
        "arch_002 saved ELF unreadable: load action offered, port connected",
        page.evaluate("() => __cockpit.store.gate") == "no-elf",
    )

    # ── view prefs (theme, log collapse) reapply — unspecced chrome, no tag ──
    page.goto(URL)
    page.wait_for_function("() => window.__cockpit !== undefined")
    page.click('[data-theme-pick="graphite"]')
    page.click('[data-log="collapse"]')
    page.wait_for_timeout(600)
    page.reload()
    page.wait_for_function("() => window.__cockpit !== undefined")
    check(
        "view prefs (theme, log collapse) reapply at launch",
        page.evaluate("() => document.documentElement.dataset.theme") == "graphite"
        and page.locator(".log-pane--collapsed").count() == 1,
    )
    page.click('[data-theme-pick="warm"]')
    page.click('[data-log="collapse"]')

    # ═══ batch 5 (#22): the free layout canvas — snap, stack, raise, migrate ═══

    fresh_boot(page)
    add_plot_with(page, ["task1msRuns"])
    page.wait_for_selector(".plot-widget")

    # ── [test->app~views_004~1] positionless widgets stack vertically ──
    page.evaluate("() => __cockpit.addWidget({ type: 'plot', signals: [] })")
    geo = page.evaluate(
        "() => { const g = []; __cockpit.forEachWidget(w =>"
        " g.push({ x: w.cfg.x, y: w.cfg.y, w: w.cfg.w, h: w.cfg.h })); return g; }"
    )
    check(
        "views_004 default placement stacks vertically on the lattice",
        len(geo) == 2
        and geo[0]["x"] == geo[1]["x"]
        and geo[1]["y"] >= geo[0]["y"] + geo[0]["h"]
        and all(v % 50 == 0 for g in geo for v in g.values()),
        geo,
    )

    # ── [test->app~views_004~1] header drag moves anywhere, snapped 50 px ──
    ids = page.evaluate(
        "() => { const out = []; __cockpit.forEachWidget(w => out.push(w.cfg.id)); return out; }"
    )
    wid_a, wid_b = ids[0], ids[1]
    head = page.locator(f"[data-widget-id='{wid_b}'] .widget-head").bounding_box()
    page.mouse.move(head["x"] + head["width"] / 2, head["y"] + 10)
    page.mouse.down()
    page.mouse.move(head["x"] + head["width"] / 2 + 137, head["y"] + 10 - 213, steps=6)
    page.mouse.up()
    moved = widget_eval(page, wid_b, "(w) => ({ x: w.cfg.x, y: w.cfg.y })")
    check(
        "views_004 header drag moves the widget onto the lattice",
        moved == {"x": geo[1]["x"] + 150, "y": geo[1]["y"] - 200},
        (geo[1], moved),
    )

    # ── [test->app~views_004~1] widgets overlap; pointerdown raises to front ──
    grid_box = page.locator(".widget-grid").bounding_box()
    overlap_owner = (
        "() => { const g = document.querySelector('.widget-grid');"
        " const r = g.getBoundingClientRect();"
        " const el = document.elementFromPoint(r.left + 400 - g.scrollLeft, r.top + 300 - g.scrollTop);"
        " return el?.closest('.widget')?.dataset.widgetId || null; }"
    )
    check(
        "views_004 overlap region belongs to the raised (dragged) widget",
        page.evaluate(overlap_owner) == wid_b,
        page.evaluate(overlap_owner),
    )
    # Click widget A's visible head strip (above B's top edge) to raise it.
    page.mouse.move(grid_box["x"] + 400, grid_box["y"] + 70)
    page.mouse.down()
    page.mouse.up()
    check(
        "views_004 pointerdown raises the back widget to the front",
        page.evaluate(overlap_owner) == wid_a,
        page.evaluate(overlap_owner),
    )

    # ── [test->app~views_004~1] corner resize snaps BOTH extents to 50 px ──
    hb = page.locator(f"[data-widget-id='{wid_a}'] .resize-handle").bounding_box()
    page.mouse.move(hb["x"] + 6, hb["y"] + 6)
    page.mouse.down()
    page.mouse.move(hb["x"] + 6 + 63, hb["y"] + 6 + 88, steps=4)
    page.mouse.up()
    sized = widget_eval(page, wid_a, "(w) => ({ w: w.cfg.w, h: w.cfg.h })")
    check(
        "views_004 corner resize snaps width and height to the lattice",
        sized == {"w": geo[0]["w"] + 50, "h": geo[0]["h"] + 100},
        (geo[0], sized),
    )

    # ── [test->app~views_004~1] restart restores geometry + stacking exactly ──
    geometry_js = (
        "() => { const g = []; __cockpit.forEachWidget(w => g.push({ id: w.cfg.id,"
        " x: w.cfg.x, y: w.cfg.y, w: w.cfg.w, h: w.cfg.h, z: w.el.style.zIndex })); return g; }"
    )
    geo_before = page.evaluate(geometry_js)
    restart(page, load_elf=True, wait_js="() => document.querySelectorAll('.widget').length === 2")
    geo_after = page.evaluate(geometry_js)
    check(
        "views_004 restart restores positions, sizes, and stacking",
        geo_before == geo_after,
        (geo_before, geo_after),
    )

    # ── [test->app~views_004~1] flow-grid snapshots migrate to the canvas ──
    page.evaluate(
        """() => { localStorage.clear();
          localStorage.setItem('cockpit.config.v1', JSON.stringify({
            'cockpit.workspace.v1': {
              widgets: [
                { type: 'plot', id: 'w1', signals: [], w: 2, hpx: 340 },
                { type: 'table', id: 'w2', signals: [], w: 1, hpx: 263 },
                { type: 'plot', id: 'w3', signals: [], x: 100, y: 50, w: 800, h: 350 },
              ],
              watched: [], colors: null, appearance: null } })); }"""
    )
    restart(page, wait_js="() => document.querySelectorAll('.widget').length === 3")
    mig = page.evaluate(
        "() => { const g = []; __cockpit.forEachWidget(w => g.push({ id: w.cfg.id,"
        " x: w.cfg.x, y: w.cfg.y, w: w.cfg.w, h: w.cfg.h, hpx: w.cfg.hpx ?? null })); return g; }"
    )
    check(
        "views_004 flow cfgs migrate to the stack; canvas cfgs pass through",
        mig == [
            {"id": "w1", "x": 50, "y": 50, "w": 900, "h": 350, "hpx": None},
            {"id": "w2", "x": 50, "y": 450, "w": 450, "h": 250, "hpx": None},
            {"id": "w3", "x": 100, "y": 50, "w": 800, "h": 350, "hpx": None},
        ],
        mig,
    )
    rewritten = page.evaluate(
        "() => JSON.parse(localStorage.getItem('cockpit.config.v1'))['cockpit.workspace.v1'].widgets"
    )
    check(
        "views_004 migrated snapshot persists in the canvas shape",
        all(("x" in w) and ("hpx" not in w) for w in rewritten),
        rewritten,
    )

    # ═══ batch 6 (#23): enum string display — the value rendering table ═══

    fresh_boot(page)

    # ── [test->app~views_013~1] the rendering table, row by row ──
    fv = page.evaluate(
        """async () => {
          const { formatValue } = await import('./js/workspace/plotwidget.js');
          const enums = [[0, 'IDLE'], [2, 'FAULT']];
          return {
            named: formatValue(2, 'enum', enums),
            unnamed: formatValue(1, 'enum', enums),   // value with no enumerator
            bare: formatValue(1, 'enum', undefined),  // no enumerators resolved
            rounded: formatValue(1.9, 'enum', enums), // decoded floats round first
            boolT: formatValue(1, 'bool'),
            boolF: formatValue(0, 'bool'),
            integer: formatValue(42.0, 'u32'),
            floatSmall: formatValue(3.14159, 'f32'),
            floatBig: formatValue(1234.5678, 'f32'),
          }; }"""
    )
    check(
        "views_013 enum value with a resolved name renders the name",
        fv["named"] == "FAULT" and fv["rounded"] == "FAULT",
        fv,
    )
    check(
        "views_013 enum value with no enumerator renders the whole number",
        fv["unnamed"] == "1" and fv["bare"] == "1",
        fv,
    )
    check(
        "views_013 bool/integer/float rows render per the table",
        fv["boolT"] == "true"
        and fv["boolF"] == "false"
        and fv["integer"] == "42"
        and fv["floatSmall"] == "3.1416"
        and fv["floatBig"] == "1234.6",
        fv,
    )

    # ── [test->app~views_013~1] the name renders in all three surfaces ──
    enum_sig = "IO_AS5048_data.channels[0].status"
    add_plot_with(page, [enum_sig], period=10)
    wait_for_samples(page, enum_sig, 5)
    # Watch panel row (views_010's surface); the cell refreshes at batch rate,
    # so wait for the next refresh rather than racing it.
    page.wait_for_function(
        f"""() => (document.querySelector(".watch-row[data-path='{enum_sig}'] [data-value]")
          ?.textContent || "").startsWith("AS5048_")""",
        timeout=5000,
    )
    panel_val = page.locator(
        f".watch-row[data-path='{enum_sig}'] [data-value]"
    ).inner_text()
    check(
        "views_013 watch panel renders the enumerator name",
        panel_val.startswith("AS5048_"),
        panel_val,
    )
    # Value table cell (views_006's surface), keeping its enum accent class.
    page.evaluate(
        f"""() => {{ const t = __cockpit.addWidget({{ type: 'table', signals: [] }});
          t.addSignal({enum_sig!r}); }}"""
    )
    page.evaluate("() => __cockpit.forEachWidget(w => w.refresh())")
    cell = page.evaluate(
        """() => { const td = document.querySelector('.value-table td.col-value');
          return { text: td.textContent, enumClass: td.classList.contains('value--enum') }; }"""
    )
    check(
        "views_013 value table renders the enumerator name with its accent",
        cell["text"].startswith("AS5048_") and cell["enumClass"],
        cell,
    )
    # Cursor readout (views_005's surface), at the newest sampled tick — on
    # the widget that HOLDS the enum signal, waiting for its row to render.
    # (First-match readout sampling was order/timing-flaky: which plot sits
    # first in the DOM depends on suite history, and reading between batch
    # re-renders raced the row's content.)
    enum_wid = page.evaluate(
        f"""() => {{ let id = null; __cockpit.forEachWidget(w => {{
          if (w.renderedTables && w.cfg.signals.includes({enum_sig!r})) id = w.cfg.id; }});
          return id; }}"""
    )
    check("views_013 a plot holds the enum signal", enum_wid is not None, enum_wid)
    page.evaluate(
        f"() => __cockpit.setCursorTick(__cockpit.histories.get({enum_sig!r}).newestTick())"
    )
    page.wait_for_function(
        f"""() => (document.querySelector(
          "[data-widget-id='{enum_wid}'] .cursor-readout .readout-row[data-path='{enum_sig}'] .readout-value")
          ?.textContent || "").startsWith("AS5048_")""",
        timeout=5000,
    )
    readout = page.locator(f"[data-widget-id='{enum_wid}'] .cursor-readout").inner_text()
    check(
        "views_013 cursor readout renders the enumerator name",
        "AS5048_" in readout,
        readout[:120],
    )
    page.evaluate("() => __cockpit.clearCursor()")

    # ── [test->app~views_013~1] enumerators survive restart via the snapshot ──
    page.wait_for_timeout(700)  # let the watched-list persist debounce settle
    restart(page)
    boot(page)
    page.wait_for_function(
        f"() => (__cockpit.histories.get({enum_sig!r})?.ticks.length || 0) >= 3",
        timeout=8000,
    )
    panel_val = page.locator(
        f".watch-row[data-path='{enum_sig}'] [data-value]"
    ).inner_text()
    check(
        "views_013 restored watch renders the name from the snapshot's enumerators",
        panel_val.startswith("AS5048_"),
        panel_val,
    )

    # ── [test->app~views_013~1] a pre-enum snapshot heals on connect: the
    #    watched entry carries kind but NO enumerators, and the arriving
    #    signal list refreshes the meta so names appear ──
    page.evaluate(
        f"""() => {{ localStorage.clear();
          localStorage.setItem('cockpit.config.v1', JSON.stringify({{
            'cockpit.workspace.v1': {{
              widgets: [],
              watched: [{{ path: {enum_sig!r}, period_ms: 10, size: 1, kind: 'enum' }}],
              colors: null, appearance: null }} }})); }}"""
    )
    restart(page)
    boot(page)
    page.wait_for_function(
        f"""() => (document.querySelector(".watch-row[data-path='{enum_sig}'] [data-value]")
          ?.textContent || "").startsWith("AS5048_")""",
        timeout=8000,
    )
    healed = page.locator(
        f".watch-row[data-path='{enum_sig}'] [data-value]"
    ).inner_text()
    check(
        "views_013 enum-less old snapshot heals when the signal list arrives",
        healed.startswith("AS5048_"),
        healed,
    )


    # ═══ batch 7 (#21): perf round — FPS readout, trace decimation, culling ═══

    # ── the render-perf cell (unspecced chrome) reports real frames ──
    page.wait_for_function(
        "() => /fps/.test(document.querySelector('[data-perf-cell]')?.textContent || '')",
        timeout=5000,
    )
    check(
        "perf cell reports fps and worst frame",
        "worst" in page.locator("[data-perf-cell]").inner_text(),
        page.locator("[data-perf-cell]").inner_text(),
    )

    # ── [test->app~views_014~1] dense trace: ≤2 rendered points per pixel
    #    column, every column's extent equal to the raw extent, a
    #    single-sample spike preserved ──
    dense_sig = "task1msRuns"
    page.evaluate(f"() => __cockpit.addWatch({dense_sig!r}, 1)")
    dense_id = page.evaluate(
        f"() => {{ const w = __cockpit.addWidget({{ type: 'plot', signals: [] }});"
        f"  w.addSignal({dense_sig!r}); return w.cfg.id; }}"
    )
    # Deterministic dense history: 10 000 samples @1 ms seeded AT the live
    # edge (the devmock stream keeps moving globalNewest, so the seed anchors
    # to it and the live window lands exactly on the synthetic span). One
    # spike at base+6000.
    base = page.evaluate(
        f"""() => {{
          const newest = [...__cockpit.histories.values()]
            .reduce((n, h) => Math.max(n, h.newestTick() ?? 0), 0);
          const base = Math.ceil(newest) + 1;
          const h = __cockpit.histories.get({dense_sig!r});
          h.clear();
          const pts = [];
          for (let t = 0; t <= 9999; t++) {{
            pts.push([base + t, t === 6000 ? 99 : Math.sin(t / 37)]);
          }}
          h.append(pts);
          __cockpit.timeline.setSpan(10000);
          __cockpit.forEachWidget(w => w.refresh());
          return base;
        }}"""
    )
    dec = page.evaluate(
        f"""() => {{
          let out = null;
          __cockpit.forEachWidget(w => {{
            if (w.cfg.id !== {dense_id!r} || !w.renderedTables) return;
            const [xs, ys] = w.renderedTables().get({dense_sig!r});
            const host = w.el.querySelector('.trace-host');
            const cols = Math.round(host.getBoundingClientRect().width *
                                    (window.devicePixelRatio || 1));
            const [t0, t1] = w.window;
            const colMs = (t1 - t0) / cols;
            const perCol = new Map();
            let n = 0, min = Infinity, max = -Infinity, spike = false;
            for (let i = 0; i < xs.length; i++) {{
              const v = ys[i];
              if (v === null || v === undefined) continue;
              n++;
              const c = Math.floor((xs[i] - t0) / colMs);
              perCol.set(c, (perCol.get(c) || 0) + 1);
              if (v < min) min = v;
              if (v > max) max = v;
              if (v === 99) spike = true;
            }}
            out = {{ n, worstCol: Math.max(...perCol.values()), min, max, spike, cols,
                     drew: Boolean(w.gl && w.gl.drawCount > 0 && w.gl.alive()) }};
          }});
          return out;
        }}"""
    )
    check(
        "views_014 dense trace renders at most two points per pixel column",
        dec and dec["worstCol"] <= 2 and dec["n"] < 10000 and dec["drew"],
        dec,
    )
    check(
        "views_014 decimated extent equals the raw extent, spike preserved",
        dec and dec["spike"] and dec["max"] == 99 and abs(dec["min"] - (-1)) < 0.01,
        dec,
    )

    # ── [test->app~views_014~1] paused zoom below two samples per column
    #    renders every raw sample ──
    page.evaluate("() => __cockpit.timeline.pause()")
    page.evaluate(f"() => __cockpit.timeline.selectRange({base} + 5900, {base} + 6100)")
    sparse = page.evaluate(
        f"""() => {{
          const lo = {base} + 5900, hi = {base} + 6100;
          let out = null;
          __cockpit.forEachWidget(w => {{
            if (w.cfg.id !== {dense_id!r} || !w.renderedTables) return;
            const [xs, ys] = w.renderedTables().get({dense_sig!r});
            let n = 0, spike = false;
            for (let i = 0; i < xs.length; i++) {{
              if (ys[i] === null || ys[i] === undefined) continue;
              if (xs[i] >= lo && xs[i] <= hi) n++;
              if (ys[i] === 99) spike = true;
            }}
            const h = __cockpit.histories.get({dense_sig!r});
            let raw = 0;
            for (const t of h.ticks) if (t >= lo && t <= hi) raw++;
            out = {{ n, raw, spike }};
          }});
          return out;
        }}"""
    )
    check(
        "views_014 paused sparse zoom renders every raw sample",
        sparse and sparse["n"] == sparse["raw"] and sparse["spike"],
        sparse,
    )
    page.evaluate("() => __cockpit.timeline.resume()")

    # ── [test->app~views_014~1] non-finite samples never poison a column's
    #    extent: NaNs are skipped for min/max, an all-NaN dense column emits
    #    nothing, and the finite extent (a spike beside NaNs) is preserved ──
    nan_dec = page.evaluate(
        """async () => {
          const { decimateTable } = await import('./js/workspace/decimate.js');
          const xs = [], ys = [];
          for (let t = 0; t < 1000; t++) {
            xs.push(t);
            let v = Math.sin(t / 9);
            if (t >= 50 && t <= 54) v = NaN;     // NaNs beside the spike
            if (t === 55) v = 42;                 // global max
            if (t >= 200 && t <= 209) v = NaN;    // one all-NaN column
            ys.push(v);
          }
          const [oxs, oys] = decimateTable(xs, ys, 0, 1000, 100); // 10/column
          const finite = oys.filter(v => v !== null && Number.isFinite(v));
          const rawFiniteMin = Math.min(...ys.filter(Number.isFinite));
          return {
            hasNaN: oys.some(v => v !== null && !Number.isFinite(v)),
            max: Math.max(...finite),
            minOk: Math.abs(Math.min(...finite) - rawFiniteMin) < 1e-12,
            nanColEmpty: !oxs.some((x, i) => x >= 200 && x < 210),
            bounded: finite.length <= 200,
          };
        }"""
    )
    check(
        "views_014 NaN samples don't poison the rendered extent",
        nan_dec
        and not nan_dec["hasNaN"]
        and nan_dec["max"] == 42
        and nan_dec["minOk"]
        and nan_dec["nanColEmpty"]
        and nan_dec["bounded"],
        nan_dec,
    )

    # cleanup: strip the probe signal so the final checks see a stable canvas
    page.evaluate(
        f"""() => {{
          let target = null;
          __cockpit.forEachWidget(w => {{ if (w.cfg.id === {dense_id!r}) target = w; }});
          if (target) target.cfg.signals.slice().forEach(p => target.removeSignal(p));
          __cockpit.removeWatch({dense_sig!r});
        }}"""
    )

    # ═══ batch 8: WebGL renderer plumbing ═══

    # ── GL context loss/restore: the widget rebuilds its programs and
    #    redraws from the CPU-side tables; nothing else notices ──
    survived = page.evaluate(
        """() => new Promise((resolve) => {
          let w0 = null;
          __cockpit.forEachWidget(w => { if (!w0 && w.gl) w0 = w; });
          if (!w0) { resolve({ ok: false, why: 'no gl widget' }); return; }
          const glt = w0.gl;
          const ext = glt.gl.getExtension('WEBGL_lose_context');
          const before = glt.drawCount;
          ext.loseContext();
          setTimeout(() => {
            ext.restoreContext();
            setTimeout(() => resolve({
              ok: glt.contextLosses === 1 && glt.drawCount > before,
              losses: glt.contextLosses, before, after: glt.drawCount,
            }), 300);
          }, 100);
        })"""
    )
    check("glrender context loss restores and redraws", survived["ok"], survived)

    # ── a plot created while PAUSED must draw immediately (attach-time
    #    refresh + paused self-heal), and hovering it must still move the
    #    app-wide shared cursor ──
    paused_add = page.evaluate(
        """() => {
          __cockpit.timeline.pause();
          const p = [...__cockpit.store.watched.keys()][0];
          const w = __cockpit.addWidget({ type: 'plot', signals: [] });
          w.addSignal(p);
          const out = {
            drew: Boolean(w.gl && w.gl.alive() && w.gl.drawCount > 0),
            axes: w.el.querySelector('.y-axis--left').children.length > 0,
            id: w.cfg.id,
          };
          // Shared cursor from a renderer-independent path: simulate a hover.
          const canvas = w.el.querySelector('.plot-canvas');
          const r = canvas.getBoundingClientRect();
          canvas.dispatchEvent(new PointerEvent('pointermove',
            { clientX: r.left + r.width / 2, clientY: r.top + 10, bubbles: true }));
          out.cursorSet = __cockpit.cursor.tick !== null;
          __cockpit.timeline.resume();
          const path = w.cfg.signals[0];
          w.removeSignal(path);
          __cockpit.forEachWidget(x => { if (x.cfg.id === out.id) x.hooks.onRemove(out.id); });
          return out;
        }"""
    )
    check(
        "views_004 plot added while paused draws at once, cursor unaffected",
        paused_add["drew"] and paused_add["axes"] and paused_add["cursorSet"],
        paused_add,
    )

    # ── non-finite samples break a dashed run instead of poisoning its arc:
    #    geometry on both sides of a NaN, no NaN in any vertex buffer,
    #    per-side arcs restarting at 0 ──
    nan_geo = page.evaluate(
        """async () => {
          const { buildTraceGeometry } = await import('./js/workspace/glrender.js');
          const xs = [], ys = [];
          for (let t = 0; t <= 100; t++) { xs.push(t); ys.push(t === 50 ? NaN : Math.sin(t / 5)); }
          const geo = buildTraceGeometry(xs, ys, x => x * 4, y => 50 - y * 20, 'linear', 1, true);
          const arcsOk = geo.runs.every(r => r.verts[2] === 0);
          let hasNaN = false;
          for (const r of geo.runs) for (const v of r.verts) if (!Number.isFinite(v)) hasNaN = true;
          for (const v of geo.dots) if (!Number.isFinite(v)) hasNaN = true;
          return { runs: geo.runs.length,
                   left: geo.runs[0]?.verts.length / 3, right: geo.runs[1]?.verts.length / 3,
                   arcsOk, hasNaN, dots: geo.dots.length / 2 };
        }"""
    )
    check(
        "glrender NaN splits the run — geometry on both sides, clean arcs",
        nan_geo["runs"] == 2 and nan_geo["left"] == 50 and nan_geo["right"] == 50
        and nan_geo["arcsOk"] and not nan_geo["hasNaN"] and nan_geo["dots"] == 100,
        nan_geo,
    )

    # ── context budget: 18 live plots exceed the browser's context cap; the
    #    LRU release + on-demand re-creation must leave every widget able to
    #    draw (no permanently blank plot) ──
    eviction = page.evaluate(
        """() => new Promise((resolve) => {
          const p = [...__cockpit.store.watched.keys()][0];
          const made = [];
          for (let i = 0; i < 18; i++) {
            const w = __cockpit.addWidget({ type: 'plot', signals: [] });
            w.addSignal(p);
            made.push(w);
          }
          // Interact with each (a refresh — scrub/hover would do the same
          // on the bench) and verify it drew during THIS interaction.
          setTimeout(() => {
            let allDrew = true;
            for (const w of made) {
              // refresh() replaces an evicted renderer wholesale, so judge
              // the CURRENT instance: alive and drawn since (re-)creation.
              w.refresh();
              if (!(w.gl && w.gl.alive() && w.gl.drawCount > 0)) allDrew = false;
            }
            const ids = made.map(w => w.cfg.id);
            for (const id of ids) {
              __cockpit.forEachWidget(w => { if (w.cfg.id === id) w.cfg.signals.slice().forEach(s => w.removeSignal(s)); });
            }
            resolve({ allDrew, n: made.length });
          }, 300);
        })"""
    )
    check(
        "glrender 18 plots: LRU context budget leaves no permanent blank",
        eviction["allDrew"] and eviction["n"] == 18,
        eviction,
    )
    # drop the 18 scratch widgets so the tail checks see a stable canvas
    page.evaluate(
        """() => { const gone = [];
          __cockpit.forEachWidget(w => { if (!w.cfg.signals.length) gone.push(w.cfg.id); });
          for (const id of gone) { let t = null;
            __cockpit.forEachWidget(w => { if (w.cfg.id === id) t = w; });
            if (t) t.hooks.onRemove(id); } }"""
    )

    # ═══ batch 9: widget titles (app~views_016) ═══════════════════════════

    # ── [test->app~views_016~1] unset titles derive per the widget-state
    #    table; removing the earliest-added signal moves the title ──
    page.evaluate("() => __cockpit.addWatch('task1msRuns', 10)")
    page.evaluate("() => __cockpit.addWatch('serverRuns', 10)")
    titles = page.evaluate(
        """() => {
          const read = (w) => w.el.querySelector('.widget-title').textContent;
          const p = __cockpit.addWidget({ type: 'plot', signals: [] });
          const t = __cockpit.addWidget({ type: 'table', signals: [] });
          const out = { empty: read(p), table: read(t) };
          p.addSignal('task1msRuns');
          p.addSignal('serverRuns');
          out.withSignals = read(p);
          p.removeSignal('task1msRuns');       // the earliest-added leaves
          out.afterRemove = read(p);
          window.__titleWid = p.cfg.id;
          t.hooks.onRemove(t.cfg.id);          // scratch table leaves
          return out;
        }"""
    )
    check(
        "views_016 unset titles derive per the widget-state table",
        titles["empty"] == "Plot"
        and titles["table"] == "Live values"
        and titles["withSignals"] == "task1msRuns",
        titles,
    )
    check(
        "views_016 removing the earliest-added signal moves the title",
        titles["afterRemove"] == "serverRuns",
        titles,
    )

    # ── [test->app~views_016~1] a committed name replaces the unset title
    #    and survives restart ──
    twid = page.evaluate("() => window.__titleWid")
    title_span = page.locator(f"[data-widget-id='{twid}'] .widget-title")
    title_span.evaluate("el => el.scrollIntoView({ block: 'center' })")
    title_span.click()
    page.locator(f"[data-widget-id='{twid}'] .widget-title-edit").fill("Motor speeds")
    page.keyboard.press("Enter")
    named = title_span.inner_text()
    check("views_016 a committed name replaces the unset title", named == "Motor speeds", named)
    page.wait_for_timeout(700)  # let the persist debounce settle
    restart(page)
    boot(page)
    page.wait_for_selector(f"[data-widget-id='{twid}'] .widget-title")
    restored = page.locator(f"[data-widget-id='{twid}'] .widget-title").inner_text()
    check("views_016 a set title survives restart", restored == "Motor speeds", restored)

    # ── [test->app~views_016~1] a committed empty name returns the unset title ──
    title_span = page.locator(f"[data-widget-id='{twid}'] .widget-title")
    title_span.evaluate("el => el.scrollIntoView({ block: 'center' })")
    title_span.click()
    page.locator(f"[data-widget-id='{twid}'] .widget-title-edit").fill("")
    page.keyboard.press("Enter")
    unset = title_span.inner_text()
    check(
        "views_016 a committed empty name returns the unset title",
        unset == "serverRuns",
        unset,
    )

    # ── [test->app~views_016~1] the header still drags the widget after an
    #    edit session (the title's drag exclusion must not eat the head) ──
    head_loc = page.locator(f"[data-widget-id='{twid}'] .widget-head")
    head_loc.evaluate("el => el.scrollIntoView({ block: 'center' })")
    geo0 = widget_eval(page, twid, "(w) => ({ x: w.cfg.x, y: w.cfg.y })")
    head = head_loc.bounding_box()
    page.mouse.move(head["x"] + head["width"] * 0.75, head["y"] + 8)
    page.mouse.down()
    page.mouse.move(head["x"] + head["width"] * 0.75 + 107, head["y"] + 8 + 63, steps=5)
    page.mouse.up()
    geo1 = widget_eval(page, twid, "(w) => ({ x: w.cfg.x, y: w.cfg.y })")
    check(
        "views_016 header drag still moves the widget after an edit",
        geo1 == {"x": geo0["x"] + 100, "y": geo0["y"] + 50},
        (geo0, geo1),
    )

    # ═══ batch 10: comparison cursor (views_017 anchor + views_018 deltas) ═══

    VEL_M = "app_motorControl_data.channels[0].velocityMeasured_radPerSec"
    VEL_S = "app_motorControl_data.channels[0].velocitySetpointCurrent_radPerSec"
    ENUM_S = "IO_AS5048_data.channels[0].status"
    U32_S = "task1msRuns"
    for p in (VEL_M, VEL_S, ENUM_S, U32_S):
        page.evaluate(f"() => __cockpit.addWatch({p!r}, 1)")
        wait_for_samples(page, p)
    # The gap-gate check needs a mock gap window (every 5 s of ticks) inside
    # the paused view: stream past one before pausing (~5 s of wall time).
    page.wait_for_function(
        f"() => (__cockpit.histories.get({VEL_S!r})?.ticks.length || 0) >= 5200",
        timeout=15000,
    )
    cwid = page.evaluate(
        """(paths) => {
          const w = __cockpit.addWidget({ type: 'plot', signals: [] });
          for (const p of paths) w.addSignal(p);
          w.setSide(paths[3], 'R');   // the u32 counter gets the right axis
          return w.cfg.id;
        }""",
        [VEL_M, VEL_S, ENUM_S, U32_S],
    )
    page.evaluate("() => __cockpit.timeline.pause()")

    def cwidget_eval(expr, arg=None):
        """widget_eval against the comparison widget."""
        return widget_eval(page, cwid, expr, arg)

    def probe_target(path, frac):
        """A real-mouse point 6 px under `path`'s trace at window fraction
        `frac`, only where every other trace is at least 22 px away (the
        pointed rule must resolve to `path`), or None."""
        return cwidget_eval(
            """(w, [path, frac]) => {
              w.el.scrollIntoView({ block: 'center' });
              w.refresh();
              const canvas = w.el.querySelector('.plot-canvas');
              const rect = canvas.getBoundingClientRect();
              const px = rect.width * frac;
              const t = w.tickAtPx(px, rect.width);
              """ + NEAREST_Y_JS + """
              const yOf = (p) => nearestY(w, p, t, rect);
              const yT = yOf(path);
              if (yT === null) return null;
              for (const p of w.cfg.signals) {
                if (p === path) continue;
                const y = yOf(p);
                if (y !== null && Math.abs(y - yT) < 22) return null;
              }
              const cy = Math.min(rect.height - 3, Math.max(3, yT + 6));
              const at = [rect.left + px, rect.top + cy];
              if (document.elementFromPoint(...at)?.closest('.plot-canvas') !== canvas) return null;
              // The exact resolver the click will run: only a target the
              // pointed rule already resolves to `path` is a valid probe
              // (nearest-sample y and interpolated y can diverge).
              if (w.computePointed(at[0], at[1], canvas) !== path) return null;
              return { at, clickTick: t };
            }""",
            [path, frac],
        )

    def find_target(path):
        # Bounded retry: on a starved host the fraction sweep can run
        # before the paused plot's geometry settles (run-5 CI returned
        # None once); healthy hosts resolve on the first round.
        for _ in range(10):
            for frac in (0.5, 0.35, 0.65, 0.25, 0.75, 0.45, 0.55):
                tgt = probe_target(path, frac)
                if tgt:
                    return tgt
            page.wait_for_timeout(500)
        return None

    def require_target(path, label):
        # check() + never-None: exhausting the retry sweep fails the probe
        # check but returns a harmless fallback (a workspace-center click
        # that points no trace), so dependent checks fail on their own
        # terms instead of a NoneType subscript aborting the whole suite.
        tgt = find_target(path)
        check(label, tgt is not None, tgt)
        if tgt is None:
            ws = page.evaluate(
                "() => { const r = document.querySelector('.workspace').getBoundingClientRect();"
                " return [r.x + r.width / 2, r.y + r.height / 2]; }"
            )
            return {"at": tuple(ws), "clickTick": -(10**9)}
        return tgt

    def ctrl_click(at):
        page.keyboard.down("Control")
        page.mouse.click(*at)
        page.keyboard.up("Control")

    anchor_state = (
        """(w) => w.anchor && {
          path: w.anchor.path, tick: w.anchor.tick, value: w.anchor.value,
          isSample: __cockpit.histories.get(w.anchor.path)?.valueAt(w.anchor.tick) === w.anchor.value,
          lx: !w.el.querySelector('.anchor-line-x').hidden,
          ly: !w.el.querySelector('.anchor-line-y').hidden,
        }"""
    )

    # ── [test->app~views_017~1] Ctrl+click drops the anchor at the pointed
    #    signal's nearest sample; both lines appear ──
    tgt_a = require_target(VEL_M, "views_017 probe found a pointable spot on the anchor signal")
    ctrl_click(tgt_a["at"])
    a0 = cwidget_eval(anchor_state)
    check(
        "views_017 ctrl+click anchors the pointed signal's nearest sample, lines shown",
        bool(a0)
        and a0["path"] == VEL_M
        and a0["isSample"]
        and abs(a0["tick"] - tgt_a["clickTick"]) <= 2
        and a0["lx"]
        and a0["ly"],
        a0,
    )

    # ── [test->app~views_017~1] a second ctrl+click replaces the anchor ──
    tgt_b = None
    a0_tick = a0["tick"] if a0 else 0
    for frac in (0.2, 0.8, 0.3, 0.7, 0.6, 0.4):
        cand = probe_target(VEL_M, frac)
        if cand and abs(cand["clickTick"] - a0_tick) >= 500:
            tgt_b = cand
            break
    check("views_017 probe found a second distinct spot", tgt_b is not None, tgt_b)
    if tgt_b is None:
        tgt_b = tgt_a  # same-spot fallback: dependent checks fail honestly
    ctrl_click(tgt_b["at"])
    a1 = cwidget_eval(anchor_state)
    check(
        "views_017 a second ctrl+click replaces the anchor in place",
        bool(a1) and a1["isSample"] and a1["tick"] != a0["tick"],
        (a0 and a0["tick"], a1 and a1["tick"]),
    )

    # ── [test->app~views_017~1] a sub-6 px click within 8 px of the vertical
    #    line releases the anchor ──
    line_click = cwidget_eval(
        """(w) => {
          const rect = w.el.querySelector('.plot-canvas').getBoundingClientRect();
          const [t0, t1] = w.window;
          const ax = rect.left + ((w.anchor.tick - t0) / (t1 - t0)) * rect.width;
          return [ax + 2, rect.top + rect.height - 12];
        }"""
    )
    page.mouse.click(*line_click)
    check(
        "views_017 a click on the line releases the anchor (both lines hide)",
        cwidget_eval(
            "(w) => w.anchor === null"
            " && w.el.querySelector('.anchor-line-x').hidden"
            " && w.el.querySelector('.anchor-line-y').hidden"
        ),
    )

    # ── [test->app~views_017~1] removing the anchor's signal releases ──
    ctrl_click(tgt_a["at"])
    check("views_017 re-drop for the removal check", cwidget_eval("(w) => !!w.anchor"))
    released = cwidget_eval(
        f"""(w) => {{
          w.removeSignal({VEL_M!r});
          const gone = w.anchor === null;
          w.addSignal({VEL_M!r});   // restore for the delta checks
          return gone;
        }}"""
    )
    check("views_017 the anchor's signal leaving the widget releases it", released)

    # ── [test->app~views_017~1] resume releases ──
    tgt_a = require_target(VEL_M, "views_017 probe (post re-add)")
    ctrl_click(tgt_a["at"])
    check("views_017 re-drop for the resume check", cwidget_eval("(w) => !!w.anchor"))
    page.evaluate("() => __cockpit.timeline.resume()")
    check("views_017 resume releases the anchor", cwidget_eval("(w) => w.anchor === null"))

    # ── [test->app~views_017~1] independence: a second widget anchors its
    #    own sample; each widget's lines mark itself alone ──
    page.evaluate("() => __cockpit.timeline.pause()")
    tgt_a = require_target(VEL_M, "views_017 probe (fresh pause)")
    ctrl_click(tgt_a["at"])
    check(
        "views_017 re-drop for the delta checks",
        cwidget_eval("(w) => w.anchor?.path") == VEL_M,
        cwidget_eval("(w) => w.anchor?.path"),
    )
    other = page.evaluate(
        f"""([cwid, twid]) => {{
          let wB = null;
          __cockpit.forEachWidget(w => {{ if (w.cfg.id === twid) wB = w; }});
          wB.el.scrollIntoView({{ block: 'center' }});
          wB.refresh();
          const canvas = wB.el.querySelector('.plot-canvas');
          const rect = canvas.getBoundingClientRect();
          const p = wB.cfg.signals[0];
          const [txs, tys] = wB.renderedTables().get(p);
          const mid = Math.floor(txs.length / 2);
          if (tys[mid] == null) return null;
          const px = ((txs[mid] - wB.window[0]) / (wB.window[1] - wB.window[0])) * rect.width;
          const py = Math.min(rect.height - 3, Math.max(3, wB.yPxOf(tys[mid], wB.sideOf(p), rect.height) + 6));
          const at = [rect.left + px, rect.top + py];
          if (document.elementFromPoint(...at)?.closest('.plot-canvas') !== canvas) return null;
          if (wB.computePointed(at[0], at[1], canvas) !== p) return null;
          return {{ at, path: p }};
        }}""",
        [cwid, twid],
    )
    check("views_017 second-widget probe", other is not None, other)
    ctrl_click(other["at"])
    indep = page.evaluate(
        f"""([cwid, twid]) => {{
          const out = {{}};
          __cockpit.forEachWidget(w => {{
            if (w.cfg.id === cwid) out.a = {{ path: w.anchor?.path, lx: !w.el.querySelector('.anchor-line-x').hidden }};
            if (w.cfg.id === twid) out.b = {{ path: w.anchor?.path, lx: !w.el.querySelector('.anchor-line-x').hidden }};
          }});
          return out;
        }}""",
        [cwid, twid],
    )
    check(
        "views_017 two widgets hold independent anchors, lines on their own plots",
        indep["a"]["path"] == VEL_M
        and indep["b"]["path"] == other["path"]
        and indep["a"]["lx"]
        and indep["b"]["lx"],
        indep,
    )
    widget_eval(page, twid, "(w) => w.releaseAnchor()")

    # ── [test->app~views_018~1] the time delta shows on the anchoring widget
    #    with the cursor set from ANOTHER plot ──
    hover_b = page.evaluate(
        f"""(twid) => {{
          let wB = null;
          __cockpit.forEachWidget(w => {{ if (w.cfg.id === twid) wB = w; }});
          const r = wB.el.querySelector('.plot-canvas').getBoundingClientRect();
          return [r.left + r.width / 2, r.top + 10];
        }}""",
        twid,
    )
    page.mouse.move(*hover_b)
    page.wait_for_function("() => __cockpit.cursor.tick !== null")
    dt_read = cwidget_eval(
        """(w) => {
          const el = w.el.querySelector('[data-delta-t]');
          return el && { text: el.textContent, expect: __cockpit.cursor.tick - w.anchor.tick };
        }"""
    )
    dt_num = None
    if dt_read:
        digits = "".join(ch for ch in dt_read["text"] if ch.isdigit())
        dt_num = int(digits) * (-1 if "-" in dt_read["text"] else 1)
    check(
        "views_018 time delta shows on the anchoring widget from a foreign cursor",
        dt_read is not None and dt_num == dt_read["expect"],
        dt_read,
    )

    # ── [test->app~views_018~1] value delta on a same-axis numeric pointed
    #    trace, with the anchor's arithmetic ──
    tgt_s = require_target(VEL_S, "views_018 probe found a spot on the same-axis signal")
    page.mouse.move(*tgt_s["at"])
    page.wait_for_function(
        f"""() => {{ let p = null; __cockpit.forEachWidget(w => {{ if (w.cfg.id === {cwid!r}) p = w.pointed; }});
          return p === {VEL_S!r}; }}""",
        timeout=3000,
    )
    dv_read = cwidget_eval(
        """(w, p) => {
          const el = w.el.querySelector(`[data-path="${p}"] [data-delta-v]`);
          const v = w.cursorValueFor(p);
          return { text: el ? el.textContent : null,
                   expect: v === null || !w.anchor ? null : v - w.anchor.value };
        }""",
        VEL_S,
    )
    dv_ok = False
    if dv_read and dv_read["text"] and dv_read["expect"] is not None:
        num = float(dv_read["text"].replace("Δ", "").replace("+", "").replace(" ", ""))
        dv_ok = abs(num - dv_read["expect"]) < 2e-4
    check("views_018 same-axis pointed row carries the value delta", dv_ok, dv_read)

    # ── [test->app~views_018~1] no value delta: other axis, enum, or a
    #    cursor time with no sample ──
    gates = cwidget_eval(
        f"""(w, [U32, ENUM, VELS]) => {{
          const has = (p) => !!w.el.querySelector(`[data-path="${{p}}"] [data-delta-v]`);
          w.applyPointed(U32);
          const otherAxis = has(U32);
          w.applyPointed(ENUM);
          const enumRow = has(ENUM);
          // A tick inside the mock's 5 s gap window (t % 5000 in
          // [4880, 5000)), within the paused view.
          const h = __cockpit.histories.get(VELS);
          const newest = h.ticks[h.ticks.length - 1];
          const gt = Math.floor(newest / 5000) * 5000 - 60;
          if (gt < w.window[0]) return {{ gapUnreachable: true }};
          w.applyPointed(VELS);
          __cockpit.setCursorTick(gt);
          const row = w.el.querySelector(`[data-path="${{VELS}}"]`);
          return {{ otherAxis, enumRow,
                    gapAbsent: row ? row.textContent.includes('no sample') : null,
                    gapDelta: has(VELS) }};
        }}""",
        [U32_S, ENUM_S, VEL_S],
    )
    check(
        "views_018 no value delta off-axis, for an enum, or in a gap",
        gates
        and not gates["otherAxis"]
        and not gates["enumRow"]
        and gates["gapAbsent"] is True
        and not gates["gapDelta"],
        gates,
    )

    # ── views_017/views_009 seam: a ctrl+drag neither zooms nor drops; a
    #    plain drag-select still zooms and the anchor survives it ──
    win0 = page.evaluate("() => __cockpit.timeline.currentWindow()")
    tick_before = cwidget_eval("(w) => w.anchor && w.anchor.tick")
    box = cwidget_eval(
        "(w) => { const r = w.el.querySelector('.plot-canvas').getBoundingClientRect();"
        " return { x: r.left, y: r.top, w: r.width, h: r.height }; }"
    )
    page.keyboard.down("Control")
    page.mouse.move(box["x"] + box["w"] * 0.3, box["y"] + box["h"] - 20)
    page.mouse.down()
    page.mouse.move(box["x"] + box["w"] * 0.6, box["y"] + box["h"] - 20, steps=4)
    page.mouse.up()
    page.keyboard.up("Control")
    win1 = page.evaluate("() => __cockpit.timeline.currentWindow()")
    tick_mid = cwidget_eval("(w) => w.anchor && w.anchor.tick")
    check(
        "views_017 ctrl+drag neither zooms nor re-drops",
        win1 == win0 and tick_mid == tick_before,
        (win0, win1, tick_before, tick_mid),
    )
    page.mouse.move(box["x"] + box["w"] * 0.2, box["y"] + box["h"] - 20)
    page.mouse.down()
    page.mouse.move(box["x"] + box["w"] * 0.7, box["y"] + box["h"] - 20, steps=4)
    page.mouse.up()
    win2 = page.evaluate("() => __cockpit.timeline.currentWindow()")
    tick_after = cwidget_eval("(w) => w.anchor && w.anchor.tick")
    check(
        "views_009 drag-select still zooms with an anchor held, anchor survives",
        win2 != win1 and tick_after == tick_before,
        (win1, win2, tick_before, tick_after),
    )

    # ── [test->app~views_017~1] hidden lines are not release targets: a line
    #    whose coordinate left the window/range must not keep a phantom 8 px
    #    click strip at its projected position ──
    page.evaluate("() => __cockpit.timeline.resume()")
    page.evaluate("() => __cockpit.timeline.pause()")
    tgt_h = require_target(VEL_M, "views_017 probe (hidden-line block)")
    ctrl_click(tgt_h["at"])
    check(
        "views_017 drop for the hidden-line block",
        cwidget_eval("(w) => w.anchor?.path") == VEL_M,
    )

    # (a) a manual scale projects the horizontal line 4 px above the canvas
    #     top (hidden); the vertical stays visible.
    seta = cwidget_eval(
        """(w) => {
          const rect = w.el.querySelector('.plot-canvas').getBoundingClientRect();
          const h = rect.height, v = w.anchor.value, D = 10;
          const min = v - D * (1 + 4 / h);   // ay lands at exactly -4 px
          w.setManualRange(w.sideOf(w.anchor.path), min, min + D);
          const [t0, t1] = w.window;
          return {
            rect: { x: rect.left, y: rect.top, w: rect.width, h },
            ax: ((w.anchor.tick - t0) / (t1 - t0)) * rect.width,
            ay: (1 - (v - min) / D) * h,
            lxHidden: w.el.querySelector('.anchor-line-x').hidden,
            lyHidden: w.el.querySelector('.anchor-line-y').hidden,
          };
        }"""
    )
    check(
        "views_017 manual scale hides only the horizontal line",
        seta and not seta["lxHidden"] and seta["lyHidden"] and -8 <= seta["ay"] <= 0,
        seta,
    )
    ra = seta["rect"]
    px_off = ra["x"] + (seta["ax"] + 150 if seta["ax"] + 158 < ra["w"] else seta["ax"] - 150)
    page.mouse.click(px_off, ra["y"] + 3)  # 7 px from the hidden line's projection
    check(
        "views_017 a hidden horizontal line is not a release target",
        cwidget_eval("(w) => !!w.anchor"),
    )
    page.mouse.click(ra["x"] + seta["ax"] + 2, ra["y"] + ra["h"] - 12)
    check(
        "views_017 the still-visible vertical line releases (horizontal hidden)",
        cwidget_eval("(w) => w.anchor === null"),
    )
    cwidget_eval("(w) => w.setScaleMode('L', 'auto')")

    # (b) a paused zoom past the anchor time projects the vertical line 5 px
    #     left of the canvas (hidden); a manual range keeps the horizontal
    #     mid-canvas and visible.
    tgt_h = require_target(VEL_M, "views_017 probe (zoom-hide block)")
    ctrl_click(tgt_h["at"])
    setb = cwidget_eval(
        """(w) => {
          const rect = w.el.querySelector('.plot-canvas').getBoundingClientRect();
          const v = w.anchor.value;
          w.setManualRange(w.sideOf(w.anchor.path), v - 5, v + 5); // ay = h/2, visible
          const [t0, t1] = w.window;
          const S = (t1 - t0) * 0.3;
          __cockpit.timeline.selectRange(w.anchor.tick + (5 * S) / rect.width,
                                         w.anchor.tick + (5 * S) / rect.width + S);
          const [u0, u1] = w.window;
          return {
            rect: { x: rect.left, y: rect.top, w: rect.width, h: rect.height },
            ax: ((w.anchor.tick - u0) / (u1 - u0)) * rect.width,
            ay: w.yPxOf(v, w.sideOf(w.anchor.path), rect.height),
            lxHidden: w.el.querySelector('.anchor-line-x').hidden,
            lyHidden: w.el.querySelector('.anchor-line-y').hidden,
          };
        }"""
    )
    check(
        "views_017 zoom past the anchor hides only the vertical line",
        setb and setb["lxHidden"] and not setb["lyHidden"] and -8 <= setb["ax"] <= 0,
        setb,
    )
    rb = setb["rect"]
    page.mouse.click(rb["x"] + 2, rb["y"] + rb["h"] - 12)  # 7 px from hidden lx
    check(
        "views_017 a hidden vertical line is not a release target",
        cwidget_eval("(w) => !!w.anchor"),
    )
    page.mouse.click(rb["x"] + rb["w"] * 0.75, rb["y"] + setb["ay"] + 2)
    check(
        "views_017 the still-visible horizontal line releases (vertical hidden)",
        cwidget_eval("(w) => w.anchor === null"),
    )

    # (c) both lines hidden: bare clicks release nothing anywhere; Resume
    #     still releases.
    page.evaluate("() => __cockpit.timeline.resume()")
    page.evaluate("() => __cockpit.timeline.pause()")
    cwidget_eval("(w) => w.setScaleMode('L', 'auto')")
    tgt_h = require_target(VEL_M, "views_017 probe (both-hidden block)")
    ctrl_click(tgt_h["at"])
    setc = cwidget_eval(
        """(w) => {
          const rect = w.el.querySelector('.plot-canvas').getBoundingClientRect();
          const h = rect.height, v = w.anchor.value, D = 10;
          w.setManualRange(w.sideOf(w.anchor.path), v - D * (1 + 4 / h) , v - D * (1 + 4 / h) + D);
          const [t0, t1] = w.window;
          const S = (t1 - t0) * 0.3;
          __cockpit.timeline.selectRange(w.anchor.tick + (5 * S) / rect.width,
                                         w.anchor.tick + (5 * S) / rect.width + S);
          return {
            rect: { x: rect.left, y: rect.top, w: rect.width, h: rect.height },
            lxHidden: w.el.querySelector('.anchor-line-x').hidden,
            lyHidden: w.el.querySelector('.anchor-line-y').hidden,
          };
        }"""
    )
    check(
        "views_017 both lines hidden (zoom + manual scale)",
        setc and setc["lxHidden"] and setc["lyHidden"],
        setc,
    )
    rc = setc["rect"]
    for cx, cy in [
        (rc["x"] + 2, rc["y"] + rc["h"] / 2),
        (rc["x"] + rc["w"] - 3, rc["y"] + rc["h"] / 2),
        (rc["x"] + rc["w"] / 2, rc["y"] + 3),
        (rc["x"] + rc["w"] / 2, rc["y"] + rc["h"] - 3),
        (rc["x"] + rc["w"] / 2, rc["y"] + rc["h"] / 2),
    ]:
        page.mouse.click(cx, cy)
    check(
        "views_017 with both lines hidden no bare click releases",
        cwidget_eval("(w) => !!w.anchor"),
    )
    page.evaluate("() => __cockpit.timeline.resume()")
    check(
        "views_017 resume releases a fully-hidden anchor",
        cwidget_eval("(w) => w.anchor === null"),
    )

    # ── [test->app~views_017~1] ctrl+re-drop within 8 px of an anchor line
    #    replaces (the set action wins over the release gesture) ──
    page.evaluate("() => __cockpit.timeline.pause()")
    cwidget_eval("(w) => w.setScaleMode('L', 'auto')")
    tgt_h = require_target(VEL_M, "views_017 probe (precedence block)")
    ctrl_click(tgt_h["at"])
    old_tick = cwidget_eval("(w) => w.anchor && w.anchor.tick")
    near_line = cwidget_eval(
        """(w, path) => {
          const canvas = w.el.querySelector('.plot-canvas');
          const rect = canvas.getBoundingClientRect();
          const [t0, t1] = w.window;
          const ax = ((w.anchor.tick - t0) / (t1 - t0)) * rect.width;
          // A spot ON the anchor's own trace, 4-8 px right of the vertical
          // line — inside the release strip, but Ctrl must re-drop instead.
          """ + NEAREST_Y_JS + """
          for (const dx of [4, 6, 8]) {
            const px = ax + dx;
            if (px > rect.width - 3) continue;
            const t = w.tickAtPx(px, rect.width);
            const y = nearestY(w, path, t, rect);
            if (y === null) continue;
            const py = Math.min(rect.height - 3, Math.max(3, y + 3));
            const at = [rect.left + px, rect.top + py];
            if (document.elementFromPoint(...at)?.closest('.plot-canvas') !== canvas) continue;
            if (w.computePointed(at[0], at[1], canvas) !== path) continue;
            return at;
          }
          return null;
        }""",
        VEL_M,
    )
    check("views_017 probe found an on-trace spot inside the release strip", near_line is not None, near_line)
    ctrl_click(near_line)
    redropped = cwidget_eval("(w) => w.anchor && w.anchor.tick")
    check(
        "views_017 ctrl+click inside the release strip re-drops, not releases",
        redropped is not None and redropped != old_tick,
        (old_tick, redropped),
    )
    page.evaluate("() => __cockpit.timeline.resume()")
    page.mouse.move(10, 10)

    # ── history ring: a single oversized append() stays consistent — the
    #    ring force-drops its oldest chunk instead of writing past capacity
    #    (regression for the silent-overflow QA finding; only the test/seed
    #    surface can produce an append this large) ──
    ring = page.evaluate(
        """() => {
          const proto = [...__cockpit.histories.values()][0].constructor;
          const h = new proto(1000); // slow period -> small capacity
          const cap = h._cap;
          const pts = [];
          let t = 0;
          for (let i = 0; i < cap + 5; i++) { t += 1000; pts.push([t, i]); }
          h.append(pts);
          const [xs, ys] = h.windowTable(t - 5000, t);
          let ascending = true;
          for (let i = 1; i < xs.length; i++) if (!(xs[i] > xs[i - 1])) ascending = false;
          return {
            cap, len: h.size, newest: h.newestTick(), expected: t,
            tail_ok: ascending && xs.length === 6 && ys.every(v => Number.isFinite(v)),
            at_newest: h.valueAt(t),
          };
        }"""
    )
    check(
        "history ring survives an oversized single append",
        ring["len"] <= ring["cap"]
        and ring["newest"] == ring["expected"]
        and ring["tail_ok"]
        and ring["at_newest"] == ring["cap"] + 4,
        ring,
    )

    # ═══ batch 11: live smooth scroll — the window glides at display rate
    #     while geometry stays at batch rate (presentation cadence,
    #     unspecced; the FPS-cell precedent) ═══

    # (a) cached-geometry scroll redraws track the page's REAL frame rate
    #     (measured in-page — an absolute fps floor would just re-test the
    #     host machine's load) and land well above batch-rate draws
    page.evaluate("() => __cockpit.timeline.resume()")
    rates = page.evaluate(
        """async () => {
          let n = 0;
          __cockpit.forEachWidget((w) => {
            if (w.scrollTick && w.cfg.signals?.length && w.gl) n++;
          });
          const s0 = __cockpit.perf.snapshot();
          let rafs = 0;
          const t0 = performance.now();
          await new Promise((done) => {
            const tick = (now) => {
              rafs++;
              if (now - t0 < 2000) requestAnimationFrame(tick); else done();
            };
            requestAnimationFrame(tick);
          });
          const s1 = __cockpit.perf.snapshot();
          return { n, rafs, scrolls: s1.scrolls - s0.scrolls, draws: s1.draws - s0.draws };
        }"""
    )
    # Per plot, at least half the page's frames scrolled (the lead clamp
    # legitimately freezes the tail of a slow batch gap), and scroll
    # updates outnumber geometry draws — the feature's whole point.
    # Liveliness gate: below ~10 rAF/s the host (a starved CI runner ran
    # ~4 fps) is not rendering frames to track — the cadence claim is
    # unmeasurable there, so skip with the observed numbers rather than
    # re-test load. A merely-busy box (measured 18 rAF/s locally) still
    # asserts and passes.
    if rates["rafs"] >= 20:  # 2 s window
        # The beat-draws ratio only discriminates when the display ticks
        # meaningfully faster than geometry lands: at rAF ≈ batch cadence
        # (run-3 macOS: 23 rafs vs 26 draws) scrolls cannot outnumber
        # draws 1.5x by construction — gate that clause on rafs ≥ 2x
        # draws; the frame-tracking clause asserts at any liveliness.
        beat_ok = (
            rates["scrolls"] > rates["draws"] * 1.5
            if rates["rafs"] >= rates["draws"] * 2
            else True
        )
        if rates["rafs"] < rates["draws"] * 2:
            print(f"  [note] beat-draws clause ungated only above 2x batch cadence — {rates}")
        check(
            "smooth scroll: per-plot scroll redraws track the frame rate, beat draws",
            rates["n"] > 0
            and rates["scrolls"] >= rates["n"] * rates["rafs"] * 0.5
            and beat_ok,
            rates,
        )
    else:
        print(f"  [skip] smooth-scroll cadence: host delivered {rates['rafs']} rAF in 2 s — {rates}")
    walk = page.evaluate(
        """async () => {
          const ends = [];
          const t0 = performance.now();
          let newest0 = 0;
          for (const h of __cockpit.histories.values()) {
            const t = h.newestTick(); if (t != null && t > newest0) newest0 = t;
          }
          for (let i = 0; i < 15; i++) {
            ends.push(__cockpit.timeline.displayWindow()[1]);
            await new Promise((r) => setTimeout(r, 100));
          }
          let mono = true;
          for (let i = 1; i < ends.length; i++) if (ends[i] < ends[i - 1]) mono = false;
          let newest1 = 0;
          for (const h of __cockpit.histories.values()) {
            const t = h.newestTick(); if (t != null && t > newest1) newest1 = t;
          }
          return { mono, advance: ends[ends.length - 1] - ends[0],
                   wall: performance.now() - t0, dataAdv: newest1 - newest0 };
        }"""
    )
    # The display clock may only track DELIVERED data plus the lead clamp:
    # on a starved host the devmock's own batch emitter gaps (run-4 CI:
    # advance 750 over 2360 ms wall — the clamp honestly froze through the
    # gaps), so the advance floor is bounded by the data's advance, never
    # by wall time. Monotonicity is load-independent honesty and is
    # asserted in EVERY condition. With healthy data flow (dataAdv >= 900)
    # the display must track it (>= 75% covers clamp-freeze tails); with a
    # starved emitter the floor is unmeasurable — skip with the numbers.
    # The upper bound stays: advance can never exceed wall + lead.
    upper_ok = walk["advance"] <= walk["wall"] + 200
    if walk["dataAdv"] >= 900:
        check(
            "smooth scroll: display window end is monotonic and tracks wall time",
            walk["mono"] and walk["advance"] >= min(900, walk["dataAdv"] * 0.75) and upper_ok,
            walk,
        )
    else:
        print(f"  [skip] smooth-scroll advance floor: emitter starved (dataAdv={walk['dataAdv']}) — {walk}")
        check(
            "smooth scroll: display window end is monotonic and tracks wall time",
            walk["mono"] and upper_ok,
            walk,
        )

    # (b) honesty: the displayed now never leads the newest data past the
    #     lead clamp (a stalled stream freezes instead of scrolling on)
    lead = page.evaluate(
        """async () => {
          let worst = -1e9;
          for (let i = 0; i < 20; i++) {
            let newest = 0;
            for (const h of __cockpit.histories.values()) {
              const t = h.newestTick();
              if (t != null && t > newest) newest = t;
            }
            const d = __cockpit.timeline.displayWindow()[1] - newest;
            if (d > worst) worst = d;
            await new Promise((r) => setTimeout(r, 60));
          }
          return worst;
        }"""
    )
    check("smooth scroll: display lead stays within the clamp", lead <= 80, lead)

    # (c) cursor mapping under a scrolling window: a pointermove lands on
    #     the tick the displayed window puts under the pointer (same-task
    #     read — the window cannot advance mid-evaluate). The expectation is
    #     derived INDEPENDENTLY from displayWindow + rect math, never from
    #     the widget's own mapping (which is the code under test).
    cur = page.evaluate(
        """() => {
          let out = null;
          __cockpit.forEachWidget((w) => {
            if (out || !w.scrollTick || !w.cfg.signals?.length) return;
            const canvas = w.el.querySelector('.plot-canvas');
            const rect = canvas.getBoundingClientRect();
            if (rect.width < 50) return;
            const px = rect.width * 0.6;
            canvas.dispatchEvent(new PointerEvent('pointermove', {
              clientX: rect.left + px, clientY: rect.top + rect.height / 2, bubbles: true,
            }));
            const [d0, d1] = __cockpit.timeline.displayWindow();
            const expected = d0 + (px / rect.width) * (d1 - d0);
            out = { tick: __cockpit.cursor.tick, expected, err: Math.abs(__cockpit.cursor.tick - expected) };
          });
          return out;
        }"""
    )
    check(
        "smooth scroll: cursor tick matches the displayed window's mapping",
        cur is not None and cur["err"] <= 1,
        cur,
    )
    page.mouse.move(10, 10)

    # (d) paused parks the scroll loop: the scroll counter goes flat
    page.evaluate("() => __cockpit.timeline.pause()")
    page.wait_for_timeout(400)  # drain any queued frame
    p0 = page.evaluate("() => __cockpit.perf.snapshot().scrolls")
    page.wait_for_timeout(700)
    p1 = page.evaluate("() => __cockpit.perf.snapshot().scrolls")
    check("smooth scroll: paused stops scroll redraws", p1 == p0, (p0, p1))
    page.evaluate("() => __cockpit.timeline.resume()")

    # (e) dash-phase continuity: a fixed sample's dash phase holds across
    #     geometry rebuilds while the live window clips the run's left edge
    #     (regression: the clipped run re-anchored its arc datum per batch,
    #     popping the phase 3.6-6.3 px per rebuild at a 13 px period)
    DASH_SIG = "IO_AS5048_data.channels[0].angle_deg"
    prev_span = page.evaluate("() => __cockpit.timeline.get().span_ms")
    page.evaluate(
        """(sig) => {
          __cockpit.addWatch(sig, 10);
          const w = __cockpit.addWidget({ type: 'plot', signals: [] });
          w.addSignal(sig);
          window.__dashWid = w.cfg.id;
          const a = __cockpit.appearance.of(sig) || {};
          __cockpit.appearance.set(sig, { ...a, style: 'dashed' });
          __cockpit.timeline.setSpan(5000);
        }""",
        DASH_SIG,
    )
    page.wait_for_function(
        f"() => (__cockpit.histories.get({DASH_SIG!r})?.size || 0) > 300", timeout=8000
    )
    page.wait_for_timeout(400)
    dash = page.evaluate(
        """async (sig) => {
          let w = null;
          __cockpit.forEachWidget((x) => { if (x.cfg.id === window.__dashWid) w = x; });
          if (!w || !w._built?.length || !w._built[0].runs?.length) {
            return { n: 0, worst: -1 };
          }
          const PERIOD = 8 + 5; // the dashed style's on+off, css px
          const arcAtX = (v, x) => {
            if (!v || v.length < 3 || x < v[0] - 1e-6) return null;
            for (let k = 0; k + 3 < v.length; k += 3) {
              if (v[k + 3] >= x) {
                const xA = v[k], yA = v[k + 1], aA = v[k + 2];
                const dx = v[k + 3] - xA;
                if (dx <= 1e-9) return v[k + 5];
                const t = (x - xA) / dx;
                const y = yA + t * (v[k + 4] - yA);
                return aA + Math.hypot(x - xA, y - yA);
              }
            }
            return null;
          };
          // A fixed sample INSIDE the window-clipped first run (75% along
          // it, so it outlives ~20 rebuilds of left-clip churn); the arc
          // lookup scans every run — the sample's run index drifts as
          // earlier runs scroll out.
          const v0 = w._built[0].runs[0].verts;
          const xPick = v0[0] + (v0[v0.length - 3] - v0[0]) * 0.75;
          const T = w.window[0] + xPick / w._pxPerMs;
          const phases = [];
          let last = w.gl.drawCount;
          const t0 = performance.now();
          while (phases.length < 20 && performance.now() - t0 < 4000) {
            await new Promise((r) => setTimeout(r, 30));
            if (w.gl.drawCount === last) continue;
            last = w.gl.drawCount;
            const x = (T - w.window[0]) * w._pxPerMs;
            let arc = null;
            for (const run of w._built[0]?.runs || []) {
              arc = arcAtX(run.verts, x);
              if (arc !== null) break;
            }
            if (arc !== null) phases.push(((arc % PERIOD) + PERIOD) % PERIOD);
          }
          let worst = 0;
          for (const ph of phases) {
            let d = Math.abs(ph - phases[0]);
            d = Math.min(d, PERIOD - d);
            if (d > worst) worst = d;
          }
          return { n: phases.length, worst };
        }""",
        DASH_SIG,
    )
    # The 12-rebuild floor assumes batch-rate rebuilds land within the 4 s
    # probe window; a starved host delivers fewer. The PHASE bound is the
    # regression being pinned (the bug popped 3.6+ px on EVERY rebuild, so
    # even a handful of samples catches it) and is asserted whenever any
    # rebuild was observed; only the cadence expectation relaxes, noted.
    if dash["n"] < 12:
        print(f"  [note] dash-phase probe saw only {dash['n']} rebuilds in 4 s (loaded host)")
    if dash["n"] >= 1:
        check(
            "smooth scroll: dash phase of a fixed sample holds across rebuilds",
            0 <= dash["worst"] <= 0.25,
            dash,
        )
    else:
        print("  [skip] dash-phase continuity: no rebuilds observed under load")
    page.evaluate(
        """([sig, span]) => {
          const a = __cockpit.appearance.of(sig) || {};
          __cockpit.appearance.set(sig, { ...a, style: null });
          __cockpit.timeline.setSpan(span);
        }""",
        [DASH_SIG, prev_span],
    )

    # ═══ batch 12 (bench): cable-pull reconnect + watch reinstall ═══
    # The real-hardware failure pair: a replugged port's first open fails
    # while the OS finishes device setup (killing the old poll), and the
    # board clears its watch list with the CDC line state (the committed-
    # list cache then swallowed the reinstall).

    fresh_boot(page)
    add_plot_with(page, ["task1msRuns"], period=10)
    wait_for_samples(page, "task1msRuns")
    installs0 = page.evaluate("() => (window.__devmockInstalls || []).length")

    # ── [test->app~conn_001~1] pull: lost state, port out of enumeration ──
    page.evaluate("() => window.__devmockConn.pull()")
    page.wait_for_function("() => __cockpit.store.connection.state === 'lost'")
    check(
        "conn_001 cable pull lands in the lost state with the lost pill",
        page.evaluate("() => document.querySelector('.conn-port--lost') !== null"),
        page.evaluate("() => __cockpit.store.connection.state"),
    )
    ports = page.evaluate("async () => (await __cockpit.api.listPorts()).map((p) => p.name)")
    check("conn_001 pulled port leaves enumeration", "COM8" not in ports, ports)

    # ── [test->app~conn_001~1] replug with ONE failing open: the poll must
    #    ride through the failure and reconnect without user action ──
    page.evaluate("() => window.__devmockConn.replug({ failConnects: 1 })")
    reconnected = True
    try:
        page.wait_for_function(
            "() => __cockpit.store.connection.state === 'connected'", timeout=10000
        )
    except Exception:
        reconnected = False
    check(
        "conn_001 replug auto-reconnects without user action, surviving a failed first open",
        reconnected,
        page.evaluate("() => [__cockpit.store.connection.state, __cockpit.store.connectError]"),
    )

    # ── [test->app~conn_001~1] the unchanged watch list recommits in full,
    #    exactly once, and the stream comes back to life ──
    page.wait_for_function(
        f"() => (window.__devmockInstalls || []).length > {installs0}", timeout=5000
    )
    page.wait_for_timeout(900)  # settle: room for any duplicate install to land
    installs = page.evaluate(f"() => window.__devmockInstalls.slice({installs0})")
    check(
        "conn_001 reconnect recommits the unchanged watch list exactly once",
        len(installs) == 1 and [w["path"] for w in installs[0]] == ["task1msRuns"],
        installs,
    )
    page.wait_for_function(
        "() => (__cockpit.histories.get('task1msRuns')?.size || 0) > 100", timeout=8000
    )
    h0 = page.evaluate("() => __cockpit.histories.get('task1msRuns').size")
    page.wait_for_timeout(300)
    h1 = page.evaluate("() => __cockpit.histories.get('task1msRuns').size")
    check("conn_001 stream restarts and histories repopulate after reconnect", h1 > h0, (h0, h1))
    val = page.locator(".watch-row .watch-value").first.inner_text()
    check("conn_001 watch panel shows a live value after reconnect", val not in ("", "no sample"), val)

    # ── [test->app~conn_001~1] manual-connect variant reinstalls too (the
    #    invalidation is connection-driven, not poll-driven) ──
    installs1 = page.evaluate("() => (window.__devmockInstalls || []).length")
    page.evaluate("() => window.__devmockConn.pull()")
    page.wait_for_function("() => __cockpit.store.connection.state === 'lost'")
    page.evaluate(
        "() => { window.__devmockConn.replug(); return __cockpit.api.connect('COM8'); }"
    )
    page.wait_for_function("() => __cockpit.store.connection.state === 'connected'")
    page.wait_for_function(
        f"() => (window.__devmockInstalls || []).length > {installs1}", timeout=5000
    )
    manual = page.evaluate(f"() => window.__devmockInstalls.length - {installs1}")
    check("conn_001 manual reconnect also reinstalls the watch list", manual == 1, manual)

    # ═══ batch 13 (bench round 3): anchor preview, widget launcher, watch
    # drag sources, UI zoom (the views_009 dead-stop lives with its batch) ═══

    fresh_boot(page)
    for p in (VEL_M, VEL_S):
        page.evaluate(f"() => __cockpit.addWatch({p!r}, 1)")
    cwid = page.evaluate(
        """(paths) => { const w = __cockpit.addWidget({ type: 'plot', signals: [] });
          for (const p of paths) w.addSignal(p); return w.cfg.id; }""",
        [VEL_M, VEL_S],
    )
    page.wait_for_function(
        "() => __cockpit.store.traceStatus && __cockpit.store.traceStatus.link_rate_bytes_per_s > 0",
        timeout=5000,
    )
    for p in (VEL_M, VEL_S):
        wait_for_samples(page, p)
    page.evaluate("() => __cockpit.timeline.pause()")

    # ── [test->app~views_019~1] Ctrl held with a pointed trace shows the
    #    candidate mark at the nearest sample's value (the key-only path:
    #    the pointer parked first, Ctrl pressed with the mouse still) ──
    tgt = require_target(VEL_M, "views_019 probe found a pointable spot")
    page.mouse.move(*tgt["at"])
    page.wait_for_timeout(80)
    page.keyboard.down("Control")
    page.wait_for_timeout(80)
    p0 = cwidget_eval(
        """(w) => {
          const el = w.el.querySelector('.anchor-preview-y');
          const rect = w.el.querySelector('.plot-canvas').getBoundingClientRect();
          const pv = w.preview;
          return {
            shown: !el.hidden,
            path: pv?.path ?? null,
            tick: pv?.tick ?? null,
            isSample: pv ? __cockpit.histories.get(pv.path)?.valueAt(pv.tick) === pv.value : false,
            yErr: pv && !el.hidden
              ? Math.abs((parseFloat(el.style.top) / 100) * rect.height
                         - w.yPxOf(pv.value, w.sideOf(pv.path), rect.height))
              : null,
            anchor: w.anchor,
          }; }"""
    )
    check(
        "views_019 ctrl-held candidate mark sits at the pointed signal's nearest sample",
        p0["shown"] and p0["path"] == VEL_M and p0["isSample"]
        and abs(p0["tick"] - tgt["clickTick"]) <= 2
        and p0["yErr"] is not None and p0["yErr"] < 1.5,
        p0,
    )

    # ── [test->app~views_019~1] the mark follows the pointer ──
    tgt2 = None
    for frac in (0.65, 0.3, 0.75, 0.2):
        c = probe_target(VEL_M, frac)
        if c and abs(c["clickTick"] - tgt["clickTick"]) > 4:
            tgt2 = c
            break
    check("views_019 second probe spot found", tgt2 is not None, tgt2)
    if tgt2 is None:
        tgt2 = tgt  # same-spot fallback: dependent checks fail honestly
    page.mouse.move(*tgt2["at"])
    page.wait_for_timeout(80)
    moved = cwidget_eval("(w) => w.preview && { path: w.preview.path, tick: w.preview.tick }")
    check(
        "views_019 the candidate follows the pointer",
        bool(moved) and moved["path"] == VEL_M
        and moved["tick"] != p0["tick"]
        and abs(moved["tick"] - tgt2["clickTick"]) <= 2,
        (p0["tick"], moved, tgt2["clickTick"]),
    )

    # ── [test->app~views_019~1] the mark transfers with the pointed trace ──
    tgt_s = require_target(VEL_S, "views_019 probe spot on the second signal found")
    page.mouse.move(*tgt_s["at"])
    page.wait_for_timeout(80)
    transferred = cwidget_eval("(w) => w.preview && w.preview.path")
    check("views_019 the candidate transfers with the emphasis", transferred == VEL_S, transferred)

    # ── [test->app~views_019~1] releasing Ctrl removes the mark and leaves
    #    the anchor state as it was — in both directions ──
    page.keyboard.up("Control")
    page.wait_for_timeout(80)
    p1 = cwidget_eval(
        "(w) => ({ shown: !w.el.querySelector('.anchor-preview-y').hidden, anchor: w.anchor })"
    )
    check(
        "views_019 Ctrl release removes the mark without dropping an anchor",
        (not p1["shown"]) and p1["anchor"] is None,
        p1,
    )
    ctrl_click(tgt2["at"])  # drop a real anchor (views_017 path)
    page.keyboard.down("Control")
    page.mouse.move(*tgt["at"])
    page.wait_for_timeout(80)
    both = cwidget_eval(
        """(w) => ({ pv: !w.el.querySelector('.anchor-preview-y').hidden,
                     ax: !w.el.querySelector('.anchor-line-x').hidden,
                     anchored: !!w.anchor })"""
    )
    check(
        "views_019 the candidate mark coexists with a dropped anchor",
        both["pv"] and both["ax"] and both["anchored"],
        both,
    )
    page.keyboard.up("Control")
    page.wait_for_timeout(80)
    kept = cwidget_eval(
        "(w) => ({ shown: !w.el.querySelector('.anchor-preview-y').hidden, anchored: !!w.anchor })"
    )
    check(
        "views_019 Ctrl release leaves the dropped anchor in place",
        (not kept["shown"]) and kept["anchored"],
        kept,
    )

    # ── [test->app~views_019~1] resume clears the mark (the condition ends) ──
    page.keyboard.down("Control")
    page.mouse.move(*tgt["at"])
    page.wait_for_timeout(80)
    page.evaluate("() => __cockpit.timeline.resume()")
    page.wait_for_timeout(80)
    gone = cwidget_eval("(w) => !w.el.querySelector('.anchor-preview-y').hidden")
    page.keyboard.up("Control")
    check("views_019 resume clears the candidate mark", gone is False, gone)

    # ── [test->app~views_004~1] the widget launcher adds an empty plot and
    #    an empty table at unoccupied lattice positions ──
    n0 = page.evaluate("() => { let n = 0; __cockpit.forEachWidget(() => n++); return n; }")
    page.click(".launcher-fab")
    page.click(".launcher-menu [data-workspace='new-plot']")
    page.click(".launcher-fab")
    page.click(".launcher-menu [data-workspace='new-table']")
    geo = page.evaluate(
        """(n0) => {
          const all = [];
          __cockpit.forEachWidget(w => all.push({ id: w.cfg.id, type: w.toJSON().type,
            x: w.cfg.x, y: w.cfg.y, w: w.cfg.w, h: w.cfg.h,
            empty: w.cfg.signals.length === 0,
            hint: w.el.querySelector('.plot-empty-hint')
              ? !w.el.querySelector('.plot-empty-hint').hidden : null }));
          const added = all.slice(n0);
          const overlap = (a, b) =>
            a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
          const overlaps = added.some(a => all.some(b => b.id !== a.id && overlap(a, b)));
          return { added, overlaps,
                   menuHidden: document.querySelector('.launcher-menu').hidden }; }""",
        n0,
    )
    added = geo["added"]
    check(
        "views_004 launcher adds an empty plot + table on the lattice, unoccupied",
        len(added) == 2
        and added[0]["type"] == "plot" and added[1]["type"] == "table"
        and all(a["empty"] for a in added)
        and all(a[k] % 50 == 0 for a in added for k in ("x", "y", "w", "h"))
        and not geo["overlaps"]
        and added[0]["hint"] is True
        and geo["menuHidden"],
        geo,
    )

    # ── [test->app~views_004~1] a drop onto the launcher-added plot traces it ──
    new_plot_id = added[0]["id"]
    page.evaluate(
        """([id, path]) => {
          let el = null; __cockpit.forEachWidget(w => { if (w.cfg.id === id) el = w.el; });
          const canvas = el.querySelector('.plot-canvas');
          const dt = new DataTransfer();
          dt.setData('text/x-signal', path);
          canvas.dispatchEvent(new DragEvent('dragover', { dataTransfer: dt, bubbles: true }));
          canvas.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true })); }""",
        [new_plot_id, U32_S],
    )
    joined = widget_eval(
        page, new_plot_id,
        "(w) => ({ sigs: [...w.cfg.signals], hintHidden: w.el.querySelector('.plot-empty-hint').hidden })",
    )
    check(
        "views_004 a drop onto the launcher-added plot traces it",
        joined["sigs"] == [U32_S] and joined["hintHidden"],
        joined,
    )

    # ── watch panel rows are drag sources (chrome affordance — the drop
    #    behaviors they feed are views_004's) ──
    drag = page.evaluate(
        """(path) => {
          const row = [...document.querySelectorAll('.watch-row')]
            .find(r => r.dataset.path === path);
          const handle = row?.querySelector('.watch-drag');
          if (!handle || handle.draggable !== true) return { ok: false };
          const dt = new DataTransfer();
          handle.dispatchEvent(new DragEvent('dragstart', { dataTransfer: dt, bubbles: true }));
          return { ok: true, payload: dt.getData('text/x-signal') }; }""",
        VEL_S,
    )
    check(
        "watch row drags with the signal payload from its name region",
        drag["ok"] and drag["payload"] == VEL_S,
        drag,
    )
    page.evaluate(
        """([id, path]) => {
          let el = null; __cockpit.forEachWidget(w => { if (w.cfg.id === id) el = w.el; });
          const canvas = el.querySelector('.plot-canvas');
          const dt = new DataTransfer();
          dt.setData('text/x-signal', path);
          canvas.dispatchEvent(new DragEvent('dragover', { dataTransfer: dt, bubbles: true }));
          canvas.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true })); }""",
        [new_plot_id, VEL_S],
    )
    sigs2 = widget_eval(page, new_plot_id, "(w) => [...w.cfg.signals]")
    check("watch-row drag joins an existing plot on drop", sigs2 == [U32_S, VEL_S], sigs2)
    nw0 = page.evaluate("() => { let n = 0; __cockpit.forEachWidget(() => n++); return n; }")
    page.evaluate(
        """(path) => {
          const dt = new DataTransfer();
          dt.setData('text/x-signal', path);
          const ws = document.querySelector('.workspace');
          ws.dispatchEvent(new DragEvent('dragover', { dataTransfer: dt, bubbles: true }));
          ws.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true,
            clientX: ws.getBoundingClientRect().left + 300,
            clientY: ws.getBoundingClientRect().top + 90 })); }""",
        VEL_M,
    )
    nw1 = page.evaluate("() => { let n = 0; __cockpit.forEachWidget(() => n++); return n; }")
    check("watch-row drag onto empty canvas creates a plot", nw1 == nw0 + 1, (nw0, nw1))

    # ── UI zoom hotkeys (chrome ergonomics, unspecced like the render
    #    cell): devmock path = body CSS zoom, persisted. LAST in the batch —
    #    a zoomed body would skew every later real-mouse coordinate. ──
    page.keyboard.down("Control")
    page.keyboard.press("=")
    page.keyboard.up("Control")
    z1 = page.evaluate("() => document.body.style.zoom")
    check("ui zoom in: Ctrl+'=' scales the UI", z1 == "1.1", z1)
    restart(page, wait_js="() => window.__cockpit !== undefined")
    z2 = page.evaluate("() => document.body.style.zoom")
    check("ui zoom persists across a restart", z2 == "1.1", z2)
    page.keyboard.down("Control")
    page.keyboard.press("-")
    page.keyboard.up("Control")
    z3 = page.evaluate("() => document.body.style.zoom")
    check("ui zoom out: Ctrl+'-' steps back to exactly 1", z3 == "", z3)
    page.keyboard.down("Control")
    page.keyboard.press("-")
    page.keyboard.up("Control")
    z4 = page.evaluate("() => document.body.style.zoom")
    check("ui zoom out below 1 applies the fractional factor", z4 == "0.91", z4)
    page.keyboard.down("Control")
    page.keyboard.press("0")
    page.keyboard.up("Control")
    z5 = page.evaluate("() => document.body.style.zoom")
    check("ui zoom resets with Ctrl+'0'", z5 == "", z5)

    # ═══ batch 14 (QA): reconnect races, paused-reconnect deferral, zoom
    #     hotkeys vs focused editors ═══

    # ── [test->app~conn_001~1] the P1 race: "Reconnect now" lands while a
    #    poll tick is parked inside its listPorts await; the stale tick must
    #    NOT fire a second connect (its teardown would kill the fresh
    #    session), and its aftermath must not clobber "connected" ──
    fresh_boot(page)
    add_plot_with(page, ["task1msRuns"], period=10)
    wait_for_samples(page, "task1msRuns")
    page.evaluate("() => window.__devmockConn.pull()")
    page.wait_for_function("() => __cockpit.store.connection.state === 'lost'")
    page.evaluate(
        """() => {
          const orig = __cockpit.api.listPorts.bind(__cockpit.api);
          window.__portsGate = { entries: 0 };
          window.__portsGate.promise = new Promise((res) => (window.__portsGate.release = res));
          let armed = true;
          __cockpit.api.listPorts = async () => {
            window.__portsGate.entries++;
            if (armed) {
              armed = false;
              window.__portsGate.entered = true;
              await window.__portsGate.promise;
              __cockpit.api.listPorts = orig;
            }
            return orig();
          };
        }"""
    )
    page.wait_for_function("() => window.__portsGate.entered === true", timeout=4000)
    # Mid-await: replug and click "Reconnect now" IN THE SAME TASK — the
    # manual attempt must win before any poll tick can observe the port.
    connects0 = page.evaluate(
        """() => {
          const c0 = window.__devmockConnects || 0;
          window.__devmockConn.replug();
          document.querySelector('[data-act=reconnect]')?.click();
          return c0;
        }"""
    )
    page.wait_for_function(
        "() => __cockpit.store.connection.state === 'connected'", timeout=6000
    )
    # Release the parked tick; a stale-firing connect would hit a transient
    # failure (the Windows first-open shape) and clobber the session.
    page.evaluate(
        "() => { window.__devmockConn.replug({ failConnects: 1 }); window.__portsGate.release(); }"
    )
    page.wait_for_timeout(1200)
    after = page.evaluate(
        f"""() => ({{
          state: __cockpit.store.connection.state,
          gate: __cockpit.store.gate,
          connects: (window.__devmockConnects || 0) - {connects0},
        }})"""
    )
    check(
        "conn_001 stale poll tick never fires a second connect over the fresh session",
        after["state"] == "connected" and after["gate"] == "matched" and after["connects"] == 1,
        after,
    )
    h0 = page.evaluate("() => __cockpit.histories.get('task1msRuns')?.size || 0")
    page.wait_for_timeout(400)
    h1 = page.evaluate("() => __cockpit.histories.get('task1msRuns')?.size || 0")
    check("conn_001 stream keeps flowing through the raced reconnect", h1 > h0, (h0, h1))

    # ── [test->app~conn_001~1] + views_008: reconnect while PAUSED defers
    #    the recommit — the frozen inspection stays honest until resume ──
    fresh_boot(page)
    VEL = "app_motorControl_data.channels[0].velocityMeasured_radPerSec"
    add_plot_with(page, [VEL], period=1)
    page.wait_for_function(
        f"() => (__cockpit.histories.get({VEL!r})?.size || 0) > 400", timeout=8000
    )
    page.evaluate("() => __cockpit.timeline.pause()")
    frozen = page.evaluate(
        f"""() => {{
          const h = __cockpit.histories.get({VEL!r});
          const win = __cockpit.timeline.get().window;
          const tick = h.tickAtOrBefore((win[0] + win[1]) / 2);
          let w = null; __cockpit.forEachWidget(x => {{ w = w || x; }});
          w.anchor = {{ path: {VEL!r}, tick, value: h.valueAt(tick) }};
          w.renderAnchor();
          return {{ tick, value: h.valueAt(tick), size: h.size, newest: h.newestTick() }};
        }}"""
    )
    installs_b = page.evaluate("() => (window.__devmockInstalls || []).length")
    page.evaluate("() => window.__devmockConn.pull()")
    page.wait_for_function("() => __cockpit.store.connection.state === 'lost'")
    # Baseline AFTER the pull: paused catch-up appends legitimately grow the
    # history until the stream dies; frozen-ness is asserted from here on.
    at_pull = page.evaluate(
        f"""() => {{
          const h = __cockpit.histories.get({VEL!r});
          return {{ size: h.size, newest: h.newestTick() }};
        }}"""
    )
    page.evaluate("() => window.__devmockConn.replug()")
    page.wait_for_function(
        "() => __cockpit.store.connection.state === 'connected'", timeout=8000
    )
    page.wait_for_timeout(1200)  # room for a (wrong) eager recommit to land
    paused_state = page.evaluate(
        f"""() => {{
          const h = __cockpit.histories.get({VEL!r});
          let w = null; __cockpit.forEachWidget(x => {{ w = w || x; }});
          return {{
            installs: (window.__devmockInstalls || []).length,
            size: h.size, newest: h.newestTick(),
            anchorHeld: !!w.anchor,
            readout: h.valueAt({frozen['tick']}),
          }};
        }}"""
    )
    check(
        "conn_001 paused reconnect defers the recommit (no install, histories frozen)",
        paused_state["installs"] == installs_b
        and paused_state["size"] == at_pull["size"]
        and paused_state["newest"] == at_pull["newest"],
        (installs_b, at_pull, paused_state),
    )
    check(
        "views_008 frozen readout still matches the frozen history after reconnect",
        paused_state["readout"] == frozen["value"] and paused_state["anchorHeld"],
        (frozen["value"], paused_state["readout"]),
    )
    page.evaluate("() => __cockpit.timeline.resume()")
    page.wait_for_function(
        f"() => (window.__devmockInstalls || []).length > {installs_b}", timeout=6000
    )
    page.wait_for_timeout(900)  # settle: room for any duplicate install
    resumed = page.evaluate(
        f"""() => {{
          let w = null; __cockpit.forEachWidget(x => {{ w = w || x; }});
          return {{
            installs: (window.__devmockInstalls || []).length - {installs_b},
            anchorHeld: !!w.anchor,
          }};
        }}"""
    )
    check(
        "conn_001 resume fires the deferred recommit exactly once and releases the anchor",
        resumed["installs"] == 1 and not resumed["anchorHeld"],
        resumed,
    )
    page.wait_for_function(
        f"() => (__cockpit.histories.get({VEL!r})?.size || 0) > 100", timeout=8000
    )

    # ── zoom hotkeys vs a focused title editor (capture-phase handler):
    #    Ctrl+'-' zooms while typing still reaches the editor ──
    wid14 = page.evaluate(
        "() => { const w = __cockpit.addWidget({ type: 'plot', signals: [] }); return w.cfg.id; }"
    )
    page.evaluate(
        """(wid) => {
          let w = null; __cockpit.forEachWidget(x => { if (x.cfg.id === wid) w = x; });
          w.el.querySelector('.widget-title').click();
        }""",
        wid14,
    )
    page.wait_for_selector(".widget-title-edit")
    page.keyboard.type("My plot")
    page.keyboard.down("Control")
    page.keyboard.press("-")
    page.keyboard.up("Control")
    zoom_in_editor = page.evaluate("() => document.body.style.zoom")
    typed = page.evaluate("() => document.querySelector('.widget-title-edit')?.value")
    check(
        "ui zoom works while the title editor is focused",
        zoom_in_editor == "0.91" and typed == "My plot",
        (zoom_in_editor, typed),
    )
    page.keyboard.type("!")
    typed2 = page.evaluate("() => document.querySelector('.widget-title-edit')?.value")
    check("typing still reaches the title editor after a zoom combo", typed2 == "My plot!", typed2)
    page.keyboard.press("Escape")
    page.keyboard.down("Control")
    page.keyboard.press("0")
    page.keyboard.up("Control")


BUDGET_FULL = os.environ.get("PCS_RENDER_BUDGET") == "full"
# "off": run the budget scenario for error coverage but skip the numeric
# asserts — for hosts with no GPU-class renderer (CI's SwiftShader measured
# far under the views_015 floor on its first run; the spec's budget is a
# bench/laptop gate, not a shared-runner property). Never silent: the skip
# prints, and draws still must advance (a dead renderer fails everywhere).
BUDGET_OFF = os.environ.get("PCS_RENDER_BUDGET") == "off"


def run_budget(pw):
    """[test->app~views_015~1] the render budget, measured on the reference
    shape: 8 watched signals @ 10 ms on 4 plots (2 each), 30 s span,
    1920x1080 css px @ DPR 1. Headed (the real GPU renders; headless
    SwiftShader is not the machine the app ships on), with the frame-rate
    limiter OFF so the run measures render capability, not the display's
    vsync — a sub-60 Hz monitor can't false-fail, and exceeding 60 means
    real headroom. 60 s when PCS_RENDER_BUDGET=full, a 12 s smoke
    otherwise — both assert the same budget rows: every 1 s window >= 60
    rendered fps, every rendered frame within 33 ms, and the trace
    renderer's draw counter advancing across the whole run (frames that
    tick while draws stall cannot pass)."""
    seconds = 60 if BUDGET_FULL else 12
    browser = pw.chromium.launch(
        headless=False,
        args=[
            "--force-device-scale-factor=1",
            "--disable-frame-rate-limit",
            "--disable-gpu-vsync",
        ],
    )
    ctx = browser.new_context(viewport={"width": 1920, "height": 1080})
    page = ctx.new_page()
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    boot(page)
    page.evaluate(
        """() => {
        const pool = __cockpit.store.signals.filter(s => s.kind === 'f32').map(s => s.path);
        let k = 0;
        for (let p = 0; p < 4; p++) {
          const w = __cockpit.addWidget({ type: 'plot', signals: [] });
          for (let i = 0; i < 2; i++) { const s = pool[k++]; __cockpit.addWatch(s, 10); w.addSignal(s); }
        }
        __cockpit.timeline.setSpan(30000);
      }"""
    )
    page.wait_for_function(
        "() => __cockpit.store.traceStatus && __cockpit.store.traceStatus.link_rate_bytes_per_s > 0",
        timeout=8000,
    )
    page.wait_for_timeout(3000)  # settle past the backfill
    min_fps, max_frame = 1e9, 0.0
    draws0 = page.evaluate("() => __cockpit.perf.snapshot().draws")
    batches0 = page.evaluate("() => window.__devmockBatches || 0")
    for _ in range(seconds):
        page.wait_for_timeout(1000)
        s = page.evaluate("() => __cockpit.perf.snapshot()")
        min_fps = min(min_fps, s["fps"])
        max_frame = max(max_frame, s["worst_ms"])
    draws = page.evaluate("() => __cockpit.perf.snapshot().draws") - draws0
    batches = page.evaluate("() => window.__devmockBatches || 0") - batches0
    # 4 plots x ~20 batches/s; 15/s of slack covers batch coalescing.
    min_draws = seconds * 15 * 4
    if BUDGET_OFF:
        print(
            f"  [skip] views_015 budget asserts (PCS_RENDER_BUDGET=off, "
            f"software-GL host) — observed min_fps={min_fps} max_frame={max_frame:.1f} "
            f"batches={batches}"
        )
        # A starved runner delivers a fraction of the nominal 20 Hz batch
        # cadence (first CI run: 4 fps, ~1/4 of the batches), so the floor
        # scales to the batches the host ACTUALLY emitted — "every batch
        # drew on every plot, with coalescing slack" — with an absolute
        # minimum so a dead renderer (zero draws) fails on any host.
        min_draws_off = max(20, int(batches * 4 * 0.75))
        check(
            f"views_015 renderer draws advance over {seconds} s (budget asserts off)",
            draws >= min_draws_off,
            (draws, min_draws_off, batches),
        )
    else:
        check(
            f"views_015 render budget holds over {seconds} s (min 1s fps, max frame ms, draws)",
            min_fps >= 60 and max_frame <= 33.0 and draws >= min_draws,
            (min_fps, max_frame, draws, min_draws),
        )
    check("views_015 run raised no page errors", not errors, errors[:3])
    browser.close()


def main():
    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        page = browser.new_page(viewport={"width": 1440, "height": 900})
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        run(page)
        check("no page errors", not errors, errors[:3])
        browser.close()
        run_budget(pw)
    if FAILURES:
        print(f"FAILED: {len(FAILURES)}: {FAILURES}")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
