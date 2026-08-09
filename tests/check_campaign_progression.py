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
B  Rematch awards the amulet.  All four worlds' second boss races, won, each one
                               fought on the EEPROM the previous one persisted --
                               so wizpigAmulet climbing 1, 2, 3, 4 is something
                               production wrote four times rather than something
                               a fixture asserted. The negative control is the
                               same race from a save where the FIRST boss was
                               never beaten: production must award the first-boss
                               bit and NO amulet, which is what proves the amulet
                               is gated on the rematch actually being a rematch.
                               A further arm enters the Dino Domain rematch by
                               DRIVING the human through the lobby's own boss
                               door rather than by MDKR_LOAD_TRACK retarget, and
                               asserts the identical post-seam state, so the warp
                               swap obj_loop_exit performs at eight world
                               balloons is witnessed rather than bypassed. The
                               approach is watched by tests/route_plan.py, which
                               reports the waypoint, the closest approach and the
                               reason instead of oscillating to the budget.
C  Four pieces open Wizpig 1.  The carried four-piece save loaded into Timber's
                               Island must redirect to the Wizpig mouth sequence
                               and latch CUTSCENE_WIZPIG_FACE; the same chain one
                               rematch earlier, at three pieces, must not. Then
                               Wizpig 1 itself is raced and won, setting its
                               central-area boss bit.
T  Four championships give     check_trophy_series.py's own championship driver,
   trophies == 0xFF.           imported and chained: four golds, each won on the
                               EEPROM the previous one persisted, so `trophies`
                               climbing 0x3 -> 0xf -> 0x3f -> 0xff is production's
                               arithmetic. It used to be a fixture-E premise.
T.T. Four challenges give      The four challenge courses, won in four separate
   ttAmulet == 4.              processes on one another's saves, shaped exactly
                               like seam B. The negative control replays the
                               FIRST challenge on the save it produced: the piece
                               is gated on the course not already being cleared,
                               so a replay must award nothing and leave ttAmulet
                               at one.
E  Wizpig 2 selects the true   The final boss, won. `menu_credits_init` chooses
   ending.                     between "THE END" (cheat), "THE END?" (legitimate,
                               Wizpig 2 not beaten) and "TO BE CONTINUED ..." +
                               SEQUENCE_CRESCENT_ISLAND (legitimate, Wizpig 2
                               beaten) purely on `gViewingCreditsFromCheat` and
                               `settings->bosses & 0x20`. This arm proves the
                               campaign sets and persists that bit, and then
                               REACHES the screen: the run drives the post-win
                               cutscene stack all the way to MENU_CREDITS and
                               asserts the branch menu_credits_init took. The
                               cheat arm reaches the same screen from a save that
                               was never started and must produce "THE END", so
                               the two endings are distinguished by state this
                               check owns end to end.

What this deliberately does NOT prove
-------------------------------------
See tests/fixtures/README.md, "Residual manual acceptance". The three named
residuals -- the lobby rematch door driven, the trophy/T.T.-amulet chain, and the
credits screen reached from a won Wizpig 2 -- are now seams above rather than
premises. What is still constructed is Future Fun Land's own state: its four
cleared races, its four world balloons and the 47 total the T.T. door counts, the
four world keys, and the world-arrival cutscene flag. Those are the world Wizpig 2
lives in, and this check does not drive to it; the lighthouse unlock that opens
it is gated separately by tests/check_future_fun_land.py. None of the residuals
is weakened into a vacuous assertion in this file.

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
from route_plan import RouteFailure, RouteFollower, Waypoint
from check_trophy_series import (drive_gold_championship,
                                 AWARD_RE as TROPHY_AWARD_RE,
                                 TROPHY_BIT_OFFSET)

import struct


ROOT = Path(__file__).resolve().parent.parent
RESUME_SCRIPT = ROOT / "tests/input_scripts/adventure_resume_race.txt"
LOOP_SCRIPT = ROOT / "tests/input_scripts/adventure_race_loop.txt"
CHEAT_SCRIPT = ROOT / "tests/input_scripts/credits_via_cheat.txt"
DRIVEN_SCRIPT = ROOT / "tests/input_scripts/lobby_rematch_door.txt"

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
TT_AMULET_SEQUENCE = 44
MENU_CREDITS = 25

