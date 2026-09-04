#!/usr/bin/env python3
"""Capture a test-only PUAE oracle memory image at the main-image sync point.

The separately built oracle writes its capture below the stable
``scratch/harness-puae`` activity directory after it reaches the deterministic
boot checkpoint. This tool drives that oracle to the checkpoint and copies the
result to the oracle-analysis scratch directory. The capture is RE evidence
only: it is never a gameplay input or source-generation substrate.
"""

from __future__ import annotations

import argparse
import logging
import os
import shutil
import subprocess
from pathlib import Path

from tools.paths import HARNESS_ACTIVITY, ROOT, SCRATCH

LOGGER = logging.getLogger("benefactor.oracle-capture")
DEFAULT_ORACLE = ROOT / "build" / "test-oracle" / "benefactor-puae-oracle"
ORACLE_DUMP = HARNESS_ACTIVITY / "harness_puae_chipram.bin"
DEFAULT_OUTPUT = SCRATCH / "oracle" / "main_chipram.bin"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--oracle", type=Path, default=DEFAULT_ORACLE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--kick-dir", type=Path, default=ROOT / "harness")
    parser.add_argument("--whdload", type=Path, default=ROOT / "harness" / "Benefactor.slave")
    parser.add_argument("disks", nargs=3, type=Path, metavar="DISK")
    return parser


def _run_oracle(oracle: Path, arguments: argparse.Namespace) -> int:
    environment = dict(os.environ)
    environment["SDL_VIDEODRIVER"] = "offscreen"
    command = [
        str(oracle),
        str(arguments.kick_dir),
        str(arguments.whdload),
        *(str(path) for path in arguments.disks),
    ]
    result = subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        input="q\n",
        text=True,
        check=False,
    )
    return result.returncode


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    arguments = _parser().parse_args(argv)
    oracle = arguments.oracle.resolve()
    if not oracle.is_file():
        LOGGER.error(
            "separate PUAE oracle is missing: %s; the gameplay build never supplies this target",
            oracle,
        )
        return 2

    ORACLE_DUMP.parent.mkdir(parents=True, exist_ok=True)
    ORACLE_DUMP.unlink(missing_ok=True)
    result = _run_oracle(oracle, arguments)
    if not ORACLE_DUMP.is_file():
        LOGGER.error("oracle produced no capture at %s", ORACLE_DUMP)
        return result or 3

    output = arguments.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ORACLE_DUMP, output)
    LOGGER.info("wrote %s (%d bytes)", output, output.stat().st_size)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
