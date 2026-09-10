"""Run the interpreter and the reference product IN STEP, and stop the moment they differ.

`tools/oracle_diff.py` runs both products to the end and compares the tables
that come out. That says a screen was 127 frames instead of 191 — a symptom
hundreds of frames downstream of whatever caused it. `tools/state_diff.py`
narrows that to addresses, but only at a moment you have to guess in advance.

This does neither. It drives the two products **frame by frame**: each one
stops at the end of every presented frame and reports what its guest memory
hashes to; neither is allowed to start the next frame until both have reported
and the two reports have been compared. The first frame on which they disagree
is where the divergence was BORN, and the run stops there with both products
still parked on that frame — so the whole guest address space can be dumped
from each and diffed to the byte.

    uv run --frozen python -m tools.lockstep --play 7300:8,7420:8,7560:8

The protocol is four lines of text over a pipe pair, so a product speaks it in
about a hundred lines of C (`src/port/lockstep.c` here, the same header injected
into the reference by `tools/oracle_diff.py`):

    product -> driver   lockstep hello version=<n> regions=<n> bytes=<total>
    product -> driver   lockstep frame=<n> <field>=<value> ... mem=<hash>,<hash>,...
    driver  -> product  go | dump <path> | stop
    product -> driver   lockstep dumped bytes=<n>          (after a dump)

Both products hash the same bytes with the same function, so a region hash that
differs means those bytes differ. The hashes are per region only so that a
difference can be pointed at without shipping eight megabytes down a pipe every
frame; the byte-exact answer comes from the dump the driver asks for once, at
the frame that matters.
"""

from __future__ import annotations

import argparse
import contextlib
import logging
import os
import selectors
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from tools.paths import ROOT
from tools.state_diff import Run, runs_of_difference

LOGGER = logging.getLogger("lockstep")

PROTOCOL_VERSION = 2

#: Fields the two products report but that are not compared, each with why —
#: the same rule as STRUCTURAL_DIFFERENCES below, for state that is not memory.
STRUCTURAL_FIELDS = (
    (
        "aaud",
        "the AUD0..3 DMA enables. They are written by $005892, the level-6 link that "
        "acknowledges CIA-B and re-arms timer A — the one handler a static recompiler "
        "cannot run at all, so the reference calls $0055A0 and $0058C2 by hand and "
        "never gets there (docs/issues/0008). Measured: this product enables AUD0 on "
        "frame 8 of a cold boot; the reference leaves every audio channel's DMA off "
        "until frame 7167. The rest of DMACON is still compared.",
    ),
    (
        "alc",
        "the AUD0..3 sample pointers. $0058C2 copies each channel's LOOP pointer over "
        "the note's START pointer, and it is the leaf of the level-6 chain: this "
        "product reaches it on the game's own sub-frame timer tick (three a frame in "
        "the intro), the reference calls $0055A0 and $0058C2 back to back once a frame "
        "(src/port/game_loop.c there), so in the reference the copy always follows the "
        "note in the same breath. Measured on frame 37 of a cold boot: channel 3 holds "
        "its loop pointer $06A86E in the reference and the note's start $068F3C here, "
        "and the two agree again on frame 38 — the only difference in 80 frames. What "
        "the pointers are COPIED FROM is compared: the channel structures at $0069F6 "
        "are ordinary guest memory, and periods, volumes and the palette still stand.",
    ),
)

#: Fields that describe the frame rather than the machine, so they are reported
#: alongside a divergence but never counted as one themselves.
CONTEXT_FIELDS = ("cycles",) + tuple(name for name, _ in STRUCTURAL_FIELDS)

