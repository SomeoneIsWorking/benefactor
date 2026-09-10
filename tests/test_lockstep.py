"""Tests for the frame-by-frame lockstep runner (`tools/lockstep.py`).

The runner drives the two products in step — neither is allowed past a frame
until both have shown it — and stops at the FIRST frame where they disagree.
These tests stand in fake products for the real ones: the protocol is the
contract, so a script that speaks it exercises the runner end to end without
disks, a build, or ten minutes of game.
"""

from __future__ import annotations

import contextlib
import shutil
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from tools import lockstep

#: A fake product. Emits `frames` frames; `mutate` may return a replacement for
#: any frame's field/region values so a test can make the two disagree at a
#: chosen frame. Speaks exactly the protocol in tools/lockstep.py.
FAKE_PRODUCT = textwrap.dedent(
    '''
    """A stand-in product that speaks the lockstep protocol."""
    import os
    import sys

    REGIONS = {regions}
    FRAMES = {frames}
    DIVERGE_AT = {diverge_at}
    DIVERGE_REGION = {diverge_region}
    DIVERGE_FIELD = {diverge_field!r}
    STOP_AFTER = {stop_after}
    LAG_FROM = {lag_from}
    MEMORY_BYTE = {memory_byte}
    TOTAL_BYTES = {total_bytes}
    DUMP_WRITES = {dump_writes}

    print("lockstep_ignore=" + os.environ.get("BENEFACTOR_LOCKSTEP_IGNORE", ""), flush=True)
    read_fd, write_fd = (int(piece) for piece in os.environ["BENEFACTOR_LOCKSTEP"].split(":"))
    incoming = os.fdopen(read_fd, "r")
    outgoing = os.fdopen(write_fd, "w")


    def send(line):
        outgoing.write(line + "\\n")
        outgoing.flush()


    def command():
        line = incoming.readline()
        return line.strip()


    send(f"lockstep hello version={version} regions={{REGIONS}} bytes={{TOTAL_BYTES}}")
    frame = 0
    while frame < FRAMES:
        cop1lc = "007BC8"
        # A product that fell one frame behind at LAG_FROM: it still numbers its
        # frames consecutively, but from there on each one carries the content
        # the other product showed a frame earlier.
        content = frame - 1 if LAG_FROM is not None and frame > LAG_FROM else frame
        digests = ["%016X" % (content * 7 + index) for index in range(REGIONS)]
        if frame == DIVERGE_AT:
            if DIVERGE_REGION is not None:
                digests[DIVERGE_REGION] = "DEADBEEFDEADBEEF"
            if DIVERGE_FIELD:
                cop1lc = DIVERGE_FIELD
        send(
            f"lockstep frame={{frame}} cop1lc={{cop1lc}} pal=00000001 "
            f"mem={{','.join(digests)}}"
        )
        while True:
            reply = command()
            if reply.startswith("dump "):
                path = reply.split(" ", 1)[1]
                if DUMP_WRITES:
                    with open(path, "wb") as sink:
                        image = bytearray(TOTAL_BYTES)
                        image[0x100] = MEMORY_BYTE
                        sink.write(image)
                send(f"lockstep dumped bytes={{TOTAL_BYTES}}")
                continue
            break
        if reply != "go":
            break
        frame += 1
        if STOP_AFTER is not None and frame >= STOP_AFTER:
            break
    '''
)


def write_fake(
    directory: Path,
    name: str,
    *,
    regions: int = 4,
    frames: int = 6,
    diverge_at: int | None = None,
    diverge_region: int | None = None,
    diverge_field: str = "",
    stop_after: int | None = None,
    lag_from: int | None = None,
    memory_byte: int = 0,
    total_bytes: int = 4096,
    dump_writes: bool = True,
) -> list[str]:
    path = directory / name
    path.write_text(
        FAKE_PRODUCT.format(
            regions=regions,
            frames=frames,
            diverge_at=diverge_at,
            diverge_region=diverge_region,
            diverge_field=diverge_field,
            stop_after=stop_after,
            lag_from=lag_from,
            memory_byte=memory_byte,
            total_bytes=total_bytes,
            dump_writes=dump_writes,
            version=lockstep.PROTOCOL_VERSION,
        ),
        encoding="utf-8",
    )
    return [sys.executable, str(path)]


