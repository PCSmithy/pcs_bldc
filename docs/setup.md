# First-time setup

This document is the single source of truth for setting up a development
environment for `pcs_bldc`. Run `./setup.sh` from the repo root for the
automated parts; this document describes what it checks, what it sets up,
and what to do for missing prerequisites.

## Supported platforms

- **Windows** (primary)
- **macOS** (primary)

Linux is not currently targeted and may be added later. The setup script
detects the OS via `uname -s` and adapts.

## Prerequisites

These tools are not installed automatically — `setup.sh` checks for them
and prints install instructions if they are missing.

| Tool      | Version       | Required for     | Windows install                         | macOS install                       |
|-----------|---------------|------------------|-----------------------------------------|-------------------------------------|
| Java JDK  | 17 or later   | OFT (all work)   | `winget install Microsoft.OpenJDK.21`   | `brew install --cask temurin@21`    |
| Python    | 3.10 or later | SIL, analysis    | `winget install Python.Python.3.12`     | `brew install python@3.12`          |
| KiCad     | 10.x          | **HW design only** | `winget install KiCad.KiCad`          | `brew install --cask kicad`         |
| Git       | recent        | (cloning)        | `winget install Git.Git`                | preinstalled / `brew install git`   |

**KiCad is required only for hardware design work.** If you are only
contributing to firmware, SIL infrastructure, analysis notebooks, or the
desktop tooling, you can ignore a missing-KiCad warning from `setup.sh`
and proceed. The setup script warns rather than blocks when KiCad is
missing.

The KiCad detection in `setup.sh` is version-agnostic: it globs
`C:\Program Files\KiCad\<version>\bin\kicad-cli.exe` (Windows) or uses
the standard `/Applications/KiCad/KiCad.app` location (macOS) and picks
the newest installed version. The hardware files in `hw/` are currently
maintained in **KiCad 10**.

After installing fundamental tools (especially Java) on Windows via
`winget`, restart your shell so `PATH` picks up the new entries. The OFT
wrapper has a fallback for the same-session case, but everything else
assumes `java` is on `PATH`.

## What `setup.sh` does

Once prerequisites are present, `./setup.sh` is idempotent and:

1. Detects the OS and verifies prereqs (Java, Python, KiCad).
2. Downloads and SHA256-verifies the project-pinned OpenFastTrace JAR via
   [`tools/oft/install.sh`](../tools/oft/install.sh).
3. Creates `.venv/` if it does not exist and installs Python dependencies
   from [`requirements.txt`](../requirements.txt).

Re-running is safe; already-installed steps are skipped where possible.

## Verifying the install

Smoke-test the OpenFastTrace traceability tool:

```bash
./tools/oft/oft.sh trace tools/oft/_smoketest/
```

Expected output: `ok - 5 total`, exit code 0.

The `_smoketest/` directory contains one `sys~`, one `sw~`, one `impl`,
and two `test` items that fully cover each other — useful as a working
example of the spec system described in [`spec-system.md`](spec-system.md).

## Coming later

Setup steps that will be added as the project grows:

- **Rust toolchain** (`rustup`, `cargo`) — when the first Rust component
  lands. Likely candidates: USB telemetry desktop visualizer / control
  app.
- **ARM C toolchain** (`arm-none-eabi-gcc`) — when firmware work begins.
- **Docker / devcontainer** — for fully reproducible builds in CI.
- **CI runner config** — once there is something to test in CI.

## Troubleshooting

- **`java: command not found` on Windows after `winget install`.** `PATH`
  is captured at shell start; restart your shell. The OFT wrapper
  ([`tools/oft/oft.sh`](../tools/oft/oft.sh)) will fall back to
  `C:\Program Files\Microsoft\jdk-*-hotspot\` in the meantime.
- **`pip` complains about missing modules in `.venv` on Windows.**
  Activate the venv (`.venv/Scripts/activate`) or invoke pip directly
  (`.venv/Scripts/pip.exe`).
- **`kicad-cli` not on `PATH` on Windows.** This is normal; KiCad does
  not add its CLI to `PATH` by default. Use the absolute path under
  `/c/Program Files/KiCad/<version>/bin/kicad-cli.exe` (e.g.
  `/c/Program Files/KiCad/10.0/bin/kicad-cli.exe`); `setup.sh` and
  [`CLAUDE.md`](../CLAUDE.md) detect this path automatically.
