#!/usr/bin/env python3
"""Author Benefactor's app icon once and emit every platform's form of it.

The checked-in SVG in `platforms/icons/` is the artwork; this file is what draws
it, so one change lands everywhere instead of leaving four platforms with four
different marks. It writes:

    platforms/icons/benefactor.svg                        desktop raster source
    platforms/web/icon.svg                                browser tab icon
    platforms/android/.../drawable/ic_launcher_*.xml      adaptive layers + monochrome
    platforms/android/.../mipmap-anydpi/ic_launcher*.xml  filled tile for API 21-25
    platforms/android/.../mipmap-anydpi-v26/ic_launcher*.xml  adaptive icon
    platforms/windows/benefactor.rc                       icon resource statement
    platforms/windows/benefactor.ico                      multi-size Windows icon
    platforms/macos/Benefactor.icns                       macOS bundle icon

Only the vector text is authored here. The binary containers are rasterised from
the SVG with ImageMagick, and `--check` both re-derives the text and reads the
containers back, so a stale or truncated icon cannot pass as current.

    python3 -m tools.draw_app_icon --write
    python3 -m tools.draw_app_icon --check
"""

from __future__ import annotations

import argparse
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from xml.etree import ElementTree

from tools.paths import ROOT

ANDROID_NAMESPACE = "{http://schemas.android.com/apk/res/android}"

# The port's own palette: the pause panel's navy, its gold rule, and the cyan it
# already uses for the touch controls. Flat fills, because an icon is read at 16 px.
NAVY = "#16233a"
GOLD = "#f5cf69"
CYAN = "#6dc4d8"
WHITE = "#ffffff"

CANVAS = 512
CORNER_FRACTION = 0.219

MARK_SCALE = 1.08
"""How much of the tile the mark fills; tuned by looking, not by arithmetic."""

#: The mark's coordinate system: a "B" over the platform it stands on. The letter
#: identifies the app; the bar says platformer and matches the port's accent.
LETTER_LEFT = 161.0
LETTER_STEM_RIGHT = 217.0
LETTER_TOP = 108.0
LETTER_MID_TOP = 224.0
LETTER_MID_BOTTOM = 248.0
LETTER_BOTTOM = 364.0
LETTER_BOWL_FLAT = 261.0
LETTER_JOIN_RIGHT = 253.0
COUNTER_LEFT = 237.0
COUNTER_RADIUS = 32.0
PLATE_TOP = 382.0
PLATE_BOTTOM = 410.0
PLATE_RADIUS = 14.0

# Android's adaptive canvas is 108 units and the launcher may crop the outer 18,
# so the safe middle is 72 and the mark is sized to stay inside it.
ANDROID_CANVAS = 108.0
#: The mark has to survive the tightest mask a launcher may apply, which is a
#: 66-unit circle in the middle of the 108-unit canvas.
ANDROID_SAFE = 62.0
ANDROID_LEGACY_SAFE = 74.0

ICNS_TYPES = {
    b"ic11": 32,  # 16x16@2x
    b"ic12": 64,  # 32x32@2x
    b"ic07": 128,
    b"ic13": 256,  # 128x128@2x
    b"ic08": 256,
    b"ic14": 512,  # 256x256@2x
    b"ic09": 512,
    b"ic10": 1024,  # 512x512@2x
}
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

ANDROID_ROOT = ROOT / "platforms/android/app/src/main/res"
ICON_SVG = ROOT / "platforms/icons/benefactor.svg"
WEB_SVG = ROOT / "platforms/web/icon.svg"
WINDOWS_RC = ROOT / "platforms/windows/benefactor.rc"
WINDOWS_ICO = ROOT / "platforms/windows/benefactor.ico"
MACOS_ICNS = ROOT / "platforms/macos/Benefactor.icns"

MIN_ICON_INK = 0.12
"""Fraction of a rasterised icon that must be non-transparent at 16 px."""


def scaled(value: float, scale: float, offset: float) -> str:
    text = f"{value * scale + offset:.2f}".rstrip("0").rstrip(".")
    return text if text not in ("", "-0") else "0"


def plain(value: float) -> str:
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return text if text not in ("", "-0") else "0"


def _point(x: float, y: float, scale: float, dx: float, dy: float) -> str:
    return f"{scaled(x, scale, dx)} {scaled(y, scale, dy)}"


