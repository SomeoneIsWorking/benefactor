#!/usr/bin/env python3
"""Fit one authored image to every platform's form of the app icon.

`platforms/icons/benefactor-mark.png` is the artwork — a frame of the game
itself, used as it is rather than redrawn — and this file is what fits it to
each platform, so one change lands everywhere instead of leaving four platforms
with four different marks.

The mark is a bitmap, and that decides the shape of everything below. Nothing
here draws: every output is the same image cropped square, scaled, and masked.
The desktop and web forms carry it inside an SVG, which is a wrapper around the
authored PNG rather than a drawing of it. Android cannot use that at all — a
VectorDrawable holds no raster — so its launcher icon is density bitmaps, and
its adaptive icon puts the image on the background layer, which is the layer a
launcher's mask is meant to crop. It writes:

    platforms/icons/benefactor.svg                            desktop icon
    platforms/web/icon.svg                                    browser tab icon
    platforms/android/.../mipmap-<density>/ic_launcher.png        launcher tile
    platforms/android/.../mipmap-<density>/ic_launcher_round.png  round launcher tile
    platforms/android/.../mipmap-<density>/ic_launcher_background.png  adaptive layer
    platforms/android/.../drawable/ic_launcher_foreground.xml  empty adaptive layer
    platforms/android/.../mipmap-anydpi-v26/ic_launcher*.xml   adaptive icon
    platforms/windows/benefactor.rc                           icon resource statement
    platforms/windows/benefactor.ico                          multi-size Windows icon
    platforms/macos/Benefactor.icns                           macOS bundle icon

`--check` re-derives the text forms and compares them exactly, re-renders every
bitmap and compares it to the committed one, and reads the two binary containers
back, so a stale or truncated icon cannot pass as current. The bitmap comparison
allows a small difference rather than demanding identical bytes: ImageMagick's
resampling is not identical across versions, and the check runs on three
operating systems. A stale icon is a different picture, not a rounding
difference, so the bar catches it either way.

    python3 -m tools.draw_app_icon --write
    python3 -m tools.draw_app_icon --check
"""

from __future__ import annotations

import argparse
import base64
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from tools.paths import ROOT

MARK = ROOT / "platforms/icons/benefactor-mark.png"

CANVAS = 512
CORNER_FRACTION = 0.219
"""How far the tile's corners are rounded, as a share of its side.

The proportion Apple and the Android reference squircle both sit near; the
image itself is square-cropped and full-bleed, so this rounding is the only
shaping the icon gets.
"""

#: The mask a form's corners get. `TILE` is the rounded square every desktop
#: platform draws, `ROUND` the circle Android's round launcher icon wants, and
#: `FULL` no mask at all — the adaptive background layer, which a launcher masks
#: itself and which must therefore reach every edge.
TILE, ROUND, FULL = "tile", "round", "full"

ANDROID_ROOT = ROOT / "platforms/android/app/src/main/res"

#: Android sizes an icon in density-independent pixels and expects one bitmap
#: per density bucket. A launcher icon is 48dp; an adaptive layer is 108dp.
ANDROID_DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
ANDROID_LAUNCHER_DP = 48
ANDROID_CANVAS = 108.0

#: The adaptive layer a launcher guarantees is visible: the central 72 of 108dp,
#: two thirds of each side. Everything outside it is parallax and crop, so the
#: image is composed to survive being cut back to it.
ANDROID_SAFE_DP = 72.0

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

ICON_SVG = ROOT / "platforms/icons/benefactor.svg"
WEB_SVG = ROOT / "platforms/web/icon.svg"
WINDOWS_RC = ROOT / "platforms/windows/benefactor.rc"
WINDOWS_ICO = ROOT / "platforms/windows/benefactor.ico"
MACOS_ICNS = ROOT / "platforms/macos/Benefactor.icns"

MAX_BITMAP_DIFFERENCE = 0.02
"""How far a committed bitmap may sit from a fresh render, as root-mean-square.

Two ImageMagick versions resample slightly differently, so demanding identical
bytes would make the check fail on a host rather than on a stale icon. Two
percent is far below any real change to the artwork and far above the difference
between two renderers' filters.
"""


def plain(value: float) -> str:
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return text if text not in ("", "-0") else "0"


