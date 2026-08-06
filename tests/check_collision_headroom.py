#!/usr/bin/env python3
"""Per-level collision-candidate headroom, and a tripwire if it shrinks.

Background
----------
`generate_collision_candidates()` (game/src/hasm/collision.c) fills
`gCollisionCandidates[MAX_COLLISION_CANDIDATES]` (500 entries,
game/src/collision.h). Wave "boundsweep" (docs/open-items/collision.md) found
that the SEGMENT insert at the top of the outer loop advanced the write index
with no bound at all -- only the FACET insert's post-increment equality test
(`j == 500`) guarded anything, and an equality test cannot catch an index that
steps over it. It is fixed with a `j >= mdkr_coll_cap(MAX_COLLISION_CANDIDATES)`
pre-check at BOTH insert sites (this check's first assertion is that both are
still there).

That fix stops the overrun, but it does not add headroom: a saturated list
still silently DROPS every candidate past the cap, which is the exact mechanism
that dropped racers through Tricky's volcano in wave "gridmask" (an
over-permissive grid mask fed the list too many triangles). Measured peaks
today: 416 of 500 on boss levels 41 and 54 -- 84 slots of margin, the tightest
in the game. This follow-up is INSTRUMENT-ONLY: measure
and gate, not raise the cap -- the layout/perf effects of a larger allocation are
unmeasured, so a speculative raise is outside its scope.

What this check does
---------------------
1. Confirms the `j >= cap` guard source text is present at both insert sites
   (a static, cheap check that the fix this gate depends on has not regressed
   to the ROM's bare equality test).
2. Drives all ten boss levels (the tightest margins in the game) plus one
   ordinary race, one run each, MDKR_AUTOPILOT + MDKR_TRACE, and reads the
   `[COLL] maxCandidates=N truncated=N cap=N` line every route already emits
   (platform/platform_sdl_min.c, fed by platform/stubs_dkr.c
   mdkr_coll_candidates()). Fails if truncation happens in normal play (that is
   never survivable -- see wave "gridmask"), and fails if any level's high-water
   mark exceeds a frozen baseline ceiling (measured peak + a small slack), which
   is the regression tripwire: something that quietly widens the candidate list
   would move these numbers before it ever drops a candidate.
3. Positive control: MDKR_COLLCAP=<n> (platform/stubs_dkr.c, also used by
   check_array_bounds_sweep.py's own cap control) lowers the EFFECTIVE cap on
   boss level 41's own route -- same track, same script, same frame budget as
   row 41 in the table above, cap alone shrunk from 500 to 150, well under its
   natural 416 peak. This must (a) make the guard hold -- maxCandidates never
   exceeds the lowered cap, i.e. the fix from wave "boundsweep" is still doing
   its job -- and (b) make the run truncate, which is exactly the condition the
   step 2 "truncated must be 0" rule exists to catch. Evaluating that same rule
   against the forced run confirms it actually fires: the control is not
   vacuous.

This does NOT raise MAX_COLLISION_CANDIDATES, touch generate_collision_candidates()
or compute_grid_overlap_mask(), or change gameplay in any way -- read-only
instrumentation plus a headless regression gate.

    MDKR_AUDIO=0 python3 tests/check_collision_headroom.py -v      # ~3 min

Always muted + headless, per tests/README.md. Exit 0 = pass.
"""
import argparse
import os
import re
import subprocess
import sys

