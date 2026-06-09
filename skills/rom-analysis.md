# Skill: ROM Analysis

Disassemble and analyze the Benefactor M68K binary from the PUAE chip RAM dump. Use when tracing a function, finding what code writes to an address, or understanding the per-frame call sequence. Source of truth: `logs/harness_puae_chipram.bin` (game code at offset $3000, base addr $3000).

> **Self-evolution:** If you extend or build a tool that changes this skill's procedure, update this file in the same step.

## Source of Truth
- Binary: `logs/harness_puae_chipram.bin` (524288 bytes)
- Game code base: offset `$3000` in file = address `$3000` in chip RAM
- Recompiled output: `src/engine/generated/game.c`
- Symbol table (function → address): last entries in `game.c` dispatch table

## Quick Disassembly (run in terminal)
```python
python3 -c "
import struct
data = open('logs/harness_puae_chipram.bin','rb').read()
off = 0xADDR   # file offset = chip RAM address (base 0)
for i in range(0, 64, 2):
    w = (data[off+i]<<8)|data[off+i+1]
    print('  \$%04X: %04X' % (off+i, w))
"
```

## Procedure: Find What Writes to an Address
1. Add `[WATCH16]` watchpoint in `rt.c:rt_write16` for the target address range.
2. Build and run: `bash run_harness.sh --frames 3 --boot-frames 600 2>&1 | grep WATCH`
3. Note the call stack (`[stack N] $XXXXXX`). The deepest frame is the writing function.
4. Disassemble from that address to understand what the function computes.
5. Cross-reference with `game.c` — search `gfn_00XXXX` for the C translation.

## Procedure: Understand Per-Frame Call Sequence
1. Start from `pc.c` S_TITLE loop: `call_fn(0x0041A4)` → `call_fn(0x00405C)` → `call_fn(0x0055A0)` → `hw_present_frame()`.
2. Enable trace in `rt.c`: set `rt_trace_insns = 1` around a specific call to see all instructions.
3. Or disassemble the M68K function directly from the chip dump.

## Procedure: Find Function that Writes a Copper Instruction
1. Know the target copper address (e.g., BPLCON2 at `$8728`).
2. Check if `rt_write16` fires for that address (watchpoint).
3. If not, check blitter: add `[WATCH_BLT]` in `hw.c:hw_do_blit()`.
4. If blitter clobbered it, find the REBUILD function — look for calls after the blitter fill.
5. Search `game.c` for `MW16` with the register code value (e.g., `0x0104` = DFF104 = BPLCON2):
   ```bash
   grep -n "MW16.*0x0104\|0x0100\|0x0102" src/engine/generated/game.c | head -20
   ```
6. Note: copper instructions are pairs of 16-bit writes: reg-word then val-word. The reg-word is `$01xx` for display registers.

## Known Facts (Do Not Re-Examine)
- Blitter fill: `bltcon0=$19F0`, dest=`$8720`, writes `$FFFF` to `$8720–$872C` (BPLCON0/1/2 copper instructions).
- `gfn_00405C` rebuilds BPL1PT–BPL4PT at `$8788–$87AE` only. Does NOT rebuild BPLCON entries.
- `gfn_0041A4` triggers the blitter fill. It does NOT rebuild BPLCON copper entries.
- BPLCON2=$0040 at `$872A` is set in PUAE's chip dump (from level-init run by PUAE, not PC port).
- See `instructions/harness.md` for all confirmed addresses.
