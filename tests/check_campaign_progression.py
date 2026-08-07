#!/usr/bin/env python3
"""Witness the Adventure campaign's late progression seams, end to end.

Why this exists
---------------
The silver-coins -> boss-rematch -> Wizpig chain is decompiled retail logic that
has always been present and has never been watched. `check_first_boss_progression.py`
closes the campaign up to a world's FIRST boss; `check_bluey2_rematch.py` proves
one rematch race runs and is lost naturally. Everything after that -- collecting
silver coins at all, the flag they write, the world balloons they feed, the
rematch warp they unlock, the Wizpig amulet the rematch awards, and the Wizpig 2
win that selects the true-ending credits -- was ungated.

A single continuous start-to-credits drive would run for hours and would fail as
one opaque blob. This check instead gates each SEAM: a fixture that enters it,
and an assertion on what the game writes leaving it. The fixtures compose, so the
chain of (fixture, seam) pairs is a witnessed campaign. Fixture legitimacy -- how
every bit of fixture N+1 is something seam N is proved to write -- is derived in
tests/fixtures/README.md and re-derived in code by `derive_*` below.

The seams
---------
A  Silver coins persist.       Ancient Lake, entered through the real hub and
                               lobby from the state check_first_boss_progression
                               leaves behind. All eight coins are collected by
                               the game's own coin objects; the flag, the balloon
                               counts and the EEPROM round-trip are asserted, the
                               last of them from a second process.
B  Rematch awards the amulet.  Three worlds' second boss races, won. Each must
                               set the world's rematch boss bit and advance
                               wizpigAmulet exactly once. The negative control is
                               the same race from a save where the FIRST boss was
                               never beaten: production must award the first-boss
                               bit and NO amulet, which is what proves the amulet
                               is gated on the rematch actually being a rematch.
E  Wizpig 2 selects the true   The final boss, won. `menu_credits_init` chooses
   ending.                     between "THE END" (cheat), "THE END?" (legitimate,
                               Wizpig 2 not beaten) and "TO BE CONTINUED ..." +
                               SEQUENCE_CRESCENT_ISLAND (legitimate, Wizpig 2
                               beaten) purely on `gViewingCreditsFromCheat` and
                               `settings->bosses & 0x20`. This arm proves the
                               campaign sets and persists that bit, and the cheat
                               arm proves the cheat route reaches credits without
                               it -- so the two credits endings are distinguished
                               by state this check owns end to end.

What this deliberately does NOT prove
-------------------------------------
See tests/fixtures/README.md, "Residual manual acceptance". In short: Wizpig 1's
own race, the T.T.-amulet challenge levels, the lighthouse/Future Fun Land
unlock, and the credits SCREEN reached by finishing Wizpig 2 rather than by cheat
are not driven here. Each has a specific headless obstacle, recorded there. None
of them is weakened into a vacuous assertion in this file.

Every EEPROM image is built in a private temporary directory and deleted. No
ROM-derived data is committed by this check.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from check_first_boss_progression import (
    normalize_rom,
    read_bits,
    RECORD_BYTES,
    RECORD_OFFSETS,
    rom_section,
    ROM_LAYOUTS,
    save_order,
    sum_block,
)
from harness_utils import (config_block as save_config_block, DEFAULT_BUILD_DIR,
                           put_bits, resolve_binary, seal_slot, SLOT_BYTES,
                           slot_checksum_valid)

import struct


ROOT = Path(__file__).resolve().parent.parent
RESUME_SCRIPT = ROOT / "tests/input_scripts/adventure_resume_race.txt"
LOOP_SCRIPT = ROOT / "tests/input_scripts/adventure_race_loop.txt"
CHEAT_SCRIPT = ROOT / "tests/input_scripts/credits_via_cheat.txt"

EEPROM_BYTES = 512
CONFIG_OFFSET = 120

# Save slot bit offsets. Derived from the write order in
# game/src/save_data.c func_800732E8 (and the mirrored read at :394); the same
# offsets are used independently by check_first_boss_progression.decode_slot.
BIT_COURSES = 16
BIT_TAJ = 84
BIT_TROPHIES = 90
BIT_BOSSES = 100
BIT_BALLOONS = 112
BIT_TT_AMULET = 154
BIT_WIZPIG_AMULET = 157
BIT_WORLD_FLAGS = 160
BIT_KEYS = 256
BIT_CUTSCENES = 264
BIT_FILENAME = 296

# courseFlagsPtr low bits -- game/src/menu.h:298.
RACE_VISITED = 1 << 0
RACE_CLEARED = 1 << 1
RACE_CLEARED_SILVER_COINS = 1 << 2
# ...and the 2-bit on-disk status they collapse to (platform/save_codec.c:93).
STATUS_VISITED = 1
STATUS_CLEARED = 2
STATUS_SILVER = 3

# game/include/enums.h:71 -- the world ids the save and the boss bits index by.
WORLD_CENTRAL_AREA = 0
WORLD_FUTURE_FUN_LAND = 5
# settings->bosses is 12 bits: 1 << world is the world's first boss,
# 1 << (world + 6) its rematch (game/src/vehicle_tricky.c:378-386).
BOSS_FIRST_BIT = lambda world: 1 << world          # noqa: E731
BOSS_REMATCH_BIT = lambda world: 1 << (world + 6)  # noqa: E731
BOSS_WIZPIG_TWO_BIT = 1 << WORLD_FUTURE_FUN_LAND   # 0x20, the credits selector

# Level ids are ordinals of AssetLevelHeadersEnum (game/include/asset_enums.h).
# World membership is NOT taken from those names -- it is read out of each level
# header's own `world` byte below, because the retail naming does not track the
# world enum (Whale Bay and friends are world 2, not the "Snowflake" the name
# suggests) and a fixture that guesses wrong silently exercises nothing.
LEVEL_HEADER_WORLD = 0x00
LEVEL_HEADER_RACE_TYPE = 0x4C
ASSET_LEVEL_HEADERS_TABLE = 22
ASSET_LEVEL_HEADERS = 23
RACETYPE_DEFAULT = 0
RACETYPE_BOSS = 8

ANCIENT_LAKE = 5
WIZPIG_TWO = 55
WIZPIG_AMULET_SEQUENCE = 43
MENU_CREDITS = 25

# Two hub balloons the canonical route drives past. B10 lies on the way to Dino
# Domain, so marking it collected keeps the fixture from silently gaining one.
HUB_BALLOON_FLAGS = (1 << 10) | (1 << 14)
# CUTSCENE_DINO_BOSS_DOOR: the four-balloon boss-door scene, which
# check_first_boss_progression asserts is persisted by the run that beats
# Tricky 1. `<< 5` is the same scene's eight-balloon sibling (game/src/game.c:766).
CUTSCENE_BOSS_DOOR = lambda world: (8 << (world - 1)) | (0x100 << (world - 1))  # noqa: E731

# The canonical Timber's Island -> Dino Domain lobby drive, shared with
# check_first_boss_progression; the trailing step is per-arm.
HUB_TO_DINO = "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"

LEVEL_RE = re.compile(
    r"level_load: levelId=(?P<level>\d+) numPlayers=(?P<players>-?\d+) "
    r"entrance=(?P<entrance>-?\d+) vehicle=-?\d+ cutscene=(?P<cutscene>-?\d+) "
    r"@frame~(?P<frame>\d+)"
)
COIN_RE = re.compile(
    r"silvercoin: playerIndex=(?P<player>\d+) count=(?P<count>\d+) "
    r"action=0x[0-9a-f]+ @frame~(?P<frame>\d+)"
)
COIN_RACE_RE = re.compile(
    r"silvercoinrace: courseId=(?P<course>\d+) worldId=(?P<world>-?\d+) "
    r"bosses=0x(?P<bosses>[0-9a-f]+) courseFlags=0x(?P<flags>[0-9a-f]+) "
    r"raceType=(?P<racetype>-?\d+) tracksMode=(?P<tracks>-?\d+) "
    r"verdict=(?P<verdict>-?\d+)"
)
COIN_FINISH_RE = re.compile(
    r"silvercoinfinish: courseId=(?P<course>\d+) leadPlayerIndex=(?P<lead>-?\d+) "
    r"coins=(?P<coins>-?\d+) silverRace=(?P<silver>-?\d+) timeTrial=(?P<tt>-?\d+) "
    r"courseFlags=0x(?P<flags>[0-9a-f]+)"
)
WATCH_RE = re.compile(
    r"\[BOSSW\] frame=(?P<frame>\d+) courseFlags\[(?P<level>\d+)\]="
    r"0x(?P<flags>[0-9a-f]+) \(was 0x(?P<was>[0-9a-f]+)\) "
    r"bosses=0x(?P<bosses>[0-9a-f]+)"
)
BOSS_FINISH_RE = re.compile(
    r"bossfinish: finishPos=(?P<position>-?\d+) playerIndex=(?P<player>-?\d+) "
    r"courseId=(?P<course>\d+) worldId=(?P<world>-?\d+) "
    r"bosses=0x(?P<bosses>[0-9a-f]+) courseFlags=0x(?P<flags>[0-9a-f]+)"
)
BOSS_FORCE_RE = re.compile(
    r"bossforcewin: finishPosition (?P<old>-?\d+) -> 1 "
    r"\(playerIndex=-?\d+ raceFinished=(?P<finished>\d+)\)"
)
MENU_RE = re.compile(r"menu_init: menuId=(?P<menu>\d+) @frame~(?P<frame>\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)


# --------------------------------------------------------------- ROM topology


def level_worlds(rom_path: str) -> dict[int, tuple[int, int]]:
    """levelId -> (world, race_type), read from the ROM's own level headers."""
    rom = normalize_rom(Path(rom_path).read_bytes())
    crc = struct.unpack_from(">II", rom, 0x10)
    if crc not in ROM_LAYOUTS:
        raise ValueError(f"unsupported ROM CRC {crc[0]:08x}/{crc[1]:08x}")
    lut_start, data_base = ROM_LAYOUTS[crc]
    raw = rom_section(rom, ASSET_LEVEL_HEADERS_TABLE, lut_start, data_base)
    table = list(struct.unpack(f">{len(raw) // 4}i", raw))
    table_end = table.index(-1)
    headers = rom_section(rom, ASSET_LEVEL_HEADERS, lut_start, data_base)
    out: dict[int, tuple[int, int]] = {}
    for level in range(table_end - 1):
        base = table[level]
        world = struct.unpack_from(">b", headers, base + LEVEL_HEADER_WORLD)[0]
        out[level] = (world, headers[base + LEVEL_HEADER_RACE_TYPE])
    return out


