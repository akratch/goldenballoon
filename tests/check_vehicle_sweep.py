#!/usr/bin/env python3
"""Drive every playable track in every vehicle that track actually permits.

Why this exists
---------------
`tests/check_track_sweep.py` sweeps all 20 playable tracks -- but with NO
`--vehicle` it sweeps them all in the **car**, and that is not obvious. The
`MDKR_LOAD_TRACK=<level>[:<vehicle>]` hook rewrites the level id at `level_load()`,
which happens long after the menus have already chosen the vehicle from
`leveltable_vehicle_default(gTrackIdForPreview)` -- and the previewed track is
always Ancient Lake, whose default is the car. So every race this port had ever
validated, on every track, was a car race. Diddy Kong Racing has three player
vehicles with genuinely separate physics modules (`update_player_racer()`'s
`switch (vehicleID)` -> `func_8004F7F4` / `func_80046524` / `func_80049794`), and
several tracks cannot be raced in a car at all.

That gap was real: the very first plane race ever run aborted in
`__stack_chk_fail` inside `func_80049794` -- `f32 sp60[4]` holding the 4x4 MtxF
that `mtxf_from_transform()` writes, a 48-byte stack overflow, exit 134 with
nothing at all on stderr. See the comment at that declaration in racer.c.

Which combinations are legitimate
---------------------------------
Not all 60. `LevelHeader.available_vehicles` (ROM offset 0x4E) is a bitmask of the
vehicles the track-select menu will offer, and `LevelHeader.vehicle` (0x4D) is the
one it defaults to; `level_global_init()` packs them into
`gGlobalLevelTable[i].vehicles` as `(available << 4) | (default & 0xF)`, which
`leveltable_vehicle_usable()` / `leveltable_vehicle_default()` read back.
Forcing a plane onto a hovercraft-only track is not a bug, it is a meaningless
input, so this check sweeps exactly the mask -- 47 combinations.

The mask is decoded here straight from the ROM, INDEPENDENTLY of any port code
(same approach as tools/dump_misc_asset.py), and then cross-checked against the
game's own reading, published by `level_global_init()` as
`[TRACE] level_vehicles: id=N default=D avail=0xM`. If the header offsets, the
`<<4` packing or the asset-table lookup ever drift, the two disagree and this
check fails instead of quietly sweeping the wrong matrix.

Two tracks narrow the mask further, but only for two-or-more players
(`menu.c`, `VERSION >= VERSION_79`): Spaceport Alpha (15) drops the hovercraft and
Frosty Village (28) drops the plane. This is a one-player sweep, so both are swept
in all three vehicles; `--two-player-mask` applies the 2P narrowing instead.

What it asserts, per combination
--------------------------------
  1. the process exits 0 (no crash, no abort)
  2. the intended level loaded as a race **with the requested vehicle**
  3. the racer really was updated by that vehicle module -- `[PVEH]` reports the
     `vehicleID` `update_player_racer()` dispatches on, so a silently-ignored
     override cannot pass 47 rows by driving a car every time. `VEHICLE_LOOPDELOOP`
     is additionally permitted mid-race, and marked `+loopdeloop` in the report: a
     per-track trigger object swaps the racer into the loop physics module over
     corkscrew sections and restores `vehicleIDPrev` after. Walrus Cove (6) and
     DarkMoon Caverns (32) do it, which is how that module got covered at all.
  4. the racer produced position samples, all finite
  5. real forward progress (`courseCheckpoint` climbed) and real displacement
  6. a plane must actually use the vertical axis -- `y` has to move, so "loads,
     'drives', and sits at one altitude" is a failure and not a pass
  7. no [FATAL] / sanitizer / assertion text appeared
  8. per track, the vehicles must produce DISTINCT trajectories -- two vehicles
     tracing the same path means the override did nothing

Usage:
    tests/check_vehicle_sweep.py                 # the full legitimate matrix (47)
    tests/check_vehicle_sweep.py --vehicles 1,2  # hovercraft + plane rows only
    tests/check_vehicle_sweep.py --tracks 6,15
    tests/check_vehicle_sweep.py --matrix        # print the matrix and exit
    tests/check_vehicle_sweep.py --expect-fail 15:1

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), per
tests/README.md. Exit 0 = every combination passed (or failed only as expected).
"""
import argparse
import os
import re
import struct
import subprocess
import sys

from harness_utils import resolve_binary

# ASSET_MISC_MAIN_TRACKS_IDS from the US v80 ROM, in menu order (see
# tests/check_track_sweep.py, which decodes it with tools/dump_misc_asset.py).
MAIN_TRACK_IDS = [5, 3, 29, 7, 8, 4, 10, 30, 13, 6, 9, 28, 19, 18, 20, 31, 17, 32, 33, 15]

