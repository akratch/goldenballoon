#!/usr/bin/env python3
"""Inject one unprovable exact probe and prove the tick still publishes.

A real track query can come back INVALID (unprovable) for one probe while
every other query that tick stays answerable. The camera contract says a FULL
follow camera with fan freedom never goes target-blind, so a single unprovable
probe must not take the whole recovery ladder down with it: the mainline seal,
the emergency fan and the safe fallback each prove their own candidate, and a
candidate whose own proof is complete has to stay publishable.

The seam (MDKR_TEST_CAMERA_EXACT_INVALID_TICK) makes exactly one exact static
probe of one tick INVALID. The injected tick must still report degraded=1 --
the probe's failure is real and telemetry keeps it -- while publishing a valid,
target-visible pose: invalid=0, target_hidden=0.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
from pathlib import Path

from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, find_fatal,
                           resolve_binary)


SCRIPT = "tests/input_scripts/race_drive_long.txt"
# The runtime's tick counter is scene-local and several camera slots print
# rows at the same tick value, so the seam fires wherever an exact static
# probe first executes at this tick value. The assertions are therefore
# whole-run invariants rather than single-row surgery: the injected probe may
# land in any scene, and no tick anywhere is allowed to die of it.
FAULT_TICK = 500
SUMMARY = "camera_obstruction_observe summary"


def field(row: str, name: str) -> int:
    match = re.search(rf"\b{re.escape(name)}=(\d+)", row)
    if match is None:
        raise SystemExit(f"missing {name}: {row[:260]}")
    return int(match.group(1))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=3400)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not Path(path).is_file():
            raise SystemExit(f"missing: {path}")

    env = dict(os.environ)
    env.update(
        MDKR_RENDERER="gl",
        MDKR_AUDIO="0",
        MDKR_PRESENT_RATE="original",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_CAMERA_OBSTRUCTION="modern",
        MDKR_CAMERA_TRACE="1",
        MDKR_TEST_CAMERA_EXACT_INVALID_TICK=str(FAULT_TICK),
    )
    proc = subprocess.run(
        [binary, "--headless-frames", str(args.frames), "--input-script", SCRIPT,
         "--rom", args.rom],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, timeout=args.timeout,
    )
    failures: list[str] = []
    if proc.returncode != 0:
        failures.append(f"process exited {proc.returncode}")
    marker = find_fatal(proc.stdout, *ASSERT_MARKERS)
    if marker:
        failures.append(f"runtime emitted {marker}")

    rows = [row for row in proc.stdout.splitlines() if SUMMARY in row]
    if not rows:
        failures.append("no camera summary rows at all")

    # The injection must actually have happened: exactly one probe was made
    # unprovable, and telemetry has to keep the mark.
    marked = [row for row in rows if field(row, "probe_degraded") != 0]
    if not marked:
        failures.append("no row carries probe_degraded=1 -- the seam did not fire")
    elif len(marked) > 1:
        failures.append(
            f"{len(marked)} rows carry probe_degraded -- the seam leaked "
            f"beyond its one probe: {marked[1]}")

    # The contract: one unprovable probe may cost its own answer, never the
    # tick. Every tick in the run still publishes a proven, target-visible
    # pose.
    for row in rows:
        for name in ("invalid", "target_hidden"):
            if field(row, name) != 0:
                failures.append(
                    f"tick {field(row, 'tick')} died of one unprovable probe "
                    f"({name}!=0): {row}")
                break

    if failures:
        print("check_camera_probe_invalid_recovery: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_camera_probe_invalid_recovery: PASS "
          f"(tick {FAULT_TICK} degraded yet published)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
