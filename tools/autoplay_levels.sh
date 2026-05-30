#!/usr/bin/env bash
# Auto-traverse: hold fire + L through every level, until either the game ends
# or strict-mode aborts on an rt-miss. The miss site's post-mortem dump
# (logs/pc_freeze.bin) is then ready for tools/recomp/discover_benefactor_targets.py.
#
# Each cycle:
#   1. runto $003484 (cavern gameplay)
#   2. brief play, then release fire
#   3. `done` (sets the level-complete flag bit5 of $10AC)
#   4. fire press / release pattern to dismiss the win banner and the next
#      level's intro card
#
# Usage:  tools/autoplay_levels.sh [LEVELS]   (default: 30)

set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"

LEVELS=${1:-30}
LOG=logs/autoplay.log
mkdir -p logs

cmake --build build --target benefactor-harness -j"$(nproc)" 2>&1 | tail -3

{
  echo "fire 1"
  echo "runto 003484"
  for i in $(seq 1 "$LEVELS"); do
    cat <<CMDS
state
pc 100
fire 0
pc 5
done
pc 30
fire 1
pc 10
fire 0
pc 300
fire 1
pc 80
fire 0
pc 30
fire 1
pc 200
CMDS
  done
  echo "state"
  echo "q"
} | tee logs/autoplay_cmds.txt | timeout 1800 ./build/benefactor-harness \
    harness harness/Benefactor.slave Disk.1 Disk.2 Disk.3 2>&1 | tee "$LOG"

echo "---"
grep -E "rt-miss|rt_call.*aborting|crepl\] PC cop1lc" "$LOG" | tail -10
