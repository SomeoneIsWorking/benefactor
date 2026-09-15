import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _main_checkout(root: Path) -> Path:
    """The checkout the shared trees sit beside.

    Usually `root` itself. Not when the work is happening in a linked git
    worktree: that lives under `.claude/worktrees/<name>`, so "beside this
    repository" from inside one is `.claude/worktrees`, where none of the shared
    trees are and none of them ever will be. Every candidate list here means the
    main checkout, and a worktree names it in its own `.git` file — which is a
    file rather than a directory, and that is how a worktree is recognised.
    """
    marker = root / ".git"
    if not marker.is_file():
        return root
    named = marker.read_text(encoding="utf-8").strip()
    if not named.startswith("gitdir:"):
        return root
    gitdir = Path(named.split(":", 1)[1].strip())
    if not gitdir.is_absolute():
        gitdir = (root / gitdir).resolve()
    # <main>/.git/worktrees/<name> — anything else is a layout we do not know.
    if gitdir.parent.name == "worktrees" and gitdir.parent.parent.name == ".git":
        return gitdir.parent.parent.parent
    return root


#: What `..` means when a path is looked for beside this repository. See above:
#: from a worktree that is the main checkout, not the worktree's own parent.
CHECKOUT = _main_checkout(ROOT)
BESIDE = CHECKOUT.parent

#: Shared checkouts are consumed, never vendored. A developer keeps them beside
#: this repository (`../shared/<name>`); CI checks the same tree out inside it,
#: so both layouts are candidates and a miss names every path that was tried.
SHARED_LAYOUTS = (ROOT / "shared", BESIDE / "shared")

#: The developer-layout amigaport path, kept as the sentinel callers compare
#: against when they mean "wherever the resolver finds it".
AMIGAPORT = BESIDE / "shared" / "amigaport"


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
