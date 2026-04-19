#!/usr/bin/env bash
# Re-downloads the project-pinned OpenFastTrace JAR and verifies its SHA256.
# Idempotent — safe to re-run. The JAR is not committed; this script
# reproduces it from the pinned upstream release.

set -euo pipefail

OFT_VERSION="4.2.2"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAR_NAME="openfasttrace-${OFT_VERSION}.jar"
JAR_PATH="${SCRIPT_DIR}/${JAR_NAME}"
SHA_PATH="${JAR_PATH}.sha256"
RELEASE_BASE="https://github.com/itsallcode/openfasttrace/releases/download/${OFT_VERSION}"

echo "Downloading OpenFastTrace ${OFT_VERSION}..."
curl -fsSL "${RELEASE_BASE}/${JAR_NAME}.sha256" -o "${SHA_PATH}"
curl -fsSL "${RELEASE_BASE}/${JAR_NAME}"        -o "${JAR_PATH}"

echo "Verifying SHA256..."
( cd "${SCRIPT_DIR}" && sha256sum -c "${JAR_NAME}.sha256" )

echo "Installed: ${JAR_PATH}"
