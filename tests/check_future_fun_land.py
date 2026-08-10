#!/usr/bin/env python3
"""Witness the lighthouse rocket unlock -- Future Fun Land, opened by playing.

Why this exists
---------------
Future Fun Land is the last world, and the only thing that opens it is
`begin_lighthouse_rocket_cutscene()` (game/src/thread3_main.c:1925-1938), called
from exactly one place: the rocket signpost on Timber's Island
(`obj_loop_rocketsignpost`, game/src/object_functions.c). It fires only when

    (settings->trophies & 0xFF) == 0xFF        four gold trophies
    !(cutsceneFlags & CUTSCENE_LIGHTHOUSE_ROCKET)   not already seen
    settings->bosses & 1                       Wizpig 1 beaten

and when it does it latches the cutscene bit, plays ASSET_LEVEL_ROCKETSEQUENCE
and warps to ASSET_LEVEL_FUTUREFUNLANDHUB. `check_campaign_progression.py`
carries Future Fun Land's own state as a fixture premise -- it does not drive
there -- so without this file the door into the last world is never opened by
anything.

Why three arms
--------------
A refusal writes NOTHING. No save bit, no level load, no trace. So "the route
never reached the signpost" and "the signpost read the save and said no" look
identical from outside, and a one-armed gate would pass with the whole unlock
deleted. The `rocketsign: trigger` line exists for exactly that reason: it is
emitted where the honk/collision is detected and BEFORE the gate is evaluated,
so every arm below proves it got to the signpost, and the arms differ only in
what the save said when it arrived.

    unlock       four golds + Wizpig 1  ->  rocket sequence, FFL hub, bit set
    one short    three golds + a SILVER ->  trigger fires, nothing happens
    no Wizpig 1  four golds, bit cleared ->  trigger fires, nothing happens

Where the trophies come from
----------------------------
Not from this file. `check_trophy_series.drive_gold_championship` is that gate's
own championship driver -- production owns round selection, points, sorting,
the podium, the trophy upgrade and the EEPROM write -- and the four worlds are
chained on one another's saves, so `trophies == 0xFF` is a value production
wrote four times. The "one short" arm is produced the same way: worlds 1-3 gold,
world 4 driven to second place, which production turns into a silver.

What is constructed rather than produced is the campaign checkpoint underneath:
all four worlds silver-coin complete and Wizpig 1 beaten, which is the state
`check_campaign_progression.py`'s seams A-C are what witness. The
`bosses`-cleared arm is a deliberate negative control and is NOT a legitimate
campaign state, exactly like seam B's first-boss control.

Every EEPROM image is built in a private temporary directory and deleted. No
ROM-derived data is committed by this check.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from check_campaign_progression import (
    BAD_RE, BOSS_FIRST_BIT, BOSS_REMATCH_BIT, carry_trophy_save,
    CUTSCENE_WIZPIG_FACE, decode, derive_all_worlds_silver, LEVEL_RE, Slot,
    STATUS_CLEARED, WIZPIG_ONE, WORLD_CENTRAL_AREA, WORLD_FUTURE_FUN_LAND,
    world_topology,
)
from check_first_boss_progression import save_order
from check_trophy_series import drive_gold_championship, run_case
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary


ROOT = Path(__file__).resolve().parent.parent
HUB_SCRIPT = ROOT / "tests/input_scripts/adventure_race_loop.txt"

# game/include/asset_enums.h ordinals.
ASSET_LEVEL_FUTUREFUNLANDHUB = 35
ASSET_LEVEL_ROCKETSEQUENCE = 45
# game/include/structs.h:274.
CUTSCENE_LIGHTHOUSE_ROCKET = 0x1

# Racer 0 second in every round: 7 points a round, 28, championship rank 1,
# which production turns into a SILVER. The same order check_trophy_series.py's
# world matrix uses for its rank-1 podium branch.
SILVER_ORDER = "/".join(("1,0,2,3,4,5,6,7",) * 4)
TROPHIES_ALL_GOLD = 0xFF
TROPHIES_ONE_SHORT = 0xBF  # worlds 1-3 gold (0x3f) plus a world-4 silver (0x80)

# The rocket signpost sits at (3892, -149, 2226) -- read from its own
# `rocketsign: spawned` line, not guessed. The first four waypoints are lifted
# from check_adventure_hub.py's hub tour, which is sampled from ground the port
# is documented to drive; the last three extend it north-east to the signpost
# and one follow-through point past it, because the drive hook retires a
# waypoint at 220 units and the trigger wants a collision, not a near miss.
SIGNPOST_ROUTE = (
    "0:200,500:896,928:1534,1693:2400,1900:3200,2100:3892,2226:4100,2350"
)
# Measured on the unlock arm: signpost triggered at frame ~3000, rocket
# sequence at ~3038, Future Fun Land's hub at ~4119.
HUB_FRAMES = 6000

SIGNPOST_SPAWN_RE = re.compile(
    r"rocketsign: spawned pos=\((?P<x>\S+), (?P<y>\S+), (?P<z>\S+)\)"
)
SIGNPOST_TRIGGER_RE = re.compile(
    r"rocketsign: trigger distance=(?P<distance>-?\d+) "
    r"trophies=0x(?P<trophies>[0-9a-f]+) bosses=0x(?P<bosses>[0-9a-f]+) "
    r"cutsceneFlags=0x(?P<flags>[0-9a-f]+)"
)


def campaign_checkpoint(rom: Path, eligible: list[int]) -> bytes:
    """The state seams A-C of check_campaign_progression.py leave behind.

    Four worlds silver-coin complete (which is what seam A proves a silver clear
    does, applied sixteen times), all four rematches won -- their `bosses` bits
    and the four amulet pieces, which is seam B -- and Wizpig 1's cleared course
    and central-area bit, which is seam C. Nothing here is a trophy: production
    must award all four.

    The rematch bits are not padding. `obj_loop_trophycab` only opens a world's
    cabinet on `balloonsPtr[worldId] >= 8 && (1 << (worldId + 6)) & bosses` --
    eight world balloons AND that world's rematch beaten -- so a save holding
    four golds necessarily holds all four rematch bits. Without them the cabinet
    refuses and the championships award nothing at all (measured: trophies
    stayed 0x0 through three worlds).
    """
    topos = world_topology(str(rom), eligible)
    slot = derive_all_worlds_silver(topos)
    for world in sorted(topos):
        if world != WORLD_FUTURE_FUN_LAND:
            slot.bosses |= BOSS_REMATCH_BIT(world)
            slot.status[topos[world].rematch_boss] = STATUS_CLEARED
    slot.wizpig_amulet = 4
    slot.bosses |= BOSS_FIRST_BIT(WORLD_CENTRAL_AREA)
    slot.status[WIZPIG_ONE] = STATUS_CLEARED
    # ...and the scene four pieces triggers, already seen. game_load_level
    # redirects a hub load to ASSET_LEVEL_WIZPIGMOUTHSEQUENCE at four amulet
    # pieces until CUTSCENE_WIZPIG_FACE is latched (game/src/game.c:648), and a
    # player who has beaten Wizpig 1 has necessarily watched it. Without the
    # flag every run here spends the redirect and never reaches the lobby
    # (measured: two hub loads, no level 12 in 13,000 frames).
    slot.cutscenes |= CUTSCENE_WIZPIG_FACE
    return slot.image(eligible)


def clear_wizpig_one(save: bytes, eligible: list[int]) -> bytes:
    """The negative control: the same save with Wizpig 1 un-beaten.

    Deliberately not a legitimate state -- a player holding four golds has
    necessarily beaten Wizpig 1 -- and that is the point: it isolates the
    `bosses & 1` conjunct of the unlock from the trophy one.
    """
    slot = Slot.from_save(save, eligible)
    slot.bosses &= ~BOSS_FIRST_BIT(WORLD_CENTRAL_AREA)
    slot.status.pop(WIZPIG_ONE, None)
    return slot.image(eligible)


def drive_to_signpost(
    binary: Path, rom: Path, label: str, fixture: bytes, timeout: int,
) -> tuple[int, str, bytes]:
    """One headless process, in its own temporary directory and save dir."""
    with tempfile.TemporaryDirectory(prefix=f"mdkr_ffl_{label}_") as temp:
        run_dir = Path(temp)
        save_dir = run_dir / "save"
        save_dir.mkdir()
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
            MDKR_AUTOPILOT="1",
            MDKR_DRIVE_ROUTE=SIGNPOST_ROUTE,
        )
        proc = subprocess.run(
            [str(binary), "--headless-frames", str(HUB_FRAMES),
             "--input-script", str(HUB_SCRIPT), "--rom", str(rom)],
            cwd=run_dir, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=timeout, check=False,
        )
        eeprom = save_dir / "eeprom.bin"
        return proc.returncode, proc.stdout or "", (
            eeprom.read_bytes() if eeprom.is_file() else b""
        )


def check_arm(
    binary: Path, rom: Path, label: str, fixture: bytes, timeout: int,
    *, expect_unlock: bool, failures: list[str],
) -> None:
    returncode, output, save = drive_to_signpost(
        binary, rom, label, fixture, timeout
    )
    if returncode != 0:
        failures.append(f"{label}: process exit {returncode}")
    marker = BAD_RE.search(output)
    if marker:
        failures.append(f"{label}: runtime failure marker {marker.group(0)!r}")

    if not SIGNPOST_SPAWN_RE.search(output):
        failures.append(f"{label}: the rocket signpost never spawned")
    triggers = list(SIGNPOST_TRIGGER_RE.finditer(output))
    if not triggers:
        # This is the failure a one-armed gate would have reported as a pass.
        failures.append(
            f"{label}: the route never triggered the rocket signpost, so this "
            f"arm says nothing about the unlock gate"
        )
        return
    incoming = decode(fixture)
    if int(triggers[0]["trophies"], 16) != incoming["trophies"]:
        failures.append(
            f"{label}: the signpost read trophies "
            f"0x{int(triggers[0]['trophies'], 16):x}, but the fixture holds "
            f"0x{incoming['trophies']:x}"
        )

    loads = [int(m["level"]) for m in LEVEL_RE.finditer(output)]
    rockets = loads.count(ASSET_LEVEL_ROCKETSEQUENCE)
    reached_ffl = ASSET_LEVEL_FUTUREFUNLANDHUB in loads
    persisted = bool(save) and bool(
        decode(save)["cutscenes"] & CUTSCENE_LIGHTHOUSE_ROCKET
    )

    if expect_unlock:
        if rockets != 1:
            failures.append(
                f"{label}: {rockets} loads of ASSET_LEVEL_ROCKETSEQUENCE "
                f"({ASSET_LEVEL_ROCKETSEQUENCE}), want 1"
            )
        if not reached_ffl:
            failures.append(
                f"{label}: the rocket cutscene did not warp to "
                f"ASSET_LEVEL_FUTUREFUNLANDHUB ({ASSET_LEVEL_FUTUREFUNLANDHUB})"
            )
        if not persisted:
            failures.append(
                f"{label}: CUTSCENE_LIGHTHOUSE_ROCKET was not persisted, so a "
                f"second session would not find Future Fun Land open"
            )
    else:
        if rockets:
            failures.append(
                f"{label}: the lighthouse cutscene played anyway "
                f"(trophies=0x{incoming['trophies']:x} "
                f"bosses=0x{incoming['bosses']:x})"
            )
        if reached_ffl:
            failures.append(f"{label}: warped to Future Fun Land anyway")
        if persisted:
            failures.append(
                f"{label}: CUTSCENE_LIGHTHOUSE_ROCKET was set anyway"
            )


def build_trophy_saves(
    binary: Path, rom: Path, failures: list[str], base: bytes,
    eligible: list[int],
) -> tuple[bytes | None, bytes | None]:
    """Three golds, then the fourth world twice: gold, and second place."""
    carried = base
    for world in (1, 2, 3):
        proc, produced = drive_gold_championship(binary, rom, world, carried)
        if proc.returncode != 0:
            failures.append(f"trophy w{world}: process exit {proc.returncode}")
            return None, None
        carried = carry_trophy_save(base, produced, eligible)
        print(f"  championship w{world}: "
              f"trophies=0x{decode(carried)['trophies']:x}")
    if decode(carried)["trophies"] != 0x3F:
        failures.append(
            f"trophy chain: three golds gave "
            f"0x{decode(carried)['trophies']:x}, want 0x3f"
        )
        return None, None

    gold_proc, produced = drive_gold_championship(binary, rom, 4, carried)
    all_gold = carry_trophy_save(base, produced, eligible)
    if gold_proc.returncode != 0:
        failures.append(f"trophy w4 gold: process exit {gold_proc.returncode}")
        all_gold = None
    elif decode(all_gold)["trophies"] != TROPHIES_ALL_GOLD:
        failures.append(
            f"trophy w4 gold: trophies=0x{decode(all_gold)['trophies']:x}, "
            f"want 0x{TROPHIES_ALL_GOLD:x}"
        )
        all_gold = None

    silver_proc, silver_produced = run_case(
        binary, rom, 13000, carried, "retry", SILVER_ORDER, 4
    )
    one_short = carry_trophy_save(base, silver_produced, eligible)
    if silver_proc.returncode != 0:
        failures.append(f"trophy w4 silver: process exit {silver_proc.returncode}")
        one_short = None
    elif decode(one_short)["trophies"] != TROPHIES_ONE_SHORT:
        failures.append(
            f"trophy w4 silver: trophies=0x{decode(one_short)['trophies']:x}, "
            f"want 0x{TROPHIES_ONE_SHORT:x} -- the 'one trophy short' arm is "
            f"not one trophy short"
        )
        one_short = None
    return all_gold, one_short


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom, HUB_SCRIPT):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    try:
        eligible = save_order(str(rom))
        base = campaign_checkpoint(rom, eligible)
    except (OSError, ValueError, IndexError) as exc:
        print(f"FAIL: could not build the campaign checkpoint: {exc}",
              file=sys.stderr)
        return 1

    print("Future Fun Land unlock")
    try:
        all_gold, one_short = build_trophy_saves(
            binary, rom, failures, base, eligible
        )
        if all_gold is not None:
            check_arm(binary, rom, "unlock", all_gold, args.timeout,
                      expect_unlock=True, failures=failures)
            check_arm(binary, rom, "no-wizpig",
                      clear_wizpig_one(all_gold, eligible), args.timeout,
                      expect_unlock=False, failures=failures)
        if one_short is not None:
            check_arm(binary, rom, "one-short", one_short, args.timeout,
                      expect_unlock=False, failures=failures)
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    if failures:
        print(f"check_future_fun_land: FAIL ({len(failures)} issue(s))")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_future_fun_land: PASS -- four production golds plus Wizpig 1 "
          "open the lighthouse; one silver, or no Wizpig 1, does not")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
