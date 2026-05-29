#!/usr/bin/env python3
"""Unit tests for the recomp emitter (recomp.py Emitter.translate).

Run with:
  python3 -m pytest benefactor-pc/tools/test_recomp.py -v
  # or directly:
  python3 benefactor-pc/tools/test_recomp.py
"""
import sys, os, re
sys.path.insert(0, os.path.dirname(__file__))

from capstone import *
from capstone.m68k import *
from recomp.recomp import Translator

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def make_emitter(func_addrs=()):
    return Translator(func_addrs)

_md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
_md.detail = True

def disasm_one(raw: bytes, addr: int = 0x1000):
    """Disassemble a single instruction from raw bytes at addr."""
    insns = list(_md.disasm(raw, addr))
    assert insns, f"capstone produced no instruction for {raw.hex()}"
    return insns[0]

def translate(raw: bytes, addr: int = 0x1000, func_addrs=()):
    insn = disasm_one(raw, addr)
    e = make_emitter(func_addrs)
    return e.translate(insn)

# ---------------------------------------------------------------------------
# Helper: M68K opcodes
# ---------------------------------------------------------------------------
# subq.w #1, (a2)   → 53 52
SUBQ_W_1_IND_A2   = bytes([0x53, 0x52])
# subq.w #1, d0     → 53 40
SUBQ_W_1_D0       = bytes([0x53, 0x40])
# subq.w #2, $10(a2) → 55 6A 00 10
SUBQ_W_2_DISP_A2  = bytes([0x55, 0x6A, 0x00, 0x10])
# subq.l #1, (a2)   → 53 92
SUBQ_L_1_IND_A2   = bytes([0x53, 0x92])

# addq.w #1, (a2)   → 52 52
ADDQ_W_1_IND_A2   = bytes([0x52, 0x52])
# addq.w #1, d0     → 52 40
ADDQ_W_1_D0       = bytes([0x52, 0x40])
# addq.w #2, $10(a2) → 54 6A 00 10
ADDQ_W_2_DISP_A2  = bytes([0x54, 0x6A, 0x00, 0x10])

# subi.w #5, $10(a2) → 04 6A 00 05 00 10
SUBI_W_5_DISP_A2  = bytes([0x04, 0x6A, 0x00, 0x05, 0x00, 0x10])
# addi.w #5, $10(a2) → 06 6A 00 05 00 10
ADDI_W_5_DISP_A2  = bytes([0x06, 0x6A, 0x00, 0x05, 0x00, 0x10])

# suba.w d0, a0     → 90 C0  (no flags)
SUBA_W_D0_A0      = bytes([0x90, 0xC0])

# ---------------------------------------------------------------------------
# Tests: subq / subi with memory destination
# ---------------------------------------------------------------------------

class TestSubqMemory:
    """The critical correctness invariant:
    For a memory-destination sub/subq/subi, the generated code must:
      1. Read memory ONCE into a local variable (_old_).
      2. Compute result ONCE into a local variable (_res_).
      3. WRITE _res_ to memory.
      4. Compute RT_SUB_FLAGS using _old_ (NOT a second memory read).
    This ensures the Z/N flags reflect the pre-write value, matching M68K semantics.
    """

    def _check_mem_sub_invariants(self, code: str, sz_bits: int):
        # Must declare _old_ and _res_ temp locals
        assert re.search(r'uint\d+_t _old_[0-9a-f]+ =', code), \
            f"Missing _old_ capture in: {code!r}"
        assert re.search(r'uint\d+_t _res_[0-9a-f]+ =', code), \
            f"Missing _res_ capture in: {code!r}"

        # MW (write) must use _res_ as its value, NOT a re-computed expression
        assert re.search(r'MW\d+\(.*?_res_[0-9a-f]+\)', code), \
            f"MW should write _res_, got: {code!r}"

        # RT_SUB_FLAGS must take _old_ as first arg
        assert re.search(rf'RT_SUB_FLAGS_{sz_bits}\(_old_[0-9a-f]+,', code), \
            f"RT_SUB_FLAGS_{sz_bits} should use _old_ as first arg: {code!r}"

        # Must NOT contain a second MR (memory re-read) after the MW
        mw_pos = code.find('MW')
        mr_after = code.find('MR', mw_pos) if mw_pos >= 0 else -1
        assert mr_after == -1, \
            f"Memory re-read (MR) found after MW write in: {code!r}"

    def test_subq_w1_indirect(self):
        code = translate(SUBQ_W_1_IND_A2)
        self._check_mem_sub_invariants(code, 16)

    def test_subq_w2_disp(self):
        code = translate(SUBQ_W_2_DISP_A2)
        self._check_mem_sub_invariants(code, 16)

    def test_subq_l1_indirect(self):
        code = translate(SUBQ_L_1_IND_A2)
        self._check_mem_sub_invariants(code, 32)

    def test_subi_w_disp(self):
        code = translate(SUBI_W_5_DISP_A2)
        self._check_mem_sub_invariants(code, 16)


