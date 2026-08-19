#!/usr/bin/env python3
"""Use every standard-race balloon level inside a corrected rollback window."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from check_rollback_track_matrix import FRAME_RE, RESIM_RE, STATS_RE
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/nav_to_time_trial_race.txt"
PROCESS_TICKS = 2800
AUTHORED_TICKS = 180
TARGET_TICK = 150
RELEASE_TICK = TARGET_TICK - 3
BALLOON_NAMES = ("boost", "missile", "trap", "shield", "magnet")
ROWS = tuple((balloon, level) for balloon in range(5) for level in range(3))

ARM_RE = re.compile(
    r"^\[ROLLBACK\] item probe armed: balloon=(\d+) level=(\d+) "
    r"weapon=(-?\d+) quantity=1 release=(\d+) mutation=(\d+)$",
    re.MULTILINE,
)
RESULT_RE = re.compile(
    r"^\[ROLLBACK\] item probe result: balloon=(\d+) level=(\d+) "
    r"weapon=(-?\d+) quantity=(-?\d+) spawns=(\d+) rumble=(\d+) "
    r"boost=(-?\d+) shield=(-?\d+) shieldType=(-?\d+) observed=(\d+)$",
    re.MULTILINE,
)
READY_RE = re.compile(
    r"^\[ROLLBACK\] lab ready: ranges=(\d+) snapshot=(\d+) bytes "
    r"ring=(\d+) bytes target=(\d+) epoch=0$",
    re.MULTILINE,
)
EFFECTS_RE = re.compile(
    r"^\[ROLLBACK\] effects: tracked=(\d+) emitted=(\d+) "
    r"duplicates=(\d+) committed=(\d+) cancelled=(\d+) "
    r"overflows=(\d+) forbidden_io=(\d+)$",
    re.MULTILINE,
)
CORRECTION = (
    "[ROLLBACK] delayed-input correction passed ticks=147..150 depth=4 "
    "non_input_divergence=1 exact_replay=1"
)


def environment(root: Path, balloon: int | str, level: int,
                mutation: bool = False, delayed: bool = True
                ) -> dict[str, str]:
    values = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    values.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_DUMP_EVERY="100000",
        MDKR_LOAD_TRACK="8:0",
        MDKR_PRESENT_RATE="original",
        MDKR_RENDERER="gl",
        MDKR_ROLLBACK_LAB="1",
        MDKR_ROLLBACK_LAB_ROUNDTRIP="1",
        MDKR_ROLLBACK_LAB_TARGET_TICK=str(TARGET_TICK),
        MDKR_ROLLBACK_LAB_ITEM_BALLOON=str(balloon),
        MDKR_ROLLBACK_LAB_ITEM_LEVEL=str(level),
        MDKR_ROLLBACK_LAB_ITEM_MUTATION_CONTROL="1" if mutation else "0",
        MDKR_SAVE_DIR=str(root / "saves"),
        # Isolate the video config with the save (see check_door_blocks.py).
        MDKR_VIDEO_CONFIG_PATH=str(root / "saves" / "video.ini"),
        MDKR_TEST_SCRIPT_ONLY_INPUT="1",
        MDKR64_HIDDEN="1",
    )
    if delayed:
        values["MDKR_ROLLBACK_LAB_DELAYED_INPUT"] = "1"
    return values


def invoke(binary: Path, rom: Path, balloon: int | str, level: int,
           timeout: int, mutation: bool = False, delayed: bool = True
           ) -> tuple[int, str]:
    with tempfile.TemporaryDirectory(prefix="mdkr64-rollback-item-") as temp:
        root = Path(temp)
        (root / "saves").mkdir()
        try:
            process = subprocess.run(
                [
                    str(binary), "--rom", str(rom),
                    "--headless-ticks", str(PROCESS_TICKS),
                    "--input-script", str(SCRIPT),
                    "--window-size", "320x240",
                ],
                cwd=root,
                env=environment(root, balloon, level, mutation, delayed),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            return 255, str(error)
    return process.returncode, process.stdout or ""


def check_timing(output: str) -> list[str]:
    failures: list[str] = []
    ready = [tuple(map(int, row)) for row in READY_RE.findall(output)]
    if len(ready) != 1:
        failures.append(f"ready={ready!r}")
    else:
        ranges, snapshot, ring, target = ready[0]
        if ranges < 140 or snapshot == 0 or ring != snapshot * 32 or \
                ring > 16 * 1024 * 1024 or target != TARGET_TICK:
            failures.append(f"authority-shape={ready[0]!r}")
    stats = [tuple(map(int, row)) for row in STATS_RE.findall(output)]
    if len(stats) != 1:
        failures.append(f"stats={stats!r}")
    else:
        row = stats[0]
        if row[:3] != (AUTHORED_TICKS, AUTHORED_TICKS + 9, 3) or \
                not (row[4] <= row[5] <= row[6]) or \
                not (row[9] <= row[10] <= row[11]) or any(row[13:19]):
            failures.append(f"capture-restore-budget={row!r}")
    resim = [tuple(map(int, row)) for row in RESIM_RE.findall(output)]
    if len(resim) != 1 or resim[0][0] != 8 or \
            not (resim[0][2] <= resim[0][3] <= resim[0][4]) or \
            resim[0][5] == 0 or any(resim[0][6:]):
        failures.append(f"resimulation-budget={resim!r}")
    frame = [tuple(map(int, row)) for row in FRAME_RE.findall(output)]
    if len(frame) != 1 or frame[0][0] != AUTHORED_TICKS - 1 or \
            not (frame[0][2] <= frame[0][3] <= frame[0][4]) or \
            frame[0][4] > 8_333_333 or frame[0][5] == 0 or \
            frame[0][6] != 0 or frame[0][8] != 0:
        failures.append(f"authored-frame-budget={frame!r}")
    effects = [tuple(map(int, row)) for row in EFFECTS_RE.findall(output)]
    if len(effects) != 1 or effects[0][1] == 0 or effects[0][5:] != (0, 0):
        failures.append(f"effect-journal={effects!r}")
    return failures


def run_row(binary: Path, rom: Path, balloon: int, level: int,
            timeout: int) -> dict[str, object]:
    code, output = invoke(binary, rom, balloon, level, timeout)
    failures: list[str] = []
    arms = [tuple(map(int, row)) for row in ARM_RE.findall(output)]
    results = [tuple(map(int, row)) for row in RESULT_RE.findall(output)]
    if code != 0:
        failures.append(f"exit={code}")
    if len(arms) != 1 or arms[0][:2] != (balloon, level) or \
            arms[0][3:] != (RELEASE_TICK, 0):
        failures.append(f"arm={arms!r}")
    if len(results) != 1 or results[0][:2] != (balloon, level) or \
            results[0][3] != 0 or results[0][-1] != 1:
        failures.append(f"result={results!r}")
    if output.count(CORRECTION) != 1 or output.count(
            "[ROLLBACK] first-boundary restore roundtrip passed tick=1") != 1:
        failures.append("missing exact correction/roundtrip witness")
    if output.count("[HOST-SHUTDOWN] rom=0 arena=0 delayedFree=0") != 1:
        failures.append("missing exact clean teardown")
    failures.extend(check_timing(output))
    forbidden = (
        "[FATAL]", "[CRASH]", "AddressSanitizer", "overflow=1",
        "overflows=1", "forbidden_io=1", "observed=0",
        "corrected item breadth was not observed",
    )
    if any(marker in output for marker in forbidden):
        failures.append("forbidden diagnostic")
    weapon = arms[0][2] if len(arms) == 1 else -1
    return {
        "balloon": balloon,
        "level": level,
        "weapon": weapon,
        "ok": not failures,
        "why": "; ".join(failures),
        "output": output,
    }


def mapping_failure(results: dict[tuple[int, int], dict[str, object]]) -> str:
    weapons = {
        key: int(result["weapon"]) for key, result in results.items()
    }
    expected = {
        0: (4, 8, 15),
        1: (1, 0, 1),
        2: (3, 2, 10),
        3: (12, 13, 14),
        4: (5, 7, 6),
    }
    for balloon, expected_weapons in expected.items():
        actual = tuple(weapons[(balloon, level)] for level in range(3))
        if actual != expected_weapons:
            return (
                f"{BALLOON_NAMES[balloon]} mapping changed: "
                f"expected={expected_weapons!r} actual={actual!r}"
            )
    # Missile L1 and L3 intentionally map to the same standard rocket. The
    # matrix covers 15 inventory configurations and 14 concrete weapon IDs.
    if len(set(weapons.values())) != 14:
        return f"expected 14 concrete weapons across 15 configurations: {weapons!r}"
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 15:
        parser.error("--jobs must be in 1..15")
    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"),
                        (SCRIPT, "input script")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    print(
        f"check_rollback_item_matrix: rows={len(ROWS)} target={TARGET_TICK} "
        f"jobs={args.jobs}", flush=True,
    )
    results: dict[tuple[int, int], dict[str, object]] = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(
                run_row, binary, rom, balloon, level, args.timeout,
            ): (balloon, level)
            for balloon, level in ROWS
        }
        for future in as_completed(futures):
            key = futures[future]
            result = future.result()
            results[key] = result
            print(
                f"  {BALLOON_NAMES[key[0]]:<7} L{key[1] + 1} "
                f"weapon={result['weapon']:<2} "
                f"{'ok' if result['ok'] else 'FAIL'}",
                flush=True,
            )

    failures = [results[key] for key in ROWS if not results[key]["ok"]]
    mapping = mapping_failure(results)
    if mapping:
        print(f"FAIL rollback item matrix: {mapping}", file=sys.stderr)
    for result in failures:
        print(
            f"FAIL {BALLOON_NAMES[int(result['balloon'])]} "
            f"L{int(result['level']) + 1}: {result['why']}",
            file=sys.stderr,
        )
        if args.verbose:
            print(str(result["output"])[-16000:], file=sys.stderr)
    if failures or mapping:
        return 1

    # Non-vacuity: the exact same granted item must fail when the corrected
    # Z-up edge is suppressed. This is the broken-direction half of the probe.
    code, output = invoke(binary, rom, 0, 0, args.timeout, mutation=True)
    mutation_results = [
        tuple(map(int, row)) for row in RESULT_RE.findall(output)
    ]
    if code == 0 or len(mutation_results) != 1 or \
            mutation_results[0][-1] != 0 or \
            "corrected item breadth was not observed" not in output:
        print("FAIL rollback item matrix: suppressed-release mutation passed",
              file=sys.stderr)
        if args.verbose:
            print(output[-16000:], file=sys.stderr)
        return 1

    # Bounds and mode controls: an invalid balloon index resolves to disabled
    # and never indexes ROM data; an armed probe without delayed correction is
    # rejected before the registry is frozen.
    code, output = invoke(binary, rom, "7", 0, args.timeout)
    if code != 0 or "[ENV] MDKR_ROLLBACK_LAB_ITEM_BALLOON=7 is invalid" not in output or \
            "item probe armed" in output or "item probe result" in output:
        print("FAIL rollback item matrix: invalid index did not fail closed",
              file=sys.stderr)
        return 1
    code, output = invoke(binary, rom, 0, 0, args.timeout, delayed=False)
    if code == 0 or "item probe rejected: requires a local delayed-input lab" not in output or \
            "item probe armed" in output:
        print("FAIL rollback item matrix: non-correction probe was admitted",
              file=sys.stderr)
        return 1

    ordered = ",".join(
        f"{BALLOON_NAMES[balloon]}{level + 1}="
        f"{int(results[(balloon, level)]['weapon'])}"
        for balloon, level in ROWS
    )
    print(
        "PASS rollback item matrix: "
        f"rows={len(ROWS)} distinctWeapons=14 correction={RELEASE_TICK}.."
        f"{TARGET_TICK} mutationRejected=1 boundsRejected=1 modesRejected=1 "
        f"mapping={ordered}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
