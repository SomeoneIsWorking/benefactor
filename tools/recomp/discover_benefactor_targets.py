#!/usr/bin/env python3
"""Tailored Benefactor dispatch-target scanner.

Combines two pattern-specific scans we've reverse-engineered for this
game, so we know about (most of) what runtime code can dispatch to,
*without* needing the runtime to actually reach it. This is what avoids
"player triggered something new on level 7 → rt-miss" surprises.

The patterns:

  (A) Object-struct dispatch
      Two dispatchers walk a chain of object struct pointers
      ($10E6(a5)) and jmp into method bodies:
          $57D3F0: jmp (a1, d0.w)   — body = struct + 2 + mo
          $57D8CE: jmp (a1)         — body = struct + mo (a1 pre-computed)
      where mo = first word of struct. Each method body starts with a
      known entry preamble. Statically we don't know every struct
      pointer (some are computed dynamically), so this pass walks a
      post-mortem g_mem dump's $57F000..$57FFFF work area to recover
      every struct in the active chain and the spawn lists. Use the
      strict rt-miss handler's dump (logs/pc_freeze.bin).

  (B) Cross-function branch targets
      Some functions share epilogue / cleanup blocks: a handler ends
      with `bra.w $57D8A4` to use the dispatcher's shared restore-and-
      continue block. The recompiler's static descent only registers
      entries reached from explicit seeds; cross-function `bra/bne/...`
      targets that land inside *another* already-registered function
      end up as unregistered addresses, so rt_jump misses them at
      runtime. This pass disassembles every gpl entry, computes every
      conditional/unconditional branch's target, and reports any target
      that lies inside a *different* function's range.

  (C) Alternate return-landing points (in-function indirect jumps)
      The dispatcher in $57D56C pushes a cleanup label with `pea`
      before `jmp (a1)`; the dispatched handler's `rts` pops it and
      jumps. The pop target is INSIDE $57D56C (e.g. $57D8A4, $57D88A)
      but rt_jump misses it because no `gfn_gpl_57D8A4` exists. These
      labels can't be reached via static branch analysis — they're
      computed via `pea $X(pc)` / register arithmetic. Heuristic: each
      alternate landing point starts with `4CDF` (movem.l (a7)+,<mask>
      — restore-from-stack epilogue) and sits IMMEDIATELY after an
      unconditional terminator (rts/bra/jmp) inside a seeded function.

Run after a play session that hits an rt-miss to add both kinds in one
shot:

    python3 tools/recomp/discover_benefactor_targets.py --append
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
DEFAULT_GMEM = REPO / "logs" / "gmem_after_load.bin"
DEFAULT_FREEZE = REPO / "logs" / "pc_freeze.bin"
DEFAULT_SEEDS = REPO / "tools" / "recomp" / "gpl_seeds.txt"

CODE_LO = 0x577000
CODE_HI = 0x5A0000

# Function-entry opcodes we've observed in this binary. We want to be
# permissive enough to accept anything the game's handlers actually start
# with (some lead with `lea $X(pc),An` for self-relative data, some with a
# direct `move.w from $X(a5)` register read, etc.) while still rejecting
# random data bytes.
def is_entry_opcode(op_high: int, op_full: int) -> bool:
    # movem.w (a0),<mask>  family: $4C90..$4C9F
    if (op_full & 0xFFF0) == 0x4C90:
        return True
    # movem.w d16(a0),<mask>: $4CA8..$4CAF + d16 byte after — common when
    # saving to a per-object scratch buffer rather than the stack.
    if (op_full & 0xFFF8) == 0x4CA8:
        return True
    # movem.l <mask>,-(a7) at function entry: $48E7
    if op_full == 0x48E7:
        return True
    # bra.w trampoline: $6000 + signed disp16
    if op_full == 0x6000:
        return True
    # link an,#imm: $4E50..$4E57 — classic stack-frame setup
    if 0x4E50 <= op_full <= 0x4E57:
        return True
    # lea $disp(pc),An: $41FA..$4FFA in steps of 0x200 (any An). Used by
    # object handlers that start by loading their own data area.
    if (op_full & 0xF1FF) == 0x41FA:
        return True
    # lea ea,An: $41C0..$4FFF general lea encoding (a wider net).
    # bit pattern: 0100 RRR 111 mmm rrr where mode 7/2 is d16(PC) already
    # covered above; the broader $41C8..$4FE8 (lea (An),An) is too common,
    # so we stop at the safer lea-pc form.
    # move.w $d16(a5),Dn: $3X2D (where X is the dest reg). Object handlers
    # often start by loading a state byte from the a5 work area.
    if (op_full & 0xF1FF) == 0x302D:
        return True
    # tst.w/tst.b/cmpi at function entry — sometimes the first thing a
    # handler does is test an input register. Allow any of the OPxxx forms
    # whose high byte is a known opcode family.
    return False


# ────────────────────────────────────────────────────────────────────────────
# Pattern A: walk live object chains in a post-mortem dump
# ────────────────────────────────────────────────────────────────────────────

def discover_chain_bodies(freeze_path: Path) -> set[int]:
    """Walk the work area $57F000..$57FFFF for object struct pointers."""
    if not freeze_path.exists():
        return set()
    data = freeze_path.read_bytes()

    def r16(a: int) -> int: return struct.unpack(">H", data[a:a + 2])[0]
    def r32(a: int) -> int: return struct.unpack(">I", data[a:a + 4])[0]

    def body_for(p: int) -> int | None:
        mo = r16(p)
        if mo > 0x400 or mo < 4:
            return None
        for off in (mo, mo + 2):
            body = (p + off) & 0xFFFFFF
            if CODE_LO <= body < CODE_HI:
                op = r16(body)
                if is_entry_opcode(op >> 8, op):
                    return body
        return None

    structs: set[int] = set()
    for a in range(0x57F000, 0x580000, 4):
        p = r32(a)
        if CODE_LO <= p < CODE_HI:
            structs.add(p)
    return {b for b in (body_for(s) for s in structs) if b is not None}


# ────────────────────────────────────────────────────────────────────────────
# Pattern B: cross-function branch targets
# ────────────────────────────────────────────────────────────────────────────

def disassemble_function(data: bytes, start: int) -> tuple[set[int], int]:
    """Disassemble forward from `start` and return (branch_targets, fn_end).

    fn_end is the highest address we know belongs to this function (the
    last instruction's end). We stop at an unconditional terminator
    (rts/rte/jmp/bra) that has no remaining un-visited targets.
    """
    try:
        from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_020
        from capstone.m68k import M68K_OP_BR_DISP
    except ImportError:
        print("capstone is required (pip install capstone)", file=sys.stderr)
        sys.exit(2)

    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_020)
    md.detail = True

    queue = [start]
    visited: set[int] = set()
    targets: set[int] = set()
    max_addr = start

    def branch_target(insn):
        try:
            for op in insn.operands:
                if op.type == M68K_OP_BR_DISP:
                    return insn.address + 2 + op.br_disp.disp
        except Exception:
            # Capstone raises CsError when operands are accessed on a SKIPDATA
            # instruction (a byte that doesn't decode). Treat as not-a-branch.
            pass
        return None

    while queue:
        addr = queue.pop()
        while addr not in visited and CODE_LO <= addr < CODE_HI:
            visited.add(addr)
            chunk = data[addr - 0:addr - 0 + 16]  # data is full g_mem
            decoded = list(md.disasm(chunk, addr, count=1))
            if not decoded:
                break
            insn = decoded[0]
            max_addr = max(max_addr, addr + insn.size)
            mn = insn.mnemonic.lower().split('.')[0]
            t = branch_target(insn)
            if t is not None and CODE_LO <= t < CODE_HI:
                targets.add(t)
                queue.append(t)
            if mn in ("rts", "rte", "rtr", "jmp", "bra"):
                break
            addr += insn.size

    return targets, max_addr


def discover_return_landing_points(seeds: set[int], gmem_path: Path) -> set[int]:
    """Scan inside each seeded function for `movem.l (a7)+,<mask>` ($4CDF)
    instructions that sit right after an unconditional terminator. These are
    the alternate "return-from-handler" landing points reached by indirect
    rts (after a `pea $X(pc)` push)."""
    if not gmem_path.exists():
        return set()
    try:
        from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_020
    except ImportError:
        return set()
    data = gmem_path.read_bytes()
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_020)
    md.detail = True

    sorted_seeds = sorted(s for s in seeds if CODE_LO <= s < CODE_HI)
    landings: set[int] = set()
    for i, fn in enumerate(sorted_seeds):
        next_fn = sorted_seeds[i + 1] if i + 1 < len(sorted_seeds) else CODE_HI
        # Linear sweep through the function's address range.
        addr = fn
        prev_term = False
        while addr < min(fn + 0x2000, next_fn):
            try:
                decoded = list(md.disasm(data[addr:addr + 16], addr, count=1))
            except Exception:
                break
            if not decoded:
                break
            insn = decoded[0]
            op = struct.unpack(">H", data[addr:addr + 2])[0]
            # $4CDF is movem.l (a7)+, <mask>. If it follows an unconditional
            # terminator (or starts a basic block) and isn't the function's
            # own entry, treat it as an alternate landing point.
            if op == 0x4CDF and prev_term and addr != fn:
                landings.add(addr)
            mn = insn.mnemonic.lower().split('.')[0]
            prev_term = mn in ("rts", "rte", "rtr", "jmp", "bra")
            addr += insn.size
    return landings


def discover_cross_fn_targets(seeds: set[int], gmem_path: Path) -> set[int]:
    """For each seeded entry, disassemble and find branches that land inside
    a *different* entry's [start, end) range."""
    if not gmem_path.exists():
        print(f"{gmem_path}: not found; skipping cross-function pass", file=sys.stderr)
        return set()
    data = gmem_path.read_bytes()
    sorted_seeds = sorted(s for s in seeds if CODE_LO <= s < CODE_HI)

    # Pre-compute each function's [start, end) — end is the next seed or a
    # heuristic terminator. We walk each function once.
    fn_ranges: dict[int, tuple[int, int]] = {}
    fn_targets: dict[int, set[int]] = {}
    for i, s in enumerate(sorted_seeds):
        next_s = sorted_seeds[i + 1] if i + 1 < len(sorted_seeds) else CODE_HI
        targets, end = disassemble_function(data, s)
        fn_ranges[s] = (s, min(end, next_s))
        fn_targets[s] = targets

    # For each branch target, check if it falls inside a *different* function
    # — i.e., a function whose start != target. Targets that ARE another
    # registered entry are fine (rt_jump will find them). The interesting
    # ones are mid-function labels.
    seeds_set = set(sorted_seeds)
    new_targets: set[int] = set()
    for owner, ts in fn_targets.items():
        own_lo, own_hi = fn_ranges[owner]
        for t in ts:
            if t in seeds_set:           # already a known entry
                continue
            if own_lo <= t < own_hi:     # local label inside this function
                continue
            # Land in some other function's range?
            for s, (lo, hi) in fn_ranges.items():
                if s == owner:
                    continue
                if lo <= t < hi:
                    new_targets.add(t)
                    break
    return new_targets


# ────────────────────────────────────────────────────────────────────────────

def read_seeds(seeds_path: Path) -> set[int]:
    out: set[int] = set()
    for line in seeds_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            out.add(int(line, 16))
        except ValueError:
            pass
    return out


def read_registered_entries() -> set[int]:
    """Read the FULL set of registered gpl entries from the generated table.
    gpl_seeds.txt is only one of the recompiler's input sources (entries.py
    and discovered.py contribute too); the generated table is the union, so
    this is what we should diff against."""
    import re
    out: set[int] = set()
    table = REPO / "src" / "generated" / "game_gpl_table.c"
    if not table.exists():
        return out
    pat = re.compile(r"\{\s*0x([0-9A-Fa-f]+)u,")
    for line in table.read_text().splitlines():
        m = pat.search(line)
        if m:
            out.add(int(m.group(1), 16))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--freeze", default=str(DEFAULT_FREEZE),
                        help="Post-mortem g_mem dump for the chain walk")
    parser.add_argument("--gmem", default=str(DEFAULT_GMEM),
                        help="gpl-bank g_mem dump for the cross-function scan")
    parser.add_argument("--seeds", default=str(DEFAULT_SEEDS),
                        help="Seed file to diff against")
    parser.add_argument("--append", action="store_true",
                        help="Append new addresses to the seed file")
    args = parser.parse_args()

    seeds = read_seeds(Path(args.seeds))
    # Union with the full registered-entry set from the generated table — the
    # recompiler accepts entries from seeds.txt PLUS entries.py + discovered.py,
    # and we should diff against the actual final set, not just seeds.txt.
    seeds |= read_registered_entries()
    print(f"existing seeds + registered entries: {len(seeds)}")

    a_bodies = discover_chain_bodies(Path(args.freeze))
    a_new = sorted(a_bodies - seeds)
    print(f"\n[A] object struct chain bodies: {len(a_bodies)} ({len(a_bodies & seeds)} already seeded)")
    for b in a_new:
        print(f"    {b:06X}  (chain)")

    seeds_for_cross = seeds | a_bodies
    b_targets = discover_cross_fn_targets(seeds_for_cross, Path(args.gmem))
    b_new = sorted(b_targets - seeds_for_cross)
    print(f"\n[B] cross-function branch targets: {len(b_targets)} ({len(b_targets & seeds_for_cross)} already seeded)")
    for t in b_new:
        print(f"    {t:06X}  (cross-fn)")

    c_landings = discover_return_landing_points(seeds_for_cross, Path(args.gmem))
    c_new = sorted(c_landings - seeds_for_cross)
    print(f"\n[C] alternate return-landing points: {len(c_landings)} ({len(c_landings & seeds_for_cross)} already seeded)")
    for t in c_new:
        print(f"    {t:06X}  (return-landing)")

    # Only Pattern A is safe to auto-seed. Patterns B (cross-function branch
    # targets) and C (return-landing points) point INSIDE an existing function,
    # and creating a new gfn entry at a mid-function label duplicates the
    # decoded body in two functions and breaks runtime. Use B/C as a
    # diagnostic — fix the emitter (emit `goto L_X` for in-function bra.w)
    # rather than splitting the function.
    safe_new = sorted(a_new)
    diag_new = sorted(set(b_new) | set(c_new))
    print(f"\nsafe NEW (auto-seedable): {len(safe_new)}")
    print(f"diagnostic NEW (B+C — DO NOT auto-seed): {len(diag_new)}")

    if args.append and safe_new:
        with open(args.seeds, "a") as f:
            f.write("# discover_benefactor_targets.py: object struct chain bodies (Pattern A)\n")
            for x in safe_new:
                f.write(f"{x:06X}\n")
        print(f"appended {len(safe_new)} (Pattern A only) to {args.seeds}")
    elif args.append:
        print("nothing to append (Pattern B/C results are diagnostic-only)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
