# SIL bring-up — handover

Last updated: 2026-07-04. Orientation for picking up the SIL (software-in-the-
loop) effort in a fresh session. Read this first, then `README.md` +
`roadmap.md` in this folder.

## TL;DR

We're building **`voyant`** — a generic, firmware-agnostic framework that runs
the pcs_bldc firmware (cross-compiled to a host shared library) in a
deterministic virtual world, with total white-box access to its state.
**Phase 1 (firmware runs on the SIM target) and the Phase-2 core (Rust drives +
introspects the firmware) are done and working.** The design is fully decided
and captured (decisions D1–D12). What's left is the *engine* on top of the State
Table: models, the Route Table, the sim clock/step loop, comms, run modes, and
Python bindings.

All work is on branch **`sil`** (worktree `C:/code/pcs_bldc-sil`), pushed to
`origin/sil`. The branch was **rebased onto current `main`** (`c9b4238`, the
complete firmware foundation) on 2026-07-04, and the **firmware/SIL build was
unified**: the SIM target now runs the SAME four FreeRTOS tasks and real
io/dev/app modules as embedded (see "What's done").

## Build & run

```bash
tools/run_sil.sh            # build firmware DLL + voyant, run the sanity suite
tools/run_sil.sh --clean    # wipe the native build dir first
```

The suite loads the firmware DLL, drives it over the control ABI, and runs
named PASS/FAIL checks (exit nonzero on any failure): boot; all four real
FreeRTOS tasks advancing; State Table historian/enum/ZOH; and an end-to-end
path — inject an AS5048 SPI frame via DWARF write, assert the exact angle
comes out in the telemetry text captured by the sim USB driver. All
injection/inspection is white-box DWARF access (never the deprecated `_sim_*`
C APIs — see `backlog.md`).

Rust unit tests: `cd sw/sil && cargo test -p voyant` (26 tests).

