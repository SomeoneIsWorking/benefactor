#!/usr/bin/env python3
"""Build and stage the browser product without bundling player disks."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

from tools.release_common import ROOT, ensure_disk_free, replace_directory, require_runtime, run


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    require_runtime("wasm")
    output = args.output.resolve()
    build = ROOT / "build" / "wasm"
    replace_directory(build)
    run(
        "wasm",
        [
            "emcmake",
            "cmake",
            "-S",
            str(ROOT),
            "-B",
            str(build),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBENEFACTOR_WEB=ON",
        ],
    )
    run("wasm", ["cmake", "--build", str(build), "--target", "benefactor_web", "--parallel"])
    replace_directory(output)
    for name in ("benefactor.js", "benefactor.wasm"):
        source = build / name
        if not source.is_file():
            raise SystemExit(f"wasm: required build output is missing: {source}")
        shutil.copy2(source, output / name)
    # The page and its two bridges to what only a browser can do: the file
    # chooser and the network stack the update check uses.
    for name in (
        "index.html",
        "icon.svg",
        "disk_setup.js",
        "release_check.js",
        "isolation.mjs",
        "service-worker.js",
    ):
        shutil.copy2(ROOT / "platforms/web" / name, output / name)
    ensure_disk_free(output, "wasm")
    print(f"wasm: staged {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
