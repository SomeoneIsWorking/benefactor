#!/usr/bin/env python3
"""Build the native Benefactor product for the current desktop host."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from tools.paths import ROOT
from tools.shared_checkouts import cmake_arguments, ensure_shared_checkouts

#: LaunchServices caches a bundle's icon against its path, and the launcher runs
#: the executable inside the bundle rather than opening the bundle, so nothing
#: ever tells macOS the icon changed. Measured: a new icon was built, committed
#: and sitting in the bundle while the Dock still drew the previous one.
ICON_STAMP = "icon-stamp"


def refresh_bundle_icon(bundle: Path) -> None:
    """Make macOS re-read the bundle when its icon has actually changed.

    Only on a change: re-registering is not free, and a launcher runs on every
    play. Best effort — a Dock that has already drawn the old icon may keep it
    until it restarts, and that is the Dock's cache, not the bundle's contents.
    """
    icon = bundle / "Contents/Resources/Benefactor.icns"
    if not icon.is_file():
        return
    stamp = bundle.parent / ICON_STAMP
    current = f"{icon.stat().st_size}:{icon.stat().st_mtime_ns}"
    if stamp.is_file() and stamp.read_text(encoding="utf-8") == current:
        return
    bundle.touch()
    register = Path(
        "/System/Library/Frameworks/CoreServices.framework/Frameworks"
        "/LaunchServices.framework/Support/lsregister"
    )
    if register.is_file():
        subprocess.run([str(register), "-f", str(bundle)], check=False)
    stamp.write_text(current, encoding="utf-8")


def build_product() -> Path:
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if cmake is None or ninja is None:
        missing = ", ".join(
            name for name, value in (("cmake", cmake), ("ninja", ninja)) if value is None
        )
        raise RuntimeError(f"required native build tools are missing: {missing}")

    shared = ensure_shared_checkouts(cmake)
    build = ROOT / "build" / "run"
    build.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            cmake,
            "-S",
            str(ROOT),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            *cmake_arguments(shared),
        ],
        cwd=ROOT,
        check=True,
    )
    subprocess.run(
        [cmake, "--build", str(build), "--target", "benefactor_product", "--parallel"],
        cwd=ROOT,
        check=True,
    )

    bundle = build / "Benefactor.app"
    if (bundle / "Contents/MacOS/Benefactor").is_file():
        refresh_bundle_icon(bundle)
        return bundle / "Contents/MacOS/Benefactor"
    executable = build / ("benefactor-pc.exe" if sys.platform == "win32" else "benefactor-pc")
    if not executable.is_file():
        raise RuntimeError(f"native build completed without its executable: {executable}")
    return executable