from harness_utils import (DEFAULT_BUILD_DIR, preserved_eeprom, resolve_binary,
                           save_env, test_save_dir)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ASSET_MISC_BOSS_TRACKS_IDS, decoded with tools/dump_misc_asset.py (sub-asset
# 30) and used in this exact order throughout the suite (see
# check_collision_gridmask.py, check_webgpu_content_census.py).
BOSS_TRACKS = (38, 46, 40, 53, 1, 52, 41, 54, 37, 55)
# Boss tracks need the larger budget to clear the pre-race cutscene and reach
# Tricky's summit / a finish -- the same figure wave "boundsweep"'s own 33-route
# sweep used and check_collision_gridmask.py uses for boss levels.
BOSS_FRAMES = 13000

# One ordinary race, so the table is not only boss levels. Ancient Lake is the
# established negative control elsewhere in this suite
# (check_collision_gridmask.py's CONTROL_TRACK) -- never saturates, included
# here for contrast with the boss levels' margins.
NORMAL_TRACK = 5
NORMAL_FRAMES = 6500

SCRIPT = os.path.join("tests", "input_scripts", "race_full_3lap_tt.txt")
SAVE_DIR = test_save_dir()
SAVE = os.path.join(SAVE_DIR, "eeprom.bin")

CAP = 500  # MAX_COLLISION_CANDIDATES, game/src/collision.h

# The track this check forces saturation on for the positive control. 41 (tied
# with 54) has the tightest natural margin measured -- 84 of 500 -- so it is
# also the level a real regression would hit first.
CONTROL_TRACK = 41
CONTROL_FORCED_CAP = 150  # well under the natural 416 peak on this track

COLL_RE = re.compile(r"\[COLL\] maxCandidates=(\d+) truncated=(\d+) cap=(\d+)")

# --- the guard this check depends on ----------------------------------------
# Both insert sites in generate_collision_candidates() must pre-check the
# EFFECTIVE cap before writing. Wave "boundsweep" added the segment-insert
# guard; the facet-insert guard existed already (as an equality test) and was
# tightened to the same >= form alongside it. If either regresses to a bare
# `==` or disappears, the guard this whole check relies on is gone and every
# assertion below would be measuring a moving target.
COLLISION_C = os.path.join("game", "src", "hasm", "collision.c")
GUARD_PATTERN = re.compile(
    r"if\s*\(\s*j\s*>=\s*mdkr_coll_cap\s*\(\s*MAX_COLLISION_CANDIDATES\s*\)\s*\)\s*\{\s*goto\s+out\s*;")

# --- frozen baseline ----------------------------------------------------------
# Measured on this binary: one MDKR_AUTOPILOT run per level (race_full_3lap_tt.txt,
# boss levels at 13000 frames, the normal track at 6500), same methodology as
# wave "boundsweep"'s original 33-route sweep
# (docs/open-items/collision.md). SLACK is added on top so this is a regression
# TRIPWIRE (a level's candidate count creeping up) rather than a byte-exact
# assertion that would fail on an immaterial route nudge -- MDKR_AUTOPILOT is
# deterministic, so the slack is headroom for a legitimate small change (e.g. a
# cutscene timing shift moving the AI line by a frame), not run-to-run noise.
# If a real geometry or route change legitimately moves one of these, update
# the peak deliberately (and re-run docs/open-items/collision.md's
# accounting) -- never widen it to silence an unexplained increase.
SLACK = 16

# key -> (frames, measured peak candidates)
BASELINE = {
    38: (BOSS_FRAMES, 270),
    46: (BOSS_FRAMES, 270),
    40: (BOSS_FRAMES, 55),
    53: (BOSS_FRAMES, 55),
    1: (BOSS_FRAMES, 152),
    52: (BOSS_FRAMES, 152),
    41: (BOSS_FRAMES, 416),
    54: (BOSS_FRAMES, 416),
    37: (BOSS_FRAMES, 149),
    55: (BOSS_FRAMES, 92),
    NORMAL_TRACK: (NORMAL_FRAMES, 30),
}

ROUTES = list(BOSS_TRACKS)
if NORMAL_TRACK not in ROUTES:
    ROUTES.append(NORMAL_TRACK)


def check_guard_present() -> list:
    """Static check: the `j >= cap` pre-check guard text at both insert sites."""
    path = os.path.join(ROOT, COLLISION_C)
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        return ["could not read %s: %s" % (COLLISION_C, e)]
    matches = GUARD_PATTERN.findall(text)
    if len(matches) < 2:
        return ["expected 2 occurrences of the `j >= mdkr_coll_cap(MAX_COLLISION_"
                "CANDIDATES)) { goto out; }` guard in %s (segment insert + facet "
                "insert), found %d -- the wave \"boundsweep\" fix has regressed"
                % (COLLISION_C, len(matches))]
    return []


def run(binary, rom, track, frames, forced_cap=None, verbose=False):
    if os.path.exists(SAVE):
        os.remove(SAVE)
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"
    env["MDKR_AUTOPILOT"] = "1"
    save_env(env, SAVE_DIR)
    env["MDKR_TRACE"] = "1"
    env["MDKR_LOAD_TRACK"] = str(track)
    if forced_cap is not None:
        env["MDKR_COLLCAP"] = str(forced_cap)
    else:
        env.pop("MDKR_COLLCAP", None)
    cmd = [binary, "--headless-frames", str(frames), "--input-script", SCRIPT, "--rom", rom]
    if verbose:
        print("  $ MDKR_LOAD_TRACK=%d MDKR_COLLCAP=%s %s" %
              (track, forced_cap if forced_cap is not None else "(unset)", " ".join(cmd)))
    proc = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=900)
    out = proc.stdout.decode("utf-8", "replace")
    m = COLL_RE.search(out)
    coll = {
        "maxCandidates": int(m.group(1)),
        "truncated": int(m.group(2)),
        "cap": int(m.group(3)),
    } if m else None
    return proc.returncode, coll, out


