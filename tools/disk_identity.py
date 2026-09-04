from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class DiskIdentity:
    name: str
    size: int
    sha256: str


EXPECTED_DISKS = (
    DiskIdentity(
        "Disk.1", 1003520, "25416a6e390cbe94e4b2375c9513a2adf3411072fc5b6069ea34a0f3ff697916"
    ),
    DiskIdentity(
        "Disk.2", 1003520, "f3649c8db4adfce3c7da5e21cb018be098404771eceeec44741c2528e9071b73"
    ),
    DiskIdentity(
        "Disk.3", 1003520, "8dd262d02174a6706d5214b25f7bd9fc4bffe94761e16c209b880bc1dd8e7a42"
    ),
)


def validate_disk(path: Path, expected: DiskIdentity) -> None:
    if not path.is_file():
        raise ValueError(f"{expected.name}: file not found: {path}")
    size = path.stat().st_size
    if size != expected.size:
        raise ValueError(f"{expected.name}: expected {expected.size} bytes, got {size}: {path}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != expected.sha256:
        raise ValueError(f"{expected.name}: SHA-256 mismatch: {path}")


def validate_disk_set(paths: tuple[Path, Path, Path]) -> None:
    for path, expected in zip(paths, EXPECTED_DISKS, strict=True):
        validate_disk(path, expected)