#: Guest addresses where the two products differ because of what the ORACLE is,
#: not because of anything this product does wrong — so comparing them reports
#: a divergence on frame 1 and hides every real one behind it. Each is here
#: with the evidence for it; nothing goes in this list on a hunch, and
#: --compare-everything turns them all back on.
STRUCTURAL_DIFFERENCES = (
    (
        "78-7B",
        "the level-6 autovector. The intro's handlers chain by rewriting it — $003160 "
        "moves $78 on to $005892, which moves it to $0058C2, which moves it back — so "
        "in this product it holds a different link on almost every frame. The reference "
        "cannot take an asynchronous interrupt at all: it calls $0055A0 and $0058C2 by "
        "hand and never runs $005892 (docs/issues/0008), so its $78 stays at $003160 "
        "for the whole run. Measured on frame 2 of a cold boot.",
    ),
    (
        "700000-7FFFFF",
        "the port's own scratch, not guest state. pc_preload_all_level_names() "
        "(src/port/level_layout.c) decrunches each world's chunk at $700000 to read the "
        "level names, and this product asks for them at boot while the reference asks at "
        "gameplay entry — so the same bytes are there in both, at different moments. No "
        "guest code addresses this area; it sits past every live region.",
    ),
    (
        "78000-7FFFF",
        "the guest stack. The reference is a static recompiler: a bsr/jsr becomes a C "
        "call, and it pushes a 68000 return address only while g_gameplay_active "
        "(src/engine/rt.c, rt_call_impl). Real hardware pushes one every time, so the "
        "interpreter's stack carries return addresses the reference's does not and the "
        "two are offset by the call depth. Measured on frame 1 of a cold boot: the same "
        "six bytes at $07FFFA in the reference and $07FFF6 here, under an extra "
        "$00003062 return address.",
    ),
)


#: Frames THIS product shows that the reference never shows, so everything
#: after one of them is a frame out of step and every later comparison reports
#: the same single difference again. Dropping the named candidate frame puts
#: the two back in step. Same rule as the lists above: each one is here with
#: the measurement behind it, and never on a hunch.
REALIGNMENTS = (
    (
        7160,
        "the intro -> poster handover. $0033A6 points COP1LC at $008182, then the poster "
        "runs two blits and patches the six bitplane pointers into that list at $0082BC "
        "before $00345A points COP1LC at the finished $0081D2. A blit costs this product "
        "guest time (docs/issues/0007), so the beam boundary falls between the BLTSIZE "
        "write at $003424 and the BBUSY poll at $00342A and the half-built list is shown "
        "for a frame; the reference has no cycle cost for a blit, so its whole pass lands "
        "in one frame and it never shows $008182. Measured with a breakpoint on $00345A: "
        "the level-3 and level-6 handlers run between $003424 and $00342A, which is the "
        "host presenting. Real hardware takes that time, so this product is the faithful "
        "one and the frame is real — it is dropped here, not removed from the product.",
    ),
)


def excluded_spec(extra: str = "", structural: bool = True) -> str:
    """The address ranges neither product hashes, as both are told them.

    One spec, given to both products, so their digests stay comparable: the
    exclusion happens where the bytes are hashed, not afterwards in the driver,
    which is what lets a four-byte vector be left out without losing the 32K
    region around it.
    """
    ranges = [addresses for addresses, _ in STRUCTURAL_DIFFERENCES] if structural else []
    if extra:
        ranges.append(extra)
    return ",".join(ranges)


def parse_ranges(spec: str) -> list[tuple[int, int]]:
    """`78-7B,78000-7FFFF` as [(start, one past the end)] — the same reading the
    products' own parser does (src/port/lockstep_digest.h)."""
    found: list[tuple[int, int]] = []
    for entry in spec.split(","):
        entry = entry.strip().lstrip("$")
        if not entry:
            continue
        low, _, high = entry.partition("-")
        first = int(low, 16)
        last = int(high, 16) if high else first
        found.append((first, last + 1))
    return found


class ProtocolError(RuntimeError):
    """A product said something the protocol does not allow, or stopped saying anything."""


@dataclass(frozen=True)
class Frame:
    """One presented frame, as a product described it."""

    frame: int
    fields: dict[str, str]
    regions: tuple[str, ...]


@dataclass(frozen=True)
class Difference:
    """One thing the two products disagree about on the same frame."""

    what: str
    reference: str
    candidate: str

    def __str__(self) -> str:
        return f"{self.what}: reference {self.reference}, interpreter {self.candidate}"