class TestAddqMemory:
    """Same invariant as subq but for add variants."""

    def _check_mem_add_invariants(self, code: str, sz_bits: int):
        assert re.search(r'uint\d+_t _old_[0-9a-f]+ =', code), \
            f"Missing _old_ capture in: {code!r}"
        assert re.search(r'uint\d+_t _res_[0-9a-f]+ =', code), \
            f"Missing _res_ capture in: {code!r}"
        assert re.search(r'MW\d+\(.*?_res_[0-9a-f]+\)', code), \
            f"MW should write _res_, got: {code!r}"
        assert re.search(rf'RT_ADD_FLAGS_{sz_bits}\(_old_[0-9a-f]+,', code), \
            f"RT_ADD_FLAGS_{sz_bits} should use _old_ as first arg: {code!r}"
        mw_pos = code.find('MW')
        mr_after = code.find('MR', mw_pos) if mw_pos >= 0 else -1
        assert mr_after == -1, \
            f"Memory re-read (MR) found after MW write in: {code!r}"

    def test_addq_w1_indirect(self):
        code = translate(ADDQ_W_1_IND_A2)
        self._check_mem_add_invariants(code, 16)

    def test_addq_w2_disp(self):
        code = translate(ADDQ_W_2_DISP_A2)
        self._check_mem_add_invariants(code, 16)

    def test_addi_w_disp(self):
        code = translate(ADDI_W_5_DISP_A2)
        self._check_mem_add_invariants(code, 16)


# ---------------------------------------------------------------------------
# Tests: subq / addq with register destination (still must be correct)
# ---------------------------------------------------------------------------

class TestSubqRegister:
    """For register destinations no MR/MW involved, but _old_/_res_ are still
    used to ensure the write and flag computation are consistent."""

    def test_subq_w1_d0_writes_reg(self):
        code = translate(SUBQ_W_1_D0)
        # Must still update the register
        assert 'ctx->D[0]' in code, f"Missing register write in: {code!r}"
        # Must still have RT_SUB_FLAGS
        assert 'RT_SUB_FLAGS_16' in code, f"Missing RT_SUB_FLAGS_16 in: {code!r}"
        # _old_ must be the value going into RT_SUB_FLAGS (not re-read expression)
        assert re.search(r'RT_SUB_FLAGS_16\(_old_[0-9a-f]+,', code), \
            f"RT_SUB_FLAGS should use _old_: {code!r}"

    def test_addq_w1_d0_writes_reg(self):
        code = translate(ADDQ_W_1_D0)
        assert 'ctx->D[0]' in code
        assert 'RT_ADD_FLAGS_16' in code
        assert re.search(r'RT_ADD_FLAGS_16\(_old_[0-9a-f]+,', code), \
            f"RT_ADD_FLAGS should use _old_: {code!r}"


# ---------------------------------------------------------------------------
# Tests: suba has NO flags (regression guard)
# ---------------------------------------------------------------------------

class TestSubaNoFlags:
    def test_suba_no_rt_flags(self):
        # suba.w d0, a0 (0x90 0xC0) — suba must never emit RT_SUB_FLAGS
        code = translate(SUBA_W_D0_A0)
        assert 'RT_SUB_FLAGS' not in code, \
            f"suba must not set flags, got: {code!r}"


# ---------------------------------------------------------------------------
# Tests: addq/subq with An register destination — no CCR change (M68K rule)
# ---------------------------------------------------------------------------

class TestAnDestNoFlags:
    """addq/subq/add/sub to An registers never modify CCR on the 68000."""

    def test_addq_l_a0_no_flags(self):
        # addq.l #8, a0 = 50 48
        code = translate(bytes([0x50, 0x48]))
        assert 'RT_ADD_FLAGS' not in code, f"addq An must not set flags: {code!r}"
        assert 'RT_SUB_FLAGS' not in code, f"addq An must not set flags: {code!r}"
        assert 'ctx->A[0]' in code, f"addq An must update A0: {code!r}"

    def test_subq_w_a0_no_flags(self):
        # subq.w #1, a0 = 51 48
        code = translate(bytes([0x51, 0x48]))
        assert 'RT_SUB_FLAGS' not in code, f"subq An must not set flags: {code!r}"
        assert 'ctx->A[0]' in code, f"subq An must update A0: {code!r}"

    def test_addq_w_mem_still_has_flags(self):
        # addq.w #1, (a2) = 52 52 — memory destination DOES set flags
        code = translate(ADDQ_W_1_IND_A2)
        assert 'RT_ADD_FLAGS' in code, f"addq mem must set flags: {code!r}"

    def test_subq_w_mem_still_has_flags(self):
        # subq.w #1, (a2) = 53 52 — memory destination DOES set flags
        code = translate(SUBQ_W_1_IND_A2)
        assert 'RT_SUB_FLAGS' in code, f"subq mem must set flags: {code!r}"


