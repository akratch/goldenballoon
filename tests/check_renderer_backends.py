#!/usr/bin/env python3
"""Exercise qualified WebGPU, explicit diagnostic GL, and fail-closed startup.

This is intentionally more than a boot/survival check:

* GL and WebGPU each drive the same deterministic route into an actual race.
* Their menu/level transition traces must be identical.
* Coarsely sampled PPMs must contain real scenes and stay within a global
  pixel-delta budget. This is a route/backend plumbing check, not GL visual
  qualification: dense localized GL parity remains open. One transition sample
  may differ because native WebGPU readback completes one presented frame later
  than ``glReadPixels``.
* A consecutive GL boot capture must never alternate a completed frame with an
  undefined black back buffer on VI presents that submit no new graphics task.
* No selector must resolve to WebGPU and produce byte-identical targeted intro
  samples to an explicit WebGPU run at the guarded frames which exposed GL
  corruption.
* A fault-injected WebGPU-window failure must stop cleanly without changing the
  cached backend or silently entering the unqualified GL diagnostic path.

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

from harness_utils import completed_tick_conservation, resolve_binary


DEFAULT_SCRIPT = Path("tests/input_scripts/nav_to_time_trial_race.txt")
DEFAULT_FRAMES = 3200
SAMPLE_EVERY = 400
CAPTURE_FROM = 15
CAPTURE_FRAMES = 230
DEFAULT_PROBE_FROM = 640
DEFAULT_PROBE_EVERY = 12
DEFAULT_PROBE_FRAMES = 653
AUTHORED_INTRO_FROM = 296
AUTHORED_INTRO_FRAMES = 302
RATE60_INTRO_FROM = AUTHORED_INTRO_FROM * 2
RATE60_INTRO_FRAMES = AUTHORED_INTRO_FRAMES * 2
AUTHORED_VEHICLE_FROM = 2996
AUTHORED_VEHICLE_FRAMES = 3002
RATE60_VEHICLE_FROM = AUTHORED_VEHICLE_FROM * 2
RATE60_VEHICLE_FRAMES = AUTHORED_VEHICLE_FRAMES * 2
MAX_AUTHORED_PAIR_MAD = 0.20
MAX_INTRO_FRAMING_MAD = 1.50

MIN_SCENE_COLOURS = 200
MIN_SCENE_SIGMA = 10.0
MIN_STABLE_PAIRS = 5
MAX_TRANSITION_MISMATCHES = 1
MAX_FRAME_MAD = 4.0
MAX_MEAN_MAD = 2.0
MIN_RACE_SAMPLE_FRAME = 2800

FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion|SIGSEGV|Segmentation fault|SIGABRT|Abort trap"
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
SCHED_RE = re.compile(r"^\[PRESENTSCHED-SUMMARY\] (.*)$", re.MULTILINE)


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
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(updates)
    return env


def run_engine(
    binary: Path,
    rom: Path,
    script: Path | None,
    backend: str | None,
    frames: int,
    frame_dir: Path,
    run_dir: Path,
    timeout: int,
    *,
    dump_every: int = SAMPLE_EVERY,
    dump_from: int | None = None,
    window_size: str = "640x480",
    extra_env: dict[str, str] | None = None,
    verbose: bool = False,
) -> Run:
    frame_dir.mkdir(parents=True)
    run_dir.mkdir(parents=True)
    env = clean_environment(
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_DUMP_EVERY=str(dump_every),
    )
    if backend is not None:
        env["MDKR_RENDERER"] = backend
    if dump_from is not None:
        env["MDKR_DUMP_FROM"] = str(dump_from)
    if extra_env is not None:
        env.update(extra_env)
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
            f"{backend or 'default'} arm timed out after {timeout}s\n"
            f"{output[-4000:]}"
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
            f"{backend or 'default'} arm {'; '.join(failures)}\n"
            f"{output[-4000:]}"
        )
    return Run(backend or "default", output, frame_dir)


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


def parse_last_fields(
    output: str, pattern: re.Pattern[str]
) -> dict[str, int] | None:
    rows = list(pattern.finditer(output))
    if not rows:
        return None
    fields: dict[str, int] = {}
    for token in rows[-1].group(1).split():
        key, separator, value = token.partition("=")
        if not separator:
            continue
        try:
            fields[key] = int(value)
        except ValueError:
            continue
    return fields


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


def check_fail_closed_window(binary: Path, rom: Path, root: Path,
                             timeout: int, verbose: bool) -> list[str]:
    run_dir = root / "run-webgpu-window-failure"
    run_dir.mkdir(parents=True)
    env = clean_environment(
        MDKR_AUDIO="0",
        MDKR_RENDERER="webgpu",
        MDKR_TEST_WEBGPU_WINDOW_FAIL="1",
    )
    command = [
        str(binary), "--headless-frames", "5", "--rom", str(rom),
    ]
    if verbose:
        print("$ " + " ".join(command))
    proc = subprocess.run(
        command, cwd=run_dir, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False,
    )
    output = proc.stdout.decode("utf-8", "replace")
    failures: list[str] = []
    if proc.returncode != 1:
        failures.append(
            "forced WebGPU window failure did not use clean EXIT_FAILURE: "
            f"returncode={proc.returncode}"
        )
    if fatal := FATAL_RE.search(output):
        failures.append(
            f"forced WebGPU window failure emitted {fatal.group(0)!r}"
        )
    for marker in (
        "[SDL] WebGPU window failure forced for fail-closed test.",
        "stopping without an automatic GL fallback",
    ):
        if marker not in output:
            failures.append(f"fail-closed arm is missing {marker!r}")
    for marker in (
        "[SDL] GL ready:",
        "[mdkr64] renderer backend: gl",
        "[TRACE] gfx_init(gl) done;",
    ):
        if marker in output:
            failures.append(f"fail-closed arm silently emitted {marker!r}")
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


def check_default_webgpu(default_run: Run, explicit_run: Run) -> list[str]:
    """Pin both the renderer policy and the targeted intro pixels it selects."""

    failures: list[str] = []
    require_backend(default_run, "webgpu", failures)
    require_backend(explicit_run, "webgpu", failures)
    default_paths = frame_paths(default_run)
    explicit_paths = frame_paths(explicit_run)
    expected = [DEFAULT_PROBE_FROM, DEFAULT_PROBE_FROM + DEFAULT_PROBE_EVERY]
    if sorted(default_paths) != expected or sorted(explicit_paths) != expected:
        failures.append(
            "default-backend probe did not capture the two guarded intro "
            f"frames: default={sorted(default_paths)}, "
            f"explicit={sorted(explicit_paths)}, expected={expected}"
        )
        return failures

    for frame in expected:
        try:
            default_image = read_ppm(default_paths[frame])
            explicit_image = read_ppm(explicit_paths[frame])
        except ValueError as exc:
            failures.append(str(exc))
            continue
        colours, sigma = scene_metrics(default_image)
        if colours < MIN_SCENE_COLOURS or sigma < MIN_SCENE_SIGMA:
            failures.append(
                f"default WebGPU intro frame {frame} is not a real scene: "
                f"colours={colours}, sigma={sigma:.1f}"
            )
        if default_image != explicit_image:
            mad = mean_absolute_difference(default_image, explicit_image)
            failures.append(
                f"default renderer diverged from explicit WebGPU at guarded "
                f"intro frame {frame}: MAD={mad:.6f}"
            )

    # Both-direction proof: byte identity is discriminating for these frames.
    control = read_ppm(default_paths[expected[-1]])
    black = Image(control.width, control.height, bytes(len(control.pixels)))
    if control == black:
        failures.append("default-backend intro positive control was black")
    return failures


def check_first_dump_prearm(cli_run: Run, prearmed_run: Run) -> list[str]:
    """The first CLI dump must already target the final output texture."""

    failures: list[str] = []
    cli_paths = frame_paths(cli_run)
    prearmed_paths = frame_paths(prearmed_run)
    expected = [DEFAULT_PROBE_FROM, DEFAULT_PROBE_FROM + DEFAULT_PROBE_EVERY]
    if sorted(cli_paths) != expected or sorted(prearmed_paths) != expected:
        return [
            "WebGPU first-dump prearm captured the wrong frame set: "
            f"CLI={sorted(cli_paths)}, prearmed={sorted(prearmed_paths)}, "
            f"expected={expected}"
        ]
    for frame in expected:
        try:
            cli_image = read_ppm(cli_paths[frame])
            prearmed_image = read_ppm(prearmed_paths[frame])
        except ValueError as error:
            failures.append(str(error))
            continue
        if cli_image != prearmed_image:
            failures.append(
                "WebGPU CLI dump did not prearm the final output target at "
                f"frame {frame}: MAD="
                f"{mean_absolute_difference(cli_image, prearmed_image):.6f}"
            )
    return failures


def check_authored_rate_sequence(
    original: Run,
    rate60: Run,
    original_from: int,
    original_frames: int,
    rate60_from: int,
    rate60_frames: int,
    maximum_mad: float,
    label: str,
) -> list[str]:
    """Compare every Original image with both =60 images for that tick."""

    failures: list[str] = []
    original_paths = frame_paths(original)
    rate60_paths = frame_paths(rate60)
    expected_original = list(range(original_from, original_frames))
    expected_rate60 = list(range(rate60_from, rate60_frames))
    if sorted(original_paths) != expected_original or \
            sorted(rate60_paths) != expected_rate60:
        return [
            f"{label}: incomplete exact frame sequence: "
            f"Original={sorted(original_paths)}, =60={sorted(rate60_paths)}"
        ]

    original_sched = parse_last_fields(original.output, SCHED_RE)
    rate60_sched = parse_last_fields(rate60.output, SCHED_RE)
    if original_sched is None or rate60_sched is None:
        failures.append(f"{label}: missing scheduler summary")
    else:
        for run_label, sched in (("Original", original_sched),
                                 ("=60", rate60_sched)):
            conservation_error = completed_tick_conservation(
                sched, original_frames, f"{label}: {run_label}")
            if conservation_error:
                failures.append(conservation_error)
        for key, expected in (
            ("ticks", original_frames),
            ("simticks", original_frames),
            ("presents", original_frames),
        ):
            if original_sched.get(key) != expected:
                failures.append(
                    f"{label}: Original {key}={original_sched.get(key)}, "
                    f"expected {expected}"
                )
        for key, expected in (
            ("ticks", original_frames),
            ("simticks", original_frames),
            ("presents", rate60_frames),
        ):
            if rate60_sched.get(key) != expected:
                failures.append(
                    f"{label}: =60 {key}={rate60_sched.get(key)}, "
                    f"expected {expected}"
                )
        for key in ("blocked", "multidue", "lag", "catchup",
                    "skips", "rebases", "updatebad"):
            if original_sched.get(key, 0) != 0 or \
                    rate60_sched.get(key, 0) != 0:
                failures.append(
                    f"{label}: scheduler {key} is nonzero "
                    f"(Original={original_sched.get(key)}, "
                    f"=60={rate60_sched.get(key)})"
                )

    compared = 0
    worst_frame = -1
    worst_mad = 0.0
    for authored_frame in expected_original:
        try:
            authored = read_ppm(original_paths[authored_frame])
        except ValueError as error:
            failures.append(str(error))
            continue
        for present_frame in (authored_frame * 2, authored_frame * 2 + 1):
            if present_frame not in rate60_paths:
                failures.append(
                    f"{label}: missing =60 presentation {present_frame}"
                )
                continue
            try:
                presented = read_ppm(rate60_paths[present_frame])
                mad = mean_absolute_difference(authored, presented)
            except ValueError as error:
                failures.append(str(error))
                continue
            compared += 1
            # Record the first successful comparison even when it is byte
            # identical. Otherwise an ideal all-zero-MAD sequence leaves the
            # sentinel at -1 and is misreported below as "no frame measured".
            if worst_frame < 0 or mad > worst_mad:
                worst_mad = mad
                worst_frame = present_frame
            if mad > maximum_mad:
                failures.append(
                    f"{label}: =60 frame {present_frame} diverged from "
                    f"Original {authored_frame}: MAD={mad:.6f}, "
                    f"limit={maximum_mad:.2f}"
                )
    if compared != len(expected_original) * 2:
        failures.append(
            f"{label}: compared {compared} pairs, expected "
            f"{len(expected_original) * 2}"
        )

    # The threshold must reject a materially missing scene. This is an
    # in-process detector control, independent of the production pair.
    control = read_ppm(original_paths[expected_original[-1]])
    black = Image(control.width, control.height, bytes(len(control.pixels)))
    if mean_absolute_difference(control, black) <= maximum_mad:
        failures.append(
            f"{label}: black-frame positive control would pass the "
            f"MAD limit {maximum_mad:.2f}"
        )
    if not failures and worst_frame < 0:
        failures.append(f"{label}: no frame pair was measured")
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

            failures.extend(check_fail_closed_window(
                binary, rom, root, args.timeout, args.verbose))

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

            default_intro_run = run_engine(
                binary,
                rom,
                None,
                None,
                DEFAULT_PROBE_FRAMES,
                root / "frames-default-intro",
                root / "run-default-intro",
                args.timeout,
                dump_every=DEFAULT_PROBE_EVERY,
                dump_from=DEFAULT_PROBE_FROM,
                verbose=args.verbose,
            )
            runs.append(default_intro_run)
            explicit_intro_run = run_engine(
                binary,
                rom,
                None,
                "webgpu",
                DEFAULT_PROBE_FRAMES,
                root / "frames-webgpu-intro",
                root / "run-webgpu-intro",
                args.timeout,
                dump_every=DEFAULT_PROBE_EVERY,
                dump_from=DEFAULT_PROBE_FROM,
                verbose=args.verbose,
            )
            runs.append(explicit_intro_run)
            failures.extend(
                check_default_webgpu(default_intro_run, explicit_intro_run)
            )
            prearmed_intro_run = run_engine(
                binary,
                rom,
                None,
                "webgpu",
                DEFAULT_PROBE_FRAMES,
                root / "frames-webgpu-intro-prearmed",
                root / "run-webgpu-intro-prearmed",
                args.timeout,
                dump_every=DEFAULT_PROBE_EVERY,
                dump_from=DEFAULT_PROBE_FROM,
                extra_env={
                    "GE007_WEBGPU_DUMP_FRAME": str(DEFAULT_PROBE_FROM),
                },
                verbose=args.verbose,
            )
            runs.append(prearmed_intro_run)
            failures.extend(check_first_dump_prearm(
                explicit_intro_run, prearmed_intro_run
            ))

            authored_common = {
                "MDKR_SIMULATION_CADENCE": "original",
                "MDKR_SYNTH_FIELDS": "2",
                "MDKR_PRESENT_SCHED_TRACE": "1",
            }
            authored_intro_run = run_engine(
                binary, rom, script, "webgpu", AUTHORED_INTRO_FRAMES,
                root / "frames-authored-intro",
                root / "run-authored-intro", args.timeout,
                dump_every=1, dump_from=AUTHORED_INTRO_FROM,
                window_size="320x240",
                extra_env={**authored_common, "MDKR_PRESENT_RATE": "original"},
                verbose=args.verbose,
            )
            rate60_intro_run = run_engine(
                binary, rom, script, "webgpu", RATE60_INTRO_FRAMES,
                root / "frames-rate60-intro",
                root / "run-rate60-intro", args.timeout,
                dump_every=1, dump_from=RATE60_INTRO_FROM,
                window_size="320x240",
                extra_env={**authored_common, "MDKR_PRESENT_RATE": "60"},
                verbose=args.verbose,
            )
            runs.extend((authored_intro_run, rate60_intro_run))
            failures.extend(check_authored_rate_sequence(
                authored_intro_run, rate60_intro_run,
                AUTHORED_INTRO_FROM, AUTHORED_INTRO_FRAMES,
                RATE60_INTRO_FROM, RATE60_INTRO_FRAMES,
                MAX_INTRO_FRAMING_MAD,
                "authored intro/menu framing",
            ))

            authored_vehicle_run = run_engine(
                binary, rom, script, "webgpu", AUTHORED_VEHICLE_FRAMES,
                root / "frames-authored-vehicle",
                root / "run-authored-vehicle", args.timeout,
                dump_every=1, dump_from=AUTHORED_VEHICLE_FROM,
                window_size="320x240",
                extra_env={**authored_common, "MDKR_PRESENT_RATE": "original"},
                verbose=args.verbose,
            )
            rate60_vehicle_run = run_engine(
                binary, rom, script, "webgpu", RATE60_VEHICLE_FRAMES,
                root / "frames-rate60-vehicle",
                root / "run-rate60-vehicle", args.timeout,
                dump_every=1, dump_from=RATE60_VEHICLE_FROM,
                window_size="320x240",
                extra_env={**authored_common, "MDKR_PRESENT_RATE": "60"},
                verbose=args.verbose,
            )
            runs.extend((authored_vehicle_run, rate60_vehicle_run))
            failures.extend(check_authored_rate_sequence(
                authored_vehicle_run, rate60_vehicle_run,
                AUTHORED_VEHICLE_FROM, AUTHORED_VEHICLE_FRAMES,
                RATE60_VEHICLE_FROM, RATE60_VEHICLE_FRAMES,
                MAX_AUTHORED_PAIR_MAD,
                "authored racer/vehicle-part sequence",
            ))
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
        "check_renderer_backends: PASS — diagnostic GL and WebGPU reached the "
        "same race within coarse global pixel budgets; forced window failure "
        "failed closed without GL; "
        "consecutive GL captures retained completed frames; the no-selector "
        "intro matched explicit WebGPU byte for byte; first CLI dump matched "
        "a prearmed final target; authored intro and vehicle-part sequences "
        "remained framed and complete at =60"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
