# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

BLDC motor controller — a learning project for modeling and control of brushless DC motors. This is a **KiCad 9.0 hardware-only project** (no firmware yet). The design is USB-PD powered with an STM32G4 microcontroller and STSPIN32G4 gate driver.

## Repository Structure

All hardware files live in `hw/`. The schematic hierarchy is:

```
hw/bldc.kicad_sch (root - page 1)
├── usb-pd.kicad_sch        (sheet "input-power" - page 2: CYPD3177 USB-PD, LMR50410 buck, power sensing)
├── micro.kicad_sch          (sheet "micro" - page 3: STM32G431, SWD debug, encoder SPI)
├── power-stage.kicad_sch    (sheet "motor-phases" - page 4: STSPIN32G4 gate driver, 3-phase bridge)
│   ├── half_bridge.kicad_sch (sheet "half_bridge_U" - page 5)
│   ├── half_bridge.kicad_sch (sheet "half_bridge_V" - page 6)
│   └── half_bridge.kicad_sch (sheet "half_bridge_W" - page 7)
└── rgb_LEDs.kicad_sch       (sheet "rgb_LEDs" - page 8)
```

Note: `half_bridge.kicad_sch` is a single file instantiated 3 times (U/V/W phases).

`datasheets/` contains reference PDFs for all major ICs (STM32G431, STSPIN32G4, CYPD3177, LMR50410, AS5048).

## PCB Design Rules

- Board size: ~177.8 x 101.6 mm, targeting JLCPCB fabrication
- Min trace/spacing: 0.1524mm (6mil)
- Net classes: **Default** (0.2032mm track), **Power** (0.508mm track), **Signal** (0.2032mm track), **USB_diff** (0.267208mm track)
- Gerber output directory: `hw/gerber_to_order/`

## KiCad CLI

KiCad 9.0.2 is installed at `C:/Program Files/KiCad/9.0/bin/kicad-cli.exe`. Common commands:

```bash
KICAD_CLI="/c/Program Files/KiCad/9.0/bin/kicad-cli.exe"

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

All `.kicad_sch` and `.kicad_pcb` files are plain-text S-expressions and can be read/parsed directly. The root schematic references sub-sheets via `(sheet ...)` blocks containing `(property "Sheetfile" "filename.kicad_sch")`. The project file `bldc.kicad_pro` is JSON.

## kicad-happy Skills

The [kicad-happy](https://github.com/aklofas/kicad-happy) skills are cloned at `C:/code/kicad-happy/` and symlinked into this project at `.claude/skills/`. All 8 skills are installed: kicad, bom, digikey, mouser, lcsc, element14, jlcpcb, pcbway.

**At the start of each session**, read the SKILL.md files for any skills relevant to the task at hand. The most commonly needed are:

- `C:/code/kicad-happy/skills/kicad/SKILL.md` — Schematic/PCB/Gerber analysis. Has Python scripts (`analyze_schematic.py`, `analyze_pcb.py`, `analyze_gerbers.py`) that parse S-expressions into structured JSON for design review. Read this for any design review, DRC/ERC, net tracing, or circuit analysis task.
- `C:/code/kicad-happy/skills/bom/SKILL.md` — BOM lifecycle (analyze, source, export). Scripts can edit schematic properties directly. Read this for BOM, sourcing, or ordering tasks.
- `C:/code/kicad-happy/skills/jlcpcb/SKILL.md` — JLCPCB design rules, BOM/CPL format, ordering. Read this before generating fabrication outputs.

The distributor skills (`digikey/`, `mouser/`, `lcsc/`, `element14/`) each have a SKILL.md with API usage and datasheet fetching instructions — read the relevant one when sourcing components.

If the symlinks are broken (e.g. kicad-happy was moved), recreate them:

```bash
for skill in kicad bom digikey mouser lcsc element14 jlcpcb pcbway; do
  ln -sf "/c/code/kicad-happy/skills/$skill" ".claude/skills/$skill"
done
```

A Python venv is set up at `.venv/` with optional dependencies for the analysis scripts. Always use this venv when running them:

```bash
.venv/Scripts/python <script>   # on Windows
```

Installed packages: `requests` (better datasheet downloads), `playwright` (JS-heavy datasheet sites fallback, Chromium installed).

## Key Components

| Component | Role | Datasheet in repo |
|-----------|------|-------------------|
| STM32G431C6 | MCU (Cortex-M4, 170MHz) | Yes |
| STSPIN32G4 | 3-phase gate driver (integrated) | Yes |
| CYPD3177 | USB-PD sink controller | Yes |
| LMR50410 | 5V buck converter | Yes |
| AS5048 | Magnetic position encoder (SPI) | Yes |



PLEASE CONFIRM YOU'VE READ THIS CLAUDE.md