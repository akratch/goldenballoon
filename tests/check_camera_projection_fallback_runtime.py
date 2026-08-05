#!/usr/bin/env python3
"""Inject one projection-latch failure and prove the restored pair stays safe."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary


SCRIPT = "tests/input_scripts/race_drive_long.txt"
FAULT_TICK = 800
BAD = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:", "Assertion")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=3400)
    parser.add_argument("--timeout", type=int, default=180)
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
        MDKR_CAMERA_TRACE="2",
        MDKR_TEST_CAMERA_PROJECTION_FAIL_TICK=str(FAULT_TICK),
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
    for marker in BAD:
        if marker in proc.stdout:
            failures.append(f"runtime emitted {marker}")

    token = f"tick={FAULT_TICK} "
    details = [
        row for row in proc.stdout.splitlines()
        if "camera_obstruction_observe detail" in row and token in row
    ]
    summaries = [
        row for row in proc.stdout.splitlines()
        if "camera_obstruction_observe summary" in row and token in row
    ]
    if len(details) != 1 or len(summaries) != 1:
        emitted_ticks = [
            int(match.group(1))
            for row in proc.stdout.splitlines()
            if "camera_obstruction_observe summary" in row
            and (match := re.search(r"\btick=(\d+)", row))
        ]
        failures.append(
            f"fault tick emitted {len(details)} detail and {len(summaries)} summary rows; "
            f"range={min(emitted_ticks, default=-1)}..{max(emitted_ticks, default=-1)}"
        )
    else:
        detail = details[0]
        summary = summaries[0]
        for pattern, label in (
            (r"resolve=\{status=4 valid=1 .* discontinuity=1 .* clearance=0 ",
             "validated camera fallback"),
            (r"projection_mismatches=0 ", "projection generation handshake"),
            (r"resolved=\{corrected=0 penetrated=0 invalid=0 degraded=0\}",
             "safe fallback summary"),
            (r"missing_cache=0 missing_identity=0 .* uncategorized=0 ",
             "complete dynamic source"),
            (r"invalid_transform=0 capacity_failures=0", "dynamic publication health"),
            (r"invalid_sweeps=0 sphere_invalid_sweeps=0 exact_invalid_sweeps=0",
             "dynamic query health"),
        ):
            row = detail if label == "validated camera fallback" else summary
            if re.search(pattern, row) is None:
                failures.append(f"fault tick lacks {label}: {row[:300]}")

    all_summaries = [
        row for row in proc.stdout.splitlines()
        if "camera_obstruction_observe summary" in row
    ]
    if any(
        "projection_mismatches=0" not in row or
        not re.search(r"resolved=\{corrected=\d+ penetrated=0 invalid=0 degraded=0\}", row)
        for row in all_summaries
    ):
        failures.append("another tick reported an unsafe or mismatched Modern pair")

    if failures:
        print("check_camera_projection_fallback_runtime: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print(
        "check_camera_projection_fallback_runtime: PASS — injected tick 800 "
        "restored a freshly revalidated pose/projection pair"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
