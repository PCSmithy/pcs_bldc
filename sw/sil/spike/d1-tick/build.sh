#!/usr/bin/env bash
# Build the D1 fiber-port spike (native, MinGW gcc).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FRT="$HERE/../../../lib/c/FreeRTOS"

gcc -O2 -g -Wall \
  -I"$HERE" -I"$FRT/include" \
  "$HERE/spike.c" "$HERE/port.c" \
  "$FRT/tasks.c" "$FRT/list.c" "$FRT/queue.c" \
  "$FRT/portable/MemMang/heap_4.c" \
  -o "$HERE/d1_spike.exe"

echo "built $HERE/d1_spike.exe"
