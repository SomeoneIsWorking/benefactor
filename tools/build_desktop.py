#!/usr/bin/env python3
"""Build and package the native desktop product for Windows or macOS."""

from __future__ import annotations

import argparse
import plistlib
import shutil
import zipfile
from pathlib import Path

from tools import draw_app_icon
from tools.release_common import ROOT, ensure_disk_free, replace_directory, require_runtime, run

WINDOWS_ICON = ROOT / "platforms/windows/benefactor.ico"
WINDOWS_ICON_FRAME = 256
MACOS_ICON_NAME = "Benefactor.icns"


def check_windows_icon(executable: Path) -> None:
    """A Windows player only ever sees the icon compiled into the executable.

    A resource compiler rebuilds the container around the .ico's frames, so the
    frame's bitmap is what ends up in the executable. The checked artifact is the
    executable this function is handed, not the .rc beside it.
    """
    signature = draw_app_icon.ico_frame_bytes(WINDOWS_ICON, WINDOWS_ICON_FRAME)
    if signature not in executable.read_bytes():
        raise SystemExit(
            f"desktop: {executable.name} carries no icon resource; the build did not "
            f"compile {WINDOWS_ICON.relative_to(ROOT)}"
        )
    print(
        f"desktop: {executable.name} carries the {WINDOWS_ICON_FRAME}x{WINDOWS_ICON_FRAME} "
        f"frame of the {len(draw_app_icon.ico_frames(WINDOWS_ICON))}-size icon"
    )


def check_macos_icon(app: Path) -> None:
    """A .app shows an icon only if the bundle names one and actually holds it."""
    resources = app / "Contents/Resources" / MACOS_ICON_NAME
    if not resources.is_file():
        raise SystemExit(f"desktop: {app.name} has no Contents/Resources/{MACOS_ICON_NAME}")
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    if info.get("CFBundleIconFile") != MACOS_ICON_NAME:
        raise SystemExit(
            f"desktop: {app.name} names {info.get('CFBundleIconFile')!r} as its icon, "
            f"not {MACOS_ICON_NAME}"
        )
    print(
        f"desktop: {app.name} carries {MACOS_ICON_NAME} "
        f"({resources.stat().st_size} B) and names it in its Info.plist"
    )


def package_windows(build: Path, output: Path) -> None:
    install = build / "install"
    executable = next(install.rglob("benefactor-pc.exe"), None)
    if executable is None:
        raise SystemExit(f"desktop: Windows install did not produce {install}/benefactor-pc.exe")
    check_windows_icon(executable)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(install.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(install).as_posix())
    ensure_disk_free(output.parent, "desktop")


def package_macos(build: Path, output: Path) -> None:
    app = next((build / "install").rglob("Benefactor.app"), None)
    if app is None or not app.is_dir():
        raise SystemExit(f"desktop: macOS build did not produce {build}/Benefactor.app")
    check_macos_icon(app)
    ensure_disk_free(app, "desktop")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.suffix == ".zip":
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(app.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(app.parent).as_posix())
    elif output.name == "Benefactor.app":
        if output.exists():
            shutil.rmtree(output)
        shutil.copytree(app, output)
    else:
        raise SystemExit("desktop: macOS output must be Benefactor.app or a .zip archive")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=("windows", "macos"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    require_runtime("desktop")
    build = ROOT / "build" / args.platform
    replace_directory(build)
    run(
        "desktop",
        ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
    )
    run("desktop", ["cmake", "--build", str(build), "--target", "benefactor_product", "--parallel"])
    install = build / "install"
    replace_directory(install)
    run("desktop", ["cmake", "--install", str(build), "--prefix", str(install)])
    if args.platform == "windows":
        package_windows(build, args.output.resolve())
    else:
        package_macos(build, args.output.resolve())
    print(f"desktop: wrote {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