# The Dino Domain lobby, and its boss-rematch door. Both the exit position and
# its radius are read out of the level's own object map (MDKR_OBJDUMP=1), not
# guessed: `EXIT i=35 pos=(-777.0, -4.0, 1812.0) dest=46 radius=248 boss=1`,
# where boss=1 is WARP_BOSS_REMATCH. Its first-encounter twin, dest=38, sits 35
# units away and is the one obj_loop_exit disables at eight world balloons --
# which is why arriving at this door at all is the observable form of the gate.
DINO_LOBBY = 12
BOSS_REMATCH_EXIT = (-777.0, 1812.0)
BOSS_EXIT_RADIUS = 248.0
# The one intermediate waypoint the driven approach needs, and the whole of the
# empirical work behind seam B's driven arm. See run_seam_b's `driven` branch.
LOBBY_APPROACH = (-300.0, 700.0)
# MDKR_DRIVE_WPR's default, i.e. the radius the drive hook itself retires a
# waypoint at; the follower has to agree with the driver or it would report a
# waypoint unreached that the driver had already moved past.
DRIVE_WAYPOINT_RADIUS = 220.0
# Measured: lobby entered at frame ~2947, approach waypoint reached at ~3021,
# the exit taken at ~3124. 1,500 frames per leg is twenty times the measured
# cost and still an order of magnitude below the frame budget, so a route that
# is merely unlucky is not cut off and one that is stuck is reported in seconds.
LOBBY_LEG_BUDGET = 1500
# The driven arm reaches the rematch through the lobby instead of by retarget,
# which costs the drive from the frontend to the door. Measured end to end:
# amulet sequence at ~7061, back in the lobby at ~7441.
DRIVEN_REMATCH_FRAMES = 12000

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
# The per-frame position stream every ROM check already gets from MDKR_TRACE=1.
# Used as the position source for the driven arm's route follower: the drive
# hook's own MDKR_DRIVE_VERBOSE line is 30-frame sampled and carries no frame
# number, so a stall would be reported at the wrong time and with the wrong
# budget.
PACE_RE = re.compile(
    r"\[PACE\] frame=(?P<frame>\d+).*racer x=(?P<x>\S+) y=\S+ z=(?P<z>\S+) "
    r"clock="
)
BOSS_WARP_RE = re.compile(
    r"bosswarp: level=(?P<level>\d+) dest=(?P<dest>\d+) warp=(?P<warp>\w+) "
    r"worldBalloons=(?P<balloons>\d+) enabled=(?P<enabled>\d+)"
)
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

    @classmethod
    def from_save(cls, save: bytes, eligible: list[int]) -> "Slot":
        """Read a real persisted slot back into the same shape.

        This is what lets a fixture be *carried* rather than *constructed*: a
        seam's own output becomes the next seam's input with every field
        production wrote left exactly as production wrote it, and only the
        fields a later gate needs -- each one named at the call site -- overridden.
        """
        slot = save[:SLOT_BYTES]
        return cls(
            status={
                course: read_bits(slot, BIT_COURSES + i * 2, 2)
                for i, course in enumerate(eligible)
            },
            taj=read_bits(slot, BIT_TAJ, 6),
            trophies=read_bits(slot, BIT_TROPHIES, 10),
            bosses=read_bits(slot, BIT_BOSSES, 12),
            balloons=[read_bits(slot, BIT_BALLOONS + i * 7, 7) for i in range(6)],
            tt_amulet=read_bits(slot, BIT_TT_AMULET, 3),
            wizpig_amulet=read_bits(slot, BIT_WIZPIG_AMULET, 3),
            world_flags=[
                read_bits(slot, BIT_WORLD_FLAGS + i * 16, 16) for i in range(6)
            ],
            keys=read_bits(slot, BIT_KEYS, 8),
            cutscenes=read_bits(slot, BIT_CUTSCENES, 32),
        )

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


def derive_all_worlds_silver(topos: dict[int, WorldTopology]) -> Slot:
    """The seam B chain's starting point -- fixture A x4, seam A x16.

    Four copies of fixture A, one per world, each with seam A applied to all four
    of its courses. Per world that is status 3 on the four races and eight world
    balloons; across four worlds the global counter is 4 x 8 plus the two hub
    balloons fixture A already carried.
    """
    status: dict[int, int] = {}
    bosses = 0
    cutscenes = 0
    balloons = [0] * 6
    for world, topo in sorted(topos.items()):
        if world == WORLD_FUTURE_FUN_LAND:
            continue
        for course in topo.races:
            status[course] = STATUS_SILVER
        status[topo.first_boss] = STATUS_CLEARED
        bosses |= BOSS_FIRST_BIT(world)
        balloons[world] = 8
        cutscenes |= CUTSCENE_BOSS_DOOR(world)
    balloons[0] = 4 * 8 + 2
    return Slot(
        status=status,
        bosses=bosses,
        balloons=balloons,
        cutscenes=cutscenes,
        world_flags=[
            HUB_BALLOON_FLAGS if i == 0 else 0xFFFF for i in range(6)
        ],
    )


# CUTSCENE_DINO_DOMAIN_BOSS << (world - 1) -- the "you have arrived in this
# world" scene game_load_level latches on a RACETYPE_HUBWORLD load
# (game/src/game.c:762-802). For Future Fun Land that is 8 << 4 = 0x80, and see
# derive_seam_e for why fixture E has to carry it.
CUTSCENE_WORLD_ARRIVAL = lambda world: 8 << (world - 1)  # noqa: E731


def derive_seam_e(base: bytes, topos: dict[int, WorldTopology],
                  eligible: list[int]) -> Slot:
    """Fixture E -- the chain's own persisted save, plus Future Fun Land.

    Everything Wizpig 2 checks about the *campaign* is now carried out of the
    seam A -> B -> C -> T -> T.T. chain untouched: the four rematch bits and
    wizpigAmulet == 4 from seam B's wins, Wizpig 1's cleared course and `bosses`
    bit 0 from seam C's, `trophies == 0xFF` from seam T's four championships and
    `ttAmulet == 4` from seam T.T.'s four challenge wins. Those last two used to
    be premises; they are asserted-as-carried below rather than written.

    What is still constructed is Future Fun Land itself -- the world Wizpig 2
    lives in, which this check does not drive to:

    - the four FFL races cleared, its four world balloons, and 47 total, which
      is the number the T.T. door counts (game/src/object_functions.c:4159);
    - the four world keys;
    - CUTSCENE_WORLD_ARRIVAL(5). This one is not cosmetic and was found by
      measurement. Every FFL level header is RACETYPE_HUBWORLD, so
      game_load_level's world-arrival branch runs on the post-race cutscene
      levels too, and with the flag clear it OVERWRITES the cutscene channel the
      level-properties stack pushed with CUTSCENE_ID_UNK_5. func_8001E4C4() then
      deactivates every animation object whose channel is not 5 -- measured on
      ASSET_LEVEL_WIZPIG2ANIM, all 159 of them -- so the scene has no animation
      left to run, never signals its end, and the cutscene stack never drains.
      A player cannot reach Wizpig 2 without having arrived in Future Fun Land
      once, so carrying the flag is what makes the fixture legitimate, not a
      workaround: a fixture without it is a state the campaign cannot produce.
    """
    slot = Slot.from_save(base, eligible)
    slot.keys = 0xF
    for course in topos[WORLD_FUTURE_FUN_LAND].races:
        slot.status[course] = STATUS_CLEARED
    slot.balloons[WORLD_FUTURE_FUN_LAND] = 4
    slot.balloons[0] = max(slot.balloons[0], 47)
    slot.cutscenes |= CUTSCENE_WORLD_ARRIVAL(WORLD_FUTURE_FUN_LAND)
    return slot


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