def rasteriser() -> str:
    found = shutil.which("magick") or shutil.which("convert")
    if not found:
        raise SystemExit(
            "app icon: ImageMagick's `magick` (or `convert`) is required to fit the icon "
            "to the sizes it ships in, and it is not on PATH"
        )
    return found


def _magick(*arguments: str) -> str:
    result = subprocess.run([rasteriser(), *arguments], check=True, capture_output=True, text=True)
    return result.stdout.strip()


def mask_shape(shape: str, size: int) -> str | None:
    """The ImageMagick drawing that keeps a form's own pixels, or None for all."""
    if shape == FULL:
        return None
    if shape == ROUND:
        half = size / 2
        return f"circle {half},{half} {half},0"
    radius = size * CORNER_FRACTION
    return f"roundrectangle 0,0 {size},{size} {plain(radius)},{plain(radius)}"


def render(shape: str, size: int, destination: Path) -> Path:
    """The mark, cropped square about its centre, at `size` px, masked by `shape`.

    The mask is drawn four times too large and scaled down, because ImageMagick
    draws an aliased edge: at 16 px a jagged corner is the whole difference
    between a tile and a staircase. The image itself is enlarged with a point
    filter — it is a frame of a 1994 game, and smoothing its pixels into each
    other on a 1024 px macOS layer would lose the thing being shown.
    """
    filtering = ["-filter", "point"] if size > min(png_size(MARK)) else []
    destination.parent.mkdir(parents=True, exist_ok=True)
    shape_drawing = mask_shape(shape, size * 4)
    arguments = [
        str(MARK),
        *filtering,
        "-resize",
        f"{size}x{size}^",
        "-gravity",
        "center",
        "-extent",
        f"{size}x{size}",
    ]
    if shape_drawing is not None:
        arguments += [
            "(",
            "-size",
            f"{size * 4}x{size * 4}",
            "xc:none",
            "-fill",
            "white",
            "-draw",
            shape_drawing,
            "-resize",
            f"{size}x{size}",
            ")",
            "-alpha",
            "set",
            "-compose",
            "DstIn",
            "-composite",
        ]
    _magick(*arguments, "-strip", str(destination))
    return destination


def master_svg() -> str:
    """The desktop and web icon: the authored PNG, clipped to the tile.

    An SVG here is a container, not a drawing. Every consumer of this file — a
    browser tab, a freedesktop icon theme, an AppImage — renders raster content
    inside SVG, and wrapping the bitmap keeps one file serving every size
    instead of a directory of them.
    """
    radius = plain(CORNER_FRACTION * CANVAS)
    payload = base64.b64encode(MARK.read_bytes()).decode("ascii")
    return "\n".join(
        [
            f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"'
            f' width="{CANVAS}" height="{CANVAS}" viewBox="0 0 {CANVAS} {CANVAS}">',
            "  <defs>",
            '    <clipPath id="tile">',
            f'      <rect width="{CANVAS}" height="{CANVAS}" rx="{radius}"/>',
            "    </clipPath>",
            "  </defs>",
            f'  <image clip-path="url(#tile)" width="{CANVAS}" height="{CANVAS}"',
            '      preserveAspectRatio="xMidYMid slice" image-rendering="pixelated"',
            f'      xlink:href="data:image/png;base64,{payload}"/>',
            "</svg>",
            "",
        ]
    )


def android_foreground() -> str:
    """An empty foreground layer: the picture is the background layer.

    An adaptive icon needs both layers declared, and a photographic mark cannot
    be split into a background and a thing standing on it. Putting it on the
    background is what makes a launcher's mask crop the picture rather than
    float it; the foreground is then a layer with nothing in it, which is
    declared rather than omitted because a missing drawable is a build failure.
    """
    size = plain(ANDROID_CANVAS)
    return "\n".join(
        [
            '<?xml version="1.0" encoding="utf-8"?>',
            '<vector xmlns:android="http://schemas.android.com/apk/res/android"',
            f'    android:width="{size}dp"',
            f'    android:height="{size}dp"',
            f'    android:viewportWidth="{size}"',
            f'    android:viewportHeight="{size}">',
            f'    <path android:fillColor="#00000000" '
            f'android:pathData="M0,0h{size}v{size}h-{size}z" />',
            "</vector>",
            "",
        ]
    )


