"""Playwright suite for the cockpit's views specs, run against dist/ over
file:// with the browser devmock (no board, no Tauri).

Run:  .venv/Scripts/python sw/gui/tests/test_views.py
"""

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

    # Two plots for the shared-cursor assertions.
    add_plot_with(page, ["app_motorControl_data.channels[0].velocityMeasured_radPerSec"], 1)
    wait_for_samples(page, "task1msRuns")
    wait_for_samples(page, "app_motorControl_data.channels[0].velocityMeasured_radPerSec")

    # ── [test->app~views_001~1] traces render; a tick-count gap breaks them ──
    plot_ready = page.evaluate(
        "() => { let n = 0; __cockpit.forEachWidget(w => { if (w.uplot?.data[0].length > 40) n++; }); return n; }"
    )
    check("views_001 both plots hold trace data", plot_ready == 2, plot_ready)
    # Wait past a mock gap window (every 5 s), then assert: an explicit null
    # marker in the plot data (the line break) AND an accent gap-ribbon span.
    page.wait_for_function(
        "() => __cockpit.histories.get('task1msRuns')?.gaps.length >= 1", timeout=9000
    )
    has_null = page.evaluate(
        """() => { let found = false;
          __cockpit.forEachWidget(w => { if (!w.uplot) return;
            for (let s = 1; s < w.uplot.data.length; s++)
              if (w.uplot.data[s].some(v => v === null)) found = true; });
          return found; }"""
    )
    check("views_001 gap yields a broken trace (explicit null)", has_null)
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
    page.evaluate(
        f"() => __cockpit.forEachWidget(w => {{ if (w.cfg.id === '{wid}' && w.cfg.signals.length < 2)"
        " w.addSignal('app_motorControl_data.channels[0].velocityMeasured_radPerSec'); })"
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

    # Auto vs manual on a scratch plot fed a monotonic (ramp-kind) signal.
    ramp = page.evaluate(
        "() => (__cockpit.store.signals.find(s => (s.kind === 'u16' || s.kind === 'u32')"
        " && !__cockpit.store.watched.has(s.path)) || {}).path || None"
        .replace("None", "null")
    )
    check("views_007 a ramp-kind signal exists for the auto check", ramp is not None, ramp)
    page.evaluate(f"() => __cockpit.addWatch({ramp!r}, 1)")
    scratch = page.evaluate(
        f"() => {{ const w = __cockpit.addWidget({{ type: 'plot', signals: [] }}); w.addSignal({ramp!r}); return w.cfg.id; }}"
    )
    wait_for_samples(page, ramp, 100)

    def scale_l(widget_id):
        return page.evaluate(
            f"() => {{ let m = null; __cockpit.forEachWidget(w => {{ if (w.cfg.id === '{widget_id}' && w.uplot && w.uplot.scales.L) m = [w.uplot.scales.L.min, w.uplot.scales.L.max]; }}); return m; }}"
        )

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
    left_state = page.evaluate(
        f"() => {{ let s = null; __cockpit.forEachWidget(w => {{ if (w.cfg.id === '{scratch}') s = {{ n: w.cfg.signals.length,"
        " hint: !w.el.querySelector('.plot-empty-hint').hidden } }); return s; }"
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
    page.evaluate(
        f"() => __cockpit.forEachWidget(w => {{ if (w.cfg.id === '{scratch}') w.hooks.onRemove(w.cfg.id); }})"
    )
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

    # zoom-in floor: the spec's 50 ms bound
    page.evaluate(
        "() => { const w = __cockpit.timeline.get().window;"
        " __cockpit.timeline.zoomAt((w[0] + w[1]) / 2, 1e-9); }"
    )
    floor_w = page.evaluate("() => { const w = __cockpit.timeline.get().window; return w[1] - w[0]; }")
    check("views_009 zoom-in stops at 50 ms", abs(floor_w - 50) < 1, floor_w)

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
    page.reload()
    page.wait_for_function("() => window.__cockpit !== undefined")
    page.evaluate("() => __cockpit.api.loadElf('mock.elf')")
    page.wait_for_function("() => document.querySelectorAll('.widget').length > 0", timeout=5000)
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
    page.evaluate("() => localStorage.clear()")
    boot(page)
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
    page.reload()
    page.wait_for_function("() => window.__cockpit !== undefined")
    page.evaluate("() => __cockpit.api.loadElf('mock.elf')")
    page.wait_for_function("() => document.querySelectorAll('.plot-widget').length > 0", timeout=5000)
    restored_h = page.locator(".plot-widget").first.bounding_box()["height"]
    check(
        "views_004 resized height survives restart",
        abs(restored_h - after_h) <= 2,
        (after_h, restored_h),
    )

    # ── [test->app~obs_005~1] filter: substring / glob / regex / invalid ──
    boot(page)

    def filter_paths(q):
        page.fill(".picker-search input", q)
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

    sub = filter_paths("velocity")
    check(
        "obs_005 substring narrows to matches",
        len(sub) > 0 and all("velocity" in p.lower() for p in sub) and len(sub) < full,
        (len(sub), full),
    )
    glob = filter_paths("chan*velocity")
    check(
        "obs_005 glob * matches any run",
        len(glob) > 0 and all("chan" in p.lower() and "velocity" in p.lower() for p in glob),
        glob[:3],
    )
    rex = filter_paths(r"buf\[1[0-2]\]")
    check(
        "obs_005 regex matches exactly",
        sorted(rex) == [f"est_flux_data.buf[{i}]" for i in (10, 11, 12)],
        rex[:5],
    )
    bad = filter_paths("velocity(")
    check("obs_005 invalid regex falls back to substring", bad == [], bad[:3])
    restored = filter_paths("")
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
    page.reload()
    page.wait_for_function("() => window.__cockpit !== undefined")
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
          __cockpit.forEachWidget(w => {{ if (!w.uplot) return;
            const i = w.cfg.signals.indexOf({sig_a!r});
            if (i < 0) return;
            const s = w.uplot.series[i + 1];
            // dots via the widget's series-opts mapping: uPlot normalizes
            // points internally, so the live object hides the plain flag.
            out.push({{ dash: s.dash, dots: w.seriesOpts({sig_a!r}).points.show, name: s.paths.name }}); }});
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
    for mode, builder in (("zoh", "steppedPaths"), ("cubic", "monotonePaths"), ("linear", "linearPaths")):
        page.evaluate(f"() => __cockpit.appearance.set({sig_a!r}, {{ interp: {mode!r} }})")
        state = page.evaluate(
            f"""() => {{ const out = {{ names: [], hasNull: false }};
              __cockpit.forEachWidget(w => {{ if (!w.uplot) return;
                const i = w.cfg.signals.indexOf({sig_a!r});
                if (i < 0) return;
                out.names.push(w.uplot.series[i + 1].paths.name);
                if (w.uplot.data[i + 1].some(v => v === null)) out.hasNull = true; }});
              return out; }}"""
        )
        check(
            f"views_011 {mode} uses {builder} and keeps gap markers",
            state["names"] and all(n == builder for n in state["names"]) and state["hasNull"],
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
            if (res || !w.uplot || w.cfg.signals.length !== 2) return;
            // The grid scrolls: the widget must be truly on screen, or the
            // real mouse events land on whatever covers those coordinates.
            w.el.scrollIntoView({{ block: 'center' }});
            w.refresh();
            const canvas = w.el.querySelector('.plot-canvas');
            const inCanvas = ([x, y]) =>
              document.elementFromPoint(x, y)?.closest('.plot-canvas') === canvas;
            const rect = canvas.getBoundingClientRect();
            const u = w.uplot, xs = u.data[0];
            for (const frac of [0.5, 0.4, 0.6, 0.3, 0.7]) {{
              const px = rect.width * frac;
              const t = u.posToVal(px, 'x');
              const ys = [];
              for (let s = 1; s < u.data.length; s++) {{
                const col = u.data[s];
                let bi = -1, bd = 1e18;
                for (let i = 0; i < xs.length; i++) {{
                  if (col[i] == null) continue;
                  const d = Math.abs(xs[i] - t);
                  if (d < bd) {{ bd = d; bi = i; }}
                }}
                ys.push(bi < 0 ? null : u.valToPos(col[bi], u.series[s].scale, false));
              }}
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
          __cockpit.forEachWidget(w => { if (w.cfg.id === id && w.uplot)
            out = w.uplot.series.slice(1).map(s => s.width); });
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
    page.reload()
    page.wait_for_function("() => window.__cockpit !== undefined")
    restored = page.evaluate(f"() => __cockpit.appearance.of({sig_a!r})")
    check(
        "views_011 appearance persists across restart",
        restored["color"] == "#ff0000" and restored["style"] == "dashed" and restored["dots"],
        restored,
    )
    base_widths = page.evaluate(
        """() => { const out = [];
          __cockpit.forEachWidget(w => { if (w.uplot) out.push(...w.uplot.series.slice(1).map(s => s.width)); });
          return out; }"""
    )
    check(
        "views_012 emphasis is transient — base stroke widths after restart",
        all(w == 1.5 for w in base_widths),
        base_widths,
    )

    # ═══ batch 4: hide-const, group collapse, session restore, prefs ═══

    # ── [test->app~obs_006~1] the exclusion narrows to writable ∩ matches ──
    page.evaluate("() => localStorage.clear()")
    boot(page)
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
    page.reload()
    page.wait_for_function("() => window.__cockpit !== undefined")
    page.wait_for_function("() => __cockpit.store.gate === 'matched'", timeout=8000)
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
    page.evaluate("() => localStorage.clear()")
    boot(page)
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
        "() => { let d = null; __cockpit.forEachWidget(w => { if (!w.uplot) return;"
        "  const s = w.uplot.series.find(s => s.label === 'task1msRuns' || (s.paths && s._path === 'task1msRuns'));"
        "  const last = w.uplot.series[w.uplot.series.length - 1];"
        "  d = Boolean((s || last).dash && (s || last).dash.length); }); return d; }"
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
    page.reload()
    page.wait_for_function("() => window.__cockpit !== undefined")
    page.evaluate("() => __cockpit.api.loadElf('mock.elf')")
    page.wait_for_function("() => document.querySelectorAll('.plot-widget').length > 0", timeout=5000)
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


def main():
    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        page = browser.new_page(viewport={"width": 1440, "height": 900})
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        run(page)
        check("no page errors", not errors, errors[:3])
        browser.close()
    if FAILURES:
        print(f"FAILED: {len(FAILURES)}: {FAILURES}")
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