def _radius(value: float, scale: float) -> str:
    return plain(value * scale)


def letter_path(scale: float = 1.0, dx: float = 0.0, dy: float = 0.0) -> str:
    """The "B" as one path: stem, two bowls, joining bar, and two counters.

    No subpath overlaps another except along shared edges, so
    `fill-rule="evenodd"` cuts the counters out instead of filling them — which
    is what lets this same path serve as Android's monochrome layer, where the
    counters have to be transparent rather than painted.
    """
    bowl_radius = (LETTER_MID_TOP - LETTER_TOP) / 2
    stem_radius = 18.0
    # The stem, rounded on its outer corners only: the inner ones are where the
    # bowls attach, and the contours have to meet there without a seam.
    stem_block = (
        f"M{_point(LETTER_LEFT + stem_radius, LETTER_TOP, scale, dx, dy)} "
        f"L{_point(LETTER_STEM_RIGHT, LETTER_TOP, scale, dx, dy)} "
        f"L{_point(LETTER_STEM_RIGHT, LETTER_BOTTOM, scale, dx, dy)} "
        f"L{_point(LETTER_LEFT + stem_radius, LETTER_BOTTOM, scale, dx, dy)} "
        f"A{_radius(stem_radius, scale)} {_radius(stem_radius, scale)} 0 0 1 "
        f"{_point(LETTER_LEFT, LETTER_BOTTOM - stem_radius, scale, dx, dy)} "
        f"L{_point(LETTER_LEFT, LETTER_TOP + stem_radius, scale, dx, dy)} "
        f"A{_radius(stem_radius, scale)} {_radius(stem_radius, scale)} 0 0 1 "
        f"{_point(LETTER_LEFT + stem_radius, LETTER_TOP, scale, dx, dy)} Z"
    )
    top_bowl = (
        f"M{_point(LETTER_STEM_RIGHT, LETTER_TOP, scale, dx, dy)} "
        f"L{_point(LETTER_BOWL_FLAT, LETTER_TOP, scale, dx, dy)} "
        f"A{_radius(bowl_radius, scale)} {_radius(bowl_radius, scale)} 0 0 1 "
        f"{_point(LETTER_BOWL_FLAT, LETTER_MID_TOP, scale, dx, dy)} "
        f"L{_point(LETTER_STEM_RIGHT, LETTER_MID_TOP, scale, dx, dy)} Z"
    )
    join = (
        f"M{_point(LETTER_STEM_RIGHT, LETTER_MID_TOP, scale, dx, dy)} "
        f"L{_point(LETTER_JOIN_RIGHT, LETTER_MID_TOP, scale, dx, dy)} "
        f"L{_point(LETTER_JOIN_RIGHT, LETTER_MID_BOTTOM, scale, dx, dy)} "
        f"L{_point(LETTER_STEM_RIGHT, LETTER_MID_BOTTOM, scale, dx, dy)} Z"
    )
    bottom_bowl = (
        f"M{_point(LETTER_STEM_RIGHT, LETTER_MID_BOTTOM, scale, dx, dy)} "
        f"L{_point(LETTER_BOWL_FLAT, LETTER_MID_BOTTOM, scale, dx, dy)} "
        f"A{_radius(bowl_radius, scale)} {_radius(bowl_radius, scale)} 0 0 1 "
        f"{_point(LETTER_BOWL_FLAT, LETTER_BOTTOM, scale, dx, dy)} "
        f"L{_point(LETTER_STEM_RIGHT, LETTER_BOTTOM, scale, dx, dy)} Z"
    )
    counters = " ".join(
        f"M{_point(COUNTER_LEFT, centre - COUNTER_RADIUS, scale, dx, dy)} "
        f"A{_radius(COUNTER_RADIUS, scale)} {_radius(COUNTER_RADIUS, scale)} 0 0 1 "
        f"{_point(COUNTER_LEFT, centre + COUNTER_RADIUS, scale, dx, dy)} Z"
        for centre in (
            (LETTER_TOP + LETTER_MID_TOP) / 2,
            (LETTER_MID_BOTTOM + LETTER_BOTTOM) / 2,
        )
    )
    return " ".join([stem_block, top_bowl, join, bottom_bowl, counters])