def follow_lobby_approach(run: Run, label: str, failures: list[str]) -> None:
    """Watch the driven lobby approach against tests/route_plan.py.

    The point of doing this rather than only asserting on the outcome is the
    failure mode the residual recorded: the drive hook has no give-up condition,
    so a blocked heading is re-attempted until the frame budget runs out and the
    run reports nothing but "the boss never loaded". RouteFollower turns that
    into the waypoint, the closest approach and the reason -- oscillation, held
    off, or budget -- at the frame it happened.

    The trace is bracketed to the lobby visit, so a position sampled in the hub
    or in the boss level can never be credited as an arrival, and frames are
    rebased to that arrival -- the per-waypoint budget is time spent IN THE
    LOBBY, not time since the process started, which would otherwise be spent
    on the frontend before the first sample is even taken.
    """
    loads = [(int(m["level"]), int(m["frame"])) for m in LEVEL_RE.finditer(run.output)]
    entered = next((f for level, f in loads if level == DINO_LOBBY), None)
    if entered is None:
        failures.append(f"{label}: never reached the Dino Domain lobby")
        return
    left = next((f for level, f in loads if f > entered and level != DINO_LOBBY), None)
    route = RouteFollower([
        Waypoint(*LOBBY_APPROACH, radius=DRIVE_WAYPOINT_RADIUS,
                 budget_frames=LOBBY_LEG_BUDGET, name="lobby approach"),
        Waypoint(*BOSS_REMATCH_EXIT, radius=BOSS_EXIT_RADIUS,
                 budget_frames=LOBBY_LEG_BUDGET, name="boss rematch exit"),
    ])
    try:
        for match in PACE_RE.finditer(run.output):
            frame = int(match["frame"])
            if frame < entered:
                continue
            if left is not None and frame > left:
                break
            if route.observe(frame - entered, float(match["x"]),
                             float(match["z"])):
                break
        route.require_complete()
    except RouteFailure as exc:
        failures.append(
            f"{label}: lobby approach failed (frames counted from the lobby "
            f"load at {entered}) -- {exc}"
        )
    except ValueError as exc:
        failures.append(f"{label}: unreadable position trace -- {exc}")


