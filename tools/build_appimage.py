#!/usr/bin/env python3
"""Stage and verify a disk-free Benefactor AppImage."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def refuse(message: str) -> None:
    raise SystemExit(f"appimage: {message}")


def run(command: list[str], *, cwd: Path = ROOT, environment: dict[str, str] | None = None) -> None:
    print("appimage:", " ".join(command))
    subprocess.run(command, cwd=cwd, env=environment, check=True)


def ensure_disk_free(root: Path) -> None:
    files = [path.relative_to(root).as_posix() for path in root.rglob("Disk.*") if path.is_file()]
    if files:
        refuse("artifact staging contains original disk images: " + ", ".join(files))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/clang")
    parser.add_argument("--appimagetool", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/release/Benefactor-x86_64.AppImage")
    parser.add_argument("--stage-only", action="store_true")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    if not (build / "benefactor-pc").is_file():
        refuse(f"{build}/benefactor-pc is missing; build the desktop target first")
    appdir = ROOT / "build/appimage/Benefactor.AppDir"
    if appdir.exists():
        shutil.rmtree(appdir)
    appdir.mkdir(parents=True)
    environment = dict(os.environ)
    environment["DESTDIR"] = str(appdir)
    run(["cmake", "--install", str(build), "--prefix", "/usr"], environment=environment)
    shutil.copy2(ROOT / "platforms/appimage/AppRun", appdir / "AppRun")
    (appdir / "AppRun").chmod(0o755)
    shutil.copy2(ROOT / "platforms/freedesktop/io.github.SomeoneIsWorking.benefactor.desktop",
                 appdir / "io.github.SomeoneIsWorking.benefactor.desktop")
    icon = ROOT / "platforms/freedesktop/io.github.SomeoneIsWorking.benefactor.svg"
    shutil.copy2(icon, appdir / ".DirIcon")
    shutil.copy2(icon, appdir / icon.name)
    ensure_disk_free(appdir)
    if args.stage_only:
        print(f"appimage: staged {appdir}")
        return 0
    if not args.appimagetool or not args.appimagetool.is_file():
        refuse("--appimagetool must name a verified appimagetool executable")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    run([str(args.appimagetool.resolve()), str(appdir), str(args.output.resolve())])
    if not args.output.is_file():
        refuse("appimagetool reported success but did not create an artifact")
    ensure_disk_free(appdir)
    print(f"appimage: wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