VEHICLE_NAMES = {0: "car", 1: "hovercraft", 2: "plane"}
VEHICLE_LOOPDELOOP = 4   # game/include/enums.h; a mid-race transient, see run_combo()

# 2-player-only narrowing in menu_track_select (game/src/menu.c, VERSION >= 79).
TWO_PLAYER_EXCLUDE = {15: 1, 28: 2}   # Spaceport Alpha: no hovercraft; Frosty Village: no plane

# --- independent ROM decode of the level headers -----------------------------
# Mirrors game/src/asset_loading.c asset_table_load()/asset_load(): the asset LUT
# is a big-endian u32 array at 0xED0E0, LUT[0] is the section count and section S
# spans [LUT[S+1], LUT[S+2]) relative to the asset-data base 0xED1B0. Unlike
# ASSET_MISC_TABLE (word offsets), the level-header table holds BYTE offsets --
# asset_load() adds them to the section start directly.
LUT_START = 0x000ED0E0
DATA_BASE = 0x000ED1B0
ASSET_LEVEL_HEADERS_TABLE = 22
ASSET_LEVEL_HEADERS = 23
LEVELHEADER_VEHICLE = 0x4D          # s8 LevelHeader.vehicle
LEVELHEADER_AVAILABLE_VEHICLES = 0x4E   # s8 LevelHeader.available_vehicles

PACE_RE = re.compile(r"racer x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(\d+)")
PVEH_RE = re.compile(r"\[PVEH\] frame=\d+ player=0 vehicleID=(-?\d+)")
BAD_RE = re.compile(
    r"\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion|\[FX BUG\]"
)
GAME_VEH_RE = re.compile(r"level_vehicles: id=(\d+) default=(\d+) avail=0x([0-9a-fA-F]+)")


def rom_section(rom, idx):
    start = struct.unpack_from(">I", rom, LUT_START + 4 * (idx + 1))[0]
    end = struct.unpack_from(">I", rom, LUT_START + 4 * (idx + 2))[0]
    return rom[DATA_BASE + start:DATA_BASE + end]


