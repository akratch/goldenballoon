#!/usr/bin/env python3
"""HUD RNG/timers/audio must not depend on whether the scene was drawn.

The assertion is a normal-vs-skip-odd comparison of the complete v3 state and
ordered gameplay-event streams.  HUD-specific action traces additionally prove
that the comparison reached the countdown cues in 1P/2P/4P, the correct
music-start/multiplayer-mute branch, and a complete forced wrong-way nag timer.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Case:
    name: str
    script: Path
    frames: int
    force_wrong_way: bool
    music_action: str


CASES = (
    Case(
        "1p",
        ROOT / "tests/input_scripts/nav_to_time_trial_race.txt",
        4300,
        True,
        "race_music_start",
    ),
    Case(
        "2p",
        ROOT / "tests/input_scripts/race_2p_split.txt",
        3300,
        False,
        "race_music_start",
    ),
    Case(
        "4p",
        ROOT / "tests/input_scripts/race_4p_split.txt",
        3400,
        False,
        "race_music_mute",
    ),
)


def marked(output: str, prefix: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith(prefix)]


def hud_actions(output: str) -> list[str]:
    prefix = "[TRACE] hud_authority: "
    return [line[len(prefix):] for line in output.splitlines()
            if line.startswith(prefix)]


def action_name(row: str) -> str:
    for token in row.split():
        if token.startswith("action="):
            return token.split("=", 1)[1]
    raise RuntimeError(f"malformed HUD authority row: {row}")


def first_difference(left: list[str], right: list[str]) -> str:
    for index, (left_row, right_row) in enumerate(zip(left, right)):
        if left_row != right_row:
            return f"row {index}: normal={left_row!r}, skip={right_row!r}"
    if len(left) != len(right):
        index = min(len(left), len(right))
        left_row = left[index] if index < len(left) else "<missing>"
        right_row = right[index] if index < len(right) else "<missing>"
        return f"row {index}: normal={left_row!r}, skip={right_row!r}"
    return "no differing row"


def run_arm(binary: Path, rom: Path, case: Case, skip: bool,
            run_root: Path, timeout: int, verbose: bool) -> str:
    label = f"{case.name}-{'skip' if skip else 'normal'}"
    run_dir = run_root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_EVENT_HASH="1",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_STATE_HASH="3",
        MDKR_TRACE="1",
    )
    if case.force_wrong_way:
        env["MDKR_TEST_HUD_FORCE_WRONG_WAY"] = "1"
    if skip:
        env["MDKR_TEST_SKIP_RENDER"] = "odd"
    command = [
        str(binary),
        "--headless-frames", str(case.frames),
        "--input-script", str(case.script),
        "--rom", str(rom),
        "--window-size", "320x240",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
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
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{label}: exit {process.returncode}\n{output[-4000:]}")
    for marker in ("[CRASH]", "[FATAL]", "AddressSanitizer"):
        if marker in output:
            raise RuntimeError(f"{label}: fatal marker {marker}")
    return output


def assert_case(case: Case, normal: str, skip: str) -> list[str]:
    failures: list[str] = []
    normal_state = marked(normal, "[SIMHASH]")
    skip_state = marked(skip, "[SIMHASH]")
    normal_events = marked(normal, "[EVENTHASH]")
    skip_events = marked(skip, "[EVENTHASH]")
    normal_hud = hud_actions(normal)
    skip_hud = hud_actions(skip)

    if not normal_state or normal_state != skip_state:
        failures.append(
            f"{case.name}: normal/skip v3 state streams differ "
            f"({len(normal_state)} vs {len(skip_state)} rows); "
            f"{first_difference(normal_state, skip_state)}")
    if not normal_events or normal_events != skip_events:
        failures.append(
            f"{case.name}: normal/skip ordered event streams differ "
            f"({len(normal_events)} vs {len(skip_events)} rows); "
            f"{first_difference(normal_events, skip_events)}")
    if not normal_hud or normal_hud != skip_hud:
        failures.append(
            f"{case.name}: HUD action order/timing differs under skipped render\n"
            f"  normal={normal_hud}\n  skip={skip_hud}")
        return failures

    counts = Counter(action_name(row) for row in normal_hud)
    for action in ("race_ready", "race_go", case.music_action):
        if counts[action] != 1:
            failures.append(
                f"{case.name}: expected exactly one {action}, "
                f"observed {counts[action]} in {normal_hud}")
    other_music = (
        "race_music_mute" if case.music_action == "race_music_start"
        else "race_music_start"
    )
    if counts[other_music] != 0:
        failures.append(
            f"{case.name}: took wrong music branch {other_music}")

    if case.force_wrong_way:
        nag_count = counts["wrong_way"] + counts["wrong_way_prefix"]
        if nag_count < 2:
            failures.append(
                f"{case.name}: forced wrong-way path produced only "
                f"{nag_count} nag cue(s)")
        if counts["wrong_way_timer_zero"] < 1:
            failures.append(
                f"{case.name}: forced wrong-way timer never completed")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument(
        "--case", choices=("all", *(case.name for case in CASES)),
        default="all", help="run one route while iterating (default: all)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    selected = CASES if args.case == "all" else tuple(
        case for case in CASES if case.name == args.case)
    missing = [path for path in (binary, rom, *(case.script for case in selected))
               if not path.exists()]
    if missing:
        print("check_hud_render_authority: FAIL — missing " +
              ", ".join(str(path) for path in missing))
        return 1

    failures: list[str] = []
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-hud-authority-") as tmp:
            run_root = Path(tmp)
            for case in selected:
                normal = run_arm(
                    binary, rom, case, False, run_root,
                    args.timeout, args.verbose)
                skip = run_arm(
                    binary, rom, case, True, run_root,
                    args.timeout, args.verbose)
                failures.extend(assert_case(case, normal, skip))
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        failures.append(str(error))

    if failures:
        print("check_hud_render_authority: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    scope = "1P/2P/4P" if args.case == "all" else args.case.upper()
    print(
        f"check_hud_render_authority: PASS — {scope} countdown audio and "
        "forced wrong-way RNG/timer/event order are render-independent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
