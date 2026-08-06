#!/usr/bin/env python3
"""Keep each Adventure door's balloon numeral bound to that door.

The four ordinary Dino Domain race doors share one cached ObjectModel but carry
different requirements: Ancient Lake 1, Fossil Canyon 2, Jungle Falls 3, and
Hot Top Volcano 5.  A 1.0.1 refactor selected the numeral during a
view-dependent fixed-tick traversal by writing ``ObjectModel.batches[].texOffset``.
The last admitted door therefore supplied its digit to every other door, and a
camera move could change all visible signs without changing game state.

This check drives a real new Adventure save into the Dino Domain lobby on both
native renderers.  Opt-in display-list telemetry records the immutable door
requirement, shared model identity, shared stored offset, and the offset chosen
for that submitted door.  The gate requires:

* all four authored door/count pairs, stable for the complete capture;
* one frame where all four shared-model doors are submitted together;
* a per-door chosen offset equal to ``balloon_count * 4``; and
* at least one mismatch between the shared stored offset and the required
  per-door offset, the counterfactual control proving that shared state would
  reproduce the old defect; and
* final GL/WebGPU door-face agreement in Original presentation mode, which detects an
  OpenGL texture-object sampler omission that repeated numerals across the wood.

No pixels or ROM-derived assets are committed.  The route, save, and trace are
temporary and the run is muted/headless.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "tests", "input_scripts", "adventure_race_loop.txt")
TICKS = 7000
LOBBY_LEVEL = 12

# Stop after entering the lobby.  The unconsumed race-menu inputs in the source
# script are harmless, while the drive hook leaves the kart on the exact camera
# path that exposed the defect at frames 6820 and 6880.
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12;12"
)

# doorID -> authored balloon requirement for the four race doors in level 12.
EXPECTED_DOORS = {2: 1, 1: 2, 3: 3, 6: 5}
EXPECTED_SIXTEEN_OFFSETS = {8: 24, 9: 0}
PIXEL_FRAME = 6820
DOOR_CROP = (130, 285, 980, 430)
GLYPH_BOXES = (
    (209, 328, 273, 392),
    (545, 328, 609, 392),
    (856, 328, 920, 392),
)
DOOR_FACE_BOXES = (
    (155, 295, 307, 420),
    (508, 298, 650, 419),
    (821, 291, 965, 419),
)
MAX_DOOR_CROP_MAD = 0.5
MAX_DOOR_CROP_LARGE_DIFF = 0.005
MIN_DOOR_COLOURS = 200
MIN_DOOR_LUMA_SIGMA = 10.0
GLYPH_EDGE_THRESHOLD = 20
MIN_GLYPH_PAIR_EDGE_DIFFERENCE = 0.15

LEVEL_RE = re.compile(r"level_load: levelId=(\d+).*@frame~(\d+)")
DOOR_RE = re.compile(
    r"\[DOOR-TEX\] frame=(\d+) object=(-?\d+) model=(\S+) "
    r"door=(-?\d+) balloons=(-?\d+) batch=(\d+) place=(\d+) "
    r"stored=(-?\d+) applied=(-?\d+)"
)


def compare_door_pixels(
    left: tuple[int, int, bytes], right: tuple[int, int, bytes]
) -> tuple[float, float]:
    """Compare the three visible door faces without scoring unrelated pixels."""

    left_width, left_height, left_pixels = left
    right_width, right_height, right_pixels = right
    if (left_width, left_height) != (right_width, right_height):
        raise ValueError(
            f"dimension mismatch: {left_width}x{left_height} vs "
            f"{right_width}x{right_height}"
        )
    if (left_width, left_height) != (1280, 960):
        raise ValueError(
            f"unexpected capture size {left_width}x{left_height}; expected 1280x960"
        )

    x0, y0, x1, y1 = DOOR_CROP
    total = large = difference = 0
    for y in range(y0, y1):
        begin = (y * left_width + x0) * 3
        end = (y * left_width + x1) * 3
        for a, b in zip(left_pixels[begin:end], right_pixels[begin:end]):
            delta = abs(a - b)
            difference += delta
            large += delta >= 24
            total += 1
    return difference / total, large / total


def crop_bytes(image: tuple[int, int, bytes], box: tuple[int, int, int, int]) -> bytes:
    width, _, pixels = image
    x0, y0, x1, y1 = box
    output = bytearray()
    for y in range(y0, y1):
        output.extend(pixels[(y * width + x0) * 3:(y * width + x1) * 3])
    return bytes(output)


def door_content_metrics(image: tuple[int, int, bytes]) -> tuple[int, float]:
    width, _, pixels = image
    x0, y0, x1, y1 = DOOR_CROP
    colours: set[tuple[int, int, int]] = set()
    lumas: list[int] = []
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            offset = (y * width + x) * 3
            r, g, b = pixels[offset:offset + 3]
            colours.add((r >> 3, g >> 3, b >> 3))
            lumas.append((r * 299 + g * 587 + b * 114) // 1000)
    mean = sum(lumas) / len(lumas)
    sigma = (sum((value - mean) ** 2 for value in lumas) / len(lumas)) ** 0.5
    return len(colours), sigma


def glyph_edge_mask(
    image: tuple[int, int, bytes], box: tuple[int, int, int, int]
) -> tuple[bool, ...]:
    """Return a lighting-resistant mask of the glyph's local edges."""

    width, _, pixels = image
    x0, y0, x1, y1 = box
    luma: list[int] = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = (y * width + x) * 3
            r, g, b = pixels[offset:offset + 3]
            luma.append((r * 299 + g * 587 + b * 114) // 1000)

    glyph_width = x1 - x0
    mask: list[bool] = []
    for row in range(y1 - y0):
        for column in range(glyph_width):
            index = row * glyph_width + column
            horizontal = (
                abs(luma[index] - luma[index - 1]) if column > 0 else 0
            )
            vertical = (
                abs(luma[index] - luma[index - glyph_width]) if row > 0 else 0
            )
            mask.append(horizontal + vertical > GLYPH_EDGE_THRESHOLD)
    return tuple(mask)


def glyph_pair_edge_differences(
    image: tuple[int, int, bytes]
) -> tuple[float, float, float]:
    glyphs = [glyph_edge_mask(image, box) for box in GLYPH_BOXES]
    differences = []
    for left, right in ((0, 1), (0, 2), (1, 2)):
        a = glyphs[left]
        b = glyphs[right]
        differences.append(sum(x != y for x, y in zip(a, b)) / len(a))
    return tuple(differences)


def pixel_oracle_failures(
    captures: dict[str, tuple[int, int, bytes]]
) -> tuple[list[str], dict[str, tuple[int, float, tuple[float, float, float]]]]:
    failures: list[str] = []
    metrics: dict[str, tuple[int, float, tuple[float, float, float]]] = {}
    for renderer, image in captures.items():
        width, height, _ = image
        if (width, height) != (1280, 960):
            failures.append(
                f"{renderer}: unexpected capture size {width}x{height}; "
                "expected 1280x960"
            )
            continue
        colours, sigma = door_content_metrics(image)
        pair_differences = glyph_pair_edge_differences(image)
        metrics[renderer] = (colours, sigma, pair_differences)
        if colours < MIN_DOOR_COLOURS or sigma < MIN_DOOR_LUMA_SIGMA:
            failures.append(
                f"{renderer}: door capture lacks live scene content: "
                f"colours={colours} (min {MIN_DOOR_COLOURS}), "
                f"luma sigma={sigma:.2f} (min {MIN_DOOR_LUMA_SIGMA:.2f})"
            )
        if min(pair_differences) < MIN_GLYPH_PAIR_EDGE_DIFFERENCE:
            failures.append(
                f"{renderer}: final pixels do not distinguish the 2/1/3 door "
                "glyph shapes: pair edge differences="
                f"{tuple(round(value, 3) for value in pair_differences)} "
                f"(min {MIN_GLYPH_PAIR_EDGE_DIFFERENCE:.3f})"
            )

    if set(captures) == {"webgpu", "gl"}:
        try:
            mad, large_fraction = compare_door_pixels(
                captures["webgpu"], captures["gl"]
            )
        except ValueError as error:
            failures.append(f"door pixel comparison failed: {error}")
        else:
            if mad > MAX_DOOR_CROP_MAD or large_fraction > MAX_DOOR_CROP_LARGE_DIFF:
                failures.append(
                    "GL/WebGPU door pixels diverged: "
                    f"MAD={mad:.3f} (max {MAX_DOOR_CROP_MAD:.3f}), "
                    f"large-difference fraction={large_fraction:.4f} "
                    f"(max {MAX_DOOR_CROP_LARGE_DIFF:.4f})"
                )
    return failures, metrics


def replace_glyphs_with_first(
    image: tuple[int, int, bytes]
) -> tuple[int, int, bytes]:
    width, height, pixels = image
    output = bytearray(pixels)
    source = crop_bytes(image, GLYPH_BOXES[0])
    glyph_width = GLYPH_BOXES[0][2] - GLYPH_BOXES[0][0]
    row_bytes = glyph_width * 3
    for box in GLYPH_BOXES[1:]:
        x0, y0, _, y1 = box
        for row, y in enumerate(range(y0, y1)):
            begin = (y * width + x0) * 3
            output[begin:begin + row_bytes] = source[
                row * row_bytes:(row + 1) * row_bytes
            ]
    return width, height, bytes(output)


def light_glyphs_independently(
    image: tuple[int, int, bytes]
) -> tuple[int, int, bytes]:
    """Vary common-glyph lighting without changing its edge structure."""

    width, height, pixels = replace_glyphs_with_first(image)
    output = bytearray(pixels)
    for box, delta in zip(GLYPH_BOXES, (0, 24, 48)):
        x0, y0, x1, y1 = box
        for y in range(y0, y1):
            begin = (y * width + x0) * 3
            end = (y * width + x1) * 3
            output[begin:end] = bytes(
                min(value + delta, 255) for value in output[begin:end]
            )
    return width, height, bytes(output)


def stamp_glyph_across_doors(
    image: tuple[int, int, bytes]
) -> tuple[int, int, bytes]:
    width, height, pixels = image
    output = bytearray(pixels)
    source_box = GLYPH_BOXES[0]
    tile_width = tile_height = 16
    source_x, source_y = source_box[0], source_box[1]
    for x0, y0, x1, y1 in DOOR_FACE_BOXES:
        for y in range(y0, y1):
            for x in range(x0, x1):
                source_offset = (
                    (source_y + (y - y0) % tile_height) * width
                    + source_x + (x - x0) % tile_width
                ) * 3
                target_offset = (y * width + x) * 3
                output[target_offset:target_offset + 3] = pixels[
                    source_offset:source_offset + 3
                ]
    return width, height, bytes(output)


def check_pixel_controls(reference: tuple[int, int, bytes]) -> list[str]:
    width, height, _ = reference
    controls = {
        "blank": {"webgpu": (width, height, bytes(width * height * 3))},
        "common-glyph-different-lighting": {
            "webgpu": light_glyphs_independently(reference)
        },
        "sampler-repeat": {
            "webgpu": reference,
            "gl": stamp_glyph_across_doors(reference),
        },
    }
    failures = []
    for name, captures in controls.items():
        control_failures, _ = pixel_oracle_failures(captures)
        if not control_failures:
            failures.append(
                f"pixel oracle accepted its synthetic {name} failing control"
            )
    return failures


def run_renderer(
    binary: str, rom: str, renderer: str, rate: str, verbose: bool
):
    with tempfile.TemporaryDirectory(prefix=f"mdkr-door-glyph-{renderer}-") as run_dir:
        os.mkdir(os.path.join(run_dir, "save"))
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("MDKR_")
        }
        env.update(
            MDKR_AUDIO="0",
            MDKR_RENDERER=renderer,
            MDKR_PRESENT_RATE=rate,
            MDKR_SAVE_DIR=os.path.join(run_dir, "save"),
            MDKR_VIDEO_CONFIG_PATH=os.path.join(run_dir, "mdkr64.ini"),
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
            MDKR_TRACE="1",
            MDKR_DOOR_TEX_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_ADVENTURE_WIN="1",
            MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
        )
        cmd = [
            binary,
            "--headless-ticks", str(TICKS),
            "--input-script", SCRIPT,
            "--rom", rom,
        ]
        frame_dir = Path(run_dir) / "frames"
        if rate == "original":
            frame_dir.mkdir()
            env["MDKR_DUMP_FROM"] = str(PIXEL_FRAME)
            env["MDKR_DUMP_EVERY"] = "999999"
            cmd[3:3] = ["--dump-frames", str(frame_dir)]
        if verbose:
            print("$ " + " ".join(cmd) + f"  # {renderer}, rate={rate}")
        proc = subprocess.run(
            cmd, cwd=run_dir, env=env, capture_output=True, text=True
        )
        capture = None
        capture_error = None
        if rate == "original":
            frame = frame_dir / f"frame_{PIXEL_FRAME}.ppm"
            try:
                capture = read_ppm(frame)
            except (OSError, ValueError) as error:
                capture_error = str(error)
        return proc, proc.stdout + proc.stderr, capture, capture_error