@dataclass
class Report:
    """What the run found. `divergence` is None when the two never disagreed."""

    frames: int = 0
    frame: int | None = None
    divergence: Difference | None = None
    all_differences: list[Difference] = field(default_factory=list)
    runs: list[Run] = field(default_factory=list)
    ended: str = ""
    context: dict[str, tuple[str, str]] = field(default_factory=dict)
    snapshots: list[Path] = field(default_factory=list)


def parse_frame(line: str) -> Frame:
    """Read one `lockstep frame=...` line. Raises ProtocolError on anything else."""
    words = line.split()
    if not words or words[0] != "lockstep":
        raise ProtocolError(f"not a lockstep line: {line!r}")
    fields: dict[str, str] = {}
    regions: tuple[str, ...] = ()
    for word in words[1:]:
        if "=" not in word:
            continue
        key, _, value = word.partition("=")
        if key == "mem":
            regions = tuple(value.split(",")) if value else ()
        else:
            fields[key] = value
    if "frame" not in fields:
        raise ProtocolError(f"lockstep line carries no frame number: {line!r}")
    number = int(fields.pop("frame"))
    return Frame(number, fields, regions)


def parse_hello(line: str) -> dict[str, str]:
    words = line.split()
    if len(words) < 2 or words[0] != "lockstep" or words[1] != "hello":
        raise ProtocolError(f"expected a lockstep hello, got {line!r}")
    found = {}
    for word in words[2:]:
        key, _, value = word.partition("=")
        found[key] = value
    for required in ("version", "regions", "bytes"):
        if required not in found:
            raise ProtocolError(f"lockstep hello has no {required}: {line!r}")
    if int(found["version"]) != PROTOCOL_VERSION:
        raise ProtocolError(
            f"the product speaks lockstep version {found['version']}, this tool speaks "
            f"{PROTOCOL_VERSION} — rebuild it"
        )
    return found


def differences(
    reference: Frame,
    candidate: Frame,
    region_bytes: int,
) -> list[Difference]:
    """Everything the two frames disagree about, memory regions named by address."""
    found: list[Difference] = []
    for key in sorted(set(reference.fields) | set(candidate.fields)):
        if key in CONTEXT_FIELDS:
            continue
        mine = reference.fields.get(key, "(absent)")
        theirs = candidate.fields.get(key, "(absent)")
        if mine != theirs:
            found.append(Difference(key, mine, theirs))
    if len(reference.regions) != len(candidate.regions):
        found.append(
            Difference(
                "region count",
                str(len(reference.regions)),
                str(len(candidate.regions)),
            )
        )
        return found
    for index, (mine, theirs) in enumerate(zip(reference.regions, candidate.regions, strict=True)):
        if mine == theirs:
            continue
        low = index * region_bytes
        found.append(Difference(f"memory ${low:06X}-${low + region_bytes:06X}", mine, theirs))
    return found


