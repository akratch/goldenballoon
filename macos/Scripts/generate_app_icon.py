#!/usr/bin/env python3
"""Generate the macOS app icon at build time.

With --source, resizes the tracked brand/appicon-source.png (a narrow,
audited exception to the release guard's tracked-binary ban -- see
tools/check_no_rom.sh / tools/check_clean_room.sh) into an .iconset via
`sips`. Without --source, falls back to the original procedurally-rendered
placeholder (used by the asset-free-verifier test harness, which just needs
*an* icon, not the real one).
"""

from __future__ import annotations

import argparse
import math
import shutil
import struct
import subprocess
import zlib
from pathlib import Path


ICON_SPECS = (
    (16, 1, "icon_16x16.png"),
    (16, 2, "icon_16x16@2x.png"),
    (32, 1, "icon_32x32.png"),
    (32, 2, "icon_32x32@2x.png"),
    (128, 1, "icon_128x128.png"),
    (128, 2, "icon_128x128@2x.png"),
    (256, 1, "icon_256x256.png"),
    (256, 2, "icon_256x256@2x.png"),
    (512, 1, "icon_512x512.png"),
    (512, 2, "icon_512x512@2x.png"),
)

CHARCOAL = (28, 28, 30)
GRAPHITE = (44, 44, 46)
STEEL_BLUE = (74, 111, 165)
STEEL_BLUE_LIGHT = (111, 151, 206)
AMBER = (212, 168, 67)
AMBER_LIGHT = (238, 196, 101)


def clamp(value: float, low: float = 0.0, high: float = 1.0) -> float:
    return max(low, min(high, value))


def smoothstep(edge0: float, edge1: float, value: float) -> float:
    if edge0 == edge1:
        return 1.0 if value >= edge1 else 0.0
    t = clamp((value - edge0) / (edge1 - edge0))
    return t * t * (3.0 - 2.0 * t)


def mix(a: tuple[int, int, int], b: tuple[int, int, int], t: float) -> tuple[int, int, int]:
    return tuple(int(round(a[i] * (1.0 - t) + b[i] * t)) for i in range(3))


def over(
    dst: tuple[float, float, float, float],
    src_rgb: tuple[int, int, int],
    src_alpha: float,
) -> tuple[float, float, float, float]:
    src_alpha = clamp(src_alpha)
    if src_alpha <= 0.0:
        return dst
    out_alpha = src_alpha + dst[3] * (1.0 - src_alpha)
    if out_alpha <= 0.0:
        return (0.0, 0.0, 0.0, 0.0)
    src = tuple(c / 255.0 for c in src_rgb)
    out_rgb = tuple(
        (src[i] * src_alpha + dst[i] * dst[3] * (1.0 - src_alpha)) / out_alpha
        for i in range(3)
    )
    return (out_rgb[0], out_rgb[1], out_rgb[2], out_alpha)


def rounded_rect_distance(x: float, y: float, cx: float, cy: float, half: float, radius: float) -> float:
    qx = abs(x - cx) - (half - radius)
    qy = abs(y - cy) - (half - radius)
    outside_x = max(qx, 0.0)
    outside_y = max(qy, 0.0)
    outside = math.hypot(outside_x, outside_y)
    inside = min(max(qx, qy), 0.0)
    return outside + inside - radius


def segment_distance(px: float, py: float, ax: float, ay: float, bx: float, by: float) -> float:
    vx = bx - ax
    vy = by - ay
    wx = px - ax
    wy = py - ay
    length_sq = vx * vx + vy * vy
    if length_sq <= 0.0:
        return math.hypot(px - ax, py - ay)
    t = clamp((wx * vx + wy * vy) / length_sq)
    return math.hypot(px - (ax + vx * t), py - (ay + vy * t))


def ring_alpha(distance: float, radius: float, width: float, aa: float) -> float:
    return 1.0 - smoothstep(width * 0.5, width * 0.5 + aa, abs(distance - radius))


def line_alpha(distance: float, width: float, aa: float) -> float:
    return 1.0 - smoothstep(width * 0.5, width * 0.5 + aa, distance)


