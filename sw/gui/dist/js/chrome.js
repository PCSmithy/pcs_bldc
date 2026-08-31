// Connection bar, telemetry strip, identity gate scrim, and the connection
// lifecycle (pre-connection picker, lost-port auto-reconnect).

import { icon } from "./icons.js";
import { api, store, set, subscribe, prefs } from "./state.js";
import { pickFile, invoke, isTauri } from "./bridge.js";
import { perfCellText } from "./perf.js";
import { $, esc } from "./dom.js";

const THEMES = [
  ["warm", "#f6a06b"],
  ["graphite", "#5fd8e8"],
  ["neon", "#ff6fe0"],
];

// ── connection bar ──────────────────────────────────────────────────────────

function connPortChip() {
  const c = store.connection;
  if (c.state === "connected") {
    return `
      <span class="conn-port">
        <span class="conn-port-dot"></span>
        <span class="conn-port-path">${esc(c.port)}</span>
        <button class="btn btn-secondary" data-act="disconnect">Disconnect</button>
      </span>`;
  }
  if (c.state === "lost" || (c.state === "connecting" && reconnectInFlight)) {
    // A reconnect ATTEMPT keeps this pill (with a progress note) — flipping
    // to the picker once a second reads as a strobe.
    const attempting = c.state === "connecting";
    return `
      <span class="conn-port conn-port--lost">
        <span class="conn-port-dot"></span>
        <span class="conn-port-path">${esc(store.lastPort ?? "—")}</span>
        <span class="conn-identity-meta">${attempting ? "reconnecting…" : "port lost — reconnecting when it returns"}</span>
        ${!attempting && store.connectError ? `<span class="conn-error">${esc(store.connectError)}</span>` : ""}
        <button class="btn btn-secondary" data-act="reconnect" ${attempting ? "disabled" : ""}>Reconnect now</button>
      </span>`;
  }
  // pre-connection: the pill becomes the picker
  const opts = store.ports
    .map((p) => `<option value="${esc(p.name)}" ${p.name === store.lastPort ? "selected" : ""}>${esc(p.name)} — ${esc(p.kind)}</option>`)
    .join("");
  const busy = c.state === "connecting";
  return `
    <span class="conn-picker">
      <select data-role="port" aria-label="Serial port" ${busy ? "disabled" : ""}>
        ${opts || `<option value="">no ports found</option>`}
      </select>
      ${icon("chevron-down")}
      <button class="btn btn-secondary" data-act="refresh" ${busy ? "disabled" : ""}>Refresh</button>
      <button class="btn btn-primary" data-act="connect" ${busy || !store.ports.length ? "disabled" : ""}>
        ${busy ? "Connecting…" : "Connect"}
      </button>
    </span>
    ${store.connectError ? `<span class="conn-error">${esc(store.connectError)}</span>` : ""}`;
}

function identityChip() {
  const { gate, connection, elf } = store;
  if (gate === "offline") return "";
  if (gate === "no-elf") {
    return `
      <span class="conn-identity conn-identity--idle">
        <span class="conn-identity-word">no .elf</span>
        <span class="conn-identity-div"></span>
        <span class="conn-identity-meta">load the firmware .elf to resolve signals</span>
      </span>`;
  }
  if (gate === "matched") {
    return `
      <span class="conn-identity conn-identity--matched">
        <span class="conn-identity-word">matched</span>
        <span class="conn-identity-div"></span>
        <span class="conn-identity-build">${esc(connection.buildId)}</span>
        <span class="conn-identity-meta">${elf.signalCount} symbols resolve</span>
      </span>`;
  }
  return `
    <span class="conn-identity conn-identity--mismatch">
      ${icon("alert-triangle")}
      <span class="conn-identity-word">mismatch</span>
      <span class="conn-identity-div"></span>
      <span class="conn-identity-build">board ${esc(connection.buildId)} · elf ${esc(elf.buildId)}</span>
      <span class="conn-identity-meta">tracing off</span>
    </span>
    <span class="conn-gate-note">Telemetry and logs keep flowing.<a href="#" data-act="choose-elf">Load the matching .elf</a></span>`;
}

