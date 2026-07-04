#!/usr/bin/env bash
#
# Build the native firmware (as a shared library), build the SIL Rust
# framework, and run the sim (Rust drives the firmware over the control ABI and
# reads its state by symbol).
#
# Usage:
#   tools/run_sil.sh            # incremental build + run
#   tools/run_sil.sh --clean    # wipe the native build dir first
#
# Any args are forwarded to tools/build_native.sh (e.g. --clean).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SIL_MANIFEST="$ROOT/sw/sil/Cargo.toml"

# cargo is installed by rustup at ~/.cargo/bin, which isn't always on PATH
# (notably in non-interactive shells). Add it if needed.
if ! command -v cargo >/dev/null 2>&1; then
  export PATH="$HOME/.cargo/bin:$PATH"
fi
if ! command -v cargo >/dev/null 2>&1; then
  echo "error: cargo not found. Install the Rust toolchain:" >&2
  echo "  rustup toolchain install stable-x86_64-pc-windows-gnu" >&2
  echo "  rustup default stable-x86_64-pc-windows-gnu" >&2
  exit 1
fi

# 1. Native firmware -> build/native-fw/src/libpcs_bldc_fw.dll
echo "==> [1/3] Building native firmware (shared library)"
bash "$HERE/build_native.sh" "$@"

DLL="$ROOT/build/native-fw/src/libpcs_bldc_fw.dll"
if [ ! -f "$DLL" ]; then
  echo "error: firmware DLL not found at $DLL" >&2
  exit 1
fi

# 2. SIL Rust framework
echo "==> [2/3] Building SIL framework (cargo)"
cargo build --manifest-path "$SIL_MANIFEST"

# 3. Run the sim sanity suite. The Rust binary is a native Windows process, so
#    LoadLibrary needs a Windows-style path (C:/...), not an MSYS path (/c/...).
#    The suite exits nonzero if any check FAILs; propagate that so this script
#    (and CI) fails loudly. `set -e` alone would abort here, but we want a
#    summary line either way, so capture the status explicitly.
DLL_WIN="$(cygpath -m "$DLL" 2>/dev/null || echo "$DLL")"
echo "==> [3/3] Running SIL sanity suite against $DLL_WIN"
status=0
cargo run --quiet --manifest-path "$SIL_MANIFEST" -p pcs_bldc_sil -- "$DLL_WIN" || status=$?

if [ "$status" -eq 0 ]; then
  echo "==> SIL sanity suite PASSED"
else
  echo "==> SIL sanity suite FAILED (exit $status)" >&2
fi
exit "$status"
