#!/usr/bin/env python3
"""Static jump-table target extractor for the gameplay (gpl) bank.

The gameplay engine dispatches its state machines (player animation states,
etc.) through PC-relative jump tables:

    lea   $d(a5), aN            ; aN = table base (a5 = $57EE12)
    move.w idx(a5), d0
    add.w  d0, d0               ; word index
    move.w (aN, d0.w), d0       ; d0 = 16-bit signed offset from the table
    jmp    BASE(pc, d0.w)        ; target = BASE + offset   (BASE == table base)

The recompiler's recursive descent can't follow these computed jumps, so the
target handlers are never emitted and abort at runtime ("no function at $X").
This tool finds every `jmp $BASE(pc, Dn.w)` in the region, reads the 16-bit
offset table that sits AT $BASE, and prints every target (= BASE + offset) until
an entry yields a misaligned (odd) or out-of-range address — the table end.

Run from the repo root:
    python3 tools/recomp/extract_jumptables.py [--append]
With --append, the discovered targets are appended to gpl_seeds.txt.
"""
import os, sys
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000, CS_MODE_BIG_ENDIAN
from capstone.m68k import M68K_OP_MEM, M68K_OP_IMM

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))   # repo root
MEM  = os.path.join(ROOT, "logs", "gmem_after_load.bin")
# These targets are now AUTO-DERIVED at regen time (recomp.py calls
# discover_jumptable_targets), so --append is only for inspection/debugging; it
# writes into the discovered bucket of the structured seed tree.
SEEDS = os.path.join(HERE, "seeds", "zz-discovered.txt")

CODE_LO, CODE_HI = 0x577000, 0x59F000     # gameplay code region (data/zeros above this)


