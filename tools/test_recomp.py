#!/usr/bin/env python3
"""Test suite for the Benefactor recompiler — `python3 tools/test_recomp.py`.

All tests run in well under a second (no regeneration); run after every change.

  1. UNIT      — pure helpers, no inputs: register naming, seed-tree parsing,
                 single-instruction translation, _scan_func / _terminates /
                 _emittable / handler-prologue detection on hand-built bytes.
  2. ARTIFACT  — invariants on the COMMITTED generated bank: no untranslatable
                 UNK_R_/ctx->PC; every literal rt_call/rt_jump target in the gpl
                 region resolves to a real function. These are the regressions
                 that bit us (UNK_R_27 from misdecoded ccr data, the
                 $5772A8 / $59B80A dangling dispatch targets).

The ARTIFACT scope reads src/generated and skips cleanly when it isn't present
(e.g. a checkout without the disks). Verifying that a regeneration reproduces
the bank is a manual step (regenerate, then run the harness), not a unit test.
"""
import os
import re
import sys
import glob
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000, CS_MODE_BIG_ENDIAN
from capstone.m68k import M68K_REG_CCR

from recomp.helpers import reg_name
from recomp.emitter import Translator
from recomp import scanner
from recomp.seeds_loader import load_seed_tree

GEN = os.path.join(ROOT, "src", "generated")
GPL_LO, GPL_HI = 0x577000, 0x59F000
# Two known data artifacts whose rt_jump comes from a misdecoded branch in a
# never-executed region; they reference pc-rel-with-index / raw data so they
# can't (and needn't) be registered. See check_unregistered_targets.py.
KNOWN_ARTIFACTS = {0x57746C, 0x59E9B6}

_MD = Cs(CS_ARCH_M68K, CS_MODE_M68K_000 | CS_MODE_BIG_ENDIAN)
_MD.detail = True


def insn(hexbytes, addr=0x1000):
    """Disassemble one instruction from a hex string (big-endian m68k word(s))."""
    return next(_MD.disasm(bytes.fromhex(hexbytes.replace(" ", "")), addr))


def insns(hexbytes, addr=0x1000):
    return list(_MD.disasm(bytes.fromhex(hexbytes.replace(" ", "")), addr))


# ===========================================================================
# Scope 1 — UNIT
# ===========================================================================

class TestRegNaming(unittest.TestCase):
    def test_data_address_pc_sr(self):
        self.assertEqual(reg_name(scanner.M68K_REG_D0), "D[0]")
        self.assertEqual(reg_name(scanner.M68K_REG_A7), "A[7]")
        self.assertEqual(reg_name(scanner.M68K_REG_PC), "PC")
        self.assertEqual(reg_name(scanner.M68K_REG_SR), "SR")

    def test_unmapped_register_is_flagged(self):
        # CCR/FP/control regs aren't emitter fields -> UNK_R_, which won't
        # compile. The recompiler must never silently accept them.
        self.assertTrue(reg_name(M68K_REG_CCR).startswith("UNK_R_"))


class TestSeedTree(unittest.TestCase):
    def test_parse_names_and_multi_address_lines(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "a.txt"), "w") as f:
                f.write("# comment\n")
                f.write("577000  game_overlay_entry   # entry\n")
                f.write("586E40\n")
                f.write("596212 596224 596236  # three bare addrs\n")
            addrs, names = load_seed_tree(d)
        self.assertEqual(addrs,
                         [0x577000, 0x586E40, 0x596212, 0x596224, 0x596236])
        self.assertEqual(names[0x577000], "game_overlay_entry")
        # bare addresses carry no name
        self.assertNotIn(0x586E40, names)
        self.assertNotIn(0x596224, names)

    def test_name_sanitized_to_identifier(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "a.txt"), "w") as f:
                f.write("577000  Card-State!Dispatch\n")
            _addrs, names = load_seed_tree(d)
        self.assertEqual(names[0x577000], "card_state_dispatch")


