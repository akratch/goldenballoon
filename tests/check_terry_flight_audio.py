#!/usr/bin/env python3
"""Gate Terry's authored flap cue and flight-only engine suppression."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
BAD_RE = re.compile(
    r"\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|Assertion"
)
FLAP_RE = re.compile(
    r"\[TERRYAUDIO\] event=flap racer=(\d+) animation=(\d+) "
    r"previous=(\d+) frame=(\d+) sound=(\d+) tick=(\d+) "
    r"interval=(\d+) count=(\d+)"
)
SUPPRESS_RE = re.compile(
    r"\[TERRYAUDIO\] event=engine_suppressed racer=(\d+) vehicle=(\d+) "
    r"had_main=([01]) had_idle=([01]) main=([01]) idle=([01])"
)
VEHICLE_RE = re.compile(
    r"\[VEHAUDIO\].* vehicle=(-?\d+).* main=([01]) idle=([01])"
)
WING_TRAIL_RE = re.compile(
    r"terry_particles: vehicle=plane suppressed=0x([0-9A-F]+) "
    r"before=0x([0-9A-F]+) after=0x([0-9A-F]+)"
)
PHYSICS_PROFILE_RE = re.compile(
    r"terry_physics_profile: presentation=(\d+) stats=(\d+) "
    r"weight=(\S+) handling=(\S+) response=(\S+) performance=(\S+)"
)


class TerryAudioError(RuntimeError):
    pass


def run_case(binary: Path, rom: Path, vehicle: int, frames: int,
             timeout: int, output_path: Path) -> str:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update({
        "LC_ALL": "C",
        "MDKR_AUDIO": "0",
        "MDKR_TRACE": "1",
        "MDKR_PRESENT_RATE": "original",
        "MDKR_SIMULATION_CADENCE": "original",
        "MDKR_SYNTH_FIELDS": "2",
        "MDKR_AUTOPILOT": "1",
        "MDKR_LOAD_TRACK": f"15:{vehicle}",
        "MDKR_TERRY_TEST_PLAYER": "0",
        "MDKR_TERRY_AUDIO_TRACE": "1",
        "MDKR_VEHICLE_AUDIO_TRACE": "1",
    })
    command = [
        str(binary), "--headless-frames", str(frames),
        "--input-script", str(SCRIPT), "--rom", str(rom),
    ]
    proc = subprocess.run(
        command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False,
    )
    output = proc.stdout.decode("utf-8", "replace")
    output_path.write_text(output, encoding="utf-8")
    if proc.returncode != 0:
        raise TerryAudioError(
            f"vehicle {vehicle}: runner exited {proc.returncode}\n{output[-3000:]}")
    bad = BAD_RE.search(output)
    if bad:
        raise TerryAudioError(f"vehicle {vehicle}: emitted {bad.group(0)}")
    return output


def check_plane(output: str) -> tuple[int, int]:
    flaps = [tuple(map(int, row)) for row in FLAP_RE.findall(output)]
    suppressed = [tuple(map(int, row)) for row in SUPPRESS_RE.findall(output)]
    if len(flaps) < 10:
        raise TerryAudioError(f"plane: only {len(flaps)} flap cues")
    for racer, animation, previous, frame, sound, _, interval, count in flaps:
        if (racer, animation, previous, frame, sound) != (0, 4, 28, 32, 547):
            raise TerryAudioError(
                "plane: flap did not use Terry's authored fly threshold/sound: "
                f"{(racer, animation, previous, frame, sound)}")
        if count > 1 and interval != 48:
            raise TerryAudioError(
                f"plane: flap {count} cadence was {interval}, expected 48 ticks")
    if [row[7] for row in flaps] != list(range(1, len(flaps) + 1)):
        raise TerryAudioError("plane: flap sequence was dropped or duplicated")
    if len(suppressed) < 5:
        raise TerryAudioError(
            f"plane: only {len(suppressed)} engine-suppression witnesses")
    if any(vehicle != 2 or main != 0 or idle != 0
           for _, vehicle, _, _, main, idle in suppressed):
        raise TerryAudioError("plane: a suppressed engine handle remained live")
    if any(vehicle == 2 and main for vehicle, main, _ in
           (tuple(map(int, row)) for row in VEHICLE_RE.findall(output))):
        raise TerryAudioError("plane: continuous vehicle engine started")
    trail_rows = [tuple(int(value, 16) for value in row)
                  for row in WING_TRAIL_RE.findall(output)]
    if len(trail_rows) != 1:
        raise TerryAudioError(
            f"plane: expected one wing-trail suppression witness, got {len(trail_rows)}")
    trail_suppressed, before, after = trail_rows[0]
    if (trail_suppressed == 0 or trail_suppressed & ~0xCC or
            after & 0xCC or not before & trail_suppressed):
        raise TerryAudioError(
            "plane: suppression escaped the four donor wing emitters: "
            f"suppressed=0x{trail_suppressed:X} before=0x{before:X} "
            f"after=0x{after:X}")
    profiles = PHYSICS_PROFILE_RE.findall(output)
    if len(profiles) != 1:
        raise TerryAudioError(
            f"plane: expected one Terry physics profile, got {len(profiles)}")
    presentation, stats, weight, handling, response, performance = profiles[0]
    if ((int(presentation), int(stats)) != (0, 7) or
            abs(float(performance) - 1.03) > 0.0001 or
            not all(abs(float(value)) > 0.0001
                    for value in (weight, handling, response))):
        raise TerryAudioError(
            "plane: Terry did not combine Krunch presentation with Pipsy's "
            f"light stat row and 1.03x tuning: {profiles[0]}")
    return len(flaps), len(suppressed)


def check_ground(output: str) -> int:
    if (FLAP_RE.search(output) or SUPPRESS_RE.search(output) or
            WING_TRAIL_RE.search(output)):
        raise TerryAudioError(
            "kart: flight-only audio/particle policy leaked to ground mode")
    rows = [tuple(map(int, row)) for row in VEHICLE_RE.findall(output)]
    car_rows = [row for row in rows if row[0] == 0]
    if len(car_rows) < 10 or not any(main for _, main, _ in car_rows):
        raise TerryAudioError("kart: ordinary engine loop did not remain active")
    return len(car_rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=3200)
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    try:
        if not binary.is_file() or not rom.is_file():
            raise TerryAudioError("missing binary or ROM")
        with tempfile.TemporaryDirectory(prefix="mdkr-terry-audio-") as temp:
            root = Path(temp)
            plane = run_case(binary, rom, 2, args.frames, args.timeout,
                             root / "plane.log")
            ground = run_case(binary, rom, 0, args.frames, args.timeout,
                              root / "kart.log")
            flap_count, suppression_count = check_plane(plane)
            ground_count = check_ground(ground)
    except (OSError, subprocess.TimeoutExpired, TerryAudioError) as exc:
        print(f"check_terry_flight_audio: FAIL -- {exc}", file=sys.stderr)
        return 1
    print("check_terry_flight_audio: PASS -- "
          f"{flap_count} authored flap cues at 48-tick cadence; "
          f"{suppression_count} zero-handle engine witnesses; "
          f"kart engine retained across {ground_count} samples")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
