#!/usr/bin/env python3
"""Measure the representative WD-0 caster matrix on GL and WebGPU.

This is a baseline instrument, not a visual acceptance oracle. It drives the
same capture-only frontend through contrasting worlds and 1P/2P/4P layouts,
requires bounded complete ownership, and requires the common frontend telemetry
to agree exactly between shipped backends. The focused world_fx_capture gate
separately proves that this neutral instrument changes neither pixels nor state.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from check_adventure_hub import HUB_TOUR_ROUTE
from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
INPUTS = ROOT / "tests" / "input_scripts"
BACKENDS = ("gl", "webgpu")
FATAL_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion|\[FX BUG\]"
)
WORLD_FX_RE = re.compile(
    r"\[WORLD-FX\] mode=neutral frames=(\d+) committed=(\d+) "
    r"failed=(\d+) views=(\d+) triangles=(\d+) ranges=(\d+) "
    r"static=(\d+) dynamic=(\d+) cache=(\d+)/(\d+) "
    r"opaque=(\d+) masked=(\d+) matrices=(\d+) matrixPeak=(\d+) "
    r"lookup=(\d+)/(\d+) allocFails=(\d+)"
)
SHADOW_PLAN_RE = re.compile(
    r"\[SHADOW-PLAN\] valid=(\d+) views=(\d+) maps=(\d+) res=(\d+) "
    r"bytes=(\d+) nearTexel=([0-9.]+) farTexel=([0-9.]+)"
)


@dataclass(frozen=True)
class Scene:
    name: str
    script: str
    frames: int
    views: int
    track: int | None = None
    hub_route: bool = False


SCENES = (
    Scene("ancient-lake", "nav_to_time_trial_race.txt", 3500, 1, 5),
    Scene("fire-mountain", "nav_to_time_trial_race.txt", 3500, 1, 11),
    Scene("everfrost-peak", "nav_to_time_trial_race.txt", 3500, 1, 13),
    Scene("whale-bay", "nav_to_time_trial_race.txt", 3500, 1, 8),
    Scene("tricky-boss", "nav_to_time_trial_race.txt", 3500, 1, 38),
    Scene("timbers-island", "adventure_hub_drive.txt", 7500, 1, None, True),
    Scene("ancient-lake-2p", "race_2p_split.txt", 3500, 2, 5),
    Scene("ancient-lake-4p", "race_4p_split.txt", 3500, 4, 5),
)


@dataclass(frozen=True)
class Result:
    scene: Scene
    backend: str
    telemetry: tuple[int, ...]
    elapsed: float


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run_scene(
    binary: Path,
    rom: Path,
    scene: Scene,
    backend: str,
    root: Path,
    timeout: int,
    verbose: bool,
) -> Result:
    run_dir = root / f"{scene.name}-{backend}"
    save_dir = run_dir / "save"
    run_dir.mkdir(parents=True)
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
        MDKR_WORLD_SHADOW="0",
        MDKR_WORLD_FX_TRACE="1",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(save_dir),
        LC_ALL="C",
    )
    if scene.track is not None:
        env["MDKR_LOAD_TRACK"] = str(scene.track)
    if scene.hub_route:
        env["MDKR_DRIVE_ROUTE"] = HUB_TOUR_ROUTE

    command = [
        str(binary),
        "--headless-frames", str(scene.frames),
        "--input-script", str(INPUTS / scene.script),
        "--rom", str(rom),
        "--window-size", "320x240",
        "--remastered",
    ]
    if verbose:
        print(
            f"$ ({scene.name}-{backend}) {' '.join(command)}",
            flush=True,
        )
    started = time.perf_counter()
    try:
        process = subprocess.run(
            command,
            cwd=run_dir,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            f"{scene.name}-{backend}: timed out\n"
            f"{(exc.stdout or '')[-5000:]}"
        ) from exc
    elapsed = time.perf_counter() - started
    output = process.stdout
    fatal = FATAL_RE.search(output)
    rows = list(WORLD_FX_RE.finditer(output))
    plan_rows = list(SHADOW_PLAN_RE.finditer(output))
    require(
        process.returncode == 0 and fatal is None and
        len(rows) == 1 and len(plan_rows) == 1,
        f"{scene.name}-{backend}: exit={process.returncode}, "
        f"fatal={fatal.group(0) if fatal else 'none'}, telemetry={len(rows)}, "
        f"plans={len(plan_rows)}\n"
        f"{output[-6000:]}",
    )
    telemetry = tuple(int(value) for value in rows[0].groups())
    (
        frames, committed, failed, views, triangles, ranges,
        static, dynamic, cache_hits, cache_misses,
        opaque, _masked, matrices, matrix_peak,
        hits, _misses, alloc_fails,
    ) = telemetry
    require(
        frames > 500 and committed == frames and failed == 0,
        f"{scene.name}-{backend}: incomplete frame lifecycle {telemetry}",
    )
    require(
        views == scene.views and triangles > 0 and ranges > 0 and
        static > 0 and dynamic > 0,
        f"{scene.name}-{backend}: caster/view contract {telemetry}",
    )
    require(
        cache_hits > 0 and cache_misses > 0 and opaque > 0 and
        matrices > 0 and matrix_peak >= matrices and hits > 0 and
        alloc_fails == 0,
        f"{scene.name}-{backend}: ownership telemetry {telemetry}",
    )
    plan = plan_rows[0]
    expected_cascades = 2 if scene.views <= 2 else 1
    expected_resolution = 2048 if scene.views == 1 else 1024
    expected_maps = scene.views * expected_cascades
    expected_bytes = (
        expected_maps * expected_resolution * expected_resolution * 4
    )
    require(
        int(plan.group(1)) == 1 and
        int(plan.group(2)) == scene.views and
        int(plan.group(3)) == expected_maps and
        int(plan.group(4)) == expected_resolution and
        int(plan.group(5)) == expected_bytes and
        float(plan.group(6)) > 0.0 and
        float(plan.group(7)) > 0.0 and
        (
            scene.views != 1 or
            float(plan.group(7)) >= float(plan.group(6))
        ),
        f"{scene.name}-{backend}: invalid cascade plan {plan.groups()}",
    )
    return Result(scene, backend, telemetry, elapsed)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument(
        "--rom",
        default=str(ROOT / "baserom.us.v80.z64"),
    )
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    require(binary.is_file(), f"native binary not found: {binary}")
    require(rom.is_file(), f"ROM not found: {rom}")

    results: list[Result] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-world-fx-matrix-") as temp:
        run_root = Path(temp)
        for scene in SCENES:
            pair = [
                run_scene(
                    binary, rom, scene, backend, run_root,
                    args.timeout, args.verbose,
                )
                for backend in BACKENDS
            ]
            require(
                pair[0].telemetry == pair[1].telemetry,
                f"{scene.name}: GL/WebGPU frontend telemetry differs: "
                f"{pair[0].telemetry} != {pair[1].telemetry}",
            )
            results.extend(pair)

    print(
        "scene                 views  static dynamic total ranges masked "
        "cache-hit/miss    GL-s WGPU-s"
    )
    for scene in SCENES:
        gl = next(
            item for item in results
            if item.scene == scene and item.backend == "gl"
        )
        wgpu = next(
            item for item in results
            if item.scene == scene and item.backend == "webgpu"
        )
        telemetry = gl.telemetry
        print(
            f"{scene.name:21} {telemetry[3]:5d} "
            f"{telemetry[6]:7d} {telemetry[7]:7d} "
            f"{telemetry[4]:5d} {telemetry[5]:6d} {telemetry[11]:6d} "
            f"{telemetry[8]:9d}/{telemetry[9]:-5d} "
            f"{gl.elapsed:6.2f} {wgpu.elapsed:6.2f}"
        )
    print(
        "measure_world_fx_matrix: PASS — representative 1P worlds and 2P/4P "
        "layouts publish bounded, backend-identical caster ownership"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