function elfGroup() {
  const { elf, gate } = store;
  const primary = gate === "mismatch" || gate === "no-elf";
  return `
    <span class="conn-elf">
      <span class="field-label">elf</span>
      <span class="conn-elf-path">${esc(elf.path ?? "—")}</span>
      <button class="btn ${primary ? "btn-primary" : "btn-secondary"}" data-act="choose-elf">
        ${elf.path && !primary ? "Reload" : "Choose…"}
      </button>
    </span>`;
}

function themeSwitch() {
  const current = document.documentElement.dataset.theme || "warm";
  return `
    <span class="theme-switch" role="group" aria-label="Theme">
      ${THEMES.map(
        ([name, swatch]) => `
        <button data-theme-pick="${name}" aria-pressed="${name === current}">
          <span class="theme-swatch" style="background:${swatch}"></span>${name[0].toUpperCase()}${name.slice(1)}
        </button>`
      ).join("")}
    </span>`;
}

function renderConnBar() {
  const bar = $(".conn-bar");
  const warn = store.gate === "mismatch" || store.connection.state === "lost";
  bar.classList.toggle("conn-bar--warn", warn);
  bar.innerHTML = `
    <span class="app-mark">pcs_bldc</span>
    ${connPortChip()}
    ${identityChip()}
    ${elfGroup()}
    ${themeSwitch()}`;
}

// ── telemetry strip ─────────────────────────────────────────────────────────

const RAD_TO_RPM = 60 / (2 * Math.PI);
const thin = (num) => num.toLocaleString("en-US").replaceAll(",", " ");

const DASH_HTML = `<span class="slash">—</span>`;

// Skeleton built once; ticks rewrite only the value cells that changed
// (the [data-perf-cell] node stays put across updates).
let telemEls = null;

function buildTelemetryStrip() {
  const strip = $(".telemetry-strip");
  const defs = [
    ["link", "telemetry-cell--link"],
    ["render"],
    ["drive state"],
    ["vbus"],
    ["ibus"],
    ["velocity — cmd / meas"],
    ["tick"],
  ];
  strip.innerHTML =
    defs
      .map(
        ([label, cls]) => `
      <div class="telemetry-cell ${cls || ""}">
        <span class="field-label">${label}</span>
        <span class="telemetry-value">${label === "render" ? `<span data-perf-cell></span>` : ""}</span>
      </div>`
      )
      .join("") +
    `<div class="telemetry-tags">
      <span class="tag tag-accent"></span>
      <span class="tag tag-neutral"></span>
    </div>`;
  const values = [...strip.querySelectorAll(".telemetry-value")];
  telemEls = {
    values,
    perf: strip.querySelector("[data-perf-cell]"),
    tagWatched: strip.querySelector(".tag-accent"),
    tagGaps: strip.querySelector(".tag-neutral"),
    last: new Array(values.length).fill(null),
  };
}

// [impl->app~views_002~1]
function renderTelemetry() {
  if (!telemEls) buildTelemetryStrip();
  const t = store.telemetry;
  const vals = [
    store.linkHz ? `${icon("activity")}${Math.round(store.linkHz)} Hz` : DASH_HTML,
    null, // render: the perf node updates by textContent below
    t ? `${esc(t.state.replace("DRIVE_STATE_", ""))} · ${esc(t.mode.replace("MODE_", ""))}` : DASH_HTML,
    t ? `${t.bus_voltage_v.toFixed(2)} V` : DASH_HTML,
    t ? `${t.bus_current_a.toFixed(3)} A` : DASH_HTML,
    t
      ? `${(t.velocity_setpoint_radps * RAD_TO_RPM).toFixed(1)} <span class="slash">/</span> <span class="meas">${(t.velocity_measured_radps * RAD_TO_RPM).toFixed(1)}</span> rpm`
      : DASH_HTML,
    t ? `${thin(t.timestamp_ms)} ms` : DASH_HTML,
  ];
  vals.forEach((html, i) => {
    if (html === null) return;
    if (telemEls.last[i] !== html) {
      telemEls.last[i] = html;
      telemEls.values[i].innerHTML = html;
    }
  });
  telemEls.perf.textContent = perfCellText();
  const watched = `${store.watched.size} signals watched`;
  if (telemEls.tagWatched.textContent !== watched) telemEls.tagWatched.textContent = watched;
  const gaps = `${store.gapCount} gaps`;
  if (telemEls.tagGaps.textContent !== gaps) telemEls.tagGaps.textContent = gaps;
}

