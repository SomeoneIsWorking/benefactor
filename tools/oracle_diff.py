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
PHASE_LINE = re.compile(r"phase: frame=(\d+) cop1lc=([0-9A-F]+)")
# What the frame PLAYED and SHOWED — see src/port/frame_signature.h. Frame
# counts alone cannot see a stalled melody or a fade that never ramps.
SIGNATURE_LINE = re.compile(
    r"sig: frame=(\d+) pal=([0-9A-F]+) alc=(\S+) aper=(\S+) avol=(\S+) adma=([0-9A-F]+)"
)

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
    hw_reference_signature();
    s_in_present_frame = 0;""",
    ),
    (
        """int hw_present_frame(void)""",
        """/* The frame signature instrument — see src/port/frame_signature.c in the
 * interpreter product. Both products must emit byte-identical lines. */
static void hw_reference_signature(void) {
    static const unsigned kAudioBase[4] = {0x0A0u, 0x0B0u, 0x0C0u, 0x0D0u};
    uint32_t lc[4], per[4], vol[4];
    for (unsigned c = 0; c < 4; c++) {
        lc[c] = (((uint32_t)s_regs[kAudioBase[c] >> 1] << 16) |
                 s_regs[(kAudioBase[c] + 2) >> 1]) & 0xFFFFFFu;
        per[c] = s_regs[(kAudioBase[c] + 6) >> 1];
        vol[c] = s_regs[(kAudioBase[c] + 8) >> 1];
    }
    uint32_t pal = 2166136261u;
    uint32_t list = (((uint32_t)s_regs[COP1LCH >> 1] << 16) | s_regs[COP1LCL >> 1]) & 0xFFFFFFu;
    if (list && g_mem) {
        for (uint32_t i = 0; i + 1 < 2048u; i += 2) {
            const uint8_t *word = g_mem + list + i * 2u;
            uint16_t control = (uint16_t)((word[0] << 8) | word[1]);
            uint16_t value = (uint16_t)((word[2] << 8) | word[3]);
            if (control == 0xFFFFu) break;
            if (control & 1u) continue;
            uint16_t reg = control & 0x01FEu;
            if (reg < 0x180u || reg > 0x1BEu) continue;
            pal ^= (uint32_t)reg; pal *= 16777619u;
            pal ^= (uint32_t)(value & 0x0FFFu); pal *= 16777619u;
        }
    }
    const uint32_t dma = (uint32_t)(s_dmacon & 0x020Fu);
    static uint32_t last_pal = 0xFFFFFFFFu, last_dma = 0xFFFFFFFFu;
    static uint32_t last_lc[4], last_per[4], last_vol[4];
    int same = (pal == last_pal) && (dma == last_dma);
    for (unsigned c = 0; c < 4 && same; c++)
        same = (lc[c] == last_lc[c]) && (per[c] == last_per[c]) && (vol[c] == last_vol[c]);
    if (same) return;
    last_pal = pal; last_dma = dma;
    for (unsigned c = 0; c < 4; c++) {
        last_lc[c] = lc[c]; last_per[c] = per[c]; last_vol[c] = vol[c];
    }
    fprintf(stderr,
            "sig: frame=%d pal=%08X alc=%06X,%06X,%06X,%06X "
            "aper=%u,%u,%u,%u avol=%u,%u,%u,%u adma=%03X\\n",
            s_frame_num, pal, lc[0], lc[1], lc[2], lc[3], per[0], per[1], per[2], per[3],
            vol[0], vol[1], vol[2], vol[3], dma);
    fflush(stderr);
}

int hw_present_frame(void)""",
    ),
)


@dataclass(frozen=True)
class Signature:
    """One frame's audio + palette state, emitted only when it changed."""

    frame: int
    palette: str
    pointers: tuple[str, ...]
    periods: tuple[str, ...]
    volumes: tuple[str, ...]
    dmacon: str


@dataclass(frozen=True)
class Run:
    """One product's run: the screens it showed and how they played."""

    phases: list[Phase]
    signatures: list[Signature]


@dataclass(frozen=True)
class Phase:
    """One screen: the frames it spanned and the copper lists it alternated.

    `last_frame` is where the NEXT screen began, not where this screen last
    wrote a copper pointer. A screen that holds still writes nothing while it
    is on display, so measuring to its last write reported the boot logo — held
    for 34 frames in the reference — as 3 frames, and called an eleven-fold
    burst a perfect match. Only a screen that flips buffers every frame (the
    crawl) reads the same either way.
    """

    first_frame: int
    last_frame: int
    cop1lc: tuple[str, ...]

    @property
    def frames(self) -> int:
        return max(1, self.last_frame - self.first_frame)

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

    # Re-patch from pristine every time: an edit whose text changed here would
    # otherwise find neither its original (already rewritten) nor its new
    # replacement in a previously patched worktree, and abort.
    source = worktree / "src/engine/hw.c"
    _run(["git", "checkout", "--", "src/engine/hw.c"], cwd=worktree)
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


def collect_run(executable: Path, seconds: float, log: Path) -> Run:
    """Run one product headless for `seconds` and return the screens it showed.

    The binary is run from a snapshot taken now, not from the build tree: a
    rebuild part-way through a measurement otherwise silently replaces the
    program being measured, and the table that comes out looks like a real
    result (measured once: a 6290-frame screen reported as 1391).
    """
    log.parent.mkdir(parents=True, exist_ok=True)
    executable = _snapshot(executable)
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
    text = log.read_text(encoding="utf-8", errors="replace")
    return Run(_phases(text), _signatures(text))