def plate_path(scale: float = 1.0, dx: float = 0.0, dy: float = 0.0) -> str:
    """The platform bar alone, so it can carry its own colour."""
    right = mark_right()
    radius = _radius(PLATE_RADIUS, scale)
    return (
        f"M{_point(LETTER_LEFT + PLATE_RADIUS, PLATE_TOP, scale, dx, dy)} "
        f"L{_point(right - PLATE_RADIUS, PLATE_TOP, scale, dx, dy)} "
        f"A{radius} {radius} 0 0 1 {_point(right - PLATE_RADIUS, PLATE_BOTTOM, scale, dx, dy)} "
        f"L{_point(LETTER_LEFT + PLATE_RADIUS, PLATE_BOTTOM, scale, dx, dy)} "
        f"A{radius} {radius} 0 0 1 "
        f"{_point(LETTER_LEFT + PLATE_RADIUS, PLATE_TOP, scale, dx, dy)} Z"
    )


def mark_right() -> float:
    return LETTER_BOWL_FLAT + (LETTER_MID_TOP - LETTER_TOP) / 2


def mark_extent() -> tuple[float, float, float, float]:
    return (LETTER_LEFT, LETTER_TOP, mark_right(), PLATE_BOTTOM)


def centre_mark(scale: float, canvas: float) -> tuple[float, float]:
    left, top, right, bottom = mark_extent()
    return (
        (canvas - (right - left) * scale) / 2 - left * scale,
        (canvas - (bottom - top) * scale) / 2 - top * scale,
    )


def master_svg() -> str:
    radius = plain(CORNER_FRACTION * CANVAS)
    dx, dy = centre_mark(MARK_SCALE, CANVAS)
    return "\n".join(
        [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{CANVAS}" height="{CANVAS}"'
            f' viewBox="0 0 {CANVAS} {CANVAS}">',
            f'  <rect width="{CANVAS}" height="{CANVAS}" rx="{radius}" fill="{NAVY}"/>',
            f'  <path fill="{GOLD}" fill-rule="evenodd" d="{letter_path(MARK_SCALE, dx, dy)}"/>',
            f'  <path fill="{CYAN}" d="{plate_path(MARK_SCALE, dx, dy)}"/>',
            "</svg>",
            "",
        ]
    )


def android_vector(body: str) -> str:
    size = plain(ANDROID_CANVAS)
    return "\n".join(
        [
            '<?xml version="1.0" encoding="utf-8"?>',
            '<vector xmlns:android="http://schemas.android.com/apk/res/android"',
            f'    android:width="{size}dp"',
            f'    android:height="{size}dp"',
            f'    android:viewportWidth="{size}"',
            f'    android:viewportHeight="{size}">',
            body,
            "</vector>",
            "",
        ]
    )


def path_element(d: str, color: str, fill_type: str | None = None) -> str:
    fill = f' android:fillType="{fill_type}"' if fill_type else ""
    return f'    <path android:fillColor="{color}"{fill} android:pathData="{d}" />'


def rounded_rect(x: float, y: float, width: float, height: float, radius: float) -> str:
    return (
        f"M{plain(x + radius)},{plain(y)}"
        f"h{plain(width - 2 * radius)}"
        f"a{plain(radius)},{plain(radius)} 0 0 1 {plain(radius)},{plain(radius)}"
        f"v{plain(height - 2 * radius)}"
        f"a{plain(radius)},{plain(radius)} 0 0 1 -{plain(radius)},{plain(radius)}"
        f"h-{plain(width - 2 * radius)}"
        f"a{plain(radius)},{plain(radius)} 0 0 1 -{plain(radius)},-{plain(radius)}"
        f"v-{plain(height - 2 * radius)}"
        f"a{plain(radius)},{plain(radius)} 0 0 1 {plain(radius)},-{plain(radius)}z"
    )


def mark_scale(safe: float) -> float:
    _, top, _, bottom = mark_extent()
    return safe / (bottom - top)


def android_background() -> str:
    side = plain(ANDROID_CANVAS)
    return android_vector(
        path_element(f"M0,0h{side}v{side}h-{side}z", NAVY),
    )


def android_foreground() -> str:
    scale = mark_scale(ANDROID_SAFE)
    dx, dy = centre_mark(scale, ANDROID_CANVAS)
    body = "\n".join(
        [
            path_element(letter_path(scale, dx, dy), GOLD, "evenOdd"),
            path_element(plate_path(scale, dx, dy), CYAN),
        ]
    )
    return android_vector(body)


