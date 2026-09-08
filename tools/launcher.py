from __future__ import annotations

import logging
import os
import subprocess
from pathlib import Path

from tools.build_product import build_product
from tools.config import parse_launch_config
from tools.disk_browse import browse_for_disks
from tools.disk_identity import validate_disk_set
from tools.paths import AMIGAPORT

LOGGER = logging.getLogger("benefactor.launcher")


def runtime_blocker(amigaport: Path = AMIGAPORT) -> str | None:
    configured = os.environ.get("BENEFACTOR_AMIGAPORT_DIR")
    if configured and amigaport == AMIGAPORT:
        amigaport = Path(configured).expanduser().resolve()
    if not amigaport.is_dir():
        return f"shared/amigaport is missing at {amigaport}"
    adapter = Path(__file__).resolve().parents[1] / "src/runtime/guest_runtime.cpp"
    if not adapter.is_file():
        return f"the Benefactor runtime adapter is missing at {adapter}"
    return None


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    blocker = runtime_blocker()
    if blocker:
        LOGGER.error("Benefactor gameplay product unavailable: %s", blocker)
        return 2

    config = parse_launch_config(argv)
    disks = config.disks
    if config.browse:
        try:
            disks = browse_for_disks(Path.cwd())
        except (RuntimeError, ValueError) as error:
            LOGGER.error("disk browser: %s", error)
            return 2
    try:
        validate_disk_set(disks)
    except ValueError as error:
        LOGGER.error("%s", error)
        return 2

    try:
        executable = build_product()
        subprocess.run([str(executable), "--disk", *(str(path) for path in disks)], check=True)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        LOGGER.error("could not launch Benefactor: %s", error)
        return 2
    return 0