class TestTranslate(unittest.TestCase):
    def setUp(self):
        self.t = Translator(set())

    def test_moveq(self):
        out = self.t.translate(insn("7001"))          # moveq #1,d0
        self.assertIn("ctx->D[0]", out)

    def test_addq_word(self):
        out = self.t.translate(insn("5240"))          # addq.w #1,d0
        self.assertIn("ctx->D[0]", out)
        self.assertIn("ADD_FLAGS_16", out.replace(" ", "").upper().replace("RT_", ""))

    def test_rts_returns(self):
        self.assertEqual(self.t.translate(insn("4e75")).strip(), "return;")

    def test_jsr_absolute_is_rt_call(self):
        out = self.t.translate(insn("4eb9 0057 7000"))  # jsr $577000.l
        self.assertIn("rt_call(ctx, 0x577000", out)

    def test_jmp_absolute_is_rt_jump(self):
        out = self.t.translate(insn("4ef9 0057 7000"))  # jmp $577000.l
        self.assertIn("rt_jump(ctx, 0x577000", out)


class TestScanner(unittest.TestCase):
    def test_scan_simple_leaf(self):
        # moveq #0,d0 ; rts  (padded — _disasm_one reads a 4-byte lookahead).
        ins, end = scanner._scan_func(bytes.fromhex("70004e75") + b"\0" * 16, 0, 0, _MD)
        self.assertEqual([i.mnemonic for i in ins], ["moveq", "rts"])
        self.assertEqual(end, 4)

    def test_terminates(self):
        self.assertTrue(scanner._terminates(insns("4e75")))         # rts
        self.assertFalse(scanner._terminates(insns("7001")))        # moveq (none)

    def test_emittable_rejects_ccr(self):
        # ori.b #0,ccr ($003C 0000) — the misdecoded-data signature that emits
        # ctx->UNK_R_27. _emittable must reject it so gap-fill never accepts it.
        self.assertFalse(scanner._emittable(insns("003c 0000")))
        self.assertTrue(scanner._emittable(insns("7001")))          # moveq d0

    def test_discover_handler_prologue(self):
        # movem.w (a0),d0-d1  ($4C90 reglist) at gpl-range addr -> discovered.
        data = bytearray(0x600000)
        body = bytes.fromhex("4c90 0003 4e75")   # movem.w (a0),d0/d1 ; rts
        at = 0x580000
        data[at:at + len(body)] = body
        found = scanner.discover_object_handlers(bytes(data), 0, _MD,
                                                 0x577000, 0x59F000)
        self.assertIn(at, found)

    # --- This game's object-handler ABI: tail-call epilogue + inline jump table.
    # Benefactor's $57D3EC-dispatched handlers don't end in rts; they tail-call
    # back into the dispatcher (`jmp d(a5)`) and embed a pc-relative jump table
    # in the body, with execution resuming at a later case label that branches
    # OVER the data (e.g. $599BB2). The scanner must skip the unreachable inline
    # data to the next label, not decode it as code (which fails _terminates and
    # made the prologue-pin discard real handlers -> the $599BB2 dispatch miss).
    _TAILCALL_HANDLER = (
        "4c90 0001"   # 1000: movem.w (a0),d0          (handler prologue)
        "6700 000a"   # 1004: beq.w   $1010            (jump OVER inline data)
        "4ee9 fff0"   # 1008: jmp     -$10(a5)         (tail-call to dispatcher)
        "ffff ffff"   # 100c: inline jump-table DATA (unreachable; not code)
        "7000 4e75")  # 1010: moveq #0,d0 ; rts        (resumed case body)

    def test_scan_skips_inline_data_after_tailcall(self):
        ins, _end = scanner._scan_func(
            bytes.fromhex(self._TAILCALL_HANDLER.replace(" ", "")) + b"\0" * 16,
            0, 0, _MD)
        self.assertEqual([i.mnemonic.split(".")[0] for i in ins],
                         ["movem", "beq", "jmp", "moveq", "rts"])
        self.assertTrue(scanner._terminates(ins))

    def test_discover_tailcall_handler(self):
        data = bytearray(0x600000)
        at = 0x580000
        body = bytes.fromhex(self._TAILCALL_HANDLER.replace(" ", ""))
        data[at:at + len(body)] = body
        found = scanner.discover_object_handlers(bytes(data), 0, _MD,
                                                 0x577000, 0x59F000)
        self.assertIn(at, found)


