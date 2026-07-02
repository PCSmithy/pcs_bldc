# Session handoff — pcs_bldc

Scratch orientation for the next session: what's done, what's in flight, and
logical next moves. For durable context (repo layout, conventions, spec system,
build system) read `CLAUDE.md` first — it's always loaded. Cross-session gotchas
live in memory (`MEMORY.md`) — read those too. The sprint plan with the
authoritative milestone list is `docs/foundation-sprint.md`.

This file is untracked by git on purpose (ephemeral). Refresh it when you wrap.

## Where we are

**Foundation sprint — Milestone 1 (driver spec back-fill) is COMPLETE.** Every
HAL/IO driver now has: OFT specs (`fw~...`) tracing up to a `sys~` anchor,
`[impl->]`/`[test->]` tags, a dual-target (`stm32g4` + `sim`) implementation,
and a native Unity/SIL suite. All committed and pushed to `origin/main` (latest
sprint commit: GPIO cached-input read).

| Driver | Spec file | IDs | Parent | Status |
|---|---|---|---|---|
| ADC | `specs/firmware/hal/adc.md` | `fw~hal_adc_001..008` | `sys~arch_005` | spec+impl+test; modes 003/008 OFT-uncovered (timer/IRQ/DMA unbuilt — motor sprint) |
| AS5048 encoder | `specs/firmware/est/encoder.md` | `fw~est_encoder_001..006` | `sys~mc_001` (new anchor) | spec+impl+test, dual-target |
| SK6805 RGB ring | `specs/firmware/obs/rgb_leds.md` | `fw~obs_led_001..005` | `sys~arch_001` | spec+impl+test; channelized, dual-target |
| USB | `specs/firmware/hal/usb.md` | `fw~hal_usb_001..004` | `sys~arch_005` | HW_USB (stm32g4 TinyUSB + sim loopback) |
| Serial | `specs/firmware/conn/serial.md` | `fw~conn_serial_001..005` | `sys~arch_003` | IO_serial over HW_USB; transport-dispatch switch |
| GPIO input cache | `specs/firmware/hal/gpio.md` | `fw~hal_gpio_004` (revised) | `sys~arch_005` | cached snapshot (run1ms/readCached); live readPin removed |

`tools/oft/oft.sh trace specs/ sw/` is clean of broken/dangling tags; uncovered
specs (ADC 003/008, the new `fw~obs_ring_001..003`) are expected — written ahead
of their impl. Native `ctest` is green (11 suites). ARM links clean (~89 KB flash).

**Milestone 2 (app_rgbLedRing) — DONE (specs + impl).**
`specs/firmware/obs/rgb_led_ring.md` holds `fw~obs_ring_001..003` (control loop +
mode FSM / per-mode rendering / colour persistence), parent `sys~arch_001`,
faithfully back-filling the throwaway `ledTask` demo (chosen: spec the demo
modes as-is, one flash not two, 3-spec minimal decomposition). The impl is
**channelized** (one instance per ring): shared lib
`sw/lib/c/shared/app/rgbLedRing/app_rgbLedRing.{c,h}` — a single TU holding the
pure render core (mode FSM, per-mode rendering, colour persistence) + the
FreeRTOS/IO shell (task over an array of rings, encoder/button reads, ring
writes, blink, cadence). The render-core functions are declared in the header so
the native Unity suite compiles the TU directly against a FreeRTOS + IO/dev
**mock harness** (`test/FreeRTOS.h`, `task.h`, `IO_*.h`, `dev_switch.h`,
`mock_app_deps.c`). All board-specific channel wiring lives in the project seam
`sw/fw/src/app/rgbLedRing/app_rgbLedRing_channels.{c,h}` (ledChannel / dial /
motor / button / pixelCount per ring) — no project channel names in lib code.
`main.c`'s `ledTask` + helpers are gone, replaced by
`app_rgbLedRing_init(&app_rgbLedRing_config, TASK_PRIO_LED)`. All identifiers are
`app_rgbLedRing_*` (lowercase). All 3 specs OFT-covered; native ctest 12 suites;
ARM ~91 KB. `vTaskSuspendAll` guard stays (DMA = M4). As a prerequisite,
**`dev_switch` is now dual-target** (ungated in both CMake roots, mirroring
`io_AS5048`; only the executable link stays embedded-only since the SIM `main()`
has no UI consumer) **with a native Unity suite** (`sw/lib/c/shared/dev/switch/
test/`, compiles `dev_switch.c` against a mock `HW_GPIO`). Those tests are
**untagged** — `dev_switch` has no `fw~` spec yet. `dev_switch_init` now resets
per-channel debounced state (re-init starts clean; also what makes it testable).

