from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.launcher import runtime_blocker


class LauncherTests(unittest.TestCase):
    def test_names_missing_amigaport(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "amigaport"
            self.assertIn("shared/amigaport is missing", runtime_blocker(missing) or "")

    def test_present_runtime_has_title_adapter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            present = Path(directory) / "amigaport"
            present.mkdir()
            self.assertIsNone(runtime_blocker(present))

    def test_runtime_override_uses_canonical_shared_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            present = Path(directory) / "amigaport"
            present.mkdir()
            with patch.dict("os.environ", {"BENEFACTOR_AMIGAPORT_DIR": str(present)}):
                self.assertIsNone(runtime_blocker())


if __name__ == "__main__":
    unittest.main()
