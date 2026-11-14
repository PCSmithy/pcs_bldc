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
  ["app_motorControl_data.channels[0]", ["velocityMeasured_radPerSec:f32", "velocitySetpoint_radPerSec:f32", "rotorAngle_rad:f32", "mechanicalAngle_rad:f32", "magneticAngle_rad:f32", "busCurrent:f32", "iD_a:f32", "iQ_a:f32", "faultLatched:bool", "encoderFaultCount:u16", "duty[0]:f32", "duty[1]:f32", "duty[2]:f32", "phaseCurrent_a[0]:f32", "phaseCurrent_a[1]:f32", "phaseCurrent_a[2]:f32"]],
  ["app_rotorEstimation_data.channels[0]", ["x_hat.e[0][0]:f32", "x_hat.e[1][0]:f32", "angleMeasured_rad:f32", "angleError_rad:f32", "isSeeded:bool"]],
  // Read-only fixtures (const config in rodata) for the hide-const filter.
  ["IO_bridge_data.channels[0]", ["measuredPhaseCurrent_amps[0]:f32", "measuredPhaseCurrent_amps[1]:f32", "measuredPhaseCurrent_amps[2]:f32", "phaseCurrentZeroOffset_amps[0]:f32", "phaseCurrentZeroOffset_amps[1]:f32", "phaseCurrentZeroOffset_amps[2]:f32", "idleSamples[0]:u32", "idleSamples[1]:u32", "idleSamples[2]:u32"]],
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
// CPU load counters: cumulative µs at a fixed share of mock time. The task
// and idle rates partition elapsed time; the ISR's runs on top of them.
// The idle counter starts near 2^32 so it wraps at 45 s of stream time;
// from 56.2 s to 57.2 s the server takes 30 % of elapsed from idle.
const CPU_COUNTERS = new Map([
  ["app_cpuLoad_data.taskTime_us[0]", { rate: 0.05 }],
  ["app_cpuLoad_data.taskTime_us[1]", { rate: 0.40, offset: 2 ** 32 - 0.40 * 45e6, burst: -0.30 }],
  ["app_cpuLoad_data.taskTime_us[2]", { rate: 0.15 }],
  ["app_cpuLoad_data.taskTime_us[3]", { rate: 0.08 }],
  ["app_cpuLoad_data.taskTime_us[4]", { rate: 0.02 }],
  ["app_cpuLoad_data.taskTime_us[5]", { rate: 0.10 }],
  ["app_cpuLoad_data.taskTime_us[6]", { rate: 0.20, burst: 0.30 }],
  ["HW_ADC_data.irqTime_us", { rate: 0.45 }],
]);
const CPU_BURST_MS = [56_200, 57_200];
for (const path of CPU_COUNTERS.keys()) signals.push({ path, kind: "u32", size: 4, readonly: false });
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
        window.__devmockElfLoads = (window.__devmockElfLoads || 0) + 1;
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
        // Budget admission (fw~conn_trace_002), with the board's causes — the
        // app only presents the cause it gets back.
        const want = usage(requested);
        if (want.u > RAM_BUDGET) throw "exceeds sample-RAM budget";
        if (want.r > LINK_BUDGET) throw "exceeds link budget";
        watchList = requested;
        // An accepted list restarts the stream from cycle 0 — and the mock
        // backfills ~66 s at once — at every period alike, retention
        // being uniform — so 60 s spans and pause/zoom are exercisable
        // immediately.
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
      case "send_command":
        return sendCommand(args.command);
      case "get_motor_info":
        return { ...MOTOR_INFO };
      case "save_capture_csv": {
        // Test surface: __devmockCancelSave stands in for a cancelled dialog.
        if (window.__devmockCancelSave) return null;
        const path = "C:/captures/capture.csv";
        window.__devmockWritten = { path, text: args.text };
        return path;
      }
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
const RAM_BUDGET = 2048;
const LINK_BUDGET = 480_000;

