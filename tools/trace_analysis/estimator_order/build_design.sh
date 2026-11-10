#!/usr/bin/env bash
# Build a comparison design's firmware DLL into build/native-fw-est<design>.
# The estimator source is rewritten in place for the build and restored on
# exit (a signal included), so the rest of the working tree is untouched;
# nothing else may build or commit meanwhile. The branch head is design C.
#
#   A    constant-velocity advance (u = 0), second-order gains, no torque state
#   B    the known-input advance without the Coulomb term, second-order gains
#   Bc   the known-input advance with the Coulomb term, second-order gains:
#        design C with its torque state held at zero
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
DESIGN="${1:?usage: build_design.sh A|B|Bc}"
case "$DESIGN" in A|B|Bc) ;; *) echo "unknown design $DESIGN (A, B or Bc)" >&2; exit 1 ;; esac
FILE="$ROOT/sw/lib/c/shared/app/rotorEstimation/app_rotorEstimation.cpp"
BUILD_DIR="$ROOT/build/native-fw-est$DESIGN"
PY="$ROOT/.venv/Scripts/python"
[ -x "$PY" ] || PY="$ROOT/.venv/bin/python"

# Restoring with mv keeps the backup's old mtime, so a build dir that last
# saw the rewritten file would keep its stale objects: touch the restored file.
cp "$FILE" "$FILE.orig"
trap 'mv -f "$FILE.orig" "$FILE" && touch "$FILE"' EXIT

"$PY" - "$FILE" "$DESIGN" <<'EOF'
import pathlib, sys
path, design = pathlib.Path(sys.argv[1]), sys.argv[2]
raw = path.read_bytes()
crlf = b"\r\n" in raw
text = raw.decode("utf-8").replace("\r\n", "\n")

def swap(old, new):
    global text
    assert text.count(old) == 1, f"expected one match for {old[:50]!r}"
    text = text.replace(old, new)

swap("        const float32_t l1 = (2.0f * zeta * wn) + pd - bOverJ;\n"
     "        const float32_t l2 = (wn * wn) + (2.0f * zeta * wn * pd) - (l1 * bOverJ);\n"
     "        const float32_t l3 = J * wn * wn * pd;\n",
     f"        (void)pd;   // design {design}: second-order gains, no torque state\n"
     "        (void)bOverJ;\n"
     "        const float32_t l1 = 2.0f * zeta * wn;\n"
     "        const float32_t l2 = wn * wn;\n"
     "        const float32_t l3 = ZERO;\n")
u_old = ("        this->u = this->torqueInput_Nm - (this->config->viscousFriction_NmPerRadPerSec * omegaHat) -\n"
         "                  (this->config->coulombFriction_Nm * SATURATE((omegaHat * this->coulombRampGain), -1.0f, 1.0f));\n")
if design == "A":
    swap(u_old, "        (void)omegaHat;\n        this->u = ZERO;   // design A: no known-input term\n")
elif design == "B":
    swap(u_old, "        this->u = this->torqueInput_Nm - (this->config->viscousFriction_NmPerRadPerSec * omegaHat);   // design B: no Coulomb term\n")
path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode("utf-8"))
print(f"estimator rewritten as design {design}")
EOF

echo "==> Configuring design $DESIGN (native, -O3, LTO)"
cmake -S "$ROOT/sw/fw" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/sw/cmake/toolchains/native.cmake" \
      -DPCS_OPT_LEVEL=-O3 -DPCS_LTO=ON
echo "==> Building"
cmake --build "$BUILD_DIR"
echo "==> Design $DESIGN DLL in $BUILD_DIR/src"