class Product:
    """One launched product, held at a frame boundary until it is told to go on.

    The two pipes are the whole channel: the product's own stdout/stderr go to a
    log, so nothing it logs can be mistaken for a protocol line.
    """

    def __init__(
        self, name: str, command: list[str], log: Path, env: dict[str, str] | None, cwd: Path | None
    ):
        self.name = name
        self.log = log
        to_product_read, to_product_write = os.pipe()
        from_product_read, from_product_write = os.pipe()
        environment = dict(os.environ if env is None else env)
        environment["BENEFACTOR_LOCKSTEP"] = f"{to_product_read}:{from_product_write}"
        log.parent.mkdir(parents=True, exist_ok=True)
        self._sink = log.open("wb")
        self.process = subprocess.Popen(
            command,
            stdout=self._sink,
            stderr=subprocess.STDOUT,
            env=environment,
            cwd=str(cwd) if cwd else None,
            pass_fds=(to_product_read, from_product_write),
            close_fds=True,
        )
        os.close(to_product_read)
        os.close(from_product_write)
        self._incoming = from_product_read
        self._outgoing = to_product_write
        self._buffer = b""
        self._selector = selectors.DefaultSelector()
        self._selector.register(self._incoming, selectors.EVENT_READ)

    def read_line(self, timeout: float) -> str | None:
        """The next protocol line, or None if the product has ended or gone quiet."""
        deadline = time.monotonic() + timeout
        while True:
            if b"\n" in self._buffer:
                line, _, self._buffer = self._buffer.partition(b"\n")
                return line.decode("utf-8", errors="replace").strip()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            if not self._selector.select(min(remaining, 0.5)):
                if self.process.poll() is not None and not self._buffer:
                    return None
                continue
            chunk = os.read(self._incoming, 65536)
            if not chunk:
                return None
            self._buffer += chunk

    def send(self, command: str) -> None:
        try:
            os.write(self._outgoing, (command + "\n").encode("utf-8"))
        except BrokenPipeError as error:
            raise ProtocolError(f"{self.name} closed the channel: {error}") from error

    def dump(self, path: Path, timeout: float) -> Path:
        self.send(f"dump {path}")
        reply = self.read_line(timeout)
        if reply is None or "dumped" not in reply:
            raise ProtocolError(f"{self.name} did not write the memory dump (said {reply!r})")
        return path

    def close(self) -> None:
        with contextlib.suppress(ProtocolError):
            self.send("stop")
        for handle in (self._incoming, self._outgoing):
            with contextlib.suppress(OSError):
                os.close(handle)
        self._selector.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self._sink.close()


def run(
    reference_command: list[str],
    candidate_command: list[str],
    *,
    out: Path,
    frame_timeout: float = 120.0,
    start: int = 0,
    limit: int | None = None,
    ignore_spec: str = "",
    structural: bool = True,
    dump_at: tuple[int, ...] = (),
    realign: tuple[int, ...] = (),
    reference_env: dict[str, str] | None = None,
    candidate_env: dict[str, str] | None = None,
    reference_cwd: Path | None = None,
    candidate_cwd: Path | None = None,
) -> Report:
    """Drive both products in step until they disagree, one ends, or `limit` frames pass."""
    # Absolute, because the products are run from their own build trees: a
    # relative dump path is written wherever the PRODUCT stands, and the driver
    # then finds nothing to diff.
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = Report()
    spec = excluded_spec(ignore_spec, structural)
    reference_env = {**(reference_env or os.environ), "BENEFACTOR_LOCKSTEP_IGNORE": spec}
    candidate_env = {**(candidate_env or os.environ), "BENEFACTOR_LOCKSTEP_IGNORE": spec}
    reference = Product(
        "reference", reference_command, out / "reference.log", reference_env, reference_cwd
    )
    candidate = Product(
        "candidate", candidate_command, out / "interpreter.log", candidate_env, candidate_cwd
    )
    try:
        total_bytes, regions = _agree_on_geometry(reference, candidate, frame_timeout)
        region_bytes = total_bytes // regions
        excluded = parse_ranges(excluded_spec(ignore_spec, structural))
        skew = 0
        while True:
            left = _next_frame(reference, frame_timeout, report)
            if left is None:
                return report
            right = _next_frame(candidate, frame_timeout, report)
            if right is None:
                return report
            # A frame this product shows and the reference never does: let it
            # go by unmatched rather than comparing every frame after it
            # against the wrong one (REALIGNMENTS says which, and why).
            while right.frame in realign:
                LOGGER.info("realigning: this product's frame %d has no partner", right.frame)
                skew += 1
                candidate.send("go")
                right = _next_frame(candidate, frame_timeout, report)
                if right is None:
                    return report
            report.frames = max(report.frames, left.frame + 1)
            if left.frame in dump_at:
                for product, name in ((reference, "reference"), (candidate, "interpreter")):
                    report.snapshots.append(
                        product.dump(out / f"{name}-{left.frame}.bin", frame_timeout)
                    )
            if left.frame + skew != right.frame:
                report.frame = min(left.frame, right.frame)
                report.divergence = Difference("frame number", str(left.frame), str(right.frame))
                report.ended = "the two products are no longer on the same frame"
                return report
            if left.frame >= start:
                found = differences(left, right, region_bytes)
                if found:
                    report.frame = left.frame
                    report.divergence = found[0]
                    report.all_differences = found
                    report.context = {
                        key: (left.fields.get(key, ""), right.fields.get(key, ""))
                        for key in CONTEXT_FIELDS
                        if key in left.fields or key in right.fields
                    }
                    report.ended = f"they first disagreed on frame {left.frame}"
                    report.runs = _locate(reference, candidate, out, frame_timeout, excluded)
                    return report
            if limit is not None and left.frame + 1 >= limit:
                report.ended = f"reached the {limit}-frame limit with no divergence"
                return report
            reference.send("go")
            candidate.send("go")
    finally:
        candidate.close()
        reference.close()


