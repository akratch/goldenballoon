#!/usr/bin/env python3
"""ROM-backed wall witness for the modern camera obstruction runtime.

The same deterministic Ancient Lake route runs through three policies in one
binary. Legacy must reproduce an authored lens overlap, center-ray must prove
that protecting only the eye center is insufficient, and Modern must correct
every resolved lens overlap while leaving the logical camera untouched.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import resolve_binary


SCRIPT = "tests/input_scripts/race_drive_long.txt"
BAD = ("[CRASH]", "[FATAL]", "AddressSanitizer", "runtime error:", "Assertion")
DETAIL = "camera_obstruction_observe detail"
SUMMARY = "camera_obstruction_observe summary"


def run(binary: str, rom: str, policy: str, frames: int, timeout: int,
        window: str = "1280x960", fov: str = "authored",
        max_hfov: str | None = None,
        extra_env: dict[str, str] | None = None) -> str:
    env = dict(os.environ)
    env.update(
        MDKR_RENDERER="gl",
        MDKR_AUDIO="0",
        MDKR_PRESENT_RATE="original",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_CAMERA_OBSTRUCTION=policy,
        MDKR_CAMERA_TRACE="2",
    )
    if extra_env:
        env.update(extra_env)
    command = [
        binary, "--headless-frames", str(frames), "--window-size", window,
        "--fov", fov,
    ]
    if max_hfov is not None:
        command.extend(("--max-hfov", max_hfov))
    command.extend(("--input-script", SCRIPT, "--rom", rom))
    proc = subprocess.run(
        command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, timeout=timeout,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"{policy} exited {proc.returncode}\n" + "\n".join(proc.stdout.splitlines()[-80:]))
    for marker in BAD:
        if marker in proc.stdout:
            raise RuntimeError(f"{policy} emitted {marker}")
    return proc.stdout


def field(row: str, name: str) -> int:
    match = re.search(rf"\b{re.escape(name)}=(\d+)", row)
    if match is None:
        raise RuntimeError(f"missing {name} in trace row: {row[:240]}")
    return int(match.group(1))


def block_field(row: str, block: str, name: str) -> int:
    match = re.search(
        rf"\b{re.escape(block)}=\{{[^}}]*\b{re.escape(name)}=(\d+)", row
    )
    if match is None:
        raise RuntimeError(f"missing {block}.{name} in trace row: {row[:300]}")
    return int(match.group(1))


def inspect(policy: str, output: str) -> dict[str, int]:
    details = [line for line in output.splitlines() if DETAIL in line]
    summaries = [line for line in output.splitlines() if SUMMARY in line]
    if len(details) < 3000 or not summaries:
        raise RuntimeError(f"{policy} produced incomplete telemetry ({len(details)} detail rows)")
    result = {
        "rows": len(details),
        "corrected": sum(field(row, "corrected") for row in details),
        "penetrated": sum(field(row, "clearance") != 0 for row in details),
        "degraded": sum(field(row, "source_degraded") for row in details),
        "invalid": sum(field(row, "valid") == 0 for row in details),
        "target_hidden": sum(
            field(row, "target_visible") == 0 and
            field(row, "target_embedded") == 0
            for row in details
        ),
        "emergency": sum(field(row, "emergency") for row in details),
        "racer_faded": sum(field(row, "racer_opacity") < 255 for row in details),
        "exact_invoked": sum(
            block_field(row, "exact_runtime", "invoked") for row in details
        ),
        "exact_override": sum(
            block_field(row, "exact_runtime", "override") for row in details
        ),
        "exact_degraded": sum(
            block_field(row, "exact_runtime", "degraded") for row in details
        ),
        "transition_invoked": sum(
            block_field(row, "transition", "invoked") for row in details
        ),
        "transition_clear": sum(
            block_field(row, "transition", "clear") for row in details
        ),
        "transition_cut": sum(
            block_field(row, "transition", "cut") for row in details
        ),
    }
    if result["degraded"] or (policy.startswith("modern") and
                              (result["invalid"] or result["target_hidden"])):
        raise RuntimeError(f"{policy} degraded/invalid runtime: {result}")
    for row in summaries:
        if "duplicates=0" not in row or "projection_mismatches=0" not in row:
            raise RuntimeError(f"{policy} authority/projection violation: {row}")
        for counter in (
            "missing_cache", "missing_identity", "uncategorized",
            "invalid_transform", "capacity_failures",
            "invalid_sweeps", "sphere_invalid_sweeps",
            "exact_invalid_sweeps",
        ):
            match = re.search(rf"{counter}=(\d+)", row)
            if match is None or int(match.group(1)) != 0:
                raise RuntimeError(f"{policy} dynamic publication violation {counter}: {row}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=5200)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not Path(path).is_file():
            raise SystemExit(f"missing: {path}")

    results = {}
    for policy in ("legacy", "center-ray", "modern"):
        results[policy] = inspect(
            policy, run(binary, args.rom, policy, args.frames, args.timeout))
        print(f"  {policy:10s} {results[policy]}")

    failures = []
    if results["legacy"]["penetrated"] == 0:
        failures.append("legacy positive control did not reproduce a lens overlap")
    if results["center-ray"]["penetrated"] == 0:
        failures.append("center-ray positive control did not expose a near-plane overlap")
    if results["modern"]["corrected"] == 0:
        failures.append("modern path never applied a correction")
    if results["modern"]["exact_invoked"] == 0:
        failures.append("modern path never invoked the exact rounded-lens phase")
    if results["modern"]["exact_degraded"] != 0:
        failures.append(
            f"modern exact runtime degraded {results['modern']['exact_degraded']} time(s)"
        )
    if results["modern"]["transition_invoked"] == 0 or \
            results["modern"]["transition_clear"] == 0:
        failures.append("modern path never proved a fractional presentation chord clear")
    if results["modern"]["penetrated"] != 0:
        failures.append(
            f"modern published {results['modern']['penetrated']} penetrated resolved pose(s)"
        )
    if failures:
        print("check_camera_obstruction_runtime: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_camera_obstruction_runtime: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
