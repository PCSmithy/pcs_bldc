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
           │  ────────── FreeRTOS scheduler (cooperative fiber port · 1 thread) ──│
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
- **Single-threaded cooperative fibers, performance-first.** The whole sim
  (framework + firmware tasks-as-fibers) runs in one OS thread (D1). This is the
  foundation of both determinism and the many×-realtime performance target;
  the hot loop is built around it (gated discrete work, zero-alloc, a pluggable
  historian change-detector). See [`performance.md`](performance.md).
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

### 3.2 The public member seam and the internal execution backend

The framework's one **public** seam onto everything it executes is the
**`Member`** trait (see "Member model" below): the engine drives firmware
instances, plant models, and future native apps uniformly through it. Firmware is
one *kind* of member — a **`FirmwareMember`** wrapping the concrete **`Firmware`**
handle (native shared lib + DWARF white-box; white-box `read_cvar`/`write_cvar`
and lifecycle `start`/`shutdown` are inherent methods on that handle).

Behind `FirmwareMember` sits an **internal, crate-private** `Backend` trait — the
narrow execution seam the member drives *around each tick*:

```rust
pub(crate) trait Backend {
    fn advance_tick(&self);                    // let firmware run one tick of sim time
    fn read_cvar(&self, path: &str) -> Value;  // white-box read
    fn write_cvar(&self, path: &str, v: &Value); // white-box write / injection
    // + the port registration seam (defaults to no-ops)
}
```

It is **not** a public architectural seam. Its value is the in-crate test double
(mock backends prove `FirmwareMember`/engine semantics without a firmware DLL),
and it keeps the door open for a second execution mechanism to slot in behind
`FirmwareMember` without disturbing the engine, models, or modes. Lifecycle
(`start`/`shutdown`) and DLL loading are deliberately *off* the trait — they are
called on the concrete `Firmware` handle the driver holds.

- `Firmware` (native shared lib) — the first and primary backend (§4).
- `ArmEmu` — a possible later impl (Unicorn/QEMU) for a high-fidelity
  subset, same internal trait.

### 3.3 Open decisions (to resolve)

