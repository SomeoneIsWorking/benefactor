#!/usr/bin/env python3
"""Build and package the native desktop product for Windows or macOS."""

from __future__ import annotations

import argparse
import shutil
import zipfile
from pathlib import Path

from tools.release_common import ROOT, ensure_disk_free, replace_directory, require_runtime, run


def package_windows(build: Path, output: Path) -> None:
    install = build / "install"
    executable = next(install.rglob("benefactor-pc.exe"), None)
    if executable is None:
        raise SystemExit(f"desktop: Windows install did not produce {install}/benefactor-pc.exe")
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