def android_monochrome() -> str:
    scale = mark_scale(ANDROID_SAFE)
    dx, dy = centre_mark(scale, ANDROID_CANVAS)
    body = "\n".join(
        [
            path_element(letter_path(scale, dx, dy), WHITE, "evenOdd"),
            path_element(plate_path(scale, dx, dy), WHITE),
        ]
    )
    return android_vector(body)


def android_legacy() -> str:
    """API 21-25 has no launcher mask, so the icon brings its own tile."""
    scale = mark_scale(ANDROID_LEGACY_SAFE)
    dx, dy = centre_mark(scale, ANDROID_CANVAS)
    radius = CORNER_FRACTION * ANDROID_CANVAS
    tile = rounded_rect(0, 0, ANDROID_CANVAS, ANDROID_CANVAS, radius)
    body = "\n".join(
        [
            path_element(tile, NAVY),
            path_element(letter_path(scale, dx, dy), GOLD, "evenOdd"),
            path_element(plate_path(scale, dx, dy), CYAN),
        ]
    )
    return android_vector(body)


def android_adaptive() -> str:
    return "\n".join(
        [
            '<?xml version="1.0" encoding="utf-8"?>',
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">',
            '    <background android:drawable="@drawable/ic_launcher_background" />',
            '    <foreground android:drawable="@drawable/ic_launcher_foreground" />',
            '    <monochrome android:drawable="@drawable/ic_launcher_monochrome" />',
            "</adaptive-icon>",
            "",
        ]
    )


def windows_rc() -> str:
    return "\n".join(
        [
            "/* Generated by tools/draw_app_icon.py -- do not edit. */",
            "#include <windows.h>",
            "",
            'IDI_BENEFACTOR ICON "benefactor.ico"',
            "",
        ]
    )


def text_outputs() -> dict[Path, str]:
    return {
        ICON_SVG: master_svg(),
        WEB_SVG: master_svg(),
        WINDOWS_RC: windows_rc(),
        ANDROID_ROOT / "drawable/ic_launcher_background.xml": android_background(),
        ANDROID_ROOT / "drawable/ic_launcher_foreground.xml": android_foreground(),
        ANDROID_ROOT / "drawable/ic_launcher_monochrome.xml": android_monochrome(),
        ANDROID_ROOT / "mipmap-anydpi/ic_launcher.xml": android_legacy(),
        ANDROID_ROOT / "mipmap-anydpi/ic_launcher_round.xml": android_legacy(),
        ANDROID_ROOT / "mipmap-anydpi-v26/ic_launcher.xml": android_adaptive(),
        ANDROID_ROOT / "mipmap-anydpi-v26/ic_launcher_round.xml": android_adaptive(),
    }


def rasteriser() -> str:
    found = shutil.which("magick") or shutil.which("convert")
    if not found:
        raise SystemExit(
            "app icon: ImageMagick's `magick` (or `convert`) is required to rasterise "
            "the icon at the sizes it ships in, and it is not on PATH"
        )
    return found


def rasterise(source: Path, size: int, destination: Path, viewport: float = CANVAS) -> Path:
    """Rasterise an SVG at `size` px, rendering at that size rather than above it.

    ImageMagick rasterises an SVG at its intrinsic size and resizes afterwards,
    so a 1024 px macOS layer would be a blurred 512 px one. Setting the density
    first makes the renderer draw at the size being asked for; the resize that
    follows only ever downsamples.
    """
    density = max(1, math.ceil(96 * size / viewport))
    subprocess.run(
        [
            rasteriser(),
            "-background",
            "none",
            "-density",
            str(density),
            str(source),
            "-resize",
            f"{size}x{size}",
            str(destination),
        ],
        check=True,
        capture_output=True,
    )
    return destination


@dataclass(frozen=True)
class IcoFrame:
    """One image inside an .ico, and where its bitmap sits in the file."""

    width: int
    height: int
    offset: int
    size: int