class ParseFrameTest(unittest.TestCase):
    def test_reads_the_frame_number_fields_and_region_digests(self) -> None:
        frame = lockstep.parse_frame(
            "lockstep frame=12 cop1lc=007BC8 pal=0000ABCD mem=1111111111111111,2222222222222222"
        )
        self.assertEqual(12, frame.frame)
        self.assertEqual("007BC8", frame.fields["cop1lc"])
        self.assertEqual("0000ABCD", frame.fields["pal"])
        self.assertEqual(("1111111111111111", "2222222222222222"), frame.regions)

    def test_a_line_without_a_frame_number_is_a_protocol_error(self) -> None:
        with self.assertRaises(lockstep.ProtocolError):
            lockstep.parse_frame("lockstep cop1lc=007BC8 mem=00")

    def test_a_line_that_is_not_ours_is_a_protocol_error(self) -> None:
        with self.assertRaises(lockstep.ProtocolError):
            lockstep.parse_frame("app: loading Disk.1")


class DifferencesTest(unittest.TestCase):
    def frame(
        self, *, cop1lc: str = "007BC8", regions: tuple[str, ...] = ("A", "B")
    ) -> lockstep.Frame:
        return lockstep.Frame(4, {"cop1lc": cop1lc, "pal": "1"}, regions)

    def test_identical_frames_do_not_differ(self) -> None:
        self.assertEqual([], lockstep.differences(self.frame(), self.frame(), 0x1000))

    def test_a_field_difference_names_the_field_and_both_values(self) -> None:
        found = lockstep.differences(self.frame(), self.frame(cop1lc="0086CC"), 0x1000)
        self.assertEqual(1, len(found))
        self.assertEqual("cop1lc", found[0].what)
        self.assertEqual("007BC8", found[0].reference)
        self.assertEqual("0086CC", found[0].candidate)

    def test_a_region_difference_names_the_address_range_it_covers(self) -> None:
        found = lockstep.differences(self.frame(), self.frame(regions=("A", "C")), 0x1000)
        self.assertEqual(1, len(found))
        self.assertIn("$001000", found[0].what)
        self.assertIn("$002000", found[0].what)

    def test_a_different_region_count_is_reported_rather_than_compared(self) -> None:
        found = lockstep.differences(self.frame(), self.frame(regions=("A",)), 0x1000)
        self.assertEqual(1, len(found))
        self.assertIn("region count", found[0].what)


class RunTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="lockstep-test-"))
        self.addCleanup(self._cleanup)

    def _cleanup(self) -> None:
        shutil.rmtree(self.directory, ignore_errors=True)

    def test_two_products_that_agree_run_to_the_end_with_no_divergence(self) -> None:
        left = write_fake(self.directory, "left.py", frames=6)
        right = write_fake(self.directory, "right.py", frames=6)
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0)
        self.assertIsNone(report.divergence)
        self.assertEqual(6, report.frames)

    def test_it_stops_at_the_first_frame_that_differs_and_says_which_region(self) -> None:
        left = write_fake(self.directory, "left.py")
        right = write_fake(self.directory, "right.py", diverge_at=3, diverge_region=2)
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0)
        self.assertIsNotNone(report.divergence)
        self.assertEqual(3, report.frame)
        self.assertIn("$", report.divergence.what)

    def test_it_stops_at_a_field_that_differs(self) -> None:
        left = write_fake(self.directory, "left.py")
        right = write_fake(self.directory, "right.py", diverge_at=2, diverge_field="0086CC")
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0)
        self.assertEqual(2, report.frame)
        self.assertEqual("cop1lc", report.divergence.what)

    def test_frames_before_the_start_frame_are_not_compared(self) -> None:
        left = write_fake(self.directory, "left.py")
        right = write_fake(self.directory, "right.py", diverge_at=2, diverge_field="0086CC")
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0, start=4)
        self.assertIsNone(report.divergence)

    def test_a_divergence_dumps_both_memories_and_names_the_differing_address(self) -> None:
        left = write_fake(self.directory, "left.py", memory_byte=0x11)
        right = write_fake(
            self.directory, "right.py", diverge_at=1, diverge_region=0, memory_byte=0x22
        )
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0)
        self.assertEqual(1, report.frame)
        self.assertTrue(report.runs, "the divergence should be located to a byte address")
        self.assertEqual(0x100, report.runs[0].start)

    def test_the_dump_still_lands_when_the_products_run_from_another_directory(self) -> None:
        """The products are run from their own build trees, not from --out.

        A relative dump path is written wherever the PRODUCT happens to be, and
        the driver then reports nothing while the two images sit somewhere else.
        """
        elsewhere = self.directory / "elsewhere"
        elsewhere.mkdir()
        left = write_fake(self.directory, "left.py", memory_byte=0x11)
        right = write_fake(
            self.directory, "right.py", diverge_at=1, diverge_region=0, memory_byte=0x22
        )
        # --out is a relative path in ordinary use, which is where this bites.
        with contextlib.chdir(self.directory):
            report = lockstep.run(
                left,
                right,
                out=Path("out"),
                frame_timeout=20.0,
                reference_cwd=elsewhere,
                candidate_cwd=elsewhere,
            )
        self.assertEqual(1, report.frame)
        self.assertTrue(report.runs, "the dump must land where the driver reads it")

    def test_a_dump_that_never_arrives_still_reports_the_divergence(self) -> None:
        left = write_fake(self.directory, "left.py", dump_writes=False)
        right = write_fake(
            self.directory, "right.py", diverge_at=1, diverge_region=0, dump_writes=False
        )
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0)
        self.assertEqual(1, report.frame)
        self.assertEqual([], report.runs)

    def test_it_can_snapshot_both_products_at_chosen_frames(self) -> None:
        """A divergence that is already complete when it is noticed needs earlier
        snapshots to say where it was born — at frames chosen by the caller, not
        only at the frame the comparison happened to stop on."""
        left = write_fake(self.directory, "left.py", memory_byte=0x11)
        right = write_fake(self.directory, "right.py", memory_byte=0x11)
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0, dump_at=(1, 3))
        self.assertIsNone(report.divergence)
        for frame in (1, 3):
            for name in ("reference", "interpreter"):
                path = self.directory / f"{name}-{frame}.bin"
                self.assertTrue(path.is_file(), f"{path} was not written")
        self.assertEqual(
            sorted(
                (self.directory / f"{n}-{f}.bin").resolve()
                for n in ("reference", "interpreter")
                for f in (1, 3)
            ),
            sorted(report.snapshots),
        )

    def test_a_product_that_exits_early_is_reported_not_waited_on(self) -> None:
        left = write_fake(self.directory, "left.py", frames=6)
        right = write_fake(self.directory, "right.py", frames=6, stop_after=2)
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0)
        self.assertIn("candidate", report.ended)
        self.assertIn("frame 2", report.ended)

    def test_products_that_disagree_about_the_memory_geometry_stop_immediately(self) -> None:
        left = write_fake(self.directory, "left.py", regions=4, total_bytes=4096)
        right = write_fake(self.directory, "right.py", regions=8, total_bytes=4096)
        with self.assertRaises(lockstep.ProtocolError) as caught:
            lockstep.run(left, right, out=self.directory, frame_timeout=20.0)
        self.assertIn("regions", str(caught.exception))

    def test_the_limit_stops_a_run_that_would_otherwise_go_on(self) -> None:
        left = write_fake(self.directory, "left.py", frames=1000)
        right = write_fake(self.directory, "right.py", frames=1000)
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0, limit=5)
        self.assertIsNone(report.divergence)
        self.assertEqual(5, report.frames)


class RealignTest(unittest.TestCase):
    """One product showing a frame the other never shows offsets everything after
    it, so every later frame reports a difference that is really the same one.
    A realignment drops that frame — and only where it is asked for."""

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix="lockstep-realign-"))
        self.addCleanup(lambda: shutil.rmtree(self.directory, ignore_errors=True))

    def test_a_product_that_falls_a_frame_behind_diverges_and_stays_diverged(self) -> None:
        left = write_fake(self.directory, "left.py", frames=8)
        right = write_fake(self.directory, "right.py", frames=8, lag_from=3)
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0)
        self.assertEqual(4, report.frame)

    def test_dropping_the_repeated_frame_puts_them_back_in_step(self) -> None:
        left = write_fake(self.directory, "left.py", frames=8)
        right = write_fake(self.directory, "right.py", frames=9, lag_from=3)
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0, realign=(4,))
        self.assertIsNone(report.divergence)

    def test_a_real_difference_after_a_realignment_is_still_reported(self) -> None:
        left = write_fake(self.directory, "left.py", frames=9)
        right = write_fake(
            self.directory, "right.py", frames=9, lag_from=3, diverge_at=7, diverge_region=1
        )
        report = lockstep.run(left, right, out=self.directory, frame_timeout=20.0, realign=(4,))
        self.assertIsNotNone(report.divergence)
        self.assertEqual(6, report.frame)

    def test_every_recorded_realignment_carries_its_reason(self) -> None:
        for frame, why in lockstep.REALIGNMENTS:
            self.assertIsInstance(frame, int)
            self.assertTrue(why.strip(), "a realignment without a reason is a hidden bug")


