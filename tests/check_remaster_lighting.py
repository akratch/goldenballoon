#!/usr/bin/env python3
"""Verify RL-2/CO-1/RL-5 with real GL and WebGPU race frames.

The production arm must exercise smooth normals on racers and characters,
change two differently authored worlds without changing simulation, and agree
between the shipped backends. The negative direction is equally important:
Pure and Restored must remain byte-identical when the diagnostic seam toggles.
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

from harness_utils import (ASSERT_MARKERS, fatal_re, FX_MARKERS,
                           read_ppm as read_ppm_bytes, resolve_binary)


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
TRACKS = (("ancient-lake", 5), ("fire-mountain", 11))
BACKENDS = ("gl", "webgpu")
CAPTURE_FRAME = 3475
FRAMES = CAPTURE_FRAME + 1
LIGHT_RE = re.compile(
    r"\[LIGHT\] arm=([a-z-]+) valid=(\d+) world=(-?\d+) geometry=(-?\d+) "
    r"dir=([-0-9.]+),([-0-9.]+),([-0-9.]+) "
    r"colour=([-0-9.]+),([-0-9.]+),([-0-9.]+) "
    r"strength=([-0-9.]+) sources=(0x[0-9a-fA-F]+) "
    r"racerTris=(\d+) characterTris=(\d+) missingNormals=(\d+) "
    r"space=([a-z0-9/-]+)"
)
GRADE_RE = re.compile(
    r"\[GRADE\] valid=(\d+) sat=([-0-9.]+) contrast=([-0-9.]+) "
    r"tint=([-0-9.]+),([-0-9.]+),([-0-9.]+) "
    r"space=([a-z0-9/-]+)"
)
FATAL_RE = fatal_re(*ASSERT_MARKERS, *FX_MARKERS)


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True)
class Result:
    label: str
    arm: str
    valid: int
    world: int
    geometry: int
    direction: tuple[float, float, float]
    colour: tuple[float, float, float]
    strength: float
    sources: int
    racer_triangles: int
    character_triangles: int
    missing_normals: int
    colour_space: str
    grade_valid: int
    grade_saturation: float
    grade_contrast: float
    grade_tint: tuple[float, float, float]
    grade_space: str
    pace: tuple[str, ...]
    image: Image


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def environment(
    backend: str, track_id: int, enabled: bool
) -> dict[str, str]:
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
        MDKR_RL5_LIGHT="1" if enabled else "0",
        # Keep this a single-variable RL-5/grade gate. Production world shadows
        # have their own map/decal/failure A/B in check_world_shadows.py.
        MDKR_WORLD_SHADOW="0",
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
    return Image(*read_ppm_bytes(path))


def run_arm(
    binary: Path,
    rom: Path,
    backend: str,
    track: str,
    track_id: int,
    mode: str,
    enabled: bool,
    work: Path,
    timeout: int,
    verbose: bool,
) -> Result:
    arm = "smooth-sun" if enabled else "baked"
    label = f"{backend}-{track}-{mode}-{arm}"
    run_dir = work / label
    frames_dir = run_dir / "frames"
    frames_dir.mkdir(parents=True)
    command = [
        str(binary),
        "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT),
        "--dump-frames", str(frames_dir),
        "--rom", str(rom),
        "--window-size", "320x240",
        f"--{mode}",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    try:
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=environment(backend, track_id, enabled),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"{label}: timed out\n{(exc.stdout or '')[-5000:]}"
        ) from exc

    output = proc.stdout
    fatal = FATAL_RE.search(output)
    telemetry = list(LIGHT_RE.finditer(output))
    grade_telemetry = list(GRADE_RE.finditer(output))
    frames = sorted(frames_dir.glob("*.ppm"))
    require(
        proc.returncode == 0 and fatal is None and
        len(telemetry) == 1 and len(grade_telemetry) == 1 and
        len(frames) == 1,
        f"{label}: exit={proc.returncode}, "
        f"fatal={fatal.group(0) if fatal else 'none'}, "
        f"telemetry={len(telemetry)}, grade={len(grade_telemetry)}, "
        f"frames={len(frames)}\n"
        f"{output[-5000:]}",
    )
    match = telemetry[0]
    grade = grade_telemetry[0]
    pace = normalized_pace(output)
    require(pace, f"{label}: no [PACE] rows")
    return Result(
        label=label,
        arm=match.group(1),
        valid=int(match.group(2)),
        world=int(match.group(3)),
        geometry=int(match.group(4)),
        direction=tuple(float(match.group(i)) for i in range(5, 8)),
        colour=tuple(float(match.group(i)) for i in range(8, 11)),
        strength=float(match.group(11)),
        sources=int(match.group(12), 16),
        racer_triangles=int(match.group(13)),
        character_triangles=int(match.group(14)),
        missing_normals=int(match.group(15)),
        colour_space=match.group(16),
        grade_valid=int(grade.group(1)),
        grade_saturation=float(grade.group(2)),
        grade_contrast=float(grade.group(3)),
        grade_tint=tuple(float(grade.group(i)) for i in range(4, 7)),
        grade_space=grade.group(7),
        pace=pace,
        image=read_ppm(frames[0]),
    )


def mad(left: Image, right: Image) -> float:
    require(
        (left.width, left.height) == (right.width, right.height),
        "capture dimensions differ",
    )
    return sum(
        abs(a - b)
        for a, b in zip(left.pixels, right.pixels, strict=True)
    ) / len(left.pixels)


def verify_production_pair(off: Result, on: Result) -> float:
    require(off.arm == "baked", f"{off.label}: wrong diagnostic arm")
    require(on.arm == "smooth-sun", f"{on.label}: wrong production arm")
    require(on.valid == 1, f"{on.label}: level rig is invalid")
    require(on.pace == off.pace, f"{on.label}: lighting changed simulation")
    require(
        off.racer_triangles == 0 and off.character_triangles == 0,
        f"{off.label}: baked control unexpectedly lit geometry",
    )
    require(
        on.racer_triangles > 0 and on.character_triangles > 0,
        f"{on.label}: target classes were not exercised",
    )
    require(
        on.missing_normals == 0,
        f"{on.label}: {on.missing_normals} target batches lacked normals",
    )
    length = sum(component * component for component in on.direction) ** 0.5
    require(
        abs(length - 1.0) < 0.0015 and on.direction[1] >= 0.48,
        f"{on.label}: invalid sun direction {on.direction}",
    )
    require(
        0.10 <= on.strength <= 0.16,
        f"{on.label}: unrestrained strength {on.strength}",
    )
    require(
        on.colour_space == "srgb-authored/linear-light/srgb-output",
        f"{on.label}: undocumented colour path {on.colour_space!r}",
    )
    require(
        on.grade_valid == 1 and
        0.995 <= on.grade_saturation <= 1.025 and
        0.990 <= on.grade_contrast <= 1.020 and
        all(0.980 <= channel <= 1.020 for channel in on.grade_tint) and
        on.grade_space == "srgb-input/linear-grade/srgb-output",
        f"{on.label}: invalid or unbounded world grade",
    )
    difference = mad(off.image, on.image)
    require(
        0.02 < difference < 2.0,
        f"{on.label}: visual A/B is inert or overbroad (MAD {difference:.4f})",
    )
    return difference


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build))
    if not binary.is_absolute():
        binary = (ROOT / binary).resolve()
    rom = Path(args.rom).expanduser()
    if not rom.is_absolute():
        rom = ROOT / rom
    require(binary.is_file(), f"missing binary: {binary}")
    require(rom.is_file(), f"missing ROM: {rom}")

    with tempfile.TemporaryDirectory(prefix="mdkr_rl5_") as temp_name:
        work = Path(temp_name)
        production: dict[tuple[str, str], tuple[Result, Result]] = {}
        for backend in BACKENDS:
            for track, track_id in TRACKS:
                off = run_arm(
                    binary, rom, backend, track, track_id, "remastered",
                    False, work, args.timeout, args.verbose,
                )
                on = run_arm(
                    binary, rom, backend, track, track_id, "remastered",
                    True, work, args.timeout, args.verbose,
                )
                production[(backend, track)] = (off, on)

        differences = {
            key: verify_production_pair(*pair)
            for key, pair in production.items()
        }

        for backend in BACKENDS:
            for mode in ("pure", "restored"):
                off = run_arm(
                    binary, rom, backend, TRACKS[0][0], TRACKS[0][1],
                    mode, False, work, args.timeout, args.verbose,
                )
                on = run_arm(
                    binary, rom, backend, TRACKS[0][0], TRACKS[0][1],
                    mode, True, work, args.timeout, args.verbose,
                )
                require(
                    on.pace == off.pace and on.image.pixels == off.image.pixels,
                    f"{backend}-{mode}: diagnostic seam changed legacy mode",
                )
                require(
                    on.racer_triangles == 0 and
                    on.character_triangles == 0,
                    f"{backend}-{mode}: production light escaped mode gate",
                )

        for track, _track_id in TRACKS:
            gl = production[("gl", track)][1]
            webgpu = production[("webgpu", track)][1]
            require(
                gl.direction == webgpu.direction and
                gl.colour == webgpu.colour and
                gl.strength == webgpu.strength and
                gl.sources == webgpu.sources and
                gl.grade_valid == webgpu.grade_valid and
                gl.grade_saturation == webgpu.grade_saturation and
                gl.grade_contrast == webgpu.grade_contrast and
                gl.grade_tint == webgpu.grade_tint,
                f"{track}: backends consumed different level rigs",
            )
            require(
                mad(gl.image, webgpu.image) <= 4.0,
                f"{track}: lit backend frame parity exceeded MAD 4.0",
            )

        ancient = production[("gl", "ancient-lake")][1]
        fire = production[("gl", "fire-mountain")][1]
        require(
            ancient.direction != fire.direction or
            ancient.colour != fire.colour,
            "two different level headers produced the same lighting rig",
        )
        require(
            ancient.grade_saturation != fire.grade_saturation or
            ancient.grade_contrast != fire.grade_contrast or
            ancient.grade_tint != fire.grade_tint,
            "two different authored worlds produced the same grade",
        )
        print(
            "check_remaster_lighting: PASS — runtime-derived rigs differ "
            "across Ancient Lake/Fire Mountain; smooth racer+character normals "
            "active on GL/WebGPU with zero missing batches; "
            + ", ".join(
                f"{backend}/{track} MAD={differences[(backend, track)]:.3f}"
                for backend in BACKENDS
                for track, _track_id in TRACKS
            )
            + "; Pure/Restored exact and [PACE] invariant",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, ValueError) as exc:
        print(f"check_remaster_lighting: FAIL — {exc}", file=sys.stderr)
        raise SystemExit(1)
