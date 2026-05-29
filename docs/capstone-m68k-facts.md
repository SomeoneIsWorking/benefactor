# Capstone M68K Facts (verified 2026-04-30)

## Setup

```python
from capstone import *
from capstone.m68k import *
md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
md.detail = True
```

## Mnemonics Include Size Suffix

Capstone M68K includes the operand size in the mnemonic string:

```
"bne.b"   not "bne"
"move.l"  not "move"
"clr.w"   not "clr"
```

**Fix:** `m_base = insn.mnemonic.lower().split('.')[0]`

## Instruction Data Size

NOT in `insn.data_size` (doesn't exist). It's in `insn.op_size.size`:

```python
sz = insn.op_size.size   # 1, 2, or 4
```

Default to 4 if `op_size` is None or `size == 0`.

## Operand Types

| Constant | Value | Meaning |
|----------|-------|---------|
| `M68K_OP_REG` | 1 | Register; name in `op.reg` |
| `M68K_OP_IMM` | 2 | Immediate; value in `op.imm` |
| `M68K_OP_MEM` | 3 | Memory; details in `op.mem` |
| `M68K_OP_FP_SINGLE` | 4 | Float |
| `M68K_OP_FP_DOUBLE` | 5 | Double |
| `M68K_OP_REG_BITS` | 6 | MOVEM register list; bits in `op.register_bits` |
| `M68K_OP_REG_PAIR` | 7 | Register pair |
| `M68K_OP_BR_DISP` | 8 | Branch displacement; object in `op.br_disp` |

## Address Modes (`op.address_mode`)

| Constant | Value | Meaning |
|----------|-------|---------|
| `M68K_AM_REGI_ADDR_DISP` | 6 | `d(An)` |
| `M68K_AM_PCI_DISP` | 11 | `d(PC)` — PC-relative |
| `M68K_AM_PCI_INDEX_8_BIT_DISP` | 12 | `d(PC,Xi.sz)` |
| `M68K_AM_PCI_INDEX_BASE_DISP` | 13 | extended PC-relative |
| `M68K_AM_BRANCH_DISPLACEMENT` | — | used for Bcc |

Post-increment / pre-decrement mode constants — verify with:
```python
python3 -c "from capstone.m68k import *; print([x for x in dir() if 'INC' in x or 'DEC' in x or 'POST' in x or 'PRE' in x or 'AM_' in x])"
```

## Memory Operand (`op.mem`)

| Field | Meaning |
|-------|---------|
| `base_reg` | Base address register; `0` or `M68K_REG_INVALID` if none |
| `index_reg` | Index register; `0` or `M68K_REG_INVALID` if none |
| `index_size` | `0` = word (sign-extend), `1` = longword |
| `disp` | Displacement (signed integer) |

## Absolute Addresses

When `base_reg == 0`, `index_reg == 0`, `disp == 0` — the absolute address is in **`op.imm`**:

```python
# lea.l $dff002.l, a6
op.mem.base_reg == 0    # True
op.mem.disp     == 0    # True
op.imm          == 0xDFF002  # address here!
```

## PC-Relative Operands

`op.address_mode == M68K_AM_PCI_DISP` (value 11).  
Resolved address = `insn.address + 2 + op.mem.disp`:

```python
# lea.l $531c(pc), a5 at address 0x3000
# insn.address=0x3000, op.mem.disp=0x531c
resolved = 0x3000 + 2 + 0x531c  # = 0x831e
```

## Branch / BSR Targets

Operand type is `M68K_OP_BR_DISP` (value 8), NOT `M68K_OP_IMM`.  
Target = `insn.address + 2 + op.br_disp.disp`:

```python
def branch_target(insn, op):
    if op.type == M68K_OP_BR_DISP:
        return insn.address + 2 + op.br_disp.disp
    if op.type == M68K_OP_IMM:
        return op.imm
    return None
```

## MOVEM Register List

`op.type == M68K_OP_REG_BITS`. Bits in `op.register_bits` (NOT `op.reg_bits`):
- Bits 0–7: D0–D7
- Bits 8–15: A0–A7

## Register Constants

`M68K_REG_SP` does **not** exist. Use `M68K_REG_A7` instead.

```python
_REG = {
    M68K_REG_D0:'D[0]', ..., M68K_REG_D7:'D[7]',
    M68K_REG_A0:'A[0]', ..., M68K_REG_A7:'A[7]',
}
```
