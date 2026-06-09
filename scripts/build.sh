#!/usr/bin/env bash
# build.sh — the green-build gate. Clean-configure + build BOTH executables.
#
# Run this before committing/pushing. It exercises exactly what a fresh checkout on
# another machine (incl. macOS/arm64) does: a clean cmake configure (which regenerates
# src/engine/generated/ from the disks) and a build of both targets that link the same
# sources. A green single-target build in an existing build/ dir is NOT sufficient —
# see docs/working-agreements.md #1.
#
# Usage: scripts/build.sh [--fresh]
#   --fresh   remove build/ first (forces a fully clean configure)
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

# /tmp is a small tmpfs here; keep build/regen temp output in-repo (see ~/.claude rules).
export TMPDIR="$PWD/scratch/tmp"
mkdir -p "$TMPDIR"

if [ "${1:-}" = "--fresh" ]; then
  echo "[build] removing build/ for a fully clean configure"
  rm -rf build
fi

echo "[build] configure"
cmake -S . -B build

echo "[build] benefactor-pc"
cmake --build build --target benefactor-pc

echo "[build] benefactor-harness"
cmake --build build --target benefactor-harness

echo "[build] OK — both targets built"
