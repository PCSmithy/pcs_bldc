#!/usr/bin/env bash
# Regenerate the SIL captures under sil/: design C from the branch's release
# DLL, designs A, B and Bc from the rewritten ones (build_design.sh). Every
# DLL is rebuilt first (incremental), so a capture never comes from a stale
# image. Pass design letters to run a subset, e.g. `run_scenarios.sh C`.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
case "$(uname -s)" in
  Darwin) LIBNAME=libpcs_bldc_fw.dylib ;;
  Linux)  LIBNAME=libpcs_bldc_fw.so ;;
  *)      LIBNAME=libpcs_bldc_fw.dll ;;
esac
DESIGNS=("$@")
[ ${#DESIGNS[@]} -gt 0 ] || DESIGNS=(C A B Bc)

dll_for() {
  case "$1" in
    C) echo "$ROOT/build/native-fw-release/src/$LIBNAME" ;;
    *) echo "$ROOT/build/native-fw-est$1/src/$LIBNAME" ;;
  esac
}

for design in "${DESIGNS[@]}"; do
  case "$design" in
    C) bash "$ROOT/tools/build_native.sh" --opt -O3 --lto --no-test ;;
    *) bash "$HERE/build_design.sh" "$design" ;;
  esac
done

cd "$ROOT/sw/sil"
RUN=(cargo nextest run -p pcs_bldc_sil --release --run-ignored ignored-only -E 'binary(estimator_order)' --no-capture)
for design in "${DESIGNS[@]}"; do
  dll="$(dll_for "$design")"
  # The Rust side wants a native path on Windows.
  if command -v cygpath >/dev/null 2>&1; then dll="$(cygpath -m "$dll")"; fi
  echo "==> Design $design captures"
  PCS_SIL_DLL="$dll" PCS_EST_DESIGN="$design" "${RUN[@]}"
done
echo "==> Captures in $HERE/sil"
