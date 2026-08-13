#!/usr/bin/env python3
"""Modern camera safety and state retirement across real level lifecycles."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from check_presentation_lifecycle import SCENARIOS, Scenario
from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, find_fatal,
                           resolve_binary)


RESET_RE = re.compile(
    r"\[CAMERA-RESET\] serial=(\d+) retired_tick=(\d+) selected=(\d+) "
    r"validated=(\d+) obstructed=(\d+)"
)
LOAD_RE = re.compile(r"level_load: levelId=(-?\d+) numPlayers=(-?\d+)")
SUMMARY = "camera_obstruction_observe summary"


@dataclass(frozen=True)
class Positioned:
    position: int
    values: tuple[int, ...]


def field(row: str, name: str) -> int:
    match = re.search(rf"\b{re.escape(name)}=(\d+)", row)
    if match is None:
        raise RuntimeError(f"missing {name}: {row[:260]}")
    return int(match.group(1))


def run(binary: Path, rom: Path, scenario: Scenario, root: Path,
        timeout: int) -> str:
    run_root = root / scenario.name
    run_root.mkdir()
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_AUTOPILOT="1",
        MDKR_CAMERA_OBSTRUCTION="modern",
        MDKR_CAMERA_RESET_TRACE="1",
        MDKR_CAMERA_TRACE="1",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(run_root / "save"),
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_VIDEO_CONFIG_PATH=str(run_root / "missing.ini"),
    )
    env.update(scenario.environment)
    if scenario.name == "adventure-postrace":
        env["MDKR_OBJDUMP"] = "1"
    process = subprocess.run(
        [str(binary), "--headless-ticks", str(scenario.ticks),
         "--window-size", "1280x720", "--input-script", str(scenario.script),
         "--rom", str(rom)],
        cwd=run_root, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False,
    )
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(
            f"{scenario.name}: exit {process.returncode}\n{output[-4000:]}")
    marker = find_fatal(output, *ASSERT_MARKERS)
    if marker:
        raise RuntimeError(f"{scenario.name}: emitted {marker}")
    return output


def inspect(scenario: Scenario, output: str) -> list[str]:
    failures: list[str] = []
    resets = [Positioned(match.start(), tuple(map(int, match.groups())))
              for match in RESET_RE.finditer(output)]
    loads = [Positioned(match.start(), tuple(map(int, match.groups())))
             for match in LOAD_RE.finditer(output)]
    summaries = [
        (match.start(), match.group(0))
        for match in re.finditer(rf"^.*{re.escape(SUMMARY)}.*$", output, re.MULTILINE)
    ]

    if len(resets) < 2 or not loads or len(summaries) < 1000:
        failures.append(
            f"{scenario.name}: incomplete lifecycle evidence: "
            f"resets={len(resets)} loads={len(loads)} summaries={len(summaries)}")
        return failures
    serials = [entry.values[0] for entry in resets]
    if serials != list(range(serials[0], serials[0] + len(serials))):
        failures.append(f"{scenario.name}: reset serials are not consecutive: {serials}")

    # load_level_game/menu call reset immediately before level_load. Every load
    # therefore needs its own intervening retirement witness.
    previous_load = -1
    for load in loads:
        candidates = [reset for reset in resets
                      if previous_load < reset.position < load.position]
        if not candidates:
            failures.append(
                f"{scenario.name}: level load {load.values} had no preceding reset")
            continue
        reset = candidates[-1]
        next_summary = next((row for position, row in summaries
                             if position > load.position), None)
        if next_summary is None or field(next_summary, "tick") != 1:
            failures.append(
                f"{scenario.name}: load {load.values} did not restart at camera tick 1")
        previous_load = load.position

    corrected = 0
    embedded_run = 0
    max_embedded_run = 0
    hidden_run = 0
    max_hidden_run = 0
    hidden_rows = 0
    follow_hidden = 0
    follow_hidden_sample = ""
    for _, row in summaries:
        corrected += field(row, "corrected")
        if field(row, "target_embedded") > 0:
            embedded_run += 1
            max_embedded_run = max(max_embedded_run, embedded_run)
        else:
            embedded_run = 0
        # Follow cameras fan to an alternate shot to keep the target visible,
        # so a hidden target on a tick where every selected camera is a follow
        # camera is a resolver defect. Fixed door cameras are depenetrate-only
        # -- they never swing off the door they present -- and may lose the
        # racer behind a door frame for a bounded stretch; those ticks are
        # held to a run bound and an overall budget instead.
        hidden = field(row, "target_hidden")
        if hidden > 0 and field(row, "depenetrate_only") == 0:
            follow_hidden += 1
            follow_hidden_sample = follow_hidden_sample or row
        if hidden > 0:
            hidden_rows += 1
            hidden_run += 1
            max_hidden_run = max(max_hidden_run, hidden_run)
        else:
            hidden_run = 0
        for name in ("penetrated", "invalid", "degraded", "duplicates",
                     "projection_mismatches", "missing_cache", "missing_identity",
                     "uncategorized", "invalid_transform", "capacity_failures",
                     "invalid_sweeps", "sphere_invalid_sweeps",
                     "exact_invalid_sweeps"):
            if field(row, name) != 0:
                failures.append(f"{scenario.name}: nonzero {name}: {row}")
                break
    # The post-race route is the obstruction stimulus. Pause/restart routes
    # exist to prove state retirement and may remain entirely in open space.
    if corrected == 0 and scenario.name == "adventure-postrace":
        failures.append(f"{scenario.name}: lifecycle route never exercised correction")
    if max_embedded_run > 120:
        failures.append(
            f"{scenario.name}: target remained embedded for {max_embedded_run} ticks"
        )
    if follow_hidden:
        failures.append(
            f"{scenario.name}: target hidden on {follow_hidden} tick(s) with "
            f"every selected camera free to fan: {follow_hidden_sample}"
        )
    # A depenetrate-only shot may lose the racer briefly; 45 ticks is 1.5s.
    if max_hidden_run > 45:
        failures.append(
            f"{scenario.name}: target remained hidden for {max_hidden_run} ticks"
        )
    if hidden_rows * 20 > len(summaries):
        failures.append(
            f"{scenario.name}: target hidden on {hidden_rows} of "
            f"{len(summaries)} camera ticks, above the 1-in-20 budget"
        )
    if scenario.name == "adventure-postrace":
        active_transition_rows = sum(
            field(row, "active_transitioning_doors") > 0 for _, row in summaries)
        temporal_moved_rows = sum(
            field(row, "temporal_moved") > 0 for _, row in summaries)
        if "balloon 10 COLLECTED" not in output:
            failures.append("adventure-postrace: balloon/open-door trigger was not reached")
        if re.search(r"objdump:\s+DOOR.*\bopen=1\b", output) is None:
            failures.append("adventure-postrace: no post-trigger open door was observed")
        if active_transition_rows == 0:
            failures.append("adventure-postrace: no moving door publication was observed")
        if temporal_moved_rows == 0:
            failures.append(
                "adventure-postrace: moving door never published a temporal envelope"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--only", help="comma-separated scenario names")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    scenarios = list(SCENARIOS)
    if args.only:
        wanted = {item.strip() for item in args.only.split(",")}
        scenarios = [scenario for scenario in scenarios if scenario.name in wanted]
    for path in (binary, rom, *(scenario.script for scenario in scenarios)):
        if not path.is_file():
            raise SystemExit(f"missing: {path}")
    if not scenarios:
        raise SystemExit("--only selected no scenarios")

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr_camera_lifecycle_") as temp:
        root = Path(temp)
        try:
            for scenario in scenarios:
                output = run(binary, rom, scenario, root, args.timeout)
                scenario_failures = inspect(scenario, output)
                failures.extend(scenario_failures)
                print(
                    f"  {scenario.name}: "
                    f"{output.count('[CAMERA-RESET]')} resets, "
                    f"{output.count(SUMMARY)} summaries"
                )
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            failures.append(str(error))
    if failures:
        print("check_camera_obstruction_lifecycle_runtime: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_camera_obstruction_lifecycle_runtime: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
