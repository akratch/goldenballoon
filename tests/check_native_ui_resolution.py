#!/usr/bin/env python3
"""Prove UI-3 renders terminal HUD/text at physical output resolution.

The gate runs one deterministic Remastered race on GL and WebGPU with the
production path and with ``MDKR_UI_NATIVE_RES=0``. It requires:

* the production arm to cross the scene->output boundary repeatedly;
* zero world-after-overlay ordering violations and zero begin failures;
* byte-identical simulation and byte-identical world pixels across the A/B;
* all changed pixels to remain in the authored top HUD or minimap regions;
* materially higher glyph/HUD edge energy in the native-resolution arm.

The disabled arm is the positive control: it is the historical path where the
whole supersampled frame, including UI, is downsampled together.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "race_drive_long.txt"
CAPTURE_FRAME = 3000
FRAMES = 3050
FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion|\[FX BUG\]|Validation Error"
)
UI_RE = re.compile(
    r"\[UI-3\] frame=(\d+) active=(\d+) draws=(\d+) lateWorld=(\d+) "
    r"primitives=(\d+) lastWorld=(\d+) .* beginFailures=(\d+)"
)


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class Arm:
    backend: str
    enabled: bool
    image: Image
    pace: tuple[str, ...]
    ui_rows: tuple[tuple[int, ...], ...]
    output: str


def read_ppm(path: Path) -> Image:
    data = path.read_bytes()
    match = re.match(br"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if match is None:
        raise ValueError(f"{path}: malformed P6 PPM")
    width, height = int(match.group(1)), int(match.group(2))
    pixels = data[match.end():]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: truncated raster")
    return Image(width, height, pixels)


def normalized_pace(output: str) -> tuple[str, ...]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker >= 0:
            rows.append(re.sub(r" dtms=\S+", " dtms=<wall>", line[marker:]))
    return tuple(rows)


def environment(backend: str, enabled: bool, save_dir: Path) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_UI_NATIVE_RES="1" if enabled else "0",
        MDKR_UI_OVERLAY_TRACE="1",
        MDKR_DUMP_FROM=str(CAPTURE_FRAME),
        MDKR_DUMP_EVERY="999",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(save_dir),
        LC_ALL="C",
    )
    return env


def run_arm(
    binary: Path,
    rom: Path,
    backend: str,
    enabled: bool,
    work: Path,
    timeout: int,
    verbose: bool,
) -> Arm:
    label = f"{backend}-{'native' if enabled else 'scaled-control'}"
    run_dir = work / label
    dump_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    dump_dir.mkdir(parents=True)
    save_dir.mkdir()
    command = [
        str(binary),
        "--headless-frames", str(FRAMES),
        "--window-size", "960x540",
        "--input-script", str(SCRIPT),
        "--dump-frames", str(dump_dir),
        "--rom", str(rom),
        "--remastered",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=environment(backend, enabled, save_dir),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"{label}: timed out\n{(exc.stdout or '')[-4000:]}"
        ) from exc

    output = proc.stdout
    fatal = FATAL_RE.search(output)
    dumps = sorted(dump_dir.glob("*.ppm"))
    ui_rows = tuple(
        tuple(int(value) for value in match.groups())
        for match in UI_RE.finditer(output)
    )
    if (proc.returncode != 0 or fatal is not None or len(dumps) != 1 or
            dumps[0].name != f"frame_{CAPTURE_FRAME:04d}.ppm" or
            not ui_rows):
        raise RuntimeError(
            f"{label}: exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}, "
            f"dumps={[path.name for path in dumps]}, uiRows={len(ui_rows)}\n"
            f"{output[-5000:]}"
        )
    if "scale=2.00" not in output:
        raise RuntimeError(f"{label}: Remastered arm did not use 2x rendering")
    pace = normalized_pace(output)
    if not pace:
        raise RuntimeError(f"{label}: no [PACE] stream")
    return Arm(
        backend=backend,
        enabled=enabled,
        image=read_ppm(dumps[0]),
        pace=pace,
        ui_rows=ui_rows,
        output=output,
    )


def inside(x: int, y: int, box: tuple[int, int, int, int]) -> bool:
    return box[0] <= x < box[2] and box[1] <= y < box[3]


def changed_pixels(left: Image, right: Image) -> list[tuple[int, int]]:
    if (left.width, left.height) != (right.width, right.height):
        raise ValueError("A/B dimensions differ")
    changed: list[tuple[int, int]] = []
    for pixel in range(left.width * left.height):
        offset = pixel * 3
        if left.pixels[offset:offset + 3] != right.pixels[offset:offset + 3]:
            changed.append((pixel % left.width, pixel // left.width))
    return changed


def region_equal(
    left: Image, right: Image, box: tuple[int, int, int, int]
) -> bool:
    x0, y0, x1, y1 = box
    for y in range(y0, y1):
        start = (y * left.width + x0) * 3
        end = (y * left.width + x1) * 3
        if left.pixels[start:end] != right.pixels[start:end]:
            return False
    return True


def luminance(image: Image, x: int, y: int) -> int:
    offset = (y * image.width + x) * 3
    r, g, b = image.pixels[offset:offset + 3]
    return (54 * r + 183 * g + 19 * b) // 256


def laplacian_energy(
    image: Image, box: tuple[int, int, int, int]
) -> int:
    x0, y0, x1, y1 = box
    energy = 0
    for y in range(y0 + 1, y1 - 1):
        for x in range(x0 + 1, x1 - 1):
            center = luminance(image, x, y)
            energy += abs(
                4 * center
                - luminance(image, x - 1, y)
                - luminance(image, x + 1, y)
                - luminance(image, x, y - 1)
                - luminance(image, x, y + 1)
            )
    return energy


def scaled_box(
    image: Image, x0: float, y0: float, x1: float, y1: float
) -> tuple[int, int, int, int]:
    return (
        round(image.width * x0),
        round(image.height * y0),
        round(image.width * x1),
        round(image.height * y1),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument(
        "--renderer",
        choices=("gl", "webgpu"),
        action="append",
        help="limit iteration; default proves both shipped native backends",
    )
    parser.add_argument("--keep-frames")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    backends = tuple(dict.fromkeys(args.renderer or ("gl", "webgpu")))
    failures: list[str] = []
    summaries: list[str] = []

    try:
        with tempfile.TemporaryDirectory(prefix="mdkr_native_ui_") as temp:
            work = Path(temp)
            for backend in backends:
                on = run_arm(
                    binary, rom, backend, True, work,
                    args.timeout, args.verbose,
                )
                off = run_arm(
                    binary, rom, backend, False, work,
                    args.timeout, args.verbose,
                )

                if on.pace != off.pace:
                    failures.append(f"{backend}: A/B [PACE] streams differ")

                active_rows = [row for row in on.ui_rows if row[1] == 1]
                late_rows = [row for row in on.ui_rows if row[3] != 0]
                begin_fail_rows = [row for row in on.ui_rows if row[6] != 0]
                if len(active_rows) < 500:
                    failures.append(
                        f"{backend}: only {len(active_rows)} output-overlay frames"
                    )
                if late_rows:
                    failures.append(
                        f"{backend}: {len(late_rows)} world-after-overlay frame(s)"
                    )
                if begin_fail_rows:
                    failures.append(
                        f"{backend}: output-overlay begin failure telemetry"
                    )
                if any(row[1] != 0 for row in off.ui_rows):
                    failures.append(
                        f"{backend}: disabled positive-control arm still activated"
                    )

                image = on.image
                if image.width / image.height < 1.7:
                    failures.append(
                        f"{backend}: expected a widescreen output, got "
                        f"{image.width}x{image.height}"
                    )
                changed = changed_pixels(on.image, off.image)
                if len(changed) < max(1000, image.width * image.height // 200):
                    failures.append(
                        f"{backend}: positive control changed only "
                        f"{len(changed)} pixels"
                    )

                safe_left = round((image.width - image.height * 4 / 3) / 2)
                safe_right = image.width - safe_left
                top_hud = (safe_left, 0, safe_right, round(image.height * 0.23))
                minimap = (
                    round(image.width * 0.62),
                    round(image.height * 0.60),
                    safe_right,
                    image.height,
                )
                escaped = [
                    (x, y) for x, y in changed
                    if not inside(x, y, top_hud)
                    and not inside(x, y, minimap)
                ]
                if escaped:
                    failures.append(
                        f"{backend}: {len(escaped)} changed pixel(s) escaped "
                        f"the terminal HUD/minimap regions; first={escaped[0]}"
                    )

                world_center = scaled_box(image, 0.0, 0.23, 1.0, 0.63)
                world_bottom = scaled_box(image, 0.0, 0.63, 0.62, 1.0)
                if not region_equal(on.image, off.image, world_center):
                    failures.append(
                        f"{backend}: center-world pixels changed across UI A/B"
                    )
                if not region_equal(on.image, off.image, world_bottom):
                    failures.append(
                        f"{backend}: bottom-world pixels changed across UI A/B"
                    )

                text_roi = scaled_box(image, 0.20, 0.04, 0.84, 0.22)
                native_energy = laplacian_energy(on.image, text_roi)
                scaled_energy = laplacian_energy(off.image, text_roi)
                ratio = (
                    native_energy / scaled_energy
                    if scaled_energy > 0 else 0.0
                )
                if ratio < 1.15:
                    failures.append(
                        f"{backend}: native HUD edge-energy ratio {ratio:.3f} "
                        f"is below 1.15 positive-control threshold"
                    )
                summaries.append(
                    f"{backend} active={len(active_rows)} "
                    f"changed={len(changed)} edge={ratio:.3f}x"
                )

            if args.keep_frames:
                destination = Path(args.keep_frames).expanduser().resolve()
                if destination.exists():
                    raise ValueError(
                        f"--keep-frames destination already exists: {destination}"
                    )
                shutil.copytree(work, destination)
                print(f"artifacts: {destination}")
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"check_native_ui_resolution: FAIL — {exc}", file=sys.stderr)
        return 1

    if failures:
        print("check_native_ui_resolution: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(
        "check_native_ui_resolution: PASS — "
        + "; ".join(summaries)
        + "; world pixels exact and ordering clean"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