def render_pixel(x: float, y: float, size: int) -> tuple[int, int, int, int]:
    n = float(size)
    aa = max(1.0, n / 384.0)
    center = n * 0.5

    dist = rounded_rect_distance(x, y, center, center, n * 0.465, n * 0.185)
    bg_alpha = 1.0 - smoothstep(-aa, aa, dist)
    if bg_alpha <= 0.0:
        return (0, 0, 0, 0)

    vertical = y / n
    radial = clamp(math.hypot(x - center, y - center) / (n * 0.68))
    bg = mix(CHARCOAL, GRAPHITE, vertical * 0.55)
    bg = mix(bg, STEEL_BLUE, (1.0 - radial) * 0.45)
    pixel = (bg[0] / 255.0, bg[1] / 255.0, bg[2] / 255.0, bg_alpha)

    highlight = 1.0 - smoothstep(0.0, n * 0.55, math.hypot(x - n * 0.45, y - n * 0.18))
    pixel = over(pixel, (255, 255, 255), highlight * 0.08 * bg_alpha)

    dish_cx = center
    dish_cy = n * 0.49
    dish_distance = math.hypot(x - dish_cx, y - dish_cy)
    for radius, width, alpha in (
        (n * 0.305, n * 0.035, 0.92),
        (n * 0.218, n * 0.030, 0.82),
        (n * 0.132, n * 0.026, 0.72),
    ):
        a = ring_alpha(dish_distance, radius, width, aa) * alpha * bg_alpha
        ring_color = mix(STEEL_BLUE_LIGHT, (255, 255, 255), 0.12)
        pixel = over(pixel, ring_color, a)

    ax = n * 0.355
    ay = n * 0.735
    bx = n * 0.625
    by = n * 0.395
    shadow_distance = segment_distance(x, y, ax + n * 0.012, ay + n * 0.014, bx + n * 0.012, by + n * 0.014)
    pixel = over(pixel, (0, 0, 0), line_alpha(shadow_distance, n * 0.064, aa) * 0.22 * bg_alpha)

    arm_distance = segment_distance(x, y, ax, ay, bx, by)
    arm_color = mix(AMBER, AMBER_LIGHT, clamp((0.72 * n - y) / (0.45 * n)))
    pixel = over(pixel, arm_color, line_alpha(arm_distance, n * 0.052, aa) * 0.96 * bg_alpha)

    dot_distance = math.hypot(x - dish_cx, y - dish_cy)
    pixel = over(pixel, (0, 0, 0), line_alpha(dot_distance, n * 0.088, aa) * 0.18 * bg_alpha)
    pixel = over(pixel, AMBER_LIGHT, line_alpha(dot_distance, n * 0.064, aa) * 0.98 * bg_alpha)

    return tuple(int(round(clamp(channel) * 255.0)) for channel in pixel)


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def write_png(path: Path, size: int) -> None:
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            rows.extend(render_pixel(x + 0.5, y + 0.5, size))

    compressed = zlib.compress(bytes(rows), level=9)
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", compressed)
        + png_chunk(b"IEND", b"")
    )


def generate_iconset(iconset: Path) -> None:
    if iconset.exists():
        shutil.rmtree(iconset)
    iconset.mkdir(parents=True)
    for base_size, scale, filename in ICON_SPECS:
        write_png(iconset / filename, base_size * scale)


def generate_iconset_from_source(source: Path, iconset: Path) -> None:
    if iconset.exists():
        shutil.rmtree(iconset)
    iconset.mkdir(parents=True)
    for base_size, scale, filename in ICON_SPECS:
        px = base_size * scale
        subprocess.run(
            ["sips", "-z", str(px), str(px), str(source), "--out", str(iconset / filename)],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iconset", required=True, type=Path, help="Output .iconset directory")
    parser.add_argument("--icns", type=Path, help="Optional output .icns path")
    parser.add_argument("--source", type=Path, help="Real branding PNG to resize instead of the procedural placeholder")
    args = parser.parse_args()

    if args.source is not None:
        generate_iconset_from_source(args.source, args.iconset)
    else:
        generate_iconset(args.iconset)

    if args.icns is not None:
        args.icns.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["iconutil", "-c", "icns", "-o", str(args.icns), str(args.iconset)],
            check=True,
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
