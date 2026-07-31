#!/usr/bin/env python3
"""Drive Adventure trophy-series progression through production menus and save.

The fixture starts from a checksum-valid campaign checkpoint with the Dino
Domain cabinet legitimately unlocked (eight local balloons plus its first boss
flag), drives into that cabinet, and then lets DKR run all four trophy races.
``MDKR_TROPHY_ORDER`` controls only the already-complete finish permutation at
the rankings boundary. Production code still owns round selection, points,
championship sorting, ties, podium selection, cinematic selection, trophy
upgrade, and EEPROM persistence.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from harness_utils import resolve_binary


ROOT = Path(__file__).resolve().parent.parent
EEPROM_BYTES = 512
SLOT_BYTES = 40
CONFIG_OFFSET = 120
RECORD_OFFSETS = (128, 320)
RECORD_BYTES = 192
TROPHY_BIT_OFFSET = 16 + 68 + 6

ROUTE = (
    "0:200,500:-1004,946:-1858,1099:-3381,1946:-3948,2180:E12"
    ";12:T"
)
TIE_ORDER = "/".join((
    "0,1,2,3,4,5,6,7",
    "1,0,2,3,4,5,6,7",
    "0,1,2,3,4,5,6,7",
    "1,0,2,3,4,5,6,7",
))
EXPECTED_TRACKS = (5, 3, 29, 7)

MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
ROUND_RE = re.compile(
    r"trophyround: world=(\d+) round=(\d+) track=(\d+) points0=(\d+)"
)
RANK_RE = re.compile(
    r"trophyrankings: world=(\d+) completedRound=(\d+) nextRound=(\d+).*"
    r"order=([0-7](?:,[0-7]){7})"
)
AWARD_RE = re.compile(
    r"trophyaward: world=(\d+) rank=(\d+) points=(\d+) old=0x([0-9a-f]+) "
    r"new=0x([0-9a-f]+) cinematic=(\d+)"
)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)


def put_bits(bits: list[int], width: int, value: int) -> None:
    bits.extend((value >> shift) & 1 for shift in range(width - 1, -1, -1))


def save_slot() -> bytes:
    bits: list[int] = []
    put_bits(bits, 16, 0)
    put_bits(bits, 68, 0)
    put_bits(bits, 6, 0)
    put_bits(bits, 10, 0)       # no trophy: production must award it
    put_bits(bits, 12, 1 << 7)  # Dino Domain first boss complete
    for value in (8, 8, 0, 0, 0, 0):
        put_bits(bits, 7, value)
    put_bits(bits, 3, 0)
    put_bits(bits, 3, 0)
    for _ in range(6):
        put_bits(bits, 16, 0)
    put_bits(bits, 8, 0)
    put_bits(bits, 32, 0)
    put_bits(bits, 16, 0x1234)
    put_bits(bits, 8, 0)
    if len(bits) != SLOT_BYTES * 8:
        raise AssertionError(f"slot builder emitted {len(bits)} bits")
    out = bytearray(
        int("".join(str(bit) for bit in bits[i:i + 8]), 2)
        for i in range(0, len(bits), 8)
    )
    out[0:2] = ((5 + sum(out[2:])) & 0xFFFF).to_bytes(2, "big")
    return bytes(out)


def sum_block(size: int) -> bytes:
    out = bytearray(size)
    out[0:2] = ((5 + sum(out[2:])) & 0xFFFF).to_bytes(2, "big")
    return bytes(out)


def config_block() -> bytes:
    value = 1  # retail default subtitle bit
    checksum = 5 + sum((value >> (i * 4)) & 0xF for i in range(14))
    value |= (checksum & 0xFF) << 56
    return value.to_bytes(8, "big")


def eeprom_image() -> bytes:
    out = bytearray(EEPROM_BYTES)
    out[:SLOT_BYTES] = save_slot()
    out[SLOT_BYTES:CONFIG_OFFSET] = b"\xFF" * (CONFIG_OFFSET - SLOT_BYTES)
    out[CONFIG_OFFSET:CONFIG_OFFSET + 8] = config_block()
    for offset in RECORD_OFFSETS:
        out[offset:offset + RECORD_BYTES] = sum_block(RECORD_BYTES)
    return bytes(out)


def read_bits(data: bytes, offset: int, width: int) -> int:
    value = 0
    for bit in range(offset, offset + width):
        value = (value << 1) | ((data[bit // 8] >> (7 - bit % 8)) & 1)
    return value


def write_input_script(path: Path, frames: int, mode: str) -> None:
    lines = [
        "1250 START 4", "1330 START 4", "1450 A 4", "1540 A 4",
        "1750 A 4", "1900 A 4", "2050 A 4",
    ]
    if mode in ("full", "retry"):
        # Edge-triggered A advances cabinet, round, post-race, rankings and final
        # cinematic screens whenever they are ready. During a race it is merely
        # the throttle button already supplied by MDKR_AUTOPILOT.
        lines.extend(f"{frame} A 4" for frame in range(2400, frames, 220))
    elif mode == "quit_retry":
        lines.extend(f"{frame} A 4" for frame in range(2400, 5900, 220))
        # First rankings RANKINGS_ORDER: select QUIT TROPHY RACE and confirm.
        lines.append("7400 DOWN+A 4")
        # Back in the world lobby, collide with and enter the same cabinet again.
        lines.extend(f"{frame} A 4" for frame in range(8000, frames, 220))
    elif mode != "reload":
        raise ValueError(f"unknown input mode {mode}")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def run_case(
    binary: Path,
    rom: Path,
    frames: int,
    initial_eeprom: bytes,
    mode: str,
    order: str | None,
    world: int = 1,
) -> tuple[subprocess.CompletedProcess[str], bytes]:
    with tempfile.TemporaryDirectory(prefix="mdkr_trophy_") as temp:
        run_dir = Path(temp)
        (run_dir / "save").mkdir()
        (run_dir / "save/eeprom.bin").write_bytes(initial_eeprom)
        script = run_dir / "trophy_input.txt"
        write_input_script(script, frames, mode)
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("MDKR_")
        }
        env.update(
            MDKR_AUDIO="0",
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
            MDKR_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_DRIVE_ROUTE=ROUTE,
            MDKR_TROPHY_COMPLETE_AFTER="600",
        )
        if mode == "quit_retry":
            env["MDKR_TROPHY_QUIT_AFTER"] = "0"
        if mode == "retry":
            env["MDKR_TROPHY_COLLIDE"] = "1"
        if order is not None:
            env["MDKR_TROPHY_ORDER"] = order
        if world != 1:
            env["MDKR_TROPHY_WORLD"] = str(world)
        proc = subprocess.run(
            [
                str(binary), "--headless-frames", str(frames),
                "--input-script", str(script), "--rom", str(rom),
            ],
            cwd=run_dir,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=max(240, frames // 20),
            check=False,
        )
        return proc, (run_dir / "save/eeprom.bin").read_bytes()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int, default=18000)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    for path in (binary, rom):
        if not path.exists():
            print(f"FAIL: missing {path}")
            return 1

    proc, save = run_case(
        binary, rom, args.frames, eeprom_image(), "full", TIE_ORDER
    )
    output = proc.stdout
    failures: list[str] = []
    if proc.returncode != 0:
        failures.append(f"exit code {proc.returncode}")
    bad = BAD_RE.search(output)
    if bad:
        failures.append(f"runtime marker {bad.group(0)!r}")
    if "trophycabinet: ENTER world=1" not in output:
        failures.append("production Dino Domain cabinet entry was not observed")
    if "trophyseries: ENTER world=1" not in output:
        failures.append("Adventure trophy-series initialization was not observed")

    rounds = [
        tuple(map(int, match.groups()))
        for match in ROUND_RE.finditer(output)
    ]
    if [(entry[1], entry[2]) for entry in rounds[:4]] != list(enumerate(EXPECTED_TRACKS)):
        failures.append(f"round/track sequence {rounds[:4]}, expected {list(enumerate(EXPECTED_TRACKS))}")
    rankings = list(RANK_RE.finditer(output))
    if len(rankings) != 4:
        failures.append(f"observed {len(rankings)} rankings screens, expected 4")
    elif rankings[-1].group(4).split(",")[:2] != ["0", "1"]:
        failures.append(
            "32-point tie did not preserve racer 0 before racer 1: "
            f"final order={rankings[-1].group(4)}"
        )
    controls = [
        line for line in output.splitlines()
        if line.startswith("[TRACE] trophyorder:")
    ]
    if len(controls) != 4 or any("REJECT" in line for line in controls):
        failures.append(f"finish-order positive control traces: {controls}")

    award = AWARD_RE.search(output)
    if not award:
        failures.append("final trophy award/cinematic branch was not observed")
    else:
        world, rank, points, old, new, cinematic = award.groups()
        if (world, rank, points, old, new, cinematic) != (
            "1", "0", "32", "0", "3", "1"
        ):
            failures.append(f"unexpected award tuple {award.groups()}")

    if len(save) != EEPROM_BYTES:
        failures.append(f"EEPROM size {len(save)}, expected {EEPROM_BYTES}")
    else:
        slot = save[:SLOT_BYTES]
        checksum = int.from_bytes(slot[:2], "big")
        expected_checksum = (5 + sum(slot[2:])) & 0xFFFF
        if checksum != expected_checksum:
            failures.append(
                f"EEPROM checksum 0x{checksum:04x}, expected 0x{expected_checksum:04x}"
            )
        trophies = read_bits(slot, TROPHY_BIT_OFFSET, 10)
        if trophies != 3:
            failures.append(f"persisted trophies=0x{trophies:x}, expected Dino gold 0x3")

    # A second process must consume the persisted bytes through read_save_file,
    # return to Dino Domain, and instantiate the production cabinet trophy.
    reload_proc, reload_save = run_case(
        binary, rom, 6500, save, "reload", None
    )
    if reload_proc.returncode != 0:
        failures.append(f"reload process exit code {reload_proc.returncode}")
    if "trophycabinet: DISPLAY world=1 state=3 trophies=0x3" not in reload_proc.stdout:
        failures.append("second process did not display reloaded Dino gold trophy")
    if "quarantined" in reload_proc.stdout:
        failures.append("second process quarantined the production-written EEPROM")
    if len(reload_save) == EEPROM_BYTES:
        reloaded_trophies = read_bits(
            reload_save[:SLOT_BYTES], TROPHY_BIT_OFFSET, 10
        )
        if reloaded_trophies != 3:
            failures.append(
                f"reload process lost Dino gold (trophies=0x{reloaded_trophies:x})"
            )

    # Fail-closed control plus the formerly broken production QUIT branch. The
    # malformed result request must be rejected, round one is scored naturally,
    # QUIT returns to the lobby without an award, and cabinet re-entry restarts
    # the series at round zero.
    malformed = "0,0,2,3,4,5,6,7"
    quit_proc, quit_save = run_case(
        binary, rom, 8000, eeprom_image(), "quit_retry", malformed
    )
    quit_output = quit_proc.stdout
    if quit_proc.returncode != 0:
        failures.append(f"quit/retry process exit code {quit_proc.returncode}")
    if "trophyorder: REJECT round=0 invalid permutations" not in quit_output:
        failures.append("malformed/non-permutation finish order was not rejected")
    if "trophyquit: world=1 afterRound=0" not in quit_output:
        failures.append("production QUIT TROPHY RACE branch was not observed")
    if len(quit_save) == EEPROM_BYTES:
        quit_trophies = read_bits(
            quit_save[:SLOT_BYTES], TROPHY_BIT_OFFSET, 10
        )
        if quit_trophies != 0:
            failures.append(
                f"quit/retry arm persisted trophy 0x{quit_trophies:x}"
            )
    retry_proc, _retry_save = run_case(
        binary, rom, 6000, quit_save, "retry", None
    )
    if retry_proc.returncode != 0:
        failures.append(f"retry process exit code {retry_proc.returncode}")
    if "trophyseries: ENTER world=1" not in retry_proc.stdout:
        failures.append("post-quit EEPROM resume did not re-enter the cabinet")
    if "trophyround: world=1 round=0 track=5 points0=0" not in retry_proc.stdout:
        failures.append("post-quit retry did not restart at round zero")

    # The other three production championship schedules and podium branches.
    world_matrix = (
        (2, "1,0,2,3,4,5,6,7", (8, 4, 10, 30), 1, 28, 0x8),
        (3, "1,2,0,3,4,5,6,7", (13, 6, 9, 28), 2, 20, 0x10),
        (4, "1,2,3,0,4,5,6,7", (19, 18, 20, 31), 3, 12, 0x0),
    )
    matrix_outputs: list[tuple[int, str]] = []
    for world, per_round, tracks, rank, points, trophy_bits in world_matrix:
        order = "/".join((per_round,) * 4)
        matrix_proc, matrix_save = run_case(
            binary, rom, 13000, eeprom_image(), "full", order, world
        )
        matrix_output = matrix_proc.stdout
        matrix_outputs.append((world, matrix_output))
        if matrix_proc.returncode != 0:
            failures.append(f"world {world} process exit code {matrix_proc.returncode}")
        got_tracks = [
            int(match.group(3)) for match in ROUND_RE.finditer(matrix_output)
        ][:4]
        if got_tracks != list(tracks):
            failures.append(
                f"world {world} tracks {got_tracks}, expected {list(tracks)}"
            )
        matrix_award = AWARD_RE.search(matrix_output)
        if matrix_award is None:
            failures.append(f"world {world} final award decision missing")
        elif (
            int(matrix_award.group(1)), int(matrix_award.group(2)),
            int(matrix_award.group(3)), int(matrix_award.group(5), 16),
            int(matrix_award.group(6)),
        ) != (world, rank, points, trophy_bits, int(rank < 3)):
            failures.append(
                f"world {world} award tuple {matrix_award.groups()}"
            )
        if len(matrix_save) == EEPROM_BYTES:
            persisted = read_bits(
                matrix_save[:SLOT_BYTES], TROPHY_BIT_OFFSET, 10
            )
            if persisted != trophy_bits:
                failures.append(
                    f"world {world} persisted trophies=0x{persisted:x}, "
                    f"expected 0x{trophy_bits:x}"
                )

    if failures:
        print("FAIL: trophy series")
        for failure in failures:
            print(f"  - {failure}")
        if args.verbose:
            for line in output.splitlines():
                if any(token in line for token in (
                    "trophycabinet:", "trophyseries:", "trophyround:",
                    "trophycomplete:", "trophyorder:", "trophyrankings:",
                    "trophyaward:", "[CRASH]", "[FATAL]",
                )):
                    print("  " + line)
            for line in quit_output.splitlines():
                if any(token in line for token in (
                    "trophyorder:", "trophyquit:", "trophyseries:",
                    "trophyround:", "menu_init:",
                )):
                    print("  quit/retry: " + line)
        return 1
    print(
        "PASS: trophy series — cabinet entry, four production rounds, "
        "stable tie, gold + reload, fail-closed order, quit + retry"
    )
    if args.verbose:
        for line in output.splitlines():
            if any(token in line for token in (
                "trophycabinet:", "trophyseries:", "trophyround:",
                "trophyorder:", "trophyrankings:", "trophyaward:",
            )):
                print("  " + line)
        print("  reload: " + next(
            line for line in reload_proc.stdout.splitlines()
            if "trophycabinet: DISPLAY" in line
        ))
        for line in quit_output.splitlines():
            if any(token in line for token in (
                "trophyorder: REJECT", "trophyquit:",
                "trophyseries:", "trophyround:",
            )):
                print("  quit/retry: " + line)
        for line in retry_proc.stdout.splitlines():
            if "trophyseries:" in line or "trophyround:" in line:
                print("  resumed retry: " + line)
        for world, matrix_output in matrix_outputs:
            for line in matrix_output.splitlines():
                if "trophyaward:" in line:
                    print(f"  world {world}: " + line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