def rom_vehicle_matrix(rom_path):
    """{levelId: (defaultVehicle, availableMask)} decoded from the ROM alone."""
    rom = open(rom_path, "rb").read()
    raw = rom_section(rom, ASSET_LEVEL_HEADERS_TABLE)
    table = list(struct.unpack(">%di" % (len(raw) // 4), raw))
    count = table.index(-1) if -1 in table else len(table)
    headers = rom_section(rom, ASSET_LEVEL_HEADERS)
    out = {}
    for i in range(count - 1):          # last entry is the end offset, not a level
        base = table[i]
        default = headers[base + LEVELHEADER_VEHICLE]
        mask = headers[base + LEVELHEADER_AVAILABLE_VEHICLES]
        out[i] = (default & 0xF, mask & 0xF)
    return out


def game_vehicle_matrix(build, rom, timeout):
    """The same thing as the GAME reads it, from level_global_init()'s trace."""
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"
    env["MDKR_TRACE"] = "1"
    cmd = [build, "--headless-frames", "30", "--rom", rom]
    proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    out = proc.stdout.decode("utf-8", "replace")
    return {int(m[0]): (int(m[1]), int(m[2], 16)) for m in GAME_VEH_RE.findall(out)}


def run_combo(build, rom, script, level, vehicle, frames, timeout):
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"          # belt-and-braces; --headless-frames is the guarantee
    env["MDKR_AUTOPILOT"] = "1"      # drive with DKR's own AI -- no per-track input tuning
    env["MDKR_TRACE"] = "1"          # emit the [PACE] / [PVEH] probes
    env["MDKR_LOAD_TRACK"] = "%d:%d" % (level, vehicle)
    cmd = [build, "--headless-frames", str(frames), "--input-script", script, "--rom", rom]
    try:
        proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
        out = proc.stdout.decode("utf-8", "replace")
        rc = proc.returncode
    except subprocess.TimeoutExpired as e:
        return {"rc": "timeout", "why": "timed out after %ss" % timeout, "sig": None,
                "out": (e.stdout or b"").decode("utf-8", "replace")}

    samples = PACE_RE.findall(out)
    bad = BAD_RE.findall(out)
    # The race load, not the track-select preview: the preview passes
    # numberOfPlayers=-1 and a non-zero cutsceneId, and is deliberately left alone
    # by the MDKR_LOAD_TRACK hook.
    load_re = re.compile(r"level_load: levelId=%d numPlayers=0 entrance=\d+ vehicle=(\d+) cutscene=0" % level)
    loads = load_re.findall(out)
    dispatched = PVEH_RE.findall(out)

    why = []
    if rc != 0:
        why.append("exited %d%s" % (rc, " (signal %d)" % -rc if rc < 0 else ""))
    if not loads:
        why.append("level %d never loaded as a race" % level)
    elif int(loads[-1]) != vehicle:
        why.append("loaded with vehicle %s, wanted %d" % (loads[-1], vehicle))
    # The racer must START in the requested vehicle, and may only ever be in the
    # requested vehicle or VEHICLE_LOOPDELOOP. The latter is not slack: DKR tracks
    # place a vehicle-change trigger object (object_functions.c, the
    # `racer->vehicleID = trigger->vehicleID` branch) around corkscrew/loop sections,
    # which swaps the racer into the loop-de-loop physics module for the duration and
    # restores `vehicleIDPrev` at the exit trigger. Walrus Cove (6) and DarkMoon
    # Caverns (32) do exactly that, in both car and hovercraft. Requiring the FIRST
    # dispatch to equal the request keeps this falsifiable: an override that was
    # silently ignored would report vehicleID 0 first on every row.
    seen = [int(v) for v in dispatched]
    stray = sorted(set(seen) - {vehicle, VEHICLE_LOOPDELOOP})
    if not dispatched:
        why.append("no [PVEH] -- update_player_racer never dispatched a human racer")
    elif seen[0] != vehicle:
        why.append("racer started as vehicleID %d, wanted %d" % (seen[0], vehicle))
    elif stray:
        why.append("racer was updated as unexpected vehicleID %s (wanted %d, or %d in a loop)"
                   % (stray, vehicle, VEHICLE_LOOPDELOOP))
    if not samples:
        why.append("no racer samples")
    if bad:
        why.append("%d bad-log line(s): %s" % (len(bad), bad[0]))

    maxcp = 0
    nonfinite = 0
    yspan = 0.0
    span = 0.0
    sig = None
    if samples:
        maxcp = max(int(s[4]) for s in samples)
        for s in samples:
            if any(t in (s[0] + s[1] + s[2]).lower() for t in ("inf", "nan")):
                nonfinite += 1
        if not nonfinite:
            xs = [float(s[0]) for s in samples]
            ys = [float(s[1]) for s in samples]
            zs = [float(s[2]) for s in samples]
            yspan = max(ys) - min(ys)
            span = max(max(xs) - min(xs), max(zs) - min(zs))
            # Trajectory signature: the full position stream, so two vehicles that
            # trace the same path (i.e. the override did nothing) collide here.
            sig = hash(tuple((s[0], s[1], s[2]) for s in samples))
        if maxcp < 3:
            why.append("no forward progress (max courseCheckpoint %d)" % maxcp)
        if nonfinite:
            why.append("%d non-finite position sample(s)" % nonfinite)
        elif span < MIN_SPAN:
            why.append("racer barely moved (%.1f world units of spread, need %.0f)" % (span, MIN_SPAN))
        # A plane that never changes altitude is a defect even though nothing
        # crashes: the pitch axis is a whole input path the car/hovercraft never
        # touch, and MDKR_AUTOPILOT does drive it (func_80045C48 derives
        # gCurrentStickY from the spline's vertical derivative). Measured on us.v80:
        # the 11 plane rows span 331..1235 world units of y travel, so the 60-unit
        # threshold has 5x of headroom; the flattest row of any vehicle is 76
        # (Whale Bay, hovercraft) and the flattest car row is 210.
        if not nonfinite and vehicle == 2 and yspan < MIN_PLANE_YSPAN:
            why.append("plane never changed altitude (y spread %.1f, need %.0f)"
                       % (yspan, MIN_PLANE_YSPAN))

    return {"rc": rc, "why": "; ".join(why), "samples": len(samples), "maxcp": maxcp,
            "nonfinite": nonfinite, "yspan": yspan, "span": span, "sig": sig, "out": out,
            "loop": VEHICLE_LOOPDELOOP in seen}


MIN_SPAN = 200.0            # world units of x-or-z spread over the whole race
MIN_PLANE_YSPAN = 60.0      # world units of altitude travel required of a plane


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--script", default="tests/input_scripts/race_full_3lap_tt.txt")
    ap.add_argument("--frames", type=int, default=5200)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--tracks", default=None, help="comma-separated level ids (default: all 20)")
    ap.add_argument("--vehicles", default=None, help="comma-separated vehicle ids to include")
    ap.add_argument("--two-player-mask", action="store_true",
                    help="apply menu.c's 2-player narrowing (no hovercraft on 15, no plane on 28)")
    ap.add_argument("--matrix", action="store_true", help="print the swept matrix and exit 0")
    ap.add_argument("--expect-fail", default="",
                    help="comma-separated level:vehicle known to fail; reported but tolerated")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    args.build = resolve_binary(args.build)

    for path in (args.build, args.rom, args.script):
        if not os.path.exists(path):
            sys.exit("missing: %s" % path)

    rom_matrix = rom_vehicle_matrix(args.rom)

    tracks = [int(t) for t in args.tracks.split(",")] if args.tracks else MAIN_TRACK_IDS
    want_vehicles = ({int(v) for v in args.vehicles.split(",")} if args.vehicles
                     else set(VEHICLE_NAMES))
    expected_fail = set()
    for tok in args.expect_fail.split(","):
        if tok.strip():
            lvl, veh = tok.split(":")
            expected_fail.add((int(lvl), int(veh)))

    combos = []
    for level in tracks:
        if level not in rom_matrix:
            sys.exit("level %d is not in the ROM's level-header table" % level)
        _default, mask = rom_matrix[level]
        for veh in sorted(VEHICLE_NAMES):
            if not (mask >> veh) & 1:
                continue
            if args.two_player_mask and TWO_PLAYER_EXCLUDE.get(level) == veh:
                continue
            if veh in want_vehicles:
                combos.append((level, veh))

    if args.matrix:
        print("legitimate (track, vehicle) matrix from LevelHeader.available_vehicles:")
        for level in tracks:
            d, m = rom_matrix[level]
            allowed = [VEHICLE_NAMES[v] for v in sorted(VEHICLE_NAMES) if (m >> v) & 1]
            print("  level %-3d default=%-11s avail=0x%x  %s"
                  % (level, VEHICLE_NAMES.get(d, str(d)), m, ", ".join(allowed)))
        print("%d combination(s)" % len(combos))
        return 0

    # Cross-check the ROM decode against the game's own reading before trusting it.
    print("check_vehicle_sweep: cross-checking the ROM level-header decode against "
          "the game's gGlobalLevelTable...")
    game_matrix = game_vehicle_matrix(args.build, args.rom, args.timeout)
    if not game_matrix:
        print("check_vehicle_sweep: FAIL - the game emitted no 'level_vehicles:' trace "
              "lines, so the matrix could not be cross-checked")
        return 1
    mismatch = [(lvl, rom_matrix.get(lvl), game_matrix.get(lvl))
                for lvl in sorted(set(tracks))
                if rom_matrix.get(lvl) != game_matrix.get(lvl)]
    if mismatch:
        print("check_vehicle_sweep: FAIL - ROM decode disagrees with the game for "
              "%d level(s):" % len(mismatch))
        for lvl, r, g in mismatch:
            print("    level %d: rom=%s game=%s" % (lvl, r, g))
        return 1
    print("  agree on all %d swept track(s) (%d levels traced)"
          % (len(set(tracks)), len(game_matrix)))

    print("check_vehicle_sweep: %d combination(s) over %d track(s), %d frames each, autopilot"
          % (len(combos), len(tracks), args.frames))

    unexpected = []
    sigs = {}
    for level, veh in combos:
        r = run_combo(args.build, args.rom, args.script, level, veh, args.frames, args.timeout)
        why = r["why"]
        if r["sig"] is not None:
            prev = sigs.get((level, r["sig"]))
            if prev is not None:
                why = ((why + "; ") if why else "") + (
                    "identical trajectory to vehicle %d on this track - the vehicle "
                    "override did nothing" % prev)
            sigs[(level, r["sig"])] = veh
        ok = not why
        tag = "ok" if ok else "FAIL"
        if not ok and (level, veh) in expected_fail:
            tag = "FAIL (expected)"
        elif not ok:
            unexpected.append((level, veh))
        detail = ("samples=%d maxcp=%-3d spread=%-7.0f yspread=%-6.0f%s"
                  % (r["samples"], r["maxcp"], r["span"], r["yspan"],
                     " +loopdeloop" if r.get("loop") else "")) if ok else why
        print("  level %-3d %-11s %-15s %s" % (level, VEHICLE_NAMES[veh], tag, detail))
        # A failed child is evidence, not optional verbosity. Keep enough of its
        # tail to retain sanitizer summaries/stacks; -v widens the excerpt for
        # investigation without flooding ordinary successful matrix output.
        if not ok:
            excerpt_lines = 120 if args.verbose else 40
            for line in r["out"].splitlines()[-excerpt_lines:]:
                print("      | %s" % line)

    if unexpected:
        print("check_vehicle_sweep: FAIL - %d combination(s) broke unexpectedly: %s"
              % (len(unexpected), ", ".join("%d:%d" % c for c in unexpected)))
        return 1
    print("check_vehicle_sweep: PASS (%d combinations)" % len(combos))
    return 0


if __name__ == "__main__":
    sys.exit(main())
