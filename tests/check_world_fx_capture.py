#!/usr/bin/env python3
"""Prove the world-FX caster feed is capture-only and reaches real content.

The enabled arm records opaque and alpha-tested world geometry on GL/WebGPU
without enabling a visual effect. Its frame and normalized gameplay trace must
match the disabled arm exactly. A matrix-drop control removes every registered
world transform and must collapse capture to zero, proving the telemetry is
observing the intended ownership path rather than an unrelated draw count.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
FRAMES = 3500
CAPTURE_FRAME = FRAMES - 1
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
class Result:
    label: str
    pace: tuple[str, ...]
    frame: bytes
    telemetry: tuple[int, ...] | None


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


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
    arm: str,
    work: Path,
    timeout: int,
    verbose: bool,
) -> Result:
    run_dir = work / f"{backend}-{arm}"
    frames_dir = run_dir / "frames"
    save_dir = run_dir / "save"
    frames_dir.mkdir(parents=True)
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
        MDKR_LOAD_TRACK="5",
        MDKR_DUMP_FROM=str(CAPTURE_FRAME),
        MDKR_DUMP_EVERY="999",
        MDKR64_HIDDEN="1",
        MDKR_SAVE_DIR=str(save_dir),
        LC_ALL="C",
    )
    if arm != "off":
        env["MDKR_WORLD_FX_TRACE"] = "1"
    if arm == "matrix-drop":
        env["MDKR_WORLD_FX_MATRIX_CONTROL"] = "drop"

    command = [
        str(binary),
        "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT),
        "--dump-frames", str(frames_dir),
        "--rom", str(rom),
        "--window-size", "320x240",
        "--remastered",
    ]
    if verbose:
        print(f"$ ({backend}-{arm}) {' '.join(command)}", flush=True)
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
            f"{backend}-{arm}: timed out\n{(exc.stdout or '')[-5000:]}"
        ) from exc

    output = process.stdout
    fatal = FATAL_RE.search(output)
    frame_files = sorted(frames_dir.glob("*.ppm"))
    telemetry_rows = list(WORLD_FX_RE.finditer(output))
    plan_rows = list(SHADOW_PLAN_RE.finditer(output))
    expected_telemetry = 0 if arm == "off" else 1
    require(
        process.returncode == 0 and fatal is None and
        len(frame_files) == 1 and
        len(telemetry_rows) == expected_telemetry and
        len(plan_rows) == expected_telemetry,
        f"{backend}-{arm}: exit={process.returncode}, "
        f"fatal={fatal.group(0) if fatal else 'none'}, "
        f"frames={len(frame_files)}, telemetry={len(telemetry_rows)}, "
        f"plans={len(plan_rows)}\n"
        f"{output[-6000:]}",
    )
    pace = normalized_pace(output)
    require(pace, f"{backend}-{arm}: no [PACE] rows")
    telemetry = (
        tuple(int(value) for value in telemetry_rows[0].groups())
        if telemetry_rows else None
    )
    if plan_rows:
        plan = plan_rows[0]
        expected_valid = 0 if arm == "matrix-drop" else 1
        require(
            int(plan.group(1)) == expected_valid,
            f"{backend}-{arm}: unexpected plan {plan.groups()}",
        )
        if expected_valid:
            require(
                tuple(int(plan.group(index)) for index in range(2, 6)) ==
                (1, 2, 2048, 32 * 1024 * 1024) and
                float(plan.group(6)) > 0.0 and
                float(plan.group(7)) >= float(plan.group(6)),
                f"{backend}-{arm}: invalid plan {plan.groups()}",
            )
    return Result(
        label=f"{backend}-{arm}",
        pace=pace,
        frame=frame_files[0].read_bytes(),
        telemetry=telemetry,
    )


def verify_live(result: Result) -> None:
    require(result.telemetry is not None, f"{result.label}: missing telemetry")
    assert result.telemetry is not None
    (
        frames, committed, failed, views, triangles, ranges,
        static, dynamic, cache_hits, cache_misses,
        opaque, masked, matrices, matrix_peak, hits, misses, alloc_fails,
    ) = result.telemetry
    require(
        frames > 3000 and committed == frames and failed == 0,
        f"{result.label}: incomplete frame ownership {result.telemetry}",
    )
    require(
        views == 1 and triangles > 1000 and ranges > 100,
        f"{result.label}: real race caster set not reached {result.telemetry}",
    )
    require(
        static > 100 and dynamic > 0 and
        cache_hits > 100_000 and cache_misses > 100,
        f"{result.label}: static/dynamic ownership not exercised "
        f"{result.telemetry}",
    )
    require(
        opaque > 1_000_000 and masked > 0,
        f"{result.label}: material classes not both exercised {result.telemetry}",
    )
    require(
        matrices > 10 and matrix_peak >= matrices and
        hits > 10_000 and misses > 0,
        f"{result.label}: matrix lifecycle not exercised {result.telemetry}",
    )
    require(alloc_fails == 0, f"{result.label}: allocation failure")


def verify_drop(result: Result) -> None:
    require(result.telemetry is not None, f"{result.label}: missing telemetry")
    assert result.telemetry is not None
    (
        frames, committed, failed, views, triangles, ranges,
        static, dynamic, cache_hits, cache_misses,
        opaque, masked, matrices, matrix_peak, hits, misses, alloc_fails,
    ) = result.telemetry
    require(
        frames > 3000 and committed == frames and failed == 0,
        f"{result.label}: frame lifecycle failed in control",
    )
    require(
        views == 0 and triangles == 0 and ranges == 0 and
        static == 0 and dynamic == 0 and
        cache_hits == 0 and cache_misses == 0 and
        opaque == 0 and masked == 0 and matrices == 0 and matrix_peak == 0 and
        hits == 0 and misses > 0 and alloc_fails == 0,
        f"{result.label}: matrix-drop control did not collapse capture "
        f"{result.telemetry}",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True)
    parser.add_argument("--rom", default=str(ROOT / "baserom.us.v80.z64"))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    require(rom.is_file(), f"ROM not found: {rom}")

    with tempfile.TemporaryDirectory(prefix="mdkr-world-fx-") as temp:
        work = Path(temp)
        for backend in BACKENDS:
            off = run_arm(
                binary, rom, backend, "off", work, args.timeout, args.verbose)
            live = run_arm(
                binary, rom, backend, "capture", work, args.timeout,
                args.verbose)
            control = run_arm(
                binary, rom, backend, "matrix-drop", work, args.timeout,
                args.verbose)
            verify_live(live)
            verify_drop(control)
            require(
                live.pace == off.pace == control.pace,
                f"{backend}: capture changed normalized gameplay state",
            )
            require(
                live.frame == off.frame == control.frame,
                f"{backend}: neutral capture changed rendered output",
            )

    print(
        "check_world_fx_capture: PASS — GL/WebGPU capture real opaque+masked "
        "race geometry with byte-identical frames and [PACE]; matrix-drop "
        "control collapses the feed to zero"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
