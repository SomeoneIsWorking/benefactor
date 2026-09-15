"""Find the shared re-harness checkout, which owns the cross-project gates.

`re-harness` is tooling rather than a build input, so it is not pinned or
checked out the way `shared/amigaport` and the rest are: a developer either has
it beside this repository or they do not. What matters is that a gate which
cannot run says so by name instead of quietly passing, which is why this returns
None and lets the caller report the absence.
"""

from __future__ import annotations

import os
from pathlib import Path

from tools.paths import ROOT

#: Where the checkout is, in the order a developer is likely to have put it.
#: `RE_HARNESS_DIR` overrides all of them.
CANDIDATES = (
    ROOT.parent / "re-harness",
    ROOT.parent / "shared/re-harness",
    ROOT / "shared/re-harness",
)


def re_harness_dir() -> Path | None:
    """The re-harness checkout, or None when this host has none."""
    override = os.environ.get("RE_HARNESS_DIR")
    if override:
        candidate = Path(override).expanduser()
        return candidate if (candidate / "tools").is_dir() else None
    for candidate in CANDIDATES:
        if (candidate / "tools").is_dir():
            return candidate
    return None


def harness_tool(name: str) -> Path | None:
    """One tool from the shared checkout, or None when it cannot be found."""
    directory = re_harness_dir()
    if directory is None:
        return None
    tool = directory / "tools" / name
    return tool if tool.is_file() else None


def main() -> int:
    """Print where the shared checkout was found, or say that it was not."""
    directory = re_harness_dir()
    if directory is None:
        looked = ", ".join(str(path) for path in CANDIDATES)
        print(f"re-harness: not found (looked in {looked}); set RE_HARNESS_DIR")
        return 1
    print(f"re-harness: {directory}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