def android_adaptive() -> str:
    """No monochrome layer, so a themed launcher draws the picture instead.

    A themed icon is one flat shape in the system's tint. There is no honest
    one-colour reduction of a photograph, and a launcher only themes an icon
    that offers the layer — leaving it out means a themed home screen shows this
    icon as it is, which is the better of the two available outcomes.
    """
    return "\n".join(
        [
            '<?xml version="1.0" encoding="utf-8"?>',
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">',
            '    <background android:drawable="@mipmap/ic_launcher_background" />',
            '    <foreground android:drawable="@drawable/ic_launcher_foreground" />',
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
        ANDROID_ROOT / "drawable/ic_launcher_foreground.xml": android_foreground(),
        ANDROID_ROOT / "mipmap-anydpi-v26/ic_launcher.xml": android_adaptive(),
        ANDROID_ROOT / "mipmap-anydpi-v26/ic_launcher_round.xml": android_adaptive(),
    }


@dataclass(frozen=True)
class Bitmap:
    """One committed PNG: how big it is and what shape its corners are."""

    size: int
    shape: str


def bitmap_outputs() -> dict[Path, Bitmap]:
    """Every PNG that ships, so `--write` and `--check` cannot disagree."""
    outputs: dict[Path, Bitmap] = {}
    for density, factor in ANDROID_DENSITIES.items():
        folder = ANDROID_ROOT / f"mipmap-{density}"
        launcher = round(ANDROID_LAUNCHER_DP * factor)
        layer = round(ANDROID_CANVAS * factor)
        outputs[folder / "ic_launcher.png"] = Bitmap(launcher, TILE)
        outputs[folder / "ic_launcher_round.png"] = Bitmap(launcher, ROUND)
        outputs[folder / "ic_launcher_background.png"] = Bitmap(layer, FULL)
    return outputs


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
    return [(kind, size) for kind, size, _payload in icns_payloads(path)]


def icns_payloads(path: Path) -> list[tuple[str, int, bytes]]:
    """Every chunk of an .icns: its kind, the size of its PNG, and the PNG."""
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
        frames.append((kind.decode("latin-1"), width, data[offset + 8 : offset + size]))
        offset += size
    return frames


def write_ico(destination: Path) -> None:
    """A multi-size .ico: Windows picks the entry that fits the surface."""
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        frames = [render(TILE, size, temporary / f"{size}.png") for size in ICO_SIZES]
        destination.parent.mkdir(parents=True, exist_ok=True)
        _magick(*[str(frame) for frame in frames], str(destination))
    sizes = [(frame.width, frame.height) for frame in ico_frames(destination)]
    if sizes != [(size, size) for size in ICO_SIZES]:
        raise SystemExit(f"app icon: wrote {sizes} into {destination.name}")


def write_icns(destination: Path) -> None:
    """A PNG-based .icns, written directly so any host can produce one."""
    chunks, cache = [], {}
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        for kind, size in ICNS_TYPES.items():
            if size not in cache:
                cache[size] = render(TILE, size, temporary / f"{size}.png").read_bytes()
            payload = cache[size]
            chunks.append(kind + struct.pack(">I", len(payload) + 8) + payload)
    body = b"".join(chunks)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)


def png_size(path: Path) -> tuple[int, int]:
    """A PNG's own dimensions, read from the file rather than from a renderer."""
    header = path.read_bytes()[:24]
    if header[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"app icon: {path.name} is not a PNG")
    return struct.unpack_from(">II", header, 16)


def comparer() -> list[str]:
    """ImageMagick's comparison, however this host spells it.

    Version 7 puts it behind `magick compare`; version 6 ships it as its own
    `compare` binary and its `convert` does not answer to the name.
    """
    seven = shutil.which("magick")
    if seven:
        return [seven, "compare"]
    six = shutil.which("compare")
    if not six:
        raise SystemExit(
            "app icon: ImageMagick's `compare` is required to check a committed bitmap "
            "against the artwork, and it is not on PATH"
        )
    return [six]


def difference(first: Path, second: Path) -> float:
    """Root-mean-square distance between two images, 0 for identical ones."""
    result = subprocess.run(
        [*comparer(), "-metric", "RMSE", str(first), str(second), "null:"],
        capture_output=True,
        text=True,
    )
    reported = (result.stderr or result.stdout).strip()
    try:
        return float(reported.split("(")[1].split(")")[0])
    except (IndexError, ValueError) as error:
        raise SystemExit(f"app icon: could not compare {first.name}: {reported}") from error


