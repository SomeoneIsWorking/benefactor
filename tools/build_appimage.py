#!/usr/bin/env python3
"""Stage and verify a disk-free Benefactor AppImage."""

from __future__ import annotations

import argparse
import os
import shutil
from pathlib import Path

from tools.release_common import (
    ROOT,
    ensure_disk_free,
    replace_directory,
    require_runtime,
    run,
)


def refuse(message: str) -> None:
    raise SystemExit(f"appimage: {message}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/linux")
    parser.add_argument("--appimagetool", type=Path)
    parser.add_argument(
        "--output", type=Path, default=ROOT / "build/release/Benefactor-x86_64.AppImage"
    )
    parser.add_argument("--stage-only", action="store_true")
    args = parser.parse_args()
    require_runtime("appimage")
    build = args.build_dir.resolve()
    replace_directory(build)
    run(
        "appimage",
        ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
    )
    run("appimage", ["cmake", "--build", str(build), "--target", "benefactor_product", "--parallel"])
    if not (build / "benefactor-pc").is_file():
        refuse(f"{build}/benefactor-pc is missing after the native build")
    appdir = ROOT / "build/appimage/Benefactor.AppDir"
    replace_directory(appdir)
    environment = dict(os.environ)
    environment["DESTDIR"] = str(appdir)
    run(
        "appimage",
        ["cmake", "--install", str(build), "--prefix", "/usr"],
        environment=environment,
    )
    (appdir / "AppRun").symlink_to("usr/bin/benefactor-pc")
    shutil.copy2(
        ROOT / "platforms/freedesktop/io.github.SomeoneIsWorking.benefactor.desktop",
        appdir / "io.github.SomeoneIsWorking.benefactor.desktop",
    )
    icon = ROOT / "platforms/freedesktop/io.github.SomeoneIsWorking.benefactor.svg"
    shutil.copy2(icon, appdir / ".DirIcon")
    shutil.copy2(icon, appdir / icon.name)
    ensure_disk_free(appdir, "appimage")
    if args.stage_only:
        print(f"appimage: staged {appdir}")
        return 0
    if not args.appimagetool or not args.appimagetool.is_file():
        refuse("--appimagetool must name a verified appimagetool executable")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    run("appimage", [str(args.appimagetool.resolve()), str(appdir), str(args.output.resolve())])
    if not args.output.is_file():
        refuse("appimagetool reported success but did not create an artifact")
    ensure_disk_free(appdir, "appimage")
    print(f"appimage: wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
