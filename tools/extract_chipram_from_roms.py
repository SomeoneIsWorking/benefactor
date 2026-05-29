#!/usr/bin/env python3
"""Extract a fresh PUAE chip RAM dump from ROMs/WHDLoad disks.

This tool runs benefactor-harness in headless mode, captures the PUAE sync-point
chip RAM dump, and copies it to a canonical output path (default: chip_ram_dump.bin).
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> int:
    proc = subprocess.run(cmd, cwd=str(cwd), env=env)
    return int(proc.returncode)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="chip_ram_dump.bin", help="Output chip RAM dump path")
    parser.add_argument("--kick-dir", default="harness", help="Kickstart directory")
    parser.add_argument("--whdload", default="harness/Benefactor.slave", help="WHDLoad install file path")
    parser.add_argument("--disk1", default="Disk.1", help="Disk 1 path")
    parser.add_argument("--disk2", default="Disk.2", help="Disk 2 path")
    parser.add_argument("--disk3", default="Disk.3", help="Disk 3 path")
    parser.add_argument("--build", action="store_true", help="Build benefactor-harness before extracting")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    build_dir = repo / "benefactor-pc" / "build"
    harness_bin = build_dir / "benefactor-harness"
    harness_dump = repo / "logs" / "harness_puae_chipram.bin"
    out_path = (repo / args.output).resolve() if not os.path.isabs(args.output) else Path(args.output)

    if args.build:
        cfg_rc = run(["cmake", "-S", "benefactor-pc", "-B", "build", "-DCMAKE_BUILD_TYPE=Debug"], repo)
        if cfg_rc != 0:
            return cfg_rc
        jobs = str(os.cpu_count() or 4)
        bld_rc = run(["cmake", "--build", "build", "--target", "benefactor-harness", "-j", jobs], repo)
        if bld_rc != 0:
            return bld_rc

    if not harness_bin.exists():
        print("benefactor-harness not found. Build it first or pass --build.", file=sys.stderr)
        return 2

    harness_dump.parent.mkdir(parents=True, exist_ok=True)
    if harness_dump.exists():
        harness_dump.unlink()

    cmd = [
        str(harness_bin),
        args.kick_dir,
        args.whdload,
        args.disk1,
        args.disk2,
        args.disk3,
        "--frames",
        "1",
        "--report",
        "logs/harness_report.txt",
    ]
    env = os.environ.copy()
    env["SDL_VIDEODRIVER"] = "offscreen"

    rc = run(cmd, repo, env=env)

    if not harness_dump.exists():
        print("Failed to produce logs/harness_puae_chipram.bin", file=sys.stderr)
        return 3 if rc == 0 else rc

    out_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(harness_dump, out_path)
    print(f"Wrote {out_path}")

    # Harness may exit non-zero due later compare failures; dump is still valid.
    if rc != 0:
        print(f"Warning: harness exited with {rc}, but chip dump extraction succeeded.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
