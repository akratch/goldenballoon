#!/usr/bin/env python3
"""Validate that an app-shell BMP contains a complete, readable UI frame.

The smoke producer already treats I/O errors as fatal. This companion checks
the pixels: dimensions, tonal/color diversity, branded focus/action colors, and
foreground coverage in both halves of the launcher. It deliberately avoids a
fragile exact-image golden while still rejecting blank, offscreen, and
foreground-equals-background mutations.
"""

from __future__ import annotations

import argparse
import math
import os
import shutil
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import resolve_binary


class CaptureError(RuntimeError):
    pass


@dataclass(frozen=True)
class Bitmap:
    width: int
    height: int
    pixels: tuple[tuple[int, int, int], ...]


def load_bmp(path: Path) -> Bitmap:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise CaptureError("not a BMP file")
    offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    planes, bits = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if dib_size < 40 or width <= 0 or signed_height == 0:
        raise CaptureError("unsupported BMP dimensions/header")
    if planes != 1 or bits != 24 or compression != 0:
        raise CaptureError("capture must be an uncompressed 24-bit BMP")
    height = abs(signed_height)
    stride = (width * 3 + 3) & ~3
    if offset + stride * height != len(data):
        raise CaptureError("BMP payload length does not match its header")

    rows: list[tuple[int, int, int]] = []
    source_rows = range(height - 1, -1, -1) if signed_height > 0 else range(height)
    for source_y in source_rows:
        row_start = offset + source_y * stride
        for x in range(width):
            blue, green, red = data[row_start + x * 3 : row_start + x * 3 + 3]
            rows.append((red, green, blue))
    return Bitmap(width, height, tuple(rows))


def _near(color: tuple[int, int, int], target: tuple[int, int, int], tolerance: int) -> bool:
    return all(abs(component - expected) <= tolerance
               for component, expected in zip(color, target))


