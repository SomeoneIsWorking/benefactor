"""Finding the shared harness, and saying so when it is not there.

The gate that runs out of that checkout is allowed to be absent on a host, but
it is never allowed to be absent quietly, so the thing that decides "absent" is
worth a test of its own.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import harness_tools

#: An empty override is no override, which is how these tests reach the
#: candidate search without depending on the developer's own environment.
NO_OVERRIDE = {"RE_HARNESS_DIR": ""}


def checkout_in(directory: str) -> Path:
    """A directory shaped like the harness: what the resolver looks for."""
    checkout = Path(directory) / "re-harness"
    (checkout / "tools").mkdir(parents=True)
    return checkout


class HarnessToolsTests(unittest.TestCase):
    def test_the_environment_names_the_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            checkout = checkout_in(directory)
            with patch.dict("os.environ", {"RE_HARNESS_DIR": str(checkout)}):
                self.assertEqual(harness_tools.re_harness_dir(), checkout)

    def test_a_named_directory_that_is_not_a_checkout_is_refused(self) -> None:
        """A path with no `tools/` is somebody's mistake, not a harness."""
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.dict("os.environ", {"RE_HARNESS_DIR": directory}),
        ):
            self.assertIsNone(harness_tools.re_harness_dir())

    def test_the_checkout_beside_this_one_is_found(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            checkout = checkout_in(directory)
            with (
                patch.dict("os.environ", NO_OVERRIDE),
                patch.object(harness_tools, "CANDIDATES", (checkout,)),
            ):
                self.assertEqual(harness_tools.re_harness_dir(), checkout)

    def test_no_checkout_reports_nothing_rather_than_a_path(self) -> None:
        with (
            tempfile.TemporaryDirectory() as directory,
            patch.dict("os.environ", NO_OVERRIDE),
            patch.object(harness_tools, "CANDIDATES", (Path(directory) / "absent",)),
        ):
            self.assertIsNone(harness_tools.re_harness_dir())
            self.assertIsNone(harness_tools.harness_tool("cpp_policy.py"))

    def test_a_tool_the_checkout_does_not_hold_is_not_invented(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            checkout = checkout_in(directory)
            (checkout / "tools/cpp_policy.py").write_text("", encoding="utf-8")
            with patch.dict("os.environ", {"RE_HARNESS_DIR": str(checkout)}):
                self.assertEqual(
                    harness_tools.harness_tool("cpp_policy.py"), checkout / "tools/cpp_policy.py"
                )
                self.assertIsNone(harness_tools.harness_tool("not_a_tool.py"))


if __name__ == "__main__":
    unittest.main()
