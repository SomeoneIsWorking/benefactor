# Benefactor PC Port — Session Handoff

**Date:** 2026-04-30  
**Session:** 3  
**Goal:** Static recompilation of Benefactor (1994, Psygnosis/Digital Illusions) from Amiga 68k to native C + SDL2.  
No 68k interpreter at runtime — all game logic compiles to native functions.

---

## Immediate Next Tasks

### 1. Fix `(An)+` / `-(An)` post-increment/pre-decrement in `_emit`

`pre_decrement()` and `post_increment()` helpers exist in `tools/recomp/recomp.py` but are **never called** from `_emit`.  
For instructions that read/write `(An)+` or `-(An)`, the current code uses a stale constant address instead of `ctx->A[n]`.

Fix: detect `address_mode` on the operand:
- `M68K_AM_REGI_ADDR_POST_INC` → emit `r` then `r += sz`
- `M68K_AM_REGI_ADDR_PRE_DEC` → emit `r -= sz` then `r`

Check actual constant name:
```python
python3 -c "from capstone.m68k import *; print([x for x in dir() if 'POST' in x or 'PRE' in x or 'INC' in x or 'DEC' in x])"
```

### 2. Pass `insn` through all `read_op` / `write_op` calls in `_emit`

The signatures already accept `insn=None`:
```python
def read_op(op, sz, insn=None)
def write_op(op, sz, val, insn=None)
def mem_addr_expr(m, op=None, insn=None)
```

But every call site in `_emit` still passes only `(ops[x], sz)`.  
Do a bulk replace: all `read_op(ops[` → `read_op(ops[` with `insn=insn` appended, and similarly for `write_op`.

### 3. Re-run recompiler and check function count

```bash
cd <repo>
python3 tools/recomp/recomp.py chip_ram_dump.bin --chip-dump \
  --out-c src/engine/generated/game.c \
  --out-h src/engine/generated/game.h 2>&1
```

After the fixes above the output should report **hundreds of functions**, not just 4.

### 4. Build

```bash
cd benefactor-pc
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --parallel 4
```

### 5. Run

```bash
cd <repo>
./build/benefactor-pc \
  chip_ram_dump.bin \
  Disk.1 \
  Disk.2 \
  Disk.3
```

---

## Known Issues / Gotchas

| Issue | Status | Notes |
|-------|--------|-------|
| `M68K_REG_SP` does not exist | Fixed | Removed from `_REG` dict |
| Mnemonic has size suffix (`bne.b`) | Fixed | Strip via `m.split('.')[0]` |
| `op_size` is `M68KOpSize` object | Fixed | Use `op_size.size` |
| Absolute addresses in `op.imm` | Fixed | Not in `mem.disp` |
| PC-relative (`M68K_AM_PCI_DISP`) | Fixed | `insn.address + 2 + mem.disp` |
| Branch targets via `M68K_OP_BR_DISP` | Fixed | Use `branch_target()` helper |
| MOVEM uses `register_bits` | Fixed | Not `reg_bits` |
| BSR targets not explored by CFG | Fixed | Added to worklist in `analyse()` |
| `(An)+` / `-(An)` emits stale address | **Not fixed** | See task 1 above |
| `insn` not passed to `mem_addr_expr` | **Not fixed** | See task 2 above |
| Only 4 functions generated | Symptom of above two bugs | Will resolve once fixed |

---

## Context: Why We Recompile

The original emulator approach (Musashi + custom chip emulation) was abandoned because:
- The copper list was running off into random memory (game builds new copper list each frame at variable addresses — the chip_ram_dump was taken mid-execution)
- The vblank sync loop took 400+ real-time frames to pass
- Debugging Amiga hardware timing is unbounded scope

The recompiler approach instead:
- Extracts the 68k binary from the chip RAM dump
- Translates it to native C functions (`gfn_XXXXXX(M68KCtx *ctx)`)
- Routes all hardware register reads/writes through `hw.c` → SDL2
- Game logic runs at full native speed, only I/O is intercepted

---

## File Map

See **`docs/codebase-layout.md`** for the canonical, up-to-date module map. (This
handoff predates the `port/engine/render/common` reorganization; the layout it once
described — `recomp/`, `amiga/`, `platform/`, a single `game.c` — no longer exists.)
