#!/usr/bin/env python3
"""Prove UI-3 renders terminal HUD/text at physical output resolution.

The gate runs one deterministic Remastered race on GL and WebGPU with the
production path and with ``MDKR_UI_NATIVE_RES=0``. It requires:

* the production arm to cross the scene->output boundary repeatedly;
* zero world-after-overlay ordering violations and zero begin failures;
* byte-identical simulation and byte-identical world pixels across the A/B;
* all changed pixels to remain in the authored top HUD or minimap regions;
* materially higher glyph/HUD edge energy in the native-resolution arm.
* opaque HUD pixels to remain byte-exact when Remastered's world finish is
  toggled, at both 1x and 2x render scale.

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

from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, fatal_re,
                           FX_MARKERS, read_ppm as read_ppm_bytes,
                           resolve_binary, VALIDATION_MARKERS)


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "race_drive_long.txt"
CAPTURE_FRAME = 3000
FRAMES = 3050
# Runtime WebGPU admission is deliberately nonblocking, so a headless loop can
# outrun the GPU and legitimately elide most presentation opportunities. The
# capture path forces a bounded pre-roll before frame 3000; eight successful
# output-overlay passes prove repeated boundary use without reintroducing a
# hardware-throughput-dependent quota.
MIN_OUTPUT_OVERLAY_FRAMES = 8
FATAL_RE = fatal_re(*ASSERT_MARKERS, *FX_MARKERS, *VALIDATION_MARKERS)
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
    finish_enabled: bool
    image: Image
    pace: tuple[str, ...]
    ui_rows: tuple[tuple[int, ...], ...]
    output: str


def read_ppm(path: Path) -> Image:
    return Image(*read_ppm_bytes(path))


def normalized_pace(output: str) -> tuple[str, ...]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker >= 0:
            rows.append(re.sub(r" dtms=\S+", " dtms=<wall>", line[marker:]))
    return tuple(rows)


def environment(
    backend: str, enabled: bool, render_scale: int, finish_enabled: bool,
    save_dir: Path,
) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_RENDER_SCALE=str(render_scale),
        MDKR_UI_NATIVE_RES="1" if enabled else "0",
        MDKR_UI_OVERLAY_TRACE="1",
        MDKR_DUMP_FROM=str(CAPTURE_FRAME),
        MDKR_DUMP_EVERY="999",
        MDKR_FRAME_DUMP_TRACE="1",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(save_dir),
        # This gate compares world pixels byte-for-byte across five separate
        # webgpu processes. WebGPU's nonblocking frame admission skips frames
        # as a function of live GPU occupancy, and the dither seed advances
        # per ADMITTED frame -- so under host load two arms land on different
        # seeds and scattered world pixels differ. Full admission pins the
        # admitted-frame count, exactly as the five sibling pixel gates do.
        # It does not weaken the assertion; it stops grading host load.
        # (2026-08-10: one in-suite red with this exact signature passed
        # standalone on a quiet machine.)
        MDKR_TEST_RENDER_FULL_ADMISSION="1",
        LC_ALL="C",
    )
    if not finish_enabled:
        env["MDKR_TEST_WORLD_FINISH_OFF"] = "1"
    return env


def run_arm(
    binary: Path,
    rom: Path,
    backend: str,
    enabled: bool,
    render_scale: int,
    finish_enabled: bool,
    work: Path,
    timeout: int,
    verbose: bool,
) -> Arm:
    label = (
        f"{backend}-{render_scale}x-"
        f"{'native' if enabled else 'scaled-control'}"
        f"-finish-{'on' if finish_enabled else 'off'}"
    )
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
            env=environment(
                backend, enabled, render_scale, finish_enabled, save_dir
            ),
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
    if f"scale={render_scale:.2f}" not in output:
        raise RuntimeError(
            f"{label}: Remastered arm did not use {render_scale}x rendering"
        )
    pace = normalized_pace(output)
    if not pace:
        raise RuntimeError(f"{label}: no [PACE] stream")
    if verbose:
        for line in output.splitlines():
            if "[FRAME-DUMP]" in line:
                print(f"  {label}: {line}", flush=True)
    return Arm(
        backend=backend,
        enabled=enabled,
        finish_enabled=finish_enabled,
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


def opaque_yellow_hud_mask(images: tuple[Image, ...]) -> list[int]:
    """Select stable opaque banana/time glyph cores in every A/B image."""

    if not images:
        return []
    first = images[0]
    if any(
        (image.width, image.height) != (first.width, first.height)
        for image in images[1:]
    ):
        raise ValueError("world-finish A/B dimensions differ")

    # Normalized from the authored 640x480 banana/time HUD region. Requiring the
    # colour predicate in all four images makes the mask symmetric: neither arm
    # gets to choose pixels that only it happens to preserve.
    authored_width = first.height * 4 / 3
    authored_left = (first.width - authored_width) / 2
    x0 = round(authored_left + authored_width * 290 / 640)
    x1 = round(authored_left + authored_width * 600 / 640)
    y0 = round(first.height * 25 / 480)
    y1 = round(first.height * 105 / 480)
    selected: list[int] = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            pixel = y * first.width + x
            offset = pixel * 3
            if all(
                image.pixels[offset] >= 245
                and image.pixels[offset + 1] >= 175
                and image.pixels[offset + 2] <= 10
                for image in images
            ):
                selected.append(pixel)
    return selected


def masked_changed_pixels(left: Image, right: Image, mask: list[int]) -> int:
    return sum(
        left.pixels[pixel * 3:pixel * 3 + 3]
        != right.pixels[pixel * 3:pixel * 3 + 3]
        for pixel in mask
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
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
                    binary, rom, backend, True, 2, True, work,
                    args.timeout, args.verbose,
                )
                off = run_arm(
                    binary, rom, backend, False, 2, True, work,
                    args.timeout, args.verbose,
                )
                one_x = run_arm(
                    binary, rom, backend, True, 1, True, work,
                    args.timeout, args.verbose,
                )
                finish_off_2x = run_arm(
                    binary, rom, backend, True, 2, False, work,
                    args.timeout, args.verbose,
                )
                finish_off_1x = run_arm(
                    binary, rom, backend, True, 1, False, work,
                    args.timeout, args.verbose,
                )

                if any(
                    arm.pace != on.pace
                    for arm in (off, one_x, finish_off_2x, finish_off_1x)
                ):
                    failures.append(
                        f"{backend}: UI/scale arms changed the [PACE] stream"
                    )

                active_rows = [row for row in on.ui_rows if row[1] == 1]
                late_rows = [row for row in on.ui_rows if row[3] != 0]
                begin_fail_rows = [row for row in on.ui_rows if row[6] != 0]
                if len(active_rows) < MIN_OUTPUT_OVERLAY_FRAMES:
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
                one_x_active = [row for row in one_x.ui_rows if row[1] == 1]
                one_x_late = [row for row in one_x.ui_rows if row[3] != 0]
                one_x_fail = [row for row in one_x.ui_rows if row[6] != 0]
                if len(one_x_active) < MIN_OUTPUT_OVERLAY_FRAMES:
                    failures.append(
                        f"{backend}: only {len(one_x_active)} output-overlay "
                        "frames at Remastered 1x"
                    )
                if one_x_late:
                    failures.append(
                        f"{backend}: {len(one_x_late)} world-after-overlay "
                        "frame(s) at Remastered 1x"
                    )
                if one_x_fail:
                    failures.append(
                        f"{backend}: output-overlay begin failure at "
                        "Remastered 1x"
                    )
                if (
                    one_x.image.width != on.image.width or
                    one_x.image.height != on.image.height or
                    finish_off_1x.image.width != on.image.width or
                    finish_off_1x.image.height != on.image.height or
                    finish_off_2x.image.width != on.image.width or
                    finish_off_2x.image.height != on.image.height
                ):
                    failures.append(
                        f"{backend}: 1x/2x output dimensions differ"
                    )

                finish_mask = opaque_yellow_hud_mask((
                    finish_off_1x.image,
                    one_x.image,
                    finish_off_2x.image,
                    on.image,
                ))
                if len(finish_mask) < 200:
                    failures.append(
                        f"{backend}: world-finish HUD mask selected only "
                        f"{len(finish_mask)} stable pixels"
                    )
                for scale, finish_off, finish_on in (
                    (1, finish_off_1x, one_x),
                    (2, finish_off_2x, on),
                ):
                    changed_hud = masked_changed_pixels(
                        finish_off.image, finish_on.image, finish_mask
                    )
                    if changed_hud != 0:
                        failures.append(
                            f"{backend}: Remastered world finish changed "
                            f"{changed_hud}/{len(finish_mask)} opaque HUD "
                            f"pixels at {scale}x"
                        )
                    world_changed = len(changed_pixels(
                        finish_off.image, finish_on.image
                    ))
                    if world_changed < max(
                        1000,
                        finish_on.image.width * finish_on.image.height // 100,
                    ):
                        failures.append(
                            f"{backend}: world-finish {scale}x positive "
                            f"control changed only {world_changed} pixels"
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
                # The authored first-place digit reaches y=113 in the 480-line
                # HUD canvas. Keep the boundary just below it; 0.23 stopped at
                # y=110 and misclassified the digit's bottom edge as world.
                top_hud_bottom = round(image.height * 0.24)
                top_hud = (safe_left, 0, safe_right, top_hud_bottom)
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

                world_center = (
                    0, top_hud_bottom, image.width, round(image.height * 0.63)
                )
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
                    f"1xActive={len(one_x_active)} changed={len(changed)} "
                    f"edge={ratio:.3f}x finishSafe={len(finish_mask)}"
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