# ---------------------------------------------------------------------------
# Tests: uniqueness of temp variable names across different PCs
# ---------------------------------------------------------------------------

class TestTempVarUniqueness:
    """Each instruction at a different address should get a distinct temp var name."""

    def test_distinct_temps_at_different_pcs(self):
        e = make_emitter()
        insn_a = disasm_one(SUBQ_W_1_IND_A2, addr=0x1000)
        insn_b = disasm_one(SUBQ_W_1_IND_A2, addr=0x2000)
        code_a = e.translate(insn_a)
        code_b = e.translate(insn_b)
        # Extract temp names from each
        names_a = set(re.findall(r'_(?:old|res)_([0-9a-f]+)', code_a))
        names_b = set(re.findall(r'_(?:old|res)_([0-9a-f]+)', code_b))
        assert names_a.isdisjoint(names_b), \
            f"Temp names overlap between PC=0x1000 and PC=0x2000: {names_a & names_b}"


# ---------------------------------------------------------------------------
# Tests: result of computation is correct (not just naming)
# ---------------------------------------------------------------------------

class TestFlagsOnPreWriteValue:
    """The core correctness test: the Z flag should reflect the pre-write value.
    For subq.w #1, mem: if mem==1 → result==0 → Z should be set.
    The generated _res_ must equal (_old_ - imm), not a re-read.
    """

    def test_result_expression_uses_old_not_reread(self):
        code = translate(SUBQ_W_1_IND_A2)
        # _res_ must be defined as (_old_ - 0x1), not as (MR16(...) - 0x1)
        assert re.search(r'_res_[0-9a-f]+ = \(uint\d+_t\)\(_old_[0-9a-f]+ - 0x1\)', code), \
            f"_res_ should be computed from _old_, got: {code!r}"

    def test_add_result_expression_uses_old_not_reread(self):
        code = translate(ADDQ_W_1_IND_A2)
        assert re.search(r'_res_[0-9a-f]+ = \(uint\d+_t\)\(_old_[0-9a-f]+ \+ 0x1\)', code), \
            f"_res_ should be computed from _old_, got: {code!r}"


# ---------------------------------------------------------------------------
# Tests: bra (unconditional branch) to cross-function target must return
# ---------------------------------------------------------------------------

# bra.w $2000 from addr $1000 → disp = 0x2000 - 0x1002 = 0x0FFE
# encoding: 60 00 0F FE
BRA_W_TO_2000 = bytes([0x60, 0x00, 0x0F, 0xFE])

class TestBraCrossFunction:
    """An unconditional bra to another function entry point must terminate the
    current path with return; — not fall through to the next label.  This
    mirrors the M68K semantics: bra is an unconditional jump, it never returns
    to the next instruction."""

    def test_bra_cross_fn_has_return(self):
        # 0x2000 is in func_addrs, so the bra target is a cross-function call
        code = translate(BRA_W_TO_2000, addr=0x1000, func_addrs=(0x2000,))
        assert 'return;' in code, \
            f"bra to cross-function target must emit 'return;', got: {code!r}"
        assert 'rt_call(ctx, 0x002000u);' in code, \
            f"bra to cross-function target must call dispatcher: {code!r}"

    def test_bra_same_fn_is_goto(self):
        # When the target is NOT in func_addrs, bra should be a goto (no call)
        code = translate(BRA_W_TO_2000, addr=0x1000, func_addrs=())
        assert 'goto' in code, \
            f"bra within same function should emit goto, got: {code!r}"
        assert 'gfn_' not in code, \
            f"bra within same function must not call gfn_, got: {code!r}"


# ---------------------------------------------------------------------------
# Tests: conditional branch (bne/beq/etc.) to cross-function target must return
# ---------------------------------------------------------------------------
# bne.b $1010 from addr $1000 → disp = $0E (14); target = $1002 + $0E = $1010
BNE_B_TO_1010 = bytes([0x66, 0x0E])
# beq.b $1010 from addr $1000 → same target, different condition
BEQ_B_TO_1010 = bytes([0x67, 0x0E])