def run_seam_b(
    binary: Path, rom: Path, topo: WorldTopology, eligible: list[int],
    timeout: int, failures: list[str], *, first_boss_beaten: bool = True,
    fixture: bytes | None = None, expect_amulet: int = 1,
    driven: bool = False,
) -> dict[str, object]:
    label = (f"seamB-w{topo.world}" + ("-driven" if driven else "")
             + ("" if first_boss_beaten else "-control"))
    if fixture is None:
        fixture = derive_seam_b(topo, first_boss_beaten=first_boss_beaten).image(eligible)
    boss = topo.rematch_boss
    if driven:
        # The completeness arm. No MDKR_LOAD_TRACK: the rematch is entered by
        # driving the human through the lobby's own boss door, so the thing
        # being witnessed is obj_loop_exit having swapped WARP_BOSS_FIRST for
        # WARP_BOSS_REMATCH at eight world balloons -- the retarget arm below
        # reaches the same race without ever consulting that decision.
        #
        # `LOBBY_APPROACH` is the whole of the route work and was found by
        # bisection, recorded in tests/input_scripts/lobby_rematch_door.txt.
        values = {
            "MDKR_AUTOPILOT": "1",
            "MDKR_AUTOPILOT_UNSTICK": "1",
            "MDKR_DRIVE_ROUTE": (
                f"{HUB_TO_DINO}"
                f";{DINO_LOBBY}:{LOBBY_APPROACH[0]:.0f},{LOBBY_APPROACH[1]:.0f}"
                f":E{boss}"
            ),
            "MDKR_BOSS_WIN": "1",
            "MDKR_WATCH_COURSEFLAGS": str(boss),
        }
        script, frames = DRIVEN_SCRIPT, DRIVEN_REMATCH_FRAMES
    else:
        values = {
            "MDKR_AUTOPILOT": "1",
            "MDKR_AUTOPILOT_UNSTICK": "1",
            "MDKR_DRIVE_ROUTE": f"{HUB_TO_DINO};12:E{ANCIENT_LAKE}",
            "MDKR_LOAD_TRACK": str(boss),
            # Post-finish verdict control only: it writes finishPosition once
            # raceFinished has already latched on the human's own crossing.
            "MDKR_BOSS_WIN": "1",
            "MDKR_WATCH_COURSEFLAGS": str(boss),
        }
        script, frames = RESUME_SCRIPT, REMATCH_FRAMES
    run = invoke(
        binary, rom, label, fixture=fixture, script=script,
        frames=frames, timeout=timeout, values=values,
    )
    base_failures(run, failures)

    if driven:
        follow_lobby_approach(run, label, failures)
        warps = {
            m["warp"]: m for m in BOSS_WARP_RE.finditer(run.output)
            if int(m["level"]) == DINO_LOBBY
        }
        if "rematch" not in warps or int(warps["rematch"]["enabled"]) != 1:
            failures.append(
                f"{label}: the lobby's WARP_BOSS_REMATCH was not the live boss "
                f"warp ({warps.get('rematch', 'no verdict').group(0) if 'rematch' in warps else 'no verdict'})"
            )
        elif int(warps["rematch"]["balloons"]) != 8:
            failures.append(
                f"{label}: rematch warp enabled at "
                f"{warps['rematch']['balloons']} world balloons, want 8"
            )
        if "first" not in warps or int(warps["first"]["enabled"]) != 0:
            failures.append(
                f"{label}: WARP_BOSS_FIRST was still live, so the door the "
                f"route drove through was not necessarily the rematch door"
            )
        taken = [
            m for m in re.finditer(
                r"drive: level=(?P<level>\d+) step \d+: exit to (?P<dest>\d+) "
                r"TAKEN \(now in level (?P<now>\d+)\)", run.output)
            if int(m["level"]) == DINO_LOBBY and int(m["dest"]) == boss
        ]
        if not taken:
            failures.append(
                f"{label}: the lobby's exit to {boss} was never taken, so the "
                f"rematch was not entered through the door"
            )

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
        if state["wizpig_amulet"] != expect_amulet:
            failures.append(
                f"{label}: wizpigAmulet={state['wizpig_amulet']}, want exactly "
                f"{expect_amulet} piece(s) after this many rematch wins"
            )
        if len(amulet_scenes) != 1:
            failures.append(
                f"{label}: {len(amulet_scenes)} Wizpig amulet cutscenes, want 1"
            )
        if course_status(run.save[:SLOT_BYTES], eligible, boss) != STATUS_CLEARED:
            failures.append(f"{label}: rematch course {boss} was not marked cleared")
        # Every bit the incoming save already held must survive this win, or the
        # chain is not carrying forward what it claims to.
        carried = decode(fixture)["bosses"]
        if state["bosses"] & carried != carried:
            failures.append(
                f"{label}: persisted bosses=0x{state['bosses']:x} dropped bits from "
                f"the carried save 0x{carried:x}"
            )
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
    return {
        "bosses": state["bosses"],
        "amulet": state["wizpig_amulet"],
        "save": run.save,
    }


# ------------------------------------------------------------------- seam C


WIZPIG_ONE = 37
WIZPIG_MOUTH_SEQUENCE = 42
CUTSCENE_WIZPIG_FACE = 0x2000
FACE_FRAMES = 6000
WIZPIG_ONE_FRAMES = 20000

WIZPIG_FACE_RE = re.compile(
    r"wizpigface: hub=(?P<hub>\d+) -> cutsceneLevel=(?P<target>\d+) "
    r"wizpigAmulet=(?P<amulet>\d+) cutsceneFlags=0x(?P<flags>[0-9a-f]+)"
)


