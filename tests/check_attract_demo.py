#!/usr/bin/env python3
"""Validate title-demo vehicle selection, long-soak stability, and teardown.

GAME-08 was a source-labelled retail defect: ``load_level_for_menu()`` forced
``VEHICLE_PLANE`` for every menu level. Ancient Lake and the following rolling
demo both declare car, so their AI racers consumed the plane-node family while
the camera made individual frames look plausible.

The fixed arm idles for 12,000 frames, crosses both rolling demos, returns to the
title, and requires every live menu racer to use its level-header default. The
legacy arm comes from the same binary via ``MDKR_MENU_VEHICLE=legacy`` and must
reproduce plane selection on both demos. A third arm presses START during the
first demo and proves normal teardown into the interactive menu.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


SOAK_FRAMES = 12000
CONTROL_FRAMES = 7200
EXIT_FRAMES = 6200
EXIT_SCRIPT = "tests/input_scripts/attract_exit.txt"
ROLLING_DEMOS = (18, 28)

LEVEL_INFO_RE = re.compile(
    r"level_vehicles: id=(\d+) default=(\d+) avail=0x([0-9a-fA-F]+)"
    r"(?: type=(-?\d+) world=(-?\d+))?"
)
LOAD_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=(-?\d+) entrance=(-?\d+) "
    r"vehicle=(-?\d+) cutscene=(-?\d+) @frame~(\d+)"
)
EVTQ_RE = re.compile(r"\[EVTQ\] q\d+\([^)]+\) new peak (\d+) of (\d+)")
DEMO_RE = re.compile(
    r"demo_vehicle: level=(\d+) requested=(-?\d+) path=(-?\d+) racer=(-?\d+)"
)
BAD_RE = re.compile(
    r"\[FATAL\]|\[CRASH\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion|\[EVTQ\] post DROPPED"
)


@dataclass
class Run:
    rc: int
    output: str
    defaults: dict[int, int]
    loads: list[tuple[int, int, int, int, int, int]]
    demos: dict[int, tuple[int, int, int]]
    peaks: list[tuple[int, int]]
    bad: list[str]


def run(binary: str, rom: str, frames: int, *, legacy: bool,
        script: str | None = None) -> Run:
    env = dict(os.environ)
    env.update({
        "MDKR_AUDIO": "0",
        "MDKR_SIMULATION_CADENCE": "enhanced",
        "MDKR_SYNTH_FIELDS": "1",
        "MDKR_TRACE": "1",
        "MDKR_EVTQ_STATS": "1",
    })
    if legacy:
        env["MDKR_MENU_VEHICLE"] = "legacy"
    else:
        env.pop("MDKR_MENU_VEHICLE", None)
    command = [binary, "--headless-frames", str(frames)]
    if script is not None:
        command.extend(("--input-script", script))
    command.extend(("--rom", rom))
    with tempfile.TemporaryDirectory(prefix="mdkr_attract_") as run_dir:
        env["MDKR_VIDEO_CONFIG_PATH"] = os.path.join(run_dir, "video.ini")
        env["MDKR_SAVE_DIR"] = os.path.join(run_dir, "save")
        process = subprocess.run(
            command, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=900, check=False,
        )
    output = process.stdout.decode("utf-8", "replace")
    defaults = {int(match.group(1)): int(match.group(2))
                for match in LEVEL_INFO_RE.finditer(output)}
    loads = [tuple(map(int, match.groups())) for match in LOAD_RE.finditer(output)]
    demos = {
        int(match.group(1)): tuple(map(int, match.groups()[1:]))
        for match in DEMO_RE.finditer(output)
    }
    peaks = [tuple(map(int, match.groups())) for match in EVTQ_RE.finditer(output)]
    return Run(process.returncode, output, defaults, loads, demos, peaks,
               BAD_RE.findall(output))


def check_process(name: str, result: Run, failures: list[str]) -> None:
    if result.rc != 0:
        failures.append(f"{name}: exited {result.rc}")
    if result.bad:
        failures.append(f"{name}: bad runtime marker {result.bad[0]}")
    if not result.defaults:
        failures.append(f"{name}: no level-header vehicle census")
    if not result.loads:
        failures.append(f"{name}: no level-load trace")


def rolling_loads(result: Run) -> dict[int, tuple[int, int, int, int, int, int]]:
    return {load[0]: load for load in result.loads if load[0] in ROLLING_DEMOS}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    for path in (binary, args.rom, EXIT_SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    fixed = run(binary, args.rom, SOAK_FRAMES, legacy=False)
    legacy = run(binary, args.rom, CONTROL_FRAMES, legacy=True)
    exit_run = run(binary, args.rom, EXIT_FRAMES, legacy=False,
                   script=EXIT_SCRIPT)
    failures: list[str] = []
    for name, result in (("fixed", fixed), ("legacy", legacy),
                         ("exit", exit_run)):
        check_process(name, result, failures)

    fixed_demo = rolling_loads(fixed)
    legacy_demo = rolling_loads(legacy)
    for level in ROLLING_DEMOS:
        default = fixed.defaults.get(level)
        if default is None:
            failures.append(f"fixed: missing default vehicle for level {level}")
            continue
        if level not in fixed_demo:
            failures.append(f"fixed: rolling demo {level} never loaded")
        elif fixed_demo[level][3] != default:
            failures.append(
                f"fixed: demo {level} loaded vehicle {fixed_demo[level][3]}, "
                f"header default is {default}"
            )
        if level not in fixed.demos:
            failures.append(f"fixed: demo {level} emitted no path/racer witness")
        else:
            requested, path, racer = fixed.demos[level]
            if (requested, path, racer) != (default, default, default):
                failures.append(
                    f"fixed: demo {level} requested/path/racer "
                    f"{requested}/{path}/{racer}, want {default}/{default}/{default}"
                )
        if level not in legacy_demo:
            failures.append(f"legacy: rolling demo {level} never loaded")
        elif legacy_demo[level][3] != 2:
            failures.append(
                f"legacy: demo {level} loaded vehicle {legacy_demo[level][3]}, "
                "want forced plane (2)"
            )
        if level not in legacy.demos:
            failures.append(f"legacy: demo {level} emitted no path/racer witness")
        else:
            requested, path, racer = legacy.demos[level]
            if requested != 2 or path != 2 or racer != default:
                failures.append(
                    f"legacy: demo {level} requested/path/racer "
                    f"{requested}/{path}/{racer}, want 2/2/{default}"
                )
        if default == 2:
            failures.append(
                f"control is vacuous: demo {level}'s header also selects plane"
            )

    # The production soak must complete both demos and tear them down to the
    # frontend/title level. Current deterministic sequence is 18 -> 28 -> 23.
    fixed_ids = [load[0] for load in fixed.loads]
    try:
        first = fixed_ids.index(18)
        second = fixed_ids.index(28, first + 1)
        fixed_ids.index(23, second + 1)
    except ValueError:
        failures.append(
            f"fixed: did not complete rolling sequence 18 -> 28 -> 23; "
            f"loads={fixed_ids}"
        )

    # START during Ancient Lake must end that demo before its normal 1,500-frame
    # timer would select demo 28, entering the interactive menu instead.
    exit_ids = [load[0] for load in exit_run.loads]
    if 18 not in exit_ids:
        failures.append(f"exit: Ancient Lake never loaded; loads={exit_ids}")
    if 28 in exit_ids:
        failures.append(
            "exit: second demo loaded despite START during the first demo"
        )
    if not any(level not in ROLLING_DEMOS and frame > 5400
               for level, _players, _entrance, _vehicle, _cutscene, frame
               in exit_run.loads):
        failures.append(
            f"exit: START did not leave the rolling demo; loads={exit_ids}"
        )

    if not fixed.peaks:
        failures.append("fixed: event-queue telemetry never ran")
    for peak, capacity in fixed.peaks:
        if peak > capacity:
            failures.append(
                f"fixed: event queue exceeded capacity {peak}/{capacity}"
            )

    if failures:
        print("check_attract_demo: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        if args.verbose:
            print("  fixed loads:", fixed.loads)
            print("  legacy loads:", legacy.loads)
            print("  exit loads:", exit_run.loads)
        return 1

    print("check_attract_demo: PASS")
    print(
        "  fixed: 18/28 used header-default car; sequence 18 -> 28 -> 23 "
        f"completed in {SOAK_FRAMES} frames"
    )
    print("  control: both demos reproduced the historical forced-plane path")
    print("  input: START exited demo 18 into the interactive menu before demo 28")
    if fixed.peaks:
        peak, capacity = max(fixed.peaks, key=lambda item: item[0] / item[1])
        print(f"  audio event-queue high water: {peak}/{capacity}, zero drops")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