class StructuralDifferenceTest(unittest.TestCase):
    """Differences that are the oracle's own shape, not a fault in this product."""

    def test_the_guest_stack_is_excluded_by_name_and_reason(self) -> None:
        addresses = [addresses for addresses, _ in lockstep.STRUCTURAL_DIFFERENCES]
        self.assertIn("78000-7FFFF", addresses)
        for _, why in lockstep.STRUCTURAL_DIFFERENCES:
            self.assertTrue(why.strip(), "an exclusion without a reason is a hidden bug")

    def test_a_structural_field_is_reported_but_never_a_divergence(self) -> None:
        for name, why in lockstep.STRUCTURAL_FIELDS:
            self.assertIn(name, lockstep.CONTEXT_FIELDS)
            self.assertTrue(why.strip(), "an exclusion without a reason is a hidden bug")
        left = lockstep.Frame(1, {"aaud": "0", "adma": "200"}, ("A",))
        right = lockstep.Frame(1, {"aaud": "1", "adma": "200"}, ("A",))
        self.assertEqual([], lockstep.differences(left, right, 0x1000))

    def test_the_sample_pointers_are_reported_but_never_a_divergence(self) -> None:
        """AUDxLC is a sub-frame shadow of memory that IS compared.

        The reference calls $0055A0 and $0058C2 back to back, so a note's start
        pointer is replaced by its loop pointer in the same breath; this product
        runs the real level-6 chain, so the copy lands on the next sub-frame
        tick and can fall after the frame is shown.
        """
        left = lockstep.Frame(1, {"alc": "064446,0,0,06A86E", "avol": "64"}, ("A",))
        right = lockstep.Frame(1, {"alc": "064446,0,0,068F3C", "avol": "64"}, ("A",))
        self.assertEqual([], lockstep.differences(left, right, 0x1000))

    def test_the_volumes_and_periods_are_still_compared(self) -> None:
        left = lockstep.Frame(1, {"alc": "0", "avol": "64", "aper": "320"}, ("A",))
        right = lockstep.Frame(1, {"alc": "0", "avol": "63", "aper": "285"}, ("A",))
        self.assertEqual(
            ["aper", "avol"],
            sorted(d.what for d in lockstep.differences(left, right, 0x1000)),
        )

    def test_the_rest_of_dmacon_is_still_compared(self) -> None:
        left = lockstep.Frame(1, {"aaud": "0", "adma": "200"}, ("A",))
        right = lockstep.Frame(1, {"aaud": "0", "adma": "000"}, ("A",))
        self.assertEqual(["adma"], [d.what for d in lockstep.differences(left, right, 0x1000)])

    def test_they_are_excluded_by_default_and_comparable_on_request(self) -> None:
        self.assertIn("78000-7FFFF", lockstep.excluded_spec())
        self.assertEqual("", lockstep.excluded_spec(structural=False))

    def test_both_products_are_told_the_same_ranges(self) -> None:
        """The exclusion happens where the bytes are hashed, in each product.

        If only one of them were told, its digests would be over different
        bytes and every frame would read as divergent.
        """
        directory = Path(tempfile.mkdtemp(prefix="lockstep-env-"))
        self.addCleanup(shutil.rmtree, directory, True)
        left = write_fake(directory, "left.py", frames=1)
        right = write_fake(directory, "right.py", frames=1)
        lockstep.run(left, right, out=directory, frame_timeout=20.0, ignore_spec="1234-1240")
        for name in ("reference.log", "interpreter.log"):
            said = (directory / name).read_text(encoding="utf-8", errors="replace")
            self.assertIn(lockstep.excluded_spec("1234-1240"), said)
            self.assertIn("1234-1240", said)


class ExcludedRangeTest(unittest.TestCase):
    def test_a_range_is_read_inclusive_at_both_ends(self) -> None:
        # The same reading src/port/lockstep_digest.h's parser does, or the two
        # products hash different bytes and every frame looks divergent.
        self.assertEqual([(0x78, 0x7C)], lockstep.parse_ranges("78-7B"))

    def test_a_single_address_is_one_byte(self) -> None:
        self.assertEqual([(0x3800, 0x3801)], lockstep.parse_ranges("3800"))

    def test_several_ranges_are_accepted(self) -> None:
        self.assertEqual([(0x0, 0x1), (0x4000, 0x5000)], lockstep.parse_ranges("0,4000-4fff"))

    def test_the_spec_the_products_get_carries_the_structural_ones_and_the_extra(self) -> None:
        spec = lockstep.excluded_spec("1234-1240")
        self.assertIn("78-7B", spec)
        self.assertIn("1234-1240", spec)
        self.assertEqual("1234-1240", lockstep.excluded_spec("1234-1240", structural=False))


if __name__ == "__main__":
    unittest.main()
