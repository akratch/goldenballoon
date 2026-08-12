#!/usr/bin/env python3
"""Exact-replay every ROM-legal standard-track/player-vehicle pairing."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import sys

from check_rollback_track_matrix import run_track
from check_vehicle_sweep import MAIN_TRACK_IDS, VEHICLE_NAMES, rom_vehicle_matrix
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
AUTHORED_TICKS = 179


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--tracks", help="comma-separated standard-track subset")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 16:
        parser.error("--jobs must be in 1..16")

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"),
                        (SCRIPT, "input script")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")
    catalog = rom_vehicle_matrix(str(rom))
    tracks = MAIN_TRACK_IDS if args.tracks is None else [
        int(value) for value in args.tracks.split(",") if value
    ]
    if not tracks or len(set(tracks)) != len(tracks) or any(
            level not in MAIN_TRACK_IDS for level in tracks):
        parser.error("--tracks must be unique standard track ids")
    combinations = [
        (level, vehicle)
        for level in tracks
        for vehicle in sorted(VEHICLE_NAMES)
        if catalog[level][1] & (1 << vehicle)
    ]
    if args.tracks is None and len(combinations) != 47:
        parser.error(
            f"ROM legal standard-track/vehicle corpus changed: "
            f"{len(combinations)} rows (expected 47)"
        )

    print(
        f"check_rollback_vehicle_matrix: rows={len(combinations)} "
        f"jobs={args.jobs}", flush=True,
    )
    results: dict[tuple[int, int], dict[str, object]] = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_track, binary, rom, level, vehicle, args.timeout, SCRIPT,
                AUTHORED_TICKS,
            ): (level, vehicle)
            for level, vehicle in combinations
        }
        for future in as_completed(futures):
            result = future.result()
            key = (int(result["level"]), int(result["vehicle"]))
            results[key] = result
            tag = "ok" if result["ok"] else "FAIL"
            print(
                f"  level {key[0]:<3} {VEHICLE_NAMES[key[1]]:<10} {tag:<4} "
                f"snapshot={result['snapshot']} "
                f"p99={result['capture_p99']}/{result['restore_p99']}/"
                f"{result['resim_p99']}/{result['frame_p99']} ns",
                flush=True,
            )

    failed = [results[key] for key in combinations if not results[key]["ok"]]
    if failed:
        for result in failed:
            print(
                f"FAIL level {result['level']} vehicle {result['vehicle']}: "
                f"{result['why']}", file=sys.stderr,
            )
            if args.verbose:
                print(str(result["output"])[-16000:], file=sys.stderr)
        return 1
    max_snapshot = max(int(result["snapshot"]) for result in results.values())
    max_capture = max(int(result["capture_p99"]) for result in results.values())
    max_restore = max(int(result["restore_p99"]) for result in results.values())
    max_resim = max(int(result["resim_p99"]) for result in results.values())
    max_frame = max(int(result["frame_p99"]) for result in results.values())
    print(
        "PASS rollback legal-vehicle matrix: "
        f"rows={len(combinations)} exactReplays={len(combinations)} "
        f"maxSnapshot={max_snapshot} maxCaptureP99Ns={max_capture} "
        f"maxRestoreP99Ns={max_restore} maxResimP99Ns={max_resim} "
        f"maxFrameP99Ns={max_frame}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
