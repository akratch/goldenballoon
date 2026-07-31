#!/usr/bin/env python3
"""P3.5 check: entering ADVENTURE and driving Timber's Island must work.

Why this exists
---------------
Adventure is the mode most players spend their time in, and until P3.5 nothing
had ever driven it.  Getting from FILE SELECT to a controllable kart on the hub
crosses three things no other fixture touches, and each one was broken:

  1. `set_current_text()` overlays `GameTextTableStruct.entries[]` directly on
     ASSET_GAME_TEXT_TABLE bytes.  Those entries are 32-bit ROM words, but the
     decomp declares them `char *` — 8 bytes on LP64.  The stride was therefore
     doubled, `size` came out as garbage (measured 65536), and the follow-up
     `asset_load(ASSET_GAME_TEXT, ...)` DMA'd 64 KB over live arena objects.  The
     first *symptom* was a SIGSEGV in `free_3d_model()` while tearing the intro
     cutscene down, ~600 frames after the corrupting write.  Reached by ordinary
     play twice over: the intro cutscene's dialogue, and every hub door's
     balloon-requirement textbox (`obj_loop_door`).
  2. `ParticleBehaviour` ends in a pointer, so `sizeof` grew 0xA0 -> 0xA8 and
     `colourLoop` moved 0x9C -> 0xA0 while the on-disk record stride stayed 0xA0.
     Every colour-looping emitter read the next record's floats as a pointer:
     crash in `create_general_particle()` ~1280 frames into the hub.
  3. `func_80026E54()` (void/skybox segment sorting) writes TWO entries per
     segment into an array declared for one, so with the nine void segments the
     hub can put in view at once it overran `sp94[10]` by eight elements and
     aborted in `__stack_chk_fail`.

All three are silent-until-fatal and none is reachable from any pre-P3.5
fixture, so this check exists to keep the route covered.

What this asserts
-----------------
  1. **The route arrives.**  The menu trace must show GAME_SELECT(19) ->
     FILE_SELECT(6) -> MENU_NEWGAME_CINEMATIC(23), the cutscene level must load
     (levelId 36, cutsceneId 15) *and must be left again*, and
     ASSET_LEVEL_CENTRALAREAHUB (levelId 0) must load.  Asserting the cutscene is
     left matters: bug 1 above showed up exactly at that teardown.
  2. **There is a real, controllable racer for >= 1000 frames.**  [PACE] racer
     rows after the hub load, positions all finite, y inside a generous band, no
     single-frame teleport, and the race clock advancing.
  3. **It actually drives.**  Total path length and the bounding box of the
     visited positions must both be large, and the closed-loop tour must actually
     REACH its named waypoints — position alone cannot tell driving from
     vibrating against a wall, and a waypoint on the far side of some terrain is
     a stronger statement than a spline counter (see HUB_TOUR_ROUTE).
  4. **The scene still draws.**  Sampled frames must contain a real rendered
     scene (distinct 5-bit colours + luma sigma over a centre crop), reusing
     check_race_drive.py's metric and thresholds.  One sample is allowed to fail:
     the route brushes a hub door, whose fade briefly whites the screen out (a
     ~30-frame dip measured at 209 colours / sigma 7.9 around frame 6600), and
     that is DKR's own transition, not a render fault.
  5. **The process exits 0.**  Two of the three bugs above aborted with nothing
     at all on stderr, exactly like the `timetrial_ghost_read` overflow that an
     earlier version of check_race_finish_time.py silently passed.

Usage:
    tests/check_adventure_hub.py [--build build] [--rom baserom.us.v80.z64]
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
import subprocess
import sys
import tempfile

from harness_utils import resolve_binary

SCRIPT = "tests/input_scripts/adventure_hub_drive.txt"
FRAMES = 12000
HUB_LEVEL_ID = 0            # ASSET_LEVEL_CENTRALAREAHUB
CUTSCENE_LEVEL_ID = 36      # the new-game intro cutscene level

# CLOSED-LOOP HUB TOUR ("closedloop" wave).
#
# The script's own driving inputs -- throttle held from frame 6000, stick RIGHT
# for a few frames every 200 -- are a blind open-loop route, and they stopped
# working the moment the ROM's RNG seed / arctan table / table-based trig became
# the defaults: the drifted line put the kart inside a hub EXIT's activation
# radius, so instead of touring Timber's Island it entered the Dino Domain lobby
# at frame 6883 and then boss level 38 at 7336. Every in-hub assertion below then
# failed for a reason that has nothing to do with the hub -- cp 2, lap 0, and a
# 4367-unit "teleport" that was really the level change.
#
# These 44 waypoints are SAMPLED FROM THAT SAME ROUTE'S OWN [PACE] TRACE, one
# every 1000 world units of arc, so the tour covers the ground the open-loop
# fixture is documented to cover -- including the stretch that puts nine void
# segments in view at once and caught func_80026E54's sp94[10] overrun. What
# changes is only that the kart now STEERS AT each point
# (platform/mdkr_adventure.c) instead of hoping a fixed pulse train still lands
# there.
#
# SPACING IS LOAD-BEARING, and two runs were spent learning it. The hook drives
# straight at the next waypoint, so the CHORD between two of them has to be
# drivable, not just the sampled positions:
#   - 4500-unit spacing (9 points): a chord passed 158 units from Taj
#     (BHV_PARK_WARDEN at (-197, 255, 193)), whose dialogue opens on contact
#     within 300 units and then calls disable_racer_input() every frame. The kart
#     travelled 224 units in 6133 frames.
#   - 2200-unit spacing (20 points): the chord (-1180, 691) -> (890, 921) cuts
#     across the island's middle and the kart wedged at (79, 243, 766) with
#     velocity 0 for 5000 frames.
# At 1000 the polyline hugs the traced line. The first leg is hand-placed at
# (200, 500) -- the same north-east opening check_adventure_race_loop.py has
# always used -- because the sampled points start 1000 units along and a straight
# line from the spawn to the first of them would run over Taj.
#
# Once the tour is exhausted the hook hands the kart back and MDKR_AUTOPILOT
# drives the tail: the hub's AI-node graph is a closed loop on the central beach,
# so the tail can lap for ever but can never turn in at a door.
HUB_TOUR_ROUTE = (
    "0:200,500:-550,830:-1530,1032:-2248,1385:-3157,1820:-3948,2180:-3448,1970"
    ":-2495,1654:-1796,1188:-1009,582:-11,693:896,928:1534,1693:897,2258"
    ":386,1556:210,623:660,-273:1255,-552:1860,164:1679,1150:857,1646:-144,1515"
    ":-1123,1398:-1676,706:-1773,-89:-1031,-754:-130,-929:865,-803:1267,86"
    ":1517,1057:964,1826:-25,1664:-782,1115:-1392,322:-1636,-545:-1188,-1422"
    ":-290,-1706:672,-1420:1664,-1260:1484,-759:1322,6:580,672:-249,1075"
    ":-1249,1011")

# --- thresholds -------------------------------------------------------------
# All "measured" numbers below come from the current build on US v80; the routes
# are deterministic (tests/check_determinism.py), so they are stable.
HUB_LOAD_MAX_FRAME = 7000   # measured ~5867; a big margin, but it must not hang
MIN_HUB_FRAMES = 1000       # the P3.5 definition of done
MAX_STEP = 40.0             # world units in one frame (same bound as the race check)
Y_MIN, Y_MAX = -400.0, 1200.0   # Timber's Island: measured -35 .. 539
MIN_PATH_LENGTH = 4000.0    # measured ~29000 over the drive
MIN_BBOX_SPAN = 2000.0      # measured ~4800 (x) / ~6700 (z)
# Waypoints of HUB_TOUR_ROUTE that must actually be reached. Replaces the old
# MIN_FINAL_CP = 5 / MIN_FINAL_LAP = 1 (which measured the AI-node spline the tour
# does not follow). See the assertion below for why this is the stronger test.
#
# Measured 26 of 44 inside the 12000-frame budget -- the tour is deliberately
# longer than the budget so that a line which gets faster still has somewhere to
# go, and 26 already covers a 5938 x 3472 box, i.e. the whole island. The two
# spacings that failed reached 5 and 0.
MIN_WAYPOINTS_REACHED = 20
MIN_DISTINCT_COLOURS = 200  # check_race_drive.py's calibration: broken 59
MIN_LUMA_SIGMA = 12.0       # check_race_drive.py's calibration: broken 5.9
MAX_BAD_SAMPLES = 1         # the door-fade dip; see the docstring
SAMPLE_EVERY = 700

PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(-?\d+) lap=(-?\d+)")
MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
LEVEL_RE = re.compile(r"level_load: levelId=(\d+) numPlayers=(-?\d+).*cutscene=(-?\d+) @frame~(\d+)")
# platform/mdkr_adventure.c emits one of these each time a waypoint completes.
WAYPOINT_RE = re.compile(r"drive: level=0 step (\d+): waypoint \((-?\d+), (-?\d+)\) reached")


def load_scene_metrics():
    """Reuse check_race_drive.py's scene metric so both checks agree by construction."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_race_drive.py")
    spec = importlib.util.spec_from_file_location("mdkr_check_race_drive", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.scene_metrics


def is_finite(v: float) -> bool:
    return v == v and -1e30 < v < 1e30


def print_result(failures: list[str]) -> None:
    if failures:
        print("FAIL: adventure hub check")
        for f in failures:
            print("  - " + f)
    else:
        print("PASS: adventure hub check")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--keep-frames", default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    scene_metrics = load_scene_metrics()
    frame_dir = args.keep_frames or tempfile.mkdtemp(prefix="mdkr_hub_")
    os.makedirs(frame_dir, exist_ok=True)

    env = dict(os.environ,
               MDKR_AUDIO="0",          # belt-and-braces; --headless-frames is the guarantee
               MDKR_SIMULATION_CADENCE="enhanced",
               MDKR_SYNTH_FIELDS="1",
               MDKR_TRACE="1",
               MDKR_SAVE_DIR=os.path.join(frame_dir, "save"),
               MDKR_DRIVE_ROUTE=HUB_TOUR_ROUTE,   # closed loop -- see the docstring
               MDKR_AUTOPILOT="1",                # drives the tail once the tour ends
               MDKR_DUMP_FROM=str(HUB_LOAD_MAX_FRAME),
               MDKR_DUMP_EVERY=str(SAMPLE_EVERY))
    cmd = [binary, "--headless-frames", str(FRAMES),
           "--input-script", SCRIPT, "--dump-frames", frame_dir, "--rom", args.rom]
    if args.verbose:
        print("$ " + " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    out = proc.stdout + proc.stderr

    failures: list[str] = []

    # 5. exit code / crash markers first — two of the three P3.5 bugs printed nothing.
    if proc.returncode != 0:
        failures.append(f"exit code {proc.returncode} "
                        f"(negative = killed by signal {-proc.returncode})")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            line = next((l for l in out.splitlines() if marker in l), marker)
            failures.append(f"{marker} in output: {line.strip()}")

    # 1. the route arrives
    menus = [(int(m.group(1)), int(m.group(2))) for m in
             (MENU_RE.search(l) for l in out.splitlines()) if m]
    levels = [(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))) for m in
              (LEVEL_RE.search(l) for l in out.splitlines()) if m]
    menu_ids = [m[0] for m in menus]
    for want, name in ((19, "GAME_SELECT"), (6, "FILE_SELECT"), (23, "MENU_NEWGAME_CINEMATIC")):
        if want not in menu_ids:
            failures.append(f"never reached menuId={want} ({name}); saw {menu_ids}")

    cutscene_loads = [l for l in levels if l[0] == CUTSCENE_LEVEL_ID]
    if not cutscene_loads:
        failures.append(f"the intro cutscene level (levelId={CUTSCENE_LEVEL_ID}) never loaded")
    hub_loads = [l for l in levels if l[0] == HUB_LEVEL_ID]
    if not hub_loads:
        failures.append(f"ASSET_LEVEL_CENTRALAREAHUB (levelId={HUB_LEVEL_ID}) never loaded — "
                        f"level loads seen: {[(l[0], l[3]) for l in levels]}")
        print_result(failures)
        return 1
    hub_frame = hub_loads[0][3]
    if hub_frame > HUB_LOAD_MAX_FRAME:
        failures.append(f"hub loaded at frame {hub_frame} (expected <= {HUB_LOAD_MAX_FRAME})")
    if cutscene_loads and cutscene_loads[0][3] >= hub_frame:
        failures.append("the intro cutscene never ended before the hub load "
                        f"(cutscene @{cutscene_loads[0][3]}, hub @{hub_frame})")

    # 2. a real racer, for long enough
    rows = [(int(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4)),
             int(m.group(5)), int(m.group(6)), int(m.group(7))) for m in
            (PACE_RE.search(l) for l in out.splitlines()) if m]
    hub = [r for r in rows if r[0] >= hub_frame]
    if len(hub) < MIN_HUB_FRAMES:
        failures.append(f"only {len(hub)} in-hub racer frames traced (want >= {MIN_HUB_FRAMES})")
    if not hub:
        print_result(failures)
        return 1

    bad = [r for r in hub if not all(is_finite(v) for v in (r[1], r[2], r[3]))]
    if bad:
        failures.append(f"non-finite position at frame {bad[0][0]}: "
                        f"({bad[0][1]}, {bad[0][2]}, {bad[0][3]}) — {len(bad)} frames affected")
    y_bad = [r for r in hub if is_finite(r[2]) and not (Y_MIN <= r[2] <= Y_MAX)]
    if y_bad:
        worst = min(y_bad, key=lambda r: r[2]) if y_bad[0][2] < Y_MIN else max(y_bad, key=lambda r: r[2])
        failures.append(f"y out of the hub band [{Y_MIN}, {Y_MAX}] at frame {worst[0]}: y={worst[2]}")

    max_step, max_step_frame, path = 0.0, None, 0.0
    for a, b in zip(hub, hub[1:]):
        if b[0] != a[0] + 1:
            continue
        if not all(is_finite(v) for v in (a[1], a[2], a[3], b[1], b[2], b[3])):
            continue
        d = ((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2 + (b[3] - a[3]) ** 2) ** 0.5
        path += d
        if d > max_step:
            max_step, max_step_frame = d, b[0]
    if max_step > MAX_STEP:
        failures.append(f"teleport: {max_step:.1f} world units in one frame at "
                        f"frame {max_step_frame} (limit {MAX_STEP})")
    if hub[-1][4] <= hub[0][4]:
        failures.append(f"the race clock never advanced in the hub "
                        f"({hub[0][4]} -> {hub[-1][4]})")

    # 3. it actually drives
    if path < MIN_PATH_LENGTH:
        failures.append(f"path length only {path:.0f} world units (want >= {MIN_PATH_LENGTH:.0f})")
    span_x = max(r[1] for r in hub) - min(r[1] for r in hub)
    span_z = max(r[3] for r in hub) - min(r[3] for r in hub)
    if max(span_x, span_z) < MIN_BBOX_SPAN:
        failures.append(f"the kart stayed in one place: bounding box {span_x:.0f} x {span_z:.0f} "
                        f"(want a span >= {MIN_BBOX_SPAN:.0f})")
    # "It went somewhere ON PURPOSE" -- the closed-loop replacement for the hub
    # spline's cp=/lap= counters. Those count progress along the AI-node spline
    # (`courseCheckpoint`), which a waypoint tour does not follow at all: the
    # converted route scores cp 3 / lap 0 while covering 29872 units and a
    # 5938 x 3472 box, so asserting on them would now measure nothing.
    #
    # Reaching a named waypoint is a strictly stronger statement than advancing a
    # spline counter, because each one is a specific coordinate on the far side of
    # some terrain: a kart vibrating against a wall reaches none of them, and the
    # two failed spacings above reached 5 of 20 and 0 of 9 respectively.
    reached = WAYPOINT_RE.findall(out)
    if len(reached) < MIN_WAYPOINTS_REACHED:
        failures.append(f"the drive route only reached {len(reached)} of "
                        f"{HUB_TOUR_ROUTE.count(':')} waypoints "
                        f"(want >= {MIN_WAYPOINTS_REACHED}) — the kart is stuck, not "
                        f"touring. Last reached: {reached[-3:] if reached else 'none'}")

    # 4. the scene still draws
    # Sort NUMERICALLY, not lexically: frame_9800 must come after frame_11900,
    # otherwise the "last sampled frame" assertion below checks the wrong frame.
    def frame_no(name: str) -> int:
        m = re.search(r"(\d+)", name)
        return int(m.group(1)) if m else -1
    samples = sorted((f for f in os.listdir(frame_dir) if f.endswith(".ppm")), key=frame_no)
    if not samples:
        failures.append("no frames were dumped — cannot check that the scene draws")
    scored = []
    for name in samples:
        colours, sigma = scene_metrics(os.path.join(frame_dir, name))
        scored.append((name, colours, sigma))
    weak = [s for s in scored if s[1] < MIN_DISTINCT_COLOURS or s[2] < MIN_LUMA_SIGMA]
    if len(weak) > MAX_BAD_SAMPLES:
        failures.append(f"{len(weak)} of {len(scored)} sampled frames do not contain a real "
                        f"scene (allowed {MAX_BAD_SAMPLES}): " +
                        ", ".join(f"{n} {c} colours / sigma {s:.1f}" for n, c, s in weak))
    if scored and (scored[-1][1] < MIN_DISTINCT_COLOURS or scored[-1][2] < MIN_LUMA_SIGMA):
        failures.append(f"the LAST sampled frame does not contain a real scene: "
                        f"{scored[-1][0]} {scored[-1][1]} colours / sigma {scored[-1][2]:.1f}")

    if args.verbose:
        print(f"  menus:            {menu_ids}")
        print(f"  level loads:      {[(l[0], l[3]) for l in levels]}")
        print(f"  hub load frame:   {hub_frame}")
        print(f"  in-hub frames:    {len(hub)}")
        print(f"  clock:            {hub[0][4]} -> {hub[-1][4]}")
        print(f"  path length:      {path:.0f}  bbox {span_x:.0f} x {span_z:.0f}")
        print(f"  waypoints reached: {len(reached)} of "
              f"{HUB_TOUR_ROUTE.count(':')}")
        print(f"  max single step:  {max_step:.1f} at frame {max_step_frame}")
        print(f"  first position:   ({hub[0][1]:.1f}, {hub[0][2]:.1f}, {hub[0][3]:.1f})")
        print(f"  last position:    ({hub[-1][1]:.1f}, {hub[-1][2]:.1f}, {hub[-1][3]:.1f})")
        for n, c, s in scored:
            print(f"  frame {n}: {c} colours / sigma {s:.1f}")

    print_result(failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