def check_renderer(
    binary: str, rom: str, renderer: str, rate: str, verbose: bool
) -> tuple[list[str], tuple[int, int, bytes] | None]:
    proc, output, capture, capture_error = run_renderer(
        binary, rom, renderer, rate, verbose
    )
    failures: list[str] = []

    if proc.returncode != 0:
        failures.append(f"{renderer}: exit code {proc.returncode}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in output:
            line = next((line for line in output.splitlines() if marker in line), marker)
            failures.append(f"{renderer}: {line.strip()}")
    if capture_error is not None:
        failures.append(f"{renderer}: final-pixel capture failed: {capture_error}")

    lobby_frames = [
        int(match.group(2)) for match in LEVEL_RE.finditer(output)
        if int(match.group(1)) == LOBBY_LEVEL
    ]
    if not lobby_frames:
        failures.append(f"{renderer}: never reached Dino Domain (level {LOBBY_LEVEL})")
        return failures, capture
    lobby_frame = lobby_frames[0]

    rows = []
    for match in DOOR_RE.finditer(output):
        row = {
            "frame": int(match.group(1)),
            "object": int(match.group(2)),
            "model": match.group(3),
            "door": int(match.group(4)),
            "balloons": int(match.group(5)),
            "batch": int(match.group(6)),
            "place": int(match.group(7)),
            "stored": int(match.group(8)),
            "applied": int(match.group(9)),
        }
        rows.append(row)
    lobby_rows = [
        row for row in rows
        if row["frame"] >= lobby_frame and row["door"] in EXPECTED_DOORS
    ]
    if not lobby_rows:
        failures.append(f"{renderer}: no numbered-door display-list records after lobby load")
        return failures, capture

    by_door: dict[int, set[int]] = defaultdict(set)
    models: dict[str, set[int]] = defaultdict(set)
    frames: dict[int, set[int]] = defaultdict(set)
    bad_offsets = []
    stored_control = []
    for row in lobby_rows:
        by_door[row["door"]].add(row["balloons"])
        models[row["model"]].add(row["door"])
        frames[row["frame"]].add(row["door"])
        expected_offset = EXPECTED_DOORS[row["door"]] * 4
        if row["place"] != 1 or row["applied"] != expected_offset:
            bad_offsets.append((row, expected_offset))
        if row["stored"] != row["applied"]:
            stored_control.append((row, expected_offset))

    for door, expected_count in EXPECTED_DOORS.items():
        observed = by_door.get(door, set())
        if observed != {expected_count}:
            failures.append(
                f"{renderer}: door {door} requirements changed or were absent: "
                f"observed {sorted(observed)}, expected [{expected_count}]"
            )

    shared_models = [
        model for model, doors in models.items()
        if set(EXPECTED_DOORS).issubset(doors)
    ]
    if not shared_models:
        detail = {model: sorted(doors) for model, doors in models.items()}
        failures.append(
            f"{renderer}: did not prove all four requirements share one model: {detail}"
        )

    simultaneous = [
        frame for frame, doors in frames.items()
        if set(EXPECTED_DOORS).issubset(doors)
    ]
    if not simultaneous:
        failures.append(
            f"{renderer}: no frame submitted all four differently numbered doors"
        )

    if bad_offsets:
        row, expected = bad_offsets[0]
        failures.append(
            f"{renderer}: door {row['door']} requires {row['balloons']} balloons "
            f"but applied texture offset {row['applied']} for place "
            f"{row['place']} (expected {expected} for ones) at "
            f"frame {row['frame']}; {len(bad_offsets)} bad submissions"
        )

    if not stored_control:
        failures.append(
            f"{renderer}: shared-offset counterfactual did not differ from any "
            "per-door requirement, so the gate cannot detect the 1.0.1 defect"
        )

    sixteen_rows = [
        row for row in rows
        if row["balloons"] == 16 and row["door"] in EXPECTED_SIXTEEN_OFFSETS
    ]
    for door, expected_offset in EXPECTED_SIXTEEN_OFFSETS.items():
        observed = {
            (row["place"], row["applied"])
            for row in sixteen_rows if row["door"] == door
        }
        expected_place = 1 if door == 8 else 10
        if observed != {(expected_place, expected_offset)}:
            failures.append(
                f"{renderer}: 16-balloon door {door} atlas binding was "
                f"{sorted(observed)}, expected "
                f"[({expected_place}, {expected_offset})]"
            )

    if verbose and not failures:
        counts = {door: sorted(values) for door, values in by_door.items()}
        print(
            f"  {renderer}/{rate}: lobby@{lobby_frame}, records={len(lobby_rows)}, "
            f"requirements={counts}, shared_model={shared_models[0]}, "
            f"all-visible@{simultaneous[0]}, "
            f"shared-offset mismatches={len(stored_control)}"
        )
    return failures, capture


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--renderer", choices=("both", "webgpu", "gl"), default="both"
    )
    parser.add_argument(
        "--rate",
        choices=("original", "30", "60", "120", "240", "uncapped"),
        default="original",
        help="presentation policy to exercise (default: original)",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = os.path.abspath(resolve_binary(args.build))
    rom = os.path.abspath(args.rom)
    for path in (binary, rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    renderers = ("webgpu", "gl") if args.renderer == "both" else (args.renderer,)
    failures: list[str] = []
    captures: dict[str, tuple[int, int, bytes]] = {}
    for renderer in renderers:
        renderer_failures, capture = check_renderer(
            binary, rom, renderer, args.rate, args.verbose
        )
        failures.extend(renderer_failures)
        if capture is not None:
            captures[renderer] = capture

    if args.rate == "original" and captures:
        pixel_failures, pixel_metrics = pixel_oracle_failures(captures)
        failures.extend(pixel_failures)
        reference = captures.get("webgpu", next(iter(captures.values())))
        failures.extend(check_pixel_controls(reference))
        if args.verbose and not pixel_failures:
            for renderer, (colours, sigma, pair_differences) in pixel_metrics.items():
                print(
                    f"  {renderer} door pixels: colours={colours}, "
                    f"luma sigma={sigma:.2f}, glyph pair edge differences="
                    f"{tuple(round(value, 3) for value in pair_differences)}"
                )
            if set(captures) == {"webgpu", "gl"}:
                mad, large_fraction = compare_door_pixels(
                    captures["webgpu"], captures["gl"]
                )
                print(
                    "  GL/WebGPU door pixels: "
                    f"MAD={mad:.3f}, large-difference fraction={large_fraction:.4f}"
                )

    if failures:
        print("FAIL: per-door balloon glyph binding")
        for failure in failures:
            print("  - " + failure)
        return 1

    print("PASS: per-door balloon glyph binding")
    print(
        "  Dino Domain's shared race-door model retained independent "
        f"1/2/3/5 requirements at {args.rate} on " + "/".join(renderers)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