class TestBneCrossFunction:
    """A conditional branch (bne/beq) to another function entry point must call
    gfn_<target>(ctx) AND then return; — the current function must not fall
    through to the next instruction when the branch is taken.

    M68K semantics: a branch (not BSR) pushes no return address, so the
    target function's RTS returns to the original caller of the *current*
    function, not back to the instruction after the branch.  The C translation
    must mirror this by returning after the cross-function call."""

    def test_bne_cross_fn_has_return(self):
        # $1010 is in func_addrs → conditional cross-function branch
        code = translate(BNE_B_TO_1010, addr=0x1000, func_addrs=(0x1010,))
        assert 'return;' in code, \
            f"bne to cross-function target must emit 'return;', got: {code!r}"
        assert 'rt_call(ctx, 0x001010u);' in code, \
            f"bne to cross-function target must call dispatcher: {code!r}"
        assert 'RT_CC_NE' in code or 'if' in code, \
            f"bne must be conditional (if/RT_CC_NE), got: {code!r}"

    def test_beq_cross_fn_has_return(self):
        # Same test for beq to ensure the fix is not bne-specific
        code = translate(BEQ_B_TO_1010, addr=0x1000, func_addrs=(0x1010,))
        assert 'return;' in code, \
            f"beq to cross-function target must emit 'return;', got: {code!r}"
        assert 'rt_call(ctx, 0x001010u);' in code, \
            f"beq to cross-function target must call dispatcher: {code!r}"

    def test_jsr_cross_fn_uses_dispatcher(self):
        code = translate(bytes([0x4E, 0xB9, 0x00, 0x00, 0x20, 0x00]), addr=0x1000, func_addrs=(0x2000,))
        assert 'rt_call(ctx,' in code and '0x2000' in code, \
            f"jsr to cross-function target must call dispatcher: {code!r}"

    def test_bne_same_fn_is_goto(self):
        # Target NOT in func_addrs → should be a conditional goto (no call, no return)
        code = translate(BNE_B_TO_1010, addr=0x1000, func_addrs=())
        assert 'goto' in code, \
            f"bne within same function should emit goto, got: {code!r}"
        assert 'gfn_' not in code, \
            f"bne within same function must not call gfn_, got: {code!r}"
        assert 'return;' not in code, \
            f"bne within same function must not emit return, got: {code!r}"


# ---------------------------------------------------------------------------
# Tests: movem instruction — register list load/store
# ---------------------------------------------------------------------------

# movem.l (a3)+, a0-a1  = 4C DB 03 00  (load, post-increment)
MOVEM_L_A3PI_A0A1   = bytes([0x4C, 0xDB, 0x03, 0x00])
# movem.w (a3)+, d6-d7  = 4C 9B 00 C0  (load word, post-increment)
MOVEM_W_A3PI_D6D7   = bytes([0x4C, 0x9B, 0x00, 0xC0])
# movem.l a5-a6, -(a7)  = 48 E7 00 06  (store, pre-decrement)
MOVEM_L_A5A6_A7PD   = bytes([0x48, 0xE7, 0x00, 0x06])
# movem.l (a7)+, a5-a6  = 4C DF 60 00  (load, post-increment from stack)
MOVEM_L_A7PI_A5A6   = bytes([0x4C, 0xDF, 0x60, 0x00])
# movem.l d3-d4, $4(a1) = 48 E9 00 18 00 04  (store, d16 EA)
MOVEM_L_D3D4_D16A1  = bytes([0x48, 0xE9, 0x00, 0x18, 0x00, 0x04])
# movem.w (a0), d0/d2/d4 = 4C 90 00 15  (load word, indirect EA)
MOVEM_W_A0_D0D2D4   = bytes([0x4C, 0x90, 0x00, 0x15])


class TestMovem:
    """movem load/store — register list should be transferred correctly."""

    def test_movem_l_postinc_load(self):
        # (a3)+, a0-a1: load A0 from [A3], advance A3; load A1, advance A3
        code = translate(MOVEM_L_A3PI_A0A1)
        assert 'MR32' in code, f"movem load must use MR32: {code!r}"
        assert 'A[0]' in code and 'A[1]' in code, f"A0 and A1 must appear: {code!r}"
        assert 'A[3]' in code, f"base register A3 must appear: {code!r}"
        assert '+= 4' in code, f"post-increment by 4 must appear: {code!r}"

    def test_movem_w_postinc_load(self):
        # (a3)+, d6-d7: sign-extend word loads into D6, D7
        code = translate(MOVEM_W_A3PI_D6D7)
        assert 'MR16' in code, f"movem.w load must use MR16: {code!r}"
        assert 'D[6]' in code and 'D[7]' in code, f"D6 and D7 must appear: {code!r}"
        assert '+= 2' in code, f"post-increment by 2 must appear: {code!r}"
        assert 'RT_SX16' in code, f"word load must use RT_SX16 sign-extend macro: {code!r}"

    def test_movem_l_predec_store(self):
        # a5-a6, -(a7): pre-decrement store, reverse order (A6 first)
        code = translate(MOVEM_L_A5A6_A7PD)
        assert 'MW32' in code, f"movem store must use MW32: {code!r}"
        assert 'A[5]' in code and 'A[6]' in code, f"A5 and A6 must appear: {code!r}"
        assert '-= 4' in code, f"pre-decrement by 4 must appear: {code!r}"
        # A6 must be stored before A5 (reverse order for -(An))
        assert code.index('A[6]') < code.index('A[5]'), \
            f"A6 must be stored before A5 (reversed): {code!r}"

    def test_movem_l_predec_restore(self):
        # (a7)+, a5-a6: restore A5 from stack, then A6
        code = translate(MOVEM_L_A7PI_A5A6)
        assert 'MR32' in code, f"movem load must use MR32: {code!r}"
        assert 'A[5]' in code and 'A[6]' in code, f"A5 and A6 must appear: {code!r}"
        # A5 loaded first, then A6
        assert code.index('A[5]') < code.index('A[6]'), \
            f"A5 must be restored before A6: {code!r}"

    def test_movem_l_d16_store(self):
        # d3-d4, $4(a1): store D3 and D4 to fixed address $4(a1)
        code = translate(MOVEM_L_D3D4_D16A1)
        assert 'MW32' in code, f"movem store must use MW32: {code!r}"
        assert 'D[3]' in code and 'D[4]' in code, f"D3 and D4 must appear: {code!r}"
        assert 'A[1]' in code, f"base register A1 must appear: {code!r}"

    def test_movem_w_indirect_load(self):
        # (a0), d0/d2/d4: load words at sequential offsets from (a0)
        code = translate(MOVEM_W_A0_D0D2D4)
        assert 'MR16' in code, f"movem.w load must use MR16: {code!r}"
        assert 'D[0]' in code and 'D[2]' in code and 'D[4]' in code, \
            f"D0, D2, D4 must appear: {code!r}"
        assert 'RT_SX16' in code, f"word load must use RT_SX16 sign-extend macro: {code!r}"