@dataclass(frozen=True)
class WorldTopology:
    world: int
    races: tuple[int, ...]
    first_boss: int
    rematch_boss: int


def world_topology(rom_path: str, eligible: list[int]) -> dict[int, WorldTopology]:
    """Group the save-eligible courses into worlds using the ROM's own headers.

    A world's two boss courses are distinguished by level id order: DKR always
    authors the rematch after the first encounter in the level table.
    """
    info = level_worlds(rom_path)
    races: dict[int, list[int]] = {}
    bosses: dict[int, list[int]] = {}
    for level in eligible:
        world, race_type = info[level]
        if world <= 0:
            continue
        if race_type == RACETYPE_DEFAULT:
            races.setdefault(world, []).append(level)
        elif race_type == RACETYPE_BOSS:
            bosses.setdefault(world, []).append(level)
    out: dict[int, WorldTopology] = {}
    for world, course_list in races.items():
        boss_list = sorted(bosses.get(world, []))
        # Four worlds have a first boss and a rematch; Future Fun Land has only
        # Wizpig 2, which is why it is never a seam B world.
        if len(course_list) != 4 or not 1 <= len(boss_list) <= 2:
            continue
        out[world] = WorldTopology(
            world=world,
            races=tuple(sorted(course_list)),
            first_boss=boss_list[0],
            rematch_boss=boss_list[-1],
        )
    return out


