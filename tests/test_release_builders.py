from __future__ import annotations

import sys
import unittest
from html.parser import HTMLParser
from pathlib import Path
from unittest import mock

from tools import build_desktop, build_wasm
from tools.paths import SCRATCH
from tools.release_common import ensure_disk_free


class DiskInputParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.attributes: dict[str, str | None] | None = None

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if tag == "input" and attributes.get("id") == "disk-files":
            self.attributes = attributes


class ReleaseBuilderTests(unittest.TestCase):
    def test_web_picker_has_no_filter_for_numeric_disk_suffixes(self) -> None:
        page = (Path(__file__).parents[1] / "platforms" / "web" / "index.html").read_text()
        parser = DiskInputParser()
        parser.feed(page)
        self.assertIsNotNone(parser.attributes)
        self.assertNotIn("accept", parser.attributes)
        self.assertIn("multiple", parser.attributes)

    def test_desktop_builder_stops_at_runtime_boundary(self) -> None:
        with (
            mock.patch.object(
                sys, "argv", ["build_desktop.py", "--platform", "macos", "--output", "app"]
            ),
            mock.patch(
                "tools.release_common.runtime_blocker", return_value="runtime adapter is missing"
            ),
            mock.patch.object(build_desktop, "run") as run,
            self.assertRaisesRegex(SystemExit, "runtime adapter is missing"),
        ):
            build_desktop.main()
        run.assert_not_called()

    def test_wasm_builder_stops_at_runtime_boundary(self) -> None:
        with (
            mock.patch.object(sys, "argv", ["build_wasm.py", "--output", "web"]),
            mock.patch(
                "tools.release_common.runtime_blocker", return_value="runtime adapter is missing"
            ),
            mock.patch.object(build_wasm, "run") as run,
            self.assertRaisesRegex(SystemExit, "runtime adapter is missing"),
        ):
            build_wasm.main()
        run.assert_not_called()

    def test_artifact_rejects_player_files(self) -> None:
        root = SCRATCH / "verification" / "release-builder"
        root.mkdir(parents=True, exist_ok=True)
        disk = root / "Disk.1"
        disk.write_bytes(b"player input")
        try:
            with self.assertRaisesRegex(SystemExit, "Disk.1"):
                ensure_disk_free(root, "release")
        finally:
            disk.unlink()


if __name__ == "__main__":
    unittest.main()
