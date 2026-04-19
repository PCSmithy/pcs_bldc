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

Two software components are planned. Neither has real application code yet,
but the firmware build system is fully stood up (CMake + Ninja, dual-target
embedded + native for SIL — see **Build System** below).

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
│   │   ├── c/            C/C++ libraries — channelized; layered app/dev/io/hw/lib
│   │   │   ├── Unity/    Vendored Unity v2.6.1 (test framework)
│   │   │   └── lib/ringbuf/   First demo library + Unity tests
│   │   └── rust/         Rust libraries (future)
│   └── fw/               Firmware project (pcs_bldc-specific integration)
│       ├── stm32cube/g4/ STM32CubeMX-generated reference (not built directly)
│       └── src/          Layered: app/ dev/ io/ hw/{sim,stm32g4} lib/
│
├── specs/                OFT spec tree (sys / fw / app requirements)
│   ├── README.md          Top-level MOC
│   ├── system/            sys~ requirements (system-level)
│   │   └── overview.md    System overview + 6 architectural anchor specs
│   ├── firmware/          fw~ requirements (STM32G4 firmware) — empty so far
│   └── desktop-app/       app~ requirements (Rust GUI) — empty so far
│
├── docs/                 Project documentation
│   ├── setup.md           Full first-time-setup guide (Win + macOS)
│   ├── spec-system.md     Spec convention + OFT integration (source of truth)
│   └── spec-template.md   Worked spec examples (fw~, app~, sys~, code tags)
│
└── tools/                Project tooling
    ├── oft/              OpenFastTrace JAR (4.2.2) + wrapper scripts
    ├── next-spec-id.py   Allocate next spec ID for a (type, topic) tuple
    ├── validate-specs.py Validate all spec IDs against the convention
    ├── spec_convention.py Shared helper (parses the canonical topic table)
    ├── build_native.sh   Configure + build + ctest a CMake project natively
    └── build_arm.sh      Cross-compile a CMake project for STM32G431

(Build outputs: build/<target>-<source-basename>/, gitignored. Real fw
source code, sim/, notebooks/ — created when that work begins.)
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

`<type>~<topic>_<NNN>~1` where:

- `type` ∈ `{sys, fw, app}`.
- `topic` is from the canonical table in `docs/spec-system.md` (current
  topics: `arch`, `mc`, `est`, `obs`, `ops`, `safety`, `pd`, `persist`,
  `conn`, `views`).
- `NNN` is sequential within `(type, topic)`, zero-padded to 3 digits,
  never reused.
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

- `specs/system/overview.md` — drafted, with 6 anchor `sys~` specs:
  `sys~arch_001..004` (standalone device, app-as-auxiliary, USB CDC,
  multi-device stretch), `sys~ops_001` (operating-mode FSM),
  `sys~persist_001` (NVRAM in flash). All currently uncovered (expected —
  no firmware or app code yet).
- All other topic folders (`motor-control/`, `estimation/`, etc.) are
  anticipated but empty until their first spec is written.

### Decisions explicitly deferred (will become specs when made)

- USB CDC framing protocol — custom binary vs schema-driven (gRPC+protobuf
  over stream). Currently TBD in `specs/system/overview.md`.
- NVRAM implementation — littlefs vs emulated EEPROM middleware. Currently
  TBD in `specs/system/overview.md`.

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
tools/build_native.sh                # configure + build + ctest sw/lib/c
tools/build_native.sh sw/fw          # same, but for sw/fw (when populated)
tools/build_native.sh --clean        # wipe build dir first
tools/build_arm.sh                   # cross-compile sw/lib/c for embedded
tools/build_arm.sh sw/fw             # same, but for sw/fw
```

Build outputs land in `build/native-<basename>/` and `build/arm-<basename>/`.

### Code organization

Two layered trees that mirror each other in folder names:

- **`sw/lib/c/`** — channelized library implementations (the "platform").
  Reusable, generic, channel-config-driven. Builds for both targets.
- **`sw/fw/src/`** — pcs_bldc-specific integration: channel configs, board
  glue, entry point at `sw/fw/src/main.c`.

Layered architecture used in both: **`app → dev → io → hw`**, with `lib/`
(cross-cutting libraries — `nvm/`, `rtos/`) available to all layers.
Strict downward dependency by convention; same-layer or one-layer-down
calls are fine; calling up or skipping layers is discouraged but not
forbidden (judgment-call exceptions allowed).

The HW layer holds the dual-target swap:

- `sw/lib/c/hw/sim/` — stub HAL (linked into native builds for SIL).
- `sw/lib/c/hw/stm32g4/` — vendored ST HAL + CMSIS + wrapper code (linked
  into embedded builds).
- `sw/fw/src/hw/stm32g4/` — board-specific files (linker script, startup,
  HAL MSP, IT) that depend on *our* MCU pinout and memory map.

### Conventions

- **CMake target naming:** `<layer>_<module>` (e.g. `lib_ringbuf`,
  `app_mode_fsm`, `dev_kalman_observer`).
- **Module layout:** flat — `<module>.h` and `<module>.c` at the module
  root, no `include/` or `src/` subdirs. Tests live in a `test/` subdir.
- **Public-header naming:** named after the module (`ringbuf.h`); internal
  helpers prefixed (`ringbuf_internal.h`) to avoid include-path collisions
  across libraries.

### Test framework

[Unity](https://github.com/ThrowTheSwitch/Unity) v2.6.1, vendored at
`sw/lib/c/Unity/`. Tests are only built for the native target (gated by
`PCS_TARGET=native` in `sw/lib/c/CMakeLists.txt`). Each module has a
`test/` subdir with `CMakeLists.txt` + `test_<module>.c` using Unity's
`setUp` / `tearDown` / `RUN_TEST` / `UNITY_END` pattern. See
`sw/lib/c/lib/ringbuf/test/test_ringbuf.c` for a working example.

### Third-party / vendored code

Lives at the top of `sw/lib/c/` (one directory per project), e.g.
`sw/lib/c/Unity/`, `sw/lib/c/FreeRTOS/` (when added), `sw/lib/c/littlefs/`
(when added). Processor-specific third-party (CMSIS, ST HAL drivers) is
the exception — those go under `sw/lib/c/hw/stm32g4/`.

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

Installed packages (from `requirements.txt`): `requests`, `playwright`.
After install, run `.venv/Scripts/playwright install chromium` (Windows)
or `.venv/bin/playwright install chromium` (macOS) for the Playwright
browser fallback used by some datasheet sites.

## Key Components

| Component   | Role                                  | Datasheet in repo |
|-------------|---------------------------------------|-------------------|
| STM32G431C6 | MCU (Cortex-M4, 170MHz)               | Yes               |
| STSPIN32G4  | 3-phase gate driver (integrated)      | Yes               |
| CYPD3177    | USB-PD sink controller                | Yes               |
| LMR50410    | 5V buck converter                     | Yes               |
| AS5048      | Magnetic position encoder (SPI)       | Yes               |

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
`sw/lib/c/lib/ringbuf/` is the canonical example of the module/test layout
to copy.

For git commit messages, do not include a "Co-Authored-By: ..." line

PLEASE CONFIRM YOU'VE READ THIS CLAUDE.md
