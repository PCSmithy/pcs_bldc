# SIL roadmap

Phased build-out + status tracking. Each phase has a goal and an exit
criterion (the demo that proves it). Keep this current as work lands.

Legend: ☐ todo · ◐ in progress · ☑ done

---

## Current sprint — full-loop motor commutation (started 2026-07-12)

**North star:** the firmware's six-step drive commutates a simulated motor
end-to-end — PWM out → motor model spins → encoder/current models feed back —
driven and asserted purely through the State Table.

**Owner-set API direction** (from review of the sanity suite, 2026-07-12):

- Tests interact with voyant (the State Table), never a `Firmware` handle.
  Direct cvar writes are the rare case, and even those go through the table:
  `st.write("cvar:pcs_bldc:HW_USB_sim_data.connected", true)`.
- The same call shape writes model inputs —
  `st.write("vsig:as5048:angle_deg", 90.0)` — one uniform API for cvars,
  model inputs, and (later) bus signals.
- Target end-state (Python, Phase 4) the Rust API must shape toward:

  ```python
  def test_encoder_spi_comms():
      sim["vsig:pcs_bldc:usb:connected"] = True
      sim.run_for(10)
      sim["vsig:as5048:angle_deg"] = 90.0
      sim.run_for(1)
      assert sim["usb:pcs_bldc:motor_angle"] == 90.0
  ```

**Stages** (each lands as its own reviewed commit):

- ☑ **1. String-keyed table write/read** — `write`/`read` over
  `SignalId::parse` → `record`/`current_value`; migrate the
  sanity suite off direct `fw.read_cvar`/`write_cvar` (only boot/shutdown
  stay below the engine).
- ☑ **2. DuplexTransfer** — synchronous member↔member serial transactions
  (`spi` first), an **engine-scoped** primitive. An engine-owned `DuplexRouter`
  couples any initiating member to a linked `DuplexPeer`: a model initiates via
  `MemberCtx::duplex_transfer`, a firmware member through its C SPI upcall — both
  forward into the same router (tx in, the peer's rx back synchronously, one
  full-duplex step, no D8). The engine force-records each exchange as
  `spi:<ep>:tx` / `:rx` event entries (`Value::Bytes`) for the historian. The
  sim `HW_SPI` `injectedRx` inject + MOSI loopback are removed; an unhandled
  transfer reads `0xFF` (a floating/disconnected bus).
- ☑ **3. AS5048 encoder model** (`pcs_bldc_sil/src/as5048.rs`,
  owner-implemented) — one `vsig:<name>:angle` input (canonical rad; unit
  asks convert at the boundary) + `raw_encoder_ticks` out; u16-native
  command parse (even parity, read/addr), one-frame response pipeline,
  parity on every outgoing frame. Two instances linked in check 4: the
  motor encoder answers the firmware's real READ-ANGLE polls
  (`write angle[deg]=90` → telemetry `90.00`), the dial idles at 0 —
  its demand path (`st.write("vsig:dial:angle[deg]", …)`) is exercised
  in stage 6/7.
- ☑ **4. PWM/bridge observation ports** — sim `HW_TIM` publishes the commanded
  bridge state as output ports (the D6 route source): normalized per-phase duty
  (compare/period ∈ [0,1]), per-phase enable, and one master output enable
  (`vsig:pcs_bldc:PWM_{U,V,W}_{duty,enabled}` + `TIM1_MOE`, all f64 0/1 for
  flags). Publication is event-driven from the setters; raw CCR/ARR never crosses
  the boundary. `HW_TIM_sim.h` + the carrier/waveform sim machinery are deleted;
  the Unity suite exercises the production seam via a test-owned hooks double.
  Suite check 12 pins registration + the dark-bridge boot state.
- ☑ **4.5. Trace visibility (MF4 export)** — every SIL run can drop ASAM MDF4
  (`.mf4`) trace files of the State Table historian, openable in asammdf's GUI.
  voyant serializes the change-log as a versioned little-endian binary stream
  (pure Rust, no Python knowledge — `Engine::dump_trace`); the suite spawns the
  venv's `tools/mf4_build.py` (asammdf) over stdin to write `.mf4` — enum signals
  carry value-to-text conversions, `Bytes` signals are skipped, units come from
  the canonical registration, and a missing venv degrades to a raw `.bin` next to
  the target (never loses data). `PCS_SIL_TRACE_DIR` gates per-test drops (each `Sil`
  world dumps `<test-fn>.mf4` on drop); `tools/validate_mf4.py` round-trips
  `end_to_end.mf4` + `adc_ports.mf4`. This is the model-validation instrument for
  stage 5 — plot the plant against the firmware.