# ------------------------------------------------------------------ fixtures


@dataclass
class Slot:
    """A campaign save slot, addressed by the fields production writes."""

    status: dict[int, int] = field(default_factory=dict)
    taj: int = 0
    trophies: int = 0
    bosses: int = 0
    balloons: list[int] = field(default_factory=lambda: [0] * 6)
    tt_amulet: int = 0
    wizpig_amulet: int = 0
    world_flags: list[int] = field(default_factory=lambda: [0] * 6)
    keys: int = 0
    cutscenes: int = 0

    def encode(self, eligible: list[int]) -> bytes:
        bits: list[int] = []
        put_bits(bits, 16, 0)  # checksum, sealed below
        for ordinal in range(34):
            course = eligible[ordinal] if ordinal < len(eligible) else -1
            put_bits(bits, 2, self.status.get(course, 0))
        put_bits(bits, 6, self.taj)
        put_bits(bits, 10, self.trophies)
        put_bits(bits, 12, self.bosses)
        for value in self.balloons:
            put_bits(bits, 7, value)
        put_bits(bits, 3, self.tt_amulet)
        put_bits(bits, 3, self.wizpig_amulet)
        for value in self.world_flags:
            put_bits(bits, 16, value)
        put_bits(bits, 8, self.keys)
        put_bits(bits, 32, self.cutscenes)
        put_bits(bits, 16, 0x1234)  # named/started file
        put_bits(bits, 8, 0)
        if len(bits) != SLOT_BYTES * 8:
            raise ValueError(f"slot encoded {len(bits)} bits")
        slot = bytearray(
            sum(bits[i + j] << (7 - j) for j in range(8))
            for i in range(0, len(bits), 8)
        )
        return bytes(seal_slot(slot))

    def image(self, eligible: list[int]) -> bytes:
        out = bytearray(EEPROM_BYTES)
        out[:SLOT_BYTES] = self.encode(eligible)
        out[SLOT_BYTES:CONFIG_OFFSET] = b"\xFF" * (CONFIG_OFFSET - SLOT_BYTES)
        out[CONFIG_OFFSET:CONFIG_OFFSET + 8] = save_config_block(1)
        for offset in RECORD_OFFSETS:
            out[offset:offset + RECORD_BYTES] = sum_block(RECORD_BYTES)
        if not slot_checksum_valid(out[:SLOT_BYTES]):
            raise ValueError("fixture slot checksum is invalid")
        return bytes(out)


