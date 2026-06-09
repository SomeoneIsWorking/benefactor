# Recompiler Notes (`tools/recomp/recomp.py`)

## Purpose

Offline Python tool: reads the raw 68k binary, does recursive-descent CFG analysis, emits a C source+header pair.

```
python3 tools/recomp/recomp.py <binary> <base_hex> <entry_hex ...> \
    [--out-c src/engine/generated/game.c] [--out-h src/engine/generated/game.h]
```

## Key Classes / Functions

| Symbol | Role |
|--------|------|
| `Translator` | Main class. Holds capstone disassembler, `blocks` dict, `funcs` dict, `call_targets` set |
| `Translator.analyse(entries)` | BFS CFG analysis from entry points; also explores BSR call targets |
| `Translator._translate_block(addr)` | Disassemble one basic block → `Block` |
| `Translator._emit(insn)` | Translate one instruction → `(lines, succs, terminal)` |
| `Translator._emit_movem(insn, ops, sz)` | Special handler for MOVEM |
| `Translator.emit_c(out_c, out_h)` | Write generated C/H |
| `mem_addr_expr(m, op, insn)` | Compute C address expression from capstone M68KOpMem |
| `read_op(op, sz, insn)` | C expression that reads an operand |
| `write_op(op, sz, val, insn)` | C statement that writes an operand |
| `branch_target(insn, op)` | Resolve `M68K_OP_BR_DISP` target to absolute address |
| `cc_expr(cc)` | Condition code name → C boolean expression |

## Instruction Coverage

All of these are handled (emit real C, not `RT_UNIMPL`):

`NOP`, `MOVE`, `MOVEA`, `MOVEQ`, `MOVEM`, `CLR`,  
`ADD`, `ADDA`, `ADDI`, `ADDQ`, `ADDX`,  
`SUB`, `SUBA`, `SUBI`, `SUBQ`, `SUBX`,  
`CMP`, `CMPA`, `CMPI`, `TST`,  
`AND`, `ANDI`, `OR`, `ORI`, `EOR`, `EORI`, `NOT`, `NEG`, `NEGX`,  
`EXT`, `EXTB`, `SWAP`,  
`LEA`, `PEA`, `LINK`, `UNLK`,  
`LSL`, `LSR`, `ASL`, `ASR`, `ROL`, `ROR`,  
`BTST`, `BSET`, `BCLR`, `BCHG`,  
`MULS`, `MULU`, `DIVS`, `DIVU`, `EXG`,  
`BRA`, all `Bcc`, `BSR`, `JSR`, `JMP`, `RTS`, `RTE`,  
`DBcc`, `Scc`, `TRAP`, `ILLEGAL`, `STOP`, `RESET`

Not yet handled: `ROXL`, `ROXR` (emits `RT_UNIMPL`)

## Generated Output Format

```c
// game.h
#define GAME_FN_COUNT  N
void gfn_003000(M68KCtx *ctx);
...

// game.c
#include "game.h"
const int g_fn_count = GAME_FN_COUNT;

void gfn_003000(M68KCtx *ctx) {
  L_003000:;
  /* 003000: lea.l $531c(pc), a5 */
  ctx->A[5] = 0x05831eu;
  ...
}

const GameFnEntry g_fn_table[GAME_FN_COUNT] = {
  { 0x003000u, gfn_003000 },
  ...
};
```

## Known Bugs (as of 2026-04-30)

### BUG 1 — `(An)+` / `-(An)` post-inc / pre-dec emits stale constant

**Symptom:** `clr.l (a0)+` at `$00303A` emits `MW32((uint32_t)(0x000009u), ...)` — a hardcoded `0x9` instead of `ctx->A[0]`.

**Root cause:** `mem_addr_expr` sees `base_reg=0` for post-increment mode (capstone quirk?) and falls back to `op.imm`. The `pre_decrement()` / `post_increment()` helpers exist but `_emit` never calls them.

**Fix needed:**
1. Check `op.address_mode` before calling `mem_addr_expr`:
   - If `M68K_AM_REGI_ADDR_POST_INC` → use `ctx->A[n]` directly, then append `ctx->A[n] += sz`
   - If `M68K_AM_REGI_ADDR_PRE_DEC` → prepend `ctx->A[n] -= sz`, then use `ctx->A[n]`
2. Find the right constant: `python3 -c "from capstone.m68k import *; print([x for x in dir() if 'INC' in x or 'DEC' in x or 'POST' in x or 'PRE' in x])"`

### BUG 2 — `insn` not passed to `mem_addr_expr` at call sites in `_emit`

**Symptom:** PC-relative LEA (`lea.l $531c(pc), a5`) resolves to wrong constant because `insn=None`.

**Fix needed:** All `read_op(ops[x], sz)` calls in `_emit` → `read_op(ops[x], sz, insn)`.  
All `write_op(ops[x], sz, val)` → `write_op(ops[x], sz, val, insn)`.  
All direct `mem_addr_expr(ops[x].mem)` → `mem_addr_expr(ops[x].mem, ops[x], insn)`.

### BUG 3 — Only 4 functions / 9 blocks generated

**Root cause:** Bugs 1 and 2 cause some branches/calls to terminate prematurely or target wrong addresses, starving the CFG worklist.  
After fixing Bugs 1 & 2, re-run and expect hundreds of functions from the 148 KB binary.
