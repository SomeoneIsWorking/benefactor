"""The app icon is checked as a player meets it, and every platform's copy exists.

Two things are being protected. The first is that one authored mark reaches all
four platforms: each packaging path names a file, and that file is there. The
second is that the mark is still legible after rasterising — a launcher and a file
manager draw it at 16 px, so measuring the SVG text alone would prove nothing.
"""

from __future__ import annotations

import math
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
MIN_MARK_FRACTION = 0.02
"""Share of the icon that must read as the hero's amber at launcher sizes.

Measured on this artwork with `--sheet`: 0.109 at 16 px, 0.103 at 32 px, 0.096 at
48 px. The bar sits far below that because the property being defended is "the
mark is there", not "the mark is exactly this big".
"""


def rasteriser() -> str | None:
    return shutil.which("magick") or shutil.which("convert")


def run_magick(*arguments: str) -> str:
    result = subprocess.run(
        [str(rasteriser()), *arguments], check=True, capture_output=True, text=True
    )
    return result.stdout.strip()


def alpha_at(path: Path, x: int, y: int) -> float:
    return float(run_magick(str(path), "-format", f"%[fx:p{{{x},{y}}}.a]", "info:"))


SOLID = "50%"
"""Alpha above which a pixel is the drawn shape rather than its antialiased edge.

Renderers disagree about how far an edge feathers — and ImageMagick 6's SVG
delegate rasterises at the file's intrinsic size and resizes afterwards, which
spreads the edge over several pixels. Measuring what is *drawn* rather than where
the fuzz ends is what makes this check mean the same thing on both.
"""