def run_seam_c(
    binary: Path, rom: Path, chained: bytes, three_pieces: bytes,
    eligible: list[int], timeout: int, failures: list[str],
) -> bytes:
    """Wizpig 1: the four-piece unlock, then the race itself.

    The unlock is NOT visible in the level-load stream. `game_load_level` traces
    the level it was asked for and only afterwards rewrites it to the Wizpig
    mouth sequence, so a redirected hub load and an ordinary one print the same
    line; the redirect then pushes the hub, plays the scene, and pops it back, so
    the log shows two plain hub loads either way. The `wizpigface:` trace exists
    for that reason, and `CUTSCENE_WIZPIG_FACE` -- which has exactly one writer,
    inside the redirect branch -- corroborates it in the save.
    """
    face = invoke(
        binary, rom, "seamC-face", fixture=chained, script=RESUME_SCRIPT,
        frames=FACE_FRAMES, timeout=timeout,
        values={"MDKR_AUTOPILOT": "1", "MDKR_DRIVE_ROUTE": HUB_TO_DINO},
    )
    base_failures(face, failures)
    if decode(chained)["cutscenes"] & CUTSCENE_WIZPIG_FACE:
        failures.append("seamC: the carried save had already seen the Wizpig face")
    redirects = list(WIZPIG_FACE_RE.finditer(face.output))
    if len(redirects) != 1:
        failures.append(
            f"seamC: {len(redirects)} Wizpig-face redirects on the hub load, want 1"
        )
    else:
        redirect = redirects[0]
        if int(redirect["target"]) != WIZPIG_MOUTH_SEQUENCE:
            failures.append(
                f"seamC: hub redirected to level {redirect['target']}, "
                f"want {WIZPIG_MOUTH_SEQUENCE}"
            )
        if int(redirect["amulet"]) != 4:
            failures.append(f"seamC: redirect fired at {redirect['amulet']} amulet pieces")
    if face.save is None or not decode(face.save)["cutscenes"] & CUTSCENE_WIZPIG_FACE:
        failures.append("seamC: CUTSCENE_WIZPIG_FACE was not persisted")

    # Negative control: the identical hub load from the same chain one rematch
    # earlier. Three pieces is not four, so production must not redirect.
    control = invoke(
        binary, rom, "seamC-control", fixture=three_pieces, script=RESUME_SCRIPT,
        frames=FACE_FRAMES, timeout=timeout,
        values={"MDKR_AUTOPILOT": "1", "MDKR_DRIVE_ROUTE": HUB_TO_DINO},
    )
    base_failures(control, failures)
    if decode(three_pieces)["wizpig_amulet"] != 3:
        failures.append("seamC control: the carried save did not hold three pieces")
    if WIZPIG_FACE_RE.search(control.output):
        failures.append("seamC control: Wizpig 1 unlocked with only three amulet pieces")
    if control.save is not None and decode(control.save)["cutscenes"] & CUTSCENE_WIZPIG_FACE:
        failures.append("seamC control: CUTSCENE_WIZPIG_FACE set with three pieces")

    # The race. Entered by retarget, as every boss in this file is; what is being
    # asserted is the verdict and the central-area boss bit it writes.
    race = invoke(
        binary, rom, "seamC-race", fixture=face.save or chained, script=RESUME_SCRIPT,
        frames=WIZPIG_ONE_FRAMES, timeout=timeout,
        values={
            "MDKR_AUTOPILOT": "1",
            "MDKR_AUTOPILOT_UNSTICK": "1",
            "MDKR_DRIVE_ROUTE": f"{HUB_TO_DINO};12:E{ANCIENT_LAKE}",
            "MDKR_LOAD_TRACK": str(WIZPIG_ONE),
            "MDKR_BOSS_WIN": "1",
            "MDKR_WATCH_COURSEFLAGS": str(WIZPIG_ONE),
        },
    )
    base_failures(race, failures)
    verdicts = [
        m for m in BOSS_FINISH_RE.finditer(race.output) if int(m["course"]) == WIZPIG_ONE
    ]
    if not verdicts:
        failures.append("seamC: no production verdict for Wizpig 1")
    else:
        verdict = verdicts[0]
        if int(verdict["position"]) != 1 or int(verdict["player"]) != 0:
            failures.append(f"seamC: not a human first place: {verdict.group(0)}")
        if int(verdict["world"]) != WORLD_CENTRAL_AREA:
            failures.append(
                f"seamC: Wizpig 1 raced under world {verdict['world']}, "
                f"want {WORLD_CENTRAL_AREA}"
            )
        if int(verdict["bosses"], 16) & BOSS_FIRST_BIT(WORLD_CENTRAL_AREA):
            failures.append("seamC: the Wizpig 1 bit was already set when the race ended")
    if race.save is None:
        failures.append("seamC: no EEPROM was written")
        return chained
    state = decode(race.save)
    if not state["checksum_ok"]:
        failures.append("seamC: persisted slot checksum is invalid")
    if not state["bosses"] & BOSS_FIRST_BIT(WORLD_CENTRAL_AREA):
        failures.append(
            f"seamC: persisted bosses=0x{state['bosses']:x} lacks Wizpig 1's bit"
        )
    if course_status(race.save[:SLOT_BYTES], eligible, WIZPIG_ONE) != STATUS_CLEARED:
        failures.append("seamC: Wizpig 1 was not marked cleared")
    if state["wizpig_amulet"] != 4:
        failures.append(
            f"seamC: wizpigAmulet became {state['wizpig_amulet']}; Wizpig 1 is not "
            "an amulet boss"
        )
    return race.save


# ------------------------------------------------------- seam T -- trophies


# One gold per world, chained. `trophies` is two bits per world and gold is 3,
# so the running totals are the only values four production awards can leave.
TROPHY_WORLDS = (1, 2, 3, 4)
TROPHY_TOTALS = (0x3, 0xF, 0x3F, 0xFF)
# Racer 0 first in all four rounds: 9 points a round (the same scale that gives
# check_trophy_series.py's 32 for a 1st/2nd/1st/2nd tie), so 36 and rank 0.
TROPHY_GOLD_POINTS = 36


def carry_trophy_save(base: bytes, produced: bytes,
                      eligible: list[int]) -> bytes:
    """Carry a championship's EEPROM into the next one, pinning the hub back.

    A championship drives Timber's Island to the Dino Domain cabinet, and the
    hub writes two things on the way that have nothing to do with trophies:
    `taj` gains TAJ_CAR_OFFERED, and the hub's own course flags gain the
    balloons and doors the route passed (measured, 0x4400 -> 0x67b3). Left
    carried, the NEXT championship's hub drive stalls before it reaches the
    lobby and awards nothing at all -- measured, worlds 2, 3 and 4 all
    persisted `trophies=0x3`, i.e. only world 1's.

    So exactly those two hub-side fields are pinned to the value the chain
    entered seam T with, and nothing else is touched. `trophies` -- the only
    field this seam is about -- is carried untouched, and each championship's
    own `trophyaward:` line is checked to have STARTED from the carried value,
    so the chain is verified rather than assumed.
    """
    slot = Slot.from_save(produced, eligible)
    origin = Slot.from_save(base, eligible)
    slot.taj = origin.taj
    slot.world_flags[0] = origin.world_flags[0]
    return slot.image(eligible)


