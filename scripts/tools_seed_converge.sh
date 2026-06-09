#!/usr/bin/env bash
# Iteratively seed the gpl (gameplay) bank until a gameplay+input run produces no
# rt_call misses. Each round: regenerate the gpl bank from gpl_seeds.txt, rebuild
# the harness, run a scripted gameplay session with RT_ALLOW_MISS=1 (so it logs
# ALL missed indirect-dispatch targets in one pass instead of aborting on the
# first), append the new addresses to gpl_seeds.txt, and repeat to a fixpoint.
set -u
cd "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
SEEDS=tools/recomp/gpl_seeds.txt
ARGS=( harness harness/Benefactor.slave Disk.1 Disk.2 Disk.3 )

# A session that reaches gameplay and exercises every input (walk both ways,
# up/down, jump = fire+direction) so all movement/animation handlers get hit.
read -r -d '' INPUT <<'EOF'
fire 1
pc 500
fire 0
pc 300
joy 0 0 0 1
pc 120
joy 0 0 0 0
pc 20
joy 0 0 1 0
pc 120
joy 0 0 0 0
pc 20
joy 1 0 0 0
pc 100
joy 0 1 0 0
pc 100
joy 0 0 0 0
pc 20
fire 1
joy 0 0 0 1
pc 120
joy 0 0 0 0
fire 0
pc 30
fire 1
joy 0 0 1 0
pc 120
joy 0 0 0 0
fire 0
pc 30
fire 1
joy 0 1 0 0
pc 80
joy 0 1 0 1
pc 80
joy 0 1 1 0
pc 80
joy 1 0 0 1
pc 80
joy 1 0 1 0
pc 80
joy 0 0 0 0
fire 0
pc 200
q
EOF

for round in $(seq 1 24); do
  python3 tools/recomp/recomp.py logs/gmem_after_load.bin --chip-dump \
    --base 3000 --code-size 5A0000 --areg 5=57EE12 --bank gpl \
    --out-dir src/engine/generated --seed "$(cat "$SEEDS")" >/dev/null 2>&1
  cmake --build build --target benefactor-harness -j"$(nproc)" 2>&1 | grep -iE "error:" && exit 1
  MISS=$(printf '%s\n' "$INPUT" | timeout 220 env RT_ALLOW_MISS=1 \
           ./build/benefactor-harness "${ARGS[@]}" 2>&1 \
         | grep -aoE '\[rt-miss\] \$[0-9A-F]+' | grep -oE '[0-9A-F]{6}' | sort -u)
  if [ -z "$MISS" ]; then echo "=== CONVERGED after round $round (no misses) ==="; exit 0; fi
  echo "round $round: + $(echo "$MISS" | tr '\n' ' ')"
  { echo "# auto-converge round $round"; echo "$MISS"; } >> "$SEEDS"
done
echo "=== stopped after 24 rounds (still finding misses) ==="