| # | Decision | Options | Leaning |
|---|----------|---------|---------|
| D1 | **FreeRTOS execution / time source** | — | **RESOLVED:** single-threaded **cooperative fiber port** (tasks = fibers, framework = main context, one OS thread) + pluggable tick source (realtime-paced vs framework-driven). Chosen for determinism *and* performance over OS-thread host ports. See [`freertos-tick.md`](freertos-tick.md) + [`performance.md`](performance.md). |
| D2 | **Rust↔C boundary** | — | **RESOLVED:** in-process, one fw instance/process, firmware as a dynamically-loaded shared lib; tiny control ABI only; all data via direct DWARF-located memory R/W (no sim getters/setters), surfaced as the State Table + Route Table. See [`ffi-boundary.md`](ffi-boundary.md) + [`state-route-tables.md`](state-route-tables.md). |
| D3 | **Python binding** | `pyo3` native extension vs C-ABI + `cffi`/`ctypes` | TBD (pyo3 likely) |
| D4 | **Dashboard stack** | Rust web framework (axum/...) + frontend plotting lib | TBD |
| D5 | **Sim USB-CDC transport** | virtual COM port (Win + macOS) vs TCP socket the app opts into | TBD; ties to the deferred CDC framing decision in `specs/system/overview.md` |
| D6 | **Inverter fidelity / base dt** | — | **RESOLVED (contract):** averaged-duty default, switching-resolved swappable; abc contract (normalized leg duty in, currents+angle out) routed via sim HW-driver state; base `dt` set by model stability (finer than PWM), control ISR fires on a multiple, model advances one `dt`/tick. Two values finalize with the firmware. See [`inverter-timestep.md`](inverter-timestep.md). |
| D7 | **Determinism & float tolerance** | — | **RESOLVED:** tolerance-based float assertions everywhere (default ε≈1e-6, per-signal override), no bit-exact contract; engine stays reproducible (deterministic scheduling + one seeded PRNG) so ε-tests don't flake; mind discrete threshold-divergence; behavioral-within-tolerance vs hardware. See [`determinism.md`](determinism.md). |
| D8 | **Simulated interrupt model** | — | **RESOLVED:** framework-owned interrupt table (periodic + one-shot; registered at config by name and at runtime by the sim HW layer via a C→Rust upcall); dispatched through the port in the firmware fiber context; fixed base-`dt` grid; priority-ordered, no nesting. See [`sim-interrupts.md`](sim-interrupts.md). |
| D9 | **Firmware time virtualization** | — | **RESOLVED:** sim time advances only at yields — every wait is a yield, every time read reflects the sim clock. Reads (`HAL_GetTick`/DWT/timer CNT) are sim-clock-derived; delays become cooperative sim-time advances; portable code uses FreeRTOS delays or a `HW_time` shim. See [`time-virtualization.md`](time-virtualization.md). |
| D10 | **Scenario / config representation** | how a test declares routes + model params + injection (config file? Python API? both) | TBD; the day-to-day test-authoring surface |
| D11 | **State Table binding stability** | symbol-path keys across fw rebuilds — fail-loud at load vs lazy | TBD; small convention |
| D12 | **Signal trace format** | — | **RESOLVED:** the State Table *is* the historian — each traced entry is a change-logged, timestamped time series (ZOH). Whole namespace traced by default (stacks/large arrays excluded); discretes exact, floats on an ε-deadband (default 1e-3, per-signal). Fast mode unbounded; realtime rolls to disk; MDF4 export. See [`signal-trace.md`](signal-trace.md). |

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
3. **A single-threaded cooperative FreeRTOS fiber port + pluggable tick source**
   (decision D1, resolved — see [`freertos-tick.md`](freertos-tick.md) +
   [`performance.md`](performance.md)). Tasks are fibers, the framework is the
   main context, all in one OS thread; the tick comes from a swappable source —
   wall-clock-paced (realtime) or framework-driven (deterministic). Chosen over
   the OS-thread host ports for determinism *and* performance (fiber swaps
   ~30–100 ns vs ~1–5 µs OS-thread switches, no cross-thread signaling). We
   author the fiber port rather than vendoring a host port.
4. **A sim ABI / native entry point** so the `FwBackend` can `init()` and
   `advance()` the firmware and the framework owns the outer loop (rather
   than `vTaskStartScheduler()` never returning).

**What this buys:** the real task priorities, blocking, scheduling order, and
sim-time timing of the shipping firmware are all under test — not a
reimplementation. (Preemption is *cooperative* — faithful in sim-time because
firmware bursts are instantaneous, with one fidelity trade noted in
[`freertos-tick.md`](freertos-tick.md) §5.)

## Member model

