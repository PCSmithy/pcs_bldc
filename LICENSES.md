# Licensing

This repository contains first-party work under more than one license, plus
vendored third-party code that keeps its own license. This file is the map.

## First-party (our) work

| Part | License | Text |
|------|---------|------|
| Firmware, SIL/desktop tooling, build scripts, specs, docs | **Apache-2.0** | [`LICENSE`](LICENSE) |
| Hardware design in [`hw/`](hw/) (KiCad schematics, PCB, gerbers) | **CERN-OHL-W-2.0** (Weakly Reciprocal) | [`hw/LICENSE`](hw/LICENSE) |

Rationale: software licenses are written for source code, so the KiCad
hardware design is licensed under CERN-OHL-W — a license written for hardware.
"Weakly reciprocal" means changes to *this design* should be shared back, but
the design may be used within a larger product without that product having to
be open.

## Third-party code vendored in this repository

Each retains its own license and copyright; our licenses above do not apply to
it. Keep these license files and headers intact.

| Component | Location | License |
|-----------|----------|---------|
| ARM CMSIS | [`sw/lib/c/CMSIS/`](sw/lib/c/CMSIS/) | Apache-2.0 ([`LICENSE.txt`](sw/lib/c/CMSIS/LICENSE.txt)) |
| STMicroelectronics STM32G4xx HAL driver | [`sw/lib/c/STM32G4xx_HAL_Driver/`](sw/lib/c/STM32G4xx_HAL_Driver/) | BSD-3-Clause ([`LICENSE.txt`](sw/lib/c/STM32G4xx_HAL_Driver/LICENSE.txt)) |
| FreeRTOS kernel | [`sw/lib/c/FreeRTOS/`](sw/lib/c/FreeRTOS/) | MIT |
| TinyUSB | [`sw/lib/c/tinyusb/`](sw/lib/c/tinyusb/) | MIT |
| Unity (test framework) | [`sw/lib/c/Unity/`](sw/lib/c/Unity/) | MIT ([`LICENSE.txt`](sw/lib/c/Unity/LICENSE.txt)) |

## Third-party Rust crates (fetched by Cargo, not vendored in-tree)

The SIL framework (`sw/sil/`) depends on these via Cargo; their license terms
apply to the fetched crates, not to code in this repository.

| Crate | License |
|-------|---------|
| `gimli` | MIT OR Apache-2.0 |
| `object` | MIT OR Apache-2.0 |
| `libloading` | ISC |
