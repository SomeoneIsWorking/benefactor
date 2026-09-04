from __future__ import annotations

import logging
from pathlib import Path

from tools.config import parse_launch_config
from tools.disk_identity import validate_disk_set
from tools.paths import AMIGAPORT

LOGGER = logging.getLogger("benefactor.launcher")


def runtime_blocker(amigaport: Path = AMIGAPORT) -> str | None:
    if not amigaport.is_dir():
        return f"shared/amigaport is missing at {amigaport}"
    return "the Benefactor runtime adapter to shared/amigaport is not implemented"


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    blocker = runtime_blocker()
    if blocker:
        LOGGER.error("Benefactor gameplay product unavailable: %s", blocker)
        return 2

    config = parse_launch_config(argv)
    try:
        validate_disk_set(config.disks)
    except ValueError as error:
        LOGGER.error("%s", error)
        return 2

    LOGGER.error("Benefactor gameplay product unavailable: runtime composition is incomplete")
    return 2