- ☑ **4.6. pytest-shaped test harness** — every SIL scenario is an independent
  `#[test]` in `pcs_bldc_sil/tests/*.rs` over a `Sil`: **the simulation itself**, which
  derefs to its `Engine` (so `sim.add_member` / `sim.link_duplex` / `sim.step` are
  direct). `Sil::new()` is a zero-firmware world — model-only scenarios (`duplex`,
  `encoder`) are first-class; `sim.load_firmware(name)` boots one firmware instance per
  call, its image copied to a unique temp path so N instances (and repeated loads of one
  path) each get their own module statics. `Drop` dumps a per-test `<test-fn>.mf4`
  trace, shuts every firmware down in reverse load order, unloads the libraries (the
  last `Rc<Firmware>` drops → `FreeLibrary`), and deletes the temp copies. voyant's
  `FirmwareMember` owns its firmware via `Rc`, so `Engine` carries no lifetime. A
  process-global mutex, taken at the first firmware load, serializes vanilla (threaded)
  `cargo test` — the sequential single-process baseline — and is uncontended under
  cargo-nextest's process-per-test model. `tests/lifecycle.rs` is the reload gate: the
  fiber port re-inits cleanly on a fresh thread **and** back-to-back on one thread (it
  un-converts the thread at `shutdown`), so `reload_cycles_same_thread` boots N images
  in a row on a single thread. `tests/multi_firmware.rs` (`two_firmwares`) runs two
  images sharing one thread (the second borrows the first's fiber conversion). The
  sanity-check bin shrinks to the perf report; `tools/run_sil.sh` runs the checks, then
  the perf bin. Firmware clocks assert start-from-reset + 1000 us/tick, never sim-axis
  alignment. nextest integrated (process-per-test parallel; run_sil.sh prefers it, cargo
  test remains the fallback). **Reset lifecycle available** (`tests/reset.rs`): disabling
  a firmware member holds it in reset (memory frozen, sim time flows); re-enabling with a
  reload recipe (`FirmwareMember::set_reload_path`, wired by `Sil::load_firmware`) reboots
  a fresh image from the same path — statics from reset, DWARF/bindings/ports/duplex
  rebuilt, signal history preserved — on one continuous timeline. `firmware_reset_lifecycle`
  proves the sawtooth (100 ms up, 100 ms dark, 100 ms up).
- ☑ **5. Inverter + motor model** (owner-implemented physics) — averaged-duty
  inverter into a trapezoidal-BEMF BLDC model, f64 throughout: per-leg hybrid
  ideal-diode modes (Driven / Clamped(rail) / Open — demag freewheel to exact
  zero, voltage re-engagement with anti-chatter margin), R/L phases about a
  floating neutral, J/B mechanics, semi-implicit Euler at 1 µs sub-steps.
  Ports follow the declare-your-own-namespace convention: inputs
  `vsig:<motor>:{duty,enable}_{u,v,w}/moe/vbus` fed by `wire_bridge`'s seven
  delayed routes (`src/wiring.rs`; suspend-and-write = bridge fault
  injection, proven in `tests/motor.rs`); outputs add
  `terminal_voltage_*`/`bemf_*`/`neutral_voltage` for vsense matching.
  `tests/motor_dynamics.rs` (8 physics tests, model-only worlds, per-test
  MF4 drops) pins locked-rotor analytics, demag both rails, KCL through
  commutation, coast at τ_mech = J/B, SVPWM common-mode rejection, and
  collapsed-bus diode rectification — expectations derived from the params.
  Known approximation (backlogged): per-terminal diode window below the
  line-to-line conduction threshold colors undriven terminal voltages.
- ◐ **6. Feedback + harness models** — current-sense model driving the
  existing ADC ports (U=ADC1_IN6, V=ADC2_IN7, W=ADC1_IN8, bus=ADC2_IN11)
  every tick; STSPIN32G4 I2C STATUS seeding (LOCK set, faults clear); button
  gestures via sim GPIO. Alignment harness landed — `tests/alignment.rs`
  drives the button-to-alignment path end to end: I2C STATUS seeded
  (`HW_I2C_data.buses[1].devices[0].regMem[128]`), the four ADC ports held at
  zero current, a button tap injected via the `HW_GPIO_data.inputLevel[port]
  [bit]` static (the `cachedInput` mirror is recomputed from `inputLevel` at
  the top of every tick — before `dev_switch` reads it — so it can't hold an
  injected value; the resolver now addresses flat multi-dim DWARF leaves),
  dial demand turned. The current-sense **model** itself is the remaining
  piece (the ports are held constant here, not plant-driven).
- ☐ **7. North-star scenario** — seed gate driver → alignment dwell (500 ms)
  → dial + button tap → assert the sector sequence advances, the rotor spins,
  currents stay under trip, telemetry reports motion; fault-injection
  variants (suspend + write overcurrent, starved encoder) prove the latches.

**Bring-up traps** (from the 04b2cf8 firmware survey — each costs a day if
forgotten):

- Undriven ADC ports ramp to ~3.3 V ≈ 16 A computed phase current → the
  overcurrent latch trips almost immediately: current-sense ports must be
  driven every tick.
- Un-injected SPI reads back `0xFFFF` (error flag set) → the encoder-fault
  latch trips after 5 consecutive ticks: both AS5048 channels need valid
  frames.
- Drive stays blocked until `dev_gateDriver_isOperational()`: the sim I2C
  STATUS register must be seeded AND `task_200ms` must have completed a
  configure+status pass (first pass lands within ~200–400 ticks).
- ~~The sim TIM2 counter is frozen~~ — fixed 2026-07-28: `countsPerUs`-
  configured sim TIM counters advance with sim time
  (`HW_TIM_advanceTime`, called per tick from `sil_fw_advance_tick`);
  check 2 asserts `lib_timer` time flows.

**Deferred** (owner, 2026-07-12): **D8 interrupt controller** — the current
control path is entirely cooperative in `task_1ms`, so D8 isn't needed until
the interrupt-driven control sprint that follows this one.
**`usb_cdc`/`teleplot` sig_type** — filed near the top of `backlog.md`.

---

## Phase 0 — Worktree + architecture
**Goal:** branch, worktree, agreed ground rules, planning docs.
**Exit:** `architecture.md` decisions table reflects reality.

- ☑ `sil` branch + worktree at `C:/code/pcs_bldc-sil`
- ☑ README / architecture / roadmap drafts
- ☑ **D1** resolved — FreeRTOS tick source ([`freertos-tick.md`](freertos-tick.md))
- ☑ **D2** resolved — Rust↔C FFI boundary ([`ffi-boundary.md`](ffi-boundary.md))
- ☑ **D8** resolved — simulated interrupt model ([`sim-interrupts.md`](sim-interrupts.md))
- ☑ **D6** resolved (contract) — inverter fidelity + base `dt` ([`inverter-timestep.md`](inverter-timestep.md))
- ☑ **D9** resolved — firmware time virtualization ([`time-virtualization.md`](time-virtualization.md))
- ☑ **D7** resolved — determinism & float tolerance ([`determinism.md`](determinism.md))
- ☑ **D12** resolved — signal trace = State Table historian ([`signal-trace.md`](signal-trace.md))
- ☐ Close out remaining open decisions D3–D5, D10–D11

## Phase 1 — Firmware runs on the SIM target
**Goal:** FreeRTOS + io/dev/app build and run natively; only the
hardware-specific bottom layer is swapped.
**Exit:** a native binary boots FreeRTOS, spawns the real tasks, and the
synthetic ADC ramp advances under the scheduler.

- ☑ **D1 spike (fiber-based):** cooperative fiber FreeRTOS port, two-task
  program, framework-driven tick, idle-fiber quiescence — `sw/sil/spike/d1-tick/`.
  **PASS:** 30 serial + 16 parallel runs bit-identical; ~5.0 Mticks/s (~5000×
  realtime at 1 kHz) on one core, near-linear parallel scaling.
- ☑ **Fiber port promoted** to canonical `sw/lib/c/FreeRTOS/portable/Native-Fiber/`;
  FreeRTOS CMake is dual-target (ARM_CM4F embedded / fiber native); native
  FreeRTOSConfig + `pcs_freertos_config` in `hw/sim/`.
- ☑ **FreeRTOS bring-up ungated in `main.c` (SIM)** — minimal 1 ms task drives
  `HW_GPIO_run1ms`/`HW_ADC_run1ms`, temporary in-process tick loop. **Native
  binary boots FreeRTOS, the task runs each tick, the ADC ramp advances**
  (Phase-1 exit met); embedded ARM `.elf` unaffected.
- ☑ **Control ABI seam** (`sw/fw/src/sil_fw.h`): `sil_fw_start` /
  `sil_fw_advance_tick` / `sil_fw_shutdown` (D2). `main.c` is now a thin driver
  over it (embedded entry / SIM smoke driver); Phase-2 Rust drives the same
  three calls. Pacing (fast vs realtime) is the driver's choice.
- ☐ Ungate the rest of io/dev/app in `main.c` for SIM (the real tasks)
- ☐ Cross-platform context-switch primitive (macOS ucontext / small asm)
- ☐ Realtime-paced driver (wall-clock pacing of `sil_fw_advance_tick`)
- ☐ Sim impls of the remaining bottom-layer drivers (USB CDC stub, any
  direct-hardware pokes); IO_AS5048 / IO_SK6805 build for SIM over `HW_SPI`
- ☐ `HW_time` module (stm32g4 + sim) + audit drivers for direct
  `HAL_Delay`/`DWT`/timer-`CNT` use (D9)
- ☑ Native build is green (`tools/build_native.sh`) — both targets build

## Phase 2 — Rust sim core + FFI backend
**Goal:** the `NativeFreeRtos` `FwBackend` drives the firmware; sim clock,
scheduler, signal log working.
**Exit:** Rust steps the firmware in sim time, reads the ADC ramp by symbol,
writes a global and sees the firmware react. (proof of white-box loop) — **MET.**

- ☑ Native **SHARED** firmware target (`libpcs_bldc_fw.dll`) exporting the
  `sil_*` ABI + globals. Validated with a C `LoadLibrary` harness: load → drive
  ticks → read `sim_task1msRuns` live from DLL memory (the white-box loop).
- ☑ Cargo workspace `sw/sil/` (`sil-sys` raw FFI, `sil-core` driver) — rustup
  `*-windows-gnu` toolchain installed.
- ☑ **Proof-of-life loop in Rust:** `sil-core` loads `libpcs_bldc_fw.dll`, drives
  it over the control ABI (`start`/`advance_tick`/`shutdown`), and reads
  `sim_task1msRuns` live by symbol (1→20). Full stack proven: Rust → ABI →
  fiber scheduler → task → white-box read.
- ☑ **`Backend` trait + `Firmware` impl** — the execution-backend seam
  (architecture.md §3.2) formalized: `voyant::Backend` (lifecycle
  `start`/`advance_tick`/`shutdown` + `cvar` read/write by path), with `Firmware`
  as its first impl. (An ARM-emu backend could be a later impl.)
- ☑ DWARF reader (`object`+`gimli`) — `sil-sys::DwarfMap` resolves
  `var.member`, `arr[i]`, nested paths → link address + scalar leaf kind
  (members, array indexing, typedef/const/volatile pass-through, base/enum
  scalar types); ASLR slide via the `sim_task1msRuns` anchor. `Firmware::read`/
  `write` give typed `Value` access. **Proven:** Rust reads the real ADC ramp
  `HW_ADC_data.channelData[0].counts[6]` and **writes** `tickCounter` → the
  firmware recomputes the ramp from it (Phase-2 exit). Next: pointer-chasing +
  qualified paths when the State Table proper lands.
- ☑ **Restructured** into the generic **`voyant`** framework crate + the
  **`pcs_bldc_sil`** instantiation (open-source-able framework; board code only
  in the instantiation). DWARF demo runs through it unchanged.
- ☑ **State Table design locked** (state-route-tables.md): trait-backed entries
  (`cvar`/`vsig`/comms) over one `Value` enum; naming convention
  `<sig_type>:<source>:<local>[:<modifier>]`; comms = logical payloads captured
  via sim-HW upcall; routes first-class + suspendable (injection).
- ☑ **State Table impl** (pure-data historian design) — `voyant` modules
  `signal`/`state_table`/`backend`/`dwarf`:
  - `signal`: `SignalId` (owned, validated `sig_type:source:name[:modifier]`) +
    the logical `Value` (`F32/F64/I32/U32/U64/Bool/Enum/Bytes`) + `approx_eq`.
  - `state_table`: registry + **per-signal change-logged history** + current
    cache + **per-signal epsilon** (default 1e-3) +
    time-based retention (`None`=unbounded fast mode). `record`/`force_record`/
    `current_value` (O(1))/`value_at` (O(log n) ZOH). *No FFI — pure data.*
  - `backend`: the **cvar sample-resolver** — `read_cvar`/`write_cvar` coerce
    firmware widths ↔ `Value` (the only unsafe/DWARF part).
  - 10 unit tests; demo samples firmware into the table, shows change-log dedup,
    ZOH lookup, and inject-and-react. Next: `vsig` (Model trait), comms, routes.
- ☑ **`Model` trait + `vsig` backing** (`voyant::model`) — minimal `Model`
  (`name`/`signals`/`advance(dt_us)`/`read`) + `ModelSignal`; `register_model` /
  `record_model` sample a model into the State Table the way cvars are sampled
  (StateTable stays pure data — the glue lives in `model`, not the table).
  `RampModel` reference impl; 8 new unit tests; sanity-suite check 5 shows
  register → advance-with-time → historian record.
- ☑ **Route Table** (`voyant::route`): `RouteTable` of `source → destination`
  routes, one **snapshot-then-write** pass/tick (`propagate` snapshots all enabled
  sources from the State Table cache, then writes all dests — order-independent,
  one hop/tick). Sources are any entry (`vsig`/`cvar`); `cvar` dests are driven
  via `Backend::write_cvar` (DWARF path = id `name`). Per-route `suspend`/`resume`
  for fault injection (suspend + direct write; the table `override` it once paired
  with was removed 2026-07-12); `vsig` dests deferred to
  a `Model::write` seam. 8 unit tests + sanity-suite check 6 (model `vsig` →
  firmware `cvar`, suspend/resume gating).
- ☐ **Interrupt controller** (D8): table of periodic + one-shot entries;
  config-time registration by name; C→Rust upcall vtable for runtime
  registration; port dispatch shim (ISR entry/exit, FromISR/yield) —
  **deferred** (owner, 2026-07-12) to the interrupt-driven-control sprint;
  the current firmware control path is fully cooperative in `task_1ms`
- ☑ **Sim clock + step loop** (`voyant::engine`): `Engine` owns the State Table /
  Route Table / models, borrows a `Backend`, and `step()`s the canonical order —
  advance sim time (monotonic, wall-clock-free) → advance models (registration
  order) + record vsigs → propagate routes → `advance_tick` → sample registered
  cvars into the historian. `add_model`/`add_route`/`sample_cvar` registration;
  model vsig ids + sampled list resolved once (hot loop skips `Model::signals()`).
  7 unit tests; the sanity suite drives checks 2/3/4/5 through the engine. (The
  historian itself — the change-logged per-signal series — is the State Table,
  already done above; end-of-run trace **dump/MDF4 export** remains, D12.)
- ☐ **Perf seams from the start** (performance.md): zero-alloc hot loop +
  columnar historian buffers; gated discrete work (continuous-integration every
  tick, firmware/routes/algebraic-models/historian-scan gated); pluggable
  change-detector (naive-scan now, dirty-page later)

## Phase 3 — Plant models + closed loop
**Goal:** motor + encoder + sensor models close the loop with firmware.
**Exit:** firmware commands PWM → motor model spins → encoder/ADC feed back →
a basic control action is observable end-to-end.

Staged in detail in **Current sprint** above (stages 3–7 map here).

- ☐ Averaged-duty inverter model (norm leg duty + Vbus → phase voltages, D6)
- ☐ Motor model (electrical + mechanical; model-owned sub-stepped integrator)
- ☐ Encoder (AS5048) model → routed into fw SPI-rx state
- ☐ Current / voltage / temp sensor models → routed into fw ADC counts

## Phase 4 — Fast mode + Python/pytest
**Goal:** scripted, deterministic, parallel regression.
**Exit:** a pytest test boots the sim, runs a scenario, asserts on signals,
and passes deterministically under `pytest -n`.

- ☐ Python bindings (D3)
- ☐ Scenario/scripting API (set params, inject, step, assert)
- ☐ Determinism story locked (D1 + D7)
- ☐ First regression test + CI hook

## Phase 5 — Realtime mode + desktop-app bridge
**Goal:** sim time tracks wall-clock; desktop app talks to it as the board.
**Exit:** the desktop app connects over USB-CDC to the sim and can't tell it
from real hardware.

- ☐ Wall-clock pacing of the step loop
- ☐ Sim USB-CDC transport (D5) — virtual COM / socket
- ☐ Desktop-app connection validated

## Phase 6 — Web dashboard
**Goal:** live observability + interaction in realtime mode.
**Exit:** browser dashboard plots signals live, injects values, tweaks model
params on a running sim.

- ☐ WebSocket server (D4)
- ☐ Live signal plotting
- ☐ Signal injection + model-parameter manipulation UI

## Ongoing
- ☐ Periodic `git rebase main` from the `sil` worktree
- ☐ Keep firmware-side changes small + individually `main`-mergeable
- ☐ Merge `sil` → `main` once the harness is proven