def run_seam_t(
    binary: Path, rom: Path, base: bytes, eligible: list[int],
    failures: list[str],
) -> bytes:
    """Seam T -- the four trophy championships, chained on one save.

    check_trophy_series.py already gates one championship's production
    machinery (cabinet entry, round schedule, scoring, ties, the podium, the
    QUIT branch and the EEPROM round trip) and its own gate is untouched by
    this: what is imported is `drive_gold_championship`, which is that file's
    own `run_case` with the winning finish order and a caller-supplied save.

    Chaining the four is what this adds. `trophies == 0xFF` is half of Wizpig
    2's other gate and the whole of Future Fun Land's, and it used to be a
    fixture-E premise; here it is the value production wrote four times, each
    time onto the EEPROM the previous championship persisted.
    """
    carried = base
    for index, world in enumerate(TROPHY_WORLDS):
        label = f"seamT-w{world}"
        incoming = decode(carried)["trophies"]
        try:
            proc, save = drive_gold_championship(binary, rom, world, carried)
        except subprocess.TimeoutExpired:
            failures.append(f"{label}: championship timed out")
            return carried
        if proc.returncode != 0:
            failures.append(f"{label}: process exit {proc.returncode}")
        marker = BAD_RE.search(proc.stdout)
        if marker:
            failures.append(f"{label}: runtime failure marker {marker.group(0)!r}")
        award = TROPHY_AWARD_RE.search(proc.stdout)
        if not award:
            failures.append(f"{label}: no production trophy award")
        else:
            got_world, rank, points, old, new, cinematic = award.groups()
            if (int(got_world), int(rank), int(points)) != (
                world, 0, TROPHY_GOLD_POINTS
            ):
                failures.append(
                    f"{label}: award tuple {award.groups()}, want world "
                    f"{world} rank 0 points {TROPHY_GOLD_POINTS}"
                )
            if int(old, 16) != incoming:
                failures.append(
                    f"{label}: the championship started from trophies "
                    f"0x{int(old, 16):x}, but the carried save holds "
                    f"0x{incoming:x} -- the chain is not being carried"
                )
            if int(new, 16) != TROPHY_TOTALS[index]:
                failures.append(
                    f"{label}: awarded trophies 0x{int(new, 16):x}, want "
                    f"0x{TROPHY_TOTALS[index]:x}"
                )
            if int(cinematic) != 1:
                failures.append(f"{label}: no podium cinematic for a gold")
        state = decode(save)
        if not state["checksum_ok"]:
            failures.append(f"{label}: persisted slot checksum is invalid")
        if state["trophies"] != TROPHY_TOTALS[index]:
            failures.append(
                f"{label}: persisted trophies=0x{state['trophies']:x}, want "
                f"0x{TROPHY_TOTALS[index]:x}"
            )
        # A trophy race must not be able to move the campaign on: everything
        # seams A-C wrote has to come out the other side unchanged.
        for name in ("bosses", "wizpig_amulet", "tt_amulet", "cutscenes"):
            if state[name] != decode(carried)[name]:
                failures.append(
                    f"{label}: {name} changed across a trophy championship "
                    f"({decode(carried)[name]!r} -> {state[name]!r})"
                )
        carried = carry_trophy_save(base, save, eligible)
    return carried


# ------------------------------------------------- seam T.T. -- the amulet


# The four challenge courses, in the order check_challenge_modes.py names them:
# Fire Mountain (eggs), Smokey Castle (bananas), Darkwater Beach and Icicle
# Pyramid (battle). objects.c:9358-9367 awards one amulet piece per course, and
# only for a course that is not already RACE_CLEARED.
TT_CHALLENGES = (11, 25, 26, 27)
CHALLENGE_FRAMES = 5000
CHALLENGE_SCRIPT = ROOT / "tests/input_scripts/tt_challenge_win.txt"


def run_tt_challenge(
    binary: Path, rom: Path, label: str, fixture: bytes, course: int,
    timeout: int, failures: list[str],
) -> Run:
    run = invoke(
        binary, rom, label, fixture=fixture, script=CHALLENGE_SCRIPT,
        frames=CHALLENGE_FRAMES, timeout=timeout,
        values={
            "MDKR_AUTOPILOT": "1",
            "MDKR_LOAD_TRACK": str(course),
            # The production challenge driver: it delivers the mode's own
            # terminal gameplay events (an egg hatch, a deposited banana, a
            # point of health) and cannot assign raceFinished, a place, a
            # verdict, ttAmulet or save data. See platform/mdkr_challenge.c.
            "MDKR_CHALLENGE_OUTCOME": "win",
        },
    )
    base_failures(run, failures)
    if not any(int(m["level"]) == course for m in LEVEL_RE.finditer(run.output)):
        failures.append(f"{label}: challenge course {course} never loaded")
    return run