// The board staggers its period groups so two captures never land on the
// same cycle (app_server_trace.c): a group's cycle indices are ≡ its offset
// (mod its period).
const GROUP_OFFSET = { 1: 0, 20: 1, 200: 2 };

/** First cycle >= `from` on a group's stagger grid. */
function firstCycle(from, period) {
  const off = GROUP_OFFSET[period] ?? 0;
  return from + ((((off - from) % period) + period) % period);
}

/** A watch list by period: [{ period, size, entries }]. */
function groups(list = watchList) {
  const byPeriod = new Map();
  for (const w of list) {
    if (!byPeriod.has(w.period_cycles)) {
      byPeriod.set(w.period_cycles, { period: w.period_cycles, size: 0, entries: [] });
    }
    const g = byPeriod.get(w.period_cycles);
    g.size += w.size;
    g.entries.push(w);
  }
  return [...byPeriod.values()];
}

/** The fw~conn_trace_002 formulas over a watch list → { u, r }. */
function usage(list) {
  let u = 0, r = 0;
  for (const g of groups(list)) {
    const f = CYCLES_PER_S / g.period;
    u += Math.max(1, CYCLES_PER_MS / g.period) * (4 + g.size);
    r += g.size * f + WIRE_OVERHEAD_W * Math.min(f, Math.max(1000, Math.floor((f * g.size) / SAMPLES_DATA_CAPACITY)));
  }
  return { u, r };
}

