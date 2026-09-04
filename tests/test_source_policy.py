from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.paths import ROOT
from tools.source_policy import check


class SourcePolicyTests(unittest.TestCase):
    def _fixture(self) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        temporary = tempfile.TemporaryDirectory()
        root = Path(temporary.name)
        (root / "src").mkdir()
        (root / "tools").mkdir()
        (root / "CMakeLists.txt").write_text(
            "add_executable(product src/product.c)\n", encoding="utf-8"
        )
        (root / "src/product.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
        return temporary, root

    def test_accepts_clean_retained_source(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            findings, count = check(root)
            self.assertEqual([], findings)
            self.assertEqual(1, count)

    def test_scans_source_not_selected_by_cmake(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            (root / "CMakeLists.txt").write_text(
                "project(empty LANGUAGES NONE)\n", encoding="utf-8"
            )
            (root / "src/product.c").write_text(
                'void x(void) { printf("bad"); }\n', encoding="utf-8"
            )
            findings, count = check(root)
            self.assertEqual(1, count)
            self.assertIn("direct printf", {finding.message for finding in findings})

    def test_allows_only_named_config_and_logging_owners(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            (root / "src/common").mkdir()
            (root / "src/port").mkdir()
            (root / "src/common/log.c").write_text(
                'void sink(void) { fprintf(stderr, "owned"); }\n', encoding="utf-8"
            )
            (root / "src/port/config.c").write_text(
                'void config(void) { getenv("OWNED"); }\n', encoding="utf-8"
            )
            findings, count = check(root)
            self.assertEqual([], findings)
            self.assertEqual(3, count)

    def test_repository_passes_policy(self) -> None:
        findings, _ = check(ROOT)
        self.assertEqual([], findings)

    def test_rejects_static_and_diagnostic_emulator_product_paths(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            (root / "src/product.c").write_text("void x(void) { gfn_dead(); }\n", encoding="utf-8")
            (root / "CMakeLists.txt").write_text(
                "add_executable(product src/product.c vendor/libretro-uae/cpuemu_0.c)\n",
                encoding="utf-8",
            )
            findings, _ = check(root)
            messages = {finding.message for finding in findings}
            self.assertIn("generated guest symbol", messages)
            self.assertIn("direct diagnostic-emulator product dependency", messages)

    def test_allows_shipping_interpreter_owned_by_amigaport(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            (root / "CMakeLists.txt").write_text(
                "# amigaport provides the maintained shipping interpreter\n",
                encoding="utf-8",
            )
            findings, _ = check(root)
            self.assertEqual([], findings)

    def test_rejects_static_dispatch_state_and_fallback_selectors(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            (root / "src/product.c").write_text(
                "int n = GAME_FN_COUNT; extern int g_fn_bank;\n"
                "void x(void) { g_rt_last_call = 1; "
                'getenv("BENEFACTOR_RECOMP_AUDIO"); pc_dump_banks_from_disk(); }\n',
                encoding="utf-8",
            )
            findings, _ = check(root)
            messages = {finding.message for finding in findings}
            self.assertIn("static corpus function count", messages)
            self.assertIn("static function symbol", messages)
            self.assertIn("static dispatcher call state", messages)
            self.assertIn("static fallback selector", messages)
            self.assertIn("retired bank-dump entry", messages)

    def test_rejects_stderr_getenv_and_shell_tool(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            (root / "src/product.c").write_text(
                'void x(void) { fprintf(stderr, "x"); getenv("X"); }\n', encoding="utf-8"
            )
            (root / "tools/bad.sh").write_text("#!/bin/sh\n", encoding="utf-8")
            (root / "tools/AppRun").write_text("#!/bin/sh\n", encoding="utf-8")
            findings, _ = check(root)
            messages = {finding.message for finding in findings}
            self.assertIn("direct process stream fprintf", messages)
            self.assertIn("getenv outside config owner", messages)
            self.assertIn("non-launcher shell tooling is forbidden", messages)
            self.assertEqual(
                2,
                sum(
                    finding.message == "non-launcher shell tooling is forbidden"
                    for finding in findings
                ),
            )

    def test_rejects_debug_gated_process_log(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            (root / "src/product.c").write_text(
                'void x(void) { if (debug_log) benefactor_log_write(1, "x", "bad"); }\n',
                encoding="utf-8",
            )
            findings, _ = check(root)
            self.assertIn("debug-gated process log", {finding.message for finding in findings})

    def test_rejects_new_source_over_structural_limit(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            (root / "src/product.c").write_text("\n" * 1201, encoding="utf-8")
            findings, _ = check(root)
            self.assertIn(
                "source has 1201 lines; structural limit is 1200",
                {finding.message for finding in findings},
            )

    def test_rejects_tmp_runtime_path_in_code_or_comment(self) -> None:
        temporary, root = self._fixture()
        with temporary:
            (root / "src/product.c").write_text(
                'static const char *path = "/tmp/game.state";\n/* fallback scratch path: /tmp */\n',
                encoding="utf-8",
            )
            findings, _ = check(root)
            self.assertEqual(
                2,
                sum(finding.message == "forbidden /tmp runtime path" for finding in findings),
            )


if __name__ == "__main__":
    unittest.main()
