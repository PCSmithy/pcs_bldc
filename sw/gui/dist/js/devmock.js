// ── DEV MOCK — browser-only stand-in for the Tauri backend ──────────────────
// Loaded ONLY when window.__TAURI__ is absent (plain-browser visual QA).
// Serves the design handoff's sample data and honors ?state=pre|matched|
// mismatch|lost|coldboot|coldboot-noport|badelf|rejected to force app
// states for screenshots and the suite. Not production.

const params = new URLSearchParams(location.search);
const FORCED = params.get("state") || "matched";

const DEVICE_BUILD = "6bded7b9907a+a1b3ce2d";
const ELF_BUILD = FORCED === "mismatch" ? "1d77b2e4a09c" : DEVICE_BUILD;

// A plausible firmware namespace, ~704 leaves.
const MODULES = [
  ["app_motorControl_data.channels[0]", ["velocityMeasured_radPerSec:f32", "velocitySetpointCurrent_radPerSec:f32", "mechanicalAngle_rad:f32", "magneticAngle_rad:f32", "busCurrent:f32", "currentQ_a:f32", "faultLatched:bool", "encoderFaultCount:u16", "duty[0]:f32", "duty[1]:f32", "duty[2]:f32", "phaseCurrent_a[0]:f32", "phaseCurrent_a[1]:f32", "phaseCurrent_a[2]:f32"]],
  // Read-only fixtures (const config in rodata) for the hide-const filter.
  ["IO_bridge_channelConfig", ["phaseCurrent_gain[0]:f32:ro", "phaseCurrent_gain[1]:f32:ro", "phaseCurrent_gain[2]:f32:ro", "deadtime_ns:u16:ro", "pwmFreq_hz:u32:ro"]],
  ["IO_AS5048_data.channels[0]", ["raw:u16", "angle_deg:f32", "angle_rad:f32", "status:enum"]],
  ["IO_AS5048_data.channels[1]", ["raw:u16", "angle_deg:f32", "angle_rad:f32", "status:enum"]],
  ["HW_ADC_data.channels[0]", ["counts[0]:u32", "counts[1]:u32", "counts[2]:u32", "volts[0]:f32", "volts[1]:f32"]],
  ["app_server_data", ["telemetryDivider:u32", "wasConnected:bool"]],
  ["app_userControls_data", ["dialRaw:u16", "buttonState:enum", "modeRequested:enum"]],
];
const signals = [];
for (const [group, leaves] of MODULES) {
  for (const leaf of leaves) {
    const [name, kind, ro] = leaf.split(":");
    signals.push({
      path: `${group}.${name}`,
      kind,
      size: kind === "bool" ? 1 : kind === "u16" ? 2 : 4,
      readonly: ro === "ro",
    });
  }
}
// Enumerator lists ([value, name] pairs, the backend's serialization).
// buttonState deliberately omits value 3 — the enum waveform reaches it, so
// the no-enumerator raw-number fallback stays exercisable (app~views_013).
const AS5048_STATUS = [
  [0, "AS5048_OK"],
  [1, "AS5048_WEAK_FIELD"],
  [2, "AS5048_HIGH_FIELD"],
  [3, "AS5048_CRC_ERROR"],
];
const ENUMS = {
  "IO_AS5048_data.channels[0].status": AS5048_STATUS,
  "IO_AS5048_data.channels[1].status": AS5048_STATUS,
  "app_userControls_data.buttonState": [
    [0, "SWITCH_RELEASED"],
    [1, "SWITCH_PRESSED"],
    [2, "SWITCH_HELD"],
  ],
  "app_userControls_data.modeRequested": [
    [0, "APP_MOTORCONTROL_MODE_IDLE"],
    [1, "APP_MOTORCONTROL_MODE_SIX_STEP_TRAP"],
    [2, "APP_MOTORCONTROL_MODE_VELOCITY"],
    [3, "APP_MOTORCONTROL_MODE_CALIBRATE"],
  ],
};
for (const s of signals) {
  if (ENUMS[s.path]) s.enums = ENUMS[s.path];
}
signals.push({ path: "task1msRuns", kind: "u32", size: 4, readonly: false });
signals.push({ path: "serverRuns", kind: "u32", size: 4, readonly: false });
let n = signals.length;
for (let i = 0; n + i < 704; i++) {
  signals.push({ path: `est_flux_data.buf[${i}]`, kind: "f32", size: 4 });
}

const listeners = new Map();
function emit(event, payload) {
  for (const fn of listeners.get(event) || []) fn(payload);
}

let connected = false;
// Cable-pull simulation state (test surface, see __devmockConn below).
let portPresent = true;
let failConnects = 0;

