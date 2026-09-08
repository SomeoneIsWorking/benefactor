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
    shutil.copy2(ROOT / "platforms/web/index.html", output / "index.html")
    shutil.copy2(ROOT / "platforms/web/disk_setup.js", output / "disk_setup.js")
    ensure_disk_free(output, "wasm")
    print(f"wasm: staged {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