# ---------------------------------------------------------------------------
# Tests: addq/subq to (An)+ destination — RMW must not double-increment
# ---------------------------------------------------------------------------

# addq.w #1, (a0)+ = 0x5258
ADDQ_W_1_POSTINC_A0  = bytes([0x52, 0x58])
# subq.w #1, (a0)+ = 0x5358
SUBQ_W_1_POSTINC_A0  = bytes([0x53, 0x58])

class TestRMWAutoIncrement:
    """addq/subq to (An)+ must read and write the same address, increment once."""

    def _check_single_increment(self, code: str, reg: int, sz: int):
        """Verify A[reg] is advanced exactly once (appears once as += sz)."""
        import re as _re
        advances = _re.findall(rf'ctx->A\[{reg}\] \+= {sz}', code)
        assert len(advances) == 1, \
            f"Expected single A[{reg}] += {sz}, found {len(advances)} in: {code!r}"

    def _check_same_addr_for_read_and_write(self, code: str):
        """Verify the MW address variable is the same captured _rmw_addr_ used by MR."""
        # After the fix: _rmw_addr_XXXX is captured, MR uses it, MW uses it
        import re as _re
        addr_cap = _re.search(r'uint32_t (_rmw_addr_[0-9a-f]+) = ctx->A\[\d+\]', code)
        assert addr_cap, f"Missing _rmw_addr_ capture in: {code!r}"
        addr_var = addr_cap.group(1)
        assert f'MR16({addr_var})' in code or f'MR32({addr_var})' in code, \
            f"MR should use {addr_var} in: {code!r}"
        assert f'MW16({addr_var}' in code or f'MW32({addr_var}' in code, \
            f"MW should use {addr_var} in: {code!r}"

    def test_addq_w_postinc_single_increment(self):
        code = translate(ADDQ_W_1_POSTINC_A0)
        self._check_single_increment(code, 0, 2)
        self._check_same_addr_for_read_and_write(code)
        assert 'RT_ADD_FLAGS_16' in code

    def test_subq_w_postinc_single_increment(self):
        code = translate(SUBQ_W_1_POSTINC_A0)
        self._check_single_increment(code, 0, 2)
        self._check_same_addr_for_read_and_write(code)
        assert 'RT_SUB_FLAGS_16' in code


# ---------------------------------------------------------------------------
# Division instructions: divs.w / divu.w
# ---------------------------------------------------------------------------

# divs.w d2, d0 = 0x81C2
DIVS_W_D2_D0 = bytes([0x81, 0xC2])
# divu.w d2, d0 = 0x80C2
DIVU_W_D2_D0 = bytes([0x80, 0xC2])

class TestDivision:
    """divs.w / divu.w must emit a working signed/unsigned division with quotient+remainder packing."""

    def test_divs_emits_signed_division(self):
        code = translate(DIVS_W_D2_D0)
        # Must NOT be a UNK comment
        assert 'UNK' not in code, f"divs.w emitted as UNK: {code!r}"
        # Must contain a signed division operator
        assert '_dvd / _dvsr' in code or '/ _dvsr' in code, f"No division in: {code!r}"
        # Must pack quotient in low 16 bits and remainder in high 16 bits
        assert '<< 16' in code, f"Remainder shift missing in: {code!r}"
        assert '(uint16_t)_quot' in code, f"Quotient truncation missing in: {code!r}"
        # Must use int32_t for signed division
        assert 'int32_t' in code, f"Signed type missing in: {code!r}"
        # Must set NZ flags from quotient
        assert 'RT_SET_NZ_16' in code, f"NZ flags missing in: {code!r}"

    def test_divu_emits_unsigned_division(self):
        code = translate(DIVU_W_D2_D0)
        assert 'UNK' not in code, f"divu.w emitted as UNK: {code!r}"
        assert '/ _dvsr' in code, f"No division in: {code!r}"
        assert '<< 16' in code, f"Remainder shift missing in: {code!r}"
        assert '(uint16_t)_quot' in code, f"Quotient truncation missing in: {code!r}"
        # Must use uint32_t for unsigned division
        assert 'uint32_t' in code, f"Unsigned type missing in: {code!r}"
        assert 'RT_SET_NZ_16' in code, f"NZ flags missing in: {code!r}"


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

