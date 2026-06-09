#!/bin/sh
# run-wide.sh — launch Benefactor at level 9 in widescreen for quick testing.
#
# Usage:
#   ./run-wide.sh            # standalone SDL window, level 9, 480px wide (press fire to start)
#   ./run-wide.sh 640        # wider
#   ./run-wide.sh 480 build  # rebuild benefactor-pc first
#
# The standalone lands on the level card; press fire (Ctrl/Space/Z) to enter gameplay,
# arrows to move. Same level-9 cavern used in all the widescreen comparison work.
cd "$(dirname "$0")/.." || exit 1

WIDTH="${1:-480}"
if [ "$2" = "build" ]; then
    TMPDIR="$PWD/scratch/tmp" cmake --build build --target benefactor-pc || exit 1
fi

echo "[run-wide] level 9, BENEFACTOR_WIDESCREEN=$WIDTH — press fire to start, arrows to move"
exec env BENEFACTOR_WIDESCREEN="$WIDTH" ./build/benefactor-pc Disk.1 Disk.2 Disk.3 --level 9
