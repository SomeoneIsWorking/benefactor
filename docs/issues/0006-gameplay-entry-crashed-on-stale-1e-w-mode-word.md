---
id: 6
title: Gameplay entry crashed on stale $1E.w mode word
status: resolved
symptom: Entering gameplay $577000 (attract or --level): illegal opcode $0FFF at pc=0 after ~71k insns, guest execution stops reason=6 vector=4
state_items: S005,S023
tags: interpreter,gameplay,crash
created: 2026-09-09
updated: 2026-09-09
---

CAUSE: the $150 gameplay hand-off and pc_init_to_gameplay entered $577000 without normalising absolute low-mem $1E.w. Attract/game-over leaves $1E.w=8; the $577000 prologue reads ($1E.w & 7) as difficulty index and $57DEAC treats the exact value 8 as demo/game-over input playback -> reads scripted joystick tokens from an uninitialised stream at $22.w -> d7=$9A -> overflows the grounded-input jump table at $F2A(a5) -> jsr into zeroed RAM -> illegal instr -> no exception vector -> PC=0 cascade.

FIX: new src/engine/gameplay_handoff.c reconstructs the retail $150 body low-mem init: $3e/$184 card-renderer sentinels (previously duplicated inline 3x) + $1E.w normalisation (keep a menu EASY/NORMAL/HARD bit 1/2/4, else force NORMAL=2, matching the engine's own $57720C inter-level write). Wired into native_overlay_loader_reloc, pc_init_to_gameplay, and the restart-reinit path.

VERIFIED headless: natural attract path now reaches the level-intro card ('$57FEA5=20 cop1lc=003914') and holds for fire; --level 1 + fire injection at f90 transitions to in-level ('$57FEA5=80 cop1lc=003484') and runs 1735 frames with no illegal-instruction exit. verify.py green.
