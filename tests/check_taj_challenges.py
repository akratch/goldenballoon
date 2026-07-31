#!/usr/bin/env python3
"""End-to-end coverage for all three Taj vehicle challenges.

First-time arms begin from checksum-valid Adventure saves immediately before
the ROM-authored 5/10/18-balloon offer thresholds. They require the real Timber's
Island offer, Taj dialogue/vehicle transformation, carpet spawn, three-lap race,
result dialogue, completion bit, EEPROM checksum, and a second-process reload.

For each car/hovercraft/plane challenge the matrix covers:

* first win: natural autopilot laps and first completion;
* loss: one carpet final-lap progress event, with production assigning places;
* abort: production CHALLENGE_END_QUIT teardown;
* replay: already-completed state through the challenge-accept seam;
* positive control: natural win with only the completion predicate disabled.

The native driver never writes the human's lap, raceFinished, finishPosition,
Taj flags, result menu, or save data. Replay is the only arm that skips the
offer dialogue, because a completed challenge is not auto-offered; its narrow
request enters at init_racer_for_challenge() with the target vehicle already
loaded. All first-time arms exercise the visible player path.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from check_adventure_two import eeprom_image
from check_race_drive import scene_metrics
from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
BASE_SCRIPT = ROOT / "tests/input_scripts/adventure_resume_race.txt"
FRAMES = 12000
ABORT_FRAMES = 4200
RELOAD_FRAMES = 2300

# Progress floors for the race-row assertion (ship review, G5). MEASURED on
# this branch, not guessed -- MDKR_TAJ_REPORT_LAPS=1 reprints the per-arm
# figures. Measured over all fifteen arms, Release build:
#
#   lap reached, win arms   human [2]x9   ai [2]x9
#   lap reached, loss arms  human [2]x3   ai [4]x3
#   lap reached, abort arms human [0]x3   ai [0]x3   (quits mid-race by design)
#   largest step between consecutive samples:
#       win/loss  human 1441..1492   ai 1282..1478
#       abort     human 138..551     ai 100..138
#
# The floors sit one measured unit BELOW those minima rather than on them, so
# they assert the property (a completed circuit; a racer still in motion at the
# end) instead of pinning a phase-sensitive equality: a legitimate physics
# change that moves where the 120-tick samples land must not turn the gate red,
# which is the exact mistake the displacement assertion made before 49b1840.
MIN_HUMAN_LAP = 1        # measured min 2 over the twelve non-abort arms
MIN_AI_LAP = 1           # measured min 2 (loss arms reach 4)
MIN_SAMPLE_STEP = 50.0   # measured min 100 (abort/ai); non-abort min 1282
MEASURED_LAPS: dict[str, list[int]] = {}

VEHICLES = {
    "car": {
        "id": 0, "threshold": 5, "prereq": 0x00,
        "offered": 0x01, "completed": 0x09,
    },
    "hover": {
        "id": 1, "threshold": 10, "prereq": 0x09,
        "offered": 0x0B, "completed": 0x1B,
    },
    "plane": {
        "id": 2, "threshold": 18, "prereq": 0x1B,
        "offered": 0x1F, "completed": 0x3F,
    },
}

TAJ_RE = re.compile(
    r"\[TAJ\] phase=(\w+) vehicle=(-?\d+) flags=0x([0-9a-fA-F]+) "
    r"balloons=(\d+) thresholds=(-?\d+),(-?\d+),(-?\d+) "
    r"racers=(\d+) tick=(\d+) "
    r"human=(-?\d+),(-?\d+),(-?\d+),([^, ]+),([^, ]+),([^, ]+) "
    r"ai=(-?\d+),(-?\d+),(-?\d+),([^, ]+),([^, ]+),([^, ]+) "
    r"reason=(-?\d+) menu=(-?\d+) gate=(\d+)"
)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)


def replace_bits(data: bytearray, offset: int, width: int, value: int) -> None:
    bits = "".join(f"{byte:08b}" for byte in data)
    encoded = f"{value:0{width}b}"
    bits = bits[:offset] + encoded + bits[offset + width:]
    data[:] = bytes(
        int(bits[index:index + 8], 2)
        for index in range(0, len(bits), 8)
    )


def make_eeprom(flags: int, balloons: int) -> bytes:
    payload = bytearray(eeprom_image(False))
    slot = bytearray(payload[:40])
    replace_bits(slot, 16 + 68, 6, flags)
    replace_bits(slot, 16 + 68 + 6 + 10 + 12, 7, balloons)
    slot[:2] = ((5 + sum(slot[2:])) & 0xFFFF).to_bytes(2, "big")
    payload[:40] = slot
    return bytes(payload)


def decode_slot(payload: bytes) -> dict[str, int | bool]:
    slot = payload[:40]
    bits = "".join(f"{byte:08b}" for byte in slot)
    return {
        "checksum_ok": (
            int.from_bytes(slot[:2], "big") ==
            ((5 + sum(slot[2:])) & 0xFFFF)
        ),
        "flags": int(bits[84:90], 2),
        "balloons": int(bits[112:119], 2),
    }


def make_script(path: Path) -> None:
    taps = "".join(f"{frame} A 4\n" for frame in range(2200, FRAMES, 24))
    path.write_text(
        BASE_SCRIPT.read_text() +
        "\n# Advance Taj offer/result dialogue\n" + taps
    )


def parse_rows(output: str) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for match in TAJ_RE.finditer(output):
        values = match.groups()
        rows.append({
            "phase": values[0],
            "vehicle": int(values[1]),
            "flags": int(values[2], 16),
            "balloons": int(values[3]),
            "thresholds": tuple(int(value) for value in values[4:7]),
            "racers": int(values[7]),
            "tick": int(values[8]),
            "human": {
                "lap": int(values[9]),
                "finished": int(values[10]),
                "place": int(values[11]),
                "position": tuple(float(value) for value in values[12:15]),
            },
            "ai": {
                "lap": int(values[15]),
                "finished": int(values[16]),
                "place": int(values[17]),
                "position": tuple(float(value) for value in values[18:21]),
            },
            "reason": int(values[21]),
            "menu": int(values[22]),
            "gate": int(values[23]),
        })
    return rows


def run_arm(
    binary: Path,
    rom: Path,
    name: str,
    outcome: str,
    *,
    replay: bool = False,
    break_completion: bool = False,
    dump_frame: bool = False,
) -> dict[str, object]:
    spec = VEHICLES[name]
    initial_flags = (
        int(spec["completed"]) if replay else int(spec["prereq"])
    )
    frames = ABORT_FRAMES if outcome == "abort" else FRAMES
    with tempfile.TemporaryDirectory(prefix=f"mdkr_taj_{name}_") as temp:
        run_dir = Path(temp)
        (run_dir / "save").mkdir()
        original = make_eeprom(initial_flags, int(spec["threshold"]))
        (run_dir / "save/eeprom.bin").write_bytes(original)
        script = run_dir / "taj.txt"
        make_script(script)
        captures = run_dir / "frames"
        if dump_frame:
            captures.mkdir()

        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("MDKR_")
        }
        env.update(
            MDKR_AUDIO="0",
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
            MDKR_TRACE="1",
            MDKR_TAJ_PROBE="1",
            MDKR_TAJ_OUTCOME=outcome,
            MDKR_AUTOPILOT="1",
            MDKR_RENDER_SCALE="1",
        )
        if replay:
            env["MDKR_TAJ_REQUEST"] = name
            env["MDKR_LOAD_TRACK"] = f"0:{spec['id']}"
        if break_completion:
            env["MDKR_TAJ_BREAK_COMPLETION"] = name
        command = [
            str(binary),
            "--headless-frames", str(frames),
            "--input-script", str(script),
        ]
        if dump_frame:
            env.update(MDKR_DUMP_FROM="3400", MDKR_DUMP_EVERY="99999")
            command.extend(("--dump-frames", str(captures)))
        command.extend(("--rom", str(rom)))
        proc = subprocess.run(
            command,
            cwd=run_dir,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=300,
            check=False,
        )
        persisted = (run_dir / "save/eeprom.bin").read_bytes()

        reload_env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("MDKR_")
        }
        reload_env.update(
            MDKR_AUDIO="0",
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
            MDKR_TRACE="1",
            MDKR_TAJ_PROBE="1",
            MDKR_AUTOPILOT="1",
            MDKR_RENDER_SCALE="1",
        )
        reload_proc = subprocess.run(
            [
                str(binary),
                "--headless-frames", str(RELOAD_FRAMES),
                "--input-script", str(script),
                "--rom", str(rom),
            ],
            cwd=run_dir,
            env=reload_env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=300,
            check=False,
        )
        metrics = [
            (frame.name, *scene_metrics(str(frame)))
            for frame in captures.glob("frame_*.ppm")
        ]
        return {
            "rc": proc.returncode,
            "out": proc.stdout,
            "save": persisted,
            "original": original,
            "reload_rc": reload_proc.returncode,
            "reload_out": reload_proc.stdout,
            "metrics": metrics,
            "dump_requested": dump_frame,
        }


def validate_arm(
    result: dict[str, object],
    name: str,
    outcome: str,
    failures: list[str],
    *,
    replay: bool = False,
    break_completion: bool = False,
) -> None:
    spec = VEHICLES[name]
    vehicle = int(spec["id"])
    expected_flags = (
        int(spec["completed"])
        if replay or (outcome == "win" and not break_completion)
        else int(spec["offered"])
    )
    if result["rc"] != 0:
        failures.append(f"exit code {result['rc']}")
    bad = BAD_RE.search(str(result["out"]))
    if bad:
        failures.append(f"runtime marker {bad.group(0)!r}")
    rows = parse_rows(str(result["out"]))
    if not rows:
        failures.append("no [TAJ] lifecycle states")
        return

    load = next((row for row in rows if row["phase"] == "load"), None)
    accept = next((row for row in rows if row["phase"] == "accept"), None)
    start = next((row for row in rows if row["phase"] == "start"), None)
    end_before = next(
        (row for row in rows if row["phase"] == "end_before"), None,
    )
    end_after = next(
        (row for row in rows if row["phase"] == "end_after"), None,
    )
    for phase, row in (
        ("load", load), ("accept", accept), ("start", start),
        ("end_before", end_before), ("end_after", end_after),
    ):
        if row is None:
            failures.append(f"missing {phase} state")
    if load is not None:
        if load["thresholds"] != (5, 10, 18):
            failures.append(
                f"ROM thresholds {load['thresholds']}, want (5, 10, 18)"
            )
        if int(load["balloons"]) != int(spec["threshold"]):
            failures.append(
                f"loaded balloons {load['balloons']}, "
                f"want {spec['threshold']}"
            )
    offers = [row for row in rows if row["phase"] == "offer"]
    if replay:
        if offers:
            failures.append("completed replay was unexpectedly auto-offered")
    else:
        if len(offers) != 1:
            failures.append(f"{len(offers)} offer states, want exactly one")
        elif (
            int(offers[0]["vehicle"]) != vehicle or
            int(offers[0]["flags"]) != int(spec["offered"])
        ):
            failures.append(
                f"offer vehicle/flags {offers[0]['vehicle']}/"
                f"0x{int(offers[0]['flags']):x}, want {vehicle}/"
                f"0x{int(spec['offered']):x}"
            )

    if accept is not None and int(accept["vehicle"]) != vehicle:
        failures.append(
            f"accepted vehicle {accept['vehicle']}, want {vehicle}"
        )
    if start is not None:
        if int(start["vehicle"]) != vehicle:
            failures.append(
                f"started vehicle {start['vehicle']}, want {vehicle}"
            )
        if int(start["racers"]) != 2:
            failures.append(
                f"started with {start['racers']} racers, want 2"
            )

    race_rows = [row for row in rows if row["phase"] == "race"]
    if len(race_rows) < 3:
        failures.append(f"only {len(race_rows)} moving race states")
    elif start is not None:
        for key in ("human", "ai"):
            before = start[key]["position"]  # type: ignore[index]
            samples = [row[key]["position"] for row in race_rows]  # type: ignore[index]
            if not all(
                math.isfinite(float(value))
                for position in (before, *samples)
                for value in position
            ):
                failures.append(f"{key} has a non-finite position")
                continue
            # Displacement is measured as the MAXIMUM over every sampled race
            # row, not start-vs-last: the course is a loop, so a healthy
            # racer periodically returns to within metres of its grid slot,
            # and the last 120-tick sample landing there is a phase accident
            # (observed live: a 1-ULP physics change shifted the sampling
            # phase and a racer that had driven 1.5 laps "moved" 84 units).
            # A genuinely stuck racer never exceeds the bound at ANY sample,
            # so this is strictly stronger against the real failure mode.
            if max(math.dist(before, position) for position in samples) < 100.0:
                failures.append(
                    f"{key} never moved 100 world units from its start")

        # ---- progress floor (ship review, G5) --------------------------
        #
        # Max-displacement is immune to lap phase but it is also STRICTLY
        # WEAKER than the start-vs-last check it replaced. It says "this racer
        # was 100 units from its grid slot at some point", which a racer that
        # oscillates between two teleport endpoints satisfies, and which a
        # racer that drives away once and then freezes satisfies forever. It
        # proves "moved", not "raced". Two floors close that, on the axes
        # movement cannot fake:
        #
        #   * LAP -- checkpoint_update_all only advances it on a legal
        #     circuit, so no amount of displacement produces one. Monotonic
        #     too: a lap counter that goes backwards is its own bug.
        #     Not applied to the abort arm, which quits mid-race by design
        #     (ABORT_FRAMES) and legitimately never completes a lap.
        #   * MOTION -- the largest gap between consecutive sampled positions.
        #     A racer that stops dead partway through still clears the
        #     displacement bound with its earlier samples; it cannot clear
        #     this, and this one DOES apply to the abort arm.
        #
        # Both racers, because the loss and abort arms depend on the AI carpet
        # running the course too.
        #
        # The floors are MEASURED, not chosen. See MIN_*_LAP / MIN_SAMPLE_STEP
        # for the figures and the headroom rule; MDKR_TAJ_REPORT_LAPS=1
        # reprints the per-arm numbers they came from.
        for key in ("human", "ai"):
            laps = [int(row[key]["lap"]) for row in race_rows]  # type: ignore[index]
            if any(b < a for a, b in zip(laps, laps[1:])):
                failures.append(
                    f"{key} lap counter went backwards over the race rows: "
                    f"{laps}")
            reached = max(laps)
            floor = MIN_HUMAN_LAP if key == "human" else MIN_AI_LAP
            if outcome != "abort" and reached < floor:
                failures.append(
                    f"{key} only reached lap {reached} over "
                    f"{len(race_rows)} sampled race rows, floor {floor} — "
                    "the racer moved but did not make course progress")
            positions = [row[key]["position"] for row in race_rows]  # type: ignore[index]
            step = max(
                (math.dist(a, b) for a, b in zip(positions, positions[1:])),
                default=0.0)
            if step < MIN_SAMPLE_STEP:
                failures.append(
                    f"{key} never moved more than {step:.1f} units between "
                    f"consecutive samples (floor {MIN_SAMPLE_STEP}) — it is "
                    "displaced from its start but not still racing")
            MEASURED_LAPS.setdefault(f"{outcome}/{key} lap", []).append(reached)
            MEASURED_LAPS.setdefault(f"{outcome}/{key} step", []).append(
                round(step))

    reason = 1 if outcome == "abort" else 0
    menu = (
        0 if outcome == "abort" else
        4 if outcome == "loss" else
        5 if replay or break_completion else
        vehicle + 6
    )
    if end_before is not None:
        if int(end_before["reason"]) != reason:
            failures.append(
                f"end reason {end_before['reason']}, want {reason}"
            )
        human = end_before["human"]
        ai = end_before["ai"]
        if outcome == "loss":
            if (
                int(human["finished"]) != 1 or int(human["place"]) != 2 or
                int(ai["finished"]) != 1 or int(ai["place"]) != 1
            ):
                failures.append(
                    "production did not assign carpet/human places 1/2"
                )
        elif outcome == "win":
            if (
                int(human["finished"]) != 1 or
                int(human["place"]) != 1
            ):
                failures.append(
                    f"natural human finish was {human['finished']}/"
                    f"{human['place']}, want finished/place 1/1"
                )
        elif int(human["finished"]) != 0:
            failures.append("abort arm had already finished the human")
    if end_after is not None:
        if int(end_after["menu"]) != menu:
            failures.append(
                f"result menu {end_after['menu']}, want {menu}"
            )
        if int(end_after["flags"]) != expected_flags:
            failures.append(
                f"in-memory flags 0x{int(end_after['flags']):x}, "
                f"want 0x{expected_flags:x}"
            )
        if int(end_after["racers"]) != 1:
            failures.append(
                f"teardown left {end_after['racers']} racers, want 1"
            )
        expected_gate = 0 if break_completion else 1
        if int(end_after["gate"]) != expected_gate:
            failures.append(
                f"completion gate {end_after['gate']}, want {expected_gate}"
            )

    phases = {str(row["phase"]) for row in rows}
    if outcome == "loss" and "event_ai_finish" not in phases:
        failures.append("loss arm never delivered the carpet finish event")
    if outcome == "win" and name == "plane" and "event_ai_hold" not in phases:
        failures.append("plane win never exercised the carpet final-lap hold")
    if outcome == "abort" and "event_abort" not in phases:
        failures.append("abort arm never requested CHALLENGE_END_QUIT")

    decoded = decode_slot(result["save"])  # type: ignore[arg-type]
    if not decoded["checksum_ok"]:
        failures.append("persisted save checksum is invalid")
    if decoded["flags"] != expected_flags:
        failures.append(
            f"persisted flags 0x{int(decoded['flags']):x}, "
            f"want 0x{expected_flags:x}"
        )
    if decoded["balloons"] != int(spec["threshold"]):
        failures.append(
            f"persisted balloons {decoded['balloons']}, "
            f"want {spec['threshold']}"
        )
    if replay and result["save"][:40] != result["original"][:40]:  # type: ignore[index]
        failures.append("completed replay rewrote the Adventure save slot")

    if result["reload_rc"] != 0:
        failures.append(f"second-process reload exited {result['reload_rc']}")
    reload_bad = BAD_RE.search(str(result["reload_out"]))
    if reload_bad:
        failures.append(f"reload marker {reload_bad.group(0)!r}")
    reload_rows = parse_rows(str(result["reload_out"]))
    reload_load = next(
        (row for row in reload_rows if row["phase"] == "load"), None,
    )
    if reload_load is None:
        failures.append("second process never decoded the Taj save")
    elif int(reload_load["flags"]) != expected_flags:
        failures.append(
            f"reloaded flags 0x{int(reload_load['flags']):x}, "
            f"want 0x{expected_flags:x}"
        )
    if any(row["phase"] in ("accept", "start") for row in reload_rows):
        failures.append("reload witness unexpectedly started a Taj challenge")

    if result["dump_requested"] and not result["metrics"]:
        failures.append("requested rendered-frame capture produced no frame")
    for frame, colours, sigma in result["metrics"]:  # type: ignore[misc]
        if colours < 200 or sigma < 12.0:
            failures.append(
                f"{frame} render is flat ({colours} colours, sigma {sigma:.1f})"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).expanduser().resolve()
    for path in (binary, rom, BASE_SCRIPT):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    names = ["car"] if args.quick else list(VEHICLES)
    arms = (
        ("first_win", "win", False, False),
        ("loss", "loss", False, False),
        ("abort", "abort", False, False),
        ("replay", "win", True, False),
        ("completion_control", "win", False, True),
    )
    failures: list[str] = []
    for name in names:
        for label, outcome, replay, break_completion in arms:
            if args.verbose:
                print(f"  {name:5s} {label}")
            result = run_arm(
                binary, rom, name, outcome,
                replay=replay,
                break_completion=break_completion,
                dump_frame=label == "first_win",
            )
            arm_failures: list[str] = []
            validate_arm(
                result, name, outcome, arm_failures,
                replay=replay,
                break_completion=break_completion,
            )
            failures.extend(
                f"{name} {label}: {failure}"
                for failure in arm_failures
            )
            if args.verbose and result["metrics"]:
                print(f"    render {result['metrics']}")

    if os.environ.get("MDKR_TAJ_REPORT_LAPS"):
        # How MIN_HUMAN_LAP / MIN_AI_LAP were derived, reproducibly.
        for key, values in sorted(MEASURED_LAPS.items()):
            print(f"  measured {key} laps per arm: {values} (min {min(values)})")

    if failures:
        print(f"FAIL: {len(failures)} Taj challenge regression(s)")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    count = len(names) * len(arms)
    print(
        f"PASS: {count}/{count} Taj arms; car/hover/plane offer, accept, "
        "race, first win, loss, abort, replay, completion control, "
        "render, EEPROM checksum, and second-process reload"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
