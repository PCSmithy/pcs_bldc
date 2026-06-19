# SIL architecture

Status: **draft / planning.** This captures the agreed ground rules and the
decisions still open. It is a living doc — update it as decisions land.

## 1. Goal

A Rust framework that runs the **native cross-compiled pcs_bldc firmware**
against simulated plant models (motor, encoder, sensors, power), with
white-box access to firmware state, in two modes:

- **Realtime** — sim time == wall-clock; web dashboard; presents a USB-CDC
  endpoint the desktop app talks to as if it were the real board.
- **Fast** — faster-than-realtime, deterministic, scripted from Python
  (pytest), parallelizable for regression.

## 2. The big picture

```
            ┌─────────────────────────── Rust SIL framework ──────────────────────────┐
            │                                                                          │
  pytest ──┐  ┌───────────┐   ┌────────────────────────┐   ┌────────────────────────┐
  (fast)   ├──┤ scripting │──▶│        sim core         │◀─▶│  plant models (Rust)   │
           │  │ / py bind │   │  sim clock · run modes  │   │  motor · encoder ·     │
  web UI ──┘  └───────────┘   │  ┌───────────────────┐  │   │  sensors · power       │
 (realtime)  ┌───────────┐    │  │    STATE TABLE    │  │   └───────────┬────────────┘
             │ dashboard │──▶ │  │ every fw static + │  │   model state │ register
             │ (ws+plot) │    │  │   model states    │  │◀──────────────┘
             └───────────┘    │  └───────────────────┘  │
                              │  ┌───────────────────┐  │
                              │  │    ROUTE TABLE    │  │  src→dst transport each tick
                              │  └───────────────────┘  │
                              └────────────┬────────────┘
        control ABI (start / advance_tick) │  direct memory R/W via DWARF + ASLR slide
                                           ▼  (NO sim getters/setters in firmware)
           ┌─────────────── native firmware (BUILD_TARGET_SIM) ──────────────────┐
           │  app · dev · io   (mode FSM, control, estimator, IO, USB)   in SIM   │
           │  ───────────────── FreeRTOS scheduler (host port) ──────────────────│
           │  hw   HW_ADC/SPI/GPIO/systemClock  — only HAL/register code swapped  │
           └─────────────────────────────────────────────────────────────────────┘
```

## 3. Decisions

### 3.1 Confirmed

- **Rust** framework.
- **Native execution**, not ARM emulation. Rationale: fast, deterministic,
  trivially parallel, and it reuses the `sim/` HAL swaps already in the
  tree. ARM emulation (Unicorn/QEMU) is slower and would require modeling
  peripherals at the register level, fighting the existing HAL-level swap.
  Its one advantage — running the bit-exact shipping binary to catch
  32-bit-ARM-vs-host bugs — is not worth the cost now. Kept as a *future
  backend* behind the execution-backend seam (§3.2).
- **Interception at the HAL/channel boundary.** The bottom, register-specific
  layer (`sw/lib/c/shared/hw/<Module>/sim/`) is what's swapped. But the
  framework does **not** call sim getters/setters — it reads/writes the sim
  drivers' (and any other) firmware state directly in memory via the State
  Table (below).
- **FreeRTOS + io/dev/app run in SIM.** We run the *real* scheduler and the
  *real* task code on the native target. Only the bottom, register-specific
  layer is replaced. This is the high-fidelity-where-it's-cheap choice: all
  the control / FSM / estimator / UI logic and its concurrency structure are
  exercised exactly as they ship.
- **State Table + Route Table are the framework's core data structures**
  (see [`state-route-tables.md`](state-route-tables.md)). The **State Table**
  is one namespace over *every* firmware static (auto-derived from DWARF) plus
  all model states; the **Route Table** declaratively transports
  source→destination each tick. This replaces any sim-specific data API in the
  firmware entirely.
- **White-box via symbols, no firmware instrumentation.** Read/write any
  firmware global by name through the native shared-lib symbol table + DWARF
  (types/addresses), dereferenced in-process. No hand-written accessor per
  signal; the firmware doesn't know it's being simulated.

### 3.2 The execution-backend seam

The framework talks to the firmware through one narrow trait so the
execution mechanism can evolve without disturbing the core engine, models,
or modes:

