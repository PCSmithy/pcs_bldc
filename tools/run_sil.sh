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
  # LTO is gated to Windows in native.cmake (Linux/macOS keep -O3 without -flto;
  # see the CMAKE_HOST_WIN32 gate + docs/sil/backlog.md). Name the flavor by OS so
  # the perf report is honest about where LTO actually applies.
  case "$(uname -s)" in
    Darwin|Linux) DLL_FLAVOR="-O3 -g (release)" ;;
    *)            DLL_FLAVOR="-O3 -flto -g (release)" ;;
  esac
  BUILD_OPT=(--opt -O3 --lto --no-test)   # --lto is a no-op off Windows (cmake gate); the SIL suite is the check for the opt DLL
  CARGO_PROFILE=(--release)
else
  DLL_DIR="native-fw"
  DLL_FLAVOR="-O0 -g (debug)"
  BUILD_OPT=()
  CARGO_PROFILE=()
fi

# 1. Native firmware -> build/<DLL_DIR>/src/$LIBNAME
echo "==> [1/3] Building native firmware (shared library, $DLL_FLAVOR)"
# The debug flavor leaves BUILD_OPT and CARGO_PROFILE empty, and a plain
# no-args run leaves FWD_ARGS empty. On macOS's bash 3.2, expanding an empty
# array as "${arr[@]}" under `set -u` (nounset) aborts with "unbound variable"
# (bash 4.4+ on Linux/Windows tolerates it). The ${arr[@]+"${arr[@]}"} guard
# (used at every array expansion below) yields the quoted elements when the
# array is non-empty and nothing when empty — portable across all three shells.
bash "$HERE/build_native.sh" ${BUILD_OPT[@]+"${BUILD_OPT[@]}"} ${FWD_ARGS[@]+"${FWD_ARGS[@]}"}

LIB="$ROOT/build/$DLL_DIR/src/$LIBNAME"
if [ ! -f "$LIB" ]; then
  echo "error: firmware shared library not found at $LIB" >&2
  exit 1
fi

# 2. SIL Rust framework (lib + integration tests + perf bin)
echo "==> [2/4] Building SIL framework (cargo, $PROFILE)"
cargo build ${CARGO_PROFILE[@]+"${CARGO_PROFILE[@]}"} --manifest-path "$SIL_MANIFEST"

# On Windows (MSYS) the Rust harness needs a Windows-style path for LoadLibrary;
# cygpath handles that and is a no-op elsewhere. Pass the freshly-built DLL to the
# tests + bin via PCS_SIL_DLL so they load exactly this image (matching the profile).
LIB_ARG="$(cygpath -m "$LIB" 2>/dev/null || echo "$LIB")"
export PCS_SIL_DLL="$LIB_ARG"

# 3. Run the checks — the whole workspace (voyant unit tests + the pcs_bldc_sil
#    behavioral suite). Each scenario is an independent #[test] over a fresh Sil
#    world (a firmware DLL loaded, booted from reset, unloaded on drop). cargo
#    nextest gives each test its own process, so the worlds parallelize with the
#    world mutex uncontended; when nextest is absent, plain cargo test serializes
#    them in one process. Exits nonzero on test failure either way.
status=0
# Whole workspace: the pcs_bldc_sil scenarios plus voyant's unit suites.
if command -v cargo-nextest >/dev/null 2>&1; then
  echo "==> [3/4] Running SIL checks (cargo nextest, process-per-test) against $LIB_ARG"
  cargo nextest run ${CARGO_PROFILE[@]+"${CARGO_PROFILE[@]}"} --manifest-path "$SIL_MANIFEST" \
    --workspace || status=$?
else
  echo "==> [3/4] cargo-nextest not on PATH; running SIL checks (cargo test) against $LIB_ARG"
  cargo test ${CARGO_PROFILE[@]+"${CARGO_PROFILE[@]}"} --manifest-path "$SIL_MANIFEST" \
    --workspace || status=$?
fi

# The shared sw/lib/rust crates are workspace-less (each consumer workspace
# resolves them by path), so their suites — dwarf_map's dSYM regressions
# included — run here explicitly.
for lib_crate in prng dwarf_map pcs_wire pcs_proto; do
  echo "==> [3/4] Running shared-crate checks: $lib_crate"
  cargo test ${CARGO_PROFILE[@]+"${CARGO_PROFILE[@]}"} \
    --manifest-path "$ROOT/sw/lib/rust/$lib_crate/Cargo.toml" || status=$?
done
if [ "$status" -ne 0 ]; then
  echo "==> SIL checks FAILED (exit $status)" >&2
  exit "$status"
fi
echo "==> SIL checks PASSED"

# 4. Performance report (informational). The DLL flavor is named so a copied-out table
#    is self-describing; a nonzero exit still fails the run.
echo "==> [4/4] Running SIL performance report"
PCS_SIL_DLL_FLAVOR="$DLL_FLAVOR" \
  cargo run --quiet ${CARGO_PROFILE[@]+"${CARGO_PROFILE[@]}"} --manifest-path "$SIL_MANIFEST" \
  -p pcs_bldc_sil || status=$?

if [ "$status" -eq 0 ]; then
  echo "==> SIL suite PASSED"
else
  echo "==> SIL performance report FAILED (exit $status)" >&2
fi
exit "$status"
