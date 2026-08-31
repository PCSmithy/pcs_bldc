# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

## Project Overview

BLDC motor controller — a learning project for modeling and control of
brushless DC motors. The hardware is a USB-PD-powered, single-axis BLDC
controller built around an STM32G431, an STSPIN32G4 integrated gate driver,
and an AS5048 magnetic rotor encoder, with on-device user controls (knob +
mode button + RGB status LEDs) for standalone operation. Hardware is designed
in **KiCad 10**, currently frozen pending firmware work.

Both software components exist and run on the bench (README's Status
section carries the feature inventory). The **firmware**: the spec'd +
traced driver stack, first motor drive (six-step trapezoidal; V/f
specced but not yet implemented), USB-PD sink monitoring, overcurrent +
encoder-fault protection, and a protobuf-over-USB-CDC protocol
(telemetry, signal trace engine, log stream, firmware identity). The
**desktop app** (`sw/gui`, Tauri 2): the observability slice,
spec-driven, verified by the playwright suite
(`sw/gui/tests/test_views.py`). An SIL harness (`sw/sil`) exercises the
native firmware build. Build system is fully stood up (CMake + Ninja,
dual-target embedded + native for SIL — see **Build System** below);
`.github/workflows/` carries `ci.yml` (firmware + SIL, 3 OS) and
`app.yml` (desktop app, Windows + macOS).

- **Firmware** — C/C++ on FreeRTOS, on the STM32G431. Owns the device
  experience — control loops, estimator, mode FSM, on-device UI, persistence.
- **Desktop app** — Rust GUI for Windows + macOS. Optional augmentation
  for live observability, configuration, diagnostics, and scripted/preset
  operation.

The device is **standalone-first**; the desktop app is auxiliary. They
communicate over USB CDC (virtual COM port). For full system context see
[`specs/system/overview.md`](specs/system/overview.md).