export const mock = {
  async invoke(cmd, args) {
    switch (cmd) {
      case "list_ports":
        if (FORCED === "lost") return [];
        if (FORCED === "coldboot-noport") return [{ name: "COM3", kind: "other" }];
        return [
          ...(portPresent ? [{ name: "COM8", kind: "USB Serial Device [cafe:4001]" }] : []),
          { name: "COM3", kind: "other" },
        ];
      case "connect":
        // Test surface: every connect attempt, for doubled-connect assertions.
        window.__devmockConnects = (window.__devmockConnects || 0) + 1;
        if (args.port === "COM8" && !portPresent) throw `open ${args.port}: not found`;
        if (failConnects > 0) {
          // A replugged port rejects opens while the OS finishes device
          // setup — the real Windows behavior the reconnect poll must ride.
          failConnects--;
          throw `open ${args.port}: access denied`;
        }
        connected = true;
        emit("connection", { state: "connected", port: args.port, build_id: DEVICE_BUILD });
        return DEVICE_BUILD;
      case "disconnect":
        connected = false;
        emit("connection", { state: "disconnected", port: null, build_id: null });
        return null;
      case "get_status":
        // The coldboot family: a fresh app process, no live core session —
        // the session-restore orchestration is what reconnects.
        if (["pre", "coldboot", "coldboot-noport", "badelf"].includes(FORCED)) {
          return { connected: false, port: null, device_build_id: null };
        }
        connected = FORCED !== "lost";
        return { connected, port: "COM8", device_build_id: DEVICE_BUILD };
      case "load_elf":
        if (FORCED === "badelf") throw `read ${args.path}: os error 2 (not found)`;
        return { build_id: ELF_BUILD, signal_count: signals.length };
      case "list_signals": {
        const needle = (args.filter || "").toLowerCase();
        return signals
          .filter((s) => !needle || s.path.toLowerCase().includes(needle))
          .slice(0, args.limit || 400);
      }
      case "install_watches": {
        // Test surface: every install call, in order, for one-recommit
        // assertions.
        (window.__devmockInstalls ??= []).push(args.watches || []);
        if (FORCED === "rejected") throw "exceeds link budget";
        const requested = (args.watches || []).map((w) => {
          const sig = signals.find((s) => s.path === w.path);
          return {
            path: w.path,
            period_cycles: w.period_cycles,
            size: sig?.size ?? 4,
            kind: sig?.kind ?? "f32",
          };
        });
        // The board's one-cycle cap (fw~conn_trace_002) — the app only
        // presents the cause it gets back.
        if (requested.filter((w) => w.period_cycles === 1).length > 4) {
          throw "more than 4 one-cycle watches";
        }
        watchList = requested;
        // An accepted list restarts the stream from cycle 0 — and the mock
        // backfills ~66 s at once so 60 s spans and pause/zoom are
        // exercisable immediately.
        streamCycle = 0;
        setTimeout(() => {
          emitBatchRange(0, BACKFILL_CYCLES);
          streamCycle = BACKFILL_CYCLES;
        }, 0);
        // The Tauri side pushes a trace-status event on every list change.
        emit("trace-status", traceStatusInfo());
        return traceStatusInfo();
      }
      case "clear_watches":
        watchList = [];
        emit("trace-status", traceStatusInfo());
        return traceStatusInfo();
      case "trace_status":
        return traceStatusInfo();
      default:
        throw new Error(`devmock: unknown command ${cmd}`);
    }
  },

  async listen(event, handler) {
    if (!listeners.has(event)) listeners.set(event, []);
    listeners.get(event).push(handler);
    return () => {};
  },

  async pickFile() {
    return "C:/code/pcs_bldc-serial-protocol/build/arm-fw/src/pcs_bldc_fw.elf";
  },
};

// ── trace stream fixtures: deterministic waveforms per watched signal, a
// dropout window every ~5 s, and budgets computed with the real admission
// formulas (fw~conn_trace_002) so the meters and suggested-fix math can be
// QA'd honestly. Everything here runs in the wire's PWM-cycle domain; the
// emitted tick is the cycle index in ms, one cycle = 0.05 ms.

let watchList = [];
let streamCycle = 0;

const CYCLES_PER_MS = 20;
const CYCLES_PER_S = 20_000;
const WIRE_OVERHEAD_W = 27;
const SAMPLES_DATA_CAPACITY = 256;

/** The watch list by period: [{ period, size, entries }]. */
function groups() {
  const byPeriod = new Map();
  for (const w of watchList) {
    if (!byPeriod.has(w.period_cycles)) {
      byPeriod.set(w.period_cycles, { period: w.period_cycles, size: 0, entries: [] });
    }
    const g = byPeriod.get(w.period_cycles);
    g.size += w.size;
    g.entries.push(w);
  }
  return [...byPeriod.values()];
}

function traceStatusInfo() {
  let u = 0, r = 0;
  for (const g of groups()) {
    const f = CYCLES_PER_S / g.period;
    u += Math.max(1, CYCLES_PER_MS / g.period) * (4 + g.size);
    r += g.size * f + WIRE_OVERHEAD_W * Math.min(f, Math.max(1000, (f * g.size) / SAMPLES_DATA_CAPACITY));
  }
  return {
    ram_budget_bytes: 2048,
    ram_usage_bytes_per_ms: Math.round(u),
    link_budget_bytes_per_s: 480000,
    link_rate_bytes_per_s: Math.round(r),
  };
}

