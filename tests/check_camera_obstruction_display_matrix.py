#!/usr/bin/env python3
"""Projection-shape matrix for the Modern camera obstruction runtime."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from harness_utils import resolve_binary
from check_camera_obstruction_runtime import inspect, run


MATRIX = (
    ("4:3-low", "320x240"),
    ("4:3-high", "1280x960"),
    ("16:9", "1920x1080"),
    ("21:9", "2560x1080"),
    ("32:9", "5120x1440"),
    ("portrait", "1080x1920"),
)
FOV_MATRIX = (
    ("authored", None, "authored"),
    ("20", None, "20"),
    ("140", None, "140-capped"),
    ("140", "off", "140-uncapped"),
)
DETAIL = "camera_obstruction_observe detail"


def resolved_trace(output: str) -> list[str]:
    values = []
    for row in output.splitlines():
        if DETAIL not in row:
            continue
        match = re.search(r"\bresolved=\(([^)]+)\)", row)
        if match is None:
            raise RuntimeError("missing resolved pose")
        values.append(match.group(1))
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=3600)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    for path in (binary, args.rom):
        if not Path(path).is_file():
            raise SystemExit(f"missing: {path}")

    traces = {}
    failures = []
    corrected_by_fov = {}
    for fov, max_hfov, fov_label in FOV_MATRIX:
        corrected_by_fov[fov_label] = 0
        for label, window in MATRIX:
            case = f"{label}-fov-{fov_label}"
            output = run(
                binary, args.rom, "modern", args.frames, args.timeout,
                window, fov, max_hfov)
            result = inspect(case, output)
            corrected_by_fov[fov_label] += result["corrected"]
            traces[case] = resolved_trace(output)
            if result["penetrated"] != 0:
                failures.append(
                    f"{case} published {result['penetrated']} penetrated poses")
            print(f"  {label:9s} {window:10s} fov={fov_label:12s} {result}")

        low = f"4:3-low-fov-{fov_label}"
        high = f"4:3-high-fov-{fov_label}"
        if traces[low] != traces[high]:
            failures.append(
                "equal-aspect 320x240 and 1280x960 camera traces differ "
                f"at FOV {fov_label}"
            )
    # A small lens footprint can legitimately remain clear for the whole
    # route. Authored and stress bands must collectively exercise correction;
    # every individual case is still gated by inspect() for exact-query use
    # and zero unsafe publication.
    for fov_label in ("authored", "140-capped", "140-uncapped"):
        if corrected_by_fov[fov_label] == 0:
            failures.append(f"FOV band {fov_label} never exercised correction")
    if failures:
        print("check_camera_obstruction_display_matrix: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_camera_obstruction_display_matrix: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
