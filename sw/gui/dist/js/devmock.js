// ── DEV MOCK — browser-only stand-in for the Tauri backend ──────────────────
// Loaded ONLY when window.__TAURI__ is absent (plain-browser visual QA).
// Serves the design handoff's sample data and honors ?state=pre|matched|
// mismatch|lost|empty to force app states for screenshots. Not production.

const params = new URLSearchParams(location.search);
const FORCED = params.get("state") || "matched";

const DEVICE_BUILD = "6bded7b9907a+a1b3ce2d";
const ELF_BUILD = FORCED === "mismatch" ? "1d77b2e4a09c" : DEVICE_BUILD;

// A plausible firmware namespace, ~704 leaves.
const MODULES = [
  ["app_motorControl_data.channels[0]", ["velocityMeasured_radPerSec:f32", "velocitySetpointCurrent_radPerSec:f32", "mechanicalAngle_rad:f32", "magneticAngle_rad:f32", "busCurrent:f32", "faultLatched:bool", "encoderFaultCount:u16", "duty[0]:f32", "duty[1]:f32", "duty[2]:f32", "phaseCurrent_a[0]:f32", "phaseCurrent_a[1]:f32", "phaseCurrent_a[2]:f32"]],
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

export const mock = {
  async invoke(cmd, args) {
    switch (cmd) {
      case "list_ports":
        if (FORCED === "lost") return [];
        if (FORCED === "coldboot-noport") return [{ name: "COM3", kind: "other" }];
        return [
          { name: "COM8", kind: "USB Serial Device [cafe:4001]" },
          { name: "COM3", kind: "other" },
        ];
      case "connect":
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
      case "install_watches":
        // Test surface: every install call, in order, for one-recommit
        // assertions.
        (window.__devmockInstalls ??= []).push(args.watches || []);
        if (FORCED === "rejected") throw "exceeds link budget";
        watchList = (args.watches || []).map((w) => ({
          path: w.path,
          period_ms: w.period_ms,
          size: signals.find((s) => s.path === w.path)?.size ?? 4,
        }));
        // An accepted list restarts the stream from tick 0 — and the mock
        // backfills ~66 s at once so 60 s spans and pause/zoom are
        // exercisable immediately.
        streamTick = 0;
        setTimeout(() => {
          emitBatchRange(0, BACKFILL_MS);
          streamTick = BACKFILL_MS;
        }, 0);
        return traceStatusInfo();
      case "clear_watches":
        watchList = [];
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
// gap window every ~5 s, budgets computed with the real admission formulas
// (u = 4 + Σsize, r = Σ(size·1000/p) + 21·max(1000/p)) so the meters and
// suggested-fix math can be QA'd honestly.

let watchList = [];
let streamTick = 0;

function traceStatusInfo() {
  let sum = 0, rate = 0, maxF = 0;
  for (const w of watchList) {
    const f = 1000 / w.period_ms;
    sum += w.size;
    rate += w.size * f;
    if (f > maxF) maxF = f;
  }
  return {
    ram_budget_bytes: 2048,
    ram_worst_tick_bytes: watchList.length ? 4 + sum : 0,
    link_budget_bytes_per_s: 1100000,
    link_rate_bytes_per_s: watchList.length ? rate + 21 * maxF : 0,
  };
}

function waveform(path, kind, tick) {
  const phase = [...path].reduce((a, c) => a + c.charCodeAt(0), 0) % 97;
  if (kind === "bool") return Math.sin(tick / 700 + phase) > 0 ? 1 : 0;
  if (kind === "enum") return Math.floor(tick / 2000 + phase) % 4;
  if (kind === "u16" || kind === "u32") return (tick + phase * 1000) % 65536;
  return Math.sin(tick / (300 + phase * 5) + phase) * (2 + (phase % 5));
}

const BATCH_MS = 50;
const GAP_EVERY_MS = 5000;
const GAP_LEN_MS = 120;
const BACKFILL_MS = 66_000;

function emitBatchRange(t0, t1) {
  const inGap = (t) => t % GAP_EVERY_MS >= GAP_EVERY_MS - GAP_LEN_MS;
  let dropped = 0;
  const sigs = watchList.map((w) => ({ path: w.path, points: [] }));
  for (let t = t0; t < t1; t++) {
    if (inGap(t)) { dropped++; continue; }
    for (let i = 0; i < watchList.length; i++) {
      const w = watchList[i];
      if (t % w.period_ms === 0) {
        const kind = signals.find((s) => s.path === w.path)?.kind ?? "f32";
        sigs[i].points.push([t, waveform(w.path, kind, t)]);
      }
    }
  }
  emit("samples", { signals: sigs.filter((s) => s.points.length), dropped_ticks: dropped });
}

setInterval(() => {
  if (!connected || !watchList.length) return;
  const t0 = streamTick;
  streamTick += BATCH_MS;
  emitBatchRange(t0, streamTick);
}, BATCH_MS);

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