def decode(save: bytes) -> dict[str, object]:
    if len(save) != EEPROM_BYTES:
        raise ValueError(f"EEPROM is {len(save)} bytes, expected {EEPROM_BYTES}")
    slot = save[:SLOT_BYTES]
    return {
        "checksum_ok": slot_checksum_valid(slot),
        "raw": slot,
        "trophies": read_bits(slot, BIT_TROPHIES, 10),
        "bosses": read_bits(slot, BIT_BOSSES, 12),
        "balloons": tuple(
            read_bits(slot, BIT_BALLOONS + i * 7, 7) for i in range(6)
        ),
        "tt_amulet": read_bits(slot, BIT_TT_AMULET, 3),
        "wizpig_amulet": read_bits(slot, BIT_WIZPIG_AMULET, 3),
        "keys": read_bits(slot, BIT_KEYS, 8),
        "cutscenes": read_bits(slot, BIT_CUTSCENES, 32),
    }


def course_status(slot: bytes, eligible: list[int], course: int) -> int:
    return read_bits(slot, BIT_COURSES + eligible.index(course) * 2, 2)


def derive_seam_a(topo: WorldTopology) -> Slot:
    """Fixture A -- the state check_first_boss_progression's win arm persists.

    That check asserts, on the save it leaves behind: its three starting courses
    and the fourth all RACE_CLEARED, the world's first boss RACE_CLEARED, the
    world's first-boss bit in `bosses`, balloons (6, 4, 0, 0, 0, 0), the
    boss-door cutscene flag, and the two hub balloon bits it started with. Every
    field below is one of those and nothing else. In particular no course is
    silver-cleared and no amulet is held: this fixture is a player who has just
    beaten their first boss, which is exactly when silver coins unlock.
    """
    return Slot(
        status={course: STATUS_CLEARED for course in topo.races}
        | {topo.first_boss: STATUS_CLEARED},
        bosses=BOSS_FIRST_BIT(topo.world),
        balloons=[6 if i == 0 else (4 if i == topo.world else 0) for i in range(6)],
        world_flags=[HUB_BALLOON_FLAGS if i == 0 else 0 for i in range(6)],
        cutscenes=CUTSCENE_BOSS_DOOR(topo.world),
    )


def derive_seam_b(topo: WorldTopology, *, first_boss_beaten: bool = True) -> Slot:
    """Fixture B -- fixture A after seam A has been run on all four courses.

    Seam A proves that silver-clearing one course does exactly three things:
    moves its 2-bit status from 2 to 3, and increments both balloons[world] and
    balloons[0]. Applying that four times to fixture A gives status 3 on all four
    courses and balloons (6+4, 4+4) = (10, 8) -- and eight world balloons is the
    number obj_loop_exit tests to swap the lobby's first-boss warp for the
    rematch warp. The lobby's own door bits are set because a player who has
    driven into all four courses has opened all four doors.

    `first_boss_beaten=False` is the negative control and is NOT a legitimate
    campaign state: it exists only to show that the same won boss race awards the
    first-boss bit and no amulet when the rematch is not in fact a rematch.
    """
    return Slot(
        status={course: STATUS_SILVER for course in topo.races}
        | ({topo.first_boss: STATUS_CLEARED} if first_boss_beaten else {}),
        bosses=BOSS_FIRST_BIT(topo.world) if first_boss_beaten else 0,
        balloons=[10 if i == 0 else (8 if i == topo.world else 0) for i in range(6)],
        world_flags=[
            HUB_BALLOON_FLAGS if i == 0 else (0xFFFF if i == topo.world else 0)
            for i in range(6)
        ],
        cutscenes=CUTSCENE_BOSS_DOOR(topo.world),
    )


def derive_seam_e(topos: dict[int, WorldTopology]) -> Slot:
    """Fixture E -- fixture B applied to every world, then seam B run on each.

    Seam B proves that winning a world's rematch sets `1 << (world + 6)` and
    advances wizpigAmulet by exactly one. Four worlds therefore give the four
    rematch bits and wizpigAmulet == 4, which is the whole of Wizpig 1's gate
    (game/src/game.c:643). Wizpig 1's own cleared course and its `bosses` bit 0
    are the residual manual step recorded in tests/fixtures/README.md -- they are
    set here because Wizpig 2 is unreachable without them, and this arm is
    explicitly not claiming to have witnessed Wizpig 1.

    The remaining fields are Wizpig 2's own gate (game/src/game.c:755): all four
    trophy championships (`trophies == 0xFF`, the shape check_trophy_series
    proves production writes), the four T.T. amulet pieces and their world keys,
    Future Fun Land's four races cleared, and at least 47 total balloons.
    """
    status: dict[int, int] = {}
    bosses = BOSS_FIRST_BIT(WORLD_CENTRAL_AREA)
    balloons = [0] * 6
    for world, topo in sorted(topos.items()):
        if world == WORLD_FUTURE_FUN_LAND:
            for course in topo.races:
                status[course] = STATUS_CLEARED
            balloons[world] = 4
            continue
        for course in topo.races:
            status[course] = STATUS_SILVER
        status[topo.first_boss] = STATUS_CLEARED
        status[topo.rematch_boss] = STATUS_CLEARED
        bosses |= BOSS_FIRST_BIT(world) | BOSS_REMATCH_BIT(world)
        balloons[world] = 8
    status[37] = STATUS_CLEARED  # Wizpig 1, per the residual note above
    balloons[0] = 47
    return Slot(
        status=status,
        trophies=0xFF,
        bosses=bosses,
        balloons=balloons,
        tt_amulet=4,
        wizpig_amulet=4,
        world_flags=[HUB_BALLOON_FLAGS if i == 0 else 0xFFFF for i in range(6)],
        keys=0xF,
    )


