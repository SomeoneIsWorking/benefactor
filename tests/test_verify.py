"""Tests for the verification runner (`tools/verify.py`).

The runner shells out to tools that are not always installed. A missing one
must say WHICH tool and how to get it — a raw FileNotFoundError traceback out
of subprocess names a path, not a remedy, and reads as a broken repository
rather than a missing package.
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import verify


class MissingToolTest(unittest.TestCase):
    def test_a_missing_tool_names_itself(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            verify._require("definitely-not-installed-xyz")
        self.assertIn("definitely-not-installed-xyz", str(raised.exception))

    def test_every_named_tool_carries_the_command_that_installs_it(self) -> None:
        for tool in verify.EXTERNAL_TOOLS:
            self.assertIn("brew", verify._INSTALLED_BY[tool])

    def test_a_tool_that_is_there_is_returned_for_running(self) -> None:
        self.assertEqual("cc", verify._require("cc"))

    def test_a_keg_only_tool_is_found_and_put_on_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "bin"
            prefix.mkdir()
            tool = prefix / "clang-tidy"
            tool.write_text("#!/bin/sh\n", encoding="utf-8")
            tool.chmod(0o755)
            with (
                patch.object(verify.shutil, "which", side_effect=lambda name: None),
                patch.object(verify, "_keg_only_bin", return_value=tool),
                patch.dict("os.environ", {"PATH": "/nowhere"}),
            ):
                self.assertEqual("clang-tidy", verify._require("clang-tidy"))
                self.assertIn(str(prefix), os.environ["PATH"])

    def test_a_missing_keg_only_tool_says_where_it_looked(self) -> None:
        with (
            patch.object(verify, "_keg_only_bin", return_value=None),
            patch.object(verify.shutil, "which", side_effect=lambda name: None),
            self.assertRaises(SystemExit) as raised,
        ):
            verify._require("clang-tidy")
        self.assertIn("Homebrew prefix", str(raised.exception))

    def test_every_external_tool_the_run_needs_is_named(self) -> None:
        self.assertEqual(("clang-format", "clang-tidy", "node"), verify.EXTERNAL_TOOLS)
