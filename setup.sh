#!/usr/bin/env bash
# pcs_bldc first-time / idempotent project setup.
#
# This script does NOT install fundamental system tools (Java, Python, Rust,
# KiCad). It checks for them and prints install instructions if they are
# missing. Once prereqs are installed, re-run this script to install the
# project-local artifacts (OFT JAR, Python venv).
#
# See docs/setup.md for full documentation and troubleshooting.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# --- OS detection ------------------------------------------------------------
case "$(uname -s)" in
  Darwin)               OS=macos ;;
  MINGW*|MSYS*|CYGWIN*) OS=windows ;;
  Linux)                OS=linux ;;
  *)                    OS=unknown ;;
esac

echo "==> Detected OS: $OS"
if [ "$OS" = "linux" ] || [ "$OS" = "unknown" ]; then
  echo "    WARNING: Linux/other platforms are not currently a supported target."
  echo "    Continuing on a best-effort basis."
fi

# --- Helpers -----------------------------------------------------------------
MISSING=0
fail() {
  echo "    MISSING: $1" >&2
  echo "    install: $2" >&2
  MISSING=$((MISSING + 1))
}

have() { command -v "$1" >/dev/null 2>&1; }

# --- Prereq: Java 17+ --------------------------------------------------------
echo
echo "==> Checking Java (>= 17)..."
JAVA_OK=0
if have java; then
  JAVA_VER="$(java -version 2>&1 | awk -F'"' 'NR==1 {print $2}' | cut -d. -f1)"
  if [ -n "$JAVA_VER" ] && [ "$JAVA_VER" -ge 17 ] 2>/dev/null; then
    echo "    OK (Java $JAVA_VER on PATH)"
    JAVA_OK=1
  else
    fail "Java 17+ required, found version $JAVA_VER" "see docs/setup.md"
  fi
else
  # Windows fallback: look for Microsoft OpenJDK install (winget default)
  if [ "$OS" = "windows" ]; then
    for dir in "/c/Program Files/Microsoft"/jdk-*-hotspot; do
      if [ -x "${dir}/bin/java.exe" ]; then
        echo "    OK (Microsoft OpenJDK at ${dir})"
        echo "    NOTE: java is not on PATH in this shell — restart your shell"
        echo "          or run \`refreshenv\`. The OFT wrapper will use the"
        echo "          fallback path in the meantime."
        JAVA_OK=1
        break
      fi
    done
  fi
  if [ "$JAVA_OK" -eq 0 ]; then
    case "$OS" in
      windows) fail "java" "winget install Microsoft.OpenJDK.21" ;;
      macos)   fail "java" "brew install --cask temurin@21" ;;
      *)       fail "java" "install Java 17+ JDK (Microsoft OpenJDK or Temurin)" ;;
    esac
  fi
fi

# --- Prereq: Python 3.10+ ----------------------------------------------------
echo
echo "==> Checking Python (>= 3.10)..."
PY_BIN=""
for cand in python3 python; do
  if have "$cand"; then
    PY_VER="$($cand --version 2>&1 | awk '{print $2}')"
    PY_MAJ="$(echo "$PY_VER" | cut -d. -f1)"
    PY_MIN="$(echo "$PY_VER" | cut -d. -f2)"
    if [ "$PY_MAJ" -eq 3 ] && [ "$PY_MIN" -ge 10 ] 2>/dev/null; then
      PY_BIN="$cand"
      echo "    OK (Python $PY_VER, '$cand')"
      break
    fi
  fi
done
if [ -z "$PY_BIN" ]; then
  case "$OS" in
    windows) fail "python 3.10+" "winget install Python.Python.3.12" ;;
    macos)   fail "python 3.10+" "brew install python@3.12" ;;
    *)       fail "python 3.10+" "install Python 3.10 or newer" ;;
  esac
fi

# --- Prereq: KiCad CLI (warn-only; HW-design only) ---------------------------
# KiCad is required ONLY for hardware design work. Contributors working on
# firmware, SIL, analysis notebooks, or desktop tooling can ignore a missing
# KiCad and proceed without issue.
echo
echo "==> Checking KiCad CLI (optional; required only for hardware design)..."
KICAD_PATH=""
if have kicad-cli; then
  KICAD_PATH=kicad-cli
