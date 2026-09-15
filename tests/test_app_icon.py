"""The app icon is the authored picture, and every platform's copy is that picture.

Two things are being protected. The first is that one authored image reaches all
four platforms: each packaging path names a file, and that file is there and
carries the artwork rather than something a generator invented. The second is
that the picture survives being scaled down — a launcher and a file manager draw
it at 16 px, so checking the SVG text alone would prove nothing.
"""

from __future__ import annotations

import base64
import plistlib
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from xml.etree import ElementTree

from tools import build_desktop, draw_app_icon
from tools.paths import ROOT

ANDROID_CHROME = "{http://schemas.android.com/apk/res/android}"
LAUNCHER_SIZES = (16, 32, 48)

MIN_DETAIL = 0.03
"""How much the icon must still vary across itself once it has been scaled down.

A photograph reduced to a launcher size still changes from pixel to pixel; a
picture that has been lost — replaced by a flat tile, or rendered as nothing —
does not. Measured on this artwork with `--sheet`: 0.111 at 16 px, rising to
0.150 at 128 px. The bar sits far below that, because the property being
defended is "the picture is there", not "the picture is exactly this busy".
"""


def rasteriser() -> str | None:
    return shutil.which("magick") or shutil.which("convert")


class AppIconFormsTest(unittest.TestCase):
    """Every platform names an icon, and each named file is the authored one."""

    def test_the_generator_reports_the_committed_forms_as_current(self) -> None:
        result = subprocess.run(
            ["python3", "-m", "tools.draw_app_icon", "--check"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("authored form(s) current", result.stdout)

    def test_the_svg_carries_the_authored_image_itself(self) -> None:
        """ "Directly" means the bytes: the SVG holds the PNG, not a copy of it."""
        for relative in ("platforms/icons/benefactor.svg", "platforms/web/icon.svg"):
            svg = (ROOT / relative).read_text(encoding="utf-8")
            payload = re.search(r"base64,([A-Za-z0-9+/=]+)", svg)
            self.assertIsNotNone(payload, f"{relative} embeds no image")
            self.assertEqual(
                base64.b64decode(payload.group(1)),
                draw_app_icon.MARK.read_bytes(),
                f"{relative} carries something other than the authored mark",
            )

    def test_android_ships_a_launcher_bitmap_for_every_density(self) -> None:
        manifest = ElementTree.parse(
            ROOT / "platforms/android/app/src/main/AndroidManifest.xml"
        ).getroot()
        application = manifest.find("application")
        self.assertEqual(application.get(f"{ANDROID_CHROME}icon"), "@mipmap/ic_launcher")
        self.assertEqual(application.get(f"{ANDROID_CHROME}roundIcon"), "@mipmap/ic_launcher_round")
        for density in draw_app_icon.ANDROID_DENSITIES:
            for name in ("ic_launcher", "ic_launcher_round", "ic_launcher_background"):
                resource = draw_app_icon.ANDROID_ROOT / f"mipmap-{density}/{name}.png"
                self.assertTrue(resource.is_file(), f"{resource.relative_to(ROOT)} is missing")

    def test_no_anydpi_resource_shadows_the_density_bitmaps(self) -> None:
        """`anydpi` outranks every density qualifier, so a leftover one wins.

        The icon used to be a VectorDrawable in `mipmap-anydpi`, which is exactly
        the folder a pre-26 launcher prefers over the bitmaps this now ships. A
        file left there would hide the picture on those releases and nowhere
        else, which is the kind of thing nobody notices for a year.
        """
        stale = list((draw_app_icon.ANDROID_ROOT / "mipmap-anydpi").glob("*"))
        self.assertEqual(stale, [], f"mipmap-anydpi still holds {[p.name for p in stale]}")

    def test_the_adaptive_icon_references_layers_that_exist(self) -> None:
        for name in ("ic_launcher", "ic_launcher_round"):
            adaptive = ElementTree.parse(
                draw_app_icon.ANDROID_ROOT / f"mipmap-anydpi-v26/{name}.xml"
            ).getroot()
            layers = {child.tag: child.get(f"{ANDROID_CHROME}drawable") for child in adaptive}
            self.assertEqual(
                layers,
                {
                    "background": "@mipmap/ic_launcher_background",
                    # The picture is the background layer, because that is the
                    # layer a launcher's mask crops. There is no monochrome
                    # layer: a themed launcher only themes an icon that offers
                    # one, and a photograph has no honest one-colour form.
                    "foreground": "@drawable/ic_launcher_foreground",
                },
            )
        foreground = draw_app_icon.ANDROID_ROOT / "drawable/ic_launcher_foreground.xml"
        self.assertTrue(foreground.is_file(), f"{foreground.relative_to(ROOT)} is missing")

    def test_every_committed_bitmap_is_square_and_the_size_it_claims(self) -> None:
        for path, bitmap in draw_app_icon.bitmap_outputs().items():
            with self.subTest(path=str(path.relative_to(ROOT))):
                self.assertEqual(draw_app_icon.png_size(path), (bitmap.size, bitmap.size))

    def test_the_desktop_windows_macos_and_web_paths_name_the_generated_icon(self) -> None:
        for relative in (
            "platforms/icons/benefactor-mark.png",
            "platforms/icons/benefactor.svg",
            "platforms/windows/benefactor.ico",
            "platforms/windows/benefactor.rc",
            "platforms/macos/Benefactor.icns",
            "platforms/web/icon.svg",
        ):
            self.assertTrue((ROOT / relative).is_file(), f"{relative} is missing")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("platforms/windows/benefactor.rc", cmake)
        self.assertIn("platforms/macos/Benefactor.icns", cmake)
        self.assertIn("MACOSX_BUNDLE_ICON_FILE", cmake)
        self.assertIn("install(FILES platforms/icons/benefactor.svg", cmake)
        # appimagetool refuses an AppDir unless an icon is named after the desktop
        # entry's Icon key, so the key and the staged file name have to agree.
        desktop = (
            ROOT / "platforms/freedesktop/io.github.SomeoneIsWorking.benefactor.desktop"
        ).read_text(encoding="utf-8")
        key = re.search(r"^Icon=(.+)$", desktop, re.MULTILINE)
        self.assertIsNotNone(key, "the desktop entry names no icon")
        appimage = (ROOT / "tools/build_appimage.py").read_text(encoding="utf-8")
        self.assertIn(f'DESKTOP_NAME = "{key.group(1).strip()}"', appimage)
        self.assertIn("platforms/icons/benefactor.svg", appimage)
        page = (ROOT / "platforms/web/index.html").read_text(encoding="utf-8")
        self.assertIn('rel="icon"', page)
        self.assertIn('"icon.svg"', (ROOT / "tools/build_wasm.py").read_text(encoding="utf-8"))

    def test_the_binary_containers_hold_the_sizes_they_claim(self) -> None:
        self.assertEqual(
            [
                (frame.width, frame.height)
                for frame in draw_app_icon.ico_frames(draw_app_icon.WINDOWS_ICO)
            ],
            [(size, size) for size in draw_app_icon.ICO_SIZES],
        )
        self.assertEqual(
            draw_app_icon.icns_frames(draw_app_icon.MACOS_ICNS),
            [(kind.decode("latin-1"), size) for kind, size in draw_app_icon.ICNS_TYPES.items()],
        )

    def test_a_package_without_the_icon_is_refused(self) -> None:
        """The packaging gates, run against artifacts that must not pass them."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        temporary = Path(directory.name)
        bundle = temporary / "Benefactor.app/Contents"
        (bundle / "Resources").mkdir(parents=True)
        plist = bundle / "Info.plist"
        plist.write_bytes(plistlib.dumps({"CFBundleIconFile": "Benefactor.icns"}))
        with self.assertRaises(SystemExit):
            build_desktop.check_macos_icon(bundle.parent)
        shutil.copy2(draw_app_icon.MACOS_ICNS, bundle / "Resources/Benefactor.icns")
        plist.write_bytes(plistlib.dumps({"CFBundleIconFile": "Other.icns"}))
        with self.assertRaises(SystemExit):
            build_desktop.check_macos_icon(bundle.parent)
        plist.write_bytes(plistlib.dumps({"CFBundleIconFile": "Benefactor.icns"}))
        build_desktop.check_macos_icon(bundle.parent)
        executable = temporary / "benefactor-pc.exe"
        executable.write_bytes(b"MZ" + bytes(4096))
        with self.assertRaises(SystemExit):
            build_desktop.check_windows_icon(executable)
        executable.write_bytes(
            b"MZ"
            + draw_app_icon.ico_frame_bytes(
                draw_app_icon.WINDOWS_ICO, build_desktop.WINDOWS_ICON_FRAME
            )
        )
        build_desktop.check_windows_icon(executable)


class AppIconRasterTest(unittest.TestCase):
    """The picture is still a picture after it has been scaled to a launcher."""

    def setUp(self) -> None:
        if not rasteriser():
            self.skipTest(
                "ImageMagick's `magick`/`convert` is not on PATH, so the icon cannot be "
                "rendered at launcher sizes here"
            )
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.temporary = Path(directory.name)

    def test_the_picture_survives_every_launcher_size(self) -> None:
        for size in LAUNCHER_SIZES:
            frame = draw_app_icon.render(
                draw_app_icon.TILE, size, self.temporary / f"icon-{size}.png"
            )
            varies = draw_app_icon.detail(frame)
            with self.subTest(size=size):
                self.assertGreaterEqual(
                    varies,
                    MIN_DETAIL,
                    f"the {size} px icon varies by only {varies:.3f}: the picture is gone",
                )

    def test_the_measurement_would_fail_on_a_flat_tile(self) -> None:
        """The negative control: the identical measurement on a picture-free tile."""
        flat = self.temporary / "flat.png"
        subprocess.run(
            [str(rasteriser()), "-size", "48x48", "xc:#524531", str(flat)],
            check=True,
            capture_output=True,
        )
        self.assertLess(
            draw_app_icon.detail(flat),
            MIN_DETAIL,
            "the measurement passes on a tile with no picture on it",
        )

    def test_a_committed_bitmap_that_went_stale_is_caught(self) -> None:
        """The negative control for `--check`: a different picture must not pass."""
        stale = self.temporary / "stale.png"
        subprocess.run(
            [str(rasteriser()), "-size", "192x192", "xc:#524531", str(stale)],
            check=True,
            capture_output=True,
        )
        fresh = draw_app_icon.render(draw_app_icon.TILE, 192, self.temporary / "fresh.png")
        self.assertGreater(
            draw_app_icon.difference(stale, fresh),
            draw_app_icon.MAX_BITMAP_DIFFERENCE,
            "the staleness check would pass a bitmap that is not the artwork",
        )


if __name__ == "__main__":
    unittest.main()
