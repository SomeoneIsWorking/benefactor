from __future__ import annotations

import unittest

from tools.build_puae_oracle import BUILD, VENDOR, commands


class PuaeOracleBuildTests(unittest.TestCase):
    def test_build_stays_under_project_build_root_and_uses_vendor_sources(self) -> None:
        core, c_log, c_options, cpp_oracle = commands(["clang"], ["clang++"])
        self.assertIn(str(BUILD), core)
        self.assertIn(f"CORE_DIR={VENDOR}", core)
        self.assertIn("CC=clang", core)
        self.assertIn("CXX=clang++", core)
        self.assertEqual(c_log[0], "clang")
        self.assertEqual(cpp_oracle[0], "clang++")
        self.assertIn("src/harness/puae_options.c", c_options)
        self.assertIn(str(BUILD / "log.o"), cpp_oracle)
        self.assertIn(str(BUILD / "puae_options.o"), cpp_oracle)
        self.assertIn(str(BUILD / "puae_oracle"), cpp_oracle)


if __name__ == "__main__":
    unittest.main()
