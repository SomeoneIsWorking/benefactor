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
    def __init__(self) -> None:
        super().__init__()
        self.attributes: dict[str, str | None] | None = None

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if tag == "input" and attributes.get("id") == "disk-files":
            self.attributes = attributes


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
        page = (Path(__file__).parents[1] / "platforms" / "web" / "index.html").read_text()
        parser = DiskInputParser()
        parser.feed(page)
        self.assertIsNotNone(parser.attributes)
        self.assertNotIn("accept", parser.attributes)
        self.assertIn("multiple", parser.attributes)
        self.assertIn("image-rendering: pixelated", page)

    def test_web_package_bootstraps_isolation_before_emscripten(self) -> None:
        root = Path(__file__).parents[1] / "platforms" / "web"
        page = (root / "index.html").read_text()
        isolation = (root / "isolation.mjs").read_text()
        worker = (root / "service-worker.js").read_text()
        self.assertIn('import { prepareApplication } from "./isolation.mjs"', page)
        self.assertIn('picker.src = "disk_setup.js"', page)
        self.assertIn('runtime.src = "benefactor.js"', page)
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
