#!/usr/bin/env python3
"""Make the shared trees this product builds against available, at their pins.

`./run.sh` is the fresh-clone interface, and a fresh clone has none of the
shared trees the native build consumes: the 68000 runtime, the logging and
control library, the first-run setup screen, and that screen's renderer. They
are consumed, never vendored, so each one is resolved here — a developer's own
checkout when there is one, otherwise a copy checked out at the revision
`.github/workflows/release.yml` pins, beside the build rather than inside
anyone else's working tree.

A developer's checkout is kept: work in progress and commits of its own are
left alone and reported. One that is clean and only behind the pin is moved
onto it, because a checkout the build cannot compile against is the other way
this fails — a `lucent` 24 commits behind the pin surfaced as a missing
`lucent/version.h` several minutes into the build.
"""

from __future__ import annotations

import logging
import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from tools.paths import ROOT
from tools.pinned_revisions import pinned_revisions

LOGGER = logging.getLogger("benefactor.shared")

WORKFLOW = ROOT / ".github/workflows/release.yml"

#: Written into a checkout this module made, so it can be moved to a later pin
#: without ever refreshing a tree somebody else put there.
STAMP = ".benefactor-pinned"


@dataclass(frozen=True)
class SharedTree:
    """One shared checkout: where it may already be, and where it comes from."""

    name: str
    repository: str
    #: CMake cache entry the build reads the resolved path from.
    variable: str
    #: Existing layouts, in order: beside this repository, then inside it.
    candidates: tuple[Path, ...]
    #: Where a pinned copy is checked out when no candidate exists.
    provision: Path
    submodules: bool = False


TREES = (
    SharedTree(
        name="amigaport",
        repository="SomeoneIsWorking/amigaport",
        variable="BENEFACTOR_AMIGAPORT_DIR",
        candidates=(ROOT.parent / "shared/amigaport", ROOT / "shared/amigaport"),
        provision=ROOT / "shared/amigaport",
        submodules=True,
    ),
    SharedTree(
        name="lucent",
        repository="SomeoneIsWorking/lucent",
        variable="BENEFACTOR_LUCENT_DIR",
        candidates=(ROOT.parent / "lucent", ROOT / "dependencies/lucent"),
        provision=ROOT / "dependencies/lucent",
    ),
    SharedTree(
        name="setup-ui",
        repository="SomeoneIsWorking/setup-ui",
        variable="BENEFACTOR_SETUP_UI_DIR",
        candidates=(ROOT.parent / "shared/setup-ui", ROOT / "shared/setup-ui"),
        provision=ROOT / "shared/setup-ui",
    ),
    SharedTree(
        name="RmlUi",
        repository="mikke89/RmlUi",
        variable="SETUP_UI_RMLUI_DIR",
        candidates=(
            ROOT.parent / "shared/RmlUi",
            ROOT.parent / "RmlUi",
            ROOT / "dependencies/RmlUi",
        ),
        provision=ROOT / "dependencies/RmlUi",
    ),
)


def pinned_revision(repository: str) -> str:
    """The single revision the release workflow pins for `repository`."""
    revisions = {pin.revision for pin in pinned_revisions(WORKFLOW) if pin.repository == repository}
    if len(revisions) != 1:
        raise RuntimeError(
            f"{WORKFLOW.name} pins {len(revisions)} revisions for {repository}; "
            "it must name exactly one"
        )
    return revisions.pop()