// Per-path phase, memoized: a 20 kHz backfill calls waveform 200 k+ times
// a signal, and re-walking the path string each time dominates it.
const PHASES = new Map();
function phaseOf(path) {
  let p = PHASES.get(path);
  if (p === undefined) {
    p = [...path].reduce((a, c) => a + c.charCodeAt(0), 0) % 97;
    PHASES.set(path, p);
  }
  return p;
}

function waveform(path, kind, t) {
  const phase = phaseOf(path);
  if (kind === "bool") return Math.sin(t / 700 + phase) > 0 ? 1 : 0;
  if (kind === "enum") return Math.floor(t / 2000 + phase) % 4;
  if (kind === "u16" || kind === "u32") return (Math.round(t) + phase * 1000) % 65536;
  // A ~5 ms ripple rides the slow swing: aliased away at 1 ms, resolved at
  // 20 kHz — a one-cycle watch has to look different from a slow one.
  return Math.sin(t / (300 + phase * 5) + phase) * (2 + (phase % 5)) + Math.sin(t / 0.8) * 0.4;
}

const BATCH_CYCLES = 50 * CYCLES_PER_MS;     // 50 ms of wire time a batch
const GAP_EVERY_CYCLES = 5 * CYCLES_PER_S;
const GAP_LEN_CYCLES = 120 * CYCLES_PER_MS;
const BACKFILL_CYCLES = 66 * CYCLES_PER_S;
// A one-cycle signal retains 10 s (app~views_008): backfill just past that
// so its window is full the moment a list installs, and no further.
const FAST_BACKFILL_CYCLES = 11 * CYCLES_PER_S;

/** Emit one "samples" event covering cycle indices [c0, c1). */
function emitBatchRange(c0, c1) {
  const inGap = (c) => c % GAP_EVERY_CYCLES >= GAP_EVERY_CYCLES - GAP_LEN_CYCLES;
  let dropped = 0;
  const sigs = new Map(watchList.map((w) => [w.path, { path: w.path, points: [] }]));
  for (const g of groups()) {
    const from = g.period === 1 ? Math.max(c0, c1 - FAST_BACKFILL_CYCLES) : c0;
    // Every group is phase 0 here: the board's per-period offsets are a
    // sampler detail no app requirement rests on, and they would put every
    // slow tick a cycle off a whole millisecond.
    for (let c = Math.ceil(from / g.period) * g.period; c < c1; c += g.period) {
      if (inGap(c)) { dropped++; continue; }
      const t = c / CYCLES_PER_MS;
      for (const w of g.entries) sigs.get(w.path).points.push([t, waveform(w.path, w.kind, t)]);
    }
  }
  // Test surface (like __devmockInstalls): lets the suite scale timing
  // floors to the batches a loaded host ACTUALLY delivered.
  window.__devmockBatches = (window.__devmockBatches || 0) + 1;
  emit("samples", {
    signals: [...sigs.values()].filter((s) => s.points.length),
    dropped_records: dropped,
  });
}

setInterval(() => {
  if (!connected || !watchList.length) return;
  const c0 = streamCycle;
  streamCycle += BATCH_CYCLES;
  emitBatchRange(c0, streamCycle);
}, BATCH_CYCLES / CYCLES_PER_MS);

// Live-ish fixtures: 10 Hz telemetry, occasional log lines, a lost event.
let t = 184000;
setInterval(() => {
  if (!connected) return;
  t += 100;
  const meas = 145.3 + Math.sin(t / 900) * 1.8;
  emit("telemetry", {
    timestamp_ms: t,
    mode: "MODE_SIX_STEP_TRAP",
    state: "DRIVE_STATE_ENABLED",
    bus_voltage_v: 19.84 + Math.sin(t / 1300) * 0.02,
    bus_current_a: 1.271 + Math.sin(t / 700) * 0.05,
    velocity_measured_radps: meas,
    velocity_setpoint_radps: 146.6,
  });
}, 100);
const LOG_LINES = [
  "heartbeat 184s up, server 184000 runs",
  "watch list installed: 14 entries, r=968000 B/s",
  "heartbeat 185s up, server 185000 runs",
];
let li = 0;
setInterval(() => {
  if (connected) emit("log", { text: LOG_LINES[li++ % LOG_LINES.length] + "\n" });
}, 1400);
setTimeout(() => {
  if (FORCED === "lost") {
    connected = false;
    emit("connection", { state: "lost", port: null, build_id: null });
  }
}, 400);

// Test surface: a scripted cable pull/replug cycle, mirroring the Tauri
// side's behavior — the reader thread emits "lost", the port drops out of
// enumeration, and the board (DTR gone) clears its own watch list.
window.__devmockConn = {
  pull() {
    connected = false;
    portPresent = false;
    watchList = []; // the board's list dies with the CDC line state
    emit("connection", { state: "lost", port: null, build_id: null });
  },
  replug({ failConnects: n = 0 } = {}) {
    portPresent = true;
    failConnects = n;
  },
};
