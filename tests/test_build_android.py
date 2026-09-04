from __future__ import annotations

import sys
import unittest
from unittest import mock

from tools import build_android


class AndroidBuildTests(unittest.TestCase):
    def test_runtime_blocker_precedes_android_dependency_probes(self) -> None:
        with (
            mock.patch.object(sys, "argv", ["build_android.py"]),
            mock.patch.object(
                build_android, "runtime_blocker", return_value="runtime adapter is missing"
            ),
            mock.patch.object(build_android, "android_sdk") as android_sdk,
            mock.patch.object(build_android, "required_jdk") as required_jdk,
            self.assertRaisesRegex(
                SystemExit,
                "Benefactor gameplay product unavailable: runtime adapter is missing",
            ),
        ):
            build_android.main()

        android_sdk.assert_not_called()
        required_jdk.assert_not_called()


if __name__ == "__main__":
    unittest.main()
