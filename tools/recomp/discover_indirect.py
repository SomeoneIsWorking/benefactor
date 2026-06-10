#!/usr/bin/env python3
"""
Iteratively discover indirect-dispatch call targets the static recompiler
misses, and register them as recompiler entry seeds.

The static descent only follows constant bsr/jsr/jmp targets; handlers reached
via register-indirect dispatch (jump tables, IRQ-vector pokes) are never
translated. At runtime rt dispatch logs `[rt-miss] $X` for each — proof that X
is a real call target — so it can safely be appended to the bank's seeds file.

This drives a run -> collect -> append-seeds -> rebuild loop to a fixpoint.
The rebuild regenerates the bank from the seeds (scripts/build.sh is
hash-gated, so it only regenerates when the seeds actually changed).

Usage:
    python3 tools/recomp/discover_indirect.py --seeds tools/recomp/credits_seeds.txt \
        --script scratch/win.repl [--max-iters N] [--note "victory cutscene"]

  --seeds   the bank's seeds file to append to (required)
  --script  REPL command file fed to benefactor-harness stdin (required);
            e.g. "load\npc 2300\npc 6000\nq" to replay a savestate scenario.
            Must end with q (added if missing).
  --note    short provenance note written into the seeds-file comment.

Runs the headless harness (BENEFACTOR_SKIP_PUAE=1 build/benefactor-harness
Disk.*) from the repo root.

2026-06-10: rewritten for the current tree (harness REPL + per-bank seeds
files; the old standalone-binary/Floppies/discovered.py flow is gone).
"""
import datetime
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BIN = os.path.join(ROOT, "build", "benefactor-harness")

MISS_RE = re.compile(r"\[rt-miss\] \$([0-9A-Fa-f]+)")


def seeds_in_file(path):
    addrs = set()
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if re.fullmatch(r"[0-9A-Fa-f]+", line):
                addrs.add(line.upper())
    return addrs


def append_seeds(path, addrs, note, iteration):
    stamp = datetime.date.today().isoformat()
    with open(path, "a") as f:
        f.write(f"\n# discover_indirect {stamp} iter {iteration}"
                f"{' — ' + note if note else ''}:\n")
        for a in sorted(addrs, key=lambda x: int(x, 16)):
            f.write(f"{a}\n")


def rebuild():
    subprocess.run(["bash", os.path.join(ROOT, "scripts", "build.sh")],
                   check=True, cwd=ROOT,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def run_and_collect(script_text):
    disks = sorted(glob.glob(os.path.join(ROOT, "Disk.*")))
    if not disks:
        sys.exit("[discover] no Disk.* images in repo root")
    env = dict(os.environ, BENEFACTOR_SKIP_PUAE="1")
    p = subprocess.run(["timeout", "-s", "KILL", "600", BIN] + disks,
                       input=script_text, env=env, cwd=ROOT,
                       capture_output=True, text=True)
    out = p.stdout + p.stderr
    return {m.group(1).upper() for m in MISS_RE.finditer(out)}


def main():
    seeds_path = script_path = None
    note = ""
    max_iters = 10
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--seeds":
            seeds_path = args[i + 1]; i += 1
        elif a == "--script":
            script_path = args[i + 1]; i += 1
        elif a == "--note":
            note = args[i + 1]; i += 1
        elif a == "--max-iters":
            max_iters = int(args[i + 1]); i += 1
        else:
            sys.exit(f"[discover] unknown arg {a}\n{__doc__}")
        i += 1
    if not seeds_path or not script_path:
        sys.exit(__doc__)
    seeds_path = os.path.join(ROOT, seeds_path)
    with open(os.path.join(ROOT, script_path)) as f:
        script_text = f.read()
    if script_text.split() and script_text.split()[-1] != "q":
        script_text = script_text.rstrip() + "\nq\n"

    known = seeds_in_file(seeds_path)
    print(f"[discover] {len(known)} seeds in {os.path.relpath(seeds_path, ROOT)}")
    for it in range(1, max_iters + 1):
        misses = run_and_collect(script_text)
        new = misses - known
        if not new:
            print(f"[discover] iter {it}: FIXPOINT — no new rt-misses")
            return
        print(f"[discover] iter {it}: +{len(new)} new: " + " ".join(sorted(new)))
        append_seeds(seeds_path, new, note, it)
        known |= new
        print(f"[discover] iter {it}: rebuilding …")
        rebuild()
    print(f"[discover] hit max-iters={max_iters}; may not be complete")


if __name__ == "__main__":
    main()
