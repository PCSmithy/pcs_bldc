# SIL (Software-in-the-Loop) testing environment

This directory holds the planning + architecture docs for the pcs_bldc SIL
environment: a Rust framework that runs the **native cross-compiled C
firmware** against simulated plant models, with white-box inspection of
firmware state during execution.

Two run modes share one core engine:

- **Realtime mode** — sim time tracks wall-clock. Hosts a web dashboard
  (signal plotting, injection, live model tweaks) and presents a USB-CDC
  endpoint so the desktop app can't tell it apart from the real board.
- **Fast mode** — faster-than-realtime, fully scripted via Python/pytest
  bindings. Deterministic, parallelizable, for automated regression.

## Documents

| Doc | Purpose |
|-----|---------|
| [`architecture.md`](architecture.md) | The technical architecture: execution model, sim boundary, time/scheduler core, plant models, Rust↔C boundary, the two run modes, and the open decisions. **Read this first.** |
| [`roadmap.md`](roadmap.md) | Phased milestones + status tracking for building it out. |

## Worktree / branch workflow

This work lives on the **`sil`** branch, developed in a separate worktree so
`main` stays free for parallel work:

```
C:/code/pcs_bldc       main   (hardware + firmware)
C:/code/pcs_bldc-sil   sil    (this SIL effort)
```

- **Rebase cadence:** periodically `git fetch && git rebase main` from the
  `sil` worktree to pull in firmware/build changes. The SIL effort requires
  real firmware-side work — running FreeRTOS and the io/dev/app layers on
  the SIM target (today they're gated `BUILD_TARGET_STM32G4`-only), plus a
  sim ABI for the framework to drive. Keep these changes clean and
  upstream-friendly to reduce rebase friction; much of it (ungating layers,
  sim impls of low-level drivers) is genuinely useful on `main` regardless.
- **Merge back:** once the harness is proven, merge `sil` into `main`. The
  firmware-side changes (native FreeRTOS port wiring, ungating the upper
  layers for SIM, sim impls of the bottom-layer drivers, a sim ABI/entry
  point) are the parts that touch shared files; the Rust framework under
  `sw/sil/` is purely additive.

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
   the logic. (Today they're gated `BUILD_TARGET_STM32G4`-only; making them
   build + run on SIM is the bulk of the firmware-side work.)
5. White-box inspection is via the **native `.elf`/shared-lib symbol table
   (+ DWARF)** — read/write firmware globals by name during execution.

See [`architecture.md`](architecture.md) for the reasoning and the decisions
still open.