def ico_frames(path: Path) -> list[IcoFrame]:
    """Read an .ico back: the size of every frame, after checking its bitmap."""
    data = path.read_bytes()
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if (reserved, kind) != (0, 1):
        raise SystemExit(f"app icon: {path.name} is not an icon container")
    frames = []
    for index in range(count):
        width, height, _colors, _reserved, _planes, _bits, size, offset = struct.unpack_from(
            "<BBBBHHII", data, 6 + index * 16
        )
        if offset + size > len(data):
            raise SystemExit(f"app icon: {path.name} frame {index} runs past the end of the file")
        if struct.unpack_from("<I", data, offset)[0] != 40:
            raise SystemExit(f"app icon: {path.name} frame {index} is not a bitmap")
        frames.append(IcoFrame(width or 256, height or 256, offset, size))
    return frames


def ico_frame_bytes(path: Path, size: int) -> bytes:
    """One frame's bitmap, which is what a Windows resource actually carries.

    A resource compiler rebuilds the container around the frames rather than
    storing the .ico file, so this — not the file header — is the signature the
    frames leave inside an executable.
    """
    for frame in ico_frames(path):
        if frame.width == size and frame.height == size:
            return path.read_bytes()[frame.offset : frame.offset + frame.size]
    raise SystemExit(f"app icon: {path.name} holds no {size}x{size} frame")


def icns_frames(path: Path) -> list[tuple[str, int]]:
    """Read an .icns back, reporting each chunk's kind and the size of its PNG."""
    data = path.read_bytes()
    if data[:4] != b"icns":
        raise SystemExit(f"app icon: {path.name} does not start with an icns header")
    declared = struct.unpack_from(">I", data, 4)[0]
    if declared != len(data):
        raise SystemExit(f"app icon: {path.name} declares {declared}, holds {len(data)}")
    frames, offset = [], 8
    while offset < len(data):
        kind = data[offset : offset + 4]
        size = struct.unpack_from(">I", data, offset + 4)[0]
        if offset + size > len(data):
            raise SystemExit(f"app icon: {path.name} chunk {kind!r} runs past the end of the file")
        if data[offset + 8 : offset + 16] != b"\x89PNG\r\n\x1a\n":
            raise SystemExit(f"app icon: {path.name} chunk {kind!r} is not PNG data")
        width, height = struct.unpack_from(">II", data, offset + 24)
        if width != height:
            raise SystemExit(f"app icon: {path.name} chunk {kind!r} is {width}x{height}")
        frames.append((kind.decode("latin-1"), width))
        offset += size
    return frames


def write_ico(source: Path, destination: Path) -> None:
    """A multi-size .ico: Windows picks the entry that fits the surface."""
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        frames = [rasterise(source, size, temporary / f"{size}.png") for size in ICO_SIZES]
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [rasteriser(), *[str(frame) for frame in frames], str(destination)],
            check=True,
            capture_output=True,
        )
    sizes = [(frame.width, frame.height) for frame in ico_frames(destination)]
    if sizes != [(size, size) for size in ICO_SIZES]:
        raise SystemExit(f"app icon: wrote {sizes} into {destination.name}")


def write_icns(source: Path, destination: Path) -> None:
    """A PNG-based .icns, written directly so any host can produce one."""
    chunks, cache = [], {}
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        for kind, size in ICNS_TYPES.items():
            if size not in cache:
                cache[size] = rasterise(source, size, temporary / f"{size}.png").read_bytes()
            payload = cache[size]
            chunks.append(kind + struct.pack(">I", len(payload) + 8) + payload)
    body = b"".join(chunks)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)


def check() -> int:
    problems = []
    for path, expected in text_outputs().items():
        relative = path.relative_to(ROOT)
        if not path.is_file():
            problems.append(f"{relative} is missing")
        elif path.read_text(encoding="utf-8") != expected:
            problems.append(f"{relative} differs from the generator")
    if WINDOWS_ICO.is_file():
        sizes = [(frame.width, frame.height) for frame in ico_frames(WINDOWS_ICO)]
        if sizes != [(size, size) for size in ICO_SIZES]:
            problems.append(f"benefactor.ico holds {sizes}, wanted {list(ICO_SIZES)}")
    else:
        problems.append("platforms/windows/benefactor.ico is missing")
    if MACOS_ICNS.is_file():
        frames = icns_frames(MACOS_ICNS)
        wanted = [(kind.decode("latin-1"), size) for kind, size in ICNS_TYPES.items()]
        if frames != wanted:
            problems.append(f"Benefactor.icns holds {frames}, wanted {wanted}")
    else:
        problems.append("platforms/macos/Benefactor.icns is missing")
    if problems:
        for problem in problems:
            print(f"app icon: {problem}", file=sys.stderr)
        print(f"app icon: {len(problems)} problem(s); run --write to regenerate", file=sys.stderr)
        return 1
    print(
        f"app icon: {len(text_outputs())} authored form(s) current; "
        f"benefactor.ico holds {len(ICO_SIZES)} sizes; "
        f"Benefactor.icns holds {len(ICNS_TYPES)} PNG chunks"
    )
    return 0


