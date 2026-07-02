#!/usr/bin/env bash
# Configure + build + test a CMake project for the native (host) target.
# Used for SIL builds and unit testing.
#
# Usage:
#   tools/build_native.sh [<source-subdir>]
#
# Defaults to sw/fw (the firmware project — pulls sw/lib/c in via
# add_subdirectory, so all lib unit tests run too). Pass `sw/lib/c`
# explicitly for a lib-only build.
#
# Build output lives in build/native-<basename>/. Re-running is
# incremental; pass --clean to wipe the build dir first.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN="${REPO_ROOT}/sw/cmake/toolchains/native.cmake"

CLEAN=0
SOURCE_SUBDIR=""
for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    -*)      echo "Unknown flag: $arg" >&2; exit 2 ;;
    *)       SOURCE_SUBDIR="$arg" ;;
  esac
done
SOURCE_SUBDIR="${SOURCE_SUBDIR:-sw/fw}"
BUILD_DIR="${REPO_ROOT}/build/native-$(basename "${SOURCE_SUBDIR}")"

if [ "${CLEAN}" -eq 1 ] && [ -d "${BUILD_DIR}" ]; then
  echo "==> Removing ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

echo "==> Configuring (native, ${SOURCE_SUBDIR})"
cmake -S "${REPO_ROOT}/${SOURCE_SUBDIR}" -B "${BUILD_DIR}" \
      -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}"

echo "==> Building"
cmake --build "${BUILD_DIR}"

echo "==> Running tests"
ctest --test-dir "${BUILD_DIR}" --output-on-failure