def _agree_on_geometry(reference: Product, candidate: Product, timeout: float) -> tuple[int, int]:
    """Both products must hash the same address space the same way, or nothing means anything."""
    hellos = {}
    for product in (reference, candidate):
        line = product.read_line(timeout)
        if line is None:
            raise ProtocolError(
                f"the {product.name} product never said hello — it is probably not built "
                f"with the lockstep hook, or it failed to start. See {product.log}"
            )
        hellos[product.name] = parse_hello(line)
    left, right = hellos["reference"], hellos["candidate"]
    if left["regions"] != right["regions"]:
        raise ProtocolError(
            f"the products hash a different number of regions ({left['regions']} vs "
            f"{right['regions']}); their digests are not comparable"
        )
    if left["bytes"] != right["bytes"]:
        raise ProtocolError(
            f"the products hash a different amount of memory ({left['bytes']} vs "
            f"{right['bytes']} bytes); their digests are not comparable"
        )
    return int(left["bytes"]), int(left["regions"])


def _next_frame(product: Product, timeout: float, report: Report) -> Frame | None:
    line = product.read_line(timeout)
    if line is None:
        code = product.process.poll()
        report.ended = (
            f"the {product.name} product stopped reporting at frame {report.frames} "
            + (f"(exit code {code})" if code is not None else f"(no frame within {timeout:g}s)")
            + f" — see {product.log}"
        )
        return None
    return parse_frame(line)


def _locate(
    reference: Product,
    candidate: Product,
    out: Path,
    timeout: float,
    excluded: list[tuple[int, int]],
) -> list[Run]:
    """Both products are parked on the divergent frame: dump and diff them to the byte."""
    try:
        left = reference.dump(out / "reference.bin", timeout)
        right = candidate.dump(out / "interpreter.bin", timeout)
    except ProtocolError as error:
        LOGGER.warning("could not locate the divergence to a byte: %s", error)
        return []
    if not left.is_file() or not right.is_file():
        LOGGER.warning(
            "a product said it had dumped its memory but wrote no file (%s, %s); the "
            "divergence is reported by region only",
            left,
            right,
        )
        return []
    found = runs_of_difference(left.read_bytes(), right.read_bytes(), coalesce=16)
    # The digests did not compare these bytes, so the byte report must not show
    # them either: a run the comparison never made is not a finding.
    return [
        run for run in found if not any(run.start < end and low < run.end for low, end in excluded)
    ]


