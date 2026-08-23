// Connection bar, telemetry strip, identity gate scrim, and the connection
// lifecycle (pre-connection picker, lost-port auto-reconnect).

import { icon } from "./icons.js";
import { api, store, subscribe, prefs } from "./state.js";
import { pickFile } from "./bridge.js";
import { perfCellText } from "./perf.js";

const $ = (sel) => document.querySelector(sel);
const esc = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

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
  if (c.state === "lost") {
    // Designed here (state missing from the handoff): warn surface, no
    // animation, the reconnect story in words.
    return `
      <span class="conn-port conn-port--lost">
        <span class="conn-port-dot"></span>
        <span class="conn-port-path">${esc(store.lastPort ?? "—")}</span>
        <span class="conn-identity-meta">port lost — reconnecting when it returns</span>
        <button class="btn btn-secondary" data-act="reconnect">Reconnect now</button>
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

// [impl->app~views_002~1]
function renderTelemetry() {
  const t = store.telemetry;
  const dash = `<span class="slash">—</span>`;
  const strip = $(".telemetry-strip");
  const cells = [
    ["link", store.linkHz ? `${icon("activity")}${Math.round(store.linkHz)} Hz` : dash, "telemetry-cell--link"],
    ["render", `<span data-perf-cell>${perfCellText()}</span>`],
    ["drive state", t ? `${esc(t.state.replace("DRIVE_STATE_", ""))} · ${esc(t.mode.replace("MODE_", ""))}` : dash],
    ["vbus", t ? `${t.bus_voltage_v.toFixed(2)} V` : dash],
    ["ibus", t ? `${t.bus_current_a.toFixed(3)} A` : dash],
    [
      "velocity — cmd / meas",
      t
        ? `${(t.velocity_setpoint_radps * RAD_TO_RPM).toFixed(1)} <span class="slash">/</span> <span class="meas">${(t.velocity_measured_radps * RAD_TO_RPM).toFixed(1)}</span> rpm`
        : dash,
    ],
    ["tick", t ? `${thin(t.timestamp_ms)} ms` : dash],
  ];
  strip.innerHTML =
    cells
      .map(
        ([label, value, cls]) => `
      <div class="telemetry-cell ${cls || ""}">
        <span class="field-label">${label}</span>
        <span class="telemetry-value">${value}</span>
      </div>`
      )
      .join("") +
    `<div class="telemetry-tags">
      <span class="tag tag-accent">${store.watched.size} signals watched</span>
      <span class="tag tag-neutral">${store.gapCount} gaps</span>
    </div>`;
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
    store.connectError = String(e);
    renderConnBar();
  }
}

let reconnectTimer = null;
function startReconnectPoll() {
  // The port takes ~2 s to re-enumerate after a reset; poll until it is
  // back, then reconnect without user action (app~conn_001's session spec —
  // implemented core-side; this drives the retry loop).
  if (reconnectTimer) return;
  reconnectTimer = setInterval(async () => {
    if (store.connection.state !== "lost") {
      clearInterval(reconnectTimer);
      reconnectTimer = null;
      return;
    }
    const ports = await api.listPorts();
    if (store.lastPort && ports.some((p) => p.name === store.lastPort)) {
      try {
        await api.connect(store.lastPort);
      } catch {
        /* port present but not ready yet — next poll retries */
      }
    }
  }, 1000);
}

export function initChrome() {
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
        if (store.lastPort) await api.connect(store.lastPort).catch(() => {});
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
      document.querySelector(".app-shell").classList.toggle("app-stale", store.connection.state === "lost");
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
