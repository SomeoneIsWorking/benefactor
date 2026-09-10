"""Differential test: run the interpreter product against the retired reference
product and diff their screen timelines.

The reference is the last commit of the retired offline-translation product
(REFERENCE_COMMIT). It is NOT restored into this tree — `tools/source_policy.py`
forbids that, and the current engine could not link it anyway. It is checked out
into a sibling git worktree, built there, and driven as a black box. Both builds
emit the same `phase: frame=<n> cop1lc=<addr>` line on every screen change, so
two runs reduce to two timelines that can be compared phase by phase.

What this measures: how many FRAMES each screen lasts. A screen that runs at the
wrong speed in the interpreter shows up as a frame count that does not match the
reference — the thing a stopwatch and a description of the symptom cannot pin
down. It deliberately does not compare wall-clock time: the reference is only
authoritative about the game's own frame sequence.

    uv run --frozen python -m tools.oracle_diff --setup     # build the reference
    uv run --frozen python -m tools.oracle_diff             # run both, diff

The reference build needs the player's own Disk.1/2/3 in the repository root,
exactly like the product does; nothing copyrighted is stored by this tool.
"""

from __future__ import annotations

import argparse
import logging
import os
import re
import shutil
import signal
import subprocess
from dataclasses import dataclass
from pathlib import Path

from tools.paths import ROOT

LOGGER = logging.getLogger("benefactor.oracle")

REFERENCE_COMMIT = "028be16"
REFERENCE_WORKTREE = ROOT.parent / "benefactor-oracle"
DISKS = ("Disk.1", "Disk.2", "Disk.3")
PHASE_LINE = re.compile(r"(?:phase: )?frame=(\d+) cop1lc=([0-9A-F]+)")

# The reference build paces only when it has a window, and says nothing about
# which screen it is on. Both are one-line changes to its hw.c, applied to the
# worktree at setup so the two products are driven the same way and report the
# same thing. Written as explicit replacements rather than a patch file: a patch
# fails silently on a near-miss, an assert here does not.
REFERENCE_EDITS = (
    (
        """        hw_pace_frame();   /* PAL 50 Hz scaled by the effective speed */
    }
    s_in_present_frame = 0;""",
        """    }
    hw_pace_frame();   /* pace headless too, so frame counts are comparable */
    {
        static uint32_t s_last_clc = 0xFFFFFFFFu;
        uint32_t clc = (((uint32_t)s_regs[COP1LCH >> 1] << 16) | s_regs[COP1LCL >> 1]) & 0xFFFFFFu;
        if (clc != s_last_clc) {
            s_last_clc = clc;
            fprintf(stderr, "phase: frame=%d cop1lc=%06X\\n", s_frame_num, clc);
            fflush(stderr);
        }
    }
    s_in_present_frame = 0;""",
    ),
)


@dataclass(frozen=True)
class Phase:
    """One screen: the frames it spanned and the copper lists it alternated."""

    first_frame: int
    last_frame: int
    cop1lc: tuple[str, ...]

    @property
    def frames(self) -> int:
        return self.last_frame - self.first_frame + 1

    @property
    def name(self) -> str:
        return "/".join(self.cop1lc)


