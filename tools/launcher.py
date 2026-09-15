from __future__ import annotations

import logging
import subprocess
from pathlib import Path

from tools.build_product import build_product
from tools.config import parse_launch_config
from tools.control_port import (
    choose_port,
    configured_port,
    forget_port,
    record_port,
)
from tools.disk_browse import browse_for_disks
from tools.disk_identity import validate_disk_set
from tools.paths import AMIGAPORT, amigaport_dir
from tools.shared_checkouts import ensure_shared_checkouts

LOGGER = logging.getLogger("benefactor.launcher")


def runtime_blocker(amigaport: Path = AMIGAPORT) -> str | None:
    if amigaport == AMIGAPORT:
        amigaport = amigaport_dir()
    if not amigaport.is_dir():
        return f"shared/amigaport is missing at {amigaport}"
    adapter = Path(__file__).resolve().parents[1] / "src/runtime/guest_runtime.cpp"
    if not adapter.is_file():
        return f"the Benefactor runtime adapter is missing at {adapter}"
    return None


def control_arguments(extra_args: tuple[str, ...]) -> list[str]:
    """`--http <port>`, unless this launch has said not to open the channel.

    The channel is opened by default: a player who reports something is the
    only chance there is to look at it while it is still happening, and asking
    them to restart under an environment variable first loses it. Passing
    `--http` explicitly, or setting `BENEFACTOR_HTTP`, decides the port instead;
    `BENEFACTOR_HTTP=0` (or `off`) leaves the channel closed.
    """
    if "--http" in extra_args:
        return []
    requested = configured_port()
    if requested == 0:
        return []
    port = requested if requested else choose_port()
    return ["--http", str(port)]


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    try:
        # A fresh clone has none of the shared trees the build consumes, so they
        # are resolved before anything reports one of them missing.
        ensure_shared_checkouts()
    except RuntimeError as error:
        LOGGER.error("shared checkouts: %s", error)
        return 2
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
        control = control_arguments(config.extra_args)
    except ValueError as error:
        LOGGER.error("%s", error)
        return 2

    try:
        executable = build_product()
        if control:
            port = int(control[1])
            record_port(port)
            LOGGER.info("control channel on http://127.0.0.1:%d/", port)
        subprocess.run(
            [
                str(executable),
                "--disk",
                *(str(path) for path in disks),
                *control,
                *config.extra_args,
            ],
            check=True,
        )
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        LOGGER.error("could not launch Benefactor: %s", error)
        return 2
    finally:
        forget_port()
    return 0
