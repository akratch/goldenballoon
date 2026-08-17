#!/usr/bin/env python3
"""P3.5b check: the ADVENTURE RACE LOOP -- hub -> race -> back to the hub.

Why this exists
---------------
`check_adventure_hub.py` proved Timber's Island is *drivable*.  It never entered
a race, so the Adventure loop -- the mode most of DKR's play time lives in -- was
still unvalidated end to end:

    Timber's Island  --(door)-->  Dino Domain lobby  --(door)-->  Ancient Lake
                     <--(exit)--                     <--(post-race)--

Entering a race means driving through a BHV_EXIT object: `obj_loop_exit()` latches
`racer->exitObj` (inside the exit's activation radius AND on the correct side of
its facing plane), `racer_enter_door()` then drives the final approach and calls
`func_8006D968()` on the exit's `level_entry`.  None of that had ever run.  18
open-loop stick routes had failed to hit a door and `MDKR_AUTOPILOT=1` laps the
hub's AI-node spline for ever without turning in at one (closest approach to any
hub exit: 2144 units), so the route here is driven by closed-loop steering at
named objects -- `MDKR_DRIVE_ROUTE`, see platform/mdkr_adventure.c.

What this asserts
-----------------
  1. **The whole chain of transitions, in order and by frame.**  levelId 0
     (CENTRALAREAHUB) -> 12 (DINODOMAINHUB) -> 5 (ANCIENTLAKE) -> 12 -> 0, each
     within a frame window.  The two returning loads must carry the entrance ids
     DKR derives from the exits' `returnSpawnIndex`/`overworldSpawnIndex`
     (3 and 2), which is what distinguishes a real return transition from a
     fresh load.
  2. **A golden balloon was really collected.**  The Dino Domain doors need one
     (`obj_loop_door`: `*settings->balloonsPtr >= door->balloonCount`), so the
     door-open path is only exercised if the balloon flag was actually set.
  3. **The full race really ran.**  All three laps of `[PACE]` progress on
     Ancient Lake, a plausible finish clock, `fin=1` from controller port 1's
     racer (not starting-grid slot zero), and real motion: total path length,
     no single-frame teleport, finite positions.
  4. **The kart is alive and controllable back in the hub.**  Post-return
     `[PACE]` rows must show fresh movement in levelId 0, not a frozen kart.
  5. **The scene still draws** at the end, using check_race_drive.py's own
     colour/sigma metric and thresholds, so a silent "world stopped drawing"
     regression (the ASSET_MISC_8 shape) fails here too.
  6. **The process exits 0.**  Three of the bugs on this route's neighbourhood
     aborted with NOTHING on stderr (`__stack_chk_fail` goes straight to
     SIGABRT), so the return code is part of the assertion, not just the log.

  7. **Win and loss persistence, independently decoded.** Verdict hooks run only
     after the human naturally finishes all three laps, then request first or
     non-first place. The arms must report the same natural result even when
     Debug/Release/sanitizer builds produce different natural places. Win must
     persist Ancient Lake cleared plus exactly one Dino Domain race balloon;
     loss must persist neither. Finish frame and clock must match, proving the
     hook changes the verdict rather than the drive or finish.

EEPROM isolation
----------------
Each arm runs in its own temporary working directory with a fresh `save/`
directory. This is load-bearing, not tidiness: with a save present, FILE SELECT
resumes an existing file instead of starting a new game, and the whole route
changes. The 40-byte slot is captured before that directory is removed and
decoded here using the ROM's level-header table and save_data.c's MSB-first bit
layout. No shared `save/eeprom.bin` is read, removed, or left behind.

Usage:
    tests/check_adventure_race_loop.py [--build build] [--rom baserom.us.v80.z64]
                                       [--keep-frames DIR] [-v]

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one assertion failed (each
printed with the measured value).
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import struct
import subprocess
import sys
import tempfile

from harness_utils import (DEFAULT_BUILD_DIR, resolve_binary, SLOT_BYTES,
                           slot_checksum)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "tests", "input_scripts", "adventure_race_loop.txt")
FRAMES = 17000
ASSET_LEVEL_HEADERS_TABLE = 22
ASSET_LEVEL_HEADERS = 23
LEVELHEADER_RACE_TYPE = 0x4C
ROM_LAYOUTS = {
    (0xE402430D, 0xD2FCFC9D): (0x000ED0E0, 0x000ED1B0),  # us.v80
    (0x596E145B, 0xF7D9879F): (0x000ED170, 0x000ED240),  # pal.v80
}

HUB = 0        # ASSET_LEVEL_CENTRALAREAHUB   (Timber's Island)
LOBBY = 12     # ASSET_LEVEL_DINODOMAINHUB    (the Dino Domain world hub)
RACE = 5       # ASSET_LEVEL_ANCIENTLAKE
CUTSCENE = 36  # ASSET_LEVEL_OPENINGSEQUENCE  (the new-game intro)

# The route steered by the drive hook.  The hub waypoints are points ON the
# trajectory the pre-existing open-loop fixture already proved drivable
# (adventure_hub_drive.txt), sampled from its own [PACE] trace; the balloon and
# the exits are named objects, so their coordinates come from the game.  The
# first waypoint (200, 500) exists only to swing wide of Taj at (-197, 255, 193):
# bumping him opens a dialogue that calls disable_racer_input() every frame and
# wedges the kart permanently (measured frozen at (-125.3, 149.6), 84 units away).
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
    ":-3381,1946:-2519,1516:-1858,1099"
    ";12:E5:E0"
)

# --- thresholds -------------------------------------------------------------
# Every "measured" number is from the current build on US v80.  The route is
# deterministic (tests/check_determinism.py), so these are stable; the windows
# are wide enough to absorb a small physics change but narrow enough that a
# missed transition fails.
WINDOWS = {          # levelId -> (first-load-frame lo, hi)   measured
    HUB:   (5000, 7000),      # 5867
    LOBBY: (6000, 9000),      # 6648
    RACE:  (6200, 9500),      # 6950
}
RETURN_LOBBY_MAX = 16000      # measured 13492
RETURN_HUB_MAX = 16500        # measured 13840
LOBBY_RETURN_ENTRANCE = 3     # the Ancient Lake door (exit dest=5 retSpawn=3)
HUB_RETURN_ENTRANCE = 2       # the Dino Domain exit  (exit dest=0 owSpawn=2)

MIN_RACE_FRAMES = 3000        # measured 6543 rows between the race load and the
                              # return to the lobby
# `lap=` comes from update_player_racer, which stops publishing when a racer
# finishes. `rlap=` comes from race_check_finish and observes the finish itself.
MIN_RACE_RLAP = 3
CLOCK_MIN, CLOCK_MAX = 4000, 7000   # measured 5027
MIN_RACE_PATH = 50000.0       # measured ~64000 world units over three laps
MIN_HUB_BACK_FRAMES = 300     # measured 3158 after the return
MIN_HUB_BACK_PATH = 500.0     # measured 37433; only needs to prove it moves at all
MAX_STEP = 40.0               # world units in one frame (same bound as the race check)
MAX_RETURN_LOBBY_Y = 80.0     # ground is ~5; the broken moving-door launch reached 232
LOAD_SKIP = 3                 # racer rows to drop after a level load (see race_rows below)
MIN_DISTINCT_COLOURS = 200    # check_race_drive.py calibration: broken build 59
MIN_LUMA_SIGMA = 12.0         # check_race_drive.py calibration: broken build 5.9
# Slack for the route's THREE level-transition fades, each of which whites the
# screen out for ~30 frames. The current 800-frame sampling grid happens to miss
# all of them -- measured 13/13 samples at 1121-2990 colours / sigma 32.8-61.6 --
# so this allowance is unused headroom, kept so a small timing shift that lands a
# sample inside a fade does not fail the check for a reason that is not a bug.
MAX_BAD_SAMPLES = 2
SAMPLE_EVERY = 800

PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(-?\d+) lap=(-?\d+)"
    r" rlap=(-?\d+) fin=(-?\d+).*fpos=(-?\d+) ridx=(-?\d+) pidx=(-?\d+)")
LEVEL_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=(-?\d+) entrance=(-?\d+) vehicle=(-?\d+)"
    r" cutscene=(-?\d+) @frame~(\d+)")
BALLOON_RE = re.compile(r"drive: level=(\d+) step \d+: balloon (\d+) COLLECTED \(total=(\d+)\) @frame~(\d+)")
EXIT_RE = re.compile(r"drive: level=(\d+) step \d+: exit to (\d+) TAKEN \(now in level (\d+)\) @frame~(\d+)")


def load_scene_metrics():
    """Reuse check_race_drive.py's scene metric so the checks agree by construction."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_race_drive.py")
    spec = importlib.util.spec_from_file_location("mdkr_check_race_drive", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.scene_metrics


def is_finite(v: float) -> bool:
    return v == v and -1e30 < v < 1e30


def path_and_step(rows):
    """Total path length and the largest single-frame jump over consecutive frames."""
    path, max_step, at = 0.0, 0.0, None
    for a, b in zip(rows, rows[1:]):
        if b[0] != a[0] + 1:
            continue
        if not all(is_finite(v) for v in (a[1], a[2], a[3], b[1], b[2], b[3])):
            continue
        d = ((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2 + (b[3] - a[3]) ** 2) ** 0.5
        path += d
        if d > max_step:
            max_step, at = d, b[0]
    return path, max_step, at


def normalize_rom(rom: bytes) -> bytes:
    """Normalize z64/v64/n64 bytes exactly as the native ROM loader does."""
    magic = rom[:4]
    if magic == b"\x80\x37\x12\x40":
        return rom
    normalized = bytearray(len(rom))
    if magic == b"\x37\x80\x40\x12":
        normalized[0::2] = rom[1::2]
        normalized[1::2] = rom[0::2]
    elif magic == b"\x40\x12\x37\x80":
        normalized[0::4] = rom[3::4]
        normalized[1::4] = rom[2::4]
        normalized[2::4] = rom[1::4]
        normalized[3::4] = rom[0::4]
    else:
        raise ValueError(f"unsupported ROM byte-order magic {magic.hex()}")
    return bytes(normalized)


def rom_section(rom: bytes, index: int, lut_start: int, data_base: int) -> bytes:
    """Decode one asset section independently of game code."""
    start = struct.unpack_from(">I", rom, lut_start + 4 * (index + 1))[0]
    end = struct.unpack_from(">I", rom, lut_start + 4 * (index + 2))[0]
    return rom[data_base + start:data_base + end]


def read_bits(data: bytes, offset: int, width: int) -> int:
    value = 0
    for bit in range(offset, offset + width):
        value = (value << 1) | ((data[bit // 8] >> (7 - bit % 8)) & 1)
    return value


def decode_progress(save: bytes, rom_path: str) -> dict[str, object]:
    """Decode slot 0 using the ROM's actual save-eligible level ordering."""
    if len(save) < SLOT_BYTES:
        raise ValueError(f"EEPROM is only {len(save)} bytes (need at least {SLOT_BYTES})")
    slot = save[:SLOT_BYTES]
    stored_checksum = read_bits(slot, 0, 16)
    expected_checksum = slot_checksum(slot)

    with open(rom_path, "rb") as f:
        rom = normalize_rom(f.read())
    crc = struct.unpack_from(">II", rom, 0x10)
    if crc not in ROM_LAYOUTS:
        raise ValueError(f"unsupported ROM CRC1/CRC2 {crc[0]:08x}/{crc[1]:08x}")
    lut_start, data_base = ROM_LAYOUTS[crc]
    table_raw = rom_section(
        rom, ASSET_LEVEL_HEADERS_TABLE, lut_start, data_base)
    table = list(struct.unpack(f">{len(table_raw) // 4}i", table_raw))
    table_end = table.index(-1) if -1 in table else len(table)
    headers = rom_section(rom, ASSET_LEVEL_HEADERS, lut_start, data_base)
    eligible: list[int] = []
    for level_id in range(table_end - 1):  # final positive entry is the end offset
        race_type = headers[table[level_id] + LEVELHEADER_RACE_TYPE]
        if race_type == 0 or (race_type & 0x40) or race_type == 8:
            eligible.append(level_id)
    if RACE not in eligible:
        raise ValueError(f"level {RACE} is absent from ROM save ordering {eligible}")
    ordinal = eligible.index(RACE)
    course_status = read_bits(slot, 16 + ordinal * 2, 2)
    balloons_offset = 16 + 68 + 6 + 10 + 12
    balloons = tuple(read_bits(slot, balloons_offset + i * 7, 7) for i in range(6))
    return {
        "checksum_ok": stored_checksum == expected_checksum,
        "stored_checksum": stored_checksum,
        "expected_checksum": expected_checksum,
        "course_status": course_status,
        "balloons": balloons,
        "course_ordinal": ordinal,
    }


def run_case(binary: str, rom: str, frame_dir: str | None, verdict: str):
    """Run one new-save arm and capture its EEPROM before the sandbox vanishes."""
    with tempfile.TemporaryDirectory(prefix="mdkr_advloop_run_") as run_dir:
        os.mkdir(os.path.join(run_dir, "save"))
        # No inherited MDKR hook may silently alter either arm. Keep unrelated
        # process settings (including sanitizer controls) intact.
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("MDKR_")}
        env.update(
            MDKR_AUDIO="0",
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
            MDKR_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
            # Closed-loop post-race selection (menu.c POSTRACE_STAGE_OPTIONS).
            # Row 1 is RETURNTOLOBBY on both verdicts (row 0 = try again,
            # row 2 = quit adventure entirely, measured).  Without this, the
            # return-to-lobby leg rode on open-loop tap timing and the loss
            # arm measured BOTH outcomes across two trees.
            MDKR_TEST_POSTRACE_OPTION="1",
        )
        if verdict == "win":
            env["MDKR_ADVENTURE_WIN"] = "1"
        elif verdict == "loss":
            env["MDKR_ADVENTURE_LOSS"] = "1"
        if frame_dir is not None:
            env["MDKR_DUMP_FROM"] = "7000"
            env["MDKR_DUMP_EVERY"] = str(SAMPLE_EVERY)
        cmd = [
            binary, "--headless-frames", str(FRAMES),
            "--input-script", SCRIPT,
        ]
        if frame_dir is not None:
            cmd.extend(("--dump-frames", frame_dir))
        cmd.extend(("--rom", rom))
        proc = subprocess.run(
            cmd, capture_output=True, text=True, env=env, cwd=run_dir)
        eeprom = os.path.join(run_dir, "save", "eeprom.bin")
        if os.path.exists(eeprom):
            with open(eeprom, "rb") as f:
                save = f.read()
        else:
            save = b""
        return proc, proc.stdout + proc.stderr, save


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--keep-frames", default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = os.path.abspath(resolve_binary(args.build))
    args.rom = os.path.abspath(args.rom)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    scene_metrics = load_scene_metrics()
    frame_owner = None
    if args.keep_frames:
        frame_dir = os.path.abspath(args.keep_frames)
    else:
        frame_owner = tempfile.TemporaryDirectory(prefix="mdkr_advloop_")
        frame_dir = frame_owner.name
    os.makedirs(frame_dir, exist_ok=True)

    if args.verbose:
        print("  running full three-lap win arm (native verdict control)")
    proc, out, win_save = run_case(binary, args.rom, frame_dir, verdict="win")
    if args.verbose:
        print("  running identical full three-lap loss control")
    loss_proc, loss_out, loss_save = run_case(
        binary, args.rom, frame_dir=None, verdict="loss")

    failures: list[str] = []

    # 6. exit code / crash markers FIRST -- an -fstack-protector abort prints nothing.
    if proc.returncode != 0:
        failures.append(f"exit code {proc.returncode} "
                        f"(negative = killed by signal {-proc.returncode})")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            line = next((l for l in out.splitlines() if marker in l), marker)
            failures.append(f"{marker} in output: {line.strip()}")
        if marker in loss_out:
            line = next((l for l in loss_out.splitlines() if marker in l), marker)
            failures.append(f"loss control: {marker} in output: {line.strip()}")
    if loss_proc.returncode != 0:
        failures.append(f"loss control exit code {loss_proc.returncode} "
                        f"(negative = killed by signal {-loss_proc.returncode})")

    # 1. the transition chain
    levels = [(int(m.group(1)), int(m.group(3)), int(m.group(5)), int(m.group(6)))
              for m in (LEVEL_RE.search(l) for l in out.splitlines()) if m]
    seq = [(lv, fr) for lv, _e, _c, fr in levels]

    def first(levelId, after=-1):
        return next(((lv, en, cs, fr) for lv, en, cs, fr in levels if lv == levelId and fr > after), None)

    if first(CUTSCENE) is None:
        failures.append(f"the new-game intro cutscene (levelId={CUTSCENE}) never loaded — "
                        f"FILE SELECT did not start a new game. level loads: {seq}")

    hub1 = first(HUB, after=3000)   # skip the frontend's own background loads
    lobby1 = first(LOBBY)
    race1 = first(RACE)
    for got, levelId, name in ((hub1, HUB, "CENTRALAREAHUB"), (lobby1, LOBBY, "DINODOMAINHUB"),
                               (race1, RACE, "ANCIENTLAKE")):
        if got is None:
            failures.append(f"levelId={levelId} ({name}) never loaded — level loads: {seq}")
        else:
            lo, hi = WINDOWS[levelId]
            if not (lo <= got[3] <= hi):
                failures.append(f"levelId={levelId} ({name}) loaded at frame {got[3]}, "
                                f"expected within [{lo}, {hi}]")
    if hub1 is None or lobby1 is None or race1 is None:
        print_result(failures)
        return 1
    if not (hub1[3] < lobby1[3] < race1[3]):
        failures.append(f"transitions out of order: hub @{hub1[3]}, lobby @{lobby1[3]}, "
                        f"race @{race1[3]} (want hub < lobby < race)")

    lobby2 = first(LOBBY, after=race1[3])
    if lobby2 is None:
        failures.append("never returned to the Dino Domain lobby after the race — "
                        f"level loads after the race: {[(lv, fr) for lv, fr in seq if fr > race1[3]]}")
    else:
        if lobby2[3] > RETURN_LOBBY_MAX:
            failures.append(f"returned to the lobby at frame {lobby2[3]} (expected <= {RETURN_LOBBY_MAX})")
        if lobby2[1] != LOBBY_RETURN_ENTRANCE:
            failures.append(f"the lobby return load has entrance={lobby2[1]}, expected "
                            f"{LOBBY_RETURN_ENTRANCE} (the Ancient Lake door, from the exit's "
                            f"returnSpawnIndex) — this may not be a real return transition")

    hub2 = first(HUB, after=lobby2[3]) if lobby2 else None
    if hub2 is None:
        failures.append("never returned to Timber's Island (levelId=0) — "
                        f"level loads: {seq}")
    else:
        if hub2[3] > RETURN_HUB_MAX:
            failures.append(f"returned to the hub at frame {hub2[3]} (expected <= {RETURN_HUB_MAX})")
        if hub2[1] != HUB_RETURN_ENTRANCE:
            failures.append(f"the hub return load has entrance={hub2[1]}, expected "
                            f"{HUB_RETURN_ENTRANCE} (the Dino Domain exit, from the exit's "
                            f"overworldSpawnIndex)")

    # 2. a balloon was really collected, and the exits were really taken
    balloons = [(int(m.group(2)), int(m.group(3)), int(m.group(4)))
                for m in (BALLOON_RE.search(l) for l in out.splitlines()) if m]
    if not balloons:
        failures.append("the route's golden-balloon step never completed: balloon 10's flag in "
                        "courseFlagsPtr was never set. The Dino Domain doors need 1 balloon, so "
                        "the route is not driving what it claims to drive")
    elif balloons[0][1] < 1:
        failures.append(f"balloon collected but the total is {balloons[0][1]}, expected >= 1")

    exits = [(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
             for m in (EXIT_RE.search(l) for l in out.splitlines()) if m]
    for src, dst in ((HUB, LOBBY), (LOBBY, RACE), (LOBBY, HUB)):
        if not any(e[0] == src and e[1] == dst for e in exits):
            failures.append(f"the drive hook never reported taking the exit from level {src} "
                            f"to level {dst} — exits taken: {[(e[0], e[1], e[3]) for e in exits]}")

    # 3. the race really ran
    rows = [(int(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)),
             int(m.group(5)), int(m.group(6)), int(m.group(7)), int(m.group(8)),
             int(m.group(9)), int(m.group(10)), int(m.group(11)), int(m.group(12)))
            for m in (PACE_RE.search(l) for l in out.splitlines()) if m]
    # Start a few frames AFTER the load: the first racer row in a new level is a
    # legitimate discontinuity from the last row of the previous one (measured 7452
    # world units between the lobby and the Ancient Lake grid), and comparing across
    # it would read as a teleport.
    race_rows = [r for r in rows if race1[3] + LOAD_SKIP <= r[0] < (lobby2[3] if lobby2 else FRAMES)]

    # Returning from a race uses cutscene 100 to drive the kart out of the
    # corresponding lobby door. Completed object-mesh collision once let that
    # moving door carry the kart to the alcove ceiling before dropping it.
    # Bound the whole return cinematic, not just its eventual landing.
    return_lobby_rows = [
        r for r in rows
        if lobby2 and lobby2[3] < r[0] < (hub2[3] if hub2 else FRAMES)
    ]
    if not return_lobby_rows:
        failures.append("no racer positions were traced during the post-race lobby return")
    elif max(r[2] for r in return_lobby_rows) > MAX_RETURN_LOBBY_Y:
        peak = max(return_lobby_rows, key=lambda r: r[2])
        failures.append(
            f"post-race lobby return launched the kart to y={peak[2]:.1f} "
            f"at frame {peak[0]} (maximum {MAX_RETURN_LOBBY_Y:.1f})"
        )
    # MOTION is asserted on the RACING window only -- the load to the finish -- not
    # on everything before the level changes back. After `fin=1` DKR freezes the
    # race clock and repositions the racer for the results sequence, which produces
    # perfectly legitimate multi-frame position jumps (measured 53.1 and 80.7 world
    # units in consecutive frames at 12564/12565). Including them made MAX_STEP
    # read a podium placement as a teleport; excluding them is a scope fix, not a
    # widened tolerance -- MAX_STEP itself is untouched at 40.
    finish_rows = [r for r in race_rows if r[8] == 1]
    drive_rows = ([r for r in race_rows if r[0] <= finish_rows[0][0]]
                  if finish_rows else race_rows)
    if len(race_rows) < MIN_RACE_FRAMES:
        failures.append(f"only {len(race_rows)} in-race racer frames traced "
                        f"(want >= {MIN_RACE_FRAMES})")
    if race_rows:
        bad = [r for r in race_rows if not all(is_finite(v) for v in (r[1], r[2], r[3]))]
        if bad:
            failures.append(f"non-finite race position at frame {bad[0][0]}: "
                            f"({bad[0][1]}, {bad[0][2]}, {bad[0][3]}) — {len(bad)} frames")
        max_rlap = max(r[7] for r in race_rows)
        if max_rlap < MIN_RACE_RLAP:
            failures.append(f"only reached race lap {max_rlap} (want >= {MIN_RACE_RLAP}); "
                            f"update_player_racer's own lap= peaked at "
                            f"{max(r[6] for r in race_rows)}")
        finished = finish_rows
        if not finished:
            failures.append("the race never finished (fin= never went to 1)")
        else:
            clock = finished[0][4]
            if not (CLOCK_MIN <= clock <= CLOCK_MAX):
                failures.append(f"finish clock {clock} outside [{CLOCK_MIN}, {CLOCK_MAX}]")
            if finished[0][9] != 1:
                failures.append(f"win arm finished in place {finished[0][9]}, expected 1")
        bad_identity = [r for r in race_rows if r[10] != 0]
        if bad_identity:
            failures.append(f"race probe followed racerIndex={bad_identity[0][10]} at frame "
                            f"{bad_identity[0][0]}, expected controller port 1's racerIndex=0")
        rpath, rstep, rat = path_and_step(drive_rows)
        if rpath < MIN_RACE_PATH:
            failures.append(f"race path length only {rpath:.0f} world units "
                            f"(want >= {MIN_RACE_PATH:.0f})")
        if rstep > MAX_STEP:
            failures.append(f"teleport in the race: {rstep:.1f} world units in one frame at "
                            f"frame {rat} (limit {MAX_STEP})")

    # 7. Both directions of the Adventure result and its persisted reward.
    verdict_re = re.compile(
        r"adventureforceverdict: requested=(win|loss) natural=(\d+) "
        r"final=(\d+) lap=(\d+)"
    )
    win_hooks = [
        match for match in (verdict_re.search(line) for line in out.splitlines())
        if match
    ]
    loss_hooks = [
        match for match in (verdict_re.search(line) for line in loss_out.splitlines())
        if match
    ]
    if len(win_hooks) != 1:
        failures.append(
            f"win arm emitted {len(win_hooks)} adventure verdict traces, expected 1"
        )
    if len(loss_hooks) != 1:
        failures.append(
            f"loss arm emitted {len(loss_hooks)} adventure verdict traces, expected 1"
        )
    if win_hooks and (
        win_hooks[0].group(1) != "win" or int(win_hooks[0].group(3)) != 1
    ):
        failures.append(f"win verdict trace is invalid: {win_hooks[0].group(0)}")
    if loss_hooks and (
        loss_hooks[0].group(1) != "loss" or int(loss_hooks[0].group(3)) == 1
    ):
        failures.append(f"loss verdict trace is invalid: {loss_hooks[0].group(0)}")
    if win_hooks and loss_hooks and win_hooks[0].group(2) != loss_hooks[0].group(2):
        failures.append(
            "identical arms had different natural finish places before verdict control: "
            f"win={win_hooks[0].group(2)} loss={loss_hooks[0].group(2)}"
        )

    loss_levels = [
        (int(m.group(1)), int(m.group(3)), int(m.group(5)), int(m.group(6)))
        for m in (LEVEL_RE.search(line) for line in loss_out.splitlines()) if m
    ]
    loss_race = next((entry for entry in loss_levels if entry[0] == RACE), None)
    loss_lobby = (next((entry for entry in loss_levels
                        if loss_race and entry[0] == LOBBY and entry[3] > loss_race[3]), None))
    loss_hub = (next((entry for entry in loss_levels
                      if loss_lobby and entry[0] == HUB and entry[3] > loss_lobby[3]), None))
    if loss_race is None or loss_lobby is None or loss_hub is None:
        failures.append("loss control did not complete race -> lobby -> hub: "
                        f"{[(entry[0], entry[3]) for entry in loss_levels]}")

    loss_rows_all = [
        (int(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)),
         int(m.group(5)), int(m.group(6)), int(m.group(7)), int(m.group(8)),
         int(m.group(9)), int(m.group(10)), int(m.group(11)), int(m.group(12)))
        for m in (PACE_RE.search(line) for line in loss_out.splitlines()) if m
    ]
    loss_race_rows = [
        row for row in loss_rows_all
        if loss_race and loss_race[3] + LOAD_SKIP <= row[0] <
        (loss_lobby[3] if loss_lobby else FRAMES)
    ]
    loss_finished = [row for row in loss_race_rows if row[8] == 1]
    if not loss_finished:
        failures.append("loss control never naturally finished the three-lap race")
    else:
        loss_finish = loss_finished[0]
        if loss_finish[7] < MIN_RACE_RLAP:
            failures.append(f"loss control finished at lap {loss_finish[7]}, "
                            f"expected >= {MIN_RACE_RLAP}")
        if loss_finish[9] == 1:
            failures.append("loss verdict control left the human in first place")
        if loss_finish[10] != 0:
            failures.append(f"loss control finish followed racerIndex={loss_finish[10]}, expected 0")
        if finish_rows:
            win_finish = finish_rows[0]
            if (win_finish[0], win_finish[4], win_finish[7]) != (
                    loss_finish[0], loss_finish[4], loss_finish[7]):
                failures.append(
                    "win control changed the natural finish timing: "
                    f"win frame/clock/lap={(win_finish[0], win_finish[4], win_finish[7])}, "
                    f"loss={(loss_finish[0], loss_finish[4], loss_finish[7])}")

    decoded: dict[str, dict[str, object]] = {}
    for name, save in (("win", win_save), ("loss", loss_save)):
        try:
            decoded[name] = decode_progress(save, args.rom)
        except (OSError, ValueError, IndexError, struct.error) as exc:
            failures.append(f"{name} arm EEPROM decode failed: {exc}")
            continue
        state = decoded[name]
        if not state["checksum_ok"]:
            failures.append(
                f"{name} arm EEPROM checksum 0x{state['stored_checksum']:04x}, "
                f"expected 0x{state['expected_checksum']:04x}")

    if "win" in decoded:
        status = decoded["win"]["course_status"]
        saved_balloons = decoded["win"]["balloons"]
        if status != 2:
            failures.append(f"win arm persisted Ancient Lake status {status}, expected 2 (cleared)")
        if saved_balloons[:2] != (2, 1) or any(saved_balloons[2:]):
            failures.append(f"win arm persisted balloon counts {saved_balloons}, "
                            "expected (2, 1, 0, 0, 0, 0)")
    if "loss" in decoded:
        status = decoded["loss"]["course_status"]
        saved_balloons = decoded["loss"]["balloons"]
        if status != 1:
            failures.append(f"loss arm persisted Ancient Lake status {status}, expected 1 (visited only)")
        if saved_balloons[:2] != (1, 0) or any(saved_balloons[2:]):
            failures.append(f"loss arm persisted balloon counts {saved_balloons}, "
                            "expected (1, 0, 0, 0, 0, 0)")

    # 4. alive and driving back in the hub
    hub_back = [r for r in rows if hub2 and r[0] >= hub2[3] + LOAD_SKIP]
    if hub2 is not None:
        if len(hub_back) < MIN_HUB_BACK_FRAMES:
            failures.append(f"only {len(hub_back)} racer frames after returning to the hub "
                            f"(want >= {MIN_HUB_BACK_FRAMES}) — the kart may not be controllable")
        hpath, hstep, hat = path_and_step(hub_back)
        if hpath < MIN_HUB_BACK_PATH:
            failures.append(f"the kart barely moved after returning to the hub: {hpath:.0f} "
                            f"world units (want >= {MIN_HUB_BACK_PATH:.0f})")
        if hstep > MAX_STEP:
            failures.append(f"teleport back in the hub: {hstep:.1f} world units in one frame "
                            f"at frame {hat} (limit {MAX_STEP})")

    # 5. the scene still draws
    def frame_no(name: str) -> int:
        m = re.search(r"(\d+)", name)
        return int(m.group(1)) if m else -1
    samples = sorted((f for f in os.listdir(frame_dir) if f.endswith(".ppm")), key=frame_no)
    if not samples:
        failures.append("no frames were dumped — cannot check that the scene draws")
    scored = [(n,) + scene_metrics(os.path.join(frame_dir, n)) for n in samples]
    weak = [s for s in scored if s[1] < MIN_DISTINCT_COLOURS or s[2] < MIN_LUMA_SIGMA]
    if len(weak) > MAX_BAD_SAMPLES:
        failures.append(f"{len(weak)} of {len(scored)} sampled frames do not contain a real scene "
                        f"(allowed {MAX_BAD_SAMPLES}): " +
                        ", ".join(f"{n} {c} colours / sigma {s:.1f}" for n, c, s in weak))
    if scored and (scored[-1][1] < MIN_DISTINCT_COLOURS or scored[-1][2] < MIN_LUMA_SIGMA):
        failures.append(f"the LAST sampled frame does not contain a real scene: "
                        f"{scored[-1][0]} {scored[-1][1]} colours / sigma {scored[-1][2]:.1f}")

    if args.verbose:
        print(f"  level loads:      {seq}")
        print(f"  hub -> lobby:     {hub1[3]} -> {lobby1[3]}")
        print(f"  lobby -> race:    {lobby1[3]} -> {race1[3]}")
        if lobby2:
            print(f"  race -> lobby:    {race1[3]} -> {lobby2[3]} (entrance {lobby2[1]})")
        if hub2:
            print(f"  lobby -> hub:     {lobby2[3]} -> {hub2[3]} (entrance {hub2[1]})")
        print(f"  balloons:         {balloons}")
        print(f"  exits taken:      {[(e[0], e[1], e[3]) for e in exits]}")
        if race_rows:
            rpath, rstep, rat = path_and_step(drive_rows)
            fin = [r for r in race_rows if r[8] == 1]
            print(f"  in-race frames:   {len(race_rows)}  path {rpath:.0f}  max step {rstep:.1f}")
            print(f"  race lap max:     rlap {max(r[7] for r in race_rows)} "
                  f"finish clock {fin[0][4] if fin else 'n/a'} @frame {fin[0][0] if fin else 'n/a'} "
                  f"place {fin[0][9] if fin else 'n/a'}")
        if loss_finished:
            print(f"  loss control:     finish clock {loss_finished[0][4]} "
                  f"@frame {loss_finished[0][0]} place {loss_finished[0][9]}")
        if decoded:
            print(f"  persisted state:  {decoded}")
        if hub_back:
            hpath, hstep, hat = path_and_step(hub_back)
            print(f"  hub-again frames: {len(hub_back)}  path {hpath:.0f}  max step {hstep:.1f}")
            print(f"  last position:    ({hub_back[-1][1]:.1f}, {hub_back[-1][2]:.1f}, "
                  f"{hub_back[-1][3]:.1f})")
        for n, c, s in scored:
            print(f"  frame {n}: {c} colours / sigma {s:.1f}")

    print_result(failures)
    return 1 if failures else 0


def print_result(failures: list[str]) -> None:
    if failures:
        print("FAIL: adventure race loop check")
        for f in failures:
            print("  - " + f)
    else:
        print("PASS: adventure race loop check")


if __name__ == "__main__":
    sys.exit(main())