Everything the sim executes is a **`Member`** — the framework's single, uniform
seam. The [`Engine`](#5-sim-core-rust) holds shared members (`Rc<RefCell<dyn
Member>>`) and drives each one *only* through this trait, in registration order
(deterministic). `add_member` takes the member **by value** and returns a typed
handle (`Rc<RefCell<M>>`): ignorable for a plain model, or — when `M` also impls
`DuplexPeer` — linked straight to a bus so one struct fills both roles.

```rust
pub trait Member {
    fn name(&self) -> &str;                                    // instance name = <source> of its signals
    fn advance(&mut self, dt_us: u64, ctx: &mut MemberCtx);    // one deterministic step
    fn set_enabled(&mut self, on: bool, st: &mut StateTable);
}
```

`MemberCtx` is the initiator seam the engine hands each `advance`: `ctx.st` is the
State Table (register / record / read), and `ctx.duplex_transfer(handle, tx)` runs a
synchronous serial exchange against a linked peer (the model-side twin of the
firmware's C SPI upcall — both drive the engine's one shared `DuplexRouter`).

Members do **not** declare their signals (`signals()`) and are **not** pulled
(`read()`). Instead a member treats the State Table as its live workspace: it
**registers** its signals as a runtime act directly on the table (inside
`set_enabled(true)` / `advance`), **pushes** its outputs via `StateTable::record`,
and reads its routed inputs via `StateTable::current_value`. Registration is open
(see [`state-route-tables.md`](state-route-tables.md) §1): any member may register
any `sig_type`, and nothing infers a member's kind from what it registers. All
cross-member coupling flows through **routes**, never one member reaching into
another's signals (a discipline convention, not enforced — members are first-party
trusted code; a narrowed per-member table view is the escalation if scripted
members ever change the trust model).

**The firmware is one *kind* of member.** A **`FirmwareMember`** wraps a concrete
**`Firmware`** handle (DLL load, lifecycle, DWARF white-box) and drives it through
the internal, crate-private `Backend` seam (§3.2) — which demotes from "the
firmware seam" to internal plumbing behind the member. It is constructed with an
explicit instance **`name`** (not derived from the DLL — two boards may run the
same image as distinct members) and a firmware tick period; its `advance`
accumulates sim time and, per elapsed firmware-tick period, **flushes** the *fresh*
(route-/test-written) `cvar`s in its namespace into firmware memory, runs one
`advance_tick`, then **sweeps** its whole cvar leaf list back out into the table
(`record_mirror`). The cvar mirror is **automatic** — the member enumerates the
firmware's traceable namespace from DWARF at enable (minus a built-in
array-size/pointer exclusion policy; `exclude`/`include` tune it) and mirrors it
with **no per-signal declarations** (the D12 end-state). The flush is sparse (a
State Table **dirty set** tracks command writes; writes are one-shot,
last-writer-wins). It is the **only** thing that touches firmware memory — routes never do
(they are table-mediated; see
[`state-route-tables.md`](state-route-tables.md) §2). Lifecycle (`start`/`shutdown`)
stays an explicit call on the concrete `Firmware` handle the driver holds.
Multi-firmware is simply multiple `FirmwareMember`s, each syncing through its own
backend.

**Firmware ports.** A firmware member's sim HW drivers can register signals of
their own at runtime — **ports** — through a hook vtable the framework installs
over the control ABI (`sil_fw_setHooks`, before `sil_fw_start`; the null-safe
`SIL_ports` C helper wraps it, so hookless standalone/unit-test runs are
unchanged). Ports carry **native-format** values (volts stay volts; the driver
owns conversion to its C representation), and their I/O is **cache-mediated**
exactly like the cvar mirror — the member fills every port's input cache
from the table before each `advance_tick` and drains the port-write buffer back
into the table after it, so C never touches the State Table mid-tick. The C side
names only `{sig_type, local, unit}`; the member prefixes its instance name. The
sim `HW_ADC` (one input port per enabled input, volts→counts in the driver,
synthetic-ramp fallback when undriven) is the first user. See
[`state-route-tables.md`](state-route-tables.md) §1 "Ports".

**Multi-instance vision.** Firmware instances, plant/peer models, and future native
apps are all **peers** — members side by side in one engine. The `<source>` segment
of a signal's id names its producing member, so *N* firmware instances (the
multi-device stretch goal) and their models coexist in one flat State Table.
`RampModel` is voyant's reference model member; real plant models (motor, encoder,
sensors) are instantiation-side members.

**Enable semantics.** Members start enabled when added. The engine **gates** a
disabled member out — its `advance` is skipped while sim time keeps flowing, and
its signals hold their last recorded value (a firmware member disabled = **held in
reset**: memory frozen, sim time flowing). `set_enabled` is where a member
(re-)registers its signals (idempotent) and runs its member-kind-specific *re-enable
depth*. A `FirmwareMember` with a **reload recipe** (`set_reload_path`) reboots on
re-enable: it shuts the old image down, drops its `Rc` (sole ownership), reloads the
same path from reset, and rebuilds every image-bound cache — State-Table entries
re-register idempotently, so signal history is preserved across the reload. Without a
recipe, re-enable resumes advancing. A model member's own reinit (integration state)
is the same seam when a model wants it.

The per-tick order the engine runs — advance sim time → validate wiring if dirty →
propagate delayed routes from a pre-tick snapshot → for each enabled member:
resolve the zero-latency route DAG (topo order, fresh reads) then advance — and the
**settled per-route latency** model (0 forward / 1 delayed ZOH cut) are documented
in [`state-route-tables.md`](state-route-tables.md) §3. Member registration order is
a design surface for forward flow; the step-time validator flags a route that needs
to be delayed or reordered.

## 5. Sim core (Rust)

One **sim clock** drives a discrete time-step engine at a fixed base `dt`
(D6; set by model numerical stability, typically finer than the PWM rate, with
ISRs firing on integer multiples). State flows through the State
Table; the Route Table moves it (see
[`state-route-tables.md`](state-route-tables.md) §3 for the settled per-route
latency model). Each step:

1. Advance sim time; **validate the wiring if dirty** (acyclic zero-latency graph,
   forward flow along member order, single-driver) — a bad verdict is raised at
   this step and cached until fixed.
2. **Propagate delayed (latency-1) routes** from a snapshot taken before any member
   advances — each delayed destination gets its source's end-of-previous-tick value
   (the ZOH cut).
3. **For each enabled member, in registration order:** re-resolve the enabled
   **zero-latency** routes in topological order with fresh reads (forward dataflow,
   zero added latency), then advance the member. This is **table-only**: routes move
   values between entries and never touch a backend; the firmware member flushes the
   fresh (route-written) `cvar` destinations into firmware memory and sweeps its
   whole cvar mirror back out inside its own advance (`sil_fw_advance_tick` runs to
   quiescence — race-free, the firmware is quiescent here, D1).
4. Append changed signals to the State Table historian (change-logged,
   per-signal timeseries — D12); run test asserts/injection (ad-hoc State
   Table reads/writes). The `Engine` holds **no backend handle** — it touches
   only members, routes, and the table.

The State Table also owns a **unified log system**: a bounded, drop-oldest ring of
`LogEntry { time_us, level, source, message }` that members and the driver append to
via `StateTable::log(level, source, msg)`. The table stamps each entry with its
*current sim time* (members cannot fake a timestamp, and logging never feeds back
into behaviour — determinism is untouched); the driver drains it with `take_logs`.
The swallowed-`record`-error sites now log a `Warning` instead of dropping the error.

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

## 7. Code layout — generic framework + instantiation

The framework is a **generic, firmware-agnostic crate** (`voyant`) that could
stand alone / be open-sourced; this board is just an **instantiation** that
implements voyant's trait seams. Nothing board-specific lives in `voyant`.

```
sw/sil/                 Rust cargo workspace
  voyant/                THE FRAMEWORK (generic, no pcs_bldc): the native
                         firmware backend (control ABI + DWARF white-box), and
                         the State Table, Route Table, sim clock, historian, run
                         modes, plus the trait seams a project implements:
                           Member    — any executable participant (the engine
                                       drives everything through this); a model
                                       member, or a FirmwareMember wrapping the
                                       concrete Firmware handle. (An internal,
                                       crate-private Backend trait is the
                                       test-double seam behind FirmwareMember —
                                       not something a project implements.)
                           Transport — a comms bus (tx → rx, completion timing)
                           Scenario  — the project's wiring (members/routes/trace)
  pcs_bldc_sil/          THE INSTANTIATION: impls voyant's traits for this board
                         (motor/encoder/sensor models, firmware config, routes).
                         A lib (src/) exposes `Sil` — the simulation itself, derefing
                         to its `Engine`. `Sil::new()` is a zero-firmware world;
                         `sim.load_firmware(name)` boots one instance per call (image
                         copied per load), and drop unloads + deletes the copies. Each
                         scenario is an independent `#[test]` in tests/*.rs; the perf
                         bin (main.rs) prints the per-tick performance report.
sw/fw/src/                firmware control ABI + native entry / fiber-port wiring
sw/lib/c/shared/hw/*/sim/ bottom-layer sim drivers (no sim getters/setters)
docs/sil/                 these docs
```

(Dashboard + Python bindings become further crates/members as they land. The
framework is developed in-repo now and is extractable to its own repo later —
the `voyant` ↔ `pcs_bldc_sil` boundary already enforces the no-board-code rule.)

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
