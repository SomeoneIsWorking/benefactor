#!/usr/bin/env python3
"""Build the native Benefactor product for the current desktop host."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from tools.paths import ROOT


def build_product() -> Path:
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if cmake is None or ninja is None:
        missing = ", ".join(
            name for name, value in (("cmake", cmake), ("ninja", ninja)) if value is None
        )
        raise RuntimeError(f"required native build tools are missing: {missing}")

    build = ROOT / "build" / "run"
    build.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [cmake, "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        cwd=ROOT,
        check=True,
    )
    subprocess.run(
        [cmake, "--build", str(build), "--target", "benefactor_product", "--parallel"],
        cwd=ROOT,
        check=True,
    )

    if (build / "Benefactor.app/Contents/MacOS/Benefactor").is_file():
        return build / "Benefactor.app/Contents/MacOS/Benefactor"
    executable = build / ("benefactor-pc.exe" if sys.platform == "win32" else "benefactor-pc")
    if not executable.is_file():
        raise RuntimeError(f"native build completed without its executable: {executable}")
    return executable