// ── identity gate scrim ─────────────────────────────────────────────────────

function renderGate() {
  const host = $(".trace-gate");
  if (store.gate !== "mismatch") {
    host.hidden = true;
    return;
  }
  host.hidden = false;
  host.innerHTML = `
    <div class="trace-gate-note">
      <h2 class="gate-title display">${icon("alert-triangle")}Tracing is paused — build identity doesn't match</h2>
      <p class="gate-body">
        The board reports <code>${esc(store.connection.buildId)}</code>; the loaded .elf is
        <code>${esc(store.elf.buildId)}</code>. Symbol addresses from a different build would read
        the wrong memory, so the watch list is held. Telemetry and the log keep streaming.
      </p>
      <div class="gate-actions">
        <button class="btn btn-primary btn-lg" data-act="choose-elf">Choose matching .elf…</button>
      </div>
      <span class="gate-foot">Layout and watch list are preserved — tracing resumes the moment identities agree.</span>
    </div>`;
}

// ── lifecycle actions ───────────────────────────────────────────────────────

async function chooseElf() {
  const path = await pickFile([{ name: "Firmware ELF", extensions: ["elf"] }]);
  if (!path) return;
  try {
    await api.loadElf(path);
    await api.listSignals("");
  } catch (e) {
    set({ connectError: String(e) });
  }
}

let reconnectTimer = null;
// One in-flight guard shared by the poll and the "Reconnect now" button:
// the core's connect() begins with a session teardown, so a second attempt
// racing a first would tear down whatever the first just established.
let reconnectInFlight = false;

/** One guarded reconnect attempt (poll tick + button). The claim is taken
 *  SYNCHRONOUSLY — no await before it — so two callers can never both reach
 *  connect (which begins with a session teardown). Lost-state only. */
async function attemptReconnect() {
  if (reconnectInFlight || !store.lastPort) return;
  if (store.connection.state !== "lost") return;
  reconnectInFlight = true;
  renderConnBar(); // the lost pill's "reconnecting…" note keys on the flag
  try {
    await api.connect(store.lastPort);
  } catch {
    /* port present but not ready yet — the poll retries */
  } finally {
    reconnectInFlight = false;
    renderConnBar();
  }
}

// [impl->app~conn_001~1] the retry loop behind "re-opens without user
// action": poll while lost, reconnect when the port returns.
function startReconnectPoll() {
  // Race discipline: the lost state is re-checked after the list_ports
  // await (it can outlive a tick), and attemptReconnect's synchronous claim
  // excludes doubled connects; the claim deliberately does NOT span the
  // list_ports await — that would deadlock the "Reconnect now" button.
  if (reconnectTimer) return;
  reconnectTimer = setInterval(async () => {
    if (reconnectInFlight || store.connection.state === "connecting") return;
    if (store.connection.state !== "lost") {
      clearInterval(reconnectTimer);
      reconnectTimer = null;
      return;
    }
    const ports = await api.listPorts();
    if (store.connection.state !== "lost") return; // changed mid-await: stale tick
    if (store.lastPort && ports.some((p) => p.name === store.lastPort)) {
      await attemptReconnect();
    }
  }, 1000);
}

// ── UI zoom: Ctrl+'+'/'-'/'0' and Ctrl+wheel, persisted. Tauri uses the
// native webview zoom (set_zoom); the browser/devmock path falls back to
// body CSS zoom so the binding logic stays testable.

