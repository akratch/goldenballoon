#!/usr/bin/env python3
"""Exercise Adventure Two as a real campaign mode on every race track.

This is deliberately not a magic-code mirror check.  Each arm starts with a
checksum-valid, canonical N64 EEPROM image, selects Adventure Two through
GAME_SELECT, resumes a save carrying CUTSCENE_ADVENTURE_TWO, drives through
Timber's Island and the Dino Domain lobby, and enters a race.  The race load is
then retargeted with MDKR_LOAD_TRACK so one proven campaign route covers all 20
main tracks.

The check closes five independent contracts:

* the big-endian global config block really unlocks Adventure Two on little-
  endian native/web hosts and remains byte-canonical;
* the selected save retains CUTSCENE_ADVENTURE_TWO with a valid checksum;
* every main track loads and the racer makes finite forward progress;
* camera/host viewport, stereo pan, minimap and human steering all execute their
  mirrored paths (trace-only witnesses emitted by the production operations);
* a representative world render is the horizontal reflection of Adventure One,
  not the byte-identical frame produced when negative N64 viewport widths were
  passed illegally to GL/WebGPU.

No developer save is read or changed: every process runs in a temporary cwd.
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

from harness_utils import resolve_binary


MAIN_TRACK_IDS = [
    5, 3, 29, 7, 8, 4, 10, 30, 13, 6,
    9, 28, 19, 18, 20, 31, 17, 32, 33, 15,
]
NORMAL_SCRIPT = "tests/input_scripts/adventure_resume_race.txt"
ADV2_SCRIPT = "tests/input_scripts/adventure_two_resume_race.txt"
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12;12:E5"
)
VISUAL_DUMP_FROM = 2125
VISUAL_DUMP_EVERY = 25
VISUAL_DUMP_THROUGH = 2250

EEPROM_BYTES = 512
SLOT_BYTES = 40
CONFIG_OFFSET = 120
RECORD_OFFSETS = (128, 320)
RECORD_BYTES = 192
CUTSCENE_BIT_OFFSET = 16 + 68 + 6 + 10 + 12 + (6 * 7) + 3 + 3 + (6 * 16) + 8
CUTSCENE_ADVENTURE_TWO = 4

LEVEL_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=(-?\d+).*@frame~(\d+)"
)
PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+)"
    r" clock=(\d+) cp=(-?\d+) lap=(-?\d+)"
)
MODE_RE = re.compile(
    r"adventure_mode: level=(\d+) adventureTwo=(\d+) mirrored=(\d+)"
    r" saveFlags=0x([0-9a-fA-F]+)"
)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)

REQUIRED_ADV2_WITNESSES = (
    "adventure_audio:",
    "adventure_viewport:",
    "adventure_hud:",
    "adventure_steering:",
)


def put_bits(bits: list[int], width: int, value: int) -> None:
    bits.extend((value >> shift) & 1 for shift in range(width - 1, -1, -1))


def save_slot(adventure_two: bool) -> bytes:
    bits: list[int] = []
    put_bits(bits, 16, 0)       # checksum, populated below
    put_bits(bits, 68, 0)       # per-course flags
    put_bits(bits, 6, 0)        # Taj flags
    put_bits(bits, 10, 0)       # trophies
    put_bits(bits, 12, 0)       # bosses
    for _ in range(6):
        put_bits(bits, 7, 0)    # total + five world balloon counts
    put_bits(bits, 3, 0)        # TT amulet
    put_bits(bits, 3, 0)        # Wizpig amulet
    for _ in range(6):
        put_bits(bits, 16, 0)   # world door flags
    put_bits(bits, 8, 0)        # keys
    put_bits(bits, 32, CUTSCENE_ADVENTURE_TWO if adventure_two else 0)
    put_bits(bits, 16, 0x1234)  # a named/started file
    put_bits(bits, 8, 0)        # pad
    if len(bits) != SLOT_BYTES * 8:
        raise AssertionError(f"slot builder emitted {len(bits)} bits")
    out = bytearray(
        int("".join(str(bit) for bit in bits[i:i + 8]), 2)
        for i in range(0, len(bits), 8)
    )
    out[0:2] = ((5 + sum(out[2:])) & 0xFFFF).to_bytes(2, "big")
    return bytes(out)


def config_block() -> bytes:
    # Adventure Two unlocked + the retail default subtitle bit.
    value = (1 << 25) | 1
    checksum = 5 + sum((value >> (i * 4)) & 0xF for i in range(14))
    value |= (checksum & 0xFF) << 56
    return value.to_bytes(8, "big")


def sum_block(size: int) -> bytes:
    out = bytearray(size)
    out[0:2] = ((5 + sum(out[2:])) & 0xFFFF).to_bytes(2, "big")
    return bytes(out)


def eeprom_image(adventure_two: bool) -> bytes:
    out = bytearray(EEPROM_BYTES)
    out[:SLOT_BYTES] = save_slot(adventure_two)
    out[SLOT_BYTES:CONFIG_OFFSET] = b"\xFF" * (CONFIG_OFFSET - SLOT_BYTES)
    out[CONFIG_OFFSET:CONFIG_OFFSET + 8] = config_block()
    for offset in RECORD_OFFSETS:
        out[offset:offset + RECORD_BYTES] = sum_block(RECORD_BYTES)
    return bytes(out)


def slot_checksum_ok(slot: bytes) -> bool:
    return int.from_bytes(slot[:2], "big") == ((5 + sum(slot[2:])) & 0xFFFF)


def slot_cutscene_flags(slot: bytes) -> int:
    bits = "".join(f"{byte:08b}" for byte in slot)
    return int(bits[CUTSCENE_BIT_OFFSET:CUTSCENE_BIT_OFFSET + 32], 2)


def clean_env() -> dict[str, str]:
    return {
        key: value for key, value in os.environ.items()
        if not key.startswith("MDKR_")
    }


def run_case(
    binary: str,
    rom: str,
    track: int,
    adventure_two: bool,
    frames: int,
    dump_frames: bool = False,
    renderer: str | None = None,
    mode: str = "remastered",
    cadence: str = "original",
) -> dict[str, object]:
    with tempfile.TemporaryDirectory(prefix="mdkr_adv2_") as run_dir:
        root = Path(run_dir)
        (root / "save").mkdir()
        (root / "save/eeprom.bin").write_bytes(eeprom_image(adventure_two))
        frame_dir = root / "frames"
        if dump_frames:
            frame_dir.mkdir()

        env = clean_env()
        env.update(
            MDKR_AUDIO="0",
            MDKR_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
            MDKR_LOAD_TRACK=str(track),
            MDKR_SIMULATION_CADENCE=cadence,
            MDKR_SYNTH_FIELDS="2" if cadence == "original" else "1",
        )
        if renderer is not None:
            env["MDKR_RENDERER"] = renderer
        if dump_frames:
            env.update(
                MDKR_DUMP_FROM=str(VISUAL_DUMP_FROM),
                MDKR_DUMP_EVERY=str(VISUAL_DUMP_EVERY),
            )

        script = os.path.abspath(ADV2_SCRIPT if adventure_two else NORMAL_SCRIPT)
        command = [
            binary,
            f"--{mode}",
            "--headless-frames", str(frames),
            "--input-script", script,
        ]
        if dump_frames:
            command.extend(("--dump-frames", str(frame_dir)))
        command.extend(("--rom", rom))
        try:
            proc = subprocess.run(
                command,
                cwd=run_dir,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=max(120, frames // 20),
            )
            output = proc.stdout.decode("utf-8", "replace")
            rc: int | str = proc.returncode
        except subprocess.TimeoutExpired as exc:
            output = (exc.stdout or b"").decode("utf-8", "replace")
            rc = "timeout"

        save_path = root / "save/eeprom.bin"
        saved = save_path.read_bytes() if save_path.exists() else b""
        captures = {
            path.name: path.read_bytes()
            for path in frame_dir.glob("frame_*.ppm")
        } if dump_frames else {}
        quarantined = (root / "save/eeprom.bin.bad").exists()
        return {
            "rc": rc,
            "out": output,
            "save": saved,
            "captures": captures,
            "quarantined": quarantined,
        }


def ppm(payload: bytes) -> tuple[int, int, bytes]:
    header = payload.split(b"\n", 3)
    if len(header) != 4 or header[0] != b"P6" or header[2] != b"255":
        raise ValueError("not an 8-bit binary PPM")
    width, height = (int(value) for value in header[1].split())
    pixels = header[3]
    if len(pixels) != width * height * 3:
        raise ValueError(
            f"PPM payload is {len(pixels)} bytes, expected {width * height * 3}"
        )
    return width, height, pixels


def crop_world(pixels: bytes, width: int, height: int) -> tuple[int, int, bytes]:
    # Use a symmetric centre/lower-world crop. The top and bottom carry DKR's
    # safe-4:3 HUD, which is deliberately NOT reflected with the world. This
    # band retains track, racers and scenery while excluding those fixed-space
    # overlays. Symmetric X bounds make flipping the crop equivalent to cropping
    # the full reflected frame.
    x0, x1 = int(width * 0.05), int(width * 0.95)
    y0, y1 = int(height * 0.30), int(height * 0.70)
    cropped = b"".join(
        pixels[(y * width + x0) * 3:(y * width + x1) * 3]
        for y in range(y0, y1)
    )
    return x1 - x0, y1 - y0, cropped


def horizontal_flip(pixels: bytes, width: int, height: int) -> bytes:
    row_bytes = width * 3
    out = bytearray(len(pixels))
    for y in range(height):
        row = pixels[y * row_bytes:(y + 1) * row_bytes]
        target = y * row_bytes
        for x in range(width):
            src = (width - 1 - x) * 3
            dst = target + x * 3
            out[dst:dst + 3] = row[src:src + 3]
    return bytes(out)


def mean_abs_difference(left: bytes, right: bytes) -> float:
    if len(left) != len(right) or not left:
        return math.inf
    return sum(abs(a - b) for a, b in zip(left, right)) / len(left)


def validate_run(
    result: dict[str, object],
    track: int,
    adventure_two: bool,
) -> tuple[list[str], int, int]:
    failures: list[str] = []
    output = str(result["out"])
    if result["rc"] != 0:
        failures.append(f"exit={result['rc']}")
    bad = BAD_RE.search(output)
    if bad:
        failures.append(f"bad runtime marker {bad.group(0)!r}")

    levels = [
        (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        for m in LEVEL_RE.finditer(output)
    ]
    target_loads = [entry for entry in levels if entry[0] == track and entry[1] == 0]
    if not target_loads:
        failures.append(f"level {track} never loaded as a one-player race")
        load_frame = 10**9
    else:
        load_frame = target_loads[-1][2]

    modes = [
        (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4), 16))
        for m in MODE_RE.finditer(output)
        if int(m.group(1)) == track
    ]
    expected = (track, int(adventure_two), int(adventure_two),
                CUTSCENE_ADVENTURE_TWO if adventure_two else 0)
    if expected not in modes:
        failures.append(f"mode witness {expected} absent; saw {modes}")

    rows = []
    for match in PACE_RE.finditer(output):
        row = (
            int(match.group(1)),
            float(match.group(2)),
            float(match.group(3)),
            float(match.group(4)),
            int(match.group(5)),
            int(match.group(6)),
            int(match.group(7)),
        )
        if row[0] >= load_frame:
            rows.append(row)
    if not rows:
        failures.append("no racer rows after target load")
        max_checkpoint = -1
    else:
        nonfinite = [
            row for row in rows
            if not all(math.isfinite(value) for value in row[1:4])
        ]
        if nonfinite:
            failures.append(f"{len(nonfinite)} non-finite racer rows")
        max_checkpoint = max(row[5] for row in rows)
        if max_checkpoint < 3:
            failures.append(f"forward progress only reached checkpoint {max_checkpoint}")

    if adventure_two:
        for witness in REQUIRED_ADV2_WITNESSES:
            if witness not in output:
                failures.append(f"missing live mirrored-path witness {witness}")
    else:
        for witness in REQUIRED_ADV2_WITNESSES:
            if witness in output:
                failures.append(f"Adventure One unexpectedly emitted {witness}")

    saved = bytes(result["save"])
    if len(saved) != EEPROM_BYTES:
        failures.append(f"EEPROM length {len(saved)}, expected {EEPROM_BYTES}")
    else:
        slot = saved[:SLOT_BYTES]
        if not slot_checksum_ok(slot):
            failures.append("save-slot checksum is invalid after campaign transition")
        flags = slot_cutscene_flags(slot)
        expected_flags = CUTSCENE_ADVENTURE_TWO if adventure_two else 0
        if (flags & CUTSCENE_ADVENTURE_TWO) != expected_flags:
            failures.append(f"saved cutsceneFlags=0x{flags:x}, expected Adv2 bit "
                            f"{'set' if adventure_two else 'clear'}")
        actual_config = saved[CONFIG_OFFSET:CONFIG_OFFSET + 8]
        if actual_config != config_block():
            failures.append(
                f"config serialized as {actual_config.hex()}, expected canonical "
                f"big-endian {config_block().hex()}"
            )
    if result["quarantined"]:
        failures.append("valid synthetic EEPROM was quarantined")
    return failures, len(rows), max_checkpoint


def validate_visual_pair(
    normal: dict[str, object],
    adventure_two: dict[str, object],
) -> tuple[list[str], list[tuple[str, float, float]]]:
    failures: list[str] = []
    metrics: list[tuple[str, float, float]] = []
    normal_frames = dict(normal["captures"])
    adv2_frames = dict(adventure_two["captures"])
    names = sorted(
        name for name in set(normal_frames) & set(adv2_frames)
        if VISUAL_DUMP_FROM
        <= int(name.removeprefix("frame_").removesuffix(".ppm"))
        <= VISUAL_DUMP_THROUGH
    )
    if len(names) < 4:
        return [f"only {len(names)} matched visual samples (want >= 4)"], metrics

    for name in names:
        try:
            width, height, normal_pixels = ppm(normal_frames[name])
            adv_width, adv_height, adv_pixels = ppm(adv2_frames[name])
        except ValueError as exc:
            failures.append(f"{name}: {exc}")
            continue
        if (width, height) != (adv_width, adv_height):
            failures.append(
                f"{name}: dimensions {width}x{height} vs {adv_width}x{adv_height}"
            )
            continue
        crop_width, crop_height, normal_crop = crop_world(
            normal_pixels, width, height
        )
        _, _, adv_crop = crop_world(adv_pixels, width, height)
        same_mad = mean_abs_difference(normal_crop, adv_crop)
        flip_mad = mean_abs_difference(
            horizontal_flip(normal_crop, crop_width, crop_height), adv_crop
        )
        metrics.append((name, same_mad, flip_mad))
        if same_mad < 10.0:
            failures.append(
                f"{name}: modes are effectively identical (same-orientation MAD "
                f"{same_mad:.3f})"
            )
        if flip_mad > 6.0:
            failures.append(
                f"{name}: horizontal reflection MAD {flip_mad:.3f} exceeds 6.0"
            )
        if same_mad <= flip_mad * 5.0:
            failures.append(
                f"{name}: reflection is not decisively better than the unflipped "
                f"control ({flip_mad:.3f} vs {same_mad:.3f})"
            )
    return failures, metrics


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=5200)
    parser.add_argument("--tracks", default=None,
                        help="comma-separated subset (default: all 20)")
    parser.add_argument("--renderer", choices=("gl", "wgpu"), default=None)
    parser.add_argument(
        "--mode",
        choices=("pure", "restored", "remastered"),
        default="remastered",
    )
    parser.add_argument(
        "--cadence",
        choices=("original", "enhanced"),
        default="original",
    )
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="optional directory for the Adventure One/Two visual captures",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = os.path.abspath(resolve_binary(args.build))
    rom = os.path.abspath(args.rom)
    scripts = [os.path.abspath(NORMAL_SCRIPT), os.path.abspath(ADV2_SCRIPT)]
    for path in (binary, rom, *scripts):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1
    tracks = (
        [int(value) for value in args.tracks.split(",")]
        if args.tracks else MAIN_TRACK_IDS
    )

    failures: list[str] = []
    print(f"Adventure Two: {len(tracks)} track(s), {args.frames} frames each")
    adv2_by_track: dict[int, dict[str, object]] = {}
    for track in tracks:
        result = run_case(
            binary, rom, track, True, args.frames,
            dump_frames=track == 5, renderer=args.renderer,
            mode=args.mode, cadence=args.cadence,
        )
        adv2_by_track[track] = result
        problems, rows, max_checkpoint = validate_run(result, track, True)
        print(f"  track {track:2d}: {'FAIL' if problems else 'ok':4s} "
              f"rows={rows:4d} maxcp={max_checkpoint:2d}")
        for problem in problems:
            failures.append(f"track {track}: {problem}")
        if args.verbose and problems:
            print("\n".join(str(result["out"]).splitlines()[-80:]))

    # Adventure One is both the visual oracle and a negative control: none of
    # the Adv2-only path witnesses may fire.
    normal = run_case(
        binary, rom, 5, False, args.frames,
        dump_frames=True, renderer=args.renderer,
        mode=args.mode, cadence=args.cadence,
    )
    normal_problems, rows, max_checkpoint = validate_run(normal, 5, False)
    for problem in normal_problems:
        failures.append(f"Adventure One control: {problem}")
    print(f"  Adv1 control: {'FAIL' if normal_problems else 'ok':4s} "
          f"rows={rows:4d} maxcp={max_checkpoint:2d}")

    if 5 in adv2_by_track:
        if args.artifacts is not None:
            args.artifacts.mkdir(parents=True, exist_ok=True)
            for arm, result in (
                ("adventure_one", normal),
                ("adventure_two", adv2_by_track[5]),
            ):
                for name, payload in dict(result["captures"]).items():
                    (args.artifacts / f"{arm}_{name}").write_bytes(payload)
        visual_problems, metrics = validate_visual_pair(normal, adv2_by_track[5])
        for problem in visual_problems:
            failures.append(f"visual mirror: {problem}")
        if metrics:
            same = sum(row[1] for row in metrics) / len(metrics)
            flipped = sum(row[2] for row in metrics) / len(metrics)
            print(f"  visual mirror: {len(metrics)} samples, "
                  f"same MAD={same:.3f}, reflected MAD={flipped:.3f}")
    else:
        print("  visual mirror: skipped (track 5 not selected)")

    if failures:
        print(f"FAIL: Adventure Two check ({len(failures)} issue(s))")
        for failure in failures:
            print("  - " + failure)
        return 1
    print("PASS: Adventure Two unlock/save identity, all selected racing lines, "
          "camera/viewport, stereo, HUD and steering mirror paths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
