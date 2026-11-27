# SIL (Software-in-the-Loop) testing environment

> **Picking this up fresh?** [`handover.md`](handover.md) (code map, how to
> run), [`roadmap.md`](roadmap.md) and [`ffi-boundary.md`](ffi-boundary.md)
> are dated records; the design docs below and the code are current.

This directory holds the planning + architecture docs for the pcs_bldc SIL
environment: a Rust framework (**`voyant`**) that runs the **native
cross-compiled C firmware** against simulated plant models, with white-box
inspection of firmware state during execution.

Two run modes share one core engine:

- **Realtime mode** — sim time tracks wall-clock. Hosts a web dashboard
  (signal plotting, injection, live model tweaks) and presents a USB-CDC
  endpoint so the desktop app can't tell it apart from the real board.
- **Fast mode** — faster-than-realtime, fully scripted via Python/pytest
  bindings. Deterministic, parallelizable, for automated regression.

## Build & run

```bash
tools/run_sil.sh            # build native firmware (DLL) + SIL framework, run the sim
tools/run_sil.sh --debug    # -O0 firmware DLL + debug Rust
tools/run_sil.sh --clean    # wipe the native build dir first
```

Builds the firmware as a shared library (`build/native-fw-release/`, or
`build/native-fw/` with `--debug`), builds the `sw/sil/` Rust workspace
(`voyant`, the framework, and `pcs_bldc_sil`, this board's instantiation),
and runs its tests and the `pcs_bldc_sil_perf` report, which load the
firmware, drive it over the control ABI, and read its state by symbol.

## Documents

| Doc | Purpose |
|-----|---------|
| [`architecture.md`](architecture.md) | The technical architecture: execution model, sim boundary, time/scheduler core, plant models, Rust↔C boundary, the two run modes, and the open decisions. **Read this first.** |
| [`state-route-tables.md`](state-route-tables.md) | The two core framework data structures: the **State Table** (every firmware static + model states; traced entries are change-logged historians) and the **Route Table** (declarative per-tick transport). |
| [`signal-trace.md`](signal-trace.md) | D12 — the State Table as historian: change-logged, timestamped per-signal timeseries that Python tests evaluate against. |
| [`ffi-boundary.md`](ffi-boundary.md) | D2 — Rust↔C boundary: in-process, dynamically-loaded firmware, DWARF memory introspection, tiny control ABI + C→Rust upcalls. |
| [`freertos-tick.md`](freertos-tick.md) | D1 — single-threaded cooperative FreeRTOS **fiber** port + pluggable tick source (realtime vs framework-driven). |
| [`performance.md`](performance.md) | The performance architecture: cost model + levers (fibers, gated discrete work, better integrators, dirty-page historian, zero-alloc) + the seams baked in from the start. |
| [`sim-interrupts.md`](sim-interrupts.md) | D8 — framework-owned interrupt table (periodic + one-shot, config + runtime); dispatched through the port in the firmware fiber context. |
| [`inverter-timestep.md`](inverter-timestep.md) | D6 — inverter fidelity (averaged-duty default, switching-resolved opt-in), the abc model↔firmware contract, and base `dt`. |
| [`time-virtualization.md`](time-virtualization.md) | D9 — firmware time sources backed by the sim clock; every wait is a yield (busy-waits would deadlock). |
| [`determinism.md`](determinism.md) | D7 — tolerance-based float assertions (ε≈1e-6) over a reproducible engine; the discrete threshold-divergence caveat. |
| [`roadmap.md`](roadmap.md) | Phased milestones + status tracking for building it out. |

## Confirmed ground rules

1. Framework is **Rust**.
2. Runs the **native** firmware build (`BUILD_TARGET_SIM`), not an ARM
   emulator — for speed, determinism, and parallelism. An ARM-emulation
   backend stays a future option behind a clean execution-backend seam.
3. The sim plugs in at the existing **HAL/channel boundary**
   (`sw/lib/c/shared/hw/<Module>/sim/`). Only the genuinely
   hardware-specific bottom layer is swapped.
4. **FreeRTOS and the io/dev/app layers run on the SIM target too** — the
   real scheduler and the real task code execute in sim, not a re-host of
   the logic, through the control ABI in `sw/fw/src/sil_fw.h`.
5. White-box inspection is via the **native `.elf`/shared-lib symbol table
   (+ DWARF)** — read/write firmware globals by name during execution. **No
   sim-specific getter/setter functions in the firmware.**
6. The framework's two core data structures are the **State Table** (one
   namespace over every firmware static + all model states) and the **Route
   Table** (per-tick `source → destination` transport). See
   [`state-route-tables.md`](state-route-tables.md).
7. **Single-threaded cooperative fibers, performance-first.** The whole sim runs
   in one OS thread (tasks = fibers); the hot loop is engineered for many×
   realtime on a normal desktop. See [`performance.md`](performance.md).

See [`architecture.md`](architecture.md) for the reasoning and the decisions
still open.
