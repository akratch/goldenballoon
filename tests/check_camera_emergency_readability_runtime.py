#!/usr/bin/env python3
"""Trigger and validate the per-viewport emergency racer readability path."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from check_camera_obstruction_runtime import DETAIL, field, inspect, run
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=5200)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    for path in (binary, args.rom):
        if not Path(path).is_file():
            raise SystemExit(f"missing: {path}")

    try:
        output = run(
            binary, args.rom, "modern", args.frames, args.timeout,
            extra_env={"MDKR_TEST_CAMERA_DISABLE_ALTERNATE": "1"},
        )
        result = inspect("modern-emergency-readability", output)
        details = [row for row in output.splitlines() if DETAIL in row]
        faded = [row for row in details if field(row, "racer_opacity") < 255]
    except RuntimeError as error:
        print(f"check_camera_emergency_readability_runtime: FAIL\n  - {error}",
              file=sys.stderr)
        return 1

    failures: list[str] = []
    if not faded:
        failures.append("stress arm never triggered emergency racer opacity")
    if any(field(row, "corrected") != 1 for row in faded):
        failures.append("racer opacity changed without a corrected camera pose")
    opacities = [field(row, "racer_opacity") for row in faded]
    if opacities and min(opacities) < 96:
        failures.append(f"opacity escaped the 96..254 envelope: {min(opacities)}..{max(opacities)}")
    # The upper end of the envelope has to be read from the unfiltered trace:
    # `faded` is defined as opacity < 255, so its own maximum can never witness
    # a violation. What the trace must show is that the fade has a full-opacity
    # resting state to return to, and that nothing ever exceeds it.
    sampled = [field(row, "racer_opacity") for row in details]
    if max(sampled, default=0) > 255:
        failures.append(f"opacity exceeded full opacity: {max(sampled)}")
    if 255 not in sampled:
        failures.append("no full-opacity sample: the fade never rests at 255")
    if (result["penetrated"] or result["degraded"] or result["invalid"] or
            result["target_hidden"]):
        failures.append(f"readability stress compromised geometric safety: {result}")

    print(
        f"  faded_rows={len(faded)} opacity="
        f"{min(opacities) if opacities else 'n/a'}.."
        f"{max(opacities) if opacities else 'n/a'} result={result}"
    )
    if failures:
        print("check_camera_emergency_readability_runtime: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_camera_emergency_readability_runtime: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
