#!/usr/bin/env bash
# Cross-compile a CMake project for the embedded ARM target
# (STM32G431, Cortex-M4F).
#
# Usage:
#   tools/build_arm.sh [<source-subdir>]
#
# Defaults to sw/lib/c. Build output lives in build/arm-<basename>/.
# Re-running is incremental; pass --clean to wipe the build dir first.
# No tests are run — the cross-compiled binaries don't run on the host.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN="${REPO_ROOT}/sw/cmake/toolchains/arm-none-eabi.cmake"

CLEAN=0
SOURCE_SUBDIR=""
for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    -*)      echo "Unknown flag: $arg" >&2; exit 2 ;;
    *)       SOURCE_SUBDIR="$arg" ;;
  esac
done
SOURCE_SUBDIR="${SOURCE_SUBDIR:-sw/lib/c}"
BUILD_DIR="${REPO_ROOT}/build/arm-$(basename "${SOURCE_SUBDIR}")"

if [ "${CLEAN}" -eq 1 ] && [ -d "${BUILD_DIR}" ]; then
  echo "==> Removing ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

echo "==> Configuring (arm, ${SOURCE_SUBDIR})"
cmake -S "${REPO_ROOT}/${SOURCE_SUBDIR}" -B "${BUILD_DIR}" \
      -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}"

echo "==> Building"
cmake --build "${BUILD_DIR}"
