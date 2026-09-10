"""Say WHERE the interpreter and the reference product diverge, not just that they do.

`tools/oracle_diff.py` compares timelines: how many frames each screen lasted,
and what it played and showed. That answers "is this screen wrong?". It cannot
answer "wrong where?", and "gameplay is glitchy" is not something a person can
act on.

This runs both products to the same moment in the game and diffs their entire
guest address space, then reports the differing bytes grouped into runs, largest
first. A run of differing bytes at an address is a lead: it can be looked up in
the disassembly, watched with a breakpoint (`/break?at=`), or read as a game
variable.

The moment is anchored to a SCREEN, not a frame number. The two products do not
agree on frame numbers — gameplay begins at frame 7608 in the reference and 7612
in the interpreter — so diffing frame 7700 against frame 7700 compares two
different instants and reports the whole playfield as divergent. Both products
therefore snapshot at "N frames after this copper list first appeared".

    uv run --frozen python -m tools.state_diff --at 0,30,120

Reading the output: the framebuffer and copper lists live in low chip memory and
will differ whenever anything else does, so they are labelled and can be muted
with --skip-graphics. Divergence in the game's own variables is the interesting
kind, because that is state the game computed differently rather than a picture
drawn from it.
"""

from __future__ import annotations

import argparse
import logging
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from tools.oracle_diff import (
    PHASE_LINE,
    _disk_arguments,
    _snapshot,
    interpreter_executable,
    setup_reference,
)
from tools.paths import ROOT

LOGGER = logging.getLogger("state_diff")

#: The screen to anchor on by default: real gameplay. See docs/issues/0008.
GAMEPLAY_SCREEN = "003484"

#: The screens the intro walks through, in order, with what each one is. Anchor
#: on an earlier one to find where a divergence is BORN: a difference that is
#: already complete at a screen's first frame was inherited from before it.
SCREENS = (
    ("000000", "boot / first logo"),
    ("007BC8", "title crawl"),
    ("0077C0", "logo fade"),
    ("007770", "second logo"),
    ("008182", "credits scroller (interpreter only)"),
    ("0081D2", "credits scroller"),
    ("008302", "main menu"),
    ("003914", "level / title banner"),
    ("003484", "gameplay"),
)

#: The fire timeline that walks credits -> menu -> level intro -> playing.
DEFAULT_PLAY = "7300:8,7420:8,7560:8"

#: Regions that are pictures rather than game state. A difference here is
#: expected the moment anything else differs, so it is labelled, not hidden.
REGIONS = (
    (0x000000, 0x000400, "68000 exception vectors"),
    (0x000400, 0x001000, "low chip RAM (system)"),
    (0x001000, 0x080000, "chip RAM: bitplanes, copper lists, sprites, audio samples"),
    (0x080000, 0x200000, "chip RAM (upper)"),
    (0x200000, 0x800000, "fast RAM / game working set"),
)

GRAPHICS_REGIONS = ("chip RAM: bitplanes, copper lists, sprites, audio samples",)


@dataclass(frozen=True)
class Run:
    """A maximal stretch of addresses whose bytes differ between the two images."""

    start: int
    length: int

    @property
    def end(self) -> int:
        return self.start + self.length

    def region(self) -> str:
        for low, high, name in REGIONS:
            if low <= self.start < high:
                return name
        return "outside the mapped regions"


def collect(
    executable: Path, out: Path, offsets: str, play: str, timeout: float, screen: str
) -> None:
    """Run one product until it has taken every snapshot, writing them into `out`."""
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob("*.bin"):
        stale.unlink()
    executable = _snapshot(executable)
    environment = dict(os.environ)
    environment["BENEFACTOR_NO_PACE"] = "1"
    environment["BENEFACTOR_PRESSES"] = play
    environment["BENEFACTOR_STATE_DUMP"] = f"{screen}:{offsets}"
    environment["BENEFACTOR_STATE_DUMP_DIR"] = str(out)
    wanted = {int(piece) for piece in offsets.split(",")}
    log = out / "run.log"
    with log.open("w", encoding="utf-8") as sink:
        process = subprocess.Popen(
            [str(executable), "--headless", "--disk", *_disk_arguments()],
            cwd=executable.parent,
            stdout=sink,
            stderr=subprocess.STDOUT,
            env=environment,
        )
        try:
            _await_dumps(process, out, wanted, timeout, screen)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()