def _git(*arguments: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", *arguments], cwd=cwd, text=True, capture_output=True, check=False)


def _holds_revision(checkout: Path, revision: str) -> bool:
    """Whether the object is present at all — fetched, not necessarily built."""
    return _git("cat-file", "-e", f"{revision}^{{commit}}", cwd=checkout).returncode == 0


def _contains_revision(checkout: Path, revision: str) -> bool:
    """Whether what is checked out includes the revision: on it, or past it."""
    if not _holds_revision(checkout, revision):
        return False
    return _git("merge-base", "--is-ancestor", revision, "HEAD", cwd=checkout).returncode == 0


def _describe_age(checkout: Path, revision: str) -> str:
    """How a checkout that predates the pin differs from it, in one clause."""
    if not _holds_revision(checkout, revision):
        return "does not hold that revision even after a fetch"
    behind = _git("rev-list", "--count", f"HEAD..{revision}", cwd=checkout)
    if behind.returncode != 0:
        return "is at an unrelated revision"
    return f"is {behind.stdout.strip()} commit(s) behind it"


def _dirty(checkout: Path) -> bool:
    status = _git("status", "--porcelain", cwd=checkout)
    return status.returncode != 0 or bool(status.stdout.strip())


def _align_checkout(tree: SharedTree, checkout: Path, revision: str) -> None:
    """Move a checkout that only predates the pin onto it; never past work.

    A developer's tree is theirs: anything uncommitted, or any commit of their
    own, is left exactly as it is and reported. A clean checkout that is purely
    behind the pin is fast-forwarded, which is the step whose absence used to
    read as a missing header halfway through the build.
    """
    if _git("rev-parse", "--git-dir", cwd=checkout).returncode != 0:
        return
    if _contains_revision(checkout, revision):
        return
    _git("fetch", "--quiet", "origin", cwd=checkout)
    if not _holds_revision(checkout, revision):
        LOGGER.warning(
            "%s at %s cannot reach the pinned %s; the build uses it as it stands",
            tree.name,
            checkout,
            revision[:12],
        )
        return
    behind_only = (
        _git("merge-base", "--is-ancestor", "HEAD", revision, cwd=checkout).returncode == 0
    )
    if not behind_only or _dirty(checkout):
        LOGGER.warning(
            "%s at %s carries work of its own and %s the pinned %s; the build "
            "uses it as it stands. Set %s to build against another tree.",
            tree.name,
            checkout,
            _describe_age(checkout, revision),
            revision[:12],
            tree.variable,
        )
        return
    previous = _git("rev-parse", "--short", "HEAD", cwd=checkout).stdout.strip()
    result = _git("merge", "--ff-only", revision, cwd=checkout)
    if result.returncode != 0:
        LOGGER.warning(
            "%s at %s stays at %s: %s",
            tree.name,
            checkout,
            previous,
            (result.stderr or result.stdout).strip(),
        )
        return
    if tree.submodules:
        _git("submodule", "update", "--init", "--recursive", "--quiet", cwd=checkout)
    LOGGER.info(
        "%s at %s moved from %s to the pinned %s", tree.name, checkout, previous, revision[:12]
    )


def _clone(tree: SharedTree, revision: str) -> Path:
    """Check the pinned revision out, without a history nobody here reads."""
    destination = tree.provision
    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    LOGGER.info("checking %s out at %s into %s", tree.name, revision[:12], destination)
    url = f"https://github.com/{tree.repository}.git"
    steps = [
        ("init", "--quiet", str(destination)),
        ("-C", str(destination), "remote", "add", "origin", url),
        ("-C", str(destination), "fetch", "--quiet", "--depth", "1", "origin", revision),
        ("-C", str(destination), "checkout", "--quiet", "FETCH_HEAD"),
    ]
    if tree.submodules:
        steps.append(
            (
                "-C",
                str(destination),
                "submodule",
                "update",
                "--init",
                "--recursive",
                "--depth",
                "1",
                "--quiet",
            )
        )
    for step in steps:
        result = _git(*step)
        if result.returncode != 0:
            shutil.rmtree(destination, ignore_errors=True)
            raise RuntimeError(
                f"could not check {tree.repository} out at {revision[:12]}: "
                f"{(result.stderr or result.stdout).strip()}"
            )
    (destination / STAMP).write_text(f"{tree.repository} {revision}\n", encoding="utf-8")
    return destination


#: Resolution is reported and may fetch, so each tree is resolved once a run.
_RESOLVED: dict[str, Path] = {}


def resolve_tree(tree: SharedTree, *, provision: bool = True) -> Path:
    """An existing checkout of `tree`, or a copy at the pinned revision."""
    if tree.name in _RESOLVED:
        return _RESOLVED[tree.name]
    resolved = _resolve_tree(tree, provision=provision)
    _RESOLVED[tree.name] = resolved
    return resolved


def _resolve_tree(tree: SharedTree, *, provision: bool) -> Path:
    override = os.environ.get(tree.variable, "")
    if override:
        checkout = Path(override).expanduser().resolve()
        if not checkout.is_dir():
            raise RuntimeError(f"{tree.variable} is not a directory: {checkout}")
        return checkout
    revision = pinned_revision(tree.repository)
    for candidate in tree.candidates:
        if not candidate.is_dir():
            continue
        checkout = candidate.resolve()
        if (checkout / STAMP).is_file():
            # This module's own copy. It follows the pin, and re-checking it
            # out costs nothing anybody typed.
            if _contains_revision(checkout, revision):
                return checkout
            return _clone(tree, revision).resolve()
        _align_checkout(tree, checkout, revision)
        return checkout
    if not provision:
        tried = ", ".join(str(candidate) for candidate in tree.candidates)
        raise RuntimeError(f"shared {tree.name} is missing; tried {tried}")
    if shutil.which("git") is None:
        raise RuntimeError(
            f"shared {tree.name} is missing and git is not installed to check it out"
        )
    return _clone(tree, revision).resolve()


def _host_freetype(cmake: str) -> bool:
    """Whether the host already provides the font engine RmlUi renders with."""
    with tempfile.TemporaryDirectory() as directory:
        probe = Path(directory)
        (probe / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(freetype_probe NONE)\n"
            "find_package(Freetype QUIET)\n"
            "if(NOT Freetype_FOUND AND NOT FREETYPE_FOUND)\n"
            '    message(FATAL_ERROR "no Freetype")\n'
            "endif()\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [cmake, "-S", str(probe), "-B", str(probe / "build")],
            text=True,
            capture_output=True,
            check=False,
        )
    return result.returncode == 0


FREETYPE = SharedTree(
    name="freetype",
    repository="freetype/freetype",
    variable="SETUP_UI_FREETYPE_DIR",
    candidates=(ROOT.parent / "freetype", ROOT / "dependencies/freetype"),
    provision=ROOT / "dependencies/freetype",
)


def ensure_shared_checkouts(cmake: str | None = None) -> dict[str, Path]:
    """Every shared tree the desktop build needs, as CMake cache entries."""
    resolved = {tree.variable: resolve_tree(tree) for tree in TREES}
    if cmake is not None and not _host_freetype(cmake):
        # setup-ui builds a checkout statically when no host package answers
        # find_package(Freetype); a fresh host has none.
        resolved[FREETYPE.variable] = resolve_tree(FREETYPE)
    return resolved


def cmake_arguments(resolved: dict[str, Path]) -> list[str]:
    return [f"-D{variable}={path}" for variable, path in sorted(resolved.items())]


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    for variable, path in sorted(ensure_shared_checkouts(shutil.which("cmake")).items()):
        print(f"{variable}={path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