const ZOOM_KEY = "cockpit.ui.zoom.v1";
const ZOOM_STEP = 1.1;
const ZOOM_MIN = 0.5;
const ZOOM_MAX = 2.0;
let uiZoom = 1;

function applyUiZoom(factor) {
  uiZoom = Math.round(Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, factor)) * 100) / 100;
  if (isTauri) invoke("set_zoom", { factor: uiZoom }).catch(() => {});
  else document.body.style.zoom = uiZoom === 1 ? "" : String(uiZoom);
  prefs.set(ZOOM_KEY, uiZoom);
}

function initUiZoom() {
  const saved = Number(prefs.get(ZOOM_KEY));
  if (Number.isFinite(saved) && saved > 0 && saved !== 1) applyUiZoom(saved);
  // Capture phase: focused editors (the widget title input) stopPropagation
  // on bubble-phase keydown, which would starve this handler and let the
  // webview's NATIVE Ctrl+±/0 accelerator fire unclamped, desyncing uiZoom.
  // Capture runs before any bubble-phase stop; preventDefault still keeps
  // the native accelerator from double-firing.
  window.addEventListener(
    "keydown",
    (ev) => {
      if (!ev.ctrlKey || ev.altKey || ev.metaKey) return;
      if (ev.key === "+" || ev.key === "=") {
        ev.preventDefault();
        applyUiZoom(uiZoom * ZOOM_STEP);
      } else if (ev.key === "-" || ev.key === "_") {
        ev.preventDefault();
        applyUiZoom(uiZoom / ZOOM_STEP);
      } else if (ev.key === "0") {
        ev.preventDefault();
        applyUiZoom(1);
      }
    },
    true,
  );
  // Ctrl+wheel is UI zoom everywhere EXCEPT over a paused plot, where the
  // wheel is the views_009 range-zoom gesture (Ctrl included).
  window.addEventListener(
    "wheel",
    (ev) => {
      if (!ev.ctrlKey) return;
      if (store.timeline.mode === "paused" && ev.target.closest?.(".plot-canvas")) return;
      ev.preventDefault();
      applyUiZoom(ev.deltaY < 0 ? uiZoom * ZOOM_STEP : uiZoom / ZOOM_STEP);
    },
    { passive: false, capture: true },
  );
}

export function initChrome() {
  initUiZoom();
  document.body.addEventListener("click", async (ev) => {
    const themePick = ev.target.closest("[data-theme-pick]");
    if (themePick) {
      const name = themePick.dataset.themePick;
      if (name === "warm") delete document.documentElement.dataset.theme;
      else document.documentElement.dataset.theme = name;
      prefs.set("pcs-theme", name);
      renderConnBar();
      return;
    }
    const act = ev.target.closest("[data-act]");
    if (!act) return;
    ev.preventDefault();
    switch (act.dataset.act) {
      case "refresh":
        await api.listPorts();
        break;
      case "connect": {
        const port = $('[data-role="port"]')?.value;
        if (port) await api.connect(port).catch(() => {});
        break;
      }
      case "reconnect":
        await attemptReconnect();
        break;
      case "disconnect":
        await api.disconnect();
        break;
      case "choose-elf":
        await chooseElf();
        break;
    }
  });

  const saved = prefs.get("pcs-theme");
  if (saved && saved !== "warm") document.documentElement.dataset.theme = saved;

  for (const topic of ["connection", "ports", "elf", "gate", "connectError"]) {
    subscribe(topic, () => {
      renderConnBar();
      renderGate();
      const lostish =
        store.connection.state === "lost" ||
        (store.connection.state === "connecting" && reconnectInFlight);
      document.querySelector(".app-shell").classList.toggle("app-stale", lostish);
      if (store.connection.state === "lost") startReconnectPoll();
    });
  }
  for (const topic of ["telemetry", "watched", "gapCount", "connection"]) {
    subscribe(topic, renderTelemetry);
  }
  renderConnBar();
  renderTelemetry();
  renderGate();
}