def drawn_ink_box(path: Path) -> tuple[int, int, int, int]:
    """The bounding box of the drawn pixels, as x, y, width, height."""
    box = run_magick(
        str(path), "-channel", "A", "-threshold", SOLID, "+channel", "-format", "%@", "info:"
    )
    match = re.match(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", box)
    if match is None:
        raise AssertionError(f"ImageMagick reported no ink box for {path}: {box!r}")
    width, height, x, y = (int(value) for value in match.groups())
    return x, y, width, height


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

    def test_android_declares_an_adaptive_and_a_filled_launcher_icon(self) -> None:
        manifest = ElementTree.parse(
            ROOT / "platforms/android/app/src/main/AndroidManifest.xml"
        ).getroot()
        application = manifest.find("application")
        self.assertEqual(application.get(f"{ANDROID_CHROME}icon"), "@mipmap/ic_launcher")
        self.assertEqual(application.get(f"{ANDROID_CHROME}roundIcon"), "@mipmap/ic_launcher_round")
        # From API 26 a launcher composites the icon from layers; the releases
        # before that draw whatever mipmap-anydpi holds, so both must exist.
        for qualifier in ("mipmap-anydpi", "mipmap-anydpi-v26"):
            for name in ("ic_launcher", "ic_launcher_round"):
                resource = draw_app_icon.ANDROID_ROOT / qualifier / f"{name}.xml"
                self.assertTrue(resource.is_file(), f"{resource.relative_to(ROOT)} is missing")

    def test_the_adaptive_icon_references_drawables_that_exist(self) -> None:
        adaptive = ElementTree.parse(
            draw_app_icon.ANDROID_ROOT / "mipmap-anydpi-v26/ic_launcher.xml"
        ).getroot()
        layers = {child.tag: child.get(f"{ANDROID_CHROME}drawable") for child in adaptive}
        self.assertEqual(
            layers,
            {
                "background": "@drawable/ic_launcher_background",
                "foreground": "@drawable/ic_launcher_foreground",
                # Without a monochrome layer a themed launcher tints a copy of the
                # foreground, counters and all.
                "monochrome": "@drawable/ic_launcher_monochrome",
            },
        )
        for drawable in layers.values():
            resource = draw_app_icon.ANDROID_ROOT / "drawable" / f"{drawable.split('/', 1)[1]}.xml"
            self.assertTrue(resource.is_file(), f"{resource.relative_to(ROOT)} is missing")

    def test_the_desktop_windows_macos_and_web_paths_name_the_generated_icon(self) -> None:
        for relative in (
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
    """Both halves of "authored once": it rasterises, and it rasterises at size."""

    def setUp(self) -> None:
        if not rasteriser():
            self.skipTest(
                "ImageMagick's `magick`/`convert` is not on PATH, so the icon cannot be "
                "rasterised at launcher sizes here"
            )
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.temporary = Path(directory.name)

    def preview(self, relative: str, size: int) -> Path:
        """Rasterise a committed Android layer, so the shipped numbers are measured."""
        svg = draw_app_icon.vector_preview_svg(
            [draw_app_icon.ANDROID_ROOT / relative], self.temporary / "preview.svg"
        )
        return draw_app_icon.rasterise(
            svg, size, self.temporary / f"preview-{size}.png", viewport=draw_app_icon.ANDROID_CANVAS
        )

    def test_the_gold_mark_is_present_at_every_launcher_size(self) -> None:
        for size in LAUNCHER_SIZES:
            frame = draw_app_icon.rasterise(
                draw_app_icon.ICON_SVG, size, self.temporary / f"icon-{size}.png"
            )
            fraction = draw_app_icon.mark_fraction(frame)
            with self.subTest(size=size):
                self.assertGreaterEqual(
                    fraction,
                    MIN_MARK_FRACTION,
                    f"only {fraction:.3f} of the {size} px icon reads as the mark",
                )

    def test_the_measurement_would_fail_without_the_mark(self) -> None:
        """The negative control: the identical measurement on a bare tile."""
        bare = self.temporary / "bare.svg"
        bare.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512">'
            f'<rect width="512" height="512" fill="{draw_app_icon.CAVE}"/></svg>',
            encoding="utf-8",
        )
        frame = draw_app_icon.rasterise(bare, 48, self.temporary / "bare.png")
        self.assertLess(
            draw_app_icon.mark_fraction(frame),
            MIN_MARK_FRACTION,
            "the mark measurement passes on a tile with no mark on it",
        )

    def worst_radius(self, frame: Path) -> float:
        """How far the drawn mark reaches from the canvas centre, in canvas units."""
        size = frame.read_bytes() and int(draw_app_icon.ANDROID_CANVAS * 4)
        x, y, width, height = drawn_ink_box(frame)
        scale = size / draw_app_icon.ANDROID_CANVAS
        centre = draw_app_icon.ANDROID_CANVAS / 2
        corners = ((x, y), (x + width, y), (x, y + height), (x + width, y + height))
        return max(
            math.hypot(corner_x / scale - centre, corner_y / scale - centre)
            for corner_x, corner_y in corners
        )

    def test_the_mark_fits_inside_the_circle_a_launcher_may_crop_to(self) -> None:
        frame = self.preview(
            "drawable/ic_launcher_foreground.xml", int(draw_app_icon.ANDROID_CANVAS * 4)
        )
        # A drawn edge still lands somewhere inside its boundary pixel.
        allowed = draw_app_icon.ANDROID_SAFE_DIAMETER / 2 + 1 / 4
        reached = self.worst_radius(frame)
        self.assertLessEqual(
            reached, allowed, f"the mark reaches {reached:.2f} of the {allowed:.2f} it may"
        )

    def test_the_circle_measurement_would_fail_for_an_oversized_mark(self) -> None:
        """The negative control: the same measurement on a mark that must not fit."""
        size = int(draw_app_icon.ANDROID_CANVAS * 4)
        scale = draw_app_icon.mark_scale(
            draw_app_icon.fit_height(
                draw_app_icon.ANDROID_SAFE_DIAMETER - draw_app_icon.ANDROID_FIT_MARGIN
            )
        )
        grown = scale * 1.3
        dx, dy = draw_app_icon.centre_mark(grown, draw_app_icon.ANDROID_CANVAS)
        svg = self.temporary / "oversized.svg"
        body = draw_app_icon.hero_path(grown, dx, dy)
        svg.write_text(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{draw_app_icon.ANDROID_CANVAS}"'
            f' height="{draw_app_icon.ANDROID_CANVAS}" viewBox="0 0 {draw_app_icon.ANDROID_CANVAS}'
            f' {draw_app_icon.ANDROID_CANVAS}"><path fill="{draw_app_icon.GOLD}"'
            f' d="{body}"/></svg>',
            encoding="utf-8",
        )
        frame = draw_app_icon.rasterise(
            svg, size, self.temporary / "oversized.png", viewport=draw_app_icon.ANDROID_CANVAS
        )
        allowed = draw_app_icon.ANDROID_SAFE_DIAMETER / 2 + 1 / 4
        self.assertGreater(
            self.worst_radius(frame),
            allowed,
            "the circle measurement passes for a mark drawn beyond the mask",
        )

    def test_the_monochrome_layer_keeps_the_gap_between_the_legs_open(self) -> None:
        """A themed icon must show the launcher's surface between the hero's legs."""
        size = 432
        frame = self.preview("drawable/ic_launcher_monochrome.xml", size)
        scale = draw_app_icon.mark_scale(
            draw_app_icon.fit_height(
                draw_app_icon.ANDROID_SAFE_DIAMETER - draw_app_icon.ANDROID_FIT_MARGIN
            )
        )
        dx, dy = draw_app_icon.centre_mark(scale, draw_app_icon.ANDROID_CANVAS)
        sample = size / draw_app_icon.ANDROID_CANVAS

        def at(x: float, y: float) -> tuple[int, int]:
            return (
                round(float(draw_app_icon.scaled(x, scale, dx)) * sample),
                round(float(draw_app_icon.scaled(y, scale, dy)) * sample),
            )

        knee = (draw_app_icon.HERO_LEG_TOP + draw_app_icon.HERO_LEG_BOTTOM) / 2
        leg = (
            draw_app_icon.HERO_HEAD_X
            + draw_app_icon.HERO_LEG_GAP_HALF
            + draw_app_icon.HERO_LEG_HALF
        )
        self.assertEqual(
            alpha_at(frame, *at(draw_app_icon.HERO_HEAD_X, knee)),
            0.0,
            "the gap between the legs is filled, not cut out",
        )
        self.assertEqual(
            alpha_at(frame, *at(leg, knee)), 1.0, "a leg is missing where it should be"
        )


if __name__ == "__main__":
    unittest.main()