def ink_fraction(path: Path) -> float:
    """Share of an image's pixels that are not transparent."""
    result = subprocess.run(
        [
            rasteriser(),
            str(path),
            "-alpha",
            "extract",
            "-format",
            "%[fx:mean]",
            "info:",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return float(result.stdout.strip())


def mark_fraction(path: Path) -> float:
    """Share of an icon's pixels that read as the mark rather than its tile.

    The mark is warm (red above blue) and the tile behind it is cold, which
    separates one from the other without depending on exact colours — the one
    thing that has to survive antialiasing at 16 px. `--sheet` prints it and the
    retained-source test asserts it, so both measure the icon the same way.
    """
    result = subprocess.run(
        [
            rasteriser(),
            str(path),
            "-alpha",
            "remove",
            "-fx",
            "(r > b) ? 1 : 0",
            "-format",
            "%[fx:mean]",
            "info:",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return float(result.stdout.strip())


def write_legibility_sheet(destination: Path) -> list[tuple[int, float]]:
    """Rasterise the shipping sizes over light, dark, and mid backgrounds.

    Art is checked as a player meets it: rasterised, at the size it ships, on the
    backgrounds a launcher or a file manager might put behind it.
    """
    measured = []
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        rows = []
        for size in (16, 24, 32, 48, 64, 128):
            frame = rasterise(ICON_SVG, size, temporary / f"icon-{size}.png")
            measured.append((size, mark_fraction(frame)))
            row = []
            for background in ("#ffffff", "#7f7f7f", "#101010"):
                cell = temporary / f"cell-{size}-{background.lstrip('#')}.png"
                subprocess.run(
                    [
                        rasteriser(),
                        "-size",
                        f"{size * 2}x{size * 2}",
                        f"xc:{background}",
                        str(frame),
                        "-gravity",
                        "center",
                        "-composite",
                        str(cell),
                    ],
                    check=True,
                    capture_output=True,
                )
                row.append(str(cell))
            rows.append(temporary / f"row-{size}.png")
            subprocess.run(
                [rasteriser(), *row, "+append", str(rows[-1])],
                check=True,
                capture_output=True,
            )
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                rasteriser(),
                *[str(row) for row in rows],
                "-background",
                "#2b2b2b",
                "-append",
                "-filter",
                "point",
                "-resize",
                "50%",
                str(destination),
            ],
            check=True,
            capture_output=True,
        )
    return measured


def android_preview(destination: Path) -> None:
    """Draw the committed Android resources the way a launcher composes them.

    The vector XML is parsed back out of the files that ship, so this shows the
    shipped numbers rather than the numbers this module meant to write, and the
    circle and squircle cells show what an adaptive mask crops away.
    """
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        side = int(ANDROID_CANVAS * 4)
        cells = []
        combined = vector_preview_svg(
            [
                ANDROID_ROOT / "drawable/ic_launcher_background.xml",
                ANDROID_ROOT / "drawable/ic_launcher_foreground.xml",
            ],
            temporary / "adaptive.svg",
        )
        raster = rasterise(combined, side, temporary / "adaptive.png", viewport=ANDROID_CANVAS)
        for name, shape in (
            ("adaptive", None),
            (
                "circle",
                f"circle {side // 2},{side // 2} {side // 2},{side // 2 + side // 2 - 1}",
            ),
            (
                "squircle",
                f"roundrectangle 0,0 {side - 1},{side - 1} "
                f"{int(side * CORNER_FRACTION)},{int(side * CORNER_FRACTION)}",
            ),
        ):
            cell = temporary / f"cell-{name}.png"
            if shape is None:
                shutil.copy2(raster, cell)
            else:
                mask = temporary / f"mask-{name}.png"
                subprocess.run(
                    [
                        rasteriser(),
                        "-background",
                        "none",
                        "-size",
                        f"{side}x{side}",
                        "xc:none",
                        "-fill",
                        "white",
                        "-draw",
                        shape,
                        str(mask),
                    ],
                    check=True,
                    capture_output=True,
                )
                subprocess.run(
                    [
                        rasteriser(),
                        str(raster),
                        str(mask),
                        "-compose",
                        "DstIn",
                        "-composite",
                        str(cell),
                    ],
                    check=True,
                    capture_output=True,
                )
            cells.append(str(cell))
        # Themed icons are tinted by the system, so the monochrome layer is drawn
        # the way a launcher draws it: the stored shape in the theme's tint, over a
        # themed surface. The counters must let that surface through.
        themed = temporary / "cell-monochrome.png"
        tinted = rasterise(
            vector_preview_svg(
                [ANDROID_ROOT / "drawable/ic_launcher_monochrome.xml"],
                temporary / "monochrome.svg",
                fill="#a8c7fa",
            ),
            side,
            temporary / "monochrome.png",
            viewport=ANDROID_CANVAS,
        )
        subprocess.run(
            [
                rasteriser(),
                "-size",
                f"{side}x{side}",
                "xc:#1c1b1f",
                str(tinted),
                "-composite",
                str(themed),
            ],
            check=True,
            capture_output=True,
        )
        cells.append(str(themed))
        cells.append(
            str(
                rasterise(
                    vector_preview_svg(
                        [ANDROID_ROOT / "mipmap-anydpi/ic_launcher.xml"],
                        temporary / "legacy.svg",
                    ),
                    side,
                    temporary / "cell-legacy.png",
                    viewport=ANDROID_CANVAS,
                )
            )
        )
        destination.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [
                rasteriser(),
                *cells,
                "-background",
                "#2b2b2b",
                "-resize",
                "25%",
                "+append",
                str(destination),
            ],
            check=True,
            capture_output=True,
        )


def vector_preview_svg(sources: list[Path], destination: Path, fill: str | None = None) -> Path:
    """Turn committed VectorDrawables into a single SVG ImageMagick can rasterise.

    `fill` substitutes every path's colour, which is how a themed layer is drawn:
    one shape, whatever tint the system chose.
    """
    parts = []
    size = None
    for source in sources:
        root = ElementTree.parse(source).getroot()
        size = size or (
            root.get(f"{ANDROID_NAMESPACE}viewportWidth"),
            root.get(f"{ANDROID_NAMESPACE}viewportHeight"),
        )
        for child in root.findall("path"):
            colour = fill or child.get(f"{ANDROID_NAMESPACE}fillColor")
            rule = ' fill-rule="evenodd"' if child.get(f"{ANDROID_NAMESPACE}fillType") else ""
            path = child.get(f"{ANDROID_NAMESPACE}pathData")
            parts.append(f'  <path fill="{colour}"{rule} d="{path}"/>')
    width, height = size
    destination.write_text(
        "\n".join(
            [
                f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}"'
                f' viewBox="0 0 {width} {height}">',
                *parts,
                "</svg>",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true", help="write every form of the icon")
    group.add_argument("--check", action="store_true", help="verify the committed forms")
    group.add_argument(
        "--sheet",
        type=Path,
        metavar="PATH",
        help="rasterise a legibility sheet over light, dark, and mid backgrounds",
    )
    group.add_argument(
        "--android-preview",
        type=Path,
        metavar="PATH",
        help="rasterise the committed Android layers as a launcher composes them",
    )
    args = parser.parse_args()
    if args.write:
        for path, content in text_outputs().items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        write_ico(ICON_SVG, WINDOWS_ICO)
        write_icns(ICON_SVG, MACOS_ICNS)
        print(
            f"app icon: wrote {len(text_outputs())} authored form(s), benefactor.ico "
            f"({len(ICO_SIZES)} sizes), and Benefactor.icns ({len(ICNS_TYPES)} chunks)"
        )
        return 0
    if args.sheet is not None:
        for size, mark in write_legibility_sheet(args.sheet.resolve()):
            print(f"app icon: {size:>4} px carries the mark over {mark:.3f} of its square")
        print(f"app icon: sheet written to {args.sheet}")
        return 0
    if args.android_preview is not None:
        android_preview(args.android_preview.resolve())
        print(f"app icon: Android preview written to {args.android_preview}")
        return 0
    return check()


if __name__ == "__main__":
    raise SystemExit(main())