def check_bitmaps() -> list[str]:
    """Re-render every committed PNG and report the ones that have gone stale."""
    problems = []
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        for path, bitmap in bitmap_outputs().items():
            relative = path.relative_to(ROOT)
            if not path.is_file():
                problems.append(f"{relative} is missing")
                continue
            if png_size(path) != (bitmap.size, bitmap.size):
                width, height = png_size(path)
                problems.append(
                    f"{relative} is {width}x{height}, wanted {bitmap.size}x{bitmap.size}"
                )
                continue
            fresh = render(bitmap.shape, bitmap.size, temporary / path.name)
            apart = difference(path, fresh)
            if apart > MAX_BITMAP_DIFFERENCE:
                problems.append(f"{relative} differs from the artwork by {apart:.3f}")
    return problems


def check_containers() -> list[str]:
    """Compare what the desktop containers actually hold against the artwork.

    The frame counts and sizes are structure, and structure was all this
    checked: an .icns full of last year's drawing at exactly the right eight
    sizes passed. These are the two files a desktop launcher reads, so they are
    the two where being stale is least visible and most worth catching.
    """
    problems = []
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        fresh = {}

        def artwork(size: int) -> Path:
            if size not in fresh:
                fresh[size] = render(TILE, size, temporary / f"fresh-{size}.png")
            return fresh[size]

        for index, frame in enumerate(ico_frames(WINDOWS_ICO)):
            held = temporary / f"ico-{index}.png"
            _magick(f"{WINDOWS_ICO}[{index}]", str(held))
            apart = difference(held, artwork(frame.width))
            if apart > MAX_BITMAP_DIFFERENCE:
                problems.append(
                    f"benefactor.ico's {frame.width}x{frame.width} frame differs "
                    f"from the artwork by {apart:.3f}"
                )
        for kind, size, payload in icns_payloads(MACOS_ICNS):
            held = temporary / f"icns-{kind}.png"
            held.write_bytes(payload)
            apart = difference(held, artwork(size))
            if apart > MAX_BITMAP_DIFFERENCE:
                problems.append(
                    f"Benefactor.icns chunk {kind} ({size}x{size}) differs "
                    f"from the artwork by {apart:.3f}"
                )
    return problems


def check() -> int:
    problems = []
    if not MARK.is_file():
        problems.append(f"{MARK.relative_to(ROOT)} is missing, so there is no icon to fit")
    for path, expected in text_outputs().items():
        relative = path.relative_to(ROOT)
        if not path.is_file():
            problems.append(f"{relative} is missing")
        elif path.read_text(encoding="utf-8") != expected:
            problems.append(f"{relative} differs from the generator")
    bitmaps = "not checked"
    if shutil.which("magick") or shutil.which("convert"):
        problems += check_bitmaps()
        if WINDOWS_ICO.is_file() and MACOS_ICNS.is_file():
            problems += check_containers()
        bitmaps = f"{len(bitmap_outputs())} bitmap(s) match the artwork"
    else:
        print(
            "app icon: ImageMagick is not on PATH, so the committed bitmaps were NOT "
            "checked against the artwork; only the authored text forms were",
            file=sys.stderr,
        )
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
        f"app icon: {len(text_outputs())} authored form(s) current; {bitmaps}; "
        f"every frame of benefactor.ico ({len(ICO_SIZES)}) and of "
        f"Benefactor.icns ({len(ICNS_TYPES)}) matches it too"
    )
    return 0


def detail(path: Path) -> float:
    """How much the image varies across itself, as a standard deviation of 0 to 1.

    A picture that has survived being scaled down to a launcher size still varies
    from pixel to pixel; one that has been reduced to a flat square does not. It
    is the cheapest measurement that tells those two apart, and the legibility
    sheet and the test that guards it both read it from here.
    """
    return float(
        _magick(str(path), "-colorspace", "gray", "-format", "%[fx:standard_deviation]", "info:")
    )


