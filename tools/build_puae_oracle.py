#!/usr/bin/env python3
"""Build the independent headless PUAE diagnostic under the shared build root."""

from __future__ import annotations

import os
import shlex
import subprocess
from collections import deque

from tools.paths import ROOT, SCRATCH

VENDOR = ROOT / "vendor" / "libretro-uae"
BUILD = ROOT / "build" / "puae-oracle"


def commands(cc: list[str], cxx: list[str]) -> tuple[list[str], ...]:
    core = [
        "make",
        "-C",
        str(BUILD),
        "-I",
        str(VENDOR),
        "-f",
        str(VENDOR / "Makefile"),
        f"CORE_DIR={VENDOR}",
        f"CC={' '.join(cc)}",
        f"CXX={' '.join(cxx)}",
        "SILENT=1",
        "-s",
        "-j4",
        "puae_libretro.so",
    ]
    c_log = [
        *cc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Isrc",
        "-c",
        "src/common/log.c",
        "-o",
        str(BUILD / "log.o"),
    ]
    c_options = [
        *cc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Isrc",
        "-c",
        "src/harness/puae_options.c",
        "-o",
        str(BUILD / "puae_options.o"),
    ]
    cpp_oracle = [
        *cxx,
        "-std=c++20",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Isrc",
        f"-I{VENDOR / 'libretro-common' / 'include'}",
        "src/harness/puae_oracle.cpp",
        str(BUILD / "log.o"),
        str(BUILD / "puae_options.o"),
        "-ldl",
        "-o",
        str(BUILD / "puae_oracle"),
    ]
    return core, c_log, c_options, cpp_oracle


def main() -> int:
    if not (VENDOR / "Makefile").is_file():
        raise SystemExit("puae-oracle: vendor/libretro-uae is missing; initialize that submodule")
    BUILD.mkdir(parents=True, exist_ok=True)
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    if not cc or not cxx:
        raise SystemExit("puae-oracle: CC and CXX must name compilers")
    core, *local = commands(cc, cxx)
    log_path = SCRATCH / "logs" / "puae-core-build.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run(core, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        with log_path.open(encoding="utf-8") as log:
            print("".join(deque(log, maxlen=40)), end="")
        raise SystemExit(f"puae-oracle: core build failed; full log: {log_path}")
    for command in local:
        subprocess.run(command, cwd=ROOT, check=True)
    for output in (BUILD / "puae_libretro.so", BUILD / "puae_oracle"):
        if not output.is_file():
            raise SystemExit(f"puae-oracle: missing build output: {output}")
    print(f"puae-oracle: built {BUILD / 'puae_oracle'} and source-matched libretro core")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
