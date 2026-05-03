#!/usr/bin/env bash
# Convenience wrapper: run both build_arm.sh and build_native.sh.
# All args are forwarded to both scripts (e.g. --clean, sw/lib/c).
# Bails on the first failure (set -e).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==================== ARM ===================="
"${SCRIPT_DIR}/build_arm.sh" "$@"

echo
echo "=================== NATIVE =================="
"${SCRIPT_DIR}/build_native.sh" "$@"