```rust
trait FwBackend {
    fn init(&mut self);                       // run HW_*_init, create tasks
    fn advance(&mut self, dt: SimDuration);   // let firmware run for dt of sim time
    fn read(&self, sym: &str) -> Value;       // white-box read
    fn write(&mut self, sym: &str, v: Value); // white-box write / injection
}
```

- `NativeFreeRtos` — the first and primary impl (§4).
- `ArmEmu` — a possible later impl (Unicorn/QEMU) for a high-fidelity
  subset, same trait.

### 3.3 Open decisions (to resolve)

| # | Decision | Options | Leaning |
|---|----------|---------|---------|
| D1 | **FreeRTOS port / time source** — how the scheduler tick and context switches relate to sim time | — | **RESOLVED:** pluggable tick source at the port layer (realtime-paced vs framework-driven); retrofit existing host ports, escalate to a custom port only if needed. See [`freertos-tick.md`](freertos-tick.md). |
| D2 | **Rust↔C boundary** | — | **RESOLVED:** in-process, one fw instance/process, firmware as a dynamically-loaded shared lib; tiny control ABI only; all data via direct DWARF-located memory R/W (no sim getters/setters), surfaced as the State Table + Route Table. See [`ffi-boundary.md`](ffi-boundary.md) + [`state-route-tables.md`](state-route-tables.md). |
| D3 | **Python binding** | `pyo3` native extension vs C-ABI + `cffi`/`ctypes` | TBD (pyo3 likely) |
| D4 | **Dashboard stack** | Rust web framework (axum/...) + frontend plotting lib | TBD |
| D5 | **Sim USB-CDC transport** | virtual COM port (Win + macOS) vs TCP socket the app opts into | TBD; ties to the deferred CDC framing decision in `specs/system/overview.md` |
| D6 | **Inverter fidelity / base dt** | — | **RESOLVED (contract):** averaged-duty default, switching-resolved swappable; abc contract (normalized leg duty in, currents+angle out) routed via sim HW-driver state; base `dt` set by model stability (finer than PWM), control ISR fires on a multiple, model advances one `dt`/tick. Two values finalize with the firmware. See [`inverter-timestep.md`](inverter-timestep.md). |
| D7 | **Cross-platform float determinism** | accept host-FP variance vs pin it (compiler flags, soft-float, fixed reductions) | TBD; matters for byte-exact regression baselines |
| D8 | **Simulated interrupt model** | — | **RESOLVED:** framework-owned interrupt table (periodic + one-shot; registered at config by name and at runtime by the sim HW layer via a C→Rust upcall); dispatched through the port in the firmware thread; fixed base-`dt` grid; priority-ordered, no nesting. See [`sim-interrupts.md`](sim-interrupts.md). |
| D9 | **Firmware time virtualization** | — | **RESOLVED:** sim time advances only at yields — every wait is a yield, every time read reflects the sim clock. Reads (`HAL_GetTick`/DWT/timer CNT) are sim-clock-derived; delays become cooperative sim-time advances; portable code uses FreeRTOS delays or a `HW_time` shim. See [`time-virtualization.md`](time-virtualization.md). |
| D10 | **Scenario / config representation** | how a test declares routes + model params + injection (config file? Python API? both) | TBD; the day-to-day test-authoring surface |
| D11 | **State Table binding stability** | symbol-path keys across fw rebuilds — fail-loud at load vs lazy | TBD; small convention |
| D12 | **Signal trace format** | what's recorded, rate, retention, export (shared by pytest asserts + dashboard) | TBD |

## 4. Execution model (the crux)

Today the native `main()` runs the `HW_*_init` calls and returns — FreeRTOS,
the `task_1ms`/`ledTask` loops, and all io/dev code are gated
`BUILD_TARGET_STM32G4`-only. The decided direction is to **run FreeRTOS and
the upper layers in SIM**, which means:

**Firmware-side work (on `main`-mergeable):**

1. **Ungate the upper layers for SIM.** Move io / dev / app and the FreeRTOS
   bring-up out of the `#if (BUILD_TARGET == BUILD_TARGET_STM32G4)` blocks in
   `main.c`, and make their `CMakeLists` build for both targets.
2. **Sim impls of the bottom-layer drivers** the upper layers sit on. HW
   already has `sim/` for ADC/SPI/GPIO/systemClock. The gaps are the things
   currently STM32G4-only: USB (tinyusb) → a sim CDC transport, and anything
   else that pokes hardware directly. IO modules (AS5048, SK6805) ride on
   `HW_SPI`, so they should build for SIM once SPI's sim behavior carries the
   encoder/LED model data.
