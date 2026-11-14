#!/usr/bin/env bash
# Regenerate the SIL captures under sil/ from the release firmware DLL, rebuilt
# first (incremental) so a capture never comes from a stale image. Scenario
# names as arguments run a subset, e.g. `run_scenarios.sh S4_velstep`.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
case "$(uname -s)" in
  Darwin) LIBNAME=libpcs_bldc_fw.dylib ;;
  Linux)  LIBNAME=libpcs_bldc_fw.so ;;
  *)      LIBNAME=libpcs_bldc_fw.dll ;;
esac

bash "$ROOT/tools/build_native.sh" --opt -O3 --lto --no-test

dll="$ROOT/build/native-fw-release/src/$LIBNAME"
# The Rust side wants a native path on Windows.
if command -v cygpath >/dev/null 2>&1; then dll="$(cygpath -m "$dll")"; fi

cd "$ROOT/sw/sil"
PCS_SIL_DLL="$dll" PCS_EST_DESIGN=C \
  cargo run --release -p pcs_bldc_sil --example estimator_order -- "$@"
echo "==> Captures in $HERE/sil"