def _await_dumps(
    process: subprocess.Popen[bytes],
    out: Path,
    wanted: set[int],
    timeout: float,
    screen: str,
) -> None:
    """Wait until every snapshot exists, and fail the MOMENT it is clear one never will.

    The naive version waits for the timeout and then reports. That is a
    fifteen-minute silence for a fault the log already showed in the first
    second, and it was measured: a reference binary that had been rebuilt
    without the snapshot patch ran 44,960 frames and only then said "never
    taken".

    So the log is read as it is written. Once the anchor screen has been shown
    and the run has gone `GRACE_FRAMES` past the last offset without the files
    appearing, the product cannot be going to write them — say so, with which
    frame the anchor was on, rather than waiting out the clock.
    """
    import time

    grace_frames = 120
    deadline = time.monotonic() + timeout
    log = out / "run.log"
    anchor: int | None = None
    newest = 0
    offset = 0
    while True:
        have = {_offset_of(path) for path in out.glob("*.bin")}
        if wanted <= have:
            LOGGER.info("%s: all %d snapshots taken", out.name, len(wanted))
            return

        if log.is_file():
            with log.open("r", encoding="utf-8", errors="replace") as source:
                source.seek(offset)
                fresh = source.read()
                offset = source.tell()
            for line in fresh.splitlines():
                found = PHASE_LINE.search(line)
                if not found:
                    continue
                newest = max(newest, int(found.group(1)))
                if anchor is None and found.group(2).upper() == screen.upper():
                    anchor = int(found.group(1))
                    LOGGER.info("%s: $%s first shown at frame %d", out.name, screen, anchor)

        missing = sorted(wanted - have)
        if anchor is not None and newest > anchor + max(wanted) + grace_frames:
            raise RuntimeError(
                f"{out.name}: ${screen} was shown at frame {anchor} and the run has "
                f"reached frame {newest}, but snapshots {missing} were never written. "
                f"The product is not taking them — check that this build has the "
                f"snapshot code (the reference gets it patched in by "
                f"tools/oracle_diff.py REFERENCE_EDITS, and a rebuild without the "
                f"patch silently drops it). See {log}"
            )
        if process.poll() is not None:
            raise RuntimeError(
                f"{out.name}: the product exited (code {process.returncode}) at frame "
                f"{newest} with snapshots {missing} never taken"
                + (
                    f" (${screen} never appeared at all)"
                    if anchor is None
                    else f" (${screen} was at frame {anchor})"
                )
                + f" — see {log}"
            )
        if time.monotonic() > deadline:
            raise RuntimeError(
                f"{out.name}: timed out at frame {newest} with snapshots {missing} "
                f"never taken"
                + (f"; ${screen} never appeared" if anchor is None else "")
                + f" — see {log}"
            )
        time.sleep(0.25)


def _offset_of(path: Path) -> int:
    return int(path.stem)


def runs_of_difference(left: bytes, right: bytes, coalesce: int) -> list[Run]:
    """Group differing bytes into runs, joining runs closer than `coalesce` bytes.

    Without coalescing, one differing structure reads as hundreds of one-byte
    runs — every field that happens to match splits it. Joining across a small
    gap reports the structure instead of its holes.
    """
    if len(left) != len(right):
        raise RuntimeError(f"snapshots differ in size: {len(left)} vs {len(right)}")
    found: list[Run] = []
    start = -1
    last_difference = -1
    for index, (a, b) in enumerate(zip(left, right, strict=True)):
        if a == b:
            continue
        if start < 0:
            start = index
        elif index - last_difference > coalesce:
            found.append(Run(start, last_difference - start + 1))
            start = index
        last_difference = index
    if start >= 0:
        found.append(Run(start, last_difference - start + 1))
    return found


def _what(screen: str) -> str:
    """The human name of a screen, for output that reads without a lookup table."""
    for code, what in SCREENS:
        if code.upper() == screen.upper():
            return what
    return "unknown screen"