def evaluate_normal_play(track, rc, coll) -> list:
    """The rules a level must satisfy in ordinary (unforced) play."""
    problems = []
    label = "track %d" % track
    if rc != 0:
        problems.append("%s: exit code %d" % (label, rc))
    if coll is None:
        problems.append("%s: no '[COLL] maxCandidates=' line in output" % label)
        return problems
    if coll["cap"] != CAP:
        problems.append("%s: effective cap reported as %d, expected %d (an "
                        "MDKR_COLLCAP leaked into the environment?)"
                        % (label, coll["cap"], CAP))
    if coll["truncated"] != 0:
        problems.append("%s: truncated=%d in NORMAL play -- the candidate list "
                        "saturated and silently dropped terrain (wave "
                        "\"gridmask\" mechanism). This must be 0."
                        % (label, coll["truncated"]))
    ceiling = BASELINE[track][1] + SLACK
    if coll["maxCandidates"] > ceiling:
        problems.append("%s: maxCandidates=%d exceeds the recorded ceiling %d "
                        "(baseline peak %d + slack %d) -- headroom has shrunk. "
                        "Re-measure and, if the increase is a legitimate "
                        "geometry/route change, update BASELINE deliberately "
                        "and note it in docs/open-items/collision.md."
                        % (label, coll["maxCandidates"], ceiling,
                           BASELINE[track][1], SLACK))
    return problems


def main() -> int:
    # This check deletes and rewrites EEPROM images to assert a known starting
    # state. Under tools/run_checks.py SAVE_DIR is a run-scoped temporary
    # directory; a standalone run defaults to the repository's playable save/,
    # whose prior contents must survive however this exits.
    with preserved_eeprom(SAVE_DIR):
        return run_check()


