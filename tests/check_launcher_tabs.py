#!/usr/bin/env python3
"""Validate the launcher's intermediate-width top navigation.

The wide sidebar and narrow dropdown have separate coverage. This gate owns the
800x600 layout between them: exactly one destination may use the brand-cobalt
selection fill, and that destination must carry the gold active indicator.
"""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path

from check_app_capture import CaptureError, load_bmp, validate


def near(color: tuple[int, int, int], target: tuple[int, int, int],
         tolerance: int = 8) -> bool:
    return all(abs(actual - expected) <= tolerance
               for actual, expected in zip(color, target))


def primary_action_taper(bitmap) -> tuple[int, int, int]:
    """Prove the gold primary action owns its own bottom edge.

    A population count cannot see a clipped button: losing the bottom five
    points of a 48-point control costs about a tenth of its pixels, well inside
    the headroom any single ratio threshold needs to cover both capture scales
    this gate runs at. The SHAPE is decisive instead. The action is drawn with a
    corner radius, so its last row is markedly narrower than its widest one; a
    control cut off by its container's edge ends on a full-width row.
    """
    width, height = bitmap.width, bitmap.height
    gold = [near(color, (212, 168, 67)) for color in bitmap.pixels]
    seen = bytearray(len(gold))
    best: list[int] = []
    for start, present in enumerate(gold):
        if not present or seen[start]:
            continue
        queue = deque([start])
        seen[start] = 1
        component = []
        while queue:
            index = queue.popleft()
            component.append(index)
            y, x = divmod(index, width)
            for neighbor, ny, nx in (
                (index - 1, y, x - 1), (index + 1, y, x + 1),
                (index - width, y - 1, x), (index + width, y + 1, x),
            ):
                if (0 <= nx < width and 0 <= ny < height and
                        not seen[neighbor] and gold[neighbor]):
                    seen[neighbor] = 1
                    queue.append(neighbor)
        if len(component) > len(best):
            best = component
    if not best:
        raise CaptureError("no gold primary action was found")
    rows: dict[int, int] = {}
    for index in best:
        rows[index // width] = rows.get(index // width, 0) + 1
    widest = max(rows.values())
    last = rows[max(rows)]
    return len(best), widest, last


def selected_components(path: Path) -> tuple[int, int, int, int, int, int, int]:
    bitmap = load_bmp(path)
    general_failures = validate(bitmap)
    if general_failures:
        raise CaptureError("; ".join(general_failures))

    width, height = bitmap.width, bitmap.height
    # Prove each wordmark half independently inside the header. The global
    # gold population cannot stand in for “Golden” because the primary CTA is
    # deliberately gold too.
    wordmark_x1 = int(width * 0.30)
    wordmark_y1 = int(height * 0.075)
    wordmark = [
        bitmap.pixels[y * width + x]
        for y in range(wordmark_y1)
        for x in range(wordmark_x1)
    ]
    wordmark_gold_pixels = sum(
        near(color, (212, 168, 67)) for color in wordmark)
    wordmark_sky_pixels = sum(
        near(color, (113, 190, 249)) for color in wordmark)
    gold_pixels = sum(
        near(color, (212, 168, 67)) for color in bitmap.pixels)
    wordmark_minimum = max(100, len(wordmark) // 100)
    if wordmark_gold_pixels < wordmark_minimum:
        raise CaptureError("gold ‘Golden’ wordmark half is missing")
    if wordmark_sky_pixels < wordmark_minimum:
        raise CaptureError("sky-blue ‘Balloon’ wordmark half is missing")
    if gold_pixels < len(bitmap.pixels) // 80:
        raise CaptureError(
            "solid-gold primary action is missing or too small")
    action_pixels, action_widest, action_last = primary_action_taper(bitmap)
    # The corner radius removes roughly two radii from the final row (18 points
    # at 1.00x/2x capture, 10 at 0.75x). A clipped control ends on exactly its
    # widest row, so any real taper separates the two cases.
    if action_widest - action_last < 4:
        raise CaptureError(
            "gold primary action ends on a full-width row: its rounded bottom "
            f"edge is clipped by the header ({action_last} of {action_widest} "
            "points on the last row)")

    # BrandRule is alpha-blended over the header surface. Find a row containing
    # both blended colors across most of the window, then require many direct
    # color transitions so two solid half-lines cannot impersonate the quiet
    # checkered gantry motif.
    rule_found = False
    for y in range(int(height * 0.07), int(height * 0.15)):
        classified: list[int] = []
        for x in range(width):
            color = bitmap.pixels[y * width + x]
            if near(color, (161, 129, 57)):
                classified.append(1)
            elif near(color, (43, 74, 118)):
                classified.append(2)
            else:
                classified.append(0)
        colored = sum(value != 0 for value in classified)
        transitions = sum(
            classified[x] != 0 and classified[x - 1] != 0 and
            classified[x] != classified[x - 1]
            for x in range(1, width)
        )
        if colored >= int(width * 0.70) and transitions >= 50:
            rule_found = True
            break
    if not rule_found:
        raise CaptureError("alternating gold/cobalt brand rule is missing")

    # Ratios make the gate independent of Retina capture scale. The crop owns
    # only the four navigation destinations: it excludes the brand, Quit,
    # status, Play action, and panel content.
    x0, x1 = int(width * 0.025), int(width * 0.66)
    # Touch-sized tabs extend slightly farther down at 1.00x. Keep their gold
    # baseline inside the crop while still ending above the status/CTA row.
    y0, y1 = int(height * 0.09), int(height * 0.27)
    crop_width, crop_height = x1 - x0, y1 - y0
    blue = bytearray(crop_width * crop_height)
    gold = bytearray(crop_width * crop_height)
    for y in range(y0, y1):
        source = y * width
        target = (y - y0) * crop_width
        for x in range(x0, x1):
            color = bitmap.pixels[source + x]
            blue[target + x - x0] = near(color, (49, 92, 152))
            gold[target + x - x0] = near(color, (212, 168, 67))

    seen = bytearray(len(blue))
    components: list[tuple[int, int, int, int, int]] = []
    for start, present in enumerate(blue):
        if not present or seen[start]:
            continue
        queue = deque([start])
        seen[start] = 1
        area = 0
        min_x = min_y = 1 << 30
        max_x = max_y = -1
        while queue:
            index = queue.popleft()
            y, x = divmod(index, crop_width)
            area += 1
            min_x, max_x = min(min_x, x), max(max_x, x)
            min_y, max_y = min(min_y, y), max(max_y, y)
            for neighbor in (index - 1, index + 1,
                             index - crop_width, index + crop_width):
                if neighbor < 0 or neighbor >= len(blue) or seen[neighbor]:
                    continue
                ny, nx = divmod(neighbor, crop_width)
                if abs(nx - x) + abs(ny - y) != 1 or not blue[neighbor]:
                    continue
                seen[neighbor] = 1
                queue.append(neighbor)
        # Ignore isolated antialiasing pixels; a selected destination is a
        # solid control surface occupying thousands of capture pixels.
        if area >= max(200, (width * height) // 4000):
            components.append((area, min_x, min_y, max_x, max_y))

    if len(components) != 1:
        raise CaptureError(
            "top navigation must contain exactly one cobalt selected surface; "
            f"found {len(components)}")

    area, min_x, min_y, max_x, max_y = components[0]
    if max_x - min_x < width * 0.06 or max_y - min_y < height * 0.025:
        raise CaptureError("selected tab surface is clipped or too small")

    indicator = 0
    indicator_y0 = max(0, max_y - int(height * 0.012))
    indicator_y1 = min(crop_height, max_y + int(height * 0.008) + 1)
    for y in range(indicator_y0, indicator_y1):
        row = y * crop_width
        for x in range(min_x, max_x + 1):
            indicator += bool(gold[row + x])
    if indicator < max(20, width // 20):
        raise CaptureError("selected tab is missing its gold active indicator")

    return (area, indicator, len(components), wordmark_gold_pixels,
            wordmark_sky_pixels, gold_pixels, action_widest - action_last)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bmp", type=Path)
    args = parser.parse_args()
    try:
        (area, indicator, components, wordmark_gold_pixels,
         wordmark_sky_pixels, gold_pixels,
         action_taper) = selected_components(args.bmp)
    except (OSError, CaptureError) as error:
        print(f"launcher top-tab validation failed: {error}")
        return 1
    print("launcher top tabs valid: "
          f"selected_components={components} selected_pixels={area} "
          f"indicator_pixels={indicator} "
          f"wordmark_gold_pixels={wordmark_gold_pixels} "
          f"wordmark_sky_pixels={wordmark_sky_pixels} "
          f"gold_action_pixels={gold_pixels} "
          f"gold_action_bottom_taper={action_taper}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
