#!/usr/bin/env python3
"""Find literal rt_jump/rt_call targets in the generated gpl bank that aren't
registered functions — i.e. constant branch/call destinations that will
`rt_call: NO FUNCTION ... aborting` if the path is ever taken.

The gameplay engine embeds jump-offset/data tables inside functions, so the
linear decode lands an instruction boundary off from where the original code
branches; the emitter then emits a cross-function `rt_jump(ctx, 0xADDR)` to an
address that no function starts at. This catches those statically instead of by
play-testing.

Each hit is classified so you know whether to seed it (clean-code) or ignore it
(an artifact whose rt_jump sits in a never-executed misdecoded data region and
which would not even compile as a function):
  clean-code        -> add ADDR to gpl_seeds.txt, regen, re-run (iterate to 0)
  PC-rel-index      -> artifact: emits ctx->PC (no such field); do NOT seed
  data(skipdata)    -> artifact: contains raw data bytes; do NOT seed

Usage: python3 tools/recomp/check_unregistered_targets.py
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GEN  = os.path.join(ROOT, "src", "generated")
MEM  = os.path.join(ROOT, "logs", "gmem_after_load.bin")
GPL_LO, GPL_HI = 0x577000, 0x59F000
BASE, CODE_SIZE = 0x3000, 0x5A0000

sys.path.insert(0, os.path.join(ROOT, "tools"))
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000, CS_MODE_BIG_ENDIAN
from capstone.m68k import M68K_OP_MEM, M68K_REG_PC
from recomp.scanner import _scan_func, mem_base, mem_index


def main():
    src = "".join(open(os.path.join(GEN, f)).read()
                  for f in ("game_gpl_0.c", "game_gpl_1.c", "game_gpl_2.c"))
    targets = set(int(x, 16) for x in
                  re.findall(r'rt_(?:jump|call)\(ctx, 0x([0-9A-Fa-f]+)u\)', src))
    reg = set(int(x, 16) for x in
              re.findall(r'0x([0-9A-Fa-f]{6})u, gfn_gpl_',
                         open(os.path.join(GEN, "game_gpl_table.c")).read()))
    missing = sorted(t for t in targets
                     if t not in reg and GPL_LO <= t < GPL_HI)

    raw = open(MEM, "rb").read()
    data = raw[BASE:BASE + CODE_SIZE]
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000 | CS_MODE_BIG_ENDIAN)
    md.detail = True

    def classify(t):
        ins, _ = _scan_func(data, BASE, t, md, known_entries=set())
        if not ins:
            return "empty"
        for i in ins:
            try:
                ops = i.operands
            except Exception:
                return "data(skipdata)"
            for op in ops:
                if op.type == M68K_OP_MEM and M68K_REG_PC in (mem_base(op), mem_index(op)):
                    return "PC-rel-index"
        return "clean-code"

    if not missing:
        print("OK: 0 unregistered in-region literal rt_jump/rt_call targets")
        return 0
    print(f"{len(missing)} unregistered in-region literal target(s):")
    seedable = []
    for t in missing:
        c = classify(t)
        if c == "clean-code":
            seedable.append(t)
        print(f"  ${t:06X}  {c}")
    if seedable:
        print("\nclean-code -> seed in gpl_seeds.txt, regen, re-run:")
        print("  " + " ".join(f"{t:06X}" for t in seedable))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