The project uses **spec-driven development with end-to-end traceability** via
[OpenFastTrace](https://github.com/itsallcode/openfasttrace). See the
**Spec System** section below.

## Repository Layout

```
.
├── README.md             Project goals, status, repo map
├── CLAUDE.md             (this file) AI-assistant working notes
├── setup.sh              First-time / idempotent project setup
├── requirements.txt      Python venv dependencies
│
├── hw/                   KiCad 10 hardware design (schematic + PCB)
├── datasheets/           Reference PDFs for major ICs
│
├── sw/                   Software trees + build infrastructure
│   ├── cmake/
│   │   └── toolchains/   native.cmake + arm-none-eabi.cmake
│   ├── lib/
│   │   ├── c/            C/C++ libraries (vendor + ours)
│   │   │   ├── CMSIS/                ARM Cortex-M std headers (vendor)
│   │   │   ├── STM32G4xx_HAL_Driver/ ST HAL driver (vendor, embedded only)
│   │   │   ├── Unity/                Test framework (vendor, native only)
│   │   │   ├── FreeRTOS/  nanopb/  tinyusb/   (vendor: kernel, protobuf, USB)
│   │   │   └── shared/               OUR library code, layered app/dev/io/hw/lib
│   │   │       ├── lib/build/        BUILD_TARGET_* constants
│   │   │       ├── lib/types/        lib_types.h (uint8_t, bool, size_t, ...)
│   │   │       ├── lib/utils/        lib_utils.h (COUNTOF, ...)
│   │   │       ├── lib/ringbuf/      Demo library + Unity tests
│   │   │       ├── hw/               Channelized HW modules:
│   │   │       │   ├── systemClock/  systemClock_init (single-instance)
│   │   │       │   ├── GPIO/  ADC/  DMA/  SPI/  I2C/  TIM/  USB/  OPAMP/  (hw-layer drivers)
│   │   │       │   └── stm32g4/      Family glue (system_, syscalls, sysmem)
│   │   │       ├── io/               IO-layer drivers (AS5048, SK6805, serial,
│   │   │       │                     bridge, i2c, COBSFrame, ...)
│   │   │       ├── dev/              Device drivers (switch, gateDriver, CYPD3177)
│   │   │       └── app/              App-layer modules (rgbLedRing, motorControl,
│   │   │                             server)
│   │   └── rust/         Rust libraries, workspace-less path deps:
│   │       ├── prng/         Deterministic PRNG (SIL models)
│   │       ├── dwarf_map/    DWARF reader: variable path → address/size/type
│   │       ├── pcs_wire/     Protocol wire codec (COBS+CRC frames, deframers)
│   │       └── pcs_proto/    prost schema bindings (protox — no protoc needed)
│   ├── proto/            board.proto — board-specific protobuf schema
│   ├── gui/              Desktop operator app (Tauri 2: Rust core `src-tauri/`
│   │                     + static webview frontend `dist/`, no build step;
│   │                     playwright suite in `tests/`; own workspace)
│   ├── sil/              SIL harness workspace (voyant + pcs_bldc_sil)
│   └── fw/               Firmware project (pcs_bldc-specific integration)
│       ├── stm32cube/g4/ STM32CubeMX-generated reference (not built directly)
│       └── src/
│           ├── main.c    Entry point: HW/IO init + FreeRTOS tasks + Error_Handler
│           ├── hw/       pcs_bldc-board channel configs + STM32G4 board glue
│           │   ├── stm32g4/      hal_msp.c, it.c, startup, linker script, hal_conf.h
│           │   ├── sim/          STATIC fw_hw placeholder for native build
│           │   ├── systemClock/  HW_systemClock_config.{h,c}
│           │   └── GPIO/ ADC/ DMA/ SPI/ I2C/ TIM/ OPAMP/  HW_<Module>_channels.{h,c}
│           ├── io/       pcs_bldc IO-layer configs (AS5048, SK6805, serial,
│           │             bridge, i2c, COBSFrame)
│           ├── dev/      pcs_bldc device configs (switch, gateDriver, CYPD3177)
│           └── app/      pcs_bldc app-layer configs (rgbLedRing, motorControl,
│                         server, userControls)
│
├── specs/                OFT spec tree (sys / fw / app requirements)
│   ├── README.md          Top-level MOC
│   ├── system/            sys~ requirements (system-level)
│   │   ├── overview.md    System overview + arch/persist anchor specs
│   │   └── conn/ mc/ obs/ ops/ pd/ safety/   (sys~ topic folders)
│   ├── firmware/          fw~ requirements — hal/io/est/obs/ui/conn/mc/pd/safety
│   └── desktop-app/       app~ requirements — arch/conn/obs/views
│
├── docs/                 Project documentation
│   ├── setup.md           Full first-time-setup guide (Win + macOS)
│   ├── spec-system.md     Spec convention + OFT integration (source of truth)
│   ├── spec-style.md      Spec wording rules (companion to spec-system.md)
│   ├── spec-template.md   Worked spec examples (fw~, app~, sys~, code tags)
│   ├── backlog.md         Firmware/tooling backlog
│   ├── motor-sprint.md    Motor-control sprint plan (FOC/estimation ahead)
│   └── c-coding-conventions.md  C code style: naming, MISRA-flavored patterns
│
└── tools/                Project tooling
    ├── oft/              OpenFastTrace JAR (4.2.2) + wrapper scripts
    ├── next-spec-id.py   Allocate next spec ID for a (type, topic) tuple
    ├── validate-specs.py Validate all spec IDs against the convention
    ├── spec_convention.py Shared helper (parses the canonical topic table)
    ├── build_native.sh   Configure + build + ctest a CMake project natively
    ├── build_arm.sh      Cross-compile a CMake project for STM32G431
    ├── run_sil.sh        Build the native firmware lib + run the SIL suite
    ├── generate_proto.sh Regenerate the nanopb protocol bindings manually
    ├── pcs_client.py     Reference host client (protocol decode over serial)
    ├── convert_cubemx_to_canonical.sh
    │                     Copy CubeMX-generated code from sw/fw/stm32cube/g4/
    │                     into the canonical layout (vendor packages at the
    │                     top of sw/lib/c/, board-specific files at
    │                     sw/fw/src/hw/stm32g4/)
    └── …                 plus bench/analysis scripts (serial capture, MF4,
                          trace_analysis/ notebooks)

(Build outputs: build/<target>-<source-basename>/, gitignored.
notebooks/ — created when that work begins.)
```

The schematic hierarchy under `hw/` is:

```
hw/bldc.kicad_sch (root - page 1)
├── usb-pd.kicad_sch         (sheet "input-power" - page 2: CYPD3177 USB-PD, LMR50410 buck, power sensing)
├── micro.kicad_sch          (sheet "micro" - page 3: STM32G431, SWD debug, encoder SPI)
├── power-stage.kicad_sch    (sheet "motor-phases" - page 4: STSPIN32G4 gate driver, 3-phase bridge)
│   ├── half_bridge.kicad_sch (sheet "half_bridge_U" - page 5)
│   ├── half_bridge.kicad_sch (sheet "half_bridge_V" - page 6)
│   └── half_bridge.kicad_sch (sheet "half_bridge_W" - page 7)
└── rgb_LEDs.kicad_sch       (sheet "rgb_LEDs" - page 8)
```

Note: `half_bridge.kicad_sch` is a single file instantiated 3 times (U/V/W
phases). `datasheets/` contains reference PDFs for all major ICs (STM32G431,
STSPIN32G4, CYPD3177, LMR50410, AS5048).

## Setup

Run `./setup.sh` from the repo root. It checks prereqs (Java 17+ for OFT,
Python 3.10+ for the venv, KiCad 10 for hardware, ARM GCC + CMake + Ninja
for firmware) and installs project-local artifacts (the OpenFastTrace JAR,
the Python venv). Idempotent and safe to re-run. Supported platforms:
Windows + macOS (no Linux). Full guide in [`docs/setup.md`](docs/setup.md).

Java and Python are blocking. KiCad, ARM GCC, CMake, and Ninja are
warn-only — each is required only for a specific workflow (hardware
design / embedded builds / C/C++ builds / build scripts respectively).

## Spec System

The project uses spec-driven development with full traceability. Every
`fw~` (firmware) or `app~` (desktop app) software requirement traces both
upward (to a `sys~` system requirement, ultimately a project-goal section
in `README.md`) and downward (to source-code `// [impl->...]` and
`// [test->...]` tags). [OpenFastTrace](https://github.com/itsallcode/openfasttrace)
enforces the trace.

**Single source of truth:** [`docs/spec-system.md`](docs/spec-system.md).
**Read this first for any spec-related task.** Artifact types, ID convention,
file organization, OFT integration, and anti-bloat rules all live there.

**Worked examples:** [`docs/spec-template.md`](docs/spec-template.md).

### ID convention (project policy)

`<type>~<topic>[_<subtopic>]_<NNN>~1` where:

- `type` ∈ `{sys, fw, app}`.
- `topic` is from the canonical table in `docs/spec-system.md` (current
  topics: `arch`, `hal`, `io`, `mc`, `est`, `obs`, `ui`, `ops`, `safety`,
  `pd`, `persist`, `conn`, `views`).
- `subtopic` is an optional per-area number space under a topic. Used by
  `hal` (one per peripheral: `hal_spi`, `hal_adc`, `hal_gpio`, ...); most
  topics omit it.
- `NNN` is sequential within `(type, topic, subtopic)`, zero-padded to 3
  digits, never reused.
- Version is **always `~1`** — never bumped. Specs are edited in place.

The smoketest under `tools/oft/_smoketest/` is a documented exception and
uses descriptive names instead of the numeric convention.

### Helper scripts

```bash
tools/next-spec-id.py sys mc                  # next available, e.g. sys~mc_001~1
tools/next-spec-id.py --list sys arch         # all existing + next
tools/validate-specs.py                       # validate all specs (exit 0 / 1)
tools/oft/oft.sh trace specs/                 # run formal OFT traceability
tools/oft/oft.sh trace tools/oft/_smoketest/  # verify OFT install (5 specs)
```

Both `next-spec-id.py` and `validate-specs.py` parse the canonical topic
table from `docs/spec-system.md` at runtime, so adding a new row there is
sufficient to make the tooling aware of a new topic.

### Current spec state

- `specs/system/` — `sys~` anchors in `overview.md` (`sys~arch_001..005`,
  `sys~ops_001`, `sys~persist_001`) plus topic folders (`conn`, `mc`,
  `obs`, `ops`, `pd`, `safety`). 22 are intentionally uncovered
  (system-level tests deferred to the SIL / system-test phases).
- **Firmware specs** (`specs/firmware/`), all back-filled + traced to code:
  - `hal/` — `adc`, `spi`, `tim`, `gpio`, `dma`, `usb`, `opamp`, `i2c` (one
    file per peripheral, per-peripheral sub-topic IDs).
  - `est/encoder.md` (AS5048), `obs/rgb_leds.md` (SK6805) +
    `obs/rgb_led_ring.md` (app_rgbLedRing), `ui/switch.md` (dev_switch),
    `conn/serial.md` (IO_serial over USB CDC) + the `conn/` trace-engine
    set (`fw~conn_trace_*`), `io/i2c.md` (IO_i2c) +
    `io/bridge.md` (IO_bridge three-phase actuation + current sense),
    `pd/cypd3177.md` (lib_CYPD3177 + dev_CYPD3177 USB-PD sink monitoring),
    `mc/gate-driver.md` (dev_gateDriver), `mc/motor-control-application.md`
    + `mc/six-step.md` + `mc/vf-sinusoidal.md` (app_motorControl),
    `safety/overcurrent.md` + `safety/encoder-fault.md`.
- **Desktop-app specs** (`specs/desktop-app/`) — `app~` specs across
  `arch`/`conn`/`obs`/`views` (core ownership, session + wire codec,
  signal picker + identity gate + trace client, plots with decimation +
  render budget, cursor + comparison anchor/deltas, table + value
  rendering, timeline, watch panel, workspace + widget titles). All
  impl-covered; test-covered except `app~arch_001` (its UI-reload test
  needs the live Tauri core). `[impl->]`/`[test->]` tags live in `.js`
  and `.py` files too, and the UI verification surface is the playwright
  suite `sw/gui/tests/test_views.py` (over the devmock).
- 173 spec defs across 68 files; `tools/validate-specs.py` clean. Trace
  with `tools/oft/oft.sh trace specs/ sw/ README.md` (code tags are not
  scanned without the source dirs). The intentional defect baseline is
  **30**: the 22 `sys~` anchors; 7 reserved `fw~` specs —
  `fw~hal_adc_003`/`fw~hal_adc_008` (timer-triggered injected + async
  completion) + `fw~hal_tim_006` (TRGO) for the interrupt-driven-control
  sprint, `fw~mc_007` (gesture map) + `fw~mc_010` (V/f) future app
  methods, `fw~hal_tim_005`/`_007` (dead-time, break input — sim modeling
  pending); and `app~arch_001` (implemented; its test needs the live
  Tauri core). Anything else = investigate. Both `[test->]` and
  `[impl->]` tags live in `.rs` files too (the SIL tests carry spec tags).

### Decisions explicitly deferred (will become specs when made)

- NVRAM implementation — littlefs vs emulated EEPROM middleware. Still an
  open implementation choice; see the Persistence section of
  `specs/system/overview.md`.

## Build System (firmware)

Firmware (and any C/C++ library code) uses **CMake + Ninja** with two
target profiles selected via toolchain file:

- **Native** (host gcc, MinGW on Windows / system gcc on macOS) — for
  SIL builds and unit tests.
- **Embedded** (`arm-none-eabi-gcc`) — cross-compiled for STM32G431
  (Cortex-M4F: `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
  -mthumb`).

Both compilers are GCC family at the same major.minor (15.2.x), so
warnings, language semantics, and sanitizer behavior are aligned across
the two targets. Build dirs are under `build/` (gitignored).

### Toolchain files

- `sw/cmake/toolchains/native.cmake` — sets `PCS_TARGET=native`, finds
  host `gcc`/`g++`, applies dev compile flags.
- `sw/cmake/toolchains/arm-none-eabi.cmake` — sets `PCS_TARGET=embedded`,
  finds `arm-none-eabi-gcc` on PATH or in standard install locations,
  applies Cortex-M4F MCU flags.

### Build invocations

Use the wrapper scripts (they handle the absolute-path requirement that
CMake imposes for toolchain files):

```bash
tools/build_native.sh                # native firmware build (default: sw/fw) + ctest
tools/build_native.sh --clean        # wipe build dir first
tools/build_arm.sh                   # cross-compile firmware (.elf/.bin/.hex)
tools/build_native.sh sw/lib/c       # standalone lib-only native build (cross-cutting libs + tests)
tools/run_sil.sh                     # SIL: release firmware DLL + Rust workspace tests + perf report
```

Default source dir for both scripts is `sw/fw` (the firmware project,
which pulls in `sw/lib/c` transitively). Standalone embedded builds of
`sw/lib/c` aren't supported — the HAL needs project-provided
`stm32g4xx_hal_conf.h` that only exists at `sw/fw` level. Standalone
native `sw/lib/c` skips the channelized HW modules (they need
project-provided `pcs_<module>_channels` libs) and just builds the
cross-cutting libs and their unit tests; gating is via `PCS_BUILDING_FW`.

Build outputs land in `build/native-<basename>/` and `build/arm-<basename>/`.
For `tools/build_arm.sh`, post-build hooks emit `pcs_bldc_fw.bin`,
`pcs_bldc_fw.hex`, and a `--print-memory-usage` size report alongside
`pcs_bldc_fw.elf`.

Firmware builds **require the project venv** (`./setup.sh`): the protocol
bindings `sw/fw/src/lib/protobuf/generated/{shared,trace,board}.pb.{h,c}`
are generated at build time by the venv's nanopb generator and are
gitignored, never committed. The schema is split: the reusable framework
schema (`sw/lib/c/shared/proto/` — `shared.proto` envelope + generic
services, `trace.proto` trace messages) imports this board's
`sw/proto/board.proto` through two fixed-name
extension payloads (`board.Request`, `board.Telemetry`). CMake fails the
configure with a pointer to `setup.sh` if `.venv` is missing;
`tools/generate_proto.sh` is the standalone manual regeneration path.
Encode/decode goes through the generic `lib_protobuf` module, and the
generic `app_server` core (`sw/lib/c/shared/app/server/`) serves the
protocol with board specifics supplied only through its config's
`handleRequest`/`buildTelemetry` hooks (`sw/fw/src/app/server/`).

### Code organization

Two layered trees that mirror each other in folder names:

- **`sw/lib/c/shared/`** — channelized library implementations (the
  "platform"). Reusable, generic, channel-config-driven. Builds for both
  targets. Sits alongside the vendor packages (`CMSIS/`,
  `STM32G4xx_HAL_Driver/`, `Unity/`) at the `sw/lib/c/` level so the
  vendor-vs-ours distinction is visually obvious.
- **`sw/fw/src/`** — pcs_bldc-specific integration: channel configs,
  board glue, entry point at `sw/fw/src/main.c`.

Layered architecture used in both: **`app → dev → io → hw`**, with `lib/`
(cross-cutting libraries) available to all layers. Strict downward
dependency by convention; same-layer or one-layer-down calls are fine;
calling up or skipping layers is discouraged but not forbidden
(judgment-call exceptions allowed).

`main.c` orchestrates startup directly:

```c
bool initSuccess = true;
initSuccess &= HW_systemClock_init(&HW_systemClock_config);
initSuccess &= HW_ADC_init(&HW_ADC_config);
// ...
if (!initSuccess) Error_Handler();
```

All `HW_*_init` / `IO_*_init` etc. functions return `bool`; `main.c` is
the single place that calls `Error_Handler` (which is itself defined in
`main.c`, always part of the executable's link). Library code never
calls `Error_Handler` directly. See
`memory/feedback_init_returns_bool.md`.

### Channelization pattern (canonical idiom)

Every HW-layer (and most IO-layer) module is split into two halves:

**Library side — `sw/lib/c/shared/hw/<Module>/`**, with one subdir per
target (`stm32g4/`, `sim/`, ...):
- `HW_<Module>.h` at the module root is the one header consumers
  include: shared types, `HW_<Module>_config_S`, and every API
  declaration. It `#include`s `HW_<Module>_<channels|config>.h` (the
  consumer-extension seam) and `HW_<Module>_target.h`.
- `<target>/HW_<Module>_target.h` holds only what genuinely differs per
  target: `HW_<Module>_channelConfig_S` (or `_config_S`) and any
  target-only declaration. Never included directly.
- `<target>/HW_<Module>.c` implements the API against the target.
- All target subdirs define a library named `hw_<Module>` (lowercase
  `hw_`, original module-case name) exposing both `.` and `..` as
  include dirs; `<Module>/CMakeLists.txt` does the conditional
  `add_subdirectory(stm32g4|sim)`.

**Project side — `sw/fw/src/hw/<Module>/`**:
- `HW_<Module>_channels.h` (multi-instance, e.g. ADC) or
  `HW_<Module>_config.h` (single-instance, e.g. systemClock) — the
  extension header. Holds the channel enum or project-level macros.
- `HW_<Module>_channels.c` (or `_config.c`) — defines the const
  `HW_<Module>_channelConfig[]` array (or single config struct) using
  `#if BUILD_TARGET == BUILD_TARGET_STM32G4 / BUILD_TARGET_SIM` to
  populate target-specific struct shapes from one source file.
- `CMakeLists.txt` defines `pcs_<Module>_channels` (or `_config`)
  INTERFACE library that just exposes its dir as an include path, then
  attaches the .c via `target_sources(fw_hw PRIVATE ...)` and pulls in
  `target_link_libraries(fw_hw PUBLIC hw_<Module> lib_build lib_utils)`.

The `_channels` vs `_config` naming distinguishes multi-instance from
single-instance modules. See [`docs/c-coding-conventions.md`](docs/c-coding-conventions.md).

**Canonical examples** (copy from these for new modules):
- Single-instance: `sw/lib/c/shared/hw/systemClock/` +
  `sw/fw/src/hw/systemClock/`
- Multi-channel: `sw/lib/c/shared/hw/ADC/` + `sw/fw/src/hw/ADC/`

### Cross-cutting libs

Three foundational header-only/INTERFACE libs in `sw/lib/c/shared/lib/`
that almost everything depends on:

- **`lib_build`** — `BUILD_TARGET_STM32G4` / `BUILD_TARGET_SIM`
  constants used in `#if` branching. The active target is set via
  `add_compile_definitions(BUILD_TARGET=BUILD_TARGET_*)` in the
  toolchain file.
- **`lib_types`** — `lib_types.h` with `<stdint.h>`, `<stdbool.h>`,
  `<stddef.h>` and project-wide typedefs (e.g. `float32_t`).
- **`lib_utils`** — `lib_utils.h` with cross-cutting macros. Currently:
  `COUNTOF(arr)` (GCC-checked, errors at compile time on pointer args).

### HW layer & dual-target swap

The HW layer holds the dual-target swap. `fw_hw` is a STATIC library
defined on both target sides (the native side has a `_placeholder.c` to
satisfy CMake's "STATIC needs sources" rule); `pcs_bldc_fw` always links
against `fw_hw`, and the toolchain file picks which side gets added.

- `sw/lib/c/STM32G4xx_HAL_Driver/` — vendored ST HAL driver, top-level,
  built as `stm32g4_hal` (embedded only).
- `sw/lib/c/CMSIS/` — vendored ARM CMSIS headers, top-level, exposed
  via the `cmsis` interface library.
- `sw/lib/c/shared/hw/stm32g4/` — STM32G4-family support files
  (`system_stm32g4xx.c`, `syscalls.c`, `sysmem.c`), compiled into
  `hw_stm32g4` (embedded only).
- `sw/fw/src/hw/stm32g4/` — pcs_bldc-board-specific HAL glue: HAL MSP,
  IT, linker script, startup file. No `board.c` — orchestration lives
  in `main.c`.
- `sw/fw/src/hw/sim/` — `_placeholder.c` to keep `fw_hw` STATIC on
  native; will grow real sim infrastructure (motor model, etc.) over time.

### CubeMX-generated code

Auto-generated content from STM32CubeMX is placed flat (no `cubemx/`
subdir) into the canonical locations:

- `sw/lib/c/CMSIS/` — ARM Cortex-M standard headers (Cortex-M Core +
  ST's STM32G4 device headers). Top-level vendored package alongside
  Unity, exposed as a `cmsis` INTERFACE library.
- `sw/lib/c/STM32G4xx_HAL_Driver/` — ST's STM32G4 HAL driver. Top-level
  vendored package, exposed as a `stm32g4_hal` STATIC library.
- `sw/lib/c/shared/hw/stm32g4/` — `system_stm32g4xx.c`, `syscalls.c`,
  `sysmem.c` (STM32G4-family support glue). This layer is also where our
  future channelized SPI/UART/ADC/etc. wrappers around the HAL will live.
- `sw/fw/src/hw/stm32g4/` — `main.h`, `stm32g4xx_hal_conf.h`,
  `stm32g4xx_it.c`/`.h`, `stm32g4xx_hal_msp.c`, `startup_stm32g431vbtx.s`,
  `STM32G431VBTX_FLASH.ld` (board-specific).

The CubeMX-generated `main.c` is intentionally **not** copied — its
untouched original lives in `sw/fw/stm32cube/g4/Core/Src/main.c`. Read it
there when you need to see what CubeMX generated for `SystemClock_Config`
/ peripheral inits.

`tools/convert_cubemx_to_canonical.sh` removes only the known fixed list
of generated filenames and re-copies them; hand-written files
(`CMakeLists.txt`, `HW_<Module>_channels.c`, `HW_<Module>_config.c`)
sit in the same directories and are never touched. Naming convention
tells the two apart at a glance: vendor files are `stm32g4xx_*`,
`startup_*`, `STM32G431*.ld`, `system_*`, `syscalls.c`, `sysmem.c`, or
live in the `CMSIS/` and `STM32G4xx_HAL_Driver/` directories.

Workflow when CubeMX needs to regenerate:

1. Edit `sw/fw/stm32cube/g4/pcs_bldc_g4.ioc` in STM32CubeMX, regenerate.
2. Run `tools/convert_cubemx_to_canonical.sh`.
3. If the clock tree or any peripheral channel config changed, diff the
   regenerated `sw/fw/stm32cube/g4/Core/Src/main.c` against the
   relevant `HW_<Module>_channels.c` / `HW_<Module>_config.c` and
   hand-merge the new init values.

### Conventions

C code style (naming, MISRA-flavored patterns, init contracts, file
naming) is the source-of-truth doc at
[`docs/c-coding-conventions.md`](docs/c-coding-conventions.md).
**Read it before writing or reviewing any C in this project.** It
covers:

- Function / variable / type / macro / enum naming (`_private_` infix,
  `_S`/`_E` type suffixes, etc.).
- Single return per function (MISRA Rule 15.5).
- Explicit parens on every operand of compound boolean logic.
- `const` on every local that isn't reassigned (MISRA Rule 8.13).
- `bool HW_<Module>_init(...)` contract; only `main.c` calls
  `Error_Handler`.
- `_channels` (multi-instance) vs `_config` (single-instance) module
  naming.

Build-system-specific conventions that don't fit the C-style doc:

- **CMake target naming:** `<layer>_<module>` (e.g. `lib_ringbuf`,
  `hw_systemClock`, `hw_ADC`, `app_mode_fsm`, `dev_kalman_observer`).
  The `<module>` part keeps original case (so `hw_ADC` not `hw_adc`).
- **Module file layout:** flat — `<module>.h` and `<module>.c` at the
  module root, no `include/` or `src/` subdirs. Tests live in a
  `test/` subdir.
- **BUILD_TARGET branching:** project channel-config files use
  `#if (BUILD_TARGET == BUILD_TARGET_STM32G4) ... #elif (BUILD_TARGET ==
  BUILD_TARGET_SIM) ... #endif` to define per-target struct contents from
  a single source file. Constants come from `lib_build/lib_build.h`.

### Test framework

[Unity](https://github.com/ThrowTheSwitch/Unity) v2.6.1, vendored at
`sw/lib/c/Unity/`. Tests are only built for the native target (gated by
`PCS_TARGET=native` in `sw/lib/c/CMakeLists.txt`). Each module has a
`test/` subdir with `CMakeLists.txt` + `test_<module>.c` using Unity's
`setUp` / `tearDown` / `RUN_TEST` / `UNITY_END` pattern. See
`sw/lib/c/shared/lib/ringbuf/test/test_ringbuf.c` for a working example.

### Third-party / vendored code

Lives at the top of `sw/lib/c/` (one directory per project), each with
its own hand-written `CMakeLists.txt` defining the consumable target:
`Unity/` (test framework), `CMSIS/` (ARM Cortex-M standard headers, target
`cmsis`), `STM32G4xx_HAL_Driver/` (ST HAL driver, target `stm32g4_hal`),
`FreeRTOS/` (kernel), `nanopb/` (protobuf runtime), `tinyusb/` (USB
device stack), `littlefs/` (when added). The conversion script's
surgical wipe pattern preserves these `CMakeLists.txt` files across
CubeMX regenerations.

A few small vendor-shipped support files (`system_stm32g4xx.c`,
`syscalls.c`, `sysmem.c`) live at `sw/lib/c/shared/hw/stm32g4/` rather
than at the top level, because they're conceptually "STM32G4 family
glue" and will sit alongside our future channelized peripheral wrappers
in that layer.

## PCB Design Rules

- Board size: ~177.8 x 101.6 mm, targeting JLCPCB fabrication.
- Min trace/spacing: 0.1524mm (6mil).
- Net classes: **Default** (0.2032mm track), **Power** (0.508mm track),
  **Signal** (0.2032mm track), **USB_diff** (0.267208mm track).
- Gerber output directory: `hw/gerber_to_order/`.

## KiCad CLI

KiCad 10 is installed at `C:/Program Files/KiCad/10.0/bin/kicad-cli.exe`
(path is version-numbered, so update if you upgrade). Common commands:

```bash
KICAD_CLI="/c/Program Files/KiCad/10.0/bin/kicad-cli.exe"

# Design rule / electrical rule checks
"$KICAD_CLI" pcb drc --output drc.json --format json --severity-all --exit-code-violations hw/bldc.kicad_pcb
"$KICAD_CLI" sch erc --output erc.json --format json --severity-all --exit-code-violations hw/bldc.kicad_sch

# Export gerbers (uses board's saved plot settings)
"$KICAD_CLI" pcb export gerbers --output hw/gerber_to_order/ --board-plot-params hw/bldc.kicad_pcb
"$KICAD_CLI" pcb export drill --output hw/gerber_to_order/ --format excellon hw/bldc.kicad_pcb

# Export BOM, schematic PDF, board render
"$KICAD_CLI" sch export bom --output hw/bldc_bom.csv --exclude-dnp hw/bldc.kicad_sch
"$KICAD_CLI" sch export pdf --output hw/bldc_schematic.pdf hw/bldc.kicad_sch
"$KICAD_CLI" pcb render --output hw/bldc_render.png --side top --quality basic hw/bldc.kicad_pcb
```

## KiCad File Format

All `.kicad_sch` and `.kicad_pcb` files are plain-text S-expressions and
can be read/parsed directly. The root schematic references sub-sheets via
`(sheet ...)` blocks containing `(property "Sheetfile" "filename.kicad_sch")`.
The project file `bldc.kicad_pro` is JSON.

## kicad-happy Skills

The [kicad-happy](https://github.com/aklofas/kicad-happy) skills are cloned
at `C:/code/kicad-happy/` and symlinked into this project at
`.claude/skills/`. All 8 skills are installed: kicad, bom, digikey, mouser,
lcsc, element14, jlcpcb, pcbway.

**At the start of each session**, read the SKILL.md files for any skills
relevant to the task at hand. The most commonly needed are:

- `C:/code/kicad-happy/skills/kicad/SKILL.md` — Schematic/PCB/Gerber
  analysis. Has Python scripts (`analyze_schematic.py`, `analyze_pcb.py`,
  `analyze_gerbers.py`) that parse S-expressions into structured JSON for
  design review. Read this for any design review, DRC/ERC, net tracing, or
  circuit analysis task.
- `C:/code/kicad-happy/skills/bom/SKILL.md` — BOM lifecycle (analyze,
  source, export). Scripts can edit schematic properties directly. Read
  this for BOM, sourcing, or ordering tasks.
- `C:/code/kicad-happy/skills/jlcpcb/SKILL.md` — JLCPCB design rules,
  BOM/CPL format, ordering. Read this before generating fabrication outputs.

The distributor skills (`digikey/`, `mouser/`, `lcsc/`, `element14/`) each
have a SKILL.md with API usage and datasheet fetching instructions — read
the relevant one when sourcing components.

If the symlinks are broken (e.g. kicad-happy was moved), recreate them:

```bash
for skill in kicad bom digikey mouser lcsc element14 jlcpcb pcbway; do
  ln -sf "/c/code/kicad-happy/skills/$skill" ".claude/skills/$skill"
done
```

A Python venv is set up at `.venv/` (created by `./setup.sh`) with
dependencies for the analysis scripts. Always use this venv when running
them:

```bash
.venv/Scripts/python <script>   # on Windows
.venv/bin/python <script>       # on macOS
```

Installed packages: see `requirements.txt`. After install, run
`.venv/Scripts/playwright install chromium` (Windows) or
`.venv/bin/playwright install chromium` (macOS) — Playwright drives the
app's UI test suite and is the browser fallback used by some datasheet
sites.

## Key Components

| Component   | Role                                                    | Datasheet in repo |
|-------------|---------------------------------------------------------|-------------------|
| STM32G431VB | MCU (Cortex-M4F @170MHz, 128 KB flash / 32 KB RAM)      | Yes               |
| STSPIN32G4  | 3-phase gate driver (integrated)                        | Yes               |
| CYPD3177    | USB-PD sink controller                                  | Yes               |
| LMR50410    | 5V buck converter                                       | Yes               |
| AS5048      | Magnetic position encoder (SPI)                         | Yes               |

## What to read first

For a new agent joining the project, read these in order:

1. This file (`CLAUDE.md`) — project overview, repo layout, current state.
2. [`README.md`](README.md) — full project goals, non-goals, status.
3. [`docs/spec-system.md`](docs/spec-system.md) — the spec convention,
   tooling, and rules. **Required for any spec-related work.**
4. [`specs/system/overview.md`](specs/system/overview.md) — the
   architectural picture: standalone-first device, two-component plan,
   USB CDC transport, operating modes, persistence, plurality.
5. [`docs/setup.md`](docs/setup.md) — only if setting up a fresh
   environment.

For firmware / build-system work specifically, the **Build System** section
of this file is the orientation; `tools/build_native.sh` and
`tools/build_arm.sh` are the entry points; the demo at
`sw/lib/c/shared/lib/ringbuf/` is the canonical example of the module/test layout
to copy.

For desktop-app work, `specs/desktop-app/README.md` is the spec
orientation, the frontend modules under `sw/gui/dist/js/` and the Rust
core under `sw/gui/src-tauri/src/` are the code, and the playwright
suite `sw/gui/tests/test_views.py` (run with the project venv; serves
`dist/` on localhost over the devmock) is the verification surface.

For writing or reviewing C code, [`docs/c-coding-conventions.md`](docs/c-coding-conventions.md)
is required reading — it's the source of truth for naming,
MISRA-flavored patterns, init contracts, and module-naming rules.
Canonical worked examples for those conventions are
`sw/lib/c/shared/hw/ADC/` (multi-channel) and
`sw/lib/c/shared/hw/systemClock/` (single-instance).

For git commit messages, do not include a "Co-Authored-By: ..." line

PLEASE CONFIRM YOU'VE READ THIS CLAUDE.md