def discover_jumptable_targets(d, md=None, verbose=False):
    """Return the set of gpl handler addresses reachable through the engine's
    dispatch tables — derived structurally so they don't have to be hand-seeded:

      * pc-relative jump tables   (`jmp $BASE(pc,Dn.w)` + 16-bit offset table)
      * installed handler pointers (code-range `move.l #h,slot` / `cmpi.l #h,..`)
      * dc.l function-pointer tables (runs of >=4 consecutive in-range code ptrs)

    `d` is the full chip dump (indexed by absolute address; the gpl bank loads at
    its real address so no rebasing). Pass `verbose=True` to print the tables.
    """
    if md is None:
        md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000 | CS_MODE_BIG_ENDIAN)
        md.detail = True

    def s16(o):
        v = (d[o] << 8) | d[o + 1]
        return v - 0x10000 if v >= 0x8000 else v

    def extract_table(base):
        """Read 16-bit signed offsets at `base`; target = base + offset. Stop at
        the first misaligned (odd) or out-of-range entry = table end.

        Reject self-referential tables: if an offset points INTO the table's own
        byte span (target within [base, base + 2*entries]) it's not a jump table
        (e.g. $57B61C, whose 'offsets' 0,6,12... index a 6-byte struct array, not
        code) — extracting it would seed data as code."""
        tg = []
        for i in range(256):
            off = s16(base + i*2)
            t = base + off
            if (t & 1) or not (CODE_LO <= t < CODE_HI):
                break
            if base <= t < base + 2*(i+1):
                return []          # offset lands inside the table itself = bogus
            tg.append(t)
        return sorted(set(tg))

    # A8..A15 = capstone A0..A7 reg ids start at M68K_REG_A0; track the most
    # recent `lea $B(pc), aN` base per A-register so an `jsr/jmp (aN, Dn.w)`
    # dispatch a few instructions later resolves its table base.
    from capstone.m68k import M68K_REG_A0, M68K_REG_A7
    lea_pc = {}        # reg id -> base address from a recent lea $B(pc), aN
    tbl_loaded = set()  # reg ids that just did `move.w (aN, Dm.w), Dm` (offset load)

    def is_code(addr, n=5):
        """True if `addr` disassembles into n consecutive valid instructions —
        i.e. it's a real function/handler start, not data or a mid-instruction
        byte. Guards against seeding non-code (which the emitter can't translate)."""
        p = addr
        if d[addr] == 0 and d[addr+1] == 0:
            return False                  # zero-run (ori.b #0,d0 ...) = data, not code
        for _ in range(n):
            try:
                ins = next(md.disasm(d[p:p+8], p, 1))
            except StopIteration:
                return False
            if ins.size < 2 or ins.id == 0:
                return False
            try:
                _ = ins.operands
            except Exception:
                return False
            if ins.mnemonic.lower() in ("rts", "rte", "rtr"):
                return True            # short leaf is fine
            p += ins.size
        return True

    # The game installs state-machine handlers as code-address immediates
    # (move.l #handler, slot; cmpi.l #handler, slot). A code-range immediate is
    # an installed/referenced handler entry — collect them all.
    handler_imms = set()

    tables = {}   # base -> sorted list of targets
    a = CODE_LO
    while a < CODE_HI:
        try:
            insn = next(md.disasm(d[a:a+8], a, 1))
        except StopIteration:
            a += 2; continue
        mn = insn.mnemonic.lower().split('.')[0]
        opstr = insn.op_str.lower()
        try:
            ops = insn.operands
        except Exception:
            a += insn.size if insn.size >= 2 else 2; continue

        # Track `lea $B(pc), aN`  -> lea_pc[aN] = B
        if mn == "lea" and len(ops) == 2 and ops[1].type != M68K_OP_MEM:
            src = ops[0]
            dstreg = ops[1].reg if hasattr(ops[1], 'reg') else None
            if (src.type == M68K_OP_MEM and "(pc" in opstr and src.mem.disp
                    and dstreg and M68K_REG_A0 <= dstreg <= M68K_REG_A7):
                lea_pc[dstreg] = insn.address + 2 + src.mem.disp

        # Dispatch via `jmp $BASE(pc, Dn.w)` — base from the pc-relative disp.
        if mn == "jmp" and "(pc," in opstr and ("d0" in opstr or "d1" in opstr or "d2" in opstr):
            for op in ops:
                if op.type == M68K_OP_MEM and op.mem.disp:
                    base = insn.address + 2 + op.mem.disp
                    if CODE_LO <= base <= CODE_HI:
                        tg = extract_table(base)
                        if tg: tables[base] = tg

        # Track `move.w (aN, Dm.w), Dm` — the offset-table load that confirms aN
        # is a base+offset jump table (vs. a struct/data table indexed directly).
        if mn == "move" and ".w" in insn.mnemonic.lower() and len(ops) == 2:
            src = ops[0]
            if (src.type == M68K_OP_MEM and src.mem.index_reg
                    and src.mem.base_reg in lea_pc):
                tbl_loaded.add(src.mem.base_reg)

        # Dispatch via `jsr/jmp (aN, Dn.w)` where aN = lea $B(pc) AND an offset was
        # just loaded from (aN, Dm) — the confirmed base+offset jump-table shape.
        if mn in ("jsr", "jmp"):
            for op in ops:
                if (op.type == M68K_OP_MEM and op.mem.index_reg
                        and op.mem.base_reg in lea_pc
                        and op.mem.base_reg in tbl_loaded):
                    base = lea_pc[op.mem.base_reg]
                    if CODE_LO <= base <= CODE_HI:
                        tg = extract_table(base)
                        if tg: tables[base] = tg

        # Collect code-range immediate operands (installed handler pointers).
        for op in ops:
            if op.type == M68K_OP_IMM:
                v = op.imm
                if v and (v & 1) == 0 and CODE_LO <= v < CODE_HI:
                    handler_imms.add(v)

        a += insn.size if insn.size >= 2 else 2

    allt = set()
    for base, tg in sorted(tables.items()):
        if verbose:
            print("table @ $%06X: %d unique targets" % (base, len(tg)))
            print("   ", " ".join("%06X" % t for t in tg))
        allt |= set(tg)
    if verbose:
        print("TOTAL unique jump-table targets: %d" % len(allt))

    # Validate immediate handler pointers as real code starts before seeding.
    handlers = sorted(h for h in handler_imms if is_code(h))
    allt |= set(handlers)

    # dc.l function-pointer tables: the game also dispatches state handlers
    # through arrays of 32-bit code pointers (e.g. the $579Dxx stubs are reached
    # via the pointer table at $579AE0, then `movea.l d0,a1; jsr (a1)`). Find runs
    # of >=4 consecutive 4-aligned, in-range, validated-code pointers = a table.
    def u32(o): return (d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3]
    ptrs = set()
    p = CODE_LO & ~3
    while p + 4 <= CODE_HI:
        run = []
        q = p
        while q + 4 <= CODE_HI:
            v = u32(q)
            if (v & 1) == 0 and CODE_LO <= v < CODE_HI and is_code(v):
                run.append(v); q += 4
            else:
                break
        if len(run) >= 4:
            ptrs |= set(run)
            p = q
        else:
            p += 4
    allt |= ptrs
    if verbose:
        print("handler-pointer immediates (validated code): %d" % len(handlers))
        print("dc.l pointer-table handlers (validated code): %d" % len(ptrs))
        print("TOTAL unique discovered handlers: %d" % len(allt))
    return allt


def main():
    d = open(MEM, "rb").read()
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000 | CS_MODE_BIG_ENDIAN)
    md.detail = True
    allt = discover_jumptable_targets(d, md, verbose=True)

    if "--append" in sys.argv and allt:
        existing = open(SEEDS).read()
        new = [t for t in sorted(allt) if "%06X" % t not in existing.upper()]
        if new:
            with open(SEEDS, "a") as f:
                f.write("\n# pc-relative jump-table targets (extract_jumptables.py)\n")
                f.write(" ".join("%06X" % t for t in new) + "\n")
            print("appended %d new seeds to gpl_seeds.txt" % len(new))
        else:
            print("no new seeds (all already present)")

if __name__ == "__main__":
    main()