**Rust toolchain gotcha:** it's `stable-x86_64-pc-windows-gnu` (matches MinGW),
installed at `~/.cargo/bin`. The Bash tool's shell does NOT source `~/.bashrc`,
so prefix cargo commands with `export PATH="$HOME/.cargo/bin:$PATH"`. (The
user's own terminals get it via `~/.bash_profile`.) See
`memory/reference_rust_toolchain.md`.

## Code map

```
sw/sil/
  voyant/                 THE FRAMEWORK (generic, no pcs_bldc code)
    src/signal.rs           SignalId (sig_type:source:name[:modifier]) + Value
    src/state_table.rs      StateTable: registry + per-signal change-log history
                            + current cache + overrides + per-signal epsilon
                            + retention. Pure data, no FFI. (10 unit tests)
    src/backend.rs          Backend trait (lifecycle + cvar R/W) + Firmware, its
                            first impl: control ABI + cvar sample-resolver
                            (read_cvar/write_cvar; the only unsafe/DWARF part)
    src/model.rs            Model trait + vsig backing: register_model/record_model
                            glue + RampModel reference impl (models sampled into
                            the State Table like cvars; StateTable stays pure data)
    src/route.rs            RouteTable: source->destination transport, one
                            snapshot-then-write pass/tick, per-route suspend/resume
                            (pure data + explicit propagate; cvar dests via Backend)
    src/dwarf.rs            DwarfMap: resolve var.member/arr[i] paths -> Leaf
                            (Scalar | Enum), incl. enum value->name
  pcs_bldc_sil/           THE INSTANTIATION (board-specific driver/demo)
    src/main.rs             loads the DLL, builds a StateTable, runs the demo
  spike/d1-tick/          standalone D1 spike: cooperative fiber FreeRTOS port
                            determinism + throughput test (throwaway scaffolding)

sw/lib/c/FreeRTOS/portable/Native-Fiber/   the native cooperative fiber port
sw/fw/src/sil_fw.h                          the control ABI (start/advance/shutdown)
sw/fw/src/main.c                            SIM path: fiber-port bring-up + ABI impl
sw/fw/src/hw/sim/FreeRTOSConfig.h           native FreeRTOS config
tools/run_sil.sh                            one-command build + run

docs/sil/*.md            the design (see "Design docs" below)
```

## What's done

- **Design (D1–D12): all decided + captured** in `docs/sil/`. Key choices:
  single-threaded cooperative **fiber** execution (determinism + perf); native
  firmware as a **shared library** driven in-process; DWARF white-box; **State
  Table + Route Table** with the State Table *being* the historian; comms
  captured as state via sim-HW upcalls; generic **`voyant`** framework +
  `pcs_bldc_sil` instantiation.
- **Phase 1 — firmware on the SIM target (done):** FreeRTOS runs on native via
  the hand-written cooperative fiber port; the firmware builds as
  `libpcs_bldc_fw.dll` exporting the `sil_fw_*` control ABI; both native and
  ARM targets build green. (D1 spike proved determinism + ~5000× realtime.)
- **Phase 2 core — Rust drives + introspects (done):**
  - `voyant::Firmware` loads the DLL, calls the control ABI, and reads/writes
    **any** firmware `static` by DWARF path — scalars, struct members, **array
    elements**, and **enums by symbolic name**.
  - **State Table** implemented: `SignalId`, logical `Value`, per-signal
    change-logged history (dedup + per-signal epsilon, default 1e-3), current
    cache (O(1)), `value_at` ZOH (O(log n)), injection **overrides**, time-based
    retention (`None` = unbounded for fast mode).
- **Rebased onto current `main` + firmware/SIL build unified (2026-07-04):**
  the SIM target runs the SAME four FreeRTOS tasks (`task_1ms`, `task_10ms`,
  `task_usb`, `telemetryTask`) and the same io/dev/app init as embedded, via
  shared `prvHwInit`/`prvAppInit`/`prvCreateTasks` in `main.c`. Target gating
  survives only at the hw-layer seam plus `HAL_Init`/timebase/`__io_putchar`
  and the per-target entry points. All io/dev/app modules link on both targets
  against real sim hw drivers; sim `HW_USB_run()` yields via `vTaskDelay(1)`
  (interim until D8). Per-task heartbeat counters + the sanity suite (above)
  prove the real code runs natively. Embedded ELF impact: +~80 B flash,
  +16 B bss.

## What's next (prioritized)

1. ~~**`Model` trait + `vsig` backing**~~ — **DONE (2026-07-04).** `voyant::model`
   adds the minimal `Model` trait (`name`/`signals`/`advance(dt_us)`/`read`), the
   `ModelSignal` descriptor, and `register_model`/`record_model` glue that samples
   a model into the State Table exactly like cvars (StateTable stays pure data).
   `RampModel` is the reference impl; the sanity suite check 5 demonstrates
   register → advance-with-time → historian record. Plant models come later
   (Phase 3, instantiation-side).
2. ~~**Route Table**~~ — **DONE (2026-07-04).** `voyant::route` adds `RouteTable`
   (`add`/`suspend`/`resume`/`propagate`): a flat list of `source → destination`
   routes propagated in one **snapshot-then-write** pass per tick (snapshot all
   enabled sources from the State Table's current cache, then write all dests), so
   a chain `x→y→z` advances one hop per tick. Sources are any State Table entry
   (`vsig`/`cvar`); destinations are `cvar`s driven via `Backend::write_cvar` (the
   DWARF path is the id's `name` segment — no separate mapping). Per-route
   `suspend`/`resume` gates driving for fault injection (pairs with the table's
   `override`). A `vsig` destination (model input) needs a `Model::write` seam and
   is rejected at `add` for now. 8 unit tests (add/propagate/suspend/resume/
   snapshot-consistency + a RampModel→cvar route); sanity-suite check 6 routes a
   model's `vsig` into a firmware `cvar` and proves suspend/resume gating.
3. **Sim clock / step loop** — the engine loop: `advance models → propagate
   routes → sil_fw_advance_tick → record`. (`StateTable::set_time` + `record`
   are ready.)
4. **Formalize the trait seams** — `Backend` and `Model` **DONE (2026-07-04):**
   `voyant::Backend` (lifecycle `start`/`advance_tick`/`shutdown` + `cvar`
   read/write) is extracted, with `Firmware` as its first impl; `Model` above.
   `Transport` / `Scenario` remain (later chunks).
5. **D8 interrupt controller** — the C→Rust upcall registration + port dispatch
   shim; needed before the fast control ISR / comms fire.
6. **Comms** — the `Transport` trait + sim-HW upcall capture (logical payloads),
   routed to peer models (external transport / desktop app later, D5).
7. **Run modes** (fast/realtime pacing) + **Python bindings** (pytest, D3) +
   **dashboard** (D4). **Perf seams** from `performance.md` (zero-alloc hot loop,
   gated discrete work, dirty-page historian scan) — bake in as the loop grows.

## Open threads / pending decisions

- **Trait seams — `Backend` + `Model` done; `Transport` + `Scenario` remain.**
  `voyant::Backend` (with `Firmware` as first impl) and `voyant::Model` (with
  `RampModel` reference impl) are formalized (2026-07-04). The generic-framework
  vision (`architecture.md` §7) still wants `Transport` (comms) and `Scenario`
  (wiring), which land with their chunks (#6 / run-config).
- **Coercion follow-ups (low priority, accepted):** `i64` firmware fields narrow
  to `Value::I32` (rare; user OK with it); unknown enumerators read as `<n>`.
- **Deferred cleanups live in `backlog.md`** — notably: remove the
  `HW_<Module>_sim.h` inject/inspect layer (redundant once SIL white-box
  injection is feature-complete) and rework the unit tests that lean on it.

## Design docs (source of truth)

In `docs/sil/`: `README.md` (index) · `architecture.md` (the picture + decisions
table) · `roadmap.md` (phase-by-phase status) · `state-route-tables.md`
(State/Route Table + naming + comms) · `signal-trace.md` (D12 historian) ·
`freertos-tick.md` (D1 fiber port) · `ffi-boundary.md` (D2) ·
`sim-interrupts.md` (D8) · `performance.md` (perf levers) · `inverter-timestep.md`
(D6) · `time-virtualization.md` (D9) · `determinism.md` (D7).
