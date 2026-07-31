#!/usr/bin/env python3
"""Exercise native GL, native WebGPU, and WebGPU's window fallback to GL.

This is intentionally more than a boot/survival check:

* GL and WebGPU each drive the same deterministic route into an actual race.
* Their menu/level transition traces must be identical.
* Sampled PPMs must contain real scenes and stay within a measured pixel-delta
  budget. One transition-boundary sample may differ because native WebGPU
  readback completes one presented frame later than ``glReadPixels``.
* A consecutive GL boot capture must never alternate a completed frame with an
  undefined black back buffer on VI presents that submit no new graphics task.
* A fault-injected WebGPU-window failure must change the cached backend before
  ``main_pc.c`` selects the renderer vtable, then GL must render a non-flat
  menu frame.

The visual comparator has an in-process positive control: the last race frame is
compared with a black raster and the test fails if that deliberately bad pair
would pass the parity threshold.

Always runs muted, hidden, and with a finite headless frame budget.
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


DEFAULT_SCRIPT = Path("tests/input_scripts/nav_to_time_trial_race.txt")
DEFAULT_FRAMES = 3200
SAMPLE_EVERY = 400
FALLBACK_FRAMES = 1200
CAPTURE_FROM = 15
CAPTURE_FRAMES = 230

MIN_SCENE_COLOURS = 200
MIN_SCENE_SIGMA = 10.0
MIN_STABLE_PAIRS = 5
MAX_TRANSITION_MISMATCHES = 1
MAX_FRAME_MAD = 4.0
MAX_MEAN_MAD = 2.0
MIN_RACE_SAMPLE_FRAME = 2800

FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion"
)
ROUTE_RE = re.compile(
    r"^\[TRACE\] (?:menu_init|level_load):.*(?:@frame~\d+)$", re.MULTILINE
)
RACE_RE = re.compile(r"level_load: levelId=5 numPlayers=0\b")
FRAME_RE = re.compile(r"^frame_(\d+)\.ppm$")
DISPLAY_SIZE_RE = re.compile(
    r"^\[DISPLAY\] output=(\d+)x(\d+) render=(\d+)x(\d+) scale=([\d.]+)"
    r" effectiveScale=([\d.]+)$",
    re.MULTILINE,
)


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class Run:
    backend: str
    output: str
    frame_dir: Path


def read_ppm(path: Path) -> Image:
    """Read the binary P6 subset emitted by ``--dump-frames``."""

    data = path.read_bytes()
    index = 0
    fields: list[bytes] = []
    while len(fields) < 4:
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if index >= len(data):
            raise ValueError(f"{path}: truncated PPM header")
        if data[index:index + 1] == b"#":
            newline = data.find(b"\n", index)
            if newline < 0:
                raise ValueError(f"{path}: unterminated PPM comment")
            index = newline + 1
            continue
        end = index
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        fields.append(data[index:end])
        index = end

    if fields[0] != b"P6":
        raise ValueError(f"{path}: expected P6, got {fields[0]!r}")
    width, height, maximum = map(int, fields[1:])
    if width <= 0 or height <= 0 or maximum != 255:
        raise ValueError(
            f"{path}: invalid dimensions/range {width}x{height}, max={maximum}"
        )
    if index >= len(data) or not data[index:index + 1].isspace():
        raise ValueError(f"{path}: missing raster separator")
    # The emitter writes one newline. Accept CRLF without treating LF as pixel 0.
    if data[index:index + 2] == b"\r\n":
        index += 2
    else:
        index += 1
    expected = width * height * 3
    pixels = data[index:]
    if len(pixels) != expected:
        raise ValueError(
            f"{path}: raster is {len(pixels)} bytes, expected {expected}"
        )
    return Image(width, height, pixels)


def scene_metrics(image: Image) -> tuple[int, float]:
    """Return quantized colour count and luma sigma over the scene centre."""

    x0, x1 = int(image.width * 0.15), int(image.width * 0.85)
    y0, y1 = int(image.height * 0.20), int(image.height * 0.95)
    colours: set[tuple[int, int, int]] = set()
    count = total = total_sq = 0
    for y in range(y0, y1, 3):
        row = y * image.width * 3
        for x in range(x0, x1, 3):
            offset = row + x * 3
            r, g, b = image.pixels[offset:offset + 3]
            colours.add((r >> 3, g >> 3, b >> 3))
            luma = (r * 299 + g * 587 + b * 114) // 1000
            count += 1
            total += luma
            total_sq += luma * luma
    if count == 0:
        return 0, 0.0
    mean = total / count
    variance = max(0.0, total_sq / count - mean * mean)
    return len(colours), variance ** 0.5


def mean_absolute_difference(left: Image, right: Image) -> float:
    if (left.width, left.height) != (right.width, right.height):
        raise ValueError(
            f"dimension mismatch: {left.width}x{left.height} vs "
            f"{right.width}x{right.height}"
        )
    return sum(abs(a - b) for a, b in zip(left.pixels, right.pixels)) / len(
        left.pixels
    )


def clean_environment(**updates: str) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("MDKR")
    }
    env.update(updates)
    return env


def run_engine(
    binary: Path,
    rom: Path,
    script: Path | None,
    backend: str,
    frames: int,
    frame_dir: Path,
    run_dir: Path,
    timeout: int,
    *,
    force_window_failure: bool = False,
    dump_every: int = SAMPLE_EVERY,
    dump_from: int | None = None,
    window_size: str = "640x480",
    verbose: bool = False,
) -> Run:
    frame_dir.mkdir(parents=True)
    run_dir.mkdir(parents=True)
    env = clean_environment(
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_DUMP_EVERY=str(dump_every),
    )
    if dump_from is not None:
        env["MDKR_DUMP_FROM"] = str(dump_from)
    if force_window_failure:
        env["MDKR_TEST_WEBGPU_WINDOW_FAIL"] = "1"
    cmd = [
        str(binary),
        "--headless-frames",
        str(frames),
        "--dump-frames",
        str(frame_dir),
        "--window-size",
        window_size,
        "--rom",
        str(rom),
    ]
    if script is not None:
        cmd[3:3] = ["--input-script", str(script)]
    if verbose:
        print("$ " + " ".join(cmd))
    try:
        proc = subprocess.run(
            cmd,
            cwd=run_dir,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or b"").decode("utf-8", "replace")
        raise RuntimeError(
            f"{backend} arm timed out after {timeout}s\n{output[-4000:]}"
        ) from exc
    output = proc.stdout.decode("utf-8", "replace")
    failures: list[str] = []
    if proc.returncode != 0:
        failures.append(f"exited {proc.returncode}")
    fatal = FATAL_RE.search(output)
    if fatal:
        failures.append(f"emitted {fatal.group(0)!r}")
    if failures:
        raise RuntimeError(
            f"{backend} arm {'; '.join(failures)}\n{output[-4000:]}"
        )
    return Run(backend, output, frame_dir)


def frame_paths(run: Run) -> dict[int, Path]:
    frames: dict[int, Path] = {}
    for path in run.frame_dir.iterdir():
        match = FRAME_RE.match(path.name)
        if match:
            frames[int(match.group(1))] = path
    return frames


def require_backend(run: Run, expected: str, failures: list[str]) -> None:
    for marker in (
        f"[mdkr64] renderer backend: {expected}",
        f"[TRACE] gfx_init({expected}) done;",
    ):
        if marker not in run.output:
            failures.append(f"{run.backend} arm is missing {marker!r}")


def reported_display_size(
    run: Run, failures: list[str]
) -> tuple[int, int, int, int, float, float] | None:
    rows = DISPLAY_SIZE_RE.findall(run.output)
    if not rows:
        failures.append(f"{run.backend} arm emitted no output/render size contract")
        return None
    ow, oh, rw, rh, requested_scale, effective_scale = rows[-1]
    result = (
        int(ow),
        int(oh),
        int(rw),
        int(rh),
        float(requested_scale),
        float(effective_scale),
    )
    actual_scale_w = result[2] / result[0]
    actual_scale_h = result[3] / result[1]
    if (
        abs(actual_scale_w - result[5]) > 0.011
        or abs(actual_scale_h - result[5]) > 0.011
        or result[5] < 1.0
        or result[5] > result[4]
    ):
        failures.append(
            f"{run.backend} arm broke output/render scale contract: "
            f"output={result[0]}x{result[1]} render={result[2]}x{result[3]} "
            f"requested={result[4]:.2f} effective={result[5]:.2f}"
        )
    return result


def check_parity(gl_run: Run, webgpu_run: Run, verbose: bool) -> list[str]:
    failures: list[str] = []
    require_backend(gl_run, "gl", failures)
    require_backend(webgpu_run, "webgpu", failures)
    gl_display = reported_display_size(gl_run, failures)
    webgpu_display = reported_display_size(webgpu_run, failures)
    if gl_display is not None and webgpu_display is not None:
        if gl_display[:2] != webgpu_display[:2]:
            failures.append(
                "backend output dimensions diverged for the same 640x480 "
                f"window: GL={gl_display[0]}x{gl_display[1]}, "
                f"WebGPU={webgpu_display[0]}x{webgpu_display[1]}"
            )

    gl_route = ROUTE_RE.findall(gl_run.output)
    webgpu_route = ROUTE_RE.findall(webgpu_run.output)
    if not gl_route or not webgpu_route:
        failures.append(
            "one or both backend arms emitted no menu/level transition trace"
        )
    elif gl_route != webgpu_route:
        failures.append(
            "GL and WebGPU followed different menu/level transition traces:\n"
            f"    GL: {gl_route}\n"
            f"    WebGPU: {webgpu_route}"
        )
    if not RACE_RE.search(gl_run.output) or not RACE_RE.search(webgpu_run.output):
        failures.append("one or both backend arms never entered the playable race")

    gl_paths = frame_paths(gl_run)
    webgpu_paths = frame_paths(webgpu_run)
    if sorted(gl_paths) != sorted(webgpu_paths):
        failures.append(
            "backend arms dumped different frame sets: "
            f"GL={sorted(gl_paths)}, WebGPU={sorted(webgpu_paths)}"
        )
        return failures
    if not gl_paths:
        failures.append("backend arms dumped no frames")
        return failures

    stable: list[tuple[int, float]] = []
    transition_mismatches: list[int] = []
    stable_race_frames: list[int] = []
    images: dict[tuple[str, int], Image] = {}
    for frame in sorted(gl_paths):
        try:
            gl_image = read_ppm(gl_paths[frame])
            webgpu_image = read_ppm(webgpu_paths[frame])
            images[("gl", frame)] = gl_image
            images[("webgpu", frame)] = webgpu_image
            if gl_display is not None and (
                gl_image.width, gl_image.height
            ) != gl_display[:2]:
                failures.append(
                    f"frame {frame}: GL capture {gl_image.width}x{gl_image.height} "
                    f"does not match output {gl_display[0]}x{gl_display[1]}"
                )
            if webgpu_display is not None and (
                webgpu_image.width, webgpu_image.height
            ) != webgpu_display[:2]:
                failures.append(
                    f"frame {frame}: WebGPU capture "
                    f"{webgpu_image.width}x{webgpu_image.height} does not match "
                    f"output {webgpu_display[0]}x{webgpu_display[1]}"
                )
            gl_colours, gl_sigma = scene_metrics(gl_image)
            webgpu_colours, webgpu_sigma = scene_metrics(webgpu_image)
            gl_is_scene = (
                gl_colours >= MIN_SCENE_COLOURS
                and gl_sigma >= MIN_SCENE_SIGMA
            )
            webgpu_is_scene = (
                webgpu_colours >= MIN_SCENE_COLOURS
                and webgpu_sigma >= MIN_SCENE_SIGMA
            )
            mad = mean_absolute_difference(gl_image, webgpu_image)
        except ValueError as exc:
            failures.append(str(exc))
            continue
        if gl_is_scene and webgpu_is_scene:
            stable.append((frame, mad))
            if frame >= MIN_RACE_SAMPLE_FRAME:
                stable_race_frames.append(frame)
            if mad > MAX_FRAME_MAD:
                failures.append(
                    f"frame {frame}: GL/WebGPU MAD {mad:.3f} exceeds "
                    f"{MAX_FRAME_MAD:.3f}"
                )
        elif gl_is_scene != webgpu_is_scene:
            transition_mismatches.append(frame)
        if verbose:
            print(
                f"  frame {frame:4d}: GL colours={gl_colours:4d} "
                f"sigma={gl_sigma:5.1f}; WebGPU colours={webgpu_colours:4d} "
                f"sigma={webgpu_sigma:5.1f}; MAD={mad:.3f}"
            )

    if len(stable) < MIN_STABLE_PAIRS:
        failures.append(
            f"only {len(stable)} stable scene pairs (want >= {MIN_STABLE_PAIRS})"
        )
    if len(transition_mismatches) > MAX_TRANSITION_MISMATCHES:
        failures.append(
            f"{len(transition_mismatches)} one-sided transition samples "
            f"{transition_mismatches} (allow {MAX_TRANSITION_MISMATCHES})"
        )
    if not stable_race_frames:
        failures.append(
            f"no stable paired race scene at or after frame {MIN_RACE_SAMPLE_FRAME}"
        )
    if stable:
        average_mad = sum(mad for _, mad in stable) / len(stable)
        if average_mad > MAX_MEAN_MAD:
            failures.append(
                f"mean GL/WebGPU MAD {average_mad:.3f} exceeds "
                f"{MAX_MEAN_MAD:.3f}"
            )
    else:
        average_mad = float("nan")

    # Both-direction proof for the comparator itself. A real late race raster
    # must be far enough from black that this intentionally bad pair is rejected.
    if stable_race_frames:
        control_frame = max(stable_race_frames)
        control_image = images[("gl", control_frame)]
        black = Image(
            control_image.width,
            control_image.height,
            bytes(len(control_image.pixels)),
        )
        control_mad = mean_absolute_difference(control_image, black)
        if control_mad <= MAX_FRAME_MAD:
            failures.append(
                f"positive control is ineffective: race-vs-black MAD "
                f"{control_mad:.3f} would pass the parity threshold"
            )
    else:
        control_frame = -1
        control_mad = float("nan")

    if verbose and stable:
        print(
            f"  stablePairs={len(stable)} meanMAD={average_mad:.3f}; "
            f"transitionMismatches={transition_mismatches}; "
            f"control frame {control_frame} vs black MAD={control_mad:.3f}"
        )
    return failures


def check_fallback(run: Run) -> list[str]:
    failures: list[str] = []
    for marker in (
        "[SDL] WebGPU window failure forced for fallback test.",
        "forced by MDKR_TEST_WEBGPU_WINDOW_FAIL",
        "[mdkr64] renderer backend: gl",
        "[TRACE] gfx_init(gl) done;",
    ):
        if marker not in run.output:
            failures.append(f"fallback arm is missing {marker!r}")
    if "[TRACE] gfx_init(webgpu)" in run.output:
        failures.append("fallback arm initialized WebGPU after selecting GL")

    good_frames: list[int] = []
    for frame, path in sorted(frame_paths(run).items()):
        try:
            colours, sigma = scene_metrics(read_ppm(path))
        except ValueError as exc:
            failures.append(str(exc))
            continue
        if colours >= MIN_SCENE_COLOURS and sigma >= MIN_SCENE_SIGMA:
            good_frames.append(frame)
    if not good_frames:
        failures.append("fallback GL arm produced no non-flat sampled frame")
    return failures


def isolated_black_frames(values: list[tuple[int, int]]) -> list[int]:
    return [
        middle[0]
        for left, middle, right in zip(values, values[1:], values[2:])
        if left[1] > 0 and middle[1] == 0 and right[1] > 0
    ]


def check_consecutive_gl_capture(run: Run) -> list[str]:
    failures: list[str] = []
    paths = frame_paths(run)
    expected = list(range(CAPTURE_FROM, CAPTURE_FRAMES))
    if sorted(paths) != expected:
        failures.append(
            "consecutive GL capture omitted or invented presents: "
            f"got {len(paths)} frames spanning "
            f"{min(paths, default=-1)}..{max(paths, default=-1)}, "
            f"expected {len(expected)} frames spanning "
            f"{expected[0]}..{expected[-1]}"
        )
        return failures

    luminance: list[tuple[int, int]] = []
    for frame in expected:
        try:
            image = read_ppm(paths[frame])
        except ValueError as exc:
            failures.append(str(exc))
            continue
        luminance.append((frame, sum(image.pixels)))
    if not any(value > 0 for _, value in luminance):
        failures.append("consecutive GL capture never produced a visible frame")
    artifacts = isolated_black_frames(luminance)
    if artifacts:
        failures.append(
            "completed GL frames alternated with undefined black buffers at "
            f"presents {artifacts}"
        )

    # Both-direction control for the detector: the historical A/B/A shape must
    # be rejected even when the two visible values differ.
    if isolated_black_frames([(1, 100), (2, 0), (3, 90)]) != [2]:
        failures.append("isolated-black positive control did not detect A/B/A")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build",
        default="build",
        help="build directory or mdkr64 executable (default: build)",
    )
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--script", default=str(DEFAULT_SCRIPT))
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument(
        "--keep-frames",
        help="copy sampled PPMs and logs into this directory",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    script = Path(args.script).expanduser().resolve()
    for label, path in (
        ("binary", binary),
        ("ROM", rom),
        ("input script", script),
    ):
        if not path.is_file():
            print(
                f"check_renderer_backends: FAIL — missing {label}: {path}",
                file=sys.stderr,
            )
            return 1
    if args.frames < MIN_RACE_SAMPLE_FRAME + 1:
        print(
            "check_renderer_backends: FAIL — --frames must reach at least "
            f"{MIN_RACE_SAMPLE_FRAME + 1}",
            file=sys.stderr,
        )
        return 1

    root = Path(tempfile.mkdtemp(prefix="mdkr_renderers_"))
    failures: list[str] = []
    runs: list[Run] = []
    try:
        try:
            gl_run = run_engine(
                binary,
                rom,
                script,
                "gl",
                args.frames,
                root / "frames-gl",
                root / "run-gl",
                args.timeout,
                verbose=args.verbose,
            )
            runs.append(gl_run)
            webgpu_run = run_engine(
                binary,
                rom,
                script,
                "webgpu",
                args.frames,
                root / "frames-webgpu",
                root / "run-webgpu",
                args.timeout,
                verbose=args.verbose,
            )
            runs.append(webgpu_run)
            failures.extend(check_parity(gl_run, webgpu_run, args.verbose))

            fallback_run = run_engine(
                binary,
                rom,
                None,
                "webgpu",
                FALLBACK_FRAMES,
                root / "frames-fallback",
                root / "run-fallback",
                args.timeout,
                force_window_failure=True,
                verbose=args.verbose,
            )
            runs.append(fallback_run)
            failures.extend(check_fallback(fallback_run))

            capture_run = run_engine(
                binary,
                rom,
                None,
                "gl",
                CAPTURE_FRAMES,
                root / "frames-gl-consecutive",
                root / "run-gl-consecutive",
                args.timeout,
                dump_every=1,
                dump_from=CAPTURE_FROM,
                window_size="320x240",
                verbose=args.verbose,
            )
            runs.append(capture_run)
            failures.extend(check_consecutive_gl_capture(capture_run))
        except RuntimeError as exc:
            failures.append(str(exc))

        for run in runs:
            (root / f"{run.backend}-{run.frame_dir.name}.log").write_text(
                run.output, encoding="utf-8"
            )

        if args.keep_frames:
            destination = Path(args.keep_frames).expanduser().resolve()
            if destination.exists():
                print(
                    "check_renderer_backends: FAIL — --keep-frames destination "
                    f"already exists: {destination}",
                    file=sys.stderr,
                )
                return 1
            shutil.copytree(root, destination)
            if args.verbose:
                print(f"  kept artifacts in {destination}")
    finally:
        shutil.rmtree(root, ignore_errors=True)

    if failures:
        print("check_renderer_backends: FAIL", file=sys.stderr)
        for failure in failures:
            print("  - " + failure, file=sys.stderr)
        return 1

    print(
        "check_renderer_backends: PASS — GL and WebGPU reached the same race "
        "within pixel budgets; forced window failure rendered through GL; "
        "consecutive GL captures retained completed frames"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
