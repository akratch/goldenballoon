#!/usr/bin/env python3
"""Prove that mipmaps reduce temporal shimmer on a moving track surface.

M1 originally had only a still Ancient Lake comparison. That can show that two
settings render differently, but it cannot show the problem mipmaps exist to
solve: unstable minification while the camera moves.

This gate drives Everfrost Peak (level 13) with the game's closed-loop AI and
captures 24 consecutive frames on both native rendering backends. Each backend
has two otherwise-identical Remastered arms:

    Video.AnisotropicFiltering=1, Video.Mipmaps=0
    Video.AnisotropicFiltering=1, Video.Mipmaps=1

The fixed crop is the distant icy track band visible above the racers. Within
that crop, the comparison selects pixels materially affected by mipmapping in
at least three frames, then measures mean absolute temporal second difference:

    abs(luma[t] - 2*luma[t-1] + luma[t-2])

That rejects constant appearance differences and measures frame-to-frame
instability. The enabled arm must reduce it by at least 5%. Runtime upload
telemetry independently proves that complete mip chains reached the backend;
the normalized PACE stream and traced displacement prove identical simulation
and a genuinely moving camera.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path

from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, fatal_re,
                           FX_MARKERS, read_ppm as read_ppm_bytes,
                           resolve_binary)


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TRACK = 13  # ASSET_LEVEL_EVERFROSTPEAK
CAPTURE_FROM = 3476
# Six consecutive 24-frame windows. A single window's benefit swings 8x with
# the capture start (measured 1.87%-15.65% across six nearby windows when the
# AI-table fix legitimately moved the autopilot line), so one window
# characterises a frame choice, not the renderer. The gate now scores every
# window and asserts the ensemble, which is stable against small trajectory
# shifts while keeping full power against a genuinely broken mip path (whose
# benefit is ~0 in EVERY window).
WINDOW_FRAMES = 24
WINDOW_COUNT = 6
CAPTURE_COUNT = WINDOW_FRAMES * WINDOW_COUNT
FRAMES = CAPTURE_FROM + CAPTURE_COUNT
WINDOW_SIZE = "320x240"  # 640x480 framebuffer on the macOS Retina test host
MIN_ACTIVE_FRACTION = 0.02
MIN_TEMPORAL_BENEFIT = 0.05

MIP_RE = re.compile(r"\[MIP\] uploads=(\d+) levels=(\d+)")
PACE_POSITION_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+)"
)
FATAL_RE = fatal_re(*ASSERT_MARKERS, *FX_MARKERS)


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class Arm:
    backend: str
    enabled: bool
    uploads: int
    levels: int
    pace: tuple[str, ...]
    positions: tuple[tuple[int, float, float, float], ...]
    images: tuple[Image, ...]


@dataclass(frozen=True)
class MotionMetric:
    active_pixels: int
    crop_pixels: int
    disabled_energy: float
    enabled_energy: float

    @property
    def benefit(self) -> float:
        if self.disabled_energy <= 0.0:
            return 0.0
        return 1.0 - self.enabled_energy / self.disabled_energy


def environment(backend: str) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_LOAD_TRACK=str(TRACK),
        MDKR_DUMP_FROM=str(CAPTURE_FROM),
        MDKR_DUMP_EVERY="1",
        MDKR64_HIDDEN="1",
        LC_ALL="C",
        # World shadows are pinned OFF in BOTH arms, via the documented
        # diagnostic seam (video_config_runtime.c publishes g_pcSunShadow with
        # this exact off-switch "for exact fallback A/Bs"). The gate measures
        # one variable — Video.Mipmaps — and the measured crop's temporal
        # energy must not depend on unrelated Remastered features. Bisect
        # evidence for why this matters: b5f227d (drop shadow-pass culling,
        # a correct casting improvement) darkened this crop and moved the
        # benefit from 5.27%/5.20% to 4.64%/4.64% with mip behavior
        # unchanged (identical 1037/5845 uploads) — a shadow change failing
        # a mip gate. An earlier scenario change had already eroded the
        # margin from 10.25%/10.04% at the gate's introduction (moved
        # 271.3 -> 516.0). Shadows-off restores a shadow-independent
        # baseline; the 5% floor is unchanged.
        MDKR_WORLD_SHADOW="0",
    )
    return env


def read_ppm(path: Path) -> Image:
    return Image(*read_ppm_bytes(path))


def normalized_pace(output: str) -> tuple[str, ...]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker >= 0:
            rows.append(re.sub(r" dtms=\S+", " dtms=<wall>", line[marker:]))
    return tuple(rows)


def run_arm(
    binary: Path,
    rom: Path,
    backend: str,
    enabled: bool,
    work: Path,
    timeout: int,
    verbose: bool,
) -> Arm:
    label = f"{backend}-mips-{'on' if enabled else 'off'}"
    run_dir = work / label
    dump_dir = run_dir / "frames"
    dump_dir.mkdir(parents=True)
    command = [
        str(binary),
        "--headless-frames",
        str(FRAMES),
        "--input-script",
        str(SCRIPT),
        "--dump-frames",
        str(dump_dir),
        "--rom",
        str(rom),
        "--window-size",
        WINDOW_SIZE,
        "--remastered",
        "--video-set",
        "Video.RenderScale=1",
        "--video-set",
        "Video.AnisotropicFiltering=1",
        "--video-set",
        f"Video.Mipmaps={int(enabled)}",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=environment(backend),
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
    mip_matches = list(MIP_RE.finditer(output))
    dumps = sorted(dump_dir.glob("*.ppm"))
    if (
        proc.returncode != 0
        or fatal is not None
        or len(mip_matches) != 1
        or len(dumps) != CAPTURE_COUNT
    ):
        raise RuntimeError(
            f"{label}: exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}, "
            f"mipTelemetry={len(mip_matches)}, "
            f"dumps={len(dumps)} (want {CAPTURE_COUNT})\n"
            f"{output[-4000:]}"
        )

    pace = normalized_pace(output)
    positions = tuple(
        (
            int(match.group(1)),
            float(match.group(2)),
            float(match.group(3)),
            float(match.group(4)),
        )
        for match in PACE_POSITION_RE.finditer(output)
        if CAPTURE_FROM <= int(match.group(1)) < FRAMES
    )
    if not pace or len(positions) != CAPTURE_COUNT:
        raise RuntimeError(
            f"{label}: PACE rows={len(pace)}, "
            f"capture-window positions={len(positions)}"
        )

    mip = mip_matches[0]
    return Arm(
        backend=backend,
        enabled=enabled,
        uploads=int(mip.group(1)),
        levels=int(mip.group(2)),
        pace=pace,
        positions=positions,
        images=tuple(read_ppm(path) for path in dumps),
    )


def luma(pixels: bytes, offset: int) -> int:
    # Integer BT.601 approximation. Exact weights matter less than using the
    # same deterministic transform for both arms.
    return (
        pixels[offset] * 77
        + pixels[offset + 1] * 150
        + pixels[offset + 2] * 29
    ) >> 8


def motion_metric(disabled: Arm, enabled: Arm) -> MotionMetric:
    first = disabled.images[0]
    if any(
        image.width != first.width or image.height != first.height
        for image in disabled.images + enabled.images
    ):
        raise ValueError(f"{disabled.backend}: capture dimensions differ")

    width, height = first.width, first.height
    # Fixed evidence band: the distant, minified icy track surface. Keeping it
    # narrow avoids the HUD, the player kart, minimap, and most near geometry.
    x0, x1 = int(width * 0.03), int(width * 0.97)
    y0, y1 = int(height * 0.40), int(height * 0.50)
    crop_pixels = max(0, x1 - x0) * max(0, y1 - y0)
    active: list[int] = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            offset = (y * width + x) * 3
            changed_frames = 0
            for off_image, on_image in zip(
                disabled.images, enabled.images, strict=True
            ):
                channel_delta = max(
                    abs(
                        off_image.pixels[offset + channel]
                        - on_image.pixels[offset + channel]
                    )
                    for channel in range(3)
                )
                if channel_delta >= 2:
                    changed_frames += 1
            if changed_frames >= 3:
                active.append(offset)

    def temporal_second_difference(images: tuple[Image, ...]) -> float:
        total = 0
        samples = 0
        for index in range(2, len(images)):
            previous2 = images[index - 2].pixels
            previous = images[index - 1].pixels
            current = images[index].pixels
            for offset in active:
                total += abs(
                    luma(current, offset)
                    - 2 * luma(previous, offset)
                    + luma(previous2, offset)
                )
                samples += 1
        return total / samples if samples else 0.0

    return MotionMetric(
        active_pixels=len(active),
        crop_pixels=crop_pixels,
        disabled_energy=temporal_second_difference(disabled.images),
        enabled_energy=temporal_second_difference(enabled.images),
    )


def displacement(positions: tuple[tuple[int, float, float, float], ...]) -> float:
    _, x0, y0, z0 = positions[0]
    _, x1, y1, z1 = positions[-1]
    return math.sqrt((x1 - x0) ** 2 + (y1 - y0) ** 2 + (z1 - z0) ** 2)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--backend",
        action="append",
        choices=("gl", "webgpu"),
        help="backend to test; repeatable (default: both)",
    )
    parser.add_argument(
        "--keep-frames",
        help="retain the four evidence sequences under this directory",
    )
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    for path in (binary, rom, SCRIPT):
        if not path.is_file():
            print(f"FAIL mip motion: missing {path}", file=sys.stderr)
            return 1

    backends = tuple(dict.fromkeys(args.backend or ("gl", "webgpu")))
    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.keep_frames:
        work = Path(args.keep_frames).expanduser().resolve()
        if work.exists():
            print(
                f"FAIL mip motion: --keep-frames target already exists: {work}",
                file=sys.stderr,
            )
            return 1
        work.mkdir(parents=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="mdkr_mip_motion_")
        work = Path(temporary.name)

    failures: list[str] = []
    results: dict[tuple[str, bool], Arm] = {}
    try:
        for backend in backends:
            for enabled in (False, True):
                try:
                    results[(backend, enabled)] = run_arm(
                        binary,
                        rom,
                        backend,
                        enabled,
                        work,
                        args.timeout,
                        args.verbose,
                    )
                except (OSError, RuntimeError, ValueError) as exc:
                    failures.append(str(exc))

        baseline_pace: tuple[str, ...] | None = None
        for backend in backends:
            off = results.get((backend, False))
            on = results.get((backend, True))
            if off is None or on is None:
                continue

            if off.uploads != 0 or off.levels != 0:
                failures.append(
                    f"{backend}: disabled arm uploaded "
                    f"{off.uploads} chains/{off.levels} levels"
                )
            if on.uploads <= 0 or on.levels <= on.uploads:
                failures.append(
                    f"{backend}: enabled arm did not upload multi-level chains "
                    f"({on.uploads} chains/{on.levels} levels)"
                )
            if off.pace != on.pace:
                failures.append(
                    f"{backend}: mip toggle changed normalized PACE state"
                )
            if baseline_pace is None:
                baseline_pace = off.pace
            elif off.pace != baseline_pace:
                failures.append(
                    f"{backend}: normalized PACE differs across backends"
                )

            moved = displacement(off.positions)
            if moved < 100.0:
                failures.append(
                    f"{backend}: capture was effectively still "
                    f"({moved:.1f} world units)"
                )

            window_benefits: list[float] = []
            window_metrics = []
            window_failed = False
            for w in range(WINDOW_COUNT):
                lo, hi = w * WINDOW_FRAMES, (w + 1) * WINDOW_FRAMES
                off_w = replace(off, images=off.images[lo:hi])
                on_w = replace(on, images=on.images[lo:hi])
                try:
                    metric = motion_metric(off_w, on_w)
                except ValueError as exc:
                    failures.append(f"window {w}: {exc}")
                    window_failed = True
                    break
                window_metrics.append(metric)
                active_fraction = (
                    metric.active_pixels / metric.crop_pixels
                    if metric.crop_pixels
                    else 0.0
                )
                if active_fraction < MIN_ACTIVE_FRACTION:
                    failures.append(
                        f"{backend} window {w}: only {metric.active_pixels}/"
                        f"{metric.crop_pixels} mip-sensitive track pixels "
                        f"({active_fraction:.2%})"
                    )
                    window_failed = True
                if metric.disabled_energy <= 0.0:
                    failures.append(
                        f"{backend} window {w}: disabled sequence had no "
                        f"temporal energy"
                    )
                    window_failed = True
                window_benefits.append(metric.benefit)
            if window_failed:
                continue
            mean_benefit = sum(window_benefits) / len(window_benefits)
            positive = sum(1 for b in window_benefits if b > 0.0)
            print(
                f"  {backend}: window benefits "
                + " ".join(f"{b:.2%}" for b in window_benefits)
                + f" | mean={mean_benefit:.2%} positive={positive}/"
                + f"{len(window_benefits)}"
            )
            if mean_benefit < MIN_TEMPORAL_BENEFIT:
                failures.append(
                    f"{backend}: ensemble mean temporal benefit "
                    f"{mean_benefit:.2%} over {WINDOW_COUNT} windows "
                    f"(want >= {MIN_TEMPORAL_BENEFIT:.0%})"
                )
            if positive < WINDOW_COUNT - 1:
                failures.append(
                    f"{backend}: only {positive}/{WINDOW_COUNT} windows show "
                    f"positive mip benefit (want >= {WINDOW_COUNT - 1})"
                )
            metric = window_metrics[0]

            print(
                f"  {backend}: moved={moved:.1f}, "
                f"mipUploads={on.uploads}/{on.levels} levels, "
                f"activeTrackPixels={metric.active_pixels}/"
                f"{metric.crop_pixels}, temporalEnergy="
                f"{metric.disabled_energy:.3f}->{metric.enabled_energy:.3f} "
                f"({metric.benefit:.2%} reduction)"
            )

        if failures:
            print("FAIL mip motion:", file=sys.stderr)
            for failure in failures:
                print(f"  - {failure}", file=sys.stderr)
            if args.keep_frames:
                print(f"  evidence retained in {work}", file=sys.stderr)
            return 1

        pace_rows = len(baseline_pace or ())
        print(
            f"PASS mip motion: Everfrost Peak, {CAPTURE_COUNT} consecutive "
            f"moving frames, {pace_rows} identical PACE rows across "
            f"{len(backends) * 2} arms"
        )
        if args.keep_frames:
            print(f"  evidence retained in {work}")
        return 0
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