# ext.l d0 = $48C0, ext.l d1 = $48C1, ext.w d0 = $4880
EXT_L_D0 = bytes([0x48, 0xC0])
EXT_L_D1 = bytes([0x48, 0xC1])
EXT_W_D0 = bytes([0x48, 0x80])

class TestExtInstruction:
    def test_ext_l_sign_extends_word_to_long(self):
        code = translate(EXT_L_D0)
        assert 'UNK' not in code, f"ext.l emitted as UNK: {code!r}"
        # Must sign-extend low 16 bits to 32 bits via RT_SX16 macro
        assert 'RT_SX16' in code, f"RT_SX16 macro missing in: {code!r}"
        # Must target D0
        assert 'D[0]' in code, f"D0 not targeted in: {code!r}"
        # Must set NZ flags
        assert 'RT_SET_NZ_32' in code, f"NZ flags missing in: {code!r}"
        assert 'ctx->V = ctx->C = 0' in code, f"VC clear missing in: {code!r}"
        code = translate(EXT_L_D1)
        assert 'UNK' not in code, f"ext.l d1 emitted as UNK: {code!r}"
        assert 'D[1]' in code, f"D1 not targeted in: {code!r}"

    def test_ext_w_sign_extends_byte_to_word(self):
        code = translate(EXT_W_D0)
        assert 'UNK' not in code, f"ext.w emitted as UNK: {code!r}"
        # Must sign-extend low 8 bits to 16 bits via RT_SX8W macro, preserve high 16
        assert 'RT_SX8W' in code, f"RT_SX8W macro missing in: {code!r}"
        assert 'RT_SET_NZ_16' in code, f"NZ16 flags missing in: {code!r}"
        assert 'ctx->V = ctx->C = 0' in code, f"VC clear missing in: {code!r}"


# ---------------------------------------------------------------------------
# Tests: no double-evaluation of memory reads in cmp/and/or/not/lsl/mulu
# ---------------------------------------------------------------------------

# cmp.w 2(a1), d0  → B0 69 00 02  (CMP.W EA,Dn: src=2(a1), dst=D0)
CMP_W_DISP_A1_D0  = bytes([0xB0, 0x69, 0x00, 0x02])
# and.w 2(a0), d1  → C2 68 00 02  (AND.W EA,Dn: src=2(a0), dst=D1)
AND_W_DISP_A0_D1  = bytes([0xC2, 0x68, 0x00, 0x02])
# not.w 2(a0)      → 46 68 00 02
NOT_W_DISP_A0     = bytes([0x46, 0x68, 0x00, 0x02])
# lsl.w #1, d0    → E3 48
LSL_W_1_D0        = bytes([0xE3, 0x48])
# lsr.w #1, d0    → E2 48
LSR_W_1_D0        = bytes([0xE2, 0x48])
# mulu.w d1, d0   → C0 C1
MULU_W_D1_D0      = bytes([0xC0, 0xC1])
# muls.w d1, d0   → C1 C1
MULS_W_D1_D0      = bytes([0xC1, 0xC1])


