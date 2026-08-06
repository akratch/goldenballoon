#!/usr/bin/env python3
"""Drive EVERY playable track and assert each one loads, runs and makes progress.

Why this exists
---------------
Until this check, everything validated in this port was ONE track in ONE vehicle:
Ancient Lake in a car. There are 20 playable tracks and three player vehicles, and
this project's recurring failure mode is a per-track asset that decodes wrong and
fails *silently* -- the boost table (`ASSET_MISC_20`), the per-character steer
divisor (`ASSET_MISC_8`) and the per-triangle collision facets were all that
shape, and none of them crashed at the point of the defect.

Reaching each track through the menus would need 20 hand-tuned input scripts. The
`MDKR_LOAD_TRACK=<levelId>[:<vehicle>]` hook (game/src/game.c, no-op unless set)
instead retargets the race load of ONE proven route, so a sweep is a loop. Driving
is done by `MDKR_AUTOPILOT=1`, which hands the human racer to DKR's own AI, so no
per-track input tuning is needed either.

The very first run of this sweep found a hard SIGSEGV on level 9 that 19 other
tracks do not hit, inside `obj_loop_snowball`. The first reading of it -- "a NULL
`animatedObject`" -- was WRONG and is retracted: `anyBehaviorData` was a perfectly
valid pointer. The fault was `Object_AnimatedObject.soundMask` declared `u32` while
holding a host `AudioPoint *`, so the readback was truncated to its low 32 bits and
the store's top half landed on `currentSound`. Fixed at the type; see the comment on
that field in `game/include/structs.h`. Level 9 is now the sweep's best-progressing
track (maxcp=32), so this check no longer needs `--expect-fail` for anything.

What it asserts, per track
--------------------------
  1. the process exits 0 (no crash, no abort)
  2. the intended level actually loaded as a race
  3. the racer produced position samples, all finite
  4. the racer made real forward progress (courseCheckpoint climbed)
  5. no [FATAL] / sanitizer / assertion text appeared

Usage:
    tests/check_track_sweep.py                    # all 20 tracks, default vehicle
    tests/check_track_sweep.py --frames 5200
    tests/check_track_sweep.py --tracks 5,3,9     # a subset
    tests/check_track_sweep.py --vehicle 1        # force hovercraft
    tests/check_track_sweep.py --expect-fail N    # diagnostic only; release runs use none

Always runs muted + headless (MDKR_AUDIO=0 and --headless-frames), with one
authored Original-cadence ticket per host opportunity (MDKR_SYNTH_FIELDS=2),
per tests/README.md. The synthetic clock keeps this content sweep independent
of renderer/compositor and sanitizer speed. Exit 0 = every track passed (or
failed only as expected).
"""
import argparse
import os
import re
import subprocess
import sys

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

# ASSET_MISC_MAIN_TRACKS_IDS from the US v80 ROM, in menu order. Decoded with
# tools/dump_misc_asset.py (sub-asset 28) -- 20 entries before the -1 terminator.
MAIN_TRACK_IDS = [5, 3, 29, 7, 8, 4, 10, 30, 13, 6, 9, 28, 19, 18, 20, 31, 17, 32, 33, 15]

PACE_RE = re.compile(r"racer x=(\S+) y=(\S+) z=(\S+) clock=(\d+) cp=(\d+)")
ANIM_TARGET_RE = re.compile(r"\[ANIMTARGET\] init=(\d+) invalid=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"MemorySanitizer|runtime error:|Assertion|\[FX BUG\]"
)


