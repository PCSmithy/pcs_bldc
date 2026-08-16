#!/usr/bin/env bash
# Regenerate the nanopb C bindings for the protocol schema. Run from anywhere
# after editing sw/proto/*.proto or *.options; output lands in
# sw/fw/src/lib/protobuf/generated/ (gitignored). Requires the project venv
# (./setup.sh).
set -euo pipefail
cd "$(dirname "$0")/.."

PY=.venv/Scripts/python
[ -x "$PY" ] || PY=.venv/bin/python

"$PY" -m nanopb.generator.nanopb_generator \
  --proto-path=sw/lib/c/shared/proto \
  --proto-path=sw/proto \
  --output-dir=sw/fw/src/lib/protobuf/generated \
  shared.proto board.proto

echo "Generated sw/fw/src/lib/protobuf/generated/{shared,board}.pb.{h,c}"
