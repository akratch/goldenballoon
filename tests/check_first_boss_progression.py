#!/usr/bin/env python3
"""Legal Adventure progression from the fourth Dino race through Tricky 1.

The checkpoint is a checksum-valid retail EEPROM state: three Dino races
cleared, two real Timber's Island balloons collected, and Taj's five-balloon car
challenge already offered.  From there the game must drive through the real hub
exits and doors, finish all three Hot Top Volcano laps, open the four-local-
balloon boss door, present Tricky's first challenge, and physically cross the
boss finish before either verdict path is allowed to run.

Both boss outcomes are exercised.  The win arm changes finishPosition only
after raceFinished is live; the loss arm uses the natural second place.  Raw
EEPROM is decoded independently and then loaded by a second process. A third,
deliberately broken collision-grid arm sets MDKR_BOSS_WIN, restores the old
Z-row mask defect, and omits the summit steering recovery. It must reach the
upper course, fail to finish, and produce no forced verdict. That control proves
the verdict hook cannot manufacture a finish without depending on a formerly
broken production racing line staying broken forever.

MDKR_AUTOPILOT_UNSTICK, and why the fourth race needs it
--------------------------------------------------------
The ASSET_AI_BEHAVIOUR byte-swap fix made the AI table decode correctly, and the
table drives the autopilot racer too (docs/asset_swap_notes.md).  On the
corrected data this route's fourth race wipes out at Hot Top Volcano's crater
jump: the kart leaves the track around checkpoint 7 at frame ~3025, lands wedged
at (-2139.4, -120.7, 1498.9), and stays there for the rest of the run.  It never
recovers because DKR's own stuck-recovery is gated on a cooldown (unk215) that
only decays while the kart is driving forwards, so a kart that stops with the
cooldown hot can never re-arm it - upstream behaviour, unchanged here.  The
campaign arms therefore re-arm that cooldown once the kart is provably immobile
and let the game's own reverse-out drive; the broken control does not set it.

Attribution is by A/B build, not inference: with only the ASSET_AI_BEHAVIOUR
swap reverted, this identical route reaches Tricky 1, finishes it and persists
every flag this check asserts - which is also what proves the four persistence
failures the release battery saw (Tricky 1 status, balloon counts, boss-door
cutscene flag, reloaded runtime state) were downstream of the wipeout and not a
save defect.  Measured on the fixed build: the recovery fires exactly twice, both
in Hot Top Volcano, and is asserted below never to fire during Tricky 1, so it
cannot be what carries the kart across the boss finish.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from harness_utils import (config_block as save_config_block, put_bits,
                           resolve_binary, seal_slot, SLOT_BYTES,
                           slot_checksum_valid)


ROOT = Path(__file__).resolve().parent.parent
CAMPAIGN_SCRIPT = ROOT / "tests/input_scripts/adventure_race_loop.txt"
BOSS_SCRIPT = ROOT / "tests/input_scripts/race_full_3lap_tt.txt"
RELOAD_SCRIPT = ROOT / "tests/input_scripts/adventure_resume_race.txt"

EEPROM_BYTES = 512
CONFIG_OFFSET = 120
RECORD_OFFSETS = (128, 320)
RECORD_BYTES = 192

ASSET_LEVEL_HEADERS_TABLE = 22
ASSET_LEVEL_HEADERS = 23
LEVELHEADER_RACE_TYPE = 0x4C
ROM_LAYOUTS = {
    (0xE402430D, 0xD2FCFC9D): (0x000ED0E0, 0x000ED1B0),  # us.v80
    (0x596E145B, 0xF7D9879F): (0x000ED170, 0x000ED240),  # pal.v80
}

HUB = 0
LOBBY = 12
HOT_TOP_VOLCANO = 7
TRICKY_ONE = 38
TRICKY_CUTSCENE = 57
FIRST_THREE = (5, 3, 29)

RACE_CLEARED_STATUS = 2
RACE_VISITED_STATUS = 1
TAJ_CAR_OFFERED = 0x01
CUTSCENE_DINO_BOSS_DOOR = 0x08
DINO_WORLD_BIT = 1 << 1

# These are actual ordinary balloons from level 0's object map. B10 lies on the
# route to Dino Domain; marking it collected is load-bearing, otherwise the
# synthetic checkpoint silently gains an extra balloon while driving past it.
HUB_BALLOON_FLAGS = (1 << 10) | (1 << 14)

CAMPAIGN_FRAMES = 22000
BROKEN_FRAMES = 9400
RELOAD_FRAMES = 2400
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:-3381,1946:-3948,2180:E12"
    ";12:E7:E38"
)

LEVEL_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=(-?\d+) entrance=(-?\d+) "
    r"vehicle=(-?\d+) cutscene=(-?\d+) @frame~(\d+)"
)
EXIT_RE = re.compile(
    r"drive: level=(\d+) step \d+: exit to (\d+) TAKEN "
    r"\(now in level (\d+)\) @frame~(\d+)"
)
ADV_VERDICT_RE = re.compile(
    r"adventureforceverdict: requested=win natural=(\d+) final=(\d+) "
    r"lap=(\d+).*@frame~(\d+)"
)
BOSS_REDIRECT_RE = re.compile(
    r"bossredirect: boss=(\d+) -> cutsceneLevel=(\d+) cutsceneId=(\d+) "
    r"bosses=0x([0-9a-f]+) courseFlags=0x([0-9a-f]+) worldId=(-?\d+)"
)
BOSS_ROUTE_RE = re.compile(
    r"bossroute: level=38 checkpoint=(\d+) steering through summit finish "
    r"@frame~(\d+)"
)
BOSS_FORCE_RE = re.compile(
    r"bossforcewin: finishPosition (-?\d+) -> 1 "
    r"\(playerIndex=(-?\d+) raceFinished=(\d+)\) @frame~(\d+)"
)
BOSS_FINISH_RE = re.compile(
    r"bossfinish: finishPos=(-?\d+) playerIndex=(-?\d+) courseId=(\d+) "
    r"worldId=(-?\d+) bosses=0x([0-9a-f]+) courseFlags=0x([0-9a-f]+) "
    r"cutsceneLevel=(-?\d+) timer=(-?\d+) raceFinished=(-?\d+)"
)
BOSS_WATCH_RE = re.compile(
    r"\[BOSSW\] frame=(\d+) courseFlags\[38\]=0x([0-9a-f]+).*"
    r"bosses=0x([0-9a-f]+)"
)
OBJ_HIT_RE = re.compile(r"\[OBJCOLL\] hit #(\d+) frame=(\d+)")
UNSTICK_RE = re.compile(
    r"autopilotunstick: level=(\d+) wedged at \((\S+), (\S+), (\S+)\) "
    r"checkpoint=(\d+) cooldown=(\d+) -> 0 @frame~(\d+)"
)
PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+) "
    r"clock=(\d+) cp=(-?\d+) lap=(-?\d+) rlap=(-?\d+) fin=(-?\d+)"
)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)


def read_bits(data: bytes, offset: int, width: int) -> int:
    value = 0
    for bit in range(offset, offset + width):
        value = (value << 1) | ((data[bit // 8] >> (7 - bit % 8)) & 1)
    return value


def normalize_rom(rom: bytes) -> bytes:
    if rom[:4] == b"\x80\x37\x12\x40":
        return rom
    out = bytearray(len(rom))
    if rom[:4] == b"\x37\x80\x40\x12":
        out[0::2], out[1::2] = rom[1::2], rom[0::2]
    elif rom[:4] == b"\x40\x12\x37\x80":
        out[0::4], out[1::4] = rom[3::4], rom[2::4]
        out[2::4], out[3::4] = rom[1::4], rom[0::4]
    else:
        raise ValueError(f"unsupported ROM byte order {rom[:4].hex()}")
    return bytes(out)


def rom_section(
    rom: bytes, index: int, lut_start: int, data_base: int
) -> bytes:
    start = struct.unpack_from(">I", rom, lut_start + 4 * (index + 1))[0]
    end = struct.unpack_from(">I", rom, lut_start + 4 * (index + 2))[0]
    return rom[data_base + start:data_base + end]


def save_order(rom_path: str) -> list[int]:
    rom = normalize_rom(Path(rom_path).read_bytes())
    crc = struct.unpack_from(">II", rom, 0x10)
    if crc not in ROM_LAYOUTS:
        raise ValueError(f"unsupported ROM CRC {crc[0]:08x}/{crc[1]:08x}")
    lut_start, data_base = ROM_LAYOUTS[crc]
    raw = rom_section(
        rom, ASSET_LEVEL_HEADERS_TABLE, lut_start, data_base
    )
    table = list(struct.unpack(f">{len(raw) // 4}i", raw))
    table_end = table.index(-1)
    headers = rom_section(rom, ASSET_LEVEL_HEADERS, lut_start, data_base)
    eligible: list[int] = []
    for level_id in range(table_end - 1):
        race_type = headers[table[level_id] + LEVELHEADER_RACE_TYPE]
        if race_type == 0 or (race_type & 0x40) or race_type == 8:
            eligible.append(level_id)
    return eligible


def config_block() -> bytes:
    # retail subtitle default; Adventure Two remains locked
    return save_config_block(1)


def sum_block(size: int) -> bytes:
    return bytes(seal_slot(bytearray(size)))


def checkpoint_slot(eligible: list[int]) -> bytes:
    status = {level: RACE_CLEARED_STATUS for level in FIRST_THREE}
    bits: list[int] = []
    put_bits(bits, 16, 0)
    for ordinal in range(34):
        level = eligible[ordinal] if ordinal < len(eligible) else -1
        put_bits(bits, 2, status.get(level, 0))
    put_bits(bits, 6, TAJ_CAR_OFFERED)
    put_bits(bits, 10, 0)  # trophies
    put_bits(bits, 12, 0)  # bosses
    for value in (5, 3, 0, 0, 0, 0):
        put_bits(bits, 7, value)
    put_bits(bits, 3, 0)  # TT amulet
    put_bits(bits, 3, 0)  # Wizpig amulet
    put_bits(bits, 16, HUB_BALLOON_FLAGS)
    for _ in range(5):
        put_bits(bits, 16, 0)
    put_bits(bits, 8, 0)       # keys
    put_bits(bits, 32, 0)      # cutscene flags
    put_bits(bits, 16, 0x1234) # named/started file
    put_bits(bits, 8, 0)
    if len(bits) != SLOT_BYTES * 8:
        raise AssertionError(f"checkpoint encoded {len(bits)} bits")
    slot = bytearray(
        sum(bits[i + j] << (7 - j) for j in range(8))
        for i in range(0, len(bits), 8)
    )
    return bytes(seal_slot(slot))


def checkpoint_image(eligible: list[int]) -> bytes:
    out = bytearray(EEPROM_BYTES)
    out[:SLOT_BYTES] = checkpoint_slot(eligible)
    out[SLOT_BYTES:CONFIG_OFFSET] = b"\xFF" * (
        CONFIG_OFFSET - SLOT_BYTES
    )
    out[CONFIG_OFFSET:CONFIG_OFFSET + 8] = config_block()
    for offset in RECORD_OFFSETS:
        out[offset:offset + RECORD_BYTES] = sum_block(RECORD_BYTES)
    return bytes(out)


def decode_slot(save: bytes, eligible: list[int]) -> dict[str, object]:
    if len(save) != EEPROM_BYTES:
        raise ValueError(f"EEPROM is {len(save)} bytes, expected {EEPROM_BYTES}")
    slot = save[:SLOT_BYTES]
    course = {
        level: read_bits(slot, 16 + eligible.index(level) * 2, 2)
        for level in (*FIRST_THREE, HOT_TOP_VOLCANO, TRICKY_ONE)
    }
    return {
        "checksum_ok": slot_checksum_valid(slot),
        "course": course,
        "taj": read_bits(slot, 84, 6),
        "bosses": read_bits(slot, 100, 12),
        "balloons": tuple(
            read_bits(slot, 112 + i * 7, 7) for i in range(6)
        ),
        "tt_amulet": read_bits(slot, 154, 3),
        "wizpig_amulet": read_bits(slot, 157, 3),
        "hub_flags": read_bits(slot, 160, 16),
        "cutscenes": read_bits(slot, 264, 32),
    }


def clean_env() -> dict[str, str]:
    return {
        key: value for key, value in os.environ.items()
        if not key.startswith("MDKR_")
    }


def invoke(
    binary: str,
    rom: str,
    cwd: str,
    frames: int,
    script: Path,
    values: dict[str, str],
) -> tuple[int | str, str]:
    env = clean_env()
    env.update(
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_SIMULATION_CADENCE="original",
        MDKR_SYNTH_FIELDS="2",
    )
    env.update(values)
    command = [
        binary, "--headless-frames", str(frames),
        "--input-script", str(script), "--rom", rom,
    ]
    try:
        proc = subprocess.run(
            command, cwd=cwd, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=max(180, frames // 10),
        )
        return proc.returncode, proc.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or b"").decode("utf-8", "replace")
        return "timeout", output


def run_campaign(
    binary: str,
    rom: str,
    checkpoint: bytes,
    win: bool,
    break_invariant: bool,
) -> dict[str, object]:
    with tempfile.TemporaryDirectory(prefix="mdkr_first_boss_") as run_dir:
        save_dir = Path(run_dir) / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(checkpoint)
        values = {
            "MDKR_AUTOPILOT": "1",
            "MDKR_ADVENTURE_WIN": "1",
            "MDKR_DRIVE_ROUTE": DRIVE_ROUTE,
            "MDKR_WATCH_COURSEFLAGS": str(TRICKY_ONE),
            "MDKR_OBJCOLL": "trace",
            # Hot Top Volcano's corrected AI line wipes out at the crater jump
            # and wedges the kart where DKR's own recovery can no longer re-arm.
            # See the module docstring; the broken control deliberately omits it.
            "MDKR_AUTOPILOT_UNSTICK": "1",
        }
        if not (win and break_invariant):
            values["MDKR_BOSS_ROUTE"] = "1"
        if win:
            values["MDKR_BOSS_WIN"] = "1"
        rc, output = invoke(
            binary, rom, run_dir, CAMPAIGN_FRAMES, CAMPAIGN_SCRIPT, values
        )
        save = (save_dir / "eeprom.bin").read_bytes()

        reload_rc, reload_output = invoke(
            binary, rom, run_dir, RELOAD_FRAMES, RELOAD_SCRIPT,
            {"MDKR_WATCH_COURSEFLAGS": str(TRICKY_ONE)},
        )
        return {
            "rc": rc,
            "out": output,
            "save": save,
            "reload_rc": reload_rc,
            "reload_out": reload_output,
        }


def run_broken_control(
    binary: str, rom: str
) -> dict[str, object]:
    with tempfile.TemporaryDirectory(prefix="mdkr_first_boss_control_") as run_dir:
        save_dir = Path(run_dir) / "save"
        save_dir.mkdir()
        # The direct boss fixture follows the Tracks-menu script and therefore
        # needs an empty save directory. A started Adventure file changes that
        # menu route before MDKR_LOAD_TRACK can retarget the race.
        rc, output = invoke(
            binary, rom, run_dir, BROKEN_FRAMES, BOSS_SCRIPT,
            {
                "MDKR_AUTOPILOT": "1",
                "MDKR_LOAD_TRACK": str(TRICKY_ONE),
                "MDKR_BOSS_WIN": "1",
                "MDKR_OBJCOLL": "trace",
                "MDKR_GRIDMASK": "off",
            },
        )
        return {"rc": rc, "out": output}


def parsed(output: str) -> dict[str, object]:
    return {
        "loads": [
            tuple(map(int, match.groups())) for match in LEVEL_RE.finditer(output)
        ],
        "exits": [
            tuple(map(int, match.groups())) for match in EXIT_RE.finditer(output)
        ],
        "adv": [
            tuple(map(int, match.groups()))
            for match in ADV_VERDICT_RE.finditer(output)
        ],
        "redirects": [
            (
                int(match.group(1)), int(match.group(2)), int(match.group(3)),
                int(match.group(4), 16), int(match.group(5), 16),
                int(match.group(6)),
            )
            for match in BOSS_REDIRECT_RE.finditer(output)
        ],
        "routes": [
            tuple(map(int, match.groups()))
            for match in BOSS_ROUTE_RE.finditer(output)
        ],
        "forces": [
            tuple(map(int, match.groups()))
            for match in BOSS_FORCE_RE.finditer(output)
        ],
        "finishes": [
            (
                int(match.group(1)), int(match.group(2)), int(match.group(3)),
                int(match.group(4)), int(match.group(5), 16),
                int(match.group(6), 16), int(match.group(7)),
                int(match.group(8)), int(match.group(9)),
            )
            for match in BOSS_FINISH_RE.finditer(output)
        ],
        "watch": [
            (int(match.group(1)), int(match.group(2), 16),
             int(match.group(3), 16))
            for match in BOSS_WATCH_RE.finditer(output)
        ],
        "hits": [
            tuple(map(int, match.groups())) for match in OBJ_HIT_RE.finditer(output)
        ],
        "unstick": [
            (int(match.group(1)), int(match.group(5)), int(match.group(7)))
            for match in UNSTICK_RE.finditer(output)
        ],
        "pace": [
            (
                int(match.group(1)), float(match.group(2)),
                float(match.group(3)), float(match.group(4)),
                int(match.group(5)), int(match.group(6)),
                int(match.group(7)), int(match.group(8)),
                int(match.group(9)),
            )
            for match in PACE_RE.finditer(output)
        ],
        "bad": BAD_RE.findall(output),
    }


def validate_campaign(
    name: str,
    result: dict[str, object],
    state: dict[str, object],
    failures: list[str],
) -> dict[str, object]:
    data = parsed(str(result["out"]))
    if result["rc"] != 0:
        failures.append(f"{name}: process exit {result['rc']}")
    if data["bad"]:
        failures.append(f"{name}: bad runtime marker {data['bad'][0]}")

    loads = data["loads"]
    gameplay = [row for row in loads if row[0] in (HUB, LOBBY, HOT_TOP_VOLCANO, TRICKY_ONE, TRICKY_CUTSCENE)]
    ids = [row[0] for row in gameplay]
    cursor = -1
    for wanted in (HUB, LOBBY, HOT_TOP_VOLCANO, LOBBY, TRICKY_ONE, TRICKY_ONE, TRICKY_CUTSCENE, LOBBY):
        try:
            cursor = ids.index(wanted, cursor + 1)
        except ValueError:
            failures.append(
                f"{name}: legal load chain missing {wanted}; saw {ids}"
            )
            break

    for source, dest in ((HUB, LOBBY), (LOBBY, HOT_TOP_VOLCANO), (LOBBY, TRICKY_ONE)):
        if not any(row[0] == source and row[1] == dest for row in data["exits"]):
            failures.append(f"{name}: no production exit witness {source}->{dest}")

    if len(data["adv"]) != 1:
        failures.append(
            f"{name}: fourth race emitted {len(data['adv'])} verdict witnesses"
        )
    elif data["adv"][0][1:3] != (1, 3):
        failures.append(
            f"{name}: fourth-race final/lap={data['adv'][0][1:3]}, want (1, 3)"
        )

    expected_redirect = (TRICKY_ONE, TRICKY_CUTSCENE, 3, 0, 0, 1)
    if expected_redirect not in data["redirects"]:
        failures.append(
            f"{name}: first-visit challenge redirect {expected_redirect} absent; "
            f"saw {data['redirects']}"
        )
    # The stuck-recovery re-arm exists for the fourth race's crater wipeout only.
    # If it ever fires inside Tricky 1 the boss finish stops being a property of
    # the production racing line, so that is a failure, not a rescue.
    off_course = [row for row in data["unstick"] if row[0] != HOT_TOP_VOLCANO]
    if off_course:
        failures.append(
            f"{name}: autopilot stuck-recovery fired outside Hot Top Volcano: "
            f"{off_course}"
        )
    if len(data["routes"]) != 1 or data["routes"][0][0] != 36:
        failures.append(
            f"{name}: summit steering did not activate at checkpoint 36; "
            f"saw {data['routes']}"
        )

    boss_loads = [row for row in loads if row[0] == TRICKY_ONE]
    boss_race_frame = boss_loads[-1][5] if len(boss_loads) >= 2 else 10**9
    boss_hits = [frame for _count, frame in data["hits"] if frame >= boss_race_frame]
    if not boss_hits:
        failures.append(
            f"{name}: no production object-collision hits during Tricky 1"
        )

    finish = data["finishes"]
    if len(finish) != 1:
        failures.append(f"{name}: bossfinish count {len(finish)}, expected 1")
    else:
        expected_pos = 1 if name == "win" else 2
        if finish[0][0] != expected_pos:
            failures.append(
                f"{name}: boss verdict read position {finish[0][0]}, "
                f"want {expected_pos}"
            )
        if finish[0][2:4] != (TRICKY_ONE, 1) or finish[0][8] != 1:
            failures.append(
                f"{name}: bossfinish did not read course/world/raceFinished "
                f"(38,1,1): {finish[0]}"
            )

    expected_cutscene = 4 if name == "win" else 5
    if not any(
        row[0] == TRICKY_CUTSCENE
        and row[2] == expected_cutscene
        and row[4] == expected_cutscene
        for row in loads
    ):
        failures.append(f"{name}: post-race cutscene {expected_cutscene} never loaded")

    if name == "win":
        if len(data["forces"]) != 1:
            failures.append(
                f"win: post-finish verdict control count {len(data['forces'])}, expected 1"
            )
        elif data["forces"][0][2] != 1:
            failures.append(
                f"win: MDKR_BOSS_WIN fired before physical finish: {data['forces'][0]}"
            )
    elif data["forces"]:
        failures.append(f"loss: unexpected boss win force {data['forces']}")

    if not state["checksum_ok"]:
        failures.append(f"{name}: persisted slot checksum invalid")
    for level in FIRST_THREE:
        if state["course"][level] != RACE_CLEARED_STATUS:
            failures.append(
                f"{name}: prior course {level} changed to {state['course'][level]}"
            )
    if state["course"][HOT_TOP_VOLCANO] != RACE_CLEARED_STATUS:
        failures.append(
            f"{name}: Hot Top Volcano status {state['course'][HOT_TOP_VOLCANO]}, want cleared"
        )
    expected_boss_status = (
        RACE_CLEARED_STATUS if name == "win" else RACE_VISITED_STATUS
    )
    if state["course"][TRICKY_ONE] != expected_boss_status:
        failures.append(
            f"{name}: Tricky 1 status {state['course'][TRICKY_ONE]}, "
            f"want {expected_boss_status}"
        )
    expected_bosses = DINO_WORLD_BIT if name == "win" else 0
    if state["bosses"] != expected_bosses:
        failures.append(
            f"{name}: bosses=0x{state['bosses']:x}, want 0x{expected_bosses:x}"
        )
    if state["balloons"][:2] != (6, 4) or any(state["balloons"][2:]):
        failures.append(
            f"{name}: balloon counts {state['balloons']}, want (6,4,0,0,0,0)"
        )
    if state["tt_amulet"] != 0 or state["wizpig_amulet"] != 0:
        failures.append(
            f"{name}: first boss incorrectly awarded an amulet "
            f"(TT={state['tt_amulet']}, Wizpig={state['wizpig_amulet']}); "
            "the amulet piece belongs to the second boss"
        )
    if not state["cutscenes"] & CUTSCENE_DINO_BOSS_DOOR:
        failures.append(f"{name}: Dino boss-door cutscene flag was not persisted")
    if state["hub_flags"] & HUB_BALLOON_FLAGS != HUB_BALLOON_FLAGS:
        failures.append(
            f"{name}: checkpoint hub balloon flags were lost "
            f"0x{state['hub_flags']:04x}"
        )

    if result["reload_rc"] != 0:
        failures.append(f"{name}: reload process exit {result['reload_rc']}")
    reload_data = parsed(str(result["reload_out"]))
    expected_runtime = (3, DINO_WORLD_BIT) if name == "win" else (1, 0)
    if not any(
        flags == expected_runtime[0] and bosses == expected_runtime[1]
        for _frame, flags, bosses in reload_data["watch"]
    ):
        failures.append(
            f"{name}: second process did not reload runtime boss state "
            f"{expected_runtime}; saw {reload_data['watch']}"
        )
    if not any(row[0] == HUB for row in reload_data["loads"]):
        failures.append(f"{name}: persisted file did not resume into Timber's Island")
    return data


def validate_broken_control(
    result: dict[str, object], failures: list[str]
) -> None:
    data = parsed(str(result["out"]))
    if result["rc"] != 0:
        failures.append(f"broken control: process exit {result['rc']}")
    if data["bad"]:
        failures.append(f"broken control: bad marker {data['bad'][0]}")
    if data["routes"]:
        failures.append("broken control: summit recovery unexpectedly activated")
    if data["forces"]:
        failures.append(
            "broken control: MDKR_BOSS_WIN manufactured a verdict before finish"
        )
    if data["finishes"]:
        failures.append(
            "broken control: legacy collision-grid fault unexpectedly finished; "
            "the positive control no longer reproduces the terrain-loss defect"
        )
    if any(row[0] == TRICKY_CUTSCENE and row[4] == 4 for row in data["loads"]):
        failures.append("broken control: win cutscene loaded without bossfinish")
    if max((row[5] for row in data["pace"]), default=-1) < 37:
        failures.append(
            "broken control: human never reached checkpoint 37, so the missed "
            "finish is not reproduced"
        )
    boss_loads = [row for row in data["loads"] if row[0] == TRICKY_ONE]
    race_frame = boss_loads[-1][5] if boss_loads else 10**9
    if not any(frame >= race_frame for _count, frame in data["hits"]):
        failures.append("broken control: object collision was not exercised")
    if not any(row[0] == HUB and row[5] > race_frame for row in data["loads"]):
        failures.append("broken control: missed summit did not return to the hub")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument(
        "--break-invariant", action="store_true",
        help="omit summit recovery in the full win arm; the check must fail",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = os.path.abspath(resolve_binary(args.build))
    rom = os.path.abspath(args.rom)
    for path in (binary, rom, str(CAMPAIGN_SCRIPT), str(BOSS_SCRIPT), str(RELOAD_SCRIPT)):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    try:
        eligible = save_order(rom)
        for level in (*FIRST_THREE, HOT_TOP_VOLCANO, TRICKY_ONE):
            if level not in eligible:
                raise ValueError(f"level {level} absent from save ordering")
        checkpoint = checkpoint_image(eligible)
        checkpoint_state = decode_slot(checkpoint, eligible)
    except (OSError, ValueError, IndexError, struct.error) as exc:
        print(f"FAIL: could not build canonical checkpoint: {exc}")
        return 1

    failures: list[str] = []
    if (
        not checkpoint_state["checksum_ok"]
        or checkpoint_state["balloons"] != (5, 3, 0, 0, 0, 0)
        or checkpoint_state["hub_flags"] != HUB_BALLOON_FLAGS
        or checkpoint_state["taj"] != TAJ_CAR_OFFERED
    ):
        failures.append(f"checkpoint self-check failed: {checkpoint_state}")

    print("First Adventure boss: canonical 3-race checkpoint")
    results: dict[str, dict[str, object]] = {}
    parsed_arms: dict[str, dict[str, object]] = {}
    for name, win in (("win", True), ("loss", False)):
        if args.verbose:
            print(f"  running full {name} campaign arm ({CAMPAIGN_FRAMES} frames)")
        result = run_campaign(
            binary, rom, checkpoint, win, args.break_invariant
        )
        results[name] = result
        try:
            state = decode_slot(bytes(result["save"]), eligible)
        except (ValueError, IndexError) as exc:
            failures.append(f"{name}: EEPROM decode failed: {exc}")
            state = {
                "checksum_ok": False, "course": {},
                "bosses": -1, "balloons": (), "tt_amulet": -1,
                "wizpig_amulet": -1, "cutscenes": 0, "hub_flags": -1,
            }
        parsed_arms[name] = validate_campaign(
            name, result, state, failures
        )
        print(
            f"  {name:4s}: rc={result['rc']} reload={result['reload_rc']} "
            f"bossfinish={len(parsed_arms[name]['finishes'])}"
        )

    if parsed_arms["win"]["adv"] and parsed_arms["loss"]["adv"]:
        win_natural = parsed_arms["win"]["adv"][0][0]
        loss_natural = parsed_arms["loss"]["adv"][0][0]
        if win_natural != loss_natural:
            failures.append(
                f"identical fourth-race arms had different natural places "
                f"{win_natural} vs {loss_natural}"
            )

    if args.verbose:
        print("  running legacy collision-grid positive control")
    broken = run_broken_control(binary, rom)
    validate_broken_control(broken, failures)
    broken_data = parsed(str(broken["out"]))
    print(
        f"  control: rc={broken['rc']} maxcp="
        f"{max((row[5] for row in broken_data['pace']), default=-1)} "
        f"bossfinish={len(broken_data['finishes'])} "
        f"forced={len(broken_data['forces'])}"
    )

    if failures:
        print(f"FAIL: first boss progression ({len(failures)} issue(s))")
        for failure in failures:
            print("  - " + failure)
        return 1
    print(
        "PASS: legal fourth Dino race, boss-door/challenge, physical Tricky 1 "
        "finish, win/loss cutscenes, flags, hub return, EEPROM save/reload; "
        "legacy collision-grid fault proves verdict control cannot manufacture "
        "a finish"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
