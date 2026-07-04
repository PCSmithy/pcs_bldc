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
`origin/sil`. Latest commit: enum-name resolution (`967d83a`).

## Build & run

```bash
tools/run_sil.sh            # build firmware DLL + voyant, run the demo
tools/run_sil.sh --clean    # wipe the native build dir first
```

The demo loads the firmware DLL, drives it over the control ABI, samples state
into the State Table, and shows the historian + enum reads + inject-and-react.

Rust unit tests: `cd sw/sil && cargo test -p voyant` (10 tests).

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
    src/backend.rs          Firmware: control ABI + cvar sample-resolver
                            (read_cvar/write_cvar; the only unsafe/DWARF part)
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

## What's next (prioritized)

1. **`Model` trait + `vsig` backing** — model-backed signals registered into the
   State Table (the first non-cvar backing). Foundation for plant models.
2. **Route Table** — `source → destination`, snapshot-then-write once per tick,
   with per-route **suspend/resume** (pairs with the table's `override`) for
   fault injection.
3. **Sim clock / step loop** — the engine loop: `advance models → propagate
   routes → sil_fw_advance_tick → record`. (`StateTable::set_time` + `record`
   are ready.)
4. **Formalize the trait seams** — `Backend` / `Model` / `Transport` /
   `Scenario` (today `Firmware` is concrete; make it the `Backend` impl).
5. **D8 interrupt controller** — the C→Rust upcall registration + port dispatch
   shim; needed before the fast control ISR / comms fire.
6. **Comms** — the `Transport` trait + sim-HW upcall capture (logical payloads),
   routed to peer models (external transport / desktop app later, D5).
7. **Run modes** (fast/realtime pacing) + **Python bindings** (pytest, D3) +
   **dashboard** (D4). **Perf seams** from `performance.md` (zero-alloc hot loop,
   gated discrete work, dirty-page historian scan) — bake in as the loop grows.

## Open threads / pending decisions

- **Rebase `sil` onto fresh `main`.** `sil` is based on a June `main`; `main` has
  since landed the *complete firmware foundation* (full hw/io/dev/app driver
  stack — see `CLAUDE.md`). The Rust engine is firmware-agnostic so it doesn't
  need this, but **Phase 3 (plant models closing the loop with real firmware
  control code) will** — and there's no motor/control app code yet anyway. Plan:
  rebase before Phase 3, expecting conflicts in `main.c` (the SIM path) and the
  HW drivers. Not urgent for engine work (#1–#6 above).
- **Trait seams not yet formalized.** `Firmware` is a concrete struct; the
  generic-framework vision (D-doc `architecture.md` §7) wants `Backend`/`Model`/
  `Transport`/`Scenario` traits. Do this alongside #1/#4.
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
