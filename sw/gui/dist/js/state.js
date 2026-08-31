// App state and the backend subscription fan-out. The chrome renders FROM
// this store; the workspace module (plots/table/cursor/watch flow) builds
// against the same surface:
//
//   store            read-only snapshot fields (mutate via set()/helpers)
//   subscribe(t, fn) topics: connection | gate | ports | elf | signals |
//                    telemetry | log | traceStatus | samples | watched |
//                    timeline (set() topics are store keys, camelCase)
//   set(patch)       shallow-merge + notify the affected topics
//   api.*            typed wrappers over the Tauri commands
//
// "samples" batches are fanned out verbatim (~20 Hz) and never stored here —
// the plot store owns sample history and its decimation.

import { invoke, listen, isTauri } from "./bridge.js";

// ── prefs: the persisted session context — one JSON config file via the
// core (load_config/save_config); localStorage stands in under the browser
// devmock. Hydrated ONCE at boot (initPrefs, before any module reads);
// writes are whole-object, debounced. (app~arch_002's storage.)

const PREFS_BROWSER_KEY = "cockpit.config.v1";
// Legacy per-module localStorage keys, imported once when absent from the
// config so pre-config-file sessions keep their layouts and theme.
const PREFS_MIGRATE = [
  "cockpit.workspace.v1",
  "cockpit.timeline.span.v1",
  "cockpit.picker.v1",
  "pcs-theme",
];

let prefsObj = {};
let prefsSaveTimer = null;

function flushPrefs() {
  clearTimeout(prefsSaveTimer);
  prefsSaveTimer = null;
  if (isTauri) invoke("save_config", { value: prefsObj }).catch(() => {});
  else localStorage.setItem(PREFS_BROWSER_KEY, JSON.stringify(prefsObj));
}

function schedulePrefsSave() {
  // The debounce batches config-FILE writes; the browser fallback is a cheap
  // synchronous localStorage write-through.
  if (!isTauri) return flushPrefs();
  clearTimeout(prefsSaveTimer);
  prefsSaveTimer = setTimeout(flushPrefs, 500);
}

export const prefs = {
  get(key, fallback = null) {
    return key in prefsObj ? prefsObj[key] : fallback;
  },
  set(key, value) {
    prefsObj[key] = value;
    schedulePrefsSave();
  },
};

export async function initPrefs() {
  if (isTauri) {
    prefsObj = (await invoke("load_config").catch(() => null)) || {};
  } else {
    try {
      prefsObj = JSON.parse(localStorage.getItem(PREFS_BROWSER_KEY) || "{}");
    } catch {
      prefsObj = {};
    }
  }
  let migrated = false;
  for (const key of PREFS_MIGRATE) {
    if (key in prefsObj) continue;
    const raw = localStorage.getItem(key);
    if (raw === null) continue;
    try {
      prefsObj[key] = JSON.parse(raw);
    } catch {
      prefsObj[key] = raw; // plain-string legacy value (the theme name)
    }
    migrated = true;
  }
  if (migrated) schedulePrefsSave();
  // A pending debounced write must not be lost to a fast app close.
  window.addEventListener("pagehide", () => {
    if (prefsSaveTimer) flushPrefs();
  });
}

export const store = {
  connection: { state: "disconnected", port: null, buildId: null }, // 'disconnected'|'connecting'|'connected'|'lost'
  lastPort: null,           // survives 'lost' for auto-reconnect
  connectError: null,       // verbatim backend error string
  ports: [],
  elf: { path: null, buildId: null, signalCount: 0 },
  gate: "offline",          // derived: 'offline'|'no-elf'|'matched'|'mismatch'
  signals: [],              // last list_signals result
  telemetry: null,          // last TelemetryEvent
  linkHz: 0,                // measured telemetry arrival rate
  traceStatus: null,        // last TraceStatusInfo
  budgetVerdict: "—",       // 'accepted' | rejection cause | '—'
  watched: new Map(),       // path -> { period_ms }  (workspace-owned)
  gapCount: 0,              // cumulative dropped ticks from samples batches
  // The plot timeline (workspace-owned, see workspace/timeline.js; mutations
  // notify the "timeline" topic). window/pausedSpan are [t0, t1] ms or null.
  timeline: { span_ms: 10_000, mode: "live", window: null, pausedSpan: null },
};

