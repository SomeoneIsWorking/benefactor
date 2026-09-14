from __future__ import annotations

import sys
import unittest
import zipfile
from html.parser import HTMLParser
from pathlib import Path
from unittest import mock

from tools import build_desktop, build_wasm, check_windows_imports
from tools.paths import SCRATCH
from tools.release_common import ensure_disk_free


class DiskInputParser(HTMLParser):
    """Collects every <input> the shipped page declares, if any."""

    def __init__(self) -> None:
        super().__init__()
        self.inputs: list[dict[str, str | None]] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "input":
            self.inputs.append(dict(attrs))


class ReleaseBuilderTests(unittest.TestCase):
    def test_windows_import_check_rejects_unbundled_mingw_runtime(self) -> None:
        imports = "DLL Name: KERNEL32.dll\nDLL Name: libstdc++-6.dll\n"
        self.assertEqual(
            check_windows_imports.unbundled_imports(imports, set()), ["libstdc++-6.dll"]
        )

    def test_windows_import_check_accepts_bundled_or_system_dlls(self) -> None:
        imports = "DLL Name: KERNEL32.dll\nDLL Name: SDL3.dll\n"
        self.assertEqual(check_windows_imports.unbundled_imports(imports, {"SDL3.dll"}), [])

    def test_windows_import_check_refuses_empty_inspection(self) -> None:
        with self.assertRaisesRegex(ValueError, "did not report any"):
            check_windows_imports.unbundled_imports("not a PE import table", set())

    def test_web_picker_has_no_filter_for_numeric_disk_suffixes(self) -> None:
        root = Path(__file__).parents[1] / "platforms" / "web"
        page = (root / "index.html").read_text()
        # The page declares no file input at all: the chooser belongs to the
        # picker bridge, which opens it when the product asks for disks.
        parser = DiskInputParser()
        parser.feed(page)
        self.assertEqual(parser.inputs, [])
        self.assertIn("image-rendering: pixelated", page)
        self.assertIn('<canvas id="canvas"', page)

        # Numeric-suffix Disk.1..3 files must not be MIME-filtered, and the set
        # has to arrive in one pick, so the input stays unrestricted and multiple.
        picker = (root / "disk_setup.js").read_text()
        self.assertIn('input.accept = ""', picker)
        self.assertIn("input.multiple = true", picker)
        self.assertIn('input.type = "file"', picker)

    def test_web_package_bootstraps_isolation_before_emscripten(self) -> None:
        root = Path(__file__).parents[1] / "platforms" / "web"
        page = (root / "index.html").read_text()
        isolation = (root / "isolation.mjs").read_text()
        worker = (root / "service-worker.js").read_text()
        self.assertIn('import { prepareApplication } from "./isolation.mjs"', page)
        # The page loads the two bridges to what only a browser can do — the file
        # chooser and the update check's request — before the product module.
        self.assertIn('for (const bridge of ["disk_setup.js", "release_check.js"])', page)
        self.assertIn('await load("benefactor.js")', page)
        self.assertIn("Cross-Origin-Opener-Policy", worker)
        self.assertIn("Cross-Origin-Embedder-Policy", worker)
        self.assertIn("crossOriginIsolated", isolation)

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

    def test_macos_archive_preserves_app_path_and_executable_mode(self) -> None:
        build = SCRATCH / "verification" / "macos-archive" / "build"
        executable = build / "install" / "Benefactor.app" / "Contents" / "MacOS" / "Benefactor"
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.write_bytes(b"synthetic executable")
        executable.chmod(0o755)
        archive_path = build.parent / "Benefactor-macos-arm64.zip"
        build_desktop.package_macos(build, archive_path)
        with zipfile.ZipFile(archive_path) as archive:
            member = archive.getinfo("Benefactor.app/Contents/MacOS/Benefactor")
            self.assertEqual(archive.read(member), b"synthetic executable")
            self.assertEqual((member.external_attr >> 16) & 0o111, 0o111)
        archive_path.unlink()
        executable.unlink()

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
