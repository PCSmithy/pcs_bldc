#!/usr/bin/env bash
# Wrapper that runs OpenFastTrace via the project-pinned JAR.
# Forwards all arguments to OFT.

set -euo pipefail

OFT_VERSION="4.2.2"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAR_PATH="${SCRIPT_DIR}/openfasttrace-${OFT_VERSION}.jar"

if [ ! -f "${JAR_PATH}" ]; then
  echo "OpenFastTrace JAR not found at ${JAR_PATH}" >&2
  echo "Run ${SCRIPT_DIR}/install.sh first." >&2
  exit 1
fi

# Resolve java: prefer PATH, fall back to a Microsoft OpenJDK install
# (this covers the case where the JDK was installed in the same shell session
#  and the PATH has not yet been refreshed).
if command -v java >/dev/null 2>&1; then
  JAVA_BIN="java"
else
  JAVA_BIN=""
  for dir in "/c/Program Files/Microsoft"/jdk-*-hotspot; do
    if [ -x "${dir}/bin/java.exe" ]; then
      JAVA_BIN="${dir}/bin/java.exe"
      break
    fi
  done
  if [ -z "${JAVA_BIN}" ]; then
    echo "Error: java not found on PATH or at any known JDK install location." >&2
    echo "Install a Java 17+ JDK, or restart your shell so java is on PATH." >&2
    exit 1
  fi
fi

exec "${JAVA_BIN}" -jar "${JAR_PATH}" "$@"
