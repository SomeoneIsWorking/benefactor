#!/usr/bin/env python3
"""Recover object-handler entry points from a post-mortem g_mem dump.

When the port hits an rt-miss in the gameplay loop and the strict-abort
handler dumps g_mem (logs/pc_freeze.bin), the live object structures and
the spawn list for the current level are sitting in chip RAM. Each
struct holds a method-offset word followed by header bytes and then code
starting with a movem.w/movem.l (the standard entry preamble).

This walker:
  1. Scans 4-byte slots in the a5-relative struct-pointer region
     ($57F000..$57FFFF) for pointers into the gpl code range.
  2. For each candidate struct, tries both dispatcher arithmetics
     (struct+mo and struct+mo+2 — different struct families use one or
     the other) and accepts the one whose target opcode matches the
     known function-entry pattern (movem.w (a0),... = $4Cxx with the
     $4C90 family, or movem.l <regs>,-(a7) = $48E7).
  3. Prints any body addresses that aren't already in gpl_seeds.txt so
     they can be appended in one shot, regenerated, and the level
     continued.

Usage:
  ./run_pc_game.sh                       # hits rt-miss, writes logs/pc_freeze.bin
  python3 tools/recomp/discover_object_handlers.py
  # ... appends suggested seeds to tools/recomp/gpl_seeds.txt with --append
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
DEFAULT_DUMP = REPO / "logs" / "pc_freeze.bin"
DEFAULT_SEEDS = REPO / "tools" / "recomp" / "gpl_seeds.txt"

# Region where the gameplay code lives (gpl bank).
CODE_LO = 0x577000
CODE_HI = 0x5A0000

# a5-relative work area where the object struct pointer chain lives ($10E6(a5)
# and $10F2(a5)) plus the spawn list at $1162(a5). a5 = $0057EE12 in gameplay,
# so the absolute window is $57F000..$57FFFF, which is small enough to scan
# every 4-byte slot.
SCAN_LO = 0x57F000
SCAN_HI = 0x580000


def discover(dump_path: Path) -> set[int]:
    data = dump_path.read_bytes()

    def r32(a: int) -> int:
        return struct.unpack(">I", data[a:a + 4])[0]

    def r16(a: int) -> int:
        return struct.unpack(">H", data[a:a + 2])[0]

    def is_entry(body: int) -> bool:
        # Real function entries in this binary start with movem.w (a0),<mask>
        # ($4Cx0 family) or movem.l <regs>,-(sp) ($48E7).
        op = r16(body)
        return (op & 0xFFF0) == 0x4C90 or op == 0x48E7

    def body_for(struct_addr: int) -> int | None:
        mo = r16(struct_addr)
        for off in (mo, mo + 2):
            body = (struct_addr + off) & 0xFFFFFF
            if CODE_LO <= body < CODE_HI and is_entry(body):
                return body
        return None

    structs: set[int] = set()
    for a in range(SCAN_LO, SCAN_HI, 4):
        p = r32(a)
        if CODE_LO <= p < CODE_HI:
            structs.add(p)

    return {b for b in (body_for(s) for s in structs) if b is not None}


def read_seeds(seeds_path: Path) -> set[int]:
    seeded: set[int] = set()
    for line in seeds_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            seeded.add(int(line, 16))
        except ValueError:
            pass
    return seeded


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dump", default=str(DEFAULT_DUMP),
                        help="Post-mortem g_mem dump (default: logs/pc_freeze.bin)")
    parser.add_argument("--seeds", default=str(DEFAULT_SEEDS),
                        help="Seed file to diff against (default: tools/recomp/gpl_seeds.txt)")
    parser.add_argument("--append", action="store_true",
                        help="Append the new bodies to the seed file (with a comment)")
    parser.add_argument("--comment", default="object handlers recovered by discover_object_handlers.py",
                        help="Comment line written above the appended seeds")
    args = parser.parse_args()

    dump_path = Path(args.dump)
    if not dump_path.exists():
        print(f"dump not found: {dump_path}", file=sys.stderr)
        return 2

    bodies = discover(dump_path)
    seeded = read_seeds(Path(args.seeds))
    new = sorted(bodies - seeded)

    print(f"validated bodies: {len(bodies)}  (already seeded: {len(bodies & seeded)})")
    print(f"new bodies:       {len(new)}")
    for b in new:
        print(f"  {b:06X}")

    if args.append and new:
        with open(args.seeds, "a") as f:
            f.write(f"# {args.comment}\n")
            for b in new:
                f.write(f"{b:06X}\n")
        print(f"appended {len(new)} seeds to {args.seeds}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
