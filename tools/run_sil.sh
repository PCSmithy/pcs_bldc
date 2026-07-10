#!/usr/bin/env bash
#
# Build the native firmware (as a shared library), build the SIL Rust
# framework, and run the sim (Rust drives the firmware over the control ABI and
# reads its state by symbol).
#
# Usage:
#   tools/run_sil.sh            # RELEASE: -O3 -flto firmware DLL + --release Rust (default)
#   tools/run_sil.sh --debug    # DEBUG:   -O0 firmware DLL + debug Rust (dev/introspection)
#   tools/run_sil.sh --clean    # wipe the native build dir first
#
# The default is an OPTIMIZED build on both sides — the SIL suite's performance
# numbers are only meaningful optimized. `--debug` restores the unoptimized
# flow (faster to build, source-faithful for debugging). Other args (e.g.
# --clean) are forwarded to tools/build_native.sh.
#
# Release and debug use SEPARATE build dirs (build/native-fw-release vs
# build/native-fw) and separate cargo target dirs, so the two flavors coexist.

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

# Host shared-library flavour. On macOS the DWARF sits in a sibling .dSYM
# (the CMake build runs dsymutil); the Rust reader finds it from the .dylib path.
case "$(uname -s)" in
  Darwin) LIBNAME=libpcs_bldc_fw.dylib ;;
  Linux)  LIBNAME=libpcs_bldc_fw.so ;;
  *)      LIBNAME=libpcs_bldc_fw.dll ;;
esac

# Parse our own --debug flag out; forward the rest to build_native.sh.
PROFILE="release"
FWD_ARGS=()
for arg in "$@"; do
  case "$arg" in
    --debug) PROFILE="debug" ;;
    *)       FWD_ARGS+=("$arg") ;;
  esac
done

if [ "$PROFILE" = "release" ]; then
  DLL_DIR="native-fw-release"
  DLL_FLAVOR="-O3 -flto -g (release)"
  BUILD_OPT=(--opt -O3 --lto --no-test)   # the SIL suite is the check for the opt DLL
  CARGO_PROFILE=(--release)
else
  DLL_DIR="native-fw"
  DLL_FLAVOR="-O0 -g (debug)"
  BUILD_OPT=()
  CARGO_PROFILE=()
fi

# 1. Native firmware -> build/<DLL_DIR>/src/$LIBNAME
echo "==> [1/3] Building native firmware (shared library, $DLL_FLAVOR)"
bash "$HERE/build_native.sh" "${BUILD_OPT[@]}" "${FWD_ARGS[@]}"

LIB="$ROOT/build/$DLL_DIR/src/$LIBNAME"
if [ ! -f "$LIB" ]; then
  echo "error: firmware shared library not found at $LIB" >&2
  exit 1
fi

# 2. SIL Rust framework
echo "==> [2/3] Building SIL framework (cargo, $PROFILE)"
cargo build "${CARGO_PROFILE[@]}" --manifest-path "$SIL_MANIFEST"

# 3. Run the sim sanity suite. On Windows (MSYS) the Rust binary needs a
#    Windows-style path for LoadLibrary; cygpath handles that and is a no-op
#    elsewhere. The suite exits nonzero if any check FAILs; capture the status
#    explicitly so we still print a summary line either way. The DLL flavor is
#    passed via env so the suite's perf report can name it.
LIB_ARG="$(cygpath -m "$LIB" 2>/dev/null || echo "$LIB")"
echo "==> [3/3] Running SIL sanity suite against $LIB_ARG"
status=0
PCS_SIL_DLL_FLAVOR="$DLL_FLAVOR" \
  cargo run --quiet "${CARGO_PROFILE[@]}" --manifest-path "$SIL_MANIFEST" \
  -p pcs_bldc_sil -- "$LIB_ARG" || status=$?

if [ "$status" -eq 0 ]; then
  echo "==> SIL sanity suite PASSED"
else
  echo "==> SIL sanity suite FAILED (exit $status)" >&2
fi
exit "$status"