class TestEdgeTargets(unittest.TestCase):
    """The single edge rule that the scanner and emitter must agree on."""
    LO, HI = 0x577000, 0x600000

    def _edges(self, hexbytes, addr, source_addrs=frozenset(), areg=None):
        i = insn(hexbytes, addr)
        return set(scanner._edge_targets(i, set(source_addrs), self.LO, self.HI,
                                         areg or {}, 'gpl'))

    def test_abs_long_jmp_is_followed(self):
        # jmp $577384.l — capstone M68K_OP_MEM (addr_mode 17), target in op.imm.
        # The descent must follow it (the emitter does); missing it left $577384
        # dangling.
        self.assertEqual(self._edges("4ef9 00577384", 0x580000), {0x577384})

    def test_bsr_target_registers_even_inside_source(self):
        # bsr is a CALL -> always rt_call; the target must register even when it
        # falls inside the source function's own range ($57C79E was such a case).
        i_addr = 0x580000
        tgt = i_addr + 2 + 0x10
        edges = self._edges("6110", i_addr, source_addrs={i_addr, tgt})  # bsr +$10
        self.assertIn(tgt, edges)

    def test_local_bcc_is_not_an_edge(self):
        # A Bcc whose target is one of the function's own instructions is a LOCAL
        # goto (emit_branch), not a cross-function rt_jump — so it is NOT a forced
        # entry and must not be yielded.
        i_addr = 0x580000
        tgt = i_addr + 2 + 0x10
        self.assertEqual(self._edges("6710", i_addr, source_addrs={i_addr, tgt}),
                         set())                                   # beq.b +$10
        # but an ESCAPING Bcc (target not in source) IS a cross-fn rt_jump.
        self.assertEqual(self._edges("6710", i_addr, source_addrs={i_addr}),
                         {tgt})


# ===========================================================================
# Scope 2 — ARTIFACT invariants on the committed generated bank
# ===========================================================================

def _gpl_present():
    return os.path.exists(os.path.join(GEN, "game_gpl_table.c"))


def _read(path):
    with open(path) as f:
        return f.read()


def _gpl_sources():
    return "".join(_read(f)
                   for f in sorted(glob.glob(os.path.join(GEN, "game_gpl_[0-9]*.c"))))


def _registered_addrs():
    txt = _read(os.path.join(GEN, "game_gpl_table.c"))
    return set(int(x, 16) for x in re.findall(r'\{\s*0x([0-9A-Fa-f]{6})u,', txt))


@unittest.skipUnless(_gpl_present(), "generated gpl bank not present")
class TestGeneratedBank(unittest.TestCase):
    def test_no_untranslatable_symbols(self):
        src = _gpl_sources()
        self.assertNotIn("UNK_R_", src, "emitted an unmapped register field")
        self.assertNotIn("ctx->PC", src, "emitted ctx->PC (pc-rel-with-index)")

    def test_no_dangling_dispatch_targets(self):
        # Every constant rt_call/rt_jump target in the gpl region must resolve to
        # a registered function — else the runtime aborts "NO FUNCTION at $X".
        src = _gpl_sources()
        reg = _registered_addrs()
        targets = set(int(x, 16) for x in
                      re.findall(r'rt_(?:jump|call)\(ctx, 0x([0-9A-Fa-f]+)u\)', src))
        dangling = sorted(t for t in targets
                          if t not in reg and GPL_LO <= t < GPL_HI
                          and t not in KNOWN_ARTIFACTS)
        self.assertEqual(dangling, [],
                         "unregistered dispatch targets: "
                         + ", ".join("$%06X" % t for t in dangling))

    def test_table_and_defs_consistent(self):
        reg = _registered_addrs()
        defs = set(int(x, 16) for x in
                   re.findall(r'void gfn_gpl_([0-9A-F]{6})', _gpl_sources()))
        self.assertEqual(reg, defs, "table entries and function defs disagree")


if __name__ == "__main__":
    unittest.main(verbosity=2)