# ------------------------------------------------------------------- running


@dataclass
class Run:
    label: str
    output: str
    save: bytes | None
    returncode: int


def invoke(
    binary: Path,
    rom: Path,
    label: str,
    *,
    fixture: bytes | None,
    script: Path,
    frames: int,
    values: dict[str, str],
    timeout: int,
) -> Run:
    """One headless process, in its own temporary directory and save dir.

    The save directory is always private to this run. Nothing here reads or
    writes a shared save/eeprom.bin, so the suite can run other ROM checks
    concurrently without either side seeing the other's EEPROM.
    """
    with tempfile.TemporaryDirectory(prefix=f"mdkr_campaign_{label}_") as temp:
        run_dir = Path(temp)
        save_dir = run_dir / "save"
        save_dir.mkdir()
        if fixture is not None:
            (save_dir / "eeprom.bin").write_bytes(fixture)
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("MDKR", "GE007_"))
        }
        env.update(
            LC_ALL="C",
            MDKR_AUDIO="0",
            MDKR_RENDERER="gl",
            MDKR_SAVE_DIR=str(save_dir),
            MDKR_SIMULATION_CADENCE="original",
            MDKR_SYNTH_FIELDS="2",
            MDKR_TRACE="1",
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        env.update(values)
        command = [
            str(binary), "--headless-frames", str(frames),
            "--input-script", str(script), "--rom", str(rom),
        ]
        process = subprocess.run(
            command, cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout, check=False,
        )
        output = process.stdout or ""
        saved = None
        eeprom = save_dir / "eeprom.bin"
        if eeprom.is_file():
            saved = eeprom.read_bytes()
        return Run(label, output, saved, process.returncode)


def base_failures(run: Run, failures: list[str]) -> None:
    if run.returncode != 0:
        failures.append(f"{run.label}: process exit {run.returncode}")
    marker = BAD_RE.search(run.output)
    if marker:
        failures.append(f"{run.label}: runtime failure marker {marker.group(0)!r}")


# ------------------------------------------------------------------- seam A


SILVER_FRAMES = 22000
RELOAD_FRAMES = 2400


def run_seam_a(
    binary: Path, rom: Path, topo: WorldTopology, eligible: list[int],
    timeout: int, failures: list[str], *, break_invariant: bool = False,
) -> dict[str, object]:
    fixture = derive_seam_a(topo).image(eligible)
    course = ANCIENT_LAKE if ANCIENT_LAKE in topo.races else topo.races[0]
    values = {
        "MDKR_AUTOPILOT": "1",
        "MDKR_AUTOPILOT_UNSTICK": "1",
        "MDKR_DRIVE_ROUTE": f"{HUB_TO_DINO};12:E{course}",
        # The coin detour costs race position, so the human finishes last on
        # merit. MDKR_ADVENTURE_WIN rotates that already-complete order after
        # the natural finish of all three laps; it is the same post-finish
        # verdict control check_first_boss_progression uses and it cannot
        # produce a finish, a lap or a coin.
        "MDKR_ADVENTURE_WIN": "1",
        "MDKR_SILVER_ROUTE": "1",
        "MDKR_WATCH_COURSEFLAGS": str(course),
    }
    if break_invariant:
        # --break-invariant: drive the identical race on the stock AI line. The
        # line passes 5-6 of the 8 coins, so `silverCoinCount >= 8` is never
        # reached and nothing is written. This check MUST fail here; if it
        # passes, its silver-coin assertions have stopped depending on the coins.
        del values["MDKR_SILVER_ROUTE"]
    run = invoke(
        binary, rom, "seamA", fixture=fixture, script=LOOP_SCRIPT,
        frames=SILVER_FRAMES, timeout=timeout, values=values,
    )
    base_failures(run, failures)

    gate = [
        m for m in COIN_RACE_RE.finditer(run.output)
        if int(m["course"]) == course and int(m["racetype"]) == RACETYPE_DEFAULT
    ]
    if not gate or int(gate[-1]["verdict"]) != 1:
        failures.append(
            f"seamA: course {course} never became a silver-coin race "
            f"(gate={gate[-1].group(0) if gate else 'absent'})"
        )
    elif int(gate[-1]["bosses"], 16) & BOSS_FIRST_BIT(topo.world) == 0:
        failures.append("seamA: silver-coin race ran without the first-boss bit")

    coins = [m for m in COIN_RE.finditer(run.output) if int(m["player"]) == 0]
    counts = [int(m["count"]) for m in coins]
    if counts != list(range(1, 9)):
        failures.append(
            f"seamA: coin pickups were {counts}, want 1..8 collected exactly once"
        )

    finish = [m for m in COIN_FINISH_RE.finditer(run.output) if int(m["course"]) == course]
    if len(finish) != 1:
        failures.append(f"seamA: {len(finish)} finish verdicts for course {course}, want 1")
    else:
        verdict = finish[0]
        if int(verdict["lead"]) != 0:
            failures.append("seamA: a computer racer led at the finish, so nothing was written")
        if int(verdict["coins"]) != 8 or int(verdict["silver"]) != 1 or int(verdict["tt"]) != 0:
            failures.append(f"seamA: wrong finish inputs {verdict.group(0)}")
        if int(verdict["flags"], 16) & RACE_CLEARED_SILVER_COINS:
            failures.append("seamA: the course was already silver-cleared before the race")

    writes = [
        m for m in WATCH_RE.finditer(run.output)
        if int(m["level"]) == course
        and not int(m["was"], 16) & RACE_CLEARED_SILVER_COINS
        and int(m["flags"], 16) & RACE_CLEARED_SILVER_COINS
    ]
    if len(writes) != 1:
        failures.append(
            f"seamA: {len(writes)} live writes of RACE_CLEARED_SILVER_COINS to "
            f"course {course}, want exactly 1"
        )

    if run.save is None:
        failures.append("seamA: no EEPROM was written")
        return {}
    state = decode(run.save)
    if not state["checksum_ok"]:
        failures.append("seamA: persisted slot checksum is invalid")
    persisted = course_status(run.save[:SLOT_BYTES], eligible, course)
    if persisted != STATUS_SILVER:
        failures.append(
            f"seamA: persisted status for course {course} is {persisted}, want {STATUS_SILVER}"
        )
    for other in topo.races:
        if other == course:
            continue
        if course_status(run.save[:SLOT_BYTES], eligible, other) != STATUS_CLEARED:
            failures.append(f"seamA: course {other} changed; only {course} was raced")
    # One silver clear is worth one world balloon and one total balloon.
    if state["balloons"][topo.world] != 5 or state["balloons"][0] != 7:
        failures.append(
            f"seamA: balloons {state['balloons']}, want world {topo.world}=5 and total=7"
        )
    if state["wizpig_amulet"] or state["tt_amulet"]:
        failures.append("seamA: a silver-coin clear incorrectly awarded an amulet piece")
    if state["bosses"] != BOSS_FIRST_BIT(topo.world):
        failures.append(f"seamA: bosses changed to 0x{state['bosses']:x}")

    # Round-trip: a second process must read the bit back out of that EEPROM.
    reload_run = invoke(
        binary, rom, "seamA-reload", fixture=run.save, script=RESUME_SCRIPT,
        frames=RELOAD_FRAMES, timeout=timeout,
        values={"MDKR_WATCH_COURSEFLAGS": str(course)},
    )
    base_failures(reload_run, failures)
    reloaded = [
        m for m in WATCH_RE.finditer(reload_run.output)
        if int(m["level"]) == course
        and int(m["flags"], 16) & RACE_CLEARED_SILVER_COINS
    ]
    if not reloaded:
        failures.append(
            "seamA: a second process did not reload RACE_CLEARED_SILVER_COINS "
            "from the persisted file"
        )
    return {"coins": counts, "balloons": state["balloons"]}


# ------------------------------------------------------------------- seam B


REMATCH_FRAMES = 9000


def run_seam_b(
    binary: Path, rom: Path, topo: WorldTopology, eligible: list[int],
    timeout: int, failures: list[str], *, first_boss_beaten: bool = True,
) -> dict[str, object]:
    label = f"seamB-w{topo.world}" + ("" if first_boss_beaten else "-control")
    fixture = derive_seam_b(topo, first_boss_beaten=first_boss_beaten).image(eligible)
    boss = topo.rematch_boss
    run = invoke(
        binary, rom, label, fixture=fixture, script=RESUME_SCRIPT,
        frames=REMATCH_FRAMES, timeout=timeout,
        values={
            "MDKR_AUTOPILOT": "1",
            "MDKR_AUTOPILOT_UNSTICK": "1",
            "MDKR_DRIVE_ROUTE": f"{HUB_TO_DINO};12:E{ANCIENT_LAKE}",
            "MDKR_LOAD_TRACK": str(boss),
            # Post-finish verdict control only: it writes finishPosition once
            # raceFinished has already latched on the human's own crossing.
            "MDKR_BOSS_WIN": "1",
            "MDKR_WATCH_COURSEFLAGS": str(boss),
        },
    )
    base_failures(run, failures)

    if not any(int(m["level"]) == boss for m in LEVEL_RE.finditer(run.output)):
        failures.append(f"{label}: boss course {boss} never loaded")

    forced = list(BOSS_FORCE_RE.finditer(run.output))
    if len(forced) != 1:
        failures.append(f"{label}: {len(forced)} verdict controls fired, want 1")
    elif int(forced[0]["finished"]) != 1:
        failures.append(f"{label}: the verdict control fired before a physical finish")

    verdicts = [
        m for m in BOSS_FINISH_RE.finditer(run.output) if int(m["course"]) == boss
    ]
    if not verdicts:
        failures.append(f"{label}: no production boss verdict for course {boss}")
    else:
        verdict = verdicts[0]
        if int(verdict["position"]) != 1 or int(verdict["player"]) != 0:
            failures.append(f"{label}: not a human first place: {verdict.group(0)}")
        if int(verdict["world"]) != topo.world:
            failures.append(
                f"{label}: boss raced under world {verdict['world']}, want {topo.world}"
            )

    if run.save is None:
        failures.append(f"{label}: no EEPROM was written")
        return {}
    state = decode(run.save)
    if not state["checksum_ok"]:
        failures.append(f"{label}: persisted slot checksum is invalid")

    rematch = BOSS_REMATCH_BIT(topo.world)
    first = BOSS_FIRST_BIT(topo.world)
    amulet_scenes = [
        m for m in LEVEL_RE.finditer(run.output)
        if int(m["level"]) == WIZPIG_AMULET_SEQUENCE
    ]
    if first_boss_beaten:
        if not state["bosses"] & rematch:
            failures.append(
                f"{label}: rematch bit 0x{rematch:x} absent from persisted "
                f"bosses=0x{state['bosses']:x}"
            )
        if state["wizpig_amulet"] != 1:
            failures.append(
                f"{label}: wizpigAmulet={state['wizpig_amulet']}, want exactly 1 piece"
            )
        if len(amulet_scenes) != 1:
            failures.append(
                f"{label}: {len(amulet_scenes)} Wizpig amulet cutscenes, want 1"
            )
        if course_status(run.save[:SLOT_BYTES], eligible, boss) != STATUS_CLEARED:
            failures.append(f"{label}: rematch course {boss} was not marked cleared")
    else:
        # The control: identical race, identical win, but the save says this
        # world's first boss is still standing. Production must read it as a
        # FIRST encounter -- first-boss bit, no rematch bit, no amulet.
        if state["bosses"] & rematch:
            failures.append(
                f"{label}: rematch bit was awarded without a first-boss win"
            )
        if not state["bosses"] & first:
            failures.append(f"{label}: first-boss bit was not awarded")
        if state["wizpig_amulet"] != 0:
            failures.append(
                f"{label}: amulet piece awarded for a first-boss win "
                f"(wizpigAmulet={state['wizpig_amulet']})"
            )
        if amulet_scenes:
            failures.append(f"{label}: Wizpig amulet cutscene played for a first-boss win")
    return {"bosses": state["bosses"], "amulet": state["wizpig_amulet"]}


# ------------------------------------------------------------------- seam E


WIZPIG_FRAMES = 14000
CHEAT_FRAMES = 4200


def run_seam_e(
    binary: Path, rom: Path, topos: dict[int, WorldTopology], eligible: list[int],
    timeout: int, failures: list[str],
) -> dict[str, object]:
    fixture = derive_seam_e(topos).image(eligible)
    before = decode(fixture)
    if before["bosses"] & BOSS_WIZPIG_TWO_BIT:
        failures.append("seamE: fixture already carried the Wizpig 2 bit")
    run = invoke(
        binary, rom, "seamE", fixture=fixture, script=RESUME_SCRIPT,
        frames=WIZPIG_FRAMES, timeout=timeout,
        values={
            "MDKR_AUTOPILOT": "1",
            "MDKR_AUTOPILOT_UNSTICK": "1",
            "MDKR_DRIVE_ROUTE": f"{HUB_TO_DINO};12:E{ANCIENT_LAKE}",
            "MDKR_LOAD_TRACK": str(WIZPIG_TWO),
            "MDKR_BOSS_WIN": "1",
            "MDKR_WATCH_COURSEFLAGS": str(WIZPIG_TWO),
        },
    )
    base_failures(run, failures)

    verdicts = [
        m for m in BOSS_FINISH_RE.finditer(run.output)
        if int(m["course"]) == WIZPIG_TWO
    ]
    if not verdicts:
        failures.append("seamE: no production verdict for Wizpig 2")
    else:
        verdict = verdicts[0]
        if int(verdict["position"]) != 1 or int(verdict["player"]) != 0:
            failures.append(f"seamE: not a human first place: {verdict.group(0)}")
        if int(verdict["world"]) != WORLD_FUTURE_FUN_LAND:
            failures.append(
                f"seamE: Wizpig 2 raced under world {verdict['world']}, "
                f"want {WORLD_FUTURE_FUN_LAND}"
            )
        if int(verdict["bosses"], 16) & BOSS_WIZPIG_TWO_BIT:
            failures.append("seamE: the Wizpig 2 bit was already set when the race ended")

    live = [
        m for m in WATCH_RE.finditer(run.output)
        if int(m["bosses"], 16) & BOSS_WIZPIG_TWO_BIT
    ]
    if not live:
        failures.append("seamE: settings->bosses never gained the Wizpig 2 bit")

    if run.save is None:
        failures.append("seamE: no EEPROM was written")
        return {}
    state = decode(run.save)
    if not state["checksum_ok"]:
        failures.append("seamE: persisted slot checksum is invalid")
    if not state["bosses"] & BOSS_WIZPIG_TWO_BIT:
        failures.append(
            f"seamE: persisted bosses=0x{state['bosses']:x} lacks the Wizpig 2 bit "
            f"0x{BOSS_WIZPIG_TWO_BIT:x}, so the true-ending credits would not be chosen"
        )
    if course_status(run.save[:SLOT_BYTES], eligible, WIZPIG_TWO) != STATUS_CLEARED:
        failures.append("seamE: Wizpig 2 was not marked cleared")
    for keep, name in ((4, "wizpig_amulet"), (4, "tt_amulet")):
        if state[name] != keep:
            failures.append(f"seamE: {name} became {state[name]}, want {keep}")
    if state["trophies"] != 0xFF:
        failures.append(f"seamE: trophies became 0x{state['trophies']:x}, want 0xff")

    # The cheat route is the other credits entry, and the reason reaching credits
    # is not by itself evidence of anything. Recorded here as the contrast: the
    # WHODIDTHIS route arrives at MENU_CREDITS from a save file that was never
    # started, so it carries no campaign state at all -- while the arm above had
    # to win Wizpig 2 to produce the one bit menu_credits_init reads to choose
    # the true ending over "THE END".
    cheat = invoke(
        binary, rom, "credits-cheat", fixture=None, script=CHEAT_SCRIPT,
        frames=CHEAT_FRAMES, timeout=timeout, values={},
    )
    base_failures(cheat, failures)
    if not any(int(m["menu"]) == MENU_CREDITS for m in MENU_RE.finditer(cheat.output)):
        failures.append("credits-cheat: the known-good cheat route did not reach MENU_CREDITS")
    if cheat.save is None:
        failures.append("credits-cheat: no EEPROM was written")
    elif decode(cheat.save)["checksum_ok"]:
        failures.append(
            "credits-cheat: the cheat route started a campaign file, so it is no "
            "longer the state-free contrast this arm relies on"
        )
    return {"bosses": state["bosses"]}


# ---------------------------------------------------------------------- main


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument(
        "--quick", action="store_true",
        help="seam A plus one seam B world and its control; skips the rest",
    )
    parser.add_argument(
        "--break-invariant", action="store_true",
        help="drive seam A on the stock AI line, which cannot collect eight "
             "coins; the check must FAIL",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, RESUME_SCRIPT, LOOP_SCRIPT, CHEAT_SCRIPT):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    try:
        eligible = save_order(str(rom))
        topos = world_topology(str(rom), eligible)
        if len(topos) != 5:
            raise ValueError(f"ROM yielded {len(topos)} four-race worlds, want 5")
        dino = next(t for t in topos.values() if ANCIENT_LAKE in t.races)
    except (OSError, ValueError, IndexError, struct.error) as exc:
        print(f"FAIL: could not read campaign topology: {exc}", file=sys.stderr)
        return 1

    print("Campaign progression seams")
    print(f"  topology: " + ", ".join(
        f"w{t.world}={t.races}/{t.first_boss},{t.rematch_boss}"
        for t in sorted(topos.values(), key=lambda t: t.world)
    ))

    try:
        seam_a = run_seam_a(
            binary, rom, dino, eligible, args.timeout, failures,
            break_invariant=args.break_invariant,
        )
        print(f"  seam A  silver coins on course {ANCIENT_LAKE}: "
              f"coins={len(seam_a.get('coins', []))} balloons={seam_a.get('balloons')}")

        rematch_worlds = [dino.world] if args.quick else sorted(
            w for w, t in topos.items() if w != WORLD_FUTURE_FUN_LAND
        )
        for world in rematch_worlds:
            result = run_seam_b(binary, rom, topos[world], eligible, args.timeout, failures)
            print(f"  seam B  world {world} rematch {topos[world].rematch_boss}: "
                  f"bosses=0x{result.get('bosses', 0):x} "
                  f"amulet={result.get('amulet')}")
        control = run_seam_b(
            binary, rom, topos[dino.world], eligible, args.timeout, failures,
            first_boss_beaten=False,
        )
        print(f"  seam B  first-encounter control: "
              f"bosses=0x{control.get('bosses', 0):x} amulet={control.get('amulet')}")

        if not args.quick:
            seam_e = run_seam_e(binary, rom, topos, eligible, args.timeout, failures)
            print(f"  seam E  Wizpig 2: bosses=0x{seam_e.get('bosses', 0):x}")
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    if failures:
        print(f"check_campaign_progression: FAIL ({len(failures)} issue(s))")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_campaign_progression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
