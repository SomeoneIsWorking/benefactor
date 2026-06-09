#!/bin/bash
# Side-by-side PUAE (reference) vs PC port, in one window, driven by a REPL.
# The two cores run INDEPENDENTLY (no lockstep) — you advance them with commands
# and compare what each shows. Type commands at the prompt; the window updates on
# each advance.
set -e
cd "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
cmake -S . -B build >/dev/null
cmake --build build --target benefactor-harness -j"$(nproc)" 2>&1 | tail -3

cat <<'EOF'

=== PUAE (left) vs PC (right) — REPL ===
Commands (type then Enter):
  play                   LIVE keyboard drive of BOTH cores (focus the window):
                         arrows = move, Z/Ctrl/Space = fire, ESC/close = quit
                         L = debug: force PC level-complete (test level transition)
  fire 1 | fire 0        hold / release fire on BOTH cores
  joy U D L R            hold a direction on both (1=on,0=off), e.g. joy 0 0 0 1 = right
  both N                 advance BOTH cores N frames (interleaved) — window animates
  pc N  |  pu N          advance only PC / only PUAE N frames
  headed 1 | headed 0    show / hide the window
  state                  print each core's cop1lc (+ PC last fn/read)
  fb [tag]               dump framebuffers to logs/fb_{pc,puae}[_tag].bin
  m  ADDR [n]            peek PUAE memory (any bank);  mp ADDR [n] = peek PC memory
  q                      quit

Tip: to reach gameplay, hold fire and run a while:  fire 1   then   both 1000
EOF

exec ./build/benefactor-harness \
    harness \
    harness/Benefactor.slave \
    Disk.1 \
    Disk.2 \
    Disk.3 \
    --play "$@"