## The repeatable driver recipe (use for Milestones 2–5 + new drivers)

This sprint locked a workflow — follow it:

1. **Spec via the `pcs_spec` skill.** It drives: interview for intent → fan out
   two `Explore` agents (spec-tree cartographer + codebase investigator) →
   draft per `docs/spec-style.md` → **blind** style auditor subagent (only the
   draft + the two rule docs, no convo context) → on approval write + validate +
   trace. Spec the **ideal** final-state driver; record impl gaps, don't water
   the spec down.
2. **Impl cleanup** (after the spec): channelize if needed (`_channels` seam),
   make it **dual-target** (un-gate so it builds native — see the io/ gating
   below), add a **Unity suite** with a mocked/sim HW dependency, then add
   `[impl->]`/`[test->]` tags. Verify: `build_native.sh` (ctest) +
   `build_arm.sh` + `oft trace`.
3. Commit spec+impl together once green.

**Subtopic convention:** busy topics subdivide (`hal_adc`, `hal_usb`,
`est_encoder`, `obs_led`, `conn_serial`). Add the subtopic in the ID; no
topic-table row needed (only new top-level topics need a row).

**Dual-target gotcha:** the `io/`/`dev/` trees were once blanket-gated
embedded-only because `io/usb`→tinyusb. Per-module gating now lives in the
`io/CMakeLists.txt` files — a dual-target io module (only needs a dual-target HW
dep) builds for both; only `usb`/tinyusb-bound code stays embedded. Tests
compile the driver `.c` directly against test-local channel seams + a mock HW.

## What's next (foundation sprint, dependency order)

- **Milestone 2 — `app_rgbLedRing`**: DONE. Channelized module (shared render
  core + RTOS/IO shell at `sw/lib/c/shared/app/rgbLedRing/`; project channel seam
  at `sw/fw/src/app/rgbLedRing/`), wired via
  `app_rgbLedRing_init(&app_rgbLedRing_config, TASK_PRIO_LED)` in `main.c`;
  `fw~obs_ring_001..003` covered. The `app/` CMake layer is now established on
  both the shared and project sides (the canonical `_channels` seam pattern).
- **Milestone 3 — HW_DMA** (new `hw` module, topic `hal_dma`): specs → impl →
  dual-target + test. The real fix for two prototype hacks.
- **Milestone 4 — HW_DMA ↔ HW_SPI ↔ SK6805 integration**: DMA-backed SPI
  transfers; SK6805 uses them; **remove the `vTaskSuspendAll` LED stream guard**
  in `ledTask`. Amend the SPI + SK6805 specs. Depends on M3.
- **Milestone 5 — remaining ADC channels**: instantiate all board ADC inputs +
  CDC printout to verify. Depends on the ADC spec (done).

Then the **motor-control sprint** (FOC): this is where ADC timer-trigger +
injected + dual-ADC multimode (specs 003/006/007/008) and the encoder's role in
sensored control get built out under `sys~mc_001`.

## Current runtime / architecture state

`sw/fw/src/main.c` on the STM32G4 target: `HW_*_init` + `IO_*_init` chain, then
tasks under a centralized priority hierarchy (encoder > LED > USB):
- `task_1ms` (TASK_PRIO_ENCODER): `HW_GPIO_run1ms` (cache inputs) →
  `HW_ADC_run1ms` (sample) → `IO_AS5048_run1ms` (encoders) →
  `dev_switch_run1ms` (debounce off the cached GPIO snapshot).
- `led` (TASK_PRIO_LED): button-cycled LED modes; the ~1.25 ms blocking SK6805
  transmit is wrapped in `vTaskSuspendAll()` (the M4 DMA fix removes this).