def _signatures(text: str) -> list[Signature]:
    found = []
    for line in text.splitlines():
        match = SIGNATURE_LINE.search(line)
        if match:
            found.append(
                Signature(
                    int(match.group(1)),
                    match.group(2),
                    tuple(match.group(3).split(",")),
                    tuple(match.group(4).split(",")),
                    tuple(match.group(5).split(",")),
                    match.group(6),
                )
            )
    return found


def _snapshot(executable: Path) -> Path:
    """Copy `executable` beside itself so a concurrent rebuild cannot swap it.

    The macOS product is an .app bundle whose loader resolves paths relative to
    the bundle, so the bundle is copied whole and the same binary inside it is
    returned.
    """
    bundle = next((parent for parent in executable.parents if parent.suffix == ".app"), None)
    root = bundle if bundle is not None else executable
    frozen = root.with_name(root.name + ".measuring")
    if frozen.exists():
        shutil.rmtree(frozen) if frozen.is_dir() else frozen.unlink()
    if root.is_dir():
        shutil.copytree(root, frozen, symlinks=True)
        return frozen / executable.relative_to(root)
    shutil.copy2(root, frozen)
    frozen.chmod(0o755)
    return frozen


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
        # This screen ran until the frame the next one started on.
        phases.append(Phase(first, frame, tuple(sorted(current))))
        first, last, current = frame, frame, {address}
    if first is not None and last is not None:
        # The run ended here, so the last write is all this screen can be
        # credited with; a run cut off mid-screen cannot be measured further.
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


def _events(run: Run) -> list[tuple[int, bool, bool, bool]]:
    """Per emitted signature: (frame, music moved, palette moved, volume moved).

    "Moved" is relative to the previous emitted signature, so the three counts
    are the rate at which the tune advances, the screen fades, and the mixer
    changes level — the three things a frame count cannot see.
    """
    events: list[tuple[int, bool, bool, bool]] = []
    previous: Signature | None = None
    for signature in run.signatures:
        if previous is not None:
            events.append(
                (
                    signature.frame,
                    signature.pointers != previous.pointers,
                    signature.palette != previous.palette,
                    signature.volumes != previous.volumes,
                )
            )
        previous = signature
    return events


def _counts(run: Run, phase: Phase | None) -> tuple[int, int, int]:
    if phase is None:
        return (0, 0, 0)
    music = fade = volume = 0
    for frame, moved_music, moved_palette, moved_volume in _events(run):
        if not phase.first_frame <= frame <= phase.last_frame:
            continue
        music += moved_music
        fade += moved_palette
        volume += moved_volume
    return (music, fade, volume)


def report_signatures(reference: Run, candidate: Run) -> int:
    """Per screen, how often the music advanced and the palette changed."""
    if not reference.signatures:
        print("no signature lines in the reference run — the instrument is not wired up")
        return 1
    mine = {phase.name: phase for phase in candidate.phases}

    print()
    print("what each screen PLAYED and SHOWED (events per screen, reference vs interpreter)")
    print(f"{'screen (cop1lc)':<22}{'music':>16}{'fade':>16}{'volume':>16}")
    print("-" * 70)
    mismatches = 0
    for phase in reference.phases:
        want = _counts(reference, phase)
        got = _counts(candidate, mine.get(phase.name))
        cells = ""
        differs = False
        for expected, actual in zip(want, got, strict=True):
            cells += f"{f'{expected} / {actual}':>16}"
            floor, ceiling = expected * 0.8, expected * 1.25
            if not (floor <= actual <= ceiling or expected == actual == 0):
                differs = True
        mismatches += differs
        print(f"{phase.name:<22}{cells}{'  <-- differs' if differs else ''}")
    print()
    print(
        "music = frames where a channel's sample pointer moved (the tune advancing); "
        "fade = frames where the palette changed; volume = frames where a channel's "
        "level changed. Reference / interpreter."
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
    parser.add_argument(
        "--reuse-reference",
        action="store_true",
        help="do not re-run the reference; read the reference.log already in --out. The "
        "reference is a fixed commit, so its timeline only changes when the instrument "
        "does — this halves the turnaround while iterating on the interpreter.",
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

    reference_log = options.out / "reference.log"
    if options.reuse_reference and reference_log.is_file():
        LOGGER.info("reusing the reference run in %s", reference_log)
        text = reference_log.read_text(encoding="utf-8", errors="replace")
        reference = Run(_phases(text), _signatures(text))
    else:
        LOGGER.info("running the reference product for %.0fs", options.seconds)
        reference = collect_run(reference_executable, options.seconds, reference_log)
    LOGGER.info("running the interpreter product for %.0fs", options.seconds)
    candidate = collect_run(candidate_executable, options.seconds, options.out / "interpreter.log")

    if not reference.phases:
        LOGGER.error(
            "the reference run produced no timeline; see %s", options.out / "reference.log"
        )
        return 2
    mismatches = report(reference.phases, candidate.phases)
    mismatches += report_signatures(reference, candidate)
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
