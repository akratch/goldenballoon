#!/usr/bin/env python3
"""Correct and exact-replay a human-driven car on every standard track."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from check_vehicle_sweep import MAIN_TRACK_IDS, VEHICLE_NAMES
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/nav_to_time_trial_race.txt"
PROCESS_TICKS = 2800
AUTHORED_TICKS = 180
READY_RE = re.compile(
    r"^\[ROLLBACK\] lab ready: ranges=(\d+) snapshot=(\d+) bytes "
    r"ring=(\d+) bytes target=120 epoch=0$", re.MULTILINE,
)
RACE_RE = re.compile(
    r"^\[ROLLBACK\] lab race: loadedTrack=(\d+) raceType=(\d+) "
    r"authoredHz=(\d+)$", re.MULTILINE,
)
STATS_RE = re.compile(
    r"^\[ROLLBACK\] lab stats: ticks=(\d+) captures=(\d+) restores=(\d+) "
    r"capture_avg_ns=(\d+) capture_p50_ns=(\d+) capture_p95_ns=(\d+) "
    r"capture_p99_ns=(\d+) capture_max_ns=(\d+) restore_avg_ns=(\d+) "
    r"restore_p50_ns=(\d+) restore_p95_ns=(\d+) restore_p99_ns=(\d+) "
    r"restore_max_ns=(\d+) timing_overflow=(\d+)/(\d+) "
    r"over_8333333ns=(\d+)/(\d+) over_16666667ns=(\d+)/(\d+)$",
    re.MULTILINE,
)
RESIM_RE = re.compile(
    r"^\[ROLLBACK\] resimulation stats: samples=(\d+) avg_ns=(\d+) "
    r"p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+) "
    r"timing_overflow=(\d+) over_8333333ns=(\d+) "
    r"over_16666667ns=(\d+)$", re.MULTILINE,
)
FRAME_RE = re.compile(
    r"^\[ROLLBACK\] authored-frame stats: samples=(\d+) avg_ns=(\d+) "
    r"p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+) max_ns=(\d+) "
    r"timing_overflow=(\d+) over_8333333ns=(\d+) "
    r"over_16666667ns=(\d+)$", re.MULTILINE,
)
CORRECTION = (
    "[ROLLBACK] delayed-input correction passed ticks=117..120 depth=4 "
    "non_input_divergence=1 exact_replay=1"
)
PVEH_RE = re.compile(r"\[PVEH\] frame=\d+ player=0 vehicleID=(-?\d+)")
VEHICLE_LOOPDELOOP = 4


def run_track(binary: Path, rom: Path, level: int, vehicle: int,
              timeout: int, script: Path = SCRIPT,
              expected_authored_ticks: int = AUTHORED_TICKS
              ) -> dict[str, object]:
    with tempfile.TemporaryDirectory(
            prefix=f"mdkr64-rollback-track-{level}-") as temp:
        root = Path(temp)
        (root / "saves").mkdir()
        environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("MDKR", "GE007_"))
        }
        environment.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_DUMP_EVERY="100000",
            MDKR_LOAD_TRACK=f"{level}:{vehicle}",
            MDKR_PRESENT_RATE="original",
            MDKR_RENDERER="gl",
            MDKR_ROLLBACK_LAB="1",
            MDKR_ROLLBACK_LAB_DELAYED_INPUT="1",
            MDKR_ROLLBACK_LAB_ROUNDTRIP="1",
            MDKR_SAVE_DIR=str(root / "saves"),
            MDKR_TEST_SCRIPT_ONLY_INPUT="1",
            MDKR_TRACE_VEHICLE="1",
            MDKR64_HIDDEN="1",
        )
        try:
            process = subprocess.run(
                [
                    str(binary), "--rom", str(rom),
                    "--headless-ticks", str(PROCESS_TICKS),
                    "--input-script", str(script),
                    "--window-size", "320x240",
                ],
                cwd=root, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=timeout, check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return {"level": level, "vehicle": vehicle, "ok": False,
                    "why": str(error), "output": ""}

    output = process.stdout or ""
    failures: list[str] = []
    if process.returncode != 0:
        failures.append(f"exit={process.returncode}")
    race_rows = [tuple(map(int, row)) for row in RACE_RE.findall(output)]
    if race_rows != [(level, 0, 30)]:
        failures.append(
            f"loaded race={race_rows!r}, expected={(level, 0, 30)!r}"
        )
    seen_vehicles = {int(value) for value in PVEH_RE.findall(output)}
    if vehicle not in seen_vehicles or any(
            value not in (vehicle, VEHICLE_LOOPDELOOP)
            for value in seen_vehicles):
        failures.append(
            f"vehicle dispatch={sorted(seen_vehicles)!r}, expected={vehicle}"
        )
    ready = [tuple(map(int, row)) for row in READY_RE.findall(output)]
    if len(ready) != 1:
        failures.append(f"ready={ready!r}")
        ready_row = (0, 0, 0)
    else:
        ready_row = ready[0]
        if ready_row[0] < 130 or ready_row[1] == 0 or \
                ready_row[2] != ready_row[1] * 32 or \
                ready_row[2] > 16 * 1024 * 1024:
            failures.append(f"authority-shape={ready_row!r}")
    stats = [tuple(map(int, row)) for row in STATS_RE.findall(output)]
    if len(stats) != 1:
        failures.append(f"stats={stats!r}")
        stats_row = (0,) * 19
    else:
        stats_row = stats[0]
        if stats_row[:3] != (
                expected_authored_ticks, expected_authored_ticks + 9, 3):
            failures.append(f"boundaries={stats_row[:3]!r}")
        if not (stats_row[4] <= stats_row[5] <= stats_row[6]) or \
                not (stats_row[9] <= stats_row[10] <= stats_row[11]) or \
                any(stats_row[13:19]):
            failures.append("timing histogram/budget failed")
    resim = [tuple(map(int, row)) for row in RESIM_RE.findall(output)]
    if len(resim) != 1:
        failures.append(f"resimulation-stats={resim!r}")
        resim_row = (0,) * 9
    else:
        resim_row = resim[0]
        if resim_row[0] != 8 or resim_row[1] == 0 or not (
                resim_row[2] <= resim_row[3] <= resim_row[4]) or \
                resim_row[5] == 0 or any(resim_row[6:]):
            failures.append(f"resimulation-budget={resim_row!r}")
    frame = [tuple(map(int, row)) for row in FRAME_RE.findall(output)]
    if len(frame) != 1:
        failures.append(f"authored-frame-stats={frame!r}")
        frame_row = (0,) * 9
    else:
        frame_row = frame[0]
        if frame_row[0] != expected_authored_ticks - 1 or \
                frame_row[1] == 0 or not (
                    frame_row[2] <= frame_row[3] <= frame_row[4]) or \
                frame_row[4] > 8_333_333 or frame_row[5] == 0 or \
                frame_row[6] != 0 or frame_row[8] != 0:
            failures.append(f"authored-frame-budget={frame_row!r}")
    required = (
        CORRECTION,
        "[ROLLBACK] first-boundary restore roundtrip passed tick=1",
        "[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0",
    )
    if any(output.count(marker) != 1 for marker in required):
        failures.append("missing exact replay/teardown witness")
    forbidden = (
        "[FATAL]", "[CRASH]", "AddressSanitizer", "overflow=1",
        "overflows=1", "forbidden_io=1", "simulation witness mismatch",
    )
    if any(marker in output for marker in forbidden):
        failures.append("forbidden diagnostic")
    return {
        "level": level,
        "vehicle": vehicle,
        "ok": not failures,
        "why": "; ".join(failures),
        "ranges": ready_row[0],
        "snapshot": ready_row[1],
        "ring": ready_row[2],
        "capture_p99": stats_row[6],
        "restore_p99": stats_row[11],
        "resim_p99": resim_row[4],
        "frame_p99": frame_row[4],
        "vehicle_seen": tuple(sorted(seen_vehicles)),
        "output": output,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--tracks", help="comma-separated subset of level ids")
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
    tracks = MAIN_TRACK_IDS if args.tracks is None else [
        int(value) for value in args.tracks.split(",") if value
    ]
    if not tracks or len(set(tracks)) != len(tracks) or any(
            level not in MAIN_TRACK_IDS for level in tracks):
        parser.error("--tracks must be unique standard track ids")
    print(
        f"check_rollback_track_matrix: {len(tracks)} standard tracks, "
        f"{AUTHORED_TICKS} authored ticks each, jobs={args.jobs}",
        flush=True,
    )
    results: dict[int, dict[str, object]] = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_track, binary, rom, level, 0, args.timeout,
            ): level
            for level in tracks
        }
        for future in as_completed(futures):
            result = future.result()
            results[int(result["level"])] = result
            tag = "ok" if result["ok"] else "FAIL"
            print(
                f"  level {result['level']:<3} "
                f"{VEHICLE_NAMES[int(result['vehicle'])]:<10} {tag:<4} "
                f"ranges={result['ranges']} snapshot={result['snapshot']} "
                f"p99={result['capture_p99']}/{result['restore_p99']}/"
                f"{result['resim_p99']}/{result['frame_p99']} ns",
                flush=True,
            )

    failed = [results[level] for level in tracks if not results[level]["ok"]]
    if failed:
        for result in failed:
            print(
                f"FAIL level {result['level']}: {result['why']}",
                file=sys.stderr,
            )
            if args.verbose:
                print(str(result["output"])[-16000:], file=sys.stderr)
        return 1
    max_snapshot = max(int(results[level]["snapshot"]) for level in tracks)
    max_capture = max(int(results[level]["capture_p99"]) for level in tracks)
    max_restore = max(int(results[level]["restore_p99"]) for level in tracks)
    max_resim = max(int(results[level]["resim_p99"]) for level in tracks)
    max_frame = max(int(results[level]["frame_p99"]) for level in tracks)
    print(
        "PASS rollback standard-track matrix: "
        f"tracks={len(tracks)} exactReplays={len(tracks)} "
        f"maxSnapshot={max_snapshot} maxCaptureP99Ns={max_capture} "
        f"maxRestoreP99Ns={max_restore} maxResimP99Ns={max_resim} "
        f"maxFrameP99Ns={max_frame}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