def run_seam_tt(
    binary: Path, rom: Path, base: bytes, eligible: list[int],
    timeout: int, failures: list[str],
) -> bytes:
    """Seam T.T. -- ttAmulet 1, 2, 3, 4 across four EEPROM round trips.

    Shaped exactly like seam B: four wins, each fought in its own process on
    the EEPROM the previous one persisted, so the climb is something production
    wrote four times rather than something a fixture asserted.

    The negative control is the second half of the seam and the reason it means
    anything. objects.c only awards a piece when the course is not already
    RACE_CLEARED, so replaying the FIRST challenge -- identical course,
    identical forced win, on the save that challenge itself produced -- must
    award nothing and leave ttAmulet at one. Run at one piece rather than at
    four deliberately: at four the counter is clamped anyway
    (`if (i > 4) i = 4`), so a control there would pass even if the gate were
    gone.
    """
    carried = base
    after_first: bytes | None = None
    for index, course in enumerate(TT_CHALLENGES, start=1):
        label = f"seamTT-c{course}"
        incoming = decode(carried)
        run = run_tt_challenge(
            binary, rom, label, carried, course, timeout, failures
        )
        if run.save is None:
            failures.append(f"{label}: no EEPROM was written")
            return carried
        state = decode(run.save)
        if not state["checksum_ok"]:
            failures.append(f"{label}: persisted slot checksum is invalid")
        if state["tt_amulet"] != index:
            failures.append(
                f"{label}: ttAmulet={state['tt_amulet']}, want exactly {index} "
                f"piece(s) after this many challenge wins"
            )
        if course_status(run.save[:SLOT_BYTES], eligible, course) != STATUS_CLEARED:
            failures.append(f"{label}: challenge course {course} was not cleared")
        scenes = [
            m for m in LEVEL_RE.finditer(run.output)
            if int(m["level"]) == TT_AMULET_SEQUENCE
        ]
        if len(scenes) != 1:
            failures.append(
                f"{label}: {len(scenes)} T.T. amulet cutscenes, want 1"
            )
        for name in ("bosses", "wizpig_amulet", "trophies"):
            if state[name] != incoming[name]:
                failures.append(
                    f"{label}: {name} changed across a challenge win "
                    f"({incoming[name]!r} -> {state[name]!r})"
                )
        carried = run.save
        if index == 1:
            after_first = run.save

    if after_first is None:
        return carried
    control = run_tt_challenge(
        binary, rom, "seamTT-control", after_first, TT_CHALLENGES[0],
        timeout, failures,
    )
    if control.save is None:
        failures.append("seamTT-control: no EEPROM was written")
    else:
        state = decode(control.save)
        if state["tt_amulet"] != 1:
            failures.append(
                f"seamTT-control: replaying an already-cleared challenge moved "
                f"ttAmulet to {state['tt_amulet']}; the piece is not gated on "
                f"the course being new"
            )
        if any(int(m["level"]) == TT_AMULET_SEQUENCE
               for m in LEVEL_RE.finditer(control.output)):
            failures.append(
                "seamTT-control: the amulet cutscene played for a challenge "
                "that had already been won"
            )
    return carried


# ------------------------------------------------------------------- seam E


# The Wizpig 2 win, then the cutscene stack, then MENU_CREDITS. Measured on the
# fixture this seam builds: level 62 (ASSET_LEVEL_WIZPIG2ANIM) at frame ~7400,
# MENU_CREDITS at ~12050.
WIZPIG_FRAMES = 17000
CHEAT_FRAMES = 4200
CREDITS_SCRIPT = ROOT / "tests/input_scripts/wizpig_to_credits.txt"
CREDITS_RE = re.compile(
    r"credits: cheat=(?P<cheat>\d+) bosses=0x(?P<bosses>[0-9a-f]+) "
    r"ending=(?P<ending>\w+) sequence=(?P<sequence>\d+)"
)
# game/src/menu.c:15188-15201 -- the true ending, and the sequence it plays.
ENDING_TRUE = "TO_BE_CONTINUED"
ENDING_CHEAT = "THE_END"
# Ordinals in game/include/sequence_ids.h.
SEQUENCE_DARKMOON_CAVERNS = 8
SEQUENCE_CRESCENT_ISLAND = 37


