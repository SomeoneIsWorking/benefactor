import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

#: Shared checkouts are consumed, never vendored. A developer keeps them beside
#: this repository (`../shared/<name>`); CI checks the same tree out inside it,
#: so both layouts are candidates and a miss names every path that was tried.
SHARED_LAYOUTS = (ROOT / "shared", ROOT.parent / "shared")

#: The developer-layout amigaport path, kept as the sentinel callers compare
#: against when they mean "wherever the resolver finds it".
AMIGAPORT = ROOT.parent / "shared" / "amigaport"


def shared_checkout(override: str, name: str) -> Path:
    """The shared `name` checkout: an explicit override, else a known layout."""
    candidates = ([Path(override).expanduser()] if override else []) + [
        layout / name for layout in SHARED_LAYOUTS
    ]
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved.is_dir():
            return resolved
    tried = ", ".join(str(candidate) for candidate in candidates)
    raise SystemExit(f"shared/{name} is missing; tried {tried}")


def amigaport_dir() -> Path:
    return shared_checkout(os.environ.get("BENEFACTOR_AMIGAPORT_DIR", ""), "amigaport")


def setup_ui_dir() -> Path:
    return shared_checkout(os.environ.get("BENEFACTOR_SETUP_UI_DIR", ""), "setup-ui")


def port_assets_dir() -> Path:
    return shared_checkout(os.environ.get("PORT_ASSETS_DIR", ""), "port-assets")


SCRATCH = ROOT / "scratch"
HARNESS_ACTIVITY = SCRATCH / "harness-puae"
DISK_NAMES = ("Disk.1", "Disk.2", "Disk.3")
