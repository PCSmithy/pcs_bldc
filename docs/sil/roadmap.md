# SIL roadmap

Phased build-out + status tracking. Each phase has a goal and an exit
criterion (the demo that proves it). Keep this current as work lands.

Legend: ☐ todo · ◐ in progress · ☑ done

---

## Current sprint — interrupt-driven sampling (started 2026-08-13)

**North star:** phase currents sampled at the PWM-period center, hardware-
triggered from TIM1's trigger output, landed in firmware statics by the
injected-EOC interrupt — on **both targets**. Bench: telemetry shows
mid-window samples while six-step drives the motor. SIL: the D8 controller
dispatches the ISR on the sim grid and a scenario asserts sample cadence and
values against the plant. **No control law consumes the samples this sprint**
— the FOC current loop is its own later sprint.

**Why now:** the low-side shunts only see phase current during the 1−duty
window (campaign finding), so FOC needs center-of-period injected sampling;
D8 and the TRGO seam are its prerequisites. Closes `fw~hal_tim_006`,
`fw~hal_adc_003`, `fw~hal_adc_008` (OFT baseline 22 → 19).

**Stages** (each lands as its own reviewed commit):

- ☑ **1. D8 interrupt controller** (voyant + fiber port) — the framework
  interrupt table per [`sim-interrupts.md`](sim-interrupts.md): periodic +
  one-shot entries, config-time (by name via DWARF) and runtime (by pointer
  via a C→Rust upcall vtable, `SIL_ports`-style) registration,
  priority-then-registration-index ordering, per-entry enable,
  masked-holds-pending. Dispatch runs in the firmware fiber inside the
  port's ISR entry/exit bracket so `...FromISR` wakeups + `portYIELD_FROM_ISR`
  behave as on hardware. Exit: unit tests cover ordering, one-shot
  quantization, masking, cancel/disable, and a FromISR task wakeup.
- ☐ **2. Sub-ms engine grid + perf gating** — the engine step supports the
  interrupt grid (PWM period 50 µs at 20 kHz center-aligned; base `dt` per
  D8 §5 / D6). Systick keeps firing on integer multiples (D9). The mirror
  sweep + route propagation get a **gated cadence** (performance.md
  "gated discrete work" lever) so per-grid cost stays bounded; re-baseline
  the performance report and record the target there. Exit: existing 44
  SIL tests pass on the refined grid with suite runtime within budget.
- ☐ **3. Sim TRGO seam** (`fw~hal_tim_006`) — sim `HW_TIM` emits its
  configured trigger event (update / OC match): `HW_TIM_advanceTime`
  detects the crossing and calls registered sinks. Unity tests + tag close
  the spec.
- ☐ **4. Embedded injected ADC** (`fw~hal_adc_003/008`, stm32g4 target) —
  injected group on the phase-current inputs, TIM1-TRGO-triggered,
  interrupt transfer with JEOS completion callback + pollable status.
  NVIC/MSP/IT wiring (mind the unhandled-IRQ wedge; provide the handler,
  whole-archive). Revisit the AUTDLY interaction with the polled regular
  path (flagged when AUTDLY landed). Bench check: injected samples visible
  in telemetry while six-step runs.
- ☐ **5. Sim injected ADC mirror** — sim `HW_ADC` injected group: the TRGO
  sink starts the injected conversion from the driven port volts; completion
  lands via a D8 one-shot through the same callback/status surface as
  embedded (`fw~hal_adc_008` semantics on both targets). Unity tests + tags.
- ☐ **6. Firmware consumer + SIL north-star scenario** — a minimal firmware
  path (through the IO_bridge current seam, keeping app→IO layering)
  configures injected sampling and stores the latest center-sample phase
  currents from the completion callback. SIL scenario on the board world:
  six-step spinning, assert the ISR cadence (one sample set per PWM period),
  the sample instant (period center on the grid), and values matching the
  plant's phase currents at those instants.
- ☐ **7. Docs + re-baseline** — sim-interrupts.md implementation notes,
  performance.md new baseline, handover/backlog refresh.

**Dependencies:** 1 ∥ 3 ∥ 4 (independent starts); 2 needs 1; 5 needs 1 + 3;
6 needs 2 + 4 + 5.

**Explicitly out of scope** (deferred): the FOC current loop (own sprint);
`usb_cdc`/`teleplot` capture (owner call 2026-08-13); dead-time /
injectable-break seams; encoder frame-fault injection + `fw~safety_002`
scenario; event-driven timeline + ISR nesting (D8 §10).

---

## Commutation sprint — full-loop motor commutation ☑ (2026-07-12 → 2026-08-10)

**North star achieved:** the firmware's six-step drive commutates a simulated
motor end to end — button tap → alignment physically swings the rotor →
offset captured from the plant → dial demand → closed-loop spin — driven and
asserted purely through the State Table (`tests/north_star.rs`). Merged as
PR #4 (2026-08-13).

Stages, all ☑ (current-state detail lives in the design docs + tests):

1. String-keyed table write/read — tests interact with voyant, never a
   `Firmware` handle.
2. DuplexTransfer — synchronous member↔member serial transactions
   ([`state-route-tables.md`](state-route-tables.md)).
3. AS5048 encoder model (owner physics, `src/as5048.rs`).
4. PWM/bridge observation ports; plus MF4 trace export
   ([`signal-trace.md`](signal-trace.md)), the pytest-shaped `Sil` harness,
   and the firmware reset lifecycle ([`architecture.md`](architecture.md)).
5. Inverter + motor model (owner physics, `src/motor.rs`;
   `tests/motor_dynamics.rs` pins the analytics against the params).
6. Current-sense model + zero-latency sense wiring (`src/current_sense.rs`,
   `src/wiring.rs`); alignment harness.
7. North-star closed-loop scenario (`tests/north_star.rs`).

**Bring-up traps** (each costs a day if forgotten):

- Undriven ADC ports ramp to ~3.3 V ≈ 16 A computed phase current → the
  overcurrent latch trips almost immediately: current-sense ports must be
  driven every tick.
- Un-injected SPI reads back `0xFFFF` (error flag set) → the encoder-fault
  latch trips after 5 consecutive ticks: both AS5048 channels need valid
  frames.
- Drive stays blocked until `dev_gateDriver_isOperational()`: the sim I2C
  STATUS register must be seeded AND `task_200ms` must have completed a
  configure+status pass (first pass lands within ~200–400 ticks).

**Deferred out of the sprint:** D8 interrupt controller (the
interrupt-driven-control sprint that follows); `usb_cdc`/`teleplot`
sig_type (filed near the top of `backlog.md`).

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
- ☑ **Interrupt controller** (D8, `voyant::irq`): table of periodic + one-shot
  entries; config-time registration by name (DWARF `low_pc` + slide); C→Rust
  upcall vtable (`SIL_irq`) for runtime registration; port dispatch bracket
  (ISR entry/exit, deferred FromISR yield, per-context critical nesting).
  Landed in the interrupt-driven-sampling sprint, stage 1; the sim `HW_USB`
  driver is its first consumer (its device interrupt wakes `task_usb`)
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

Landed via the commutation sprint (above).

- ☑ Averaged-duty inverter model (norm leg duty + Vbus → phase voltages, D6)
- ☑ Motor model (electrical + mechanical; model-owned sub-stepped integrator)
- ☑ Encoder (AS5048) model → duplex-linked to the fw SPI driver
- ☑ Current sensor model → routed into fw ADC ports (voltage/temp sensors
  remain with the vsense-matching work)

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