class TestNoDoubleEval:
    """Verify that memory reads and result expressions are not evaluated twice.

    The double-eval problem: if an operand is MR16(addr), the old emitter used
    that expression both in the write and in the flags — computing the memory
    read twice.  This is:
      1. A correctness bug if the memory address has side effects
      2. A semantic bug for lsl/lsr: flags read the *updated* register value,
         not the pre-shift value, producing double-shifted flag results
      3. Unreadable and hard to audit

    All affected handlers must capture into typed locals and use those locals.
    """

    @staticmethod
    def _mr_count(code: str) -> int:
        return len(re.findall(r'MR(?:8|16|32)\(', code))

    def test_cmp_w_no_double_mr(self):
        """cmp.w must not read memory twice."""
        code = translate(CMP_W_DISP_A1_D0)
        assert self._mr_count(code) <= 1, \
            f"MR appears {self._mr_count(code)} times (double-eval): {code!r}"
        assert 'RT_SUB_FLAGS_16' in code, f"Missing RT_SUB_FLAGS_16: {code!r}"

    def test_cmp_w_uses_typed_locals(self):
        """cmp.w must capture operands into named typed locals."""
        code = translate(CMP_W_DISP_A1_D0)
        assert '_s' in code, f"Missing _s local: {code!r}"
        assert '_d' in code, f"Missing _d local: {code!r}"
        assert 'RT_SUB_FLAGS_16(_d, _s,' in code, \
            f"RT_SUB_FLAGS should use _d, _s: {code!r}"

    def test_and_w_no_double_mr(self):
        """and.w mem, Dn — MR for the source must appear exactly once."""
        code = translate(AND_W_DISP_A0_D1)
        assert self._mr_count(code) <= 1, \
            f"MR appears {self._mr_count(code)} times in AND: {code!r}"
        assert 'RT_SET_NZ_16' in code, f"Missing NZ flags: {code!r}"

    def test_and_w_result_in_local(self):
        """and.w result must be captured before write and NZ flags."""
        code = translate(AND_W_DISP_A0_D1)
        assert re.search(r'uint\d+_t _res_[0-9a-f]+', code), \
            f"Missing _res_ local: {code!r}"
        assert re.search(r'RT_SET_NZ_16\(_res_[0-9a-f]+\)', code), \
            f"NZ flags should reference _res_: {code!r}"

    def test_not_w_no_double_mr(self):
        """not.w must not read memory twice (once for invert, once for flags)."""
        code = translate(NOT_W_DISP_A0)
        assert self._mr_count(code) <= 1, \
            f"MR appears {self._mr_count(code)} times in NOT: {code!r}"
        assert 'RT_SET_NZ_16' in code, f"Missing NZ flags: {code!r}"

    def test_not_w_flags_from_result(self):
        """not.w: NZ flags must reflect ~v (the result), not the original v."""
        code = translate(NOT_W_DISP_A0)
        assert re.search(r'uint\d+_t _res_[0-9a-f]+', code), \
            f"Missing _res_ local: {code!r}"
        assert re.search(r'RT_SET_NZ_16\(_res_[0-9a-f]+\)', code), \
            f"NZ flags should reference _res_ (the result ~v), not re-read memory: {code!r}"

    def test_lsl_result_in_local(self):
        """lsl: shift result must be captured before write to prevent double-shift in flags."""
        code = translate(LSL_W_1_D0)
        assert re.search(r'uint\d+_t _res_[0-9a-f]+', code), \
            f"Missing _res_ local (lsl would double-shift flags without it): {code!r}"
        assert re.search(r'RT_SET_NZ_16\(_res_[0-9a-f]+\)', code), \
            f"NZ flags should reference _res_, not the (already-shifted) register: {code!r}"

    def test_lsr_result_in_local(self):
        """lsr: same double-eval guard as lsl."""
        code = translate(LSR_W_1_D0)
        assert re.search(r'uint\d+_t _res_[0-9a-f]+', code), \
            f"Missing _res_ local: {code!r}"
        assert re.search(r'RT_SET_NZ_16\(_res_[0-9a-f]+\)', code), \
            f"NZ flags should reference _res_: {code!r}"

    def test_mulu_result_in_local(self):
        """mulu: product must be captured once for both write and NZ flags."""
        code = translate(MULU_W_D1_D0)
        assert re.search(r'uint32_t _res_[0-9a-f]+', code), \
            f"Missing _res_ local: {code!r}"
        assert re.search(r'RT_SET_NZ_32\(_res_[0-9a-f]+\)', code), \
            f"NZ flags should reference _res_: {code!r}"

    def test_muls_result_in_local(self):
        """muls: same double-eval guard as mulu."""
        code = translate(MULS_W_D1_D0)
        assert re.search(r'uint32_t _res_[0-9a-f]+', code), \
            f"Missing _res_ local: {code!r}"
        assert re.search(r'RT_SET_NZ_32\(_res_[0-9a-f]+\)', code), \
            f"NZ flags should reference _res_: {code!r}"


# ---------------------------------------------------------------------------
# Tests: adda/suba/movea/cmpa 32-bit address register semantics
# ---------------------------------------------------------------------------
# adda.w #$fa0, a1  → D2 FC 0F A0  (adda.w immediate to a1)
ADDA_W_FA0_A1     = bytes([0xD2, 0xFC, 0x0F, 0xA0])
# adda.l #$fa0, a1  → D3 C1 00 00 0F A0  (but actually adda.l imm is different encoding)
# adda.w d0, a1     → D2 C0
ADDA_W_D0_A1      = bytes([0xD2, 0xC0])
# suba.w d0, a1     → 92 C0  (wait: suba.w d0, a1 = 9x C0? Let me recalculate)
# suba.w d0, a0     → 90 C0
# movea.w d0, a1    → 32 40
MOVEA_W_D0_A1     = bytes([0x32, 0x40])
# movea.l d0, a1    → 22 40
MOVEA_L_D0_A1     = bytes([0x22, 0x40])
# cmpa.w d0, a1     → B2 C0  (cmpa.w d0, a1)
CMPA_W_D0_A1      = bytes([0xB2, 0xC0])
# cmpa.l d0, a1     → B3 C0? Actually cmpa.l d0, a1 = B2 C1... let me check
# cmpa.l #$417a, a2  : B5 FC 00 00 41 7A
CMPA_L_IMM_A2     = bytes([0xB5, 0xFC, 0x00, 0x00, 0x41, 0x7A])


