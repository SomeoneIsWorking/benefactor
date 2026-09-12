#!/usr/bin/env python3
"""Publish a qualified, asset-free GitHub release from this workflow's packages."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import zipfile
from pathlib import Path, PurePosixPath

from tools.paths import ROOT
from tools.release_common import ensure_disk_free

PACKAGE_NAMES = (
    "Benefactor-windows-x86_64.zip",
    "Benefactor-macos-arm64.zip",
    "Benefactor-x86_64.AppImage",
    "Benefactor-arm64-v8a-release.apk",
)
WEB_NAMES = ("index.html", "disk_setup.js", "benefactor.js", "benefactor.wasm")
RELEASE_STATE_IDS = (
    "S001",
    "S004",
    "S005",
    "S020",
    "S021",
    "S022",
    "S023",
    "S026",
    "S027",
    "S028",
    "S029",
    "S030",
    "S031",
)
FORBIDDEN_FILENAMES = {
    "disk.1",
    "disk.2",
    "disk.3",
    "benefactor.slave",
    "kick40068.a1200",
    "whdload.hdf",
    "whdsaves.hdf",
}


def require_qualified_state(state_document: str) -> None:
    states: dict[str, str] = {}
    for line in state_document.splitlines():
        fields = [field.strip() for field in line.strip().strip("|").split("|")]
        if len(fields) < 3 or re.fullmatch(r"S\d{3}", fields[0]) is None:
            continue
        if fields[0] in states:
            raise SystemExit(f"release: duplicate state item {fields[0]}")
        states[fields[0]] = fields[2]
    unverified = [
        f"{item}={states.get(item, 'missing')}"
        for item in RELEASE_STATE_IDS
        if states.get(item) != "verified"
    ]
    if unverified:
        raise SystemExit("release: product is not qualified: " + ", ".join(unverified))


def inspect_archive(path: Path) -> None:
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise SystemExit(f"release: duplicate archive entries in {path.name}")
        for name in names:
            member = PurePosixPath(name)
            if (
                member.is_absolute()
                or ".." in member.parts
                or "\\" in name
                or member.name.lower() in FORBIDDEN_FILENAMES
            ):
                raise SystemExit(f"release: unsafe or player-owned entry in {path.name}: {name}")
        if archive.testzip() is not None:
            raise SystemExit(f"release: corrupt archive member in {path.name}")
        if path.name == "Benefactor-windows-x86_64.zip":
            valid = any(name.lower().endswith("benefactor-pc.exe") for name in names)
        elif path.name == "Benefactor-macos-arm64.zip":
            valid = "Benefactor.app/Contents/MacOS/Benefactor" in names
        else:
            valid = "lib/arm64-v8a/libmain.so" in names and "resources.arsc" in names
        if not valid:
            raise SystemExit(f"release: {path.name} lacks its expected product entry")


def stage_assets(root: Path) -> tuple[Path, ...]:
    if not root.is_dir():
        raise SystemExit(f"release: downloaded artifacts are missing: {root}")
    ensure_disk_free(root, "release")
    packages = tuple(root / name for name in PACKAGE_NAMES)
    missing = [path.name for path in packages if not path.is_file() or path.stat().st_size == 0]
    missing += [name for name in WEB_NAMES if not (root / name).is_file()]
    if missing:
        raise SystemExit("release: required package output is missing: " + ", ".join(missing))
    for path in packages:
        if path.suffix in (".zip", ".apk"):
            inspect_archive(path)
        else:
            with path.open("rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    raise SystemExit("release: Linux package is not an ELF AppImage")
    checksums = root / "SHA256SUMS"
    lines = []
    for path in packages:
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        lines.append(f"{digest}  {path.name}\n")
    checksums.write_text("".join(lines), encoding="utf-8")
    return (*packages, checksums)


def release_command(tag: str, assets: tuple[Path, ...]) -> list[str]:
    return [
        "gh",
        "release",
        "create",
        tag,
        *(str(path) for path in assets),
        "--verify-tag",
        "--fail-on-no-commits",
        "--title",
        f"Benefactor {tag}",
        "--notes",
        "Bring your own unmodified Disk.1, Disk.2, and Disk.3. "
        "No game files are included. Use the in-app Browse setup to select them. "
        "The browser build is available on GitHub Pages.",
        "--generate-notes",
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    if re.fullmatch(r"v\d+\.\d+\.\d+", args.tag) is None:
        raise SystemExit("release: tag must be a stable vMAJOR.MINOR.PATCH version")
    if os.environ.get("GITHUB_REF") != f"refs/tags/{args.tag}":
        raise SystemExit("release: publication requires the matching pushed tag ref")
    if not os.environ.get("GH_TOKEN"):
        raise SystemExit("release: GH_TOKEN is required for publication")
    head = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True, capture_output=True, check=True
    ).stdout.strip()
    event_sha = os.environ.get("GITHUB_SHA")
    if not event_sha:
        raise SystemExit("release: GITHUB_SHA is required for publication")
    event_commit = subprocess.run(
        ["git", "rev-parse", f"{event_sha}^{{commit}}"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    if head != event_commit:
        raise SystemExit("release: checked-out commit does not match the workflow commit")
    require_qualified_state((ROOT / "docs/project-state.md").read_text(encoding="utf-8"))
    assets = stage_assets(args.artifacts.resolve())
    subprocess.run(release_command(args.tag, assets), cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
