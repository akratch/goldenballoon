#!/usr/bin/env python3
"""Run the current 32-slot rollback ring for 10,180 Whale Bay race ticks."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
# The explicit Time Trial route enters the authored race one process tick later
# than the Tracks-menu route. Keep the same 10,180-tick race soak while making
# the requested player vehicle authoritative and observable.
PROCESS_TICKS = 12801
AUTHORED_TICKS = 10180
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
READY_RE = re.compile(
    r"^\[ROLLBACK\] lab ready: ranges=(\d+) snapshot=(\d+) bytes "
    r"ring=(\d+) bytes target=120 epoch=0$",
    re.MULTILINE,
)
RACE_RE = re.compile(
    r"^\[ROLLBACK\] lab race: loadedTrack=(\d+) raceType=(\d+) "
    r"authoredHz=(\d+)$", re.MULTILINE,
)
EFFECTS_RE = re.compile(
    r"^\[ROLLBACK\] effects: tracked=(\d+) emitted=(\d+) duplicates=(\d+) "
    r"committed=(\d+) cancelled=(\d+) overflows=(\d+) forbidden_io=(\d+)$",
    re.MULTILINE,
)
PVEH_RE = re.compile(r"\[PVEH\] frame=\d+ player=0 vehicleID=(-?\d+)")


def fail(message: str, output: str) -> int:
    print(f"FAIL rollback soak: {message}", file=sys.stderr)
    print(output[-16000:], file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"),
                        (SCRIPT, "input script")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-rollback-soak-") as temp:
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
            MDKR_LOAD_TRACK="8:1",
            MDKR_PRESENT_RATE="original",
            MDKR_RENDERER="gl",
            MDKR_ROLLBACK_LAB="1",
            MDKR_ROLLBACK_LAB_DELAYED_INPUT="1",
            MDKR_ROLLBACK_LAB_ROUNDTRIP="1",
            MDKR_SAVE_DIR=str(root / "saves"),
            # Isolate the video config with the save (see check_door_blocks.py).
            MDKR_VIDEO_CONFIG_PATH=str(root / "saves" / "video.ini"),
            MDKR_TEST_SCRIPT_ONLY_INPUT="1",
            MDKR_TRACE_VEHICLE="1",
            MDKR64_HIDDEN="1",
        )
        command = [
            str(binary), "--rom", str(rom),
            "--headless-ticks", str(PROCESS_TICKS),
            "--input-script", str(SCRIPT),
            "--window-size", "320x240",
        ]
        if args.verbose:
            print("$ " + " ".join(command), flush=True)
        try:
            process = subprocess.run(
                command, cwd=root, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return fail(f"could not complete soak: {error}", "")

    output = process.stdout or ""
    if process.returncode != 0:
        return fail(f"application exited {process.returncode}", output)
    seen_vehicles = {int(value) for value in PVEH_RE.findall(output)}
    if seen_vehicles != {1}:
        return fail(
            f"actual player vehicle was {sorted(seen_vehicles)!r}, "
            "expected hovercraft (1)", output,
        )
    race_rows = [tuple(map(int, row)) for row in RACE_RE.findall(output)]
    if race_rows != [(8, 0, 30)]:
        return fail(
            f"actual loaded race was {race_rows!r}, expected Whale Bay "
            "standard NTSC authored cadence", output,
        )
    ready = [tuple(map(int, row)) for row in READY_RE.findall(output)]
    if len(ready) != 1:
        return fail(f"expected one current-ring admission, got {ready!r}", output)
    ranges, snapshot_bytes, ring_bytes = ready[0]
    if ranges < 140 or snapshot_bytes == 0 or ring_bytes != snapshot_bytes * 32:
        return fail(f"wrong authority/ring shape {ready[0]!r}", output)
    if ring_bytes > 16 * 1024 * 1024:
        return fail(f"ring exceeded 12 MiB cap: {ring_bytes}", output)

    stats = [tuple(map(int, row)) for row in STATS_RE.findall(output)]
    if len(stats) != 1:
        return fail(f"expected one timing summary, got {stats!r}", output)
    row = stats[0]
    ticks, captures, restores = row[:3]
    capture_avg, capture_p50, capture_p95, capture_p99, capture_max = row[3:8]
    restore_avg, restore_p50, restore_p95, restore_p99, restore_max = row[8:13]
    if ticks != AUTHORED_TICKS or captures != AUTHORED_TICKS + 9 or restores != 3:
        return fail(
            f"wrong soak boundaries captures/restores {row[:3]!r}", output,
        )
    if capture_avg == 0 or not (capture_p50 <= capture_p95 <= capture_p99) or \
            capture_max == 0 or restore_avg == 0 or not (
                restore_p50 <= restore_p95 <= restore_p99) or restore_max == 0:
        return fail(f"invalid timing distribution {row!r}", output)
    if any(row[13:19]):
        return fail(f"timing budget overflow/violation {row[13:19]!r}", output)

    resim = [tuple(map(int, row)) for row in RESIM_RE.findall(output)]
    if len(resim) != 1:
        return fail(f"expected one resimulation summary, got {resim!r}", output)
    resim_row = resim[0]
    if resim_row[0] != 8 or resim_row[1] == 0 or not (
            resim_row[2] <= resim_row[3] <= resim_row[4]) or \
            resim_row[5] == 0 or any(resim_row[6:]):
        return fail(f"invalid resimulation budget {resim_row!r}", output)
    frame = [tuple(map(int, row)) for row in FRAME_RE.findall(output)]
    if len(frame) != 1:
        return fail(f"expected one authored-frame summary, got {frame!r}", output)
    frame_row = frame[0]
    if frame_row[0] != AUTHORED_TICKS - 1 or frame_row[1] == 0 or not (
            frame_row[2] <= frame_row[3] <= frame_row[4]) or \
            frame_row[4] > 8_333_333 or frame_row[5] == 0 or \
            frame_row[6] != 0 or frame_row[8] != 0:
        return fail(f"invalid authored-frame budget {frame_row!r}", output)

    effects = [tuple(map(int, row)) for row in EFFECTS_RE.findall(output)]
    if len(effects) != 1 or effects[0][1] == 0 or effects[0][5:] != (0, 0):
        return fail(f"unsafe/vacuous effect journal {effects!r}", output)
    correction = (
        "[ROLLBACK] delayed-input correction passed ticks=117..120 depth=4 "
        "non_input_divergence=1 exact_replay=1"
    )
    required = (
        correction,
        "[ROLLBACK] first-boundary restore roundtrip passed tick=1",
        f"[SDL] headless: reached {PROCESS_TICKS} simulation ticks, exiting cleanly.",
        "[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0",
    )
    if any(output.count(marker) != 1 for marker in required):
        return fail("missing exact correction/exit/teardown witness", output)
    forbidden = (
        "[FATAL]", "[CRASH]", "AddressSanitizer", "overflow=1",
        "overflows=1", "forbidden_io=1", "simulation witness mismatch",
    )
    if any(marker in output for marker in forbidden):
        return fail("observed forbidden diagnostic", output)
    if args.verbose:
        print(output, end="" if output.endswith("\n") else "\n")
    print(
        "PASS rollback soak: track=Whale-Bay vehicle=hovercraft "
        f"authoredTicks={ticks} captures={captures} restores={restores} "
        f"snapshot={snapshot_bytes} ring={ring_bytes} "
        f"captureP99Ns={capture_p99} restoreP99Ns={restore_p99} "
        f"resimP99Ns={resim_row[4]} frameP99Ns={frame_row[4]}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