def run_seam_e(
    binary: Path, rom: Path, base: bytes, topos: dict[int, WorldTopology],
    eligible: list[int], timeout: int, failures: list[str],
) -> dict[str, object]:
    fixture = derive_seam_e(base, topos, eligible).image(eligible)
    before = decode(fixture)
    if before["bosses"] & BOSS_WIZPIG_TWO_BIT:
        failures.append("seamE: fixture already carried the Wizpig 2 bit")
    # The campaign half of this fixture is carried, not constructed: seam B wrote
    # the four rematch bits and the four amulet pieces, seam C wrote Wizpig 1's.
    carried = decode(base)
    if before["bosses"] & carried["bosses"] != carried["bosses"]:
        failures.append("seamE: fixture lost boss bits the chain had written")
    if before["wizpig_amulet"] != 4 or not carried["bosses"] & BOSS_FIRST_BIT(WORLD_CENTRAL_AREA):
        failures.append(
            "seamE: fixture was not built on a chain that reached four amulet "
            "pieces and a Wizpig 1 win"
        )
    # `trophies` and `ttAmulet` are Wizpig 2's other gate and used to be stated
    # here. They are now produced by seams T and T.T., so the fixture must
    # ALREADY hold them; if it does not, the chain broke rather than the
    # override being missing.
    if carried["trophies"] != 0xFF or before["trophies"] != 0xFF:
        failures.append(
            f"seamE: carried trophies=0x{carried['trophies']:x}, want 0xff "
            f"produced by the four trophy championships"
        )
    if carried["tt_amulet"] != 4 or before["tt_amulet"] != 4:
        failures.append(
            f"seamE: carried ttAmulet={carried['tt_amulet']}, want 4 produced "
            f"by the four T.T. challenge wins"
        )
    run = invoke(
        binary, rom, "seamE", fixture=fixture, script=CREDITS_SCRIPT,
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

    # ...and the screen the bit selects, reached by finishing the game rather
    # than asserted about. The win pushes a stack of cutscene levels which
    # thread3_main.c:894-919 pops on the scene ending; the A press half of that
    # predicate can never fire here (func_8006C300() is zero on every frame of a
    # post-race cutscene, because game_load_level zeroes D_800DD330 on every load
    # and only sets it for a REPEAT Wizpig boss entry), so the stack draining at
    # all is the animation running -- see derive_seam_e for the arrival flag that
    # makes that possible.
    if not any(int(m["menu"]) == MENU_CREDITS for m in MENU_RE.finditer(run.output)):
        failures.append(
            "seamE: the won campaign never reached MENU_CREDITS; the post-race "
            "cutscene stack did not drain"
        )
    endings = list(CREDITS_RE.finditer(run.output))
    if len(endings) != 1:
        failures.append(f"seamE: {len(endings)} credits ending decisions, want 1")
    else:
        ending = endings[0]
        if int(ending["cheat"]) != 0:
            failures.append(
                "seamE: menu_credits_init read gViewingCreditsFromCheat set, so "
                "the campaign arm arrived at the cheat ending"
            )
        if ending["ending"] != ENDING_TRUE:
            failures.append(
                f"seamE: credits chose {ending['ending']}, want {ENDING_TRUE} "
                f"(bosses=0x{int(ending['bosses'], 16):x})"
            )
        if int(ending["sequence"]) != SEQUENCE_CRESCENT_ISLAND:
            failures.append(
                f"seamE: credits played sequence {ending['sequence']}, want "
                f"SEQUENCE_CRESCENT_ISLAND ({SEQUENCE_CRESCENT_ISLAND})"
            )
        if not int(ending["bosses"], 16) & BOSS_WIZPIG_TWO_BIT:
            failures.append(
                "seamE: the credits screen read bosses without the Wizpig 2 bit"
            )

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
    cheat_endings = list(CREDITS_RE.finditer(cheat.output))
    if len(cheat_endings) != 1:
        failures.append(
            f"credits-cheat: {len(cheat_endings)} ending decisions, want 1"
        )
    else:
        chosen = cheat_endings[0]
        if int(chosen["cheat"]) != 1 or chosen["ending"] != ENDING_CHEAT:
            failures.append(
                f"credits-cheat: chose {chosen['ending']} with cheat="
                f"{chosen['cheat']}, want {ENDING_CHEAT} from the cheat flag -- "
                f"the two endings are no longer distinguished"
            )
        if int(chosen["bosses"], 16) & BOSS_WIZPIG_TWO_BIT:
            failures.append(
                "credits-cheat: the cheat route carried the Wizpig 2 bit, so it "
                "is no longer the state-free contrast this arm relies on"
            )
    if cheat.save is None:
        failures.append("credits-cheat: no EEPROM was written")
    elif decode(cheat.save)["checksum_ok"]:
        failures.append(
            "credits-cheat: the cheat route started a campaign file, so it is no "
            "longer the state-free contrast this arm relies on"
        )
    return {"bosses": state["bosses"]}


# ---------------------------------------------------------------------- main


def report(failures: list[str]) -> int:
    print(f"check_campaign_progression: FAIL ({len(failures)} issue(s))")
    for failure in failures:
        print(f"  - {failure}")
    return 1


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

        control = run_seam_b(
            binary, rom, topos[dino.world], eligible, args.timeout, failures,
            first_boss_beaten=False,
        )
        print(f"  seam B  first-encounter control: "
              f"bosses=0x{control.get('bosses', 0):x} amulet={control.get('amulet')}")

        if args.quick:
            result = run_seam_b(binary, rom, dino, eligible, args.timeout, failures)
            print(f"  seam B  world {dino.world} rematch {dino.rematch_boss}: "
                  f"bosses=0x{result.get('bosses', 0):x} amulet={result.get('amulet')}")
            if failures:
                return report(failures)
            print("check_campaign_progression: PASS (quick)")
            return 0

        # The completeness arm for the lobby door: same fixture and same
        # assertions as the retarget arm above, but the rematch is entered by
        # driving the human through the lobby's own boss door.
        driven = run_seam_b(
            binary, rom, dino, eligible, args.timeout, failures, driven=True
        )
        print(f"  seam B  world {dino.world} rematch {dino.rematch_boss} DRIVEN "
              f"through the lobby door: bosses=0x{driven.get('bosses', 0):x} "
              f"amulet={driven.get('amulet')}")

        # The chain proper. Each rematch is fought on the EEPROM the previous one
        # persisted, so wizpigAmulet reaching four is something production wrote
        # four times, not something this file asserted into a fixture.
        carried = derive_all_worlds_silver(topos).image(eligible)
        three_pieces = carried
        for index, world in enumerate(
            sorted(w for w in topos if w != WORLD_FUTURE_FUN_LAND), start=1
        ):
            if index == 4:
                three_pieces = carried
            result = run_seam_b(
                binary, rom, topos[world], eligible, args.timeout, failures,
                fixture=carried, expect_amulet=index,
            )
            if result.get("save") is None:
                failures.append(f"seam B chain: world {world} produced no save")
                break
            carried = result["save"]
            print(f"  seam B  world {world} rematch {topos[world].rematch_boss}: "
                  f"bosses=0x{result.get('bosses', 0):x} amulet={result.get('amulet')}")

        if not failures:
            after_wizpig_one = run_seam_c(
                binary, rom, carried, three_pieces, eligible, args.timeout, failures
            )
            print(f"  seam C  Wizpig 1: bosses="
                  f"0x{decode(after_wizpig_one)['bosses']:x} "
                  f"cutscenes=0x{decode(after_wizpig_one)['cutscenes']:x}")

            after_trophies = run_seam_t(
                binary, rom, after_wizpig_one, eligible, failures
            )
            print(f"  seam T  four championships: "
                  f"trophies=0x{decode(after_trophies)['trophies']:x}")

            after_amulet = run_seam_tt(
                binary, rom, after_trophies, eligible, args.timeout, failures
            )
            print(f"  seam T.T. four challenges: "
                  f"ttAmulet={decode(after_amulet)['tt_amulet']}")

            seam_e = run_seam_e(
                binary, rom, after_amulet, topos, eligible, args.timeout, failures
            )
            print(f"  seam E  Wizpig 2: bosses=0x{seam_e.get('bosses', 0):x}")
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    if failures:
        return report(failures)
    print("check_campaign_progression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
