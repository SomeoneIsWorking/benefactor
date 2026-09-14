#!/usr/bin/env python3
"""Every shared input the release workflow pins must exist on its remote.

A consumer resolves its shared dependencies from a sibling checkout, so a pin
that names a commit which was never pushed passes every local build and fails
the moment CI authenticates it. That happened: an Android job died with
`upload-pack: not our ref` on an android-port revision that only existed here.

The workflow is the single source of truth for the pins, so this reads them from
it rather than keeping a second list. The check needs the network and is run
deliberately — by the release workflow before publishing, and by a maintainer
before pushing a pin bump.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from tools.paths import ROOT

WORKFLOW = Path(".github/workflows/release.yml")
CHECKOUT = re.compile(
    r"repository:\s*(?P<repository>\S+)\s*\n(?:[^\n]*\n)*?\s*ref:\s*(?P<revision>[0-9a-f]{40})"
)


@dataclass(frozen=True)
class Pin:
    repository: str
    revision: str


def pinned_revisions(workflow: Path) -> tuple[Pin, ...]:
    """Every distinct (repository, revision) pair the workflow checks out."""
    found: dict[tuple[str, str], Pin] = {}
    for match in CHECKOUT.finditer(workflow.read_text(encoding="utf-8")):
        pin = Pin(match.group("repository"), match.group("revision"))
        found[(pin.repository, pin.revision)] = pin
    return tuple(found[key] for key in sorted(found))


def revision_exists(pin: Pin) -> bool:
    """True when GitHub reports the commit, i.e. it is pushed and reachable."""
    result = subprocess.run(
        ["gh", "api", f"repos/{pin.repository}/commits/{pin.revision}", "--jq", ".sha"],
        text=True,
        capture_output=True,
    )
    return result.returncode == 0 and result.stdout.strip() == pin.revision


def missing_revisions(pins: tuple[Pin, ...] = ()) -> tuple[Pin, ...]:
    if not pins:
        pins = pinned_revisions(ROOT / WORKFLOW)
    return tuple(pin for pin in pins if not revision_exists(pin))


def require_pinned_revisions() -> None:
    """Refuse to publish while any pin names a commit the remote cannot serve."""
    pins = pinned_revisions(ROOT / WORKFLOW)
    if not pins:
        raise SystemExit(f"release: {WORKFLOW} pins no revisions; refusing to guess")
    missing = missing_revisions(pins)
    if missing:
        listed = ", ".join(f"{pin.repository}@{pin.revision[:12]}" for pin in missing)
        raise SystemExit(
            f"release: pinned revision(s) not on the remote: {listed}; "
            "push the shared repository, then bump the pin to the pushed commit"
        )


def main() -> int:
    require_pinned_revisions()
    pins = pinned_revisions(ROOT / WORKFLOW)
    print(f"pinned revisions: {len(pins)} pin(s) resolve on their remotes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
