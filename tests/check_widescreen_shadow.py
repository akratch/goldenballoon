#!/usr/bin/env python3
"""Closed-loop widescreen and shadow regression.

This is deliberately ROM-backed and therefore is not registered with CTest.
It complements the millisecond, ROM-free display_config unit test with five
real-game runs:

  * 4:3, 16:9 and 21:9 must emit identical normalized [PACE] state streams.
    That catches the subtle DKR failure where widening object visibility changes
    AI/physics/RNG through render-side effects even though every frame looks fine.
  * Normal shadow heaps must stay in bounds and every shadow group must request
    decal depth mode.
  * Tiny fault-injection heaps must drop whole meshes without crashing or
    crossing the physical allocation.
  * MDKR_SHADOW_DECAL=0 is a positive control: the same route must report real
    non-decal groups and fewer final ZMODE_DEC triangles, proving the check did
    not pass because it rendered no shadows or because a source flag was lost
    before reaching the renderer.

Usage:
    python3 tests/check_widescreen_shadow.py \
        --build build-ws-webgpu --rom baserom.us.v80.z64 -v

Run it again against an ASan build to validate the forced-overflow arm under the
allocator:
    python3 tests/check_widescreen_shadow.py \
        --build build-ws-asan --rom baserom.us.v80.z64
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

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"
MIN_FRAMES = 3400

SHADOW_RE = re.compile(
    r"\[SHADOW\] decal=(on|off) "
    r"dataPeak=(\d+)/(\d+) triPeak=(\d+)/(\d+) vtxPeak=(\d+)/(\d+) "
    r"overflowDrops=(\d+) emptyMeshes=(\d+) drawGroups=(\d+) nonDecal=(\d+)"
)
DEPTH_RE = re.compile(
    r"\[DEPTH\] decalTriangles=(\d+) comparedTriangles=(\d+)"
)


@dataclass(frozen=True)
class ShadowStats:
    decal: str
    data_peak: int
    data_cap: int
    tri_peak: int
    tri_cap: int
    vtx_peak: int
    vtx_cap: int
    overflow_drops: int
    empty_meshes: int
    draw_groups: int
    non_decal: int


@dataclass(frozen=True)
class DepthStats:
    decal_triangles: int
    compared_triangles: int


@dataclass
class RunResult:
    label: str
    command: list[str]
    returncode: int
    output: str
    pace: list[str]
    shadow: ShadowStats | None
    depth: DepthStats | None


def normalized_pace(output: str) -> list[str]:
    rows: list[str] = []
    for line in output.splitlines():
        marker = line.find("[PACE]")
        if marker < 0:
            continue
        row = line[marker:]
        # Wall-clock measurement is intentionally non-deterministic; every
        # simulation field to either side must remain byte-identical.
        row = re.sub(r" dtms=\S+", " dtms=<wall>", row)
        rows.append(row)
    return rows


def parse_shadow(output: str) -> ShadowStats | None:
    matches = list(SHADOW_RE.finditer(output))
    if not matches:
        return None
    match = matches[-1]
    values = [int(value) for value in match.groups()[1:]]
    return ShadowStats(match.group(1), *values)


def parse_depth(output: str) -> DepthStats | None:
    matches = list(DEPTH_RE.finditer(output))
    if not matches:
        return None
    match = matches[-1]
    return DepthStats(int(match.group(1)), int(match.group(2)))


def clean_environment(renderer: str | None) -> dict[str, str]:
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("MDKR")
    }
    # Prevent a developer's diagnostic shell from silently changing either arm.
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


def run_case(
    binary: Path,
    rom: Path,
    frames: int,
    label: str,
    size: str,
    base_env: dict[str, str],
    overrides: dict[str, str] | None,
    timeout: int,
    verbose: bool,
) -> RunResult:
    env = dict(base_env)
    if overrides:
        env.update(overrides)
    command = [
        str(binary),
        "--headless-frames",
        str(frames),
        "--window-size",
        size,
        "--input-script",
        str(SCRIPT),
        "--rom",
        str(rom),
    ]
    if verbose:
        print(f"$ ({label}) " + shlex.join(command), flush=True)
    try:
        # A fresh cwd gives each arm an independent save/ directory. This keeps
        # the trajectory reproducible and lets GL/WebGPU/ASan harnesses run in
        # parallel without racing one EEPROM file.
        with tempfile.TemporaryDirectory(prefix=f"mdkr_{label.replace(':', '_')}_") as run_dir:
            proc = subprocess.run(
                command,
                cwd=run_dir,
                env=env,
                text=True,
                capture_output=True,
                timeout=timeout,
                check=False,
            )
        output = proc.stdout + proc.stderr
        result = RunResult(
            label,
            command,
            proc.returncode,
            output,
            normalized_pace(output),
            parse_shadow(output),
            parse_depth(output),
        )
    except subprocess.TimeoutExpired as exc:
        captured = (exc.stdout or "") + (exc.stderr or "")
        result = RunResult(
            label, command, 124, captured, normalized_pace(captured), None, None
        )
    if verbose and result.shadow:
        print(f"  {result.shadow}")
    if verbose and result.depth:
        print(f"  {result.depth}")
    return result


def run_failures(result: RunResult) -> list[str]:
    failures: list[str] = []
    if result.returncode != 0:
        failures.append(f"{result.label}: exit code {result.returncode}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer"):
        if marker in result.output:
            failures.append(f"{result.label}: output contains {marker}")
    if not result.pace:
        failures.append(f"{result.label}: no [PACE] rows")
    if result.shadow is None:
        failures.append(f"{result.label}: no parseable [SHADOW] report")
        return failures
    if result.depth is None:
        failures.append(f"{result.label}: no parseable [DEPTH] report")
        return failures

    stats = result.shadow
    for name, peak, cap in (
        ("data", stats.data_peak, stats.data_cap),
        ("tri", stats.tri_peak, stats.tri_cap),
        ("vtx", stats.vtx_peak, stats.vtx_cap),
    ):
        if not 0 <= peak <= cap:
            failures.append(
                f"{result.label}: {name} high-water {peak} outside capacity {cap}"
            )
    if stats.draw_groups <= 0:
        failures.append(f"{result.label}: rendered zero shadow groups")
    if result.depth.compared_triangles <= 0:
        failures.append(f"{result.label}: emitted zero depth-compared triangles")
    if not 0 <= result.depth.decal_triangles <= result.depth.compared_triangles:
        failures.append(
            f"{result.label}: decal-triangle count "
            f"{result.depth.decal_triangles} outside compared-triangle count "
            f"{result.depth.compared_triangles}"
        )
    return failures


def first_pace_difference(reference: RunResult, candidate: RunResult) -> str:
    limit = min(len(reference.pace), len(candidate.pace))
    for index in range(limit):
        if reference.pace[index] != candidate.pace[index]:
            return (
                f"row {index} differs:\n"
                f"    {reference.label}: {reference.pace[index]}\n"
                f"    {candidate.label}: {candidate.pace[index]}"
            )
    return (
        f"row counts differ: {reference.label}={len(reference.pace)}, "
        f"{candidate.label}={len(candidate.pace)}"
    )


def print_failure_context(result: RunResult) -> None:
    print(f"\n--- {result.label}: last 50 output lines ---", file=sys.stderr)
    for line in result.output.splitlines()[-50:]:
        print(line, file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=4000)
    parser.add_argument("--timeout", type=int, default=180, help="seconds per arm")
    parser.add_argument("--renderer", choices=("gl", "webgpu"), default=None)
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
    if args.frames < MIN_FRAMES:
        print(
            f"FAIL: --frames must be >= {MIN_FRAMES} so the route reaches live shadows",
            file=sys.stderr,
        )
        return 1

    env = clean_environment(args.renderer)
    cases = [
        run_case(binary, rom, args.frames, "4:3", "640x480", env, None,
                 args.timeout, args.verbose),
        run_case(binary, rom, args.frames, "16:9", "960x540", env, None,
                 args.timeout, args.verbose),
        run_case(binary, rom, args.frames, "21:9", "1260x540", env, None,
                 args.timeout, args.verbose),
        run_case(
            binary,
            rom,
            args.frames,
            "tiny shadow heaps",
            "1260x540",
            env,
            {"MDKR_SHADOW_CAPS": "2,8,16"},
            args.timeout,
            args.verbose,
        ),
        run_case(
            binary,
            rom,
            args.frames,
            "decal positive control",
            "640x480",
            env,
            {"MDKR_SHADOW_DECAL": "0"},
            args.timeout,
            args.verbose,
        ),
    ]

    failures: list[str] = []
    for result in cases:
        failures.extend(run_failures(result))

    reference = cases[0]
    for candidate in cases[1:3]:
        if candidate.pace != reference.pace:
            failures.append(
                f"{candidate.label}: simulation stream changed from 4:3; "
                + first_pace_difference(reference, candidate)
            )

    # The diagnostic changes only the shadow render mode. It must not feed back
    # into race state, which also makes the visual A/B frames directly comparable.
    if cases[4].pace != reference.pace:
        failures.append(
            "decal positive control: simulation stream changed from production; "
            + first_pace_difference(reference, cases[4])
        )

    for result in cases[:3]:
        if result.shadow:
            if result.shadow.decal != "on":
                failures.append(f"{result.label}: production decal mode is not on")
            if result.shadow.overflow_drops != 0:
                failures.append(
                    f"{result.label}: {result.shadow.overflow_drops} unexpected overflow drops"
                )
            if result.shadow.non_decal != 0:
                failures.append(
                    f"{result.label}: {result.shadow.non_decal} non-decal shadow groups"
                )

    forced = cases[3].shadow
    if forced:
        if forced.overflow_drops <= 0:
            failures.append("tiny shadow heaps: positive control caused no overflow drops")
        if forced.non_decal != 0:
            failures.append(
                f"tiny shadow heaps: {forced.non_decal} non-decal shadow groups"
            )

    disabled = cases[4].shadow
    disabled_depth = cases[4].depth
    if disabled and disabled_depth:
        if disabled.decal != "off":
            failures.append("decal positive control: diagnostic did not disable decal mode")
        if disabled.non_decal <= 0:
            failures.append(
                "decal positive control: no non-decal groups observed; "
                "the test is not sensitive to the old bug"
            )
        production_depth = reference.depth
        assert production_depth is not None
        if production_depth.decal_triangles <= disabled_depth.decal_triangles:
            failures.append(
                "decal positive control: final decoded ZMODE_DEC triangle count "
                f"did not decrease ({production_depth.decal_triangles} -> "
                f"{disabled_depth.decal_triangles})"
            )

    if failures:
        print("FAIL: widescreen/shadow regression", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        failed_labels = {
            failure.split(":", 1)[0] for failure in failures if ":" in failure
        }
        for result in cases:
            if result.label in failed_labels or result.returncode != 0:
                print_failure_context(result)
        return 1

    normal = reference.shadow
    forced = cases[3].shadow
    assert normal is not None and forced is not None
    print(
        "PASS: 4:3/16:9/21:9 simulation streams match; "
        f"{normal.draw_groups} production shadow groups all use decal depth; "
        f"tiny heaps dropped {forced.overflow_drops} complete meshes safely; "
        "the no-decal positive control changed final decoded depth state without "
        "changing simulation"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
