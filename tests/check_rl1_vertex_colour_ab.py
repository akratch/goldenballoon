#!/usr/bin/env python3
"""Run the bounded RL-1 three-arm vertex-colour decision experiment.

This is evidence for an art-direction decision, not RL-2 implementation. The
same fixed diagnostic sun and camera frame are applied to:

* Ancient Lake (level 5), the bright reference;
* Fire Mountain (level 11), the dark reference.

Each track captures baked-only, baked-as-low-frequency-base plus directional
light, and baked-colour-superseded. The gate proves all arms render the same
simulation state, diagnostic triangles are genuinely exercised, the hypotheses
are visually material, and retaining baked colour stays closer to the authored
bright and dark exposures than superseding it.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TRACKS = (("ancient-lake", 5), ("fire-mountain", 11))
ARMS = ("baked", "baked-sun", "supersede")
CAPTURE_FRAME = 3475
FRAMES = CAPTURE_FRAME + 1
RL1_RE = re.compile(r"\[RL1\] arm=([a-z-]+) triangles=(\d+)")
FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion|\[FX BUG\]"
)


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class ArmResult:
    track: str
    arm: str
    triangles: int
    pace: tuple[str, ...]
    image: Image


@dataclass(frozen=True)
class SceneMetrics:
    mean_luma: float
    mean_saturation: float


def environment(backend: str, track_id: int, arm: str) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    env.update(
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_AUTOPILOT="1",
        MDKR_TRACE="1",
        MDKR_RENDERER=backend,
        MDKR_LOAD_TRACK=str(track_id),
        MDKR_RL1_ARM=arm,
        MDKR_TEST_RENDER_FULL_ADMISSION="1",
        MDKR_DUMP_FROM=str(CAPTURE_FRAME),
        MDKR_DUMP_EVERY="999",
        MDKR64_HIDDEN="1",
        LC_ALL="C",
    )
    return env


def normalized_pace(output: str) -> tuple[str, ...]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker >= 0:
            rows.append(re.sub(r" dtms=\S+", " dtms=<wall>", line[marker:]))
    return tuple(rows)


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


def run_arm(
    binary: Path,
    rom: Path,
    backend: str,
    track: str,
    track_id: int,
    arm: str,
    work: Path,
    timeout: int,
    verbose: bool,
) -> ArmResult:
    label = f"{track}-{arm}"
    run_dir = work / label
    frames_dir = run_dir / "frames"
    frames_dir.mkdir(parents=True)
    command = [
        str(binary),
        "--headless-frames",
        str(FRAMES),
        "--input-script",
        str(SCRIPT),
        "--dump-frames",
        str(frames_dir),
        "--rom",
        str(rom),
        "--window-size",
        "320x240",
        "--remastered",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=environment(backend, track_id, arm),
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
    telemetry = list(RL1_RE.finditer(output))
    frames = sorted(frames_dir.glob("*.ppm"))
    if (
        proc.returncode != 0
        or fatal is not None
        or len(telemetry) != 1
        or len(frames) != 1
    ):
        raise RuntimeError(
            f"{label}: exit={proc.returncode}, "
            f"fatal={fatal.group(0) if fatal else 'none'}, "
            f"telemetry={len(telemetry)}, frames={len(frames)}\n"
            f"{output[-4000:]}"
        )
    pace = normalized_pace(output)
    if not pace:
        raise RuntimeError(f"{label}: no [PACE] rows")
    actual_arm, triangles = telemetry[0].groups()
    if actual_arm != arm:
        raise RuntimeError(
            f"{label}: runtime selected arm {actual_arm!r}"
        )
    return ArmResult(
        track=track,
        arm=arm,
        triangles=int(triangles),
        pace=pace,
        image=read_ppm(frames[0]),
    )


def scene_metrics(image: Image) -> SceneMetrics:
    # Exclude the HUD edges and minimap; retain road, cliffs, trees and racers.
    x0, x1 = int(image.width * 0.04), int(image.width * 0.96)
    y0, y1 = int(image.height * 0.18), int(image.height * 0.90)
    total_luma = 0
    total_saturation = 0
    samples = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            offset = (y * image.width + x) * 3
            r, g, b = image.pixels[offset:offset + 3]
            total_luma += (77 * r + 150 * g + 29 * b) >> 8
            total_saturation += max(r, g, b) - min(r, g, b)
            samples += 1
    return SceneMetrics(
        mean_luma=total_luma / samples,
        mean_saturation=total_saturation / samples,
    )


def mean_absolute_difference(left: Image, right: Image) -> float:
    if (left.width, left.height) != (right.width, right.height):
        raise ValueError("capture dimensions differ")
    return sum(
        abs(a - b)
        for a, b in zip(left.pixels, right.pixels, strict=True)
    ) / len(left.pixels)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--backend", choices=("gl", "webgpu"), default="webgpu"
    )
    parser.add_argument(
        "--keep-frames",
        help="retain the six evidence captures under this new directory",
    )
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    for path in (binary, rom, SCRIPT):
        if not path.is_file():
            print(f"FAIL RL-1 A/B: missing {path}", file=sys.stderr)
            return 1

    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.keep_frames:
        work = Path(args.keep_frames).expanduser().resolve()
        if work.exists():
            print(
                f"FAIL RL-1 A/B: --keep-frames target already exists: {work}",
                file=sys.stderr,
            )
            return 1
        work.mkdir(parents=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="mdkr_rl1_ab_")
        work = Path(temporary.name)

    failures: list[str] = []
    results: dict[tuple[str, str], ArmResult] = {}
    try:
        for track, track_id in TRACKS:
            for arm in ARMS:
                try:
                    result = run_arm(
                        binary,
                        rom,
                        args.backend,
                        track,
                        track_id,
                        arm,
                        work,
                        args.timeout,
                        args.verbose,
                    )
                    results[(track, arm)] = result
                except (OSError, RuntimeError, ValueError) as exc:
                    failures.append(str(exc))

        for track, _track_id in TRACKS:
            if any((track, arm) not in results for arm in ARMS):
                continue
            baked = results[(track, "baked")]
            baked_sun = results[(track, "baked-sun")]
            supersede = results[(track, "supersede")]

            if baked.triangles != 0:
                failures.append(
                    f"{track}: baked arm changed {baked.triangles} triangles"
                )
            if (
                baked_sun.triangles < 1000
                or supersede.triangles != baked_sun.triangles
            ):
                failures.append(
                    f"{track}: diagnostic reach differs "
                    f"(baked-sun={baked_sun.triangles}, "
                    f"supersede={supersede.triangles})"
                )
            if not (
                baked.pace == baked_sun.pace == supersede.pace
            ):
                failures.append(
                    f"{track}: an RL-1 arm changed normalized PACE state"
                )

            base_metrics = scene_metrics(baked.image)
            sun_metrics = scene_metrics(baked_sun.image)
            supersede_metrics = scene_metrics(supersede.image)
            sun_mad = mean_absolute_difference(
                baked.image, baked_sun.image
            )
            supersede_mad = mean_absolute_difference(
                baked.image, supersede.image
            )
            if sun_mad < 1.0 or supersede_mad < 1.0:
                failures.append(
                    f"{track}: an RL-1 hypothesis was visually inert "
                    f"(baked-sun MAD={sun_mad:.2f}, "
                    f"supersede MAD={supersede_mad:.2f})"
                )
            if supersede_mad <= sun_mad * 1.25:
                failures.append(
                    f"{track}: supersede did not discard materially more "
                    f"authored signal ({sun_mad:.2f} vs {supersede_mad:.2f})"
                )
            if abs(sun_metrics.mean_luma - base_metrics.mean_luma) >= abs(
                supersede_metrics.mean_luma - base_metrics.mean_luma
            ):
                failures.append(
                    f"{track}: baked-sun did not preserve exposure better "
                    f"than supersede"
                )

            print(
                f"  {track}: triangles={baked_sun.triangles}, "
                f"meanLuma={base_metrics.mean_luma:.2f}/"
                f"{sun_metrics.mean_luma:.2f}/"
                f"{supersede_metrics.mean_luma:.2f}, "
                f"saturation={base_metrics.mean_saturation:.2f}/"
                f"{sun_metrics.mean_saturation:.2f}/"
                f"{supersede_metrics.mean_saturation:.2f}, "
                f"MAD-from-baked={sun_mad:.2f}/{supersede_mad:.2f}"
            )

        fire = {
            arm: results.get(("fire-mountain", arm))
            for arm in ARMS
        }
        if all(fire.values()):
            fire_baked = scene_metrics(fire["baked"].image)  # type: ignore[union-attr]
            fire_sun = scene_metrics(fire["baked-sun"].image)  # type: ignore[union-attr]
            fire_super = scene_metrics(fire["supersede"].image)  # type: ignore[union-attr]
            if fire_super.mean_luma - fire_baked.mean_luma < 20.0:
                failures.append(
                    "fire-mountain: supersede did not expose the expected "
                    "dark-mood loss"
                )
            if abs(fire_sun.mean_luma - fire_baked.mean_luma) > 10.0:
                failures.append(
                    "fire-mountain: baked-sun failed to retain the authored "
                    "dark exposure"
                )

        if failures:
            print("FAIL RL-1 A/B:", file=sys.stderr)
            for failure in failures:
                print(f"  - {failure}", file=sys.stderr)
            if args.keep_frames:
                print(f"  evidence retained in {work}", file=sys.stderr)
            return 1

        print(
            "PASS RL-1 A/B: retain baked vertex colour as the "
            "low-frequency ambient base; reject full supersession"
        )
        if args.keep_frames:
            print(f"  evidence retained in {work}")
        return 0
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