- `telem` (TASK_PRIO_USB): Teleplot telemetry over CDC via `IO_serial`
  (encoder + ADC signals). Throwaway bring-up consumer — to become an obs module.
- USB device-service task: spawned inside `HW_USB_init`, owns `tud_task`.

USB layering after the cleanup: **HW_USB** (hal: TinyUSB stack + descriptors +
CDC byte API; sim = loopback) → **IO_serial** (io: channelized byte stream,
bounded backpressure, transport-dispatch switch so a future
`IO_SERIAL_TRANSPORT_UART` is a one-case-per-switch add). `__io_putchar` (in
`main.c`) retargets printf → `IO_serial`.

A `BUILD_TARGET_SIM` branch in `main.c` exists (FreeRTOS fiber port, a temporary
in-process scheduler loop) — early SIL bring-up; Phase 2 hands tick-driving to
the Rust framework via a control ABI.

## Known deferred items / gaps

- **ADC modes** (timer trigger, interrupt/DMA transfer): unbuilt; `run1ms`
  services only software-polled. `fw~hal_adc_003`/`008` OFT-uncovered. FOC needs
  these (injected, TIM1-triggered, dual-ADC) — motor sprint.
- **dev_switch is un-spec'd**: now dual-target with a native Unity suite, but
  has no `fw~` spec, so its tests are untagged. Back-fill a spec (pcs_spec) when
  convenient so it traces like the M1 drivers.
- **USB VID/PID** still placeholder (`0xCAFE/0x4001`) — assign real before
  anything public/field. CDC **read** path is built (`HW_USB_read` /
  `IO_serial_read`) but no consumer yet.
- **HW_DMA** absent — encoder SPI is blocking in the 1 ms task and the LED frame
  freezes all tasks ~1.25 ms via `vTaskSuspendAll`. M3/M4 fix.
- **SPI BUS_3 (SK6805)** is hand-written, not .ioc-sourced (CubeMX forces
  NSSPMode on for CPHA=0, corrupting the bit-banged stream). See the BUS_3 config
  comment.

## Build / flash / debug workflow

- Build: `tools/build_arm.sh` (ARM .elf/.bin/.hex) / `tools/build_native.sh`
  (native + ctest). `--clean` wipes the build dir.
- Spec tooling: `tools/validate-specs.py` (ID convention), `tools/oft/oft.sh
  trace specs/ sw/` (traceability). `tools/next-spec-id.py <type> <topic>
  [subtopic]` allocates the next ID.
- VSCode (`.vscode/`, gitignored — recreate from the **STM32 debug setup**
  memory): **F5** = build+flash+debug. Stop the debug session before flashing.
- CubeMX round-trip: edit `sw/fw/stm32cube/g4/pcs_bldc_g4.ioc`, Generate Code,
  then `tools/convert_cubemx_to_canonical.sh`. (Heads-up: the converter can drop
  a harmless `bash.exe.stackdump` in the repo root — delete it.)

## Memories worth reading (MEMORY.md)

- **STM32 ADC rank constants** — `ADC_REGULAR/INJECTED_RANK_n` are register-field
  encodings (RANK_1==6, INJ_RANK_1==9), NOT ordinals; map via a constants table.
- **Unhandled-IRQ wedge** — enabled-but-unhandled IRQ hangs in Default_Handler;
  `--whole-archive fw_hw`.
- **BOOT0 / option bytes** — burn `nSWBOOT0=0` once the USB-PD I2C bus is used.
- **STM32 debug setup** — working OpenOCD/GDB paths.
- **Rust toolchain** — gnu (not msvc); `export PATH="$HOME/.cargo/bin:$PATH"`.

## What to read first (next session)

1. `CLAUDE.md` + `MEMORY.md` (the gotcha memories).
2. This file, then `docs/foundation-sprint.md` (the milestone plan).
3. `docs/spec-system.md` + `docs/spec-style.md` for any spec work; the
   `pcs_spec` skill drives authoring.
4. A finished driver as the pattern to copy: `specs/firmware/est/encoder.md` +
   `sw/lib/c/shared/io/AS5048/` (+ its `test/`) for a channelized dual-target io
   driver; `sw/lib/c/shared/hw/USB/` for a hw module with a sim loopback.
