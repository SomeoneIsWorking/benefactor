from __future__ import annotations

import argparse
import os
from dataclasses import dataclass
from pathlib import Path

from tools.paths import DISK_NAMES, ROOT


@dataclass(frozen=True)
class LaunchConfig:
    disks: tuple[Path, Path, Path]


def _read_dotenv(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{path}:{line_number}: expected NAME=VALUE")
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def parse_launch_config(argv: list[str] | None = None) -> LaunchConfig:
    parser = argparse.ArgumentParser(description="Launch the Benefactor native/interpreter port")
    parser.add_argument("--disk1", type=Path)
    parser.add_argument("--disk2", type=Path)
    parser.add_argument("--disk3", type=Path)
    args = parser.parse_args(argv)

    dotenv = _read_dotenv(ROOT / ".env")
    resolved: list[Path] = []
    for index, name in enumerate(DISK_NAMES, 1):
        explicit = getattr(args, f"disk{index}")
        configured = os.environ.get(f"BENEFACTOR_DISK{index}") or dotenv.get(
            f"BENEFACTOR_DISK{index}"
        )
        candidate = explicit or (Path(configured) if configured else ROOT / name)
        resolved.append(candidate.resolve())
    return LaunchConfig(disks=(resolved[0], resolved[1], resolved[2]))
