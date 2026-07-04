# SIL roadmap

Phased build-out + status tracking. Each phase has a goal and an exit
criterion (the demo that proves it). Keep this current as work lands.

Legend: ☐ todo · ◐ in progress · ☑ done

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
    cache + injection **overrides** + **per-signal epsilon** (default 1e-3) +
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
- ☐ **Route Table**: `source → destination`, one snapshot-then-write pass/tick,
  with per-route suspend/resume
- ☐ **Interrupt controller** (D8): table of periodic + one-shot entries;
  config-time registration by name; C→Rust upcall vtable for runtime
  registration; port dispatch shim (ISR entry/exit, FromISR/yield)
- ☐ Sim clock + step loop + **State Table historian** (change-logged,
  timestamped per-signal series; dump at end-of-run — D12)
- ☐ **Perf seams from the start** (performance.md): zero-alloc hot loop +
  columnar historian buffers; gated discrete work (continuous-integration every
  tick, firmware/routes/algebraic-models/historian-scan gated); pluggable
  change-detector (naive-scan now, dirty-page later)

## Phase 3 — Plant models + closed loop
**Goal:** motor + encoder + sensor models close the loop with firmware.
**Exit:** firmware commands PWM → motor model spins → encoder/ADC feed back →
a basic control action is observable end-to-end.

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
