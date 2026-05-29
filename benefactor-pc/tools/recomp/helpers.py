"""Capstone M68K register / operand helpers shared across the recompiler."""
from capstone.m68k import *

# ---------------------------------------------------------------------------
# Register name mapping
# ---------------------------------------------------------------------------

def reg_name(r):
    """Map Capstone register id to C field name."""
    if r == M68K_REG_SR:  return 'SR'
    if r == M68K_REG_PC:  return 'PC'
    if r == M68K_REG_D0:  return 'D[0]'
    if r == M68K_REG_D1:  return 'D[1]'
    if r == M68K_REG_D2:  return 'D[2]'
    if r == M68K_REG_D3:  return 'D[3]'
    if r == M68K_REG_D4:  return 'D[4]'
    if r == M68K_REG_D5:  return 'D[5]'
    if r == M68K_REG_D6:  return 'D[6]'
    if r == M68K_REG_D7:  return 'D[7]'
    if r == M68K_REG_A0:  return 'A[0]'
    if r == M68K_REG_A1:  return 'A[1]'
    if r == M68K_REG_A2:  return 'A[2]'
    if r == M68K_REG_A3:  return 'A[3]'
    if r == M68K_REG_A4:  return 'A[4]'
    if r == M68K_REG_A5:  return 'A[5]'
    if r == M68K_REG_A6:  return 'A[6]'
    if r == M68K_REG_A7:  return 'A[7]'
    return f'UNK_R_{r}'


# ---------------------------------------------------------------------------
# Operand field accessors (Capstone attribute name varies by version)
# ---------------------------------------------------------------------------

def mem_base(op):
    """Return the base register id of a MEM operand."""
    return getattr(op.mem, 'base_reg', getattr(op.mem, 'base', None))


def mem_index(op):
    """Return the index register id of a MEM operand, or None if absent."""
    r = getattr(op.mem, 'index_reg', getattr(op.mem, 'index', None))
    return r if r != M68K_REG_INVALID else None


def op_str(op):
    """Pretty-print operand for comments."""
    if op.type == M68K_OP_REG:      return reg_name(op.reg)
    if op.type == M68K_OP_IMM:      return f'#{op.imm:#x}'
    if op.type == M68K_OP_MEM:
        b    = mem_base(op)
        disp = op.mem.disp
        if b is not None:
            if disp: return f'{disp}({reg_name(b)})'
            return f'({reg_name(b)})'
        return f'${disp:#x}'
    if op.type == M68K_OP_BR_DISP:  return f'{op.br_disp.disp:+d}'
    return '?'


# ---------------------------------------------------------------------------
# Condition-code map  (mnemonic → RT_CC_xxx suffix)
# ---------------------------------------------------------------------------

CC_MAP = {
    'hi':'HI','ls':'LS','cc':'CC','cs':'CS','ne':'NE','eq':'EQ',
    'vc':'VC','vs':'VS','pl':'PL','mi':'MI','ge':'GE','lt':'LT',
    'gt':'GT','le':'LE','bhi':'HI','bls':'LS','bcc':'CC','bcs':'CS',
    'bne':'NE','beq':'EQ','bvc':'VC','bvs':'VS','bpl':'PL','bmi':'MI',
    'bge':'GE','blt':'LT','bgt':'GT','ble':'LE',
}
