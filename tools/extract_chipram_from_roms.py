#!/usr/bin/env python3
"""Extract a fresh PUAE chip RAM dump for the intro/default recompiler bank.

How it works: the harness writes logs/harness_puae_chipram.bin during PUAE
boot (at the deterministic sync point — see src/harness/harness_puae.c).
This script just runs the harness with an immediate-quit REPL command and
then copies that dump to chip_ram_dump.bin at the repo root.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path, env: dict[str, str] | None = None,
        stdin_text: str | None = None) -> int:
    proc = subprocess.run(cmd, cwd=str(cwd), env=env,
                          input=stdin_text, text=True)
    return int(proc.returncode)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="chip_ram_dump.bin",
                        help="Output chip RAM dump path (default: chip_ram_dump.bin at repo root)")
    parser.add_argument("--kick-dir", default="harness", help="Kickstart directory")
    parser.add_argument("--whdload", default="harness/Benefactor.slave",
                        help="WHDLoad install file path")
    parser.add_argument("--disk1", default="Disk.1")
    parser.add_argument("--disk2", default="Disk.2")
    parser.add_argument("--disk3", default="Disk.3")
    parser.add_argument("--build", action="store_true",
                        help="Build benefactor-harness before extracting")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    build_dir = repo / "build"
    harness_bin = build_dir / "benefactor-harness"
    harness_dump = repo / "logs" / "harness_puae_chipram.bin"
    out_path = repo / args.output if not os.path.isabs(args.output) else Path(args.output)

    if args.build:
        cfg_rc = run(["cmake", "-S", ".", "-B", "build"], repo)
        if cfg_rc != 0:
            return cfg_rc
        jobs = str(os.cpu_count() or 4)
        bld_rc = run(["cmake", "--build", "build", "--target",
                      "benefactor-harness", "-j", jobs], repo)
        if bld_rc != 0:
            return bld_rc

    if not harness_bin.exists():
        print("build/benefactor-harness not found. Pass --build or build it first.",
              file=sys.stderr)
        return 2

    harness_dump.parent.mkdir(parents=True, exist_ok=True)
    if harness_dump.exists():
        harness_dump.unlink()

    cmd = [str(harness_bin), args.kick_dir, args.whdload,
           args.disk1, args.disk2, args.disk3]
    env = os.environ.copy()
    env["SDL_VIDEODRIVER"] = "offscreen"

    # The harness boots PUAE (writes the chipram dump at sync) and then waits
    # at its REPL. Feed it 'q' so it exits cleanly once the dump has been
    # written.
    rc = run(cmd, repo, env=env, stdin_text="q\n")

    if not harness_dump.exists():
        print("Failed to produce logs/harness_puae_chipram.bin", file=sys.stderr)
        return 3 if rc == 0 else rc

    out_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(harness_dump, out_path)
    print(f"Wrote {out_path} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