def write_legibility_sheet(destination: Path) -> list[tuple[int, float]]:
    """Render the shipping sizes over light, dark, and mid backgrounds.

    Art is checked as a player meets it: at the size it ships, on the
    backgrounds a launcher or a file manager might put behind it.
    """
    measured = []
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        rows = []
        for size in (16, 24, 32, 48, 64, 128):
            frame = render(TILE, size, temporary / f"icon-{size}.png")
            measured.append((size, detail(frame)))
            row = []
            for background in ("#ffffff", "#7f7f7f", "#101010"):
                cell = temporary / f"cell-{size}-{background.lstrip('#')}.png"
                _magick(
                    "-size",
                    f"{size * 2}x{size * 2}",
                    f"xc:{background}",
                    str(frame),
                    "-gravity",
                    "center",
                    "-composite",
                    str(cell),
                )
                row.append(str(cell))
            rows.append(temporary / f"row-{size}.png")
            _magick(*row, "+append", str(rows[-1]))
        destination.parent.mkdir(parents=True, exist_ok=True)
        _magick(
            *[str(row) for row in rows],
            "-background",
            "#2b2b2b",
            "-append",
            "-filter",
            "point",
            "-resize",
            "50%",
            str(destination),
        )
    return measured


def android_preview(destination: Path) -> None:
    """Draw the committed adaptive layer the way a launcher composes it.

    The background bitmap that ships is the one read, so this shows what a phone
    would show. A launcher keeps the central 72 of the layer's 108dp and masks
    that square to its own shape, so the circle and squircle cells crop first and
    mask second, which is the order that shows what is actually lost.
    """
    source = ANDROID_ROOT / "mipmap-xxxhdpi/ic_launcher_background.png"
    side = png_size(source)[0]
    visible = round(side * ANDROID_SAFE_DP / ANDROID_CANVAS)
    inset = (side - visible) // 2
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        cells = [str(source)]
        for shape in (ROUND, TILE):
            cropped = temporary / f"crop-{shape}.png"
            _magick(
                str(source),
                "-crop",
                f"{visible}x{visible}+{inset}+{inset}",
                "+repage",
                str(cropped),
            )
            mask = temporary / f"mask-{shape}.png"
            _magick(
                "-size",
                f"{visible * 4}x{visible * 4}",
                "xc:none",
                "-fill",
                "white",
                "-draw",
                str(mask_shape(shape, visible * 4)),
                "-resize",
                f"{visible}x{visible}",
                str(mask),
            )
            cell = temporary / f"cell-{shape}.png"
            _magick(
                str(cropped),
                str(mask),
                "-alpha",
                "set",
                "-compose",
                "DstIn",
                "-composite",
                str(cell),
            )
            cells.append(str(cell))
        cells.append(str(ANDROID_ROOT / "mipmap-xxxhdpi/ic_launcher.png"))
        cells.append(str(ANDROID_ROOT / "mipmap-xxxhdpi/ic_launcher_round.png"))
        destination.parent.mkdir(parents=True, exist_ok=True)
        _magick(
            *cells,
            "-background",
            "#2b2b2b",
            "-gravity",
            "center",
            "-resize",
            "25%",
            "+append",
            str(destination),
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true", help="write every form of the icon")
    group.add_argument("--check", action="store_true", help="verify the committed forms")
    group.add_argument(
        "--sheet",
        type=Path,
        metavar="PATH",
        help="render a legibility sheet over light, dark, and mid backgrounds",
    )
    group.add_argument(
        "--android-preview",
        type=Path,
        metavar="PATH",
        help="render the committed Android layer as a launcher masks it",
    )
    args = parser.parse_args()
    if args.write:
        for path, content in text_outputs().items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
        for path, bitmap in bitmap_outputs().items():
            render(bitmap.shape, bitmap.size, path)
        write_ico(WINDOWS_ICO)
        write_icns(MACOS_ICNS)
        print(
            f"app icon: wrote {len(text_outputs())} authored form(s), "
            f"{len(bitmap_outputs())} bitmap(s), benefactor.ico "
            f"({len(ICO_SIZES)} sizes), and Benefactor.icns ({len(ICNS_TYPES)} chunks)"
        )
        return 0
    if args.sheet is not None:
        for size, varies in write_legibility_sheet(args.sheet.resolve()):
            print(f"app icon: {size:>4} px keeps {varies:.3f} of the picture's variation")
        print(f"app icon: sheet written to {args.sheet}")
        return 0
    if args.android_preview is not None:
        android_preview(args.android_preview.resolve())
        print(f"app icon: Android preview written to {args.android_preview}")
        return 0
    return check()


if __name__ == "__main__":
    raise SystemExit(main())
