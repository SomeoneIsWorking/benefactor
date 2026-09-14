"""The product version, read from its one source.

`version.txt` at the repository root is the single authority: the CMake build
compiles it into the binary, the Android package derives its `versionName` and
`versionCode` from it, the release publisher refuses a tag that disagrees with
it, and the verifier states it when it builds a host-specific check. Nothing
else parses that file, so those consumers cannot drift apart.
"""

from __future__ import annotations

import re

from tools.paths import ROOT

VERSION_FILE = ROOT / "version.txt"

# Anchored so a stray suffix cannot pass as a release version.
VERSION_PATTERN = re.compile(r"\d+\.\d+\.\d+")


def read_version() -> str:
    """The product version, refusing a file that is not MAJOR.MINOR.PATCH."""
    version = VERSION_FILE.read_text(encoding="utf-8").strip()
    if VERSION_PATTERN.fullmatch(version) is None:
        raise SystemExit(f"version.txt must hold MAJOR.MINOR.PATCH, got '{version}'")
    return version


def release_tag() -> str:
    """The tag a release of this version is published under."""
    return f"v{read_version()}"


def version_code(version: str) -> int:
    """Android's monotonically increasing versionCode for a MAJOR.MINOR.PATCH."""
    major, minor, patch = (int(part) for part in version.split("."))
    return major * 10000 + minor * 100 + patch
