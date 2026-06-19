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
- ☐ Close out remaining open decisions D3–D7

## Phase 1 — Firmware runs on the SIM target
**Goal:** FreeRTOS + io/dev/app build and run natively; only the
hardware-specific bottom layer is swapped.
**Exit:** a native binary boots FreeRTOS, spawns the real tasks, and the
synthetic ADC ramp advances under the scheduler.

- ☐ **D1 spike:** deterministic, framework-driven FreeRTOS tick on the host
  (two-task toy program) — highest-risk item, do first. Spec + pass criterion
  in [`freertos-tick.md`](freertos-tick.md) §6
- ☐ Retrofit the host ports with the pluggable tick source (MSVC-MingW +
  POSIX) per the resolved D1 design
- ☐ Ungate FreeRTOS bring-up + io/dev/app in `main.c` for `BUILD_TARGET_SIM`
- ☐ Sim impls of the remaining bottom-layer drivers (USB CDC stub, any
  direct-hardware pokes); IO_AS5048 / IO_SK6805 build for SIM over `HW_SPI`
- ☐ Native build is green (`tools/build_native.sh sw/fw`)

## Phase 2 — Rust sim core + FFI backend
**Goal:** the `NativeFreeRtos` `FwBackend` drives the firmware; sim clock,
scheduler, signal log working.
**Exit:** Rust steps the firmware in sim time, reads the ADC ramp by symbol,
writes a global and sees the firmware react. (proof of white-box loop)

- ☐ Cargo workspace `sw/sil/` (`sil-sys` raw FFI+DWARF, `sil-core` engine)
- ☐ Native **SHARED** firmware target + `sil_*` ABI (start/advance/shutdown)
- ☐ `FwBackend` trait + `NativeFreeRtos` impl (dlopen the fw lib, per D2)
- ☐ DWARF symbol map (`object`+`gimli`) + ASLR-slide read/write of globals
- ☐ **State Table**: firmware entries (auto from DWARF) + model-state entries
- ☐ **Route Table**: `source → destination` per-tick transport + phase
  inference around the firmware step
- ☐ Sim clock + step loop + signal ring buffers

## Phase 3 — Plant models + closed loop
**Goal:** motor + encoder + sensor models close the loop with firmware.
**Exit:** firmware commands PWM → motor model spins → encoder/ADC feed back →
a basic control action is observable end-to-end.

- ☐ Motor model (electrical + mechanical)
- ☐ Encoder (AS5048) model → routed into fw SPI-rx state
- ☐ Current / voltage / temp sensor models → routed into fw ADC counts
- ☐ Inverter command (fw PWM state) → routed into motor model (settle D6)

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