def run_track(build, rom, script, level, frames, vehicle, timeout):
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"          # belt-and-braces; --headless-frames is the guarantee
    env["MDKR_PRESENT_RATE"] = "original"
    env["MDKR_SIMULATION_CADENCE"] = "original"
    env["MDKR_SYNTH_FIELDS"] = "2"   # one authored gameplay ticket per opportunity
    env["MDKR_AUTOPILOT"] = "1"      # drive with DKR's own AI
    env["MDKR_TRACE"] = "1"          # emit the [PACE] racer probe
    env["MDKR_LOAD_TRACK"] = str(level) if vehicle is None else "%d:%d" % (level, vehicle)
    cmd = [build, "--headless-frames", str(frames), "--input-script", script, "--rom", rom]
    try:
        proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
        out = proc.stdout.decode("utf-8", "replace")
        rc = proc.returncode
    except subprocess.TimeoutExpired as e:
        return {"rc": "timeout", "ok": False, "why": "timed out after %ss" % timeout,
                "out": (e.stdout or b"").decode("utf-8", "replace")}

    samples = PACE_RE.findall(out)
    loaded = ("level_load: levelId=%d numPlayers=0" % level) in out
    bad = BAD_RE.findall(out)
    anim_target = ANIM_TARGET_RE.search(out)

    why = []
    if rc != 0:
        why.append("exited %d%s" % (rc, " (signal %d)" % -rc if rc < 0 else ""))
    if not loaded:
        why.append("level %d never loaded as a race" % level)
    if not samples:
        why.append("no racer samples")
    if bad:
        why.append("%d bad-log line(s): %s" % (len(bad), bad[0]))
    if anim_target is None:
        why.append("no animation-target census summary")
    elif int(anim_target.group(2)) != 0:
        why.append("%s invalid animation target binding(s)" % anim_target.group(2))

    maxcp = 0
    nonfinite = 0
    if samples:
        maxcp = max(int(s[4]) for s in samples)
        for s in samples:
            joined = (s[0] + s[1] + s[2]).lower()
            if "inf" in joined or "nan" in joined:
                nonfinite += 1
        if maxcp < 3:
            why.append("no forward progress (max courseCheckpoint %d)" % maxcp)
        if nonfinite:
            why.append("%d non-finite position sample(s)" % nonfinite)

    return {"rc": rc, "ok": not why, "why": "; ".join(why), "samples": len(samples),
            "maxcp": maxcp, "nonfinite": nonfinite,
            "anim_init": int(anim_target.group(1)) if anim_target else 0,
            "out": out}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--script", default="tests/input_scripts/race_full_3lap_tt.txt")
    ap.add_argument("--frames", type=int, default=5200)
    ap.add_argument("--timeout", type=int, default=300)
    # NOTE: unset does NOT mean "the track's own default vehicle" (an earlier version
    # of this help text said so, and that wording is what hid the gap this comment
    # documents). MDKR_LOAD_TRACK rewrites the level id inside level_load(), long
    # after the menus chose the vehicle from leveltable_vehicle_default() for the
    # PREVIEWED track -- which on this route is always Ancient Lake, i.e. the CAR.
    # So an unset --vehicle sweeps all 20 tracks in a car, including the ones whose
    # available_vehicles mask forbids one. tests/check_vehicle_sweep.py is the check
    # that drives each track in the vehicles it actually permits.
    ap.add_argument("--vehicle", type=int, default=None,
                    help="force a vehicle id (0=car 1=hovercraft 2=plane); "
                         "unset = whatever the menu route already picked, which is the CAR")
    ap.add_argument("--tracks", default=None, help="comma-separated level ids (default: all 20)")
    ap.add_argument("--expect-fail", default="",
                    help="comma-separated level ids known to fail; reported but do not fail the suite")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    args.build = resolve_binary(args.build)

    for path in (args.build, args.rom, args.script):
        if not os.path.exists(path):
            sys.exit("missing: %s" % path)

    tracks = [int(t) for t in args.tracks.split(",")] if args.tracks else MAIN_TRACK_IDS
    expected_fail = {int(t) for t in args.expect_fail.split(",") if t.strip()}

    print("check_track_sweep: %d track(s), %d frames each, autopilot, vehicle=%s"
          % (len(tracks), args.frames, args.vehicle if args.vehicle is not None else "track default"))

    unexpected = []
    animation_inits = 0
    for level in tracks:
        r = run_track(args.build, args.rom, args.script, level, args.frames,
                      args.vehicle, args.timeout)
        tag = "ok" if r["ok"] else "FAIL"
        animation_inits += r.get("anim_init", 0)
        if not r["ok"] and level in expected_fail:
            tag = "FAIL (expected)"
        elif not r["ok"]:
            unexpected.append(level)
        detail = ""
        if r["ok"]:
            detail = "samples=%d maxcp=%d" % (r["samples"], r["maxcp"])
        else:
            detail = r["why"]
        print("  level %-3d %-15s %s" % (level, tag, detail))
        if not r["ok"]:
            excerpt_lines = 120 if args.verbose else 40
            for line in r["out"].splitlines()[-excerpt_lines:]:
                print("      | %s" % line)

    if unexpected:
        print("check_track_sweep: FAIL — %d track(s) broke unexpectedly: %s"
              % (len(unexpected), ", ".join(str(t) for t in unexpected)))
        return 1
    if animation_inits == 0:
        print("check_track_sweep: FAIL — the complete track corpus initialized no "
              "animation targets, so its binding assertion was vacuous")
        return 1
    print("check_track_sweep: PASS (animation targets initialized=%d, invalid=0)"
          % animation_inits)
    return 0


if __name__ == "__main__":
    sys.exit(main())
