#!/usr/bin/env python3
"""Verify the player engine loop from ROM record through live pitch updates.

The general PCM gate intentionally cannot identify an individual instrument or
prove that its pitch follows vehicle state. This focused gate independently
decodes the selected vehicle records from the retail ROM, drives real car,
hovercraft, and plane routes, and checks the opt-in ``[VEHAUDIO]``
production-seam witness.

The original defect fails independently on sound ID (115 became 29440), update
state (intensity and enginePitch stayed zero), main-loop creation, pitch range,
and idle/main crossfade. The other two vehicle models must also select their
ROM-authored main loops and vary pitch while moving.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SCRIPT = ROOT / "tests/input_scripts/race_drive_time_trial.txt"
DEFAULT_VEHICLE_SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
ASSET_AUDIO_TABLE = 38
ASSET_AUDIO = 39
VEHICLE_RECORD_SIZE = 0x4C
CHARACTER_COUNT = 10
SUPPORTED = {
    (0xE402430D, 0xD2FCFC9D): ("us.v80", 0x000ED0E0, 0x000ED1B0),
    (0x596E145B, 0xF7D9879F): ("pal.v80", 0x000ED170, 0x000ED240),
}
TRACE_RE = re.compile(
    r"\[VEHAUDIO\] character=(-?\d+) vehicle=(-?\d+) sound=(\d+) "
    r"speed=([-+0-9.eE]+) intensity=(\d+) basePitch=([-+0-9.eE]+) "
    r"enginePitch=([-+0-9.eE]+) main=([01]) idle=([01])"
)
BAD_RE = re.compile(
    r"\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion"
)


class VehicleAudioError(RuntimeError):
    pass


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def normalize_rom(data: bytes) -> bytes:
    magic = be32(data, 0)
    if magic == 0x80371240:
        return data
    if magic == 0x37804012:
        out = bytearray(data)
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out)
    if magic == 0x40123780:
        out = bytearray(len(data))
        out[0::4], out[1::4], out[2::4], out[3::4] = (
            data[3::4], data[2::4], data[1::4], data[0::4])
        return bytes(out)
    raise VehicleAudioError("unrecognised ROM byte order")


def rom_sections(path: Path) -> tuple[str, bytes, tuple[int, ...]]:
    data = normalize_rom(path.read_bytes())
    revision = SUPPORTED.get((be32(data, 0x10), be32(data, 0x14)))
    if revision is None:
        raise VehicleAudioError("ROM is not a supported revision-1 asset corpus")
    name, lut_start, data_base = revision

    def section(index: int) -> bytes:
        start = data_base + be32(data, lut_start + 4 * (index + 1))
        end = data_base + be32(data, lut_start + 4 * (index + 2))
        return data[start:end]

    table_blob = section(ASSET_AUDIO_TABLE)
    table = struct.unpack(f">{len(table_blob) // 4}i", table_blob)
    return name, section(ASSET_AUDIO), table


def sound_model(vehicle: int) -> int:
    if vehicle in (0, 4):
        return 0
    if vehicle == 1:
        return 1
    if vehicle in (2, 3):
        return 2
    raise VehicleAudioError(f"trace reported unsupported vehicle {vehicle}")


def expected_sound(audio: bytes, offsets: tuple[int, ...], character: int,
                   vehicle: int) -> int:
    if not 0 <= character < CHARACTER_COUNT:
        raise VehicleAudioError(f"trace reported invalid character {character}")
    row = sound_model(vehicle) * CHARACTER_COUNT + character
    start = offsets[7] + row * VEHICLE_RECORD_SIZE
    end = start + VEHICLE_RECORD_SIZE
    if end > offsets[8] or end > len(audio):
        raise VehicleAudioError(f"vehicle sound row {row} falls outside ASSET_AUDIO_7")
    return struct.unpack_from(">H", audio, start)[0]


def run_case(binary: Path, rom: Path, audio: bytes, offsets: tuple[int, ...],
             script: Path, frames: int, timeout: int, label: str,
             expected_model: int, forced_vehicle: int | None) -> str:
    env = dict(os.environ)
    env.pop("MDKR_AUTOPILOT", None)
    env.pop("MDKR_LOAD_TRACK", None)
    env.update({
        "MDKR_AUDIO": "0",
        "MDKR_PRESENT_RATE": "original",
        "MDKR_SIMULATION_CADENCE": "original",
        "MDKR_SYNTH_FIELDS": "2",
        "MDKR_VEHICLE_AUDIO_TRACE": "1",
    })
    if forced_vehicle is not None:
        env["MDKR_AUTOPILOT"] = "1"
        env["MDKR_LOAD_TRACK"] = f"15:{forced_vehicle}"
    command = [
        str(binary), "--headless-frames", str(frames),
        "--input-script", str(script), "--rom", str(rom),
    ]
    proc = subprocess.run(
        command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout,
    )
    output = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0:
        raise VehicleAudioError(
            f"{label}: runner exited {proc.returncode}\n{output[-4000:]}")
    bad = BAD_RE.findall(output)
    if bad:
        raise VehicleAudioError(f"{label}: runtime emitted failure marker {bad[0]}")

    rows = [
        (int(c), int(v), int(s), float(speed), int(intensity), float(base),
         float(engine), int(main), int(idle))
        for c, v, s, speed, intensity, base, engine, main, idle
        in TRACE_RE.findall(output)
    ]
    if len(rows) < 40:
        raise VehicleAudioError(
            f"{label}: only {len(rows)} vehicle-audio samples, expected >= 40")

    for character, vehicle, sound, *_ in rows:
        if sound_model(vehicle) != expected_model:
            raise VehicleAudioError(
                f"{label}: runtime used vehicle sound model {sound_model(vehicle)}, "
                f"expected {expected_model}")
        wanted = expected_sound(audio, offsets, character, vehicle)
        if sound != wanted:
            raise VehicleAudioError(
                f"{label}: runtime sound {sound} != ROM BE sound {wanted} for "
                f"character {character}, vehicle {vehicle}")

    moving = [row for row in rows if abs(row[3]) >= 5.0]
    if len(moving) < 10:
        raise VehicleAudioError(f"{label}: route never sustained driving speed")
    intensities = [row[4] for row in moving]
    base_pitches = [row[5] for row in moving]
    engine_pitches = [row[6] for row in rows]
    if max(base_pitches) - min(base_pitches) < 0.20:
        raise VehicleAudioError(f"{label}: base engine pitch did not vary while driving")
    if not any(row[7] for row in moving):
        raise VehicleAudioError(f"{label}: main engine loop never became active")
    if expected_model == 0:
        if max(intensities) < 50 or len(set(intensities)) < 5:
            raise VehicleAudioError(f"{label}: engine intensity did not follow vehicle speed")
        if max(engine_pitches) < 0.30 or max(engine_pitches) - min(engine_pitches) < 0.25:
            raise VehicleAudioError(f"{label}: throttle pitch boost did not ramp")
        if not any(row[8] for row in rows) or not any(not row[8] for row in moving):
            raise VehicleAudioError(
                f"{label}: idle/main engine crossfade did not exercise both states")
        extra = (f", intensity {min(intensities)}..{max(intensities)}, "
                 f"throttle {min(engine_pitches):.3f}..{max(engine_pitches):.3f}")
    else:
        extra = ""
    return (f"{label} {len(rows)} samples, sound {rows[-1][2]}, "
            f"base pitch {min(base_pitches):.3f}..{max(base_pitches):.3f}{extra}")


def run(args: argparse.Namespace) -> None:
    binary = resolve_binary(args.build)
    rom = Path(args.rom).resolve()
    revision, audio, offsets = rom_sections(rom)
    results = [run_case(
        binary, rom, audio, offsets, Path(args.script).resolve(), args.frames,
        args.timeout, "car", 0, None)]
    if not args.car_only:
        vehicle_script = Path(args.vehicle_script).resolve()
        results.append(run_case(
            binary, rom, audio, offsets, vehicle_script, args.vehicle_frames,
            args.timeout, "hovercraft", 1, 1))
        results.append(run_case(
            binary, rom, audio, offsets, vehicle_script, args.vehicle_frames,
            args.timeout, "plane", 2, 2))
    print(f"check_vehicle_audio: PASS — {revision}; " + "; ".join(results))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--script", default=str(DEFAULT_SCRIPT))
    parser.add_argument("--frames", type=int, default=3500)
    parser.add_argument("--vehicle-script", default=str(DEFAULT_VEHICLE_SCRIPT))
    parser.add_argument("--vehicle-frames", type=int, default=5200)
    parser.add_argument("--car-only", action="store_true")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    try:
        run(args)
    except (OSError, subprocess.TimeoutExpired, struct.error, VehicleAudioError) as exc:
        print(f"check_vehicle_audio: FAIL — {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