def _run(arguments: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    subprocess.run(arguments, cwd=cwd, check=True, env=env)


def _disk_arguments() -> list[str]:
    missing = [name for name in DISKS if not (ROOT / name).is_file()]
    if missing:
        raise RuntimeError(
            f"the reference run needs your own disk images in {ROOT}: missing {', '.join(missing)}"
        )
    return [str(ROOT / name) for name in DISKS]


def setup_reference(worktree: Path = REFERENCE_WORKTREE) -> Path:
    """Check out, patch and build the reference product. Returns its executable."""
    if not worktree.is_dir():
        LOGGER.info("checking out %s into %s", REFERENCE_COMMIT, worktree)
        _run(
            ["git", "worktree", "add", "-f", "--detach", str(worktree), REFERENCE_COMMIT],
            cwd=ROOT,
        )

    # The reference links the diagnostic emulator, whose sources live in this
    # repository's submodule; the worktree gets an empty directory for it.
    vendored = worktree / "vendor/libretro-uae"
    if not (vendored / "sources/src/a2065.c").is_file():
        if vendored.is_dir() and not vendored.is_symlink():
            vendored.rmdir()
        vendored.symlink_to(ROOT / "vendor/libretro-uae")

    source = worktree / "src/engine/hw.c"
    text = source.read_text(encoding="utf-8")
    for original, replacement in REFERENCE_EDITS:
        if replacement in text:
            continue
        if original not in text:
            raise RuntimeError(
                f"cannot instrument the reference build: {source} no longer contains "
                "the text this tool rewrites. The reference commit moved, or the "
                "worktree is dirty."
            )
        text = text.replace(original, replacement, 1)
    source.write_text(text, encoding="utf-8")

    scratch = worktree / "scratch/tmp"
    scratch.mkdir(parents=True, exist_ok=True)
    environment = {**os.environ, "TMPDIR": str(scratch)}
    if shutil.which("ninja") is None:
        raise RuntimeError("ninja is required to build the reference product")
    LOGGER.info("configuring the reference build (this regenerates it from your disks)")
    _run(
        ["cmake", "-S", ".", "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        cwd=worktree,
        env=environment,
    )
    LOGGER.info("building the reference product")
    _run(
        ["cmake", "--build", "build", "--target", "benefactor-pc", "--parallel"],
        cwd=worktree,
        env=environment,
    )
    executable = worktree / "build/benefactor-pc"
    if not executable.is_file():
        raise RuntimeError(f"the reference build produced no executable at {executable}")
    return executable


def interpreter_executable() -> Path:
    from tools.build_product import build_product

    return build_product()


def collect_timeline(executable: Path, seconds: float, log: Path) -> list[Phase]:
    """Run one product headless for `seconds` and return the screens it showed."""
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w", encoding="utf-8") as sink:
        process = subprocess.Popen(
            [str(executable), "--headless", "--disk", *_disk_arguments()],
            cwd=executable.parent,
            stdout=sink,
            stderr=subprocess.STDOUT,
        )
        try:
            process.wait(timeout=seconds)
            LOGGER.warning("%s exited on its own after %.0fs", executable.name, seconds)
        except subprocess.TimeoutExpired:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
    return _phases(log.read_text(encoding="utf-8", errors="replace"))


def _phases(text: str) -> list[Phase]:
    """Group the timeline into screens.

    A screen alternates between at most two copper lists (double buffering), so a
    phase runs until a THIRD address shows up. Grouping on every change instead
    would report one 'phase' per frame for the whole intro.
    """
    samples: list[tuple[int, str]] = []
    for line in text.splitlines():
        found = PHASE_LINE.search(line)
        if found:
            samples.append((int(found.group(1)), found.group(2)))

    phases: list[Phase] = []
    current: set[str] = set()
    first = last = None
    for frame, address in samples:
        if first is None:
            first, last, current = frame, frame, {address}
            continue
        if address in current or len(current) < 2:
            current.add(address)
            last = frame
            continue
        phases.append(Phase(first, last, tuple(sorted(current))))
        first, last, current = frame, frame, {address}
    if first is not None and last is not None:
        phases.append(Phase(first, last, tuple(sorted(current))))
    return phases


def report(reference: list[Phase], candidate: list[Phase]) -> int:
    """Print the two timelines side by side. Returns the number of mismatches."""
    by_name: dict[str, Phase] = {}
    for phase in candidate:
        by_name.setdefault(phase.name, phase)

    print(f"{'screen (cop1lc)':<22}{'reference':>12}{'interpreter':>14}{'ratio':>10}")
    print("-" * 58)
    mismatches = 0
    for phase in reference:
        mine = by_name.get(phase.name)
        if mine is None:
            print(f"{phase.name:<22}{phase.frames:>12}{'never shown':>14}{'':>10}")
            mismatches += 1
            continue
        ratio = phase.frames / mine.frames if mine.frames else float("inf")
        flag = "" if 0.8 <= ratio <= 1.25 else "  <-- differs"
        if flag:
            mismatches += 1
        print(f"{phase.name:<22}{phase.frames:>12}{mine.frames:>14}{ratio:>9.2f}x{flag}")

    unexpected = [phase for phase in candidate if phase.name not in {p.name for p in reference}]
    for phase in unexpected:
        print(f"{phase.name:<22}{'not shown':>12}{phase.frames:>14}{'':>10}  <-- extra")
    print()
    print(
        "ratio = reference frames / interpreter frames. Above 1 means the "
        "interpreter runs that screen too fast."
    )
    return mismatches


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--setup", action="store_true", help="check out and build the reference product, then exit"
    )
    parser.add_argument(
        "--seconds", type=float, default=180.0, help="how long to run each product (default 180)"
    )
    parser.add_argument(
        "--out", type=Path, default=ROOT / "scratch/oracle", help="where the two run logs go"
    )
    options = parser.parse_args(argv)

    try:
        reference_executable = setup_reference()
        if options.setup:
            print(f"reference product ready: {reference_executable}")
            return 0
        candidate_executable = interpreter_executable()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        LOGGER.error("%s", error)
        return 2

    LOGGER.info("running the reference product for %.0fs", options.seconds)
    reference = collect_timeline(
        reference_executable, options.seconds, options.out / "reference.log"
    )
    LOGGER.info("running the interpreter product for %.0fs", options.seconds)
    candidate = collect_timeline(
        candidate_executable, options.seconds, options.out / "interpreter.log"
    )

    if not reference:
        LOGGER.error(
            "the reference run produced no timeline; see %s", options.out / "reference.log"
        )
        return 2
    return 1 if report(reference, candidate) else 0


if __name__ == "__main__":
    raise SystemExit(main())
