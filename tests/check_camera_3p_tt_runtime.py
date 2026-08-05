#!/usr/bin/env python3
"""3P + T.T. fourth-view camera-bank safety without race-progress coupling."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from check_camera_obstruction_runtime import field
from harness_utils import resolve_binary


SCRIPT = Path("tests/input_scripts/race_3p_tt_camera.txt")
SUMMARY = "camera_obstruction_observe summary"
DETAIL = "camera_obstruction_observe detail"
BAD = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:", "Assertion")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=7000)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom, SCRIPT):
        if not path.is_file():
            raise SystemExit(f"missing: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr_camera_3p_tt_") as temp:
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_CAMERA_OBSTRUCTION="modern",
            MDKR_CAMERA_TRACE="2",
            MDKR_RENDERER="gl",
            MDKR_SAVE_DIR=str(Path(temp) / "save"),
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
            MDKR_VIDEO_CONFIG_PATH=str(Path(temp) / "missing.ini"),
        )
        process = subprocess.run(
            [str(binary), "--headless-frames", str(args.frames),
             "--window-size", "1920x1080", "--input-script", str(SCRIPT.resolve()),
             "--rom", str(rom)],
            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=args.timeout, check=False,
        )
    output = process.stdout or ""
    failures: list[str] = []
    if process.returncode != 0:
        failures.append(f"process exited {process.returncode}")
    for marker in BAD:
        if marker in output:
            failures.append(f"runtime emitted {marker}")

    summaries = [row for row in output.splitlines()
                 if SUMMARY in row and " tt=1 " in row]
    tt_details = [
        row for row in output.splitlines()
        if DETAIL in row and "viewport=3 normal_slot=3 physical_slot=3 bank=normal" in row
    ]
    if len(summaries) < 3500 or len(tt_details) < 3500:
        failures.append(
            f"incomplete 3P+T.T. coverage: summaries={len(summaries)} "
            f"details={len(tt_details)}")
    for row in summaries:
        for name, expected in (("selected", 4), ("fresh_intents", 4),
                               ("stale_or_missing", 0), ("penetrated", 0),
                               ("invalid", 0), ("degraded", 0), ("duplicates", 0),
                               ("projection_mismatches", 0), ("missing_cache", 0),
                               ("missing_identity", 0), ("uncategorized", 0),
                               ("invalid_transform", 0), ("capacity_failures", 0),
                               ("invalid_sweeps", 0),
                               ("sphere_invalid_sweeps", 0),
                               ("exact_invalid_sweeps", 0)):
            if field(row, name) != expected:
                failures.append(f"summary {name} != {expected}: {row[:320]}")
                break
    corrected = 0
    for row in tt_details:
        family = re.search(r"\bintent=\{[^}]*\bfamily=(\d+)", row)
        if family is None or int(family.group(1)) != 8:
            failures.append(f"camera 3 did not publish T.T. family intent: {row[:300]}")
            break
        corrected += field(row, "corrected")
        for name, expected in (("valid", 1), ("clearance", 0),
                               ("source_degraded", 0)):
            if field(row, name) != expected:
                failures.append(f"T.T. detail {name} != {expected}: {row[:300]}")
                break
    if corrected == 0:
        failures.append("T.T. camera 3 never exercised correction")

    print(f"  tt_summaries={len(summaries)} tt_details={len(tt_details)} corrected={corrected}")
    if failures:
        print("check_camera_3p_tt_runtime: FAIL", file=sys.stderr)
        for failure in failures[:20]:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_camera_3p_tt_runtime: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