3. **A FreeRTOS host port with a pluggable tick source** (decision D1,
   resolved — see [`freertos-tick.md`](freertos-tick.md)). The scheduler runs
   on the host; the tick comes from a swappable source — wall-clock-paced
   (realtime) or framework-driven (deterministic). Both modes then share one
   control loop, differing only in pacing. Approach: retrofit the existing
   MSVC-MingW (Windows) and POSIX (macOS) host ports; escalate to a custom
   cooperative port only if the determinism spike demands it.
4. **A sim ABI / native entry point** so the `FwBackend` can `init()` and
   `advance()` the firmware and the framework owns the outer loop (rather
   than `vTaskStartScheduler()` never returning).

**What this buys:** the real task priorities, preemption, blocking, and
timing of the shipping firmware are all under test — not a reimplementation.

## 5. Sim core (Rust)

One **sim clock** drives a discrete time-step engine at a fixed base `dt`
(D6; set by model numerical stability, typically finer than the PWM rate, with
ISRs firing on integer multiples). State flows through the State
Table; the Route Table moves it (see
[`state-route-tables.md`](state-route-tables.md)). Each step:

1. Advance plant models by `dt` (updates their State Table entries).
2. **Propagate routes** in one uniform pass — snapshot all sources, then write
   all destinations (sensors into fw-input statics, fw-output statics back to
   models, together). Race-free: the firmware is quiescent here (D1).
3. `sil_fw_advance_tick()` — firmware runs to quiescence (D1).
4. Record signals (ring buffers) for plotting / assertions; run test
   asserts/injection (ad-hoc State Table reads/writes).

The two run modes are thin wrappers over this loop:

- **Realtime:** pace step 3 to wall-clock (sleep/spin so sim-time tracks real
  time), serve the dashboard over WebSocket, and bridge the firmware's CDC
  endpoint to a host virtual COM port / socket (D5) for the desktop app.
- **Fast:** no pacing, no UI. Drive from the scripting API; expose Python
  bindings (D3) for pytest. Deterministic (D1, D7) → reproducible and
  parallelizable across processes (one firmware instance per process).

## 6. Plant models (Rust)

Behind traits so they're swappable and configurable (live in realtime,
per-test in fast mode):

- **Motor** — electrical (3-phase / dq) + mechanical (inertia, friction,
  load torque). Input: inverter command from firmware (PWM duty / phase
  voltages). Output: phase currents, rotor angle + speed.
- **Encoder (AS5048)** — rotor angle → SPI response bytes.
- **Sensors** — phase-current shunts, bus voltage, temperature → ADC counts.
- **Power / USB-PD** — bus/supply behavior.

The model↔firmware contract (what's sampled, at what rate, averaged vs
switching-resolved) is set by D6.

## 7. Code layout

```
sw/sil/                 Rust cargo workspace (the framework) — additive, new
  sil-sys/                raw FFI (control ABI) + DWARF/symbol reader
  sil-core/               State Table, Route Table, sim clock, run modes, log
  backend/                FwBackend impls (native-freertos; arm-emu later)
  models/                 motor / encoder / sensor / power plant models
  dashboard/              realtime web UI (server + frontend)
  pybind/                 Python bindings for fast mode
sw/fw/src/                firmware control ABI + native entry / host-port wiring
sw/lib/c/shared/hw/*/sim/ bottom-layer sim drivers (no sim getters/setters)
docs/sil/                 these docs
```

Formal OFT specs for the SIL infra are deferred until the design firms up;
a new `sil` topic in `docs/spec-system.md`'s canonical table would be the
home for `fw~sil_*` requirements when we get there.

## 8. Risks / things to prove early

- **D1 spike** — a deterministic, framework-driven FreeRTOS tick on the host
  is the single highest-risk piece. Design is resolved
  ([`freertos-tick.md`](freertos-tick.md)); prove it with the two-task
  spike (§6 there) before building models on top.
- **Float determinism across Win/macOS** (D7) — decide early whether
  regression baselines are byte-exact or tolerance-based; it shapes how
  models and reductions are written.
- **USB-CDC sim transport** (D5) — coupled to the still-undecided CDC framing
  protocol; the desktop-app-as-indistinguishable goal depends on it.
- **Rebase friction** — the firmware-side changes touch `main.c` and module
  `CMakeLists`; keep them small, well-factored, and individually
  `main`-mergeable.
