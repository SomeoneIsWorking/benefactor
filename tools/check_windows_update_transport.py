#!/usr/bin/env python3
"""Run the Windows update transport against the live release service.

The Windows transport is the one owner no other platform's build can exercise:
it is WinHTTP-specific, and a successful compile says nothing about whether the
release address it is given and the API's own buffer rules produce a request that
works. This runs the check the Windows CI job builds, on Windows, so the gate is
a real fetch rather than an assumption.

It requires a Windows build tree (`tools/build_desktop.py --platform windows`) or
builds the one target into an existing tree.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from tools.release_common import ROOT, run

TARGET = "benefactor-winhttp-transport"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=ROOT / "build" / "windows")
    args = parser.parse_args()

    run("winhttp", ["cmake", "--build", str(args.build), "--target", TARGET, "--parallel"])
    executable = args.build / f"{TARGET}.exe"
    if not executable.is_file():
        raise SystemExit(f"winhttp: the build did not produce {executable}")
    run("winhttp", [str(executable)])
    print("winhttp: the Windows update transport completed a real release check")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
