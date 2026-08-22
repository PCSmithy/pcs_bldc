#!/usr/bin/env bash
# Configure + build + test a CMake project for the native (host) target.
# Used for SIL builds and unit testing.
#
# Usage:
#   tools/build_native.sh [<source-subdir>] [--opt <-Oflag>] [--no-test]
#
# Defaults to sw/fw (the firmware project — pulls sw/lib/c in via
# add_subdirectory, so all lib unit tests run too). Pass `sw/lib/c`
# explicitly for a lib-only build.
#
# --opt <-Oflag>  optimization level for the native build (default -O0, the
#                 dev/test flow). Passed to native.cmake as PCS_OPT_LEVEL. A
#                 non-default level builds into a SEPARATE dir so the optimized
#                 and -O0 artifacts coexist (tools/run_sil.sh uses -O3 for its
#                 optimized SIL DLL).
# --lto           enable link-time optimization (PCS_LTO=ON in native.cmake).
#                 Release SIL DLL only; the -O0 dev/test flow never sets it.
# --no-test       skip ctest (used for the SIL DLL build — the SIL suite is the
#                 check there, not the Unity tests).
#
# Build output lives in build/native-<basename>[<-opt-suffix>]/. Re-running is
# incremental; pass --clean to wipe the build dir first.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN="${REPO_ROOT}/sw/cmake/toolchains/native.cmake"

CLEAN=0
SOURCE_SUBDIR=""
OPT="-O0"
LTO=0
RUN_TESTS=1
while [ $# -gt 0 ]; do
  case "$1" in
    --clean)   CLEAN=1 ;;
    --no-test) RUN_TESTS=0 ;;
    --opt)     shift; OPT="${1:?--opt needs a value}" ;;
    --lto)     LTO=1 ;;
    -*)        echo "Unknown flag: $1" >&2; exit 2 ;;
    *)         SOURCE_SUBDIR="$1" ;;
  esac
  shift
done
SOURCE_SUBDIR="${SOURCE_SUBDIR:-sw/fw}"

# A non-default opt level gets its own build dir so optimized and -O0 artifacts
# never clobber each other (e.g. build/native-fw vs build/native-fw-release).
BUILD_SUFFIX=""
if [ "${OPT}" != "-O0" ]; then
  BUILD_SUFFIX="-release"
fi
BUILD_DIR="${REPO_ROOT}/build/native-$(basename "${SOURCE_SUBDIR}")${BUILD_SUFFIX}"

if [ "${CLEAN}" -eq 1 ] && [ -d "${BUILD_DIR}" ]; then
  echo "==> Removing ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

LTO_CMAKE="OFF"
if [ "${LTO}" -eq 1 ]; then
  LTO_CMAKE="ON"
fi

echo "==> Configuring (native, ${SOURCE_SUBDIR}, ${OPT}, LTO=${LTO_CMAKE})"
cmake -S "${REPO_ROOT}/${SOURCE_SUBDIR}" -B "${BUILD_DIR}" \
      -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
      -DPCS_OPT_LEVEL="${OPT}" \
      -DPCS_LTO="${LTO_CMAKE}" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo "==> Building"
cmake --build "${BUILD_DIR}"

if [ "${RUN_TESTS}" -eq 1 ]; then
  echo "==> Running tests"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
else
  echo "==> Skipping tests (--no-test)"
fi
