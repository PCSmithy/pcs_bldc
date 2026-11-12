#!/usr/bin/env bash
# Velocity-loop gain sweep on S4 in SIL. For each bandwidth w_v the FOC velocity
# gains in app_motorControl_channels.c are rewritten to K_vp = J w_v and
# K_vi = K_vp w_v / 10 (J the bench's 1.45e-4), the release DLL is built into
# build/native-fw-vsweep, and S4 is captured on the 1x plant with bench-like
# friction (Coulomb 15 mNm, viscous 1e-4) into sil/wv<w_v>/. The configured
# gains run first as `baseline`. The file is restored on exit (a signal
# included); nothing else may build or commit meanwhile.
#
#   tools/trace_analysis/velocity_loop/sweep.sh [baseline] [w_v ...]   default: baseline 50 60 80 100
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
FILE="$ROOT/sw/fw/src/app/motorControl/app_motorControl_channels.c"
BUILD_DIR="$ROOT/build/native-fw-vsweep"
J=1.45e-4
PLANT_TC=0.015
PLANT_B=1e-4
case "$(uname -s)" in
  Darwin) LIBNAME=libpcs_bldc_fw.dylib ;;
  Linux)  LIBNAME=libpcs_bldc_fw.so ;;
  *)      LIBNAME=libpcs_bldc_fw.dll ;;
esac
PY="$ROOT/.venv/Scripts/python"
[ -x "$PY" ] || PY="$ROOT/.venv/bin/python"
WV=("$@")
[ ${#WV[@]} -gt 0 ] || WV=(baseline 50 60 80 100)

# Restoring with mv keeps the backup's old mtime, which would leave a stale
# DLL in the build dir on the next build: touch the restored file.
cp "$FILE" "$FILE.orig"
trap 'mv -f "$FILE.orig" "$FILE" && touch "$FILE"' EXIT
touch "$FILE"

build() {
  cmake -S "$ROOT/sw/fw" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/sw/cmake/toolchains/native.cmake" \
        -DPCS_OPT_LEVEL=-O3 -DPCS_LTO=ON >/dev/null
  cmake --build "$BUILD_DIR" | tail -1
}

capture() {   # label
  local dll="$BUILD_DIR/src/$LIBNAME"
  if command -v cygpath >/dev/null 2>&1; then dll="$(cygpath -m "$dll")"; fi
  (cd "$ROOT/sw/sil" && PCS_SIL_DLL="$dll" PCS_EST_DESIGN=C PCS_EST_OUT="$HERE/sil/$1" PCS_EST_VARIANTS=nominal \
     PCS_EST_PLANT_TC_NM="$PLANT_TC" PCS_EST_PLANT_B="$PLANT_B" \
     cargo run --release -p pcs_bldc_sil --example estimator_order -- S4_velstep 2>&1 | tail -3)
}

for wv in "${WV[@]}"; do
  if [ "$wv" = baseline ]; then
    echo "==> baseline (configured gains)"
    build
    capture baseline
    continue
  fi
  echo "==> w_v = $wv rad/s"
  "$PY" - "$FILE" "$J" "$wv" <<'EOF'
import pathlib, re, sys
path, j, wv = pathlib.Path(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
kp = j * wv
ki = kp * wv / 10
raw = path.read_bytes()
crlf = b"\r\n" in raw
text = raw.decode("utf-8").replace("\r\n", "\n")
text, n1 = re.subn(r"\.velocityKp_NmPerRadPerSec = [^,]+,[^\n]*", f".velocityKp_NmPerRadPerSec = {kp:.5g}f,   // sweep: J = {j:g}, w_v = {wv:g}", text)
text, n2 = re.subn(r"\.velocityKi_NmPerRad = [^,]+,[^\n]*", f".velocityKi_NmPerRad = {ki:.5g}f,   // sweep: K_vp * w_v / 10", text)
assert n1 == 1 and n2 == 1, (n1, n2)
path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
print(f"    K_vp = {kp:.5g} Nm/(rad/s), K_vi = {ki:.5g} Nm/rad")
EOF
  build
  capture "wv$wv"
done
echo "==> Captures in $HERE/sil; analyse with analyze.py"
