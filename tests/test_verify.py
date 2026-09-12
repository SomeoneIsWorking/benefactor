"""Tests for the verification runner (`tools/verify.py`).

The runner shells out to tools that are not always installed. A missing one
must say WHICH tool and how to get it — a raw FileNotFoundError traceback out
of subprocess names a path, not a remedy, and reads as a broken repository
rather than a missing package.
"""

from __future__ import annotations

import unittest

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

    def test_every_external_tool_the_run_needs_is_named(self) -> None:
        self.assertEqual(("clang-format", "clang-tidy", "node"), verify.EXTERNAL_TOOLS)