const subs = new Map();
export function subscribe(topic, fn) {
  if (!subs.has(topic)) subs.set(topic, new Set());
  subs.get(topic).add(fn);
  return () => subs.get(topic).delete(fn);
}
export function notify(topic, payload) {
  for (const fn of subs.get(topic) || []) fn(payload);
}

function deriveGate() {
  const s = store;
  const gate =
    s.connection.state !== "connected" ? "offline"
    : !s.elf.buildId ? "no-elf"
    : s.elf.buildId === s.connection.buildId ? "matched"
    : "mismatch";
  if (gate !== s.gate) {
    s.gate = gate;
    notify("gate", gate);
  }
}

export function set(patch) {
  Object.assign(store, patch);
  for (const key of Object.keys(patch)) notify(key, store[key]);
  deriveGate();
}

// Monotonic connect-attempt counter (see api.connect's failure guard).
let connectEpoch = 0;

export const api = {
  async listPorts() {
    const ports = await invoke("list_ports");
    set({ ports });
    return ports;
  },
  async connect(port) {
    // A failed attempt from the lost state returns TO the lost state: the
    // reconnect poll keys on it, and a replugged port routinely rejects the
    // first open while the OS finishes enumeration — that must read as
    // "still waiting", never as a user-facing disconnect. The epoch guards
    // the failure path: a STALE attempt's failure (a newer attempt has
    // since started or landed) must never clobber the newer outcome —
    // without it, a raced reconnect could paint "disconnected" over a
    // healthy session.
    const epoch = ++connectEpoch;
    const wasLost = store.connection.state === "lost";
    set({ connection: { ...store.connection, state: "connecting" }, connectError: null });
    try {
      const buildId = await invoke("connect", { port });
      set({ connection: { state: "connected", port, buildId }, lastPort: port });
      prefs.set("cockpit.session.port", port);
      return buildId;
    } catch (e) {
      if (epoch === connectEpoch) {
        set({
          connection: { state: wasLost ? "lost" : "disconnected", port: null, buildId: null },
          connectError: String(e),
        });
      } else {
        set({ connectError: String(e) }); // stale failure: surface the reason only
      }
      throw e;
    }
  },
  async disconnect() {
    await invoke("disconnect");
    set({ connection: { state: "disconnected", port: null, buildId: null } });
    // A deliberate disconnect ends the session: the next boot must not
    // auto-reconnect to this port.
    prefs.set("cockpit.session.port", null);
  },
  async getStatus() {
    return invoke("get_status");
  },
  async loadElf(path) {
    const info = await invoke("load_elf", { path });
    set({ elf: { path, buildId: info.build_id, signalCount: info.signal_count } });
    prefs.set("cockpit.session.elf", path);
    return info;
  },
  async listSignals(filter, limit = 400) {
    const signals = await invoke("list_signals", { filter, limit });
    set({ signals });
    return signals;
  },
  async installWatches(watches) {
    const status = await invoke("install_watches", { watches });
    set({ traceStatus: status, budgetVerdict: "accepted" });
    return status;
  },
  async traceStatus() {
    const status = await invoke("trace_status");
    set({ traceStatus: status });
    return status;
  },
};

/** Wire the backend event stream into the store. Call once at boot. */
export async function attachEvents() {
  let lastTelemetry = 0;
  await listen("connection", (ev) => {
    if (ev.state === "connected") {
      set({ connection: { state: "connected", port: ev.port, buildId: ev.build_id }, lastPort: ev.port });
    } else if (ev.state === "lost") {
      set({ connection: { state: "lost", port: null, buildId: null } });
    } else {
      set({ connection: { state: "disconnected", port: null, buildId: null } });
    }
  });
  await listen("telemetry", (t) => {
    const now = performance.now();
    if (lastTelemetry) {
      const hz = 1000 / (now - lastTelemetry);
      store.linkHz = store.linkHz ? store.linkHz * 0.8 + hz * 0.2 : hz;
    }
    lastTelemetry = now;
    set({ telemetry: t });
  });
  await listen("log", (l) => notify("log", l.text));
  await listen("trace-status", (s) => set({ traceStatus: s }));
  await listen("samples", (batch) => {
    if (batch.dropped_ticks) set({ gapCount: store.gapCount + batch.dropped_ticks });
    notify("samples", batch);
  });
}