function traceStatusInfo() {
  const { u, r } = usage(watchList);
  return {
    ram_budget_bytes_per_ms: RAM_BUDGET,
    ram_usage_bytes_per_ms: Math.round(u),
    link_budget_bytes_per_s: LINK_BUDGET,
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
  const cpu = CPU_COUNTERS.get(path);
  if (cpu) {
    const inBurst = Math.min(Math.max(t - CPU_BURST_MS[0], 0), CPU_BURST_MS[1] - CPU_BURST_MS[0]);
    return Math.floor((cpu.offset ?? 0) + (cpu.rate * t + (cpu.burst ?? 0) * inBurst) * 1000) % 2 ** 32;
  }
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

/** Emit one "samples" event covering cycle indices [c0, c1). */
function emitBatchRange(c0, c1) {
  const inGap = (c) => c % GAP_EVERY_CYCLES >= GAP_EVERY_CYCLES - GAP_LEN_CYCLES;
  let dropped = 0;
  const sigs = new Map(watchList.map((w) => [w.path, { path: w.path, points: [] }]));
  for (const g of groups()) {
    for (let c = firstCycle(c0, g.period); c < c1; c += g.period) {
      // The board counts a drop per ENTRY per missed cycle (trace.rs), not
      // one per cycle.
      if (inGap(c)) { dropped += g.entries.length; continue; }
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

// Diagnostic surface for the UI suite: is the stream timer alive?
const streamDiag = { ticks: 0, emits: 0, lastTickWall: 0 };
window.__devmockDiag = () => ({
  connected, watch: watchList.length, streamCycle, ...streamDiag,
  sinceTickMs: streamDiag.lastTickWall ? Date.now() - streamDiag.lastTickWall : null,
  visibility: document.visibilityState,
});
setInterval(() => {
  streamDiag.ticks++;
  streamDiag.lastTickWall = Date.now();
  if (!connected || !watchList.length) return;
  const c0 = streamCycle;
  streamCycle += BATCH_CYCLES;
  emitBatchRange(c0, streamCycle);
  streamDiag.emits++;
}, BATCH_CYCLES / CYCLES_PER_MS);

// ── the board command plane: the ratings, and a Command's verdict — the
// core's build step (a malformed spec or a frame over the board's 448 B
// throws, unsent) then the firmware's rejection table (fw~conn_server_007:
// capacity, the setpoint rows, then the setpoint kind against the mode; the
// busy, fault and bridge rows are not modelled). An accepted Command reports
// running from its commit until its first stop, the setpoints after it
// dropped; one with no setpoints reports not running.
const MOTOR_INFO = { kt_nm_per_a: 0.825, i_max_a: 1.0, max_torque_nm: 0.825, max_velocity_radps: 18.0, max_position_rad: 62.83 };
const MODES = ["MODE_OFF", "MODE_SIX_STEP_TRAP", "MODE_SINUSOIDAL", "MODE_FOC_TORQUE", "MODE_FOC_VELOCITY", "MODE_FOC_POSITION"];
// The modes each value setpoint kind runs under: speed under any driving method.
const KIND_MODES = { velocity_radps: MODES.slice(1), torque_nm: ["MODE_FOC_TORQUE"], position_rad: ["MODE_FOC_POSITION"] };
const SEQUENCE_CAPACITY = 24;
const MAX_FRAME_PAYLOAD = 448;
let sequence = null;   // { startMs, durationMs, mode, setpoints } while a Command plays
window.__devmockLastCommand = null;
// Test surfaces: every Command in send order, a reply latency, and a one-shot
// rejection cause for the next Command.
window.__devmockCommands = [];
window.__devmockCommandDelayMs = 0;
window.__devmockRejectNext = null;

/** The Command's envelope length as prost encodes it, at the widest request id. */
function frameLen(command) {
  const varint = (v) => (v < 2 ** 7 ? 1 : v < 2 ** 14 ? 2 : v < 2 ** 21 ? 3 : v < 2 ** 28 ? 4 : 5);
  const field = (len) => 1 + varint(len) + len;
  const f32 = (v) => (v ? 5 : 0); // proto3 omits a zero
  let body = command.mode === "MODE_OFF" ? 0 : 2;
  for (const sp of command.setpoints || []) {
    const target = sp.stop != null ? 2 : 5; // a oneof arm is always written
    body += field((sp.t_us ? 1 + varint(sp.t_us) : 0) + target
      + f32(sp.rate_limit) + f32(sp.accel_limit_radps2) + f32(sp.velocity_at_point_radps));
  }
  const request = field(body);
  return 6 + 2 + varint(request) + request; // request_id, then the two-byte board_request key
}

async function sendCommand(command) {
  if (!connected) throw "not connected";
  const verdict = runCommand(command);
  if (window.__devmockCommandDelayMs) await new Promise((r) => setTimeout(r, window.__devmockCommandDelayMs));
  return verdict;
}

function runCommand(command) {
  window.__devmockLastCommand = command;
  window.__devmockCommands.push(command);
  const reject = (cause) => ({ accepted: false, cause });
  if (!MODES.includes(command.mode)) throw `unknown mode ${command.mode}`;
  const setpoints = command.setpoints || [];
  for (const [i, sp] of setpoints.entries()) {
    const targets = ["torque_nm", "velocity_radps", "stop", "position_rad"].filter((k) => sp[k] != null);
    if (targets.length !== 1) throw `setpoint ${i}: exactly one target`;
    if (!Number.isInteger(sp.t_us) || sp.t_us < 0) throw `setpoint ${i}: missing field t_us`;
  }
  const len = frameLen(command);
  if (len > MAX_FRAME_PAYLOAD) throw `frame too large: ${len} B, the board takes ${MAX_FRAME_PAYLOAD} B`;
  if (window.__devmockRejectNext) {
    const cause = window.__devmockRejectNext;
    window.__devmockRejectNext = null;
    return reject(cause);
  }
  if (setpoints.length > SEQUENCE_CAPACITY) return reject("sequence exceeds the player's capacity");
  let lastT = 0;
  for (const sp of setpoints) {
    const kind = Object.keys(KIND_MODES).find((k) => sp[k] != null);
    const hasPosition = kind === "position_rad";
    // A stop stages as a zero within a zero bound, as the firmware does.
    const value = kind ? sp[kind] : 0;
    const bound = { torque_nm: MOTOR_INFO.max_torque_nm, velocity_radps: MOTOR_INFO.max_velocity_radps,
      position_rad: MOTOR_INFO.max_position_rad }[kind] ?? 0;
    const limit = sp.rate_limit ?? 0;
    const accel = sp.accel_limit_radps2 ?? 0;
    const atPoint = sp.velocity_at_point_radps ?? 0;
    if (![value, limit, accel, atPoint].every(Number.isFinite)) return reject("setpoint not finite");
    if (limit < 0 || accel < 0) return reject("rate limit negative");
    if (Math.abs(value) > bound) return reject("setpoint exceeds the motor rating");
    // A position's rate limit is its velocity limit; it bounds the velocity at the point.
    if (hasPosition && limit > MOTOR_INFO.max_velocity_radps) return reject("velocity limit exceeds the motor rating");
    if (hasPosition && Math.abs(atPoint) > (limit > 0 ? limit : MOTOR_INFO.max_velocity_radps)) {
      return reject("velocity at the point exceeds the velocity limit");
    }
    if (sp.t_us < lastT) return reject("setpoint times not in order");
    lastT = sp.t_us;
  }
  // The kind against the mode, after every per-setpoint cause.
  const misfit = setpoints.some((sp) => {
    const kind = Object.keys(KIND_MODES).find((k) => sp[k] != null);
    return kind && !KIND_MODES[kind].includes(command.mode);
  });
  if (misfit) return reject("setpoint kind not for this mode");
  const stopAt = setpoints.findIndex((sp) => sp.stop != null);
  const played = stopAt < 0 ? setpoints : setpoints.slice(0, stopAt + 1);
  const durationMs = stopAt >= 0 ? played[stopAt].t_us / 1000 : setpoints.length ? Math.ceil(lastT / 1000) + 1 : 0;
  sequence = { startMs: t, durationMs, mode: command.mode, setpoints: played };
  return { accepted: true, cause: "" };
}

/** The telemetry's view of the playing sequence at mock time `now`: the last
 *  due setpoint of each kind stands as the commanded target; a due stop has
 *  the drive off with zero targets, as the firmware's dark bridge does. */
function sequenceState(now) {
  if (!sequence) return { running: false, elapsed_ms: 0, torque_nm: 0, velocity_radps: 146.6, position_rad: 0, stopped: false };
  const elapsed = now - sequence.startMs;
  const running = elapsed < sequence.durationMs;
  const due = sequence.setpoints.filter((sp) => sp.t_us / 1000 <= elapsed);
  const stopped = due.some((sp) => sp.stop != null);
  const lastOf = (key) => (stopped ? 0 : due.filter((sp) => sp[key] != null).at(-1)?.[key] ?? 0);
  return {
    running,
    elapsed_ms: running ? elapsed : sequence.durationMs,
    torque_nm: lastOf("torque_nm"),
    velocity_radps: lastOf("velocity_radps"),
    position_rad: lastOf("position_rad"),
    stopped,
  };
}

// Live-ish fixtures: 10 Hz telemetry, occasional log lines, a lost event.
let t = 184000;
setInterval(() => {
  if (!connected) return;
  t += 100;
  const meas = 145.3 + Math.sin(t / 900) * 1.8;
  const seq = sequenceState(t);
  emit("telemetry", {
    timestamp_ms: t,
    mode: !sequence ? "MODE_SIX_STEP_TRAP" : seq.stopped ? "MODE_OFF" : sequence.mode,
    state: sequence && (sequence.mode === "MODE_OFF" || seq.stopped) ? "DRIVE_STATE_DISABLED" : "DRIVE_STATE_ENABLED",
    bus_voltage_v: 19.84 + Math.sin(t / 1300) * 0.02,
    bus_current_a: 1.271 + Math.sin(t / 700) * 0.05,
    velocity_measured_radps: meas,
    velocity_setpoint_radps: seq.velocity_radps,
    torque_target_nm: seq.torque_nm,
    position_target_rad: seq.position_rad,
    rotor_position_rad: seq.position_rad + Math.sin(t / 500) * 0.002,
    sequence_running: seq.running,
    sequence_elapsed_ms: seq.elapsed_ms,
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
