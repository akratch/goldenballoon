#!/usr/bin/env python3
"""Issue #41: returning from an Adventure race must not launch the kart.

Why this exists
---------------
Returning to a world lobby from a race (quit from the pause menu, quit during
the starting camera pan, or finish and leave) reloads the lobby with
``cutscene 100``: the kart is placed inside the race door's alcove and driven
back out into the world while the door -- opened by the kart's own proximity --
is still rising.  ``collision_objectmodel()`` evaluates racer points against an
object with the PREVIOUS tick's inverse and the CURRENT tick's forward matrix;
that authored pairing is what lets moving meshes carry riders, but for a rising
gate it welds the door's vertical step onto every laterally blocked point.  The
facet walk blocks the kart with a purely horizontal normal, the frame pair adds
+2 of lift per tick, and the racer integrator amplifies that into a launch:
measured y 5 -> 231 in 28 ticks, pinned against the alcove ceiling for ~60
ticks, then an odd-angle landing -- exactly the report, on every exit path,
because all three converge on the same return load (levelId=12 entrance=3
cutscene=100).

The fix evaluates RISING sliding doors in one consistent current-pose frame
with world-history origins (they still block laterally, they no longer lift),
and re-primes a spawned object's collision matrices from its FINAL pose so the
first tick after a level load reads and writes one frame (the behaviour init
that sets a door's closedRotation/scale used to run after the priming; the
mismatched round trip displaced a stationary point by ~59 world units and
seeded the pair cache on the door's faces).

What this asserts
-----------------
  1. **QUIT MID-RACE (fixed arm)** -- the full chain runs: the race loads, the
     race clock really runs, the pause-menu quit produces the return load
     (levelId=12, entrance=3, cutscene=100), and across the whole return window
     the kart stays below ``MAX_RETURN_Y``, never teleports more than
     ``MAX_STEP`` in one frame, and really lands on the alcove floor.
  2. **QUIT DURING THE START PAN (fixed arm)** -- same bounds on the second
     reproduction path; the return load must arrive within ``PAN_QUIT_WINDOW``
     frames of the race load, proving the race never started.
  3. **LEGACY ARM (positive control)** -- ``MDKR_DOORCARRY=legacy`` restores
     the carry-frame pairing for rising doors in the SAME binary and the same
     mid-race route.  The launch must reappear (peak y >= ``MIN_LEGACY_PEAK``).
     Without this arm a build that quietly stopped tracing ``[PACE]`` rows in
     the window, or a route that stopped reaching the door, would pass
     assertion 1 vacuously.
  4. All arms exit 0 with no ``[CRASH]``/``[FATAL]``.

Usage:
    tests/check_postrace_door_fling.py [--build build] [--rom baserom.us.v80.z64] [-v]

Always runs muted + headless (``MDKR_AUDIO=0`` and ``--headless-frames``), per
tests/README.md.  Exit 0 = pass; exit 1 = at least one assertion failed.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(ROOT, "tests", "input_scripts")

# The same closed-loop route as check_adventure_race_loop.py, and for the same
# reason: only MDKR_DRIVE_ROUTE steering ever reaches a hub door on purpose.
DRIVE_ROUTE = (
    "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
    ":-3381,1946:-2519,1516:-1858,1099"
    ";12:E5:E0"
)

LOBBY = 12           # ASSET_LEVEL_DINODOMAINHUB
RACE = 5             # ASSET_LEVEL_ANCIENTLAKE
RETURN_ENTRANCE = 3  # the Ancient Lake door (the exit's returnSpawnIndex)
RETURN_CUTSCENE = 100  # the post-race pan that drives the kart back out

MAX_RETURN_Y = 80.0    # alcove floor -48, spawn 5; the broken launch pins at 231
MIN_LANDED_Y = -30.0   # the descent to the floor must really happen
MAX_STEP = 40.0        # same single-frame teleport bound as the race checks
MIN_LEGACY_PEAK = 150.0  # the control launch measured 231; demand most of it
LOAD_SKIP = 3          # rows to drop right after a level load (see race loop)
PAN_QUIT_WINDOW = 400  # return load this close to the race load = quit in pan
MIN_RACE_CLOCK = 200   # mid-race arm: the clock really ran before the quit

ARMS = (
    # name, script, frames, legacy, pan
    ("quit-midrace", "adventure_quit_midrace.txt", 9200, False, False),
    ("quit-startpan", "adventure_quit_startpan.txt", 8400, False, True),
    ("legacy-control", "adventure_quit_midrace.txt", 9200, True, False),
)

PACE_RE = re.compile(
    r"\[PACE\] frame=(\d+).*racer x=(\S+) y=(\S+) z=(\S+) clock=(\d+)")
LEVEL_RE = re.compile(
    r"level_load: levelId=(\d+) numPlayers=(-?\d+) entrance=(-?\d+)"
    r" vehicle=(-?\d+) cutscene=(-?\d+) @frame~(\d+)")


def run_arm(binary: str, rom: str, script: str, frames: int, legacy: bool):
    with tempfile.TemporaryDirectory(prefix="mdkr_doorfling_") as run_dir:
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("MDKR_")}
        env.update(
            MDKR_AUDIO="0",
            MDKR_SIMULATION_CADENCE="enhanced",
            MDKR_SYNTH_FIELDS="1",
            MDKR_TRACE="1",
            MDKR_AUTOPILOT="1",
            MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
        )
        if legacy:
            env["MDKR_DOORCARRY"] = "legacy"
        env = save_env(env, run_dir)
        proc = subprocess.run(
            [binary, "--headless-frames", str(frames),
             "--input-script", os.path.join(SCRIPTS, script),
             "--rom", rom],
            capture_output=True, text=True, env=env, cwd=run_dir)
    return proc, proc.stdout + proc.stderr


def analyse(name: str, proc, out: str, frames: int,
            legacy: bool, pan: bool, verbose: bool) -> list[str]:
    failures: list[str] = []
    prefix = f"{name}: "

    if proc.returncode != 0:
        failures.append(f"{prefix}exit code {proc.returncode} "
                        f"(negative = killed by signal {-proc.returncode})")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            line = next((l for l in out.splitlines() if marker in l), marker)
            failures.append(f"{prefix}{marker} in output: {line.strip()}")

    levels = [(int(m.group(1)), int(m.group(3)), int(m.group(5)),
               int(m.group(6)))
              for m in (LEVEL_RE.search(l) for l in out.splitlines()) if m]
    race = next((lv for lv in levels if lv[0] == RACE), None)
    if race is None:
        failures.append(f"{prefix}the race (levelId={RACE}) never loaded — "
                        f"level loads: {[(lv[0], lv[3]) for lv in levels]}")
        return failures
    ret = next((lv for lv in levels
                if lv[0] == LOBBY and lv[3] > race[3]), None)
    if ret is None:
        failures.append(f"{prefix}never returned to the lobby after the quit "
                        f"— level loads: {[(lv[0], lv[3]) for lv in levels]}")
        return failures
    if ret[1] != RETURN_ENTRANCE or ret[2] != RETURN_CUTSCENE:
        failures.append(
            f"{prefix}the return load has entrance={ret[1]} "
            f"cutscene={ret[2]}, expected entrance={RETURN_ENTRANCE} "
            f"cutscene={RETURN_CUTSCENE} — not a real post-race return")
    if pan and ret[3] - race[3] > PAN_QUIT_WINDOW:
        failures.append(
            f"{prefix}the quit landed {ret[3] - race[3]} frames after the "
            f"race load (max {PAN_QUIT_WINDOW} for a start-pan quit) — the "
            f"race had already started")

    rows = [(int(m.group(1)), float(m.group(2)), float(m.group(3)),
             float(m.group(4)), int(m.group(5)))
            for m in (PACE_RE.search(l) for l in out.splitlines()) if m]
    if not pan and not legacy:
        race_clock = max((r[4] for r in rows if race[3] < r[0] < ret[3]),
                        default=0)
        if race_clock < MIN_RACE_CLOCK:
            failures.append(
                f"{prefix}race clock only reached {race_clock} before the "
                f"quit (want >= {MIN_RACE_CLOCK}) — the quit did not happen "
                f"mid-race")

    end = next((lv[3] for lv in levels if lv[3] > ret[3]), frames)
    window = [r for r in rows if ret[3] + LOAD_SKIP <= r[0] < end]
    if not window:
        failures.append(f"{prefix}no racer positions were traced during the "
                        f"lobby return")
        return failures

    peak = max(window, key=lambda r: r[2])
    if verbose:
        print(f"  {name}: race @{race[3]}, return @{ret[3]}, "
              f"{len(window)} rows, peak y={peak[2]:.1f} @frame {peak[0]}, "
              f"floor y={min(r[2] for r in window):.1f}")

    if legacy:
        if peak[2] < MIN_LEGACY_PEAK:
            failures.append(
                f"{prefix}the carry-frame control only reached y="
                f"{peak[2]:.1f} (want >= {MIN_LEGACY_PEAK:.0f}) — the launch "
                f"did not reproduce, so the fixed arms' altitude bound is "
                f"not being tested")
        return failures

    if peak[2] > MAX_RETURN_Y:
        failures.append(
            f"{prefix}the lobby return launched the kart to y={peak[2]:.1f} "
            f"at frame {peak[0]} (maximum {MAX_RETURN_Y:.1f})")
    if min(r[2] for r in window) > MIN_LANDED_Y:
        failures.append(
            f"{prefix}the kart never descended to the alcove floor "
            f"(min y {min(r[2] for r in window):.1f}, want <= "
            f"{MIN_LANDED_Y:.1f}) — the return cinematic did not complete")
    worst, at = 0.0, None
    for a, b in zip(window, window[1:]):
        if b[0] != a[0] + 1:
            continue
        step = ((b[1] - a[1]) ** 2 + (b[2] - a[2]) ** 2 +
                (b[3] - a[3]) ** 2) ** 0.5
        if step > worst:
            worst, at = step, b[0]
    if worst > MAX_STEP:
        failures.append(
            f"{prefix}teleport during the lobby return: {worst:.1f} world "
            f"units in one frame at frame {at} (limit {MAX_STEP})")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = os.path.abspath(resolve_binary(args.build))
    rom = os.path.abspath(args.rom)
    for path in (binary, rom):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    for name, script, frames, legacy, pan in ARMS:
        if args.verbose:
            print(f"  running {name} arm ({script}, {frames} frames"
                  f"{', MDKR_DOORCARRY=legacy' if legacy else ''})")
        proc, out = run_arm(binary, rom, script, frames, legacy)
        failures.extend(analyse(name, proc, out, frames, legacy, pan,
                                args.verbose))

    if failures:
        print("FAIL: post-race door fling check")
        for f in failures:
            print("  - " + f)
        return 1
    print("PASS: post-race door fling check")
    return 0


if __name__ == "__main__":
    sys.exit(main())