def run_check() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, os.path.join(ROOT, SCRIPT)):
        if not os.path.exists(path):
            print("FAIL: missing %s" % path, file=sys.stderr)
            return 1

    failures: list = []

    # --- 1. the guard the rest of this check depends on ---------------------
    guard_problems = check_guard_present()
    failures += guard_problems
    if not guard_problems:
        print("guard: j >= mdkr_coll_cap(MAX_COLLISION_CANDIDATES) present at "
              "both insert sites in %s" % COLLISION_C)

    # --- 2. per-level sweep: ten boss levels + one ordinary race ------------
    print("\nper-level high-water sweep (%d routes):" % len(ROUTES))
    print("  %-6s %6s %6s %6s %8s %8s %8s" %
          ("track", "frames", "peak", "trunc", "cap", "ceiling", "margin"))
    results = {}
    for track in ROUTES:
        frames, _ = BASELINE[track]
        rc, coll, out = run(binary, args.rom, track, frames, verbose=args.verbose)
        results[track] = (rc, coll)
        problems = evaluate_normal_play(track, rc, coll)
        failures += problems
        ceiling = BASELINE[track][1] + SLACK
        if coll is not None:
            margin = CAP - coll["maxCandidates"]
            print("  %-6d %6d %6d %6d %8d %8d %8d%s" %
                  (track, frames, coll["maxCandidates"], coll["truncated"], CAP,
                   ceiling, margin, "  FAIL" if problems else ""))
        else:
            print("  %-6d %6d    --     --       --       --       --  FAIL" % (track, frames))

    # --- 3. positive control: force saturation on track 41's own route ------
    print("\npositive control: MDKR_COLLCAP=%d on track %d (natural peak %d, "
          "same route as the row above)" % (CONTROL_FORCED_CAP, CONTROL_TRACK,
                                            BASELINE[CONTROL_TRACK][1]))
    frames, _ = BASELINE[CONTROL_TRACK]
    rc_f, coll_f, out_f = run(binary, args.rom, CONTROL_TRACK, frames,
                              forced_cap=CONTROL_FORCED_CAP, verbose=args.verbose)
    if rc_f != 0:
        failures.append("control: forced-cap run exited %d (expected 0 -- the "
                        "guard must hold cleanly, not crash)" % rc_f)
    elif coll_f is None:
        failures.append("control: forced-cap run produced no '[COLL]' line")
    else:
        print("  forced run: maxCandidates=%d truncated=%d cap=%d" %
              (coll_f["maxCandidates"], coll_f["truncated"], coll_f["cap"]))
        if coll_f["cap"] != CONTROL_FORCED_CAP:
            failures.append("control: effective cap reported as %d, expected the "
                            "forced %d -- MDKR_COLLCAP was not applied"
                            % (coll_f["cap"], CONTROL_FORCED_CAP))
        if coll_f["maxCandidates"] > CONTROL_FORCED_CAP:
            failures.append("control: maxCandidates=%d exceeded the forced cap "
                            "%d -- the wave \"boundsweep\" guard did NOT hold "
                            "under saturation" % (coll_f["maxCandidates"],
                                                  CONTROL_FORCED_CAP))
        else:
            print("  guard held: maxCandidates never exceeded the forced cap")
        if coll_f["truncated"] == 0:
            failures.append("control: truncated=0 with MDKR_COLLCAP=%d on a "
                            "track whose natural peak is %d -- the cap was not "
                            "actually forced below the route's real candidate "
                            "count, so the control proves nothing"
                            % (CONTROL_FORCED_CAP, BASELINE[CONTROL_TRACK][1]))
        else:
            # This is the actual "exercises the guard and FAILS the check"
            # requirement: run the SAME rule step 2 applies (truncated must be
            # 0 in play) against this forced arm and confirm it fires.
            would_fail = evaluate_normal_play(CONTROL_TRACK, rc_f, coll_f)
            saturation_hits = [p for p in would_fail if "saturated and silently"
                               in p]
            if not saturation_hits:
                failures.append("control: forced saturation (truncated=%d) did "
                                "NOT trip the normal-play truncated==0 rule -- "
                                "the check's own assertion is not exercising the "
                                "guard" % coll_f["truncated"])
            else:
                print("  confirmed: the same rule step 2 applies would FAIL this "
                      "forced arm (truncated=%d) -- the control is not vacuous"
                      % coll_f["truncated"])

    print()
    if failures:
        print("check_collision_headroom: FAIL")
        for f in failures:
            print("  - %s" % f)
        return 1
    tightest_track, tightest_peak = max(
        ((t, results[t][1]["maxCandidates"]) for t in ROUTES if results[t][1]),
        key=lambda kv: kv[1])
    print("check_collision_headroom: PASS  (tightest margin: track %d, peak "
          "%d/%d, %d slots headroom)" %
          (tightest_track, tightest_peak, CAP, CAP - tightest_peak))
    return 0


if __name__ == "__main__":
    sys.exit(main())
