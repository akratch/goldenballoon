#!/usr/bin/env python3
"""ROM-backed wall witness for the modern camera obstruction runtime.

The same deterministic Ancient Lake route runs through five arms in one binary.
Legacy must reproduce an authored lens overlap, center-ray must prove that
protecting only the eye center is insufficient, and Modern must correct every
resolved lens overlap while leaving the logical camera untouched.

The last arm runs with MDKR_CAMERA_OBSTRUCTION removed from the environment.
Correction is the default, so the unset arm must report the MODERN gate and
reproduce the explicit Modern arm's counters exactly -- this is the runtime
witness that a player who sets nothing gets the corrected camera.

The explicit observe arm stays, and stays load-bearing, as the opt-out witness:
it must still apply zero corrections, which is what proves that choosing the
original camera actually returns the authored one rather than a renamed
default.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import (ASSERT_MARKERS, DEFAULT_BUILD_DIR, find_fatal,
                           resolve_binary)


SCRIPT = "tests/input_scripts/race_drive_long.txt"
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
        MDKR_CAMERA_TRACE="2",
    )
    # "unset" is the shipped configuration, not a policy string: the variable is
    # removed rather than emptied so an inherited value cannot mask the default.
    if policy == "unset":
        env.pop("MDKR_CAMERA_OBSTRUCTION", None)
    else:
        env["MDKR_CAMERA_OBSTRUCTION"] = policy
    # Camera.Comfort is off by default and every arm here measures the authored
    # motion. Removed rather than set, for the same reason as above: an
    # inherited MDKR_CAMERA_COMFORT must not quietly become what this gate
    # measured.
    env.pop("MDKR_CAMERA_COMFORT", None)
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
    marker = find_fatal(proc.stdout, *ASSERT_MARKERS)
    if marker:
        raise RuntimeError(f"{policy} emitted {marker}")
    return proc.stdout


def resolved_trace(output: str) -> list[str]:
    """Every published resolved pose, in order. Two runs that framed the same
    picture produce identical lists."""
    values = []
    for row in output.splitlines():
        if DETAIL not in row:
            continue
        match = re.search(r"\bresolved=\(([^)]+)\)", row)
        if match is None:
            raise RuntimeError("missing resolved pose")
        values.append(match.group(1))
    return values


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
    gates = {match.group(1) for match in
             (re.search(r"\bgate=([A-Z_]+)\(", row) for row in summaries)
             if match is not None}
    if len(gates) != 1:
        raise RuntimeError(f"{policy} reported {sorted(gates)} gate(s), expected one")
    result = {
        "gate": gates.pop(),
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
    # Only the corrective arms owe zero invalid/hidden results; importers pass
    # their own labels, and the ones that mean Modern prefix them with it.
    # "unset" is now one of them: the default resolves to Modern, so the arm a
    # player who sets nothing runs owes exactly what the explicit Modern arm
    # owes. That is the point of the flip, not an exception to it.
    corrective = policy.startswith("modern") or policy == "unset"
    if result["degraded"] or (corrective and
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
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=5200)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not Path(path).is_file():
            raise SystemExit(f"missing: {path}")

    results = {}
    outputs = {}
    for policy in ("legacy", "center-ray", "modern", "observe", "unset"):
        outputs[policy] = run(binary, args.rom, policy, args.frames, args.timeout)
        results[policy] = inspect(policy, outputs[policy])
        print(f"  {policy:10s} {results[policy]}")

    # Camera.Comfort's reduced-motion arm, run on the default camera because
    # that is the only camera it can soften -- observe publishes nothing for it
    # to act on. Labelled "modern-..." so it carries Modern's obligations.
    outputs["comfort"] = run(
        binary, args.rom, "modern", args.frames, args.timeout,
        extra_env={"MDKR_CAMERA_COMFORT": "reduced"})
    results["comfort"] = inspect("modern-comfort-reduced", outputs["comfort"])
    print(f"  {'comfort':10s} {results['comfort']}")

    failures = []
    # Comfort is presentation, so it may change the picture and may change
    # nothing else. Safety first: a softened desired eye is still solved
    # against, so the published pose must still be proven clear.
    if results["comfort"]["penetrated"] != 0:
        failures.append(
            f"comfort arm published {results['comfort']['penetrated']} "
            "penetrated resolved pose(s)"
        )
    if results["comfort"]["gate"] != "MODERN":
        failures.append(
            f"comfort arm selected {results['comfort']['gate']}, not MODERN"
        )
    # The runtime has to have actually read the setting, and it has to have
    # done something. A comfort option that silently no-ops is the failure this
    # pair exists to catch.
    if "comfort=reduced" not in outputs["comfort"]:
        failures.append("comfort arm did not report comfort=reduced")
    if "comfort=authored" not in outputs["modern"]:
        failures.append("default arm did not report comfort=authored")
    if resolved_trace(outputs["comfort"]) == resolved_trace(outputs["modern"]):
        failures.append(
            "comfort=reduced reproduced the authored-motion camera exactly; "
            "the vertical smoother did nothing"
        )
    # ...and it has to stay legible as the thing it is. "corrected" means the
    # resolver moved the camera off the shot it was asked for, and it feeds
    # was_obstructed, the resolver status, and the whole MOTION-01 census.
    # Comfort changes what is asked for. Counting that as an obstruction
    # correction reports a permanently retracted camera on a wall-free route --
    # it is a 35x inflation of this counter, and this bound is what catches it.
    if results["comfort"]["corrected"] > 2 * results["modern"]["corrected"]:
        failures.append(
            f"comfort arm reported {results['comfort']['corrected']} corrections "
            f"against the default arm's {results['modern']['corrected']}; the "
            "comfort offset is being counted as an obstruction correction"
        )
    if results["legacy"]["penetrated"] == 0:
        failures.append("legacy positive control did not reproduce a lens overlap")
    if results["center-ray"]["penetrated"] == 0:
        failures.append("center-ray positive control did not expose a near-plane overlap")
    # target_hidden is the focus-occlusion outcome every other gate asserts is
    # zero, which leaves nothing requiring it to be producible at all. The
    # uncorrected legacy arm drives the focus behind real geometry on this
    # route, so it is the positive control: without it, publishing the focus as
    # permanently visible would satisfy the whole suite.
    if results["legacy"]["target_hidden"] == 0:
        failures.append("legacy positive control never hid the camera focus")
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
    # The opt-out witness. Choosing the original camera has to actually return
    # it: the observe arm must correct nothing at all. Without this the flip
    # could ship a default that has no working way back.
    if results["observe"]["corrected"] != 0:
        failures.append(
            f"observe arm applied {results['observe']['corrected']} correction(s)"
        )
    if results["observe"]["gate"] != "OBSERVE":
        failures.append(
            f"explicit observe selected {results['observe']['gate']}, not OBSERVE"
        )
    # Default policy. Naming the arm is not enough -- a gate label can be wrong
    # about what ran -- so the unset arm must also reproduce the Modern arm's
    # every counter.
    if results["unset"]["gate"] != "MODERN":
        failures.append(
            f"unset MDKR_CAMERA_OBSTRUCTION selected {results['unset']['gate']}, not MODERN"
        )
    if results["unset"] != results["modern"]:
        differing = sorted(
            key for key in results["modern"]
            if results["unset"][key] != results["modern"][key]
        )
        failures.append(
            "unset arm diverged from the explicit modern arm: " + ", ".join(
                f"{key} {results['unset'][key]} vs {results['modern'][key]}"
                for key in differing
            )
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