def describe(report: Report) -> str:
    """The whole finding, as the run's last word."""
    lines: list[str] = []
    if report.divergence is None:
        lines.append(f"no divergence in {report.frames} frames — {report.ended}")
        return "\n".join(lines)
    lines.append(f"DIVERGED on frame {report.frame}: {report.divergence}")
    for key, (mine, theirs) in report.context.items():
        lines.append(f"  {key}: reference {mine}, interpreter {theirs}")
    if len(report.all_differences) > 1:
        lines.append(f"  and {len(report.all_differences) - 1} more on the same frame:")
        for difference in report.all_differences[1:9]:
            lines.append(f"    {difference}")
    if report.runs:
        total = sum(run.length for run in report.runs)
        lines.append(
            f"  {total} bytes differ, in {len(report.runs)} runs — the first few, by address:"
        )
        for run in report.runs[:12]:
            lines.append(
                f"    ${run.start:06X}..${run.end - 1:06X}  {run.length:>7} bytes  {run.region()}"
            )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--play",
        default="",
        metavar="FRAME:HELD,...",
        help="drive both products with the same frame-indexed fire timeline, so the "
        "comparison reaches gameplay instead of stopping in the attract loop",
    )
    parser.add_argument(
        "--start",
        type=int,
        default=0,
        help="compare from this frame on (both products still run in step before it). "
        "Use it to step past a divergence already understood and recorded",
    )
    parser.add_argument("--limit", type=int, default=None, help="stop after this many frames")
    parser.add_argument(
        "--dump-at",
        default="",
        metavar="FRAME,...",
        help="snapshot both products' whole guest memory at these frames and carry on. "
        "A divergence is often already complete when it is noticed; earlier snapshots "
        "are how you find the frame it was born on",
    )
    parser.add_argument(
        "--no-realign",
        action="store_true",
        help="do not drop the frames in REALIGNMENTS — frames this product shows "
        "and the reference never does, each recorded with its measurement. Without "
        "the drop, every frame after one of them reports the same difference again",
    )
    parser.add_argument(
        "--ignore",
        default="",
        metavar="ADDR-ADDR,...",
        help="guest addresses whose difference is known and recorded; the regions "
        "holding them are not compared",
    )
    parser.add_argument(
        "--frame-timeout",
        type=float,
        default=120.0,
        help="how long one product may go without reporting a frame before the run "
        "gives up on it (default 120s)",
    )
    parser.add_argument(
        "--compare-everything",
        action="store_true",
        help="also compare the addresses in STRUCTURAL_DIFFERENCES — where the two "
        "products differ because of what the reference IS, not because of a fault "
        "here. Off by default: comparing them reports a divergence on frame 1 and "
        "buries every real one behind it",
    )
    parser.add_argument("--out", type=Path, default=ROOT / "scratch/lockstep")
    options = parser.parse_args(argv)

    from tools.build_product import build_product
    from tools.oracle_diff import _disk_arguments, _snapshot, setup_reference

    try:
        reference_executable = _snapshot(setup_reference())
        candidate_executable = _snapshot(build_product())
        disks = _disk_arguments()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        LOGGER.error("%s", error)
        return 2

    environment = dict(os.environ)
    environment["BENEFACTOR_NO_PACE"] = "1"
    if options.play:
        environment["BENEFACTOR_PRESSES"] = options.play

    if not options.compare_everything:
        for addresses, why in STRUCTURAL_DIFFERENCES:
            LOGGER.info("not comparing $%s — %s", addresses, why)
        for name, why in STRUCTURAL_FIELDS:
            LOGGER.info("not comparing %s — %s", name, why)
    if not options.no_realign:
        for frame, why in REALIGNMENTS:
            LOGGER.info("dropping this product's frame %d — %s", frame, why)
    LOGGER.info("running both products in step; the first disagreement stops them")
    try:
        report = run(
            [str(reference_executable), "--headless", "--disk", *disks],
            [str(candidate_executable), "--headless", "--disk", *disks],
            out=options.out,
            frame_timeout=options.frame_timeout,
            start=options.start,
            limit=options.limit,
            ignore_spec=options.ignore,
            structural=not options.compare_everything,
            dump_at=tuple(int(frame) for frame in options.dump_at.split(",") if frame.strip()),
            realign=() if options.no_realign else tuple(frame for frame, _ in REALIGNMENTS),
            reference_env=environment,
            candidate_env=environment,
            reference_cwd=reference_executable.parent,
            candidate_cwd=candidate_executable.parent,
        )
    except ProtocolError as error:
        LOGGER.error("%s", error)
        return 2
    print(describe(report))
    return 1 if report.divergence else 0


if __name__ == "__main__":
    sys.exit(main())
