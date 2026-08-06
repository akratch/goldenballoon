#!/usr/bin/env python3
"""Consecutive-frame visual A/B for the projected-shadow depth fix.

The ordinary widescreen/shadow gate proves game-side flags, final decoded depth
state, heap bounds and simulation equivalence. This companion check proves the
pixel effect over a moving-camera interval:

  * run the same closed-loop race with production decal bias and with the
    MDKR_SHADOW_DECAL=0 positive control;
  * require byte-identical normalized gameplay-state streams;
  * compare 300 consecutive sampled frames (3300..3898 by default);
  * require a mix of identical and different frames, showing that the old
    coplanar coverage loss appears and disappears as the camera moves;
  * require every affected production pixel to be component-wise darker than
    the old route. The fix may restore shadow coverage, but it may not alter
    geometry, brighten pixels, or perturb the rest of the image.

Both arms run in --pure. The strict "only darker" invariant assumes shadow
coverage is the only thing differing between them, so texture filtering has to
be held at the reference presentation -- Remastered's mipmapping and anisotropy
flip pixels brighter and the check fails for reasons that have nothing to do
with shadows.

No golden image and no third-party image library are required. The engine emits
P6 PPM files and this script compares their RGB rasters directly.

Usage:
    python3 tests/check_shadow_visual_ab.py \
        --build build-ws-webgpu \
        --rom baserom.us.v80.z64 \
        --renderer webgpu -v
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, read_ppm, resolve_binary


REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"

SHADOW_RE = re.compile(
    r"\[SHADOW\] decal=(on|off).*drawGroups=(\d+) nonDecal=(\d+)"
)
DEPTH_RE = re.compile(
    r"\[DEPTH\] decalTriangles=(\d+) comparedTriangles=(\d+)"
)

# Every pixel verdict here is a comparison between the two arms, and two frames
# that drew nothing agree perfectly. These are check_renderer_backends' scene
# floors; the weakest frame in the sampled 3300..3898 window carries 453
# colours and sigma 24.8, so the floor is a blank-frame guard and not a
# content threshold.
MIN_SCENE_COLOURS = 200
MIN_SCENE_SIGMA = 10.0


@dataclass
class RunResult:
    label: str
    returncode: int
    output: str
    pace: list[str]
    shadow: tuple[str, int, int] | None
    depth: tuple[int, int] | None


def normalized_pace(output: str) -> list[str]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker >= 0:
            rows.append(re.sub(r" dtms=\S+", " dtms=<wall>", line[marker:]))
    return rows


def final_match(pattern: re.Pattern[str], output: str) -> re.Match[str] | None:
    matches = list(pattern.finditer(output))
    return matches[-1] if matches else None


def clean_environment(renderer: str | None) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("MDKR")
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_NO_CRASH_HANDLER="1",
        MDKR64_HIDDEN="1",
        LC_ALL="C",
    )
    if renderer:
        env["MDKR_RENDERER"] = renderer
    return env


def run_arm(
    binary: Path,
    rom: Path,
    frames: int,
    dump_from: int,
    dump_every: int,
    window_size: str,
    label: str,
    frame_dir: Path,
    save_dir: Path,
    base_env: dict[str, str],
    decal_enabled: bool,
    timeout: int,
    verbose: bool,
) -> RunResult:
    env = dict(base_env)
    env["MDKR_DUMP_FROM"] = str(dump_from)
    env["MDKR_DUMP_EVERY"] = str(dump_every)
    if not decal_enabled:
        env["MDKR_SHADOW_DECAL"] = "0"
    command = [
        str(binary),
        "--headless-frames",
        str(frames),
        "--window-size",
        window_size,
        "--input-script",
        str(SCRIPT),
        "--dump-frames",
        str(frame_dir),
        "--rom",
        str(rom),
        # Pin the presentation mode. This check asserts that every pixel the
        # decal fix touches gets DARKER and that nothing else moves -- an
        # invariant that only holds when shadow coverage is the sole difference
        # between the two arms. Remastered's mipmaps and 16x anisotropy change
        # texture filtering, and that was measured
        # flipping 2 RGB components brighter. Filtering is presentation; this is
        # a fidelity check, so it runs against the reference presentation for
        # the same reason the oracle does.
        "--pure",
    ]
    if verbose:
        print(f"$ ({label}) " + shlex.join(command), flush=True)
    frame_dir.mkdir()
    save_dir.mkdir()
    try:
        proc = subprocess.run(
            command,
            cwd=save_dir,
            env=env,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        output = proc.stdout + proc.stderr
        returncode = proc.returncode
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") + (exc.stderr or "")
        returncode = 124

    shadow_match = final_match(SHADOW_RE, output)
    depth_match = final_match(DEPTH_RE, output)
    shadow = (
        (
            shadow_match.group(1),
            int(shadow_match.group(2)),
            int(shadow_match.group(3)),
        )
        if shadow_match
        else None
    )
    depth = (
        (int(depth_match.group(1)), int(depth_match.group(2)))
        if depth_match
        else None
    )
    return RunResult(
        label, returncode, output, normalized_pace(output), shadow, depth
    )


def scene_metrics(width: int, height: int, rgb: bytes) -> tuple[int, float]:
    """Return quantized colour count and luma sigma over the scene centre."""
    colours: set[tuple[int, int, int]] = set()
    count = total = total_sq = 0
    for y in range(int(height * 0.20), int(height * 0.95), 3):
        row = y * width * 3
        for x in range(int(width * 0.15), int(width * 0.85), 3):
            offset = row + x * 3
            red, green, blue = rgb[offset : offset + 3]
            colours.add((red >> 3, green >> 3, blue >> 3))
            luma = (red * 299 + green * 587 + blue * 114) // 1000
            count += 1
            total += luma
            total_sq += luma * luma
    if count == 0:
        return 0, 0.0
    mean = total / count
    return len(colours), max(0.0, total_sq / count - mean * mean) ** 0.5


def frame_number(path: Path) -> int:
    return int(path.stem.split("_", 1)[1])


def summarize_runs(frame_numbers: list[int], step: int) -> str:
    if not frame_numbers:
        return "none"
    ranges: list[str] = []
    start = previous = frame_numbers[0]
    for number in frame_numbers[1:]:
        if number != previous + step:
            ranges.append(str(start) if start == previous else f"{start}-{previous}")
            start = number
        previous = number
    ranges.append(str(start) if start == previous else f"{start}-{previous}")
    return ", ".join(ranges)


def print_context(result: RunResult) -> None:
    print(f"\n--- {result.label}: last 50 output lines ---", file=sys.stderr)
    for line in result.output.splitlines()[-50:]:
        print(line, file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--renderer", choices=("gl", "webgpu"), default=None)
    parser.add_argument("--frames", type=int, default=3900)
    parser.add_argument("--dump-from", type=int, default=3300)
    parser.add_argument("--dump-every", type=int, default=2)
    parser.add_argument("--window-size", default="160x120")
    parser.add_argument("--timeout", type=int, default=240, help="seconds per arm")
    parser.add_argument(
        "--keep-frames",
        metavar="DIR",
        help="retain decal-on/decal-off PPMs in an empty directory",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom)
    if not rom.is_absolute():
        rom = (REPO / rom).resolve()
    missing = [path for path in (binary, rom, SCRIPT) if not path.is_file()]
    if missing:
        for path in missing:
            print(f"FAIL: missing {path}", file=sys.stderr)
        return 1
    if (
        args.dump_from < 0
        or args.dump_every < 1
        or args.frames <= args.dump_from
        or not re.fullmatch(r"[1-9]\d*x[1-9]\d*", args.window_size)
    ):
        print(
            "FAIL: require frames > dump-from >= 0, dump-every >= 1, "
            "and window-size WIDTHxHEIGHT",
            file=sys.stderr,
        )
        return 1

    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.keep_frames:
        root = Path(args.keep_frames).resolve()
        root.mkdir(parents=True, exist_ok=True)
        if any(root.iterdir()):
            print(f"FAIL: --keep-frames directory is not empty: {root}", file=sys.stderr)
            return 1
    else:
        temporary = tempfile.TemporaryDirectory(prefix="mdkr_shadow_visual_ab_")
        root = Path(temporary.name)

    on_frames = root / "decal-on"
    off_frames = root / "decal-off"
    base_env = clean_environment(args.renderer)
    on = run_arm(
        binary,
        rom,
        args.frames,
        args.dump_from,
        args.dump_every,
        args.window_size,
        "decal on",
        on_frames,
        root / "save-on",
        base_env,
        True,
        args.timeout,
        args.verbose,
    )
    off = run_arm(
        binary,
        rom,
        args.frames,
        args.dump_from,
        args.dump_every,
        args.window_size,
        "decal off positive control",
        off_frames,
        root / "save-off",
        base_env,
        False,
        args.timeout,
        args.verbose,
    )

    failures: list[str] = []
    for result in (on, off):
        if result.returncode != 0:
            failures.append(f"{result.label}: exit code {result.returncode}")
        for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer"):
            if marker in result.output:
                failures.append(f"{result.label}: output contains {marker}")
        if not result.pace:
            failures.append(f"{result.label}: no [PACE] state rows")
        if result.shadow is None:
            failures.append(f"{result.label}: no parseable [SHADOW] report")
        if result.depth is None:
            failures.append(f"{result.label}: no parseable [DEPTH] report")

    if on.pace != off.pace:
        failures.append("normalized gameplay state differs between visual A/B arms")
    if on.shadow:
        if on.shadow[0] != "on" or on.shadow[1] <= 0 or on.shadow[2] != 0:
            failures.append(f"decal on: unexpected shadow report {on.shadow}")
    if off.shadow:
        if off.shadow[0] != "off" or off.shadow[1] <= 0 or off.shadow[2] <= 0:
            failures.append(f"decal off: positive control was not exercised {off.shadow}")
    if on.depth and off.depth:
        if not 0 <= on.depth[0] <= on.depth[1]:
            failures.append(f"decal on: invalid final depth counts {on.depth}")
        if not 0 <= off.depth[0] <= off.depth[1]:
            failures.append(f"decal off: invalid final depth counts {off.depth}")
        if on.depth[0] <= off.depth[0]:
            failures.append(
                f"final ZMODE_DEC count did not decrease: {on.depth[0]} -> {off.depth[0]}"
            )

    on_paths = {path.name: path for path in on_frames.glob("frame_*.ppm")}
    off_paths = {path.name: path for path in off_frames.glob("frame_*.ppm")}
    if set(on_paths) != set(off_paths):
        failures.append(
            f"dumped frame sets differ: on={len(on_paths)} off={len(off_paths)}"
        )
    expected_frames = (args.frames - 1 - args.dump_from) // args.dump_every + 1
    if len(on_paths) != expected_frames:
        failures.append(
            f"dumped {len(on_paths)} comparable frames, expected {expected_frames}"
        )

    changed_frames: list[int] = []
    changed_pixels = 0
    brighter_components = 0
    total_pixels = 0
    largest = ("", 0)
    weakest_colours = ("", 1 << 30)
    weakest_sigma = ("", float("inf"))
    try:
        for name in sorted(set(on_paths) & set(off_paths)):
            on_width, on_height, on_rgb = read_ppm(on_paths[name])
            off_width, off_height, off_rgb = read_ppm(off_paths[name])
            if (on_width, on_height) != (off_width, off_height):
                failures.append(
                    f"{name}: dimensions differ "
                    f"{on_width}x{on_height} vs {off_width}x{off_height}"
                )
                continue
            total_pixels += on_width * on_height
            colours, sigma = scene_metrics(on_width, on_height, on_rgb)
            if colours < weakest_colours[1]:
                weakest_colours = (name, colours)
            if sigma < weakest_sigma[1]:
                weakest_sigma = (name, sigma)
            frame_changed = 0
            for offset in range(0, len(on_rgb), 3):
                production = on_rgb[offset : offset + 3]
                control = off_rgb[offset : offset + 3]
                if production == control:
                    continue
                frame_changed += 1
                changed_pixels += 1
                brighter_components += sum(
                    production[channel] > control[channel] for channel in range(3)
                )
            if frame_changed:
                number = frame_number(on_paths[name])
                changed_frames.append(number)
                if frame_changed > largest[1]:
                    largest = (name, frame_changed)
    except (OSError, ValueError) as exc:
        failures.append(str(exc))

    if weakest_colours[1] < MIN_SCENE_COLOURS:
        failures.append(
            f"{weakest_colours[0]}: production capture is blank or "
            f"near-uniform ({weakest_colours[1]} colours); the A/B compared "
            "frames with no scene in them"
        )
    if weakest_sigma[1] < MIN_SCENE_SIGMA:
        failures.append(
            f"{weakest_sigma[0]}: production capture has no tonal spread "
            f"(luma sigma {weakest_sigma[1]:.1f})"
        )
    if not changed_frames:
        failures.append("visual positive control changed no frames")
    elif len(changed_frames) == len(on_paths):
        failures.append(
            "every frame changed; expected intermittent coplanar-coverage loss"
        )
    if brighter_components:
        failures.append(
            f"{brighter_components} RGB components became brighter with shadow bias"
        )
    if total_pixels > 0 and changed_pixels * 100 >= total_pixels:
        failures.append(
            f"visual delta is not localized: {changed_pixels}/{total_pixels} pixels"
        )

    if failures:
        print("FAIL: consecutive-frame shadow visual A/B", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print_context(on)
        print_context(off)
        if temporary is not None:
            temporary.cleanup()
        return 1

    assert on.shadow is not None and on.depth is not None and off.depth is not None
    print(
        "PASS: gameplay state is identical; "
        f"{len(changed_frames)}/{len(on_paths)} moving-camera frames restore "
        f"{changed_pixels} dark shadow pixels; no other pixel brightens; "
        f"final ZMODE_DEC triangles {off.depth[0]} -> {on.depth[0]}"
    )
    if args.verbose:
        print(
            f"  affected frame runs: {summarize_runs(changed_frames, args.dump_every)}"
        )
        print(f"  largest localized delta: {largest[0]} ({largest[1]} pixels)")
        print(f"  production shadow draw groups: {on.shadow[1]}")
    if args.keep_frames:
        print(f"  retained frames: {root}")
    if temporary is not None:
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
