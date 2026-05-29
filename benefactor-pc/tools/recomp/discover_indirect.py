#!/usr/bin/env python3
"""
Iteratively discover indirect-dispatch call targets the static recompiler
misses, and register them as recompiler entry points.

The static descent (step2_descent.py) cannot follow register-indirect jsr or
jmp-table dispatch, so handler functions reached only that way are never
translated. At runtime rt_call logs "no function at $X – skipping" for each.
Such a log is *proof* X is a real call target (the game tried to call it), so
we can safely add it as an entry.

This tool drives a collect -> append -> regenerate -> rebuild -> rerun loop to
a fixpoint, accumulating addresses into discovered.py (merged by entries.py).

Usage:
    python3 tools/recomp/discover_indirect.py [--max-iters N] [--frames N]

Run from the benefactor-pc directory. Requires a built benefactor-pc binary
and the disk image at Floppies/disk.adf relative to the repo root.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PC_DIR = os.path.abspath(os.path.join(HERE, "..", ".."))          # benefactor-pc
ROOT = os.path.abspath(os.path.join(PC_DIR, ".."))                # repo root
CHIP_DUMP = os.path.join(ROOT, "chip_ram_dump.bin")
DISK = os.path.join(ROOT, "Floppies", "disk.adf")
BIN = os.path.join(PC_DIR, "build", "benefactor-pc")
DISCOVERED_PY = os.path.join(HERE, "discovered.py")

SKIP_RE = re.compile(r"no function at \$([0-9A-Fa-f]+)")


def load_discovered():
    ns = {}
    with open(DISCOVERED_PY) as f:
        exec(f.read(), ns)
    return [a.upper() for a in ns.get("DISCOVERED", [])]


def save_discovered(addrs):
    addrs = sorted(set(a.upper() for a in addrs), key=lambda x: int(x, 16))
    with open(DISCOVERED_PY, "w") as f:
        f.write("# Auto-maintained by tools/recomp/discover_indirect.py — do not hand-edit.\n")
        f.write("# Hex address strings of indirect-dispatch call targets discovered at runtime.\n")
        f.write("DISCOVERED = [\n")
        for a in addrs:
            f.write(f'    "{a}",\n')
        f.write("]\n")


def regenerate():
    subprocess.run([sys.executable, os.path.join(HERE, "recomp.py"),
                    CHIP_DUMP, "--chip-dump", "--out-dir",
                    os.path.join(PC_DIR, "src", "generated")],
                   check=True, cwd=PC_DIR, stdout=subprocess.DEVNULL)


def rebuild():
    subprocess.run(["cmake", "--build", os.path.join(PC_DIR, "build"),
                    "--target", "benefactor-pc", "-j", str(os.cpu_count())],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def run_and_collect(frames):
    env = dict(os.environ, RT_CALLS="1", BENEFACTOR_HEADLESS="1",
               BENEFACTOR_DUMP_FRAME=str(frames))
    p = subprocess.run(["timeout", "-s", "KILL", "60", BIN, CHIP_DUMP, DISK],
                       env=env, capture_output=True, text=True)
    out = p.stdout + p.stderr
    return {m.group(1).upper().lstrip("0").rjust(4, "0")
            for m in SKIP_RE.finditer(out)}


def main():
    frames = 120
    max_iters = 10
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--frames":
            frames = int(args[i + 1])
        elif a == "--max-iters":
            max_iters = int(args[i + 1])

    discovered = set(load_discovered())
    print(f"[discover] starting with {len(discovered)} known indirect targets")
    for it in range(1, max_iters + 1):
        print(f"[discover] iter {it}: regenerate + rebuild + run …")
        regenerate()
        rebuild()
        skips = run_and_collect(frames)
        new = skips - discovered
        if not new:
            print(f"[discover] FIXPOINT reached: no new skips. "
                  f"Total discovered: {len(discovered)}")
            break
        print(f"[discover]   +{len(new)} new: " + " ".join(sorted(new)))
        discovered |= new
        save_discovered(discovered)
    else:
        print(f"[discover] hit max-iters={max_iters}; "
              f"{len(discovered)} discovered, may not be complete")
    save_discovered(discovered)
    print(f"[discover] wrote {len(discovered)} entries to discovered.py")


if __name__ == "__main__":
    main()
