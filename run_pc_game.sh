#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_PATH="$ROOT_DIR/build/benefactor-pc"
DISK1="$ROOT_DIR/Disk.1"
DISK2="$ROOT_DIR/Disk.2"
DISK3="$ROOT_DIR/Disk.3"

usage() {
  cat <<'EOF'
Usage:
  ./run_pc_game.sh [--build]

Options:
  --build   Build before running.
  -h, --help
EOF
}

DO_BUILD=0
case "${1:-}" in
  "") ;;
  --build) DO_BUILD=1 ;;
  -h|--help) usage; exit 0 ;;
  *)
    echo "Unknown option: $1" >&2
    usage >&2
    exit 2
    ;;
esac

if [[ "$DO_BUILD" -eq 1 ]]; then
  cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" >/dev/null
  cmake --build "$ROOT_DIR/build" --target benefactor-pc -j"$(nproc)"
fi

for path in "$BIN_PATH" "$DISK1" "$DISK2" "$DISK3"; do
  if [[ ! -e "$path" ]]; then
    echo "Missing required file: $path" >&2
    exit 1
  fi
done

# Native boot from the original disk images (no snapshot): the game runs its
# own flow (intro -> logos -> title -> menu -> game) on a coroutine.
# Stderr is teed to logs/pc_run.log so diagnostics (e.g. BENEFACTOR_DBG_ENDLEVEL=1
# end-of-level / level-load traces) survive the run for inspection.
mkdir -p "$ROOT_DIR/logs"
"$BIN_PATH" --disk "$DISK1" "$DISK2" "$DISK3" 2> >(tee "$ROOT_DIR/logs/pc_run.log" >&2)