def report(
    offset: int, left: bytes, right: bytes, coalesce: int, skip_graphics: bool, screen: str
) -> int:
    """Print one snapshot's divergence. Returns the number of differing bytes shown."""
    found = runs_of_difference(left, right, coalesce)
    total = sum(run.length for run in found)
    print(f"\n=== {offset:+d} frames into ${screen} ({_what(screen)}) ===")
    if not found:
        print("  identical: the two products' entire guest memory matches")
        return 0

    by_region: dict[str, tuple[int, int]] = {}
    for run in found:
        count, size = by_region.get(run.region(), (0, 0))
        by_region[run.region()] = (count + 1, size + run.length)
    print(f"  {total:,} of {len(left):,} bytes differ, in {len(found):,} runs")
    print(f"  {'region':<58} {'runs':>7} {'bytes':>10}")
    for name, (count, size) in sorted(by_region.items(), key=lambda item: -item[1][1]):
        print(f"  {name:<58} {count:>7,} {size:>10,}")

    shown = [run for run in found if not (skip_graphics and run.region() in GRAPHICS_REGIONS)]
    shown.sort(key=lambda run: -run.length)
    if not shown:
        print("  every run is in the graphics regions (muted by --skip-graphics)")
        return total
    print(f"\n  largest runs{' outside the graphics regions' if skip_graphics else ''}:")
    print(f"  {'address':>10} {'bytes':>8}  region")
    for run in shown[:20]:
        print(f"  ${run.start:08X} {run.length:>8,}  {run.region()}")
    return total


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--at",
        default="0,30,120",
        help="frames after the gameplay screen appears to snapshot at (default 0,30,120). "
        "Offset 0 is the moment gameplay starts, so a difference there was already "
        "present before any gameplay ran.",
    )
    parser.add_argument(
        "--screen",
        default=GAMEPLAY_SCREEN,
        help="the cop1lc to anchor the snapshots on (default %(default)s, gameplay). "
        "Anchor on an earlier screen to find where a divergence is born: known screens "
        "are " + ", ".join(f"{code} ({what})" for code, what in SCREENS) + ".",
    )
    parser.add_argument("--play", default=DEFAULT_PLAY, help="the fire timeline for both products")
    parser.add_argument(
        "--out", type=Path, default=ROOT / "scratch/state-diff", help="where snapshots go"
    )
    parser.add_argument(
        "--coalesce",
        type=int,
        default=16,
        help="join differing runs separated by fewer than this many matching bytes "
        "(default 16), so one structure reads as one run instead of many",
    )
    parser.add_argument(
        "--skip-graphics",
        action="store_true",
        help="do not list individual runs in the bitplane/copper/sprite region. They are "
        "pictures drawn FROM the state, so they differ whenever anything does; the "
        "region totals are still reported.",
    )
    parser.add_argument(
        "--timeout", type=float, default=900.0, help="per-product backstop against a hang"
    )
    parser.add_argument(
        "--reuse", action="store_true", help="diff the snapshots already in --out, running nothing"
    )
    options = parser.parse_args(argv)
    # Absolute, always: the products are run from their own build directories,
    # so a relative --out reaches them as a path that does not exist there and
    # the snapshots are written nowhere. (Measured: --out scratch/state-crawl
    # produced "CANNOT WRITE scratch/state-crawl/reference/+0.bin" while the
    # absolute default worked, which read as the reference build being broken.)
    options.out = options.out.resolve()

    reference_out = options.out / "reference"
    candidate_out = options.out / "interpreter"

    if not options.reuse:
        try:
            reference_executable = setup_reference()
            candidate_executable = interpreter_executable()
        except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
            LOGGER.error("%s", error)
            return 2
        if shutil.disk_usage(options.out.parent if options.out.exists() else ROOT).free < (1 << 30):
            LOGGER.warning("less than 1GB free; each snapshot is 8MB per product")
        for executable, out in (
            (reference_executable, reference_out),
            (candidate_executable, candidate_out),
        ):
            LOGGER.info("running %s", executable.name)
            try:
                collect(executable, out, options.at, options.play, options.timeout, options.screen)
            except RuntimeError as error:
                LOGGER.error("%s", error)
                return 2

    offsets = sorted(int(piece) for piece in options.at.split(","))
    diverged = 0
    for offset in offsets:
        left = reference_out / f"{offset:+d}.bin"
        right = candidate_out / f"{offset:+d}.bin"
        if not left.is_file() or not right.is_file():
            LOGGER.error("missing snapshot for %+d (%s / %s)", offset, left, right)
            return 2
        if report(
            offset,
            left.read_bytes(),
            right.read_bytes(),
            options.coalesce,
            options.skip_graphics,
            options.screen,
        ):
            diverged += 1

    print(
        "\nA run of differing bytes is an address to look at: read it with /mem, "
        "stop on it with /break?at=, or find it in the disassembly."
    )
    return 1 if diverged else 0


if __name__ == "__main__":
    sys.exit(main())