class TestAddressRegister32BitSemantics:
    """68000 address register operations always use full 32-bit semantics:
    - adda/suba: source sign-extended from sz, full 32-bit add/sub to An
    - movea: source sign-extended from sz, written to full 32-bit An
    - cmpa: source sign-extended from sz, 32-bit comparison with An
    These must NOT use the (An & 0xFFFF0000) | uint16_t(...) pattern.
    """

    def test_adda_w_imm_full_32bit(self):
        """adda.w #$fa0, a1 must do full 32-bit add, not 16-bit truncation."""
        code = translate(ADDA_W_FA0_A1)
        # Must NOT use the 16-bit truncation pattern
        assert '0xFFFF0000' not in code, \
            f"adda.w must not truncate to 16-bit: {code!r}"
        # Must use RT_SX16 sign-extension for the word source
        assert 'RT_SX16' in code, \
            f"adda.w word source must use RT_SX16: {code!r}"
        # Must assign to ctx->A[1] (now as +=)
        assert 'ctx->A[1] +=' in code, \
            f"adda.w must write full 32-bit result via +=: {code!r}"

    def test_adda_w_reg_full_32bit(self):
        """adda.w d0, a1 must do full 32-bit add."""
        code = translate(ADDA_W_D0_A1)
        assert '0xFFFF0000' not in code, \
            f"adda.w must not truncate to 16-bit: {code!r}"
        assert 'ctx->A[1] +=' in code, \
            f"adda.w must write full 32-bit result via +=: {code!r}"

    def test_movea_w_sign_extends(self):
        """movea.w d0, a1 must sign-extend d0.word to 32-bit."""
        code = translate(MOVEA_W_D0_A1)
        # Must NOT use 16-bit mask pattern
        assert '0xFFFF0000' not in code, \
            f"movea.w must not truncate to 16-bit: {code!r}"
        # Must use RT_SX16 sign-extension for the source
        assert 'RT_SX16' in code, \
            f"movea.w must use RT_SX16 to sign-extend source: {code!r}"
        assert 'ctx->A[1] = RT_SX16' in code, \
            f"movea.w must write via RT_SX16: {code!r}"

    def test_movea_l_no_sign_extend_needed(self):
        """movea.l d0, a1 writes full 32-bit value directly."""
        code = translate(MOVEA_L_D0_A1)
        assert 'ctx->A[1] = (uint32_t)' in code, \
            f"movea.l must write full 32-bit: {code!r}"

    def test_cmpa_w_uses_32bit_compare(self):
        """cmpa.w d0, a1 must compare full 32-bit An with sign-extended d0."""
        code = translate(CMPA_W_D0_A1)
        # Must use 32-bit flags
        assert 'RT_SUB_FLAGS_32' in code, \
            f"cmpa.w must use 32-bit comparison: {code!r}"
        # Must use RT_SX16 sign-extension for the source
        assert 'RT_SX16' in code, \
            f"cmpa.w source must use RT_SX16: {code!r}"

    def test_cmpa_l_uses_32bit_compare(self):
        """cmpa.l #imm, a2 must use 32-bit comparison."""
        code = translate(CMPA_L_IMM_A2)
        assert 'RT_SUB_FLAGS_32' in code, \
            f"cmpa.l must use 32-bit comparison: {code!r}"


if __name__ == '__main__':
    import traceback
    failures = []
    test_classes = [
        TestSubqMemory,
        TestAddqMemory,
        TestSubqRegister,
        TestSubaNoFlags,
        TestAnDestNoFlags,
        TestTempVarUniqueness,
        TestFlagsOnPreWriteValue,
        TestBraCrossFunction,
        TestBneCrossFunction,
        TestMovem,
        TestRMWAutoIncrement,
        TestDivision,
        TestExtInstruction,
        TestNoDoubleEval,
        TestAddressRegister32BitSemantics,
    ]
    total = 0
    for cls in test_classes:
        obj = cls()
        for name in [n for n in dir(obj) if n.startswith('test_')]:
            total += 1
            method = getattr(obj, name)
            try:
                method()
                print(f"  PASS  {cls.__name__}.{name}")
            except Exception as e:
                failures.append(f"{cls.__name__}.{name}")
                print(f"  FAIL  {cls.__name__}.{name}")
                traceback.print_exc()
    print(f"\n{total - len(failures)}/{total} passed", end='')
    if failures:
        print(f", {len(failures)} failed: {failures}")
        sys.exit(1)
    else:
        print()
