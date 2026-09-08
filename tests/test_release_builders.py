from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import build_desktop, build_wasm
from tools.release_common import ensure_disk_free


class ReleaseBuilderTests(unittest.TestCase):
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
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Disk.1").write_bytes(b"player input")
            with self.assertRaisesRegex(SystemExit, "Disk.1"):
                ensure_disk_free(root, "release")


if __name__ == "__main__":
    unittest.main()
