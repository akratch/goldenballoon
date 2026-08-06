#!/usr/bin/env python3
"""Wave "objcoll" check: a locked hub door must physically block the kart.

Why this exists
---------------
`func_80017A18()` is the per-facet object-model collision test — the only thing
that makes a collision-meshed object solid, and the ONLY non-NULL writer of
`collisionData->collidedObj` anywhere in the game.  Until wave "objcoll" it was a
`return 0` WEAK stub in `platform/hasm_stubs_temp.c`, because `objects.c` guarded
the body with `#ifdef NON_EQUIVALENT`.  So every collision-meshed object was
intangible, and the player could drive through a shut door into a world they had
not earned: `obj_loop_exit()` has no door check by design (a 253-unit sphere plus
a half-plane through the exit's own origin), so once past the leaf it warps you.

Measured with the legacy arm, on a clean EEPROM with **zero balloons**: the kart
reaches the Dino Domain lobby at frame ~6731 and *Ancient Lake* at ~7017, with
`balloons_total=0` and both gating doors reporting `open=0`.

This check is the reason the fix is falsifiable.  Its failure mode is **silence** —
you simply drive through, nothing crashes, no log line appears — so a check that
only ran the fixed build would prove nothing (CONTRIBUTING.md rule 2).  Both arms
therefore run from ONE binary via `MDKR_OBJCOLL=legacy`, which restores the stub's
`return 0`, exactly as `MDKR_COLLTEX=legacy` does for the untextured-batch case.

What this asserts
-----------------
  1. **FIXED ARM** — with collision live, the 0-balloon route must NOT reach the
     Dino Domain lobby (levelId 12) or Ancient Lake (levelId 5).  It must still
     reach Timber's Island (levelId 0), otherwise the route broke for some
     unrelated reason and the "did not pass the door" result is vacuous.
  2. **FIXED ARM** — object-model collision must report a non-zero number of hits
     (`[OBJCOLL] objectmodel_collision_hits`).  Without this the check would also
     pass if the kart simply never drove anywhere near the door.
  3. **LEGACY ARM (positive control)** — with `MDKR_OBJCOLL=legacy` the SAME route
     must reach levelId 12 and levelId 5, and must report **zero** collision hits.
     This is what proves assertion 1 is caused by the fix and not by the route.
  4. Both arms exit 0 with no `[CRASH]`/`[FATAL]`.

Note the asymmetry is deliberate: the fixed arm asserts a *negative* (cannot get
in), which is only meaningful next to the legacy arm's *positive* (could get in,
same route, same seed, same binary).

Usage:
    tests/check_door_blocks.py [--build build] [--rom baserom.us.v80.z64] [-v]

Always runs muted + headless (`MDKR_AUDIO=0` and `--headless-frames`), per
CONTRIBUTING.md's audio-safety rule.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

SCRIPT = "tests/input_scripts/adventure_hub_drive.txt"
FRAMES = 9000

# Waypoints around Taj (who wedges the kart on contact), then a center-line
# approach to the hub's two Dino Domain door leaves before targeting the exit.
# The older advloop reproducer omitted (-3336,2111); its diagonal approach could
# latch obj_loop_exit() from the near-side corner without touching either leaf,
# making a zero-hit "blocked" result vacuous.  The centered approach hits the
# closed mesh in the fixed arm and crosses the same mesh in the legacy arm.
ROUTE = "0:200,500:-1004,946:-3336,2111:E12;12:E5"

HUB_LEVEL = 0        # Timber's Island — must always be reached
LOBBY_LEVEL = 12     # Dino Domain lobby — behind a 1-balloon door
RACE_LEVEL = 5       # Ancient Lake — behind another 1-balloon door

LEVEL_RE = re.compile(r"level_load: levelId=(\d+) .*@frame~(\d+)")
HITS_RE = re.compile(r"\[OBJCOLL\] objectmodel_collision_hits=(\d+)")


def run(binary: str, rom: str, save_dir: str, legacy: bool, verbose: bool):
    """One arm.  Returns (levels_reached, collision_hits, returncode, output)."""
    env = dict(
        os.environ,
        MDKR_AUDIO="0",          # belt-and-braces; --headless-frames is the guarantee
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_DRIVE_ROUTE=ROUTE,
        MDKR_SAVE_DIR=save_dir,
    )
    if legacy:
        env["MDKR_OBJCOLL"] = "legacy"
    else:
        env.pop("MDKR_OBJCOLL", None)

    cmd = [binary, "--headless-frames", str(FRAMES),
           "--input-script", SCRIPT, "--rom", rom]
    if verbose:
        print(f"$ {'MDKR_OBJCOLL=legacy ' if legacy else ''}" + " ".join(cmd))

    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    out = proc.stdout + proc.stderr

    levels = {}
    for m in LEVEL_RE.finditer(out):
        levels.setdefault(int(m.group(1)), int(m.group(2)))
    hits = None
    m = HITS_RE.search(out)
    if m:
        hits = int(m.group(1))
    return levels, hits, proc.returncode, out


def check_crash(tag: str, rc: int, out: str, failures: list):
    if rc != 0:
        failures.append(f"[{tag}] exit code {rc}")
    for marker in ("[CRASH]", "[FATAL]"):
        if marker in out:
            line = next((l for l in out.splitlines() if marker in l), marker)
            failures.append(f"[{tag}] {marker}: {line.strip()}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, SCRIPT):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []

    # ---- arm 1: the fix ---------------------------------------------------
    # Private save directories guarantee zero balloons without reading or
    # deleting the player's repository-local EEPROM.
    with tempfile.TemporaryDirectory(prefix="mdkr-door-fixed-") as save_dir:
        levels, hits, rc, out = run(
            binary, args.rom, save_dir, legacy=False, verbose=args.verbose)
    check_crash("fixed", rc, out, failures)

    if HUB_LEVEL not in levels:
        failures.append(
            f"[fixed] never reached Timber's Island (levelId {HUB_LEVEL}); the route "
            f"broke before the door, so the 'door blocked us' result is vacuous. "
            f"levels seen: {sorted(levels)}")
    if LOBBY_LEVEL in levels:
        failures.append(
            f"[fixed] drove through the shut 1-balloon door into the Dino Domain "
            f"lobby (levelId {LOBBY_LEVEL}) at frame {levels[LOBBY_LEVEL]} with 0 balloons")
    if RACE_LEVEL in levels:
        failures.append(
            f"[fixed] reached Ancient Lake (levelId {RACE_LEVEL}) at frame "
            f"{levels[RACE_LEVEL]} with 0 balloons")
    if hits is None:
        failures.append("[fixed] no [OBJCOLL] line -- MDKR_TRACE off, or the counter "
                        "was removed; cannot tell collision ran at all")
    elif hits == 0:
        failures.append("[fixed] object-model collision reported 0 hits, so the kart "
                        "never touched a collision-meshed object -- assertion 1 is "
                        "vacuous")

    # ---- arm 2: positive control ------------------------------------------
    with tempfile.TemporaryDirectory(prefix="mdkr-door-legacy-") as save_dir:
        l2, h2, rc2, out2 = run(
            binary, args.rom, save_dir, legacy=True, verbose=args.verbose)
    check_crash("legacy", rc2, out2, failures)

    if LOBBY_LEVEL not in l2 or RACE_LEVEL not in l2:
        failures.append(
            f"[legacy] POSITIVE CONTROL BROKEN: with collision stubbed the route was "
            f"expected to reach levelIds {LOBBY_LEVEL} and {RACE_LEVEL} (measured "
            f"~6731 and ~7017) but reached {sorted(l2)}. Either "
            f"MDKR_OBJCOLL=legacy no longer restores the old behaviour, or the route "
            f"drifted -- until this passes, the fixed arm proves nothing.")
    if h2:
        failures.append(f"[legacy] expected 0 collision hits with the stub arm, got {h2}")

    if args.verbose:
        print(f"  fixed : levels={sorted(levels)} hits={hits}")
        print(f"  legacy: levels={sorted(l2)} hits={h2}")

    if failures:
        print("FAIL: door blocking check")
        for f in failures:
            print("  - " + f)
        return 1

    print("PASS: door blocking check")
    print(f"  fixed  arm: reached {sorted(levels)}, {hits} object-collision hits "
          f"-- locked doors held")
    print(f"  legacy arm: reached {sorted(l2)}, {h2} hits "
          f"-- drove through the shut door, as before the fix")
    return 0


if __name__ == "__main__":
    sys.exit(main())