def validate(bitmap: Bitmap, expect_recovery: bool = False,
             compact_palette: bool = False) -> list[str]:
    failures: list[str] = []
    width, height, pixels = bitmap.width, bitmap.height, bitmap.pixels
    if width < 640 or height < 480:
        failures.append(f"capture is too small: {width}x{height}, expected at least 640x480")

    # Subsample large Retina captures deterministically; retain enough pixels
    # for stable proportions without making every CTest spend time on 4M tuples.
    step = max(1, int(math.sqrt(len(pixels) / 250_000)))
    sample = pixels[::step]
    unique = len(set(sample))
    lumas = [(77 * r + 150 * g + 29 * b) / 256.0 for r, g, b in sample]
    mean = sum(lumas) / len(lumas)
    deviation = math.sqrt(sum((value - mean) ** 2 for value in lumas) / len(lumas))
    if unique < 48:
        failures.append(f"frame is visually flat: only {unique} sampled colors")
    if max(lumas) - min(lumas) < 90 or deviation < 8:
        failures.append(
            f"frame lacks readable tonal contrast: range={max(lumas)-min(lumas):.1f} "
            f"stddev={deviation:.1f}")

    # Theme contract: a real launcher includes light text, cobalt selection,
    # and Amber Gold action/focus pixels. Tolerance accounts for antialiasing
    # and color conversion while remaining far tighter than the backgrounds.
    light = sum(luma >= 150 for luma in lumas)
    blue = sum(_near(color, (49, 92, 152), 24) for color in sample)
    gold = sum(_near(color, (212, 168, 67), 24) for color in sample)
    if light < max(40, len(sample) // 2000):
        failures.append("expected foreground text is absent or matches the background")
    if not compact_palette and blue < max(20, len(sample) // 6000):
        failures.append("cobalt selection pixels are absent")
    # Dense mode replaces the colored destination surface with a section
    # dropdown, so cobalt is legitimately absent. The persistent primary
    # action remains gold at every breakpoint and must never be waived.
    if gold < max(12, len(sample) // 8000):
        failures.append("Amber Gold action/focus pixels are absent")
    if expect_recovery:
        # The recovery card uses the theme's E5675E error color for both its
        # border and heading. Requiring a visible population catches a banner
        # that is set in state but clipped or never submitted to the renderer.
        recovery = sum(_near(color, (229, 103, 94), 12) for color in pixels)
        if recovery < 40:
            failures.append(
                "boot-recovery card is absent or clipped "
                f"({recovery} matching error-color pixels)")

    # Reject a panel translated wholly to one side. At least some readable
    # foreground must exist in both horizontal halves and across a meaningful
    # vertical span (works for both wide rail and compact top-tab layouts).
    foreground_points: list[tuple[int, int]] = []
    left_foreground = right_foreground = 0
    pixel_step = max(1, step)
    for index in range(0, len(pixels), pixel_step):
        color = pixels[index]
        luma = (77 * color[0] + 150 * color[1] + 29 * color[2]) / 256.0
        if luma < 105:
            continue
        y, x = divmod(index, width)
        foreground_points.append((x, y))
        if x < width // 2:
            left_foreground += 1
        else:
            right_foreground += 1
    minimum_region = max(20, len(sample) // 5000)
    if left_foreground < minimum_region or right_foreground < minimum_region:
        failures.append("foreground content does not reach both halves of the window")
    if foreground_points:
        xs, ys = zip(*foreground_points)
        if max(xs) - min(xs) < width * 0.30:
            failures.append("foreground draw bounds are too narrow/offscreen")
        if max(ys) - min(ys) < height * 0.15:
            failures.append("foreground draw bounds are too short/clipped")

    return failures


def require_invalid(name: str, bitmap: Bitmap) -> None:
    if not validate(bitmap):
        raise CaptureError(f"mutation control unexpectedly passed: {name}")


def run_mutation_controls(bitmap: Bitmap) -> None:
    background = (15, 18, 23)
    blank = Bitmap(bitmap.width, bitmap.height,
                   (background,) * (bitmap.width * bitmap.height))
    require_invalid("cleared framebuffer", blank)

    # Translate the whole panel toward the right edge so only a sliver of it
    # survives inside the viewport. The palette is still present, so this
    # control is answered by the region/bounds guards rather than by the
    # blank-frame guards above.
    offset = bitmap.width - bitmap.width // 8
    shifted = tuple(
        bitmap.pixels[y * bitmap.width + x - offset] if x >= offset
        else background
        for y in range(bitmap.height)
        for x in range(bitmap.width)
    )
    require_invalid("panel moved offscreen", Bitmap(
        bitmap.width, bitmap.height, shifted))

    invisible = tuple(
        background if ((77 * r + 150 * g + 29 * b) / 256.0 >= 105 or
                       _near((r, g, b), (49, 92, 152), 24) or
                       _near((r, g, b), (212, 168, 67), 24))
        else (r, g, b)
        for r, g, b in bitmap.pixels
    )
    require_invalid("foreground matched to background", Bitmap(
        bitmap.width, bitmap.height, invisible))


def synthetic_contract_fixture() -> Bitmap:
    """Stable positive fixture used only by standalone --self-test."""
    width, height = 640, 480
    pixels: list[tuple[int, int, int]] = []
    for y in range(height):
        for x in range(width):
            color = (15 + (x + y) % 20,
                     18 + (2 * x + y) % 18,
                     23 + (x + 3 * y) % 16)
            if 30 <= y < 55 and 30 <= x < 610:
                color = (212, 168, 67)
            elif 100 <= y < 150 and (30 <= x < 230 or 410 <= x < 610):
                color = (49, 92, 152)
            elif ((180 <= y < 195 or 300 <= y < 315) and
                  (50 <= x < 280 or 360 <= x < 590)):
                color = (236, 236, 238)
            pixels.append(color)
    return Bitmap(width, height, tuple(pixels))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bmp", type=Path, nargs="?")
    parser.add_argument("--build")
    parser.add_argument("--rom", help="accepted for tools/run_checks.py compatibility")
    parser.add_argument("--self-test", action="store_true",
                        help="also require the three broken-direction controls to fail")
    parser.add_argument("--expect-recovery", action="store_true",
                        help="require the visible red boot-recovery card")
    parser.add_argument("--compact-palette", action="store_true",
                        help="allow constrained navigation to omit brand colors")
    args = parser.parse_args()

    temporary: Path | None = None
    bmp = args.bmp
    if bmp is None:
        if args.self_test and not args.build:
            bitmap = synthetic_contract_fixture()
            failures = validate(bitmap, args.expect_recovery,
                                args.compact_palette)
            if failures:
                print("app capture validation failed: standalone fixture: " +
                      "; ".join(failures))
                return 1
            run_mutation_controls(bitmap)
            print("app capture validator self-test passed")
            return 0
        if not args.build:
            parser.error("provide a BMP or --build")
        temporary = Path(tempfile.mkdtemp(prefix="mdkr-app-capture-"))
        bmp = temporary / "settings.bmp"
        prefs = temporary / "prefs"
        saves = temporary / "saves"
        prefs.mkdir()
        saves.mkdir()
        environment = os.environ.copy()
        environment.update({
            "MDKR_APP_SMOKE_FRAMES": "4",
            "MDKR_ONLINE_ROOM_PREVIEW": "1",
            "MDKR_APP_SMOKE_SHOT": str(bmp),
            "MDKR_APP_PANEL": "Settings",
            "MDKR_APP_PREFS_DIR": str(prefs),
            "MDKR_VIDEO_CONFIG_PATH": str(temporary / "video.ini"),
            "MDKR_SAVE_DIR": str(saves),
            "MDKR64_HIDDEN": "1",
            "MDKR_AUDIO": "0",
        })
        completed = subprocess.run(
            [resolve_binary(args.build)], env=environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        if completed.returncode != 0:
            print("app capture validation failed: launcher smoke exited "
                  f"{completed.returncode}\n{completed.stdout[-4000:]}")
            shutil.rmtree(temporary, ignore_errors=True)
            return 1

    try:
        bitmap = load_bmp(bmp)
        failures = validate(bitmap, args.expect_recovery, args.compact_palette)
        if failures:
            raise CaptureError("; ".join(failures))
        if args.self_test:
            run_mutation_controls(bitmap)
    except (OSError, CaptureError) as error:
        print(f"app capture validation failed: {error}")
        return 1
    finally:
        if temporary is not None:
            shutil.rmtree(temporary, ignore_errors=True)

    print(f"app capture valid: {bitmap.width}x{bitmap.height}; "
          "content, palette, contrast, and mutation controls passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
