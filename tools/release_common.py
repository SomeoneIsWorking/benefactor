from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from tools.launcher import runtime_blocker

ROOT = Path(__file__).resolve().parents[1]


def refuse(prefix: str, message: str) -> None:
    raise SystemExit(f"{prefix}: {message}")


def require_runtime(prefix: str) -> None:
    blocker = runtime_blocker()
    if blocker:
        refuse(prefix, f"Benefactor gameplay product unavailable: {blocker}")


def run(
    prefix: str,
    command: list[str],
    *,
    cwd: Path = ROOT,
    environment: dict[str, str] | None = None,
) -> None:
    print(prefix + ":", " ".join(command))
    subprocess.run(command, cwd=cwd, env=environment, check=True)


def replace_directory(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)


def ensure_disk_free(root: Path, prefix: str) -> None:
    forbidden = sorted(
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
        and path.name.lower()
        in {
            "disk.1",
            "disk.2",
            "disk.3",
            "kick40068.a1200",
            "benefactor.slave",
            "whdload.hdf",
            "whdsaves.hdf",
        }
    )
    if forbidden:
        refuse(prefix, "artifact staging contains player-owned files: " + ", ".join(forbidden))