elif [ "$OS" = "windows" ]; then
  # Glob across installed versions and pick the newest; bash glob expansion is
  # asciibetical (which would put "10.0" before "9.0"), so re-sort with sort -V.
  shopt -s nullglob
  kicad_candidates=("/c/Program Files/KiCad"/*/bin/kicad-cli.exe)
  shopt -u nullglob
  if [ "${#kicad_candidates[@]}" -gt 0 ]; then
    KICAD_PATH=$(printf '%s\n' "${kicad_candidates[@]}" | sort -V | tail -1)
  fi
elif [ "$OS" = "macos" ] && [ -x "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli" ]; then
  KICAD_PATH="/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
fi
if [ -n "$KICAD_PATH" ] && { [ "$KICAD_PATH" = "kicad-cli" ] || [ -x "$KICAD_PATH" ]; }; then
  KICAD_VER=$("$KICAD_PATH" --version 2>&1 | head -1)
  echo "    OK ($KICAD_VER)"
else
  case "$OS" in
    windows) echo "    Not found. To install: winget install KiCad.KiCad" ;;
    macos)   echo "    Not found. To install: brew install --cask kicad" ;;
    *)       echo "    Not found." ;;
  esac
  echo "    (Skip this if you are only working on firmware / SIL / analysis"
  echo "     tooling — KiCad is required only for hardware design work.)"
fi

# --- Prereq: ARM GCC (warn-only; embedded firmware builds only) --------------
echo
echo "==> Checking ARM GCC (optional; required only for embedded firmware builds)..."
ARM_GCC_PATH=""
if have arm-none-eabi-gcc; then
  ARM_GCC_PATH=arm-none-eabi-gcc
elif [ "$OS" = "windows" ]; then
  shopt -s nullglob
  arm_candidates=("/c/Program Files/Arm"/*/bin/arm-none-eabi-gcc.exe)
  shopt -u nullglob
  if [ "${#arm_candidates[@]}" -gt 0 ]; then
    ARM_GCC_PATH="${arm_candidates[0]}"
  fi
elif [ "$OS" = "macos" ] && [ -x "/Applications/ArmGNUToolchain/bin/arm-none-eabi-gcc" ]; then
  ARM_GCC_PATH="/Applications/ArmGNUToolchain/bin/arm-none-eabi-gcc"
fi
if [ -n "$ARM_GCC_PATH" ]; then
  ARM_GCC_VER=$("$ARM_GCC_PATH" --version 2>&1 | head -1)
  echo "    OK ($ARM_GCC_VER)"
else
  case "$OS" in
    windows) echo "    Not found. Download from https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads" ;;
    macos)   echo "    Not found. To install: brew install --cask gcc-arm-embedded" ;;
    *)       echo "    Not found." ;;
  esac
  echo "    (Skip if not building embedded firmware.)"
fi

# --- Prereq: CMake (warn-only; required for any C/C++ build) -----------------
echo
echo "==> Checking CMake (optional; required for firmware / SIL builds)..."
if have cmake; then
  echo "    OK ($(cmake --version 2>&1 | head -1))"
else
  case "$OS" in
    windows) echo "    Not found. To install: winget install Kitware.CMake" ;;
    macos)   echo "    Not found. To install: brew install cmake" ;;
    *)       echo "    Not found." ;;
  esac
  echo "    (Skip if not building C/C++ code.)"
fi

# --- Prereq: Ninja (warn-only; used by tools/build_*.sh) ---------------------
echo
echo "==> Checking Ninja (optional; used by tools/build_*.sh)..."
if have ninja; then
  echo "    OK (ninja $(ninja --version 2>&1 | head -1))"
else
  case "$OS" in
    windows) echo "    Not found. To install: winget install Ninja-build.Ninja" ;;
    macos)   echo "    Not found. To install: brew install ninja" ;;
    *)       echo "    Not found." ;;
  esac
  echo "    (Skip if not using the build scripts.)"
fi

# --- Bail if any blocking prereqs missing ------------------------------------
if [ "$MISSING" -gt 0 ]; then
  echo
  echo "==> Missing $MISSING blocking prerequisite(s). Install and re-run."
  echo "    Full instructions: docs/setup.md"
  exit 1
fi

# --- Install project-local artifacts -----------------------------------------
echo
echo "==> Installing project-local artifacts..."

echo
echo "--- OpenFastTrace ---"
./tools/oft/install.sh

echo
echo "--- Python virtual environment ---"
if [ ! -d ".venv" ]; then
  echo "    Creating .venv ..."
  "$PY_BIN" -m venv .venv
else
  echo "    .venv already exists"
fi
if [ "$OS" = "windows" ]; then
  VENV_PY=".venv/Scripts/python.exe"
else
  VENV_PY=".venv/bin/python"
fi
echo "    Upgrading pip ..."
"$VENV_PY" -m pip install --quiet --upgrade pip
echo "    Installing requirements.txt ..."
"$VENV_PY" -m pip install --quiet -r requirements.txt
echo "    OK"

# --- Done --------------------------------------------------------------------
echo
echo "==> Setup complete."
echo
echo "Verify the OFT install with:"
echo "  ./tools/oft/oft.sh trace tools/oft/_smoketest/"
echo "Expected: 'ok - 5 total', exit 0."
echo
echo "See docs/setup.md for next steps and troubleshooting."
